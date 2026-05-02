-- Phase 2 test 15: secondary index lookup.
-- Tests one-CF-per-index logic.
CREATE TABLE t1 (
    id   INT PRIMARY KEY,
    name VARCHAR(64),
    INDEX idx_name (name)
) ENGINE=TIDESDB;
INSERT INTO t1 VALUES (1,'alice'),(2,'bob'),(3,'carol');
SELECT id, name FROM t1 WHERE name = 'bob';
SELECT id, name FROM t1 WHERE name LIKE 'a%';
DROP TABLE t1;
