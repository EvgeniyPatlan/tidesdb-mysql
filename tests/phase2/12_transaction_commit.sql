-- Phase 2 test 12: BEGIN/COMMIT round-trip.
-- Exercises transaction registration with hton->commit.
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
START TRANSACTION;
INSERT INTO t1 VALUES (1,'a'),(2,'b');
COMMIT;
SELECT id, v FROM t1 ORDER BY id;
DROP TABLE t1;
