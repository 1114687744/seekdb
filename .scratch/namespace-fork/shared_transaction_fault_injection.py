#!/usr/bin/env python3
"""Inject/remove local shared-transaction fault points; never commit these hooks."""
import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser()
parser.add_argument('action', choices=('enable', 'disable'))
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
p = root / 'src/rootserver/fork_table/namespace_fork_kernel_prototype.cpp'
s = p.read_text()
s = re.sub(r'^[ \t]*// LOCAL_SHARED_TX_BEGIN\n.*?^[ \t]*// LOCAL_SHARED_TX_END\n', '', s, flags=re.M | re.S)
if args.action == 'enable':
    def hook(anchor, code, before=False):
        global s
        assert s.count(anchor) == 1, anchor
        block = '// LOCAL_SHARED_TX_BEGIN\n' + code + '\n// LOCAL_SHARED_TX_END\n'
        s = s.replace(anchor, block + anchor if before else anchor + block, 1)
    hook('int NamespaceForkKernelPrototype::ensure_tablet(const ObTabletID &tablet_id) {\n',
         '#include "' + str(Path(__file__).with_name('shared_transaction_fault_probe.ipp').resolve()) + '"', True)
    hook('layout_store.attach(layout_tx, native, deadline)))) {\n      } else {\n',
         '        fprintf(stderr, "SHARED_TX_NATIVE ns=%llu stage=attach tx=%ld ptr=%p\\n", (unsigned long long)db, native.get_tx_id().get_id(), &native);\n'
         '        ret = shared_transaction_fault("before_lock", db);\n        if (ret != OB_SUCCESS) { return ret; }')
    hook('  if (OB_SUCC(ret) && !already) {\n    failure_stage = "sources";\n',
         '  if (ret == OB_SUCCESS && !already) { ret = shared_transaction_fault("after_create", db); }', True)
    hook('  // Keep both participants and source protection alive until the native\n',
         '  if (ret == OB_SUCCESS && !already) { ret = shared_transaction_fault("after_owned", db); }', True)
    hook('    const int end = ret == OB_SUCCESS ? directory_kv_store()->commit(directory_tx)\n'
         '                                      : directory_kv_store()->rollback(directory_tx);\n'
         '    if (ret == OB_SUCCESS) { ret = end; }\n',
         '    if (ret == OB_SUCCESS && !already) { ret = shared_transaction_fault("after_commit", db); }')
p.write_text(s)
print('Shared transaction hooks', args.action)
# Force redo submission only for a deliberately armed test case. This exercises
# the native GC path that must wait for durable cleanup of an aborted CREATE.
observer = root / 'src/observer/ob_inner_sql_connection.cpp'
t = observer.read_text()
t = re.sub(r'^[ \t]*// LOCAL_SHARED_TX_BEGIN\n.*?^[ \t]*// LOCAL_SHARED_TX_END\n', '', t, flags=re.M | re.S)
if args.action == 'enable':
    anchor = '  return register_multi_data_source(\n      connection,\n'
    assert t.count(anchor) == 1
    code = '''// LOCAL_SHARED_TX_BEGIN
  const char *probe_control = getenv("SEEKDB_SHARED_TX_CONTROL");
  if (probe_control && access((std::string(probe_control) + ".flush_redo").c_str(), F_OK) == 0) {
    transaction::ObRegisterMdsFlag flag;
    flag.need_flush_redo_instantly_ = true;
    return register_multi_data_source(connection, type, buffer, buffer_size, flag);
  }
// LOCAL_SHARED_TX_END
'''
    t = t.replace(anchor, code + anchor, 1)
observer.write_text(t)

# Materialization now registers CREATE directly on its native transaction.
# Keep the SQL hook above for SQL DDL, and exercise the actual native path too.
creator = root / 'src/rootserver/ob_tablet_creator.cpp'
original = ('    int rc = service->register_mds_into_tx(\n'
            '        trans, transaction::ObTxDataSourceType::CREATE_TABLET_NEW_MDS, buf, size);\n')
text = re.sub(r'^// LOCAL_SHARED_TX_NATIVE_BEGIN\n.*?^// LOCAL_SHARED_TX_NATIVE_END\n',
              lambda _: original, creator.read_text(), flags=re.M | re.S)
if args.action == 'enable':
    assert text.count(original) == 1
    replacement = '''// LOCAL_SHARED_TX_NATIVE_BEGIN
    transaction::ObRegisterMdsFlag probe_flag;
    const char *probe_control = getenv("SEEKDB_SHARED_TX_CONTROL");
    if (probe_control && access((std::string(probe_control) + ".flush_redo").c_str(), F_OK) == 0) {
      probe_flag.need_flush_redo_instantly_ = true;
    }
    int rc = service->register_mds_into_tx(
        trans, transaction::ObTxDataSourceType::CREATE_TABLET_NEW_MDS, buf, size, probe_flag);
// LOCAL_SHARED_TX_NATIVE_END
'''
    text = text.replace(original, replacement, 1)
creator.write_text(text)
