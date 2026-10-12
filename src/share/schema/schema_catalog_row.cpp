/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "share/schema/schema_catalog_row.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/object/ob_obj_cast.h"

namespace oceanbase {
namespace share {
namespace schema {
using namespace common;

int SchemaCatalogRow::init(const ObTableSchema &schema, int64_t statement_time)
{
  if (schema_ != nullptr) { return OB_INIT_TWICE; }
  if (statement_time <= 0) { return OB_INVALID_ARGUMENT; }
  int ret = schema.get_column_ids(columns_);
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    if (OB_FAIL(cells_.push_back(ObObj()))) {
    } else { ret = provided_.push_back(false); }
  }
  if (OB_SUCC(ret)) { schema_ = &schema; statement_time_ = statement_time; }
  return ret;
}

int SchemaCatalogRow::add_column(const char *name, const ObString &value)
{
  ObObj cell;
  if (value.ptr() == nullptr) { cell.set_null(); }
  else { cell.set_varchar(value); }
  return set(name, cell);
}

int SchemaCatalogRow::add_column(const char *name, const char *value)
{ return add_column(name, ObString::make_string(value)); }
int SchemaCatalogRow::add_column(const char *name, const ObHexEscapeSqlStr &value)
{ return add_column(name, value.str()); }
int SchemaCatalogRow::add_column(const char *name, const share::ObRealUInt64 &value)
{ ObObj cell; cell.set_uint64(value.value()); return set(name, cell); }

int SchemaCatalogRow::add_time(const char *name, int64_t value)
{
  ObObj cell;
  cell.set_timestamp(value < 0 ? statement_time_ : value);
  return set(name, cell);
}

int SchemaCatalogRow::set(const char *name, const ObObj &value)
{
  if (schema_ == nullptr) { return OB_NOT_INIT; }
  if (name == nullptr) { return OB_INVALID_ARGUMENT; }
  const auto *column = schema_->get_column_schema(ObString::make_string(name));
  if (column == nullptr) { return OB_ERR_COLUMN_NOT_FOUND; }
  for (int64_t i = 0; i < columns_.count(); ++i) {
    if (columns_.at(i).col_id_ == column->get_column_id()) { return set(i, value); }
  }
  return OB_ERR_UNEXPECTED;
}

int SchemaCatalogRow::set(int64_t position, const ObObj &value)
{
  const auto *column = schema_->get_column_schema(columns_.at(position).col_id_);
  if (provided_.at(position)) { return OB_INVALID_ARGUMENT; }
  ObObj stored = value;
  int ret = OB_SUCCESS;
  if (value.is_null()) {
    if (!column->is_nullable()) { ret = OB_ERR_NULL_VALUE; }
  } else if (value.is_string_type() && ob_is_string_type(column->get_data_type())) {
    stored.set_meta_type(column->get_meta_type());
    stored.set_string(column->get_data_type(), value.get_string());
    if (stored.is_lob_storage()) { stored.set_inrow(); }
  } else if (value.is_integer_type() && ob_is_integer_type(column->get_data_type())) {
    ObCastCtx ctx(&allocator_, nullptr, CM_NONE, column->get_collation_type());
    ret = ObObjCaster::to_type(column->get_data_type(), ctx, value, stored);
  } else if (value.get_type() != column->get_data_type()) {
    ret = OB_OBJ_TYPE_ERROR;
  }
  if (OB_SUCC(ret)) { ret = ob_write_obj(allocator_, stored, cells_.at(position)); }
  if (OB_SUCC(ret)) { provided_.at(position) = true; }
  return ret;
}

int SchemaCatalogRow::finish(ObNewRow &row)
{
  row.reset();
  if (schema_ == nullptr) { return OB_NOT_INIT; }
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    if (provided_.at(i)) { continue; }
    const auto *column = schema_->get_column_schema(columns_.at(i).col_id_);
    ObObj value = column->get_cur_default_value();
    if (column->is_rowkey_column()) { ret = OB_INVALID_ARGUMENT; }
    else if (value.is_ext()) {
      if (value.get_ext() != ObActionFlag::OP_DEFAULT_NOW_FLAG || column->get_data_type() != ObTimestampType) {
        ret = OB_NOT_SUPPORTED;
      } else { value.set_timestamp(statement_time_); }
    }
    if (OB_SUCC(ret)) { ret = set(i, value); }
  }
  if (OB_SUCC(ret)) { row.assign(cells_.get_data(), cells_.count()); }
  return ret;
}

int SchemaCatalogRow::finish_update(ObNewRow &row, ObIArray<uint64_t> &column_ids)
{
  row.reset();
  column_ids.reset();
  if (schema_ == nullptr) { return OB_NOT_INIT; }
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    if (i < schema_->get_rowkey_column_num()) {
      if (!provided_.at(i)) { ret = OB_INVALID_ARGUMENT; }
    } else if (provided_.at(i)) { ret = column_ids.push_back(columns_.at(i).col_id_); }
  }
  if (OB_SUCC(ret)) { row.assign(cells_.get_data(), cells_.count()); }
  return ret;
}

int SchemaCatalogRow::finish_key_prefix(ObRowkey &key)
{
  key.reset();
  if (schema_ == nullptr) { return OB_NOT_INIT; }
  int64_t count = 0;
  while (count < schema_->get_rowkey_column_num() && provided_.at(count)) { ++count; }
  if (count == 0) { return OB_INVALID_ARGUMENT; }
  for (int64_t i = count; i < provided_.count(); ++i) {
    if (provided_.at(i)) { return OB_INVALID_ARGUMENT; }
  }
  key.assign(cells_.get_data(), count);
  return OB_SUCCESS;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
