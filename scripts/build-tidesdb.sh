#!/usr/bin/env bash
# Build TidesDB as a static archive (PIC) and install it to /work/tidesdb-prefix
# so the MySQL plugin's CMake (FIND_LIBRARY + FIND_PATH) can locate it via
# TIDESDB_ROOT / CMAKE_PREFIX_PATH.
#
# Run inside the tides-builder Docker container. The host repo is mounted at /work.
# Run as the host UID/GID (passed in via docker --user) so build outputs are owned
# by the invoking user, not root.
set -euo pipefail

REPO=${REPO:-/work}
SRC="$REPO/vendor/tidesdb"
BUILD="$SRC/build-static"
PREFIX="$REPO/vendor/tidesdb-prefix"

[ -d "$SRC" ] || { echo "ERROR: $SRC not found"; exit 1; }

echo "[build-tidesdb] configuring (static, PIC, no tests, system allocator)"
cmake -S "$SRC" -B "$BUILD" \
      -DBUILD_SHARED_LIBS=OFF \
      -DTIDESDB_BUILD_TESTS=OFF \
      -DTIDESDB_WITH_MIMALLOC=OFF \
      -DTIDESDB_WITH_TCMALLOC=OFF \
      -DTIDESDB_WITH_JEMALLOC=OFF \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON

echo "[build-tidesdb] compiling"
cmake --build "$BUILD" -j"$(nproc)"

echo "[build-tidesdb] installing to $PREFIX"
cmake --install "$BUILD"

echo
echo "[build-tidesdb] done"
echo "  archive: $(find "$PREFIX/lib" -name 'libtidesdb.a' -printf '%p (%s bytes)\n')"
echo "  headers: $(ls "$PREFIX/include/tidesdb/" 2>/dev/null | wc -l) files in $PREFIX/include/tidesdb/"
