/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifndef OCEANBASE_OBSERVER_SQL_SCHEMA_CATALOG_H_
#define OCEANBASE_OBSERVER_SQL_SCHEMA_CATALOG_H_

#include "observer/schema/native_schema_catalog.h"

namespace oceanbase {
namespace common { class ObISQLClient; }
namespace observer {

// SQL callers adapt their explicit target and transaction here. The native
// catalog itself accepts only a Namespace, protected read view or native owner.
// The DDL owner retains the catalog until commit/rollback; this never starts a
// second transaction. Each operation preserves its existing RC read snapshot.
int with_ddl_schema_catalog(common::ObISQLClient &, int64_t deadline,
    const std::function<int(NativeSchemaCatalog &)> &);
// An old SCN must name an already protected view. A transaction reads its own
// writes through the same adapter as DDL; a proxy starts a standalone read.
int with_read_schema_catalog(common::ObISQLClient &, int64_t deadline, int64_t snapshot,
    const std::function<int(NativeSchemaCatalog &)> &);

} // namespace observer
} // namespace oceanbase
#endif
