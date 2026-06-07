# Pure-function unit tests for the atomic-DDL surface. Linked against gtest
# directly (no MySQL test harness) so they run in seconds outside the MTR loop.
#
# We compile only tidesdb_atomic_ddl_pure.cc here — the server-free sibling
# of tidesdb_atomic_ddl.cc that holds the genuinely pure helpers
# (SdiStore::pack_key, compute_delta_pure). The production TU pulls in
# sql/dd, sql/log, my_dbug, g_engine_ctx, and the rest of the server
# surface, which a test binary cannot link without the full mysqld.
#
# Because the test sources include sql/handler.h transitively, we inherit
# the include path, compile flags, and compile definitions from the
# `tidesdb` plugin target (zlib, rapidjson, gnu++23, etc.) via generator
# expressions.

find_package(GTest)
if(NOT GTest_FOUND)
    message(STATUS "[TIDESDB] gtest not found; atomic-ddl unit tests skipped")
    return()
endif()

if(NOT TARGET tidesdb)
    message(STATUS "[TIDESDB] tidesdb plugin target not built; unit tests skipped")
    return()
endif()

add_executable(tidesdb_atomic_ddl_tests
    test_sdi_pack_key.cc
    test_reconciler_delta.cc
    test_server_stubs.cc
    test_perf_ring.cc
    ${CMAKE_SOURCE_DIR}/storage/tidesdb/tidesdb_atomic_ddl_pure.cc
    ${CMAKE_SOURCE_DIR}/storage/tidesdb/tidesdb_perf_ring.cc
)

# Inherit the plugin target's include paths and compile definitions. This is
# the minimum to make sql/handler.h compile in an isolated TU.
target_include_directories(tidesdb_atomic_ddl_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/storage/tidesdb
    $<TARGET_PROPERTY:tidesdb,INCLUDE_DIRECTORIES>
)
target_compile_definitions(tidesdb_atomic_ddl_tests PRIVATE
    $<TARGET_PROPERTY:tidesdb,COMPILE_DEFINITIONS>
)
# The plugin softens -Werror via -fpermissive; mirror it so the test TU
# compiles the same way the production TU does.
target_compile_options(tidesdb_atomic_ddl_tests PRIVATE -Wno-error -fpermissive)

# The pure TU has no TidesDB engine calls (no tidesdb_txn_*, no
# tidesdb_iter_*) so we don't need ${TIDESDB_LIBS}. Just gtest + pthread.
target_link_libraries(tidesdb_atomic_ddl_tests PRIVATE
    GTest::gtest GTest::gtest_main
    pthread
)

add_test(NAME tidesdb_atomic_ddl_tests COMMAND tidesdb_atomic_ddl_tests)
