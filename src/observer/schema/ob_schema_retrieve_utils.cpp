/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SHARE_SCHEMA
#include "observer/schema/ob_schema_retrieve_utils.h"

namespace oceanbase {
namespace share {
namespace schema {
using namespace common;

int ObSchemaRetrieveUtils::sort_partition_array(ObPartitionSchema &partition_schema)
{
  int ret = OB_SUCCESS;
  if (!partition_schema.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid partition schema", K(ret), K(partition_schema));
  } else if (!partition_schema.is_user_partition_table()) {
    // skip
  } else if (OB_ISNULL(partition_schema.get_part_array())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("partition array is empty", K(ret), K(partition_schema));
  } else {
    int64_t part_num = partition_schema.get_first_part_num();
    int64_t partition_num = partition_schema.get_partition_num();
    if (part_num != partition_num) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("partition num not match", K(ret), K(part_num), K(partition_num));
    } else if (OB_ISNULL(partition_schema.get_part_array())
               || partition_num <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("partition array is empty", K(ret), K(partition_schema));
    } else if (partition_schema.is_range_part()) {
      lib::ob_sort(partition_schema.get_part_array(),
          partition_schema.get_part_array() + partition_num,
          ObBasePartition::range_like_func_less_than);
      lib::ob_sort(partition_schema.get_hidden_part_array(),
          partition_schema.get_hidden_part_array() + partition_schema.get_hidden_partition_num(),
          ObBasePartition::range_like_func_less_than);
    } else if (partition_schema.is_hash_like_part()) {
      lib::ob_sort(partition_schema.get_part_array(),
          partition_schema.get_part_array() + partition_num,
          ObBasePartition::hash_like_func_less_than);
      lib::ob_sort(partition_schema.get_hidden_part_array(),
          partition_schema.get_hidden_part_array() + partition_schema.get_hidden_partition_num(),
          ObBasePartition::hash_like_func_less_than);
    } else if (partition_schema.is_list_part()) {
      lib::ob_sort(partition_schema.get_part_array(),
          partition_schema.get_part_array() + partition_num,
          ObBasePartition::list_part_func_layout);
      lib::ob_sort(partition_schema.get_hidden_part_array(),
          partition_schema.get_hidden_part_array() + partition_schema.get_hidden_partition_num(),
          ObBasePartition::list_part_func_layout);
    } else {
      //nothing
    }
  }
  return ret;
}

int ObSchemaRetrieveUtils::sort_subpartition_array(ObPartitionSchema &partition_schema)
{
  int ret = OB_SUCCESS;
  if (!partition_schema.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid partition schema", K(ret), K(partition_schema));
  } else if (!partition_schema.is_user_partition_table()
             || PARTITION_LEVEL_TWO != partition_schema.get_part_level()) {
    // skip
  } else if (OB_ISNULL(partition_schema.get_part_array())
             || partition_schema.get_partition_num() <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("partition array is empty", K(ret), K(partition_schema));
  } else {
    // sort subpartition array
    for (int64_t i = 0; OB_SUCC(ret) && i < partition_schema.get_partition_num(); i++) {
      ObPartition* partition = partition_schema.get_part_array()[i];
      if (OB_ISNULL(partition)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("partition is null", K(ret), K(i), K(partition_schema));
      } else {
        ObSubPartition **subpart_array = partition->get_subpart_array();
        int64_t subpart_num = partition->get_sub_part_num();
        int64_t subpartition_num = partition->get_subpartition_num();
        if (subpart_num != subpartition_num) {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("subpartition num not match", K(ret), K(subpart_num), K(subpartition_num));
        } else if (OB_ISNULL(subpart_array) || subpartition_num <= 0) {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("subpartition array is empty", K(ret), K(i), K(partition_schema));
        } else if (partition_schema.is_range_subpart()) {
          lib::ob_sort(subpart_array, subpart_array + subpartition_num, ObBasePartition::less_than);
          lib::ob_sort(partition->get_hidden_subpart_array(),
                    partition->get_hidden_subpart_array() + partition->get_hidden_subpartition_num(),
                    ObBasePartition::less_than);
        } else if (partition_schema.is_hash_like_subpart()) {
          lib::ob_sort(subpart_array, subpart_array + subpartition_num,
                    ObSubPartition::hash_like_func_less_than);
          lib::ob_sort(partition->get_hidden_subpart_array(),
                    partition->get_hidden_subpart_array() + partition->get_hidden_subpartition_num(),
                    ObSubPartition::hash_like_func_less_than);
        } else if (partition_schema.is_list_subpart()) {
          lib::ob_sort(subpart_array, subpart_array + subpartition_num,
                    ObBasePartition::list_part_func_layout);
          lib::ob_sort(partition->get_hidden_subpart_array(),
                    partition->get_hidden_subpart_array() + partition->get_hidden_subpartition_num(),
                    ObBasePartition::list_part_func_layout);
        }
      }
    }

    // sort def_subpartition_array
    if (OB_SUCC(ret) && partition_schema.has_sub_part_template_def()) {
      int64_t def_subpart_num = partition_schema.get_def_sub_part_num();
      int64_t def_subpartition_num = partition_schema.get_def_subpartition_num();
      if (OB_ISNULL(partition_schema.get_def_subpart_array())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("def_subpartition array is empty", K(ret), K(partition_schema));
      } else if (def_subpart_num != def_subpartition_num) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("def_subpartition num not match", K(ret), K(def_subpart_num), K(def_subpartition_num));
      } else if (def_subpartition_num <= 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("def_subpartition array is empty", K(ret), K(partition_schema));
      } else if (partition_schema.is_range_subpart()) {
        lib::ob_sort(partition_schema.get_def_subpart_array(),
            partition_schema.get_def_subpart_array() + def_subpartition_num,
            ObBasePartition::less_than);
      } else if (partition_schema.is_hash_like_subpart()) {
        lib::ob_sort(partition_schema.get_def_subpart_array(),
            partition_schema.get_def_subpart_array() + def_subpartition_num,
            ObSubPartition::hash_like_func_less_than);
      } else if (partition_schema.is_list_subpart()) {
        lib::ob_sort(partition_schema.get_def_subpart_array(),
            partition_schema.get_def_subpart_array() + def_subpartition_num,
            ObBasePartition::list_part_func_layout);
      } else {
        //nothing
      }
    } else {
    }
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
