-- Phase 5 test 34: composite UNIQUE secondary index (non-PK).
-- Verifies that UNIQUE KEY (a, b) is enforced even when the table has
-- AUTO_INCREMENT PK (which previously bypassed the secondary unique check).
CREATE TABLE t_uq (
    id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    region VARCHAR(8) NOT NULL,
    sku INT NOT NULL,
    label VARCHAR(20),
    UNIQUE KEY uk_region_sku (region, sku)
) ENGINE=TIDESDB;

INSERT INTO t_uq (region, sku, label) VALUES
    ('us', 1, 'a'),
    ('us', 2, 'b'),
    ('eu', 1, 'c');

SELECT region, sku, label FROM t_uq WHERE region = 'us' AND sku = 2;

-- This must fail with ER_DUP_ENTRY (1062) -- the row (us, 1) already exists.
-- With AUTO_INCREMENT PK + composite UNIQUE, the engine previously skipped
-- the secondary-unique check because PK uniqueness was implicit.
INSERT INTO t_uq (region, sku, label) VALUES ('us', 1, 'dup-attempt');
