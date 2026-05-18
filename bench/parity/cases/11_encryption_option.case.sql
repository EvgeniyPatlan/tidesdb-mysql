-- @case: at_rest_encryption_roundtrip
-- @axis: encryption
-- @note: INCONCLUSIVE: both SUTs need a server keyring/master-key prerequisite absent in the base images; not an A-vs-B divergence.
CREATE TABLE t (id INT PRIMARY KEY, secret VARCHAR(64))
  ENGINE=TidesDB {{TABLE_OPTS:encrypted=1}};
INSERT INTO t VALUES (1,'classified'),(2,'topsecret');
SELECT id, secret FROM t ORDER BY id;
-- @expect:
-- 1	classified
-- 2	topsecret
-- @endexpect
