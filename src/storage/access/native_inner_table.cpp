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
#include "storage/access/native_inner_table.h"
#include <new>
#include <mutex>
#include <unordered_map>
#include "share/schema/ob_table_schema.h"
#include "storage/access/ob_dml_table_plan_access.h"
#include "storage/access/ob_dml_param.h"
#include "storage/access/ob_table_scan_iterator.h"
#include "storage/blocksstable/ob_datum_row.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/tx_storage/ob_access_service.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
using namespace transaction;
namespace storage
{
struct NativeInnerTable::ReadSnapshots
{
  std::mutex mutex;
  Transaction *transactions = nullptr;
  std::unordered_map<const Snapshot *, share::SCN> retained;
};

NativeInnerTable::Snapshot::Snapshot(std::shared_ptr<ReadSnapshots> readers,
    const share::SCN &version) : readers_(std::move(readers)), version_(version)
{
  std::lock_guard<std::mutex> guard(readers_->mutex);
  readers_->retained.emplace(this, version_);
}

NativeInnerTable::Snapshot::~Snapshot()
{
  std::lock_guard<std::mutex> guard(readers_->mutex);
  readers_->retained.erase(this);
}

struct NativeInnerTable::State
{
  ObArenaAllocator allocator{ObMemAttr("NativeInnerTbl")};
  ObTableSchema schema{&allocator};
  ObTableParam read_plan{allocator};
  data_plane::ObDmlTablePlan write_plan{allocator};
  ObSEArray<uint64_t, 8> columns;
  ObSEArray<ObColDesc, 8> descriptions;
  ObTabletID tablet;
  std::shared_ptr<ReadSnapshots> readers = std::make_shared<ReadSnapshots>();
};

NativeInnerTable::Transaction::Transaction()
  : owner_(nullptr), descriptor_(nullptr), snapshot_(), deadline_(0),
    read_only_(false), scanning_(false), borrowed_(false), previous_(nullptr), next_(nullptr)
{}

NativeInnerTable::Transaction::~Transaction()
{
  if (descriptor_ != nullptr) {
    const int ret = borrowed_ ? owner_->detach(*this) : owner_->rollback(*this);
    if (OB_SUCCESS != ret) { LOG_WARN("native inner table transaction cleanup failed", K(ret)); }
  }
}

NativeInnerTable::NativeInnerTable(ObAccessService &access, ObTransService &transactions)
  : access_(access), transactions_(transactions), state_(nullptr)
{}

NativeInnerTable::~NativeInnerTable() { delete state_; }

int NativeInnerTable::init(const ObTabletID &tablet_id, const ObTableSchema &schema)
{
  int ret = OB_SUCCESS;
  State *state = nullptr;
  if (state_ != nullptr) {
    ret = OB_INIT_TWICE;
  } else if (!tablet_id.is_valid() || schema.get_table_id() == OB_INVALID_ID
             || schema.get_schema_version() <= 0 || schema.get_rowkey_column_num() <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (schema.is_partitioned_table() || schema.is_index_table()
             || schema.get_index_tid_count() != 0 || schema.has_generated_column()
             || schema.get_constraint_count() != 0 || schema.get_trigger_list().count() != 0
             || schema.get_foreign_key_infos().count() != 0) {
    ret = OB_NOT_SUPPORTED;
  } else if (nullptr == (state = new (std::nothrow) State())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(state->schema.assign(schema))) {
  } else if (OB_FAIL(schema.get_column_ids(state->descriptions))) {
  } else {
    state->tablet = tablet_id;
    for (int64_t i = 0; OB_SUCC(ret) && i < state->descriptions.count(); ++i) {
      const auto &desc = state->descriptions.at(i);
      const auto *column = schema.get_column_schema(desc.col_id_);
      if (column == nullptr) {
        ret = OB_ERR_UNEXPECTED;
      } else if ((!ob_is_integer_type(desc.col_type_.get_type())
                  && !desc.col_type_.is_varbinary())
                 || column->is_autoincrement() || column->is_generated_column()
                 || column->is_on_update_current_timestamp()
                 || (i < schema.get_rowkey_column_num() && desc.col_order_ != ASC)) {
        ret = OB_NOT_SUPPORTED;
      } else {
        ret = state->columns.push_back(desc.col_id_);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(state->read_plan.convert(state->schema, state->columns,
                                               sql::ObStoragePushdownFlag()))) {
    } else if (OB_FAIL(state->write_plan.build(&state->schema, schema.get_schema_version(), state->columns))) {
    } else {
      state_ = state;
      state = nullptr;
    }
  }
  delete state;
  return ret;
}

int NativeInnerTable::begin(Transaction &tx, const int64_t deadline, const bool read_only)
{
  return begin_impl(tx, deadline, read_only);
}

int NativeInnerTable::begin_read(Transaction &tx, int64_t deadline,
    const SnapshotAcquirer &acquire)
{
  return acquire ? begin_impl(tx, deadline, true, nullptr, &acquire)
                 : OB_INVALID_ARGUMENT;
}

int NativeInnerTable::begin_weak_read(Transaction &tx, int64_t deadline)
{
  return begin_read(tx, deadline, [&](share::SCN &snapshot) {
    return transactions_.get_weak_read_snapshot_version(-1, snapshot);
  });
}

int NativeInnerTable::attach(Transaction &tx, ObTxDesc &descriptor, const int64_t deadline)
{
  if (!descriptor.is_in_tx() || descriptor.is_shadow()
      || !descriptor.is_RC_isolevel()) {
    return OB_INVALID_ARGUMENT;
  }
  return begin_impl(tx, deadline, descriptor.is_rdonly(), &descriptor);
}

int NativeInnerTable::attach(Transaction &tx, const Transaction &owner, const int64_t deadline)
{
  if (!owner.is_active() || owner.owner_ == nullptr || owner.scanning_
      || owner.owner_ == this || &owner.owner_->transactions_ != &transactions_) {
    return OB_INVALID_ARGUMENT;
  }
  return attach(tx, *owner.descriptor_, std::min(deadline, owner.deadline_));
}

int NativeInnerTable::begin_impl(Transaction &tx, const int64_t deadline,
    const bool read_only, ObTxDesc *borrowed,
    const SnapshotAcquirer *acquire)
{
  int ret = OB_SUCCESS;
  if (state_ == nullptr) {
    ret = OB_NOT_INIT;
  } else if (tx.is_active()) {
    ret = OB_INIT_TWICE;
  } else if (deadline <= ObTimeUtility::current_time()) {
    ret = OB_TIMEOUT;
  }
  if (ret == OB_SUCCESS) {
    ObTxParam param;
    param.access_mode_ = read_only ? ObTxAccessMode::RD_ONLY : ObTxAccessMode::RW;
    param.isolation_ = ObTxIsolationLevel::RC;
    param.timeout_us_ = deadline - ObTimeUtility::current_time();
    tx.descriptor_ = borrowed;
    if (borrowed == nullptr && OB_FAIL(transactions_.acquire_tx(tx.descriptor_))) {
    } else {
      tx.borrowed_ = borrowed != nullptr;
      tx.owner_ = this;
      tx.deadline_ = deadline;
      tx.read_only_ = read_only;
      // Register before acquiring a timestamp. A not-yet-published snapshot
      // conservatively prevents compaction from advancing past this reader.
      {
        std::lock_guard<std::mutex> guard(state_->readers->mutex);
        tx.next_ = state_->readers->transactions;
        if (tx.next_ != nullptr) { tx.next_->previous_ = &tx; }
        state_->readers->transactions = &tx;
      }
      ObTxReadSnapshot snapshot;
      if (!tx.borrowed_ && OB_FAIL(transactions_.start_tx(*tx.descriptor_, param))) {
      } else if (OB_FAIL(transactions_.get_read_snapshot(*tx.descriptor_, param.isolation_,
                                                        deadline, snapshot))) {
      } else if (acquire != nullptr) {
        share::SCN selected;
        if (OB_FAIL((*acquire)(selected))) {
        } else if (!selected.is_valid() || selected.is_min() || selected.is_max()) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          snapshot.core_.version_ = selected;
        }
      }
      if (OB_SUCC(ret)) {
        std::lock_guard<std::mutex> guard(state_->readers->mutex);
        ret = tx.snapshot_.assign(snapshot);
      }
      if (OB_FAIL(ret)) {
        const int cleanup_ret = tx.borrowed_ ? detach(tx) : rollback(tx);
        if (cleanup_ret != OB_SUCCESS) { LOG_WARN("failed to close metadata transaction", K(cleanup_ret)); }
      }
    }
  }
  return ret;
}

int NativeInnerTable::end(Transaction &tx, const bool do_commit)
{
  int ret = OB_SUCCESS;
  if (tx.owner_ != this || !tx.is_active() || tx.scanning_ || tx.borrowed_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ret = do_commit ? transactions_.commit_tx(*tx.descriptor_, tx.deadline_)
                    : transactions_.rollback_tx(*tx.descriptor_);
    const int release_ret = transactions_.release_tx(*tx.descriptor_);
    if (ret == OB_SUCCESS) { ret = release_ret; }
    reset_transaction(tx);
  }
  return ret;
}

int NativeInnerTable::detach(Transaction &tx)
{
  if (tx.owner_ != this || !tx.is_active() || tx.scanning_ || !tx.borrowed_) {
    return OB_INVALID_ARGUMENT;
  }
  reset_transaction(tx);
  return OB_SUCCESS;
}

void NativeInnerTable::reset_transaction(Transaction &tx)
{
  // The owner may already have released the native descriptor. Cleanup only
  // touches our reader registration and never dereferences that descriptor.
  {
    std::lock_guard<std::mutex> guard(state_->readers->mutex);
    if (tx.previous_ != nullptr) { tx.previous_->next_ = tx.next_; }
    else { state_->readers->transactions = tx.next_; }
    if (tx.next_ != nullptr) { tx.next_->previous_ = tx.previous_; }
    tx.previous_ = tx.next_ = nullptr;
    tx.descriptor_ = nullptr;
    tx.owner_ = nullptr;
    tx.borrowed_ = false;
    tx.snapshot_.reset();
  }
}

int NativeInnerTable::commit(Transaction &tx) { return end(tx, true); }
int NativeInnerTable::rollback(Transaction &tx) { return end(tx, false); }

int NativeInnerTable::retain_snapshot(const Transaction &tx, SnapshotHandle &snapshot)
{
  if (state_ == nullptr) { return OB_NOT_INIT; }
  if (snapshot || tx.owner_ != this || !tx.is_active() || !tx.snapshot_.is_valid()) {
    return OB_INVALID_ARGUMENT;
  }
  // tx remains registered throughout construction: no gap between the short
  // transaction's retention and the independent handle's retention.
  auto *retained = new (std::nothrow) Snapshot(state_->readers, tx.snapshot_.core_.version_);
  if (retained == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  snapshot.reset(retained);
  return OB_SUCCESS;
}

int NativeInnerTable::min_retained_snapshot(share::SCN &snapshot)
{
  int ret = OB_SUCCESS;
  share::SCN weak_snapshot;
  if (state_ == nullptr) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(transactions_.get_read_snapshot_version(
      ObTimeUtility::current_time() + 1000000, snapshot))) {
  } else if (OB_FAIL(transactions_.get_weak_read_snapshot_version(-1, weak_snapshot))) {
  } else {
    // Future weak readers can select an older timestamp than a strong reader.
    // Sample both native horizons before inspecting registered readers. Reads
    // at an older fixed snapshot must already have a transaction or lease.
    if (weak_snapshot < snapshot) { snapshot = weak_snapshot; }
    std::lock_guard<std::mutex> guard(state_->readers->mutex);
    for (Transaction *tx = state_->readers->transactions; tx != nullptr; tx = tx->next_) {
      if (!tx->snapshot_.is_valid()) { snapshot.set_min(); }
      else if (tx->snapshot_.core_.version_ < snapshot) { snapshot = tx->snapshot_.core_.version_; }
    }
    for (const auto &held : state_->readers->retained) {
      if (held.second < snapshot) { snapshot = held.second; }
    }
  }
  return ret;
}

int NativeInnerTable::check(const Transaction &tx, const bool write) const
{
  if (state_ == nullptr) { return OB_NOT_INIT; }
  if (tx.owner_ != this || !tx.is_active() || tx.scanning_ || (write && tx.read_only_)) {
    return OB_INVALID_ARGUMENT;
  }
  return tx.deadline_ <= ObTimeUtility::current_time() ? OB_TIMEOUT : OB_SUCCESS;
}

int NativeInnerTable::check_cells(const ObObj *cells, int64_t count, bool key, bool bounds) const
{
  if (state_ == nullptr) { return OB_NOT_INIT; }
  const int64_t expected = key ? state_->schema.get_rowkey_column_num() : state_->columns.count();
  if (cells == nullptr || count != expected) { return OB_INVALID_ARGUMENT; }
  for (int64_t i = 0; i < count; ++i) {
    const auto &cell = cells[i];
    const auto &desc = state_->descriptions.at(i);
    const auto *column = state_->schema.get_column_schema(desc.col_id_);
    if (bounds && (cell.is_min_value() || cell.is_max_value())) { continue; }
    if (cell.is_null()) {
      if (i < state_->schema.get_rowkey_column_num() || !column->is_nullable()) {
        return OB_INVALID_ARGUMENT;
      }
    } else if (cell.get_type() != desc.col_type_.get_type()
        || (cell.is_string_type() && cell.get_collation_type() != desc.col_type_.get_collation_type())) {
      return OB_INVALID_ARGUMENT;
    } else if (cell.is_string_type()) {
      const auto &bytes = cell.get_string();
      if (bytes.length() < 0 || (bytes.length() > 0 && bytes.ptr() == nullptr)) {
        return OB_INVALID_ARGUMENT;
      }
      if (bytes.length() > column->get_data_length()) { return OB_SIZE_OVERFLOW; }
    }
  }
  return OB_SUCCESS;
}

int NativeInnerTable::scan_rows(Transaction &tx, const ObNewRange &range,
    const RowVisitor &visitor, bool latest, bool reverse)
{
  int ret = check(tx, false);
  if (OB_SUCC(ret) && !visitor) { ret = OB_INVALID_ARGUMENT; }
  if (OB_SUCC(ret)) {
    ret = check_cells(range.start_key_.get_obj_ptr(), range.start_key_.get_obj_cnt(), true, true);
  }
  if (OB_SUCC(ret)) {
    ret = check_cells(range.end_key_.get_obj_ptr(), range.end_key_.get_obj_cnt(), true, true);
  }
  if (OB_SUCC(ret)) {
    ObArenaAllocator allocator(ObMemAttr("NativeInnerRd"));
    ObTableScanParam param;
    ObNewRange keys = range;
    keys.table_id_ = state_->schema.get_table_id();
    param.index_id_ = state_->schema.get_table_id();
    param.tablet_id_ = state_->tablet;
    param.schema_tablet_id_ = state_->tablet;
    param.table_param_ = &state_->read_plan;
    param.schema_version_ = state_->schema.get_schema_version();
    param.runtime_schema_version_ = state_->schema.get_schema_version();
    param.timeout_ = tx.deadline_;
    param.tx_lock_timeout_ = tx.deadline_;
    param.trans_desc_ = tx.descriptor_;
    param.tx_id_ = tx.descriptor_->get_tx_id();
    param.scan_flag_.scan_order_ = reverse ? ObQueryFlag::Reverse : ObQueryFlag::Forward;
    param.allocator_ = &allocator;
    param.scan_allocator_ = &allocator;
    param.reserved_cell_count_ = state_->columns.count();
    param.limit_param_.limit_ = -1;
    param.limit_param_.offset_ = 0;
    // The current transaction sequence includes preceding writes. Ordinary
    // reads retain the pinned commit snapshot; locked reads use the snapshot
    // acquired after obtaining the row lock.
    if (OB_FAIL(transactions_.get_read_snapshot(*tx.descriptor_, ObTxIsolationLevel::RC,
                                               tx.deadline_, param.snapshot_))) {
    }
    if (OB_SUCC(ret) && !latest) { param.snapshot_.core_.version_ = tx.snapshot_.core_.version_; }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(param.column_ids_.assign(state_->columns))) {
    } else if (OB_FAIL(param.key_ranges_.push_back(keys))) {
    } else {
      ObSEArray<ObObj, 8> cells;
      for (int64_t i = 0; OB_SUCC(ret) && i < state_->columns.count(); ++i) {
        ret = cells.push_back(ObObj());
      }
      ObNewRowIterator *iter = nullptr;
      tx.scanning_ = true;
      if (OB_SUCC(ret)) { ret = access_.table_scan(param, iter); }
      bool stop = false;
      blocksstable::ObDatumRow *row = nullptr;
      while (OB_SUCC(ret) && !stop
          && OB_SUCC(ret = static_cast<ObTableScanIterator *>(iter)->get_next_row(row))) {
        if (row == nullptr || row->get_column_count() != cells.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < cells.count(); ++i) {
            ret = row->storage_datums_[i].to_obj_enhance(cells.at(i), state_->descriptions.at(i).col_type_);
          }
          ObNewRow result;
          result.assign(cells.get_data(), cells.count());
          if (OB_SUCC(ret)) { ret = visitor(result, stop); }
        }
      }
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      if (iter != nullptr) {
        const int cleanup_ret = access_.revert_scan_iter(iter);
        if (OB_SUCC(ret)) { ret = cleanup_ret; }
      }
      tx.scanning_ = false;
    }
  }
  return ret;
}

int NativeInnerTable::scan(Transaction &tx, const ObNewRange &range,
    const RowVisitor &visitor, bool reverse)
{ return scan_rows(tx, range, visitor, false, reverse); }

int NativeInnerTable::read(Transaction &tx, const ObRowkey &key,
    ObIAllocator &allocator, ObNewRow &row, bool latest)
{
  row.reset();
  int ret = check(tx, false);
  if (OB_SUCC(ret)) { ret = check_cells(key.get_obj_ptr(), key.get_obj_cnt(), true, false); }
  if (OB_SUCC(ret)) {
    ObNewRange range;
    range.start_key_ = range.end_key_ = key;
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();
    bool found = false;
    ret = scan_rows(tx, range, [&](const ObNewRow &result, bool &stop) {
      found = stop = true;
      return ob_write_row(allocator, result, row);
    }, latest, false);
    if (OB_SUCC(ret) && !found) { ret = OB_ENTRY_NOT_EXIST; }
  }
  return ret;
}

int NativeInnerTable::get(Transaction &tx, const ObRowkey &key,
    ObIAllocator &allocator, ObNewRow &row)
{ return read(tx, key, allocator, row, false); }

int NativeInnerTable::write(Transaction &tx, const ObNewRow &row, Write operation)
{
  int ret = check(tx, true);
  if (OB_SUCC(ret) && row.projector_size_ != 0) { ret = OB_INVALID_ARGUMENT; }
  if (OB_SUCC(ret)) { ret = check_cells(row.cells_, row.count_, operation == Write::LOCK, false); }
  if (OB_SUCC(ret)) {
    ObArenaAllocator allocator(ObMemAttr("NativeInnerWr"));
    ObStoreCtxGuard context;
    ObDMLBaseParam param;
    ObTimeZoneInfo timezone;
    ObTxReadSnapshot snapshot;
    // Each write uses a current snapshot, while ordinary reads retain the
    // transaction's pinned snapshot. The engine arbitrates concurrent writers.
    if (OB_FAIL(transactions_.get_read_snapshot(*tx.descriptor_, ObTxIsolationLevel::RC,
                                               tx.deadline_, snapshot))) {
    } else if (OB_FAIL(access_.get_write_store_ctx_guard(tx.deadline_, *tx.descriptor_,
                       snapshot, 0, param.write_flag_, context))) {
    } else {
      param.timeout_ = tx.deadline_;
      param.schema_version_ = state_->schema.get_schema_version();
      param.runtime_schema_version_ = state_->schema.get_schema_version();
      param.tz_info_ = &timezone;
      param.table_param_ = ObDmlTablePlanAccess::get(state_->write_plan);
      param.check_schema_version_ = false;
      param.dml_allocator_ = &allocator;
      param.store_ctx_guard_ = &context;
      ret = param.snapshot_.assign(snapshot);
      class Row final : public blocksstable::ObDatumRowIterator
      {
      public:
        blocksstable::ObDatumRow row;
        bool consumed = false;
        int get_next_row(blocksstable::ObDatumRow *&out) override {
          if (consumed) { return OB_ITER_END; }
          consumed = true;
          out = &row;
          return OB_SUCCESS;
        }
      } rows;
      if (OB_SUCC(ret)) { ret = rows.row.init(row.count_); }
      for (int64_t i = 0; OB_SUCC(ret) && i < row.count_; ++i) {
        ret = rows.row.storage_datums_[i].from_obj_enhance(row.cells_[i]);
      }
      rows.row.row_flag_.set_flag(blocksstable::ObDmlFlag::DF_INSERT);
      int64_t affected = 0;
      if (OB_FAIL(ret)) {
      } else if (operation == Write::INSERT) {
        ret = access_.insert_rows(state_->tablet, *tx.descriptor_, param, state_->columns, &rows, affected);
      } else if (operation == Write::PUT) {
        ret = access_.put_rows(state_->tablet, *tx.descriptor_, param, state_->columns, &rows, affected);
      } else if (operation == Write::ERASE) {
        ret = access_.delete_rows(state_->tablet, *tx.descriptor_, param, state_->columns, &rows, affected);
      } else {
        ret = access_.lock_rows(state_->tablet, *tx.descriptor_, param, tx.deadline_, LF_NONE, &rows, affected);
      }
    }
    // context is destroyed before the caller can commit or start another
    // operation, merging its write sequence/result into the transaction.
  }
  return ret;
}

int NativeInnerTable::get_for_update(Transaction &tx, const ObRowkey &key,
    ObIAllocator &allocator, ObNewRow &row)
{
  row.reset();
  ObNewRow keys;
  keys.assign(const_cast<ObObj *>(key.get_obj_ptr()), key.get_obj_cnt());
  int ret = OB_SUCCESS;
  // Retry only acquisition of the row lock. Preserve the caller's native
  // transaction, preceding writes and original deadline.
  while (OB_TRY_LOCK_ROW_CONFLICT == (ret = write(tx, keys, Write::LOCK))) {
    if (ObTimeUtility::current_time() >= tx.deadline_) {
      ret = OB_TIMEOUT;
      break;
    } else if (OB_FAIL(THIS_WORKER.check_status())) {
      break;
    }
    ob_usleep(1000);
  }
  if (OB_SUCC(ret)) { ret = read(tx, key, allocator, row, true); }
  return ret;
}
int NativeInnerTable::insert(Transaction &tx, const ObNewRow &row)
{ return write(tx, row, Write::INSERT); }
int NativeInnerTable::put(Transaction &tx, const ObNewRow &row)
{ return write(tx, row, Write::PUT); }
int NativeInnerTable::erase(Transaction &tx, const ObRowkey &key, bool &existed)
{
  ObArenaAllocator allocator(ObMemAttr("NativeInnerDel"));
  ObNewRow old_row;
  int ret = get_for_update(tx, key, allocator, old_row);
  existed = ret == OB_SUCCESS;
  if (ret == OB_ENTRY_NOT_EXIST) { ret = OB_SUCCESS; }
  else if (OB_SUCC(ret)) { ret = write(tx, old_row, Write::ERASE); }
  return ret;
}

} // namespace storage
} // namespace oceanbase
