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

#define USING_LOG_PREFIX SHARE_SCHEMA
#include "ob_partition_sql_helper.h"

#include <stdio.h>
#include <string.h>

#include "share/ob_timezone_mgr.h"
#include "lib/net/ob_addr.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_level.h"
#include "lib/oblog/ob_log_print_kv.h"
#include "lib/utility/ob_smart_var.h"
#include "lib/utility/utility.h"
#include "mysqlclient/ob_isql_client.h"
#include "object/ob_object.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "observer/schema/sql_schema_catalog.h"
#include "share/schema/schema_catalog_row.h"
#include "share/schema/ob_schema_utils.h"
#include "timezone/ob_timezone_info.h"

namespace oceanbase {
namespace common {
class ObNewRow;
class ObRowkey;
template <class T> class ObIArray;
}  // namespace common
}  // namespace oceanbase

namespace oceanbase
{
using namespace obcall;
namespace share
{
namespace schema
{
using namespace common;

int ObPartDMLGenerator::gen_dml(SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  PartInfo part_info;
  if (OB_FAIL(extract_part_info(part_info))) {
  } else if (OB_FAIL(convert_to_dml(part_info, dml))) {
  }
  return ret;
}

int ObPartDMLGenerator::gen_high_bound_val_str(
    const ObRowkey &high_bound_val,
    ObString &high_bound_val_str,
    ObString &b_high_bound_val_str)
{
  int ret = OB_SUCCESS;

  int64_t pos = 0;
  //TODO:@yanhua add session timezone_info is better
  ObTimeZoneInfo tz_info;
  tz_info.set_offset(0);
  if (OB_FAIL(OTTZ_MGR.get_timezone_map(tz_info.get_tz_map_wrap()))) {
  } else if (OB_FAIL(ObPartitionUtils::convert_rowkey_to_sql_literal(
             high_bound_val, high_bound_val_,
             OB_MAX_B_HIGH_BOUND_VAL_LENGTH, pos, false, &tz_info))) {
  } else {
    high_bound_val_str.assign_ptr(high_bound_val_, static_cast<int32_t>(pos));
  }
  if (OB_SUCC(ret)) {
    pos = 0;
    if (OB_FAIL(ObPartitionUtils::convert_rowkey_to_hex(
        high_bound_val, b_high_bound_val_,
        OB_MAX_B_HIGH_BOUND_VAL_LENGTH, pos))) {
    } else {
      b_high_bound_val_str.assign_ptr(b_high_bound_val_, static_cast<int32_t>(pos));
    }
  }

  return ret;
}
int ObPartDMLGenerator::gen_list_val_str(
    const common::ObIArray<common::ObNewRow>& list_value,
    common::ObString &list_val_str,
    common::ObString &b_list_val_str)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  //TODO:@yanhua add session timezone_info is better
  ObTimeZoneInfo tz_info;
  tz_info.set_offset(0);
  if (OB_FAIL(OTTZ_MGR.get_timezone_map(tz_info.get_tz_map_wrap()))) {
  } else if (OB_FAIL(ObPartitionUtils::convert_rows_to_sql_literal(
             list_value, list_val_,
             OB_MAX_B_PARTITION_EXPR_LENGTH, pos, false, &tz_info))) {
  } else {
    list_val_str.assign_ptr(list_val_, static_cast<int32_t>(pos));
  }
  pos = 0;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObPartitionUtils::convert_rows_to_hex(list_value, b_list_val_,
                                                           OB_MAX_B_PARTITION_EXPR_LENGTH, pos))) {
  } else {
    b_list_val_str.assign_ptr(b_list_val_, static_cast<int32_t>(pos));
  }
  return ret;
}

int ObPartSqlHelper::init(const ObPartitionSchema *table)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!tables_.empty())) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", KR(ret), K(tables_));
  } else if (OB_FAIL(tables_.push_back(table))) {
  }
  return ret;
}

int ObPartSqlHelper::init(ObIArray<const ObPartitionSchema *> &tables)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!tables_.empty())) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", KR(ret), K(tables_));
  } else if (OB_FAIL(tables_.assign(tables))) {
  }
  return ret;
}

namespace {
using observer::NativeSchemaCatalog;

int write_partition_rows(NativeSchemaCatalog &catalog, uint64_t current, uint64_t history,
    const NativeSchemaCatalog::FieldWriter &fill, bool only_history, bool deleted)
{
  int ret = only_history ? OB_SUCCESS : catalog.insert_fields(current, fill);
  if (OB_SUCC(ret)) {
    ret = catalog.insert_fields(history, [&](SchemaCatalogRow &fields) {
      int ret = fill(fields);
      if (OB_SUCC(ret)) { ret = fields.add_column("is_deleted", deleted); }
      return ret;
    });
  }
  return ret;
}
} // namespace

int ObPartSqlHelper::iterate_all_part_(NativeSchemaCatalog &catalog, const bool only_history, const ObPartitionSchema *table,
                                       const bool include_hidden)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (table->is_user_partition_table()) {
    
    
    const ObPartitionOption &part_expr = table->get_part_option();
    int64_t part_num = part_expr.get_part_num();
    ObPartition **part_array = table->get_part_array();
    ObPartition **hidden_part_array = table->get_hidden_part_array();
    int64_t hidden_part_num = include_hidden ? table->get_hidden_partition_num() : 0;
    int64_t total_part_num = part_num + hidden_part_num; 
    if (OB_ISNULL(part_array)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("part array is null", K(ret), KP(part_array));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < total_part_num; i++) {
      ObPartition *part = NULL;
      if (i < part_num) {
        part = part_array[i];
      } else if (OB_ISNULL(hidden_part_array)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("hidden part array is null", K(ret), KP(hidden_part_array)); 
      } else {
        part = hidden_part_array[i - part_num];
      }
      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(part)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("part is null", K(ret), K(i), K(part_num), K(hidden_part_num));
      }
      if (OB_SUCC(ret)) {
        ret = write_partition_rows(catalog, OB_ALL_PART_TID, OB_ALL_PART_HISTORY_TID,
            [&](SchemaCatalogRow &fields) { return add_part_dml_column(table, *part, fields); }, only_history, is_deleted());
      }
    }
  }
  return ret;
}

int ObPartSqlHelper::iterate_all_sub_part_(NativeSchemaCatalog &catalog, const bool only_history,
    const ObPartitionSchema *table, const bool include_hidden)
{
  int ret = OB_SUCCESS;
  UNUSED(include_hidden);
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (table->is_user_subpartition_table()) {
    
    
    int64_t part_num = table->get_part_option().get_part_num();
    ObPartition **part_array = table->get_part_array();
    ObSubPartition **subpart_array = NULL;
    ObSubPartition *subpart = NULL;
    int64_t sub_part_num = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < part_num; i++) {
      int64_t part_id = -1;
      if (OB_ISNULL(part_array) || OB_ISNULL(part_array[i])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("subpart_array is invalid", KR(ret));
      } else {
        subpart_array = part_array[i]->get_subpart_array();
        sub_part_num = part_array[i]->get_subpartition_num();
        part_id = part_array[i]->get_part_id();
        if (part_id < 0 || sub_part_num < 0) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("part_id or sub_part_num is invalid", K(ret), K(part_id), K(sub_part_num));
        }
      }
      for (int64_t j = 0; OB_SUCC(ret) && j < sub_part_num; j++) {
          int64_t sub_part_id = -1;
        if (OB_ISNULL(subpart_array) || OB_ISNULL(subpart_array[j])) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("subpart_array is invalid", KR(ret));
        } else {
          subpart = subpart_array[j];
          sub_part_id = subpart->get_sub_part_id();
          if (sub_part_id < 0) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("sub_part_id or is invalid", K(ret), K(sub_part_id));
          } else {
            ret = write_partition_rows(catalog, OB_ALL_SUB_PART_TID, OB_ALL_SUB_PART_HISTORY_TID,
                [&](SchemaCatalogRow &fields) { return add_subpart_dml_column(table, part_id, sub_part_id, *subpart, fields); },
                only_history, is_deleted());
          }
        }
      }
    }
  }
  return ret;
}

int ObPartSqlHelper::iterate_all_def_sub_part_(NativeSchemaCatalog &catalog, const bool only_history,
    const ObPartitionSchema *table, const bool include_hidden)
{
  int ret = OB_SUCCESS;
  UNUSED(include_hidden);
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (table->is_user_subpartition_table()
             && table->has_sub_part_template_def()) {
    
    
    const int64_t def_sub_part_num = table->get_sub_part_option().get_part_num();
    ObSubPartition **def_subpart_array = table->get_def_subpart_array();
    for (int64_t j = 0; OB_SUCC(ret) && j < def_sub_part_num; j++) {
      if (OB_ISNULL(def_subpart_array) || OB_ISNULL(def_subpart_array[j])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("def_subpart is null", KR(ret), KP(def_subpart_array), K(j));
      } else {
        ret = write_partition_rows(catalog, OB_ALL_DEF_SUB_PART_TID, OB_ALL_DEF_SUB_PART_HISTORY_TID,
            [&](SchemaCatalogRow &fields) { return add_def_subpart_dml_column(table, j, *def_subpart_array[j], fields); },
            only_history, is_deleted());
      }
    }
  }
  return ret;
}

int ObPartSqlHelper::iterate_part_info_(NativeSchemaCatalog &catalog, const bool only_history,
    const ObPartitionSchema *table, const bool include_hidden)
{
  int ret = OB_SUCCESS;
  UNUSED(include_hidden);
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (table->is_user_partition_table()) {
    
    
    ret = write_partition_rows(catalog, OB_ALL_PART_INFO_TID, OB_ALL_PART_INFO_HISTORY_TID,
        [&](SchemaCatalogRow &fields) { return add_part_info_dml_column(table, fields); }, only_history, is_deleted());
  }
  return ret;
}

int ObPartSqlHelper::write_all(bool only_history)
{
  bool has_partitions = false;
  for (int64_t i = 0; i < tables_.count(); ++i) {
    if (tables_.at(i) == nullptr) { return OB_ERR_UNEXPECTED; }
    has_partitions |= tables_.at(i)->is_user_partition_table();
  }
  if (!has_partitions) { return OB_SUCCESS; }
  return observer::with_ddl_schema_catalog(sql_client_, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
    int ret = OB_SUCCESS;
    for (int64_t i = 0; OB_SUCC(ret) && i < tables_.count(); ++i) {
      const auto *table = tables_.at(i);
      if (OB_FAIL(iterate_part_info_(catalog, only_history, table, true))) {
      } else if (OB_FAIL(iterate_all_part_(catalog, only_history, table, true))) {
      } else if (OB_FAIL(iterate_all_sub_part_(catalog, only_history, table, true))) {
      } else { ret = iterate_all_def_sub_part_(catalog, only_history, table, true); }
    }
    return ret;
  });
}

int ObAddPartInfoHelper::add_partition_info()
{ return write_all(false); }

int ObAddPartInfoHelper::add_part_info_dml_column(const ObPartitionSchema *table,
                                                  SchemaCatalogRow &dml)
{
   int ret = OB_SUCCESS;
   if (OB_ISNULL(table)) {
     ret = OB_ERR_UNEXPECTED;
     LOG_WARN("table is null", K(ret));
   } else {
     const ObPartitionOption &part_option = table->get_part_option();
     const ObSubPartitionOption &subpart_option = table->get_sub_part_option();
     if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                                table->get_table_id())))
       || OB_FAIL(dml.add_column("part_type", part_option.get_part_func_type()))
       || OB_FAIL(dml.add_column(OBJ_GET_K(part_option, part_num)))
       || OB_FAIL(dml.add_column("part_space", 0))
       || OB_FAIL(dml.add_column("part_expr", part_option.get_part_func_expr_str()))
       || OB_FAIL(dml.add_column("sub_part_type", subpart_option.get_part_func_type()))
       || OB_FAIL(dml.add_column("def_sub_part_num", subpart_option.get_part_num()))
       || OB_FAIL(dml.add_column("schema_version", table->get_schema_version()))
       || OB_FAIL(dml.add_column("sub_part_expr", subpart_option.get_part_func_expr()))) {
       LOG_WARN("add column failed", K(ret));
     }
   }
   return ret;
}

int ObAddPartInfoHelper::add_part_dml_column(const ObPartitionSchema *table,
                                             const ObPartition &part,
                                             SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (part.get_part_idx() < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("part_idx is invalid", KR(ret), K(part));
  } else {
    int64_t sub_part_num = 0;
    if (PARTITION_LEVEL_TWO == table->get_part_level()) {
      sub_part_num = part.get_sub_part_num();
    }
    PartitionType partition_type = part.get_partition_type();
    if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                                 table->get_table_id())))
        || OB_FAIL(dml.add_pk_column("part_id", part.get_part_id()))
        || OB_FAIL(dml.add_column("part_idx", part.get_part_idx()))
        || OB_FAIL(dml.add_column("schema_version", table->get_schema_version()))
        || OB_FAIL(dml.add_column("sub_part_num", sub_part_num))
        || OB_FAIL(dml.add_column("sub_part_space", 0 /*unused now*/))
        || OB_FAIL(dml.add_column("new_sub_part_space", 0 /*unused now*/))
        || OB_FAIL(dml.add_column("status", PARTITION_STATUS_INVALID))
        || OB_FAIL(dml.add_column("spare1", 0 /*unused now*/))
        || OB_FAIL(dml.add_column("spare2", 0 /*unused now*/))
        || OB_FAIL(dml.add_column("spare3", "" /*unused now*/))
        || OB_FAIL(dml.add_column("comment", "" /*unused now*/))
        || OB_FAIL(dml.add_column("tablespace_id", ObSchemaUtils::get_extract_schema_id(
                                                   part.get_tablespace_id())))
        || OB_FAIL(dml.add_column("partition_type", partition_type))
        || OB_FAIL(dml.add_column("tablet_id", part.get_tablet_id().id()))
        || OB_FAIL(dml.add_column("part_name", ObHexEscapeSqlStr(part.get_part_name())))) {
      LOG_WARN("dml add part info failed", K(ret));
    } else if (OB_FAIL(add_part_high_bound_val_column(table, part, dml))) {
    } else if (OB_FAIL(add_part_list_val_column(table, part, dml))) {
    }
  }
  return ret;
}

// For some paths, hash-like subpartition info will only store in sub_part_option from resolver,
// which means subpart from def_sub_part_array/sub_part_array may be NULL.
int ObAddPartInfoHelper::add_subpart_dml_column(const ObPartitionSchema *table,
                                                const int64_t part_id,
                                                const int64_t subpart_id,
                                                const ObSubPartition &subpart,
                                                SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (subpart.get_sub_part_idx() < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("subpart_idx is invalid", KR(ret), K(subpart));
  } else {
    PartitionType partition_type = subpart.get_partition_type();
    if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                                 table->get_table_id())))
        || OB_FAIL(dml.add_pk_column("part_id", part_id))
        || OB_FAIL(dml.add_pk_column("sub_part_id", subpart_id))
        || OB_FAIL(dml.add_column("schema_version", table->get_schema_version()))
        || OB_FAIL(dml.add_column("status", PARTITION_STATUS_INVALID))
        || OB_FAIL(dml.add_column("spare1", 0 /*unused now*/))
        || OB_FAIL(dml.add_column("spare2", 0 /*unused now*/))
        || OB_FAIL(dml.add_column("spare3", "" /*unused now*/))
        || OB_FAIL(dml.add_column("comment", "" /*unused now*/))
        || OB_FAIL(dml.add_column("sub_part_idx", subpart.get_sub_part_idx()))
        || OB_FAIL(dml.add_column("source_partition_id", "-1"))
        || OB_FAIL(dml.add_column("tablespace_id", ObSchemaUtils::get_extract_schema_id(
                                                   subpart.get_tablespace_id())))
        || OB_FAIL(dml.add_column("tablet_id", subpart.get_tablet_id().id()))
        || OB_FAIL(dml.add_column("sub_part_name", ObHexEscapeSqlStr(subpart.get_part_name())))) {
        LOG_WARN("dml add part info failed", K(ret));
    } else if (OB_FAIL(add_subpart_high_bound_val_column(table, subpart, dml))) {
    } else if (OB_FAIL(add_subpart_list_val_column(table, subpart, dml))) {
    }
  }
  return ret;
}

// For some paths, hash-like subpartition info will only store in sub_part_option from resolver,
// which means subpart from def_sub_part_array/sub_part_array may be NULL.
int ObAddPartInfoHelper::add_def_subpart_dml_column(const ObPartitionSchema *table,
                                                    const int64_t def_subpart_idx,
                                                    const ObSubPartition &subpart,
                                                    SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else {
    // For def subpartition, sub_part_idx and sub_part_id should be equal
    if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                                 table->get_table_id())))
        || OB_FAIL(dml.add_pk_column("sub_part_id", def_subpart_idx))
        || OB_FAIL(dml.add_column("schema_version", table->get_schema_version()))
        || OB_FAIL(dml.add_column("spare1", 0 /*unused now*/))
        || OB_FAIL(dml.add_column("spare2", 0 /*unused now*/))
        || OB_FAIL(dml.add_column("spare3", "" /*unused now*/))
        || OB_FAIL(dml.add_column("comment", "" /*unused now*/))
        || OB_FAIL(dml.add_column("sub_part_idx", def_subpart_idx))
        || OB_FAIL(dml.add_column("tablespace_id", ObSchemaUtils::get_extract_schema_id(
                                                   subpart.get_tablespace_id())))
        || OB_FAIL(dml.add_column("sub_part_name", ObHexEscapeSqlStr(subpart.get_part_name())))) {
        LOG_WARN("dml add part info failed", K(ret));
    } else if (OB_FAIL(add_subpart_high_bound_val_column(table, subpart, dml))) {
    } else if (OB_FAIL(add_subpart_list_val_column(table, subpart, dml))) {
    }
  }

  return ret;
}

int ObAddPartInfoHelper::add_part_high_bound_val_column(const ObPartitionSchema *table,
                                                        const ObBasePartition &part,
                                                        SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (table->is_range_part()) {
    if (OB_FAIL(add_high_bound_val_column(table, part, dml))) {
    }
  } else if (OB_FAIL(dml.add_null_column("high_bound_val"))) {
  } else if (OB_FAIL(dml.add_null_column("b_high_bound_val"))) {
  }
  return ret;
}

int ObAddPartInfoHelper::add_part_list_val_column(const ObPartitionSchema *table,
                                                        const ObBasePartition &part,
                                                        SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (table->is_list_part()) {
    if (OB_FAIL(add_list_val_column(table, part, dml))) {
    }
  } else if (OB_FAIL(dml.add_null_column("list_val"))) {
  } else if (OB_FAIL(dml.add_null_column("b_list_val"))) {
  }
  return ret;
}

int ObAddPartInfoHelper::add_subpart_high_bound_val_column(const ObPartitionSchema *table,
                                                        const ObBasePartition &part,
                                                        SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (table->is_range_subpart()) {
    if (OB_FAIL(add_high_bound_val_column(table, part, dml))) {
    }
  } else if (OB_FAIL(dml.add_null_column("high_bound_val"))) {
  } else if (OB_FAIL(dml.add_null_column("b_high_bound_val"))) {
  }
  return ret;
}

int ObAddPartInfoHelper::add_subpart_list_val_column(const ObPartitionSchema *table,
                                                        const ObBasePartition &part,
                                                        SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (table->is_list_subpart()) {
    if (OB_FAIL(add_list_val_column(table, part, dml))) {
    }
  } else if (OB_FAIL(dml.add_null_column("list_val"))) {
  } else if (OB_FAIL(dml.add_null_column("b_list_val"))) {
  }
  return ret;
}

template<typename P>
int ObAddPartInfoHelper::add_high_bound_val_column(
    const ObPartitionSchema *table,
    const P &part_option,
    SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (high_bound_val_ == NULL) {
    high_bound_val_ = static_cast<char *>(allocator_.alloc(OB_MAX_B_HIGH_BOUND_VAL_LENGTH));
    if (OB_ISNULL(high_bound_val_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("high_bound_val is null", K(ret), K(high_bound_val_));
    }
  }
  // determine if it is a list partition
  if (OB_SUCC(ret)) {
    MEMSET(high_bound_val_, 0, OB_MAX_B_HIGH_BOUND_VAL_LENGTH);
    int64_t pos = 0;
    //TODO:@yanhua add session timezone_info is better
    ObTimeZoneInfo tz_info;
    tz_info.set_offset(0);
    if (OB_ISNULL(table)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table ptr is null", KR(ret));
    } else if (OB_FAIL(OTTZ_MGR.get_timezone_map(tz_info.get_tz_map_wrap()))) {
    } else if (OB_FAIL(ObPartitionUtils::convert_rowkey_to_sql_literal(
               part_option.get_high_bound_val(), high_bound_val_,
               OB_MAX_B_HIGH_BOUND_VAL_LENGTH, pos, false, &tz_info))) {
    } else if (OB_FAIL(dml.add_column("high_bound_val",
                                      ObHexEscapeSqlStr(ObString(pos, high_bound_val_))))) {
    } else if (FALSE_IT(pos = 0)) {
    } else if (OB_FAIL(ObPartitionUtils::convert_rowkey_to_hex(
        part_option.get_high_bound_val(), high_bound_val_,
        OB_MAX_B_HIGH_BOUND_VAL_LENGTH, pos))) {
    } else if (OB_FAIL(dml.add_column("b_high_bound_val", ObString(pos, high_bound_val_)))) {
    } else {
      LOG_DEBUG("high bound info", "high_bound_val", ObString(pos, high_bound_val_).ptr(), K(pos));
    } //do nothing
  }
  return ret;
}

template<class P>
int ObAddPartInfoHelper::add_list_val_column(
    const ObPartitionSchema *table,
    const P &part_option,
    SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (list_val_ == NULL) {
    list_val_ = static_cast<char *>(allocator_.alloc(OB_MAX_B_PARTITION_EXPR_LENGTH));
    if (OB_ISNULL(list_val_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("list_val is null", K(ret), K(list_val_));
    }
  }
  // determine if it is a list partition, if it is a list partition
  if (OB_SUCC(ret)) {
    MEMSET(list_val_, 0, OB_MAX_B_PARTITION_EXPR_LENGTH);
    int64_t pos = 0;
    //TODO:@yanhua add session timezone_info is better
    ObTimeZoneInfo tz_info;
    tz_info.set_offset(0);
    if (OB_ISNULL(table)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table ptr is null", KR(ret));
    } else if (OB_FAIL(OTTZ_MGR.get_timezone_map(tz_info.get_tz_map_wrap()))) {
    } else if (OB_FAIL(ObPartitionUtils::convert_rows_to_sql_literal(
               part_option.get_list_row_values(), list_val_,
               OB_MAX_B_PARTITION_EXPR_LENGTH, pos, false, &tz_info))) {
    } else if (OB_FAIL(dml.add_column("list_val",
                                      ObHexEscapeSqlStr(ObString(pos, list_val_))))) {
    } else if (FALSE_IT(pos = 0)) {
    } else if (OB_FAIL(ObPartitionUtils::convert_rows_to_hex(
        part_option.get_list_row_values(), list_val_, OB_MAX_B_PARTITION_EXPR_LENGTH, pos))) {
    } else if (OB_FAIL(dml.add_column("b_list_val", ObString(pos, list_val_)))) {
    }
  }
  return ret;
}

int ObDropPartInfoHelper::delete_partition_info()
{ return write_all(true); }

int ObDropPartInfoHelper::add_part_info_dml_column(
    const ObPartitionSchema *table,
    SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                                      table->get_table_id())))
             || OB_FAIL(dml.add_column("schema_version", table->get_schema_version()))) {
    LOG_WARN("dml add part info failed", K(ret));
  }
  return ret;
}

int ObDropPartInfoHelper::add_part_dml_column(const ObPartitionSchema *table,
                                              const ObPartition &part,
                                              SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                                      table->get_table_id())))
             || OB_FAIL(dml.add_pk_column("part_id", part.get_part_id()))
             || OB_FAIL(dml.add_column("schema_version", table->get_schema_version()))) {
    LOG_WARN("dml add part info failed", K(ret));
  }
  return ret;
}

int ObDropPartInfoHelper::add_subpart_dml_column(const ObPartitionSchema *table,
                                                 const int64_t part_id,
                                                 const int64_t subpart_id,
                                                 const ObSubPartition &subpart,
                                                 SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  UNUSED(subpart);
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                                      table->get_table_id())))
             || OB_FAIL(dml.add_pk_column("part_id", part_id))
             || OB_FAIL(dml.add_pk_column("sub_part_id", subpart_id))
             || OB_FAIL(dml.add_column("schema_version", table->get_schema_version()))) {
    LOG_WARN("dml add part info failed", K(ret));
  }
  return ret;
}

int ObDropPartInfoHelper::add_def_subpart_dml_column(const ObPartitionSchema *table,
                                                     const int64_t subpart_idx,
                                                     const ObSubPartition &subpart,
                                                     SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  UNUSED(subpart);
  if (OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                               table->get_table_id())))
      || OB_FAIL(dml.add_pk_column("sub_part_id", subpart_idx))
      || OB_FAIL(dml.add_column("schema_version", table->get_schema_version()))) {
    LOG_WARN("dml add part info failed", K(ret));
  }
  return ret;
}

int ObAddIncSubPartDMLGenerator::convert_to_dml(const PartInfo &part_info, SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  
  
  PartitionType partition_type = part_info.partition_type_;
  int64_t subpart_idx = part_info.sub_part_idx_;
  if (subpart_idx < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("subpart_idx is invalid", KR(ret), K(part_info));
  } else if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                               part_info.table_id_)))
      || OB_FAIL(dml.add_pk_column("part_id", part_info.part_id_))
      || OB_FAIL(dml.add_pk_column("sub_part_id", part_info.sub_part_id_))
      || OB_FAIL(dml.add_column("sub_part_idx", subpart_idx))
      || OB_FAIL(dml.add_column("sub_part_name", ObHexEscapeSqlStr(part_info.part_name_)))
      || OB_FAIL(dml.add_column("schema_version", part_info.schema_version_))
      || OB_FAIL(dml.add_column("status", part_info.status_))
      || OB_FAIL(dml.add_column("spare1", 0 /*unused now*/))
      || OB_FAIL(dml.add_column("spare2", 0 /*unused now*/))
      || OB_FAIL(dml.add_column("spare3", "" /*unused now*/))
      || OB_FAIL(dml.add_column("comment", "" /*unused now*/))
      || OB_FAIL(dml.add_column("high_bound_val",
                                ObHexEscapeSqlStr(part_info.high_bound_val_)))
      || OB_FAIL(dml.add_column("b_high_bound_val",
                                part_info.b_high_bound_val_))
      || OB_FAIL(dml.add_column("list_val", ObHexEscapeSqlStr(part_info.list_val_)))
      || OB_FAIL(dml.add_column("b_list_val", part_info.b_list_val_))
      || OB_FAIL(dml.add_column("partition_type", partition_type))
      || OB_FAIL(dml.add_column("tablet_id", part_info.tablet_id_.id()))) {
    LOG_WARN("dml add part info failed", K(ret));
  }
  if (OB_FAIL(ret)) {
    //nothing todo
  }
  return ret;
}

int ObAddIncSubPartDMLGenerator::extract_part_info(PartInfo &part_info)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(ori_table_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else if (part_idx_ < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid part_idx", K(part_idx_), K(ret));
  } else {
    
    part_info.table_id_ = ori_table_->get_table_id();
    part_info.part_id_ = part_.get_part_id();
    part_info.sub_part_id_ = sub_part_.get_sub_part_id();
    part_info.part_name_ = sub_part_.get_part_name();
    part_info.schema_version_ = schema_version_;
    part_info.status_ = PARTITION_STATUS_INVALID;
    part_info.sub_part_idx_ = sub_part_.get_sub_part_idx();
    part_info.partition_type_ = sub_part_.get_partition_type();
    part_info.tablet_id_ = sub_part_.get_tablet_id();
    if (OB_FAIL(ret)) {
    } else if (ori_table_->is_range_subpart()) {
      if (OB_FAIL(gen_high_bound_val_str(sub_part_.get_high_bound_val(),
                                         part_info.high_bound_val_,
                                         part_info.b_high_bound_val_))) {
      }
    } else if (ori_table_->is_list_subpart()) {
      if (OB_FAIL(gen_list_val_str(
                  sub_part_.get_list_row_values(),
                  part_info.list_val_,
                  part_info.b_list_val_))) {
      }
    } else if (is_hash_like_part(ori_table_->get_sub_part_option().get_part_func_type())) {
      part_info.sub_part_idx_ = subpart_idx_;
    }
  }

  return ret;
}

int ObAddIncPartDMLGenerator::convert_to_dml(const PartInfo &part_info, SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  
  
  PartitionType partition_type = part_info.partition_type_;
  int64_t part_idx = part_info.part_idx_;
  if (part_idx < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("part_idx is invalid", KR(ret), K(part_info));
  } else if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                               part_info.table_id_)))
      || OB_FAIL(dml.add_pk_column("part_id", part_info.part_id_))
      || OB_FAIL(dml.add_column("part_idx", part_idx))
      || OB_FAIL(dml.add_column("part_name", ObHexEscapeSqlStr(part_info.part_name_)))
      || OB_FAIL(dml.add_column("schema_version", part_info.schema_version_))
      || OB_FAIL(dml.add_column("sub_part_num", part_info.sub_part_num_))
      || OB_FAIL(dml.add_column("sub_part_space", 0 /*unused now*/))
      || OB_FAIL(dml.add_column("new_sub_part_space", 0 /*unused now*/))
      || OB_FAIL(dml.add_column("status", part_info.status_))
      || OB_FAIL(dml.add_column("spare1", 0 /*unused now*/))
      || OB_FAIL(dml.add_column("spare2", 0 /*unused now*/))
      || OB_FAIL(dml.add_column("spare3", ""/*unused now*/))
      || OB_FAIL(dml.add_column("comment", "" /*unused now*/))
      || OB_FAIL(dml.add_column("high_bound_val",
                                ObHexEscapeSqlStr(part_info.high_bound_val_)))
      || OB_FAIL(dml.add_column("b_high_bound_val",
                                part_info.b_high_bound_val_))
      || OB_FAIL(dml.add_column("list_val", ObHexEscapeSqlStr(part_info.list_val_)))
      || OB_FAIL(dml.add_column("b_list_val", part_info.b_list_val_))
      || OB_FAIL(dml.add_column("partition_type", partition_type))
      || OB_FAIL(dml.add_column("tablet_id", part_info.tablet_id_.id()))) {
    LOG_WARN("dml add part info failed", K(ret));
  }
  if (OB_FAIL(ret)) {
    //nothing todo
  }
  return ret;
}

int ObAddIncPartDMLGenerator::extract_part_info(PartInfo &part_info)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(ori_table_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table is null", K(ret));
  } else {
    int64_t sub_part_num = 0;
    if (PARTITION_LEVEL_TWO == ori_table_->get_part_level()) {
      sub_part_num = part_.get_sub_part_num();
    }
    
    part_info.table_id_ = ori_table_->get_table_id();
    part_info.part_id_ = part_.get_part_id();
    part_info.schema_version_ = schema_version_;
    part_info.sub_part_num_ = sub_part_num;
    part_info.status_ = PARTITION_STATUS_INVALID;
    part_info.part_idx_ = part_.get_part_idx();
    part_info.partition_type_ = part_.get_partition_type();
    part_info.tablet_id_ = part_.get_tablet_id();
    part_info.part_name_ = part_.get_part_name();

    if (OB_FAIL(ret)) {
    } else if (ori_table_->is_range_part()) {
      if (OB_FAIL(gen_high_bound_val_str(part_.get_high_bound_val(),
                                         part_info.high_bound_val_,
                                         part_info.b_high_bound_val_))) {
      }
    } else if (ori_table_->is_list_part()) {
      if (OB_FAIL(gen_list_val_str(part_.get_list_row_values(),
                                   part_info.list_val_,
                                   part_info.b_list_val_))) {
      }
    } else if (is_hash_like_part(ori_table_->get_part_option().get_part_func_type())) {
      part_info.part_idx_ = part_.get_part_idx();
    }
  }

  return ret;
}

int ObDropIncSubPartDMLGenerator::convert_to_dml(const PartInfo &part_info, SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  
  
  const int64_t deleted = true;
  if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                               part_info.table_id_)))
      || OB_FAIL(dml.add_pk_column("part_id", part_info.part_id_))
      || OB_FAIL(dml.add_pk_column("sub_part_id", part_info.sub_part_id_))
      || OB_FAIL(dml.add_column("is_deleted", deleted))
      || OB_FAIL(dml.add_column("schema_version", part_info.schema_version_))) {
    LOG_WARN("dml drop part info failed", K(ret));
  }
  return ret;
}

int ObDropIncSubPartDMLGenerator::extract_part_info(PartInfo &part_info)
{
  int ret = OB_SUCCESS;

  
  part_info.table_id_ = sub_part_.get_table_id();
  part_info.part_id_ = sub_part_.get_part_id();
  part_info.sub_part_id_ = sub_part_.get_sub_part_id();
  part_info.schema_version_ = schema_version_;

  return ret;
}

int ObDropIncPartDMLGenerator::convert_to_dml(const PartInfo &part_info, SchemaCatalogRow &dml)
{
  int ret = OB_SUCCESS;
  
  
  if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                               part_info.table_id_)))
      || OB_FAIL(dml.add_pk_column("part_id", part_info.part_id_))
      || OB_FAIL(dml.add_column("schema_version", part_info.schema_version_))) {
    LOG_WARN("dml drop part info failed", K(ret));
  }
  return ret;
}

int ObDropIncPartDMLGenerator::extract_part_info(PartInfo &part_info)
{
  int ret = OB_SUCCESS;

  
  part_info.table_id_ = part_.get_table_id();
  part_info.part_id_ = part_.get_part_id();
  part_info.schema_version_ = schema_version_;

  return ret;
}

int ObUpdatePartHelper::update_partition_info()
{
  if (ori_table_ == nullptr || upd_table_ == nullptr) { return OB_ERR_UNEXPECTED; }
  if (!ori_table_->is_user_partition_table()) { return OB_INVALID_ARGUMENT; }
  if (ori_table_->get_part_level() == PARTITION_LEVEL_TWO) { return OB_NOT_SUPPORTED; }
  return observer::with_ddl_schema_catalog(sql_client_, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
    int ret = OB_SUCCESS;
    const int64_t visible = upd_table_->get_partition_num();
    const int64_t count = visible + upd_table_->get_hidden_partition_num();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      auto **parts = i < visible ? upd_table_->get_part_array() : upd_table_->get_hidden_part_array();
      const auto *part = parts == nullptr ? nullptr : parts[i < visible ? i : i - visible];
      if (part == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        HEAP_VAR(ObAddIncPartDMLGenerator, generator, ori_table_, *part, count, i, schema_version_) {
          bool matched = false;
          ret = catalog.update_fields(OB_ALL_PART_TID, [&](SchemaCatalogRow &fields) {
            int ret = generator.gen_dml(fields);
            if (OB_SUCC(ret)) { ret = fields.add_gmt_modified(); }
            return ret;
          }, matched);
          if (OB_SUCC(ret) && !matched) { ret = OB_ERR_UNEXPECTED; }
          if (OB_SUCC(ret)) {
            ret = write_partition_rows(catalog, OB_ALL_PART_TID, OB_ALL_PART_HISTORY_TID,
                [&](SchemaCatalogRow &fields) { return generator.gen_dml(fields); }, true, false);
          }
        }
      }
    }
    return ret;
  });
}

// Add the incremental partition definitions recorded by inc_table_.
int ObAddIncPartHelper::add_partition_info()
{
  if (ori_table_ == nullptr || inc_table_ == nullptr) { return OB_ERR_UNEXPECTED; }
  if (!ori_table_->is_user_partition_table() || inc_table_->get_partition_num() == 0) { return OB_SUCCESS; }
  return observer::with_ddl_schema_catalog(sql_client_, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
    int ret = OB_SUCCESS;
    const int64_t count = inc_table_->get_partition_num();
    auto **parts = inc_table_->get_part_array();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      const auto *part = parts == nullptr ? nullptr : parts[i];
      if (part == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        HEAP_VAR(ObAddIncPartDMLGenerator, generator, ori_table_, *part, count, i, schema_version_) {
          ret = write_partition_rows(catalog, OB_ALL_PART_TID, OB_ALL_PART_HISTORY_TID,
              [&](SchemaCatalogRow &fields) { return generator.gen_dml(fields); }, false, false);
        }
        if (ori_table_->get_part_level() == PARTITION_LEVEL_TWO) {
          auto **subparts = part->get_subpart_array();
          for (int64_t j = 0; OB_SUCC(ret) && j < part->get_subpartition_num(); ++j) {
            if (subparts == nullptr || subparts[j] == nullptr) { ret = OB_ERR_UNEXPECTED; }
            else {
              HEAP_VAR(ObAddIncSubPartDMLGenerator, generator, ori_table_, *part, *subparts[j], count, i, j, schema_version_) {
                ret = write_partition_rows(catalog, OB_ALL_SUB_PART_TID, OB_ALL_SUB_PART_HISTORY_TID,
                    [&](SchemaCatalogRow &fields) { return generator.gen_dml(fields); }, false, false);
              }
            }
          }
        }
      }
    }
    return ret;
  });
}

int ObAddIncSubPartHelper::add_subpartition_info(const bool is_subpart_idx_specified)
{
  if (ori_table_ == nullptr || inc_table_ == nullptr) { return OB_ERR_UNEXPECTED; }
  if (inc_table_->get_partition_num() == 0) { return OB_SUCCESS; }
  return observer::with_ddl_schema_catalog(sql_client_, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
    int ret = OB_SUCCESS;
    const int64_t count = inc_table_->get_partition_num();
    auto **parts = inc_table_->get_part_array();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      const auto *part = parts == nullptr ? nullptr : parts[i];
      if (part == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        auto **subparts = part->get_subpart_array();
        for (int64_t j = 0; OB_SUCC(ret) && j < part->get_subpartition_num(); ++j) {
          if (subparts == nullptr || subparts[j] == nullptr) { ret = OB_ERR_UNEXPECTED; }
          else {
            const int64_t index = is_subpart_idx_specified ? subparts[j]->get_sub_part_idx() : j;
            HEAP_VAR(ObAddIncSubPartDMLGenerator, generator, ori_table_, *part, *subparts[j], count, i, index, schema_version_) {
              ret = write_partition_rows(catalog, OB_ALL_SUB_PART_TID, OB_ALL_SUB_PART_HISTORY_TID,
                  [&](SchemaCatalogRow &fields) { return generator.gen_dml(fields); }, false, false);
            }
          }
        }
      }
    }
    return ret;
  });
}

int ObDropIncPartHelper::drop_partition_info()
{
  if (ori_table_ == nullptr || inc_table_ == nullptr) { return OB_ERR_UNEXPECTED; }
  if (!ori_table_->is_user_partition_table() || inc_table_->get_partition_num() == 0) { return OB_SUCCESS; }
  return observer::with_ddl_schema_catalog(sql_client_, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
    int ret = OB_SUCCESS;
    auto **parts = inc_table_->get_part_array();
    for (int64_t i = 0; OB_SUCC(ret) && i < inc_table_->get_partition_num(); ++i) {
      const auto *part = parts == nullptr ? nullptr : parts[i];
      if (part == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        HEAP_VAR(ObDropIncPartDMLGenerator, generator, *part, schema_version_) {
          ret = write_partition_rows(catalog, OB_ALL_PART_TID, OB_ALL_PART_HISTORY_TID,
              [&](SchemaCatalogRow &fields) { return generator.gen_dml(fields); }, true, true);
        }
        auto **subparts = part->get_subpart_array();
        for (int64_t j = 0; OB_SUCC(ret) && j < part->get_subpartition_num(); ++j) {
          if (subparts == nullptr || subparts[j] == nullptr) { ret = OB_ERR_UNEXPECTED; }
          else {
            HEAP_VAR(ObDropIncSubPartDMLGenerator, generator, *subparts[j], schema_version_) {
              ret = catalog.insert_fields(OB_ALL_SUB_PART_HISTORY_TID,
                  [&](SchemaCatalogRow &fields) { return generator.gen_dml(fields); });
            }
          }
        }
      }
    }
    return ret;
  });
}

int ObDropIncSubPartHelper::drop_subpartition_info()
{
  if (ori_table_ == nullptr || inc_table_ == nullptr) { return OB_ERR_UNEXPECTED; }
  if (!ori_table_->is_user_partition_table() || inc_table_->get_partition_num() == 0) { return OB_SUCCESS; }
  return observer::with_ddl_schema_catalog(sql_client_, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
    int ret = OB_SUCCESS;
    auto **parts = inc_table_->get_part_array();
    for (int64_t i = 0; OB_SUCC(ret) && i < inc_table_->get_partition_num(); ++i) {
      const auto *part = parts == nullptr ? nullptr : parts[i];
      if (part == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        auto **subparts = part->get_subpart_array();
        for (int64_t j = 0; OB_SUCC(ret) && j < part->get_subpartition_num(); ++j) {
          if (subparts == nullptr || subparts[j] == nullptr) { ret = OB_ERR_UNEXPECTED; }
          else {
            HEAP_VAR(ObDropIncSubPartDMLGenerator, generator, *subparts[j], schema_version_) {
              ret = catalog.insert_fields(OB_ALL_SUB_PART_HISTORY_TID,
                  [&](SchemaCatalogRow &fields) { return generator.gen_dml(fields); });
            }
          }
        }
      }
    }
    return ret;
  });
}

int ObRenameIncPartHelper::rename_partition_info(const bool update_part_idx)
{
  if (ori_table_ == nullptr || inc_table_ == nullptr) { return OB_ERR_UNEXPECTED; }
  if (!ori_table_->is_user_partition_table()) { return OB_NOT_SUPPORTED; }
  auto **parts = inc_table_->get_part_array();
  if (inc_table_->get_partition_num() != 1 || parts == nullptr || parts[0] == nullptr) { return OB_ERR_UNEXPECTED; }
  const auto &part = *parts[0];
  return observer::with_ddl_schema_catalog(sql_client_, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
    bool matched = false;
    int ret = catalog.update_fields(OB_ALL_PART_TID, [&](SchemaCatalogRow &fields) {
      int ret = OB_SUCCESS;
      if (OB_FAIL(fields.add_pk_column("table_id", ori_table_->get_table_id()))
          || OB_FAIL(fields.add_pk_column("part_id", part.get_part_id()))
          || OB_FAIL(fields.add_column("schema_version", schema_version_))
          || OB_FAIL(fields.add_column("part_name", part.get_part_name()))
          || OB_FAIL(fields.add_gmt_modified())) {}
      if (OB_SUCC(ret) && update_part_idx) { ret = fields.add_column("part_idx", part.get_part_idx()); }
      return ret;
    }, matched);
    if (OB_SUCC(ret) && !matched) { ret = OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret)) {
      HEAP_VAR(ObAddIncPartDMLGenerator, generator, ori_table_, part, 1, part.get_part_idx(), schema_version_) {
        ret = write_partition_rows(catalog, OB_ALL_PART_TID, OB_ALL_PART_HISTORY_TID,
            [&](SchemaCatalogRow &fields) { return generator.gen_dml(fields); }, true, false);
      }
    }
    return ret;
  });
}

int ObRenameIncSubpartHelper::rename_subpartition_info()
{
  if (ori_table_ == nullptr || inc_table_ == nullptr) { return OB_ERR_UNEXPECTED; }
  if (!ori_table_->is_user_subpartition_table()) { return OB_NOT_SUPPORTED; }
  auto **parts = inc_table_->get_part_array();
  if (inc_table_->get_partition_num() != 1 || parts == nullptr || parts[0] == nullptr) { return OB_ERR_UNEXPECTED; }
  const auto &part = *parts[0];
  auto **subparts = part.get_subpart_array();
  if (part.get_subpartition_num() != 1 || subparts == nullptr || subparts[0] == nullptr) { return OB_ERR_UNEXPECTED; }
  const auto &subpart = *subparts[0];
  return observer::with_ddl_schema_catalog(sql_client_, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
    bool matched = false;
    int ret = catalog.update_fields(OB_ALL_SUB_PART_TID, [&](SchemaCatalogRow &fields) {
      int ret = OB_SUCCESS;
      if (OB_FAIL(fields.add_pk_column("table_id", ori_table_->get_table_id()))
          || OB_FAIL(fields.add_pk_column("part_id", part.get_part_id()))
          || OB_FAIL(fields.add_pk_column("sub_part_id", subpart.get_sub_part_id()))
          || OB_FAIL(fields.add_column("schema_version", schema_version_))
          || OB_FAIL(fields.add_column("sub_part_name", subpart.get_part_name()))
          || OB_FAIL(fields.add_gmt_modified())) {}
      return ret;
    }, matched);
    if (OB_SUCC(ret) && !matched) { ret = OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret)) {
      HEAP_VAR(ObAddIncSubPartDMLGenerator, generator, ori_table_, part, subpart, 1,
               part.get_part_idx(), subpart.get_sub_part_idx(), schema_version_) {
        ret = write_partition_rows(catalog, OB_ALL_SUB_PART_TID, OB_ALL_SUB_PART_HISTORY_TID,
            [&](SchemaCatalogRow &fields) { return generator.gen_dml(fields); }, true, false);
      }
    }
    return ret;
  });
}
} //end of schema
} //end of share
} //end of oceanbase
