#!/usr/bin/env python3
"""Hook durable native catalog retention checks after Namespace bootstrap."""
import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('action', choices=('enable', 'disable'))
args = parser.parse_args()
local = Path(__file__).resolve().parent
source = local.parents[1] / 'src/rootserver/fork_table/namespace_fork_kernel_prototype.cpp'
text = source.read_text()
text = re.sub(r'^[ \t]*// LOCAL_LAYOUT_RETENTION_BEGIN\n.*?^[ \t]*// LOCAL_LAYOUT_RETENTION_END\n',
              '', text, flags=re.M | re.S)


def block(body):
    return '// LOCAL_LAYOUT_RETENTION_BEGIN\n' + body + '// LOCAL_LAYOUT_RETENTION_END\n'


if args.action == 'enable':
    includes = '#include "namespace/catalog.h"\n'
    entry = 'int NamespaceForkKernelPrototype::ensure_control_schema(bool initial_install) {\n'
    recovery = '    return observer::namespace_worker_prototype::complete_namespace_schema_bootstrap(*schema_service);\n'
    ready = '  LOG_INFO("PROTOTYPE_NAMESPACE_CONTROL_SCHEMA", K(ret));\n'
    for anchor in (includes, entry, recovery, ready):
        if text.count(anchor) != 1:
            raise SystemExit('Retention hook moved: ' + anchor)
    text = text.replace(includes, includes + block(
        '#include "observer/schema/native_schema_catalog.h"\n#include "share/schema/schema_catalog_row.h"\n'
        '#include "storage/tx/ob_trans_service.h"\n'
        '#include "storage/compaction/ob_compaction_schedule_util.h"\n'
        '#include "storage/compaction/ob_partition_merge_policy.h"\n'
        '#include "share/ob_global_merge_table_operator.h"\n'
        '#include "share/ob_merge_info.h"\n'))
    text = text.replace(entry, block('#include "' + str(local / 'layout_retention_native_probe.ipp') + '"\n') + entry)
    text = text.replace(recovery, block(
        '    if (getenv("SEEKDB_LAYOUT_RETENTION_PROBE") != nullptr) {\n'
        '      int rc = observer::namespace_worker_prototype::complete_namespace_schema_bootstrap(*schema_service);\n'
        '      return rc == OB_SUCCESS ? run_layout_retention_native_probe() : rc;\n'
        '    }\n') + recovery)
    text = text.replace(ready, block(
        '  if (ret == OB_SUCCESS && getenv("SEEKDB_LAYOUT_RETENTION_PROBE") != nullptr) {\n'
        '    ret = run_layout_retention_native_probe();\n'
        '  }\n') + ready)
source.write_text(text)
policy = local.parents[1] / 'src/storage/compaction/ob_freeze_info_mgr.cpp'
text = policy.read_text()
text = re.sub(r'^[ \t]*// LOCAL_LAYOUT_RETENTION_BEGIN\n.*?^[ \t]*// LOCAL_LAYOUT_RETENTION_END\n',
              '', text, flags=re.M | re.S)
if args.action == 'enable':
    namespace = 'namespace oceanbase\n{\n'
    sample = '    share::SCN readers;\n'
    if text.count(namespace) != 1 or text.count(sample) != 1:
        raise SystemExit('Retention policy hook moved')
    text = text.replace(namespace, block(
        'namespace oceanbase { namespace storage { int run_layout_retention_sample_hook(); } }\n') + namespace)
    text = text.replace(sample, block(
        '    if (OB_SUCC(ret)) { ret = storage::run_layout_retention_sample_hook(); }\n') + sample)
policy.write_text(text)
print('Native catalog retention probe ' + args.action + 'd')
