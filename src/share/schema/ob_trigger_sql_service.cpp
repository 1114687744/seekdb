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
#include "ob_trigger_sql_service.h"
#include "observer/schema/native_schema_catalog.h"
#include "share/schema/ob_trigger_info.h"

namespace oceanbase
{
using namespace common;
namespace share
{
namespace schema
{
using observer::NativeSchemaCatalog;

int ObTriggerSqlService::create_trigger(const ObTriggerInfo &trigger_info,
                                        bool is_replace,
                                        ObISQLClient &sql_client,
                                        const ObString *ddl_stmt_str)
{
  int ret = NativeSchemaCatalog::with_ddl_transaction(sql_client, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
        return catalog.write_trigger(trigger_info, trigger_info.get_schema_version(),
            is_replace ? NativeSchemaCatalog::TriggerWrite::UPDATE : NativeSchemaCatalog::TriggerWrite::INSERT);
      });
  if (OB_SUCC(ret)) {
    ret = log_trigger_operation(trigger_info, trigger_info.get_schema_version(),
        OB_DDL_CREATE_TRIGGER, ddl_stmt_str, sql_client);
  }
  return ret;
}

int ObTriggerSqlService::drop_trigger(const ObTriggerInfo &trigger_info,
                                      bool drop_to_recyclebin,
                                      int64_t new_schema_version,
                                      ObISQLClient &sql_client,
                                      const ObString *ddl_stmt_str)
{
  int ret = NativeSchemaCatalog::with_ddl_transaction(sql_client, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
        return catalog.write_trigger(trigger_info, new_schema_version,
            drop_to_recyclebin ? NativeSchemaCatalog::TriggerWrite::UPDATE : NativeSchemaCatalog::TriggerWrite::ERASE);
      });
  if (OB_SUCC(ret)) {
    ret = log_trigger_operation(trigger_info, new_schema_version,
        drop_to_recyclebin ? OB_DDL_DROP_TRIGGER_TO_RECYCLEBIN : OB_DDL_DROP_TRIGGER, ddl_stmt_str, sql_client);
  }
  return ret;
}

int ObTriggerSqlService::alter_trigger(const ObTriggerInfo &trigger_info,
                                       int64_t new_schema_version,
                                       ObISQLClient &sql_client,
                                       const ObString *ddl_stmt_str)
{
  int ret = NativeSchemaCatalog::with_ddl_transaction(sql_client, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
        return catalog.write_trigger(trigger_info, new_schema_version, NativeSchemaCatalog::TriggerWrite::UPDATE);
      });
  if (OB_SUCC(ret)) {
    ret = log_trigger_operation(trigger_info, new_schema_version,
        OB_DDL_ALTER_TRIGGER, ddl_stmt_str, sql_client);
  }
  return ret;
}

int ObTriggerSqlService::restore_trigger(const ObTriggerInfo &trigger_info,
                                           int64_t new_schema_version,
                                           ObISQLClient &sql_client)
{
  int ret = NativeSchemaCatalog::with_ddl_transaction(sql_client, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
        return catalog.write_trigger(trigger_info, new_schema_version, NativeSchemaCatalog::TriggerWrite::UPDATE);
      });
  if (OB_SUCC(ret)) {
    ret = log_trigger_operation(trigger_info, new_schema_version,
        OB_DDL_RESTORE_TRIGGER_FROM_RECYCLEBIN, nullptr, sql_client);
  }
  return ret;
}

int ObTriggerSqlService::update_trigger_on_rename(
    const ObTriggerInfo &trigger_info,
    int64_t new_schema_version,
    ObISQLClient &sql_client,
    ObSchemaOperationType op_type)
{
  int ret = NativeSchemaCatalog::with_ddl_transaction(sql_client, THIS_WORKER.get_timeout_ts(),
      [&](NativeSchemaCatalog &catalog) {
        return catalog.write_trigger(trigger_info, new_schema_version, NativeSchemaCatalog::TriggerWrite::UPDATE);
      });
  if (OB_SUCC(ret)) {
    ret = log_trigger_operation(trigger_info, new_schema_version,
        op_type, nullptr, sql_client);
  }
  return ret;
}

int ObTriggerSqlService::log_trigger_operation(const ObTriggerInfo &trigger_info,
                                               int64_t new_schema_version,
                                               ObSchemaOperationType op_type,
                                               const ObString *ddl_stmt_str,
                                               ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  ObSchemaOperation opt;
  OX (opt.database_id_ = trigger_info.get_database_id());
  OX (opt.table_id_ = trigger_info.get_trigger_id());
  OX (opt.schema_version_ = new_schema_version);
  OX (opt.op_type_ = op_type);
  OX (opt.ddl_stmt_str_ = (NULL != ddl_stmt_str) ? *ddl_stmt_str : ObString());
  OZ (log_operation(opt, sql_client));
  return ret;
}

}  // namespace schema
}  // namespace share
}  // namespace oceanbase
