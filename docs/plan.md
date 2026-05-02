# TIDES — short implementation plan

Target: `ENGINE=TIDESDB` working in MySQL 9.7 LTS, shipped as a loadable plugin (`ha_tidesdb.so`) backed by the existing TidesDB C library.

## Ground rules

- TidesDB library is **not modified**. Build it with `-DBUILD_SHARED_LIBS=OFF`, link the static archive into the plugin.
- All new code lives under `mysql-server/storage/tidesdb/` and is built `MYSQL_ADD_PLUGIN(... STORAGE_ENGINE MODULE_ONLY ...)`. No changes to the rest of the MySQL tree.
- TideSQL (the MariaDB plugin) is read-only design reference. Don't try to port `.cc` files; rewrite against MySQL 9.7's `handler.h`.
- One mysqld process holds **one** open TidesDB instance rooted at `<datadir>/tidesdb_data/`. One MySQL table → one TidesDB column family named `<schema>/<table>`.

## Phase 1 — Loadable stub (target: 1–2 days)

**Deliverable:** `INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so'` succeeds; `SHOW ENGINES` lists `TIDESDB`; `CREATE TABLE t (...) ENGINE=TIDESDB` accepts the statement (no rows actually stored).

Steps:
1. Copy `mysql-server/storage/example/` → `mysql-server/storage/tidesdb/`. Rename `ha_example` → `ha_tidesdb`, `example` → `tidesdb` everywhere.
2. Write `CMakeLists.txt`: `MYSQL_ADD_PLUGIN(tidesdb ha_tidesdb.cc tidesdb_handlerton.cc STORAGE_ENGINE MODULE_ONLY)`. Don't link TidesDB yet.
3. Reproducible build: `scripts/build-plugin.sh` that runs in an Ubuntu 24.04 Docker container (gcc-13, cmake 3.28, MySQL 9.7 deps + Boost via `DOWNLOAD_BOOST=1`). Output: `ha_tidesdb.so` in the build tree.
4. Smoke test: start `mysqld` from the build, `INSTALL PLUGIN`, `SHOW ENGINES`, `CREATE TABLE`. Capture the steps in `docs/build-and-load.md`.

**Exit criteria:** plugin loads cleanly, no errors in error log, `SHOW ENGINES` lists it, `CREATE/DROP TABLE … ENGINE=TIDESDB` is accepted.

## Phase 2 — Wire to TidesDB (target: 1–2 weeks)

**Deliverable:** `INSERT` + `SELECT *` round-trip through TidesDB on a single-PK table.

Steps:
1. Build TidesDB static archive: `cmake -S tidesdb -B tidesdb/build -DBUILD_SHARED_LIBS=OFF -DTIDESDB_BUILD_TESTS=OFF`. Plumb `-I` and `libtidesdb.a` into the plugin's CMake.
2. **Lifecycle**: in the handlerton's `init`, open one `tidesdb_t*` rooted at `<datadir>/tidesdb_data/`. In `deinit`, close it. Store the handle in handlerton-private data.
3. **Row codec** (`tidesdb_row_codec.{h,cc}`): two functions — encode `(TABLE*, uchar* row_buf) → (key_bytes, value_bytes)` and decode the reverse. Start with: PK fields → key (memcmp-ordered), full unpacked row image → value. Cover INT/BIGINT/VARCHAR/TEXT only in this phase.
4. **DDL**: `ha_tidesdb::create()` → `tidesdb_create_column_family(db, "<schema>/<table>", default_config)`. `delete_table()` → `tidesdb_drop_column_family`. `open()` → resolve the CF handle, store on `this`.
5. **DML**: `write_row()` → encode + `tidesdb_put`. Skip `update_row` / `delete_row` for now (return `HA_ERR_WRONG_COMMAND`).
6. **Scan**: `rnd_init` → open a TidesDB iterator over the CF. `rnd_next` → advance + decode. `rnd_end` → close iterator.
7. **PK lookup**: `index_read_map(HA_READ_KEY_EXACT)` → `tidesdb_get` by encoded PK.

**Exit criteria:** the following session works:
```sql
CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c');
SELECT * FROM t;             -- returns 3 rows in PK order
SELECT * FROM t WHERE id=2;  -- returns 1 row
DROP TABLE t;                -- CF removed from disk
```

## Phase 3 — Make it usable (target: 2–3 weeks)

**Deliverable:** real CRUD, transactions, and the `tidesdb_*` knob set.

Steps:
1. `update_row`, `delete_row` (point ops via `tidesdb_put` / `tidesdb_delete`).
2. **Secondary indexes**: one extra CF per index, key = `index_tuple || pk`, value = empty. `index_read`, `index_next`, `index_prev` over those CFs. Index maintenance hooks in `write_row`/`update_row`/`delete_row`.
3. **Transactions**: open a `tidesdb_txn_t*` per `THD` lazily, register with `trans_register_ha`. `hton->commit` → `tidesdb_txn_commit`, `hton->rollback` → `tidesdb_txn_rollback`. Default isolation = `TDB_ISOLATION_READ_COMMITTED`; map `SET TRANSACTION ISOLATION LEVEL` to TidesDB levels.
4. **System variables** (`MYSQL_SYSVAR_*`), names matching TideSQL: `tidesdb_flush_threads`, `tidesdb_compaction_threads`, `tidesdb_block_cache_size`, `tidesdb_max_open_sstables`, `tidesdb_default_write_buffer_size`, `tidesdb_default_sync_mode`, `tidesdb_default_compression`, `tidesdb_log_level`. Read at handlerton init.
5. **Per-table options** via `ha_create_table_option`: `COMPRESSION` enum, `BLOOM_FILTER` bool. Translate to a `tidesdb_column_family_config_t` at `create()` time.
6. **MTR test suite** at `mysql-test/suite/tidesdb/`: `engine_loaded`, `ddl_basic`, `insert_select`, `update_delete`, `secondary_index`, `transaction_basic`, `types_smoke`, `compression_options`. Each as `.test` + `.result`.

**Exit criteria:** suite passes; `mtr --suite=tidesdb` green; ASAN build of plugin loads and runs the suite without leaks.

## Phase 4 — Production-grade (open-ended)

These can be done in any order; none gates the others.

- **Atomic DDL**: set `HTON_SUPPORTS_ATOMIC_DDL`, implement `sdi_set` / `sdi_get` (store SDI inside a dedicated TidesDB CF), implement `pre_ddl` / `post_ddl`. Until done, leave the flag off — DDL still works, just isn't crash-atomic across DD+engine.
- **Crash-safe XA**: register the handlerton as a 2PC participant alongside the binlog. Map TidesDB's WAL+manifest to the prepare/commit phases.
- **Row-based replication**: ensure RBR works (mostly inherited from the handler API; needs RPL test coverage).
- **Wider type coverage**: DECIMAL, DATETIME with fractional seconds, JSON, BLOB/LONGBLOB, generated columns, charset-aware string comparison.
- **Online schema changes** via `inplace_alter_table`.
- **Performance**: bulk insert path (`start_bulk_insert`/`end_bulk_insert` → batched `tidesdb_put`), parallel scan, statistics for the optimizer (`info()`).
- **Packaging**: `.deb` / `.rpm` with the plugin and TidesDB deps; install script that drops `ha_tidesdb.so` into the right plugin dir for a stock MySQL 9.7 install.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| MySQL build is enormous; iteration is slow | Plugin builds as `MODULE_ONLY` — incremental rebuilds touch only `storage/tidesdb/`. Full server is built once. |
| `MERGE_CONVENIENCE_LIBRARIES` errors (the previous attempt's blocker) | `MODULE_ONLY` avoids that path entirely. Don't pass `DEFAULT` to `MYSQL_ADD_PLUGIN`. |
| TidesDB allocator interaction with mysqld's allocator | Build TidesDB without `TIDESDB_WITH_MIMALLOC/TCMALLOC/JEMALLOC`; let mysqld own the heap. Static archive embeds nothing extra. |
| Row codec drift between MySQL field types and what TidesDB expects | Phase 2 covers only INT/BIGINT/VARCHAR/TEXT; expand explicitly in Phase 4 with a typed test matrix. |
| Atomic DDL & DD layer (the divergence point from MariaDB) | Deferred to Phase 4. Plugin works without the flag, just not crash-atomic across `mysqld` kills mid-DDL. |

## Repo additions (cumulative)

```
docs/
  mariadb-vs-mysql.md          # done
  plan.md                      # this file
  build-and-load.md            # Phase 1
mysql-server/storage/tidesdb/  # Phase 1+
  CMakeLists.txt
  ha_tidesdb.{h,cc}
  tidesdb_handlerton.{h,cc}
  tidesdb_row_codec.{h,cc}     # Phase 2
mysql-server/mysql-test/suite/tidesdb/   # Phase 3
  t/*.test
  r/*.result
scripts/
  build-plugin.sh              # Phase 1
  build-tidesdb.sh             # Phase 2
```

## Definition of done (whole project)

`mtr --suite=tidesdb` green on a stock MySQL 9.7 build, plugin installable into a vanilla MySQL 9.7 binary distribution with one `INSTALL PLUGIN` statement, and `SELECT … FROM information_schema.engines WHERE engine='TIDESDB'` shows `SUPPORT='YES'` and `TRANSACTIONS='YES'`.
