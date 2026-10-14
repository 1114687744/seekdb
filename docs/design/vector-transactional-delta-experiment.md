# Transactional vector delta experiment

## Scope

Validate asynchronous HNSW maintenance independently of Namespace fork. Base:
`76f871eb5a84b664d487b03b3412ce50194135b5` (master fetched 2026-10-15).
Worktree: `/data/1/nijia.nj/herdr-worktree/seekdb/vector-transactional-delta`.
Branch: `codex/vector-transactional-delta`.

This is an experiment, not evidence that Namespace fork integration is complete.
The Namespace branch and its running instances are not test targets.

## Compared implementations

1. Unmodified master, heap table, HNSW ASYNC, ChangeStream enabled.
2. Experimental master, same heap table and HNSW parameters: main row and delta
   record commit in the same transaction; background processing constructs the
   graph and transfers/cleans durable delta records. ChangeStream must not also
   maintain these indexes.
3. Unmodified master, same heap table, HNSW IMMEDIATE.

Foreground delta insertion must not call VSAG graph insertion. Durable records
are authoritative; notifications are optional wakeups. Recovery must work without
reconstructing an in-memory notification queue. Standby replays durable records
and reconstructs local graph state without independently changing replicated
tables; promotion resumes primary maintenance.

## Existing mechanisms being evaluated for reuse

- Delta has `(vid, type)` as its key. Existing synchronous insertion clears the
  vector payload after foreground graph insertion. VID changes on heap vector
  update; recovery retrieves visible vectors from the main table at the same
  read snapshot as the auxiliary records.
- Refresh transfers `(original commit SCN, vid, type, partition columns)` to
  index_id and deletes delta records in one transaction.
- Persisted graph snapshots and covered-record cleanup already have a
  transactional implementation. The experiment must exercise this lifecycle,
  rather than measuring an append-only queue that never drains.
- Existing adapter recovery reconstructs from snapshot, delta, index_id and
  main table. Its async bitmap and snapshot rules need verification when used
  as the background consumer.

## Correctness gates

- Insert/update/delete, commit/rollback, multiple DML in one transaction.
- An open or aborted transaction never becomes searchable through the index.
- Eventual visibility with no new writes and no manual refresh per batch.
- Explicit refresh barrier, old repeatable-read snapshots and concurrent writes.
- Cleanup/rebuild, graceful restart and forced process restart after commit.
- Standby replay and promotion: replicated rows remain authoritative and reads
  recover without a local primary commit callback.

## Streaming measurements

Use cached Cohere 100k/768d vectors from
`/home/nijia.nj/benchmarks/quantization_20260914`; cosine, M=16,
ef_construction=200, ef_search=128, identical write batches and resources.
Create the index on an empty heap table before writes. Run several offered write
rates, concurrent approximate queries, and a drain phase after writes stop.

Record actual committed rows/s; write and search P50/P95/P99; search throughput;
recall; commit-to-searchable lag; pending work and drain time; process CPU/RSS
and I/O. Keep raw outputs, failures, execution plans and exact binary identity.
Ground truth during insertion must use the committed prefix, not all future
rows. Account for queries racing a commit when calculating recall.

The cached VectorDBBench OceanBase driver currently creates a primary-key table,
builds the index only in `optimize()` and injects parallel DML hints. Those defaults
cannot be used unchanged for this experiment. A dedicated streaming harness may
reuse the dataset and workload shape, with its deviations explicitly reported.

Local instances share a host with unrelated workloads. Formal measurements use
6.13.50.37 with fixed CPU affinities. Unrelated remote processes are left running;
host load is sampled. This is a controlled experiment, not a hardware benchmark.

## Status (2026-10-15)

- Both binaries built from the pinned master. The original Namespace worktree
  and its running instances were not changed.
- Baseline and prototype passed insert/update/delete, transaction rollback,
  repeatable-read and explicit refresh checks (`correctness.py`).
- Prototype passed forced restart with committed and uncommitted work, rebuild
  with concurrent updates, and graceful restart after the rebuild (`recovery.py`).
  Snapshot table contained a persisted graph; delta and index_id were empty.
- Prototype passed standby replay of insert/update/delete and promotion followed
  by fresh writes (`standby.py`). Primary RPC 15922, standby RPC 15932; SQL ports
  15921/15931. These are dedicated experiment instances.
- Without any ANN query, three inserts were transferred and the background graph
  had three points (GDB `obvectorutil::get_index_number`, returned success).
  Virtual `incr_cnt` is follower synchronization statistics, not a live count.
- Six formal performance runs completed on the user-provided clean host 6.13.50.37.
  Results and the decision are in [the results report](vector-transactional-delta-results.md).
  Each formal arm uses a fresh instance and waits 12 seconds after creating the
  empty index before writes begin. Earlier cold-start runs are retained separately.
  Server CPU affinity 48–55, client 56–59; 8 configured server CPUs, 6 GiB budget.
  Existing unrelated processes are left running and host load is sampled.
  Binaries were stripped of debug information only. Dependencies and cached
  dataset were copied directly over SSH; no external dataset download was needed.

## Failures found during implementation

1. Initial delta transfer returned `OB_TABLE_NOT_EXIST`: an ordinary internal SQL
   session cannot write vector auxiliary tables. Use the existing dummy DDL
   visibility flag on the transfer transaction's session, and reset it before
   returning the connection. The original failing correctness run is retained.
2. Using LS weak-read SCN on primary added several seconds of visibility delay.
   Primary now asks the injected transaction service for a fresh read snapshot;
   standby continues to use the replay-safe readable snapshot. Focused checks
   passed after this change.
3. One rebuild attempt returned 4179 (another refresh/rebuild may be running).
   The harness retains this failure and allows bounded retries for that specific
   documented busy error. A subsequent run completed rebuild and recovery.
4. Initial adapter completion still relies on the existing 10-second maintenance
   schedule. This is a prototype limitation; report cold-start visibility
   separately from steady-state throughput/latency.

## Measurement limits

Freshness is sampled: first positive ANN probe gives an upper bound on visibility
lag, with 0.5-second polling plus probe scheduling delay. Keep censored samples.
The fixed drain phase is 30 seconds; auxiliary counts and final recall are recorded,
not a claim that every possible query is correct or that every task queue is empty.
During-write summary must distinguish initial empty-index responses from later
queries; empty responses can inflate QPS. Raw queries include time and committed
prefix so separate windows can be calculated.

This prototype reuses ChangeStream's lifecycle holder but does not start its
fetcher/dispatcher/worker. Their old virtual statistics are not consumer progress.
The consumer periodically scans schema and durable records; commit notifications
and production scheduler integration are outside the current proof.

## Additional boundary and design findings

- `snapshot_boundary.py`: open a repeatable-read snapshot immediately after insert,
  before asynchronous index visibility, then delete from another connection.
  For 30 seconds the old ordinary read returned the row while ANN returned none,
  on **both** unmodified master and prototype. This is not a passing completeness
  test. The earlier successful old-snapshot gate waits for index visibility first.
  Its semantics must be specified before claiming arbitrary historical ANN reads.
- The prototype calls `refresh_adp_from_table` for each consumption pass. Its
  `write_into_delta_mem` holds `incr_data_->mem_data_rwlock_` exclusively across
  a whole graph insertion batch; ANN takes the corresponding read lock. Existing
  ChangeStream's graph append uses a different path. This is a concrete contention
  opportunity, not yet a measured attribution of all latency loss.
- A local debugger captured both `ReqWorker` and `VecDelta` entering this graph
  fill path for the same adapter and 1000-row batches. Merely changing the lock
  to shared would lose the existing protection around duplicate checking and
  insertion. No such lock weakening was made. The attempted duration probe was
  interrupted by another breakpoint, so its printed duration is invalid and is
  not used as performance evidence. Debugger runs are excluded from benchmarks.
- Production work should give incremental append and recovery clear ownership,
  bound batch size and lock duration, and ensure adapter metadata is ready without
  waiting for a periodic maintenance tick. Preserve durable records as the source
  of work; a notification should only reduce discovery latency.

## Reproducing the experiment

Build from the pinned base using the compile skill. Preserve the unmodified binary
before building the prototype. `build-manifest.json` records exact source and binary
hashes; both performance binaries were made with `llvm-strip --strip-debug`.

Focused checks (each needs a fresh database name):

```sh
python3 unittest/sql/vector_transactional_delta/correctness.py --port 15891 --database vecdelta_correctness --output correctness.json
python3 unittest/sql/vector_transactional_delta/recovery.py --port 15891 --database vecdelta_recovery --base-dir /data/1/nijia.nj/test/vector-delta-prototype-20261015 --binary /path/to/seekdb-delta3 --output recovery.json
python3 unittest/sql/vector_transactional_delta/standby.py --primary-port 15921 --standby-port 15931 --database vecdelta_standby --output standby.json
```

The primary/standby probe follows the existing obtest standby protocol: enable RPC,
start standby with `--role STANDBY` and `log_restore_source=primary_ip:rpc_port`,
fence the old primary with `SWITCHOVER TO STANDBY`, promote with `SWITCHOVER TO
PRIMARY`. It exercises a targeted vector case; the full obtest suite was not run.

Remote layout: `/data/1/nijia.nj/test/vector-delta-perf-20261015` contains `bin`,
`data`, `python`, `scripts`, `results` and dedicated instance directories. With
Python modules supplied through `PYTHONPATH`, `perf_matrix.py --root <layout>
--label <new_label>` runs six fresh instances sequentially. It preserves data and
logs and stops only the processes it started. A test invocation uses:

```sh
OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 taskset -c 56-59 python3.8 stream_bench.py --port 15881 --pid <server_pid> --database vecdelta_example --mode async --rows 100000 --queries 100 --rate 1000 --settle-seconds 12 --data <layout>/data --output <layout>/results/example
```

`analyze.py <run directories...> --output <json>` also reports the matched 20k–80k
committed-row window. CPU/RSS describe the combined write plus query workload;
a lower CPU value with lower query throughput is not evidence of better efficiency.
The drain phase continues issuing queries and is not an idle measurement.
