#!/usr/bin/env bash
# HammerDB 5.0 TPROC-H (TPC-H) power test against tidesdb/mysql:9.7.
#
# Complements run-hammerdb.sh (TPROC-C / OLTP). TPROC-H exercises the
# read/scan path the OLTP mix barely touches: sequential SSTable
# iteration, block cache, bloom filters, range scans, joins, large
# aggregations -- where the LSM design actually shows.
#
# Primary signal: schema row counts match TPC-H spec (lineitem
# = 6,001,215 at SF=1) -- if the bulk-load loss returns (e.g. someone
# re-enables unified_memtable=ON without the upstream fix) this is
# where it'll surface. Secondary: per-query latencies + geometric mean
# from the 22-query power test (HammerDB prints them).
#
# Env: SCALE BUILDVU RUNVU CPUS MEM DB_EXTRA_ARGS KEEP_DB
#   SMOKE=1 -> scale_fact=1 (~6M lineitem rows, ~1GB raw)
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)

if [ "${SMOKE:-0}" = 1 ]; then
    SCALE=${SCALE:-1}; BUILDVU=${BUILDVU:-2}; RUNVU=${RUNVU:-1}
else
    SCALE=${SCALE:-1}; BUILDVU=${BUILDVU:-4}; RUNVU=${RUNVU:-1}
fi
CPUS=${CPUS:-6}; MEM=${MEM:-16g}
USER=bench; PASS='Bench_9xQ!z'

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$BENCH/results/hammerdb-tproch-$TS"; mkdir -p "$OUT"
NET="hdbhnet-$$"; DB="hdbh-db-$$"; CLI="hdbh-cli-$$"

cleanup(){ docker rm -f "$CLI" >/dev/null 2>&1 || true
           if [ "${KEEP_DB:-0}" = 1 ]; then
             echo "[hdbh] KEEP_DB=1 -- leaving '$DB' (net '$NET') up" >&2
             return 0
           fi
           docker rm -f "$DB" >/dev/null 2>&1 || true
           docker network rm "$NET" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "[hdbh] profile: SCALE=$SCALE BUILDVU=$BUILDVU RUNVU=$RUNVU caps=${CPUS}cpu/${MEM}"
docker network create "$NET" >/dev/null 2>&1 || true

read -r -a _db_extra <<< "${DB_EXTRA_ARGS:-}"
echo "[hdbh] starting DB (tidesdb/mysql:9.7)"
docker run -d --name "$DB" --network "$NET" --cpus "$CPUS" --memory "$MEM" \
  -e MYSQL_ALLOW_EMPTY_PASSWORD=1 tidesdb/mysql:9.7 \
  --max-connections=512 --max-connect-errors=1000000 --connect-timeout=30 \
  --net-read-timeout=300 --net-write-timeout=300 "${_db_extra[@]}" >/dev/null

echo "[hdbh] waiting for mysqld (socket)"
i=0; until docker exec "$DB" mysql -uroot -N -B -e "SELECT 1" 2>/dev/null | grep -q '^1$'; do
  i=$((i+1)); [ "$i" -gt 90 ] && { echo "[hdbh] DB never ready (socket)" >&2; exit 1; }; sleep 2; done

UNIFIED_MT=$(docker exec "$DB" mysql -uroot -N -B \
  -e "SHOW GLOBAL VARIABLES LIKE 'tidesdb_unified_memtable'" 2>/dev/null | awk '{print $2}')
echo "[hdbh] effective tidesdb_unified_memtable = ${UNIFIED_MT:-?}"

docker exec -i "$DB" mysql -uroot >/dev/null 2>&1 <<SQL
CREATE USER IF NOT EXISTS '$USER'@'%' IDENTIFIED BY '$PASS';
GRANT ALL PRIVILEGES ON *.* TO '$USER'@'%' WITH GRANT OPTION;
SET GLOBAL log_bin_trust_function_creators=1;
FLUSH PRIVILEGES;
SQL

echo "[hdbh] waiting for durable TCP (bench user, 3 consecutive)"
i=0; streak=0
until [ "$streak" -ge 3 ]; do
  if docker exec "$DB" mysql -h127.0.0.1 -P3306 --protocol=TCP \
       -u"$USER" -p"$PASS" -N -B -e "SELECT 1" 2>/dev/null | grep -q '^1$'; then
    streak=$((streak+1)); else streak=0; fi
  i=$((i+1)); [ "$i" -gt 120 ] && { echo "[hdbh] DB never ready (TCP)" >&2; exit 1; }
  sleep 2
done
echo "[hdbh] DB ready on TCP"

echo "[hdbh] starting HammerDB 5.0 client"
docker run -d --name "$CLI" --network "$NET" hammerdb:5.0 >/dev/null

render(){ sed -e "s/@DBHOST@/$DB/g" -e "s/@USER@/$USER/g" -e "s/@PASS@/$PASS/g" \
              -e "s/@SCALE@/$SCALE/g" -e "s/@BUILDVU@/$BUILDVU/g" -e "s/@RUNVU@/$RUNVU/g" "$1"; }

echo "[hdbh] === schema build (ENGINE=tidesdb, SF=$SCALE) ==="
render "$HERE/build-tproch.tcl" > /tmp/hdbh_build.tcl
docker cp /tmp/hdbh_build.tcl "$CLI":/opt/hammerdb/build.tcl
docker exec "$CLI" sh -c 'cd /opt/hammerdb && ./hammerdbcli auto build.tcl' \
  > "$OUT/build.log" 2>&1
BUILD_OK=1
grep -qa 'SCHEMA BUILD COMPLETED' "$OUT/build.log" || BUILD_OK=0
grep -qaE 'invoked from within|zipfs:/app/main.tcl|FINISHED FAILED|Failed to load mysqltcl|Error in Virtual User|can.t read' \
    "$OUT/build.log" && BUILD_OK=0
echo "[hdbh] build $([ $BUILD_OK = 1 ] && echo COMPLETED || echo FAILED)"

# --- Row-count gate: TPC-H schema sizes are exact at each scale factor.
# Mismatch == data loss (the bug v0.2.2 fixes; this also catches a
# regression if anyone re-enables unified_memtable=ON without the
# upstream rotation-race fix). Spec at SF=1: 6,001,215 lineitems.
# Other tables scale linearly; we sanity-check the largest two.
#
# HammerDB's mysql TPROC-H builder creates tables UPPERCASE
# (LINEITEM/ORDERS/...), which on Linux's case-sensitive filesystem
# with MySQL default `lower_case_table_names=0` is the literal table
# name. Use the exact name HammerDB writes -- a lowercase query would
# error "table doesn't exist" and look like data loss.
LI_EXPECTED=$((SCALE * 6001215))
ORD_EXPECTED=$((SCALE * 1500000))
ROW_OK=0
if [ $BUILD_OK = 1 ]; then
  LI_ACTUAL=$(docker exec "$DB" mysql -uroot -N -B \
    -e "SELECT COUNT(*) FROM tpch.LINEITEM" 2>/dev/null)
  ORD_ACTUAL=$(docker exec "$DB" mysql -uroot -N -B \
    -e "SELECT COUNT(*) FROM tpch.ORDERS" 2>/dev/null)
  echo "[hdbh] LINEITEM: ${LI_ACTUAL:-?} (expected ${LI_EXPECTED})"
  echo "[hdbh] ORDERS  : ${ORD_ACTUAL:-?} (expected ${ORD_EXPECTED})"
  if [ "${LI_ACTUAL:-0}" = "$LI_EXPECTED" ] && [ "${ORD_ACTUAL:-0}" = "$ORD_EXPECTED" ]; then
    ROW_OK=1
  fi
fi

# --- Power test: 1 VU runs the 22 ad-hoc queries sequentially.
RUN_OK=0
if [ $ROW_OK = 1 ]; then
  echo "[hdbh] === power test (RUNVU=$RUNVU, 22 queries) ==="
  render "$HERE/run-tproch.tcl" > /tmp/hdbh_run.tcl
  docker cp /tmp/hdbh_run.tcl "$CLI":/opt/hammerdb/run.tcl
  docker exec "$CLI" sh -c 'cd /opt/hammerdb && ./hammerdbcli auto run.tcl' \
    > "$OUT/run.log" 2>&1
  grep -qaE 'TEST COMPLETE|Geometric Mean|completed in' "$OUT/run.log" && RUN_OK=1
fi

# Extract per-query times + geometric mean if present.
QTIMES=$(grep -aE 'Query .* completed in|Geometric' "$OUT/run.log" 2>/dev/null | head -25)

{
  echo "HammerDB 5.0 TPROC-H verification -- $TS"
  echo "DB image: tidesdb/mysql:9.7"
  echo "profile : SCALE=$SCALE BUILDVU=$BUILDVU RUNVU=$RUNVU caps=${CPUS}cpu/${MEM}"
  echo "tidesdb_unified_memtable = ${UNIFIED_MT:-?}"
  echo
  echo "schema build       : $([ $BUILD_OK = 1 ] && echo COMPLETED || echo FAILED)"
  echo "row-count gate     : $([ $ROW_OK = 1 ] && echo PASS || echo FAIL)"
  echo "  lineitem: ${LI_ACTUAL:-n/a} (expected ${LI_EXPECTED})"
  echo "  orders  : ${ORD_ACTUAL:-n/a} (expected ${ORD_EXPECTED})"
  echo "power test         : $([ $RUN_OK = 1 ] && echo COMPLETED || echo 'NOT RUN/FAILED')"
  echo
  if [ -n "$QTIMES" ]; then
    echo "Query timings (subset):"
    echo "$QTIMES"
    echo
  fi
  if [ $BUILD_OK = 1 ] && [ $ROW_OK = 1 ] && [ $RUN_OK = 1 ]; then
    echo "verdict: PASS"
  elif [ $BUILD_OK != 1 ]; then
    echo "verdict: FAIL -- schema build did not complete (see build.log)"
  elif [ $ROW_OK != 1 ]; then
    echo "verdict: FAIL -- row-count gate: expected lineitem=$LI_EXPECTED got ${LI_ACTUAL:-0}; this is silent bulk-load data loss"
  else
    echo "verdict: INCONCLUSIVE -- power test did not produce expected output (see run.log)"
  fi
} | tee "$OUT/SUMMARY.txt"

cp "$BENCH/images/versions.lock" "$OUT/" 2>/dev/null || true
echo "[hdbh] artifacts in $OUT"
