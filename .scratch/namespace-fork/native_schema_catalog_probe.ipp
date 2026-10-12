// Injected into the test build after fork commits, before any child login.
static int probe_native_schema_catalog(uint64_t namespace_id, const std::string &name)
{
  const char *control = getenv("SEEKDB_NATIVE_SCHEMA_CATALOG_CONTROL");
  if (control == nullptr) { return OB_SUCCESS; }
  FILE *input = fopen(control, "r");
  if (input == nullptr) { return OB_SUCCESS; }
  char selected[128] = {};
  unsigned long long table_id = 0;
  int64_t expected_columns = 0, expected_parts = 0, expected_indexes = 0;
  int64_t expected_constraints = 0, expected_foreign_keys = 0, expected_default_length = 0;
  int64_t requested_version = INT64_MAX;
  const int fields = fscanf(input, "%127s %llu %ld %ld %ld %ld %ld %ld %ld", selected, &table_id,
      &expected_columns, &expected_parts, &expected_indexes, &expected_constraints,
      &expected_foreign_keys, &expected_default_length, &requested_version);
  fclose(input);
  if (fields < 3 || name != selected) { return OB_SUCCESS; }
  ns::NamespaceRuntime *runtime = nullptr;
  if (!ns::namespace_registry().get(namespace_id, runtime) || runtime == nullptr) {
    return OB_ERR_UNEXPECTED;
  }
  auto check_cold = [&]() {
    for (uint8_t slot = 0; slot < ns::NamespaceRuntime::SLOT_COUNT; ++slot) {
      if (runtime->service(static_cast<ns::NamespaceRuntime::ServiceSlot>(slot)) != nullptr) {
        return OB_ERR_UNEXPECTED;
      }
    }
    return OB_SUCCESS;
  };
  auto *access = share::server_service<storage::ObAccessService>();
  auto *transactions = share::server_service<transaction::ObTransService>();
  if (access == nullptr || transactions == nullptr) { return OB_NOT_INIT; }
  observer::NativeSchemaCatalog catalog(namespace_id, *access, *transactions);
  int ret = check_cold();
  if (ret == OB_SUCCESS) { ret = catalog.begin_read(ObTimeUtility::current_time() + 30000000); }
  auto column_index = [](const ObTableSchema &schema, const char *name, int64_t &index) {
    index = -1;
    const auto *column = schema.get_column_schema(ObString::make_string(name));
    if (column == nullptr) { return OB_ENTRY_NOT_EXIST; }
    ObSEArray<ObColDesc, 8> columns;
    int rc = schema.get_column_ids(columns);
    for (int64_t i = 0; rc == OB_SUCCESS && i < columns.count(); ++i) {
      if (columns.at(i).col_id_ == column->get_column_id()) { index = i; break; }
    }
    return rc != OB_SUCCESS ? rc : index < 0 ? OB_ERR_UNEXPECTED : OB_SUCCESS;
  };
  int64_t table_rows = 0, column_rows = 0;
  for (int pass = 0; ret == OB_SUCCESS && pass < 2; ++pass) {
    const uint64_t inner_id = pass == 0 ? OB_ALL_TABLE_HISTORY_TID : OB_ALL_COLUMN_HISTORY_TID;
    const int key_count = pass == 0 ? 2 : 3;
    ObObj lower[3], upper[3];
    lower[0].set_int(table_id); upper[0] = lower[0];
    for (int i = 1; i < key_count; ++i) { lower[i].set_min_value(); upper[i].set_max_value(); }
    ObNewRange range;
    range.start_key_.assign(lower, key_count); range.end_key_.assign(upper, key_count);
    range.border_flag_.set_inclusive_start(); range.border_flag_.set_inclusive_end();
    ret = catalog.scan(inner_id, range, [&](const ObTableSchema &schema, const ObNewRow &row, bool &) {
      if (row.cells_[0].get_int() != static_cast<int64_t>(table_id)) { return OB_ERR_UNEXPECTED; }
      int64_t name_index = -1, deleted_index = -1;
      int rc = column_index(schema, pass == 0 ? "table_name" : "column_name", name_index);
      if (rc == OB_SUCCESS) { rc = column_index(schema, "is_deleted", deleted_index); }
      if (rc == OB_SUCCESS && row.cells_[deleted_index].get_int() == 0
          && (row.cells_[name_index].is_null() || row.cells_[name_index].get_string().empty())) {
        rc = OB_ERR_UNEXPECTED;
      }
      if (rc == OB_SUCCESS) { ++(pass == 0 ? table_rows : column_rows); }
      return rc;
    });
  }
  if (ret == OB_SUCCESS && (table_rows < 1 || column_rows < expected_columns)) { ret = OB_ERR_UNEXPECTED; }
  if (ret == OB_SUCCESS) {
    observer::NativeSchemaCatalog::Cursor first, second;
    ObObj key; key.set_int(table_id);
    const ObRowkey prefix(&key, 1);
    ret = catalog.open_prefix(OB_ALL_COLUMN_HISTORY_TID, prefix, first);
    if (ret == OB_SUCCESS && (catalog.close() != OB_INVALID_ARGUMENT
        || catalog.open_prefix(OB_ALL_COLUMN_HISTORY_TID, prefix, second) != OB_INVALID_ARGUMENT)) {
      ret = OB_ERR_UNEXPECTED;
    }
    const ObNewRow *row = nullptr;
    if (ret == OB_SUCCESS) { ret = first.next(row); }
    const int ended = first.close();
    if (ret == OB_SUCCESS) { ret = ended; }
    if (ret == OB_SUCCESS && first.next(row) != OB_NOT_INIT) { ret = OB_ERR_UNEXPECTED; }
  }
  if (ret == OB_SUCCESS) {
    ObArenaAllocator allocator(ObMemAttr("CatalogProbe"));
    ObTableSchema *definition = nullptr;
    ret = catalog.get_table(table_id, requested_version, allocator, definition);
    if (ret == OB_SUCCESS && (definition == nullptr || !definition->is_valid()
        || definition->get_column_count() != expected_columns
        || definition->get_partition_num() != expected_parts
        || definition->get_index_tid_count() != expected_indexes
        || definition->get_constraint_count() != expected_constraints
        || definition->get_foreign_key_infos().count() != expected_foreign_keys
        || definition->get_table_name_str() != ObString::make_string("t"))) {
      ret = OB_ERR_UNEXPECTED;
    }
    if (definition != nullptr) {
      fprintf(stderr, "NATIVE_SCHEMA_CATALOG_SHAPE namespace=%lu columns=%ld expected_columns=%ld valid=%d\n",
          namespace_id, definition->get_column_count(), expected_columns, definition->is_valid());
      if (ret == OB_SUCCESS && (name.find("_dropped") != std::string::npos
          || name.find("_history") != std::string::npos)
          && definition->get_column_schema(ObString::make_string("n")) != nullptr) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (ret == OB_SUCCESS && name.find("_part2") != std::string::npos) {
      if (definition->get_part_level() != PARTITION_LEVEL_TWO) { ret = OB_ERR_UNEXPECTED; }
      for (int64_t i = 0; ret == OB_SUCCESS && i < definition->get_partition_num(); ++i) {
        if (definition->get_part_array()[i]->get_subpartition_num() != 2) { ret = OB_ERR_UNEXPECTED; }
      }
    }
    const auto *value_column = definition == nullptr ? nullptr
        : definition->get_column_schema(ObString::make_string("v"));
    if (ret == OB_SUCCESS && (value_column == nullptr
        || !value_column->get_cur_default_value().is_varchar()
        || (expected_default_length == 0
            ? value_column->get_cur_default_value().get_string() != ObString::make_string("初始")
            : value_column->get_cur_default_value().get_string().length() != expected_default_length))) {
      ret = OB_ERR_UNEXPECTED;
    }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_FULL namespace=%lu table=%llu ret=%d\n", namespace_id, table_id, ret);
  }
  if (ret == OB_SUCCESS && name.find("_plain") != std::string::npos) {
    // A catalog attached to a native owner must see its uncommitted metadata;
    // an independent operation must not. Materialize this child's history
    // tablet first, without loading any Namespace service.
    ObArenaAllocator rows_allocator(ObMemAttr("CatalogTxProbe"));
    ObTableSchema fixed(&rows_allocator);
    ObNewRow changed;
    observer::NativeSchemaCatalog::Cursor rows;
    ObObj id; id.set_int(table_id);
    ret = catalog.open_prefix(OB_ALL_TABLE_HISTORY_TID, ObRowkey(&id, 1), rows, true);
    const ObNewRow *row = nullptr;
    if (ret == OB_SUCCESS) { ret = fixed.assign(*rows.schema()); }
    if (ret == OB_SUCCESS) { ret = rows.next(row); }
    if (ret == OB_SUCCESS) { ret = ob_write_row(rows_allocator, *row, changed); }
    const int rows_end = rows.close();
    if (ret == OB_SUCCESS) { ret = rows_end; }
    ns::TabletAccess target;
    const char *stage = "materialize";
    if (ret == OB_SUCCESS) {
      ret = target.prepare_write(namespace_id, OB_ALL_TABLE_HISTORY_TID, fixed.get_tablet_id(),
          data_plane::ObNamespaceAccessMode::LEASED);
    }
    storage::NativeInnerTable native(*access, *transactions);
    storage::NativeInnerTable::Table table;
    storage::NativeInnerTable::Transaction owner;
    observer::NativeSchemaCatalog borrowed(namespace_id, *access, *transactions);
    const int64_t deadline = ObTimeUtility::current_time() + 30000000;
    int64_t name_index = -1;
    if (ret == OB_SUCCESS) { ret = native.bind(table, target.tablet(), fixed); }
    if (ret == OB_SUCCESS) { ret = column_index(fixed, "table_name", name_index); }
    if (ret == OB_SUCCESS) {
      changed.cells_[name_index].set_string(changed.cells_[name_index].get_type(),
          ObString::make_string("tx_visible"));
      ret = native.begin(owner, deadline);
    }
    if (ret == OB_SUCCESS) { stage = "write"; ret = native.update(owner, table, changed); }
    if (ret == OB_SUCCESS) {
      stage = "borrow";
      ret = native.with_native_transaction(owner, [&](transaction::ObTxDesc &descriptor) {
        return borrowed.attach(descriptor, deadline);
      });
    }
    auto verify_name = [&](observer::NativeSchemaCatalog &reader, const char *expected) {
      ObArenaAllocator allocator(ObMemAttr("CatalogTxRead"));
      ObTableSchema *definition = nullptr;
      int rc = reader.get_table(table_id, INT64_MAX - 1, allocator, definition);
      if (rc == OB_SUCCESS && definition->get_table_name_str() != ObString::make_string(expected)) {
        rc = OB_ERR_UNEXPECTED;
      }
      return rc;
    };
    if (ret == OB_SUCCESS) { stage = "read_own"; ret = verify_name(borrowed, "tx_visible"); }
    if (ret == OB_SUCCESS) {
      stage = "read_external";
      observer::NativeSchemaCatalog outside(namespace_id, *access, *transactions);
      ret = outside.begin_read(deadline);
      if (ret == OB_SUCCESS) { ret = verify_name(outside, "t"); }
    }
    const int owner_end = owner.is_active() ? native.rollback(owner) : OB_SUCCESS;
    const int participant_end = borrowed.close();
    if (ret == OB_SUCCESS) { ret = owner_end == OB_SUCCESS ? participant_end : owner_end; }
    if (ret == OB_SUCCESS) {
      stage = "read_rollback";
      observer::NativeSchemaCatalog outside(namespace_id, *access, *transactions);
      ret = outside.begin_read(deadline);
      if (ret == OB_SUCCESS) { ret = verify_name(outside, "t"); }
    }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_BORROWED namespace=%lu stage=%s ret=%d\n", namespace_id, stage, ret);
  }
  if (ret == OB_SUCCESS && expected_default_length > 0) {
    // Exercise the fixed history layout directly on its resolved physical
    // source. The synthetic table ID is never published as a logical schema.
    ObArenaAllocator allocator(ObMemAttr("CatalogLobProbe"));
    ObTableSchema fixed(&allocator);
    ObNewRow sample;
    int64_t value_index = -1;
    observer::NativeSchemaCatalog::Cursor rows;
    ObObj id; id.set_int(table_id);
    ret = catalog.open_prefix(OB_ALL_TABLE_HISTORY_TID, ObRowkey(&id, 1), rows);
    if (ret == OB_SUCCESS) { ret = fixed.assign(*rows.schema()); }
    const ObNewRow *row = nullptr;
    if (ret == OB_SUCCESS) { ret = rows.next(row); }
    if (ret == OB_SUCCESS) { ret = ob_write_row(allocator, *row, sample); }
    const int cursor_end = rows.close();
    if (ret == OB_SUCCESS) { ret = cursor_end; }
    if (ret == OB_SUCCESS) { ret = column_index(fixed, "view_definition", value_index); }
    ns::TabletAccess source;
    if (ret == OB_SUCCESS) {
      ret = source.prepare_read(namespace_id, OB_ALL_TABLE_HISTORY_TID, fixed.get_tablet_id(),
          data_plane::ObNamespaceAccessMode::LEASED, catalog.view());
    }
    storage::NativeInnerTable native(*access, *transactions);
    storage::NativeInnerTable::Table table;
    storage::NativeInnerTable::Transaction tx;
    if (ret == OB_SUCCESS) { ret = native.bind(table, source.tablet(), fixed); }
    const int64_t deadline = ObTimeUtility::current_time() + 30000000;
    std::string first(90000, 'a'), second(120000, 'b');
    if (ret == OB_SUCCESS) {
      sample.cells_[0].set_int(5000000000LL + namespace_id);
      sample.cells_[value_index].set_meta_type(fixed.get_column_schema(ObString::make_string("view_definition"))->get_meta_type());
      if (!sample.cells_[value_index].is_lob_storage()) { ret = OB_ERR_UNEXPECTED; }
      sample.cells_[value_index].set_string(sample.cells_[value_index].get_type(),
          ObString(first.size(), first.data()));
      sample.cells_[value_index].set_inrow();
    }
    const ObRowkey key(sample.cells_, fixed.get_rowkey_column_num());
    auto verify = [&](const std::string *expected) {
      ObArenaAllocator output(ObMemAttr("CatalogLobRead"));
      ObNewRow found;
      int rc = native.get(tx, table, key, output, found);
      if (expected == nullptr) { return rc == OB_ENTRY_NOT_EXIST ? OB_SUCCESS : OB_ERR_UNEXPECTED; }
      if (rc == OB_SUCCESS && found.cells_[value_index].get_string()
          != ObString(expected->size(), expected->data())) { rc = OB_ERR_UNEXPECTED; }
      return rc;
    };
    const char *stage = "insert";
    if (ret == OB_SUCCESS) { ret = native.begin(tx, deadline); }
    if (ret == OB_SUCCESS) { ret = native.insert(tx, table, sample); }
    stage = "read_own_insert";
    if (ret == OB_SUCCESS) { ret = verify(&first); }
    if (ret == OB_SUCCESS) {
      sample.cells_[value_index].set_string(sample.cells_[value_index].get_type(),
          ObString(second.size(), second.data()));
      stage = "update";
      ret = native.update(tx, table, sample);
    }
    if (ret == OB_SUCCESS) { stage = "read_own_update"; ret = verify(&second); }
    const int rollback = tx.is_active() ? native.rollback(tx) : OB_SUCCESS;
    if (ret == OB_SUCCESS) { ret = rollback; }
    if (ret == OB_SUCCESS) { ret = native.begin(tx, deadline); }
    if (ret == OB_SUCCESS) { stage = "rollback"; ret = verify(nullptr); }
    if (ret == OB_SUCCESS) {
      sample.cells_[value_index].set_string(sample.cells_[value_index].get_type(),
          ObString(first.size(), first.data()));
      stage = "insert_after_rollback";
      // rollback_tx can return before its old row locks finish cleanout. Retry
      // the complete new owner, including its LOB side writes, on that normal
      // storage conflict; never retry half a LOB statement in the same tx.
      do {
        ret = native.insert(tx, table, sample);
        if (ret != OB_TRY_LOCK_ROW_CONFLICT || ObTimeUtility::current_time() >= deadline) { break; }
        ret = native.rollback(tx);
        if (ret != OB_SUCCESS) { break; }
        ob_usleep(1000);
        ret = native.begin(tx, deadline);
      } while (ret == OB_SUCCESS);
    }
    if (ret == OB_SUCCESS) { stage = "commit"; ret = native.commit(tx); }
    if (ret == OB_SUCCESS) { ret = native.begin(tx, deadline); }
    if (ret == OB_SUCCESS) { stage = "read_committed"; ret = verify(&first); }
    bool existed = false;
    if (ret == OB_SUCCESS) { stage = "erase"; ret = native.erase(tx, table, key, existed); }
    if (ret == OB_SUCCESS && !existed) { ret = OB_ERR_UNEXPECTED; }
    if (ret == OB_SUCCESS) { stage = "read_own_erase"; ret = verify(nullptr); }
    if (ret == OB_SUCCESS) { ret = native.rollback(tx); }
    if (ret == OB_SUCCESS) { ret = native.begin(tx, deadline); }
    if (ret == OB_SUCCESS) { stage = "rollback_erase"; ret = verify(&first); }
    if (ret == OB_SUCCESS) { ret = native.erase(tx, table, key, existed); }
    if (ret == OB_SUCCESS) { ret = native.commit(tx); }
    if (ret == OB_SUCCESS) { ret = native.begin(tx, deadline, true); }
    if (ret == OB_SUCCESS) { stage = "erase_committed"; ret = verify(nullptr); }
    if (tx.is_active()) {
      const int end = native.rollback(tx);
      if (ret == OB_SUCCESS) { ret = end; }
    }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_LOB_DML namespace=%lu stage=%s ret=%d\n", namespace_id, stage, ret);
  }
  if (ret == OB_SUCCESS) { ret = check_cold(); }
  const int close = catalog.close();
  if (ret == OB_SUCCESS) { ret = close; }
  fprintf(stderr, "NATIVE_SCHEMA_CATALOG_COLD namespace=%lu table=%llu table_rows=%ld column_rows=%ld runtime_slots=0 ret=%d\n",
      namespace_id, table_id, table_rows, column_rows, ret);
  return ret;
}
