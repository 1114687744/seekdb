#!/usr/bin/env python3
"""Streaming HNSW experiment against a dedicated, already running instance.

Uses the cached Cohere corpus, not the unmodified VectorDBBench driver (which
builds a primary-key table and creates its index after loading). All results,
including failed requests, are saved. No refresh is issued during measurement.
"""

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import threading
import time

import numpy as np
import psutil
import pymysql


def connect(args):
    conn = pymysql.connect(host="127.0.0.1", port=args.port, user="root@sys",
                           autocommit=True, connect_timeout=10,
                           read_timeout=120, write_timeout=120)
    with conn.cursor() as cur:
        cur.execute("SET ob_query_timeout=120000000")
        cur.execute("SET ob_trx_timeout=120000000")
        cur.execute("SET ob_hnsw_ef_search=128")
    return conn


def sql(conn, statement):
    with conn.cursor() as cur:
        cur.execute(statement)
        return cur.fetchall()


def vector(value):
    return "X'" + np.asarray(value, dtype="<f4").tobytes().hex() + "'"


def percentiles(values):
    return dict(zip(["p50", "p95", "p99", "max"],
                    np.percentile(values, [50, 95, 99, 100]).tolist())) if values else {}


def prepare(args):
    conn = connect(args)
    # A fresh database name prevents accidental deletion/reuse of another run.
    sql(conn, f"CREATE DATABASE `{args.database}`")
    sql(conn, f"CREATE TABLE `{args.database}`.docs (id BIGINT, emb VECTOR(768), "
        "VECTOR INDEX idx(emb) WITH(distance=cosine,type=hnsw,lib=vsag,"
        f"m=16,ef_construction=200,sync_mode={args.mode})) ORGANIZATION HEAP")
    metadata = {"version": sql(conn, "SELECT version()"),
                "ddl": sql(conn, f"SHOW CREATE TABLE `{args.database}`.docs"),
                "parameters": sql(conn, "SHOW PARAMETERS WHERE name IN "
                    "('memory_budget','cpu_count','vector_memory_limit','log_disk_size')")}
    query = f"SELECT /*+ INDEX(docs idx) */ id FROM `{args.database}`.docs ORDER BY "
    query += f"cosine_distance(emb,{vector(np.ones(768))}) APPROXIMATE LIMIT 10"
    metadata["plan"] = sql(conn, "EXPLAIN " + query)
    metadata["auxiliary_tables"] = sql(conn,
        "SELECT a.table_name FROM oceanbase.__all_table a "
        "JOIN oceanbase.__all_table t ON a.data_table_id=t.table_id "
        "JOIN oceanbase.__all_database d ON t.database_id=d.database_id "
        f"WHERE d.database_name='{args.database}' AND t.table_name='docs' "
        "AND a.table_name LIKE '__idx_%'")
    conn.close()
    return metadata


def run(args):
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    base = np.load(Path(args.data) / "base.npy", mmap_mode="r")[:args.rows]
    queries = np.load(Path(args.data) / "queries.npy", mmap_mode="r")[:args.queries]
    ids = np.load(Path(args.data) / "ids.npy", mmap_mode="r")[:args.rows]
    assert len(base) == args.rows and base.shape[1] == 768
    meta = prepare(args)
    if args.settle_seconds:
        time.sleep(args.settle_seconds)
    ready = connect(args)
    meta["vector_info_before_writes"] = sql(ready, "SELECT * FROM oceanbase.__all_virtual_vector_index_info")
    ready.close()
    meta.update(vars(args))
    meta["host_cpu_count"] = os.cpu_count()
    meta["host_load_before"] = os.getloadavg()
    process = psutil.Process(args.pid) if args.pid else None
    if process:
        meta["server_exe"] = process.exe()
        meta["server_cmdline"] = process.cmdline()
        meta["server_affinity"] = process.cpu_affinity()
    (out / "metadata.json").write_text(json.dumps(meta, indent=2, default=str))

    # Precompute scores before timing. During writes the expected answer is
    # restricted to the committed prefix at query start. Save returned IDs and
    # both prefix boundaries so races with commits can be inspected afterwards.
    normalized = np.asarray(base).copy()
    normalized /= np.linalg.norm(normalized, axis=1, keepdims=True)
    nq = np.asarray(queries).copy()
    nq /= np.linalg.norm(nq, axis=1, keepdims=True)
    scores = nq @ normalized.T
    query_sql = [f"SELECT /*+ INDEX(docs idx) */ id FROM `{args.database}`.docs "
                 f"ORDER BY cosine_distance(emb,{vector(q)}) APPROXIMATE LIMIT 10"
                 for q in queries]
    state = {"committed": 0, "writer_done": False}
    stop = threading.Event()
    writes, reads, probes, resources, errors, backlog = [], [], [], [], [], []
    start = time.monotonic()

    def writer():
        conn = connect(args)
        try:
            for lo in range(0, len(base), args.batch):
                due = start + lo / args.rate
                if due > time.monotonic():
                    time.sleep(due - time.monotonic())
                hi = min(lo + args.batch, len(base))
                values = ",".join(f"({int(ids[i])},{vector(base[i])})" for i in range(lo, hi))
                t0 = time.monotonic()
                sql(conn, f"INSERT INTO `{args.database}`.docs VALUES " + values)
                t1 = time.monotonic()
                state["committed"] = hi
                writes.append({"lo": lo, "hi": hi, "start_s": t0-start,
                               "commit_s": t1-start, "latency_ms": (t1-t0)*1000,
                               "schedule_lag_ms": max(0, t0-due)*1000})
                if t1 - start > args.write_timeout:
                    raise TimeoutError("write phase exceeded limit")
        except Exception as exc:
            errors.append({"phase": "write", "error": repr(exc)})
        finally:
            state["writer_done"] = True
            conn.close()

    def reader(worker):
        conn = connect(args)
        seq = worker
        try:
            while not stop.is_set():
                n0 = state["committed"]
                if n0 < max(10, args.batch):
                    stop.wait(.05)
                    continue
                qi = seq % len(queries)
                t0 = time.monotonic()
                rows = sql(conn, query_sql[qi])
                t1 = time.monotonic()
                n1 = state["committed"]
                expected = ids[np.argpartition(-scores[qi, :n0], 9)[:10]]
                actual = [int(row[0]) for row in rows]
                reads.append({"worker": worker, "query": qi, "start_s": t0-start,
                              "end_s": t1-start, "latency_ms": (t1-t0)*1000,
                              "prefix_start": n0, "prefix_end": n1, "ids": actual,
                              "recall_at_start": len(set(actual) & set(expected))/10,
                              "after_write": state["writer_done"]})
                seq += args.readers
        except Exception as exc:
            errors.append({"phase": "search", "error": repr(exc)})
        finally:
            conn.close()

    def monitor():
        conn = connect(args)
        seen = set()
        pending = {}
        inspected = 0
        next_aux = 0.0
        sample_stride = max(1, (args.rows + args.batch - 1) // args.batch // 50)
        sql(conn, "SET ob_enable_index_direct_select=1")
        try:
            while not stop.is_set():
                if process:
                    io = process.io_counters()._asdict()
                    workers = []
                    for thread in process.threads():
                        try:
                            name = Path(f"/proc/{process.pid}/task/{thread.id}/comm").read_text().strip()
                        except FileNotFoundError:  # Adaptive worker exited during sampling.
                            continue
                        if name.startswith(("VecBuild", "VecDelta", "CSWorker")):
                            workers.append(dict(thread._asdict(), name=name))
                    resources.append({"time_s": time.monotonic()-start,
                                      "rss": process.memory_info().rss,
                                      "cpu": process.cpu_times()._asdict(), "io": io,
                                      "background_workers": workers,
                                      "host_load": os.getloadavg()})
                current = list(writes)
                for bi in range(inspected, len(current)):
                    batch = current[bi]
                    if bi % sample_stride == 0 or batch["hi"] == args.rows:
                        pending[batch["hi"] - 1] = batch
                inspected = len(current)
                # Keep probing sampled commits until visible; do not discard an
                # old sample merely because a newer batch has committed.
                for position, batch in list(pending.items())[:8]:
                    statement = f"SELECT /*+ INDEX(docs idx) */ id FROM `{args.database}`.docs "
                    statement += f"ORDER BY cosine_distance(emb,{vector(base[position])}) APPROXIMATE LIMIT 10"
                    actual = [int(row[0]) for row in sql(conn, statement)]
                    visible = int(ids[position]) in actual
                    probes.append({"position": position, "commit_s": batch["commit_s"],
                                   "probe_s": time.monotonic()-start, "visible": visible})
                    if visible:
                        seen.add(position)
                        del pending[position]
                if time.monotonic() >= next_aux:
                    counts = {}
                    for (name,) in meta["auxiliary_tables"]:
                        counts[name] = sql(conn, f"SELECT COUNT(*) FROM `{args.database}`.`{name}`")[0][0]
                    backlog.append({"time_s": time.monotonic()-start,
                                    "committed": state["committed"], "rows": counts,
                                    "pending_freshness_samples": len(pending)})
                    next_aux = time.monotonic() + 2
                stop.wait(.5)
        except Exception as exc:
            errors.append({"phase": "monitor", "error": repr(exc)})
        finally:
            conn.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.readers + 2) as pool:
        futures = [pool.submit(reader, i) for i in range(args.readers)]
        futures.append(pool.submit(monitor))
        pool.submit(writer).result()
        write_end = time.monotonic()
        # Continue concurrent reads and freshness probes after the last commit.
        time.sleep(args.drain_seconds)
        stop.set()
        for future in futures:
            future.result()
    elapsed = time.monotonic() - start
    conn = connect(args)
    final = {"rows": sql(conn, f"SELECT COUNT(*) FROM `{args.database}`.docs"),
             "vector_info": sql(conn, "SELECT * FROM oceanbase.__all_virtual_vector_index_info")}
    sql(conn, "SET ob_enable_index_direct_select=1")
    final["auxiliary_rows"] = {
        name: sql(conn, f"SELECT COUNT(*) FROM `{args.database}`.`{name}`")[0][0]
        for (name,) in meta["auxiliary_tables"]}
    final["recall"] = []
    for qi, statement in enumerate(query_sql):
        actual = [int(row[0]) for row in sql(conn, statement)]
        n = state["committed"]
        if n >= 10:
            expected = ids[np.argpartition(-scores[qi, :n], 9)[:10]]
            final["recall"].append(len(set(actual) & set(expected))/10)
    conn.close()
    for name, records in [("writes", writes), ("queries", reads), ("freshness", probes),
                          ("resources", resources), ("errors", errors), ("backlog", backlog)]:
        with (out / (name + ".jsonl")).open("w") as stream:
            for record in records:
                stream.write(json.dumps(record, default=str) + "\n")
    during = [row for row in reads if row["end_s"] <= write_end-start]
    stable = [row for row in during if row["prefix_start"] == row["prefix_end"]]
    visible = {}
    for probe in probes:
        if probe["visible"] and probe["position"] not in visible:
            visible[probe["position"]] = (probe["probe_s"] - probe["commit_s"]) * 1000
    sample_stride = max(1, (args.rows + args.batch - 1) // args.batch // 50)
    expected_samples = {b["hi"]-1 for i,b in enumerate(writes) if i % sample_stride == 0 or b["hi"] == args.rows}
    censored = sorted(expected_samples - set(visible))
    summary = {"freshness_upper_bound_ms": percentiles(list(visible.values())),
               "freshness_samples": len(visible), "freshness_censored_positions": censored,
               "sampled_catchup_after_last_commit_s": max(0, max((p["probe_s"] for p in probes if p["visible"]), default=0) - (writes[-1]["commit_s"] if writes else 0)),
               "committed": state["committed"], "elapsed_s": elapsed,
               "write_phase_s": write_end-start,
               "actual_write_rows_s": state["committed"]/(write_end-start),
               "write_latency_ms": percentiles([row["latency_ms"] for row in writes]),
               "schedule_lag_ms": percentiles([row["schedule_lag_ms"] for row in writes]),
               "search_qps_during_write": len(during)/(write_end-start),
               "search_latency_ms": percentiles([row["latency_ms"] for row in during]),
               "recall_stable_prefix": float(np.mean([r["recall_at_start"] for r in stable])) if stable else None,
               "stable_prefix_samples": len(stable), "errors": errors, "final": final}
    (out / "summary.json").write_text(json.dumps(summary, indent=2, default=str))
    print(json.dumps(summary, default=str), flush=True)
    if (errors or state["committed"] != args.rows or not final["recall"]
            or float(np.mean(final["recall"])) < args.minimum_final_recall):
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--database", required=True)
    parser.add_argument("--mode", choices=["async", "immediate"], default="async")
    parser.add_argument("--data", default="/home/nijia.nj/benchmarks/quantization_20260914")
    parser.add_argument("--rows", type=int, default=100000)
    parser.add_argument("--queries", type=int, default=100)
    parser.add_argument("--rate", type=int, default=1000)
    parser.add_argument("--batch", type=int, default=100)
    parser.add_argument("--readers", type=int, default=2)
    parser.add_argument("--drain-seconds", type=int, default=30)
    parser.add_argument("--settle-seconds", type=int, default=0)
    parser.add_argument("--minimum-final-recall", type=float, default=.98)
    parser.add_argument("--write-timeout", type=int, default=600)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"vecdelta_[a-zA-Z0-9_]+", args.database):
        parser.error("use a new database name starting with vecdelta_")
    if min(args.rows, args.queries, args.rate, args.batch, args.readers) <= 0 or args.rows < 10:
        parser.error("positive sizes and at least 10 rows are required")
    run(args)
