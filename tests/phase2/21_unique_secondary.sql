-- Phase 4 test 21: UNIQUE secondary index — duplicate detection.
CREATE TABLE t1 (
    id    INT PRIMARY KEY,
    email VARCHAR(64),
    UNIQUE KEY uk_email (email)
) ENGINE=TIDESDB;

INSERT INTO t1 VALUES (1, 'alice@x'), (2, 'bob@x');

-- Should fail: duplicate email
INSERT INTO t1 VALUES (3, 'alice@x');

-- Should succeed: different email
INSERT INTO t1 VALUES (4, 'carol@x');

-- Verify final state
SELECT id, email FROM t1 ORDER BY id;

-- Lookup via the unique index
SELECT id FROM t1 WHERE email = 'bob@x';

DROP TABLE t1;
