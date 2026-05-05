-- Phase 5 test 36: ALGORITHM=INSTANT ADD COLUMN.
-- Verifies the deserialize_row default-fill correctly back-fills new
-- columns on existing rows, including the null-bitmap fix-up that was
-- missing previously (existing rows carried 1-bits in default_values'
-- "padding" slots that became the new column's null bit, making them
-- read as NULL).

CREATE TABLE t_iac (
    id INT PRIMARY KEY,
    v VARCHAR(32)
) ENGINE=TIDESDB;

INSERT INTO t_iac VALUES (1, 'a'), (2, 'b'), (3, 'c');

-- ADD COLUMN with NON-NULL DEFAULT, INSTANT.
ALTER TABLE t_iac ADD COLUMN extra INT DEFAULT 99, ALGORITHM=INSTANT;
SELECT id, v, extra FROM t_iac ORDER BY id;

-- ADD COLUMN with NOT NULL DEFAULT, INSTANT.
ALTER TABLE t_iac ADD COLUMN qty INT NOT NULL DEFAULT 7, ALGORITHM=INSTANT;
SELECT id, v, extra, qty FROM t_iac ORDER BY id;

-- ADD COLUMN nullable, no DEFAULT (i.e. defaults to NULL).
ALTER TABLE t_iac ADD COLUMN note VARCHAR(20), ALGORITHM=INSTANT;
SELECT id, v, extra, qty, note FROM t_iac ORDER BY id;

-- UPDATE a row's new columns; values must persist correctly.
UPDATE t_iac SET extra = 555, qty = 11, note = 'updated' WHERE id = 2;
SELECT id, v, extra, qty, note FROM t_iac ORDER BY id;

-- INSERT a fresh row with the new schema.
INSERT INTO t_iac VALUES (4, 'd', 1, 1, 'new');
SELECT id, v, extra, qty, note FROM t_iac ORDER BY id;

DROP TABLE t_iac;
