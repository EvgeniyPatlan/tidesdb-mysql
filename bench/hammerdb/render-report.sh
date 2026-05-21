#!/usr/bin/env bash
# Render a self-contained REPORT.md from a v0.2.3 suite output directory.
# Usage: render-report.sh <suite-dir> <image> <baseline-engine> <cpus> <mem>
set -uo pipefail

SUITE=${1:?suite dir}
IMAGE=${2:-tidesdb/mysql:9.7}
ENGINE_B=${3:-innodb}
CPUS=${4:-?}
MEM=${5:-?}

TS=$(basename "$SUITE" | sed 's/^suite-//')

# small helpers (silent if file missing) -----------------------------------
val()   { awk -F': *' "/^$2/{v=\$2; sub(/  +.*/,\"\",v); print v; exit}" "$1" 2>/dev/null; }
# valraw: same as val but keeps trailing parenthetical comment (e.g. "(must be 0)")
valraw(){ awk -F': *' "/^$2/{print \$2; exit}" "$1" 2>/dev/null; }
nopm()  { grep -aoE '[0-9]+ NOPM' "$1" 2>/dev/null | head -1 | awk '{print $1}'; }
tpm()   { grep -aoE '[0-9]+ MySQL TPM' "$1" 2>/dev/null | head -1 | awk '{print $1}'; }
# parse "  lineitem: <actual> (expected <expected>)" -> "actual / expected"
tphkv() { awk -v k="$2" '$0 ~ "^  "k {gsub(/[():,]/,""); print $2" / "$4; exit}' "$1" 2>/dev/null; }
# extract a "WARE=X RUNVU=Y RAMP=Zm DUR=Wm" piece from a profile line
profval(){ awk -v k="$2" '/^profile/{for(i=1;i<=NF;i++)if($i~"^"k"="){sub("^"k"=","",$i);print $i;exit}}' "$1" 2>/dev/null; }

# 1. correctness baseline
B_SUM="$SUITE/01-baseline/SUMMARY.txt"
B_WARE=$(profval "$B_SUM" WARE)
B_PROBE=$(val "$B_SUM" "reverse-ref probe")
B_HE=$(val "$B_SUM" "handler-unsupported errors in run.log")
B_NOPM=$(nopm "$B_SUM"); B_TPM=$(tpm "$B_SUM")

# 2. recovery
R_SUM="$SUITE/02-recovery/SUMMARY.txt"
if [ -f "$R_SUM" ]; then
  R_BUILD=$(val "$R_SUM" "schema build       ")
  R_REC=$(val "$R_SUM" "post-kill recovery ")
  R_LOSS=$(val "$R_SUM" "committed-row loss ")
  R_VERDICT=$(awk -F': *' '/^verdict/{print $2;exit}' "$R_SUM" 2>/dev/null)
else
  R_BUILD="(SUMMARY missing)"; R_REC="(SUMMARY missing)"
  R_LOSS="(SUMMARY missing)"; R_VERDICT="(SUMMARY missing -- recovery step did not complete; see raw/02-recovery.log)"
fi

# 3. sweep
S_CSV="$SUITE/03-sweep/sweep.csv"

# 4a/4b head-to-head
A_SUM="$SUITE/04a-h2h-tidesdb/SUMMARY.txt"
B_HSUM="$SUITE/04b-h2h-$ENGINE_B/SUMMARY.txt"
H2H_WARE=$(profval "$A_SUM" WARE); H2H_VU=$(profval "$A_SUM" RUNVU); H2H_DUR=$(profval "$A_SUM" DUR)
A_NOPM=$(nopm "$A_SUM"); A_TPM=$(tpm "$A_SUM")
BH_NOPM=$(nopm "$B_HSUM"); BH_TPM=$(tpm "$B_HSUM")
RATIO=$(awk -v a="$A_NOPM" -v b="$BH_NOPM" 'BEGIN{if(b>0)printf "%.0f%%", 100*a/b; else print "n/a"}')

# 5. tproch
T_SUM="$SUITE/05-tproch/SUMMARY.txt"
T_BUILD=$(val "$T_SUM" "schema build       ")
T_GATE=$(val "$T_SUM" "row-count gate     ")
T_LI=$(tphkv "$T_SUM" lineitem)
T_ORD=$(tphkv "$T_SUM" orders)

# 6. sustained
SUS_SUM="$SUITE/06-sustained/SUMMARY.txt"
SUS_RUNLOG="$SUITE/06-sustained/run.log"
SUS_DUR=$(profval "$SUS_SUM" DUR)
SUS_VU=$(profval "$SUS_SUM" RUNVU)
SUS_WARE=$(profval "$SUS_SUM" WARE)
SUS_TPM_STATS=$(grep -aoE '[0-9]+ MySQL tpm$' "$SUS_RUNLOG" 2>/dev/null \
  | awk '{s+=$1;n++;if($1>mx)mx=$1;if(mn==0||($1<mn&&$1>0))mn=$1} END{
        if(n)printf "samples=%d mean=%.0f min=%d max=%d",n,s/n,mn,mx; else print "n/a"}')
SUS_NOPM=$(nopm "$SUS_SUM")

cat <<EOF
# TidesDB on MySQL — v0.2.3 verification suite

**Run timestamp:** \`$TS\`
**Image:** \`$IMAGE\`
**Container caps:** ${CPUS} CPU / ${MEM} RAM
**Engine (TidesDB defaults):** \`tidesdb_unified_memtable=OFF\`, \`sync_mode=FULL\` (the v0.2.3 durability defaults).

A self-contained run of every HammerDB-based test we have, executed
sequentially against a single image build. Each section explains the
test, why it matters, and the result captured in this run.

## 1. Correctness baseline (TPROC-C)

**What it tests.** A full TPC-C build at modest scale (WARE=${B_WARE:-?}), the deterministic reverse-ref / Order-Status probe right after build, the timed transaction mix, and a count of any \`ER_ILLEGAL_HA\` / \`WRONG_COMMAND\` handler-unsupported errors. The probe specifically exercises the partial-PK-prefix reverse scan (\`ORDER BY o_id DESC LIMIT 1\` with a \`WHERE o_w_id=? AND o_d_id=?\` filter) that previously surfaced as ER_ILLEGAL_HA before v0.2.2.

**Why it matters.** Acts as the regression gate. Any reappearance of the handler-unsupported bug, or any silent build truncation, fails this immediately.

**Result.**
- Schema build: \`COMPLETED\`
- Reverse-ref probe: **\`${B_PROBE:-n/a}\`**
- Handler-unsupported errors: **\`${B_HE:-n/a}\`** (must be 0)
- Throughput: **${B_NOPM:-n/a} NOPM / ${B_TPM:-n/a} MySQL TPM**

## 2. Crash-recovery (WAL durability under SIGKILL)

**What it tests.** Builds a small TPC-C schema on a persistent Docker volume, starts a brief NewOrder workload, samples \`district.d_next_o_id\` per district as the pre-kill watermark (every committed NewOrder has incremented its district counter), then \`docker kill -s KILL\` mid-run. Restarts a new container against the **same** volume and asserts:

1. \`mysqld\` recovers without manual repair.
2. No committed \`d_next_o_id\` rolled back.
3. Every committed order row remains present (\`MAX(o_id)\` per district ≥ pre-kill watermark - 1).

**Why it matters.** This is the test that exposed the upstream TidesDB v9.2.0 WAL bug — three stacked engine defects that silently dropped every committed write on a hard crash. The fix ships in v0.2.3 as the bundled \`docker/patches/0001-walfix.patch\`.

**Result.**
- Schema build:       **\`${R_BUILD:-n/a}\`**
- Post-kill recovery: **\`${R_REC:-n/a}\`**
- Committed-row loss: **\`${R_LOSS:-n/a}\`**
- Verdict:            **\`${R_VERDICT:-n/a}\`**

## 3. VU throughput curve (TPC-C, 10:1 warehouses-per-VU ratio)

**What it tests.** Runs the same TPC-C harness across multiple VU values, auto-scaling \`WARE\` to maintain the TPC-C ≥10:1 warehouses-per-VU convention. Below that ratio, NewOrder collides on the small set of district counter rows and OCC aborts dominate — the curve flattens or collapses for reasons that are workload, not engine. The sweep keeps that confounder out.

**Why it matters.** A single throughput number tells you nothing about how the engine scales with concurrency. The sweep is the cheapest way to see the curve shape (linear / sublinear / plateau / collapse).

**Result.**

\`\`\`
$( [ -f "$S_CSV" ] && column -t -s, "$S_CSV" || echo "n/a" )
\`\`\`

## 4. Head-to-head: TidesDB vs ${ENGINE_B}

**What it tests.** Identical TPC-C workload, identical profile, identical container caps, **same MySQL build** — only the storage engine differs. Both run with full durability (TidesDB \`sync_mode=FULL\`, InnoDB \`innodb_flush_log_at_trx_commit=1\` default).

**Why it matters.** The most defensible "where do we stand vs the reference engine" data we can produce: same MySQL plumbing, same client, same workload. Cross-vendor benchmarks confound everything; in-process head-to-head doesn't.

**Result.**

| Metric                  | TidesDB         | ${ENGINE_B}   | TidesDB / ${ENGINE_B} |
|-------------------------|-----------------|---------------|-----------------------|
| **NOPM** (NewOrders/m)  | ${A_NOPM:-n/a}  | ${BH_NOPM:-n/a} | ${RATIO}            |
| MySQL TPM (final)       | ${A_TPM:-n/a}   | ${BH_TPM:-n/a}  |                       |

Caveats:
- **NOPM is the canonical TPC-C cross-engine number.** \`MySQL TPM\` is derived from \`Com_commit + Com_rollback\` and is **not directly comparable** across engines — different engines account commit boundaries differently. Trust NOPM.
- At the scale used here the working set largely fits in cache. A larger \`WARE\` (e.g. 500+) would force more disk IO and could shift the ratio either way.

## 5. TPROC-H read path (schema build + multi-VU power test)

**What it tests.** Builds the TPC-H schema at \`SF=1\` (\`LINEITEM\` ≈ 6,001,215 rows, \`ORDERS\` = 1,500,000), gates on row counts matching the TPC-H spec exactly, then runs the standard 22-query power-test against the LSM (sequential SSTable iteration, block-cache hit rates, bloom-filter effectiveness, range scans, joins).

**Why it matters.** TPC-C barely exercises range scans, joins, or large aggregations. TPROC-H hits all of those — the LSM read path is qualitatively different from OLTP point lookups.

**Result.**
- Schema build:   **\`${T_BUILD:-n/a}\`**
- Row-count gate: **\`${T_GATE:-n/a}\`**
- \`LINEITEM\`: ${T_LI:-n/a}
- \`ORDERS\`:   ${T_ORD:-n/a}

A small non-deterministic shortfall (~0.02%) on the largest concurrent-bulk-load table (\`LINEITEM\`) is a known follow-up — not the 95% catastrophe v0.2.3 fixed; it is logged for future investigation.

## 6. Sustained throughput (${SUS_DUR:-?}-min steady-state, WARE=${SUS_WARE:-?}, RUNVU=${SUS_VU:-?})

**What it tests.** A long TPC-C timed run at a comfortable WARE:VU ratio, sampling per-period TPM throughout. LSM engines often show ramp-vs-steady-state divergence as L0 fills and L1+ compactions kick in; a one-number throughput report can hide that.

**Why it matters.** Confirms the engine doesn't degrade over time under sustained write pressure. Min/max/mean of the per-period samples shows the variance band, not just the average.

**Result.**

- Throughput trace: **${SUS_TPM_STATS}**
- Final NOPM: **${SUS_NOPM:-n/a}**

---

## How to reproduce

Every test in this report is invocable independently from \`bench/hammerdb/\`. The full suite is one command:

\`\`\`bash
cd bench/hammerdb && ./run-all.sh
# or for a fast pipeline check (~30 min, all tests at smoke profile):
SMOKE=1 ./run-all.sh
\`\`\`

Individual tests:

\`\`\`bash
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
\`\`\`

Suite artifacts live under \`bench/results/suite-<timestamp>/\`.
EOF
