-- @case: table_options_compression_bloom
-- @axis: options
-- Exercises the per-server option shim (ENGINE_ATTRIBUTE JSON vs MariaDB
-- option grammar). Data must round-trip regardless of dialect.
CREATE TABLE t (id INT PRIMARY KEY, s VARCHAR(64))
  ENGINE=TidesDB {{TABLE_OPTS:compression=LZ4,bloom_filter=1}};
INSERT INTO t VALUES (1,'hello'),(2,'world');
SELECT id, s FROM t ORDER BY id;
-- @expect:
-- 1	hello
-- 2	world
-- @endexpect
