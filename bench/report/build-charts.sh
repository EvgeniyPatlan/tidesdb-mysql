#!/usr/bin/env bash
# Build the pinned chart-renderer image; record versions into the lock.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
LOCK="$REPO/bench/images/versions.lock"
IMG="sut-charts:bench"

echo "[charts] building ${IMG}"
docker build -f "$REPO/bench/report/Dockerfile.charts" -t "$IMG" "$REPO/bench/report"

VER=$(docker run --rm --entrypoint python "$IMG" -c \
  'import matplotlib,pandas;print("matplotlib "+matplotlib.__version__+", pandas "+pandas.__version__)')
echo "[charts] ${VER}"
if grep -qE '^charts_versions:' "$LOCK"; then
    sed -i -E "s|^(charts_versions:[[:space:]]*).*|\1${VER}|" "$LOCK"
else
    printf '\n# --- Chart renderer ---\ncharts_versions:        %s\ncharts_image:           %s\n' \
        "$VER" "$IMG" >>"$LOCK"
fi
echo "[charts] recorded in versions.lock"
