# Shared setup/teardown for tidesdb_ddl_* tests.
#
# Loads the engine via the standard suite have_tidesdb.inc (which also
# installs the [TIDESDB] suppression so plugin INFO/WARN noise does not
# fail the suite), then drops any leftover scratch table the SDI tests
# write into. Keeping the scratch name in one place lets us refactor it
# without touching every individual test.

--source suite/tidesdb/include/have_tidesdb.inc

--disable_query_log
--disable_warnings
DROP TABLE IF EXISTS test.tdb_ddl_t;
--enable_warnings
--enable_query_log
