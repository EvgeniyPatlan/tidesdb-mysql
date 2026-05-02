-- Phase 4 test 30: RENAME TABLE — exercises atomic CF rename.
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
INSERT INTO t1 VALUES (1, 'one'), (2, 'two'), (3, 'three');

-- Rename
RENAME TABLE t1 TO t2;

-- Old name gone
SHOW TABLES LIKE 't1';

-- New name has the data
SELECT id, v FROM t2 ORDER BY id;
SELECT COUNT(*) AS cnt FROM t2;

-- Rename back
RENAME TABLE t2 TO t1;
SELECT id, v FROM t1 ORDER BY id;

DROP TABLE t1;
