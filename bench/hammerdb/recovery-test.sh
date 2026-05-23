#!/usr/bin/env bash
# Crash-recovery test for ha_tidesdb: durability via WAL.
#
# Pattern (the only meaningful crash-recovery shape for a storage
# engine): pre-load -> read pre-kill counter -> write more -> hard kill
# (docker kill -9) -> restart with the SAME datadir -> verify the
# engine recovers without manual intervention AND no committed row is
# lost. "Committed" here means a row whose acknowledged INSERT/COMMIT
# returned success before the kill -- TidesDB's WAL must replay it.
#
# Implementation:
#   1. HammerDB TPROC-C build with WARE=N inserts (build phase commits
#      ~3000 orders per district; "loader's record" is district.d_next_o_id).
#   2. Start a timed TPROC-C run that INSERTs new orders (NewOrder txn
#      keeps incrementing d_next_o_id).
#   3. ~PRE_KILL_SECS into the run, sample district.d_next_o_id for w1
#      as the pre-kill watermark (last counters the SQL layer was sure
#      about) -- in TPC-C NewOrder increments d_next_o_id under the
#      same txn that inserts the order, so a successful read after a
#      committed NewOrder means the order exists.
#   4. docker kill -9 (SIGKILL -- no graceful flush, only WAL can save us).
#   5. Restart the DB container against the SAME persistent volume.
#   6. Wait for mysqld to come back, query district.d_next_o_id and
#      orders rows again. Pass iff:
#        - mysqld recovers without manual repair / dump
#        - post-restart d_next_o_id >= pre-kill watermark (no rollback
#          of committed work)
#        - post-restart orders rows for each district == d_next_o_id - 1
#          (the d_next_o_id is the NEXT id, so committed orders = N-1)
#
# Env: WARE BUILDVU CPUS MEM PRE_KILL_SECS
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)

WARE=${WARE:-10}; BUILDVU=${BUILDVU:-4}
CPUS=${CPUS:-4}; MEM=${MEM:-8g}
PRE_KILL_SECS=${PRE_KILL_SECS:-60}
RUNVU=${RUNVU:-4}
USER=bench; PASS='Bench_9xQ!z'

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$BENCH/results/recovery-$TS"; mkdir -p "$OUT"
NET="recnet-$$"; DB="rec-db-$$"; CLI="rec-cli-$$"
VOL="rec-vol-$$"

cleanup(){ docker rm -f "$DB" "$CLI" >/dev/null 2>&1 || true
           docker network rm "$NET" >/dev/null 2>&1 || true
           docker volume rm "$VOL" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "[rec] profile: WARE=$WARE BUILDVU=$BUILDVU RUNVU=$RUNVU pre_kill=${PRE_KILL_SECS}s"
docker network create "$NET" >/dev/null 2>&1 || true
docker volume create "$VOL" >/dev/null 2>&1 || true

start_db() {
  docker run -d --name "$DB" --network "$NET" --cpus "$CPUS" --memory "$MEM" \
    -v "$VOL":/var/lib/mysql \
    -e MYSQL_ALLOW_EMPTY_PASSWORD=1 tidesdb/mysql:9.7 \
    --max-connections=512 --max-connect-errors=1000000 --connect-timeout=30 \
    --net-read-timeout=120 --net-write-timeout=120 >/dev/null
}
wait_db_socket() {
  local i=0
  until docker exec "$DB" mysql -uroot -N -B -e "SELECT 1" 2>/dev/null | grep -q '^1$'; do
    i=$((i+1)); [ "$i" -gt 120 ] && return 1; sleep 2
  done
}
wait_db_tcp() {
  local i=0 streak=0
  until [ "$streak" -ge 3 ]; do
    if docker exec "$DB" mysql -h127.0.0.1 -P3306 --protocol=TCP \
         -u"$USER" -p"$PASS" -N -B -e "SELECT 1" 2>/dev/null | grep -q '^1$'; then
      streak=$((streak+1)); else streak=0; fi
    i=$((i+1)); [ "$i" -gt 120 ] && return 1; sleep 2
  done
}

echo "[rec] === STAGE 1: initial DB on persistent volume ==="
start_db
wait_db_socket || { echo "[rec] DB never ready (initial)" >&2; exit 1; }
UMT=$(docker exec "$DB" mysql -uroot -N -B \
  -e "SHOW GLOBAL VARIABLES LIKE 'tidesdb_unified_memtable'" 2>/dev/null | awk '{print $2}')
echo "[rec] tidesdb_unified_memtable = ${UMT:-?}"

docker exec -i "$DB" mysql -uroot >/dev/null 2>&1 <<SQL
CREATE USER IF NOT EXISTS '$USER'@'%' IDENTIFIED BY '$PASS';
GRANT ALL PRIVILEGES ON *.* TO '$USER'@'%' WITH GRANT OPTION;
SET GLOBAL log_bin_trust_function_creators=1;
FLUSH PRIVILEGES;
SQL
wait_db_tcp || { echo "[rec] DB never ready (TCP)" >&2; exit 1; }

echo "[rec] === STAGE 2: TPROC-C schema build (WARE=$WARE) ==="
docker run -d --name "$CLI" --network "$NET" hammerdb:5.0 >/dev/null
render(){ sed -e "s/@DBHOST@/$DB/g" -e "s/@USER@/$USER/g" -e "s/@PASS@/$PASS/g" \
              -e "s/@WARE@/$WARE/g" -e "s/@BUILDVU@/$BUILDVU/g" -e "s/@RUNVU@/$RUNVU/g" \
              -e "s/@RAMP@/0/g" -e "s/@DUR@/10/g" -e "s/@RUNTIMER@/700/g" \
              -e "s/@ENGINE@/tidesdb/g" "$1"; }
render "$HERE/build.tcl" > /tmp/rec_build.tcl
docker cp /tmp/rec_build.tcl "$CLI":/opt/hammerdb/build.tcl
docker exec "$CLI" sh -c 'cd /opt/hammerdb && ./hammerdbcli auto build.tcl' \
  > "$OUT/build.log" 2>&1
BUILD_OK=1
grep -qa 'SCHEMA BUILD COMPLETED' "$OUT/build.log" || BUILD_OK=0
grep -qaE 'invoked from within|FINISHED FAILED|Failed to load mysqltcl' "$OUT/build.log" && BUILD_OK=0
[ $BUILD_OK = 1 ] && echo "[rec] schema build COMPLETED" || { echo "[rec] schema build FAILED" >&2; exit 1; }

echo "[rec] === STAGE 3: 10-min timed NewOrder workload (will kill mid-run) ==="
render "$HERE/run.tcl" > /tmp/rec_run.tcl
docker cp /tmp/rec_run.tcl "$CLI":/opt/hammerdb/run.tcl
docker exec -d "$CLI" sh -c 'cd /opt/hammerdb && ./hammerdbcli auto run.tcl > /tmp/run.log 2>&1'

echo "[rec] letting workload run for ${PRE_KILL_SECS}s before kill"
sleep "$PRE_KILL_SECS"

echo "[rec] === STAGE 4: sample PRE-KILL watermark (district.d_next_o_id for w1) ==="
docker exec "$DB" mysql -uroot -N -B -e \
  "SELECT d_w_id, d_id, d_next_o_id FROM tpcc.district WHERE d_w_id=1 ORDER BY d_id" \
  > "$OUT/pre_kill_district.txt" 2>/dev/null
cat "$OUT/pre_kill_district.txt" | sed 's/^/[rec]   pre-kill /' | head -10

echo "[rec] === STAGE 5: hard kill (SIGKILL) -- only WAL can save us ==="
docker kill -s KILL "$DB" >/dev/null 2>&1
sleep 2
docker rm -f "$DB" >/dev/null 2>&1 || true

echo "[rec] === STAGE 6: restart on SAME volume -- verify WAL recovery ==="
start_db
RECOVERED=1
wait_db_socket || RECOVERED=0
if [ $RECOVERED = 0 ]; then
  echo "[rec] !!! DB did not recover -- harvesting error log"
  docker logs "$DB" 2>&1 | tail -80 > "$OUT/post_kill_dblog.txt" || true
fi

echo "[rec] === STAGE 7: verify post-recovery state ==="
docker exec "$DB" mysql -uroot -N -B -e \
  "SELECT d_w_id, d_id, d_next_o_id FROM tpcc.district WHERE d_w_id=1 ORDER BY d_id" \
  > "$OUT/post_recovery_district.txt" 2>/dev/null
cat "$OUT/post_recovery_district.txt" | sed 's/^/[rec]   post /' | head -10

# Diff pre-kill vs post-recovery: post must be >= pre for each district.
# A LESS-THAN value means committed work was rolled back -> durability bug.
LOSS=0
LOSS_DETAIL=""
while IFS=$'\t' read -r w d next_pre; do
  next_post=$(awk -v w="$w" -v d="$d" '$1==w && $2==d{print $3}' "$OUT/post_recovery_district.txt")
  if [ -z "$next_post" ]; then
    LOSS=1; LOSS_DETAIL="$LOSS_DETAIL w=$w d=$d MISSING_POST_ROW;"
  elif [ "$next_post" -lt "$next_pre" ]; then
    LOSS=1; LOSS_DETAIL="$LOSS_DETAIL w=$w d=$d pre=$next_pre post=$next_post (rolled back);"
  fi
done < "$OUT/pre_kill_district.txt"

# Cross-check: orders per district should be >= d_next_o_id - 3000 - 1
# (3000 = build-phase orders inserted with explicit o_id 1..3000;
#  d_next_o_id - 1 = highest committed o_id; expect every committed o_id
#  to be present in the orders table).
docker exec "$DB" mysql -uroot -N -B -e \
  "SELECT o_w_id, o_d_id, MAX(o_id) FROM tpcc.orders WHERE o_w_id=1 GROUP BY o_w_id, o_d_id ORDER BY o_d_id" \
  > "$OUT/post_recovery_orders.txt" 2>/dev/null
ORDER_GAPS=""
while IFS=$'\t' read -r w d next_post; do
  max_oid=$(awk -v w="$w" -v d="$d" '$1==w && $2==d{print $3}' "$OUT/post_recovery_orders.txt")
  expected=$((next_post - 1))
  if [ -z "$max_oid" ] || [ "$max_oid" -lt "$expected" ]; then
    ORDER_GAPS="$ORDER_GAPS w=$w d=$d expected_max=$expected got_max=${max_oid:-NONE};"
  fi
done < "$OUT/post_recovery_district.txt"

{
  echo "Crash-recovery test -- $TS"
  echo "DB image: tidesdb/mysql:9.7  unified_memtable=${UMT:-?}"
  echo "profile : WARE=$WARE BUILDVU=$BUILDVU RUNVU=$RUNVU pre_kill_secs=${PRE_KILL_SECS}"
  echo
  echo "schema build       : $([ $BUILD_OK = 1 ] && echo COMPLETED || echo FAILED)"
  echo "post-kill recovery : $([ $RECOVERED = 1 ] && echo OK || echo 'FAILED (mysqld did not return)')"
  echo "committed-row loss : $([ $LOSS = 0 ] && echo NONE || echo "DETECTED -- $LOSS_DETAIL")"
  echo "order-row gaps     : $([ -z "$ORDER_GAPS" ] && echo NONE || echo "DETECTED -- $ORDER_GAPS")"
  echo
  if [ $BUILD_OK = 1 ] && [ $RECOVERED = 1 ] && [ $LOSS = 0 ] && [ -z "$ORDER_GAPS" ]; then
    echo "verdict: PASS -- WAL durably recovered all committed work after SIGKILL"
  elif [ $RECOVERED = 0 ]; then
    echo "verdict: FAIL -- mysqld did not recover (see post_kill_dblog.txt)"
  elif [ $LOSS != 0 ]; then
    echo "verdict: FAIL -- durability violation: committed d_next_o_id rolled back"
  elif [ -n "$ORDER_GAPS" ]; then
    echo "verdict: FAIL -- committed orders missing post-recovery (WAL replay incomplete)"
  else
    echo "verdict: INCONCLUSIVE"
  fi
} | tee "$OUT/SUMMARY.txt"

cp "$BENCH/images/versions.lock" "$OUT/" 2>/dev/null || true
echo "[rec] artifacts in $OUT"
