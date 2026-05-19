# Performance findings (contention sweep, 12 GB)

Primary data: `results/perf-20260519T051541Z/` (report.md, *.png,
results.csv, versions.lock). Diagnosis evidence:
`results/perf-20260518T195958Z/mysql.DEATH.txt`.

SUT A = MySQL 9.7 + tidesdb-mysql v0.2.1 · SUT B = MariaDB 12.3.1 +
upstream tidesql · **shared TidesDB v9.2.0** · identical caps
(4 CPU / **12 GB**) + identical benchmarking server config (README §3).
Cells = median `[min–max]` over 5 reps. Matrix: 4 workloads ×
table_size {1k,100k,1M} × threads {1,8,32}.

## 1. Headline quantitative result — point-select (read path), full A vs B

The one workload with a complete, clean A-vs-B matrix across the whole
sweep. **MySQL + tidesdb-mysql consistently outperforms MariaDB +
upstream tidesql on the same TidesDB core**, by a median ≈1.4–1.5×
(range 1.21–1.88×):

| table_size | t=1 | t=8 | t=32 |
|---|---|---|---|
| 1k   | 1.25× | 1.41× | 1.44× |
| 100k | 1.69× | 1.54× | 1.40× |
| 1M   | 1.88× | 1.49× | 1.21× |

Both run identical TidesDB v9.2.0, so this gap is attributable to the
server + integration glue (handler/optimizer/connection path), not the
storage engine. p95 is also lower on A throughout (e.g. 1M t=8:
0.44 ms vs 0.57 ms). Only outlier: 1k t=1 A is *slightly* lower in the
min of its range (6.9k) — a tiny-dataset cold effect that the 5-rep
median (10.5k vs 8.4k) still resolves in A's favor.

## 2. Write contention — the OCC cliff (qualitative, conclusive)

`oltp_read_write` at the high-contention `ts=1000`:

- **t=1 (no concurrency):** A and B essentially equal (~135 tps) —
  uncontended write latency is the same (same engine).
- **t=8 (contention):** A = 611 tps; **B = 0.03 tps, p95 ≥ 100 s**.

This collapse is **not measurable as throughput** and is **already
conclusively characterized**: every captured MariaDB `read_write`
contention cell — across the 4 GB run, the 12 GB run, and two
independent dryruns — shows the identical signature (tps ≈ 0,
p95 ≥ 50–100 s). The report's `A/B = 20378` cell is a degenerate ratio
(B ≈ 0); read it as "B collapses," not a 20000× speedup.

We deliberately did **not** spend days re-running these cells: a
server-side `max_statement_time=5` cap was tried to bound the hang and
**empirically failed** (a single cell still took 969 s; the stall is
inside TidesDB's row-lock/OCC wait, below the statement timer). More
data points would only re-confirm the collapse. The qualitative
finding stands on the preserved evidence.

Under the same contention, **MySQL + tidesdb-mysql degrades but keeps
making progress** (611 → 278 tps at t=8 → t=32 with bounded p95),
i.e. the two integrations handle TidesDB write conflict very
differently. Characterizing *why* upstream tidesql's handler holds the
lock-wait ~100 s is follow-up work, not a benchmark number.

## 3. MySQL-side write scaling (single-SUT, still useful)

`oltp_read_write`, `oltp_write_only`, `oltp_insert` ran the full matrix
on A. No comparable B numbers (collapse above), so these are
characterization of tidesdb-mysql itself, not a comparison:

- insert scales well with threads (1k: 333→918→3122 tps at t=1/8/32;
  1M: 154→2023→3552).
- read_write is contention-bound: best at the *medium* table
  (100k t=8 ≈ 740 tps) — too small = conflict, too large (1M) = read
  amplification (1M t=8 ≈ 94 tps, p95 ~480 ms).
- write_only plateaus ~2000 tps at t=32 across sizes.

## 4. Root cause of the earlier run failures (resolved)

The 4 GB pass died mid-sweep at `oltp_read_write ts=1000000`. In-situ
preserved evidence (`perf-20260518T195958Z/mysql.DEATH.txt`):
`OOMKilled=true exit=137` — the container was cgroup-OOM-killed; the
1M-row × 4-table read_write working set + TidesDB memtables/cache
exceeded the 4 GB cap. **Not a TidesDB bug.** Raising the cap to
**12 GB (identical on both SUTs)** resolved it: MySQL then completed
the full 180-cell matrix including that exact cell. (An earlier
"not OOM" claim was based on two invalid isolated repros that never
exercised the workload; the orchestrator hardening — per-cell health
check + log preservation + abort-on-death — caught the true cause
in situ.)

## 5. Disclosed fairness normalizations

- Identical TidesDB v9.2.0 + default CF config + identical 4 CPU /
  12 GB caps + identical server benchmarking flags (max_connections,
  max_connect_errors, connect/net timeouts) on **both** SUTs.
- sysbench tolerates transient TidesDB conflict errnos
  (1213/1180/1205/1969) on both SUTs — the residual conflict *rate*
  is a characterization, not a failure.
- The `max_statement_time` cap was attempted MariaDB-only but is
  **non-effective and unused in the final data** (it didn't bound the
  hang; MySQL never hits it because its conflicts return in ms). The
  comparison rests on point_select (fully symmetric) plus the
  qualitative write-collapse finding. No asymmetric config influenced
  any committed number.

## 6. Status / limitations

- Quantitative A-vs-B comparison: **point_select only** (complete,
  clean, 5-rep, full sweep). This is the publishable comparison.
- Write workloads: A characterized fully; B = qualitative collapse
  (evidenced, not numeric).
- `datadir_bytes` is whole-datadir (different system-table baselines
  per server) — a coarse proxy, **not** a clean space-amplification
  number; not used for cross-SUT claims. Refining to the
  `tidesdb_data` subdir is future work.
- Article framing: lead with the point_select 1.4–1.5× read-path
  advantage on identical TidesDB, plus the qualitative write-contention
  divergence. Both are honestly bounded by what was measured.
