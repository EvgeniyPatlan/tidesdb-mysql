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

/* tidesdb_spatial.h
 *
 * Spatial-index subsystem for the TidesDB MySQL plugin. Hilbert-curve
 * indexing of MBRs (Minimum Bounding Rectangles) for OGC geometry
 * types, with WKB parsing and MariaDB-compatible MBR predicates.
 *
 * Extracted from ha_tidesdb.cc as the spatial half of the A-2
 * architectural extraction. See tidesdb_fts.h for the FTS half.
 *
 * What this owns:
 *   - Hilbert curve encoder / decoder (32-bit per axis, 64-bit output)
 *   - tdb_mbr_t and the MBR predicate dispatch
 *     (intersects / within / contains / equals / disjoint)
 *   - WKB geometry parser covering all 7 OGC types
 *   - Spatial key/value codec (build_key / build_value /
 *     parse_query_mbr) and on-disk size constants
 *   - Hilbert range decomposition for bounding-box queries
 *
 * Constants in ha_tidesdb.h are still the source of truth for
 * IEEE754_DOUBLE_SIGN_MASK, LEX_UINT32_HI_SHIFT, BITS_PER_BYTE,
 * HILBERT_RANGE_FULL_*, MBR_CENTROID_DIV. */

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "my_base.h"      /* enum ha_rkey_function */
#include "my_inttypes.h"

struct KEY;

/* ---------------- Public predicates ---------------- */

bool is_spatial_index(const KEY *ki);

/* ---------------- MBR type ---------------- */

/* Minimum Bounding Rectangle. Layout is load-bearing: ha_tidesdb.cc's
   spatial range-scan path memcpy's an entry value straight onto this
   struct, asserting sizeof(tdb_mbr_t) == SPATIAL_MBR_VALUE_LEN at
   compile time. */
struct tdb_mbr_t
{
    double xmin, ymin, xmax, ymax;
};

/* ---------------- On-disk sizes ---------------- */

static constexpr uint SPATIAL_HILBERT_KEY_LEN = 8; /* 64-bit Hilbert value */
static constexpr uint SPATIAL_MBR_VALUE_LEN = 32;  /* 4 doubles */

/* ---------------- Hilbert curve ---------------- */

/* Convert 2D coordinates to a 64-bit Hilbert curve value. Order 32,
   each axis 32-bit precision. */
uint64_t hilbert_xy2d_64(uint32_t x, uint32_t y);

/* Convert an IEEE 754 double to a uint32 that preserves sort order
   under unsigned integer comparison. Negative values are handled by
   flipping all bits; positive values flip only the sign bit. The
   top 32 bits of the IEEE bit pattern are returned. */
uint32_t double_to_lex_uint32(double val);

/* Encode/decode 8-byte big-endian Hilbert values for lexicographic
   ordering inside the LSM. */
void encode_hilbert_be(uint64_t h, uchar *out);
uint64_t decode_hilbert_be(const uchar *in);

/* ---------------- WKB geometry parsing ---------------- */

/* Extract MBR from a GEOMETRY field's raw data (SRID prefix + WKB).
   Supports all 7 OGC geometry types. Rejects malformed data and
   coordinates with NaN/Inf values. */
bool spatial_compute_mbr(const uchar *data, size_t len, double *xmin, double *ymin,
                         double *xmax, double *ymax);

/* ---------------- Codec ---------------- */

/* Build spatial index key: [hilbert_value 8B BE][pk_bytes]. */
uint spatial_build_key(double cx, double cy, const uchar *pk, uint pk_len, uchar *out);

/* Build spatial index value: [xmin 8B][ymin 8B][xmax 8B][ymax 8B]. */
void spatial_build_value(double xmin, double ymin, double xmax, double ymax, uchar *out);

/* Parse MBR from MariaDB's spatial key buffer layout
   ([xmin 8B][xmax 8B][ymin 8B][ymax 8B]). */
void spatial_parse_query_mbr(const uchar *key, tdb_mbr_t *mbr);

/* ---------------- Predicates ---------------- */

/* MBR predicate dispatch. Returns true if the entry MBR matches the
   query predicate under MariaDB's MBR class semantics for the given
   ha_rkey_function mode. */
bool spatial_mbr_predicate(ha_rkey_function mode, const tdb_mbr_t *query,
                           const tdb_mbr_t *entry);

/* ---------------- Range decomposition ---------------- */

/* Decompose a 32-bit quantized bounding box into a vector of
   contiguous Hilbert-value ranges. Each range corresponds to one
   forward scan in the LSM. */
void spatial_decompose_ranges(uint32_t qx_min, uint32_t qy_min, uint32_t qx_max,
                              uint32_t qy_max,
                              std::vector<std::pair<uint64_t, uint64_t>> &out);
