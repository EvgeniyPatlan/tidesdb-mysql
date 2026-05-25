#!/usr/bin/env bash
# Orchestrator: runs every TPROC-C/TPROC-H test we have against the
# v0.2.3 image in a controlled sequence, captures all artifacts under
# one timestamped suite dir, and generates a self-contained REPORT.md
# explaining each test + its result.
#
# Sequential by design -- concurrent Docker work makes this engine
# flaky (we've burned enough cycles learning that). Total ~85 min at
# default profile; ~30 min in SMOKE=1 mode.
#
# Env knobs:
#   SMOKE=1        shrinks every test profile for a fast pipeline check
#   IMAGE          DB image (default tidesdb/mysql:9.7)
#   MWBENCH_IMAGE  engine-integrity image (default tidesdb/mwbench:9.3.0)
#   ENGINE_B       baseline engine for the head-to-head (default innodb)
#   SKIP=<n,n,n>   skip steps by number; step 0 is the mwbench integrity gate
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)
IMAGE=${IMAGE:-tidesdb/mysql:9.7}
MWBENCH_IMAGE=${MWBENCH_IMAGE:-tidesdb/mwbench:9.3.0}
ENGINE_B=${ENGINE_B:-innodb}
SKIP=${SKIP:-}

if [ "${SMOKE:-0}" = 1 ]; then
    BASE_WARE=4;  BASE_VU=2;  BASE_DUR=1
    REC_WARE=2;   REC_VU=1;   REC_PRE=15
    SWEEP_VUS="1 2"
    H2H_WARE=10;  H2H_VU=2;   H2H_DUR=1
    TPCH_SCALE=1; TPCH_VU=2
    SUST_WARE=10; SUST_VU=2;  SUST_DUR=2
else
    BASE_WARE=20; BASE_VU=4;  BASE_DUR=2
    REC_WARE=10;  REC_VU=4;   REC_PRE=30
    SWEEP_VUS="1 4 8"
    H2H_WARE=40;  H2H_VU=4;   H2H_DUR=3
    TPCH_SCALE=1; TPCH_VU=4
    SUST_WARE=80; SUST_VU=8;  SUST_DUR=10
fi
CPUS=${CPUS:-6}; MEM=${MEM:-16g}

TS=$(date -u +%Y%m%dT%H%M%SZ)
SUITE="$BENCH/results/suite-$TS"
mkdir -p "$SUITE/raw"
TS_FMT() { date -u +%Y-%m-%dT%H:%M:%SZ; }
log() { echo "[$(TS_FMT)] [suite] $*"; }

skipped() { case ",$SKIP," in *,$1,*) return 0;; *) return 1;; esac; }

cleanup_containers() { docker ps -aq --filter "name=hdb-\|hdbh-\|rec-" \
                       | xargs -r docker rm -f >/dev/null 2>&1; }

trap 'cleanup_containers; log "trap cleanup"' EXIT

# Helper: most-recently-created result dir matching a glob.
latest_dir() { ls -1dt $1 2>/dev/null | head -1; }

# --- 0. Engine integrity pre-gate (mwbench, no MySQL) ------------------
# Runs FIRST as a fail-fast: stresses the bundled TidesDB engine directly
# (heavy ingest + concurrent byte-verified reads + delete/compact) and
# fails on any lost write or value corruption. Cheap insurance before the
# longer MySQL-level steps. Uses its own image (tidesdb/mwbench).
if ! skipped 0; then
  log "=== STEP 0/6: engine integrity (mwbench, engine-direct) ==="
  if docker image inspect "$MWBENCH_IMAGE" >/dev/null 2>&1; then
    if [ "${SMOKE:-0}" = 1 ]; then MW_ENV=(QUICK=1); else MW_ENV=(TARGET_GIB=8); fi
    env "${MW_ENV[@]}" IMG="$MWBENCH_IMAGE" \
      "$BENCH/mwbench/run-mwbench.sh" > "$SUITE/raw/00-mwbench.log" 2>&1 || true
    D=$(latest_dir "$BENCH/results/mwbench-*")
    [ -n "$D" ] && cp -a "$D" "$SUITE/00-mwbench" 2>/dev/null || true
  else
    log "STEP 0: skipped -- image $MWBENCH_IMAGE not found (build docker/Dockerfile.mwbench)"
  fi
fi

# --- 1. Correctness baseline (TPROC-C, modest profile) -----------------
if ! skipped 1; then
  log "=== STEP 1/6: correctness baseline ==="
  cleanup_containers
  WARE=$BASE_WARE BUILDVU=$BASE_VU RUNVU=$BASE_VU RAMP=1 DUR=$BASE_DUR \
    CPUS=$CPUS MEM=$MEM ENGINE=tidesdb \
    "$HERE/run-hammerdb.sh" > "$SUITE/raw/01-baseline.log" 2>&1 || true
  D=$(latest_dir "$BENCH/results/hammerdb-*")
  [ -n "$D" ] && cp -a "$D" "$SUITE/01-baseline" 2>/dev/null || true
fi

# --- 2. Crash-recovery -------------------------------------------------
if ! skipped 2; then
  log "=== STEP 2/6: crash-recovery (SIGKILL durability) ==="
  cleanup_containers
  WARE=$REC_WARE BUILDVU=$REC_VU RUNVU=$REC_VU PRE_KILL_SECS=$REC_PRE \
    CPUS=$CPUS MEM=$MEM \
    "$HERE/recovery-test.sh" > "$SUITE/raw/02-recovery.log" 2>&1 || true
  D=$(latest_dir "$BENCH/results/recovery-*")
  [ -n "$D" ] && cp -a "$D" "$SUITE/02-recovery" 2>/dev/null || true
fi

# --- 3. VU throughput curve (TPC-C 10:1 WARE:VU ratio) -----------------
if ! skipped 3; then
  log "=== STEP 3/6: VU throughput sweep (VUs=$SWEEP_VUS) ==="
  cleanup_containers
  VUS="$SWEEP_VUS" WARE_RATIO=10 WARE_MIN=10 BUILDVU=4 RAMP=1 DUR=$BASE_DUR \
    CPUS=$CPUS MEM=$MEM \
    "$HERE/sweep-hammerdb.sh" > "$SUITE/raw/03-sweep.log" 2>&1 || true
  D=$(latest_dir "$BENCH/results/sweep-*")
  [ -n "$D" ] && cp -a "$D" "$SUITE/03-sweep" 2>/dev/null || true
fi

# --- 4. Head-to-head: TidesDB vs InnoDB --------------------------------
if ! skipped 4; then
  log "=== STEP 4/6a: head-to-head -- ENGINE=tidesdb ==="
  cleanup_containers
  ENGINE=tidesdb WARE=$H2H_WARE BUILDVU=$H2H_VU RUNVU=$H2H_VU \
    RAMP=1 DUR=$H2H_DUR CPUS=$CPUS MEM=$MEM \
    "$HERE/run-hammerdb.sh" > "$SUITE/raw/04a-h2h-tidesdb.log" 2>&1 || true
  D=$(latest_dir "$BENCH/results/hammerdb-*")
  [ -n "$D" ] && cp -a "$D" "$SUITE/04a-h2h-tidesdb" 2>/dev/null || true

  log "=== STEP 4/6b: head-to-head -- ENGINE=$ENGINE_B ==="
  cleanup_containers
  ENGINE=$ENGINE_B WARE=$H2H_WARE BUILDVU=$H2H_VU RUNVU=$H2H_VU \
    RAMP=1 DUR=$H2H_DUR CPUS=$CPUS MEM=$MEM \
    "$HERE/run-hammerdb.sh" > "$SUITE/raw/04b-h2h-$ENGINE_B.log" 2>&1 || true
  D=$(latest_dir "$BENCH/results/hammerdb-*")
  [ -n "$D" ] && cp -a "$D" "$SUITE/04b-h2h-$ENGINE_B" 2>/dev/null || true
fi

# --- 5. TPROC-H read-path (SF=1 build + multi-VU power test) -----------
if ! skipped 5; then
  log "=== STEP 5/6: TPROC-H read path ==="
  cleanup_containers
  SCALE=$TPCH_SCALE BUILDVU=$TPCH_VU RUNVU=$TPCH_VU CPUS=$CPUS MEM=$MEM \
    "$HERE/run-hammerdb-tproch.sh" > "$SUITE/raw/05-tproch.log" 2>&1 || true
  D=$(latest_dir "$BENCH/results/hammerdb-tproch-*")
  [ -n "$D" ] && cp -a "$D" "$SUITE/05-tproch" 2>/dev/null || true
fi

# --- 6. Sustained TPROC-C ----------------------------------------------
if ! skipped 6; then
  log "=== STEP 6/6: sustained (${SUST_DUR}m steady-state) ==="
  cleanup_containers
  WARE=$SUST_WARE BUILDVU=$SUST_VU RUNVU=$SUST_VU RAMP=1 DUR=$SUST_DUR \
    CPUS=$CPUS MEM=$MEM \
    "$HERE/run-hammerdb.sh" > "$SUITE/raw/06-sustained.log" 2>&1 || true
  D=$(latest_dir "$BENCH/results/hammerdb-*")
  [ -n "$D" ] && cp -a "$D" "$SUITE/06-sustained" 2>/dev/null || true
fi

log "=== suite complete -- generating REPORT.md ==="
"$HERE/render-report.sh" "$SUITE" "$IMAGE" "$ENGINE_B" "$CPUS" "$MEM" \
  > "$SUITE/REPORT.md"
log "artifacts: $SUITE"
log "report   : $SUITE/REPORT.md"
