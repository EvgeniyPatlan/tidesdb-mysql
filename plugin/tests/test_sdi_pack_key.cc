/*
  Pure-function unit tests for SdiStore::pack_key.

  These tests do NOT touch the MySQL server, the data dictionary, or a real
  TidesDB instance. They only exercise the byte-level encoding helper, which is
  what makes `pack_key` worth lifting to a `static` method on the class: it can
  be tested in seconds outside the MTR loop.
*/
#include <gtest/gtest.h>

#include "sql/handler.h"        /* sdi_key_t (top-level struct) */
#include "tidesdb_atomic_ddl.h" /* tidesdb_mysql::SdiStore, kSdiPackedKeyLen */

namespace tdm = tidesdb_mysql;

TEST(SdiPackKey, ProducesFixedLength) {
  sdi_key_t k{};
  k.type = 1;
  k.id = 42;
  auto packed = tdm::SdiStore::pack_key(k);
  EXPECT_EQ(packed.size(), tdm::kSdiPackedKeyLen);
}

TEST(SdiPackKey, IsByteComparable) {
  /* Keys sorted by (type, id) must produce byte-strings sorted
     lexicographically, so that the metadata CF's natural byte-order is
     also (type, id) order. That makes list_keys() a single forward scan. */
  sdi_key_t a{}, b{};
  a.type = 1;
  a.id = 5;
  b.type = 1;
  b.id = 7;
  EXPECT_LT(tdm::SdiStore::pack_key(a), tdm::SdiStore::pack_key(b));

  sdi_key_t c{}, d{};
  c.type = 1;
  c.id = 999;
  d.type = 2;
  d.id = 1;
  EXPECT_LT(tdm::SdiStore::pack_key(c), tdm::SdiStore::pack_key(d));
}

TEST(SdiPackKey, ZeroIsValid) {
  sdi_key_t k{};
  auto packed = tdm::SdiStore::pack_key(k);
  EXPECT_EQ(packed.size(), tdm::kSdiPackedKeyLen);
  for (char c : packed) EXPECT_EQ(c, '\0');
}
