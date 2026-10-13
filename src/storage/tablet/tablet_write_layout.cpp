/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX STORAGE
#include "storage/tablet/tablet_write_layout.h"
#include "storage/tablet/ob_tablet.h"
#include "storage/tablet/ob_tablet_replay_executor.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/ls/ob_ls_tablet_service.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/tx/ob_multi_data_source.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/time/ob_time_utility.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace transaction;
namespace storage
{
bool TabletWriteLayout::is_valid() const
{
  return layout_id_ != 0 && layout_id_ != OB_INVALID_ID
      && schema_.is_valid() && !schema_.is_column_info_simplified();
}

int TabletWriteLayout::init(ObIAllocator &allocator, uint64_t layout_id, const ObStorageSchema &schema)
{
  layout_id_ = layout_id;
  const int ret = schema_.init(allocator, schema);
  return ret != OB_SUCCESS ? ret : (is_valid() ? OB_SUCCESS : OB_INVALID_ARGUMENT);
}

int TabletWriteLayout::assign(ObIAllocator &allocator, const TabletWriteLayout &other)
{
  return init(allocator, other.layout_id_, other.schema_);
}

int TabletWriteLayout::serialize(char *buf, int64_t len, int64_t &pos) const
{
  int ret = serialization::encode_vi64(buf, len, pos, layout_id_);
  if (OB_SUCC(ret)) { ret = schema_.serialize(buf, len, pos); }
  return ret;
}

int TabletWriteLayout::deserialize(ObIAllocator &allocator, const char *buf, int64_t len, int64_t &pos)
{
  int ret = serialization::decode_vi64(buf, len, pos, reinterpret_cast<int64_t *>(&layout_id_));
  if (OB_SUCC(ret)) { ret = schema_.deserialize(allocator, buf, len, pos); }
  if (OB_SUCC(ret) && !is_valid()) { ret = OB_INVALID_DATA; }
  return ret;
}

int64_t TabletWriteLayout::get_serialize_size() const
{
  return serialization::encoded_length_vi64(layout_id_) + schema_.get_serialize_size();
}

int TabletWriteLayoutHelper::initialize(ObTablet &tablet, const ObStorageSchema &schema,
    mds::MdsCtx &ctx, bool replay, const SCN &scn)
{
  ObArenaAllocator allocator("WriteLayoutInit");
  TabletWriteLayout value;
  int ret = value.init(allocator, tablet.get_tablet_meta().storage_layout_id_, schema);
  if (OB_SUCC(ret) && (!replay || scn > tablet.get_tablet_meta().mds_checkpoint_scn_)) {
    ret = replay ? tablet.replay(value, ctx, scn) : tablet.set(value, ctx);
  }
  return ret;
}

namespace
{
class WriteLayoutReplay final : public ObTabletReplayExecutor
{
public:
  WriteLayoutReplay(const TabletWriteLayout &value, mds::MdsCtx &ctx, const SCN &scn)
    : value_(value), ctx_(ctx), scn_(scn) { is_inited_ = true; }
protected:
  bool is_replay_update_tablet_status_() const override { return false; }
  bool is_replay_update_mds_table_() const override { return true; }
  int do_replay_(ObTabletHandle &handle) override
  {
    return handle.get_obj()->get_tablet_meta().storage_layout_id_ != value_.layout_id_
        ? OB_STATE_NOT_MATCH : replay_to_mds_table_(handle, value_, ctx_, scn_);
  }
private:
  const TabletWriteLayout &value_;
  mds::MdsCtx &ctx_;
  SCN scn_;
};

int apply_write_layout(const char *buf, int64_t len, mds::BufferCtx &ctx, const SCN &scn, bool replay)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  ObTabletID tablet_id;
  ObArenaAllocator allocator("WriteLayoutMds");
  TabletWriteLayout value;
  ObLS *ls = nullptr;
  auto *service = share::server_service<ObLSService>();
  if (buf == nullptr || len <= 0 || service == nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tablet_id.deserialize(buf, len, pos))) {
  } else if (OB_FAIL(value.deserialize(allocator, buf, len, pos))) {
  } else if (pos != len || !tablet_id.is_valid()) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(service->get_ls(ls))) {
  } else if (replay) {
    WriteLayoutReplay executor(value, static_cast<mds::MdsCtx &>(ctx), scn);
    ret = executor.execute(scn, tablet_id);
    if (ret == OB_NO_NEED_UPDATE) { ret = OB_SUCCESS; }
  } else {
    ret = ls->get_tablet_svr()->set_write_layout(tablet_id, value, static_cast<mds::MdsCtx &>(ctx));
  }
  return ret;
}
}

int TabletWriteLayoutHelper::on_register(const char *buf, int64_t len, mds::BufferCtx &ctx)
{
  return apply_write_layout(buf, len, ctx, SCN::invalid_scn(), false);
}

int TabletWriteLayoutHelper::on_replay(const char *buf, int64_t len, const SCN &scn, mds::BufferCtx &ctx)
{
  return apply_write_layout(buf, len, ctx, scn, true);
}

namespace
{
int try_install_write_layout(ObTablet &tablet, const ObStorageSchema &schema,
    int64_t deadline)
{
  int ret = OB_SUCCESS;
  int64_t installed = -1;
  const auto &meta = tablet.get_tablet_meta();
  if (!schema.is_valid() || schema.is_column_info_simplified()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (tablet.is_ls_inner_tablet()) {
    // LS metadata has an engine-defined fixed schema and no MDS table.
  } else if (OB_FAIL(tablet.get_latest_committed<TabletWriteLayout>([&](const TabletWriteLayout &value) {
      installed = value.schema_.get_schema_version();
      return value.layout_id_ == meta.storage_layout_id_ ? OB_SUCCESS : OB_STATE_NOT_MATCH;
    }))) {
  } else if (installed < schema.get_schema_version()) {
    ObArenaAllocator allocator("WriteLayoutTx");
    TabletWriteLayout value;
    ObTxDesc *tx = nullptr;
    auto *service = share::server_service<ObTransService>();
    ObTxParam param;
    param.access_mode_ = ObTxAccessMode::RW;
    param.isolation_ = ObTxIsolationLevel::RC;
    param.timeout_us_ = deadline - ObTimeUtility::current_time();
    if (param.timeout_us_ <= 0) {
      ret = OB_TIMEOUT;
    } else if (service == nullptr) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(value.init(allocator, meta.storage_layout_id_, schema))) {
    } else if (OB_FAIL(service->acquire_tx(tx))) {
    } else if (OB_FAIL(service->start_tx(*tx, param))) {
    } else {
      const int64_t size = meta.tablet_id_.get_serialize_size() + value.get_serialize_size();
      char *buf = static_cast<char *>(allocator.alloc(size));
      int64_t pos = 0;
      ObRegisterMdsFlag flag;
      if (buf == nullptr) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(meta.tablet_id_.serialize(buf, size, pos))) {
      } else if (OB_FAIL(value.serialize(buf, size, pos))) {
      } else if (OB_FAIL(service->register_mds_into_tx(*tx, ObTxDataSourceType::TABLET_WRITE_LAYOUT,
          buf, pos, flag))) {
      } else {
        ret = service->commit_tx(*tx, deadline);
        if (OB_SUCC(ret)) {
          LOG_INFO("tablet write layout transaction committed", K(meta.tablet_id_),
              K(meta.storage_layout_id_), "schema_version", schema.get_schema_version());
        }
      }
    }
    if (tx != nullptr) {
      if (OB_FAIL(ret)) { (void)service->rollback_tx(*tx); }
      const int release_ret = service->release_tx(*tx);
      if (OB_SUCC(ret)) { ret = release_ret; }
    }
  }
  return ret;
}
}

int TabletWriteLayoutHelper::ensure_installed(ObTablet &tablet, const ObStorageSchema &schema,
    int64_t deadline)
{
  int ret = OB_SUCCESS;
  do {
    ret = try_install_write_layout(tablet, schema, deadline);
    if (ret == OB_EAGAIN || ret == OB_ERR_EXCLUSIVE_LOCK_CONFLICT) {
      if (ObTimeUtility::current_time() >= deadline) { ret = OB_TIMEOUT; }
      else { ob_usleep(1000); }
    }
  } while (ret == OB_EAGAIN || ret == OB_ERR_EXCLUSIVE_LOCK_CONFLICT);
  return ret;
}
}
}
