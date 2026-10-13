/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifndef OCEANBASE_STORAGE_TABLET_WRITE_LAYOUT_H_
#define OCEANBASE_STORAGE_TABLET_WRITE_LAYOUT_H_

#include "storage/ob_storage_schema.h"
#include "common/ob_tablet_id.h"
#include "share/scn.h"

namespace oceanbase
{
namespace storage
{
class ObTablet;
namespace mds { class BufferCtx; class MdsCtx; }

// One committed current value per physical tablet. MDS owns its allocator,
// transaction outcome, replay, checkpoint and eviction.
class TabletWriteLayout final
{
public:
  TabletWriteLayout() : layout_id_(0), schema_() {}
  bool is_valid() const;
  int assign(common::ObIAllocator &allocator, const TabletWriteLayout &other);
  int init(common::ObIAllocator &allocator, uint64_t layout_id, const ObStorageSchema &schema);
  int serialize(char *buf, int64_t len, int64_t &pos) const;
  int deserialize(common::ObIAllocator &allocator, const char *buf, int64_t len, int64_t &pos);
  int64_t get_serialize_size() const;
  TO_STRING_KV(K_(layout_id), K_(schema));

  uint64_t layout_id_;
  ObStorageSchema schema_;
private:
  DISALLOW_COPY_AND_ASSIGN(TabletWriteLayout);
};

class TabletWriteLayoutHelper final
{
public:
  static int load_fixed_layout(const common::ObTabletID &tablet_id,
      common::ObIAllocator &allocator, ObStorageSchema *&schema);
  static int ensure_installed(ObTablet &tablet, const ObStorageSchema &schema, int64_t deadline);
  static int initialize(ObTablet &tablet, const ObStorageSchema &schema,
      mds::MdsCtx &ctx, bool replay, const share::SCN &scn);
  static int on_register(const char *buf, int64_t len, mds::BufferCtx &ctx);
  static int on_replay(const char *buf, int64_t len, const share::SCN &scn, mds::BufferCtx &ctx);
};
}
}
#endif
