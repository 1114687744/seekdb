// Native integration probe, included only in disposable test binaries.
static thread_local std::function<int()> layout_retention_sample_hook;
int run_layout_retention_sample_hook()
{
  return layout_retention_sample_hook ? layout_retention_sample_hook() : OB_SUCCESS;
}

static int run_layout_retention_native_probe()
{
  using Tx = InstanceMetaStore::Transaction;
  auto &store = share::server_service<ObAccessService>()->storage_schema_store();
  auto &freezes = *share::server_service<ObFreezeInfoMgr>();
  auto &transactions = *share::server_service<transaction::ObTransService>();
  auto &scheduler = *compaction::ObBasicMergeScheduler::get_merge_scheduler();
  auto &proxy = *directory_sql_proxy();
  auto deadline = [] { return ObTimeUtility::current_time() + 120000000; };
  // Test-only markers stay in the existing layout KV; the data under test is
  // the original Namespace table/column history, accessed by NativeSchemaCatalog.
  const uint64_t fixture_table_id = 900000102;
  const auto collection = static_cast<MetaCollection>(10042);
  const auto key = ObString::make_string("freeze");
  using Catalog = observer::NativeSchemaCatalog;
  auto &access = *share::server_service<ObAccessService>();
  ObArenaAllocator allocator(ObMemAttr("LayoutRetention"));
  int ret = OB_SUCCESS;
#define RETENTION_CALL(expr) do { ret = (expr); if (ret != OB_SUCCESS) { \
  fprintf(stderr, "LAYOUT_RETENTION_FAIL line=%d ret=%d expr=%s\n", __LINE__, ret, #expr); return ret; } } while (0)
#define RETENTION_CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "LAYOUT_RETENTION_FAIL line=%d ret=%d check=%s\n", __LINE__, ret, #expr); return OB_ERR_UNEXPECTED; } } while (0)
  auto reload = [&]() {
    const int64_t until = deadline();
    int rc;
    do {
      rc = freezes.reload_for_test();
      if (rc == OB_EAGAIN) { usleep(100000); }
    } while (rc == OB_EAGAIN && ObTimeUtility::current_time() < until);
    return rc;
  };
  // This probe controls the persisted completion input. It does not pretend
  // that advancing this input tests the still-separate checksum implementation.
  ObGlobalMergeInfo progress;
  RETENTION_CALL(ObGlobalMergeTableOperator::load_global_merge_info(proxy, progress));
  progress.suspend_merging_.set_val(true, true);
  RETENTION_CALL(ObGlobalMergeTableOperator::update_partial_global_merge_info(proxy, progress));
  scheduler.stop_major_merge();
  scheduler.set_inner_table_merged_scn(progress.last_merged_scn().get_val_for_tx());

  Tx state;
  RETENTION_CALL(store.begin(state, deadline(), true));
  ObString saved;
  ret = store.get(state, collection, key, allocator, saved);
  const bool recovered = ret == OB_SUCCESS;
  RETENTION_CHECK(recovered || ret == OB_ENTRY_NOT_EXIST);
  SCN frozen;
  if (recovered) {
    RETENTION_CHECK(saved.length() == sizeof(int64_t));
    int64_t value = 0;
    MEMCPY(&value, saved.ptr(), sizeof(value));
    RETENTION_CALL(frozen.convert_for_tx(value));
  }
  RETENTION_CALL(store.commit(state));
  RETENTION_CALL(store.begin(state, deadline(), true));
  ret = store.get(state, collection, ObString::make_string("released"), allocator, saved);
  const bool released = ret == OB_SUCCESS;
  RETENTION_CHECK(released || ret == OB_ENTRY_NOT_EXIST);
  RETENTION_CALL(store.commit(state));
  auto read_at = [&](const SCN &snapshot, int64_t expected) -> int {
    Catalog catalog(1, access, transactions);
    RETENTION_CALL(catalog.begin_read_at(deadline(), snapshot));
    ObStorageSchema layout;
    RETENTION_CALL(catalog.get_storage_schema(fixture_table_id, allocator, layout));
    RETENTION_CHECK(layout.get_schema_version() == expected && !layout.is_column_info_simplified());
    return OB_SUCCESS;
  };
  auto read_latest = [&](int64_t expected) -> int {
    Catalog catalog(1, access, transactions);
    RETENTION_CALL(catalog.begin_read(deadline()));
    ObStorageSchema layout;
    RETENTION_CALL(catalog.get_storage_schema(fixture_table_id, allocator, layout));
    RETENTION_CHECK(layout.get_schema_version() == expected && !layout.is_column_info_simplified());
    return OB_SUCCESS;
  };
  if (released) {
    RETENTION_CALL(reload());
    Catalog expired(1, access, transactions);
    RETENTION_CHECK(expired.begin_read_at(deadline(), frozen) == OB_SNAPSHOT_DISCARDED);
    RETENTION_CALL(read_latest(12));
    fprintf(stderr, "LAYOUT_RETENTION_RELEASE_RECOVERED original_history=1 current=12\n");
    return OB_SUCCESS;
  }
  auto publish = [&](int64_t version) -> int {
    ObTableSchema logical(&allocator);
    RETENTION_CALL(InstanceMetaStore::build_schema(storage::InstanceTable::COUNTERS, logical));
    logical.set_table_id(fixture_table_id);
    logical.set_tablet_id(ObTabletID(fixture_table_id));
    logical.set_schema_version(version);
    NativeInnerTable native(access, transactions);
    NativeInnerTable::Transaction owner;
    Catalog writer(1, access, transactions);
    RETENTION_CALL(native.begin(owner, deadline()));
    RETENTION_CALL(native.with_native_transaction(owner, [&](transaction::ObTxDesc &tx) {
      return writer.attach(tx, deadline());
    }));
    // Actually DELETE old logical history rows after F. Merely appending a
    // different schema_version would not exercise MVCC retention of those rows.
    for (uint64_t inner : {OB_ALL_TABLE_HISTORY_TID, OB_ALL_COLUMN_HISTORY_TID}) {
      int64_t affected = 0;
      RETENTION_CALL(writer.erase_prefix(inner, [&](SchemaCatalogRow &fields) {
        return fields.add_pk_column("table_id", fixture_table_id);
      }, affected));
    }
    RETENTION_CALL(writer.insert_table(logical, true));
    for (auto it = logical.column_begin(); it != logical.column_end(); ++it) {
      ObColumnSchemaV2 column;
      RETENTION_CALL(column.assign(**it));
      column.set_table_id(fixture_table_id);
      column.set_schema_version(version);
      RETENTION_CALL(writer.insert_column(column, true));
    }
    RETENTION_CALL(native.commit(owner));
    RETENTION_CALL(writer.close());
    return OB_SUCCESS;
  };
  if (!recovered) {
    RETENTION_CALL(publish(10));
    ObMySQLTransaction sql;
    Tx reader;
    RETENTION_CALL(sql.start(&proxy));
    SCN fence;
    RETENTION_CALL(ObGlobalStatProxy::select_snapshot_gc_scn_for_update(sql, fence));
    RETENTION_CALL(store.begin_read(reader, deadline(), [&](SCN &snapshot) {
      return transactions.get_read_snapshot_version(deadline(), snapshot);
    }));
    frozen = reader.snapshot_version();
    RETENTION_CHECK(frozen > fence);
    ObFreezeInfo info;
    info.frozen_scn_ = frozen;
    info.data_version_ = DATA_CURRENT_VERSION;
    RETENTION_CALL(ObFreezeInfoProxy().set_freeze_info(sql, info));
    RETENTION_CALL(sql.end(true));
    RETENTION_CALL(store.commit(reader));
    RETENTION_CALL(publish(11));
    Tx marker;
    RETENTION_CALL(store.begin(marker, deadline()));
    const int64_t value = frozen.get_val_for_tx();
    RETENTION_CALL(store.put(marker, collection, key,
        ObString(sizeof(value), reinterpret_cast<const char *>(&value))));
    RETENTION_CALL(store.commit(marker));
  }

  // Move the persisted GC fence beyond F. With no old active reader, only the
  // unfinished freeze should prevent metadata mini/minor from discarding F.
  {
    ObMySQLTransaction sql;
    RETENTION_CALL(sql.start(&proxy));
    SCN previous, current;
    RETENTION_CALL(ObGlobalStatProxy::select_snapshot_gc_scn_for_update(sql, previous));
    RETENTION_CALL(transactions.get_read_snapshot_version(deadline(), current));
    RETENTION_CHECK(current > frozen && current >= previous);
    int64_t affected = 0;
    RETENTION_CALL(ObGlobalStatProxy::update_snapshot_gc_scn(sql, current, affected));
    RETENTION_CALL(sql.end(true));
  }
  // Allow a concurrent ordinary reload's immutable plan to finish first.
  RETENTION_CALL(reload());
  SCN retained, active;
  RETENTION_CALL(freezes.get_schema_history_retention(retained));
  RETENTION_CHECK(retained == frozen);
  const int64_t weak_deadline = deadline();
  do {
    RETENTION_CALL(store.min_retained_snapshot(active));
    if (active <= frozen) { usleep(100000); }
  } while (active <= frozen && ObTimeUtility::current_time() < weak_deadline);
  RETENTION_CHECK(active > frozen);

  ObLS *ls = nullptr;
  RETENTION_CALL(share::server_service<ObLSService>()->get_ls(ls));
  const ObTabletID tablet_id(encoded(1, OB_ALL_TABLE_HISTORY_TID));
  const ObTabletID column_tablet_id(encoded(1, OB_ALL_COLUMN_HISTORY_TID));
  auto dump_and_minor = [&]() -> int {
    std::map<uint64_t, std::vector<SCN>> dumped_ends;
    // Repeated writes of the current definition generate real history-table
    // memtables without advancing the logical schema version.
    for (int i = 0; i < 3; ++i) {
      RETENTION_CALL(publish(recovered ? 12 : 11));
      for (const auto &id : {tablet_id, column_tablet_id}) {
        ObTabletHandle handle;
        RETENTION_CALL(ls->get_tablet(id, handle));
        const SCN before = handle.get_obj()->get_tablet_meta().clog_checkpoint_scn_;
        RETENTION_CALL(ls->tablet_freeze(id, true, deadline(), false, ObFreezeSourceFlag::TEST_MODE));
        bool dumped = false;
        const int64_t limit = deadline();
        while (!dumped && ObTimeUtility::current_time() < limit) {
          handle.reset();
          RETENTION_CALL(ls->get_tablet(id, handle));
          dumped = handle.get_obj()->get_tablet_meta().clog_checkpoint_scn_ > before;
          if (!dumped) { usleep(100000); }
        }
        RETENTION_CHECK(dumped);
        dumped_ends[id.id()].push_back(handle.get_obj()->get_tablet_meta().clog_checkpoint_scn_);
      }
    }
    for (const auto &id : {tablet_id, column_tablet_id}) {
      bool compacted = false;
      const int64_t limit = deadline();
      while (!compacted && ObTimeUtility::current_time() < limit) {
        ObTabletHandle handle;
        RETENTION_CALL(ls->get_tablet(id, handle));
        ObTabletMemberWrapper<ObTabletTableStore> tables;
        RETENTION_CALL(handle.get_obj()->fetch_table_store(tables));
        const auto &files = tables.get_member()->get_minor_sstables();
        for (int64_t index = 0; index < files.count(); ++index) {
          const auto *file = files.at(index);
          int covered = 0;
          for (const auto &end : dumped_ends.at(id.id())) {
            covered += file->get_start_scn() < end && end <= file->get_end_scn();
          }
          // Freeze boundaries may legitimately leave several older files.
          // Prove a real minor combined this run's dumps instead of assuming
          // a maximum total file count across all historical boundaries.
          compacted |= covered >= 2;
        }
        if (!compacted) { usleep(100000); }
      }
      RETENTION_CHECK(compacted);
    }
    return OB_SUCCESS;
  };
  RETENTION_CALL(dump_and_minor());
  RETENTION_CALL(read_at(frozen, 10));
  RETENTION_CALL(read_latest(recovered ? 12 : 11));
  for (const auto &id : {tablet_id, column_tablet_id}) {
    ObTabletHandle handle;
    RETENTION_CALL(ls->get_tablet(id, handle));
    RETENTION_CHECK(handle.get_obj()->get_multi_version_start() <= frozen.get_val_for_tx());
  }
  if (recovered) {
    // Deterministically interleave a reader and completion between the
    // policy's two samples. This must never permit reclaiming that reader's F.
    Catalog late_reader(1, access, transactions);
    bool interleaved = false;
    layout_retention_sample_hook = [&]() -> int {
      RETENTION_CALL(late_reader.begin_read_at(deadline(), frozen));
      progress.last_merged_scn_.set_scn(frozen, true);
      progress.global_broadcast_scn_.set_scn(frozen, true);
      progress.frozen_scn_.set_scn(frozen, true);
      RETENTION_CALL(ObGlobalMergeTableOperator::update_partial_global_merge_info(proxy, progress));
      scheduler.set_inner_table_merged_scn(frozen.get_val_for_tx());
      RETENTION_CALL(reload());
      interleaved = true;
      return OB_SUCCESS;
    };
    ObTabletHandle before_release;
    RETENTION_CALL(ls->get_tablet(tablet_id, before_release));
    ObVersionRange range;
    SCN fresh;
    RETENTION_CALL(transactions.get_read_snapshot_version(deadline(), fresh));
    range.snapshot_version_ = fresh.get_val_for_tx();
    range.multi_version_start_ = before_release.get_obj()->get_multi_version_start();
    ObStorageSnapshotInfo unused;
    ret = compaction::ObPartitionMergePolicy::get_multi_version_start(
        compaction::MINOR_MERGE, *ls, *before_release.get_obj(), range, unused);
    layout_retention_sample_hook = {};
    RETENTION_CHECK(ret == OB_SUCCESS && interleaved);
    RETENTION_CHECK(range.multi_version_start_ <= frozen.get_val_for_tx());
    ObStorageSchema held_layout;
    RETENTION_CALL(late_reader.get_storage_schema(fixture_table_id, allocator, held_layout));
    RETENTION_CHECK(held_layout.get_schema_version() == 10);
    RETENTION_CALL(late_reader.close());
    RETENTION_CALL(freezes.get_schema_history_retention(retained));
    RETENTION_CHECK(retained > frozen);
    RETENTION_CALL(publish(12));
    RETENTION_CALL(dump_and_minor());
    for (const auto &id : {tablet_id, column_tablet_id}) {
      ObTabletHandle handle;
      RETENTION_CALL(ls->get_tablet(id, handle));
      RETENTION_CHECK(handle.get_obj()->get_multi_version_start() > frozen.get_val_for_tx());
    }
    Catalog expired(1, access, transactions);
    RETENTION_CHECK(expired.begin_read_at(deadline(), frozen) == OB_SNAPSHOT_DISCARDED);
    RETENTION_CALL(read_latest(12));
    Tx marker;
    RETENTION_CALL(store.begin(marker, deadline()));
    RETENTION_CALL(store.put(marker, collection, ObString::make_string("released"),
        ObString::make_string("yes")));
    RETENTION_CALL(store.commit(marker));
    fprintf(stderr, "LAYOUT_RETENTION_RELEASE original_history=1 current=12 old_F_discarded=1\n");
  } else {
    // Model a paused replica whose primary has already retired this freeze.
    // Its local persisted broadcast is still unfinished, even though the
    // runtime scheduler has not started that broadcast (including on restart).
    progress.global_broadcast_scn_.set_scn(frozen, true);
    progress.frozen_scn_.set_scn(frozen, true);
    RETENTION_CALL(ObGlobalMergeTableOperator::update_partial_global_merge_info(proxy, progress));
    // A real primary retires F only after publishing a later freeze. Keep
    // that later row: ordinary schema tablets still use the freeze list for
    // minor selection even while this replica's broadcast remains at F.
    ObFreezeInfo later;
    RETENTION_CALL(transactions.get_read_snapshot_version(deadline(), later.frozen_scn_));
    later.data_version_ = DATA_CURRENT_VERSION;
    RETENTION_CHECK(later.frozen_scn_ > frozen);
    RETENTION_CALL(ObFreezeInfoProxy().set_freeze_info(proxy, later));
    RETENTION_CALL(ObFreezeInfoProxy().batch_delete(proxy, frozen));
    RETENTION_CHECK(scheduler.get_frozen_version() < frozen.get_val_for_tx());
    RETENTION_CALL(reload());
    RETENTION_CALL(freezes.get_schema_history_retention(retained));
    RETENTION_CHECK(retained == frozen);
  }
  fprintf(stderr, "LAYOUT_RETENTION_PASS recovered=%d F=%ld active=%ld durable=%ld original_history=1 mini_minor=1 released=%d handoff=%d\n",
      recovered, frozen.get_val_for_tx(), active.get_val_for_tx(), retained.get_val_for_tx(), recovered, recovered);
#undef RETENTION_CALL
#undef RETENTION_CHECK
  return OB_SUCCESS;
}
