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
 * EngineContext storage + lifecycle. See tidesdb_engine_context.h for the
 * full ownership story.
 *
 * Extracted from ha_tidesdb.cc as A-4 (engine half) of the architectural
 * follow-up. The previous arrangement -- four file-static globals scattered
 * across ha_tidesdb.cc -- made every subsequent extraction either reach
 * into ha_tidesdb.cc by name or duplicate decls in its own header. This
 * file gives them a single, named home.
 */

#include "tidesdb_engine_context.h"

#include "mysql/psi/mysql_mutex.h"

/* The singleton. File-static would prevent ha_tidesdb.cc and any other
   TU from naming it; we want extern. */
EngineContext g_engine_ctx;

void EngineContext::init()
{
    /* engine, schema_cf, path: default-initialized by the member
       initializers in the struct definition. Nothing to do here for
       those.

       last_conflict_info: zero-init via the {} braced-init in the
       struct definition. Mutex needs explicit init. */
    mysql_mutex_init(0, &last_conflict_mutex, MY_MUTEX_INIT_FAST);
}

void EngineContext::destroy()
{
    /* engine pointer is cleared by the panic / deinit path via
       tdb_set_engine before destroy() is called. We do NOT close
       the engine here -- that's the caller's responsibility (so
       the destroy order can match the rest of the shutdown). */
    mysql_mutex_destroy(&last_conflict_mutex);

    /* Defensive cleanup so a second destroy is a no-op (panic + deinit
       can both fire on a clean shutdown; the second pass should not
       double-destroy the mutex). */
    schema_cf = nullptr;
    /* `path` is std::string -- its destructor runs on program exit. We
       don't clear() it here because some error-log paths in the deinit
       sequence still want the path for diagnostics. */
}
