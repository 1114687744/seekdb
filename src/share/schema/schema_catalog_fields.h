/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifndef OCEANBASE_SHARE_SCHEMA_CATALOG_FIELDS_H_
#define OCEANBASE_SHARE_SCHEMA_CATALOG_FIELDS_H_

#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_constraint.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/ob_timezone_mgr.h"

namespace oceanbase {
namespace share {
namespace schema {
// The single field mapping for persisted table definitions and their relationships. A sink
// may encode SQL or build native typed cells; it must own variable-length values.
class SchemaCatalogFields final
{
public:
  template<class RowWriter>
  static int table(const ObTableSchema &, bool update_object_status_ignore_version,
                   RowWriter &, bool is_history);
  template<class RowWriter>
  static int partition_options(const ObTableSchema &, RowWriter &);
  template<class RowWriter>
  static int column(const ObColumnSchemaV2 &, RowWriter &, bool is_history);
  template<class RowWriter>
  static int foreign_key(const ObForeignKeyInfo &, RowWriter &);
  template<class RowWriter>
  static int foreign_key_column(uint64_t foreign_key_id, uint64_t child_column_id,
                                uint64_t parent_column_id, int64_t position, RowWriter &);
  template<class RowWriter>
  static int mock_parent(const ObMockFKParentTableSchema &, RowWriter &);
  template<class RowWriter>
  static int mock_parent_column(const ObMockFKParentTableSchema &, int64_t index, RowWriter &);
  template<class RowWriter>
  static int constraint(const ObConstraint &, RowWriter &);
  template<class RowWriter>
  static int constraint_column(const ObConstraint &, uint64_t column_id, RowWriter &);
};

template<class RowWriter>
int SchemaCatalogFields::table(
    const ObTableSchema &table,
    const bool update_object_status_ignore_version,
    RowWriter &dml,
    const bool is_history)
{
  using namespace common;
  int ret = OB_SUCCESS;
  ObString empty_str("");
  const ObPartitionOption &part_option = table.get_part_option();
  const ObSubPartitionOption &sub_part_option = table.get_sub_part_option();
  const char *part_func_expr = part_option.get_part_func_expr_str().length() <= 0 ?
    "" : part_option.get_part_func_expr_str().ptr();
  const char *sub_part_func_expr = sub_part_option.get_part_func_expr_str().length() <= 0 ?
    "" : sub_part_option.get_part_func_expr_str().ptr();
  const int64_t part_num = part_option.get_part_num();
  const int64_t sub_part_num = sub_part_option.get_part_num();
  ObString index_params = table.get_index_params().empty() ? empty_str : table.get_index_params();
  const ObString parser_properties = table.get_parser_property_str().empty() ? empty_str : table.get_parser_property_str();
  ObArenaAllocator allocator(ObModIds::OB_SCHEMA_OB_SCHEMA_ARENA);
  if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
            table.get_table_id())))
      || OB_FAIL(dml.add_column("table_name", ObHexEscapeSqlStr(table.get_table_name())))
      || OB_FAIL(dml.add_column("database_id", ObSchemaUtils::get_extract_schema_id(
            table.get_database_id())))
      || OB_FAIL(dml.add_column("table_type", table.get_table_type()))
      || OB_FAIL(dml.add_column("load_type", table.get_load_type()))
      || OB_FAIL(dml.add_column("def_type", table.get_def_type()))
      || OB_FAIL(dml.add_column("rowkey_column_num", table.get_rowkey_column_num()))
      || OB_FAIL(dml.add_column("index_column_num", table.get_index_column_num()))
      || OB_FAIL(dml.add_column("max_used_column_id", table.get_max_used_column_id()))
      || OB_FAIL(dml.add_column("session_id", table.get_session_id()))
      || OB_FAIL(dml.add_column("tablet_size", table.get_tablet_size()))
      || OB_FAIL(dml.add_column("pctfree", table.get_pctfree()))
      || OB_FAIL(dml.add_column("autoinc_column_id", table.get_autoinc_column_id()))
      || OB_FAIL(dml.add_column("auto_increment", share::ObRealUInt64(table.get_auto_increment())))
      || OB_FAIL(dml.add_column("read_only", table.is_read_only()))
      || OB_FAIL(dml.add_column("rowkey_split_pos", table.get_rowkey_split_pos()))
      || OB_FAIL(dml.add_column("compress_func_name", ObHexEscapeSqlStr(table.get_compress_func_name())))
      || OB_FAIL(dml.add_column("index_attributes_set", table.get_index_attributes_set()))
      || OB_FAIL(dml.add_column("comment", ObHexEscapeSqlStr(table.get_comment())))
      || OB_FAIL(dml.add_column("block_size", table.get_block_size()))
      || OB_FAIL(dml.add_column("collation_type", table.get_collation_type()))
      || OB_FAIL(dml.add_column("data_table_id", ObSchemaUtils::get_extract_schema_id(
              table.get_data_table_id())))
      || OB_FAIL(dml.add_column("index_status", table.get_index_status()))
      || OB_FAIL(dml.add_column("progressive_merge_num", table.get_progressive_merge_num()))
      || OB_FAIL(dml.add_column("index_type", table.get_index_type()))
      || OB_FAIL(dml.add_column("index_using_type", table.get_index_using_type()))
      || OB_FAIL(dml.add_column("part_level", table.get_part_level()))
      || OB_FAIL(dml.add_column("part_func_type", part_option.get_part_func_type()))
      || OB_FAIL(dml.add_column("part_func_expr", ObHexEscapeSqlStr(part_func_expr)))
      || OB_FAIL(dml.add_column("part_num", part_num))
      || OB_FAIL(dml.add_column("sub_part_func_type", sub_part_option.get_part_func_type()))
      || OB_FAIL(dml.add_column("sub_part_func_expr", ObHexEscapeSqlStr(sub_part_func_expr)))
      || OB_FAIL(dml.add_column("sub_part_num", sub_part_num))
      || OB_FAIL(is_history
                     ? dml.add_pk_column("schema_version", table.get_schema_version())
                     : dml.add_column("schema_version", table.get_schema_version()))
      || OB_FAIL(dml.add_column("view_definition", ObHexEscapeSqlStr(table.get_view_schema().get_view_definition())))
      || OB_FAIL(dml.add_column("view_check_option", table.get_view_schema().get_view_check_option()))
      || OB_FAIL(dml.add_column("view_is_updatable", table.get_view_schema().get_view_is_updatable()))
      || OB_FAIL(dml.add_column("parser_name", ObHexEscapeSqlStr(table.get_parser_name_str())))
      || OB_FAIL(dml.add_column("partition_status", table.get_partition_status()))
      || OB_FAIL(dml.add_column("partition_schema_version", table.get_partition_schema_version()))
      || OB_FAIL(dml.add_column("pk_comment", ObHexEscapeSqlStr(table.get_pk_comment())))
      || OB_FAIL(dml.add_column("row_store_type",
            ObHexEscapeSqlStr(ObStoreFormat::get_row_store_name(table.get_row_store_type()))))
      || OB_FAIL(dml.add_column("store_format",
            ObHexEscapeSqlStr(ObStoreFormat::get_store_format_name(table.get_store_format()))))
      || OB_FAIL(dml.add_column("progressive_merge_round", table.get_progressive_merge_round()))
      || OB_FAIL(dml.add_column("table_mode", table.get_table_mode()))
      || OB_FAIL(dml.add_column("tablespace_id", ObSchemaUtils::get_extract_schema_id(
              table.get_tablespace_id())))
      || OB_FAIL(dml.add_column("sub_part_template_flags", table.get_sub_part_template_flags()))
      || OB_FAIL(dml.add_column("dop", table.get_dop()))
      || OB_FAIL(dml.add_column("character_set_client", table.get_view_schema().get_character_set_client()))
      || OB_FAIL(dml.add_column("collation_connection", table.get_view_schema().get_collation_connection()))
      || OB_FAIL(dml.add_column("association_table_id", ObSchemaUtils::get_extract_schema_id(
              table.get_association_table_id())))
      || OB_FAIL(dml.add_column("define_user_id", ObSchemaUtils::get_extract_schema_id(
              table.get_define_user_id())))
      || OB_FAIL(dml.add_column("max_dependency_version", table.get_max_dependency_version()))
      || (OB_FAIL(dml.add_column("tablet_id", table.get_tablet_id().id())))
      || (OB_FAIL(dml.add_column("object_status", static_cast<int64_t> (table.get_object_status()))))
      || (OB_FAIL(dml.add_column("table_flags", table.get_table_flags())))
      || (OB_FAIL(dml.add_column("truncate_version", table.get_truncate_version())))
      || (OB_FAIL(dml.add_column("name_generated_type", table.get_name_generated_type())))
      || (OB_FAIL(dml.add_column("lob_inrow_threshold", table.get_lob_inrow_threshold())))
      || (OB_FAIL(dml.add_column("auto_increment_cache_size", table.get_auto_increment_cache_size())))
      || (OB_FAIL(dml.add_column("index_params", ObHexEscapeSqlStr(index_params))))
      || (OB_FAIL(dml.add_column("micro_index_clustered", table.get_micro_index_clustered())))
      || (OB_FAIL(dml.add_column("parser_properties", ObHexEscapeSqlStr(parser_properties))))
      || (OB_FAIL(dml.add_column("semistruct_encoding_type", table.get_semistruct_encoding_flags())))
      ) {
        SHARE_SCHEMA_LOG(WARN, "add column failed", K(ret));
      }

  return ret;
}

template<class RowWriter>
int SchemaCatalogFields::partition_options(const ObTableSchema &table, RowWriter &dml)
{
  int ret = OB_SUCCESS;

  const uint64_t table_id = table.get_table_id();

  const ObPartitionOption &part_option = table.get_part_option();
  const ObSubPartitionOption &sub_part_option = table.get_sub_part_option();
  const char *part_func_expr = part_option.get_part_func_expr_str().length() <= 0 ?
  "" : part_option.get_part_func_expr_str().ptr();
  const char *sub_part_func_expr = sub_part_option.get_part_func_expr_str().length() <= 0 ?
  "" : sub_part_option.get_part_func_expr_str().ptr();
  const int64_t part_num = part_option.get_part_num();
  const int64_t sub_part_num = PARTITION_LEVEL_TWO == table.get_part_level()
                               && table.has_sub_part_template_def() ?
                               sub_part_option.get_part_num() : 0;
  if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                               table_id)))
      || OB_FAIL(dml.add_column("part_level", table.get_part_level()))
      || OB_FAIL(dml.add_column("part_func_type", part_option.get_part_func_type()))
      || OB_FAIL(dml.add_column("part_func_expr", ObHexEscapeSqlStr(part_func_expr)))
      || OB_FAIL(dml.add_column("part_num", part_num))
      || OB_FAIL(dml.add_column("sub_part_func_type", sub_part_option.get_part_func_type()))
      || OB_FAIL(dml.add_column("sub_part_func_expr", ObHexEscapeSqlStr(sub_part_func_expr)))
      || OB_FAIL(dml.add_column("sub_part_num", sub_part_num))
      || OB_FAIL(dml.add_column("schema_version", table.get_schema_version()))
      || OB_FAIL(dml.add_column("partition_status", table.get_partition_status()))
      || OB_FAIL(dml.add_column("partition_schema_version", table.get_partition_schema_version()))
      || OB_FAIL(dml.add_column("sub_part_template_flags", table.get_sub_part_template_flags()))
      || OB_FAIL(dml.add_gmt_create())
      || (OB_FAIL(dml.add_column("table_flags", table.get_table_flags())))
      || OB_FAIL(dml.add_gmt_modified())) {
    LOG_WARN("add column failed", K(ret));
  }
  return ret;
}

template<class RowWriter>
int SchemaCatalogFields::column(
    const ObColumnSchemaV2 &column,
    RowWriter &dml,
    const bool is_history)
{
  using namespace common;
  int ret = OB_SUCCESS;
  ObString orig_default_value;
  ObString cur_default_value;
  ObArenaAllocator allocator(ObModIds::OB_SCHEMA_OB_SCHEMA_ARENA);
  char *extended_type_info_buf = NULL;
  if (column.is_generated_column() ||
      ob_is_string_type(column.get_data_type()) ||
      ob_is_json(column.get_data_type()) ||
      ob_is_geometry(column.get_data_type())) {
    //The default value of the generated column is the expression definition of the generated column
    ObString orig_default_value_str = column.get_orig_default_value().get_string();
    ObString cur_default_value_str = column.get_cur_default_value().get_string();
    orig_default_value.assign_ptr(orig_default_value_str.ptr(), orig_default_value_str.length());
    cur_default_value.assign_ptr(cur_default_value_str.ptr(), cur_default_value_str.length());
    if (!column.get_orig_default_value().is_null() && OB_ISNULL(orig_default_value.ptr())) {
      orig_default_value.assign_ptr("", 0);
    }
    if (!column.get_cur_default_value().is_null() && OB_ISNULL(cur_default_value.ptr())) {
      cur_default_value.assign_ptr("", 0);
    }
  } else {
    const int64_t value_buf_len = 2 * OB_MAX_DEFAULT_VALUE_LENGTH + 3;
    char *orig_default_value_buf = NULL;
    char *cur_default_value_buf = NULL;
    orig_default_value_buf = static_cast<char *>(allocator.alloc(value_buf_len));
    cur_default_value_buf = static_cast<char *>(allocator.alloc(value_buf_len));
    extended_type_info_buf = static_cast<char *>(allocator.alloc(OB_MAX_VARBINARY_LENGTH));
    if (OB_ISNULL(orig_default_value_buf)
        || OB_ISNULL(cur_default_value_buf)
        || OB_ISNULL(extended_type_info_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      SHARE_SCHEMA_LOG(WARN, "allocate memory for default value buffer failed");
    } else {
      orig_default_value_buf[0] = '\0';
      cur_default_value_buf[0] = '\0';
      extended_type_info_buf[0] = '\0';

      int64_t orig_default_value_len = 0;
      int64_t cur_default_value_len = 0;
      ObTimeZoneInfo tz_info;
      if (OB_FAIL(OTTZ_MGR.get_timezone_map(tz_info.get_tz_map_wrap()))) {
      } else if (OB_FAIL(column.get_orig_default_value().print_plain_str_literal(
                      orig_default_value_buf, value_buf_len, orig_default_value_len, &tz_info))) {
      } else if (OB_FAIL(column.get_cur_default_value().print_plain_str_literal(
                             cur_default_value_buf, value_buf_len, cur_default_value_len, &tz_info))) {
      } else {
        orig_default_value.assign_ptr(orig_default_value_buf, static_cast<int32_t>(orig_default_value_len));
        cur_default_value.assign_ptr(cur_default_value_buf, static_cast<int32_t>(cur_default_value_len));
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObString cur_default_value_v1;
    if (column.get_orig_default_value().is_null()) {
      orig_default_value.reset();
    }
    if (column.get_cur_default_value().is_null()) {
      cur_default_value.reset();
    }
    ObString bin_extended_type_info;
    if (OB_SUCC(ret) && (column.is_enum_or_set() || column.is_collection())) {
      int64_t pos = 0;
      extended_type_info_buf = static_cast<char *>(allocator.alloc(OB_MAX_VARBINARY_LENGTH));
      if (OB_ISNULL(extended_type_info_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        SHARE_SCHEMA_LOG(WARN, "allocate memory for default value buffer failed", K(ret));
      } else if (OB_FAIL(column.serialize_extended_type_info(extended_type_info_buf, OB_MAX_VARBINARY_LENGTH, pos))) {
      } else {
        bin_extended_type_info.assign_ptr(extended_type_info_buf, static_cast<int32_t>(pos));
      }
    }
    ObString local_session_var;
    if (OB_SUCC(ret) && column.is_generated_column()
        && OB_FAIL(column.get_local_session_var().gen_local_session_var_str(allocator, local_session_var))) {
      SHARE_SCHEMA_LOG(WARN, "fail to gen local session var str", K(ret));
    }
    if (OB_SUCC(ret) && (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                                    column.get_table_id())))
                         || OB_FAIL(dml.add_pk_column("column_id", column.get_column_id()))
                         || OB_FAIL(dml.add_column("column_name", ObHexEscapeSqlStr(column.get_column_name())))
                         || OB_FAIL(dml.add_column("rowkey_position", column.get_rowkey_position()))
                         || OB_FAIL(dml.add_column("index_position", column.get_index_position()))
                         || OB_FAIL(dml.add_column("partition_key_position", column.get_tbl_part_key_pos()))
                         || OB_FAIL(dml.add_column("data_type", column.get_data_type()))
                         || OB_FAIL(dml.add_column("data_length", column.get_data_length()))
                         || OB_FAIL(dml.add_column("data_precision", column.get_data_precision()))
                         || OB_FAIL(dml.add_column("data_scale", column.get_data_scale()))
                         || OB_FAIL(dml.add_column("zero_fill", column.is_zero_fill()))
                         || OB_FAIL(dml.add_column("nullable", column.is_nullable()))
                         || OB_FAIL(dml.add_column("autoincrement", column.is_autoincrement()))
                         || OB_FAIL(dml.add_column("is_hidden", column.is_hidden()))
                         || OB_FAIL(dml.add_column("on_update_current_timestamp", column.is_on_update_current_timestamp()))
                         || OB_FAIL(dml.add_column("orig_default_value_v2", ObHexEscapeSqlStr(orig_default_value)))
                         || OB_FAIL(dml.add_column("cur_default_value_v2", ObHexEscapeSqlStr(cur_default_value)))
                         || OB_FAIL(dml.add_column("cur_default_value", ObHexEscapeSqlStr(cur_default_value_v1)))
                         || OB_FAIL(dml.add_column("order_in_rowkey", column.get_order_in_rowkey()))
                         || OB_FAIL(dml.add_column("collation_type", column.get_collation_type()))
                         || OB_FAIL(dml.add_column("comment", ObHexEscapeSqlStr(column.get_comment())))
                         || OB_FAIL(is_history
                                        ? dml.add_pk_column("schema_version", column.get_schema_version())
                                        : dml.add_column("schema_version", column.get_schema_version()))
                         || OB_FAIL(dml.add_column("column_flags", column.get_stored_column_flags()))
                         || OB_FAIL(dml.add_column("extended_type_info", ObHexEscapeSqlStr(bin_extended_type_info)))
                         || OB_FAIL(dml.add_column("prev_column_id", column.get_prev_column_id()))
                         || (OB_FAIL(dml.add_column("srs_id", column.get_srs_id())))
                         || (OB_FAIL(dml.add_column("udt_set_id", column.get_udt_set_id())))
                         || (OB_FAIL(dml.add_column("sub_data_type", column.get_sub_data_type())))
                         || (OB_FAIL(dml.add_column("skip_index_attr", column.get_skip_index_attr().get_packed_value())))
                         || (OB_FAIL(dml.add_column("lob_chunk_size", column.get_lob_chunk_size())))
                         || (OB_FAIL(dml.add_column("local_session_vars", ObHexEscapeSqlStr(local_session_var))))
                         || OB_FAIL(dml.add_gmt_create())
                         || OB_FAIL(dml.add_gmt_modified()))) {
      SHARE_SCHEMA_LOG(WARN, "dml add column failed", K(ret));
    }
  }
  return ret;
}

template<class RowWriter>
int SchemaCatalogFields::foreign_key(const ObForeignKeyInfo &foreign_key_info,
                                           RowWriter &dml)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(dml.add_pk_column("foreign_key_id", ObSchemaUtils::get_extract_schema_id(
                                                     foreign_key_info.foreign_key_id_)))
      || OB_FAIL(dml.add_column("foreign_key_name", ObHexEscapeSqlStr(foreign_key_info.foreign_key_name_)))
      || OB_FAIL(dml.add_column("child_table_id",  ObSchemaUtils::get_extract_schema_id(
                                                   foreign_key_info.child_table_id_)))
      || OB_FAIL(dml.add_column("parent_table_id", ObSchemaUtils::get_extract_schema_id(
                                                   foreign_key_info.parent_table_id_)))
      || OB_FAIL(dml.add_column("update_action", foreign_key_info.update_action_))
      || OB_FAIL(dml.add_column("delete_action", foreign_key_info.delete_action_))
      || OB_FAIL(dml.add_column("enable_flag", foreign_key_info.enable_flag_))
      || OB_FAIL(dml.add_column("validate_flag", foreign_key_info.validate_flag_))
      || OB_FAIL(dml.add_column("rely_flag", foreign_key_info.rely_flag_))
      || OB_FAIL(dml.add_column("ref_cst_type", foreign_key_info.fk_ref_type_))
      || OB_FAIL(dml.add_column("ref_cst_id", foreign_key_info.ref_cst_id_))
      || OB_FAIL(dml.add_column("is_parent_table_mock", foreign_key_info.is_parent_table_mock_))
      || (OB_FAIL(dml.add_column("name_generated_type", foreign_key_info.name_generated_type_)))
      || OB_FAIL(dml.add_gmt_create())
      || OB_FAIL(dml.add_gmt_modified())
      ) {
    LOG_WARN("failed to add column", K(ret));
  }
  return ret;
}

template<class RowWriter>
int SchemaCatalogFields::foreign_key_column(
    uint64_t foreign_key_id,
    uint64_t child_column_id,
    uint64_t parent_column_id,
    int64_t position,
    RowWriter &dml)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(dml.add_pk_column("foreign_key_id", ObSchemaUtils::get_extract_schema_id(
                                                     foreign_key_id)))
      || OB_FAIL(dml.add_pk_column("child_column_id", child_column_id))
      || OB_FAIL(dml.add_pk_column("parent_column_id", parent_column_id))
      || OB_FAIL(dml.add_column("position", position))
      || OB_FAIL(dml.add_gmt_create())
      || OB_FAIL(dml.add_gmt_modified())
      ) {
    LOG_WARN("failed to add column", K(ret));
  }
  return ret;
}

template<class RowWriter>
int SchemaCatalogFields::mock_parent(const ObMockFKParentTableSchema &table, RowWriter &fields)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(fields.add_pk_column("mock_fk_parent_table_id", table.get_mock_fk_parent_table_id()))
      || OB_FAIL(fields.add_column("database_id", table.get_database_id()))
      || OB_FAIL(fields.add_column("mock_fk_parent_table_name", table.get_mock_fk_parent_table_name()))
      || OB_FAIL(fields.add_column("schema_version", table.get_schema_version()))
      || OB_FAIL(fields.add_gmt_modified())) {}
  return ret;
}

template<class RowWriter>
int SchemaCatalogFields::mock_parent_column(const ObMockFKParentTableSchema &table, int64_t index, RowWriter &fields)
{
  int ret = OB_SUCCESS;
  const auto &column = table.get_column_array().at(index);
  if (OB_FAIL(fields.add_pk_column("mock_fk_parent_table_id", table.get_mock_fk_parent_table_id()))
      || OB_FAIL(fields.add_pk_column("parent_column_id", column.first))
      || OB_FAIL(fields.add_column("parent_column_name", column.second))
      || OB_FAIL(fields.add_column("schema_version", table.get_schema_version()))
      || OB_FAIL(fields.add_gmt_modified())) {}
  return ret;
}

template<class RowWriter>
int SchemaCatalogFields::constraint(const ObConstraint &constraint, RowWriter &dml)
{
  using namespace common;
  int ret = OB_SUCCESS;
  if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                               constraint.get_table_id())))
      || OB_FAIL(dml.add_pk_column("constraint_id", constraint.get_constraint_id()))
      || OB_FAIL(dml.add_column("schema_version", constraint.get_schema_version()))
      || OB_FAIL(dml.add_column("constraint_type", constraint.get_constraint_type()))
      || OB_FAIL(dml.add_column("constraint_name", ObHexEscapeSqlStr(constraint.get_constraint_name())))
      || OB_FAIL(dml.add_column("check_expr", ObHexEscapeSqlStr(constraint.get_check_expr())))
      || OB_FAIL(dml.add_column("rely_flag", constraint.get_rely_flag()))
      || OB_FAIL(dml.add_column("enable_flag", constraint.get_enable_flag()))
      || OB_FAIL(dml.add_column("validate_flag", constraint.get_validate_flag()))
      || (OB_FAIL(dml.add_column("name_generated_type", constraint.get_name_generated_type())))
      || OB_FAIL(dml.add_gmt_create())
      || OB_FAIL(dml.add_gmt_modified())) {
    LOG_WARN("dml add constraint failed", K(ret));
  }

  return ret;
}


template<class RowWriter>
int SchemaCatalogFields::constraint_column(const ObConstraint &constraint, uint64_t column_id, RowWriter &dml)
{
  using namespace common;
  int ret = OB_SUCCESS;

  if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(
                                               constraint.get_table_id())))
      || OB_FAIL(dml.add_pk_column("constraint_id", constraint.get_constraint_id()))
      || OB_FAIL(dml.add_pk_column("column_id", column_id))
      || OB_FAIL(dml.add_column("schema_version", constraint.get_schema_version()))
      || OB_FAIL(dml.add_gmt_create())
      || OB_FAIL(dml.add_gmt_modified())) {
    LOG_WARN("dml add constraint column failed", K(ret));
  }

  return ret;
}


} // namespace schema
} // namespace share
} // namespace oceanbase
#endif
