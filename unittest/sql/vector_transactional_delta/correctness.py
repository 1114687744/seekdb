#!/usr/bin/env python3
"""Focused SQL correctness probes; also usable against the unchanged baseline."""
import argparse
import json
from pathlib import Path
import time

from stream_bench import connect, sql


def run(args):
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    records = []

    def record(name, **values):
        records.append({"check": name, "time": time.time(), **values})
        output.write_text(json.dumps(records, indent=2, default=str))
        print(json.dumps(records[-1], default=str), flush=True)

    c = connect(args)
    sql(c, f"CREATE DATABASE `{args.database}`")
    sql(c, f"USE `{args.database}`")
    sql(c, "CREATE TABLE docs(id INT, tag INT, v VECTOR(3), VECTOR INDEX idx(v) "
           "WITH(distance=l2,type=hnsw,lib=vsag,sync_mode=async)) ORGANIZATION HEAP")
    query = "SELECT /*+ INDEX(docs idx) */ id,tag FROM docs ORDER BY l2_distance(v,'[1,0,0]') APPROXIMATE LIMIT 100"
    record("plan", rows=sql(c, "EXPLAIN " + query))

    def await_rows(expected, connection=c, timeout=40):
        deadline = time.monotonic() + timeout
        while True:
            actual = sorted(sql(connection, query))
            if actual == sorted(expected):
                return
            if time.monotonic() >= deadline:
                raise AssertionError((actual, expected))
            time.sleep(.1)

    try:
        sql(c, "INSERT INTO docs VALUES(1,10,'[1,0,0]'),(2,20,'[2,0,0]')")
        await_rows([(1, 10), (2, 20)])
        record("insert_eventually_visible")
        reader = connect(args)
        sql(reader, f"USE `{args.database}`")
        sql(c, "BEGIN")
        sql(c, "INSERT INTO docs VALUES(3,30,'[3,0,0]')")
        sql(c, "UPDATE docs SET tag=21,v='[2,1,0]' WHERE id=2")
        sql(c, "DELETE FROM docs WHERE id=1")
        time.sleep(1)
        await_rows([(1, 10), (2, 20)], reader)
        sql(c, "ROLLBACK")
        await_rows([(1, 10), (2, 20)], reader)
        record("uncommitted_and_rolled_back_changes_invisible")

        sql(reader, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ")
        sql(reader, "BEGIN")
        assert sorted(sql(reader, "SELECT id,tag FROM docs")) == [(1, 10), (2, 20)]
        await_rows([(1, 10), (2, 20)], reader)
        sql(c, "BEGIN")
        sql(c, "UPDATE docs SET tag=21,v='[2,1,0]' WHERE id=2")
        sql(c, "DELETE FROM docs WHERE id=1")
        sql(c, "INSERT INTO docs VALUES(3,30,'[3,0,0]')")
        sql(c, "COMMIT")
        await_rows([(2, 21), (3, 30)])
        await_rows([(1, 10), (2, 20)], reader)
        sql(reader, "COMMIT")
        await_rows([(2, 21), (3, 30)], reader)
        record("update_delete_and_old_repeatable_read")

        sql(c, "BEGIN")
        sql(c, "INSERT INTO docs VALUES(4,40,'[4,0,0]')")
        sql(c, "UPDATE docs SET tag=41,v='[4,1,0]' WHERE id=4")
        sql(c, "DELETE FROM docs WHERE id=4")
        sql(c, "UPDATE docs SET tag=31,v='[3,1,0]' WHERE id=3")
        sql(c, "UPDATE docs SET tag=32,v='[3,2,0]' WHERE id=3")
        sql(c, "COMMIT")
        await_rows([(2, 21), (3, 32)])
        record("multiple_dml_in_one_transaction")

        sql(c, "CALL dbms_vector.refresh_index('idx','docs','v',1,'FAST')")
        await_rows([(2, 21), (3, 32)])
        record("explicit_refresh_barrier")
        record("complete", rows=sql(c, "SELECT id,tag FROM docs ORDER BY id"))
        reader.close()
    except Exception as exc:
        record("failure", error=repr(exc))
        raise
    finally:
        c.close()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--database", required=True)
    p.add_argument("--output", required=True)
    args = p.parse_args()
    if not args.database.startswith("vecdelta_") or not args.database.replace("_", "").isalnum():
        p.error("use a new vecdelta_ database")
    run(args)
