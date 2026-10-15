// Copyright (c) 2026 OceanBase.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lib/thread/ob_simple_thread_pool.h"

namespace oceanbase
{
namespace share
{
// Shared, adaptive workers for one bounded batch of distinct vector IDs.
// add() joins every submitted task before returning; input buffers and the
// graph remain owned and protected by the caller throughout the call.
class VectorDeltaBuildPool final : public common::ObLinkQueueThreadPool
{
public:
  VectorDeltaBuildPool() : parallelism_(0) {}
  ~VectorDeltaBuildPool() { stop(); wait(); }
  int init(int64_t max_workers, lib::IRunWrapper *wrapper);
  int add(void *index, float *vectors, int64_t *vids, int64_t dim,
          char *extra_info, int64_t extra_info_size, int64_t count);

private:
  struct Batch;
  struct Task;
  void handle(common::LinkTask *task) override;
  void handle_drop(common::LinkTask *task) override;
  static void finish(Task &task, int result);
  int64_t parallelism_;
};
}
}
