# tidesdb-mysql

A **MySQL 9.7 storage engine plugin** that uses [TidesDB](https://github.com/tidesdb/tidesdb) — an LSM-tree key/value engine — as the row store. Builds as a loadable `ha_tidesdb.so`; install at runtime with `INSTALL PLUGIN`.

> **Status: experimental / proof-of-concept.** Single-node CRUD works, type coverage is wide, and the lifted MTR suite is **fully green — 47/47 executed tests pass with 5 cleanly skipped** (each marked with the specific feature gap that prevents it: native partitioning, inplace ALTER, MySQL keyring, MySQL vector API, libtidesdb OCC granularity). See [Status](#status) for the full breakdown.

This is a port of [TideSQL](https://github.com/tidesdb/tidesql) (TidesDB + MariaDB) to MySQL 9.7. Despite the family resemblance, MariaDB and MySQL have diverged enough at the handler API and data-dictionary layers that this is a rewrite-by-replay rather than a drop-in.

## Quick start (Docker — recommended for trying it out)

The fastest way to play with it: build a runnable `mysql:9.7` image with the plugin baked in.

```bash
git clone https://github.com/EvgeniyPatlan/tidesdb-mysql.git
cd tidesdb-mysql

# Build the image (cold: ~25-40 min — clones MySQL source, compiles the plugin
# on Oracle Linux 9 to match the official mysql:9.7 image's libstdc++ ABI).
docker build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7 .

# Run.
docker run -d --name tidesdb \
  -e MYSQL_ROOT_PASSWORD=secret \
  -p 3306:3306 \
  tidesdb/mysql:9.7

# Verify the engine is available.
docker exec tidesdb mysql -uroot -psecret -e "SHOW ENGINES;" | grep -i tidesdb
# Expected: TIDESDB  YES  TidesDB ...

# Connect and try the demo schema.
docker exec -it tidesdb mysql -uroot -psecret tidesdb_demo
# mysql> SELECT * FROM kv;
# mysql> SELECT * FROM metrics ORDER BY ts DESC LIMIT 5;
```

`docker compose -f docker/runtime/docker-compose.yml up -d` works too if you'd rather use compose.

## Building from source

If you want to develop the plugin (run tests, edit code, rebuild fast):

```bash
./scripts/build-all.sh        # ~30 min cold (clones MySQL, builds plugin)
./scripts/test-plugin.sh      # 33/33 hand-rolled tests
```

`build-all.sh` runs three steps:

1. **`setup-workspace.sh`** — clones `mysql-server@mysql-9.7.0` and `tidesdb@v9.2.0` into `vendor/`, then installs `plugin/` and `mysql-test-suite/` into the MySQL tree.
2. **`build-tidesdb.sh`** — builds `libtidesdb.a` (Debug, with symbols) into `vendor/tidesdb-prefix/`.
3. **`build-plugin.sh`** — configures MySQL with `WITH_TIDESDB_STORAGE_ENGINE=DYNAMIC`, builds `ha_tidesdb.so`.

Output: `vendor/mysql-server/build/plugin_output_directory/ha_tidesdb.so`.

## Loading the plugin

```sql
-- One-time, after copying ha_tidesdb.so to mysqld's plugin_dir:
INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so';

-- Use it:
CREATE TABLE t (id INT PRIMARY KEY, val VARCHAR(64)) ENGINE=TIDESDB;
INSERT INTO t VALUES (1, 'hello'), (2, 'world');
SELECT * FROM t WHERE id = 1;
```

Per-table options are passed through `ENGINE_ATTRIBUTE` (JSON):

```sql
CREATE TABLE t (id INT PRIMARY KEY, v BLOB) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"compression":"LZ4","bloom_filter":true}';
```

Server-level system variables: `tidesdb_flush_threads`, `tidesdb_compaction_threads`, `tidesdb_block_cache_size`, `tidesdb_max_open_sstables`, `tidesdb_unified_memtable_write_buffer_size`, `tidesdb_unified_memtable_sync_mode`, `tidesdb_default_write_buffer_size`, `tidesdb_default_sync_mode`, `tidesdb_default_compression`, `tidesdb_log_level`. See [`docs/build-and-load.md`](docs/build-and-load.md) for the full reference.

## Status

| Test layer | Result |
|---|---|
| `test-plugin.sh` (hand-rolled CRUD, 35 cases)        | **35/35 ✓** |
| `test-persistence.sh` (cross-restart durability)     | **PASS** |
| `smoke-test.sh` (end-to-end pipeline)                | **all phases ✓** |
| MTR suite lifted from TideSQL (52 tests)             | **48/48 executed pass, 4 skipped** |

What works:

- Types: `INT`, `BIGINT`, `SMALLINT`, `TINYINT`, `VARCHAR`, `CHAR`, `TEXT`, `BLOB`, `DATE`, `DATETIME`, `TIMESTAMP`, `DECIMAL`, `FLOAT`, `DOUBLE`, `ENUM`, `JSON`
- **Primary keys: single-column AND composite** (mixed types, including `AUTO_INCREMENT` as a non-leading column — `PRIMARY KEY (tenant, id)` with `id AUTO_INC` works)
- `INSERT`, `SELECT … WHERE pk = ?`, `UPDATE`, `DELETE`, full table scans, range scans (PK + secondary)
- `AUTO_INCREMENT`, `TRUNCATE TABLE`, `REPLACE INTO`, `INSERT … ON DUPLICATE KEY UPDATE`
- **Secondary indexes (single-column AND composite)** with ICP; `UNIQUE KEY (a, b)` enforced even on tables with `AUTO_INCREMENT` PK
- **Online DDL: `ALTER TABLE … ADD/DROP INDEX, ALGORITHM=INPLACE`** (concurrent reads OK; writes blocked while index populates)
- **`ALGORITHM=INSTANT ADD COLUMN`** with NULL or NOT NULL DEFAULT — existing rows back-fill from `default_values` on read, no row rewrite
- 2-phase commit via `prepare` hook → `ER_LOCK_DEADLOCK` surfaces cleanly to user code
- Cross-restart persistence
- `ALTER TABLE x ENGINE=TIDESDB` (engine conversion from InnoDB)
- Per-table compression (`NONE | SNAPPY | LZ4 | ZSTD | LZ4_FAST`) via `ENGINE_ATTRIBUTE`
- Bloom filters via `ENGINE_ATTRIBUTE`
- Per-row TTL, server-level TidesDB tuning system variables, online backup / checkpoint
- Mixed-engine transactions (TidesDB + InnoDB in the same `BEGIN…COMMIT`)
- Full-text search, spatial / R-tree indexes, generated columns

Skipped MTR tests (specific feature gaps, each documented inline):

- **`tidesdb_partition`** — native partitioning (HA_HAS_OWN_PARTITIONING + helper virtuals) not implemented; MySQL 8+ removed the legacy `ha_partition` shim.
- **`tidesdb_encryption`** — needs MySQL keyring (`keyring_file` / `keyring_okv`) bootstrap and `ENCRYPTED=` grammar port.
- **`tidesdb_vector`** — uses MariaDB's `VECTOR(N)` column type / `VECTOR INDEX` DDL; needs rewrite against MySQL 9 vector functions.
- **`tidesdb_pessimistic_insert_lock`** — TidesDB OCC false-positives on cross-row writes when prepare-phase commit surfaces conflicts (was hidden by old "silent commit on conflict" path); needs libtidesdb fix.

Not yet, but in-scope for follow-up work:

- **`ALGORITHM=INSTANT DROP COLUMN`** — needs per-row schema versioning (à la InnoDB's INSTANT DROP) to remember which positions were dropped; without it, rows written under the old schema can't be unpacked under the new layout. `ALGORITHM=COPY` rewrites every row correctly and is the fallback.
- **Atomic DDL via SDI** — registered but not exercised; restart-safe DDL is tracked in [phase4-atomic-ddl-investigation.md](docs/phase4-atomic-ddl-investigation.md).
- **Replication, FK constraints, savepoint nesting** — not in scope yet.

## Layout

```
plugin/                MySQL handler source (ha_tidesdb.{cc,h}, tidesdb_compat.h, CMakeLists.txt)
mysql-test-suite/      MTR suite — 61 tests, lifted from TideSQL and rebased onto ENGINE_ATTRIBUTE
tests/phase2/          Hand-rolled .sql / .expected pairs run by scripts/test-plugin.sh
scripts/               Build, run, and replay-port-edit scripts (Bash)
docker/                Builder image (Ubuntu 24.04 + MySQL/TidesDB build deps)
docs/                  Design notes, port log, phase status
vendor/                Cloned upstreams (mysql-server, tidesdb, optional tidesql) — gitignored
```

`vendor/` is populated by `scripts/setup-workspace.sh`. Nothing inside it is checked into this repo.

## How the port was done

`scripts/replay-port-edits.sh` is a deterministic, idempotent script that takes the upstream TideSQL MariaDB source and applies every MariaDB→MySQL edit needed for MySQL 9.7. It's there both as documentation (each edit shows the diff in pattern→replacement form) and as a re-baseline tool when TideSQL upstream changes. Run with `WITH_TIDESQL_REFERENCE=1 ./scripts/setup-workspace.sh` to clone TideSQL alongside, then `./scripts/replay-port-edits.sh` to regenerate `plugin/ha_tidesdb.cc`.

The big port hot-spots:

- `Field::sort_string()` (MariaDB) → `Field::make_sort_key()` (MySQL) — load-bearing for primary-key encoding.
- Handler virtuals `open()`, `create()`, `delete_table()`, `rename_table()` all gain a trailing `dd::Table*` parameter in MySQL.
- Per-table options grammar (MariaDB `ha_create_table_option[]`) → JSON `ENGINE_ATTRIBUTE` parsed with rapidjson.
- `HTON_SUPPORTS_ENGINE_ATTRIBUTE`, `HA_GENERATED_COLUMNS`, atomic-DDL flags must be set explicitly.
- `INSTALL SONAME 'ha_tidesdb';` (MariaDB) → `INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so';` (MySQL).

See [`docs/mariadb-vs-mysql.md`](docs/mariadb-vs-mysql.md) and [`docs/port-errors-pass1.md`](docs/port-errors-pass1.md) for the full divergence catalog.

## Documentation

- [`docs/plan.md`](docs/plan.md) — original phase plan
- [`docs/build-and-load.md`](docs/build-and-load.md) — toolchain and runtime config
- [`docs/testing.md`](docs/testing.md) — test layers and how to run each
- [`docs/mariadb-vs-mysql.md`](docs/mariadb-vs-mysql.md) — handler-API divergence catalog
- [`docs/port-errors-pass1.md`](docs/port-errors-pass1.md) — what broke at first compile (950 errors → 0)
- [`docs/phase3-status.md`](docs/phase3-status.md) — type matrix, AUTO_INCREMENT, cross-restart
- [`docs/phase4-mtr-suite-status.md`](docs/phase4-mtr-suite-status.md) — MTR pass-rate breakdown
- [`docs/phase4-atomic-ddl-investigation.md`](docs/phase4-atomic-ddl-investigation.md) — why atomic DDL was deferred
- [`docs/phase4-txn-lifecycle-progress.md`](docs/phase4-txn-lifecycle-progress.md) — Debug-build assertion + workaround
- [`docs/tidesql-install-sh-analysis.md`](docs/tidesql-install-sh-analysis.md) — what we kept vs dropped from TideSQL's installer

## License

GPLv2, matching MySQL Server and the upstream TideSQL plugin. See [`LICENSE`](LICENSE).

## Acknowledgments

- [TidesDB](https://github.com/tidesdb/tidesdb) — the underlying LSM engine.
- [TideSQL](https://github.com/tidesdb/tidesql) — the MariaDB integration this port is derived from. The handler scaffolding, system variables, and `ENGINE_ATTRIBUTE` JSON design all trace back to TideSQL.
- [MySQL Server](https://github.com/mysql/mysql-server) — the host server. This project is *not* affiliated with Oracle.
