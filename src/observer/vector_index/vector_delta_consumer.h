// Copyright (c) 2026 OceanBase.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "share/ob_thread_pool.h"
#include "share/scn.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace common { class ObMySQLProxy; }
namespace data_plane { class ObITransactionService; }
namespace share
{
class ObServerRuntimeState;
class ObPluginVectorIndexService;
class ObPluginVectorIndexAdaptor;
namespace schema
{
class ObMultiVersionSchemaService;
class ObSchemaGetterGuard;
class ObTableSchema;
}

// Experimental replacement for redo-driven HNSW maintenance. Durable delta
// rows are the work queue. A lost wakeup or process restart needs no queue repair.
class VectorDeltaConsumer final : public ObThreadPool
{
public:
  VectorDeltaConsumer();
  int init(schema::ObMultiVersionSchemaService &schemas,
           common::ObMySQLProxy &sql_proxy,
           ObPluginVectorIndexService &vectors,
           ObServerRuntimeState &runtime,
           data_plane::ObITransactionService &transactions);
  void run1() override;
  uint64_t completed_scn() const { return ATOMIC_LOAD(&completed_scn_); }

private:
  int consume_round();
  int consume_index(schema::ObSchemaGetterGuard &guard,
                    const schema::ObTableSchema &delta,
                    const SCN &snapshot, bool writable);
  int transfer_delta(const schema::ObTableSchema &delta,
                     const schema::ObTableSchema &index_id,
                     const common::ObString &database, const SCN &snapshot);
  schema::ObMultiVersionSchemaService *schemas_;
  common::ObMySQLProxy *sql_proxy_;
  ObPluginVectorIndexService *vectors_;
  ObServerRuntimeState *runtime_;
  data_plane::ObITransactionService *transactions_;
  uint64_t completed_scn_;
};
}
}
