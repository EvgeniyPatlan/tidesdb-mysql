/* Copyright (c) 2026 TidesDB Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. */

/*
 * Spatial-index subsystem implementation. Hilbert-curve indexing
 * of MBRs over OGC geometry types, with WKB parsing and MariaDB-
 * compatible MBR predicates.
 *
 * Extracted from ha_tidesdb.cc as the spatial half of A-2. See
 * tidesdb_spatial.h for the public surface and the borrowed-
 * from-ha_tidesdb.h constants this file depends on.
 */

#include "ha_tidesdb.h"  /* IEEE754_DOUBLE_SIGN_MASK, LEX_UINT32_HI_SHIFT,
                            BITS_PER_BYTE, HILBERT_RANGE_FULL_LO/HI */
#include "tidesdb_spatial.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "my_byteorder.h"
#include "my_dbug.h"
#include "sql/key.h"

/* ******************** Hilbert curve constants ******************** */

static constexpr uint HILBERT_ORDER = 32;                           /* bits per axis */
static constexpr uint HILBERT_DIM = 2;                              /* 2D curve (x, y) */
static constexpr uint64_t HILBERT_N = (uint64_t)1 << HILBERT_ORDER; /* 2^32 */

/* ******************** Predicate ******************** */

bool is_spatial_index(const KEY *ki)
{
    return ki->algorithm == HA_KEY_ALG_RTREE;
}

/* ******************** Lex-comparable double ******************** */

uint32_t double_to_lex_uint32(double val)
{
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    if (bits & IEEE754_DOUBLE_SIGN_MASK)
        bits = ~bits; /* negative, flip all bits */
    else
        bits ^= IEEE754_DOUBLE_SIGN_MASK;           /* positive, flip sign bit only */
    return (uint32_t)(bits >> LEX_UINT32_HI_SHIFT); /* top 32 bits for precision */
}

/* ******************** Hilbert curve encoder ******************** */

/* Inner rotation step of the iterative xy2d transform (Skilling 2004
   / Wikipedia). `n` is uint64_t because hilbert_xy2d_64 passes 2^32
   on the first iteration of a 32-bit-per-axis curve. */
static inline void hilbert_rot(uint64_t n, uint32_t *x, uint32_t *y, uint32_t rx, uint32_t ry)
{
    if (ry == 0)
    {
        if (rx == 1)
        {
            *x = (uint32_t)(n - 1) - *x;
            *y = (uint32_t)(n - 1) - *y;
        }
        uint32_t t = *x;
        *x = *y;
        *y = t;
    }
}

uint64_t hilbert_xy2d_64(uint32_t x, uint32_t y)
{
    uint64_t d = 0;
    for (uint64_t s = HILBERT_N >> 1; s > 0; s >>= 1)
    {
        uint32_t rx = (x & s) > 0 ? 1 : 0;
        uint32_t ry = (y & s) > 0 ? 1 : 0;
        d += s * s * (uint64_t)((3 * rx) ^ ry);
        hilbert_rot(s << 1, &x, &y, rx, ry);
    }
    return d;
}

void encode_hilbert_be(uint64_t h, uchar *out)
{
    for (uint i = 0; i < SPATIAL_HILBERT_KEY_LEN; i++)
        out[i] = (uchar)(h >> ((SPATIAL_HILBERT_KEY_LEN - 1 - i) * BITS_PER_BYTE));
}

uint64_t decode_hilbert_be(const uchar *in)
{
    uint64_t h = 0;
    for (uint i = 0; i < SPATIAL_HILBERT_KEY_LEN; i++) h = (h << BITS_PER_BYTE) | (uint64_t)in[i];
    return h;
}

/* ******************** WKB geometry parser ******************** */

/* WKB geometry type constants */
static constexpr uint32_t WKB_POINT = 1;
static constexpr uint32_t WKB_LINESTRING = 2;
static constexpr uint32_t WKB_POLYGON = 3;
static constexpr uint32_t WKB_MULTIPOINT = 4;
static constexpr uint32_t WKB_MULTILINESTRING = 5;
static constexpr uint32_t WKB_MULTIPOLYGON = 6;
static constexpr uint32_t WKB_GEOMETRYCOLLECTION = 7;

/* Limits to reject malformed WKB data */
static constexpr uint32_t WKB_MAX_POINTS = 1000000;
static constexpr uint32_t WKB_MAX_RINGS = 10000;
static constexpr uint32_t WKB_MAX_GEOMS = 100000;
static constexpr uint SPATIAL_SRID_SIZE = 4;
static constexpr uint SPATIAL_WKB_HEADER_SIZE = 5;  /* 1 byte_order + 4 type */
static constexpr uint SPATIAL_POINT_DATA_SIZE = 16; /* 2 doubles (x, y) */
static constexpr uint WKB_COUNT_SIZE = sizeof(uint32_t);

/* Parts-of-MBR encoding. build_value writes [xmin,ymin,xmax,ymax];
   parse_query_mbr reads MariaDB's [xmin,xmax,ymin,ymax]. */
static constexpr uint MBR_DOUBLE_SIZE = sizeof(double);
static constexpr uint MBR_OFFSET_SECOND = 1 * MBR_DOUBLE_SIZE;
static constexpr uint MBR_OFFSET_THIRD = 2 * MBR_DOUBLE_SIZE;
static constexpr uint MBR_OFFSET_FOURTH = 3 * MBR_DOUBLE_SIZE;

/* Read a coordinate pair from WKB and expand MBR. Skips NaN/Inf. */
static inline bool wkb_read_point(const uchar *&pp, const uchar *ee, double &mn_x, double &mn_y,
                                  double &mx_x, double &mx_y)
{
    if (pp + SPATIAL_POINT_DATA_SIZE > ee) return false;
    double x, y;
    x = float8get(pp);
    y = float8get(pp + MBR_DOUBLE_SIZE);
    pp += SPATIAL_POINT_DATA_SIZE;
    if (std::isfinite(x) && std::isfinite(y))
    {
        if (x < mn_x) mn_x = x;
        if (x > mx_x) mx_x = x;
        if (y < mn_y) mn_y = y;
        if (y > mx_y) mx_y = y;
    }
    return true;
}

/* Read a point sequence ([num_points 4B][x,y pairs...]) and expand
   MBR. Used by LINESTRING and each POLYGON ring. */
static inline bool wkb_read_point_sequence(const uchar *&pp, const uchar *ee, double &mn_x,
                                           double &mn_y, double &mx_x, double &mx_y)
{
    if (pp + WKB_COUNT_SIZE > ee) return false;
    uint32_t n_pts;
    memcpy(&n_pts, pp, WKB_COUNT_SIZE);
    pp += WKB_COUNT_SIZE;
    if (n_pts > WKB_MAX_POINTS) return false;
    for (uint32_t i = 0; i < n_pts; i++)
    {
        if (!wkb_read_point(pp, ee, mn_x, mn_y, mx_x, mx_y)) return false;
    }
    return true;
}

/* Recursive WKB geometry parser. Reads one geometry object from pp,
   expanding the MBR to include all coordinate pairs. Supports all
   7 OGC geometry types. */
static bool wkb_parse_geometry(const uchar *&pp, const uchar *ee, double &mn_x, double &mn_y,
                               double &mx_x, double &mx_y)
{
    if (pp + SPATIAL_WKB_HEADER_SIZE > ee) return false;
        /* MariaDB stores WKB in native byte order. We rely on native
           order for the memcpy reads below; debug builds assert. */
#ifndef DBUG_OFF
    {
        const uint32_t endian_probe = 1;
        uchar native_byte_order = *(const uchar *)&endian_probe; /* 1 on LE, 0 on BE */
        DBUG_ASSERT(*pp == native_byte_order);
    }
#endif
    pp++; /* skip byte_order */
    uint32_t gt;
    memcpy(&gt, pp, WKB_COUNT_SIZE);
    pp += WKB_COUNT_SIZE;

    switch (gt)
    {
        case WKB_POINT:
            return wkb_read_point(pp, ee, mn_x, mn_y, mx_x, mx_y);

        case WKB_LINESTRING:
            return wkb_read_point_sequence(pp, ee, mn_x, mn_y, mx_x, mx_y);

        case WKB_POLYGON:
        {
            if (pp + WKB_COUNT_SIZE > ee) return false;
            uint32_t n_rings;
            memcpy(&n_rings, pp, WKB_COUNT_SIZE);
            pp += WKB_COUNT_SIZE;
            if (n_rings > WKB_MAX_RINGS) return false;
            for (uint32_t r = 0; r < n_rings; r++)
            {
                if (!wkb_read_point_sequence(pp, ee, mn_x, mn_y, mx_x, mx_y)) return false;
            }
            return true;
        }

        case WKB_MULTIPOINT:
        case WKB_MULTILINESTRING:
        case WKB_MULTIPOLYGON:
        case WKB_GEOMETRYCOLLECTION:
        {
            if (pp + WKB_COUNT_SIZE > ee) return false;
            uint32_t n_geoms;
            memcpy(&n_geoms, pp, WKB_COUNT_SIZE);
            pp += WKB_COUNT_SIZE;
            if (n_geoms > WKB_MAX_GEOMS) return false;
            for (uint32_t i = 0; i < n_geoms; i++)
            {
                if (!wkb_parse_geometry(pp, ee, mn_x, mn_y, mx_x, mx_y)) return false;
            }
            return true;
        }

        default:
            return false;
    }
}

bool spatial_compute_mbr(const uchar *data, size_t len, double *xmin, double *ymin,
                         double *xmax, double *ymax)
{
    if (len < SPATIAL_SRID_SIZE + SPATIAL_WKB_HEADER_SIZE) return false;

    const uchar *p = data + SPATIAL_SRID_SIZE;
    const uchar *end = data + len;

    *xmin = *ymin = DBL_MAX;
    *xmax = *ymax = -DBL_MAX;

    if (!wkb_parse_geometry(p, end, *xmin, *ymin, *xmax, *ymax)) return false;

    /* Validate that we accumulated valid coordinates */
    return *xmin <= *xmax && *ymin <= *ymax;
}

/* ******************** Codec ******************** */

uint spatial_build_key(double cx, double cy, const uchar *pk, uint pk_len, uchar *out)
{
    uint32_t qx = double_to_lex_uint32(cx);
    uint32_t qy = double_to_lex_uint32(cy);
    uint64_t h = hilbert_xy2d_64(qx, qy);
    encode_hilbert_be(h, out);
    memcpy(out + SPATIAL_HILBERT_KEY_LEN, pk, pk_len);
    return SPATIAL_HILBERT_KEY_LEN + pk_len;
}

void spatial_build_value(double xmin, double ymin, double xmax, double ymax, uchar *out)
{
    memcpy(out, &xmin, MBR_DOUBLE_SIZE);
    memcpy(out + MBR_OFFSET_SECOND, &ymin, MBR_DOUBLE_SIZE);
    memcpy(out + MBR_OFFSET_THIRD, &xmax, MBR_DOUBLE_SIZE);
    memcpy(out + MBR_OFFSET_FOURTH, &ymax, MBR_DOUBLE_SIZE);
}

void spatial_parse_query_mbr(const uchar *key, tdb_mbr_t *mbr)
{
    mbr->xmin = float8get(key);
    mbr->xmax = float8get(key + MBR_OFFSET_SECOND);
    mbr->ymin = float8get(key + MBR_OFFSET_THIRD);
    mbr->ymax = float8get(key + MBR_OFFSET_FOURTH);
}

/* ******************** MBR predicates ******************** */

static inline bool mbr_intersects(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return !(a->xmax < b->xmin || a->xmin > b->xmax || a->ymax < b->ymin || a->ymin > b->ymax);
}

static inline bool mbr_within(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return a->xmin >= b->xmin && a->xmax <= b->xmax && a->ymin >= b->ymin && a->ymax <= b->ymax;
}

static inline bool mbr_equals(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return a->xmin == b->xmin && a->xmax == b->xmax && a->ymin == b->ymin && a->ymax == b->ymax;
}

static inline bool mbr_disjoint(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return !mbr_intersects(a, b);
}

bool spatial_mbr_predicate(ha_rkey_function mode, const tdb_mbr_t *query,
                           const tdb_mbr_t *entry)
{
    /* MariaDB MBR semantics:
       - MBRContains(search_geom, col)  HA_READ_MBR_CONTAIN  -> col within search
       - MBRWithin(col, search_geom)    HA_READ_MBR_WITHIN   -> col within search
       - MBRIntersects                  symmetric */
    switch (mode)
    {
        case HA_READ_MBR_INTERSECT:
            return mbr_intersects(entry, query);
        case HA_READ_MBR_CONTAIN:
            return mbr_within(entry, query);
        case HA_READ_MBR_WITHIN:
            return mbr_within(entry, query);
        case HA_READ_MBR_EQUAL:
            return mbr_equals(entry, query);
        case HA_READ_MBR_DISJOINT:
            return mbr_disjoint(entry, query);
        default:
            return false;
    }
}

/* ******************** Hilbert range decomposition ******************** */

/* SPATIAL_DECOMP_BITS=8 -> 256x256 grid, typically 10-50 merged ranges
   for a small query box. */
static constexpr uint SPATIAL_DECOMP_BITS = 8;
static constexpr uint SPATIAL_DECOMP_N = 1u << SPATIAL_DECOMP_BITS;

void spatial_decompose_ranges(uint32_t qx_min, uint32_t qy_min, uint32_t qx_max, uint32_t qy_max,
                              std::vector<std::pair<uint64_t, uint64_t>> &out)
{
    out.clear();

    uint shift = HILBERT_ORDER - SPATIAL_DECOMP_BITS;
    uint gx0 = qx_min >> shift;
    uint gy0 = qy_min >> shift;
    uint gx1 = qx_max >> shift;
    uint gy1 = qy_max >> shift;

    if (gx1 >= SPATIAL_DECOMP_N) gx1 = SPATIAL_DECOMP_N - 1;
    if (gy1 >= SPATIAL_DECOMP_N) gy1 = SPATIAL_DECOMP_N - 1;

    std::vector<uint64_t> cells;
    cells.reserve((gx1 - gx0 + 1) * (gy1 - gy0 + 1));
    for (uint gx = gx0; gx <= gx1; gx++)
    {
        for (uint gy = gy0; gy <= gy1; gy++)
        {
            /* Coarse cell (gx, gy) maps to fine Hilbert range
               [h << (2*shift), (h+1) << (2*shift) - 1]. */
            uint64_t h = hilbert_xy2d_64(gx << shift, gy << shift);
            cells.push_back(h);
        }
    }

    if (cells.empty())
    {
        out.push_back({HILBERT_RANGE_FULL_LO, HILBERT_RANGE_FULL_HI});
        return;
    }

    std::sort(cells.begin(), cells.end());

    /* Each coarse cell covers 2^(HILBERT_DIM*shift) fine values. */
    uint64_t cell_span = (uint64_t)1 << (HILBERT_DIM * shift);

    uint64_t range_lo = cells[0];
    uint64_t range_hi = cells[0] + cell_span - 1;

    for (size_t i = 1; i < cells.size(); i++)
    {
        uint64_t lo = cells[i];
        uint64_t hi = cells[i] + cell_span - 1;

        if (lo <= range_hi + 1)
        {
            if (hi > range_hi) range_hi = hi;
        }
        else
        {
            out.push_back({range_lo, range_hi});
            range_lo = lo;
            range_hi = hi;
        }
    }
    out.push_back({range_lo, range_hi});
}
