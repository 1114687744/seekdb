/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE
#include "storage/instance_meta/instance_meta_store.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
using namespace transaction;
namespace storage
{
// Directory page collection excludes admitted metadata transactions only while
// validating roots and deleting a bounded batch. This is a KV directory policy,
// independent of native row access, transaction ownership and MVCC retention.
struct InstanceMetaStore::State
{
  std::mutex mutex;
  std::condition_variable changed;
  int64_t ordinary_transactions = 0;
  bool directory_gc_active = false;
};

InstanceMetaStore::Transaction::Transaction()
    : owner_(nullptr), native_(), directory_guard_(false), directory_gc_(false)
{}
InstanceMetaStore::Transaction::~Transaction()
{
  if (native_.is_active()) {
    const int ret = native_.is_borrowed() ? owner_->detach(*this) : owner_->rollback(*this);
    if (ret != OB_SUCCESS) { LOG_WARN("metadata transaction cleanup failed", K(ret)); }
  }
}
InstanceMetaStore::InstanceMetaStore(ObAccessService &access, ObTransService &transactions)
    : table_(access, transactions), state_(nullptr)
{}
InstanceMetaStore::~InstanceMetaStore() { delete state_; }

int InstanceMetaStore::build_schema(InstanceTable table, ObTableSchema &schema)
{
  using namespace share::instance_meta;
  const auto *definition = instance_table_definition(table);
  if (definition == nullptr) { return OB_INVALID_ARGUMENT; }
  int ret = OB_SUCCESS;
  schema.reset();
  schema.set_table_id(definition->tablet_id);
  schema.set_tablet_id(ObTabletID(definition->tablet_id));
  schema.set_database_id(OB_SYS_DATABASE_ID);
  schema.set_schema_version(SCHEMA_VERSION);
  schema.set_table_type(SYSTEM_TABLE);
  schema.set_rowkey_column_num(definition->key_columns);
  schema.set_max_used_column_id(OB_APP_MIN_COLUMN_ID + definition->field_count - 1);
  schema.set_micro_index_clustered(false);
  schema.set_row_store_type(FLAT_ROW_STORE);
  ret = schema.set_table_name(definition->name);
  for (int64_t i = 0; OB_SUCC(ret) && i < definition->field_count; ++i) {
    const auto &field = definition->fields[i];
    ObColumnSchemaV2 column;
    column.set_table_id(schema.get_table_id());
    column.set_column_id(OB_APP_MIN_COLUMN_ID + i);
    column.set_schema_version(SCHEMA_VERSION);
    column.set_nullable(false);
    column.set_rowkey_position(i < definition->key_columns ? i + 1 : 0);
    column.set_order_in_rowkey(ASC);
    ObObjMeta type;
    switch (field.type) {
      case FieldType::UINT64: type.set_uint64(); break;
      case FieldType::INT64: type.set_int(); break;
      // Bytewise names preserve the directory's existing exact name identity.
      case FieldType::TEXT: type.set_varbinary(); break;
      case FieldType::BYTES: type.set_varbinary(); break;
    }
    column.set_meta_type(type);
    column.set_data_length(field.length > 0 ? field.length : int64_t(sizeof(uint64_t)));
    if (OB_FAIL(column.set_column_name(field.name))) {
    } else if (OB_FAIL(schema.add_column(column))) {
    }
  }
  return ret;
}

int InstanceMetaStore::init_directory()
{ return init(InstanceTable::NAMESPACES, InstanceTable::SNAPSHOT_COORDINATION); }
int InstanceMetaStore::init_layouts()
{ return init(InstanceTable::STORAGE_LAYOUT_KV, InstanceTable::STORAGE_LAYOUT_KV); }

int InstanceMetaStore::init(InstanceTable first, InstanceTable last)
{
  if (state_ != nullptr) { return OB_INIT_TWICE; }
  int ret = OB_SUCCESS;
  State *state = new (std::nothrow) State();
  if (state == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  for (int i = static_cast<int>(first); OB_SUCC(ret) && i <= static_cast<int>(last); ++i) {
    ObArenaAllocator allocator(ObMemAttr("InstanceMeta"));
    ObTableSchema schema(&allocator);
    if (OB_FAIL(build_schema(static_cast<InstanceTable>(i), schema))) {
    } else if (OB_FAIL(table_.bind(bound_tables_[i], schema.get_tablet_id(), schema))) {
    }
  }
  if (OB_SUCC(ret)) { state_ = state; } else { delete state; }
  return ret;
}

int InstanceMetaStore::begin(Transaction &tx, const int64_t deadline, const bool read_only)
{
  return begin_impl(tx, deadline, false, [&] { return table_.begin(tx.native_, deadline, read_only); });
}

int InstanceMetaStore::begin_directory_gc(Transaction &tx, const int64_t deadline)
{
  return begin_impl(tx, deadline, true, [&] { return table_.begin(tx.native_, deadline); });
}

int InstanceMetaStore::begin_read(Transaction &tx, int64_t deadline,
    const SnapshotAcquirer &acquire)
{
  return acquire ? begin_impl(tx, deadline, false, [&] {
    return table_.begin_read(tx.native_, deadline, acquire);
  }) : OB_INVALID_ARGUMENT;
}

int InstanceMetaStore::begin_weak_read(Transaction &tx, int64_t deadline)
{
  return begin_impl(tx, deadline, false, [&] { return table_.begin_weak_read(tx.native_, deadline); });
}

int InstanceMetaStore::attach(Transaction &tx, ObTxDesc &descriptor, const int64_t deadline)
{
  return begin_impl(tx, deadline, false, [&] { return table_.attach(tx.native_, descriptor, deadline); });
}

int InstanceMetaStore::with_native_transaction(Transaction &tx,
    const std::function<int(ObTxDesc &)> &operation)
{
  return tx.owner_ != this ? OB_INVALID_ARGUMENT : table_.with_native_transaction(tx.native_, operation);
}

int InstanceMetaStore::attach(Transaction &tx, const Transaction &owner, int64_t deadline)
{
  if (owner.owner_ == nullptr || owner.owner_ == this || !owner.is_active()) {
    return OB_INVALID_ARGUMENT;
  }
  return begin_impl(tx, deadline, false, [&] { return table_.attach(tx.native_, owner.native_, deadline); });
}

int InstanceMetaStore::begin_impl(Transaction &tx, const int64_t deadline,
    const bool directory_gc, const std::function<int()> &begin_native)
{
  int ret = OB_SUCCESS;
  if (state_ == nullptr) {
    ret = OB_NOT_INIT;
  } else if (tx.is_active() || tx.directory_guard_) {
    ret = OB_INIT_TWICE;
  } else if (deadline <= ObTimeUtility::current_time()) {
    ret = OB_TIMEOUT;
  } else {
    {
      std::unique_lock<std::mutex> guard(state_->mutex);
      while ((directory_gc ? state_->directory_gc_active
                            || state_->ordinary_transactions != 0
                           : state_->directory_gc_active)
             && ret == OB_SUCCESS) {
        const int64_t remain = deadline - ObTimeUtility::current_time();
        if (remain <= 0) {
          ret = OB_TIMEOUT;
        } else {
          state_->changed.wait_for(
              guard, std::chrono::microseconds(remain));
        }
      }
      if (ret == OB_SUCCESS && deadline <= ObTimeUtility::current_time()) {
        ret = OB_TIMEOUT;
      }
      if (ret == OB_SUCCESS) {
        if (directory_gc) { state_->directory_gc_active = true; }
        else { ++state_->ordinary_transactions; }
        tx.directory_guard_ = true;
        tx.directory_gc_ = directory_gc;
      }
    }
  }
  if (OB_SUCC(ret)) {
    tx.owner_ = this;
    ret = begin_native();
    if (OB_FAIL(ret)) { release_directory_guard(tx); }
  }
  return ret;
}

void InstanceMetaStore::release_directory_guard(Transaction &tx)
{
  if (tx.directory_guard_) {
    {
      std::lock_guard<std::mutex> guard(state_->mutex);
      if (tx.directory_gc_) { state_->directory_gc_active = false; }
      else { --state_->ordinary_transactions; }
      tx.owner_ = nullptr;
      tx.directory_guard_ = false;
      tx.directory_gc_ = false;
    }
    state_->changed.notify_all();
  }
}

int InstanceMetaStore::end(Transaction &tx, bool do_commit)
{
  if (tx.owner_ != this) { return OB_INVALID_ARGUMENT; }
  const int ret = do_commit ? table_.commit(tx.native_) : table_.rollback(tx.native_);
  if (!tx.is_active()) { release_directory_guard(tx); }
  return ret;
}
int InstanceMetaStore::detach(Transaction &tx)
{
  if (tx.owner_ != this) { return OB_INVALID_ARGUMENT; }
  const int ret = table_.detach(tx.native_);
  if (!tx.is_active()) { release_directory_guard(tx); }
  return ret;
}
int InstanceMetaStore::commit(Transaction &tx) { return end(tx, true); }
int InstanceMetaStore::rollback(Transaction &tx) { return end(tx, false); }
int InstanceMetaStore::retain_snapshot(const Transaction &tx, SnapshotHandle &snapshot)
{
  if (state_ == nullptr) { return OB_NOT_INIT; }
  if (tx.owner_ != this) { return OB_INVALID_ARGUMENT; }
  return table_.retain_snapshot(tx.native_, snapshot);
}
int InstanceMetaStore::min_retained_snapshot(share::SCN &snapshot)
{ return table_.min_retained_snapshot(snapshot); }

int InstanceMetaStore::check(const Transaction &tx, InstanceTable table) const
{
  if (state_ == nullptr) { return OB_NOT_INIT; }
  return tx.owner_ == this && tx.is_active()
      && share::instance_meta::instance_table_definition(table) != nullptr
      ? OB_SUCCESS : OB_INVALID_ARGUMENT;
}

int InstanceMetaStore::get(Transaction &tx, InstanceTable target, const ObObj &key,
    ObIAllocator &allocator, ObNewRow &row, Read mode)
{
  const int ret = check(tx, target);
  if (ret != OB_SUCCESS) { return ret; }
  const auto &bound = bound_tables_[static_cast<int>(target)];
  ObObj key_cell = key;
  ObRowkey rowkey(&key_cell, 1);
  return mode == Read::FOR_UPDATE ? table_.get_for_update(tx.native_, bound, rowkey, allocator, row)
      : mode == Read::CURRENT ? table_.get_current(tx.native_, bound, rowkey, allocator, row)
      : table_.get(tx.native_, bound, rowkey, allocator, row);
}
int InstanceMetaStore::insert(Transaction &tx, InstanceTable target, const ObNewRow &row)
{
  const int ret = check(tx, target);
  return ret == OB_SUCCESS ? table_.insert(tx.native_, bound_tables_[static_cast<int>(target)], row) : ret;
}
int InstanceMetaStore::put(Transaction &tx, InstanceTable target, const ObNewRow &row)
{
  const int ret = check(tx, target);
  return ret == OB_SUCCESS ? table_.put(tx.native_, bound_tables_[static_cast<int>(target)], row) : ret;
}
int InstanceMetaStore::erase(Transaction &tx, InstanceTable target, const ObObj &key, bool &existed)
{
  const int ret = check(tx, target);
  ObObj key_cell = key;
  return ret == OB_SUCCESS ? table_.erase(tx.native_, bound_tables_[static_cast<int>(target)],
                                         ObRowkey(&key_cell, 1), existed) : ret;
}
int InstanceMetaStore::scan(Transaction &tx, InstanceTable target, const NativeRowVisitor &visitor,
    const ObObj *exclusive_lower)
{
  const int ret = check(tx, target);
  if (ret != OB_SUCCESS) { return ret; }
  ObObj lower, upper;
  lower.set_min_value(); upper.set_max_value();
  if (exclusive_lower != nullptr) { lower = *exclusive_lower; }
  ObNewRange range;
  range.start_key_.assign(&lower, 1);
  range.end_key_.assign(&upper, 1);
  return table_.scan(tx.native_, bound_tables_[static_cast<int>(target)], range, visitor);
}

int InstanceMetaStore::check(const Transaction &tx, MetaCollection collection,
    const ObString &key) const
{
  if (state_ == nullptr) { return OB_NOT_INIT; }
  if (tx.owner_ != this || !tx.is_active() || static_cast<uint64_t>(collection) == 0
      || key.length() < 0 || (key.length() > 0 && key.ptr() == nullptr)) {
    return OB_INVALID_ARGUMENT;
  }
  return key.length() > MAX_KEY_LENGTH ? OB_SIZE_OVERFLOW : OB_SUCCESS;
}

int InstanceMetaStore::scan(Transaction &tx, MetaCollection collection,
    const KeyRange &range, const RowVisitor &visitor)
{
  int ret = check(tx, collection, range.has_lower ? range.lower : ObString());
  if (OB_SUCC(ret) && range.has_upper) { ret = check(tx, collection, range.upper); }
  if (OB_SUCC(ret) && !visitor) { ret = OB_INVALID_ARGUMENT; }
  if (OB_SUCC(ret)) {
    ObObj lower[2], upper[2];
    lower[0].set_uint64(static_cast<uint64_t>(collection));
    upper[0] = lower[0];
    if (range.has_lower) { lower[1].set_varbinary(range.lower); } else { lower[1].set_min_value(); }
    if (range.has_upper) { upper[1].set_varbinary(range.upper); } else { upper[1].set_max_value(); }
    ObNewRange keys;
    keys.start_key_.assign(lower, 2);
    keys.end_key_.assign(upper, 2);
    if (range.include_lower) { keys.border_flag_.set_inclusive_start(); }
    if (range.include_upper) { keys.border_flag_.set_inclusive_end(); }
    ret = table_.scan(tx.native_, bound_tables_[static_cast<int>(InstanceTable::STORAGE_LAYOUT_KV)], keys, [&](const ObNewRow &row, bool &stop) {
      if (row.count_ != 3 || row.cells_[0].is_null() || row.cells_[1].is_null()
          || row.cells_[2].is_null() || row.cells_[0].get_uint64() != static_cast<uint64_t>(collection)) {
        return OB_ERR_UNEXPECTED;
      }
      return visitor(row.cells_[1].get_string(), row.cells_[2].get_string(), stop);
    });
  }
  return ret;
}

int InstanceMetaStore::read(Transaction &tx, MetaCollection collection, const ObString &key,
    ObIAllocator &allocator, ObString &value, bool lock)
{
  value.reset();
  int ret = check(tx, collection, key);
  if (OB_SUCC(ret)) {
    ObObj cells[2];
    cells[0].set_uint64(static_cast<uint64_t>(collection));
    cells[1].set_varbinary(key);
    ObRowkey rowkey(cells, 2);
    ObNewRow row;
    ret = lock ? table_.get_for_update(tx.native_, bound_tables_[static_cast<int>(InstanceTable::STORAGE_LAYOUT_KV)], rowkey, allocator, row)
               : table_.get(tx.native_, bound_tables_[static_cast<int>(InstanceTable::STORAGE_LAYOUT_KV)], rowkey, allocator, row);
    if (OB_SUCC(ret)) { value = row.cells_[2].get_string(); }
  }
  return ret;
}
int InstanceMetaStore::get(Transaction &tx, MetaCollection collection, const ObString &key,
    ObIAllocator &allocator, ObString &value)
{ return read(tx, collection, key, allocator, value, false); }
int InstanceMetaStore::get_for_update(Transaction &tx, MetaCollection collection, const ObString &key,
    ObIAllocator &allocator, ObString &value)
{ return read(tx, collection, key, allocator, value, true); }

int InstanceMetaStore::write(Transaction &tx, MetaCollection collection, const ObString &key,
    const ObString &value, Write operation)
{
  int ret = check(tx, collection, key);
  if (OB_SUCC(ret) && (value.length() < 0 || (value.length() > 0 && value.ptr() == nullptr))) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_SUCC(ret) && value.length() > MAX_VALUE_LENGTH) { ret = OB_SIZE_OVERFLOW; }
  if (OB_SUCC(ret)) {
    ObObj cells[3];
    cells[0].set_uint64(static_cast<uint64_t>(collection));
    cells[1].set_varbinary(key);
    cells[2].set_varbinary(value);
    ObNewRow row;
    row.assign(cells, 3);
    ret = operation == Write::INSERT ? table_.insert(tx.native_, bound_tables_[static_cast<int>(InstanceTable::STORAGE_LAYOUT_KV)], row) : table_.put(tx.native_, bound_tables_[static_cast<int>(InstanceTable::STORAGE_LAYOUT_KV)], row);
  }
  return ret;
}
int InstanceMetaStore::insert(Transaction &tx, MetaCollection collection, const ObString &key,
    const ObString &value)
{ return write(tx, collection, key, value, Write::INSERT); }
int InstanceMetaStore::put(Transaction &tx, MetaCollection collection, const ObString &key,
    const ObString &value)
{ return write(tx, collection, key, value, Write::PUT); }
int InstanceMetaStore::erase(Transaction &tx, MetaCollection collection, const ObString &key, bool &existed)
{
  existed = false;
  int ret = check(tx, collection, key);
  if (OB_SUCC(ret)) {
    ObObj cells[2];
    cells[0].set_uint64(static_cast<uint64_t>(collection));
    cells[1].set_varbinary(key);
    ret = table_.erase(tx.native_, bound_tables_[static_cast<int>(InstanceTable::STORAGE_LAYOUT_KV)], ObRowkey(cells, 2), existed);
  }
  return ret;
}
} // namespace storage
} // namespace oceanbase
