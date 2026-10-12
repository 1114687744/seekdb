/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifndef OCEANBASE_OBSERVER_NATIVE_SCHEMA_CATALOG_H_
#define OCEANBASE_OBSERVER_NATIVE_SCHEMA_CATALOG_H_

#include <functional>
#include <memory>
#include "common/ob_range.h"
#include "common/row/ob_row.h"
#include "namespace/catalog.h"

namespace oceanbase {
namespace common { class ObISQLClient; }
namespace share { namespace schema {
class ObTableSchema;
class ObColumnSchemaV2;
class ObSimpleTableSchemaV2;
class ObTableLatestSchemaVersion;
struct VersionHisVal;
} }
namespace storage { class ObAccessService; }
namespace transaction { class ObTransService; class ObTxDesc; }
namespace observer {

// One Namespace directory operation. Fixed inner-table definitions come from
// code; physical locations and inherited snapshot caps come from its protected
// directory view. No SQL connection, runtime schema cache or Namespace service
// activation is needed. The caller owns this object and its operation lifetime.
class NativeSchemaCatalog final
{
public:
  class Cursor final
  {
  public:
    Cursor();
    ~Cursor();
    int next(const common::ObNewRow *&);
    int close();
    const share::schema::ObTableSchema *schema() const;
  private:
    friend class NativeSchemaCatalog;
    struct State;
    std::unique_ptr<State> state_;
    DISALLOW_COPY_AND_ASSIGN(Cursor);
  };
  using RowVisitor = std::function<int(const share::schema::ObTableSchema &,
                                      const common::ObNewRow &, bool &stop)>;
  NativeSchemaCatalog(uint64_t namespace_id, storage::ObAccessService &,
                      transaction::ObTransService &);
  ~NativeSchemaCatalog();
  // Execute in an active DDL owner and retain the directory operation through
  // commit/rollback. This rejects a standalone SQL proxy; it never starts an
  // independent transaction. Namespace is the owner's explicit target.
  static int with_ddl_transaction(common::ObISQLClient &, int64_t deadline,
      const std::function<int(NativeSchemaCatalog &)> &);
  int begin_read(int64_t deadline, bool weak = false);
  // An old snapshot must already be protected by this view's owner.
  int begin_read(int64_t deadline, const ns::NamespaceCatalogViews::Handle &view);
  // Borrow the DDL's native RC owner so catalog reads observe its own writes.
  // The owner must retain this operation until after commit/rollback, then
  // close it. close only detaches; it never ends the external transaction.
  int attach(transaction::ObTxDesc &, int64_t deadline);
  int scan(uint64_t inner_table_id, const common::ObNewRange &range,
           const RowVisitor &, bool reverse = false);
  int open_scan(uint64_t inner_table_id, const common::ObNewRange &, Cursor &, bool reverse = false);
  int open_prefix(uint64_t inner_table_id, const common::ObRowkey &prefix, Cursor &, bool reverse = false);
  // CURRENT requires INT64_MAX; HISTORY selects the latest logical version
  // at/before schema_version within this operation's protected MVCC view.
  enum class TableSource { HISTORY, CURRENT };
  int get_table(uint64_t table_id, int64_t schema_version, common::ObIAllocator &,
                share::schema::ObTableSchema *&, TableSource = TableSource::HISTORY);
  // Core definitions are cell records in __all_core_table. Its own definition
  // is compiled; the returned persisted definitions exclude that fixed table.
  int get_core_tables(int64_t schema_version,
      common::ObIArray<share::schema::ObTableSchema> &);
  // Append simple table definitions and their partition/name relationships.
  // A null selection reads all tables (except the separately loaded core table);
  // an explicit empty selection reads none. All components share this view.
  int get_simple_tables(int64_t schema_version, common::ObIAllocator &,
      common::ObIArray<share::schema::ObSimpleTableSchemaV2 *> &,
      const common::ObIArray<uint64_t> *table_ids = nullptr);
  // Logical history selection is bounded by schema_version, independently of
  // the transaction's protected SCN. Deleted versions remain visible here.
  int get_table_version_history(uint64_t table_id, int64_t schema_version,
      share::schema::VersionHisVal &);
  int get_table_latest_schema_versions(const common::ObIArray<uint64_t> &table_ids,
      common::ObIArray<share::schema::ObTableLatestSchemaVersion> &);
  int get_original_table_version(uint64_t table_id, int64_t &schema_version);
  // Complete stored rows, in fixed schema order (primary key first). Writes
  // join attach()'s owner; any failure requires that owner to roll back.
  // Physical secondary indexes are maintained in the same transaction.
  int insert(uint64_t inner_table_id, const common::ObNewRow &);
  int update(uint64_t inner_table_id, const common::ObNewRow &);
  int erase(uint64_t inner_table_id, const common::ObRowkey &, bool &existed);
  int insert_table(const share::schema::ObTableSchema &, bool history,
                   bool update_object_status_ignore_version = false);
  int insert_column(const share::schema::ObColumnSchemaV2 &, bool history);
  int insert_columns(const share::schema::ObTableSchema &);
  int close();
  const ns::NamespaceCatalogViews::Handle &view() const;
private:
  struct State;
  struct TableState;
  int prepare_write(uint64_t inner_table_id, TableState *&);
  enum class Mutation { INSERT, UPDATE, ERASE };
  int mutate(uint64_t inner_table_id, const common::ObNewRow &, Mutation, bool &existed);
  int prepare_scan(uint64_t inner_table_id, Cursor &);
  int start_scan(const common::ObNewRange &, Cursor &, bool reverse);
  std::shared_ptr<State> state_;
  DISALLOW_COPY_AND_ASSIGN(NativeSchemaCatalog);
};
} // namespace observer
} // namespace oceanbase
#endif
