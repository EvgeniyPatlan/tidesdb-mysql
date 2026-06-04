/*
  Server-free helpers for the atomic-DDL surface.

  Lives in a separate TU from tidesdb_atomic_ddl.cc so the gtest target can
  link it without pulling in sql/dd, sql/log, my_dbug, g_engine_ctx, and the
  rest of the server surface. Anything declared in tidesdb_atomic_ddl.h
  that is genuinely pure (side-effect-free, no MySQL state, no TidesDB
  engine handle) lives here.

  Currently exposes:
    * SdiStore::pack_key(k) — fixed-length big-endian (type, id) packer
      for the metadata CF's row keys.
    * compute_delta_pure(expected, actual) — symmetric difference of CF
      name sets, with CFs whose name begins with "__" (the SDI metadata
      CF and any __orphan_<epoch>_* quarantined CFs) excluded.
*/

#include "tidesdb_atomic_ddl.h"

#include <cstdint>
#include <set>
#include <string>

namespace tidesdb_mysql {

/*
  Pack a (type, id) SDI key into a fixed-length big-endian byte string so the
  metadata CF's natural lexicographic byte-order is also the logical
  (type, id) sort order. That makes SdiStore::list_keys() a single forward
  scan with no client-side sort.

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

namespace {
/* CFs whose name begins with this prefix are excluded from reconciliation:
   the dedicated SDI metadata CF (`__tidesdb_sdi`) and any
   `__orphan_<epoch>_<cf>` quarantined CFs produced by apply_delta. */
constexpr const char *kReconcilerSkipPrefix = "__";
}  // namespace

ReconcileDelta compute_delta_pure(const std::set<std::string> &expected_cfs,
                                   const std::set<std::string> &actual_cfs) {
    ReconcileDelta d;
    for (const auto &a : actual_cfs) {
        if (a.compare(0, 2, kReconcilerSkipPrefix) == 0) continue;
        if (expected_cfs.find(a) == expected_cfs.end()) {
            d.orphan_cfs.push_back(a);
        }
    }
    for (const auto &e : expected_cfs) {
        if (actual_cfs.find(e) == actual_cfs.end()) {
            d.orphan_dd_tables.push_back(e);
        }
    }
    return d;
}

}  // namespace tidesdb_mysql
