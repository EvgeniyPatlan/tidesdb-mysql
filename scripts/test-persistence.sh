#!/usr/bin/env bash
# Cross-restart persistence test: insert data, stop mysqld, restart, verify.
# Uses --keep-data so the second run sees the first run's data.
#
# Usage (inside tides-builder):
#   docker run --rm --user "$(id -u):$(id -g)" -v /home/corvin/TIDES:/work \
#     tides-builder /work/scripts/test-persistence.sh
set -uo pipefail

LIB_DIR="$(cd "$(dirname "$0")" && pwd)/lib"
# shellcheck disable=SC1091
source "$LIB_DIR/mysqld-helpers.sh"

mh_preflight || exit $?

# ---------- phase 1: write ----------
echo "[persistence] phase 1 — bootstrap & write"
mh_kill_prior
mh_bootstrap || exit $?
mh_start || exit $?

mh_run_sql <<'SQL'
CREATE DATABASE persist_test;
USE persist_test;
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(64), ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP) ENGINE=TIDESDB;
INSERT INTO t1 (id, v) VALUES (1, 'first'), (2, 'second'), (3, 'third');
INSERT INTO t1 (id, v) VALUES (100, 'hundred');
SELECT id, v FROM t1 ORDER BY id;
SQL

echo
echo "[persistence] phase 1 — graceful shutdown"
mh_stop
sleep 1

# Verify on-disk artifacts.
echo
echo "[persistence] on-disk after shutdown:"
ls -la "$DATA/.tidesdb/" 2>/dev/null | head -10
echo

# ---------- phase 2: restart, read ----------
echo "[persistence] phase 2 — restart same data dir"
# DON'T re-bootstrap. Just start.
mh_start || exit $?

echo
echo "[persistence] phase 2 — verify rows survived"
RESULT=$(mh_run_sql <<'SQL'
SELECT id, v FROM persist_test.t1 ORDER BY id;
SELECT COUNT(*) AS cnt FROM persist_test.t1;
SQL
)
echo "$RESULT"

mh_stop

# ---------- pass/fail ----------
echo
if echo "$RESULT" | grep -q "first" && \
   echo "$RESULT" | grep -q "second" && \
   echo "$RESULT" | grep -q "third" && \
   echo "$RESULT" | grep -q "hundred" && \
   echo "$RESULT" | grep -qE "^\| *4 *\|"; then
    echo "[persistence] PASS — all 4 rows survived restart"
    exit 0
else
    echo "[persistence] FAIL — expected 4 rows after restart, got:"
    echo "$RESULT"
    exit 1
fi
