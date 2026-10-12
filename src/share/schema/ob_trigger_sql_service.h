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

#ifndef OCEANBASE_SRC_SHARE_SCHEMA_OB_TRIGGER_SQL_SERVICE_H_
#define OCEANBASE_SRC_SHARE_SCHEMA_OB_TRIGGER_SQL_SERVICE_H_
#include "share/schema/ob_ddl_sql_service.h"

namespace oceanbase
{
namespace common
{
class ObString;
class ObISQLClient;
}

namespace share
{
namespace schema
{
class ObTriggerInfo;
class ObTriggerSqlService : public ObDDLSqlService
{
public:
  explicit ObTriggerSqlService(ObSchemaService &schema_service)
    : ObDDLSqlService(schema_service) {}
  virtual ~ObTriggerSqlService() {}

  int create_trigger(const ObTriggerInfo &trigger_info,
                     bool is_replace,
                     common::ObISQLClient &sql_client,
                     const common::ObString *ddl_stmt_str = NULL);
  int drop_trigger(const ObTriggerInfo &trigger_info,
                   bool drop_to_recyclebin,
                   int64_t new_schema_version,
                   common::ObISQLClient &sql_client,
                   const common::ObString *ddl_stmt_str = NULL);
  int alter_trigger(const ObTriggerInfo &trigger_info,
                    int64_t new_schema_version,
                    ObISQLClient &sql_client,
                    const ObString *ddl_stmt_str);
  int restore_trigger(const ObTriggerInfo &trigger_info,
                        int64_t new_schema_version,
                        common::ObISQLClient &sql_client);
  int update_trigger_on_rename(
      const ObTriggerInfo &trigger_info,
      int64_t new_schema_version,
      common::ObISQLClient &sql_client,
      ObSchemaOperationType op_type = OB_DDL_ALTER_TRIGGER);
private:
  int log_trigger_operation(const ObTriggerInfo &trigger_info,
                            int64_t new_schema_version,
                            ObSchemaOperationType op_type,
                            const common::ObString *ddl_stmt_str,
                            common::ObISQLClient &sql_client);
  DISALLOW_COPY_AND_ASSIGN(ObTriggerSqlService);
};
} //end of schema
} //end of share
} //end of oceanbase
#endif /* OCEANBASE_SRC_SHARE_SCHEMA_OB_TRIGGER_SQL_SERVICE_H_ */
