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

// Native access to explicitly bound, fixed-layout inner tablets. The caller
// admits the physical target and keeps it alive; this module does not resolve
// Namespace inheritance or create tablets. No SQL runtime is required.
// One instance owns a shared reader registry for all its bound tablets and
// outlives its tables and transactions.
// The tablet's compaction retention must include min_retained_snapshot(); the
// existing instance-metadata adapter provides that integration for its tablets.
//
// Rows contain the primary-key columns first, in key order, followed by the
// remaining columns in schema order. Every write supplies a complete row.
// Every write supplies stored values, including timestamps and defaults. No
// SQL expressions/defaults or on-update expressions are evaluated here.
class NativeInnerTable final
{
  struct ReadSnapshots;
public:
  // A bound physical table; its definition and plans are prepared once.
  class Table final
  {
  public:
    Table();
    ~Table();
  private:
    friend class NativeInnerTable;
    struct State;
    State *state_;
    const NativeInnerTable *owner_;
    DISALLOW_COPY_AND_ASSIGN(Table);
  };

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

  // An upper-layer owner may resolve a read to a different physical source.
  // It must protect that source and snapshot for the whole native operation.
  // The bound tablet remains the schema identity; this type carries no logical
  // namespace or inheritance policy. An invalid cap means no additional cap.
  struct ReadSource final
  {
    common::ObTabletID tablet;
    share::SCN snapshot_cap;
  };

  // A streaming scan. Table, transaction and NativeInnerTable must outlive it.
  // The returned row is borrowed until next()/close(). Close the cursor before
  // another operation on the same transaction; destruction also closes it.
  class Cursor final
  {
  public:
    Cursor();
    ~Cursor();
    int next(const common::ObNewRow *&);
    int close();
  private:
    friend class NativeInnerTable;
    struct State;
    std::unique_ptr<State> state_;
    DISALLOW_COPY_AND_ASSIGN(Cursor);
  };

  NativeInnerTable(ObAccessService &, transaction::ObTransService &);
  ~NativeInnerTable();
  int bind(Table &, const common::ObTabletID &, const share::schema::ObTableSchema &);

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
  int attach(Transaction &, transaction::ObTxDesc &, int64_t deadline, const SnapshotAcquirer &);
  int attach(Transaction &, const Transaction &owner, int64_t deadline);
  // Join storage work (such as CREATE MDS) to this owner. The callback must
  // neither end nor release the descriptor; participants must survive commit.
  int with_native_transaction(Transaction &, const std::function<int(transaction::ObTxDesc &)> &);
  int detach(Transaction &);
  int commit(Transaction &);
  int rollback(Transaction &);
  int retain_snapshot(const Transaction &, SnapshotHandle &);
  int min_retained_snapshot(share::SCN &);

  int get(Transaction &, const Table &, const common::ObRowkey &, common::ObIAllocator &, common::ObNewRow &,
          const ReadSource *source = nullptr);
  // Current committed rows plus own writes. Caller must establish why a
  // moving read is valid (e.g. immutable objects protected against GC).
  int get_current(Transaction &, const Table &, const common::ObRowkey &, common::ObIAllocator &, common::ObNewRow &);
  int get_for_update(Transaction &, const Table &, const common::ObRowkey &, common::ObIAllocator &, common::ObNewRow &);
  // Storage-form rows retain LOB locators for updates/index projections and
  // avoid reading unchanged LOB bodies. The transaction keeps the row locked.
  int get_storage_row_for_update(Transaction &, const Table &, const common::ObRowkey &,
                                common::ObIAllocator &, common::ObNewRow &);
  // Both endpoints have one cell per key column; MIN/MAX cells express open
  // prefix bounds. The range's table_id is ignored: bind selects the target.
  int scan(Transaction &, const Table &, const common::ObNewRange &, const RowVisitor &, bool reverse = false,
           const ReadSource *source = nullptr);
  int open_scan(Transaction &, const Table &, const common::ObNewRange &, Cursor &, bool reverse = false,
                const ReadSource *source = nullptr);
  int insert(Transaction &, const Table &, const common::ObNewRow &);
  // Replace the stored values of an existing row without changing its key.
  // Supports LOB columns through the storage engine's update protocol.
  int update(Transaction &, const Table &, const common::ObNewRow &);
  // Row uses fixed column order. Only the specified non-key columns are read
  // from it; all others retain the locked old row, including LOB locators.
  int update(Transaction &, const Table &, const common::ObNewRow &,
             const common::ObIArray<uint64_t> &column_ids);
  // Blind upsert uses put_rows, which does not support LOB columns. Callers
  // storing LOBs select insert/update within their owning transaction.
  int put(Transaction &, const Table &, const common::ObNewRow &);
  int erase(Transaction &, const Table &, const common::ObRowkey &, bool &existed);
  // Normal local indexes are serialized by their base row, not index locks.
  // The caller holds that row's write protection in this transaction and
  // supplies its old projected index row. LOB-bearing indexes are unsupported.
  int erase_index_row(Transaction &, const Table &, const common::ObNewRow &);

private:
  enum class Write { INSERT, PUT, UPDATE, ERASE, LOCK };
  int check(const Transaction &, bool write) const;
  int check_table(const Table &) const;
  int check_cells(const Table &, const common::ObObj *, int64_t count, bool key, bool bounds) const;
  int read(Transaction &, const Table &, const common::ObRowkey &, common::ObIAllocator &, common::ObNewRow &, bool latest,
           const ReadSource *source = nullptr, bool raw_lob = false);
  int scan_rows(Transaction &, const Table &, const common::ObNewRange &, const RowVisitor &, bool latest, bool reverse,
                const ReadSource *source = nullptr);
  int open_scan_impl(Transaction &, const Table &, const common::ObNewRange &, Cursor &, bool latest, bool reverse,
                     const ReadSource *source = nullptr, bool raw_lob = false);
  int write(Transaction &, const Table &, const common::ObNewRow &, Write,
            const common::ObNewRow *old_row = nullptr, const common::ObIArray<uint64_t> *column_ids = nullptr);
  int update_impl(Transaction &, const Table &, const common::ObNewRow &, const common::ObIArray<uint64_t> *);
  int lock_and_read(Transaction &, const Table &, const common::ObRowkey &,
                     common::ObIAllocator &, common::ObNewRow &, bool raw_lob);
  int begin_impl(Transaction &, int64_t deadline, bool read_only,
                 transaction::ObTxDesc *borrowed = nullptr, const SnapshotAcquirer *acquire = nullptr);
  int end(Transaction &, bool commit);
  void reset_transaction(Transaction &);
  ObAccessService &access_;
  transaction::ObTransService &transactions_;
  std::shared_ptr<ReadSnapshots> readers_;
  DISALLOW_COPY_AND_ASSIGN(NativeInnerTable);
};
} // namespace storage
} // namespace oceanbase
#endif
