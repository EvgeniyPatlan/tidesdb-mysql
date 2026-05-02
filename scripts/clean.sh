#!/usr/bin/env bash
# Wipe ALL build outputs (handles root-owned files via a docker-run as root).
# Source files in mysql-server/storage/tidesdb/ are NOT touched.
#
# Usage:  ./scripts/clean.sh
#
# Designed to be safe to run after any state — interrupted builds, mixed
# ownership, partial installs.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

echo "[clean] removing build artifacts..."

# Run as root inside the container so we can rm root-owned files left over
# from any earlier `docker run` that forgot --user.
docker run --rm -v "$REPO":/work tides-builder bash -c '
    set -e
    rm -rf \
        /work/vendor/tidesdb/build-static \
        /work/vendor/tidesdb-prefix \
        /work/vendor/mysql-server/build \
        /work/build-plugin.log \
        /work/test-data
    # Boost dir is auto-populated by MySQL CMake; clearing forces re-download.
    # We do NOT clear it by default — it is multi-MB and rarely the problem.
    # Pass CLEAN_BOOST=1 to also nuke vendor/boost/.
    if [ "${CLEAN_BOOST:-0}" = "1" ]; then
        rm -rf /work/vendor/boost
        mkdir -p /work/vendor/boost
    fi
'

echo "[clean] done. Run ./scripts/build-all.sh (or build-tidesdb.sh + build-plugin.sh) to rebuild."
