#include "tidesdb_perf_ring.h"
#include "tidesdb_perf_scope.h"

#if TIDESDB_PERF

namespace tidesdb_perf {

std::atomic<TLS_Ring *> g_rings_head{nullptr};
std::atomic<bool> g_capture_active{false};
thread_local TLS_Ring *t_ring = nullptr;

bool init(const char *, size_t, uint64_t) { return true; }
void deinit() {}

TLS_Ring *ring_alloc_for_thread() { return nullptr; }
void ring_free(TLS_Ring *) {}

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
PerfScope::~PerfScope() noexcept {}

}  /* namespace tidesdb_perf */

#endif  /* TIDESDB_PERF */
