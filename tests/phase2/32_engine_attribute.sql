-- Phase 4 test 32: ENGINE_ATTRIBUTE JSON parses into per-table options.
-- We can't directly read back the parsed options without exposing them,
-- so we verify two things: (1) the JSON is accepted (no parse error),
-- (2) tables with different attrs still INSERT/SELECT cleanly.

-- Default options
CREATE TABLE t_def (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
INSERT INTO t_def VALUES (1, 'default');

-- Explicit compression and bloom_filter
CREATE TABLE t_lz4 (id INT PRIMARY KEY, v VARCHAR(64))
    ENGINE=TIDESDB ENGINE_ATTRIBUTE='{"compression":"LZ4","bloom_filter":true}';
INSERT INTO t_lz4 VALUES (1, 'lz4');
SELECT id, v FROM t_lz4;

-- ZSTD compression with custom write buffer
CREATE TABLE t_zstd (id INT PRIMARY KEY, v VARCHAR(64))
    ENGINE=TIDESDB ENGINE_ATTRIBUTE='{"compression":"ZSTD","write_buffer_size":67108864}';
INSERT INTO t_zstd VALUES (1, 'zstd'), (2, 'zstd2');
SELECT id, v FROM t_zstd ORDER BY id;

-- Explicitly NONE compression
CREATE TABLE t_none (id INT PRIMARY KEY, v VARCHAR(64))
    ENGINE=TIDESDB ENGINE_ATTRIBUTE='{"compression":"NONE","bloom_filter":false}';
INSERT INTO t_none VALUES (1, 'plain');
SELECT id, v FROM t_none;

-- Verify the attribute is stored in the data dictionary
SELECT table_name, engine, create_options
  FROM information_schema.tables
 WHERE table_schema = 'test_phase2' AND table_name LIKE 't_%'
 ORDER BY table_name;

DROP TABLE t_def;
DROP TABLE t_lz4;
DROP TABLE t_zstd;
DROP TABLE t_none;
