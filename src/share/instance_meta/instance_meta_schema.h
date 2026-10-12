/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#ifndef OCEANBASE_SHARE_INSTANCE_META_SCHEMA_H_
#define OCEANBASE_SHARE_INSTANCE_META_SCHEMA_H_

#include "common/ob_tablet_id.h"

namespace oceanbase
{
namespace share
{
namespace instance_meta
{
// Fixed native tables are LS-owned, outside every Namespace SQL catalog.
// This declaration is shared by creation, row access and diagnostic display.
enum class InstanceTable { NAMESPACES, NAMESPACE_NAMES, PAGES, COUNTERS,
                           SNAPSHOT_COORDINATION, STORAGE_LAYOUT_KV, COUNT };
enum class FieldType { UINT64, INT64, TEXT, BYTES };
struct InstanceField { const char *name; FieldType type; int64_t length = 0; };
struct InstanceTableDefinition
{
  const char *name;
  uint64_t tablet_id;
  int64_t key_columns;
  const InstanceField *fields;
  int64_t field_count;
};

inline const InstanceTableDefinition *instance_table_definition(InstanceTable table)
{
  static const InstanceField namespaces[] = {
    {"namespace_id", FieldType::UINT64}, {"name", FieldType::TEXT, 128},
    {"source", FieldType::UINT64}, {"catalog_page", FieldType::UINT64},
    {"catalog_cap", FieldType::INT64}, {"directory_page", FieldType::UINT64},
    {"directory_cap", FieldType::INT64}, {"snapshot", FieldType::INT64},
    {"schema_version", FieldType::INT64}, {"state", FieldType::INT64},
    {"parent_namespace", FieldType::UINT64}, {"fork_cap", FieldType::INT64},
    {"allow_login", FieldType::INT64}
  };
  static const InstanceField names[] = {
    {"name", FieldType::TEXT, 128}, {"namespace_id", FieldType::UINT64}
  };
  static const InstanceField pages[] = {
    {"page_id", FieldType::UINT64}, {"page_bytes", FieldType::BYTES, 60000}
  };
  static const InstanceField counters[] = {
    {"record_id", FieldType::UINT64}, {"high_watermark", FieldType::UINT64}
  };
  static const InstanceField coordination[] = {
    {"record_id", FieldType::UINT64}, {"watermark", FieldType::INT64}
  };
  static const InstanceField layouts[] = {
    {"collection_id", FieldType::UINT64}, {"key", FieldType::BYTES, 512},
    {"value", FieldType::BYTES, 65536}
  };
  using common::ObTabletID;
  static const InstanceTableDefinition definitions[] = {
    {"NAMESPACES", ObTabletID::LS_NAMESPACE_TABLET_ID, 1, namespaces, 13},
    {"NAMESPACE_NAMES", ObTabletID::LS_NAMESPACE_NAME_TABLET_ID, 1, names, 2},
    {"PAGES", ObTabletID::LS_CATALOG_PAGE_TABLET_ID, 1, pages, 2},
    {"COUNTERS", ObTabletID::LS_INSTANCE_COUNTER_TABLET_ID, 1, counters, 2},
    {"SNAPSHOT_COORDINATION", ObTabletID::LS_SNAPSHOT_COORDINATION_TABLET_ID, 1, coordination, 2},
    {"STORAGE_LAYOUT_KV", ObTabletID::LS_STORAGE_SCHEMA_TABLET_ID, 2, layouts, 3}
  };
  const int64_t index = static_cast<int64_t>(table);
  return index >= 0 && index < static_cast<int64_t>(InstanceTable::COUNT)
      ? &definitions[index] : nullptr;
}
} // namespace instance_meta
} // namespace share
} // namespace oceanbase
#endif
