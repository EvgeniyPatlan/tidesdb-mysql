-- Phase 2 test 06: multi-row INSERT + ordered SELECT.
-- Exercises rnd_init/rnd_next/rnd_end (table scan path).
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
INSERT INTO t1 VALUES (1,'a'),(2,'b'),(3,'c'),(4,'d'),(5,'e');
SELECT id, v FROM t1 ORDER BY id;
SELECT COUNT(*) AS cnt FROM t1;
DROP TABLE t1;
