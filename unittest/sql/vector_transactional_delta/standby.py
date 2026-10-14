#!/usr/bin/env python3
"""Focused vector replay/promotion test against an explicitly dedicated pair."""
import argparse
import json
from pathlib import Path
import time
from types import SimpleNamespace

from stream_bench import connect, sql


def run(args):
    records = []
    output = Path(args.output)
    def record(name, **values):
        records.append(dict(check=name, time=time.time(), **values))
        output.write_text(json.dumps(records, indent=2, default=str))
        print(json.dumps(records[-1], default=str), flush=True)
    p = connect(SimpleNamespace(port=args.primary_port))
    s = connect(SimpleNamespace(port=args.standby_port))
    table = f'`{args.database}`.docs'
    query = f"SELECT /*+ INDEX(docs idx) */ id,tag FROM {table} ORDER BY l2_distance(v,'[1,0,0]') APPROXIMATE LIMIT 1000"
    def await_rows(c, expected):
        deadline = time.monotonic() + 120
        last = None
        while True:
            try:
                last = sql(c, query)
                if sorted(last) == sorted(expected):
                    return
            except Exception as exc:
                last = repr(exc)
            if time.monotonic() > deadline:
                raise AssertionError((last, expected))
            time.sleep(.25)
    try:
        assert sql(p, 'SELECT role FROM oceanbase.__all_virtual_server_stat')[0][0] == 'PRIMARY'
        assert sql(s, 'SELECT role FROM oceanbase.__all_virtual_server_stat')[0][0] == 'STANDBY'
        sql(p, f'CREATE DATABASE `{args.database}`')
        sql(p, f"CREATE TABLE {table} (id INT,tag INT,v VECTOR(3),VECTOR INDEX idx(v) "
               "WITH(distance=l2,type=hnsw,lib=vsag,sync_mode=async)) ORGANIZATION HEAP")
        sql(p, f"INSERT INTO {table} VALUES(1,10,'[1,0,0]'),(2,20,'[2,0,0]')")
        await_rows(p, [(1,10),(2,20)])
        await_rows(s, [(1,10),(2,20)])
        record('standby_replayed_insert_searchable')
        sql(p, 'BEGIN')
        sql(p, f"UPDATE {table} SET tag=21,v='[2,1,0]' WHERE id=2")
        sql(p, f'DELETE FROM {table} WHERE id=1')
        sql(p, f"INSERT INTO {table} VALUES(3,30,'[3,0,0]')")
        sql(p, 'COMMIT')
        await_rows(s, [(2,21),(3,30)])
        record('standby_replayed_update_delete')
        # Fence old primary before promoting. This pair is not reused as a
        # replication topology after the test.
        sql(p, 'ALTER SYSTEM SWITCHOVER TO STANDBY')
        sql(s, 'ALTER SYSTEM SWITCHOVER TO PRIMARY')
        assert sql(s, 'SELECT role FROM oceanbase.__all_virtual_server_stat')[0][0] == 'PRIMARY'
        sql(s, f"INSERT INTO {table} VALUES(4,40,'[4,0,0]')")
        sql(s, f"UPDATE {table} SET tag=31,v='[3,1,0]' WHERE id=3")
        await_rows(s, [(2,21),(3,31),(4,40)])
        record('promotion_resumes_durable_delta_consumption')
        record('complete')
    except Exception as exc:
        record('failure', error=repr(exc))
        raise
    finally:
        p.close()
        s.close()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--primary-port', type=int, required=True)
    p.add_argument('--standby-port', type=int, required=True)
    p.add_argument('--database', required=True)
    p.add_argument('--output', required=True)
    a = p.parse_args()
    assert a.database.startswith('vecdelta_') and a.database.replace('_', '').isalnum()
    run(a)
