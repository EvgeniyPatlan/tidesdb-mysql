-- @case: inplace_add_drop_index
-- @axis: online-ddl
CREATE TABLE t (id INT PRIMARY KEY, v INT) ENGINE=TidesDB;
INSERT INTO t VALUES (1,10),(2,20),(3,30);
ALTER TABLE t ADD INDEX idx_v (v), ALGORITHM=INPLACE;
SELECT id FROM t WHERE v=20;
ALTER TABLE t DROP INDEX idx_v, ALGORITHM=INPLACE;
SELECT id FROM t ORDER BY id;
-- @expect:
-- 2
-- 1
-- 2
-- 3
-- @endexpect
