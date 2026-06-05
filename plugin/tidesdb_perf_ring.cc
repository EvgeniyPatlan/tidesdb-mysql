#include "tidesdb_perf_ring.h"
#include "tidesdb_perf_scope.h"

#if TIDESDB_PERF

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <x86intrin.h>            // __rdtsc()

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>                    // std::nothrow
#include <string>
#include <thread>
#include <vector>

namespace tidesdb_perf {

std::atomic<TLS_Ring *> g_rings_head{nullptr};
std::atomic<bool> g_capture_active{false};
thread_local TLS_Ring *t_ring = nullptr;

namespace {

/* Module-globals owned by init()/deinit(). Lifetime is bracketed by
   tidesdb_init_func / tidesdb_deinit_func. The flusher thread reads
   shutdown via atomic + cv; output_dir is mutated by set_output_dir()
   (sysvar update callback) so it needs the same mutex. */
struct PerfModuleState {
    /* current_dir = dir that fds are currently open under (empty if no
       fds open yet). desired_dir = latest value the sysvar update path
       has handed us. flusher rotates current_dir -> desired_dir at the
       top of each tick. */
    std::string current_dir;
    std::string desired_dir;
    std::mutex  dir_mu;

    size_t      ring_capacity_pow2 = TLS_Ring::kCapacityPow2Default;
    uint64_t    flush_interval_ms  = 1000;
    /* fds for method ids 1..kMethodCount. Index 0 unused. */
    int         method_fds[kMethodCount + 1] = {0};
    std::thread flusher;
    std::atomic<bool> shutdown{false};
    std::mutex  shutdown_mu;
    std::condition_variable shutdown_cv;
    double      tsc_ghz = 0.0;
};
static PerfModuleState g_state;

/* Brief calibration: rdtsc + steady_clock over a ~20 ms spin window.
   Returns cycles per ns (= GHz). */
static double measure_tsc_ghz() {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t tsc0 = __rdtsc();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(20)) {
        /* spin */
    }
    auto t1 = std::chrono::steady_clock::now();
    uint64_t tsc1 = __rdtsc();
    double dt_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (dt_ns <= 0.0) return 0.0;
    return (static_cast<double>(tsc1 - tsc0) / dt_ns);
}

static void write_meta_json_in(const std::string &dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%d-meta.json",
             dir.c_str(), getpid());
    int fd = ::open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        /* sql_print_warning is not safe at init time; use stderr. */
        fprintf(stderr, "[TIDESDB-PERF] cannot create meta.json at %s: %s\n",
                path, std::strerror(errno));
        return;
    }
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\n"
        "  \"format_version\": 1,\n"
        "  \"tsc_ghz\": %.6f,\n"
        "  \"capacity\": %zu,\n"
        "  \"method_count\": %u,\n"
        "  \"pid\": %d\n"
        "}\n",
        g_state.tsc_ghz, static_cast<size_t>(1u) << g_state.ring_capacity_pow2,
        static_cast<unsigned>(kMethodCount), getpid());
    if (n > 0) {
        ssize_t wrote = ::write(fd, buf, static_cast<size_t>(n));
        (void)wrote;
    }
    ::close(fd);
}

static const char *method_name_for(uint8_t id) {
    static const char *names[] = {
        nullptr,  /* method_id 0 unused */
        "01-write_row",  "02-update_row", "03-delete_row",
        "04-index_read_map", "05-index_next", "06-index_prev",
        "07-rnd_next", "08-rnd_pos",
        "09-external_lock", "10-start_stmt", "11-store_lock",
        "12-commit", "13-rollback",
        "14-savepoint_set", "15-savepoint_release", "16-savepoint_rollback",
        "17-open", "18-close", "19-info", "20-table_flags_cache_init",
        "21-create", "22-delete_table",
        "23-check_if_supported_inplace_alter", "24-prepare_inplace_alter_table",
        "25-inplace_alter_table", "26-commit_inplace_alter_table",
        "27-serialize_row", "28-deserialize_row",
        "29-key_copy_to_comparable", "30-pk_from_record",
        "31-encrypt_row_into", "32-decrypt_row",
    };
    if (id < 1 || id > kMethodCount) return nullptr;
    return names[id];
}

static void open_method_files_in(const std::string &dir) {
    for (uint8_t i = 1; i <= kMethodCount; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%d-%s.bin",
                 dir.c_str(), getpid(), method_name_for(i));
        int fd = ::open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd < 0) {
            fprintf(stderr, "[TIDESDB-PERF] cannot open %s: %s\n",
                    path, std::strerror(errno));
            continue;
        }
        g_state.method_fds[i] = fd;
    }
}

static void close_method_files() {
    for (uint8_t i = 1; i <= kMethodCount; i++) {
        if (g_state.method_fds[i] > 0) ::close(g_state.method_fds[i]);
        g_state.method_fds[i] = 0;
    }
}

/* Open meta.json + 32 per-method fds under dir; mkdir -p first. On
   mkdir failure (not EEXIST), leave fds closed and return false. The
   flusher will retry next tick. */
static bool rotate_to_dir(const std::string &dir) {
    if (dir.empty()) return false;
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[TIDESDB-PERF] cannot create output dir %s: %s\n",
                dir.c_str(), std::strerror(errno));
        return false;
    }
    close_method_files();
    write_meta_json_in(dir);
    open_method_files_in(dir);
    return true;
}

/* Walk g_rings_head, snapshot each ring (window = W - R), bucket samples
   by method id, append to the per-method fd. Called repeatedly by the
   flusher and once synchronously from deinit() after shutdown=true.
   At entry, if the desired output_dir has changed since the fds were
   opened, rotate fds to the new dir. */
static void flush_once() {
    /* Rotate to desired dir if the sysvar update path moved it, or if
       the current dir disappeared under us (rm -rf, tmpfs reaper, etc.)
       so files re-materialize on the next tick. */
    std::string pending;
    {
        std::lock_guard<std::mutex> lk(g_state.dir_mu);
        if (g_state.desired_dir != g_state.current_dir) {
            pending = g_state.desired_dir;
        } else if (!g_state.current_dir.empty()) {
            struct stat st;
            if (::stat(g_state.current_dir.c_str(), &st) != 0) {
                pending = g_state.current_dir;
                g_state.current_dir.clear();
            }
        }
    }
    if (!pending.empty()) {
        if (rotate_to_dir(pending)) {
            std::lock_guard<std::mutex> lk(g_state.dir_mu);
            g_state.current_dir = pending;
        }
    }

    std::vector<std::vector<Sample>> by_method(kMethodCount + 1);
    TLS_Ring *r = g_rings_head.load(std::memory_order_acquire);
    while (r) {
        uint64_t w  = r->write_idx.load(std::memory_order_acquire);
        uint64_t rd = r->read_idx.load(std::memory_order_relaxed);
        uint64_t to_drain = w - rd;
        if (to_drain > r->capacity) to_drain = r->capacity;
        for (uint64_t i = 0; i < to_drain; i++) {
            const Sample &s = r->slots[(rd + i) & (r->capacity - 1)];
            if (s.method_id >= 1 && s.method_id <= kMethodCount) {
                by_method[s.method_id].push_back(s);
            }
        }
        r->read_idx.store(w, std::memory_order_release);
        r = r->next.load(std::memory_order_acquire);
    }
    for (uint8_t i = 1; i <= kMethodCount; i++) {
        if (by_method[i].empty() || g_state.method_fds[i] <= 0) continue;
        ssize_t wrote = ::write(g_state.method_fds[i], by_method[i].data(),
                                by_method[i].size() * sizeof(Sample));
        (void)wrote;
    }
}

static void flusher_loop() {
    /* Pin to one core so rdtsc deltas don't migrate. Best-effort -- on
       containers without CPU 0 affinity this is a no-op. */
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(0, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

    while (!g_state.shutdown.load(std::memory_order_acquire)) {
        flush_once();
        std::unique_lock<std::mutex> lk(g_state.shutdown_mu);
        g_state.shutdown_cv.wait_for(lk,
            std::chrono::milliseconds(g_state.flush_interval_ms),
            []{ return g_state.shutdown.load(std::memory_order_acquire); });
    }
}

}  /* anonymous namespace */

bool init(const char *output_dir, size_t ring_capacity_pow2, uint64_t flush_interval_ms) {
    const char *dir = (output_dir && *output_dir)
                          ? output_dir : "/var/lib/mysql/tidesdb-perf";
    g_state.ring_capacity_pow2 = ring_capacity_pow2;
    g_state.flush_interval_ms = flush_interval_ms;
    g_state.tsc_ghz = measure_tsc_ghz();
    /* Try the initial dir. If mkdir fails (e.g. /var/lib/mysql/tidesdb-perf
       on a read-only filesystem), we still start the flusher with no fds
       open. A later SET GLOBAL tidesdb_perf_output_dir = '...' will route
       through set_output_dir() and the flusher will retry on its next
       tick. */
    {
        std::lock_guard<std::mutex> lk(g_state.dir_mu);
        g_state.desired_dir = dir;
        g_state.current_dir.clear();
    }
    if (rotate_to_dir(dir)) {
        std::lock_guard<std::mutex> lk(g_state.dir_mu);
        g_state.current_dir = dir;
    }
    g_state.shutdown.store(false, std::memory_order_release);
    g_state.flusher = std::thread(flusher_loop);
    return true;
}

void deinit() {
    {
        std::lock_guard<std::mutex> lk(g_state.shutdown_mu);
        g_state.shutdown.store(true, std::memory_order_release);
    }
    g_state.shutdown_cv.notify_all();
    if (g_state.flusher.joinable()) g_state.flusher.join();
    /* Final synchronous drain: anything queued between the last tick and
       the shutdown signal still lands on disk. shutdown=true at this
       point but flush_once() doesn't read it. */
    flush_once();
    close_method_files();
    /* Free every ring on g_rings_head. */
    TLS_Ring *r = g_rings_head.load(std::memory_order_acquire);
    while (r) {
        TLS_Ring *next = r->next.load(std::memory_order_acquire);
        ring_free(r);
        r = next;
    }
    g_rings_head.store(nullptr, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_state.dir_mu);
        g_state.current_dir.clear();
        g_state.desired_dir.clear();
    }
}

void set_output_dir(const char *output_dir) {
    if (!output_dir) return;
    std::lock_guard<std::mutex> lk(g_state.dir_mu);
    g_state.desired_dir = output_dir;
}

/* Lock-free push of a newly-allocated ring onto g_rings_head.
   compare_exchange loop; standard linked-list head insert. */
static void ring_push_global(TLS_Ring *r) {
    TLS_Ring *head = g_rings_head.load(std::memory_order_acquire);
    do {
        r->next.store(head, std::memory_order_relaxed);
    } while (!g_rings_head.compare_exchange_weak(
        head, r, std::memory_order_release, std::memory_order_acquire));
}

TLS_Ring *ring_alloc_for_thread() {
    auto *r = new (std::nothrow) TLS_Ring{};
    if (!r) return nullptr;
    /* Use the per-thread default; production code sources from the sysvar
       via init()-stored module-globals (added in Task 6). */
    r->capacity = 1u << TLS_Ring::kCapacityPow2Default;
    r->slots = new (std::nothrow) Sample[r->capacity]{};
    if (!r->slots) { delete r; return nullptr; }
    r->owner_tid = static_cast<uint64_t>(pthread_self());
    ring_push_global(r);
    return r;
}

void ring_free(TLS_Ring *r) {
    if (!r) return;
    delete[] r->slots;
    delete r;
}

void push_sample(TLS_Ring *r, uint8_t method_id, uint8_t thread_id,
                 uint64_t enter_tsc, uint64_t exit_tsc) {
    if (!r) return;
    uint64_t w = r->write_idx.fetch_add(1, std::memory_order_relaxed);
    uint64_t slot_idx = w & (r->capacity - 1);
    /* Detect wrap: bump wrap_count exactly once per capacity boundary the
       writer crosses. w == capacity, 2*capacity, ... has slot_idx == 0
       and w >= capacity, so the condition fires once per lap. */
    if (w >= r->capacity && slot_idx == 0) {
        r->wrap_count.fetch_add(1, std::memory_order_relaxed);
    }
    Sample &s = r->slots[slot_idx];
    s.method_id = method_id;
    s.thread_id = thread_id;
    s.reserved  = 0;
    s.enter_tsc = enter_tsc;
    s.exit_tsc  = exit_tsc;
}

/* PerfScope dtor out-of-line so callers don't need ring.h in their includes
   (only scope.h). */
PerfScope::~PerfScope() noexcept {
    if (!g_capture_active.load(std::memory_order_relaxed)) return;
    uint64_t exit = __rdtsc();
    if (!t_ring) t_ring = ring_alloc_for_thread();
    if (!t_ring) return;  /* OOM */
    /* Cheap thread-id hash: lower 8 bits of owner_tid. */
    uint8_t tid = static_cast<uint8_t>(t_ring->owner_tid);
    push_sample(t_ring, static_cast<uint8_t>(m_id), tid, m_enter, exit);
}

}  /* namespace tidesdb_perf */

#endif  /* TIDESDB_PERF */
