#!/usr/bin/env bash
# Smoke test: bring up mysqld, INSTALL PLUGIN, run a quick CRUD round-trip.
# This is the minimum bar for "does the plugin even load."
#
# Usage (inside tides-builder container):
#   docker run --rm --user "$(id -u):$(id -g)" -v /home/corvin/TIDES:/work \
#     tides-builder /work/scripts/smoke-test.sh
set -uo pipefail

LIB_DIR="$(cd "$(dirname "$0")" && pwd)/lib"
# shellcheck disable=SC1091
source "$LIB_DIR/mysqld-helpers.sh"

# ---------- preflight ----------
mh_preflight || exit $?
ls -la "$PLUGIN_DIR/ha_tidesdb.so"

# ---------- bring up ----------
mh_kill_prior
echo "[smoke] bootstrap"; mh_bootstrap || exit $?
echo "[smoke] starting mysqld"; mh_start || exit $?

# ---------- run smoke phases ----------
phase () {
    echo
    echo "=========================================================="
    echo "[smoke] $1"
    echo "----------------------------------------------------------"
    shift
    echo "$@" | mh_run_sql || echo "  (mysql client returned non-zero)"
}

phase "A — engine listing BEFORE INSTALL PLUGIN" \
    "SELECT engine, support FROM information_schema.engines WHERE engine='TIDESDB';"

phase "B — verify plugin auto-loaded by --plugin-load-add" \
    "SELECT engine, support FROM information_schema.engines WHERE engine='TIDESDB';"

phase "C — SHOW ENGINES" \
    "SELECT engine, support, transactions FROM information_schema.engines WHERE engine='TIDESDB';"

phase "D — CREATE TABLE … ENGINE=TIDESDB" \
    "CREATE DATABASE smoke;
     CREATE TABLE smoke.t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
     SHOW CREATE TABLE smoke.t;"

phase "E — INSERT + SELECT" \
    "INSERT INTO smoke.t VALUES (1,'a'),(2,'b'),(3,'c');
     SELECT * FROM smoke.t ORDER BY id;
     SELECT * FROM smoke.t WHERE id=2;"

phase "F — DROP TABLE / DATABASE" \
    "DROP TABLE smoke.t;
     DROP DATABASE smoke;"

# ---------- shutdown + log tail ----------
echo
echo "[smoke] shutting down"
mh_stop
echo
echo "[smoke] mysqld error log tail (last 40 lines):"
echo "----------------------------------------------"
tail -40 "$ERRLOG" 2>/dev/null || echo "(none)"
echo "----------------------------------------------"
echo "[smoke] done. Full error log: $ERRLOG"
