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
 * Full-Text Search subsystem implementation.
 *
 * Extracted from ha_tidesdb.cc as the A-2 architectural extraction.
 * See tidesdb_fts.h for the public surface and the borrowed-from-
 * ha_tidesdb.cc symbols this file depends on.
 */

#include "ha_tidesdb.h"  /* TidesDB_share, KEY_NS_META, KEY_NAMESPACE_LEN, ROW_HEADER_SIZE,
                            NULL_BITMAP_SINGLE_FIELD, FIELD_VARCHAR_LEN_PREFIX,
                            FTS_TERM_*, FTS_BOOL_OP_*, tdb_sanitize_for_log */
#include "tidesdb_engine_context.h"  /* tdb_get_engine */
#include "tidesdb_fts.h"

#include <algorithm>
#include <cstring>

#include "m_string.h"
#include "my_byteorder.h"
#include "my_sys.h"
#include "mysql/plugin.h"
#include "mysql/psi/mysql_rwlock.h"
#include "mysql/service_my_plugin_log.h"
#include "mysqld_error.h"
#include "sql/field.h"
#include "sql/key.h"
#include "sql/sql_class.h"  /* THD, current_thd, Security_context, check_access */
#include "sql/table.h"

/* ******************** Storage for sysvars & vtables ******************** */

const char FTS_META_KEY_TAG[FTS_META_KEY_TAG_LEN] = {'F', 'T', 'S', '\x00'};
static constexpr uint FTS_META_KEY_TAG_OFFSET = KEY_NAMESPACE_LEN;
static constexpr uint FTS_META_KEY_KEYNR_OFFSET = FTS_META_KEY_TAG_OFFSET + FTS_META_KEY_TAG_LEN;
static constexpr uint FTS_META_KEY_LEN = FTS_META_KEY_KEYNR_OFFSET + 1;

/* Sysvar storage cells. Their addresses are taken by MYSQL_SYSVAR_*
   in ha_tidesdb.cc. Defaults documented at the declaration in
   tidesdb_fts.h. */
ulong srv_fts_min_word_len = 3;
ulong srv_fts_max_word_len = 84;
double srv_fts_bm25_k1 = 1.2;
double srv_fts_bm25_b = 0.75;
char *srv_fts_blend_chars = NULL;
char *srv_ft_stopword_table = NULL;

/* ******************** Predicate ******************** */

bool is_fts_index(const KEY *ki)
{
    return ki->algorithm == HA_KEY_ALG_FULLTEXT;
}

/* ******************** FT_INFO vtable callbacks ******************** */

/* Forward decls scoped to this TU. */
static int tdb_fts_read_next(FT_INFO *, char *);
static float tdb_fts_find_relevance(FT_INFO *, uchar *, uint);
static void tdb_fts_close_search(FT_INFO *);
static float tdb_fts_get_relevance(FT_INFO *);
static void tdb_fts_reinit_search(FT_INFO *);

const struct _ft_vft tdb_ft_vft = {tdb_fts_read_next, tdb_fts_find_relevance,
                                   tdb_fts_close_search, tdb_fts_get_relevance,
                                   tdb_fts_reinit_search};

static uint tdb_fts_get_version()
{
    return 2;
}

static ulonglong tdb_fts_get_flags()
{
    return FTS_ORDERED_RESULT;
}

static ulonglong tdb_fts_get_docid(FT_INFO_EXT *fts)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    if (info->current_idx > 0 && info->current_idx <= info->results.size())
        return (ulonglong)(info->current_idx); /* 1-based doc ID */
    return 0;
}

static ulonglong tdb_fts_count_matches(FT_INFO_EXT *fts)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    return info->match_count;
}

struct _ft_vft_ext tdb_ft_vft_ext = {tdb_fts_get_version, tdb_fts_get_flags,
                                     tdb_fts_get_docid, tdb_fts_count_matches};

static int tdb_fts_read_next(FT_INFO *, char *)
{
    return HA_ERR_END_OF_FILE; /* not used -- ft_read() is the entry point */
}

static float tdb_fts_find_relevance(FT_INFO *fts, uchar *, uint)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    return info->current_rank;
}

static float tdb_fts_get_relevance(FT_INFO *fts)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    return info->current_rank;
}

static void tdb_fts_close_search(FT_INFO *fts)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    for (auto &r : info->results) my_free(r.pk);
    delete info;
}

static void tdb_fts_reinit_search(FT_INFO *fts)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    info->current_idx = 0;
}

/* ******************** FTS codec ******************** */

uint fts_build_key(const char *term, uint term_len, const uchar *pk, uint pk_len, uchar *out)
{
    if (term_len > FTS_MAX_TERM_BYTES) term_len = FTS_MAX_TERM_BYTES;
    uint pos = 0;
    int2store(out + pos, (uint16)term_len);
    pos += FTS_TERM_LEN_PREFIX;
    memcpy(out + pos, term, term_len);
    pos += term_len;
    memcpy(out + pos, pk, pk_len);
    pos += pk_len;
    return pos;
}

uint fts_build_value(uint16 tf, uint32 doc_len, uchar *out)
{
    int2store(out, tf);
    int4store(out + FTS_VALUE_DOC_LEN_OFFSET, doc_len);
    return FTS_VALUE_LEN;
}

int fts_load_meta(tidesdb_txn_t *txn, tidesdb_column_family_t *data_cf, uint keynr,
                  int64_t *total_docs, int64_t *total_words)
{
    uchar mk[FTS_META_KEY_LEN];
    mk[0] = KEY_NS_META;
    memcpy(mk + FTS_META_KEY_TAG_OFFSET, FTS_META_KEY_TAG, FTS_META_KEY_TAG_LEN);
    mk[FTS_META_KEY_KEYNR_OFFSET] = (uchar)keynr;

    uint8_t *val = NULL;
    size_t vlen = 0;
    *total_docs = 0;
    *total_words = 0;

    int rc = tidesdb_txn_get(txn, data_cf, mk, FTS_META_KEY_LEN, &val, &vlen);
    if (rc == TDB_SUCCESS && vlen >= FTS_META_VALUE_LEN)
    {
        *total_docs = sint8korr(val);
        *total_words = sint8korr(val + FTS_META_VALUE_WORDS_OFFSET);
        tidesdb_free(val);
    }
    else if (val)
    {
        tidesdb_free(val);
    }
    return 0;
}

int fts_update_meta(TidesDB_share *share, tidesdb_txn_t *txn,
                    tidesdb_column_family_t *data_cf, uint keynr,
                    int64_t delta_docs, int64_t delta_words)
{
    mysql_mutex_lock(&share->fts_meta_mutex);

    int64_t total_docs = 0, total_words = 0;
    fts_load_meta(txn, data_cf, keynr, &total_docs, &total_words);

    total_docs += delta_docs;
    total_words += delta_words;
    if (total_docs < 0) total_docs = 0;
    if (total_words < 0) total_words = 0;

    uchar mk[FTS_META_KEY_LEN];
    mk[0] = KEY_NS_META;
    memcpy(mk + FTS_META_KEY_TAG_OFFSET, FTS_META_KEY_TAG, FTS_META_KEY_TAG_LEN);
    mk[FTS_META_KEY_KEYNR_OFFSET] = (uchar)keynr;

    uchar mv[FTS_META_VALUE_LEN];
    int8store(mv, total_docs);
    int8store(mv + FTS_META_VALUE_WORDS_OFFSET, total_words);
    int rc = tidesdb_txn_put(txn, data_cf, mk, FTS_META_KEY_LEN, mv, FTS_META_VALUE_LEN,
                             TIDESDB_TTL_NONE);

    mysql_mutex_unlock(&share->fts_meta_mutex);
    return rc;
}

/* ******************** Blend-char fast lookup ******************** */

static constexpr uint TDB_BLEND_MAP_SIZE = 256;
static bool tdb_blend_char_map[TDB_BLEND_MAP_SIZE] = {false};
static mysql_rwlock_t tdb_blend_lock;
static PSI_rwlock_key tdb_blend_lock_key;

static void tdb_rebuild_blend_map(const char *chars)
{
    memset(tdb_blend_char_map, 0, sizeof(tdb_blend_char_map));
    if (!chars) return;
    for (const char *p = chars; *p; p++) tdb_blend_char_map[(unsigned char)*p] = true;
}

void tdb_fts_blend_chars_update(void *, st_mysql_sys_var *, void *var_ptr, const void *save)
{
    const char *new_val = *static_cast<const char *const *>(save);
    mysql_rwlock_wrlock(&tdb_blend_lock);
    tdb_rebuild_blend_map(new_val);
    mysql_rwlock_unlock(&tdb_blend_lock);
    *static_cast<const char **>(var_ptr) = new_val;
    if (new_val && new_val[0])
        sql_print_information("[TIDESDB] FTS blend_chars set to '%s'", new_val);
    else
        sql_print_information("[TIDESDB] FTS blend_chars cleared");
}

/* ******************** Stop-word machinery ******************** */

/* InnoDB's default 36 stop words, matching INNODB_FT_DEFAULT_STOPWORD */
static const char *tdb_default_stopwords[] = {
    "a",   "about", "an",   "are", "as",   "at",  "be",  "by",   "com",  "de",
    "en",  "for",   "from", "how", "i",    "in",  "is",  "it",   "la",   "of",
    "on",  "or",    "that", "the", "this", "to",  "was", "what", "when", "where",
    "who", "will",  "with", "und", "the",  "www", NULL};

static std::unordered_set<std::string> tdb_stopwords;
static mysql_rwlock_t tdb_stopword_lock;
static PSI_rwlock_key tdb_stopword_lock_key;

static void tdb_load_default_stopwords_into(std::unordered_set<std::string> &out)
{
    out.clear();
    for (const char **w = tdb_default_stopwords; *w; w++) out.insert(*w);
}

/* Init-path wrapper that writes directly to the global tdb_stopwords
   (init runs single-threaded before the rwlock is initialized). */
static void tdb_load_default_stopwords()
{
    tdb_load_default_stopwords_into(tdb_stopwords);
}

/* Check if a lowercased token is a stop word.
   PRECONDITION caller holds tdb_stopword_lock for reading. */
static inline bool tdb_is_stopword_locked(const std::string &word)
{
    return tdb_stopwords.count(word) > 0;
}

/* Load stop words from a user table specified as "db_name/table_name"
   into `out`. The CF scan can take arbitrarily long; callers should
   build a local set with this function WITHOUT holding the stopword
   write-lock, then swap the populated set into tdb_stopwords under
   the lock at the end (M-11 in the original review). */
static bool tdb_load_stopwords_from_table_spec_into(const char *table_spec,
                                                    std::unordered_set<std::string> &out)
{
    if (!table_spec || !table_spec[0]) return false;

    /* MF-6: sanitize the user-supplied spec for every log line below.
       SET GLOBAL tidesdb_ft_stopword_table=... accepts arbitrary
       strings; an attacker with SYSTEM_VARIABLES_ADMIN could embed
       newlines / escape sequences and fool the error log parser. */
    char sanitized_spec[256];
    tdb_sanitize_for_log(table_spec, sanitized_spec, sizeof(sanitized_spec));

    const char *slash = strchr(table_spec, '/');
    if (!slash)
    {
        sql_print_error("[TIDESDB] ft_stopword_table format must be 'db_name/table_name', got '%s'",
                        sanitized_spec);
        return false;
    }

    std::string db_name(table_spec, slash - table_spec);
    std::string tbl_name(slash + 1);

    /* HF-1 fix: fail-closed on missing THD or security context. The
       previous form fell through to opening the CF when either was
       null, letting a system thread / plugin init / bootstrap bypass
       the privilege gate. */
    THD *cur_thd = current_thd;
    if (!cur_thd || !cur_thd->security_context())
    {
        sql_print_warning(
            "[TIDESDB] Stop word table load denied: no user security "
            "context (table_spec='%s')",
            sanitized_spec);
        return false;
    }
    Security_context *sctx = cur_thd->security_context();
    if (!sctx->check_access(SELECT_ACL, db_name, false))
    {
        char sanitized_db[256];
        tdb_sanitize_for_log(db_name.c_str(), sanitized_db, sizeof(sanitized_db));
        sql_print_warning(
            "[TIDESDB] Stop word table load denied: user '%s'@'%s' lacks "
            "SELECT on database '%s'. Refusing to load stop words from '%s'.",
            sctx->priv_user().str ? sctx->priv_user().str : "?",
            sctx->priv_host().str ? sctx->priv_host().str : "?",
            sanitized_db, sanitized_spec);
        return false;
    }

    /* Look for a TidesDB CF matching the table path convention. Cache
       the engine pointer locally so truthy check and use are guaranteed
       to see the same value. */
    std::string cf_name = db_name + "/" + tbl_name;
    tidesdb_t *engine = tdb_get_engine();
    tidesdb_column_family_t *sw_cf =
        engine ? tidesdb_get_column_family(engine, cf_name.c_str()) : NULL;

    if (!sw_cf)
    {
        char sanitized_cf[256];
        tdb_sanitize_for_log(cf_name.c_str(), sanitized_cf, sizeof(sanitized_cf));
        sql_print_warning(
            "[TIDESDB] Stop word table '%s' not found as TidesDB CF '%s'. "
            "The table must be a TidesDB ENGINE table. Keeping current stop words.",
            sanitized_spec, sanitized_cf);
        return false;
    }

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_get_engine(), &txn) != TDB_SUCCESS) return false;

    tidesdb_iter_t *iter = NULL;
    if (tidesdb_iter_new(txn, sw_cf, &iter) != TDB_SUCCESS)
    {
        tidesdb_txn_free(txn);
        return false;
    }

    tidesdb_iter_seek_to_first(iter);
    out.clear();

    while (tidesdb_iter_valid(iter))
    {
        uint8_t *val = NULL;
        size_t val_size = 0;
        if (tidesdb_iter_value(iter, &val, &val_size) == TDB_SUCCESS && val && val_size > 0)
        {
            /* Row is in packed format: [header][null_bitmap][fields...].
               For a single-column VARCHAR table, value starts after
               ROW_HEADER_SIZE + 1 byte of null bitmap. */
            const uint8_t *data = val;
            size_t data_len = val_size;

            if (data_len > ROW_HEADER_SIZE)
            {
                data += ROW_HEADER_SIZE;
                data_len -= ROW_HEADER_SIZE;
                if (data_len > NULL_BITMAP_SINGLE_FIELD)
                {
                    data += NULL_BITMAP_SINGLE_FIELD;
                    data_len -= NULL_BITMAP_SINGLE_FIELD;
                }
            }

            if (data_len >= FIELD_VARCHAR_LEN_PREFIX)
            {
                uint16 str_len = (uint16)data[0] | ((uint16)data[1] << 8);
                if (str_len <= data_len - FIELD_VARCHAR_LEN_PREFIX && str_len > 0)
                {
                    std::string word((const char *)(data + FIELD_VARCHAR_LEN_PREFIX), str_len);
                    std::transform(word.begin(), word.end(), word.begin(), ::tolower);
                    out.insert(std::move(word));
                }
            }
        }
        tidesdb_iter_next(iter);
    }

    tidesdb_iter_free(iter);
    tidesdb_txn_free(txn);

    sql_print_information("[TIDESDB] Loaded %zu stop words from table '%s'", out.size(),
                          sanitized_spec);
    return true;
}

void tdb_ft_stopword_table_update(void *, st_mysql_sys_var *, void *var_ptr, const void *save)
{
    const char *new_val = *static_cast<const char *const *>(save);

    std::unordered_set<std::string> new_set;
    bool ok;

    if (!new_val || !new_val[0])
    {
        tdb_load_default_stopwords_into(new_set);
        ok = true;
    }
    else
    {
        ok = tdb_load_stopwords_from_table_spec_into(new_val, new_set);
        if (!ok)
        {
            sql_print_warning("[TIDESDB] Failed to load stop words from '%s', keeping current set",
                              new_val);
        }
    }

    /* Hold the write-lock only for the swap + sysvar pointer publish.
       Concurrent FTS readers (held in read-lock) drain quickly. */
    mysql_rwlock_wrlock(&tdb_stopword_lock);
    if (ok) tdb_stopwords = std::move(new_set);
    *static_cast<const char **>(var_ptr) = new_val;
    mysql_rwlock_unlock(&tdb_stopword_lock);

    if (ok && (!new_val || !new_val[0]))
    {
        sql_print_information("[TIDESDB] Stop words reset to defaults (%zu words)",
                              tdb_stopwords.size());
    }
}

/* ******************** Tokenizer ******************** */

/* Helper to lowercase, check stop words, length filter, and emit a token */
static inline void fts_emit_token(const char *word_start, size_t byte_len, uint char_count,
                                  CHARSET_INFO *cs, std::vector<fts_token_t> &out)
{
    if (char_count < srv_fts_min_word_len || char_count > srv_fts_max_word_len) return;

    fts_token_t tok;
    tok.word.assign(word_start, byte_len);
    size_t lowered_len =
        cs->cset->casedn(cs, &tok.word[0], tok.word.size(), &tok.word[0], tok.word.size());
    tok.word.resize(lowered_len);

    if (tdb_is_stopword_locked(tok.word)) return;
    out.push_back(std::move(tok));
}

void fts_tokenize(const char *text, size_t text_len, CHARSET_INFO *cs,
                  std::vector<fts_token_t> &out)
{
    const char *p = text;
    const char *end = text + text_len;
    uint mblen;

    /* Snapshot blend chars under read lock once per tokenize call */
    bool has_blend = false;
    bool blend_map_copy[TDB_BLEND_MAP_SIZE];
    {
        mysql_rwlock_rdlock(&tdb_blend_lock);
        memcpy(blend_map_copy, tdb_blend_char_map, sizeof(blend_map_copy));
        mysql_rwlock_unlock(&tdb_blend_lock);
        for (uint i = 0; i < TDB_BLEND_MAP_SIZE && !has_blend; i++)
            if (blend_map_copy[i]) has_blend = true;
    }

    /* Hold the stopword rdlock once for the whole tokenize pass.
       fts_emit_token calls tdb_is_stopword_locked which assumes the
       read lock is held -- avoids N lock pairs per document. */
    mysql_rwlock_rdlock(&tdb_stopword_lock);

    while (p < end)
    {
        while (p < end)
        {
            mblen = my_ismbchar(cs, p, end);
            if (mblen) break; /* multi-byte = word char */
            if (my_isalnum(cs, (uchar)*p)) break;
            if (has_blend && blend_map_copy[(uchar)*p]) break;
            p++;
        }
        if (p >= end) break;

        const char *word_start = p;
        uint char_count = 0;
        bool contains_blend = false;

        while (p < end)
        {
            mblen = my_ismbchar(cs, p, end);
            if (mblen)
            {
                p += mblen;
                char_count++;
                continue;
            }
            if (my_isalnum(cs, (uchar)*p))
            {
                p++;
                char_count++;
                continue;
            }
            if (has_blend && blend_map_copy[(uchar)*p])
            {
                contains_blend = true;
                p++;
                char_count++;
                continue;
            }
            break;
        }
        size_t byte_len = (size_t)(p - word_start);

        if (!contains_blend)
        {
            fts_emit_token(word_start, byte_len, char_count, cs, out);
        }
        else
        {
            /* Emit full blended token plus sub-parts split by blend chars. */
            fts_emit_token(word_start, byte_len, char_count, cs, out);

            const char *sub_start = word_start;
            uint sub_chars = 0;
            for (const char *s = word_start; s < word_start + byte_len; s++)
            {
                if (blend_map_copy[(uchar)*s])
                {
                    size_t sub_len = (size_t)(s - sub_start);
                    if (sub_len > 0) fts_emit_token(sub_start, sub_len, sub_chars, cs, out);
                    sub_start = s + 1;
                    sub_chars = 0;
                }
                else
                {
                    sub_chars++;
                }
            }
            size_t sub_len = (size_t)((word_start + byte_len) - sub_start);
            if (sub_len > 0) fts_emit_token(sub_start, sub_len, sub_chars, cs, out);
        }
    }

    mysql_rwlock_unlock(&tdb_stopword_lock);
}

void fts_extract_and_tokenize(TABLE *table, const KEY *key_info, const uchar *record,
                              CHARSET_INFO *cs, std::vector<fts_token_t> &out_tokens)
{
    /* MF-8: std::unique_ptr in thread_local storage. unique_ptr's size
       is one pointer (fits the dlopen'd static-TLS budget that broke
       the original `static thread_local std::string` attempt), and
       its destructor runs at thread exit via __cxa_thread_atexit. */
    static thread_local std::unique_ptr<std::string> doc_buf;
    if (!doc_buf) doc_buf = std::make_unique<std::string>();
    std::string &doc = *doc_buf;
    doc.clear();

    my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(record - table->record[0]);

    for (uint p = 0; p < key_info->user_defined_key_parts; p++)
    {
        Field *f = key_info->key_part[p].field;
        f->move_field_offset(ptrdiff);
        if (!f->is_null())
        {
            String val;
            f->val_str(&val);
            if (!doc.empty()) doc += ' ';
            doc.append(val.ptr(), val.length());
        }
        f->move_field_offset(-ptrdiff);
    }

    fts_tokenize(doc.data(), doc.size(), cs, out_tokens);
}

/* ******************** Boolean query parser ******************** */

void fts_parse_boolean(const char *query, size_t len, CHARSET_INFO *cs,
                       std::vector<fts_query_term_t> &out)
{
    const char *p = query;
    const char *end = query + len;

    while (p < end)
    {
        while (p < end && *p == ' ') p++;
        if (p >= end) break;

        int yesno = FTS_TERM_NEUTRAL;
        if (*p == FTS_BOOL_OP_REQUIRED)
        {
            yesno = FTS_TERM_REQUIRED;
            p++;
        }
        else if (*p == FTS_BOOL_OP_EXCLUDED)
        {
            yesno = FTS_TERM_EXCLUDED;
            p++;
        }

        while (p < end && *p == ' ') p++;
        if (p >= end) break;

        if (*p == FTS_BOOL_OP_PHRASE)
        {
            p++; /* skip opening quote */
            const char *phrase_start = p;
            while (p < end && *p != FTS_BOOL_OP_PHRASE) p++;
            size_t phrase_len = (size_t)(p - phrase_start);
            if (p < end) p++; /* skip closing quote */

            if (phrase_len == 0) continue;

            std::vector<fts_token_t> phrase_tokens;
            fts_tokenize(phrase_start, phrase_len, cs, phrase_tokens);
            if (phrase_tokens.empty()) continue;

            /* Each phrase word becomes a required term for candidate
               filtering. The first word carries the phrase metadata
               for verification. */
            fts_query_term_t qt;
            qt.term = phrase_tokens[0].word;
            qt.yesno = yesno ? yesno : FTS_TERM_REQUIRED;
            qt.trunc = false;
            qt.is_phrase = true;
            for (auto &tok : phrase_tokens) qt.phrase_words.push_back(tok.word);

            out.push_back(std::move(qt));
            for (size_t i = 1; i < phrase_tokens.size(); i++)
            {
                fts_query_term_t wt;
                wt.term = phrase_tokens[i].word;
                wt.yesno = FTS_TERM_REQUIRED;
                wt.trunc = false;
                wt.is_phrase = false;
                out.push_back(std::move(wt));
            }
            continue;
        }

        while (p < end && !my_isalnum(cs, (uchar)*p) && !my_ismbchar(cs, p, end) &&
               *p != FTS_BOOL_OP_TRUNC)
            p++;
        if (p >= end) break;

        const char *word_start = p;
        while (p < end)
        {
            uint mblen = my_ismbchar(cs, p, end);
            if (mblen)
            {
                p += mblen;
                continue;
            }
            if (my_isalnum(cs, (uchar)*p) || *p == FTS_BOOL_OP_TRUNC)
            {
                p++;
                continue;
            }
            break;
        }
        size_t wlen = (size_t)(p - word_start);
        if (wlen == 0) continue;

        bool trunc = false;
        if (wlen > 0 && word_start[wlen - 1] == FTS_BOOL_OP_TRUNC)
        {
            trunc = true;
            wlen--;
        }
        if (wlen == 0) continue;

        fts_query_term_t qt;
        qt.term.assign(word_start, wlen);
        size_t lowered =
            cs->cset->casedn(cs, &qt.term[0], qt.term.size(), &qt.term[0], qt.term.size());
        qt.term.resize(lowered);
        qt.yesno = yesno;
        qt.trunc = trunc;
        qt.is_phrase = false;
        out.push_back(std::move(qt));
    }
}

bool fts_phrase_in_tokens(const std::vector<fts_token_t> &doc_tokens,
                          const std::vector<std::string> &phrase_words)
{
    if (phrase_words.empty()) return true;
    if (doc_tokens.size() < phrase_words.size()) return false;

    size_t limit = doc_tokens.size() - phrase_words.size();
    for (size_t i = 0; i <= limit; i++)
    {
        bool match = true;
        for (size_t j = 0; j < phrase_words.size(); j++)
        {
            if (doc_tokens[i + j].word != phrase_words[j])
            {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

/* ******************** Lifecycle ******************** */

void tdb_fts_init()
{
    mysql_rwlock_init(tdb_stopword_lock_key, &tdb_stopword_lock);
    tdb_load_default_stopwords();
    sql_print_information("[TIDESDB] Loaded %zu default stop words", tdb_stopwords.size());

    mysql_rwlock_init(tdb_blend_lock_key, &tdb_blend_lock);
    tdb_rebuild_blend_map(srv_fts_blend_chars);
}

void tdb_fts_destroy()
{
    mysql_rwlock_destroy(&tdb_stopword_lock);
    mysql_rwlock_destroy(&tdb_blend_lock);
}
