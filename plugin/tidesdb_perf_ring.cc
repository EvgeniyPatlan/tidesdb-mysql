#include "tidesdb_perf_ring.h"
#include "tidesdb_perf_scope.h"

#if TIDESDB_PERF

#include <pthread.h>
#include <x86intrin.h>            // __rdtsc()

#include <new>                    // std::nothrow

namespace tidesdb_perf {

std::atomic<TLS_Ring *> g_rings_head{nullptr};
std::atomic<bool> g_capture_active{false};
thread_local TLS_Ring *t_ring = nullptr;

bool init(const char *, size_t, uint64_t) { return true; }
void deinit() {}

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
