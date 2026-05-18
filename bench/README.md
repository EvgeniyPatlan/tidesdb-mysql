# TidesDB Integration Comparison — Methodology

Status: **methodology only — no harness/builds yet.** This document is the
agreed plan and the reproducibility contract for a later article. Nothing
in `bench/` runs until the harness is built in a subsequent pass.

## 1. Purpose

Compare two *shipped products* that expose the **same** LSM storage engine
through two different SQL servers:

| | System under test (SUT) | Server | Glue | Engine |
|---|---|---|---|---|
| **A** | `tidesdb-mysql` (this repo) | MySQL **9.7** | our `ha_tidesdb.so` (MySQL handler API) | TidesDB **v9.2.0** |
| **B** | upstream `tidesdb/tidesql` | MariaDB **12.x GA** | upstream `ha_tidesdb.so` (MariaDB handler API) | TidesDB **v9.2.0** |

The goal is data for an article on what it costs/gains to run TidesDB as a
first-class MySQL storage engine versus the established MariaDB path.

### 1.1 Honest framing (threat to validity, stated up front)

This is a **confounded, product-vs-product** comparison, not a controlled
single-variable experiment. We cannot run our handler on MariaDB or
upstream's on MySQL, so each result reflects *server + integration glue*
together. The article must state this explicitly. The one variable we
**do** hold constant is the storage core: **identical TidesDB v9.2.0 and
identical column-family configuration on both sides** — so differences are
attributable to the server+glue layer, not to TidesDB itself.

## 2. Pinned versions (frozen + disclosed)

- TidesDB: **v9.2.0** (commit `7bbd138`) — both SUTs. Verified: our build
  links `vendor/tidesdb-prefix/libtidesdb.a` @ v9.2.0; upstream Dockerfile
  accepts `--build-arg TIDESDB_VERSION=v9.2.0` and clones that exact tag.
- MySQL: **9.7.0** (SUT A, our `evgeniypatlan/test-images:mysql-9.7-tidesdb-v0.2.1`).
- MariaDB: **latest 12.x GA** — resolved to the newest 12.x GA tag at first
  image build, then **frozen** and recorded verbatim in every results
  manifest (no floating "latest").
- Plugin (SUT A): `tidesdb-mysql` **v0.2.1** (`461f33a`).
- Plugin (SUT B): upstream `tidesdb/tidesql` — commit recorded at build.

Every result file records the full version tuple so a run is reproducible
from the manifest alone.

## 3. Fairness controls (codified into the harness)

1. **Identical TidesDB version + CF config** on both: compression, block
   cache size, flush threads, compaction threads, memtable / write-buffer
   size, sync mode — set to the same values via `tidesdb.cfg` /
   server sysvars / `ENGINE_ATTRIBUTE` so the LSM behaves identically.
2. **Identical container resource caps**: same `--cpus`, `--memory`,
   `--memory-swap`, storage driver, and host. One SUT container at a time.
3. **Cold, equal start state**: fresh data dir per run; OS page cache
   dropped between repetitions; no warm neighbours.
4. **Warmup excluded**: a fixed warmup phase precedes every measured phase;
   only the measured phase counts.
5. **Repetition + dispersion**: ≥5 repetitions per (SUT, workload, concurrency)
   cell; report **median + IQR** (not mean), flag runs with >X% variance.
6. **Identical client**: same `sysbench` version, same Lua scripts, same
   RNG seeds, same dataset generator and row counts.
7. **Concurrency sweep**: 1, 2, 4, 8, 16, 32, 64 client threads (matched).
8. **Schema parity**: engine-agnostic DDL where possible; a thin per-server
   syntax-shim layer for the unavoidable differences (see §4.1).

## 4. Parity track (built first)

A server-neutral SQL conformance corpus run against **both** SUTs;
normalize output; diff; emit a feature matrix.

### 4.1 Syntax-shim layer

Unavoidable server-grammar differences are isolated in a shim so the
*semantic* test body stays shared:

- Plugin install: `INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so'` (MySQL)
  vs `INSTALL SONAME 'ha_tidesdb'` (MariaDB).
- Per-table options: MySQL `ENGINE_ATTRIBUTE='{"...":...}'` JSON vs
  MariaDB `... COMPRESSION='LZ4' BLOOM_FILTER=1` grammar — the shim maps a
  single logical option set to each dialect.
- Server-specific sysvar names where they differ.

### 4.2 Parity axes → matrix

For each feature, per SUT, classify: **supported / partial / wrong-result
/ unsupported / error**.

- Data types (ints, decimal/float, temporal, char/binary, enum/set, JSON,
  large BLOB/TEXT)
- Transactions & isolation levels; savepoints; consistent snapshot
- Indexes: secondary, composite, unique, hidden PK, per-index B-tree
- **FULLTEXT**: natural/boolean/phrase, BM25 ranking, stopwords, blend
  chars, **and `ALTER … ADD FULLTEXT` back-population of existing rows**
  (SUT A fixes this in v0.2.1 incl. the F-1 meta-counter fix — a likely
  headline divergence vs upstream; explicitly test the drop+re-ADD and
  abort/retry behaviour)
- SPATIAL: MBR predicates, Hilbert index, negative coords
- Online DDL: INSTANT vs INPLACE vs COPY classification + correctness;
  inplace add/drop secondary index; instant add/drop column
- TTL (table default + per-row column)
- At-rest encryption; key id; ENGINE_ATTRIBUTE/option freeze semantics
- REPLACE / INSERT … ON DUPLICATE KEY UPDATE
- Crash recovery: `kill -9` mid-write, restart, verify durability +
  no corruption; recovery time
- Mixed-engine transactions (TidesDB + InnoDB) where the server allows

Output: `bench/parity/matrix.md` (generated) + raw per-case diffs kept for
the article appendix.

## 5. Performance track (after parity)

Common driver: **sysbench** (runs against both MySQL and MariaDB with
identical Lua + seeds).

### 5.1 Workloads

- `oltp_point_select`, `oltp_read_only`, `oltp_read_write`,
  `oltp_write_only`, `oltp_update_index`, `oltp_update_non_index`,
  `bulk_insert`
- Write-heavy LSM-stress (sustained inserts/updates to exercise
  compaction; capture compaction/SSTable counts where exposed)
- FTS query throughput (MATCH … AGAINST mixes) on a fixed text corpus
- Range scan and secondary-index scan microbenchmarks
- TPC-C-like (sysbench-tpcc) at a fixed warehouse count

Each at the §3.7 concurrency sweep, dataset sizes that both fit-in-cache
and exceed-cache (to expose LSM/compaction and read-amp behaviour).

### 5.2 Metrics

- Throughput: TPS / QPS (measured phase only)
- Latency: p50 / p95 / p99 / max
- **Efficiency (most article-worthy):**
  - CPU-seconds per 1k transactions (server + engine)
  - Peak RSS for the run
  - **Space amplification**: on-disk bytes ÷ logical dataset bytes
  - Write amplification proxy: bytes written to disk ÷ logical bytes
    (from container/cgroup + TidesDB stats where available)
- Operational: cold start + plugin-load time; crash-recovery time;
  backup/checkpoint duration for a fixed dataset

## 6. Planned layout (for the build pass — not yet created)

```
bench/
  README.md                 # this file
  images/
    build-mysql-sut.sh       # tag our v0.2.1 image as sut-mysql:bench
    build-mariadb-sut.sh     # upstream docker/setup.sh, MARIADB_VERSION=12.x GA,
                             #   TIDESDB_VERSION=v9.2.0, WITH_TESTS=1 -> sut-mariadb:bench
    versions.lock            # frozen resolved version tuple (committed)
  parity/
    shim/                    # per-server DDL/sysvar dialect mapping
    cases/                   # server-neutral semantic cases
    run-parity.sh            # run both, normalize, diff
    matrix.md                # generated
  workloads/*.yaml           # declarative workload specs (the reproducibility unit)
  orchestrator/
    run.(sh|py)              # provision (capped) -> warmup -> N reps -> collect
    collectors/              # tps/latency (sysbench), cpu-sec, rss, disk size, tdb stats
  results/<UTC-ts>/          # manifest.json + raw logs + results.csv (summaries committed)
  report/
    gen-report.py            # results -> tables + charts -> report.md
```

## 7. Reproducibility contract

A result is publishable only if: pinned version tuple recorded in
`manifest.json`; container resource caps recorded; ≥5 reps with median+IQR;
raw sysbench/log output retained; workload spec is the committed YAML; the
exact image digests (both SUTs) recorded. The article cites commit + image
digests so a reader can rebuild both SUTs.

## 8. Open items before the build pass

- Resolve & freeze the concrete MariaDB 12.x GA tag at first image build.
- Confirm upstream `docker/setup.sh` builds cleanly with
  `MARIADB_VERSION=<12.x GA> TIDESDB_VERSION=v9.2.0 WITH_TESTS=1`
  (one-time ~30+ min build; Step 0 confirmed the mechanism, not yet a
  full build).
- Decide the CF-config baseline values to enforce on both sides.
- Pick sysbench-tpcc warehouse count + dataset sizes (in/out of cache).
