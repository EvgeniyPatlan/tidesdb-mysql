# Known issues

This document tracks defects we've confirmed in the bundled TidesDB engine
that affect `tidesdb-mysql` users.

## Current: bundled on TidesDB v9.3.2 — shipped unpatched

As of release **v0.3.1** the engine is pinned to **TidesDB v9.3.2** and we
continue to ship it **with zero patches**. Both fixes we used to carry are
upstream:

- `0001-walfix.patch` (four durability bugs) — fixed in **v9.2.5**, retired then.
- `0001-bloomfix.patch` (the `bloom_filter_new` UAF, TidesDB **PR #626**) —
  landed upstream **verbatim in v9.3.0**, retired with that bump.

The `docker/patches/` directory has no engine patches; no Dockerfile or script
applies one. The per-bug write-ups below are kept as a record and as a
regression checklist for future upgrades.

What's new since v9.3.0 (v9.3.1 + v9.3.2, no plugin change needed):

- **Concurrency / memory-safety hardening (v9.3.1).** Clock-cache reader-pin
  wraparound at 128 readers (would corrupt zero-copy buffers), flush-cleanup
  use-after-free over the sixteen-immutable threshold, transaction-reset
  dangling pointer (repeatable-read / snapshot), duplicate column-family
  registration race, 32-bit MSVC atomics.
- **Reader FD starvation fix (v9.3.1).** Engine-side counterpart to the
  fd-pressure behaviour the v0.3.0 100 GiB stress run documented: a flush-path
  descriptor leak (a bare `close` skipping the counted-open decrement) is fixed,
  and reader/reaper budgets are unified so the reserve always stays available.
- **Backpressure simplification (v9.3.1).** L1 hard-stop removed; admission is
  governed by L0 stall + the active-memtable ceiling.
- **Parallel compaction within a round (v9.3.1).** Per-CF rounds borrow
  ephemeral helper threads with work-stealing and shard merge output across
  key-range subcompactions; ~25 % higher ingest throughput in upstream's tests.
- **Large bloom filters / block indexes (v9.3.2).** Auxiliary klog blocks are
  chunked when they exceed the 4 GB block-manager size; **backwards-compatible**.
- **`_tidesdb_cancel_background_work_` (v9.3.2).** Quick-shutdown helper for
  large flush/compaction queues.

Carried over from v9.3.0 and still handled plugin-side
(see [CHANGELOG.md](CHANGELOG.md)):

- **`TDB_ERR_BUSY` (-14)** is returned from backpressure-stall timeouts that
  previously surfaced as `TDB_ERR_IO`. `tdb_rc_to_ha` maps it to
  `HA_ERR_LOCK_WAIT_TIMEOUT` (retriable), so a transient stall no longer looks
  like `HA_ERR_CRASHED` (corruption).
- The new **active-memtable backpressure ceiling** (2× `write_buffer_size`)
  bounds the unbounded memtable growth that produced the WARE=100 OOM during
  v0.2.5 validation; the plugin's `default_l0_queue_stall_threshold` default
  was lowered 20 → 10 to match upstream now that this is the gating surface.

## Known limitations introduced or formalised in v0.4.0

These are atomic-DDL participation limitations. They are not engine bugs — they are deliberate scope boundaries of the v0.4.0 contract, tracked here so operators know what to expect. Full write-up: [docs/v0.4.0-validation-report.md](docs/v0.4.0-validation-report.md) (*Known limitations*).

### 1. DD-commit / engine-commit two-phase-commit gap

A narrow window exists between the DD-side commit and the engine-side commit where the two can momentarily diverge. The same window exists in InnoDB. If the engine commit hook fails after the DD has already committed, the next-startup `DdSyncReconciler` sweep reconciles per the `tidesdb_orphan_action` sysvar (default `quarantine`). Full closure requires server-side XA-style 2PC, which is **not** in MySQL 9.7's atomic-DDL contract. No code-side mitigation is possible from a storage-engine plugin alone.

### 2. DDSE callback stubs are inert

All eight DDSE entry points (`ddse_dict_init`, `dict_init`, `dict_recover`, `dict_cache_reset`, `dict_cache_reset_tables_and_tablespaces`, `dict_get_server_version`, `dict_set_server_version`, `is_dict_readonly`) are wired so the handlerton registers cleanly. Each logs once at INFO if invoked, then returns success. No production MySQL 9.7 code path drives them for an engine that does not host the data dictionary itself. They exist as forward-capability slots for a future "TidesDB hosts the data dictionary" project.

### 3. Legacy v0.3.x table SDI not auto-retrofitted on open

Pre-v0.4.0 tables have no `se_private_data` and no SDI blob in `__tidesdb_sdi`. The supported upgrade path is **`ALTER TABLE t ENGINE=TIDESDB`** per user table, which populates `se_private_data` and emits the SDI blob. Strict mode (`tidesdb_atomic_ddl_strict=ON`, the default) refuses to open legacy tables; setting it to `OFF` temporarily during upgrade allows opens with a warning. Auto-retrofit on open was considered and rejected — it would silently rewrite metadata for tables the operator may not have intended to touch.

### 4. `mysqldump --tab` round-trip is smoke-tested only

The four SDI MTR tests exercise round-trip on the `__tidesdb_sdi` metadata CF, but a full `mysqldump --tab` end-to-end integration test is deferred to **v0.5.0**.

### 5. Twelve crash-injection MTR tests skip on the Release `mysql-mtr` image

The atomic-DDL test suite includes 12 tests that use `DBUG_SUICIDE` for controlled crash injection. These require a Debug `mysqld` (gated by `have_debug.inc`) and so skip on the Release-mode `tidesdb/mysql-mtr:9.7` image used for CI. They were validated locally on a one-off Debug image (`tidesdb/mysql-mtr:9.7-dbg4`, preserved locally) during root-cause analysis of the COPY-ALTER 2PC SIGSEGV. Producing a steady-state Debug MTR image for CI is a follow-up.

### 6. COPY-ALTER fix is tactical

The v0.4.0 fix for the COPY-ALTER 2PC use-after-free (`tidesdb_flush_engine_txn_before_cf_mutation`, commit `0d7fe2c`) flushes the engine session txn at the top of `ha_tidesdb::rename_table` and `ha_tidesdb::delete_table`. This restores pre-flag-flip engine-layer ordering and preserves the atomic-DDL contract at the server / DD layer, but it gives up a narrow window (engine has committed; DD has not) — the startup sweep recovers it. The architecturally correct fix — a SE-private DDL journal so CF rename / drop is itself transactional alongside user data writes — is deferred to **v0.5.0**.

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
