# Known issues

This document tracks defects we've confirmed in the bundled TidesDB engine
that affect `tidesdb-mysql` users.

## Current: bundled on TidesDB v10.0.1 — one patch carried, one issue open

The engine is pinned to **TidesDB v10.0.1** and we carry **one** patch,
`docker/patches/tidesdb/0001-reservation-retirement-floor.patch`. It is applied
by both `docker/Dockerfile.mysql` and `scripts/setup-workspace.sh`, so a local
build and the shipped image run the same engine.

One issue is **open and unfixed**: concurrent bulk loaders take false conflicts
and fail. It is the unpatched half of the same reservation-collision behaviour,
it needs a design change upstream, and it is written up below.

### `0001-reservation-retirement-floor.patch` — conflict-free commits refused

**Severity:** high — aborts transactions that have no conflict, on workloads
with no concurrency at all.

TidesDB 10 added a first-committer-wins reservation table: 2^20 slots indexed
by the low bits of a key hash, each packing a 16-bit fingerprint with a commit
sequence. When two unrelated keys land in one slot, the fingerprint is what
distinguishes a real same-key writer from the collision. It only gets to decide
if the slot's current occupant can be retired, and the bound deciding that was
wrong in two ways:

- It counted the committing transaction itself. That transaction has already
  weighed its whole write set against its own read versions, so it is never the
  reader the bound is protecting; counting itself only barred it from evicting
  records nothing else wanted. Snapshots are drawn one below the highest
  assigned sequence, so the previous commit on the same connection always sat
  above the next transaction's snapshot — permanently unretirable.
- It read `published_min_snapshot`, which is maintained for the compaction GC
  floor. That consumer wants the value to err low; this one needs the opposite,
  because low makes occupants look unretirable and refuses good commits. It is
  also stale between compaction scans and zero before the first one, since the
  publishing call has a single caller.

**Symptom:** `ERROR 1180 ... Got error 149 - 'Lock deadlock; Retry transaction'`
on `COMMIT`, on a single connection, with no other writer on the server. A
1000-transaction single-connection loop (4 UPDATEs + 1 DELETE + 1 INSERT over
5000 rows with a secondary index) produced 2 such aborts; with the patch, 0
over 5000 transactions.

**Fix:** take the exact minimum over the *other* live transactions. The
registry also gained a live count so the common single-writer case answers
without walking all 32 shards, which keeps single-threaded commit throughput
where it was.

Sent upstream; drop the patch once it lands.

### OPEN: concurrent bulk loaders take false conflicts and fail

**Severity:** high for multi-threaded bulk loading; no effect on ordinary OLTP.
**Status:** upstream design limitation. Our patch above fixes one half of it;
this is the half that remains. Not fixable plugin-side without giving up
conflict detection that users are entitled to.

**Symptom.** A bulk load run by several connections at once fails partway
through, with a statement error and this in the error log:

```
[Warning] [TIDESDB] bulk mid-commit failed rc=-7; the engine has already
          aborted this transaction, so the statement is rolled back rather
          than retried
```

`-7` is `TDB_ERR_CONFLICT`. It is reported even when the loaders write
completely disjoint keys and no real conflict is possible. A client that does
not retry the statement simply stops; a HammerDB TPROC-C schema build at 10
warehouses with 4 loader threads hangs at roughly 40% loaded, every time. The
same build on the previous release completes.

**Mechanism.** TidesDB 10 detects write-write conflicts with a
first-committer-wins reservation table: 2^20 slots indexed by the low bits of
a key hash, each holding a 16-bit fingerprint of that hash plus the commit
sequence. Two unrelated keys can land in one slot. The fingerprint is what
distinguishes a real same-key writer from that collision, but it is only
consulted when the slot's current occupant can be retired:

```c
if (cseq > read_base && (TDB_MVCC_RES_FP(cur) == myfp || cseq > min_snapshot))
    return 0;   /* conflict */
```

`min_snapshot` is the oldest snapshot any live transaction holds. With several
loaders running concurrently that floor sits well behind the newest commits,
so almost every occupant is unretirable, the fingerprint stops deciding
anything, and a mere collision becomes a refused commit. The more concurrent
writers, the wider the window and the more often it fires.

**Why the arm cannot simply be deleted.** It is load-bearing. Claiming a slot
evicts whatever record was there, and that record is what a later writer of
the *colliding* key would have used to notice a conflict of its own. If the
evicting transaction's sequence has fallen below the floor by the time that
writer commits, the conflict is missed rather than merely mis-reported. A
missed conflict is a lost update, which is far worse than a spurious abort.
Trusting the fingerprint unconditionally trades a loud wrong answer for a
silent one.

Fixing it properly is a design change in the engine -- a larger table, chained
slots, or storing enough of the key to verify a collision -- not a patch we
should improvise into a vendored dependency.

**Why there is no plugin-side workaround.** The obvious one is to run bulk DML
at `READ COMMITTED`, where reservations are not taken at all. It was tried and
reverted. `maybe_bulk_commit` already resets to `READ COMMITTED` after each
mid-statement commit, so only the first batch of a statement is exposed, and
closing that gap looked free. It is not: MySQL routes a plain `INSERT` inside
`START TRANSACTION` through `start_bulk_insert`, so the change silently
removed conflict detection from ordinary transactional inserts. Two
transactions inserting the same primary key stopped conflicting and became
last-writer-wins. `tidesdb_insert_conflict` and `tidesdb_concurrent_conflict`
caught it immediately.

**What this means in practice.**

- **Single-connection work is unaffected.** Reservations only fire at
  `SNAPSHOT` isolation and above, and one connection committing in a loop no
  longer takes false conflicts at all since the patch above.
- **Concurrent OLTP takes a large volume of spurious conflicts.** Measured on
  a TPC-C mix, 8 connections, 10 warehouses, three minutes: **4380** conflicts
  on v0.5.0 against **1** on v0.4.1 for the identical workload. They are spread
  evenly across every connection and sustained for the whole run, which is the
  signature of hash collisions rather than genuine row contention -- real
  contention would concentrate on the hot per-warehouse rows.

  A client that retries makes progress regardless: that run completed and
  posted higher throughput than the v0.4.1 baseline. But the retries are
  wasted work, and an application that treats a deadlock error as fatal will
  see failures where v0.4.1 saw none.
- **Multi-threaded bulk loading can fail outright**, because a bulk statement
  that takes a conflict mid-commit cannot be replayed: `mysqlslap
  --concurrency`, a parallel `mysqldump` restore, a TPC-C loader, any `LOAD
  DATA` fan-out.
- **Mitigation:** load with a single connection -- verified, a 10-warehouse
  TPC-C schema build completes with one loader where four hang -- and use a
  client that retries on `ER_LOCK_DEADLOCK` (1213) / error 1180. The data is
  never wrong: the transaction aborts cleanly and nothing partial is kept.

**Reproducer.** `IMG=<image> WARE=10 BUILDVU=4 RUNVU=8 RAMP=1 DUR=3
./bench/hammerdb/run-hammerdb.sh`. Compare against the previous release with
the same command and a different `IMG=`; it completes and reports ~1783 NOPM.

**Related:** the same collision behaviour, in its single-connection form, is
what `0001-reservation-retirement-floor.patch` above fixes. That patch removed
the case where the floor was *never* advanced (published as zero before the
first compaction, and counting the committing transaction itself). What is
left is the case where the floor is real but simply older than the commits
being collided with, which concurrency makes routine.

### Retired patches

Both patches we used to carry are upstream and no longer applied:

- `0001-walfix.patch` (four durability bugs) — fixed in **v9.2.5**.
- `0001-bloomfix.patch` (the `bloom_filter_new` UAF, TidesDB **PR #626**) —
  landed upstream verbatim in **v9.3.0**.

The per-bug write-ups below are kept as a record and as a regression checklist
for future upgrades.

Handled plugin-side (see [CHANGELOG.md](CHANGELOG.md)):

- **Error code -14 changed meaning in v10.** It was `TDB_ERR_BUSY`
  (backpressure-stall timeout) through the 9.x line; it is now
  `TDB_ERR_TXN_EXPIRED`. The number is the same, so nothing fails to compile --
  `tdb_rc_to_ha` was updated by hand. v10 also adds `TDB_ERR_NO_SPACE` (-15),
  `TDB_ERR_TXN_ABORTED` (-16) and `TDB_ERR_TOO_OLD` (-17).
- **`TDB_ERR_LOCKED`** marks a read left unservable by contention, which the
  caller is expected to retry rather than surface. The plugin wraps the
  affected engine reads in a bounded retry (`plugin/tidesdb_retry.h`).
- The **active-memtable backpressure ceiling** (2x `write_buffer_size`) bounds
  the unbounded memtable growth that produced the WARE=100 OOM during v0.2.5
  validation; the plugin's `default_l0_queue_stall_threshold` default was
  lowered 20 -> 10 to match upstream now that this is the gating surface.

## Known limitations (carried into v0.5.0)

These are atomic-DDL participation limitations. They are not engine bugs — they are deliberate scope boundaries of the v0.4.0 contract, tracked here so operators know what to expect. Full write-up: [docs/v0.4.0-validation-report.md](docs/v0.4.0-validation-report.md) (*Known limitations*).

### 1. DDL: DD-commit / engine-commit two-phase-commit gap

**Narrowed in v0.5.0, and worth separating into two halves that used to be described as one.**

**DML is closed.** Through v0.4.x the engine had no durable prepare, so the plugin ran the whole commit inside the prepare hook. A crash between prepare and commit left writes durable in the engine and absent from the binlog with no way to find them, and a binlog flush that failed after a successful prepare left the engine holding data the server had discarded. TidesDB 10 has a real prepare: transactions prepare durably, stay invisible until phase two, and any left in doubt by a crash are resolved against the binlog at startup through the `recover` / `commit_by_xid` / `rollback_by_xid` hooks. `tidesdb_v10_prepared_recovery` covers both directions.

**DDL is not.** Creating, dropping and renaming a column family are not transactional engine operations — they are direct calls, not writes inside a transaction — so they cannot be carried by the prepare above. The window between the DD-side commit and the engine-side CF mutation therefore remains, and the next-startup `DdSyncReconciler` sweep is still what reconciles it, per the `tidesdb_orphan_action` sysvar (default `quarantine`). Closing this needs the SE-private DDL journal described in §6, not more 2PC.

Note also that the sweep this section relies on runs on a **manual trigger**, not at startup. See §5a.

### 2. DDSE callback stubs are inert

All eight DDSE entry points (`ddse_dict_init`, `dict_init`, `dict_recover`, `dict_cache_reset`, `dict_cache_reset_tables_and_tablespaces`, `dict_get_server_version`, `dict_set_server_version`, `is_dict_readonly`) are wired so the handlerton registers cleanly. Each logs once at INFO if invoked, then returns success. No production MySQL 9.7 code path drives them for an engine that does not host the data dictionary itself. They exist as forward-capability slots for a future "TidesDB hosts the data dictionary" project.

### 3. Legacy v0.3.x table SDI not auto-retrofitted on open

Pre-v0.4.0 tables have no `se_private_data` and no SDI blob in `__tidesdb_sdi`. The supported upgrade path is **`ALTER TABLE t ENGINE=TIDESDB`** per user table, which populates `se_private_data` and emits the SDI blob. Strict mode (`tidesdb_atomic_ddl_strict=ON`, the default) refuses to open legacy tables; setting it to `OFF` temporarily during upgrade allows opens with a warning. Auto-retrofit on open was considered and rejected — it would silently rewrite metadata for tables the operator may not have intended to touch.

**Interaction with the v0.5.0 format break.** This retrofit has to happen on the **old** server, before dumping. v0.5.0 cannot open a v0.4.x data directory at all — it refuses to start the engine (see [docs/upgrade-v0.5.0.md](docs/upgrade-v0.5.0.md)) — so there is no v0.5.0 server on which to run the `ALTER`. A v0.3.x user upgrading to v0.5.0 does the retrofit on v0.4.x, dumps, then loads into v0.5.0.

### 4. `mysqldump --tab` round-trip is smoke-tested only

The four SDI MTR tests exercise round-trip on the `__tidesdb_sdi` metadata CF, but a full `mysqldump --tab` end-to-end integration test is deferred to **v0.5.0**.

### 5. Run the suite against a Debug build; the Release image is reduced coverage

The crash-injection tests use `DBUG_SUICIDE` and are gated by `have_debug.inc`, so they do not execute on a Release build. This used to be written down as twelve tests that "skip on the Release image", with a Debug CI image listed as a follow-up.

That framing was the problem. Tests that quietly skip read as passes, and four real plugin defects accumulated behind them — including a reconciler whose engine-name comparison never matched, which made the whole subsystem inert, and status variables that were invisible because every one of them was missing its scope field.

**The expected way to run the suite is the local Debug tree** (`scripts/setup-workspace.sh` then `./mtr --suite=tidesdb`), where the crash-injection tests actually execute. The Release `tidesdb/mysql-mtr:9.7` image is the reduced-coverage path, not the default.

Two further gates only a Debug build reaches: `tidesdb_v10_prepared_recovery`, which crashes the server between prepare and binlog and again between binlog and engine commit, and the perf-instrumented build (`-DTIDESDB_PERF=1`), which runs four tests and seven unit tests the default build skips. Check for `[ skipped ]` in the MTR summary rather than reading the pass count alone.

### 5a. The reconciler sweep does not run at startup

`DdSyncReconciler::apply_delta` is gated off at plugin init and driven by a DBUG hook instead. The reason is DD warm-up timing: on the bootstrap thread the data dictionary's table cache may not yet list user tables, and an init-time sweep then classifies live tables as orphan CFs and quarantines them — observed, not theorised.

This matters because §1 and §6 both name that sweep as the recovery mechanism for their window. It is available, but an operator has to run it; it is not automatic. The principled fix is the `post_recover` handlerton hook, which runs after recovery with a warm DD.

### 6. COPY-ALTER fix is tactical

The v0.4.0 fix for the COPY-ALTER 2PC use-after-free (`tidesdb_flush_engine_txn_before_cf_mutation`, commit `0d7fe2c`) flushes the engine session txn at the top of `ha_tidesdb::rename_table` and `ha_tidesdb::delete_table`. This restores pre-flag-flip engine-layer ordering and preserves the atomic-DDL contract at the server / DD layer, but it gives up a narrow window (engine has committed; DD has not) — the sweep in §5a recovers it.

**Still open after v0.5.0, and the durable prepare does not close it.** The architecturally correct fix is an SE-private DDL journal, so CF rename and drop are themselves transactional alongside user data writes. TidesDB 10's prepare covers transactions, and CF create / drop / rename are not transaction operations, so they cannot ride on it. Deferred again.

## Verified fixed upstream in v9.3.0 (formerly our bloomfix patch)

### `bloom_filter_new()` use-after-free on its failure paths

**File:** `tidesdb/src/bloom_filter.c` (`bloom_filter_new`) +
`tidesdb/src/tidesdb.c` (`tidesdb_partitioned_merge` file_max split).
**Symptom:** a TPC-C run hit a general protection fault in a compaction
worker at `bloom_filter_add`, loading `bf->bitset` from a non-canonical
address. `bloom_filter_new` `malloc()`s the struct first, then runs four
post-malloc validators that on failure `free(*bf)` and return -1 **without
setting `*bf = NULL`**. Every caller checks the return value and clears
`bloom` itself except one — the file_max split path in
`tidesdb_partitioned_merge` — which under the right partition size left a
dangling pointer that the next `bloom_filter_add` faulted on (the freed
chunk could be recycled by another thread in between). The failure paths
are reached on `m`/`h`/`size_in_words` overflow or a `bitset` calloc
failure.
**Upstream (v9.3.0):** `bloom_filter_new` now sets `*bf = NULL` on all four
post-malloc failure paths, and the `tidesdb_partitioned_merge` file-max-split
caller checks the return value and logs a `TDB_LOG_WARN` on decline — the
exact change we had carried as `0001-bloomfix.patch`. A new
`bloom_filter_tests` case exercises each failure path and asserts the
post-condition.

## Verified fixed upstream in v9.2.5 (formerly our walfix patch)

These four bugs were patched against vendored v9.2.0 in the old
`0001-walfix.patch`. On migrating to v9.2.5 we verified each is fixed in
the upstream tree and dropped the patch. Applying the old patch to v9.2.5
would in fact be **harmful** (see #1) — it is kept here only as a record.

### 1. `convert_sync_mode()` had inverted case logic

**File:** `tidesdb/src/block_manager.c`, function `convert_sync_mode`.
**Symptom:** Plugin requested `sync_mode=FULL` but the engine silently
skipped per-write `fdatasync` (no `O_DSYNC`, no sync at all). The
function's `case 1` returned `BLOCK_MANAGER_SYNC_FULL` (mapping
`TDB_SYNC_INTERVAL → FULL`) and `case 2` fell through `default →
BLOCK_MANAGER_SYNC_NONE` (mapping `TDB_SYNC_FULL → NONE`). Inverted.
**Fix (v9.2.0 walfix):** rewrite the switch to map `0 → NONE`, `1 → NONE`
(interval is handled by engine's background flusher), `2 → FULL`.
**Upstream (v9.2.5):** the engine sync enum was **reordered** to
`TDB_SYNC_NONE=0, TDB_SYNC_FULL=1, TDB_SYNC_INTERVAL=2`, which makes the
existing `convert_sync_mode` switch (`0→NONE, 1→FULL, default→NONE`)
correct, and `TDB_SYNC_FULL` now equals `BLOCK_MANAGER_SYNC_FULL=1`.
**Warning:** because of this reorder, applying the old walfix
`convert_sync_mode` rewrite to v9.2.5 would map `FULL(1) → NONE` and
silently re-break durability — which is why the patch is retired, not
ported.

### 2. Multiple WAL `block_manager_open` sites passed the raw engine enum

**File:** `tidesdb/src/tidesdb.c`, lines 17269, 18600, 19174, 19190, 23298.
**Symptom:** Even after (1) was fixed, these call sites passed
`config->sync_mode` / `cf->config.sync_mode` / `umt_sync_mode` directly
to `block_manager_open` without going through `convert_sync_mode()`, so
the wrong enum value still reached the block manager.
**Fix (v9.2.0 walfix):** wrap each with `convert_sync_mode(...)`
(consistent with the klog/vlog open sites that already did so).
**Upstream (v9.2.5):** `block_manager_open` now calls
`convert_sync_mode()` **internally**, so call sites correctly pass the raw
engine enum and conversion happens in one place. Combined with the enum
reorder in #1, every WAL open resolves to the right mode whether or not
the caller wraps.

### 3. `tidesdb_create_column_family` unconditionally truncated the WAL

**File:** `tidesdb/src/tidesdb.c`, around line 18610.
**Symptom:** The function is invoked both for fresh `CREATE TABLE` and
during database open when an existing CF directory is rediscovered on
disk. It called `block_manager_truncate(new_wal)`, which wipes the WAL
to header-only — running **before** `recover_wals` had a chance to
replay it. Result: silent loss of every committed write on a hard
crash.
**Fix (v9.2.0 walfix):** replace with
`block_manager_validate_last_block(PERMISSIVE)`, which (a) writes the
header for a 0-byte file, (b) leaves a valid header-only file alone (fresh
CF case), and (c) forward-scans + sets `current_file_size` to the last
valid block (recovery case).
**Upstream (v9.2.5):** `tidesdb_create_column_family` now scans the CF
directory for an existing WAL and branches: an existing WAL gets
`validate_last_block(PERMISSIVE)` (preserved for recovery replay) and only
a genuinely fresh CF gets `block_manager_truncate`. Same outcome as our
fix, with the fresh-vs-existing distinction made explicit.

**Verification of (1)+(2)+(3):** MTR full tidesdb suite 61/61 PASS;
minimal repro (5 INSERTs → `docker kill -9` → restart → SELECT) returns
all 5 rows pre/post-restart; mixed 100-row `BEGIN/COMMIT` + 5-row
autocommit recovers 105/105.

### 4. SSTable cursor cached the wrong `block_size`, causing iterator to skip past `klog_data_end_offset`

**File:** `tidesdb/src/tidesdb.c`, four sites (lines ~10763, ~25279,
~25629, ~25636) that set `cursor->current_block_size = bdata_size` /
`= block_data_size` plus `cursor->block_size_valid = 1`.
**Symptom:** After heavy concurrent bulk writes + SIGKILL, tables
whose post-recovery `level 1` ended up with **two SSTables** (one
loaded from disk + one newly recovery-flushed) returned a tiny
fraction of their rows on full scans:
`tpcc__order_line` 2,669 of 3.18M (0.08%), `tpcc__stock` 717 of 1M
(0.07%). Tables with one SSTable were unaffected.
**Root cause:** the four sites cached the **cache-entry size**
(`bdata_size = cached_size - hdr_size`, i.e. decompressed block data
+ appended per-entry index entries) as the cursor's "current block
size". The next `block_manager_cursor_next` call used this inflated
value to advance `current_pos` by `header + bdata_size + footer` —
which can be HUGE (we measured deltas of 160 MB / 318 MB / 812 MB /
1.2 GB / 1.7 GB per cursor advance). `current_pos` then exceeded
`klog_data_end_offset` after 2–3 calls and `cursor_next` returned
`TDB_ERR_NOT_FOUND`, dropping the rest of the SSTable.
**Fix (v9.2.0 walfix):** removed the `current_block_size = bdata_size` /
`block_size_valid = 1` assignments at all four sites. The next
`cursor_next` call now `pread`'s the real 4-byte on-disk size header
— one cheap syscall per block, in the host page cache anyway.
**Upstream (v9.2.5):** the cursor path was rewritten to enforce the same
invariant more precisely — it caches `bmblock->size` (the real on-disk
size) only when a block was actually read from disk, and otherwise sets
`block_size_valid = 0` to force `cursor_next` to re-read the size header.
The old merge-advance site now likewise clears `block_size_valid`. Net
effect matches our fix while keeping the pread optimization when the size
is genuinely known.
**Verification:** MTR 61/61 PASS; recovery-diag post-fix shows full
row counts (`tpcc__order_line` 3,100,951, `tpcc__stock` 1,000,000,
`tpcc__orders_w1` per-district matches `district.d_next_o_id - 1`
exactly).

## How the bugs were found

All four were found via a single repeatable scenario: build a TPC-C
schema, run a brief NewOrder mix, `docker kill -9` mid-write, restart
on the same volume, compare row counts to the loader's claim
(`district.d_next_o_id`). The harness in `bench/hammerdb/`:

- `recovery-test.sh` — end-to-end pass/fail recovery verdict.
- `recovery-diag.sh` — captures pre-kill + T+0/T+30/T+60 row counts,
  TidesDB LOG, per-CF on-disk state, and (with instrumented binaries)
  per-source heap-pop / cursor traces.
- `run-all.sh` step 2 — includes recovery in the v0.2.3 suite.

Bug #4 was nailed across five focused instrumentation rounds. The
write side, metadata persistence, level array, iterator setup, and
heap pop all checked out. The bug was inside the cursor: a single
`fprintf` inside `block_manager_cursor_next` showed `current_pos`
jumping by 160 MB → 1.7 GB per call when the cursor's cached
`current_block_size` had been set from a cache-entry size by the
upstream lazy path.

### Verifying after a future TidesDB upgrade

```bash
cd bench/hammerdb
./recovery-diag.sh
# bench/results/recovery-diag-*/snapshots.txt -- post-restart counts
# must match `district.d_next_o_id - 1` for each (w, d).
```

`run-all.sh` runs the full suite (correctness baseline, recovery,
VU sweep, head-to-head vs InnoDB, TPROC-H, sustained) and generates
a self-contained `REPORT.md`.
