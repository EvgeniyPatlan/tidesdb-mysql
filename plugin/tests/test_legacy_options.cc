/*
  Unit tests for the retired-ENGINE_ATTRIBUTE table (tidesdb_legacy_options.h).

  The shipped table is empty until an engine version retires an option, so
  asserting against it alone would prove nothing today and would keep proving
  nothing if a later edit broke the search rather than the data. The search is
  therefore driven against a fixture, and the shipped table is checked only
  for the properties it must hold whatever it contains.
*/
#include <gtest/gtest.h>

#include "tidesdb_legacy_options.h"

namespace {

/* Stands in for a populated table: one of each disposition, sentinel last. */
constexpr TdbLegacyOption kFixture[] = {
    {"write_buffer_size", TdbLegacyDisposition::Relocated, "some_global_var", nullptr},
    {"use_btree", TdbLegacyDisposition::Removed, nullptr, "key logs are always btrees"},
    {nullptr, TdbLegacyDisposition::Removed, nullptr, nullptr},
};

TEST(LegacyOptions, FindsRelocatedEntry) {
    const TdbLegacyOption *e = tdb_legacy_option_lookup("write_buffer_size", kFixture);
    ASSERT_NE(nullptr, e);
    EXPECT_EQ(TdbLegacyDisposition::Relocated, e->disposition);
    EXPECT_STREQ("some_global_var", e->replacement);
}

TEST(LegacyOptions, FindsRemovedEntry) {
    const TdbLegacyOption *e = tdb_legacy_option_lookup("use_btree", kFixture);
    ASSERT_NE(nullptr, e);
    EXPECT_EQ(TdbLegacyDisposition::Removed, e->disposition);
    EXPECT_EQ(nullptr, e->replacement);
    EXPECT_STREQ("key logs are always btrees", e->reason);
}

/* A live key must not match. The parser ignores keys it does not know, and a
   false positive here would reject DDL that is perfectly valid. */
TEST(LegacyOptions, LiveKeyDoesNotMatch) {
    EXPECT_EQ(nullptr, tdb_legacy_option_lookup("bloom_filter", kFixture));
}

/* Matching is exact -- a prefix or a superstring of a retired key is a
   different option and must be left alone. */
TEST(LegacyOptions, MatchIsExactNotPrefix) {
    EXPECT_EQ(nullptr, tdb_legacy_option_lookup("use_btre", kFixture));
    EXPECT_EQ(nullptr, tdb_legacy_option_lookup("use_btree_extra", kFixture));
}

/* The walk stops at the sentinel rather than reading past the table. */
TEST(LegacyOptions, StopsAtSentinel) {
    constexpr TdbLegacyOption only_sentinel[] = {
        {nullptr, TdbLegacyDisposition::Removed, nullptr, nullptr},
    };
    EXPECT_EQ(nullptr, tdb_legacy_option_lookup("anything", only_sentinel));
}

TEST(LegacyOptions, NullInputsAreSafe) {
    EXPECT_EQ(nullptr, tdb_legacy_option_lookup(nullptr, kFixture));
    EXPECT_EQ(nullptr, tdb_legacy_option_lookup("write_buffer_size", nullptr));
}

/* Properties the shipped table must hold no matter what it contains: it is
   sentinel-terminated, and every entry carries the field its disposition
   promises. A Removed entry naming a replacement, or a Relocated entry
   without one, would produce an error message that misleads. */
TEST(LegacyOptions, ShippedTableIsWellFormed) {
    size_t n = 0;
    for (const TdbLegacyOption *e = tdb_legacy_options; e->key != nullptr; e++) {
        if (e->disposition == TdbLegacyDisposition::Relocated) {
            EXPECT_NE(nullptr, e->replacement) << "Relocated '" << e->key << "' names no replacement";
        } else {
            EXPECT_NE(nullptr, e->reason) << "Removed '" << e->key << "' gives no reason";
            EXPECT_EQ(nullptr, e->replacement) << "Removed '" << e->key << "' invents a replacement";
        }
        ASSERT_LT(++n, 256u) << "table appears not to be sentinel-terminated";
    }
    EXPECT_EQ(tdb_legacy_options_empty(), n == 0);
}

}  // namespace
