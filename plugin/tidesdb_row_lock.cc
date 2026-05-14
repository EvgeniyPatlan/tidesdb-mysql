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

/*
 * Pessimistic row-lock manager implementation. Hash-partitioned
 * lock table, deadlock graph walker, acquire (with cond_wait + H-1
 * deadlock re-check), release.
 *
 * Extracted from ha_tidesdb.cc as the third architectural
 * extraction from the follow-up code review. See tidesdb_row_lock.h
 * for the API contract and the borrowed-from-ha_tidesdb.cc symbols
 * this file depends on.
 */

#include "ha_tidesdb.h"  /* tidesdb_trx_t */

#include <cstring>

extern "C" {
#define XXH_INLINE_ALL
#include <tidesdb/xxhash.h>
}

#include "my_dbug.h"
#include "my_sys.h"
#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_mutex.h"
#include "sql/sql_class.h"  /* THD, thd_killed */

#include "tidesdb_compat.h"      /* HA_ERR_* numeric aliases if any drift */
#include "tidesdb_row_lock.h"

/* g_trx_lifecycle_lock is defined in ha_tidesdb.cc (it protects
   tidesdb_trx_t lifetime across all deref paths, not just the
   row-lock walker's; see CF-1 / HF-1 in the follow-up review).
   We take its read side around the walker. */
extern mysql_rwlock_t g_trx_lifecycle_lock;

/* Storage for the public extern in the header. */
tdb_lock_partition_t *lock_partitions = nullptr;

static inline uint tdb_lock_part(const uchar *key, uint len)
{
    uint64_t h = XXH3_64bits(key, len);
    return (uint)(h % ROW_LOCK_PARTITIONS);
}

/* Find or create a lock entry in the partition's hash chain.
   Caller must hold partition mutex. */
static tdb_row_lock_t *tdb_lock_find_or_create(tdb_lock_partition_t *part, uint part_idx,
                                               const uchar *pk, uint pk_len)
{
    for (tdb_row_lock_t *e = part->chain; e; e = e->hash_next)
    {
        if (e->pk_len == pk_len && memcmp(e->pk, pk, pk_len) == 0) return e;
    }
    tdb_row_lock_t *e =
        (tdb_row_lock_t *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(tdb_row_lock_t), MYF(MY_ZEROFILL));
    if (!e) return nullptr;
    e->pk = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, pk_len, MYF(0));
    if (!e->pk)
    {
        my_free(e);
        return nullptr;
    }
    memcpy(e->pk, pk, pk_len);
    e->pk_len = pk_len;
    e->owner_txn_id.store(0, std::memory_order_relaxed);
    e->owner_trx.store(nullptr, std::memory_order_relaxed);
    e->waiters = 0;
    e->partition = part_idx;
    mysql_cond_init(0, &e->cond);
    e->hash_next = part->chain;
    part->chain = e;
    return e;
}

/* Deadlock detection -- walk the wait-for graph with atomic loads.
   Returns true if the requestor waiting on target_lock would create a cycle.

   The walk does not hold any partition mutex; lock and trx structs are never
   freed (locks stay in their hash chain for the lifetime of the plugin; trx
   structs are freed only at connection close, which requires the connection
   to have released all locks first). The owner / waiting_on fields are
   atomic so that cross-partition reads never tear. Racy state observed
   during the walk can produce a stale answer (false positive rare spurious
   deadlock return, application retries; false negative wait and rely on
   the lock-wait-timeout to recover). Neither outcome corrupts memory. */
bool tdb_lock_would_deadlock(tidesdb_trx_t *requestor, tdb_row_lock_t *target_lock)
{
    /* Chain is requestor -> target_lock -> target_lock.owner_trx ->
       owner.waiting_on -> ... -- if we reach the requestor there's a cycle.
       Depth capped at DEADLOCK_MAX_DEPTH to avoid pathological runs.

       Read-locking g_trx_lifecycle_lock prevents the trx structs we deref
       (`cur->waiting_on`) from being my_free()d by a concurrent
       tidesdb_close_connection between our atomic load of the pointer
       and the subsequent dereference. Lock entries themselves are never
       freed by design, so target_lock + cur_waiting are safe. */
    mysql_rwlock_rdlock(&g_trx_lifecycle_lock);
    tidesdb_trx_t *cur = target_lock->owner_trx.load(std::memory_order_acquire);
    bool found_cycle = false;
    for (int depth = 0; depth < DEADLOCK_MAX_DEPTH && cur; depth++)
    {
        if (cur == requestor)
        {
            found_cycle = true;
            break;
        }
        tdb_row_lock_t *cur_waiting = cur->waiting_on.load(std::memory_order_acquire);
        if (!cur_waiting) break; /* not currently waiting */
        cur = cur_waiting->owner_trx.load(std::memory_order_acquire);
    }
    mysql_rwlock_unlock(&g_trx_lifecycle_lock);
    return found_cycle;
}

/*
  Acquire a row lock. Returns 0 on success, HA_ERR_LOCK_DEADLOCK on deadlock.
  Blocks if the row is locked by another transaction (unless deadlock detected).
  Re-entrant -- returns immediately if already held by this txn.

  Deadlock detection runs without the partition mutex held; we publish our
  wait intent (trx->waiting_on) under the mutex so other walkers see us,
  drop the mutex, walk the wait-for graph with atomic loads, then re-acquire.
  This keeps other lockers on the same partition unblocked while we walk
  and removes the data races that the old code had when traversing pointers
  in unrelated partitions.
*/
int row_lock_acquire(tidesdb_trx_t *trx, const uchar *key, uint len, THD *thd)
{
    if (!lock_partitions || !trx) return 0;

    uint part_idx = tdb_lock_part(key, len);
    tdb_lock_partition_t *part = &lock_partitions[part_idx];

    mysql_mutex_lock(&part->mutex);

    tdb_row_lock_t *lock = tdb_lock_find_or_create(part, part_idx, key, len);
    if (!lock)
    {
        mysql_mutex_unlock(&part->mutex);
        return HA_ERR_OUT_OF_MEM;
    }

    /* Already own it? */
    if (lock->owner_txn_id.load(std::memory_order_relaxed) == trx->lock_txn_id &&
        lock->owner_trx.load(std::memory_order_relaxed) == trx)
    {
        mysql_mutex_unlock(&part->mutex);
        return 0;
    }

    /* Free? Claim it. */
    if (lock->owner_txn_id.load(std::memory_order_relaxed) == 0)
    {
        lock->owner_txn_id.store(trx->lock_txn_id, std::memory_order_release);
        lock->owner_trx.store(trx, std::memory_order_release);
        lock->held_next = trx->held_locks_head;
        trx->held_locks_head = lock;
        mysql_mutex_unlock(&part->mutex);
        return 0;
    }

    /* Owned by someone else -- publish our wait intent so concurrent deadlock
       walks that traverse through this trx can see what we're waiting on,
       then drop the mutex to run the walk without blocking our partition. */
    trx->waiting_on.store(lock, std::memory_order_release);
    mysql_mutex_unlock(&part->mutex);

    bool deadlock = tdb_lock_would_deadlock(trx, lock);

    mysql_mutex_lock(&part->mutex);

    if (deadlock)
    {
        trx->waiting_on.store(nullptr, std::memory_order_relaxed);
        mysql_mutex_unlock(&part->mutex);
        return HA_ERR_LOCK_DEADLOCK;
    }

    /* The owner may have released while we were walking. If so, claim the
       lock directly instead of falling through to cond_wait. */
    if (lock->owner_txn_id.load(std::memory_order_relaxed) == 0)
    {
        lock->owner_txn_id.store(trx->lock_txn_id, std::memory_order_release);
        lock->owner_trx.store(trx, std::memory_order_release);
        lock->held_next = trx->held_locks_head;
        trx->held_locks_head = lock;
        trx->waiting_on.store(nullptr, std::memory_order_relaxed);
        mysql_mutex_unlock(&part->mutex);
        return 0;
    }

    /* Still owned -- wait for release. kill_query wakes us by broadcasting
       on lock->cond; we re-check thd_killed() on every wake-up and bail
       with HA_ERR_LOCK_WAIT_TIMEOUT so the client sees a proper error
       instead of hanging until the holder eventually commits.

       We also re-run the deadlock detector on each wake-up: a new
       wait-for cycle can form AFTER the initial pre-wait check (the
       lock holder waited on something we now own, for example). Without
       a re-check, that deadlock turns into a hang until lock_wait_timeout
       rather than the proper ER_LOCK_DEADLOCK. */
    lock->waiters++;
    bool killed = false;
    bool deadlock_in_wait = false;
    while (lock->owner_txn_id.load(std::memory_order_relaxed) != 0 &&
           lock->owner_trx.load(std::memory_order_relaxed) != trx)
    {
        if (thd && thd_killed(thd))
        {
            killed = true;
            break;
        }
        /* Deadlock walker uses atomic loads only -- safe to call with
           part->mutex held. We MUST re-publish waiting_on first so the
           walker (running on another thread) sees the same wait
           intention it saw before we entered cond_wait; the publish
           happened on entry to row_lock_acquire and is still valid. */
        if (tdb_lock_would_deadlock(trx, lock))
        {
            deadlock_in_wait = true;
            break;
        }
        mysql_cond_wait(&lock->cond, &part->mutex);
    }
    lock->waiters--;
    trx->waiting_on.store(nullptr, std::memory_order_relaxed);

    if (killed)
    {
        mysql_mutex_unlock(&part->mutex);
        return HA_ERR_LOCK_WAIT_TIMEOUT;
    }
    if (deadlock_in_wait)
    {
        mysql_mutex_unlock(&part->mutex);
        return HA_ERR_LOCK_DEADLOCK;
    }

    /* We claim the lock */
    lock->owner_txn_id.store(trx->lock_txn_id, std::memory_order_release);
    lock->owner_trx.store(trx, std::memory_order_release);
    lock->held_next = trx->held_locks_head;
    trx->held_locks_head = lock;
    mysql_mutex_unlock(&part->mutex);
    return 0;
}

/*
  Release all row locks held by this transaction.
  Called from tidesdb_commit() and tidesdb_rollback().
*/
void row_locks_release_all(tidesdb_trx_t *trx)
{
    if (!lock_partitions || !trx) return;

    tdb_row_lock_t *lock = trx->held_locks_head;
    while (lock)
    {
        tdb_row_lock_t *next = lock->held_next;
        uint part_idx = lock->partition;
        tdb_lock_partition_t *part = &lock_partitions[part_idx];

        mysql_mutex_lock(&part->mutex);
        lock->owner_txn_id.store(0, std::memory_order_release);
        lock->owner_trx.store(nullptr, std::memory_order_release);
        lock->held_next = nullptr;
        if (lock->waiters > 0) mysql_cond_broadcast(&lock->cond);
        mysql_mutex_unlock(&part->mutex);

        lock = next;
    }
    trx->held_locks_head = nullptr;
    trx->waiting_on.store(nullptr, std::memory_order_relaxed);
}

void tdb_row_lock_init()
{
    lock_partitions = (tdb_lock_partition_t *)my_malloc(
        PSI_NOT_INSTRUMENTED, ROW_LOCK_PARTITIONS * sizeof(tdb_lock_partition_t),
        MYF(MY_ZEROFILL));
    if (lock_partitions)
    {
        for (ulong i = 0; i < ROW_LOCK_PARTITIONS; i++)
        {
            mysql_mutex_init(0, &lock_partitions[i].mutex, MY_MUTEX_INIT_FAST);
            lock_partitions[i].chain = nullptr;
        }
    }
}

void tdb_row_lock_destroy()
{
    if (!lock_partitions) return;
    for (ulong i = 0; i < ROW_LOCK_PARTITIONS; i++)
    {
        /* Free all lock entries in the hash chain. */
        tdb_row_lock_t *e = lock_partitions[i].chain;
        while (e)
        {
            tdb_row_lock_t *next = e->hash_next;
            mysql_cond_destroy(&e->cond);
            my_free(e->pk);
            my_free(e);
            e = next;
        }
        mysql_mutex_destroy(&lock_partitions[i].mutex);
    }
    my_free(lock_partitions);
    lock_partitions = nullptr;
}
