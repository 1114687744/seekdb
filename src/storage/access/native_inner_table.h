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

#ifndef OCEANBASE_STORAGE_NATIVE_INNER_TABLE_H_
#define OCEANBASE_STORAGE_NATIVE_INNER_TABLE_H_

#include <functional>
#include <memory>
#include "common/ob_tablet_id.h"
#include "common/ob_range.h"
#include "common/row/ob_row.h"
#include "data_plane/transaction/ob_tx_read_snapshot.h"

namespace oceanbase
{
namespace share { namespace schema { class ObTableSchema; } }
namespace transaction { class ObTransService; class ObTxDesc; }
namespace storage
{
class ObAccessService;

// Native access to one explicitly bound, fixed-layout inner tablet. The caller
// admits the physical target and keeps it alive; this module does not resolve
// Namespace inheritance or create tablets. No SQL runtime is required.
// One instance owns the tablet's reader registry and outlives its transactions.
// The tablet's compaction retention must include min_retained_snapshot(); the
// existing instance-metadata adapter provides that integration for its tablets.
//
// Rows contain the primary-key columns first, in key order, followed by the
// remaining columns in schema order. Every write supplies a complete row.
// The first implementation supports integer and binary string columns, NULL
// for nullable non-key columns, and ascending primary keys. Unsupported table
// features are rejected at init; no SQL expressions/defaults are evaluated.
class NativeInnerTable final
{
  struct ReadSnapshots;
public:
  class Transaction final
  {
  public:
    Transaction();
    ~Transaction();
    bool is_active() const { return descriptor_ != nullptr; }
    bool is_borrowed() const { return borrowed_; }
    const share::SCN &snapshot_version() const { return snapshot_.core_.version_; }
  private:
    friend class NativeInnerTable;
    NativeInnerTable *owner_;
    transaction::ObTxDesc *descriptor_;
    transaction::ObTxReadSnapshot snapshot_;
    int64_t deadline_;
    bool read_only_;
    bool scanning_;
    bool borrowed_;
    Transaction *previous_;
    Transaction *next_;
    DISALLOW_COPY_AND_ASSIGN(Transaction);
  };

  class Snapshot final
  {
  public:
    ~Snapshot();
    const share::SCN &version() const { return version_; }
  private:
    friend class NativeInnerTable;
    Snapshot(std::shared_ptr<ReadSnapshots> readers, const share::SCN &version);
    std::shared_ptr<ReadSnapshots> readers_;
    share::SCN version_;
    DISALLOW_COPY_AND_ASSIGN(Snapshot);
  };
  using SnapshotHandle = std::shared_ptr<const Snapshot>;
  using SnapshotAcquirer = std::function<int(share::SCN &)>;
  // Cells, including variable-length bytes, are borrowed until callback return.
  // stop ends a scan successfully. Reentry on the same transaction is rejected.
  using RowVisitor = std::function<int(const common::ObNewRow &, bool &stop)>;

  NativeInnerTable(ObAccessService &, transaction::ObTransService &);
  ~NativeInnerTable();
  int init(const common::ObTabletID &, const share::schema::ObTableSchema &);

  int begin(Transaction &, int64_t deadline, bool read_only = false);
  // acquire selects a new timestamp or one already protected by its owner.
  // Register retention before acquisition; arbitrary unprotected old SCNs
  // cannot be made readable by this method. The transaction is read-only.
  int begin_read(Transaction &, int64_t deadline, const SnapshotAcquirer &acquire);
  int begin_weak_read(Transaction &, int64_t deadline);
  // Only the external owner ends/releases a borrowed native transaction.
  // End that owner before detach so retention covers its commit. Detach does
  // not dereference the descriptor, which may already have been released.
  int attach(Transaction &, transaction::ObTxDesc &, int64_t deadline);
  int attach(Transaction &, const Transaction &owner, int64_t deadline);
  int detach(Transaction &);
  int commit(Transaction &);
  int rollback(Transaction &);
  int retain_snapshot(const Transaction &, SnapshotHandle &);
  int min_retained_snapshot(share::SCN &);

  int get(Transaction &, const common::ObRowkey &, common::ObIAllocator &, common::ObNewRow &);
  int get_for_update(Transaction &, const common::ObRowkey &, common::ObIAllocator &, common::ObNewRow &);
  // Both endpoints have one cell per key column; MIN/MAX cells express open
  // prefix bounds. The range's table_id is ignored: init binds the target.
  int scan(Transaction &, const common::ObNewRange &, const RowVisitor &, bool reverse = false);
  int insert(Transaction &, const common::ObNewRow &);
  int put(Transaction &, const common::ObNewRow &);
  int erase(Transaction &, const common::ObRowkey &, bool &existed);

private:
  enum class Write { INSERT, PUT, ERASE, LOCK };
  struct State;
  int check(const Transaction &, bool write) const;
  int check_cells(const common::ObObj *, int64_t count, bool key, bool bounds) const;
  int read(Transaction &, const common::ObRowkey &, common::ObIAllocator &, common::ObNewRow &, bool latest);
  int scan_rows(Transaction &, const common::ObNewRange &, const RowVisitor &, bool latest, bool reverse);
  int write(Transaction &, const common::ObNewRow &, Write);
  int begin_impl(Transaction &, int64_t deadline, bool read_only,
                 transaction::ObTxDesc *borrowed = nullptr, const SnapshotAcquirer *acquire = nullptr);
  int end(Transaction &, bool commit);
  void reset_transaction(Transaction &);
  ObAccessService &access_;
  transaction::ObTransService &transactions_;
  State *state_;
  DISALLOW_COPY_AND_ASSIGN(NativeInnerTable);
};
} // namespace storage
} // namespace oceanbase
#endif
