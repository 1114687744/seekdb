#!/usr/bin/env python3
"""Characterize an old snapshot opened before async-index visibility.

This records a known limitation in both baseline and prototype. It is separate
from correctness.py, whose old reader starts after initial index visibility.
"""
import argparse
import json
from pathlib import Path
import time
from stream_bench import connect, sql


def run(args):
    c, reader = connect(args), connect(args)
    records = []
    try:
        sql(c, f'CREATE DATABASE `{args.database}`')
        sql(c, f'USE `{args.database}`')
        sql(reader, f'USE `{args.database}`')
        sql(c, "CREATE TABLE docs(id INT,v VECTOR(3),VECTOR INDEX idx(v) "
               "WITH(distance=l2,type=hnsw,lib=vsag,sync_mode=async)) ORGANIZATION HEAP")
        sql(c, "INSERT INTO docs VALUES(1,'[1,0,0]')")
        sql(reader, 'SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ')
        sql(reader, 'BEGIN')
        assert sql(reader, 'SELECT id FROM docs') == ((1,),)
        sql(c, 'DELETE FROM docs WHERE id=1')
        start = time.monotonic()
        for _ in range(30):
            result = sql(reader, "SELECT /*+ INDEX(docs idx) */ id FROM docs "
                         "ORDER BY l2_distance(v,'[1,0,0]') APPROXIMATE LIMIT 10")
            records.append(dict(seconds=time.monotonic()-start, ann=result))
            time.sleep(1)
        records.append(dict(full_scan=sql(reader, 'SELECT id FROM docs')))
        Path(args.output).write_text(json.dumps(records, indent=2))
        print(json.dumps(records[-2:]), flush=True)
    finally:
        reader.close()
        c.close()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', type=int, required=True)
    p.add_argument('--database', required=True)
    p.add_argument('--output', required=True)
    a = p.parse_args()
    assert a.database.startswith('vecdelta_') and a.database.replace('_','').isalnum()
    run(a)
