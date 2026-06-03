/*
  Atomic-DDL participation surface for the TidesDB-MySQL plugin.

  Closes A-5 from docs/code-review-report.md. See
  docs/superpowers/specs/2026-06-02-atomic-ddl-participation-design.md
  for the design rationale.
*/
#pragma once

#include <cstdint>
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

/* Sysvar enum values. */
enum class OrphanAction { Drop, Quarantine, LogOnly };

extern OrphanAction g_orphan_action;
extern bool g_atomic_ddl_strict;

}  // namespace tidesdb_mysql
