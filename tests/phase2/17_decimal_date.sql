-- Phase 2 test 17: DECIMAL and DATE types.
CREATE TABLE t1 (id INT PRIMARY KEY, price DECIMAL(10,2), born DATE) ENGINE=TIDESDB;
INSERT INTO t1 VALUES
    (1, 99.99, '2000-01-15'),
    (2, 100.50, '1985-06-30'),
    (3,  0.01, '2026-12-31');
SELECT id, price, born FROM t1 ORDER BY id;
SELECT SUM(price) AS total FROM t1;
DROP TABLE t1;
