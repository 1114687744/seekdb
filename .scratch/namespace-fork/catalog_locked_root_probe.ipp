// Deterministic regression for following a concurrently published immutable
// page root while ordinary reads keep their original fixed snapshot.
static int run_catalog_locked_root_probe()
{
  auto &kv = share::server_service<storage::ObAccessService>()->instance_meta_store();
  using Tx = storage::InstanceMetaStore::Transaction;
  using Metadata = rootserver::InstanceNamespaceMetadata;
  auto deadline = [] { return common::ObTimeUtility::current_time() + 30000000; };
  int ret = OB_SUCCESS;
#define LOCK_ROOT_CALL(expr) do { ret = (expr); if (ret != OB_SUCCESS) { \
  fprintf(stderr, "INSTANCE_META_PROBE_FAIL locked_root line=%d ret=%d operation=%s\n", \
      __LINE__, ret, #expr); return ret; } } while (0)
  auto changes = [](uint64_t tablet) {
    ns::CatalogChanges result;
    result[ns::NamespaceCatalogCodec::object_key(tablet)] = {
        {ns::NamespaceCatalogCodec::encode_source(
          {987, ns::NamespaceObjectKey{1, tablet}.storage_id(), 71, tablet, 0, 0}), 0}, false};
    return result;
  };
  const uint64_t id = 90001;
  Tx seed;
  LOCK_ROOT_CALL(kv.begin(seed, deadline()));
  Metadata initial(kv, seed);
  rootserver::InstanceNamespaceRecord record;
  record.id = id;
  record.name = "locked-root-reproduction";
  record.allow_login = false;
  record.roots.schema_version = 1;
  record.roots.snapshot = 1;
  LOCK_ROOT_CALL(initial.insert_namespace(record));
  LOCK_ROOT_CALL(initial.stage_catalog_delta(id, 1, 1, {}, changes(321)));
  rootserver::InstanceNamespaceRecord unrelated = record;
  unrelated.id = id + 1;
  unrelated.name += "-unlocked";
  LOCK_ROOT_CALL(initial.insert_namespace(unrelated));
  LOCK_ROOT_CALL(kv.commit(seed));

  Tx stale, writer;
  LOCK_ROOT_CALL(kv.begin(stale, deadline()));
  LOCK_ROOT_CALL(kv.begin(writer, deadline()));
  Metadata concurrent(kv, writer);
  LOCK_ROOT_CALL(concurrent.stage_catalog_delta(id, 1, 2, {}, changes(322)));
  unrelated.roots.schema_version = 2;
  LOCK_ROOT_CALL(concurrent.update_namespace(unrelated));
  LOCK_ROOT_CALL(kv.commit(writer));

  Metadata publication(kv, stale, Metadata::PageRead::LOCKED_ROOT);
  LOCK_ROOT_CALL(publication.get_namespace(id, record, true));
  std::string page;
  const int page_ret = publication.read_page(record.roots.directory.page, page);
  LOCK_ROOT_CALL(page_ret);
  rootserver::InstanceNamespaceRecord snapshot_record;
  LOCK_ROOT_CALL(publication.get_namespace(id + 1, snapshot_record));
  // Locking a row creates an own-transaction lock node; use a different,
  // unlocked row to verify the ordinary committed snapshot remains fixed.
  if (snapshot_record.roots.schema_version != 1) {
    fprintf(stderr, "INSTANCE_META_PROBE_FAIL locked_root fixed_snapshot expected=1 actual=%ld\n",
        snapshot_record.roots.schema_version);
    return OB_ERR_UNEXPECTED;
  }
  const int stage_ret = publication.stage_catalog_delta(id, 2, 3, {}, changes(323));
  fprintf(stderr, "INSTANCE_CATALOG_LOCKED_ROOT_REPRO snapshot=%lu root_version=%ld page=%lu page_ret=%d stage_ret=%d\n",
      stale.snapshot_version().get_val_for_tx(), record.roots.schema_version,
      record.roots.directory.page, page_ret, stage_ret);
  LOCK_ROOT_CALL(stage_ret);
  LOCK_ROOT_CALL(kv.commit(stale));
  Tx verify;
  LOCK_ROOT_CALL(kv.begin(verify, deadline(), true));
  Metadata committed(kv, verify);
  LOCK_ROOT_CALL(committed.get_namespace(id, record));
  if (record.roots.schema_version != 3) {
    fprintf(stderr, "INSTANCE_META_PROBE_FAIL locked_root committed_version=%ld\n", record.roots.schema_version);
    return OB_ERR_UNEXPECTED;
  }
  for (uint64_t tablet : {321, 322, 323}) {
    ns::CatalogTabletSource source;
    int64_t cap = 0;
    LOCK_ROOT_CALL(committed.find_tablet_source(record.roots.directory, tablet, source, cap));
  }
  LOCK_ROOT_CALL(kv.commit(verify));
  Tx cleanup;
  LOCK_ROOT_CALL(kv.begin(cleanup, deadline()));
  Metadata removing(kv, cleanup);
  LOCK_ROOT_CALL(removing.erase_namespace(id));
  LOCK_ROOT_CALL(removing.erase_namespace(id + 1));
  LOCK_ROOT_CALL(kv.commit(cleanup));
#undef LOCK_ROOT_CALL
  return ret;
}
