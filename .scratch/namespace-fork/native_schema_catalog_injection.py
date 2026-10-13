#!/usr/bin/env python3
"""Test-only cold Namespace native schema reads; disable for production."""
import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('action', choices=('enable', 'disable'))
args = parser.parse_args()
local = Path(__file__).resolve().parent
source = local.parents[1] / 'src/rootserver/fork_table/namespace_fork_kernel_prototype.cpp'
text = re.sub(r'^[ \t]*// NATIVE_SCHEMA_CATALOG_PROBE_BEGIN\n.*?^[ \t]*// NATIVE_SCHEMA_CATALOG_PROBE_END\n',
              '', source.read_text(), flags=re.M | re.S)
if args.action == 'enable':
    def before(anchor, code):
        global text
        assert text.count(anchor) == 1, anchor
        text = text.replace(anchor, '// NATIVE_SCHEMA_CATALOG_PROBE_BEGIN\n' + code +
                            '\n// NATIVE_SCHEMA_CATALOG_PROBE_END\n' + anchor, 1)
    before('#include "namespace/catalog.h"\n',
           '#include "observer/schema/native_schema_catalog.h"\n#include "share/schema/schema_catalog_row.h"\n#include "share/schema/ob_trigger_info.h"\n#include "storage/tx/ob_trans_service.h"')
    before('int NamespaceForkKernelPrototype::control_namespace(const ObString &source, const ObString &target,\n    uint64_t &id, bool allow_login) {\n',
           '#include "' + str(local / 'native_schema_catalog_probe.ipp') + '"')
    before('  LOG_INFO("PROTOTYPE_NAMESPACE_KV_REGISTER", K(ret), K(id),\n',
           '  if (ret == OB_SUCCESS) { ret = probe_native_schema_catalog(id, target_name); }')
source.write_text(text)
source = local.parents[1] / 'src/observer/mysql/obmp_query.cpp'
text = re.sub(r'^[ \t]*// NATIVE_SCHEMA_CATALOG_PROBE_BEGIN\n.*?^[ \t]*// NATIVE_SCHEMA_CATALOG_PROBE_END\n',
              '', source.read_text(), flags=re.M | re.S)
if args.action == 'enable':
    before('    // Execute under the bound namespace and refresh its schema before the command.\n',
           '    if (sql_.prefix_match("SELECT /* native_request_deadline */")) {\n'
           '      THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() - 1);\n'
           '      LOG_INFO("NATIVE_REQUEST_DEADLINE injected expired prior request");\n'
           '    }')
source.write_text(text)
print('Native schema catalog test hooks', args.action)
