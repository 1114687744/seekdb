#!/usr/bin/env python3
"""Observe real mini/fork schema installation in disposable test builds."""
import argparse
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('action', choices=('enable', 'disable'))
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
changes = (
    ('src/storage/compaction/ob_basic_tablet_merge_ctx.cpp',
     '    FLOG_INFO("get storage schema to merge", "param", get_dag_param(), KPC_(static_param_.schema), K(schema_on_tablet),\n',
     '''    if (getenv("SEEKDB_LOCAL_SCHEMA_INSTALL_PROBE")) {
      fprintf(stderr, "LOCAL_SCHEMA_MINI tablet=%lu before=%ld observed=%ld result=%ld simplified=%d before_columns=%ld observed_columns=%ld ret=%d\\n",
          get_tablet_id().id(), schema_on_tablet.get_schema_version(), max_schema_version_in_memtable,
          static_param_.schema_->get_schema_version(), static_param_.schema_->is_column_info_simplified(),
          column_cnt_in_schema, max_column_cnt_in_memtable, ret);
      fflush(stderr);
    }
'''),
    ('src/storage/tablet/ob_tablet.cpp',
     '    LOG_INFO("succeeded to init tablet with local batch tables", K(ret), K(param), K(old_tablet), KPC(this));\n',
     '''    if (getenv("SEEKDB_LOCAL_SCHEMA_INSTALL_PROBE")) {
      ObArenaAllocator probe_allocator("LayoutProbe");
      ObStorageSchema *probe_before = nullptr, *probe_after = nullptr;
      int probe_ret = old_tablet.load_write_layout(probe_allocator, probe_before);
      if (probe_ret == OB_SUCCESS) { probe_ret = load_write_layout(probe_allocator, probe_after); }
      fprintf(stderr, "LOCAL_SCHEMA_FORK tablet=%lu before=%ld incoming=%ld result=%ld simplified=%d ret=%d\\n",
          get_tablet_id().id(), probe_before == nullptr ? -1 : probe_before->get_schema_version(),
          storage_schema->get_schema_version(), probe_after == nullptr ? -1 : probe_after->get_schema_version(),
          probe_after == nullptr ? 1 : probe_after->is_column_info_simplified(), probe_ret);
      ObTabletObjLoadHelper::free(probe_allocator, probe_before);
      ObTabletObjLoadHelper::free(probe_allocator, probe_after);
      fflush(stderr);
    }
'''),
)
for relative, anchor, code in changes:
    path = root / relative
    text = re.sub(r'^[ \t]*// LOCAL_SCHEMA_INSTALL_BEGIN\n.*?^[ \t]*// LOCAL_SCHEMA_INSTALL_END\n',
                  '', path.read_text(), flags=re.M | re.S)
    if a.action == 'enable':
        assert text.count(anchor) == 1, relative
        text = text.replace(anchor, '// LOCAL_SCHEMA_INSTALL_BEGIN\n' + code +
                            '// LOCAL_SCHEMA_INSTALL_END\n' + anchor)
    path.write_text(text)
print('Local schema installation hooks', a.action)
