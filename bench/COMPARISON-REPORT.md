# TidesDB on MySQL vs TidesDB on MariaDB — Consolidated Comparison

Single source-of-truth report synthesizing the functional-parity and
performance tracks. Article-source document, not the article itself.
Every figure here is traceable to a committed artifact (paths cited);
nothing is estimated.

---

## Executive summary

Two shipped products wrap the **same** LSM engine, TidesDB **v9.2.0**:

- **A** — MySQL **9.7.0** + `tidesdb-mysql` **v0.2.1** (`461f33a`), our work.
- **B** — MariaDB **12.3.1** + upstream `tidesdb/tidesql` (`9b5f5b8`).

Three findings, in decreasing confidence:

1. **Read path: A is ~1.4–1.5× faster than B** (median across the full
   contention×concurrency sweep; range 1.21–1.88×), with lower p95.
   Same engine both sides → attributable to the server + integration
   glue, not TidesDB. *Quantitative, complete, 5-rep.*
2. **Functional parity is high (8/11 identical)**, with **one real
   product divergence**: `ALTER TABLE … ADD FULLTEXT` back-populates
   pre-existing rows on **A** (the v0.2.1 feature + F-1 fix) but **not
   on B**. *Qualitative, conclusive, evidenced.*
3. **Write contention: B collapses, A degrades gracefully.** Under
   concurrent write contention upstream tidesql drops to ≈0 tps with
   p95 ≥ 100 s; tidesdb-mysql keeps making progress. *Qualitative,
   conclusive (not a throughput number — see §4.2).*

**Honest framing:** this is a *product-vs-product* comparison
(server + glue together), not a controlled single-variable experiment —
A's handler can't run on MariaDB or vice-versa. The one variable held
constant is the storage core (identical TidesDB v9.2.0 + default CF
config + identical container/server caps). State this in any article.

---

## 1. What was compared (frozen pins)

| | SUT A | SUT B |
|---|---|---|
| Server | MySQL 9.7.0 | MariaDB 12.3.1 (`mariadb-12.3.1`) |
| Glue | tidesdb-mysql v0.2.1 (`461f33a`) | upstream tidesql (`9b5f5b8`) |
| Engine | **TidesDB v9.2.0** (`7bbd138`) | **TidesDB v9.2.0** (`7bbd138`) |
| Load gen | sysbench 1.0.20 (same client both) | same |

Full tuple frozen in `bench/images/versions.lock`. Fairness controls
(identical 4 CPU / 12 GB caps, identical benchmarking server flags,
tolerated transient-conflict errnos, warmup-excluded, ≥5 reps) are
codified in `bench/README.md` §3 and the orchestrator; the one
attempted asymmetric knob (MariaDB `max_statement_time`) proved
non-effective and influenced **no** committed number (see §4.2 / 5).

---

## 2. Functional parity

Server-neutral SQL corpus, 11 cases, run on both SUTs; output
normalized and diffed. Primary evidence:
`bench/results/20260518T152853Z/matrix.md`, interpretation in
`bench/parity/FINDINGS.md`.

**Result: 8/11 byte-identical.**

| Verdict | Cases |
|---|---|
| **SAME (8)** | core CRUD; secondary-index range; txn commit/rollback; inplace add/drop index; instant add-column backfill; FULLTEXT natural/boolean; REPLACE & INSERT…ODKU; per-table option round-trip (validates the `ENGINE_ATTRIBUTE`-JSON vs MariaDB-option-grammar shim) |
| **Real divergence (1)** | **`ADD FULLTEXT` back-populate** — A indexes pre-existing rows and ranks `MATCH` correctly, stable across drop/re-ADD (v0.2.1 + F-1); **B returns empty** (existing rows never indexed) |
| **Not conclusive (2)** | spatial — case used MySQL-only `POINT … SRID 0` DDL that MariaDB's parser rejects (test-portability artifact, **not** a capability gap; needs a dialect shim); encryption — **both** ERROR identically (keyring/master-key prerequisite absent in both base images; not an A-vs-B difference) |

The two non-conclusive rows are honestly *not* claimed as B
deficiencies — they are corpus/setup limitations, annotated as such in
the matrix.

---

## 3. Performance — read path (headline, complete A-vs-B)

`oltp_point_select`, the one workload with a clean full matrix on both
SUTs. Median of 5 reps. Source:
`bench/results/perf-20260519T051541Z/report.md` + charts.

**A / B throughput ratio (higher = A faster):**

| table_size | t=1 | t=8 | t=32 |
|---|---|---|---|
| 1 000      | 1.25× | 1.41× | 1.44× |
| 100 000    | 1.69× | 1.54× | 1.40× |
| 1 000 000  | 1.88× | 1.49× | 1.21× |

Representative absolutes (median tps): 100k/t=8 **30,413 (A) vs 19,700
(B)**; 1M/t=1 **14,438 vs 7,662**. p95 latency is lower on A across the
board (e.g. 1M/t=8: 0.44 ms vs 0.57 ms). Identical engine both sides ⇒
the gap is the MySQL vs MariaDB server + handler/optimizer/connection
path. Charts: `tps_threads_oltp_point_select.png`,
`contention_oltp_point_select.png`, `p95_threads_oltp_point_select.png`.

---

## 4. Performance — write path

### 4.1 MySQL-side scaling (single-SUT characterization)

`oltp_read_write` / `oltp_write_only` / `oltp_insert` ran the full
matrix on A (no comparable B — see §4.2). Characterizes tidesdb-mysql:
insert scales with threads (1k: 333→918→3,122 tps at t=1/8/32);
read_write is contention-bound, best at the *medium* table
(100k/t=8 ≈ 740 tps) — small = conflict, 1M = read amplification
(1M/t=8 ≈ 94 tps); write_only plateaus ≈2,000 tps at t=32.

### 4.2 Write contention — the OCC cliff (qualitative, conclusive)

`oltp_read_write` at high-contention `ts=1000`:

| | A (MySQL) | B (MariaDB) |
|---|---|---|
| t=1 (no concurrency) | ~135 tps | ~135 tps (equal — same engine) |
| t=8 (contention) | **611 tps** | **0.03 tps, p95 ≥ 100 s** |

Upstream tidesql **collapses** under TidesDB optimistic-concurrency
write contention — the *identical* signature (tps≈0, p95 50–100 s)
recurred across the 4 GB run, the 12 GB run, and two independent
dryruns. This is **not reported as a throughput number**: a
server-side `max_statement_time=5` cap was tried to bound the hang and
**empirically failed** (a single cell still took 969 s — the stall is
inside TidesDB's row-lock/OCC wait, below the statement timer). The
finding is qualitative and stands on the repeated evidence; more data
points would only re-confirm it. Under the same contention A degrades
gracefully (611→278 tps t=8→t=32, bounded p95) — the two integrations
handle TidesDB write conflict very differently. *Why* upstream holds
the lock-wait ~100 s is follow-up work, not a benchmark figure.

---

## 5. Diagnosed & resolved during the campaign (process integrity)

- **4 GB mid-run SUT death** root-caused to a **container OOM-kill**
  (`OOMKilled=true`, exit 137) at the 1M-row read_write working set —
  a resource-cap issue, **not** a TidesDB bug. In-situ evidence:
  `bench/results/perf-20260518T195958Z/mysql.DEATH.txt`. Re-running at
  **12 GB (identical on both SUTs)** resolved it; MySQL then completed
  all 180 cells including that exact cell.
- An earlier "not OOM" conclusion was **wrong** (based on two invalid
  isolated repros that never exercised the workload). The orchestrator
  hardening — per-cell health check, log preservation, abort-on-death —
  caught the true cause in situ. Recorded transparently in
  `bench/perf-FINDINGS.md`.
- Parity harness: one author-error (`secondary_index_range` expected
  value) and several harness bugs were caught and fixed *before*
  trusting numbers; logged in `bench/parity/FINDINGS.md` as the audit
  trail distinguishing real divergences from harness/author error.

---

## 6. Limitations & threats to validity

- **Confounded by design** (server+glue); the article must say so.
- Quantitative A-vs-B = **point_select only** (complete, clean). Write
  workloads: A fully characterized; B = qualitative collapse
  (evidenced, not numeric).
- Spatial & encryption parity rows are **inconclusive** (corpus/setup),
  not B deficiencies.
- `datadir_bytes` is whole-datadir (different per-server system-table
  baselines) — a coarse proxy, **not** a clean space-amplification
  metric; excluded from cross-SUT claims.
- Single host, single TidesDB/server version pair, fixed CF defaults —
  results are point-in-time for this exact tuple.

---

## 7. Reproducibility

Branch `bench/comparison-framework`. Rebuild SUTs:
`bench/images/build-{mysql,mariadb,sysbench}-sut.sh` (pins in
`versions.lock`). Re-run: `bench/parity/run-parity.sh`,
`bench/orchestrator/run-perf.sh bench/workloads/oltp_smoke.yaml`.
Charts: `bench/report/run-charts.sh <results-dir>`. Raw sysbench
output is gitignored; curated `results.csv` + `report.md` + charts +
`matrix.md` + `versions.lock` + DEATH evidence are committed.

---

## 8. Suggested article angle

Lead with the thesis: **"Same LSM engine, two servers."** Pillars:

1. **Read-path win, quantified:** TidesDB as a first-class MySQL 9.7
   engine delivers ~1.4–1.5× the point-select throughput of the
   established MariaDB path, on byte-identical TidesDB — the cost is in
   the server/glue layer, and MySQL's is faster here.
2. **A correctness feature MySQL has and MariaDB doesn't:** `ADD
   FULLTEXT` back-populates existing rows (v0.2.1 + the F-1 fix);
   upstream silently indexes nothing.
3. **Write-contention behavior diverges sharply:** under OCC conflict
   the MariaDB integration stalls to ≈0; the MySQL one degrades but
   survives — a real operational distinction.
4. **Methodology as a feature:** identical-engine fairness controls,
   in-situ failure diagnosis (the OOM story), and transparent
   correction of a wrong early conclusion — credibility through rigor.

Keep every claim bounded by what was measured; cite the committed
artifacts.
