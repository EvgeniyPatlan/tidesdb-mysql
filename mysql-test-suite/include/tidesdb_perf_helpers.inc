# Shared setup for tidesdb_perf_* tests.

--source suite/tidesdb/include/have_tidesdb.inc

# These tests need TIDESDB_PERF=1 in the build. The macros are no-ops
# otherwise, so each test will mark itself skipped if perf_capture isn't
# in SHOW VARIABLES.
let $perf_present = `SELECT COUNT(*) FROM performance_schema.session_variables
                      WHERE VARIABLE_NAME='tidesdb_perf_capture'`;
if (!$perf_present) {
  --skip Test requires plugin built with -DTIDESDB_PERF=1
}

# Default OFF and clean output dir for every test.
SET @save_capture = @@global.tidesdb_perf_capture;
SET GLOBAL tidesdb_perf_capture = OFF;
