#!/usr/bin/env bash
# Run the TidesDB plugin test suite. For each tests/phase2/*.sql file, executes
# the SQL against a fresh mysqld and (if a sibling .expected exists) diffs the
# output against it. Reports per-test pass/fail.
#
# Usage:
#   docker run --rm --user "$(id -u):$(id -g)" -v /home/corvin/TIDES:/work \
#     tides-builder /work/scripts/test-plugin.sh [pattern]
#
# Optional pattern filters by test name (e.g. test-plugin.sh insert).
set -uo pipefail

LIB_DIR="$(cd "$(dirname "$0")" && pwd)/lib"
# shellcheck disable=SC1091
source "$LIB_DIR/mysqld-helpers.sh"

REPO=${REPO:-/work}
TESTS_DIR="$REPO/tests/phase2"
RESULTS_DIR="$REPO/tests/results-$(date +%Y%m%d-%H%M%S)"
PATTERN="${1:-}"

mkdir -p "$RESULTS_DIR"

# ---------- preflight ----------
mh_preflight || exit $?
[ -d "$TESTS_DIR" ] || { echo "ERROR: $TESTS_DIR missing — no tests to run"; exit 2; }

# ---------- bring up mysqld once and reuse for all tests ----------
echo "[test-plugin] cleaning prior state"
mh_kill_prior

echo "[test-plugin] bootstrapping fresh data dir at $DATA"
mh_bootstrap || exit $?

echo "[test-plugin] starting mysqld with plugin-dir=$PLUGIN_DIR"
mh_start || exit $?

# Plugin is auto-loaded by --plugin-load-add in mh_start; verify it's there.
echo "[test-plugin] verifying plugin is loaded"
if ! mh_run_sql <<<"SELECT engine FROM information_schema.engines WHERE engine='TIDESDB';" \
        | grep -qi tidesdb; then
    echo "[test-plugin] PLUGIN NOT LOADED — mysqld error log tail:"
    tail -30 "$ERRLOG" 2>/dev/null
    mh_stop
    exit 5
fi
echo "[test-plugin] plugin loaded"

# ---------- run each test ----------
total=0; passed=0; failed=0; skipped=0; missing_expected=0
fails=()

for test_file in "$TESTS_DIR"/*.sql; do
    [ -f "$test_file" ] || continue
    name=$(basename "$test_file" .sql)
    if [ -n "$PATTERN" ] && [[ "$name" != *"$PATTERN"* ]]; then
        continue
    fi
    total=$((total + 1))

    out_file="$RESULTS_DIR/${name}.out"
    expected_file="${test_file%.sql}.expected"

    # Run against a fresh database to keep tests independent.
    {
        echo "DROP DATABASE IF EXISTS test_phase2;"
        echo "CREATE DATABASE test_phase2;"
        echo "USE test_phase2;"
        cat "$test_file"
        echo "DROP DATABASE IF EXISTS test_phase2;"
    } | mh_run_sql > "$out_file" 2>&1
    rc=$?

    if [ ! -f "$expected_file" ]; then
        printf "  ?  %-30s   (no .expected — captured output to %s)\n" "$name" "$out_file"
        missing_expected=$((missing_expected + 1))
        skipped=$((skipped + 1))
        continue
    fi

    if diff -u "$expected_file" "$out_file" > "$out_file.diff" 2>&1; then
        printf "  ✓  %-30s\n" "$name"
        passed=$((passed + 1))
        rm -f "$out_file.diff"
    else
        printf "  ✗  %-30s   (rc=%s, diff in %s.diff)\n" "$name" "$rc" "${out_file}"
        failed=$((failed + 1))
        fails+=("$name")
    fi
done

# ---------- shut down ----------
echo
echo "[test-plugin] shutting down mysqld"
mh_stop

# ---------- summary ----------
echo
echo "================================================="
echo "[test-plugin] summary"
echo "  total:    $total"
echo "  passed:   $passed"
echo "  failed:   $failed"
echo "  no-expected: $missing_expected"
echo "  results dir: $RESULTS_DIR"
if [ "$failed" -gt 0 ]; then
    echo
    echo "  failed tests:"
    for t in "${fails[@]}"; do echo "    - $t"; done
fi
echo "================================================="

[ "$failed" -eq 0 ]
