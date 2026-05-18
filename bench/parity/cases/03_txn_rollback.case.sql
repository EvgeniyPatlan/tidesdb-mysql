-- @case: txn_commit_rollback
-- @axis: transactions
CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TidesDB;
START TRANSACTION;
INSERT INTO t VALUES (1),(2);
COMMIT;
START TRANSACTION;
INSERT INTO t VALUES (3);
ROLLBACK;
SELECT id FROM t ORDER BY id;
-- @expect:
-- 1
-- 2
-- @endexpect
