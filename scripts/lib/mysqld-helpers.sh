#!/usr/bin/env bash
# Shared bootstrap/start/stop/sql helpers for the TidesDB plugin test scripts.
# Source this from inside the tides-builder container — paths assume /work mount.
#
# Required env (caller exports or relies on defaults):
#   REPO       - repo root (default /work)
#   DATA       - mysqld data dir (default /tmp/tidesdb-data)
#   PORT       - tcp port for mysqld (default 3307; --skip-networking by default)
#   PLUGIN_DIR - default $REPO/vendor/mysql-server/build/plugin_output_directory

set -uo pipefail

: "${REPO:=/work}"
: "${BUILD:=$REPO/vendor/mysql-server/build}"
: "${MYSQLD:=$BUILD/runtime_output_directory/mysqld}"
: "${MYSQL:=$BUILD/runtime_output_directory/mysql}"
: "${PLUGIN_DIR:=$BUILD/plugin_output_directory}"
: "${DATA:=$REPO/test-data}"
: "${PORT:=3307}"
: "${SOCKET:=$DATA/mysqld.sock}"
: "${PIDFILE:=$DATA/mysqld.pid}"
: "${ERRLOG:=$DATA/error.log}"

# ---------- preflight ----------
mh_preflight () {
    local missing=0
    [ -x "$MYSQLD" ]                        || { echo "ERROR: mysqld not built ($MYSQLD)"; missing=1; }
    [ -x "$MYSQL"  ]                        || { echo "ERROR: mysql client not built ($MYSQL)"; missing=1; }
    [ -f "$PLUGIN_DIR/ha_tidesdb.so" ]      || { echo "ERROR: ha_tidesdb.so missing ($PLUGIN_DIR/ha_tidesdb.so)"; missing=1; }
    [ "$missing" -eq 0 ] || return 2
    return 0
}

# ---------- lifecycle ----------
mh_kill_prior () {
    if [ -f "$PIDFILE" ]; then
        local old; old=$(cat "$PIDFILE" 2>/dev/null || true)
        if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
            kill "$old" 2>/dev/null
            sleep 2
            kill -9 "$old" 2>/dev/null || true
        fi
    fi
    [ -S "$SOCKET" ] && rm -f "$SOCKET"
}

mh_bootstrap () {
    rm -rf "$DATA"; mkdir -p "$DATA"
    # TidesDB stores its CFs in $DATA/.tidesdb (inside the datadir), so wiping
    # $DATA above already clears them. Also remove the pre-0.2.5 sibling
    # location ($REPO/tidesdb_data) in case an old checkout left data there;
    # stale CFs would otherwise trip false UNIQUE / PK conflicts on first INSERT.
    rm -rf "$REPO/tidesdb_data"
    # NB: --no-defaults must precede --initialize-insecure (it's a meta-option
    # consumed before the initialization options are parsed).
    "$MYSQLD" \
        --no-defaults \
        --initialize-insecure \
        --basedir="$BUILD" --datadir="$DATA" \
        --log-error="$ERRLOG.bootstrap" \
        || { echo "ERROR: bootstrap failed; tail of log:"; tail -40 "$ERRLOG.bootstrap"; return 3; }
    return 0
}

mh_start () {
    "$MYSQLD" \
        --no-defaults \
        --basedir="$BUILD" --datadir="$DATA" \
        --plugin-dir="$PLUGIN_DIR" \
        --plugin-load-add=tidesdb=ha_tidesdb.so \
        --port="$PORT" --socket="$SOCKET" --pid-file="$PIDFILE" \
        --log-error="$ERRLOG" \
        --skip-grant-tables --skip-networking &
    local pid=$!
    # wait up to 30s for socket
    local i
    for i in $(seq 1 30); do
        [ -S "$SOCKET" ] && return 0
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "ERROR: mysqld exited during startup; tail of $ERRLOG:"
            tail -40 "$ERRLOG" 2>/dev/null
            return 4
        fi
        sleep 1
    done
    echo "ERROR: mysqld didn't open socket $SOCKET in 30s; tail of $ERRLOG:"
    tail -40 "$ERRLOG" 2>/dev/null
    kill "$pid" 2>/dev/null || true
    return 4
}

mh_stop () {
    "$MYSQL" -S "$SOCKET" -uroot -e "SHUTDOWN;" 2>/dev/null
    sleep 1
    if [ -f "$PIDFILE" ]; then
        local pid; pid=$(cat "$PIDFILE" 2>/dev/null || true)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null; sleep 1; kill -9 "$pid" 2>/dev/null || true
        fi
    fi
    [ -S "$SOCKET" ] && rm -f "$SOCKET"
}

# ---------- sql ----------
# Run SQL from stdin against the running mysqld; capture output + exit code.
mh_run_sql () {
    "$MYSQL" -S "$SOCKET" -uroot --batch --table 2>&1
}

# Same, but reads file and prints exit code on its own line at the end.
mh_run_sql_file () {
    local f="$1"
    "$MYSQL" -S "$SOCKET" -uroot --batch --table 2>&1 < "$f"
    local rc=$?
    return $rc
}
