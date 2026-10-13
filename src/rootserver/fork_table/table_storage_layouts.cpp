/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "rootserver/fork_table/table_storage_layouts.h"
#include "storage/compaction/ob_freeze_info_mgr.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/ob_storage_schema.h"
#include "storage/ob_common_id_utils.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/worker.h"

namespace oceanbase {
namespace rootserver {
using namespace common;
using namespace storage;

int TableStorageLayouts::read_at(InstanceMetaStore &store, const share::SCN &target,
    int64_t deadline, ObIArray<Definition> &definitions)
{
  definitions.reuse();
  InstanceMetaStore::Transaction tx;
  auto *freezes = share::server_service<ObFreezeInfoMgr>();
  auto *transactions = share::server_service<transaction::ObTransService>();
  if (freezes == nullptr || transactions == nullptr) { return OB_NOT_INIT; }
  int ret = store.begin_read(tx, deadline, [&](share::SCN &snapshot) {
    share::SCN retained, readable;
    int rc = freezes->get_schema_history_retention(retained);
    if (rc == OB_SUCCESS && target < retained) { rc = OB_SNAPSHOT_DISCARDED; }
    if (rc == OB_SUCCESS) { rc = transactions->get_weak_read_snapshot_version(-1, readable); }
    if (rc == OB_SUCCESS && target > readable) { rc = OB_EAGAIN; }
    if (rc == OB_SUCCESS) { snapshot = target; }
    return rc;
  });
  if (ret == OB_SUCCESS) {
    ret = store.scan(tx, MetaCollection::TABLE_STORAGE_LAYOUTS, {},
        [&](const ObString &key, const ObString &value, bool &) {
      Definition item;
      int64_t ns = 0, table = 0, layout = 0, pos = 0;
      int rc = key.length() != 16 || value.length() != 8 ? OB_CHECKSUM_ERROR : OB_SUCCESS;
      if (rc == OB_SUCCESS) { rc = serialization::decode_i64(key.ptr(), key.length(), pos, &ns); }
      if (rc == OB_SUCCESS) { rc = serialization::decode_i64(key.ptr(), key.length(), pos, &table); }
      pos = 0;
      if (rc == OB_SUCCESS) { rc = serialization::decode_i64(value.ptr(), value.length(), pos, &layout); }
      if (rc == OB_SUCCESS && (ns <= 0 || table <= 0 || layout <= 0)) { rc = OB_CHECKSUM_ERROR; }
      item.namespace_id = ns;
      item.table_id = table;
      item.layout_id = layout;
      if (rc == OB_SUCCESS) { rc = definitions.push_back(item); }
      return rc;
    });
  }
  if (tx.is_active()) {
    const int end = store.commit(tx);
    if (ret == OB_SUCCESS) { ret = end; }
  }
  if (ret != OB_SUCCESS) { definitions.reuse(); }
  return ret;
}

int TableStorageLayouts::attach(ObMySQLTransaction &owner, transaction::ObTxDesc &native,
    InstanceMetaStore &store, std::shared_ptr<InstanceMetaStore::Transaction> &tx)
{
  tx.reset(new (std::nothrow) InstanceMetaStore::Transaction());
  int ret = tx == nullptr ? OB_ALLOCATE_MEMORY_FAILED : owner.retain_until_end(tx);
  if (ret == OB_SUCCESS) { ret = store.attach(*tx, native, THIS_WORKER.get_timeout_ts()); }
  return ret;
}

int TableStorageLayouts::bind(ObCreateTabletSchema &schema)
{
  int ret = OB_SUCCESS;
  if (namespace_id_ == 0 || namespace_id_ == OB_INVALID_ID || !schema.is_valid()
      || schema.is_column_info_simplified()) { return OB_INVALID_ARGUMENT; }
  char key_bytes[16];
  int64_t pos = 0;
  if (OB_FAIL(serialization::encode_i64(key_bytes, sizeof(key_bytes), pos, namespace_id_))) {
  } else if (OB_FAIL(serialization::encode_i64(key_bytes, sizeof(key_bytes), pos, schema.get_table_id()))) {
  }
  const ObString key(sizeof(key_bytes), key_bytes);
  ObArenaAllocator allocator(ObMemAttr("TableLayout"));
  ObString value;
  int64_t layout_id = 0;
  if (ret == OB_SUCCESS) {
    ret = store_.get(tx_, MetaCollection::TABLE_STORAGE_LAYOUTS, key, allocator, value);
    if (ret == OB_ENTRY_NOT_EXIST) {
      share::ObCommonID allocated;
      ret = ObCommonIDUtils::gen_unique_id(allocated);
      if (ret == OB_SUCCESS) {
        layout_id = allocated.id();
        char encoded[8];
        pos = 0;
        ret = serialization::encode_i64(encoded, sizeof(encoded), pos, layout_id);
        // INSERT is deliberately non-waiting on native row conflicts. A DDL
        // can arrive here before taking the Namespace publication lock, while
        // first materialization already holds that lock. Waiting on this row
        // would invert their lock order. The losing owner must roll back.
        if (ret == OB_SUCCESS) {
          ret = store_.insert(tx_, MetaCollection::TABLE_STORAGE_LAYOUTS,
              key, ObString(sizeof(encoded), encoded));
        }
        if (ret == OB_ERR_PRIMARY_KEY_DUPLICATE) { ret = OB_TRY_LOCK_ROW_CONFLICT; }
      }
    } else if (ret == OB_SUCCESS) {
      pos = 0;
      ret = serialization::decode_i64(value.ptr(), value.length(), pos, &layout_id);
      if (ret == OB_SUCCESS && (pos != value.length() || layout_id <= 0)) { ret = OB_CHECKSUM_ERROR; }
    }
  }
  if (ret == OB_SUCCESS) { schema.set_storage_layout_id(layout_id); }
  return ret;
}

int TableStorageLayouts::prepare_create(ObCreateTabletSchema &schema)
{
  return bind(schema);
}

int TableStorageLayouts::publish(ObCreateTabletSchema &schema)
{
  return bind(schema);
}

int TableStorageLayouts::retire(uint64_t table_id)
{
  if (namespace_id_ == 0 || namespace_id_ == OB_INVALID_ID
      || table_id == 0 || table_id == OB_INVALID_ID) { return OB_INVALID_ARGUMENT; }
  char key[16];
  int64_t pos = 0;
  int ret = serialization::encode_i64(key, sizeof(key), pos, namespace_id_);
  if (ret == OB_SUCCESS) { ret = serialization::encode_i64(key, sizeof(key), pos, table_id); }
  bool existed = false;
  if (ret == OB_SUCCESS) {
    ret = store_.erase(tx_, MetaCollection::TABLE_STORAGE_LAYOUTS, ObString(sizeof(key), key), existed);
  }
  return ret;
}

int TableStorageLayouts::retire_namespace()
{
  if (namespace_id_ == 0 || namespace_id_ >= INT64_MAX) { return OB_INVALID_ARGUMENT; }
  char lower[8], upper[8];
  int64_t pos = 0;
  int ret = serialization::encode_i64(lower, sizeof(lower), pos, namespace_id_);
  pos = 0;
  if (ret == OB_SUCCESS) {
    ret = serialization::encode_i64(upper, sizeof(upper), pos, namespace_id_ + 1);
  }
  InstanceMetaStore::KeyRange range;
  range.lower.assign_ptr(lower, sizeof(lower));
  range.upper.assign_ptr(upper, sizeof(upper));
  range.has_lower = range.has_upper = true;
  ObArray<uint64_t> tables;
  if (ret == OB_SUCCESS) {
    ret = store_.scan(tx_, MetaCollection::TABLE_STORAGE_LAYOUTS, range,
        [&](const ObString &key, const ObString &, bool &) {
      int64_t owner = 0, table = 0, offset = 0;
      int rc = key.length() != 16 ? OB_CHECKSUM_ERROR : OB_SUCCESS;
      if (rc == OB_SUCCESS) { rc = serialization::decode_i64(key.ptr(), key.length(), offset, &owner); }
      if (rc == OB_SUCCESS) { rc = serialization::decode_i64(key.ptr(), key.length(), offset, &table); }
      if (rc == OB_SUCCESS && (owner != namespace_id_ || table <= 0)) { rc = OB_CHECKSUM_ERROR; }
      if (rc == OB_SUCCESS) { rc = tables.push_back(table); }
      return rc;
    });
  }
  // End the scan before writing through its transaction.
  for (int64_t i = 0; ret == OB_SUCCESS && i < tables.count(); ++i) { ret = retire(tables.at(i)); }
  return ret;
}

} // namespace rootserver
} // namespace oceanbase
