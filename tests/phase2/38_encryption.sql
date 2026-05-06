-- Phase 5 test 38: at-rest encryption sysvar exposed.
--
-- The hand-rolled harness can't bootstrap a 32-byte master key file +
-- restart mysqld with --tidesdb-master-key-file (that needs MTR's .opt
-- machinery). What we *can* check here is that the master_key_file
-- system variable is registered and visible at the SQL layer; the
-- end-to-end encryption round-trip is covered by the
-- mysql-test/suite/tidesdb/t/tidesdb_encryption MTR test.
SELECT VARIABLE_NAME
  FROM performance_schema.global_variables
  WHERE VARIABLE_NAME = 'tidesdb_master_key_file';
