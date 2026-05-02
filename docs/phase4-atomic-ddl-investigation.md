# Phase 4 atomic DDL — investigation notes

This session attempted to wire up `HTON_SUPPORTS_ATOMIC_DDL`. Both attempts failed at runtime; documenting precisely why so the next session has a clear starting point.

## The contract MySQL expects

When `HTON_SUPPORTS_ATOMIC_DDL` is set on a handlerton:

1. The SQL layer wraps every DDL (`CREATE`/`DROP`/`RENAME`/`ALTER TABLE`) in an implicit transaction via `trans_commit_implicit`.
2. The engine's `create()` / `delete_table()` / `rename_table()` operations get the `dd::Table*` parameter for their corresponding *new* and *old* metadata.
3. After the SQL layer has both updated the data dictionary AND finished its end of the work, it triggers an implicit commit.
4. The commit propagates through `MYSQL_BIN_LOG::commit` → `process_commit_stage_queue` → `finish_transaction_in_engines` → `commit_in_engines` → `ha_commit_low` → `tidesdb_commit`.
5. `handlerton::post_ddl(THD*)` fires once the commit succeeds, giving the engine a chance to finalize anything still pending.
6. On failure: SQL layer compensates by calling `delete_table()` (for failed `CREATE`) or rolling back via `rollback`.

## Attempts and outcomes

### Attempt 1 — just set the flag

Code:
```cpp
tidesdb_hton->flags = HTON_SUPPORTS_ATOMIC_DDL;
tidesdb_hton->post_ddl = [](THD *) { /* no-op */ };
```

Result: SIGSEGV at `ha_tidesdb.cc:2767` during ALTER TABLE's implicit commit.

```
Signal SIGSEGV at address 0x20
 #5 tidesdb_commit at ha_tidesdb.cc:2767                              (int rc = tidesdb_txn_commit(trx->txn);)
 #6 ha_commit_low at sql/handler.cc:2002
 #7 trx_coordinator::commit_in_engines at sql/tc_log.cc:147
 #8 finish_transaction_in_engines at sql/binlog.cc:10742
…
#14 mysql_alter_table at sql/sql_table.cc:19191                       (trans_commit_implicit)
…
Query: ALTER TABLE t1 ADD COLUMN dt6 DATETIME(6)
```

`tidesdb_txn_commit` dereferences something at offset `0x20` that's `NULL` — i.e. the `tidesdb_txn_t` struct passed in has a field zeroed out at that offset, so the call is on a partially-finalized handle.

`tidesdb_commit` does have an early return for `!trx || !trx->txn`, so the *outer* pointer is fine. The poisoned state is *inside* the `tidesdb_txn_t` itself.

### Attempt 2 — eager-free the txn after every commit

Hypothesis: the `needs_reset = true` reuse path leaves a "half-state" handle that the next commit dereferences. If we free the handle eagerly post-commit and let `get_or_create_trx` allocate fresh, the race goes away.

Code (Round 21 of `scripts/replay-port-edits.sh`):
```cpp
// Both successful-commit and read-only-rollback branches:
tidesdb_txn_free(trx->txn);
trx->txn = NULL;
trx->txn_generation++;
trx->needs_reset = false;
```

Result: **same crash at the same line**. The eager-free changes only the post-commit cleanup; the crash happens *inside* `tidesdb_txn_commit` itself, before our new cleanup ever runs. So the bug isn't a stale-handle-reaching-the-second-commit issue. It's that the *first* commit on this particular handle dies inside the library.

## Why we can't make further progress without a debugger

The crash trace's frames 3 and 4 are `<unknown>` — they're inside `libtidesdb.a`, which we linked into `ha_tidesdb.so` *without* `-g` on the TidesDB side. So we can't see *which* internal function in TidesDB is dereferencing `0x20`.

```
 #2 0x71fcb8d5832f <unknown>            ← libc fault handler
 #3 0x71fca8d27baa <unknown>            ← inside tidesdb internal
 #4 0x71fca8d2f86a <unknown>            ← inside tidesdb internal
 #5 tidesdb_commit at ha_tidesdb.cc:2767  ← entry into tidesdb library
```

To make progress we need:

1. **Build TidesDB with full debug symbols**:
    ```bash
    cmake -S tidesdb -B tidesdb/build-debug \
          -DBUILD_SHARED_LIBS=OFF -DTIDESDB_BUILD_TESTS=OFF \
          -DCMAKE_BUILD_TYPE=Debug
    ```
2. **Reproduce under `gdb`** with the test that triggers the crash (test 25 — `ALTER TABLE t1 ADD COLUMN dt6 DATETIME(6)`).
3. **Identify the field at offset `0x20`** in whatever struct the crashing code is dereferencing. Likely candidates: the txn's `db` back-pointer, the WAL buffer, the write-set hashmap.
4. **Fix the actual cause**, which will be one of:
    - A. The handle was already committed by an earlier `tidesdb_txn_commit` call from inside the same `tidesdb_commit`. (Idempotency failure inside TidesDB.)
    - B. The handle was created against a `tidesdb_t*` that's been replaced/closed (highly unlikely — `tdb_global` is set once at init).
    - C. MySQL is calling `tidesdb_commit` on a `THD` whose `trx` came from a *different* engine slot, so `trx->txn` looks plausible but is actually a struct of a different type. (Would need to check `thd_get_ha_data` indexing.)

## Hypothesis I lean toward

(A) — `tidesdb_txn_commit` is non-idempotent. The MySQL atomic-DDL commit path likely makes two calls (one for the prepare-equivalent, one for the actual commit), and the second one crashes. To verify: add a `printf("[tdb] commit pid=%p\n", trx->txn);` at the top of `tidesdb_commit`, run the failing test, see if it prints twice with the same handle.

If confirmed, the upstream fix in TidesDB would be: make `tidesdb_txn_commit` set internal pointers to `NULL` after committing, and check for NULL on entry and return `TDB_ERR_ALREADY_COMMITTED`. Plus `tidesdb_txn_prepare` for real 2PC participation.

## What works *without* the flag

The plugin already passes 30/30 tests including:
- `30_rename_table` — RENAME TABLE works (atomic on its own because `tidesdb_rename_column_family` is atomic in TidesDB)
- `31_alter_add_drop_column` — ALTER ADD COLUMN, UPDATE on new column, ALTER DROP COLUMN all clean
- `25_temporal_types` — includes a mid-test `ALTER TABLE … ADD COLUMN DATETIME(6)`
- `test-persistence.sh` — data survives a full `mysqld` shutdown/restart

So most DDL is *already* effectively atomic at the engine layer; the missing piece is purely the SQL-layer's ability to roll back a partial DDL across the data-dictionary boundary on crash. That's what `HTON_SUPPORTS_ATOMIC_DDL` brings to the table, and getting it requires resolving the txn-commit idempotency issue above.

## Tests parked pending atomic DDL

- `tests/phase2/_29_ddl_rollback_failed_create.sql.disabled` — verifies rollback of a failed CREATE TABLE.

When atomic DDL is fixed, rename back to `.sql` and capture the baseline.
