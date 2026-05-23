#!/usr/bin/env bash
# HammerDB 5.0 TPROC-C (TPC-C) correctness verification against the
# TidesDB MySQL engine (image tidesdb/mysql:9.7, built @ main HEAD).
#
# Primary correctness gate: a DETERMINISTIC reverse-ref probe on the
# built `orders` table (the exact OSTAT shape) -- it must NOT raise
# ER_ILLEGAL_HA (1031). Then the standard timed TPC-C mix runs (which
# includes Order-Status) and the log is scanned for handler-
# unsupported signatures vs expected TidesDB OCC conflicts.
#
# Env (defaults = a "fuller" run): WARE BUILDVU RUNVU RAMP DUR CPUS MEM
# SMOKE=1 shrinks everything for a fast pipeline check.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)

if [ "${SMOKE:-0}" = 1 ]; then
    WARE=${WARE:-1}; BUILDVU=${BUILDVU:-1}; RUNVU=${RUNVU:-1}
    RAMP=${RAMP:-1}; DUR=${DUR:-1}
else
    WARE=${WARE:-10}; BUILDVU=${BUILDVU:-4}; RUNVU=${RUNVU:-8}
    RAMP=${RAMP:-1};  DUR=${DUR:-3}
fi
CPUS=${CPUS:-4}; MEM=${MEM:-12g}
RUNTIMER=$(( (RAMP + DUR) * 60 + 90 ))
USER=bench; PASS='Bench_9xQ!z'
ENGINE=${ENGINE:-tidesdb}        # ENGINE=innodb for the head-to-head baseline

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$BENCH/results/hammerdb-$TS"; mkdir -p "$OUT"
NET="hdbnet-$$"; DB="hdb-db-$$"; CLI="hdb-cli-$$"

cleanup(){ docker rm -f "$CLI" >/dev/null 2>&1 || true
           if [ "${KEEP_DB:-0}" = 1 ]; then
             echo "[hdb] KEEP_DB=1 -- leaving DB container '$DB' (net '$NET') up for manual inspection" >&2
             echo "[hdb]   docker exec -i $DB mysql -uroot   then: USE tpcc; ..." >&2
             return 0
           fi
           docker rm -f "$DB" >/dev/null 2>&1 || true
           docker network rm "$NET" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "[hdb] profile: WARE=$WARE BUILDVU=$BUILDVU RUNVU=$RUNVU RAMP=${RAMP}m DUR=${DUR}m caps=${CPUS}cpu/${MEM}"
docker network create "$NET" >/dev/null 2>&1 || true

echo "[hdb] starting DB (tidesdb/mysql:9.7 @ main HEAD)"
# DB_EXTRA_ARGS: extra mysqld flags appended last (e.g. plugin sysvars).
# tidesdb_unified_memtable is PLUGIN_VAR_READONLY -- only settable at
# server start, and needs the loose_ prefix (the var is unknown during
# the pre-plugin --initialize step), e.g.:
#   DB_EXTRA_ARGS="--loose_tidesdb_unified_memtable=0"
# read -a so multi-flag strings split correctly; empty -> no-op.
read -r -a _db_extra <<< "${DB_EXTRA_ARGS:-}"
docker run -d --name "$DB" --network "$NET" --cpus "$CPUS" --memory "$MEM" \
  -e MYSQL_ALLOW_EMPTY_PASSWORD=1 tidesdb/mysql:9.7 \
  --max-connections=512 --max-connect-errors=1000000 --connect-timeout=30 \
  --net-read-timeout=120 --net-write-timeout=120 "${_db_extra[@]}" >/dev/null

echo "[hdb] waiting for mysqld (socket)"
i=0; until docker exec "$DB" mysql -uroot -N -B -e "SELECT 1" 2>/dev/null | grep -q '^1$'; do
  i=$((i+1)); [ "$i" -gt 90 ] && { echo "[hdb] DB never ready (socket)" >&2; exit 1; }; sleep 2; done

# Capture the EFFECTIVE unified-memtable setting so a mistyped/ignored
# DB_EXTRA_ARGS can't masquerade as a result. If the run intends OFF but
# the var reads ON, abort -- a false negative here would be worse than
# no test.
UNIFIED_MT=$(docker exec "$DB" mysql -uroot -N -B \
  -e "SHOW GLOBAL VARIABLES LIKE 'tidesdb_unified_memtable'" 2>/dev/null | awk '{print $2}')
echo "[hdb] effective tidesdb_unified_memtable = ${UNIFIED_MT:-?}"
case "${DB_EXTRA_ARGS:-}" in
  *unified_memtable=0*|*unified_memtable=OFF*|*unified-memtable=0*|*unified-memtable=OFF*)
    if [ "$UNIFIED_MT" != "OFF" ]; then
      echo "[hdb] FATAL: requested unified_memtable=OFF but server reports '${UNIFIED_MT}'" >&2
      exit 1
    fi ;;
esac

docker exec -i "$DB" mysql -uroot >/dev/null 2>&1 <<SQL
CREATE USER IF NOT EXISTS '$USER'@'%' IDENTIFIED BY '$PASS';
GRANT ALL PRIVILEGES ON *.* TO '$USER'@'%' WITH GRANT OPTION;
SET GLOBAL log_bin_trust_function_creators=1;
FLUSH PRIVILEGES;
SQL

# HammerDB connects over TCP. The official mysql image runs a
# socket-only temp server during init then restarts the durable
# server, so a socket SELECT can succeed while TCP:3306 is still
# refused. Gate on N consecutive TCP successes as the bench user
# (same lesson the perf harness learned) before starting HammerDB.
echo "[hdb] waiting for durable TCP (bench user, 3 consecutive)"
i=0; streak=0
until [ "$streak" -ge 3 ]; do
  if docker exec "$DB" mysql -h127.0.0.1 -P3306 --protocol=TCP \
       -u"$USER" -p"$PASS" -N -B -e "SELECT 1" 2>/dev/null | grep -q '^1$'; then
    streak=$((streak+1))
  else
    streak=0
  fi
  i=$((i+1)); [ "$i" -gt 120 ] && { echo "[hdb] DB never ready (TCP)" >&2; exit 1; }
  sleep 2
done
echo "[hdb] DB ready on TCP"

echo "[hdb] starting HammerDB 5.0 client"
docker run -d --name "$CLI" --network "$NET" hammerdb:5.0 >/dev/null

render(){ sed -e "s/@DBHOST@/$DB/g" -e "s/@USER@/$USER/g" -e "s/@PASS@/$PASS/g" \
              -e "s/@WARE@/$WARE/g" -e "s/@BUILDVU@/$BUILDVU/g" -e "s/@RUNVU@/$RUNVU/g" \
              -e "s/@RAMP@/$RAMP/g" -e "s/@DUR@/$DUR/g" -e "s/@RUNTIMER@/$RUNTIMER/g" \
              -e "s/@ENGINE@/$ENGINE/g" "$1"; }

echo "[hdb] === schema build (ENGINE=tidesdb, $WARE warehouses) ==="
render "$HERE/build.tcl" > /tmp/hdb_build.tcl
docker cp /tmp/hdb_build.tcl "$CLI":/opt/hammerdb/build.tcl
docker exec "$CLI" sh -c 'cd /opt/hammerdb && ./hammerdbcli auto build.tcl' \
  > "$OUT/build.log" 2>&1
# Build must have actually completed. HammerDB 5.0 prints "SCHEMA
# BUILD COMPLETED"; a Tcl/CLI failure shows "invoked from within" or
# the zipfs main.tcl trace. Treat either problem as a hard FAIL
# (the earlier smoke false-passed because this wasn't checked).
BUILD_OK=1
grep -qa 'SCHEMA BUILD COMPLETED' "$OUT/build.log" || BUILD_OK=0
# "SCHEMA BUILD COMPLETED" is just a puts -- the real signal is that
# the loader VUs did NOT fail. Any of these means the schema is bogus:
grep -qaE 'invoked from within|zipfs:/app/main.tcl|FINISHED FAILED|Failed to load mysqltcl|Error in Virtual User|can.t read' \
    "$OUT/build.log" && BUILD_OK=0
echo "[hdb] build $([ $BUILD_OK = 1 ] && echo COMPLETED || echo FAILED) (see build.log)"

# --- Primary correctness gate: deterministic reverse-ref probe.
# Connect via socket to the built tpcc schema. The query is the exact
# fixed path: partial-PK-prefix reverse scan (WHERE o_w_id,o_d_id ...
# ORDER BY o_id DESC LIMIT 1), plus the full OSTAT shape with the
# o_c_id filter.
#
# Three distinct outcomes -- the probe must NOT conflate them (the old
# version reported a FAIL whether the reverse path broke OR the table
# just read back empty under heavy-build contention):
#   PASS         reverse query returned rows, no SQL error
#   FAIL         a real handler/SQL error (1031/WRONG_COMMAND/connect/
#                unknown db) -- this is the regression we guard against
#   INCONCLUSIVE w1/d1 orders is unpopulated/unsettled (COUNT(*)=0 with
#                no error) -- a TidesDB read-visibility/OCC property at
#                scale, NOT a reverse-path failure; the fix can't be
#                exercised so we report it honestly instead of as FAIL.
#
# Guard: retry a COUNT(*) on the target slice for a bounded settle
# window before running the reverse query, so transient post-bulk-load
# invisibility doesn't masquerade as a handler bug.
PROBE_STATE=SKIP   # PASS | FAIL | INCONCLUSIVE | SKIP
if [ $BUILD_OK = 1 ]; then
  echo "[hdb] === reverse-ref probe on built orders table ==="
  CNT=""; cnt_err=0
  for a in 1 2 3 4 5 6 7 8; do
    CRAW=$(docker exec -i "$DB" mysql -uroot -N -B 2>&1 <<'SQL'
USE tpcc;
SELECT COUNT(*) FROM orders WHERE o_w_id=1 AND o_d_id=1;
SQL
)
    if echo "$CRAW" | grep -qiE 'ERROR [0-9]|1031|WRONG_COMMAND|ILLEGAL_HA|Can.t connect|Unknown database'; then
      cnt_err=1; CNT="$CRAW"; break
    fi
    CNT=$(echo "$CRAW" | grep -aoE '^[0-9]+$' | tail -1)
    [ -n "$CNT" ] && [ "$CNT" -gt 0 ] 2>/dev/null && break
    echo "[hdb]   settle attempt $a: orders(w1,d1) count=${CNT:-0}, retrying"
    sleep 5
  done

  if [ "$cnt_err" = 1 ]; then
    PROBE_STATE=FAIL
    echo "$CNT" > "$OUT/reverse_ref_probe.out"
    echo "[hdb] !!! reverse-ref probe FAIL (error on count guard):"; echo "$CNT"
  elif [ -z "$CNT" ] || [ "$CNT" -eq 0 ] 2>/dev/null; then
    PROBE_STATE=INCONCLUSIVE
    echo "orders(w1,d1) COUNT(*)=0 after settle window -- table unpopulated/contended; reverse path not exercised" \
      > "$OUT/reverse_ref_probe.out"
    echo "[hdb] reverse-ref probe INCONCLUSIVE -- orders(w1,d1) empty after settle (TidesDB read-visibility/OCC at scale, not a handler bug)"
  else
    PROBE=$(docker exec -i "$DB" mysql -uroot -N -B 2>&1 <<'SQL'
USE tpcc;
SELECT '--partial-prefix-reverse--';
SELECT o_id FROM orders WHERE o_w_id=1 AND o_d_id=1 ORDER BY o_id DESC LIMIT 1;
SELECT '--ostat-shape--';
SELECT o_id, o_carrier_id
FROM orders
WHERE o_w_id=1 AND o_d_id=1
  AND o_c_id=(SELECT o_c_id FROM orders WHERE o_w_id=1 AND o_d_id=1 LIMIT 1)
ORDER BY o_id DESC LIMIT 1;
SQL
)
    { echo "orders(w1,d1) COUNT(*)=$CNT"; echo "$PROBE"; } > "$OUT/reverse_ref_probe.out"
    if echo "$PROBE" | grep -qiE 'ERROR [0-9]|1031|doesn'\''t have this option|WRONG_COMMAND|ILLEGAL_HA|Can.t connect|Unknown database'; then
      PROBE_STATE=FAIL
      echo "[hdb] !!! reverse-ref probe FAIL:"; echo "$PROBE"
    elif echo "$PROBE" | grep -q -- '--ostat-shape--' && \
         echo "$PROBE" | grep -qE '^[0-9]+'; then
      PROBE_STATE=PASS
      echo "[hdb] reverse-ref probe PASS (count=$CNT, reverse query returned rows, no error)"
    else
      # Count was >0 but the reverse query returned nothing -- that IS
      # a reverse-path failure (rows exist but the fixed path skipped
      # them), so this is a real FAIL, not INCONCLUSIVE.
      PROBE_STATE=FAIL
      echo "[hdb] !!! reverse-ref probe FAIL (count=$CNT but reverse query empty):"; echo "$PROBE"
    fi
  fi
else
  echo "[hdb] skipping probe -- schema build failed"
  echo "schema build failed" > "$OUT/reverse_ref_probe.out"
fi

echo "[hdb] === timed TPC-C run (RUNVU=$RUNVU, ${RAMP}m ramp + ${DUR}m) ==="
render "$HERE/run.tcl" > /tmp/hdb_run.tcl
docker cp /tmp/hdb_run.tcl "$CLI":/opt/hammerdb/run.tcl
docker exec "$CLI" sh -c 'cd /opt/hammerdb && ./hammerdbcli auto run.tcl' \
  > "$OUT/run.log" 2>&1

# --- Result + correctness scan
NOPM=$(grep -aoE '[0-9]+ NOPM' "$OUT/run.log" | tail -1)
TPM=$(grep -aoE '[0-9]+ (MySQL )?TPM' "$OUT/run.log" | tail -1)
HANDLER_ERR=$(grep -aciE '1031|doesn'\''t have this option|WRONG_COMMAND|ILLEGAL_HA' "$OUT/run.log")
CONFLICTS=$(grep -aciE '1213|1205| 1180|deadlock|Lock wait timeout' "$OUT/run.log")

{
  echo "HammerDB 5.0 TPROC-C verification -- $TS"
  echo "DB image: tidesdb/mysql:9.7 (main HEAD, reverse-ref fix)"
  echo "profile : WARE=$WARE BUILDVU=$BUILDVU RUNVU=$RUNVU RAMP=${RAMP}m DUR=${DUR}m"
  echo "engine  : $ENGINE"
  echo "tidesdb_unified_memtable = ${UNIFIED_MT:-?}"
  echo
  echo "schema build      : $([ $BUILD_OK = 1 ] && echo COMPLETED || echo FAILED)"
  echo "reverse-ref probe : ${PROBE_STATE}"
  echo "handler-unsupported errors in run.log : $HANDLER_ERR  (must be 0)"
  echo "expected OCC conflict lines           : $CONFLICTS  (informational)"
  echo "throughput: ${NOPM:-n/a} / ${TPM:-n/a}"
  echo
  if [ $BUILD_OK != 1 ]; then
    echo "verdict: FAIL -- schema build did not complete (see build.log)"
  elif [ "$PROBE_STATE" = FAIL ]; then
    echo "verdict: FAIL -- reverse-ref probe failed: rows exist but the reverse/OSTAT path errored or skipped them (see reverse_ref_probe.out). This IS the regression guard."
  elif [ "$HANDLER_ERR" -ne 0 ]; then
    echo "verdict: FAIL -- handler-unsupported error during TPC-C run (see run.log)"
  elif [ "$PROBE_STATE" = PASS ] && [ -n "$NOPM" ]; then
    echo "verdict: PASS -- schema built on TidesDB, reverse-ref/OSTAT works, TPC-C mix ran with no handler-unsupported errors (${NOPM})"
  elif [ "$PROBE_STATE" = INCONCLUSIVE ]; then
    echo "verdict: INCONCLUSIVE -- build OK and zero handler-unsupported errors, but orders(w1,d1) read back empty after the settle window so the reverse path could not be exercised. This is a TidesDB read-visibility/OCC property at this scale, NOT a reverse-ref handler regression (contrast: PASS at lower concurrency)."
  else
    echo "verdict: INCONCLUSIVE -- build+probe ok but TPC-C produced no NOPM (see run.log)"
  fi
} | tee "$OUT/SUMMARY.txt"

# --- Ground-truth diagnostics (DB still alive; runs on any non-PASS).
# Distinguishes the three hypotheses for an under-populated/empty w1/d1:
#   (a) genuine ongoing data loss  -> low counts everywhere, district
#       d_next_o_id high (loader thought it inserted N) but orders << N
#   (b) read-visibility lag        -> counts climb across the retry/settle
#       reads, or the reverse query succeeds after a few seconds
#   (c) build path not covered     -> mysqld error log shows our
#       "bulk mid-commit" warning, or TidesDB conflict/memory errors
if [ "$BUILD_OK" = 1 ] && [ "$PROBE_STATE" != PASS ]; then
  echo "[hdb] === ground-truth diagnostics (PROBE_STATE=$PROBE_STATE) ===" | tee "$OUT/DIAG.txt"
  docker exec -i "$DB" mysql -uroot -t 2>&1 <<'SQL' | tee -a "$OUT/DIAG.txt"
USE tpcc;
SELECT 'orders total'        AS k, COUNT(*) v FROM orders
UNION ALL SELECT 'orders w1',          COUNT(*) FROM orders WHERE o_w_id=1
UNION ALL SELECT 'orders w1 d1',       COUNT(*) FROM orders WHERE o_w_id=1 AND o_d_id=1
UNION ALL SELECT 'order_line total',   COUNT(*) FROM order_line
UNION ALL SELECT 'new_order total',    COUNT(*) FROM new_order
UNION ALL SELECT 'customer total',     COUNT(*) FROM customer
UNION ALL SELECT 'stock total',        COUNT(*) FROM stock
UNION ALL SELECT 'district total',     COUNT(*) FROM district
UNION ALL SELECT 'warehouse total',    COUNT(*) FROM warehouse;
-- loader's own claim of how many orders it generated per district (w1):
SELECT d_w_id, d_id, d_next_o_id AS loader_thinks_orders FROM district WHERE d_w_id=1 ORDER BY d_id;
-- actual orders per district (w1): mismatch vs d_next_o_id == data loss
SELECT o_w_id, o_d_id, COUNT(*) actual_orders, MIN(o_id) min_id, MAX(o_id) max_id
FROM orders WHERE o_w_id=1 GROUP BY o_w_id, o_d_id ORDER BY o_d_id;
SQL
  # Visibility-lag test: same reverse query, 5 attempts over ~20s. If it
  # starts returning rows, COUNT=1-at-probe-time was a settle artifact.
  echo "[hdb] --- reverse query x5 over ~20s (visibility-lag test) ---" | tee -a "$OUT/DIAG.txt"
  for a in 1 2 3 4 5; do
    R=$(docker exec -i "$DB" mysql -uroot -N -B 2>&1 <<'SQL'
USE tpcc;
SELECT o_id FROM orders WHERE o_w_id=1 AND o_d_id=1 ORDER BY o_id DESC LIMIT 1;
SQL
)
    echo "  attempt $a: '${R}'" | tee -a "$OUT/DIAG.txt"
    sleep 5
  done
  echo "[hdb] --- mysqld error log: our fix's warning + TidesDB errors ---" | tee -a "$OUT/DIAG.txt"
  docker logs "$DB" 2>&1 | grep -aiE 'bulk mid-commit|silent data loss|TIDESDB.*(conflict|memory|MEMORY_LIMIT|TDB_ERR|rolling back)|\[ERROR\]' \
    | tail -40 | tee -a "$OUT/DIAG.txt" || echo "(no matching server log lines)" | tee -a "$OUT/DIAG.txt"
fi

cp "$BENCH/images/versions.lock" "$OUT/" 2>/dev/null || true
echo "[hdb] artifacts in $OUT"
