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

#ifndef OCEANBASE_OB_SCHEMA_RUNTIME_SERVICE_H
#define OCEANBASE_OB_SCHEMA_RUNTIME_SERVICE_H

#include <atomic>
#include <cstdint>

namespace oceanbase
{
namespace common { class ObIAllocator; }
namespace storage { class ObTablet; class ObStorageSchema; }
namespace share
{
class SCN;
namespace schema
{
class ObMultiVersionSchemaService;
class ObSchemaRuntimeService
{
public:
  using TabletSchemaResolver = int (*)(uint64_t, ObMultiVersionSchemaService *&, uint64_t &);
  using MergeSchemaLoader = int (*)(const storage::ObTablet &, const SCN &, bool,
      common::ObIAllocator &, SCN &, storage::ObStorageSchema &);
  static int server_module_init(ObSchemaRuntimeService *&schema_runtime_service);

  ObSchemaRuntimeService()
    : tablet_schema_resolver_(nullptr), merge_schema_loader_(nullptr)
  {
  }
  ~ObSchemaRuntimeService() {}

  void destroy();
  void set_tablet_schema_resolver(TabletSchemaResolver resolver)
  { tablet_schema_resolver_.store(resolver, std::memory_order_release); }
  void set_merge_schema_loader(MergeSchemaLoader loader)
  { merge_schema_loader_.store(loader, std::memory_order_release); }
  // Returns a complete output layout from a protected catalog snapshot;
  // this operation never activates a Namespace runtime or schema cache.
  int load_merge_schema(const storage::ObTablet &, const SCN &requested, bool choose_current,
      common::ObIAllocator &, SCN &selected, storage::ObStorageSchema &);
  int resolve_tablet_schema(uint64_t tablet_id,
                            ObMultiVersionSchemaService *&schema_service,
                            uint64_t &schema_tablet_id);
  int resolve_tablet_schema(uint64_t tablet_id,
                            ObMultiVersionSchemaService *&schema_service)
  {
    uint64_t schema_tablet_id = tablet_id;
    return resolve_tablet_schema(tablet_id, schema_service, schema_tablet_id);
  }

private:
  std::atomic<TabletSchemaResolver> tablet_schema_resolver_;
  std::atomic<MergeSchemaLoader> merge_schema_loader_;
};

}
}
}

#endif // OCEANBASE_OB_SCHEMA_RUNTIME_SERVICE_H
