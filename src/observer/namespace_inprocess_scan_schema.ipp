int worker_storage_space_for_schema(const ObTableSchema &schema,
                                    ObSchemaGetterGuard &guard,
                                    StorageSpaceHandle &storage_space) {
  storage_space = StorageSpaceHandle();
  if (!serves_namespace_schema()
      || NamespaceForkKernelPrototype::is_encoded_id(schema.get_table_id())) {
    return OB_INVALID_ARGUMENT;
  }
  // Native all_* tables are the catalog of the namespace worker itself.
  if (is_inner_table(schema.get_table_id())) {
    storage_space = StorageSpaceHandle::namespace_space(serving_namespace());
    return storage_space.is_valid() ? OB_SUCCESS : OB_INVALID_ARGUMENT;
  }
  const ObDatabaseSchema *database = nullptr;
  int ret = guard.get_database_schema(schema.get_database_id(), database);
  if (OB_SUCC(ret) && OB_ISNULL(database)) {
    ret = OB_ERR_UNEXPECTED;
  }
  if (OB_SUCC(ret)) {
    storage_space = uses_global_storage_scope()
        ? StorageSpaceHandle::global_space()
        : StorageSpaceHandle::namespace_space(serving_namespace());
    if (!storage_space.is_valid()) { ret = OB_INVALID_ARGUMENT; }
  }
  return ret;
}
