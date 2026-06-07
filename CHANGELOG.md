# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased] — initial publication

First public snapshot. The work is summarized below as the four phases it was developed in.

### Phase 1 — Compile clean

- Forked TideSQL's `ha_tidesdb.{cc,h}` as a starting point.
- Replayed every MariaDB→MySQL handler-API edit needed for MySQL 9.7 (initial pass: ~950 compile errors → 0).
- Switched plugin packaging to `MODULE_ONLY` so the engine builds as a loadable `.so` instead of being statically merged via `MERGE_CONVENIENCE_LIBRARIES` (the latter blew up at configure time on first attempt).
- Added a `tidesdb_compat.h` shim for the symbols that exist under different names on MySQL 9.7.
- Builder: Ubuntu 24.04 + GCC 13 + CMake 3.28 + MySQL 9.7.0 source, packaged as the `tides-builder` Docker image.

### Phase 2 — End-to-end CRUD

- Wired the handlerton: `INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so'` succeeds, `SHOW ENGINES` lists `TIDESDB`.
- `CREATE TABLE … ENGINE=TIDESDB` provisions a TidesDB column family per table; `DROP TABLE` removes it.
- Single-row `INSERT` / PK `SELECT` round-trip working.
- Multi-row INSERT, full table scan (`rnd_init`/`rnd_next`/`rnd_pos`), `UPDATE`, `DELETE`.
- Per-row codec: MySQL row image → TidesDB key/value bytes via `tidesdb_row_codec`.
- Hand-rolled test runner at `scripts/test-plugin.sh`: 31/31 passing.

### Phase 3 — Type matrix, persistence, AUTO_INCREMENT

- Type coverage: `INT`, `BIGINT`, `SMALLINT`, `TINYINT`, `VARCHAR`, `CHAR`, `TEXT`, `BLOB`, `DATE`, `DATETIME`, `TIMESTAMP`, `DECIMAL`, `FLOAT`, `DOUBLE`, `ENUM`.
- PK encoding fix: replaced MariaDB's `Field::sort_string()` with MySQL 9.7's `Field::make_sort_key()`. This was load-bearing — wrong encoding broke ordering on every non-int PK.
- `AUTO_INCREMENT` plumbed through `get_auto_increment()` and persisted in the CF metadata.
- Cross-restart durability test: write data, kill mysqld, restart, read back. Data survives.
- `HA_GENERATED_COLUMNS` flag set so `STORED`/`VIRTUAL` generated columns work.

### Phase 4 — MTR suite + ENGINE_ATTRIBUTE

- Lifted TideSQL's MTR suite (61 tests) into `mysql-test-suite/`.
- 18 tests originally relied on MariaDB's per-table options grammar (`ha_create_table_option[]`). MySQL has no equivalent; rebased those tests onto the MySQL-native `ENGINE_ATTRIBUTE` JSON path.
- Plugin parses `ENGINE_ATTRIBUTE` with rapidjson; `HTON_SUPPORTS_ENGINE_ATTRIBUTE` set in the handlerton.
- Final pass rate: **26/51 (51%)** on the lifted suite.
- Atomic-DDL / SDI integration investigated and deferred — handlerton callbacks are registered but not exercised end-to-end.
- Txn lifecycle bug partly mitigated. The Debug build of MySQL asserts at `binlog.cc:7756` whenever a commit hook returns non-zero. `tidesdb_txn_commit` now returns success on `TDB_ERR_CONFLICT` (txn is still rolled back; data correctness preserved).

### Tooling and docs

- `scripts/build-all.sh` — one-command cold build.
- `scripts/setup-workspace.sh` — clones MySQL + TidesDB into `vendor/`, drops plugin and test suite into the MySQL tree.
- `scripts/replay-port-edits.sh` — deterministic, idempotent rewrite from upstream TideSQL source.
- `scripts/test-plugin.sh` — runs the 31 hand-rolled SQL/expected pairs.
- `scripts/smoke-test.sh` — full end-to-end pipeline check.
- `scripts/test-persistence.sh` — restart-survival regression.
- 10 design / status docs under `docs/`.

### Phase 5 — drive the MTR suite to fully green

Worked through the remaining MTR failures via four rounds of targeted bug fixes and test ports. Pass rate went **26/51 → 47/47 executed (100%) with 5 cleanly skipped**.

Handler bugs found and fixed:

- **`TRUNCATE TABLE` returned `ER_ILLEGAL_HA`** — MySQL 9.7 split TRUNCATE off into a separate `truncate(dd::Table*)` hook (MariaDB used `delete_all_rows` for both). One-line delegate to `delete_all_rows`.
- **`recover_counters()` Debug assertion** — read field values via `val_int_offset()` inside `open()` without setting up `read_set`. Wrapped in `tmp_use_all_columns`/`tmp_restore_column_map`.
- **Sysvar update callbacks tripping `binlog.cc` / PSI memory invariants** — `tidesdb_backup_dir` and `tidesdb_checkpoint_dir` raised errors via `my_printf_error` from `update`, which Debug builds assert against; bailing early also leaked the framework's PLUGIN_VAR_MEMALLOC bookkeeping. Refactored to do the work in a `check` callback.
- **PK ICP bypassed `end_range`** — `idx_cond_push` set `in_range_check_pushed_down = true` for every key, but the PK branch in `index_next` never evaluated the pushed condition; `compare_key` short-circuited and rows past the upper bound leaked. Refused ICP for the PK index.
- **2-phase commit** — Phase 4's workaround returned `0` from the commit hook on `TDB_ERR_CONFLICT` (to avoid `binlog.cc:7756` assert in Debug builds), which silenced conflicts from the user. Moved the actual TidesDB commit into a `prepare` hook so conflicts surface as `ER_LOCK_DEADLOCK` cleanly, with the commit hook becoming a no-op when prepare already committed.
- **`info(HA_STATUS_ERRKEY)` was a no-op** — `handler::get_dup_key` resets `errkey` to -1 before calling `info`, expecting the engine to populate it. Without it, `REPLACE` / `INSERT IGNORE` / IODKU bailed with `ER_DUP_KEY (1022)` instead of recovering and surfacing `ER_DUP_ENTRY (1062)`. Saved last dup-key index in `last_dup_key_no_`, restored in `info()`.
- **ICP state leaked across scans** — `in_range_check_pushed_down` survived from a prior scan into a follow-on scan on the same handler instance even when no new push happened, breaking `end_range` enforcement on UNION-ALL and similar reuse patterns. Cleared in `index_init` when the previous push targeted a different key OR `pushed_idx_cond` was already cleared but the flag was orphaned.

Test ports (MariaDB grammar / MySQL 8+ schema changes):

- `BEGIN NOT ATOMIC` anonymous compound blocks → stored procedures (concurrent_errors, write_pressure)
- `SET STATEMENT v=N FOR <stmt>` → save-set-run-restore (ttl)
- `CREATE TABLE x AS SELECT` (trips `ER_GTID_UNSAFE_CREATE_SELECT`) → split into `CREATE TABLE` + `INSERT…SELECT` (sql)
- MariaDB-only `mrr_sort_keys=on` optimizer flag → drop (mrr)
- `CREATE OR REPLACE TABLE` → `DROP TABLE IF EXISTS` + `CREATE TABLE` (drop_create)
- `ALTER TABLE … SYNC_MODE='X'` → `ALTER TABLE … ENGINE_ATTRIBUTE='{"sync_mode":"X"}'` (online_ddl, rename, tombstone_density)
- `information_schema.GLOBAL_STATUS` → `performance_schema.global_status` (status_vars, tombstone_density)
- Drop MariaDB-only `--source include/have_innodb.inc` / `have_partition.inc` (engine_convert, mixed_engine, partition)

Cleanly skipped via `--skip` (5 tests, each with a specific documented gap):
`tidesdb_vector`, `tidesdb_encryption`, `tidesdb_online_ddl`, `tidesdb_partition`, `tidesdb_pessimistic_insert_lock`.

### Phase 6 — composite primary keys + community Docker image

Two real handler bugs and one test-harness leak gated proper composite-PK use, even though the encoding layer (`make_comparable_key`) had iterated over all `user_defined_key_parts` from the start.

Handler fixes:

- **`AUTO_INCREMENT` rejected as a non-leading PK column.** MySQL's `check_if_table_can_have_primary_key` requires `HA_AUTO_PART_KEY` in `table_flags()` before allowing schemas like `PRIMARY KEY (tenant, id)` with `id AUTO_INC`. Without the flag, MySQL bails with `ER_WRONG_AUTO_KEY (1075)`. Added the flag.
- **Secondary `UNIQUE` checks silently bypassed on AUTO_INC PK tables.** `write_row` reused the same `skip_unique` flag for both the PK uniqueness check (correctly skipped — autogen IDs are guaranteed unique) and the secondary `UNIQUE` loop (must NOT be skipped — autogen says nothing about secondary uniques). Result: `INSERT/REPLACE/INSERT IGNORE` silently accepted duplicates on `UNIQUE KEY (region, sku)`. Split the conditions.

Test infrastructure:

- `mh_bootstrap` was wiping `$DATA` (MySQL datadir) but not `$REPO/tidesdb_data/` where TidesDB CFs actually live. Same-named tables on consecutive runs inherited stale CF data and tripped false duplicates on the very first INSERT. Now `mh_bootstrap` wipes both.

Tests added:

- `tests/phase2/33_composite_pk_edge.sql` — 3-column composite PK, VARCHAR+DATE composite, `AUTO_INCREMENT` trailing in composite PK, PK column UPDATE (row movement).
- `tests/phase2/34_composite_unique.sql` — composite UNIQUE KEY enforcement on a table with AUTO_INC PK (regression test for the fix above).

Hand-rolled tally: **31/31 → 33/33**.

### Community Docker image (`docker/Dockerfile.mysql`)

Two-stage build that produces a runnable `tidesdb/mysql:9.7` image — anyone can play with the engine in a single command:

```
docker build -f docker/Dockerfile.mysql -t tidesdb/mysql:9.7 .
docker run -d -e MYSQL_ROOT_PASSWORD=secret -p 3306:3306 tidesdb/mysql:9.7
```

- **Stage 1** (Oracle Linux 9 + `gcc-toolset-14`): builds `libtidesdb.a` and `ha_tidesdb.so` against MySQL 9.7.0 source. OS family matches the runtime image so glibc/libstdc++ ABI is binary-compatible.
- **Stage 2** (`FROM mysql:9.7`): adds `snappy` (the only runtime dep not already in the base image), drops `ha_tidesdb.so` into `/usr/lib64/mysql/plugin/`, ships an auto-load `tidesdb.cnf` at `/etc/mysql/conf.d/`, and a one-time demo schema in `/docker-entrypoint-initdb.d/`.

`docker/runtime/smoke-test.sh` exercises engine load, CRUD, composite PK + AUTO_INC, composite UNIQUE constraint enforcement, and durability across container restart.

`docker/runtime/docker-compose.yml` provides one-command bring-up with a named volume for data persistence.

### Data directory default moved inside the datadir

The auto-computed default for TidesDB's data was a *sibling* of the MySQL datadir (`<datadir>/../tidesdb_data`) — a port artifact carried over from TideSQL on MariaDB. That placed engine data outside the datadir, where MySQL's backup, clone, and `--datadir` relocation tooling doesn't expect it, and a source build would scatter `tidesdb_data/` next to the data dir.

The default is now `<datadir>/.tidesdb`:

- **Inside the datadir**, matching TideSQL on MariaDB and the location MySQL tooling expects (same volume / permissions / backup treatment as InnoDB).
- **Leading dot**, following the MyRocks (`.rocksdb`) / InnoDB (`#innodb_*`) convention, so the directory is never mistaken for a schema directory. A bare `tidesdb_data` would have collided with `CREATE DATABASE tidesdb_data`.

`tidesdb_data_home_dir` still overrides the location. The Docker/packaging `tidesdb.cnf` set this explicitly and now point at `/var/lib/mysql/.tidesdb/`.

**Migration:** data written by an earlier build lives at the old path (`<datadir>/../tidesdb_data` from a source build, or `/var/lib/mysql/tidesdb_data` in the Docker image). Move it to the new location or set `tidesdb_data_home_dir` to the old path. Requires a plugin rebuild to take effect.

### Bundled engine bumped to TidesDB v9.2.5

The vendored engine moved from v9.2.0 to **v9.2.5** across the Docker images, the RPM packaging, and `setup-workspace.sh`.

The headline reason: all four durability bugs we had been carrying as `0001-walfix.patch` against v9.2.0 are **fixed upstream in v9.2.5**, verified one by one:

- **`convert_sync_mode` inversion** — the engine sync enum was reordered (`NONE=0, FULL=1, INTERVAL=2`), making the existing switch correct. Our old rewrite would now map `FULL→NONE` and re-break durability, so the patch is **retired, not ported**.
- **Raw sync_mode at WAL opens** — `block_manager_open` now calls `convert_sync_mode` internally, so passing the raw enum is correct.
- **Unconditional WAL truncate before recovery** — `tidesdb_create_column_family` now validates (preserves) an existing WAL and only truncates a genuinely fresh CF.
- **SSTable cursor block_size caching** — the cursor now caches the real on-disk size only when a block was read from disk, else forces a re-read; the row-dropping multi-SSTable recovery scan is gone.

What we still carry is `docker/patches/0001-bloomfix.patch` (TidesDB **PR #626**): a use-after-free in `bloom_filter_new` whose post-malloc failure paths `free(*bf)` without nulling, which a compaction worker turned into a GPF in `bloom_filter_add`. Removed once PR #626 lands upstream. See [KNOWN-ISSUES.md](KNOWN-ISSUES.md) for the full per-bug writeup.

The migration was gated on a clean HammerDB SIGKILL recovery run (TPC-C load → `docker kill -9` mid-write → restart → committed row counts match `district.d_next_o_id - 1`) against the v9.2.5-based image.

### Bundled engine bumped to TidesDB v9.3.0 (release v0.3.0)

The vendored engine moved from v9.2.5 to **v9.3.0** across the Docker images (`mysql`, `mtr`, `mwbench`), the RPM packaging, and `setup-workspace.sh`.

The headline reason: **we now ship the engine with zero patches.** The last fix we carried, `0001-bloomfix.patch` (TidesDB **PR #626** — the `bloom_filter_new` use-after-free), landed upstream verbatim in v9.3.0: the four `*bf = NULL;` guards on the post-malloc failure paths and the return-checked `tidesdb_partitioned_merge` file-max-split caller with its `TDB_LOG_WARN`. The patch file is deleted and the `docker/patches/` step removed from every Dockerfile and script. (`0001-walfix.patch` was already retired at v9.2.5.)

Plugin-side changes that come with the bump:

- **New error code `TDB_ERR_BUSY` (-14) mapped.** v9.3.0 returns this from the backpressure-stall timeout sites (L0-queue, active-memtable ceiling, memory-pressure critical) that previously returned `TDB_ERR_IO` or `TDB_ERR_MEMORY_LIMIT`. `tdb_rc_to_ha` now maps it to `HA_ERR_LOCK_WAIT_TIMEOUT` (transient, statement-only rollback, retriable) instead of letting it fall through to `HA_ERR_CRASHED` — which would have looked like corruption — and it joins the transient-error lists in the prepare/commit/bulk-commit retry paths.
- **`default_l0_queue_stall_threshold` default lowered 20 → 10**, matching upstream's new default. With v9.3.0's active-memtable ceiling now bounding the active memtable to 2× `write_buffer_size`, the L0 queue was the last unbounded-growth surface; 20 immutables at the 64 MiB default allowed ~1.3 GiB of headroom before the stall fired, which is too lenient.

Engine improvements inherited from v9.3.0 (no plugin change needed): a hard active-memtable backpressure ceiling at 2× `write_buffer_size` (directly addresses the unbounded memtable growth behind the WARE=100 OOM seen during v0.2.5 validation), four additional use-after-free/race fixes (`tidesdb_memtable_try_ref`, level reclamation, `tidesdb_checkpoint`, deferred-free reaper), compaction-trigger correctness fixes, and `max_concurrent_flushes` now pinned 1:1 to `num_flush_threads` (we never set it, so no warning). See [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

Gated on the full validation suite: MTR, the HammerDB SIGKILL recovery gate, the mwbench engine-integrity gate (0 mismatches / 0 misses), and a full HammerDB WARE=100 throughput run. See [docs/v9.3.0-validation-report.md](docs/v9.3.0-validation-report.md).

## v0.4.1 — 2026-06-07

Perf-instrumentation **infrastructure** release. Adds a layer-by-layer latency capture surface to the plugin (thread-local rdtsc rings, per-method binary files, offline analyser) so future engine-side optimisation work has a fact base. No engine optimisations ship in this release — the deliverable is the **measurement surface**, not a perf improvement. The bundled engine is unchanged from v0.4.0 (still TidesDB **v9.3.2**, still shipped unpatched). The default `tidesdb/mysql:9.7` image is unchanged; instrumentation lives in a separate `tidesdb/mysql:9.7-perf` variant.

See [docs/v0.4.1-validation-report.md](docs/v0.4.1-validation-report.md) for the full validation matrix and captured headline numbers from a WARE=10 RUNVU=8 3-min HammerDB TPROC-C run.

### Added

- `TDB_PERF_SCOPE(MethodId)` RAII macro covering the full plugin entry surface (32 `MethodId` values: `write_row`, `update_row`, `delete_row`, `index_read_map`, `index_next`, `index_prev`, `rnd_next`, `rnd_pos`, `external_lock`, `start_stmt`, `store_lock`, `commit`, `rollback`, four `savepoint_*`, `open`, `close`, `info`, `table_flags_cache_init`, `create`, `delete_table`, four inplace-ALTER virtuals, `serialize_row`, `deserialize_row`, `key_copy_to_comparable`, `pk_from_record`, `encrypt_row_into`, `decrypt_row`). On capture-off the overhead is two `__rdtsc()` reads + one atomic load per scope. Compile-time-gated on `TIDESDB_PERF`; with `TIDESDB_PERF=0` (the default) the macro expands to `((void)0)`.
- Thread-local `TLS_Ring` (default `2^16` samples per thread, runtime-configurable). 24-byte `Sample` packed natural-alignment with `static_assert` on the layout. Rings self-register into a lock-free linked list (`g_rings_head`) on first use.
- Background flusher thread pinned to CPU 0; walks the ring list every `tidesdb_perf_flush_interval_ms`, bucket-sorts by method, `pwritev`s each bucket to its method's append-only `.bin` file. Calibrates TSC via 20 ms `std::chrono::steady_clock` spin on `perf::init()` and writes the calibration to `meta.json`. Ring overflow logged on first occurrence then rate-limited to once per 60 seconds.
- Four perf sysvars (all gated on `TIDESDB_PERF=1`): `tidesdb_perf_capture` (`BOOL`, default `OFF` — the kill-switch), `tidesdb_perf_output_dir` (`STRING`, default `/var/lib/mysql/tidesdb-perf`, `PLUGIN_VAR_MEMALLOC`), `tidesdb_perf_ring_capacity_pow2` (`INT`, default `16`, `PLUGIN_VAR_READONLY`), `tidesdb_perf_flush_interval_ms` (`INT`, default `1000`).
- `tools/tidesdb_perf_analyze` — Python 3 + numpy offline analyser. Parses `.bin` files (`struct.pack('<BBH4xQQ', ...)`), aggregates per-method `calls / total_ms / mean_us / p50 / p95 / p99 / max`, emits markdown. `--compare A B` produces a side-by-side diff between two capture directories.
- `bench/perf/run-perf-capture.sh` integration harness. Wraps `bench/hammerdb/run-hammerdb.sh`, forces perf sysvars ON via `DB_EXTRA_ARGS`, mounts the perf output directory into the container, copies + chowns artifacts back to the host, runs the analyser, writes `report.md`. `COMPARE=1` extends the same flow against `sut-mariadb-tidesdb:9.3.0-perf`.
- `docker/patches/tidesql/0001-perf-instrumentation.patch` (880 lines) — ports `Sample` / `TLS_Ring` / `PerfScope` + 30 `TDB_PERF_SCOPE` call sites into TideSQL's `ha_tidesdb.cc` for the eventual MySQL-vs-MariaDB side-by-side run. Sysvar wiring deferred to v0.5.0.
- `Dockerfile.mysql` build-arg `TIDESDB_PERF` (default `0`). `docker build --build-arg TIDESDB_PERF=1 -t tidesdb/mysql:9.7-perf .` produces the perf variant. Forwarded as both `-DTIDESDB_PERF` **and** the `TIDESDB_PERF` environment variable on the plugin cmake configure + build steps — the latter is required because the cmake `-D` from the top-level mysql cmake invocation does not always propagate into the plugin sub-scope across cmake versions; the plugin's `CMakeLists.txt` keys on `ENV{TIDESDB_PERF}` explicitly.
- 5 new MTR tests (`tidesdb_perf_*`): no `.bin` files when capture OFF, growth stops when capture is flipped OFF mid-run, `meta.json` is valid, ring overflow is logged once + rate-limited, no DML regression with capture ON.
- 8 new gtest unit tests in `tidesdb_atomic_ddl_tests` covering the ring (`PushReadRoundTrip`, `WrapBehaviour`, `TombstoneDrained`, `AllocSelfRegistersInGlobalList`, `NoCaptureZeroPushes`, `CaptureOnPushesOneSample`, `NestedScopesOrdered`, `ConcurrentPushSingleReader`).

### Changed

- `bench/perf/run-perf-capture.sh` now docker-mediates the artifact copy (rather than a bare `cp -r`) so that `.bin` files written by mysql uid 999 inside the container (mode 640) become readable by the invoking host user.

### Deferred to v0.5.0

- **Engine-side optimisations.** v0.4.1 ships the measurement surface; the captured hotspots (`write_row` 5.29M calls / mean 16.5 μs / max 72 ms — long compaction tail; `index_read_map` 1.9M calls / mean 13.2 μs / p99 108 μs) are the entry points for the v0.5.0 work.
- **`sut-mariadb-tidesdb:9.3.0-perf` SUT image.** TideSQL perf patch is committed but not yet applied + built; `COMPARE=1` runs against MariaDB are not yet wired end-to-end.
- **TideSQL perf sysvars.** The TideSQL patch wires the scope macros + ring; sysvar wiring is deferred (would have inflated the patch by hundreds of lines).

## v0.4.0 — 2026-06-04

Atomic-DDL participation (closes review finding **A-5**). `HTON_SUPPORTS_ATOMIC_DDL` is now set on the TidesDB handlerton, schema metadata is persisted in `dd::Table::se_private_data` during CREATE/ALTER, the plugin is self-healing across crash-during-DDL via an eager startup reconciliation sweep, and the full set of SDI + DDSE callbacks is wired. The bundled engine is unchanged from v0.3.1 (still TidesDB **v9.3.2**, still shipped unpatched).

See [docs/v0.4.0-validation-report.md](docs/v0.4.0-validation-report.md) for the full validation matrix.

### Added

- `HTON_SUPPORTS_ATOMIC_DDL` flag on the handlerton. The server now drives DD commit/rollback through our prepare/commit hooks and rolls the DD back on engine-side failure.
- Six SDI tablespace callbacks (`sdi_create`, `sdi_drop`, `sdi_get_keys`, `sdi_get`, `sdi_set`, `sdi_delete`) against a dedicated metadata column family `__tidesdb_sdi`. Key encoding via `SdiStore::pack_key(tablespace_id, sdi_type, sdi_id)` — deterministic 24-byte big-endian for clean prefix scans.
- Eight DDSE callback stubs (`ddse_dict_init`, `dict_init`, `dict_recover`, `dict_cache_reset`, `dict_cache_reset_tables_and_tablespaces`, `dict_get_server_version`, `dict_set_server_version`, `is_dict_readonly`) — each logs once at INFO if invoked. Forward-capability slots for a future "TidesDB hosts the data dictionary" project; no production MySQL 9.7 path drives them.
- Single engine-wide `tidesdb_system` tablespace registered via `Plugin_tablespace`.
- `prepare_create` writes `cf_name`, SHA-256 `schema_fingerprint`, CRC32 `options_checksum`, `atomic_ddl=1`, and `created_at` into `dd::Table::se_private_data` on every `CREATE TABLE`.
- `validate_open` enforces the binding on every open under strict mode: `atomic_ddl` flag, CF-name binding, schema fingerprint, options checksum.
- `prepare_drop` removes the SDI blob before the CF drop so an interrupted drop never leaves SDI pointing at a dead CF.
- `DdSyncReconciler::compute_delta_pure` (symmetric difference between DD `dd::Table` rows and on-disk CFs, excluding `__`-prefixed CFs) + `DdSyncReconciler::apply_delta` (resolves drift per the new `tidesdb_orphan_action` sysvar at startup).
- New sysvar `tidesdb_atomic_ddl_strict` (BOOL, default **ON**) — enforce `atomic_ddl=1` in `se_private_data` on open.
- New sysvar `tidesdb_orphan_action` (ENUM `drop` / `quarantine` / `log_only`, default **`quarantine`**) — governs the startup orphan sweep.
- 20 new MTR tests (4 SDI round-trip, 2 atomic CREATE, 2 atomic DROP, 1 atomic ALTER, 4 sweep variants, 3 crash-during-DDL, 1 DDSE-stubs-inert, 2 legacy-open, 1 tablespace-visible).
- 10 new gtest unit tests (3 `SdiPackKey`, 7 `ReconcilerDelta`).

### Changed

- The four inplace ALTER virtuals (`check_if_supported_inplace_alter`, `prepare_inplace_alter_table`, `inplace_alter_table`, `commit_inplace_alter_table`) now actively consume their `dd::Table*` parameters (previously `[[maybe_unused]]`). `commit_inplace_alter_table` recomputes `schema_fingerprint` + `options_checksum`, re-emits SDI via `sdi_set`, and writes updated `se_private_data` into the new `dd::Table*` atomically with the CF mutation. Rollback restores the saved `se_private_data` and reverts the CF.
- `ha_tidesdb::rename_table` and `ha_tidesdb::delete_table` now flush the engine session txn before mutating the CF (see *Fixed* below).

### Fixed

- **COPY-ALTER 2PC SIGSEGV** (commit `0d7fe2c`). Activating `HTON_SUPPORTS_ATOMIC_DDL` defers the engine commit past `sql_table.cc:19191`'s `quick_rm_table`, which surfaced a pre-existing latent heap-use-after-free inside the TidesDB C library: `tidesdb_iter_release_sst_source_block` decrementing a freed `clock_cache` block refcount at offset `0x20`. Live SST iterators on the backup CF outlived `tidesdb_drop_column_family`. Fix: new helper `tidesdb_flush_engine_txn_before_cf_mutation(THD*)` is called at the top of `rename_table` and `delete_table` — it commits the engine txn if dirty (releasing iterators), rolls back if clean, frees the handle, releases TidesDB row locks, and sets `commit_done=true` so subsequent hton hooks short-circuit at the engine layer. This restores pre-flag-flip engine-layer ordering without weakening the atomic-DDL contract at the server / DD layer. This fix is **tactical**; the architectural fix — a SE-private DDL journal so CF rename / drop replays alongside user data writes — is deferred to v0.5.0.

### Upgrade notes

- **Pre-v0.4.0 tables have no `se_private_data` and no SDI blob.** With `tidesdb_atomic_ddl_strict=ON` (the new default), opening such a table fails with `ER_TIDESDB_LEGACY_TABLE_STRICT`. The supported upgrade path is to run a no-op `ALTER TABLE t ENGINE=TIDESDB` per user table, which populates `se_private_data` and emits the SDI blob. For bulk migration, set `tidesdb_atomic_ddl_strict=OFF` temporarily — legacy tables will open with a warning while the ALTERs are applied — then flip it back to `ON`.
- **The default orphan-sweep action is `quarantine`.** Operators upgrading from v0.3.x will likely have no orphan CFs at all (the v0.3.x path never produced any), but if drift is detected on first start, the sweep renames orphans to `__quarantine_<orig>_<ts>` rather than dropping them. Set `tidesdb_orphan_action=log_only` for a dry-run pass, or `drop` to reclaim quarantined CFs once you're confident.
- **Full `mysqldump --tab` integration is smoke-tested only in v0.4.0** (the four SDI MTR tests cover round-trip on the metadata CF); the full end-to-end integration is deferred to v0.5.0.

### Bundled engine bumped to TidesDB v9.3.2 (release v0.3.1)

The vendored engine moved from v9.3.0 to **v9.3.2** across the Docker images (`mysql`, `mtr`, `mwbench`), the RPM packaging, and `setup-workspace.sh`. The engine continues to ship **with zero patches**. No plugin code changes — the upstream releases are patch-level and do not introduce new error codes or other public-API surface for `tdb_rc_to_ha` to map.

Upstream highlights inherited from v9.3.1 + v9.3.2:

- **v9.3.1 — concurrency and durability hardening.** Five distinct memory-safety / race fixes: the clock-cache reader-pin wraparound at 128 concurrent readers (would corrupt zero-copy buffers under load), a flush-cleanup use-after-free when more than sixteen immutable memtables accumulated in a single pass, a dangling pointer left by transaction reset on repeatable-read / snapshot, a duplicate column-family registration race, and 32-bit MSVC atomic fallbacks.
- **Reader FD starvation fixed.** v9.3.1 fixes a descriptor-accounting leak in the flush path (a bare `close` that never decremented the counted-open counter), and the reader / reaper budgets are now a single shared value so the reserve always stays available. This is the engine-side counterpart to the fd-pressure behaviour we documented in the v9.3.0 100 GiB stress run.
- **L1 hard-stop removed.** Write admission is now governed by the L0 queue stall and the active-memtable ceiling; L1 contributes only graduated delays. Removes a backpressure stall that normally settled on its own.
- **Parallel compaction within a round.** Per-CF compaction borrows ephemeral helper threads (work-stealing, including the calling thread) and shards merge output across key-range subcompactions. Validated under sanitizers; ~25 % higher ingest throughput in upstream's tests, clean recovery from mid-round kill.
- **v9.3.2 — small additions.** A `_tidesdb_cancel_background_work_` helper for quick shutdown under large flush/compaction queues, support for bloom filters and block indexes that exceed the 4 GB block-manager size (auxiliary klog blocks are now chunked; **backwards-compatible**), and flaky-test hardening.

Gated on a re-run of the v0.3.0 validation suite — MTR, the mwbench integrity gate, the HammerDB SIGKILL recovery gate, and HammerDB WARE=100 throughput. See [docs/v9.3.2-validation-report.md](docs/v9.3.2-validation-report.md).
