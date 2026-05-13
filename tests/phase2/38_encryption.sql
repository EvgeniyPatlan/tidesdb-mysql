-- Phase 5 test 38: at-rest encryption sysvar registration.
--
-- The hand-rolled harness can't bootstrap a 32-byte master key file +
-- restart mysqld with --tidesdb-master-key-file (that needs MTR's .opt
-- machinery). What we *can* check is:
--   (a) master_key_file remains queryable (kept visible by design --
--       see code comment in ha_tidesdb.cc; operators need to verify
--       encryption config and the path itself is not a credential)
--   (b) the H-5 fix: the actual S3 credentials are now hidden from
--       SHOW VARIABLES / performance_schema (PLUGIN_VAR_NOSYSVAR).
--
-- Encryption end-to-end is covered by mysql-test/suite/tidesdb/t/tidesdb_encryption.

SELECT VARIABLE_NAME
  FROM performance_schema.global_variables
  WHERE VARIABLE_NAME = 'tidesdb_master_key_file';

SELECT COUNT(*) AS leaked_credentials
  FROM performance_schema.global_variables
  WHERE VARIABLE_NAME IN (
    'tidesdb_s3_secret_key',
    'tidesdb_s3_access_key'
  );
