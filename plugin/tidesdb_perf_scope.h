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
