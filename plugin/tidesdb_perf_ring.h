/*
  Per-thread perf-ring + sample primitive for the TidesDB-MySQL plugin.

  Companion to docs/superpowers/specs/2026-06-04-perf-instrumentation-design.md.
  Compiled in under -DTIDESDB_PERF=1; runtime-gated by tidesdb_perf_capture sysvar.
*/
#pragma once

#include <atomic>
#include <cstdint>

#include "tidesdb_perf_scope.h"  // for MethodId enum

namespace tidesdb_perf {

#if TIDESDB_PERF

struct Sample {
    uint8_t  method_id;
    uint8_t  thread_id;
    uint16_t reserved;
    uint64_t enter_tsc;
    uint64_t exit_tsc;
};
static_assert(sizeof(Sample) == 24, "Sample must be 24 bytes");

struct TLS_Ring {
    static constexpr size_t kCapacityPow2Default = 16;
    size_t capacity;            // 1u << capacity_pow2; populated at alloc
    alignas(64) std::atomic<uint64_t> write_idx;
    alignas(64) std::atomic<uint64_t> read_idx;
    std::atomic<TLS_Ring *> next;
    std::atomic<bool> tombstoned;
    std::atomic<uint32_t> wrap_count;
    uint64_t owner_tid;
    Sample *slots;              // allocated separately so capacity is runtime-tunable
};

extern std::atomic<TLS_Ring *> g_rings_head;
extern std::atomic<bool> g_capture_active;
extern thread_local TLS_Ring *t_ring;

/* Lifecycle entry points. */
bool init(const char *output_dir, size_t ring_capacity_pow2, uint64_t flush_interval_ms);
void deinit();

/* Sysvar update hook: change the output directory at runtime. The
   flusher thread picks up the new value at the next tick, mkdirs the
   path, reopens meta.json + per-method fds. Safe to call before init()
   (stashes the value; init() will read it). */
void set_output_dir(const char *output_dir);

/* Allocator helpers: exposed for unit tests. */
TLS_Ring *ring_alloc_for_thread();
void ring_free(TLS_Ring *r);

/* Push a sample. Called from PerfScope dtor. */
void push_sample(TLS_Ring *r, uint8_t method_id, uint8_t thread_id,
                 uint64_t enter_tsc, uint64_t exit_tsc);

#endif  /* TIDESDB_PERF */

}  /* namespace tidesdb_perf */
