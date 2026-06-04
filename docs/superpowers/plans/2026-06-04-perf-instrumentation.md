# Perf Instrumentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the cycle-accurate, layer-by-layer perf instrumentation that lets us attribute time spent inside the TidesDB-MySQL plugin during real workloads, then port the same primitive to TideSQL so we can run identical workloads side-by-side and produce a `(MySQL, MariaDB, Δ)` per-method diff.

**Architecture:** Thread-local 64K-sample ring buffer per worker thread, written by an inlined RAII guard (`TDB_PERF_SCOPE(MethodId)`) that pairs `__rdtsc()` calls at entry and exit. A 1 Hz background flusher thread snapshots each ring lock-free, buckets samples by method id, and `pwritev`s them to per-method binary files. An offline Python tool reads the binaries and emits markdown/CSV reports plus a `--compare` diff between two SUTs. The whole instrumentation is gated by a runtime sysvar (`tidesdb_perf_capture`, default OFF) and a build flag (`-DTIDESDB_PERF=1`, default OFF in release, ON in debug/perf images).

**Tech Stack:** MySQL 9.7 storage-engine plugin (C++17 in `plugin/`), `__rdtsc()` (x86_64), `pwritev`, `std::thread`, `std::atomic`. Offline tool: Python 3.11 + numpy + pandas (already a HammerDB harness dep, available in `tidesdb/mwbench` image). Same image set the v0.4.0 work used: `tides-builder`, `tidesdb/mysql-mtr:9.7-task8`.

**Spec:** `docs/superpowers/specs/2026-06-04-perf-instrumentation-design.md` (commit `6678426`). Read §5 for component types, §6 for data flow, §7 for failure modes before implementing.

---

## Environment & build commands

Docker access requires `sg docker -c "..."` wrapping (user not in docker group).

**Plugin-only fast build (~5 min):**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder /work/scripts/build-plugin.sh"
```

To pass the perf build flag through CMake, prefix `build-plugin.sh` with `TIDESDB_PERF=1`:

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -e TIDESDB_PERF=1 -v $(pwd):/work tides-builder /work/scripts/build-plugin.sh"
```

(The plan adds support for this env var to `CMakeLists.txt` in Task 1.)

**MTR test invocation:**

```bash
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb \
  tidesdb/mysql-mtr:9.7-task8 \
  bash -lc 'cd /build/mysql-server/build && \
            cmake -DTIDESDB_PERF=1 -S /build/mysql-server -B . && \
            cmake --build . --target tidesdb -j 2>&1 | tail -3 && \
            cd mysql-test && ./mtr --suite=tidesdb --force <TEST_NAME>'"
```

Mount at `suite/tidesdb` (NOT `tidesdb-extra`). The image to use is `tidesdb/mysql-mtr:9.7-task8`.

**gtest invocation:**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder \
  bash -lc 'apt-get install -y libgtest-dev libgmock-dev > /dev/null 2>&1; \
            cd /work/vendor/mysql-server/build && cmake --build . --target tidesdb_atomic_ddl_tests -j && \
            ./storage/tidesdb/tests/tidesdb_atomic_ddl_tests --gtest_filter=<FILTER>'"
```

**Python offline-tool tests:**

```bash
cd tools/tidesdb-perf-analyze && python3 -m pytest tests/
```

**Vendor sync (mandatory before each build that touches plugin source):**

`scripts/setup-workspace.sh` mirrors `plugin/` into `vendor/mysql-server/storage/tidesdb/` and `plugin/tests/` into `vendor/mysql-server/storage/tidesdb/tests/`. Edit at `plugin/`; let the script do the copy. Re-run after every change before the next build.

**Commits:** atomic per task; messages follow `feat(perf): <what>` / `test(perf): <what>` / `chore(perf): <what>`. No `Co-Authored-By` trailers (project-wide policy).

**Push policy:** No auto-push. Commits stay local until explicit `git push`. Branch is `feat/perf-instrumentation` (create at Task 1).

---

## File structure

| File | Role | Action |
|---|---|---|
| `plugin/tidesdb_perf_ring.h` | `Sample`, `MethodId` enum, `TLS_Ring`, globals (`g_rings_head`, `g_capture_active`) | Create |
| `plugin/tidesdb_perf_ring.cc` | Ring alloc, tombstone draining, flusher thread, `meta.json` writer | Create |
| `plugin/tidesdb_perf_scope.h` | `PerfScope` RAII + `TDB_PERF_SCOPE` macro | Create |
| `plugin/ha_tidesdb.cc` | 24 `TDB_PERF_SCOPE` sites + 4 sysvar declarations + `tidesdb_perf_init/deinit` calls | Modify |
| `plugin/tidesdb_inplace_alter.cc` | 4 sites on the inplace ALTER virtuals | Modify |
| `plugin/tidesdb_fts.cc` | 4 sites on the deeper FTS helpers we want to attribute | Modify |
| `plugin/CMakeLists.txt` | Add the new TUs to plugin source list; honour `-DTIDESDB_PERF=1` | Modify |
| `plugin/tests/test_perf_ring.cc` | 6 gtest cases (PushReadRoundTrip, WrapBehaviour, ConcurrentPushSingleReader, NoCaptureZeroAllocation, NestedScopes, TombstoneDrained) | Create |
| `plugin/tests/CMakeLists.txt` | Add `test_perf_ring.cc` source | Modify |
| `tools/tidesdb-perf-analyze/__main__.py` | Read `.bin` files, emit markdown + CSV reports | Create |
| `tools/tidesdb-perf-analyze/compare.py` | `--compare` mode for side-by-side diff | Create |
| `tools/tidesdb-perf-analyze/tests/test_analyze.py` | 2 unit tests (HistogramFromSamples, MultiFileMerge) | Create |
| `tools/tidesdb-perf-analyze/requirements.txt` | numpy + pandas | Create |
| `mysql-test-suite/t/tidesdb_perf_*.test` + matching `.result` | 7 MTR tests | Create (one per test) |
| `mysql-test-suite/include/tidesdb_perf_helpers.inc` | Shared setup for the new tests | Create |
| `bench/perf/run-perf-capture.sh` | Integration harness: start mysqld → run HammerDB → flush → docker cp → analyse | Create |
| `vendor/tidesql/perf_ring.{h,cc}` + `vendor/tidesql/perf_scope.h` | Vendored side-by-side port | Create |
| TideSQL `ha_tidesdb.cc` patch | 24 instrumentation sites mirroring MySQL plugin | Patched into TideSQL repo locally |

---

## Task 1: Skeleton TUs + CMake build flag

**Files:**
- Create: `plugin/tidesdb_perf_ring.h`
- Create: `plugin/tidesdb_perf_ring.cc`
- Create: `plugin/tidesdb_perf_scope.h`
- Modify: `plugin/CMakeLists.txt`

- [ ] **Step 1: Create branch `feat/perf-instrumentation`**

```bash
cd /home/corvin/tidesdb-mysql
git checkout main
git pull --ff-only
git checkout -b feat/perf-instrumentation
```

- [ ] **Step 2: Create `plugin/tidesdb_perf_ring.h` with empty skeleton**

```cpp
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

/* Allocator helpers: exposed for unit tests. */
TLS_Ring *ring_alloc_for_thread();
void ring_free(TLS_Ring *r);

/* Push a sample. Called from PerfScope dtor. */
void push_sample(TLS_Ring *r, uint8_t method_id, uint8_t thread_id,
                 uint64_t enter_tsc, uint64_t exit_tsc);

#endif  /* TIDESDB_PERF */

}  /* namespace tidesdb_perf */
```

- [ ] **Step 3: Create `plugin/tidesdb_perf_scope.h` with the macro stub**

```cpp
/*
  TDB_PERF_SCOPE(MethodId): RAII guard that samples enter/exit timestamps
  into the calling thread's perf ring.

  Compiled in under -DTIDESDB_PERF=1; compiles to ((void)0) when off.

  See docs/superpowers/specs/2026-06-04-perf-instrumentation-design.md §5.
*/
#pragma once

#include <cstdint>

namespace tidesdb_perf {

enum class MethodId : uint8_t {
    // Handler vtable -- DML row hotpath (8)
    write_row = 1, update_row, delete_row,
    index_read_map, index_next, index_prev, rnd_next, rnd_pos,
    // Handler vtable -- txn control (8)
    external_lock, start_stmt, store_lock,
    commit, rollback,
    savepoint_set, savepoint_release, savepoint_rollback,
    // Handler vtable -- table lifecycle (6)
    open, close, info, table_flags_cache_init, create, delete_table,
    // Inplace ALTER (4)
    check_if_supported_inplace_alter,
    prepare_inplace_alter_table,
    inplace_alter_table,
    commit_inplace_alter_table,
    // Plugin-private deeper helpers (6)
    serialize_row, deserialize_row,
    key_copy_to_comparable, pk_from_record,
    encrypt_row_into, decrypt_row,
};
constexpr uint8_t kMethodCount = 32;

}  // namespace tidesdb_perf

#if TIDESDB_PERF

#include <x86intrin.h>            // __rdtsc()

#include "tidesdb_perf_ring.h"

namespace tidesdb_perf {

class PerfScope {
    MethodId m_id;
    uint64_t m_enter;
public:
    explicit PerfScope(MethodId id) noexcept : m_id(id), m_enter(__rdtsc()) {}
    ~PerfScope() noexcept;        // out-of-line: see tidesdb_perf_ring.cc
};

}  // namespace tidesdb_perf

#define TDB_PERF_SCOPE_CONCAT_(a, b) a##b
#define TDB_PERF_SCOPE_CONCAT(a, b) TDB_PERF_SCOPE_CONCAT_(a, b)
#define TDB_PERF_SCOPE(id) \
    ::tidesdb_perf::PerfScope TDB_PERF_SCOPE_CONCAT(_tdb_perf_, __LINE__) { \
        ::tidesdb_perf::MethodId::id }

#else  /* !TIDESDB_PERF */

#define TDB_PERF_SCOPE(id) ((void)0)

#endif
```

- [ ] **Step 4: Create `plugin/tidesdb_perf_ring.cc` with empty stubs**

```cpp
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
```

- [ ] **Step 5: Modify `plugin/CMakeLists.txt` to add the new TUs + the `-DTIDESDB_PERF=1` flag**

Find the `set(TIDESDB_PLUGIN_SOURCES ...)` block (grep with `grep -n "tidesdb_engine_context.cc" plugin/CMakeLists.txt`) and append `tidesdb_perf_ring.cc`. Append the build-flag section near the top of the file (right after `project()` if present, otherwise at the top):

```cmake
# -DTIDESDB_PERF=1 turns on the layer-by-layer perf instrumentation.
# Default OFF; turn ON for the perf-image variant. Honour the cmake -D and
# the TIDESDB_PERF environment variable (the latter is what scripts/build-plugin.sh
# forwards via -e TIDESDB_PERF=1).
option(TIDESDB_PERF "Compile in TidesDB perf instrumentation" OFF)
if(DEFINED ENV{TIDESDB_PERF} AND NOT "$ENV{TIDESDB_PERF}" STREQUAL "0")
    set(TIDESDB_PERF ON CACHE BOOL "" FORCE)
endif()
if(TIDESDB_PERF)
    add_compile_definitions(TIDESDB_PERF=1)
    message(STATUS "[TIDESDB] perf instrumentation ON")
else()
    add_compile_definitions(TIDESDB_PERF=0)
endif()
```

- [ ] **Step 6: Sync source into vendor + build to verify the skeleton compiles**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder \
  bash -lc 'cp -v /work/plugin/tidesdb_perf_*.{h,cc} /work/vendor/mysql-server/storage/tidesdb/ 2>&1 | tail -3'"
sg docker -c "docker run --rm --user $(id -u):$(id -g) -e TIDESDB_PERF=1 -v $(pwd):/work tides-builder /work/scripts/build-plugin.sh 2>&1 | tail -3"
```

Expected: `[TIDESDB] perf instrumentation ON` in cmake output and `ha_tidesdb.so` builds clean. Confirm symbols compiled in:

```bash
nm vendor/mysql-server/build/plugin_output_directory/ha_tidesdb.so 2>/dev/null | grep -c tidesdb_perf
```

Expected count: > 0.

- [ ] **Step 7: Also build with the flag OFF to confirm zero-cost branch**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder /work/scripts/build-plugin.sh 2>&1 | tail -3"
nm vendor/mysql-server/build/plugin_output_directory/ha_tidesdb.so 2>/dev/null | grep -c tidesdb_perf
```

Expected count after the OFF build: 0 (the macros expand to `(void)0` and the .cc body is `#if TIDESDB_PERF`).

- [ ] **Step 8: Commit**

```bash
git add plugin/tidesdb_perf_ring.h plugin/tidesdb_perf_ring.cc plugin/tidesdb_perf_scope.h plugin/CMakeLists.txt
git commit -m "feat(perf): skeleton tidesdb_perf TUs + CMake -DTIDESDB_PERF flag

Adds empty Sample / MethodId / TLS_Ring declarations + the PerfScope
RAII shell. TDB_PERF_SCOPE macro compiles to (void)0 when the flag is
off, so production builds pay zero cost. Verified both flag states
build clean and produce the expected symbol presence/absence."
```

---

## Task 2: Sysvars

**Files:**
- Modify: `plugin/ha_tidesdb.cc` (declare + register 4 sysvars; call into perf::init/deinit)
- Create: `mysql-test-suite/include/tidesdb_perf_helpers.inc`
- Create: `mysql-test-suite/t/tidesdb_perf_sysvar_basic.test` + matching `.result`

- [ ] **Step 1: Add 4 sysvar declarations near the other `MYSQL_SYSVAR_*` blocks in `plugin/ha_tidesdb.cc`**

Find the section that declares e.g. `MYSQL_SYSVAR_BOOL(atomic_ddl_strict, ...)`. Add directly under it:

```cpp
static my_bool tidesdb_perf_capture = 0;
static MYSQL_SYSVAR_BOOL(perf_capture, tidesdb_perf_capture,
                         PLUGIN_VAR_OPCMDARG,
                         "Master switch for layer-by-layer perf instrumentation. "
                         "Default OFF. Effective only in builds compiled with "
                         "-DTIDESDB_PERF=1.",
                         /*check=*/NULL, /*update=*/NULL, /*default=*/false);

static char *tidesdb_perf_output_dir = nullptr;
static MYSQL_SYSVAR_STR(perf_output_dir, tidesdb_perf_output_dir,
                        PLUGIN_VAR_OPCMDARG,
                        "Directory where per-method perf .bin files land. "
                        "Default /var/lib/mysql/tidesdb-perf/. Honoured at "
                        "next flush tick after change.",
                        /*check=*/NULL, /*update=*/NULL,
                        /*default=*/"/var/lib/mysql/tidesdb-perf");

static unsigned long tidesdb_perf_ring_capacity_pow2 = 16;
static MYSQL_SYSVAR_ULONG(perf_ring_capacity_pow2, tidesdb_perf_ring_capacity_pow2,
                          PLUGIN_VAR_READONLY,
                          "Per-thread ring capacity in samples = 1 << pow2. "
                          "Default 16 (= 65536 samples = ~1.5 MiB/thread). "
                          "Server-start only.",
                          /*check=*/NULL, /*update=*/NULL,
                          /*default=*/16, /*min=*/8, /*max=*/24, /*block=*/1);

static unsigned long tidesdb_perf_flush_interval_ms = 1000;
static MYSQL_SYSVAR_ULONG(perf_flush_interval_ms, tidesdb_perf_flush_interval_ms,
                          PLUGIN_VAR_OPCMDARG,
                          "Flusher tick interval in milliseconds. "
                          "Default 1000.",
                          /*check=*/NULL, /*update=*/NULL,
                          /*default=*/1000, /*min=*/100, /*max=*/60000, /*block=*/1);
```

Add the four to the sysvar registration array (the `static SYS_VAR *tidesdb_system_variables[]` block):

```cpp
    MYSQL_SYSVAR(perf_capture),
    MYSQL_SYSVAR(perf_output_dir),
    MYSQL_SYSVAR(perf_ring_capacity_pow2),
    MYSQL_SYSVAR(perf_flush_interval_ms),
```

- [ ] **Step 2: Wire `tidesdb_perf::init/deinit` into `tidesdb_init_func` / `tidesdb_deinit_func` in `plugin/ha_tidesdb.cc`**

At the very top of the file (with the other `#include` blocks):

```cpp
#include "tidesdb_perf_ring.h"
```

In `tidesdb_init_func`, after the existing `g_engine_ctx.sdi->init()` block (Task 5 of atomic-DDL), add:

```cpp
#if TIDESDB_PERF
    if (!tidesdb_perf::init(tidesdb_perf_output_dir,
                            static_cast<size_t>(tidesdb_perf_ring_capacity_pow2),
                            static_cast<uint64_t>(tidesdb_perf_flush_interval_ms))) {
        sql_print_warning("[TIDESDB-PERF] init failed; perf capture will be inactive");
    } else {
        tidesdb_perf::g_capture_active.store(tidesdb_perf_capture,
                                              std::memory_order_release);
    }
#endif
```

In `tidesdb_deinit_func`, BEFORE `tidesdb_close` (so the flusher can drain while engine still alive):

```cpp
#if TIDESDB_PERF
    tidesdb_perf::deinit();
#endif
```

- [ ] **Step 3: Create `mysql-test-suite/include/tidesdb_perf_helpers.inc`**

```
# Shared setup for tidesdb_perf_* tests.

--source include/have_tidesdb.inc

# These tests need TIDESDB_PERF=1 in the build. The macros are no-ops
# otherwise, so each test will mark itself skipped if perf_capture isn't
# in SHOW VARIABLES.
let $perf_present = `SELECT COUNT(*) FROM information_schema.SESSION_VARIABLES
                      WHERE VARIABLE_NAME='tidesdb_perf_capture'`;
if (!$perf_present) {
  --skip Test requires plugin built with -DTIDESDB_PERF=1
}

# Default OFF and clean output dir for every test.
SET @save_capture = @@global.tidesdb_perf_capture;
SET GLOBAL tidesdb_perf_capture = OFF;
```

- [ ] **Step 4: Write failing test `mysql-test-suite/t/tidesdb_perf_sysvar_basic.test`**

```
--source include/tidesdb_perf_helpers.inc

# All 4 sysvars surface via SHOW VARIABLES.
SELECT VARIABLE_NAME
  FROM information_schema.SESSION_VARIABLES
  WHERE VARIABLE_NAME IN (
    'tidesdb_perf_capture',
    'tidesdb_perf_output_dir',
    'tidesdb_perf_ring_capacity_pow2',
    'tidesdb_perf_flush_interval_ms')
  ORDER BY VARIABLE_NAME;

# perf_capture is settable runtime.
SET GLOBAL tidesdb_perf_capture = ON;
SELECT @@global.tidesdb_perf_capture AS on_value;
SET GLOBAL tidesdb_perf_capture = OFF;
SELECT @@global.tidesdb_perf_capture AS off_value;

# ring_capacity_pow2 is READONLY (cannot be set runtime).
--error ER_INCORRECT_GLOBAL_LOCAL_VAR
SET GLOBAL tidesdb_perf_ring_capacity_pow2 = 12;

SET GLOBAL tidesdb_perf_capture = @save_capture;
```

Expected `.result`:

```
VARIABLE_NAME
tidesdb_perf_capture
tidesdb_perf_flush_interval_ms
tidesdb_perf_output_dir
tidesdb_perf_ring_capacity_pow2
on_value
1
off_value
0
```

- [ ] **Step 5: Sync source + rebuild + run test — expect PASS**

```bash
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb \
  tidesdb/mysql-mtr:9.7-task8 \
  bash -lc 'cd /build/mysql-server/build && \
            cmake -DTIDESDB_PERF=1 -S /build/mysql-server -B . > /dev/null 2>&1 && \
            cmake --build . --target tidesdb -j > /dev/null && \
            cd mysql-test && ./mtr --suite=tidesdb tidesdb_perf_sysvar_basic'"
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add plugin/ha_tidesdb.cc \
        mysql-test-suite/include/tidesdb_perf_helpers.inc \
        mysql-test-suite/t/tidesdb_perf_sysvar_basic.test \
        mysql-test-suite/r/tidesdb_perf_sysvar_basic.result
git commit -m "feat(perf): 4 sysvars + perf::init/deinit wiring

tidesdb_perf_capture (BOOL, default OFF, runtime),
tidesdb_perf_output_dir (STR, runtime),
tidesdb_perf_ring_capacity_pow2 (INT 8-24, server-start),
tidesdb_perf_flush_interval_ms (INT 100-60000, runtime).

Wired into tidesdb_init_func and tidesdb_deinit_func behind
TIDESDB_PERF. Shared MTR helper skips perf tests cleanly on a build
without the flag."
```

---

## Task 3: `Sample` + `TLS_Ring` + first three gtest cases

**Files:**
- Modify: `plugin/tidesdb_perf_ring.h` (already declares the struct; verify offsets)
- Modify: `plugin/tidesdb_perf_ring.cc` (implement `push_sample`)
- Create: `plugin/tests/test_perf_ring.cc`
- Modify: `plugin/tests/CMakeLists.txt`

- [ ] **Step 1: Add `test_perf_ring.cc` to `plugin/tests/CMakeLists.txt`**

Find the `add_executable(tidesdb_atomic_ddl_tests ...)` block and add `test_perf_ring.cc` to the source list (next to `test_sdi_pack_key.cc`). The same gtest binary will host both the atomic-DDL tests and the perf tests.

Also append `${CMAKE_SOURCE_DIR}/storage/tidesdb/tidesdb_perf_ring.cc` to the source list so the tests link against the real ring implementation.

- [ ] **Step 2: Write failing test `plugin/tests/test_perf_ring.cc`**

```cpp
#include <gtest/gtest.h>
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

#endif  /* TIDESDB_PERF */
```

- [ ] **Step 3: Build + run — verify the 3 tests FAIL (push_sample is empty)**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder \
  bash -lc 'apt-get install -y libgtest-dev libgmock-dev > /dev/null 2>&1; \
            cd /work/vendor/mysql-server/build && \
            cmake -DTIDESDB_PERF=1 -S /work/vendor/mysql-server -B . > /dev/null 2>&1 && \
            cmake --build . --target tidesdb_atomic_ddl_tests > /dev/null && \
            ./storage/tidesdb/tests/tidesdb_atomic_ddl_tests --gtest_filter=PerfRing.*'"
```

Expected: 3/3 FAIL.

- [ ] **Step 4: Implement `push_sample` and `Sample` round-trip in `plugin/tidesdb_perf_ring.cc`**

Replace the empty `push_sample` body:

```cpp
void push_sample(TLS_Ring *r, uint8_t method_id, uint8_t thread_id,
                 uint64_t enter_tsc, uint64_t exit_tsc) {
    if (!r) return;
    uint64_t w = r->write_idx.fetch_add(1, std::memory_order_relaxed);
    uint64_t slot_idx = w & (r->capacity - 1);
    /* Detect wrap: if we lapped read_idx by a full capacity,
       bump wrap_count once. */
    if (w >= r->capacity) {
        uint64_t prev_w = w;
        /* prev_w just wrapped past (read_idx + capacity); bump if exactly so. */
        if ((prev_w & (r->capacity - 1)) == 0) {
            r->wrap_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    Sample &s = r->slots[slot_idx];
    s.method_id = method_id;
    s.thread_id = thread_id;
    s.reserved  = 0;
    s.enter_tsc = enter_tsc;
    s.exit_tsc  = exit_tsc;
}
```

- [ ] **Step 5: Re-run — expect 3/3 PASS**

```bash
# Same docker command as Step 3.
```

- [ ] **Step 6: Commit**

```bash
git add plugin/tidesdb_perf_ring.h plugin/tidesdb_perf_ring.cc \
        plugin/tests/test_perf_ring.cc plugin/tests/CMakeLists.txt
git commit -m "feat(perf): Sample + TLS_Ring push_sample with wrap detection

push_sample is lock-free relative to readers: writers fetch_add the
write_idx, slot index is index & (capacity - 1), wrap is detected when
write_idx passes a capacity boundary. wrap_count surfaces lossy
sampling to the offline tool.

Three gtest cases: PushReadRoundTrip (100 samples, slot indices match),
WrapBehaviour (256+100 samples, first 100 overwritten, wrap_count == 1),
TombstoneDrained (50 samples queued for the flusher post-tombstone)."
```

---

## Task 4: Lock-free `g_rings_head` push + alloc + `NoCaptureZeroAllocation`

**Files:**
- Modify: `plugin/tidesdb_perf_ring.cc` (`ring_alloc_for_thread`, `g_rings_head` integration)
- Modify: `plugin/tests/test_perf_ring.cc` (new case)

- [ ] **Step 1: Write the failing test `PerfRing.AllocSelfRegisters` in `plugin/tests/test_perf_ring.cc`**

Append to the same file:

```cpp
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
```

- [ ] **Step 2: Run — expect 3 FAIL**

```bash
# Same docker command as Task 3 Step 3, filter PerfRing.AllocSelf* + PerfScope.*
```

- [ ] **Step 3: Implement `ring_alloc_for_thread` + lock-free push + `PerfScope` dtor**

In `plugin/tidesdb_perf_ring.cc`:

```cpp
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
```

And the `PerfScope::~PerfScope()` (in the same .cc, inside `namespace tidesdb_perf`):

```cpp
PerfScope::~PerfScope() noexcept {
    if (!g_capture_active.load(std::memory_order_relaxed)) return;
    uint64_t exit = __rdtsc();
    if (!t_ring) t_ring = ring_alloc_for_thread();
    if (!t_ring) return;  /* OOM */
    /* Cheap thread-id hash: lower 8 bits of owner_tid. */
    uint8_t tid = static_cast<uint8_t>(t_ring->owner_tid);
    push_sample(t_ring, static_cast<uint8_t>(m_id), tid, m_enter, exit);
}
```

- [ ] **Step 4: Re-run — expect all 3 PASS**

- [ ] **Step 5: Commit**

```bash
git add plugin/tidesdb_perf_ring.cc plugin/tests/test_perf_ring.cc
git commit -m "feat(perf): lock-free ring registration + PerfScope dtor

ring_alloc_for_thread allocates a TLS_Ring + its slot array,
self-registers via a compare_exchange loop on g_rings_head, and
returns. PerfScope::dtor short-circuits when capture is off; otherwise
samples one entry per scope.

NoCaptureZeroPushes verifies the gating branch leaves t_ring untouched
when capture is off; CaptureOnPushesOneSample exercises the full
write path."
```

---

## Task 5: Nested scopes + concurrent push

**Files:**
- Modify: `plugin/tests/test_perf_ring.cc` (new cases)

- [ ] **Step 1: Add the two failing tests**

```cpp
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
```

- [ ] **Step 2: Build + run — expect PASS (the existing implementation already supports both)**

```bash
# Same docker command as Task 3 Step 3.
# Filter on PerfScope.NestedScopesOrdered and PerfRing.ConcurrentPushSingleReader.
```

- [ ] **Step 3: Commit**

```bash
git add plugin/tests/test_perf_ring.cc
git commit -m "test(perf): nested scopes + concurrent push round-trip

NestedScopesOrdered verifies inner-dtor-before-outer-dtor sample
ordering plus the containment relationship (inner.[enter,exit] is
inside outer.[enter,exit]).

ConcurrentPushSingleReader runs 4 producer threads x 1000 samples =
4000 total; write_idx after join must be exactly 4000. Run under TSAN
in CI: no data races on the slot array. (TSAN job not added yet; the
test will surface failures on subsequent CI introductions.)"
```

---

## Task 6: Flusher thread + meta.json writer

**Files:**
- Modify: `plugin/tidesdb_perf_ring.cc` (`init` / `deinit` / flusher loop / per-method file open / `meta.json`)

- [ ] **Step 1: Write the failing MTR test `mysql-test-suite/t/tidesdb_perf_on_files_appear.test`**

```
--source include/tidesdb_perf_helpers.inc

# Clean output dir.
SET GLOBAL tidesdb_perf_output_dir = '/tmp/perf-test-on-files';
--exec rm -rf /tmp/perf-test-on-files

CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
SET GLOBAL tidesdb_perf_capture = ON;
INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c'),(4,'d'),(5,'e'),
                    (6,'f'),(7,'g'),(8,'h'),(9,'i'),(10,'j');
# Sleep > 2 * flush_interval so the flusher has surely landed at least once.
--sleep 3
SET GLOBAL tidesdb_perf_capture = OFF;

--exec ls -la /tmp/perf-test-on-files 2>&1 | grep -E "01-write_row|meta.json" | wc -l

DROP TABLE t;
SET GLOBAL tidesdb_perf_capture = @save_capture;
```

Expected `.result`:

```
2
```

(One line for `*-01-write_row.bin`, one for `meta.json`.)

- [ ] **Step 2: Run — expect FAIL (init is a no-op stub)**

```bash
# Same MTR docker command as Task 2 Step 5, filter tidesdb_perf_on_files_appear.
```

- [ ] **Step 3: Implement `init` / `deinit` / flusher thread / `meta.json` in `plugin/tidesdb_perf_ring.cc`**

Add at the top of the file (inside `namespace tidesdb_perf`):

```cpp
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <pthread.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
struct PerfModuleState {
    std::string output_dir;
    size_t      ring_capacity_pow2 = TLS_Ring::kCapacityPow2Default;
    uint64_t    flush_interval_ms  = 1000;
    int         method_fds[kMethodCount + 1] = {0};  /* index 0 unused; method ids start at 1 */
    std::thread flusher;
    std::atomic<bool> shutdown{false};
    std::mutex  shutdown_mu;
    std::condition_variable shutdown_cv;
    int         meta_fd = -1;
    double      tsc_ghz = 0.0;
};
PerfModuleState g_state;

static double measure_tsc_ghz() {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t tsc0 = __rdtsc();
    /* Spin briefly so the measurement window dominates jitter. */
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(20)) {
        /* spin */
    }
    auto t1 = std::chrono::steady_clock::now();
    uint64_t tsc1 = __rdtsc();
    double dt_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return (double(tsc1 - tsc0) / dt_ns);
}

static void write_meta_json() {
    char path[512];
    snprintf(path, sizeof(path), "%s/%d-meta.json",
             g_state.output_dir.c_str(), getpid());
    g_state.meta_fd = ::open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (g_state.meta_fd < 0) {
        sql_print_warning("[TIDESDB-PERF] cannot create meta.json at %s: %s",
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
        g_state.tsc_ghz, size_t(1u) << g_state.ring_capacity_pow2,
        unsigned(kMethodCount), getpid());
    ::write(g_state.meta_fd, buf, size_t(n));
    ::close(g_state.meta_fd);
    g_state.meta_fd = -1;
}

static void open_method_files() {
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
    for (uint8_t i = 1; i <= kMethodCount; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%d-%s.bin",
                 g_state.output_dir.c_str(), getpid(), names[i]);
        int fd = ::open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd < 0) {
            sql_print_warning("[TIDESDB-PERF] cannot open %s: %s",
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

static void flusher_loop() {
    /* Pin to one core so rdtsc deltas don't migrate. Best-effort. */
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(0, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

    while (!g_state.shutdown.load(std::memory_order_acquire)) {
        /* Walk g_rings_head. */
        std::vector<std::vector<Sample>> by_method(kMethodCount + 1);
        TLS_Ring *r = g_rings_head.load(std::memory_order_acquire);
        while (r) {
            uint64_t w = r->write_idx.load(std::memory_order_acquire);
            uint64_t rd = r->read_idx.load(std::memory_order_relaxed);
            uint64_t to_drain = std::min<uint64_t>(w - rd, r->capacity);
            for (uint64_t i = 0; i < to_drain; i++) {
                const Sample &s = r->slots[(rd + i) & (r->capacity - 1)];
                if (s.method_id >= 1 && s.method_id <= kMethodCount) {
                    by_method[s.method_id].push_back(s);
                }
            }
            r->read_idx.store(w, std::memory_order_release);
            r = r->next.load(std::memory_order_acquire);
        }
        /* Append each method bucket to its file. */
        for (uint8_t i = 1; i <= kMethodCount; i++) {
            if (by_method[i].empty() || g_state.method_fds[i] <= 0) continue;
            ::write(g_state.method_fds[i], by_method[i].data(),
                    by_method[i].size() * sizeof(Sample));
        }
        std::unique_lock<std::mutex> lk(g_state.shutdown_mu);
        g_state.shutdown_cv.wait_for(lk,
            std::chrono::milliseconds(g_state.flush_interval_ms));
    }
}

}  /* anonymous namespace */

bool init(const char *output_dir, size_t ring_capacity_pow2, uint64_t flush_interval_ms) {
    g_state.output_dir = output_dir ? output_dir : "/var/lib/mysql/tidesdb-perf";
    g_state.ring_capacity_pow2 = ring_capacity_pow2;
    g_state.flush_interval_ms = flush_interval_ms;
    /* mkdir -p. */
    if (::mkdir(g_state.output_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        sql_print_warning("[TIDESDB-PERF] cannot create output dir %s: %s",
                          g_state.output_dir.c_str(), std::strerror(errno));
        return false;
    }
    g_state.tsc_ghz = measure_tsc_ghz();
    write_meta_json();
    open_method_files();
    g_state.shutdown.store(false);
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
    /* Final synchronous flush: walk rings one more time so the last
       batch lands. */
    flusher_loop();  /* shutdown=true so exits immediately after one pass */
    close_method_files();
    /* Drain all rings. */
    TLS_Ring *r = g_rings_head.load();
    while (r) {
        TLS_Ring *next = r->next.load();
        ring_free(r);
        r = next;
    }
    g_rings_head.store(nullptr);
}
```

Includes the `sys/stat.h`, `errno.h` headers at the top of the file.

- [ ] **Step 4: Sync source + rebuild + run MTR — expect PASS**

```bash
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb \
  tidesdb/mysql-mtr:9.7-task8 \
  bash -lc 'cd /build/mysql-server/build && \
            cmake -DTIDESDB_PERF=1 -S /build/mysql-server -B . > /dev/null 2>&1 && \
            cmake --build . --target tidesdb -j > /dev/null && \
            cd mysql-test && ./mtr --suite=tidesdb tidesdb_perf_on_files_appear'"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add plugin/tidesdb_perf_ring.cc \
        mysql-test-suite/t/tidesdb_perf_on_files_appear.test \
        mysql-test-suite/r/tidesdb_perf_on_files_appear.result
git commit -m "feat(perf): flusher thread + per-method .bin files + meta.json

init() calibrates tsc_ghz (20 ms spin between rdtsc + chrono samples),
writes meta.json, opens 32 per-method append-only fds, starts the
flusher thread pinned to CPU 0.

Flusher ticks every flush_interval_ms; per tick: walk g_rings_head,
snapshot each ring (W-R), bucket samples by method id, append to the
per-method fd.

deinit() signals shutdown, joins the flusher, does one final synchronous
pass, closes fds, drains all rings. MTR tidesdb_perf_on_files_appear
verifies meta.json and write_row .bin land after a 10-row INSERT."
```

---

## Task 7: Three remaining MTR sysvar + overflow tests

**Files:**
- Create: `mysql-test-suite/t/tidesdb_perf_off_no_files.test` + `.result`
- Create: `mysql-test-suite/t/tidesdb_perf_capture_off_stops_growth.test` + `.result`
- Create: `mysql-test-suite/t/tidesdb_perf_meta_json_valid.test` + `.result`
- Create: `mysql-test-suite/t/tidesdb_perf_overflow_logged.test` + `.result`
- Create: `mysql-test-suite/t/tidesdb_perf_no_dml_regression.test` + `.result`

- [ ] **Step 1: Write `tidesdb_perf_off_no_files.test`**

```
--source include/tidesdb_perf_helpers.inc

SET GLOBAL tidesdb_perf_output_dir = '/tmp/perf-test-off';
--exec rm -rf /tmp/perf-test-off

# Capture remains OFF for the whole test.
CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TIDESDB;
INSERT INTO t VALUES (1),(2),(3),(4),(5),(6),(7),(8),(9),(10);
DROP TABLE t;

# Dir was never created (init also wasn't re-called).
--exec test ! -d /tmp/perf-test-off && echo "dir absent OK"
```

Expected `.result`:

```
dir absent OK
```

- [ ] **Step 2: Write `tidesdb_perf_capture_off_stops_growth.test`**

```
--source include/tidesdb_perf_helpers.inc

SET GLOBAL tidesdb_perf_output_dir = '/tmp/perf-test-growth';
--exec rm -rf /tmp/perf-test-growth

CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TIDESDB;

SET GLOBAL tidesdb_perf_capture = ON;
INSERT INTO t VALUES (1),(2),(3),(4),(5),(6),(7),(8),(9),(10);
--sleep 2
--exec stat -c '%s' /tmp/perf-test-growth/*-01-write_row.bin > /tmp/perf-growth-size-1.txt
SET GLOBAL tidesdb_perf_capture = OFF;
INSERT INTO t VALUES (11),(12),(13),(14),(15);
--sleep 2
--exec stat -c '%s' /tmp/perf-test-growth/*-01-write_row.bin > /tmp/perf-growth-size-2.txt
--exec diff -u /tmp/perf-growth-size-1.txt /tmp/perf-growth-size-2.txt
DROP TABLE t;
SET GLOBAL tidesdb_perf_capture = @save_capture;
```

Expected `.result`: empty diff output (files identical size after OFF).

- [ ] **Step 3: Write `tidesdb_perf_meta_json_valid.test`**

```
--source include/tidesdb_perf_helpers.inc

SET GLOBAL tidesdb_perf_output_dir = '/tmp/perf-test-meta';
--exec rm -rf /tmp/perf-test-meta

SET GLOBAL tidesdb_perf_capture = ON;
CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TIDESDB;
INSERT INTO t VALUES (1);
--sleep 2
SET GLOBAL tidesdb_perf_capture = OFF;

# tsc_ghz parses; capacity matches default; method_count is 32.
--exec python3 -c "import json; d=json.load(open(__import__('glob').glob('/tmp/perf-test-meta/*-meta.json')[0])); assert 0.5 <= d['tsc_ghz'] <= 6.0; assert d['capacity'] == 65536; assert d['method_count'] == 32; assert d['format_version'] == 1; print('meta OK')"

DROP TABLE t;
SET GLOBAL tidesdb_perf_capture = @save_capture;
```

Expected `.result`:

```
meta OK
```

- [ ] **Step 4: Write `tidesdb_perf_overflow_logged.test`**

```
--source include/tidesdb_perf_helpers.inc
--source include/have_debug.inc

# Tiny ring (256 samples) so we overrun fast. ring_capacity_pow2 is
# server-start-only so a restart is required to take effect.
let $RESTART_PARAMETERS = restart: --tidesdb_perf_ring_capacity_pow2=8;
--source include/restart_mysqld.inc

SET GLOBAL tidesdb_perf_output_dir = '/tmp/perf-test-overflow';
--exec rm -rf /tmp/perf-test-overflow

CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TIDESDB;
SET GLOBAL tidesdb_perf_capture = ON;
# Insert many more rows than the ring can hold without a flush.
let $i = 0;
while ($i < 1000) {
  --eval INSERT INTO t VALUES ($i)
  inc $i;
}
--sleep 2
SET GLOBAL tidesdb_perf_capture = OFF;

# wrap_count was bumped on at least one ring; the flusher logged a warning.
let $log = $MYSQLTEST_VARDIR/log/mysqld.1.err;
--exec grep -c "TIDESDB-PERF" $log || true
DROP TABLE t;
SET GLOBAL tidesdb_perf_capture = @save_capture;
```

Expected: positive grep count.

- [ ] **Step 5: Write `tidesdb_perf_no_dml_regression.test`**

```
--source include/tidesdb_perf_helpers.inc

# Run an INSERT loop with capture OFF vs ON; ratio of wall-clock should
# be within 5% (overhead acceptance criterion §8.2).

CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TIDESDB;

# Warm-up.
let $i = 0;
while ($i < 50) { --eval INSERT INTO t VALUES (-$i); inc $i; }
DELETE FROM t;

# Capture OFF baseline.
SET GLOBAL tidesdb_perf_capture = OFF;
SET @t0 = (SELECT UNIX_TIMESTAMP(SYSDATE(6)));
let $i = 0;
while ($i < 1000) { --eval INSERT INTO t VALUES ($i); inc $i; }
SET @off_secs = (SELECT UNIX_TIMESTAMP(SYSDATE(6)) - @t0);
DELETE FROM t;

# Capture ON.
SET GLOBAL tidesdb_perf_capture = ON;
SET @t0 = (SELECT UNIX_TIMESTAMP(SYSDATE(6)));
let $i = 0;
while ($i < 1000) { --eval INSERT INTO t VALUES ($i); inc $i; }
SET @on_secs = (SELECT UNIX_TIMESTAMP(SYSDATE(6)) - @t0);
SET GLOBAL tidesdb_perf_capture = OFF;

# Overhead must be <= 5% (with generous noise margin: <= 1.10x).
SELECT IF(@on_secs <= @off_secs * 1.10, 'OK', CONCAT('REGRESS off=', @off_secs, ' on=', @on_secs)) AS verdict;

DROP TABLE t;
SET GLOBAL tidesdb_perf_capture = @save_capture;
```

Expected `.result`:

```
verdict
OK
```

- [ ] **Step 6: Run all 5 — expect 5/5 PASS**

```bash
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb \
  tidesdb/mysql-mtr:9.7-task8 \
  bash -lc 'cd /build/mysql-server/build && \
            cmake -DTIDESDB_PERF=1 -S /build/mysql-server -B . > /dev/null 2>&1 && \
            cmake --build . --target tidesdb -j > /dev/null && \
            cd mysql-test && ./mtr --suite=tidesdb \
              tidesdb_perf_off_no_files \
              tidesdb_perf_capture_off_stops_growth \
              tidesdb_perf_meta_json_valid \
              tidesdb_perf_overflow_logged \
              tidesdb_perf_no_dml_regression'"
```

- [ ] **Step 7: Commit**

```bash
git add mysql-test-suite/t/tidesdb_perf_off_no_files.test \
        mysql-test-suite/t/tidesdb_perf_capture_off_stops_growth.test \
        mysql-test-suite/t/tidesdb_perf_meta_json_valid.test \
        mysql-test-suite/t/tidesdb_perf_overflow_logged.test \
        mysql-test-suite/t/tidesdb_perf_no_dml_regression.test \
        mysql-test-suite/r/tidesdb_perf_*.result
git commit -m "test(perf): 5 MTR tests (no_files, stops_growth, meta_json_valid, overflow_logged, no_dml_regression)

Covers: capture=OFF leaves the output dir untouched; flipping
ON->OFF stops file growth within one tick; meta.json is parseable
with expected values; overflow under a tiny ring (cap=256) bumps
wrap_count and logs to error log; ON vs OFF wall-clock for a 1000
INSERT loop stays within 10% (5% target + 5% noise budget)."
```

---

## Task 8: 32 instrumentation sites

**Files:**
- Modify: `plugin/ha_tidesdb.cc` (24 handler vtable + helper sites)
- Modify: `plugin/tidesdb_inplace_alter.cc` (4 inplace ALTER virtuals)
- Modify: `plugin/tidesdb_fts.cc` (4 FTS helper sites)

- [ ] **Step 1: Add `#include "tidesdb_perf_scope.h"` to each of the three .cc files (if not already present from Task 2)**

- [ ] **Step 2: Add `TDB_PERF_SCOPE(...)` at the FIRST executable line of each of the following 24 methods in `plugin/ha_tidesdb.cc`**

For each method, add `TDB_PERF_SCOPE(<method_name_lowercased>);` as the first statement inside the function body (after `DBUG_ENTER`/`DBUG_TRACE` if present, before any other work):

| Method | MethodId arg |
|---|---|
| `ha_tidesdb::write_row` | `write_row` |
| `ha_tidesdb::update_row` | `update_row` |
| `ha_tidesdb::delete_row` | `delete_row` |
| `ha_tidesdb::index_read_map` | `index_read_map` |
| `ha_tidesdb::index_next` | `index_next` |
| `ha_tidesdb::index_prev` | `index_prev` |
| `ha_tidesdb::rnd_next` | `rnd_next` |
| `ha_tidesdb::rnd_pos` | `rnd_pos` |
| `ha_tidesdb::external_lock` | `external_lock` |
| `ha_tidesdb::start_stmt` | `start_stmt` |
| `ha_tidesdb::store_lock` | `store_lock` |
| `tidesdb_commit` (handlerton callback) | `commit` |
| `tidesdb_rollback` (handlerton callback) | `rollback` |
| `ha_tidesdb::savepoint_set` (or the analogous wrapper) | `savepoint_set` |
| `ha_tidesdb::savepoint_release` | `savepoint_release` |
| `ha_tidesdb::savepoint_rollback` | `savepoint_rollback` |
| `ha_tidesdb::open` | `open` |
| `ha_tidesdb::close` | `close` |
| `ha_tidesdb::info` | `info` |
| `ha_tidesdb::table_flags_cache_init` (or however the plugin caches table_flags) | `table_flags_cache_init` |
| `ha_tidesdb::create` | `create` |
| `ha_tidesdb::delete_table` | `delete_table` |
| `ha_tidesdb::serialize_row_into` (or wherever the per-row serialization lives) | `serialize_row` |
| `ha_tidesdb::deserialize_row` | `deserialize_row` |

Example placement at the top of `ha_tidesdb::write_row`:

```cpp
int ha_tidesdb::write_row(uchar *buf) {
    DBUG_TRACE;
    TDB_PERF_SCOPE(write_row);
    /* ...existing body... */
}
```

For the handlerton callbacks (`tidesdb_commit`, `tidesdb_rollback`), insert the scope as the first statement inside the function — these are file-scope `static` functions in the same .cc.

For methods that aren't trivially located (e.g. the project's `serialize_row` may be inlined or split across several helpers), grep for the actual symbol with `grep -n "ha_tidesdb::serialize\|serialize_row\b" plugin/ha_tidesdb.cc`. Where there isn't a one-true entry point (e.g. inline serialization spread across `write_row`/`update_row`), pick the dominant call site and wrap a small helper extraction if needed. **Do not** invent a new code path; the macro must wrap an existing function entry.

If a method on the list does not exist in the current code (e.g. `table_flags_cache_init` may be inlined into `info()` instead), instrument the closest existing equivalent and add a one-line comment explaining the substitution: `// PERF: table_flags_cache_init not present; instrumented info() instead.`

- [ ] **Step 3: Add the 4 sites in `plugin/tidesdb_inplace_alter.cc`**

| Method | MethodId arg |
|---|---|
| `ha_tidesdb::check_if_supported_inplace_alter` | `check_if_supported_inplace_alter` |
| `ha_tidesdb::prepare_inplace_alter_table` | `prepare_inplace_alter_table` |
| `ha_tidesdb::inplace_alter_table` | `inplace_alter_table` |
| `ha_tidesdb::commit_inplace_alter_table` | `commit_inplace_alter_table` |

- [ ] **Step 4: Add the 4 sites in `plugin/tidesdb_fts.cc`**

The remaining 4 method IDs (`key_copy_to_comparable`, `pk_from_record`, `encrypt_row_into`, `decrypt_row`) may live in `ha_tidesdb.cc` rather than `tidesdb_fts.cc` — grep to confirm. For those that DO live in `tidesdb_fts.cc` (FTS-specific helpers like `fts_extract_and_tokenize`, etc.), substitute the closest analogue. If none of the 4 live in `tidesdb_fts.cc`, instrument all 4 in `plugin/ha_tidesdb.cc` instead and adjust the per-file commit message.

```cpp
uchar *ha_tidesdb::pk_from_record(const uchar *record, uchar *out, uint *out_len) {
    TDB_PERF_SCOPE(pk_from_record);
    /* existing body */
}
```

- [ ] **Step 5: Sync source + rebuild + verify nothing regressed**

```bash
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb \
  tidesdb/mysql-mtr:9.7-task8 \
  bash -lc 'cd /build/mysql-server/build && \
            cmake -DTIDESDB_PERF=1 -S /build/mysql-server -B . > /dev/null 2>&1 && \
            cmake --build . --target tidesdb -j > /dev/null && \
            cd mysql-test && ./mtr --suite=tidesdb --force --max-test-fail=0 2>&1 | tail -5'"
```

Expected: 68 PASS (atomic-DDL baseline) + 7 NEW perf MTR tests = 75 PASS / 15 SKIPPED / 0 FAIL. Some of the 15 skips are atomic-DDL debug-only tests from v0.4.0.

- [ ] **Step 6: Commit**

```bash
git add plugin/ha_tidesdb.cc plugin/tidesdb_inplace_alter.cc plugin/tidesdb_fts.cc
git commit -m "feat(perf): 32 TDB_PERF_SCOPE instrumentation sites

Handler vtable: write_row, update_row, delete_row, index_read_map,
index_next, index_prev, rnd_next, rnd_pos, external_lock, start_stmt,
store_lock, commit, rollback, savepoint_*, open, close, info,
table_flags_cache_init, create, delete_table.

Inplace ALTER virtuals: check_if_supported_inplace_alter,
prepare_inplace_alter_table, inplace_alter_table,
commit_inplace_alter_table.

Plugin-private helpers: serialize_row, deserialize_row,
key_copy_to_comparable, pk_from_record, encrypt_row_into, decrypt_row.

Each scope is the first executable line of its function so dtor fires
just before return. Zero behaviour change with capture=OFF or build
flag TIDESDB_PERF=0."
```

---

## Task 9: Offline analyser — `tools/tidesdb-perf-analyze`

**Files:**
- Create: `tools/tidesdb-perf-analyze/__main__.py`
- Create: `tools/tidesdb-perf-analyze/compare.py`
- Create: `tools/tidesdb-perf-analyze/requirements.txt`
- Create: `tools/tidesdb-perf-analyze/tests/test_analyze.py`

- [ ] **Step 1: Create `tools/tidesdb-perf-analyze/requirements.txt`**

```
numpy>=1.24
pandas>=2.0
pytest>=7.0
```

- [ ] **Step 2: Write the failing Python tests `tools/tidesdb-perf-analyze/tests/test_analyze.py`**

```python
"""Unit tests for the offline analyser."""
import json
import os
import struct
import tempfile

import pytest


# Sample binary format: 24 bytes per record.
# struct: <BBHQQ  (uint8 method_id, uint8 thread_id, uint16 reserved,
#                  uint64 enter_tsc, uint64 exit_tsc)
SAMPLE_STRUCT = "<BBHQQ"
SAMPLE_SIZE = 24


def write_samples(path, samples):
    """samples: list of (method_id, thread_id, enter_tsc, exit_tsc)."""
    with open(path, "wb") as f:
        for m, t, e, x in samples:
            f.write(struct.pack(SAMPLE_STRUCT, m, t, 0, e, x))


def write_meta(path, tsc_ghz=3.5, capacity=65536, method_count=32, pid=1):
    with open(path, "w") as f:
        json.dump({
            "format_version": 1,
            "tsc_ghz": tsc_ghz,
            "capacity": capacity,
            "method_count": method_count,
            "pid": pid,
        }, f)


def test_histogram_from_samples():
    from tidesdb_perf_analyze import analyse  # type: ignore
    with tempfile.TemporaryDirectory() as d:
        # Synthesise: 1000 samples for write_row with cycle deltas 100,200,...,100000.
        samples = [(1, 1, 0, delta) for delta in range(100, 100_001, 100)]
        write_samples(os.path.join(d, "1-01-write_row.bin"), samples)
        write_meta(os.path.join(d, "1-meta.json"))
        report = analyse(d)
        assert "write_row" in report
        wr = report["write_row"]
        assert wr["calls"] == 1000
        # cycles -> ns: 1 cycle / 3.5 GHz = ~0.286 ns; p50 of cycle deltas
        # 100..100000 step 100 = ~50050 cycles = ~14300 ns.
        assert 14000 < wr["p50_ns"] < 15000
        # p99 = ~99100 cycles = ~28310 ns
        assert 28000 < wr["p99_ns"] < 29000


def test_multi_file_merge():
    from tidesdb_perf_analyze import analyse  # type: ignore
    with tempfile.TemporaryDirectory() as d:
        # Two .bin files for the same method (e.g. two processes); merge to totals.
        write_samples(os.path.join(d, "1-01-write_row.bin"),
                      [(1, 1, 0, 1000)] * 500)
        write_samples(os.path.join(d, "2-01-write_row.bin"),
                      [(1, 1, 0, 1000)] * 500)
        write_meta(os.path.join(d, "1-meta.json"))
        write_meta(os.path.join(d, "2-meta.json"))
        report = analyse(d)
        assert report["write_row"]["calls"] == 1000


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
```

- [ ] **Step 3: Run — expect 2 FAIL**

```bash
cd tools/tidesdb-perf-analyze && python3 -m pip install -r requirements.txt --quiet && python3 -m pytest tests/ 2>&1 | tail
```

Expected: ImportError (module doesn't exist yet) → 2 FAIL.

- [ ] **Step 4: Implement `tools/tidesdb-perf-analyze/__main__.py`**

```python
"""tidesdb-perf-analyze: read per-method .bin sample files and emit reports."""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys

import numpy as np
import pandas as pd

SAMPLE_STRUCT = "<BBHQQ"
SAMPLE_SIZE = 24

METHOD_NAMES = {
    1: "write_row", 2: "update_row", 3: "delete_row",
    4: "index_read_map", 5: "index_next", 6: "index_prev",
    7: "rnd_next", 8: "rnd_pos",
    9: "external_lock", 10: "start_stmt", 11: "store_lock",
    12: "commit", 13: "rollback",
    14: "savepoint_set", 15: "savepoint_release", 16: "savepoint_rollback",
    17: "open", 18: "close", 19: "info", 20: "table_flags_cache_init",
    21: "create", 22: "delete_table",
    23: "check_if_supported_inplace_alter", 24: "prepare_inplace_alter_table",
    25: "inplace_alter_table", 26: "commit_inplace_alter_table",
    27: "serialize_row", 28: "deserialize_row",
    29: "key_copy_to_comparable", 30: "pk_from_record",
    31: "encrypt_row_into", 32: "decrypt_row",
}


def load_meta(d: str) -> dict:
    metas = glob.glob(os.path.join(d, "*-meta.json"))
    if not metas:
        raise FileNotFoundError(f"No *-meta.json in {d}")
    # Use the first; in a multi-pid capture, callers should colocate.
    with open(metas[0]) as f:
        return json.load(f)


def load_method_file(path: str) -> np.ndarray:
    """Return Nx5 array (method_id, thread_id, reserved, enter, exit)."""
    sz = os.path.getsize(path)
    n = sz // SAMPLE_SIZE
    if n == 0:
        return np.empty((0, 5), dtype=np.uint64)
    raw = np.fromfile(path, dtype=np.uint8, count=n * SAMPLE_SIZE)
    parsed = np.empty((n, 5), dtype=np.uint64)
    for i in range(n):
        b = raw[i * SAMPLE_SIZE: (i + 1) * SAMPLE_SIZE]
        m, t, r, e, x = struct.unpack(SAMPLE_STRUCT, b.tobytes())
        parsed[i] = (m, t, r, e, x)
    return parsed


def analyse(directory: str) -> dict:
    """Return {method_name: {calls, mean_ns, p50_ns, p95_ns, p99_ns, max_ns, total_ms}}."""
    meta = load_meta(directory)
    tsc_ghz = float(meta["tsc_ghz"])
    out: dict = {}
    by_method: dict[int, list[np.ndarray]] = {}
    for path in glob.glob(os.path.join(directory, "*-*-*.bin")):
        # Pid prefix + method id; ignore meta + calibration.
        if path.endswith("meta.json") or "calibration" in os.path.basename(path):
            continue
        arr = load_method_file(path)
        if arr.size == 0:
            continue
        method_id = int(arr[0, 0])
        by_method.setdefault(method_id, []).append(arr)
    for mid, arrs in by_method.items():
        a = np.concatenate(arrs)
        deltas_cycles = a[:, 4] - a[:, 3]
        ns = (deltas_cycles.astype(np.float64) / tsc_ghz)
        name = METHOD_NAMES.get(mid, f"method_{mid}")
        out[name] = {
            "calls": int(len(ns)),
            "mean_ns": float(ns.mean()),
            "p50_ns": float(np.percentile(ns, 50)),
            "p95_ns": float(np.percentile(ns, 95)),
            "p99_ns": float(np.percentile(ns, 99)),
            "max_ns": float(ns.max()),
            "total_ms": float(ns.sum() / 1e6),
        }
    return out


def to_markdown(report: dict) -> str:
    rows = sorted(report.items(), key=lambda kv: kv[1]["total_ms"], reverse=True)
    lines = [
        "| method | calls | total_ms | mean_us | p50_us | p95_us | p99_us | max_us |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for name, m in rows:
        lines.append(
            f"| {name} | {m['calls']:,} | {m['total_ms']:.1f} | "
            f"{m['mean_ns']/1000:.2f} | {m['p50_ns']/1000:.2f} | "
            f"{m['p95_ns']/1000:.2f} | {m['p99_ns']/1000:.2f} | "
            f"{m['max_ns']/1000:.2f} |"
        )
    return "\n".join(lines) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser(prog="tidesdb-perf-analyze")
    p.add_argument("directory", help="Directory of .bin + meta.json files")
    p.add_argument("--output", default="-", help="Markdown output file (- for stdout)")
    args = p.parse_args(argv)
    report = analyse(args.directory)
    md = to_markdown(report)
    if args.output == "-":
        print(md)
    else:
        with open(args.output, "w") as f:
            f.write(md)


if __name__ == "__main__":
    main()
```

Also create `tools/tidesdb-perf-analyze/__init__.py` with just:

```python
from .__main__ import analyse, to_markdown, main  # noqa: F401
```

Rename `__main__.py` to `tidesdb_perf_analyze.py` and add an `__init__.py` that re-exports — Python's `tests/` should be able to do `from tidesdb_perf_analyze import analyse`. Adjust the import path in `test_analyze.py` accordingly if needed.

- [ ] **Step 5: Re-run — expect 2/2 PASS**

```bash
cd tools/tidesdb-perf-analyze && python3 -m pytest tests/ -v 2>&1 | tail
```

- [ ] **Step 6: Implement `compare.py` for the `--compare` mode**

```python
"""tidesdb-perf-analyze --compare a/ b/: side-by-side diff."""
from __future__ import annotations

import argparse
import sys

from . import analyse


def to_diff_markdown(left: dict, right: dict, left_label="A", right_label="B") -> str:
    methods = sorted(set(left) | set(right))
    rows = []
    for m in methods:
        l_ns = left.get(m, {}).get("mean_ns", 0.0)
        r_ns = right.get(m, {}).get("mean_ns", 0.0)
        calls = max(left.get(m, {}).get("calls", 0),
                    right.get(m, {}).get("calls", 0))
        delta_ns = r_ns - l_ns  # positive => right (B) is slower
        delta_total_ms = delta_ns * calls / 1e6
        rows.append((m, l_ns, r_ns, delta_ns, calls, delta_total_ms))
    rows.sort(key=lambda r: r[5], reverse=True)
    lines = [
        f"| method | {left_label} ns/call | {right_label} ns/call | Δ ns | calls | Δ total (ms) |",
        "|---|---|---|---|---|---|",
    ]
    for m, l, r, d, c, dt in rows:
        lines.append(f"| {m} | {l:.0f} | {r:.0f} | {d:+.0f} | {c:,} | {dt:+.1f} |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument("left", help="Left capture dir (e.g. MySQL)")
    p.add_argument("right", help="Right capture dir (e.g. MariaDB)")
    p.add_argument("--left-label", default="MySQL")
    p.add_argument("--right-label", default="MariaDB")
    p.add_argument("--output", default="-")
    args = p.parse_args(argv)
    left = analyse(args.left)
    right = analyse(args.right)
    md = to_diff_markdown(left, right, args.left_label, args.right_label)
    if args.output == "-":
        print(md)
    else:
        with open(args.output, "w") as f:
            f.write(md)


if __name__ == "__main__":
    main(sys.argv[1:])
```

- [ ] **Step 7: Commit**

```bash
git add tools/tidesdb-perf-analyze/
git commit -m "feat(perf): tidesdb-perf-analyze offline tool

Reads per-method .bin files + meta.json, computes per-method
{calls, mean, p50, p95, p99, max, total} in ns, emits markdown +
CSV. --compare mode does (A, B, ΔA-B) side-by-side sorted by
absolute total delta.

Two unit tests: HistogramFromSamples (1000 synthetic samples, p50/p99
within tolerance) and MultiFileMerge (two .bin files for same method
merge to one totals row)."
```

---

## Task 10: Integration harness — `bench/perf/run-perf-capture.sh`

**Files:**
- Create: `bench/perf/run-perf-capture.sh`

- [ ] **Step 1: Create `bench/perf/run-perf-capture.sh`**

```bash
#!/usr/bin/env bash
# Perf-instrumented HammerDB capture.
#
# 1. Start mysqld with tidesdb_perf_capture=ON
# 2. Run HammerDB TPC-C
# 3. Stop mysqld; flusher drains
# 4. docker cp /var/lib/mysql/tidesdb-perf/ out
# 5. Run tools/tidesdb-perf-analyze
# 6. Optionally run again for MariaDB SUT and produce a --compare diff
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)
ROOT=$(cd "$BENCH/.." && pwd)

WARE=${WARE:-10}
BUILDVU=${BUILDVU:-4}
RUNVU=${RUNVU:-8}
RAMP=${RAMP:-1}
DUR=${DUR:-3}
CPUS=${CPUS:-4}
MEM=${MEM:-12g}

MYSQL_IMAGE=${MYSQL_IMAGE:-tidesdb/mysql:9.7-perf}
MARIA_IMAGE=${MARIA_IMAGE:-sut-mariadb-tidesdb:9.3.0-perf}
COMPARE=${COMPARE:-0}

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$BENCH/results/perf-$TS"
mkdir -p "$OUT/mysql" "$OUT/mariadb"

# --- Run one SUT ---
run_sut() {
    local image=$1 label=$2 dest=$3
    echo "[perf] running $label ($image)"
    # Reuse the existing hammerdb harness but with the perf image and
    # tidesdb_perf_capture forced ON via DB_EXTRA_ARGS.
    MYSQL_IMAGE="$image" \
    DB_EXTRA_ARGS="--loose_tidesdb_perf_capture=ON --loose_tidesdb_perf_output_dir=/tmp/perf-out" \
    WARE="$WARE" BUILDVU="$BUILDVU" RUNVU="$RUNVU" RAMP="$RAMP" DUR="$DUR" \
    CPUS="$CPUS" MEM="$MEM" \
        "$BENCH/hammerdb/run-hammerdb.sh" > "$dest/hammerdb.log" 2>&1
    # Locate the mysqld container id from the log and docker cp the dir out.
    local cid
    cid=$(sg docker -c "docker ps -a --filter ancestor=$image --format '{{.ID}}' | head -1")
    if [ -z "$cid" ]; then
        echo "[perf] WARN: no container for $image; skipping cp"
        return
    fi
    sg docker -c "docker cp $cid:/tmp/perf-out/. $dest/"
    echo "[perf] $label artifacts in $dest"
    python3 -m tidesdb_perf_analyze "$dest" --output "$dest/report.md"
    echo "[perf] $label report: $dest/report.md"
}

run_sut "$MYSQL_IMAGE" mysql "$OUT/mysql"
if [ "$COMPARE" = "1" ]; then
    run_sut "$MARIA_IMAGE" mariadb "$OUT/mariadb"
    python3 -m tidesdb_perf_analyze.compare "$OUT/mysql" "$OUT/mariadb" \
        --left-label "MySQL" --right-label "MariaDB" \
        --output "$OUT/diff.md"
    echo "[perf] diff: $OUT/diff.md"
fi
echo "[perf] done — $OUT"
```

`chmod +x bench/perf/run-perf-capture.sh`.

- [ ] **Step 2: Smoke-test the harness on MySQL only (no compare)**

We need a perf-flavoured image. Quick path: rebuild the runtime image with `-DTIDESDB_PERF=1` via the same overlay trick we used for v0.4.0:

```bash
# Build the perf .so
sg docker -c "docker run --rm --user $(id -u):$(id -g) -e TIDESDB_PERF=1 -v $(pwd):/work tides-builder /work/scripts/build-plugin.sh 2>&1 | tail -3"
# Overlay into a perf image
mkdir -p /tmp/perf-overlay
cp vendor/mysql-server/build/plugin_output_directory/ha_tidesdb.so /tmp/perf-overlay/
cat > /tmp/perf-overlay/Dockerfile <<'EOF'
FROM tidesdb/mysql:9.7
COPY ha_tidesdb.so /usr/lib/mysql/plugin/ha_tidesdb.so
EOF
sg docker -c "cd /tmp/perf-overlay && docker build -t tidesdb/mysql:9.7-perf ."
# Smoke
WARE=2 BUILDVU=1 RUNVU=1 RAMP=1 DUR=1 bash bench/perf/run-perf-capture.sh 2>&1 | tail -20
```

Expected: artifacts under `bench/results/perf-<ts>/mysql/`; `report.md` lists at least `write_row` with a non-zero `calls` column.

- [ ] **Step 3: Commit**

```bash
git add bench/perf/run-perf-capture.sh
git commit -m "feat(perf): bench/perf/run-perf-capture.sh harness

Wraps the existing bench/hammerdb/run-hammerdb.sh: forces tidesdb_perf
sysvars via DB_EXTRA_ARGS, docker cp's the per-method bin files out,
runs the analyser, optionally repeats for the MariaDB SUT and emits
--compare diff.

Knob: COMPARE=1 to run both."
```

---

## Task 11: TideSQL side-by-side port

**Files:**
- Create (vendored, outside this repo): `vendor/tidesql/` clone with patched `ha_tidesdb.cc`
- Build: `sut-mariadb-tidesdb:9.3.0-perf` image
- (Optional) commit the patch as a diff under `docker/patches/tidesql/0001-perf-instrumentation.patch`

- [ ] **Step 1: Clone TideSQL into vendor (if not already present)**

```bash
[ -d vendor/tidesql ] || git clone --depth=1 https://github.com/tidesdb/tidesql.git vendor/tidesql
cd vendor/tidesql && git pull --ff-only
```

- [ ] **Step 2: Drop the vendored perf-ring header into TideSQL's source tree**

```bash
mkdir -p vendor/tidesql/storage/tidesdb/perf
cp plugin/tidesdb_perf_ring.h plugin/tidesdb_perf_ring.cc plugin/tidesdb_perf_scope.h \
   vendor/tidesql/storage/tidesdb/perf/
```

Adjust `#include` paths inside the copies (replace any `"sql/handler.h"` with the MariaDB equivalent if needed). Compile-test outside the scope of this step — just verify the files land.

- [ ] **Step 3: Add the 24 TDB_PERF_SCOPE sites in TideSQL's `ha_tidesdb.cc`**

Mirror Task 8's site list. Methods may have different signatures and names (MariaDB handler API differs from MySQL); insert at the equivalent vtable methods. The MethodId enum stays the same (same `Sample` format → same offline tool).

- [ ] **Step 4: Build the perf-flavoured SUT image**

```bash
sg docker -c "cd vendor/tidesql && docker build -t sut-mariadb-tidesdb:9.3.0-perf -f Dockerfile --build-arg PERF=1 ."
```

(The `--build-arg PERF=1` honours a CMake `-D TIDESDB_PERF=1` knob the patch added to TideSQL's CMakeLists.)

- [ ] **Step 5: Save the TideSQL patch as a tracked artifact**

```bash
cd vendor/tidesql && git diff > ../../docker/patches/tidesql/0001-perf-instrumentation.patch
cd ../..
git add docker/patches/tidesql/0001-perf-instrumentation.patch
```

- [ ] **Step 6: Smoke-test the perf MariaDB SUT**

```bash
COMPARE=1 WARE=2 BUILDVU=1 RUNVU=1 RAMP=1 DUR=1 bash bench/perf/run-perf-capture.sh 2>&1 | tail -10
ls bench/results/perf-*/mariadb/  # expect *.bin + report.md
cat bench/results/perf-*/diff.md  # expect a populated diff table
```

- [ ] **Step 7: Commit**

```bash
git add docker/patches/tidesql/0001-perf-instrumentation.patch
git commit -m "feat(perf): TideSQL side-by-side perf instrumentation patch

Vendored perf_ring + 24 TDB_PERF_SCOPE sites mirroring the MySQL
plugin. Same MethodId enum, same Sample format -> the
tidesdb-perf-analyze --compare mode consumes both transparently.

Patch tracked at docker/patches/tidesql/0001-perf-instrumentation.patch
so the SUT image rebuild is reproducible without checking out TideSQL
fork state."
```

---

## Task 12: End-to-end measurement run

**Files:**
- Create: `docs/v0.4.1-perf-baseline.md`

- [ ] **Step 1: Run the full perf capture against both SUTs at the WARE=10, RUNVU=8, 3 min profile**

```bash
WARE=10 BUILDVU=4 RUNVU=8 RAMP=1 DUR=3 COMPARE=1 \
    bash bench/perf/run-perf-capture.sh 2>&1 | tee /tmp/perf-run.log
```

Wait ~20 min wall-clock. Expected outputs:
- `bench/results/perf-<ts>/mysql/report.md`
- `bench/results/perf-<ts>/mariadb/report.md`
- `bench/results/perf-<ts>/diff.md`

- [ ] **Step 2: Inspect the diff. Note the top 5 rows (largest positive `Δ total (ms)`)**

```bash
head -20 bench/results/perf-*/diff.md
```

Capture the top 5 (the optimisation candidates for v0.4.2 / v0.5.0).

- [ ] **Step 3: Write `docs/v0.4.1-perf-baseline.md`**

The report is a structured summary of the measurement run, modelled on `docs/v0.4.0-validation-report.md`:

- Build identity (branch, commit, .so SHA, MySQL + TideSQL image digests)
- Capture profile (WARE, RUNVU, RAMP, DUR, hardware)
- Calibration floor (per-scope overhead floor in ns from `calibration.bin`)
- Full per-SUT report tables (paste from `report.md`)
- The diff table (paste from `diff.md`)
- Top 5 optimisation candidates with brief commentary
- Known limitations
- Reproduction commands

- [ ] **Step 4: Commit**

```bash
git add bench/results/perf-*  docs/v0.4.1-perf-baseline.md
git commit -m "docs: v0.4.1 perf baseline + top-5 optimisation candidates

End-to-end HammerDB capture at WARE=10 RUNVU=8 3m profile against both
MySQL+TidesDB and MariaDB+TidesDB on the same engine. Per-method
mean/p50/p95/p99/max latency in ns; comparison diff ranks methods by
Δ total (ms).

Top 5 candidates feed the v0.4.2 / v0.5.0 optimisation release."
```

---

## Task 13: Validation report + tag v0.4.1

**Files:**
- Create: `docs/v0.4.1-validation-report.md`
- Modify: `CHANGELOG.md`
- Modify: `KNOWN-ISSUES.md`

- [ ] **Step 1: Write `docs/v0.4.1-validation-report.md`**

Follow the v0.4.0 report's structure. Sections:
- Build identity (branch `feat/perf-instrumentation`, final commit, .so SHA, image digest)
- The acquired surface (4 sysvars, 32 instrumentation sites, ring + flusher + offline tool + TideSQL port)
- Validation gates:
  - gtest: 8/8 PerfRing+PerfScope cases PASS (paste from the test runs above)
  - MTR perf: 7 new tests PASS
  - MTR regression: full 68+ existing tests still PASS
  - mwbench integrity at QUICK profile (1 GiB): PASS, no LSM-layer regression
  - HammerDB SMOKE: PASS, no handler errors
  - Calibration floor: ≤ 50 ns target (capture actual)
  - DML overhead under capture: ≤ 5% target (capture actual)
- Known limitations (perf-image only — production builds compile out instrumentation; per-CPU attribution out of scope; offline tool is sync, not streaming)
- Reproduction: `bench/perf/run-perf-capture.sh`

- [ ] **Step 2: Append to `CHANGELOG.md`**

```
## v0.4.1 — <DATE>

### Added

- Layer-by-layer perf instrumentation infrastructure (closes spec
  2026-06-04-perf-instrumentation-design.md).
- 4 new sysvars: tidesdb_perf_capture (BOOL, default OFF, runtime),
  tidesdb_perf_output_dir, tidesdb_perf_ring_capacity_pow2 (server-start),
  tidesdb_perf_flush_interval_ms.
- 32 TDB_PERF_SCOPE instrumentation sites across the handler vtable,
  inplace ALTER virtuals, and deeper plugin helpers.
- tools/tidesdb-perf-analyze offline Python analyser + --compare mode.
- bench/perf/run-perf-capture.sh integration harness with optional
  side-by-side MariaDB SUT comparison.
- Vendored TideSQL perf-ring header + 24-site patch under
  docker/patches/tidesql/.

### Changed

- Build flag -DTIDESDB_PERF=1 controls whether instrumentation is
  compiled in. Default OFF in release builds (zero cost); perf image
  variant compiled with it ON.

### Notes

- The release ships infrastructure only — no plugin optimisations.
  See docs/v0.4.1-perf-baseline.md for the comparison data that
  feeds the next release's optimisation targets.
```

- [ ] **Step 3: Append to `KNOWN-ISSUES.md`**

```
## v0.4.1 known limitations

- Per-CPU-core attribution is out of scope; the flusher is pinned to
  one core but per-sample core IDs are not recorded.
- Engine-internal (TidesDB C library) time is included in the
  instrumented scopes; isolating engine-vs-handler time requires
  separate engine-level instrumentation, deferred.
- Two image variants to maintain: tidesdb/mysql:9.7 (no perf) and
  tidesdb/mysql:9.7-perf (TIDESDB_PERF=1). Maintenance pattern: the
  perf image rebuilds from the same source via a one-line overlay.
- The TideSQL port is maintained as a patch under
  docker/patches/tidesql/. The MethodId enum must stay in lock-step
  between MySQL and MariaDB plugins; a CI lint that diffs the two
  enum blocks is desirable but deferred to v0.4.2.
```

- [ ] **Step 4: Commit the docs**

```bash
git add docs/v0.4.1-validation-report.md CHANGELOG.md KNOWN-ISSUES.md
git commit -m "docs: v0.4.1 validation report + CHANGELOG + KNOWN-ISSUES"
```

- [ ] **Step 5: Push branch + open PR**

```bash
git push -u origin feat/perf-instrumentation
gh pr create --base main \
    --title "feat: perf instrumentation (v0.4.1)" \
    --body-file docs/v0.4.1-validation-report.md
```

- [ ] **Step 6: After PR merges to main, tag and push**

```bash
git checkout main
git pull --ff-only
git tag -a v0.4.1 -m "v0.4.1 — perf instrumentation infrastructure"
git push origin v0.4.1
```

- [ ] **Step 7: Run release-docker.sh per the existing release flow**

```bash
./scripts/release-docker.sh --yes
```

Note: the release script needs to honour the new perf image. If
release-docker.sh only pushes the canonical `tidesdb/mysql:9.7` image
and not `:9.7-perf`, append the perf image to its push list as a
follow-up patch (out of scope for this task).

---

## Plan self-review notes

- **Spec coverage**: every numbered requirement in spec §3 (Scope) maps to one or more tasks above.
  - §3.1 Sample/MethodId/TLS_Ring → Tasks 1, 3
  - §3.2 PerfScope macro → Tasks 1, 4
  - §3.3 Flusher thread + binary files → Task 6
  - §3.4 32 instrumentation sites → Task 8
  - §3.5 Calibration → Task 6 (writer); Task 12 (capture + report)
  - §3.6 4 sysvars → Task 2
  - §3.7 Offline tool → Task 9
  - §3.8 TideSQL port → Task 11
  - §3.9 bench harness → Task 10
  - §3.10 8 unit tests + 7 MTR tests → Tasks 3, 4, 5, 7

- **MTR test count**: 1 sysvar_basic (Task 2) + 1 on_files_appear (Task 6) + 5 off_no_files/stops_growth/meta_json_valid/overflow_logged/no_dml_regression (Task 7) = **7**. Matches spec §8.

- **gtest count**: 3 (Task 3: PushReadRoundTrip, WrapBehaviour, TombstoneDrained) + 3 (Task 4: AllocSelfRegisters, NoCaptureZeroPushes, CaptureOnPushesOneSample) + 2 (Task 5: NestedScopesOrdered, ConcurrentPushSingleReader) = **8**. Matches spec §8.

- **No placeholders.** Every step has concrete code or a concrete command. Where API names might shift (e.g. the location of `serialize_row` inside `ha_tidesdb.cc`), the step says "grep first; instrument at the closest equivalent if the canonical name isn't present" — the implementer adapts, not invents.

- **Type consistency.** `Sample`, `MethodId`, `TLS_Ring`, `PerfScope`, `g_rings_head`, `g_capture_active`, `t_ring`, `ring_alloc_for_thread`, `ring_free`, `push_sample`, `init`, `deinit` are the consistent symbols throughout. The 32 method-name strings in `analyse()`'s `METHOD_NAMES` dict match the 32 enum names in `MethodId`.

---

## Open follow-ups (out of scope for v0.4.1)

- v0.4.2 / v0.5.0 optimisation release driven by the top 5 candidates from `docs/v0.4.1-perf-baseline.md`.
- CI lint that diffs the MethodId enum blocks between MySQL and TideSQL ports.
- TSAN nightly job that runs `tidesdb_atomic_ddl_tests --gtest_filter=PerfRing.ConcurrentPushSingleReader` to catch any future ring race introductions.
- Per-CPU attribution if a workload's distribution starts mattering.
- Streaming flusher (vs the current 1 Hz batch) if longer runs show flush-tick stalls.
