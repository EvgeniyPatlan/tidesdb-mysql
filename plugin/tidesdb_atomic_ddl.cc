/*
  Atomic-DDL participation surface for the TidesDB-MySQL plugin.

  Closes A-5 from docs/code-review-report.md. See
  docs/superpowers/specs/2026-06-02-atomic-ddl-participation-design.md
  for the design rationale.

  Status of the surface:
    * SdiStore         — Task 2: implemented against the __tidesdb_sdi CF
                         (LZ4, 16 MiB write buffer). Pure-helper pack_key
                         covered by gtest in plugin/tests/.
    * DdSyncReconciler — Task 3+: still stubs; orphan reconciliation lands
                         in a later task.
    * AtomicDdlBridge,
      DdseStubs,
      SdiCallbacks,
      register_tablespace() — Task 5: handlerton wiring lands later; these
                         remain no-ops for now.
*/

#include "tidesdb_atomic_ddl.h"

#include <cstring>
#include <ctime>

#include "sql/handler.h"
#include "sql/log.h" /* sql_print_error */
#include "sql/plugin_table.h"

namespace tidesdb_mysql {

OrphanAction g_orphan_action = OrphanAction::Quarantine;
bool g_atomic_ddl_strict = true;

/*
  TIDESDB_TTL_NONE is defined as (time_t)-1 in ha_tidesdb.h; we duplicate the
  literal here so this TU does not have to pull the whole plugin header in.
*/
static constexpr time_t kSdiTtlNone = static_cast<time_t>(-1);

/* -------------------- SdiStore -------------------- */

SdiStore::SdiStore(tidesdb_t *engine) : engine_(engine), cf_(nullptr) {}
SdiStore::~SdiStore() = default;

/*
  Open the dedicated __tidesdb_sdi CF; create it (with LZ4 compression and a
  small 16 MiB write-buffer/flush threshold) if it does not already exist.

  Idempotent: safe to call on every server start. Returns true on success.

  Deviation from spec: TidesDB v9.3.2 has no "open_or_create" entry point and
  no `flush_threshold_bytes` config field. We use the existing-CF probe +
  create pattern (mirrors ha_tidesdb.cc's schema_cf bring-up) and set
  `write_buffer_size`, which is the closest equivalent.
*/
bool SdiStore::init() {
    if (!engine_) return false;

    /* If the CF already exists from a previous run, just bind to it. */
    cf_ = tidesdb_get_column_family(engine_, kSdiCfName);
    if (cf_) return true;

    tidesdb_column_family_config_t cfg = tidesdb_default_column_family_config();
    /* Fixed-size char[TDB_MAX_CF_NAME_LEN] -- copy with a guaranteed NUL. */
    strncpy(cfg.name, kSdiCfName, sizeof(cfg.name) - 1);
    cfg.name[sizeof(cfg.name) - 1] = '\0';
    cfg.write_buffer_size = 16 * 1024 * 1024; /* 16 MiB; metadata CF is small */
    cfg.compression_algorithm = TDB_COMPRESS_LZ4;

    int rc = tidesdb_create_column_family(engine_, cfg.name, &cfg);
    if (rc != TDB_SUCCESS && rc != TDB_ERR_EXISTS) {
        sql_print_error("[TIDESDB] SdiStore::init: failed to create __tidesdb_sdi CF (rc=%d)", rc);
        return false;
    }
    cf_ = tidesdb_get_column_family(engine_, kSdiCfName);
    if (!cf_) {
        sql_print_error("[TIDESDB] SdiStore::init: __tidesdb_sdi CF not found after create");
        return false;
    }
    return true;
}

/*
  Store an SDI blob keyed by the packed (type, id). One-shot transaction
  wrapping a single tidesdb_txn_put (the public v9.3.2 API has no top-level
  put; every write must go through a txn). On any failure the txn is rolled
  back and freed; on success it is committed and freed.
*/
bool SdiStore::put(const sdi_key_t &k, const void *blob, uint64_t len) {
    if (!engine_ || !cf_) return false;
    std::string key = pack_key(k);

    tidesdb_txn_t *txn = nullptr;
    int rc = tidesdb_txn_begin(engine_, &txn);
    if (rc != TDB_SUCCESS || !txn) {
        sql_print_error("[TIDESDB] SdiStore::put: txn_begin rc=%d", rc);
        return false;
    }
    rc = tidesdb_txn_put(txn, cf_, reinterpret_cast<const uint8_t *>(key.data()), key.size(),
                         static_cast<const uint8_t *>(blob), static_cast<size_t>(len), kSdiTtlNone);
    if (rc != TDB_SUCCESS) {
        sql_print_error("[TIDESDB] SdiStore::put: txn_put rc=%d", rc);
        tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        return false;
    }
    rc = tidesdb_txn_commit(txn);
    tidesdb_txn_free(txn);
    if (rc != TDB_SUCCESS) {
        sql_print_error("[TIDESDB] SdiStore::put: txn_commit rc=%d", rc);
        return false;
    }
    return true;
}

/*
  Read an SDI blob.

  Caller contract (matches MySQL's sdi_get callback):
    - out  == nullptr  -> size-probe; *len receives the required buffer size,
                          return value indicates whether the key exists.
    - out  != nullptr  -> copy up to *len bytes into out; *len is updated with
                          the actual blob size. If the caller's buffer is too
                          small, *len is set to the required size and false is
                          returned without partial-copy.

  Returns false on "not found" and on any IO/transaction error; only true when
  the blob was read (or its size probed) successfully. Any value buffer
  allocated by TidesDB is freed with tidesdb_free() -- it may come from
  jemalloc / mimalloc / tcmalloc depending on build flags, so libc free() is
  incorrect.
*/
bool SdiStore::get(const sdi_key_t &k, void *out, uint64_t *len) {
    if (!engine_ || !cf_ || !len) return false;
    std::string key = pack_key(k);

    tidesdb_txn_t *txn = nullptr;
    int rc = tidesdb_txn_begin(engine_, &txn);
    if (rc != TDB_SUCCESS || !txn) {
        sql_print_error("[TIDESDB] SdiStore::get: txn_begin rc=%d", rc);
        return false;
    }
    uint8_t *val = nullptr;
    size_t val_len = 0;
    rc = tidesdb_txn_get(txn, cf_, reinterpret_cast<const uint8_t *>(key.data()), key.size(), &val,
                         &val_len);
    /* Reads do not need to be committed; rollback discards txn state cheaply. */
    tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);

    if (rc == TDB_ERR_NOT_FOUND) {
        if (val) tidesdb_free(val);
        return false;
    }
    if (rc != TDB_SUCCESS) {
        sql_print_error("[TIDESDB] SdiStore::get: txn_get rc=%d", rc);
        if (val) tidesdb_free(val);
        return false;
    }

    if (out == nullptr) {
        /* Size probe -- caller just wanted the length. */
        *len = val_len;
        if (val) tidesdb_free(val);
        return true;
    }
    if (*len < val_len) {
        /* Caller buffer too small; report required size and bail. */
        *len = val_len;
        if (val) tidesdb_free(val);
        return false;
    }
    if (val_len > 0 && val) memcpy(out, val, val_len);
    *len = val_len;
    if (val) tidesdb_free(val);
    return true;
}

/*
  Delete an SDI blob. Idempotent: a missing key is treated as success, since
  the contract is "after this call the key is gone".
*/
bool SdiStore::del(const sdi_key_t &k) {
    if (!engine_ || !cf_) return false;
    std::string key = pack_key(k);

    tidesdb_txn_t *txn = nullptr;
    int rc = tidesdb_txn_begin(engine_, &txn);
    if (rc != TDB_SUCCESS || !txn) {
        sql_print_error("[TIDESDB] SdiStore::del: txn_begin rc=%d", rc);
        return false;
    }
    rc = tidesdb_txn_delete(txn, cf_, reinterpret_cast<const uint8_t *>(key.data()), key.size());
    if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND) {
        sql_print_error("[TIDESDB] SdiStore::del: txn_delete rc=%d", rc);
        tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        return false;
    }
    rc = tidesdb_txn_commit(txn);
    tidesdb_txn_free(txn);
    if (rc != TDB_SUCCESS) {
        sql_print_error("[TIDESDB] SdiStore::del: txn_commit rc=%d", rc);
        return false;
    }
    return true;
}

/*
  Enumerate every (type, id) currently stored in the SDI CF. Because pack_key
  is big-endian, the CF's natural byte-order matches the (type, id) sort
  order, so this is a single forward scan with no extra sort.

  Deviation from spec: iterators in v9.3.2 are anchored to a txn and freed
  with tidesdb_iter_free (not tidesdb_iter_destroy). tidesdb_iter_key signals
  end-of-stream via the iter_valid() check and the int return code, not an
  out-parameter alone -- we treat any non-success rc on the key fetch as a
  hard stop.
*/
bool SdiStore::list_keys(sdi_vector_t &out) {
    if (!engine_ || !cf_) return false;

    tidesdb_txn_t *txn = nullptr;
    int rc = tidesdb_txn_begin(engine_, &txn);
    if (rc != TDB_SUCCESS || !txn) {
        sql_print_error("[TIDESDB] SdiStore::list_keys: txn_begin rc=%d", rc);
        return false;
    }
    tidesdb_iter_t *it = nullptr;
    rc = tidesdb_iter_new(txn, cf_, &it);
    if (rc != TDB_SUCCESS || !it) {
        sql_print_error("[TIDESDB] SdiStore::list_keys: iter_new rc=%d", rc);
        tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        return false;
    }
    tidesdb_iter_seek_to_first(it);
    while (tidesdb_iter_valid(it)) {
        uint8_t *k = nullptr;
        size_t klen = 0;
        if (tidesdb_iter_key(it, &k, &klen) != TDB_SUCCESS || !k) break;
        if (klen == kSdiPackedKeyLen) {
            sdi_key_t key{};
            key.type = (static_cast<uint32_t>(k[0]) << 24) |
                       (static_cast<uint32_t>(k[1]) << 16) |
                       (static_cast<uint32_t>(k[2]) << 8) | static_cast<uint32_t>(k[3]);
            uint64_t id = 0;
            for (int i = 0; i < 8; i++) id = (id << 8) | static_cast<uint64_t>(k[4 + i]);
            key.id = id;
            out.m_vec.push_back(key);
        }
        /* Skip rows whose key isn't our fixed-length format. Defensive: should
           never happen unless something else wrote into this CF. */
        tidesdb_iter_next(it);
    }
    tidesdb_iter_free(it);
    tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
    return true;
}
/*
  Pack a (type, id) SDI key into a fixed-length big-endian byte string so the
  metadata CF's natural lexicographic byte-order is also the logical
  (type, id) sort order. That makes list_keys() a single forward scan with no
  client-side sort.

  Layout (12 bytes total):
    bytes 0..3   uint32 type, big-endian
    bytes 4..11  uint64 id,   big-endian
*/
std::string SdiStore::pack_key(const sdi_key_t &k) {
    std::string out;
    out.resize(kSdiPackedKeyLen);
    uint8_t *p = reinterpret_cast<uint8_t *>(&out[0]);
    p[0] = static_cast<uint8_t>(k.type >> 24);
    p[1] = static_cast<uint8_t>(k.type >> 16);
    p[2] = static_cast<uint8_t>(k.type >> 8);
    p[3] = static_cast<uint8_t>(k.type);
    for (int i = 0; i < 8; i++) {
        p[4 + i] = static_cast<uint8_t>(k.id >> ((7 - i) * 8));
    }
    return out;
}

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
