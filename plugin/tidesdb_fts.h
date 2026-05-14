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

/* tidesdb_fts.h
 *
 * Full-Text Search subsystem for the TidesDB MySQL plugin.
 * Extracted from ha_tidesdb.cc as the A-2 architectural extraction
 * recommended in docs/code-review-followup-report.md.
 *
 * What this owns:
 *   - FT_INFO vtables (tdb_ft_vft / tdb_ft_vft_ext) and the
 *     tdb_ft_info_t search context the handler returns from ft_init_ext
 *   - FTS inverted-index key/value codec helpers (build_key/build_value)
 *   - FTS per-index meta counters (load_meta / update_meta), serialized
 *     by TidesDB_share::fts_meta_mutex
 *   - Charset-aware tokenizer with stop-word filtering and blend-char
 *     splitting, plus the document-extract entry point used by the
 *     DML paths
 *   - Boolean query parser (+required -excluded "phrase" word* trunc)
 *     and phrase verifier
 *   - Stop-word and blend-char machinery (sysvar storage + update
 *     callbacks + read-mostly rwlocks)
 *
 * What it borrows:
 *   - `ha_tidesdb` (forward-declared) -- tdb_ft_info_t holds a back
 *     pointer, but the FTS code itself never dereferences handler
 *     members; only ft_init_ext / ft_read in ha_tidesdb.cc do.
 *   - `TidesDB_share` (forward-declared) -- fts_update_meta serializes
 *     load-modify-put on the per-share fts_meta_mutex (HF-2 follow-up
 *     fix moved this off the former process-global mutex).
 *   - Constants live in ha_tidesdb.h (KEY_NS_META, FTS_TERM_*,
 *     FTS_BOOL_OP_*, BM25_*, ROW_HEADER_SIZE, ...). */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "my_inttypes.h"
#include "mysql/psi/mysql_rwlock.h"

extern "C" {
#include <tidesdb/db.h>
}

/* Forward decls -- avoid dragging the full SQL layer into this header.
   ha_tidesdb is needed by tdb_ft_info_t's back-pointer; KEY / FT_INFO /
   FT_INFO_EXT / SYS_VAR / MYSQL_THD are MySQL plugin types that callers
   already have transitively via handler.h. */
class ha_tidesdb;
class TidesDB_share;
struct KEY;
struct _ft_vft;
struct _ft_vft_ext;
struct FT_INFO;
struct FT_INFO_EXT;
struct st_mysql_sys_var;
struct CHARSET_INFO;
struct TABLE;
class THD;

/* The MariaDB MYSQL_THD typedef pulls in sql_class.h; we just take a
   void * in update-callback signatures and cast inside. */

/* ---------------- Public predicates ---------------- */

bool is_fts_index(const KEY *ki);

/* ---------------- FTS search context ---------------- */

/* FTS result entry -- one per matching document */
struct tdb_fts_result_t
{
    uchar *pk; /* heap-allocated comparable PK bytes */
    uint pk_len;
    float rank; /* BM25 score */
};

/* FTS search context returned by ft_init_ext as FT_INFO* */
struct tdb_ft_info_t
{
    struct _ft_vft *please;                /* required by MariaDB FT_INFO layout */
    struct _ft_vft_ext *could_you;         /* extended FT API (HA_CAN_FULLTEXT_EXT) */
    ha_tidesdb *handler;                   /* back-pointer for row fetching */
    uint keynr;                            /* which FTS index */
    std::vector<tdb_fts_result_t> results; /* sorted by rank descending */
    size_t current_idx;                    /* iteration position */
    float current_rank;                    /* rank of last-returned row */
    ulonglong match_count;                 /* total matches for count_matches() */
};

/* FT_INFO vtables exposed so ha_tidesdb.cc::ft_init_ext can wire them
   into newly-allocated tdb_ft_info_t instances. */
extern const struct _ft_vft tdb_ft_vft;
extern struct _ft_vft_ext tdb_ft_vft_ext;

/* ---------------- FTS codec layout ---------------- */

/* Maximum term byte length in the FTS index. Terms longer than this
   are truncated. 512 bytes accommodates even long CJK compound words. */
static constexpr uint FTS_MAX_TERM_BYTES = 512;
static constexpr uint FTS_TERM_LEN_PREFIX = 2;
static constexpr uint FTS_KEY_BUF_LEN = FTS_TERM_LEN_PREFIX + FTS_MAX_TERM_BYTES + MAX_KEY_LENGTH;

static constexpr uint FTS_VALUE_TF_LEN = 2;
static constexpr uint FTS_VALUE_DOC_LEN_OFFSET = FTS_VALUE_TF_LEN;
static constexpr uint FTS_VALUE_DOC_LEN_LEN = 4;
static constexpr uint FTS_VALUE_LEN = FTS_VALUE_TF_LEN + FTS_VALUE_DOC_LEN_LEN;

/* FTS per-index meta key layout. The tag is "FTS\0" (4 bytes incl. NUL). */
extern const char FTS_META_KEY_TAG[];
static constexpr uint FTS_META_KEY_TAG_LEN = 4;
static constexpr uint FTS_META_VALUE_DOCS_LEN = 8;
static constexpr uint FTS_META_VALUE_WORDS_OFFSET = FTS_META_VALUE_DOCS_LEN;
static constexpr uint FTS_META_VALUE_WORDS_LEN = 8;
static constexpr uint FTS_META_VALUE_LEN = FTS_META_VALUE_DOCS_LEN + FTS_META_VALUE_WORDS_LEN;

/* Build an FTS inverted index key:
   [2-byte term_len LE][lowercased term bytes][comparable PK bytes]
   Term is silently truncated to FTS_MAX_TERM_BYTES. */
uint fts_build_key(const char *term, uint term_len, const uchar *pk, uint pk_len, uchar *out);

/* Build FTS value: [2-byte tf LE][4-byte doc_len LE]. */
uint fts_build_value(uint16 tf, uint32 doc_len, uchar *out);

/* Read FTS metadata counters for keynr from the data CF. Returns 0
   even when the meta key is missing (treated as zero-initialized). */
int fts_load_meta(tidesdb_txn_t *txn, tidesdb_column_family_t *data_cf, uint keynr,
                  int64_t *total_docs, int64_t *total_words);

/* Update FTS metadata counters atomically within the current
   transaction. Serialized by `share->fts_meta_mutex`. */
int fts_update_meta(TidesDB_share *share, tidesdb_txn_t *txn,
                    tidesdb_column_family_t *data_cf, uint keynr,
                    int64_t delta_docs, int64_t delta_words);

/* ---------------- Tokenizer ---------------- */

struct fts_token_t
{
    std::string word;
};

/* Charset-aware tokenizer with blend-char support. Emits lowercased
   tokens filtered by srv_fts_min_word_len / srv_fts_max_word_len. */
void fts_tokenize(const char *text, size_t text_len, CHARSET_INFO *cs,
                  std::vector<fts_token_t> &out);

/* Extract and tokenize the document from all FULLTEXT key_part fields.
   Uses a per-thread heap-backed scratch string (MF-8) to avoid the
   dlopen static-TLS limit and per-row reallocation. */
void fts_extract_and_tokenize(TABLE *table, const KEY *key_info, const uchar *record,
                              CHARSET_INFO *cs, std::vector<fts_token_t> &out_tokens);

/* ---------------- Boolean query ---------------- */

struct fts_query_term_t
{
    std::string term;
    int yesno;                             /* +1 = required, -1 = excluded, 0 = optional */
    bool trunc;                            /* prefix match (wildcard) */
    bool is_phrase;                        /* exact phrase match */
    std::vector<std::string> phrase_words; /* lowercased words in the phrase */
};

void fts_parse_boolean(const char *query, size_t len, CHARSET_INFO *cs,
                       std::vector<fts_query_term_t> &out);

/* True when `phrase_words` appears as a consecutive subsequence in
   the already-tokenized `doc_tokens`. */
bool fts_phrase_in_tokens(const std::vector<fts_token_t> &doc_tokens,
                          const std::vector<std::string> &phrase_words);

/* ---------------- Sysvar storage & callbacks ---------------- */

/* These four are storage cells for plugin sysvars. The MYSQL_SYSVAR_*
   declarations live in ha_tidesdb.cc and take their address. */
extern ulong srv_fts_min_word_len;
extern ulong srv_fts_max_word_len;
extern double srv_fts_bm25_k1;
extern double srv_fts_bm25_b;
extern char *srv_fts_blend_chars;
extern char *srv_ft_stopword_table;

/* Sysvar update callbacks. Types match MYSQL_SYSVAR_STR's update_func
   signature, but we keep the THD / SYS_VAR / void* opaque here. */
void tdb_fts_blend_chars_update(void *thd, st_mysql_sys_var *var, void *var_ptr,
                                const void *save);
void tdb_ft_stopword_table_update(void *thd, st_mysql_sys_var *var, void *var_ptr,
                                  const void *save);

/* ---------------- Lifecycle ---------------- */

/* Called once from tidesdb_init_func. Initializes the stopword /
   blend-char rwlocks, loads the default stopword set, and rebuilds
   the blend-char fast-lookup table from srv_fts_blend_chars. */
void tdb_fts_init();

/* Called once from tidesdb_deinit_func. Destroys the rwlocks. */
void tdb_fts_destroy();
