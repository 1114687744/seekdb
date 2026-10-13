/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "observer/schema/sql_schema_catalog.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "namespace/namespace.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/tx_storage/ob_access_service.h"

namespace oceanbase {
namespace observer {
using namespace common;

int with_ddl_schema_catalog(ObISQLClient &client, int64_t deadline,
    const std::function<int(NativeSchemaCatalog &)> &operation)
{
  auto *owner = dynamic_cast<ObMySQLTransaction *>(&client);
  if (owner == nullptr || !owner->is_started() || !operation) { return OB_INVALID_ARGUMENT; }
  auto *access = share::server_service<storage::ObAccessService>();
  auto *transactions = share::server_service<transaction::ObTransService>();
  if (access == nullptr || transactions == nullptr) { return OB_NOT_INIT; }
  std::shared_ptr<NativeSchemaCatalog> catalog(new (std::nothrow)
      NativeSchemaCatalog(owner->target_namespace(), *access, *transactions));
  if (!catalog) { return OB_ALLOCATE_MEMORY_FAILED; }
  int ret = owner->retain_until_end(catalog);
  if (OB_SUCC(ret)) {
    ret = query::ObInnerSQLConnectionAccess::with_native_transaction(owner->get_connection(),
        [&](transaction::ObTxDesc &native) {
      const int rc = catalog->attach(native, deadline);
      return rc == OB_SUCCESS ? operation(*catalog) : rc;
    });
  }
  return ret;
}

int with_read_schema_catalog(ObISQLClient &client, int64_t deadline, int64_t snapshot,
    const std::function<int(NativeSchemaCatalog &)> &operation)
{
  if (!operation) { return OB_INVALID_ARGUMENT; }
  if (dynamic_cast<ObMySQLTransaction *>(&client) != nullptr) {
    if (snapshot > 0) { return OB_INVALID_ARGUMENT; }
    return observer::with_ddl_schema_catalog(client, deadline, operation);
  }
  auto *access = share::server_service<storage::ObAccessService>();
  auto *transactions = share::server_service<transaction::ObTransService>();
  if (access == nullptr || transactions == nullptr) { return OB_NOT_INIT; }
  observer::NativeSchemaCatalog catalog(client.target_namespace(), *access, *transactions);
  int ret = OB_SUCCESS;
  if (snapshot > 0) {
    const auto view = ns::namespace_registry().catalog_views().find(
        client.target_namespace(), snapshot);
    ret = view ? catalog.begin_read(deadline, view) : OB_SNAPSHOT_DISCARDED;
  } else {
    ret = catalog.begin_read(deadline, !share::server_is_write_enabled());
  }
  if (OB_SUCC(ret)) { ret = operation(catalog); }
  const int cleanup = catalog.close();
  return ret == OB_SUCCESS ? cleanup : ret;
}

} // namespace observer
} // namespace oceanbase
