/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SHARE_SCHEMA
#include "observer/schema/native_schema_catalog.h"
#include "observer/schema/ob_schema_retrieve_utils.h"
#include "share/schema/schema_catalog_fields.h"
#include "share/schema/schema_catalog_row.h"
#include "share/schema/ob_dependency_info.h"
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
#include "storage/compaction/ob_freeze_info_mgr.h"
#include "namespace/namespace.h"

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

int NativeSchemaCatalog::begin_read(int64_t deadline, bool weak)
{
  return begin_read_with_snapshot(deadline, [&](share::SCN &snapshot) {
    return weak ? state_->transactions.get_weak_read_snapshot_version(-1, snapshot)
                : state_->transactions.get_read_snapshot_version(deadline, snapshot);
  });
}

int NativeSchemaCatalog::begin_read_with_snapshot(int64_t deadline,
    const std::function<int(share::SCN &)> &acquire)
{
  auto &s = *state_;
  if (s.transaction.is_active() || s.view) { return OB_INIT_TWICE; }
  if (!acquire) { return OB_INVALID_ARGUMENT; }
  int ret = s.native.begin_read(s.transaction, deadline, [&](share::SCN &snapshot) {
    return NamespaceForkKernelPrototype::acquire_read_view(s.namespace_id,
        [&](share::SCN &selected) {
      const int rc = acquire(selected);
      if (rc == OB_SUCCESS) { snapshot = selected; }
      return rc;
    }, s.view);
  });
  if (ret != OB_SUCCESS) { s.view.reset(); }
  return ret;
}

int NativeSchemaCatalog::begin_read_at(int64_t deadline, const share::SCN &target)
{
  if (!target.is_valid() || target.is_min() || target.is_max()) { return OB_INVALID_ARGUMENT; }
  auto *freezes = share::server_service<storage::ObFreezeInfoMgr>();
  if (freezes == nullptr) { return OB_NOT_INIT; }
  return begin_read_with_snapshot(deadline, [&](share::SCN &snapshot) {
    // Register the reader first, then validate the durable requirement. The
    // retention sampler observes these in the reverse order.
    share::SCN retained, readable;
    int rc = freezes->get_schema_history_retention(retained);
    if (rc == OB_SUCCESS && target < retained) { rc = OB_SNAPSHOT_DISCARDED; }
    if (rc == OB_SUCCESS) { rc = state_->transactions.get_weak_read_snapshot_version(-1, readable); }
    if (rc == OB_SUCCESS && target > readable) { rc = OB_EAGAIN; }
    if (rc == OB_SUCCESS) { snapshot = target; }
    return rc;
  });
}

int NativeSchemaCatalog::load_merge_schema(const storage::ObTablet &tablet,
    const share::SCN &requested, bool choose_current, ObIAllocator &allocator,
    share::SCN &selected, storage::ObStorageSchema &out)
{
  selected.reset();
  auto *access = share::server_service<storage::ObAccessService>();
  auto *transactions = share::server_service<transaction::ObTransService>();
  const auto &meta = tablet.get_tablet_meta();
  if (!requested.is_valid() || requested.is_min() || requested.is_max()
      || !ns::NamespaceObjectKey::is_encoded(meta.tablet_id_.id())) { return OB_INVALID_ARGUMENT; }
  if (access == nullptr || transactions == nullptr) { return OB_NOT_INIT; }
  const uint64_t owner = ns::NamespaceObjectKey::encoded_namespace(meta.tablet_id_.id());
  NativeSchemaCatalog catalog(owner, *access, *transactions);
  const int64_t deadline = ObTimeUtility::current_time() + 10L * 1000 * 1000;
  int ret = choose_current ? catalog.begin_read(deadline, true) : catalog.begin_read_at(deadline, requested);
  if (ret == OB_SUCCESS && catalog.view()->entry().snapshot < requested.get_val_for_tx()) {
    ret = OB_EAGAIN;
  }
  if (ret == OB_SUCCESS) { ret = catalog.get_storage_schema(meta.create_table_id_, allocator, out); }
  if (ret == OB_SUCCESS) { ret = selected.convert_for_tx(catalog.view()->entry().snapshot); }
  const int closed = catalog.close();
  return ret == OB_SUCCESS ? closed : ret;
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
    const int ret = catalog.open_prefix(inner_id, prefix, cursor_, reverse);
    return ret == OB_SUCCESS ? load_columns() : ret;
  }
  int open_range(NativeSchemaCatalog &catalog, uint64_t inner_id,
      const ObNewRange &range, bool reverse = false)
  {
    version_ = INT64_MAX;
    const int ret = catalog.open_scan(inner_id, range, cursor_, reverse);
    return ret == OB_SUCCESS ? load_columns() : ret;
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
  int load_columns()
  {
    ObSEArray<ObColDesc, 8> columns;
    int ret = cursor_.schema()->get_column_ids(columns);
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      positions_.emplace(columns.at(i).col_id_, i);
    }
    return ret;
  }
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

int read_dependency(CatalogRows &result, ObDependencyInfo &dep)
{
  int ret = OB_SUCCESS;
  dep.reset();

  EXTRACT_INT_FIELD_TO_CLASS_MYSQL(result, dep_obj_id, dep, uint64_t);
  EXTRACT_INT_FIELD_TO_CLASS_MYSQL(result, dep_obj_type, dep, ObObjectType);
  EXTRACT_INT_FIELD_TO_CLASS_VALUE_MYSQL(result, dep_order, order, dep, uint64_t);
  EXTRACT_INT_FIELD_TO_CLASS_MYSQL(result, schema_version, dep, int64_t);
  EXTRACT_INT_FIELD_TO_CLASS_MYSQL(result, dep_timestamp, dep, int64_t);
  EXTRACT_INT_FIELD_TO_CLASS_MYSQL(result, ref_obj_type, dep, ObObjectType);
  EXTRACT_INT_FIELD_TO_CLASS_MYSQL(result, ref_obj_id, dep, uint64_t);
  EXTRACT_INT_FIELD_TO_CLASS_MYSQL(result, ref_timestamp, dep, int64_t);
  EXTRACT_INT_FIELD_TO_CLASS_MYSQL_WITH_DEFAULT_VALUE(result, dep_obj_owner_id, dep, uint64_t, true, false, OB_INVALID_ID);
  EXTRACT_INT_FIELD_TO_CLASS_MYSQL(result, property, dep, uint64_t);
  EXTRACT_VARCHAR_FIELD_TO_CLASS_MYSQL_SKIP_RET(result, dep_attrs, dep);
  EXTRACT_VARCHAR_FIELD_TO_CLASS_MYSQL_SKIP_RET(result, dep_reason, dep);
  EXTRACT_VARCHAR_FIELD_TO_CLASS_MYSQL_SKIP_RET(result, ref_obj_name, dep);
  return ret;
}

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
    // The core table's complete definition is compiled. Its persisted metadata
    // row still participates in name and version queries, as in the SQL view.
    if (rc != OB_SUCCESS || v > version || (with_columns && id == OB_ALL_CORE_TABLE_TID)) { return rc; }
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

bool catalog_name_equal(const ObString &a, const ObString &b, bool insensitive)
{
  return ObCharset::strcmp(insensitive ? CS_TYPE_UTF8MB4_GENERAL_CI : CS_TYPE_UTF8MB4_BIN, a, b) == 0;
}

int name_key(uint64_t inner_id, const char *column_name, const ObString &name, ObObj &key)
{
  const auto creator = share::fixed_inner_table_creator(inner_id);
  if (creator == nullptr) { return OB_INVALID_ARGUMENT; }
  ObTableSchema fixed;
  int ret = creator(fixed);
  const auto *column = OB_SUCC(ret) ? fixed.get_column_schema(ObString::make_string(column_name)) : nullptr;
  if (OB_SUCC(ret) && column == nullptr) { ret = OB_ERR_COLUMN_NOT_FOUND; }
  if (OB_SUCC(ret)) {
    key.set_meta_type(column->get_meta_type());
    key.set_string(column->get_data_type(), name);
  }
  return ret;
}

struct TableLookupRow
{
  uint64_t id = OB_INVALID_ID;
  uint64_t database = OB_INVALID_ID;
  uint64_t session = 0;
  ObString name;
  ObTableType type = MAX_TABLE_TYPE;
  ObTableMode mode;
  int64_t version = OB_INVALID_VERSION;
  bool user_visible() const
  { return !mode.is_user_hidden_table() && !is_index_table(type) && !is_aux_lob_table(type); }
};

int read_lookup_row(const CatalogRows &rows, TableLookupRow &out)
{
  int64_t type = MAX_TABLE_TYPE, mode = 0;
  int ret = rows.get_uint("table_id", out.id);
  if (OB_FAIL(ret)
      || OB_FAIL(rows.get_uint("database_id", out.database))
      || OB_FAIL(rows.get_uint("session_id", out.session))
      || OB_FAIL(rows.get_varchar("table_name", out.name))
      || OB_FAIL(rows.get_int("table_type", type))
      || OB_FAIL(rows.get_int("table_mode", mode))
      || OB_FAIL(rows.get_int("schema_version", out.version))) {
  } else {
    out.type = static_cast<ObTableType>(type);
    out.mode.mode_ = static_cast<uint32_t>(mode);
  }
  return ret;
}

// Visit only the columns needed for name resolution. Callback strings borrow
// the current row; no table columns, partitions or relationship schemas load.
int visit_named_tables(NativeSchemaCatalog &catalog, uint64_t database, const ObString &name,
    const std::function<int(const TableLookupRow &)> &visitor)
{
  int ret = OB_SUCCESS;
  if (is_oceanbase_sys_database_id(database)) {
    ObArray<ObTableSchema> core;
    ret = read_core_tables(catalog, INT64_MAX, core, false);
    for (int64_t i = 0; OB_SUCC(ret) && i < core.count(); ++i) {
      const auto &table = core.at(i);
      if (table.get_database_id() != database) { continue; }
      TableLookupRow row;
      row.id = table.get_table_id(); row.database = table.get_database_id();
      row.session = table.get_session_id(); row.name = table.get_table_name_str();
      row.type = table.get_table_type(); row.mode = table.get_table_mode_struct(); row.version = table.get_schema_version();
      ret = visitor(row);
    }
  }
  ObObj keys[2];
  keys[0].set_int(database);
  if (OB_SUCC(ret)) { ret = name_key(OB_ALL_TABLE_IDX_DB_TB_NAME_TID, "table_name", name, keys[1]); }
  std::set<uint64_t> ids;
  if (OB_SUCC(ret)) {
    ret = collect_related_ids(catalog, OB_ALL_TABLE_IDX_DB_TB_NAME_TID, ObRowkey(keys, 2),
                             INT64_MAX, "table_id", ids);
  }
  for (const auto id : ids) {
    if (OB_FAIL(ret)) { break; }
    ObObj key; key.set_int(id);
    CatalogRows rows;
    TableLookupRow row;
    if (OB_FAIL(rows.open(catalog, OB_ALL_TABLE_TID, ObRowkey(&key, 1), INT64_MAX, false, 0, false, {}, false))) {
    } else if (OB_FAIL(rows.next())) {
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    } else if (OB_FAIL(read_lookup_row(rows, row))) {
    } else if (row.database == database) { ret = visitor(row); }
  }
  return ret;
}

int find_named_relation(NativeSchemaCatalog &catalog, uint64_t database, const ObString &name,
    bool foreign_key, uint64_t &out)
{
  out = OB_INVALID_ID;
  if (database == OB_INVALID_ID || name.empty()) { return OB_INVALID_ARGUMENT; }
  const uint64_t index = foreign_key ? OB_ALL_FOREIGN_KEY_IDX_FK_NAME_TID : OB_ALL_CONSTRAINT_IDX_CST_NAME_TID;
  const char *name_column = foreign_key ? "foreign_key_name" : "constraint_name";
  const char *id_column = foreign_key ? "foreign_key_id" : "constraint_id";
  ObObj key;
  int ret = name_key(index, name_column, name, key);
  std::set<std::pair<uint64_t, uint64_t>> candidates;
  {
    CatalogRows rows;
    if (OB_SUCC(ret)) { ret = rows.open(catalog, index, ObRowkey(&key, 1), INT64_MAX); }
    while (OB_SUCC(ret) && OB_SUCC(rows.next())) {
      uint64_t owner = OB_INVALID_ID, id = OB_INVALID_ID;
      if (OB_FAIL(rows.get_uint(id_column, id))) {
      } else if (!foreign_key && OB_FAIL(rows.get_uint("table_id", owner))) {
      } else { candidates.emplace(owner, id); }
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  }
  for (const auto &candidate : candidates) {
    if (OB_FAIL(ret) || out != OB_INVALID_ID) { break; }
    uint64_t owner = candidate.first;
    bool matches = false;
    {
      ObObj keys[2]; keys[0].set_int(owner); keys[1].set_int(candidate.second);
      const ObRowkey prefix(foreign_key ? &keys[1] : keys, foreign_key ? 1 : 2);
      CatalogRows rows;
      ObString stored_name;
      if (OB_FAIL(rows.open(catalog, foreign_key ? OB_ALL_FOREIGN_KEY_TID : OB_ALL_CONSTRAINT_TID,
                           prefix, INT64_MAX, false, 0, false, {}, false))) {
      } else if (OB_FAIL(rows.next())) {
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      } else if (OB_FAIL(rows.get_varchar(name_column, stored_name))) {
      } else if (foreign_key && OB_FAIL(rows.get_uint("child_table_id", owner))) {
      } else { matches = catalog_name_equal(name, stored_name, true); }
    }
    if (OB_SUCC(ret) && matches) {
      key.set_int(owner);
      CatalogRows rows;
      TableLookupRow table;
      if (OB_FAIL(rows.open(catalog, OB_ALL_TABLE_TID, ObRowkey(&key, 1), INT64_MAX, false, 0, false, {}, false))) {
      } else if (OB_FAIL(rows.next())) {
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      } else if (OB_FAIL(read_lookup_row(rows, table))) {
      } else if (table.database == database && table.user_visible()
                 && (foreign_key || !is_mysql_tmp_table(table.type))) { out = candidate.second; }
    }
  }
  return ret;
}

template<typename Table>
int read_foreign_keys(NativeSchemaCatalog &catalog, int64_t version, bool history, Table &definition)
{
  Table *table = &definition;
  const uint64_t table_id = table->get_table_id();
  ObObj id;
  id.set_int(table_id);
  const ObRowkey prefix(&id, 1);
  auto open = [&](CatalogRows &rows, uint64_t current_id, uint64_t history_id,
      const ObRowkey &key, bool reverse, int64_t minimum, bool latest, CatalogRows::Filter filter = {}) {
    return rows.open(catalog, history ? history_id : current_id, key, version,
                     reverse, history ? minimum : 0, history && latest, std::move(filter), history);
  };
  int ret = OB_SUCCESS;
  {
    std::set<uint64_t> ids;
    if (OB_SUCC(ret)) {
      ret = collect_related_ids(catalog, history ? OB_ALL_FOREIGN_KEY_HISTORY_IDX_FK_HIS_CHILD_TID_TID
                                              : OB_ALL_FOREIGN_KEY_IDX_FK_CHILD_TID_TID,
                                prefix, version, "foreign_key_id", ids);
    }
    if (OB_SUCC(ret)) {
      ret = collect_related_ids(catalog, history ? OB_ALL_FOREIGN_KEY_HISTORY_IDX_FK_HIS_PARENT_TID_TID
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
  return ret;
}

template<typename Table>
int read_mock_parent_rows(NativeSchemaCatalog &catalog, int64_t version, ObIArray<Table> &out,
    const ObIArray<uint64_t> *table_ids)
{
  if (version <= 0) { return OB_INVALID_ARGUMENT; }
  auto read = [&](const ObRowkey &prefix) {
    CatalogRows rows;
    const int rc = rows.open(catalog, OB_ALL_MOCK_FK_PARENT_TABLE_HISTORY_TID,
                            prefix, version, true, 0, true);
    return rc == OB_SUCCESS ? ObSchemaRetrieveUtils::retrieve_mock_fk_parent_table_schema(rows, out) : rc;
  };
  if (table_ids == nullptr) { return read(ObRowkey()); }
  std::set<uint64_t, std::greater<uint64_t>> ids;
  for (int64_t i = 0; i < table_ids->count(); ++i) {
    if (table_ids->at(i) == OB_INVALID_ID) { return OB_INVALID_ARGUMENT; }
    ids.insert(table_ids->at(i));
  }
  int ret = OB_SUCCESS;
  for (const auto id : ids) {
    if (OB_FAIL(ret)) { break; }
    ObObj key;
    key.set_int(id);
    ret = read(ObRowkey(&key, 1));
  }
  return ret;
}

template<typename Table>
int read_trigger_rows(NativeSchemaCatalog &catalog, int64_t version, ObIArray<Table> &out,
    const ObIArray<uint64_t> *table_ids)
{
  if (version <= 0) { return OB_INVALID_ARGUMENT; }
  auto read = [&](const ObRowkey &prefix) {
    CatalogRows rows;
    const int rc = rows.open(catalog, OB_ALL_TRIGGER_HISTORY_TID,
                            prefix, version, true, 0, true);
    return rc == OB_SUCCESS ? ObSchemaRetrieveUtils::retrieve_trigger_schema(rows, out) : rc;
  };
  if (table_ids == nullptr) { return read(ObRowkey()); }
  std::set<uint64_t, std::greater<uint64_t>> ids;
  for (int64_t i = 0; i < table_ids->count(); ++i) {
    if (table_ids->at(i) == OB_INVALID_ID) { return OB_INVALID_ARGUMENT; }
    ids.insert(table_ids->at(i));
  }
  int ret = OB_SUCCESS;
  for (const auto id : ids) {
    if (OB_FAIL(ret)) { break; }
    ObObj key;
    key.set_int(id);
    ret = read(ObRowkey(&key, 1));
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
  return read_table(table_id, version, allocator, out, source, false);
}

int NativeSchemaCatalog::get_storage_schema(uint64_t table_id,
    ObIAllocator &allocator, storage::ObStorageSchema &out)
{
  ObArenaAllocator temporary(ObMemAttr("CatalogLayout"));
  ObTableSchema *table = nullptr;
  int ret = read_table(table_id, INT64_MAX, temporary, table, TableSource::HISTORY, true);
  if (ret == OB_SUCCESS) { ret = out.init(allocator, *table); }
  if (table != nullptr) { table->~ObTableSchema(); }
  return ret;
}

int NativeSchemaCatalog::get_table_ids(ObIArray<uint64_t> &out)
{
  out.reuse();
  std::set<uint64_t> ids{OB_ALL_CORE_TABLE_TID};
  ObArray<ObTableSchema> core;
  int ret = read_core_tables(*this, INT64_MAX, core, false);
  for (int64_t i = 0; OB_SUCC(ret) && i < core.count(); ++i) {
    ids.insert(core.at(i).get_table_id());
  }
  CatalogRows rows;
  if (OB_SUCC(ret)) {
    ret = rows.open(*this, OB_ALL_TABLE_TID, ObRowkey(), INT64_MAX,
        false, 0, false, {}, false);
  }
  while (OB_SUCC(ret) && OB_SUCC(rows.next())) {
    uint64_t id = OB_INVALID_ID;
    if (OB_SUCC(ret = rows.get_uint("table_id", id))) { ids.insert(id); }
  }
  if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  for (uint64_t id : ids) {
    if (OB_FAIL(ret)) { break; }
    ret = out.push_back(id);
  }
  return ret;
}

int NativeSchemaCatalog::read_table(uint64_t table_id, int64_t version,
    ObIAllocator &allocator, ObTableSchema *&out, TableSource source, bool storage_only)
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
  if (storage_only) {
    if (OB_SUCC(ret)) { out = table; }
    else if (table != nullptr) { table->~ObTableSchema(); }
    return ret;
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
  if (OB_SUCC(ret)) { ret = read_foreign_keys(*this, version, history, *table); }
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
  if (OB_SUCC(ret)) {
    ObSEArray<ObAuxTableMetaInfo, 8> auxiliary;
    ret = get_aux_tables(table_id, version, auxiliary, source);
    for (int64_t i = 0; OB_SUCC(ret) && i < auxiliary.count(); ++i) {
      const auto &aux = auxiliary.at(i);
      if (aux.table_type_ == USER_INDEX) { ret = table->add_simple_index_info(aux); }
      else if (aux.table_type_ == AUX_LOB_META) { table->set_aux_lob_meta_tid(aux.table_id_); }
      else if (aux.table_type_ == AUX_LOB_PIECE) { table->set_aux_lob_piece_tid(aux.table_id_); }
    }
  }
  if (OB_SUCC(ret)) { out = table; }
  else if (table != nullptr) { table->~ObTableSchema(); }
  return ret;
}

int NativeSchemaCatalog::get_aux_tables(uint64_t table_id, int64_t version,
    ObIArray<ObAuxTableMetaInfo> &out, TableSource source)
{
  out.reset();
  const bool history = source == TableSource::HISTORY;
  if (version <= 0 || table_id == OB_INVALID_ID || (!history && version != INT64_MAX)) {
    return OB_INVALID_ARGUMENT;
  }
  ObObj key;
  key.set_int(table_id);
  std::set<uint64_t> ids;
  int ret = collect_related_ids(*this, history ? OB_ALL_TABLE_HISTORY_IDX_DATA_TABLE_ID_TID
                                              : OB_ALL_TABLE_IDX_DATA_TABLE_ID_TID,
                                ObRowkey(&key, 1), version, "table_id", ids);
  // The index supplies candidates. Recheck each latest visible base row so
  // deleted or reassigned auxiliary tables do not survive through old keys.
  for (const uint64_t related : ids) {
    if (OB_FAIL(ret)) { break; }
    key.set_int(related);
    CatalogRows rows;
    if (OB_FAIL(rows.open(*this, history ? OB_ALL_TABLE_HISTORY_TID : OB_ALL_TABLE_TID,
                         ObRowkey(&key, 1), version, true, 0, history,
                         [table_id](const CatalogRows &row, bool &accept) {
      int64_t data_table = 0, deleted = 0;
      int rc = row.get_int("is_deleted", deleted);
      if (rc == OB_SUCCESS && !deleted) { rc = row.get_int("data_table_id", data_table); }
      accept = !deleted && data_table == table_id;
      return rc;
    }, history))) {
    } else { ret = ObSchemaRetrieveUtils::retrieve_aux_tables(rows, out); }
  }
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

int NativeSchemaCatalog::get_schema_watermarks(SchemaWatermarks &out)
{
  out = SchemaWatermarks{};
  int64_t rows = 0;
  const int ret = read_core_rows(*this, "__all_global_stat", [&](const share::ObCoreTableProxy::Row &row) {
    int rc = ++rows == 1 ? OB_SUCCESS : OB_ERR_UNEXPECTED;
    if (rc == OB_SUCCESS) { rc = row.get_int("core_schema_version", out.core); }
    if (rc == OB_SUCCESS) { rc = row.get_int("sys_schema_version", out.system); }
    if (rc == OB_SUCCESS) { rc = row.get_int("normal_schema_version", out.normal); }
    if (rc == OB_SUCCESS) { rc = row.get_int("baseline_schema_version", out.baseline); }
    return rc;
  });
  return ret != OB_SUCCESS ? ret : rows == 0 ? OB_EMPTY_RESULT : OB_SUCCESS;
}

int NativeSchemaCatalog::advance_schema_watermark(SchemaWatermark kind, int64_t version)
{
  const char *name = nullptr;
  switch (kind) {
    case SchemaWatermark::CORE: name = "core_schema_version"; break;
    case SchemaWatermark::SYSTEM: name = "sys_schema_version"; break;
    case SchemaWatermark::NORMAL: name = "normal_schema_version"; break;
    case SchemaWatermark::BASELINE: name = "baseline_schema_version"; break;
    default: return OB_INVALID_ARGUMENT;
  }
  if (kind == SchemaWatermark::BASELINE ? version < -1 : version <= 0) { return OB_INVALID_ARGUMENT; }
  TableState *target = nullptr;
  int ret = prepare_write(OB_ALL_CORE_TABLE_TID, target);
  ObObj prefix;
  prefix.set_varchar("__all_global_stat");
  prefix.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ObArenaAllocator allocator(ObMemAttr("SchemaWatermark"));
  ObSEArray<ObRowkey, 16> keys;
  Cursor cursor;
  const ObNewRow *row = nullptr;
  int64_t logical_row = OB_INVALID_INDEX;
  if (OB_SUCC(ret)) { ret = open_prefix(OB_ALL_CORE_TABLE_TID, ObRowkey(&prefix, 1), cursor); }
  while (OB_SUCC(ret) && OB_SUCC(ret = cursor.next(row))) {
    // Fixed core-table key: table_name, row_id, column_name. Schema watermarks
    // only change values after bootstrap; the singleton's key set is stable.
    ObRowkey key;
    const int64_t id = row->cells_[1].get_int();
    if (logical_row != OB_INVALID_INDEX && logical_row != id) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(ObRowkey(row->cells_, 3).deep_copy(key, allocator))) {
    } else {
      logical_row = id;
      ret = keys.push_back(key);
    }
  }
  if (ret == OB_ITER_END) { ret = keys.empty() ? OB_EMPTY_RESULT : OB_SUCCESS; }
  const int closed = cursor.close();
  if (OB_SUCC(ret)) { ret = closed; }
  int64_t value_position = -1;
  if (OB_SUCC(ret)) {
    const auto *column = target->schema.get_column_schema("column_value");
    if (column == nullptr) { ret = OB_ERR_UNEXPECTED; }
    for (int64_t i = 0; OB_SUCC(ret) && i < target->columns.count(); ++i) {
      if (target->columns.at(i).col_id_ == column->get_column_id()) { value_position = i; break; }
    }
    if (OB_SUCC(ret) && value_position < 0) { ret = OB_ERR_UNEXPECTED; }
  }
  int64_t previous = 0;
  bool found = false;
  // Preserve ObGlobalStatProxy's FOR UPDATE order across all cells. Other
  // metadata writers still take these locks, and DDL takes its epoch first.
  for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
    ObNewRow current;
    ret = state_->native.get_for_update(state_->transaction, target->table, keys.at(i), allocator, current);
    if (OB_SUCC(ret) && keys.at(i).get_obj_ptr()[2].get_string() == name) {
      const auto &value = current.cells_[value_position];
      share::ObCoreTableCell cell;
      cell.name_ = ObString::make_string(name);
      if (value.is_null()) { ret = OB_ERR_NULL_VALUE; }
      else { cell.value_ = value.get_string(); }
      ObSEArray<share::ObCoreTableCell, 1> cells;
      share::ObCoreTableCell stored;
      share::ObCoreTableProxy::Row decoded;
      if (OB_SUCC(ret)) { ret = stored.assign(allocator, cell); }
      if (OB_SUCC(ret)) { ret = cells.push_back(stored); }
      if (OB_SUCC(ret)) { ret = decoded.init(logical_row, allocator, cells); }
      if (OB_SUCC(ret)) { ret = decoded.get_int(name, previous); }
      if (OB_SUCC(ret)) { found = true; }
    }
  }
  if (OB_SUCC(ret) && !found) { ret = OB_ERR_NULL_VALUE; }
  if (OB_SUCC(ret) && version > previous) {
    ObSqlString value;
    bool matched = false;
    if (OB_FAIL(value.append_fmt("%ld", version))) {
    } else {
      ret = update_fields(OB_ALL_CORE_TABLE_TID, [&](SchemaCatalogRow &fields) {
        int rc = fields.add_pk_column("table_name", "__all_global_stat");
        if (rc == OB_SUCCESS) { rc = fields.add_pk_column("row_id", logical_row); }
        if (rc == OB_SUCCESS) { rc = fields.add_pk_column("column_name", name); }
        if (rc == OB_SUCCESS) { rc = fields.add_column("column_value", value.string()); }
        if (rc == OB_SUCCESS) { rc = fields.add_gmt_modified(); }
        return rc;
      }, matched);
      if (OB_SUCC(ret) && !matched) { ret = OB_ERR_UNEXPECTED; }
    }
  }
  return ret;
}

int NativeSchemaCatalog::scan_core_cells(const char *table_name, bool for_update,
    const RowVisitor &visitor)
{
  if (table_name == nullptr || !visitor) { return OB_INVALID_ARGUMENT; }
  TableState *target = nullptr;
  int ret = for_update ? prepare_write(OB_ALL_CORE_TABLE_TID, target) : OB_SUCCESS;
  ObObj prefix;
  prefix.set_varchar(table_name);
  prefix.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  Cursor cursor;
  ObArenaAllocator allocator(ObMemAttr("CoreCatalogKeys"));
  ObArray<ObRowkey> keys;
  bool stop = false;
  if (OB_SUCC(ret) && for_update) {
    // FOR UPDATE enumerates current keys too: a long-running owner's older
    // catalog snapshot must not hide rows committed before this statement.
    ObObj lower[3], upper[3];
    lower[0] = upper[0] = prefix;
    lower[1].set_min_value(); lower[2].set_min_value();
    upper[1].set_max_value(); upper[2].set_max_value();
    ObNewRange range;
    range.start_key_.assign(lower, 3);
    range.end_key_.assign(upper, 3);
    range.border_flag_.set_inclusive_start(); range.border_flag_.set_inclusive_end();
    ret = state_->native.scan_current(state_->transaction, target->table, range,
        [&](const ObNewRow &row, bool &) {
      ObRowkey key;
      const int rc = ObRowkey(row.cells_, 3).deep_copy(key, allocator);
      return rc == OB_SUCCESS ? keys.push_back(key) : rc;
    });
  } else if (OB_SUCC(ret)) { ret = open_prefix(OB_ALL_CORE_TABLE_TID, ObRowkey(&prefix, 1), cursor); }
  const ObNewRow *row = nullptr;
  while (OB_SUCC(ret) && !for_update && !stop && OB_SUCC(ret = cursor.next(row))) {
    ret = visitor(*cursor.schema(), *row, stop);
  }
  if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  const int closed = cursor.close();
  if (OB_SUCC(ret)) { ret = closed; }
  for (int64_t i = 0; OB_SUCC(ret) && !stop && i < keys.count(); ++i) {
    ObNewRow current;
    ret = state_->native.get_for_update(state_->transaction, target->table, keys.at(i), allocator, current);
    if (ret == OB_ENTRY_NOT_EXIST) { ret = OB_SUCCESS; }
    else if (OB_SUCC(ret)) { ret = visitor(target->schema, current, stop); }
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

int NativeSchemaCatalog::get_table_schema_versions(const ObIArray<uint64_t> &table_ids,
    ObIArray<ObSchemaIdVersion> &out)
{
  if (table_ids.empty()) { return OB_INVALID_ARGUMENT; }
  std::set<uint64_t> ids;
  bool has_core = false;
  for (int64_t i = 0; i < table_ids.count(); ++i) {
    if (table_ids.at(i) == OB_INVALID_ID) { return OB_INVALID_ARGUMENT; }
    ids.insert(table_ids.at(i));
    has_core |= is_core_table(table_ids.at(i));
  }
  auto append = [&](uint64_t id, int64_t version) {
    ObSchemaIdVersion pair;
    const int rc = pair.init(id, version);
    return rc == OB_SUCCESS ? out.push_back(pair) : rc;
  };
  int ret = OB_SUCCESS;
  if (has_core) {
    ObArray<ObTableSchema> core;
    ret = read_core_tables(*this, INT64_MAX, core, false);
    for (int64_t i = 0; OB_SUCC(ret) && i < core.count(); ++i) {
      const auto &table = core.at(i);
      if (ids.count(table.get_table_id())) { ret = append(table.get_table_id(), table.get_schema_version()); }
    }
  }
  for (const auto id : ids) {
    if (OB_FAIL(ret)) { break; }
    if (is_core_table(id)) { continue; }
    ObObj key;
    key.set_int(id);
    CatalogRows rows;
    int64_t version = OB_INVALID_VERSION;
    if (OB_FAIL(rows.open(*this, OB_ALL_TABLE_TID, ObRowkey(&key, 1),
                         INT64_MAX, false, 0, false, {}, false))) {
    } else if (OB_FAIL(rows.next())) {
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    } else if (OB_FAIL(rows.get_int("schema_version", version))) {
    } else { ret = append(id, version); }
  }
  return ret;
}

int NativeSchemaCatalog::get_table_index_infos(ObIAllocator &allocator, uint64_t database_id,
    uint64_t data_table_id, ObIArray<ObIndexSchemaInfo> &out)
{
  out.reset();
  if (database_id == OB_INVALID_ID || data_table_id == OB_INVALID_ID) { return OB_INVALID_ARGUMENT; }
  auto append = [&](const ObString &name, uint64_t id, int64_t version, ObIndexType type) {
    ObString owned_name;
    ObIndexSchemaInfo info;
    int rc = ob_write_string(allocator, name, owned_name, true);
    if (rc == OB_SUCCESS) { rc = info.init(owned_name, id, version, type); }
    return rc == OB_SUCCESS ? out.push_back(info) : rc;
  };
  int ret = OB_SUCCESS;
  if (is_oceanbase_sys_database_id(database_id)) {
    ObArray<ObTableSchema> core;
    ret = read_core_tables(*this, INT64_MAX, core, false);
    for (int64_t i = 0; OB_SUCC(ret) && i < core.count(); ++i) {
      const auto &table = core.at(i);
      if (table.get_database_id() == database_id && table.get_data_table_id() == data_table_id
          && table.get_table_type() == USER_INDEX) {
        ret = append(table.get_table_name_str(), table.get_table_id(),
                     table.get_schema_version(), table.get_index_type());
      }
    }
  }
  ObObj key;
  key.set_int(data_table_id);
  std::set<uint64_t> ids;
  if (OB_SUCC(ret)) {
    ret = collect_related_ids(*this, OB_ALL_TABLE_IDX_DATA_TABLE_ID_TID,
                             ObRowkey(&key, 1), INT64_MAX, "table_id", ids);
  }
  // Close the index cursor before opening a base row. Both use this operation's
  // view, including its owner's uncommitted index/base changes.
  for (const auto id : ids) {
    if (OB_FAIL(ret)) { break; }
    key.set_int(id);
    CatalogRows rows;
    uint64_t database = OB_INVALID_ID, owner = OB_INVALID_ID;
    int64_t table_type = MAX_TABLE_TYPE, index_type = INDEX_TYPE_IS_NOT, version = OB_INVALID_VERSION;
    ObString name;
    if (OB_FAIL(rows.open(*this, OB_ALL_TABLE_TID, ObRowkey(&key, 1),
                         INT64_MAX, false, 0, false, {}, false))) {
    } else if (OB_FAIL(rows.next())) {
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    } else if (OB_FAIL(rows.get_uint("database_id", database))
               || OB_FAIL(rows.get_uint("data_table_id", owner))
               || OB_FAIL(rows.get_int("table_type", table_type))) {
    } else if (database != database_id || owner != data_table_id || table_type != USER_INDEX) {
    } else if (OB_FAIL(rows.get_varchar("table_name", name))
               || OB_FAIL(rows.get_int("schema_version", version))
               || OB_FAIL(rows.get_int("index_type", index_type))) {
    } else { ret = append(name, id, version, static_cast<ObIndexType>(index_type)); }
  }
  return ret;
}

int NativeSchemaCatalog::get_table_id(uint64_t database, uint64_t session, const ObString &name,
    bool insensitive, uint64_t &out, ObTableType &type, int64_t &version)
{
  out = OB_INVALID_ID; type = MAX_TABLE_TYPE; version = OB_INVALID_VERSION;
  if (database == OB_INVALID_ID || name.empty()) { return OB_INVALID_ARGUMENT; }
  bool system_name = false;
  int ret = ObSysTableChecker::is_sys_table_name(database, name, system_name);
  int best = 0;
  if (OB_SUCC(ret)) {
    ret = visit_named_tables(*this, database, name, [&](const TableLookupRow &row) {
      if (!row.user_visible() || (row.session != 0 && row.session != session)) { return OB_SUCCESS; }
      const bool normal_match = catalog_name_equal(name, row.name, insensitive);
      const bool inner_match = system_name && row.session == 0 && is_inner_table(row.id)
          && catalog_name_equal(name, row.name, true);
      const int rank = normal_match ? (row.session != 0 ? 3 : 2) : inner_match ? 1 : 0;
      if (rank > best) { best = rank; out = row.id; type = row.type; version = row.version; }
      return OB_SUCCESS;
    });
  }
  return ret;
}

int NativeSchemaCatalog::get_index_id(uint64_t database, const ObString &name, uint64_t &out)
{
  out = OB_INVALID_ID;
  if (database == OB_INVALID_ID || name.empty()) { return OB_INVALID_ARGUMENT; }
  return visit_named_tables(*this, database, name, [&](const TableLookupRow &row) {
    if (out == OB_INVALID_ID && row.type == USER_INDEX && catalog_name_equal(name, row.name, true)) { out = row.id; }
    return OB_SUCCESS;
  });
}

int NativeSchemaCatalog::get_constraint_id(uint64_t database, const ObString &name, uint64_t &out)
{ return find_named_relation(*this, database, name, false, out); }

int NativeSchemaCatalog::get_foreign_key_id(uint64_t database, const ObString &name, uint64_t &out)
{ return find_named_relation(*this, database, name, true, out); }

int NativeSchemaCatalog::get_mock_fk_parent_table_id(uint64_t database, const ObString &name, uint64_t &out)
{
  out = OB_INVALID_ID;
  if (database == OB_INVALID_ID || name.empty()) { return OB_INVALID_ARGUMENT; }
  // This fixed table has no name index. Preserve its byte-exact name policy.
  CatalogRows rows;
  int ret = rows.open(*this, OB_ALL_MOCK_FK_PARENT_TABLE_TID, ObRowkey(), INT64_MAX, false, 0, false, {}, false);
  while (OB_SUCC(ret) && out == OB_INVALID_ID && OB_SUCC(rows.next())) {
    uint64_t owner = OB_INVALID_ID;
    ObString stored;
    if (OB_FAIL(rows.get_uint("database_id", owner))) {
    } else if (owner != database) {
    } else if (OB_FAIL(rows.get_varchar("mock_fk_parent_table_name", stored))) {
    } else if (name == stored) { ret = rows.get_uint("mock_fk_parent_table_id", out); }
  }
  return ret == OB_ITER_END ? OB_SUCCESS : ret;
}

int NativeSchemaCatalog::get_mock_fk_parent_table_schema_versions(const ObIArray<uint64_t> &table_ids,
    ObIArray<ObSchemaIdVersion> &out)
{
  if (table_ids.empty()) { return OB_INVALID_ARGUMENT; }
  std::set<uint64_t> ids;
  for (int64_t i = 0; i < table_ids.count(); ++i) {
    if (table_ids.at(i) == OB_INVALID_ID) { return OB_INVALID_ARGUMENT; }
    ids.insert(table_ids.at(i));
  }
  int ret = OB_SUCCESS;
  for (const auto id : ids) {
    if (OB_FAIL(ret)) { break; }
    ObObj key; key.set_int(id);
    CatalogRows rows;
    int64_t version = OB_INVALID_VERSION;
    ObSchemaIdVersion pair;
    if (OB_FAIL(rows.open(*this, OB_ALL_MOCK_FK_PARENT_TABLE_TID, ObRowkey(&key, 1),
                         INT64_MAX, false, 0, false, {}, false))) {
    } else if (OB_FAIL(rows.next())) {
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    } else if (OB_FAIL(rows.get_int("schema_version", version))) {
    } else if (OB_FAIL(pair.init(id, version))) {
    } else { ret = out.push_back(pair); }
  }
  return ret;
}

int NativeSchemaCatalog::get_schema_operations(int64_t lower, int64_t upper,
    ObSchemaService::SchemaOperationSetWithAlloc &out)
{
  out.reset();
  if (lower < 0 || upper < 0) { return OB_INVALID_ARGUMENT; }
  if (lower >= upper) { return OB_SUCCESS; }
  ObObj start, end;
  start.set_int(lower); end.set_int(upper);
  ObNewRange range;
  range.start_key_.assign(&start, 1); range.end_key_.assign(&end, 1);
  range.border_flag_.set_inclusive_end();
  CatalogRows rows;
  int ret = rows.open_range(*this, OB_ALL_DDL_OPERATION_TID, range);
  while (OB_SUCC(ret) && OB_SUCC(rows.next())) {
    ObSchemaOperation operation;
    if (OB_FAIL(ObSchemaRetrieveUtils::fill_schema_operation(rows, out, operation))) {
    } else if (operation.op_type_ == OB_INVALID_DDL_OP) { ret = OB_ERR_UNEXPECTED; }
    else { ret = out.push_back(operation); }
  }
  return ret == OB_ITER_END ? OB_SUCCESS : ret;
}

int NativeSchemaCatalog::get_latest_schema_version(int64_t &version)
{
  version = OB_INVALID_VERSION;
  CatalogRows rows;
  int ret = rows.open(*this, OB_ALL_DDL_OPERATION_TID, ObRowkey(), INT64_MAX, true);
  if (OB_SUCC(ret)) { ret = rows.next(); }
  if (OB_SUCC(ret)) { ret = rows.get_int("schema_version", version); }
  return ret == OB_ITER_END ? OB_SUCCESS : ret;
}

int NativeSchemaCatalog::get_completed_schema_version(int64_t upper, int64_t &version)
{
  version = OB_INVALID_VERSION;
  if (upper <= 0) { return OB_INVALID_ARGUMENT; }
  ObObj start[2], end[2];
  start[0].set_int(OB_DDL_END_SIGN); end[0] = start[0];
  start[1].set_min_value(); end[1].set_int(upper);
  ObNewRange range;
  range.start_key_.assign(start, 2); range.end_key_.assign(end, 2);
  range.border_flag_.set_inclusive_start(); range.border_flag_.set_inclusive_end();
  CatalogRows rows;
  int ret = rows.open_range(*this, OB_ALL_DDL_OPERATION_IDX_DDL_TYPE_TID, range, true);
  if (OB_SUCC(ret)) { ret = rows.next(); }
  if (OB_SUCC(ret)) { ret = rows.get_int("schema_version", version); }
  if (ret == OB_ITER_END || (OB_SUCC(ret) && (version <= 0 || !ObSchemaService::is_formal_version(version)))) {
    ret = OB_EAGAIN;
  }
  return ret;
}

int NativeSchemaCatalog::has_table_schema_changes(const ObIArray<uint64_t> &table_ids,
    int64_t lower, int64_t upper, bool &changed)
{
  changed = false;
  if (lower < 0 || upper < 0) { return OB_INVALID_ARGUMENT; }
  if (lower >= upper || table_ids.empty()) { return OB_SUCCESS; }
  std::set<uint64_t> selected;
  for (int64_t i = 0; i < table_ids.count(); ++i) { selected.insert(table_ids.at(i)); }
  ObObj start, end;
  start.set_int(lower); end.set_int(upper);
  ObNewRange range;
  range.start_key_.assign(&start, 1); range.end_key_.assign(&end, 1);
  range.border_flag_.set_inclusive_end();
  CatalogRows rows;
  int ret = rows.open_range(*this, OB_ALL_DDL_OPERATION_TID, range);
  while (OB_SUCC(ret) && !changed && OB_SUCC(rows.next())) {
    int64_t type = 0;
    uint64_t table_id = OB_INVALID_ID;
    if (OB_FAIL(rows.get_int("operation_type", type))) {
    } else if (type <= OB_DDL_TABLE_OPERATION_BEGIN || type >= OB_DDL_TABLE_OPERATION_END) {
    } else if (OB_FAIL(rows.get_uint("table_id", table_id))) {
    } else { changed = selected.count(table_id) != 0; }
  }
  return ret == OB_ITER_END ? OB_SUCCESS : ret;
}

int NativeSchemaCatalog::insert_schema_operation(const ObSchemaOperation &operation)
{
  return insert_fields(OB_ALL_DDL_OPERATION_TID, [&](SchemaCatalogRow &row) {
    return SchemaCatalogFields::operation(operation, row);
  });
}

int NativeSchemaCatalog::check_table_history(uint64_t table_id, int64_t version)
{
  if (table_id == OB_INVALID_ID || version <= 0) { return OB_INVALID_ARGUMENT; }
  ObArenaAllocator allocator(ObMemAttr("CatalogCompare"));
  struct Field {
    ObString name;
    ObObj value;
    TO_STRING_KV(K(name), K(value));
  };
  ObSEArray<Field, 64> expected;
  ObObj key[2]; key[0].set_int(table_id); key[1].set_int(version);
  Cursor history;
  const ObNewRow *row = nullptr;
  int ret = open_prefix(OB_ALL_TABLE_HISTORY_TID, ObRowkey(key, 2), history);
  if (OB_SUCC(ret)) { ret = history.next(row); }
  if (ret == OB_ITER_END) { return OB_SUCCESS; }
  ObSEArray<ObColDesc, 64> columns;
  if (OB_SUCC(ret)) { ret = history.schema()->get_column_ids(columns); }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const auto *column = history.schema()->get_column_schema(columns.at(i).col_id_);
    const auto &name = column->get_column_name_str();
    if (name == "gmt_create" || name == "gmt_modified" || name == "is_deleted") { continue; }
    Field field;
    if (OB_FAIL(ob_write_string(allocator, name, field.name))
        || OB_FAIL(ob_write_obj(allocator, row->cells_[i], field.value))) {}
    else { ret = expected.push_back(field); }
  }
  const int closed = history.close();
  if (OB_SUCC(ret)) { ret = closed; }
  Cursor current;
  if (OB_SUCC(ret)) { ret = open_prefix(OB_ALL_TABLE_TID, ObRowkey(key, 1), current); }
  if (OB_SUCC(ret)) { ret = current.next(row); }
  if (ret == OB_ITER_END) { ret = OB_STATE_NOT_MATCH; }
  columns.reset();
  std::unordered_map<uint64_t, int64_t> positions;
  if (OB_SUCC(ret)) { ret = current.schema()->get_column_ids(columns); }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) { positions.emplace(columns.at(i).col_id_, i); }
  for (int64_t i = 0; OB_SUCC(ret) && i < expected.count(); ++i) {
    const auto &field = expected.at(i);
    const auto *column = current.schema()->get_column_schema(field.name);
    if (column == nullptr) { ret = OB_ERR_COLUMN_NOT_FOUND; }
    else {
      const auto position = positions.find(column->get_column_id());
      if (position == positions.end()) { ret = OB_ERR_UNEXPECTED; }
      else {
        int comparison = 0;
        ret = field.value.compare(row->cells_[position->second], comparison);
        if (OB_SUCC(ret) && comparison != 0) {
          ret = OB_STATE_NOT_MATCH;
          LOG_WARN("table history differs from current metadata", K(ret), K(table_id), K(version), K(field.name));
        }
      }
    }
  }
  return ret;
}

int NativeSchemaCatalog::get_references(uint64_t dependent_id, ObIArray<ObDependencyInfo> &out)
{
  out.reset();
  CatalogRows rows;
  // The primary key starts with object type; there is no dependent-ID index.
  int ret = rows.open(*this, OB_ALL_DEPENDENCY_TID, ObRowkey(), INT64_MAX, false, 0, false,
      [&](const CatalogRows &row, bool &accept) {
    uint64_t id = OB_INVALID_ID;
    const int rc = row.get_uint("dep_obj_id", id);
    accept = id == dependent_id;
    return rc;
  });
  while (OB_SUCC(ret) && OB_SUCC(rows.next())) {
    ObDependencyInfo info;
    if (OB_FAIL(read_dependency(rows, info))) {
    } else { ret = out.push_back(info); }
  }
  if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  if (OB_SUCC(ret) && out.count() > 1) {
    std::sort(out.get_data(), out.get_data() + out.count(), [](const ObDependencyInfo &a, const ObDependencyInfo &b) {
      return a.get_order() < b.get_order();
    });
  }
  return ret;
}

int NativeSchemaCatalog::get_dependents(uint64_t reference_id, ObObjectType reference_type,
    ObIArray<ObDependencyInfo> &out)
{
  out.reset();
  ObObj prefix[2];
  prefix[0].set_int(reference_id);
  prefix[1].set_int(static_cast<int64_t>(reference_type));
  CatalogRows index;
  int ret = index.open(*this, OB_ALL_DEPENDENCY_IDX_DEPENDENCY_REF_OBJ_TID,
      ObRowkey(prefix, reference_type == ObObjectType::INVALID ? 1 : 2), INT64_MAX);
  struct Key { int64_t type; int64_t id; int64_t order; TO_STRING_KV(K(type), K(id), K(order)); };
  ObSEArray<Key, 8> keys;
  while (OB_SUCC(ret) && OB_SUCC(index.next())) {
    Key key{};
    if (OB_FAIL(index.get_int("dep_obj_type", key.type))
        || OB_FAIL(index.get_int("dep_obj_id", key.id))
        || OB_FAIL(index.get_int("dep_order", key.order))) {
    } else { ret = keys.push_back(key); }
  }
  if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  const int closed = index.close();
  if (OB_SUCC(ret)) { ret = closed; }
  for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
    ObObj key[3];
    key[0].set_int(keys.at(i).type); key[1].set_int(keys.at(i).id); key[2].set_int(keys.at(i).order);
    CatalogRows rows;
    ret = rows.open(*this, OB_ALL_DEPENDENCY_TID, ObRowkey(key, 3), INT64_MAX);
    if (OB_SUCC(ret)) { ret = rows.next(); }
    if (ret == OB_ITER_END) { ret = OB_ERR_UNEXPECTED; }
    ObDependencyInfo info;
    if (OB_SUCC(ret)) { ret = read_dependency(rows, info); }
    if (OB_SUCC(ret) && (info.get_ref_obj_id() != reference_id
        || (reference_type != ObObjectType::INVALID && info.get_ref_obj_type() != reference_type))) {
      ret = OB_ERR_UNEXPECTED;
    }
    if (OB_SUCC(ret)) { ret = out.push_back(info); }
  }
  return ret;
}

int NativeSchemaCatalog::write_dependency(const ObDependencyInfo &info, bool upsert)
{
  const auto fill = [&](SchemaCatalogRow &row) {
    return SchemaCatalogFields::dependency(info, state_->statement_time, row);
  };
  return upsert ? upsert_fields(OB_ALL_DEPENDENCY_TID, fill) : insert_fields(OB_ALL_DEPENDENCY_TID, fill);
}

int NativeSchemaCatalog::erase_dependencies(uint64_t dependent_id, ObObjectType type)
{
  int64_t erased = 0;
  return erase_prefix(OB_ALL_DEPENDENCY_TID, [&](SchemaCatalogRow &row) {
    int ret = row.add_pk_column("dep_obj_type", type);
    if (OB_SUCC(ret)) { ret = row.add_pk_column("dep_obj_id", dependent_id); }
    return ret;
  }, erased);
}

int NativeSchemaCatalog::get_triggers(int64_t version, ObIArray<ObTriggerInfo> &out,
    const ObIArray<uint64_t> *ids)
{
  return read_trigger_rows(*this, version, out, ids);
}

int NativeSchemaCatalog::get_simple_triggers(int64_t version, ObIArray<ObSimpleTriggerSchema> &out,
    const ObIArray<uint64_t> *ids)
{
  return read_trigger_rows(*this, version, out, ids);
}

int NativeSchemaCatalog::write_trigger(const ObTriggerInfo &info, int64_t version, TriggerWrite mode)
{
  if (!info.is_valid() || version <= 0 || info.get_trigger_body().empty()) { return OB_INVALID_ARGUMENT; }
  const auto fill = [&](SchemaCatalogRow &row) { return SchemaCatalogFields::trigger(info, version, row); };
  int ret = OB_SUCCESS;
  if (mode == TriggerWrite::INSERT) {
    ret = insert_fields(OB_ALL_TRIGGER_TID, fill);
  } else if (mode == TriggerWrite::UPDATE) {
    bool matched = false;
    ret = update_fields(OB_ALL_TRIGGER_TID, fill, matched);
    if (OB_SUCC(ret) && !matched) { ret = OB_ERR_UNEXPECTED; }
  } else if (mode == TriggerWrite::ERASE) {
    int64_t deleted = 0;
    ret = erase_prefix(OB_ALL_TRIGGER_TID, [&](SchemaCatalogRow &row) {
      return row.add_pk_column("trigger_id", info.get_trigger_id());
    }, deleted);
    if (OB_SUCC(ret) && deleted != 1) { ret = OB_ERR_UNEXPECTED; }
  } else { ret = OB_INVALID_ARGUMENT; }
  if (OB_SUCC(ret)) {
    ret = insert_fields(OB_ALL_TRIGGER_HISTORY_TID, [&](SchemaCatalogRow &row) {
      int ret = fill(row);
      if (OB_SUCC(ret)) { ret = row.add_column("is_deleted", mode == TriggerWrite::ERASE); }
      return ret;
    });
  }
  return ret;
}

int NativeSchemaCatalog::get_simple_mock_fk_parent_tables(int64_t version,
    ObIArray<ObSimpleMockFKParentTableSchema> &out, const ObIArray<uint64_t> *table_ids)
{
  return read_mock_parent_rows(*this, version, out, table_ids);
}

int NativeSchemaCatalog::get_mock_fk_parent_tables(int64_t version,
    ObIArray<ObMockFKParentTableSchema> &out, const ObIArray<uint64_t> *table_ids)
{
  const int64_t begin = out.count();
  int ret = read_mock_parent_rows(*this, version, out, table_ids);
  for (int64_t i = begin; OB_SUCC(ret) && i < out.count(); ++i) {
    auto &table = out.at(i);
    ObObj key;
    key.set_int(table.get_table_id());
    {
      CatalogRows rows;
      if (OB_FAIL(rows.open(*this, OB_ALL_MOCK_FK_PARENT_TABLE_COLUMN_HISTORY_TID,
                           ObRowkey(&key, 1), version, true, 0, true))) {
      } else { ret = ObSchemaRetrieveUtils::retrieve_mock_fk_parent_table_schema_column(rows, table); }
    }
    // Reverse history scans select the newest row of each column. Restore
    // the ascending column-ID order expected by existing schema consumers.
    ObMockFKParentTableColumnArray columns;
    if (OB_SUCC(ret)) { ret = columns.assign(table.get_column_array()); }
    if (OB_SUCC(ret)) {
      lib::ob_sort(columns.begin(), columns.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
      ret = table.set_column_array(columns);
    }
    if (OB_SUCC(ret)) { ret = read_foreign_keys(*this, version, true, table); }
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

int NativeSchemaCatalog::insert_constraint(const ObConstraint &constraint, bool only_history, bool with_columns)
{
  int ret = OB_SUCCESS;
  if (!only_history) {
    ret = insert_fields(OB_ALL_CONSTRAINT_TID, [&](SchemaCatalogRow &fields) {
      return SchemaCatalogFields::constraint(constraint, fields);
    });
  }
  if (OB_SUCC(ret)) {
    ret = insert_fields(OB_ALL_CONSTRAINT_HISTORY_TID, [&](SchemaCatalogRow &fields) {
      int ret = SchemaCatalogFields::constraint(constraint, fields);
      if (OB_SUCC(ret)) { ret = fields.add_column("is_deleted", 0); }
      return ret;
    });
  }
  if (OB_SUCC(ret) && with_columns) { ret = insert_constraint_columns(constraint, only_history); }
  return ret;
}

int NativeSchemaCatalog::insert_constraint_columns(const ObConstraint &constraint, bool only_history)
{
  int ret = OB_SUCCESS;
  for (auto it = constraint.cst_col_begin(); OB_SUCC(ret) && it != constraint.cst_col_end(); ++it) {
    const auto fields = [&](SchemaCatalogRow &row) { return SchemaCatalogFields::constraint_column(constraint, *it, row); };
    if (!only_history) { ret = insert_fields(OB_ALL_CONSTRAINT_COLUMN_TID, fields); }
    if (OB_SUCC(ret)) {
      ret = insert_fields(OB_ALL_CONSTRAINT_COLUMN_HISTORY_TID, [&](SchemaCatalogRow &row) {
        int ret = fields(row);
        if (OB_SUCC(ret)) { ret = row.add_column("is_deleted", 0); }
        return ret;
      });
    }
  }
  return ret;
}

int NativeSchemaCatalog::insert_constraints(const ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  // Keep the caller's published objects stamped just as CREATE did before.
  for (auto it = table.constraint_begin_for_non_const_iter();
       OB_SUCC(ret) && it != table.constraint_end_for_non_const_iter(); ++it) {
    if (*it == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else {
      (*it)->set_table_id(table.get_table_id());
      (*it)->set_schema_version(table.get_schema_version());
      ret = insert_constraint(**it, false, true);
    }
  }
  return ret;
}

int NativeSchemaCatalog::update_constraint(const ObConstraint &constraint)
{
  if (!constraint.get_is_modify_rely_flag() && !constraint.get_is_modify_enable_flag()
      && !constraint.get_is_modify_validate_flag() && !constraint.get_is_modify_check_expr()) {
    return OB_SUCCESS;
  }
  bool matched = false;
  int ret = update_fields(OB_ALL_CONSTRAINT_TID, [&](SchemaCatalogRow &fields) {
    int ret = OB_SUCCESS;
    if (OB_FAIL(fields.add_pk_column("table_id", constraint.get_table_id()))
        || OB_FAIL(fields.add_pk_column("constraint_id", constraint.get_constraint_id()))
        || OB_FAIL(fields.add_column("schema_version", constraint.get_schema_version()))
        || OB_FAIL(fields.add_column("check_expr", constraint.get_check_expr()))
        || OB_FAIL(fields.add_gmt_modified())) {}
    if (OB_SUCC(ret) && constraint.get_is_modify_rely_flag()) { ret = fields.add_column("rely_flag", constraint.get_rely_flag()); }
    if (OB_SUCC(ret) && constraint.get_is_modify_enable_flag()) { ret = fields.add_column("enable_flag", constraint.get_enable_flag()); }
    if (OB_SUCC(ret) && constraint.get_is_modify_validate_flag()) { ret = fields.add_column("validate_flag", constraint.get_validate_flag()); }
    return ret;
  }, matched);
  if (OB_SUCC(ret)) { ret = insert_constraint(constraint, true, false); }
  return ret;
}

int NativeSchemaCatalog::rename_constraint(const ObConstraint &constraint, const ObString &name, int64_t version)
{
  ObConstraint renamed;
  int ret = renamed.assign(constraint);
  if (OB_SUCC(ret)) { ret = renamed.set_constraint_name(name); }
  renamed.set_schema_version(version);
  bool matched = false;
  if (OB_SUCC(ret)) {
    ret = update_fields(OB_ALL_CONSTRAINT_TID, [&](SchemaCatalogRow &fields) {
      int ret = OB_SUCCESS;
      if (OB_FAIL(fields.add_pk_column("table_id", renamed.get_table_id()))
          || OB_FAIL(fields.add_pk_column("constraint_id", renamed.get_constraint_id()))
          || OB_FAIL(fields.add_column("schema_version", version))
          || OB_FAIL(fields.add_column("constraint_name", name))
          || OB_FAIL(fields.add_column("name_generated_type", renamed.get_name_generated_type()))
          || OB_FAIL(fields.add_gmt_modified())) {}
      return ret;
    }, matched);
    if (OB_SUCC(ret) && !matched) { ret = OB_ERR_UNEXPECTED; }
  }
  // A rename publishes a complete history row, including the existing flags.
  if (OB_SUCC(ret)) { ret = insert_constraint(renamed, true, false); }
  return ret;
}

int NativeSchemaCatalog::erase_constraint(const ObConstraint &constraint, int64_t version, int64_t &deleted_columns)
{
  deleted_columns = 0;
  const auto key = [&](SchemaCatalogRow &fields) {
    int ret = fields.add_pk_column("table_id", constraint.get_table_id());
    if (OB_SUCC(ret)) { ret = fields.add_pk_column("constraint_id", constraint.get_constraint_id()); }
    return ret;
  };
  int64_t affected = 0;
  int ret = erase_prefix(OB_ALL_CONSTRAINT_TID, key, affected);
  if (OB_SUCC(ret) && affected != 1) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) { ret = erase_prefix(OB_ALL_CONSTRAINT_COLUMN_TID, key, deleted_columns); }
  if (OB_SUCC(ret)) {
    ret = insert_fields(OB_ALL_CONSTRAINT_HISTORY_TID, [&](SchemaCatalogRow &fields) {
      int ret = key(fields);
      if (OB_SUCC(ret) && (OB_FAIL(fields.add_pk_column("schema_version", version))
          || OB_FAIL(fields.add_column("is_deleted", 1)))) {}
      return ret;
    });
  }
  for (auto it = constraint.cst_col_begin(); OB_SUCC(ret) && it != constraint.cst_col_end(); ++it) {
    ret = insert_fields(OB_ALL_CONSTRAINT_COLUMN_HISTORY_TID, [&](SchemaCatalogRow &fields) {
      int ret = key(fields);
      if (OB_SUCC(ret) && (OB_FAIL(fields.add_pk_column("column_id", *it))
          || OB_FAIL(fields.add_pk_column("schema_version", version))
          || OB_FAIL(fields.add_column("is_deleted", 1)))) {}
      return ret;
    });
  }
  return ret;
}

int NativeSchemaCatalog::write_foreign_key(const ObForeignKeyInfo &info, int64_t version, ForeignKeyWrite mode)
{
  const auto fill = [&](SchemaCatalogRow &fields) { return SchemaCatalogFields::foreign_key(info, fields); };
  int ret = OB_SUCCESS;
  if (mode == ForeignKeyWrite::INSERT) { ret = insert_fields(OB_ALL_FOREIGN_KEY_TID, fill); }
  else if (mode == ForeignKeyWrite::UPSERT) { ret = upsert_fields(OB_ALL_FOREIGN_KEY_TID, fill); }
  if (OB_SUCC(ret)) {
    ret = insert_fields(OB_ALL_FOREIGN_KEY_HISTORY_TID, [&](SchemaCatalogRow &fields) {
      int ret = fill(fields);
      if (OB_SUCC(ret) && (OB_FAIL(fields.add_pk_column("schema_version", version))
          || OB_FAIL(fields.add_column("is_deleted", mode == ForeignKeyWrite::HISTORY_DELETE)))) {}
      return ret;
    });
  }
  return ret;
}

int NativeSchemaCatalog::insert_foreign_key_columns(const ObForeignKeyInfo &info, int64_t version, bool only_history)
{
  if (info.child_column_ids_.count() != info.parent_column_ids_.count()) { return OB_INVALID_ARGUMENT; }
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < info.child_column_ids_.count(); ++i) {
    const auto fill = [&](SchemaCatalogRow &fields) {
      return SchemaCatalogFields::foreign_key_column(info.foreign_key_id_, info.child_column_ids_.at(i),
          info.parent_column_ids_.at(i), i + 1, fields);
    };
    if (!only_history) { ret = insert_fields(OB_ALL_FOREIGN_KEY_COLUMN_TID, fill); }
    if (OB_SUCC(ret)) {
      ret = insert_fields(OB_ALL_FOREIGN_KEY_COLUMN_HISTORY_TID, [&](SchemaCatalogRow &fields) {
        int ret = fill(fields);
        if (OB_SUCC(ret) && (OB_FAIL(fields.add_pk_column("schema_version", version))
            || OB_FAIL(fields.add_column("is_deleted", only_history)))) {}
        return ret;
      });
    }
  }
  return ret;
}

int NativeSchemaCatalog::erase_foreign_key(const ObForeignKeyInfo &info, int64_t version, bool require_current)
{
  int64_t affected = 0;
  int ret = erase_prefix(OB_ALL_FOREIGN_KEY_TID, [&](SchemaCatalogRow &fields) {
    return fields.add_pk_column("foreign_key_id", info.foreign_key_id_);
  }, affected);
  // Parent and child deletion can encounter the same relationship in one DDL.
  if (OB_SUCC(ret) && require_current && affected != 1) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) { ret = write_foreign_key(info, version, ForeignKeyWrite::HISTORY_DELETE); }
  return ret;
}

int NativeSchemaCatalog::erase_foreign_key_columns(const ObForeignKeyInfo &info, int64_t version)
{
  if (info.child_column_ids_.count() != info.parent_column_ids_.count()) { return OB_INVALID_ARGUMENT; }
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < info.child_column_ids_.count(); ++i) {
    const auto key = [&](SchemaCatalogRow &fields) {
      int ret = OB_SUCCESS;
      if (OB_FAIL(fields.add_pk_column("foreign_key_id", info.foreign_key_id_))
          || OB_FAIL(fields.add_pk_column("child_column_id", info.child_column_ids_.at(i)))
          || OB_FAIL(fields.add_pk_column("parent_column_id", info.parent_column_ids_.at(i)))) {}
      return ret;
    };
    int64_t affected = 0;
    ret = erase_prefix(OB_ALL_FOREIGN_KEY_COLUMN_TID, key, affected);
    if (OB_SUCC(ret)) {
      ret = insert_fields(OB_ALL_FOREIGN_KEY_COLUMN_HISTORY_TID, [&](SchemaCatalogRow &fields) {
        int ret = key(fields);
        if (OB_SUCC(ret) && (OB_FAIL(fields.add_pk_column("schema_version", version))
            || OB_FAIL(fields.add_column("is_deleted", 1))
            || OB_FAIL(fields.add_column("position", i + 1)))) {}
        return ret;
      });
    }
  }
  return ret;
}

int NativeSchemaCatalog::insert_mock_parent(const ObMockFKParentTableSchema &table, bool only_history)
{
  const auto fill = [&](SchemaCatalogRow &fields) { return SchemaCatalogFields::mock_parent(table, fields); };
  int ret = only_history ? OB_SUCCESS : insert_fields(OB_ALL_MOCK_FK_PARENT_TABLE_TID, fill);
  if (OB_SUCC(ret)) {
    ret = insert_fields(OB_ALL_MOCK_FK_PARENT_TABLE_HISTORY_TID, [&](SchemaCatalogRow &fields) {
      int ret = fill(fields);
      if (OB_SUCC(ret)) { ret = fields.add_column("is_deleted", 0); }
      return ret;
    });
  }
  return ret;
}

int NativeSchemaCatalog::insert_mock_parent_columns(const ObMockFKParentTableSchema &table)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_column_array().count(); ++i) {
    const auto fill = [&](SchemaCatalogRow &fields) { return SchemaCatalogFields::mock_parent_column(table, i, fields); };
    ret = insert_fields(OB_ALL_MOCK_FK_PARENT_TABLE_COLUMN_TID, fill);
    if (OB_SUCC(ret)) {
      ret = insert_fields(OB_ALL_MOCK_FK_PARENT_TABLE_COLUMN_HISTORY_TID, [&](SchemaCatalogRow &fields) {
        int ret = fill(fields);
        if (OB_SUCC(ret)) { ret = fields.add_column("is_deleted", 0); }
        return ret;
      });
    }
  }
  return ret;
}

int NativeSchemaCatalog::erase_mock_parent(const ObMockFKParentTableSchema &table)
{
  const auto key = [&](SchemaCatalogRow &fields) {
    return fields.add_pk_column("mock_fk_parent_table_id", table.get_mock_fk_parent_table_id());
  };
  int64_t affected = 0;
  int ret = erase_prefix(OB_ALL_MOCK_FK_PARENT_TABLE_TID, key, affected);
  if (OB_SUCC(ret) && affected != 1) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) {
    ret = insert_fields(OB_ALL_MOCK_FK_PARENT_TABLE_HISTORY_TID, [&](SchemaCatalogRow &fields) {
      int ret = key(fields);
      if (OB_SUCC(ret) && (OB_FAIL(fields.add_column("database_id", table.get_database_id()))
          || OB_FAIL(fields.add_pk_column("schema_version", table.get_schema_version()))
          || OB_FAIL(fields.add_column("is_deleted", 1)) || OB_FAIL(fields.add_gmt_modified()))) {}
      return ret;
    });
  }
  return ret;
}

int NativeSchemaCatalog::erase_mock_parent_columns(const ObMockFKParentTableSchema &table)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_column_array().count(); ++i) {
    const auto key = [&](SchemaCatalogRow &fields) {
      int ret = fields.add_pk_column("mock_fk_parent_table_id", table.get_mock_fk_parent_table_id());
      if (OB_SUCC(ret)) { ret = fields.add_pk_column("parent_column_id", table.get_column_array().at(i).first); }
      return ret;
    };
    int64_t affected = 0;
    ret = erase_prefix(OB_ALL_MOCK_FK_PARENT_TABLE_COLUMN_TID, key, affected);
    if (OB_SUCC(ret) && affected != 1) { ret = OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret)) {
      ret = insert_fields(OB_ALL_MOCK_FK_PARENT_TABLE_COLUMN_HISTORY_TID, [&](SchemaCatalogRow &fields) {
        int ret = key(fields);
        if (OB_SUCC(ret) && (OB_FAIL(fields.add_pk_column("schema_version", table.get_schema_version()))
            || OB_FAIL(fields.add_column("is_deleted", 1)) || OB_FAIL(fields.add_gmt_modified()))) {}
        return ret;
      });
    }
  }
  return ret;
}

int NativeSchemaCatalog::update_mock_parent_version(const ObMockFKParentTableSchema &table)
{
  bool matched = false;
  int ret = update_fields(OB_ALL_MOCK_FK_PARENT_TABLE_TID, [&](SchemaCatalogRow &fields) {
    int ret = OB_SUCCESS;
    if (OB_FAIL(fields.add_pk_column("mock_fk_parent_table_id", table.get_mock_fk_parent_table_id()))
        || OB_FAIL(fields.add_column("schema_version", table.get_schema_version()))
        || OB_FAIL(fields.add_gmt_modified())) {}
    return ret;
  }, matched);
  if (OB_SUCC(ret) && !matched) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) { ret = insert_mock_parent(table, true); }
  return ret;
}

int NativeSchemaCatalog::upsert_fields(uint64_t inner_table_id, const FieldWriter &fill)
{
  if (!fill) { return OB_INVALID_ARGUMENT; }
  TableState *target = nullptr;
  int ret = prepare_write(inner_table_id, target);
  SchemaCatalogRow fields;
  ObNewRow row;
  ObSEArray<uint64_t, 16> columns;
  if (OB_SUCC(ret)) { ret = fields.init(target->schema, state_->statement_time); }
  if (OB_SUCC(ret)) { ret = fill(fields); }
  if (OB_SUCC(ret)) { ret = fields.finish_update(row, columns); }
  // lock_rows assumes an existing row: locking an absent key produces a lock
  // node which a following INSERT treats as a duplicate. Inspect the current
  // row first; the DDL owner serializes this object's metadata mutations.
  ObArenaAllocator allocator(ObMemAttr("CatalogUpsert"));
  ObNewRow current;
  if (OB_SUCC(ret)) {
    ret = state_->native.get_current(state_->transaction, target->table,
        ObRowkey(row.cells_, target->schema.get_rowkey_column_num()), allocator, current);
    if (ret == OB_ENTRY_NOT_EXIST) { ret = insert_fields(inner_table_id, fill); }
    else if (OB_SUCC(ret)) {
      bool matched = false;
      ret = update_fields(inner_table_id, fill, matched);
      if (OB_SUCC(ret) && !matched) { ret = OB_STATE_NOT_MATCH; }
    }
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

int NativeSchemaCatalog::insert_fields(uint64_t inner_table_id, const FieldWriter &fill)
{
  if (!fill) { return OB_INVALID_ARGUMENT; }
  TableState *target = nullptr;
  int ret = prepare_write(inner_table_id, target);
  SchemaCatalogRow fields;
  ObNewRow row;
  if (OB_SUCC(ret)) { ret = fields.init(target->schema, state_->statement_time); }
  if (OB_SUCC(ret)) { ret = fill(fields); }
  if (OB_SUCC(ret)) { ret = fields.finish(row); }
  if (OB_SUCC(ret)) { ret = insert(inner_table_id, row); }
  return ret;
}

int NativeSchemaCatalog::erase_prefix(uint64_t inner_table_id, const FieldWriter &fill,
    int64_t &affected_rows)
{
  affected_rows = 0;
  if (!fill) { return OB_INVALID_ARGUMENT; }
  TableState *target = nullptr;
  int ret = prepare_write(inner_table_id, target);
  SchemaCatalogRow fields;
  ObRowkey prefix;
  if (OB_SUCC(ret)) { ret = fields.init(target->schema, state_->statement_time); }
  if (OB_SUCC(ret)) { ret = fill(fields); }
  if (OB_SUCC(ret)) { ret = fields.finish_key_prefix(prefix); }
  // A point deletion needs no scan. For a range, close the read iterator
  // before mutations, retaining only keys rather than complete metadata rows.
  if (OB_SUCC(ret) && prefix.get_obj_cnt() == target->schema.get_rowkey_column_num()) {
    bool existed = false;
    ret = erase(inner_table_id, prefix, existed);
    if (OB_SUCC(ret)) { affected_rows = existed ? 1 : 0; }
  } else if (OB_SUCC(ret)) {
    Cursor cursor;
    ObArenaAllocator allocator(ObMemAttr("CatalogDelKeys"));
    ObSEArray<ObRowkey, 16> keys;
    const ObNewRow *row = nullptr;
    ret = open_prefix(inner_table_id, prefix, cursor);
    while (OB_SUCC(ret) && OB_SUCC(ret = cursor.next(row))) {
      ObRowkey key;
      if (OB_FAIL(ObRowkey(row->cells_, target->schema.get_rowkey_column_num()).deep_copy(key, allocator))) {
      } else { ret = keys.push_back(key); }
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    const int cleanup = cursor.close();
    if (OB_SUCC(ret)) { ret = cleanup; }
    for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
      bool existed = false;
      ret = erase(inner_table_id, keys.at(i), existed);
      if (OB_SUCC(ret) && existed) { ++affected_rows; }
    }
  }
  return ret;
}

int NativeSchemaCatalog::update_fields(uint64_t inner_table_id, const FieldWriter &fill, bool &matched)
{
  matched = false;
  if (!fill) { return OB_INVALID_ARGUMENT; }
  TableState *target = nullptr;
  int ret = prepare_write(inner_table_id, target);
  SchemaCatalogRow fields;
  ObNewRow row;
  ObSEArray<uint64_t, 16> columns;
  if (OB_SUCC(ret)) { ret = fields.init(target->schema, state_->statement_time); }
  if (OB_SUCC(ret)) { ret = fill(fields); }
  if (OB_SUCC(ret)) { ret = fields.finish_update(row, columns); }
  if (OB_SUCC(ret)) { ret = mutate(inner_table_id, row, Mutation::UPDATE, matched, &columns); }
  return ret;
}

int NativeSchemaCatalog::update_table(const ObTableSchema &table, bool update_object_status_ignore_version)
{
  bool matched = false;
  return update_fields(OB_ALL_TABLE_TID, [&](SchemaCatalogRow &fields) {
    int ret = SchemaCatalogFields::table(table, update_object_status_ignore_version, fields, false);
    // SQL supplied this column's ON UPDATE timestamp implicitly.
    if (OB_SUCC(ret)) { ret = fields.add_gmt_modified(); }
    return ret;
  }, matched);
}

int NativeSchemaCatalog::update_column(const ObColumnSchemaV2 &column)
{
  bool matched = false;
  return update_fields(OB_ALL_COLUMN_TID, [&](SchemaCatalogRow &fields) {
    return SchemaCatalogFields::column(column, fields, false);
  }, matched);
}

int NativeSchemaCatalog::erase(uint64_t inner_table_id, const ObRowkey &key, bool &existed)
{
  ObNewRow row;
  row.assign(const_cast<ObObj *>(key.get_obj_ptr()), key.get_obj_cnt());
  return mutate(inner_table_id, row, Mutation::ERASE, existed);
}

int NativeSchemaCatalog::mutate(uint64_t inner_table_id, const ObNewRow &input,
    Mutation operation, bool &existed, const ObIArray<uint64_t> *column_ids)
{
  existed = false;
  ObNewRow row = input;
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
    ret = s.native.get_storage_row_for_update(s.transaction, base->table, key, allocator, old);
    existed = ret == OB_SUCCESS;
    if (ret == OB_ENTRY_NOT_EXIST && (operation == Mutation::ERASE || column_ids != nullptr)) { return OB_SUCCESS; }
  }
  ObSEArray<ObObj, 16> merged;
  if (OB_SUCC(ret) && column_ids != nullptr) {
    for (int64_t i = 0; OB_SUCC(ret) && i < old.count_; ++i) {
      bool provided = false;
      for (int64_t j = 0; j < column_ids->count(); ++j) {
        if (base->columns.at(i).col_id_ == column_ids->at(j)) { provided = true; break; }
      }
      ret = merged.push_back(provided ? input.cells_[i] : old.cells_[i]);
    }
    row.assign(merged.get_data(), merged.count());
  }
  if (OB_SUCC(ret)) {
    if (operation == Mutation::INSERT) { ret = s.native.insert(s.transaction, base->table, row); }
    else if (operation == Mutation::UPDATE) {
      ret = column_ids == nullptr ? s.native.update(s.transaction, base->table, row)
                                 : s.native.update(s.transaction, base->table, row, *column_ids);
    }
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
