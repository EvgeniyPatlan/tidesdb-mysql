/* tidesdb_portability.h
 *
 * Real implementations of the small OS / runtime helpers that MariaDB
 * exposes as free functions but MySQL 9.7 does not ship. Extracted
 * from tidesdb_compat.h as the A-8 pass: compat.h is now pure
 * renames / #define-as-0 / type-alias stubs, and every shim that had
 * an actual function body lives here with the definition in
 * tidesdb_portability.cc.
 *
 * What this owns:
 *   - my_count_bits          popcount wrapper
 *   - mysql_file_stat        ::stat wrapper
 *   - my_rmtree              recursive depth-first directory removal
 *   - my_random_bytes        getrandom(2)-backed CSPRNG fill
 *   - microsecond_interval_timer  CLOCK_MONOTONIC microsecond counter
 *   - strxnmov               4-arg length-capped string concat
 *   - sql_print_{information,warning,error}  stderr logging shims
 *
 * tidesdb_compat.h #includes this header at its end (mirroring how it
 * pulls in tidesdb_master_key.h), so existing translation units that
 * include compat.h keep seeing these symbols with no include-list
 * changes. */

#pragma once

#include <cstddef>

/* Count set bits. MariaDB exposes my_count_bits; MySQL does not, but
   GCC and clang both ship __builtin_popcountll. */
unsigned int my_count_bits(unsigned long long v);

/* MariaDB's mysql_file_stat. Returns `st` on success, nullptr on
   failure (matching the MariaDB contract callers expect). */
struct stat;
struct stat *mysql_file_stat(unsigned int key, const char *path, struct stat *st, int flags);

/* Recursive directory removal. Walks `path` depth-first, unlinking
   every regular file / empty directory, then removes `path` itself.
   Returns 0 on success (a missing path is success), non-zero on the
   first unrecoverable error. Symlinks are not followed. */
int my_rmtree(const char *path, int flags);

/* Fill `buf` with `n` cryptographically-strong random bytes via
   getrandom(2). Returns 0 on success, 1 on failure (callers check
   for non-zero). One call per encrypted row write (IV generation). */
int my_random_bytes(unsigned char *buf, int n);

/* Monotonic microsecond counter (CLOCK_MONOTONIC). */
unsigned long long microsecond_interval_timer();

/* MariaDB strxnmov: variadic length-capped string concat. Only the
   4-source pattern TideSQL uses (path = a + b + c + NullS) is
   supported. Writes at most n-1 chars plus a NUL; returns the write
   cursor. */
char *strxnmov(char *dst, size_t n, const char *a, const char *b = nullptr,
               const char *c = nullptr, const char *d = nullptr, const char * = nullptr);

/* MariaDB exposed sql_print_{information,warning,error} as free
   functions; MySQL 8.0+ removed them in favor of LogErr / LogPluginErr
   which fan out through the `log_bs` service handle that MODULE_ONLY
   plugins do not get linked. These shims write to stderr (mysqld
   redirects stderr into the error log early in startup) with a
   MySQL-style component prefix so log aggregators still pick them up.
   See the extended rationale at the call-site comment in
   tidesdb_portability.cc. */
void sql_print_information(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void sql_print_warning(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void sql_print_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
