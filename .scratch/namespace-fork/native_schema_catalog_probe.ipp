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
  const bool old_mock_fk = name.find("_old_mock_fk") != std::string::npos;
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
  ObArray<int64_t> expected_versions;
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
      if (rc == OB_SUCCESS && pass == 0) {
        int64_t version_index = -1;
        rc = column_index(schema, "schema_version", version_index);
        if (rc == OB_SUCCESS && row.cells_[version_index].get_int() <= requested_version) {
          rc = expected_versions.push_back(row.cells_[version_index].get_int());
        }
      }
      if (rc == OB_SUCCESS) { ++(pass == 0 ? table_rows : column_rows); }
      return rc;
    });
  }
  if (ret == OB_SUCCESS && (table_rows < 1 || column_rows < expected_columns)) { ret = OB_ERR_UNEXPECTED; }
  if (ret == OB_SUCCESS && name.find("_plain") != std::string::npos) {
    ObArray<ObTableSchema> core;
    ret = catalog.get_core_tables(INT64_MAX, core);
    bool found_table = false, found_column = false;
    int64_t columns = 0;
    for (int64_t i = 0; ret == OB_SUCCESS && i < core.count(); ++i) {
      const auto &actual = core.at(i);
      ObTableSchema expected;
      const auto creator = share::fixed_inner_table_creator(actual.get_table_id());
      if (creator == nullptr) {
        fprintf(stderr, "NATIVE_SCHEMA_CATALOG_CORE_MISSING_CREATOR table=%lu\n", actual.get_table_id());
        ret = OB_ERR_UNEXPECTED;
      }
      else { ret = creator(expected); }
      if (ret == OB_SUCCESS && (!actual.is_valid()
          || actual.get_table_name_str() != expected.get_table_name_str()
          || actual.get_column_count() != expected.get_column_count()
          || actual.get_rowkey_column_num() != expected.get_rowkey_column_num()
          || actual.get_tablet_id() != expected.get_tablet_id())) {
        fprintf(stderr, "NATIVE_SCHEMA_CATALOG_CORE_DIFF table=%lu valid=%d name=%.*s/%.*s columns=%ld/%ld keys=%ld/%ld tablet=%lu/%lu\n",
            actual.get_table_id(), actual.is_valid(), actual.get_table_name_str().length(),
            actual.get_table_name_str().ptr(), expected.get_table_name_str().length(), expected.get_table_name_str().ptr(),
            actual.get_column_count(), expected.get_column_count(), actual.get_rowkey_column_num(),
            expected.get_rowkey_column_num(), actual.get_tablet_id().id(), expected.get_tablet_id().id());
        ret = OB_ERR_UNEXPECTED;
      }
      for (auto column = actual.column_begin(); ret == OB_SUCCESS && column != actual.column_end(); ++column) {
        const auto *fixed = expected.get_column_schema((*column)->get_column_id());
        if (fixed == nullptr || fixed->get_column_name_str() != (*column)->get_column_name_str()
            || fixed->get_data_type() != (*column)->get_data_type()
            || fixed->get_rowkey_position() != (*column)->get_rowkey_position()) {
          fprintf(stderr, "NATIVE_SCHEMA_CATALOG_CORE_COLUMN_DIFF table=%lu column=%lu name=%.*s type=%d key=%ld\n",
              actual.get_table_id(), (*column)->get_column_id(), (*column)->get_column_name_str().length(),
              (*column)->get_column_name_str().ptr(), (*column)->get_data_type(), (*column)->get_rowkey_position());
          ret = OB_ERR_UNEXPECTED;
        }
      }
      found_table |= actual.get_table_id() == OB_ALL_TABLE_TID;
      found_column |= actual.get_table_id() == OB_ALL_COLUMN_TID;
      columns += actual.get_column_count();
      uint64_t found = OB_INVALID_ID;
      ObTableType type = MAX_TABLE_TYPE;
      int64_t version = 0;
      if (ret == OB_SUCCESS) { ret = catalog.get_table_id(actual.get_database_id(), 0, actual.get_table_name_str(), true, found, type, version); }
      const uint64_t expected_id = actual.is_index_table() || actual.is_aux_lob_table() ? OB_INVALID_ID : actual.get_table_id();
      if (ret == OB_SUCCESS && found != expected_id) { ret = OB_ERR_UNEXPECTED; }
      if (ret == OB_SUCCESS && actual.is_index_table()) {
        ret = catalog.get_index_id(actual.get_database_id(), actual.get_table_name_str(), found);
        if (ret == OB_SUCCESS && found != actual.get_table_id()) { ret = OB_ERR_UNEXPECTED; }
      }
    }
    if (ret == OB_SUCCESS && (!found_table || !found_column)) { ret = OB_ERR_UNEXPECTED; }
    ObArenaAllocator allocator(ObMemAttr("CoreCatalogTest"));
    ObArray<uint64_t> ids;
    ObArray<ObSchemaIdVersion> versions;
    ObSchemaIdVersion sentinel;
    if (ret == OB_SUCCESS) { ret = sentinel.init(999999999, 1); }
    if (ret == OB_SUCCESS) { ret = versions.push_back(sentinel); }
    for (int64_t i = 0; ret == OB_SUCCESS && i < core.count(); ++i) { ret = ids.push_back(core.at(i).get_table_id()); }
    if (ret == OB_SUCCESS) { ret = ids.push_back(OB_ALL_TABLE_TID); }
    if (ret == OB_SUCCESS) { ret = ids.push_back(1000000000); }
    if (ret == OB_SUCCESS) { ret = catalog.get_table_schema_versions(ids, versions); }
    if (ret == OB_SUCCESS && (versions.count() != core.count() + 1
        || versions.at(0).get_schema_id() != sentinel.get_schema_id())) { ret = OB_ERR_UNEXPECTED; }
    for (int64_t i = 0; ret == OB_SUCCESS && i < core.count(); ++i) {
      if (versions.at(i + 1).get_schema_id() != core.at(i).get_table_id()
          || versions.at(i + 1).get_schema_version() != core.at(i).get_schema_version()) { ret = OB_ERR_UNEXPECTED; }
    }
    ObArray<ObIndexSchemaInfo> indexes;
    if (ret == OB_SUCCESS) { ret = catalog.get_table_index_infos(allocator, OB_SYS_DATABASE_ID, OB_ALL_TABLE_TID, indexes); }
    int64_t expected_indexes = 0;
    for (int64_t i = 0; ret == OB_SUCCESS && i < core.count(); ++i) {
      const auto &table = core.at(i);
      if (table.get_data_table_id() != OB_ALL_TABLE_TID || table.get_table_type() != USER_INDEX) { continue; }
      bool found = false;
      for (int64_t j = 0; j < indexes.count(); ++j) {
        const auto &index = indexes.at(j);
        if (index.get_index_id() == table.get_table_id()) {
          found = index.get_index_name() == table.get_table_name_str()
              && index.get_schema_version() == table.get_schema_version() && index.get_index_type() == table.get_index_type();
        }
      }
      if (!found) { ret = OB_ERR_UNEXPECTED; }
      ++expected_indexes;
    }
    if (ret == OB_SUCCESS && indexes.count() != expected_indexes) { ret = OB_ERR_UNEXPECTED; }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_CORE_METADATA namespace=%lu indexes=%ld ret=%d\n", namespace_id, indexes.count(), ret);
    uint64_t self_id = OB_INVALID_ID;
    ObTableType self_type = MAX_TABLE_TYPE;
    int64_t self_version = 0;
    if (ret == OB_SUCCESS) { ret = catalog.get_table_id(OB_SYS_DATABASE_ID, 0, ObString::make_string(OB_ALL_CORE_TABLE_TNAME), true, self_id, self_type, self_version); }
    ObSEArray<uint64_t, 1> self_ids;
    ObArray<ObSchemaIdVersion> self_versions;
    if (ret == OB_SUCCESS) { ret = self_ids.push_back(OB_ALL_CORE_TABLE_TID); }
    if (ret == OB_SUCCESS) { ret = catalog.get_table_schema_versions(self_ids, self_versions); }
    if (ret == OB_SUCCESS && (self_versions.count() != 1 || self_versions.at(0).get_schema_version() != self_version)) { ret = OB_ERR_UNEXPECTED; }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_CORE_SELF namespace=%lu id=%lu version=%ld ret=%d\n", namespace_id, self_id, self_version, ret);
    for (const uint64_t id : {OB_ALL_CORE_TABLE_TID, OB_ALL_TABLE_TID, OB_ALL_COLUMN_TID}) {
      if (ret != OB_SUCCESS) { break; }
      ObTableSchema *table = nullptr;
      ret = catalog.get_table(id, INT64_MAX, allocator, table);
      if (ret == OB_SUCCESS && (table == nullptr || !table->is_valid())) { ret = OB_ERR_UNEXPECTED; }
      if (table != nullptr) { table->~ObTableSchema(); }
    }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_CORE namespace=%lu tables=%ld columns=%ld ret=%d\n",
        namespace_id, core.count(), columns, ret);
  }
  if (ret == OB_SUCCESS && name.find("_plain") != std::string::npos) {
    // Core row_id is an allocation identity, not schema version order. Insert
    // versions in the opposite order and a tombstone, all rolled back below.
    ObArenaAllocator allocator(ObMemAttr("CoreHistoryTest"));
    ObArray<ObNewRow> sample;
    ObTableSchema fixed;
    ret = share::ObInnerTableSchema::all_core_table_schema(fixed);
    int64_t value_column = -1;
    if (ret == OB_SUCCESS) { ret = column_index(fixed, "column_value", value_column); }
    ObObj keys[2];
    keys[0].set_varchar(OB_ALL_TABLE_HISTORY_TNAME);
    keys[0].set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    int64_t selected_row = -1, max_row = -1, original_version = -1;
    const char *stage = "find_source";
    {
      observer::NativeSchemaCatalog::Cursor cursor;
      if (ret == OB_SUCCESS) { ret = catalog.open_prefix(OB_ALL_CORE_TABLE_TID, ObRowkey(keys, 1), cursor); }
      const ObNewRow *row = nullptr;
      int64_t current_row = -1, current_version = -1, current_table = -1;
      auto consume = [&]() {
        if (current_table == OB_ALL_TABLE_TID && current_version > original_version) {
          selected_row = current_row; original_version = current_version;
        }
      };
      while (ret == OB_SUCCESS && (ret = cursor.next(row)) == OB_SUCCESS) {
        const int64_t row_id = row->cells_[1].get_int();
        if (row_id != current_row) {
          consume(); current_row = row_id; current_version = -1; current_table = -1;
        }
        max_row = std::max(max_row, row_id);
        const ObString field = row->cells_[2].get_string();
        if (field == ObString::make_string("table_id") || field == ObString::make_string("schema_version")) {
          const ObString bytes = row->cells_[value_column].get_string();
          const std::string number(bytes.ptr(), bytes.length());
          const int64_t value = strtoll(number.c_str(), nullptr, 10);
          (field == ObString::make_string("table_id") ? current_table : current_version) = value;
        }
      }
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; consume(); }
    }
    if (ret == OB_SUCCESS && (selected_row < 0 || original_version < 0)) { ret = OB_ERR_UNEXPECTED; }
    {
      observer::NativeSchemaCatalog::Cursor cursor;
      keys[1].set_int(selected_row);
      if (ret == OB_SUCCESS) { ret = catalog.open_prefix(OB_ALL_CORE_TABLE_TID, ObRowkey(keys, 2), cursor); }
      const ObNewRow *row = nullptr;
      while (ret == OB_SUCCESS && (ret = cursor.next(row)) == OB_SUCCESS) {
        ObNewRow copy;
        if ((ret = ob_write_row(allocator, *row, copy)) == OB_SUCCESS) { ret = sample.push_back(copy); }
      }
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    }
    ns::TabletAccess target;
    stage = "materialize";
    if (ret == OB_SUCCESS) {
      ret = target.prepare_write(namespace_id, OB_ALL_CORE_TABLE_TID, fixed.get_tablet_id(),
                                 data_plane::ObNamespaceAccessMode::LEASED);
    }
    storage::NativeInnerTable native(*access, *transactions);
    storage::NativeInnerTable::Transaction owner;
    observer::NativeSchemaCatalog borrowed(namespace_id, *access, *transactions);
    const int64_t deadline = ObTimeUtility::current_time() + 30000000;
    if (ret == OB_SUCCESS) { ret = native.begin(owner, deadline); }
    if (ret == OB_SUCCESS) {
      ret = native.with_native_transaction(owner, [&](transaction::ObTxDesc &descriptor) {
        return borrowed.attach(descriptor, deadline);
      });
    }
    auto append = [&](int64_t row_id, int64_t version, const char *table_name, bool deleted) {
      int rc = OB_SUCCESS;
      char version_text[32];
      snprintf(version_text, sizeof(version_text), "%ld", version);
      for (int64_t i = 0; rc == OB_SUCCESS && i < sample.count(); ++i) {
        ObNewRow copy;
        rc = ob_write_row(allocator, sample.at(i), copy);
        if (rc != OB_SUCCESS) { break; }
        copy.cells_[1].set_int(row_id);
        const ObString field = copy.cells_[2].get_string();
        const char *value = field == ObString::make_string("schema_version") ? version_text
            : field == ObString::make_string("table_name") ? table_name
            : field == ObString::make_string("is_deleted") ? (deleted ? "1" : "0") : nullptr;
        if (value != nullptr) { copy.cells_[value_column].set_string(ObVarcharType, ObString::make_string(value)); }
        rc = borrowed.insert(OB_ALL_CORE_TABLE_TID, copy);
      }
      return rc;
    };
    auto verify = [&](observer::NativeSchemaCatalog &reader, int64_t version, const char *expected) {
      ObTableSchema *definition = nullptr;
      int rc = reader.get_table(OB_ALL_TABLE_TID, version, allocator, definition);
      if (expected == nullptr) { rc = rc == OB_TABLE_NOT_EXIST ? OB_SUCCESS : OB_ERR_UNEXPECTED; }
      else if (rc == OB_SUCCESS && definition->get_table_name_str() != ObString::make_string(expected)) {
        rc = OB_ERR_UNEXPECTED;
      }
      if (definition != nullptr) { definition->~ObTableSchema(); }
      return rc;
    };
    if (ret == OB_SUCCESS) { stage = "append_new"; ret = append(max_row + 1, original_version + 2, "__native_core_new", false); }
    if (ret == OB_SUCCESS) { stage = "append_old"; ret = append(max_row + 2, original_version + 1, "__native_core_old", false); }
    if (ret == OB_SUCCESS) { stage = "read_latest"; ret = verify(borrowed, INT64_MAX, "__native_core_new"); }
    if (ret == OB_SUCCESS) { stage = "read_history"; ret = verify(borrowed, original_version + 1, "__native_core_old"); }
    if (ret == OB_SUCCESS) {
      stage = "read_external";
      observer::NativeSchemaCatalog outside(namespace_id, *access, *transactions);
      ret = outside.begin_read(deadline);
      if (ret == OB_SUCCESS) { ret = verify(outside, INT64_MAX, OB_ALL_TABLE_TNAME); }
    }
    if (ret == OB_SUCCESS) { stage = "append_deleted"; ret = append(max_row + 3, original_version + 3, "__native_core_deleted", true); }
    if (ret == OB_SUCCESS) { stage = "read_deleted"; ret = verify(borrowed, INT64_MAX, nullptr); }
    if (ret == OB_SUCCESS) { stage = "read_before_deleted"; ret = verify(borrowed, original_version + 2, "__native_core_new"); }
    const int owner_end = owner.is_active() ? native.rollback(owner) : OB_SUCCESS;
    const int participant_end = borrowed.close();
    if (ret == OB_SUCCESS) { ret = owner_end == OB_SUCCESS ? participant_end : owner_end; }
    if (ret == OB_SUCCESS) {
      stage = "read_rollback";
      observer::NativeSchemaCatalog outside(namespace_id, *access, *transactions);
      ret = outside.begin_read(deadline);
      if (ret == OB_SUCCESS) { ret = verify(outside, INT64_MAX, OB_ALL_TABLE_TNAME); }
    }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_CORE_HISTORY namespace=%lu stage=%s ret=%d\n", namespace_id, stage, ret);
  }
  if (ret == OB_SUCCESS) {
    VersionHisVal versions;
    ret = catalog.get_table_version_history(table_id, requested_version, versions);
    const int64_t expected_count = std::min<int64_t>(MAX_CACHED_VERSION_CNT, expected_versions.count());
    if (ret == OB_SUCCESS && (versions.valid_cnt_ != expected_count || versions.is_deleted_
        || versions.snapshot_version_ != requested_version
        || versions.min_version_ != expected_versions.at(0))) { ret = OB_ERR_UNEXPECTED; }
    for (int64_t i = 0; ret == OB_SUCCESS && i < expected_count; ++i) {
      if (versions.versions_[i] != expected_versions.at(expected_versions.count() - 1 - i)) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    ObSEArray<uint64_t, 3> ids;
    ObArray<ObTableLatestSchemaVersion> latest;
    if (ret == OB_SUCCESS) { ret = ids.push_back(table_id); }
    if (ret == OB_SUCCESS) { ret = ids.push_back(table_id); }
    if (ret == OB_SUCCESS) { ret = ids.push_back(1000000000); }
    if (ret == OB_SUCCESS) { ret = catalog.get_table_latest_schema_versions(ids, latest); }
    if (ret == OB_SUCCESS && (latest.count() != 1 || latest.at(0).get_table_id() != table_id
        || latest.at(0).is_deleted() || latest.at(0).get_schema_version() < versions.versions_[0])) {
      ret = OB_ERR_UNEXPECTED;
    }
    int64_t original = 0;
    if (ret == OB_SUCCESS) { ret = catalog.get_original_table_version(table_id, original); }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_VERSIONS namespace=%lu count=%d min=%ld latest=%ld original=%ld ret=%d\n",
        namespace_id, versions.valid_cnt_, versions.min_version_,
        latest.empty() ? 0 : latest.at(0).get_schema_version(), original, ret);
    if (ret == OB_SUCCESS) { ret = catalog.get_table_version_history(1000000000, INT64_MAX, versions); }
    if (ret == OB_SUCCESS && (versions.valid_cnt_ != 0 || versions.min_version_ != OB_INVALID_VERSION
        || versions.is_deleted_ || catalog.get_original_table_version(1000000000, original) != OB_ITER_END)) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
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
    if (ret == OB_SUCCESS && expected_foreign_keys > 0 && name.find("_mock_fk") == std::string::npos) {
      const auto &fk = definition->get_foreign_key_infos().at(0);
      ObTableSchema *parent = nullptr;
      ret = catalog.get_table(fk.parent_table_id_, requested_version, allocator, parent);
      if (ret == OB_SUCCESS && (parent->get_foreign_key_infos().count() != 1
          || parent->get_foreign_key_infos().at(0).foreign_key_id_ != fk.foreign_key_id_)) {
        ret = OB_ERR_UNEXPECTED;
      }
      if (parent != nullptr) { parent->~ObTableSchema(); }
    }
    if (ret == OB_SUCCESS && name.find("_mock_fk") != std::string::npos) {
      const auto &fk = definition->get_foreign_key_infos().at(0);
      ObArray<uint64_t> ids;
      ObArray<ObMockFKParentTableSchema> full;
      ObArray<ObSimpleMockFKParentTableSchema> simple;
      ret = ids.push_back(fk.parent_table_id_);
      if (ret == OB_SUCCESS) { ret = ids.push_back(fk.parent_table_id_); }
      if (ret == OB_SUCCESS) { ret = ids.push_back(1000000000); }
      if (ret == OB_SUCCESS) { ret = catalog.get_mock_fk_parent_tables(requested_version, full, &ids); }
      if (ret == OB_SUCCESS && (full.count() != 1 || full.at(0).get_table_id() != fk.parent_table_id_
          || full.at(0).get_database_id() != definition->get_database_id()
          || full.at(0).get_table_name_str() != ObString::make_string("missing_parent")
          || full.at(0).get_foreign_key_infos().count() != 1
          || full.at(0).get_foreign_key_infos().at(0).foreign_key_id_ != fk.foreign_key_id_
          || full.at(0).get_column_array().count() != fk.parent_column_ids_.count())) { ret = OB_ERR_UNEXPECTED; }
      int64_t mock_version = 0;
      if (ret == OB_SUCCESS) { mock_version = full.at(0).get_schema_version(); }
      for (int64_t i = 0; ret == OB_SUCCESS && i < full.at(0).get_column_array().count(); ++i) {
        const auto &column = full.at(0).get_column_array().at(i);
        if (i > 0 && full.at(0).get_column_array().at(i - 1).first >= column.first) { ret = OB_ERR_UNEXPECTED; }
        fprintf(stderr, "NATIVE_SCHEMA_CATALOG_MOCK_COLUMN namespace=%lu id=%lu name=%.*s\n",
            namespace_id, column.first, column.second.length(), column.second.ptr());
      }
      for (int64_t i = 0; ret == OB_SUCCESS && i < fk.parent_column_ids_.count(); ++i) {
        ObString column;
        bool found = false;
        full.at(0).get_column_name_by_column_id(fk.parent_column_ids_.at(i), column, found);
        const char *expected = fk.parent_column_ids_.count() == 1 ? "id" : i == 0 ? "b" : "a";
        const auto &parent_fk = full.at(0).get_foreign_key_infos().at(0);
        if (!found || column != ObString::make_string(expected)
            || parent_fk.parent_column_ids_.count() != fk.parent_column_ids_.count()
            || parent_fk.parent_column_ids_.at(i) != fk.parent_column_ids_.at(i)
            || parent_fk.child_column_ids_.at(i) != fk.child_column_ids_.at(i)) { ret = OB_ERR_UNEXPECTED; }
      }
      if (ret == OB_SUCCESS) { ret = catalog.get_simple_mock_fk_parent_tables(requested_version, simple, &ids); }
      if (ret == OB_SUCCESS && (simple.count() != 1 || simple.at(0).get_table_id() != fk.parent_table_id_
          || simple.at(0).get_schema_version() != mock_version)) { ret = OB_ERR_UNEXPECTED; }
      simple.reset();
      if (ret == OB_SUCCESS) { ret = catalog.get_simple_mock_fk_parent_tables(requested_version, simple); }
      bool found = false;
      for (int64_t i = 0; ret == OB_SUCCESS && i < simple.count(); ++i) {
        if (simple.at(i).get_table_id() == fk.parent_table_id_) { found = true; }
      }
      if (ret == OB_SUCCESS && !found) { ret = OB_ERR_UNEXPECTED; }
      fprintf(stderr, "NATIVE_SCHEMA_CATALOG_MOCK_FULL namespace=%lu id=%lu version=%ld columns=%ld ret=%d\n",
          namespace_id, fk.parent_table_id_, mock_version, full.empty() ? 0 : full.at(0).get_column_array().count(), ret);
      full.reset(); simple.reset();
      if (ret == OB_SUCCESS && old_mock_fk) {
        ret = catalog.get_mock_fk_parent_tables(INT64_MAX, full, &ids);
        if (ret == OB_SUCCESS) { ret = catalog.get_simple_mock_fk_parent_tables(INT64_MAX, simple, &ids); }
        if (ret == OB_SUCCESS && (!full.empty() || !simple.empty())) { ret = OB_ERR_UNEXPECTED; }
      }
      full.reset(); simple.reset(); ids.reset();
      if (ret == OB_SUCCESS) { ret = catalog.get_mock_fk_parent_tables(requested_version, full, &ids); }
      if (ret == OB_SUCCESS) { ret = catalog.get_simple_mock_fk_parent_tables(requested_version, simple, &ids); }
      if (ret == OB_SUCCESS && (!full.empty() || !simple.empty())) { ret = OB_ERR_UNEXPECTED; }
    }
    if (ret == OB_SUCCESS) {
      ObArray<ObAuxTableMetaInfo> auxiliary;
      ret = catalog.get_aux_tables(table_id, requested_version, auxiliary);
      for (int64_t i = 0; ret == OB_SUCCESS && i < auxiliary.count(); ++i) {
        const auto &aux = auxiliary.at(i);
        fprintf(stderr, "NATIVE_SCHEMA_CATALOG_AUX_ROW namespace=%lu id=%lu type=%d index_type=%d\n",
            namespace_id, aux.table_id_, aux.table_type_, aux.index_type_);
      }
      fprintf(stderr, "NATIVE_SCHEMA_CATALOG_AUX namespace=%lu count=%ld ret=%d\n", namespace_id, auxiliary.count(), ret);
    }
    if (definition != nullptr) { definition->~ObTableSchema(); definition = nullptr; }
    if (ret == OB_SUCCESS) {
      ret = catalog.get_table(table_id, INT64_MAX, allocator, definition,
          observer::NativeSchemaCatalog::TableSource::CURRENT);
      const int64_t current_columns = expected_columns
          + (name.find("_history") != std::string::npos ? 1 : 0);
      const int64_t current_fks = name.find("_old_fk") != std::string::npos || old_mock_fk ? 0 : expected_foreign_keys;
      if (ret == OB_SUCCESS && (definition->get_column_count() != current_columns
          || definition->get_partition_num() != expected_parts
          || definition->get_index_tid_count() != expected_indexes
          || definition->get_constraint_count() != expected_constraints
          || definition->get_foreign_key_infos().count() != current_fks)) {
        ret = OB_ERR_UNEXPECTED;
      }
      fprintf(stderr, "NATIVE_SCHEMA_CATALOG_CURRENT namespace=%lu table=%llu ret=%d\n",
          namespace_id, table_id, ret);
      ObSEArray<uint64_t, 3> ids;
      ObArray<ObSchemaIdVersion> versions;
      if (ret == OB_SUCCESS) { ret = ids.push_back(table_id); }
      if (ret == OB_SUCCESS) { ret = ids.push_back(table_id); }
      if (ret == OB_SUCCESS) { ret = ids.push_back(1000000000); }
      if (ret == OB_SUCCESS) { ret = catalog.get_table_schema_versions(ids, versions); }
      if (ret == OB_SUCCESS && (versions.count() != 1 || versions.at(0).get_schema_id() != table_id
          || versions.at(0).get_schema_version() != definition->get_schema_version())) { ret = OB_ERR_UNEXPECTED; }
      ObArray<ObIndexSchemaInfo> indexes;
      if (ret == OB_SUCCESS) { ret = catalog.get_table_index_infos(allocator, definition->get_database_id(), table_id, indexes); }
      if (ret == OB_SUCCESS && indexes.count() != expected_indexes) { ret = OB_ERR_UNEXPECTED; }
      for (int64_t i = 0; ret == OB_SUCCESS && i < indexes.count(); ++i) {
        const auto &index = indexes.at(i);
        uint64_t found = OB_INVALID_ID;
        ret = catalog.get_index_id(definition->get_database_id(), index.get_index_name(), found);
        if (ret == OB_SUCCESS && found != index.get_index_id()) { ret = OB_ERR_UNEXPECTED; }
        ObTableType type = MAX_TABLE_TYPE;
        int64_t version = 0;
        if (ret == OB_SUCCESS) { ret = catalog.get_table_id(definition->get_database_id(), 0, index.get_index_name(), true, found, type, version); }
        if (ret == OB_SUCCESS && found != OB_INVALID_ID) { ret = OB_ERR_UNEXPECTED; }
        fprintf(stderr, "NATIVE_SCHEMA_CATALOG_INDEX_INFO namespace=%lu id=%lu name=%.*s version=%ld type=%d\n",
            namespace_id, index.get_index_id(), index.get_index_name().length(), index.get_index_name().ptr(),
            index.get_schema_version(), index.get_index_type());
      }
      fprintf(stderr, "NATIVE_SCHEMA_CATALOG_CURRENT_METADATA namespace=%lu table=%llu version=%ld indexes=%ld ret=%d\n",
          namespace_id, table_id, versions.empty() ? 0 : versions.at(0).get_schema_version(), indexes.count(), ret);
      if (ret == OB_SUCCESS) { ret = catalog.get_table_index_infos(allocator, definition->get_database_id() + 1000000000, table_id, indexes); }
      if (ret == OB_SUCCESS && !indexes.empty()) { ret = OB_ERR_UNEXPECTED; }
      uint64_t found = OB_INVALID_ID;
      ObTableType type = MAX_TABLE_TYPE;
      int64_t version = 0;
      if (ret == OB_SUCCESS) { ret = catalog.get_table_id(definition->get_database_id(), 0, ObString::make_string("t"), false, found, type, version); }
      if (ret == OB_SUCCESS && (found != table_id || type != definition->get_table_type()
          || version != definition->get_schema_version())) { ret = OB_ERR_UNEXPECTED; }
      if (ret == OB_SUCCESS) { ret = catalog.get_table_id(definition->get_database_id(), 0, ObString::make_string("T"), true, found, type, version); }
      if (ret == OB_SUCCESS && found != table_id) { ret = OB_ERR_UNEXPECTED; }
      if (ret == OB_SUCCESS) { ret = catalog.get_table_id(definition->get_database_id(), 0, ObString::make_string("T"), false, found, type, version); }
      if (ret == OB_SUCCESS && found != OB_INVALID_ID) { ret = OB_ERR_UNEXPECTED; }
      if (ret == OB_SUCCESS) { ret = catalog.get_table_id(definition->get_database_id() + 1000000000, 0, ObString::make_string("t"), true, found, type, version); }
      if (ret == OB_SUCCESS && found != OB_INVALID_ID) { ret = OB_ERR_UNEXPECTED; }
      for (auto it = definition->constraint_begin(); ret == OB_SUCCESS && it != definition->constraint_end(); ++it) {
        ret = catalog.get_constraint_id(definition->get_database_id(), (*it)->get_constraint_name_str(), found);
        if (ret == OB_SUCCESS && found != (*it)->get_constraint_id()) { ret = OB_ERR_UNEXPECTED; }
        if (ret == OB_SUCCESS) { ret = catalog.get_constraint_id(definition->get_database_id(), ObString::make_string("CK"), found); }
        if (ret == OB_SUCCESS && found != (*it)->get_constraint_id()) { ret = OB_ERR_UNEXPECTED; }
      }
      for (int64_t i = 0; i < definition->get_foreign_key_infos().count(); ++i) {
        const auto &fk = definition->get_foreign_key_infos().at(i);
        if (ret != OB_SUCCESS || fk.child_table_id_ != table_id) { continue; }
        ret = catalog.get_foreign_key_id(definition->get_database_id(), fk.foreign_key_name_, found);
        if (ret == OB_SUCCESS && found != fk.foreign_key_id_) { ret = OB_ERR_UNEXPECTED; }
        if (ret == OB_SUCCESS) { ret = catalog.get_foreign_key_id(definition->get_database_id(), ObString::make_string("FK"), found); }
        if (ret == OB_SUCCESS && found != fk.foreign_key_id_) { ret = OB_ERR_UNEXPECTED; }
      }
      if (ret == OB_SUCCESS && (name.find("_drop_fk") != std::string::npos || name.find("_old_fk") != std::string::npos || old_mock_fk)) {
        ret = catalog.get_foreign_key_id(definition->get_database_id(), ObString::make_string("fk"), found);
        if (ret == OB_SUCCESS && found != OB_INVALID_ID) { ret = OB_ERR_UNEXPECTED; }
      }
      if (ret == OB_SUCCESS) { ret = catalog.get_constraint_id(definition->get_database_id(), ObString::make_string("__absent_constraint"), found); }
      if (ret == OB_SUCCESS && found != OB_INVALID_ID) { ret = OB_ERR_UNEXPECTED; }
      if (ret == OB_SUCCESS) { ret = catalog.get_foreign_key_id(definition->get_database_id(), ObString::make_string("__absent_fk"), found); }
      if (ret == OB_SUCCESS && found != OB_INVALID_ID) { ret = OB_ERR_UNEXPECTED; }
      if (ret == OB_SUCCESS && name.find("_mock_fk") != std::string::npos && !old_mock_fk) {
        uint64_t mock_id = OB_INVALID_ID;
        ret = catalog.get_mock_fk_parent_table_id(definition->get_database_id(), ObString::make_string("missing_parent"), mock_id);
        if (ret == OB_SUCCESS && (definition->get_foreign_key_infos().empty()
            || mock_id != definition->get_foreign_key_infos().at(0).parent_table_id_)) { ret = OB_ERR_UNEXPECTED; }
        ObSEArray<uint64_t, 3> ids;
        ObArray<ObSchemaIdVersion> versions;
        if (ret == OB_SUCCESS) { ret = ids.push_back(mock_id); }
        if (ret == OB_SUCCESS) { ret = ids.push_back(mock_id); }
        if (ret == OB_SUCCESS) { ret = ids.push_back(1000000000); }
        if (ret == OB_SUCCESS) { ret = catalog.get_mock_fk_parent_table_schema_versions(ids, versions); }
        if (ret == OB_SUCCESS && (versions.count() != 1 || versions.at(0).get_schema_id() != mock_id)) { ret = OB_ERR_UNEXPECTED; }
        if (ret == OB_SUCCESS) { ret = catalog.get_mock_fk_parent_table_id(definition->get_database_id(), ObString::make_string("MISSING_PARENT"), found); }
        if (ret == OB_SUCCESS && found != OB_INVALID_ID) { ret = OB_ERR_UNEXPECTED; }
        fprintf(stderr, "NATIVE_SCHEMA_CATALOG_MOCK_PARENT namespace=%lu id=%lu version=%ld ret=%d\n",
            namespace_id, mock_id, versions.empty() ? 0 : versions.at(0).get_schema_version(), ret);
      }
      fprintf(stderr, "NATIVE_SCHEMA_CATALOG_NAMES namespace=%lu ret=%d\n", namespace_id, ret);
    }
    if (definition != nullptr) { definition->~ObTableSchema(); }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_FULL namespace=%lu table=%llu ret=%d\n", namespace_id, table_id, ret);
  }
  if (ret == OB_SUCCESS) {
    ObArenaAllocator allocator(ObMemAttr("SimpleCatalog"));
    ObArray<ObSimpleTableSchemaV2 *> simple;
    ObSEArray<uint64_t, 3> ids;
    ret = ids.push_back(table_id);
    if (ret == OB_SUCCESS) { ret = ids.push_back(table_id + 1000000000); }
    if (ret == OB_SUCCESS) { ret = ids.push_back(table_id); }
    auto verify = [&](const ObSimpleTableSchemaV2 *table) {
      return table != nullptr && table->is_valid() && table->get_table_id() == table_id
          && table->get_schema_version() <= requested_version
          && table->get_partition_num() == expected_parts
          && table->get_simple_foreign_key_info_array().count() == expected_foreign_keys
          && table->get_simple_constraint_info_array().count() == expected_constraints
          ? OB_SUCCESS : OB_ERR_UNEXPECTED;
    };
    if (ret == OB_SUCCESS) { ret = catalog.get_simple_tables(requested_version, allocator, simple, &ids); }
    if (ret == OB_SUCCESS) { ret = simple.count() == 1 ? verify(simple.at(0)) : OB_ERR_UNEXPECTED; }
    for (auto *table : simple) { table->~ObSimpleTableSchemaV2(); }
    simple.reset();
    // Full refresh shares the same reduced history and relationships. Exercise
    // it with partitions and a deleted FK visible only at the requested V.
    if (ret == OB_SUCCESS && (name.find("_part") != std::string::npos
        || name.find("_old_fk") != std::string::npos)) {
      ret = catalog.get_simple_tables(requested_version, allocator, simple);
      const ObSimpleTableSchemaV2 *found = nullptr;
      uint64_t previous = OB_INVALID_ID;
      for (auto *table : simple) {
        if (table->get_table_id() >= previous || table->get_table_id() == OB_ALL_CORE_TABLE_TID) {
          ret = OB_ERR_UNEXPECTED;
        }
        previous = table->get_table_id();
        if (table->get_table_id() == table_id) { found = table; }
      }
      if (ret == OB_SUCCESS) { ret = verify(found); }
      for (auto *table : simple) { table->~ObSimpleTableSchemaV2(); }
      simple.reset();
    }
    ids.reuse();
    if (ret == OB_SUCCESS) { ret = catalog.get_simple_tables(requested_version, allocator, simple, &ids); }
    if (ret == OB_SUCCESS && !simple.empty()) { ret = OB_ERR_UNEXPECTED; }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_SIMPLE namespace=%lu table=%llu ret=%d\n",
        namespace_id, table_id, ret);
  }
  if (ret == OB_SUCCESS && name.find("_part") != std::string::npos && expected_indexes == 1) {
    ObArenaAllocator allocator(ObMemAttr("IndexMetadataTx"));
    ObTableSchema *definition = nullptr;
    ret = catalog.get_table(table_id, INT64_MAX, allocator, definition,
        observer::NativeSchemaCatalog::TableSource::CURRENT);
    const uint64_t database_id = definition == nullptr ? OB_INVALID_ID : definition->get_database_id();
    if (definition != nullptr) { definition->~ObTableSchema(); }
    ObArray<ObIndexSchemaInfo> indexes;
    if (ret == OB_SUCCESS) { ret = catalog.get_table_index_infos(allocator, database_id, table_id, indexes); }
    if (ret == OB_SUCCESS && indexes.count() != 1) { ret = OB_ERR_UNEXPECTED; }
    const uint64_t index_id = indexes.empty() ? OB_INVALID_ID : indexes.at(0).get_index_id();
    const int64_t original_version = indexes.empty() ? -1 : indexes.at(0).get_schema_version();
    const ObString original_name = indexes.empty() ? ObString() : indexes.at(0).get_index_name();
    const uint64_t new_owner = table_id + 1000000000;
    ObNewRow changed;
    {
      observer::NativeSchemaCatalog::Cursor rows;
      ObObj key; key.set_int(index_id);
      if (ret == OB_SUCCESS) { ret = catalog.open_prefix(OB_ALL_TABLE_TID, ObRowkey(&key, 1), rows); }
      const ObNewRow *row = nullptr;
      if (ret == OB_SUCCESS) { ret = rows.next(row); }
      if (ret == OB_SUCCESS) { ret = ob_write_row(allocator, *row, changed); }
      for (const char *field : {"data_table_id", "table_name", "schema_version"}) {
        int64_t position = -1;
        if (ret == OB_SUCCESS) { ret = column_index(*rows.schema(), field, position); }
        if (ret != OB_SUCCESS) { break; }
        if (0 == STRCMP(field, "table_name")) {
          changed.cells_[position].set_string(ObVarcharType, ObString::make_string("__native_index_renamed"));
        } else { changed.cells_[position].set_int(0 == STRCMP(field, "data_table_id") ? new_owner : original_version + 1); }
      }
    }
    const int64_t deadline = ObTimeUtility::current_time() + 30000000;
    storage::NativeInnerTable native(*access, *transactions);
    storage::NativeInnerTable::Transaction owner;
    observer::NativeSchemaCatalog writer(namespace_id, *access, *transactions);
    if (ret == OB_SUCCESS) { ret = native.begin(owner, deadline); }
    if (ret == OB_SUCCESS) {
      ret = native.with_native_transaction(owner, [&](transaction::ObTxDesc &tx) { return writer.attach(tx, deadline); });
    }
    auto verify = [&](observer::NativeSchemaCatalog &reader, bool modified) {
      ObArray<ObIndexSchemaInfo> found;
      int rc = reader.get_table_index_infos(allocator, database_id, table_id, found);
      if (rc == OB_SUCCESS && found.count() != (modified ? 0 : 1)) { rc = OB_ERR_UNEXPECTED; }
      if (rc == OB_SUCCESS && !modified && (found.at(0).get_index_id() != index_id
          || found.at(0).get_index_name() != original_name)) { rc = OB_ERR_UNEXPECTED; }
      if (rc == OB_SUCCESS) { rc = reader.get_table_index_infos(allocator, database_id, new_owner, found); }
      if (rc == OB_SUCCESS && found.count() != (modified ? 1 : 0)) { rc = OB_ERR_UNEXPECTED; }
      if (rc == OB_SUCCESS && modified && (found.at(0).get_index_id() != index_id
          || found.at(0).get_index_name() != ObString::make_string("__native_index_renamed")
          || found.at(0).get_schema_version() != original_version + 1)) { rc = OB_ERR_UNEXPECTED; }
      ObArray<ObSchemaIdVersion> versions;
      ObSEArray<uint64_t, 1> ids;
      if (rc == OB_SUCCESS) { rc = ids.push_back(index_id); }
      if (rc == OB_SUCCESS) { rc = reader.get_table_schema_versions(ids, versions); }
      if (rc == OB_SUCCESS && (versions.count() != 1
          || versions.at(0).get_schema_version() != original_version + (modified ? 1 : 0))) { rc = OB_ERR_UNEXPECTED; }
      return rc;
    };
    auto outside = [&]() {
      observer::NativeSchemaCatalog reader(namespace_id, *access, *transactions);
      int rc = reader.begin_read(deadline);
      return rc == OB_SUCCESS ? verify(reader, false) : rc;
    };
    const char *stage = "update";
    if (ret == OB_SUCCESS) { ret = writer.update(OB_ALL_TABLE_TID, changed); }
    if (ret == OB_SUCCESS) { stage = "own_read"; ret = verify(writer, true); }
    if (ret == OB_SUCCESS) { stage = "external_read"; ret = outside(); }
    const int owner_end = owner.is_active() ? native.rollback(owner) : OB_SUCCESS;
    const int participant_end = writer.close();
    if (ret == OB_SUCCESS) { ret = owner_end == OB_SUCCESS ? participant_end : owner_end; }
    if (ret == OB_SUCCESS) { stage = "read_rollback"; ret = outside(); }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_INDEX_METADATA_TX namespace=%lu stage=%s ret=%d\n", namespace_id, stage, ret);
  }
  if (ret == OB_SUCCESS && name.find("_plain") != std::string::npos) {
    ObArenaAllocator allocator(ObMemAttr("CatalogNameTx"));
    ObTableSchema *source = nullptr;
    ret = catalog.get_table(table_id, INT64_MAX, allocator, source, observer::NativeSchemaCatalog::TableSource::CURRENT);
    const uint64_t database = source == nullptr ? OB_INVALID_ID : source->get_database_id();
    const uint64_t first_id = 7000000000ULL + namespace_id * 10;
    const int64_t deadline = ObTimeUtility::current_time() + 30000000;
    storage::NativeInnerTable native(*access, *transactions);
    storage::NativeInnerTable::Transaction owner;
    observer::NativeSchemaCatalog writer(namespace_id, *access, *transactions);
    if (ret == OB_SUCCESS) { ret = native.begin(owner, deadline); }
    if (ret == OB_SUCCESS) {
      ret = native.with_native_transaction(owner, [&](transaction::ObTxDesc &tx) { return writer.attach(tx, deadline); });
    }
    for (int i = 0; ret == OB_SUCCESS && i < 6; ++i) {
      ObTableSchema table;
      ret = table.assign(*source);
      table.set_table_id(first_id + i);
      table.set_session_id(i == 1 ? 71 : i == 2 ? 72 : 0);
      table.set_table_type(i == 1 || i == 2 ? TMP_TABLE : i == 4 ? USER_INDEX : i == 5 ? AUX_LOB_META : USER_TABLE);
      if (i == 3) {
        ObTableMode mode = table.get_table_mode_struct(); mode.state_flag_ |= TABLE_STATE_IS_HIDDEN_MASK;
        table.set_table_mode_struct(mode);
      }
      if (i >= 4) { table.set_data_table_id(table_id); }
      if (i == 4) { table.set_index_type(INDEX_TYPE_NORMAL_LOCAL); }
      if (ret == OB_SUCCESS) { ret = table.set_table_name(i < 3 ? "lookup_name" : i == 3 ? "hidden_name" : i == 4 ? "idx_name" : "lob_name"); }
      if (ret == OB_SUCCESS) { ret = writer.insert_table(table, false); }
    }
    auto verify = [&](observer::NativeSchemaCatalog &reader, bool present) {
      int rc = OB_SUCCESS;
      for (uint64_t session : {0UL, 71UL, 72UL, 73UL}) {
        uint64_t found = OB_INVALID_ID;
        ObTableType type = MAX_TABLE_TYPE;
        int64_t version = 0;
        if (rc == OB_SUCCESS) { rc = reader.get_table_id(database, session, ObString::make_string("LOOKUP_NAME"), true, found, type, version); }
        const uint64_t expected = !present ? OB_INVALID_ID : first_id + (session == 71 ? 1 : session == 72 ? 2 : 0);
        if (rc == OB_SUCCESS && found != expected) { rc = OB_ERR_UNEXPECTED; }
      }
      for (const char *name : {"LOOKUP_NAME", "hidden_name", "idx_name", "lob_name"}) {
        uint64_t found = OB_INVALID_ID;
        ObTableType type = MAX_TABLE_TYPE;
        int64_t version = 0;
        if (rc == OB_SUCCESS) { rc = reader.get_table_id(database, 0, ObString::make_string(name), false, found, type, version); }
        if (rc == OB_SUCCESS && found != OB_INVALID_ID) { rc = OB_ERR_UNEXPECTED; }
      }
      uint64_t found = OB_INVALID_ID;
      if (rc == OB_SUCCESS) { rc = reader.get_index_id(database, ObString::make_string("IDX_NAME"), found); }
      if (rc == OB_SUCCESS && found != (present ? first_id + 4 : OB_INVALID_ID)) { rc = OB_ERR_UNEXPECTED; }
      return rc;
    };
    auto outside = [&]() {
      observer::NativeSchemaCatalog reader(namespace_id, *access, *transactions);
      int rc = reader.begin_read(deadline);
      return rc == OB_SUCCESS ? verify(reader, false) : rc;
    };
    const char *stage = "own_read";
    if (ret == OB_SUCCESS) { ret = verify(writer, true); }
    if (ret == OB_SUCCESS) { stage = "external_read"; ret = outside(); }
    const int owner_end = owner.is_active() ? native.rollback(owner) : OB_SUCCESS;
    const int participant_end = writer.close();
    if (ret == OB_SUCCESS) { ret = owner_end == OB_SUCCESS ? participant_end : owner_end; }
    if (ret == OB_SUCCESS) { stage = "read_rollback"; ret = outside(); }
    if (source != nullptr) { source->~ObTableSchema(); }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_NAMES_TX namespace=%lu stage=%s ret=%d\n", namespace_id, stage, ret);
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
      if (definition != nullptr) { definition->~ObTableSchema(); }
      ObArray<ObSimpleTableSchemaV2 *> simple;
      ObSEArray<uint64_t, 1> ids;
      if (rc == OB_SUCCESS) { rc = ids.push_back(table_id); }
      if (rc == OB_SUCCESS) { rc = reader.get_simple_tables(INT64_MAX - 1, allocator, simple, &ids); }
      if (rc == OB_SUCCESS && (simple.count() != 1
          || simple.at(0)->get_table_name_str() != ObString::make_string(expected))) {
        rc = OB_ERR_UNEXPECTED;
      }
      for (auto *table : simple) { table->~ObSimpleTableSchemaV2(); }
      return rc;
    };
    if (ret == OB_SUCCESS) { stage = "read_own"; ret = verify_name(borrowed, "tx_visible"); }
    if (ret == OB_SUCCESS) {
      stage = "read_external";
      observer::NativeSchemaCatalog outside(namespace_id, *access, *transactions);
      ret = outside.begin_read(deadline);
      if (ret == OB_SUCCESS) { ret = verify_name(outside, "t"); }
    }
    if (ret == OB_SUCCESS) {
      stage = "version_tombstone";
      int64_t version_index = -1, deleted_index = -1;
      ret = column_index(fixed, "schema_version", version_index);
      if (ret == OB_SUCCESS) { ret = column_index(fixed, "is_deleted", deleted_index); }
      int64_t deleted_version = 0;
      if (ret == OB_SUCCESS) {
        deleted_version = changed.cells_[version_index].get_int() + 1;
        changed.cells_[version_index].set_int(deleted_version);
        changed.cells_[deleted_index].set_int(1);
        ret = borrowed.insert(OB_ALL_TABLE_HISTORY_TID, changed);
      }
      VersionHisVal history;
      if (ret == OB_SUCCESS) { ret = borrowed.get_table_version_history(table_id, INT64_MAX, history); }
      if (ret == OB_SUCCESS && (!history.is_deleted_ || history.valid_cnt_ < 2
          || history.versions_[0] != deleted_version)) { ret = OB_ERR_UNEXPECTED; }
      ObSEArray<uint64_t, 1> ids;
      ObArray<ObTableLatestSchemaVersion> latest;
      if (ret == OB_SUCCESS) { ret = ids.push_back(table_id); }
      if (ret == OB_SUCCESS) { ret = borrowed.get_table_latest_schema_versions(ids, latest); }
      if (ret == OB_SUCCESS && (latest.count() != 1 || !latest.at(0).is_deleted()
          || latest.at(0).get_schema_version() != deleted_version)) { ret = OB_ERR_UNEXPECTED; }
      if (ret == OB_SUCCESS) {
        observer::NativeSchemaCatalog outside(namespace_id, *access, *transactions);
        ret = outside.begin_read(deadline);
        if (ret == OB_SUCCESS) { ret = outside.get_table_version_history(table_id, INT64_MAX, history); }
        if (ret == OB_SUCCESS && (history.is_deleted_ || history.valid_cnt_ < 1
            || history.versions_[0] >= deleted_version)) { ret = OB_ERR_UNEXPECTED; }
      }
      fprintf(stderr, "NATIVE_SCHEMA_CATALOG_TOMBSTONE namespace=%lu ret=%d\n", namespace_id, ret);
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
  if (ret == OB_SUCCESS && name.find("_indexed") != std::string::npos) {
    ObArenaAllocator allocator(ObMemAttr("CatalogIdxProbe"));
    ObTableSchema fixed(&allocator);
    ObNewRow sample;
    observer::NativeSchemaCatalog::Cursor rows;
    ObObj id; id.set_int(table_id);
    ret = catalog.open_prefix(OB_ALL_TABLE_HISTORY_TID, ObRowkey(&id, 1), rows, true);
    if (ret == OB_SUCCESS) { ret = fixed.assign(*rows.schema()); }
    const ObNewRow *row = nullptr;
    if (ret == OB_SUCCESS) { ret = rows.next(row); }
    if (ret == OB_SUCCESS) { ret = ob_write_row(allocator, *row, sample); }
    const int rows_end = rows.close();
    if (ret == OB_SUCCESS) { ret = rows_end; }
    int64_t body_pos = -1, data_pos = -1;
    if (ret == OB_SUCCESS) { ret = column_index(fixed, "view_definition", body_pos); }
    if (ret == OB_SUCCESS) { ret = column_index(fixed, "data_table_id", data_pos); }
    const int64_t synthetic_id = 5000000000LL + namespace_id;
    const int64_t first_data = 6000000000LL + namespace_id, second_data = first_data + 100000;
    std::string first(90000, 'a'), second(120000, 'b');
    int64_t version = 0;
    if (ret == OB_SUCCESS) {
      sample.cells_[0].set_int(synthetic_id);
      version = sample.cells_[1].get_int();
      sample.cells_[body_pos].set_meta_type(fixed.get_column_schema(ObString::make_string("view_definition"))->get_meta_type());
      if (!sample.cells_[body_pos].is_lob_storage()) { ret = OB_ERR_UNEXPECTED; }
    }
    auto set_values = [&](const std::string &body, int64_t data) {
      sample.cells_[data_pos].set_int(data);
      sample.cells_[body_pos].set_string(sample.cells_[body_pos].get_type(), ObString(body.size(), body.data()));
      sample.cells_[body_pos].set_inrow();
    };
    const int64_t deadline = ObTimeUtility::current_time() + 30000000;
    storage::NativeInnerTable native(*access, *transactions);
    storage::NativeInnerTable::Transaction owner;
    observer::NativeSchemaCatalog writer(namespace_id, *access, *transactions);
    auto begin = [&]() {
      int rc = native.begin(owner, deadline);
      if (rc == OB_SUCCESS) {
        rc = native.with_native_transaction(owner, [&](transaction::ObTxDesc &tx) {
          return writer.attach(tx, deadline);
        });
      }
      return rc;
    };
    auto end = [&](bool commit) {
      int rc = owner.is_active() ? (commit ? native.commit(owner) : native.rollback(owner)) : OB_SUCCESS;
      const int close = writer.close();
      return rc == OB_SUCCESS ? close : rc;
    };
    auto verify = [&](observer::NativeSchemaCatalog &reader, const std::string *expected, int64_t data) {
      observer::NativeSchemaCatalog::Cursor scan;
      ObObj key; key.set_int(synthetic_id);
      int rc = reader.open_prefix(OB_ALL_TABLE_HISTORY_TID, ObRowkey(&key, 1), scan);
      const ObNewRow *found = nullptr;
      if (rc == OB_SUCCESS) { rc = scan.next(found); }
      if (expected == nullptr) { rc = rc == OB_ITER_END ? OB_SUCCESS : OB_ERR_UNEXPECTED; }
      else if (rc == OB_SUCCESS && (found->cells_[body_pos].get_string()
          != ObString(expected->size(), expected->data()) || found->cells_[data_pos].get_int() != data)) {
        rc = OB_ERR_UNEXPECTED;
      }
      const int closed = scan.close();
      if (rc == OB_SUCCESS) { rc = closed; }
      for (int pass = 0; rc == OB_SUCCESS && pass < 2; ++pass) {
        const int64_t index_data = pass == 0 ? first_data : second_data;
        key.set_int(index_data);
        rc = reader.open_prefix(OB_ALL_TABLE_HISTORY_IDX_DATA_TABLE_ID_TID, ObRowkey(&key, 1), scan);
        const bool should_exist = expected != nullptr && data == index_data;
        if (rc == OB_SUCCESS) { rc = scan.next(found); }
        if (!should_exist) { rc = rc == OB_ITER_END ? OB_SUCCESS : OB_ERR_UNEXPECTED; }
        else if (rc == OB_SUCCESS) {
          if (found->count_ != 3 || found->cells_[0].get_int() != index_data
              || found->cells_[1].get_int() != synthetic_id || found->cells_[2].get_int() != version) {
            rc = OB_ERR_UNEXPECTED;
          } else if (scan.next(found) != OB_ITER_END) { rc = OB_ERR_UNEXPECTED; }
        }
        const int close = scan.close();
        if (rc == OB_SUCCESS) { rc = close; }
      }
      return rc;
    };
    auto verify_committed = [&](const std::string *expected, int64_t data) {
      observer::NativeSchemaCatalog reader(namespace_id, *access, *transactions);
      int rc = reader.begin_read(deadline);
      if (rc == OB_SUCCESS) { rc = verify(reader, expected, data); }
      return rc;
    };
    const char *stage = "begin";
    if (ret == OB_SUCCESS) { ret = begin(); }
    if (ret == OB_SUCCESS) { set_values(first, first_data); stage = "insert"; ret = writer.insert(OB_ALL_TABLE_HISTORY_TID, sample); }
    if (ret == OB_SUCCESS) { stage = "read_insert"; ret = verify(writer, &first, first_data); }
    bool matched = false;
    if (ret == OB_SUCCESS) {
      stage = "partial_index_key";
      ret = writer.update_fields(OB_ALL_TABLE_HISTORY_TID, [&](SchemaCatalogRow &fields) {
        int rc = fields.add_pk_column("table_id", synthetic_id);
        if (rc == OB_SUCCESS) { rc = fields.add_pk_column("schema_version", version); }
        if (rc == OB_SUCCESS) { rc = fields.add_column("data_table_id", second_data); }
        return rc;
      }, matched);
      if (ret == OB_SUCCESS && !matched) { ret = OB_ERR_UNEXPECTED; }
    }
    if (ret == OB_SUCCESS) { ret = verify(writer, &first, second_data); }
    if (ret == OB_SUCCESS) {
      stage = "partial_lob";
      ret = writer.update_fields(OB_ALL_TABLE_HISTORY_TID, [&](SchemaCatalogRow &fields) {
        int rc = fields.add_pk_column("table_id", synthetic_id);
        if (rc == OB_SUCCESS) { rc = fields.add_pk_column("schema_version", version); }
        if (rc == OB_SUCCESS) { rc = fields.add_column("view_definition", ObString(second.size(), second.data())); }
        return rc;
      }, matched);
      if (ret == OB_SUCCESS && !matched) { ret = OB_ERR_UNEXPECTED; }
    }
    if (ret == OB_SUCCESS) { ret = verify(writer, &second, second_data); }
    if (ret == OB_SUCCESS) {
      stage = "partial_missing";
      ret = writer.update_fields(OB_ALL_TABLE_HISTORY_TID, [&](SchemaCatalogRow &fields) {
        int rc = fields.add_pk_column("table_id", synthetic_id + 10000000);
        if (rc == OB_SUCCESS) { rc = fields.add_pk_column("schema_version", version); }
        if (rc == OB_SUCCESS) { rc = fields.add_column("data_table_id", second_data); }
        return rc;
      }, matched);
      if (ret == OB_SUCCESS && matched) { ret = OB_ERR_UNEXPECTED; }
    }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_PARTIAL namespace=%lu stage=%s ret=%d\n", namespace_id, stage, ret);
    if (ret == OB_SUCCESS) { set_values(second, second_data); stage = "update_key"; ret = writer.update(OB_ALL_TABLE_HISTORY_TID, sample); }
    if (ret == OB_SUCCESS) { stage = "read_update"; ret = verify(writer, &second, second_data); }
    if (ret == OB_SUCCESS) { stage = "uncommitted_invisible"; ret = verify_committed(nullptr, 0); }
    const int rollback = end(false);
    if (ret == OB_SUCCESS) { ret = rollback; }
    if (ret == OB_SUCCESS) { stage = "rollback"; ret = verify_committed(nullptr, 0); }
    if (ret == OB_SUCCESS) {
      set_values(first, first_data);
      stage = "insert_commit";
      do {
        ret = begin();
        if (ret == OB_SUCCESS) { ret = writer.insert(OB_ALL_TABLE_HISTORY_TID, sample); }
        if (ret != OB_TRY_LOCK_ROW_CONFLICT || ObTimeUtility::current_time() >= deadline) { break; }
        ret = end(false);
        if (ret != OB_SUCCESS) { break; }
        ob_usleep(1000);
      } while (ret == OB_SUCCESS);
    }
    if (ret == OB_SUCCESS) { ret = end(true); }
    if (ret == OB_SUCCESS) { stage = "committed"; ret = verify_committed(&first, first_data); }
    if (ret == OB_SUCCESS) { ret = begin(); }
    bool existed = false;
    if (ret == OB_SUCCESS) { set_values(second, second_data); stage = "update_again"; ret = writer.update(OB_ALL_TABLE_HISTORY_TID, sample); }
    if (ret == OB_SUCCESS) { stage = "erase"; ret = writer.erase(OB_ALL_TABLE_HISTORY_TID, ObRowkey(sample.cells_, 2), existed); }
    if (ret == OB_SUCCESS && !existed) { ret = OB_ERR_UNEXPECTED; }
    if (ret == OB_SUCCESS) { ret = verify(writer, nullptr, 0); }
    const int undo = end(false);
    if (ret == OB_SUCCESS) { ret = undo; }
    if (ret == OB_SUCCESS) { stage = "rollback_erase"; ret = verify_committed(&first, first_data); }
    if (ret == OB_SUCCESS) { ret = begin(); }
    if (ret == OB_SUCCESS) { ret = writer.erase(OB_ALL_TABLE_HISTORY_TID, ObRowkey(sample.cells_, 2), existed); }
    if (ret == OB_SUCCESS) { ret = end(true); }
    if (ret == OB_SUCCESS) { stage = "erase_committed"; ret = verify_committed(nullptr, 0); }
    const int cleanup = end(false);
    if (ret == OB_SUCCESS) { ret = cleanup; }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_INDEXED namespace=%lu stage=%s ret=%d\n", namespace_id, stage, ret);
  }
  if (ret == OB_SUCCESS && name.find("_typed") != std::string::npos) {
    ObArenaAllocator allocator(ObMemAttr("CatalogTyped"));
    ObTableSchema *definition = nullptr;
    ret = catalog.get_table(table_id, INT64_MAX - 1, allocator, definition);
    const int64_t synthetic_id = 5000000000LL + namespace_id;
    if (ret == OB_SUCCESS) {
      definition->set_table_id(synthetic_id);
      ret = definition->set_table_name("typed_copy");
    }
    const int64_t deadline = ObTimeUtility::current_time() + 30000000;
    storage::NativeInnerTable native(*access, *transactions);
    storage::NativeInnerTable::Transaction owner;
    observer::NativeSchemaCatalog writer(namespace_id, *access, *transactions);
    auto begin = [&]() {
      int rc = native.begin(owner, deadline);
      if (rc == OB_SUCCESS) {
        rc = native.with_native_transaction(owner, [&](transaction::ObTxDesc &tx) { return writer.attach(tx, deadline); });
      }
      return rc;
    };
    auto end = [&](bool commit) {
      int rc = owner.is_active() ? (commit ? native.commit(owner) : native.rollback(owner)) : OB_SUCCESS;
      const int close = writer.close();
      return rc == OB_SUCCESS ? close : rc;
    };
    auto insert = [&]() {
      int rc = writer.insert_table(*definition, false);
      if (rc == OB_SUCCESS) { rc = writer.insert_table(*definition, true); }
      for (auto it = definition->column_begin(); rc == OB_SUCCESS && it != definition->column_end(); ++it) {
        ObColumnSchemaV2 column;
        rc = column.assign(**it);
        column.set_table_id(synthetic_id);
        column.set_schema_version(definition->get_schema_version());
        if (rc == OB_SUCCESS) { rc = writer.insert_column(column, false); }
        if (rc == OB_SUCCESS) { rc = writer.insert_column(column, true); }
      }
      return rc;
    };
    auto verify = [&](observer::NativeSchemaCatalog &reader, bool present) {
      ObArenaAllocator read_allocator(ObMemAttr("CatalogTypedRd"));
      ObArray<ObSchemaIdVersion> versions;
      ObSEArray<uint64_t, 1> ids;
      int rc = ids.push_back(synthetic_id);
      if (rc == OB_SUCCESS) { rc = reader.get_table_schema_versions(ids, versions); }
      if (rc == OB_SUCCESS && (versions.count() != (present ? 1 : 0)
          || (present && versions.at(0).get_schema_version() != definition->get_schema_version()))) { rc = OB_ERR_UNEXPECTED; }
      if (rc != OB_SUCCESS) { return rc; }
      ObTableSchema *found = nullptr;
      rc = reader.get_table(synthetic_id, INT64_MAX - 1, read_allocator, found);
      if (rc != OB_SUCCESS && present) { fprintf(stderr, "NATIVE_SCHEMA_CATALOG_TYPED_READ table=%ld ret=%d\n", synthetic_id, rc); }
      if (!present) { return rc == OB_ERR_SCHEMA_HISTORY_EMPTY ? OB_SUCCESS : OB_ERR_UNEXPECTED; }
      if (rc == OB_SUCCESS && (!found->is_valid() || found->get_column_count() != definition->get_column_count()
          || found->get_table_name_str() != definition->get_table_name_str())) { rc = OB_ERR_UNEXPECTED; }
      for (auto it = definition->column_begin(); rc == OB_SUCCESS && it != definition->column_end(); ++it) {
        const auto *column = found->get_column_schema((*it)->get_column_id());
        if (column == nullptr || column->get_column_name_str() != (*it)->get_column_name_str()
            || column->get_cur_default_value() != (*it)->get_cur_default_value()
            || column->get_orig_default_value() != (*it)->get_orig_default_value()
            || column->get_data_type() != (*it)->get_data_type()) { rc = OB_ERR_UNEXPECTED; }
      }
      if (found != nullptr) { found->~ObTableSchema(); }
      // Verify the real current-table name index and both column-name indexes.
      for (int pass = 0; rc == OB_SUCCESS && pass < 3; ++pass) {
        observer::NativeSchemaCatalog::Cursor cursor;
        ObObj keys[2];
        uint64_t index = OB_ALL_TABLE_IDX_DB_TB_NAME_TID;
        if (pass == 0) {
          keys[0].set_int(definition->get_database_id());
          keys[1].set_varchar("typed_copy");
        } else {
          index = pass == 1 ? OB_ALL_COLUMN_IDX_TB_COLUMN_NAME_TID : OB_ALL_COLUMN_IDX_COLUMN_NAME_TID;
          if (pass == 1) { keys[0].set_int(synthetic_id); keys[1].set_varchar("v"); }
          else { keys[0].set_varchar("v"); keys[1].set_int(synthetic_id); }
        }
        ObTableSchema fixed;
        rc = share::fixed_inner_table_creator(index)(fixed);
        ObSEArray<ObColDesc, 4> descriptions;
        if (rc == OB_SUCCESS) { rc = fixed.get_column_ids(descriptions); }
        for (int i = 0; rc == OB_SUCCESS && i < 2; ++i) { keys[i].set_meta_type(descriptions.at(i).col_type_); }
        if (rc == OB_SUCCESS) { rc = reader.open_prefix(index, ObRowkey(keys, 2), cursor); }
        const ObNewRow *row = nullptr;
        if (rc == OB_SUCCESS) { rc = cursor.next(row); }
        if (rc == OB_SUCCESS && cursor.next(row) != OB_ITER_END) { rc = OB_ERR_UNEXPECTED; }
        if (rc != OB_SUCCESS) { fprintf(stderr, "NATIVE_SCHEMA_CATALOG_TYPED_INDEX index=%lu ret=%d\n", index, rc); }
      }
      return rc;
    };
    auto outside = [&](bool present) {
      observer::NativeSchemaCatalog reader(namespace_id, *access, *transactions);
      int rc = reader.begin_read(deadline);
      return rc == OB_SUCCESS ? verify(reader, present) : rc;
    };
    const char *stage = "begin";
    if (ret == OB_SUCCESS) { ret = begin(); }
    if (ret == OB_SUCCESS) { stage = "insert"; ret = insert(); }
    if (ret == OB_SUCCESS) { stage = "own_read"; ret = verify(writer, true); }
    if (ret == OB_SUCCESS) { stage = "invisible"; ret = outside(false); }
    const int rollback = end(false);
    if (ret == OB_SUCCESS) { ret = rollback; }
    if (ret == OB_SUCCESS) { stage = "rolled_back"; ret = outside(false); }
    if (ret == OB_SUCCESS) {
      stage = "commit";
      do {
        ret = begin();
        if (ret == OB_SUCCESS) { ret = insert(); }
        if (ret != OB_TRY_LOCK_ROW_CONFLICT || ObTimeUtility::current_time() >= deadline) { break; }
        ret = end(false);
        if (ret == OB_SUCCESS) { ob_usleep(1000); }
      } while (ret == OB_SUCCESS);
    }
    if (ret == OB_SUCCESS) { ret = end(true); }
    if (ret == OB_SUCCESS) { stage = "committed"; ret = outside(true); }
    const int cleanup = end(false);
    if (ret == OB_SUCCESS) { ret = cleanup; }
    if (definition != nullptr) { definition->~ObTableSchema(); }
    fprintf(stderr, "NATIVE_SCHEMA_CATALOG_TYPED namespace=%lu stage=%s ret=%d\n", namespace_id, stage, ret);
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
      stage = "retain_lob_locator";
      ObArenaAllocator raw_arena(ObMemAttr("RawLobProbe"));
      ObNewRow before, after;
      ret = native.get_storage_row_for_update(tx, table, key, raw_arena, before);
      int64_t comment = -1;
      if (ret == OB_SUCCESS) { ret = column_index(fixed, "comment", comment); }
      ObSEArray<uint64_t, 1> changed;
      if (ret == OB_SUCCESS) {
        const auto *column = fixed.get_column_schema(ObString::make_string("comment"));
        ret = changed.push_back(column->get_column_id());
        sample.cells_[comment].set_meta_type(column->get_meta_type());
        sample.cells_[comment].set_string(column->get_data_type(), ObString::make_string("partial update"));
      }
      if (ret == OB_SUCCESS) { ret = native.update(tx, table, sample, changed); }
      if (ret == OB_SUCCESS) { ret = native.get_storage_row_for_update(tx, table, key, raw_arena, after); }
      if (ret == OB_SUCCESS && (before.cells_[value_index].get_string() != after.cells_[value_index].get_string()
          || after.cells_[comment].get_string() != ObString::make_string("partial update"))) { ret = OB_ERR_UNEXPECTED; }
      if (ret == OB_SUCCESS) { ret = verify(&first); }
      fprintf(stderr, "NATIVE_SCHEMA_CATALOG_LOB_RETAIN namespace=%lu ret=%d\n", namespace_id, ret);
    }
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
    if (ret == OB_SUCCESS) {
      // tx predates another writer's commit. Its partial update must retain
      // that writer's comment even though an ordinary snapshot read is older.
      stage = "partial_after_other_commit";
      storage::NativeInnerTable::Transaction other;
      int64_t comment = -1;
      ret = column_index(fixed, "comment", comment);
      ObSEArray<uint64_t, 1> changed;
      if (ret == OB_SUCCESS) { ret = changed.push_back(fixed.get_column_schema(ObString::make_string("comment"))->get_column_id()); }
      if (ret == OB_SUCCESS) {
        sample.cells_[comment].set_string(sample.cells_[comment].get_type(), ObString::make_string("other committed"));
        ret = native.begin(other, deadline);
      }
      if (ret == OB_SUCCESS) { ret = native.update(other, table, sample, changed); }
      if (ret == OB_SUCCESS) { ret = native.commit(other); }
      if (other.is_active()) { native.rollback(other); }
      changed.reset();
      if (ret == OB_SUCCESS) { ret = changed.push_back(fixed.get_column_schema(ObString::make_string("view_definition"))->get_column_id()); }
      if (ret == OB_SUCCESS) {
        sample.cells_[comment].set_null(); // unselected input must be ignored
        sample.cells_[value_index].set_string(sample.cells_[value_index].get_type(), ObString(second.size(), second.data()));
        ret = native.update(tx, table, sample, changed);
      }
      ObArenaAllocator raw_arena(ObMemAttr("PartialRace"));
      ObNewRow current;
      if (ret == OB_SUCCESS) { ret = native.get_storage_row_for_update(tx, table, key, raw_arena, current); }
      if (ret == OB_SUCCESS && current.cells_[comment].get_string() != ObString::make_string("other committed")) { ret = OB_ERR_UNEXPECTED; }
      if (ret == OB_SUCCESS) { ret = verify(&second); }
      fprintf(stderr, "NATIVE_SCHEMA_CATALOG_PARTIAL_LATEST namespace=%lu ret=%d\n", namespace_id, ret);
      const int undo = tx.is_active() ? native.rollback(tx) : OB_SUCCESS;
      if (ret == OB_SUCCESS) { ret = undo; }
      if (ret == OB_SUCCESS) { ret = native.begin(tx, deadline); }
      if (ret == OB_SUCCESS) { ret = verify(&first); }
    }
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
