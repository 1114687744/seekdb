#!/usr/bin/env python3
"""Complete write layouts survive replay, mini and divergent fork file takeover."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import resource
import threading
import time

from fork_parent_truncate_probe import BootstrapExperiment, connect, namespace_id, physical_id


def run(binary):
    exp = BootstrapExperiment(binary, 'owned_layout', prototype=6)
    exp.extra_parameters = [('ob_compaction_schedule_interval', '3s')]
    child = None

    def wait_for(predicate, message, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            assert exp.proc.poll() is None, ('process exited', exp.base)
            if predicate():
                return
            time.sleep(.3)
        raise AssertionError((message, str(exp.base)))

    def crash():
        nonlocal child
        if child:
            child.close()
            child = None
        exp.connection.close()
        exp.connection = None
        exp.proc.kill()
        exp.proc.wait(timeout=15)
        exp.record('crash_for_recovery', pid=exp.proc.pid)
        exp.start()

    def dump(tablets):
        bound = int(exp.sql('SELECT current_scn()', log=False)[0][0])
        wait_for(lambda: int(exp.sql('SELECT weak_read_scn FROM oceanbase.__all_virtual_ls_info',
                                   log=False)[0][0]) >= bound, 'weak snapshot behind writes')
        exp.sql('ALTER SYSTEM MINOR FREEZE')
        ids = ','.join(map(str, tablets))
        wait_for(lambda: len(exp.sql('SELECT DISTINCT tablet_id FROM oceanbase.V$OB_SSTABLES '
            f"WHERE tablet_id IN ({ids}) AND table_type IN ('MINI','MINOR')", log=False)) == len(tablets),
            'post-DDL data did not produce a complete-schema file')

    try:
        exp.start()
        exp.sql('CREATE DATABASE owned_layout')
        exp.sql('CREATE TABLE owned_layout.t(id INT PRIMARY KEY, v INT DEFAULT 3)')
        exp.sql('INSERT INTO owned_layout.t(id) VALUES(1)')
        exp.sql('ALTER TABLE owned_layout.t ALTER COLUMN v SET DEFAULT 9')
        exp.sql('INSERT INTO owned_layout.t(id) VALUES(2)')
        exp.sql('ALTER TABLE owned_layout.t ADD COLUMN extra INT DEFAULT 7')
        exp.sql('INSERT INTO owned_layout.t VALUES(3,30,8)')
        exp.sql('ALTER TABLE owned_layout.t ALTER COLUMN extra SET DEFAULT 11')
        exp.sql('INSERT INTO owned_layout.t(id) VALUES(4)')
        expected = ((1,3,7), (2,9,7), (3,30,8), (4,9,11))
        assert exp.sql('SELECT * FROM owned_layout.t ORDER BY id') == expected
        # Crash before a forced dump: the installed descriptions must replay
        # independently of the later row transactions and the SQL services.
        crash()
        assert exp.sql('SELECT * FROM owned_layout.t ORDER BY id') == expected
        logical = int(exp.sql("SELECT tablet_id FROM oceanbase.__all_table WHERE table_name='t' "
            "AND database_id=(SELECT database_id FROM oceanbase.__all_database "
            "WHERE database_name='owned_layout')", log=False)[0][0])
        exp.sql('UPDATE owned_layout.t SET v=v+1')
        expected = tuple((i,v+1,e) for i,v,e in expected)
        dump([physical_id(1, logical)])
        crash()
        assert exp.sql('SELECT * FROM owned_layout.t ORDER BY id') == expected
        exp.record('PASS', case='owned_layout_replay_and_dump', rows=expected)

        # All writers initially see the same older installed layout. Their
        # metadata installation must serialize without failing user DML.
        exp.sql('CREATE TABLE owned_layout.concurrent_writes(id INT PRIMARY KEY, v INT)')
        exp.sql('ALTER TABLE owned_layout.concurrent_writes ADD COLUMN extra INT DEFAULT 17')
        barrier = threading.Barrier(8)

        def insert(i):
            with connect(exp, 'root') as conn:
                barrier.wait(timeout=30)
                with conn.cursor() as cursor:
                    cursor.execute('INSERT INTO owned_layout.concurrent_writes(id,v) VALUES(%s,%s)', (i,i))

        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(insert, range(1,9)))
        exp.sql('ALTER TABLE owned_layout.concurrent_writes ADD COLUMN unused INT DEFAULT 23')
        exp.sql('BEGIN')
        exp.sql('INSERT INTO owned_layout.concurrent_writes(id,v) VALUES(9,9)')
        exp.sql('ROLLBACK')
        assert exp.sql('SELECT COUNT(*),SUM(extra),SUM(unused) FROM owned_layout.concurrent_writes') \
            == ((8,136,184),)
        exp.record('PASS', case='owned_layout_concurrent_install_and_rollback')

        exp.sql('FORK NAMESPACE owned_layout_child FROM ns1')
        ns = namespace_id(exp, 'owned_layout_child')
        child = connect(exp, 'root@owned_layout_child')
        exp.sql('ALTER TABLE owned_layout.t ADD COLUMN fork_col VARCHAR(30) DEFAULT \'child\'', child)
        exp.sql('ALTER TABLE owned_layout.t ADD COLUMN parent_col INT DEFAULT 99')
        exp.sql('INSERT INTO owned_layout.t(id,v,fork_col) VALUES(5,50,\'own\')', child)
        exp.sql('INSERT INTO owned_layout.t(id,v,parent_col) VALUES(6,60,100)')
        root_rows = tuple(row + (99,) for row in expected) + ((6,60,11,100),)
        child_rows = tuple(row + ('child',) for row in expected) + ((5,50,11,'own'),)
        assert exp.sql('SELECT * FROM owned_layout.t ORDER BY id') == root_rows
        assert exp.sql('SELECT * FROM owned_layout.t ORDER BY id', child) == child_rows
        tablet = physical_id(ns, logical)
        dump([physical_id(1, logical), tablet])
        wait_for(lambda: exp.sql('SELECT ref_tablet_id FROM oceanbase.__all_virtual_tablet_info '
            f'WHERE tablet_id={tablet}', log=False) == ((0,),), 'fork did not finish file takeover')
        crash()
        child = connect(exp, 'root@owned_layout_child')
        assert exp.sql('SELECT * FROM owned_layout.t ORDER BY id') == root_rows
        assert exp.sql('SELECT * FROM owned_layout.t ORDER BY id', child) == child_rows
        exp.record('PASS', case='owned_layout_divergent_fork', root=root_rows, child=child_rows)
    finally:
        if child:
            child.close()
        exp.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    run(args.binary)
