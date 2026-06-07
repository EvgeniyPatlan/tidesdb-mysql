/*
  Unit tests for the per-thread perf ring (Sample + TLS_Ring + push_sample).

  These tests do NOT touch the MySQL server, the data dictionary, or a real
  TidesDB instance. They exercise the lock-free ring directly with a tiny
  capacity (256 slots) so wrap-around and overflow behaviour can be checked
  in a single forward push loop.

  Companion to docs/superpowers/specs/2026-06-04-perf-instrumentation-design.md.
*/
#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "tidesdb_perf_ring.h"
#include "tidesdb_perf_scope.h"

#if !TIDESDB_PERF
TEST(PerfRing, SkippedWithoutPerfFlag) { GTEST_SKIP() << "TIDESDB_PERF=0"; }
#else

namespace tdp = tidesdb_perf;

static tdp::TLS_Ring *make_ring(size_t capacity_pow2 = 8) {
    /* 256 samples; small enough to exercise wrap quickly. */
    auto *r = new tdp::TLS_Ring{};
    r->capacity = 1u << capacity_pow2;
    r->slots = new tdp::Sample[r->capacity]{};
    r->owner_tid = 1;
    return r;
}
static void free_ring(tdp::TLS_Ring *r) { delete[] r->slots; delete r; }

TEST(PerfRing, PushReadRoundTrip) {
    auto *r = make_ring(8);
    constexpr size_t N = 100;
    for (size_t i = 0; i < N; i++) {
        tdp::push_sample(r, uint8_t(tdp::MethodId::write_row), 1,
                          /*enter=*/i * 10, /*exit=*/i * 10 + 5);
    }
    EXPECT_EQ(r->write_idx.load(), N);
    EXPECT_EQ(r->read_idx.load(), 0u);
    /* Read back: slots [0..N-1] should match what we pushed. */
    for (size_t i = 0; i < N; i++) {
        EXPECT_EQ(r->slots[i].enter_tsc, i * 10);
        EXPECT_EQ(r->slots[i].exit_tsc,  i * 10 + 5);
        EXPECT_EQ(r->slots[i].method_id, uint8_t(tdp::MethodId::write_row));
    }
    free_ring(r);
}

TEST(PerfRing, WrapBehaviour) {
    auto *r = make_ring(8);                /* capacity 256 */
    size_t to_push = r->capacity + 100;    /* overrun by 100 */
    for (size_t i = 0; i < to_push; i++) {
        tdp::push_sample(r, uint8_t(tdp::MethodId::index_next), 1,
                          /*enter=*/i, /*exit=*/i + 1);
    }
    EXPECT_EQ(r->write_idx.load(), to_push);
    EXPECT_EQ(r->wrap_count.load(), 1u);
    /* The first 100 samples were overwritten by samples [256..355]. */
    /* slot[0] now holds sample 256 (enter == 256). */
    EXPECT_EQ(r->slots[0].enter_tsc, 256u);
    /* slot[99] holds sample 355. */
    EXPECT_EQ(r->slots[99].enter_tsc, 355u);
    /* slot[100] still holds the original sample 100 (not yet overwritten). */
    EXPECT_EQ(r->slots[100].enter_tsc, 100u);
    free_ring(r);
}

TEST(PerfRing, TombstoneDrained) {
    auto *r = make_ring(8);
    /* Push 50 samples. */
    for (size_t i = 0; i < 50; i++) {
        tdp::push_sample(r, uint8_t(tdp::MethodId::commit), 1, i, i + 1);
    }
    r->tombstoned.store(true);
    /* After tombstone: the flusher should be able to read read_idx..write_idx. */
    EXPECT_EQ(r->write_idx.load() - r->read_idx.load(), 50u);
    free_ring(r);
}

TEST(PerfRing, AllocSelfRegistersInGlobalList) {
    /* Reset head; allocate two rings; expect both in g_rings_head chain. */
    tdp::g_rings_head.store(nullptr);
    auto *r1 = tdp::ring_alloc_for_thread();
    auto *r2 = tdp::ring_alloc_for_thread();
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);

    /* Walk the chain. */
    int count = 0;
    for (auto *p = tdp::g_rings_head.load(); p != nullptr; p = p->next.load()) {
        count++;
    }
    EXPECT_EQ(count, 2);

    tdp::ring_free(r1);
    tdp::ring_free(r2);
    tdp::g_rings_head.store(nullptr);
}

TEST(PerfScope, NoCaptureZeroPushes) {
    /* g_capture_active = false: PerfScope dtor must not write to any ring. */
    tdp::g_capture_active.store(false);
    tdp::t_ring = nullptr;
    {
        TDB_PERF_SCOPE(write_row);
    }
    EXPECT_EQ(tdp::t_ring, nullptr);  /* no ring allocated */
}

TEST(PerfScope, CaptureOnPushesOneSample) {
    tdp::g_rings_head.store(nullptr);
    tdp::t_ring = nullptr;
    tdp::g_capture_active.store(true);
    {
        TDB_PERF_SCOPE(write_row);
    }
    ASSERT_NE(tdp::t_ring, nullptr);
    EXPECT_EQ(tdp::t_ring->write_idx.load(), 1u);
    EXPECT_EQ(tdp::t_ring->slots[0].method_id, uint8_t(tdp::MethodId::write_row));
    /* enter <= exit, both nonzero. */
    EXPECT_LE(tdp::t_ring->slots[0].enter_tsc, tdp::t_ring->slots[0].exit_tsc);
    EXPECT_GT(tdp::t_ring->slots[0].enter_tsc, 0u);

    tdp::g_capture_active.store(false);
    tdp::ring_free(tdp::t_ring);
    tdp::t_ring = nullptr;
    tdp::g_rings_head.store(nullptr);
}

TEST(PerfScope, NestedScopesOrdered) {
    tdp::g_rings_head.store(nullptr);
    tdp::t_ring = nullptr;
    tdp::g_capture_active.store(true);
    {
        TDB_PERF_SCOPE(write_row);                     // outer
        {
            TDB_PERF_SCOPE(serialize_row);             // inner
        }
    }
    ASSERT_NE(tdp::t_ring, nullptr);
    /* Inner finishes (dtor runs) BEFORE outer; rings push in dtor order. */
    ASSERT_GE(tdp::t_ring->write_idx.load(), 2u);
    const auto &inner = tdp::t_ring->slots[0];
    const auto &outer = tdp::t_ring->slots[1];
    EXPECT_EQ(inner.method_id, uint8_t(tdp::MethodId::serialize_row));
    EXPECT_EQ(outer.method_id, uint8_t(tdp::MethodId::write_row));
    /* Inner is contained in outer (enter >, exit <). */
    EXPECT_GE(inner.enter_tsc, outer.enter_tsc);
    EXPECT_LE(inner.exit_tsc,  outer.exit_tsc);

    tdp::g_capture_active.store(false);
    tdp::ring_free(tdp::t_ring); tdp::t_ring = nullptr;
    tdp::g_rings_head.store(nullptr);
}

TEST(PerfRing, ConcurrentPushSingleReader) {
    auto *r = make_ring(12);  /* 4096 samples */
    constexpr size_t kPerThread = 1000;
    constexpr int kThreads = 4;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&, t]() {
            for (size_t i = 0; i < kPerThread; i++) {
                tdp::push_sample(r, uint8_t(tdp::MethodId::index_next),
                                  static_cast<uint8_t>(t), i, i + 1);
            }
        });
    }
    for (auto &th : threads) th.join();
    EXPECT_EQ(r->write_idx.load(), uint64_t(kPerThread) * kThreads);
    free_ring(r);
}

#endif  /* TIDESDB_PERF */
