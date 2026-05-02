#!/usr/bin/env bash
# Reproduce the txn-commit crash under gdb to capture the actual stack frame.
# Run inside tides-builder container with /work mounted.
set -uo pipefail

# Need apt install gdb (not in our minimal image — install on the fly).
which gdb >/dev/null || { apt-get update -qq && apt-get install -y -qq gdb; }

LIB_DIR="$(cd "$(dirname "$0")" && pwd)/lib"
# shellcheck disable=SC1091
source "$LIB_DIR/mysqld-helpers.sh"

mh_kill_prior
mh_bootstrap >/dev/null 2>&1
mh_start
sleep 1

# Run a SQL session that triggers the commit crash. Try the simplest one:
# CREATE TABLE + multi-row INSERT + TRUNCATE (matches tidesdb_crud).
"$MYSQL" -S "$SOCKET" -uroot 2>&1 <<'SQL' &
CREATE DATABASE IF NOT EXISTS gdb_test;
USE gdb_test;
CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c');
SELECT * FROM t;
TRUNCATE TABLE t;
DROP DATABASE gdb_test;
SQL

# Wait for client to either finish or trigger crash
sleep 5
kill %1 2>/dev/null || true

if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "[gdb-repro] mysqld still alive — attaching gdb to dump where it is"
    PID=$(cat "$PIDFILE")
    gdb -batch \
        -ex "set pagination off" \
        -ex "thread apply all bt 30" \
        -p "$PID" 2>&1 | head -100
    mh_stop
else
    echo "[gdb-repro] mysqld died — looking for core file"
    find / -name "core*" 2>/dev/null | head
    # Look for the error log's stack trace (richer with -g symbols now)
    echo
    echo "=== mysqld error log (last stack trace) ==="
    grep -B1 -A50 "got signal\|SIGSEGV\|SIGABRT" "$ERRLOG" 2>/dev/null | tail -60
fi
