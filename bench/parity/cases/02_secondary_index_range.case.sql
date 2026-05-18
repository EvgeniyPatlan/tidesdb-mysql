-- @case: secondary_index_range
-- @axis: indexes
CREATE TABLE t (id INT PRIMARY KEY, price DECIMAL(10,2), KEY idx_price (price)) ENGINE=TidesDB;
INSERT INTO t VALUES (1,9.99),(2,24.50),(3,4.25),(4,15.00);
SELECT id FROM t WHERE price BETWEEN 5 AND 20 ORDER BY price;
-- @expect:
-- 1
-- 4
-- @endexpect
