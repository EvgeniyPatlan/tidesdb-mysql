#!/usr/bin/env bash
# Build a .deb of the TidesDB storage-engine plugin.
#
# Usage:
#   ./scripts/package-deb.sh [VERSION]
#
# If VERSION is omitted it defaults to 0.1.0+<git short sha>+today's date,
# which is fine for personal builds. For releases pass an explicit version.
#
# Prerequisites: Docker. Reuses the tides-builder image (built by
# scripts/build-all.sh) for the plugin compile and dpkg-deb assembly.
#
# Output:  dist/tidesdb-mysql-plugin_<version>_amd64.deb
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
VERSION="${1:-0.1.0+$(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo nogit)+$(date +%Y%m%d)}"
ARCH="${ARCH:-amd64}"
DIST="$REPO/dist"
mkdir -p "$DIST"

# Allow an externally-supplied .so (e.g. from a CI artifact) to skip
# the rebuild. Defaults to the path build-all.sh produces.
PLUGIN_SO="${PLUGIN_SO:-$REPO/vendor/mysql-server/build/plugin_output_directory/ha_tidesdb.so}"

# 1) Build the plugin .so on Ubuntu 24.04 via the existing tides-builder
#    (only if no externally-supplied .so was provided).
if [ ! -f "$PLUGIN_SO" ]; then
    if [ ! -d "$REPO/vendor/mysql-server" ] || [ ! -d "$REPO/vendor/tidesdb" ]; then
        echo "[package-deb] vendor/ not populated -- running setup-workspace.sh"
        "$REPO/scripts/setup-workspace.sh"
    fi
    if ! docker image inspect tides-builder >/dev/null 2>&1; then
        echo "[package-deb] tides-builder image missing -- running build-all.sh"
        "$REPO/scripts/build-all.sh"
    fi
    echo "[package-deb] $PLUGIN_SO missing -- running build-all.sh"
    "$REPO/scripts/build-all.sh"
fi
[ -f "$PLUGIN_SO" ] || { echo "ERROR: build did not produce $PLUGIN_SO"; exit 1; }

# tides-builder image is needed for dpkg-deb assembly even when the .so
# was supplied externally.
if ! docker image inspect tides-builder >/dev/null 2>&1; then
    echo "[package-deb] tides-builder image missing -- building"
    docker build -t tides-builder "$REPO/docker"
fi

# 2) Stage the .deb tree.
PKG_NAME="tidesdb-mysql-plugin"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
PKG_ROOT="$STAGE/${PKG_NAME}_${VERSION}_${ARCH}"
mkdir -p "$PKG_ROOT/DEBIAN"
mkdir -p "$PKG_ROOT/usr/lib/mysql/plugin"
mkdir -p "$PKG_ROOT/usr/share/doc/$PKG_NAME"

cp "$PLUGIN_SO"                              "$PKG_ROOT/usr/lib/mysql/plugin/ha_tidesdb.so"
cp "$REPO/README.md"                         "$PKG_ROOT/usr/share/doc/$PKG_NAME/README.md"
cp "$REPO/LICENSE"                           "$PKG_ROOT/usr/share/doc/$PKG_NAME/copyright"
cp "$REPO/LICENSE"                           "$PKG_ROOT/usr/share/doc/$PKG_NAME/LICENSE"
cp "$REPO/CHANGELOG.md"                      "$PKG_ROOT/usr/share/doc/$PKG_NAME/changelog"
gzip -9n "$PKG_ROOT/usr/share/doc/$PKG_NAME/changelog"
cp "$REPO/packaging/common/tidesdb.cnf.example" \
                                             "$PKG_ROOT/usr/share/doc/$PKG_NAME/tidesdb.cnf.example"

chmod 0755 "$PKG_ROOT/usr/lib/mysql/plugin/ha_tidesdb.so"
chmod 0644 "$PKG_ROOT/usr/share/doc/$PKG_NAME/"*

# control file with substitutions
sed -e "s|@VERSION@|$VERSION|g" -e "s|@ARCH@|$ARCH|g" \
    "$REPO/packaging/deb/control.in" \
    > "$PKG_ROOT/DEBIAN/control"

cp "$REPO/packaging/deb/postinst" "$PKG_ROOT/DEBIAN/postinst"
cp "$REPO/packaging/deb/prerm"    "$PKG_ROOT/DEBIAN/prerm"
chmod 0755 "$PKG_ROOT/DEBIAN/postinst" "$PKG_ROOT/DEBIAN/prerm"

# 3) Build the .deb inside tides-builder (matches the toolchain that
#    compiled the .so so the package's md5sums look consistent).
echo "[package-deb] running dpkg-deb -b"
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$STAGE":/stage \
    tides-builder \
    dpkg-deb --root-owner-group --build "/stage/${PKG_NAME}_${VERSION}_${ARCH}" \
                                         "/stage/${PKG_NAME}_${VERSION}_${ARCH}.deb"

OUT="$DIST/${PKG_NAME}_${VERSION}_${ARCH}.deb"
mv "$STAGE/${PKG_NAME}_${VERSION}_${ARCH}.deb" "$OUT"

echo
echo "[package-deb] produced $OUT"
ls -lh "$OUT"
