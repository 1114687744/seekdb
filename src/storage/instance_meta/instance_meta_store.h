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

#ifndef OCEANBASE_STORAGE_INSTANCE_META_STORE_H_
#define OCEANBASE_STORAGE_INSTANCE_META_STORE_H_

#include <functional>
#include <memory>
#include "common/ob_tablet_id.h"
#include "storage/access/native_inner_table.h"
#include "lib/string/ob_string.h"
#include "share/instance_meta/instance_meta_collection.h"

namespace oceanbase
{
namespace common { class ObIAllocator; }
namespace share { namespace schema { class ObTableSchema; } }
namespace transaction { class ObTransService; class ObTxDesc; }
namespace storage
{
class ObAccessService;

using share::instance_meta::MetaCollection;

class InstanceMetaStore final
{
public:
  static constexpr int64_t MAX_KEY_LENGTH = 512;
  static constexpr int64_t MAX_VALUE_LENGTH = 64 * 1024;
  static constexpr int64_t SCHEMA_VERSION = 1;

  class Transaction final
  {
  public:
    Transaction();
    ~Transaction(); // Rolls back an owned transaction, or detaches a borrowed one.
    bool is_active() const { return native_.is_active(); }
    bool is_directory_gc() const { return directory_gc_; }
    const share::SCN &snapshot_version() const { return native_.snapshot_version(); }
  private:
    friend class InstanceMetaStore;
    InstanceMetaStore *owner_;
    NativeInnerTable::Transaction native_;
    bool directory_guard_;
    bool directory_gc_;
    DISALLOW_COPY_AND_ASSIGN(Transaction);
  };

  using Snapshot = NativeInnerTable::Snapshot;
  using SnapshotHandle = NativeInnerTable::SnapshotHandle;
  int retain_snapshot(const Transaction &tx, SnapshotHandle &snapshot);

  // Bounds apply to the key within one collection. Empty byte strings are
  // valid keys; bound presence is therefore separate from bound contents.
  struct KeyRange
  {
    common::ObString lower;
    common::ObString upper;
    bool has_lower = false;
    bool has_upper = false;
    bool include_lower = true;
    bool include_upper = false;
  };

  // Row bytes are borrowed for the duration of the callback. Set stop to end
  // the scan successfully. The callback must not reenter this transaction.
  using RowVisitor = std::function<int(const common::ObString &key,
                                      const common::ObString &value, bool &stop)>;

  InstanceMetaStore(ObAccessService &access, transaction::ObTransService &transactions);
  ~InstanceMetaStore();
  int init(const common::ObTabletID &tablet_id);
  static int build_schema(const common::ObTabletID &tablet_id,
                          share::schema::ObTableSchema &schema);

  int begin(Transaction &tx, int64_t deadline, bool read_only = false);
  // Acquire an external read timestamp while page GC is excluded and MVCC
  // retention is registered. acquire must obtain a new timestamp, or one
  // already protected by its owner; an unprotected arbitrary old SCN is invalid.
  // The transaction is owned and read-only, even when the timestamp comes from
  // a user's native transaction. It never observes that user's uncommitted rows.
  using SnapshotAcquirer = std::function<int(share::SCN &)>;
  int begin_read(Transaction &tx, int64_t deadline, const SnapshotAcquirer &acquire);
  // Read at the local horizon available to a newly admitted weak reader.
  // Retention collectors need this view in addition to the latest committed one.
  int begin_weak_read(Transaction &tx, int64_t deadline);
  // Excludes ordinary KV transactions for the collector's final root validation
  // and bounded page deletion. Full traversal uses an ordinary read transaction.
  int begin_directory_gc(Transaction &tx, int64_t deadline);
  // Borrow an active native RC transaction. Its owner must end it before
  // detaching, so snapshot retention and directory GC protection span commit.
  // Only the owner may commit/rollback/release the native descriptor.
  int attach(Transaction &tx, transaction::ObTxDesc &descriptor, int64_t deadline);
  // Join another KV tablet's transaction using the same native owner. As for
  // the descriptor overload, end the owner before detaching this participant.
  int attach(Transaction &tx, const Transaction &owner, int64_t deadline);
  int detach(Transaction &tx);
  int commit(Transaction &tx);
  int rollback(Transaction &tx);
  // Compaction retains active KV transactions and independent snapshot handles.
  int min_retained_snapshot(share::SCN &snapshot);
  int get(Transaction &tx, MetaCollection collection, const common::ObString &key,
          common::ObIAllocator &allocator, common::ObString &value);
  int get_for_update(Transaction &tx, MetaCollection collection, const common::ObString &key,
                     common::ObIAllocator &allocator, common::ObString &value);
  int insert(Transaction &tx, MetaCollection collection, const common::ObString &key,
             const common::ObString &value);
  int put(Transaction &tx, MetaCollection collection, const common::ObString &key,
          const common::ObString &value);
  int erase(Transaction &tx, MetaCollection collection, const common::ObString &key,
            bool &existed);
  int scan(Transaction &tx, MetaCollection collection, const KeyRange &range,
           const RowVisitor &visitor);

private:
  enum class Write { INSERT, PUT };
  struct State;
  int check(const Transaction &tx, MetaCollection collection, const common::ObString &key) const;
  int read(Transaction &tx, MetaCollection collection, const common::ObString &key,
           common::ObIAllocator &allocator, common::ObString &value, bool lock);
  int write(Transaction &tx, MetaCollection collection, const common::ObString &key,
            const common::ObString &value, Write operation);
  int end(Transaction &tx, bool commit);
  int begin_impl(Transaction &tx, int64_t deadline, bool directory_gc,
                 const std::function<int()> &begin_native);
  void release_directory_guard(Transaction &tx);
  NativeInnerTable table_;
  State *state_;
  DISALLOW_COPY_AND_ASSIGN(InstanceMetaStore);
};

} // namespace storage
} // namespace oceanbase
#endif
