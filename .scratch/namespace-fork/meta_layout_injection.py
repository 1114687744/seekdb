#!/usr/bin/env python3
"""Force small real meta merges and trace the selected physical definition."""
import argparse
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('action', choices=('enable', 'disable'))
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
changes = (
    ('src/storage/compaction/ob_tablet_scheduler.cpp',
     '    if (::oceanbase::share::server_service<::oceanbase::storage::ObTabletStatMgr>()->contain_extreme_tablet()) {',
     '''    if (const char *path = getenv("SEEKDB_META_LAYOUT_TARGET")) {
      uint64_t target = 0;
      if (FILE *file = fopen(path, "r")) {
        (void)fscanf(file, "%lu", &target);
        fclose(file);
      }
      if (target == tablet_id.id()) {
        bool created = false;
        return schedule_tablet_meta_merge(ls, tablet_handle, created);
      }
    }
'''),
    ('src/storage/compaction/ob_partition_merge_policy.cpp',
     '    if (OB_FAIL(ret)) {\n    } else if (scanty_tx_determ_table || scanty_inc_row_cnt) {',
     '''    if (const char *path = getenv("SEEKDB_META_LAYOUT_TARGET")) {
      uint64_t target = 0;
      if (FILE *file = fopen(path, "r")) {
        (void)fscanf(file, "%lu", &target);
        fclose(file);
      }
      if (target == tablet.get_tablet_id().id()) {
        scanty_inc_row_cnt = false;
      }
    }
'''),
    ('src/storage/compaction/ob_basic_tablet_merge_ctx.cpp',
     '    FLOG_INFO("get storage schema to meta merge", "param", get_dag_param(), KPC_(static_param_.schema));\n',
     '''    if (getenv("SEEKDB_META_LAYOUT_TARGET")) {
      storage::ObStorageSchema *local = nullptr;
      int probe_ret = tablet->load_write_layout(mem_ctx_.get_allocator(), local);
      fprintf(stderr, "META_LAYOUT_SELECTED tablet=%lu G=%lu local=%ld selected=%ld snapshot=%ld full=%d ret=%d\\n",
          get_tablet_id().id(), tablet->get_tablet_meta().storage_layout_id_,
          local == nullptr ? -1 : local->get_schema_version(), static_param_.schema_->get_schema_version(),
          static_param_.version_range_.snapshot_version_, !static_param_.schema_->is_column_info_simplified(), probe_ret);
      if (local != nullptr) { ObStorageSchemaUtil::free_storage_schema(mem_ctx_.get_allocator(), local); }
      fflush(stderr);
    }
'''),
)
for relative, anchor, code in changes:
    path = root / relative
    text = re.sub(r'^// LOCAL_META_LAYOUT_BEGIN\n.*?^// LOCAL_META_LAYOUT_END\n', '',
                  path.read_text(), flags=re.M | re.S)
    if a.action == 'enable':
        assert text.count(anchor) == 1, relative
        text = text.replace(anchor, '// LOCAL_META_LAYOUT_BEGIN\n' + code +
                            '// LOCAL_META_LAYOUT_END\n' + anchor)
    path.write_text(text)
print('Meta layout hooks', a.action)
