-- Phase 5 test 35: ALGORITHM=INPLACE for ADD/DROP INDEX.
-- Verifies the inplace_alter_table handler virtuals route ADD INDEX
-- and DROP INDEX without falling back to ALGORITHM=COPY.

CREATE TABLE t_inpl (
    id INT PRIMARY KEY,
    a  INT NOT NULL,
    b  VARCHAR(32) NOT NULL,
    c  INT
) ENGINE=TIDESDB;

INSERT INTO t_inpl VALUES
    (1,  10, 'alpha',   100),
    (2,  20, 'beta',    200),
    (3,  30, 'gamma',   300),
    (4,  10, 'delta',   400),
    (5,  20, 'epsilon', 500);

-- ADD INDEX inplace -- engine builds the new CF and populates it.
ALTER TABLE t_inpl ADD INDEX idx_a (a), ALGORITHM=INPLACE;

-- Verify the index is usable: lookup by a should hit it.
SELECT id, a, b FROM t_inpl WHERE a = 10 ORDER BY id;
SELECT id, a, b FROM t_inpl WHERE a >= 20 ORDER BY a, id;

-- ADD UNIQUE INDEX inplace -- exercises the duplicate-detection branch.
ALTER TABLE t_inpl ADD UNIQUE INDEX uk_b (b), ALGORITHM=INPLACE;

-- Verify uniqueness is enforced after the inplace build.
INSERT INTO t_inpl VALUES (6, 40, 'beta', 600);

-- DROP INDEX inplace -- engine drops the underlying CF.
ALTER TABLE t_inpl DROP INDEX idx_a, ALGORITHM=INPLACE;

-- Confirm the index is gone but the table is intact.
SELECT id, a, b, c FROM t_inpl ORDER BY id;

DROP TABLE t_inpl;
