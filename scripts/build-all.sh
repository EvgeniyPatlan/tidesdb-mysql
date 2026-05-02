#!/usr/bin/env bash
# One-command rebuild: docker image (if missing) -> tidesdb -> plugin.
#
# Usage:
#   ./scripts/build-all.sh           # incremental
#   ./scripts/build-all.sh --clean   # nuke build outputs first, then rebuild
#
# Reproduces the full pipeline from a cold checkout. Safe to interrupt and
# re-run.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        -h|--help)
            sed -n '1,15p' "$0"
            exit 0
            ;;
        *) echo "Unknown arg: $arg" >&2; exit 2 ;;
    esac
done

# 0. Workspace bootstrap — clones MySQL + TidesDB into vendor/, drops plugin
#    + test suite into the MySQL tree. Safe to re-run (idempotent).
if [ ! -d "$REPO/vendor/mysql-server" ] || [ ! -d "$REPO/vendor/tidesdb" ]; then
    echo "[build-all] vendor/ not yet populated — running setup-workspace.sh"
    "$REPO/scripts/setup-workspace.sh"
fi

# 1. Builder image — rebuild only if missing.
if ! docker image inspect tides-builder >/dev/null 2>&1; then
    echo "[build-all] tides-builder image missing — building (~5 min)"
    docker build -t tides-builder "$REPO/docker"
else
    echo "[build-all] tides-builder image cached"
fi

# 2. Optional clean.
if [ "$CLEAN" -eq 1 ]; then
    "$REPO/scripts/clean.sh"
fi

# 2. TidesDB static archive (~1 min).
echo "[build-all] step 1/2 — building libtidesdb.a"
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$REPO":/work \
    tides-builder /work/scripts/build-tidesdb.sh

# 3. MySQL plugin (~3 min config + 10–30 min first-time plugin compile).
echo "[build-all] step 2/2 — configuring MySQL and building ha_tidesdb.so"
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$REPO":/work \
    tides-builder /work/scripts/build-plugin.sh

# Surface the result.
echo
echo "[build-all] done"
ls -la "$REPO/vendor/mysql-server/build/plugin_output_directory/ha_tidesdb."* 2>/dev/null \
    || echo "[build-all] plugin .so not produced — check build-plugin.log"
