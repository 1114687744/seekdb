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

#include "storage/ddl/ob_tablet_copy_util.h"
#include "storage/ddl/ob_ddl_storage_util.h"

#include "storage/blocksstable/ob_sstable.h"
#include "storage/ob_storage_schema.h"
#include "storage/tablet/ob_tablet.h"
#include "storage/tablet/ob_tablet_create_sstable_param.h"

namespace oceanbase
{
namespace storage
{

int ObTabletCopyUtil::check_need_fill_empty_sstable(
    ObLS *ls,
    const bool is_minor_sstable,
    const ObITable::TableKey &table_key,
    const common::ObTabletID &dst_tablet_id,
    bool &need_fill_empty_sstable,
    share::SCN &end_scn)
{
  int ret = OB_SUCCESS;
  ObTabletHandle dst_tablet_handle;
  ObTabletMemberWrapper<ObTabletTableStore> table_store_handle;
  need_fill_empty_sstable = false;
  end_scn.reset();

  if (is_minor_sstable) {
    if (OB_FAIL(ObDDLStorageUtil::ddl_get_tablet(ls, dst_tablet_id, dst_tablet_handle,
        ObMDSGetTabletMode::READ_ALL_COMMITED))) {
    } else if (OB_FAIL(dst_tablet_handle.get_obj()->fetch_table_store(table_store_handle))) {
    } else {
      ObITable *first_dst_table = table_store_handle.get_member()->get_minor_sstables().get_boundary_table(false/*is_last*/);
      const share::SCN dst_start_scn = nullptr != first_dst_table
          ? first_dst_table->get_key().get_start_scn()
          : dst_tablet_handle.get_obj()->get_tablet_meta().clog_checkpoint_scn_;
      if (table_key.get_end_scn() < dst_start_scn) {
        need_fill_empty_sstable = true;
        end_scn = dst_start_scn;
      }
    }
  }
  return ret;
}

int ObTabletCopyUtil::build_create_empty_sstable_param(
    const blocksstable::ObSSTableMeta &meta,
    const ObITable::TableKey &table_key,
    const common::ObTabletID &dst_tablet_id,
    const share::SCN &end_scn,
    ObTabletCreateSSTableParam &create_sstable_param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!meta.is_valid() || !table_key.is_valid() || !dst_tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(meta), K(table_key), K(dst_tablet_id));
  } else if (OB_FAIL(create_sstable_param.init_for_empty_minor_sstable(
      dst_tablet_id,
      table_key.get_end_scn()/*start_scn*/,
      end_scn,
      meta))) {
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
