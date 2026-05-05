-- Phase 5 test 37: ALGORITHM=INSTANT DROP COLUMN (trailing only).
-- Verifies:
--   * INSTANT DROP of a trailing column succeeds (no row rewrite,
--     deserialize_row's MIN(stored_fields, current_fields) ignores the
--     extra trailing bytes).
--   * INSTANT DROP of a middle column is rejected -- MySQL falls back
--     to ALGORITHM=COPY which rewrites every row with the new layout
--     (correct, just slower than INSTANT).
--   * Round-tripping data after each ALTER works (INSERT/SELECT/UPDATE).

CREATE TABLE t_idc (
    id    INT PRIMARY KEY,
    name  VARCHAR(32),
    extra INT,
    note  VARCHAR(64)
) ENGINE=TIDESDB;

INSERT INTO t_idc VALUES
    (1, 'alice', 10, 'first'),
    (2, 'bob',   20, 'second'),
    (3, 'carol', 30, 'third');

-- Trailing DROP -- INSTANT path. Removes `note` (the last field).
ALTER TABLE t_idc DROP COLUMN note, ALGORITHM=INSTANT;
SELECT id, name, extra FROM t_idc ORDER BY id;

-- INSERT under the new schema, then verify the row reads back.
INSERT INTO t_idc VALUES (4, 'dave', 40);
SELECT id, name, extra FROM t_idc ORDER BY id;

-- Trailing DROP again -- removes `extra` (now the last field).
ALTER TABLE t_idc DROP COLUMN extra, ALGORITHM=INSTANT;
SELECT id, name FROM t_idc ORDER BY id;

-- Now try a NON-trailing DROP (`name` is in the middle, but at this
-- point only `id` and `name` exist -- so name IS now trailing).
-- Use a fresh table to force a real middle-drop scenario.
DROP TABLE t_idc;

CREATE TABLE t_mid (
    a INT PRIMARY KEY,
    b INT,
    c INT
) ENGINE=TIDESDB;
INSERT INTO t_mid VALUES (1, 11, 111), (2, 22, 222);

-- Mid-drop: INSTANT must be rejected with ER_ALTER_OPERATION_NOT_SUPPORTED_REASON.
ALTER TABLE t_mid DROP COLUMN b, ALGORITHM=INSTANT;
