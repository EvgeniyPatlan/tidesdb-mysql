#!/usr/bin/env bash
# Cross-version upgrade check: what happens when this build is pointed at a
# data directory written by the previous engine line (TidesDB 9.x).
#
# The outcome that matters is not "does it work" -- it cannot, the on-disk
# format changed -- but *which* of the three failure shapes we get:
#
#   crash              unacceptable
#   silent empty start unacceptable, and the default without the check:
#                      TidesDB 10 does not recognise the 9.x layout, decides
#                      the directory is a new database, and starts cleanly
#                      with every table's data invisible
#   refusal            what this asserts, with a message naming the cause,
#                      saying the data is intact, and giving the way forward
#
# Usage (inside tides-builder):
#   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work \
#     tides-builder /work/scripts/test-cross-version.sh
set -uo pipefail

REPO=${REPO:-/work}
WORK=${WORK:-/tmp/tdb-crossver}
V9_PREFIX=${V9_PREFIX:-$REPO/vendor/tidesdb-prefix}
V10_PREFIX=${V10_PREFIX:-$REPO/vendor/tidesdb-prefix-v10}

fail() { echo "[cross-version] FAIL: $*"; exit 1; }
note() { echo "[cross-version] $*"; }

[ -f "$V9_PREFIX/lib/libtidesdb.a" ] || {
    echo "[cross-version] SKIP: no 9.x engine at $V9_PREFIX"
    echo "[cross-version]   build one with:"
    echo "[cross-version]   TIDESDB_TAG=v9.3.2 TIDESDB_SRC_DIR=vendor/tidesdb \\"
    echo "[cross-version]   TIDESDB_PREFIX_DIR=vendor/tidesdb-prefix ./scripts/build-tidesdb.sh"
    exit 0
}
[ -f "$V10_PREFIX/lib/libtidesdb.a" ] || fail "no 10.x engine at $V10_PREFIX"

rm -rf "$WORK"; mkdir -p "$WORK"

# ---------- 1) write a 9.x data directory ----------
cat > "$WORK/mk9.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "tidesdb/tidesdb.h"
int main(int argc, char **argv) {
    tidesdb_t *db = NULL;
    tidesdb_config_t cfg = tidesdb_default_config();
    cfg.db_path = argv[1];
    if (tidesdb_open(&cfg, &db) != TDB_SUCCESS) return 1;
    tidesdb_column_family_config_t c = tidesdb_default_column_family_config();
    tidesdb_create_column_family(db, "test__t1", &c);
    tidesdb_column_family_t *cf = tidesdb_get_column_family(db, "test__t1");
    if (!cf) return 1;
    tidesdb_txn_t *t = NULL;
    tidesdb_txn_begin(db, &t);
    for (int i = 0; i < 200; i++) {
        char k[32], v[64];
        snprintf(k, sizeof k, "key%05d", i);
        snprintf(v, sizeof v, "value-%05d", i);
        tidesdb_txn_put(t, cf, (const uint8_t *)k, strlen(k), (const uint8_t *)v, strlen(v), -1);
    }
    if (tidesdb_txn_commit(t) != TDB_SUCCESS) return 1;
    tidesdb_txn_free(t);
    tidesdb_close(db);
    return 0;
}
EOF
gcc -O0 -I"$V9_PREFIX/include" "$WORK/mk9.c" "$V9_PREFIX/lib/libtidesdb.a" \
    -o "$WORK/mk9" -lpthread -lm -lzstd -llz4 -lsnappy -lcurl >/dev/null 2>&1 \
    || fail "could not build the 9.x writer"
"$WORK/mk9" "$WORK/datadir" >/dev/null 2>&1 || fail "could not write a 9.x data directory"
note "wrote a 9.x data directory with one column family and 200 keys"

# ---------- 2) the detector must recognise it ----------
cat > "$WORK/detect.cc" <<'EOF'
#include <cstdio>
#include <string>
#include "tidesdb_datadir_version.h"
int main(int argc, char **argv) {
    std::string cf;
    const bool legacy = tdb_datadir_is_tidesdb9(argv[1], &cf);
    printf("%s %s\n", legacy ? "LEGACY" : "NOT-LEGACY", legacy ? cf.c_str() : "");
    return legacy ? 0 : 1;
}
EOF
g++ -std=c++17 -O0 -I"$REPO/plugin" "$WORK/detect.cc" -o "$WORK/detect" >/dev/null 2>&1 \
    || fail "could not build the detector harness"

out=$("$WORK/detect" "$WORK/datadir")
case "$out" in
    LEGACY*) note "detector on a 9.x directory: $out" ;;
    *)       fail "detector did not recognise a 9.x data directory (said: $out)" ;;
esac

# ---------- 3) and must NOT fire on a 10.x directory, or an empty one ----------
cat > "$WORK/mk10.c" <<'EOF'
#include <string.h>
#include "tidesdb/db.h"
int main(int argc, char **argv) {
    tidesdb_t *db = NULL;
    tidesdb_config_t cfg = tidesdb_default_config();
    cfg.db_path = argv[1];
    if (tidesdb_open(&cfg, &db) != TDB_SUCCESS) return 1;
    tidesdb_column_family_config_t c = tidesdb_default_column_family_config();
    tidesdb_create_column_family(db, "test__t1", &c);
    tidesdb_close(db);
    return 0;
}
EOF
gcc -O0 -I"$V10_PREFIX/include" "$WORK/mk10.c" "$V10_PREFIX/lib/libtidesdb.a" \
    -o "$WORK/mk10" -lpthread -lm -lzstd -llz4 -lsnappy >/dev/null 2>&1 \
    || fail "could not build the 10.x writer"
"$WORK/mk10" "$WORK/v10dir" >/dev/null 2>&1 || fail "could not write a 10.x data directory"

"$WORK/detect" "$WORK/v10dir" >/dev/null 2>&1 \
    && fail "detector fired on a 10.x data directory -- it would block a normal start"
note "detector on a 10.x directory: NOT-LEGACY, as required"

mkdir -p "$WORK/emptydir"
"$WORK/detect" "$WORK/emptydir" >/dev/null 2>&1 \
    && fail "detector fired on an empty directory -- it would block a first start"
"$WORK/detect" "$WORK/does-not-exist" >/dev/null 2>&1 \
    && fail "detector fired on a missing directory -- it would block a first start"
note "detector on an empty and a missing directory: NOT-LEGACY, as required"

# ---------- 4) the 9.x data must still be there afterwards ----------
# The claim in the refusal message is that nothing was modified. Opening the
# 9.x directory with the 10.x engine is what an operator does before reading
# the message, so check the data survives exactly that.
cat > "$WORK/open10.c" <<'EOF'
#include "tidesdb/db.h"
int main(int argc, char **argv) {
    tidesdb_t *db = NULL;
    tidesdb_config_t cfg = tidesdb_default_config();
    cfg.db_path = argv[1];
    if (tidesdb_open(&cfg, &db) != TDB_SUCCESS) return 1;
    tidesdb_close(db);
    return 0;
}
EOF
gcc -O0 -I"$V10_PREFIX/include" "$WORK/open10.c" "$V10_PREFIX/lib/libtidesdb.a" \
    -o "$WORK/open10" -lpthread -lm -lzstd -llz4 -lsnappy >/dev/null 2>&1 \
    || fail "could not build the 10.x opener"
"$WORK/open10" "$WORK/datadir" >/dev/null 2>&1

cat > "$WORK/read9.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "tidesdb/tidesdb.h"
int main(int argc, char **argv) {
    tidesdb_t *db = NULL;
    tidesdb_config_t cfg = tidesdb_default_config();
    cfg.db_path = argv[1];
    if (tidesdb_open(&cfg, &db) != TDB_SUCCESS) return 1;
    tidesdb_column_family_t *cf = tidesdb_get_column_family(db, "test__t1");
    if (!cf) return 2;
    tidesdb_txn_t *t = NULL;
    tidesdb_txn_begin(db, &t);
    uint8_t *val = NULL; size_t vl = 0;
    int rc = tidesdb_txn_get(t, cf, (const uint8_t *)"key00042", 8, &val, &vl);
    if (rc == TDB_SUCCESS) { printf("%.*s\n", (int)vl, (char *)val); tidesdb_free(val); }
    tidesdb_txn_free(t); tidesdb_close(db);
    return rc == TDB_SUCCESS ? 0 : 3;
}
EOF
gcc -O0 -I"$V9_PREFIX/include" "$WORK/read9.c" "$V9_PREFIX/lib/libtidesdb.a" \
    -o "$WORK/read9" -lpthread -lm -lzstd -llz4 -lsnappy -lcurl >/dev/null 2>&1 \
    || fail "could not build the 9.x reader"

got=$("$WORK/read9" "$WORK/datadir" 2>/dev/null)
[ "$got" = "value-00042" ] \
    || fail "9.x data did not survive a 10.x open (read back: '${got:-nothing}')"
note "9.x data still readable by 9.x after a 10.x open: the refusal message's claim holds"

# also still detected as legacy, so a second start is refused the same way
"$WORK/detect" "$WORK/datadir" >/dev/null 2>&1 \
    || fail "detector stopped recognising the directory after a 10.x open"
note "still detected after a 10.x open, so a restart is refused rather than silently accepted"

# ---------- 5) end to end: a real mysqld pointed at a 9.x directory ----------
# The unit-level checks above prove the detector. This proves what an operator
# actually gets: the engine refuses to initialise, and the tables that need it
# fail loudly instead of reading back empty.
if [ -x "${MYSQLD:-$REPO/vendor/mysql-server/build/runtime_output_directory/mysqld}" ]; then
    # shellcheck disable=SC1091
    source "$REPO/scripts/lib/mysqld-helpers.sh"
    if mh_preflight >/dev/null 2>&1; then
        mh_kill_prior
        rm -rf "$DATA"
        mh_bootstrap >/dev/null 2>&1 || fail "could not bootstrap a server"
        rm -rf "$DATA/.tidesdb"
        "$WORK/mk9" "$DATA/.tidesdb" >/dev/null 2>&1 || fail "could not plant a 9.x engine directory"

        mh_start >/dev/null 2>&1
        grep -q "holds a TidesDB 9 data directory" "$ERRLOG" \
            || { mh_stop >/dev/null 2>&1; fail "server did not report the legacy data directory"; }
        note "server reported the legacy directory and refused to open it"

        # The server's own statement is the authority here, and it survives
        # whatever the client does with quoting.
        if grep -q "registration as a STORAGE ENGINE failed" "$ERRLOG"; then
            note "the server did not register TIDESDB, so a TidesDB table fails loudly"
        else
            mh_stop >/dev/null 2>&1
            fail "TIDESDB registered anyway -- tables would read back empty"
        fi
        if mh_run_sql "SHOW ENGINES" 2>/dev/null | grep -qi "tidesdb"; then
            mh_stop >/dev/null 2>&1
            fail "TIDESDB still listed in SHOW ENGINES"
        fi
        note "SHOW ENGINES does not list TIDESDB"
        mh_stop >/dev/null 2>&1 || true
    else
        note "SKIP end-to-end phase: server not built"
    fi
else
    note "SKIP end-to-end phase: no mysqld"
fi

echo
echo "[cross-version] PASS"
