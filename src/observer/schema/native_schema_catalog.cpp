/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SHARE_SCHEMA
#include "observer/schema/native_schema_catalog.h"
#include "observer/schema/ob_schema_retrieve_utils.h"
#include "share/schema/schema_catalog_fields.h"
#include "share/schema/schema_catalog_row.h"
#include "share/ob_core_table_proxy.h"
#include <map>
#include <unordered_map>
#include <set>
#include "rootserver/fork_table/namespace_tablet_access.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "storage/access/native_inner_table.h"
#include "storage/tablet/ob_tablet.h"
#include "storage/tablet/ob_tablet_create_delete_helper.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/tx_storage/ob_access_service.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "query/session/ob_inner_sql_connection_access.h"

namespace oceanbase {
namespace observer {
using namespace common;
using namespace share::schema;
using storage::NativeInnerTable;
using storage::NamespaceForkKernelPrototype;

struct NativeSchemaCatalog::TableState
{
  ObArenaAllocator allocator{ObMemAttr("NativeCatalog")};
  ObTableSchema schema{&allocator};
  ObSEArray<ObColDesc, 8> columns;
  ns::TabletAccess access;
  NativeInnerTable::Table table;
  NativeInnerTable::ReadSource source;
  TO_STRING_KV("table_id", schema.get_table_id(), "tablet", source.tablet);

  int prepare(NativeInnerTable &native, uint64_t namespace_id, uint64_t inner_table_id,
      const ns::NamespaceCatalogViews::Handle &view, bool write)
  {
    const auto creator = share::fixed_inner_table_creator(inner_table_id);
    if (creator == nullptr) { return OB_INVALID_ARGUMENT; }
    int ret = creator(schema);
    if (OB_SUCC(ret)) {
      ret = write
          ? access.prepare_write(namespace_id, inner_table_id, schema.get_tablet_id(),
                                 data_plane::ObNamespaceAccessMode::LEASED)
          : access.prepare_read(namespace_id, inner_table_id, schema.get_tablet_id(),
                                data_plane::ObNamespaceAccessMode::LEASED, view);
    }
    if (OB_SUCC(ret)) {
      source.tablet = access.tablet();
      if (access.cap_scn() > 0) { ret = source.snapshot_cap.convert_for_tx(access.cap_scn()); }
    }
    // Fixed layout comes from code, installed version from physical metadata.
    storage::ObTabletHandle handle;
    if (OB_SUCC(ret)) {
      ret = storage::ObTabletCreateDeleteHelper::check_and_get_tablet(
          storage::ObTabletMapKey(source.tablet), handle, 0,
          storage::ObMDSGetTabletMode::READ_WITHOUT_CHECK,
          transaction::ObTransVersion::MAX_TRANS_VERSION);
    }
    int64_t version = 0;
    if (OB_SUCC(ret)) { ret = handle.get_obj()->get_newest_schema_version(version); }
    if (OB_SUCC(ret)) {
      schema.set_schema_version(version);
      ret = schema.get_column_ids(columns);
    }
    if (OB_SUCC(ret)) { ret = native.bind(table, access.schema_tablet(), schema); }
    return ret;
  }

  int project(const ObNewRow &row, const TableState &index, ObIArray<ObObj> &cells) const
  {
    cells.reuse();
    if (row.count_ != columns.count() || row.cells_ == nullptr) { return OB_INVALID_ARGUMENT; }
    int ret = OB_SUCCESS;
    for (const auto &column : index.columns) {
      if (OB_FAIL(ret)) { break; }
      int64_t position = -1;
      for (int64_t i = 0; i < columns.count(); ++i) {
        if (columns.at(i).col_id_ == column.col_id_) { position = i; break; }
      }
      if (position < 0) { ret = OB_NOT_SUPPORTED; }
      else { ret = cells.push_back(row.cells_[position]); }
    }
    return ret;
  }
};

struct NativeSchemaCatalog::State
{
  State(uint64_t id, storage::ObAccessService &access, transaction::ObTransService &transactions)
      : namespace_id(id), transactions(transactions), native(access, transactions) {}
  ~State()
  {
    // A borrowed owner has already ended; owned reads roll back before leases
    // and views are released, including when the last cursor owns this state.
    if (transaction.is_active()) {
      const int ret = transaction.is_borrowed() ? native.detach(transaction) : native.rollback(transaction);
      if (ret != OB_SUCCESS) { LOG_WARN("native catalog cleanup failed", K(ret)); }
    }
  }
  const uint64_t namespace_id;
  transaction::ObTransService &transactions;
  NativeInnerTable native;
  // The transaction ends before the protected directory view is released.
  ns::NamespaceCatalogViews::Handle view;
  NativeInnerTable::Transaction transaction;
  std::unordered_map<uint64_t, std::unique_ptr<TableState>> writers;
  bool writable = false;
  int64_t statement_time = 0;
  int64_t cursors = 0;
};

NativeSchemaCatalog::NativeSchemaCatalog(uint64_t id, storage::ObAccessService &access,
    transaction::ObTransService &transactions) : state_(new State(id, access, transactions)) {}
NativeSchemaCatalog::~NativeSchemaCatalog() { close(); }

int NativeSchemaCatalog::with_ddl_transaction(ObISQLClient &client, int64_t deadline,
    const std::function<int(NativeSchemaCatalog &)> &operation)
{
  auto *owner = dynamic_cast<ObMySQLTransaction *>(&client);
  if (owner == nullptr || !owner->is_started() || !operation) { return OB_INVALID_ARGUMENT; }
  auto *access = share::server_service<storage::ObAccessService>();
  auto *transactions = share::server_service<transaction::ObTransService>();
  if (access == nullptr || transactions == nullptr) { return OB_NOT_INIT; }
  std::shared_ptr<NativeSchemaCatalog> catalog(new (std::nothrow)
      NativeSchemaCatalog(owner->target_namespace(), *access, *transactions));
  if (!catalog) { return OB_ALLOCATE_MEMORY_FAILED; }
  int ret = owner->retain_until_end(catalog);
  if (OB_SUCC(ret)) {
    ret = query::ObInnerSQLConnectionAccess::with_native_transaction(owner->get_connection(),
        [&](transaction::ObTxDesc &native) {
      const int rc = catalog->attach(native, deadline);
      return rc == OB_SUCCESS ? operation(*catalog) : rc;
    });
  }
  return ret;
}

int NativeSchemaCatalog::begin_read(int64_t deadline, bool weak)
{
  auto &s = *state_;
  if (s.transaction.is_active() || s.view) { return OB_INIT_TWICE; }
  int ret = s.native.begin_read(s.transaction, deadline, [&](share::SCN &snapshot) {
    return NamespaceForkKernelPrototype::acquire_read_view(s.namespace_id,
        [&](share::SCN &selected) {
      const int rc = weak ? s.transactions.get_weak_read_snapshot_version(-1, selected)
                         : s.transactions.get_read_snapshot_version(deadline, selected);
      if (rc == OB_SUCCESS) { snapshot = selected; }
      return rc;
    }, s.view);
  });
  if (ret != OB_SUCCESS) { s.view.reset(); }
  return ret;
}

int NativeSchemaCatalog::begin_read(int64_t deadline,
    const ns::NamespaceCatalogViews::Handle &view)
{
  auto &s = *state_;
  if (s.transaction.is_active() || s.view) { return OB_INIT_TWICE; }
  if (!view || view->entry().namespace_id != s.namespace_id) { return OB_INVALID_ARGUMENT; }
  s.view = view;
  const int ret = s.native.begin_read(s.transaction, deadline, [&](share::SCN &snapshot) {
    return snapshot.convert_for_tx(view->entry().snapshot);
  });
  if (ret != OB_SUCCESS) { s.view.reset(); }
  return ret;
}

int NativeSchemaCatalog::attach(transaction::ObTxDesc &owner, int64_t deadline)
{
  auto &s = *state_;
  if (s.transaction.is_active() || s.view) { return OB_INIT_TWICE; }
  const int ret = s.native.attach(s.transaction, owner, deadline, [&](share::SCN &snapshot) {
    return NamespaceForkKernelPrototype::acquire_read_view(s.namespace_id,
        [&](share::SCN &selected) {
      const int rc = s.transactions.get_read_snapshot_version(deadline, selected);
      if (rc == OB_SUCCESS) { snapshot = selected; }
      return rc;
    }, s.view);
  });
  if (ret != OB_SUCCESS) { s.view.reset(); }
  else { s.writable = !owner.is_rdonly(); s.statement_time = ObTimeUtility::current_time(); }
  return ret;
}

struct NativeSchemaCatalog::Cursor::State
{
  explicit State(std::shared_ptr<NativeSchemaCatalog::State> operation)
      : operation(std::move(operation)) { ++this->operation->cursors; }
  ~State() { --operation->cursors; }
  // The operation and its directory view outlive the physical scan and lease.
  std::shared_ptr<NativeSchemaCatalog::State> operation;
  TableState bound;
  NativeInnerTable::Cursor scan;
};

NativeSchemaCatalog::Cursor::Cursor() = default;
NativeSchemaCatalog::Cursor::~Cursor() { close(); }
int NativeSchemaCatalog::Cursor::next(const ObNewRow *&row)
{
  row = nullptr;
  return state_ ? state_->scan.next(row) : OB_NOT_INIT;
}
int NativeSchemaCatalog::Cursor::close()
{
  const int ret = state_ ? state_->scan.close() : OB_SUCCESS;
  state_.reset();
  return ret;
}
const ObTableSchema *NativeSchemaCatalog::Cursor::schema() const
{ return state_ ? &state_->bound.schema : nullptr; }

int NativeSchemaCatalog::prepare_scan(uint64_t inner_table_id, Cursor &cursor)
{
  auto &s = *state_;
  if (cursor.state_) { return OB_INIT_TWICE; }
  if (!s.transaction.is_active() || !s.view) { return OB_NOT_INIT; }
  std::unique_ptr<Cursor::State> prepared(new (std::nothrow) Cursor::State(state_));
  if (!prepared) { return OB_ALLOCATE_MEMORY_FAILED; }
  const int ret = prepared->bound.prepare(s.native, s.namespace_id, inner_table_id, s.view, false);
  if (ret == OB_SUCCESS) { cursor.state_ = std::move(prepared); }
  return ret;
}

int NativeSchemaCatalog::prepare_write(uint64_t inner_table_id, TableState *&out)
{
  out = nullptr;
  auto &s = *state_;
  if (!s.transaction.is_active() || !s.view) { return OB_NOT_INIT; }
  if (!s.writable || s.cursors != 0) { return OB_INVALID_ARGUMENT; }
  const auto found = s.writers.find(inner_table_id);
  if (found != s.writers.end()) { out = found->second.get(); return OB_SUCCESS; }
  std::unique_ptr<TableState> prepared(new (std::nothrow) TableState());
  if (!prepared) { return OB_ALLOCATE_MEMORY_FAILED; }
  const int ret = prepared->prepare(s.native, s.namespace_id, inner_table_id, s.view, true);
  if (ret == OB_SUCCESS) {
    out = prepared.get();
    s.writers.emplace(inner_table_id, std::move(prepared));
  }
  return ret;
}

int NativeSchemaCatalog::start_scan(const ObNewRange &range, Cursor &cursor, bool reverse)
{
  auto &c = *cursor.state_;
  const int ret = state_->native.open_scan(state_->transaction, c.bound.table, range, c.scan, reverse, &c.bound.source);
  if (ret != OB_SUCCESS) { cursor.close(); }
  return ret;
}

int NativeSchemaCatalog::open_scan(uint64_t inner_table_id, const ObNewRange &range,
    Cursor &cursor, bool reverse)
{
  const int ret = prepare_scan(inner_table_id, cursor);
  return ret != OB_SUCCESS ? ret : start_scan(range, cursor, reverse);
}

int NativeSchemaCatalog::open_prefix(uint64_t inner_table_id, const ObRowkey &prefix,
    Cursor &cursor, bool reverse)
{
  int ret = prepare_scan(inner_table_id, cursor);
  if (OB_SUCC(ret)) {
    const int64_t count = cursor.schema()->get_rowkey_column_num();
    ObSEArray<ObObj, 8> lower, upper;
    if (prefix.get_obj_cnt() > count || prefix.get_obj_cnt() < 0
        || (prefix.get_obj_cnt() > 0 && prefix.get_obj_ptr() == nullptr)) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      ObObj first, last;
      if (i < prefix.get_obj_cnt()) { first = last = prefix.get_obj_ptr()[i]; }
      else { first.set_min_value(); last.set_max_value(); }
      if (OB_FAIL(lower.push_back(first))) {
      } else { ret = upper.push_back(last); }
    }
    if (OB_SUCC(ret)) {
      ObNewRange range;
      range.start_key_.assign(lower.get_data(), lower.count());
      range.end_key_.assign(upper.get_data(), upper.count());
      range.border_flag_.set_inclusive_start();
      range.border_flag_.set_inclusive_end();
      ret = start_scan(range, cursor, reverse);
    } else { cursor.close(); }
  }
  return ret;
}

int NativeSchemaCatalog::scan(uint64_t inner_table_id, const ObNewRange &range,
    const RowVisitor &visitor, bool reverse)
{
  if (!visitor) { return OB_INVALID_ARGUMENT; }
  Cursor cursor;
  int ret = open_scan(inner_table_id, range, cursor, reverse);
  bool stop = false;
  const ObNewRow *row = nullptr;
  while (OB_SUCC(ret) && !stop && OB_SUCC(ret = cursor.next(row))) {
    ret = visitor(*cursor.schema(), *row, stop);
  }
  if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  const int cleanup = cursor.close();
  return ret == OB_SUCCESS ? cleanup : ret;
}

namespace {
// Typed row access for the existing schema decoders. This has no SQL client or
// runtime dependency; values come directly from the native storage cursor.
class CatalogRows final
{
public:
  using Filter = std::function<int(const CatalogRows &, bool &)>;
  int open(NativeSchemaCatalog &catalog, uint64_t inner_id, const ObRowkey &prefix,
      int64_t version, bool reverse = false, int64_t minimum_version = 0,
      bool latest_per_key = false, Filter filter = {}, bool history = true)
  {
    version_ = version;
    history_ = history;
    minimum_version_ = minimum_version;
    latest_per_key_ = latest_per_key;
    filter_ = std::move(filter);
    // Taking the first version of an entity requires descending version order.
    if (latest_per_key && !reverse) { return OB_INVALID_ARGUMENT; }
    int ret = catalog.open_prefix(inner_id, prefix, cursor_, reverse);
    ObSEArray<ObColDesc, 8> columns;
    if (OB_SUCC(ret)) { ret = cursor_.schema()->get_column_ids(columns); }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      positions_.emplace(columns.at(i).col_id_, i);
    }
    return ret;
  }
  int next()
  {
    row_ = nullptr;
    int ret = OB_SUCCESS;
    while (OB_SUCC(ret = cursor_.next(row_))) {
      int64_t version = 0;
      if (version_ != INT64_MAX || minimum_version_ > 0) {
        if (OB_FAIL(get_int("schema_version", version))) { break; }
        if (version > version_ || version < minimum_version_) { continue; }
      }
      if (latest_per_key_) {
        ObRowkey entity(row_->cells_, cursor_.schema()->get_rowkey_column_num() - 1);
        if (last_entity_.get_obj_cnt() > 0 && last_entity_ == entity) { continue; }
        key_allocator_.reuse();
        if (OB_FAIL(entity.deep_copy(last_entity_, key_allocator_))) { break; }
      }
      bool accept = true;
      if (filter_ && OB_FAIL(filter_(*this, accept))) { break; }
      if (accept) { return OB_SUCCESS; }
    }
    row_ = nullptr;
    return ret;
  }
  int get_int(const char *name, int64_t &value) const
  {
    // Current records have no tombstone column; shared relationship decoders
    // consume the same logical field as their history counterparts.
    if (!history_ && 0 == STRCMP(name, "is_deleted")) { value = 0; return OB_SUCCESS; }
    const ObObj *obj = nullptr;
    int ret = cell(name, obj);
    if (OB_SUCC(ret)) {
      if (!obj->is_integer_type()) { ret = OB_OBJ_TYPE_ERROR; }
      else { value = obj->get_int(); }
    }
    return ret;
  }
  int get_uint(const char *name, uint64_t &value) const
  {
    int64_t bits = 0;
    const int ret = get_int(name, bits);
    if (ret == OB_SUCCESS) { value = static_cast<uint64_t>(bits); }
    return ret;
  }
  int get_bool(const char *name, bool &value) const
  {
    int64_t bits = 0;
    const int ret = get_int(name, bits);
    if (ret == OB_SUCCESS) { value = bits != 0; }
    return ret;
  }
  int get_varchar(const char *name, ObString &value) const
  {
    const ObObj *obj = nullptr;
    int ret = cell(name, obj);
    if (OB_SUCC(ret)) {
      if (!obj->is_string_type()) { ret = OB_OBJ_TYPE_ERROR; }
      else { value = obj->get_string(); }
    }
    return ret;
  }
  int get_datetime(const char *name, int64_t &value) const
  {
    const ObObj *obj = nullptr;
    int ret = cell(name, obj);
    if (OB_SUCC(ret)) {
      if (obj->is_timestamp()) { value = obj->get_timestamp(); }
      else { ret = obj->get_datetime(value); }
    }
    return ret;
  }
  int close() { row_ = nullptr; return cursor_.close(); }
private:
  int cell(const char *name, const ObObj *&out) const
  {
    out = nullptr;
    if (!row_ || !cursor_.schema()) { return OB_NOT_INIT; }
    const auto *column = cursor_.schema()->get_column_schema(ObString::make_string(name));
    if (!column) { return OB_ERR_COLUMN_NOT_FOUND; }
    const auto position = positions_.find(column->get_column_id());
    if (position == positions_.end()) { return OB_ERR_UNEXPECTED; }
    out = &row_->cells_[position->second];
    return out->is_null() ? OB_ERR_NULL_VALUE : OB_SUCCESS;
  }
  NativeSchemaCatalog::Cursor cursor_;
  std::unordered_map<uint64_t, int64_t> positions_;
  const ObNewRow *row_ = nullptr;
  int64_t version_ = 0;
  int64_t minimum_version_ = 0;
  bool latest_per_key_ = false;
  bool history_ = true;
  Filter filter_;
  ObArenaAllocator key_allocator_{ObMemAttr("CatalogRowKey")};
  ObRowkey last_entity_;
};

// An index can retain older relationships. Collect candidate identities, then
// let the caller inspect each entity's latest visible history in the same view.
// Index rowkeys do not necessarily put schema_version last, so do not use the
// main history table's latest_per_key rule here.
int collect_related_ids(NativeSchemaCatalog &catalog, uint64_t index_id,
    const ObRowkey &prefix, int64_t version, const char *id_column,
    std::set<uint64_t> &ids)
{
  CatalogRows rows;
  int ret = rows.open(catalog, index_id, prefix, version);
  while (OB_SUCC(ret) && OB_SUCC(rows.next())) {
    uint64_t id = OB_INVALID_ID;
    if (OB_SUCC(ret = rows.get_uint(id_column, id))) { ids.insert(id); }
  }
  return ret == OB_ITER_END ? OB_SUCCESS : ret;
}

// The core table stores one physical row per named cell. Group only one
// logical row at a time, then reuse the existing cell decoder. Callback values
// must be copied before it returns; both strings and cell storage are local.
int read_core_rows(NativeSchemaCatalog &catalog, const char *table_name,
    const std::function<int(const share::ObCoreTableProxy::Row &)> &visitor)
{
  ObObj key;
  key.set_varchar(table_name);
  key.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  CatalogRows rows;
  int ret = rows.open(catalog, OB_ALL_CORE_TABLE_TID, ObRowkey(&key, 1),
                      INT64_MAX, false, 0, false, {}, false);
  ObArenaAllocator allocator(ObMemAttr("CoreCatalogRow"));
  ObArray<share::ObCoreTableCell> cells;
  int64_t row_id = OB_INVALID_INDEX;
  const auto consume = [&]() {
    share::ObCoreTableProxy::Row row;
    const int rc = row.init(row_id, allocator, cells);
    return rc == OB_SUCCESS ? visitor(row) : rc;
  };
  while (OB_SUCC(ret) && OB_SUCC(rows.next())) {
    int64_t next_id = OB_INVALID_INDEX;
    ObString name, value;
    if (OB_FAIL(rows.get_int("row_id", next_id))) {
    } else if (OB_FAIL(rows.get_varchar("column_name", name))) {
    } else {
      ret = rows.get_varchar("column_value", value);
      if (ret == OB_ERR_NULL_VALUE) { ret = OB_SUCCESS; }
      else if (OB_SUCC(ret) && value.empty()) { value = ObString::make_string(""); }
    }
    if (OB_SUCC(ret) && row_id != OB_INVALID_INDEX && next_id != row_id) {
      ret = consume();
      cells.reuse();
      allocator.reuse();
    }
    if (OB_SUCC(ret)) {
      share::ObCoreTableCell source, cell;
      row_id = next_id;
      source.name_ = name;
      source.value_ = value;
      if (OB_FAIL(cell.assign(allocator, source))) {
      } else { ret = cells.push_back(cell); }
    }
  }
  if (ret == OB_ITER_END) {
    ret = row_id == OB_INVALID_INDEX ? OB_SUCCESS : consume();
  }
  return ret;
}

template<typename T>
struct CoreDefinition
{
  int64_t version = OB_INVALID_VERSION;
  std::unique_ptr<T> value;
};

int read_core_tables(NativeSchemaCatalog &catalog, int64_t version,
    ObIArray<ObTableSchema> &out, bool with_columns)
{
  std::map<uint64_t, CoreDefinition<ObTableSchema>> tables;
  int ret = read_core_rows(catalog, OB_ALL_TABLE_HISTORY_TNAME,
      [&](const share::ObCoreTableProxy::Row &row) {
    int64_t v = 0;
    uint64_t id = OB_INVALID_ID;
    int rc = row.get_int("schema_version", v);
    if (rc == OB_SUCCESS) { rc = row.get_uint("table_id", id); }
    if (rc != OB_SUCCESS || v > version || id == OB_ALL_CORE_TABLE_TID) { return rc; }
    auto &latest = tables[id];
    if (v <= latest.version) { return OB_SUCCESS; }
    std::unique_ptr<ObTableSchema> table(new (std::nothrow) ObTableSchema());
    if (!table) { return OB_ALLOCATE_MEMORY_FAILED; }
    bool deleted = false;
    rc = ObSchemaRetrieveUtils::fill_table_schema(true, row, *table, deleted);
    if (rc == OB_SUCCESS) {
      latest.version = v;
      latest.value = deleted ? nullptr : std::move(table);
    }
    return rc;
  });
  if (OB_SUCC(ret) && with_columns && !tables.empty()) {
    std::map<std::pair<uint64_t, uint64_t>, CoreDefinition<ObColumnSchemaV2>> columns;
    ret = read_core_rows(catalog, OB_ALL_COLUMN_HISTORY_TNAME,
        [&](const share::ObCoreTableProxy::Row &row) {
      int64_t v = 0;
      uint64_t table = OB_INVALID_ID, column = OB_INVALID_ID;
      int rc = row.get_int("schema_version", v);
      if (rc == OB_SUCCESS) { rc = row.get_uint("table_id", table); }
      if (rc == OB_SUCCESS) { rc = row.get_uint("column_id", column); }
      if (rc != OB_SUCCESS || v > version) { return rc; }
      const auto owner = tables.find(table);
      if (owner == tables.end() || !owner->second.value) { return OB_SUCCESS; }
      auto &latest = columns[{table, column}];
      if (v <= latest.version) { return OB_SUCCESS; }
      std::unique_ptr<ObColumnSchemaV2> definition(new (std::nothrow) ObColumnSchemaV2());
      if (!definition) { return OB_ALLOCATE_MEMORY_FAILED; }
      bool deleted = false;
      rc = ObSchemaRetrieveUtils::fill_column_schema(true, row, *definition, deleted);
      if (rc == OB_SUCCESS) {
        latest.version = v;
        latest.value = deleted ? nullptr : std::move(definition);
      }
      return rc;
    });
    for (const auto &column : columns) {
      if (OB_FAIL(ret)) { break; }
      if (column.second.value) {
        ret = tables.at(column.first.first).value->add_column(*column.second.value);
      }
    }
  }
  for (const auto &table : tables) {
    if (OB_FAIL(ret)) { break; }
    if (!table.second.value) { continue; }
    if (with_columns && table.second.value->get_column_count() <= 0) { ret = OB_ERR_UNEXPECTED; }
    else { ret = out.push_back(*table.second.value); }
  }
  return ret;
}

template<typename Table>
int read_partitions(NativeSchemaCatalog &catalog, int64_t version, bool history, Table &definition)
{
  Table *table = &definition;
  const uint64_t table_id = table->get_table_id();
  ObObj id;
  id.set_int(table_id);
  const ObRowkey prefix(&id, 1);
  auto open = [&](CatalogRows &rows, uint64_t current_id, uint64_t history_id,
      const ObRowkey &key, bool reverse = false, int64_t minimum = 0) {
    return rows.open(catalog, history ? history_id : current_id, key, version,
                     reverse, history ? minimum : 0, false, {}, history);
  };
  int ret = OB_SUCCESS;
  if (OB_SUCC(ret) && table->get_part_level() != PARTITION_LEVEL_ZERO) {
    CatalogRows rows;
    if (table->get_truncate_version() > version) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(open(rows, OB_ALL_PART_TID, OB_ALL_PART_HISTORY_TID, prefix,
                              false, table->get_truncate_version()))) {
    } else { ret = ObSchemaRetrieveUtils::retrieve_part_info(history, rows, table); }
  }
  if (OB_SUCC(ret) && table->get_part_level() == PARTITION_LEVEL_TWO
      && table->has_sub_part_template_def()) {
    CatalogRows rows;
    if (OB_FAIL(open(rows, OB_ALL_DEF_SUB_PART_TID, OB_ALL_DEF_SUB_PART_HISTORY_TID, prefix))) {
    } else { ret = ObSchemaRetrieveUtils::retrieve_def_subpart_info(history, rows, table); }
  }
  if (OB_SUCC(ret) && table->get_part_level() == PARTITION_LEVEL_TWO) {
    CatalogRows rows;
    if (OB_FAIL(open(rows, OB_ALL_SUB_PART_TID, OB_ALL_SUB_PART_HISTORY_TID, prefix,
                          false, table->get_truncate_version()))) {
    } else { ret = ObSchemaRetrieveUtils::retrieve_subpart_info(history, rows, table); }
  }
  if (OB_SUCC(ret) && is_virtual_table(table_id)
      && table->get_part_level() == PARTITION_LEVEL_ONE && table->get_part_array() == nullptr) {
    ret = table->mock_list_partition_array();
  }
  if (OB_SUCC(ret)) { ret = ObSchemaRetrieveUtils::sort_partition_array(*table); }
  if (OB_SUCC(ret)) { ret = ObSchemaRetrieveUtils::sort_subpartition_array(*table); }
  if (OB_SUCC(ret) && table->is_user_partition_table()
      && (table->get_part_array() == nullptr || table->get_partition_num() <= 0)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}
} // namespace

int NativeSchemaCatalog::get_table(uint64_t table_id, int64_t version,
    ObIAllocator &allocator, ObTableSchema *&out, TableSource source)
{
  out = nullptr;
  const bool history = source == TableSource::HISTORY;
  if (version <= 0 || table_id == OB_INVALID_ID || (!history && version != INT64_MAX)) {
    return OB_INVALID_ARGUMENT;
  }
  if (is_core_table(table_id)) {
    if (table_id == OB_ALL_CORE_TABLE_TID) {
      ObTableSchema definition;
      const int ret = share::ObInnerTableSchema::all_core_table_schema(definition);
      return ret == OB_SUCCESS ? ObSchemaUtils::alloc_schema(allocator, definition, out) : ret;
    }
    ObArray<ObTableSchema> definitions;
    int ret = get_core_tables(version, definitions);
    for (int64_t i = 0; OB_SUCC(ret) && i < definitions.count(); ++i) {
      if (definitions.at(i).get_table_id() == table_id) {
        return ObSchemaUtils::alloc_schema(allocator, definitions.at(i), out);
      }
    }
    return ret == OB_SUCCESS ? OB_TABLE_NOT_EXIST : ret;
  }
  auto open = [&](CatalogRows &rows, uint64_t current_id, uint64_t history_id,
      const ObRowkey &key, bool reverse = false, int64_t minimum = 0,
      bool latest = false, CatalogRows::Filter filter = {}) {
    return rows.open(*this, history ? history_id : current_id, key, version,
                     reverse, history ? minimum : 0, history && latest, std::move(filter), history);
  };
  ObObj id;
  id.set_int(table_id);
  const ObRowkey prefix(&id, 1);
  ObTableSchema *table = nullptr;
  int ret = OB_SUCCESS;
  {
    CatalogRows rows;
    if (OB_FAIL(open(rows, OB_ALL_TABLE_TID, OB_ALL_TABLE_HISTORY_TID, prefix, true, 0, true))) {
    } else if (OB_FAIL(rows.next())) {
      if (ret == OB_ITER_END) { ret = OB_ERR_SCHEMA_HISTORY_EMPTY; }
    } else {
      ObTableSchema definition;
      bool deleted = false;
      if (OB_FAIL(ObSchemaRetrieveUtils::fill_table_schema(history, rows, definition, deleted))) {
      } else if (deleted) { ret = OB_TABLE_NOT_EXIST; }
      else { ret = ObSchemaUtils::alloc_schema(allocator, definition, table); }
    }
  }
  {
    CatalogRows rows;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(open(rows, OB_ALL_COLUMN_TID, OB_ALL_COLUMN_HISTORY_TID, prefix))) {
    } else { ret = ObSchemaRetrieveUtils::retrieve_column_schema(history, rows, table); }
  }
  if (OB_SUCC(ret)) { ret = read_partitions(*this, version, history, *table); }
  {
    CatalogRows rows;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(open(rows, OB_ALL_CONSTRAINT_TID, OB_ALL_CONSTRAINT_HISTORY_TID, prefix))) {
    } else { ret = ObSchemaRetrieveUtils::retrieve_constraint(history, rows, table); }
  }
  if (OB_SUCC(ret)) {
    for (auto it = table->constraint_begin_for_non_const_iter();
         OB_SUCC(ret) && it != table->constraint_end_for_non_const_iter(); ++it) {
      ObObj keys[2] = {id, ObObj()};
      keys[1].set_int((*it)->get_constraint_id());
      CatalogRows rows;
      ObSEArray<uint64_t, 8> columns;
      if (OB_FAIL(open(rows, OB_ALL_CONSTRAINT_COLUMN_TID, OB_ALL_CONSTRAINT_COLUMN_HISTORY_TID,
                            ObRowkey(keys, 2), true, 0, true))) {
      } else {
        while (OB_SUCC(ret = rows.next())) {
          uint64_t column = OB_INVALID_ID;
          bool deleted = false;
          if (OB_FAIL(ObSchemaRetrieveUtils::fill_constraint_column_info(rows, column, deleted))) { break; }
          if (!deleted && OB_FAIL(columns.push_back(column))) { break; }
        }
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        if (OB_SUCC(ret)) {
          lib::ob_sort(columns.begin(), columns.end());
          ret = (*it)->assign_column_ids(columns);
        }
      }
    }
  }
  {
    std::set<uint64_t> ids;
    if (OB_SUCC(ret)) {
      ret = collect_related_ids(*this, history ? OB_ALL_FOREIGN_KEY_HISTORY_IDX_FK_HIS_CHILD_TID_TID
                                              : OB_ALL_FOREIGN_KEY_IDX_FK_CHILD_TID_TID,
                                prefix, version, "foreign_key_id", ids);
    }
    if (OB_SUCC(ret)) {
      ret = collect_related_ids(*this, history ? OB_ALL_FOREIGN_KEY_HISTORY_IDX_FK_HIS_PARENT_TID_TID
                                              : OB_ALL_FOREIGN_KEY_IDX_FK_PARENT_TID_TID,
                                prefix, version, "foreign_key_id", ids);
    }
    for (const uint64_t related : ids) {
      if (OB_FAIL(ret)) { break; }
      ObObj key;
      key.set_int(related);
      CatalogRows rows;
      if (OB_FAIL(open(rows, OB_ALL_FOREIGN_KEY_TID, OB_ALL_FOREIGN_KEY_HISTORY_TID, ObRowkey(&key, 1),
                                true, 0, true, [table_id](const CatalogRows &row, bool &accept) {
        int64_t child = 0, parent = 0, deleted = 0;
        int rc = row.get_int("is_deleted", deleted);
        if (rc == OB_SUCCESS && !deleted) { rc = row.get_int("child_table_id", child); }
        if (rc == OB_SUCCESS && !deleted) { rc = row.get_int("parent_table_id", parent); }
        accept = !deleted && (child == table_id || parent == table_id);
        return rc;
      }))) {
      } else { ret = ObSchemaRetrieveUtils::retrieve_foreign_key_info(rows, *table); }
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < table->get_foreign_key_infos().count(); ++i) {
      auto &foreign_key = table->get_foreign_key_infos().at(i);
      ObObj fk;
      fk.set_int(foreign_key.foreign_key_id_);
      CatalogRows rows;
      struct Column {
        int64_t position, child, parent;
        TO_STRING_KV(K(position), K(child), K(parent));
      };
      ObSEArray<Column, 8> columns;
      if (OB_FAIL(open(rows, OB_ALL_FOREIGN_KEY_COLUMN_TID, OB_ALL_FOREIGN_KEY_COLUMN_HISTORY_TID,
                            ObRowkey(&fk, 1), true, 0, true))) {
      } else {
        while (OB_SUCC(ret = rows.next())) {
          bool deleted = false;
          Column column{};
          if (OB_FAIL(ObSchemaRetrieveUtils::fill_foreign_key_column_info(
                          rows, column.child, column.parent, deleted))) { break; }
          if (!deleted) {
            if (OB_FAIL(rows.get_int("position", column.position))) { break; }
            if (OB_FAIL(columns.push_back(column))) { break; }
          }
        }
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        lib::ob_sort(columns.begin(), columns.end(),
            [](const Column &a, const Column &b) { return a.position < b.position; });
        for (int64_t j = 0; OB_SUCC(ret) && j < columns.count(); ++j) {
          if (OB_FAIL(foreign_key.child_column_ids_.push_back(columns.at(j).child))) {
          } else { ret = foreign_key.parent_column_ids_.push_back(columns.at(j).parent); }
        }
      }
    }
  }
  {
    std::set<uint64_t> ids;
    if (OB_SUCC(ret)) {
      ret = collect_related_ids(*this, history ? OB_ALL_TRIGGER_HISTORY_IDX_TRIGGER_HIS_BASE_OBJ_ID_TID
                                              : OB_ALL_TRIGGER_IDX_TRIGGER_BASE_OBJ_ID_TID,
                                prefix, version, "trigger_id", ids);
    }
    for (const uint64_t related : ids) {
      if (OB_FAIL(ret)) { break; }
      ObObj key;
      key.set_int(related);
      CatalogRows rows;
      if (OB_FAIL(open(rows, OB_ALL_TRIGGER_TID, OB_ALL_TRIGGER_HISTORY_TID, ObRowkey(&key, 1),
                                true, 0, true, [table_id](const CatalogRows &row, bool &accept) {
        int64_t object = 0, deleted = 0;
        int rc = row.get_int("is_deleted", deleted);
        if (rc == OB_SUCCESS && !deleted) { rc = row.get_int("base_object_id", object); }
        accept = !deleted && object == table_id;
        return rc;
      }))) {
      } else { ret = ObSchemaRetrieveUtils::retrieve_trigger_list(rows, table->get_trigger_list()); }
    }
  }
  {
    std::set<uint64_t> ids;
    if (OB_SUCC(ret)) {
      ret = collect_related_ids(*this, history ? OB_ALL_TABLE_HISTORY_IDX_DATA_TABLE_ID_TID
                                              : OB_ALL_TABLE_IDX_DATA_TABLE_ID_TID,
                                prefix, version, "table_id", ids);
    }
    for (const uint64_t related : ids) {
      if (OB_FAIL(ret)) { break; }
      ObObj key;
      key.set_int(related);
      CatalogRows rows;
      if (OB_FAIL(open(rows, OB_ALL_TABLE_TID, OB_ALL_TABLE_HISTORY_TID, ObRowkey(&key, 1),
                                true, 0, true, [table_id](const CatalogRows &row, bool &accept) {
        int64_t data_table = 0, deleted = 0;
        int rc = row.get_int("is_deleted", deleted);
        if (rc == OB_SUCCESS && !deleted) { rc = row.get_int("data_table_id", data_table); }
        accept = !deleted && data_table == table_id;
        return rc;
      }))) {
      } else {
        ObSEArray<ObAuxTableMetaInfo, 8> auxiliary;
        if (OB_FAIL(ObSchemaRetrieveUtils::retrieve_aux_tables(rows, auxiliary))) {
        } else {
          // Candidates are sorted by table ID, matching full schema consumers.
          for (int64_t i = 0; OB_SUCC(ret) && i < auxiliary.count(); ++i) {
            const auto &aux = auxiliary.at(i);
            if (aux.table_type_ == USER_INDEX) { ret = table->add_simple_index_info(aux); }
            else if (aux.table_type_ == AUX_LOB_META) { table->set_aux_lob_meta_tid(aux.table_id_); }
            else if (aux.table_type_ == AUX_LOB_PIECE) { table->set_aux_lob_piece_tid(aux.table_id_); }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) { out = table; }
  else if (table != nullptr) { table->~ObTableSchema(); }
  return ret;
}

int NativeSchemaCatalog::get_simple_tables(int64_t version, ObIAllocator &allocator,
    ObIArray<ObSimpleTableSchemaV2 *> &out, const ObIArray<uint64_t> *table_ids)
{
  if (version <= 0) { return OB_INVALID_ARGUMENT; }
  ObArray<ObSimpleTableSchemaV2 *> tables;
  int ret = OB_SUCCESS;
  if (table_ids == nullptr) {
    CatalogRows rows;
    if (OB_FAIL(rows.open(*this, OB_ALL_TABLE_HISTORY_TID, ObRowkey(), version,
                         true, 0, true, [](const CatalogRows &row, bool &accept) {
      uint64_t id = OB_INVALID_ID;
      const int rc = row.get_uint("table_id", id);
      accept = id != OB_ALL_CORE_TABLE_TID;
      return rc;
    }))) {
    } else { ret = ObSchemaRetrieveUtils::retrieve_table_schema(true, rows, allocator, tables); }
  } else {
    // Preserve the descending order required by the existing schema helpers,
    // and the duplicate-elimination semantics of the old SQL IN selection.
    std::set<uint64_t, std::greater<uint64_t>> ids;
    for (int64_t i = 0; i < table_ids->count(); ++i) { ids.insert(table_ids->at(i)); }
    for (const auto id : ids) {
      if (OB_FAIL(ret)) { break; }
      if (id == OB_INVALID_ID || id == 0) { ret = OB_INVALID_ARGUMENT; break; }
      ObObj key;
      key.set_int(id);
      CatalogRows rows;
      if (OB_FAIL(rows.open(*this, OB_ALL_TABLE_HISTORY_TID, ObRowkey(&key, 1),
                           version, true, 0, true))) {
      } else { ret = ObSchemaRetrieveUtils::retrieve_table_schema(true, rows, allocator, tables); }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
    ret = read_partitions(*this, version, true, *tables.at(i));
  }
  // Filter after latest-per-entity reduction: a tombstone must hide older live
  // rows, and an entity's old relation must not attach it to its former owner.
  const auto owned_live_row = [&tables](const char *owner) -> CatalogRows::Filter {
    return [&tables, owner](const CatalogRows &row, bool &accept) {
      bool deleted = false;
      uint64_t id = OB_INVALID_ID;
      int rc = row.get_bool("is_deleted", deleted);
      if (rc == OB_SUCCESS && !deleted) { rc = row.get_uint(owner, id); }
      accept = !deleted && ObSchemaRetrieveUtils::find_table_schema(id, tables) != nullptr;
      return rc;
    };
  };
  if (OB_SUCC(ret) && !tables.empty()) {
    if (table_ids == nullptr) {
      // A full refresh needs every relationship: scan each history once.
      {
        CatalogRows rows;
        if (OB_FAIL(rows.open(*this, OB_ALL_FOREIGN_KEY_HISTORY_TID, ObRowkey(), version,
                             true, 0, true, owned_live_row("child_table_id")))) {
        } else { ret = ObSchemaRetrieveUtils::retrieve_simple_foreign_key_info(rows, tables); }
      }
      {
        CatalogRows rows;
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(rows.open(*this, OB_ALL_CONSTRAINT_HISTORY_TID, ObRowkey(), version,
                                    true, 0, true, owned_live_row("table_id")))) {
        } else { ret = ObSchemaRetrieveUtils::retrieve_simple_constraint_info(rows, tables); }
      }
    } else {
      std::set<uint64_t> foreign_keys;
      for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
        ObObj key;
        key.set_int(tables.at(i)->get_table_id());
        const ObRowkey prefix(&key, 1);
        if (OB_FAIL(collect_related_ids(*this, OB_ALL_FOREIGN_KEY_HISTORY_IDX_FK_HIS_CHILD_TID_TID,
                                       prefix, version, "foreign_key_id", foreign_keys))) {
        } else {
          CatalogRows rows;
          if (OB_FAIL(rows.open(*this, OB_ALL_CONSTRAINT_HISTORY_TID, prefix, version,
                               true, 0, true, owned_live_row("table_id")))) {
          } else { ret = ObSchemaRetrieveUtils::retrieve_simple_constraint_info(rows, tables); }
        }
      }
      for (const auto id : foreign_keys) {
        if (OB_FAIL(ret)) { break; }
        ObObj key;
        key.set_int(id);
        CatalogRows rows;
        if (OB_FAIL(rows.open(*this, OB_ALL_FOREIGN_KEY_HISTORY_TID, ObRowkey(&key, 1), version,
                             true, 0, true, owned_live_row("child_table_id")))) {
        } else { ret = ObSchemaRetrieveUtils::retrieve_simple_foreign_key_info(rows, tables); }
      }
    }
  }
  if (OB_SUCC(ret)) { ret = out.reserve(out.count() + tables.count()); }
  int64_t transferred = 0;
  while (OB_SUCC(ret) && transferred < tables.count()) {
    if (OB_SUCC(ret = out.push_back(tables.at(transferred)))) { ++transferred; }
  }
  for (int64_t i = transferred; i < tables.count(); ++i) {
    tables.at(i)->~ObSimpleTableSchemaV2();
    allocator.free(tables.at(i));
  }
  return ret;
}

int NativeSchemaCatalog::get_core_tables(int64_t version, ObIArray<ObTableSchema> &out)
{
  if (version <= 0) { return OB_INVALID_ARGUMENT; }
  return read_core_tables(*this, version, out, true);
}

int NativeSchemaCatalog::get_table_version_history(uint64_t table_id, int64_t version,
    VersionHisVal &out)
{
  out.reset();
  if (table_id == OB_INVALID_ID || version <= 0) { return OB_INVALID_ARGUMENT; }
  ObObj id;
  id.set_int(table_id);
  const ObRowkey prefix(&id, 1);
  CatalogRows rows;
  int ret = rows.open(*this, OB_ALL_TABLE_HISTORY_TID, prefix, version, true);
  while (OB_SUCC(ret) && out.valid_cnt_ < MAX_CACHED_VERSION_CNT && OB_SUCC(rows.next())) {
    if (out.valid_cnt_ == 0) { ret = rows.get_bool("is_deleted", out.is_deleted_); }
    if (OB_SUCC(ret)) { ret = rows.get_int("schema_version", out.versions_[out.valid_cnt_]); }
    if (OB_SUCC(ret)) { ++out.valid_cnt_; }
  }
  if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  const int cleanup = rows.close();
  if (OB_SUCC(ret)) { ret = cleanup; }
  if (OB_SUCC(ret) && out.valid_cnt_ > 0) {
    // MIN covers all visible history, not just the newest cached versions.
    // Its first ascending row is enough; both cursors use this operation's view.
    CatalogRows first;
    if (OB_FAIL(first.open(*this, OB_ALL_TABLE_HISTORY_TID, prefix, version))) {
    } else if (OB_FAIL(first.next())) {
    } else { ret = first.get_int("schema_version", out.min_version_); }
  }
  if (OB_SUCC(ret)) { out.snapshot_version_ = version; }
  return ret;
}

int NativeSchemaCatalog::get_table_latest_schema_versions(const ObIArray<uint64_t> &table_ids,
    ObIArray<ObTableLatestSchemaVersion> &out)
{
  out.reset();
  if (table_ids.empty()) { return OB_INVALID_ARGUMENT; }
  std::set<uint64_t> ids;
  for (int64_t i = 0; i < table_ids.count(); ++i) {
    if (table_ids.at(i) == OB_INVALID_ID) { return OB_INVALID_ARGUMENT; }
    ids.insert(table_ids.at(i));
  }
  int ret = out.reserve(ids.size());
  for (const auto id : ids) {
    if (OB_FAIL(ret)) { break; }
    ObObj key;
    key.set_int(id);
    CatalogRows rows;
    if (OB_FAIL(rows.open(*this, OB_ALL_TABLE_HISTORY_TID, ObRowkey(&key, 1),
                         INT64_MAX, true, 0, true))) {
    } else { ret = ObSchemaRetrieveUtils::retrieve_table_latest_schema_versions(rows, out); }
  }
  return ret;
}

int NativeSchemaCatalog::get_original_table_version(uint64_t table_id, int64_t &out)
{
  out = OB_INVALID_VERSION;
  if (table_id == OB_INVALID_ID) { return OB_INVALID_ARGUMENT; }
  ObObj key;
  key.set_int(table_id);
  CatalogRows rows;
  int ret = rows.open(*this, OB_ALL_ORI_SCHEMA_VERSION_TID, ObRowkey(&key, 1),
                      INT64_MAX, false, 0, false, {}, false);
  if (OB_SUCC(ret)) { ret = rows.next(); }
  if (OB_SUCC(ret)) { ret = rows.get_int("ori_schema_version", out); }
  if (OB_SUCC(ret) && out <= 0) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) {
    const int next = rows.next();
    if (next != OB_ITER_END) { ret = next == OB_SUCCESS ? OB_ERR_UNEXPECTED : next; }
  }
  return ret;
}

int NativeSchemaCatalog::insert_table(const ObTableSchema &table, bool history,
    bool update_object_status_ignore_version)
{
  const uint64_t inner = history ? OB_ALL_TABLE_HISTORY_TID : OB_ALL_TABLE_TID;
  TableState *target = nullptr;
  int ret = prepare_write(inner, target);
  SchemaCatalogRow fields;
  ObNewRow row;
  if (OB_SUCC(ret)) { ret = fields.init(target->schema, state_->statement_time); }
  if (OB_SUCC(ret)) { ret = SchemaCatalogFields::table(table, update_object_status_ignore_version, fields, history); }
  if (OB_SUCC(ret) && history) { ret = fields.add_column("is_deleted", 0); }
  if (OB_SUCC(ret)) { ret = fields.finish(row); }
  if (OB_SUCC(ret)) { ret = insert(inner, row); }
  return ret;
}

int NativeSchemaCatalog::insert_column(const ObColumnSchemaV2 &column, bool history)
{
  const uint64_t inner = history ? OB_ALL_COLUMN_HISTORY_TID : OB_ALL_COLUMN_TID;
  TableState *target = nullptr;
  int ret = prepare_write(inner, target);
  SchemaCatalogRow fields;
  ObNewRow row;
  if (OB_SUCC(ret)) { ret = fields.init(target->schema, state_->statement_time); }
  if (OB_SUCC(ret)) { ret = SchemaCatalogFields::column(column, fields, history); }
  if (OB_SUCC(ret) && history) { ret = fields.add_column("is_deleted", 0); }
  if (OB_SUCC(ret)) { ret = fields.finish(row); }
  if (OB_SUCC(ret)) { ret = insert(inner, row); }
  return ret;
}

int NativeSchemaCatalog::insert_columns(const ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  for (auto it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
    ObColumnSchemaV2 column;
    if (it == nullptr || *it == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else { ret = column.assign(**it); }
    if (OB_SUCC(ret)) {
      column.set_schema_version(table.get_schema_version());
      column.set_table_id(table.get_table_id());
      ret = insert_column(column, false);
    }
    if (OB_SUCC(ret)) { ret = insert_column(column, true); }
  }
  return ret;
}

int NativeSchemaCatalog::insert(uint64_t inner_table_id, const ObNewRow &row)
{
  bool existed = false;
  return mutate(inner_table_id, row, Mutation::INSERT, existed);
}

int NativeSchemaCatalog::update(uint64_t inner_table_id, const ObNewRow &row)
{
  bool existed = false;
  return mutate(inner_table_id, row, Mutation::UPDATE, existed);
}

int NativeSchemaCatalog::erase(uint64_t inner_table_id, const ObRowkey &key, bool &existed)
{
  ObNewRow row;
  row.assign(const_cast<ObObj *>(key.get_obj_ptr()), key.get_obj_cnt());
  return mutate(inner_table_id, row, Mutation::ERASE, existed);
}

int NativeSchemaCatalog::mutate(uint64_t inner_table_id, const ObNewRow &row,
    Mutation operation, bool &existed)
{
  existed = false;
  auto &s = *state_;
  TableState *base = nullptr;
  int ret = prepare_write(inner_table_id, base);
  ObSEArray<TableState *, 4> indexes;
  if (OB_SUCC(ret) && (base->schema.is_index_table() || row.cells_ == nullptr
      || row.projector_size_ != 0 || row.count_ != (operation == Mutation::ERASE
          ? base->schema.get_rowkey_column_num() : base->columns.count()))) {
    ret = OB_INVALID_ARGUMENT;
  }
  int64_t count = 0;
  const uint64_t *ids = share::fixed_inner_table_indexes(inner_table_id, count);
  // Admit every physical target before modifying any row. A missing or
  // unsupported index must fail the whole owner, never leave an unmaintained index.
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    TableState *index = nullptr;
    if (OB_FAIL(prepare_write(ids[i], index))) {
    } else if (!index->schema.is_index_table() || index->schema.get_data_table_id() != inner_table_id) {
      ret = OB_ERR_UNEXPECTED;
    } else { ret = indexes.push_back(index); }
  }
  ObArenaAllocator allocator(ObMemAttr("CatalogMutation"));
  ObNewRow old;
  if (OB_SUCC(ret) && operation != Mutation::INSERT) {
    const ObRowkey key(row.cells_, base->schema.get_rowkey_column_num());
    ret = s.native.get_for_update(s.transaction, base->table, key, allocator, old);
    existed = ret == OB_SUCCESS;
    if (ret == OB_ENTRY_NOT_EXIST && operation == Mutation::ERASE) { return OB_SUCCESS; }
  }
  if (OB_SUCC(ret)) {
    if (operation == Mutation::INSERT) { ret = s.native.insert(s.transaction, base->table, row); }
    else if (operation == Mutation::UPDATE) { ret = s.native.update(s.transaction, base->table, row); }
    else {
      ret = s.native.erase(s.transaction, base->table,
          ObRowkey(row.cells_, base->schema.get_rowkey_column_num()), existed);
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < indexes.count(); ++i) {
    auto &index = *indexes.at(i);
    ObSEArray<ObObj, 8> old_cells, new_cells;
    ObNewRow previous, next;
    if (operation != Mutation::INSERT && OB_FAIL(base->project(old, index, old_cells))) {
    } else if (operation != Mutation::ERASE && OB_FAIL(base->project(row, index, new_cells))) {
    }
    previous.assign(old_cells.get_data(), old_cells.count());
    next.assign(new_cells.get_data(), new_cells.count());
    bool unchanged = operation == Mutation::UPDATE;
    for (int64_t j = 0; OB_SUCC(ret) && unchanged && j < previous.count_; ++j) {
      const auto &a = previous.cells_[j];
      const auto &b = next.cells_[j];
      // A collation-equal key may still change the bytes returned by a
      // covering index (e.g. a case-only rename).
      unchanged = a.is_string_type() && b.is_string_type()
          ? a.get_string() == b.get_string() : a == b;
    }
    if (OB_SUCC(ret) && unchanged) { continue; }
    if (OB_SUCC(ret) && operation != Mutation::INSERT) {
      ret = s.native.erase_index_row(s.transaction, index.table, previous);
    }
    if (OB_SUCC(ret) && operation != Mutation::ERASE) {
      ret = s.native.insert(s.transaction, index.table, next);
    }
  }
  return ret;
}

int NativeSchemaCatalog::close()
{
  auto &s = *state_;
  if (s.cursors != 0) { return OB_INVALID_ARGUMENT; }
  const int ret = !s.transaction.is_active() ? OB_SUCCESS
      : s.transaction.is_borrowed() ? s.native.detach(s.transaction) : s.native.rollback(s.transaction);
  if (ret == OB_SUCCESS) { s.writers.clear(); s.writable = false; s.view.reset(); }
  return ret;
}

const ns::NamespaceCatalogViews::Handle &NativeSchemaCatalog::view() const
{ return state_->view; }
} // namespace observer
} // namespace oceanbase
