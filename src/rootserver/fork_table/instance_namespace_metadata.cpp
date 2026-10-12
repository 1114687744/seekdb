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

#define USING_LOG_PREFIX RS
#include "rootserver/fork_table/instance_namespace_metadata.h"
#include "rootserver/fork_table/table_storage_layouts.h"
#include "namespace/namespace.h"
#include "lib/hash_func/murmur_hash.h"
#include "lib/allocator/ob_allocator.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oceanbase
{
namespace rootserver
{
namespace
{
using namespace common;
using storage::InstanceMetaStore;
using storage::InstanceTable;

ObObj id_cell(uint64_t id) { ObObj cell; cell.set_uint64(id); return cell; }
ObObj name_cell(const std::string &name)
{
  ObObj cell;
  cell.set_varbinary(ObString(name.size(), name.data()));
  return cell;
}
ObNewRow row_of(ObObj *cells, int64_t count)
{ ObNewRow row; row.assign(cells, count); return row; }

bool valid_namespace(const InstanceNamespaceRecord &record)
{
  return ns::NamespaceObjectKey{record.id, 1}.is_valid()
      && (!record.name.empty() || record.roots.state == 2)
      && record.name.size() <= 128
      && record.roots.state >= 0 && record.roots.state <= 2
      && record.parent_namespace < record.id && record.fork_cap >= 0;
}

int decode_namespace(const ObNewRow &row, InstanceNamespaceRecord &record)
{
  if (row.count_ != 13) { return OB_CHECKSUM_ERROR; }
  InstanceNamespaceRecord decoded;
  const auto *c = row.cells_;
  decoded.id = c[0].get_uint64();
  decoded.name.assign(c[1].get_string().ptr(), c[1].get_string().length());
  decoded.roots.source = c[2].get_uint64();
  decoded.roots.catalog.page = c[3].get_uint64();
  decoded.roots.catalog.cap = c[4].get_int();
  decoded.roots.directory.page = c[5].get_uint64();
  decoded.roots.directory.cap = c[6].get_int();
  decoded.roots.snapshot = c[7].get_int();
  decoded.roots.schema_version = c[8].get_int();
  decoded.roots.state = c[9].get_int();
  decoded.parent_namespace = c[10].get_uint64();
  decoded.fork_cap = c[11].get_int();
  const int64_t allow_login = c[12].get_int();
  if (!valid_namespace(decoded) || (allow_login != 0 && allow_login != 1)) {
    return OB_CHECKSUM_ERROR;
  }
  decoded.allow_login = allow_login != 0;
  record = std::move(decoded);
  return OB_SUCCESS;
}

int write_namespace(InstanceMetaStore &store, InstanceMetaStore::Transaction &tx,
    const InstanceNamespaceRecord &record, bool insert)
{
  if (!valid_namespace(record)) { return OB_INVALID_ARGUMENT; }
  ObObj cells[13];
  cells[0] = id_cell(record.id);
  cells[1] = name_cell(record.name);
  cells[2].set_uint64(record.roots.source);
  cells[3].set_uint64(record.roots.catalog.page);
  cells[4].set_int(record.roots.catalog.cap);
  cells[5].set_uint64(record.roots.directory.page);
  cells[6].set_int(record.roots.directory.cap);
  cells[7].set_int(record.roots.snapshot);
  cells[8].set_int(record.roots.schema_version);
  cells[9].set_int(record.roots.state);
  cells[10].set_uint64(record.parent_namespace);
  cells[11].set_int(record.fork_cap);
  cells[12].set_int(record.allow_login ? 1 : 0);
  const auto row = row_of(cells, 13);
  return insert ? store.insert(tx, InstanceTable::NAMESPACES, row)
                : store.put(tx, InstanceTable::NAMESPACES, row);
}

int insert_name(InstanceMetaStore &store, InstanceMetaStore::Transaction &tx,
    const InstanceNamespaceRecord &record)
{
  ObObj cells[] = {name_cell(record.name), id_cell(record.id)};
  return store.insert(tx, InstanceTable::NAMESPACE_NAMES, row_of(cells, 2));
}
int erase_row(InstanceMetaStore &store, InstanceMetaStore::Transaction &tx,
    InstanceTable table, const ObObj &key)
{
  bool existed = false;
  const int ret = store.erase(tx, table, key, existed);
  return ret == OB_SUCCESS && !existed ? OB_ENTRY_NOT_EXIST : ret;
}
} // namespace

int InstanceNamespaceMetadata::get_namespace(uint64_t id,
    InstanceNamespaceRecord &record, bool lock)
{
  ObArenaAllocator allocator(ObMemAttr("InstanceMetaGet"));
  ObNewRow row;
  const int ret = store_.get(transaction_, InstanceTable::NAMESPACES, id_cell(id), allocator, row,
      lock ? InstanceMetaStore::Read::FOR_UPDATE : InstanceMetaStore::Read::SNAPSHOT);
  return ret == OB_SUCCESS ? decode_namespace(row, record) : ret;
}

int InstanceNamespaceMetadata::find_namespace(const std::string &name, uint64_t &id)
{
  id = 0;
  if (name.empty() || name.size() > 128) { return OB_INVALID_ARGUMENT; }
  ObArenaAllocator allocator(ObMemAttr("InstanceMetaGet"));
  ObNewRow row;
  const int ret = store_.get(transaction_, InstanceTable::NAMESPACE_NAMES, name_cell(name), allocator, row);
  if (ret == OB_SUCCESS) { id = row.cells_[1].get_uint64(); }
  return ret;
}

int InstanceNamespaceMetadata::insert_namespace(const InstanceNamespaceRecord &record)
{
  if (record.name.empty() || !valid_namespace(record)) { return OB_INVALID_ARGUMENT; }
  int ret = insert_name(store_, transaction_, record);
  if (ret == OB_SUCCESS) { ret = write_namespace(store_, transaction_, record, true); }
  return ret;
}

int InstanceNamespaceMetadata::update_namespace(const InstanceNamespaceRecord &record)
{
  if (!valid_namespace(record)) { return OB_INVALID_ARGUMENT; }
  InstanceNamespaceRecord previous;
  int ret = get_namespace(record.id, previous, true);
  if (ret == OB_SUCCESS && previous.roots.state == 2 && record.roots.state != 2) {
    ret = OB_STATE_NOT_MATCH;
  }
  if (ret == OB_SUCCESS && record.name != previous.name) {
    if (!previous.name.empty()) {
      ret = erase_row(store_, transaction_, InstanceTable::NAMESPACE_NAMES, name_cell(previous.name));
    }
    if (ret == OB_SUCCESS && !record.name.empty()) { ret = insert_name(store_, transaction_, record); }
  }
  if (ret == OB_SUCCESS) { ret = write_namespace(store_, transaction_, record, false); }
  return ret;
}

int InstanceNamespaceMetadata::rename_namespace(uint64_t id,
    const std::string &expected_name, const std::string &new_name)
{
  if (expected_name.empty() || new_name.empty() || new_name.size() > 128
      || expected_name == new_name) { return OB_INVALID_ARGUMENT; }
  InstanceNamespaceRecord record;
  int ret = get_namespace(id, record, true);
  if (ret == OB_SUCCESS && (record.roots.state != 0
      || record.name != expected_name)) { ret = OB_STATE_NOT_MATCH; }
  if (ret == OB_SUCCESS) {
    record.name = new_name;
    ret = update_namespace(record);
  }
  return ret;
}

int InstanceNamespaceMetadata::erase_namespace(uint64_t id)
{
  InstanceNamespaceRecord previous;
  int ret = get_namespace(id, previous, true);
  if (ret == OB_SUCCESS && !previous.name.empty()) {
    ret = erase_row(store_, transaction_, InstanceTable::NAMESPACE_NAMES, name_cell(previous.name));
  }
  if (ret == OB_SUCCESS) { ret = erase_row(store_, transaction_, InstanceTable::NAMESPACES, id_cell(id)); }
  return ret;
}

int InstanceNamespaceMetadata::scan_namespaces(const NamespaceVisitor &visitor)
{
  if (!visitor) { return OB_INVALID_ARGUMENT; }
  return store_.scan(transaction_, InstanceTable::NAMESPACES,
      [&](const ObNewRow &row, bool &) {
        InstanceNamespaceRecord record;
        const int ret = decode_namespace(row, record);
        return ret == OB_SUCCESS ? visitor(record) : ret;
      });
}

int InstanceNamespaceMetadata::insert_root_namespace(const std::string &name,
    int64_t schema_version)
{
  if (name.empty() || name.size() > 128 || schema_version <= 0) {
    return OB_INVALID_ARGUMENT;
  }
  InstanceNamespaceRecord root;
  root.id = 1;
  root.name = name;
  root.roots.schema_version = schema_version;
  int ret = initialize_namespace_counter(root.id);
  if (ret == OB_SUCCESS) { ret = insert_namespace(root); }
  return ret;
}

int InstanceNamespaceMetadata::initialize_namespace_counter(uint64_t high_watermark)
{
  if (high_watermark < 1 || high_watermark >= ns::NamespaceObjectKey::NAMESPACE_LIMIT) {
    return OB_INVALID_ARGUMENT;
  }
  ObObj cells[] = {id_cell(1), id_cell(high_watermark)};
  return store_.insert(transaction_, InstanceTable::COUNTERS, row_of(cells, 2));
}

int InstanceNamespaceMetadata::allocate_namespace_id(uint64_t &id)
{
  id = 0;
  ObArenaAllocator allocator(ObMemAttr("InstanceCounter"));
  ObNewRow row;
  int ret = store_.get(transaction_, InstanceTable::COUNTERS, id_cell(1), allocator, row,
                       InstanceMetaStore::Read::FOR_UPDATE);
  if (ret == OB_ENTRY_NOT_EXIST) { return OB_NOT_INIT; }
  if (ret != OB_SUCCESS) { return ret; }
  const uint64_t high = row.cells_[1].get_uint64();
  if (high >= ns::NamespaceObjectKey::NAMESPACE_LIMIT - 1) { return OB_SIZE_OVERFLOW; }
  id = high + 1;
  row.cells_[1].set_uint64(id);
  return store_.put(transaction_, InstanceTable::COUNTERS, row);
}

int InstanceNamespaceMetadata::lock_namespace_creation()
{
  ObArenaAllocator allocator(ObMemAttr("InstanceCounter"));
  ObNewRow row;
  const int ret = store_.get(transaction_, InstanceTable::COUNTERS, id_cell(1), allocator, row,
                             InstanceMetaStore::Read::FOR_UPDATE);
  return ret == OB_ENTRY_NOT_EXIST ? OB_NOT_INIT : ret;
}

int InstanceNamespaceMetadata::initialize_snapshot_gc_watermark(int64_t watermark)
{
  if (watermark < 0) { return OB_INVALID_ARGUMENT; }
  ObObj cells[2]; cells[0] = id_cell(1); cells[1].set_int(watermark);
  return store_.insert(transaction_, InstanceTable::SNAPSHOT_COORDINATION, row_of(cells, 2));
}

int InstanceNamespaceMetadata::get_snapshot_gc_watermark(int64_t &watermark, bool lock)
{
  watermark = 0;
  ObArenaAllocator allocator(ObMemAttr("InstSnapshot"));
  ObNewRow row;
  int ret = store_.get(transaction_, InstanceTable::SNAPSHOT_COORDINATION, id_cell(1), allocator, row,
      lock ? InstanceMetaStore::Read::FOR_UPDATE : InstanceMetaStore::Read::SNAPSHOT);
  if (ret == OB_SUCCESS) {
    watermark = row.cells_[1].get_int();
    if (watermark < 0) { ret = OB_CHECKSUM_ERROR; }
  }
  return ret;
}

int InstanceNamespaceMetadata::advance_snapshot_gc_watermark(int64_t watermark)
{
  if (watermark < 0) { return OB_INVALID_ARGUMENT; }
  int64_t current = 0;
  int ret = get_snapshot_gc_watermark(current, true);
  if (ret == OB_ENTRY_NOT_EXIST) { return OB_NOT_INIT; }
  if (ret == OB_SUCCESS && watermark > current) {
    ObObj cells[2]; cells[0] = id_cell(1); cells[1].set_int(watermark);
    ret = store_.put(transaction_, InstanceTable::SNAPSHOT_COORDINATION, row_of(cells, 2));
  }
  return ret;
}

int InstanceNamespaceMetadata::read_page(uint64_t page_id, std::string &data)
{
  ObArenaAllocator allocator(ObMemAttr("InstancePage"));
  ObNewRow row;
  // Locked roots may be newer than the ordinary read snapshot. Historical
  // roots may instead reference pages already deleted from the current view.
  // The caller chooses the view; do not silently retry using a different one.
  const int ret = store_.get(transaction_, InstanceTable::PAGES, id_cell(page_id), allocator, row,
                             page_read_ == PageRead::LOCKED_ROOT
                                 ? InstanceMetaStore::Read::CURRENT : InstanceMetaStore::Read::SNAPSHOT);
  if (ret != OB_SUCCESS) { return ret; }
  const auto &bytes = row.cells_[1].get_string();
  data.assign(bytes.ptr(), bytes.length());
  return murmurhash(data.data(), static_cast<int32_t>(data.size()), 0) == page_id
      ? OB_SUCCESS : OB_CHECKSUM_ERROR;
}

int InstanceNamespaceMetadata::save_page(const std::string &data, uint64_t &page_id)
{
  page_id = murmurhash(data.data(), static_cast<int32_t>(data.size()), 0);
  if (page_id == 0 || data.size() > 60000) { return OB_SIZE_OVERFLOW; }
  std::string existing;
  // Deduplication is an insertion check against current immutable content.
  // The enclosing transaction excludes physical page deletion.
  InstanceNamespaceMetadata current(store_, transaction_, PageRead::LOCKED_ROOT);
  const int ret = current.read_page(page_id, existing);
  if (ret == OB_SUCCESS) { return existing == data ? OB_SUCCESS : OB_CHECKSUM_ERROR; }
  if (ret != OB_ENTRY_NOT_EXIST) { return ret; }
  ObObj cells[2]; cells[0] = id_cell(page_id); cells[1].set_varbinary(ObString(data.size(), data.data()));
  return store_.insert(transaction_, InstanceTable::PAGES, row_of(cells, 2));
}

int InstanceNamespaceMetadata::erase_page(uint64_t page_id)
{
  return erase_row(store_, transaction_, InstanceTable::PAGES, id_cell(page_id));
}

int InstanceNamespaceMetadata::scan_pages(const PageVisitor &visitor)
{
  if (!visitor) { return OB_INVALID_ARGUMENT; }
  return store_.scan(transaction_, InstanceTable::PAGES,
      [&](const ObNewRow &row, bool &) { return visitor(row.cells_[0].get_uint64()); });
}

int InstanceNamespaceMetadata::save_object(const std::string &data, uint64_t &object)
{
  using Codec = ns::NamespaceCatalogCodec;
  if (data.size() > Codec::MAX_OBJECT_CHUNKS * Codec::OBJECT_CHUNK_BYTES) {
    return OB_SIZE_OVERFLOW;
  }
  std::vector<uint64_t> chunks;
  int ret = OB_SUCCESS;
  for (size_t pos = 0; ret == OB_SUCCESS && pos < data.size(); pos += Codec::OBJECT_CHUNK_BYTES) {
    uint64_t chunk = 0;
    ret = save_page(data.substr(pos, Codec::OBJECT_CHUNK_BYTES), chunk);
    if (ret == OB_SUCCESS) { chunks.push_back(chunk); }
  }
  uint64_t staged = 0;
  if (ret == OB_SUCCESS) { ret = save_page(Codec::encode_object(data.size(), chunks), staged); }
  if (ret == OB_SUCCESS) { object = staged; }
  return ret;
}

int InstanceNamespaceMetadata::read_object(uint64_t object, std::string &data)
{
  using Codec = ns::NamespaceCatalogCodec;
  std::string manifest, decoded;
  uint64_t size = 0;
  std::vector<uint64_t> chunks;
  int ret = read_page(object, manifest);
  if (ret == OB_SUCCESS && !Codec::decode_object(manifest, size, chunks)) {
    ret = OB_CHECKSUM_ERROR;
  }
  for (uint64_t chunk : chunks) {
    if (ret != OB_SUCCESS) { break; }
    std::string bytes;
    ret = read_page(chunk, bytes);
    if (ret == OB_SUCCESS && bytes.size()
        != std::min<uint64_t>(Codec::OBJECT_CHUNK_BYTES, size - decoded.size())) {
      ret = OB_CHECKSUM_ERROR;
    }
    if (ret == OB_SUCCESS) { decoded += bytes; }
  }
  if (ret == OB_SUCCESS) { data.swap(decoded); }
  return ret;
}

int InstanceNamespaceMetadata::find_tablet_source(ns::CatalogPageRef root,
    uint64_t logical_tablet, ns::CatalogTabletSource &source, int64_t &cap)
{
  InstanceCatalogPageStore pages(*this);
  ns::NamespaceCatalogTree tree(pages);
  ns::CatalogValue value;
  const auto result = tree.find(root, ns::NamespaceCatalogCodec::object_key(logical_tablet), value);
  if (!result.ok()) {
    return result.error == ns::CatalogTreeError::NOT_FOUND ? OB_ENTRY_NOT_EXIST
        : result.error == ns::CatalogTreeError::STORE ? result.store_error : OB_CHECKSUM_ERROR;
  }
  if (!ns::NamespaceCatalogCodec::decode_source(value.data, source)) { return OB_CHECKSUM_ERROR; }
  cap = value.cap;
  return OB_SUCCESS;
}

int InstanceNamespaceMetadata::read_table_definition(ns::CatalogPageRef root,
    uint64_t table_id, std::string &definition)
{
  InstanceCatalogPageStore pages(*this);
  ns::NamespaceCatalogTree tree(pages);
  ns::CatalogValue value;
  const auto result = tree.find(root, ns::NamespaceCatalogCodec::object_key(table_id), value);
  if (!result.ok()) {
    return result.error == ns::CatalogTreeError::NOT_FOUND ? OB_ENTRY_NOT_EXIST
        : result.error == ns::CatalogTreeError::STORE ? result.store_error : OB_CHECKSUM_ERROR;
  }
  uint64_t object = 0, table = 0, tablet = 0, bound = 0;
  if (!ns::NamespaceCatalogCodec::decode_entry(value.data, object, table, tablet, bound)
      || object == 0 || table != table_id || tablet != 0 || bound != 0) {
    return OB_CHECKSUM_ERROR;
  }
  return read_object(object, definition);
}

int InstanceNamespaceMetadata::stage_catalog_delta(uint64_t namespace_id,
    int64_t base_schema_version, int64_t schema_version,
    const ns::CatalogChanges &definitions, const ns::CatalogChanges &sources)
{
  if (base_schema_version < 0 || schema_version <= 0
      || schema_version < base_schema_version) { return OB_INVALID_ARGUMENT; }
  InstanceNamespaceRecord record;
  int ret = get_namespace(namespace_id, record, true);
  if (ret == OB_SUCCESS && record.roots.state != 0) { ret = OB_OP_NOT_ALLOW; }
  if (ret == OB_SUCCESS && record.roots.schema_version != base_schema_version) {
    ret = OB_EAGAIN;
  }
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata current(store_, transaction_, PageRead::LOCKED_ROOT);
    InstanceCatalogPageStore pages(current);
    ns::NamespaceCatalogTree tree(pages);
    auto result = tree.apply(record.roots.catalog, definitions, record.roots.catalog);
    if (result.ok()) {
      result = tree.apply(record.roots.directory, sources, record.roots.directory);
    }
    switch (result.error) {
      case ns::CatalogTreeError::NONE: break;
      case ns::CatalogTreeError::INVALID: ret = OB_INVALID_ARGUMENT; break;
      case ns::CatalogTreeError::TOO_LARGE:
      case ns::CatalogTreeError::TOO_DEEP: ret = OB_SIZE_OVERFLOW; break;
      case ns::CatalogTreeError::CORRUPT: ret = OB_CHECKSUM_ERROR; break;
      case ns::CatalogTreeError::STORE: ret = result.store_error; break;
      default: ret = OB_ERR_UNEXPECTED; break;
    }
    if (ret == OB_SUCCESS) {
      record.roots.schema_version = schema_version;
      ret = update_namespace(record);
    }
  }
  return ret;
}

int InstanceNamespaceMetadata::page_roots(PageRoots &roots)
{
  roots.clear();
  return scan_namespaces([&](const InstanceNamespaceRecord &record) {
    roots.emplace(record.id, std::make_pair(record.roots.catalog.page, record.roots.directory.page));
    return OB_SUCCESS;
  });
}

int InstanceNamespaceMetadata::find_unreachable_pages(
    int64_t max_deletes, PageRoots &roots, std::vector<uint64_t> &garbage)
{
  garbage.clear();
  if (max_deletes <= 0 || max_deletes > 256) {
    return OB_INVALID_ARGUMENT;
  }
  // Only table-definition leaves own description objects. Source leaves have
  // physical identities, not object-page references.
  struct PendingTree { ns::CatalogPageRef ref; bool definitions; };
  std::vector<PendingTree> pending;
  int ret = page_roots(roots);
  for (const auto &root : roots) {
    if (root.second.first != 0) { pending.push_back({{root.second.first, 0}, true}); }
    if (root.second.second != 0) { pending.push_back({{root.second.second, 0}, false}); }
  }
  // Active readers retain their KV snapshot, so logical page deletes may
  // proceed. Their historical page versions remain readable until the last
  // holder releases, including when deletes originate on another server.
  InstanceCatalogPageStore pages(*this);
  ns::NamespaceCatalogTree tree(pages);
  std::unordered_set<uint64_t> reachable;
  std::unordered_set<uint64_t> visited_nodes[2];
  std::unordered_set<uint64_t> visited_objects;
  while (ret == OB_SUCCESS && !pending.empty()) {
    const PendingTree item = pending.back();
    const ns::CatalogPageRef ref = item.ref;
    pending.pop_back();
    if (!visited_nodes[item.definitions].insert(ref.page).second) { continue; }
    reachable.insert(ref.page);
    ns::CatalogNode node;
    const auto result = tree.read_node(ref, node);
    if (!result.ok()) {
      switch (result.error) {
        case ns::CatalogTreeError::CORRUPT: ret = OB_CHECKSUM_ERROR; break;
        case ns::CatalogTreeError::TOO_DEEP: ret = OB_SIZE_OVERFLOW; break;
        case ns::CatalogTreeError::STORE: ret = result.store_error; break;
        default: ret = OB_ERR_UNEXPECTED; break;
      }
    } else if (!node.leaf) {
      for (const auto &child : node.children) { pending.push_back({child, item.definitions}); }
    } else if (item.definitions) {
      for (const auto &value : node.values) {
        uint64_t object = 0, table = 0, tablet = 0, bound = 0;
        if (!ns::NamespaceCatalogCodec::decode_entry(
                value.data, object, table, tablet, bound)) {
          ret = OB_CHECKSUM_ERROR;
          break;
        }
        if (object != 0 && visited_objects.insert(object).second) {
          reachable.insert(object);
          std::string manifest;
          uint64_t size = 0;
          std::vector<uint64_t> chunks;
          if (OB_SUCCESS != (ret = read_page(object, manifest))) { break; }
          if (!ns::NamespaceCatalogCodec::decode_object(manifest, size, chunks)) {
            ret = OB_CHECKSUM_ERROR;
            break;
          }
          for (uint64_t chunk : chunks) {
            std::string bytes;
            if (OB_SUCCESS != (ret = read_page(chunk, bytes))) { break; }
            const uint64_t expected = std::min<uint64_t>(
                ns::NamespaceCatalogCodec::OBJECT_CHUNK_BYTES, size);
            if (bytes.size() != expected) { ret = OB_CHECKSUM_ERROR; break; }
            size -= expected;
            reachable.insert(chunk);
          }
          if (ret != OB_SUCCESS) { break; }
        }
      }
    }
  }
  if (ret == OB_SUCCESS) {
    ret = store_.scan(transaction_, InstanceTable::PAGES,
        [&](const ObNewRow &row, bool &stop) {
      const uint64_t page = row.cells_[0].get_uint64();
      if (page == 0) { return OB_CHECKSUM_ERROR; }
      if (reachable.count(page) == 0) { garbage.push_back(page); }
      stop = garbage.size() >= static_cast<size_t>(max_deletes);
      return OB_SUCCESS;
    });
  }
  return ret;
}

int InstanceNamespaceMetadata::erase_unreachable_pages(
    const PageRoots &roots, const std::vector<uint64_t> &garbage)
{
  if (!transaction_.is_directory_gc() || garbage.size() > 256) { return OB_INVALID_ARGUMENT; }
  PageRoots current;
  int ret = page_roots(current);
  // Contents are immutable, including hash-based page reuse. Once every root
  // still matches, none of the candidates is referenced by a current tree.
  // Writers remain excluded through the native DELETE commit.
  if (ret == OB_SUCCESS && roots != current) { ret = OB_EAGAIN; }
  for (const uint64_t page : garbage) {
    if (ret != OB_SUCCESS) { break; }
    ret = erase_page(page);
  }
  return ret;
}

int InstanceNamespaceDirectory::scan_sources(uint64_t id, const std::string &position, int64_t deadline,
    std::vector<std::pair<std::string, ns::CatalogValue>> &entries)
{
  auto *store = &store_;
  InstanceMetaStore::Transaction tx;
  int ret = store->begin(tx, deadline, true);
  rootserver::InstanceNamespaceMetadata metadata(*store, tx);
  rootserver::InstanceNamespaceRecord record;
  if (OB_SUCC(ret)) { ret = metadata.get_namespace(id, record); }
  if (OB_SUCC(ret) && record.roots.state != 0) { ret = OB_OP_NOT_ALLOW; }
  if (OB_SUCC(ret)) {
    rootserver::InstanceCatalogPageStore pages(metadata);
    ns::NamespaceCatalogTree tree(pages);
    const auto result = tree.scan(record.roots.directory, position, 64, entries);
    if (!result.ok()) {
      ret = result.error == ns::CatalogTreeError::STORE ? result.store_error : OB_CHECKSUM_ERROR;
    }
  }
  if (tx.is_active()) {
    const int end = store->rollback(tx);
    if (OB_SUCC(ret)) { ret = end; }
  }
  return ret;
}

int InstanceNamespaceDirectory::collect_catalog_pages(int64_t deadline, int64_t &deleted)
{
  deleted = 0;
  InstanceNamespaceMetadata::PageRoots roots;
  std::vector<uint64_t> garbage;
  storage::InstanceMetaStore::Transaction scan;
  int ret = store_.begin(scan, deadline, true);
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, scan);
    ret = metadata.find_unreachable_pages(256, roots, garbage);
  }
  if (scan.is_active()) {
    const int end = store_.rollback(scan);
    if (ret == OB_SUCCESS) { ret = end; }
  }
  // There is no cross-invocation plan or lease. After the scan transaction
  // closes, only IDs remain; validation uses a fresh committed snapshot.
  if (ret == OB_SUCCESS && !garbage.empty()) {
    storage::InstanceMetaStore::Transaction sweep;
    const int64_t sweep_deadline = std::min(deadline, ObTimeUtility::current_time() + 1000000);
    ret = store_.begin_directory_gc(sweep, sweep_deadline);
    if (ret == OB_SUCCESS) {
      InstanceNamespaceMetadata metadata(store_, sweep);
      ret = metadata.erase_unreachable_pages(roots, garbage);
    }
    if (sweep.is_active()) {
      const int end = ret == OB_SUCCESS ? store_.commit(sweep) : store_.rollback(sweep);
      if (ret == OB_SUCCESS) { ret = end; }
    }
    if (ret == OB_SUCCESS) { deleted = garbage.size(); }
  }
  return ret;
}

int InstanceNamespaceMetadata::fork_namespace(const std::string &source_name,
    const std::string &target_name, const SnapshotAcquirer &acquire_snapshot,
    uint64_t &child_id, bool allow_login)
{
  child_id = 0;
  if (source_name.empty() || source_name.size() > 128 || target_name.empty()
      || target_name.size() > 128 || !acquire_snapshot) {
    return OB_INVALID_ARGUMENT;
  }
  uint64_t source_id = 0;
  int ret = find_namespace(source_name, source_id);
  InstanceNamespaceRecord source;
  if (ret == OB_SUCCESS) { ret = get_namespace(source_id, source, true); }
  if (ret == OB_SUCCESS && (source.name != source_name || source.roots.state != 0)) {
    ret = OB_STATE_NOT_MATCH;
  }
  ns::CatalogRoots roots = source.roots;
  if (ret == OB_SUCCESS) { ret = acquire_snapshot(roots.snapshot); }
  if (ret == OB_SUCCESS && (roots.snapshot <= 0 || roots.schema_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  }
  // Hold this row lock through publication. A collector which observes C
  // sees every already admitted fork at S<=C; later forks cannot introduce
  // a dependency older than that floor. Roots carry all inherited older caps.
  int64_t watermark = 0;
  if (ret == OB_SUCCESS) {
    ret = get_snapshot_gc_watermark(watermark, true);
    if (ret == OB_ENTRY_NOT_EXIST) { ret = OB_NOT_INIT; }
  }
  if (ret == OB_SUCCESS && roots.snapshot <= watermark) { ret = OB_SNAPSHOT_DISCARDED; }
  uint64_t allocated_id = 0;
  if (ret == OB_SUCCESS) { ret = allocate_namespace_id(allocated_id); }
  if (ret == OB_SUCCESS) {
    roots.catalog.cap = ns::NamespaceCatalogCodec::cap_min(roots.catalog.cap, roots.snapshot);
    roots.directory.cap = ns::NamespaceCatalogCodec::cap_min(roots.directory.cap, roots.snapshot);
    roots.source = 0;
    InstanceNamespaceRecord child;
    child.id = allocated_id;
    child.name = target_name;
    child.roots = roots;
    child.parent_namespace = source_id;
    child.fork_cap = roots.snapshot;
    child.allow_login = allow_login;
    ret = insert_namespace(child);
  }
  if (ret == OB_SUCCESS) { child_id = allocated_id; }
  return ret;
}

int InstanceNamespaceMetadata::mark_namespace_deleting(uint64_t id, bool &done)
{
  done = false;
  if (id == 1) { return OB_OP_NOT_ALLOW; }
  InstanceNamespaceRecord record;
  int ret = get_namespace(id, record, true);
  if (ret == OB_SUCCESS) {
    if (record.roots.state == 2) {
      done = true;
    } else if (record.roots.state == 0) {
      record.roots.state = 1;
      ret = update_namespace(record);
    } else if (record.roots.state != 1) {
      ret = OB_STATE_NOT_MATCH;
    }
  }
  return ret;
}

int InstanceNamespaceMetadata::finish_namespace_drop(uint64_t id)
{
  if (id == 1) { return OB_OP_NOT_ALLOW; }
  InstanceNamespaceRecord record;
  int ret = get_namespace(id, record, true);
  if (ret == OB_SUCCESS && record.roots.state != 1) {
    ret = OB_STATE_NOT_MATCH;
  }
  if (ret == OB_SUCCESS) {
    record.name.clear();
    record.roots = ns::CatalogRoots();
    record.roots.state = 2;
    ret = update_namespace(record);
  }
  return ret;
}

int InstanceNamespaceMetadata::prune_deleted_namespace(uint64_t id,
    const NamespacePhysicalProbe &has_physical, bool &pruned)
{
  pruned = false;
  if (id <= 1 || !has_physical) { return OB_INVALID_ARGUMENT; }
  InstanceNamespaceRecord record;
  int ret = get_namespace(id, record, true);
  if (ret == OB_SUCCESS && record.roots.state != 2) { ret = OB_STATE_NOT_MATCH; }
  bool has_child = false;
  if (ret == OB_SUCCESS) {
    ret = scan_namespaces([&](const InstanceNamespaceRecord &other) {
      if (other.parent_namespace == id) { has_child = true; }
      return OB_SUCCESS;
    });
  }
  if (ret != OB_SUCCESS || has_child) { return ret; }
  bool physical = false;
  ret = has_physical(id, physical);
  if (ret != OB_SUCCESS || physical) { return ret; }
  if (ret == OB_SUCCESS) { ret = erase_namespace(id); }
  if (ret == OB_SUCCESS) { pruned = true; }
  return ret;
}

namespace
{
int finish_directory_transaction(storage::InstanceMetaStore &store,
    storage::InstanceMetaStore::Transaction &tx, int ret)
{
  if (tx.is_active()) {
    const int end_ret = ret == OB_SUCCESS ? store.commit(tx) : store.rollback(tx);
    if (ret == OB_SUCCESS) { ret = end_ret; }
  }
  return ret;
}
} // namespace

int InstanceNamespaceDirectory::ensure_root(const std::string &name,
    int64_t schema_version, int64_t gc_watermark, int64_t deadline, bool &created)
{
  created = false;
  if (name.empty() || name.size() > 128 || schema_version <= 0
      || gc_watermark < 0) { return OB_INVALID_ARGUMENT; }
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin(tx, deadline);
  bool staged = false;
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    InstanceNamespaceRecord root;
    // Missing native rows must be inserted without first taking a row lock:
    // this DML path cannot turn a lock on absence into a later insert.
    ret = metadata.get_namespace(1, root);
    if (ret == OB_ENTRY_NOT_EXIST) {
      // The freeze detector can initialize coordination before the Namespace
      // directory. Preserve that record and its monotonic watermark.
      int64_t stored_watermark = 0;
      ret = metadata.get_snapshot_gc_watermark(stored_watermark);
      if (ret == OB_ENTRY_NOT_EXIST) {
        ret = metadata.initialize_snapshot_gc_watermark(gc_watermark);
      } else if (ret == OB_SUCCESS && stored_watermark < gc_watermark) {
        ret = metadata.advance_snapshot_gc_watermark(gc_watermark);
      }
      if (ret == OB_SUCCESS) { ret = metadata.insert_root_namespace(name, schema_version); }
      staged = ret == OB_SUCCESS;
    } else if (ret == OB_SUCCESS) {
      ret = metadata.get_namespace(1, root, true);
    }
    if (ret == OB_SUCCESS && !staged) {
      if (root.name != name || root.roots.state != 0
          || root.parent_namespace != 0 || root.fork_cap != 0
          || root.roots.schema_version <= 0
          || root.roots.schema_version > schema_version) {
        ret = OB_STATE_NOT_MATCH;
      } else {
        int64_t stored_watermark = 0;
        ret = metadata.get_snapshot_gc_watermark(stored_watermark, true);
        if (ret == OB_ENTRY_NOT_EXIST) { ret = OB_STATE_NOT_MATCH; }
        if (ret == OB_SUCCESS && stored_watermark < gc_watermark) {
          ret = metadata.advance_snapshot_gc_watermark(gc_watermark);
        }
        // SQL schema and catalog publication share the native commit.
        // Startup validates the existing publication without repairing it.
      }
    }
  }
  ret = finish_directory_transaction(store_, tx, ret);
  if (ret == OB_SUCCESS) { created = staged; }
  return ret;
}

int InstanceNamespaceDirectory::fork_namespace(const std::string &source_name,
    const std::string &target_name,
    const InstanceNamespaceMetadata::SnapshotAcquirer &acquire_snapshot,
    int64_t deadline, InstanceNamespaceRecord &child, bool allow_login)
{
  child = InstanceNamespaceRecord();
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin(tx, deadline);
  InstanceNamespaceRecord staged;
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    uint64_t child_id = 0;
    ret = metadata.fork_namespace(source_name, target_name, acquire_snapshot, child_id, allow_login);
    if (ret == OB_SUCCESS) { ret = metadata.get_namespace(child_id, staged); }
    if (ret == OB_SUCCESS && (staged.roots.state != 0
        || staged.parent_namespace == 0 || staged.fork_cap <= 0
        || staged.name != target_name)) { ret = OB_STATE_NOT_MATCH; }
  }
  ret = finish_directory_transaction(store_, tx, ret);
  if (ret == OB_SUCCESS) { child = std::move(staged); }
  return ret;
}

int InstanceNamespaceDirectory::find_live(const std::string &name,
    int64_t deadline, InstanceNamespaceRecord &record)
{
  int ret = find_named(name, deadline, record);
  if (ret == OB_SUCCESS && record.roots.state != 0) {
    record = InstanceNamespaceRecord();
    ret = OB_ENTRY_NOT_EXIST;
  }
  return ret;
}

int InstanceNamespaceDirectory::find_named(const std::string &name,
    int64_t deadline, InstanceNamespaceRecord &record)
{
  record = InstanceNamespaceRecord();
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin(tx, deadline, true);
  InstanceNamespaceRecord found;
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    uint64_t id = 0;
    ret = metadata.find_namespace(name, id);
    if (ret == OB_SUCCESS) { ret = metadata.get_namespace(id, found); }
    if (ret == OB_SUCCESS && found.name != name) { ret = OB_STATE_NOT_MATCH; }
  }
  ret = finish_directory_transaction(store_, tx, ret);
  if (ret == OB_SUCCESS) { record = std::move(found); }
  return ret;
}

int InstanceNamespaceDirectory::get(uint64_t id,
    int64_t deadline, InstanceNamespaceRecord &record)
{
  record = InstanceNamespaceRecord();
  if (id == 0) { return OB_INVALID_ARGUMENT; }
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin(tx, deadline, true);
  InstanceNamespaceRecord found;
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    ret = metadata.get_namespace(id, found);
  }
  ret = finish_directory_transaction(store_, tx, ret);
  if (ret == OB_SUCCESS) { record = std::move(found); }
  return ret;
}

int InstanceNamespaceDirectory::acquire_read_view(uint64_t namespace_id,
    int64_t deadline, const storage::InstanceMetaStore::SnapshotAcquirer &acquire,
    ns::NamespaceCatalogViews::Handle &view,
    const ns::NamespaceCatalogViews::Handle &previous)
{
  if (namespace_id == 0 || view || !acquire) { return OB_INVALID_ARGUMENT; }
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin_read(tx, deadline, acquire);
  ns::NamespaceCatalogViews::Handle held;
  if (ret == OB_SUCCESS && previous && previous->entry().namespace_id == namespace_id
      && previous->entry().snapshot == tx.snapshot_version().get_val_for_tx()) {
    // RR already holds this immutable root and its KV snapshot lease. Reuse
    // both without consulting the Namespace row again.
    held = previous;
  } else if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    InstanceNamespaceRecord record;
    ret = metadata.get_namespace(namespace_id, record);
    if (ret == OB_SUCCESS && record.roots.state != 0) { ret = OB_OP_NOT_ALLOW; }
    storage::InstanceMetaStore::SnapshotHandle retention;
    if (ret == OB_SUCCESS) { ret = store_.retain_snapshot(tx, retention); }
    if (ret == OB_SUCCESS) {
      held = ns::namespace_registry().catalog_views().hold(
          namespace_id, tx.snapshot_version().get_val_for_tx(), record.roots, std::move(retention));
      if (!held) { ret = OB_STATE_NOT_MATCH; }
    }
  }
  ret = finish_directory_transaction(store_, tx, ret);
  if (ret == OB_SUCCESS) { view = std::move(held); }
  return ret;
}

int InstanceNamespaceDirectory::list_live(int64_t deadline,
    std::vector<InstanceNamespaceRecord> &records)
{
  records.clear();
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin(tx, deadline, true);
  std::vector<InstanceNamespaceRecord> staged;
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    ret = metadata.scan_namespaces([&](const InstanceNamespaceRecord &record) {
      if (record.roots.state == 0) {
        if (record.name.empty()) { return OB_CHECKSUM_ERROR; }
        staged.push_back(record);
      }
      return OB_SUCCESS;
    });
  }
  ret = finish_directory_transaction(store_, tx, ret);
  if (ret == OB_SUCCESS) { records.swap(staged); }
  return ret;
}

int InstanceNamespaceDirectory::list_deleted(int64_t deadline,
    std::vector<InstanceNamespaceRecord> &records)
{
  records.clear();
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin(tx, deadline, true);
  std::vector<InstanceNamespaceRecord> staged;
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    ret = metadata.scan_namespaces([&](const InstanceNamespaceRecord &record) {
      if (record.roots.state == 2) { staged.push_back(record); }
      return OB_SUCCESS;
    });
  }
  ret = finish_directory_transaction(store_, tx, ret);
  if (ret == OB_SUCCESS) { records.swap(staged); }
  return ret;
}

int InstanceNamespaceDirectory::rename_live(uint64_t id,
    const std::string &expected_name, const std::string &new_name, int64_t deadline)
{
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin(tx, deadline);
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    ret = metadata.rename_namespace(id, expected_name, new_name);
  }
  return finish_directory_transaction(store_, tx, ret);
}

int InstanceNamespaceDirectory::mark_deleting(uint64_t id,
    const std::string &expected_name, int64_t deadline, bool &done)
{
  done = false;
  if (id == 0 || expected_name.empty()) { return OB_INVALID_ARGUMENT; }
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin(tx, deadline);
  bool staged_done = false;
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    InstanceNamespaceRecord record;
    ret = metadata.get_namespace(id, record, true);
    if (ret == OB_SUCCESS && record.roots.state != 2
        && record.name != expected_name) {
      ret = OB_STATE_NOT_MATCH;
    }
    if (ret == OB_SUCCESS) {
      ret = metadata.mark_namespace_deleting(id, staged_done);
    }
  }
  ret = finish_directory_transaction(store_, tx, ret);
  if (ret == OB_SUCCESS) { done = staged_done; }
  return ret;
}

int InstanceNamespaceDirectory::prune_deleted(uint64_t id,
    const InstanceNamespaceMetadata::NamespacePhysicalProbe &has_physical,
    int64_t deadline, bool &pruned)
{
  pruned = false;
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin(tx, deadline);
  bool staged = false;
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    ret = metadata.prune_deleted_namespace(id, has_physical, staged);
  }
  ret = finish_directory_transaction(store_, tx, ret);
  if (ret == OB_SUCCESS) { pruned = staged; }
  return ret;
}

int InstanceNamespaceDirectory::finish_drop(uint64_t id, storage::InstanceMetaStore &layouts,
    int64_t deadline)
{
  if (id <= 1) { return OB_INVALID_ARGUMENT; }
  storage::InstanceMetaStore::Transaction tx, layout_tx;
  int ret = store_.begin(tx, deadline);
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    ret = metadata.finish_namespace_drop(id);
  }
  if (ret == OB_SUCCESS) { ret = layouts.attach(layout_tx, tx, deadline); }
  if (ret == OB_SUCCESS) { ret = TableStorageLayouts(layouts, layout_tx, id).retire_namespace(); }
  ret = finish_directory_transaction(store_, tx, ret);
  if (layout_tx.is_active()) {
    const int end = layouts.detach(layout_tx);
    if (ret == OB_SUCCESS) { ret = end; }
  }
  return ret;
}

int InstanceNamespaceDirectory::schema_version(uint64_t id,
    int64_t deadline, int64_t &version)
{
  version = 0;
  if (id == 0) { return OB_INVALID_ARGUMENT; }
  storage::InstanceMetaStore::Transaction tx;
  int ret = store_.begin(tx, deadline, true);
  int64_t staged = 0;
  if (ret == OB_SUCCESS) {
    InstanceNamespaceMetadata metadata(store_, tx);
    InstanceNamespaceRecord record;
    ret = metadata.get_namespace(id, record);
    if (ret == OB_SUCCESS && record.roots.state != 0) {
      ret = OB_ENTRY_NOT_EXIST;
    }
    if (ret == OB_SUCCESS) { staged = record.roots.schema_version; }
  }
  ret = finish_directory_transaction(store_, tx, ret);
  if (ret == OB_SUCCESS) { version = staged; }
  return ret;
}

} // namespace rootserver
} // namespace oceanbase
