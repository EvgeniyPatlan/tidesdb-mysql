#!/usr/bin/env bash
# Build SUT B: upstream tidesdb/tidesql -> MariaDB + TidesDB image.
#
# Pins are read from bench/images/versions.lock (frozen, disclosed).
# We invoke the upstream docker/ubuntu/Dockerfile directly (NOT
# docker/setup.sh) because setup.sh auto-runs a fixed-name container and
# does not expose WITH_TESTS; we want only the image, with the MTR suite
# baked in for the parity track.
#
# One-time ~30+ min cold build (full MariaDB). Re-run is layer-cached.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
LOCK="$REPO/bench/images/versions.lock"

val() { grep -E "^$1:" "$LOCK" | head -1 | sed -E "s/^$1:[[:space:]]*//; s/[[:space:]]*(#.*)?$//"; }

TIDESDB_VERSION=$(val tidesdb_version)          # v9.2.0 -- identical to SUT A
MARIADB_TAG=$(val sut_b_mariadb_tag)            # mariadb-12.3.1
SRC="${TIDESQL_SRC:-/tmp/tidesql}"
IMAGE="sut-mariadb:bench"

echo "[sut-b] TidesDB=${TIDESDB_VERSION}  MariaDB=${MARIADB_TAG}  src=${SRC}"

if [ ! -d "$SRC/.git" ]; then
    echo "[sut-b] cloning tidesdb/tidesql -> $SRC"
    git clone --depth 1 https://github.com/tidesdb/tidesql.git "$SRC"
fi
TIDESQL_COMMIT=$(cd "$SRC" && git rev-parse --short HEAD)
echo "[sut-b] tidesql commit ${TIDESQL_COMMIT}"

# Record the upstream commit back into the lock (auditable provenance).
sed -i -E "s|^(sut_b_plugin_commit:[[:space:]]*).*|\1${TIDESQL_COMMIT}|" "$LOCK"

echo "[sut-b] building ${IMAGE} (cold ~30+ min; cached fast)"
docker build \
    -f "$SRC/docker/ubuntu/Dockerfile" \
    -t "$IMAGE" \
    --build-arg "MARIADB_VERSION=${MARIADB_TAG}" \
    --build-arg "TIDESDB_VERSION=${TIDESDB_VERSION}" \
    --build-arg "WITH_TESTS=1" \
    "$SRC"

echo "[sut-b] done -> ${IMAGE}"
docker images "$IMAGE" --format '  {{.Repository}}:{{.Tag}}  {{.Size}}  {{.ID}}'
