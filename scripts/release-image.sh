#!/usr/bin/env bash
# Rebuild the runnable tidesdb/mysql:9.7 image, smoke-test it, and
# optionally push it to Docker Hub under evgeniypatlan/test-images.
#
# Usage:
#   ./scripts/release-image.sh [VERSION] [--push]
#
#   VERSION  Optional; defaults to v0.1.0+<short-sha>+<date>. Becomes
#            part of the published tag.
#   --push   Actually push to Docker Hub. Without this flag the script
#            only builds and smoke-tests locally (safe to run on a
#            laptop without credentials).
#
# Published tags (when --push):
#   evgeniypatlan/test-images:mysql-9.7-tidesdb-<VERSION>
#   evgeniypatlan/test-images:mysql-9.7-tidesdb-latest
#
# Locally always-tagged (whether --push or not):
#   tidesdb/mysql:9.7
#   tidesdb/mysql:latest
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

# ---- Parse args -----------------------------------------------------
VERSION=""
DO_PUSH=0
for arg in "$@"; do
    case "$arg" in
        --push)   DO_PUSH=1 ;;
        --help|-h)
            sed -n '1,25p' "$0"; exit 0
            ;;
        -*)
            echo "Unknown flag: $arg" >&2; exit 2 ;;
        *)
            if [ -n "$VERSION" ]; then
                echo "Unexpected positional arg: $arg" >&2; exit 2
            fi
            VERSION="$arg"
            ;;
    esac
done

if [ -z "$VERSION" ]; then
    SHA=$(git rev-parse --short HEAD 2>/dev/null || echo nogit)
    VERSION="v0.1.0+${SHA}+$(date +%Y%m%d)"
fi

DOCKERHUB_REPO="${DOCKERHUB_REPO:-evgeniypatlan/test-images}"
LOCAL_TAG_VER="tidesdb/mysql:9.7"
LOCAL_TAG_LAT="tidesdb/mysql:latest"
PUBLIC_TAG_VER="${DOCKERHUB_REPO}:mysql-9.7-tidesdb-${VERSION}"
PUBLIC_TAG_LAT="${DOCKERHUB_REPO}:mysql-9.7-tidesdb-latest"

echo "[release] version=${VERSION}"
echo "[release] local tags : ${LOCAL_TAG_VER}, ${LOCAL_TAG_LAT}"
if [ "$DO_PUSH" -eq 1 ]; then
    echo "[release] push tags  : ${PUBLIC_TAG_VER}, ${PUBLIC_TAG_LAT}"
else
    echo "[release] push DISABLED (re-run with --push to publish)"
fi
echo

# ---- 1. Build ------------------------------------------------------
echo "[release] step 1/4 -- building image (Stage 1 ~25 min cold, much faster cached)"
docker build \
    -f docker/Dockerfile.mysql \
    -t "$LOCAL_TAG_VER" \
    -t "$LOCAL_TAG_LAT" \
    .

# ---- 2. Smoke test --------------------------------------------------
echo
echo "[release] step 2/4 -- smoke-testing the freshly built image"
"$REPO/docker/runtime/smoke-test.sh"

# ---- 3. Tag for Docker Hub -----------------------------------------
echo
echo "[release] step 3/4 -- tagging for ${DOCKERHUB_REPO}"
docker tag "$LOCAL_TAG_VER" "$PUBLIC_TAG_VER"
docker tag "$LOCAL_TAG_VER" "$PUBLIC_TAG_LAT"
docker images "$DOCKERHUB_REPO" --format 'table {{.Repository}}:{{.Tag}}\t{{.Size}}\t{{.CreatedAt}}' | head -20

# ---- 4. Push (or report) -------------------------------------------
echo
if [ "$DO_PUSH" -eq 1 ]; then
    echo "[release] step 4/4 -- pushing to Docker Hub"
    docker push "$PUBLIC_TAG_VER"
    docker push "$PUBLIC_TAG_LAT"
    echo
    echo "[release] published:"
    echo "   docker pull ${PUBLIC_TAG_VER}"
    echo "   docker pull ${PUBLIC_TAG_LAT}"
else
    echo "[release] step 4/4 -- skipped (no --push)"
    echo "[release] to publish, re-run with --push, e.g.:"
    echo "   $0 ${VERSION} --push"
fi
