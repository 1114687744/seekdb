/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "storage/physical_tablet_access.h"
#include "storage/tablet/ob_tablet.h"
#include "storage/tablet/ob_tablet_create_delete_helper.h"
#include "lib/worker.h"
#include <chrono>
#include <shared_mutex>
#include <unordered_map>

namespace oceanbase {
namespace storage {
using namespace common;
namespace {
std::shared_timed_mutex publication_mutex;
thread_local int publication_depth = 0;
std::mutex reclamation_mutex;
std::mutex access_mutex;
std::unordered_map<uint64_t, int64_t> accesses;
}

PhysicalPublicationGuard::PhysicalPublicationGuard() : held_(false), ret_(OB_SUCCESS)
{
  if (publication_depth == 0) {
    while (!publication_mutex.try_lock_shared_for(std::chrono::milliseconds(1))) {
      if (OB_SUCCESS != (ret_ = THIS_WORKER.check_status())) { return; }
    }
  }
  ++publication_depth;
  held_ = true;
}
PhysicalPublicationGuard::~PhysicalPublicationGuard()
{
  if (held_ && --publication_depth == 0) { publication_mutex.unlock_shared(); }
}
bool PhysicalPublicationGuard::is_held() { return publication_depth != 0; }

PhysicalReclamationGuard::PhysicalReclamationGuard()
    : serial_(reclamation_mutex, std::try_to_lock), held_(false), ret_(OB_EAGAIN)
{
  if (!serial_.owns_lock() || publication_depth != 0) { return; }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (!publication_mutex.try_lock_for(std::chrono::milliseconds(1))) {
    if (std::chrono::steady_clock::now() >= deadline) { return; }
  }
  held_ = true;
  ++publication_depth;
  ret_ = OB_SUCCESS;
}
PhysicalReclamationGuard::~PhysicalReclamationGuard() { release_publication(); }
void PhysicalReclamationGuard::release_publication()
{
  if (held_) {
    --publication_depth;
    publication_mutex.unlock();
    held_ = false;
  }
}

PhysicalTabletAccess::~PhysicalTabletAccess() { reset(); }
int PhysicalTabletAccess::acquire(const ObTabletID &tablet)
{
  if (!PhysicalPublicationGuard::is_held() || !tablet.is_valid()) { return OB_INVALID_ARGUMENT; }
  ObTabletID current = tablet;
  ObSEArray<uint64_t, 8> chain;
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && current.is_valid()) {
    if (has_exist_in_array(chain, current.id())) { return OB_STATE_NOT_MATCH; }
    if (chain.count() >= 64) { return OB_SIZE_OVERFLOW; }
    if (OB_FAIL(chain.push_back(current.id()))) { break; }
    if (tablets_.insert(current.id()).second) {
      std::lock_guard<std::mutex> lock(access_mutex);
      ++accesses[current.id()];
    }
    ObTabletHandle handle;
    ret = ObTabletCreateDeleteHelper::check_and_get_tablet(ObTabletMapKey(current),
        handle, 0, ObMDSGetTabletMode::READ_WITHOUT_CHECK,
        transaction::ObTransVersion::MAX_TRANS_VERSION);
    if (ret == OB_TABLET_NOT_EXIST || ret == OB_ENTRY_NOT_EXIST) { return OB_SUCCESS; }
    if (OB_FAIL(ret) || handle.get_obj()->is_empty_shell()) { break; }
    const auto &fork = handle.get_obj()->get_tablet_meta().fork_info_;
    if (!fork.is_valid() || fork.is_complete()) { break; }
    current = fork.get_fork_src_tablet_id();
  }
  return ret;
}
void PhysicalTabletAccess::reset()
{
  if (tablets_.empty()) { return; }
  std::lock_guard<std::mutex> lock(access_mutex);
  for (uint64_t id : tablets_) {
    auto found = accesses.find(id);
    OB_ASSERT(found != accesses.end() && found->second > 0);
    if (--found->second == 0) { accesses.erase(found); }
  }
  tablets_.clear();
}
int PhysicalTabletAccess::exclude_active(ObIArray<ObTabletID> &candidates, bool &need_retry)
{
  ObArray<ObTabletID> idle;
  int ret = OB_SUCCESS;
  std::lock_guard<std::mutex> lock(access_mutex);
  for (int64_t i = 0; OB_SUCC(ret) && i < candidates.count(); ++i) {
    if (accesses.count(candidates.at(i).id()) != 0) { need_retry = true; }
    else { ret = idle.push_back(candidates.at(i)); }
  }
  return ret ? ret : candidates.assign(idle);
}
} // namespace storage
} // namespace oceanbase
