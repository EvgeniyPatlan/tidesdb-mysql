# Atomic-DDL participation — design spec

| | |
|---|---|
| **Author** | brainstorm session, 2026-06-02 |
| **Status** | Design — awaiting implementation plan |
| **Target release** | v0.4.0 |
| **Closes** | A-5 from `docs/code-review-report.md` |
| **Predecessor** | `2026-06-02-txn-lifecycle-refactor-design.md` (SUPERSEDED — was targeting already-closed findings) |

## 1. Summary

Wire the TidesDB-MySQL plugin into MySQL 9.7's atomic-DDL contract end-to-end so crash-during-DDL no longer leaves the data dictionary and the TidesDB column-family list divergent, and so engine-private schema metadata is portable via SDI.

Sets `HTON_SUPPORTS_ATOMIC_DDL`, wires all six SDI tablespace callbacks against a dedicated `__tidesdb_sdi` metadata column family, registers a single engine-wide logical tablespace `tidesdb_system`, persists schema metadata in `dd::Table::se_private_data` during CREATE / inplace ALTER, runs an eager startup reconciliation sweep that finds and quarantines orphan CFs, and adds the eight DDSE callback stubs so a future "TidesDB hosts the data dictionary" project has the contract surface in place.

Ships as v0.4.0 in one release (big-bang).

## 2. Why now

The original code-review report's A-5 architectural item is the only major piece from that review that hasn't been at least partially executed. The other big-ticket extractions (A-1 row-lock manager, A-2 FTS + spatial, A-4 EngineContext, A-7 inplace ALTER state machine) have all landed as separate translation units, and the inplace ALTER state machine is the natural seat for the dd::Table * parameter consumption that atomic-DDL participation requires — half the wiring point is already in place.

Without atomic-DDL participation, the documented gap is:

> *MySQL 9.7 expects storage engines to participate in atomic DDL by writing SE-private data into the data dictionary. Without this, crash-during-DDL behavior is undefined: TidesDB CF list and the MySQL DD can diverge, leaving orphan CFs or orphan DD rows.* (`docs/code-review-report.md` A-5)

In production, this manifests as: a crash between the engine's CF create and the DD txn commit leaves an unreferenced CF; a crash between the DD commit and the engine drop leaves a dd::Table for which there is no backing CF. The plugin has no automated recovery from either.

## 3. Scope

**In scope for v0.4.0 (one release, big-bang):**

1. Set `HTON_SUPPORTS_ATOMIC_DDL` on `tidesdb_hton->flags`.
2. Persist `dd::Table::se_private_data` in `ha_tidesdb::create()` and `commit_inplace_alter_table()`; read it on `ha_tidesdb::open()`.
3. Integrate with the existing inplace ALTER state machine — the four virtuals consume their currently-`[[maybe_unused]]` `dd::Table *` parameters.
4. Register a single logical tablespace `tidesdb_system` at plugin init via `Plugin_tablespace`.
5. Wire all six SDI callbacks (`sdi_create`, `sdi_drop`, `sdi_get_keys`, `sdi_get`, `sdi_set`, `sdi_delete`) against a dedicated `__tidesdb_sdi` metadata column family.
6. Wire all eight DDSE callbacks (`ddse_dict_init`, `dict_init`, `dict_recover`, `dict_cache_reset`, `dict_cache_reset_tables_and_tablespaces`, `dict_get_server_version`, `dict_set_server_version`, `is_dict_readonly`) as no-op stubs that log INFO once per server lifetime.
7. Eager startup reconciliation sweep that walks the DD and the CF list, computes the symmetric difference, and applies the recovery action per `tidesdb_orphan_action` sysvar.
8. Two new sysvars: `tidesdb_orphan_action` (enum: `drop`, `quarantine`, `log_only`; default `quarantine`) and `tidesdb_atomic_ddl_strict` (bool; default `ON`).
9. Twenty MTR tests under the `tidesdb_ddl_*` prefix.
10. Validation report; existing 50+ MTR tests + mwbench + WARE=100 must all continue to pass.

**Out of scope (deferred to v0.5.0 or later):**

- Making TidesDB the active data-dictionary storage engine. The DDSE stubs are wired but unused; flipping that switch is a separate, much larger project.
- Two-phase commit between the DD and the TidesDB txn at the final commit boundary. The same atomicity gap exists for InnoDB; fully closing it requires a server-side XA-style protocol that's not in MySQL 9.7's contract. Documented as a known limitation.
- Full `mysqldump --tab` integration test (smoke test only in v0.4.0; full integration in v0.5.0).
- Per-database or per-table tablespace abstractions; single engine-wide tablespace is final.

## 4. The handlerton surface acquired

| Slot | Today | After v0.4.0 |
|---|---|---|
| `HTON_SUPPORTS_ATOMIC_DDL` | unset | set |
| `sdi_create` | unset | `SdiStore`-backed; no-op for non-`tidesdb_system` tablespaces |
| `sdi_drop` | unset | `SdiStore`-backed |
| `sdi_get_keys` | unset | `SdiStore::list_keys` |
| `sdi_get` | unset | `SdiStore::get` |
| `sdi_set` | unset | `SdiStore::put` |
| `sdi_delete` | unset | `SdiStore::del` |
| `ddse_dict_init` | unset | stub; INFO log, returns success |
| `dict_init` | unset | stub |
| `dict_recover` | unset | stub |
| `dict_cache_reset` | unset | stub |
| `dict_cache_reset_tables_and_tablespaces` | unset | stub |
| `dict_get_server_version` | unset | stub |
| `dict_set_server_version` | unset | stub |
| `is_dict_readonly` | unset | stub; returns `false` |

Source-of-truth signatures for each callback are in `mysql-server/sql/handler.h`:2925–2985.

## 5. Components

### `plugin/tidesdb_atomic_ddl.{cc,h}` — new translation unit (~600 lines)

**`class SdiStore`** — wraps the `__tidesdb_sdi` metadata column family.

```cpp
class SdiStore {
public:
    explicit SdiStore(tidesdb_t *engine);
    bool init();                                        // creates __tidesdb_sdi if absent
    bool put(const sdi_key_t &k, const void *blob, uint64 len);
    bool get(const sdi_key_t &k, void *out, uint64 *len); // out=nullptr → size probe
    bool del(const sdi_key_t &k);
    bool list_keys(sdi_vector_t &out);
private:
    tidesdb_column_family_t *cf_;
    static std::string pack_key(const sdi_key_t &k);    // [u32 sdi_type][u64 sdi_id]
};
```

The metadata CF uses a fixed schema: `[u32 sdi_type] [u64 sdi_id]` byte-keys → JSON blob values. Keys are byte-comparable so iterator-based enumeration via TidesDB's range scan gives `list_keys` for free.

**`class DdSyncReconciler`** — startup sweep, pure-function delta + apply phases.

```cpp
struct ReconcileDelta {
    std::vector<std::string> orphan_cfs;        // CF exists, no dd::Table
    std::vector<std::string> orphan_dd_tables;  // dd::Table exists, no CF
};

class DdSyncReconciler {
public:
    DdSyncReconciler(tidesdb_t *engine, dd::cache::Dictionary_client *dc);
    ReconcileDelta compute_delta();             // pure; unit-testable
    bool apply_delta(const ReconcileDelta &d);  // drop/quarantine; log
};
```

`compute_delta` enumerates `dd::Table` rows filtered by `ENGINE=TIDESDB` and the TidesDB CF list (excluding `__tidesdb_sdi` and `__orphan_*`), produces the symmetric difference. Side-effect-free so unit tests can supply mocked enumerators.

`apply_delta` honours `tidesdb_orphan_action`:

| Sysvar value | For orphan_cfs |
|---|---|
| `drop` | `tidesdb_column_family_drop(cf)` |
| `quarantine` | rename to `__orphan_<epoch>_<cf>` |
| `log_only` | `sql_print_warning` only |

For `orphan_dd_tables`, always `log_only` — the sweep never touches the DD.

**`class TidesdbAtomicDdlBridge`** — extends create/drop/alter paths.

- `prepare_create(THD*, dd::Table*)` — called from `ha_tidesdb::create()`. Performs the CF allocation inside the active TidesDB txn; computes **schema fingerprint** (SHA-256 over a canonical column-list serialisation: for each column, the bytewise concatenation of `name|type|length|nullable|character_set_id|collation_id` in column-position order) and **options checksum** (CRC32 over the engine-attribute JSON after rapidjson normalisation); writes the `se_private_data` Properties; emits SDI via `SdiStore::put`.
- `prepare_drop(THD*, const dd::Table*)` — called from `ha_tidesdb::delete_table()`. Marks the CF for drop-on-commit; deletes SDI via `SdiStore::del`.
- `register_post_ddl_cleanup(THD*, std::function<void(bool committed)>)` — not strictly required since engine-side mutations already flow through the existing handlerton `commit`/`rollback` hooks; included for forward extensibility (e.g., SDI emission of complex changes that span multiple statements).

**`struct DdseStubs`** — eight DDSE callbacks.

Each follows this shape:

```cpp
static bool tidesdb_dict_init(dict_init_mode_t, uint, List<const Plugin_table>*,
                              List<const Plugin_tablespace>*) {
    log_ddse_stub_once("dict_init");
    return false;  /* false = success in handlerton convention */
}
```

A single `log_ddse_stub_once(const char *name)` helper uses a `std::atomic<uint32_t> seen_stubs_bitmask` so each callback logs at most once per server lifetime.

### `plugin/tidesdb_inplace_alter.cc` — extensions (~+110 lines)

The four virtuals consume their `dd::Table *` parameters:

- **`prepare_inplace_alter_table`** — read `old_table_def->se_private_data()` into context `ctx.old_se_private`; precompute new se_private_data into `ctx.new_se_private` (CF name unchanged for inplace; fingerprint updated; schema_version incremented). Do not write yet.
- **`inplace_alter_table`** — unchanged signature; reads from `ctx.stage` as today.
- **`commit_inplace_alter_table(commit=true)`** — after the existing CF-swap phase: write `new_table_def->set_se_private_data(ctx.new_se_private)`; emit `SdiStore::put(SDI_TYPE_TABLE, dd::Table.id, new_json)`.
- **`commit_inplace_alter_table(commit=false)`** — after the existing rollback phase: leave `new_table_def` untouched; no SDI emit.

### `plugin/ha_tidesdb.cc` — call-site changes (~+110 lines)

- **`tidesdb_init_func`** (existing line 2592): after `g_engine_ctx.engine.store(engine, ...)`, instantiate `SdiStore`; run `DdSyncReconciler`; register `Plugin_tablespace`.
- **Handlerton flags line** (existing line 2610): `tidesdb_hton->flags = HTON_SUPPORTS_ENGINE_ATTRIBUTE | HTON_SUPPORTS_ATOMIC_DDL;`.
- **New callback wiring block** (~line 2615): bind all six `sdi_*` callbacks to `SdiStore` adapters; bind all eight `dict_*` callbacks to `DdseStubs`.
- **`ha_tidesdb::create()`**: call `TidesdbAtomicDdlBridge::prepare_create(thd, table_def)`.
- **`ha_tidesdb::delete_table()`**: call `TidesdbAtomicDdlBridge::prepare_drop(thd, table_def)`.
- **`ha_tidesdb::open()`**: read `table_def->se_private_data()` early; validate the persisted CF name binding matches the path-derived name; fail with `ER_TABLEACCESS_DENIED_ERROR` if mismatched (`tidesdb_atomic_ddl_strict=ON`) or fall back to path-derived inference with WARNING (`OFF`).

### `plugin/tidesdb_engine_context.h` — extensions (~+60 lines)

```cpp
struct EngineCtx {
    std::atomic<tidesdb_t*> engine{nullptr};
    std::unique_ptr<SdiStore> sdi;
    const Plugin_tablespace *tablespace{nullptr};
};
extern EngineCtx g_engine_ctx;
```

### New sysvars

| Name | Type | Default | Behaviour |
|---|---|---|---|
| `tidesdb_orphan_action` | enum `{drop, quarantine, log_only}` | `quarantine` | Recovery-sweep behaviour for orphan CFs |
| `tidesdb_atomic_ddl_strict` | bool | `ON` | Refuse to open a table whose `se_private_data` is malformed or absent (ON) vs warn and infer (OFF) |

### File-size deltas

| File | Lines before | Lines after | Δ |
|---|---|---|---|
| `ha_tidesdb.cc` | 8,688 | ~8,800 | +~110 |
| `tidesdb_inplace_alter.cc` | 850 | ~960 | +~110 |
| `tidesdb_engine_context.{cc,h}` | 57 | ~120 | +~60 |
| `tidesdb_atomic_ddl.{cc,h}` | — | ~600 | new TU |

## 6. Data flow

### CREATE TABLE

```
SQL layer               plugin/                            TidesDB engine
---------               -------                            --------------
CREATE TABLE t (...)
  → ha_tidesdb::create()
       1. get or create THD's tidesdb_trx_t
       2. tidesdb_txn_column_family_create(txn, "t", opts)
                                                ──► CF staged within txn
       3. compute schema fingerprint + options checksum
       4. table_def->set_se_private_data(
            cf_name=t  fingerprint=<sha256>
            options_csum=<crc32>  atomic_ddl=1
            created_at=<epoch>)
       5. SdiStore::put(SDI_TYPE_TABLE, table_def->id(), json_blob)
                                                ──► JSON written into
                                                    __tidesdb_sdi CF in
                                                    same txn
       6. return success → SQL layer COMMITs DD txn → TidesDB txn commits
  ← committed or rolled back; both halves follow the same fate
```

### DROP TABLE

```
DROP TABLE t
  → ha_tidesdb::delete_table()
       1. open the DDL txn
       2. tidesdb_txn_column_family_drop(txn, "t")     ──► CF marked for drop in txn
       3. SdiStore::del(SDI_TYPE_TABLE, table_def->id())
                                                       ──► SDI blob deleted in txn
       4. se_private_data is auto-deleted with the dd::Table row
       5. return success → SQL layer COMMITs DD txn → TidesDB txn commits
  ← on rollback, both undone together
```

### ALTER TABLE — inplace

```
ALTER TABLE t MODIFY col_a BIGINT
  → check_if_supported_inplace_alter(...)                            (unchanged)
  → prepare_inplace_alter_table(old_def, new_def)
       1. existing inplace-context setup
       2. NEW: ctx.old_se_private = old_def->se_private_data()
       3. NEW: ctx.new_se_private = precomputed new blob (CF name unchanged,
               fingerprint updated, schema_version++)
  → inplace_alter_table(old_def, new_def)                            (unchanged)
  → commit_inplace_alter_table(old_def, new_def, commit)
       commit == true:
            1. existing CF-swap phase                  ──► CF state updated in txn
            2. NEW: new_def->set_se_private_data(ctx.new_se_private)
            3. NEW: SdiStore::put(SDI_TYPE_TABLE, new_def->id(), new_json)
            4. return success → DD txn COMMITs → engine txn commits
       commit == false:
            1. existing rollback phase
            2. NEW: do nothing to new_def
            3. NEW: no SdiStore call
            4. return success → DD txn ROLLBACKs → engine txn rolls back
```

### Startup recovery sweep

```
mysqld start
  → tidesdb_init_func()
       1. existing engine open + EngineCtx wiring                    (unchanged)
       2. SdiStore::init()         — creates __tidesdb_sdi if absent
       3. NEW: DdSyncReconciler::compute_delta()
            walk dd::Catalog::tables() filtering ENGINE=TIDESDB
              → set of expected CF names
            walk tidesdb_list_column_families()
              → set of actual CF names (excluding __tidesdb_sdi and __orphan_*)
            delta.orphan_cfs       = actual − expected
            delta.orphan_dd_tables = expected − actual
       4. NEW: DdSyncReconciler::apply_delta(delta)
            per tidesdb_orphan_action
       5. existing handlerton registration completes                 (unchanged)
```

### Atomic-DDL signal flow

The crucial property: TidesDB's existing `commit` / `rollback` handlerton hooks already propagate the verdict from the DD transaction to the TidesDB transaction. `HTON_SUPPORTS_ATOMIC_DDL` just informs the server it can rely on this propagation. **No separate undo log or pending-DDL queue is needed.**

## 7. Error handling

### Failure-mode matrix

| Stage | Failure | Recovery action | Visible outcome |
|---|---|---|---|
| `ha_tidesdb::create` step 2 | CF allocation fails | Return `HA_ERR_GENERIC`; engine txn rolls back; DD txn rolls back | `ER_GET_ERRNO`; no orphan state |
| `ha_tidesdb::create` step 5 | SDI write fails | Return `HA_ERR_GENERIC`; engine txn rolls back including the CF from step 2 | `ER_GET_ERRNO`; no orphan state |
| `commit_inplace_alter_table(commit=true)` SDI emit | `SdiStore::put` fails | Return true (error); engine txn rolls back including the CF mutation; `new_def->se_private_data` never written | ALTER fails atomically; original table observable |
| `delete_table` step 3 | SDI delete fails | Return `HA_ERR_GENERIC`; engine txn rolls back, un-marking the CF for drop | DROP fails; table still present |
| DD txn commits but engine txn commit hook fails | TidesDB txn commit raises after DD has committed | Same risk InnoDB has; full fix needs 2PC outside the contract. **Out of scope for v0.4.0; documented as known limitation. Recovery sweep on next startup reconciles.** | Operator sees inconsistent state until restart |
| `DdSyncReconciler::compute_delta` enumeration fails | DD cache enumeration error | Log at ERROR; abort the sweep; plugin init continues with no reconciliation | WARNING in error log; nothing reaped |
| `DdSyncReconciler::apply_delta` CF drop fails | `tidesdb_column_family_drop` fails for a single CF | Log per-CF failure at WARNING; continue with next CF (sweep is best-effort) | Orphan persists for next startup |
| `ha_tidesdb::open` se_private_data validation | binding mismatch or malformed blob | `strict=ON` → `ER_TABLEACCESS_DENIED_ERROR` with detailed message; `strict=OFF` → WARNING and proceed with path-inferred CF name | Operator-controllable |
| DDSE stub invoked | shouldn't happen (TidesDB not active DDSE) | Log INFO once; return `false` (success); MySQL proceeds | Diagnostic noise only |

### Edge cases

1. **Plugin loaded → tables created → plugin unloaded → plugin reloaded.** Recovery sweep runs again; SDI store re-instantiates against the existing `__tidesdb_sdi` CF; existing CFs are matched against persisted `dd::Table` entries with persisted `se_private_data`. **Result: no-op sweep, no false orphan reports.** This is the load-bearing test for the sweep.

2. **v0.3.x table opened by v0.4.0.** `se_private_data` is absent or empty. `ha_tidesdb::open` enters the `strict=OFF` path: WARNING logged, CF binding inferred from path. SDI is *not* retrofitted on open. SDI emission triggers on the next ALTER. Operators who want SDI on legacy tables can run `ALTER TABLE t ENGINE=TIDESDB` (no-op ALTER) to trigger emission. Documented in upgrade notes.

3. **Crash during CREATE TABLE between step 2 and step 5.** TidesDB txn has CF-create staged but not committed. DD txn has not committed either. On restart: both roll back automatically. **No orphan.**

4. **Crash during CREATE TABLE between TidesDB txn commit and DD txn commit.** The "real" atomic-DDL gap. CF exists but no `dd::Table`. On restart: recovery sweep finds the orphan CF and quarantines/drops per `tidesdb_orphan_action`. **Recoverable, with audit trail.**

5. **Crash during ALTER TABLE inplace_alter_table.** Engine txn was modifying the CF; both halves roll back on restart. Original CF state preserved (TidesDB's own crash recovery handles this). `se_private_data` never written. **No partial state visible.**

6. **SDI tablespace API called for a tablespace the engine doesn't own.** `sdi_get_keys` etc. take a `dd::Tablespace`. The plugin only owns `tidesdb_system`. If called for a different tablespace, the callback returns `false` (success) with an empty result. Caller-side responsibility per MySQL's contract.

7. **`mysqldump --tab` round-trip with SDI.** Smoke test only in v0.4.0 (emit SDI, read it back, assert JSON parses); full mysqldump integration deferred to v0.5.0.

### Logging discipline

- Every callback path that returns an error logs once at the appropriate level (ERROR for engine-state-affecting, WARNING for recoverable, INFO for diagnostic).
- All user-supplied strings go through `tdb_sanitize_for_log` (the MF-6 helper, already in tree).
- DDSE stub callbacks log at INFO **once per server lifetime per callback** via `std::atomic` bitmask to avoid spam.

## 8. Testing

### MTR — 20 new tests under `tidesdb_ddl_*`

**Atomic semantics (5 tests):**

| Test | What it verifies |
|---|---|
| `tidesdb_ddl_atomic_create_commit` | CREATE commits → CF exists, se_private_data parses, SDI retrievable |
| `tidesdb_ddl_atomic_create_rollback` | CREATE inside an explicit `BEGIN` forced to roll back → no CF, no SDI, no dd::Table |
| `tidesdb_ddl_atomic_drop_commit` | DROP commits → CF gone, SDI gone |
| `tidesdb_ddl_atomic_drop_rollback` | DROP rolled back → CF and SDI restored |
| `tidesdb_ddl_atomic_alter_commit` | ALTER ADD COLUMN inplace; se_private_data updated; SDI re-emitted with new schema |

**Crash recovery (3 tests, `debug_sync` + the existing SIGKILL harness):**

| Test | Crash point | Expected state after restart |
|---|---|---|
| `tidesdb_ddl_crash_during_create` | Between CF create and dd::Table commit | Recovery sweep finds orphan CF; quarantines or drops per sysvar |
| `tidesdb_ddl_crash_during_drop` | Between dd::Table commit and CF drop | Orphan dd::Table logged at WARNING; no data loss |
| `tidesdb_ddl_crash_during_alter` | Mid-`inplace_alter_table` | Original CF state restored; se_private_data unchanged; no SDI emit |

**Recovery sweep behaviour (4 tests):**

| Test | Scenario | Assertion |
|---|---|---|
| `tidesdb_ddl_sweep_noop` | Clean shutdown, restart | No delta found; INFO log shows zero deltas |
| `tidesdb_ddl_sweep_orphan_cf_quarantine` | Manually create CF, restart with `quarantine` | CF renamed to `__orphan_<epoch>_<cf>`, WARNING logged |
| `tidesdb_ddl_sweep_orphan_cf_drop` | Same with `drop` | CF dropped |
| `tidesdb_ddl_sweep_orphan_dd_table` | Drop CF manually while server is down, restart | dd::Table preserved, WARNING logged with operator instructions |

**SDI surface (4 tests):**

| Test | What it verifies |
|---|---|
| `tidesdb_ddl_sdi_get_after_create` | `sdi_get_keys` enumerates the new table's SDI; `sdi_get` returns parseable JSON |
| `tidesdb_ddl_sdi_round_trip` | Generated SDI JSON parses to a `dd::Table` equivalent structure |
| `tidesdb_ddl_sdi_after_alter` | Post-ALTER SDI reflects new schema; old key absent |
| `tidesdb_ddl_sdi_other_tablespace` | sdi_get called for a non-TidesDB tablespace returns empty (contract compliance) |

**DDSE stubs (1 test):**

| Test | What it verifies |
|---|---|
| `tidesdb_ddl_ddse_stubs_inert` | dict_init/dict_recover/dict_cache_reset invocations (forced via debug build hook) return success; INFO log line written exactly once per callback |

**Backward compatibility (2 tests):**

| Test | What it verifies |
|---|---|
| `tidesdb_ddl_legacy_open_strict_off` | Pre-v0.4.0 table (manually-zeroed se_private_data) opens with WARNING |
| `tidesdb_ddl_legacy_open_strict_on` | Same table fails to open with `ER_TABLEACCESS_DENIED_ERROR`; no-op ALTER promotes to v0.4.0 format |

**Sysvar (1 test):**

| Test | What it verifies |
|---|---|
| `tidesdb_ddl_sysvar_basic` | `tidesdb_orphan_action` and `tidesdb_atomic_ddl_strict` accept enum/bool values; SET GLOBAL works; invalid values rejected |

### Unit tests

- `DdSyncReconciler::compute_delta` — pure function. Mocked DD enumeration + mocked CF list. Six cases: empty/empty, only-dd, only-cf, perfect-match, mixed-with-orphan-cf, mixed-with-orphan-dd.
- `SdiStore::pack_key` — round-trips a `sdi_key_t` through pack → cf-key → unpack.

### Integration with existing harnesses

- **Existing 50+ MTR tests** — all must continue to pass. The atomic-DDL flag flip is the primary regression detector.
- **mwbench** — integrity test should pass unchanged; if it doesn't, atomic-DDL wiring is wrong somewhere in the commit/rollback path.
- **WARE=100 throughput** — per-DDL changes are zero-cost on DML paths; this gate should regress by ≤ 1%.

### Acceptance criteria for v0.4.0

1. All 20 new MTR tests pass on the release build and the ASAN+UBSAN build.
2. All existing 50+ MTR tests continue to pass.
3. mwbench integrity completes (0 corruption, 0 misses) at 100 GiB.
4. WARE=100 throughput within 1% of v0.3.1 baseline.
5. Validation report drafted per the existing release workflow.

## 9. Known limitations and follow-ups

- **DD-commit / engine-commit two-phase gap.** Same risk InnoDB has. Recovery sweep makes it recoverable but not invisible. Full fix requires server-side 2PC and is out of scope.
- **DDSE stubs are not exercised in v0.4.0.** Wired for future capability; no caller drives them today.
- **mysqldump --tab full integration deferred to v0.5.0.** Smoke test only in v0.4.0.
- **Legacy v0.3.x tables don't auto-emit SDI on open.** Operator must run a no-op ALTER. Documented in upgrade notes.

## 10. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| `HTON_SUPPORTS_ATOMIC_DDL` flag flip surfaces hidden assumption in DML paths | medium | break existing tests | Full MTR sweep + mwbench gate before tag |
| Recovery sweep mis-identifies a legitimate CF as orphan (e.g. `__tidesdb_sdi` or `__orphan_*`) | low | data loss if `tidesdb_orphan_action=drop` | Default to `quarantine`; explicit exclude list; dedicated MTR test for noop case |
| `se_private_data` validation in `open()` fails on tables created with edge-case CHARACTER SETs or COLLATIONs that affect fingerprint computation | medium | tables become unopenable | Strict mode default ON but operator can flip to OFF; documented escape hatch |
| SDI metadata CF grows unbounded with table churn | low | disk usage | SDI delete on DROP TABLE; compaction handles tombstones |
| DDSE stubs return wrong value, confusing MySQL's bootstrap | low | server fails to start | Stubs are no-ops returning success; bootstrap doesn't actually invoke them when InnoDB is the DDSE |

## 11. References

- `mysql-server/sql/handler.h` :2925–2985 — handlerton atomic-DDL/SDI/DDSE callback signatures
- `mysql-server/storage/innobase/handler/ha_innodb.cc` :3295, :3970–4030, :5414–5447 — InnoDB's wiring (canonical reference implementation)
- `mysql-server/sql/dd/types/{table,index,column}.h` — `se_private_data` accessor patterns
- `docs/code-review-report.md` § A-5 — original architectural finding
- `plugin/tidesdb_inplace_alter.cc` — existing inplace ALTER state machine (the wiring point)
- `plugin/tidesdb_engine_context.{cc,h}` — existing EngineContext (extended to host SdiStore)

## 12. Implementation sequencing (for the plan)

The plan should sequence the work so each step ships a green test pass:

1. Skeleton: `tidesdb_atomic_ddl.{cc,h}` with empty `SdiStore` / `DdSyncReconciler` / `DdseStubs`; build and link cleanly; no behaviour change.
2. `SdiStore` implementation + the `__tidesdb_sdi` CF init path; unit tests for `pack_key`.
3. Plugin_tablespace registration; integration test that confirms `SHOW TABLESPACES` lists `tidesdb_system`.
4. DDSE stubs wiring; `tidesdb_ddl_ddse_stubs_inert` MTR test green.
5. SDI callback adapter wiring; `tidesdb_ddl_sdi_*` MTR tests green.
6. `se_private_data` persistence in `ha_tidesdb::create()`; `tidesdb_ddl_atomic_create_*` MTR tests green.
7. `se_private_data` validation in `ha_tidesdb::open()`; backward-compat MTR tests green.
8. `delete_table` integration; `tidesdb_ddl_atomic_drop_*` MTR tests green.
9. Inplace ALTER state-machine integration; `tidesdb_ddl_atomic_alter_*` MTR tests green.
10. `DdSyncReconciler` `compute_delta` + unit tests.
11. `DdSyncReconciler::apply_delta` + sysvar wiring; recovery-sweep MTR tests green.
12. Crash-during-DDL MTR tests green.
13. `HTON_SUPPORTS_ATOMIC_DDL` flag flip — last because it activates the contract end-to-end. Steps 1–12 build out the SDI / `se_private_data` / sweep machinery while the flag is still off; each component is individually tested. Step 13 tells the server "you can trust the engine's commit/rollback to honour atomic-DDL semantics" — the engine's commit/rollback hooks already propagate verdicts, so the flag flip is the activation, not new behaviour. Full MTR + mwbench + WARE=100 sweep gates the merge to main.
14. Validation report; tag v0.4.0; docker release per the existing workflow.
