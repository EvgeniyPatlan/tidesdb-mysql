#!/usr/bin/env bash
# Focused diagnostic for the suite's recovery-step finding (orders rows
# for districts 2-10 missing post-SIGKILL despite district.d_next_o_id
# counters being intact).
#
# Three hypotheses to disambiguate:
#   H1 (harness race): wait_db_socket returns on the init-phase mysqld
#       before TidesDB has finished its per-CF WAL replay on the durable
#       mysqld. Counts should settle to expected if we wait.
#   H2 (recovery never finishes): TidesDB recovery hangs or fails for
#       the orders CF specifically. The LOG won't show "Database
#       recovery completed successfully" for it.
#   H3 (real durability gap): recovery completes successfully but the
#       orders CF's WAL was prematurely deleted / its data was never
#       durably persisted. Counts stay missing forever.
#
# What we do:
#   1. Build a TPC-C schema (small, deterministic) + run a brief mix
#      that bumps d_next_o_id.
#   2. Sample district.d_next_o_id + per-district MAX(o_id) pre-kill.
#   3. docker kill -9.
#   4. Restart on the SAME volume.
#   5. Sample at T+0 (immediately after wait_db_socket), T+30, T+60.
#   6. Dump TidesDB's LOG (especially "Database recovery" messages and
#      per-CF "WAL recovery completed: N blocks, M entries").
#   7. Compare: do counts settle? Are all CFs in the LOG? Is any CF
#      missing recovery completion?
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)
WARE=${WARE:-10}; BUILDVU=${BUILDVU:-4}; RUNVU=${RUNVU:-4}
PRE_KILL_SECS=${PRE_KILL_SECS:-30}
CPUS=${CPUS:-6}; MEM=${MEM:-16g}
USER=bench; PASS='Bench_9xQ!z'
IMG=${IMG:-tidesdb/mysql:9.7}

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$BENCH/results/recovery-diag-$TS"; mkdir -p "$OUT"
NET="diag-net-$$"; DB="diag-db-$$"; CLI="diag-cli-$$"; VOL="diag-vol-$$"

cleanup(){ docker rm -f "$DB" "$CLI" >/dev/null 2>&1 || true
           docker network rm "$NET" >/dev/null 2>&1 || true
           docker volume rm "$VOL" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "[diag] WARE=$WARE BUILDVU=$BUILDVU pre_kill=${PRE_KILL_SECS}s"
docker network create "$NET" >/dev/null 2>&1 || true
docker volume create "$VOL" >/dev/null 2>&1 || true

start_db() {
  docker run -d --name "$DB" --network "$NET" --cpus "$CPUS" --memory "$MEM" \
    -v "$VOL":/var/lib/mysql -e MYSQL_ALLOW_EMPTY_PASSWORD=1 "$IMG" \
    --max-connections=512 --max-connect-errors=1000000 --connect-timeout=30 \
    --net-read-timeout=120 --net-write-timeout=120 >/dev/null
}
wait_socket() {
  local i=0
  until docker exec "$DB" mysql -uroot -N -B -e "SELECT 1" 2>/dev/null | grep -q '^1$'; do
    i=$((i+1)); [ "$i" -gt 120 ] && return 1; sleep 2
  done
}
wait_tcp_streak() {
  local i=0 streak=0
  until [ "$streak" -ge 3 ]; do
    if docker exec "$DB" mysql -h127.0.0.1 -P3306 --protocol=TCP \
         -u"$USER" -p"$PASS" -N -B -e "SELECT 1" 2>/dev/null | grep -q '^1$'; then
      streak=$((streak+1)); else streak=0; fi
    i=$((i+1)); [ "$i" -gt 120 ] && return 1; sleep 2
  done
}

dump_state() {                                      # $1=label, $2=outfile
  local label="$1" f="$2"
  {
    echo "=== $label  $(date -u +%FT%TZ) ==="
    docker exec -i "$DB" mysql -uroot -t 2>&1 <<'SQL'
USE tpcc;
-- per-district counters (district CF)
SELECT 'district' AS tbl, d_w_id w, d_id d, d_next_o_id next_oid FROM district WHERE d_w_id=1 ORDER BY d_id;
-- per-(w,d) orders rows visible (filtered, uses PK prefix)
SELECT 'orders_w1' AS tbl, o_w_id w, o_d_id d, COUNT(*) cnt, MIN(o_id) min_id, MAX(o_id) max_id
FROM orders WHERE o_w_id=1 GROUP BY o_w_id, o_d_id ORDER BY o_d_id;
-- per-warehouse summary (still PK-prefix filtered, just different key)
SELECT 'orders_by_w' AS tbl, o_w_id w, COUNT(*) cnt FROM orders GROUP BY o_w_id ORDER BY o_w_id;
-- unfiltered counts -- if these are >> filtered, the read path drops data on prefix scans
SELECT 'unfiltered' AS tbl,
       (SELECT COUNT(*) FROM orders)      AS orders_total,
       (SELECT COUNT(*) FROM order_line)  AS order_line_total,
       (SELECT COUNT(*) FROM customer)    AS customer_total,
       (SELECT COUNT(*) FROM stock)       AS stock_total;
SQL
  } >> "$f" 2>&1
}

# ---- STAGE 1: initial DB + build + brief workload ----------------------
echo "[diag] === STAGE 1: build + warmup ==="
start_db
wait_socket || { echo "[diag] DB never ready" >&2; exit 1; }
docker exec -i "$DB" mysql -uroot >/dev/null 2>&1 <<SQL
CREATE USER IF NOT EXISTS '$USER'@'%' IDENTIFIED BY '$PASS';
GRANT ALL PRIVILEGES ON *.* TO '$USER'@'%' WITH GRANT OPTION;
FLUSH PRIVILEGES;
SQL
wait_tcp_streak || { echo "[diag] TCP never durable" >&2; exit 1; }
docker run -d --name "$CLI" --network "$NET" hammerdb:5.0 >/dev/null
render(){ sed -e "s/@DBHOST@/$DB/g" -e "s/@USER@/$USER/g" -e "s/@PASS@/$PASS/g" \
              -e "s/@WARE@/$WARE/g" -e "s/@BUILDVU@/$BUILDVU/g" -e "s/@RUNVU@/$RUNVU/g" \
              -e "s/@RAMP@/0/g" -e "s/@DUR@/10/g" -e "s/@RUNTIMER@/700/g" \
              -e "s/@ENGINE@/tidesdb/g" "$1"; }
render "$HERE/build.tcl" > /tmp/diag_build.tcl
docker cp /tmp/diag_build.tcl "$CLI":/opt/hammerdb/build.tcl
docker exec "$CLI" sh -c 'cd /opt/hammerdb && ./hammerdbcli auto build.tcl' \
  > "$OUT/build.log" 2>&1
grep -qa 'SCHEMA BUILD COMPLETED' "$OUT/build.log" \
  || { echo "[diag] build failed" >&2; tail -40 "$OUT/build.log"; exit 1; }
echo "[diag] build OK"

render "$HERE/run.tcl" > /tmp/diag_run.tcl
docker cp /tmp/diag_run.tcl "$CLI":/opt/hammerdb/run.tcl
docker exec -d "$CLI" sh -c 'cd /opt/hammerdb && ./hammerdbcli auto run.tcl > /tmp/run.log 2>&1'

echo "[diag] running mix for ${PRE_KILL_SECS}s before kill"
sleep "$PRE_KILL_SECS"

# ---- STAGE 2: pre-kill snapshot ----------------------------------------
echo "[diag] === STAGE 2: pre-kill snapshot ==="
dump_state "PRE_KILL" "$OUT/snapshots.txt"
docker exec "$DB" sh -c 'cp /var/lib/mysql/tidesdb_data/LOG /tmp/LOG_pre' 2>/dev/null \
  && docker cp "$DB":/tmp/LOG_pre "$OUT/LOG.pre_kill" 2>/dev/null

# ---- STAGE 3: SIGKILL --------------------------------------------------
echo "[diag] === STAGE 3: SIGKILL ==="
# Capture pre-kill docker logs (stderr) -- this is where the engine's
# fprintf instrumentation ([sstinstr], [walinstr]) ends up.
docker logs "$DB" > "$OUT/docker.pre_kill.stdout" 2> "$OUT/docker.pre_kill.stderr"
docker kill -s KILL "$DB" >/dev/null && sleep 2 && docker rm -f "$DB" >/dev/null

# ---- STAGE 4: restart + sample at T+0, T+30, T+60 ----------------------
echo "[diag] === STAGE 4: restart + sample ==="
start_db
T0_START=$(date +%s)
wait_socket || { echo "[diag] DB did not recover" >&2; exit 1; }
T0_END=$(date +%s)
echo "[diag] wait_socket returned after $((T0_END - T0_START))s -- sampling T+0"
dump_state "T+0 (wait_socket just returned)" "$OUT/snapshots.txt"

echo "[diag] sleeping 30s ..."; sleep 30
dump_state "T+30s" "$OUT/snapshots.txt"

echo "[diag] sleeping another 30s ..."; sleep 30
dump_state "T+60s (final)" "$OUT/snapshots.txt"

# Capture post-restart docker logs (where [sstinstr] / [walinstr] live)
docker logs "$DB" > "$OUT/docker.post_restart.stdout" 2> "$OUT/docker.post_restart.stderr"

# ---- STAGE 5: harvest TidesDB recovery log -----------------------------
echo "[diag] === STAGE 5: TidesDB LOG (recovery-relevant lines) ==="
docker exec "$DB" sh -c 'cp /var/lib/mysql/tidesdb_data/LOG /tmp/LOG_post' 2>/dev/null \
  && docker cp "$DB":/tmp/LOG_post "$OUT/LOG.post_restart" 2>/dev/null
{
  echo "=== recovery messages from TidesDB LOG ==="
  grep -E 'recover|WAL recovery|CF .* WAL|Recovering CF|Recovering SSTables|Database recovery' \
    "$OUT/LOG.post_restart" 2>/dev/null
} > "$OUT/recovery_log.txt"

# ---- STAGE 6: per-CF on-disk state -------------------------------------
echo "[diag] === STAGE 6: per-CF on-disk state ==="
{
  echo "=== /var/lib/mysql/tidesdb_data/ contents ==="
  docker exec "$DB" find /var/lib/mysql/tidesdb_data -maxdepth 2 -type f -exec ls -la {} \;
  echo
  echo "=== sizes per CF dir (non-zero bytes per WAL) ==="
  for d in $(docker exec "$DB" find /var/lib/mysql/tidesdb_data -maxdepth 1 -type d -name 'tpcc__*' 2>/dev/null); do
    echo "--- $d ---"
    docker exec "$DB" sh -c "ls -la '$d' 2>/dev/null"
  done
} > "$OUT/disk_state.txt" 2>&1

echo "[diag] === artifacts: $OUT ==="
echo "  snapshots.txt    -- pre-kill + T+0/T+30/T+60 row counts"
echo "  recovery_log.txt -- TidesDB LOG filtered for recovery lines"
echo "  disk_state.txt   -- per-CF on-disk file listing"
echo "  LOG.post_restart -- full TidesDB LOG from the restarted process"
