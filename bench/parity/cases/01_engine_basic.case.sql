-- @case: engine_basic_crud
-- @axis: core
CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(32)) ENGINE=TidesDB;
INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c');
UPDATE t SET name='B' WHERE id=2;
DELETE FROM t WHERE id=3;
SELECT id, name FROM t ORDER BY id;
-- @expect:
-- 1	a
-- 2	B
-- @endexpect
