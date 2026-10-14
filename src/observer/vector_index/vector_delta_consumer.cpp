// Copyright (c) 2026 OceanBase.
// SPDX-License-Identifier: Apache-2.0
#define USING_LOG_PREFIX SERVER
#include "observer/vector_index/vector_delta_consumer.h"
#include "observer/vector_index/ob_plugin_vector_index_service.h"
#include "observer/vector_index/ob_plugin_vector_index_utils.h"
#include "observer/vector_index/ob_vector_index_util.h"
#include "observer/ob_inner_sql_connection.h"
#include "query/session/ob_session_access.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/rc/ob_server_runtime.h"
#include "share/vector/ob_vector_index_mode.h"
#include "storage/tablelock/ob_lock_inner_connection_util.h"
#include "sql/ob_sql_utils.h"
#include "lib/thread/ob_thread_name.h"
#include "data_plane/transaction/ob_i_transaction_service.h"

namespace oceanbase
{
namespace share
{
using namespace common;
using namespace schema;

VectorDeltaConsumer::VectorDeltaConsumer()
  : ObThreadPool(1), schemas_(nullptr), sql_proxy_(nullptr), vectors_(nullptr),
    runtime_(nullptr), transactions_(nullptr), completed_scn_(0)
{}

int VectorDeltaConsumer::init(ObMultiVersionSchemaService &schemas,
                             ObMySQLProxy &sql_proxy,
                             ObPluginVectorIndexService &vectors,
                             ObServerRuntimeState &runtime,
                             data_plane::ObITransactionService &transactions)
{
  schemas_ = &schemas;
  sql_proxy_ = &sql_proxy;
  vectors_ = &vectors;
  runtime_ = &runtime;
  transactions_ = &transactions;
  set_run_wrapper(&runtime);
  return ObThreadPool::init();
}

void VectorDeltaConsumer::run1()
{
  lib::set_thread_name("VecDelta");
  while (!has_set_stop()) {
    const int ret = consume_round();
    if (ret != OB_SUCCESS && REACH_TIME_INTERVAL(5 * 1000 * 1000)) {
      LOG_WARN("transactional vector delta round deferred", KR(ret));
    }
    ob_usleep(100 * 1000);
  }
}

int VectorDeltaConsumer::consume_round()
{
  int ret = OB_SUCCESS;
  SCN snapshot;
  ObSchemaGetterGuard guard;
  ObSEArray<const ObSimpleTableSchemaV2 *, 64> tables;
  // Primary obtains a fresh committed snapshot. Standby uses its replay-safe
  // readable snapshot and only rebuilds local graph state.
  const bool writable = runtime_->is_write_enabled();
  if (OB_FAIL(writable
          ? transactions_->get_read_snapshot_version(
                ObTimeUtility::current_time() + 1000000, snapshot)
          : ObPluginVectorIndexUtils::get_task_read_snapshot(snapshot))) {
  } else if (!snapshot.is_valid_and_not_min()) {
    ret = OB_EAGAIN;
  } else if (OB_FAIL(schemas_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schemas_in_runtime(tables))) {
  } else {
    int round_ret = OB_SUCCESS;
    for (int64_t i = 0; i < tables.count() && !has_set_stop(); ++i) {
      const ObSimpleTableSchemaV2 *simple = tables.at(i);
      if (simple->is_vec_delta_buffer_type() && !simple->is_in_recyclebin()
          && simple->get_index_status() == INDEX_STATUS_AVAILABLE) {
        const ObTableSchema *table = nullptr;
        int one_ret = guard.get_table_schema(simple->get_table_id(), table);
        if (one_ret == OB_SUCCESS && table != nullptr
            && is_vector_index_sync_mode_async(table->get_index_params())) {
          one_ret = consume_index(guard, *table, snapshot, writable);
        }
        if (one_ret != OB_SUCCESS) {
          round_ret = one_ret;
        }
      }
    }
    ret = round_ret;
    if (OB_SUCC(ret) && !has_set_stop()) {
      ATOMIC_STORE(&completed_scn_, snapshot.get_val_for_sql());
    }
  }
  return ret;
}

int VectorDeltaConsumer::consume_index(ObSchemaGetterGuard &guard,
                                      const ObTableSchema &delta,
                                      const SCN &snapshot, const bool writable)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObTabletID, 8> tablets;
  const ObDatabaseSchema *database = nullptr;
  const ObTableSchema *index_id = nullptr;
  int64_t dim = 0;
  ObString params = delta.get_index_params();
  if (OB_FAIL(delta.get_tablet_ids(tablets))) {
  } else if (OB_FAIL(guard.get_database_schema(delta.get_database_id(), database))) {
  } else if (OB_ISNULL(database)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(ObVectorIndexUtil::get_vector_index_column_dim(delta, dim))) {
  }
  // Construct the graph before publishing moved rows to async queries. If the
  // move rolls back, extra graph points are invisible through the MVCC bitmap.
  for (int64_t i = 0; OB_SUCC(ret) && i < tablets.count(); ++i) {
    ObPluginVectorIndexAdapterGuard adapter_guard;
    if (OB_FAIL(vectors_->acquire_adapter_guard(tablets.at(i),
            INDEX_TYPE_VEC_DELTA_BUFFER_LOCAL, adapter_guard, &params, dim))) {
    } else {
      ObPluginVectorIndexAdaptor *adapter = adapter_guard.get_adatper();
      if (adapter->get_create_type() != CreateTypeComplete) {
        ret = OB_EAGAIN;
      } else if (OB_FAIL(guard.get_table_schema(adapter->get_vbitmap_table_id(), index_id))) {
      } else if (OB_ISNULL(index_id)) {
        ret = OB_TABLE_NOT_EXIST;
      } else {
        ObArenaAllocator allocator("VecDelta");
        common::ObILobReadService *lob = server_service<common::ObILobReadService>();
        if (OB_ISNULL(lob)) {
          ret = OB_NOT_INIT;
        } else {
          RWLock::RLockGuard query_guard(adapter->get_query_lock());
          common::ObSpinLockGuard reload_guard(adapter->get_reload_lock());
          const common::ObLobReadOptions options(*lob);
          if (OB_FAIL(ObPluginVectorIndexUtils::refresh_adp_from_table(
                  adapter, false, snapshot, allocator, options))) {
          } else {
            adapter->set_reload_finish(true);
            // Disable the stale empty-index shortcut before the transfer. The
            // bitmap progress still only advances over committed index_id rows.
            SCN dirty_scn = snapshot;
            adapter->update_index_id_dml_scn(dirty_scn);
            adapter->update_can_skip(NOT_SKIP);
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && writable && OB_NOT_NULL(index_id)) {
    ret = transfer_delta(delta, *index_id, database->get_database_name_str(), snapshot);
  }
  return ret;
}

int VectorDeltaConsumer::transfer_delta(const ObTableSchema &delta,
                                       const ObTableSchema &index_id,
                                       const ObString &database, const SCN &snapshot)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("VecDeltaSQL");
  ObString db_name, delta_name, id_name;
  ObSqlString source_columns, destination_columns;
  ObMySQLTransaction trans;
  using namespace transaction::tablelock;
  if (OB_FAIL(sql::ObSQLUtils::generate_new_name_with_escape_character(allocator, database, db_name))) {
  } else if (OB_FAIL(sql::ObSQLUtils::generate_new_name_with_escape_character(
          allocator, delta.get_table_name_str(), delta_name))) {
  } else if (OB_FAIL(sql::ObSQLUtils::generate_new_name_with_escape_character(
          allocator, index_id.get_table_name_str(), id_name))) {
  }
  for (auto it = index_id.column_begin(); OB_SUCC(ret) && it != index_id.column_end(); ++it) {
    if ((*it)->get_column_name_str().prefix_match(OB_VEC_SCN_COLUMN_NAME_PREFIX)) {
      ObString escaped;
      if (OB_FAIL(sql::ObSQLUtils::generate_new_name_with_escape_character(
              allocator, (*it)->get_column_name_str(), escaped))) {
      } else if (OB_FAIL(destination_columns.append_fmt("`%.*s`", escaped.length(), escaped.ptr()))) {
      }
    }
  }
  for (auto it = delta.column_begin(); OB_SUCC(ret) && it != delta.column_end(); ++it) {
    const ObColumnSchemaV2 *column = *it;
    if (!column->get_column_name_str().prefix_match(OB_VEC_VECTOR_COLUMN_NAME_PREFIX)
        && !column->get_column_name_str().prefix_match(OB_VEC_SCN_COLUMN_NAME_PREFIX)) {
      ObString escaped_source, escaped_dest;
      const ObColumnSchemaV2 *dest = index_id.get_column_schema(column->get_column_id());
      if (OB_ISNULL(dest)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(sql::ObSQLUtils::generate_new_name_with_escape_character(
              allocator, column->get_column_name_str(), escaped_source))) {
      } else if (OB_FAIL(sql::ObSQLUtils::generate_new_name_with_escape_character(
              allocator, dest->get_column_name_str(), escaped_dest))) {
      } else if (OB_FAIL(source_columns.append_fmt(",`%.*s`", escaped_source.length(), escaped_source.ptr()))) {
      } else if (OB_FAIL(destination_columns.append_fmt(",`%.*s`", escaped_dest.length(), escaped_dest.ptr()))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObLockObjRequest lock;
    lock.obj_type_ = ObLockOBJType::OBJ_TYPE_REFRESH_VECTOR_INDEX;
    lock.obj_id_ = delta.get_table_id();
    lock.owner_id_ = ObTableLockOwnerID::default_owner();
    lock.lock_mode_ = EXCLUSIVE;
    lock.op_type_ = ObTableLockOpType::IN_TRANS_COMMON_LOCK;
    lock.timeout_us_ = 0;
    if (OB_FAIL(trans.start(sql_proxy_))) {
    } else if (OB_FAIL(ObInnerConnectionLockUtil::lock_obj(lock, trans.get_connection()))) {
    } else {
      auto &session = static_cast<observer::ObInnerSQLConnection *>(trans.get_connection())->get_session();
      query::ObSessionAccess::set_dummy_ddl_visibility(&session, true);
      ObSqlString insert, erase;
      int64_t inserted = 0, erased = 0;
      if (OB_FAIL(insert.append_fmt(
          "INSERT INTO `%.*s`.`%.*s` (%s) SELECT ora_rowscn%s FROM `%.*s`.`%.*s` WHERE ora_rowscn <= %lu",
          db_name.length(), db_name.ptr(), id_name.length(), id_name.ptr(),
          destination_columns.ptr(), source_columns.ptr(), db_name.length(), db_name.ptr(),
          delta_name.length(), delta_name.ptr(), snapshot.get_val_for_sql()))) {
      } else if (OB_FAIL(trans.write(insert.ptr(), inserted))) {
      } else if (inserted > 0) {
        if (OB_FAIL(erase.append_fmt("DELETE FROM `%.*s`.`%.*s` WHERE ora_rowscn <= %lu",
            db_name.length(), db_name.ptr(), delta_name.length(), delta_name.ptr(), snapshot.get_val_for_sql()))) {
        } else if (OB_FAIL(trans.write(erase.ptr(), erased))) {
        } else if (inserted != erased) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
      if (inserted > 0 || OB_FAIL(ret)) {
        LOG_INFO("transactional vector delta transfer", KR(ret), K(inserted), K(erased), K(snapshot));
      }
      query::ObSessionAccess::set_dummy_ddl_visibility(&session, false);
    }
  }
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}
}
}
