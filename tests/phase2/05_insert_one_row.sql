-- Phase 2 test 05: single-row INSERT — exercises ha_tidesdb::write_row().
-- This is the first test that writes through TidesDB's actual storage path.
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
INSERT INTO t1 VALUES (1, 'hello');
SELECT id, v FROM t1;
DROP TABLE t1;
