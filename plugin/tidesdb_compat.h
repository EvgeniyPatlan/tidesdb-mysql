/* tidesdb_compat.h
 *
 * Compatibility shims that let TideSQL's MariaDB-targeted source compile against
 * MySQL 9.7. Include this BEFORE any MySQL header — most shims are #defines
 * and stub inline functions whose definitions need to win over MySQL's own.
 *
 * Categories covered:
 *   1. MariaDB-only types         (range_id_t, check_result_t, my_bool, my_ptrdiff_t)
 *   2. MariaDB-only flag values   (ALTER_*, CHECK_*, HA_CAN_*, TL_FIRST_WRITE, WARN_LEVEL_*)
 *   3. Encryption API stubs       (compile-only — runtime always fails)
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
 * that do `cost.io + cost.cpu` keep compiling. Round 4 will replace the real
 * scan_time/read_time bodies with double returns. */
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

/* MariaDB exposes thd_kill_levels enum at top scope; MySQL has it inside
 * the THD class. Forward-declare it as a simple enum so engines that
 * compare against it compile. Phase 4: use THD::Killed_state. */
enum thd_kill_levels {
    THD_IS_NOT_KILLED = 0,
    THD_ABORT_SOFTLY = 50,
    THD_ABORT_ASAP = 100
};

/* MariaDB-only handler_index_cond_check pushdown helper. Stub. */
inline int handler_index_cond_check(void * /*opaque*/) { return 0; }

/* MariaDB exposes LOCK_global_system_variables; MySQL doesn't expose it as
 * a global symbol but engines can use mysql_mutex_t. Provide an inline
 * mysql_mutex_t (C++17 inline variable for single-instance) so TideSQL's
 * { mysql_mutex_lock(&LOCK_global_system_variables); ... } sites compile.
 * Initialized lazily on first use; never destroyed (deliberate — short-lived
 * server lifecycle). Phase 4: replace with proper sysvar serialization. */
#include "mysql/psi/mysql_mutex.h"
inline mysql_mutex_t LOCK_global_system_variables{};

/* MariaDB-only utilities. Stub each to compile-only behavior. */
#ifndef mysql_file_stat
#include <sys/stat.h>
inline struct stat *mysql_file_stat(unsigned int /*key*/, const char *path,
                                    struct stat *st, int /*flags*/) {
    return ::stat(path, st) == 0 ? st : nullptr;
}
#endif

#ifndef my_rmtree
inline int my_rmtree(const char* /*path*/, int /*flags*/) { return 0; }  /* TODO: real impl */
#endif

#ifndef my_random_bytes
#include <fcntl.h>
#include <unistd.h>
inline int my_random_bytes(unsigned char *buf, int n) {
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 1;
    int got = 0;
    while (got < n) {
        int r = ::read(fd, buf + got, n - got);
        if (r <= 0) { ::close(fd); return 1; }
        got += r;
    }
    ::close(fd);
    return 0;
}
#endif

#ifndef microsecond_interval_timer
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

/* HA_ERR_* values that MariaDB defines but MySQL doesn't. Map to the closest
 * generic MySQL error so the engine can still report failure. Refine in
 * Round 4 when we audit error reporting end-to-end. */
#ifndef HA_ERR_ABORTED_BY_USER
#define HA_ERR_ABORTED_BY_USER HA_ERR_GENERIC
#endif

/* MariaDB row-status code used in iterator/scan paths. MySQL has nothing
 * equivalent at this layer — rnd_next returns HA_ERR_END_OF_FILE. Map to
 * the engine-internal value -1 so comparisons stop failing. Use sites
 * may need real edits in Round 4. */
#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND (-1)
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
 * them to MySQL. We can refine in Round 4 if a test actually depends on
 * specific bit patterns. */
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
 * so they contribute nothing to the table_flags() bitmap. We re-enable
 * specific flags in Phase 4 when (and if) we add the underlying support. */
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

/* ----- Category 7: Logging stubs -----
 * MariaDB has free functions sql_print_{information,warning,error}.
 * MySQL 8.0+ removed them in favor of LogErr() / LogPlugin*(). For Phase 2,
 * stub to fprintf(stderr) — switch to LogErr in Phase 4. */
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

/* ----- Category 3: Encryption API ports -----
 * MariaDB exposes a public C API for at-rest encryption that storage engines
 * call directly. MySQL has an analogous component-based stack (keyring_aes,
 * keyring_reader_with_status). For our v1 we use the simplest workable
 * mechanism: the DBA puts a 32-byte AES-256 key in a file pointed to by
 * the tidesdb_master_key_file system variable; the plugin reads it once at
 * init and uses my_aes_encrypt/my_aes_decrypt for the actual crypto. This
 * avoids the dependency on a separately-installed keyring component while
 * still giving real encryption-at-rest. Per-table key_id is accepted in
 * the public API but maps to the same master key for now -- key rotation
 * and per-table keys are follow-up work. Real implementations live in
 * plugin/tidesdb_keyring_compat.cc. */
#ifndef ENCRYPTION_FLAG_DECRYPT
#define ENCRYPTION_FLAG_DECRYPT 0
#endif
#ifndef ENCRYPTION_FLAG_ENCRYPT
#define ENCRYPTION_FLAG_ENCRYPT 1
#endif
#ifndef ENCRYPTION_KEY_VERSION_INVALID
#define ENCRYPTION_KEY_VERSION_INVALID ((unsigned int)-1)
#endif

int encryption_crypt(const unsigned char *src, unsigned int src_len,
                     unsigned char *dst, unsigned int *dst_len,
                     const unsigned char *key, unsigned int key_len,
                     const unsigned char *iv, unsigned int iv_len,
                     int flags, unsigned int key_id,
                     unsigned int key_version);

unsigned int encryption_encrypted_length(unsigned int src_len,
                                         unsigned int key_id,
                                         unsigned int key_version);

int encryption_key_get(unsigned int key_id, unsigned int key_version,
                       unsigned char *key, unsigned int *key_len);

unsigned int encryption_key_get_latest_version(unsigned int key_id);

/* Master-key bootstrap. Called from plugin init with the path the DBA
   set in tidesdb_master_key_file. Returns false on success, true on
   failure (file missing, wrong size, IO error). When the master key is
   not loaded, encryption_key_get returns -1 and any CREATE TABLE that
   asks for encryption fails with a clear error. */
bool tidesdb_master_key_load_from_file(const char *path);

/* Test-only: clears the loaded key. Used between test runs. */
void tidesdb_master_key_clear();
