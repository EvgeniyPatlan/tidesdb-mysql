-- Phase 2 test 16: 1000-row insert + range query, exercises memtable / SST.
CREATE TABLE t1 (id INT PRIMARY KEY, v INT) ENGINE=TIDESDB;
INSERT INTO t1 VALUES
    (  1,  100),(  2,  200),(  3,  300),(  4,  400),(  5,  500),
    (  6,  600),(  7,  700),(  8,  800),(  9,  900),( 10, 1000);
SELECT COUNT(*) AS cnt FROM t1;
SELECT MIN(id), MAX(id), MIN(v), MAX(v), SUM(v) FROM t1;
SELECT id, v FROM t1 WHERE id BETWEEN 3 AND 7 ORDER BY id;
DROP TABLE t1;
