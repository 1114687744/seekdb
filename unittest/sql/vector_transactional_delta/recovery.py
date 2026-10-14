#!/usr/bin/env python3
"""Crash/rebuild probes; only operates on an explicitly named experiment instance."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import pymysql

from stream_bench import connect, sql


def run(args):
    base = Path(args.base_dir).resolve()
    assert str(base).startswith('/data/1/nijia.nj/test/vector-delta-')
    output = Path(args.output)
    records = []

    def record(name, **values):
        records.append(dict(check=name, time=time.time(), **values))
        output.write_text(json.dumps(records, indent=2, default=str))
        print(json.dumps(records[-1], default=str), flush=True)

    def connection():
        c = connect(args)
        sql(c, f'USE `{args.database}`')
        return c

    def restart(sig):
        pid = int((base / 'run/seekdb.pid').read_text())
        cmdline = Path(f'/proc/{pid}/cmdline').read_bytes()
        assert str(base).encode() in cmdline
        os.kill(pid, sig)
        deadline = time.monotonic() + 30
        while Path(f'/proc/{pid}').exists():
            if time.monotonic() > deadline:
                raise TimeoutError('stop')
            time.sleep(.1)
        with output.with_suffix('.startup.log').open('a') as log:
            subprocess.run([str(Path(args.binary).resolve()), '--base-dir='+str(base),
                            '-P'+str(args.port), '--log-level=WARN'], stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        deadline = time.monotonic() + 120
        while True:
            try:
                c = connection()
                sql(c, 'SELECT 1')
                return c
            except Exception:
                if time.monotonic() > deadline:
                    raise
                time.sleep(.5)

    query = "SELECT /*+ INDEX(docs idx) */ id,tag FROM docs ORDER BY l2_distance(v,'[1,0,0]') APPROXIMATE LIMIT 1000"
    expected = {i: i for i in range(1, 101)}

    def await_expected(c):
        deadline = time.monotonic() + 90
        while True:
            rows = sql(c, query)
            if len(rows) == len(expected) and dict(rows) == expected:
                return
            if time.monotonic() > deadline:
                raise AssertionError((rows, expected))
            time.sleep(.2)

    c = connect(args)
    try:
        sql(c, f'CREATE DATABASE `{args.database}`')
        sql(c, f'USE `{args.database}`')
        sql(c, "CREATE TABLE docs(id INT,tag INT,v VECTOR(3),VECTOR INDEX idx(v) "
               "WITH(distance=l2,type=hnsw,lib=vsag,sync_mode=async)) ORGANIZATION HEAP")
        sql(c, 'INSERT INTO docs VALUES ' + ','.join(f"({i},{i},'[{i},0,0]')" for i in expected))
        await_expected(c)
        record('initial_visible')
        # Crash immediately after a commit, while another connection owns an
        # uncommitted transaction. Recovery must discover durable work unaided.
        sql(c, "INSERT INTO docs VALUES(101,101,'[101,0,0]')")
        expected[101] = 101
        sql(c, 'BEGIN')
        sql(c, "INSERT INTO docs VALUES(102,102,'[102,0,0]')")
        sql(c, "UPDATE docs SET tag=-1,v='[-1,0,0]' WHERE id=1")
        c = restart(signal.SIGKILL)
        assert dict(sql(c, 'SELECT id,tag FROM docs')) == expected
        await_expected(c)
        record('crash_committed_recovered_uncommitted_discarded')

        def rebuild():
            r = connection()
            try:
                deadline = time.monotonic() + 90
                while True:
                    try:
                        sql(r, "CALL dbms_vector.rebuild_index('idx','docs','v')")
                        break
                    except pymysql.err.OperationalError as exc:
                        if exc.args[0] != 4179 or time.monotonic() >= deadline:
                            raise
                        record('rebuild_busy_retry', error=str(exc))
                        time.sleep(.3)
            finally:
                r.close()

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            rebuilding = pool.submit(rebuild)
            for i in range(1, 21):
                sql(c, f"UPDATE docs SET tag={i+1000},v='[{i},1,0]' WHERE id={i}")
                expected[i] = i+1000
            rebuilding.result(timeout=120)
        await_expected(c)
        sql(c, 'SET ob_enable_index_direct_select=1')
        aux = sql(c, "SELECT a.table_name FROM oceanbase.__all_table a JOIN oceanbase.__all_table t "
                  "ON a.data_table_id=t.table_id JOIN oceanbase.__all_database d ON t.database_id=d.database_id "
                  f"WHERE d.database_name='{args.database}' AND t.table_name='docs' AND a.table_name LIKE '__idx_%'")
        counts = {name: sql(c, f'SELECT COUNT(*) FROM `{name}`')[0][0] for (name,) in aux}
        record('rebuild_with_concurrent_updates', auxiliary_rows=counts)
        c.close()
        c = restart(signal.SIGTERM)
        await_expected(c)
        record('graceful_restart_after_rebuild')
        record('complete', rows=len(expected))
    except Exception as exc:
        record('failure', error=repr(exc))
        raise
    finally:
        c.close()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', type=int, required=True)
    p.add_argument('--database', required=True)
    p.add_argument('--base-dir', required=True)
    p.add_argument('--binary', required=True)
    p.add_argument('--output', required=True)
    args = p.parse_args()
    assert args.database.startswith('vecdelta_') and args.database.replace('_', '').isalnum()
    run(args)
