-- Phase 4 test 26: CHAR (fixed) and BINARY/VARBINARY (binary charset) types.
CREATE TABLE t1 (
    id INT PRIMARY KEY,
    fixed_char  CHAR(10),
    fixed_bin   BINARY(8),
    var_bin     VARBINARY(64)
) ENGINE=TIDESDB;

INSERT INTO t1 VALUES
    (1, 'abc',     'abcd',         X'48656c6c6f20776f726c6421'),  -- "Hello world!"
    (2, 'longest!!', 'AAAABBBB',   X'00010203fffffefd'),
    (3, '',          '',           X'');

SELECT id, fixed_char, LENGTH(fixed_char) AS clen FROM t1 ORDER BY id;
SELECT id, HEX(fixed_bin) AS bhex, LENGTH(fixed_bin) AS blen FROM t1 ORDER BY id;
SELECT id, HEX(var_bin) AS vhex, LENGTH(var_bin) AS vlen FROM t1 ORDER BY id;

DROP TABLE t1;
