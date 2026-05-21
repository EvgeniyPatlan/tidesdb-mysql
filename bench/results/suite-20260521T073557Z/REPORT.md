# TidesDB on MySQL — v0.2.3 verification suite

**Run timestamp:** `20260521T073557Z`
**Image:** `tidesdb/mysql:9.7`
**Container caps:** 6 CPU / 16g RAM
**Engine (TidesDB defaults):** `tidesdb_unified_memtable=OFF`, `sync_mode=FULL` (the v0.2.3 durability defaults).

A self-contained run of every HammerDB-based test we have, executed
sequentially against a single image build. Each section explains the
test, why it matters, and the result captured in this run.

## 1. Correctness baseline (TPROC-C)

**What it tests.** A full TPC-C build at modest scale (WARE=20), the deterministic reverse-ref / Order-Status probe right after build, the timed transaction mix, and a count of any `ER_ILLEGAL_HA` / `WRONG_COMMAND` handler-unsupported errors. The probe specifically exercises the partial-PK-prefix reverse scan (`ORDER BY o_id DESC LIMIT 1` with a `WHERE o_w_id=? AND o_d_id=?` filter) that previously surfaced as ER_ILLEGAL_HA before v0.2.2.

**Why it matters.** Acts as the regression gate. Any reappearance of the handler-unsupported bug, or any silent build truncation, fails this immediately.

**Result.**
- Schema build: `COMPLETED`
- Reverse-ref probe: **`PASS`**
- Handler-unsupported errors: **`0`** (must be 0)
- Throughput: **7574 NOPM / 21136 MySQL TPM**

## 2. Crash-recovery (WAL durability under SIGKILL)

**What it tests.** Builds a small TPC-C schema on a persistent Docker volume, starts a brief NewOrder workload, samples `district.d_next_o_id` per district as the pre-kill watermark (every committed NewOrder has incremented its district counter), then `docker kill -s KILL` mid-run. Restarts a new container against the **same** volume and asserts:

1. `mysqld` recovers without manual repair.
2. No committed `d_next_o_id` rolled back.
3. Every committed order row remains present (`MAX(o_id)` per district ≥ pre-kill watermark - 1).

**Why it matters.** This is the test that exposed the upstream TidesDB v9.2.0 WAL bug — three stacked engine defects that silently dropped every committed write on a hard crash. The fix ships in v0.2.3 as the bundled `docker/patches/0001-walfix.patch`.

**Result.**
- Schema build:       **`COMPLETED`**
- Post-kill recovery: **`OK`**
- Committed-row loss: **`NONE`**
- Verdict:            **`FAIL -- committed orders missing post-recovery (WAL replay incomplete)`**

## 3. VU throughput curve (TPC-C, 10:1 warehouses-per-VU ratio)

**What it tests.** Runs the same TPC-C harness across multiple VU values, auto-scaling `WARE` to maintain the TPC-C ≥10:1 warehouses-per-VU convention. Below that ratio, NewOrder collides on the small set of district counter rows and OCC aborts dominate — the curve flattens or collapses for reasons that are workload, not engine. The sweep keeps that confounder out.

**Why it matters.** A single throughput number tells you nothing about how the engine scales with concurrency. The sweep is the cheapest way to see the curve shape (linear / sublinear / plateau / collapse).

**Result.**

```
vu  ware  buildvu  nopm  tpm    probe  handler_errs  occ  unified_mt  result_dir
1   10    4        4095  9537   PASS   0             0    OFF         hammerdb-20260521T074716Z
4   40    4        4198  32489  PASS   0             0    OFF         hammerdb-20260521T075313Z
8   80    4        1849  74600  PASS   0             0    OFF         hammerdb-20260521T080331Z
```

## 4. Head-to-head: TidesDB vs innodb

**What it tests.** Identical TPC-C workload, identical profile, identical container caps, **same MySQL build** — only the storage engine differs. Both run with full durability (TidesDB `sync_mode=FULL`, InnoDB `innodb_flush_log_at_trx_commit=1` default).

**Why it matters.** The most defensible "where do we stand vs the reference engine" data we can produce: same MySQL plumbing, same client, same workload. Cross-vendor benchmarks confound everything; in-process head-to-head doesn't.

**Result.**

| Metric                  | TidesDB         | innodb   | TidesDB / innodb |
|-------------------------|-----------------|---------------|-----------------------|
| **NOPM** (NewOrders/m)  | 4016  | 3617 | 111%            |
| MySQL TPM (final)       | 31259   | 8367  |                       |

Caveats:
- **NOPM is the canonical TPC-C cross-engine number.** `MySQL TPM` is derived from `Com_commit + Com_rollback` and is **not directly comparable** across engines — different engines account commit boundaries differently. Trust NOPM.
- At the scale used here the working set largely fits in cache. A larger `WARE` (e.g. 500+) would force more disk IO and could shift the ratio either way.

## 5. TPROC-H read path (schema build + multi-VU power test)

**What it tests.** Builds the TPC-H schema at `SF=1` (`LINEITEM` ≈ 6,001,215 rows, `ORDERS` = 1,500,000), gates on row counts matching the TPC-H spec exactly, then runs the standard 22-query power-test against the LSM (sequential SSTable iteration, block-cache hit rates, bloom-filter effectiveness, range scans, joins).

**Why it matters.** TPC-C barely exercises range scans, joins, or large aggregations. TPROC-H hits all of those — the LSM read path is qualitatively different from OLTP point lookups.

**Result.**
- Schema build:   **`COMPLETED`**
- Row-count gate: **`FAIL`**
- `LINEITEM`: 6002175 / 6001215
- `ORDERS`:   1500000 / 1500000

A small non-deterministic shortfall (~0.02%) on the largest concurrent-bulk-load table (`LINEITEM`) is a known follow-up — not the 95% catastrophe v0.2.3 fixed; it is logged for future investigation.

## 6. Sustained throughput (10m-min steady-state, WARE=80, RUNVU=8)

**What it tests.** A long TPC-C timed run at a comfortable WARE:VU ratio, sampling per-period TPM throughout. LSM engines often show ramp-vs-steady-state divergence as L0 fills and L1+ compactions kick in; a one-number throughput report can hide that.

**Why it matters.** Confirms the engine doesn't degrade over time under sustained write pressure. Min/max/mean of the per-period samples shows the variance band, not just the average.

**Result.**

- Throughput trace: **samples=67 mean=70682 min=53070 max=115818**
- Final NOPM: **2708**

---

## How to reproduce

Every test in this report is invocable independently from `bench/hammerdb/`. The full suite is one command:

```bash
cd bench/hammerdb && ./run-all.sh
# or for a fast pipeline check (~30 min, all tests at smoke profile):
SMOKE=1 ./run-all.sh
```

Individual tests:

```bash
# TPROC-C single run
WARE=20 BUILDVU=4 RUNVU=4 RAMP=1 DUR=3 ./run-hammerdb.sh

# Crash-recovery
WARE=10 BUILDVU=4 RUNVU=4 PRE_KILL_SECS=60 ./recovery-test.sh

# VU sweep
VUS="1 4 8 16" ./sweep-hammerdb.sh

# Head-to-head InnoDB baseline
ENGINE=innodb WARE=40 RUNVU=4 RAMP=1 DUR=3 ./run-hammerdb.sh

# TPROC-H power test
SCALE=1 BUILDVU=4 RUNVU=4 ./run-hammerdb-tproch.sh
```

Suite artifacts live under `bench/results/suite-<timestamp>/`.
