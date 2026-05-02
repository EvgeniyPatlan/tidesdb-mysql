-- Phase 4 test 20: composite (multi-column) PRIMARY KEY.
-- Exercises pk_from_record over multiple columns.
CREATE TABLE t1 (
    region VARCHAR(8),
    sku    INT,
    price  DECIMAL(8,2),
    PRIMARY KEY (region, sku)
) ENGINE=TIDESDB;

INSERT INTO t1 VALUES
    ('us', 1001, 9.99),
    ('us', 1002, 19.99),
    ('us', 1003, 29.99),
    ('eu', 1001,  8.50),
    ('eu', 1002, 17.00);

-- Composite PK lookup (full key)
SELECT price FROM t1 WHERE region = 'us' AND sku = 1002;
SELECT price FROM t1 WHERE region = 'eu' AND sku = 1001;

-- Prefix lookup (region only — should use leading PK column)
SELECT region, sku, price FROM t1 WHERE region = 'us' ORDER BY sku;
SELECT region, sku, price FROM t1 WHERE region = 'eu' ORDER BY sku;

-- Full table scan still works
SELECT region, sku, price FROM t1 ORDER BY region, sku;

-- Update via composite PK
UPDATE t1 SET price = 999.00 WHERE region = 'us' AND sku = 1001;
SELECT price FROM t1 WHERE region = 'us' AND sku = 1001;

-- Delete via composite PK
DELETE FROM t1 WHERE region = 'eu' AND sku = 1002;
SELECT COUNT(*) AS remaining FROM t1;

DROP TABLE t1;
