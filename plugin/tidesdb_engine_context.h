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

/* tidesdb_engine_context.h
 *
 * EngineContext: the plugin's process-level engine state, grouped into
 * a single struct rather than scattered as file-static globals across
 * ha_tidesdb.cc. Architectural extraction A-4 from the follow-up review
 * (https://github.com/EvgeniyPatlan/tidesdb-mysql/blob/main/docs/code-review-followup-report.md).
 *
 * Members owned today:
 *   - engine          -- the TidesDB engine handle, atomic so shutdown
 *                        races (C-2) stay safe across handler threads.
 *   - schema_cf       -- the schema column family pointer (used by the
 *                        replica / object-store paths to materialize
 *                        DD entries from the shared store).
 *   - path            -- the resolved data directory; computed in
 *                        tidesdb_init_func from either srv_data_home_dir
 *                        or a sibling of mysql_real_data_home.
 *   - last_conflict_* -- the buffer + mutex powering the "Last conflict"
 *                        line under SHOW ENGINE TIDESDB STATUS.
 *
 * What this does NOT own (yet):
 *   - g_trx_lifecycle_lock     -- still defined in ha_tidesdb.cc;
 *                                 referenced by the row-lock module and
 *                                 the kill_query / close_connection
 *                                 code paths.
 *   - The row-lock partitioned table -- owned by tidesdb_row_lock.{h,cc}.
 *   - Master-key state -- owned by tidesdb_master_key.{h,cc}.
 *   - The MYSQL_SYSVAR_* block -- can't be moved because the macros
 *                                 expand to file-static storage that
 *                                 the plugin registration array points
 *                                 at by name.
 *
 * What this enables:
 *   - tdb_get_engine() / tdb_set_engine() are now header-inline and
 *     callable from any TU, so future extractions (FTS, TidesStore)
 *     can use the engine handle without further globals.
 *   - When subsystems start taking `EngineContext&` parameters rather
 *     than reaching for the global instance, this is the type they
 *     accept. The architect's destination state is "subsystems
 *     parameterized on context"; this header is the foundation. */

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

#include "mysql/psi/mysql_mutex.h"

/* Forward declaration: the engine-wide logical tablespace pointer is
   stashed here at plugin init so the atomic-DDL SDI callbacks can refer
   to it by identity. The class itself lives in sql/plugin_table.h; we
   forward-declare to avoid pulling a server header from every TU that
   only touches engine state. */
class Plugin_tablespace;

/* TidesDB C types are typedef'd through opaque structs whose tag names
   are not stable; rather than fragile forward declarations, pull the
   real SDK header. It's already in every TU that touches engine state. */
extern "C" {
#include <tidesdb/db.h>
}

/* Atomic-DDL participation surface. Pulling the header in here keeps
   std::unique_ptr<SdiStore>'s destructor instantiable without a
   header-bloat dance (no explicit out-of-line ~EngineContext required). */
#include "tidesdb_atomic_ddl.h"

struct EngineContext
{
    /* The engine handle.  Atomic + acquire/release because plugin
       shutdown clears it (panic / deinit) while connections are still
       running DML through it.  See C-2 in docs/code-review-report.md
       for the race this guards. */
    std::atomic<tidesdb_t *> engine{nullptr};

    /* Schema-CF pointer used by the object-store / replica path. NULL
       outside that mode. Read-only after init; no synchronization. */
    tidesdb_column_family_t *schema_cf{nullptr};

    /* Resolved data directory.  Computed once in tidesdb_init_func
       and not mutated after; reads do not need synchronization. */
    std::string path;

    /* "Last conflict" diagnostic surfaced under SHOW ENGINE TIDESDB
       STATUS.  The mutex serializes the writer (any connection that
       hits a TidesDB conflict) and the reader (any SHOW STATUS). */
    mysql_mutex_t last_conflict_mutex;
    static constexpr std::size_t LAST_CONFLICT_INFO_LEN = 1024;
    char last_conflict_info[LAST_CONFLICT_INFO_LEN]{};

    /* Atomic-DDL: SDI store (the dedicated __tidesdb_sdi CF wrapper) is
       allocated lazily by Task 5's wiring; nullptr until then. unique_ptr
       so the destructor releases it on plugin unload. */
    std::unique_ptr<tidesdb_mysql::SdiStore> sdi;

    /* Atomic-DDL: pointer to the engine-wide logical tablespace
       descriptor. Populated in tidesdb_init_func by register_tablespace().
       Used by the (future) SDI callbacks as identity for the
       tidesdb_system tablespace. The object is a function-local static
       owned by register_tablespace(); EngineContext does NOT own it. */
    const Plugin_tablespace *tablespace{nullptr};

    /* Lifecycle. init() is called from tidesdb_init_func once at plugin
       load; destroy() from tidesdb_deinit_func / tidesdb_hton_panic.
       Idempotent destroy is acceptable -- panic and deinit can both
       fire on a clean shutdown path; whichever runs first wins. */
    void init();
    void destroy();

    /* Atomic-DDL teardown: release per-engine resources whose lifetime is
       bounded by the open TidesDB handle. MUST be called BEFORE
       tidesdb_close(engine), otherwise the SdiStore destructor would
       drop a column-family handle owned by an already-freed engine.
       Idempotent: a second call is a no-op (unique_ptr / nullptr). */
    void reset();
};

/* The one and only instance.  Defined in tidesdb_engine_context.cc. */
extern EngineContext g_engine_ctx;

/* Backwards-compatible accessors. Existing call sites use these names
   (50+ of them); routing them through the context preserves the diff
   surface area for this extraction while giving future code the option
   of going through g_engine_ctx directly. */
inline tidesdb_t *tdb_get_engine()
{
    return g_engine_ctx.engine.load(std::memory_order_acquire);
}

inline void tdb_set_engine(tidesdb_t *p)
{
    g_engine_ctx.engine.store(p, std::memory_order_release);
}
