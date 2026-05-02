# tidesdb-mysql

A **MySQL 9.7 storage engine plugin** that uses [TidesDB](https://github.com/tidesdb/tidesdb) — an LSM-tree key/value engine — as the row store. Builds as a loadable `ha_tidesdb.so`; install at runtime with `INSTALL PLUGIN`.

> **Status: experimental / proof-of-concept.** Single-node CRUD works, type coverage is wide, and the upstream TideSQL MTR suite passes 51% (26/51). See [Status](#status) for what works and what doesn't.

This is a port of [TideSQL](https://github.com/tidesdb/tidesql) (TidesDB + MariaDB) to MySQL 9.7. Despite the family resemblance, MariaDB and MySQL have diverged enough at the handler API and data-dictionary layers that this is a rewrite-by-replay rather than a drop-in.

## Quick start

Requires Docker (the build runs inside `tides-builder`, an Ubuntu 24.04 image with the MySQL+TidesDB toolchain pre-installed).

```bash
git clone https://github.com/<you>/tidesdb-mysql.git
cd tidesdb-mysql
./scripts/build-all.sh        # ~30 min cold (clones MySQL, builds plugin)
./scripts/test-plugin.sh      # 31/31 hand-rolled tests
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
| `test-plugin.sh` (hand-rolled CRUD, 31 cases)        | **31/31 ✓** |
| `test-persistence.sh` (cross-restart durability)     | **PASS** |
| `smoke-test.sh` (end-to-end pipeline)                | **all phases ✓** |
| MTR suite lifted from TideSQL (51 tests, run by MTR) | **26/51 (51%)** |

What works:

- `INT`, `BIGINT`, `SMALLINT`, `TINYINT`, `VARCHAR`, `CHAR`, `TEXT`, `BLOB`, `DATE`, `DATETIME`, `TIMESTAMP`, `DECIMAL`, `FLOAT`, `DOUBLE`, `ENUM`
- Primary keys (single-column, integer & string)
- `INSERT`, `SELECT … WHERE pk = ?`, `UPDATE`, `DELETE`, full table scans
- `AUTO_INCREMENT`
- Cross-restart persistence
- Per-table compression (`NONE | SNAPPY | LZ4 | ZSTD | LZ4_FAST`) via `ENGINE_ATTRIBUTE`
- Bloom filters via `ENGINE_ATTRIBUTE`

What doesn't (yet):

- **TRUNCATE TABLE** — returns `ER_ILLEGAL_HA`. ~30 lines to implement (see [phase4-txn-lifecycle-progress.md](docs/phase4-txn-lifecycle-progress.md)).
- **Atomic DDL via SDI** — registered but not exercised; restart-safe DDL is tracked in [phase4-atomic-ddl-investigation.md](docs/phase4-atomic-ddl-investigation.md).
- **Composite / multi-column primary keys** — encoder is single-column only.
- **Secondary indexes** — not implemented; all queries fall back to PK lookup or full scan.
- **2-phase commit / XA** — TidesDB has no `prepare`/`commit_by_xid`; conflict handling currently swallows `TDB_ERR_CONFLICT` to avoid a Debug-build assertion in `MYSQL_BIN_LOG::finish_commit`. See [phase4-txn-lifecycle-progress.md](docs/phase4-txn-lifecycle-progress.md).
- **Replication, FK constraints, savepoints** — not in scope yet.

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
