-- Phase 2 test 18: data should persist within a single mysqld run.
-- (Cross-restart persistence requires the runner to bounce mysqld; not yet.)
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
INSERT INTO t1 VALUES (1,'first'),(2,'second');
-- (no explicit COMMIT — autocommit on by default)
SELECT COUNT(*) AS rows_after_insert FROM t1;

-- Try OPTIMIZE TABLE (should at least not crash even if it's a no-op).
OPTIMIZE TABLE t1;

-- Confirm rows survived OPTIMIZE.
SELECT id, v FROM t1 ORDER BY id;
DROP TABLE t1;
