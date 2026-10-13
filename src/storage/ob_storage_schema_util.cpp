/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE

#include "ob_storage_schema_util.h"

namespace oceanbase
{

using namespace common;
using namespace share::schema;

namespace storage
{

int ObStorageSchemaUtil::alloc_storage_schema(
    common::ObIAllocator &allocator,
    ObStorageSchema *&new_storage_schema)
{
  int ret = OB_SUCCESS;
  void *buffer = allocator.alloc(sizeof(ObStorageSchema));

  if (OB_ISNULL(buffer)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
    STORAGE_LOG(WARN, "fail to allocate mem for storage schema", K(ret));
  } else {
    new_storage_schema = new (buffer) ObStorageSchema();
  }
  return ret;
}

void ObStorageSchemaUtil::free_storage_schema(
    common::ObIAllocator &allocator,
    ObStorageSchema *&storage_schema)
{
  if (NULL != storage_schema) {
    storage_schema->~ObStorageSchema();
    allocator.free(storage_schema);
    storage_schema = nullptr;
  }
}

} // namespace storage
} // namespace oceanbase
