#!/usr/bin/env python3
"""Fork rewrite preserves partial updates, NULL, deletion and old row versions."""
import argparse
import hashlib
import re
import resource

from fork_parent_truncate_probe import BootstrapExperiment, connect, namespace_id, physical_id
from minor_last_flag_probe import Trace
from physical_merge_layout_probe import wait_for


def payload(version):
    return ''.join(hashlib.sha256(f'{version}/{block}'.encode()).hexdigest() for block in range(32))


def run(binary):
    exp = BootstrapExperiment(binary, 'fork_snapshot_rows', prototype=6)
    exp.extra_parameters = [('ob_compaction_schedule_interval', '3s')]
    child = pin = writer = None
    try:
        exp.start()
        exp.sql('CREATE DATABASE snapshot_rows')
        exp.sql('CREATE TABLE snapshot_rows.t(id INT PRIMARY KEY,a INT,b INT,pad VARCHAR(4096))')
        exp.sql("INSERT INTO snapshot_rows.t VALUES(1,10,20,'base'),(2,10,20,'base'),"
                "(3,10,20,'base'),(4,10,20,'base'),(5,10,20,'base')")
        logical = int(exp.sql("SELECT tablet_id FROM oceanbase.__all_table WHERE table_name='t' "
            "AND database_id=(SELECT database_id FROM oceanbase.__all_database "
            "WHERE database_name='snapshot_rows')")[0][0])
        source = physical_id(1, logical)
        exp.sql('ALTER SYSTEM MINOR FREEZE')
        wait_for(lambda: exp.sql("SELECT tablet_id FROM oceanbase.V$OB_SSTABLES "
            f"WHERE tablet_id={source} AND table_type IN ('MINI','MINOR')", log=False),
            'initial source mini missing')

        # Keep the pre-update snapshot alive so the hot row retains a version
        # chain much larger than a micro block instead of only its latest value.
        pin = connect(exp, 'root')
        exp.sql('SET TRANSACTION ISOLATION LEVEL REPEATABLE READ', pin)
        exp.sql('START TRANSACTION WITH CONSISTENT SNAPSHOT', pin)
        assert exp.sql('SELECT a,b,pad FROM snapshot_rows.t WHERE id=1', pin) == ((10,20,'base'),)
        for version in range(1, 97):
            column = 'a' if version % 2 else 'b'
            exp.sql(f"UPDATE snapshot_rows.t SET {column}={version},"
                    f"pad='{payload(version)}' WHERE id=1", log=False)
        exp.sql('UPDATE snapshot_rows.t SET a=22 WHERE id=2')
        exp.sql('DELETE FROM snapshot_rows.t WHERE id=3')
        exp.sql('UPDATE snapshot_rows.t SET b=NULL WHERE id=4')

        # The source memtable contains a transaction committing after the fork
        # SCN. Its SSTable must be rewritten, even if dumping starts immediately.
        writer = connect(exp, 'root')
        exp.sql('BEGIN', writer)
        exp.sql('UPDATE snapshot_rows.t SET a=555 WHERE id=5', writer)
        exp.sql('FORK NAMESPACE snapshot_rows_child FROM ns1')
        exp.sql('COMMIT', writer)
        writer.close()
        writer = None
        exp.sql('UPDATE snapshot_rows.t SET a=-1,b=-1 WHERE id=1')
        exp.sql('DELETE FROM snapshot_rows.t WHERE id=2')
        exp.sql("INSERT INTO snapshot_rows.t VALUES(3,333,333,'after fork')")
        ns = namespace_id(exp, 'snapshot_rows_child')
        target = physical_id(ns, logical)
        child = connect(exp, 'root@snapshot_rows_child')
        exp.sql("INSERT INTO snapshot_rows.t VALUES(6,60,60,'child')", child)
        trace = Trace(exp)
        exp.sql('ALTER SYSTEM MINOR FREEZE')
        query = 'SELECT id,a,b,LENGTH(pad),MD5(pad) FROM snapshot_rows.t ORDER BY id'
        md5 = lambda value: hashlib.md5(value.encode()).hexdigest()
        expected = ((1,95,96,2048,md5(payload(96))), (2,22,20,4,md5('base')),
                    (4,10,None,4,md5('base')), (5,10,20,4,md5('base')),
                    (6,60,60,5,md5('child')))
        assert exp.sql(query, child) == expected
        assert exp.sql('SELECT a,b,pad FROM snapshot_rows.t WHERE id=1', pin) == ((10,20,'base'),)
        # Require a real minor of the inherited and local rows, not just a
        # successful query before merging. current_scn() is a read snapshot;
        # a dumped file's log boundary need not reach that later timestamp.
        def merged():
            assert not trace.read(), trace.failures[:1]
            return any(f'tablet_id:{{id:{target}}}' in line for line in trace.completed_minors)
        wait_for(merged, 'target minor missing', 90)
        assert exp.sql(query, child) == expected
        rewrites = []
        source_blocks = []
        for path in (exp.base / 'log').glob('seekdb.log*'):
            with path.open(errors='replace') as stream:
                for line in stream:
                    if ('fork rewrite: successfully created sstable' in line
                            and f'param_->dest_tablet_id_={{id:{target}}}' in line):
                        rewrites.append(line)
                    if ('sstable merge finish' in line
                            and f'static_info:{{tablet_id:{{id:{source}}}, merge_type:"MINI_MERGE"' in line):
                        source_blocks.extend((int(rows), int(blocks)) for rows, blocks in
                            re.findall(r'row_count_:(\d+).*?data_micro_block_count_:(\d+)', line))
        assert rewrites, 'test did not rewrite the user SSTable'
        assert any(rows >= 97 and blocks > 2 for rows, blocks in source_blocks), source_blocks
        exp.record('snapshot_rows_verified', target=target, versions=96,
                   source_blocks=source_blocks, rewrites=len(rewrites))
        exp.sql('ROLLBACK', pin)
        pin.close()
        pin = None

        child.close()
        child = None
        exp.connection.close()
        exp.connection = None
        exp.proc.kill()
        exp.proc.wait(timeout=15)
        exp.start()
        child = connect(exp, 'root@snapshot_rows_child')
        assert exp.sql(query, child) == expected
        assert not trace.read(), trace.failures[:1]
        exp.record('PASS', case='fork_snapshot_rows', target_minor=True,
                   partial_update=True, delete=True, null=True,
                   snapshot_cut=True, crash_recovery=True)
    finally:
        for con in (child, pin, writer):
            if con is not None and con.open:
                con.close()
        exp.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    run(args.binary)
