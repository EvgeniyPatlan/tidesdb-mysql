#!/usr/bin/env bash
# Render charts for a results dir: run-charts.sh <results/perf-TS>
# Mounts the dir into the pinned renderer; PNGs land next to results.csv.
set -euo pipefail
D="${1:?usage: run-charts.sh <results/perf-DIR>}"
D=$(cd "$D" && pwd)
[ -f "$D/results.csv" ] || { echo "no results.csv in $D" >&2; exit 1; }
docker run --rm -v "$D":/data sut-charts:bench /data/results.csv
echo "[charts] PNGs in $D"
ls -1 "$D"/*.png 2>/dev/null || true
