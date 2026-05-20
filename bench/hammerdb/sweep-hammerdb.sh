#!/usr/bin/env bash
# Autopilot VU-sweep around run-hammerdb.sh (TPROC-C).
#
# Runs the same TPROC-C harness at multiple RUNVU values, auto-scaling
# WARE to maintain the >=10:1 warehouses-per-VU ratio that TPC-C
# convention requires to avoid pathological hot-row OCC contention
# (otherwise low VU counts look fine and high VU counts artificially
# collapse, painting a misleading throughput curve).
#
# Collects NOPM / TPM from each child SUMMARY into a single CSV/table
# so you can plot the actual throughput curve instead of a single
# point. Each child run is fully isolated (fresh DB container) -- the
# rebuild cost is the dominant time, not the timed phase, so set
# RAMP/DUR per child accordingly.
#
# Env:
#   VUS         space-separated RUNVU values to sweep (default: "1 4 8 16")
#   WARE_RATIO  warehouses per VU (default: 10, TPC-C minimum)
#   WARE_MIN    floor on WARE (default: 20)
#   BUILDVU     loader VUs per child (default: 4; raise for large WARE)
#   RAMP DUR    per-child timing in minutes (default: 1 / 3)
#   CPUS MEM    docker caps (default: 6 / 16g)
#   DB_EXTRA_ARGS  forwarded as-is to each child (e.g. unified=1 sweep)
#
# Example -- honest 1..32 VU curve at the TPC-C 10:1 ratio:
#   VUS="1 4 8 16 32" RAMP=1 DUR=3 ./sweep-hammerdb.sh
#
# Example -- sweep with the unsafe path opted back in (post-fix):
#   DB_EXTRA_ARGS="--loose_tidesdb_unified_memtable=1" ./sweep-hammerdb.sh
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)

VUS=${VUS:-"1 4 8 16"}
WARE_RATIO=${WARE_RATIO:-10}
WARE_MIN=${WARE_MIN:-20}
BUILDVU=${BUILDVU:-4}
RAMP=${RAMP:-1}; DUR=${DUR:-3}
CPUS=${CPUS:-6}; MEM=${MEM:-16g}

TS=$(date -u +%Y%m%dT%H%M%SZ)
SWEEP="$BENCH/results/sweep-$TS"; mkdir -p "$SWEEP"
CSV="$SWEEP/sweep.csv"
echo "vu,ware,buildvu,nopm,tpm,probe,handler_errs,occ,unified_mt,result_dir" > "$CSV"

echo "[sweep] VUs='$VUS' ratio=${WARE_RATIO}:1 floor=${WARE_MIN} build=${BUILDVU} ramp=${RAMP}m dur=${DUR}m caps=${CPUS}cpu/${MEM}"
echo "[sweep] DB_EXTRA_ARGS='${DB_EXTRA_ARGS:-}'  (forwarded to each child)"

for vu in $VUS; do
  ware=$((vu * WARE_RATIO))
  [ "$ware" -lt "$WARE_MIN" ] && ware=$WARE_MIN
  echo
  echo "================================================================"
  echo "[sweep] cell: RUNVU=$vu WARE=$ware (>=${WARE_RATIO}:1)"
  echo "================================================================"

  # snapshot the latest child dir id before so we can find the new one
  before=$(ls -1dt "$BENCH/results/hammerdb-"* 2>/dev/null | head -1)
  WARE=$ware BUILDVU=$BUILDVU RUNVU=$vu RAMP=$RAMP DUR=$DUR \
    CPUS=$CPUS MEM=$MEM DB_EXTRA_ARGS="${DB_EXTRA_ARGS:-}" \
    "$HERE/run-hammerdb.sh" || echo "[sweep] cell vu=$vu exited non-zero (continuing)"

  child=$(ls -1dt "$BENCH/results/hammerdb-"* 2>/dev/null | head -1)
  if [ -z "$child" ] || [ "$child" = "$before" ]; then
    echo "[sweep] !!! no new result dir for vu=$vu; recording empty row"
    echo "$vu,$ware,$BUILDVU,,,NO_DIR,,,," >> "$CSV"
    continue
  fi
  # Extract metrics from the child's SUMMARY.txt
  S="$child/SUMMARY.txt"
  nopm=$(grep -aoE '[0-9]+ NOPM' "$S" 2>/dev/null | head -1 | awk '{print $1}')
  tpm=$(grep -aoE '[0-9]+ MySQL TPM' "$S" 2>/dev/null | head -1 | awk '{print $1}')
  probe=$(awk -F': *' '/^reverse-ref probe/{print $2;exit}' "$S" 2>/dev/null)
  herr=$(awk -F': *' '/handler-unsupported errors/{n=$2;sub(/  .*/,"",n);print n;exit}' "$S" 2>/dev/null)
  occ=$(awk -F': *' '/expected OCC conflict/{n=$2;sub(/  .*/,"",n);print n;exit}' "$S" 2>/dev/null)
  umt=$(awk -F'= *' '/tidesdb_unified_memtable/{print $2;exit}' "$S" 2>/dev/null)

  echo "$vu,$ware,$BUILDVU,${nopm:-},${tpm:-},${probe:-?},${herr:-?},${occ:-?},${umt:-?},$(basename "$child")" >> "$CSV"
  echo "[sweep] vu=$vu  NOPM=${nopm:-n/a}  TPM=${tpm:-n/a}  probe=${probe:-?}  hdl_err=${herr:-?}  unified=${umt:-?}"
done

echo
echo "================================================================"
echo "[sweep] DONE -- $SWEEP/sweep.csv"
echo "================================================================"
# Pretty-print using column if available, otherwise raw CSV.
if command -v column >/dev/null 2>&1; then
  column -t -s, "$CSV" | tee "$SWEEP/sweep.txt"
else
  cat "$CSV"
fi
echo
echo "[sweep] children:"
awk -F, 'NR>1{print "  " $10}' "$CSV"
