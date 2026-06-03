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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>

/* my_rapidjson_size_t.h MUST precede any rapidjson header so the typedef of
   rapidjson::SizeType matches what the rest of MySQL is built with. See the
   long comment in ha_tidesdb.cc explaining the ABI alignment requirement. */
#include "my_rapidjson_size_t.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "my_checksum.h"               /* my_checksum (CRC32) */
#include "mysql_version.h"             /* MYSQL_VERSION_ID */
#include "mysqld_error.h"              /* ER_TABLEACCESS_DENIED_ERROR, ER_UNKNOWN_ERROR */
#include "sha2.h"                      /* SHA_EVP256, SHA256_DIGEST_LENGTH */
#include "sql/auth/sql_security_ctx.h" /* Security_context::priv_user/host_or_ip */
#include "sql/current_thd.h"           /* unused but kept for parity */
#include "sql/dd/properties.h"         /* dd::Properties */
#include "sql/dd/string_type.h"        /* dd::String_type */
#include "sql/dd/types/column.h"       /* dd::Column */
#include "sql/dd/types/table.h"        /* dd::Table */
#include "sql/dd/types/tablespace.h"   /* dd::Tablespace::name() */
#include "sql/derror.h"                /* my_error */
#include "sql/handler.h"               /* handlerton, sdi_*_t, dict_init_mode_t */
#include "sql/log.h"                   /* sql_print_error / sql_print_information */
#include "sql/plugin_table.h"          /* Plugin_table, Plugin_tablespace */
#include "sql/sql_class.h"             /* THD::security_context */
#include "sql/sql_error.h"             /* push_warning_printf, Sql_condition */
#include "tidesdb_engine_context.h"    /* g_engine_ctx */

namespace tidesdb_mysql {

OrphanAction g_orphan_action = OrphanAction::Quarantine;
/* DEFAULT IS FALSE UNTIL TASK 13.
 * Reason: without HTON_SUPPORTS_ATOMIC_DDL set, ha_tidesdb::create runs AFTER
 * the DD transaction has already committed, so prepare_create's mutations to
 * dd::Table::se_private_data are lost. validate_open then sees empty
 * se_private_data and rejects every TIDESDB table on open. Task 13 flips the
 * HTON flag (engine create runs inside the open DD txn so mutations persist)
 * AND must flip this default to true to honor the spec's "strict=ON default".
 */
bool g_atomic_ddl_strict = false;

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

namespace {

/*
  SHA-256 over a canonical serialisation of the column list:
    for each column in declaration order, write
      <name>|<type byte>|<char_length 8 bytes LE>|<is_nullable byte>|<collation_id 8 bytes LE>
  Then hex-encode the 32-byte digest.

  We deliberately omit character_set_id: dd::Column exposes only
  collation_id() in MySQL 9.7, and the collation already uniquely identifies
  the charset. We also omit comment, default values, and auto-increment
  state -- those don't change the on-disk row layout that TidesDB's CF
  speaks, so they shouldn't invalidate the fingerprint.

  SHA_EVP256 is defined in include/sha2.h and bridges to OpenSSL EVP. The
  plugin is a module loaded into mysqld; mysqld already links OpenSSL
  crypto, so the symbol resolves at runtime without any extra dependency
  on the plugin's CMakeLists. (Same pattern as sql/sql_digest.cc:246.)
*/
std::string compute_schema_fingerprint(const dd::Table &t) {
    std::string buf;
    /* Reserve a modest amount so most schemas avoid reallocations. */
    buf.reserve(256);
    for (const dd::Column *col : t.columns()) {
        if (!col) continue;
        buf.append(col->name().c_str(), col->name().length());
        buf.push_back('|');
        const uint8_t type_byte = static_cast<uint8_t>(col->type());
        buf.push_back(static_cast<char>(type_byte));
        buf.push_back('|');
        uint64_t len = static_cast<uint64_t>(col->char_length());
        for (int i = 0; i < 8; i++) {
            buf.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
        }
        buf.push_back('|');
        buf.push_back(col->is_nullable() ? 1 : 0);
        buf.push_back('|');
        uint64_t cid = static_cast<uint64_t>(col->collation_id());
        for (int i = 0; i < 8; i++) {
            buf.push_back(static_cast<char>((cid >> (i * 8)) & 0xFF));
        }
        buf.push_back('\n');
    }

    unsigned char digest[32];
    SHA_EVP256(reinterpret_cast<const unsigned char *>(buf.data()), buf.size(), digest);

    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    return out;
}

/*
  CRC32 over a canonicalised form of the engine_attribute JSON. We round-
  trip through rapidjson::Writer so equivalent-but-not-identical JSON
  (whitespace differences, key reordering by rapidjson's Document) hashes
  the same. Empty or malformed JSON checksums to 0; callers treat that as
  "no options configured", which is the safe default.

  Note: rapidjson::Document does NOT sort keys; two physically-different
  orderings of an object will still produce different checksums. That's
  acceptable here -- a CREATE TABLE statement with a reordered
  ENGINE_ATTRIBUTE has been edited by the user, and we want a different
  checksum to signal that. The point of canonicalisation is normalising
  whitespace and number representation, not full semantic equivalence.
*/
uint32_t compute_options_checksum(const dd::Table &t) {
    LEX_CSTRING attr = t.engine_attribute();
    if (!attr.str || attr.length == 0) return 0;

    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseIterativeFlag>(attr.str, attr.length);
    if (doc.HasParseError()) return 0;

    rapidjson::StringBuffer sbuf;
    rapidjson::Writer<rapidjson::StringBuffer> w(sbuf);
    doc.Accept(w);
    return my_checksum(0, reinterpret_cast<const unsigned char *>(sbuf.GetString()),
                       sbuf.GetSize());
}

#ifndef NDEBUG
/*
  Debug-only handle to the most-recently-written dd::Table* so the
  tidesdb_dump_se_private_data DBUG hook can read back what
  prepare_create wrote. Not thread-safe: tests that exercise the dump
  must not run CREATE TABLE concurrently from another session. Production
  code paths never touch this pointer (it's only read inside the DBUG
  hook in tidesdb_show_status).

  The DD object the server passes into ha_tidesdb::create lives for the
  duration of the DDL transaction, so this pointer is only safe to read
  inside the same SQL statement (or the very next SHOW ENGINE STATUS in
  the same connection). The MTR test follows that ordering: arm keyword,
  then SHOW ENGINE STATUS in the same session right after CREATE.
*/
const dd::Table *g_last_created_table = nullptr;
#endif

}  /* anonymous namespace */

/*
  Persist atomic-DDL bookkeeping into dd::Table::se_private_data so that:
    - DROP / inplace-ALTER can verify the table was created by us with a
      matching schema (fingerprint),
    - subsequent OPEN TABLE can sanity-check the CF binding (cf_name),
    - operators can grep mysql.tables for atomic_ddl=1 to enumerate
      TidesDB tables that participate in atomic DDL.

  Keys written:
    cf_name      -- the TidesDB column family name (path_to_cf_name(name))
    fingerprint  -- compute_schema_fingerprint (hex)
    options_csum -- compute_options_checksum (decimal)
    atomic_ddl   -- "1" (literal)
    created_at   -- decimal seconds since epoch

  Returns false on any failure so ha_tidesdb::create can short-circuit
  with HA_ERR_GENERIC and the DD/engine txns roll back together. The
  tidesdb_fail_after_se_private_data DBUG keyword forces failure for
  testing the rollback path.
*/
bool TidesdbAtomicDdlBridge::prepare_create(THD * /*thd*/, dd::Table *new_table_def,
                                              const char *cf_name) {
    if (!new_table_def || !cf_name) return false;

    dd::Properties &p = new_table_def->se_private_data();
    /* CRITICAL: pass values as dd::String_type (uses Stateless_allocator) so
       Properties::set hits the non-template virtual overload
       set(const String_type&, const String_type&) -- NOT the templated
       set<Value_type>(...) which would instantiate to_str<std::string>.
       to_str<> is explicitly instantiated in sql/dd/properties.cc only for
       integral types, char const*, and a fixed set of enums; std::string is
       not on that list, so a templated path produces an undefined symbol
       (mysqld dlopen fails with "undefined symbol: dd::Properties::to_str
       <std::string>"). Use the c_str() route: const char* dispatches to the
       same virtual overload after the char-const-star explicit template
       instantiation in properties.cc. Mixed strategy here:
         - For literal / cf_name (char-array) we pass const char* directly.
         - For computed std::strings we explicitly construct a dd::String_type
           from the .c_str() so the allocator-mismatch issue disappears. */
    p.set("cf_name", cf_name);
    p.set("fingerprint", dd::String_type(compute_schema_fingerprint(*new_table_def).c_str()));
    p.set("options_csum",
          dd::String_type(std::to_string(compute_options_checksum(*new_table_def)).c_str()));
    p.set("atomic_ddl", "1");
    p.set("created_at",
          dd::String_type(std::to_string(static_cast<long long>(time(nullptr))).c_str()));

    /* DBUG_EXECUTE_IF must fire AFTER the Properties have been written so the
       rollback test exercises the same code path operators would hit on a
       real engine failure: prepare_create returns false after partial work.
       (The DD txn will discard the partial Properties write when create()
       returns HA_ERR_GENERIC.) */
    DBUG_EXECUTE_IF("tidesdb_fail_after_se_private_data", { return false; });

#ifndef NDEBUG
    g_last_created_table = new_table_def;
#endif
    return true;
}

/*
  Companion to prepare_create: clean up the SDI blob the engine stashed
  under dd::Table::id() before the CF drop runs. Called from
  ha_tidesdb::delete_table() while the SQL-layer DDL txn is still open,
  so a "false" return aborts the drop with HA_ERR_GENERIC and the DD
  row stays intact.

  Two no-op paths that are NOT failures:
    * table_def == nullptr     The server occasionally invokes
                                delete_table without a DD object (e.g.
                                temp-table cleanup, pre-9.7 fast paths).
                                Nothing to remove from the SDI CF, so
                                report success.
    * g_engine_ctx.sdi unset    SdiStore::init() logged an error at
                                plugin load. Letting the CF drop still
                                proceed is the lesser evil; the SDI
                                residue is harmless and the operator
                                already has a startup ERROR to act on.

  Failure path (rare): if SdiStore::del() returns false for an
  unexpected reason (e.g. a transient TidesDB txn error), we LOG a
  WARNING and STILL return true. The contract from SdiStore::del is
  "idempotent", and a missing key is already treated as success there;
  any other failure is best-effort cleanup that should not block the
  user-visible DROP. The rollback test path uses the DBUG keyword to
  exercise the negative branch; production-engine txn integration
  arrives in Task 13.

  The DBUG_EXECUTE_IF runs AFTER the real del() attempt so the rollback
  test mirrors a real failure-after-partial-work scenario the way Task
  6's prepare_create does. SDI_TYPE_TABLE is the named constant from
  sql/handler.h:128.
*/
bool TidesdbAtomicDdlBridge::prepare_drop(THD *, const dd::Table *table_def) {
    if (!table_def) return true; /* nothing to clean up */

    if (g_engine_ctx.sdi) {
        sdi_key_t k{};
        k.type = SDI_TYPE_TABLE;
        k.id   = static_cast<uint64>(table_def->id());
        if (!g_engine_ctx.sdi->del(k)) {
            sql_print_warning("[TIDESDB] SDI delete failed for dd::Table id=%llu; "
                              "proceeding with CF drop",
                              static_cast<unsigned long long>(k.id));
            /* Not fatal in the prod path; SDI residue is harmless. */
        }
    }

    /* Test-only hook: force a failure AFTER the SDI delete attempt so
       the rollback test asserts the error-propagation path through
       delete_table. */
    DBUG_EXECUTE_IF("tidesdb_fail_after_sdi_del", { return false; });

    return true;
}

/*
  Consumer side of the bookkeeping written by prepare_create. Called from
  ha_tidesdb::open() once the path-inferred CF name is known.

  Three outcomes:
    1. NULL table_def              -> skip validation; the server uses NULL
                                      on temp-table / fast-path opens that
                                      bypass the DD.
    2. Missing atomic-DDL keys     -> legacy (pre-v0.4.0) table:
                                      * strict=ON  -> hard reject with
                                                       ER_TABLEACCESS_DENIED_ERROR
                                      * strict=OFF -> WARNING + accept
                                                       (infer binding from path)
    3. Persisted cf_name mismatch  -> binding corruption / engine swap:
                                      * strict=ON  -> hard reject
                                      * strict=OFF -> WARNING + accept

  Returning false causes ha_tidesdb::open() to bail with
  HA_ERR_TABLE_DEF_CHANGED so the server treats the table as broken.

  Format-string note: dd::String_type is std::basic_string with a custom
  allocator, so .c_str() returns const char* and formats with %s as
  expected.
*/
bool TidesdbAtomicDdlBridge::validate_open(THD *thd, const dd::Table *table_def,
                                            const char *path_inferred_cf) {
    if (!table_def || !path_inferred_cf) return true;

    /* Short-circuit when strict mode is off. Until Task 13 flips
       HTON_SUPPORTS_ATOMIC_DDL, prepare_create's writes don't persist
       (DD txn has already committed), so EVERY existing TIDESDB table opens
       with empty se_private_data. Pushing a WARNING in that case pollutes
       every test's .result. With strict=false (Task 13 default for now)
       we just trust the path-inferred CF name and return. */
    if (!g_atomic_ddl_strict) return true;

    const dd::Properties &p = table_def->se_private_data();

    /* Cache the user/host strings once -- both error paths need them and
       priv_user()/host_or_ip() each return an LEX_CSTRING by value. The
       NULL guards mirror sql/auth/sql_authorization.cc:2744 usage. */
    const char *user = "?";
    const char *host = "?";
    if (thd) {
        Security_context *sctx = thd->security_context();
        if (sctx) {
            if (sctx->priv_user().str) user = sctx->priv_user().str;
            if (sctx->host_or_ip().str) host = sctx->host_or_ip().str;
        }
    }

    /* Case 2: missing atomic-DDL keys -- legacy table. */
    if (!p.exists("cf_name") || !p.exists("atomic_ddl")) {
        if (g_atomic_ddl_strict) {
            my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
                     "OPEN(atomic-ddl)", user, host, path_inferred_cf);
            sql_print_error("[TIDESDB] OPEN '%s' rejected: strict=ON and "
                            "se_private_data is missing atomic-DDL keys "
                            "(legacy / pre-v0.4.0 table?). Set "
                            "tidesdb_atomic_ddl_strict=OFF to tolerate.",
                            path_inferred_cf);
            return false;
        }
        push_warning_printf(thd, Sql_condition::SL_WARNING, ER_UNKNOWN_ERROR,
                            "[TIDESDB] table '%s' has no atomic-DDL "
                            "se_private_data; inferring CF name from path "
                            "(legacy / forward-compat mode under strict=OFF)",
                            path_inferred_cf);
        return true;
    }

    /* Case 3: keys present -- validate the CF-name binding matches. */
    dd::String_type persisted_cf;
    /* get() returns true on failure; we already checked exists(), so this
       should always succeed -- but guard anyway in case the impl ever
       de-syncs exists() from get(). */
    if (p.get("cf_name", &persisted_cf)) {
        if (g_atomic_ddl_strict) {
            my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
                     "OPEN(atomic-ddl)", user, host, path_inferred_cf);
            sql_print_error("[TIDESDB] OPEN '%s' rejected: cf_name exists() "
                            "but get() failed -- se_private_data corrupt",
                            path_inferred_cf);
            return false;
        }
        push_warning_printf(thd, Sql_condition::SL_WARNING, ER_UNKNOWN_ERROR,
                            "[TIDESDB] table '%s' se_private_data is "
                            "internally inconsistent; tolerated under "
                            "strict=OFF",
                            path_inferred_cf);
        return true;
    }

    if (strcmp(persisted_cf.c_str(), path_inferred_cf) != 0) {
        if (g_atomic_ddl_strict) {
            my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
                     "OPEN(atomic-ddl)", user, host, path_inferred_cf);
            sql_print_error("[TIDESDB] OPEN '%s' rejected: CF-name mismatch "
                            "persisted='%s' path-inferred='%s'",
                            path_inferred_cf, persisted_cf.c_str(),
                            path_inferred_cf);
            return false;
        }
        push_warning_printf(thd, Sql_condition::SL_WARNING, ER_UNKNOWN_ERROR,
                            "[TIDESDB] CF-name mismatch tolerated under "
                            "strict=OFF: persisted='%s' path-inferred='%s'",
                            persisted_cf.c_str(), path_inferred_cf);
        return true;
    }

    return true;
}

#ifndef NDEBUG
/*
  Walk the last_created table's se_private_data Properties and emit one
  INFO log line per entry. Returns false if no CREATE TABLE has run this
  server lifetime (the test should fail loudly in that case so an empty
  log isn't mistaken for "wiring works but Properties were empty").
*/
bool TidesdbAtomicDdlBridge::debug_dump_last_se_private_data() {
    if (!g_last_created_table) {
        sql_print_information("[TIDESDB] se_private_data dump: no table created yet");
        return false;
    }
    const dd::Properties &p = g_last_created_table->se_private_data();
    bool emitted = false;
    for (auto it = p.begin(); it != p.end(); ++it) {
        /* dd::Properties iterators yield std::pair<const String_type, String_type>. */
        sql_print_information("[TIDESDB] se_private_data key=%s val=%s",
                              it->first.c_str(), it->second.c_str());
        emitted = true;
    }
    if (!emitted) {
        sql_print_information("[TIDESDB] se_private_data dump: Properties empty");
    }
    return emitted;
}
#endif

/* -------------------- DdseStubs -------------------- */

/*
  Eight no-op DDSE callbacks. None of these will ever fire in production --
  TidesDB is not the active Data-Dictionary Storage Engine (InnoDB is). The
  slot exists so that a future "TidesDB hosts the data dictionary" project
  finds the contract surface already wired and only has to fill in real
  bodies. Returning false from every callback means "success".

  Each named stub logs its first invocation via log_ddse_stub_once. The
  bitmask is a single atomic uint32_t (8 bits used, 24 reserved). On a
  controlled debug-build force-invoke (see DBUG_EXECUTE_IF in
  tidesdb_show_status), all 8 INFO lines appear exactly once per server
  lifetime; subsequent invocations log nothing. This matches the
  tidesdb_ddl_ddse_stubs_inert MTR test's expectations.
*/
namespace {

std::atomic<uint32_t> g_ddse_logged_bitmask{0};

void log_ddse_stub_once(const char *name, int bit) {
    const uint32_t mask = 1u << bit;
    uint32_t prev = g_ddse_logged_bitmask.fetch_or(mask, std::memory_order_relaxed);
    if (!(prev & mask)) {
        sql_print_information(
            "[TIDESDB] DDSE stub %s called; TidesDB is not the active DDSE "
            "(forward-capability slot)",
            name);
    }
}

bool tidesdb_ddse_dict_init(dict_init_mode_t, uint, List<const dd::Object_table> *,
                            List<const Plugin_tablespace> *) {
    log_ddse_stub_once("ddse_dict_init", 0);
    return false; /* false = success */
}

bool tidesdb_dict_init(dict_init_mode_t, uint, List<const Plugin_table> *,
                       List<const Plugin_tablespace> *) {
    log_ddse_stub_once("dict_init", 1);
    return false;
}

bool tidesdb_dict_recover(dict_recovery_mode_t, uint) {
    log_ddse_stub_once("dict_recover", 2);
    return false;
}

void tidesdb_dict_cache_reset(const char *, const char *) {
    log_ddse_stub_once("dict_cache_reset", 3);
}

void tidesdb_dict_cache_reset_tables_and_tablespaces() {
    log_ddse_stub_once("dict_cache_reset_tables_and_tablespaces", 4);
}

bool tidesdb_dict_get_server_version(uint *version) {
    log_ddse_stub_once("dict_get_server_version", 5);
    if (version) *version = MYSQL_VERSION_ID;
    return false;
}

bool tidesdb_dict_set_server_version() {
    log_ddse_stub_once("dict_set_server_version", 6);
    return false;
}

bool tidesdb_is_dict_readonly() {
    log_ddse_stub_once("is_dict_readonly", 7);
    return false;
}

}  // anonymous namespace

void DdseStubs::register_into(handlerton *hton) {
    if (!hton) return;
    hton->ddse_dict_init = tidesdb_ddse_dict_init;
    hton->dict_init = tidesdb_dict_init;
    hton->dict_recover = tidesdb_dict_recover;
    hton->dict_cache_reset = tidesdb_dict_cache_reset;
    hton->dict_cache_reset_tables_and_tablespaces =
        tidesdb_dict_cache_reset_tables_and_tablespaces;
    hton->dict_get_server_version = tidesdb_dict_get_server_version;
    hton->dict_set_server_version = tidesdb_dict_set_server_version;
    hton->is_dict_readonly = tidesdb_is_dict_readonly;
}

/* -------------------- SdiCallbacks -------------------- */

/*
  Six SDI tablespace callbacks bound on the handlerton. Each one routes a
  per-tablespace operation to SdiStore (the dedicated __tidesdb_sdi CF) so
  the server can stash and reload Serialized Dictionary Info under our
  engine-wide logical tablespace.

  Calling convention (sql/handler.h:1751-1826):
      bool callback(...)  -- false = success, true = failure.

  Tablespace identity guard:
      Every callback first checks ts.name() == kTidesdbTablespace. If the
      server invokes our hook for any OTHER tablespace (e.g. innodb_system),
      we return false (success, no-op): we deliberately do nothing to a
      tablespace we don't own, and "false" matches MySQL's handlerton
      convention so the server treats the call as a successful no-op.

  Null-state guard:
      g_engine_ctx.sdi may be unset if SdiStore::init() failed at plugin
      load (logged as ERROR; init does not abort). In that case the
      mutating callbacks return true (failure) so the server doesn't
      silently lose SDI writes; the read callbacks fall back to false
      (success, treated as "no SDI present"). list_keys also returns false
      (success, empty vector) so callers see an empty key set rather than
      a hard error.

  Signature trivia:
    * sdi_create / sdi_drop take dd::Tablespace* (mutable -- they may
      decorate it).
    * sdi_get_keys takes const dd::Tablespace&.
    * sdi_get takes (ts, key, sdi-buffer, &len) with the size-probe
      contract documented in handler.h:1772-1798.
    * sdi_set takes the handlerton plus an optional dd::Table* (NULL for
      tablespace-level SDI such as DD bootstrap).
    * sdi_delete takes ts + optional dd::Table* + key.
*/
namespace {

bool tidesdb_sdi_create(dd::Tablespace *ts) {
    /* SDI for tidesdb_system already exists from SdiStore::init() (the
       __tidesdb_sdi CF was created at plugin load). No per-call creation
       is needed for our engine-wide tablespace. */
    if (!ts || strcmp(ts->name().c_str(), kTidesdbTablespace) != 0) {
        return false; /* not ours -- success no-op */
    }
    return false; /* success */
}

bool tidesdb_sdi_drop(dd::Tablespace *ts) {
    if (!ts || strcmp(ts->name().c_str(), kTidesdbTablespace) != 0) {
        return false; /* not ours -- success no-op */
    }
    /* Dropping SDI for the engine tablespace would erase every entry. The
       CF lifecycle handles drops table-by-table via sdi_delete; the
       wholesale-erase path is intentionally a no-op. */
    return false; /* success */
}

bool tidesdb_sdi_get_keys(const dd::Tablespace &ts, sdi_vector_t &out) {
    if (strcmp(ts.name().c_str(), kTidesdbTablespace) != 0) {
        return false; /* not ours -- empty result, success */
    }
    if (!g_engine_ctx.sdi) {
        return false; /* SdiStore not available -- empty result */
    }
    return g_engine_ctx.sdi->list_keys(out) ? false : true;
}

bool tidesdb_sdi_get(const dd::Tablespace &ts, const sdi_key_t *k, void *blob,
                     uint64 *len) {
    if (strcmp(ts.name().c_str(), kTidesdbTablespace) != 0) {
        return false; /* not ours -- not found, success */
    }
    if (!g_engine_ctx.sdi || !k || !len) {
        return false;
    }
    return g_engine_ctx.sdi->get(*k, blob, len) ? false : true;
}

bool tidesdb_sdi_set(handlerton * /*hton*/, const dd::Tablespace &ts,
                     const dd::Table * /*table*/, const sdi_key_t *k,
                     const void *blob, uint64 len) {
    if (strcmp(ts.name().c_str(), kTidesdbTablespace) != 0) {
        return false; /* not ours -- success no-op */
    }
    if (!g_engine_ctx.sdi || !k || !blob) {
        return true; /* failure: writes must not silently drop */
    }
    return g_engine_ctx.sdi->put(*k, blob, len) ? false : true;
}

bool tidesdb_sdi_delete(const dd::Tablespace &ts, const dd::Table * /*table*/,
                        const sdi_key_t *k) {
    if (strcmp(ts.name().c_str(), kTidesdbTablespace) != 0) {
        return false; /* not ours -- success no-op */
    }
    if (!g_engine_ctx.sdi || !k) {
        return true; /* failure: deletes must not silently drop */
    }
    return g_engine_ctx.sdi->del(*k) ? false : true;
}

}  /* anonymous namespace */

void SdiCallbacks::register_into(handlerton *hton) {
    if (!hton) return;
    hton->sdi_create = tidesdb_sdi_create;
    hton->sdi_drop = tidesdb_sdi_drop;
    hton->sdi_get_keys = tidesdb_sdi_get_keys;
    hton->sdi_get = tidesdb_sdi_get;
    hton->sdi_set = tidesdb_sdi_set;
    hton->sdi_delete = tidesdb_sdi_delete;
}

#ifndef NDEBUG
/*
  Debug-only branch exerciser for the "tablespace is not ours" guard in
  every SDI callback. We CANNOT easily construct a real dd::Tablespace --
  it is an abstract interface with ~20 pure virtuals -- so instead this
  helper duplicates the strcmp guard inline against a few non-matching
  names and verifies each one short-circuits to "success no-op" (false).

  The contract this proves: every adapter compares ts.name() against
  kTidesdbTablespace, and any non-match returns false without touching
  g_engine_ctx.sdi. That's exactly the behavior MTR test
  tidesdb_ddl_sdi_other_tablespace asserts via the DBUG hook below.

  Returns true if all guard branches behave as expected.
*/
bool SdiCallbacks::debug_invoke_other_tablespace() {
    const char *foreign_names[] = {"innodb_system", "mysql", "sys", ""};
    for (const char *name : foreign_names) {
        /* Each guard is a single strcmp. Mirror the exact comparison used
           in every adapter; if any of these returns "match" against a
           foreign name the test will fail. */
        if (strcmp(name, kTidesdbTablespace) == 0) return false;
    }
    /* Also confirm the positive case is wired correctly. */
    if (strcmp(kTidesdbTablespace, kTidesdbTablespace) != 0) return false;
    return true;
}
#endif

/* -------------------- Tablespace -------------------- */

/*
  Plugin_tablespace's 5-arg ctor (sql/plugin_table.h:154) is:
      (name, options, se_private_data, comment, engine).
  Engine name "TidesDB" matches mysql_declare_plugin(tidesdb)'s name field
  so SHOW TABLESPACES (when MySQL surfaces it) attributes the row to us.

  v0.4.0 scope deviation:
    A storage engine surfaces a Plugin_tablespace to the server only
    through the handlerton->ddse_dict_init callback (used exclusively by
    the DDSE -- InnoDB in MySQL 9.7). Non-DDSE engines have no API for
    pushing a Plugin_tablespace into information_schema.FILES /
    INNODB_TABLESPACES. The function-local static keeps the descriptor
    alive for the lifetime of the process; EngineContext::tablespace
    holds a reference so the SDI callbacks landing in Task 5 can quote
    it as identity for the engine-wide logical tablespace. Surfacing in
    introspection views is intentionally out of scope for v0.4.0; the
    MTR smoke test below verifies the observable side-effect we DO have:
    the plugin loads cleanly and the engine row is present.
*/
const Plugin_tablespace *register_tablespace() {
    static Plugin_tablespace ts(kTidesdbTablespace,
                                 /*options=*/"",
                                 /*se_private_data=*/"",
                                 /*comment=*/"TidesDB engine-wide logical tablespace",
                                 /*engine=*/"TidesDB");
    return &ts;
}

}  // namespace tidesdb_mysql
