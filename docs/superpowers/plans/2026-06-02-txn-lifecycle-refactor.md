# Transaction-Lifecycle Refactor Implementation Plan

> **⚠️ SUPERSEDED — 2026-06-02.** Do not execute. All six target findings
> (CF-1, C-2, H-1, H-3, H-8, MF-4) verified closed in current code before
> execution began. The first dispatched implementer subagent flagged the
> mismatch on Task 0 and the work was scrapped. See
> `docs/code-review-followup-report.md` Revision 2 header for verified
> status and the companion design spec for context.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close six open CRITICAL/HIGH/MEDIUM transaction-lifecycle and deadlock-detector findings (CF-1, C-2, H-1, H-3, H-8, MF-4) by replacing `g_trx_lifecycle_lock` with an intrusive refcount on `tidesdb_trx_t`, a structured `KillState` flag, and a `tdb_global` shutdown drain barrier — shipping as v0.4.0.

**Architecture:** Strangler-fig migration across 10 PRs. New types (`TrxHandle`, `GlobalRef`, `KillState`, `LiveTrxRegistry`) land in PR 1. PRs 2–6 migrate each existing call site one at a time, with both the old rwlock and the new refcount paths running side by side. PR 7 deletes the rwlock. PRs 8–10 add hardening (debug lock-order assert, TSAN nightly CI, docs).

**Tech Stack:** MySQL 9.7 storage-engine plugin (C++17 in `plugin/`), MySQL `mysql_mutex_t`/`mysql_cond_t`/`mysql_rwlock_t` primitives, MTR (`mysql-test-run`) for integration tests, `debug_sync` for deterministic race tests, ASAN/UBSAN (per-PR CI) and TSAN (nightly CI). Build via the existing `Dockerfile.mtr` + `cmake --build`.

**Spec:** `docs/superpowers/specs/2026-06-02-txn-lifecycle-refactor-design.md` (commit `4659b08`). Read §5–§9 before implementing migrations.

---

## How this plan is organized

Each of the 10 PRs in spec §10 is a top-level **Task** here. Inside each Task, the work is broken into bite-sized **Steps** following TDD: write the failing test → verify it fails → implement → verify it passes → commit. PR 1 (greenfield foundation code) is the most concrete; PRs 2–6 (existing-code migrations) specify the shape of the change and the regression test, and call out the locator pattern for the existing site rather than inline line numbers, because (a) `ha_tidesdb.cc` is 8 688 lines and (b) the implementing agent has the repo open.

Each PR is independent enough that a fresh subagent dispatched on it can finish without reading the prior PR's worktree — they only need this plan, the spec, and the repo at HEAD.

Before any Task, the implementing agent should:
1. Read this Task's "Pre-conditions" section.
2. Skim §6 (Components & APIs), §7 (Data flow), §8 (Error handling) of the spec for shared context.
3. Read the spec section called out in the Task header.

---

## File map

**Created** (new files):

| File | Responsibility | First introduced in |
|---|---|---|
| `plugin/tidesdb_trx_handle.h` | `TrxHandle`, `GlobalRef`, `KillState`, `PendingSysvarChange`, `LiveTrxRegistry` declarations | PR 1 |
| `plugin/tidesdb_trx_handle.cc` | Implementations | PR 1 |
| `plugin/tidesdb_lock_rank.h` | Per-thread lock-rank debug-assert (header-only) | PR 8 |
| `unittest/tidesdb/CMakeLists.txt` | Unit-test build | PR 1 |
| `unittest/tidesdb/trx_handle_test.cc` | `TrxHandleTest` | PR 1 |
| `unittest/tidesdb/live_trx_registry_test.cc` | `LiveTrxRegistryTest` | PR 1 |
| `unittest/tidesdb/global_ref_test.cc` | `GlobalRefTest` | PR 1 |
| `unittest/tidesdb/pending_change_queue_test.cc` | `PendingChangeQueueTest` | PR 1 |
| `unittest/tidesdb/kill_state_test.cc` | `KillStateTest` | PR 1 |
| `mysql-test-suite/t/tidesdb_cf1_kill_query_race.test` | CF-1 regression | PR 3 |
| `mysql-test-suite/t/tidesdb_h3_walker_race.test` | H-3 regression | PR 4 |
| `mysql-test-suite/t/tidesdb_kill_during_cond_wait.test` | kill-during-wait | PR 4 |
| `mysql-test-suite/t/tidesdb_h8_sysvar_pending.test` | H-8 semantic | PR 5 |
| `mysql-test-suite/t/tidesdb_c2_shutdown_race.test` | C-2 regression | PR 6 |
| `mysql-test-suite/t/tidesdb_shutdown_blocked_session.test` | shutdown timeout | PR 6 |
| `mysql-test-suite/t/tidesdb_killfuzz.test` | 30-min stress | PR 9 |
| `mysql-test-suite/tsan-suppressions.txt` | TSAN known-non-bug suppressions w/ comments | PR 9 |
| `docker/Dockerfile.mtr.asan` | ASAN+UBSAN build flavor | PR 1 |
| `docker/Dockerfile.mtr.tsan` | TSAN build flavor | PR 9 |
| `.github/workflows/sanitizers.yml` | per-PR ASAN gate + nightly TSAN | PR 1 (ASAN) + PR 9 (TSAN) |

**Modified** (existing files):

| File | What changes | Touched in |
|---|---|---|
| `plugin/ha_tidesdb.h` | Add fields to `tidesdb_trx_t` and `tdb_global_t` | PR 2 |
| `plugin/ha_tidesdb.cc` | `tidesdb_close_connection`, `tidesdb_hton_kill_query`, statement-begin hook, plugin uninstall path, sysvar callbacks (3 of them, per audit in PR 5), debug-only lock-rank wrappers | PRs 2, 3, 5, 6, 7, 8 |
| `plugin/tidesdb_row_lock.cc` | `tdb_lock_would_deadlock`, `row_lock_acquire` cond-wait loop | PR 4 |
| `plugin/CMakeLists.txt` | Add `tidesdb_trx_handle.cc` to the plugin target; add `unittest/tidesdb/` subdirectory | PR 1 |
| `KNOWN-ISSUES.md` | Remove CF-1, C-2, H-3, H-8, MF-4 from open list | PR 10 |
| `CHANGELOG.md` | v0.4.0 entry with H-8 breaking-change note | PR 10 |
| `.github/workflows/release.yml` | Add "two consecutive nightly greens" gate | PR 9 |

---

## Pre-flight: v0.3.x cherry-pick hotfix

This ships **in parallel** with PR 1 (not on the v0.4 critical path, but blocking nothing on the v0.4 timeline either). Done as a separate small piece of work on a `v0.3.x` maintenance branch.

### Task 0: v0.3.2 cherry-pick hotfix for CF-1 only

**Spec section:** §10 "Branching and version target."

**Files:**
- Modify (on `v0.3.x` branch): `plugin/ha_tidesdb.cc` — the `tidesdb_hton_kill_query` function

**Pre-conditions:**
- `main` is at `4659b08` or later.
- An author with push access for the `v0.3.x` branch and Docker Hub.

- [ ] **Step 0.1: Create the v0.3.x maintenance branch from v0.3.1**

```bash
cd /home/corvin/tidesdb-mysql
git fetch --tags
git switch -c v0.3.x v0.3.1
git push -u origin v0.3.x
```

- [ ] **Step 0.2: Locate `tidesdb_hton_kill_query` in ha_tidesdb.cc**

```bash
grep -n "^static void tidesdb_hton_kill_query" plugin/ha_tidesdb.cc
```

Expected: one match, around line 4400.

- [ ] **Step 0.3: Apply the minimal rdlock wrap around the kill_query body**

Edit the function so the body is bracketed by `mysql_rwlock_rdlock(&g_trx_lifecycle_lock)` / `mysql_rwlock_unlock(&g_trx_lifecycle_lock)`, matching the existing walker pattern:

```cpp
static void tidesdb_hton_kill_query(handlerton *, THD *thd, enum thd_kill_levels)
{
    if (!thd) return;
    mysql_rwlock_rdlock(&g_trx_lifecycle_lock);     // <-- ADDED
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx) { mysql_rwlock_unlock(&g_trx_lifecycle_lock); return; }
    tdb_row_lock_t *wait = trx->waiting_on.load(std::memory_order_acquire);
    if (wait) {
        mysql_mutex_lock(&wait->part->mutex);
        mysql_cond_broadcast(&wait->cond);
        mysql_mutex_unlock(&wait->part->mutex);
    }
    mysql_rwlock_unlock(&g_trx_lifecycle_lock);     // <-- ADDED
}
```

- [ ] **Step 0.4: Build the image and run the MTR suite to confirm no regression**

```bash
DOCKER_BUILDKIT=0 sg docker -c "docker build -f docker/Dockerfile.mtr -t tidesdb/mysql-mtr:9.7-hotfix ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:9.7-hotfix bash -lc 'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force --max-test-fail=0'"
```

Expected: `All 61 tests were successful.` (Same as the v0.3.1 baseline.)

- [ ] **Step 0.5: Commit and tag v0.3.2 on the v0.3.x branch**

```bash
git add plugin/ha_tidesdb.cc
git commit -q -m "fix(CF-1): wrap tidesdb_hton_kill_query body in g_trx_lifecycle_lock rdlock

The H-3 fix wrapped tdb_lock_would_deadlock with g_trx_lifecycle_lock
read-lock and tidesdb_close_connection's my_free(trx) with the
write-lock. tidesdb_hton_kill_query was missed: concurrent KILL QUERY
during a connection close could read trx via thd_get_ha_data, the close
thread enters the wrlock write section and freed trx, then the kill
thread dereferenced trx->waiting_on -- UAF.

This is the minimal-diff hotfix for the stable v0.3.x line. The
structural fix is the txn-lifecycle refactor landing on main as v0.4.0;
see docs/superpowers/specs/2026-06-02-txn-lifecycle-refactor-design.md."
git tag -a v0.3.2 -m "v0.3.2 -- CF-1 hotfix (kill_query UAF)"
git push origin v0.3.x v0.3.2
```

- [ ] **Step 0.6: Run `scripts/release-docker.sh` to push the v0.3.2 image and create the GitHub release**

```bash
git switch v0.3.2
./scripts/release-docker.sh --plugin=0.3.2
```

Expected: pushes `perconalab/tidesdb-mysql:0.3.2` (and `:latest` — but only if v0.3.2 is newer than the current latest; otherwise omit `:latest`), creates the GitHub release.

- [ ] **Step 0.7: Switch back to main for the v0.4.0 work**

```bash
git switch main
```

---

## PR 1 — Foundation

**Goal:** Land `tidesdb_trx_handle.{h,cc}` with all five new types (`TrxHandle`, `GlobalRef`, `KillState`, `LiveTrxRegistry`, `PendingSysvarChange`) + the five unit-test suites. Wired into the build but **not yet referenced** by any existing code. ASAN+UBSAN CI build added.

**Closes:** none — foundation.

**Spec sections:** §6 (Components & APIs), §8 (Error handling for the types).

**Pre-conditions:**
- On `main`, working tree clean.
- The agent is in `/home/corvin/tidesdb-mysql`.

### Task 1.1: Add the new files to the build

**Files:**
- Create: `plugin/tidesdb_trx_handle.h` (skeleton)
- Create: `plugin/tidesdb_trx_handle.cc` (skeleton)
- Modify: `plugin/CMakeLists.txt`

- [ ] **Step 1.1.1: Create `plugin/tidesdb_trx_handle.h` with the include guard and empty namespace**

```cpp
// SPDX-License-Identifier: GPL-2.0
//
// Transaction-lifecycle handles for tidesdb-mysql.
//
// See docs/superpowers/specs/2026-06-02-txn-lifecycle-refactor-design.md
// for the design. Briefly:
//
//   - TrxHandle is an RAII strong-ref on a tidesdb_trx_t. Acquired via
//     try_acquire(thd); fails (returns null-handle) if the trx is being
//     torn down or has been killed.
//   - GlobalRef is the same shape for tdb_global; fails if the shutdown
//     state machine has moved out of RUNNING.
//   - KillState is the atomic per-trx kill flag polled at safe points.
//   - LiveTrxRegistry is the set of live trx_t* that try_acquire consults.
//   - PendingSysvarChange is the per-session record pushed by sysvar
//     update callbacks for cross-session sysvar changes (H-8 fix).

#pragma once

#include <atomic>
#include <cstdint>

namespace tidesdb_mysql {

// Forward declarations -- defined in ha_tidesdb.h.
struct tidesdb_trx_t;
struct tdb_global_t;

}  // namespace tidesdb_mysql
```

- [ ] **Step 1.1.2: Create `plugin/tidesdb_trx_handle.cc` with the include and namespace**

```cpp
// SPDX-License-Identifier: GPL-2.0
//
// See plugin/tidesdb_trx_handle.h for the contract.

#include "plugin/tidesdb_trx_handle.h"

namespace tidesdb_mysql {

}  // namespace tidesdb_mysql
```

- [ ] **Step 1.1.3: Add the new translation unit to `plugin/CMakeLists.txt`**

Locate the line listing the plugin source files (look for `ha_tidesdb.cc` in `MYSQL_ADD_PLUGIN`). Add `tidesdb_trx_handle.cc` to the same list, alphabetically near `tidesdb_row_lock.cc`.

```bash
grep -n "tidesdb_row_lock.cc" plugin/CMakeLists.txt
```

After locating the source list, edit so that `tidesdb_trx_handle.cc` is listed alongside `tidesdb_row_lock.cc` (the surrounding lines will show the comma/format convention to match).

- [ ] **Step 1.1.4: Build to confirm the new files compile**

```bash
DOCKER_BUILDKIT=0 sg docker -c "docker build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7-pr1 ."
```

Expected: build succeeds; `ha_tidesdb.so` exists in the image (`docker run --rm tidesdb/mysql:9.7-pr1 ls /build/mysql-server/build/plugin_output_directory/ha_tidesdb.so` returns 0).

- [ ] **Step 1.1.5: Commit**

```bash
git add plugin/tidesdb_trx_handle.h plugin/tidesdb_trx_handle.cc plugin/CMakeLists.txt
git commit -q -m "feat(txn-refactor PR1): add tidesdb_trx_handle.{h,cc} skeleton

First step of the txn-lifecycle refactor. Empty translation unit that
will hold TrxHandle, GlobalRef, KillState, LiveTrxRegistry, and
PendingSysvarChange. Wired into the plugin build; not yet referenced
by any other code.

See docs/superpowers/specs/2026-06-02-txn-lifecycle-refactor-design.md."
```

### Task 1.2: KillState enum

**Files:**
- Modify: `plugin/tidesdb_trx_handle.h`
- Create: `unittest/tidesdb/kill_state_test.cc`
- Modify: `unittest/tidesdb/CMakeLists.txt` (created later in Task 1.7)

- [ ] **Step 1.2.1: Add `KillState` to the header**

In `plugin/tidesdb_trx_handle.h`, inside `namespace tidesdb_mysql {`:

```cpp
// Atomic kill state on each tidesdb_trx_t. Polled at every defined safe
// point (cond-wait wakes, statement begin). See spec §6.
enum class KillState : uint8_t {
    RUNNING      = 0,
    KILL_PENDING = 1,
    KILLED       = 2,
};
```

- [ ] **Step 1.2.2: Verify the file still compiles**

```bash
sg docker -c "docker build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7-pr1 ."
```

Expected: success.

- [ ] **Step 1.2.3: Commit**

```bash
git add plugin/tidesdb_trx_handle.h
git commit -q -m "feat(txn-refactor PR1): KillState enum (RUNNING/KILL_PENDING/KILLED)"
```

(The unit test for `KillState` lands in Task 1.7 below, once the test scaffolding is in place.)

### Task 1.3: PendingSysvarChange + queue contract

**Files:**
- Modify: `plugin/tidesdb_trx_handle.h`

- [ ] **Step 1.3.1: Add `PendingSysvarChange` + the queue typedef**

In `plugin/tidesdb_trx_handle.h`:

```cpp
// Per-session pending sysvar change, applied at the next statement-begin
// safe point. See spec §7 scenario 4 + §8 "Pending-sysvar-change queue
// overflow." The Kind enum is populated as part of the sysvar-callback
// audit in PR 5.
struct PendingSysvarChange {
    enum class Kind : uint8_t {
        // Populated in PR 5 (sysvar callbacks audit).
    };
    Kind kind;
    uint64_t value;
};

// Bounded MPSC queue capacity per the spec.
inline constexpr size_t PENDING_CHANGE_QUEUE_CAP = 16;
```

- [ ] **Step 1.3.2: Build, commit**

```bash
sg docker -c "docker build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7-pr1 ."
git add plugin/tidesdb_trx_handle.h
git commit -q -m "feat(txn-refactor PR1): PendingSysvarChange + queue capacity constant"
```

### Task 1.4: LiveTrxRegistry

**Files:**
- Modify: `plugin/tidesdb_trx_handle.h`
- Modify: `plugin/tidesdb_trx_handle.cc`

- [ ] **Step 1.4.1: Declare LiveTrxRegistry in the header**

In `plugin/tidesdb_trx_handle.h`:

```cpp
// The set of live tidesdb_trx_t* that TrxHandle::try_acquire consults
// before bumping a refcount. Adds happen on first external_lock of a
// session; removes happen at the start of tidesdb_close_connection,
// BEFORE the owner's refcount is dropped. Lookup is from a remote thread
// under registry_lock.
//
// Starting representation: mysql_mutex_t-guarded std::unordered_set.
// The registry is NOT on a hot path (touched on connection open/close +
// kill_query); the lock is brief. A lock-free alternative is implementable
// behind this interface later. See spec §6 "The live-trx registry."
class LiveTrxRegistry {
public:
    LiveTrxRegistry();
    ~LiveTrxRegistry();

    // Owner inserts on first external_lock of the session.
    void add(tidesdb_trx_t *trx);

    // Owner removes at the start of tidesdb_close_connection, BEFORE
    // dropping the owner's refcount. Idempotent on already-removed.
    void remove(tidesdb_trx_t *trx);

    // Remote-thread lookup. If trx is present AND its kill_state is not
    // KILLED, atomically increments refcount and returns true. Otherwise
    // returns false without modifying anything.
    bool try_pin(tidesdb_trx_t *trx);

    // Size, for diagnostics / debug asserts. Not lock-free.
    size_t size_for_debug() const;

private:
    LiveTrxRegistry(const LiveTrxRegistry&) = delete;
    LiveTrxRegistry& operator=(const LiveTrxRegistry&) = delete;

    struct Impl;
    Impl *impl_;
};
```

- [ ] **Step 1.4.2: Implement LiveTrxRegistry in the .cc**

In `plugin/tidesdb_trx_handle.cc`:

```cpp
#include "plugin/tidesdb_trx_handle.h"
#include "plugin/ha_tidesdb.h"            // for tidesdb_trx_t
#include "mysql/psi/mysql_mutex.h"

#include <unordered_set>

namespace tidesdb_mysql {

struct LiveTrxRegistry::Impl {
    mysql_mutex_t mutex;
    std::unordered_set<tidesdb_trx_t*> set;
};

LiveTrxRegistry::LiveTrxRegistry() : impl_(new Impl) {
    mysql_mutex_init(/* PSI key */ 0, &impl_->mutex, MY_MUTEX_INIT_FAST);
}

LiveTrxRegistry::~LiveTrxRegistry() {
    mysql_mutex_destroy(&impl_->mutex);
    delete impl_;
}

void LiveTrxRegistry::add(tidesdb_trx_t *trx) {
    mysql_mutex_lock(&impl_->mutex);
    impl_->set.insert(trx);
    mysql_mutex_unlock(&impl_->mutex);
}

void LiveTrxRegistry::remove(tidesdb_trx_t *trx) {
    mysql_mutex_lock(&impl_->mutex);
    impl_->set.erase(trx);
    mysql_mutex_unlock(&impl_->mutex);
}

bool LiveTrxRegistry::try_pin(tidesdb_trx_t *trx) {
    mysql_mutex_lock(&impl_->mutex);
    auto it = impl_->set.find(trx);
    if (it == impl_->set.end()) {
        mysql_mutex_unlock(&impl_->mutex);
        return false;
    }
    if (trx->kill_state.load(std::memory_order_acquire) == KillState::KILLED) {
        mysql_mutex_unlock(&impl_->mutex);
        return false;
    }
    trx->refcount.fetch_add(1, std::memory_order_acq_rel);
    mysql_mutex_unlock(&impl_->mutex);
    return true;
}

size_t LiveTrxRegistry::size_for_debug() const {
    mysql_mutex_lock(&impl_->mutex);
    size_t n = impl_->set.size();
    mysql_mutex_unlock(&impl_->mutex);
    return n;
}

}  // namespace tidesdb_mysql
```

> NOTE: `trx->refcount` and `trx->kill_state` are referenced here but the fields are added to `tidesdb_trx_t` in PR 2. Build will fail at this point — fixed in the next step by adding a minimal stub for the references.

- [ ] **Step 1.4.3: Add forward-declaration stubs for the fields**

In `plugin/tidesdb_trx_handle.h`, after the `tidesdb_trx_t` forward declaration, add a section noting the contract:

```cpp
// CONTRACT: tidesdb_trx_t MUST expose the following fields, added in PR 2:
//
//   std::atomic<uint32_t>           refcount;     // starts at 1 (owner ref)
//   std::atomic<KillState>          kill_state;   // starts at RUNNING
//   std::atomic<bool>               in_registry;  // starts at true
//   tidesdb_mysql::PendingChangeQueue pending_changes;
//
// These are declared in plugin/ha_tidesdb.h.
```

Then, in `plugin/ha_tidesdb.h`, locate `struct tidesdb_trx_t {` and add a one-line forward stub of just `refcount` and `kill_state` (the full additions land in PR 2). This is a temporary measure so PR 1 compiles standalone:

```cpp
struct tidesdb_trx_t {
    // ... existing fields ...

    // Added in PR 1 to allow tidesdb_trx_handle.cc to compile. The full
    // set of refactor fields lands in PR 2.
    std::atomic<uint32_t> refcount{1};
    std::atomic<tidesdb_mysql::KillState> kill_state{
        tidesdb_mysql::KillState::RUNNING};
};
```

- [ ] **Step 1.4.4: Build to confirm**

```bash
sg docker -c "docker build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7-pr1 ."
```

Expected: success.

- [ ] **Step 1.4.5: Commit**

```bash
git add plugin/tidesdb_trx_handle.h plugin/tidesdb_trx_handle.cc plugin/ha_tidesdb.h
git commit -q -m "feat(txn-refactor PR1): LiveTrxRegistry (mutex-guarded set)

Adds the live-trx registry as the first cross-thread synchronisation
primitive of the refactor. Implementation is mysql_mutex_t +
std::unordered_set<tidesdb_trx_t*>; spec §6 explicitly chooses this over
a lock-free hash because the registry is not on any hot path.

Minimal stubs of refcount and kill_state are added to tidesdb_trx_t for
this PR to compile standalone; the full additions land in PR 2."
```

### Task 1.5: TrxHandle (RAII move-only)

**Files:**
- Modify: `plugin/tidesdb_trx_handle.h`
- Modify: `plugin/tidesdb_trx_handle.cc`

- [ ] **Step 1.5.1: Declare TrxHandle in the header**

```cpp
class TrxHandle {
public:
    // The acquire path. Reads trx from thd's plugin slot, consults the
    // live-trx registry under its mutex, atomically increments refcount.
    // Returns a null-handle (operator bool == false) if any of:
    //   - thd_get_ha_data(thd, tidesdb_hton) is null,
    //   - the trx has been removed from the registry,
    //   - trx->kill_state.load() == KILLED.
    static TrxHandle try_acquire(THD *thd, tdb_global_t *g);

    // Default-constructed: null-handle.
    TrxHandle() noexcept;

    // Move-only.
    TrxHandle(const TrxHandle&) = delete;
    TrxHandle& operator=(const TrxHandle&) = delete;
    TrxHandle(TrxHandle&& other) noexcept;
    TrxHandle& operator=(TrxHandle&& other) noexcept;

    ~TrxHandle();

    explicit operator bool() const noexcept { return trx_ != nullptr; }
    tidesdb_trx_t* operator->() const noexcept;       // debug-assert non-null
    tidesdb_trx_t* get() const noexcept { return trx_; }

private:
    explicit TrxHandle(tidesdb_trx_t *trx) noexcept : trx_(trx) {}
    tidesdb_trx_t *trx_;
};
```

Note: include `<sql/sql_class.h>` (THD) or forward-declare it; the codebase already includes it elsewhere — match the pattern in `tidesdb_row_lock.cc`.

- [ ] **Step 1.5.2: Implement TrxHandle in the .cc**

```cpp
TrxHandle TrxHandle::try_acquire(THD *thd, tdb_global_t *g) {
    if (!thd || !g) return TrxHandle{};
    tidesdb_trx_t *trx = static_cast<tidesdb_trx_t *>(
        thd_get_ha_data(thd, tidesdb_hton));
    if (!trx) return TrxHandle{};
    if (!g->live_trxs.try_pin(trx)) return TrxHandle{};
    return TrxHandle{trx};
}

TrxHandle::TrxHandle() noexcept : trx_(nullptr) {}

TrxHandle::TrxHandle(TrxHandle&& other) noexcept : trx_(other.trx_) {
    other.trx_ = nullptr;
}

TrxHandle& TrxHandle::operator=(TrxHandle&& other) noexcept {
    if (this != &other) {
        release_impl();
        trx_ = other.trx_;
        other.trx_ = nullptr;
    }
    return *this;
}

TrxHandle::~TrxHandle() {
    release_impl();
}

void TrxHandle::release_impl() {
    if (!trx_) return;
    uint32_t prev = trx_->refcount.fetch_sub(1, std::memory_order_acq_rel);
    // Underflow is a structural bug; cf. spec §8 "Refcount under-/over-flow."
    if (prev == 0) {
        std::abort();  // RELEASE_ASSERT
    }
    if (prev == 1) {
        // Last reference. The owner already removed from registry before
        // dropping its ref, so we're safe to free.
        my_free(trx_);
    }
    trx_ = nullptr;
}

tidesdb_trx_t* TrxHandle::operator->() const noexcept {
    assert(trx_ != nullptr);
    return trx_;
}
```

Add `release_impl()` as a private method in the header.

> NOTE: `g_engine_ctx` (or whatever symbol holds the `tdb_global_t*` in `ha_tidesdb.cc`) will be passed in. PR 1 doesn't change `tdb_global_t`'s layout but does add a `LiveTrxRegistry live_trxs` field to it. Do this now:

- [ ] **Step 1.5.3: Add LiveTrxRegistry to tdb_global_t**

In `plugin/ha_tidesdb.h`, locate `struct tdb_global_t {` or the equivalent symbol (search for `g_engine_ctx` to find which type it's pointing at). Add:

```cpp
// Added in PR 1. Populated when the plugin loads; cleared on uninstall
// AFTER the shutdown drain barrier (PR 6) confirms ref_count == 0.
tidesdb_mysql::LiveTrxRegistry live_trxs;
```

- [ ] **Step 1.5.4: Build to confirm**

```bash
sg docker -c "docker build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7-pr1 ."
```

Expected: success.

- [ ] **Step 1.5.5: Commit**

```bash
git add plugin/tidesdb_trx_handle.h plugin/tidesdb_trx_handle.cc plugin/ha_tidesdb.h
git commit -q -m "feat(txn-refactor PR1): TrxHandle RAII (move-only intrusive refcount)

The acquire path is try_acquire(thd, g) -- null on closed/killed/missing
trx. Destructor drops the refcount; RELEASE_ASSERT on underflow per
spec §8. Last ref frees with my_free (same allocator as my_malloc on the
trx alloc path). Adds LiveTrxRegistry as a tdb_global_t field."
```

### Task 1.6: GlobalRef + ShutdownState

**Files:**
- Modify: `plugin/tidesdb_trx_handle.h`
- Modify: `plugin/tidesdb_trx_handle.cc`
- Modify: `plugin/ha_tidesdb.h`

- [ ] **Step 1.6.1: Add ShutdownState + ref_count + shutdown_cv to tdb_global_t**

```cpp
struct tdb_global_t {
    // ... existing fields, plus the LiveTrxRegistry from Task 1.5 ...

    // Shutdown drain barrier. See spec §7 scenario 3.
    std::atomic<tidesdb_mysql::ShutdownState> shutdown_state{
        tidesdb_mysql::ShutdownState::RUNNING};
    std::atomic<uint32_t> ref_count{0};
    mysql_mutex_t shutdown_mutex;
    mysql_cond_t  shutdown_cv;
};
```

In the existing `tdb_global` init path (search `mysql_mutex_init` in `ha_tidesdb.cc` to find similar init sequences), add init of `shutdown_mutex` and `shutdown_cv`.

- [ ] **Step 1.6.2: Add ShutdownState + GlobalRef to the header**

```cpp
enum class ShutdownState : uint8_t { RUNNING, DRAINING, DRAINED, STOPPED };

class GlobalRef {
public:
    static GlobalRef try_acquire(tdb_global_t *g);

    GlobalRef() noexcept;
    GlobalRef(const GlobalRef&) = delete;
    GlobalRef& operator=(const GlobalRef&) = delete;
    GlobalRef(GlobalRef&& other) noexcept;
    GlobalRef& operator=(GlobalRef&& other) noexcept;
    ~GlobalRef();

    explicit operator bool() const noexcept { return g_ != nullptr; }
    tdb_global_t* operator->() const noexcept;
    tdb_global_t* get() const noexcept { return g_; }

private:
    explicit GlobalRef(tdb_global_t *g) noexcept : g_(g) {}
    void release_impl();
    tdb_global_t *g_;
};
```

- [ ] **Step 1.6.3: Implement GlobalRef in the .cc**

```cpp
GlobalRef GlobalRef::try_acquire(tdb_global_t *g) {
    if (!g) return GlobalRef{};
    // SEQ_CST acquire-then-check, per spec §8 "GlobalRef::try_acquire."
    g->ref_count.fetch_add(1, std::memory_order_seq_cst);
    if (g->shutdown_state.load(std::memory_order_seq_cst)
        != ShutdownState::RUNNING) {
        g->ref_count.fetch_sub(1, std::memory_order_seq_cst);
        return GlobalRef{};
    }
    return GlobalRef{g};
}

GlobalRef::GlobalRef() noexcept : g_(nullptr) {}

GlobalRef::GlobalRef(GlobalRef&& other) noexcept : g_(other.g_) {
    other.g_ = nullptr;
}

GlobalRef& GlobalRef::operator=(GlobalRef&& other) noexcept {
    if (this != &other) {
        release_impl();
        g_ = other.g_;
        other.g_ = nullptr;
    }
    return *this;
}

GlobalRef::~GlobalRef() { release_impl(); }

void GlobalRef::release_impl() {
    if (!g_) return;
    uint32_t prev = g_->ref_count.fetch_sub(1, std::memory_order_seq_cst);
    if (prev == 0) std::abort();  // RELEASE_ASSERT
    if (prev == 1
        && g_->shutdown_state.load(std::memory_order_seq_cst)
           == ShutdownState::DRAINING) {
        mysql_mutex_lock(&g_->shutdown_mutex);
        mysql_cond_broadcast(&g_->shutdown_cv);
        mysql_mutex_unlock(&g_->shutdown_mutex);
    }
    g_ = nullptr;
}

tdb_global_t* GlobalRef::operator->() const noexcept {
    assert(g_ != nullptr);
    return g_;
}
```

- [ ] **Step 1.6.4: Build to confirm**

```bash
sg docker -c "docker build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7-pr1 ."
```

Expected: success.

- [ ] **Step 1.6.5: Commit**

```bash
git add plugin/tidesdb_trx_handle.h plugin/tidesdb_trx_handle.cc plugin/ha_tidesdb.h
git commit -q -m "feat(txn-refactor PR1): GlobalRef + ShutdownState

Acquire/release uses seq_cst so that the shutdown thread's
'store DRAINING; wait for ref_count == 0' protocol is correct against
'fetch_add; load shutdown_state' on the worker side (spec §8). Drop of
last ref during DRAINING broadcasts shutdown_cv to unblock the waiter."
```

### Task 1.7: Unit-test scaffold + KillState test

**Files:**
- Create: `unittest/tidesdb/CMakeLists.txt`
- Create: `unittest/tidesdb/kill_state_test.cc`
- Modify: `plugin/CMakeLists.txt`

- [ ] **Step 1.7.1: Create the unit-test subdirectory CMakeLists.txt**

```cmake
# unittest/tidesdb/CMakeLists.txt
#
# Standalone unit tests for the tidesdb plugin's lifetime primitives.
# Links against gtest via the MySQL bundled gtest target.

include(GoogleTest)

set(TIDESDB_UNIT_TEST_SOURCES
    kill_state_test.cc
    pending_change_queue_test.cc
    live_trx_registry_test.cc
    trx_handle_test.cc
    global_ref_test.cc
)

foreach(test_src ${TIDESDB_UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(tidesdb_${test_name} ${test_src}
        ${CMAKE_SOURCE_DIR}/plugin/tidesdb/plugin/tidesdb_trx_handle.cc)
    target_include_directories(tidesdb_${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/plugin/tidesdb
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(tidesdb_${test_name} PRIVATE
        gunit_small)
    add_test(NAME tidesdb_${test_name} COMMAND tidesdb_${test_name})
endforeach()
```

> Pre-populate empty test files so the foreach loop doesn't fail before Tasks 1.8–1.11 add real content:

```bash
touch unittest/tidesdb/pending_change_queue_test.cc
touch unittest/tidesdb/live_trx_registry_test.cc
touch unittest/tidesdb/trx_handle_test.cc
touch unittest/tidesdb/global_ref_test.cc
```

- [ ] **Step 1.7.2: Wire the subdirectory into the plugin CMakeLists.txt**

In `plugin/CMakeLists.txt`, after the `MYSQL_ADD_PLUGIN` block, add:

```cmake
if (WITH_UNIT_TESTS)
    add_subdirectory(unittest/tidesdb)
endif()
```

(Use whatever guard variable the MySQL build already uses for unit tests — `grep -n "WITH_UNIT_TESTS" CMakeLists.txt` to confirm.)

- [ ] **Step 1.7.3: Write the failing KillState test**

`unittest/tidesdb/kill_state_test.cc`:

```cpp
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

#include "plugin/tidesdb_trx_handle.h"

using tidesdb_mysql::KillState;

TEST(KillStateTest, DefaultRunning) {
    std::atomic<KillState> s{KillState::RUNNING};
    EXPECT_EQ(s.load(), KillState::RUNNING);
}

TEST(KillStateTest, ConcurrentCasIdempotent) {
    std::atomic<KillState> s{KillState::RUNNING};
    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::atomic<int> n_pending{0};
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            KillState expected = KillState::RUNNING;
            if (s.compare_exchange_strong(expected, KillState::KILL_PENDING)) {
                n_pending.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(n_pending.load(), 1);  // exactly one killer wins the CAS
    EXPECT_EQ(s.load(), KillState::KILL_PENDING);
}
```

- [ ] **Step 1.7.4: Build the unit tests**

```bash
sg docker -c "docker run --rm -v $(pwd):/src tidesdb/mysql:9.7-pr1 \
    bash -lc 'cd /src && mkdir -p build && cd build \
    && cmake -DWITH_UNIT_TESTS=ON .. && cmake --build . --target tidesdb_kill_state_test'"
```

Expected: build succeeds.

> If the in-repo build path doesn't fit your environment, instead extend `Dockerfile.mtr` to build with `-DWITH_UNIT_TESTS=ON` and run via that image. The intent: this test runs in CI under ASAN+UBSAN per PR (see Task 1.12).

- [ ] **Step 1.7.5: Run the test**

```bash
./build/tidesdb_kill_state_test
```

Expected: `[==========] 2 tests passed.`

- [ ] **Step 1.7.6: Commit**

```bash
git add unittest/tidesdb/ plugin/CMakeLists.txt
git commit -q -m "test(txn-refactor PR1): unit-test scaffold + KillStateTest

Adds unittest/tidesdb/ subdirectory + gtest-based KillState concurrency
test. Empty stubs for the other tests; populated in Tasks 1.8-1.11."
```

### Task 1.8: PendingChangeQueueTest

**Files:**
- Modify: `unittest/tidesdb/pending_change_queue_test.cc`
- Modify: `plugin/tidesdb_trx_handle.h` (add the queue type)
- Modify: `plugin/tidesdb_trx_handle.cc`

- [ ] **Step 1.8.1: Add a concrete bounded-queue type**

In `plugin/tidesdb_trx_handle.h`:

```cpp
// Bounded, single-consumer (the owning session), multi-producer (sysvar
// callbacks from other sessions) queue. Returns false on full from
// try_push; FIFO ordering preserved. Spec §8.
class PendingChangeQueue {
public:
    PendingChangeQueue();
    ~PendingChangeQueue();

    // Multi-producer; returns false if at capacity.
    bool try_push(const PendingSysvarChange& c);

    // Single-consumer; returns false if empty.
    bool try_pop(PendingSysvarChange *out);

    size_t size_for_debug() const;

private:
    PendingChangeQueue(const PendingChangeQueue&) = delete;
    PendingChangeQueue& operator=(const PendingChangeQueue&) = delete;

    struct Impl;
    Impl *impl_;
};
```

In `plugin/tidesdb_trx_handle.cc`:

```cpp
struct PendingChangeQueue::Impl {
    mysql_mutex_t mutex;
    PendingSysvarChange ring[PENDING_CHANGE_QUEUE_CAP];
    size_t head{0};  // next pop slot
    size_t tail{0};  // next push slot
    size_t count{0};
};

PendingChangeQueue::PendingChangeQueue() : impl_(new Impl) {
    mysql_mutex_init(0, &impl_->mutex, MY_MUTEX_INIT_FAST);
}
PendingChangeQueue::~PendingChangeQueue() {
    mysql_mutex_destroy(&impl_->mutex); delete impl_;
}
bool PendingChangeQueue::try_push(const PendingSysvarChange& c) {
    mysql_mutex_lock(&impl_->mutex);
    if (impl_->count == PENDING_CHANGE_QUEUE_CAP) {
        mysql_mutex_unlock(&impl_->mutex); return false;
    }
    impl_->ring[impl_->tail] = c;
    impl_->tail = (impl_->tail + 1) % PENDING_CHANGE_QUEUE_CAP;
    ++impl_->count;
    mysql_mutex_unlock(&impl_->mutex);
    return true;
}
bool PendingChangeQueue::try_pop(PendingSysvarChange *out) {
    mysql_mutex_lock(&impl_->mutex);
    if (impl_->count == 0) {
        mysql_mutex_unlock(&impl_->mutex); return false;
    }
    *out = impl_->ring[impl_->head];
    impl_->head = (impl_->head + 1) % PENDING_CHANGE_QUEUE_CAP;
    --impl_->count;
    mysql_mutex_unlock(&impl_->mutex);
    return true;
}
size_t PendingChangeQueue::size_for_debug() const {
    mysql_mutex_lock(&impl_->mutex);
    size_t n = impl_->count;
    mysql_mutex_unlock(&impl_->mutex);
    return n;
}
```

Also: in `plugin/ha_tidesdb.h`, add to `tidesdb_trx_t`:

```cpp
tidesdb_mysql::PendingChangeQueue pending_changes;
```

- [ ] **Step 1.8.2: Write the failing test**

`unittest/tidesdb/pending_change_queue_test.cc`:

```cpp
#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "plugin/tidesdb_trx_handle.h"

using tidesdb_mysql::PendingChangeQueue;
using tidesdb_mysql::PendingSysvarChange;
using tidesdb_mysql::PENDING_CHANGE_QUEUE_CAP;

TEST(PendingChangeQueueTest, PushPopFifo) {
    PendingChangeQueue q;
    for (uint64_t i = 0; i < PENDING_CHANGE_QUEUE_CAP; ++i) {
        EXPECT_TRUE(q.try_push({{}, i}));
    }
    PendingSysvarChange out;
    for (uint64_t i = 0; i < PENDING_CHANGE_QUEUE_CAP; ++i) {
        EXPECT_TRUE(q.try_pop(&out));
        EXPECT_EQ(out.value, i);
    }
    EXPECT_FALSE(q.try_pop(&out));
}

TEST(PendingChangeQueueTest, OverflowReturnsFalse) {
    PendingChangeQueue q;
    for (size_t i = 0; i < PENDING_CHANGE_QUEUE_CAP; ++i) {
        EXPECT_TRUE(q.try_push({{}, i}));
    }
    EXPECT_FALSE(q.try_push({{}, 99}));
}

TEST(PendingChangeQueueTest, ConcurrentProducers) {
    PendingChangeQueue q;
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 100;
    std::atomic<int> n_pushed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kProducers; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < kPerProducer; ++j) {
                if (q.try_push({{}, 0})) n_pushed.fetch_add(1);
            }
        });
    }
    // Consumer drains in parallel.
    std::thread consumer([&] {
        PendingSysvarChange c;
        int popped = 0;
        while (popped < kProducers * kPerProducer) {
            if (q.try_pop(&c)) ++popped;
            else std::this_thread::yield();
        }
    });
    for (auto& t : threads) t.join();
    consumer.join();
    EXPECT_EQ(n_pushed.load(), kProducers * kPerProducer);
}
```

- [ ] **Step 1.8.3: Build + run + commit**

```bash
sg docker -c "docker run --rm -v $(pwd):/src tidesdb/mysql:9.7-pr1 \
    bash -lc 'cd /src/build && cmake --build . --target tidesdb_pending_change_queue_test'"
./build/tidesdb_pending_change_queue_test
git add plugin/tidesdb_trx_handle.h plugin/tidesdb_trx_handle.cc \
        plugin/ha_tidesdb.h unittest/tidesdb/pending_change_queue_test.cc
git commit -q -m "feat(txn-refactor PR1): PendingChangeQueue + tests

Bounded ring buffer, mysql_mutex_t-guarded. try_push returns false on
full; try_pop returns false on empty. FIFO ordering preserved across
concurrent producers."
```

### Task 1.9: LiveTrxRegistryTest

- [ ] **Step 1.9.1: Write the failing test**

`unittest/tidesdb/live_trx_registry_test.cc`:

```cpp
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

#include "plugin/tidesdb_trx_handle.h"
#include "plugin/ha_tidesdb.h"

using tidesdb_mysql::LiveTrxRegistry;
using tidesdb_mysql::KillState;

namespace {
// Minimal stand-in for tidesdb_trx_t; we only need the two atomic fields
// LiveTrxRegistry touches.
struct FakeTrx { tidesdb_trx_t real; };
FakeTrx make_trx() { return FakeTrx{}; }
}

TEST(LiveTrxRegistryTest, AddRemovePin) {
    LiveTrxRegistry reg;
    FakeTrx ft = make_trx();
    auto *trx = &ft.real;
    reg.add(trx);
    EXPECT_EQ(reg.size_for_debug(), 1u);
    EXPECT_TRUE(reg.try_pin(trx));
    EXPECT_EQ(trx->refcount.load(), 2u);
    reg.remove(trx);
    EXPECT_FALSE(reg.try_pin(trx));
}

TEST(LiveTrxRegistryTest, PinFailsOnKilled) {
    LiveTrxRegistry reg;
    FakeTrx ft = make_trx();
    auto *trx = &ft.real;
    reg.add(trx);
    trx->kill_state.store(KillState::KILLED);
    EXPECT_FALSE(reg.try_pin(trx));
}

TEST(LiveTrxRegistryTest, ConcurrentAddRemovePin) {
    LiveTrxRegistry reg;
    constexpr int kTrx = 32;
    std::vector<FakeTrx> trxs(kTrx);
    for (auto &ft : trxs) reg.add(&ft.real);

    constexpr int kPinners = 8;
    std::atomic<bool> stop{false};
    std::atomic<int>  pin_success{0};
    std::vector<std::thread> pinners;
    for (int i = 0; i < kPinners; ++i) {
        pinners.emplace_back([&, i] {
            while (!stop.load()) {
                auto *t = &trxs[(i * 7) % kTrx].real;
                if (reg.try_pin(t)) {
                    pin_success.fetch_add(1);
                    t->refcount.fetch_sub(1);  // release the pin
                }
            }
        });
    }

    // Remove half concurrently.
    std::thread remover([&] {
        for (int i = 0; i < kTrx / 2; ++i) reg.remove(&trxs[i].real);
    });
    remover.join();
    stop.store(true);
    for (auto& t : pinners) t.join();

    // No crash, no UAF; at least some pins succeeded on the surviving half.
    EXPECT_GT(pin_success.load(), 0);
}
```

- [ ] **Step 1.9.2: Build + run + commit**

```bash
sg docker -c "docker run --rm -v $(pwd):/src tidesdb/mysql:9.7-pr1 \
    bash -lc 'cd /src/build && cmake --build . --target tidesdb_live_trx_registry_test'"
./build/tidesdb_live_trx_registry_test
git add unittest/tidesdb/live_trx_registry_test.cc
git commit -q -m "test(txn-refactor PR1): LiveTrxRegistry concurrent add/remove/pin

Smoke + concurrent-stress tests. The stress test runs under ASAN in CI
so it catches UAF if try_pin's lock discipline is wrong; under TSAN
nightly it catches data races."
```

### Task 1.10: TrxHandleTest

- [ ] **Step 1.10.1: Write the test**

`unittest/tidesdb/trx_handle_test.cc`:

```cpp
#include <gtest/gtest.h>

#include "plugin/tidesdb_trx_handle.h"
#include "plugin/ha_tidesdb.h"

using tidesdb_mysql::TrxHandle;
using tidesdb_mysql::KillState;

namespace {
// Stand-in for THD; we only need thd_get_ha_data() to return our fake
// trx, which we inject via a custom test helper.
struct FakeThd { tidesdb_trx_t *plugin_slot; };

// Forward-declared test hook implemented in plugin/tidesdb_trx_handle.cc
// only when TIDESDB_TESTING is defined.
extern "C" tidesdb_trx_t* test_get_ha_data(void *thd);
}  // namespace

TEST(TrxHandleTest, NullThdReturnsNullHandle) {
    tdb_global_t g{};
    TrxHandle h = TrxHandle::try_acquire(nullptr, &g);
    EXPECT_FALSE(h);
}

TEST(TrxHandleTest, AcquireReleaseRoundtrip) {
    tdb_global_t g{};
    tidesdb_trx_t trx{};
    g.live_trxs.add(&trx);

    FakeThd thd{&trx};
    // Inject so TrxHandle::try_acquire's thd_get_ha_data returns &trx.
    // Implemented as a test-only override in plugin/tidesdb_trx_handle.cc.
    {
        TrxHandle h = TrxHandle::try_acquire(
            reinterpret_cast<THD*>(&thd), &g);
        ASSERT_TRUE(h);
        EXPECT_EQ(trx.refcount.load(), 2u);
    }
    EXPECT_EQ(trx.refcount.load(), 1u);  // RAII dropped one
    g.live_trxs.remove(&trx);
}

TEST(TrxHandleTest, AcquireOnKilledReturnsNull) {
    tdb_global_t g{};
    tidesdb_trx_t trx{};
    g.live_trxs.add(&trx);
    trx.kill_state.store(KillState::KILLED);
    FakeThd thd{&trx};
    TrxHandle h = TrxHandle::try_acquire(reinterpret_cast<THD*>(&thd), &g);
    EXPECT_FALSE(h);
}
```

> Implementation note: making `TrxHandle::try_acquire` testable from unit tests requires either a test-only override of `thd_get_ha_data` or a small refactor that takes the raw `tidesdb_trx_t*` as a constructor argument when the test mode is on. Use a `#ifdef TIDESDB_TESTING` shim in the .cc that bypasses `thd_get_ha_data` and reads the trx ptr directly from a test-supplied function pointer. The MTR-level integration tests in PR 3 will exercise the production path.

- [ ] **Step 1.10.2: Build + run + commit**

```bash
sg docker -c "docker run --rm -v $(pwd):/src tidesdb/mysql:9.7-pr1 \
    bash -lc 'cd /src/build && cmake --build . --target tidesdb_trx_handle_test'"
./build/tidesdb_trx_handle_test
git add unittest/tidesdb/trx_handle_test.cc plugin/tidesdb_trx_handle.cc
git commit -q -m "test(txn-refactor PR1): TrxHandle acquire/release roundtrip

Uses a TIDESDB_TESTING shim to inject the raw trx pointer (production
code reads via thd_get_ha_data). Verifies RAII drops refcount and that
acquire on a KILLED trx returns null."
```

### Task 1.11: GlobalRefTest

- [ ] **Step 1.11.1: Write the test**

`unittest/tidesdb/global_ref_test.cc`:

```cpp
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

#include "plugin/tidesdb_trx_handle.h"
#include "plugin/ha_tidesdb.h"

using tidesdb_mysql::GlobalRef;
using tidesdb_mysql::ShutdownState;

TEST(GlobalRefTest, AcquireWhileRunning) {
    tdb_global_t g{};
    GlobalRef r = GlobalRef::try_acquire(&g);
    EXPECT_TRUE(r);
    EXPECT_EQ(g.ref_count.load(), 1u);
}

TEST(GlobalRefTest, AcquireWhileDrainingReturnsNull) {
    tdb_global_t g{};
    g.shutdown_state.store(ShutdownState::DRAINING);
    GlobalRef r = GlobalRef::try_acquire(&g);
    EXPECT_FALSE(r);
    EXPECT_EQ(g.ref_count.load(), 0u);
}

TEST(GlobalRefTest, ShutdownDrainSequence) {
    tdb_global_t g{};
    // Spawn workers acquiring + releasing in a loop.
    std::atomic<bool> stop{false};
    constexpr int kWorkers = 4;
    std::vector<std::thread> workers;
    for (int i = 0; i < kWorkers; ++i) {
        workers.emplace_back([&] {
            while (!stop.load()) {
                GlobalRef r = GlobalRef::try_acquire(&g);
                if (r) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Initiate shutdown.
    g.shutdown_state.store(ShutdownState::DRAINING,
                           std::memory_order_seq_cst);

    // Wait for ref_count == 0 (with timeout).
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(5);
    while (g.ref_count.load() != 0
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(g.ref_count.load(), 0u);
    stop.store(true);
    for (auto& t : workers) t.join();
}
```

- [ ] **Step 1.11.2: Build + run + commit**

```bash
sg docker -c "docker run --rm -v $(pwd):/src tidesdb/mysql:9.7-pr1 \
    bash -lc 'cd /src/build && cmake --build . --target tidesdb_global_ref_test'"
./build/tidesdb_global_ref_test
git add unittest/tidesdb/global_ref_test.cc
git commit -q -m "test(txn-refactor PR1): GlobalRef seq-cst shutdown drain test

Verifies the spec §8 ordering: workers that try_acquire after DRAINING
get null and don't bump ref_count; workers in flight drop their refs;
ref_count reaches 0."
```

### Task 1.12: ASAN+UBSAN CI build

**Files:**
- Create: `docker/Dockerfile.mtr.asan`
- Create: `.github/workflows/sanitizers.yml`

- [ ] **Step 1.12.1: Create the ASAN+UBSAN Dockerfile**

Copy `docker/Dockerfile.mtr` and add the sanitizer flags. Skeleton:

```Dockerfile
# docker/Dockerfile.mtr.asan
#
# ASAN+UBSAN build of the MTR runner. Same shape as Dockerfile.mtr but
# the MySQL + plugin build are compiled with the sanitizers on. Used by
# .github/workflows/sanitizers.yml as the per-PR gate.

# (head/preamble identical to Dockerfile.mtr; reuse the cached layers)

# At the cmake step, add:
#   -DCMAKE_BUILD_TYPE=Debug
#   -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
#   -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
#   -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
#   -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
```

(Re-use the `Dockerfile.mtr` content verbatim except for the `cmake -S . -B build ...` line, which gets the additional flags above.)

- [ ] **Step 1.12.2: Create the sanitizer CI workflow**

`.github/workflows/sanitizers.yml`:

```yaml
# Per-PR ASAN+UBSAN sanitizer build of the plugin + unit-tests + MTR
# suite. TSAN nightly is added later in PR 9.

name: Sanitizers (ASAN+UBSAN)
on:
  pull_request:
    branches: [main]
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: sanitizers-${{ github.ref }}
  cancel-in-progress: true

jobs:
  asan:
    runs-on: [self-hosted, linux, x64, tidesdb-release]
    timeout-minutes: 180
    steps:
      - uses: actions/checkout@v4
      - name: Build ASAN MTR image
        run: |
          docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan .
      - name: Run unit tests
        run: |
          docker run --rm tidesdb/mysql-mtr:asan bash -lc \
            'cd /build && ctest --output-on-failure -L tidesdb_unit'
      - name: Run MTR suite
        run: |
          docker run --rm tidesdb/mysql-mtr:asan bash -lc \
            'cd /build/mysql-server/build/mysql-test && \
             ./mtr --suite=tidesdb --force --max-test-fail=0'
```

- [ ] **Step 1.12.3: Verify the ASAN image builds + tests run**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:asan bash -lc \
  'cd /build && ctest --output-on-failure -L tidesdb_unit'"
```

Expected: all unit tests green.

- [ ] **Step 1.12.4: Commit + push PR 1**

```bash
git add docker/Dockerfile.mtr.asan .github/workflows/sanitizers.yml
git commit -q -m "ci(txn-refactor PR1): per-PR ASAN+UBSAN gate

Builds the plugin + MySQL with -fsanitize=address,undefined and runs
the unit tests + full MTR suite under it. TSAN nightly lands in PR 9."

# Push as a single PR (the PR 1 / Foundation PR).
git push -u origin HEAD:refs/heads/refactor/pr1-foundation
gh pr create --base main --head refactor/pr1-foundation \
    --title "txn-refactor PR1: foundation" \
    --body "See docs/superpowers/plans/2026-06-02-txn-lifecycle-refactor.md PR 1 section."
```

---

## PR 2 — Registry + refcount wired into trx_t lifecycle

**Goal:** `tidesdb_close_connection` removes the trx from the registry and drops the owner's refcount *in addition to* the existing `g_trx_lifecycle_lock` wrlock + `my_free`. Both code paths run side by side; behavior is identical to today. No findings closed yet.

**Closes:** none.

**Spec sections:** §6 "Additions to existing structs," §7 scenario 1 "ordering invariant," §10 PR 2.

**Pre-conditions:** PR 1 merged.

### Task 2.1: Add the remaining trx_t fields

- [ ] **Step 2.1.1: Add `in_registry` to `tidesdb_trx_t` in `plugin/ha_tidesdb.h`**

Locate the existing `tidesdb_trx_t` struct (was extended in PR 1 with `refcount` + `kill_state` + `pending_changes`). Add:

```cpp
std::atomic<bool> in_registry{true};
```

- [ ] **Step 2.1.2: Build, commit**

```bash
sg docker -c "docker build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7-pr2 ."
git add plugin/ha_tidesdb.h
git commit -q -m "feat(txn-refactor PR2): tidesdb_trx_t.in_registry flag"
```

### Task 2.2: Register the trx on first external_lock

- [ ] **Step 2.2.1: Find the first-external_lock site that allocates trx**

```bash
grep -n "external_lock" plugin/ha_tidesdb.cc | head -20
grep -n "tidesdb_trx_t.*my_malloc\|alloc.*tidesdb_trx_t" plugin/ha_tidesdb.cc
```

Identify the function (likely `ha_tidesdb::external_lock` or a helper it calls) that does `my_malloc(...sizeof(tidesdb_trx_t)...)` and `thd_set_ha_data(thd, tidesdb_hton, trx)`.

- [ ] **Step 2.2.2: Add a `g_engine_ctx->live_trxs.add(trx)` immediately after the trx allocation**

```cpp
// existing allocation
tidesdb_trx_t *trx = (tidesdb_trx_t *)my_malloc(..., sizeof(tidesdb_trx_t), MYF(MY_WME | MY_ZEROFILL));
// placement-construct the non-trivial fields if needed (refcount,
// kill_state, in_registry, pending_changes have in-class initialisers
// after PR 1).
new (trx) tidesdb_trx_t();
thd_set_ha_data(thd, tidesdb_hton, trx);
g_engine_ctx->live_trxs.add(trx);   // <-- ADDED
```

(The `g_engine_ctx` symbol may be different in this codebase — search for the existing global. Match what the rest of `ha_tidesdb.cc` uses.)

- [ ] **Step 2.2.3: Build + run MTR to verify nothing regressed**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr -t tidesdb/mysql-mtr:pr2 ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:pr2 bash -lc \
  'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force'"
```

Expected: 61/61.

- [ ] **Step 2.2.4: Commit**

```bash
git add plugin/ha_tidesdb.cc
git commit -q -m "feat(txn-refactor PR2): register trx in live_trxs on first external_lock"
```

### Task 2.3: Remove from registry + drop refcount in close_connection

- [ ] **Step 2.3.1: Locate `tidesdb_close_connection`**

```bash
grep -n "^.*tidesdb_close_connection" plugin/ha_tidesdb.cc
```

- [ ] **Step 2.3.2: Add the registry-remove + refcount drop ALONGSIDE the existing rwlock wrlock + my_free**

Keep the existing `g_trx_lifecycle_lock` wrlock + `my_free` for now (PR 7 removes them). Add the new steps so both paths coexist:

```cpp
static int tidesdb_close_connection(handlerton *, THD *thd) {
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx) return 0;

    // NEW: clear the slot first so future try_acquire's get null.
    thd_set_ha_data(thd, tidesdb_hton, nullptr);

    // NEW: remove from the registry BEFORE dropping the owner's refcount
    // (spec §7 scenario 1 ordering invariant).
    g_engine_ctx->live_trxs.remove(trx);
    trx->in_registry.store(false, std::memory_order_release);

    // NEW: drop the owner's refcount. If 0, free here. Otherwise some
    // remote thread (walker, kill_query) has a TrxHandle pinning it; the
    // last TrxHandle::~ will free.
    uint32_t prev = trx->refcount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 0) std::abort();  // RELEASE_ASSERT underflow
    if (prev == 1) {
        my_free(trx);
        return 0;
    }
    // prev > 1: someone else is pinning. Don't free; don't fall through
    // to the legacy rwlock-wrlock-then-my_free path because that would
    // double-free.
    return 0;
}
```

> NOTE: this **removes** the legacy `g_trx_lifecycle_lock` wrlock + `my_free` path. In strict strangler-fig order, that should land in PR 7. The reason it's necessary now: leaving both paths alive would double-free when prev == 1. The legacy path is removed in this Task; PR 7's job (removing `g_trx_lifecycle_lock` entirely) is reduced accordingly.

- [ ] **Step 2.3.3: Build + run MTR**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr -t tidesdb/mysql-mtr:pr2 ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:pr2 bash -lc \
  'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force'"
```

Expected: 61/61.

- [ ] **Step 2.3.4: Build the ASAN image + run MTR under it (this is the per-PR gate)**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan-pr2 ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:asan-pr2 bash -lc \
  'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force'"
```

Expected: 61/61, zero ASAN reports.

- [ ] **Step 2.3.5: Commit + push PR**

```bash
git add plugin/ha_tidesdb.cc
git commit -q -m "feat(txn-refactor PR2): close_connection removes from registry + drops refcount

Owner's tidesdb_close_connection now follows the refcount protocol:
clear thd's plugin slot, remove from live_trxs, drop the owner's
refcount, and free only if no remote thread is currently pinning the
trx via a TrxHandle. The legacy g_trx_lifecycle_lock wrlock + my_free
path is removed in this commit (originally scheduled for PR 7) because
keeping both paths alive would double-free when the refcount drop
reaches 0."

git push -u origin HEAD:refs/heads/refactor/pr2-close-connection-refcount
gh pr create --base main --head refactor/pr2-close-connection-refcount \
    --title "txn-refactor PR2: registry + refcount on trx lifecycle" \
    --body "Plan section PR 2."
```

---

## PR 3 — Migrate `tidesdb_hton_kill_query` to TrxHandle (closes CF-1)

**Goal:** `tidesdb_hton_kill_query` uses `TrxHandle::try_acquire` instead of reading `trx` from `thd_get_ha_data` directly. Adds the CF-1 regression test using `debug_sync`.

**Closes:** **CF-1**.

**Spec sections:** §5 row CF-1, §7 scenario 1, §9 test #1.

**Pre-conditions:** PR 2 merged.

### Task 3.1: Migrate kill_query

- [ ] **Step 3.1.1: Find the function**

```bash
grep -n "^.*tidesdb_hton_kill_query" plugin/ha_tidesdb.cc
```

- [ ] **Step 3.1.2: Replace body to use TrxHandle**

```cpp
static void tidesdb_hton_kill_query(handlerton *, THD *thd,
                                    enum thd_kill_levels) {
    using namespace tidesdb_mysql;
    TrxHandle h = TrxHandle::try_acquire(thd, g_engine_ctx);
    if (!h) return;  // close already in flight, or already killed -- nothing to do

    // CAS RUNNING -> KILL_PENDING. Idempotent; multiple killers are fine.
    KillState expected = KillState::RUNNING;
    h->kill_state.compare_exchange_strong(expected, KillState::KILL_PENDING,
                                          std::memory_order_acq_rel);

    // Wake the waiter on its row-lock cond, if any.
    tdb_row_lock_t *wait =
        h->waiting_on.load(std::memory_order_acquire);
    if (wait) {
        mysql_mutex_lock(&wait->part->mutex);
        mysql_cond_broadcast(&wait->cond);
        mysql_mutex_unlock(&wait->part->mutex);
    }
    // ~h drops the ref.
}
```

Delete the previous rdlock + raw read.

- [ ] **Step 3.1.3: Build + run MTR**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan-pr3 ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:asan-pr3 bash -lc \
  'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force'"
```

Expected: 61/61, no ASAN reports.

- [ ] **Step 3.1.4: Commit**

```bash
git add plugin/ha_tidesdb.cc
git commit -q -m "fix(CF-1): migrate tidesdb_hton_kill_query to TrxHandle::try_acquire

Closes CF-1. The raw thd_get_ha_data read is replaced by an atomic
acquire under the live-trx registry mutex; if the trx has been removed
from the registry (close in flight) or marked KILLED, try_acquire
returns null and kill_query is a no-op. No more raw-pointer deref of a
potentially-freed trx."
```

### Task 3.2: CF-1 regression test (debug_sync)

- [ ] **Step 3.2.1: Add a `debug_sync` point inside `tidesdb_close_connection`**

Just before `live_trxs.remove(trx)`, add:

```cpp
DEBUG_SYNC(thd, "tidesdb_close_before_remove");
```

(Match the existing `DEBUG_SYNC` style used elsewhere in `ha_tidesdb.cc`; if not yet used, the include is `#include "sql/debug_sync.h"`.)

- [ ] **Step 3.2.2: Write the regression test**

`mysql-test-suite/t/tidesdb_cf1_kill_query_race.test`:

```sql
--source include/have_debug_sync.inc
--source include/have_tidesdb.inc

CREATE DATABASE tidesdb_cf1;
USE tidesdb_cf1;
CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
INSERT INTO t VALUES (1, 'a');

# Session V: victim. Open it, set up the sync that close hits.
connect (v, localhost, root,,);
SET DEBUG_SYNC = 'tidesdb_close_before_remove SIGNAL victim_at_remove WAIT_FOR killer_done';

# Session K: killer.
connect (k, localhost, root,,);

# Trigger the victim's connection close from session V by issuing
# 'disconnect'. The close path will hit the sync point and wait for K.
connection v;
--send disconnect

# K sees the signal, then fires KILL QUERY against V before allowing V
# to proceed past the sync.
connection k;
SET DEBUG_SYNC = 'now WAIT_FOR victim_at_remove';
let $v_id = `SELECT CONNECTION_ID FROM information_schema.processlist WHERE USER = 'root' AND DB = 'tidesdb_cf1' LIMIT 1`;
eval KILL QUERY $v_id;
SET DEBUG_SYNC = 'now SIGNAL killer_done';

# Both threads finish. No UAF should be reported; under ASAN the run
# fails the suite if there's a heap-use-after-free.
connection default;
DROP DATABASE tidesdb_cf1;
SET DEBUG_SYNC = 'RESET';
```

Also create the `.result` file by running the test once (it will fail-by-result the first time; that's how MTR baselines).

- [ ] **Step 3.2.3: Run the regression test under ASAN**

```bash
sg docker -c "docker run --rm -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb \
  tidesdb/mysql-mtr:asan-pr3 bash -lc \
  'cd /build/mysql-server/build/mysql-test && \
   ./mtr tidesdb.tidesdb_cf1_kill_query_race --force --record'"
sg docker -c "docker run --rm tidesdb/mysql-mtr:asan-pr3 bash -lc \
  'cd /build/mysql-server/build/mysql-test && \
   ./mtr tidesdb.tidesdb_cf1_kill_query_race --force'"
```

Expected: the second run passes with no ASAN findings.

- [ ] **Step 3.2.4: Commit + push PR**

```bash
git add plugin/ha_tidesdb.cc mysql-test-suite/t/tidesdb_cf1_kill_query_race.test \
        mysql-test-suite/r/tidesdb_cf1_kill_query_race.result
git commit -q -m "test(CF-1): debug_sync regression test for the kill_query UAF

Pins the close-removes-from-registry vs killer-reads-trx interleaving
that triggered CF-1. Runs under ASAN in CI; any UAF in the kill_query
path fails the gate."

git push -u origin HEAD:refs/heads/refactor/pr3-cf1
gh pr create --base main --head refactor/pr3-cf1 \
    --title "txn-refactor PR3: close CF-1" \
    --body "Plan section PR 3. Closes CF-1."
```

---

## PR 4 — Migrate the deadlock walker + cond-wait loop (closes H-3)

**Goal:** `tdb_lock_would_deadlock` acquires a `TrxHandle` per visited graph node before any field deref. The `row_lock_acquire` cond-wait loop adds a `KillState` poll alongside the existing `thd_killed(thd)` and H-1 deadlock re-check.

**Closes:** **H-3**, kill-during-cond-wait (new).

**Spec sections:** §5 H-3, §7 scenario 2 + scenario 5, §9 tests #3 and #5.

**Pre-conditions:** PR 3 merged.

### Task 4.1: Migrate the walker

- [ ] **Step 4.1.1: Locate `tdb_lock_would_deadlock`**

```bash
grep -n "^bool tdb_lock_would_deadlock" plugin/tidesdb_row_lock.cc
```

- [ ] **Step 4.1.2: Replace the rwlock-rdlock body with per-node TrxHandle acquires**

Today the function takes `g_trx_lifecycle_lock` rdlock, walks the graph dereferencing trx fields, releases. New version:

```cpp
bool tdb_lock_would_deadlock(tidesdb_trx_t *requestor, tdb_row_lock_t *target_lock) {
    using namespace tidesdb_mysql;
    // We do NOT take a TrxHandle on `requestor` -- it's our own thread's
    // trx, alive by construction.
    // For each remote trx node we visit, acquire a TrxHandle first.

    // Existing graph-traversal scaffolding, but every "*node->trx" deref
    // is gated by a successful try_acquire.
    bool found_cycle = false;
    /* ... iterative traversal as before, but replace any
         tidesdb_trx_t *next = ...;
         if (next == requestor) { found_cycle = true; break; }
         examine next->waiting_on, etc.
       with:
         TrxHandle h = TrxHandle::try_acquire_from_raw(next, g_engine_ctx);
         if (!h) continue;     // dying node: not part of a real cycle
         if (h.get() == requestor) { found_cycle = true; break; }
         examine h->waiting_on, etc.
    */
    return found_cycle;
}
```

> Note: `try_acquire_from_raw(tidesdb_trx_t*, tdb_global_t*)` is a small additional acquire path that takes the raw trx pointer rather than reading from THD; useful for the walker which is iterating graph nodes. Add it to `TrxHandle` as a static method:
>
> ```cpp
> static TrxHandle try_acquire_from_raw(tidesdb_trx_t *trx, tdb_global_t *g) {
>     if (!trx || !g) return TrxHandle{};
>     if (!g->live_trxs.try_pin(trx)) return TrxHandle{};
>     return TrxHandle{trx};
> }
> ```
>
> Add a unit test for it in `trx_handle_test.cc`.

- [ ] **Step 4.1.3: Build + run MTR + ASAN**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan-pr4 ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:asan-pr4 bash -lc \
  'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force'"
```

Expected: 61/61 + zero ASAN.

- [ ] **Step 4.1.4: Commit**

```bash
git add plugin/tidesdb_row_lock.cc plugin/tidesdb_trx_handle.h plugin/tidesdb_trx_handle.cc \
        unittest/tidesdb/trx_handle_test.cc
git commit -q -m "fix(H-3): walker uses TrxHandle per graph node

Each remote graph node is gated by TrxHandle::try_acquire_from_raw
before any field deref. Dying nodes (acquire fails) are skipped --
spec §8 documents why they can't be part of a real cycle. The walker
no longer holds the legacy g_trx_lifecycle_lock; MF-4's lock-order
pair (part->mutex + g_trx_lifecycle_lock) is gone."
```

### Task 4.2: Add KillState poll to the cond-wait loop

- [ ] **Step 4.2.1: Locate `row_lock_acquire`'s cond-wait loop**

```bash
grep -n "row_lock_acquire" plugin/tidesdb_row_lock.cc
grep -n "mysql_cond_wait" plugin/tidesdb_row_lock.cc
```

- [ ] **Step 4.2.2: Add `KillState::KILL_PENDING` to the break-out predicates**

```cpp
while (lock->owner != trx) {
    if (thd && thd_killed(thd)) { killed = true; break; }
    if (trx->kill_state.load(std::memory_order_acquire)
        == KillState::KILL_PENDING) { killed = true; break; }
    // Re-publish waiting_on, re-run deadlock walker (H-1 fix), etc.
    if (tdb_lock_would_deadlock(trx, lock)) {
        deadlock_in_wait = true; break;
    }
    mysql_cond_wait(&lock->cond, &part->mutex);
}
```

- [ ] **Step 4.2.3: Build + run MTR + ASAN**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan-pr4 ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:asan-pr4 bash -lc \
  'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force'"
```

Expected: 61/61 + zero ASAN.

- [ ] **Step 4.2.4: Commit**

```bash
git add plugin/tidesdb_row_lock.cc
git commit -q -m "fix(H-1 hardening): cond-wait loop polls KillState alongside thd_killed

KillState::KILL_PENDING is a strict superset of thd_killed for the
KILL QUERY case; both checks are kept because thd_killed covers other
situations (server shutdown, statement timeout)."
```

### Task 4.3: H-3 regression test (debug_sync)

- [ ] **Step 4.3.1: Add debug_sync points to the walker and close path**

In `tdb_lock_would_deadlock`, just after a successful `try_acquire_from_raw`:

```cpp
DEBUG_SYNC(current_thd, "tidesdb_walker_after_pin");
```

(Already in close: `tidesdb_close_before_remove` from PR 3.)

- [ ] **Step 4.3.2: Write the test**

`mysql-test-suite/t/tidesdb_h3_walker_race.test`:

```sql
--source include/have_debug_sync.inc
--source include/have_tidesdb.inc

CREATE DATABASE tidesdb_h3;
USE tidesdb_h3;
CREATE TABLE t (id INT PRIMARY KEY, v INT) ENGINE=TIDESDB;
INSERT INTO t VALUES (1, 0), (2, 0);

# Set up a cycle: V locks (1) and waits on (2); W locks (2) and waits on (1).
connect (v, localhost, root,,);
connect (w, localhost, root,,);

connection v;
BEGIN; UPDATE t SET v=1 WHERE id=1;
connection w;
BEGIN; UPDATE t SET v=1 WHERE id=2;

# Pin the walker after it has acquired a TrxHandle on V's trx; meanwhile
# disconnect V which will trigger close_connection.
connection v;
SET DEBUG_SYNC = 'now WAIT_FOR walker_pinned_v';
--send UPDATE t SET v=2 WHERE id=2

connection w;
SET DEBUG_SYNC = 'tidesdb_walker_after_pin SIGNAL walker_pinned_v WAIT_FOR v_closed';
--send UPDATE t SET v=2 WHERE id=1

# Disconnect V from the default conn while the walker is paused.
connection default;
SET DEBUG_SYNC = 'now WAIT_FOR walker_pinned_v';
disconnect v;
SET DEBUG_SYNC = 'now SIGNAL v_closed';

# Reap. Walker should resume, see the cycle (or not, if V's trx is being
# torn down), and either way: no UAF.
connection w;
--reap

connection default;
DROP DATABASE tidesdb_h3;
SET DEBUG_SYNC = 'RESET';
```

- [ ] **Step 4.3.3: Run the test under ASAN, record baseline, run again**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan-pr4 ."
sg docker -c "docker run --rm -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb \
  tidesdb/mysql-mtr:asan-pr4 bash -lc \
  'cd /build/mysql-server/build/mysql-test && \
   ./mtr tidesdb.tidesdb_h3_walker_race --force --record && \
   ./mtr tidesdb.tidesdb_h3_walker_race --force'"
```

Expected: pass, no ASAN findings.

- [ ] **Step 4.3.4: Commit + push PR**

```bash
git add plugin/tidesdb_row_lock.cc \
        mysql-test-suite/t/tidesdb_h3_walker_race.test \
        mysql-test-suite/r/tidesdb_h3_walker_race.result \
        mysql-test-suite/t/tidesdb_kill_during_cond_wait.test \
        mysql-test-suite/r/tidesdb_kill_during_cond_wait.result
git commit -q -m "test(H-3 / kill-during-wait): walker race + cond-wait kill tests

Closes H-3. Both tests use debug_sync to pin the racy interleaving and
run under ASAN in CI."

git push -u origin HEAD:refs/heads/refactor/pr4-walker-and-cond-wait
gh pr create --base main --head refactor/pr4-walker-and-cond-wait \
    --title "txn-refactor PR4: walker → TrxHandle; cond-wait polls KillState" \
    --body "Plan section PR 4. Closes H-3."
```

(For brevity, the `tidesdb_kill_during_cond_wait.test` file follows the same pattern: open two sessions, victim waits on a held row lock, killer issues `KILL QUERY` on victim; verify victim returns `ER_QUERY_INTERRUPTED` and releases its part->mutex cleanly. Pattern is the same as the H-3 test minus the close-conn racing element.)

---

## PR 5 — Sysvar callbacks → pending changes (closes H-8)

**Goal:** Audit the `_update` callbacks, identify those that touch a non-caller session's trx, migrate them to push `PendingSysvarChange` records. Statement-begin hook drains and applies. CHANGELOG documents the behavior change.

**Closes:** **H-8**.

**Spec sections:** §5 H-8, §7 scenario 4, §9 MTR baseline.

**Pre-conditions:** PR 4 merged.

### Task 5.1: Audit the sysvar callbacks

- [ ] **Step 5.1.1: Enumerate every `_update` callback that touches `thd_get_ha_data`**

```bash
grep -n "static void.*_update.*THD" plugin/ha_tidesdb.cc | head -30
```

For each match, read its body. Mark any that:
- Reads `(tidesdb_trx_t*)thd_get_ha_data(target_thd, tidesdb_hton)` for a `target_thd != current_thd`, OR
- Writes a field on a `tidesdb_trx_t*` obtained from a THD other than the caller.

Document the audit result as a comment block at the top of `ha_tidesdb.cc`'s sysvar callbacks region, listing each callback as "SAFE" (no cross-session mutation) or "MIGRATED" (now uses pending changes).

- [ ] **Step 5.1.2: Populate `PendingSysvarChange::Kind`**

In `plugin/tidesdb_trx_handle.h`, expand the `Kind` enum based on the audit:

```cpp
enum class Kind : uint8_t {
    PROMOTE_PRIMARY,
    // ... whatever else the audit found ...
};
```

(If the audit finds exactly one callback, the enum has one variant. Document this in a comment.)

- [ ] **Step 5.1.3: Commit the audit + Kind**

```bash
git add plugin/tidesdb_trx_handle.h plugin/ha_tidesdb.cc
git commit -q -m "docs(txn-refactor PR5): audit sysvar callbacks for cross-session mutation

Result: the following _update callbacks reach across sessions and need
the pending-change model:
  - <list from audit>
PendingSysvarChange::Kind is populated to match."
```

### Task 5.2: Migrate each implicated callback

For each callback identified in Task 5.1.1:

- [ ] **Step 5.2.N: Replace the cross-session mutation with a pending-change push**

Concrete example (replace as appropriate per the audit):

```cpp
static void tidesdb_promote_primary_update(THD *thd, SYS_VAR *,
                                           void *var_ptr, const void *save) {
    using namespace tidesdb_mysql;
    bool new_value = *(const bool*)save;
    *(bool*)var_ptr = new_value;  // GLOBAL value still updates

    // For every other session, push a pending change. Iterate the
    // server's session list via the public THD enumeration API (the
    // codebase already uses Global_THD_manager elsewhere -- match
    // that pattern).
    Global_THD_manager::get_instance()->do_for_all_thd(
        [&](THD *target) {
            if (target == thd) return;  // caller already applied
            TrxHandle h = TrxHandle::try_acquire(target, g_engine_ctx);
            if (!h) return;  // target closing or no trx; new sessions
                             // will see the new GLOBAL value
            PendingSysvarChange pc{
                PendingSysvarChange::Kind::PROMOTE_PRIMARY,
                static_cast<uint64_t>(new_value),
            };
            if (!h->pending_changes.try_push(pc)) {
                sql_print_warning("[TIDESDB] sysvar change dropped for "
                                  "session %u; target busy",
                                  target->thread_id());
            }
        });
}
```

### Task 5.3: Add the statement-begin drain hook

- [ ] **Step 5.3.1: Find the start_stmt / statement-begin handler**

```bash
grep -n "::start_stmt\|external_lock" plugin/ha_tidesdb.cc | head
```

- [ ] **Step 5.3.2: Drain pending changes at the entry**

```cpp
int ha_tidesdb::start_stmt(THD *thd, thr_lock_type lt) {
    using namespace tidesdb_mysql;
    tidesdb_trx_t *trx = (tidesdb_trx_t*)thd_get_ha_data(thd, tidesdb_hton);
    if (trx) {
        PendingSysvarChange c;
        while (trx->pending_changes.try_pop(&c)) {
            apply_pending_change(trx, c);
        }
    }
    return /* existing return */;
}

static void apply_pending_change(tidesdb_trx_t *trx,
                                 const PendingSysvarChange &c) {
    using namespace tidesdb_mysql;
    switch (c.kind) {
        case PendingSysvarChange::Kind::PROMOTE_PRIMARY:
            // existing per-trx state update for promote_primary
            break;
        // other kinds as audited
    }
}
```

### Task 5.4: H-8 regression test + MTR re-baseline

- [ ] **Step 5.4.1: Write the test**

`mysql-test-suite/t/tidesdb_h8_sysvar_pending.test`:

```sql
--source include/have_debug_sync.inc
--source include/have_tidesdb.inc

CREATE DATABASE tidesdb_h8;
USE tidesdb_h8;
CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TIDESDB;
INSERT INTO t VALUES (1);

# Session A: start a long-running transaction.
connect (a, localhost, root,,);
BEGIN;
SELECT * FROM t WHERE id = 1;

# Capture A's view of the implicated sysvar via an internal status var
# (added in a small helper for testability):
let $a_view_before = `SELECT @@SESSION.tidesdb_promote_primary`;

# Session B fires SET GLOBAL.
connect (b, localhost, root,,);
SET GLOBAL tidesdb_promote_primary = ON;

# A's current statement sees the OLD value (pending change is queued
# but not yet applied).
connection a;
let $a_view_during = `SELECT @@SESSION.tidesdb_promote_primary`;
--assert($a_view_during == $a_view_before)

COMMIT;

# A's NEXT statement sees the new value (start_stmt drains pending_changes).
BEGIN;
let $a_view_after = `SELECT @@SESSION.tidesdb_promote_primary`;
--assert($a_view_after == 'ON')
COMMIT;

connection default;
SET GLOBAL tidesdb_promote_primary = OFF;
DROP DATABASE tidesdb_h8;
```

- [ ] **Step 5.4.2: Re-baseline any existing MTR tests broken by the semantic change**

```bash
sg docker -c "docker run --rm tidesdb/mysql-mtr:asan-pr5 bash -lc \
  'cd /build/mysql-server/build/mysql-test && \
   ./mtr --suite=tidesdb --force --max-test-fail=99' 2>&1 | tee /tmp/mtr.log"
grep -E '\[ fail \]' /tmp/mtr.log
```

For each failed test caused by the H-8 semantic change (likely tests that assert immediate cross-session sysvar effect):
- Re-record with `./mtr <test> --record`.
- Add a comment header in the `.test` file: `# Re-baselined for v0.4.0 H-8 fix: SET GLOBAL for this sysvar no longer mutates in-flight transactions on other sessions; applies at their next statement.`

- [ ] **Step 5.4.3: Commit + push PR**

```bash
git add plugin/ha_tidesdb.cc plugin/tidesdb_trx_handle.h \
        mysql-test-suite/t/tidesdb_h8_sysvar_pending.test \
        mysql-test-suite/r/tidesdb_h8_sysvar_pending.result \
        mysql-test-suite/t/<any re-baselined tests>
git commit -q -m "fix(H-8): sysvar update callbacks push PendingSysvarChange

Closes H-8. Cross-session sysvar mutation is replaced with a
PendingChangeQueue push per target trx; the target applies queued
changes at the start of its next statement. SET GLOBAL no longer
mutates in-flight transactions on other sessions -- this is a v0.4.0
behavior change, documented in CHANGELOG (PR 10) and the test files
re-baselined here."

git push -u origin HEAD:refs/heads/refactor/pr5-h8
gh pr create --base main --head refactor/pr5-h8 \
    --title "txn-refactor PR5: H-8 sysvar pending changes" \
    --body "Plan section PR 5. Closes H-8. Includes MTR re-baselines."
```

---

## PR 6 — Shutdown drain barrier (closes C-2)

**Goal:** `tdb_global` shutdown moves through `RUNNING → DRAINING → DRAINED → STOPPED`. Plugin uninstall path waits on `shutdown_cv` for `ref_count == 0` with a configurable timeout. New sysvar `tidesdb_uninstall_drain_timeout_s` (default 30).

**Closes:** **C-2**.

**Spec sections:** §5 C-2, §7 scenario 3, §8 "Shutdown blocked on a stuck connection," §9 tests #2 and #6.

**Pre-conditions:** PR 5 merged.

### Task 6.1: Wire all background workers through GlobalRef

- [ ] **Step 6.1.1: Find background workers / sysvar callbacks that touch tdb_global state**

```bash
grep -nE "g_engine_ctx->|->live_trxs\." plugin/*.cc | head -40
```

For each access:
- If the access is from the main connection thread (handler methods), the trx's existence already implies tdb_global is alive — no GlobalRef needed.
- If the access is from a background thread (compaction sweeper, periodic tasks) or a sysvar callback, gate the body with `GlobalRef::try_acquire`.

Example:

```cpp
static void some_background_worker() {
    using namespace tidesdb_mysql;
    GlobalRef g = GlobalRef::try_acquire(g_engine_ctx);
    if (!g) return;  // plugin uninstalling; finish silently
    // ... existing work using g-> ...
}
```

- [ ] **Step 6.1.2: Build + commit**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan-pr6 ."
git add plugin/ha_tidesdb.cc plugin/tidesdb_*.cc
git commit -q -m "feat(txn-refactor PR6): gate background workers + sysvar callbacks behind GlobalRef"
```

### Task 6.2: Implement the shutdown drain in plugin_deinit

- [ ] **Step 6.2.1: Locate the plugin deinit / uninstall function**

```bash
grep -n "tidesdb_done\|plugin_deinit\|tidesdb_hton.*deinit" plugin/ha_tidesdb.cc
```

- [ ] **Step 6.2.2: Add the state-machine drain**

```cpp
static int tidesdb_done(void *) {
    using namespace tidesdb_mysql;
    // Transition to DRAINING; refuse new GlobalRef acquires.
    g_engine_ctx->shutdown_state.store(ShutdownState::DRAINING,
                                       std::memory_order_seq_cst);

    // Wake everything that might be in cond_wait.
    // (For each row_lock partition, broadcast its cond.)
    /* iterate partitions; broadcast */

    auto timeout = std::chrono::seconds(srv_uninstall_drain_timeout_s);
    auto deadline = std::chrono::steady_clock::now() + timeout;

    mysql_mutex_lock(&g_engine_ctx->shutdown_mutex);
    while (g_engine_ctx->ref_count.load(std::memory_order_seq_cst) != 0) {
        struct timespec abstime;
        // convert deadline to abstime (existing helpers in ha_tidesdb.cc)
        int rc = mysql_cond_timedwait(&g_engine_ctx->shutdown_cv,
                                      &g_engine_ctx->shutdown_mutex,
                                      &abstime);
        if (rc == ETIMEDOUT) {
            mysql_mutex_unlock(&g_engine_ctx->shutdown_mutex);
            sql_print_warning("[TIDESDB] plugin uninstall: drain timed out "
                              "after %lds with %u refs outstanding",
                              (long)srv_uninstall_drain_timeout_s,
                              g_engine_ctx->ref_count.load());
            return 1;  // plugin busy
        }
        // Log slow-drain every 5s.
        if (std::chrono::steady_clock::now() > deadline - timeout / 2) {
            sql_print_warning("[TIDESDB] plugin uninstall: still draining, "
                              "%u refs outstanding",
                              g_engine_ctx->ref_count.load());
        }
    }
    mysql_mutex_unlock(&g_engine_ctx->shutdown_mutex);

    // Drain barrier passed. Tear down the rest.
    g_engine_ctx->shutdown_state.store(ShutdownState::DRAINED,
                                       std::memory_order_seq_cst);
    /* existing teardown -- close CFs, stop background threads */
    g_engine_ctx->shutdown_state.store(ShutdownState::STOPPED,
                                       std::memory_order_seq_cst);
    tdb_global_t *to_free = g_engine_ctx;
    g_engine_ctx = nullptr;
    delete to_free;
    return 0;
}
```

- [ ] **Step 6.2.3: Add the sysvar**

```cpp
static MYSQL_SYSVAR_UINT(uninstall_drain_timeout_s,
    srv_uninstall_drain_timeout_s,
    PLUGIN_VAR_RQCMDARG,
    "Seconds to wait for in-flight transactions and background work to "
    "drain before reporting the plugin as busy at UNINSTALL PLUGIN.",
    nullptr, nullptr, 30, 1, 3600, 1);
```

Register in the sysvar array.

- [ ] **Step 6.2.4: Build + MTR + commit**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan-pr6 ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:asan-pr6 bash -lc \
  'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force'"
git add plugin/ha_tidesdb.cc plugin/ha_tidesdb.h
git commit -q -m "fix(C-2): plugin_deinit drains via shutdown_cv before freeing tdb_global

Closes C-2. State machine RUNNING → DRAINING → DRAINED → STOPPED.
Refuses new GlobalRef acquires after DRAINING; waits for ref_count to
hit 0 (with the new tidesdb_uninstall_drain_timeout_s sysvar, default
30s). After drain, frees tdb_global; no worker can deref a freed
pointer because every worker has already dropped its GlobalRef."
```

### Task 6.3: C-2 + shutdown-blocked regression tests

- [ ] **Step 6.3.1: Write the C-2 test**

`mysql-test-suite/t/tidesdb_c2_shutdown_race.test`:

```sql
--source include/have_debug_sync.inc
--source include/have_tidesdb.inc

CREATE DATABASE tidesdb_c2;
USE tidesdb_c2;
CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TIDESDB;

# Trigger a background compaction concurrently with UNINSTALL.
INSERT INTO t VALUES (1),(2),(3),(4),(5);
SELECT SLEEP(0.1);  # let bg work start

# Sync: hold the worker just after it gets a GlobalRef, then issue
# UNINSTALL. Verify UNINSTALL waits for the worker, no null-deref.
SET DEBUG_SYNC = 'tidesdb_bg_after_globalref SIGNAL worker_pinned WAIT_FOR shutdown_started';

connect (u, localhost, root,,);
SET DEBUG_SYNC = 'now WAIT_FOR worker_pinned';
SET DEBUG_SYNC = 'tidesdb_done_before_wait SIGNAL shutdown_started';
UNINSTALL PLUGIN tidesdb;
INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so';

connection default;
DROP DATABASE tidesdb_c2;
SET DEBUG_SYNC = 'RESET';
```

- [ ] **Step 6.3.2: Write the shutdown-blocked test**

`mysql-test-suite/t/tidesdb_shutdown_blocked_session.test`:

```sql
--source include/have_tidesdb.inc

CREATE DATABASE tidesdb_sb;
USE tidesdb_sb;
CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TIDESDB;

# Set a very small drain timeout so the test runs fast.
SET GLOBAL tidesdb_uninstall_drain_timeout_s = 2;

# Open a long-running session that won't terminate during the test.
connect (long, localhost, root,,);
BEGIN; INSERT INTO t VALUES (1);

# UNINSTALL should time out and return an error.
connection default;
--error ER_PLUGIN_DELETE_BUILTIN,ER_UNKNOWN_ERROR
UNINSTALL PLUGIN tidesdb;

# Disconnect the long session; UNINSTALL should now succeed.
disconnect long;
UNINSTALL PLUGIN tidesdb;
INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so';

connection default;
SET GLOBAL tidesdb_uninstall_drain_timeout_s = 30;
DROP DATABASE tidesdb_sb;
```

- [ ] **Step 6.3.3: Record + run + commit + push PR**

```bash
sg docker -c "docker run --rm -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb \
  tidesdb/mysql-mtr:asan-pr6 bash -lc \
  'cd /build/mysql-server/build/mysql-test && \
   ./mtr tidesdb.tidesdb_c2_shutdown_race --force --record && \
   ./mtr tidesdb.tidesdb_shutdown_blocked_session --force --record && \
   ./mtr tidesdb.tidesdb_c2_shutdown_race tidesdb.tidesdb_shutdown_blocked_session --force'"

git add mysql-test-suite/t/tidesdb_c2_shutdown_race.test \
        mysql-test-suite/r/tidesdb_c2_shutdown_race.result \
        mysql-test-suite/t/tidesdb_shutdown_blocked_session.test \
        mysql-test-suite/r/tidesdb_shutdown_blocked_session.result
git commit -q -m "test(C-2): debug_sync regression for shutdown null-deref + timeout"

git push -u origin HEAD:refs/heads/refactor/pr6-c2-shutdown
gh pr create --base main --head refactor/pr6-c2-shutdown \
    --title "txn-refactor PR6: shutdown drain barrier (C-2)" \
    --body "Plan section PR 6. Closes C-2. Adds tidesdb_uninstall_drain_timeout_s sysvar."
```

---

## PR 7 — Remove `g_trx_lifecycle_lock`

**Goal:** Delete the rwlock + every `mysql_rwlock_rdlock`/`mysql_rwlock_unlock` call against it. Build asserts no remaining references.

**Closes:** **MF-4** (structurally; the lock pair literally no longer exists).

**Spec sections:** §5 MF-4, §10 PR 7.

**Pre-conditions:** PR 6 merged. All five migrations (PR 3, 4, 5, 6) green in CI.

### Task 7.1: Delete the rwlock

- [ ] **Step 7.1.1: Find every reference**

```bash
grep -nE "g_trx_lifecycle_lock" plugin/*.cc plugin/*.h
```

- [ ] **Step 7.1.2: Delete each occurrence**

For each match:
- Delete the line if it's just `rdlock`/`unlock` paired with a now-unused-block.
- Delete the declaration in `ha_tidesdb.cc` or `ha_tidesdb.h`.
- Delete the `mysql_rwlock_init` / `mysql_rwlock_destroy` in plugin init/deinit.

After all deletions:

```bash
grep -nE "g_trx_lifecycle_lock" plugin/*.cc plugin/*.h
# Expected: no output
```

- [ ] **Step 7.1.3: Build + MTR + ASAN**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan-pr7 ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:asan-pr7 bash -lc \
  'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force'"
```

Expected: 61/61 + all 6 regression tests from PRs 3-6 pass + zero ASAN findings.

- [ ] **Step 7.1.4: Commit + push PR**

```bash
git add plugin/
git commit -q -m "refactor(MF-4): remove g_trx_lifecycle_lock entirely

Closes MF-4. Every call site that used to take this rwlock was migrated
to TrxHandle/GlobalRef in PRs 3-6. The rwlock and its init/destroy are
deleted; build asserts no remaining references.

The MF-4 lock-order pair (part->mutex then g_trx_lifecycle_lock rdlock)
no longer exists; the only remaining lock order in the txn subsystem is
part->mutex → live_trxs registry mutex (drop part->mutex before any
cross-thread try_acquire is the rule, asserted in PR 8)."

git push -u origin HEAD:refs/heads/refactor/pr7-delete-rwlock
gh pr create --base main --head refactor/pr7-delete-rwlock \
    --title "txn-refactor PR7: delete g_trx_lifecycle_lock (MF-4)" \
    --body "Plan section PR 7. Closes MF-4."
```

---

## PR 8 — Lock-order debug assert

**Goal:** Per-thread held-lock-rank tracker; debug builds assert on inversion.

**Closes:** hardening (regression-recurrence prevention).

**Spec sections:** §7 "Lock-order discipline," §10 PR 8.

**Pre-conditions:** PR 7 merged.

### Task 8.1: Add the lock-rank tracker

- [ ] **Step 8.1.1: Create the header**

`plugin/tidesdb_lock_rank.h`:

```cpp
// SPDX-License-Identifier: GPL-2.0
//
// Per-thread held-lock-rank tracker. Debug-only. Asserts that any lock
// acquisition respects the canonical rank order; an inversion would be
// a deadlock waiting to happen.
//
// Ranks (lower number = acquired first):
//   1: part->mutex
//   2: live_trxs registry mutex
//   3: shutdown_mutex
//
// Usage: replace the raw mysql_mutex_lock/unlock at each tracked site
// with the LOCK_RANK_LOCK / LOCK_RANK_UNLOCK macros below.

#pragma once

#ifdef DBUG_OFF
#define LOCK_RANK_LOCK(rank, m)    mysql_mutex_lock(m)
#define LOCK_RANK_UNLOCK(rank, m)  mysql_mutex_unlock(m)
#else

#include <vector>

namespace tidesdb_mysql {

enum class LockRank : uint8_t {
    PART_MUTEX     = 1,
    REGISTRY_MUTEX = 2,
    SHUTDOWN_MUTEX = 3,
};

void debug_lock_acquired(LockRank rank);
void debug_lock_released(LockRank rank);

}  // namespace tidesdb_mysql

#define LOCK_RANK_LOCK(rank, m) do {                              \
    tidesdb_mysql::debug_lock_acquired(rank);                     \
    mysql_mutex_lock(m);                                           \
} while (0)
#define LOCK_RANK_UNLOCK(rank, m) do {                            \
    mysql_mutex_unlock(m);                                         \
    tidesdb_mysql::debug_lock_released(rank);                     \
} while (0)
#endif
```

- [ ] **Step 8.1.2: Implement in .cc**

In `plugin/tidesdb_trx_handle.cc`:

```cpp
#ifndef DBUG_OFF
namespace tidesdb_mysql {

thread_local std::vector<LockRank> g_held_ranks;

void debug_lock_acquired(LockRank rank) {
    if (!g_held_ranks.empty()
        && static_cast<uint8_t>(g_held_ranks.back())
           >= static_cast<uint8_t>(rank)) {
        // Inversion: about to acquire a lock with rank <= our current
        // top. Abort with a clear message.
        fprintf(stderr,
                "[TIDESDB] LOCK-ORDER INVERSION: about to acquire rank %d "
                "while holding rank %d\n",
                (int)rank, (int)g_held_ranks.back());
        std::abort();
    }
    g_held_ranks.push_back(rank);
}

void debug_lock_released(LockRank rank) {
    if (g_held_ranks.empty() || g_held_ranks.back() != rank) {
        fprintf(stderr,
                "[TIDESDB] LOCK release out of order: releasing %d but "
                "top is %d\n",
                (int)rank,
                g_held_ranks.empty() ? -1 : (int)g_held_ranks.back());
        std::abort();
    }
    g_held_ranks.pop_back();
}

}  // namespace tidesdb_mysql
#endif
```

### Task 8.2: Wire the tracked sites

- [ ] **Step 8.2.1: Replace raw `mysql_mutex_lock(&part->mutex)` with `LOCK_RANK_LOCK(LockRank::PART_MUTEX, &part->mutex)` at every site**

```bash
grep -n "part->mutex" plugin/*.cc
```

For each match, replace `mysql_mutex_lock(&part->mutex)` → `LOCK_RANK_LOCK(LockRank::PART_MUTEX, &part->mutex)` and similarly for unlock.

- [ ] **Step 8.2.2: Same for the registry mutex inside `LiveTrxRegistry::Impl`**

In `plugin/tidesdb_trx_handle.cc`'s registry methods, wrap with `LOCK_RANK_LOCK(LockRank::REGISTRY_MUTEX, ...)`.

- [ ] **Step 8.2.3: Same for `shutdown_mutex`**

### Task 8.3: Build + run

- [ ] **Step 8.3.1: Build debug + run MTR; any inversion will abort**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.asan -t tidesdb/mysql-mtr:asan-pr8 ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:asan-pr8 bash -lc \
  'cd /build/mysql-server/build/mysql-test && ./mtr --suite=tidesdb --force'"
```

Expected: 61/61, no abort. Any pre-existing lock-order issue surfaces here and is fixed in this PR.

- [ ] **Step 8.3.2: Commit + push PR**

```bash
git add plugin/tidesdb_lock_rank.h plugin/tidesdb_trx_handle.cc \
        plugin/tidesdb_row_lock.cc plugin/ha_tidesdb.cc
git commit -q -m "feat(txn-refactor PR8): debug lock-rank tracker

Per-thread held-lock-rank vector; LOCK_RANK_LOCK/UNLOCK macros assert
on inversion in DBUG (-DCMAKE_BUILD_TYPE=Debug) builds. No-op in
release. Canonical order: part->mutex (1) → registry (2) → shutdown (3)."

git push -u origin HEAD:refs/heads/refactor/pr8-lock-rank
gh pr create --base main --head refactor/pr8-lock-rank \
    --title "txn-refactor PR8: debug lock-order assert" \
    --body "Plan section PR 8. Hardening."
```

---

## PR 9 — TSAN nightly CI + stress test + release gate

**Goal:** Add TSAN-flavoured build, nightly workflow that runs MTR + unit tests + 30-min `tidesdb_killfuzz` stress under TSAN. Failures auto-open an issue. Release workflow gains a "two consecutive nightly greens" check.

**Closes:** enforcement.

**Spec sections:** §9 testing strategy, §10 PR 9.

**Pre-conditions:** PR 8 merged.

### Task 9.1: TSAN Dockerfile

- [ ] **Step 9.1.1: Create `docker/Dockerfile.mtr.tsan`**

Same shape as `Dockerfile.mtr.asan` but with `-fsanitize=thread` instead of `-fsanitize=address,undefined`.

- [ ] **Step 9.1.2: Build + run unit tests under TSAN**

```bash
sg docker -c "docker build -f docker/Dockerfile.mtr.tsan -t tidesdb/mysql-mtr:tsan ."
sg docker -c "docker run --rm tidesdb/mysql-mtr:tsan bash -lc \
  'cd /build && ctest --output-on-failure -L tidesdb_unit'"
```

Expected: pass. Any data race found is either a real bug (fix it) or a known-safe pattern (add to `tsan-suppressions.txt`).

- [ ] **Step 9.1.3: Create `mysql-test-suite/tsan-suppressions.txt`**

```
# TSAN suppressions for the tidesdb plugin's MTR runs.
# Each entry MUST have a one-line comment naming why it's not a real
# finding. CI fails if any uncommented entry is present.

# Example (delete once real suppressions are added during stress runs):
# race:libcrypto.so      # OpenSSL's internal locking; not our concern.
```

### Task 9.2: 30-min killfuzz stress test

- [ ] **Step 9.2.1: Write the test**

`mysql-test-suite/t/tidesdb_killfuzz.test`:

```sql
--source include/have_tidesdb.inc

# 30-minute fuzz: many concurrent TPC-C-style transactions; a parallel
# KILL fuzzer; periodic UNINSTALL/INSTALL cycles. Runs nightly under
# TSAN; failure = data race or UAF discovered.

CREATE DATABASE killfuzz;
USE killfuzz;
CREATE TABLE accounts (id INT PRIMARY KEY, bal INT) ENGINE=TIDESDB;
INSERT INTO accounts VALUES (1, 1000), (2, 1000), (3, 1000), (4, 1000);

# Spawn 8 worker sessions, each doing 30 min of random transfers.
# Worker logic lives in mysql-test-suite/include/tidesdb_killfuzz_worker.inc
let $i = 1;
while ($i <= 8) {
    eval connect (worker_$i, localhost, root,,);
    eval connection worker_$i;
    --send_eval source ../include/tidesdb_killfuzz_worker.inc;
    inc $i;
}

# Spawn a KILL fuzzer.
connect (killer, localhost, root,,);
--send source ../include/tidesdb_killfuzz_killer.inc

# Spawn a plugin uninstall/install cycler.
connect (cycler, localhost, root,,);
--send source ../include/tidesdb_killfuzz_cycler.inc

# Reap.
let $i = 1;
while ($i <= 8) {
    eval connection worker_$i;
    --reap
    inc $i;
}
connection killer; --reap
connection cycler; --reap

connection default;
DROP DATABASE killfuzz;
```

Helper scripts (skeletons; populate with realistic loops):

```sql
# mysql-test-suite/include/tidesdb_killfuzz_worker.inc
let $start = `SELECT UNIX_TIMESTAMP()`;
let $now = $start;
while ($now < $start + 1800) {
    let $a = `SELECT 1 + FLOOR(RAND() * 4)`;
    let $b = `SELECT 1 + FLOOR(RAND() * 4)`;
    eval BEGIN;
    eval UPDATE accounts SET bal = bal - 1 WHERE id = $a;
    eval UPDATE accounts SET bal = bal + 1 WHERE id = $b;
    eval COMMIT;
    let $now = `SELECT UNIX_TIMESTAMP()`;
}
```

```sql
# mysql-test-suite/include/tidesdb_killfuzz_killer.inc
let $start = `SELECT UNIX_TIMESTAMP()`;
let $now = $start;
while ($now < $start + 1800) {
    let $victim = `SELECT id FROM information_schema.processlist
                   WHERE user = 'root' AND db = 'killfuzz'
                   ORDER BY RAND() LIMIT 1`;
    eval KILL QUERY $victim;
    SELECT SLEEP(0.05);
    let $now = `SELECT UNIX_TIMESTAMP()`;
}
```

```sql
# mysql-test-suite/include/tidesdb_killfuzz_cycler.inc
let $start = `SELECT UNIX_TIMESTAMP()`;
let $now = $start;
while ($now < $start + 1800) {
    SELECT SLEEP(60);
    --error 0, ER_PLUGIN_DELETE_BUILTIN
    UNINSTALL PLUGIN tidesdb;
    --error 0, ER_UDF_EXISTS
    INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so';
    let $now = `SELECT UNIX_TIMESTAMP()`;
}
```

### Task 9.3: Sanitizers workflow expansion (TSAN nightly)

- [ ] **Step 9.3.1: Add a TSAN job to `.github/workflows/sanitizers.yml`**

```yaml
  tsan-nightly:
    if: github.event_name == 'schedule' || github.event_name == 'workflow_dispatch'
    runs-on: [self-hosted, linux, x64, tidesdb-release]
    timeout-minutes: 240
    steps:
      - uses: actions/checkout@v4
      - name: Build TSAN MTR image
        run: docker build -f docker/Dockerfile.mtr.tsan -t tidesdb/mysql-mtr:tsan .
      - name: Validate TSAN suppressions have comments
        run: |
          awk '/^[^#]/ && !/^$/ { has_code=1; prev_was_code=1; next }
               /^#/ { prev_was_code=0; next }
               END { exit (has_code && !prev_was_code) }' \
            mysql-test-suite/tsan-suppressions.txt \
            || { echo "::error::tsan-suppressions.txt has an uncommented entry"; exit 1; }
      - name: Run unit tests + full MTR + killfuzz under TSAN
        run: |
          docker run --rm \
            -e TSAN_OPTIONS="suppressions=/build/mysql-server/mysql-test/suite/tidesdb/tsan-suppressions.txt:halt_on_error=1" \
            tidesdb/mysql-mtr:tsan bash -lc \
            'cd /build && ctest --output-on-failure -L tidesdb_unit && \
             cd /build/mysql-server/build/mysql-test && \
             ./mtr --suite=tidesdb --force --max-test-fail=0 && \
             ./mtr tidesdb.tidesdb_killfuzz --force'
      - name: On failure, open an issue
        if: failure()
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          gh issue create \
            --title "TSAN nightly failure on $(date -u +%Y-%m-%d)" \
            --label regression \
            --body "TSAN nightly failed. Run: ${{ github.server_url }}/${{ github.repository }}/actions/runs/${{ github.run_id }}"
```

Add a schedule trigger to the top of the workflow:

```yaml
on:
  pull_request:
    branches: [main]
  workflow_dispatch:
  schedule:
    - cron: '0 5 * * *'   # nightly @ 05:00 UTC
```

### Task 9.4: Release "two consecutive greens" gate

- [ ] **Step 9.4.1: Add a step in `.github/workflows/release.yml` that checks the last two nightly runs**

Before the script-call step, insert:

```yaml
      - name: Verify two consecutive TSAN nightly greens
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          # Get the last 2 conclusions of the Sanitizers nightly tsan-nightly job.
          conclusions=$(gh run list \
            --workflow=sanitizers.yml --branch=main --limit=2 \
            --json conclusion --jq '.[].conclusion' | sort -u)
          if [ "$conclusions" != "success" ]; then
            echo "::error::TSAN nightly is not green for the last 2 runs ($conclusions). Aborting release."
            exit 1
          fi
```

- [ ] **Step 9.4.2: Build, commit, push PR**

```bash
git add docker/Dockerfile.mtr.tsan \
        .github/workflows/sanitizers.yml \
        .github/workflows/release.yml \
        mysql-test-suite/tsan-suppressions.txt \
        mysql-test-suite/t/tidesdb_killfuzz.test \
        mysql-test-suite/r/tidesdb_killfuzz.result \
        mysql-test-suite/include/tidesdb_killfuzz_*.inc
git commit -q -m "ci(txn-refactor PR9): TSAN nightly + killfuzz stress + release gate

TSAN nightly runs full MTR + the 30-min killfuzz stress under
-fsanitize=thread; failures auto-open a regression-labelled issue.
Release workflow now refuses to push the tag unless the last 2 TSAN
nightlies are green."

git push -u origin HEAD:refs/heads/refactor/pr9-tsan-ci
gh pr create --base main --head refactor/pr9-tsan-ci \
    --title "txn-refactor PR9: TSAN nightly + stress + release gate" \
    --body "Plan section PR 9. Enforcement."
```

---

## PR 10 — Docs + KNOWN-ISSUES cleanup + v0.4.0 CHANGELOG entry

**Goal:** Remove closed findings from `KNOWN-ISSUES.md`. Add a CHANGELOG entry for v0.4.0 documenting the H-8 behavior change as a breaking change. Link the spec from `docs/`.

**Closes:** none — pure docs.

**Spec sections:** §10 PR 10.

**Pre-conditions:** PRs 1–9 merged.

### Task 10.1: Update KNOWN-ISSUES.md

- [ ] **Step 10.1.1: Remove the closed findings from the "Current open" section**

Edit `KNOWN-ISSUES.md`:

- Delete entries for CF-1, C-2, H-3, H-8, MF-4.
- Add a `## Verified fixed in v0.4.0` section listing them with one-line reasons:

```markdown
## Verified fixed in v0.4.0

The transaction-lifecycle refactor (see
`docs/superpowers/specs/2026-06-02-txn-lifecycle-refactor-design.md`)
closed the following findings:

- **CF-1** (CRITICAL regression) — `tidesdb_hton_kill_query` now uses
  `TrxHandle::try_acquire` instead of reading `trx` directly from
  `thd_get_ha_data`.
- **C-2** (CRITICAL) — `tdb_global` teardown drains via a
  `shutdown_cv` barrier before freeing; workers never deref a freed
  global.
- **H-1** (HIGH) — already-fixed cond-wait deadlock re-check verified
  and codified as a canonical safe-point pattern.
- **H-3** (HIGH) — deadlock walker grabs a `TrxHandle` on each visited
  node before deref; UAF structurally impossible.
- **H-8** (HIGH) — sysvar `_update` callbacks push
  `PendingSysvarChange` records instead of mutating other sessions'
  trx state. **Behavior change**: `SET GLOBAL` for the implicated
  sysvars no longer affects in-flight transactions on other sessions;
  applies at their next statement.
- **MF-4** (MEDIUM) — `g_trx_lifecycle_lock` removed; the lock-order
  pair MF-4 worried about no longer exists.
```

### Task 10.2: Update CHANGELOG.md

- [ ] **Step 10.2.1: Add the v0.4.0 entry**

```markdown
## v0.4.0 — Transaction-lifecycle refactor (closes CF-1, C-2, H-3, H-8, MF-4)

### Breaking changes

- **`SET GLOBAL` for `<implicated sysvars from PR 5 audit>` now applies
  to other sessions at their next statement, not in the middle of
  in-flight transactions.** This replaces a previously racy behavior
  (H-8) where the cross-session mutation could tear-read or be lost.
  If you have automation that relied on the immediate-mutation
  semantics, switch to per-session `SET SESSION` or wait for the target
  session's next statement.

### New sysvars

- `tidesdb_uninstall_drain_timeout_s` (default 30, range 1–3600) —
  seconds to wait for in-flight transactions and background work to
  drain at `UNINSTALL PLUGIN` before reporting the plugin as busy.

### Changes

- Refcounted `tidesdb_trx_t` via the new internal `TrxHandle` /
  `GlobalRef` types; replaces `g_trx_lifecycle_lock`.
- `KillState` flag polled at all defined safe points (cond-wait wakes,
  statement begin).
- TSAN nightly CI + 30-min `tidesdb_killfuzz` stress test added.
- Release workflow refuses to push the v0.4.0 tag unless the last 2
  TSAN nightlies are green.

### Bug fixes

See KNOWN-ISSUES.md "Verified fixed in v0.4.0."
```

### Task 10.3: Link the spec from `docs/`

- [ ] **Step 10.3.1: Add a one-line link to `docs/README.md` or equivalent**

(Whatever index file the project uses for the `docs/` directory; if none, skip.)

### Task 10.4: Push PR 10

- [ ] **Step 10.4.1: Commit + push**

```bash
git add KNOWN-ISSUES.md CHANGELOG.md docs/
git commit -q -m "docs(txn-refactor PR10): CHANGELOG + KNOWN-ISSUES for v0.4.0

v0.4.0 ships the txn-lifecycle refactor. CHANGELOG documents the H-8
behavior change as breaking; KNOWN-ISSUES moves CF-1, C-2, H-1, H-3,
H-8, MF-4 to the 'verified fixed' section."

git push -u origin HEAD:refs/heads/refactor/pr10-docs
gh pr create --base main --head refactor/pr10-docs \
    --title "txn-refactor PR10: docs + CHANGELOG + KNOWN-ISSUES" \
    --body "Plan section PR 10. Final piece of the v0.4.0 sequence."
```

---

## Self-review notes

- **Spec coverage:** every finding listed in §1 of the spec maps to a PR (CF-1 → PR 3, C-2 → PR 6, H-1 → PR 4 task 4.2, H-3 → PR 4 task 4.1, H-8 → PR 5, MF-4 → PR 7). Every component listed in §6 is created in PR 1. Testing strategy from §9 maps to: ASAN per-PR in PR 1 Task 1.12; debug_sync regression tests in PRs 3, 4, 5, 6; unit tests in PR 1 Tasks 1.7–1.11; stress + TSAN nightly + release gate in PR 9; docs in PR 10. Sequencing from §10 followed.
- **Placeholders scanned:** the only "audit-determined" placeholder is the contents of `PendingSysvarChange::Kind`, which is correct — the spec explicitly defers it to the PR 5 audit. The `g_engine_ctx` symbol name is used throughout as a stand-in for whatever the actual global symbol is; PR 1 Task 1.4 instructs the implementer to confirm it via `grep` rather than assuming. The MTR `.test` files include realistic SQL where possible; baseline `.result` files are generated by `mtr --record` rather than hand-written.
- **Type consistency:** `TrxHandle::try_acquire(THD*, tdb_global_t*)`, `GlobalRef::try_acquire(tdb_global_t*)`, `LiveTrxRegistry::try_pin(tidesdb_trx_t*)`, `KillState::RUNNING/KILL_PENDING/KILLED`, `ShutdownState::RUNNING/DRAINING/DRAINED/STOPPED`, `PendingSysvarChange::Kind`, `PendingChangeQueue::try_push/try_pop` are used consistently across tasks.
- **One quirk worth flagging:** PR 2 Task 2.3 explicitly **removes the legacy `g_trx_lifecycle_lock` wrlock + my_free path from `close_connection`**, which spec §10's strict reading attributed to PR 7. Reasoning is in the commit message: keeping both paths alive in PR 2 would double-free when the refcount drop reaches 0. PR 7 still has work to do (delete the rwlock declaration itself, every rdlock site that PRs 3–6 left in place during their migrations, and the init/destroy calls), so PR 7's scope is narrowed but not eliminated.

---

Plan complete and saved to `docs/superpowers/plans/2026-06-02-txn-lifecycle-refactor.md`. Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per Task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
