-- Phase 2 test 09: DELETE — exercises ha_tidesdb::delete_row.
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
INSERT INTO t1 VALUES (1,'a'),(2,'b'),(3,'c');
DELETE FROM t1 WHERE id = 2;
SELECT id, v FROM t1 ORDER BY id;
SELECT COUNT(*) AS remaining FROM t1;
DROP TABLE t1;
