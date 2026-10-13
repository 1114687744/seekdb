// Included inside oceanbase::observer::namespace_worker_prototype.
class InProcessWriteContext final : public ObIWriteContextService {
public:
  int acquire_write_context(int64_t, ObTxDesc &tx, const ObTxReadSnapshot &, int16_t,
                            concurrent_control::ObWriteFlag &, ObWriteContext &context) override {
    // Acquire the native context when the execution has its plan and snapshot.
    context.bind(&tx, nullptr); return OB_SUCCESS;
  }
};

struct InProcessExecution final : public ObIDmlExecutionState {
  uint64_t handle = 0, txid = 0;
  ObTxDesc *tx = nullptr; // Borrowed query view; never sent across IPC.
  sql::ObSQLSessionInfo *session = nullptr;
  int64_t deadline = 0;
  bool skip_flush_redo = false;
  void destroy() override {
    if (handle) {
      StorageSessionScope scope(session);
      if (!scope.error() && tx) { close_in_process_write(*tx, handle); }
    }
    delete this;
  }
  int64_t timeout() const override { return deadline; }
  void set_skip_flush_redo(bool skip) override { skip_flush_redo = skip; }
};

class CompletedLobReadCursor final : public common::ObILobReadCursor {
public:
  int get_next_row(ObString &) override { return OB_ITER_END; }
  void reset() override {}
};

int read_in_process_lob(common::ObLobLocatorV2 &locator, int64_t timeout,
                        common::ObIAllocator &allocator, common::ObString &output,
                        common::ObString *destination = nullptr);

class InProcessLobReadService final : public common::ObILobReadService {
public:
  void set_local(common::ObILobReadService *service) { local_ = service; }

  int get_outrow_lob_full_data(
      common::ObLobTextIterCtx &ctx,
      common::ObCollationType cs_type,
      bool has_lob_header,
      bool is_outrow,
      common::ObIAllocator *tmp_alloc, common::ObString *destination) override {
    return use_remote(ctx.locator_)
        ? (!has_lob_header || !is_outrow ? OB_INVALID_ARGUMENT : materialize(ctx, ctx.locator_, destination))
        : local_ ? local_->get_outrow_lob_full_data(
              ctx, cs_type, has_lob_header, is_outrow, tmp_alloc, destination) : OB_NOT_INIT;
  }

  int get_delta_lob_full_data(
      common::ObLobTextIterCtx &ctx,
      common::ObObjType type,
      common::ObCollationType cs_type,
      common::ObLobLocatorV2 &locator,
      common::ObIAllocator *allocator,
      common::ObString &data) override {
    return local_ ? local_->get_delta_lob_full_data(
        ctx, type, cs_type, locator, allocator, data) : OB_NOT_INIT;
  }

  int get_outrow_prefix_data(
      common::ObLobTextIterCtx &ctx,
      common::ObCollationType cs_type,
      bool has_lob_header,
      bool is_outrow,
      common::ObIAllocator *tmp_alloc,
      uint32_t prefix_char_len) override {
    if (!use_remote(ctx.locator_)) {
      return local_ ? local_->get_outrow_prefix_data(
          ctx, cs_type, has_lob_header, is_outrow, tmp_alloc, prefix_char_len) : OB_NOT_INIT;
    }
    int ret = !has_lob_header || !is_outrow ? OB_INVALID_ARGUMENT : materialize(ctx, ctx.locator_);
    if (!ret && ctx.content_byte_len_ > 0) {
      const int64_t chars = common::ObCharset::strlen_char(cs_type, ctx.buff_, ctx.content_byte_len_);
      const int64_t wanted = std::min<int64_t>(chars, prefix_char_len);
      ctx.content_byte_len_ = static_cast<uint32_t>(
          common::ObCharset::charpos(cs_type, ctx.buff_, ctx.content_byte_len_, wanted));
    }
    return ret;
  }

  int get_first_block(
      common::ObLobTextIterCtx &ctx,
      common::ObCollationType cs_type,
      bool has_lob_header,
      bool is_outrow,
      common::ObIAllocator *tmp_alloc,
      common::ObString &str,
      common::ObTextStringIterState &state) override {
    if (!use_remote(ctx.locator_)) {
      return local_ ? local_->get_first_block(
          ctx, cs_type, has_lob_header, is_outrow, tmp_alloc, str, state) : OB_NOT_INIT;
    }
    int ret = !has_lob_header || !is_outrow ? OB_INVALID_ARGUMENT : materialize(ctx, ctx.locator_);
    if (!ret) {
      free_lob_query_iter(ctx);
      // The reader materializes the full LOB; callers size their result for
      // the configured window, so expose only that window as the first block.
      const int64_t byte_len = ctx.content_byte_len_;
      const int64_t char_len = common::ObCharset::strlen_char(cs_type, ctx.buff_, byte_len);
      const int64_t start_char = static_cast<int64_t>(
          std::min<uint64_t>(ctx.start_offset_, static_cast<uint64_t>(char_len)));
      const int64_t read_chars = ctx.total_access_len_ == 0
          ? char_len - start_char
          : std::min<int64_t>(ctx.total_access_len_, char_len - start_char);
      if (read_chars < 0) { return OB_INVALID_ARGUMENT; }
      const int64_t start_byte = common::ObCharset::charpos(
          cs_type, ctx.buff_, byte_len, start_char);
      const int64_t end_byte = common::ObCharset::charpos(
          cs_type, ctx.buff_, byte_len, start_char + read_chars);
      if (start_byte > end_byte || end_byte > byte_len) {
        return OB_INVALID_ARGUMENT;
      }
      if (ctx.buff_ != nullptr) { ctx.buff_ += start_byte; }
      ctx.buff_byte_len_ -= static_cast<uint32_t>(start_byte);
      ctx.content_byte_len_ = static_cast<uint32_t>(end_byte - start_byte);
      ctx.content_len_ = static_cast<uint32_t>(read_chars);
      str.assign_ptr(ctx.buff_, ctx.content_byte_len_);
      ctx.accessed_byte_len_ = ctx.content_byte_len_;
      ctx.accessed_len_ = ctx.content_len_;
      ++ctx.iter_count_;
      if (ctx.content_byte_len_ == 0) {
        state = common::TEXTSTRING_ITER_END;
      } else if (OB_ISNULL(ctx.read_cursor_ = new (std::nothrow) CompletedLobReadCursor())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        state = common::TEXTSTRING_ITER_NEXT;
      }
    }
    return ret;
  }

  int get_next_block_inner(
      common::ObLobTextIterCtx &ctx,
      common::ObCollationType cs_type,
      bool has_lob_header,
      bool is_outrow,
      common::ObString &str,
      common::ObTextStringIterState &state) override {
    if (!use_remote(ctx.locator_)) {
      return local_ ? local_->get_next_block_inner(
          ctx, cs_type, has_lob_header, is_outrow, str, state) : OB_NOT_INIT;
    }
    if (!has_lob_header || !is_outrow || !ctx.read_cursor_) { return OB_INVALID_ARGUMENT; }
    free_lob_query_iter(ctx);
    str.reset();
    state = common::TEXTSTRING_ITER_END;
    return OB_SUCCESS;
  }

  int get_outrow_char_len(
      common::ObLobTextIterCtx &ctx,
      common::ObCollationType cs_type,
      common::ObIAllocator *tmp_alloc,
      int64_t &char_length) override {
    if (!use_remote(ctx.locator_)) {
      return local_ ? local_->get_outrow_char_len(ctx, cs_type, tmp_alloc, char_length) : OB_NOT_INIT;
    }
    int ret = materialize(ctx, ctx.locator_);
    if (!ret) {
      char_length = common::ObCharset::strlen_char(cs_type, ctx.buff_, ctx.content_byte_len_);
    }
    return ret;
  }

  void free_lob_query_iter(common::ObLobTextIterCtx &ctx) override {
    if (dynamic_cast<CompletedLobReadCursor *>(ctx.read_cursor_)) {
      delete static_cast<CompletedLobReadCursor *>(ctx.read_cursor_);
      ctx.read_cursor_ = nullptr;
    } else if (local_) { local_->free_lob_query_iter(ctx); }
  }

private:
  bool use_remote(common::ObLobLocatorV2 &locator) const {
    // All persistent LOB storage belongs to the shared process. This includes
    // namespace-local __all_* tables; routing those locators to the worker's
    // local LobManager loses the only process that owns their tablets.
    return locator.has_lob_header() && locator.is_persist_lob();
  }

  int materialize(common::ObLobTextIterCtx &ctx, common::ObLobLocatorV2 &locator,
                  common::ObString *destination = nullptr) {
    if (!ctx.alloc_ || !locator.has_lob_header()) { return OB_INVALID_ARGUMENT; }
    auto *session = THIS_WORKER.get_session();
    StorageSessionScope scope(session);
    if (scope.error()) { return scope.error(); }
    const int64_t timeout = ctx.timeout_ts_ > 0 ? ctx.timeout_ts_ : THIS_WORKER.get_timeout_ts();
    ObString data;
    int ret = read_in_process_lob(locator, timeout, *ctx.alloc_, data, destination);
    if (!ret && data.length() > UINT32_MAX) { ret = OB_INVALID_ARGUMENT; }
    if (!ret) {
      ctx.buff_ = data.empty() ? nullptr : data.ptr();
      ctx.buff_byte_len_ = static_cast<uint32_t>(data.length());
      ctx.content_byte_len_ = static_cast<uint32_t>(data.length());
      ctx.total_byte_len_ = data.length();
    }
    return ret;
  }

  common::ObILobReadService *local_ = nullptr;
};

int compare_in_process_lobs(ObLobLocatorV2 &left, ObLobLocatorV2 &right,
                            int64_t timeout, ObTxDesc &tx, bool &equal);

class InProcessDmlService final : public ObIDmlService {
public:
  int lob_binary_equal(
      ObLobLocatorV2 &left,
      ObLobLocatorV2 &right,
      int64_t timeout,
      ObTxDesc &tx,
      bool &equal) override {
    auto *session = THIS_WORKER.get_session();
    StorageSessionScope scope(session && session->get_tx_desc() == &tx ? session : nullptr);
    if (scope.error()) { return scope.error(); }
    return compare_in_process_lobs(left, right, timeout, tx, equal);
  }

  int prepare_execution(
      const ObDmlWriteSpec &write_spec,
      const ObDmlTablePlan &table_plan,
      const transaction::ObTxReadSnapshot &snapshot,
      common::ObIAllocator &allocator,
      const ObWriteContext &write_context,
      const concurrent_control::ObWriteFlag &write_flag,
      ObDmlExecution &execution) override {
    auto *session = THIS_WORKER.get_session();
    StorageSessionScope scope(session);
    if (scope.error()) { return scope.error(); }
    if (!write_context.is_valid()) { return OB_INVALID_ARGUMENT; }
    auto prepared = std::make_unique<InProcessExecution>();
    prepared->session = session;
    auto &tx = *static_cast<ObTxDesc *>(write_context.native_handle());
    prepared->tx = &tx;
    prepared->txid = tx.get_tx_id().get_id();
    prepared->deadline = write_spec.timeout_;
    // Release the previous context before replacing it, so its write state
    // reaches the transaction owner even when preparing the new one fails.
    execution.reset();
    const WritePrepareRequest request{active_worker_storage_space(), write_spec,
                                     table_plan, snapshot, write_flag};
    int ret = prepare_in_process_write(request, tx, prepared->handle);
    if (OB_SUCC(ret)) { bind_execution(execution, prepared.release()); }
    return ret;
  }
  int delete_rows(const ObTabletID &tablet, ObTxDesc &tx, const ObDmlExecution &execution,
      const ObIArray<uint64_t> &columns, ObDatumRowIterator *rows, int64_t &affected) override {
    return invoke(tablet, tx, execution, [&](ObIDmlService &storage, const ObTabletID &physical,
        ObTxDesc &owner, const ObDmlExecution &native) {
      return storage.delete_rows(physical, owner, native, columns, rows, affected);
    });
  }
  int put_rows(const ObTabletID &tablet, ObTxDesc &tx, const ObDmlExecution &execution,
      const ObIArray<uint64_t> &columns, ObDatumRowIterator *rows, int64_t &affected) override {
    return invoke(tablet, tx, execution, [&](ObIDmlService &storage, const ObTabletID &physical,
        ObTxDesc &owner, const ObDmlExecution &native) {
      return storage.put_rows(physical, owner, native, columns, rows, affected);
    });
  }
  int insert_rows(const ObTabletID &tablet, ObTxDesc &tx, const ObDmlExecution &execution,
      const ObIArray<uint64_t> &columns, ObDatumRowIterator *rows, int64_t &affected) override {
    return invoke(tablet, tx, execution, [&](ObIDmlService &storage, const ObTabletID &physical,
        ObTxDesc &owner, const ObDmlExecution &native) {
      return storage.insert_rows(physical, owner, native, columns, rows, affected);
    });
  }
  int insert_rows_fetch_duplicates(const ObTabletID &tablet, ObTxDesc &tx,
      const ObDmlExecution &execution, const ObIArray<uint64_t> &columns,
      const ObIArray<uint64_t> &duplicate_columns, ObDatumRowIterator *rows,
      ObDuplicateReturnMode mode, int64_t &affected, ObDatumRowIterator *&duplicates) override {
    return invoke(tablet, tx, execution, [&](ObIDmlService &storage, const ObTabletID &physical,
        ObTxDesc &owner, const ObDmlExecution &native) {
      return storage.insert_rows_fetch_duplicates(physical, owner, native, columns,
          duplicate_columns, rows, mode, affected, duplicates);
    });
  }
  void free_duplicate_rows_iterator(ObDatumRowIterator *iterator) override {
    share::server_service<ObIDmlService>()->free_duplicate_rows_iterator(iterator);
  }
  int update_rows(const ObTabletID &tablet, ObTxDesc &tx, const ObDmlExecution &execution,
      const ObIArray<uint64_t> &columns, const ObIArray<uint64_t> &updated_columns,
      ObDatumRowIterator *rows, int64_t &affected) override {
    return invoke(tablet, tx, execution, [&](ObIDmlService &storage, const ObTabletID &physical,
        ObTxDesc &owner, const ObDmlExecution &native) {
      return storage.update_rows(physical, owner, native, columns, updated_columns, rows, affected);
    });
  }
  int lock_rows(const ObTabletID &tablet, ObTxDesc &tx, const ObDmlExecution &execution,
      int64_t lock_timeout, ObRowLockMode mode, ObDatumRowIterator *rows, int64_t &affected) override {
    return invoke(tablet, tx, execution, [&](ObIDmlService &storage, const ObTabletID &physical,
        ObTxDesc &owner, const ObDmlExecution &native) {
      return storage.lock_rows(physical, owner, native, lock_timeout, mode, rows, affected);
    });
  }
private:
  int invoke(const ObTabletID &tablet, ObTxDesc &tx, const ObDmlExecution &execution,
             const NativeWriteOperation &operation) {
    auto *state = static_cast<InProcessExecution *>(execution_state(execution));
    if (state == nullptr || state->txid != static_cast<uint64_t>(tx.get_tx_id().get_id())) {
      return OB_INVALID_ARGUMENT;
    }
    StorageSessionScope scope(state->session);
    return scope.error() ? scope.error()
        : invoke_in_process_write(tx, state->handle, tablet, state->skip_flush_redo, operation);
  }
};
