# Freshness experiment evidence

See [the report](../../vector-transactional-delta-freshness.md) for conditions,
interpretation and limits. Final C++ source: `37a7bc65b`.

- `summary.json`: ten measured runs; includes controls and unsuccessful intermediate versions.
- `checks.json`: three failing comparisons followed by four passing final comparisons.
- `build-manifest.json`: source/binary hashes and intermediate patch mapping.
- `raw-manifest.json`: hashes of complete remote request/resource files. Small metadata,
  summary, freshness, backlog and error files are also saved in each run subdirectory.
- `fresh8correctness.json`: focused DML, rollback, snapshot and older-VID delete checks.
- `fresh8-range-plans.json`: separate failed EXPLAIN diagnostic; an ordinary client cannot
  reference the hidden key. This is not a captured successful execution plan.
- `diagnostic-timing.json`: temporary phase counters from `latprobe3`; not a formal
  performance run. Timing is elapsed coordinator time, not aggregate worker CPU time.

Diagnostic patches are artifacts, not applied product code:

- Apply `master-eight-workers.patch` to master `76f871eb5` for the eight-executor control.
- Apply each of `perf6.patch`, `perf7.patch`, `perf8.patch` independently to `ed44b77d8`.
  They respectively remove unconditional waits; also try greater small-batch concurrency
  (rejected); or bound transfer SQL while keeping the original pool task granularity.
- Apply `diagnostic-timing.patch` after `perf6.patch` to reproduce the temporary counters.
  The final C++ source contains no instrumentation.

Full remote results and binaries remain under
`/data/1/nijia.nj/test/vector-delta-perf-20261015`. Experiment instances are stopped.
The exact failing comparison can be rerun with `compare.py` against `perf5base`/`perf5delta`
or `fresh6_async`/`fresh6_delta`; the final comparisons use `fresh8`/`confirm8`/`low8`.
