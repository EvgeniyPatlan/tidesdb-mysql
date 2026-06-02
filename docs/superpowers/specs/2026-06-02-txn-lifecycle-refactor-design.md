# Transaction-lifecycle refactor — design spec

| | |
|---|---|
| **Author**  | brainstorm session, 2026-06-02 |
| **Status**  | Design — awaiting implementation plan |
| **Target release** | v0.4.0 |
| **Hotfix in parallel** | v0.3.2 cherry-pick (CF-1 only, minimal rdlock wrap) |

## 1. Summary

A structural refactor of the plugin's transaction-object lifetime model and
deadlock-detector concurrency, closing five open CRITICAL/HIGH findings plus
the related MEDIUM lock-order finding (MF-4). Replaces the
`g_trx_lifecycle_lock` rwlock with an intrusive refcount on `tidesdb_trx_t`
(via a `TrxHandle` RAII type), introduces a structured KILL flag polled at
defined safe points, and adds a `tdb_global` shutdown drain barrier
(`GlobalRef`). After the refactor, the lock pair that MF-4 worries about
literally no longer exists.

### Findings closed

| ID | Severity | Source |
|---|---|---|
| **CF-1** | CRITICAL (regression from H-3) | `code-review-followup-report.md` |
| **C-2**  | CRITICAL | `code-review-report.md` |
| **H-1**  | HIGH (already fixed; covered by tests + verification only) | `code-review-report.md` |
| **H-3**  | HIGH | `code-review-report.md` |
| **H-8**  | HIGH | `code-review-report.md` |
| **MF-4** | MEDIUM | `code-review-followup-report.md` |

### Out of scope (decided explicitly during brainstorm)

- **HF-4** (KILL-aware backup/checkpoint): separate subsystem; will build on
  the `KillState` + `GlobalRef` infrastructure landed here.
- **The other 8 HIGH findings** (H-2, H-4, H-5, H-6, H-7, H-9, H-10, HF-1..HF-3):
  unrelated cohesion; tracked separately.
- **Refactoring the rest of `ha_tidesdb.cc`'s 8 688 lines**: separate work
  ("split the elephant").
- **Public sysvar / handler-API changes**: the refactor is invisible outside
  `plugin/`.

## 2. Context — what's broken today

### CF-1: kill_query UAF on `trx->waiting_on`

`code-review-followup-report.md`. The H-3 fix wrapped
`tdb_lock_would_deadlock` with `g_trx_lifecycle_lock` read-lock and
`tidesdb_close_connection`'s `my_free(trx)` with the write-lock.
**`tidesdb_hton_kill_query` was missed.** Concurrent `KILL QUERY <victim>`
while the victim's connection is closing: the kill thread reads `trx` via
`thd_get_ha_data`, the close thread enters the rwlock write section and
frees `trx`, the kill thread dereferences `trx->waiting_on`. Trivially
exploitable on any workload that combines KILL QUERY with TidesDB.

### C-2: `tdb_global` shutdown race null-deref

`code-review-report.md`. The plugin uninstall / process exit path tears
down `tdb_global` while background workers and sysvar callbacks may still
be running; they observe a null `tdb_global` and crash.

### H-1: deadlock detector cond-wait wakeup

`code-review-report.md`. Original finding: cond-wait wakes do not re-poll
the deadlock walker; victim could hang after a topology change. **Already
fixed** in `tidesdb_row_lock.cc` (the cond-wait loop re-runs
`tdb_lock_would_deadlock` on each wake; the comment names H-1). The
refactor verifies the fix remains intact and codifies the re-check as a
canonical safe-point pattern.

### H-3: UAF in deadlock-graph walker

`code-review-report.md`. Walker dereferences `trx` fields after dropping
`part->mutex`; another thread can close the connection and free the trx in
between. Partial fix shipped (the `g_trx_lifecycle_lock` rdlock); CF-1 is
the regression that proved the wrapper-discipline approach insufficient.

### H-8: sysvar callback mutates another connection's trx

`code-review-report.md`. A `_update` callback runs in the caller's THD
context but reaches into a target session's transaction state, racing
against that session's own statement execution. Torn reads / lost updates.

### MF-4: lock-order fragility

`code-review-followup-report.md`. The walker holds `part->mutex` then
takes `g_trx_lifecycle_lock` read-lock. The pair is the only safe order
today but is not documented or enforced. Any new code path that takes the
locks in the inverse order would deadlock.

## 3. Goals and non-goals

### Goals

1. Close the six findings above structurally — not via additional
   wrapper-discipline rules that can be forgotten at a new call site.
2. Make the next finding of this class **mechanically detectable** by CI
   (TSAN nightly + ASAN per-PR + debug lock-rank assertions).
3. Keep the public sysvar set, handler API, and on-disk format unchanged.
4. Ship a minimal-diff cherry-pick of CF-1 to the v0.3.x line in parallel
   with the refactor work so users on the stable line are not exposed
   during the months the refactor takes.

### Non-goals

1. Improving throughput. The throughput bottleneck on MySQL 9.7 is above
   the plugin layer (per `frontend-perf-investigation.md`, Phase 0);
   reducing the deadlock walker's lock cost is not a win we can observe.
2. Touching anything in `ha_tidesdb.cc` outside the lifetime / KILL / shutdown
   call sites.
3. Changing the deadlock detection algorithm itself.

## 4. Approach

**Approach A** (chosen during brainstorm): Refcount + KILL flag + drain
barrier. Three interlocking primitives:

1. **Intrusive refcount on `tidesdb_trx_t`** via a `TrxHandle` RAII wrapper.
   Every cross-thread reference holds a strong ref; the freer can't free
   what a remote thread has pinned.
2. **`KillState` atomic flag on `trx_t`** polled at defined safe points
   (cond-wait wakes, statement begin). `kill_query` sets `KILL_PENDING`;
   victim observes it and unwinds cleanly via existing
   `HA_ERR_QUERY_INTERRUPTED` paths.
3. **`tdb_global` shutdown state machine** with a refcount on the global
   itself; uninstall transitions `RUNNING → DRAINING → DRAINED → STOPPED`
   and waits for the global refcount to hit zero before tearing down.

Alternatives considered and rejected:

- **Approach B** — arena-allocated `trx_t` with explicit per-session
  lifetimes: structurally sound but rewrites every `trx_t` alloc/free site;
  outsized for the bug class being addressed.
- **Approach C** — hazard pointers / EBR for the walker: highest concurrency
  ceiling, but complex and over-engineered given the workload is bottlenecked
  above us anyway.

## 5. Architecture and per-finding mapping

The three primitives map onto the findings as follows. Three of the five
findings close as a direct structural consequence of the refcount + drain
barrier; H-8 closes via the pending-record model (no cross-thread mutation
possible); H-1 stays as it is, verified, with the design making the
re-check pattern canonical; MF-4 disappears with the lock it was about.

| Finding | What goes wrong today | Fix under this model |
|---|---|---|
| **CF-1** | `tidesdb_hton_kill_query` reads `trx` from `thd_get_ha_data` without holding the lifecycle lock; the close thread can free in between | `kill_query` opens `TrxHandle::try_acquire(thd)`; if null (close already in flight), returns silently; otherwise sets `KillState::KILL_PENDING` and broadcasts. Handle drops on scope exit. |
| **C-2**  | Workers / sysvar callbacks may run with `tdb_global == nullptr` mid-teardown | All `tdb_global` access goes through `GlobalRef::try_acquire`; teardown waits until the counter is zero before assigning null. |
| **H-1**  | (already fixed: cond-wait loop re-runs the walker) | Verified intact and documented in the model; cond-wait wake-up is a canonical safe point that re-checks both the walker AND `KillState`. |
| **H-3**  | Walker drops `part->mutex` then reads `trx` fields — race with close | Walker grabs `TrxHandle` for the target before dropping `part->mutex`; closes can't free a pinned trx. |
| **H-8**  | `_update` callback runs in caller's THD but writes into a target session's transaction state | Callback writes a `pending_sysvar_change` record on the target via `TrxHandle::try_acquire`, no in-place mutation; target applies at next statement-begin. |
| **MF-4** | `part->mutex` then `g_trx_lifecycle_lock` rdlock — pair not enforced | `g_trx_lifecycle_lock` ceases to exist. Only ordering rule left: "drop `part->mutex` before any cross-thread `TrxHandle::try_acquire`" — asserted in debug builds. |

## 6. Components and APIs

### New types

```cpp
// Intrusive refcount holder for tidesdb_trx_t. RAII; move-only.
class TrxHandle {
public:
    // Atomic acquire from a THD's plugin slot. Returns null-handle if:
    //  - thd_get_ha_data returned null;
    //  - the trx was already removed from the live-trx registry;
    //  - the trx's KillState is KILLED.
    static TrxHandle try_acquire(THD *thd);
    explicit operator bool() const noexcept;
    tidesdb_trx_t* operator->() const noexcept;   // debug-asserts non-null
    ~TrxHandle();                                  // dec refcount; free if 0
};

// Atomic kill state on each trx. Polled at every defined safe point.
enum class KillState : uint8_t {
    RUNNING      = 0,
    KILL_PENDING = 1,
    KILLED       = 2
};

// Per-session pending sysvar change, applied at next statement-begin.
struct PendingSysvarChange {
    enum Kind : uint8_t { /* enumerated when sysvar callbacks are audited */ } kind;
    uint64_t value;
};

// RAII handle on tdb_global. Null if shutdown_state != RUNNING.
class GlobalRef {
public:
    static GlobalRef try_acquire();
    explicit operator bool() const noexcept;
    tdb_global_t* operator->() const noexcept;
    ~GlobalRef();
};

// tdb_global shutdown state.
enum class ShutdownState : uint8_t { RUNNING, DRAINING, DRAINED, STOPPED };
```

### Additions to existing structs

```cpp
struct tidesdb_trx_t {                              // existing fields...
    std::atomic<uint32_t> refcount{1};              // + owner holds 1
    std::atomic<KillState> kill_state{KillState::RUNNING};
    std::atomic<bool> in_registry{true};            // flipped under registry lock
    mpsc_queue<PendingSysvarChange> pending_changes;
};

struct tdb_global_t {                               // existing fields...
    std::atomic<ShutdownState> shutdown_state{ShutdownState::RUNNING};
    std::atomic<uint32_t> ref_count{0};             // outstanding GlobalRef holders
    mysql_cond_t shutdown_cv;
    LiveTrxRegistry live_trxs;
};
```

### The live-trx registry

A set of `tidesdb_trx_t*` consulted by `try_acquire` so a refcount is never
bumped on a freed pointer.

- **Add** under registry-lock from the trx-creation path (first
  `external_lock` of the session).
- **Remove** under registry-lock from `tidesdb_close_connection` **before**
  dropping the owner's refcount.
- **Lookup** under registry-lock from `TrxHandle::try_acquire`: read
  `trx = thd_get_ha_data(thd)`; if non-null and present, atomically
  `fetch_add(1)` on `refcount`, return handle; else null.

**Starting representation**: `mysql_mutex_t + std::unordered_set<tidesdb_trx_t*>`.
The registry is not on a hot path (touched on connection start/close and on
`kill_query`); the lock is held just long enough for a hash lookup and a
`fetch_add`. A lock-free hash set is implementable behind the same interface
later if profiling demands it; the brainstorm concluded YAGNI for now.

### What `g_trx_lifecycle_lock` becomes

Deleted (PR 7). `tdb_lock_would_deadlock` and `tidesdb_hton_kill_query` use
`TrxHandle`. `tidesdb_close_connection` uses the registry-remove +
refcount-drop pattern. No lock-order pair to enforce.

### Call sites changed (rough count)

- `tidesdb_close_connection` (~10 lines): null `thd_get_ha_data`, remove
  from registry, drop refcount; free is delayed until the last ref drops.
- `tidesdb_hton_kill_query` (CF-1 site, ~5 lines): `try_acquire`, set kill
  state, broadcast.
- `tdb_lock_would_deadlock` (walker, ~15 lines): `try_acquire` on each node
  before any field read; skip nodes whose acquire fails.
- `row_lock_acquire` cond-wait loop (~10 lines): poll `KillState` alongside
  `thd_killed`; break out cleanly on either.
- Three implicated sysvar `_update` callbacks (~10 lines each): push
  `PendingSysvarChange` via `TrxHandle::try_acquire`. The exact set is
  enumerated during PR 5 by auditing the `_update` callbacks; expected
  candidates include `tidesdb_promote_primary_update` and any sysvar whose
  update path reaches `thd_get_ha_data` for a target other than
  `current_thd`.
- Statement-begin hook (~5 lines): drain `pending_changes`, apply, clear.
- Plugin uninstall path (~30 lines, mostly new): the shutdown state machine.

Total estimate: **~1 500 LOC of plugin diff** + **~600 LOC of new tests**
+ **~200 LOC of CI**.

## 7. Data flow under the racy scenarios

### Lock-order discipline

Three locks survive:

1. `part->mutex` (per-partition row-lock mutex, existing)
2. `LiveTrxRegistry` mutex (new, leaf)
3. `shutdown_mutex` (new, leaf — paired with `shutdown_cv`)

**Canonical order: 1 → 2 → 3.** Only the walker holds two at once
(`part->mutex` then registry mutex briefly, drops registry mutex before
dereferencing). No path crosses the order. Debug builds assert it via a
per-thread lock-rank check (~100 LOC, header-only).

### Scenario 1 — CF-1: KILL QUERY races connection close

A's read of `trx_raw` from `thd_get_ha_data` may be stale, but the
**registry lookup under the mutex is the source of truth**: if the trx has
been removed, A's lookup fails and returns a null handle. **No deref ever
happens through a raw pointer that's been freed**, because the registry
lookup happens before any field access. Ordering invariant: the close path
*must* remove from registry **before** the final refcount-drop that may
free.

### Scenario 2 — H-3: walker traverses N while N's owner closes

Walker gets the strong ref under registry-lock *before* it dereferences any
field. Owner removes from registry then drops its ref, but the walker's
ref keeps the struct alive. The walker's ref drops last, the free happens
last, no UAF. **MF-4 is solved here**: the old
`g_trx_lifecycle_lock` rdlock-while-holding-`part->mutex` pair is gone.

### Scenario 3 — C-2: plugin uninstall while background work is mid-flight

Two-phase drain: refuse new entries (`GlobalRef::try_acquire` returns null
after `DRAINING`), then wait for already-acquired refs to drop. **No worker
ever runs against a freed `tdb_global`**, because `tdb_global = nullptr`
happens after `STOPPED`, which happens after every `GlobalRef` has been
dropped.

### Scenario 4 — H-8: SET GLOBAL update mid-statement on another session

Caller acquires `TrxHandle::try_acquire(target_thd)`. If null, target is
already closing; the change is dropped (the GLOBAL value still applies to
new sessions). Otherwise the caller atomically pushes a
`PendingSysvarChange` onto target's queue and drops the handle.

Target picks up changes at the next `handler::start_stmt` safe point, drains
the queue, and applies.

**Semantic change**: `SET GLOBAL` for the implicated sysvars no longer
takes effect on in-flight transactions on other sessions; it takes effect
on each session's *next* statement. Documented as a breaking change in
v0.4.0.

### Scenario 5 — KILL QUERY while victim is in `mysql_cond_wait`

Killer (holding a `TrxHandle`) sets `KillState::KILL_PENDING` and
broadcasts on the *specific* row-lock cond from `victim->waiting_on`
(safe to read because the killer's handle holds a ref). Victim's existing
cond-wait loop re-checks predicates on every wake; adds
`kill_state == KILL_PENDING` to its break conditions alongside
`thd_killed(thd)`. **Both signals are kept** — `thd_killed` covers
non-KILL-QUERY situations (server shutdown, statement timeout).

H-1's deadlock re-check stays exactly where it is in the same loop.

## 8. Error handling

### `TrxHandle::try_acquire` returns null

| Caller | Why null happens | Response |
|---|---|---|
| `kill_query` (CF-1 site) | victim's trx already removed from registry | return silently |
| deadlock-graph walker (H-3) | target node being torn down | skip node; a dying node can only be holding locks about to be released, so any wait-edge through it will resolve on its own |
| sysvar `_update` callback (H-8) | target session is closing | drop the pending change silently; GLOBAL value still applies to new sessions |
| statement-begin pending-changes drain | n/a — own-thread, never null | — |

### `GlobalRef::try_acquire` returns null

Means `shutdown_state != RUNNING`. Callers abort cleanly. The shutdown
race is handled by atomic ordering: acquire is `fetch_add(1, seq_cst)`
then `load(seq_cst)` of `shutdown_state`; if observes `DRAINING`,
decrements and returns null. Symmetrically, shutdown does
`store(DRAINING, seq_cst)` then waits for `ref_count == 0`. The seq_cst
total-order guarantees no deadlock.

### Shutdown blocked on a stuck connection

`shutdown_cv.wait_for(timeout)` expiry → `plugin_deinit` returns the
"plugin busy" error code MySQL expects. The connection's trx still exists;
it tears down when the session eventually disconnects. **No leak, no
UAF**, just deferred completion.

Timeout sysvar: default 30 s (matches `innodb_fast_shutdown=0` patience).
Below the timeout, WARN-log the offending session count every few seconds.

### `KillState` transitions

CAS from `kill_query` is `compare_exchange_strong(RUNNING, KILL_PENDING)`.
Three idempotent outcomes:

1. Was `RUNNING` → now `KILL_PENDING`; broadcast.
2. Was `KILL_PENDING` → another killer already fired; broadcast anyway
   (cheap, safe).
3. Was `KILLED` → victim already noticed; return.

Victim is the only writer of `KILLED`, only during own teardown after
observing `KILL_PENDING`. No race.

### Pending-sysvar-change queue overflow

Bounded MPSC queue, capacity 16 per session. On overflow: `try_push`
returns false; caller logs
`[WARN] sysvar change dropped for session %u; target busy` and returns
success to `SET GLOBAL` (the GLOBAL value still updates). Debug builds
assert on persistent overflow (>3 drops in a row for the same session).

### Refcount under-/over-flow

- **Underflow** (`fetch_sub` on a zero refcount): unpaired release.
  `RELEASE_ASSERT` aborts in both debug and release — corruption is worse
  than crash. (Same call InnoDB makes.)
- **Overflow** is structurally impossible at our concurrency. Debug
  asserts upper bound 1024.

Refcount manipulation is **only via the RAII `TrxHandle` / `GlobalRef`**.
No manual `fetch_add` / `fetch_sub` outside the handle implementation.
CI grep-check rule enforces.

### Error propagation to MySQL handler errors

| Condition | Handler error |
|---|---|
| Victim observes `kill_state == KILL_PENDING` in cond-wait | `HA_ERR_QUERY_INTERRUPTED` |
| Walker fails to acquire on every node (degenerate) | "no deadlock"; requestor's wait continues |
| Sysvar callback can't acquire target trx | no error; `SET GLOBAL` succeeds |
| Shutdown timeout exceeded | plugin-busy code |

## 9. Testing strategy

### Sanitizer build matrix

| Build | Flags | Runs |
|---|---|---|
| ASAN+UBSAN | `-DCMAKE_BUILD_TYPE=Debug -fsanitize=address,undefined -fno-omit-frame-pointer` | every PR |
| TSAN      | `-DCMAKE_BUILD_TYPE=Debug -fsanitize=thread -fno-omit-frame-pointer` | nightly |

Both run full MTR suite + new tests below. PR gate runs ASAN; TSAN is
nightly (slower, noisier). Known non-bug TSAN reports get explicit
suppressions in `mysql-test-suite/tsan-suppressions.txt`, **each with a
one-line comment** citing the reason; CI fails if any suppression lacks a
comment.

### Per-finding regression tests using `debug_sync`

| Test | Race pinned | Assertion |
|---|---|---|
| `tidesdb_cf1_kill_query_race.test` | freer's registry-remove ↔ killer's `thd_get_ha_data` read | no ASAN/TSAN finding; killer observes null-handle |
| `tidesdb_c2_shutdown_race.test` | UNINSTALL PLUGIN ↔ background CF compaction worker | shutdown blocks until worker drains; no null-deref |
| `tidesdb_h3_walker_race.test` | walker just-acquired `TrxHandle` ↔ target's `close_connection` | walker's handle keeps target alive; no UAF |
| `tidesdb_h8_sysvar_pending.test` | `SET GLOBAL <var>` from B ↔ session A mid-statement | A's current statement completes with old value; A's next statement sees the change |
| `tidesdb_kill_during_cond_wait.test` | KILL QUERY ↔ victim in `mysql_cond_wait` | victim wakes; returns `HA_ERR_QUERY_INTERRUPTED`; no leaked lock |
| `tidesdb_shutdown_blocked_session.test` | UNINSTALL ↔ long-running session that refuses to terminate | shutdown returns plugin-busy after timeout; disconnect lets uninstall succeed |

Plus an **H-1 regression guard** — the existing H-1 test gains an explicit
comment tying it to H-1 and a TSAN run.

### Unit tests (gtest-style, `unittest/tidesdb/`)

- `TrxHandleTest`: acquire/release; failure modes; move-only.
- `LiveTrxRegistryTest`: N threads concurrently add/remove/lookup.
- `GlobalRefTest`: seq-cst acquire/shutdown interleavings; verify shutdown
  completes iff all acquires drop.
- `PendingChangeQueueTest`: capacity bound; overflow drops with WARN;
  FIFO ordering.
- `KillStateTest`: CAS idempotency under concurrent killers.

### Stress test

`tidesdb_killfuzz.test`, ~30 minutes: many concurrent connections doing
TPC-C-style transactions; parallel KILL-QUERY/KILL-CONNECTION fuzzer hits
random session IDs at random intervals; periodic `UNINSTALL PLUGIN` /
`INSTALL PLUGIN` cycles. Runs nightly under TSAN. Any new TSAN finding
fails the nightly and auto-opens an issue with label `regression`.

### CI gating policy

| Gate | What runs | When | Failure |
|---|---|---|---|
| PR | ASAN+UBSAN build + full MTR + unit tests + 6 deterministic regression tests | every push | merge blocked |
| Nightly | TSAN build + same suite + 30-min stress | nightly | auto-issue with `regression` label |
| Pre-release | both sanitizers + stress + four existing release gates | on `release.yml` | release bails before tag push |

**For the v0.4.0 release that ships this refactor, all three gates must be
green for two consecutive nights before tag push.**

### MTR baseline updates

The H-8 semantic change likely invalidates one or two existing MTR tests
that assert immediate cross-session behavior on the implicated sysvars
(probably `tidesdb_promote_primary` and similar). PR 5 audits, re-baselines,
and documents the diff explicitly.

## 10. Sequencing and rollout

Strangler-fig: new code lives alongside `g_trx_lifecycle_lock` until every
call site has migrated, then the rwlock is removed in a single cleanup PR.

### PR sequence

| PR | Scope | Loc | What lands | Findings closed |
|---|---|---|---|---|
| **1** | Foundation | ~400 | `tidesdb_trx_handle.{h,cc}` with all new types + unit tests. ASAN+UBSAN CI build added (unit tests run under it). | none — laying tracks |
| **2** | Registry + refcount on `trx_t` | ~200 | New fields on `trx_t`; `close_connection` adds registry-remove + refcount-drop *alongside* existing rwlock. Behavior identical to today. | none |
| **3** | Migrate `kill_query` → `TrxHandle` | ~50 + 1 test | CF-1 site uses `try_acquire` + `KillState`. **CF-1 regression test goes green.** | **CF-1** |
| **4** | Migrate walker → `TrxHandle` | ~100 + 1 test | Walker uses `try_acquire` per visited node. **H-3 test green.** Cond-wait loop adds `KillState` poll. | **H-3**, kill-during-cond-wait |
| **5** | Sysvar callbacks → pending changes | ~150 + 1 test + MTR re-baselines | Implicated `_update` callbacks switch to `try_acquire` + `pending_changes.push`. Statement-begin drains. CHANGELOG breaking-change entry. | **H-8** |
| **6** | Shutdown drain barrier | ~200 + 1 test | `tdb_global` gains the shutdown state machine. `GlobalRef::try_acquire` gates new entries. Uninstall path waits on `shutdown_cv`. Shutdown-timeout sysvar. | **C-2** |
| **7** | Remove `g_trx_lifecycle_lock` | ~50 (deletions) | rwlock and surrounding `rdlock`/`wrlock` calls gone. Build asserts no remaining references. | **MF-4** |
| **8** | Lock-order debug assert | ~100 | Per-thread held-lock-rank tracker; debug asserts on inversion. Re-runs MTR to surface any pre-existing inversions. | hardening |
| **9** | CI / TSAN nightly | ~150 of YAML + driver | TSAN build added; nightly job + stress test; auto-issue on failure; pre-release "two-greens" gate added to `release.yml`. | enforcement |
| **10** | Docs + KNOWN-ISSUES | ~300 prose | KNOWN-ISSUES cleaned of CF-1, C-2, H-3, H-8, MF-4. CHANGELOG v0.4.0 documents the H-8 behavior change. Spec link added. | — |

### Behavior compatibility per PR

PRs 1–6 land **without changing observable behavior** (except H-8's
intentional semantic change in PR 5). After PR 6, every CRIT/HIGH finding
in scope is structurally fixed — PR 7 is pure cleanup, PR 8 is debug-only,
PR 9 is CI plumbing. **The cluster is functionally complete after PR 6**;
PRs 7–10 lower regression-recurrence risk further.

### Branching and version target

- This ships as **v0.4.0** — not a patch bump (new internal symbol surface,
  H-8 behavior change, new shutdown-timeout sysvar).
- Each PR merges to `main`.
- **The v0.3.x line stays in maintenance** — we cherry-pick CF-1's
  minimal-diff fix (rdlock wrap in `kill_query`) onto v0.3.x as a
  **v0.3.2 hotfix** in parallel with the refactor, so users on the stable
  line are not exposed to CF-1 for the months of refactor work.

## 11. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Refactor takes longer than v0.3.x can tolerate CF-1 in the wild | v0.3.2 hotfix (rdlock wrap) ships first, in parallel. |
| TSAN suppressions accumulate untriaged | Every suppression must have a one-line comment with the upstream commit/issue. CI fails on uncommented suppressions. |
| The "PR 6 is functionally complete" claim is wrong | Each PR's regression test is the proof. If PR 6's CI is green on both sanitizers, the finding is closed. |
| H-8 semantic change breaks downstream | Documented in CHANGELOG under "Breaking changes (v0.4.0)" with migration note. The change is from broken-and-racy to deterministic, so users should be net better. |
| A new call site is added later that bypasses `TrxHandle` | CI grep-check rule: no manual `fetch_add` / `fetch_sub` on `trx_t::refcount` outside the handle implementation. |

## 12. Open questions

None at the design level — all addressed during the brainstorm. Items
deliberately deferred to the implementation plan:

- The exact set of sysvar `_update` callbacks implicated by H-8 (enumerated
  during PR 5 audit; expected candidates listed above).
- The starting representation of `LiveTrxRegistry` (mutex-guarded
  `std::unordered_set` until profiling indicates otherwise).
- The shutdown-timeout sysvar's name and default (proposed:
  `tidesdb_uninstall_drain_timeout_s`, default 30).
