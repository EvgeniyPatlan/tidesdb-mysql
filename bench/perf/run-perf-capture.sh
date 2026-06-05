#!/usr/bin/env bash
# ============================================================================
#  TidesDB-MySQL perf-instrumentation capture harness.
#
#  Pairs with tools/tidesdb_perf_analyze. Wraps bench/hammerdb/run-hammerdb.sh
#  by forcing tidesdb_perf_capture=ON via DB_EXTRA_ARGS, mounting the perf
#  output dir as a volume so files land on host, and running the analyser
#  after the workload completes.
#
#  Optional COMPARE=1 mode runs the same workload against
#  sut-mariadb-tidesdb:9.3.0 and emits a side-by-side --compare diff.
#
#  Usage:
#    ./run-perf-capture.sh                          # MySQL-only capture
#    COMPARE=1 ./run-perf-capture.sh                # MySQL + MariaDB + diff
#    SMOKE=1 ./run-perf-capture.sh                  # WARE=2 RUNVU=1 1m -- pipeline check
#    WARE=10 RUNVU=8 DUR=3 ./run-perf-capture.sh    # explicit profile
#
#  Output:
#    bench/results/perf-<ts>/mysql/   .bin + meta.json + report.md
#    bench/results/perf-<ts>/mariadb/ (if COMPARE=1)
#    bench/results/perf-<ts>/diff.md  (if COMPARE=1)
#
#  KNOWN LIMITATION (v0.4.1): the perf-flavoured plugin .so links against
#  glibc 2.39 (from the tides-builder Ubuntu 24.04 image), but the runtime
#  image base is Oracle Linux 9 with glibc 2.34. This causes dlopen to fail
#  with "GLIBC_2.38 not found". Tracked as a Task 13 release-validation
#  work item: either rebuild tides-builder on Oracle Linux 9, or rebase the
#  runtime image on a newer distro. Until then, this harness is for code
#  review and CI lint only; it will not produce a successful run end-to-end.
# ============================================================================
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)

# --- profile (env-overridable) ---------------------------------------------
if [ "${SMOKE:-0}" = 1 ]; then
    WARE=${WARE:-2}; BUILDVU=${BUILDVU:-1}; RUNVU=${RUNVU:-1}
    RAMP=${RAMP:-1}; DUR=${DUR:-1}
else
    WARE=${WARE:-10}; BUILDVU=${BUILDVU:-4}; RUNVU=${RUNVU:-8}
    RAMP=${RAMP:-1};  DUR=${DUR:-3}
fi
CPUS=${CPUS:-4}; MEM=${MEM:-12g}

MYSQL_IMAGE=${MYSQL_IMAGE:-tidesdb/mysql:9.7-perf}
MARIA_IMAGE=${MARIA_IMAGE:-sut-mariadb-tidesdb:9.3.0-perf}
COMPARE=${COMPARE:-0}

# Perf sysvars (the v0.4.1 surface).
PERF_CAPTURE=${PERF_CAPTURE:-ON}
PERF_DIR=${PERF_DIR:-/tmp/perf-out}
PERF_FLUSH_MS=${PERF_FLUSH_MS:-200}

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$BENCH/results/perf-$TS"
mkdir -p "$OUT/mysql"
[ "$COMPARE" = 1 ] && mkdir -p "$OUT/mariadb"

# --- helpers ----------------------------------------------------------------
clean_perf_out() {
    sg docker -c "docker run --rm -v ${PERF_DIR}:/x alpine sh -c 'rm -rf /x/* /x/.[!.]* 2>/dev/null || true'"
}

run_sut() {
    local image=$1 label=$2 dest=$3
    echo "[perf] === $label SUT ($image) ==="

    clean_perf_out

    # Tag the perf image as tidesdb/mysql:9.7 so run-hammerdb.sh picks it up.
    # Save the current tag (if any) so we can restore it after.
    local saved=""
    if sg docker -c "docker image inspect tidesdb/mysql:9.7" > /dev/null 2>&1; then
        saved=$(sg docker -c "docker image inspect -f '{{.Id}}' tidesdb/mysql:9.7")
        sg docker -c "docker tag $saved tidesdb/mysql:9.7-runperf-saved"
        sg docker -c "docker rmi tidesdb/mysql:9.7" > /dev/null 2>&1 || true
    fi
    sg docker -c "docker tag $image tidesdb/mysql:9.7"

    # Run HammerDB with perf sysvars forced ON. The harness's docker run
    # command needs the perf-dir volume mount; if bench/hammerdb/run-hammerdb.sh
    # doesn't include it, this harness symlinks a modified version.
    DB_EXTRA_ARGS="--loose_tidesdb_perf_capture=$PERF_CAPTURE --loose_tidesdb_perf_output_dir=$PERF_DIR --loose_tidesdb_perf_flush_interval_ms=$PERF_FLUSH_MS" \
    WARE=$WARE BUILDVU=$BUILDVU RUNVU=$RUNVU RAMP=$RAMP DUR=$DUR CPUS=$CPUS MEM=$MEM \
    KEEP_DB=0 \
        sg docker -c "cd $(pwd) && DB_EXTRA_ARGS='--loose_tidesdb_perf_capture=$PERF_CAPTURE --loose_tidesdb_perf_output_dir=$PERF_DIR --loose_tidesdb_perf_flush_interval_ms=$PERF_FLUSH_MS' WARE=$WARE BUILDVU=$BUILDVU RUNVU=$RUNVU RAMP=$RAMP DUR=$DUR CPUS=$CPUS MEM=$MEM $HERE/run-hammerdb-perf.sh 2>&1" \
        > "$dest/hammerdb.log" 2>&1 || true
    cp -r "$PERF_DIR"/. "$dest/" 2>/dev/null || true

    # Restore the original tidesdb/mysql:9.7 tag if we saved one.
    if [ -n "$saved" ]; then
        sg docker -c "docker rmi tidesdb/mysql:9.7" > /dev/null 2>&1 || true
        sg docker -c "docker tag tidesdb/mysql:9.7-runperf-saved tidesdb/mysql:9.7"
        sg docker -c "docker rmi tidesdb/mysql:9.7-runperf-saved" > /dev/null 2>&1 || true
    fi

    # Analyse.
    if ls "$dest"/*-meta.json > /dev/null 2>&1; then
        python3 -m tidesdb_perf_analyze "$dest" --output "$dest/report.md" \
            2> "$dest/analyse.err" || echo "[perf] $label analyse failed; see $dest/analyse.err"
        echo "[perf] $label report -> $dest/report.md"
    else
        echo "[perf] $label produced no perf data (likely the GLIBC ABI skew limitation -- see script header)"
    fi
}

# --- main -------------------------------------------------------------------
mkdir -p "$PERF_DIR"; chmod 777 "$PERF_DIR"

run_sut "$MYSQL_IMAGE" mysql "$OUT/mysql"

if [ "$COMPARE" = 1 ]; then
    run_sut "$MARIA_IMAGE" mariadb "$OUT/mariadb"
    if [ -f "$OUT/mysql/report.md" ] && [ -f "$OUT/mariadb/report.md" ]; then
        python3 -m tidesdb_perf_analyze.compare \
            "$OUT/mysql" "$OUT/mariadb" \
            --left-label MySQL --right-label MariaDB \
            --output "$OUT/diff.md" \
            2> "$OUT/diff.err" || echo "[perf] compare failed; see $OUT/diff.err"
        echo "[perf] diff -> $OUT/diff.md"
    fi
fi

echo "[perf] done -- $OUT"
