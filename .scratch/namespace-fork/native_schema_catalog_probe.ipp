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
    if (ret == OB_SUCCESS && expected_foreign_keys > 0) {
      const auto &fk = definition->get_foreign_key_infos().at(0);
      ObTableSchema *parent = nullptr;
      ret = catalog.get_table(fk.parent_table_id_, requested_version, allocator, parent);
      if (ret == OB_SUCCESS && (parent->get_foreign_key_infos().count() != 1
          || parent->get_foreign_key_infos().at(0).foreign_key_id_ != fk.foreign_key_id_)) {
        ret = OB_ERR_UNEXPECTED;
      }
      if (parent != nullptr) { parent->~ObTableSchema(); }
    }
    if (definition != nullptr) { definition->~ObTableSchema(); definition = nullptr; }
    if (ret == OB_SUCCESS) {
      ret = catalog.get_table(table_id, INT64_MAX, allocator, definition,
          observer::NativeSchemaCatalog::TableSource::CURRENT);
      const int64_t current_columns = expected_columns
          + (name.find("_history") != std::string::npos ? 1 : 0);
      const int64_t current_fks = name.find("_old_fk") != std::string::npos ? 0 : expected_foreign_keys;
      if (ret == OB_SUCCESS && (definition->get_column_count() != current_columns
          || definition->get_partition_num() != expected_parts
          || definition->get_index_tid_count() != expected_indexes
          || definition->get_constraint_count() != expected_constraints
          || definition->get_foreign_key_infos().count() != current_fks)) {
        ret = OB_ERR_UNEXPECTED;
      }
      fprintf(stderr, "NATIVE_SCHEMA_CATALOG_CURRENT namespace=%lu table=%llu ret=%d\n",
          namespace_id, table_id, ret);
    }
    if (definition != nullptr) { definition->~ObTableSchema(); }
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
      ObTableSchema *found = nullptr;
      int rc = reader.get_table(synthetic_id, INT64_MAX - 1, read_allocator, found);
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
