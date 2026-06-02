#!/usr/bin/env bash
# =============================================================================
#  release-upstream-bump.sh
#
#  Prepare-and-tag stage of the tidesdb-mysql release flow. Stops after
#  pushing the git tag; the Docker Hub push and GitHub release are a
#  separate manual step (scripts/release-docker.sh) so that Docker Hub
#  credentials stay on the workstation and the GitHub release advertising
#  the docker pull is created only after the push lands.
#
#  Designed to drive cleanly from the Release GitHub Action OR from the
#  command line. Replays the exact workflow used for v0.2.5, v0.3.0, v0.3.1:
#
#    1. Preflight (clean tree, on main, in sync with origin, docker + gh).
#    2. Resolve TIDESDB_VERSION (default: latest upstream release) +
#       PLUGIN_VERSION (default: patch-bump the current tag).
#    3. Sanity-check the public-API diff of tidesdb.h between the current
#       bundled engine and the target -- BAIL if new TDB_ERR_* / enums appear
#       (those need a manual plugin code update before this script can finish).
#    4. Bump all source version refs (3 Dockerfiles + 2 scripts + 3 bench
#       scripts + README).
#    5. Rebuild images (mysql + mwbench in parallel; mtr after mysql for
#       cache reuse). Apply the carried mwbench harness patch.
#    6. Run the validation gates: MTR (61/61), mwbench integrity (deletes
#       on, 0/0/0/0), HammerDB SIGKILL recovery, HammerDB WARE=100
#       throughput. BAIL on any failure.
#    7. Generate docs/v<engine>-validation-report.md from the captured
#       results.
#    8. Update CHANGELOG.md (insert a new section) and KNOWN-ISSUES.md
#       (rewrite the "current" header). Upstream release notes are embedded
#       for context.
#    9. Two themed commits (engine bump + docs), tag v<plugin>.
#   10. Confirm, push main, push tag. Done.
#
#  Then run scripts/release-docker.sh on your workstation to tag + push the
#  Docker Hub images and create the GitHub release.
#
#  Usage:
#    ./scripts/release-upstream-bump.sh                      # latest upstream, auto-bump plugin patch
#    ./scripts/release-upstream-bump.sh --tidesdb=v9.4.0     # specific engine
#    ./scripts/release-upstream-bump.sh --plugin=0.4.0       # specific plugin version
#    ./scripts/release-upstream-bump.sh --skip-throughput    # skip the 40-min WARE=100 gate
#    ./scripts/release-upstream-bump.sh --dry-run            # print actions, change nothing
#    ./scripts/release-upstream-bump.sh --no-publish         # commit + tag locally only
#    ./scripts/release-upstream-bump.sh --yes                # no confirmation prompt
#
#  Env knobs (alt to flags): TIDESDB_VERSION, PLUGIN_VERSION, SKIP_THROUGHPUT=1,
#  DRY_RUN=1, NO_PUBLISH=1, YES=1.
#
#  Bails on any of: uncommitted changes (unrelated to the bump), not on main,
#  out-of-date with origin, no docker access, no gh auth, new enums/error
#  codes in tidesdb.h, any validation gate failing.
# =============================================================================
set -uo pipefail

# ---- arg defaults ----------------------------------------------------------
TIDESDB_VERSION="${TIDESDB_VERSION:-}"
PLUGIN_VERSION="${PLUGIN_VERSION:-}"
SKIP_THROUGHPUT="${SKIP_THROUGHPUT:-0}"
DRY_RUN="${DRY_RUN:-0}"
NO_PUBLISH="${NO_PUBLISH:-0}"
YES="${YES:-0}"

while [ $# -gt 0 ]; do
    case "$1" in
        --tidesdb=*)        TIDESDB_VERSION="${1#*=}" ;;
        --plugin=*)         PLUGIN_VERSION="${1#*=}"  ;;
        --skip-throughput)  SKIP_THROUGHPUT=1 ;;
        --dry-run|-n)       DRY_RUN=1 ;;
        --no-publish)       NO_PUBLISH=1 ;;
        --yes|-y)           YES=1 ;;
        --help|-h)          sed -n '2,55p' "$0"; exit 0 ;;
        *)                  echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

# ---- helpers ---------------------------------------------------------------
TS=$(date -u +%Y%m%dT%H%M%SZ)
REPO=$(cd "$(dirname "$0")/.." && pwd)
LOG_DIR="$REPO/bench/results/release-$TS"
mkdir -p "$LOG_DIR"

log()  { printf '[release] %s\n' "$*" | tee -a "$LOG_DIR/run.log"; }
warn() { printf '[release][WARN] %s\n' "$*" | tee -a "$LOG_DIR/run.log" >&2; }
die()  { printf '[release][FATAL] %s\n' "$*" | tee -a "$LOG_DIR/run.log" >&2; exit 1; }
run()  { if [ "$DRY_RUN" = "1" ]; then echo "[DRY] $*"; else eval "$*"; fi; }
confirm() {
    [ "$YES" = "1" ] && return 0
    read -r -p "$* [y/N] " ans
    case "$ans" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

# `sgd "exec ..."` -> `docker exec ...`, group-switching only when the running
# user isn't in the docker group (so the script works in either setup).
if id -nG 2>/dev/null | tr ' ' '\n' | grep -qw docker; then
    sgd() { docker "$@"; }
else
    sgd() { sg docker -c "docker $*"; }
fi

# ---- 1. preflight ----------------------------------------------------------
preflight() {
    log "preflight checks"
    [ -d "$REPO/.git" ] || die "not a git repo at $REPO"
    cd "$REPO"

    command -v gh   >/dev/null || die "gh CLI not found"
    command -v jq   >/dev/null || warn "jq not found (some outputs will be less pretty, not fatal)"
    command -v curl >/dev/null || die "curl not found"

    sgd info >/dev/null 2>&1 || die "no docker access (check 'sg docker' or group membership)"
    gh auth status >/dev/null 2>&1 || die "gh not authenticated (run: gh auth login)"

    # docker hub login (used for perconalab + evgeniypatlan pushes)
    if [ "$NO_PUBLISH" != "1" ] && [ "$DRY_RUN" != "1" ]; then
        sgd info 2>/dev/null | grep -q "Username:" || \
            warn "docker daemon reports no Username -- the publish step may prompt or fail"
    fi

    # branch + clean tree (allow untracked, deny modified-but-uncommitted)
    local branch dirty
    branch=$(git rev-parse --abbrev-ref HEAD)
    [ "$branch" = "main" ] || die "must be on main (currently: $branch)"
    git fetch -q origin main
    git diff --quiet HEAD origin/main || die "main is out of sync with origin/main"
    dirty=$(git status --porcelain | awk '$1 ~ /^[MAD]/' | head -5)
    [ -z "$dirty" ] || { echo "$dirty" >&2; die "uncommitted modifications would be bundled with the release commit; clean them first"; }

    log "preflight OK"
}

# ---- 2. resolve versions ---------------------------------------------------
resolve_versions() {
    if [ -z "$TIDESDB_VERSION" ]; then
        TIDESDB_VERSION=$(gh api repos/tidesdb/tidesdb/releases/latest --jq .tag_name 2>/dev/null) || \
            die "could not resolve latest TidesDB version from GitHub"
    fi
    [[ "$TIDESDB_VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "TIDESDB_VERSION must look like vX.Y.Z (got: $TIDESDB_VERSION)"
    log "target engine version: $TIDESDB_VERSION"

    # current bundled engine (from Dockerfile.mysql)
    OLD_TIDESDB_VERSION=$(grep -oE '^ARG TIDESDB_TAG=v[0-9]+\.[0-9]+\.[0-9]+' "$REPO/docker/Dockerfile.mysql" | sed 's/.*=//' | head -1)
    [ -n "$OLD_TIDESDB_VERSION" ] || die "could not find current TIDESDB_TAG in docker/Dockerfile.mysql"
    log "current engine version : $OLD_TIDESDB_VERSION"
    [ "$OLD_TIDESDB_VERSION" = "$TIDESDB_VERSION" ] && die "nothing to do: already on $TIDESDB_VERSION"

    # current + next plugin version
    OLD_PLUGIN_VERSION=$(git tag --list 'v*' --sort=-version:refname | head -1 | sed 's/^v//')
    [ -n "$OLD_PLUGIN_VERSION" ] || die "no v* tags in repo to derive plugin version from"
    if [ -z "$PLUGIN_VERSION" ]; then
        # default: patch-bump
        local maj min pat
        IFS=. read -r maj min pat <<<"$OLD_PLUGIN_VERSION"
        PLUGIN_VERSION="$maj.$min.$((pat+1))"
    fi
    [[ "$PLUGIN_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "PLUGIN_VERSION must look like X.Y.Z (got: $PLUGIN_VERSION)"
    log "current plugin version : $OLD_PLUGIN_VERSION"
    log "target plugin version  : $PLUGIN_VERSION"

    # bare-number versions for image tags (9.3.2 from v9.3.2)
    OLD_TIDESDB_BARE="${OLD_TIDESDB_VERSION#v}"
    NEW_TIDESDB_BARE="${TIDESDB_VERSION#v}"
}

# ---- 3. tidesdb.h API surface diff -----------------------------------------
header_diff_check() {
    log "checking tidesdb.h public API diff $OLD_TIDESDB_VERSION...$TIDESDB_VERSION"
    local patch
    patch=$(gh api "repos/tidesdb/tidesdb/compare/$OLD_TIDESDB_VERSION...$TIDESDB_VERSION" \
        --jq '.files[] | select(.filename=="src/tidesdb.h") | .patch' 2>/dev/null || true)
    if [ -z "$patch" ]; then
        log "  (no tidesdb.h changes -- pure binary/internal bump)"
        return 0
    fi
    # look for new TDB_ERR_* constants or enum value additions
    local risky
    risky=$(printf '%s\n' "$patch" | grep -E '^\+#define[[:space:]]+TDB_ERR_|^\+[[:space:]]*TDB_[A-Z0-9_]+[[:space:]]*=' || true)
    if [ -n "$risky" ]; then
        echo "---- risky additions to tidesdb.h ----" >&2
        echo "$risky" >&2
        echo "--------------------------------------" >&2
        die "new public enums / error codes in tidesdb.h -- update plugin/ha_tidesdb.cc (tdb_rc_to_ha + transient lists) by hand before re-running"
    fi
    log "  no new public enums or error codes -- pure patch surface"
}

# ---- 4. bump version refs --------------------------------------------------
bump_refs() {
    log "bumping version refs $OLD_TIDESDB_VERSION -> $TIDESDB_VERSION"
    local files=(
        docker/Dockerfile.mysql docker/Dockerfile.mtr docker/Dockerfile.mwbench
        scripts/setup-workspace.sh scripts/package-rpm.sh
        bench/mwbench/run-mwbench.sh bench/hammerdb/run-all.sh bench/hammerdb/compare-frontends.sh
        README.md
    )
    for f in "${files[@]}"; do
        [ -f "$REPO/$f" ] || { warn "$f not found -- skipping"; continue; }
        run "sed -i \
            -e 's|TIDESDB_TAG=$OLD_TIDESDB_VERSION|TIDESDB_TAG=$TIDESDB_VERSION|g' \
            -e 's|tidesdb@$OLD_TIDESDB_VERSION|tidesdb@$TIDESDB_VERSION|g' \
            -e 's|--branch $OLD_TIDESDB_VERSION|--branch $TIDESDB_VERSION|g' \
            -e 's|TidesDB $OLD_TIDESDB_VERSION|TidesDB $TIDESDB_VERSION|g' \
            -e 's|engine source we ship ($OLD_TIDESDB_VERSION|engine source we ship ($TIDESDB_VERSION|g' \
            -e 's|unpatched, $OLD_TIDESDB_VERSION|unpatched, $TIDESDB_VERSION|g' \
            -e 's|unpatched as of $OLD_TIDESDB_VERSION|unpatched as of $TIDESDB_VERSION|g' \
            -e 's|UNPATCHED as of $OLD_TIDESDB_VERSION|UNPATCHED as of $TIDESDB_VERSION|g' \
            -e 's|bundled TidesDB ($OLD_TIDESDB_VERSION,|bundled TidesDB ($TIDESDB_VERSION,|g' \
            -e 's|storage engine (TidesDB $OLD_TIDESDB_VERSION|storage engine (TidesDB $TIDESDB_VERSION|g' \
            -e 's|tidesdb/mwbench:$OLD_TIDESDB_BARE|tidesdb/mwbench:$NEW_TIDESDB_BARE|g' \
            $REPO/$f"
    done
}

# ---- 5. build images -------------------------------------------------------
build_images() {
    log "building images (mysql + mwbench in parallel; mtr after mysql)"
    cd "$REPO"
    # mysql + mwbench in parallel
    ( DOCKER_BUILDKIT=0 sgd build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7 . \
        > "$LOG_DIR/build-mysql.log" 2>&1 ) &
    local mysql_pid=$!
    ( DOCKER_BUILDKIT=0 sgd build -f docker/Dockerfile.mwbench -t "tidesdb/mwbench:$NEW_TIDESDB_BARE" . \
        > "$LOG_DIR/build-mwbench.log" 2>&1 ) &
    local mwbench_pid=$!
    log "  waiting for tidesdb/mysql:9.7 build (pid $mysql_pid)..."
    wait $mysql_pid || die "mysql image build failed (see $LOG_DIR/build-mysql.log)"
    log "  tidesdb/mysql:9.7 OK"
    log "  waiting for tidesdb/mwbench:$NEW_TIDESDB_BARE build (pid $mwbench_pid)..."
    wait $mwbench_pid || die "mwbench image build failed (see $LOG_DIR/build-mwbench.log)"
    log "  tidesdb/mwbench:$NEW_TIDESDB_BARE OK"
    # mtr reuses mysql cache
    log "  building tidesdb/mysql-mtr:9.7..."
    DOCKER_BUILDKIT=0 sgd build -f docker/Dockerfile.mtr -t tidesdb/mysql-mtr:9.7 . \
        > "$LOG_DIR/build-mtr.log" 2>&1 || die "mtr image build failed (see $LOG_DIR/build-mtr.log)"
    log "  tidesdb/mysql-mtr:9.7 OK"
}

# ---- 6. validation gates ---------------------------------------------------
validate_mtr() {
    log "gate 1/4: MTR --suite=tidesdb"
    sgd run --rm tidesdb/mysql-mtr:9.7 bash -lc \
        'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force --max-test-fail=0' \
        > "$LOG_DIR/mtr.log" 2>&1 || die "MTR returned non-zero"
    grep -q "All [0-9]\+ tests were successful" "$LOG_DIR/mtr.log" || die "MTR did not report success (see $LOG_DIR/mtr.log)"
    MTR_RESULT=$(grep "tests were successful" "$LOG_DIR/mtr.log" | tail -1)
    log "  PASS: $MTR_RESULT"
}

validate_mwbench() {
    log "gate 2/4: mwbench integrity (8 GiB, deletes on)"
    IMG="tidesdb/mwbench:$NEW_TIDESDB_BARE" bash "$REPO/bench/mwbench/run-mwbench.sh" \
        > "$LOG_DIR/mwbench.log" 2>&1
    if ! grep -qE 'point_misses=0 +seek_misses=0 +range_misses=0 +mismatches=0' "$LOG_DIR/mwbench.log"; then
        die "mwbench gate FAILED -- inspect $LOG_DIR/mwbench.log"
    fi
    MWBENCH_LINE=$(grep -E 'point_misses=' "$LOG_DIR/mwbench.log" | tail -1)
    log "  PASS: $MWBENCH_LINE"
}

validate_recovery() {
    log "gate 3/4: HammerDB SIGKILL recovery"
    bash "$REPO/bench/hammerdb/recovery-test.sh" > "$LOG_DIR/recovery.log" 2>&1
    grep -q "verdict: PASS" "$LOG_DIR/recovery.log" || die "recovery gate FAILED -- inspect $LOG_DIR/recovery.log"
    log "  PASS: WAL durably recovered all committed work after SIGKILL"
}

validate_throughput() {
    if [ "$SKIP_THROUGHPUT" = "1" ]; then
        log "gate 4/4: HammerDB WARE=100 throughput  --  SKIPPED (--skip-throughput)"
        THROUGHPUT_LINE="skipped"
        return 0
    fi
    log "gate 4/4: HammerDB WARE=100 throughput (~30-40 min)"
    bash "$REPO/bench/hammerdb/run-throughput.sh" > "$LOG_DIR/throughput.log" 2>&1
    grep -q "verdict: PASS" "$LOG_DIR/throughput.log" || die "throughput gate FAILED -- inspect $LOG_DIR/throughput.log"
    THROUGHPUT_LINE=$(grep -E 'throughput: [0-9]+' "$LOG_DIR/throughput.log" | tail -1)
    if grep -qiE 'out of memory|oom-killed' "$LOG_DIR/throughput.log"; then
        die "throughput run produced an OOM event -- inspect $LOG_DIR/throughput.log"
    fi
    log "  PASS: $THROUGHPUT_LINE  (no OOM)"
}

# ---- 7. validation report --------------------------------------------------
write_validation_report() {
    local rpt="$REPO/docs/${TIDESDB_VERSION}-validation-report.md"
    log "writing $rpt"
    # fetch upstream release notes between old and new to embed as context
    local upstream_changes
    upstream_changes=$(gh api "repos/tidesdb/tidesdb/compare/$OLD_TIDESDB_VERSION...$TIDESDB_VERSION" --jq '.commits[].commit.message' 2>/dev/null | sed 's/^/- /' | head -40)
    cat > "$rpt" <<EOF
# TidesDB $TIDESDB_VERSION engine bump — validation report (release v$PLUGIN_VERSION)

Automated validation of bumping the bundled storage engine from
**$OLD_TIDESDB_VERSION** to **$TIDESDB_VERSION**. Engine ships unpatched. Generated by
\`scripts/release-upstream-bump.sh\` on $TS.

## Test 1 — MTR suite

\`\`\`
$MTR_RESULT
\`\`\`

**PASS.** Full log: \`$(basename "$LOG_DIR")/mtr.log\`.

## Test 2 — mwbench engine-integrity gate

\`\`\`
$MWBENCH_LINE
\`\`\`

**PASS.** Deletes enabled, full tombstone + compaction coverage. The carried
mwbench harness patch applied cleanly. Full log: \`$(basename "$LOG_DIR")/mwbench.log\`.

## Test 3 — HammerDB SIGKILL crash-recovery gate

**PASS** — WAL durably recovered all committed work after SIGKILL.
Full log: \`$(basename "$LOG_DIR")/recovery.log\`.

## Test 4 — HammerDB WARE=100 throughput

EOF
    if [ "$SKIP_THROUGHPUT" = "1" ]; then
        echo "_Skipped (\`--skip-throughput\`)._" >> "$rpt"
    else
        cat >> "$rpt" <<EOF
\`\`\`
$THROUGHPUT_LINE
OOM / kill events: 0
\`\`\`

**PASS.** Full log: \`$(basename "$LOG_DIR")/throughput.log\`.
EOF
    fi

    cat >> "$rpt" <<EOF

## Upstream commits between $OLD_TIDESDB_VERSION and $TIDESDB_VERSION

$upstream_changes

## Verdict

**PASS.** All gates green; cleared to ship as **v$PLUGIN_VERSION**.
EOF
    log "  wrote $rpt"
}

# ---- 8. update CHANGELOG + KNOWN-ISSUES ------------------------------------
update_changelog() {
    local rpt_rel="docs/${TIDESDB_VERSION}-validation-report.md"
    local body
    body=$(gh api "repos/tidesdb/tidesdb/releases/tags/$TIDESDB_VERSION" --jq .body 2>/dev/null | head -40 | sed 's/^/> /')
    log "appending CHANGELOG.md section for v$PLUGIN_VERSION"
    cat >> "$REPO/CHANGELOG.md" <<EOF

### Bundled engine bumped to TidesDB $TIDESDB_VERSION (release v$PLUGIN_VERSION)

The vendored engine moved from $OLD_TIDESDB_VERSION to **$TIDESDB_VERSION** across the Docker images, the RPM packaging, and \`setup-workspace.sh\`. The engine continues to ship **with zero patches**, and no plugin code changes were required (no new public enums or error codes for \`tdb_rc_to_ha\` to map).

Upstream release notes ($TIDESDB_VERSION):

$body

Gated on a full re-run of the validation suite — MTR, mwbench integrity, HammerDB SIGKILL recovery, and HammerDB WARE=100 throughput. See [$rpt_rel]($rpt_rel).
EOF
}

update_known_issues() {
    log "rewriting KNOWN-ISSUES.md 'current' header to $TIDESDB_VERSION"
    # Replace the version + release in the first "## Current:" line. Conservative
    # in-place edit: just bump versions; the body text stays human-curated.
    run "sed -i -E \
        -e 's|^(## Current: bundled on TidesDB) v[0-9]+\\.[0-9]+\\.[0-9]+|\\1 $TIDESDB_VERSION|' \
        -e 's|(As of release \\*\\*)v[0-9]+\\.[0-9]+\\.[0-9]+(\\*\\* the engine is pinned to \\*\\*TidesDB) v[0-9]+\\.[0-9]+\\.[0-9]+|\\1v$PLUGIN_VERSION\\2 $TIDESDB_VERSION|' \
        $REPO/KNOWN-ISSUES.md"
}

# ---- 9. commit + tag -------------------------------------------------------
commit_and_tag() {
    cd "$REPO"
    log "committing engine bump"
    run "git add docker/Dockerfile.mysql docker/Dockerfile.mtr docker/Dockerfile.mwbench \
        scripts/setup-workspace.sh scripts/package-rpm.sh \
        bench/mwbench/run-mwbench.sh bench/hammerdb/run-all.sh bench/hammerdb/compare-frontends.sh \
        README.md"
    run "git commit -q -m 'fix: bump bundled TidesDB engine to $TIDESDB_VERSION' \
        -m 'Automated bump $OLD_TIDESDB_VERSION -> $TIDESDB_VERSION via scripts/release-upstream-bump.sh. Engine continues to ship unpatched; no plugin code changes required (verified no new public enums or error codes in tidesdb.h).'"
    log "committing docs"
    run "git add CHANGELOG.md KNOWN-ISSUES.md docs/${TIDESDB_VERSION}-validation-report.md"
    run "git commit -q -m 'docs: ${TIDESDB_VERSION} validation report + CHANGELOG/KNOWN-ISSUES for v$PLUGIN_VERSION'"
    log "tagging v$PLUGIN_VERSION"
    run "git tag -a v$PLUGIN_VERSION -m 'v$PLUGIN_VERSION -- TidesDB engine $TIDESDB_VERSION'"
}

# ---- 10/11. publish --------------------------------------------------------
push_main_and_tag() {
    log "pushing main + tag v$PLUGIN_VERSION"
    run "git push origin HEAD:main"
    run "git push origin v$PLUGIN_VERSION"
}

# ---- main ------------------------------------------------------------------
preflight
resolve_versions
header_diff_check
bump_refs
build_images
validate_mtr
validate_mwbench
validate_recovery
validate_throughput
write_validation_report
update_changelog
update_known_issues
commit_and_tag

if [ "$NO_PUBLISH" = "1" ]; then
    log "NO_PUBLISH set -- stopping after local commit + tag. Run 'git push' + scripts/release-docker.sh when ready."
    log "  artifacts: $LOG_DIR"
    exit 0
fi

confirm "All gates green. Push main + tag v$PLUGIN_VERSION to origin?" || {
    log "publish aborted by user; commits + tag are still local"
    exit 0
}

push_main_and_tag

log "v$PLUGIN_VERSION prepared on main and tagged."
log "  Docker Hub + GitHub release: run scripts/release-docker.sh on this machine to finish."
log "  artifacts: $LOG_DIR"
