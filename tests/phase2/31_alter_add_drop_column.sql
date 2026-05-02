-- Phase 4 test 31: ALTER TABLE ADD/DROP COLUMN.
CREATE TABLE t1 (id INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
INSERT INTO t1 VALUES (1, 'a'), (2, 'b');

-- Add a column with default
ALTER TABLE t1 ADD COLUMN extra INT DEFAULT 99;
SELECT id, v, extra FROM t1 ORDER BY id;

-- Update some rows in the new column
UPDATE t1 SET extra = 7 WHERE id = 1;
SELECT id, v, extra FROM t1 ORDER BY id;

-- Drop the original column
ALTER TABLE t1 DROP COLUMN v;
SELECT id, extra FROM t1 ORDER BY id;

DROP TABLE t1;
