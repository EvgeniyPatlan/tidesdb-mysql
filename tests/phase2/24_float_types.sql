-- Phase 4 test 24: floating-point types.
CREATE TABLE t1 (
    id INT PRIMARY KEY,
    f4 FLOAT,
    f8 DOUBLE,
    d10_4 DECIMAL(10,4)
) ENGINE=TIDESDB;

INSERT INTO t1 VALUES
    (1, 3.14, 2.7182818284590452, 12345.6789),
    (2, -0.5, -1e100, -99.9999),
    (3,  0.0,  0.0,   0.0000);

SELECT id, ROUND(f4, 2) AS f4, f8, d10_4 FROM t1 ORDER BY id;
SELECT SUM(d10_4) AS sum_dec FROM t1;
DROP TABLE t1;
