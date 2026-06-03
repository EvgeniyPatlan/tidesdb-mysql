# Pure-function unit tests for the atomic-DDL surface. Linked against gtest
# directly (no MySQL test harness) so they run in seconds outside the MTR loop.
#
# We re-compile tidesdb_atomic_ddl.cc here rather than linking ha_tidesdb.so
# so the helper tests don't drag in the whole plugin. Because the TU includes
# sql/handler.h transitively, we also inherit the include path, compile flags,
# and compile definitions from the `tidesdb` plugin target (zlib, rapidjson,
# etc.) via generator expressions.
#
# We also link the TidesDB library so the SdiStore methods (which now call
# tidesdb_txn_* / tidesdb_iter_*) resolve, plus a tiny test_server_stubs.cc
# that provides a no-op log_message() in place of the mysqld symbol.

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
    test_server_stubs.cc
    ${CMAKE_SOURCE_DIR}/storage/tidesdb/tidesdb_atomic_ddl.cc
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

# Pull in the TidesDB library for tidesdb_txn_*/tidesdb_iter_* symbols. The
# pack_key test does not need them, but the TU it lives in references them.
# libtidesdb.a depends on zstd/lz4/snappy + pthread; link the same set of
# libs as the plugin itself so the test binary resolves cleanly.
target_link_libraries(tidesdb_atomic_ddl_tests PRIVATE
    GTest::gtest GTest::gtest_main
    ${TIDESDB_LIBS}
    pthread
)

add_test(NAME tidesdb_atomic_ddl_tests COMMAND tidesdb_atomic_ddl_tests)
