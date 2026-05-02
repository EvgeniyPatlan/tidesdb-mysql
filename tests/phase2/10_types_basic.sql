-- Phase 2 test 10: a few common column types in one table.
-- Exercises the row codec for INT, BIGINT, VARCHAR, TEXT.
-- (DECIMAL/DATE/DATETIME are deliberately deferred — likely Phase 3.)
CREATE TABLE t1 (
    id    INT PRIMARY KEY,
    big   BIGINT,
    name  VARCHAR(64),
    descr TEXT
) ENGINE=TIDESDB;

INSERT INTO t1 VALUES
    (1,  9000000000, 'alice', 'short bio'),
    (2, -9000000000, 'bob',   'longer bio with some more text');

SELECT id, big, name, descr FROM t1 ORDER BY id;
DROP TABLE t1;
