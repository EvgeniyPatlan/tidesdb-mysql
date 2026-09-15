/*
  Round-trip and rejection tests for the XID wire format.

  The encoding matters for exactly one reason: an XID written at prepare has to
  compare equal to the one the server asks about during recovery, after a
  restart. So the properties worth pinning are that equal identities encode
  identically, that a decode returns what was encoded, and that malformed input
  is refused rather than trusted -- the bytes come off disk, and a corrupt
  length would otherwise read past the buffer.
*/
#include <gtest/gtest.h>

#include "tidesdb_xid.h"

#include <cstring>
#include <string>

namespace {

struct Decoded
{
    long format_id = 0;
    long gtrid_len = 0;
    long bqual_len = 0;
    char data[TDB_XID_DATA_SIZE] = {};
};

bool RoundTrip(long format_id, const std::string &gtrid, const std::string &bqual, Decoded *out)
{
    char data[TDB_XID_DATA_SIZE] = {};
    memcpy(data, gtrid.data(), gtrid.size());
    memcpy(data + gtrid.size(), bqual.data(), bqual.size());

    unsigned char buf[TDB_XID_MAX_SERIALIZED];
    const size_t n = tdb_xid_serialize(format_id, (long)gtrid.size(), (long)bqual.size(), data, buf,
                                       sizeof(buf));
    if (n == 0) return false;
    return tdb_xid_deserialize(buf, n, &out->format_id, &out->gtrid_len, &out->bqual_len,
                               out->data);
}

TEST(Xid, RoundTripsATypicalInternalXid)
{
    Decoded d;
    ASSERT_TRUE(RoundTrip(1, "MySQL-server-uuid-here", "\x01\x02\x03\x04", &d));
    EXPECT_EQ(d.format_id, 1);
    EXPECT_EQ(d.gtrid_len, 22);
    EXPECT_EQ(d.bqual_len, 4);
    EXPECT_EQ(0, memcmp(d.data, "MySQL-server-uuid-here\x01\x02\x03\x04", 26));
}

TEST(Xid, RoundTripsANegativeFormatId)
{
    /* -1 is the server's "null XID" marker and must survive the trip intact
       rather than coming back as a large unsigned value. */
    Decoded d;
    ASSERT_TRUE(RoundTrip(-1, "g", "b", &d));
    EXPECT_EQ(d.format_id, -1);
}

TEST(Xid, RoundTripsTheLargestPermittedXid)
{
    Decoded d;
    ASSERT_TRUE(RoundTrip(7, std::string(64, 'g'), std::string(64, 'b'), &d));
    EXPECT_EQ(d.gtrid_len, 64);
    EXPECT_EQ(d.bqual_len, 64);
    EXPECT_EQ(d.data[0], 'g');
    EXPECT_EQ(d.data[63], 'g');
    EXPECT_EQ(d.data[64], 'b');
    EXPECT_EQ(d.data[127], 'b');
}

TEST(Xid, RoundTripsAnEmptyBranchQualifier)
{
    Decoded d;
    ASSERT_TRUE(RoundTrip(3, "gtrid-only", "", &d));
    EXPECT_EQ(d.bqual_len, 0);
    EXPECT_EQ(0, memcmp(d.data, "gtrid-only", 10));
}

TEST(Xid, EqualIdentitiesEncodeToIdenticalBytes)
{
    /* Two XIDs that the server considers the same must produce the same bytes
       even when the unused tail of data[] differs, because recovery matches on
       the encoded form. */
    char a[TDB_XID_DATA_SIZE];
    char b[TDB_XID_DATA_SIZE];
    memset(a, 0x00, sizeof(a));
    memset(b, 0xff, sizeof(b));
    memcpy(a, "abc", 3);
    memcpy(b, "abc", 3);

    unsigned char ba[TDB_XID_MAX_SERIALIZED], bb[TDB_XID_MAX_SERIALIZED];
    const size_t na = tdb_xid_serialize(1, 3, 0, a, ba, sizeof(ba));
    const size_t nb = tdb_xid_serialize(1, 3, 0, b, bb, sizeof(bb));
    ASSERT_EQ(na, nb);
    ASSERT_NE(na, 0u);
    EXPECT_EQ(0, memcmp(ba, bb, na));
}

TEST(Xid, EncodesOnlyTheMeaningfulPrefix)
{
    char data[TDB_XID_DATA_SIZE] = {};
    memcpy(data, "xy", 2);
    unsigned char buf[TDB_XID_MAX_SERIALIZED];
    EXPECT_EQ(tdb_xid_serialize(1, 2, 0, data, buf, sizeof(buf)), TDB_XID_HEADER_SIZE + 2);
}

TEST(Xid, RefusesLengthsOutsideTheServersRange)
{
    char data[TDB_XID_DATA_SIZE] = {};
    unsigned char buf[TDB_XID_MAX_SERIALIZED];
    EXPECT_EQ(tdb_xid_serialize(1, 65, 0, data, buf, sizeof(buf)), 0u);
    EXPECT_EQ(tdb_xid_serialize(1, 0, 65, data, buf, sizeof(buf)), 0u);
    EXPECT_EQ(tdb_xid_serialize(1, -1, 0, data, buf, sizeof(buf)), 0u);
    EXPECT_EQ(tdb_xid_serialize(1, 0, -1, data, buf, sizeof(buf)), 0u);
}

TEST(Xid, RefusesAnUndersizedOutputBuffer)
{
    char data[TDB_XID_DATA_SIZE] = {};
    unsigned char small[TDB_XID_HEADER_SIZE + 1];
    EXPECT_EQ(tdb_xid_serialize(1, 4, 0, data, small, sizeof(small)), 0u);
}

TEST(Xid, RefusesTruncatedAndOverlongEncodings)
{
    char data[TDB_XID_DATA_SIZE] = {};
    memcpy(data, "abcd", 4);
    unsigned char buf[TDB_XID_MAX_SERIALIZED];
    const size_t n = tdb_xid_serialize(1, 4, 0, data, buf, sizeof(buf));
    ASSERT_NE(n, 0u);

    Decoded d;
    /* a header alone, with the payload cut off */
    EXPECT_FALSE(tdb_xid_deserialize(buf, TDB_XID_HEADER_SIZE, &d.format_id, &d.gtrid_len,
                                     &d.bqual_len, d.data));
    /* shorter than a header */
    EXPECT_FALSE(tdb_xid_deserialize(buf, 4, &d.format_id, &d.gtrid_len, &d.bqual_len, d.data));
    /* a length that disagrees with the byte count is refused rather than
       believed -- this is the corrupt-on-disk case */
    EXPECT_FALSE(
        tdb_xid_deserialize(buf, n + 1, &d.format_id, &d.gtrid_len, &d.bqual_len, d.data));
}

TEST(Xid, RefusesACorruptLengthField)
{
    char data[TDB_XID_DATA_SIZE] = {};
    unsigned char buf[TDB_XID_MAX_SERIALIZED];
    const size_t n = tdb_xid_serialize(1, 4, 0, data, buf, sizeof(buf));
    ASSERT_NE(n, 0u);
    tdb_xid_put_u32(buf + 8, 0xffffffffu); /* gtrid_length as read off a damaged log */

    Decoded d;
    EXPECT_FALSE(tdb_xid_deserialize(buf, n, &d.format_id, &d.gtrid_len, &d.bqual_len, d.data));
}

TEST(Xid, NullInputsAreSafe)
{
    char data[TDB_XID_DATA_SIZE] = {};
    unsigned char buf[TDB_XID_MAX_SERIALIZED];
    EXPECT_EQ(tdb_xid_serialize(1, 4, 0, nullptr, buf, sizeof(buf)), 0u);
    EXPECT_EQ(tdb_xid_serialize(1, 4, 0, data, nullptr, sizeof(buf)), 0u);

    Decoded d;
    EXPECT_FALSE(tdb_xid_deserialize(nullptr, 20, &d.format_id, &d.gtrid_len, &d.bqual_len, d.data));
    EXPECT_FALSE(tdb_xid_deserialize(buf, 20, nullptr, &d.gtrid_len, &d.bqual_len, d.data));
}

TEST(Xid, ZeroLengthXidIsEncodable)
{
    /* formatID -1 with both halves empty is the null XID; it has to survive
       because the server can hand us one. */
    unsigned char buf[TDB_XID_MAX_SERIALIZED];
    const size_t n = tdb_xid_serialize(-1, 0, 0, nullptr, buf, sizeof(buf));
    EXPECT_EQ(n, TDB_XID_HEADER_SIZE);

    Decoded d;
    ASSERT_TRUE(tdb_xid_deserialize(buf, n, &d.format_id, &d.gtrid_len, &d.bqual_len, d.data));
    EXPECT_EQ(d.format_id, -1);
    EXPECT_EQ(d.gtrid_len, 0);
    EXPECT_EQ(d.bqual_len, 0);
}

}  // namespace
