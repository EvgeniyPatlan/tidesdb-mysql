# Testing the TidesDB plugin

Two layers of tests, both run inside the `tides-builder` Docker image so they don't depend on host packages.

## 1. Smoke test — does it load at all?

The minimum bar for "did the plugin survive the port." Brings up `mysqld`, loads the plugin, and walks through six phases (engine listing, INSTALL PLUGIN, SHOW ENGINES, CREATE TABLE, INSERT/SELECT, DROP).

```bash
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v /home/corvin/TIDES:/work \
    tides-builder /work/scripts/smoke-test.sh
```

Output is interleaved phase-by-phase to stdout. The mysqld error log tail prints at the end — that's where you'll see plugin-init errors, segfaults, or symbol-resolution failures.

Expected first-run failure modes (and what they mean):

| Output | Diagnosis |
|---|---|
| `INSTALL PLUGIN ... ERROR ... incompatible plugin protocol version` | Our descriptor's interface_version doesn't match MySQL's. Edit `mysql_declare_plugin` block. |
| `... undefined symbol: <C++ name>` | A method we declared in `ha_tidesdb.h` isn't actually defined in the .cc (rename mismatch from one of the override-strip rounds). |
| `Failed to register the storage engine` | `tidesdb_init_func()` returned non-zero. Check the error log for what TidesDB itself complained about. |
| `Got error 168 from storage engine` | `HA_ERR_GENERIC` — one of our stubs returned -1. Look at which method got called. |

## 2. Test runner — does it work?

`scripts/test-plugin.sh` walks `tests/phase2/` and runs each `.sql` file against a fresh mysqld, capturing output. If a `.expected` file exists alongside, the runner diffs and reports pass/fail.

```bash
# run everything
docker run --rm --user "$(id -u):$(id -g)" -v /home/corvin/TIDES:/work \
    tides-builder /work/scripts/test-plugin.sh

# run a subset by name pattern
docker run --rm --user "$(id -u):$(id -g)" -v /home/corvin/TIDES:/work \
    tides-builder /work/scripts/test-plugin.sh insert
```

Outputs land in `tests/results-YYYYMMDD-HHMMSS/`:
- `<name>.out` — captured mysql client output
- `<name>.diff` (only on fail) — unified diff vs `.expected`
- `_install.out` — output of the one-time `INSTALL PLUGIN`

### Generating .expected files

Each test currently has a `.sql` but no `.expected`. The intended workflow:

1. Run the test once: `./scripts/test-plugin.sh <name>` — output captured to `<name>.out`, marked `?` (no expected).
2. **Eyeball it.** If it looks right, copy it to `<name>.expected`:
    ```bash
    cp tests/results-*/05_insert_one_row.out tests/phase2/05_insert_one_row.expected
    ```
3. From then on, the runner enforces that exact output.

Don't auto-generate `.expected` files blindly — most tests will fail in informative ways before any of them produce *correct* output, and we don't want to lock in broken behavior.

### Phase 2 test list

Listed in expected order of difficulty — earlier tests are easier wins:

| # | File | What it exercises |
|---|---|---|
| 01 | `01_engine_listed.sql` | I_S.engines query |
| 02 | `02_show_engines.sql` | engine support level |
| 03 | `03_create_drop_int.sql` | `ha_tidesdb::create()` + `delete_table()` for INT-only table |
| 04 | `04_create_drop_varchar.sql` | same with VARCHAR (Field_varstring path) |
| 05 | `05_insert_one_row.sql` | `write_row()` (the row codec) |
| 06 | `06_insert_many_rows.sql` | `rnd_init/rnd_next/rnd_end` (table scan) |
| 07 | `07_pk_lookup.sql` | `index_read_map` (PK access) |
| 08 | `08_update_row.sql` | `update_row()` |
| 09 | `09_delete_row.sql` | `delete_row()` |
| 10 | `10_types_basic.sql` | INT/BIGINT/VARCHAR/TEXT codec |
| 11 | `11_null_handling.sql` | null-bitmap encode/decode |
| 12 | `12_transaction_commit.sql` | hton->commit |
| 13 | `13_transaction_rollback.sql` | hton->rollback |

Tests 01–04 should pass without any TidesDB I/O at all (they're pure metadata). 05+ exercises real storage. 12–13 require transaction registration to be wired through correctly — most likely to fail given how much we stubbed.

## 3. The full TideSQL MTR suite (later)

`vendor/tidesql/mysql-test/suite/tidesdb/` ships **61** MTR tests. To run them:

```bash
# one-time: copy the suite into MySQL's test tree
cp -r vendor/tidesql/mysql-test/suite/tidesdb mysql-server/mysql-test/suite/

# inside the container, run via mtr
docker run --rm --user "$(id -u):$(id -g)" -v /home/corvin/TIDES:/work \
    tides-builder bash -c "
        cd /work/mysql-server/build/mysql-test
        ./mtr --suite=tidesdb
    "
```

Most tests will fail until Phase 4 work re-enables encryption, spatial, FTS, and per-table options. Use `--do-test=tidesdb_crud` to filter for the basics first.

## Iteration loop

```bash
edit ha_tidesdb.cc / ha_tidesdb.h
  ↓
./scripts/build-all.sh           (rebuild plugin, ~30s)
  ↓
./scripts/test-plugin.sh <name>  (or smoke-test.sh)
  ↓
inspect tests/results-*/<name>.out
  ↓
fix and repeat
```
