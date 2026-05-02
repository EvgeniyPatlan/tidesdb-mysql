-- Phase 2 test 14: large TEXT values across multiple rows.
CREATE TABLE t1 (id INT PRIMARY KEY, descr TEXT) ENGINE=TIDESDB;
INSERT INTO t1 VALUES
    (1, 'aaaaaaaaaa'),
    (2, REPEAT('abc', 30)),
    (3, REPEAT('xyz', 100));
SELECT id, LENGTH(descr) AS len, SUBSTRING(descr, 1, 12) AS head FROM t1 ORDER BY id;
DROP TABLE t1;
