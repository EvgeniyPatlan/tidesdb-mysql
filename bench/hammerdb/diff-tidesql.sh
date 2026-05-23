#!/usr/bin/env bash
# Differential diagnosis: run the SAME HammerDB 5.0 TPROC-C schema build
# (WARE=20 BUILDVU=8 by default) against upstream tidesdb/tidesql
# (MariaDB + TidesDB v9.2.0 -- the IDENTICAL engine tag as our MySQL
# SUT) and dump the identical ground-truth: actual orders-per-district
# vs the loader's own district.d_next_o_id claim.
#
#   tidesql ALSO loses bulk rows  -> shared TidesDB-core bug
#   tidesql is clean              -> our MySQL handler glue triggers it
#
# Build-only: the catastrophic loss is in the bulk-load path, so no
# timed run is needed. Image sut-mariadb:bench must already be built
# (bench/images/build-mariadb-sut.sh). Bring-up mirrors the bench perf
# orchestrator's proven MariaDB path exactly.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)

WARE=${WARE:-20}; BUILDVU=${BUILDVU:-8}
CPUS=${CPUS:-6}; MEM=${MEM:-16g}
USER=bench; PASS='Bench_9xQ!z'           # strong: MariaDB simple_password_check
IMG=${IMG:-sut-mariadb:bench}
MSOCK=/tmp/mariadb.sock                   # upstream image's root socket

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$BENCH/results/tidesql-diff-$TS"; mkdir -p "$OUT"
NET="tdqnet-$$"; DB="tdq-db-$$"; CLI="tdq-cli-$$"

cleanup(){ docker rm -f "$DB" "$CLI" >/dev/null 2>&1 || true
           docker network rm "$NET" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "[tdq] profile: WARE=$WARE BUILDVU=$BUILDVU caps=${CPUS}cpu/${MEM} img=$IMG"
docker network create "$NET" >/dev/null 2>&1 || true

# Same benchmarking server config the perf orchestrator applies to both
# SUTs (fairness control): stock hardening must not throttle the loader.
SRVOPTS="--max-connections=512 --max-connect-errors=1000000 --connect-timeout=30 --net-read-timeout=120 --net-write-timeout=120"
echo "[tdq] starting tidesql (MariaDB+TidesDB v9.2.0)"
docker run -d --name "$DB" --network "$NET" --cpus "$CPUS" --memory "$MEM" \
  "$IMG" $SRVOPTS >/dev/null

# The upstream MariaDB image bootstraps on a transient init server then
# restarts to the durable one (same race as the official MySQL image).
# Provisioning on the init server is lost on restart -> later "Access
# denied for 'bench'". Gate on root being durably up: 5 CONSECUTIVE
# socket successes with a settle, BEFORE any provisioning.
echo "[tdq] waiting for DURABLE mariadbd (root socket, 5 consecutive)"
i=0; streak=0
until [ "$streak" -ge 5 ]; do
  if docker exec "$DB" mariadb --socket="$MSOCK" -uroot -N -B \
       -e "SELECT 1" 2>/dev/null | grep -q '^1$'; then
    streak=$((streak+1)); else streak=0; fi
  i=$((i+1)); [ "$i" -gt 150 ] && { echo "[tdq] DB never durably ready" >&2; exit 1; }
  sleep 2
done
echo "[tdq] root durable; extra settle then provision"
sleep 5

# tidesql engine sanity + (defensive) load. The bench image auto-loads
# ha_tidesdb, but assert TIDESDB is actually available -- a missing
# engine would make HammerDB silently fall back to InnoDB and produce a
# meaningless "no loss" result.
docker exec -i "$DB" mariadb --socket="$MSOCK" -uroot >/dev/null 2>&1 <<SQL
INSTALL SONAME 'ha_tidesdb';
SQL
ENG=$(docker exec "$DB" mariadb --socket="$MSOCK" -uroot -N -B \
  -e "SELECT SUPPORT FROM information_schema.ENGINES WHERE ENGINE='TIDESDB'" 2>/dev/null)
echo "[tdq] TIDESDB engine support = ${ENG:-MISSING}"
if [ -z "$ENG" ]; then
  echo "[tdq] FATAL: TIDESDB engine not available in $IMG -- aborting (would falsely test InnoDB)" >&2
  exit 1
fi

PROV=$(docker exec -i "$DB" mariadb --socket="$MSOCK" -uroot 2>&1 <<SQL
CREATE DATABASE IF NOT EXISTS tpcc;
CREATE USER IF NOT EXISTS '$USER'@'%' IDENTIFIED BY '$PASS';
GRANT ALL PRIVILEGES ON *.* TO '$USER'@'%' WITH GRANT OPTION;
SET GLOBAL log_bin_trust_function_creators=1;
FLUSH PRIVILEGES;
SQL
)
[ -n "$PROV" ] && echo "[tdq] provision output: $PROV"
# Confirm the bench account actually exists on the DURABLE server with a
# password set (catches password-policy rejection / lost-on-restart).
docker exec "$DB" mariadb --socket="$MSOCK" -uroot -N -B 2>/dev/null -e \
  "SELECT CONCAT(user,'@',host,' plugin=',plugin,' has_pw=',IF(authentication_string<>'' OR password<>'',1,0)) FROM mysql.user WHERE user='$USER'" \
  | sed 's/^/[tdq] account: /'

# HammerDB connects container->container over the docker network to
# $DB:3306. Probe that exact TCP+auth path from INSIDE the running DB
# container, targeting its own docker-network name (docker DNS resolves
# a container's own alias to its IP) -- NOT loopback (upstream MariaDB
# may not bind 127.0.0.1) and NOT `docker run $IMG mariadb` (that
# re-triggers the image's SERVER entrypoint). 3 consecutive successes.
echo "[tdq] waiting for durable TCP via docker net (bench user, 3 consecutive)"
i=0; streak=0
until [ "$streak" -ge 3 ]; do
  if docker exec "$DB" mariadb -h"$DB" -P3306 --protocol=TCP \
       -u"$USER" -p"$PASS" -N -B -e "SELECT 1" 2>/dev/null | grep -q '^1$'; then
    streak=$((streak+1)); else streak=0; fi
  i=$((i+1)); [ "$i" -gt 90 ] && {
     echo "[tdq] DB never ready (net TCP). last client error:" >&2
     docker exec "$DB" mariadb -h"$DB" -P3306 --protocol=TCP \
       -u"$USER" -p"$PASS" -e "SELECT 1" 2>&1 | tail -3 >&2
     exit 1; }
  sleep 2
done
echo "[tdq] DB ready on TCP"

echo "[tdq] starting HammerDB 5.0 client"
docker run -d --name "$CLI" --network "$NET" hammerdb:5.0 >/dev/null

render(){ sed -e "s/@DBHOST@/$DB/g" -e "s/@USER@/$USER/g" -e "s/@PASS@/$PASS/g" \
              -e "s/@WARE@/$WARE/g" -e "s/@BUILDVU@/$BUILDVU/g" \
              -e "s/@ENGINE@/tidesdb/g" "$1"; }

echo "[tdq] === schema build (ENGINE=tidesdb, $WARE warehouses, $BUILDVU VUs) ==="
render "$HERE/build.tcl" > /tmp/tdq_build.tcl
docker cp /tmp/tdq_build.tcl "$CLI":/opt/hammerdb/build.tcl
docker exec "$CLI" sh -c 'cd /opt/hammerdb && ./hammerdbcli auto build.tcl' \
  > "$OUT/build.log" 2>&1
BUILD_OK=1
grep -qa 'SCHEMA BUILD COMPLETED' "$OUT/build.log" || BUILD_OK=0
grep -qaE 'invoked from within|zipfs:/app/main.tcl|FINISHED FAILED|Failed to load mysqltcl|Error in Virtual User|can.t read' \
    "$OUT/build.log" && BUILD_OK=0
echo "[tdq] build $([ $BUILD_OK = 1 ] && echo COMPLETED || echo FAILED)"

# Identical ground-truth DIAG as the MySQL harness: loader's claim
# (district.d_next_o_id) vs actually-persisted orders per (w1,district).
echo "[tdq] === ground-truth diagnostics ===" | tee "$OUT/DIAG.txt"
docker exec -i "$DB" mariadb --socket="$MSOCK" -uroot -t 2>&1 <<'SQL' | tee -a "$OUT/DIAG.txt"
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
SELECT d_w_id, d_id, d_next_o_id AS loader_thinks_orders FROM district WHERE d_w_id=1 ORDER BY d_id;
SELECT o_w_id, o_d_id, COUNT(*) actual_orders, MIN(o_id) min_id, MAX(o_id) max_id
FROM orders WHERE o_w_id=1 GROUP BY o_w_id, o_d_id ORDER BY o_d_id;
SQL

{
  echo "tidesql differential -- $TS"
  echo "image   : $IMG (MariaDB + TidesDB v9.2.0 -- identical engine tag as SUT A)"
  echo "profile : WARE=$WARE BUILDVU=$BUILDVU caps=${CPUS}cpu/${MEM}"
  echo "schema build : $([ $BUILD_OK = 1 ] && echo COMPLETED || echo FAILED)"
  echo
  echo "Interpretation:"
  echo "  if actual_orders << d_next_o_id (like our MySQL SUT) -> shared TidesDB-core bulk-load data loss"
  echo "  if actual_orders ~= d_next_o_id                      -> bug is in OUR MySQL handler glue"
  echo
  echo "See DIAG.txt for the per-district actual-vs-claimed table."
} | tee "$OUT/SUMMARY.txt"
cp "$BENCH/images/versions.lock" "$OUT/" 2>/dev/null || true
echo "[tdq] artifacts in $OUT"
