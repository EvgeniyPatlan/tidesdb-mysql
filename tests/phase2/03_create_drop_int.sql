-- Phase 2 test 03: CREATE TABLE with INT primary key, then DROP.
-- Exercises ha_tidesdb::create() and delete_table().
CREATE TABLE t1 (id INT PRIMARY KEY) ENGINE=TIDESDB;
SHOW CREATE TABLE t1;
DROP TABLE t1;
