/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifndef OCEANBASE_OBSERVER_NATIVE_SCHEMA_CATALOG_H_
#define OCEANBASE_OBSERVER_NATIVE_SCHEMA_CATALOG_H_

#include <functional>
#include <memory>
#include "common/ob_range.h"
#include "common/row/ob_row.h"
#include "namespace/catalog.h"

namespace oceanbase {
namespace share { namespace schema { class ObTableSchema; } }
namespace storage { class ObAccessService; }
namespace transaction { class ObTransService; class ObTxDesc; }
namespace observer {

// One Namespace directory operation. Fixed inner-table definitions come from
// code; physical locations and inherited snapshot caps come from its protected
// directory view. No SQL connection, runtime schema cache or Namespace service
// activation is needed. The caller owns this object and its operation lifetime.
class NativeSchemaCatalog final
{
public:
  class Cursor final
  {
  public:
    Cursor();
    ~Cursor();
    int next(const common::ObNewRow *&);
    int close();
    const share::schema::ObTableSchema *schema() const;
  private:
    friend class NativeSchemaCatalog;
    struct State;
    std::unique_ptr<State> state_;
    DISALLOW_COPY_AND_ASSIGN(Cursor);
  };
  using RowVisitor = std::function<int(const share::schema::ObTableSchema &,
                                      const common::ObNewRow &, bool &stop)>;
  NativeSchemaCatalog(uint64_t namespace_id, storage::ObAccessService &,
                      transaction::ObTransService &);
  ~NativeSchemaCatalog();
  int begin_read(int64_t deadline, bool weak = false);
  // An old snapshot must already be protected by this view's owner.
  int begin_read(int64_t deadline, const ns::NamespaceCatalogViews::Handle &view);
  // Borrow the DDL's native RC owner so catalog reads observe its own writes.
  // The owner must retain this operation until after commit/rollback, then
  // close it. close only detaches; it never ends the external transaction.
  int attach(transaction::ObTxDesc &, int64_t deadline);
  int scan(uint64_t inner_table_id, const common::ObNewRange &range,
           const RowVisitor &, bool reverse = false);
  int open_scan(uint64_t inner_table_id, const common::ObNewRange &, Cursor &, bool reverse = false);
  int open_prefix(uint64_t inner_table_id, const common::ObRowkey &prefix, Cursor &, bool reverse = false);
  int get_table(uint64_t table_id, int64_t schema_version, common::ObIAllocator &,
                share::schema::ObTableSchema *&);
  int close();
  const ns::NamespaceCatalogViews::Handle &view() const;
private:
  struct State;
  int prepare_scan(uint64_t inner_table_id, Cursor &);
  int start_scan(const common::ObNewRange &, Cursor &, bool reverse);
  std::shared_ptr<State> state_;
  DISALLOW_COPY_AND_ASSIGN(NativeSchemaCatalog);
};
} // namespace observer
} // namespace oceanbase
#endif
