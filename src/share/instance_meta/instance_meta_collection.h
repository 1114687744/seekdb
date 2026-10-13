/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#ifndef OCEANBASE_SHARE_INSTANCE_META_COLLECTION_H_
#define OCEANBASE_SHARE_INSTANCE_META_COLLECTION_H_

#include <cstdint>

namespace oceanbase
{
namespace share
{
namespace instance_meta
{

// Physical layout identity bindings; instance directory records use
// the independent tables declared in instance_meta_schema.h.
enum class MetaCollection : uint64_t
{
  TABLE_STORAGE_LAYOUTS = 9,
};

} // namespace instance_meta
} // namespace share
} // namespace oceanbase
#endif
