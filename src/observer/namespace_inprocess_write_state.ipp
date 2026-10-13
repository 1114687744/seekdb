// Included inside oceanbase::observer::namespace_worker_prototype.
struct EngineWrite {
  ObArenaAllocator allocator{ObMemAttr("NsNativeWrite")};
  ns::TabletAccess access;
  // Same lifetime contract as ObIDmlService: the SQL owner retains the plan
  // until its execution is released, including across savepoint re-acquisition.
  const ObDmlTablePlan *plan = nullptr;
  data_plane::ObNamespaceAccessMode access_mode_ = data_plane::ObNamespaceAccessMode::UNBOUND;
  ObTimeZoneInfo timezone;
  ObDmlWriteSpec spec;
  ObTxReadSnapshot snapshot;
  concurrent_control::ObWriteFlag write_flag;
  ObWriteContext context;
  ObDmlExecution execution; // Released before context, access and allocator.
  StorageSpaceHandle storage_space;

  int prepare(const WritePrepareRequest &request,
              data_plane::ObNamespaceAccessMode access_mode, ObTxDesc &tx) {
    const auto table = request.plan.get_data_table();
    if (request.spec.tz_info_ == nullptr || !table.is_valid()
        || request.plan.get_col_descs().empty()
        || table.get_table_id() != request.spec.table_id_
        || table.get_schema_version() != request.spec.schema_version_) {
      return OB_INVALID_ARGUMENT;
    }
    storage_space = request.storage_space;
    spec = request.spec;
    plan = &request.plan;
    access_mode_ = storage_space.is_global()
        ? data_plane::ObNamespaceAccessMode::UNFENCED : access_mode;
    spec.timeout_ = std::min<int64_t>(spec.timeout_, THIS_WORKER.get_timeout_ts());
    int ret = timezone.assign(*spec.tz_info_);
    spec.tz_info_ = &timezone;
    if (OB_SUCC(ret)) { ret = snapshot.assign(request.snapshot); }
    write_flag = request.write_flag;
    // The query plan already pins the Namespace's definition. Storage's
    // process-wide schema lookup must not select another schema authority.
    spec.check_schema_version_ = false;
    if (OB_SUCC(ret)) { ret = acquire(tx); }
    return ret;
  }

  int acquire(ObTxDesc &tx) {
    int ret = OB_SUCCESS;
    if (context.is_valid()) { return ret; }
    if (OB_SUCC(ret)) { ret = share::server_service<ObIWriteContextService>()->acquire_write_context(
        spec.timeout_, tx, snapshot, spec.branch_id_, write_flag, context); }
    if (OB_SUCC(ret)) { ret = share::server_service<ObIDmlService>()->prepare_execution(
        spec, *plan, snapshot, allocator, context, write_flag, execution); }
    return ret;
  }

  // Context release publishes native write state to the owner. Savepoint
  // rollback calls this before deciding whether storage was touched.
  void release_context() {
    execution.reset();
    context.reset();
    access.reset();
  }

  int invoke(const ObTabletID &tablet, ObTxDesc &tx, bool skip_flush_redo,
             const NativeWriteOperation &operation) {
    int ret = access.prepare_write(storage_space.tablet_namespace_id(),
        plan->get_data_table().get_table_id(), tablet, access_mode_);
    if (OB_SUCC(ret)) { ret = acquire(tx); }
    if (OB_SUCC(ret)) {
      execution.set_skip_flush_redo(skip_flush_redo);
      ret = operation(*share::server_service<ObIDmlService>(), access.tablet(), tx, execution);
    }
    return ret;
  }
};

struct EngineWrites {
  ns::NamespaceCatalogViews::Handle transaction_view;
  StorageSpaceHandle storage_space;
  data_plane::ObNamespaceAccessMode namespace_access_mode;
  sql::ObSQLSessionInfo &session;
  uint32_t sid;
  ObTxDesc *&tx;
  uint64_t sequence = 0;
  std::map<uint64_t, std::unique_ptr<EngineWrite>> writes;
  explicit EngineWrites(StorageSpaceHandle space, sql::ObSQLSessionInfo &s,
                        data_plane::ObNamespaceAccessMode access_mode)
      : storage_space(space), namespace_access_mode(access_mode),
        session(s), sid(s.get_server_sid()), tx(s.get_tx_desc()) {}
  void reset() {
    session.reset_reserved_snapshot_version();
    writes.clear();
    if (tx) {
      auto *service = query_transaction_service();
      const bool rollback = !tx->is_shadow() && tx->is_in_tx() && !tx->is_tx_end();
      if (rollback) { service->rollback_tx(*tx); }
      service->release_tx(*tx);
      tx = nullptr;
      fprintf(stderr, "PROTOTYPE_V14_TX_RELEASED session=%u rollback=%d\n", sid, rollback);
    }
    transaction_view.reset();
  }
  ~EngineWrites() { reset(); }
  int release(uint64_t txid) {
    if (!tx || static_cast<uint64_t>(tx->get_tx_id().get_id()) != txid
        || !writes.empty()) { return OB_INVALID_ARGUMENT; }
    reset();
    return OB_SUCCESS;
  }
  int close(uint64_t txid, uint64_t handle, ObTxDesc &view) {
    if (!tx || static_cast<uint64_t>(tx->get_tx_id().get_id()) != txid
        || static_cast<uint64_t>(view.get_tx_id().get_id()) != txid) {
      return OB_INVALID_ARGUMENT;
    }
    auto it = writes.find(handle);
    if (it == writes.end()) { return OB_INVALID_ARGUMENT; }
    writes.erase(it);
    return query_transaction_service()->merge_tx_state(view, *tx);
  }
  int prepare(const WritePrepareRequest &request, const ObTxDesc &view,
              uint64_t &handle) {
    handle = 0;
    if (!tx || tx->get_tx_id() != view.get_tx_id()
        || (request.storage_space != storage_space
            && !request.storage_space.is_global())) { return OB_INVALID_ARGUMENT; }
    if (writes.size() >= 32) { return OB_SIZE_OVERFLOW; }
    auto prepared = std::make_unique<EngineWrite>();
    int ret = prepared->prepare(request, namespace_access_mode, *tx);
    if (!ret) {
      handle = ++sequence;
      writes.emplace(handle, std::move(prepared));
    }
    return ret;
  }
  int invoke(const ObTxDesc &view, uint64_t handle, const ObTabletID &tablet,
             bool skip_flush_redo, const NativeWriteOperation &operation) {
    if (!tx || tx->get_tx_id() != view.get_tx_id() || !operation) { return OB_INVALID_ARGUMENT; }
    auto it = writes.find(handle);
    return it == writes.end() ? OB_INVALID_ARGUMENT
        : it->second->invoke(tablet, *tx, skip_flush_redo, operation);
  }
};

// Compatibility view for query's existing descriptor accessors. Constructing
// and decoding this value does not start a transaction service, register a
// transaction, or allocate a storage context in the worker. Engine owns all
// authoritative transaction state; this view is refreshed by native calls.
// Ticket 05c: resolve the transaction's owning session when the ambient
// worker names another session or none (async end-trans completion runs off
// the query thread). A session borrowed from the session manager is returned
// through `borrowed` and must be reverted by the caller.
sql::ObSQLSessionInfo *tx_owner_session(transaction::ObTxDesc &tx,
                                        sql::ObSQLSessionInfo *&borrowed)
{
  borrowed = nullptr;
  sql::ObSQLSessionInfo *session = THIS_WORKER.get_session();
  if (session != nullptr && session->get_tx_desc() == &tx) { return session; }
  auto *mgr = share::server_service<sql::ObSQLSessionMgr>();
  sql::ObSQLSessionInfo *resolved = nullptr;
  if (OB_NOT_NULL(mgr)
      && OB_SUCCESS == mgr->get_session(tx.get_session_id(), resolved)
      && OB_NOT_NULL(resolved) && resolved->get_tx_desc() == &tx
      && in_process_session_ns(resolved) > 0) {
    borrowed = resolved;
    return resolved;
  }
  if (OB_NOT_NULL(resolved)) { mgr->revert_session(resolved); }
  return session;
}
void revert_tx_owner_session(sql::ObSQLSessionInfo *borrowed)
{
  if (OB_NOT_NULL(borrowed)) {
    share::server_service<sql::ObSQLSessionMgr>()->revert_session(borrowed);
  }
}
int call_in_process_tx_state(char operation, ObTxDesc &view,
                             const ObTxParam *param, int64_t deadline);
int call_in_process_tx_read_snapshot(ObTxDesc &view,
    ObTxIsolationLevel isolation, int64_t deadline, ObTxReadSnapshot &snapshot);
int call_in_process_tx_create_savepoint(ObTxDesc &view, char operation,
    const ObTxParam *param, bool release, int16_t branch, ObTxSEQ &savepoint);
int call_in_process_tx_rollback_savepoint(ObTxDesc &view, ObTxSEQ savepoint,
    int64_t deadline, bool touched_storage, ObTxCleanPolicy policy);
int call_in_process_tx_named_savepoint(ObTxDesc &view, char operation,
    const ObString &name, int64_t deadline);
int call_in_process_tx_exec_result(ObTxDesc &view, char operation,
    const ObTxExecResult *input, ObTxExecResult *output);
int call_in_process_tx_register_mds(ObTxDesc &view,
    ObTxDataSourceType type, StorageSpaceHandle space,
    const char *buffer, int64_t buffer_size,
    const ObRegisterMdsFlag &flag, ObTxSEQ sequence);
int call_in_process_tx_table_lock(ObTxDesc &view,
    obcall::ObInnerSQLTransmitArg::InnerSQLOperationType operation,
    StorageSpaceHandle space, const ObTxParam &param,
    const ObString &payload, const TableLockPlan &plan);
int call_in_process_tablet_binding(ObTxDesc &view, char operation,
    StorageSpaceHandle space, const ObIArray<ObTabletID> &tablets,
    const ObIArray<ObTabletID> *hidden, int64_t schema_version,
    int64_t deadline);
int tx_state(char operation, ObTxDesc &view,
             const ObTxParam *param = nullptr, int64_t deadline = 0)
{
  sql::ObSQLSessionInfo *borrowed = nullptr;
  auto *session = tx_owner_session(view, borrowed);
  StorageSessionScope scope(session && session->get_tx_desc() == &view ? session : nullptr);
  const int ret = scope.error() ? scope.error()
      : call_in_process_tx_state(operation, view, param, deadline);
  revert_tx_owner_session(borrowed);
  return ret;
}
int tx_read_snapshot(ObTxDesc &view, ObTxIsolationLevel isolation,
                     int64_t deadline, ObTxReadSnapshot &snapshot)
{
  sql::ObSQLSessionInfo *borrowed = nullptr;
  auto *session = tx_owner_session(view, borrowed);
  StorageSessionScope scope(session && session->get_tx_desc() == &view ? session : nullptr);
  const int ret = scope.error() ? scope.error()
      : call_in_process_tx_read_snapshot(view, isolation, deadline, snapshot);
  revert_tx_owner_session(borrowed);
  return ret;
}
int tx_create_savepoint(ObTxDesc &view, char operation,
    const ObTxParam *param, bool release, int16_t branch, ObTxSEQ &savepoint)
{
  sql::ObSQLSessionInfo *borrowed = nullptr;
  auto *session = tx_owner_session(view, borrowed);
  StorageSessionScope scope(session && session->get_tx_desc() == &view ? session : nullptr);
  const int ret = scope.error() ? scope.error()
      : call_in_process_tx_create_savepoint(
          view, operation, param, release, branch, savepoint);
  revert_tx_owner_session(borrowed);
  return ret;
}
int tx_rollback_savepoint(ObTxDesc &view, ObTxSEQ savepoint,
    int64_t deadline, bool touched_storage, ObTxCleanPolicy policy)
{
  sql::ObSQLSessionInfo *borrowed = nullptr;
  auto *session = tx_owner_session(view, borrowed);
  StorageSessionScope scope(session && session->get_tx_desc() == &view ? session : nullptr);
  const int ret = scope.error() ? scope.error()
      : call_in_process_tx_rollback_savepoint(
          view, savepoint, deadline, touched_storage, policy);
  revert_tx_owner_session(borrowed);
  return ret;
}
int tx_named_savepoint(ObTxDesc &view, char operation,
    const ObString &name, int64_t deadline = 0)
{
  sql::ObSQLSessionInfo *borrowed = nullptr;
  auto *session = tx_owner_session(view, borrowed);
  StorageSessionScope scope(session && session->get_tx_desc() == &view ? session : nullptr);
  const int ret = scope.error() ? scope.error()
      : call_in_process_tx_named_savepoint(view, operation, name, deadline);
  revert_tx_owner_session(borrowed);
  return ret;
}
int tx_exec_result(ObTxDesc &view, char operation,
    const ObTxExecResult *input, ObTxExecResult *output)
{
  sql::ObSQLSessionInfo *borrowed = nullptr;
  auto *session = tx_owner_session(view, borrowed);
  StorageSessionScope scope(session && session->get_tx_desc() == &view ? session : nullptr);
  const int ret = scope.error() ? scope.error()
      : call_in_process_tx_exec_result(view, operation, input, output);
  revert_tx_owner_session(borrowed);
  return ret;
}
int tx_register_mds(ObTxDesc &view, ObTxDataSourceType type,
    StorageSpaceHandle space, const char *buffer, int64_t buffer_size,
    const ObRegisterMdsFlag &flag, ObTxSEQ sequence)
{
  sql::ObSQLSessionInfo *borrowed = nullptr;
  auto *session = tx_owner_session(view, borrowed);
  StorageSessionScope scope(session && session->get_tx_desc() == &view ? session : nullptr);
  const int ret = scope.error() ? scope.error()
      : call_in_process_tx_register_mds(
          view, type, space, buffer, buffer_size, flag, sequence);
  revert_tx_owner_session(borrowed);
  return ret;
}
int tx_table_lock(ObTxDesc &view,
    obcall::ObInnerSQLTransmitArg::InnerSQLOperationType operation,
    StorageSpaceHandle space, const ObTxParam &param,
    const ObString &payload, const TableLockPlan &plan)
{
  sql::ObSQLSessionInfo *borrowed = nullptr;
  auto *session = tx_owner_session(view, borrowed);
  StorageSessionScope scope(session && session->get_tx_desc() == &view ? session : nullptr);
  const int ret = scope.error() ? scope.error()
      : call_in_process_tx_table_lock(
          view, operation, space, param, payload, plan);
  revert_tx_owner_session(borrowed);
  return ret;
}

class InProcessInnerConnectionLockRuntime final : public ObIInnerConnectionLockRuntime
{
public:
  int process_lock_rpc(
      const obcall::ObInnerSQLTransmitArg &arg,
      common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke_raw(arg.get_operation_type(), arg.get_inner_sql(), conn);
  }

  int lock_table(uint64_t table_id,
                 ObTableLockMode lock_mode,
                 int64_t timeout_us,
                 common::sqlclient::ObISQLConnection *conn,
                 ObTableLockOwnerID owner_id,
                 ObTableLockPriority lock_priority) override
  {
    ObLockTableRequest arg;
    arg.table_id_ = table_id;
    arg.owner_id_ = owner_id;
    arg.lock_mode_ = lock_mode;
    arg.op_type_ = IN_TRANS_COMMON_LOCK;
    arg.timeout_us_ = timeout_us;
    arg.lock_priority_ = lock_priority;
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_LOCK_TABLE, arg, conn);
  }

  int lock_table(const ObLockTableRequest &arg,
                 common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_LOCK_TABLE, arg, conn);
  }

  int unlock_table(const ObUnLockTableRequest &arg,
                   common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_UNLOCK_TABLE, arg, conn);
  }

  int lock_tablet(uint64_t table_id,
                  ObTabletID tablet_id,
                  ObTableLockMode lock_mode,
                  int64_t timeout_us,
                  common::sqlclient::ObISQLConnection *conn) override
  {
    ObLockTabletsRequest arg;
    arg.table_id_ = table_id;
    arg.owner_id_.set_default();
    arg.lock_mode_ = lock_mode;
    arg.op_type_ = IN_TRANS_COMMON_LOCK;
    arg.timeout_us_ = timeout_us;
    int ret = arg.tablet_ids_.push_back(tablet_id);
    return ret ? ret : invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_LOCK_TABLET, arg, conn);
  }

  int lock_tablet(uint64_t table_id,
                  const ObIArray<ObTabletID> &tablet_ids,
                  ObTableLockMode lock_mode,
                  int64_t timeout_us,
                  common::sqlclient::ObISQLConnection *conn) override
  {
    ObLockTabletsRequest arg;
    arg.table_id_ = table_id;
    arg.owner_id_.set_default();
    arg.lock_mode_ = lock_mode;
    arg.op_type_ = IN_TRANS_COMMON_LOCK;
    arg.timeout_us_ = timeout_us;
    int ret = arg.tablet_ids_.assign(tablet_ids);
    return ret ? ret : invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_LOCK_TABLET, arg, conn);
  }

  int lock_tablet(const ObLockAloneTabletRequest &arg,
                  common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_LOCK_ALONE_TABLET, arg, conn);
  }

  int unlock_tablet(const ObUnLockAloneTabletRequest &arg,
                    common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_UNLOCK_ALONE_TABLET, arg, conn);
  }

  int lock_obj(const ObLockObjRequest &arg,
               common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_LOCK_OBJ, arg, conn);
  }

  int unlock_obj(const ObUnLockObjRequest &arg,
                 common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_UNLOCK_OBJ, arg, conn);
  }

  int lock_obj(const ObLockObjsRequest &arg,
               common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_LOCK_OBJS, arg, conn);
  }

  int unlock_obj(const ObUnLockObjsRequest &arg,
                 common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_UNLOCK_OBJS, arg, conn);
  }

  int replace_lock(const ObReplaceLockRequest &arg,
                   common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_REPLACE_LOCK, arg, conn);
  }

  int replace_lock(const ObReplaceAllLocksRequest &arg,
                   common::sqlclient::ObISQLConnection *conn) override
  {
    return invoke(obcall::ObInnerSQLTransmitArg::OPERATION_TYPE_REPLACE_LOCKS, arg, conn);
  }

  int execute_write_sql(common::sqlclient::ObISQLConnection *conn,
                        const ObSqlString &sql,
                        int64_t &affected_rows) override
  {
    auto *inner = static_cast<ObInnerSQLConnection *>(conn);
    return inner ? inner->execute_write(sql.ptr(), affected_rows) : OB_INVALID_ARGUMENT;
  }

  int execute_read_sql(common::sqlclient::ObISQLConnection *conn,
                       const ObSqlString &sql,
                       ObISQLClient::ReadResult &result) override
  {
    auto *inner = static_cast<ObInnerSQLConnection *>(conn);
    return inner ? inner->execute_read(sql.ptr(), result) : OB_INVALID_ARGUMENT;
  }

private:
  int invoke_raw(obcall::ObInnerSQLTransmitArg::InnerSQLOperationType operation,
                 const ObString &payload,
                 common::sqlclient::ObISQLConnection *conn)
  {
    auto *inner = static_cast<ObInnerSQLConnection *>(conn);
    ObTxDesc *tx = inner ? inner->get_session().get_tx_desc() : nullptr;
    if (!inner || !inner->is_in_trans() || !tx || payload.empty()) {
      return OB_INVALID_ARGUMENT;
    }
    // An inner SQL lock may run while another session is ambient. Restore the
    // inner session's storage route so locks and catalog writes share its tx.
    if (!inner->get_session().namespace_storage_binding()) {
      return OB_NOT_INIT;
    }
    StorageSessionScope storage_scope(&inner->get_session(), false);
    if (storage_scope.error()) {
      return storage_scope.error();
    }
    ObTxParam param;
    param.access_mode_ = ObTxAccessMode::RW;
    param.isolation_ = inner->get_session().get_tx_isolation();
    inner->get_session().get_tx_timeout(param.timeout_us_);
    param.lock_timeout_us_ = inner->get_session().get_trx_lock_timeout();
    TableLockPlan lock_plan;
    lock_plan.storage_space = active_worker_storage_space();
    int ret = OB_SUCCESS;
    if (is_schema_table_lock_operation(operation)) {
      ret = append_worker_table_lock_plan(operation, payload, lock_plan);
    }
    return ret ? ret : tx_table_lock(
        *tx, operation, lock_plan.storage_space, param, payload, lock_plan);
  }

  template <typename T>
  int invoke(obcall::ObInnerSQLTransmitArg::InnerSQLOperationType operation,
             const T &arg,
             common::sqlclient::ObISQLConnection *conn)
  {
    const int64_t size = arg.get_serialize_size();
    if (size <= 0 || size > static_cast<int64_t>(MAX_SQL_MESSAGE)) {
      return OB_SIZE_OVERFLOW;
    }
    std::vector<char> buffer(size);
    int64_t pos = 0;
    int ret = arg.serialize(buffer.data(), buffer.size(), pos);
    if (!ret && pos != size) { ret = OB_ERR_UNEXPECTED; }
    return ret ? ret : invoke_raw(
        operation, ObString(static_cast<int32_t>(pos), buffer.data()), conn);
  }
};

int call_in_process_tx_clock(
    const std::function<int(ObITransactionService &)> &call);
int call_in_process_tx_interrupt(const transaction::ObTxDesc &tx, int cause);
int call_in_process_tx_snapshot(char operation,
                                transaction::ObTxReadSnapshot &snapshot);
int release_in_process_tx(const transaction::ObTxDesc &tx);
int close_in_process_write(transaction::ObTxDesc &view, uint64_t handle);
