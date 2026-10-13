/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifndef SEEKDB_STORAGE_PHYSICAL_TABLET_ACCESS_H_
#define SEEKDB_STORAGE_PHYSICAL_TABLET_ACCESS_H_

#include <mutex>
#include <unordered_set>
#include "common/ob_tablet_id.h"
#include "lib/container/ob_iarray.h"

namespace oceanbase {
namespace storage {

// Short publication/admission fence. Nested acquisitions on the same thread
// share one reader lock; no Namespace ownership or SQL service is involved.
class PhysicalPublicationGuard final {
public:
  PhysicalPublicationGuard();
  ~PhysicalPublicationGuard();
  int error() const { return ret_; }
  static bool is_held();
private:
  bool held_;
  int ret_;
  DISALLOW_COPY_AND_ASSIGN(PhysicalPublicationGuard);
};

// Serializes retention collection, physical reclamation and source-file
// capture. Publication is excluded only until release_publication().
class PhysicalReclamationGuard final {
public:
  PhysicalReclamationGuard();
  ~PhysicalReclamationGuard();
  int error() const { return ret_; }
  void release_publication();
private:
  std::unique_lock<std::mutex> serial_;
  bool held_;
  int ret_;
  DISALLOW_COPY_AND_ASSIGN(PhysicalReclamationGuard);
};

// Operation-scoped physical references. Reclamation cannot remove any tablet
// in the protected source chain. Snapshot data must additionally be protected
// by the caller's read view, or by captured SSTable handles for a copy task.
class PhysicalTabletAccess final {
public:
  PhysicalTabletAccess() = default;
  ~PhysicalTabletAccess();
  // Call under a publication or reclamation guard to close the lookup/GC gap.
  // A not-yet-created object may be protected before transactional creation.
  int acquire(const common::ObTabletID &tablet);
  void reset();
  static int exclude_active(common::ObIArray<common::ObTabletID> &candidates, bool &need_retry);
private:
  std::unordered_set<uint64_t> tablets_;
  DISALLOW_COPY_AND_ASSIGN(PhysicalTabletAccess);
};

} // namespace storage
} // namespace oceanbase
#endif
