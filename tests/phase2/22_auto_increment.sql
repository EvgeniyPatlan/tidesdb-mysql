-- Phase 4 test 22: AUTO_INCREMENT semantics.
CREATE TABLE t1 (
    id   INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(32)
) ENGINE=TIDESDB;

-- Auto-generated PKs from 1
INSERT INTO t1 (name) VALUES ('a'), ('b'), ('c');
SELECT id, name FROM t1 ORDER BY id;

-- Explicit value above current — counter must advance past it
INSERT INTO t1 VALUES (100, 'jump');
SELECT id, name FROM t1 ORDER BY id;

-- Next auto-generated PK must be > 100
INSERT INTO t1 (name) VALUES ('after_jump');
SELECT id, name FROM t1 WHERE name = 'after_jump';

-- Counter survives a DELETE (no reuse)
DELETE FROM t1 WHERE name = 'after_jump';
INSERT INTO t1 (name) VALUES ('after_delete');
SELECT id, name FROM t1 WHERE name = 'after_delete';

-- LAST_INSERT_ID() works
SELECT LAST_INSERT_ID() = (SELECT id FROM t1 WHERE name = 'after_delete') AS last_id_correct;

DROP TABLE t1;
