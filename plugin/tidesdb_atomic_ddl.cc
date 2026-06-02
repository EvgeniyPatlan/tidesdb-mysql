/*
  Atomic-DDL participation surface for the TidesDB-MySQL plugin.

  Closes A-5 from docs/code-review-report.md. See
  docs/superpowers/specs/2026-06-02-atomic-ddl-participation-design.md
  for the design rationale.

  This translation unit is the skeleton landing pad. All entry points are
  empty (return false / empty / true as appropriate for "not yet wired"
  semantics). Subsequent tasks in the atomic-DDL plan fill in the
  implementations one piece at a time.
*/

#include "tidesdb_atomic_ddl.h"

#include "sql/handler.h"
#include "sql/plugin_table.h"

namespace tidesdb_mysql {

OrphanAction g_orphan_action = OrphanAction::Quarantine;
bool g_atomic_ddl_strict = true;

/* -------------------- SdiStore -------------------- */

SdiStore::SdiStore(tidesdb_t *engine) : engine_(engine), cf_(nullptr) {}
SdiStore::~SdiStore() = default;

bool SdiStore::init() { return false; }
bool SdiStore::put(const sdi_key_t &, const void *, uint64_t) { return false; }
bool SdiStore::get(const sdi_key_t &, void *, uint64_t *) { return false; }
bool SdiStore::del(const sdi_key_t &) { return false; }
bool SdiStore::list_keys(sdi_vector_t &) { return false; }
std::string SdiStore::pack_key(const sdi_key_t &) { return {}; }

/* -------------------- DdSyncReconciler -------------------- */

DdSyncReconciler::DdSyncReconciler(tidesdb_t *engine, dd::cache::Dictionary_client *dc)
    : engine_(engine), dc_(dc) {}

ReconcileDelta DdSyncReconciler::compute_delta() { return {}; }
bool DdSyncReconciler::apply_delta(const ReconcileDelta &) { return true; }

/* -------------------- TidesdbAtomicDdlBridge -------------------- */

bool TidesdbAtomicDdlBridge::prepare_create(THD *, dd::Table *, const char *) { return true; }
bool TidesdbAtomicDdlBridge::prepare_drop(THD *, const dd::Table *) { return true; }

/* -------------------- DdseStubs -------------------- */

void DdseStubs::register_into(handlerton *) {}

/* -------------------- SdiCallbacks -------------------- */

void SdiCallbacks::register_into(handlerton *) {}

/* -------------------- Tablespace -------------------- */

const Plugin_tablespace *register_tablespace() { return nullptr; }

}  // namespace tidesdb_mysql
