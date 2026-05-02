# Port errors — pass 1 (after include-path fixes)

After fixing the 5 include-path divergences, the build progressed from "can't find handler.h" to actually compiling `ha_tidesdb.cc`. The first wave of compile errors gives us a real picture of the MariaDB→MySQL 9.7 porting cost.

**Headline numbers:**
- **950 total compiler diagnostics** (errors + warnings)
- **206 unique error messages** (after dedup)
- All originate from a single source file: `ha_tidesdb.cc` (and its `ha_tidesdb.h`)

The errors group cleanly into 7 categories. Each category has a typical "fix shape" — see Strategy at the bottom.

## Category 1 — Missing MariaDB-only types (foundational)

Pervasive — these types appear in many method signatures, so each one cascades into multiple errors.

| MariaDB type | Where used | MySQL equivalent |
|---|---|---|
| `range_id_t` | `multi_range_read_next()` | `char**` (MySQL passes a pointer to a `range_seq_t`-managed buffer) |
| `check_result_t` | return type of `check()` / `repair()` | `int` (MySQL uses plain `HA_ADMIN_*` constants) |
| `IO_AND_CPU_COST` | `scan_time()`, `read_time()` | `double` (MySQL still uses scalar costs) |
| `alter_table_operations` | `prepare_inplace_alter_table()` | `Alter_inplace_info::HA_ALTER_FLAGS` |
| `my_ptrdiff_t`, `my_bool` | low-level utility | `ptrdiff_t`, `bool` (MySQL ditched the typedefs) |
| `TYPELIB` | system var enums | exists in MySQL but in a different header location |

**Fix shape**: a small `tidesdb_compat.h` header with `using` aliases would cover types that have direct MySQL equivalents. For things like `IO_AND_CPU_COST` (a struct with two fields in MariaDB vs. a single double in MySQL) we have to actually rewrite the cost methods.

## Category 2 — Missing MariaDB-only constants/flags

Compile errors of the form `'X' was not declared in this scope`.

- **ALTER flags**: `ALTER_ADD_PK_INDEX`, `ALTER_DROP_PK_INDEX`, `ALTER_CHANGE_CREATE_OPTION` — MariaDB-specific naming. MySQL has the same concepts under `Alter_inplace_info::ADD_PK_INDEX` etc.
- **CHECK flags**: `CHECK_ABORTED_BY_USER`, `CHECK_NEG`, `CHECK_OUT_OF_RANGE` — MariaDB enum values; MySQL uses `HA_ADMIN_*`.
- **HA_CAN_* table flags**: `HA_CAN_ONLINE_BACKUPS`, `HA_CAN_TABLES_WITHOUT_ROLLBACK`, `HA_CAN_VIRTUAL_COLUMNS`, `HA_CLUSTERED_INDEX` — MariaDB extensions. MySQL has its own subset; we delete or re-map.
- **`TL_FIRST_WRITE`** — MariaDB-only `thr_lock` enum value.
- **`WARN_LEVEL_NOTE`** — MariaDB error-reporting level.

**Fix shape**: most of these can be `#define`d to MySQL equivalents in `tidesdb_compat.h`. A handful (the `HA_CAN_*` flags) just get removed from the handler's `table_flags()` return value — MySQL doesn't have those capabilities to advertise.

## Category 3 — Encryption API divergence

MariaDB exposes a public C API for at-rest encryption that storage engines call directly. MySQL handles encryption via the keyring service and a different plugin layer.

Missing in MySQL:
- `encryption_crypt`, `encryption_encrypted_length`
- `ENCRYPTION_FLAG_DECRYPT`, `ENCRYPTION_FLAG_ENCRYPT`
- `encryption_key_get`, `encryption_key_get_latest_version`
- `ENCRYPTION_KEY_VERSION_INVALID`

**Fix shape**: For Phase 2, **disable encryption support entirely**. Wrap the encryption code in `#ifdef HAVE_TIDESDB_ENCRYPTION` (we don't define it) and stub out the per-table `ENCRYPTION='Y'` option. Phase 4 work to wire it back up via MySQL's keyring service.

## Category 4 — Macro signature changes

- **`mysql_cond_init(key, cond, attr)` (MariaDB, 3 args) → `mysql_cond_init(key, cond)` (MySQL, 2 args)**: 1 occurrence in TideSQL — easy fix.
- **`DBUG_ASSERT` → `assert`**: MySQL 8.0+ deprecated `DBUG_ASSERT`. Replace site-by-site, or `#define DBUG_ASSERT(x) assert(x)` in the compat header.

## Category 5 — Handler virtual signature drift (the predicted divergence)

These methods exist in both trees but with different signatures, mostly because MySQL's handler API takes `dd::Table*` (data dictionary) parameters that MariaDB doesn't have.

| Method | MariaDB sig | MySQL 9.7 sig |
|---|---|---|
| `open` | `(name, mode, test_if_locked)` | `(name, mode, test_if_locked, const dd::Table*)` |
| `create` | `(name, TABLE*, HA_CREATE_INFO*)` | `(name, TABLE*, HA_CREATE_INFO*, dd::Table*)` |
| `delete_table` | `(name)` | `(name, const dd::Table*)` |
| `rename_table` | `(from, to)` | `(from, to, const dd::Table*, dd::Table*)` |
| `write_row` | `(const uchar*)` | `(uchar*)` (non-const) |
| `update_row` | `(const, const)` | `(const uchar*, uchar*)` |
| `start_bulk_insert` | `(rows, flags)` | `(rows)` |
| `multi_range_read_next` | `(range_id_t*)` | `(char**)` |
| `records_in_range` | `(idx, key_range*, key_range*, int*)` | `(idx, key_range*, key_range*)` |
| `prepare_inplace_alter_table` | uses `alter_table_operations` | uses `Alter_inplace_info::HA_ALTER_FLAGS` |
| `inplace_alter_table` | same | same |

**Fix shape**: edit each override in `ha_tidesdb.h` and the matching definition in `ha_tidesdb.cc`. Method bodies usually just need to ignore the new parameters (we don't yet implement DD integration). This is the biggest single chunk of mechanical work — probably 50–100 sites across both files.

## Category 6 — System variable / plugin descriptor types

`cannot convert 'SYS_VAR*' to 'st_mysql_sys_var*' in initialization`

MySQL's `tidesdb_system_variables` array expects `SYS_VAR*` (an alias for `st_mysql_sys_var*`); somewhere TideSQL is using the older spelling. Probably one fix at the array declaration. Same likely for `tidesdb_storage_engine`, `tidesdb_status_variables`, `tidesdb_table_option_list`.

## Category 7 — Spatial / GIS, full-text, and TidesDB-specific server vars

Lower-priority for Phase 2:
- `wkb_parse_geometry`, `spatial_parse_query_mbr`, `tdb_mbr_t` — TidesDB has spatial features TideSQL exposes via `ENGINE=TIDESDB`. We can `#ifdef`-out for now.
- `srv_unified_memtable`, `srv_pessimistic_locking`, `srv_print_all_conflicts` — these look like MariaDB-style server-variable backing storage. They may just need to be declared (they're our own statics — possibly we deleted them when removing `sql_priv.h`).
- `sql_print_information`, `sql_print_error`, `sql_print_warning` — MariaDB has these as free functions. MySQL replaced them with `LogErr(...)` / `error_log_print(...)` macros from `mysql/components/services/log_builtins.h`. ~9 occurrences.

## Strategy

The errors are not equally hard. Recommended attack order:

1. **Round 2 — compat header** (~1 day): create `storage/tidesdb/tidesdb_compat.h`. Add `using` aliases / `#define`s for everything in Categories 1, 2, 4 that has a clean MySQL equivalent. Remap `DBUG_ASSERT`, fix `mysql_cond_init`. Estimated to clear ~300 of the 950 diagnostics.

2. **Round 3 — disable advanced features** (~half day): wrap encryption (Category 3), spatial (Category 7), and `HA_CAN_ONLINE_BACKUPS`/`HA_CAN_VIRTUAL_COLUMNS` flag advertisements behind `#ifdef HAVE_TIDESDB_ADVANCED` and don't define it. We re-enable in Phase 4.

3. **Round 4 — handler signatures** (~3–5 days): edit every override in Category 5 to match MySQL's `handler.h`. Mechanical, repetitive. The compiler is your checklist.

4. **Round 5 — log / sysvar / final cleanup** (~1 day): replace `sql_print_*` with `LogErr`, fix `SYS_VAR*` declaration, fix the few remaining straggler errors.

**Realistic timeline to first clean compile**: 1–2 weeks focused.
**Time to first `INSERT`/`SELECT` round-trip**: another 1–2 weeks (getting the methods to do something correct, not just compile).
**Time to passing the lifted MTR suite**: another 1–2 months.

This matches the prior estimate ("compile clean: 3–7 days; CRUD round-trip: 2–3 weeks; production: 2–4 months"). The 950-error number sounds scary but ~half of it is cascading from a small set of missing types in Category 1.
