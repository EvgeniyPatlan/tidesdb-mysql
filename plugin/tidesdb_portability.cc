/* tidesdb_portability.cc
 *
 * Real implementations of the MariaDB free functions MySQL 9.7 does
 * not ship. Extracted from tidesdb_compat.h (A-8). See
 * tidesdb_portability.h for the API contract and the per-function
 * rationale that previously lived inline in compat.h.
 */

#include "tidesdb_portability.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>      /* clock_gettime, CLOCK_MONOTONIC, timespec */
#include <fcntl.h>
#include <ftw.h>
#include <sys/random.h>  /* getrandom(2), Linux >= 3.17, glibc >= 2.25 */
#include <sys/stat.h>
#include <unistd.h>

unsigned int my_count_bits(unsigned long long v)
{
    return static_cast<unsigned int>(__builtin_popcountll(v));
}

struct stat *mysql_file_stat(unsigned int /*key*/, const char *path, struct stat *st,
                             int /*flags*/)
{
    return ::stat(path, st) == 0 ? st : nullptr;
}

/* nftw visitor: remove() handles both files (unlink) and empty dirs
   (rmdir); FTW_DEPTH guarantees children are visited before parents. */
static int my_rmtree_visit_(const char *fpath, const struct stat * /*sb*/, int /*typeflag*/,
                            struct FTW * /*ftwbuf*/)
{
    return ::remove(fpath);
}

int my_rmtree(const char *path, int /*flags*/)
{
    if (!path) return -1;
    /* nopenfd=64 caps concurrent fd usage during the walk. */
    if (nftw(path, my_rmtree_visit_, 64, FTW_DEPTH | FTW_PHYS) != 0)
    {
        return (errno == ENOENT) ? 0 : -1;
    }
    return 0;
}

int my_random_bytes(unsigned char *buf, int n)
{
    int got = 0;
    while (got < n)
    {
        ssize_t r = ::getrandom(buf + got, (size_t)(n - got), 0);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            return 1;
        }
        if (r == 0) return 1; /* shouldn't happen with blocking getrandom */
        got += (int)r;
    }
    return 0;
}

unsigned long long microsecond_interval_timer()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1000000ULL +
           static_cast<unsigned long long>(ts.tv_nsec) / 1000ULL;
}

char *strxnmov(char *dst, size_t n, const char *a, const char *b, const char *c,
                const char *d, const char *)
{
    auto append = [&](const char *s) {
        if (!s) return;
        size_t avail = (n > 0) ? n - 1 : 0;
        size_t took = 0;
        while (avail-- && *s)
        {
            *dst++ = *s++;
            ++took;
        }
        n -= took;
    };
    append(a);
    append(b);
    append(c);
    append(d);
    if (n > 0) *dst = '\0';
    return dst;
}

/* Logging shims. MariaDB exposed sql_print_{information,warning,error}
   as free functions; MySQL 8.0+ removed them in favor of LogErr /
   LogPluginErr / LogEvent. Those all fan out through the `log_bs`
   service handle declared in mysql/components/services/log_builtins.h.
   MODULE_ONLY plugins do NOT get that handle linked in automatically --
   using them unconditionally produces an `undefined symbol: log_bs`
   load failure. The older my_plugin_log_message API is also
   unsuitable: storage engines' handlerton init receives the
   `handlerton *` rather than the `st_plugin_int *` the service
   expects.

   Pragmatic choice: keep the messages going to stderr. mysqld
   redirects stderr to the error log file early in startup, so these
   lines DO end up in the operator-visible log -- just plain-formatted
   rather than structured. The prefix mimics MySQL's style so grep /
   log aggregators can still pick them up by component name. */
void sql_print_information(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[Note] [tidesdb] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

void sql_print_warning(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[Warning] [tidesdb] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

void sql_print_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[ERROR] [tidesdb] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}
