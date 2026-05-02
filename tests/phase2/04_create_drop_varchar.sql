-- Phase 2 test 04: table with VARCHAR — exercises Field_varstring path.
CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(64)) ENGINE=TIDESDB;
SHOW CREATE TABLE t1;
DROP TABLE t1;
