-- Phase 2 test 08: UPDATE — exercises ha_tidesdb::update_row.
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
INSERT INTO t1 VALUES (1,'a'),(2,'b'),(3,'c');
UPDATE t1 SET v = 'BBB' WHERE id = 2;
SELECT id, v FROM t1 ORDER BY id;
DROP TABLE t1;
