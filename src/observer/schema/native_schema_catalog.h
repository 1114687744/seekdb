/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifndef OCEANBASE_OBSERVER_NATIVE_SCHEMA_CATALOG_H_
#define OCEANBASE_OBSERVER_NATIVE_SCHEMA_CATALOG_H_

#include <functional>
#include <memory>
#include "common/ob_range.h"
#include "common/row/ob_row.h"
#include "namespace/catalog.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_schema_service.h"

namespace oceanbase {
namespace common { class ObISQLClient; }
namespace share { namespace schema {
class ObTableSchema;
class ObColumnSchemaV2;
class ObSimpleTableSchemaV2;
class ObTableLatestSchemaVersion;
class ObSchemaIdVersion;
class ObIndexSchemaInfo;
class SchemaCatalogRow;
class ObConstraint;
class ObTriggerInfo;
class ObDependencyInfo;
class ObSimpleTriggerSchema;
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
  // Legacy client adapter for a single consistent read, or a DDL owner's
  // own writes. An explicit old SCN must already have a retained directory view.
  static int with_read_client(common::ObISQLClient &, int64_t deadline, int64_t snapshot,
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
  int get_aux_tables(uint64_t table_id, int64_t schema_version,
      common::ObIArray<share::schema::ObAuxTableMetaInfo> &, TableSource = TableSource::HISTORY);
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
  // Operation versions are logical bounds within this operation's SCN. The
  // completed-version lookup requires an END_SIGN and uses idx_ddl_type.
  int get_schema_operations(int64_t lower_exclusive, int64_t upper_inclusive,
      share::schema::ObSchemaService::SchemaOperationSetWithAlloc &);
  int get_latest_schema_version(int64_t &);
  int get_completed_schema_version(int64_t upper_inclusive, int64_t &);
  int has_table_schema_changes(const common::ObIArray<uint64_t> &table_ids,
      int64_t lower_exclusive, int64_t upper_inclusive, bool &);
  int insert_schema_operation(const share::schema::ObSchemaOperation &);
  // Current metadata only. Versions append once per existing requested ID;
  // index information resets the output and owns names in the caller's arena.
  int get_table_schema_versions(const common::ObIArray<uint64_t> &table_ids,
      common::ObIArray<share::schema::ObSchemaIdVersion> &);
  int get_table_index_infos(common::ObIAllocator &, uint64_t database_id,
      uint64_t data_table_id, common::ObIArray<share::schema::ObIndexSchemaInfo> &);
  // Name policy is supplied by the Namespace's caller. A missing name is
  // success with OB_INVALID_ID, matching the DDL metadata lookup contract.
  int get_table_id(uint64_t database_id, uint64_t session_id, const common::ObString &,
      bool case_insensitive, uint64_t &table_id, share::schema::ObTableType &, int64_t &schema_version);
  int get_index_id(uint64_t database_id, const common::ObString &, uint64_t &);
  int get_constraint_id(uint64_t database_id, const common::ObString &, uint64_t &);
  int get_foreign_key_id(uint64_t database_id, const common::ObString &, uint64_t &);
  int get_mock_fk_parent_table_id(uint64_t database_id, const common::ObString &, uint64_t &);
  int get_mock_fk_parent_table_schema_versions(const common::ObIArray<uint64_t> &,
      common::ObIArray<share::schema::ObSchemaIdVersion> &);
  // Dependency rows have no history table; this operation's MVCC view selects
  // them. INVALID reference type means all types for that reference ID.
  int get_references(uint64_t dependent_id, common::ObIArray<share::schema::ObDependencyInfo> &);
  int get_dependents(uint64_t reference_id, share::schema::ObObjectType reference_type,
      common::ObIArray<share::schema::ObDependencyInfo> &);
  int write_dependency(const share::schema::ObDependencyInfo &, bool upsert);
  int erase_dependencies(uint64_t dependent_id, share::schema::ObObjectType);
  int get_triggers(int64_t schema_version, common::ObIArray<share::schema::ObTriggerInfo> &,
                   const common::ObIArray<uint64_t> *ids = nullptr);
  int get_simple_triggers(int64_t schema_version, common::ObIArray<share::schema::ObSimpleTriggerSchema> &,
                          const common::ObIArray<uint64_t> *ids = nullptr);
  enum class TriggerWrite { INSERT, UPDATE, ERASE };
  int write_trigger(const share::schema::ObTriggerInfo &, int64_t version, TriggerWrite);
  int get_simple_mock_fk_parent_tables(int64_t schema_version,
      common::ObIArray<share::schema::ObSimpleMockFKParentTableSchema> &,
      const common::ObIArray<uint64_t> *table_ids = nullptr);
  int get_mock_fk_parent_tables(int64_t schema_version,
      common::ObIArray<share::schema::ObMockFKParentTableSchema> &,
      const common::ObIArray<uint64_t> *table_ids = nullptr);
  // Complete stored rows, in fixed schema order (primary key first). Writes
  // join attach()'s owner; any failure requires that owner to roll back.
  // Physical secondary indexes are maintained in the same transaction.
  int insert(uint64_t inner_table_id, const common::ObNewRow &);
  int update(uint64_t inner_table_id, const common::ObNewRow &);
  // Point UPDATE: require the full key, retain omitted fields, and report a
  // missing row as success with matched=false. Index changes share this owner.
  using FieldWriter = std::function<int(share::schema::SchemaCatalogRow &)>;
  int insert_fields(uint64_t inner_table_id, const FieldWriter &);
  // The DDL owner serializes the object; missing-row insertion still uses
  // the storage engine's uniqueness check.
  int upsert_fields(uint64_t inner_table_id, const FieldWriter &);
  int update_fields(uint64_t inner_table_id, const FieldWriter &, bool &matched);
  // Delete the nonempty key prefix visible to this operation, including own
  // writes. The DDL owner supplies object-level serialization. Temporary keys
  // live only for this call; row locks and index writes belong to the owner.
  int erase_prefix(uint64_t inner_table_id, const FieldWriter &, int64_t &affected_rows);
  int update_table(const share::schema::ObTableSchema &, bool update_object_status_ignore_version = false);
  int update_column(const share::schema::ObColumnSchemaV2 &);
  int erase(uint64_t inner_table_id, const common::ObRowkey &, bool &existed);
  int insert_table(const share::schema::ObTableSchema &, bool history,
                   bool update_object_status_ignore_version = false);
  int insert_column(const share::schema::ObColumnSchemaV2 &, bool history);
  int insert_columns(const share::schema::ObTableSchema &);
  int insert_constraint(const share::schema::ObConstraint &, bool only_history, bool with_columns);
  int insert_constraint_columns(const share::schema::ObConstraint &, bool only_history);
  int insert_constraints(const share::schema::ObTableSchema &);
  int update_constraint(const share::schema::ObConstraint &);
  int rename_constraint(const share::schema::ObConstraint &, const common::ObString &, int64_t schema_version);
  int erase_constraint(const share::schema::ObConstraint &, int64_t schema_version, int64_t &deleted_columns);
  enum class ForeignKeyWrite { INSERT, UPSERT, HISTORY_DELETE };
  int write_foreign_key(const share::schema::ObForeignKeyInfo &, int64_t version, ForeignKeyWrite);
  int insert_foreign_key_columns(const share::schema::ObForeignKeyInfo &, int64_t version, bool only_history);
  int erase_foreign_key(const share::schema::ObForeignKeyInfo &, int64_t version, bool require_current);
  int erase_foreign_key_columns(const share::schema::ObForeignKeyInfo &, int64_t version);
  int insert_mock_parent(const share::schema::ObMockFKParentTableSchema &, bool only_history);
  int insert_mock_parent_columns(const share::schema::ObMockFKParentTableSchema &);
  int erase_mock_parent(const share::schema::ObMockFKParentTableSchema &);
  int erase_mock_parent_columns(const share::schema::ObMockFKParentTableSchema &);
  int update_mock_parent_version(const share::schema::ObMockFKParentTableSchema &);
  int close();
  const ns::NamespaceCatalogViews::Handle &view() const;
private:
  struct State;
  struct TableState;
  int prepare_write(uint64_t inner_table_id, TableState *&);
  enum class Mutation { INSERT, UPDATE, ERASE };
  int mutate(uint64_t inner_table_id, const common::ObNewRow &, Mutation, bool &existed,
             const common::ObIArray<uint64_t> *column_ids = nullptr);
  int prepare_scan(uint64_t inner_table_id, Cursor &);
  int start_scan(const common::ObNewRange &, Cursor &, bool reverse);
  std::shared_ptr<State> state_;
  DISALLOW_COPY_AND_ASSIGN(NativeSchemaCatalog);
};
} // namespace observer
} // namespace oceanbase
#endif
