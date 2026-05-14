/* tidesdb_compat.h
 *
 * Compatibility shims that let TideSQL's MariaDB-targeted source compile against
 * MySQL 9.7. Include this BEFORE any MySQL header — most shims are #defines
 * and stub inline functions whose definitions need to win over MySQL's own.
 *
 * Categories covered:
 *   1. MariaDB-only types         (range_id_t, check_result_t, my_bool, my_ptrdiff_t)
 *   2. MariaDB-only flag values   (ALTER_*, CHECK_*, HA_CAN_*, TL_FIRST_WRITE, WARN_LEVEL_*)
 *   3. Encryption API             (declarations here; real impl in
 *                                  plugin/tidesdb_keyring_compat.cc, backed
 *                                  by my_aes_256_cbc + master-key file)
 *   4. Macro renames              (DBUG_ASSERT -> assert)
 *   7. Logging functions          (sql_print_information/warning/error -> stderr)
 *
 * Categories 5 (handler virtual signatures) and 6 (SYS_VAR* renames) are NOT
 * fixable here — they need real edits in ha_tidesdb.{h,cc}.
 *
 * Note: macros that reference MySQL classes (Alter_inplace_info::ADD_PK_INDEX,
 * Sql_condition::SL_NOTE, TL_WRITE) are safe because macros expand at use site,
 * not at definition. By the time TideSQL's code references them, sql/handler.h
 * (and its transitive includes) have provided the symbols.
 */
#pragma once

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

/* ----- Category 1: MariaDB-only types ----- */

/* MariaDB's multi_range_read_next() takes range_id_t*; MySQL takes char**.
 * They're equivalent in practice — both opaque pointers the engine sets for
 * the optimizer to read back. */
using range_id_t = char*;

/* MariaDB enum used as check()/repair() return type. MySQL just uses int with
 * HA_ADMIN_* constants. */
using check_result_t = int;

#ifndef my_bool
using my_bool = bool;
#endif

#ifndef my_ptrdiff_t
using my_ptrdiff_t = ptrdiff_t;
#endif

/* MariaDB cost type used in scan_time() / read_time(). MySQL uses plain double.
 * Stub as a struct that decays to double via implicit conversion so call sites
 * that do `cost.io + cost.cpu` keep compiling. scan_time() already returns a
 * plain double; the struct is kept for the non-virtual cost helpers
 * (keyread_time / rnd_pos_time) that still use it internally. */
struct IO_AND_CPU_COST {
    double io;
    double cpu;
    constexpr operator double() const { return io + cpu; }
    /* TideSQL writes `cost = some_double` where MariaDB's IO_AND_CPU_COST has
     * an implicit conversion-assignment. Mimic by interpreting the scalar as
     * pure I/O cost (cpu = 0). */
    IO_AND_CPU_COST &operator=(double v) { io = v; cpu = 0.0; return *this; }
};

/* MariaDB integer-limit macros. MySQL provides the standard library ones. */
#ifndef ULONGLONG_MAX
#include <climits>
#define ULONGLONG_MAX ULLONG_MAX
#endif

/* MariaDB's MY_MIN / MY_MAX. Use ternary form (matches MariaDB's expansion)
 * so mixed-type comparisons (ulong vs ha_rows) work without explicit casting
 * — std::min/max template deduction would reject those. */
#ifndef MY_MIN
#define MY_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MY_MAX
#define MY_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* Note: tmp_use_all_columns and tmp_restore_column_map already exist in
 * MySQL's sql/table.h with the convention `(TABLE*, MY_BITMAP*)` (no &).
 * MariaDB's convention was `(TABLE*, MY_BITMAP**)` (with &). The call sites
 * in ha_tidesdb.cc were sed-converted to MySQL convention; no macro needed. */

/* MariaDB free functions / globals we stub or alias to MySQL equivalents. */

/* Count set bits — MariaDB exposes my_count_bits. MySQL doesn't, but GCC
 * and clang both ship __builtin_popcountll. */
#ifndef my_count_bits
inline unsigned int my_count_bits(unsigned long long v) {
    return static_cast<unsigned int>(__builtin_popcountll(v));
}
#endif

/* MariaDB exposes reg_ext as a const char* of the .frm extension.
 * MySQL has no .frm files. Map to empty string — only accessed via
 * strcmp() / strlen() in TideSQL. */
#ifndef reg_ext
constexpr const char *reg_ext = "";
#endif

/* MariaDB has handler::lookup_errkey as a uint member that records which
 * index caused a duplicate-key error. MySQL has handler::errkey for the
 * same role. We sed-converted "errkey = lookup_errkey = X" to "errkey = X"
 * in ha_tidesdb.cc — no shim needed. */

/* MariaDB struct used by handler::get_record_buffer for index range iteration.
 * MySQL doesn't have this concept exposed. Forward-declare so type names
 * compile; method bodies that use it must be #ifdef'd or rewritten. */
struct page_range {
    uint64_t first;
    uint64_t last;
};

/* MariaDB's bitmask of pending in-place ALTER operations. MySQL has
 * Alter_inplace_info::HA_ALTER_FLAGS for the same role. Macro expansion
 * happens at use site, where Alter_inplace_info is already defined. */
#define alter_table_operations Alter_inplace_info::HA_ALTER_FLAGS

/* MariaDB has MY_TEST(expr) = (expr) ? 1 : 0. Equivalent to !!. */
#ifndef MY_TEST
#define MY_TEST(expr) (!!(expr))
#endif

/* MariaDB-only HA_* flags TideSQL references in table_flags() output.
 * MySQL doesn't have them; treat as 0 so they don't contribute. */
#ifndef HA_REC_NOT_IN_SEQ
#define HA_REC_NOT_IN_SEQ 0
#endif
#ifndef HA_ONLINE_ANALYZE
#define HA_ONLINE_ANALYZE 0
#endif
#ifndef HA_CONCURRENT_OPTIMIZE
#define HA_CONCURRENT_OPTIMIZE 0
#endif

/* MariaDB-only ALTER flags TideSQL uses but MySQL never had. The flags MySQL
 * DOES have (ALTER_VIRTUAL_GCOL_EXPR, ALTER_VIRTUAL_COLUMN_ORDER, etc.) we
 * leave alone — TideSQL's bare references get qualified to
 * Alter_inplace_info::FLAG by the replay-port-edits.sh Python step.
 * Define-as-0 only the genuinely-MariaDB-only ones. */
#ifndef ALTER_RENAME_INDEX
#define ALTER_RENAME_INDEX 0
#endif
#ifndef ALTER_RENAME_COLUMN
#define ALTER_RENAME_COLUMN 0
#endif
#ifndef ALTER_INDEX_ORDER
#define ALTER_INDEX_ORDER 0
#endif
#ifndef ALTER_INDEX_IGNORABILITY
#define ALTER_INDEX_IGNORABILITY 0
#endif
#ifndef ALTER_DROP_UNIQUE_INDEX
#define ALTER_DROP_UNIQUE_INDEX 0
#endif
#ifndef ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX
#define ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX 0
#endif
#ifndef ALTER_DROP_INDEX
#define ALTER_DROP_INDEX 0
#endif
#ifndef ALTER_DROP_COLUMN
#define ALTER_DROP_COLUMN 0
#endif
#ifndef ALTER_DROP_CHECK_CONSTRAINT
#define ALTER_DROP_CHECK_CONSTRAINT 0
#endif
#ifndef ALTER_COLUMN_OPTION
#define ALTER_COLUMN_OPTION 0
#endif
#ifndef ALTER_CHANGE_COLUMN_DEFAULT
#define ALTER_CHANGE_COLUMN_DEFAULT 0
#endif
#ifndef ALTER_ADD_UNIQUE_INDEX
#define ALTER_ADD_UNIQUE_INDEX 0
#endif
#ifndef ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX
#define ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX 0
#endif
#ifndef ALTER_ADD_INDEX
#define ALTER_ADD_INDEX 0
#endif
#ifndef ALTER_ADD_COLUMN
#define ALTER_ADD_COLUMN 0
#endif

/* Both MariaDB and MySQL expose `enum thd_kill_levels` at top scope with
 * the same three constants (THD_IS_NOT_KILLED / THD_ABORT_SOFTLY /
 * THD_ABORT_ASAP), but MySQL defines it in a deep header
 * (include/mysql/components/services/bits/thd.h) that is not always
 * reachable at the points TideSQL's code refers to the enum. The guard
 * below makes this a no-op once MySQL's real enum has been pulled in
 * transitively, and provides a compatible definition otherwise. */
#ifndef THD_IS_NOT_KILLED
enum thd_kill_levels {
    THD_IS_NOT_KILLED = 0,
    THD_ABORT_SOFTLY = 50,
    THD_ABORT_ASAP = 100
};
#endif

/* (deleted) The old MariaDB-port stub `handler_index_cond_check(void *)`
 * lived here returning a constant 0. With HA_DO_INDEX_COND_PUSHDOWN
 * advertised in index_flags and in_range_check_pushed_down=true in
 * idx_cond_push(), MySQL trusts the engine to filter AND honor
 * end_range; the stub did neither, so secondary-index range scans
 * could leak rows past end_range and through pushed conditions. ICP
 * evaluation is now inline in ha_tidesdb::icp_check_secondary in
 * ha_tidesdb.cc (kill check + compare_key_icp + pushed_idx_cond->val_bool). */

/* (deleted) The old inline mysql_mutex_t LOCK_global_system_variables{}
 * lived here -- zero-initialized in the header so any TideSQL-era call
 * site lock(&LOCK_global_system_variables) would compile. The
 * zero-initialization of a pthread_mutex_t is implementation-defined
 * (works on Linux/glibc, undefined behaviour on macOS/BSD), and the
 * shim was already documented as "currently unused in the MySQL port".
 * Removed entirely so any future need for the symbol fails at link
 * time with a clear "undefined reference" rather than silently using
 * an uninitialized mutex. */

/* MariaDB-only utilities. Stub each to compile-only behavior. */
#ifndef mysql_file_stat
#include <sys/stat.h>
inline struct stat *mysql_file_stat(unsigned int /*key*/, const char *path,
                                    struct stat *st, int /*flags*/) {
    return ::stat(path, st) == 0 ? st : nullptr;
}
#endif

#ifndef my_rmtree
/* Recursive directory removal. Walks `path` depth-first and unlinks every
 * regular file / directory found, then removes `path` itself. Used by
 * force_remove_cf_dir() to wipe a column-family's on-disk SSTables and
 * WAL after the underlying CF was dropped but stale fds (block cache,
 * background workers) kept the directory partially populated.
 *
 * Returns 0 on success, non-zero on the first unrecoverable error.
 * Missing path is treated as success (caller already stat()'d). Symlinks
 * are not followed (FTW_PHYS). */
#include <ftw.h>
inline int my_rmtree_visit_(const char *fpath, const struct stat * /*sb*/,
                            int /*typeflag*/, struct FTW * /*ftwbuf*/) {
    /* remove() handles both files (unlink) and empty dirs (rmdir).
     * FTW_DEPTH guarantees children are visited before their parent. */
    return ::remove(fpath);
}
inline int my_rmtree(const char *path, int /*flags*/) {
    if (!path) return -1;
    /* nopenfd=64 caps concurrent fd usage during the walk. */
    if (nftw(path, my_rmtree_visit_, 64, FTW_DEPTH | FTW_PHYS) != 0) {
        return (errno == ENOENT) ? 0 : -1;
    }
    return 0;
}
#endif

#ifndef my_random_bytes
#include <fcntl.h>
#include <unistd.h>
#include <sys/random.h>  /* getrandom(2), Linux >= 3.17, glibc >= 2.25 */
/* Fill `buf` with `n` cryptographically-strong random bytes.
 * Used to generate the IV for each encrypted row write -- one call per
 * write_row/update_row on an encrypted table. The old implementation
 * open()/read()/close()'d /dev/urandom every call (3 syscalls per row);
 * getrandom(2) is a single syscall and drops the fd table footprint
 * entirely. Returns 0 on success, 1 on failure (matches the original
 * contract; callers check for non-zero). */
inline int my_random_bytes(unsigned char *buf, int n) {
    int got = 0;
    while (got < n) {
        ssize_t r = ::getrandom(buf + got, (size_t)(n - got), 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (r == 0) return 1; /* shouldn't happen with blocking getrandom */
        got += (int)r;
    }
    return 0;
}
#endif

#ifndef microsecond_interval_timer
#include <ctime>  /* clock_gettime, CLOCK_MONOTONIC, timespec */
inline unsigned long long microsecond_interval_timer() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1000000ULL
         + static_cast<unsigned long long>(ts.tv_nsec) / 1000ULL;
}
#endif

/* MariaDB strxnmov(dst, n, str1, str2, ..., NullS) — variadic string concat
 * with length cap. MySQL doesn't ship it. Provide a 4-arg shim that handles
 * the only pattern TideSQL uses (path = a + b + c + NullS). */
#ifndef strxnmov
#include <cstring>
inline char *strxnmov(char *dst, size_t n, const char *a, const char *b = nullptr,
                      const char *c = nullptr, const char *d = nullptr,
                      const char * = nullptr) {
    auto append = [&](const char *s) {
        if (!s) return;
        size_t avail = (n > 0) ? n - 1 : 0;
        size_t took = 0;
        while (avail-- && *s) { *dst++ = *s++; ++took; }
        n -= took;
    };
    append(a); append(b); append(c); append(d);
    if (n > 0) *dst = '\0';
    return dst;
}
#endif

/* MariaDB's per-table option registration type. MySQL uses ha_create_table_option
 * conceptually but exposes it via Sys_variable / Plugin_var. For now stub the
 * type so the static array declarations compile; the actual registration code
 * is gated by HAVE_TIDESDB_TABLE_OPTIONS (we don't define it). */
#ifndef HAVE_TIDESDB_TABLE_OPTIONS
struct ha_create_table_option {
    int dummy;
};
#endif

/* HA_ERR_* values that MariaDB defines but MySQL doesn't. Map each to the
 * MySQL code that carries the same operational meaning so the application
 * sees a useful errno instead of the opaque HA_ERR_GENERIC / 1030.
 *
 *   HA_ERR_ABORTED_BY_USER -> HA_ERR_QUERY_INTERRUPTED (errno 1317 / ER_QUERY_INTERRUPTED)
 *     Returned from row reads when thd_killed() observes a KILL QUERY. */
#ifndef HA_ERR_ABORTED_BY_USER
#define HA_ERR_ABORTED_BY_USER HA_ERR_QUERY_INTERRUPTED
#endif

/* ----- Category 2: MariaDB-only flag values ----- */

/* In MariaDB these are top-level enum constants; in MySQL they're members of
 * Alter_inplace_info::HA_ALTER_FLAGS. The macro expansion happens at the use
 * site (inside .cc methods), where Alter_inplace_info is already defined. */
#ifndef ALTER_ADD_PK_INDEX
#define ALTER_ADD_PK_INDEX        (Alter_inplace_info::ADD_PK_INDEX)
#endif
#ifndef ALTER_DROP_PK_INDEX
#define ALTER_DROP_PK_INDEX       (Alter_inplace_info::DROP_PK_INDEX)
#endif
#ifndef ALTER_CHANGE_CREATE_OPTION
#define ALTER_CHANGE_CREATE_OPTION (Alter_inplace_info::CHANGE_CREATE_OPTION)
#endif

/* CHECK_* enum values from MariaDB's check_result_t.
 * MySQL uses HA_ADMIN_* return codes; semantics are similar. The literal
 * integers below are arbitrary — TideSQL only compares them, never sends
 * them to MySQL. Worth refining if a test ever depends on specific
 * bit patterns. */
#ifndef CHECK_ABORTED_BY_USER
#define CHECK_ABORTED_BY_USER 1
#endif
#ifndef CHECK_NEG
#define CHECK_NEG -1
#endif
#ifndef CHECK_OUT_OF_RANGE
#define CHECK_OUT_OF_RANGE 2
#endif
#ifndef CHECK_POS
#define CHECK_POS 3
#endif

/* HA_CAN_* table-flags advertisements: MariaDB-only capabilities. Set to 0
 * so they contribute nothing to the table_flags() bitmap. Re-enable
 * specific flags if/when the underlying support is added. */
#ifndef HA_CAN_ONLINE_BACKUPS
#define HA_CAN_ONLINE_BACKUPS 0
#endif
#ifndef HA_CAN_TABLES_WITHOUT_ROLLBACK
#define HA_CAN_TABLES_WITHOUT_ROLLBACK 0
#endif
/* MariaDB's HA_CAN_VIRTUAL_COLUMNS maps to MySQL's HA_GENERATED_COLUMNS;
 * advertising it lets `... AS (expr) STORED` and ` ... AS (expr) VIRTUAL`
 * compute over our row format. */
#ifndef HA_CAN_VIRTUAL_COLUMNS
#define HA_CAN_VIRTUAL_COLUMNS HA_GENERATED_COLUMNS
#endif
#ifndef HA_CLUSTERED_INDEX
#define HA_CLUSTERED_INDEX 0
#endif

/* MariaDB-only thr_lock enum value — used in store_lock(). Map to plain
 * TL_WRITE (the closest MySQL semantic) until we redo the lock layer. */
#ifndef TL_FIRST_WRITE
#define TL_FIRST_WRITE TL_WRITE
#endif

/* MariaDB warning level. MySQL uses Sql_condition::SL_NOTE. */
#ifndef WARN_LEVEL_NOTE
#define WARN_LEVEL_NOTE Sql_condition::SL_NOTE
#endif

/* ----- Category 4: Macro renames ----- */

/* MySQL 8.0+ removed DBUG_ASSERT in favor of plain assert. Bring it back so
 * TideSQL's site-by-site usage keeps compiling. */
#ifndef DBUG_ASSERT
#define DBUG_ASSERT(x) assert(x)
#endif

/* ----- Category 7: Logging shims -----
 *
 * MariaDB exposes free functions sql_print_{information,warning,error};
 * MySQL 8.0+ removed them in favor of LogErr / LogPluginErr / LogEvent.
 * Those replacements all fan out through the `log_bs` service handle
 * declared in mysql/components/services/log_builtins.h. MODULE_ONLY
 * plugins do NOT get that handle linked in automatically -- using them
 * unconditionally produces an `undefined symbol: log_bs` load failure.
 *
 * The older `my_plugin_log_message` API is also unsuitable here: storage
 * engines' handlerton init receives the `handlerton *` rather than the
 * `st_plugin_int *` the service expects, so there is no obvious place
 * to capture a valid plugin handle without registering as a real
 * component plugin -- a much bigger refactor than this surface needs.
 *
 * Pragmatic choice: keep the messages going to stderr. mysqld redirects
 * stderr to the error log file early in startup, so these lines DO end
 * up in the operator-visible log -- they're just plain-formatted rather
 * than structured. Format mimics MySQL's prefix style so grep / log
 * aggregators can still pick them up by component name. */
inline void sql_print_information(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fputs("[Note] [tidesdb] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

inline void sql_print_warning(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fputs("[Warning] [tidesdb] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

inline void sql_print_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fputs("[ERROR] [tidesdb] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

/* (extracted) The at-rest encryption / master-key API was hosted here
 * for the original port. It now lives in its own header
 * `tidesdb_master_key.h` -- this was the first step of the architectural
 * extraction recommended by the follow-up code review. compat.h is
 * sliding toward MariaDB-only renames and stubs as its sole role. */
#include "tidesdb_master_key.h"
