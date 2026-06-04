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

void push_sample(TLS_Ring *, uint8_t, uint8_t, uint64_t, uint64_t) {}

/* PerfScope dtor out-of-line so callers don't need ring.h in their includes
   (only scope.h). */
PerfScope::~PerfScope() noexcept {}

}  /* namespace tidesdb_perf */

#endif  /* TIDESDB_PERF */
