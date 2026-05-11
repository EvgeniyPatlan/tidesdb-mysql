# tidesdb-mysql

A **MySQL 9.7 storage engine plugin** that uses [TidesDB](https://github.com/tidesdb/tidesdb) — an LSM-tree key/value engine — as the row store. Builds as a loadable `ha_tidesdb.so`; install at runtime with `INSTALL PLUGIN`.

> **Status: experimental / proof-of-concept.** Single-node CRUD works, type coverage is wide, and the lifted MTR suite is **fully green — 50/50 executed tests pass with 2 cleanly skipped** (each marked with the specific feature gap that prevents it: native partitioning, MySQL vector API). See [Status](#status) for the full breakdown.

This is a port of [TideSQL](https://github.com/tidesdb/tidesql) (TidesDB + MariaDB) to MySQL 9.7. Despite the family resemblance, MariaDB and MySQL have diverged enough at the handler API and data-dictionary layers that this is a rewrite-by-replay rather than a drop-in.

## Quick start — install alongside your MySQL server

If you already run `mysql-server` from your distro's package manager, you can install the plugin as a sibling package and enable the engine without rebuilding mysqld.

**Debian / Ubuntu** (mysql-server-9.7+):

```bash
sudo dpkg -i tidesdb-mysql-plugin_<version>_amd64.deb
# Enable on every restart:
sudo cp /usr/share/doc/tidesdb-mysql-plugin/tidesdb.cnf.example \
        /etc/mysql/conf.d/tidesdb.cnf
sudo systemctl restart mysql
mysql -uroot -e "SHOW ENGINES;" | grep -i tidesdb
```

**RHEL / Oracle Linux / Rocky 9** (mysql-server-9.7+):

```bash
sudo dnf install ./tidesdb-mysql-plugin-<version>-1.x86_64.rpm
sudo cp /usr/share/doc/tidesdb-mysql-plugin/tidesdb.cnf.example \
        /etc/my.cnf.d/tidesdb.cnf
sudo systemctl restart mysqld
mysql -uroot -e "SHOW ENGINES;" | grep -i tidesdb
```

Or load the plugin once at runtime instead of at every restart:

```sql
INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so';
```

**Build packages from source:**

```bash
./scripts/package-deb.sh 0.1.0       # writes dist/tidesdb-mysql-plugin_0.1.0_amd64.deb
./scripts/package-rpm.sh 0.1.0       # writes dist/tidesdb-mysql-plugin-0.1.0-1.x86_64.rpm
```

Both produce a binary package containing `ha_tidesdb.so` plus docs and an example config snippet. The `.deb` is built on Ubuntu 24.04 and the `.rpm` on Oracle Linux 9 (matching the official `mysql:9.7` image's libstdc++ ABI).

## Quick start (Docker — for trying it out without installing on the host)

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

### Running tests against the Docker image

The repo ships a smoke test that spins up a container, asserts the engine loads, exercises CRUD + composite PK + composite UNIQUE, restarts the container and verifies data persisted. It is the same script CI runs on every push.

```bash
# Test the image you just built locally:
./docker/runtime/smoke-test.sh                     # uses tidesdb/mysql:9.7

# Or test an arbitrary tag (e.g. the published Docker Hub image):
./docker/runtime/smoke-test.sh evgeniypatlan/test-images:mysql-9.7-tidesdb

# Override the temp container name / root password if you have a conflict:
CONTAINER=tdb-test PASSWORD=hunter2 ./docker/runtime/smoke-test.sh tidesdb/mysql:9.7
```

The script creates a throwaway container named `tidesdb-smoke` and `docker rm -f`s it on exit (including on failure), so it never pollutes your `docker ps` and is safe to re-run.

Expected output ends with:

```
[smoke] all checks passed for tidesdb/mysql:9.7
```

If a step fails, the script prints `FAIL: <what>` plus the last 30 lines of `docker logs` for the container so you can see what mysqld said.

#### Ad-hoc SQL tests against a running container

If the smoke test passes and you want to run your own SQL against the same image (e.g. to reproduce a bug):

```bash
# Start a clean container in the background:
docker run -d --name tdb-play -e MYSQL_ROOT_PASSWORD=secret tidesdb/mysql:9.7

# Wait for it to be ready (mysql:9.7 entrypoint does a temp-mysqld bootstrap pass first):
until docker exec tdb-play mysql -uroot -psecret -e 'SELECT 1' >/dev/null 2>&1; do sleep 2; done
sleep 5   # let the entrypoint finish its restart

# Run a SQL file from your host:
docker exec -i tdb-play mysql -uroot -psecret < my_test.sql

# Or pipe inline SQL:
docker exec -i tdb-play mysql -uroot -psecret <<'SQL'
CREATE DATABASE t;
CREATE TABLE t.kv (k INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
INSERT INTO t.kv VALUES (1,'hello');
SELECT * FROM t.kv;
SQL

# Tear down:
docker rm -f tdb-play
```

The phase2 SQL fixtures under `tests/phase2/` are valid input to `mysql` and can be replayed this way (skip the `--source include/...` lines — those are MTR-specific):

```bash
docker exec -i tdb-play mysql -uroot -psecret < tests/phase2/05_insert_one_row.sql
```

#### What you *can't* run against just the runtime image

`./scripts/test-plugin.sh` and the MTR suite (`./mtr ...`) need the full MySQL build tree (`vendor/mysql-server/build/`) — they invoke `mysqld` directly with custom data dirs and tooling that don't exist inside the runtime image. To run those, build from source (see below).

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
|  `test-plugin.sh` (hand-rolled CRUD, 37 cases)        | **37/37 ✓** |
| `test-persistence.sh` (cross-restart durability)     | **PASS** |
| `smoke-test.sh` (end-to-end pipeline)                | **all phases ✓** |
| MTR suite lifted from TideSQL (52 tests)             | **50/50 executed pass, 2 skipped** |

What works:

- Types: `INT`, `BIGINT`, `SMALLINT`, `TINYINT`, `VARCHAR`, `CHAR`, `TEXT`, `BLOB`, `DATE`, `DATETIME`, `TIMESTAMP`, `DECIMAL`, `FLOAT`, `DOUBLE`, `ENUM`, `JSON`
- **Primary keys: single-column AND composite** (mixed types, including `AUTO_INCREMENT` as a non-leading column — `PRIMARY KEY (tenant, id)` with `id AUTO_INC` works)
- `INSERT`, `SELECT … WHERE pk = ?`, `UPDATE`, `DELETE`, full table scans, range scans (PK + secondary)
- `AUTO_INCREMENT`, `TRUNCATE TABLE`, `REPLACE INTO`, `INSERT … ON DUPLICATE KEY UPDATE`
- **Secondary indexes (single-column AND composite)** with ICP; `UNIQUE KEY (a, b)` enforced even on tables with `AUTO_INCREMENT` PK
- **Online DDL: `ALTER TABLE … ADD/DROP INDEX, ALGORITHM=INPLACE`** (concurrent reads OK; writes blocked while index populates)
- **`ALGORITHM=INSTANT ADD COLUMN`** with NULL or NOT NULL DEFAULT — existing rows back-fill from `default_values` on read, no row rewrite
- **`ALGORITHM=INSTANT DROP COLUMN`** when the dropped column is at the end of the table — no row rewrite. Mid-column drops fall back to `ALGORITHM=COPY` (still correct, just slower).
- **At-rest encryption (AES-256-CBC)** via a master-key file pointed to by `--tidesdb-master-key-file=<path>` (32 bytes raw key); per-table opt-in with `ENGINE_ATTRIBUTE='{"encrypted":true}'`
- **Pessimistic row locking** (opt-in via `--tidesdb-pessimistic-locking=ON`) — InnoDB-style row-level locks for write workloads; engine-level OCC is automatically downgraded to TidesDB `READ_COMMITTED` so the SQL-layer lock manager handles serialization without false-positive commit conflicts.
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
- **`tidesdb_vector`** — uses MariaDB's `VECTOR(N)` column type / `VECTOR INDEX` DDL; needs rewrite against MySQL 9 vector functions.

Not yet, but in-scope for follow-up work:

- **`ALGORITHM=INSTANT DROP COLUMN` for non-trailing columns** — currently falls back to `ALGORITHM=COPY` for middle-of-table drops. Lifting this requires per-row schema versioning (à la InnoDB's INSTANT DROP) so old rows can be remapped to the new layout. Trailing drops already work as INSTANT.
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
