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

The host also runs unrelated instances. Do not stop them. Record CPU allocation
and competing load; do not claim an isolated hardware benchmark.

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
- Performance runs are in progress on the user-provided clean host 6.13.50.37.
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
