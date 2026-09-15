/*
  Copyright (c) 2026 TidesDB Corp.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/
#pragma once

/*
  Wire format for the transaction id TidesDB records at prepare.

  The server hands us an XID as three integers plus a fixed 128-byte buffer of
  which only the first gtrid_length + bqual_length bytes are meaningful. The
  engine wants one opaque byte string, and that string has to survive a restart
  and still compare equal to the XID the server asks about during recovery --
  so the encoding is explicit rather than a memcpy of the struct. A struct copy
  would carry the padding and the unused tail of data[], which are not part of
  the identity and are not guaranteed to be stable.

  Layout, little-endian:

    0  .. 7   formatID, signed
    8  .. 11  gtrid_length
    12 .. 15  bqual_length
    16 ..     gtrid_length + bqual_length bytes of data

  Only the meaningful prefix of data[] is stored, so two XIDs that are equal by
  the server's definition encode to identical byte strings -- which is what
  lets recovery match on the encoded form and never reconstruct to compare.

  Header-only and free of server headers on purpose: the functions are pure, so
  tests can cover the round trip without linking mysqld.
*/

#include <cstddef>
#include <cstdint>
#include <cstring>

/* The server's XIDDATASIZE. Asserted against the real constant where the
   server headers are in scope, so a change upstream cannot pass silently. */
static constexpr size_t TDB_XID_DATA_SIZE = 128;

/* Each half is documented as 1..64, and their sum cannot exceed the buffer. */
static constexpr long TDB_XID_MAX_PART = 64;

static constexpr size_t TDB_XID_HEADER_SIZE = 16;
static constexpr size_t TDB_XID_MAX_SERIALIZED = TDB_XID_HEADER_SIZE + TDB_XID_DATA_SIZE;

inline void tdb_xid_put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

inline uint32_t tdb_xid_get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

inline void tdb_xid_put_u64(unsigned char *p, uint64_t v)
{
    tdb_xid_put_u32(p, (uint32_t)(v & 0xffffffffULL));
    tdb_xid_put_u32(p + 4, (uint32_t)((v >> 32) & 0xffffffffULL));
}

inline uint64_t tdb_xid_get_u64(const unsigned char *p)
{
    return (uint64_t)tdb_xid_get_u32(p) | ((uint64_t)tdb_xid_get_u32(p + 4) << 32);
}

/* True when the three integers describe an XID the server could have produced.
   Bounds are checked on both encode and decode: on encode because a malformed
   length would read past data[], and on decode because the bytes came off disk
   and a corrupt length would do the same on the way back out. */
inline bool tdb_xid_lengths_valid(long gtrid_len, long bqual_len)
{
    if (gtrid_len < 0 || bqual_len < 0) return false;
    if (gtrid_len > TDB_XID_MAX_PART || bqual_len > TDB_XID_MAX_PART) return false;
    return (size_t)(gtrid_len + bqual_len) <= TDB_XID_DATA_SIZE;
}

/* Encodes into out, returning the byte count, or 0 when the input is not a
   well-formed XID or out_cap is too small. */
inline size_t tdb_xid_serialize(long format_id, long gtrid_len, long bqual_len, const char *data,
                                unsigned char *out, size_t out_cap)
{
    if (!out || (!data && (gtrid_len + bqual_len) > 0)) return 0;
    if (!tdb_xid_lengths_valid(gtrid_len, bqual_len)) return 0;

    const size_t payload = (size_t)(gtrid_len + bqual_len);
    const size_t total = TDB_XID_HEADER_SIZE + payload;
    if (out_cap < total) return 0;

    tdb_xid_put_u64(out, (uint64_t)(int64_t)format_id);
    tdb_xid_put_u32(out + 8, (uint32_t)gtrid_len);
    tdb_xid_put_u32(out + 12, (uint32_t)bqual_len);
    if (payload > 0) memcpy(out + TDB_XID_HEADER_SIZE, data, payload);
    return total;
}

/* Decodes what tdb_xid_serialize wrote. data must have room for
   TDB_XID_DATA_SIZE bytes; the tail past the payload is zeroed, so the result
   compares equal to a freshly constructed XID carrying the same identity.
   Returns false on anything that is not a well-formed encoding. */
inline bool tdb_xid_deserialize(const unsigned char *in, size_t in_len, long *format_id,
                                long *gtrid_len, long *bqual_len, char *data)
{
    if (!in || !format_id || !gtrid_len || !bqual_len || !data) return false;
    if (in_len < TDB_XID_HEADER_SIZE) return false;

    const long g = (long)(int32_t)tdb_xid_get_u32(in + 8);
    const long b = (long)(int32_t)tdb_xid_get_u32(in + 12);
    if (!tdb_xid_lengths_valid(g, b)) return false;
    if (in_len != TDB_XID_HEADER_SIZE + (size_t)(g + b)) return false;

    *format_id = (long)(int64_t)tdb_xid_get_u64(in);
    *gtrid_len = g;
    *bqual_len = b;
    memset(data, 0, TDB_XID_DATA_SIZE);
    if (g + b > 0) memcpy(data, in + TDB_XID_HEADER_SIZE, (size_t)(g + b));
    return true;
}
