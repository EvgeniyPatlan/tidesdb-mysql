-- @case: at_rest_encryption_roundtrip
-- @axis: encryption
CREATE TABLE t (id INT PRIMARY KEY, secret VARCHAR(64))
  ENGINE=TidesDB {{TABLE_OPTS:encrypted=1}};
INSERT INTO t VALUES (1,'classified'),(2,'topsecret');
SELECT id, secret FROM t ORDER BY id;
-- @expect:
-- 1	classified
-- 2	topsecret
-- @endexpect
