// Copyright (c) 2026 OceanBase.
// SPDX-License-Identifier: Apache-2.0
#define USING_LOG_PREFIX SHARE
#include "observer/vector_index/vector_delta_build_pool.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/lock/ob_thread_cond.h"
#include "lib/vector/ob_vector_util.h"

namespace oceanbase
{
namespace share
{
using namespace common;

struct VectorDeltaBuildPool::Batch
{
  ObThreadCond done_;
  int64_t remaining_ = 0;
  int result_ = OB_SUCCESS;
};

struct VectorDeltaBuildPool::Task : public LinkTask
{
  Batch *batch_;
  void *index_;
  float *vectors_;
  int64_t *vids_;
  int64_t dim_;
  char *extra_info_;
  int64_t count_;
};

int VectorDeltaBuildPool::init(int64_t max_workers, lib::IRunWrapper *wrapper)
{
  int ret = OB_SUCCESS;
  set_run_wrapper(wrapper);
  if (OB_FAIL(ObLinkQueueThreadPool::init(max_workers, max_workers + 1, "VecBuild"))) {
  } else {
    parallelism_ = max_workers;
  }
  return ret;
}

int VectorDeltaBuildPool::add(void *index, float *vectors, int64_t *vids, int64_t dim,
                              char *extra_info, int64_t extra_info_size, int64_t count)
{
  int ret = OB_SUCCESS;
  Batch batch;
  ObArenaAllocator allocator("VecBuildTask");
  Task *tasks = nullptr;
  // Avoid dispatching tiny tasks, while allowing one index to use the pool.
  const int64_t task_count = min(parallelism_, (count + 63) / 64);
  if (task_count <= 0 || OB_ISNULL(index) || OB_ISNULL(vectors) || OB_ISNULL(vids)
      || dim <= 0 || extra_info_size < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(batch.done_.init(ObWaitEventIds::THREAD_IDLING_COND_WAIT))) {
  } else if (OB_ISNULL(tasks = static_cast<Task *>(allocator.alloc(sizeof(Task) * task_count)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    batch.remaining_ = task_count;
    for (int64_t i = 0; i < task_count; ++i) {
      Task &task = *new (&tasks[i]) Task();
      const int64_t begin = count * i / task_count;
      const int64_t end = count * (i + 1) / task_count;
      task.batch_ = &batch;
      task.index_ = index;
      task.vectors_ = vectors + begin * dim;
      task.vids_ = vids + begin;
      task.dim_ = dim;
      task.extra_info_ = extra_info == nullptr ? nullptr : extra_info + begin * extra_info_size;
      task.count_ = end - begin;
      const int push_ret = push(&task);
      if (push_ret != OB_SUCCESS) {
        finish(task, push_ret);
      }
    }
    {
      ObThreadCondGuard guard(batch.done_);
      // Even on failure, join outstanding tasks before releasing their buffers.
      while (batch.remaining_ > 0) {
        (void)batch.done_.wait(1000);
      }
      ret = batch.result_;
    }
    for (int64_t i = 0; i < task_count; ++i) {
      tasks[i].~Task();
    }
  }
  return ret;
}

void VectorDeltaBuildPool::handle(LinkTask *task)
{
  Task &build = *static_cast<Task *>(task);
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr("VIndexVsagADP"));
  lib::ObLightBacktraceGuard light_backtrace_guard(false);
  const int ret = obvectorutil::add_index(build.index_, build.vectors_, build.vids_,
                                         build.dim_, build.extra_info_, build.count_);
  finish(build, ret);
}

void VectorDeltaBuildPool::handle_drop(LinkTask *task)
{
  finish(*static_cast<Task *>(task), OB_IN_STOP_STATE);
}

void VectorDeltaBuildPool::finish(Task &task, int result)
{
  Batch &batch = *task.batch_;
  ObThreadCondGuard guard(batch.done_);
  if (batch.result_ == OB_SUCCESS) {
    batch.result_ = result;
  }
  if (--batch.remaining_ == 0) {
    batch.done_.signal();
  }
}
}
}
