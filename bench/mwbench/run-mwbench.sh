#!/usr/bin/env bash
# Engine-level integrity gate for the bundled TidesDB (v9.3.2, unpatched).
#
# Drives the upstream `mwbench` tool (https://github.com/tidesdb/mwbench)
# against the EXACT engine source we ship — built by docker/Dockerfile.mwbench.
# mwbench does a heavy sequential ingest with concurrent reader threads that
# byte-verify every value read back, then a delete -> compact reclaim phase.
# This stresses the flush / compaction / SSTable-cursor / bloom paths where
# our durability bugs and the bloom UAF lived, with NO MySQL in the loop.
#
# Verdict (parsed from samples.csv, columns by name):
#   PASS iff  mismatches == 0         (no byte-level corruption) AND
#             point/seek/range_misses == 0 across every sample row.
#   The misses counters already EXCLUDE deliberately-deleted keys (verified
#   in mwbench main.c), so any nonzero miss is a genuine lost write.
#
# Usage:
#   ./run-mwbench.sh                 # default profile (see below)
#   QUICK=1 ./run-mwbench.sh         # fast ~1 GiB smoke
#   TARGET_GIB=32 ./run-mwbench.sh   # heavier run
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)

IMG=${IMG:-tidesdb/mwbench:9.3.2}

# --- profile (env-overridable) ---------------------------------------------
# Small write-buffer on purpose: it forces many memtable flushes -> many
# SSTables -> real L0->L1 compaction, which is the path we most want to beat
# on. 8 GiB at 64 MiB buffers is ~128 flushes; finishes in a few minutes.
QUICK=${QUICK:-0}
TARGET_GIB=${TARGET_GIB:-8}
VALUE_SIZE=${VALUE_SIZE:-1024}
WRITE_THREADS=${WRITE_THREADS:-4}
READ_THREADS=${READ_THREADS:-4}
FLUSH_THREADS=${FLUSH_THREADS:-4}
COMPACTION_THREADS=${COMPACTION_THREADS:-4}
WRITE_BUFFER=${WRITE_BUFFER:-$((64 * 1024 * 1024))}
BLOCK_CACHE=${BLOCK_CACHE:-$((512 * 1024 * 1024))}
SAMPLE_INTERVAL=${SAMPLE_INTERVAL:-5}

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$BENCH/results/mwbench-$TS"
DATA="$BENCH/results/.mwbench-data-$TS"   # scratch; wiped on exit
mkdir -p "$OUT" "$DATA"

cleanup() { rm -rf "$DATA" 2>/dev/null || true; }
trap cleanup EXIT

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "[mw] ERROR: image $IMG not found. Build it first:" >&2
    echo "      docker build -f docker/Dockerfile.mwbench -t $IMG ." >&2
    exit 2
fi

# --- assemble mwbench args --------------------------------------------------
args=( --data-dir /data/db --out-dir /out
       --write-threads "$WRITE_THREADS" --read-threads "$READ_THREADS"
       --flush-threads "$FLUSH_THREADS" --compaction-threads "$COMPACTION_THREADS"
       --write-buffer "$WRITE_BUFFER" --block-cache "$BLOCK_CACHE"
       --sample-interval-sec "$SAMPLE_INTERVAL" --value-size "$VALUE_SIZE" )
if [ "$QUICK" = "1" ]; then
    args+=( --quick )
    profile="QUICK (~1 GiB)"
else
    args+=( --target-gib "$TARGET_GIB" )
    profile="target=${TARGET_GIB}GiB value=${VALUE_SIZE}B buf=$((WRITE_BUFFER/1024/1024))MiB cache=$((BLOCK_CACHE/1024/1024))MiB w=$WRITE_THREADS r=$READ_THREADS"
fi

echo "[mw] image   : $IMG"
echo "[mw] profile : $profile"
echo "[mw] out     : $OUT"
echo "[mw] === running mwbench (no MySQL; engine-direct) ==="

# Run as the invoking user so the CSVs in $OUT are user-owned. Both mounts are
# user-created host dirs, so a non-root container user can write to them.
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$OUT":/out \
    -v "$DATA":/data \
    "$IMG" "${args[@]}"
run_rc=$?

if [ "$run_rc" -ne 0 ]; then
    echo "[mw] FAIL -- mwbench exited non-zero ($run_rc)"
    exit 1
fi

# --- locate samples.csv -----------------------------------------------------
CSV=$(ls -1dt "$OUT"/run_*/samples.csv 2>/dev/null | head -1)
if [ -z "$CSV" ] || [ ! -s "$CSV" ]; then
    echo "[mw] FAIL -- no samples.csv produced under $OUT"
    exit 1
fi
echo "[mw] samples : $CSV"

# --- verdict: scan every row for misses / mismatches ------------------------
verdict=$(awk -F, '
    NR==1 {
        for (i = 1; i <= NF; i++) col[$i] = i
        need = "point_misses seek_misses range_misses mismatches"
        n = split(need, want, " ")
        for (k = 1; k <= n; k++)
            if (!(want[k] in col)) { print "ERR missing column " want[k]; exit 3 }
        next
    }
    {
        pm = $col["point_misses"]; sm = $col["seek_misses"]
        rm = $col["range_misses"]; mm = $col["mismatches"]
        if (pm+0 > max_pm) max_pm = pm+0
        if (sm+0 > max_sm) max_sm = sm+0
        if (rm+0 > max_rm) max_rm = rm+0
        if (mm+0 > max_mm) max_mm = mm+0
        if ($col["sstable_count"]+0 > max_sst) max_sst = $col["sstable_count"]+0
        last_disk = $col["disk_bytes"]; last_keys = $col["keys_written"]
        rows++
    }
    END {
        printf "rows=%d keys=%s peak_ssts=%d disk=%s\n", rows, last_keys, max_sst, last_disk
        printf "point_misses=%d seek_misses=%d range_misses=%d mismatches=%d\n", \
               max_pm, max_sm, max_rm, max_mm
        if (max_pm==0 && max_sm==0 && max_rm==0 && max_mm==0) print "RESULT PASS"
        else print "RESULT FAIL"
    }' "$CSV")

awk_rc=$?
echo "----------------------------------------------------------------------"
echo "mwbench integrity -- $TS"
echo "image    : $IMG"
echo "profile  : $profile"
echo "$verdict" | grep -vE '^(RESULT|ERR)' | sed 's/^/  /'
echo

if [ "$awk_rc" -eq 3 ] || echo "$verdict" | grep -q '^ERR'; then
    echo "verdict: ERROR -- could not parse integrity columns"
    exit 1
fi

if echo "$verdict" | grep -q '^RESULT PASS'; then
    echo "verdict: PASS -- no lost writes, no corrupted values across the run"
    echo "[mw] artifacts in $OUT"
    exit 0
else
    echo "verdict: FAIL -- mwbench reported lost writes and/or value corruption"
    echo "         (misses already exclude deleted keys; any >0 is real)"
    echo "[mw] inspect $CSV"
    exit 1
fi
