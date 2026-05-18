#!/usr/bin/env bash
# Parity harness: run every logical case on both SUTs, classify, diff,
# emit matrix.md. Server-neutral cases live in cases/*.case.sql; the only
# dialect difference (per-table TidesDB options) is shimmed (shim/dialect.sh).
#
# Per (case, SUT) classification:
#   OK          ran, normalized output == case's @expect block
#   WRONG       ran, output != @expect
#   UNSUPPORTED error matched a not-supported / unknown-engine / syntax sig
#   ERROR       any other failure
# Parity verdict per case:
#   SAME        both OK and identical normalized output
#   DIFF        otherwise (detail in the matrix)
#
# Requires images sut-mysql:bench and sut-mariadb:bench (built by
# ../images/build-*-sut.sh). Raw output kept under results/<ts>/raw
# (gitignored); matrix.md is the committed artifact.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)
. "$HERE/shim/dialect.sh"

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$BENCH/results/$TS"
RAW="$OUT/raw"
mkdir -p "$RAW"
MATRIX="$OUT/matrix.md"

MYSQL_IMG="sut-mysql:bench"
MARIA_IMG="sut-mariadb:bench"
MYSQL_C="parity-mysql-$$"
MARIA_C="parity-mariadb-$$"

cleanup() { docker rm -f "$MYSQL_C" "$MARIA_C" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "[parity] starting SUTs"
docker rm -f "$MYSQL_C" "$MARIA_C" >/dev/null 2>&1 || true
docker run -d --name "$MYSQL_C" -e MYSQL_ALLOW_EMPTY_PASSWORD=1 "$MYSQL_IMG" >/dev/null
docker run -d --name "$MARIA_C" "$MARIA_IMG" >/dev/null

echo "[parity] waiting for mysql"
sut_wait_ready mysql "$MYSQL_C" 90 || { echo "mysql never came up" >&2; exit 1; }
echo "[parity] waiting for mariadb (first boot inits the datadir)"
sut_wait_ready mariadb "$MARIA_C" 150 || { echo "mariadb never came up" >&2; exit 1; }

read_meta() {                       # sets CASE_NAME CASE_AXIS CASE_UNORDERED
    local f="$1"
    CASE_NAME=$(grep -E '^-- @case:'  "$f" | head -1 | sed -E 's/^-- @case:[[:space:]]*//')
    CASE_AXIS=$(grep -E '^-- @axis:'  "$f" | head -1 | sed -E 's/^-- @axis:[[:space:]]*//')
    CASE_UNORDERED=0
    grep -qE '^-- @unordered' "$f" && CASE_UNORDERED=1
    [ -z "$CASE_NAME" ] && CASE_NAME="$(basename "$f" .case.sql)"
    [ -z "$CASE_AXIS" ] && CASE_AXIS="misc"
}
expect_block() {                    # @expect: ... (to EOF or @endexpect)
    awk '/^-- @expect:/{f=1;next} /^-- @endexpect/{f=0} f{sub(/^-- ?/,"");print}' "$1"
}
maybe_sort() { if [ "$CASE_UNORDERED" = 1 ]; then sort; else cat; fi; }

UNSUP_RE='not supported|Unknown storage engine|Unknown ALGORITHM|syntax to use near|Unknown system variable|does not support|doesn'\''t have this option|is not supported for this'

classify() {                        # <rawfile> <exitcode> -> echoes RAN|UNSUPPORTED|ERROR
    local raw="$1" ec="$2"
    if grep -qiE "$UNSUP_RE" "$raw"; then echo UNSUPPORTED; return; fi
    if [ "$ec" -ne 0 ] || grep -qiE '^ERROR [0-9]' "$raw"; then echo ERROR; return; fi
    echo RAN
}

run_case_on() {                     # <server> <container> <casefile> -> sets R_CLASS R_NORM
    local server="$1" cid="$2" f="$3"
    local rawf="$RAW/$(basename "$f" .case.sql).$server.out"
    local sql; sql=$(render_case "$server" "$f")
    set +e
    printf 'DROP DATABASE IF EXISTS parity;\nCREATE DATABASE parity;\nUSE parity;\n%s\n' "$sql" \
        | sut_run "$server" "$cid" >"$rawf" 2>&1
    local ec=$?
    set -e
    local cls; cls=$(classify "$rawf" "$ec")
    R_NORM=$(normalize <"$rawf" | maybe_sort)
    if [ "$cls" = RAN ]; then
        if [ "$R_NORM" = "$EXP_NORM" ]; then R_CLASS=OK; else R_CLASS=WRONG; fi
    else
        R_CLASS=$cls
    fi
}

{
    echo "# Parity matrix"
    echo
    echo "Generated: $TS"
    echo
    echo 'SUT A = MySQL 9.7 + tidesdb-mysql v0.2.1 `sut-mysql:bench`'
    echo 'SUT B = MariaDB 12.3.1 + upstream tidesql `sut-mariadb:bench`'
    echo "Shared core: TidesDB v9.2.0 (both)."
    echo
    echo "| Axis | Case | SUT A (MySQL) | SUT B (MariaDB) | Parity |"
    echo "|------|------|---------------|-----------------|--------|"
} >"$MATRIX"

shopt -s nullglob
TOTAL=0; SAME=0
for f in "$HERE"/cases/*.case.sql; do
    TOTAL=$((TOTAL+1))
    read_meta "$f"
    EXP_NORM=$(expect_block "$f" | normalize | maybe_sort)

    run_case_on mysql   "$MYSQL_C" "$f"; A_CLASS=$R_CLASS; A_NORM=$R_NORM
    run_case_on mariadb "$MARIA_C" "$f"; B_CLASS=$R_CLASS; B_NORM=$R_NORM

    if [ "$A_CLASS" = OK ] && [ "$B_CLASS" = OK ] && [ "$A_NORM" = "$B_NORM" ]; then
        verdict="SAME"; SAME=$((SAME+1))
    elif [ "$A_CLASS" = "$B_CLASS" ]; then
        verdict="DIFF (both $A_CLASS)"
    else
        verdict="DIFF (A=$A_CLASS B=$B_CLASS)"
    fi
    echo "| $CASE_AXIS | $CASE_NAME | $A_CLASS | $B_CLASS | $verdict |" >>"$MATRIX"
    printf '[parity] %-38s A=%-11s B=%-11s %s\n' "$CASE_NAME" "$A_CLASS" "$B_CLASS" "$verdict"
done

{
    echo
    echo "**$SAME / $TOTAL cases fully identical (SAME).**"
    echo
    echo "Raw client output per case+SUT: \`results/$TS/raw/\` (gitignored)."
} >>"$MATRIX"

cp "$BENCH/images/versions.lock" "$OUT/versions.lock"
echo "[parity] matrix -> $MATRIX"
echo "[parity] $SAME/$TOTAL SAME"
