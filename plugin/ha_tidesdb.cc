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
#include "ha_tidesdb.h"

extern "C"
{
#define XXH_INLINE_ALL
#include <tidesdb/xxhash.h>
#ifdef TIDESDB_WITH_S3
    tidesdb_objstore_t *tidesdb_objstore_s3_create(const char *endpoint, const char *bucket,
                                                   const char *prefix, const char *access_key,
                                                   const char *secret_key, const char *region,
                                                   int use_ssl, int use_path_style);
#endif
}

#include <ft_global.h>
#include <mysql/plugin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <climits>     /* PATH_MAX for HF-3 realpath buffer */
#include <cstdlib>     /* realpath() for HF-3 confinement check */
#include "sql/auth/auth_acls.h"      /* SELECT_ACL for M-12 privilege check */
#include "sql/auth/sql_security_ctx.h" /* Security_context::check_access */
#include "sql/item.h"  /* Item::val_bool() for ICP evaluation */
#include "sql/key.h"
#include "sql/sql_class.h"

#include "tidesdb_engine_context.h"  /* g_engine_ctx, tdb_get_engine/set_engine */
#include "tidesdb_fts.h"             /* FTS subsystem (A-2 extraction) */

/* See plugin/tidesdb_compat.h's "Category 7: Logging shims" comment
 * for why sql_print_information/warning/error keep their stderr
 * implementation rather than routing through MySQL's structured log
 * APIs. Short version: LogEvent / LogPluginErrMsg need the `log_bs`
 * service registry that MODULE_ONLY plugins do not get linked, and
 * my_plugin_log_message needs a st_plugin_int* handle that storage-
 * engine inits never receive (they get the handlerton instead). */

/* MariaDB exposes per-table options via TABLE_SHARE::option_struct (an
 * engine-defined struct populated from CREATE TABLE syntax via
 * ha_create_table_option[] registration). MySQL 9.7 has no equivalent;
 * engine-specific table options come through the ENGINE_ATTRIBUTE /
 * SECONDARY_ENGINE_ATTRIBUTE JSON fields.
 *
 * TDB_TABLE_OPTIONS(tbl) delegates to tidesdb_opts_for_table() which
 * seeds the result from session defaults and then applies the JSON
 * overrides parsed from TABLE_SHARE::engine_attribute. The pointer is
 * thread-local and valid until the next TDB_TABLE_OPTIONS call on the
 * same thread; every use site consumes it immediately, so this is safe.
 *
 * Returns NULL only for tables without a TABLE_SHARE -- existing
 * `if (opts && opts->X)` null-checks throughout the engine still
 * short-circuit cleanly in that edge case. */
struct ha_table_option_struct;
struct ha_index_option_struct;
struct ha_field_option_struct;
static ha_table_option_struct *tidesdb_opts_for_table(const TABLE *table);
#define TDB_TABLE_OPTIONS(tbl)    tidesdb_opts_for_table(tbl)
#define TDB_INDEX_OPTIONS(keyref) (static_cast<ha_index_option_struct *>(nullptr))

/* tdb_sanitize_for_log is declared in ha_tidesdb.h so the FTS TU
   (tidesdb_fts.cc) can reach it for sanitizing the user-supplied
   stop-word table_spec before logging. Defined alongside the path
   validation helpers later in this file. */
#define TDB_FIELD_OPTIONS(fldref) (static_cast<ha_field_option_struct *>(nullptr))

/* Forward-declared for tdb_rc_to_ha(); defined with sysvars below */
static my_bool srv_print_all_conflicts = 0;
static my_bool srv_pessimistic_locking = 0;
/* last_conflict_mutex / last_conflict_info / LAST_CONFLICT_INFO_LEN
   moved to plugin/tidesdb_engine_context.{h,cc} (architectural
   extraction A-4). All references go through g_engine_ctx. */

/* Reader-writer lock protecting tidesdb_trx_t lifetime against the
   deadlock-graph walker.
 *
 * The deadlock walker (tdb_lock_would_deadlock) traverses pointers to
 * tidesdb_trx_t structs loaded atomically from lock entries. Each such
 * pointer points to a connection's trx struct, which tidesdb_close_connection
 * my_free()s when the connection closes -- without any prior synchronization
 * with the walker. A walker that loaded the pointer just before the free
 * would then dereference a freed struct (UAF).
 *
 * Resolution: walker takes the read lock for the duration of the walk;
 * close_connection takes the write lock around my_free. Walks are fast and
 * read locks compose freely; close_connection is rare. PSI key registered
 * for performance schema visibility.
 *
 * MF-4 lock-order invariant (DO NOT VIOLATE):
 *
 *   acquisition order:  partition->mutex  ->  g_trx_lifecycle_lock
 *
 * The H-1 deadlock re-check inside row_lock_acquire's cond_wait loop
 * calls tdb_lock_would_deadlock while holding part->mutex. The walker
 * then takes g_trx_lifecycle_lock as a read-lock. tidesdb_close_connection
 * takes the write-lock around my_free WITHOUT holding any partition
 * mutex (row_locks_release_all has already run and released them).
 * As long as no code path acquires part->mutex while holding the
 * write-side of g_trx_lifecycle_lock, there is no cycle.
 *
 * Future maintainers: if you add a partition-mutex acquisition inside
 * tidesdb_close_connection, OR add code that runs under the write-side
 * of this rwlock and touches partition mutexes, you have created a
 * deadlock. Move row_locks_release_all earlier, or restructure to
 * release the rwlock before touching partitions. */
/* External linkage so the row-lock module (tidesdb_row_lock.cc, extracted
   in the architectural refactor) can reach the lifecycle rwlock from its
   own translation unit. Used internally by the deadlock walker and by
   tidesdb_close_connection / tidesdb_hton_kill_query to gate trx-pointer
   dereferences vs concurrent my_free. */
mysql_rwlock_t g_trx_lifecycle_lock;
PSI_rwlock_key g_trx_lifecycle_lock_key;

/* HF-2 fix (follow-up review): the former process-global
 * g_fts_meta_mutex has moved into TidesDB_share::fts_meta_mutex
 * (see ha_tidesdb.h). Each table now has its own mutex; two unrelated
 * tables no longer contend on a single process-wide lock when both
 * have FTS indexes. Lost-update protection still applies (the meta
 * key is per-CF / per-share, so per-share granularity is sufficient).
 *
 * Original race documentation, preserved for context: fts_update_meta
 * does a load -> mutate -> put on a single counter key inside the
 * caller's transaction. Two concurrent writers each read the same
 * pre-update value, each increment, both commit -- one increment
 * is lost. Per-share mutex serializes the RMW. Under SNAPSHOT or
 * stronger isolation, TidesDB's commit-time conflict detection also
 * catches the case; under READ_COMMITTED the mutex still serializes
 * the read+put pair but cannot make the load see a not-yet-committed
 * write (residual race documented as future structural work). */

/*
  Map TidesDB library error codes to MySQL handler error codes.
  Transient errors (conflict, lock contention, memory pressure) are mapped
  to HA_ERR_LOCK_DEADLOCK so that the application receives ER_LOCK_DEADLOCK
  (1213) and can retry in its own retry loop, instead of seeing the opaque
  HA_ERR_GENERIC / ER_GET_ERRNO 1030.
*/
static int tdb_rc_to_ha(int rc, const char *ctx)
{
    switch (rc)
    {
        case TDB_SUCCESS:
            return 0;

        /* Transient concurrency errors -- mapped to deadlock so MySQL
           rolls back the transaction and the application can retry. */
        case TDB_ERR_CONFLICT:
            if (unlikely(srv_print_all_conflicts))
            {
                sql_print_information(
                    "[TIDESDB] %s: transaction aborted due to write-write "
                    "conflict (TDB_ERR_CONFLICT)",
                    ctx);
                mysql_mutex_lock(&g_engine_ctx.last_conflict_mutex);
                snprintf(g_engine_ctx.last_conflict_info, sizeof(g_engine_ctx.last_conflict_info), "Last conflict: %s at %ld",
                         ctx, (long)time(NULL));
                mysql_mutex_unlock(&g_engine_ctx.last_conflict_mutex);
            }
            return HA_ERR_LOCK_DEADLOCK;

        /* Lock wait timeout -- rolls back the current statement only
           (not the whole transaction), less disruptive than full deadlock. */
        case TDB_ERR_LOCKED:
            return HA_ERR_LOCK_WAIT_TIMEOUT;

        /* Memory pressure -- retriable, back off and let flush/compaction
           free memory. mapped to deadlock so MariaDB retries. */
        case TDB_ERR_MEMORY_LIMIT:
            return HA_ERR_LOCK_DEADLOCK;

        case TDB_ERR_MEMORY:
            sql_print_error("[TIDESDB] %s: TDB_ERR_MEMORY (-1)", ctx);
            return HA_ERR_OUT_OF_MEM;

        case TDB_ERR_NOT_FOUND:
            return HA_ERR_KEY_NOT_FOUND;

        case TDB_ERR_EXISTS:
            return HA_ERR_FOUND_DUPP_KEY;

        case TDB_ERR_READONLY:
            return HA_ERR_READ_ONLY_TRANSACTION;

        /* I/O and corruption errors -- table needs repair/recovery.
           matches InnoDB's mapping of DB_CORRUPTION to HA_ERR_CRASHED. */
        case TDB_ERR_IO:
            sql_print_error("[TIDESDB] %s: I/O error (TDB_ERR_IO)", ctx);
            return HA_ERR_CRASHED;

        case TDB_ERR_CORRUPTION:
            sql_print_error("[TIDESDB] %s: data corruption detected (TDB_ERR_CORRUPTION)", ctx);
            return HA_ERR_CRASHED;

        /* Row too large for the configured block/value size. */
        case TDB_ERR_TOO_LARGE:
            return HA_ERR_TOO_BIG_ROW;

        /* Database handle invalid (closed or never opened). */
        case TDB_ERR_INVALID_DB:
            sql_print_error("[TIDESDB] %s: invalid database handle (TDB_ERR_INVALID_DB)", ctx);
            return HA_ERR_INTERNAL_ERROR;

        /* Invalid arguments -- programming error in the plugin. */
        case TDB_ERR_INVALID_ARGS:
            sql_print_error("[TIDESDB] %s: invalid arguments (TDB_ERR_INVALID_ARGS)", ctx);
            return HA_ERR_INTERNAL_ERROR;

        default:
            sql_print_warning("[TIDESDB] %s: unexpected TidesDB error rc=%d", ctx, rc);
            return HA_ERR_GENERIC;
    }
}

/*
  Dispatch to tidesdb_txn_single_delete or tidesdb_txn_delete based on
  use_single_delete.  Secondary-index delete sites pass true because the
  single-delete contract (at most one put between single-deletes on the
  same key) holds by construction for (col_values, pk) / (term, pk) /
  (hilbert, pk) composites.  Primary-CF delete sites pass the cached
  value of the tidesdb_single_delete_primary session variable, which
  defaults off and is the caller's explicit promise that the session
  does no UPDATE on non-PK columns and no REPLACE INTO / IODKU overwrite
  path on no-secondary tables.
*/
static inline int tidesdb_txn_delete_cf(tidesdb_txn_t *txn, tidesdb_column_family_t *cf,
                                        const uint8_t *key, size_t key_size, bool use_single_delete)
{
    return use_single_delete ? tidesdb_txn_single_delete(txn, cf, key, key_size)
                             : tidesdb_txn_delete(txn, cf, key, key_size);
}

/* MariaDB data directory */
extern MYSQL_PLUGIN_IMPORT char mysql_real_data_home[];

/* engine handle / schema_cf / path moved to EngineContext (see
   plugin/tidesdb_engine_context.{h,cc}). tdb_get_engine and
   tdb_set_engine accessors are now header-inline and reach
   g_engine_ctx.engine. The header itself is included near the top
   of this file alongside the other plugin-internal includes. */

static handlerton *tidesdb_hton;

/* ******************** Plugin-level row lock table ******************** */
/*
  Hash-table-based row-level lock manager with deadlock detection.

  Design:
  -- Partitioned hash table       hash(pk_bytes) % NUM_PARTITIONS -> partition
  -- Each partition has its own mutex protecting a linked-list hash chain
  -- Lock entries track           owner txn, PK bytes, condition variable for waiters
  -- Deadlock detection           on wait, walk the wait-for graph (waiter->holder->waiter)
     If a cycle is found, return HA_ERR_LOCK_DEADLOCK immediately
  -- Per-txn held_locks list for bulk release at COMMIT/ROLLBACK

  This gives per-row granularity without false sharing, proper blocking
  semantics for SELECT ... FOR UPDATE, and deadlock resolution for
  multi-table transactions.
*/

/* Row-lock manager (lock-table partitions, deadlock walker, acquire,
   release, init/destroy) moved to tidesdb_row_lock.{h,cc} during the
   architecture-extraction pass. Symbols still visible:

     ROW_LOCK_PARTITIONS, DEADLOCK_MAX_DEPTH       -- constants
     struct tdb_row_lock_t, tdb_lock_partition_t  -- types (held_locks_head
                                                     on tidesdb_trx_t still
                                                     references these)
     lock_partitions                              -- the global table
     row_lock_acquire / row_locks_release_all     -- public API
     tdb_lock_would_deadlock                      -- walker (exposed)
     tdb_row_lock_init / tdb_row_lock_destroy     -- lifecycle hooks */
#include "tidesdb_row_lock.h"

static handler *tidesdb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       bool partitioned, MEM_ROOT *mem_root);
static void tidesdb_refresh_status_vars();

/* Forward declarations for the tombstone aggregates so tidesdb_show_status
   (defined earlier than the storage block) can read them. */
static long long srv_stat_total_tombstones;
static double srv_stat_tombstone_ratio;
static double srv_stat_max_sst_density;
static long long srv_stat_max_sst_density_level;

/* File extensions -- TidesDB manages its own files */
static const char *ha_tidesdb_exts[] = {NullS};

/* Full-Text Search subsystem moved to tidesdb_fts.{h,cc} during the
   A-2 architectural extraction. ha_tidesdb.cc keeps only the call
   sites (ft_init_ext / ft_read / write_row / update_row / delete_row
   FTS branches). Symbols still visible at module scope via
   tidesdb_fts.h: fts_build_key, fts_build_value, fts_load_meta,
   fts_update_meta, fts_tokenize, fts_extract_and_tokenize,
   fts_parse_boolean, fts_phrase_in_tokens, is_fts_index,
   tdb_ft_info_t, tdb_fts_result_t, tdb_ft_vft, tdb_ft_vft_ext,
   tdb_fts_blend_chars_update, tdb_ft_stopword_table_update,
   tdb_fts_init, tdb_fts_destroy. */

/* ******************** Spatial Index helpers ******************** */

static inline bool is_spatial_index(const KEY *ki)
{
    return ki->algorithm == HA_KEY_ALG_RTREE;
}

/* MBR (Minimum Bounding Rectangle) for spatial predicates */
struct tdb_mbr_t
{
    double xmin, ymin, xmax, ymax;
};

/* Hilbert curve constants */
static constexpr uint HILBERT_ORDER = 32;                           /* bits per axis */
static constexpr uint HILBERT_DIM = 2;                              /* 2D curve (x, y) */
static constexpr uint64_t HILBERT_N = (uint64_t)1 << HILBERT_ORDER; /* 2^32 */
static constexpr uint SPATIAL_HILBERT_KEY_LEN = 8;                  /* 64-bit Hilbert value */
static constexpr uint SPATIAL_MBR_VALUE_LEN = 32;                   /* 4 doubles */

/* Convert IEEE 754 double to a uint32 that preserves sort order under
   unsigned integer comparison.  Handles negative values correctly by
   flipping all bits (negative doubles have sign bit set in IEEE 754;
   flipping makes them sort before positive values). */
static inline uint32_t double_to_lex_uint32(double val)
{
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    if (bits & IEEE754_DOUBLE_SIGN_MASK)
        bits = ~bits; /* negative, flip all bits */
    else
        bits ^= IEEE754_DOUBLE_SIGN_MASK;           /* positive, flip sign bit only */
    return (uint32_t)(bits >> LEX_UINT32_HI_SHIFT); /* top 32 bits for precision */
}

/* Hilbert curve, rotate quadrant coordinates.  Implements the inner
   rotation step of the iterative xy2d transform from Skilling 2004
   ("Programming the Hilbert curve") -- see also the canonical
   Wikipedia pseudocode at https://en.wikipedia.org/wiki/Hilbert_curve.
   The literal (n - 1) is the standard reflection around the centre
   of an n-cell axis; rx and ry carry the binary quadrant flags from
   the caller.

   `n` is uint64_t because the caller (hilbert_xy2d_64) needs to pass
   2^32 on the first iteration of a 32-bit-per-axis curve, which doesn't
   fit in uint32_t. The previous signature truncated 2^32 to 0 on the
   first call, then computed `n - 1 = UINT32_MAX` causing every
   high-bit input pair to encode wrong. */
static inline void hilbert_rot(uint64_t n, uint32_t *x, uint32_t *y, uint32_t rx, uint32_t ry)
{
    if (ry == 0)
    {
        if (rx == 1)
        {
            *x = (uint32_t)(n - 1) - *x;
            *y = (uint32_t)(n - 1) - *y;
        }
        uint32_t t = *x;
        *x = *y;
        *y = t;
    }
}

/* Convert 2D coordinates (x, y) to a 64-bit Hilbert curve value.  Order
   32, each axis 32-bit precision, output 64-bit.  Iterative algorithm
   per Skilling 2004 / Wikipedia, O(32) loop, no recursion.  The literal
   `3` and the XOR encode the four-quadrant visit order of the Hilbert
   d-value: (rx, ry) = (0,0)->0, (0,1)->1, (1,1)->2, (1,0)->3.  The
   `s << 1` doubles s so hilbert_rot receives the full sub-grid size
   for this level, not the half-size step. */
static uint64_t hilbert_xy2d_64(uint32_t x, uint32_t y)
{
    uint64_t d = 0;
    for (uint64_t s = HILBERT_N >> 1; s > 0; s >>= 1)
    {
        uint32_t rx = (x & s) > 0 ? 1 : 0;
        uint32_t ry = (y & s) > 0 ? 1 : 0;
        d += s * s * (uint64_t)((3 * rx) ^ ry);
        /* s << 1 stays 64-bit; previous (uint32_t)s << 1 truncated the
           top bit on the first iteration (s = 2^31 -> 0x80000000, shifted
           left in uint32_t becomes 0). hilbert_rot now takes uint64_t n. */
        hilbert_rot(s << 1, &x, &y, rx, ry);
    }
    return d;
}

/* Store uint64 as 8-byte big-endian (for lexicographic ordering in LSM).
   Most significant byte first so that memcmp on the encoded bytes matches
   the natural numeric ordering of the Hilbert value. */
static inline void encode_hilbert_be(uint64_t h, uchar *out)
{
    for (uint i = 0; i < SPATIAL_HILBERT_KEY_LEN; i++)
        out[i] = (uchar)(h >> ((SPATIAL_HILBERT_KEY_LEN - 1 - i) * BITS_PER_BYTE));
}

/* Decode 8-byte big-endian uint64 */
static inline uint64_t decode_hilbert_be(const uchar *in)
{
    uint64_t h = 0;
    for (uint i = 0; i < SPATIAL_HILBERT_KEY_LEN; i++) h = (h << BITS_PER_BYTE) | (uint64_t)in[i];
    return h;
}

/* WKB geometry type constants */
static constexpr uint32_t WKB_POINT = 1;
static constexpr uint32_t WKB_LINESTRING = 2;
static constexpr uint32_t WKB_POLYGON = 3;
static constexpr uint32_t WKB_MULTIPOINT = 4;
static constexpr uint32_t WKB_MULTILINESTRING = 5;
static constexpr uint32_t WKB_MULTIPOLYGON = 6;
static constexpr uint32_t WKB_GEOMETRYCOLLECTION = 7;

/* Limits to reject malformed WKB data */
static constexpr uint32_t WKB_MAX_POINTS = 1000000;
static constexpr uint32_t WKB_MAX_RINGS = 10000;
static constexpr uint32_t WKB_MAX_GEOMS = 100000;
static constexpr uint SPATIAL_SRID_SIZE = 4;
static constexpr uint SPATIAL_WKB_HEADER_SIZE = 5;  /* 1 byte_order + 4 type */
static constexpr uint SPATIAL_POINT_DATA_SIZE = 16; /* 2 doubles (x, y) */

/* WKB encodes count fields (point count, ring count, geometry count) as
   uint32_t.  Used by every collection-shaped geometry type. */
static constexpr uint WKB_COUNT_SIZE = sizeof(uint32_t);

/* Parts-of-MBR encoding.  spatial_build_value writes [xmin,ymin,xmax,ymax]
   as native doubles, and spatial_parse_query_mbr reads MariaDB's
   [xmin,xmax,ymin,ymax] layout.  Sentinels for offset arithmetic. */
static constexpr uint MBR_DOUBLE_SIZE = sizeof(double);
static constexpr uint MBR_OFFSET_SECOND = 1 * MBR_DOUBLE_SIZE;
static constexpr uint MBR_OFFSET_THIRD = 2 * MBR_DOUBLE_SIZE;
static constexpr uint MBR_OFFSET_FOURTH = 3 * MBR_DOUBLE_SIZE;

/* Read a coordinate pair from WKB and expand MBR.
   Advances pp by SPATIAL_POINT_DATA_SIZE bytes.
   Skips NaN/Inf coordinates. */
static inline bool wkb_read_point(const uchar *&pp, const uchar *ee, double &mn_x, double &mn_y,
                                  double &mx_x, double &mx_y)
{
    if (pp + SPATIAL_POINT_DATA_SIZE > ee) return false;
    double x, y;
    x = float8get(pp);
    y = float8get(pp + MBR_DOUBLE_SIZE);
    pp += SPATIAL_POINT_DATA_SIZE;
    if (std::isfinite(x) && std::isfinite(y))
    {
        if (x < mn_x) mn_x = x;
        if (x > mx_x) mx_x = x;
        if (y < mn_y) mn_y = y;
        if (y > mx_y) mx_y = y;
    }
    return true;
}

/* Read a point sequence ([num_points 4B][x,y pairs...]) and expand MBR.
   Used by LINESTRING and each POLYGON ring. */
static inline bool wkb_read_point_sequence(const uchar *&pp, const uchar *ee, double &mn_x,
                                           double &mn_y, double &mx_x, double &mx_y)
{
    if (pp + WKB_COUNT_SIZE > ee) return false;
    uint32_t n_pts;
    memcpy(&n_pts, pp, WKB_COUNT_SIZE);
    pp += WKB_COUNT_SIZE;
    if (n_pts > WKB_MAX_POINTS) return false;
    for (uint32_t i = 0; i < n_pts; i++)
    {
        if (!wkb_read_point(pp, ee, mn_x, mn_y, mx_x, mx_y)) return false;
    }
    return true;
}

/* Recursive WKB geometry parser.  Reads one geometry object from pp,
   expanding the MBR to include all coordinate pairs.  Advances pp past
   the consumed bytes.  Supports all 7 OGC geometry types. */
static bool wkb_parse_geometry(const uchar *&pp, const uchar *ee, double &mn_x, double &mn_y,
                               double &mx_x, double &mx_y)
{
    if (pp + SPATIAL_WKB_HEADER_SIZE > ee) return false;
        /* MariaDB stores WKB in native byte order, so the leading byte is the
           native endianness marker (0 = big, 1 = little).  We rely on native
           order for the memcpy reads of the geometry type and coordinates
           below; if MariaDB ever changed to store non-native WKB, this assert
           would fire instead of silently returning garbage MBRs.  Release
           builds simply trust the convention. */
#ifndef DBUG_OFF
    {
        const uint32_t endian_probe = 1;
        uchar native_byte_order = *(const uchar *)&endian_probe; /* 1 on LE, 0 on BE */
        DBUG_ASSERT(*pp == native_byte_order);
    }
#endif
    pp++; /* we skip byte_order (MariaDB stores in native order) */
    uint32_t gt;
    memcpy(&gt, pp, WKB_COUNT_SIZE);
    pp += WKB_COUNT_SIZE;

    switch (gt)
    {
        case WKB_POINT:
            return wkb_read_point(pp, ee, mn_x, mn_y, mx_x, mx_y);

        case WKB_LINESTRING:
            return wkb_read_point_sequence(pp, ee, mn_x, mn_y, mx_x, mx_y);

        case WKB_POLYGON:
        {
            if (pp + WKB_COUNT_SIZE > ee) return false;
            uint32_t n_rings;
            memcpy(&n_rings, pp, WKB_COUNT_SIZE);
            pp += WKB_COUNT_SIZE;
            if (n_rings > WKB_MAX_RINGS) return false;
            for (uint32_t r = 0; r < n_rings; r++)
            {
                if (!wkb_read_point_sequence(pp, ee, mn_x, mn_y, mx_x, mx_y)) return false;
            }
            return true;
        }

        case WKB_MULTIPOINT:
        case WKB_MULTILINESTRING:
        case WKB_MULTIPOLYGON:
        case WKB_GEOMETRYCOLLECTION:
        {
            if (pp + WKB_COUNT_SIZE > ee) return false;
            uint32_t n_geoms;
            memcpy(&n_geoms, pp, WKB_COUNT_SIZE);
            pp += WKB_COUNT_SIZE;
            if (n_geoms > WKB_MAX_GEOMS) return false;
            for (uint32_t i = 0; i < n_geoms; i++)
            {
                if (!wkb_parse_geometry(pp, ee, mn_x, mn_y, mx_x, mx_y)) return false;
            }
            return true;
        }

        default:
            return false;
    }
}

/* Extract MBR from a GEOMETRY field's raw data (SRID prefix + WKB).
   Supports all OGC geometry types.  Rejects malformed data and
   coordinates with NaN/Inf values.
   Returns true on success, false on malformed data. */
static bool spatial_compute_mbr(const uchar *data, size_t len, double *xmin, double *ymin,
                                double *xmax, double *ymax)
{
    if (len < SPATIAL_SRID_SIZE + SPATIAL_WKB_HEADER_SIZE) return false;

    const uchar *p = data + SPATIAL_SRID_SIZE;
    const uchar *end = data + len;

    *xmin = *ymin = DBL_MAX;
    *xmax = *ymax = -DBL_MAX;

    if (!wkb_parse_geometry(p, end, *xmin, *ymin, *xmax, *ymax)) return false;

    /* We validate that we actually accumulated valid coordinates */
    return *xmin <= *xmax && *ymin <= *ymax;
}

/* Build spatial index key ( [hilbert_value 8B BE][pk_bytes] )
   Returns total key length. */
static uint spatial_build_key(double cx, double cy, const uchar *pk, uint pk_len, uchar *out)
{
    uint32_t qx = double_to_lex_uint32(cx);
    uint32_t qy = double_to_lex_uint32(cy);
    uint64_t h = hilbert_xy2d_64(qx, qy);
    encode_hilbert_be(h, out);
    memcpy(out + SPATIAL_HILBERT_KEY_LEN, pk, pk_len);
    return SPATIAL_HILBERT_KEY_LEN + pk_len;
}

/* Build spatial index value( [xmin 8B][ymin 8B][xmax 8B][ymax 8B] ) = 32 bytes.
   Stored as native doubles (little-endian on x86). */
static void spatial_build_value(double xmin, double ymin, double xmax, double ymax, uchar *out)
{
    memcpy(out, &xmin, MBR_DOUBLE_SIZE);
    memcpy(out + MBR_OFFSET_SECOND, &ymin, MBR_DOUBLE_SIZE);
    memcpy(out + MBR_OFFSET_THIRD, &xmax, MBR_DOUBLE_SIZE);
    memcpy(out + MBR_OFFSET_FOURTH, &ymax, MBR_DOUBLE_SIZE);
}

/* Parse MBR from MariaDB's spatial key buffer.
   MariaDB format( [xmin 8B][xmax 8B][ymin 8B][ymax 8B] )*/
static void spatial_parse_query_mbr(const uchar *key, tdb_mbr_t *mbr)
{
    mbr->xmin = float8get(key);
    mbr->xmax = float8get(key + MBR_OFFSET_SECOND);
    mbr->ymin = float8get(key + MBR_OFFSET_THIRD);
    mbr->ymax = float8get(key + MBR_OFFSET_FOURTH);
}

/* MBR spatial predicates -- match MariaDB MBR class semantics exactly */
static inline bool mbr_intersects(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return !(a->xmax < b->xmin || a->xmin > b->xmax || a->ymax < b->ymin || a->ymin > b->ymax);
}

static inline bool mbr_contains(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return b->xmin >= a->xmin && b->xmax <= a->xmax && b->ymin >= a->ymin && b->ymax <= a->ymax;
}

static inline bool mbr_within(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return a->xmin >= b->xmin && a->xmax <= b->xmax && a->ymin >= b->ymin && a->ymax <= b->ymax;
}

static inline bool mbr_equals(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return a->xmin == b->xmin && a->xmax == b->xmax && a->ymin == b->ymin && a->ymax == b->ymax;
}

static inline bool mbr_disjoint(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return !mbr_intersects(a, b);
}

/* Dispatch MBR predicate based on ha_rkey_function spatial mode.
   Returns true if the entry MBR matches the query predicate. */
static bool spatial_mbr_predicate(enum ha_rkey_function mode, const tdb_mbr_t *query,
                                  const tdb_mbr_t *entry)
{
    /* MariaDB's spatial predicate semantics:
       -- MBRContains(search_geom, col)  find rows where col is WITHIN search_geom
         -- handler receives HA_READ_MBR_CONTAIN, query=search_geom MBR
       -- MBRWithin(col, search_geom)    find rows where col is WITHIN search_geom
         -- handler receives HA_READ_MBR_WITHIN, query=search_geom MBR
       -- MBRIntersects                 symmetric, order doesn't matter */
    switch (mode)
    {
        case HA_READ_MBR_INTERSECT:
            return mbr_intersects(entry, query);
        case HA_READ_MBR_CONTAIN:
            return mbr_within(entry, query);
        case HA_READ_MBR_WITHIN:
            return mbr_within(entry, query);
        case HA_READ_MBR_EQUAL:
            return mbr_equals(entry, query);
        case HA_READ_MBR_DISJOINT:
            return mbr_disjoint(entry, query);
        default:
            return false;
    }
}

/* Hilbert range decomposition resolution.  At SPATIAL_DECOMP_BITS bits per
   axis, the coordinate space is divided into a 2^N x 2^N grid.  Higher
   values produce tighter ranges (fewer false positives) but more ranges
   to scan (more seeks).  8 bits = 256x256 grid, at most 65536 cells but
   typically 10-50 merged ranges for a small query box. */
static constexpr uint SPATIAL_DECOMP_BITS = 8;
static constexpr uint SPATIAL_DECOMP_N = 1u << SPATIAL_DECOMP_BITS;

/* Compute the Hilbert ranges that cover a quantized bounding box.
   Enumerates grid cells at SPATIAL_DECOMP_BITS resolution, computes
   the Hilbert value for each, sorts, and merges contiguous values
   into non-overlapping ranges.  Each range maps back to the full
   32-bit Hilbert space by shifting. */
static void spatial_decompose_ranges(uint32_t qx_min, uint32_t qy_min, uint32_t qx_max,
                                     uint32_t qy_max,
                                     std::vector<std::pair<uint64_t, uint64_t>> &out)
{
    out.clear();

    /* We map the 32-bit quantized coordinates to the coarse grid */
    uint shift = HILBERT_ORDER - SPATIAL_DECOMP_BITS;
    uint gx0 = qx_min >> shift;
    uint gy0 = qy_min >> shift;
    uint gx1 = qx_max >> shift;
    uint gy1 = qy_max >> shift;

    /* We clamp to grid bounds */
    if (gx1 >= SPATIAL_DECOMP_N) gx1 = SPATIAL_DECOMP_N - 1;
    if (gy1 >= SPATIAL_DECOMP_N) gy1 = SPATIAL_DECOMP_N - 1;

    /* We collect hilbret values at coarse resolution for all cells in the box */
    std::vector<uint64_t> cells;
    cells.reserve((gx1 - gx0 + 1) * (gy1 - gy0 + 1));
    for (uint gx = gx0; gx <= gx1; gx++)
    {
        for (uint gy = gy0; gy <= gy1; gy++)
        {
            /* We compute coarse hilbert value and scale to full 64-bit space.
               The coarse cell (gx, gy) at SPATIAL_DECOMP_BITS resolution
               maps to hilbert values in [h_coarse << (2*shift), (h_coarse+1) << (2*shift) - 1] */
            uint64_t h = hilbert_xy2d_64(gx << shift, gy << shift);
            cells.push_back(h);
        }
    }

    if (cells.empty())
    {
        /* Fallback is we scan everything */
        out.push_back({HILBERT_RANGE_FULL_LO, HILBERT_RANGE_FULL_HI});
        return;
    }

    /* We sort and merge into contiguous ranges */
    std::sort(cells.begin(), cells.end());

    /* Each coarse cell covers a range of 2^(HILBERT_DIM*shift) fine hilbert
       values: shift bits per axis times HILBERT_DIM axes. */
    uint64_t cell_span = (uint64_t)1 << (HILBERT_DIM * shift);

    uint64_t range_lo = cells[0];
    uint64_t range_hi = cells[0] + cell_span - 1;

    for (size_t i = 1; i < cells.size(); i++)
    {
        uint64_t lo = cells[i];
        uint64_t hi = cells[i] + cell_span - 1;

        if (lo <= range_hi + 1)
        {
            /* Merge, we extend current range */
            if (hi > range_hi) range_hi = hi;
        }
        else
        {
            /* Gap, we emit current range, start new one */
            out.push_back({range_lo, range_hi});
            range_lo = lo;
            range_hi = hi;
        }
    }
    out.push_back({range_lo, range_hi});
}

/* ******************** System variables (global DB config) ******************** */

static ulong srv_flush_threads = 4;
static ulong srv_compaction_threads = 4;
static ulong srv_log_level = 0;                                      /* TDB_LOG_DEBUG */
static ulonglong srv_block_cache_size = TIDESDB_DEFAULT_BLOCK_CACHE; /* 256M */
static ulong srv_max_open_sstables = 256;
static ulonglong srv_max_memory_usage = 0; /* 0 = auto (library decides) */
static my_bool srv_log_to_file = 1;        /* write TidesDB logs to file (default is yes) */
static ulonglong srv_log_truncation_at = 24ULL * 1024 * 1024; /* log file truncation size (24MB) */
static my_bool srv_unified_memtable = 1; /* 1 = unified WAL+memtable (default), 0 = per-CF */
static ulonglong srv_unified_memtable_write_buffer_size = 128ULL * 1024 * 1024; /* 128MB */

/* Per-session TTL override (seconds).  0 = use table default. */
static MYSQL_THDVAR_ULONGLONG(ttl, PLUGIN_VAR_RQCMDARG,
                              "Per-session TTL in seconds applied to INSERT/UPDATE; "
                              "0 means use the table-level TTL option; "
                              "can be set with SET [SESSION] tidesdb_ttl=N or "
                              "SET STATEMENT tidesdb_ttl=N FOR INSERT",
                              NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

/* Per-session skip unique check (for bulk loads where PK duplicates
   are known impossible).  Same pattern as MyRocks rocksdb_skip_unique_check. */
static MYSQL_THDVAR_BOOL(skip_unique_check, PLUGIN_VAR_RQCMDARG,
                         "Skip uniqueness check on primary key and unique secondary indexes "
                         "during INSERT.  Only safe when the application guarantees no "
                         "duplicates (e.g. bulk loads with monotonic PKs).  "
                         "SET SESSION tidesdb_skip_unique_check=1",
                         NULL, NULL, 0);

/* Per-session row-count threshold for the post-delete range compaction
   trigger.  Zero disables the feature.  When non-zero, the engine tracks
   the comparable min/max PK bytes touched by a single multi-row DELETE
   statement (the start_bulk_delete / end_bulk_delete envelope around
   range deletes) and, if the deleted row count is at least the threshold,
   calls tidesdb_compact_range on the primary CF over the touched range
   at end-of-statement.  This pairs with the new `tidesdb_compact_range`
   API in TidesDB 9.1 to physically reclaim the freshly-tombstoned range
   without waiting for a structural compaction trigger.  Threshold avoids
   making small DELETEs pay synchronous compaction cost. */
static MYSQL_THDVAR_ULONGLONG(
    compact_after_range_delete_min_rows, PLUGIN_VAR_RQCMDARG,
    "If non-zero, after a multi-row DELETE statement that touches at least "
    "this many rows, call tidesdb_compact_range over the touched primary-key "
    "range to physically reclaim tombstoned space.  Default 0 disables the "
    "feature.  Set to 0 to keep the post-DELETE behavior unchanged.",
    NULL, NULL, 0, 0, ULONGLONG_MAX, 1);

/* Per-session opt-in for single-delete semantics on the primary row CF.
   Secondary-index deletes always use tidesdb_txn_single_delete because
   each (col_values, pk) / (term, pk) / (hilbert, pk) composite is written
   exactly once per row lifetime and deleted exactly once -- the
   single-delete contract holds unconditionally for those.
   For the primary CF the contract is narrower: UPDATE ... SET non_pk_col
   writes tidesdb_txn_put(share->cf, data_key(pk), ...) with the same PK,
   producing a put-over-put, and REPLACE INTO / INSERT ... ON DUPLICATE
   KEY UPDATE on tables with no secondary indexes does the same via a
   silent overwrite.  Under either pattern, dropping a put+single-delete
   pair at compaction can re-expose an older put.  Enabling this variable
   is the caller's promise that the session does none of the above --
   typical insert-then-delete, log-style, append-only workloads. */
static MYSQL_THDVAR_BOOL(single_delete_primary, PLUGIN_VAR_RQCMDARG,
                         "Use single-delete semantics for the primary row CF on DELETE. "
                         "Caller promises no UPDATE on non-PK columns, no REPLACE INTO, "
                         "and no INSERT ... ON DUPLICATE KEY UPDATE on tables without "
                         "secondary indexes for this session.  Violating the contract may "
                         "re-expose older row versions after compaction.  Safe choice: "
                         "leave OFF unless the session is INSERT-and-DELETE only.  "
                         "SET SESSION tidesdb_single_delete_primary=1",
                         NULL, NULL, 0);

/* Session-level defaults for table options.
   These are used by HA_TOPTION_SYSVAR so that CREATE TABLE without
   explicit options inherits the session/global default.  Dynamic and
   session-scoped, matching InnoDB's innodb_default_* pattern. */

static const char *compression_names[] = {"NONE", "SNAPPY", "LZ4", "ZSTD", "LZ4_FAST", NullS};
static TYPELIB compression_typelib = {array_elements(compression_names) - 1, "compression_typelib", compression_names, NULL};

static MYSQL_THDVAR_ENUM(default_compression, PLUGIN_VAR_RQCMDARG,
                         "Default compression algorithm for new tables "
                         "(NONE, SNAPPY, LZ4, ZSTD, LZ4_FAST)",
                         NULL, NULL, 2 /* LZ4 */, &compression_typelib);

static MYSQL_THDVAR_ULONGLONG(default_write_buffer_size, PLUGIN_VAR_RQCMDARG,
                              "Default write buffer size in bytes for new tables", NULL, NULL,
                              128ULL * 1024 * 1024, 1024, ULONGLONG_MAX, 1024);

static MYSQL_THDVAR_BOOL(default_bloom_filter, PLUGIN_VAR_RQCMDARG,
                         "Default bloom filter setting for new tables", NULL, NULL, 1);

static MYSQL_THDVAR_BOOL(default_use_btree, PLUGIN_VAR_RQCMDARG,
                         "Default USE_BTREE setting for new tables (0=LSM, 1=B-tree)", NULL, NULL,
                         0);

static MYSQL_THDVAR_BOOL(default_block_indexes, PLUGIN_VAR_RQCMDARG,
                         "Default block indexes setting for new tables", NULL, NULL, 1);

static const char *sync_mode_names[] = {"NONE", "INTERVAL", "FULL", NullS};
static TYPELIB sync_mode_typelib = {array_elements(sync_mode_names) - 1, "sync_mode_typelib", sync_mode_names, NULL};

static MYSQL_THDVAR_ENUM(default_sync_mode, PLUGIN_VAR_RQCMDARG,
                         "Default sync mode for new tables (NONE, INTERVAL, FULL); "
                         "FULL ensures every commit is immediately durable",
                         NULL, NULL, 2 /* FULL */, &sync_mode_typelib);

static MYSQL_THDVAR_ULONGLONG(default_sync_interval_us, PLUGIN_VAR_RQCMDARG,
                              "Default sync interval in microseconds for new tables "
                              "(used when SYNC_MODE=INTERVAL)",
                              NULL, NULL, 128000, 0, ULONGLONG_MAX, 1);

static MYSQL_THDVAR_ULONGLONG(default_bloom_fpr, PLUGIN_VAR_RQCMDARG,
                              "Default bloom filter false positive rate for new tables "
                              "(parts per 10000; 100 = 1%%)",
                              NULL, NULL, 100, 1, 10000, 1);

static MYSQL_THDVAR_ULONGLONG(default_klog_value_threshold, PLUGIN_VAR_RQCMDARG,
                              "Default klog value threshold in bytes for new tables "
                              "(values >= this go to vlog)",
                              NULL, NULL, 512, 0, ULONGLONG_MAX, 1);

static MYSQL_THDVAR_ULONGLONG(default_l0_queue_stall_threshold, PLUGIN_VAR_RQCMDARG,
                              "Default L0 queue stall threshold for new tables", NULL, NULL, 20, 1,
                              1024, 1);

static MYSQL_THDVAR_ULONGLONG(default_l1_file_count_trigger, PLUGIN_VAR_RQCMDARG,
                              "Default L1 file count compaction trigger for new tables", NULL, NULL,
                              4, 1, 1024, 1);

static MYSQL_THDVAR_ULONGLONG(default_level_size_ratio, PLUGIN_VAR_RQCMDARG,
                              "Default level size ratio for new tables", NULL, NULL, 10, 2, 100, 1);

static MYSQL_THDVAR_ULONGLONG(default_min_levels, PLUGIN_VAR_RQCMDARG,
                              "Default minimum LSM-tree levels for new tables", NULL, NULL, 5, 1,
                              64, 1);

static MYSQL_THDVAR_ULONGLONG(default_dividing_level_offset, PLUGIN_VAR_RQCMDARG,
                              "Default dividing level offset for new tables", NULL, NULL, 2, 0, 64,
                              1);

static MYSQL_THDVAR_ULONGLONG(default_skip_list_max_level, PLUGIN_VAR_RQCMDARG,
                              "Default skip list max level for new tables", NULL, NULL, 12, 1, 64,
                              1);

static MYSQL_THDVAR_ULONGLONG(
    default_skip_list_probability, PLUGIN_VAR_RQCMDARG,
    "Default skip list probability for new tables (percentage; 25 = 0.25)", NULL, NULL, 25, 1, 100,
    1);

static MYSQL_THDVAR_ULONGLONG(default_index_sample_ratio, PLUGIN_VAR_RQCMDARG,
                              "Default block index sample ratio for new tables", NULL, NULL, 1, 1,
                              1024, 1);

static MYSQL_THDVAR_ULONGLONG(default_block_index_prefix_len, PLUGIN_VAR_RQCMDARG,
                              "Default block index prefix length for new tables", NULL, NULL, 16, 1,
                              256, 1);

static MYSQL_THDVAR_ULONGLONG(default_min_disk_space, PLUGIN_VAR_RQCMDARG,
                              "Default minimum disk space in bytes for new tables", NULL, NULL,
                              100ULL * 1024 * 1024, 0, ULONGLONG_MAX, 1024);

static MYSQL_THDVAR_BOOL(default_object_lazy_compaction, PLUGIN_VAR_RQCMDARG,
                         "Default object store lazy compaction for new tables. "
                         "When enabled, doubles the L1 file count compaction trigger "
                         "to reduce remote I/O at the cost of higher read amplification",
                         NULL, NULL, 0);

static MYSQL_THDVAR_BOOL(default_object_prefetch_compaction, PLUGIN_VAR_RQCMDARG,
                         "Default object store prefetch compaction for new tables. "
                         "When enabled, downloads all input SSTables in parallel "
                         "before compaction merge begins",
                         NULL, NULL, 1);

/* Tombstone-density compaction trigger (parts per 10000 -- 5000 = 0.50 ratio).
   When non-zero, after each flush the engine inspects level-1 SSTables and
   escalates compaction for any single SST whose tombstone count divided by
   entry count exceeds this ratio while having at least
   tombstone_density_min_entries entries.  Default 0 keeps the existing
   structural-trigger behavior. */
static MYSQL_THDVAR_ULONGLONG(default_tombstone_density_trigger, PLUGIN_VAR_RQCMDARG,
                              "Default tombstone-density compaction trigger ratio for new tables, "
                              "expressed as parts per 10000 (5000 = 0.50, 0 disables).  When set, "
                              "compaction is escalated for any level-1 SSTable whose tombstone "
                              "count divided by entry count exceeds the ratio.",
                              NULL, NULL, 0, 0, 10000, 1);

static MYSQL_THDVAR_ULONGLONG(default_tombstone_density_min_entries, PLUGIN_VAR_RQCMDARG,
                              "Minimum entry count for an SSTable to be considered by the "
                              "tombstone-density trigger; smaller SSTables are ignored",
                              NULL, NULL, 1024, 0, ULONGLONG_MAX, 1);

static const char *isolation_level_names[] = {
    "READ_UNCOMMITTED", "READ_COMMITTED", "REPEATABLE_READ", "SNAPSHOT", "SERIALIZABLE", NullS};
static TYPELIB isolation_level_typelib = {array_elements(isolation_level_names) - 1, "isolation_level_typelib", isolation_level_names, NULL};

static MYSQL_THDVAR_ENUM(default_isolation_level, PLUGIN_VAR_RQCMDARG,
                         "Default isolation level for new tables "
                         "(READ_UNCOMMITTED, READ_COMMITTED, REPEATABLE_READ, SNAPSHOT, "
                         "SERIALIZABLE)",
                         NULL, NULL, 2 /* REPEATABLE_READ */, &isolation_level_typelib);

static const char *log_level_names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL", "NONE", NullS};
static TYPELIB log_level_typelib = {array_elements(log_level_names) - 1, "log_level_typelib", log_level_names, NULL};

static MYSQL_SYSVAR_ULONG(flush_threads, srv_flush_threads,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "Number of TidesDB flush threads", NULL, NULL, 4, 1, 64, 0);

static MYSQL_SYSVAR_ULONG(compaction_threads, srv_compaction_threads,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "Number of TidesDB compaction threads", NULL, NULL, 4, 1, 64, 0);

static MYSQL_SYSVAR_ENUM(log_level, srv_log_level, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "TidesDB log level (DEBUG, INFO, WARN, ERROR, FATAL, NONE)", NULL, NULL, 0,
                         &log_level_typelib);

/* Conflict information logging.
   Similar to innodb_print_all_deadlocks -- logs all TDB_ERR_CONFLICT
   events to the error log with transaction and table details.
   (srv_print_all_conflicts, g_engine_ctx.last_conflict_mutex, g_engine_ctx.last_conflict_info
    are forward-declared near tdb_rc_to_ha().) */
static MYSQL_SYSVAR_BOOL(print_all_conflicts, srv_print_all_conflicts, PLUGIN_VAR_RQCMDARG,
                         "Log all TidesDB conflict errors to the error log "
                         "(similar to innodb_print_all_deadlocks)",
                         NULL, NULL, 0);

static MYSQL_SYSVAR_BOOL(pessimistic_locking, srv_pessimistic_locking, PLUGIN_VAR_RQCMDARG,
                         "Enable plugin-level row locks for SELECT ... FOR UPDATE, "
                         "UPDATE, DELETE, and INSERT on user-defined primary keys. "
                         "OFF (default): pure optimistic MVCC -- concurrent writers on the "
                         "same row are detected at COMMIT time (TDB_ERR_CONFLICT). "
                         "ON: all write-intent statements acquire per-row locks via a "
                         "partitioned hash-table lock manager with wait-for-graph "
                         "deadlock detection. Locks are held until COMMIT or ROLLBACK. "
                         "Both explicit transactions and autocommit statements participate. "
                         "Locks can be acquired on non-existing PK values (e.g. SELECT "
                         "... FOR UPDATE on a missing row blocks INSERT of that key). "
                         "Enable for TPC-C or workloads that depend on row-level "
                         "serialization semantics",
                         NULL, NULL, 0);

static MYSQL_SYSVAR_ULONG(fts_min_word_len, srv_fts_min_word_len, PLUGIN_VAR_RQCMDARG,
                          "Minimum word length (in characters) for full-text indexing. "
                          "Shorter words are excluded from the index and search queries",
                          NULL, NULL, 3, 1, 84, 0);

static MYSQL_SYSVAR_ULONG(fts_max_word_len, srv_fts_max_word_len, PLUGIN_VAR_RQCMDARG,
                          "Maximum word length (in characters) for full-text indexing. "
                          "Longer words are excluded from the index and search queries",
                          NULL, NULL, 84, 1, 512, 0);

static MYSQL_SYSVAR_DOUBLE(fts_bm25_k1, srv_fts_bm25_k1, PLUGIN_VAR_RQCMDARG,
                           "BM25 k1 parameter controlling term-frequency saturation. "
                           "Higher values give more weight to repeated terms. "
                           "Standard default is 1.2",
                           NULL, NULL, 1.2, 0.0, 10.0, 0);

static MYSQL_SYSVAR_DOUBLE(fts_bm25_b, srv_fts_bm25_b, PLUGIN_VAR_RQCMDARG,
                           "BM25 b parameter controlling document-length normalization. "
                           "0 = no normalization, 1 = full normalization. "
                           "Standard default is 0.75",
                           NULL, NULL, 0.75, 0.0, 1.0, 0);

static MYSQL_SYSVAR_STR(fts_blend_chars, srv_fts_blend_chars,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Characters treated as both separators and valid word characters "
                        "in full-text indexing. When a blend character appears inside a "
                        "token, the tokenizer emits both the full blended form and the "
                        "individual parts. For example, with blend_chars=\"'\" the input "
                        "\"l'aria\" produces tokens: l'aria, aria. "
                        "Set to \"'\" for Italian/French elision support. "
                        "Default is empty (no blend characters)",
                        NULL, tdb_fts_blend_chars_update, NULL);

static MYSQL_SYSVAR_STR(ft_stopword_table, srv_ft_stopword_table,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "User-defined stop word table in 'db_name/table_name' format. "
                        "The table must have a VARCHAR column named 'value'. "
                        "When NULL (default), uses the same 36 default stop words as "
                        "information_schema.INNODB_FT_DEFAULT_STOPWORD. "
                        "Set to empty string to disable stop word filtering entirely",
                        NULL, tdb_ft_stopword_table_update, NULL);

static MYSQL_SYSVAR_ULONGLONG(block_cache_size, srv_block_cache_size,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "TidesDB global block cache size in bytes", NULL, NULL,
                              TIDESDB_DEFAULT_BLOCK_CACHE, 0, ULONGLONG_MAX, 0);

static MYSQL_SYSVAR_ULONG(max_open_sstables, srv_max_open_sstables,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "Max cached SSTable structures in LRU cache", NULL, NULL, 256, 1, 65536,
                          0);

static MYSQL_SYSVAR_ULONGLONG(max_memory_usage, srv_max_memory_usage,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "TidesDB global memory limit in bytes "
                              "(0 = auto, 50% of system RAM; minimum 5% of system RAM)",
                              NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

static MYSQL_SYSVAR_BOOL(log_to_file, srv_log_to_file, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Write TidesDB logs to a LOG file in the data directory "
                         "instead of stderr (default: ON)",
                         NULL, NULL, 1);

static MYSQL_SYSVAR_ULONGLONG(log_truncation_at, srv_log_truncation_at,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "TidesDB log file truncation size in bytes "
                              "(0 = no truncation, default: 24MB)",
                              NULL, NULL, 24ULL * 1024 * 1024, 0, ULONGLONG_MAX, 0);

static MYSQL_SYSVAR_BOOL(unified_memtable, srv_unified_memtable,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Use a single unified WAL and memtable across all column families. "
                         "Reduces WAL fsync overhead from O(num_tables) to O(1) and provides "
                         "atomic cross-CF commits. Best for multi-table OLTP workloads. "
                         "Requires all CFs to use the same comparator (default: ON)",
                         NULL, NULL, 1);

static MYSQL_SYSVAR_ULONGLONG(unified_memtable_write_buffer_size,
                              srv_unified_memtable_write_buffer_size,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "Write buffer size in bytes for the unified memtable. "
                              "0 = automatic (library default). Only meaningful when "
                              "tidesdb_unified_memtable=ON",
                              NULL, NULL, 128ULL * 1024 * 1024, 0, ULONGLONG_MAX, 0);

static ulong srv_unified_memtable_sync_mode = 2; /* FULL */

static MYSQL_SYSVAR_ENUM(unified_memtable_sync_mode, srv_unified_memtable_sync_mode,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Sync mode for the unified WAL (NONE, INTERVAL, FULL). "
                         "Only meaningful when tidesdb_unified_memtable=ON. "
                         "NONE: fastest, relies on OS page cache. "
                         "INTERVAL: periodic sync every unified_memtable_sync_interval_us. "
                         "FULL: fsync on every commit (default, most durable)",
                         NULL, NULL, 2 /* FULL */, &sync_mode_typelib);

static ulonglong srv_unified_memtable_sync_interval = 128000;

static MYSQL_SYSVAR_ULONGLONG(unified_memtable_sync_interval, srv_unified_memtable_sync_interval,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "Sync interval in microseconds for the unified WAL "
                              "(only used when unified_memtable_sync_mode=INTERVAL)",
                              NULL, NULL, 128000, 0, ULONGLONG_MAX, 0);

/* Configurable data directory.
   Defaults to NULL which means the plugin computes a sibling directory
   of mysql_real_data_home.  Setting this overrides the auto-computed path. */
static char *srv_data_home_dir = NULL;

static MYSQL_SYSVAR_STR(data_home_dir, srv_data_home_dir, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Directory where TidesDB stores its data files; "
                        "defaults to <mysql_datadir>/../tidesdb_data; "
                        "must be set before server startup (read-only)",
                        NULL, NULL, NULL);

/* tidesdb_master_key_file: path to a 32-byte file holding the AES-256
   master key used by every encrypted CREATE TABLE. Read once at plugin
   init; if absent, missing, or wrong size, encryption stays unavailable
   and any CREATE TABLE ... ENGINE_ATTRIBUTE='{"encrypted":true}' fails
   with a clear error.

   Note on H-5 (review finding): we leave master_key_file visible in
   SHOW VARIABLES because (a) operators legitimately need to verify
   their encryption config, (b) MTR tests and tooling query it, and
   (c) the path itself does not grant access -- file-system permissions
   on the key file are the actual control. The S3 access/secret keys
   are different: they are real credentials in plaintext, so they get
   PLUGIN_VAR_NOSYSVAR (see below). */
static char *srv_master_key_file = NULL;
static MYSQL_SYSVAR_STR(master_key_file, srv_master_key_file,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Path to a 32-byte AES-256 master key file. Loaded once "
                        "at plugin init; required for any table created with "
                        "ENGINE_ATTRIBUTE='{\"encrypted\":true}'.",
                        NULL, NULL, NULL);

/* ******************** Object Store Configuration ******************** */

/* Object store backend (0=LOCAL (no object store), 1=S3) */
static ulong srv_object_store_backend = 0;
static const char *object_store_backend_names[] = {"LOCAL", "S3", NullS};
static TYPELIB object_store_backend_typelib = {array_elements(object_store_backend_names) - 1, "object_store_backend_typelib", object_store_backend_names, NULL};
static MYSQL_SYSVAR_ENUM(object_store_backend, srv_object_store_backend,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Object store backend (LOCAL=disabled, S3=S3-compatible)", NULL, NULL, 0,
                         &object_store_backend_typelib);

static char *srv_s3_endpoint = NULL;
static MYSQL_SYSVAR_STR(s3_endpoint, srv_s3_endpoint, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 endpoint (e.g. s3.amazonaws.com or minio.local:9000)", NULL, NULL,
                        NULL);

static char *srv_s3_bucket = NULL;
static MYSQL_SYSVAR_STR(s3_bucket, srv_s3_bucket, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 bucket name", NULL, NULL, NULL);

static char *srv_s3_prefix = NULL;
static MYSQL_SYSVAR_STR(s3_prefix, srv_s3_prefix, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 key prefix (e.g. production/db1/)", NULL, NULL, NULL);

/* S3 access + secret keys: NOSYSVAR so SHOW VARIABLES / SHOW GLOBAL
   VARIABLES cannot disclose them to users with SYSTEM_VARIABLES_ADMIN.
   Settable at startup (my.cnf / cmdline) only. */
static char *srv_s3_access_key = NULL;
static MYSQL_SYSVAR_STR(s3_access_key, srv_s3_access_key,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
                            PLUGIN_VAR_NOSYSVAR,
                        "S3 access key ID", NULL, NULL, NULL);

static char *srv_s3_secret_key = NULL;
static MYSQL_SYSVAR_STR(s3_secret_key, srv_s3_secret_key,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
                            PLUGIN_VAR_NOSYSVAR,
                        "S3 secret access key", NULL, NULL, NULL);

static char *srv_s3_region = NULL;
static MYSQL_SYSVAR_STR(s3_region, srv_s3_region, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 region (e.g. us-east-1, NULL for MinIO)", NULL, NULL, NULL);

static my_bool srv_s3_use_ssl = 1;
static MYSQL_SYSVAR_BOOL(s3_use_ssl, srv_s3_use_ssl, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Use HTTPS for S3 connections (default ON)", NULL, NULL, 1);

static my_bool srv_s3_path_style = 0;
static MYSQL_SYSVAR_BOOL(s3_path_style, srv_s3_path_style,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Use path-style S3 URLs (required for MinIO, default OFF)", NULL, NULL, 0);

static ulonglong srv_objstore_local_cache_max = 0;
static MYSQL_SYSVAR_ULONGLONG(
    objstore_local_cache_max, srv_objstore_local_cache_max,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Maximum local cache size in bytes for object store mode (0=unlimited)", NULL, NULL, 0, 0,
    ULONGLONG_MAX, 0);

static ulonglong srv_objstore_wal_sync_threshold = 1048576;
static MYSQL_SYSVAR_ULONGLONG(
    objstore_wal_sync_threshold, srv_objstore_wal_sync_threshold,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Sync active WAL to object store when it grows by this many bytes (default 1MB, 0=disable)",
    NULL, NULL, 1048576, 0, ULONGLONG_MAX, 0);

static my_bool srv_objstore_wal_sync_on_commit = 0;
static MYSQL_SYSVAR_BOOL(objstore_wal_sync_on_commit, srv_objstore_wal_sync_on_commit,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Upload WAL after every commit for RPO=0 replication (default OFF)", NULL,
                         NULL, 0);

static my_bool srv_replica_mode = 0;
static MYSQL_SYSVAR_BOOL(replica_mode, srv_replica_mode, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Enable read-only replica mode (default OFF)", NULL, NULL, 0);

static ulonglong srv_replica_sync_interval = 5000000;
static MYSQL_SYSVAR_ULONGLONG(
    replica_sync_interval, srv_replica_sync_interval, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "MANIFEST poll interval for replica sync in microseconds (default 5s)", NULL, NULL, 5000000,
    100000, ULONGLONG_MAX, 0);

/* Promote replica to primary -- trigger variable (like backup_dir) */
static my_bool srv_promote_primary = 0;
static void tidesdb_promote_primary_update(THD *thd, SYS_VAR *, void *var_ptr,
                                           const void *save)
{
    my_bool val = *static_cast<const my_bool *>(save);
    if (!val) return; /* only act on SET ... = ON */

    if (!tdb_get_engine())
    {
        my_error(ER_UNKNOWN_ERROR, MYF(0));
        return;
    }

    int rc = tidesdb_promote_to_primary(tdb_get_engine());
    if (rc == TDB_SUCCESS)
    {
        sql_print_information("[TIDESDB] Replica promoted to primary successfully");
    }
    else
    {
        sql_print_error("[TIDESDB] Failed to promote replica (err=%d)", rc);
    }

    /* reset to OFF so it can be triggered again */
    *static_cast<my_bool *>(var_ptr) = 0;
}

static MYSQL_SYSVAR_BOOL(promote_primary, srv_promote_primary, PLUGIN_VAR_RQCMDARG,
                         "Set to ON to promote this replica to primary (trigger, resets to OFF)",
                         NULL, tidesdb_promote_primary_update, 0);

/* ******************** Online backup via system variable ******************** */

static char *srv_backup_dir = NULL;

/*
  We do the actual backup work in check (not update) because:

   1. check is allowed to raise errors via my_error/my_printf_error and
      return non-zero to reject the SET. update is not -- raising an error
      from update leaves the diagnostics area in ERROR state, and Debug
      builds then assert at sql_error.cc:422 when mysql_execute_command
      reports OK after the SET completes.

   2. check runs before the framework copies the value into var_ptr, so a
      failed backup leaves the variable at its previous value -- matches
      what the user expects when they see ER_UNKNOWN_ERROR from SET.

   3. PLUGIN_VAR_MEMALLOC sysvars without a custom check use check_func_str
      to copy the input into save via thd->strmake; we must do that here
      since we're overriding the default check.
*/
/* HF-3 fix (follow-up review): operator-configured allowed-root sysvar
   for backup / checkpoint destinations. NULL (default) preserves the
   prior lexical-only behavior (any absolute non-`..` path accepted).
   When set, both backup_dir and checkpoint_dir destinations must
   canonicalize (via realpath) to a path under the canonicalized
   allowed-root -- which defeats the symlink-redirect attack class
   (SYSTEM_VARIABLES_ADMIN holder plants a symlink and aims it at
   /etc/cron.d). Operators who want strict confinement set this. */
static char *srv_backup_allowed_root = NULL;
static MYSQL_SYSVAR_STR(backup_allowed_root, srv_backup_allowed_root,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "If set, restrict tidesdb_backup_dir and "
                        "tidesdb_checkpoint_dir to paths whose realpath() "
                        "is under this directory. Must itself be an absolute "
                        "path. Default NULL = no restriction beyond the "
                        "lexical no-'..' check.",
                        NULL, NULL, NULL);

/* MF-6: sanitize untrusted strings before they reach the error log.
   sql_print_* writes to stderr via vfprintf; embedded newlines or
   ANSI escape sequences fool downstream log parsers / SIEM tools
   into thinking the attacker's content is its own log line. Replace
   any byte < 0x20 (control chars including newline / CR / NUL / ESC)
   with '?'. Output is written to `out` (caller-provided fixed buffer)
   and NUL-terminated; truncation is silent and accepted.

   Used wherever a SYSTEM_VARIABLES_ADMIN-controlled value (path,
   table_spec) is logged via %s. */
void tdb_sanitize_for_log(const char *in, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (!in)
    {
        out[0] = '\0';
        return;
    }
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < out_size; i++)
    {
        unsigned char c = (unsigned char)in[i];
        out[o++] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    out[o] = '\0';
}

/* Lexical safety check for backup / checkpoint destination paths.
   Rejects any path that is not absolute, or that contains a `..`
   component. This prevents the trivial path-traversal escape (a DBA
   with SYSTEM_VARIABLES_ADMIN setting `tidesdb_backup_dir = '../../etc'`
   to copy data files outside the intended location).

   For symlink-redirect attacks, callers should additionally call
   tdb_path_is_under_allowed_root() AFTER this check passes.

   MF-7: explicit null-byte rejection. strlen() already stops at the
   first '\0' so an attacker-supplied path with a payload after a null
   is already truncated, but we reject defensively in case a future
   length-aware consumer is added. */
static bool tdb_path_is_safe(const char *path, size_t len)
{
    if (!path || path[0] != '/') return false; /* absolute only */
    /* If the SQL layer surfaced a length, reject any embedded null
       within it. When len == 0 we fall back to strlen() semantics. */
    if (len > 0)
    {
        for (size_t i = 0; i < len; i++)
        {
            if (path[i] == '\0') return false;
        }
    }
    const char *p = path;
    while ((p = strstr(p, "..")) != nullptr)
    {
        bool prev_is_boundary = (p == path || p[-1] == '/');
        bool next_is_boundary = (p[2] == '\0' || p[2] == '/');
        if (prev_is_boundary && next_is_boundary) return false;
        p += 2;
    }
    return true;
}

/* HF-3 confinement check. Returns true if `path` is allowed -- either
   because no allowed-root is configured (NULL), or because realpath(path)
   resolves under realpath(allowed_root). realpath() follows symlinks,
   so this catches a symlinked component that aims outside the root.

   We resolve the destination's PARENT directory rather than the
   destination itself because the backup directory is expected NOT to
   exist yet (tidesdb_backup creates it). realpath on a non-existent
   path fails; resolving the parent and appending the basename is the
   defensible approximation. */
static bool tdb_path_is_under_allowed_root(const char *path)
{
    if (!srv_backup_allowed_root || !srv_backup_allowed_root[0])
        return true; /* operator did not enable confinement */

    /* Canonicalize the allowed root. If it doesn't exist, refuse to
       proceed -- a misconfigured policy should fail closed. */
    char root_real[PATH_MAX];
    if (!::realpath(srv_backup_allowed_root, root_real))
    {
        sql_print_warning("[TIDESDB] tidesdb_backup_allowed_root='%s' "
                          "does not resolve (errno=%d); rejecting all "
                          "backup/checkpoint destinations until fixed",
                          srv_backup_allowed_root, errno);
        return false;
    }

    /* Resolve the destination's parent directory; the destination
       itself does not yet exist for a fresh backup. */
    std::string dest(path);
    size_t slash = dest.find_last_of('/');
    std::string parent = (slash == std::string::npos || slash == 0)
                             ? std::string("/")
                             : dest.substr(0, slash);
    char parent_real[PATH_MAX];
    if (!::realpath(parent.c_str(), parent_real))
    {
        sql_print_warning("[TIDESDB] backup/checkpoint destination parent "
                          "'%s' does not resolve (errno=%d); rejecting",
                          parent.c_str(), errno);
        return false;
    }

    /* Prefix check: parent_real must start with root_real, AND the
       character following the prefix must be '/' or end-of-string (so
       /allowed_root_evil doesn't sneak past a /allowed_root match). */
    size_t rlen = strlen(root_real);
    if (strncmp(parent_real, root_real, rlen) != 0) return false;
    if (parent_real[rlen] != '\0' && parent_real[rlen] != '/') return false;
    return true;
}

static int tidesdb_backup_dir_check(THD *thd, SYS_VAR *, void *save,
                                    struct st_mysql_value *value)
{
    char buf[1024];
    int len = sizeof(buf);
    const char *new_dir = value->val_str(value, buf, &len);

    /* Empty / NULL -- accept (clears the variable). */
    if (!new_dir || !new_dir[0])
    {
        *static_cast<const char **>(save) = NULL;
        return 0;
    }

    /* MF-6: pre-sanitize the path for any log/error message we emit
       below. Attacker-controlled strings via SET GLOBAL must not be
       able to inject log lines via embedded newlines / escape codes. */
    char sanitized_path[1024];
    tdb_sanitize_for_log(new_dir, sanitized_path, sizeof(sanitized_path));

    if (!tdb_path_is_safe(new_dir, (size_t)len))
    {
        my_printf_error(ER_UNKNOWN_ERROR,
                        "[TIDESDB] Backup path must be absolute and free of "
                        "'..' / NUL components: '%s' rejected",
                        MYF(0), sanitized_path);
        return 1;
    }

    /* HF-3: defense against symlink-redirect when an operator has
       configured tidesdb_backup_allowed_root. */
    if (!tdb_path_is_under_allowed_root(new_dir))
    {
        my_printf_error(ER_UNKNOWN_ERROR,
                        "[TIDESDB] Backup path '%s' is not under "
                        "tidesdb_backup_allowed_root",
                        MYF(0), sanitized_path);
        return 1;
    }

    /* HF-4: tidesdb_backup() is uncancellable from MySQL's side -- there
       is no thd_killed poll inside the library. If the user's query has
       already been killed by the time this callback runs, refuse early
       rather than start a potentially-long operation we can't abort. */
    if (thd_killed(thd))
    {
        my_error(ER_QUERY_INTERRUPTED, MYF(0));
        return 1;
    }

    if (!tdb_get_engine())
    {
        my_error(ER_UNKNOWN_ERROR, MYF(0), "TidesDB is not open");
        return 1;
    }

    /* Free the calling connection's TidesDB transaction before backup.
       tidesdb_backup() waits for all open transactions to drain.

       Thread-safety note (responding to a flagged review concern):
       `thd` here is the *issuing* THD -- the same thread that ran
       SET GLOBAL tidesdb_backup_dir=... and is now executing this
       check callback synchronously. MySQL's sysvar machinery does not
       hop threads, so reading and mutating `trx->txn` is purely
       thread-local; there is no concurrent reader/writer on the same
       trx. Any other connection's DML touches its own thd_get_ha_data
       slot, not this one. */
    {
        tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
        if (trx && trx->txn)
        {
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
            trx->txn = NULL;
            trx->dirty = false;
            trx->txn_generation++;
        }
    }

    /* check is called WITHOUT LOCK_global_system_variables held (unlike
       update), so we don't need the unlock/relock dance the original
       update-based version did to dodge flush-thread deadlock. Copy the
       path because val_str's buffer is stack-local. */
    std::string backup_path(new_dir);

    int rc = tidesdb_backup(tdb_get_engine(), const_cast<char *>(backup_path.c_str()));

    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] Backup to '%s' failed (err=%d)", sanitized_path, rc);
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] Backup to '%s' failed (err=%d)",
                        MYF(0), sanitized_path, rc);
        return 1;  /* reject the SET; var keeps old value */
    }

    /* Backup succeeded -- stage the new value for the framework's update
       step. Use thd->strmake exactly like check_func_str so the buffer
       lifetime matches what PLUGIN_VAR_MEMALLOC update expects. */
    *static_cast<const char **>(save) = thd->strmake(backup_path.c_str(),
                                                     backup_path.size());
    return 0;
}

/*
  update is now the canonical PLUGIN_VAR_MEMALLOC installer: just copy the
  pointer the framework staged via check. The framework handles my_strdup'ing
  and freeing the previous value internally.
*/
static void tidesdb_backup_dir_update(THD *, SYS_VAR *, void *var_ptr, const void *save)
{
    *static_cast<const char **>(var_ptr) = *static_cast<const char *const *>(save);
}

static MYSQL_SYSVAR_STR(backup_dir, srv_backup_dir, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Set to a directory path to trigger an online TidesDB backup. "
                        "The directory must not exist or be empty. The path must be "
                        "absolute and contain no '..' components. If tidesdb_backup_allowed_root "
                        "is set, the path must additionally resolve under that root. "
                        "WARNING: the backup operation is synchronous and currently NOT "
                        "interruptible by KILL QUERY -- on a slow backend (NFS, full disk) "
                        "the connection may block for an extended period. "
                        "Example: SET GLOBAL tidesdb_backup_dir = '/path/to/backup'",
                        tidesdb_backup_dir_check, tidesdb_backup_dir_update, NULL);

/* Checkpoint (hard-link snapshot) via system variable */

static char *srv_checkpoint_dir = NULL;

/* See tidesdb_backup_dir_check for the rationale on doing the work in
   check rather than update. */
static int tidesdb_checkpoint_dir_check(THD *thd, SYS_VAR *, void *save,
                                        struct st_mysql_value *value)
{
    char buf[1024];
    int len = sizeof(buf);
    const char *new_dir = value->val_str(value, buf, &len);

    if (!new_dir || !new_dir[0])
    {
        *static_cast<const char **>(save) = NULL;
        return 0;
    }

    /* MF-6: sanitize before logging. */
    char sanitized_path[1024];
    tdb_sanitize_for_log(new_dir, sanitized_path, sizeof(sanitized_path));

    if (!tdb_path_is_safe(new_dir, (size_t)len))
    {
        my_printf_error(ER_UNKNOWN_ERROR,
                        "[TIDESDB] Checkpoint path must be absolute and free of "
                        "'..' / NUL components: '%s' rejected",
                        MYF(0), sanitized_path);
        return 1;
    }

    /* HF-3: enforce allowed-root if operator has configured one. */
    if (!tdb_path_is_under_allowed_root(new_dir))
    {
        my_printf_error(ER_UNKNOWN_ERROR,
                        "[TIDESDB] Checkpoint path '%s' is not under "
                        "tidesdb_backup_allowed_root",
                        MYF(0), sanitized_path);
        return 1;
    }

    /* HF-4: refuse early if THD already killed; tidesdb_checkpoint is
       uncancellable from our side. */
    if (thd && thd_killed(thd))
    {
        my_error(ER_QUERY_INTERRUPTED, MYF(0));
        return 1;
    }

    if (!tdb_get_engine())
    {
        my_error(ER_UNKNOWN_ERROR, MYF(0), "TidesDB is not open");
        return 1;
    }

    std::string ckpt_path(new_dir);

    /* check runs without LOCK_global_system_variables held, so no
       unlock/relock needed (see tidesdb_backup_dir_check). */
    int rc = tidesdb_checkpoint(tdb_get_engine(), ckpt_path.c_str());

    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] Checkpoint to '%s' failed (err=%d)", sanitized_path, rc);
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] Checkpoint to '%s' failed (err=%d)",
                        MYF(0), sanitized_path, rc);
        return 1;
    }

    /* `thd` is the callback parameter (no longer ignored after HF-4
       added the killed-pre-check); use it directly. */
    *static_cast<const char **>(save) =
        thd ? thd->strmake(ckpt_path.c_str(), ckpt_path.size()) : NULL;
    return 0;
}

static void tidesdb_checkpoint_dir_update(THD *, SYS_VAR *, void *var_ptr, const void *save)
{
    *static_cast<const char **>(var_ptr) = *static_cast<const char *const *>(save);
}

static MYSQL_SYSVAR_STR(checkpoint_dir, srv_checkpoint_dir,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Set to a directory path to trigger a TidesDB checkpoint "
                        "(hard-link snapshot, near-instant). "
                        "The directory must not exist or be empty. The path must be "
                        "absolute and contain no '..' components. If tidesdb_backup_allowed_root "
                        "is set, the path must additionally resolve under that root. "
                        "Example: SET GLOBAL tidesdb_checkpoint_dir = '/path/to/checkpoint'",
                        tidesdb_checkpoint_dir_check, tidesdb_checkpoint_dir_update, NULL);

static SYS_VAR *tidesdb_system_variables[] = {
    MYSQL_SYSVAR(flush_threads),
    MYSQL_SYSVAR(compaction_threads),
    MYSQL_SYSVAR(log_level),
    MYSQL_SYSVAR(block_cache_size),
    MYSQL_SYSVAR(max_open_sstables),
    MYSQL_SYSVAR(max_memory_usage),
    MYSQL_SYSVAR(backup_dir),
    MYSQL_SYSVAR(checkpoint_dir),
    MYSQL_SYSVAR(backup_allowed_root),
    MYSQL_SYSVAR(print_all_conflicts),
    MYSQL_SYSVAR(pessimistic_locking),
    MYSQL_SYSVAR(fts_min_word_len),
    MYSQL_SYSVAR(fts_max_word_len),
    MYSQL_SYSVAR(fts_bm25_k1),
    MYSQL_SYSVAR(fts_bm25_b),
    MYSQL_SYSVAR(ft_stopword_table),
    MYSQL_SYSVAR(fts_blend_chars),
    MYSQL_SYSVAR(data_home_dir),
    MYSQL_SYSVAR(master_key_file),
    MYSQL_SYSVAR(ttl),
    MYSQL_SYSVAR(skip_unique_check),
    MYSQL_SYSVAR(single_delete_primary),
    MYSQL_SYSVAR(compact_after_range_delete_min_rows),
    MYSQL_SYSVAR(default_compression),
    MYSQL_SYSVAR(default_write_buffer_size),
    MYSQL_SYSVAR(default_bloom_filter),
    MYSQL_SYSVAR(default_use_btree),
    MYSQL_SYSVAR(default_block_indexes),
    MYSQL_SYSVAR(default_sync_mode),
    MYSQL_SYSVAR(default_sync_interval_us),
    MYSQL_SYSVAR(default_bloom_fpr),
    MYSQL_SYSVAR(default_klog_value_threshold),
    MYSQL_SYSVAR(default_l0_queue_stall_threshold),
    MYSQL_SYSVAR(default_l1_file_count_trigger),
    MYSQL_SYSVAR(default_level_size_ratio),
    MYSQL_SYSVAR(default_min_levels),
    MYSQL_SYSVAR(default_dividing_level_offset),
    MYSQL_SYSVAR(default_skip_list_max_level),
    MYSQL_SYSVAR(default_skip_list_probability),
    MYSQL_SYSVAR(default_index_sample_ratio),
    MYSQL_SYSVAR(default_block_index_prefix_len),
    MYSQL_SYSVAR(default_min_disk_space),
    MYSQL_SYSVAR(default_isolation_level),
    MYSQL_SYSVAR(log_to_file),
    MYSQL_SYSVAR(log_truncation_at),
    MYSQL_SYSVAR(unified_memtable),
    MYSQL_SYSVAR(unified_memtable_write_buffer_size),
    MYSQL_SYSVAR(unified_memtable_sync_mode),
    MYSQL_SYSVAR(unified_memtable_sync_interval),
    MYSQL_SYSVAR(object_store_backend),
    MYSQL_SYSVAR(s3_endpoint),
    MYSQL_SYSVAR(s3_bucket),
    MYSQL_SYSVAR(s3_prefix),
    MYSQL_SYSVAR(s3_access_key),
    MYSQL_SYSVAR(s3_secret_key),
    MYSQL_SYSVAR(s3_region),
    MYSQL_SYSVAR(s3_use_ssl),
    MYSQL_SYSVAR(s3_path_style),
    MYSQL_SYSVAR(objstore_local_cache_max),
    MYSQL_SYSVAR(objstore_wal_sync_threshold),
    MYSQL_SYSVAR(objstore_wal_sync_on_commit),
    MYSQL_SYSVAR(replica_mode),
    MYSQL_SYSVAR(replica_sync_interval),
    MYSQL_SYSVAR(promote_primary),
    MYSQL_SYSVAR(default_object_lazy_compaction),
    MYSQL_SYSVAR(default_object_prefetch_compaction),
    MYSQL_SYSVAR(default_tombstone_density_trigger),
    MYSQL_SYSVAR(default_tombstone_density_min_entries),
    NULL};

/* ******************** Table options (per-table CF config) ******************** */

struct ha_table_option_struct
{
    ulonglong write_buffer_size;
    ulonglong min_disk_space;
    ulonglong klog_value_threshold;
    ulonglong sync_interval_us;
    ulonglong index_sample_ratio;
    ulonglong block_index_prefix_len;
    ulonglong level_size_ratio;
    ulonglong min_levels;
    ulonglong dividing_level_offset;
    ulonglong skip_list_max_level;
    ulonglong skip_list_probability; /* percentage      -- 25 = 0.25 */
    ulonglong bloom_fpr;             /* parts per 10000 -- 100 = 1% */
    ulonglong l1_file_count_trigger;
    ulonglong l0_queue_stall_threshold;
    uint compression;
    uint sync_mode;
    uint isolation_level;
    bool bloom_filter;
    bool block_indexes;
    bool use_btree;
    bool object_lazy_compaction;     /* double L1 file count trigger in object store mode */
    bool object_prefetch_compaction; /* prefetch input SSTables before compaction merge */
    ulonglong ttl;                   /* default TTL in seconds (0 = no expiration) */
    bool encrypted;                  /* ENCRYPTED=YES enables data-at-rest encryption */
    ulonglong encryption_key_id;     /* ENCRYPTION_KEY_ID (default 1) */
    /* Tombstone-density compaction trigger.  Stored as parts-per-10000
       (e.g. 5000 = 0.50 ratio) so the option list can use integer storage;
       converted to a double at build_cf_config time. */
    ulonglong tombstone_density_trigger;
    ulonglong tombstone_density_min_entries;
};


/* my_rapidjson_size_t.h MUST precede any rapidjson header so the
 * typedef of rapidjson::SizeType matches what the rest of MySQL is
 * built with. The plugin's CMakeLists sets RAPIDJSON_NO_SIZETYPEDEFINE
 * to keep the typedef site under our control; if this include is
 * missing, the plugin's rapidjson templates instantiate against an
 * unsigned-int SizeType and the resulting ABI mismatch crashes
 * Document::Parse with a `stackTop_` assertion in Debug builds. */
#include "my_rapidjson_size_t.h"
#include <rapidjson/document.h>
#include <cstring>
#include <cstdlib>

/* MariaDB-style table options grammar (e.g. `ENGINE=TIDESDB COMPRESSION='LZ4'
 * BLOOM_FILTER=1`) doesn't exist in MySQL. MySQL exposes per-table engine
 * attributes via `ENGINE_ATTRIBUTE='{...}'` JSON instead.
 *
 * This helper parses that JSON into our `ha_table_option_struct` so create()
 * can build the same tidesdb_column_family_config_t it would have produced
 * under MariaDB. Supported keys (all optional):
 *
 *   "compression"           : "NONE" | "SNAPPY" | "LZ4" | "ZSTD" | "LZ4_FAST"
 *   "bloom_filter"          : true | false | 1 | 0
 *   "write_buffer_size"     : <number bytes>
 *   "ttl"                   : <number seconds>
 *   "block_indexes"         : bool
 *   "encrypted"             : bool   (per-table data-at-rest encryption opt-in)
 *   "encryption_key_id"     : <number>
 *
 * Returns true if the JSON parsed cleanly (or attr was empty); false on
 * malformed JSON. On false, opts is left zeroed and create() falls back to
 * compiled defaults (build_cf_config(nullptr) path). */
/* Parse an ENGINE_ATTRIBUTE JSON blob into an ha_table_option_struct.
 * The caller is expected to have already seeded *opts with session/global
 * defaults (see tidesdb_seed_opts_from_session); this function only
 * overrides the fields the JSON explicitly specifies. That preserves the
 * MariaDB DSL's SYSVAR-inherits-from-session semantics: omitting a key
 * means "use the session default," not "use zero."
 *
 * Returns true on parse success (empty / NULL attr is also success — no
 * overrides). False on malformed JSON; *opts is left as the caller seeded. */
/* MF-3: hard cap on ENGINE_ATTRIBUTE length. The H-9 fix removed the
   recursion depth crash but the iterative parser still allocates
   O(depth) heap on the persisted attribute, reloaded on every
   OPEN TABLE. Cap at 64KB -- legitimate options are under 1KB; this
   is generous defense against a CREATE TABLE storing a malicious
   attribute that exhausts memory on every open. */
static constexpr size_t TIDESDB_ENGINE_ATTRIBUTE_MAX_LEN = 65536;

static bool tidesdb_engine_attribute_to_options(LEX_CSTRING attr,
                                                ha_table_option_struct *opts) {
    if (!attr.str || attr.length == 0) return true;  /* nothing supplied */

    if (attr.length > TIDESDB_ENGINE_ATTRIBUTE_MAX_LEN) {
        sql_print_warning("[TIDESDB] ENGINE_ATTRIBUTE too large (%zu bytes, "
                          "max %zu); using session defaults",
                          (size_t)attr.length,
                          TIDESDB_ENGINE_ATTRIBUTE_MAX_LEN);
        return false;
    }

    rapidjson::Document doc;
    /* Iterative parser: rapidjson's default recursive-descent parses up
       to 500 nesting levels deep, which on a debug-build mysqld is enough
       to overflow the stack from a single deeply-nested
       ENGINE_ATTRIBUTE='{"a":{"a":{...}}}' table option. The
       kParseIterativeFlag uses an internal stack on the heap with no
       depth limit risk, only memory cost. ENGINE_ATTRIBUTE is persisted
       in the MySQL data dictionary and reloaded on every OPEN TABLE,
       so a single CREATE TABLE with a malicious attribute would crash
       mysqld on every subsequent connection until the table is dropped.
       Combined with the length cap above, the worst case is bounded. */
    doc.Parse<rapidjson::kParseIterativeFlag>(attr.str, attr.length);
    if (doc.HasParseError() || !doc.IsObject()) return false;

    auto get_uint = [&](const char *k, ulonglong &dst) {
        auto it = doc.FindMember(k);
        if (it != doc.MemberEnd() && it->value.IsUint64()) dst = it->value.GetUint64();
        else if (it != doc.MemberEnd() && it->value.IsInt()) dst = (ulonglong)it->value.GetInt();
    };
    auto get_bool = [&](const char *k, bool &dst) {
        auto it = doc.FindMember(k);
        if (it == doc.MemberEnd()) return;
        if (it->value.IsBool()) dst = it->value.GetBool();
        else if (it->value.IsInt()) dst = it->value.GetInt() != 0;
    };

    /* compression: string -> enum index matching compression_names[] */
    auto cit = doc.FindMember("compression");
    if (cit != doc.MemberEnd() && cit->value.IsString()) {
        const char *s = cit->value.GetString();
        if      (!strcasecmp(s, "NONE"))     opts->compression = 0;
        else if (!strcasecmp(s, "SNAPPY"))   opts->compression = 1;
        else if (!strcasecmp(s, "LZ4"))      opts->compression = 2;
        else if (!strcasecmp(s, "ZSTD"))     opts->compression = 3;
        else if (!strcasecmp(s, "LZ4_FAST")) opts->compression = 4;
    }

    get_bool("bloom_filter",      opts->bloom_filter);
    get_bool("block_indexes",     opts->block_indexes);
    get_bool("encrypted",         opts->encrypted);
    get_uint("write_buffer_size", opts->write_buffer_size);
    get_uint("ttl",               opts->ttl);
    get_uint("encryption_key_id", opts->encryption_key_id);
    get_uint("min_disk_space",    opts->min_disk_space);
    get_uint("bloom_fpr",         opts->bloom_fpr);

    return true;
}

/* Populate *opts with the session/global defaults that MariaDB's
 * HA_TOPTION_SYSVAR() pulled in automatically. We do this so the JSON
 * parser only has to override the keys the user actually set; omitting
 * a key means "use the session default," matching the MariaDB semantics.
 *
 * Passing a NULL thd is supported -- THDVAR falls back to the global
 * value in that case, which is what tidesdb_opts_for_table needs when
 * called from a background thread that has no THD attached. */
static void tidesdb_seed_opts_from_session(THD *thd, ha_table_option_struct *opts) {
    opts->write_buffer_size       = THDVAR(thd, default_write_buffer_size);
    opts->min_disk_space          = THDVAR(thd, default_min_disk_space);
    opts->klog_value_threshold    = THDVAR(thd, default_klog_value_threshold);
    opts->sync_interval_us        = THDVAR(thd, default_sync_interval_us);
    opts->index_sample_ratio      = THDVAR(thd, default_index_sample_ratio);
    opts->block_index_prefix_len  = THDVAR(thd, default_block_index_prefix_len);
    opts->level_size_ratio        = THDVAR(thd, default_level_size_ratio);
    opts->min_levels              = THDVAR(thd, default_min_levels);
    opts->dividing_level_offset   = THDVAR(thd, default_dividing_level_offset);
    opts->skip_list_max_level     = THDVAR(thd, default_skip_list_max_level);
    opts->skip_list_probability   = THDVAR(thd, default_skip_list_probability);
    opts->bloom_fpr               = THDVAR(thd, default_bloom_fpr);
    opts->l1_file_count_trigger   = THDVAR(thd, default_l1_file_count_trigger);
    opts->l0_queue_stall_threshold= THDVAR(thd, default_l0_queue_stall_threshold);
    opts->compression             = THDVAR(thd, default_compression);
    opts->sync_mode               = THDVAR(thd, default_sync_mode);
    opts->isolation_level         = THDVAR(thd, default_isolation_level);
    opts->bloom_filter            = THDVAR(thd, default_bloom_filter);
    opts->block_indexes           = THDVAR(thd, default_block_indexes);
    opts->use_btree               = THDVAR(thd, default_use_btree);
    opts->object_lazy_compaction  = THDVAR(thd, default_object_lazy_compaction);
    opts->object_prefetch_compaction = THDVAR(thd, default_object_prefetch_compaction);
    opts->tombstone_density_trigger      = THDVAR(thd, default_tombstone_density_trigger);
    opts->tombstone_density_min_entries  = THDVAR(thd, default_tombstone_density_min_entries);
    /* Per-table opt-in keys with no session default. */
    opts->ttl                = 0;
    opts->encrypted          = false;
    opts->encryption_key_id  = 1;
}

/* Resolve effective ha_table_option_struct* for an already-opened table.
 *
 * Reads TABLE_SHARE::engine_attribute (where MySQL persists ENGINE_ATTRIBUTE
 * across server restarts via the Data Dictionary), seeds from session
 * defaults, then layers JSON overrides on top.
 *
 * Storage is a thread-local struct -- single-handler-instance call paths
 * use the result before issuing the next call, so concurrent handlers on
 * different threads do not contend. Same-thread back-to-back calls for
 * different tables refresh the buffer; this is fine because each TDB_TABLE_OPTIONS
 * use site consumes the pointer immediately.
 *
 * Returns NULL only when called with a NULL table or one without a share. */
/* Populate `out` by seeding from session and overlaying JSON from
   table->s->engine_attribute. Helper extracted so both the share-cache
   warm-up path (called once at open()) and the no-share fallback path
   (e.g. inplace_alter against an altered_table without a TidesDB share)
   can share the same logic. */
static void tidesdb_compute_opts_for_table(const TABLE *table, ha_table_option_struct *out) {
    tidesdb_seed_opts_from_session(current_thd, out);
    if (!table || !table->s) return;
    LEX_CSTRING attr = table->s->engine_attribute;
    if (attr.str && attr.length) {
        if (!tidesdb_engine_attribute_to_options(attr, out)) {
            sql_print_warning("[TIDESDB] ENGINE_ATTRIBUTE on table '%s' is "
                              "malformed JSON; falling back to session defaults",
                              table->s->table_name.str ? table->s->table_name.str : "?");
        }
    }
}

static ha_table_option_struct *tidesdb_opts_for_table(const TABLE *table) {
    if (!table || !table->s) return nullptr;

    /* M-1 + M-13: if the table has a TidesDB_share with a populated
       cache, return its stable pointer. No JSON re-parse, no THDVAR
       loops, no aliased thread-local storage. The share's cached_opts
       is filled exactly once during open() before any TDB_TABLE_OPTIONS
       call sites run; see ha_tidesdb::open.

       MF-1: atomic acquire-load. Non-null implies the pointee is fully
       initialized -- the writer in open() constructs and computes the
       struct contents BEFORE the atomic release-store. */
    if (table->s->ha_share) {
        TidesDB_share *sh = static_cast<TidesDB_share *>(table->s->ha_share);
        ha_table_option_struct *cached =
            sh->cached_opts.load(std::memory_order_acquire);
        if (cached) return cached;
    }

    /* Fallback for paths where the share is not yet populated (CREATE
       TABLE pre-open, inplace_alter against altered_table). One TLS
       struct refreshed per call -- nested calls on the same thread
       overwrite it, which is fine here because every fallback caller
       is a single TDB_TABLE_OPTIONS() consume-immediately site. */
    static thread_local ha_table_option_struct tls;
    tidesdb_compute_opts_for_table(table, &tls);
    return &tls;
}


/* (deleted) #if 0: ha_create_table_option DSL -- MariaDB-only dead code; see git history if needed. */

/* ******************** Field options (per-column) ******************** */

struct ha_field_option_struct
{
    bool ttl; /* marks this column as the per-row TTL source (seconds) */
};

/* (deleted) #if 0: field_option_list -- MariaDB-only dead code; see git history if needed. */

/* ******************** Index options (per-index) ******************** */

struct ha_index_option_struct
{
    bool use_btree; /* per-index B-tree override; -1 = inherit from table */
};

/* (deleted) #if 0: index_option_list -- MariaDB-only dead code; see git history if needed. */

/* ******************** Big-endian helpers for hidden PK ********************
   Hidden-PK rows are keyed by an 8-byte big-endian uint64 so that memcmp
   on the encoded bytes matches numeric ordering of the row id. */

static void encode_be64(uint64_t id, uint8_t *buf)
{
    for (uint i = 0; i < sizeof(uint64_t); i++)
        buf[i] = (uint8_t)(id >> ((sizeof(uint64_t) - 1 - i) * BITS_PER_BYTE));
}

static uint64_t decode_be64(const uint8_t *buf)
{
    uint64_t id = 0;
    for (uint i = 0; i < sizeof(uint64_t); i++) id = (id << BITS_PER_BYTE) | (uint64_t)buf[i];
    return id;
}

/*
  Return true if a TidesDB key is a data key (starts with KEY_NS_DATA).
*/
static inline bool is_data_key(const uint8_t *key, size_t key_size)
{
    return key_size > 0 && key[0] == KEY_NS_DATA;
}

/* Shared enum-to-constant maps (used by create, open, prepare_inplace) */

static const int tdb_compression_map[] = {TDB_COMPRESS_NONE, TDB_COMPRESS_SNAPPY, TDB_COMPRESS_LZ4,
                                          TDB_COMPRESS_ZSTD, TDB_COMPRESS_LZ4_FAST};

static const int tdb_sync_mode_map[] = {TDB_SYNC_NONE, TDB_SYNC_INTERVAL, TDB_SYNC_FULL};

static const int tdb_isolation_map[] = {TDB_ISOLATION_READ_UNCOMMITTED,
                                        TDB_ISOLATION_READ_COMMITTED, TDB_ISOLATION_REPEATABLE_READ,
                                        TDB_ISOLATION_SNAPSHOT, TDB_ISOLATION_SERIALIZABLE};

/*
  Map MariaDB session isolation level (from SET TRANSACTION ISOLATION LEVEL)
  to TidesDB isolation level.  Falls back to table-level ISOLATION_LEVEL
  option only when the session has the default (REPEATABLE READ) and the
  table overrides it.

  MariaDB enum_tx_isolation:
    ISO_READ_UNCOMMITTED = 0
    ISO_READ_COMMITTED   = 1
    ISO_REPEATABLE_READ  = 2
    ISO_SERIALIZABLE     = 3

  TidesDB has a 5th level (SNAPSHOT) that has no SQL equivalent.
  It can only be selected via the table option.  When the session
  isolation is REPEATABLE READ and the table option specifies SNAPSHOT,
  we honor the table-level SNAPSHOT setting.
*/
static tidesdb_isolation_level_t resolve_effective_isolation(THD *thd,
                                                             tidesdb_isolation_level_t table_iso)
{
    /* Pessimistic locking serialises every writer through the row-lock
       manager at the SQL layer, so TidesDB's commit-time conflict
       detection is not just unnecessary but actively harmful: it sees
       every committed write since txn start as a "conflict candidate"
       even when our SQL-layer locks already guaranteed the write was
       safe. TidesDB's READ_COMMITTED has no conflict detection, which
       is exactly what we want -- mirrors how InnoDB's pessimistic-lock
       path runs without OCC validation. Without this, the
       tidesdb_pessimistic_insert_lock pattern (conA holds row lock on
       i=3 while conB autocommit-deletes i=4 then blocks on i=3)
       false-positives on conA's COMMIT because conB's i=4 commit
       advances the global commit sequence. */
    if (srv_pessimistic_locking)
        return TDB_ISOLATION_READ_COMMITTED;

    int session_iso = thd_tx_isolation(thd);

    switch (session_iso)
    {
        case ISO_READ_UNCOMMITTED:
            return TDB_ISOLATION_READ_UNCOMMITTED;
        case ISO_READ_COMMITTED:
            return TDB_ISOLATION_READ_COMMITTED;
        case ISO_REPEATABLE_READ:
            /* InnoDB's REPEATABLE_READ is MVCC snapshot reads with
               pessimistic row locks -- no read-set conflict detection.
               TidesDB's closest equivalent is SNAPSHOT isolation:
               consistent read snapshot + write-write conflict only.
               TidesDB's REPEATABLE_READ is stricter (tracks read-set,
               detects read-write conflicts at commit) and causes
               excessive TDB_ERR_CONFLICT under normal OLTP concurrency.
               Map MariaDB RR -> TidesDB SNAPSHOT for InnoDB parity. */
            return TDB_ISOLATION_SNAPSHOT;
        case ISO_SERIALIZABLE:
            return TDB_ISOLATION_SERIALIZABLE;
        default:
            return TDB_ISOLATION_READ_COMMITTED;
    }
}

/* Single-byte placeholder value for secondary index entries (all info is in the key) */
static const uint8_t tdb_empty_val = 0;

/*
  Build a tidesdb_column_family_config_t from table options.
  Centralises the option-to-config mapping so create() and
  prepare_inplace_alter_table() stay in sync.
*/
static tidesdb_column_family_config_t build_cf_config(const ha_table_option_struct *opts)
{
    tidesdb_column_family_config_t cfg = tidesdb_default_column_family_config();
    if (!opts) return cfg;

    cfg.write_buffer_size = (size_t)opts->write_buffer_size;
    cfg.compression_algorithm = (compression_algorithm)tdb_compression_map[opts->compression];
    cfg.enable_bloom_filter = opts->bloom_filter ? 1 : 0;
    cfg.bloom_fpr = (double)opts->bloom_fpr / TIDESDB_BLOOM_FPR_DIVISOR;
    cfg.enable_block_indexes = opts->block_indexes ? 1 : 0;
    cfg.index_sample_ratio = (int)opts->index_sample_ratio;
    cfg.block_index_prefix_len = (int)opts->block_index_prefix_len;
    cfg.sync_mode = tdb_sync_mode_map[opts->sync_mode];
    cfg.sync_interval_us = (uint64_t)opts->sync_interval_us;
    cfg.klog_value_threshold = (size_t)opts->klog_value_threshold;
    cfg.min_disk_space = (size_t)opts->min_disk_space;
    cfg.default_isolation_level =
        (tidesdb_isolation_level_t)tdb_isolation_map[opts->isolation_level];
    cfg.level_size_ratio = (int)opts->level_size_ratio;
    cfg.min_levels = (int)opts->min_levels;
    cfg.dividing_level_offset = (int)opts->dividing_level_offset;
    cfg.skip_list_max_level = (int)opts->skip_list_max_level;
    cfg.skip_list_probability = (float)opts->skip_list_probability / TIDESDB_SKIP_LIST_PROB_DIV;
    cfg.l1_file_count_trigger = (int)opts->l1_file_count_trigger;
    cfg.l0_queue_stall_threshold = (int)opts->l0_queue_stall_threshold;
    cfg.use_btree = opts->use_btree ? 1 : 0;
    cfg.object_lazy_compaction = opts->object_lazy_compaction ? 1 : 0;
    cfg.object_prefetch_compaction = opts->object_prefetch_compaction ? 1 : 0;
    cfg.tombstone_density_trigger =
        (double)opts->tombstone_density_trigger / TIDESDB_TOMBSTONE_DENSITY_DIVISOR;
    cfg.tombstone_density_min_entries = (uint64_t)opts->tombstone_density_min_entries;
    return cfg;
}

/*
  Resolve a secondary index CF by name.
  Returns the CF pointer (may be NULL if not found).
  Writes the CF name into out_name.
*/
static tidesdb_column_family_t *resolve_idx_cf(tidesdb_t *db, const std::string &table_cf,
                                               const char *key_name, std::string &out_name)
{
    out_name = table_cf + CF_INDEX_INFIX + key_name;
    return tidesdb_get_column_family(db, out_name.c_str());
}

/* ******************** TidesDB_share ******************** */

TidesDB_share::TidesDB_share()
    : cf(NULL),
      has_user_pk(false),
      pk_index(0),
      pk_key_len(0),
      next_row_id(1),
      isolation_level(TDB_ISOLATION_REPEATABLE_READ),
      default_ttl(0),
      ttl_field_idx(TIDESDB_TTL_FIELD_NONE),
      has_blobs(false),
      has_ttl(false),
      num_secondary_indexes(0)
{
    memset(idx_comp_key_len, 0, sizeof(idx_comp_key_len));
    memset(idx_is_fts, 0, sizeof(idx_is_fts));
    memset(idx_is_spatial, 0, sizeof(idx_is_spatial));
    for (uint i = 0; i < MAX_KEY; i++) cached_rec_per_key[i].store(0, std::memory_order_relaxed);
    /* HF-2 fix: per-share FTS-meta serialization mutex. Replaces a
       former process-global g_fts_meta_mutex. */
    mysql_mutex_init(0, &fts_meta_mutex, MY_MUTEX_INIT_FAST);
}

TidesDB_share::~TidesDB_share()
{
    /* No concurrent readers possible by share-destruction time: MySQL
       guarantees no handler holds the share when the share is freed.
       Plain load is fine here. */
    ha_table_option_struct *opts =
        cached_opts.load(std::memory_order_relaxed);
    delete opts;
    cached_opts.store(nullptr, std::memory_order_relaxed);
    mysql_mutex_destroy(&fts_meta_mutex);
}

/* ******************** Per-connection transaction helpers ******************** */

/*
  Get or create the per-connection TidesDB transaction context.
  The txn lives for the entire BEGIN...COMMIT block (or single auto-commit
  statement).  All handler objects on the same connection share it.
*/
static tidesdb_trx_t *get_or_create_trx(THD *thd, handlerton *hton, tidesdb_isolation_level_t iso)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, hton);
    if (trx)
    {
        if (!trx->txn)
        {
            int rc = tidesdb_txn_begin_with_isolation(tdb_get_engine(), iso, &trx->txn);
            if (rc != TDB_SUCCESS)
            {
                (void)tdb_rc_to_ha(rc, "get_or_create_trx txn_begin(reuse)");
                return NULL;
            }
            trx->dirty = false;
            trx->isolation_level = iso;
            trx->txn_generation++;
        }
        else if (trx->needs_reset)
        {
            /* Txn object kept alive from previous commit/rollback (see
               tidesdb_commit).  We reset it to get a fresh MVCC snapshot at
               current-transaction-start.  This avoids the expensive
               free+begin cycle while ensuring we see the latest data.
               The bulk-insert path already uses commit+reset successfully.
               Only reset when needs_reset is true (set after real commit/
               rollback) to preserve snapshot within multi-statement txns. */
            int rrc = tidesdb_txn_reset(trx->txn, iso);
            if (rrc != TDB_SUCCESS)
            {
                /* Reset failed -- we fall back to free + begin.  Surface the
                   failure so we can spot regressions in txn recycling instead
                   of silently degrading to per-statement free+begin. */
                sql_print_warning(
                    "[TIDESDB] tidesdb_txn_reset failed (rc=%d), falling back to "
                    "free+begin -- expect higher per-statement overhead until "
                    "this is investigated",
                    rrc);
                tidesdb_txn_free(trx->txn);
                trx->txn = NULL;
                int rc = tidesdb_txn_begin_with_isolation(tdb_get_engine(), iso, &trx->txn);
                if (rc != TDB_SUCCESS)
                {
                    (void)tdb_rc_to_ha(rc, "get_or_create_trx txn_begin(reset_fallback)");
                    return NULL;
                }
            }
            trx->needs_reset = false;
            trx->isolation_level = iso;
            trx->txn_generation++;
        }
        return trx;
    }

    trx = (tidesdb_trx_t *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(tidesdb_trx_t), MYF(MY_ZEROFILL));
    if (!trx) return NULL;

    int rc = tidesdb_txn_begin_with_isolation(tdb_get_engine(), iso, &trx->txn);
    if (rc != TDB_SUCCESS)
    {
        my_free(trx);
        (void)tdb_rc_to_ha(rc, "get_or_create_trx txn_begin(new)");
        return NULL;
    }
    trx->dirty = false;
    trx->isolation_level = iso;
    trx->txn_generation = 1;
    trx->lock_txn_id = 1;
    trx->held_locks_head = NULL;
    trx->waiting_on.store(NULL, std::memory_order_relaxed);
    thd_set_ha_data(thd, hton, trx);
    return trx;
}

/* ******************** Handlerton transaction callbacks ******************** */

/* Maximum length of a TidesDB savepoint name, including the trailing NUL.
   Names are synthesized via TIDESDB_SAVEPOINT_NAME_FMT below; 32 bytes
   fits the decoded pointer plus prefix on all supported platforms. */
static constexpr uint TIDESDB_SAVEPOINT_NAME_MAX = 32;
/* Format used to synthesize a unique savepoint name for the TidesDB
   transaction layer.  The pointer to the SQL-layer savepoint slot is
   the only handle we have that survives across the set/rollback/release
   callbacks, so we encode it as the engine-level savepoint name. */
static constexpr const char TIDESDB_SAVEPOINT_NAME_FMT[] = "sv_%p";

struct tidesdb_savepoint_t
{
    char name[TIDESDB_SAVEPOINT_NAME_MAX];
};

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_savepoint_set(THD *thd, void *sv)
#else
static int tidesdb_savepoint_set(handlerton *, THD *thd, void *sv)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn || !sv) return 0;

    tidesdb_savepoint_t *sp = (tidesdb_savepoint_t *)sv;
    snprintf(sp->name, sizeof(sp->name), TIDESDB_SAVEPOINT_NAME_FMT, sv);

    int rc = tidesdb_txn_savepoint(trx->txn, sp->name);
    if (rc == TDB_SUCCESS) return 0;
    return tdb_rc_to_ha(rc, "savepoint_set");
}

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_savepoint_rollback(THD *thd, void *sv)
#else
static int tidesdb_savepoint_rollback(handlerton *, THD *thd, void *sv)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn || !sv) return 0;

    tidesdb_savepoint_t *sp = (tidesdb_savepoint_t *)sv;
    if (!sp->name[0]) snprintf(sp->name, sizeof(sp->name), TIDESDB_SAVEPOINT_NAME_FMT, sv);

    int rc = tidesdb_txn_rollback_to_savepoint(trx->txn, sp->name);
    if (rc == TDB_SUCCESS)
    {
        /* The TidesDB library may drop the savepoint as part of the rollback.
           SQL semantics require the savepoint to still exist after rollback,
           so we re-create it here to allow RELEASE SAVEPOINT to succeed. */
        (void)tidesdb_txn_savepoint(trx->txn, sp->name);
        return 0;
    }
    if (rc == TDB_ERR_NOT_FOUND) return HA_ERR_NO_SAVEPOINT;
    return tdb_rc_to_ha(rc, "savepoint_rollback");
}

#if MYSQL_VERSION_ID >= 110800
static bool tidesdb_savepoint_rollback_can_release_mdl(THD *)
#else
static bool tidesdb_savepoint_rollback_can_release_mdl(handlerton *, THD *)
#endif
{
    return true;
}

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_savepoint_release(THD *thd, void *sv)
#else
static int tidesdb_savepoint_release(handlerton *, THD *thd, void *sv)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn || !sv) return 0;

    tidesdb_savepoint_t *sp = (tidesdb_savepoint_t *)sv;
    if (!sp->name[0]) snprintf(sp->name, sizeof(sp->name), TIDESDB_SAVEPOINT_NAME_FMT, sv);

    int rc = tidesdb_txn_release_savepoint(trx->txn, sp->name);
    if (rc == TDB_SUCCESS) return 0;
    if (rc == TDB_ERR_NOT_FOUND) return HA_ERR_NO_SAVEPOINT;
    return tdb_rc_to_ha(rc, "savepoint_release");
}

/*
  2PC prepare phase.

  MySQL's binlog-driven 2PC orders engine prepares and commits around the
  binlog flush. When binlog is enabled (MTR default), MySQL calls this
  hook before the binlog write; only after the binlog flush succeeds does
  it call tidesdb_commit.

  We move the actual TidesDB commit into prepare so that conflict errors
  surface via HA_ERR_LOCK_DEADLOCK *before* binlog ordering. This avoids
  the Debug-only assertion at sql/binlog.cc:7756
  (`thd->commit_error != THD::CE_COMMIT_ERROR`) that fires whenever a
  commit hook returns non-zero, and lets the user see ER_LOCK_DEADLOCK
  cleanly -- exactly what the conflict tests expect.

  Risks:
   * If mysqld dies between prepare-success and commit, the txn is
     already durable in TidesDB but won't be in the binlog. There is no
     XA recovery path; this is consistent with the prior "commit at
     hton commit" behavior on crash and acceptable for this engine.
   * We don't register commit_by_xid / rollback_by_xid -- TidesDB has no
     prepare/commit_xid split, and the unrecovered case is the same.

  When prepare runs and succeeds, commit_done is set so the subsequent
  commit hook is a pure bookkeeping no-op.
*/
static int tidesdb_prepare(handlerton *, THD *thd, bool all)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn) return 0;

    bool is_real_commit =
        all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
    if (!is_real_commit) return 0;

    /* Read-only txn -- no TidesDB commit needed; let commit hook clean up. */
    if (!trx->dirty) return 0;

    /* Release any active stmt savepoint before commit (TidesDB requires this). */
    if (trx->stmt_savepoint_active)
    {
        tidesdb_txn_release_savepoint(trx->txn, "stmt");
        trx->stmt_savepoint_active = false;
    }

    int rc = tidesdb_txn_commit(trx->txn);
    if (rc != TDB_SUCCESS)
    {
        /* Truly unexpected errors get logged; transient conflicts don't spam. */
        if (rc != TDB_ERR_CONFLICT && rc != TDB_ERR_LOCKED && rc != TDB_ERR_MEMORY_LIMIT)
            sql_print_error(
                "[TIDESDB] hton_prepare: tidesdb_txn_commit returned %d "
                "(dirty=%d gen=%lu)",
                rc, trx->dirty, (unsigned long)trx->txn_generation);
        tidesdb_txn_rollback(trx->txn);
        tidesdb_txn_free(trx->txn);
        trx->txn = NULL;
        trx->txn_generation++;
        trx->dirty = false;
        trx->stmt_savepoint_active = false;
        trx->commit_done = false;
        row_locks_release_all(trx);
        /* Surfaces to user as ER_LOCK_DEADLOCK / similar. Because this is
           prepare (not commit), the binlog assertion does NOT fire. */
        return tdb_rc_to_ha(rc, "hton_prepare");
    }

    /* TidesDB-side commit done. Mark so the commit hook skips the actual
       commit work. The txn handle stays alive for txn_reset reuse. */
    trx->commit_done = true;
    trx->txn_generation++;
    trx->needs_reset = true;
    return 0;
}

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_commit(THD *thd, bool all)
#else
static int tidesdb_commit(handlerton *, THD *thd, bool all)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn)
    {
        return 0;
    }

    /* We determine whether this is the final commit for the transaction.
       all=true         -> explicit COMMIT or transaction-level end
       all=false        -> statement-level; only a real commit when autocommit */
    bool is_real_commit = all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

    if (!is_real_commit)
    {
        /* Statement-level commit inside a multi-statement transaction.
           Defer the actual commit -- writes stay buffered in the txn,
           avoiding expensive txn_begin + commit per statement.

           tidesdb_txn_savepoint() deep-copies the entire
           write-set (malloc+memcpy for every key/value).  For a txn
           with N ops across S statements, total copy cost is
           O(S * N * avg_kv_size) -- quadratic and devastating for
           multi-statement OLTP transactions.

           We skip the per-statement savepoint entirely.  This means
           statement-level rollback inside BEGIN...COMMIT falls back to
           full transaction rollback (same as many simple SE's).
           The trade-off is a statement failure aborts the entire txn
           instead of undoing just that statement.  For OLTP this is
           acceptable since the client will retry the whole transaction
           anyway after a conflict/error. */
        trx->stmt_was_dirty = false;
        return 0;
    }

    /* If prepare already committed to TidesDB, this is the binlog-ordering
       commit hook -- pure bookkeeping. */
    if (trx->commit_done)
    {
        trx->commit_done = false;
        trx->dirty = false;
        trx->stmt_savepoint_active = false;
        row_locks_release_all(trx);
        return 0;
    }

    /* We must release any active statement savepoint before final commit/rollback.
       Savepoints must be explicitly released before txn_commit. */
    if (trx->stmt_savepoint_active)
    {
        tidesdb_txn_release_savepoint(trx->txn, "stmt");
        trx->stmt_savepoint_active = false;
    }

    /* Single-phase path -- no prepare was called (e.g. binlog disabled, or
       autocommit statement on read-only txn). Do the same work prepare
       would have done. The Debug-only binlog assertion only fires when
       2PC is engaged via binlog, so returning the conflict error here is
       safe in this path. */
    if (trx->dirty)
    {
        int rc = tidesdb_txn_commit(trx->txn);
        if (rc != TDB_SUCCESS)
        {
            if (rc != TDB_ERR_CONFLICT && rc != TDB_ERR_LOCKED && rc != TDB_ERR_MEMORY_LIMIT)
                sql_print_error(
                    "[TIDESDB] hton_commit: tidesdb_txn_commit returned %d "
                    "(dirty=%d gen=%lu)",
                    rc, trx->dirty, (unsigned long)trx->txn_generation);
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
            trx->txn = NULL;
            trx->txn_generation++;
            trx->dirty = false;
            trx->stmt_savepoint_active = false;
            row_locks_release_all(trx);
            return tdb_rc_to_ha(rc, "hton_commit");
        }
        trx->txn_generation++;
        trx->needs_reset = true;
    }
    else
    {
        /* Read-only transaction -- we rollback, keep alive for reuse. */
        tidesdb_txn_rollback(trx->txn);
        trx->txn_generation++;
        trx->needs_reset = true;
    }
    trx->dirty = false;
    trx->stmt_savepoint_active = false;
    row_locks_release_all(trx);
    return 0;
}

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_rollback(THD *thd, bool all)
#else
static int tidesdb_rollback(handlerton *, THD *thd, bool all)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn) return 0;

    bool is_real_rollback = all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

    if (!is_real_rollback)
    {
        /* Statement-level rollback inside a multi-statement transaction.
           Without per-statement savepoints (see tidesdb_commit note),
           we fall through to full transaction rollback.  This is the
           same behavior as many simple storage engines and is correct --
           OLTP clients retry the entire transaction after any error. */
    }

    /* We release any user-created savepoints before full rollback. */
    if (trx->stmt_savepoint_active)
    {
        tidesdb_txn_release_savepoint(trx->txn, "stmt");
        trx->stmt_savepoint_active = false;
    }

    /* Full rollback -- we keep txn alive for reuse via reset on next use. */
    tidesdb_txn_rollback(trx->txn);
    trx->txn_generation++;
    trx->needs_reset = true;
    trx->dirty = false;
    trx->stmt_savepoint_active = false;
    /* Edge case: prepare succeeded but binlog flush then failed -> MySQL
       calls rollback after prepare. The TidesDB txn was already committed,
       so this rollback is a no-op for the data; just clear the flag so a
       fresh txn starts clean. */
    trx->commit_done = false;
    row_locks_release_all(trx);
    return 0;
}

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_close_connection(THD *thd)
#else
static int tidesdb_close_connection(handlerton *, THD *thd)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (trx)
    {
        row_locks_release_all(trx);
        if (trx->txn)
        {
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
        }
        /* Take the trx-lifecycle write-lock around my_free so any
           in-flight deadlock walker (which holds the read-lock for the
           duration of its walk) cannot dereference this struct after
           it's freed. row_locks_release_all already cleared the lock
           table's owner_trx pointers atomically, so no new walker can
           reach `trx` after we get the write-lock; the write-lock
           guarantees no existing walker is still mid-deref. */
        mysql_rwlock_wrlock(&g_trx_lifecycle_lock);
        my_free(trx);
        mysql_rwlock_unlock(&g_trx_lifecycle_lock);
        thd_set_ha_data(thd, tidesdb_hton, NULL);
    }
    return 0;
}

/*
  START TRANSACTION WITH CONSISTENT SNAPSHOT callback.
  Eagerly creates a TidesDB transaction so the snapshot sequence number
  is captured now, not lazily at first data access.  Without this, rows
  committed by other connections between START TRANSACTION and the first
  SELECT would be visible.

  Uses the session's isolation level (SET TRANSACTION ISOLATION LEVEL)
  rather than hard-coding REPEATABLE_READ.  Falls back to RR if the
  session is at the default.
*/
#if MYSQL_VERSION_ID >= 110800
static int tidesdb_start_consistent_snapshot(THD *thd)
#else
static int tidesdb_start_consistent_snapshot(handlerton *, THD *thd)
#endif
{
    /* START TRANSACTION WITH CONSISTENT SNAPSHOT explicitly requests a
       point-in-time snapshot.  Always use at least SNAPSHOT isolation
       so the snapshot persists for the entire transaction, regardless of
       the session's default isolation level (e.g. READ_COMMITTED would
       refresh the snapshot on each read, violating CONSISTENT_SNAPSHOT
       semantics). */
    tidesdb_isolation_level_t iso = resolve_effective_isolation(thd, TDB_ISOLATION_REPEATABLE_READ);
    if (iso < TDB_ISOLATION_SNAPSHOT) iso = TDB_ISOLATION_SNAPSHOT;
    tidesdb_trx_t *trx = get_or_create_trx(thd, tidesdb_hton, iso);
    if (!trx) return 1;

    /* We register at both statement and transaction level so the server
       knows TidesDB is participating in this BEGIN block. */
    trans_register_ha(thd, false, tidesdb_hton, 0);
    trans_register_ha(thd, true, tidesdb_hton, 0);
    return 0;
}

/* ******************** SHOW ENGINE TIDESDB STATUS ******************** */

static bool tidesdb_show_status(handlerton *hton, THD *thd, stat_print_fn *print,
                                enum ha_stat_type stat)
{
    if (stat != HA_ENGINE_STATUS) return false;
    if (!tdb_get_engine()) return false;

    /* We refresh SHOW GLOBAL STATUS variables alongside the human-readable output */
    tidesdb_refresh_status_vars();

    /* Database-level stats */
    tidesdb_db_stats_t db_st;
    memset(&db_st, 0, sizeof(db_st));
    tidesdb_get_db_stats(tdb_get_engine(), &db_st);

    /* Cache stats */
    tidesdb_cache_stats_t cache_st;
    memset(&cache_st, 0, sizeof(cache_st));
    tidesdb_get_cache_stats(tdb_get_engine(), &cache_st);

    /* Output buffer for SHOW ENGINE TIDESDB STATUS.  8 KiB is enough to
       hold the fixed-format sections plus an optional object-store block
       and the last-conflict line without truncating. */
    static constexpr uint TIDESDB_STATUS_BUF_LEN = 8192;
    char buf[TIDESDB_STATUS_BUF_LEN];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "================== TidesDB Engine Status ==================\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Data directory: %s\n", g_engine_ctx.path.c_str());
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Unified memtable: %s\n",
                    srv_unified_memtable ? "ON" : "OFF");
    pos +=
        snprintf(buf + pos, sizeof(buf) - pos, "Column families: %d\n", db_st.num_column_families);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Global sequence: %lu\n",
                    (unsigned long)db_st.global_seq);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Memory ---\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Total system memory: %lu MB\n",
                    (unsigned long)(db_st.total_memory / (1024 * 1024)));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Resolved memory limit: %lu MB\n",
                    (unsigned long)(db_st.resolved_memory_limit / (1024 * 1024)));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Memory pressure level: %d\n",
                    db_st.memory_pressure_level);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Total memtable bytes: %ld\n",
                    (long)db_st.total_memtable_bytes);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Transaction memory bytes: %ld\n",
                    (long)db_st.txn_memory_bytes);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Storage ---\n");
    pos +=
        snprintf(buf + pos, sizeof(buf) - pos, "Total SSTables: %d\n", db_st.total_sstable_count);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Open SSTable handles: %d\n",
                    db_st.num_open_sstables);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Total data size: %lu bytes\n",
                    (unsigned long)db_st.total_data_size_bytes);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Immutable memtables: %d\n",
                    db_st.total_immutable_count);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Background ---\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Flush pending: %d\n", db_st.flush_pending_count);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Flush queue size: %lu\n",
                    (unsigned long)db_st.flush_queue_size);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Compaction queue size: %lu\n",
                    (unsigned long)db_st.compaction_queue_size);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Block Cache ---\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Enabled: %s\n", cache_st.enabled ? "YES" : "NO");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Entries: %lu\n",
                    (unsigned long)cache_st.total_entries);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Size: %lu bytes\n",
                    (unsigned long)cache_st.total_bytes);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Hits: %lu\n", (unsigned long)cache_st.hits);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Misses: %lu\n", (unsigned long)cache_st.misses);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Hit rate: %.1f%%\n",
                    cache_st.hit_rate * PERCENT_SCALE);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Partitions: %lu\n",
                    (unsigned long)cache_st.num_partitions);

    /* Tombstone observability.  Aggregates are populated by the
       tidesdb_refresh_status_vars call at the top of this function, which
       walks all CFs once. */
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Tombstones ---\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Total tombstones: %ld\n",
                    (long)srv_stat_total_tombstones);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Tombstone ratio: %.2f%%\n",
                    srv_stat_tombstone_ratio * PERCENT_SCALE);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Worst SSTable density: %.2f%% at level %ld\n",
                    srv_stat_max_sst_density * PERCENT_SCALE, (long)srv_stat_max_sst_density_level);

    /* Object store stats */
    if (db_st.object_store_enabled)
    {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Object Store ---\n");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Connector: %s\n",
                        db_st.object_store_connector ? db_st.object_store_connector : "unknown");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Total uploads: %lu\n",
                        (unsigned long)db_st.total_uploads);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Upload failures: %lu\n",
                        (unsigned long)db_st.total_upload_failures);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Upload queue depth: %lu\n",
                        (unsigned long)db_st.upload_queue_depth);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Local cache: %lu / %lu bytes (%d files)\n",
                        (unsigned long)db_st.local_cache_bytes_used,
                        (unsigned long)db_st.local_cache_bytes_max, db_st.local_cache_num_files);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Replica mode: %s\n",
                        db_st.replica_mode ? "ON" : "OFF");
    }

    /* Last conflict info */
    mysql_mutex_lock(&g_engine_ctx.last_conflict_mutex);
    if (g_engine_ctx.last_conflict_info[0])
        pos +=
            snprintf(buf + pos, sizeof(buf) - pos, "\n--- Conflicts ---\n%s\n", g_engine_ctx.last_conflict_info);
    mysql_mutex_unlock(&g_engine_ctx.last_conflict_mutex);

    static constexpr const char TIDESDB_ENGINE_NAME[] = "TIDESDB";
    static constexpr uint TIDESDB_ENGINE_NAME_LEN = sizeof(TIDESDB_ENGINE_NAME) - 1;
    return print(thd, TIDESDB_ENGINE_NAME, TIDESDB_ENGINE_NAME_LEN, "", 0, buf, (size_t)pos);
}

/* ******************** Schema discovery (object store mode) ******************** */
/*
  The __tidesql_schema column family records table CF-names so DROP TABLE
  can find the underlying CF, and (in object-store mode) which database
  directories to create on replicas. The MariaDB discover_table /
  discover_table_names hooks that originally consumed .frm binaries from
  this CF are #if 0'd for MySQL -- MySQL uses the Data Dictionary instead,
  so no .frm bytes are stored here. On local-only mode g_engine_ctx.schema_cf is NULL
  and all helpers are no-ops.
*/

/*
  Build a schema CF key from db + table LEX_CSTRINGs.
  Format-- "db_name\0table_name" (null byte separator, no trailing null).
*/
static std::string schema_cf_key(const LEX_CSTRING &db, const LEX_CSTRING &tbl)
{
    std::string k;
    k.reserve(db.length + sizeof(SCHEMA_CF_KEY_SEP) + tbl.length);
    k.append(db.str, db.length);
    k.push_back(SCHEMA_CF_KEY_SEP);
    k.append(tbl.str, tbl.length);
    return k;
}

/*
  Build a schema CF key from a MariaDB table path (e.g. "./db/table").
  Extracts the db and table components using the same logic as path_to_cf_name.
*/
static std::string schema_cf_key_from_path(const char *path)
{
    std::string p(path);

    if (p.size() >= MARIADB_REL_PATH_PREFIX_LEN &&
        p.compare(0, MARIADB_REL_PATH_PREFIX_LEN, MARIADB_REL_PATH_PREFIX) == 0)
        p = p.substr(MARIADB_REL_PATH_PREFIX_LEN);

    size_t last_slash = p.rfind('/');
    if (last_slash == std::string::npos)
    {
        /* No slashes -- we treat entire path as table name with empty db */
        std::string k;
        k.push_back(SCHEMA_CF_KEY_SEP);
        k.append(p);
        return k;
    }

    std::string tblname = p.substr(last_slash + 1);

    size_t prev_slash = (last_slash > 0) ? p.rfind('/', last_slash - 1) : std::string::npos;
    std::string dbname;
    if (prev_slash == std::string::npos)
        dbname = p.substr(0, last_slash);
    else
        dbname = p.substr(prev_slash + 1, last_slash - prev_slash - 1);

    std::string k;
    k.reserve(dbname.size() + sizeof(SCHEMA_CF_KEY_SEP) + tblname.size());
    k.append(dbname);
    k.push_back(SCHEMA_CF_KEY_SEP);
    k.append(tblname);
    return k;
}

/*
  Store a .frm image in the schema CF.

  When frm_data/frm_len are provided the image is used directly (this is
  the normal path during CREATE TABLE -- MariaDB skips writing .frm to
  disk when discover_table is registered on the handlerton).

  When frm_data is NULL, the .frm is read from disk (ALTER TABLE path
  where MariaDB writes the updated .frm before calling commit).

  No-op when g_engine_ctx.schema_cf is NULL (local-only mode).
*/
static int schema_cf_store_frm(const char *path, const uchar *frm_data = NULL, size_t frm_len = 0)
{
    if (!g_engine_ctx.schema_cf) return 0;

    uchar *alloc_buf = NULL;

    if (!frm_data)
    {
        /* We read .frm from disk as fallback */
        char frm_path[FN_REFLEN];
        fn_format(frm_path, path, "", reg_ext, MY_UNPACK_FILENAME | MY_APPEND_EXT);

        MY_STAT st;
        if (!my_stat(frm_path, &st, MYF(0))) return 0; /* .frm not on disk -- not fatal */

        File fd = my_open(frm_path, O_RDONLY, MYF(0));
        if (fd < 0) return 0;

        frm_len = (size_t)st.st_size;
        alloc_buf = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, frm_len, MYF(0));
        if (!alloc_buf)
        {
            my_close(fd, MYF(0));
            return -1;
        }

        if (my_read(fd, alloc_buf, frm_len, MYF(MY_NABP)) != 0)
        {
            my_free(alloc_buf);
            my_close(fd, MYF(0));
            return 0;
        }
        my_close(fd, MYF(0));
        frm_data = alloc_buf;
    }

    std::string key = schema_cf_key_from_path(path);

    /* We write to schema CF using a dedicated one-shot transaction */
    tidesdb_txn_t *txn = NULL;
    int rc = tidesdb_txn_begin(tdb_get_engine(), &txn);
    if (rc == TDB_SUCCESS)
    {
        rc = tidesdb_txn_put(txn, g_engine_ctx.schema_cf, (const uint8_t *)key.data(), key.size(), frm_data,
                             frm_len, TIDESDB_TTL_NONE);
        if (rc == TDB_SUCCESS)
            rc = tidesdb_txn_commit(txn);
        else
            tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
    }

    if (alloc_buf) my_free(alloc_buf);
    return (rc == TDB_SUCCESS) ? 0 : -1;
}

/*
  Remove a table's .frm entry from the schema CF on DROP TABLE.
*/
static void schema_cf_delete(const char *path)
{
    if (!g_engine_ctx.schema_cf) return;

    std::string key = schema_cf_key_from_path(path);
    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_get_engine(), &txn) == TDB_SUCCESS)
    {
        tidesdb_txn_delete(txn, g_engine_ctx.schema_cf, (const uint8_t *)key.data(), key.size());
        tidesdb_txn_commit(txn);
        tidesdb_txn_free(txn);
    }
}

/*
  Remove every schema CF entry belonging to a dropped database.
  Keys are "db_name\0table_name" so we iterate the CF and delete entries
  whose prefix matches.  No-op in local-only mode (g_engine_ctx.schema_cf is NULL).
*/
static void schema_cf_delete_db(const std::string &db_name)
{
    if (!g_engine_ctx.schema_cf || db_name.empty()) return;

    /* Match keys beginning with "db_name<SCHEMA_CF_KEY_SEP>". */
    std::string prefix = db_name;
    prefix.push_back(SCHEMA_CF_KEY_SEP);

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_get_engine(), &txn) != TDB_SUCCESS) return;

    tidesdb_iter_t *it = NULL;
    if (tidesdb_iter_new(txn, g_engine_ctx.schema_cf, &it) != TDB_SUCCESS)
    {
        tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        return;
    }

    std::vector<std::string> to_delete;
    tidesdb_iter_seek(it, (const uint8_t *)prefix.data(), prefix.size());
    while (tidesdb_iter_valid(it))
    {
        uint8_t *k = NULL;
        size_t klen = 0;
        if (tidesdb_iter_key(it, &k, &klen) != TDB_SUCCESS) break;
        if (klen < prefix.size() || memcmp(k, prefix.data(), prefix.size()) != 0) break;
        to_delete.emplace_back((const char *)k, klen);
        tidesdb_iter_next(it);
    }
    tidesdb_iter_free(it);

    for (const auto &k : to_delete)
        tidesdb_txn_delete(txn, g_engine_ctx.schema_cf, (const uint8_t *)k.data(), k.size());

    if (!to_delete.empty())
        tidesdb_txn_commit(txn);
    else
        tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
}

/*
  Rename a table's schema CF entry (delete old key, insert under new key).
  Called from rename_table().
*/
static void schema_cf_rename(const char *from, const char *to)
{
    if (!g_engine_ctx.schema_cf) return;

    std::string old_key = schema_cf_key_from_path(from);
    std::string new_key = schema_cf_key_from_path(to);

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_get_engine(), &txn) != TDB_SUCCESS) return;

    /* We read existing .frm from old key */
    uint8_t *val = NULL;
    size_t val_len = 0;
    int rc = tidesdb_txn_get(txn, g_engine_ctx.schema_cf, (const uint8_t *)old_key.data(), old_key.size(), &val,
                             &val_len);
    if (rc == TDB_SUCCESS && val)
    {
        /* We write under new key */
        tidesdb_txn_put(txn, g_engine_ctx.schema_cf, (const uint8_t *)new_key.data(), new_key.size(), val,
                        val_len, TIDESDB_TTL_NONE);
        /* We delete old key */
        tidesdb_txn_delete(txn, g_engine_ctx.schema_cf, (const uint8_t *)old_key.data(), old_key.size());
        tidesdb_txn_commit(txn);
        /* Must be tidesdb_free, not libc free -- val was allocated by
           TidesDB's internal allocator, which may be mimalloc / jemalloc
           / tcmalloc depending on build flags. Calling libc free on
           those is heap corruption. */
        tidesdb_free(val);
    }
    else
    {
        tidesdb_txn_rollback(txn);
        if (val) tidesdb_free(val);

        /* We fallback, old key missing? we read .frm from disk at new path */
        schema_cf_store_frm(to);
    }

    tidesdb_txn_free(txn);
}
/* (deleted) comment + #if 0: discover_table -- MariaDB-only dead code; see git history if needed. */

/* (deleted) comment + #if 0: discover_table_names -- MariaDB-only dead code; see git history if needed. */

/* (deleted) comment + #if 0: discover_table_existence -- MariaDB-only dead code; see git history if needed. */

/*
  Scan the schema CF for all unique database names and create any missing
  database directories under mysql_real_data_home. In object-store mode,
  replicas may receive tables whose database directory was never created
  locally; this ensures the directory exists.
  (Note: the MariaDB discover_table_names hook that would consume these
  directories is #if 0'd for MySQL. MySQL's Data Dictionary handles
  discovery centrally, so this is currently dormant infrastructure kept
  for the object-store replica path.)
*/
static void schema_cf_ensure_databases()
{
    if (!g_engine_ctx.schema_cf) return;

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_get_engine(), &txn) != TDB_SUCCESS) return;

    tidesdb_iter_t *iter = NULL;
    if (tidesdb_iter_new(txn, g_engine_ctx.schema_cf, &iter) != TDB_SUCCESS || !iter)
    {
        tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        return;
    }

    std::unordered_set<std::string> seen_dbs;

    tidesdb_iter_seek_to_first(iter);
    while (tidesdb_iter_valid(iter))
    {
        uint8_t *kp = NULL;
        size_t klen = 0;
        if (tidesdb_iter_key(iter, &kp, &klen) != TDB_SUCCESS || !kp) break;

        /* Key format-- "db_name<SCHEMA_CF_KEY_SEP>table_name" --
           we find the separator */
        const char *kstr = (const char *)kp;
        size_t sep = 0;
        for (; sep < klen; sep++)
        {
            if (kstr[sep] == SCHEMA_CF_KEY_SEP) break;
        }
        if (sep > 0 && sep < klen)
        {
            std::string dbname(kstr, sep);
            if (seen_dbs.insert(dbname).second)
            {
                /* We build database directory path */
                char db_dir[FN_REFLEN];
                size_t dh_len = strlen(mysql_real_data_home);
                snprintf(db_dir, sizeof(db_dir), "%s%s%s", mysql_real_data_home,
                         (dh_len > 0 && mysql_real_data_home[dh_len - 1] != '/') ? "/" : "",
                         dbname.c_str());

                MY_STAT st;
                if (!my_stat(db_dir, &st, MYF(0)))
                {
                    if (my_mkdir(db_dir, TIDESDB_DB_DIR_MODE, MYF(0)) == 0)
                        sql_print_information(
                            "[TIDESDB] Created database directory '%s' for schema discovery",
                            dbname.c_str());
                }
            }
        }

        tidesdb_iter_next(iter);
    }

    tidesdb_iter_free(iter);
    tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
}

/* ******************** Plugin init / deinit ******************** */

static int tidesdb_hton_drop_table(handlerton *, const char *path);
static void tidesdb_hton_drop_database(handlerton *, char *path);
static bool tidesdb_hton_flush_logs(handlerton *);
static int tidesdb_hton_panic(handlerton *, enum ha_panic_function flag);
static void tidesdb_hton_pre_shutdown(void);
static void tidesdb_hton_kill_query(handlerton *, THD *thd, enum thd_kill_levels level);

static int tidesdb_init_func(void *p)
{
    DBUG_ENTER("tidesdb_init_func");

    /* Bootstrap the master key for at-rest encryption if the DBA pointed
       us at a key file. Failures are non-fatal at init -- they just mean
       any CREATE TABLE asking for encryption will return a clear error.
       Successfully loaded keys are kept in process memory until shutdown. */
    if (srv_master_key_file && srv_master_key_file[0])
    {
        if (tidesdb_master_key_load_from_file(srv_master_key_file))
            sql_print_warning("[TIDESDB] master key file '%s' could not be loaded; "
                              "encrypted tables will be unavailable",
                              srv_master_key_file);
    }

    tidesdb_hton = (handlerton *)p;
    tidesdb_hton->create = tidesdb_create_handler;
    tidesdb_hton->flags = HTON_SUPPORTS_ENGINE_ATTRIBUTE;
    tidesdb_hton->savepoint_offset = sizeof(tidesdb_savepoint_t);
    tidesdb_hton->file_extensions = ha_tidesdb_exts;  /* MariaDB: tablefile_extensions */
    /* MariaDB-only handlerton members (table_options/field_options/index_options)
     * have no MySQL equivalents -- per-table options reach the CF config
     * through the ENGINE_ATTRIBUTE JSON path; see tidesdb_opts_for_table. */
    /* tidesdb_hton->drop_table = tidesdb_hton_drop_table;  -- MariaDB-only handlerton hook;
     * MySQL drops tables through ha_tidesdb::delete_table() at the handler level. */
    tidesdb_hton->drop_database = tidesdb_hton_drop_database;

    /* Handlerton transaction callbacks -- one TidesDB txn per BEGIN..COMMIT */
    tidesdb_hton->prepare = tidesdb_prepare;
    tidesdb_hton->commit = tidesdb_commit;
    tidesdb_hton->rollback = tidesdb_rollback;
    tidesdb_hton->close_connection = tidesdb_close_connection;

    tidesdb_hton->savepoint_set = tidesdb_savepoint_set;
    tidesdb_hton->savepoint_rollback = tidesdb_savepoint_rollback;
    tidesdb_hton->savepoint_rollback_can_release_mdl = tidesdb_savepoint_rollback_can_release_mdl;
    tidesdb_hton->savepoint_release = tidesdb_savepoint_release;
    tidesdb_hton->start_consistent_snapshot = tidesdb_start_consistent_snapshot;
    tidesdb_hton->show_status = tidesdb_show_status;

    /* Durability / lifecycle / cancellation hooks. */
    tidesdb_hton->flush_logs = tidesdb_hton_flush_logs;
    tidesdb_hton->panic = tidesdb_hton_panic;
    /* tidesdb_hton->pre_shutdown = tidesdb_hton_pre_shutdown;  -- MariaDB-only.
     * MySQL has its own shutdown coordination; the handlerton's deinit() runs
     * during plugin unload which serves the same purpose. */
    /* tidesdb_hton->kill_query = tidesdb_hton_kill_query;  -- MariaDB-only.
     * MySQL signals query abort via THD::killed which the engine should poll. */

    /* Initialize the engine context (last_conflict_mutex). schema_cf,
       engine, path are populated later by the open / config code. */
    g_engine_ctx.init();

    tdb_row_lock_init();

    /* Initialize trx-lifecycle rwlock (see g_trx_lifecycle_lock comment
       for the UAF it protects against). */
    mysql_rwlock_init(g_trx_lifecycle_lock_key, &g_trx_lifecycle_lock);

    /* HF-2: FTS-meta serialization mutex is now per-share, initialized
       in TidesDB_share's constructor. The former process-global init
       has been removed; nothing to do here. */

    /* Initialize FTS subsystem (stop-word + blend-char rwlocks,
       default stop-word load, blend-char fast-lookup rebuild).
       Moved to tidesdb_fts.cc during the A-2 extraction. */
    tdb_fts_init();

    /* We use tidesdb_data_home_dir if set, otherwise compute
       a sibling directory of the MariaDB data directory. */
    if (srv_data_home_dir && srv_data_home_dir[0])
    {
        g_engine_ctx.path = srv_data_home_dir;
        while (!g_engine_ctx.path.empty() && g_engine_ctx.path.back() == '/') g_engine_ctx.path.pop_back();
    }
    else
    {
        std::string data_home(mysql_real_data_home);
        while (!data_home.empty() && data_home.back() == '/') data_home.pop_back();
        size_t slash_pos = data_home.rfind('/');
        if (slash_pos != std::string::npos)
            g_engine_ctx.path = data_home.substr(0, slash_pos + 1) + "tidesdb_data";
        else
            g_engine_ctx.path = "tidesdb_data";
    }

    /* We map log level enum index to TidesDB constants */
    static const int log_level_map[] = {TDB_LOG_DEBUG, TDB_LOG_INFO,  TDB_LOG_WARN,
                                        TDB_LOG_ERROR, TDB_LOG_FATAL, TDB_LOG_NONE};

    tidesdb_config_t cfg = tidesdb_default_config();
    cfg.db_path = const_cast<char *>(g_engine_ctx.path.c_str());
    cfg.num_flush_threads = (int)srv_flush_threads;
    cfg.num_compaction_threads = (int)srv_compaction_threads;
    cfg.log_level = (tidesdb_log_level_t)log_level_map[srv_log_level];
    cfg.block_cache_size = (size_t)srv_block_cache_size;
    cfg.max_open_sstables = (int)srv_max_open_sstables;
    cfg.log_to_file = srv_log_to_file ? 1 : 0;
    cfg.log_truncation_at = (size_t)srv_log_truncation_at;
    cfg.max_memory_usage = (size_t)srv_max_memory_usage;
    cfg.unified_memtable = srv_unified_memtable ? 1 : 0;
    cfg.unified_memtable_write_buffer_size = (size_t)srv_unified_memtable_write_buffer_size;
    cfg.unified_memtable_sync_mode = tdb_sync_mode_map[srv_unified_memtable_sync_mode];
    cfg.unified_memtable_sync_interval_us = (uint64_t)srv_unified_memtable_sync_interval;
    cfg.unified_memtable_skip_list_max_level = 0;      /* 0 = library default */
    cfg.unified_memtable_skip_list_probability = 0.0f; /* 0 = library default */

    /* Object store connector setup */
    tidesdb_objstore_t *objstore_connector = NULL;
    static tidesdb_objstore_config_t objstore_cfg;

    if (srv_object_store_backend == OBJSTORE_BACKEND_S3)
    {
#ifdef TIDESDB_WITH_S3
        if (!srv_s3_endpoint || !srv_s3_bucket || !srv_s3_access_key || !srv_s3_secret_key)
        {
            sql_print_error(
                "[TIDESDB] S3 backend requires s3_endpoint, s3_bucket, "
                "s3_access_key, and s3_secret_key");
            DBUG_RETURN(1);
        }

        /* L-4 + MF-2: redact endpoint/bucket in BOTH success and
           failure logs. The original L-4 fix only covered the success
           log; the follow-up review flagged that the failure path at
           the error site below still leaked the raw values. Hoist the
           redactor here so both sites use it. */
        auto redact = [](const char *s) -> const char * {
            if (!s || !s[0]) return "(unset)";
            return "***";
        };

        objstore_connector = tidesdb_objstore_s3_create(
            srv_s3_endpoint, srv_s3_bucket, srv_s3_prefix, srv_s3_access_key, srv_s3_secret_key,
            srv_s3_region, srv_s3_use_ssl ? 1 : 0, srv_s3_path_style ? 1 : 0);

        if (!objstore_connector)
        {
            sql_print_error("[TIDESDB] Failed to create S3 connector for %s/%s",
                            redact(srv_s3_endpoint), redact(srv_s3_bucket));
            DBUG_RETURN(1);
        }

        sql_print_information("[TIDESDB] S3 connector created (endpoint=%s, bucket=%s, ssl=%s)",
                              redact(srv_s3_endpoint), redact(srv_s3_bucket),
                              srv_s3_use_ssl ? "yes" : "no");
#else
        sql_print_error(
            "[TIDESDB] S3 backend requested but TidesDB was not built with "
            "-DTIDESDB_WITH_S3=ON");
        DBUG_RETURN(1);
#endif
    }

    if (objstore_connector)
    {
        objstore_cfg = tidesdb_objstore_default_config();
        objstore_cfg.local_cache_max_bytes = (size_t)srv_objstore_local_cache_max;
        objstore_cfg.wal_sync_threshold_bytes = (size_t)srv_objstore_wal_sync_threshold;
        objstore_cfg.wal_sync_on_commit = srv_objstore_wal_sync_on_commit ? 1 : 0;
        objstore_cfg.replicate_wal = 1; /* upload WAL segments for replica recovery */
        objstore_cfg.replica_mode = srv_replica_mode ? 1 : 0;
        objstore_cfg.replica_sync_interval_us = (uint64_t)srv_replica_sync_interval;
        objstore_cfg.replica_replay_wal = 1;

        cfg.object_store = objstore_connector;
        cfg.object_store_config = &objstore_cfg;
    }

    tidesdb_t *opened = nullptr;
    int rc = tidesdb_open(&cfg, &opened);
    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] Failed to open TidesDB at %s (err=%d)", g_engine_ctx.path.c_str(), rc);
        DBUG_RETURN(1);
    }
    /* Publish atomically -- handler threads must observe a fully-constructed
       engine handle, not a torn write. */
    tdb_set_engine(opened);

    sql_print_information("[TIDESDB] TidesDB opened at %s", g_engine_ctx.path.c_str());

    /* Schema discovery CF -- created when object store is active so that
       replicas can discover table definitions from the shared storage. */
    if (objstore_connector)
    {
        tidesdb_column_family_config_t schema_cfg = tidesdb_default_column_family_config();
        if (!tidesdb_get_column_family(tdb_get_engine(), SCHEMA_CF_NAME))
            tidesdb_create_column_family(tdb_get_engine(), SCHEMA_CF_NAME, &schema_cfg);

        g_engine_ctx.schema_cf = tidesdb_get_column_family(tdb_get_engine(), SCHEMA_CF_NAME);

        if (g_engine_ctx.schema_cf)
        {
            /* MariaDB-only discover_* hooks for engine-driven table discovery
             * (used with object-store mode). MySQL's Data Dictionary handles
             * table discovery centrally — no engine hook needed. TODO: add
             * object-store-driven discovery via MySQL SDI when adding replica
             * support. The underlying discover_* functions are #if 0'd. */

            /* Ensure database directories exist for all tables in the schema
               CF so the server can open them on replicas. */
            schema_cf_ensure_databases();

            sql_print_information("[TIDESDB] Schema discovery enabled (object store mode)");
        }
    }

    DBUG_RETURN(0);
}

/*
  Handlerton-level FLUSH LOGS callback.  Called on FLUSH LOGS and by
  backup tools (XtraBackup, MySQL Enterprise Backup) before copying files
  so the on-disk WAL is a consistent snapshot.  With unified-memtable mode
  one sync covers all CFs.  In per-CF mode we sync the schema CF (always
  present in object-store mode; otherwise we try the first registered CF).
  Returns false on success (handlerton convention).
*/
static bool tidesdb_hton_flush_logs(handlerton *)
{
    if (!tdb_get_engine()) return false;

    tidesdb_column_family_t *target = g_engine_ctx.schema_cf;
    if (!target)
    {
        char **names = NULL;
        int count = 0;
        if (tidesdb_list_column_families(tdb_get_engine(), &names, &count) == TDB_SUCCESS && names)
        {
            if (count > 0 && names[0]) target = tidesdb_get_column_family(tdb_get_engine(), names[0]);
            for (int i = 0; i < count; i++)
                if (names[i]) free(names[i]);
            free(names);
        }
    }
    if (!target) return false; /* empty database -- nothing to sync */

    int rc = tidesdb_sync_wal(target);
    if (rc != TDB_SUCCESS)
    {
        sql_print_warning("[TIDESDB] flush_logs: tidesdb_sync_wal failed (rc=%d)", rc);
        return true; /* error */
    }
    return false;
}

/*
  Handlerton-level panic callback. Invoked on signal-driven or abnormal
  shutdown paths where tidesdb_deinit_func may not run. We only react to
  HA_PANIC_CLOSE -- the other flags are legacy ISAM-era.
*/
static int tidesdb_hton_panic(handlerton *, enum ha_panic_function flag)
{
    if (flag != HA_PANIC_CLOSE) return 0;
    /* Take ownership atomically: any handler thread that loads after this
       point sees nullptr and short-circuits cleanly. We swap rather than
       load+store so no second caller can race us into a double-close. */
    tidesdb_t *engine = g_engine_ctx.engine.exchange(nullptr, std::memory_order_acq_rel);
    if (engine)
    {
        tidesdb_close(engine);
        g_engine_ctx.schema_cf = NULL;
    }
    return 0;
}

/*
  Handlerton-level pre_shutdown callback.  Runs before the deinit path so
  background threads that still need a fully-functional server (compaction,
  flush) get a clean signal to drain.  We flush the unified WAL synchronously
  and let tidesdb_close() in deinit finish the teardown.
*/
static void tidesdb_hton_pre_shutdown(void)
{
    if (!tdb_get_engine()) return;

    /* Sync the unified WAL so durability is preserved if deinit is racing
       a forced exit.  The call is cheap when there's nothing to sync. */
    (void)tidesdb_hton_flush_logs(tidesdb_hton);
}

/*
  Handlerton-level kill_query callback. Invoked when the user runs
  KILL QUERY <id> or when the connection is killed. Our job is to wake
  the victim if it is currently blocked in row_lock_acquire so that the
  wait loop observes thd_killed() and bails out promptly.

  The trx's waiting_on pointer is the lock entry being waited on; a
  broadcast on that entry's cond var wakes the waiter.  False wake-ups
  are harmless -- the wait loop re-checks ownership and goes back to
  sleep if the lock is still contended.
*/
static void tidesdb_hton_kill_query(handlerton *, THD *thd, enum thd_kill_levels)
{
    if (!thd) return;
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx) return;

    /* CF-1 fix: this callback runs on the thread executing KILL QUERY,
       not the victim's thread. The victim's tidesdb_close_connection
       may be freeing `trx` concurrently. We MUST hold the
       g_trx_lifecycle_lock read-lock for any dereference of `trx`,
       matching the discipline applied to tdb_lock_would_deadlock. The
       original H-3 rollout missed this site -- it's the same UAF class
       the rwlock was introduced to close. */
    mysql_rwlock_rdlock(&g_trx_lifecycle_lock);

    tdb_row_lock_t *wait = trx->waiting_on.load(std::memory_order_acquire);
    if (!wait)
    {
        mysql_rwlock_unlock(&g_trx_lifecycle_lock);
        return;
    }

    /* Lock entries themselves are never freed (H-3 invariant), so
       `wait` is safe to deref without the rwlock -- but `trx` IS, so
       we hold the rwlock until we're done with `trx`. We broadcast
       under the owning partition's mutex so the wake-up is serialized
       against the holder's release path. */
    if (lock_partitions && wait->partition < ROW_LOCK_PARTITIONS)
    {
        tdb_lock_partition_t *part = &lock_partitions[wait->partition];
        mysql_mutex_lock(&part->mutex);
        mysql_cond_broadcast(&wait->cond);
        mysql_mutex_unlock(&part->mutex);
    }

    mysql_rwlock_unlock(&g_trx_lifecycle_lock);
}

static int tidesdb_deinit_func(void *p)
{
    DBUG_ENTER("tidesdb_deinit_func");

    g_engine_ctx.schema_cf = NULL;

    /* Atomic exchange: takes ownership of the engine handle and races
       cleanly with tidesdb_hton_panic (which uses the same pattern). */
    tidesdb_t *engine = g_engine_ctx.engine.exchange(nullptr, std::memory_order_acq_rel);
    if (engine)
    {
        tidesdb_close(engine);
    }

    g_engine_ctx.destroy();
    /* HF-2: per-share FTS-meta mutexes destroyed in TidesDB_share dtor. */
    mysql_rwlock_destroy(&g_trx_lifecycle_lock);

    /* FTS rwlock teardown and stop-word set drop moved to tidesdb_fts.cc
       (A-2 extraction). */
    tdb_fts_destroy();

    tdb_row_lock_destroy();

    sql_print_information("[TIDESDB] TidesDB closed");
    DBUG_RETURN(0);
}

/* ******************** path_to_cf_name ******************** */

std::string ha_tidesdb::path_to_cf_name(const char *path)
{
    std::string p(path);

    if (p.size() >= MARIADB_REL_PATH_PREFIX_LEN &&
        p.compare(0, MARIADB_REL_PATH_PREFIX_LEN, MARIADB_REL_PATH_PREFIX) == 0)
        p = p.substr(MARIADB_REL_PATH_PREFIX_LEN);

    size_t last_slash = p.rfind('/');
    if (last_slash == std::string::npos) return p;

    std::string tblname = p.substr(last_slash + 1);

    size_t prev_slash = (last_slash > 0) ? p.rfind('/', last_slash - 1) : std::string::npos;
    std::string dbname;
    if (prev_slash == std::string::npos)
        dbname = p.substr(0, last_slash);
    else
        dbname = p.substr(prev_slash + 1, last_slash - prev_slash - 1);

    std::string result = dbname + CF_DB_TABLE_SEP + tblname;

    /* MariaDB temp table names embed '#'; substitute so the CF name
       remains a valid identifier in the underlying TidesDB layer. */
    for (size_t i = 0; i < result.size(); i++)
        if (result[i] == MARIADB_TEMP_NAME_MARKER) result[i] = MARIADB_TEMP_NAME_REPLACEMENT;

    return result;
}

/* ******************** Factory / Constructor ******************** */

static handler *tidesdb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       bool /*partitioned*/, MEM_ROOT *mem_root)
{
    return new (mem_root) ha_tidesdb(hton, table);
}

ha_tidesdb::ha_tidesdb(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg),
      share(NULL),
      stmt_txn(NULL),
      stmt_txn_dirty(false),
      scan_txn(NULL),
      scan_iter(NULL),
      scan_cf_(NULL),
      scan_iter_cf_(NULL),
      scan_iter_txn_(NULL),
      idx_pk_exact_done_(false),
      scan_dir_(DIR_NONE),
      current_pk_len_(0),
      idx_search_comp_len_(0),
      dup_iter_count_(0),
      cached_enc_key_ver_(0),
      enc_key_ver_valid_(false),
      cached_time_(0),
      cached_time_valid_(false),
      cached_sess_ttl_(0),
      cached_skip_unique_(false),
      cached_single_delete_primary_(false),
      cached_thdvars_valid_(false),
      last_dup_key_no_(MAX_KEY),
      stmt_has_write_lock_(false),
      is_pk_(false),
      scan_iter_last_err_(0),
      scan_iter_last_err_cf_(NULL),
      scan_iter_last_err_txn_(NULL),
      has_blobs_(false),
      encrypted_(false),
      record1_lo_(NULL),
      record1_hi_(NULL),
      cached_sql_cmd_(0),
      cached_is_autocommit_(false),
      cached_stmt_shape_valid_(false),
      cached_thd_(NULL),
      cached_trx_(NULL),
      trx_registered_(false),
      in_bulk_insert_(false),
      in_bulk_update_(false),
      in_bulk_delete_(false),
      bulk_insert_ops_(0),
      cached_compact_after_range_delete_min_rows_(0),
      bulk_delete_rows_(0),
      mrr_custom_active_(false),
      mrr_no_assoc_(false),
      mrr_keyno_(MAX_KEY),
      mrr_next_idx_(0),
      keyread_only_(false),
      write_can_replace_(false)
{
    memset(dup_iter_cache_, 0, sizeof(dup_iter_cache_));
    memset(dup_iter_txn_, 0, sizeof(dup_iter_txn_));
    memset(dup_iter_txn_gen_, 0, sizeof(dup_iter_txn_gen_));
}

/* ******************** free_dup_iter_cache ******************** */

void ha_tidesdb::free_dup_iter_cache()
{
    for (uint i = 0; i < MAX_KEY; i++)
    {
        if (dup_iter_cache_[i])
        {
            tidesdb_iter_free(dup_iter_cache_[i]);
            dup_iter_cache_[i] = NULL;
            dup_iter_txn_[i] = NULL;
            dup_iter_txn_gen_[i] = 0;
        }
    }
    dup_iter_count_ = 0;
}

/* ******************** get_share ******************** */

TidesDB_share *ha_tidesdb::get_share()
{
    TidesDB_share *tmp_share;
    DBUG_ENTER("ha_tidesdb::get_share");

    lock_shared_ha_data();
    if (!(tmp_share = static_cast<TidesDB_share *>(get_ha_share_ptr())))
    {
        tmp_share = new TidesDB_share;
        if (!tmp_share) goto err;
        set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
    }
err:
    unlock_shared_ha_data();
    DBUG_RETURN(tmp_share);
}

/* ******************** PK / Index key helpers ******************** */

/*
  We build memcmp-comparable key bytes from record fields for a given KEY.
  Uses Field::make_sort_key_part() so that big-endian, sign-bit-flipped encoding
  is produced for numeric types -- which sorts correctly under memcmp.

  The record may point to record[0] or record[1]; we adjust field pointers
  via move_field_offset to read from the correct buffer.
*/
uint ha_tidesdb::make_comparable_key(KEY *key_info, const uchar *record, uint num_parts, uchar *out)
{
    uint pos = 0;
    my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(record - table->record[0]);

    for (uint p = 0; p < num_parts && p < key_info->user_defined_key_parts; p++)
    {
        KEY_PART_INFO *kp = &key_info->key_part[p];
        Field *field = kp->field;

        /* We handle the null indicator ourselves using real_maybe_null()
           (which checks field-level nullability only) instead of relying on
           make_sort_key_part() which uses maybe_null() (includes
           table->maybe_null).  For inner tables of outer joins,
           table->maybe_null is true, causing make_sort_key_part to write
           a spurious null indicator byte even for NOT NULL PK fields.
           Using make_sort_key() directly avoids this mismatch. */
        field->move_field_offset(ptrdiff);
        if (field->is_nullable())
        {
            if (field->is_null())
            {
                out[pos++] = SORT_KEY_NULL;
                bzero(out + pos, kp->length);
                pos += kp->length;
                field->move_field_offset(-ptrdiff);
                continue;
            }
            out[pos++] = SORT_KEY_NOT_NULL;
        }
        /* For VARBINARY (binary charset variable-length fields), sort_string()
           stores the value length in the last length_bytes of the output,
           truncating trailing data bytes when the value fills the field.
           This causes false duplicate detection on UNIQUE indexes because
           different values produce identical sort keys.
           Thus for binary charset varstrings, write all data bytes zero-padded
           followed by the length, so the full value is preserved. */
        if (field->type() == MYSQL_TYPE_VARCHAR && field->charset() == &my_charset_bin)
        {
            Field_varstring *fvs = static_cast<Field_varstring *>(field);
            String buf;
            fvs->val_str(&buf, &buf);
            uint data_len = (uint)buf.length();
            uint len_bytes = ((fvs->field_length < 256) ? 1 : 2);
            uint data_space = kp->length - len_bytes;

            /* We write all data bytes, zero-padded to fill the data area */
            uint copy_len = MY_MIN(data_len, data_space);
            memcpy(out + pos, buf.ptr(), copy_len);
            if (copy_len < data_space) bzero(out + pos + copy_len, data_space - copy_len);
            pos += data_space;

            /* For values that overflow data_space (value is exactly field_length
               bytes), write the overflow bytes into the length area first */
            if (data_len > data_space)
            {
                uint overflow = MY_MIN(data_len - data_space, len_bytes);
                memcpy(out + pos, buf.ptr() + data_space, overflow);
                pos += len_bytes;
            }
            else
            {
                /* Length suffix in high-byte order (preserves sort order) */
                if (len_bytes == 1)
                    out[pos] = (uchar)data_len;
                else
                    mi_int2store(out + pos, data_len);
                pos += len_bytes;
            }

            field->move_field_offset(-ptrdiff);
            continue;
        }

        memset(out + pos, 0, kp->length); field->make_sort_key(out + pos, kp->length);
        field->move_field_offset(-ptrdiff);
        pos += kp->length;
    }

    return pos;
}

/*
  Convert a key_copy-format search key (as passed to index_read_map)
  into the comparable format that we store in TidesDB.
  Uses key_restore to unpack into record[1], then make_comparable_key.
*/
uint ha_tidesdb::key_copy_to_comparable(KEY *key_info, const uchar *key_buf, uint key_len,
                                        uchar *out)
{
    /* Use per-handler scratch buffer rather than table->record[1] so we
       don't pollute the row buffer the SQL layer is sharing. Lazy-size
       on first use (open() can't always size it -- altered tables may
       have a different reclength). */
    /* LF-1: scratch is sized once in open() and grown defensively here
       only if the table_share's reclength has somehow expanded since
       open (e.g. mid-statement ALTER -- shouldn't happen but cheap to
       check). Steady-state hot path takes the resize branch never. */
    if (unlikely(key_unpack_scratch_.size() < table->s->reclength))
        key_unpack_scratch_.resize(table->s->reclength);
    uchar *scratch = key_unpack_scratch_.data();

    key_restore(scratch, key_buf, key_info, key_len);

    /* We count how many key parts are covered by key_len */
    uint parts = 0;
    uint len = 0;
    for (parts = 0; parts < key_info->user_defined_key_parts; parts++)
    {
        uint part_len = key_info->key_part[parts].store_length;
        if (len + part_len > key_len) break;
        len += part_len;
    }
    if (parts == 0) parts = 1;

    return make_comparable_key(key_info, scratch, parts, out);
}

/*
  Build PK bytes from a record.
  -- With user PK  -- use make_comparable_key for memcmp-correct ordering.
  -- Without PK    -- not applicable for NEW rows (caller generates hidden id);
                      for EXISTING rows current_pk already holds the key.
*/
uint ha_tidesdb::pk_from_record(const uchar *record, uchar *out)
{
    if (share->has_user_pk)
    {
        return make_comparable_key(&table->key_info[share->pk_index], record,
                                   table->key_info[share->pk_index].user_defined_key_parts, out);
    }
    else
    {
        /* Hidden PK -- we copy current_pk (must have been set by a prior read) */
        memcpy(out, current_pk_buf_, current_pk_len_);
        return current_pk_len_;
    }
}

/*
  Compute the comparable key byte length for a KEY.
  Matches what make_comparable_key() actually produces:
    sum of (nullable ? 1 : 0) + kp->length for each key part.

  NOTE -- ki->key_length includes store_length overhead (e.g. 2 bytes
  per VARCHAR part for length prefix in key_copy format) which is
  not present in the comparable key output.
*/
static uint comparable_key_length(const KEY *ki)
{
    /* Spatial indexes use a fixed 8-byte Hilbert value as the comparable key.. */
    if (ki->algorithm == HA_KEY_ALG_RTREE) return SPATIAL_HILBERT_KEY_LEN;

    uint len = 0;
    for (uint p = 0; p < ki->user_defined_key_parts; p++)
    {
        if (ki->key_part[p].field->is_nullable()) len++;
        len += ki->key_part[p].length;
    }
    return len;
}

/*
  Build a secondary index CF entry key:
    [comparable index-column bytes] + [comparable PK bytes]
*/
uint ha_tidesdb::sec_idx_key(uint idx, const uchar *record, uchar *out)
{
    KEY *key_info = &table->key_info[idx];
    uint pos = make_comparable_key(key_info, record, key_info->user_defined_key_parts, out);
    /* We append PK for uniqueness */
    pos += pk_from_record(record, out + pos);
    return pos;
}

/*
  Try to fill record buf with column values decoded from the secondary
  index key, avoiding the expensive PK point-lookup.  Used when
  keyread_only_ is true (covering index scan).

  The secondary index key layout is:
    [comparable_idx_cols | comparable_pk]

  Uses decode_sort_key_part() which supports integers, DATE, DATETIME,
  TIMESTAMP, YEAR, and fixed-length CHAR/BINARY (binary/latin1).
  Returns true on success.
*/
bool ha_tidesdb::try_keyread_from_index(const uint8_t *ik, size_t iks, uint idx, uchar *buf)
{
    if (!share->has_user_pk) return false;

    KEY *pk_key = &table->key_info[share->pk_index];
    KEY *idx_key = &table->key_info[idx];
    uint idx_col_len = share->idx_comp_key_len[idx];

    /* We check every column in read_set against the precomputed coverage
       bitmap for this index.  O(read_set set-bits) instead of the prior
       O(set-bits * (pk_parts + idx_parts)) nested scan. */
    if (idx < share->idx_cover.size())
    {
        const std::vector<bool> &cover = share->idx_cover[idx];
        for (uint c = bitmap_get_first_set(table->read_set); c != MY_BIT_NONE;
             c = bitmap_get_next_set(table->read_set, c))
        {
            if (c >= cover.size() || !cover[c]) return false;
        }
    }
    else
    {
        /* Share not populated for this index (shouldn't happen). */
        return false;
    }

    /* We decode index column parts from the head of the key */
    const uint8_t *pos = ik;
    for (uint p = 0; p < idx_key->user_defined_key_parts; p++)
    {
        KEY_PART_INFO *kp = &idx_key->key_part[p];
        Field *f = kp->field;
        if (f->is_nullable())
        {
            if (pos >= ik + iks) return false;
            if (*pos == 0)
            {
                f->set_null();
                pos++;
                continue;
            }
            f->set_notnull();
            pos++;
        }
        if (pos + kp->length > ik + iks) return false;
        if (bitmap_is_set(table->read_set, kp->fieldnr - 1))
        {
            if (!decode_sort_key_part(pos, kp->length, f, buf)) return false;
        }
        pos += kp->length;
    }

    /* We decode PK parts from the tail of the key */
    const uint8_t *pk_start = ik + idx_col_len;
    pos = pk_start;
    for (uint p = 0; p < pk_key->user_defined_key_parts; p++)
    {
        KEY_PART_INFO *kp = &pk_key->key_part[p];
        Field *f = kp->field;
        if (f->is_nullable())
        {
            if (pos >= ik + iks) return false;
            if (*pos == 0)
            {
                f->set_null();
                pos++;
                continue;
            }
            f->set_notnull();
            pos++;
        }
        if (pos + kp->length > ik + iks) return false;
        if (bitmap_is_set(table->read_set, kp->fieldnr - 1))
        {
            if (!decode_sort_key_part(pos, kp->length, f, buf)) return false;
        }
        pos += kp->length;
    }

    /* We set current_pk for position() */
    uint pk_bytes = (uint)(iks - idx_col_len);
    memcpy(current_pk_buf_, pk_start, pk_bytes);
    current_pk_len_ = pk_bytes;

    return true;
}

/* ******************** ICP (Index Condition Pushdown) helpers ******************** */

/*
  Reverse a single integer sort-key part (big-endian, sign-bit-flipped)
  back to native little-endian at `to`.  Caller precomputes `to` so we
  don't re-walk f->field_ptr()/f->table->record[0] on every decode.

  MariaDB integer pack widths are TINY=1, SHORT=2, INT24=3, LONG=4,
  LONGLONG=8 -- any other width is rejected.  The decode is a plain
  byte-reverse, with the most-significant byte XORed with the sign
  flip mask for signed types so the original native value is recovered.
*/
bool ha_tidesdb::decode_int_sort_key(const uint8_t *src, uint sort_len, bool is_signed, uchar *to)
{
    if (sort_len == 0 || (sort_len > 4 && sort_len != 8)) return false;

    for (uint i = 0; i < sort_len; i++) to[i] = src[sort_len - 1 - i];
    if (is_signed) to[sort_len - 1] ^= INT_SORT_SIGN_FLIP_MASK;
    return true;
}

/*
  Extended sort-key decoder -- handles integers (via decode_int_sort_key),
  DATE (3 bytes big-endian), DATETIME/TIMESTAMP (4-8 bytes big-endian),
  YEAR (1 byte), and fixed-length CHAR/BINARY (direct memcpy of sort key).

  For integer types, delegates to decode_int_sort_key which handles the
  sign-bit-flip + endian reversal.

  For DATE/DATETIME/TIMESTAMP/YEAR, the sort key is big-endian unsigned;
  we reverse the byte order to native little-endian without sign-flip
  (these types are always unsigned internally).

  For CHAR/BINARY (MYSQL_TYPE_STRING), the sort key produced by
  Field_string::sort_string is the charset's sort weight sequence.
  For binary/latin1 charsets this is identical to the field content
  (padded with spaces to kp->length).  We copy it directly.
  For multi-byte charsets (utf8) the sort weights differ from the
  stored bytes, so we cannot reverse -- return false.

  Returns true on success, false for unsupported types.
*/
bool ha_tidesdb::decode_sort_key_part(const uint8_t *src, uint sort_len, Field *f, uchar *buf)
{
    /* Compute the destination pointer exactly once per call.  Every branch
       below wrote `buf + (f->field_ptr() - f->table->record[0])` independently. */
    uchar *to = buf + (uintptr_t)(f->field_ptr() - f->table->record[0]);

    switch (f->real_type())
    {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
            return decode_int_sort_key(src, sort_len, !f->is_unsigned(), to);

        case MYSQL_TYPE_YEAR:
            /* YEAR is 1 byte unsigned, sort key is identity */
            to[0] = src[0];
            return true;

        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_NEWDATE:
            /* DATE is DATE_PACK_LEN bytes, sort key is big-endian unsigned.
               Reverse to native little-endian. */
            if (sort_len == DATE_PACK_LEN)
            {
                for (uint b = 0; b < sort_len; b++) to[b] = src[sort_len - 1 - b];
                return true;
            }
            return false;

        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_DATETIME2:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2:
            /* DATETIME/TIMESTAMP sort keys are big-endian unsigned, at most
               DATETIME_MAX_PACK_LEN bytes.  Reverse to native little-endian. */
            if (sort_len <= DATETIME_MAX_PACK_LEN)
            {
                for (uint b = 0; b < sort_len; b++) to[b] = src[sort_len - 1 - b];
                return true;
            }
            return false;

        case MYSQL_TYPE_STRING:
            /* Fixed-length CHAR/BINARY.  For binary/latin1 charsets the
               sort key is identical to the stored content (space-padded).
               For multi-byte charsets we cannot reverse. */
            if (f->charset() == &my_charset_bin || f->charset() == &my_charset_latin1)
            {
                uint flen = f->pack_length();
                uint copy_len = (sort_len < flen) ? sort_len : flen;
                memcpy(to, src, copy_len);
                if (copy_len < flen) memset(to + copy_len, ' ', flen - copy_len);
                return true;
            }
            return false;

        default:
            return false;
    }
}

/*
  Evaluate pushed index condition on a secondary-index entry before
  the expensive PK point-lookup (InnoDB pattern).

  Decodes the index key column values and PK column values from the
  comparable-format index key into the record buffer, then evaluates
  the pushed condition (pushed_idx_cond->val_bool), end_range, and
  THD kill state inline -- replacing an old MariaDB-port stub that
  did none of these checks.

  Supports integer types, DATE, DATETIME, TIMESTAMP, YEAR, and
  fixed-length CHAR/BINARY (binary/latin1 charset) via
  decode_sort_key_part().  For unsupported types, ICP is skipped and
  CHECK_POS is returned so the caller falls through to the PK lookup.
*/
check_result_t ha_tidesdb::icp_check_secondary(const uint8_t *ik, size_t iks, uint idx, uchar *buf)
{
    if (!pushed_idx_cond || pushed_idx_cond_keyno != idx) return CHECK_POS;

    KEY *idx_key = &table->key_info[idx];
    uint idx_col_len = share->idx_comp_key_len[idx];
    bool decode_ok = true;

    /* Decode index column parts from the comparable-format key.
       If any part can't be decoded (DECIMAL, VARCHAR, etc.), we fall
       back to a full PK row fetch so the condition evaluates correctly. */
    const uint8_t *pos = ik;
    for (uint p = 0; p < idx_key->user_defined_key_parts && decode_ok; p++)
    {
        KEY_PART_INFO *kp = &idx_key->key_part[p];
        Field *f = kp->field;

        if (f->is_nullable())
        {
            if (pos >= ik + iks)
            {
                decode_ok = false;
                break;
            }
            if (*pos == 0)
            {
                f->set_null();
                pos++;
                continue;
            }
            f->set_notnull();
            pos++;
        }
        if (pos + kp->length > ik + iks)
        {
            decode_ok = false;
            break;
        }
        if (!decode_sort_key_part(pos, kp->length, f, buf)) decode_ok = false;
        pos += kp->length;
    }

    /* Decode PK parts from the tail (pushed condition may reference PK columns). */
    if (decode_ok && share->has_user_pk)
    {
        KEY *pk_key = &table->key_info[share->pk_index];
        pos = ik + idx_col_len;
        for (uint p = 0; p < pk_key->user_defined_key_parts && decode_ok; p++)
        {
            KEY_PART_INFO *kp = &pk_key->key_part[p];
            Field *f = kp->field;

            if (f->is_nullable())
            {
                if (pos >= ik + iks)
                {
                    decode_ok = false;
                    break;
                }
                if (*pos == 0)
                {
                    f->set_null();
                    pos++;
                    continue;
                }
                f->set_notnull();
                pos++;
            }
            if (pos + kp->length > ik + iks)
            {
                decode_ok = false;
                break;
            }
            if (!decode_sort_key_part(pos, kp->length, f, buf)) decode_ok = false;
            pos += kp->length;
        }
    }

    if (!decode_ok)
    {
        /* Could not decode all key parts from the sort key (unsupported type
           like DECIMAL, VARCHAR, multi-byte CHAR).  Fall back to a full PK
           row fetch so ALL columns are available for condition evaluation.
           This is more expensive than pure ICP (still does the PK lookup)
           but is correct, the server won't re-evaluate pushed conditions. */
        if (iks > idx_col_len)
        {
            const uchar *pk = ik + idx_col_len;
            uint pk_len = (uint)(iks - idx_col_len);
            if (fetch_row_by_pk(scan_txn, pk, pk_len, buf) != 0)
                return CHECK_POS; /* PK lookup failed -- we accept row, let caller handle */
        }
        else
        {
            return CHECK_POS; /* malformed key -- we accept */
        }
    }

    /* Evaluate the pushed condition.  Previously this delegated to a
       MariaDB-only stub (handler_index_cond_check in tidesdb_compat.h)
       that always returned 0, which the caller fell through as "accept"
       -- meaning ICP was advertised via HA_DO_INDEX_COND_PUSHDOWN but
       did no filtering and did no end_range checking. With
       in_range_check_pushed_down=true (set in idx_cond_push), MySQL
       trusts the engine to honor both -- so the old behavior could
       return rows past the upper bound on secondary-index range scans.
       Now: real evaluation.

       Order matches MySQL's handler_index_cond_check_icp() in
       sql/handler.cc:7929 (8.0+) -- kill check first, then end_range,
       then the pushed condition. */
    THD *thd_local = ha_thd();
    if (thd_local && thd_killed(thd_local)) return CHECK_NEG;

    if (end_range && compare_key_icp(end_range) > 0) return CHECK_OUT_OF_RANGE;

    if (!pushed_idx_cond->val_bool()) return CHECK_NEG;

    return CHECK_POS;
}

/* ******************** Counter recovery ******************** */

/*
  Recover hidden-PK next_row_id from the last data key.
  Also seed auto_inc_val for tables with AUTO_INCREMENT user-defined PKs
  so that get_auto_increment() can return O(1) instead of doing index_last()
  on every INSERT.
*/
void ha_tidesdb::recover_counters()
{
    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_get_engine(), &txn) != TDB_SUCCESS) return;

    tidesdb_iter_t *iter = NULL;
    if (tidesdb_iter_new(txn, share->cf, &iter) == TDB_SUCCESS)
    {
        tidesdb_iter_seek_to_last(iter);
        if (tidesdb_iter_valid(iter))
        {
            uint8_t *key = NULL;
            size_t key_size = 0;
            if (tidesdb_iter_key(iter, &key, &key_size) == TDB_SUCCESS &&
                is_data_key(key, key_size))
            {
                if (!share->has_user_pk && key_size == KEY_NAMESPACE_LEN + HIDDEN_PK_SIZE)
                {
                    /* Hidden PK -- we decode the big-endian row-id */
                    uint64_t max_id = decode_be64(key + KEY_NAMESPACE_LEN);
                    share->next_row_id.store(max_id + 1, std::memory_order_relaxed);
                }

                if (share->has_user_pk && table->found_next_number_field)
                {
                    /* User PK with AUTO_INCREMENT -- we read the last row to seed
                       the in-memory counter from the max PK value. */
                    uint8_t *val = NULL;
                    size_t val_size = 0;
                    if (tidesdb_iter_value(iter, &val, &val_size) == TDB_SUCCESS)
                    {
                        /* We just unpack the packed row into record[1] using the proper
                           deserialize path so field offsets are correct even when
                           variable-length fields (CHAR/VARCHAR) precede the
                           AUTO_INCREMENT column. */
                        if (share->has_blobs || share->encrypted)
                        {
                            std::string row_data((const char *)val, val_size);
                            deserialize_row(table->record[1], row_data);
                        }
                        else
                        {
                            deserialize_row(table->record[1], (const uchar *)val, val_size);
                        }
                        /* Field::val_int() asserts in Debug builds that the field
                           is in read_set. We're called from open() (and from the
                           re-open MySQL does as part of TRUNCATE), where read_set
                           is not yet set up by the caller. Temporarily mark all
                           columns readable, restore on exit. */
                        my_bitmap_map *old_map =
                            tmp_use_all_columns(table, table->read_set);
                        ulonglong max_val = table->found_next_number_field->val_int_offset(
                            table->s->rec_buff_length);
                        tmp_restore_column_map(table->read_set, old_map);
                        share->auto_inc_val.store(max_val, std::memory_order_relaxed);
                    }
                }
            }
        }
        tidesdb_iter_free(iter);
    }

    if (!share->has_user_pk && share->next_row_id.load(std::memory_order_relaxed) == 0)
        share->next_row_id.store(HIDDEN_PK_FIRST_ROW_ID, std::memory_order_relaxed);

    tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
}

/* ******************** open / close / create ******************** */

int ha_tidesdb::open(const char *name, int mode, uint test_if_locked, const dd::Table *)
{
    DBUG_ENTER("ha_tidesdb::open");

    if (!(share = get_share())) DBUG_RETURN(1);

    /* MF-1 + MF-9 fix: populate the share's ENGINE_ATTRIBUTE option
       cache. The expensive compute (25+ THDVAR reads + rapidjson Parse)
       runs OUTSIDE lock_shared_ha_data() to avoid serializing
       concurrent first-opens behind one thread doing the JSON parse.
       Atomic publish via cached_opts.store(release) -- if two threads
       race, the loser deletes its duplicate. MF-1 atomicity: subsequent
       TDB_TABLE_OPTIONS readers (no lock) see a fully-initialized
       pointee on any non-null acquire-load. */
    if (!share->cached_opts.load(std::memory_order_acquire))
    {
        ha_table_option_struct *fresh = new ha_table_option_struct();
        tidesdb_compute_opts_for_table(table, fresh);
        ha_table_option_struct *expected = nullptr;
        if (!share->cached_opts.compare_exchange_strong(
                expected, fresh, std::memory_order_release,
                std::memory_order_acquire))
        {
            /* Another thread won the race; discard our duplicate. */
            delete fresh;
        }
    }

    /*
      We resolve CF pointers only once (first open).  Subsequent opens by
      other connections reuse the already-resolved share.  We hold
      lock_shared_ha_data() to prevent concurrent open() calls from
      racing on the shared vectors.
    */
    lock_shared_ha_data();

    if (!share->cf)
    {
        share->cf_name = path_to_cf_name(name);
        share->cf = tidesdb_get_column_family(tdb_get_engine(), share->cf_name.c_str());
        if (!share->cf)
        {
            unlock_shared_ha_data();
            sql_print_error("[TIDESDB] CF '%s' not found for table '%s'", share->cf_name.c_str(),
                            name);
            DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
        }

        /* We determine PK info from table metadata */
        if (table->s->primary_key != MAX_KEY)
        {
            share->has_user_pk = true;
            share->pk_index = table->s->primary_key;
            share->pk_key_len = comparable_key_length(&table->key_info[share->pk_index]);
        }
        else
        {
            share->has_user_pk = false;
            share->pk_index = MAX_KEY;
            share->pk_key_len = HIDDEN_PK_SIZE;
        }

        /* We read isolation level from table options */
        if (TDB_TABLE_OPTIONS(table))
        {
            uint iso_idx = TDB_TABLE_OPTIONS(table)->isolation_level;
            if (iso_idx < array_elements(tdb_isolation_map))
                share->isolation_level = (tidesdb_isolation_level_t)tdb_isolation_map[iso_idx];
        }

        /* We read TTL configuration from table + field options */
        if (TDB_TABLE_OPTIONS(table)) share->default_ttl = TDB_TABLE_OPTIONS(table)->ttl;

        /* We read encryption configuration from table options */
        share->encrypted = false;
        share->encryption_key_id = TIDESDB_DEFAULT_ENCRYPTION_KEY_ID;
        share->encryption_key_version = 0;
        if (TDB_TABLE_OPTIONS(table) && TDB_TABLE_OPTIONS(table)->encrypted)
        {
            share->encrypted = true;
            share->encryption_key_id = (uint)TDB_TABLE_OPTIONS(table)->encryption_key_id;
            uint ver = encryption_key_get_latest_version(share->encryption_key_id);
            if (ver == ENCRYPTION_KEY_VERSION_INVALID)
            {
                sql_print_error("[TIDESDB] encryption key %u not available",
                                share->encryption_key_id);
                DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
            }
            share->encryption_key_version = ver;
        }

        share->ttl_field_idx = TIDESDB_TTL_FIELD_NONE;
        for (uint i = 0; i < table->s->fields; i++)
        {
            if (TDB_FIELD_OPTIONS(table->s->field[i]) && TDB_FIELD_OPTIONS(table->s->field[i])->ttl)
            {
                share->ttl_field_idx = (int)i;
                break;
            }
        }

        /* We cache table shape flags for hot-path short-circuiting.  We also
           capture the BLOB field indices so serialize_row's size estimate can
           iterate that short list instead of every field on every INSERT. */
        share->has_blobs = false;
        share->blob_field_indices.clear();
        for (uint i = 0; i < table->s->fields; i++)
        {
            if (table->s->field[i]->all_flags() & BLOB_FLAG)
            {
                share->has_blobs = true;
                share->blob_field_indices.push_back((uint16)i);
            }
        }
        share->has_ttl = (share->default_ttl > 0 || share->ttl_field_idx >= 0);

        /* We precompute comparable key lengths and index-type flags per index.
           Caching the type flags avoids a ki->algorithm dereference per row
           in write_row's dup-check loop and in update_row/delete_row. */
        for (uint i = 0; i < table->s->keys; i++)
        {
            share->idx_comp_key_len[i] = comparable_key_length(&table->key_info[i]);
            share->idx_is_fts[i] = is_fts_index(&table->key_info[i]);
            share->idx_is_spatial[i] = is_spatial_index(&table->key_info[i]);
        }

        /* Precompute per-index coverage bitmaps so try_keyread_from_index is
           O(set bits in read_set) instead of nested scans over key parts. */
        share->idx_cover.assign(table->s->keys, std::vector<bool>(table->s->fields, false));
        for (uint i = 0; i < table->s->keys; i++)
        {
            const KEY *ki = &table->key_info[i];
            for (uint p = 0; p < ki->user_defined_key_parts; p++)
            {
                uint fnr = ki->key_part[p].fieldnr;
                if (fnr > 0 && fnr - 1 < table->s->fields) share->idx_cover[i][fnr - 1] = true;
            }
            /* Secondary indexes also cover the PK columns appended to the key. */
            if (table->s->primary_key != MAX_KEY && i != table->s->primary_key)
            {
                const KEY *pk_key = &table->key_info[table->s->primary_key];
                for (uint p = 0; p < pk_key->user_defined_key_parts; p++)
                {
                    uint fnr = pk_key->key_part[p].fieldnr;
                    if (fnr > 0 && fnr - 1 < table->s->fields) share->idx_cover[i][fnr - 1] = true;
                }
            }
        }

        /* We resolve secondary index CFs */
        for (uint i = 0; i < table->s->keys; i++)
        {
            if (share->has_user_pk && i == share->pk_index)
            {
                share->idx_cfs.push_back(NULL);
                share->idx_cf_names.push_back("");
                continue;
            }
            std::string idx_name;
            tidesdb_column_family_t *icf =
                resolve_idx_cf(tdb_get_engine(), share->cf_name, table->key_info[i].name, idx_name);
            share->idx_cfs.push_back(icf);
            share->idx_cf_names.push_back(idx_name);
        }

        /* We count active secondary index CFs for fast-path skipping */
        share->num_secondary_indexes = 0;
        for (uint i = 0; i < share->idx_cfs.size(); i++)
            if (share->idx_cfs[i]) share->num_secondary_indexes++;

        /* We recover hidden-PK counter (auto-inc is derived at runtime via index_last) */
        recover_counters();

        /* We seed create_time from the .frm file's mtime */
        {
            char frm_path[FN_REFLEN];
            fn_format(frm_path, name, "", reg_ext, MY_UNPACK_FILENAME | MY_APPEND_EXT);
            MY_STAT st_buf;
            if (mysql_file_stat(0, frm_path, &st_buf, MYF(0))) share->create_time = st_buf.st_mtime;
        }
    }
    unlock_shared_ha_data();

    /* We set ref_length for position()/rnd_pos() */
    ref_length = share->pk_key_len;

    /* We mirror shape flags onto the handler so the row-fetch hot paths
       read from a local member instead of chasing `share` into shared
       memory on every row.  These mirror constants that never change for
       the open handler. */
    has_blobs_ = share->has_blobs;
    encrypted_ = share->encrypted;

    /* We precompute the record[1] pointer range so the BLOB path of
       fetch_row_by_pk/iter_read_current doesn't rebuild it per row. */
    if (table->record[1])
    {
        record1_lo_ = table->record[1];
        record1_hi_ = table->record[1] + table->s->reclength;
    }
    else
    {
        record1_lo_ = NULL;
        record1_hi_ = NULL;
    }

    /* LF-1: size key_unpack_scratch_ once now that table is available.
       Subsequent key_copy_to_comparable calls see the buffer already
       sized and skip the resize branch on the hot path. */
    if (table && table->s && key_unpack_scratch_.size() < table->s->reclength)
        key_unpack_scratch_.resize(table->s->reclength);

    DBUG_RETURN(0);
}

int ha_tidesdb::close(void)
{
    DBUG_ENTER("ha_tidesdb::close");
    if (scan_iter)
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    free_dup_iter_cache();
    /* stmt_txn is a borrowed pointer into the per-connection trx->txn.
       We do not free it here -- the txn is owned by the per-connection trx
       and will be freed in tidesdb_close_connection(). */
    stmt_txn = NULL;
    stmt_txn_dirty = false;
    DBUG_RETURN(0);
}

int ha_tidesdb::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info, dd::Table *)
{
    DBUG_ENTER("ha_tidesdb::create");

    std::string cf_name = path_to_cf_name(name);

    /* Build effective per-table options:
     *   1. Seed from the session's tidesdb_default_* THDVARs.
     *   2. If the CREATE TABLE statement supplied ENGINE_ATTRIBUTE='{...}',
     *      overlay those JSON keys on top. Unknown / unspecified keys
     *      retain their session-default values. Malformed JSON is a hard
     *      error -- we reject the CREATE TABLE rather than silently
     *      ignoring user intent.
     * The local opts_storage backs the pointer; build_cf_config copies
     * fields it needs into the CF config struct, so the storage can be
     * stack-local. */
    ha_table_option_struct opts_storage;
    tidesdb_seed_opts_from_session(ha_thd(), &opts_storage);
    if (create_info && create_info->engine_attribute.str &&
        create_info->engine_attribute.length) {
        if (!tidesdb_engine_attribute_to_options(create_info->engine_attribute,
                                                 &opts_storage)) {
            my_error(ER_WRONG_ARGUMENTS, MYF(0),
                     "ENGINE_ATTRIBUTE: malformed JSON for ENGINE=TIDESDB");
            DBUG_RETURN(HA_WRONG_CREATE_OPTION);
        }
    }
    ha_table_option_struct *opts = &opts_storage;

    tidesdb_column_family_config_t cfg = build_cf_config(opts);

    /* We create main data CF (we simply skip if it already exists, e.g. crash recovery) */
    if (!tidesdb_get_column_family(tdb_get_engine(), cf_name.c_str()))
    {
        int rc = tidesdb_create_column_family(tdb_get_engine(), cf_name.c_str(), &cfg);
        if (rc != TDB_SUCCESS)
        {
            sql_print_error("[TIDESDB] Failed to create CF '%s' (err=%d)", cf_name.c_str(), rc);
            DBUG_RETURN(tdb_rc_to_ha(rc, "create main_cf"));
        }
    }

    /* We create one CF per secondary index (named by key name for stability).
       Per-index USE_BTREE overrides the table-level setting. */
    for (uint i = 0; i < table_arg->s->keys; i++)
    {
        if (table_arg->s->primary_key != MAX_KEY && i == table_arg->s->primary_key) continue;

        std::string idx_cf = cf_name + CF_INDEX_INFIX + table_arg->key_info[i].name;
        if (!tidesdb_get_column_family(tdb_get_engine(), idx_cf.c_str()))
        {
            tidesdb_column_family_config_t idx_cfg = cfg;
            ha_index_option_struct *iopts = TDB_INDEX_OPTIONS(&table_arg->key_info[i]);
            if (iopts) idx_cfg.use_btree = iopts->use_btree ? 1 : 0;

            int rc = tidesdb_create_column_family(tdb_get_engine(), idx_cf.c_str(), &idx_cfg);
            if (rc != TDB_SUCCESS)
            {
                sql_print_error("[TIDESDB] Failed to create index CF '%s' (err=%d)", idx_cf.c_str(),
                                rc);
                DBUG_RETURN(tdb_rc_to_ha(rc, "create idx_cf"));
            }
        }
    }

    /* MySQL has no TABLE_SHARE::frm_image (no .frm files; metadata lives in
     * the Data Dictionary). TODO: persist full schema to TidesDB itself for
     * object-store replica recovery. For now just record the table name so
     * DROP TABLE can locate the underlying CF. */
    schema_cf_store_frm(name);

    DBUG_RETURN(0);
}

/* ******************** Data-at-rest encryption helpers ******************** */

/* Compiler-optimization-resistant memory wipe for key material.
   LF-3: prefer glibc's explicit_bzero(3) when available -- it includes
   a compiler barrier in addition to being un-optimizable-away, and is
   the canonical idiom that future readers expect. Fall back to a
   volatile-pointer loop where unavailable. Ubuntu 24.04 has
   glibc 2.38; explicit_bzero has been there since 2.25.

   Cf. OpenSSL's OPENSSL_cleanse and C11's memset_s. */
#if defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
#include <string.h>
static inline void tdb_secure_zero(void *p, size_t n) { explicit_bzero(p, n); }
#else
static inline void tdb_secure_zero(void *p, size_t n)
{
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}
#endif

/*
  Encrypt plaintext into enc_buf_.  Format is [IV (16 bytes)] [ciphertext].
  Returns true on success; out is set to the encrypted blob.  Returns false on
  any failure (key unavailable, /dev/urandom unreadable, cipher error) so the
  caller MUST NOT write `out` to disk -- doing so previously produced rows
  encrypted with uninitialized stack bytes as key or IV, breaking both
  recoverability and IND-CPA confidentiality (CVE-class: silent encryption
  bypass under failure mode).
*/
static bool tidesdb_encrypt_row_into(const std::string &plain, uint key_id, uint key_version,
                                     std::string &out)
{
    unsigned char key[TIDESDB_ENC_KEY_LEN];
    unsigned int klen = sizeof(key);
    if (encryption_key_get(key_id, key_version, key, &klen) != 0)
    {
        tdb_secure_zero(key, sizeof(key));
        sql_print_error("[TIDESDB] encryption_key_get failed (key_id=%u version=%u); "
                        "refusing to encrypt with uninitialized key material",
                        key_id, key_version);
        out.clear();
        return false;
    }

    unsigned char iv[TIDESDB_ENC_IV_LEN];
    if (my_random_bytes(iv, TIDESDB_ENC_IV_LEN) != 0)
    {
        tdb_secure_zero(key, sizeof(key));
        sql_print_error("[TIDESDB] my_random_bytes failed; refusing to encrypt with "
                        "uninitialized IV (would break CBC IND-CPA)");
        out.clear();
        return false;
    }

    /* L-3: guard the size_t -> unsigned int narrowing.
       encryption_encrypted_length computes ((src_len / 16) + 1) * 16
       which overflows unsigned int when src_len >= 0xFFFFFFF0; the
       resulting buffer would be far too small and the AES write would
       overrun. Real rows can't get this large under TidesDB's block
       size limits, but the check belongs here at the boundary. */
    constexpr size_t TIDESDB_ENCRYPT_MAX_PLAIN = 0xEFFFFFFFu;
    if (plain.size() > TIDESDB_ENCRYPT_MAX_PLAIN)
    {
        tdb_secure_zero(key, sizeof(key));
        sql_print_error("[TIDESDB] plaintext too large for encryption (%zu bytes); "
                        "refusing to encrypt",
                        plain.size());
        out.clear();
        return false;
    }
    unsigned int slen = (unsigned int)plain.size();
    unsigned int enc_len = encryption_encrypted_length(slen, key_id, key_version);
    out.resize(TIDESDB_ENC_IV_LEN + enc_len);

    memcpy(&out[0], iv, TIDESDB_ENC_IV_LEN);

    unsigned int dlen = enc_len;
    int rc = encryption_crypt((const unsigned char *)plain.data(), slen,
                              (unsigned char *)&out[TIDESDB_ENC_IV_LEN], &dlen, key, klen, iv,
                              TIDESDB_ENC_IV_LEN, ENCRYPTION_FLAG_ENCRYPT, key_id, key_version);
    tdb_secure_zero(key, sizeof(key));
    if (rc != 0)
    {
        sql_print_error("[TIDESDB] encryption_crypt(encrypt) failed rc=%d", rc);
        out.clear();
        return false;
    }
    out.resize(TIDESDB_ENC_IV_LEN + dlen);
    return true;
}

/*
  Decrypt a row stored as [IV (16)] [ciphertext] back to plaintext.  Returns
  an empty string on any failure (key unavailable, ciphertext truncated,
  cipher error).  Caller must check for empty -- previously the function
  proceeded with uninitialized key bytes when encryption_key_get failed,
  which on some failure modes silently returned junk plaintext.
*/
static std::string tidesdb_decrypt_row(const char *data, size_t len, uint key_id, uint key_version)
{
    if (len <= TIDESDB_ENC_IV_LEN)
    {
        sql_print_error("[TIDESDB] encrypted row too short (%zu bytes)", len);
        return std::string(); /* signal failure */
    }

    unsigned char key[TIDESDB_ENC_KEY_LEN];
    unsigned int klen = sizeof(key);
    if (encryption_key_get(key_id, key_version, key, &klen) != 0)
    {
        tdb_secure_zero(key, sizeof(key));
        sql_print_error("[TIDESDB] encryption_key_get failed (key_id=%u version=%u); "
                        "cannot decrypt row",
                        key_id, key_version);
        return std::string();
    }

    const unsigned char *iv = (const unsigned char *)data;
    const unsigned char *src = (const unsigned char *)data + TIDESDB_ENC_IV_LEN;
    unsigned int slen = (unsigned int)(len - TIDESDB_ENC_IV_LEN);

    std::string out;
    unsigned int dlen = slen + TIDESDB_ENC_KEY_LEN; /* padding slack */
    out.resize(dlen);

    int rc = encryption_crypt(src, slen, (unsigned char *)&out[0], &dlen, key, klen, iv,
                              TIDESDB_ENC_IV_LEN, ENCRYPTION_FLAG_DECRYPT, key_id, key_version);
    tdb_secure_zero(key, sizeof(key));
    if (rc != 0)
    {
        sql_print_error("[TIDESDB] encryption_crypt(decrypt) failed rc=%d", rc);
        return std::string(); /* signal failure */
    }
    out.resize(dlen);
    return out;
}

/* ******************** serialize / deserialize (BLOB deep-copy) ******************** */

/* Row format header constants live in ha_tidesdb.h so the stop-word
   loader and other callers can reference them without forward decls.
   Layout: [ROW_HEADER_MAGIC] [null_bytes_stored (2 LE)] [field_count (2 LE)]
   for ROW_HEADER_SIZE bytes total.  Enables instant ADD/DROP COLUMN. */

const std::string &ha_tidesdb::serialize_row(const uchar *buf)
{
    my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(buf - table->record[0]);

    /* Upper-bound packed size.  For non-BLOB tables the estimate is constant
       (header + null_bytes + reclength + 2 bytes per field for length-prefix
       overhead from Field_string::pack).  Cache it to avoid recomputing on
       every row.  For BLOB tables we must add the actual blob data sizes. */
    size_t est = share->cached_row_est;
    if (unlikely(est == 0))
    {
        est = ROW_HEADER_SIZE + table->s->null_bytes + table->s->reclength +
              FIELD_VARCHAR_LEN_PREFIX * table->s->fields;
        if (!share->has_blobs)
            share->cached_row_est = est; /* safe to cache -- constant for non-BLOB tables */
    }
    if (share->has_blobs)
    {
        /* Walk only the precomputed BLOB field list instead of every field. */
        for (uint16 idx : share->blob_field_indices)
        {
            Field *f = table->field[idx];
            if (f->is_real_null(ptrdiff)) continue;
            Field_blob *blob = (Field_blob *)f;
            est += blob->get_length(buf + (uintptr_t)(f->field_ptr() - table->record[0]));
        }
    }

    row_buf_.resize(est);
    uchar *start = (uchar *)&row_buf_[0];
    uchar *pos = start;

    /* Row header -- enables instant ADD/DROP COLUMN by recording the
       null bitmap size and field count at write time. */
    *pos++ = ROW_HEADER_MAGIC;
    uint16 nb = (uint16)table->s->null_bytes;
    uint16 fc = (uint16)table->s->fields;
    int2store(pos, nb);
    pos += sizeof(uint16);
    int2store(pos, fc);
    pos += sizeof(uint16);

    /* Null bitmap */
    memcpy(pos, buf, table->s->null_bytes);
    pos += table->s->null_bytes;

    /* Pack each non-null field using Field::pack().
       -- Fixed-size fields (INT, BIGINT, DATE)     copies pack_length() bytes
       -- CHAR                                      strips trailing spaces, stores length + data
       -- VARCHAR                                   stores actual length + data (not padded to max)
       -- BLOB                                      stores length + blob data inline */
    for (uint i = 0; i < table->s->fields; i++)
    {
        Field *f = table->field[i];
        if (f->is_real_null(ptrdiff)) continue;
        pos = f->pack(pos, buf + (uintptr_t)(f->field_ptr() - table->record[0]), UINT_MAX);
    }

    row_buf_.resize((size_t)(pos - start));

    if (share->encrypted)
    {
        /* We cache the encryption key version per-statement to avoid the
           expensive encryption_key_get_latest_version() syscall on every
           single row.  The cache is invalidated at statement start
           (enc_key_ver_valid_ = false in external_lock). */
        if (!enc_key_ver_valid_)
        {
            uint cur_ver = encryption_key_get_latest_version(share->encryption_key_id);
            if (cur_ver != ENCRYPTION_KEY_VERSION_INVALID)
            {
                share->encryption_key_version = cur_ver;
                cached_enc_key_ver_ = cur_ver;
            }
            else
            {
                cached_enc_key_ver_ = share->encryption_key_version;
            }
            enc_key_ver_valid_ = true;
        }
        /* We encrypt into enc_buf_ instead of replacing row_buf_, so that
           row_buf_'s heap capacity is preserved across calls.
           Writing directly into enc_buf_ reuses its heap capacity across rows,
           avoiding a per-row allocation when the encrypted size is stable. */
        if (!tidesdb_encrypt_row_into(row_buf_, share->encryption_key_id, cached_enc_key_ver_,
                                      enc_buf_))
        {
            enc_buf_.clear(); /* signal failure */
        }
        return enc_buf_;
    }

    return row_buf_;
}

void ha_tidesdb::deserialize_row(uchar *buf, const uchar *data, size_t len)
{
    const uchar *from = data;
    const uchar *from_end = data + len;

    /* All rows have the header([0xFE] [null_bytes(2)] [field_count(2)]) */
    if (unlikely(len < ROW_HEADER_SIZE || data[0] != ROW_HEADER_MAGIC))
    {
        /* Corrupted or truncated row, we zero the record to avoid garbage */
        memset(buf, 0, table->s->reclength);
        return;
    }

    from++;
    uint stored_null_bytes = uint2korr(from);
    from += sizeof(uint16);
    uint stored_fields = uint2korr(from);
    from += sizeof(uint16);

    /* Null bitmap -- we copy the smaller of stored vs current.
       When columns were added (stored_null_bytes < table->s->null_bytes),
       fill the extra null bitmap bytes from the table's default record
       so that new columns inherit their correct DEFAULT / NOT NULL state
       rather than blindly marking them NULL. */
    if ((size_t)(from_end - from) < stored_null_bytes) return;
    uint copy_nb = MY_MIN(stored_null_bytes, table->s->null_bytes);
    memcpy(buf, from, copy_nb);
    if (copy_nb < table->s->null_bytes)
        memcpy(buf + copy_nb, table->s->default_values + copy_nb, table->s->null_bytes - copy_nb);
    from += stored_null_bytes;

    /* We unpack.  Only unpack up to MIN(stored_fields, current_fields).
       If the row has more fields than the current schema (DROP COLUMN),
       the extra packed data is simply skipped.
       If the row has fewer fields (ADD COLUMN), fill the missing fields
       from the table's default record so they get their DEFAULT value. */
    uint unpack_count = MY_MIN(stored_fields, table->s->fields);

    /* Pre-fill default values for columns added after this row was written.
       For each newly-added field:
        (1) copy the field's bytes from default_values into buf,
        (2) re-derive the null bit from default_values and SET or CLEAR the
            corresponding bit in buf's null bitmap.
       Step (2) is critical: MySQL pre-initializes unused null-bitmap bits
       to 1 in default_values, so existing rows already carry "1" in bit
       positions that later become a new column's null bit. Just copying
       the stored null bitmap leaves the new column marked NULL even when
       its default is non-NULL. */
    if (stored_fields < table->s->fields)
    {
        my_ptrdiff_t def_off = (my_ptrdiff_t)(table->s->default_values - table->record[0]);
        my_ptrdiff_t buf_off = (my_ptrdiff_t)(buf - table->record[0]);
        for (uint i = stored_fields; i < table->s->fields; i++)
        {
            Field *f = table->field[i];
            uchar *to = buf + (uintptr_t)(f->field_ptr() - table->record[0]);
            const uchar *def_src = (const uchar *)f->field_ptr() + def_off;
            memcpy(to, def_src, f->pack_length());

            /* Restore the new column's null bit from default_values.
               get_null_ptr() returns the field's null byte in record[0];
               offset to default_values for the read, and offset to buf for
               the write. */
            if (f->is_nullable())
            {
                uchar *buf_null_byte = f->get_null_ptr() + buf_off;
                if (f->is_real_null(def_off))
                    *buf_null_byte |= f->null_bit;
                else
                    *buf_null_byte &= ~f->null_bit;
            }
        }
    }

    my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(buf - table->record[0]);
    for (uint i = 0; i < unpack_count; i++)
    {
        Field *f = table->field[i];
        if (f->is_real_null(ptrdiff)) continue;
        if (from >= from_end) break;
        uchar *to = buf + (uintptr_t)(f->field_ptr() - table->record[0]);
        /* Field_blob::unpack uses set_ptr() which writes through field->field_ptr()
           (always pointing into record[0]).  When buf != record[0], we must
           shift the field pointer so set_ptr writes into the correct record. */
        f->move_field_offset(ptrdiff);
        const uchar *next = f->unpack(to, from, 0u);  /* MySQL: 0 = use field's own packlength */
        f->move_field_offset(-ptrdiff);
        if (!next) break;
        from = next;
    }
}

void ha_tidesdb::deserialize_row(uchar *buf, const std::string &row)
{
    const std::string *plain = &row;
    std::string decrypted;

    if (share->encrypted)
    {
        decrypted = tidesdb_decrypt_row(row.data(), row.size(), share->encryption_key_id,
                                        share->encryption_key_version);
        if (decrypted.empty())
        {
            /* Decryption failed! we zero record to avoid returning garbage */
            memset(buf, 0, table->s->reclength);
            return;
        }
        last_row = std::move(decrypted);
        plain = &last_row;
    }

    deserialize_row(buf, (const uchar *)plain->data(), plain->size());
}

/* ******************** fetch_row_by_pk ******************** */

/*
  Point-lookup a row by its PK bytes (without namespace prefix).
  Sets current_pk + last_row.  Returns 0, HA_ERR_KEY_NOT_FOUND,
  or HA_ERR_LOCK_DEADLOCK (on TDB_ERR_CONFLICT).
*/
int ha_tidesdb::fetch_row_by_pk(tidesdb_txn_t *txn, const uchar *pk, uint pk_len, uchar *buf)
{
    uchar dk[DATA_KEY_BUF_LEN];
    uint dk_len = build_data_key(pk, pk_len, dk);

    uint8_t *value = NULL;
    size_t value_size = 0;
    int rc = tidesdb_txn_get(txn, share->cf, dk, dk_len, &value, &value_size);
    if (rc == TDB_ERR_NOT_FOUND) return HA_ERR_KEY_NOT_FOUND;
    if (rc != TDB_SUCCESS) return tdb_rc_to_ha(rc, "fetch_row_by_pk");

    if (likely(!has_blobs_ && !encrypted_))
    {
        /* Zero-copy path, we deserialize directly from API buffer */
        deserialize_row(buf, (const uchar *)value, value_size);
        tidesdb_free(value);
    }
    else
    {
        /* For BLOB tables, Field_blob::unpack() stores pointers into the
           source buffer.  These pointers must remain valid until the next
           fetch into the SAME record buffer.  The MariaDB handler API
           (e.g., mhnsw vector index maintenance) may interleave reads into
           record[0] and record[1], so we maintain two backing buffers:
           last_row for record[0] fetches, last_row2 for record[1] fetches.
           This prevents a fetch into record[1] from invalidating BLOB
           pointers that record[0] still references.

           We identify record[1] using the precomputed bounds set in open(). */
        bool is_rec1 = record1_lo_ && buf >= record1_lo_ && buf < record1_hi_;
        std::string &backing = is_rec1 ? last_row2 : last_row;
        backing.assign((const char *)value, value_size);
        tidesdb_free(value);
        deserialize_row(buf, backing);
    }
    memcpy(current_pk_buf_, pk, pk_len);
    current_pk_len_ = pk_len;

    return 0;
}

/* ******************** compute_row_ttl ******************** */

/*
  Compute the absolute TTL timestamp for a row being written.
  Priority -- per-row TTL_COL value > table-level TTL option > no expiration.
  Returns -1 (no expiration) or a future absolute Unix timestamp.
*/
time_t ha_tidesdb::compute_row_ttl(const uchar *buf)
{
    long long ttl_seconds = 0;

    if (share->ttl_field_idx >= 0)
    {
        Field *f = table->field[share->ttl_field_idx];
        my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(buf - table->record[0]);
        if (!f->is_real_null(ptrdiff))
        {
            f->move_field_offset(ptrdiff);
            ttl_seconds = f->val_int();
            f->move_field_offset(-ptrdiff);
        }
    }

    /* Session TTL override, we use cached value to avoid THDVAR + ha_thd()
       on every row.  The cache is populated once per statement in write_row
       / update_row and invalidated in external_lock(F_UNLCK). */
    if (ttl_seconds <= 0)
    {
        if (cached_sess_ttl_ > 0) ttl_seconds = (long long)cached_sess_ttl_;
    }

    if (ttl_seconds <= 0 && share->default_ttl > 0) ttl_seconds = (long long)share->default_ttl;

    if (ttl_seconds <= 0) return TIDESDB_TTL_NONE;

    /* We use cached time(NULL) to avoid the vDSO/syscall per row.
       n-second granularity is more than sufficient for TTL. */
    if (!cached_time_valid_)
    {
        cached_time_ = time(NULL);
        cached_time_valid_ = true;
    }

    return (time_t)(cached_time_ + ttl_seconds);
}

/* ******************** iter_read_current ******************** */

/*
  Read the current iterator position in the main data CF.
  Skips non-data keys (meta keys).  Sets current_pk + last_row.
  Does not advance the iterator.
*/
int ha_tidesdb::iter_read_current(uchar *buf)
{
    while (scan_iter && tidesdb_iter_valid(scan_iter))
    {
        uint8_t *key = NULL;
        size_t key_size = 0;
        uint8_t *value = NULL;
        size_t value_size = 0;
        if (tidesdb_iter_key_value(scan_iter, &key, &key_size, &value, &value_size) != TDB_SUCCESS)
            return HA_ERR_END_OF_FILE;

        /* We skip non-data keys (meta namespace) */
        if (!is_data_key(key, key_size))
        {
            tidesdb_iter_next(scan_iter);
            continue;
        }

        /* We extract PK bytes (everything after the namespace prefix) */
        current_pk_len_ = (uint)(key_size - KEY_NAMESPACE_LEN);
        memcpy(current_pk_buf_, key + KEY_NAMESPACE_LEN, current_pk_len_);

        if (likely(!has_blobs_ && !encrypted_))
        {
            deserialize_row(buf, (const uchar *)value, value_size);
        }
        else
        {
            bool is_rec1 = record1_lo_ && buf >= record1_lo_ && buf < record1_hi_;
            std::string &backing = is_rec1 ? last_row2 : last_row;
            backing.assign((const char *)value, value_size);
            deserialize_row(buf, backing);
        }
        return 0;
    }
    return HA_ERR_END_OF_FILE;
}

/* ******************** write_row (INSERT) ******************** */

int ha_tidesdb::write_row(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::write_row");

    /* We need all columns readable for PK extraction, secondary index
       key building, serialization, and TTL computation. */
    my_bitmap_map *old_map = tmp_use_all_columns(table, table->read_set);

    bool pk_auto_generated = false; /* true when PK was auto-generated (guaranteed unique) */
    if (table->next_number_field && buf == table->record[0])
    {
        /* If the PK field is 0/NULL, MariaDB's update_auto_increment() will
           generate a unique value from our atomic counter.  We can skip the
           expensive PK uniqueness point-get in that case.
           Only safe when the auto-inc field is the ENTIRE PK (single-column).
           For composite PKs, auto-inc only guarantees uniqueness within
           the auto-inc column, not the full composite key. */
        if (table->next_number_field->val_int() == 0 && share->has_user_pk &&
            table->key_info[share->pk_index].user_defined_key_parts == 1)
            pk_auto_generated = true;
        int ai_err = update_auto_increment();
        if (ai_err)
        {
            tmp_restore_column_map(table->read_set, old_map);
            DBUG_RETURN(ai_err);
        }
        /* We keep the shared counter ahead of any explicitly-supplied value
           so that future auto-generated values don't collide. */
        ulonglong val = table->next_number_field->val_int();
        ulonglong cur = share->auto_inc_val.load(std::memory_order_relaxed);
        while (val > cur)
        {
            if (share->auto_inc_val.compare_exchange_weak(cur, val, std::memory_order_relaxed))
                break;
        }
    }

    /* We build PK bytes for this new row */
    uchar pk[MAX_KEY_LENGTH];
    uint pk_len;
    if (share->has_user_pk)
    {
        pk_len = pk_from_record(buf, pk);
    }
    else
    {
        /* Hidden PK -- we generate next row-id */
        uint64_t row_id = share->next_row_id.fetch_add(1, std::memory_order_relaxed);
        encode_be64(row_id, pk);
        pk_len = HIDDEN_PK_SIZE;
    }

    uchar dk[DATA_KEY_BUF_LEN];
    uint dk_len = build_data_key(pk, pk_len, dk);

    const std::string &row_data = serialize_row(buf);
    if (share->encrypted && row_data.empty())
    {
        tmp_restore_column_map(table->read_set, old_map);
        sql_print_error("[TIDESDB] write_row encryption failed");
        DBUG_RETURN(HA_ERR_GENERIC);
    }
    const uint8_t *row_ptr = (const uint8_t *)row_data.data();
    size_t row_len = row_data.size();

    /* Lazy txn -- we ensure stmt_txn exists on first data access */
    {
        int erc = ensure_stmt_txn();
        if (erc)
        {
            tmp_restore_column_map(table->read_set, old_map);
            DBUG_RETURN(erc);
        }
    }
    tidesdb_txn_t *txn = stmt_txn;
    stmt_txn_dirty = true;

    /* We use cached pointers from external_lock to avoid per-row overhead. */
    tidesdb_trx_t *trx = cached_trx_;
    if (trx)
    {
        trx->dirty = true;
        trx->stmt_was_dirty = true;
    }

    /* We acquire pessimistic row lock for INSERT when pessimistic_locking=ON.
       Without this, INSERT bypasses locks held by SELECT ... FOR UPDATE,
       UPDATE, and DELETE on the same PK -- breaking the serialization
       guarantee that pessimistic locking is supposed to provide.
       We use the comparable PK bytes (pk, pk_len) which are the same key
       format used by index_read_map() for lock acquisition. */
    if (unlikely(srv_pessimistic_locking) && share->has_user_pk && trx)
    {
        int lrc = row_lock_acquire(trx, pk, pk_len, cached_thd_);
        if (lrc)
        {
            tmp_restore_column_map(table->read_set, old_map);
            DBUG_RETURN(lrc);
        }
    }

    /* We cache THDVAR lookups once per statement. */
    if (!cached_thdvars_valid_)
    {
        cached_skip_unique_ = THDVAR(cached_thd_, skip_unique_check);
        cached_sess_ttl_ = THDVAR(cached_thd_, ttl);
        cached_single_delete_primary_ = THDVAR(cached_thd_, single_delete_primary);
        cached_thdvars_valid_ = true;
    }

    /* We check PK uniqueness before inserting (TidesDB put overwrites silently).
       IODKU needs HA_ERR_FOUND_DUPP_KEY so the server can run the UPDATE clause.
       REPLACE INTO also needs it when secondary indexes exist (old index entries
       must be cleaned up via delete+reinsert).  When write_can_replace_ is set
       and the table has no secondary indexes, we skip the dup check entirely --
       tidesdb_txn_put will overwrite the old value, which is exactly what REPLACE
       wants, saving a full point-lookup per row.
       SET SESSION tidesdb_skip_unique_check=1 (bulk load) also bypasses this.
       When the PK was auto-generated by our O(1) atomic counter, the value is
       guaranteed unique (seeded from max existing value) -- skip the point-get. */
    bool skip_unique = cached_skip_unique_ || pk_auto_generated;
    if (share->has_user_pk && !skip_unique &&
        !(write_can_replace_ && share->num_secondary_indexes == 0))
    {
        uint8_t *dup_val = NULL;
        size_t dup_len = 0;
        int grc = tidesdb_txn_get(txn, share->cf, dk, dk_len, &dup_val, &dup_len);
        if (grc == TDB_SUCCESS)
        {
            tidesdb_free(dup_val);
            errkey = share->pk_index;
            last_dup_key_no_ = errkey;  /* preserved for info(HA_STATUS_ERRKEY) */
            memcpy(dup_ref, pk, pk_len);
            tmp_restore_column_map(table->read_set, old_map);
            DBUG_RETURN(HA_ERR_FOUND_DUPP_KEY);
        }
        if (grc != TDB_ERR_NOT_FOUND)
        {
            tmp_restore_column_map(table->read_set, old_map);
            DBUG_RETURN(tdb_rc_to_ha(grc, "write_row pk_dup_check"));
        }
    }

    /* We check UNIQUE secondary index uniqueness.
       Cached dup-check iterators avoid the catastrophically expensive
       tidesdb_iter_new() (O(num_sstables) merge-heap construction) on
       every single INSERT.  The iterator per unique index is created
       once and reused via seek() across rows within the same txn.
       Note: skip_unique (which includes pk_auto_generated) is intentionally
       NOT used here. AUTO_INCREMENT only guarantees PK uniqueness; secondary
       UNIQUE indexes (e.g., UNIQUE KEY (region, sku) on a table with an
       independent auto-inc id) still need their own check, otherwise
       INSERT/REPLACE/INSERT IGNORE silently bypass the constraint. Only the
       explicit session-level skip_unique_check should bypass this. */
    if (share->num_secondary_indexes > 0 && !cached_skip_unique_)
    {
        /* trx already cached at top of write_row */
        uint64_t cur_gen = trx ? trx->txn_generation : 0;

        for (uint i = 0; i < table->s->keys; i++)
        {
            if (share->has_user_pk && i == share->pk_index) continue;
            if (i >= share->idx_cfs.size() || !share->idx_cfs[i]) continue;
            if (share->idx_is_fts[i] || share->idx_is_spatial[i]) continue;
            if (!(table->key_info[i].flags & HA_NOSAME)) continue;

            uchar idx_prefix[MAX_KEY_LENGTH];
            uint idx_prefix_len = make_comparable_key(
                &table->key_info[i], buf, table->key_info[i].user_defined_key_parts, idx_prefix);

            /* We get or create cached dup-check iterator for this index.
               Invalidate if the txn changed (commit/reset frees txn ops
               that the iterator's MERGE_SOURCE_TXN_OPS depends on). */
            tidesdb_iter_t *dup_iter = dup_iter_cache_[i];
            if (dup_iter && (dup_iter_txn_[i] != txn || dup_iter_txn_gen_[i] != cur_gen))
            {
                tidesdb_iter_free(dup_iter);
                dup_iter = NULL;
                dup_iter_cache_[i] = NULL;
            }
            if (!dup_iter)
            {
                {
                    int irc = tidesdb_iter_new(txn, share->idx_cfs[i], &dup_iter);
                    if (irc != TDB_SUCCESS || !dup_iter)
                    {
                        /* Iterator creation failed, thus cannot safely skip the
                           uniqueness check or we risk silent UNIQUE violations.
                           Propagate the error to the caller. */
                        tmp_restore_column_map(table->read_set, old_map);
                        DBUG_RETURN(tdb_rc_to_ha(irc, "write_row dup_iter_new"));
                    }
                }
                dup_iter_cache_[i] = dup_iter;
                dup_iter_txn_[i] = txn;
                dup_iter_txn_gen_[i] = cur_gen;
                dup_iter_count_++;
            }

            tidesdb_iter_seek(dup_iter, idx_prefix, idx_prefix_len);
            if (tidesdb_iter_valid(dup_iter))
            {
                uint8_t *fk = NULL;
                size_t fks = 0;
                if (tidesdb_iter_key(dup_iter, &fk, &fks) == TDB_SUCCESS && fks >= idx_prefix_len &&
                    memcmp(fk, idx_prefix, idx_prefix_len) == 0)
                {
                    /* We extract PK suffix from the index key for dup_ref */
                    size_t dup_pk_len = fks - idx_prefix_len;
                    if (dup_pk_len > 0 && dup_pk_len <= ref_length)
                        memcpy(dup_ref, fk + idx_prefix_len, dup_pk_len);
                    errkey = i;
                    last_dup_key_no_ = errkey;  /* preserved for info(HA_STATUS_ERRKEY) */
                    tmp_restore_column_map(table->read_set, old_map);
                    DBUG_RETURN(HA_ERR_FOUND_DUPP_KEY);
                }
            }
        }
    }

    /* We compute TTL when the table has TTL configured or the session overrides it.
       Uses cached_sess_ttl_ to avoid THDVAR + ha_thd() per row. */
    time_t row_ttl =
        (share->has_ttl || cached_sess_ttl_ > 0) ? compute_row_ttl(buf) : TIDESDB_TTL_NONE;

    /* We insert data row */
    int rc = tidesdb_txn_put(txn, share->cf, dk, dk_len, row_ptr, row_len, row_ttl);
    if (rc != TDB_SUCCESS) goto err;

    /* We maintain secondary indexes */
    memcpy(current_pk_buf_, pk, pk_len);
    current_pk_len_ = pk_len;
    /* We maintain all secondary indexes in a single consolidated loop.
       Loop invariants are hoisted to avoid redundant pointer dereferences
       per iteration. Regular, FTS, and spatial indexes are dispatched
       inline to eliminate 2/3 of loop overhead vs 3 separate loops. */
    if (share->num_secondary_indexes > 0)
    {
        const uint num_keys = table->s->keys;
        const bool has_user_pk = share->has_user_pk;
        const uint pk_index = share->pk_index;
        const size_t idx_cfs_sz = share->idx_cfs.size();

        for (uint i = 0; i < num_keys; i++)
        {
            if (has_user_pk && i == pk_index) continue;
            if (i >= idx_cfs_sz || !share->idx_cfs[i]) continue;

            const KEY *ki = &table->key_info[i];

            if (ki->algorithm == HA_KEY_ALG_FULLTEXT)
            {
                /* FTS index maintenance.
                   Per-row tokenization reuses thread-local scratch buffers
                   so steady-state INSERT on FTS-indexed tables fires zero
                   allocator activity in this path beyond what the
                   tokenizer itself does. The scratch is held via a
                   thread_local POINTER (lazy-allocated on first use)
                   rather than `static thread_local std::vector ...` --
                   dlopen'd plugins have a static-TLS budget too small
                   for std::vector / std::unordered_map's storage and
                   the load fails with "cannot allocate memory in static
                   TLS block". The pointer fits in 8 bytes of TLS; the
                   heap object lives for the thread's lifetime. */
                CHARSET_INFO *fts_cs = ki->key_part[0].field->charset();
                struct WriteRowFtsScratch {
                    std::vector<fts_token_t> tokens;
                    std::unordered_map<std::string, uint16> tf_map;
                };
                /* MF-8: std::unique_ptr -- destructor runs at thread
                   exit (__cxa_thread_atexit) so no pooled-thread leak. */
                static thread_local std::unique_ptr<WriteRowFtsScratch> fts_scratch;
                if (!fts_scratch) fts_scratch = std::make_unique<WriteRowFtsScratch>();
                auto &fts_tokens = fts_scratch->tokens;
                auto &tf_map = fts_scratch->tf_map;
                fts_tokens.clear();
                tf_map.clear();
                fts_extract_and_tokenize(table, ki, buf, fts_cs, fts_tokens);

                for (auto &tok : fts_tokens) tf_map[tok.word]++;
                uint32 word_count = (uint32)fts_tokens.size();

                for (auto &[term, tf] : tf_map)
                {
                    uchar fk[FTS_KEY_BUF_LEN];
                    uint fk_len = fts_build_key(term.data(), (uint)term.size(), pk, pk_len, fk);
                    uchar fv[FTS_VALUE_LEN];
                    fts_build_value(tf, word_count, fv);
                    rc = tidesdb_txn_put(txn, share->idx_cfs[i], fk, fk_len, fv, FTS_VALUE_LEN,
                                         row_ttl);
                    if (rc != TDB_SUCCESS) goto err;
                }

                /* M-4: in bulk INSERT, accumulate the delta and flush at
                   end_bulk_insert as a single RMW per key. Outside bulk
                   mode (single-row INSERT in autocommit), the per-row
                   RMW is unavoidable without changing transaction
                   semantics, so we leave that path inline. */
                if (in_bulk_insert_ && i < MAX_KEY)
                {
                    fts_meta_doc_delta_[i] += FTS_DOC_DELTA_ADD;
                    fts_meta_word_delta_[i] += (int64_t)word_count;
                }
                else
                {
                    fts_update_meta(share, txn, share->cf, i, FTS_DOC_DELTA_ADD,
                                    (int64_t)word_count);
                }
            }
            else if (ki->algorithm == HA_KEY_ALG_RTREE)
            {
                /* Spatial index maintenance */
                Field *geom_field = ki->key_part[0].field;
                my_ptrdiff_t ptd = (my_ptrdiff_t)(buf - table->record[0]);
                geom_field->move_field_offset(ptd);
                String geom_str;
                geom_field->val_str(&geom_str, &geom_str);
                geom_field->move_field_offset(-ptd);

                double xmin, ymin, xmax, ymax;
                if (geom_str.length() > 0 &&
                    spatial_compute_mbr((const uchar *)geom_str.ptr(), geom_str.length(), &xmin,
                                        &ymin, &xmax, &ymax))
                {
                    double cx = (xmin + xmax) / MBR_CENTROID_DIV;
                    double cy = (ymin + ymax) / MBR_CENTROID_DIV;
                    uchar sk[SPATIAL_HILBERT_KEY_LEN + MAX_KEY_LENGTH];
                    uint sk_len = spatial_build_key(cx, cy, pk, pk_len, sk);
                    uchar sv[SPATIAL_MBR_VALUE_LEN];
                    spatial_build_value(xmin, ymin, xmax, ymax, sv);
                    rc = tidesdb_txn_put(txn, share->idx_cfs[i], sk, sk_len, sv,
                                         SPATIAL_MBR_VALUE_LEN, row_ttl);
                    if (rc != TDB_SUCCESS) goto err;
                }
            }
            else
            {
                /* Regular secondary index maintenance */
                uchar ik[SEC_IDX_KEY_BUF_LEN];
                uint ik_len = sec_idx_key(i, buf, ik);
                rc = tidesdb_txn_put(txn, share->idx_cfs[i], ik, ik_len, &tdb_empty_val,
                                     sizeof(tdb_empty_val), row_ttl);
                if (rc != TDB_SUCCESS) goto err;
            }
        }
    }

    /* We track ops for bulk insert batching (1 data + N secondary index puts) */
    if (in_bulk_insert_)
    {
        bulk_insert_ops_ += 1 + share->num_secondary_indexes;
        if (bulk_insert_ops_ >= TIDESDB_BULK_INSERT_BATCH_OPS)
        {
            int mrc = maybe_bulk_commit(trx);
            if (mrc)
            {
                tmp_restore_column_map(table->read_set, old_map);
                DBUG_RETURN(mrc);
            }
            bulk_insert_ops_ = 0;
        }
    }

    /* Commit happens in external_lock(F_UNLCK). */
    tmp_restore_column_map(table->read_set, old_map);
    DBUG_RETURN(0);

err:
    tmp_restore_column_map(table->read_set, old_map);
    DBUG_RETURN(tdb_rc_to_ha(rc, "write_row"));
}

/* ******************** AUTO_INCREMENT (O(1) atomic counter) ******************** */

/*
  Override the default get_auto_increment() which calls index_last() on every
  single auto-commit INSERT.  That creates and destroys a TidesDB merge-heap
  iterator each time -- O(N sources).  Instead, we maintain an in-memory atomic
  counter on TidesDB_share that is seeded once from the table data at open time
  and atomically incremented thereafter -- O(1).
*/
void ha_tidesdb::get_auto_increment(ulonglong offset, ulonglong increment,
                                    ulonglong nb_desired_values, ulonglong *first_value,
                                    ulonglong *nb_reserved_values)
{
    DBUG_ENTER("ha_tidesdb::get_auto_increment");

    /* Atomic fetch-and-add -- each caller gets a unique range.
       The counter stores the last value that was handed out. */
    ulonglong cur = share->auto_inc_val.load(std::memory_order_relaxed);
    ulonglong next;
    do
    {
        next = cur + nb_desired_values;
    } while (!share->auto_inc_val.compare_exchange_weak(cur, next, std::memory_order_relaxed));

    *first_value = cur + 1;
    /*
      We reserve exactly what was asked for.  MariaDB's update_auto_increment()
      will call us again when the interval is exhausted.
    */
    *nb_reserved_values = nb_desired_values;

    DBUG_VOID_RETURN;
}

/*
  Reset the auto-increment counter(s) to the given value.  MariaDB's default
  truncate() path calls this after delete_all_rows, and ALTER TABLE ...
  AUTO_INCREMENT=N routes here as well.  The next auto-generated ID equals
  `value` itself, so we store `value - 1` (get_auto_increment does
  fetch-add and returns cur+1).  `value == 0` is the TRUNCATE case reset
  to 1.  Hidden-PK row-id gets the same treatment for consistency.
*/
int ha_tidesdb::reset_auto_increment(ulonglong value)
{
    DBUG_ENTER("ha_tidesdb::reset_auto_increment");
    if (!share) DBUG_RETURN(0);

    ulonglong new_val = value > 0 ? value - 1 : 0;
    share->auto_inc_val.store(new_val, std::memory_order_relaxed);

    /* Hidden PK row-ids are one-based (delete_all_rows stores
       HIDDEN_PK_FIRST_ROW_ID for empty tables).  Treat value==0 as restart. */
    uint64_t new_rowid = value > 0 ? (uint64_t)value : HIDDEN_PK_FIRST_ROW_ID;
    share->next_row_id.store(new_rowid, std::memory_order_relaxed);

    DBUG_RETURN(0);
}

/* ******************** Table scan (SELECT) ******************** */

int ha_tidesdb::rnd_init(bool scan)
{
    DBUG_ENTER("ha_tidesdb::rnd_init");

    current_pk_len_ = 0;
    scan_dir_ = DIR_NONE;

    /* Lazy txn, we ensure stmt_txn exists */
    {
        int erc = ensure_stmt_txn();
        if (erc) DBUG_RETURN(erc);
    }
    scan_txn = stmt_txn;

    /* We use cached trx pointer (set in external_lock) to avoid
       ha_thd() virtual dispatch + thd_get_ha_data() hash lookup
       on every scan init -- this is a hot path in nested-loop joins. */
    uint64_t cur_gen = cached_trx_ ? cached_trx_->txn_generation : 0;

    if (scan_iter &&
        (scan_iter_cf_ != share->cf || scan_iter_txn_ != scan_txn || scan_iter_txn_gen_ != cur_gen))
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }

    if (!scan_iter)
    {
        int rc = tidesdb_iter_new(scan_txn, share->cf, &scan_iter);
        if (rc != TDB_SUCCESS)
        {
            scan_txn = NULL;
            DBUG_RETURN(tdb_rc_to_ha(rc, "rnd_init txn_begin"));
        }
        scan_iter_cf_ = share->cf;
        scan_iter_txn_ = scan_txn;
        scan_iter_txn_gen_ = cur_gen;
    }

    /* We seek past meta keys to the first data key */
    uint8_t data_prefix = KEY_NS_DATA;
    tidesdb_iter_seek(scan_iter, &data_prefix, 1);

    DBUG_RETURN(0);
}

int ha_tidesdb::rnd_end()
{
    DBUG_ENTER("ha_tidesdb::rnd_end");

    /* We do not free scan_iter, we keep cached for reuse within this statement.
       Iterator is freed in external_lock(F_UNLCK) or close(). */
    scan_txn = NULL;

    DBUG_RETURN(0);
}

int ha_tidesdb::rnd_next(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::rnd_next");

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    /* We advance past the last-read entry.  on the first call after rnd_init
     * the iterator is already positioned at the first data key by the seek
     * in rnd_init, so we skip the advance (scan_dir_ == DIR_NONE). */
    if (scan_dir_ != DIR_NONE) tidesdb_iter_next(scan_iter);

    int ret = iter_read_current(buf);
    if (ret == 0) scan_dir_ = DIR_FORWARD;

    DBUG_RETURN(ret);
}

/* ******************** position / rnd_pos ******************** */

void ha_tidesdb::position(const uchar *record)
{
    DBUG_ENTER("ha_tidesdb::position");
    /* We store current PK bytes into ref for later rnd_pos() retrieval */
    memcpy(ref, current_pk_buf_, current_pk_len_);
    DBUG_VOID_RETURN;
}

int ha_tidesdb::rnd_pos(uchar *buf, uchar *pos)
{
    DBUG_ENTER("ha_tidesdb::rnd_pos");

    /* Lazy txn, we ensure stmt_txn exists */
    {
        int erc = ensure_stmt_txn();
        if (erc) DBUG_RETURN(erc);
    }

    int ret = fetch_row_by_pk(stmt_txn, pos, ref_length, buf);
    DBUG_RETURN(ret);
}

/* ******************** Index scan ******************** */

int ha_tidesdb::index_init(uint idx, bool sorted)
{
    DBUG_ENTER("ha_tidesdb::index_init");
    active_index = idx;
    idx_pk_exact_done_ = false;
    scan_dir_ = DIR_NONE;
    spatial_scan_active_ = false;

    /* Drop stale ICP state left over from prior scans on this handler.
       Clear when:
        (a) the previous push targeted a different key, OR
        (b) the optimizer cleared pushed_idx_cond but left
            in_range_check_pushed_down set (stale push after
            cancel_pushed_idx_cond was partially run, or after the engine
            consumed the condition).
       Without this, handler::compare_key short-circuits to 0 because
       in_range_check_pushed_down is true, read_range_next stops enforcing
       end_range, and rows past the upper bound leak through (UNION ALL on
       the same secondary index reproduces this). */
    if ((pushed_idx_cond_keyno != MAX_KEY && pushed_idx_cond_keyno != idx) ||
        (pushed_idx_cond == nullptr && in_range_check_pushed_down))
        cancel_pushed_idx_cond();

    /* Cache is_pk for the duration of the scan so navigation methods can
       read a member instead of re-deriving the answer per row. */
    is_pk_ = share->has_user_pk && idx == share->pk_index;

    {
        int erc = ensure_stmt_txn();
        if (erc) DBUG_RETURN(erc);
    }
    scan_txn = stmt_txn;

    /* We determine which CF to iterate (lazily -- iterator created on demand) */
    tidesdb_column_family_t *target_cf;
    if (share->has_user_pk && idx == share->pk_index)
        target_cf = share->cf;
    else if (idx < share->idx_cfs.size() && share->idx_cfs[idx])
        target_cf = share->idx_cfs[idx];
    else
    {
        scan_txn = NULL;
        scan_cf_ = NULL;
        sql_print_error("[TIDESDB] index_init: no CF for index %u", idx);
        DBUG_RETURN(HA_ERR_GENERIC);
    }

    scan_cf_ = target_cf;

    /* We reuse cached iterator if it belongs to the same CF and same txn.
       In nested-loop joins, index_init/index_end cycle N times on the
       same index; reusing the iterator avoids N expensive iter_new() calls
       (each builds a merge heap from all SSTables).

       If the txn changed (e.g. after COMMIT created a new one), the
       iterator holds a stale txn pointer and must be recreated.
       We compare both the pointer and a monotonic generation counter
       because the allocator can reuse the same address for a new txn.

       We use cached_trx_ (set in external_lock) to avoid ha_thd() virtual
       dispatch + thd_get_ha_data() hash lookup on every iteration of
       the outer loop in nested-loop joins. */
    uint64_t cur_gen = cached_trx_ ? cached_trx_->txn_generation : 0;

    if (scan_iter &&
        (scan_iter_cf_ != target_cf || scan_iter_txn_ != scan_txn || scan_iter_txn_gen_ != cur_gen))
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    /* If scan_iter is non-NULL here, ensure_scan_iter() will reuse it. */

    DBUG_RETURN(0);
}

/*
  Lazily create the scan iterator from scan_cf_ when first needed.
  Returns 0 on success or a handler error code.
*/
int ha_tidesdb::ensure_scan_iter()
{
    if (scan_iter) return 0;

    /* If a prior attempt with this exact (scan_cf_, scan_txn) combination
       already failed, short-circuit instead of re-logging and re-failing.
       The cache is invalidated whenever the caller changes scan_cf_ or
       scan_txn (natural since those moves imply a new attempt). */
    if (scan_iter_last_err_ && scan_iter_last_err_cf_ == scan_cf_ &&
        scan_iter_last_err_txn_ == scan_txn)
        return scan_iter_last_err_;

    if (!scan_txn || !scan_cf_)
    {
        /* Invariant violation -- a scan was kicked off without an open txn
         * or column family. Programmer error, not a runtime failure;
         * HA_ERR_INTERNAL_ERROR makes that distinction visible upstream. */
        sql_print_error("[TIDESDB] ensure_scan_iter: no txn or CF");
        scan_iter_last_err_ = HA_ERR_INTERNAL_ERROR;
        scan_iter_last_err_cf_ = scan_cf_;
        scan_iter_last_err_txn_ = scan_txn;
        return HA_ERR_INTERNAL_ERROR;
    }
    int rc = tidesdb_iter_new(scan_txn, scan_cf_, &scan_iter);
    if (rc == TDB_SUCCESS)
    {
        scan_iter_cf_ = scan_cf_;
        scan_iter_txn_ = scan_txn;
        scan_iter_txn_gen_ = cached_trx_ ? cached_trx_->txn_generation : 0;
        scan_iter_last_err_ = 0;
        return 0;
    }
    int herr = tdb_rc_to_ha(rc, "ensure_scan_iter");
    scan_iter_last_err_ = herr;
    scan_iter_last_err_cf_ = scan_cf_;
    scan_iter_last_err_txn_ = scan_txn;
    return herr;
}

int ha_tidesdb::index_end()
{
    DBUG_ENTER("ha_tidesdb::index_end");

    scan_txn = NULL;
    active_index = MAX_KEY;
    spatial_scan_active_ = false;

    DBUG_RETURN(0);
}

int ha_tidesdb::index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                               enum ha_rkey_function find_flag)
{
    DBUG_ENTER("ha_tidesdb::index_read_map");

    /* key_copy_to_comparable uses key_restore + make_comparable_key,
       which reads fields via make_sort_key_part. */
    my_bitmap_map *old_map = tmp_use_all_columns(table, table->read_set);

    uint key_len = calculate_key_len(table, active_index, keypart_map);

    /* We convert the key_copy-format search key to our comparable format */
    KEY *ki = &table->key_info[active_index];
    uchar comp_key[MAX_KEY_LENGTH];
    uint comp_len = key_copy_to_comparable(ki, key, key_len, comp_key);

    tmp_restore_column_map(table->read_set, old_map);

    memcpy(idx_search_comp_, comp_key, comp_len);
    idx_search_comp_len_ = comp_len;

    if (is_pk_)
    {
        /* We build the full data key -- KEY_NS_DATA + comparable_pk_bytes */
        uchar seek_key[DATA_KEY_BUF_LEN];
        uint seek_len = build_data_key(comp_key, comp_len, seek_key);

        if (find_flag == HA_READ_KEY_EXACT)
        {
            uint full_pk_comp_len = share->idx_comp_key_len[share->pk_index];
            if (comp_len >= full_pk_comp_len)
            {
                /* We acquire pessimistic row lock when pessimistic_locking=ON
                   and this is a write-intent read (UPDATE, DELETE, or
                   SELECT ... FOR UPDATE).  Autocommit statements must also
                   participate so they block on locks held by multi-statement
                   transactions -- otherwise an autocommit UPDATE silently
                   bypasses a FOR UPDATE lock and causes commit conflicts. */
                if (unlikely(srv_pessimistic_locking) && stmt_has_write_lock_)
                {
                    tidesdb_trx_t *trx = cached_trx_;
                    if (trx)
                    {
                        int lrc = row_lock_acquire(trx, comp_key, comp_len, cached_thd_);
                        if (lrc) DBUG_RETURN(lrc);
                    }
                }

                /* Full PK match, point lookup only, no iterator needed.
                   If index_next is called later, ensure_scan_iter will create it. */
                int ret = fetch_row_by_pk(scan_txn, comp_key, comp_len, buf);
                if (ret == 0) idx_pk_exact_done_ = true;
                DBUG_RETURN(ret);
            }

            /* Partial PK prefix (e.g. first column of composite PK).
               We need an iterator-based prefix scan -- seek to the first
               matching data key and let index_next_same iterate through
               all entries sharing this prefix. */
            {
                int irc = ensure_scan_iter();
                if (irc) DBUG_RETURN(irc);
            }
            tidesdb_iter_seek(scan_iter, seek_key, seek_len);
            int ret = iter_read_current(buf);
            if (ret == 0) scan_dir_ = DIR_FORWARD;
            DBUG_RETURN(ret);
        }

        /* All other PK scan modes need the iterator */
        {
            int irc = ensure_scan_iter();
            if (irc) DBUG_RETURN(irc);
        }

        if (find_flag == HA_READ_KEY_OR_NEXT || find_flag == HA_READ_AFTER_KEY)
        {
            tidesdb_iter_seek(scan_iter, seek_key, seek_len);

            if (find_flag == HA_READ_AFTER_KEY && tidesdb_iter_valid(scan_iter))
            {
                /* We skip exact match if present */
                uint8_t *ik = NULL;
                size_t iks = 0;
                if (tidesdb_iter_key(scan_iter, &ik, &iks) == TDB_SUCCESS && iks == seek_len &&
                    memcmp(ik, seek_key, iks) == 0)
                    tidesdb_iter_next(scan_iter);
            }

            int ret = iter_read_current(buf);
            if (ret == 0) scan_dir_ = DIR_FORWARD;
            DBUG_RETURN(ret);
        }
        else if (find_flag == HA_READ_KEY_OR_PREV || find_flag == HA_READ_BEFORE_KEY ||
                 find_flag == HA_READ_PREFIX_LAST || find_flag == HA_READ_PREFIX_LAST_OR_PREV)
        {
            tidesdb_iter_seek_for_prev(scan_iter, seek_key, seek_len);
            if (find_flag == HA_READ_BEFORE_KEY && tidesdb_iter_valid(scan_iter))
            {
                uint8_t *ik = NULL;
                size_t iks = 0;
                if (tidesdb_iter_key(scan_iter, &ik, &iks) == TDB_SUCCESS && iks == seek_len &&
                    memcmp(ik, seek_key, iks) == 0)
                    tidesdb_iter_prev(scan_iter);
            }

            int ret = iter_read_current(buf);
            if (ret == 0) scan_dir_ = DIR_BACKWARD;
            DBUG_RETURN(ret);
        }

        /* Fallback is to seek forward */
        tidesdb_iter_seek(scan_iter, seek_key, seek_len);
        int ret = iter_read_current(buf);
        if (ret == 0) scan_dir_ = DIR_FORWARD;
        DBUG_RETURN(ret);
    }
    else
    {
        /* -- Spatial index MBR query, hilbert range scan with MBR post-filter */
        if (is_spatial_index(&table->key_info[active_index]) && find_flag >= HA_READ_MBR_CONTAIN &&
            find_flag <= HA_READ_MBR_EQUAL)
        {
            tdb_mbr_t qmbr;
            spatial_parse_query_mbr(key, &qmbr);
            spatial_qmbr_[MBR_XMIN_IDX] = qmbr.xmin;
            spatial_qmbr_[MBR_YMIN_IDX] = qmbr.ymin;
            spatial_qmbr_[MBR_XMAX_IDX] = qmbr.xmax;
            spatial_qmbr_[MBR_YMAX_IDX] = qmbr.ymax;
            spatial_mode_ = find_flag;

            spatial_scan_active_ = true;

            int irc = ensure_scan_iter();
            if (irc) DBUG_RETURN(irc);

            /* We decompose the query box into hilbert curve ranges.
               For DISJOINT, we must scan everything (disjoint entries
               can be anywhere on the curve). For other predicates,
               we compute a tight set of ranges covering only the cells
               that overlap the query box. */
            if (find_flag == HA_READ_MBR_DISJOINT)
            {
                spatial_ranges_.clear();
                spatial_ranges_.push_back({HILBERT_RANGE_FULL_LO, HILBERT_RANGE_FULL_HI});
            }
            else
            {
                uint32_t qx0 = double_to_lex_uint32(qmbr.xmin);
                uint32_t qy0 = double_to_lex_uint32(qmbr.ymin);
                uint32_t qx1 = double_to_lex_uint32(qmbr.xmax);
                uint32_t qy1 = double_to_lex_uint32(qmbr.ymax);
                spatial_decompose_ranges(qx0, qy0, qx1, qy1, spatial_ranges_);
            }
            spatial_range_idx_ = 0;

            /* We seek to the start of the first range */
            if (!spatial_ranges_.empty())
            {
                uchar seek_key[SPATIAL_HILBERT_KEY_LEN];
                encode_hilbert_be(spatial_ranges_[0].first, seek_key);
                tidesdb_iter_seek(scan_iter, seek_key, SPATIAL_HILBERT_KEY_LEN);
            }

            DBUG_RETURN(spatial_scan_next(buf));
        }

        /* Secondary index read, needs an iterator */
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);

        if (find_flag == HA_READ_KEY_EXACT || find_flag == HA_READ_KEY_OR_NEXT)
        {
            tidesdb_iter_seek(scan_iter, comp_key, comp_len);
        }
        else if (find_flag == HA_READ_AFTER_KEY)
        {
            /* We seek, then skip past any exact prefix matches */
            tidesdb_iter_seek(scan_iter, comp_key, comp_len);
            while (tidesdb_iter_valid(scan_iter))
            {
                uint8_t *ik = NULL;
                size_t iks = 0;
                if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) break;
                if (iks < comp_len || memcmp(ik, comp_key, comp_len) != 0) break;
                tidesdb_iter_next(scan_iter);
            }
        }
        else if (find_flag == HA_READ_KEY_OR_PREV || find_flag == HA_READ_BEFORE_KEY ||
                 find_flag == HA_READ_PREFIX_LAST || find_flag == HA_READ_PREFIX_LAST_OR_PREV)
        {
            /* We build upper bound, comp_key with all 0xFF appended for pk portion */
            uchar upper[SEC_IDX_KEY_BUF_LEN];
            memcpy(upper, comp_key, comp_len);
            memset(upper + comp_len, KEY_INF_HI_BYTE, share->pk_key_len);
            uint upper_len = comp_len + share->pk_key_len;
            tidesdb_iter_seek_for_prev(scan_iter, upper, upper_len);
        }
        else
        {
            tidesdb_iter_seek(scan_iter, comp_key, comp_len);
        }

        /* We read the current entry from the secondary index.
           ICP loop, we evaluate pushed index condition before the expensive
           PK point-lookup.  Entries that fail the condition are skipped
           without touching the data CF (same pattern as InnoDB). */
        bool is_backward =
            (find_flag == HA_READ_KEY_OR_PREV || find_flag == HA_READ_BEFORE_KEY ||
             find_flag == HA_READ_PREFIX_LAST || find_flag == HA_READ_PREFIX_LAST_OR_PREV);

        uint idx_col_len = share->idx_comp_key_len[active_index];

        for (;;)
        {
            if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);

            uint8_t *ik = NULL;
            size_t iks = 0;
            if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS)
                DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);

            /* For EXACT match, we verify the index prefix matches */
            if (find_flag == HA_READ_KEY_EXACT)
            {
                if (iks < comp_len || memcmp(ik, comp_key, comp_len) != 0)
                    DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
            }

            if (iks <= idx_col_len) DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);

            /* ICP -- we evaluate pushed condition on index columns before PK lookup */
            check_result_t icp = icp_check_secondary(ik, iks, active_index, buf);
            if (icp == CHECK_NEG)
            {
                if (is_backward)
                    tidesdb_iter_prev(scan_iter);
                else
                    tidesdb_iter_next(scan_iter);
                continue; /* skip this entry */
            }
            if (icp == CHECK_OUT_OF_RANGE) DBUG_RETURN(HA_ERR_END_OF_FILE);
            if (icp == CHECK_ABORTED_BY_USER) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

            /* CHECK_POS -- condition satisfied (or ICP not applicable) */
            int ret;
            if (keyread_only_ && try_keyread_from_index(ik, iks, active_index, buf))
                ret = 0;
            else
                ret = fetch_row_by_pk(scan_txn, ik + idx_col_len, (uint)(iks - idx_col_len), buf);
            if (ret == 0)
            {
                scan_dir_ = is_backward ? DIR_BACKWARD : DIR_FORWARD;
            }
            DBUG_RETURN(ret);
        }
    }
}

int ha_tidesdb::index_next(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::index_next");

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    /* Spatial idx continuation */
    if (spatial_scan_active_)
    {
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);
        if (scan_dir_ != DIR_NONE) tidesdb_iter_next(scan_iter);
        DBUG_RETURN(spatial_scan_next(buf));
    }

    if (idx_pk_exact_done_)
    {
        idx_pk_exact_done_ = false;
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);
        uchar seek_key[DATA_KEY_BUF_LEN];
        uint seek_len = build_data_key(current_pk_buf_, current_pk_len_, seek_key);
        tidesdb_iter_seek(scan_iter, seek_key, seek_len);
        if (tidesdb_iter_valid(scan_iter)) tidesdb_iter_next(scan_iter);
        /* iterator is now past the PK exact match -- advance+read below */
    }
    else
    {
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);
        /* We advance past the last-read entry (iterator stays at current
         * with no pre-advance).  On the first call after index_first
         * sets DIR_NONE, the iterator is already at the correct position
         * so we must not advance. */
        if (scan_dir_ != DIR_NONE) tidesdb_iter_next(scan_iter);
    }

    if (is_pk_)
    {
        int ret = iter_read_current(buf);
        scan_dir_ = DIR_FORWARD;
        DBUG_RETURN(ret);
    }
    else
    {
        /* Secondary index -- ICP loop -- we skip entries that fail the pushed
           condition without the expensive PK point-lookup. */
        uint idx_key_len = share->idx_comp_key_len[active_index];
        for (;;)
        {
            if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

            uint8_t *ik = NULL;
            size_t iks = 0;
            if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS)
                DBUG_RETURN(HA_ERR_END_OF_FILE);

            if (iks <= idx_key_len) DBUG_RETURN(HA_ERR_END_OF_FILE);

            /* ICP -- we evaluate pushed condition before PK lookup */
            check_result_t icp = icp_check_secondary(ik, iks, active_index, buf);
            if (icp == CHECK_NEG)
            {
                tidesdb_iter_next(scan_iter);
                continue;
            }
            if (icp == CHECK_OUT_OF_RANGE) DBUG_RETURN(HA_ERR_END_OF_FILE);
            if (icp == CHECK_ABORTED_BY_USER) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

            int ret;
            if (keyread_only_ && try_keyread_from_index(ik, iks, active_index, buf))
                ret = 0;
            else
                ret = fetch_row_by_pk(scan_txn, ik + idx_key_len, (uint)(iks - idx_key_len), buf);
            scan_dir_ = DIR_FORWARD;
            DBUG_RETURN(ret);
        }
    }
}

int ha_tidesdb::index_prev(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::index_prev");

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    /* If PK exact match was done without iterator, create it now and
       seek to the matched key so that prev() steps before it. */
    if (idx_pk_exact_done_)
    {
        idx_pk_exact_done_ = false;
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);
        uchar seek_key[DATA_KEY_BUF_LEN];
        uint seek_len = build_data_key(current_pk_buf_, current_pk_len_, seek_key);
        tidesdb_iter_seek(scan_iter, seek_key, seek_len);
        /* iterator is at the matched key -- fall through to prev() */
    }
    else
    {
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);
    }

    /* We advance backward past the last-read entry */
    tidesdb_iter_prev(scan_iter);

    if (is_pk_)
    {
        /* We skip meta keys going backwards */
        while (tidesdb_iter_valid(scan_iter))
        {
            uint8_t *key = NULL;
            size_t ks = 0;
            if (tidesdb_iter_key(scan_iter, &key, &ks) != TDB_SUCCESS)
                DBUG_RETURN(HA_ERR_END_OF_FILE);
            if (is_data_key(key, ks)) break;
            tidesdb_iter_prev(scan_iter);
        }
        scan_dir_ = DIR_BACKWARD;
        DBUG_RETURN(iter_read_current(buf));
    }
    else
    {
        /* Secondary index -- ICP loop (backward direction) */
        uint idx_key_len = share->idx_comp_key_len[active_index];
        for (;;)
        {
            if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

            uint8_t *ik = NULL;
            size_t iks = 0;
            if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS)
                DBUG_RETURN(HA_ERR_END_OF_FILE);

            if (iks <= idx_key_len) DBUG_RETURN(HA_ERR_END_OF_FILE);

            /* ICP -- we evaluate pushed condition before PK lookup */
            check_result_t icp = icp_check_secondary(ik, iks, active_index, buf);
            if (icp == CHECK_NEG)
            {
                tidesdb_iter_prev(scan_iter);
                continue;
            }
            if (icp == CHECK_OUT_OF_RANGE) DBUG_RETURN(HA_ERR_END_OF_FILE);
            if (icp == CHECK_ABORTED_BY_USER) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

            scan_dir_ = DIR_BACKWARD;
            int ret;
            if (keyread_only_ && try_keyread_from_index(ik, iks, active_index, buf))
                ret = 0;
            else
                ret = fetch_row_by_pk(scan_txn, ik + idx_key_len, (uint)(iks - idx_key_len), buf);
            DBUG_RETURN(ret);
        }
    }
}

int ha_tidesdb::index_first(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::index_first");

    idx_pk_exact_done_ = false;
    int irc = ensure_scan_iter();
    if (irc) DBUG_RETURN(irc);

    if (is_pk_)
    {
        /* We seek to first data key */
        uint8_t data_prefix = KEY_NS_DATA;
        tidesdb_iter_seek(scan_iter, &data_prefix, 1);
        int ret = iter_read_current(buf);
        if (ret == 0) scan_dir_ = DIR_FORWARD;
        DBUG_RETURN(ret);
    }
    else
    {
        tidesdb_iter_seek_to_first(scan_iter);
        scan_dir_ = DIR_NONE; /* index_next will set DIR_FORWARD */
        DBUG_RETURN(index_next(buf));
    }
}

int ha_tidesdb::index_last(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::index_last");

    idx_pk_exact_done_ = false;
    int irc = ensure_scan_iter();
    if (irc) DBUG_RETURN(irc);

    if (is_pk_)
    {
        tidesdb_iter_seek_to_last(scan_iter);
        /* The last key might be a data key already, but skip backwards
           past any non-data keys just in case. */
        while (tidesdb_iter_valid(scan_iter))
        {
            uint8_t *key = NULL;
            size_t ks = 0;
            if (tidesdb_iter_key(scan_iter, &key, &ks) != TDB_SUCCESS)
                DBUG_RETURN(HA_ERR_END_OF_FILE);
            if (is_data_key(key, ks)) break;
            tidesdb_iter_prev(scan_iter);
        }
        scan_dir_ = DIR_BACKWARD;
        DBUG_RETURN(iter_read_current(buf));
    }
    else
    {
        tidesdb_iter_seek_to_last(scan_iter);
        if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

        uint8_t *ik = NULL;
        size_t iks = 0;
        if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) DBUG_RETURN(HA_ERR_END_OF_FILE);

        uint idx_key_len = share->idx_comp_key_len[active_index];
        if (iks <= idx_key_len) DBUG_RETURN(HA_ERR_END_OF_FILE);

        scan_dir_ = DIR_BACKWARD;
        DBUG_RETURN(fetch_row_by_pk(scan_txn, ik + idx_key_len, (uint)(iks - idx_key_len), buf));
    }
}

int ha_tidesdb::index_next_same(uchar *buf, const uchar *key, uint keylen)
{
    DBUG_ENTER("ha_tidesdb::index_next_same");

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    /* Spatial index continuation */
    if (spatial_scan_active_)
    {
        if (!scan_iter) DBUG_RETURN(HA_ERR_END_OF_FILE);
        tidesdb_iter_next(scan_iter);
        DBUG_RETURN(spatial_scan_next(buf));
    }

    if (is_pk_)
    {
        uint full_pk_comp_len = share->idx_comp_key_len[share->pk_index];
        if (idx_search_comp_len_ >= full_pk_comp_len)
        {
            /* Full PK is unique -- after the first match there are no more */
            DBUG_RETURN(HA_ERR_END_OF_FILE);
        }

        /* Partial PK prefix on a composite PK -- iterate through data keys
           that share this prefix-- KEY_NS_DATA + comparable_pk_prefix... */
        if (!scan_iter) DBUG_RETURN(HA_ERR_END_OF_FILE);

        /* We advance past the last-read entry */
        tidesdb_iter_next(scan_iter);
        if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

        uint8_t *ik = NULL;
        size_t iks = 0;
        if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) DBUG_RETURN(HA_ERR_END_OF_FILE);

        /* Data key format-- KEY_NS_DATA + comparable_pk.
           We check if the PK prefix still matches (skip the namespace byte). */
        if (iks < KEY_NAMESPACE_LEN + idx_search_comp_len_ ||
            memcmp(ik + KEY_NAMESPACE_LEN, idx_search_comp_, idx_search_comp_len_) != 0)
            DBUG_RETURN(HA_ERR_END_OF_FILE);

        int ret = iter_read_current(buf);
        if (ret == 0) scan_dir_ = DIR_FORWARD;
        DBUG_RETURN(ret);
    }

    /* Secondary index -- we advance past the last-read entry, then ICP loop */
    if (!scan_iter) DBUG_RETURN(HA_ERR_END_OF_FILE);
    tidesdb_iter_next(scan_iter);

    uint idx_col_len = share->idx_comp_key_len[active_index];
    for (;;)
    {
        if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

        uint8_t *ik = NULL;
        size_t iks = 0;
        if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) DBUG_RETURN(HA_ERR_END_OF_FILE);

        if (iks < idx_search_comp_len_ || memcmp(ik, idx_search_comp_, idx_search_comp_len_) != 0)
        {
            DBUG_RETURN(HA_ERR_END_OF_FILE);
        }

        if (iks <= idx_col_len) DBUG_RETURN(HA_ERR_END_OF_FILE);

        /* ICP -- we evaluate pushed condition before PK lookup */
        check_result_t icp = icp_check_secondary(ik, iks, active_index, buf);
        if (icp == CHECK_NEG)
        {
            tidesdb_iter_next(scan_iter);
            continue;
        }
        if (icp == CHECK_OUT_OF_RANGE) DBUG_RETURN(HA_ERR_END_OF_FILE);
        if (icp == CHECK_ABORTED_BY_USER) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

        int ret;
        if (keyread_only_ && try_keyread_from_index(ik, iks, active_index, buf))
            ret = 0;
        else
            ret = fetch_row_by_pk(scan_txn, ik + idx_col_len, (uint)(iks - idx_col_len), buf);
        DBUG_RETURN(ret);
    }
}

/* ******************** update_row (UPDATE) ******************** */

int ha_tidesdb::update_row(const uchar *old_data, uchar *new_data)
{
    DBUG_ENTER("ha_tidesdb::update_row");

    my_bitmap_map *old_map = tmp_use_all_columns(table, table->read_set);

    /* We cache THD and trx once to avoid repeated ha_thd() virtual calls
       and thd_get_ha_data() indirect lookups throughout this function.
       We use cached_thd_/cached_trx_ set in external_lock to avoid
       per-row ha_thd() virtual dispatch and thd_get_ha_data() hash lookup. */
    tidesdb_trx_t *trx = cached_trx_;

    /* We use handler-owned pk buffer for old/new PK to avoid large stack arrays.
       old_pk is saved from current_pk_buf_ before we overwrite it. */
    uchar old_pk[MAX_KEY_LENGTH];
    uint old_pk_len = current_pk_len_;
    memcpy(old_pk, current_pk_buf_, old_pk_len);

    /* Row locks are acquired in index_read_map() during the preceding
       locking read (SELECT ... FOR UPDATE, UPDATE, DELETE all go through
       index_read_map with write intent).  No need to acquire here, the
       row is already locked. */

    /* new_pk uses its own stack buffer so it survives the current_pk_buf_
       manipulations in the secondary index loop (avoids overlapping memcpy UB)

       L-7: skip make_comparable_key when the UPDATE doesn't touch any
       PK column. The common UPDATE shape is `SET non_pk_col = x WHERE pk = y`
       -- write_set has only the non-PK columns. Rebuilding the PK from
       new_data was wasted work in that case; reuse old_pk. */
    uchar new_pk[MAX_KEY_LENGTH];
    uint new_pk_len;
    bool any_pk_col_in_write_set = false;
    if (share->has_user_pk)
    {
        KEY *pk_key = &table->key_info[share->pk_index];
        for (uint p = 0; p < pk_key->user_defined_key_parts; p++)
        {
            Field *f = pk_key->key_part[p].field;
            if (bitmap_is_set(table->write_set, f->field_index()))
            {
                any_pk_col_in_write_set = true;
                break;
            }
        }
    }
    if (any_pk_col_in_write_set)
    {
        new_pk_len = pk_from_record(new_data, new_pk);
    }
    else
    {
        memcpy(new_pk, old_pk, old_pk_len);
        new_pk_len = old_pk_len;
    }

    const std::string &new_row = serialize_row(new_data);
    if (share->encrypted && new_row.empty())
    {
        tmp_restore_column_map(table->read_set, old_map);
        sql_print_error("[TIDESDB] update_row encryption failed");
        DBUG_RETURN(HA_ERR_GENERIC);
    }
    const uint8_t *row_ptr = (const uint8_t *)new_row.data();
    size_t row_len = new_row.size();

    {
        int erc = ensure_stmt_txn();
        if (erc)
        {
            tmp_restore_column_map(table->read_set, old_map);
            DBUG_RETURN(erc);
        }
    }
    tidesdb_txn_t *txn = stmt_txn;
    stmt_txn_dirty = true;
    if (trx)
    {
        trx->dirty = true;
        trx->stmt_was_dirty = true;
    }

    /* We populate THDVAR cache if not yet done this statement */
    if (!cached_thdvars_valid_)
    {
        cached_skip_unique_ = THDVAR(cached_thd_, skip_unique_check);
        cached_sess_ttl_ = THDVAR(cached_thd_, ttl);
        cached_single_delete_primary_ = THDVAR(cached_thd_, single_delete_primary);
        cached_thdvars_valid_ = true;
    }

    int rc;
    bool pk_changed = (old_pk_len != new_pk_len || memcmp(old_pk, new_pk, old_pk_len) != 0);

    /* We compute TTL when the table has TTL configured or the session overrides it.
       Uses cached_sess_ttl_ to avoid THDVAR + ha_thd() per row. */
    time_t row_ttl =
        (share->has_ttl || cached_sess_ttl_ > 0) ? compute_row_ttl(new_data) : TIDESDB_TTL_NONE;

    /* If PK changed, we delete old entry and insert new */
    if (pk_changed)
    {
        uchar old_dk[DATA_KEY_BUF_LEN];
        uint old_dk_len = build_data_key(old_pk, old_pk_len, old_dk);
        rc = tidesdb_txn_delete_cf(txn, share->cf, old_dk, old_dk_len,
                                   cached_single_delete_primary_);
        if (rc != TDB_SUCCESS) goto err;
    }

    {
        uchar new_dk[DATA_KEY_BUF_LEN];
        uint new_dk_len = build_data_key(new_pk, new_pk_len, new_dk);
        rc = tidesdb_txn_put(txn, share->cf, new_dk, new_dk_len, row_ptr, row_len, row_ttl);
        if (rc != TDB_SUCCESS) goto err;
    }

    /* We update secondary indexes in a single consolidated dispatch loop.
       Regular, FTS, and spatial indexes are handled inline, eliminating the
       three separate passes over table->s->keys that the old code used.
       Each branch short-circuits via a write_set pre-check so unchanged
       indexes skip both key construction and LSM writes. */
    if (share->num_secondary_indexes > 0)
    {
        /* We use handler-owned buffers to avoid per-row heap allocation
           and keep the stack frame within -Wframe-larger-than limits. */
        uchar *old_ik = upd_old_ik_;
        uchar *new_ik = upd_new_ik_;
        const uint num_keys = table->s->keys;
        const bool has_user_pk = share->has_user_pk;
        const uint pk_index = share->pk_index;
        const size_t idx_cfs_sz = share->idx_cfs.size();

        for (uint i = 0; i < num_keys; i++)
        {
            if (has_user_pk && i == pk_index) continue;
            if (i >= idx_cfs_sz || !share->idx_cfs[i]) continue;

            KEY *ki = &table->key_info[i];

            if (share->idx_is_fts[i])
            {
                /* We skip if no indexed column actually changed */
                bool fts_changed = false;
                for (uint p = 0; p < ki->user_defined_key_parts; p++)
                {
                    uint fieldnr = ki->key_part[p].fieldnr - 1;
                    if (bitmap_is_set(table->write_set, fieldnr))
                    {
                        fts_changed = true;
                        break;
                    }
                }
                if (!fts_changed) continue;

                CHARSET_INFO *fts_cs = ki->key_part[0].field->charset();

                /* Tokenize both old and new docs, build term-frequency maps,
                   then emit only the minimum set of deletes/puts needed.
                   For a small edit to a large document this avoids rewriting
                   every term entry.

                   See write_row's FTS comment for why this is a
                   thread_local POINTER, not a thread_local object. */
                struct UpdateRowFtsScratch {
                    std::vector<fts_token_t> old_tokens;
                    std::vector<fts_token_t> new_tokens;
                    std::unordered_map<std::string, uint16> old_tf;
                    std::unordered_map<std::string, uint16> new_tf;
                };
                /* MF-8: std::unique_ptr cleans up at thread exit. */
                static thread_local std::unique_ptr<UpdateRowFtsScratch> upd_fts_scratch;
                if (!upd_fts_scratch) upd_fts_scratch = std::make_unique<UpdateRowFtsScratch>();
                auto &old_tokens = upd_fts_scratch->old_tokens;
                auto &new_tokens = upd_fts_scratch->new_tokens;
                auto &old_tf = upd_fts_scratch->old_tf;
                auto &new_tf = upd_fts_scratch->new_tf;
                old_tokens.clear();
                new_tokens.clear();
                old_tf.clear();
                new_tf.clear();
                fts_extract_and_tokenize(table, ki, old_data, fts_cs, old_tokens);
                fts_extract_and_tokenize(table, ki, new_data, fts_cs, new_tokens);

                for (auto &tok : old_tokens) old_tf[tok.word]++;
                for (auto &tok : new_tokens) new_tf[tok.word]++;
                uint32 old_wc = (uint32)old_tokens.size();
                uint32 new_wc = (uint32)new_tokens.size();

                if (pk_changed)
                {
                    /* PK changed -- the row identity changed so every old
                       (term, old_pk) must be deleted and every new (term, new_pk)
                       inserted.  No diffing possible across different PKs. */
                    for (auto &[term, tf] : old_tf)
                    {
                        uchar fk[FTS_KEY_BUF_LEN];
                        uint fk_len =
                            fts_build_key(term.data(), (uint)term.size(), old_pk, old_pk_len, fk);
                        tidesdb_txn_delete_cf(txn, share->idx_cfs[i], fk, fk_len, true);
                    }
                    for (auto &[term, tf] : new_tf)
                    {
                        uchar fk[FTS_KEY_BUF_LEN];
                        uint fk_len =
                            fts_build_key(term.data(), (uint)term.size(), new_pk, new_pk_len, fk);
                        uchar fv[FTS_VALUE_LEN];
                        fts_build_value(tf, new_wc, fv);
                        rc = tidesdb_txn_put(txn, share->idx_cfs[i], fk, fk_len, fv, FTS_VALUE_LEN,
                                             row_ttl);
                        if (rc != TDB_SUCCESS) goto err;
                    }
                }
                else
                {
                    /* PK stable -- apply term-level diff.  Only delete a term
                       when it disappears and only write a term when it is new,
                       its tf changes, or doc_len changes (doc_len is part of
                       the stored value used by BM25). */
                    bool doc_len_changed = (old_wc != new_wc);

                    for (auto &[term, old_cnt] : old_tf)
                    {
                        if (new_tf.find(term) != new_tf.end()) continue;
                        uchar fk[FTS_KEY_BUF_LEN];
                        uint fk_len =
                            fts_build_key(term.data(), (uint)term.size(), old_pk, old_pk_len, fk);
                        tidesdb_txn_delete_cf(txn, share->idx_cfs[i], fk, fk_len, true);
                    }

                    for (auto &[term, new_cnt] : new_tf)
                    {
                        auto it = old_tf.find(term);
                        bool need_put;
                        if (it == old_tf.end())
                            need_put = true;
                        else if (doc_len_changed)
                            need_put = true;
                        else
                            need_put = (it->second != new_cnt);

                        if (!need_put) continue;

                        uchar fk[FTS_KEY_BUF_LEN];
                        uint fk_len =
                            fts_build_key(term.data(), (uint)term.size(), new_pk, new_pk_len, fk);
                        uchar fv[FTS_VALUE_LEN];
                        fts_build_value(new_cnt, new_wc, fv);
                        rc = tidesdb_txn_put(txn, share->idx_cfs[i], fk, fk_len, fv, FTS_VALUE_LEN,
                                             row_ttl);
                        if (rc != TDB_SUCCESS) goto err;
                    }
                }

                /* Meta same doc stays, so delta_docs=0.  Word count shifts by
                   new_wc - old_wc; only write the meta row when it actually
                   moved to avoid a pointless read-modify-write. */
                int64_t wc_delta = (int64_t)new_wc - (int64_t)old_wc;
                if (wc_delta != 0) fts_update_meta(share, txn, share->cf, i, 0, wc_delta);
            }
            else if (share->idx_is_spatial[i])
            {
                /* Spatial we skip if geometry column unchanged */
                uint fieldnr = ki->key_part[0].fieldnr - 1;
                if (!bitmap_is_set(table->write_set, fieldnr)) continue;

                Field *geom_field = ki->key_part[0].field;

                /* Delete old spatial entry */
                {
                    my_ptrdiff_t ptd = (my_ptrdiff_t)(old_data - table->record[0]);
                    geom_field->move_field_offset(ptd);
                    String gs;
                    geom_field->val_str(&gs, &gs);
                    geom_field->move_field_offset(-ptd);
                    double xmn, ymn, xmx, ymx;
                    if (gs.length() > 0 && spatial_compute_mbr((const uchar *)gs.ptr(), gs.length(),
                                                               &xmn, &ymn, &xmx, &ymx))
                    {
                        uchar sk[SPATIAL_HILBERT_KEY_LEN + MAX_KEY_LENGTH];
                        uint sk_len = spatial_build_key((xmn + xmx) / MBR_CENTROID_DIV,
                                                        (ymn + ymx) / MBR_CENTROID_DIV, old_pk,
                                                        old_pk_len, sk);
                        tidesdb_txn_delete_cf(txn, share->idx_cfs[i], sk, sk_len, true);
                    }
                }

                /* Insert new spatial entry */
                {
                    my_ptrdiff_t ptd = (my_ptrdiff_t)(new_data - table->record[0]);
                    geom_field->move_field_offset(ptd);
                    String gs;
                    geom_field->val_str(&gs, &gs);
                    geom_field->move_field_offset(-ptd);
                    double xmn, ymn, xmx, ymx;
                    if (gs.length() > 0 && spatial_compute_mbr((const uchar *)gs.ptr(), gs.length(),
                                                               &xmn, &ymn, &xmx, &ymx))
                    {
                        uchar sk[SPATIAL_HILBERT_KEY_LEN + MAX_KEY_LENGTH];
                        uint sk_len = spatial_build_key((xmn + xmx) / MBR_CENTROID_DIV,
                                                        (ymn + ymx) / MBR_CENTROID_DIV, new_pk,
                                                        new_pk_len, sk);
                        uchar sv[SPATIAL_MBR_VALUE_LEN];
                        spatial_build_value(xmn, ymn, xmx, ymx, sv);
                        rc = tidesdb_txn_put(txn, share->idx_cfs[i], sk, sk_len, sv,
                                             SPATIAL_MBR_VALUE_LEN, row_ttl);
                        if (rc != TDB_SUCCESS) goto err;
                    }
                }
            }
            else
            {
                /* Regular secondary index we skip before building keys when no
                   indexed column changed and the PK is stable.  Saves the
                   per-row make_comparable_key/sec_idx_key cost on wide updates
                   that touch only unrelated columns. */
                if (!pk_changed)
                {
                    bool idx_changed = false;
                    for (uint p = 0; p < ki->user_defined_key_parts; p++)
                    {
                        uint fieldnr = ki->key_part[p].fieldnr - 1;
                        if (bitmap_is_set(table->write_set, fieldnr))
                        {
                            idx_changed = true;
                            break;
                        }
                    }
                    if (!idx_changed) continue;
                }

                /* Build old index entry key.  current_pk_buf_ is transiently
                   set to old/new PK so sec_idx_key's pk_from_record path works
                   for hidden-PK tables. */
                memcpy(current_pk_buf_, old_pk, old_pk_len);
                current_pk_len_ = old_pk_len;
                uint old_ik_len =
                    make_comparable_key(ki, old_data, ki->user_defined_key_parts, old_ik);
                memcpy(old_ik + old_ik_len, old_pk, old_pk_len);
                old_ik_len += old_pk_len;

                /* Build new index entry key */
                memcpy(current_pk_buf_, new_pk, new_pk_len);
                current_pk_len_ = new_pk_len;
                uint new_ik_len = sec_idx_key(i, new_data, new_ik);

                /* Skip if the final index bytes match (e.g. UPDATE x=x). */
                if (old_ik_len == new_ik_len && memcmp(old_ik, new_ik, old_ik_len) == 0) continue;

                rc = tidesdb_txn_delete_cf(txn, share->idx_cfs[i], old_ik, old_ik_len, true);
                if (rc != TDB_SUCCESS) goto err;
                rc = tidesdb_txn_put(txn, share->idx_cfs[i], new_ik, new_ik_len, &tdb_empty_val,
                                     sizeof(tdb_empty_val), row_ttl);
                if (rc != TDB_SUCCESS) goto err;
            }
        }
    }

    memcpy(current_pk_buf_, new_pk, new_pk_len);
    current_pk_len_ = new_pk_len;

    /* Bulk UPDATE mid-txn commit.  Symmetric to write_row's bulk path.
       One UPDATE op counts as 1 data put + 1 data delete (when PK changed)
       + up to num_secondary_indexes entries rewritten.  We overestimate as
       `1 + 2 * num_secondary_indexes` so the threshold fires earlier rather
       than later and the txn never exceeds TDB_MAX_TXN_OPS. */
    if (in_bulk_update_)
    {
        bulk_insert_ops_ += 1 + 2 * (ha_rows)share->num_secondary_indexes;
        if (bulk_insert_ops_ >= TIDESDB_BULK_INSERT_BATCH_OPS)
        {
            int mrc = maybe_bulk_commit(trx);
            if (mrc)
            {
                tmp_restore_column_map(table->read_set, old_map);
                DBUG_RETURN(mrc);
            }
            bulk_insert_ops_ = 0;
        }
    }

    /* Commit happens in external_lock(F_UNLCK). */
    tmp_restore_column_map(table->read_set, old_map);
    DBUG_RETURN(0);

err:
    tmp_restore_column_map(table->read_set, old_map);
    DBUG_RETURN(tdb_rc_to_ha(rc, "update_row"));
}

/* ******************** delete_row (DELETE) ******************** */

int ha_tidesdb::delete_row(const uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::delete_row");

    my_bitmap_map *old_map = tmp_use_all_columns(table, table->read_set);

    /* We use cached_trx_ from external_lock to avoid per-row hash lookups. */
    tidesdb_trx_t *trx = cached_trx_;

    /* Row locks are acquired in index_read_map() during the preceding
       locking read -- see update_row() comment. */

    /* We populate THDVAR cache if not yet done this statement.  A pure DELETE
       reaches delete_row without first going through write_row/update_row, so
       the cache may still be stale from the prior statement. */
    if (!cached_thdvars_valid_)
    {
        cached_skip_unique_ = THDVAR(cached_thd_, skip_unique_check);
        cached_sess_ttl_ = THDVAR(cached_thd_, ttl);
        cached_single_delete_primary_ = THDVAR(cached_thd_, single_delete_primary);
        cached_compact_after_range_delete_min_rows_ =
            THDVAR(cached_thd_, compact_after_range_delete_min_rows);
        cached_thdvars_valid_ = true;
    }

    {
        int erc = ensure_stmt_txn();
        if (erc)
        {
            tmp_restore_column_map(table->read_set, old_map);
            DBUG_RETURN(erc);
        }
    }
    tidesdb_txn_t *txn = stmt_txn;
    stmt_txn_dirty = true;
    if (trx)
    {
        trx->dirty = true;
        trx->stmt_was_dirty = true;
    }

    /* We delete data row */
    uchar dk[DATA_KEY_BUF_LEN];
    uint dk_len = build_data_key(current_pk_buf_, current_pk_len_, dk);

    /* Track the touched data-key range when the auto-compact session var
       is on and we are inside a multi-row DELETE.  We compare the full
       data keys (KEY_NS_DATA + comparable_pk) so the recorded bounds can
       be passed to tidesdb_compact_range without further conversion.

       L-6: fixed-buffer min/max tracking. dk_len is bounded by
       DATA_KEY_BUF_LEN (data key namespace + comparable PK), so the
       fixed buffer covers any legal key. memcmp is zero-alloc. */
    if (in_bulk_delete_ && cached_compact_after_range_delete_min_rows_ > 0 &&
        dk_len <= DATA_KEY_BUF_LEN)
    {
        if (bulk_delete_rows_ == 0)
        {
            memcpy(bulk_delete_min_pk_, dk, dk_len);
            memcpy(bulk_delete_max_pk_, dk, dk_len);
            bulk_delete_min_pk_len_ = dk_len;
            bulk_delete_max_pk_len_ = dk_len;
        }
        else
        {
            int cmin = memcmp(dk, bulk_delete_min_pk_,
                              std::min<uint>(dk_len, bulk_delete_min_pk_len_));
            if (cmin < 0 || (cmin == 0 && dk_len < bulk_delete_min_pk_len_))
            {
                memcpy(bulk_delete_min_pk_, dk, dk_len);
                bulk_delete_min_pk_len_ = dk_len;
            }
            int cmax = memcmp(dk, bulk_delete_max_pk_,
                              std::min<uint>(dk_len, bulk_delete_max_pk_len_));
            if (cmax > 0 || (cmax == 0 && dk_len > bulk_delete_max_pk_len_))
            {
                memcpy(bulk_delete_max_pk_, dk, dk_len);
                bulk_delete_max_pk_len_ = dk_len;
            }
        }
        bulk_delete_rows_++;
    }

    int rc = tidesdb_txn_delete_cf(txn, share->cf, dk, dk_len, cached_single_delete_primary_);
    if (rc != TDB_SUCCESS)
    {
        tmp_restore_column_map(table->read_set, old_map);
        DBUG_RETURN(tdb_rc_to_ha(rc, "delete_row"));
    }

    /* We delete secondary index entries in a single consolidated dispatch loop.
       Regular, FTS, and spatial indexes are handled inline. */
    if (share->num_secondary_indexes > 0)
    {
        const uint num_keys = table->s->keys;
        const bool has_user_pk = share->has_user_pk;
        const uint pk_index = share->pk_index;
        const size_t idx_cfs_sz = share->idx_cfs.size();

        for (uint i = 0; i < num_keys; i++)
        {
            if (has_user_pk && i == pk_index) continue;
            if (i >= idx_cfs_sz || !share->idx_cfs[i]) continue;

            KEY *ki = &table->key_info[i];

            if (share->idx_is_fts[i])
            {
                /* See write_row's FTS comment for why this is a
                   thread_local POINTER, not a thread_local object. */
                CHARSET_INFO *fts_cs = ki->key_part[0].field->charset();
                struct DeleteRowFtsScratch {
                    std::vector<fts_token_t> tokens;
                    std::unordered_map<std::string, uint16> tf_map;
                };
                /* MF-8: std::unique_ptr cleans up at thread exit. */
                static thread_local std::unique_ptr<DeleteRowFtsScratch> del_fts_scratch;
                if (!del_fts_scratch) del_fts_scratch = std::make_unique<DeleteRowFtsScratch>();
                auto &fts_tokens = del_fts_scratch->tokens;
                auto &tf_map = del_fts_scratch->tf_map;
                fts_tokens.clear();
                tf_map.clear();
                fts_extract_and_tokenize(table, ki, buf, fts_cs, fts_tokens);

                for (auto &tok : fts_tokens) tf_map[tok.word]++;
                uint32 word_count = (uint32)fts_tokens.size();

                for (auto &[term, tf] : tf_map)
                {
                    uchar fk[FTS_KEY_BUF_LEN];
                    uint fk_len = fts_build_key(term.data(), (uint)term.size(), current_pk_buf_,
                                                current_pk_len_, fk);
                    tidesdb_txn_delete_cf(txn, share->idx_cfs[i], fk, fk_len, true);
                }

                fts_update_meta(share, txn, share->cf, i, FTS_DOC_DELTA_DEL, -(int64_t)word_count);
            }
            else if (share->idx_is_spatial[i])
            {
                Field *geom_field = ki->key_part[0].field;
                my_ptrdiff_t ptd = (my_ptrdiff_t)(buf - table->record[0]);
                geom_field->move_field_offset(ptd);
                String geom_str;
                geom_field->val_str(&geom_str, &geom_str);
                geom_field->move_field_offset(-ptd);

                double xmin, ymin, xmax, ymax;
                if (geom_str.length() > 0 &&
                    spatial_compute_mbr((const uchar *)geom_str.ptr(), geom_str.length(), &xmin,
                                        &ymin, &xmax, &ymax))
                {
                    double cx = (xmin + xmax) / MBR_CENTROID_DIV;
                    double cy = (ymin + ymax) / MBR_CENTROID_DIV;
                    uchar sk[SPATIAL_HILBERT_KEY_LEN + MAX_KEY_LENGTH];
                    uint sk_len = spatial_build_key(cx, cy, current_pk_buf_, current_pk_len_, sk);
                    tidesdb_txn_delete_cf(txn, share->idx_cfs[i], sk, sk_len, true);
                }
            }
            else
            {
                uchar ik[SEC_IDX_KEY_BUF_LEN];
                uint ik_len = sec_idx_key(i, buf, ik);
                rc = tidesdb_txn_delete_cf(txn, share->idx_cfs[i], ik, ik_len, true);
                if (rc != TDB_SUCCESS)
                {
                    tmp_restore_column_map(table->read_set, old_map);
                    DBUG_RETURN(tdb_rc_to_ha(rc, "delete_row idx"));
                }
            }
        }
    }

    /* Bulk DELETE mid-txn commit-- 1 data delete + num_secondary_indexes
       secondary-index deletes per row. */
    if (in_bulk_delete_)
    {
        bulk_insert_ops_ += 1 + (ha_rows)share->num_secondary_indexes;
        if (bulk_insert_ops_ >= TIDESDB_BULK_INSERT_BATCH_OPS)
        {
            int mrc = maybe_bulk_commit(trx);
            if (mrc)
            {
                tmp_restore_column_map(table->read_set, old_map);
                DBUG_RETURN(mrc);
            }
            bulk_insert_ops_ = 0;
        }
    }

    tmp_restore_column_map(table->read_set, old_map);
    DBUG_RETURN(0);
}

/* ******************** delete_all_rows (TRUNCATE) ******************** */

int ha_tidesdb::delete_all_rows(void)
{
    DBUG_ENTER("ha_tidesdb::delete_all_rows");

    /* We free cached iterators before dropping/recreating CFs.
       The iterators hold refs to SSTables in the CFs being dropped. */
    if (scan_iter)
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    free_dup_iter_cache();

    /* We discard the connection txn before drop/recreate.  The txn may have
       buffered INSERT/UPDATE ops from earlier statements; committing them
       after the CF is recreated would re-insert stale data. */
    {
        THD *thd = ha_thd();
        tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, ht);
        if (trx && trx->txn)
        {
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
            trx->txn = NULL;
            trx->dirty = false;
        }
        stmt_txn = NULL;
        stmt_txn_dirty = false;
    }

    tidesdb_column_family_config_t cfg = build_cf_config(TDB_TABLE_OPTIONS(table));

    /* We drop and recreate the main data CF (O(1)) */
    {
        std::string cf_name = share->cf_name;
        int rc = tidesdb_drop_column_family(tdb_get_engine(), cf_name.c_str());
        if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
        {
            sql_print_error("[TIDESDB] truncate: failed to drop CF '%s' (err=%d)", cf_name.c_str(),
                            rc);
            DBUG_RETURN(tdb_rc_to_ha(rc, "truncate drop_cf"));
        }

        rc = tidesdb_create_column_family(tdb_get_engine(), cf_name.c_str(), &cfg);
        if (rc != TDB_SUCCESS)
        {
            sql_print_error("[TIDESDB] truncate: failed to recreate CF '%s' (err=%d)",
                            cf_name.c_str(), rc);
            DBUG_RETURN(tdb_rc_to_ha(rc, "truncate create_cf"));
        }

        share->cf = tidesdb_get_column_family(tdb_get_engine(), cf_name.c_str());
        if (!share->cf)
        {
            sql_print_error("[TIDESDB] truncate: CF '%s' not found after recreate",
                            cf_name.c_str());
            DBUG_RETURN(HA_ERR_GENERIC);
        }
    }

    /* We drop and recreate each secondary index CF, preserving per-index USE_BTREE */
    for (uint i = 0; i < share->idx_cfs.size(); i++)
    {
        if (!share->idx_cfs[i]) continue;

        const std::string &idx_name = share->idx_cf_names[i];
        tidesdb_drop_column_family(tdb_get_engine(), idx_name.c_str());

        tidesdb_column_family_config_t idx_cfg = cfg;
        if (i < table->s->keys && TDB_INDEX_OPTIONS(&table->key_info[i]))
        {
            ha_index_option_struct *iopts = TDB_INDEX_OPTIONS(&table->key_info[i]);
            idx_cfg.use_btree = iopts->use_btree ? 1 : 0;
        }

        int rc = tidesdb_create_column_family(tdb_get_engine(), idx_name.c_str(), &idx_cfg);
        if (rc != TDB_SUCCESS)
        {
            sql_print_warning("[TIDESDB] truncate: failed to recreate idx CF '%s' (err=%d)",
                              idx_name.c_str(), rc);
            share->idx_cfs[i] = NULL;
            continue;
        }

        share->idx_cfs[i] = tidesdb_get_column_family(tdb_get_engine(), idx_name.c_str());
    }

    share->next_row_id.store(HIDDEN_PK_FIRST_ROW_ID, std::memory_order_relaxed);

    DBUG_RETURN(0);
}

/*
  TRUNCATE TABLE entry point.

  MySQL 9.7 routes TRUNCATE through handler::truncate(dd::Table*); the default
  returns HA_ERR_WRONG_COMMAND, which surfaces as ER_ILLEGAL_HA. We already do
  the right work in delete_all_rows() (drop+recreate every CF, reset hidden-PK
  counter, discard txn buffers), so this is a one-line delegate.

  The dd::Table* arg lets atomic-DDL engines mutate the data-dictionary entry;
  TidesDB does not yet rely on the DD for table state, so we ignore it.
*/
int ha_tidesdb::truncate(dd::Table *table_def [[maybe_unused]])
{
    DBUG_ENTER("ha_tidesdb::truncate");
    DBUG_RETURN(delete_all_rows());
}

/* ******************** Bulk DML ******************** */

/*
  Commit the current txn mid-statement and reset it with READ_COMMITTED so
  the next batch starts fresh.  Shared by bulk INSERT/UPDATE/DELETE once
  buffered ops cross TIDESDB_BULK_INSERT_BATCH_OPS -- keeps us under
  TDB_MAX_TXN_OPS and bounds txn memory.  Higher isolation levels would
  cause unbounded read-set growth across batches.

  Any cached iterators and dup-check iterators are invalidated, they hold
  references to MERGE_SOURCE_TXN_OPS that txn_reset clears.
*/
int ha_tidesdb::maybe_bulk_commit(tidesdb_trx_t *trx)
{
    if (!trx || !trx->txn) return 0;

    int crc = tidesdb_txn_commit(trx->txn);
    if (crc != TDB_SUCCESS) sql_print_information("[TIDESDB] bulk mid-commit failed rc=%d", crc);

    int rrc = tidesdb_txn_reset(trx->txn, TDB_ISOLATION_READ_COMMITTED);
    if (rrc != TDB_SUCCESS)
    {
        sql_print_warning(
            "[TIDESDB] bulk tidesdb_txn_reset failed (rc=%d), falling back to "
            "free+begin",
            rrc);
        tidesdb_txn_free(trx->txn);
        trx->txn = NULL;
        int rc =
            tidesdb_txn_begin_with_isolation(tdb_get_engine(), TDB_ISOLATION_READ_COMMITTED, &trx->txn);
        if (rc != TDB_SUCCESS) return tdb_rc_to_ha(rc, "bulk_commit txn_begin");
    }

    stmt_txn = trx->txn;
    trx->txn_generation++;

    if (scan_iter)
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    free_dup_iter_cache();
    scan_txn = trx->txn;
    return 0;
}

void ha_tidesdb::start_bulk_insert(ha_rows rows)
{
    in_bulk_insert_ = true;
    bulk_insert_ops_ = 0;
    /* M-4: reset per-key FTS meta deltas; we'll accumulate while
       in_bulk_insert_ is true and flush in end_bulk_insert. */
    if (table && table->s)
    {
        uint nkeys = table->s->keys;
        for (uint i = 0; i < nkeys && i < MAX_KEY; i++)
        {
            fts_meta_doc_delta_[i] = 0;
            fts_meta_word_delta_[i] = 0;
        }
    }
}

int ha_tidesdb::end_bulk_insert()
{
    in_bulk_insert_ = false;

    /* M-4: flush buffered FTS meta deltas. One RMW per FTS key with
       non-zero delta (vs one RMW per row pre-fix).
       MF-10: capture and propagate the first non-zero rc. We still
       try to flush the rest -- a per-key meta failure shouldn't strand
       a different key's accumulated delta -- but we surface the error
       so the SQL layer rolls back the bulk insert. Without this, a
       meta-flush failure was silently swallowed and the statement
       appeared to succeed. */
    int sticky_rc = 0;
    if (table && table->s && share && stmt_txn)
    {
        uint nkeys = table->s->keys;
        for (uint i = 0; i < nkeys && i < MAX_KEY; i++)
        {
            if (fts_meta_doc_delta_[i] == 0 && fts_meta_word_delta_[i] == 0)
                continue;
            if (!share->idx_is_fts[i]) continue;
            int rc = fts_update_meta(share, stmt_txn, share->cf, i,
                                     fts_meta_doc_delta_[i],
                                     fts_meta_word_delta_[i]);
            if (rc != 0 && sticky_rc == 0)
            {
                sticky_rc = tdb_rc_to_ha(rc, "end_bulk_insert FTS meta flush");
            }
            fts_meta_doc_delta_[i] = 0;
            fts_meta_word_delta_[i] = 0;
        }
    }
    return sticky_rc;
}

/*
  start_bulk_update returns 0 when the engine will handle bulk batching.
  We then flip the flag that update_row checks at its tail so every row
  contributes to the shared ops counter.
*/
bool ha_tidesdb::start_bulk_update()
{
    /* MySQL semantics: true = "no bulk support, do single-row updates."
     * We ack-ed the call but tell MySQL not to use bulk update path. */
    in_bulk_update_ = false;
    bulk_insert_ops_ = 0;
    return true;
}

int ha_tidesdb::tdb_end_bulk_update() /* override removed: MariaDB-only */
{
    in_bulk_update_ = false;
    return 0;
}

/*
  The server calls bulk_update_row instead of update_row when start_bulk_update
  returned 0.  We don't actually buffer rows (TidesDB's txn is the buffer);
  we just delegate so the standard update_row path runs and its tail-side
  mid-commit block batches.  dup_key_found tracks duplicate-key collisions
  found in buffered-but-not-yet-applied rows -- since we apply immediately,
  it's always zero.
*/
int ha_tidesdb::bulk_update_row(const uchar *old_data, uchar *new_data, uint *dup_key_found)
{
    DBUG_ENTER("ha_tidesdb::bulk_update_row");
    if (dup_key_found) *dup_key_found = 0;
    DBUG_RETURN(update_row(old_data, new_data));
}

bool ha_tidesdb::start_bulk_delete()
{
    in_bulk_delete_ = true;
    bulk_insert_ops_ = 0;
    bulk_delete_rows_ = 0;
    /* L-6: length=0 indicates "no rows seen yet"; buffer content
       doesn't need clearing since we never read past _len_. */
    bulk_delete_min_pk_len_ = 0;
    bulk_delete_max_pk_len_ = 0;
    return 0;
}

int ha_tidesdb::end_bulk_delete()
{
    in_bulk_delete_ = false;

    /* Auto compact-after-range-delete.  Threshold zero (default) keeps the
       previous behavior, i.e. no synchronous compaction at end-of-statement.
       When the threshold is met we call tidesdb_compact_range over the
       observed [min_pk, max_pk] data-key range on the primary CF.  Secondary
       index tombstones are reclaimed by the per-CF tombstone_density_trigger
       on those CFs. */
    if (cached_compact_after_range_delete_min_rows_ > 0 &&
        bulk_delete_rows_ >= cached_compact_after_range_delete_min_rows_ && share && share->cf &&
        bulk_delete_min_pk_len_ > 0 && bulk_delete_max_pk_len_ > 0)
    {
        int crc = tidesdb_compact_range(
            share->cf, bulk_delete_min_pk_, bulk_delete_min_pk_len_,
            bulk_delete_max_pk_, bulk_delete_max_pk_len_);
        if (crc != TDB_SUCCESS)
        {
            sql_print_information(
                "[TIDESDB] post-DELETE compact_range on '%s' failed (rows=%llu, err=%d)",
                share->cf_name.c_str(), (unsigned long long)bulk_delete_rows_, crc);
        }
    }

    bulk_delete_rows_ = 0;
    bulk_delete_min_pk_len_ = 0;
    bulk_delete_max_pk_len_ = 0;
    return 0;
}

/* ******************** Index Condition Pushdown (ICP) ******************** */

Item *ha_tidesdb::idx_cond_push(uint keyno, Item *idx_cond)
{
    DBUG_ENTER("ha_tidesdb::idx_cond_push");

    /* Only secondary indexes go through icp_check_secondary, which calls
       handler_index_cond_check (end_range + pushed_cond + kill check). The
       PK branch in index_next/index_read_map skips that path -- if we
       accept ICP here, MySQL sets in_range_check_pushed_down = true,
       handler::compare_key short-circuits to 0, read_range_next stops
       enforcing end_range, and rows past the upper bound leak through.
       Refuse the push for PK; the SQL layer will filter post-fetch. */
    if (table_share && table_share->primary_key != MAX_KEY &&
        keyno == table_share->primary_key)
    {
        DBUG_RETURN(idx_cond);  /* not consumed -- server keeps it */
    }

    /* Secondary index -- accept the entire condition; we evaluate it in
       icp_check_secondary before each PK point-lookup. */
    pushed_idx_cond = idx_cond;
    pushed_idx_cond_keyno = keyno;
    in_range_check_pushed_down = true;

    DBUG_RETURN(NULL);
}

/* ******************** Multi-Range Read (MRR) ******************** */

/*
  Decide whether to accept a custom MRR strategy.  We only handle the case
  where every range the optimizer hands us is a full-key point lookup
  (UNIQUE_RANGE|EQ_RANGE) -- typically `WHERE col IN (v1, v2, ...)` on a
  PK or full-key unique index.  For mixed or true-range sequences we leave
  HA_MRR_USE_DEFAULT_IMPL set so the handler::multi_range_read_* default
  path runs unchanged.

  Iterating the sequence here consumes it; MariaDB re-initialises it before
  calling multi_range_read_init, so probing is safe.
*/
ha_rows ha_tidesdb::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq, void *seq_init_param,
                                                uint n_ranges_arg, uint *bufsz, uint *mrr_mode,
                                                ha_rows limit, Cost_estimate *cost) /* override removed: MariaDB-only signature */
{
    /* Compute the default cost + flags first so non-accepted sequences fall
       through to the server's MRR->read_range_first path with correct costing. */
    ha_rows rows = handler::multi_range_read_info_const(keyno, seq, seq_init_param, n_ranges_arg,
                                                        bufsz, mrr_mode, limit, cost);
    if (rows == HA_POS_ERROR) return rows;

    /* Partitioned tables are served by ha_partition, which dispatches
       multi_range_read_* across child handlers using its own DS-MRR-backed
       logic.  If we clear HA_MRR_USE_DEFAULT_IMPL here, ha_partition's
       ordered-index-scan path ends up invoking our custom _next without
       the state its own ordering logic expects and crashes.  Refuse to
       accept MRR for partitioned tables -- the default path runs correctly. */
    if (table && table->part_info) return rows;

    /* Probe the sequence, we accept only if every range is a full single-point
       equality.  A single non-point range forces us back to the default path. */
    KEY_MULTI_RANGE range;
    range_seq_t it = seq->init(seq_init_param, n_ranges_arg, *mrr_mode);
    bool all_point = true;
    uint count = 0;
    while (!seq->next(it, &range))
    {
        count++;
        if (!(range.range_flag & UNIQUE_RANGE) || (range.range_flag & NULL_RANGE) ||
            !(range.range_flag & EQ_RANGE))
        {
            all_point = false;
            break;
        }
    }

    /* Only accept when there are multiple ranges.  For a single point lookup
       the optimizer's eq_ref plan (plain index_read_map) is a better fit and
       -- critically -- also the only path where pessimistic row locking
       engages.  Accepting MRR for 1-range scans silently converts UPDATE
       WHERE pk=v into a range scan that bypasses that lock. */
    if (all_point && count >= MRR_ACCEPT_MIN_RANGES)
    {
        /* Clear the default-impl flag so the server calls our init/next
           instead of the fallback path. */
        *mrr_mode &= ~HA_MRR_USE_DEFAULT_IMPL;
        *bufsz = 0; /* we use our own std::vector, not HANDLER_BUFFER */
    }
    return rows;
}

/*
  Build the sorted list of point lookups, or fall through to the default
  impl if HA_MRR_USE_DEFAULT_IMPL is still set.  Sorting by comparable
  bytes converts N scattered LSM seeks into a monotone stream -- much
  friendlier to the block cache and the merge-heap.
*/
int ha_tidesdb::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param, uint n_ranges,
                                      uint mrr_mode, HANDLER_BUFFER *buf)
{
    DBUG_ENTER("ha_tidesdb::multi_range_read_init");

    mrr_custom_active_ = !(mrr_mode & HA_MRR_USE_DEFAULT_IMPL);
    if (!mrr_custom_active_)
        DBUG_RETURN(handler::multi_range_read_init(seq, seq_init_param, n_ranges, mrr_mode, buf));

    mrr_entries_.clear();
    mrr_next_idx_ = 0;
    mrr_keyno_ = active_index;
    mrr_no_assoc_ = (!!(mrr_mode & HA_MRR_NO_ASSOCIATION));
    if (n_ranges > 0) mrr_entries_.reserve(n_ranges);

    KEY *ki = &table->key_info[mrr_keyno_];

    /* We need all columns readable while translating the caller's key_copy
       bytes into our comparable format (key_copy_to_comparable calls
       key_restore into record[1] and reads fields). */
    my_bitmap_map *old_map = tmp_use_all_columns(table, table->read_set);

    KEY_MULTI_RANGE range;
    range_seq_t it = seq->init(seq_init_param, n_ranges, mrr_mode);
    while (!seq->next(it, &range))
    {
        uchar comp[MAX_KEY_LENGTH];
        uint comp_len =
            key_copy_to_comparable(ki, range.start_key.key, range.start_key.length, comp);

        tdb_mrr_entry e;
        e.comp_key.assign((const char *)comp, comp_len);
        e.ptr = range.ptr;
        mrr_entries_.push_back(std::move(e));
    }

    tmp_restore_column_map(table->read_set, old_map);

    std::sort(mrr_entries_.begin(), mrr_entries_.end(),
              [](const tdb_mrr_entry &a, const tdb_mrr_entry &b)
              { return a.comp_key < b.comp_key; });

    DBUG_RETURN(0);
}

/*
  Deliver the next row from the sorted list of point lookups.  PK lookups
  bypass the iterator entirely via fetch_row_by_pk; secondary index lookups
  reuse the cached scan iterator and a single seek per entry.  Rows that
  the index knew about but the data CF no longer has (stale entries after
  concurrent delete) are silently skipped.
*/
int ha_tidesdb::multi_range_read_next(range_id_t *range_info)
{
    DBUG_ENTER("ha_tidesdb::multi_range_read_next");

    if (!mrr_custom_active_) DBUG_RETURN(handler::multi_range_read_next(range_info));

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    /* Lazy txn -- the optimizer may invoke MRR without a prior rnd_init / index_init. */
    int erc = ensure_stmt_txn();
    if (erc) DBUG_RETURN(erc);
    if (!scan_txn) scan_txn = stmt_txn;

    bool is_pk_scan = share->has_user_pk && mrr_keyno_ == share->pk_index;
    uint idx_col_len = share->idx_comp_key_len[mrr_keyno_];

    while (mrr_next_idx_ < mrr_entries_.size())
    {
        const tdb_mrr_entry &e = mrr_entries_[mrr_next_idx_++];
        if (!mrr_no_assoc_) *range_info = e.ptr;

        if (is_pk_scan)
        {
            int rc = fetch_row_by_pk(scan_txn, (const uchar *)e.comp_key.data(),
                                     (uint)e.comp_key.size(), table->record[0]);
            if (rc == HA_ERR_KEY_NOT_FOUND) continue; /* stale range, try next */
            DBUG_RETURN(rc);
        }

        /* Secondary index point lookup -- seek, verify prefix match, then
           either cover-read from the index or PK-fetch. */
        if (mrr_keyno_ >= share->idx_cfs.size() || !share->idx_cfs[mrr_keyno_])
            continue; /* missing CF for this index -- skip defensively */
        scan_cf_ = share->idx_cfs[mrr_keyno_];
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);

        tidesdb_iter_seek(scan_iter, (const uint8_t *)e.comp_key.data(), (uint)e.comp_key.size());
        if (!tidesdb_iter_valid(scan_iter)) continue;

        uint8_t *ik = NULL;
        size_t iks = 0;
        if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) continue;
        if (iks < e.comp_key.size() || memcmp(ik, e.comp_key.data(), e.comp_key.size()) != 0)
            continue; /* no entry for this point */
        if (iks <= idx_col_len) continue;

        int rc;
        if (keyread_only_ && try_keyread_from_index(ik, iks, mrr_keyno_, table->record[0]))
            rc = 0;
        else
            rc = fetch_row_by_pk(scan_txn, ik + idx_col_len, (uint)(iks - idx_col_len),
                                 table->record[0]);
        if (rc == HA_ERR_KEY_NOT_FOUND) continue;
        DBUG_RETURN(rc);
    }

    DBUG_RETURN(HA_ERR_END_OF_FILE);
}

/* ******************** info ******************** */

int ha_tidesdb::info(uint flag)
{
    DBUG_ENTER("ha_tidesdb::info");

    if (share) ref_length = share->pk_key_len;

    /* HA_STATUS_ERRKEY -- handler::get_dup_key zeros errkey to (uint)-1 right
       before this call and expects the engine to populate it from the most
       recent dup detection. Without this, REPLACE / IODKU bail with
       ER_DUP_KEY (1022) instead of recovering via delete+insert and surfacing
       ER_DUP_ENTRY (1062) where appropriate. */
    if (flag & HA_STATUS_ERRKEY)
        errkey = last_dup_key_no_;

    if ((flag & (HA_STATUS_VARIABLE | HA_STATUS_CONST)) && share && share->cf)
    {
        long long now = (long long)microsecond_interval_timer();
        long long last = share->stats_refresh_us.load(std::memory_order_relaxed);
        if (now - last > TIDESDB_STATS_REFRESH_US &&
            share->stats_refresh_us.compare_exchange_weak(last, now, std::memory_order_relaxed))
        {
            tidesdb_stats_t *st = NULL;
            if (tidesdb_get_stats(share->cf, &st) == TDB_SUCCESS && st)
            {
                share->cached_records.store(st->total_keys, std::memory_order_relaxed);

                /* total_data_size only counts SSTable klog+vlog; memtable_size
                   holds the active memtable footprint.  Sum both so that
                   DATA_LENGTH in information_schema.TABLES is non-zero even
                   before the first flush.  When both are 0 (library gap),
                   fall back to total_keys * avg entry size. */
                uint64_t data_sz = st->total_data_size + (uint64_t)st->memtable_size;
                if (data_sz == 0 && st->total_keys > 0)
                    data_sz = (uint64_t)(st->total_keys * (st->avg_key_size + st->avg_value_size));
                share->cached_data_size.store(data_sz, std::memory_order_relaxed);
                uint32_t mrl = (uint32_t)(st->avg_key_size + st->avg_value_size);
                if (mrl == 0) mrl = table->s->reclength;
                share->cached_mean_rec_len.store(mrl, std::memory_order_relaxed);
                share->cached_read_amp.store(st->read_amp > 0 ? st->read_amp : READ_AMP_NONE,
                                             std::memory_order_relaxed);

                /* We sum secondary index CF sizes for index_file_length */
                uint64_t idx_total = 0;
                for (uint i = 0; i < share->idx_cfs.size(); i++)
                {
                    if (!share->idx_cfs[i]) continue;
                    tidesdb_stats_t *ist = NULL;
                    if (tidesdb_get_stats(share->idx_cfs[i], &ist) == TDB_SUCCESS && ist)
                    {
                        uint64_t isz = ist->total_data_size + (uint64_t)ist->memtable_size;
                        if (isz == 0 && ist->total_keys > 0)
                            isz = (uint64_t)(ist->total_keys *
                                             (ist->avg_key_size + ist->avg_value_size));
                        idx_total += isz;
                        tidesdb_free_stats(ist);
                    }
                }
                share->cached_idx_data_size.store(idx_total, std::memory_order_relaxed);

                tidesdb_free_stats(st);
            }
            share->stats_refresh_us.store(now, std::memory_order_relaxed);

            /* Also refresh SHOW GLOBAL STATUS variables while we're updating stats */
            tidesdb_refresh_status_vars();
        }

        /* We feed all cached values to the optimizer */
        stats.records = share->cached_records.load(std::memory_order_relaxed);
        if (stats.records == 0) stats.records = TIDESDB_MIN_STATS_RECORDS;
        stats.data_file_length = share->cached_data_size.load(std::memory_order_relaxed);
        stats.index_file_length = share->cached_idx_data_size.load(std::memory_order_relaxed);
        stats.mean_rec_length = share->cached_mean_rec_len.load(std::memory_order_relaxed);
        stats.delete_length = 0;
        stats.mrr_length_per_rec = ref_length + sizeof(uint64_t);
    }

    /* HA_STATUS_TIME -- we create_time from .frm stat and update_time from last DML */
    if ((flag & HA_STATUS_TIME) && share)
    {
        stats.create_time = share->create_time;
        stats.update_time = share->update_time.load(std::memory_order_relaxed);
    }

    /* HA_STATUS_CONST -- set rec_per_key for index selectivity estimates.
       PK and UNIQUE indexes -- rec_per_key = 1.
       Non-unique secondary indexes -- use cached_rec_per_key if populated
       by ANALYZE TABLE, else use a heuristic
       (total_keys / STATS_REC_PER_KEY_FALLBACK_DIVISOR). */
    if ((flag & HA_STATUS_CONST) && share)
    {
        for (uint i = 0; i < table->s->keys; i++)
        {
            KEY *key = &table->key_info[i];
            bool is_pk = share->has_user_pk && i == share->pk_index;
            bool is_unique = (key->flags & HA_NOSAME);
            ulong cached_rpk =
                (i < MAX_KEY) ? share->cached_rec_per_key[i].load(std::memory_order_relaxed) : 0;
            for (uint j = 0; j < key->actual_key_parts; j++)
            {
                if (is_pk || is_unique)
                {
                    if (j + 1 >= key->user_defined_key_parts)
                    {
                        /* Full unique key, exactly 1 row per distinct value */
                        key->rec_per_key[j] = REC_PER_KEY_UNIQUE;
                    }
                    else
                    {
                        /* Intermediate prefix of a composite unique key.
                           Estimate assuming uniform distribution:
                             cardinality(prefix_k) ≈ total^(k/N)
                             rec_per_key[j] = total^((N - j - 1) / N)
                           E.g. for PK(a,b,c) with 300K rows:
                             rec_per_key[0] ≈ 4481  (per distinct a)
                             rec_per_key[1] ≈ 67    (per distinct a,b)
                             rec_per_key[2] = 1     (unique)          */
                        uint N = key->user_defined_key_parts;
                        double rpk = pow((double)stats.records, (double)(N - j - 1) / (double)N);
                        key->rec_per_key[j] = (ulong)MY_MAX((ulong)rpk, REC_PER_KEY_FLOOR);
                    }
                }
                else if (j + 1 == key->user_defined_key_parts)
                {
                    /* Last user key part of a non-unique index.
                       We use ANALYZE-sampled value if available, else heuristic. */
                    if (cached_rpk > 0)
                        key->rec_per_key[j] = cached_rpk;
                    else
                        key->rec_per_key[j] =
                            (ulong)MY_MAX(stats.records / STATS_REC_PER_KEY_FALLBACK_DIVISOR + 1,
                                          REC_PER_KEY_FLOOR);
                }
                else
                {
                    /* Intermediate prefix of a non-unique index.
                       Geometrically interpolate between stats.records
                       (single leading column) and the last-part rec_per_key.
                       Formula is total / (total/last_rpk)^((j+1)/N) */
                    ulong last_rpk =
                        (cached_rpk > 0)
                            ? cached_rpk
                            : (ulong)(stats.records / STATS_REC_PER_KEY_FALLBACK_DIVISOR + 1);
                    uint N = key->user_defined_key_parts;
                    double base = (last_rpk > 0) ? (double)stats.records / (double)last_rpk
                                                 : (double)stats.records;
                    double rpk = (double)stats.records / pow(base, (double)(j + 1) / (double)N);
                    key->rec_per_key[j] =
                        (ulong)MY_MAX(MY_MIN((ulong)rpk, stats.records), REC_PER_KEY_FLOOR);
                }
            }
        }
    }

    DBUG_RETURN(0);
}

/* ******************** analyze ******************** */

/*
  ANALYZE TABLE -- refresh cached stats and output CF statistics as notes.
  The notes appear as additional Msg_type='note' rows in the ANALYZE TABLE
  result set, giving the user visibility into TidesDB internals.
*/
int ha_tidesdb::analyze(THD *thd, HA_CHECK_OPT *check_opt)
{
    DBUG_ENTER("ha_tidesdb::analyze");

    if (!share || !share->cf) DBUG_RETURN(HA_ADMIN_FAILED);

    share->stats_refresh_us.store(0, std::memory_order_relaxed);
    info(HA_STATUS_VARIABLE | HA_STATUS_CONST);

    tidesdb_stats_t *st = NULL;
    if (tidesdb_get_stats(share->cf, &st) != TDB_SUCCESS || !st)
    {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                            "[TIDESDB] unable to retrieve column family stats");
        DBUG_RETURN(HA_ADMIN_OK);
    }

    /* Summary line */
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                        "[TIDESDB] CF '%s'  total_keys=%llu  data_size=%llu bytes"
                        "  memtable=%zu bytes  levels=%d  read_amp=%.2f"
                        "  cache_hit=%.1f%%",
                        share->cf_name.c_str(), (unsigned long long)st->total_keys,
                        (unsigned long long)st->total_data_size, st->memtable_size, st->num_levels,
                        st->read_amp, st->hit_rate * PERCENT_SCALE);

    /* Average sizes */
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                        "[TIDESDB] avg_key=%.1f bytes  avg_value=%.1f bytes", st->avg_key_size,
                        st->avg_value_size);

    /* Per-level detail */
    for (int i = 0; i < st->num_levels; i++)
    {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                            "[TIDESDB] level %d  sstables=%d  size=%zu bytes"
                            "  keys=%llu",
                            i + 1, st->level_num_sstables[i], st->level_sizes[i],
                            (unsigned long long)st->level_key_counts[i]);
    }

    /* B+tree stats (only when use_btree=1) */
    if (st->use_btree)
    {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                            "[TIDESDB] btree  nodes=%llu  max_height=%u"
                            "  avg_height=%.2f",
                            (unsigned long long)st->btree_total_nodes, st->btree_max_height,
                            st->btree_avg_height);
    }

    tidesdb_free_stats(st);

    /* Secondary index CF stats + cardinality sampling.
       We iterate each secondary index CF, counting distinct index-column
       prefixes (everything before the PK suffix) to compute rec_per_key. */
    {
        int erc = ensure_stmt_txn();
        if (erc)
        {
            DBUG_RETURN(HA_ADMIN_OK); /* non-fatal -- stats just won't be updated */
        }
    }
    for (uint i = 0; i < table->s->keys; i++)
    {
        if (share->has_user_pk && i == share->pk_index) continue;
        if (i >= share->idx_cfs.size() || !share->idx_cfs[i]) continue;
        KEY *ki = &table->key_info[i];

        tidesdb_stats_t *ist = NULL;
        uint64_t idx_total_keys = 0;
        if (tidesdb_get_stats(share->idx_cfs[i], &ist) == TDB_SUCCESS && ist)
        {
            idx_total_keys = ist->total_keys;
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                                "[TIDESDB] idx CF '%s'  keys=%llu  data_size=%llu bytes"
                                "  levels=%d",
                                share->idx_cf_names[i].c_str(), (unsigned long long)ist->total_keys,
                                (unsigned long long)ist->total_data_size, ist->num_levels);
            tidesdb_free_stats(ist);
        }

        /* We sample the index to estimate distinct prefix count.
           For unique indexes rec_per_key is always 1.
           For non-unique indexes, scan up to ANALYZE_SAMPLE_LIMIT entries
           and count distinct index-column prefixes. */
        if (ki->flags & HA_NOSAME)
        {
            share->cached_rec_per_key[i].store(REC_PER_KEY_UNIQUE, std::memory_order_relaxed);
            continue;
        }

        uint idx_prefix_len = share->idx_comp_key_len[i];
        if (idx_prefix_len == 0) continue;

        tidesdb_iter_t *ait = NULL;
        if (tidesdb_iter_new(stmt_txn, share->idx_cfs[i], &ait) != TDB_SUCCESS || !ait) continue;

        tidesdb_iter_seek_to_first(ait);

        static constexpr uint64_t ANALYZE_SAMPLE_LIMIT = 100000;
        uint64_t sampled = 0, distinct = 0;
        uchar prev_prefix[MAX_KEY_LENGTH];
        uint prev_len = 0;

        while (tidesdb_iter_valid(ait) && sampled < ANALYZE_SAMPLE_LIMIT)
        {
            uint8_t *ik = NULL;
            size_t iks = 0;
            if (tidesdb_iter_key(ait, &ik, &iks) != TDB_SUCCESS) break;

            uint cmp_len = (iks >= idx_prefix_len) ? idx_prefix_len : (uint)iks;
            if (sampled == 0 || cmp_len != prev_len || memcmp(ik, prev_prefix, cmp_len) != 0)
            {
                distinct++;
                prev_len = cmp_len;
                memcpy(prev_prefix, ik, cmp_len);
            }
            sampled++;
            tidesdb_iter_next(ait);
        }
        tidesdb_iter_free(ait);

        if (distinct > 0)
        {
            /* We use sampled ratio to extrapolate for the full index */
            uint64_t total = (idx_total_keys > 0) ? idx_total_keys : sampled;
            if (sampled < total)
            {
                /* Extrapolate -- distinct_full ≈ distinct * (total / sampled) */
                double ratio = (double)total / (double)sampled;
                uint64_t est_distinct = (uint64_t)(distinct * ratio);
                if (est_distinct == 0) est_distinct = 1; /* divide-by-zero guard */
                ulong rpk = (ulong)(total / est_distinct);
                if (rpk == 0) rpk = REC_PER_KEY_FLOOR;
                share->cached_rec_per_key[i].store(rpk, std::memory_order_relaxed);
            }
            else
            {
                /* We sampled everything */
                ulong rpk = (ulong)(sampled / distinct);
                if (rpk == 0) rpk = REC_PER_KEY_FLOOR;
                share->cached_rec_per_key[i].store(rpk, std::memory_order_relaxed);
            }

            push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                                "[TIDESDB] idx '%s' sampled=%llu distinct=%llu rec_per_key=%lu",
                                ki->name, (unsigned long long)sampled,
                                (unsigned long long)distinct,
                                share->cached_rec_per_key[i].load(std::memory_order_relaxed));
        }
    }

    /* We re-run info to propagate the new rec_per_key values */
    info(HA_STATUS_CONST);

    DBUG_RETURN(HA_ADMIN_OK);
}

/* ******************** optimize ******************** */

/*
  OPTIMIZE TABLE -- trigger compaction on all CFs (data + secondary indexes).
  Compaction merges SSTables, removes tombstones, and reduces read
  amplification.  TidesDB enqueues the work to background compaction
  threads and returns immediately.
*/
int ha_tidesdb::optimize(THD *thd, HA_CHECK_OPT *check_opt)
{
    DBUG_ENTER("ha_tidesdb::optimize");

    if (!share || !share->cf) DBUG_RETURN(HA_ADMIN_FAILED);

    /* tidesdb_purge_cf() is synchronous -- flushes memtable to disk, then
       runs a full compaction inline, blocking until complete.  This is
       the right semantic for OPTIMIZE TABLE -- the caller expects the
       table to be fully compacted when the statement returns. */
    int rc = tidesdb_purge_cf(share->cf);
    if (rc != TDB_SUCCESS)
        sql_print_warning("[TIDESDB] optimize: purge data CF '%s' failed (err=%d)",
                          share->cf_name.c_str(), rc);

    for (uint i = 0; i < share->idx_cfs.size(); i++)
    {
        if (!share->idx_cfs[i]) continue;
        rc = tidesdb_purge_cf(share->idx_cfs[i]);
        if (rc != TDB_SUCCESS)
            sql_print_warning("[TIDESDB] optimize: purge idx CF '%s' failed (err=%d)",
                              share->idx_cf_names[i].c_str(), rc);
    }

    /* We do a refresh stats so the optimizer sees the post-compaction state sooner */
    share->stats_refresh_us.store(0, std::memory_order_relaxed);

    DBUG_RETURN(HA_ADMIN_OK);
}

int ha_tidesdb::check(THD *thd, HA_CHECK_OPT *check_opt)
{
    DBUG_ENTER("ha_tidesdb::check");

    if (!share || !share->cf) DBUG_RETURN(HA_ADMIN_CORRUPT);

    /* CHECK TABLE verifies all CFs are readable by fetching stats.
       tidesdb_get_stats reads metadata from all SSTables, which validates
       that manifests, block indexes, bloom filters, and metadata blocks
       are intact. For a deeper check, users can run REPAIR TABLE which
       does a full compaction pass that reads and re-checksums every block. */
    tidesdb_stats_t *st = NULL;
    int rc = tidesdb_get_stats(share->cf, &st);
    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] CHECK TABLE '%s': data CF check failed (err=%d)",
                        share->cf_name.c_str(), rc);
        DBUG_RETURN(HA_ADMIN_CORRUPT);
    }
    tidesdb_free_stats(st);

    for (uint i = 0; i < share->idx_cfs.size(); i++)
    {
        if (!share->idx_cfs[i]) continue;
        tidesdb_stats_t *ist = NULL;
        rc = tidesdb_get_stats(share->idx_cfs[i], &ist);
        if (rc != TDB_SUCCESS)
        {
            sql_print_error("[TIDESDB] CHECK TABLE '%s': index CF '%s' check failed (err=%d)",
                            share->cf_name.c_str(), share->idx_cf_names[i].c_str(), rc);
            DBUG_RETURN(HA_ADMIN_CORRUPT);
        }
        tidesdb_free_stats(ist);
    }

    DBUG_RETURN(HA_ADMIN_OK);
}

int ha_tidesdb::repair(THD *thd, HA_CHECK_OPT *check_opt)
{
    DBUG_ENTER("ha_tidesdb::repair");

    if (!share || !share->cf) DBUG_RETURN(HA_ADMIN_FAILED);

    /* REPAIR TABLE triggers a full purge (flush + compaction) of all CFs.
       In unified memtable mode, the first purge_cf call rotates the shared
       unified memtable and waits for the flush to complete. Subsequent
       purge_cf calls on index CFs skip the rotation (already done) and
       just run per-CF compaction. tidesdb_purge_cf is unified-mode aware
       and handles this idempotently. */
    int rc = tidesdb_purge_cf(share->cf);
    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] REPAIR TABLE '%s': purge data CF failed (err=%d)",
                        share->cf_name.c_str(), rc);
        DBUG_RETURN(HA_ADMIN_FAILED);
    }

    for (uint i = 0; i < share->idx_cfs.size(); i++)
    {
        if (!share->idx_cfs[i]) continue;
        rc = tidesdb_purge_cf(share->idx_cfs[i]);
        if (rc != TDB_SUCCESS)
            sql_print_warning("[TIDESDB] REPAIR TABLE '%s': purge idx CF '%s' failed (err=%d)",
                              share->cf_name.c_str(), share->idx_cf_names[i].c_str(), rc);
    }

    share->stats_refresh_us.store(0, std::memory_order_relaxed);
    DBUG_RETURN(HA_ADMIN_OK);
}

/* Semi-consistent read -- allows UPDATE/DELETE at READ_COMMITTED to skip
   locked rows by reading the last committed version instead of blocking. */
bool ha_tidesdb::was_semi_consistent_read()
{
    bool was = did_semi_consistent_read_;
    did_semi_consistent_read_ = false;
    return was;
}

void ha_tidesdb::try_semi_consistent_read(bool yes)
{
    semi_consistent_read_ = yes;
}

double ha_tidesdb::scan_time()
{
    IO_AND_CPU_COST cost;
    cost.io = 0.0;
    cost.cpu = 0.0;

    if (!share || !share->cf) return cost;

    /* Cache the range_cost result on the share with the same refresh
       interval as stats (TIDESDB_STATS_REFRESH_US = 2 seconds).
       tidesdb_range_cost examines in-memory metadata (block indexes,
       SSTable min/max keys) without disk I/O, but the computation
       still costs ~0.17% of TPC-C CPU when called per query plan. */
    auto now = std::chrono::steady_clock::now();
    auto cached_time = share->scan_cost_time.load(std::memory_order_relaxed);
    double cached_cost = share->cached_scan_cost.load(std::memory_order_relaxed);

    bool stale =
        (cached_cost <= 0.0) ||
        (std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() -
             cached_time >
         TIDESDB_STATS_REFRESH_US);

    if (stale)
    {
        uchar lo[KEY_NAMESPACE_LEN] = {KEY_NS_DATA};
        uchar hi[DATA_KEY_BUF_LEN];
        memset(hi, KEY_INF_HI_BYTE, sizeof(hi));
        uint hi_len = KEY_NAMESPACE_LEN + share->pk_key_len;
        if (hi_len > sizeof(hi)) hi_len = sizeof(hi);

        double full_cost = 0.0;
        if (tidesdb_range_cost(share->cf, lo, KEY_NAMESPACE_LEN, hi, hi_len, &full_cost) ==
                TDB_SUCCESS &&
            full_cost > 0.0)
        {
            cached_cost = full_cost;
            share->cached_scan_cost.store(cached_cost, std::memory_order_relaxed);
            share->scan_cost_time.store(
                std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch())
                    .count(),
                std::memory_order_relaxed);
        }
    }

    if (cached_cost > 0.0)
    {
        cost.io = cached_cost * TIDESDB_SCAN_IO_WEIGHT;
        cost.cpu = cached_cost * TIDESDB_SCAN_CPU_WEIGHT;
    }
    else
    {
        cost = handler::scan_time();
    }

    return cost;
}

ha_rows ha_tidesdb::records_in_range(uint inx, key_range *min_key, key_range *max_key)
{
    if (!share) return TIDESDB_RIR_DEFAULT_EST;

    ha_rows total = share->cached_records.load(std::memory_order_relaxed);
    if (total == 0) total = TIDESDB_MIN_STATS_RECORDS;

    /* We must determine which CF this index lives in */
    tidesdb_column_family_t *cf;
    bool is_pk = share->has_user_pk && inx == share->pk_index;
    if (is_pk)
        cf = share->cf;
    else if (inx < share->idx_cfs.size() && share->idx_cfs[inx])
        cf = share->idx_cfs[inx];
    else
        return (total / TIDESDB_RIR_UNKNOWN_DENOM) + REC_PER_KEY_FLOOR; /* no CF for this index */

    /* We convert min_key / max_key to our comparable format.
       If a bound is missing we use the natural boundary of the key space. */
    uchar lo_buf[DATA_KEY_BUF_LEN];
    uchar hi_buf[DATA_KEY_BUF_LEN];
    uint lo_len = 0, hi_len = 0;

    my_bitmap_map *old_map = tmp_use_all_columns(table, table->read_set);

    if (min_key && min_key->key)
    {
        KEY *ki = &table->key_info[inx];
        uint kl = calculate_key_len(table, inx, min_key->keypart_map);
        if (is_pk)
        {
            uchar comp[MAX_KEY_LENGTH];
            uint comp_len = key_copy_to_comparable(ki, min_key->key, kl, comp);
            lo_len = build_data_key(comp, comp_len, lo_buf);
        }
        else
        {
            lo_len = key_copy_to_comparable(ki, min_key->key, kl, lo_buf);
        }
    }
    else
    {
        /* No lower bound, we use smallest possible key */
        if (is_pk)
        {
            lo_buf[0] = KEY_NS_DATA;
            lo_len = KEY_NAMESPACE_LEN;
        }
        else
        {
            lo_buf[0] = KEY_INF_LO_BYTE;
            lo_len = KEY_NAMESPACE_LEN;
        }
    }

    if (max_key && max_key->key)
    {
        KEY *ki = &table->key_info[inx];
        uint kl = calculate_key_len(table, inx, max_key->keypart_map);
        if (is_pk)
        {
            uchar comp[MAX_KEY_LENGTH];
            uint comp_len = key_copy_to_comparable(ki, max_key->key, kl, comp);
            hi_len = build_data_key(comp, comp_len, hi_buf);
        }
        else
        {
            hi_len = key_copy_to_comparable(ki, max_key->key, kl, hi_buf);
        }
    }
    else
    {
        /* No upper bound, we use largest possible key */
        memset(hi_buf, KEY_INF_HI_BYTE, sizeof(hi_buf));
        hi_len = is_pk ? (KEY_NAMESPACE_LEN + share->pk_key_len)
                       : share->idx_comp_key_len[inx] + share->pk_key_len;
        if (hi_len > sizeof(hi_buf)) hi_len = sizeof(hi_buf);
    }

    tmp_restore_column_map(table->read_set, old_map);

    /* We detect point equality, both bounds provided with identical comparable
       bytes.  tidesdb_range_cost is an I/O cost metric, not a cardinality
       metric -- for memtable-only data it cannot distinguish a point range
       from a full scan.  For equalities we return rec_per_key directly. */
    if (min_key && max_key && lo_len > 0 && hi_len > 0 && lo_len == hi_len &&
        memcmp(lo_buf, hi_buf, lo_len) == 0)
    {
        KEY *ki = &table->key_info[inx];
        uint parts_used = my_count_bits(min_key->keypart_map);
        if (parts_used > 0 && parts_used <= ki->user_defined_key_parts)
        {
            ulong rpk = ki->rec_per_key[parts_used - 1];
            ha_rows est = (rpk > 0) ? (ha_rows)rpk : REC_PER_KEY_FLOOR;
            if (est > total) est = total;
            return est;
        }
        return REC_PER_KEY_FLOOR;
    }

    /* We ask TidesDB for the range cost (no disk I/O -- uses in-memory
       block indexes, SSTable min/max keys, and entry counts). */
    double range_cost = 0.0;
    int rc = tidesdb_range_cost(cf, lo_buf, lo_len, hi_buf, hi_len, &range_cost);
    if (rc != TDB_SUCCESS || range_cost <= 0.0)
        return (total / TIDESDB_RIR_UNKNOWN_DENOM) + REC_PER_KEY_FLOOR; /* fallback */

    /* We get full-range cost for normalization.  We use the natural boundaries
       of the key space so that range_cost / full_cost ≈ fraction of data. */
    double full_cost = 0.0;
    {
        uchar full_lo[KEY_NAMESPACE_LEN] = {(uchar)(is_pk ? KEY_NS_DATA : KEY_INF_LO_BYTE)};
        uchar full_hi[DATA_KEY_BUF_LEN];
        memset(full_hi, KEY_INF_HI_BYTE, sizeof(full_hi));
        uint full_hi_len = hi_len; /* same width as hi_buf */
        tidesdb_range_cost(cf, full_lo, KEY_NAMESPACE_LEN, full_hi, full_hi_len, &full_cost);
    }

    if (full_cost <= 0.0)
        return (total / TIDESDB_RIR_UNKNOWN_DENOM) + REC_PER_KEY_FLOOR; /* fallback */

    /* We estimate records proportionally -- narrower range -> fewer records */
    double fraction = range_cost / full_cost;
    if (fraction > FRACTION_MAX) fraction = FRACTION_MAX;
    if (fraction < FRACTION_MIN) fraction = FRACTION_MIN;

    ha_rows est = (ha_rows)(total * fraction);
    if (est == 0) est = REC_PER_KEY_FLOOR; /* never return 0 -- optimizer treats it as "empty" */

    /* When both bounds are provided but the estimated fraction is very
       high (>TIDESDB_RIR_FRACTION_UNRELIABLE), tidesdb_range_cost is
       likely unreliable -- this happens with memtable-only data where
       the cost function cannot distinguish a narrow range from a full
       scan.  Fall back to a rec_per_key-based estimate for the prefix. */
    if (min_key && max_key && fraction > TIDESDB_RIR_FRACTION_UNRELIABLE)
    {
        KEY *ki = &table->key_info[inx];
        uint parts = my_count_bits(min_key->keypart_map);
        if (parts > 0 && parts <= ki->user_defined_key_parts)
        {
            ulong rpk = ki->rec_per_key[parts - 1];
            if (rpk > 0)
            {
                ha_rows capped;
                if (lo_len == hi_len && memcmp(lo_buf, hi_buf, lo_len) == 0)
                {
                    /* Point equality, we use rec_per_key directly */
                    capped = (ha_rows)rpk;
                }
                else
                {
                    /* With range scans we multiply rec_per_key by a conservative
                       range-width factor.  Typical OLTP ranges span tens of
                       key values; the multiplier keeps the estimate tight while
                       still being vastly better than the unreliable full ratio. */
                    capped = (ha_rows)rpk * TIDESDB_RIR_RANGE_RPK_MULTIPLIER;
                    const ha_rows cap = total / TIDESDB_RIR_RANGE_CAP_DENOM;
                    if (capped > cap) capped = cap;
                }
                if (capped < est) est = MY_MAX(capped, REC_PER_KEY_FLOOR);
            }
        }
    }

    return est;
}

ulong ha_tidesdb::index_flags(uint idx, uint part, bool all_parts) const
{
    /* FULLTEXT indexes do not support ordered reads or ICP */
    if (table_share && idx < table_share->keys &&
        table_share->key_info[idx].algorithm == HA_KEY_ALG_FULLTEXT)
        return 0;

    /* SPATIAL indexes support MBR range scans and forward iteration */
    if (table_share && idx < table_share->keys && is_spatial_index(&table_share->key_info[idx]))
        return HA_READ_NEXT | HA_READ_RANGE;

    ulong flags =
        HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE | HA_DO_INDEX_COND_PUSHDOWN;
    if (table_share && table_share->primary_key != MAX_KEY && idx == table_share->primary_key)
        flags |= HA_CLUSTERED_INDEX;
    else
        flags |= HA_KEYREAD_ONLY;
    return flags;
}

const char *ha_tidesdb::index_type(uint key_number)
{
    if (key_number < table->s->keys)
    {
        if (table->key_info[key_number].algorithm == HA_KEY_ALG_FULLTEXT) return "FULLTEXT";
        if (is_spatial_index(&table->key_info[key_number])) return "RTREE";
        ha_index_option_struct *iopts = TDB_INDEX_OPTIONS(&table->key_info[key_number]);
        if (iopts && iopts->use_btree) return "BTREE";
    }
    ha_table_option_struct *opts = TDB_TABLE_OPTIONS(table);
    return (opts && opts->use_btree) ? "BTREE" : "LSM";
}

/* ******************** Spatial scan continuation ******************** */

int ha_tidesdb::spatial_scan_next(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::spatial_scan_next");

    tdb_mbr_t query_mbr;
    query_mbr.xmin = spatial_qmbr_[MBR_XMIN_IDX];
    query_mbr.ymin = spatial_qmbr_[MBR_YMIN_IDX];
    query_mbr.xmax = spatial_qmbr_[MBR_XMAX_IDX];
    query_mbr.ymax = spatial_qmbr_[MBR_YMAX_IDX];

    while (spatial_range_idx_ < spatial_ranges_.size())
    {
        uint64_t cur_hi = spatial_ranges_[spatial_range_idx_].second;

        while (tidesdb_iter_valid(scan_iter))
        {
            if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

            uint8_t *ik = NULL;
            size_t iks = 0;
            if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) break;

            if (iks <= SPATIAL_HILBERT_KEY_LEN)
            {
                tidesdb_iter_next(scan_iter);
                continue;
            }

            /* We check if past the current range's upper bound */
            uint64_t h = decode_hilbert_be(ik);
            if (h > cur_hi) break; /* advance to next range */

            /* We read stored MBR from value */
            uint8_t *val = NULL;
            size_t vlen = 0;
            if (tidesdb_iter_value(scan_iter, &val, &vlen) != TDB_SUCCESS ||
                vlen < SPATIAL_MBR_VALUE_LEN)
            {
                tidesdb_iter_next(scan_iter);
                continue;
            }

            /* The on-disk spatial value is exactly SPATIAL_MBR_VALUE_LEN bytes
               laid out as [xmin,ymin,xmax,ymax] (4 doubles in native order),
               matching tdb_mbr_t's field order.  We assert the struct size
               against the wire size so adding a field to tdb_mbr_t will
               fire the static_assert rather than silently corrupt reads. */
            static_assert(sizeof(tdb_mbr_t) == SPATIAL_MBR_VALUE_LEN,
                          "tdb_mbr_t must match on-disk spatial value layout");
            tdb_mbr_t entry_mbr;
            memcpy(&entry_mbr, val, SPATIAL_MBR_VALUE_LEN);

            /* We apply MBR predicate */
            if (!spatial_mbr_predicate(spatial_mode_, &query_mbr, &entry_mbr))
            {
                tidesdb_iter_next(scan_iter);
                continue;
            }

            /* A match, we extract PK from key suffix and fetch full row */
            const uchar *pk = ik + SPATIAL_HILBERT_KEY_LEN;
            uint pk_len = (uint)(iks - SPATIAL_HILBERT_KEY_LEN);

            int ret = fetch_row_by_pk(scan_txn, pk, pk_len, buf);
            if (ret == HA_ERR_KEY_NOT_FOUND)
            {
                tidesdb_iter_next(scan_iter);
                continue;
            }
            if (ret)
            {
                DBUG_RETURN(ret);
            }

            scan_dir_ = DIR_FORWARD;
            DBUG_RETURN(0);
        }

        /* The current range exhausted, thus we advance to next range and seek */
        spatial_range_idx_++;
        if (spatial_range_idx_ < spatial_ranges_.size())
        {
            uchar seek_key[SPATIAL_HILBERT_KEY_LEN];
            encode_hilbert_be(spatial_ranges_[spatial_range_idx_].first, seek_key);
            tidesdb_iter_seek(scan_iter, seek_key, SPATIAL_HILBERT_KEY_LEN);
        }
    }

    DBUG_RETURN(HA_ERR_END_OF_FILE);
}

/* ******************** Full-Text Search methods ******************** */

int ha_tidesdb::ft_init()
{
    DBUG_ENTER("ha_tidesdb::ft_init");
    if (ft_handler)
    {
        tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(ft_handler);
        info->current_idx = 0;
    }
    DBUG_RETURN(0);
}

void ha_tidesdb::ft_end()
{
    DBUG_ENTER("ha_tidesdb::ft_end");
    DBUG_VOID_RETURN;
}

FT_INFO *ha_tidesdb::ft_init_ext(uint flags, uint inx, String *key)
{
    DBUG_ENTER("ha_tidesdb::ft_init_ext");

    if (!share || inx >= share->idx_cfs.size() || !share->idx_cfs[inx] ||
        !is_fts_index(&table->key_info[inx]))
        DBUG_RETURN(NULL);

    /* We ensure we have a transaction for reading */
    {
        int erc = ensure_stmt_txn();
        if (erc) DBUG_RETURN(NULL);
    }

    CHARSET_INFO *cs = table->key_info[inx].key_part[0].field->charset();

    /* We parse query into terms */
    std::vector<fts_query_term_t> query_terms;
    if (flags & FT_BOOL)
    {
        fts_parse_boolean(key->ptr(), key->length(), cs, query_terms);
    }
    else
    {
        std::vector<fts_token_t> tokens;
        fts_tokenize(key->ptr(), key->length(), cs, tokens);
        for (auto &tok : tokens)
        {
            fts_query_term_t qt;
            qt.term = std::move(tok.word);
            qt.yesno = FTS_TERM_NEUTRAL;
            qt.trunc = false;
            qt.is_phrase = false;
            query_terms.push_back(std::move(qt));
        }
    }

    /* A query that tokenises down to nothing (e.g. all stop words, all
       characters below the min-word-len threshold) still has to return a
       usable FT_INFO, not NULL.  The server's execution path assumes an
       FT_INFO was handed back and leaves the diagnostics area in an
       inconsistent state when ft_init_ext yields NULL for reasons other
       than an outright error; debug builds trip Protocol::end_statement's
       DBUG_ASSERT(0).  We return an empty result set instead, which the
       optimizer folds into zero matched rows. */
    if (query_terms.empty())
    {
        tdb_ft_info_t *empty = new tdb_ft_info_t();
        empty->please = const_cast<_ft_vft *>(&tdb_ft_vft);
        empty->could_you = &tdb_ft_vft_ext;
        empty->handler = this;
        empty->keynr = inx;
        empty->current_idx = 0;
        empty->current_rank = 0.0f;
        empty->match_count = 0;
        DBUG_RETURN(reinterpret_cast<FT_INFO *>(empty));
    }

    /* We load BM25 global metadata */
    int64_t total_docs = 0, total_words = 0;
    fts_load_meta(stmt_txn, share->cf, inx, &total_docs, &total_words);
    double avgdl = total_docs > 0 ? (double)total_words / (double)total_docs : BM25_DEFAULT_AVGDL;
    if (total_docs == 0) total_docs = BM25_MIN_TOTAL_DOCS; /* avoid division by zero */
    /* We precompute 1/avgdl so the per-posting BM25 loop multiplies instead of
       dividing.  Divisions are expensive on modern CPUs (~20 cycles vs ~5
       for a multiply) and this runs per term per matched document. */
    const double inv_avgdl = 1.0 / avgdl;

    /* For each query term, prefix-scan the FTS CF to gather postings and score */
    std::unordered_map<std::string, double> doc_scores;
    /* We track which docs matched each required (+) term for boolean filtering */
    std::unordered_map<std::string, uint> doc_required_hits;
    uint num_required = 0;

    for (auto &qt : query_terms)
    {
        if (qt.yesno > FTS_TERM_NEUTRAL) num_required++;

        /* We build prefix key-- [2-byte term_len][term bytes] */
        uchar prefix[FTS_TERM_LEN_PREFIX + FTS_MAX_TERM_BYTES];
        uint prefix_len = 0;
        int2store(prefix, (uint16)qt.term.size());
        prefix_len += FTS_TERM_LEN_PREFIX;
        memcpy(prefix + prefix_len, qt.term.data(), qt.term.size());
        prefix_len += (uint)qt.term.size();

        /* We scan postings for this term */
        struct posting_entry
        {
            std::string pk;
            uint16 tf;
            uint32 doc_len;
        };
        std::vector<posting_entry> postings;

        tidesdb_iter_t *it = NULL;
        int rc = tidesdb_iter_new(stmt_txn, share->idx_cfs[inx], &it);
        if (rc != TDB_SUCCESS || !it) continue;

        if (qt.trunc)
        {
            /* Wildcard search keys are sorted by [2B term_len][term][pk],
               so terms of different lengths are in different regions.
               We iterate over each possible term length from the prefix
               length up to max_word_len, seeking directly to [len][prefix]
               for each bucket.  This is O(max_word_len) seeks, each precise. */
            uint min_len = (uint)qt.term.size();
            uint max_len = (uint)srv_fts_max_word_len;
            if (max_len > FTS_MAX_TERM_BYTES) max_len = FTS_MAX_TERM_BYTES;

            for (uint tlen = min_len; tlen <= max_len; tlen++)
            {
                /* We build seek key--[2B tlen][prefix bytes] */
                uchar seek[FTS_TERM_LEN_PREFIX + FTS_MAX_TERM_BYTES];
                int2store(seek, (uint16)tlen);
                memcpy(seek + FTS_TERM_LEN_PREFIX, qt.term.data(), qt.term.size());
                uint seek_len = FTS_TERM_LEN_PREFIX + (uint)qt.term.size();

                tidesdb_iter_seek(it, seek, seek_len);
                while (tidesdb_iter_valid(it))
                {
                    uint8_t *ik = NULL;
                    size_t iks = 0;
                    if (tidesdb_iter_key(it, &ik, &iks) != TDB_SUCCESS) break;

                    /* We verify we're still in the same length bucket */
                    if (iks < FTS_TERM_LEN_PREFIX) break;
                    uint16 stored_len = uint2korr(ik);
                    if (stored_len != tlen) break;

                    /* We verify prefix match */
                    if (iks < (size_t)(FTS_TERM_LEN_PREFIX + stored_len)) break;
                    if (memcmp(ik + FTS_TERM_LEN_PREFIX, qt.term.data(), qt.term.size()) != 0)
                        break;

                    /* We extract PK and value */
                    uint pk_off = FTS_TERM_LEN_PREFIX + stored_len;
                    if (iks <= pk_off)
                    {
                        tidesdb_iter_next(it);
                        continue;
                    }
                    std::string pk((char *)(ik + pk_off), iks - pk_off);

                    uint8_t *iv = NULL;
                    size_t ivs = 0;
                    if (tidesdb_iter_value(it, &iv, &ivs) == TDB_SUCCESS && ivs >= FTS_VALUE_LEN)
                        postings.push_back({pk, (uint16)uint2korr(iv),
                                            (uint32)uint4korr(iv + FTS_VALUE_DOC_LEN_OFFSET)});

                    tidesdb_iter_next(it);
                }
            }
            tidesdb_iter_free(it);
            it = NULL;
        }
        else
        {
            tidesdb_iter_seek(it, prefix, prefix_len);
        }

        if (it) /* exact-match path (non-truncated) */
            while (tidesdb_iter_valid(it))
            {
                uint8_t *ik = NULL;
                size_t iks = 0;
                if (tidesdb_iter_key(it, &ik, &iks) != TDB_SUCCESS) break;

                {
                    /* We exact term match */
                    if (iks < prefix_len || memcmp(ik, prefix, prefix_len) != 0) break;
                    std::string pk((char *)(ik + prefix_len), iks - prefix_len);

                    uint8_t *iv = NULL;
                    size_t ivs = 0;
                    if (tidesdb_iter_value(it, &iv, &ivs) == TDB_SUCCESS && ivs >= FTS_VALUE_LEN)
                        postings.push_back({pk, (uint16)uint2korr(iv),
                                            (uint32)uint4korr(iv + FTS_VALUE_DOC_LEN_OFFSET)});
                }
                tidesdb_iter_next(it);
            }
        if (it) tidesdb_iter_free(it);

        /* We compute BM25 scores.. */
        uint32 df = (uint32)postings.size();
        double idf = std::log(((double)total_docs - (double)df + BM25_IDF_EPSILON) /
                                  ((double)df + BM25_IDF_EPSILON) +
                              BM25_IDF_NONNEG_SHIFT);
        const double k1 = srv_fts_bm25_k1, b = srv_fts_bm25_b;

        for (auto &p : postings)
        {
            double tf_norm = ((double)p.tf * (k1 + BM25_TF_SATURATION_BOOST)) /
                             ((double)p.tf +
                              k1 * (BM25_LENGTH_NORM_BASE - b + b * (double)p.doc_len * inv_avgdl));
            double score = idf * tf_norm;

            if (qt.yesno < FTS_TERM_NEUTRAL)
            {
                /* excluded term! we remove from results */
                doc_scores.erase(p.pk);
            }
            else
            {
                doc_scores[p.pk] += score;
                if (qt.yesno > FTS_TERM_NEUTRAL) doc_required_hits[p.pk]++;
            }
        }
    }

    /* bool mode -- we filter docs that don't match all required terms */
    if (num_required > 0)
    {
        for (auto it = doc_scores.begin(); it != doc_scores.end();)
        {
            auto rh = doc_required_hits.find(it->first);
            if (rh == doc_required_hits.end() || rh->second < num_required)
                it = doc_scores.erase(it);
            else
                ++it;
        }
    }

    /* We build sorted result set */
    tdb_ft_info_t *info = new tdb_ft_info_t();
    info->please = const_cast<_ft_vft *>(&tdb_ft_vft);
    info->could_you = &tdb_ft_vft_ext;
    info->handler = this;
    info->keynr = inx;
    info->current_idx = 0;
    info->current_rank = 0.0f;
    info->match_count = 0;

    for (auto &[pk_str, score] : doc_scores)
    {
        tdb_fts_result_t r;
        r.pk_len = (uint)pk_str.size();
        r.pk = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, r.pk_len, MYF(0));
        if (!r.pk) continue;
        memcpy(r.pk, pk_str.data(), r.pk_len);
        r.rank = (float)score;
        info->results.push_back(r);
    }

    /* For any phrase terms in the query, we fetch each
       candidate row, re-tokenize the document, and check for the exact
       phrase as a consecutive word sequence.  We remove non-matching candidates. */
    bool has_phrases = false;
    std::vector<const fts_query_term_t *> phrases;
    for (auto &qt : query_terms)
    {
        if (qt.is_phrase)
        {
            has_phrases = true;
            phrases.push_back(&qt);
        }
    }

    if (has_phrases && !info->results.empty())
    {
        CHARSET_INFO *vcs = table->key_info[inx].key_part[0].field->charset();
        std::vector<tdb_fts_result_t> verified;
        /* Tokenize each candidate once and check all phrases against the
           single token vector.  The old code tokenized the same doc M times
           when M phrases were pending. */
        std::vector<fts_token_t> doc_tokens;
        for (auto &r : info->results)
        {
            int err = fetch_row_by_pk(stmt_txn, r.pk, r.pk_len, table->record[0]);
            if (err) continue;

            doc_tokens.clear();
            fts_extract_and_tokenize(table, &table->key_info[inx], table->record[0], vcs,
                                     doc_tokens);

            bool all_phrases_match = true;
            for (auto *ph : phrases)
            {
                if (!fts_phrase_in_tokens(doc_tokens, ph->phrase_words))
                {
                    all_phrases_match = false;
                    break;
                }
            }

            if (all_phrases_match)
                verified.push_back(r);
            else
                my_free(r.pk); /* free PK of non-matching result */
        }
        info->results = std::move(verified);
    }

    std::sort(info->results.begin(), info->results.end(),
              [](const tdb_fts_result_t &a, const tdb_fts_result_t &b) { return a.rank > b.rank; });

    info->match_count = (ulonglong)info->results.size();

    DBUG_RETURN(reinterpret_cast<FT_INFO *>(info));
}

int ha_tidesdb::ft_read(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::ft_read");

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(ft_handler);
    if (!info)
    {
        DBUG_RETURN(HA_ERR_END_OF_FILE);
    }

    /* We ensure we have a transaction */
    {
        int erc = ensure_stmt_txn();
        if (erc) DBUG_RETURN(erc);
    }

    /* We loop to skip rows that were deleted after ft_init_ext() built the result set */
    while (info->current_idx < info->results.size())
    {
        tdb_fts_result_t &r = info->results[info->current_idx];
        info->current_rank = r.rank;

        int err = fetch_row_by_pk(stmt_txn, r.pk, r.pk_len, buf);
        if (err == HA_ERR_KEY_NOT_FOUND)
        {
            info->current_idx++;
            continue; /* skip stale entry */
        }
        if (err)
        {
            DBUG_RETURN(err);
        }

        info->current_idx++;
        DBUG_RETURN(0);
    }

    DBUG_RETURN(HA_ERR_END_OF_FILE);
}

int ha_tidesdb::extra(enum ha_extra_function operation)
{
    switch (operation)
    {
        case HA_EXTRA_KEYREAD:
            keyread_only_ = true;
            break;
        case HA_EXTRA_NO_KEYREAD:
            keyread_only_ = false;
            break;
        case HA_EXTRA_WRITE_CAN_REPLACE:
            /* REPLACE INTO -- we skip uniqueness check, overwrite silently */
            write_can_replace_ = true;
            break;
        case HA_EXTRA_INSERT_WITH_UPDATE:
            /* INSERT ON DUPLICATE KEY UPDATE -- the server needs write_row
               to return HA_ERR_FOUND_DUPP_KEY so it can switch to update_row. */
            break;
        case HA_EXTRA_WRITE_CANNOT_REPLACE:
            write_can_replace_ = false;
            break;
        case HA_EXTRA_PREPARE_FOR_DROP:
            /* Table is about to be dropped -- skip fsync overhead */
            break;
        default:
            break;
    }
    return 0;
}

/* ******************** Locking ******************** */

/*
  Lazy txn creation.  Gets the per-connection TidesDB txn (shared by
  all handler objects on this connection).  The txn spans the entire
  BEGIN...COMMIT block, not just one statement.
*/
int ha_tidesdb::ensure_stmt_txn()
{
    if (stmt_txn) return 0;

    THD *thd = cached_thd_ ? cached_thd_ : ha_thd();

    /* We resolve isolation, really same logic as external_lock.
       DDL uses READ_COMMITTED to avoid unbounded read-set growth.
       Autocommit single-statement DML uses READ_COMMITTED (no
       concurrent modification within a single-stmt txn to conflict with).
       Multi-statement transactions (BEGIN...COMMIT) use the session's
       isolation level so that write-write conflict detection is active.

       We prefer the per-statement cache populated by external_lock and fall
       through to the live THD call only on paths where external_lock
       hasn't run yet (e.g. some DDL callbacks). */
    int sql_cmd;
    bool is_autocommit;
    if (cached_stmt_shape_valid_)
    {
        sql_cmd = cached_sql_cmd_;
        is_autocommit = cached_is_autocommit_;
    }
    else
    {
        sql_cmd = thd_sql_command(thd);
        is_autocommit = !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
    }
    bool is_ddl =
        (sql_cmd == SQLCOM_ALTER_TABLE || sql_cmd == SQLCOM_CREATE_INDEX ||
         sql_cmd == SQLCOM_DROP_INDEX || sql_cmd == SQLCOM_TRUNCATE || sql_cmd == SQLCOM_OPTIMIZE ||
         sql_cmd == SQLCOM_CREATE_TABLE || sql_cmd == SQLCOM_DROP_TABLE);
    tidesdb_isolation_level_t effective_iso;
    if (is_ddl || is_autocommit)
        effective_iso = TDB_ISOLATION_READ_COMMITTED;
    else if (unlikely(srv_pessimistic_locking))
        /* When pessimistic row locks are active, the locks already serialize
           access to hot rows.  Use READ_COMMITTED to skip the library's
           write-write conflict tracking at commit time -- the lock manager
           prevents the conflicts that tracking would detect. */
        effective_iso = TDB_ISOLATION_READ_COMMITTED;
    else
        effective_iso = resolve_effective_isolation(
            thd, share ? share->isolation_level : TDB_ISOLATION_SNAPSHOT);
    tidesdb_trx_t *trx = get_or_create_trx(thd, ht, effective_iso);
    if (!trx) return HA_ERR_OUT_OF_MEM;

    stmt_txn = trx->txn;
    return 0;
}

int ha_tidesdb::external_lock(THD *thd, int lock_type)
{
    DBUG_ENTER("ha_tidesdb::external_lock");

    if (lock_type != F_UNLCK)
    {
        /* We resolve per-statement THD shape once and cache to ensure_stmt_txn
           reads the cache instead of re-calling thd_sql_command() and
           thd_test_options(). */
        int sql_cmd = thd_sql_command(thd);
        bool is_ddl = (sql_cmd == SQLCOM_ALTER_TABLE || sql_cmd == SQLCOM_CREATE_INDEX ||
                       sql_cmd == SQLCOM_DROP_INDEX || sql_cmd == SQLCOM_TRUNCATE ||
                       sql_cmd == SQLCOM_OPTIMIZE || sql_cmd == SQLCOM_CREATE_TABLE ||
                       sql_cmd == SQLCOM_DROP_TABLE);
        bool is_autocommit = !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

        cached_sql_cmd_ = sql_cmd;
        cached_is_autocommit_ = is_autocommit;
        cached_stmt_shape_valid_ = true;

        tidesdb_isolation_level_t effective_iso;
        if (is_ddl || is_autocommit)
            effective_iso = TDB_ISOLATION_READ_COMMITTED;
        else
            effective_iso = resolve_effective_isolation(
                thd, share ? share->isolation_level : TDB_ISOLATION_SNAPSHOT);
        tidesdb_trx_t *trx = get_or_create_trx(thd, ht, effective_iso);
        if (!trx) DBUG_RETURN(HA_ERR_OUT_OF_MEM);

        stmt_txn = trx->txn;
        stmt_txn_dirty = false;
        stmt_has_write_lock_ |= (lock_type == F_WRLCK);

        /* We cache THD and trx pointers for fast access in hot paths
           (index_read_map, update_row, delete_row, ensure_stmt_txn).
           Eliminates ha_thd() virtual dispatch and thd_get_ha_data()
           hash lookup on every row operation. */
        cached_thd_ = thd;
        cached_trx_ = trx;

        /* We register at statement level (its always needed per statement) */
        trans_register_ha(thd, false, ht, 0);

        /* We register at transaction level if inside BEGIN block */
        if (!is_autocommit) trans_register_ha(thd, true, ht, 0);
    }
    else
    {
        /* For multi-statement transactions (BEGIN...COMMIT), the txn stays
           the same across statements.  Preserve scan_iter and dup_iter_cache
           across READ-ONLY statements so the next statement can reuse them
           (avoids O(sstables) merge-heap rebuild).
           After WRITE statements, iterators must be invalidated because
           new txn ops (puts/deletes) are not visible to iterators created
           before those ops were added.  For autocommit, always free. */
        bool in_multi_stmt =
            cached_stmt_shape_valid_
                ? !cached_is_autocommit_
                : (bool)thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
        if (!in_multi_stmt || stmt_txn_dirty)
        {
            if (scan_iter)
            {
                tidesdb_iter_free(scan_iter);
                scan_iter = NULL;
                scan_iter_cf_ = NULL;
                scan_iter_txn_ = NULL;
            }
            if (dup_iter_count_ > 0) free_dup_iter_cache();
        }

        /* We bump update_time once per write-statement for information_schema.
           We use cached_time_ if available to avoid another time() syscall. */
        if (stmt_txn_dirty && share)
            share->update_time.store(cached_time_valid_ ? cached_time_ : time(0),
                                     std::memory_order_relaxed);

        /* We invalidate all per-statement caches so the next statement
           picks up any changes (key rotation, session variable changes,
           clock advance). */
        enc_key_ver_valid_ = false;
        cached_time_valid_ = false;
        cached_thdvars_valid_ = false;

        stmt_txn = NULL;
        stmt_txn_dirty = false;
        stmt_has_write_lock_ = false;
        cached_thd_ = NULL;
        cached_trx_ = NULL;
        /* We reset trx_registered_ for autocommit, the txn will be freed
           in tidesdb_commit and a new one created next time.
           For multi-stmt (BEGIN...COMMIT), keep it true since the txn persists. */
        bool was_autocommit = cached_stmt_shape_valid_
                                  ? cached_is_autocommit_
                                  : !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
        if (was_autocommit) trx_registered_ = false;

        /* We invalidate statement shape cache last so the above checks still
           see it. */
        cached_stmt_shape_valid_ = false;
    }

    DBUG_RETURN(0);
}

THR_LOCK_DATA **ha_tidesdb::store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type)
{
    /* With lock_count()=0 MySQL skips THR_LOCK entirely.
       store_lock is still called for informational purposes but we
       do not push into the 'to' array (same pattern as InnoDB).

       However, we use this callback to detect locking reads
       (SELECT ... FOR UPDATE, SELECT ... IN SHARE MODE) and
       data-modifying statements.  MySQL calls store_lock() before
       external_lock(), so we can set stmt_has_write_lock_ here for
       the pessimistic row lock path.

       InnoDB uses store_lock() to set m_prebuilt->select_lock_type
       to LOCK_S/LOCK_X for these cases.  We emulate this by detecting
       the same lock_type values and setting our write-lock flag.

       We flag locking READS (SELECT ... FOR UPDATE, SELECT ... IN
       SHARE MODE) so the pessimistic row lock path in index_read_map()
       acquires locks for serialization.

       SELECT ... FOR UPDATE passes lock_type >= TL_FIRST_WRITE.
       SELECT ... IN SHARE MODE passes TL_READ_WITH_SHARED_LOCKS.

       Inside stored procedures, thd_sql_command() returns SQLCOM_CALL
       (not SQLCOM_SELECT), so we cannot filter by SQL command.
       Instead we use the lock_type directly.  This means UPDATE/DELETE
       statements also set stmt_has_write_lock_=true, but that's OK
       because we removed lock acquisition from update_row()/delete_row()
       - only index_read_map() acquires pessimistic locks now, and only
       for PK exact matches (the SELECT ... FOR UPDATE pattern). */
    if (lock_type == TL_READ_WITH_SHARED_LOCKS || lock_type >= TL_FIRST_WRITE)
    {
        stmt_has_write_lock_ = true;
    }
    return to;
}

/* ******************** Online DDL ******************** */

/*
  Classify ALTER TABLE operations into INSTANT / INPLACE / COPY.

  INSTANT     metadata-only changes (.frm rewrite, no engine work):
              rename column/index, change default, change table options,
              ADD COLUMN, DROP COLUMN (row format is self-describing via
              the ROW_HEADER_MAGIC header written by serialize_row)
  INPLACE     add/drop secondary indexes (create/drop CFs, populate)
  COPY        column type changes, PK changes
*/
enum_alter_inplace_result ha_tidesdb::check_if_supported_inplace_alter(
    TABLE *altered_table [[maybe_unused]], Alter_inplace_info *ha_alter_info)
{
    DBUG_ENTER("ha_tidesdb::check_if_supported_inplace_alter");

    Alter_inplace_info::HA_ALTER_FLAGS flags = ha_alter_info->handler_flags;

    /* Pure-metadata operations -- INSTANT (no data rewrite, no index work).
       Our row format carries its own field_count + null_bytes header, so
       deserialize_row can adapt to schema changes by back-filling missing
       trailing columns from table->s->default_values.
       DROP COLUMN is included here ONLY for the trailing-drop case --
       that's safe because deserialize_row's unpack loop runs MIN(stored,
       current) iterations and silently ignores extra trailing bytes from
       the old format. Mid-column drops would shift remaining field
       positions and need per-row schema versioning we don't have, so we
       downgrade those to ALGORITHM=COPY below. */
    static const Alter_inplace_info::HA_ALTER_FLAGS TIDESDB_INSTANT =
        Alter_inplace_info::ALTER_COLUMN_NAME |
        Alter_inplace_info::ALTER_COLUMN_DEFAULT |
        Alter_inplace_info::CHANGE_CREATE_OPTION |
        Alter_inplace_info::DROP_CHECK_CONSTRAINT |
        Alter_inplace_info::ALTER_VIRTUAL_GCOL_EXPR |
        Alter_inplace_info::ALTER_RENAME |
        Alter_inplace_info::RENAME_INDEX |
        Alter_inplace_info::CHANGE_INDEX_OPTION |
        Alter_inplace_info::ADD_COLUMN |
        Alter_inplace_info::DROP_COLUMN |
        Alter_inplace_info::ALTER_STORED_COLUMN_ORDER |
        Alter_inplace_info::ALTER_VIRTUAL_COLUMN_ORDER;

    /* Operations we can do inplace (add/drop secondary indexes). */
    static const Alter_inplace_info::HA_ALTER_FLAGS TIDESDB_INPLACE_INDEX =
        Alter_inplace_info::ADD_INDEX |
        Alter_inplace_info::DROP_INDEX |
        Alter_inplace_info::ADD_UNIQUE_INDEX |
        Alter_inplace_info::DROP_UNIQUE_INDEX |
        Alter_inplace_info::ADD_SPATIAL_INDEX;

    /* Pure-INSTANT (data-dictionary update only) */
    if (!(flags & ~TIDESDB_INSTANT))
    {
        /* Special-case DROP COLUMN: only safe as INSTANT when ALL dropped
           columns are at the END of the table -- otherwise field positions
           shift and rows written under the old schema unpack into the wrong
           field slots. Detect this by comparing the first N field names of
           the altered table against the original; any mismatch means a
           non-trailing column was removed and we must rebuild via COPY. */
        if (flags & Alter_inplace_info::DROP_COLUMN)
        {
            for (uint i = 0; i < altered_table->s->fields; i++)
            {
                if (i >= table->s->fields ||
                    strcmp(altered_table->field[i]->field_name,
                           table->field[i]->field_name) != 0)
                {
                    ha_alter_info->unsupported_reason =
                        "TidesDB INSTANT DROP COLUMN supports trailing columns only; "
                        "use ALGORITHM=COPY for non-trailing drops";
                    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
                }
            }
        }
        DBUG_RETURN(HA_ALTER_INPLACE_INSTANT);
    }

    /* INSTANT + secondary-index changes -- INPLACE with shared lock.
       Concurrent reads OK; concurrent writes blocked while the index
       population scan runs. PK changes still require ALGORITHM=COPY. */
    if (!(flags & ~(TIDESDB_INSTANT | TIDESDB_INPLACE_INDEX)))
    {
        if (flags & (Alter_inplace_info::ADD_PK_INDEX |
                     Alter_inplace_info::DROP_PK_INDEX))
        {
            ha_alter_info->unsupported_reason =
                "TidesDB cannot change PRIMARY KEY inplace";
            DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
        }
        DBUG_RETURN(HA_ALTER_INPLACE_SHARED_LOCK);
    }

    /* Everything else falls back to ALGORITHM=COPY. */
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
}

/*
  Create CFs for newly added indexes.
  Called with shared MDL lock (concurrent DML is allowed).
*/
bool ha_tidesdb::prepare_inplace_alter_table(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info,
    const dd::Table *old_table_def [[maybe_unused]],
    dd::Table *new_table_def [[maybe_unused]])
{
    DBUG_ENTER("ha_tidesdb::prepare_inplace_alter_table");

    ha_tidesdb_inplace_ctx *ctx;
    try
    {
        ctx = new ha_tidesdb_inplace_ctx();
    }
    catch (...)
    {
        DBUG_RETURN(true);
    }
    ha_alter_info->handler_ctx = ctx;

    tidesdb_column_family_config_t cfg = build_cf_config(TDB_TABLE_OPTIONS(table));

    std::string base_cf = share->cf_name;

    /* We create CFs for added indexes */
    if (ha_alter_info->index_add_count > 0)
    {
        for (uint a = 0; a < ha_alter_info->index_add_count; a++)
        {
            uint key_num = ha_alter_info->index_add_buffer[a];
            KEY *new_key = &ha_alter_info->key_info_buffer[key_num];

            /* We skip PK -- shouldn't happen (blocked in check_if_supported) */
            if (new_key->flags & HA_NOSAME &&
                altered_table->s->primary_key < altered_table->s->keys &&
                key_num == altered_table->s->primary_key)
                continue;

            std::string idx_cf = base_cf + CF_INDEX_INFIX + new_key->name;

            /* We drop stale CF if it exists from a previous failed ALTER */
            tidesdb_drop_column_family(tdb_get_engine(), idx_cf.c_str());

            tidesdb_column_family_config_t idx_cfg = cfg;
            ha_index_option_struct *iopts = TDB_INDEX_OPTIONS(new_key);
            if (iopts) idx_cfg.use_btree = iopts->use_btree ? 1 : 0;

            int rc = tidesdb_create_column_family(tdb_get_engine(), idx_cf.c_str(), &idx_cfg);
            if (rc != TDB_SUCCESS)
            {
                sql_print_error("[TIDESDB] inplace ADD INDEX: failed to create CF '%s' (err=%d)",
                                idx_cf.c_str(), rc);
                my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] failed to create index CF");
                DBUG_RETURN(true);
            }

            tidesdb_column_family_t *icf = tidesdb_get_column_family(tdb_get_engine(), idx_cf.c_str());
            if (!icf)
            {
                sql_print_error("[TIDESDB] inplace ADD INDEX: CF '%s' not found after create",
                                idx_cf.c_str());
                my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] index CF not found after create");
                DBUG_RETURN(true);
            }

            ctx->add_cfs.push_back(icf);
            ctx->add_cf_names.push_back(idx_cf);
            ctx->add_key_nums.push_back(key_num);
        }
    }

    /* We record CF names to drop for removed indexes */
    if (ha_alter_info->index_drop_count > 0)
    {
        for (uint d = 0; d < ha_alter_info->index_drop_count; d++)
        {
            KEY *old_key = ha_alter_info->index_drop_buffer[d];
            /* We find the key number in the old table */
            uint old_key_num = (uint)(old_key - table->key_info);
            if (old_key_num < share->idx_cf_names.size() &&
                !share->idx_cf_names[old_key_num].empty())
            {
                ctx->drop_cf_names.push_back(share->idx_cf_names[old_key_num]);
            }
        }
    }

    DBUG_RETURN(false);
}

/*
  Inplace phase -- we populate newly added indexes by scanning the table.
  Called with no MDL lock blocking (HA_ALTER_INPLACE_NO_LOCK).
*/
bool ha_tidesdb::inplace_alter_table(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info,
    const dd::Table *old_table_def [[maybe_unused]],
    dd::Table *new_table_def [[maybe_unused]])
{
    DBUG_ENTER("ha_tidesdb::inplace_alter_table");

    ha_tidesdb_inplace_ctx *ctx = static_cast<ha_tidesdb_inplace_ctx *>(ha_alter_info->handler_ctx);

    if (!ctx || ctx->add_cfs.empty())
        DBUG_RETURN(false); /* Nothing to populate (drop-only or instant) */

    /* We mark all columns readable on the altered table since we read
       fields via make_sort_key_part during index key construction. */
    my_bitmap_map *old_map = tmp_use_all_columns(altered_table, altered_table->read_set);

    /* We do a full table scan to populate the new secondary indexes.
       We use the altered_table's key_info for building index keys,
       since that matches the new key numbering. */

    /* We always use READ_COMMITTED for index population.  The scan reads
       potentially millions of rows; higher isolation levels would track
       each key in the read-set, causing unbounded memory growth.  Index
       builds are DDL and never need OCC conflict detection. */
    tidesdb_txn_t *txn = NULL;
    int rc = tidesdb_txn_begin_with_isolation(tdb_get_engine(), TDB_ISOLATION_READ_COMMITTED, &txn);
    if (rc != TDB_SUCCESS || !txn)
    {
        sql_print_error("[TIDESDB] inplace ADD INDEX: txn_begin failed (err=%d)", rc);
        my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] failed to begin txn for index build");
        tmp_restore_column_map(altered_table->read_set, old_map);
        DBUG_RETURN(true);
    }

    tidesdb_iter_t *iter = NULL;
    rc = tidesdb_iter_new(txn, share->cf, &iter);
    if (rc != TDB_SUCCESS || !iter)
    {
        tidesdb_txn_free(txn);
        sql_print_error("[TIDESDB] inplace ADD INDEX: iter_new failed (err=%d)", rc);
        my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] failed to create iterator for index build");
        tmp_restore_column_map(altered_table->read_set, old_map);
        DBUG_RETURN(true);
    }
    tidesdb_iter_seek_to_first(iter);

    ha_rows rows_processed = 0;

    /* For UNIQUE indexes, we track seen index-column prefixes to detect
       duplicates.  If a duplicate is found we must abort the ALTER.
       unordered_set gives O(1) amortized lookup vs O(log n) for std::set,
       which matters for tables with millions of rows. */
    std::vector<bool> idx_is_unique(ctx->add_cfs.size(), false);
    std::vector<std::unordered_set<std::string>> idx_seen(ctx->add_cfs.size());
    for (uint a = 0; a < ctx->add_cfs.size(); a++)
    {
        uint key_num = ctx->add_key_nums[a];
        KEY *ki = &altered_table->key_info[key_num];
        if (ki->flags & HA_NOSAME) idx_is_unique[a] = true;
    }

    /* We remember the last data key so we can seek directly to it after
       a batch commit (O(n²)). */
    uchar last_data_key[DATA_KEY_BUF_LEN];
    size_t last_data_key_len = 0;

    while (tidesdb_iter_valid(iter))
    {
        uint8_t *key_data = NULL;
        size_t key_size = 0;
        uint8_t *val_data = NULL;
        size_t val_size = 0;

        if (tidesdb_iter_key(iter, &key_data, &key_size) != TDB_SUCCESS ||
            tidesdb_iter_value(iter, &val_data, &val_size) != TDB_SUCCESS)
        {
            tidesdb_iter_next(iter);
            continue;
        }

        /* We skip non-data keys (meta namespace) */
        if (key_size < KEY_NAMESPACE_LEN || key_data[0] != KEY_NS_DATA)
        {
            tidesdb_iter_next(iter);
            continue;
        }

        /* We save this data key for potential batch re-seek */
        if (key_size <= sizeof(last_data_key))
        {
            memcpy(last_data_key, key_data, key_size);
            last_data_key_len = key_size;
        }

        /* We extract PK from the data key (skip KEY_NS_DATA prefix) */
        const uchar *pk = key_data + KEY_NAMESPACE_LEN;
        uint pk_len = (uint)(key_size - KEY_NAMESPACE_LEN);

        /* We decode the row into table->record[0].  The field pointers from
           altered_table->key_info will be temporarily repointed (via
           move_field_offset) to read from this buffer. */
        if (share->has_blobs || share->encrypted)
        {
            std::string row_data((const char *)val_data, val_size);
            deserialize_row(table->record[0], row_data);
        }
        else
        {
            deserialize_row(table->record[0], (const uchar *)val_data, val_size);
        }

        /* For each newly added index, build the index entry key.
           altered_table->key_info fields have ptr into altered_table->record[0],
           but the data lives in table->record[0].  We compute ptdiff to
           rebase field pointers to read from the correct buffer.
           Key format matches make_comparable_key()= [null_byte] + sort_string. */
        my_ptrdiff_t ptdiff = (my_ptrdiff_t)(table->record[0] - altered_table->record[0]);

        for (uint a = 0; a < ctx->add_cfs.size(); a++)
        {
            uint key_num = ctx->add_key_nums[a];
            KEY *ki = &altered_table->key_info[key_num];

            /* FULLTEXT and SPATIAL indexes use different population paths */
            if (is_fts_index(ki)) continue;
            if (is_spatial_index(ki)) continue;

            uchar ik[SEC_IDX_KEY_BUF_LEN];
            uint pos = 0;
            for (uint p = 0; p < ki->user_defined_key_parts; p++)
            {
                KEY_PART_INFO *kp = &ki->key_part[p];
                Field *field = kp->field;

                field->move_field_offset(ptdiff);
                if (field->is_nullable())
                {
                    if (field->is_null())
                    {
                        ik[pos++] = SORT_KEY_NULL;
                        bzero(ik + pos, kp->length);
                        pos += kp->length;
                        field->move_field_offset(-ptdiff);
                        continue;
                    }
                    ik[pos++] = SORT_KEY_NOT_NULL;
                }
                memset(ik + pos, 0, kp->length); field->make_sort_key(ik + pos, kp->length);
                field->move_field_offset(-ptdiff);
                pos += kp->length;
            }

            /* We check UNIQUE constraint before inserting */
            if (idx_is_unique[a])
            {
                std::string prefix((const char *)ik, pos);
                if (!idx_seen[a].insert(prefix).second)
                {
                    /* We found a duplicate -- abort the ALTER */
                    tidesdb_iter_free(iter);
                    tidesdb_txn_rollback(txn);
                    tidesdb_txn_free(txn);
                    tmp_restore_column_map(altered_table->read_set, old_map);
                    my_error(ER_DUP_ENTRY, MYF(0), "?", altered_table->key_info[key_num].name);
                    DBUG_RETURN(true);
                }
            }

            /* We append PK to make the key unique */
            memcpy(ik + pos, pk, pk_len);
            pos += pk_len;

            rc = tidesdb_txn_put(txn, ctx->add_cfs[a], ik, pos, &tdb_empty_val,
                                 sizeof(tdb_empty_val), TIDESDB_TTL_NONE);
            if (rc != TDB_SUCCESS)
            {
                sql_print_error("[TIDESDB] inplace ADD INDEX: put failed for key %u (err=%d)",
                                key_num, rc);
                /* Continue, best effort from here! */
            }
        }

        rows_processed++;

        /* We check for KILL signal periodically so the user can cancel
           long-running index builds via KILL <thread_id>. */
        if ((rows_processed % TIDESDB_INDEX_BUILD_BATCH) == 0 && thd_killed(ha_thd()))
        {
            tidesdb_iter_free(iter);
            tidesdb_txn_rollback(txn);
            tidesdb_txn_free(txn);
            tmp_restore_column_map(altered_table->read_set, old_map);
            my_error(ER_QUERY_INTERRUPTED, MYF(0));
            DBUG_RETURN(true);
        }

        if (rows_processed % TIDESDB_INDEX_BUILD_BATCH == 0)
        {
            {
                int crc = tidesdb_txn_commit(txn);
                if (crc != TDB_SUCCESS)
                {
                    sql_print_warning("[TIDESDB] inplace ADD INDEX: batch commit failed rc=%d",
                                      crc);
                    tidesdb_txn_rollback(txn);
                }
            }
            tidesdb_iter_free(iter);

            /* We reset the txn with READ_COMMITTED -- index builds
               don't need snapshot consistency across batches. */
            int rrc = tidesdb_txn_reset(txn, TDB_ISOLATION_READ_COMMITTED);
            if (rrc != TDB_SUCCESS)
            {
                /* We reset failed -- fall back to free+begin */
                sql_print_warning(
                    "[TIDESDB] inplace ADD INDEX: tidesdb_txn_reset failed (rc=%d), "
                    "falling back to free+begin",
                    rrc);
                tidesdb_txn_free(txn);
                txn = NULL;
                rc = tidesdb_txn_begin_with_isolation(tdb_get_engine(), TDB_ISOLATION_READ_COMMITTED,
                                                      &txn);
                if (rc != TDB_SUCCESS || !txn)
                {
                    sql_print_error("[TIDESDB] inplace ADD INDEX: batch txn_begin failed");
                    my_error(ER_INTERNAL_ERROR, MYF(0),
                             "[TIDESDB] batch txn failed during index build");
                    tmp_restore_column_map(altered_table->read_set, old_map);
                    DBUG_RETURN(true);
                }
            }
            iter = NULL;
            rc = tidesdb_iter_new(txn, share->cf, &iter);
            if (rc != TDB_SUCCESS || !iter)
            {
                tidesdb_txn_free(txn);
                my_error(ER_INTERNAL_ERROR, MYF(0),
                         "[TIDESDB] batch iter failed during index build");
                tmp_restore_column_map(altered_table->read_set, old_map);
                DBUG_RETURN(true);
            }
            /* We seek directly to the last processed key and advance past it */
            int src = tidesdb_iter_seek(iter, last_data_key, last_data_key_len);
            if (src != TDB_SUCCESS)
            {
                sql_print_warning("[TIDESDB] inplace ADD INDEX: iter_seek failed rc=%d", src);
                break; /* end scan gracefully */
            }
            if (tidesdb_iter_valid(iter)) tidesdb_iter_next(iter);
            continue; /* Don't call iter_next again */
        }

        tidesdb_iter_next(iter);
    }

    tidesdb_iter_free(iter);

    /* We commit remaining entries */
    rc = tidesdb_txn_commit(txn);
    if (rc != TDB_SUCCESS) tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);

    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] inplace ADD INDEX: final commit failed (err=%d)", rc);
        my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] final commit failed during index build");
        tmp_restore_column_map(altered_table->read_set, old_map);
        DBUG_RETURN(true);
    }
    tmp_restore_column_map(altered_table->read_set, old_map);
    DBUG_RETURN(false);
}

/*
  Commit or rollback the inplace ALTER.
  On commit       drop old index CFs, update share->idx_cfs for new table shape.
  On rollback     drop newly created CFs.
*/
bool ha_tidesdb::commit_inplace_alter_table(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info,
    bool commit,
    const dd::Table *old_table_def [[maybe_unused]],
    dd::Table *new_table_def [[maybe_unused]])
{
    DBUG_ENTER("ha_tidesdb::commit_inplace_alter_table");

    ha_tidesdb_inplace_ctx *ctx = static_cast<ha_tidesdb_inplace_ctx *>(ha_alter_info->handler_ctx);

    ha_alter_info->group_commit_ctx = NULL;

    if (!ctx) DBUG_RETURN(false);

    /* We free any cached iterators before dropping CFs.  The connection's
       scan_iter and dup_iter_cache_ may hold merge-heap references to
       SSTables in CFs about to be dropped. */
    if (scan_iter)
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    free_dup_iter_cache();

    if (!commit)
    {
        /* Rollback, we drop any CFs we created for new indexes */
        for (const auto &cf_name : ctx->add_cf_names)
            tidesdb_drop_column_family(tdb_get_engine(), cf_name.c_str());
        DBUG_RETURN(false);
    }

    /* Commit, we drop CFs for removed indexes */
    for (const auto &cf_name : ctx->drop_cf_names)
    {
        int rc = tidesdb_drop_column_family(tdb_get_engine(), cf_name.c_str());
        if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
            sql_print_warning("[TIDESDB] commit ALTER: failed to drop CF '%s' (err=%d)",
                              cf_name.c_str(), rc);
    }

    /* We rebuild share->idx_cfs and idx_cf_names based on the new table's keys.
       Since we hold exclusive MDL, no other handler is using the share. */
    lock_shared_ha_data();
    share->idx_cfs.clear();
    share->idx_cf_names.clear();

    uint new_pk = altered_table->s->primary_key;
    for (uint i = 0; i < altered_table->s->keys; i++)
    {
        if (new_pk != MAX_KEY && i == new_pk)
        {
            share->idx_cfs.push_back(NULL);
            share->idx_cf_names.push_back("");
            continue;
        }
        std::string idx_name;
        tidesdb_column_family_t *icf = resolve_idx_cf(
            tdb_get_engine(), share->cf_name, altered_table->key_info[i].name, idx_name);
        share->idx_cfs.push_back(icf);
        share->idx_cf_names.push_back(idx_name);
    }

    /* We recompute cached index metadata for the new table shape */
    for (uint i = 0; i < altered_table->s->keys; i++)
    {
        share->idx_comp_key_len[i] = comparable_key_length(&altered_table->key_info[i]);
        share->idx_is_fts[i] = is_fts_index(&altered_table->key_info[i]);
        share->idx_is_spatial[i] = is_spatial_index(&altered_table->key_info[i]);
    }

    /* We repopulate coverage bitmaps for the new table shape. */
    share->idx_cover.assign(altered_table->s->keys,
                            std::vector<bool>(altered_table->s->fields, false));
    for (uint i = 0; i < altered_table->s->keys; i++)
    {
        const KEY *ki = &altered_table->key_info[i];
        for (uint p = 0; p < ki->user_defined_key_parts; p++)
        {
            uint fnr = ki->key_part[p].fieldnr;
            if (fnr > 0 && fnr - 1 < altered_table->s->fields) share->idx_cover[i][fnr - 1] = true;
        }
        if (altered_table->s->primary_key != MAX_KEY && i != altered_table->s->primary_key)
        {
            const KEY *pk_key = &altered_table->key_info[altered_table->s->primary_key];
            for (uint p = 0; p < pk_key->user_defined_key_parts; p++)
            {
                uint fnr = pk_key->key_part[p].fieldnr;
                if (fnr > 0 && fnr - 1 < altered_table->s->fields)
                    share->idx_cover[i][fnr - 1] = true;
            }
        }
    }
    share->num_secondary_indexes = 0;
    for (uint i = 0; i < share->idx_cfs.size(); i++)
        if (share->idx_cfs[i]) share->num_secondary_indexes++;

    /* If table options changed (SYNC_MODE, COMPRESSION, BLOOM_FPR, etc.),
       we apply them to the live CF(s) so they take effect immediately instead
       of only being persisted in the .frm.

       ENGINE_ATTRIBUTE-freeze fix: compute the new options FRESH from
       altered_table's engine_attribute, bypassing the share's cached
       opts pointer entirely (the cache currently holds the OLD parse).
       Using a local struct lets us:
         (1) feed accurate values into build_cf_config and the share
             field updates below, and
         (2) atomic-swap into share->cached_opts at the end so that
             future TDB_TABLE_OPTIONS(table) calls -- after this alter
             returns -- see the new values rather than the pre-alter
             cache.
       Without this, ALTER TABLE ... ENGINE_ATTRIBUTE='{...}' appeared
       to succeed (DD updated, SHOW CREATE TABLE returns the new value)
       but the engine kept using the old values for the share's life. */
    if (ha_alter_info->handler_flags & ALTER_CHANGE_CREATE_OPTION)
    {
        ha_table_option_struct fresh_opts{};
        tidesdb_compute_opts_for_table(altered_table, &fresh_opts);
        tidesdb_column_family_config_t cfg = build_cf_config(&fresh_opts);

        /* Main data CF */
        if (share->cf)
        {
            int rc = tidesdb_cf_update_runtime_config(share->cf, &cfg, 1);
            if (rc != TDB_SUCCESS)
                sql_print_information(
                    "[TIDESDB] ALTER: failed to update runtime config for "
                    "data CF '%s' (err=%d)",
                    share->cf_name.c_str(), rc);
        }

        /* We apply per-index USE_BTREE overrides */
        for (uint i = 0; i < share->idx_cfs.size(); i++)
        {
            if (share->idx_cfs[i])
            {
                tidesdb_column_family_config_t idx_cfg = cfg;
                if (i < altered_table->s->keys && TDB_INDEX_OPTIONS(&altered_table->key_info[i]))
                {
                    ha_index_option_struct *iopts = TDB_INDEX_OPTIONS(&altered_table->key_info[i]);
                    idx_cfg.use_btree = iopts->use_btree ? 1 : 0;
                }

                int rc = tidesdb_cf_update_runtime_config(share->idx_cfs[i], &idx_cfg, 1);
                if (rc != TDB_SUCCESS)
                    sql_print_information(
                        "[TIDESDB] ALTER: failed to update runtime config for "
                        "index CF '%s' (err=%d)",
                        share->idx_cf_names[i].c_str(), rc);
            }
        }

        /* We update share-level cached options that are read from table
           options. Source: the freshly-computed `fresh_opts` (above)
           rather than TDB_TABLE_OPTIONS(altered_table) -- the latter
           might still hit the share's old cached pointer depending on
           whether altered_table->s->ha_share is set. */
        {
            uint iso_idx = fresh_opts.isolation_level;
            if (iso_idx < array_elements(tdb_isolation_map))
                share->isolation_level = (tidesdb_isolation_level_t)tdb_isolation_map[iso_idx];
            share->default_ttl = fresh_opts.ttl;
            share->has_ttl = (share->default_ttl > 0 || share->ttl_field_idx >= 0);
            /* Encryption changes mid-table aren't safe to flip via inplace
               alter -- existing rows are already encrypted (or not) with
               the previous setting. We update the share for new-row
               behavior consistency with the persisted DD, but the on-disk
               state may now mix old and new rows. Document; a stricter
               implementation would force COPY for encrypted-flag changes. */
            share->encrypted = fresh_opts.encrypted;
            if (share->encrypted)
                share->encryption_key_id = (uint)fresh_opts.encryption_key_id;
        }

        /* ENGINE_ATTRIBUTE-freeze fix: atomically replace share->cached_opts
           with the freshly-computed values so subsequent TDB_TABLE_OPTIONS
           readers (any connection that still holds this share open) see
           the post-alter view rather than the pre-alter cache. We hold
           lock_shared_ha_data, so no concurrent reader can be racing
           against the swap -- but using exchange() preserves the
           memory-ordering contract for the lock-free fast path. */
        ha_table_option_struct *new_cached = new ha_table_option_struct(fresh_opts);
        ha_table_option_struct *old_cached =
            share->cached_opts.exchange(new_cached, std::memory_order_acq_rel);
        delete old_cached;
    }

    /* We force a stats refresh on next info() call */
    share->stats_refresh_us.store(0, std::memory_order_relaxed);
    unlock_shared_ha_data();

    /* Refresh the schema-CF entry after ALTER. (Originally cached .frm
       bytes for MariaDB discover_table; under MySQL it just records the
       table name so DROP TABLE can find the CF.) */
    schema_cf_store_frm(table->s->path.str);

    DBUG_RETURN(false);
}

/*
  Tell MySQL whether changing table options requires a full table rebuild.
  For TidesDB, option changes (SYNC_MODE, TTL, etc.) are compatible at the
  data level -- no row rewrite is needed; the new options take effect on
  next open().
*/
bool ha_tidesdb::check_if_incompatible_data(HA_CREATE_INFO *create_info, uint table_changes)
{
    /* If only table options changed (not column types), data is compatible */
    if (table_changes == IS_EQUAL_YES) return COMPATIBLE_DATA_YES;
    return COMPATIBLE_DATA_NO;
}

/* ******************** rename_table (ALTER TABLE / RENAME) ******************** */

int ha_tidesdb::rename_table(const char *from, const char *to, const dd::Table *, dd::Table *)
{
    DBUG_ENTER("ha_tidesdb::rename_table");

    std::string old_cf = path_to_cf_name(from);
    std::string new_cf = path_to_cf_name(to);

    /* If the destination CF already exists (stale from a previous ALTER),
       drop it first so the rename can proceed. */
    tidesdb_drop_column_family(tdb_get_engine(), new_cf.c_str());

    int rc = tidesdb_rename_column_family(tdb_get_engine(), old_cf.c_str(), new_cf.c_str());
    if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
    {
        sql_print_error("[TIDESDB] Failed to rename CF '%s' -> '%s' (err=%d)", old_cf.c_str(),
                        new_cf.c_str(), rc);
        DBUG_RETURN(tdb_rc_to_ha(rc, "rename_table"));
    }

    /* We rename secondary index CFs by enumerating all CFs with the old prefix. */
    {
        std::string prefix = old_cf + CF_INDEX_INFIX;
        char **names = NULL;
        int count = 0;
        if (tidesdb_list_column_families(tdb_get_engine(), &names, &count) == TDB_SUCCESS && names)
        {
            for (int i = 0; i < count; i++)
            {
                if (!names[i]) continue;
                std::string cf_str(names[i]);
                if (cf_str.compare(0, prefix.size(), prefix) == 0)
                {
                    /* We replace old table prefix with new one */
                    std::string suffix = cf_str.substr(prefix.size());
                    std::string new_idx = new_cf + CF_INDEX_INFIX + suffix;

                    tidesdb_drop_column_family(tdb_get_engine(), new_idx.c_str());
                    rc = tidesdb_rename_column_family(tdb_get_engine(), cf_str.c_str(), new_idx.c_str());
                    if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
                        sql_print_error("[TIDESDB] Failed to rename idx CF '%s' -> '%s' (err=%d)",
                                        cf_str.c_str(), new_idx.c_str(), rc);
                }
                free(names[i]);
            }
            free(names);
        }
    }

    /* We update schema CF entry for the renamed table */
    schema_cf_rename(from, to);

    DBUG_RETURN(0);
}

/* ******************** delete_table (DROP TABLE) ******************** */

/*
  Force-remove a directory tree from disk.  Used as a safety net after
  tidesdb_drop_column_family() because the library's internal
  remove_directory() can fail silently (e.g. open fds from block cache,
  mmap, or background workers).  If stale SSTables survive, the next
  CREATE TABLE with the same name inherits them -- catastrophic for
  performance (bloom filters pass on every SSTable since keys overlap).
*/
static void force_remove_cf_dir(const std::string &cf_name)
{
    char dir[FN_REFLEN];
    const char sep[] = {FN_LIBCHAR, 0};
    strxnmov(dir, sizeof(dir) - 1, g_engine_ctx.path.c_str(), sep, cf_name.c_str(), NullS);

    MY_STAT st;
    if (!my_stat(dir, &st, MYF(0))) return; /* already gone */

    /* Recursive removal via nftw(); see my_rmtree() in tidesdb_compat.h. */
    if (my_rmtree(dir, MYF(0)) != 0)
        sql_print_warning("[TIDESDB] force_remove_cf_dir failed for %s", dir);
}

/*
  Shared drop logic used by both the handlerton callback (hton->drop_table)
  and the handler method (ha_tidesdb::delete_table).  Drops the main data CF
  and all secondary index CFs, then force-removes their directories.
  Returns 0 on success.
*/
static int tidesdb_drop_table_impl(const char *path)
{
    if (!tdb_get_engine()) return 0;

    std::string cf_name = ha_tidesdb::path_to_cf_name(path);

    /* We collect secondary index CF names before dropping so we can
       force-remove their directories afterwards. */
    std::vector<std::string> idx_cf_names;
    {
        std::string prefix = cf_name + CF_INDEX_INFIX;
        char **names = NULL;
        int count = 0;
        if (tidesdb_list_column_families(tdb_get_engine(), &names, &count) == TDB_SUCCESS && names)
        {
            for (int i = 0; i < count; i++)
            {
                if (!names[i]) continue;
                if (strncmp(names[i], prefix.c_str(), prefix.size()) == 0)
                    idx_cf_names.push_back(names[i]);
                free(names[i]);
            }
            free(names);
        }
    }

    int rc = tidesdb_drop_column_family(tdb_get_engine(), cf_name.c_str());
    if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
    {
        sql_print_error("[TIDESDB] Failed to drop CF '%s' (err=%d)", cf_name.c_str(), rc);
        return rc;
    }

    for (const auto &idx_name : idx_cf_names)
        tidesdb_drop_column_family(tdb_get_engine(), idx_name.c_str());

    /* We force-remove CF directories from disk in case the
       library's internal remove_directory() failed silently.  Without
       this, the next CREATE TABLE reuses the stale directory and
       inherits all old SSTables -- making reads scan 100s of SSTables
       with overlapping keys (bloom filters useless). */
    force_remove_cf_dir(cf_name);
    for (const auto &idx_name : idx_cf_names) force_remove_cf_dir(idx_name);

    /* We remove .frm from schema CF */
    schema_cf_delete(path);

    return 0;
}

/*
  Handlerton-level drop_table callback. Dead code under MySQL: the hook
  exists in MariaDB 12.x (hton->drop_table) but the callback registration
  below is commented out, so MySQL always routes drops through
  ha_tidesdb::delete_table() at the handler level. Kept compiled-in only
  to ease a future MariaDB compatibility build.
*/
static int tidesdb_hton_drop_table(handlerton *, const char *path)
{
    return tidesdb_drop_table_impl(path);
}

/*
  Extract the database name from a directory path handed to drop_database.
  The server passes something like "./test/" or "/var/lib/mysql/test/";
  we strip trailing separators and return the final path component.
*/
static std::string tidesdb_path_to_db_name(const char *path)
{
    if (!path) return std::string();
    std::string p(path);
    while (!p.empty() && (p.back() == FN_LIBCHAR || p.back() == '/')) p.pop_back();
    size_t slash = p.find_last_of("/\\");
    if (slash != std::string::npos) p = p.substr(slash + 1);
    return p;
}

/*
  Handlerton-level drop_database callback. Invoked when the server-side
  DROP DATABASE has finished removing the database files. Without this
  hook, TidesDB column families belonging to the dropped database (and
  any object-store-mode entries in g_engine_ctx.schema_cf) would outlive the database
  and accumulate on disk.

  We enumerate every CF whose name starts with "<db_name>__" (the prefix
  path_to_cf_name builds for a table in that database -- which also
  captures all "db__tbl__idx_*" secondary-index CFs) and drop each.
*/
static void tidesdb_hton_drop_database(handlerton *, char *path)
{
    if (!tdb_get_engine() || !path) return;

    std::string db = tidesdb_path_to_db_name(path);
    if (db.empty()) return;

    std::string prefix = db + CF_DB_TABLE_SEP;

    std::vector<std::string> to_drop;
    {
        char **names = NULL;
        int count = 0;
        if (tidesdb_list_column_families(tdb_get_engine(), &names, &count) == TDB_SUCCESS && names)
        {
            for (int i = 0; i < count; i++)
            {
                if (!names[i]) continue;
                if (strncmp(names[i], prefix.c_str(), prefix.size()) == 0)
                    to_drop.emplace_back(names[i]);
                free(names[i]);
            }
            free(names);
        }
    }

    for (const auto &cf_name : to_drop)
    {
        int rc = tidesdb_drop_column_family(tdb_get_engine(), cf_name.c_str());
        if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
            sql_print_warning("[TIDESDB] drop_database: failed to drop CF '%s' (err=%d)",
                              cf_name.c_str(), rc);
        force_remove_cf_dir(cf_name);
    }

    /* We clean up schema CF entries for this database (object-store mode).
       No-op when g_engine_ctx.schema_cf is NULL (local-only mode). */
    schema_cf_delete_db(db);

    sql_print_information("[TIDESDB] drop_database: removed %zu column famil%s for '%s'",
                          to_drop.size(), to_drop.size() == 1 ? "y" : "ies", db.c_str());
}

int ha_tidesdb::delete_table(const char *name, const dd::Table *)
{
    DBUG_ENTER("ha_tidesdb::delete_table");
    DBUG_RETURN(tidesdb_drop_table_impl(name));
}

/* ******************** Status variables (SHOW GLOBAL STATUS LIKE 'tidesdb%') ********************
 */

/* Static holders for status variable values.  Populated by the SHOW_FUNC
   callback which queries tidesdb_get_db_stats / tidesdb_get_cache_stats.
   These are global (not per-connection) since they reflect database-wide state. */
static long long srv_stat_column_families;
static long long srv_stat_global_seq;
static long long srv_stat_memtable_bytes;
static long long srv_stat_txn_memory_bytes;
static long long srv_stat_memory_limit;
static long long srv_stat_memory_pressure;
static long long srv_stat_total_sstables;
static long long srv_stat_open_sstables;
static long long srv_stat_data_size_bytes;
static long long srv_stat_immutable_memtables;
static long long srv_stat_flush_pending;
static long long srv_stat_flush_queue;
static long long srv_stat_compaction_queue;
static long long srv_stat_cache_entries;
static long long srv_stat_cache_bytes;
static long long srv_stat_cache_hits;
static long long srv_stat_cache_misses;
static double srv_stat_cache_hit_rate;
static long long srv_stat_cache_partitions;
/* Tombstone aggregates are forward-declared near the top of this file so
   tidesdb_show_status can read them directly.  Their definitions live up
   there. */

#define TIDESQL_VERSION_STR "4.4.0"
#define TIDESQL_VERSION_HEX 0x40400

static const char *srv_stat_version = TIDESQL_VERSION_STR;
static long long srv_stat_version_hex = TIDESQL_VERSION_HEX;

static SHOW_VAR tidesdb_status_variables[] = {
    {"tidesdb_version", (char *)&srv_stat_version, SHOW_CHAR_PTR},
    {"tidesdb_version_hex", (char *)&srv_stat_version_hex, SHOW_LONGLONG},
    {"tidesdb_column_families", (char *)&srv_stat_column_families, SHOW_LONGLONG},
    {"tidesdb_global_sequence", (char *)&srv_stat_global_seq, SHOW_LONGLONG},
    {"tidesdb_memtable_bytes", (char *)&srv_stat_memtable_bytes, SHOW_LONGLONG},
    {"tidesdb_txn_memory_bytes", (char *)&srv_stat_txn_memory_bytes, SHOW_LONGLONG},
    {"tidesdb_memory_limit", (char *)&srv_stat_memory_limit, SHOW_LONGLONG},
    {"tidesdb_memory_pressure", (char *)&srv_stat_memory_pressure, SHOW_LONGLONG},
    {"tidesdb_total_sstables", (char *)&srv_stat_total_sstables, SHOW_LONGLONG},
    {"tidesdb_open_sstables", (char *)&srv_stat_open_sstables, SHOW_LONGLONG},
    {"tidesdb_data_size_bytes", (char *)&srv_stat_data_size_bytes, SHOW_LONGLONG},
    {"tidesdb_immutable_memtables", (char *)&srv_stat_immutable_memtables, SHOW_LONGLONG},
    {"tidesdb_flush_pending", (char *)&srv_stat_flush_pending, SHOW_LONGLONG},
    {"tidesdb_flush_queue", (char *)&srv_stat_flush_queue, SHOW_LONGLONG},
    {"tidesdb_compaction_queue", (char *)&srv_stat_compaction_queue, SHOW_LONGLONG},
    {"tidesdb_cache_entries", (char *)&srv_stat_cache_entries, SHOW_LONGLONG},
    {"tidesdb_cache_bytes", (char *)&srv_stat_cache_bytes, SHOW_LONGLONG},
    {"tidesdb_cache_hits", (char *)&srv_stat_cache_hits, SHOW_LONGLONG},
    {"tidesdb_cache_misses", (char *)&srv_stat_cache_misses, SHOW_LONGLONG},
    {"tidesdb_cache_hit_rate", (char *)&srv_stat_cache_hit_rate, SHOW_DOUBLE},
    {"tidesdb_cache_partitions", (char *)&srv_stat_cache_partitions, SHOW_LONGLONG},
    {"tidesdb_total_tombstones", (char *)&srv_stat_total_tombstones, SHOW_LONGLONG},
    {"tidesdb_tombstone_ratio", (char *)&srv_stat_tombstone_ratio, SHOW_DOUBLE},
    {"tidesdb_max_sst_tombstone_density", (char *)&srv_stat_max_sst_density, SHOW_DOUBLE},
    {"tidesdb_max_sst_tombstone_density_level", (char *)&srv_stat_max_sst_density_level,
     SHOW_LONGLONG},
    {NullS, NullS, SHOW_LONG}};

/* We refresh status variable values from the library.
   Called from tidesdb_show_status (SHOW ENGINE STATUS) and periodically. */
static void tidesdb_refresh_status_vars()
{
    if (!tdb_get_engine()) return;

    tidesdb_db_stats_t db_st;
    memset(&db_st, 0, sizeof(db_st));
    tidesdb_get_db_stats(tdb_get_engine(), &db_st);

    tidesdb_cache_stats_t cache_st;
    memset(&cache_st, 0, sizeof(cache_st));
    tidesdb_get_cache_stats(tdb_get_engine(), &cache_st);

    srv_stat_column_families = db_st.num_column_families;
    srv_stat_global_seq = (long long)db_st.global_seq;
    srv_stat_memtable_bytes = (long long)db_st.total_memtable_bytes;
    srv_stat_txn_memory_bytes = (long long)db_st.txn_memory_bytes;
    srv_stat_memory_limit = (long long)db_st.resolved_memory_limit;
    srv_stat_memory_pressure = db_st.memory_pressure_level;
    srv_stat_total_sstables = db_st.total_sstable_count;
    srv_stat_open_sstables = db_st.num_open_sstables;
    srv_stat_data_size_bytes = (long long)db_st.total_data_size_bytes;
    srv_stat_immutable_memtables = db_st.total_immutable_count;
    srv_stat_flush_pending = db_st.flush_pending_count;
    srv_stat_flush_queue = (long long)db_st.flush_queue_size;
    srv_stat_compaction_queue = (long long)db_st.compaction_queue_size;
    srv_stat_cache_entries = (long long)cache_st.total_entries;
    srv_stat_cache_bytes = (long long)cache_st.total_bytes;
    srv_stat_cache_hits = (long long)cache_st.hits;
    srv_stat_cache_misses = (long long)cache_st.misses;
    srv_stat_cache_hit_rate = cache_st.hit_rate * PERCENT_SCALE;
    srv_stat_cache_partitions = (long long)cache_st.num_partitions;

    /* Tombstone aggregates in which we walk all CFs once, sum total_tombstones and
       track the worst single-SSTable density.  tidesdb_db_stats_t does not
       expose tombstone aggregates, so we iterate the CF list ourselves.
       SHOW GLOBAL STATUS reads the static atomics, so the cost is paid in
       SHOW ENGINE STATUS / SHOW GLOBAL STATUS callers, not on the write
       path. */
    char **cf_names = NULL;
    int cf_count = 0;
    if (tidesdb_list_column_families(tdb_get_engine(), &cf_names, &cf_count) == TDB_SUCCESS && cf_names)
    {
        uint64_t total_tomb = 0, total_keys = 0;
        double max_density = 0.0;
        int max_density_level = 0;
        for (int i = 0; i < cf_count; i++)
        {
            if (!cf_names[i]) continue;
            tidesdb_column_family_t *cf = tidesdb_get_column_family(tdb_get_engine(), cf_names[i]);
            if (!cf) continue;
            tidesdb_stats_t *st = NULL;
            if (tidesdb_get_stats(cf, &st) == TDB_SUCCESS && st)
            {
                total_tomb += st->total_tombstones;
                total_keys += st->total_keys;
                if (st->max_sst_density > max_density)
                {
                    max_density = st->max_sst_density;
                    max_density_level = st->max_sst_density_level;
                }
                tidesdb_free_stats(st);
            }
        }
        for (int i = 0; i < cf_count; i++) free(cf_names[i]);
        free(cf_names);

        srv_stat_total_tombstones = (long long)total_tomb;
        srv_stat_tombstone_ratio = total_keys > 0 ? (double)total_tomb / (double)total_keys : 0.0;
        srv_stat_max_sst_density = max_density;
        srv_stat_max_sst_density_level = (long long)max_density_level;
    }
}

/* ******************** Plugin declaration ******************** */

static struct st_mysql_storage_engine tidesdb_storage_engine = {MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(tidesdb){MYSQL_STORAGE_ENGINE_PLUGIN,
                              &tidesdb_storage_engine,
                              "TidesDB",
                              "TidesDB",
                              "LSM-tree engine with ACID transactions, MVCC concurrency, "
                              "secondary/spatial/full-text/vector indexes, and encryption",
                              PLUGIN_LICENSE_GPL,
                              tidesdb_init_func,
                              nullptr, /* check_uninstall (MySQL slot) */
                              tidesdb_deinit_func,
                              TIDESQL_VERSION_HEX,
                              tidesdb_status_variables,
                              tidesdb_system_variables,
                              nullptr, /* config options */
                              0 /* flags */} mysql_declare_plugin_end;
