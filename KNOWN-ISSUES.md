# Known issues

This document tracks defects we've confirmed in the bundled TidesDB engine
that affect `tidesdb-mysql` users. **All four engine bugs found during
the v0.2.3/v0.2.4 investigation are now patched** in
`docker/patches/0001-walfix.patch`.

## Patched (v0.2.3 + v0.2.4)

The bundled `docker/patches/0001-walfix.patch` applies three engine
fixes against vendored TidesDB v9.2.0 inside the Docker build (before
`cmake`). They will be removed once equivalent fixes land upstream.

### 1. `convert_sync_mode()` had inverted case logic

**File:** `tidesdb/src/block_manager.c`, function `convert_sync_mode`.
**Symptom:** Plugin requested `sync_mode=FULL` but the engine silently
skipped per-write `fdatasync` (no `O_DSYNC`, no sync at all). The
function's `case 1` returned `BLOCK_MANAGER_SYNC_FULL` (mapping
`TDB_SYNC_INTERVAL → FULL`) and `case 2` fell through `default →
BLOCK_MANAGER_SYNC_NONE` (mapping `TDB_SYNC_FULL → NONE`). Inverted.
**Fix:** rewrite the switch to map `0 → NONE`, `1 → NONE` (interval is
handled by engine's background flusher), `2 → FULL`.

### 2. Multiple WAL `block_manager_open` sites passed the raw engine enum

**File:** `tidesdb/src/tidesdb.c`, lines 17269, 18600, 19174, 19190, 23298.
**Symptom:** Even after (1) was fixed, these call sites passed
`config->sync_mode` / `cf->config.sync_mode` / `umt_sync_mode` directly
to `block_manager_open` without going through `convert_sync_mode()`, so
the wrong enum value still reached the block manager.
**Fix:** wrap each with `convert_sync_mode(...)` (consistent with the
klog/vlog open sites that already did so at lines 5290, 7955, 7972, etc).

### 3. `tidesdb_create_column_family` unconditionally truncated the WAL

**File:** `tidesdb/src/tidesdb.c`, around line 18610.
**Symptom:** The function is invoked both for fresh `CREATE TABLE` and
during database open when an existing CF directory is rediscovered on
disk. It called `block_manager_truncate(new_wal)`, which wipes the WAL
to header-only — running **before** `recover_wals` had a chance to
replay it. Result: silent loss of every committed write on a hard
crash.
**Fix:** replace with `block_manager_validate_last_block(PERMISSIVE)`,
which (a) writes the header for a 0-byte file, (b) leaves a valid
header-only file alone (fresh CF case), and (c) forward-scans + sets
`current_file_size` to the last valid block (recovery case).

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
**Fix:** removed the `current_block_size = bdata_size` /
`block_size_valid = 1` assignments at all four sites. The next
`cursor_next` call now `pread`'s the real 4-byte on-disk size header
— one cheap syscall per block, in the host page cache anyway.
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
