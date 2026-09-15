#!/usr/bin/env bash
# Bootstrap the build workspace.
#
# What it does:
#   1. Clones MySQL Server 9.7 into vendor/mysql-server (shallow, ~1 GB)
#   2. Clones TidesDB into vendor/tidesdb (shallow, ~10 MB) and applies
#      docker/patches/tidesdb/*.patch, the same set the Docker image build
#      applies
#   3. (Optional) Clones TideSQL reference into vendor/tidesql (only if you
#      want to re-run scripts/replay-port-edits.sh; not needed for normal use)
#   4. Copies the plugin source into vendor/mysql-server/storage/tidesdb/
#   5. Copies the MTR test suite into vendor/mysql-server/mysql-test/suite/tidesdb/
#
# After this finishes, scripts/build-all.sh produces ha_tidesdb.so.
#
# Run from the repo root:
#   ./scripts/setup-workspace.sh

set -euo pipefail

REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
cd "$REPO"

# Pin versions for reproducibility. Bump when known-compatible.
MYSQL_TAG="${MYSQL_TAG:-mysql-9.7.0}"
TIDESDB_TAG="${TIDESDB_TAG:-v10.0.1}"
# Where that tag is cloned to. Overridable so a second engine version can sit
# beside the pinned one without disturbing it; pair it with TIDESDB_PREFIX_DIR
# when running scripts/build-tidesdb.sh.
TIDESDB_SRC_DIR="${TIDESDB_SRC_DIR:-vendor/tidesdb}"
WITH_TIDESQL_REFERENCE="${WITH_TIDESQL_REFERENCE:-0}"

mkdir -p vendor

# Apply docker/patches/tidesdb/*.patch to an engine tree, the same set and the
# same order docker/Dockerfile.mysql applies. Keeping the two in step is what
# stops a local build from differing from the shipped image -- a difference
# that shows up as test failures with no visible cause. Patches already
# present are skipped rather than reapplied, so this is safe to re-run.
apply_engine_patches() {
    local tree="$1" p have
    # Patches are written against the pinned tag. A tree at some other version
    # is not a broken workspace -- TIDESDB_SRC_DIR exists precisely so a second
    # engine version can sit beside the pinned one -- so say what is being
    # skipped and why rather than failing on a patch that was never meant for
    # this source.
    have=$(git -C "$tree" describe --tags --exact-match 2>/dev/null \
           || git -C "$tree" describe --tags 2>/dev/null || echo "")
    if [ -n "$have" ] && [ "$have" != "$TIDESDB_TAG" ]; then
        echo "[setup] $tree is $have, not the pinned $TIDESDB_TAG -- skipping engine patches"
        return 0
    fi
    shopt -s nullglob
    local patches=("$REPO"/docker/patches/tidesdb/*.patch)
    shopt -u nullglob
    if [ ${#patches[@]} -eq 0 ]; then
        echo "[setup] no engine patches to apply"
        return 0
    fi
    for p in "${patches[@]}"; do
        if git -C "$tree" apply --check "$p" 2>/dev/null; then
            echo "[setup] applying $(basename "$p")"
            git -C "$tree" apply "$p"
        elif git -C "$tree" apply --reverse --check "$p" 2>/dev/null; then
            echo "[setup] already applied: $(basename "$p")"
        else
            echo "[setup] ERROR: $(basename "$p") neither applies nor is applied to $tree" >&2
            return 1
        fi
    done
}


# ---------- 1) MySQL Server ----------
if [ ! -d vendor/mysql-server/.git ]; then
    echo "[setup] Cloning MySQL Server $MYSQL_TAG (shallow, ~10 min)"
    git clone --depth=1 --branch "$MYSQL_TAG" \
        https://github.com/mysql/mysql-server.git vendor/mysql-server
else
    echo "[setup] vendor/mysql-server already present — skipping clone"
fi

# ---------- 2) TidesDB ----------
if [ ! -d "$TIDESDB_SRC_DIR/.git" ]; then
    echo "[setup] Cloning TidesDB $TIDESDB_TAG -> $TIDESDB_SRC_DIR"
    git clone --depth=1 --branch "$TIDESDB_TAG" \
        https://github.com/tidesdb/tidesdb.git "$TIDESDB_SRC_DIR"
    apply_engine_patches "$TIDESDB_SRC_DIR"
else
    echo "[setup] $TIDESDB_SRC_DIR already present — skipping clone"
    # A tree cloned before a patch was added, or reset since, is the case worth
    # catching: the build would be silently unpatched and the failures it
    # causes look nothing like a missing patch. Applying is idempotent because
    # an already-applied patch fails --check and is skipped.
    apply_engine_patches "$TIDESDB_SRC_DIR"
fi

# ---------- 3) TideSQL reference (optional) ----------
if [ "$WITH_TIDESQL_REFERENCE" = "1" ] && [ ! -d vendor/tidesql/.git ]; then
    echo "[setup] Cloning TideSQL reference (for replay-port-edits.sh)"
    git clone --depth=1 https://github.com/tidesdb/tidesql.git vendor/tidesql
fi

# ---------- 4) Drop plugin into MySQL tree ----------
PLUGIN_DST="vendor/mysql-server/storage/tidesdb"
echo "[setup] Installing plugin source -> $PLUGIN_DST"
mkdir -p "$PLUGIN_DST"
# Copy every file under plugin/ -- safer than naming each one, since new
# .cc/.h files (e.g. tidesdb_master_key.{cc,h} for at-rest encryption)
# were silently missed by the explicit list and broke the build.
cp plugin/*.cc plugin/*.h plugin/CMakeLists.txt "$PLUGIN_DST/"

# Also copy the atomic-DDL unit-test sources (pure helpers, no MySQL harness).
# Skipped silently by their own CMakeLists when gtest is not installed.
if [ -d plugin/tests ]; then
    mkdir -p "$PLUGIN_DST/tests"
    cp plugin/tests/*.cc plugin/tests/*.h plugin/tests/CMakeLists.txt "$PLUGIN_DST/tests/" 2>/dev/null || true
fi

# ---------- 5) Drop MTR suite into MySQL tree ----------
SUITE_DST="vendor/mysql-server/mysql-test/suite/tidesdb"
echo "[setup] Installing MTR suite -> $SUITE_DST"
mkdir -p "$SUITE_DST"
cp -r mysql-test-suite/t          "$SUITE_DST/"
cp -r mysql-test-suite/r          "$SUITE_DST/"
cp -r mysql-test-suite/include    "$SUITE_DST/"
cp -r mysql-test-suite/std_data   "$SUITE_DST/"
# Note: suite.opt now lives under mysql-test-suite/t/ (where MTR
# actually looks for it -- it reads <suite>/t/suite.opt, not the
# suite root); the cp -r t/ above already brings it along.

echo
echo "[setup] Done. Workspace ready at $REPO/vendor/."
echo "[setup] Next: ./scripts/build-all.sh        (Docker required)"
echo "[setup]       ./scripts/test-plugin.sh      (after build)"
