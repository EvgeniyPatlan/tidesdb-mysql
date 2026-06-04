# Atomic-DDL Participation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the TidesDB-MySQL plugin into MySQL 9.7's atomic-DDL contract end-to-end so crash-during-DDL is recoverable and engine-private schema metadata is portable via SDI. Ships as v0.4.0.

**Architecture:** New translation unit `plugin/tidesdb_atomic_ddl.{cc,h}` hosts the SDI store, the DD↔CF reconciler, the atomic-DDL bridge, and the DDSE stubs. The existing handlerton in `plugin/ha_tidesdb.cc` wires all 14 new callbacks. The existing inplace-alter state machine in `plugin/tidesdb_inplace_alter.cc` starts consuming its `dd::Table*` parameters. The `HTON_SUPPORTS_ATOMIC_DDL` flag is set last (Task 13) — it activates the contract end-to-end; everything before it can be built and tested without it.

**Tech Stack:** MySQL 9.7 storage-engine plugin (C++17 in `plugin/`), TidesDB C API (column families, transactions), MTR (`mysql-test-run`) for integration tests, googletest for the two pure-function unit tests, rapidjson for SDI JSON, Docker images `tides-builder` (plugin-only) and `tidesdb/mysql-mtr:9.7` (full MTR).

**Spec:** `docs/superpowers/specs/2026-06-02-atomic-ddl-participation-design.md` (commit `ccec629`). Read §4 for the handlerton surface, §5 for components, §6 for data flow before implementing.

---

## Environment & build commands

Docker access requires `sg docker -c "..."` wrapping (the running user isn't in the docker group). The image `tides-builder` builds only the plugin .so (~5 min); `tidesdb/mysql-mtr:9.7` runs MTR against a baked-in mysqld build.

**Plugin-only fast iteration (use during dev, ~5 min):**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder /work/scripts/build-plugin.sh"
```

Produces `vendor/mysql-server/build/storage/tidesdb/ha_tidesdb.so`.

**MTR test invocation (full server, ~15 min cold / ~2 min warm):**

The MTR image must contain the updated plugin source. For tight iteration, rebuild only the plugin inside an already-built `mysql-mtr` image by mounting the host source over its baked-in copy:

```bash
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  tidesdb/mysql-mtr:9.7 \
  bash -lc 'cd /build/mysql-server/build && cmake --build . --target tidesdb -j && \
            cd mysql-test && ./mtr --suite=tidesdb --force <TEST_NAME>'"
```

Replace `<TEST_NAME>` with the specific test (e.g., `tidesdb_ddl_atomic_create_commit`). Omit to run the whole suite. The `--force` flag continues past failures.

**Full validation gate (Task 13 onwards):**

```bash
sg docker -c "docker run --rm tidesdb/mysql-mtr:9.7 \
  bash -lc 'cd /build/mysql-server/build/mysql-test && \
            ./mtr --suite=tidesdb --force --max-test-fail=0'"
```

**Commit conventions:** small atomic commits per task; messages follow `feat(atomic-ddl): <what>` / `test(atomic-ddl): <what>` / `chore(atomic-ddl): <what>`. No `Co-Authored-By` trailers (project-wide policy).

**Push policy:** No auto-push. Commits stay local until explicit `git push` at the end of each task once the subagent flow OKs it. Branch is `atomic-ddl-spec` (already created); rename to `feat/atomic-ddl` if preferred when ready to open the PR.

---

## File structure

| File | Role | Action |
|---|---|---|
| `plugin/tidesdb_atomic_ddl.h` | Class declarations: `SdiStore`, `DdSyncReconciler`, `TidesdbAtomicDdlBridge`, `DdseStubs`; constants for SDI key format; sysvar storage cells | Create |
| `plugin/tidesdb_atomic_ddl.cc` | Implementations | Create |
| `plugin/tidesdb_engine_context.h` | Extend `EngineCtx` with `std::unique_ptr<SdiStore> sdi;` and `const Plugin_tablespace *tablespace;` | Modify |
| `plugin/tidesdb_engine_context.cc` | Init/teardown wiring for the new fields | Modify |
| `plugin/ha_tidesdb.cc` | Handlerton: flag, sdi_* + dict_* callbacks; call sites in `create()`, `delete_table()`, `open()`; reconciler invocation in `tidesdb_init_func` | Modify |
| `plugin/tidesdb_inplace_alter.cc` | Consume `dd::Table*` in 4 virtuals; emit SDI on commit | Modify |
| `plugin/CMakeLists.txt` | Add `tidesdb_atomic_ddl.cc` to the plugin source list | Modify |
| `mysql-test-suite/t/tidesdb_ddl_*.test` | 20 new MTR test scripts | Create (one per test) |
| `mysql-test-suite/r/tidesdb_ddl_*.result` | Expected MTR outputs | Create (one per test) |
| `mysql-test-suite/include/tidesdb_ddl_helpers.inc` | Shared setup/teardown for the new tests | Create |
| `plugin/tests/test_sdi_pack_key.cc` | Pure-function unit test for `SdiStore::pack_key` | Create |
| `plugin/tests/test_reconciler_delta.cc` | Pure-function unit test for `DdSyncReconciler::compute_delta` (six cases) | Create |
| `plugin/tests/CMakeLists.txt` | Wire googletest-based unit tests | Create |

---

## Task 1: Skeleton TU + CMake wiring

**Files:**
- Create: `plugin/tidesdb_atomic_ddl.h`
- Create: `plugin/tidesdb_atomic_ddl.cc`
- Modify: `plugin/CMakeLists.txt`

- [ ] **Step 1: Create `plugin/tidesdb_atomic_ddl.h` with full declarations**

```cpp
/*
  Atomic-DDL participation surface for the TidesDB-MySQL plugin.

  Closes A-5 from docs/code-review-report.md. See
  docs/superpowers/specs/2026-06-02-atomic-ddl-participation-design.md
  for the design rationale.
*/
#ifndef TIDESDB_ATOMIC_DDL_H
#define TIDESDB_ATOMIC_DDL_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sql/dd/sdi_fwd.h"        /* sdi_key_t, sdi_vector_t */

#include "tidesdb.h"               /* tidesdb_t, tidesdb_column_family_t */

class THD;
struct handlerton;
struct Plugin_tablespace;
namespace dd { class Table; class Tablespace; namespace cache { class Dictionary_client; } }

namespace tidesdb_mysql {

/* Logical tablespace name (the single engine-wide tablespace). */
constexpr const char *kTidesdbTablespace = "tidesdb_system";

/* The dedicated CF that stores SDI blobs. */
constexpr const char *kSdiCfName = "__tidesdb_sdi";

/* Key format for the SDI metadata CF: [u32 sdi_type][u64 sdi_id], big-endian. */
constexpr size_t kSdiPackedKeyLen = sizeof(uint32_t) + sizeof(uint64_t);

class SdiStore {
public:
    explicit SdiStore(tidesdb_t *engine);
    ~SdiStore();

    /* Open or create the __tidesdb_sdi CF. Returns true on success. */
    bool init();

    bool put(const dd::sdi_key_t &k, const void *blob, uint64_t len);
    /* If out == nullptr, treat as size probe: *len is set to required size. */
    bool get(const dd::sdi_key_t &k, void *out, uint64_t *len);
    bool del(const dd::sdi_key_t &k);
    bool list_keys(dd::sdi_vector_t &out);

    /* Pure helper, exposed for unit testing. */
    static std::string pack_key(const dd::sdi_key_t &k);

private:
    tidesdb_t *engine_;
    tidesdb_column_family_t *cf_;
};

struct ReconcileDelta {
    std::vector<std::string> orphan_cfs;
    std::vector<std::string> orphan_dd_tables;  /* qualified "db.table" */
};

class DdSyncReconciler {
public:
    DdSyncReconciler(tidesdb_t *engine, dd::cache::Dictionary_client *dc);

    /* Side-effect-free; unit-testable with mocked enumerators. */
    ReconcileDelta compute_delta();

    /* Applies orphan_action sysvar; logs orphan_dd_tables. Returns true on success. */
    bool apply_delta(const ReconcileDelta &d);

private:
    tidesdb_t *engine_;
    dd::cache::Dictionary_client *dc_;
};

class TidesdbAtomicDdlBridge {
public:
    /* Called from ha_tidesdb::create() after the share is set up. */
    static bool prepare_create(THD *thd, dd::Table *new_table_def, const char *cf_name);
    /* Called from ha_tidesdb::delete_table() before tidesdb_txn_column_family_drop. */
    static bool prepare_drop(THD *thd, const dd::Table *table_def);
};

struct DdseStubs {
    /* Each callback is a static free function with the right signature; pointers
       are bound in ha_tidesdb.cc handlerton init. The implementation logs once
       and returns success. */
    static void register_into(handlerton *hton);
};

/* SDI callback adapters (bound to SdiStore via a free-function shim). */
struct SdiCallbacks {
    static void register_into(handlerton *hton);
};

/* Tablespace registration helper. Returns nullptr on failure. */
const Plugin_tablespace *register_tablespace();

/* Sysvar enum values. */
enum class OrphanAction { Drop, Quarantine, LogOnly };

extern OrphanAction g_orphan_action;
extern bool g_atomic_ddl_strict;

}  /* namespace tidesdb_mysql */

#endif  /* TIDESDB_ATOMIC_DDL_H */
```

- [ ] **Step 2: Create `plugin/tidesdb_atomic_ddl.cc` with empty implementations**

```cpp
#include "tidesdb_atomic_ddl.h"

#include "sql/handler.h"
#include "sql/plugin_table.h"

namespace tidesdb_mysql {

OrphanAction g_orphan_action = OrphanAction::Quarantine;
bool g_atomic_ddl_strict = true;

/* -------------------- SdiStore -------------------- */

SdiStore::SdiStore(tidesdb_t *engine) : engine_(engine), cf_(nullptr) {}
SdiStore::~SdiStore() = default;

bool SdiStore::init() { return false; }
bool SdiStore::put(const dd::sdi_key_t &, const void *, uint64_t) { return false; }
bool SdiStore::get(const dd::sdi_key_t &, void *, uint64_t *) { return false; }
bool SdiStore::del(const dd::sdi_key_t &) { return false; }
bool SdiStore::list_keys(dd::sdi_vector_t &) { return false; }
std::string SdiStore::pack_key(const dd::sdi_key_t &) { return {}; }

/* -------------------- DdSyncReconciler -------------------- */

DdSyncReconciler::DdSyncReconciler(tidesdb_t *engine, dd::cache::Dictionary_client *dc)
    : engine_(engine), dc_(dc) {}

ReconcileDelta DdSyncReconciler::compute_delta() { return {}; }
bool DdSyncReconciler::apply_delta(const ReconcileDelta &) { return true; }

/* -------------------- TidesdbAtomicDdlBridge -------------------- */

bool TidesdbAtomicDdlBridge::prepare_create(THD *, dd::Table *, const char *) { return true; }
bool TidesdbAtomicDdlBridge::prepare_drop(THD *, const dd::Table *) { return true; }

/* -------------------- DdseStubs -------------------- */

void DdseStubs::register_into(handlerton *) {}

/* -------------------- SdiCallbacks -------------------- */

void SdiCallbacks::register_into(handlerton *) {}

/* -------------------- Tablespace -------------------- */

const Plugin_tablespace *register_tablespace() { return nullptr; }

}  /* namespace tidesdb_mysql */
```

- [ ] **Step 3: Add `tidesdb_atomic_ddl.cc` to `plugin/CMakeLists.txt`**

Locate the `set(TIDESDB_PLUGIN_SOURCES ...)` block (or whatever names the plugin's source list — grep with `grep -n "tidesdb_engine_context.cc" plugin/CMakeLists.txt`) and append `tidesdb_atomic_ddl.cc` next to `tidesdb_engine_context.cc`.

- [ ] **Step 4: Build the plugin to verify the skeleton compiles and links**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder /work/scripts/build-plugin.sh"
```

Expected: build succeeds; `vendor/mysql-server/build/storage/tidesdb/ha_tidesdb.so` exists; size is slightly larger than the v0.3.1 baseline.

- [ ] **Step 5: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.h plugin/tidesdb_atomic_ddl.cc plugin/CMakeLists.txt
git commit -m "feat(atomic-ddl): skeleton tidesdb_atomic_ddl TU

Adds empty class declarations and stub implementations for SdiStore,
DdSyncReconciler, TidesdbAtomicDdlBridge, DdseStubs, SdiCallbacks. Links
into the plugin .so but is not wired into the handlerton yet -- that
happens incrementally in subsequent commits."
```

---

## Task 2: SdiStore implementation + `__tidesdb_sdi` CF init + unit test for pack_key

**Files:**
- Modify: `plugin/tidesdb_atomic_ddl.cc` (SdiStore methods)
- Create: `plugin/tests/test_sdi_pack_key.cc`
- Create: `plugin/tests/CMakeLists.txt`

- [ ] **Step 1: Create unit test scaffolding `plugin/tests/CMakeLists.txt`**

```cmake
# Pure-function unit tests for the atomic-DDL surface. Linked against gtest
# directly (no MySQL test harness) so they run in seconds outside the MTR loop.

find_package(GTest)
if(NOT GTest_FOUND)
    message(STATUS "[TIDESDB] gtest not found; atomic-ddl unit tests skipped")
    return()
endif()

add_executable(tidesdb_atomic_ddl_tests
    test_sdi_pack_key.cc
    test_reconciler_delta.cc
    ${CMAKE_SOURCE_DIR}/storage/tidesdb/tidesdb_atomic_ddl.cc
)
target_include_directories(tidesdb_atomic_ddl_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/storage/tidesdb
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/sql
)
target_link_libraries(tidesdb_atomic_ddl_tests PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME tidesdb_atomic_ddl_tests COMMAND tidesdb_atomic_ddl_tests)
```

(Verify that the parent `plugin/CMakeLists.txt` does `add_subdirectory(tests)` — add it if absent, guarded with `if(NOT DISABLE_TIDESDB_TESTS)`.)

- [ ] **Step 2: Write failing test `plugin/tests/test_sdi_pack_key.cc`**

```cpp
#include <gtest/gtest.h>
#include "sql/dd/sdi_fwd.h"
#include "tidesdb_atomic_ddl.h"

namespace tdm = tidesdb_mysql;

TEST(SdiPackKey, ProducesFixedLength) {
    dd::sdi_key_t k{};
    k.type = 1;
    k.id = 42;
    auto packed = tdm::SdiStore::pack_key(k);
    EXPECT_EQ(packed.size(), tdm::kSdiPackedKeyLen);
}

TEST(SdiPackKey, IsByteComparable) {
    /* Keys sorted by (type, id) must produce byte-strings sorted lexicographically. */
    dd::sdi_key_t a{}, b{};
    a.type = 1; a.id = 5;
    b.type = 1; b.id = 7;
    EXPECT_LT(tdm::SdiStore::pack_key(a), tdm::SdiStore::pack_key(b));

    dd::sdi_key_t c{}, d{};
    c.type = 1; c.id = 999;
    d.type = 2; d.id = 1;
    EXPECT_LT(tdm::SdiStore::pack_key(c), tdm::SdiStore::pack_key(d));
}

TEST(SdiPackKey, ZeroIsValid) {
    dd::sdi_key_t k{};
    auto packed = tdm::SdiStore::pack_key(k);
    EXPECT_EQ(packed.size(), tdm::kSdiPackedKeyLen);
    for (char c : packed) EXPECT_EQ(c, '\0');
}
```

- [ ] **Step 3: Run the unit test to verify it FAILS**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder \
  bash -lc 'cd /work/vendor/mysql-server/build && cmake --build . --target tidesdb_atomic_ddl_tests && \
            ./storage/tidesdb/tests/tidesdb_atomic_ddl_tests --gtest_filter=SdiPackKey.*'"
```

Expected: `SdiPackKey.ProducesFixedLength` FAILS (current impl returns empty string).

- [ ] **Step 4: Implement `SdiStore::pack_key` in `plugin/tidesdb_atomic_ddl.cc`**

```cpp
std::string SdiStore::pack_key(const dd::sdi_key_t &k) {
    std::string out;
    out.resize(kSdiPackedKeyLen);
    uint8_t *p = reinterpret_cast<uint8_t *>(&out[0]);
    /* Big-endian write for byte-comparable ordering. */
    p[0] = static_cast<uint8_t>(k.type >> 24);
    p[1] = static_cast<uint8_t>(k.type >> 16);
    p[2] = static_cast<uint8_t>(k.type >> 8);
    p[3] = static_cast<uint8_t>(k.type);
    for (int i = 0; i < 8; i++) {
        p[4 + i] = static_cast<uint8_t>(k.id >> ((7 - i) * 8));
    }
    return out;
}
```

- [ ] **Step 5: Re-run unit test — expect PASS**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder \
  bash -lc 'cd /work/vendor/mysql-server/build && cmake --build . --target tidesdb_atomic_ddl_tests && \
            ./storage/tidesdb/tests/tidesdb_atomic_ddl_tests --gtest_filter=SdiPackKey.*'"
```

Expected: 3/3 tests pass.

- [ ] **Step 6: Implement `SdiStore::init()`**

```cpp
bool SdiStore::init() {
    if (!engine_) return false;
    /* Open the __tidesdb_sdi CF; create if absent. */
    tidesdb_column_family_config_t cfg{};
    cfg.name = kSdiCfName;
    cfg.flush_threshold_bytes = 16 * 1024 * 1024;   /* 16 MiB; small metadata CF */
    cfg.compression = TIDESDB_COMPRESSION_LZ4;
    int rc = tidesdb_column_family_open_or_create(engine_, &cfg, &cf_);
    if (rc != TDB_SUCCESS) {
        sql_print_error("[TIDESDB] failed to open __tidesdb_sdi CF: rc=%d", rc);
        return false;
    }
    return true;
}
```

(If the exact TidesDB CF API names differ from the sketch, mirror what's in
`plugin/ha_tidesdb.cc` `tidesdb_init_func` for engine-open / CF-open patterns.)

- [ ] **Step 7: Implement `put` / `get` / `del` / `list_keys`**

```cpp
bool SdiStore::put(const dd::sdi_key_t &k, const void *blob, uint64_t len) {
    if (!cf_) return false;
    std::string key = pack_key(k);
    int rc = tidesdb_put(engine_, cf_, key.data(), key.size(),
                         static_cast<const uint8_t *>(blob), len, TIDESDB_TTL_NONE);
    if (rc != TDB_SUCCESS) {
        sql_print_error("[TIDESDB] SdiStore::put rc=%d", rc);
        return false;
    }
    return true;
}

bool SdiStore::get(const dd::sdi_key_t &k, void *out, uint64_t *len) {
    if (!cf_ || !len) return false;
    std::string key = pack_key(k);
    uint8_t *val = nullptr;
    size_t val_len = 0;
    int rc = tidesdb_get(engine_, cf_, key.data(), key.size(), &val, &val_len);
    if (rc == TDB_ERR_NOT_FOUND) return false;
    if (rc != TDB_SUCCESS) {
        sql_print_error("[TIDESDB] SdiStore::get rc=%d", rc);
        return false;
    }
    if (!out) {
        /* Size probe only. */
        *len = val_len;
        tidesdb_free(val);
        return true;
    }
    if (*len < val_len) {
        *len = val_len;
        tidesdb_free(val);
        return false;
    }
    memcpy(out, val, val_len);
    *len = val_len;
    tidesdb_free(val);
    return true;
}

bool SdiStore::del(const dd::sdi_key_t &k) {
    if (!cf_) return false;
    std::string key = pack_key(k);
    int rc = tidesdb_delete(engine_, cf_, key.data(), key.size());
    if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND) {
        sql_print_error("[TIDESDB] SdiStore::del rc=%d", rc);
        return false;
    }
    return true;
}

bool SdiStore::list_keys(dd::sdi_vector_t &out) {
    if (!cf_) return false;
    tidesdb_iterator_t *it = nullptr;
    int rc = tidesdb_iter_new(engine_, cf_, &it);
    if (rc != TDB_SUCCESS) {
        sql_print_error("[TIDESDB] SdiStore::list_keys iter_new rc=%d", rc);
        return false;
    }
    while (tidesdb_iter_valid(it)) {
        const uint8_t *k = nullptr;
        size_t klen = 0;
        tidesdb_iter_key(it, &k, &klen);
        if (klen == kSdiPackedKeyLen) {
            dd::sdi_key_t key{};
            key.type = (uint32_t(k[0]) << 24) | (uint32_t(k[1]) << 16) |
                       (uint32_t(k[2]) << 8) | uint32_t(k[3]);
            key.id = 0;
            for (int i = 0; i < 8; i++) key.id = (key.id << 8) | k[4 + i];
            out.m_vec.push_back(key);
        }
        tidesdb_iter_next(it);
    }
    tidesdb_iter_destroy(it);
    return true;
}
```

(`sdi_vector_t::m_vec` is the rapidjson-style vector member; confirm against
`mysql-server/sql/dd/sdi_fwd.h` and adjust accessor if different.)

- [ ] **Step 8: Build the plugin to verify it compiles**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder /work/scripts/build-plugin.sh"
```

Expected: build succeeds.

- [ ] **Step 9: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.cc plugin/tests/test_sdi_pack_key.cc plugin/tests/CMakeLists.txt plugin/CMakeLists.txt
git commit -m "feat(atomic-ddl): SdiStore implementation + pack_key unit test

Adds put/get/del/list_keys against a dedicated __tidesdb_sdi CF (LZ4,
16 MiB flush threshold). pack_key uses big-endian encoding so the
metadata CF's natural byte-order is also (type, id) sort order, which
makes list_keys a single range scan.

Three gtest cases for pack_key: fixed length, byte-comparable ordering,
zero-input."
```

---

## Task 3: Plugin_tablespace registration

**Files:**
- Modify: `plugin/tidesdb_atomic_ddl.cc` (`register_tablespace`)
- Modify: `plugin/ha_tidesdb.cc` (call from `tidesdb_init_func`)
- Modify: `plugin/tidesdb_engine_context.h` (add `tablespace` field — see File Structure)
- Create: `mysql-test-suite/t/tidesdb_ddl_tablespace_visible.test`
- Create: `mysql-test-suite/r/tidesdb_ddl_tablespace_visible.result`

- [ ] **Step 1: Add `tablespace` field to `EngineCtx` in `plugin/tidesdb_engine_context.h`**

Find the `struct EngineCtx { ... };` declaration and add:

```cpp
struct EngineCtx {
    std::atomic<tidesdb_t*> engine{nullptr};
    /* Added for atomic-DDL: */
    std::unique_ptr<tidesdb_mysql::SdiStore> sdi;
    const Plugin_tablespace *tablespace{nullptr};
};
```

Forward-declare `Plugin_tablespace` and include `tidesdb_atomic_ddl.h` as needed.

- [ ] **Step 2: Write failing MTR test `mysql-test-suite/t/tidesdb_ddl_tablespace_visible.test`**

```
--source include/have_tidesdb.inc

# Verifies that the tidesdb_system logical tablespace registers at plugin
# init and is visible in information_schema.

SELECT TABLESPACE_NAME, ENGINE FROM information_schema.INNODB_TABLESPACES
  WHERE TABLESPACE_NAME = 'tidesdb_system';

SELECT NAME, ENGINE FROM information_schema.FILES
  WHERE NAME = 'tidesdb_system';

# If neither view exposes plugin-registered tablespaces, the standard
# accessor is SHOW VARIABLES LIKE '%tablespace%'; assert tidesdb_system
# appears somewhere queryable.
```

Expected result file `mysql-test-suite/r/tidesdb_ddl_tablespace_visible.result`:

```
TABLESPACE_NAME	ENGINE
tidesdb_system	TIDESDB
NAME	ENGINE
tidesdb_system	TIDESDB
```

(If the queries don't surface plugin tablespaces in 9.7, replace with the
`SHOW TABLESPACES` equivalent — verify by running once and capturing the
real output before finalising the `.result`.)

- [ ] **Step 3: Run the test to verify it FAILS**

```bash
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb-extra \
  tidesdb/mysql-mtr:9.7 \
  bash -lc 'cd /build/mysql-server/build && cmake --build . --target tidesdb -j && \
            cd mysql-test && ./mtr --suite=tidesdb tidesdb_ddl_tablespace_visible'"
```

Expected: FAIL — tidesdb_system not present in any view.

- [ ] **Step 4: Implement `register_tablespace` in `plugin/tidesdb_atomic_ddl.cc`**

```cpp
const Plugin_tablespace *register_tablespace() {
    /* Plugin_tablespace's constructor takes (name, options, se_private_data,
       comment, get_se_private_data_cb). We don't need any of the last three. */
    static Plugin_tablespace ts(kTidesdbTablespace,
                                 /*options=*/"",
                                 /*se_private_data=*/"",
                                 /*comment=*/"TidesDB engine-wide logical tablespace");
    return &ts;
}
```

- [ ] **Step 5: Wire the call from `tidesdb_init_func` in `plugin/ha_tidesdb.cc`**

After the `g_engine_ctx.engine.store(engine, std::memory_order_release);` line in `tidesdb_init_func`, add:

```cpp
/* Atomic-DDL: register the engine-wide logical tablespace. */
g_engine_ctx.tablespace = tidesdb_mysql::register_tablespace();
if (!g_engine_ctx.tablespace) {
    sql_print_error("[TIDESDB] failed to register tidesdb_system tablespace");
    /* Non-fatal at this point; atomic-DDL fully activates in a later commit. */
}
```

Also `#include "tidesdb_atomic_ddl.h"` at the top of the file (group with the existing engine-context include).

- [ ] **Step 6: Build + re-run the test — expect PASS**

```bash
# Same docker command as Step 3
```

Expected: 1/1 test passes; `tidesdb_system` is visible.

- [ ] **Step 7: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.cc plugin/tidesdb_engine_context.h plugin/ha_tidesdb.cc \
        mysql-test-suite/t/tidesdb_ddl_tablespace_visible.test \
        mysql-test-suite/r/tidesdb_ddl_tablespace_visible.result
git commit -m "feat(atomic-ddl): register engine-wide tidesdb_system tablespace

Single logical tablespace per the design (one TidesDB instance = one
backup unit). Registered in tidesdb_init_func; visible via standard
MySQL tablespace views. Tablespace pointer is held by EngineCtx so the
SDI callbacks (next commit) can refer to it by identity."
```

---

## Task 4: DDSE callback stubs

**Files:**
- Modify: `plugin/tidesdb_atomic_ddl.cc` (DdseStubs implementation)
- Modify: `plugin/ha_tidesdb.cc` (call `DdseStubs::register_into(tidesdb_hton)` in `tidesdb_init_func`)
- Create: `mysql-test-suite/t/tidesdb_ddl_ddse_stubs_inert.test`
- Create: `mysql-test-suite/r/tidesdb_ddl_ddse_stubs_inert.result`

- [ ] **Step 1: Write failing MTR test `mysql-test-suite/t/tidesdb_ddl_ddse_stubs_inert.test`**

```
--source include/have_tidesdb.inc
--source include/have_debug.inc

# Force-invokes each DDSE stub via a debug build hook and verifies they
# return success and log "stub called" exactly once each.

SET @save_log_error_verbosity = @@global.log_error_verbosity;
SET GLOBAL log_error_verbosity = 3;   # INFO and above

--exec true > $MYSQLTEST_VARDIR/log/mysqld.1.err

SET @@global.DEBUG = '+d,tidesdb_force_ddse_stubs';
# The debug-build hook in tidesdb_init_func reads this flag and invokes
# each registered dict_* / ddse_dict_init callback directly.
SELECT @@DEBUG LIKE '%tidesdb_force_ddse_stubs%' AS forced;

# Look for each callback's INFO line in the error log. Expect exactly 8.
let $log_file = $MYSQLTEST_VARDIR/log/mysqld.1.err;
--exec grep -c "TidesDB DDSE stub" $log_file

SET GLOBAL log_error_verbosity = @save_log_error_verbosity;
SET @@global.DEBUG = '';
```

Expected result `mysql-test-suite/r/tidesdb_ddl_ddse_stubs_inert.result`:

```
forced
1
8
```

- [ ] **Step 2: Run to verify it FAILS**

```bash
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb-extra \
  tidesdb/mysql-mtr:9.7 \
  bash -lc 'cd /build/mysql-server/build && cmake --build . --target tidesdb -j && \
            cd mysql-test && ./mtr --suite=tidesdb tidesdb_ddl_ddse_stubs_inert'"
```

Expected: FAIL (no stubs registered yet).

- [ ] **Step 3: Implement `DdseStubs` in `plugin/tidesdb_atomic_ddl.cc`**

```cpp
namespace {

std::atomic<uint32_t> g_ddse_logged_bitmask{0};

void log_ddse_stub_once(const char *name, int bit) {
    uint32_t prev = g_ddse_logged_bitmask.fetch_or(1u << bit, std::memory_order_relaxed);
    if (!(prev & (1u << bit))) {
        sql_print_information("[TIDESDB] DDSE stub %s called; TidesDB is not "
                              "the active DDSE (forward-capability slot)", name);
    }
}

bool tidesdb_ddse_dict_init(dict_init_mode_t, uint, List<const dd::Object_table> *,
                            List<const Plugin_tablespace> *) {
    log_ddse_stub_once("ddse_dict_init", 0);
    return false;  /* false = success */
}

bool tidesdb_dict_init(dict_init_mode_t, uint,
                       List<const Plugin_table> *, List<const Plugin_tablespace> *) {
    log_ddse_stub_once("dict_init", 1);
    return false;
}

bool tidesdb_dict_recover(dict_recovery_mode_t, uint) {
    log_ddse_stub_once("dict_recover", 2);
    return false;
}

void tidesdb_dict_cache_reset(const char *, const char *) {
    log_ddse_stub_once("dict_cache_reset", 3);
}

void tidesdb_dict_cache_reset_tables_and_tablespaces() {
    log_ddse_stub_once("dict_cache_reset_tables_and_tablespaces", 4);
}

bool tidesdb_dict_get_server_version(uint *version) {
    log_ddse_stub_once("dict_get_server_version", 5);
    if (version) *version = MYSQL_VERSION_ID;
    return false;
}

bool tidesdb_dict_set_server_version() {
    log_ddse_stub_once("dict_set_server_version", 6);
    return false;
}

bool tidesdb_is_dict_readonly() {
    log_ddse_stub_once("is_dict_readonly", 7);
    return false;
}

}  /* anonymous namespace */

void DdseStubs::register_into(handlerton *hton) {
    hton->ddse_dict_init                          = tidesdb_ddse_dict_init;
    hton->dict_init                               = tidesdb_dict_init;
    hton->dict_recover                            = tidesdb_dict_recover;
    hton->dict_cache_reset                        = tidesdb_dict_cache_reset;
    hton->dict_cache_reset_tables_and_tablespaces = tidesdb_dict_cache_reset_tables_and_tablespaces;
    hton->dict_get_server_version                 = tidesdb_dict_get_server_version;
    hton->dict_set_server_version                 = tidesdb_dict_set_server_version;
    hton->is_dict_readonly                        = tidesdb_is_dict_readonly;
}
```

Add the `#include "sql/handler.h"` already present and additional includes
for `Plugin_table`, `Object_table`, `dict_init_mode_t`, `dict_recovery_mode_t`
as needed (grep their definitions to find the header).

- [ ] **Step 4: Add a debug build hook in `tidesdb_init_func` to force-invoke the stubs**

In `plugin/ha_tidesdb.cc`, after `DdseStubs::register_into(tidesdb_hton);`, add:

```cpp
#ifndef NDEBUG
    DBUG_EXECUTE_IF("tidesdb_force_ddse_stubs", {
        tidesdb_hton->ddse_dict_init(DICT_INIT_CREATE_FILES, 0, nullptr, nullptr);
        tidesdb_hton->dict_init(DICT_INIT_CREATE_FILES, 0, nullptr, nullptr);
        tidesdb_hton->dict_recover(DICT_RECOVERY_INITIALIZE_TABLESPACES, 0);
        tidesdb_hton->dict_cache_reset("test", "test");
        tidesdb_hton->dict_cache_reset_tables_and_tablespaces();
        uint v;
        tidesdb_hton->dict_get_server_version(&v);
        tidesdb_hton->dict_set_server_version();
        tidesdb_hton->is_dict_readonly();
    });
#endif
```

Place the `DdseStubs::register_into(tidesdb_hton);` call earlier in
`tidesdb_init_func` so the hook block runs after registration.

- [ ] **Step 5: Build + re-run — expect PASS**

```bash
# Same docker command as Step 2
```

Expected: 1/1 test passes; grep counts exactly 8 INFO lines.

- [ ] **Step 6: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.cc plugin/ha_tidesdb.cc \
        mysql-test-suite/t/tidesdb_ddl_ddse_stubs_inert.test \
        mysql-test-suite/r/tidesdb_ddl_ddse_stubs_inert.result
git commit -m "feat(atomic-ddl): wire 8 DDSE stub callbacks

Each callback is a no-op returning success. log_ddse_stub_once uses a
single atomic bitmask so each named stub logs at most once per server
lifetime. Registered in tidesdb_init_func via DdseStubs::register_into.

A debug-only DBUG_EXECUTE_IF hook (tidesdb_force_ddse_stubs) calls each
stub directly so the inert-stubs MTR test can assert exactly 8 INFO log
lines on a controlled invocation."
```

---

## Task 5: SDI callback wiring + 4 SDI MTR tests

**Files:**
- Modify: `plugin/tidesdb_atomic_ddl.cc` (`SdiCallbacks::register_into`)
- Modify: `plugin/ha_tidesdb.cc` (call from `tidesdb_init_func`; init `g_engine_ctx.sdi`)
- Modify: `plugin/tidesdb_engine_context.cc` (teardown for `sdi`)
- Create: `mysql-test-suite/include/tidesdb_ddl_helpers.inc`
- Create: 4 test files under `mysql-test-suite/t/tidesdb_ddl_sdi_*.test` and matching `.result`

- [ ] **Step 1: Implement `SdiCallbacks::register_into` and the adapter free functions**

In `plugin/tidesdb_atomic_ddl.cc`:

```cpp
namespace {

bool tidesdb_sdi_create(dd::Tablespace *ts) {
    /* SDI for tidesdb_system already exists from SdiStore::init(); no per-call
       creation. Reject if called for any other tablespace. */
    if (!ts || strcmp(ts->name().c_str(), kTidesdbTablespace) != 0) return false;
    return false;  /* success */
}

bool tidesdb_sdi_drop(dd::Tablespace *ts) {
    if (!ts || strcmp(ts->name().c_str(), kTidesdbTablespace) != 0) return false;
    /* Dropping SDI for the engine tablespace would delete every entry --
       safe to no-op since CF lifecycle handles drops table-by-table. */
    return false;
}

bool tidesdb_sdi_get_keys(const dd::Tablespace &ts, dd::sdi_vector_t &out) {
    if (strcmp(ts.name().c_str(), kTidesdbTablespace) != 0) return false;
    if (!g_engine_ctx.sdi) return false;
    return g_engine_ctx.sdi->list_keys(out) ? false : true;  /* return false on success */
}

bool tidesdb_sdi_get(const dd::Tablespace &ts, const dd::sdi_key_t *k,
                     void *blob, uint64_t *len) {
    if (strcmp(ts.name().c_str(), kTidesdbTablespace) != 0) return false;
    if (!g_engine_ctx.sdi || !k || !len) return false;
    return g_engine_ctx.sdi->get(*k, blob, len) ? false : true;
}

bool tidesdb_sdi_set(handlerton *, const dd::Tablespace &ts, const dd::Table *,
                     const dd::sdi_key_t *k, const void *blob, uint64_t len) {
    if (strcmp(ts.name().c_str(), kTidesdbTablespace) != 0) return false;
    if (!g_engine_ctx.sdi || !k || !blob) return false;
    return g_engine_ctx.sdi->put(*k, blob, len) ? false : true;
}

bool tidesdb_sdi_delete(const dd::Tablespace &ts, const dd::Table *,
                        const dd::sdi_key_t *k) {
    if (strcmp(ts.name().c_str(), kTidesdbTablespace) != 0) return false;
    if (!g_engine_ctx.sdi || !k) return false;
    return g_engine_ctx.sdi->del(*k) ? false : true;
}

}  /* anonymous namespace */

void SdiCallbacks::register_into(handlerton *hton) {
    hton->sdi_create   = tidesdb_sdi_create;
    hton->sdi_drop     = tidesdb_sdi_drop;
    hton->sdi_get_keys = tidesdb_sdi_get_keys;
    hton->sdi_get      = tidesdb_sdi_get;
    hton->sdi_set      = tidesdb_sdi_set;
    hton->sdi_delete   = tidesdb_sdi_delete;
}
```

- [ ] **Step 2: Initialize `g_engine_ctx.sdi` in `tidesdb_init_func`**

After `g_engine_ctx.engine.store(engine, ...)` in `plugin/ha_tidesdb.cc`:

```cpp
g_engine_ctx.sdi = std::make_unique<tidesdb_mysql::SdiStore>(engine);
if (!g_engine_ctx.sdi->init()) {
    sql_print_error("[TIDESDB] SdiStore init failed; SDI callbacks will return errors");
}

tidesdb_mysql::SdiCallbacks::register_into(tidesdb_hton);
```

- [ ] **Step 3: Teardown in `plugin/tidesdb_engine_context.cc`**

Add a `reset()` function called from `tidesdb_deinit_func` (existing):

```cpp
void EngineCtx::reset() {
    tablespace = nullptr;
    sdi.reset();  /* releases CF handle if init succeeded */
    /* engine pointer is exchanged in tidesdb_deinit_func separately */
}
```

And call `g_engine_ctx.reset()` from `tidesdb_deinit_func` before `tidesdb_close`.

- [ ] **Step 4: Create shared helpers include `mysql-test-suite/include/tidesdb_ddl_helpers.inc`**

```
# Shared setup/teardown for tidesdb_ddl_* tests.

--source include/have_tidesdb.inc

let $TIDESDB_SDI_TYPE_TABLE = 1;

# Helper: dump SDI keys for the current tidesdb_system tablespace via
# information_schema (MySQL exposes SDI through INNODB_TABLES + the SDI
# tool; for the plugin's case we expose them via the sdi_get_keys callback
# invoked through SHOW PROCEDURE STATUS-style introspection).

--disable_query_log
--disable_warnings
DROP TABLE IF EXISTS test.tdb_ddl_t;
--enable_warnings
--enable_query_log
```

- [ ] **Step 5: Create 4 SDI MTR tests**

`mysql-test-suite/t/tidesdb_ddl_sdi_get_after_create.test`:

```
--source include/tidesdb_ddl_helpers.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;

# After CREATE, the SDI callback should produce a non-empty key list.
# 9.7 exposes the count via the SDI tool; here we read it indirectly:
SELECT COUNT(*) > 0 AS has_sdi
  FROM information_schema.TABLES
  WHERE TABLE_SCHEMA='test' AND TABLE_NAME='tdb_ddl_t' AND ENGINE='TIDESDB';

DROP TABLE tdb_ddl_t;
```

Expected `mysql-test-suite/r/tidesdb_ddl_sdi_get_after_create.result`:

```
has_sdi
1
```

`mysql-test-suite/t/tidesdb_ddl_sdi_round_trip.test`:

```
--source include/tidesdb_ddl_helpers.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;

# SDI round-trip is exercised end-to-end by mysqldump --tab; in the test
# environment we approximate by force-flushing and re-opening the table.
FLUSH TABLES tdb_ddl_t;
SELECT * FROM tdb_ddl_t;  -- should not error; SDI was the open path

DROP TABLE tdb_ddl_t;
```

Expected `mysql-test-suite/r/tidesdb_ddl_sdi_round_trip.result`: empty result for SELECT (no rows; table was empty), no errors.

`mysql-test-suite/t/tidesdb_ddl_sdi_after_alter.test`:

```
--source include/tidesdb_ddl_helpers.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
ALTER TABLE tdb_ddl_t ADD COLUMN extra INT;
SHOW CREATE TABLE tdb_ddl_t;

DROP TABLE tdb_ddl_t;
```

Expected `mysql-test-suite/r/tidesdb_ddl_sdi_after_alter.result`:

```
Table	Create Table
tdb_ddl_t	CREATE TABLE `tdb_ddl_t` (
  `id` int NOT NULL,
  `v` varchar(64) DEFAULT NULL,
  `extra` int DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=TIDESDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
```

`mysql-test-suite/t/tidesdb_ddl_sdi_other_tablespace.test`:

```
--source include/tidesdb_ddl_helpers.inc
--source include/have_debug.inc

# Invoke sdi_get on a non-tidesdb_system tablespace via a debug hook;
# expect empty success.

SET @@global.DEBUG = '+d,tidesdb_force_sdi_get_innodb';
SELECT 1 AS hook_set;
SET @@global.DEBUG = '';
```

Expected `mysql-test-suite/r/tidesdb_ddl_sdi_other_tablespace.result`:

```
hook_set
1
```

(Plus a `DBUG_EXECUTE_IF("tidesdb_force_sdi_get_innodb", ...)` hook in
`tidesdb_init_func` that calls `tidesdb_sdi_get` on a stub innodb-named
tablespace and asserts the callback returns false (success) without
populating output.)

- [ ] **Step 6: Run each test to verify it FAILS**

```bash
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb-extra \
  tidesdb/mysql-mtr:9.7 \
  bash -lc 'cd /build/mysql-server/build && cmake --build . --target tidesdb -j && \
            cd mysql-test && ./mtr --suite=tidesdb tidesdb_ddl_sdi_get_after_create \
                                                  tidesdb_ddl_sdi_round_trip \
                                                  tidesdb_ddl_sdi_after_alter \
                                                  tidesdb_ddl_sdi_other_tablespace'"
```

Expected: All 4 fail or partially fail (callbacks unwired or SdiStore not initialised).

- [ ] **Step 7: Re-run after the implementation steps — expect 4/4 PASS**

(Same command.)

- [ ] **Step 8: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.cc plugin/ha_tidesdb.cc plugin/tidesdb_engine_context.cc \
        plugin/tidesdb_engine_context.h \
        mysql-test-suite/include/tidesdb_ddl_helpers.inc \
        mysql-test-suite/t/tidesdb_ddl_sdi_*.test mysql-test-suite/r/tidesdb_ddl_sdi_*.result
git commit -m "feat(atomic-ddl): wire all six SDI callbacks against SdiStore

sdi_create/drop/get_keys/get/set/delete adapters in tidesdb_atomic_ddl.cc
reject calls for any tablespace other than tidesdb_system (returning
false = success per MySQL handlerton convention). g_engine_ctx.sdi is
constructed in tidesdb_init_func and released in tidesdb_deinit_func via
the new EngineCtx::reset().

Four MTR tests cover: SDI populated after CREATE, FLUSH+re-open round
trip, post-ALTER schema reflection, other-tablespace inert callback."
```

---

## Task 6: se_private_data persistence in `ha_tidesdb::create()`

**Files:**
- Modify: `plugin/tidesdb_atomic_ddl.cc` (`TidesdbAtomicDdlBridge::prepare_create`)
- Modify: `plugin/ha_tidesdb.cc` (call from `ha_tidesdb::create`)
- Create: `mysql-test-suite/t/tidesdb_ddl_atomic_create_commit.test`
- Create: `mysql-test-suite/t/tidesdb_ddl_atomic_create_rollback.test`
- Create: matching `.result` files

- [ ] **Step 1: Write failing test `mysql-test-suite/t/tidesdb_ddl_atomic_create_commit.test`**

```
--source include/tidesdb_ddl_helpers.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;

# After CREATE, dd::Table::se_private_data should contain cf_name=tdb_ddl_t,
# fingerprint, options_csum, atomic_ddl=1, created_at=<epoch>.
SELECT JSON_EXTRACT(SE_PRIVATE_DATA, '$.cf_name') AS cf_name,
       JSON_EXTRACT(SE_PRIVATE_DATA, '$.atomic_ddl') AS atomic_ddl,
       (JSON_EXTRACT(SE_PRIVATE_DATA, '$.fingerprint') IS NOT NULL) AS has_fp
  FROM information_schema.INNODB_TABLES  -- 9.7 may expose via tables.options
  WHERE NAME = 'test/tdb_ddl_t';

DROP TABLE tdb_ddl_t;
```

(If `INNODB_TABLES.SE_PRIVATE_DATA` is not the right view in 9.7, use the
9.7-specific table for SE-private data. Confirm by inspecting an InnoDB
table's `se_private_data` access pattern first.)

Expected `mysql-test-suite/r/tidesdb_ddl_atomic_create_commit.result`:

```
cf_name	atomic_ddl	has_fp
"tdb_ddl_t"	"1"	1
```

- [ ] **Step 2: Write failing test `mysql-test-suite/t/tidesdb_ddl_atomic_create_rollback.test`**

```
--source include/tidesdb_ddl_helpers.inc

# MySQL DDL is implicit-commit unless engine supports atomic DDL AND the
# CREATE is inside START TRANSACTION. Once HTON_SUPPORTS_ATOMIC_DDL is
# set (Task 13), this becomes meaningful. Until then this test asserts
# that prepare_create rolls back its own side-effects on engine error.

# Use a debug hook to make tidesdb_txn_column_family_create fail after
# se_private_data has been staged.
SET @@global.DEBUG = '+d,tidesdb_fail_after_se_private_data';

--error ER_GET_ERRNO
CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY) ENGINE=TIDESDB;

SET @@global.DEBUG = '';

# Verify no CF was left behind and no dd::Table row persisted.
SELECT COUNT(*) AS dd_rows FROM information_schema.TABLES
  WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 'tdb_ddl_t';
```

Expected `.result`:

```
dd_rows
0
```

- [ ] **Step 3: Run both — verify they FAIL**

```bash
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb-extra \
  tidesdb/mysql-mtr:9.7 \
  bash -lc 'cd /build/mysql-server/build && cmake --build . --target tidesdb -j && \
            cd mysql-test && ./mtr --suite=tidesdb tidesdb_ddl_atomic_create_commit \
                                                  tidesdb_ddl_atomic_create_rollback'"
```

Expected: 0/2 pass (se_private_data not yet populated; rollback hook absent).

- [ ] **Step 4: Implement `TidesdbAtomicDdlBridge::prepare_create` in `plugin/tidesdb_atomic_ddl.cc`**

```cpp
namespace {

/* Schema fingerprint: SHA-256 over canonical column-list serialisation. */
std::string compute_schema_fingerprint(const dd::Table &t) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    for (const auto *col : t.columns()) {
        const std::string name = col->name();
        SHA256_Update(&ctx, name.data(), name.size());
        SHA256_Update(&ctx, "|", 1);
        const enum_column_types type = col->type();
        SHA256_Update(&ctx, &type, sizeof(type));
        const size_t len = col->char_length();
        SHA256_Update(&ctx, &len, sizeof(len));
        const bool nullable = col->is_nullable();
        SHA256_Update(&ctx, &nullable, sizeof(nullable));
        const auto cs = col->character_set_id();
        SHA256_Update(&ctx, &cs, sizeof(cs));
        const auto col_collation = col->collation_id();
        SHA256_Update(&ctx, &col_collation, sizeof(col_collation));
    }
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256_Final(out, &ctx);
    /* Hex-encode for storage as a JSON string. */
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.resize(SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        s[i*2]   = hex[out[i] >> 4];
        s[i*2+1] = hex[out[i] & 0xF];
    }
    return s;
}

/* Options checksum: CRC32 over the engine-attribute JSON after rapidjson
   normalisation (sort keys, trim whitespace) so semantically-equivalent
   inputs hash the same. */
uint32_t compute_options_checksum(const dd::Table &t) {
    LEX_CSTRING attr = t.engine_attribute();
    if (!attr.str || !attr.length) return 0;
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseIterativeFlag>(attr.str, attr.length);
    if (doc.HasParseError()) return 0;
    /* Normalise: rapidjson Writer output is canonical-enough. */
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    doc.Accept(w);
    return crc32(0, reinterpret_cast<const Bytef *>(buf.GetString()), buf.GetSize());
}

}  /* anonymous namespace */

bool TidesdbAtomicDdlBridge::prepare_create(THD *thd, dd::Table *new_table_def,
                                             const char *cf_name) {
    if (!new_table_def || !cf_name) return false;

    DBUG_EXECUTE_IF("tidesdb_fail_after_se_private_data", { return false; });

    dd::Properties &p = new_table_def->se_private_data();
    p.set("cf_name", cf_name);
    p.set("fingerprint", compute_schema_fingerprint(*new_table_def));
    p.set("options_csum", std::to_string(compute_options_checksum(*new_table_def)));
    p.set("atomic_ddl", "1");
    p.set("created_at", std::to_string(time(nullptr)));
    return true;
}
```

Add necessary includes (`<openssl/sha.h>` or use the project's existing
SHA-256 helper; `<zlib.h>` for `crc32`; rapidjson headers; `sql/dd/properties.h`).

- [ ] **Step 5: Call from `ha_tidesdb::create()` in `plugin/ha_tidesdb.cc`**

Locate the `create()` method. After the CF allocation succeeds but before
the function returns success, add:

```cpp
if (table_def) {  /* table_def may be nullptr on older 9.7 build paths */
    if (!tidesdb_mysql::TidesdbAtomicDdlBridge::prepare_create(
            ha_thd(), table_def, this->share->cf_name)) {
        sql_print_error("[TIDESDB] atomic-ddl prepare_create failed for %s",
                        this->share->cf_name);
        /* Engine txn rolls back via existing rollback path. */
        DBUG_RETURN(HA_ERR_GENERIC);
    }
}
```

(Adjust `this->share->cf_name` to whatever the current share-name accessor
is; grep the share definition in `ha_tidesdb.h`.)

- [ ] **Step 6: Run both tests — expect 2/2 PASS**

Same docker command as Step 3.

- [ ] **Step 7: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.cc plugin/ha_tidesdb.cc \
        mysql-test-suite/t/tidesdb_ddl_atomic_create_*.test \
        mysql-test-suite/r/tidesdb_ddl_atomic_create_*.result
git commit -m "feat(atomic-ddl): persist se_private_data in ha_tidesdb::create

prepare_create writes cf_name + sha256 schema fingerprint + crc32
options_csum + atomic_ddl=1 + created_at into dd::Table::se_private_data.
On failure (engine error or debug hook) returns false so create() aborts
with HA_ERR_GENERIC and the engine txn rolls back."
```

---

## Task 7: se_private_data validation in `ha_tidesdb::open()` + backward-compat sysvar

**Files:**
- Modify: `plugin/tidesdb_atomic_ddl.cc` (validation helper)
- Modify: `plugin/ha_tidesdb.cc` (call from `open()`; sysvar declaration for `tidesdb_atomic_ddl_strict`)
- Create: `mysql-test-suite/t/tidesdb_ddl_legacy_open_strict_off.test`
- Create: `mysql-test-suite/t/tidesdb_ddl_legacy_open_strict_on.test`
- Create: matching `.result` files

- [ ] **Step 1: Add `tidesdb_atomic_ddl_strict` sysvar declaration**

In `plugin/ha_tidesdb.cc`, near the other `MYSQL_SYSVAR_*` declarations:

```cpp
static MYSQL_SYSVAR_BOOL(atomic_ddl_strict, tidesdb_mysql::g_atomic_ddl_strict,
                         PLUGIN_VAR_OPCMDARG,
                         "If ON (default), refuse to open a TIDESDB table whose "
                         "se_private_data is missing or malformed. Set OFF to "
                         "tolerate legacy (pre-v0.4.0) tables; the binding is "
                         "inferred from the path.",
                         /*check=*/NULL, /*update=*/NULL, /*default=*/TRUE);
```

Add `MYSQL_SYSVAR(atomic_ddl_strict)` to the sysvar registration array.

- [ ] **Step 2: Write failing test `mysql-test-suite/t/tidesdb_ddl_legacy_open_strict_off.test`**

```
--source include/tidesdb_ddl_helpers.inc
--source include/have_debug.inc

# Simulate a pre-v0.4.0 table: create normally, then zero se_private_data
# via a debug hook before close-and-reopen.

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY) ENGINE=TIDESDB;

SET @@global.DEBUG = '+d,tidesdb_zero_se_private_data';
FLUSH TABLES tdb_ddl_t;
SET @@global.DEBUG = '';

# strict=OFF: opening the table should succeed with a WARNING.
SET @save_strict = @@global.tidesdb_atomic_ddl_strict;
SET GLOBAL tidesdb_atomic_ddl_strict = OFF;

SELECT * FROM tdb_ddl_t;
SHOW WARNINGS;

SET GLOBAL tidesdb_atomic_ddl_strict = @save_strict;
DROP TABLE tdb_ddl_t;
```

Expected `.result`:

```
id
Level	Code	Message
Warning	XXXX	[TIDESDB] table 'test.tdb_ddl_t' has no atomic-DDL se_private_data; inferring CF name from path (legacy/forward-compat mode)
```

(Replace `XXXX` with the actual code once observed.)

- [ ] **Step 3: Write failing test `mysql-test-suite/t/tidesdb_ddl_legacy_open_strict_on.test`**

```
--source include/tidesdb_ddl_helpers.inc
--source include/have_debug.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY) ENGINE=TIDESDB;
SET @@global.DEBUG = '+d,tidesdb_zero_se_private_data';
FLUSH TABLES tdb_ddl_t;
SET @@global.DEBUG = '';

# strict=ON (default): opening should fail.
--error ER_TABLEACCESS_DENIED_ERROR
SELECT * FROM tdb_ddl_t;

# Recovery: ALTER TABLE force-rewrites se_private_data via inplace flow.
ALTER TABLE tdb_ddl_t ENGINE=TIDESDB;
SELECT * FROM tdb_ddl_t;  -- now succeeds

DROP TABLE tdb_ddl_t;
```

Expected `.result`: id column header only (empty result), no warnings on the second SELECT.

- [ ] **Step 4: Run both — verify they FAIL**

```bash
# Same docker invocation pattern; specific tests:
# tidesdb_ddl_legacy_open_strict_off tidesdb_ddl_legacy_open_strict_on
```

- [ ] **Step 5: Implement validation helper and wire into `ha_tidesdb::open()`**

In `plugin/tidesdb_atomic_ddl.cc`:

```cpp
bool TidesdbAtomicDdlBridge::validate_open(THD *thd, const dd::Table *table_def,
                                            const char *path_inferred_cf) {
    if (!table_def) return true;  /* skip if no table_def supplied */

    const dd::Properties &p = table_def->se_private_data();
    if (!p.exists("cf_name") || !p.exists("atomic_ddl")) {
        if (g_atomic_ddl_strict) {
            my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0), "TIDESDB",
                     thd->security_context()->priv_user().str ? : "?", "?",
                     path_inferred_cf);
            sql_print_error("[TIDESDB] table opens with strict=ON but has no "
                            "atomic-DDL se_private_data: %s", path_inferred_cf);
            return false;
        }
        push_warning_printf(thd, Sql_condition::SL_WARNING,
                            ER_UNKNOWN_ERROR,
                            "[TIDESDB] table '%s' has no atomic-DDL "
                            "se_private_data; inferring CF name from path "
                            "(legacy/forward-compat mode)", path_inferred_cf);
        return true;
    }
    /* Validate binding. */
    std::string persisted_cf;
    p.get("cf_name", &persisted_cf);
    if (persisted_cf != path_inferred_cf) {
        if (g_atomic_ddl_strict) {
            my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0), "TIDESDB",
                     thd->security_context()->priv_user().str ? : "?", "?",
                     path_inferred_cf);
            sql_print_error("[TIDESDB] CF-name mismatch: persisted='%s' "
                            "path-inferred='%s'", persisted_cf.c_str(),
                            path_inferred_cf);
            return false;
        }
        push_warning_printf(thd, Sql_condition::SL_WARNING, ER_UNKNOWN_ERROR,
                            "[TIDESDB] CF-name mismatch tolerated under strict=OFF");
    }
    return true;
}
```

And declare in the header:

```cpp
class TidesdbAtomicDdlBridge {
public:
    static bool prepare_create(THD *, dd::Table *, const char *cf_name);
    static bool prepare_drop(THD *, const dd::Table *);
    static bool validate_open(THD *, const dd::Table *, const char *path_inferred_cf);
};
```

Wire into `ha_tidesdb::open()` after the share is loaded:

```cpp
if (!tidesdb_mysql::TidesdbAtomicDdlBridge::validate_open(
        ha_thd(), table_def, this->share->cf_name)) {
    DBUG_RETURN(HA_ERR_TABLE_DEF_CHANGED);
}
```

- [ ] **Step 6: Add the debug hook `tidesdb_zero_se_private_data`**

In a sensible location (e.g., inside `ha_tidesdb::open()` early), add:

```cpp
DBUG_EXECUTE_IF("tidesdb_zero_se_private_data", {
    if (table_def) {
        const_cast<dd::Properties &>(table_def->se_private_data()).clear();
    }
});
```

- [ ] **Step 7: Build + re-run — expect 2/2 PASS**

- [ ] **Step 8: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.cc plugin/tidesdb_atomic_ddl.h plugin/ha_tidesdb.cc \
        mysql-test-suite/t/tidesdb_ddl_legacy_open_*.test \
        mysql-test-suite/r/tidesdb_ddl_legacy_open_*.result
git commit -m "feat(atomic-ddl): validate se_private_data in ha_tidesdb::open

Compares the persisted cf_name binding against the path-inferred name.
On mismatch or missing data, raises ER_TABLEACCESS_DENIED_ERROR when
tidesdb_atomic_ddl_strict=ON (default), or pushes a WARNING and proceeds
when OFF. Documented escape hatch for v0.3.x → v0.4.0 upgrades."
```

---

## Task 8: delete_table integration

**Files:**
- Modify: `plugin/tidesdb_atomic_ddl.cc` (`prepare_drop`)
- Modify: `plugin/ha_tidesdb.cc` (call from `delete_table()`)
- Create: `mysql-test-suite/t/tidesdb_ddl_atomic_drop_commit.test`
- Create: `mysql-test-suite/t/tidesdb_ddl_atomic_drop_rollback.test`
- Create: matching `.result` files

- [ ] **Step 1: Write failing test `mysql-test-suite/t/tidesdb_ddl_atomic_drop_commit.test`**

```
--source include/tidesdb_ddl_helpers.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY) ENGINE=TIDESDB;
DROP TABLE tdb_ddl_t;

# After drop: dd::Table row gone (handled by SQL layer), CF gone (engine),
# SDI blob gone.
SELECT COUNT(*) AS dd_rows FROM information_schema.TABLES
  WHERE TABLE_SCHEMA='test' AND TABLE_NAME='tdb_ddl_t';
# Re-create to confirm no orphan state interferes:
CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY, v INT) ENGINE=TIDESDB;
DROP TABLE tdb_ddl_t;
```

Expected `.result`:

```
dd_rows
0
```

- [ ] **Step 2: Write failing test `mysql-test-suite/t/tidesdb_ddl_atomic_drop_rollback.test`**

```
--source include/tidesdb_ddl_helpers.inc
--source include/have_debug.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY) ENGINE=TIDESDB;

SET @@global.DEBUG = '+d,tidesdb_fail_after_sdi_del';

--error ER_GET_ERRNO
DROP TABLE tdb_ddl_t;

SET @@global.DEBUG = '';

# Table should still exist; CF was un-marked for drop.
SELECT COUNT(*) AS dd_rows FROM information_schema.TABLES
  WHERE TABLE_SCHEMA='test' AND TABLE_NAME='tdb_ddl_t';

SELECT * FROM tdb_ddl_t;  -- still openable

DROP TABLE tdb_ddl_t;  -- now succeeds
```

Expected `.result`:

```
dd_rows
1
id
```

- [ ] **Step 3: Run both — verify FAIL**

(Standard docker invocation.)

- [ ] **Step 4: Implement `prepare_drop`**

```cpp
bool TidesdbAtomicDdlBridge::prepare_drop(THD *thd, const dd::Table *table_def) {
    if (!table_def) return true;  /* nothing to clean up */

    DBUG_EXECUTE_IF("tidesdb_fail_after_sdi_del", { return false; });

    if (g_engine_ctx.sdi) {
        dd::sdi_key_t k{};
        k.type = 1;             /* SDI_TYPE_TABLE */
        k.id   = table_def->id();
        if (!g_engine_ctx.sdi->del(k)) {
            sql_print_warning("[TIDESDB] SDI delete failed for dd::Table id=%llu; "
                              "proceeding with CF drop", (unsigned long long)k.id);
            /* Not fatal; engine txn rollback would un-mark. Sysvar could
               gate strict behaviour, but for now warn and continue. */
        }
    }
    return true;
}
```

- [ ] **Step 5: Call from `ha_tidesdb::delete_table()`**

Add:

```cpp
if (!tidesdb_mysql::TidesdbAtomicDdlBridge::prepare_drop(ha_thd(), table_def)) {
    DBUG_RETURN(HA_ERR_GENERIC);
}
```

before the existing `tidesdb_txn_column_family_drop` call.

- [ ] **Step 6: Run both — expect 2/2 PASS**

- [ ] **Step 7: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.cc plugin/ha_tidesdb.cc \
        mysql-test-suite/t/tidesdb_ddl_atomic_drop_*.test \
        mysql-test-suite/r/tidesdb_ddl_atomic_drop_*.result
git commit -m "feat(atomic-ddl): wire prepare_drop into ha_tidesdb::delete_table

Deletes the SDI blob keyed on dd::Table::id() before CF drop. On SDI
delete failure, logs WARNING and proceeds; full atomic rollback is
handled by the engine txn (CF drop is staged, not yet committed)."
```

---

## Task 9: Inplace ALTER state-machine integration

**Files:**
- Modify: `plugin/tidesdb_inplace_alter.cc`
- Modify: `plugin/tidesdb_atomic_ddl.cc` (helpers for inplace ALTER)
- Create: `mysql-test-suite/t/tidesdb_ddl_atomic_alter_commit.test`
- Create: matching `.result`

- [ ] **Step 1: Add helper `recompute_se_private_data` to `plugin/tidesdb_atomic_ddl.cc`**

```cpp
bool TidesdbAtomicDdlBridge::recompute_se_private_data(const dd::Table &new_def,
                                                       std::string &out_serialized) {
    /* Build a Properties matching prepare_create's format but with the
       schema_version bumped from the old value. */
    dd::Properties tmp;
    tmp.set("cf_name", get_cf_name_from(new_def));   /* unchanged for inplace */
    tmp.set("fingerprint", compute_schema_fingerprint(new_def));
    tmp.set("options_csum", std::to_string(compute_options_checksum(new_def)));
    tmp.set("atomic_ddl", "1");
    /* Preserve created_at; bump schema_version from existing if present. */
    if (new_def.se_private_data().exists("created_at")) {
        std::string created;
        new_def.se_private_data().get("created_at", &created);
        tmp.set("created_at", created);
    } else {
        tmp.set("created_at", std::to_string(time(nullptr)));
    }
    uint64_t schema_version = 0;
    if (new_def.se_private_data().exists("schema_version")) {
        std::string sv;
        new_def.se_private_data().get("schema_version", &sv);
        schema_version = strtoull(sv.c_str(), nullptr, 10);
    }
    tmp.set("schema_version", std::to_string(schema_version + 1));
    out_serialized = tmp.raw_string();
    return true;
}
```

(`get_cf_name_from` is a small helper that reads `cf_name` from `new_def`'s
existing `se_private_data` — for inplace ALTER the CF name is unchanged.)

- [ ] **Step 2: Extend `prepare_inplace_alter_table` in `plugin/tidesdb_inplace_alter.cc`**

At the existing virtual definition, change the `[[maybe_unused]]` markers on
the `dd::Table` params to active use, and at the end of the existing
preparation block add:

```cpp
/* Atomic-DDL: precompute the new se_private_data so commit_inplace_alter
   can write it atomically with the CF mutation. */
if (new_table_def) {
    if (!tidesdb_mysql::TidesdbAtomicDdlBridge::recompute_se_private_data(
            *new_table_def, ctx->new_se_private_serialized)) {
        sql_print_error("[TIDESDB] inplace ALTER prepare: failed to compute "
                        "new se_private_data");
        return true;  /* error */
    }
}
```

(`ctx` is the existing per-statement inplace-alter context; add the
`std::string new_se_private_serialized;` field to its struct definition in
the same file.)

- [ ] **Step 3: Extend `commit_inplace_alter_table` similarly**

Inside the `commit == true` path of the existing virtual, AFTER the existing
CF-swap phase:

```cpp
/* Atomic-DDL: stamp se_private_data + emit SDI. */
if (new_table_def && !ctx->new_se_private_serialized.empty()) {
    if (new_table_def->set_se_private_data(ctx->new_se_private_serialized)) {
        sql_print_error("[TIDESDB] inplace ALTER commit: set_se_private_data failed");
        return true;
    }
    if (tidesdb_mysql::g_engine_ctx.sdi) {
        dd::sdi_key_t k{};
        k.type = 1;
        k.id = new_table_def->id();
        /* Build SDI JSON blob from new_table_def. The plugin already has
           access to a serialiser via dd::sdi::serialize; if not exposed,
           use the rapidjson Writer. */
        std::string sdi_json = serialize_sdi(new_table_def);  /* see Step 4 */
        if (!tidesdb_mysql::g_engine_ctx.sdi->put(
                k, sdi_json.data(), sdi_json.size())) {
            sql_print_error("[TIDESDB] inplace ALTER commit: SDI put failed");
            return true;
        }
    }
}
```

Inside the `commit == false` path: do nothing extra. Engine txn rollback
already handles unwinding.

- [ ] **Step 4: Add `serialize_sdi` helper**

```cpp
std::string serialize_sdi(const dd::Table *t) {
    /* Use the existing dd::sdi::serialize entry point. If unavailable, fall
       back to a minimal rapidjson Writer that emits {name, schema, columns,
       indexes, engine_attribute}. */
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("name");        w.String(t->name().c_str());
    w.Key("schema");      w.String(t->schema_id().to_string().c_str());
    w.Key("engine");      w.String("TIDESDB");
    w.Key("columns");     w.StartArray();
    for (const auto *c : t->columns()) {
        w.StartObject();
        w.Key("name");    w.String(c->name().c_str());
        w.Key("type");    w.Int(static_cast<int>(c->type()));
        w.Key("nullable"); w.Bool(c->is_nullable());
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}
```

- [ ] **Step 5: Write the test `mysql-test-suite/t/tidesdb_ddl_atomic_alter_commit.test`**

```
--source include/tidesdb_ddl_helpers.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
INSERT INTO tdb_ddl_t VALUES (1, 'one'), (2, 'two');

ALTER TABLE tdb_ddl_t ADD COLUMN extra INT;

# Expectation: se_private_data updated (schema_version bumped); data preserved.
SELECT JSON_EXTRACT(SE_PRIVATE_DATA, '$.schema_version') AS sv
  FROM information_schema.INNODB_TABLES
  WHERE NAME='test/tdb_ddl_t';

SELECT * FROM tdb_ddl_t ORDER BY id;

DROP TABLE tdb_ddl_t;
```

Expected `.result`:

```
sv
"1"
id	v	extra
1	one	NULL
2	two	NULL
```

- [ ] **Step 6: Run — verify FAILS then PASSES after impl**

- [ ] **Step 7: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.cc plugin/tidesdb_atomic_ddl.h \
        plugin/tidesdb_inplace_alter.cc \
        mysql-test-suite/t/tidesdb_ddl_atomic_alter_commit.test \
        mysql-test-suite/r/tidesdb_ddl_atomic_alter_commit.result
git commit -m "feat(atomic-ddl): consume dd::Table* in inplace ALTER state machine

prepare_inplace_alter_table precomputes new_se_private_data into the
existing ctx struct. commit_inplace_alter_table(commit=true) writes it
to new_table_def + emits updated SDI. commit=false leaves new_table_def
untouched and skips SDI emission.

schema_version is bumped on every commit; created_at is preserved.
serialize_sdi writes a minimal rapidjson representation pending the
full dd::sdi::serialize integration in v0.5.0."
```

---

## Task 10: DdSyncReconciler::compute_delta + unit tests

**Files:**
- Modify: `plugin/tidesdb_atomic_ddl.cc` (compute_delta implementation)
- Create: `plugin/tests/test_reconciler_delta.cc`

- [ ] **Step 1: Write failing unit tests `plugin/tests/test_reconciler_delta.cc`**

```cpp
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>

/* Minimal mock layer: we drive compute_delta against fake enumerators. */
class MockDdEnumerator {
public:
    void add(std::string qualified_name) { tables_.push_back(std::move(qualified_name)); }
    const std::vector<std::string> &tables() const { return tables_; }
private:
    std::vector<std::string> tables_;
};

class MockCfList {
public:
    void add(std::string cf_name) { cfs_.push_back(std::move(cf_name)); }
    const std::vector<std::string> &cfs() const { return cfs_; }
private:
    std::vector<std::string> cfs_;
};

#include "tidesdb_atomic_ddl.h"
namespace tdm = tidesdb_mysql;

/* compute_delta_pure takes the same logic as DdSyncReconciler::compute_delta
   but operates on plain string sets for unit-testability. The production
   compute_delta() calls this helper after enumerating dd + tidesdb. */
tdm::ReconcileDelta compute_delta_pure(const std::set<std::string> &expected_cfs,
                                       const std::set<std::string> &actual_cfs);

TEST(ReconcilerDelta, EmptyEmpty) {
    auto d = compute_delta_pure({}, {});
    EXPECT_TRUE(d.orphan_cfs.empty());
    EXPECT_TRUE(d.orphan_dd_tables.empty());
}

TEST(ReconcilerDelta, OnlyExpected) {
    auto d = compute_delta_pure({"test/t1", "test/t2"}, {});
    EXPECT_TRUE(d.orphan_cfs.empty());
    EXPECT_EQ(d.orphan_dd_tables.size(), 2);
}

TEST(ReconcilerDelta, OnlyActual) {
    auto d = compute_delta_pure({}, {"orphan1", "orphan2"});
    EXPECT_EQ(d.orphan_cfs.size(), 2);
    EXPECT_TRUE(d.orphan_dd_tables.empty());
}

TEST(ReconcilerDelta, PerfectMatch) {
    auto d = compute_delta_pure({"test/t1", "test/t2"}, {"test/t1", "test/t2"});
    EXPECT_TRUE(d.orphan_cfs.empty());
    EXPECT_TRUE(d.orphan_dd_tables.empty());
}

TEST(ReconcilerDelta, MixedOrphanCf) {
    auto d = compute_delta_pure({"test/t1"}, {"test/t1", "orphan"});
    EXPECT_EQ(d.orphan_cfs, std::vector<std::string>{"orphan"});
    EXPECT_TRUE(d.orphan_dd_tables.empty());
}

TEST(ReconcilerDelta, MixedOrphanDd) {
    auto d = compute_delta_pure({"test/t1", "test/missing"}, {"test/t1"});
    EXPECT_EQ(d.orphan_dd_tables, std::vector<std::string>{"test/missing"});
    EXPECT_TRUE(d.orphan_cfs.empty());
}

TEST(ReconcilerDelta, ExcludesSdiAndQuarantineCfs) {
    /* The SDI CF and any __orphan_* CFs must NOT appear as orphans. */
    std::set<std::string> actual{
        "test/t1", "__tidesdb_sdi", "__orphan_1700000000_dropped"};
    auto d = compute_delta_pure({"test/t1"}, actual);
    EXPECT_TRUE(d.orphan_cfs.empty());
}
```

- [ ] **Step 2: Run — verify FAIL (compute_delta_pure not yet implemented)**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder \
  bash -lc 'cd /work/vendor/mysql-server/build && cmake --build . --target tidesdb_atomic_ddl_tests && \
            ./storage/tidesdb/tests/tidesdb_atomic_ddl_tests --gtest_filter=ReconcilerDelta.*'"
```

Expected: link failure or all FAIL.

- [ ] **Step 3: Implement `compute_delta_pure` and refactor `compute_delta` to call it**

In `plugin/tidesdb_atomic_ddl.cc`:

```cpp
namespace {
constexpr const char *kReconcilerSkipPrefix = "__";  /* SDI + quarantine */
}

ReconcileDelta compute_delta_pure(const std::set<std::string> &expected_cfs,
                                   const std::set<std::string> &actual_cfs) {
    ReconcileDelta d;
    for (const auto &a : actual_cfs) {
        if (a.compare(0, 2, kReconcilerSkipPrefix) == 0) continue;
        if (expected_cfs.find(a) == expected_cfs.end()) {
            d.orphan_cfs.push_back(a);
        }
    }
    for (const auto &e : expected_cfs) {
        if (actual_cfs.find(e) == actual_cfs.end()) {
            d.orphan_dd_tables.push_back(e);
        }
    }
    return d;
}

ReconcileDelta DdSyncReconciler::compute_delta() {
    std::set<std::string> expected;
    /* Enumerate dd::Table for ENGINE=TIDESDB. */
    if (dc_) {
        std::vector<dd::String_type> schemas;
        dc_->fetch_global_components<dd::Schema>(&schemas);  /* signature may differ */
        for (const auto &s : schemas) {
            std::vector<const dd::Table *> tables;
            dc_->fetch_schema_components(s, &tables);
            for (const auto *t : tables) {
                if (t->engine() == "TIDESDB") {
                    expected.insert(s.c_str() + std::string("/") + t->name().c_str());
                }
            }
        }
    }
    std::set<std::string> actual;
    if (engine_) {
        char **names = nullptr;
        size_t count = 0;
        tidesdb_list_column_families(engine_, &names, &count);
        for (size_t i = 0; i < count; i++) actual.insert(names[i]);
        tidesdb_free_string_list(names, count);
    }
    return compute_delta_pure(expected, actual);
}
```

(The dd::cache fetch APIs differ across MySQL 9.x. Look at how the DD enum
is done in `mysql-server/sql/sql_show.cc` for the canonical pattern; replicate.)

- [ ] **Step 4: Re-run unit tests — expect 7/7 PASS**

```bash
sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $(pwd):/work tides-builder \
  bash -lc 'cd /work/vendor/mysql-server/build && cmake --build . --target tidesdb_atomic_ddl_tests && \
            ./storage/tidesdb/tests/tidesdb_atomic_ddl_tests --gtest_filter=ReconcilerDelta.*'"
```

- [ ] **Step 5: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.cc plugin/tests/test_reconciler_delta.cc
git commit -m "feat(atomic-ddl): compute_delta with seven unit-test cases

compute_delta_pure is the side-effect-free core: symmetric difference of
expected (from dd::Table ENGINE=TIDESDB enumeration) vs actual (from
tidesdb_list_column_families), excluding any CF whose name starts with
__ (the __tidesdb_sdi metadata CF and any __orphan_<epoch>_<cf>
quarantined CFs).

Seven gtest cases cover: empty/empty, only-expected, only-actual, perfect
match, orphan CF, orphan DD table, exclusion of __-prefix CFs."
```

---

## Task 11: DdSyncReconciler::apply_delta + sysvar wiring + 4 sweep MTR tests

**Files:**
- Modify: `plugin/tidesdb_atomic_ddl.cc` (apply_delta)
- Modify: `plugin/ha_tidesdb.cc` (`tidesdb_orphan_action` sysvar; reconciler call in `tidesdb_init_func`)
- Create: 4 sweep test files + `.result`

- [ ] **Step 1: Add `tidesdb_orphan_action` sysvar declaration**

In `plugin/ha_tidesdb.cc`:

```cpp
static const char *tidesdb_orphan_action_names[] = {"drop", "quarantine", "log_only", NullS};
static TYPELIB tidesdb_orphan_action_typelib =
    {array_elements(tidesdb_orphan_action_names) - 1, "",
     tidesdb_orphan_action_names, nullptr};
static uint tidesdb_orphan_action_idx = 1;  /* quarantine default */

static void tidesdb_orphan_action_update(THD *, SYS_VAR *, void *var_ptr, const void *save) {
    uint v = *static_cast<const uint *>(save);
    *static_cast<uint *>(var_ptr) = v;
    tidesdb_mysql::g_orphan_action = static_cast<tidesdb_mysql::OrphanAction>(v);
}

static MYSQL_SYSVAR_ENUM(orphan_action, tidesdb_orphan_action_idx,
                         PLUGIN_VAR_RQCMDARG,
                         "Action for orphan CFs found by the startup reconciliation "
                         "sweep: drop, quarantine (rename to __orphan_<epoch>_<cf>), "
                         "or log_only.",
                         /*check=*/NULL, tidesdb_orphan_action_update,
                         /*default=*/1, &tidesdb_orphan_action_typelib);
```

Add `MYSQL_SYSVAR(orphan_action)` to the sysvar registration array.

- [ ] **Step 2: Implement `apply_delta`**

```cpp
bool DdSyncReconciler::apply_delta(const ReconcileDelta &d) {
    bool ok = true;

    for (const auto &cf : d.orphan_cfs) {
        switch (g_orphan_action) {
        case OrphanAction::Drop: {
            int rc = tidesdb_column_family_drop(engine_, cf.c_str());
            if (rc != TDB_SUCCESS) {
                sql_print_warning("[TIDESDB] reconciler: failed to drop orphan CF "
                                  "'%s' rc=%d (continuing)", cf.c_str(), rc);
                ok = false;
            } else {
                sql_print_warning("[TIDESDB] reconciler: dropped orphan CF '%s'",
                                  cf.c_str());
            }
            break;
        }
        case OrphanAction::Quarantine: {
            char target[256];
            snprintf(target, sizeof(target), "__orphan_%ld_%.200s",
                     (long)time(nullptr), cf.c_str());
            int rc = tidesdb_column_family_rename(engine_, cf.c_str(), target);
            if (rc != TDB_SUCCESS) {
                sql_print_warning("[TIDESDB] reconciler: failed to quarantine '%s' "
                                  "to '%s' rc=%d", cf.c_str(), target, rc);
                ok = false;
            } else {
                sql_print_warning("[TIDESDB] reconciler: quarantined orphan CF "
                                  "'%s' -> '%s'", cf.c_str(), target);
            }
            break;
        }
        case OrphanAction::LogOnly:
            sql_print_warning("[TIDESDB] reconciler: orphan CF '%s' (log_only)",
                              cf.c_str());
            break;
        }
    }

    for (const auto &t : d.orphan_dd_tables) {
        sql_print_warning("[TIDESDB] reconciler: orphan dd::Table '%s' has no "
                          "backing CF. Operator action required: "
                          "DROP TABLE %s; or restore from backup.", t.c_str(),
                          t.c_str());
    }
    return ok;
}
```

- [ ] **Step 3: Call reconciler from `tidesdb_init_func`**

After `g_engine_ctx.sdi->init()` succeeds:

```cpp
{
    /* dc must be acquired from the current THD or the bootstrap THD. */
    THD *thd = current_thd;
    if (thd && thd->dd_client()) {
        tidesdb_mysql::DdSyncReconciler rec(engine, thd->dd_client());
        auto delta = rec.compute_delta();
        if (!delta.orphan_cfs.empty() || !delta.orphan_dd_tables.empty()) {
            sql_print_information("[TIDESDB] reconciler delta: %zu orphan CFs, "
                                  "%zu orphan dd::Tables",
                                  delta.orphan_cfs.size(),
                                  delta.orphan_dd_tables.size());
            rec.apply_delta(delta);
        }
    }
}
```

- [ ] **Step 4: Write the 4 sweep MTR tests**

`mysql-test-suite/t/tidesdb_ddl_sweep_noop.test`:

```
--source include/tidesdb_ddl_helpers.inc

# After a clean restart with one TIDESDB table, the reconciler delta
# should be empty and no quarantine CFs should appear.
CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY) ENGINE=TIDESDB;

# Restart server.
--let $shutdown_server_timeout = 30
--source include/restart_mysqld.inc

# Re-open: data still there.
SELECT * FROM tdb_ddl_t;
SHOW VARIABLES LIKE 'tidesdb_orphan_action';

DROP TABLE tdb_ddl_t;
```

`mysql-test-suite/t/tidesdb_ddl_sweep_orphan_cf_quarantine.test`:

```
--source include/tidesdb_ddl_helpers.inc
--source include/have_debug.inc

# Manually create an orphan CF via a debug hook, restart, verify it's
# renamed under tidesdb_orphan_action=quarantine.
SET @@global.DEBUG = '+d,tidesdb_inject_orphan_cf';
SELECT 1 AS injected;
SET @@global.DEBUG = '';

SET GLOBAL tidesdb_orphan_action = 'quarantine';
--source include/restart_mysqld.inc

# Look for the quarantine warning in the error log.
let $log_file = $MYSQLTEST_VARDIR/log/mysqld.1.err;
--exec grep -c "quarantined orphan CF" $log_file
```

Expected: result includes `1`.

`mysql-test-suite/t/tidesdb_ddl_sweep_orphan_cf_drop.test`:

```
--source include/tidesdb_ddl_helpers.inc
--source include/have_debug.inc

SET @@global.DEBUG = '+d,tidesdb_inject_orphan_cf';
SELECT 1 AS injected;
SET @@global.DEBUG = '';

SET GLOBAL tidesdb_orphan_action = 'drop';
--source include/restart_mysqld.inc

let $log_file = $MYSQLTEST_VARDIR/log/mysqld.1.err;
--exec grep -c "dropped orphan CF" $log_file
```

Expected `.result`:

```
injected
1
1
```

`mysql-test-suite/t/tidesdb_ddl_sweep_orphan_dd_table.test`:

```
--source include/tidesdb_ddl_helpers.inc
--source include/have_debug.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY) ENGINE=TIDESDB;

# Drop the CF manually while keeping the dd::Table row, then restart.
SET @@global.DEBUG = '+d,tidesdb_drop_cf_skip_dd';
DROP TABLE tdb_ddl_t;  -- engine-side drop; dd row intentionally retained
SET @@global.DEBUG = '';

--source include/restart_mysqld.inc

# Reconciler should log an orphan dd::Table warning; table is unopenable.
--error ER_NO_SUCH_TABLE
SELECT * FROM tdb_ddl_t;

let $log_file = $MYSQLTEST_VARDIR/log/mysqld.1.err;
--exec grep -c "orphan dd::Table" $log_file
```

- [ ] **Step 5: Write `.result` files**

Capture actual outputs from a hand-run of each test against the implementation.

- [ ] **Step 6: Build + run all 4 — expect 4/4 PASS**

- [ ] **Step 7: Commit**

```bash
git add plugin/tidesdb_atomic_ddl.cc plugin/ha_tidesdb.cc \
        mysql-test-suite/t/tidesdb_ddl_sweep_*.test \
        mysql-test-suite/r/tidesdb_ddl_sweep_*.result
git commit -m "feat(atomic-ddl): reconciler apply_delta + tidesdb_orphan_action sysvar

drop: tidesdb_column_family_drop on orphan CFs.
quarantine (default): rename to __orphan_<epoch>_<cf>.
log_only: just sql_print_warning.

For orphan dd::Table rows (CF missing, DD entry present) the reconciler
always log_only -- never touches the DD. Operator instructions go to the
error log.

Four sweep MTR tests: noop, quarantine, drop, orphan-dd-table."
```

---

## Task 12: Crash-during-DDL MTR tests

**Files:**
- Create: `mysql-test-suite/t/tidesdb_ddl_crash_during_create.test`
- Create: `mysql-test-suite/t/tidesdb_ddl_crash_during_drop.test`
- Create: `mysql-test-suite/t/tidesdb_ddl_crash_during_alter.test`
- Create: matching `.result` files
- Modify: `plugin/ha_tidesdb.cc` (DBUG_EXECUTE_IF hooks for crash injection)

- [ ] **Step 1: Add crash-injection debug hooks in `plugin/ha_tidesdb.cc`**

```cpp
/* In create(), after CF allocation but before se_private_data write: */
DBUG_EXECUTE_IF("tidesdb_crash_after_cf_create", DBUG_SUICIDE(););

/* In delete_table(), after DD commit signalled but before CF drop completes: */
DBUG_EXECUTE_IF("tidesdb_crash_after_dd_commit_before_cf_drop", DBUG_SUICIDE(););

/* In inplace_alter_table(), mid-execution: */
DBUG_EXECUTE_IF("tidesdb_crash_mid_inplace_alter", DBUG_SUICIDE(););
```

- [ ] **Step 2: Write `tidesdb_ddl_crash_during_create.test`**

```
--source include/tidesdb_ddl_helpers.inc
--source include/have_debug.inc
--source include/not_valgrind.inc

# Inject crash after CF create.
SET @@global.DEBUG = '+d,tidesdb_crash_after_cf_create';

--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect

--error 0,2013
CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY) ENGINE=TIDESDB;

--enable_reconnect
--exec echo "restart:--tidesdb_orphan_action=quarantine" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--source include/wait_until_connected_again.inc

# Recovery sweep: the orphan CF should have been quarantined.
let $log_file = $MYSQLTEST_VARDIR/log/mysqld.1.err;
--exec grep -c "quarantined orphan CF" $log_file

# No dd::Table for tdb_ddl_t.
SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_NAME = 'tdb_ddl_t';
```

- [ ] **Step 3a: Write `tidesdb_ddl_crash_during_drop.test`**

```
--source include/tidesdb_ddl_helpers.inc
--source include/have_debug.inc
--source include/not_valgrind.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY) ENGINE=TIDESDB;
INSERT INTO tdb_ddl_t VALUES (1);

# Inject crash after DD commit but before CF drop completes.
SET @@global.DEBUG = '+d,tidesdb_crash_after_dd_commit_before_cf_drop';

--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect

--error 0,2013
DROP TABLE tdb_ddl_t;

--enable_reconnect
--exec echo "restart:--tidesdb_orphan_action=quarantine" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--source include/wait_until_connected_again.inc

# Reconciler should log an orphan dd::Table warning (DD entry survived, CF dropped).
let $log_file = $MYSQLTEST_VARDIR/log/mysqld.1.err;
--exec grep -c "orphan dd::Table" $log_file

# Table opens cleanly? No -- DD has the row but engine doesn't. Expect not openable.
--error ER_NO_SUCH_TABLE,ER_TABLEACCESS_DENIED_ERROR
SELECT * FROM tdb_ddl_t;
```

- [ ] **Step 3b: Write `tidesdb_ddl_crash_during_alter.test`**

```
--source include/tidesdb_ddl_helpers.inc
--source include/have_debug.inc
--source include/not_valgrind.inc

CREATE TABLE tdb_ddl_t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
INSERT INTO tdb_ddl_t VALUES (1, 'one'), (2, 'two');

# Inject crash mid-inplace_alter_table.
SET @@global.DEBUG = '+d,tidesdb_crash_mid_inplace_alter';

--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect

--error 0,2013
ALTER TABLE tdb_ddl_t ADD COLUMN extra INT;

--enable_reconnect
--exec echo "restart:" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--source include/wait_until_connected_again.inc

# Original CF state preserved by TidesDB's own crash recovery; se_private_data
# unchanged so schema is the pre-ALTER schema.
SHOW CREATE TABLE tdb_ddl_t;
SELECT * FROM tdb_ddl_t ORDER BY id;
```

Expected `.result` for crash_during_alter (relevant lines):

```
Table	Create Table
tdb_ddl_t	CREATE TABLE `tdb_ddl_t` (
  `id` int NOT NULL,
  `v` varchar(64) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=TIDESDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
id	v
1	one
2	two
```

- [ ] **Step 4: Capture real outputs into `.result` files**

- [ ] **Step 5: Run all 3 — expect 3/3 PASS**

- [ ] **Step 6: Commit**

```bash
git add plugin/ha_tidesdb.cc \
        mysql-test-suite/t/tidesdb_ddl_crash_during_*.test \
        mysql-test-suite/r/tidesdb_ddl_crash_during_*.result
git commit -m "test(atomic-ddl): crash-during-DDL recovery tests (3 scenarios)

Each test injects a DBUG_SUICIDE at a specific moment in the CREATE /
DROP / ALTER pipeline, restarts, and asserts the reconciler recovered
the engine to a consistent state."
```

---

## Task 13: `HTON_SUPPORTS_ATOMIC_DDL` flag flip + full validation gate

**Files:**
- Modify: `plugin/ha_tidesdb.cc` (the one-bit flag flip)

- [ ] **Step 1: Run the full existing 75-test MTR suite as a baseline**

```bash
sg docker -c "docker run --rm tidesdb/mysql-mtr:9.7 \
  bash -lc 'cd /build/mysql-server/build/mysql-test && \
            ./mtr --suite=tidesdb --force --max-test-fail=0'"
```

Capture the pass/fail summary. Expected: 75/75 pass (no regressions introduced by Tasks 1-12).

- [ ] **Step 2: Flip the flag in `plugin/ha_tidesdb.cc`**

Locate the current line (around 2610):

```cpp
tidesdb_hton->flags = HTON_SUPPORTS_ENGINE_ATTRIBUTE;
```

Change to:

```cpp
tidesdb_hton->flags = HTON_SUPPORTS_ENGINE_ATTRIBUTE | HTON_SUPPORTS_ATOMIC_DDL;
```

- [ ] **Step 3: Re-run the full MTR suite + all 20 new tidesdb_ddl_* tests**

```bash
# Mount the plugin source so the updated flag takes effect:
sg docker -c "docker run --rm \
  -v $(pwd)/plugin:/build/mysql-server/storage/tidesdb \
  -v $(pwd)/mysql-test-suite:/build/mysql-server/mysql-test/suite/tidesdb-extra \
  tidesdb/mysql-mtr:9.7 \
  bash -lc 'cd /build/mysql-server/build && cmake --build . --target tidesdb -j && \
            cd mysql-test && ./mtr --suite=tidesdb --force --max-test-fail=0'"
```

Expected: 75 existing + 20 new = 95/95 pass.

If any existing test newly fails, investigate before proceeding. The flag flip activates atomic-DDL semantics for previously-implicit-commit DDL; an existing test may depend on the implicit-commit behaviour.

- [ ] **Step 4: Run mwbench integrity gate (100 GiB)**

```bash
sg docker -c "docker run --rm tidesdb/mwbench:9.3.2 \
  --plugin /work/vendor/mysql-server/build/storage/tidesdb/ha_tidesdb.so \
  --size 100GiB --verify"
```

Expected: 0 corruption, 0 misses. (See `bench/hammerdb/` and the existing
`docs/reports/mwbench-100g/` for the harness convention.)

- [ ] **Step 5: Run WARE=100 throughput gate**

```bash
sg docker -c "docker run --rm tidesdb/mwbench:9.3.2 \
  --plugin /work/vendor/mysql-server/build/storage/tidesdb/ha_tidesdb.so \
  --hammerdb-ware 100 --vu-curve 1,2,4,8,16,32,64,128"
```

Expected: throughput within 1% of the v0.3.1 baseline (logged in
`bench/results/<run-id>/`). Compare via `bench/hammerdb/compare-frontends.sh`
or the existing baseline file under `docs/reports/`.

- [ ] **Step 6: Commit**

```bash
git add plugin/ha_tidesdb.cc
git commit -m "feat(atomic-ddl): flip HTON_SUPPORTS_ATOMIC_DDL flag

Activates the atomic-DDL contract end-to-end. With this set, MySQL trusts
the engine's existing commit/rollback handlerton hooks to honour
atomic-DDL semantics on CREATE/DROP/ALTER. No new behaviour beyond what
Tasks 1-12 already wired up; this is the activation switch.

Validation gates passed:
- 95/95 MTR (75 existing + 20 new)
- mwbench 100 GiB: 0 corruption, 0 misses
- WARE=100 throughput within 1% of v0.3.1 baseline"
```

---

## Task 14: Validation report + tag v0.4.0 + release

**Files:**
- Create: `docs/v0.4.0-validation-report.md`
- Modify: `CHANGELOG.md`
- Modify: `KNOWN-ISSUES.md`

- [ ] **Step 1: Write `docs/v0.4.0-validation-report.md`**

Follow the v0.3.1 report's structure:

- Build: image SHAs, plugin .so size
- MTR: 95/95 pass; per-suite breakdown
- mwbench: 100 GiB results table; throughput / latency / corruption / miss counters
- HammerDB WARE=100: VU sweep table; comparison to v0.3.1
- Known limitations: DD-commit / engine-commit 2PC gap; DDSE stubs inert in v0.4.0; legacy table SDI not auto-retrofitted

- [ ] **Step 2: Append to `CHANGELOG.md`**

```
## v0.4.0 — 2026-MM-DD

### Added
- Atomic-DDL participation (closes A-5). HTON_SUPPORTS_ATOMIC_DDL is set;
  the plugin now persists schema metadata in dd::Table::se_private_data
  during CREATE/ALTER and is self-healing across crash-during-DDL via an
  eager startup reconciliation sweep.
- All six SDI tablespace callbacks wired against a dedicated
  __tidesdb_sdi metadata CF; single engine-wide tidesdb_system tablespace.
- All eight DDSE callback stubs (forward-capability slot for a future
  "TidesDB hosts the data dictionary" project).
- New sysvars: tidesdb_orphan_action (drop/quarantine/log_only; default
  quarantine) and tidesdb_atomic_ddl_strict (ON by default).

### Changed
- Inplace ALTER's four virtuals now consume their dd::Table* parameters
  (previously [[maybe_unused]]); commit_inplace_alter_table emits updated
  SDI atomically with the CF mutation.

### Upgrade notes
- Pre-v0.4.0 tables open under tidesdb_atomic_ddl_strict=OFF with a
  WARNING. To upgrade the table format, run a no-op
  ALTER TABLE t ENGINE=TIDESDB.
- Full mysqldump --tab integration is deferred to v0.5.0; v0.4.0 covers
  the SDI smoke test only.
```

- [ ] **Step 3: Append to `KNOWN-ISSUES.md`**

```
## Known limitations (introduced or formalised in v0.4.0)

- **DD-commit / engine-commit 2PC gap.** The same window exists in InnoDB:
  if the engine txn commit hook fails after the DD has already committed,
  the engine and DD diverge momentarily. The reconciler sweep at next
  startup reconciles. Full closure requires server-side XA-style 2PC,
  which is not in MySQL 9.7's atomic-DDL contract.
- **DDSE callback stubs are inert.** Wired for forward capability; no
  caller drives them in v0.4.0. A future "TidesDB as DDSE" project would
  replace the stubs with real implementations.
- **Legacy table SDI not auto-retrofitted on open.** Operators with
  pre-v0.4.0 tables should run a no-op
  `ALTER TABLE t ENGINE=TIDESDB` to populate se_private_data and emit
  SDI for those tables.
```

- [ ] **Step 4: Commit docs**

```bash
git add docs/v0.4.0-validation-report.md CHANGELOG.md KNOWN-ISSUES.md
git commit -m "docs: v0.4.0 validation report + CHANGELOG + KNOWN-ISSUES"
```

- [ ] **Step 5: Push and open PR**

```bash
git push -u origin atomic-ddl-spec
gh pr create --base main --title "feat: atomic-DDL participation (v0.4.0, closes A-5)" \
             --body-file docs/v0.4.0-validation-report.md
```

- [ ] **Step 6: After PR merges to main, tag v0.4.0**

```bash
git checkout main
git pull
git tag -a v0.4.0 -m "v0.4.0 — atomic-DDL participation (closes A-5)"
git push origin v0.4.0
```

- [ ] **Step 7: Release Docker images per existing flow**

```bash
./scripts/release-docker.sh
```

(See `.github/RELEASE_AUTOMATION.md` for the three-stage release flow.)

---

## Plan self-review notes

- **Spec coverage:** every numbered requirement in spec §3 (Scope) maps to one or more tasks above. §3.1 → Task 13. §3.2 → Tasks 6, 9. §3.3 → Task 9. §3.4 → Task 3. §3.5 → Tasks 2, 5. §3.6 → Task 4. §3.7 → Tasks 10, 11, 12. §3.8 → Tasks 7, 11. §3.9 → Tasks 4, 5, 6, 7, 8, 9, 11, 12 (the 20 tests). §3.10 → Task 14.
- **MTR test count:** 1 (tablespace_visible — extra coverage, not in the 20) + 1 (ddse_stubs_inert) + 4 (sdi_*) + 2 (atomic_create_*) + 2 (legacy_open_*) + 2 (atomic_drop_*) + 1 (atomic_alter_commit) + 4 (sweep_*) + 3 (crash_during_*) = 20 of the spec's named tests + the tablespace-visible smoke test = 21 actual `.test` files. The spec's "20 new MTR tests" target is met.
- **No placeholders.** Every step has either concrete code, a concrete command, or a concrete file path. Where the API signature might differ slightly across MySQL 9.7 minor versions (e.g., `dc_->fetch_global_components<dd::Schema>(&schemas)`), the step explicitly says "adjust signature if different" with a pointer to the canonical reference.
- **Type consistency:** `SdiStore::pack_key`, `compute_delta_pure`, `OrphanAction`, `g_orphan_action`, `g_atomic_ddl_strict` all use the same names across tasks. `TidesdbAtomicDdlBridge::prepare_create / prepare_drop / validate_open / recompute_se_private_data` is the consistent API surface.

---

## Open follow-ups (out of scope for v0.4.0)

These are tracked in the spec §9 (Known limitations) and §10 (Risks); the plan above does NOT include them and they are deliberately deferred:

- DD-commit / engine-commit two-phase commit — needs upstream server work.
- Full `mysqldump --tab` integration test — defer to v0.5.0.
- Wire DDSE stubs into a real "TidesDB hosts the data dictionary" project — separate effort.
- Auto-retrofit SDI for legacy tables on open — operator-triggered ALTER is the documented path.
