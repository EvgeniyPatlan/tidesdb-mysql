#!/usr/bin/env bash
# Bootstrap the build workspace.
#
# What it does:
#   1. Clones MySQL Server 9.7 into vendor/mysql-server (shallow, ~1 GB)
#   2. Clones TidesDB into vendor/tidesdb (shallow, ~10 MB)
#   3. (Optional) Clones TideSQL reference into vendor/tidesql (only if you
#      want to re-run scripts/replay-port-edits.sh; not needed for normal use)
#   4. Copies the plugin source into vendor/mysql-server/storage/tidesdb/
#   5. Copies the MTR test suite into vendor/mysql-server/mysql-test/suite/tidesdb/
#
# After this finishes, scripts/build-all.sh produces ha_tidesdb.so.
#
# Run from the repo root:
#   ./scripts/setup-workspace.sh

set -euo pipefail

REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
cd "$REPO"

# Pin versions for reproducibility. Bump when known-compatible.
MYSQL_TAG="${MYSQL_TAG:-mysql-9.7.0}"
TIDESDB_TAG="${TIDESDB_TAG:-v9.2.0}"
WITH_TIDESQL_REFERENCE="${WITH_TIDESQL_REFERENCE:-0}"

mkdir -p vendor

# ---------- 1) MySQL Server ----------
if [ ! -d vendor/mysql-server/.git ]; then
    echo "[setup] Cloning MySQL Server $MYSQL_TAG (shallow, ~10 min)"
    git clone --depth=1 --branch "$MYSQL_TAG" \
        https://github.com/mysql/mysql-server.git vendor/mysql-server
else
    echo "[setup] vendor/mysql-server already present — skipping clone"
fi

# ---------- 2) TidesDB ----------
if [ ! -d vendor/tidesdb/.git ]; then
    echo "[setup] Cloning TidesDB $TIDESDB_TAG"
    git clone --depth=1 --branch "$TIDESDB_TAG" \
        https://github.com/tidesdb/tidesdb.git vendor/tidesdb
else
    echo "[setup] vendor/tidesdb already present — skipping clone"
fi

# ---------- 3) TideSQL reference (optional) ----------
if [ "$WITH_TIDESQL_REFERENCE" = "1" ] && [ ! -d vendor/tidesql/.git ]; then
    echo "[setup] Cloning TideSQL reference (for replay-port-edits.sh)"
    git clone --depth=1 https://github.com/tidesdb/tidesql.git vendor/tidesql
fi

# ---------- 4) Drop plugin into MySQL tree ----------
PLUGIN_DST="vendor/mysql-server/storage/tidesdb"
echo "[setup] Installing plugin source -> $PLUGIN_DST"
mkdir -p "$PLUGIN_DST"
# Copy every file under plugin/ -- safer than naming each one, since new
# .cc/.h files (e.g. tidesdb_keyring_compat.cc for at-rest encryption)
# were silently missed by the explicit list and broke the build.
cp plugin/*.cc plugin/*.h plugin/CMakeLists.txt "$PLUGIN_DST/"

# ---------- 5) Drop MTR suite into MySQL tree ----------
SUITE_DST="vendor/mysql-server/mysql-test/suite/tidesdb"
echo "[setup] Installing MTR suite -> $SUITE_DST"
mkdir -p "$SUITE_DST"
cp -r mysql-test-suite/t          "$SUITE_DST/"
cp -r mysql-test-suite/r          "$SUITE_DST/"
cp -r mysql-test-suite/include    "$SUITE_DST/"
cp    mysql-test-suite/suite.opt  "$SUITE_DST/"

echo
echo "[setup] Done. Workspace ready at $REPO/vendor/."
echo "[setup] Next: ./scripts/build-all.sh        (Docker required)"
echo "[setup]       ./scripts/test-plugin.sh      (after build)"
