/*
  Copyright (c) 2026 TidesDB Corp.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/
#pragma once

/*
  Bounded retry for engine reads that report transient contention.

  TDB_ERR_LOCKED does not mean an operation failed. It means something else
  held what the call needed, nothing was read, and the caller should ask
  again. The engine documents it that way per function -- "TDB_ERR_LOCKED if
  contention left the read unservable and it should be retried" for point
  reads, and "TDB_ERR_LOCKED when descriptor pressure kept a source from
  being read" for iterator steps, which is also why an iterator that reports
  it has not advanced and is safe to re-drive.

  It reaches far more call sites than the name suggests. A scan opens the
  same sstables a point read does, so a get, an existence check, an iterator
  seek and an iterator step can all report it while a compaction moves the
  sources underneath them. Passing that to tdb_rc_to_ha turns routine
  compaction overlap into HA_ERR_LOCK_WAIT_TIMEOUT on a plain SELECT -- a
  failure that survives MTR untouched and only shows up under sustained
  load, which is the worst shape for a bug to have.

  So absorb it here, ahead of the error mapping, and keep the mapping for
  the case where it genuinely will not clear.

  The ladder is bounded on purpose: an engine that is actually stuck must
  still surface an error rather than pin a query thread indefinitely. It
  mirrors the escalation the bulk mid-commit path already uses, with one
  extra tier because descriptor pressure clears on a reaper's schedule
  rather than a single writer's.

  Only reads belong here. A write that reports contention has a conflict
  the SQL layer needs to see, and retrying it silently would hide exactly
  the serialization failure the caller is meant to handle.
*/

#include <chrono>
#include <thread>
#include <utility>

extern "C"
{
#include <tidesdb/db.h>
}

/* 200us, 1ms, 5ms, 20ms -- ~26ms of total dwell across five attempts. */
static constexpr int TDB_READ_RETRY_BACKOFF_US[] = {200, 1000, 5000, 20000};
static constexpr int TDB_READ_RETRY_MAX_ATTEMPTS =
    1 + (int)(sizeof(TDB_READ_RETRY_BACKOFF_US) / sizeof(TDB_READ_RETRY_BACKOFF_US[0]));

/*
  Run fn(), re-running it while it reports TDB_ERR_LOCKED. Any other result,
  success or failure, is returned on the spot. On exhaustion the last rc is
  returned so the caller's tdb_rc_to_ha maps it exactly as it would have
  without the retry.
*/
template <typename Fn>
static inline int tdb_retry_transient(Fn &&fn)
{
    int rc = fn();
    for (int attempt = 0; rc == TDB_ERR_LOCKED && attempt < TDB_READ_RETRY_MAX_ATTEMPTS - 1;
         attempt++)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(TDB_READ_RETRY_BACKOFF_US[attempt]));
        rc = fn();
    }
    return rc;
}

/*
  Thin forwarding wrappers, one per engine entry point that documents
  TDB_ERR_LOCKED. Call sites use these instead of the raw calls, so the
  retry policy has exactly one definition and a new call site inherits it by
  spelling the wrapper's name.

  Deliberately variadic rather than signature-copies: these must keep
  compiling if the engine's parameter lists shift under a version bump,
  which is the change these wrappers exist to survive.

  tidesdb_iter_key and tidesdb_iter_key_value are absent on purpose. They
  read an already-positioned iterator's buffers, do no source I/O, and
  document no TDB_ERR_LOCKED -- wrapping them would imply a contention mode
  they do not have.
*/
#define TDB_DEFINE_READ_RETRY(wrapper, engine_fn)                  \
    template <typename... Args>                                    \
    static inline int wrapper(Args &&...args)                      \
    {                                                              \
        return tdb_retry_transient(                                \
            [&] { return engine_fn(std::forward<Args>(args)...); }); \
    }

TDB_DEFINE_READ_RETRY(tdb_txn_get_r, tidesdb_txn_get)
TDB_DEFINE_READ_RETRY(tdb_iter_new_r, tidesdb_iter_new)
TDB_DEFINE_READ_RETRY(tdb_iter_next_r, tidesdb_iter_next)
TDB_DEFINE_READ_RETRY(tdb_iter_prev_r, tidesdb_iter_prev)
TDB_DEFINE_READ_RETRY(tdb_iter_seek_r, tidesdb_iter_seek)
TDB_DEFINE_READ_RETRY(tdb_iter_seek_to_first_r, tidesdb_iter_seek_to_first)
TDB_DEFINE_READ_RETRY(tdb_iter_seek_to_last_r, tidesdb_iter_seek_to_last)
TDB_DEFINE_READ_RETRY(tdb_iter_seek_for_prev_r, tidesdb_iter_seek_for_prev)

#undef TDB_DEFINE_READ_RETRY
