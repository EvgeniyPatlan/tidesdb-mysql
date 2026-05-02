-- Phase 4 test 25: temporal types — DATE, TIME, DATETIME, TIMESTAMP, YEAR.
SET TIME_ZONE = '+00:00';
CREATE TABLE t1 (
    id INT PRIMARY KEY,
    d  DATE,
    t  TIME,
    dt DATETIME,
    ts TIMESTAMP NULL,
    y  YEAR
) ENGINE=TIDESDB;

INSERT INTO t1 VALUES
    (1, '2026-04-30', '12:34:56', '2026-04-30 12:34:56', '2026-04-30 12:34:56', 2026),
    (2, '1970-01-02', '00:00:01', '1970-01-02 00:00:01', '1970-01-02 00:00:01', 1970),
    -- TIMESTAMP range is 1970..2038 — store NULL for the 2099 row.
    (3, '2099-12-31', '23:59:59', '2099-12-31 23:59:59', NULL,                 2099);

SELECT id, d, t FROM t1 ORDER BY id;
SELECT id, dt, ts FROM t1 ORDER BY id;
SELECT id, y FROM t1 ORDER BY id;

-- Fractional seconds.
ALTER TABLE t1 ADD COLUMN dt6 DATETIME(6);
UPDATE t1 SET dt6 = '2026-04-30 12:34:56.123456' WHERE id = 1;
SELECT id, dt6 FROM t1 WHERE id = 1;

DROP TABLE t1;
