#!/usr/bin/env bash
# =============================================================================
#  release-docker.sh
#
#  Manual publish step for a tidesdb-mysql release. Runs on YOUR workstation,
#  not in CI -- this is where Docker Hub credentials live.
#
#  Pairs with scripts/release-upstream-bump.sh (and the Release GitHub Action
#  that drives it): the bump script does refs → build → validate → commit →
#  tag → push tag and stops. This script picks up from there:
#
#    1. Verify the local tidesdb/mysql:9.7 image exists (built by the bump
#       script during validation).
#    2. Verify the git tag v<plugin> exists locally (pushed by the bump
#       script / Action) and the validation report is in tree.
#    3. Tag and push three Docker Hub destinations:
#         perconalab/tidesdb-mysql:<plugin>
#         perconalab/tidesdb-mysql:latest
#         evgeniypatlan/test-images:mysql-9.7-tidesdb-v<plugin>
#    4. Create the GitHub release, referencing the validation report and the
#       upstream release notes; the release notes advertise the docker pull
#       so this step must run AFTER the push, not before.
#
#  Usage:
#    ./scripts/release-docker.sh                       # auto: latest tag + Dockerfile-pinned engine
#    ./scripts/release-docker.sh --plugin=0.3.2
#    ./scripts/release-docker.sh --tidesdb=v9.4.0 --plugin=0.4.0
#    ./scripts/release-docker.sh --dry-run             # show what would happen
#    ./scripts/release-docker.sh --yes                 # no confirm prompt
#    ./scripts/release-docker.sh --no-gh-release       # docker only (skip GitHub release)
#
#  Env equivalents: TIDESDB_VERSION, PLUGIN_VERSION, DRY_RUN=1, YES=1,
#  NO_GH_RELEASE=1.
#
#  Pre-reqs (run once): `docker login`, `gh auth login`.
# =============================================================================
set -uo pipefail

TIDESDB_VERSION="${TIDESDB_VERSION:-}"
PLUGIN_VERSION="${PLUGIN_VERSION:-}"
DRY_RUN="${DRY_RUN:-0}"
YES="${YES:-0}"
NO_GH_RELEASE="${NO_GH_RELEASE:-0}"

while [ $# -gt 0 ]; do
    case "$1" in
        --tidesdb=*)     TIDESDB_VERSION="${1#*=}" ;;
        --plugin=*)      PLUGIN_VERSION="${1#*=}"  ;;
        --dry-run|-n)    DRY_RUN=1 ;;
        --yes|-y)        YES=1 ;;
        --no-gh-release) NO_GH_RELEASE=1 ;;
        --help|-h)       sed -n '2,32p' "$0"; exit 0 ;;
        *)               echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

REPO=$(cd "$(dirname "$0")/.." && pwd)
log()  { printf '[release-docker] %s\n' "$*"; }
die()  { printf '[release-docker][FATAL] %s\n' "$*" >&2; exit 1; }
run()  { if [ "$DRY_RUN" = "1" ]; then echo "[DRY] $*"; else eval "$*"; fi; }
confirm() {
    [ "$YES" = "1" ] && return 0
    read -r -p "$* [y/N] " ans
    case "$ans" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

if id -nG 2>/dev/null | tr ' ' '\n' | grep -qw docker; then
    sgd() { docker "$@"; }
else
    sgd() { sg docker -c "docker $*"; }
fi

cd "$REPO"

# ---- resolve versions ------------------------------------------------------
if [ -z "$TIDESDB_VERSION" ]; then
    TIDESDB_VERSION=$(grep -oE '^ARG TIDESDB_TAG=v[0-9]+\.[0-9]+\.[0-9]+' docker/Dockerfile.mysql | head -1 | sed 's/.*=//')
fi
[ -n "$TIDESDB_VERSION" ] || die "could not resolve TIDESDB_VERSION from docker/Dockerfile.mysql"

if [ -z "$PLUGIN_VERSION" ]; then
    PLUGIN_VERSION=$(git tag --list 'v*' --sort=-version:refname | head -1 | sed 's/^v//')
fi
[ -n "$PLUGIN_VERSION" ] || die "no v* tag found; create one (or pass --plugin=) first"
[[ "$PLUGIN_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "PLUGIN_VERSION must be X.Y.Z (got: $PLUGIN_VERSION)"

log "engine: $TIDESDB_VERSION   plugin: v$PLUGIN_VERSION"

# ---- preflight -------------------------------------------------------------
command -v gh     >/dev/null || die "gh CLI not found"
command -v docker >/dev/null || die "docker not found"
sgd info >/dev/null 2>&1 || die "no docker daemon access"

# the local image must exist (the bump script built it during validation)
sgd image inspect tidesdb/mysql:9.7 >/dev/null 2>&1 || \
    die "local image tidesdb/mysql:9.7 not found -- run scripts/release-upstream-bump.sh first to build + validate, then come back"

# the git tag must exist locally
git rev-parse --verify --quiet "refs/tags/v$PLUGIN_VERSION" >/dev/null || \
    die "git tag v$PLUGIN_VERSION not found locally -- 'git fetch --tags' or run the bump script first"

# the validation report (used for the GH release body) must exist
RPT="docs/$TIDESDB_VERSION-validation-report.md"
[ "$NO_GH_RELEASE" = "1" ] || [ -f "$REPO/$RPT" ] || \
    die "validation report $RPT not found -- run scripts/release-upstream-bump.sh first, or pass --no-gh-release to skip"

# docker hub login check (best-effort; warn but don't fail)
if ! sgd info 2>/dev/null | grep -q "Username:"; then
    log "WARN: 'docker info' reports no Username; the push may prompt or fail. Run 'docker login' first."
fi

# ---- confirm ---------------------------------------------------------------
confirm "Tag + push Docker Hub images for v$PLUGIN_VERSION$( [ "$NO_GH_RELEASE" = "0" ] && echo " and create the GitHub release" )?" \
    || { log "aborted"; exit 0; }

# ---- docker tag + push -----------------------------------------------------
log "tagging Docker Hub images"
run "sgd tag tidesdb/mysql:9.7 perconalab/tidesdb-mysql:$PLUGIN_VERSION"
run "sgd tag tidesdb/mysql:9.7 perconalab/tidesdb-mysql:latest"
run "sgd tag tidesdb/mysql:9.7 evgeniypatlan/test-images:mysql-9.7-tidesdb-v$PLUGIN_VERSION"

for ref in \
    "perconalab/tidesdb-mysql:$PLUGIN_VERSION" \
    "perconalab/tidesdb-mysql:latest" \
    "evgeniypatlan/test-images:mysql-9.7-tidesdb-v$PLUGIN_VERSION"
do
    log "pushing $ref"
    run "sgd push '$ref'"
done

# ---- GitHub release --------------------------------------------------------
if [ "$NO_GH_RELEASE" = "1" ]; then
    log "skipping GitHub release (--no-gh-release)"
    log "done."
    exit 0
fi

log "creating GitHub release v$PLUGIN_VERSION"
NOTES=$(mktemp /tmp/release-notes-XXXXXX.md)
trap "rm -f '$NOTES'" EXIT

UPSTREAM_BODY=$(gh api "repos/tidesdb/tidesdb/releases/tags/$TIDESDB_VERSION" --jq .body 2>/dev/null || true)

cat > "$NOTES" <<EOF
## v$PLUGIN_VERSION — TidesDB engine $TIDESDB_VERSION

Engine bump to **TidesDB $TIDESDB_VERSION**, shipped unpatched. No plugin code changes (verified header diff: no new public enums or error codes).

### Upstream release notes ($TIDESDB_VERSION)

$UPSTREAM_BODY

### Validation

Full validation suite green — see [$RPT](https://github.com/EvgeniyPatlan/tidesdb-mysql/blob/main/$RPT):
MTR · mwbench integrity (deletes on) · HammerDB SIGKILL crash-recovery · HammerDB WARE=100 throughput.

### Docker

\`\`\`
docker pull perconalab/tidesdb-mysql:$PLUGIN_VERSION
\`\`\`
EOF

run "gh release create v$PLUGIN_VERSION \
    --title 'v$PLUGIN_VERSION -- TidesDB engine $TIDESDB_VERSION' \
    --notes-file '$NOTES' --verify-tag"

log "v$PLUGIN_VERSION published."
log "  https://github.com/EvgeniyPatlan/tidesdb-mysql/releases/tag/v$PLUGIN_VERSION"
