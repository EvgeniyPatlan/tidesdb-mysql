-- @case: instant_add_column_backfill
-- @axis: online-ddl
CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TidesDB;
INSERT INTO t VALUES (1),(2);
ALTER TABLE t ADD COLUMN c INT NOT NULL DEFAULT 7, ALGORITHM=INSTANT;
INSERT INTO t (id) VALUES (3);
SELECT id, c FROM t ORDER BY id;
-- @expect:
-- 1	7
-- 2	7
-- 3	7
-- @endexpect
