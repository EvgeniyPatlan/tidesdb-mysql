/*
  Pure-function unit tests for tidesdb_mysql::compute_delta_pure.

  These tests do NOT touch the MySQL server, the data dictionary, or a real
  TidesDB instance. They only exercise the side-effect-free symmetric
  difference between the DD-derived "expected" CF set and the engine-derived
  "actual" CF set, plus the rule that any CF whose name starts with '__'
  (the __tidesdb_sdi metadata CF and any __orphan_<epoch>_<cf> quarantined
  CFs) is excluded from both orphan sides of the delta.
*/
#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

#include "sql/handler.h"
#include "tidesdb_atomic_ddl.h"

namespace tdm = tidesdb_mysql;

static std::set<std::string> S(std::initializer_list<const char *> xs) {
    std::set<std::string> out;
    for (const char *x : xs) out.insert(x);
    return out;
}

static std::vector<std::string> sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

TEST(ReconcilerDelta, EmptyEmpty) {
    auto d = tdm::compute_delta_pure({}, {});
    EXPECT_TRUE(d.orphan_cfs.empty());
    EXPECT_TRUE(d.orphan_dd_tables.empty());
}

TEST(ReconcilerDelta, OnlyExpected) {
    auto d = tdm::compute_delta_pure(S({"test/t1", "test/t2"}), {});
    EXPECT_TRUE(d.orphan_cfs.empty());
    EXPECT_EQ(sorted(d.orphan_dd_tables),
              (std::vector<std::string>{"test/t1", "test/t2"}));
}

TEST(ReconcilerDelta, OnlyActual) {
    auto d = tdm::compute_delta_pure({}, S({"orphan1", "orphan2"}));
    EXPECT_EQ(sorted(d.orphan_cfs),
              (std::vector<std::string>{"orphan1", "orphan2"}));
    EXPECT_TRUE(d.orphan_dd_tables.empty());
}

TEST(ReconcilerDelta, PerfectMatch) {
    auto d = tdm::compute_delta_pure(S({"test/t1", "test/t2"}),
                                      S({"test/t1", "test/t2"}));
    EXPECT_TRUE(d.orphan_cfs.empty());
    EXPECT_TRUE(d.orphan_dd_tables.empty());
}

TEST(ReconcilerDelta, MixedOrphanCf) {
    auto d = tdm::compute_delta_pure(S({"test/t1"}), S({"test/t1", "orphan"}));
    EXPECT_EQ(d.orphan_cfs, (std::vector<std::string>{"orphan"}));
    EXPECT_TRUE(d.orphan_dd_tables.empty());
}

TEST(ReconcilerDelta, MixedOrphanDd) {
    auto d = tdm::compute_delta_pure(S({"test/t1", "test/missing"}),
                                      S({"test/t1"}));
    EXPECT_EQ(d.orphan_dd_tables, (std::vector<std::string>{"test/missing"}));
    EXPECT_TRUE(d.orphan_cfs.empty());
}

TEST(ReconcilerDelta, ExcludesSdiAndQuarantineCfs) {
    /* The SDI CF and any __orphan_* CFs must NOT appear as orphans. */
    auto d = tdm::compute_delta_pure(
        S({"test/t1"}),
        S({"test/t1", "__tidesdb_sdi", "__orphan_1700000000_dropped"}));
    EXPECT_TRUE(d.orphan_cfs.empty());
}
