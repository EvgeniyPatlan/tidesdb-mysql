/* Copyright (c) 2026 TidesDB Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. */

/* tidesdb_row_lock.h
 *
 * Pessimistic row-lock manager for the TidesDB MySQL plugin.
 * Extracted from ha_tidesdb.cc as the third architectural extraction
 * recommended in docs/code-review-followup-report.md.
 *
 * What this owns:
 *   - The partitioned hash table of per-row locks (`lock_partitions`)
 *   - Lock entries (`tdb_row_lock_t`) -- one per locked PK ever seen
 *   - Acquire path with deadlock detection (re-checked inside the
 *     cond_wait loop after the H-1 fix)
 *   - Release path called from txn commit / rollback / connection close
 *
 * What it borrows:
 *   - `g_trx_lifecycle_lock` (defined in ha_tidesdb.cc) protects
 *     tidesdb_trx_t pointer dereferences inside the deadlock walker.
 *     The H-3 UAF fix requires that ALL trx-deref paths take this
 *     rwlock; the walker does so, and `tidesdb_close_connection`
 *     takes the write side around `my_free(trx)`.
 *   - `tidesdb_trx_t` is defined in ha_tidesdb.h; we forward-declare
 *     it here.
 *
 * Iteration status:
 *   - First iteration (this file): code moved out of ha_tidesdb.cc.
 *     Types are still visible at top-level so tidesdb_trx_t can have
 *     `held_locks_head` and `waiting_on` members that point at them.
 *   - Future iteration: hide tdb_row_lock_t behind an opaque
 *     RowLockHandle stored on trx; expose only a `RowLockManager`
 *     class API. That removes the last header-visible type leak. */

#pragma once

#include <atomic>
#include <cstdint>

#include "my_inttypes.h"     /* uchar, uint */
#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_mutex.h"

/* Forward-decls. `tidesdb_trx_t` is defined in ha_tidesdb.h. THD is
   forward-declared so this header doesn't need to drag sql_class.h. */
struct tidesdb_trx_t;
class THD;

/* Number of hash partitions for the row lock table.
   65536 partitions (~512 KB overhead) virtually eliminates false contention
   between unrelated rows while keeping memory usage negligible. */
static constexpr ulong ROW_LOCK_PARTITIONS = 65536;

/* Maximum depth for wait-for-graph traversal during deadlock detection.
   Prevents infinite loops on corrupted graph state. */
static constexpr int DEADLOCK_MAX_DEPTH = 100;

/* Row lock entry in the hash table.

   owner_txn_id and owner_trx are read by deadlock graph walks running on
   OTHER threads without holding this partition's mutex -- they must be
   atomic so readers never observe a torn write. All writes still happen
   under the owning partition's mutex.

   Lock entries are never freed once created (only the owner fields reset
   to 0/null on release). This invariant is load-bearing: the deadlock
   walker dereferences `tdb_row_lock_t*` pointers loaded atomically from
   other entries; if entries could be freed concurrently the walker would
   UAF. M-10 (recycle entries) is deferred precisely because of this
   contract. */
struct tdb_row_lock_t
{
    uchar *pk;                              /* heap-allocated PK bytes */
    uint pk_len;                            /* length of PK bytes */
    std::atomic<uint64_t> owner_txn_id;     /* txn that holds this lock (0 = free) */
    std::atomic<tidesdb_trx_t *> owner_trx; /* trx struct of the holder */
    mysql_cond_t cond;                      /* waiters sleep on this */
    uint waiters;                           /* number of threads waiting (mutex-guarded) */
    tdb_row_lock_t *hash_next;              /* next in hash chain (mutex-guarded) */
    tdb_row_lock_t *held_next;              /* next in per-txn held list (owner-only) */
    uint partition;                         /* which partition this belongs to */
};

/* One partition of the lock table */
struct tdb_lock_partition_t
{
    mysql_mutex_t mutex;
    tdb_row_lock_t *chain; /* head of hash chain */
};

/* The global lock-table pointer. Allocated in tdb_row_lock_init,
   freed in tdb_row_lock_destroy. NULL means the subsystem is not
   initialized (acquire/release become no-ops). */
extern tdb_lock_partition_t *lock_partitions;

/* Initialize / tear down the lock-table partitions. Called once each
   from tidesdb_init_func / tidesdb_deinit_func respectively. */
void tdb_row_lock_init();
void tdb_row_lock_destroy();

/* Acquire a row lock. Returns 0 on success, or:
     HA_ERR_LOCK_DEADLOCK     - cycle detected in wait-for graph
     HA_ERR_LOCK_WAIT_TIMEOUT - THD killed while waiting
     HA_ERR_OUT_OF_MEM        - allocation failed
   Re-entrant (returns 0 immediately if already held by `trx`).
   Blocks via mysql_cond_wait when contended. The H-1 fix re-runs the
   deadlock walker on each wake-up. */
int row_lock_acquire(tidesdb_trx_t *trx, const uchar *key, uint len, THD *thd);

/* Release all row locks held by `trx`. Called from
   tidesdb_commit / tidesdb_rollback / tidesdb_close_connection. */
void row_locks_release_all(tidesdb_trx_t *trx);

/* Deadlock graph walker. Exposed so tidesdb_hton_kill_query can call it
   too (if you ever want a proactive deadlock probe rather than just
   waking the victim). Currently used only internally. Returns true if
   adding (requestor -> target_lock) would create a cycle.
   Takes `g_trx_lifecycle_lock` for reading; safe to call from any
   context that doesn't already hold the write side. */
bool tdb_lock_would_deadlock(tidesdb_trx_t *requestor, tdb_row_lock_t *target_lock);
