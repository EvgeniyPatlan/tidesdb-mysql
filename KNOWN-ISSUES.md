# Known issues

This document tracks defects we've confirmed in the bundled TidesDB engine
that affect `tidesdb-mysql` users. Three are patched in v0.2.3 via
`docker/patches/0001-walfix.patch`; one is precisely localized but not
yet patched.

## Patched (v0.2.3)

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

## Localized but not patched (residual bug #4)

Under **concurrent bulk writes followed by SIGKILL**, the engine still
exhibits partial silent loss on tables whose post-recovery state ends
up with **two SSTables in level 1** — one loaded from disk via
`tidesdb_sstable_load` (the pre-kill flushed SSTable) plus one newly
created during recovery via `tidesdb_level_add_sstable` (the
recovery-flushed memtable). Tables with only one SSTable post-recovery
read correctly.

### Observed signature (WARE=10 BUILDVU=4 RUNVU=4 + 30s NewOrder mix)

| CF | Level 1 contents | Expected | `SELECT COUNT(*)` |
|---|---|---|---|
| `tpcc__orders` | sst_id=0 only (recovery-flushed) | ~310k | **307,540 ✓** |
| `tpcc__customer` | sst_id=0 only (recovery-flushed) | 300k | **300,000 ✓** |
| `tpcc__stock` | sst_id=0 (605k, pre-kill) + sst_id=1 (454k, recovery) | ~1.05M | **717** ❌ |
| `tpcc__order_line` | sst_id=0 (2.4M, pre-kill) + sst_id=1 (770k, recovery) | ~3.18M | **2,669** ❌ |

### Root-cause localization (four rounds of instrumentation)

1. **Write side correct.** `[sstinstr]` confirmed
   `tidesdb_sstable_write_from_memtable` writes all entries
   (`entry_count == skip_list_count_entries` for every CF, e.g.
   308,267/308,267 for orders, 2,411,249/2,411,249 for order_line).

2. **Metadata persistence correct.** `[ssfooter]` (written) and
   `[ssload]` (loaded post-restart) match exactly:
   `num_entries`, `klog_data_end_offset`, `min/max_key_size`, `max_seq`.

3. **Level state correct.** `[level_add]` shows both SSTables appended
   to `cf->levels[0]` (`num_sstables: 0→1→2`); `[iter_level]` shows
   the iterator picks up both as sources (`level->num_ssts=2,
   added_now=2`).

4. **SSTable advance returns `TDB_ERR_NOT_FOUND` (-3) prematurely.**
   `[heap_pop]` exhaustion log shows each SSTable source dying after
   only ~300–1,400 pops (vs. `num_entries` in the millions). For
   `order_line`: sst_id=0 exhausts at 1,338 pops out of 2.4M expected;
   sst_id=1 exhausts at 1,329 out of 770k expected. The sum (~2,667)
   matches `SELECT COUNT(*)` (2,669) almost exactly. Same pattern for
   `stock` (433 + 323 = 756 ≈ 717).

### Code location (10-line region)

`tidesdb/src/tidesdb.c` around line 10620, in the SSTable branch of
`tidesdb_merge_source_advance`:

```c
while (block_manager_cursor_next(source->source.sstable.klog_cursor) == 0)
{
    if (source->source.sstable.sst->klog_data_end_offset > 0 &&
        source->source.sstable.klog_cursor->current_pos >=
            source->source.sstable.sst->klog_data_end_offset)
    {
        return TDB_ERR_NOT_FOUND;   // <-- fires too early
    }
    ...
}
```

The check itself is correct; the bug is upstream of it — either
`block_manager_cursor_next` is advancing `current_pos` by more than
one entry's worth (likely a block-sized jump), or some shared state
between two SSTable sources corrupts one source's cursor when both are
active simultaneously. The 300×–1800× ratio between expected and
actual pops strongly suggests block-size jumps.

### Why we haven't patched it yet

The fix requires reading `block_manager.c`'s cursor advance logic
carefully — it isn't a one-line patch, and our instrumentation evidence
points to multiple candidate causes. We've stopped here to land what
we have rather than do an unsupported speculative engine edit.

### Workarounds today

- **Single-SSTable case is unaffected.** A small-enough working set
  that never triggers a pre-kill memtable flush will recover cleanly.
- **Avoid two-SSTable post-recovery state** by either: not crashing
  mid-write (graceful shutdown is fine — the engine's
  `Flushing all active memtables before close` path works), or
  pre-flushing via `FLUSH TABLE` so the recovery side has nothing to
  add.
- **Use `tidesdb_unified_memtable=ON`** at your own risk (the bug it
  introduces — catastrophic loss under heavy bulk writes — is worse
  than this one). v0.2.3 defaults to OFF for that reason.

### Reproducing the bug

```bash
cd bench/hammerdb
WARE=10 BUILDVU=4 RUNVU=4 PRE_KILL_SECS=30 ./recovery-diag.sh
# inspect bench/results/recovery-diag-*/snapshots.txt
# look for tables where `orders_by_w` returns ~30k per warehouse
# but `unfiltered` COUNT(*) is far below 308k+
```

The instrumented build that captured the per-source pop counts is
preserved as Docker image `tidesdb/mysql:9.7-instr4` locally (rebuildable
via `docker build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7-instr4`
with the instrumentation diff applied on top of the v0.2.3 patches).

### Affected tests

These tests cover the recovery path and will flag the bug if it
reappears or worsens:

- `bench/hammerdb/recovery-test.sh` — end-to-end recovery verdict
- `bench/hammerdb/recovery-diag.sh` — detailed per-CF diagnostic
- `bench/hammerdb/run-all.sh` step 2 — included in the suite
