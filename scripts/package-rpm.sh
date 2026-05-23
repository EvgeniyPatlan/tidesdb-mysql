#!/usr/bin/env bash
# Build an .rpm of the TidesDB storage-engine plugin.
#
# Usage:
#   ./scripts/package-rpm.sh [VERSION]
#
# Builds:
#   1. tidesdb-builder-rpm Docker image (Oracle Linux 9 + gcc-toolset-14
#      + rpm-build) -- cached after first run.
#   2. ha_tidesdb.so against MySQL 9.7.0 source inside that image.
#   3. tidesdb-mysql-plugin-<ver>-1.x86_64.rpm via rpmbuild.
#
# Output:  dist/tidesdb-mysql-plugin-<version>-1.x86_64.rpm
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
VERSION="${1:-0.1.0+$(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo nogit)+$(date +%Y%m%d)}"
# RPM versions must be plain (no '+' or '-'); collapse both into '.'
RPM_VERSION="${VERSION//+/.}"
RPM_VERSION="${RPM_VERSION//-/.}"

ARCH="${ARCH:-x86_64}"
DIST="$REPO/dist"
mkdir -p "$DIST"

# Allow an externally-supplied .so (e.g. extracted from tidesdb/mysql:9.7
# via `docker cp`) to skip the rebuild step inside docker.
PLUGIN_SO="${PLUGIN_SO:-}"

# 1) Build the OL9 builder image (cached after first run). Skip if the
#    user pre-supplied a .so AND just wants the rpmbuild step.
if [ -z "$PLUGIN_SO" ] && ! docker image inspect tidesdb-builder-rpm >/dev/null 2>&1; then
    echo "[package-rpm] building tidesdb-builder-rpm image (~5 min first time)"
    docker build -t tidesdb-builder-rpm -f "$REPO/docker/Dockerfile.builder-rpm" "$REPO/docker"
fi

# 2) Stage the rpmbuild SOURCES + SPEC tree on the host. The actual
#    build runs inside the docker image.
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/rpmbuild"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
PAYLOAD="$STAGE/rpmbuild/BUILD/payload"
mkdir -p "$PAYLOAD"

# Substitutions
sed -e "s|@VERSION@|$RPM_VERSION|g" \
    -e "s|@CHANGELOG_DATE@|$(date '+%a %b %d %Y')|g" \
    "$REPO/packaging/rpm/tidesdb-mysql-plugin.spec.in" \
    > "$STAGE/rpmbuild/SPECS/tidesdb-mysql-plugin.spec"

# 3a) If the caller pre-supplied a .so, skip the heavy compile and
#     stage the rpmbuild payload directly.
if [ -n "$PLUGIN_SO" ]; then
    [ -f "$PLUGIN_SO" ] || { echo "ERROR: PLUGIN_SO=$PLUGIN_SO not found"; exit 1; }
    echo "[package-rpm] using pre-built $PLUGIN_SO (skipping compile)"
    cp "$PLUGIN_SO"                                "$PAYLOAD/ha_tidesdb.so"
    cp "$REPO/README.md"                           "$PAYLOAD/"
    cp "$REPO/LICENSE"                             "$PAYLOAD/"
    cp "$REPO/CHANGELOG.md"                        "$PAYLOAD/"
    cp "$REPO/packaging/common/tidesdb.cnf.example" "$PAYLOAD/"

    # Just the rpmbuild step, in any image with rpm-build installed.
    if ! docker image inspect tidesdb-builder-rpm >/dev/null 2>&1; then
        docker build -t tidesdb-builder-rpm \
            -f "$REPO/docker/Dockerfile.builder-rpm" "$REPO/docker"
    fi
    docker run --rm --user "$(id -u):$(id -g)" \
        -v "$STAGE":/stage \
        tidesdb-builder-rpm \
        rpmbuild --define '_topdir /stage/rpmbuild' \
                 -bb /stage/rpmbuild/SPECS/tidesdb-mysql-plugin.spec
else

# 3b) Compile + package end-to-end.
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$REPO":/repo:ro \
    -v "$STAGE":/stage \
    -e VERSION="$RPM_VERSION" \
    tidesdb-builder-rpm \
    bash -e -c '
set -e
echo "[builder] cloning + compiling against MySQL 9.7.0 + TidesDB v9.2.5"
mkdir -p /tmp/build
cd /tmp/build
git clone --depth=1 --branch mysql-9.7.0 \
    https://github.com/mysql/mysql-server.git mysql-server >/dev/null
git clone --depth=1 --branch v9.2.5 \
    https://github.com/tidesdb/tidesdb.git tidesdb >/dev/null

# TidesDB static archive
cd tidesdb
# bloom_filter_new use-after-free fix (TidesDB PR #626); remove once upstream.
patch -p1 < /repo/docker/patches/0001-bloomfix.patch
cmake -S . -B build-static \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DTIDESDB_BUILD_TESTS=OFF \
      -DTIDESDB_BUILD_BENCHMARK=OFF >/dev/null
cmake --build build-static -j"$(nproc)" >/dev/null
cmake --install build-static --prefix /tmp/build/tidesdb-prefix >/dev/null

# Plugin source
cp -r /repo/plugin /tmp/build/mysql-server/storage/tidesdb

cd /tmp/build/mysql-server
TIDESDB_ROOT=/tmp/build/tidesdb-prefix \
cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DDOWNLOAD_BOOST=1 \
      -DWITH_BOOST=/tmp/build/boost \
      -DWITH_UNIT_TESTS=OFF \
      -DCMAKE_PREFIX_PATH=/tmp/build/tidesdb-prefix \
      -DPLUGIN_ROCKSDB=NO -DPLUGIN_NDB=NO -DPLUGIN_FEDERATED=NO \
      -DPLUGIN_ARCHIVE=NO -DPLUGIN_BLACKHOLE=NO -DPLUGIN_EXAMPLE=NO \
      -DPLUGIN_GROUP_REPLICATION=NO -DWITH_GROUP_REPLICATION=OFF \
      -DWITH_AUTHENTICATION_LDAP=OFF -DWITH_AUTHENTICATION_KERBEROS=OFF \
      -DWITH_AUTHENTICATION_FIDO=OFF -DWITH_AUTHENTICATION_WEBAUTHN=OFF \
      >/dev/null
TIDESDB_ROOT=/tmp/build/tidesdb-prefix \
cmake --build build --target tidesdb -j"$(nproc)" >/dev/null

# Stage the payload for rpmbuild
PAYLOAD=/stage/rpmbuild/BUILD/payload
cp /tmp/build/mysql-server/build/plugin_output_directory/ha_tidesdb.so "$PAYLOAD/"
cp /repo/README.md                                "$PAYLOAD/"
cp /repo/LICENSE                                  "$PAYLOAD/"
cp /repo/CHANGELOG.md                             "$PAYLOAD/"
cp /repo/packaging/common/tidesdb.cnf.example     "$PAYLOAD/"

echo "[builder] running rpmbuild"
rpmbuild --define "_topdir /stage/rpmbuild" \
         -bb /stage/rpmbuild/SPECS/tidesdb-mysql-plugin.spec
'
fi   # end of else from 3a/3b

# 4) Copy out the resulting .rpm
RPM=$(find "$STAGE/rpmbuild/RPMS" -name "*.rpm" | head -n1)
[ -n "$RPM" ] || { echo "ERROR: rpmbuild produced no .rpm"; exit 1; }
mv "$RPM" "$DIST/"
OUT="$DIST/$(basename "$RPM")"

echo
echo "[package-rpm] produced $OUT"
ls -lh "$OUT"
