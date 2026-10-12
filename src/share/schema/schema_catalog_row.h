/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifndef OCEANBASE_SHARE_SCHEMA_CATALOG_ROW_H_
#define OCEANBASE_SHARE_SCHEMA_CATALOG_ROW_H_

#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_col_desc.h"
#include "common/row/ob_row.h"

namespace oceanbase {
namespace common { class ObHexEscapeSqlStr; }
namespace share {
class ObRealUInt64;
namespace schema {

// Typed sink for schema metadata fields. The fixed definition supplies column
// order, types and constant defaults. The owner supplies its statement time;
// no SQL expressions or Namespace services are evaluated here.
class SchemaCatalogRow final
{
public:
  SchemaCatalogRow() = default;
  int init(const ObTableSchema &fixed_schema, int64_t statement_time);
  template<class T> int add_column(const char *name, const T &value)
  { common::ObObj cell; cell.set_int(static_cast<int64_t>(value)); return set(name, cell); }
  int add_column(const char *, const common::ObString &);
  int add_column(const char *, const char *);
  int add_column(const char *, const common::ObHexEscapeSqlStr &);
  int add_column(const char *, const share::ObRealUInt64 &);
  int add_column(const char *name, const common::ObObj &value) { return set(name, value); }
  template<class T> int add_pk_column(const char *name, const T &value)
  {
    const auto *column = schema_ == nullptr ? nullptr : schema_->get_column_schema(common::ObString::make_string(name));
    return column == nullptr || !column->is_rowkey_column() ? common::OB_INVALID_ARGUMENT : add_column(name, value);
  }
  int add_gmt_create(int64_t value = -1) { return add_time("gmt_create", value); }
  int add_gmt_modified(int64_t value = -1) { return add_time("gmt_modified", value); }
  // The completed row, including all bytes, is owned until this sink dies.
  int finish(common::ObNewRow &);
  // Require the complete primary key, but do not fill omitted values. Only
  // returned non-key column IDs participate in the update. Bytes remain owned.
  int finish_update(common::ObNewRow &, common::ObIArray<uint64_t> &column_ids);
  // A nonempty, contiguous primary-key prefix. Reject other provided fields;
  // omitting a key component must never silently broaden a deletion.
  int finish_key_prefix(common::ObRowkey &);
private:
  int set(const char *, const common::ObObj &);
  int set(int64_t position, const common::ObObj &);
  int add_time(const char *, int64_t);
  const ObTableSchema *schema_ = nullptr;
  int64_t statement_time_ = 0;
  common::ObArenaAllocator allocator_{common::ObMemAttr("CatalogRow")};
  common::ObSEArray<ObColDesc, 16> columns_;
  common::ObSEArray<common::ObObj, 16> cells_;
  common::ObSEArray<bool, 16> provided_;
  DISALLOW_COPY_AND_ASSIGN(SchemaCatalogRow);
};

} // namespace schema
} // namespace share
} // namespace oceanbase
#endif
