#!/usr/bin/env python3
"""Focused real-engine reproduction of a minor row chain missing Last.

Exit 1 when the engine reports the missing Last flag. Passing also requires a
completed minor for the affected child tablet and correct parent/child data.
A quiet run with no completed target minor fails as inconclusive.
"""
import argparse
import json
import resource
import time

from fork_parent_truncate_probe import BootstrapExperiment, connect, namespace_id, physical_id


class Trace:
    def __init__(self, exp):
        self.exp, self.positions, self.failures, self.completed_minors = exp, {}, [], set()

    def read(self):
        for path in (self.exp.base / 'log').glob('seekdb.log*'):
            if '.wf' in path.name:
                continue
            inode = path.stat().st_ino
            with path.open('rb') as stream:
                stream.seek(self.positions.get(inode, 0))
                while True:
                    pos = stream.tell()
                    line = stream.readline()
                    if not line or not line.endswith(b'\n'):
                        stream.seek(pos)
                        break
                    if b'meet another rowkey but not meet Last flag' in line:
                        self.failures.append(line.decode(errors='replace').strip())
                    if b'dag finished(dag_ret=0' in line and b'MINOR_MERGE' in line:
                        self.completed_minors.add(line.decode(errors='replace').strip())
                self.positions[inode] = stream.tell()
        return self.failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--seconds', type=int, default=90)
    parser.add_argument('--skip-seed', action='store_true')
    parser.add_argument('--skip-major', action='store_true')
    parser.add_argument('--pre-fork-wait', type=int, default=0)
    parser.add_argument('--skip-indexes', action='store_true')
    parser.add_argument('--hold-failure', type=int, default=0)
    parser.add_argument('--core-only', action='store_true')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    exp = BootstrapExperiment(args.binary, 'minor_last_flag', prototype=6)
    exp.extra_parameters = [('ob_compaction_schedule_interval', '3s')]
    child = None
    try:
        exp.start()
        exp.connection._read_timeout = 330
        exp.sql('USE oceanbase')
        exp.sql('SET ob_query_timeout=300000000')
        exp.sql('SET ob_trx_timeout=300000000')
        trace = Trace(exp)
        if not args.skip_seed:
            exp.sql('CREATE DATABASE replica_ns')
            exp.sql('CREATE TABLE replica_ns.t(id INT PRIMARY KEY,v INT,note LONGTEXT)')
            exp.sql("INSERT INTO replica_ns.t VALUES(1,10,REPEAT('baseline',2048))")
            exp.sql('FORK NAMESPACE replica_seed FROM ns1')
        if args.pre_fork_wait:
            exp.record('wait_before_next_fork', seconds=args.pre_fork_wait)
            time.sleep(args.pre_fork_wait)
        if not args.skip_seed:
            exp.sql('DROP NAMESPACE replica_seed')
        exp.sql('CREATE DATABASE layout_replay')
        if not args.core_only:
            exp.sql('CREATE TABLE layout_replay.t(id INT PRIMARY KEY,v INT) '
                'PARTITION BY RANGE(id) (PARTITION p0 VALUES LESS THAN(100),PARTITION p1 VALUES LESS THAN(200))')
        if not args.core_only and not args.skip_indexes:
            exp.sql('CREATE INDEX by_local_v ON layout_replay.t(v) LOCAL')
            exp.sql('CREATE UNIQUE INDEX by_global_v ON layout_replay.t(v) GLOBAL PARTITION BY HASH(v) PARTITIONS 2')
        if not args.core_only:
            exp.sql('INSERT INTO layout_replay.t VALUES(1,10),(2,20),(120,30)')
        exp.sql('FORK NAMESPACE layout_replay_child FROM ns1')
        child = connect(exp, 'root@layout_replay_child')
        child._read_timeout = 330
        exp.sql('SET ob_query_timeout=300000000', child)
        exp.sql('SET ob_trx_timeout=300000000', child)
        if args.core_only:
            exp.sql('CREATE DATABASE child_local_db', child)
        else:
            exp.sql('ALTER TABLE layout_replay.t ADD COLUMN parent_only INT DEFAULT 7')
            exp.sql('ALTER TABLE layout_replay.t ADD COLUMN child_only INT DEFAULT 9', child)
            exp.sql('UPDATE layout_replay.t SET v=v+100', child)
        owner = namespace_id(exp, 'layout_replay_child')
        tablet = physical_id(owner, 1)
        exp.sql('ALTER SYSTEM MINOR FREEZE')
        if not args.skip_major:
            exp.sql('ALTER SYSTEM MAJOR FREEZE')
        until = time.monotonic() + args.seconds
        while time.monotonic() < until:
            if trace.read():
                break
            time.sleep(.3)
        trace.read()
        completed = [line for line in trace.completed_minors
                     if f'tablet_id:{{id:{tablet}}}' in line]
        result = dict(case='minor_last_flag', namespace=owner, tablet=tablet,
                      failures=trace.failures, target_minor_completed=len(completed),
                      options=vars(args))
        (exp.base / 'log' / 'minor-verdict.json').write_text(json.dumps(result, indent=2) + '\n')
        exp.record('REPRODUCED' if trace.failures else 'MINOR_COMPLETED' if completed else 'INCONCLUSIVE', **result)
        if trace.failures and args.hold_failure:
            exp.record('hold_failure_for_debugger', seconds=args.hold_failure)
            time.sleep(args.hold_failure)
        assert not trace.failures, 'minor missing Last flag (see minor-verdict.json)'
        assert completed, 'target minor did not complete; a quiet log is not a pass'
        if not args.core_only:
            assert exp.sql('SELECT id,v,child_only FROM layout_replay.t ORDER BY id', child) == ((1,110,9),(2,120,9),(120,130,9))
            assert exp.sql('SELECT id,v,parent_only FROM layout_replay.t ORDER BY id') == ((1,10,7),(2,20,7),(120,30,7))
        exp.record('PASS', **result)
    finally:
        if child is not None:
            child.close()
        exp.close()


if __name__ == '__main__':
    main()
