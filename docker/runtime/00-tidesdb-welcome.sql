-- TidesDB demo schema -- created once on first boot of the container.
-- Drop this whole file (or empty it out) if you don't want the sample data.
--
-- Verify the plugin is loaded:
--   SELECT * FROM information_schema.engines WHERE engine = 'TIDESDB';
--
-- Verify status variables:
--   SHOW GLOBAL STATUS LIKE 'tidesdb_%';

CREATE DATABASE IF NOT EXISTS tidesdb_demo;

USE tidesdb_demo;

-- Simple key/value table.
CREATE TABLE IF NOT EXISTS kv (
  k VARCHAR(64) PRIMARY KEY,
  v JSON
) ENGINE=TIDESDB;

INSERT INTO kv (k, v) VALUES
  ('hello',     JSON_OBJECT('lang', 'en', 'msg', 'world')),
  ('hola',      JSON_OBJECT('lang', 'es', 'msg', 'mundo')),
  ('bonjour',   JSON_OBJECT('lang', 'fr', 'msg', 'monde'))
ON DUPLICATE KEY UPDATE v = VALUES(v);

-- Time-series table demonstrating composite PK + per-table compression.
CREATE TABLE IF NOT EXISTS metrics (
  host    VARCHAR(64) NOT NULL,
  ts      DATETIME(3) NOT NULL,
  metric  VARCHAR(32) NOT NULL,
  value   DOUBLE      NOT NULL,
  PRIMARY KEY (host, metric, ts),
  KEY idx_metric_ts (metric, ts)
) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"compression":"ZSTD","bloom_filter":true}';

INSERT IGNORE INTO metrics VALUES
  ('host-01', NOW(3), 'cpu',  17.5),
  ('host-01', NOW(3), 'mem',  62.1),
  ('host-02', NOW(3), 'cpu',   5.0),
  ('host-02', NOW(3), 'mem',  44.8);
