// Deterministic reproduction for the pre-existing locked-root / old-page view
// mismatch. Opt-in only: this is a failing regression, not a passing gate.
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
  LOCK_ROOT_CALL(kv.commit(seed));

  Tx stale, writer;
  LOCK_ROOT_CALL(kv.begin(stale, deadline()));
  LOCK_ROOT_CALL(kv.begin(writer, deadline()));
  Metadata concurrent(kv, writer);
  LOCK_ROOT_CALL(concurrent.stage_catalog_delta(id, 1, 2, {}, changes(322)));
  LOCK_ROOT_CALL(kv.commit(writer));

  Metadata publication(kv, stale);
  LOCK_ROOT_CALL(publication.get_namespace(id, record, true));
  std::string page;
  const int page_ret = publication.read_page(record.roots.directory.page, page);
  const int stage_ret = publication.stage_catalog_delta(id, 2, 3, {}, changes(323));
  fprintf(stderr, "INSTANCE_CATALOG_LOCKED_ROOT_REPRO snapshot=%lu root_version=%ld page=%lu page_ret=%d stage_ret=%d\n",
      stale.snapshot_version().get_val_for_tx(), record.roots.schema_version,
      record.roots.directory.page, page_ret, stage_ret);
  LOCK_ROOT_CALL(kv.rollback(stale));
  // A correct directory writer must be able to follow the root it has locked.
  LOCK_ROOT_CALL(stage_ret);
  Tx cleanup;
  LOCK_ROOT_CALL(kv.begin(cleanup, deadline()));
  Metadata removing(kv, cleanup);
  LOCK_ROOT_CALL(removing.erase_namespace(id));
  LOCK_ROOT_CALL(kv.commit(cleanup));
#undef LOCK_ROOT_CALL
  return ret;
}
