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
#include "ob_ddl_sql_service.h"
#include "share/ob_server_struct.h"
#include "observer/namespace_worker_protocol_prototype.h"
#include "observer/schema/native_schema_catalog.h"
#include "share/schema/schema_catalog_fields.h"

namespace oceanbase
{
using namespace common;
namespace share
{
namespace schema
{
int ObDDLSqlService::log_operation(ObSchemaOperation &operation, ObISQLClient &client)
{
  ObSEArray<ObSchemaOperation, 1> operations;
  int ret = operations.push_back(operation);
  if (OB_SUCC(ret)) { ret = log_operations(operations, client); }
  return ret;
}

int ObDDLSqlService::log_operations(const ObIArray<ObSchemaOperation> &operations, ObISQLClient &client)
{
  if (operations.empty()) { return OB_SUCCESS; }
  auto *lifecycle = observer::namespace_worker_prototype::namespace_schema_lifecycle(client.target_namespace());
  auto *last = GET_TSI(TSILastOper);
  if (lifecycle == nullptr) { return OB_NOT_INIT; }
  if (last == nullptr) { return OB_ERR_UNEXPECTED; }
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < operations.count(); ++i) {
    if (!operations.at(i).is_valid()) { ret = OB_INVALID_ARGUMENT; }
    else { last->last_operation_schema_version_ = operations.at(i).schema_version_; }
  }
  if (OB_FAIL(ret)) {
  } else if (!lifecycle->is_bootstrapping()) {
    ret = observer::NativeSchemaCatalog::with_ddl_transaction(client, THIS_WORKER.get_timeout_ts(),
        [&](observer::NativeSchemaCatalog &catalog) {
      int ret = OB_SUCCESS;
      for (int64_t i = 0; OB_SUCC(ret) && i < operations.count(); ++i) {
        ret = catalog.insert_schema_operation(operations.at(i));
      }
      return ret;
    });
  } else {
    // The directory is not yet available during initial bootstrap.
    ObDMLSqlSplicer rows;
    ObSqlString sql;
    int64_t affected = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < operations.count(); ++i) {
      if (OB_FAIL(SchemaCatalogFields::operation(operations.at(i), rows))) {
      } else { ret = rows.finish_row(); }
    }
    if (OB_SUCC(ret)) { ret = rows.splice_batch_insert_sql(OB_ALL_DDL_OPERATION_TNAME, sql); }
    if (OB_SUCC(ret)) { ret = client.write(sql.ptr(), affected); }
    if (OB_SUCC(ret) && affected != operations.count()) { ret = OB_ERR_UNEXPECTED; }
  }
  return ret;
}

int ObDDLSqlService::log_nop_operation(const ObSchemaOperation &schema_operation,
                                       const int64_t new_schema_version,
                                       const common::ObString &ddl_sql_str,
                                       common::ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  ObSchemaOperation ddl_schema_op;
  ddl_schema_op = schema_operation;
  ddl_schema_op.ddl_stmt_str_ = ddl_sql_str;

  if (OB_INVALID_VERSION == new_schema_version) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to get schema version", K(ret), K(new_schema_version), K(ddl_schema_op));
  } else {
    ddl_schema_op.schema_version_ = new_schema_version;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_INVALID_VERSION == ddl_schema_op.schema_version_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid schema version", K(ret), K(ddl_schema_op));
  } else if (OB_FAIL(log_operation(ddl_schema_op, sql_client))) {
  }
  return ret;
}

} //end of schema
} //end of share
} //end of oceanbase
