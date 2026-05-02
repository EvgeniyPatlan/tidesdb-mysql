-- Phase 2 test 11: NULL values in nullable column.
-- Exercises null-bitmap encoding/decoding in the row codec.
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(32) NULL) ENGINE=TIDESDB;
INSERT INTO t1 VALUES (1,'a'),(2,NULL),(3,'c');
SELECT id, v, v IS NULL AS is_null FROM t1 ORDER BY id;
DROP TABLE t1;
