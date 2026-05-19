#!/usr/bin/env bash
# Build the pinned sysbench client image and record its version into
# versions.lock (load generator provenance must be auditable too).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
LOCK="$REPO/bench/images/versions.lock"
IMG="sut-sysbench:bench"

echo "[sysbench] building ${IMG}"
docker build -f "$REPO/bench/images/Dockerfile.sysbench" -t "$IMG" "$REPO/bench/images"

VER=$(docker run --rm --entrypoint sysbench "$IMG" --version 2>/dev/null | head -1)
echo "[sysbench] ${VER}"

if grep -qE '^sysbench_version:' "$LOCK"; then
    sed -i -E "s|^(sysbench_version:[[:space:]]*).*|\1${VER}|" "$LOCK"
else
    printf '\n# --- Load generator ---\nsysbench_version:       %s\nsysbench_image:         %s\n' \
        "$VER" "$IMG" >>"$LOCK"
fi
echo "[sysbench] recorded in versions.lock"
docker images "$IMG" --format '  {{.Repository}}:{{.Tag}}  {{.Size}}  {{.ID}}'
