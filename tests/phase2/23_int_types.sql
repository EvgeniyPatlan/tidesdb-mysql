-- Phase 4 test 23: full integer type matrix.
CREATE TABLE t1 (
    id INT PRIMARY KEY,
    ti  TINYINT,                 -- -128..127
    tu  TINYINT UNSIGNED,        -- 0..255
    si  SMALLINT,                -- -32768..32767
    su  SMALLINT UNSIGNED,
    mi  MEDIUMINT,
    mu  MEDIUMINT UNSIGNED,
    ii  INT,
    iu  INT UNSIGNED,
    bi  BIGINT,
    bu  BIGINT UNSIGNED
) ENGINE=TIDESDB;

-- min, max, zero across signed/unsigned
INSERT INTO t1 VALUES
    (1, -128,    0, -32768,     0, -8388608,        0, -2147483648,          0, -9223372036854775808,                    0),
    (2,  127,  255,  32767, 65535,  8388607, 16777215,  2147483647, 4294967295,  9223372036854775807, 18446744073709551615),
    (3,    0,    0,      0,     0,        0,        0,           0,          0,                    0,                    0);

SELECT id, ti, tu, si, su FROM t1 ORDER BY id;
SELECT id, mi, mu, ii, iu FROM t1 ORDER BY id;
SELECT id, bi, bu FROM t1 ORDER BY id;

DROP TABLE t1;
