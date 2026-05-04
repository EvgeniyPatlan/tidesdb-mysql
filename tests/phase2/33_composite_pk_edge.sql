-- Phase 5 test 33: composite PK edge cases.
-- Exercises:
--   * 3-column composite PK with mixed types
--   * AUTO_INCREMENT as the trailing column of a composite PK
--   * Composite PK with VARCHAR + DATE
--   * Prefix range scan (>= leading column)
--   * Full PK range scan with explicit ORDER BY
--   * Updating a row's PK columns (move PK)

-- ---------- 3-column INT/INT/INT ----------
CREATE TABLE t_3col (
    a INT NOT NULL,
    b INT NOT NULL,
    c INT NOT NULL,
    val VARCHAR(20),
    PRIMARY KEY (a, b, c)
) ENGINE=TIDESDB;

INSERT INTO t_3col VALUES
    (1, 1, 1, 'r111'),
    (1, 1, 2, 'r112'),
    (1, 2, 1, 'r121'),
    (2, 1, 1, 'r211'),
    (2, 2, 2, 'r222');

-- Full PK lookup
SELECT val FROM t_3col WHERE a = 1 AND b = 1 AND c = 2;
-- 2-column prefix
SELECT a, b, c, val FROM t_3col WHERE a = 1 AND b = 1 ORDER BY c;
-- 1-column prefix
SELECT a, b, c, val FROM t_3col WHERE a = 1 ORDER BY b, c;
-- Range on leading column
SELECT a, b, c FROM t_3col WHERE a >= 1 AND a <= 2 ORDER BY a, b, c;

DROP TABLE t_3col;

-- ---------- VARCHAR + DATE ----------
CREATE TABLE t_vd (
    region VARCHAR(8) NOT NULL,
    day DATE NOT NULL,
    qty INT,
    PRIMARY KEY (region, day)
) ENGINE=TIDESDB;

INSERT INTO t_vd VALUES
    ('us', '2026-01-01', 10),
    ('us', '2026-01-02', 11),
    ('eu', '2026-01-01', 20),
    ('eu', '2026-01-02', 21);

SELECT qty FROM t_vd WHERE region = 'us' AND day = '2026-01-02';
SELECT region, day, qty FROM t_vd WHERE region = 'eu' ORDER BY day;
-- ORDER BY full PK
SELECT region, day, qty FROM t_vd ORDER BY region, day;

DROP TABLE t_vd;

-- ---------- AUTO_INCREMENT trailing in composite PK ----------
CREATE TABLE t_ai (
    tenant INT NOT NULL,
    id INT NOT NULL AUTO_INCREMENT,
    label VARCHAR(20),
    PRIMARY KEY (tenant, id)
) ENGINE=TIDESDB;

INSERT INTO t_ai (tenant, label) VALUES (1, 'a'), (1, 'b'), (2, 'c');
SELECT tenant, id, label FROM t_ai ORDER BY tenant, id;

DROP TABLE t_ai;

-- ---------- PK column UPDATE moves the row ----------
CREATE TABLE t_move (
    a INT NOT NULL,
    b INT NOT NULL,
    val VARCHAR(20),
    PRIMARY KEY (a, b)
) ENGINE=TIDESDB;

INSERT INTO t_move VALUES (1, 1, 'one-one'), (1, 2, 'one-two'), (2, 1, 'two-one');
UPDATE t_move SET b = 5 WHERE a = 1 AND b = 1;
SELECT a, b, val FROM t_move ORDER BY a, b;
SELECT val FROM t_move WHERE a = 1 AND b = 5;
SELECT val FROM t_move WHERE a = 1 AND b = 1;

DROP TABLE t_move;
