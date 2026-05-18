#!/usr/bin/env bash
# Build SUT A: TidesDB on MySQL (this repo).
#
# The image is already published+verified (v0.2.1). We re-tag the exact
# pinned digest from versions.lock as sut-mysql:bench so the orchestrator
# uses a stable local name and the provenance is auditable. No rebuild --
# the published digest IS the artifact under test.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
LOCK="$REPO/bench/images/versions.lock"

val() { grep -E "^$1:" "$LOCK" | head -1 | sed -E "s/^$1:[[:space:]]*//; s/[[:space:]]*(#.*)?$//"; }

SRC_IMAGE=$(val sut_a_image)            # evgeniypatlan/test-images:mysql-9.7-tidesdb-v0.2.1
WANT_DIGEST=$(val sut_a_image_digest)   # sha256:822fe343...
IMAGE="sut-mysql:bench"

echo "[sut-a] source ${SRC_IMAGE}"
docker pull "$SRC_IMAGE" >/dev/null

GOT_DIGEST=$(docker inspect --format '{{index .RepoDigests 0}}' "$SRC_IMAGE" 2>/dev/null | sed -E 's/.*@//')
if [ -n "$WANT_DIGEST" ] && [ -n "$GOT_DIGEST" ] && [ "$WANT_DIGEST" != "$GOT_DIGEST" ]; then
    echo "[sut-a] FATAL: digest mismatch" >&2
    echo "  want ${WANT_DIGEST}" >&2
    echo "  got  ${GOT_DIGEST}" >&2
    exit 1
fi
echo "[sut-a] digest verified ${GOT_DIGEST:-<none>}"

docker tag "$SRC_IMAGE" "$IMAGE"
echo "[sut-a] done -> ${IMAGE}"
docker images "$IMAGE" --format '  {{.Repository}}:{{.Tag}}  {{.Size}}  {{.ID}}'
