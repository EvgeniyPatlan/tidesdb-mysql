-- Phase 2 test 07: SELECT by primary key.
-- Exercises index_read_map / index_init / index_end (PK access path).
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
INSERT INTO t1 VALUES (1,'a'),(2,'b'),(3,'c');
SELECT v FROM t1 WHERE id = 2;
SELECT v FROM t1 WHERE id = 99;
DROP TABLE t1;
