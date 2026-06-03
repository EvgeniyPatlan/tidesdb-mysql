/*
  Atomic-DDL participation surface for the TidesDB-MySQL plugin.

  Closes A-5 from docs/code-review-report.md. See
  docs/superpowers/specs/2026-06-02-atomic-ddl-participation-design.md
  for the design rationale.
*/
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "sql/handler.h" /* sdi_key_t, sdi_vector_t (top-level structs) */

extern "C" {
#include <tidesdb/db.h> /* tidesdb_t, tidesdb_column_family_t */
}

class THD;
struct handlerton;
class Plugin_tablespace;
namespace dd {
class Table;
class Tablespace;
class Object_table;
namespace cache {
class Dictionary_client;
}
}  // namespace dd

namespace tidesdb_mysql {

/* Logical tablespace name (the single engine-wide tablespace). */
constexpr const char *kTidesdbTablespace = "tidesdb_system";

/* The dedicated CF that stores SDI blobs. */
constexpr const char *kSdiCfName = "__tidesdb_sdi";

/* Key format for the SDI metadata CF: [u32 sdi_type][u64 sdi_id], big-endian. */
constexpr size_t kSdiPackedKeyLen = sizeof(uint32_t) + sizeof(uint64_t);

class SdiStore {
   public:
    explicit SdiStore(tidesdb_t *engine);
    ~SdiStore();

    /* Open or create the __tidesdb_sdi CF. Returns true on success. */
    bool init();

    bool put(const sdi_key_t &k, const void *blob, uint64_t len);
    /* If out == nullptr, treat as size probe: *len is set to required size. */
    bool get(const sdi_key_t &k, void *out, uint64_t *len);
    bool del(const sdi_key_t &k);
    bool list_keys(sdi_vector_t &out);

    /* Pure helper, exposed for unit testing. */
    static std::string pack_key(const sdi_key_t &k);

   private:
    tidesdb_t *engine_;
    tidesdb_column_family_t *cf_;
};

struct ReconcileDelta {
    std::vector<std::string> orphan_cfs;
    std::vector<std::string> orphan_dd_tables; /* qualified "db.table" */
};

/* Side-effect-free symmetric difference of expected (DD-derived) CFs
   vs actual (engine-derived) CFs. CFs whose name starts with '__' are
   ignored (covers the __tidesdb_sdi metadata CF and __orphan_<epoch>_*
   quarantined CFs). Exposed for unit testing; the production
   DdSyncReconciler::compute_delta() calls this after enumerating both
   sides. */
ReconcileDelta compute_delta_pure(const std::set<std::string> &expected_cfs,
                                   const std::set<std::string> &actual_cfs);

class DdSyncReconciler {
   public:
    DdSyncReconciler(tidesdb_t *engine, dd::cache::Dictionary_client *dc);

    /* Side-effect-free; unit-testable with mocked enumerators. */
    ReconcileDelta compute_delta();

    /* Applies orphan_action sysvar; logs orphan_dd_tables. Returns true on success. */
    bool apply_delta(const ReconcileDelta &d);

   private:
    tidesdb_t *engine_;
    dd::cache::Dictionary_client *dc_;
};

class TidesdbAtomicDdlBridge {
   public:
    /* Called from ha_tidesdb::create() after the share is set up. */
    static bool prepare_create(THD *thd, dd::Table *new_table_def, const char *cf_name);
    /* Called from ha_tidesdb::delete_table() before tidesdb_txn_column_family_drop. */
    static bool prepare_drop(THD *thd, const dd::Table *table_def);

    /* Called from ha_tidesdb::open() once the path-inferred CF name is
       known. Reads se_private_data persisted by prepare_create and either:
         - accepts (returns true) when cf_name matches path_inferred_cf,
         - tolerates with a WARNING when strict=OFF (legacy / forward-compat),
         - rejects with ER_TABLEACCESS_DENIED_ERROR when strict=ON and the
           binding is missing or mismatched.

       Returning false from validate_open causes ha_tidesdb::open() to
       fail the OPEN with HA_ERR_TABLE_DEF_CHANGED so the server treats
       the table as broken. A NULL table_def is treated as "skip
       validation" -- the server passes NULL on temp-table / fast-path
       opens that bypass the DD. */
    static bool validate_open(THD *thd, const dd::Table *table_def,
                              const char *path_inferred_cf);

    /* Recompute the serialized se_private_data for the post-ALTER schema.
       Called from prepare_inplace_alter_table to pre-stage the value that
       commit_inplace_alter_table will stamp onto new_table_def when the
       SQL layer signals commit=true.

       Preserves cf_name and created_at from the old DD (CF identity does
       not change for inplace ALTER); recomputes fingerprint and
       options_csum from the new schema; bumps schema_version by 1.

       Returns true on success; out_serialized then contains a raw_string()
       suitable for dd::Table::set_se_private_data. Returns false only if
       the Properties_impl could not be constructed (should not happen). */
    static bool recompute_se_private_data(const dd::Table &new_def,
                                          std::string &out_serialized);

#ifndef NDEBUG
    /* Debug-only: dump the most-recently-written se_private_data Properties
       to the error log via sql_print_information. Each entry prints as a
       single INFO line "[TIDESDB] se_private_data key=<k> val=<v>". Used
       by the tidesdb_dump_se_private_data DBUG hook in tidesdb_show_status
       so MTR can grep for the expected keys without needing to surface
       the DD's se_private_data through information_schema. Returns true
       if at least one line was emitted (i.e. prepare_create has been
       called at least once); false if no table has been created yet
       this server lifetime. */
    static bool debug_dump_last_se_private_data();
#endif
};

struct DdseStubs {
    /* Each callback is a static free function with the right signature; pointers
       are bound in ha_tidesdb.cc handlerton init. The implementation logs once
       and returns success. */
    static void register_into(handlerton *hton);
};

/* SDI callback adapters (bound to SdiStore via a free-function shim). */
struct SdiCallbacks {
    static void register_into(handlerton *hton);

#ifndef NDEBUG
    /* Debug-only: exercise the "other tablespace" branch of every SDI
       callback so MTR can prove the identity guard works without having
       to construct a full dd::Tablespace object. Each call must return
       false (success no-op) for a tablespace named anything except
       kTidesdbTablespace. Used from the
       'tidesdb_force_sdi_other_tablespace' DBUG hook in
       tidesdb_show_status. Returns true if every guard fired correctly. */
    static bool debug_invoke_other_tablespace();
#endif
};

/* Tablespace registration helper. Returns nullptr on failure. */
const Plugin_tablespace *register_tablespace();

/* Minimal SDI serializer for inplace ALTER commit.

   v0.4.0 scope: emits a compact rapidjson object with the engine name,
   schema_id, table name, and a per-column array of (name, type, nullable).
   The full dd::sdi::serialize integration (which also covers indexes,
   foreign keys, partitions, and DD versioning) is deferred to v0.5.0.

   Returns "{}" when t is NULL. */
std::string serialize_sdi(const dd::Table *t);

/* Sysvar enum values. */
enum class OrphanAction { Drop, Quarantine, LogOnly };

extern OrphanAction g_orphan_action;
extern bool g_atomic_ddl_strict;

}  // namespace tidesdb_mysql
