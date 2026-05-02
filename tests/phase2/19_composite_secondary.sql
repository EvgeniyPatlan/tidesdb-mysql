-- Phase 4 test 19: composite secondary index (a, b).
-- Exercises multi-column key encoding for both index population and lookup.
CREATE TABLE t1 (
    id INT PRIMARY KEY,
    a  INT,
    b  VARCHAR(32),
    INDEX idx_ab (a, b)
) ENGINE=TIDESDB;

INSERT INTO t1 VALUES
    (1, 10, 'apple'),
    (2, 10, 'banana'),
    (3, 20, 'apple'),
    (4, 20, 'banana'),
    (5, 30, 'cherry');

-- Full key match (uses both columns of the index)
SELECT id FROM t1 WHERE a = 10 AND b = 'banana';
SELECT id FROM t1 WHERE a = 20 AND b = 'apple';
SELECT id FROM t1 WHERE a = 30 AND b = 'cherry';

-- Prefix match (only first column)
SELECT id, a, b FROM t1 WHERE a = 10 ORDER BY id;
SELECT id, a, b FROM t1 WHERE a = 20 ORDER BY id;

-- Range on first column
SELECT id, a, b FROM t1 WHERE a > 10 ORDER BY id;

DROP TABLE t1;
