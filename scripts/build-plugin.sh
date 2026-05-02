#!/usr/bin/env bash
# Configure MySQL 9.7 and build only the ha_tidesdb.so plugin target.
# Requires: scripts/build-tidesdb.sh has already produced /work/tidesdb-prefix.
#
# Run inside the tides-builder Docker container. The host repo is mounted at /work.
set -euo pipefail

REPO=${REPO:-/work}
MYSQL_SRC="$REPO/vendor/mysql-server"
MYSQL_BUILD="$MYSQL_SRC/build"
TIDESDB_PREFIX="$REPO/vendor/tidesdb-prefix"
BOOST_DIR="$REPO/vendor/boost"

[ -d "$MYSQL_SRC" ]      || { echo "ERROR: $MYSQL_SRC not found"; exit 1; }
[ -d "$TIDESDB_PREFIX" ] || { echo "ERROR: $TIDESDB_PREFIX not found — run build-tidesdb.sh first"; exit 1; }
[ -f "$MYSQL_SRC/storage/tidesdb/ha_tidesdb.cc" ] || { echo "ERROR: storage/tidesdb/ha_tidesdb.cc missing — vendor TideSQL source first"; exit 1; }

mkdir -p "$BOOST_DIR"

# Engine skips — keep only what we need. Cuts MySQL build time dramatically.
ENGINE_SKIPS=(
    -DPLUGIN_ROCKSDB=NO
    -DPLUGIN_NDB=NO
    -DPLUGIN_FEDERATED=NO
    -DPLUGIN_ARCHIVE=NO
    -DPLUGIN_BLACKHOLE=NO
    -DPLUGIN_EXAMPLE=NO
)

# Configure: only first run is slow (~3 min). Subsequent runs are fast (cache).
if [ ! -f "$MYSQL_BUILD/CMakeCache.txt" ]; then
    echo "[build-plugin] running cmake configure (~3 min first time)"
    TIDESDB_ROOT="$TIDESDB_PREFIX" \
    cmake -S "$MYSQL_SRC" -B "$MYSQL_BUILD" \
          -DCMAKE_BUILD_TYPE=Debug \
          -DDOWNLOAD_BOOST=1 \
          -DWITH_BOOST="$BOOST_DIR" \
          -DWITH_UNIT_TESTS=OFF \
          -DCMAKE_PREFIX_PATH="$TIDESDB_PREFIX" \
          "${ENGINE_SKIPS[@]}"
else
    echo "[build-plugin] reusing existing CMake configure (delete $MYSQL_BUILD to force reconfigure)"
fi

echo "[build-plugin] building 'tidesdb' plugin target"
echo "    NB: first build pulls in MySQL convenience libs (~10–30 min)"
echo "        subsequent rebuilds of just the plugin: <60s"
echo

TIDESDB_ROOT="$TIDESDB_PREFIX" \
cmake --build "$MYSQL_BUILD" --target tidesdb -j"$(nproc)" 2>&1 | tee "$REPO/build-plugin.log"

echo
echo "[build-plugin] looking for plugin output"
find "$MYSQL_BUILD" -name 'ha_tidesdb*' -type f 2>/dev/null
