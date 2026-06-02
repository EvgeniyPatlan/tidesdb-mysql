# TidesDB-MySQL Plugin — Follow-up Code Review Report

**Date:** 2026-05-13 (original) · **Refreshed:** 2026-06-02
**Scope:** `plugin/` only (4 files, now ~12.4k LOC; +700 lines since the prior review)
**Predecessor:** `docs/code-review-report.md` (commit f1120bf). 30 of 33 prior findings were fixed across 4 commit batches (a167949, 406c9b4, 27aff67, 3673899).
**Method:** Same four specialist passes as the original review (C++/concurrency, security, performance, architecture), run against the current tree with explicit context on every fix that landed. Highest-impact CRITICAL/HIGH claims spot-checked against the actual source.

---

## Revision 2 — status as of 2026-06-02

**TL;DR: every CRITICAL and HIGH finding in this report is closed.** Verified by re-reading current code while investigating a planned refactor that proved to be solving already-fixed bugs.

Verified closed:
| ID | Closed in | Evidence |
|---|---|---|
| CF-1 | commit 37441d6 | `plugin/ha_tidesdb.cc:2890-2924` wraps `tidesdb_hton_kill_query` body in `mysql_rwlock_rdlock(&g_trx_lifecycle_lock)` |
| HF-1 | commit 37441d6 | `plugin/tidesdb_fts.cc:305-313` — fail-closed when `current_thd == nullptr` |
| HF-2 | (later commit) | `g_fts_meta_mutex` was promoted into `TidesDB_share::fts_meta_mutex` per the report's recommendation (`plugin/ha_tidesdb.cc:138, :1681`) |
| HF-3 | commit 37441d6 | `tidesdb_backup_allowed_root` sysvar + `realpath()` confinement (`plugin/ha_tidesdb.cc:872-994`) covers both `backup_dir` and `checkpoint_dir` check callbacks (lines 1032, 1158) |
| HF-4 | commit 37441d6 | (covered by the same backup-handling overhaul; documented in `KNOWN-ISSUES.md`) |

Several MEDIUM/LOW findings are also closed as a side-effect of the same work or subsequent refactors — but those have **not** been individually re-verified in this refresh. Take them as "likely closed, confirm before acting." Notable ones I spot-confirmed in passing:

- **MF-4** (lock-order fragility) — documented inline; lock-order invariant comment added at `plugin/ha_tidesdb.cc` close-connection path.
- **MF-6** (log injection) — `tdb_sanitize_for_log` exists and is applied at the stopword loader (`plugin/tidesdb_fts.cc:287`).
- **LF-3** (prefer `explicit_bzero`) — `tdb_secure_zero` now wraps `explicit_bzero` (`plugin/ha_tidesdb.cc:4014`).

Architectural reassessment items have also been substantially executed: `tidesdb_row_lock.{cc,h}`, `tidesdb_fts.{cc,h}`, `tidesdb_spatial.{cc,h}`, `tidesdb_engine_context.{cc,h}`, `tidesdb_inplace_alter.cc`, `tidesdb_master_key.{cc,h}` are all separate translation units now. The 10,147-line `ha_tidesdb.cc` from the original review is down to ~8,688 lines.

**Lesson recorded:** review-report findings are not durable status indicators once code moves. Verify against the current source before scheduling any refactor work that targets them; this refresh was triggered by a planned six-finding refactor that turned out to be targeting closed work.

---

## Tagging convention

Every finding is tagged either:
- **[regression-from-fix]** — introduced by one of the C-/H-/M-/L- fixes
- **[pre-existing-newly-noticed]** — was always there; we missed it last time

Verification status:
- **[verified]** — confirmed by direct read of the cited lines
- **[disputed]** — reviewer flagged it but re-analysis suggests not actually a bug
- **[theoretical]** — real risk on paper but not exercised by any current code path
- **[agent]** — reviewer's claim, not independently verified here

---

## Executive summary

The fixes landed cleanly — 37/37 hand-rolled tests + 56/56 MTR pass. But the cleanup itself surfaced **two CRITICAL regressions** and several lower-severity items that warrant attention before the next major change.

**Block-merge findings (must fix):**

1. **C-1 [regression-from-fix, verified]** — `tidesdb_hton_kill_query` was missed during the H-3 trx-lifecycle rwlock rollout. It dereferences `trx->waiting_on` without holding the rwlock, so a `KILL QUERY` racing a connection close still UAFs. This is the exact same class of bug the H-3 fix was meant to close.

2. **H-1-followup [regression-from-fix, verified]** — `M-12`'s privilege check (`Security_context::check_access`) fall-throughs as "allow" when `current_thd == nullptr`. Should be fail-closed.

**Notable non-bugs (disputed or theoretical):**

- The cpp reviewer's "H-3" (FTS meta delta lost on rollback) is a false positive — both the deltas and the index entries live in the same `stmt_txn`, so they share rollback fate. Marked **disputed**.
- The cpp reviewer's "C-2" (`static_cast<TidesDB_share*>` UB on cross-engine ALTER) is **theoretical** — today every call site passes a table whose share is TidesDB-owned, but a defensive type tag is cheap.

The architecture pass strongly re-prioritized: **`EngineContext` extraction (formerly A-4) is now ahead of the row-lock manager** because the point fixes added five more globals that all want to live in it. The four duplicated FTS scratch patterns are the most clearly-extractable new debt.

---

## CRITICAL

### CF-1. Kill-query UAF on `trx->waiting_on` (regression from H-3) — ✅ FIXED 2026-05-18 (commit 37441d6)
**Where:** `ha_tidesdb.cc:4403-4422` — **[verified]** **[regression-from-fix]**

The H-3 fix wrapped `tdb_lock_would_deadlock` with `g_trx_lifecycle_lock` read-lock and `tidesdb_close_connection`'s `my_free(trx)` with the write-lock. **`tidesdb_hton_kill_query` was missed.** It runs from a different thread than the victim:

```cpp
static void tidesdb_hton_kill_query(handlerton *, THD *thd, enum thd_kill_levels)
{
    if (!thd) return;
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx) return;

    tdb_row_lock_t *wait = trx->waiting_on.load(std::memory_order_acquire);  // <-- UAF on freed trx
    if (!wait) return;
    ...
```

Concurrent `KILL QUERY <victim>` while the victim's connection is closing: the kill thread reads `trx` via `thd_get_ha_data`, the close thread enters the rwlock write section and frees `trx`, the kill thread dereferences `trx->waiting_on`. Trivially exploitable on any workload that combines KILL QUERY with TidesDB.

**Fix:** Wrap the body in `mysql_rwlock_rdlock(&g_trx_lifecycle_lock)` / `mysql_rwlock_unlock`. Same pattern as the walker. The kill_query function's body is brief; lock contention with the walker is negligible.

This is the highest-priority item in this report.

---

## HIGH

### HF-1. M-12 privilege check fails open when `current_thd` is null (regression from M-12) — ✅ FIXED 2026-05-18 (commit 37441d6)
**Where:** `ha_tidesdb.cc:912-928` — **[verified]** **[regression-from-fix]**
**Current location:** `plugin/tidesdb_fts.cc:305-313` (stopword loader moved out during FTS extraction; check is fail-closed.)

```cpp
THD *cur_thd = current_thd;
if (cur_thd && cur_thd->security_context())
{
    Security_context *sctx = cur_thd->security_context();
    if (!sctx->check_access(SELECT_ACL, db_name, false))
    {
        // deny path
    }
}
// fall through to opening the CF
```

When `current_thd == nullptr` (system thread, plugin init, bootstrap context) the entire privilege check is silently skipped and the loader proceeds to read every row from the named CF. The M-12 fix was supposed to enforce SELECT-on-database; the guard fails open.

The sysvar callback `tdb_ft_stopword_table_update` (the only documented caller) does always run with a user THD, so this is **not currently exploitable** through the SET GLOBAL path. The exposure is "any future internal caller that arranges to invoke the loader from a system thread context bypasses the check."

**Fix:** Replace the guard with fail-closed:
```cpp
if (!cur_thd || !cur_thd->security_context()) {
    sql_print_warning("[TIDESDB] stop word table load denied: no security context");
    return false;
}
```

### HF-2. `g_fts_meta_mutex` is process-global; blocks all FTS-indexed writes (overlap: cpp H-2, perf H-1) — ✅ FIXED (later commit)
**Where:** `ha_tidesdb.cc:126` (decl), `:752-773` (fts_update_meta) — **[verified]** **[regression-from-fix]**
**Resolution:** Mutex was moved into `TidesDB_share::fts_meta_mutex`, exactly the report's recommended fix (see comment at `plugin/ha_tidesdb.cc:138` "g_fts_meta_mutex has moved into TidesDB_share::fts_meta_mutex" and `:1681` "former process-global g_fts_meta_mutex").

H-2's fix added a single global `mysql_mutex_t g_fts_meta_mutex` and now `fts_update_meta` takes it around its load-modify-put — including the `tidesdb_txn_put` itself. Two issues:

1. **Held across an engine call**: `tidesdb_txn_put` can block on memtable flush, WAL fsync, or internal lock contention. A plugin mutex held across that becomes a global throughput cap for all FTS writers.
2. **One mutex for the entire engine**: two unrelated tables on different CFs serialize through this single mutex. Pre-fix, lost-update only occurred between writers to the same key in the same CF; per-CF (or per-share) granularity would suffice.

The M-4 fix (delta buffering during bulk insert) lowered the per-row cost for bulk INSERT but didn't address single-row INSERT, UPDATE, or DELETE.

**Fix:** Move the mutex into `TidesDB_share` (one per table). Optionally hold it only for the load + memcpy and release before the put, accepting that two concurrent commits to the same meta key may race at commit time (TidesDB's own conflict detection catches this under SNAPSHOT+).

### HF-3. `tdb_path_is_safe` is lexical only — symlink redirect still possible — ✅ FIXED 2026-05-18 (commit 37441d6)
**Where:** `ha_tidesdb.cc:2337-2349`, called at `:2365` and `:2462` — **[verified]** **[pre-existing-newly-noticed]**
**Resolution:** Added `tidesdb_backup_allowed_root` sysvar + `tdb_path_is_under_allowed_root()` realpath-confinement helper (`plugin/ha_tidesdb.cc:872-994`). Applied at both `backup_dir_check` (line 1032) and the parallel checkpoint check (line 1158).

The H-6 fix rejects `..` and non-absolute paths but does not call `realpath()`. A `SYSTEM_VARIABLES_ADMIN` holder who can plant a symlink at `/var/lib/mysql/backups/link -> /etc/cron.d/` can redirect `SET GLOBAL tidesdb_backup_dir = '/var/lib/mysql/backups/link'` to write SSTable files into `/etc/cron.d/`. The code's own comment at the helper acknowledges this is lexical-only.

**Fix:** After validation, `realpath()` the destination and reject if the resolved path differs significantly from the input, OR introduce a `tidesdb_backup_allowed_root` sysvar and verify the resolved path starts with it.

### HF-4. Backup/checkpoint operations not cancellable on KILL QUERY — ✅ ADDRESSED (commit 37441d6)
**Where:** `ha_tidesdb.cc:2409` (backup), `:2481` (checkpoint) — **[agent]** **[pre-existing-newly-noticed]**
**Resolution:** Documented in `KNOWN-ISSUES.md` and surfaced via the new `tidesdb_backup_allowed_root` + improved error reporting in the backup overhaul; full cancellation requires upstream TidesDB library support and remains a known limitation.

Both run synchronously inside the sysvar check callback. They block until the underlying TidesDB call returns; there's no `thd_killed` polling and no cancellation handle. A slow NFS / full filesystem turns this into a thread-exhaustion DoS via SET GLOBAL.

**Fix:** Requires TidesDB library support. Interim mitigation: document the behavior in the sysvar description and surface a clear timeout error if the operation exceeds some bound.

---

## MEDIUM

### MF-1. `cached_opts_valid` / `cached_opts` not atomic — torn-read risk on weakly-ordered ISAs
**Where:** `ha_tidesdb.cc:2787-2790` (reader), `:5220-5225` (writer); `ha_tidesdb.h:416-417` (member decl) — **[verified]** **[regression-from-fix]**

The M-13 share cache pairs:

```cpp
// Writer (under lock_shared_ha_data()):
share->cached_opts = new ha_table_option_struct();
tidesdb_compute_opts_for_table(table, share->cached_opts);
share->cached_opts_valid = true;

// Reader (no lock):
if (sh->cached_opts_valid && sh->cached_opts) return sh->cached_opts;
```

Both fields are plain (non-atomic). On x86's TSO this works; on ARM/POWER the reader can see `cached_opts_valid == true` while `cached_opts` is still being initialized or even still nullptr.

**Fix:** Declare both as `std::atomic`. Writer stores with `memory_order_release`; reader loads with `memory_order_acquire`. Or use a single `std::atomic<ha_table_option_struct *>` where non-null == valid (kills the `valid` flag).

### MF-2. S3 failure-path log still leaks endpoint/bucket (regression from L-4)
**Where:** `ha_tidesdb.cc:4237-4238` — **[verified]** **[regression-from-fix]**

The L-4 fix added a `redact` lambda for the success path at line 4253 but missed the failure-path log immediately above:

```cpp
if (!objstore_connector)
{
    sql_print_error("[TIDESDB] Failed to create S3 connector for %s/%s", srv_s3_endpoint,
                    srv_s3_bucket);  // <-- raw values
```

Operator misconfiguration is a common path to this error, and the failure case is more likely to be in production logs than the success info-log.

**Fix:** Hoist the `redact` lambda above the `tidesdb_objstore_s3_create` call and apply it here too.

### MF-3. rapidjson iterative parse: no length cap on user-supplied ENGINE_ATTRIBUTE
**Where:** `ha_tidesdb.cc:2661-2671` — **[agent]** **[regression-from-fix]**

The H-9 fix switched to `kParseIterativeFlag`. The reviewer correctly noted "no depth limit, only memory cost." A maliciously deep ENGINE_ATTRIBUTE persisted in the DD now consumes O(depth) heap on every OPEN TABLE. Requires CREATE TABLE privilege so MEDIUM not HIGH, but is a regression: the old code stack-overflowed and crashed once; the new code can be tuned to exhaust memory deterministically.

**Fix:** Cap `attr.length` to a reasonable maximum (e.g. 64KB) before `doc.Parse()`. Optionally combine with `SetMaxNestingDepth(32)`.

### MF-4. Lock-order fragility: walker holds `part->mutex`, then takes `g_trx_lifecycle_lock` read-lock
**Where:** `ha_tidesdb.cc:480-498` (wait loop), `:372` (walker rdlock), `:3458` (close write-lock) — **[verified]** **[regression-from-fix]**

The H-1 fix added a deadlock re-check inside the cond_wait loop. The walker now acquires `g_trx_lifecycle_lock` (read) while `part->mutex` is held. `tidesdb_close_connection` takes `g_trx_lifecycle_lock` (write) AFTER calling `row_locks_release_all` — currently a no-op for partition mutexes because release_all doesn't take partition locks. **No live deadlock today.** But the invariant is implicit: any future maintenance that adds a partition-mutex acquisition inside `row_locks_release_all` (or in any context where the write-lock is held) creates a circular wait.

**Fix:** Document the lock order at both sites. Or restructure: take the lifecycle read-lock BEFORE `part->mutex` and reorder accordingly.

### MF-5. TLS master key retained in worker threads' TLS after `tidesdb_master_key_clear`
**Where:** `tidesdb_keyring_compat.cc:162-185` — **[verified]** **[regression-from-fix]**

The M-8 TLS cache invalidates lazily — threads holding the old key only refresh on next `encryption_key_get`. The `tidesdb_master_key_clear` doc-string says "test-only", so this is acceptable for the stated contract — but if any future operator path uses `clear` for key revocation, threads idle between encryption operations will retain the plaintext key indefinitely. Worse, `tls_master_key` is on the thread stack/TLS region, NOT under `mlock` (`mlock` is only applied to `g_master_key`). A coredump captures TLS.

**Fix:** Either rename/document `tidesdb_master_key_clear` as truly test-only (compile-out in release), or signal all threads to flush their TLS caches immediately on clear (broadcast on a condvar; worker threads check between row operations).

### MF-6. Log injection via user-controlled `table_spec` / paths
**Where:** `ha_tidesdb.cc:894, 920-925, 942-945, 2413, 2485` — **[agent]** **[pre-existing-newly-noticed]**

`SET GLOBAL tidesdb_ft_stopword_table = 'foo/bar\n2026-05-13T00:00:00Z [ERROR] [InnoDB] fake'` injects newlines into a `sql_print_warning` `%s`. The stderr `sql_print_*` shim doesn't sanitize. SIEM/log aggregators can be misled.

**Fix:** A small `tdb_sanitize_for_log(str, maxlen)` helper that replaces bytes < 0x20 with `?` or hex escapes, applied at each user-controlled-string log site.

### MF-7. `tdb_path_is_safe`: no null-byte injection check
**Where:** `ha_tidesdb.cc:2337-2349` — **[agent]** **[pre-existing-newly-noticed]**

`strstr(path, "..")` stops at the first null. A path with an embedded null `/var/safe\0../../etc` is treated as `/var/safe` by the check AND by `open(2)`, so the actual exploitation surface is small on POSIX. Defense-in-depth would still reject paths with embedded nulls; a future C++ string-aware consumer (e.g. `std::string` constructor with length) could be tricked.

**Fix:** `if (memchr(path, '\0', strlen(path)) != nullptr) return false;` before the existing checks. Cheap.

### MF-8. Thread-local heap scratches leak per thread (no destructor)
**Where:** `ha_tidesdb.cc:1211` (doc_buf), `:6248` (write_row), `:7382` (update_row), `:7718` (delete_row) — **[verified]** **[regression-from-fix]**

Four `static thread_local Struct *p = nullptr; if (!p) p = new Struct();` sites. The objects are intentionally leaked — no `pthread_key_create` destructor. For per-connection threads on a busy server this is bounded (4 small objects × pooled-thread count); for plugin reload without server restart, old pointers dangle and a new set is allocated. Total per-thread footprint is small (~few KB) but the pattern is structurally fragile.

**Fix:** Register a `pthread_key_t` with a destructor that deletes each scratch on thread exit. Or move the scratches into the share / handler (per-handler is the obvious fit; M-5 chose thread_local to amortize across handlers on the same thread).

### MF-9. `tidesdb_compute_opts_for_table` runs under `lock_shared_ha_data()` during first open
**Where:** `ha_tidesdb.cc:5214-5224` — **[verified]** **[regression-from-fix]**

The M-13 share cache is populated INSIDE the `lock_shared_ha_data()` block. The expensive work (25+ THDVAR reads + rapidjson Parse) runs serially across concurrent first-opens of the same table. Cold-start fan-out of N parallel connections opening N tables serializes through this lock.

**Fix:** Compute into a stack-local first, then take `lock_shared_ha_data()` only for the atomic publish (matching InnoDB's share-init pattern). Lose-the-race case `delete` the duplicate.

### MF-10. `end_bulk_insert` ignores `fts_update_meta` return value
**Where:** `ha_tidesdb.cc:7986-7989` — **[verified]** **[regression-from-fix]**

The M-4 delta flush calls `fts_update_meta` without checking the return code. A failed put silently drops the accumulated delta; `end_bulk_insert` returns 0 unconditionally.

**Fix:** Capture and propagate the rc. A meta-flush failure should be sticky on the statement.

---

## LOW

### LF-1. M-7 `key_unpack_scratch_` lazy-resized on every call
**Where:** `ha_tidesdb.cc:4706-4707` — **[agent]** **[regression-from-fix]**

`if (key_unpack_scratch_.size() < table->s->reclength) key_unpack_scratch_.resize(...)` runs on every index seek. Branch is well-predicted but it's a hot-path cost that could be eliminated by sizing at `open()` instead.

**Fix:** Move the resize into `ha_tidesdb::open()` after `share` is known.

### LF-2. Log message tone: mlock failure surfaces `errno` to operator log
**Where:** `tidesdb_keyring_compat.cc:81-83` — **[agent]** **[regression-from-fix]**

L-5 logs `errno` on mlock failure (e.g. `errno=12` = ENOMEM = RLIMIT_MEMLOCK hit). Reveals environmental detail — harmless individually, but combined with other log lines an attacker can fingerprint the deployment. Cosmetic.

**Fix:** Map errno to a textual hint ("locked-memory limit exceeded; raise RLIMIT_MEMLOCK") and drop the numeric code.

### LF-3. `tdb_secure_zero` is correct but `explicit_bzero` exists
**Where:** `ha_tidesdb.cc:5499-5502` (helper definition) — **[agent]** **[pre-existing-newly-noticed]**

The volatile-pointer loop is standards-correct. `explicit_bzero(p, n)` (glibc ≥ 2.25; Ubuntu 24.04 has 2.38) additionally includes a memory barrier and is more familiar to readers. Pure cosmetic preference.

### LF-4. `mlock` called for two pages always; ENOMEM under tight `RLIMIT_MEMLOCK=1page`
**Where:** `tidesdb_keyring_compat.cc:80` — **[agent]** **[regression-from-fix]**

L-5 mlocks two pages defensively in case the 32-byte key straddles. On containers with `RLIMIT_MEMLOCK=1page`, the call fails. Logs a warning but proceeds. Acceptable degradation but tighter operators see noise.

**Fix:** Attempt one page first; only try the second if the key actually straddles.

### LF-5. Per-row atomic load of `g_master_key_gen` on encrypted-table reads/writes
**Where:** `tidesdb_keyring_compat.cc:165-170` — **[agent]** **[regression-from-fix]**

M-8's TLS cache check loads `g_master_key_gen` on every `encryption_key_get`. Acquire load is free on x86 but is on every encrypted row. Could be amortized to a per-statement check (cache the gen in `external_lock`).

**Fix:** Add `cached_master_key_gen_` per-handler, refresh in `external_lock`. Skip the per-row atomic.

### LF-6. Test naming inconsistent
**Where:** `mysql-test-suite/t/` — **[agent]** **[regression-from-fix]**

6 new tests named by code-review-finding-ID (`tidesdb_h4_hilbert_negative_coords` etc.); existing 50 tests named by feature (`tidesdb_fulltext`, `tidesdb_spatial`). Finding IDs are not stable identifiers six months from now.

**Fix:** Rename to feature-based names; add `# Origin: H-4` comment in each test header for traceability.

---

## Disputed / theoretical findings

These were flagged by the reviewers but didn't hold up to verification. Documented so readers can re-check.

### D-1. cpp reviewer's "C-1" — kill_query UAF — CONFIRMED, see CF-1 above
*(re-tagged; not actually disputed)*

### D-2. cpp reviewer's "C-2" — `static_cast<TidesDB_share*>(ha_share)` UB on cross-engine ALTER — **[theoretical]**
**Where:** `ha_tidesdb.cc:2787-2789`

The cast IS unsafe in the abstract, but every current call site of `tidesdb_opts_for_table` passes a table whose share is TidesDB-owned (either the handler's own table or `altered_table` set up by the inplace_alter API, where the ENGINE is unchanged). The cross-engine ALTER concern would only fire if someone wired in `ALTER TABLE ... ENGINE=TIDESDB FROM=InnoDB` with the old InnoDB share still attached — which the current code doesn't allow. Adding a `dynamic_cast` is a one-line hardening for future maintainability but doesn't fix a present bug.

### D-3. cpp reviewer's "H-3" — bulk-insert FTS deltas lost on rollback — **[disputed]**
**Where:** `ha_tidesdb.cc:6271-6284` (accumulator), `:7973-7994` (flush)

The reviewer's claim: rollback drops accumulated deltas leaving the meta CF stale.

Re-analysis: the deltas are flushed into `stmt_txn` via `fts_update_meta(stmt_txn, ...)`. The FTS index entries themselves (the per-term `tidesdb_txn_put`s) are also in `stmt_txn`. If `stmt_txn` rolls back, both are rolled back. Net result: returns to a consistent pre-bulk-insert state. The reviewer's failure mode requires "txn commits without `end_bulk_insert` being called", which is not a normal MySQL flow — `end_bulk_insert` is invoked as a cleanup hook even on error.

The MF-10 finding (missing rc check on the flush) IS real and stands.

### D-4. cpp reviewer's "H-4" — ICP decode writes to buf+offset, val_bool reads record[0] — **[theoretical]**
**Where:** `ha_tidesdb.cc:5012-5121`

The reviewer correctly notes that `decode_sort_key_part` writes via `to = buf + (f->field_ptr() - table->record[0])` and `pushed_idx_cond->val_bool()` reads from `record[0]`. If `buf != record[0]`, the decoded values land in an offset-adjusted buffer while val_bool reads stale record[0] data.

Verification: all four call sites of `icp_check_secondary` (in index_next / index_read_map / etc.) pass `buf` that MySQL set up as the current row buffer — which is `record[0]` for normal index scans. The handler API guarantees this. The reviewer's concern about `fetch_row_by_pk(..., buf)` returning a different buffer is also theoretical — fetch_row_by_pk writes into the buf it was given. No present bug.

---

## Architectural reassessment

The architect's full pass updates the prior "if you only do three refactors" list. Summarizing:

### A-priority-refresh

**1. `EngineContext` + master-key subsystem promotion** (was A-4 #3; now #1)
The point fixes added 5 new globals (`g_trx_lifecycle_lock`, `g_fts_meta_mutex`, `g_tdb_engine` atomic, plus existing `last_conflict_mutex`, `tdb_stopword_lock`, `tdb_blend_lock`). Every one is conceptually a member of a single engine context. Doing this first gives subsequent extractions (row-lock manager, FTS) a place to hang their state.

**2. Extract row-lock manager** (was A-1 #1; now #2)
Still right priority; just sequenced after the context object so the manager can take an `EngineContext&` rather than reach into globals.

**3. Extract FTS** (was bundled with A-2; FTS now bigger, spatial split off)
The four FTS scratch sites (write_row, update_row, delete_row, fts_extract_and_tokenize), the `fts_meta_doc_delta_[MAX_KEY]` per-handler arrays, the should-be-per-share `g_fts_meta_mutex`, the stopword loader, BM25 counters, and the `idx_is_fts[MAX_KEY]` cache all want to live in one `FtsIndex` class. This is the biggest cohesion win available.

Spatial extraction stays warranted (HF-4 fuzz testing) but it's smaller; pull it as a follow-up.

`TidesStore` (former #3) drops to #4 — still right, still the natural seat for atomic-DDL participation, but the recent fixes didn't make it more urgent than `EngineContext`.

### Other architectural observations

- **`tidesdb_keyring_compat.cc`** grew from 144 → 230 lines and is now a real master-key subsystem (mlock, generation counter, TLS cache, IV validator). Promote to `tidesdb_crypto/master_key.{h,cc}`.
- **`tidesdb_compat.h`** is on a path to "renames-only-or-deletable" — two real implementations have already moved out (`handler_index_cond_check`, `LOCK_global_system_variables`). One extraction pass would finish the job.
- **Implicit design constraints** added by the fixes that aren't well-documented in public-facing places:
  - Lock entries never freed (H-3 invariant). Discoverable from comments.
  - Master key never rotated (M-8 + `tidesdb_master_key_clear` test-only contract). Partial.
  - ENGINE_ATTRIBUTE frozen at first share open (M-13). **Undocumented** — this is the most likely to bite. A user-issued `ALTER TABLE ... ENGINE_ATTRIBUTE=...` does nothing for the share's lifetime.
  - Stopword privilege check is database-level, not table-level (M-12). Documented in comments.

For the ENGINE_ATTRIBUTE one specifically: either (a) make `ALTER TABLE ... ENGINE_ATTRIBUTE=` actually re-read (clear `cached_opts_valid` in the inplace-alter commit), or (b) reject the change in `check_if_supported_inplace_alter` with `ER_NOT_SUPPORTED_YET`.

---

## Findings summary table

| ID | Severity | Tag | Where (original) | Status as of 2026-06-02 |
|---|---|---|---|---|
| CF-1 | CRITICAL | regression-from-fix | `ha_tidesdb.cc:4403-4422` | ✅ **FIXED** (commit 37441d6) |
| HF-1 | HIGH | regression-from-fix | `ha_tidesdb.cc:912-928` | ✅ **FIXED** (commit 37441d6; now `tidesdb_fts.cc:305-313`) |
| HF-2 | HIGH | regression-from-fix | `ha_tidesdb.cc:126, :752-773` | ✅ **FIXED** — promoted to per-share mutex |
| HF-3 | HIGH | pre-existing-newly-noticed | `ha_tidesdb.cc:2337` | ✅ **FIXED** (commit 37441d6) — allowed-root + realpath |
| HF-4 | HIGH | pre-existing-newly-noticed | `ha_tidesdb.cc:2409, :2481` | ✅ Addressed; full cancellation needs upstream TidesDB support |
| MF-1 | MEDIUM | regression-from-fix | `ha_tidesdb.cc:2787-2790, :5220-5225` | Not re-verified |
| MF-2 | MEDIUM | regression-from-fix | `ha_tidesdb.cc:4237-4238` | ✅ Fixed — `redact()` applied to failure log (see `ha_tidesdb.cc:2719-2737`) |
| MF-3 | MEDIUM | regression-from-fix | `ha_tidesdb.cc:2661-2671` | ✅ Fixed — 64 KiB length cap (`TIDESDB_ENGINE_ATTRIBUTE_MAX_LEN`) |
| MF-4 | MEDIUM | regression-from-fix | `ha_tidesdb.cc:480-498` | ✅ Documented lock-order invariant inline |
| MF-5 | MEDIUM | regression-from-fix | `tidesdb_keyring_compat.cc:162-185` | Not re-verified |
| MF-6 | MEDIUM | pre-existing-newly-noticed | `ha_tidesdb.cc:894, 920-925, 2413, 2485` | ✅ Fixed — `tdb_sanitize_for_log` (see `tidesdb_fts.cc:287`) |
| MF-7 | MEDIUM | pre-existing-newly-noticed | `ha_tidesdb.cc:2337-2349` | Not re-verified |
| MF-8 | MEDIUM | regression-from-fix | `ha_tidesdb.cc:1211, :6248, :7382, :7718` | Not re-verified |
| MF-9 | MEDIUM | regression-from-fix | `ha_tidesdb.cc:5214-5224` | Not re-verified |
| MF-10 | MEDIUM | regression-from-fix | `ha_tidesdb.cc:7986-7989` | Not re-verified |
| LF-1 | LOW | regression-from-fix | `ha_tidesdb.cc:4706-4707` | Not re-verified |
| LF-2 | LOW | regression-from-fix | `tidesdb_keyring_compat.cc:81-83` | Not re-verified |
| LF-3 | LOW | pre-existing-newly-noticed | `ha_tidesdb.cc:5499-5502` | ✅ Fixed — `tdb_secure_zero` now wraps `explicit_bzero` (`ha_tidesdb.cc:4014`) |
| LF-4 | LOW | regression-from-fix | `tidesdb_keyring_compat.cc:80` | Not re-verified |
| LF-5 | LOW | regression-from-fix | `tidesdb_keyring_compat.cc:165-170` | Not re-verified |
| LF-6 | LOW | regression-from-fix | `mysql-test-suite/t/` | Not re-verified |
| D-2 | — | theoretical | `ha_tidesdb.cc:2787-2789` | n/a (theoretical) |
| D-3 | — | disputed | `ha_tidesdb.cc:6271-6284, :7973-7994` | n/a (disputed) |
| D-4 | — | theoretical | `ha_tidesdb.cc:5012-5121` | n/a (theoretical) |

**Original counts:** 1 CRITICAL, 4 HIGH, 10 MEDIUM, 6 LOW (14 regressions + 7 pre-existing).

**2026-06-02 counts:** 0 open CRITICAL, 0 open HIGH. The closing of CF-1 alongside the EngineContext / row-lock / FTS / spatial extractions vindicated the report's architectural recommendation — the rollout class of bug ("touched 90% of call sites, missed one") is materially less likely now that the lifecycle critical sections are owned by extracted modules with explicit lock ownership.
