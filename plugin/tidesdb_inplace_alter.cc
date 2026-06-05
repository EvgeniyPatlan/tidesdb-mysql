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
 * Online-DDL (inplace ALTER) state machine for the TidesDB MySQL
 * plugin. Extracted from ha_tidesdb.cc as the A-7 architectural
 * extraction.
 *
 * These are ha_tidesdb member-function definitions split into their
 * own translation unit -- the declarations (with `override`) stay in
 * ha_tidesdb.h, so the handler class and every call site are
 * unchanged. The four-phase MySQL inplace-alter contract lives here:
 *
 *   check_if_supported_inplace_alter  classify INSTANT/INPLACE/COPY
 *   prepare_inplace_alter_table       create CFs for added indexes
 *   inplace_alter_table               populate new indexes (table scan)
 *   commit_inplace_alter_table        swap CFs in / drop removed ones
 *   check_if_incompatible_data        option-change compatibility
 *
 * What it borrows from ha_tidesdb.cc (promoted to module-scope
 * linkage in the A-7 pass, declared in ha_tidesdb.h): build_cf_config,
 * comparable_key_length, schema_cf_store_frm,
 * tidesdb_compute_opts_for_table, resolve_idx_cf,
 * tidesdb_opts_for_table, and the TDB_*_OPTIONS macros. A future
 * TidesStore (A-3) is the natural seat to re-encapsulate that CF /
 * option surface and the atomic-DDL participation that belongs with
 * it.
 */

#include "ha_tidesdb.h"

#include <mysql/plugin.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "sql/key.h"
#include "sql/sql_class.h"
#include "sql/table.h"

#include "tidesdb_atomic_ddl.h"      /* TidesdbAtomicDdlBridge, serialize_sdi */
#include "tidesdb_engine_context.h"  /* tdb_get_engine, g_engine_ctx */
#include "tidesdb_fts.h"             /* is_fts_index */
#include "tidesdb_perf_scope.h"      /* v0.4.1 perf-instrumentation: TDB_PERF_SCOPE */
#include "tidesdb_spatial.h"         /* is_spatial_index */

#include "sql/dd/properties.h"       /* dd::Properties (for clear() call) */
#include "sql/dd/string_type.h"      /* dd::String_type */
#include "sql/dd/types/table.h"      /* dd::Table::set_se_private_data, id() */
#include "sql/log.h"                 /* sql_print_error */

/*
  Classify ALTER TABLE operations into INSTANT / INPLACE / COPY.

  INSTANT     metadata-only changes (.frm rewrite, no engine work):
              rename column/index, change default, change table options,
              ADD COLUMN, DROP COLUMN (row format is self-describing via
              the ROW_HEADER_MAGIC header written by serialize_row)
  INPLACE     add/drop secondary indexes (create/drop CFs, populate)
  COPY        column type changes, PK changes
*/
enum_alter_inplace_result ha_tidesdb::check_if_supported_inplace_alter(
    TABLE *altered_table [[maybe_unused]], Alter_inplace_info *ha_alter_info)
{
    DBUG_ENTER("ha_tidesdb::check_if_supported_inplace_alter");
    TDB_PERF_SCOPE(check_if_supported_inplace_alter);

    Alter_inplace_info::HA_ALTER_FLAGS flags = ha_alter_info->handler_flags;

    /* Pure-metadata operations -- INSTANT (no data rewrite, no index work).
       Our row format carries its own field_count + null_bytes header, so
       deserialize_row can adapt to schema changes by back-filling missing
       trailing columns from table->s->default_values.
       DROP COLUMN is included here ONLY for the trailing-drop case --
       that's safe because deserialize_row's unpack loop runs MIN(stored,
       current) iterations and silently ignores extra trailing bytes from
       the old format. Mid-column drops would shift remaining field
       positions and need per-row schema versioning we don't have, so we
       downgrade those to ALGORITHM=COPY below. */
    static const Alter_inplace_info::HA_ALTER_FLAGS TIDESDB_INSTANT =
        Alter_inplace_info::ALTER_COLUMN_NAME |
        Alter_inplace_info::ALTER_COLUMN_DEFAULT |
        Alter_inplace_info::CHANGE_CREATE_OPTION |
        Alter_inplace_info::DROP_CHECK_CONSTRAINT |
        Alter_inplace_info::ALTER_VIRTUAL_GCOL_EXPR |
        Alter_inplace_info::ALTER_RENAME |
        Alter_inplace_info::RENAME_INDEX |
        Alter_inplace_info::CHANGE_INDEX_OPTION |
        Alter_inplace_info::ADD_COLUMN |
        Alter_inplace_info::DROP_COLUMN |
        Alter_inplace_info::ALTER_STORED_COLUMN_ORDER |
        Alter_inplace_info::ALTER_VIRTUAL_COLUMN_ORDER;

    /* Operations we can do inplace (add/drop secondary indexes). */
    static const Alter_inplace_info::HA_ALTER_FLAGS TIDESDB_INPLACE_INDEX =
        Alter_inplace_info::ADD_INDEX |
        Alter_inplace_info::DROP_INDEX |
        Alter_inplace_info::ADD_UNIQUE_INDEX |
        Alter_inplace_info::DROP_UNIQUE_INDEX |
        Alter_inplace_info::ADD_SPATIAL_INDEX;

    /* Pure-INSTANT (data-dictionary update only) */
    if (!(flags & ~TIDESDB_INSTANT))
    {
        /* Special-case DROP COLUMN: only safe as INSTANT when ALL dropped
           columns are at the END of the table -- otherwise field positions
           shift and rows written under the old schema unpack into the wrong
           field slots. Detect this by comparing the first N field names of
           the altered table against the original; any mismatch means a
           non-trailing column was removed and we must rebuild via COPY. */
        if (flags & Alter_inplace_info::DROP_COLUMN)
        {
            for (uint i = 0; i < altered_table->s->fields; i++)
            {
                if (i >= table->s->fields ||
                    strcmp(altered_table->field[i]->field_name,
                           table->field[i]->field_name) != 0)
                {
                    ha_alter_info->unsupported_reason =
                        "TidesDB INSTANT DROP COLUMN supports trailing columns only; "
                        "use ALGORITHM=COPY for non-trailing drops";
                    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
                }
            }
        }
        DBUG_RETURN(HA_ALTER_INPLACE_INSTANT);
    }

    /* INSTANT + secondary-index changes -- INPLACE with shared lock.
       Concurrent reads OK; concurrent writes blocked while the index
       population scan runs. PK changes still require ALGORITHM=COPY. */
    if (!(flags & ~(TIDESDB_INSTANT | TIDESDB_INPLACE_INDEX)))
    {
        if (flags & (Alter_inplace_info::ADD_PK_INDEX |
                     Alter_inplace_info::DROP_PK_INDEX))
        {
            ha_alter_info->unsupported_reason =
                "TidesDB cannot change PRIMARY KEY inplace";
            DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
        }
        DBUG_RETURN(HA_ALTER_INPLACE_SHARED_LOCK);
    }

    /* Everything else falls back to ALGORITHM=COPY. */
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
}

/*
  Create CFs for newly added indexes.
  Called with shared MDL lock (concurrent DML is allowed).
*/
bool ha_tidesdb::prepare_inplace_alter_table(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info,
    const dd::Table *old_table_def,
    dd::Table *new_table_def)
{
    DBUG_ENTER("ha_tidesdb::prepare_inplace_alter_table");
    TDB_PERF_SCOPE(prepare_inplace_alter_table);

    ha_tidesdb_inplace_ctx *ctx;
    try
    {
        ctx = new ha_tidesdb_inplace_ctx();
    }
    catch (...)
    {
        DBUG_RETURN(true);
    }
    ha_alter_info->handler_ctx = ctx;

    tidesdb_column_family_config_t cfg = build_cf_config(TDB_TABLE_OPTIONS(table));

    std::string base_cf = share->cf_name;

    /* We create CFs for added indexes */
    if (ha_alter_info->index_add_count > 0)
    {
        for (uint a = 0; a < ha_alter_info->index_add_count; a++)
        {
            uint key_num = ha_alter_info->index_add_buffer[a];
            KEY *new_key = &ha_alter_info->key_info_buffer[key_num];

            /* We skip PK -- shouldn't happen (blocked in check_if_supported) */
            if (new_key->flags & HA_NOSAME &&
                altered_table->s->primary_key < altered_table->s->keys &&
                key_num == altered_table->s->primary_key)
                continue;

            std::string idx_cf = base_cf + CF_INDEX_INFIX + new_key->name;

            /* We drop stale CF if it exists from a previous failed ALTER */
            tidesdb_drop_column_family(tdb_get_engine(), idx_cf.c_str());

            tidesdb_column_family_config_t idx_cfg = cfg;
            ha_index_option_struct *iopts = TDB_INDEX_OPTIONS(new_key);
            if (iopts) idx_cfg.use_btree = iopts->use_btree ? 1 : 0;

            int rc = tidesdb_create_column_family(tdb_get_engine(), idx_cf.c_str(), &idx_cfg);
            if (rc != TDB_SUCCESS)
            {
                sql_print_error("[TIDESDB] inplace ADD INDEX: failed to create CF '%s' (err=%d)",
                                idx_cf.c_str(), rc);
                my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] failed to create index CF");
                DBUG_RETURN(true);
            }

            tidesdb_column_family_t *icf = tidesdb_get_column_family(tdb_get_engine(), idx_cf.c_str());
            if (!icf)
            {
                sql_print_error("[TIDESDB] inplace ADD INDEX: CF '%s' not found after create",
                                idx_cf.c_str());
                my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] index CF not found after create");
                DBUG_RETURN(true);
            }

            ctx->add_cfs.push_back(icf);
            ctx->add_cf_names.push_back(idx_cf);
            ctx->add_key_nums.push_back(key_num);
        }
    }

    /* We record CF names to drop for removed indexes */
    if (ha_alter_info->index_drop_count > 0)
    {
        for (uint d = 0; d < ha_alter_info->index_drop_count; d++)
        {
            KEY *old_key = ha_alter_info->index_drop_buffer[d];
            /* We find the key number in the old table */
            uint old_key_num = (uint)(old_key - table->key_info);
            if (old_key_num < share->idx_cf_names.size() &&
                !share->idx_cf_names[old_key_num].empty())
            {
                ctx->drop_cf_names.push_back(share->idx_cf_names[old_key_num]);
            }
        }
    }

    /* Atomic-DDL participation (Task 9): pre-stage the post-ALTER
       se_private_data into the ctx so commit_inplace_alter_table can
       stamp it onto new_table_def atomically with the CF swap. We
       compute here -- in prepare -- so a failure surfaces before any CFs
       have been created/dropped, keeping the rollback path simple.

       new_table_def is NULL on temp-table / fast-path ALTERs that bypass
       the DD; nothing to stage in that case. */
    if (new_table_def)
    {
        if (!tidesdb_mysql::TidesdbAtomicDdlBridge::recompute_se_private_data(
                old_table_def, *new_table_def, ctx->new_se_private_serialized))
        {
            sql_print_error(
                "[TIDESDB] inplace ALTER prepare: failed to compute "
                "new se_private_data");
            my_error(ER_INTERNAL_ERROR, MYF(0),
                     "[TIDESDB] failed to compute post-ALTER se_private_data");
            DBUG_RETURN(true);
        }
    }

    DBUG_RETURN(false);
}

/*
  Inplace phase -- we populate newly added indexes by scanning the table.
  Called with no MDL lock blocking (HA_ALTER_INPLACE_NO_LOCK).
*/
bool ha_tidesdb::inplace_alter_table(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info,
    const dd::Table *old_table_def [[maybe_unused]],
    dd::Table *new_table_def [[maybe_unused]])
{
    DBUG_ENTER("ha_tidesdb::inplace_alter_table");
    TDB_PERF_SCOPE(inplace_alter_table);

    ha_tidesdb_inplace_ctx *ctx = static_cast<ha_tidesdb_inplace_ctx *>(ha_alter_info->handler_ctx);

    if (!ctx || ctx->add_cfs.empty())
        DBUG_RETURN(false); /* Nothing to populate (drop-only or instant) */

    /* Atomic-DDL Task 12: crash-injection point.
       prepare_inplace_alter_table created the new index CFs and staged
       the post-ALTER se_private_data into ctx, but commit_inplace_alter_table
       has not run yet -- the dd::Table swap and CF drop-of-old-indexes are
       still ahead. On crash here the pre-ALTER schema must remain intact
       after restart, because the DD-side ALTER txn has not committed and
       the engine-side CFs we created sit as orphans that the reconciler
       will sweep. */
    DBUG_EXECUTE_IF("tidesdb_crash_mid_inplace_alter", DBUG_SUICIDE(););

    /* We mark all columns readable on the altered table since we read
       fields via make_sort_key_part during index key construction. */
    my_bitmap_map *old_map = tmp_use_all_columns(altered_table, altered_table->read_set);

    /* We do a full table scan to populate the new secondary indexes.
       We use the altered_table's key_info for building index keys,
       since that matches the new key numbering. */

    /* We always use READ_COMMITTED for index population.  The scan reads
       potentially millions of rows; higher isolation levels would track
       each key in the read-set, causing unbounded memory growth.  Index
       builds are DDL and never need OCC conflict detection. */
    tidesdb_txn_t *txn = NULL;
    int rc = tidesdb_txn_begin_with_isolation(tdb_get_engine(), TDB_ISOLATION_READ_COMMITTED, &txn);
    if (rc != TDB_SUCCESS || !txn)
    {
        sql_print_error("[TIDESDB] inplace ADD INDEX: txn_begin failed (err=%d)", rc);
        my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] failed to begin txn for index build");
        tmp_restore_column_map(altered_table->read_set, old_map);
        DBUG_RETURN(true);
    }

    tidesdb_iter_t *iter = NULL;
    rc = tidesdb_iter_new(txn, share->cf, &iter);
    if (rc != TDB_SUCCESS || !iter)
    {
        tidesdb_txn_free(txn);
        sql_print_error("[TIDESDB] inplace ADD INDEX: iter_new failed (err=%d)", rc);
        my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] failed to create iterator for index build");
        tmp_restore_column_map(altered_table->read_set, old_map);
        DBUG_RETURN(true);
    }
    tidesdb_iter_seek_to_first(iter);

    ha_rows rows_processed = 0;

    /* For UNIQUE indexes, we track seen index-column prefixes to detect
       duplicates.  If a duplicate is found we must abort the ALTER.
       unordered_set gives O(1) amortized lookup vs O(log n) for std::set,
       which matters for tables with millions of rows. */
    std::vector<bool> idx_is_unique(ctx->add_cfs.size(), false);
    std::vector<std::unordered_set<std::string>> idx_seen(ctx->add_cfs.size());
    for (uint a = 0; a < ctx->add_cfs.size(); a++)
    {
        uint key_num = ctx->add_key_nums[a];
        KEY *ki = &altered_table->key_info[key_num];
        if (ki->flags & HA_NOSAME) idx_is_unique[a] = true;
    }

    /* We remember the last data key so we can seek directly to it after
       a batch commit (O(n²)). */
    uchar last_data_key[DATA_KEY_BUF_LEN];
    size_t last_data_key_len = 0;

    /* Reusable FTS tokenization scratch for back-populating FULLTEXT
       indexes added to a table that already has rows. Declared once
       and cleared per row to avoid per-row reallocation. Plain locals
       (not static thread_local) -- this path runs once per ALTER, not
       on the dlopen'd plugin's hot per-row entrypoint, so the
       static-TLS budget concern that shapes write_row's scratch does
       not apply here. */
    std::vector<fts_token_t> fts_tokens;
    std::unordered_map<std::string, uint16> fts_tf;

    /* F-1: per-added-index FTS meta accumulators. The posting entries go
       into the new index CF in batched txns, but the meta counters
       (total_docs / total_words, stored in share->cf, the data CF, which
       is NOT dropped on ALTER rollback) must be written exactly once and
       only if the whole build succeeds. Writing them per-row meant an
       aborted/KILLed build (rollback drops just the new index CF) left
       inflated counters behind, so a retry double-counted and skewed
       BM25. We accumulate here and flush a single fts_update_meta per
       FTS index after the final successful commit; on any abort path
       nothing is written. Indexed by the ctx->add_cfs position `a`. */
    std::vector<int64_t> fts_doc_acc(ctx->add_cfs.size(), 0);
    std::vector<int64_t> fts_word_acc(ctx->add_cfs.size(), 0);

    while (tidesdb_iter_valid(iter))
    {
        uint8_t *key_data = NULL;
        size_t key_size = 0;
        uint8_t *val_data = NULL;
        size_t val_size = 0;

        if (tidesdb_iter_key(iter, &key_data, &key_size) != TDB_SUCCESS ||
            tidesdb_iter_value(iter, &val_data, &val_size) != TDB_SUCCESS)
        {
            tidesdb_iter_next(iter);
            continue;
        }

        /* We skip non-data keys (meta namespace) */
        if (key_size < KEY_NAMESPACE_LEN || key_data[0] != KEY_NS_DATA)
        {
            tidesdb_iter_next(iter);
            continue;
        }

        /* We save this data key for potential batch re-seek */
        if (key_size <= sizeof(last_data_key))
        {
            memcpy(last_data_key, key_data, key_size);
            last_data_key_len = key_size;
        }

        /* We extract PK from the data key (skip KEY_NS_DATA prefix) */
        const uchar *pk = key_data + KEY_NAMESPACE_LEN;
        uint pk_len = (uint)(key_size - KEY_NAMESPACE_LEN);

        /* We decode the row into table->record[0].  The field pointers from
           altered_table->key_info will be temporarily repointed (via
           move_field_offset) to read from this buffer. */
        if (share->has_blobs || share->encrypted)
        {
            std::string row_data((const char *)val_data, val_size);
            deserialize_row(table->record[0], row_data);
        }
        else
        {
            deserialize_row(table->record[0], (const uchar *)val_data, val_size);
        }

        /* For each newly added index, build the index entry key.
           altered_table->key_info fields have ptr into altered_table->record[0],
           but the data lives in table->record[0].  We compute ptdiff to
           rebase field pointers to read from the correct buffer.
           Key format matches make_comparable_key()= [null_byte] + sort_string. */
        my_ptrdiff_t ptdiff = (my_ptrdiff_t)(table->record[0] - altered_table->record[0]);

        for (uint a = 0; a < ctx->add_cfs.size(); a++)
        {
            uint key_num = ctx->add_key_nums[a];
            KEY *ki = &altered_table->key_info[key_num];

            /* FULLTEXT: back-populate the inverted index for this
               existing row, mirroring write_row's FTS path. The data
               was deserialized into table->record[0]; ki's key-part
               fields are based at altered_table->record[0], so passing
               altered_table + table->record[0] makes
               fts_extract_and_tokenize rebase them onto the decoded
               row (ptrdiff = table->record[0] - altered_table->record[0]). */
            if (is_fts_index(ki))
            {
                CHARSET_INFO *fts_cs = ki->key_part[0].field->charset();
                fts_tokens.clear();
                fts_tf.clear();
                fts_extract_and_tokenize(altered_table, ki, table->record[0], fts_cs, fts_tokens);

                for (auto &tok : fts_tokens) fts_tf[tok.word]++;
                uint32 word_count = (uint32)fts_tokens.size();

                for (auto &[term, tf] : fts_tf)
                {
                    uchar fk[FTS_KEY_BUF_LEN];
                    uint fk_len = fts_build_key(term.data(), (uint)term.size(), pk, pk_len, fk);
                    uchar fv[FTS_VALUE_LEN];
                    fts_build_value(tf, word_count, fv);
                    int frc = tidesdb_txn_put(txn, ctx->add_cfs[a], fk, fk_len, fv, FTS_VALUE_LEN,
                                              TIDESDB_TTL_NONE);
                    if (frc != TDB_SUCCESS)
                        sql_print_error(
                            "[TIDESDB] inplace ADD FULLTEXT: put failed for key %u (err=%d)",
                            key_num, frc);
                }

                /* F-1: accumulate; the single fts_update_meta is issued
                   after the final successful commit (see end of scan).
                   keynr is the new index's key number in altered_table,
                   which is what ft_init_ext uses for fts_load_meta after
                   commit. */
                fts_doc_acc[a] += FTS_DOC_DELTA_ADD;
                fts_word_acc[a] += (int64_t)word_count;
                continue;
            }
            /* SPATIAL still uses a separate path (not back-populated). */
            if (is_spatial_index(ki)) continue;

            uchar ik[SEC_IDX_KEY_BUF_LEN];
            uint pos = 0;
            for (uint p = 0; p < ki->user_defined_key_parts; p++)
            {
                KEY_PART_INFO *kp = &ki->key_part[p];
                Field *field = kp->field;

                field->move_field_offset(ptdiff);
                if (field->is_nullable())
                {
                    if (field->is_null())
                    {
                        ik[pos++] = SORT_KEY_NULL;
                        bzero(ik + pos, kp->length);
                        pos += kp->length;
                        field->move_field_offset(-ptdiff);
                        continue;
                    }
                    ik[pos++] = SORT_KEY_NOT_NULL;
                }
                memset(ik + pos, 0, kp->length); field->make_sort_key(ik + pos, kp->length);
                field->move_field_offset(-ptdiff);
                pos += kp->length;
            }

            /* We check UNIQUE constraint before inserting */
            if (idx_is_unique[a])
            {
                std::string prefix((const char *)ik, pos);
                if (!idx_seen[a].insert(prefix).second)
                {
                    /* We found a duplicate -- abort the ALTER */
                    tidesdb_iter_free(iter);
                    tidesdb_txn_rollback(txn);
                    tidesdb_txn_free(txn);
                    tmp_restore_column_map(altered_table->read_set, old_map);
                    my_error(ER_DUP_ENTRY, MYF(0), "?", altered_table->key_info[key_num].name);
                    DBUG_RETURN(true);
                }
            }

            /* We append PK to make the key unique */
            memcpy(ik + pos, pk, pk_len);
            pos += pk_len;

            rc = tidesdb_txn_put(txn, ctx->add_cfs[a], ik, pos, &tdb_empty_val,
                                 sizeof(tdb_empty_val), TIDESDB_TTL_NONE);
            if (rc != TDB_SUCCESS)
            {
                sql_print_error("[TIDESDB] inplace ADD INDEX: put failed for key %u (err=%d)",
                                key_num, rc);
                /* Continue, best effort from here! */
            }
        }

        rows_processed++;

        /* We check for KILL signal periodically so the user can cancel
           long-running index builds via KILL <thread_id>. */
        if ((rows_processed % TIDESDB_INDEX_BUILD_BATCH) == 0 && thd_killed(ha_thd()))
        {
            tidesdb_iter_free(iter);
            tidesdb_txn_rollback(txn);
            tidesdb_txn_free(txn);
            tmp_restore_column_map(altered_table->read_set, old_map);
            my_error(ER_QUERY_INTERRUPTED, MYF(0));
            DBUG_RETURN(true);
        }

        if (rows_processed % TIDESDB_INDEX_BUILD_BATCH == 0)
        {
            {
                int crc = tidesdb_txn_commit(txn);
                if (crc != TDB_SUCCESS)
                {
                    sql_print_warning("[TIDESDB] inplace ADD INDEX: batch commit failed rc=%d",
                                      crc);
                    tidesdb_txn_rollback(txn);
                }
            }
            tidesdb_iter_free(iter);

            /* We reset the txn with READ_COMMITTED -- index builds
               don't need snapshot consistency across batches. */
            int rrc = tidesdb_txn_reset(txn, TDB_ISOLATION_READ_COMMITTED);
            if (rrc != TDB_SUCCESS)
            {
                /* We reset failed -- fall back to free+begin */
                sql_print_warning(
                    "[TIDESDB] inplace ADD INDEX: tidesdb_txn_reset failed (rc=%d), "
                    "falling back to free+begin",
                    rrc);
                tidesdb_txn_free(txn);
                txn = NULL;
                rc = tidesdb_txn_begin_with_isolation(tdb_get_engine(), TDB_ISOLATION_READ_COMMITTED,
                                                      &txn);
                if (rc != TDB_SUCCESS || !txn)
                {
                    sql_print_error("[TIDESDB] inplace ADD INDEX: batch txn_begin failed");
                    my_error(ER_INTERNAL_ERROR, MYF(0),
                             "[TIDESDB] batch txn failed during index build");
                    tmp_restore_column_map(altered_table->read_set, old_map);
                    DBUG_RETURN(true);
                }
            }
            iter = NULL;
            rc = tidesdb_iter_new(txn, share->cf, &iter);
            if (rc != TDB_SUCCESS || !iter)
            {
                tidesdb_txn_free(txn);
                my_error(ER_INTERNAL_ERROR, MYF(0),
                         "[TIDESDB] batch iter failed during index build");
                tmp_restore_column_map(altered_table->read_set, old_map);
                DBUG_RETURN(true);
            }
            /* We seek directly to the last processed key and advance past it */
            int src = tidesdb_iter_seek(iter, last_data_key, last_data_key_len);
            if (src != TDB_SUCCESS)
            {
                sql_print_warning("[TIDESDB] inplace ADD INDEX: iter_seek failed rc=%d", src);
                break; /* end scan gracefully */
            }
            if (tidesdb_iter_valid(iter)) tidesdb_iter_next(iter);
            continue; /* Don't call iter_next again */
        }

        tidesdb_iter_next(iter);
    }

    tidesdb_iter_free(iter);

    /* We commit remaining entries */
    rc = tidesdb_txn_commit(txn);
    if (rc != TDB_SUCCESS) tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);

    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] inplace ADD INDEX: final commit failed (err=%d)", rc);
        my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] final commit failed during index build");
        tmp_restore_column_map(altered_table->read_set, old_map);
        DBUG_RETURN(true);
    }

    /* F-1: the index postings are now durably committed. Flush the FTS
       meta counters exactly once, in their own txn. Reaching here means
       the whole back-populate succeeded; every abort path above returned
       earlier without touching meta, so an aborted/KILLed build leaves
       share->cf's FTS counters untouched and a retry starts clean.
       A meta-write failure here only degrades BM25 ranking (fts_load_meta
       falls back to neutral avgdl); the index itself is correct, so we
       log and proceed rather than fail an ALTER whose data is committed. */
    {
        bool any_fts = false;
        for (size_t a = 0; a < fts_doc_acc.size(); a++)
            if (fts_doc_acc[a] != 0 || fts_word_acc[a] != 0) { any_fts = true; break; }

        if (any_fts)
        {
            tidesdb_txn_t *mtxn = NULL;
            int mrc = tidesdb_txn_begin_with_isolation(tdb_get_engine(),
                                                       TDB_ISOLATION_READ_COMMITTED, &mtxn);
            if (mrc != TDB_SUCCESS || !mtxn)
            {
                sql_print_warning(
                    "[TIDESDB] inplace ADD FULLTEXT: meta txn_begin failed (err=%d); "
                    "BM25 ranking will use fallback stats until the index is rebuilt",
                    mrc);
            }
            else
            {
                for (size_t a = 0; a < fts_doc_acc.size(); a++)
                {
                    if (fts_doc_acc[a] == 0 && fts_word_acc[a] == 0) continue;
                    fts_update_meta(share, mtxn, share->cf, ctx->add_key_nums[a],
                                    fts_doc_acc[a], fts_word_acc[a]);
                }
                int mcrc = tidesdb_txn_commit(mtxn);
                if (mcrc != TDB_SUCCESS)
                {
                    tidesdb_txn_rollback(mtxn);
                    sql_print_warning(
                        "[TIDESDB] inplace ADD FULLTEXT: meta commit failed (err=%d); "
                        "BM25 ranking will use fallback stats until the index is rebuilt",
                        mcrc);
                }
                tidesdb_txn_free(mtxn);
            }
        }
    }

    tmp_restore_column_map(altered_table->read_set, old_map);
    DBUG_RETURN(false);
}

/*
  Commit or rollback the inplace ALTER.
  On commit       drop old index CFs, update share->idx_cfs for new table shape.
  On rollback     drop newly created CFs.
*/
bool ha_tidesdb::commit_inplace_alter_table(
    TABLE *altered_table,
    Alter_inplace_info *ha_alter_info,
    bool commit,
    const dd::Table *old_table_def [[maybe_unused]],
    dd::Table *new_table_def)
{
    DBUG_ENTER("ha_tidesdb::commit_inplace_alter_table");
    TDB_PERF_SCOPE(commit_inplace_alter_table);

    ha_tidesdb_inplace_ctx *ctx = static_cast<ha_tidesdb_inplace_ctx *>(ha_alter_info->handler_ctx);

    ha_alter_info->group_commit_ctx = NULL;

    if (!ctx) DBUG_RETURN(false);

    /* We free any cached iterators before dropping CFs.  The connection's
       scan_iter and dup_iter_cache_ may hold merge-heap references to
       SSTables in CFs about to be dropped. */
    if (scan_iter)
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    free_dup_iter_cache();

    if (!commit)
    {
        /* Rollback, we drop any CFs we created for new indexes */
        for (const auto &cf_name : ctx->add_cf_names)
            tidesdb_drop_column_family(tdb_get_engine(), cf_name.c_str());
        DBUG_RETURN(false);
    }

    /* Commit, we drop CFs for removed indexes */
    for (const auto &cf_name : ctx->drop_cf_names)
    {
        int rc = tidesdb_drop_column_family(tdb_get_engine(), cf_name.c_str());
        if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
            sql_print_warning("[TIDESDB] commit ALTER: failed to drop CF '%s' (err=%d)",
                              cf_name.c_str(), rc);
    }

    /* We rebuild share->idx_cfs and idx_cf_names based on the new table's keys.
       Since we hold exclusive MDL, no other handler is using the share. */
    lock_shared_ha_data();
    share->idx_cfs.clear();
    share->idx_cf_names.clear();

    uint new_pk = altered_table->s->primary_key;
    for (uint i = 0; i < altered_table->s->keys; i++)
    {
        if (new_pk != MAX_KEY && i == new_pk)
        {
            share->idx_cfs.push_back(NULL);
            share->idx_cf_names.push_back("");
            continue;
        }
        std::string idx_name;
        tidesdb_column_family_t *icf = resolve_idx_cf(
            tdb_get_engine(), share->cf_name, altered_table->key_info[i].name, idx_name);
        share->idx_cfs.push_back(icf);
        share->idx_cf_names.push_back(idx_name);
    }

    /* We recompute cached index metadata for the new table shape */
    for (uint i = 0; i < altered_table->s->keys; i++)
    {
        share->idx_comp_key_len[i] = comparable_key_length(&altered_table->key_info[i]);
        share->idx_is_fts[i] = is_fts_index(&altered_table->key_info[i]);
        share->idx_is_spatial[i] = is_spatial_index(&altered_table->key_info[i]);
    }

    /* We repopulate coverage bitmaps for the new table shape. */
    share->idx_cover.assign(altered_table->s->keys,
                            std::vector<bool>(altered_table->s->fields, false));
    for (uint i = 0; i < altered_table->s->keys; i++)
    {
        const KEY *ki = &altered_table->key_info[i];
        for (uint p = 0; p < ki->user_defined_key_parts; p++)
        {
            uint fnr = ki->key_part[p].fieldnr;
            if (fnr > 0 && fnr - 1 < altered_table->s->fields) share->idx_cover[i][fnr - 1] = true;
        }
        if (altered_table->s->primary_key != MAX_KEY && i != altered_table->s->primary_key)
        {
            const KEY *pk_key = &altered_table->key_info[altered_table->s->primary_key];
            for (uint p = 0; p < pk_key->user_defined_key_parts; p++)
            {
                uint fnr = pk_key->key_part[p].fieldnr;
                if (fnr > 0 && fnr - 1 < altered_table->s->fields)
                    share->idx_cover[i][fnr - 1] = true;
            }
        }
    }
    share->num_secondary_indexes = 0;
    for (uint i = 0; i < share->idx_cfs.size(); i++)
        if (share->idx_cfs[i]) share->num_secondary_indexes++;

    /* If table options changed (SYNC_MODE, COMPRESSION, BLOOM_FPR, etc.),
       we apply them to the live CF(s) so they take effect immediately instead
       of only being persisted in the .frm.

       ENGINE_ATTRIBUTE-freeze fix: compute the new options FRESH from
       altered_table's engine_attribute, bypassing the share's cached
       opts pointer entirely (the cache currently holds the OLD parse).
       Using a local struct lets us:
         (1) feed accurate values into build_cf_config and the share
             field updates below, and
         (2) atomic-swap into share->cached_opts at the end so that
             future TDB_TABLE_OPTIONS(table) calls -- after this alter
             returns -- see the new values rather than the pre-alter
             cache.
       Without this, ALTER TABLE ... ENGINE_ATTRIBUTE='{...}' appeared
       to succeed (DD updated, SHOW CREATE TABLE returns the new value)
       but the engine kept using the old values for the share's life. */
    if (ha_alter_info->handler_flags & ALTER_CHANGE_CREATE_OPTION)
    {
        ha_table_option_struct fresh_opts{};
        tidesdb_compute_opts_for_table(altered_table, &fresh_opts);
        tidesdb_column_family_config_t cfg = build_cf_config(&fresh_opts);

        /* Main data CF */
        if (share->cf)
        {
            int rc = tidesdb_cf_update_runtime_config(share->cf, &cfg, 1);
            if (rc != TDB_SUCCESS)
                sql_print_information(
                    "[TIDESDB] ALTER: failed to update runtime config for "
                    "data CF '%s' (err=%d)",
                    share->cf_name.c_str(), rc);
        }

        /* We apply per-index USE_BTREE overrides */
        for (uint i = 0; i < share->idx_cfs.size(); i++)
        {
            if (share->idx_cfs[i])
            {
                tidesdb_column_family_config_t idx_cfg = cfg;
                if (i < altered_table->s->keys && TDB_INDEX_OPTIONS(&altered_table->key_info[i]))
                {
                    ha_index_option_struct *iopts = TDB_INDEX_OPTIONS(&altered_table->key_info[i]);
                    idx_cfg.use_btree = iopts->use_btree ? 1 : 0;
                }

                int rc = tidesdb_cf_update_runtime_config(share->idx_cfs[i], &idx_cfg, 1);
                if (rc != TDB_SUCCESS)
                    sql_print_information(
                        "[TIDESDB] ALTER: failed to update runtime config for "
                        "index CF '%s' (err=%d)",
                        share->idx_cf_names[i].c_str(), rc);
            }
        }

        /* We update share-level cached options that are read from table
           options. Source: the freshly-computed `fresh_opts` (above)
           rather than TDB_TABLE_OPTIONS(altered_table) -- the latter
           might still hit the share's old cached pointer depending on
           whether altered_table->s->ha_share is set. */
        {
            uint iso_idx = fresh_opts.isolation_level;
            if (iso_idx < array_elements(tdb_isolation_map))
                share->isolation_level = (tidesdb_isolation_level_t)tdb_isolation_map[iso_idx];
            share->default_ttl = fresh_opts.ttl;
            share->has_ttl = (share->default_ttl > 0 || share->ttl_field_idx >= 0);
            /* Encryption changes mid-table aren't safe to flip via inplace
               alter -- existing rows are already encrypted (or not) with
               the previous setting. We update the share for new-row
               behavior consistency with the persisted DD, but the on-disk
               state may now mix old and new rows. Document; a stricter
               implementation would force COPY for encrypted-flag changes. */
            share->encrypted = fresh_opts.encrypted;
            if (share->encrypted)
                share->encryption_key_id = (uint)fresh_opts.encryption_key_id;
        }

        /* ENGINE_ATTRIBUTE-freeze fix: atomically replace share->cached_opts
           with the freshly-computed values so subsequent TDB_TABLE_OPTIONS
           readers (any connection that still holds this share open) see
           the post-alter view rather than the pre-alter cache. We hold
           lock_shared_ha_data, so no concurrent reader can be racing
           against the swap -- but using exchange() preserves the
           memory-ordering contract for the lock-free fast path. */
        ha_table_option_struct *new_cached = new ha_table_option_struct(fresh_opts);
        ha_table_option_struct *old_cached =
            share->cached_opts.exchange(new_cached, std::memory_order_acq_rel);
        delete old_cached;
    }

    /* We force a stats refresh on next info() call */
    share->stats_refresh_us.store(0, std::memory_order_relaxed);
    unlock_shared_ha_data();

    /* Refresh the schema-CF entry after ALTER. (Originally cached .frm
       bytes for MariaDB discover_table; under MySQL it just records the
       table name so DROP TABLE can find the CF.) */
    schema_cf_store_frm(table->s->path.str);

    /* Atomic-DDL participation (Task 9): stamp the pre-staged
       se_private_data onto new_table_def and emit updated SDI. We do
       this on the commit==true path only -- rollback above already
       returned without touching new_table_def, so a failed ALTER leaves
       the DD's prior se_private_data intact.

       Both operations are best-effort-failures-warn rather than
       failures-abort: the data CF has already been swapped; failing the
       ALTER at this stage would leave the engine and DD diverged. The
       set_se_private_data path is the primary atomic-DDL contract --
       on a failure here we log ERROR and return true so the SQL layer
       rolls the DD txn back; that keeps the persisted DD aligned with
       the engine's pre-ALTER state if HTON_SUPPORTS_ATOMIC_DDL flips
       in Task 13.

       The SDI write is downgraded to a WARNING because (a) the prior
       SDI is still queryable and reflects the now-old schema (which
       SHOW CREATE TABLE will overwrite when the server's own
       sdi_set runs), and (b) Task 13 will wire the SDI emit through
       the handlerton's sdi_set hook anyway, so this is a defensive
       belt-and-suspenders write rather than the source of truth. */
    if (new_table_def && !ctx->new_se_private_serialized.empty())
    {
        /* set_se_private_data delegates to insert_values, which asserts
           the target Properties is empty (debug build). Under Task 13's
           HTON_SUPPORTS_ATOMIC_DDL=ON, new_table_def carries over the
           Properties prepare_create wrote (CREATE) or the prior commit
           wrote (previous ALTER), so we must clear before re-populating.
           Today (strict=OFF default, HTON flag still OFF) the DD txn
           hasn't committed yet at commit_inplace_alter_table time, so
           any earlier writes ARE on the same in-memory object; clearing
           is still the right call. */
        new_table_def->se_private_data().clear();
        if (new_table_def->set_se_private_data(
                dd::String_type(ctx->new_se_private_serialized.c_str())))
        {
            sql_print_error(
                "[TIDESDB] inplace ALTER commit: set_se_private_data failed");
            my_error(ER_INTERNAL_ERROR, MYF(0),
                     "[TIDESDB] inplace ALTER commit: set_se_private_data failed");
            DBUG_RETURN(true);
        }

        if (g_engine_ctx.sdi)
        {
            sdi_key_t k{};
            k.type = SDI_TYPE_TABLE;
            k.id   = static_cast<uint64>(new_table_def->id());
            std::string sdi_json = tidesdb_mysql::serialize_sdi(new_table_def);
            if (!g_engine_ctx.sdi->put(
                    k, sdi_json.data(), sdi_json.size()))
            {
                sql_print_warning(
                    "[TIDESDB] inplace ALTER commit: SDI put failed for "
                    "dd::Table id=%llu (engine schema unchanged; the "
                    "server's own sdi_set on commit will re-emit)",
                    static_cast<unsigned long long>(k.id));
                /* Not fatal: server sdi_set still fires; this is belt+suspenders. */
            }
        }
    }

    DBUG_RETURN(false);
}

/*
  Tell MySQL whether changing table options requires a full table rebuild.
  For TidesDB, option changes (SYNC_MODE, TTL, etc.) are compatible at the
  data level -- no row rewrite is needed; the new options take effect on
  next open().
*/
bool ha_tidesdb::check_if_incompatible_data(HA_CREATE_INFO *create_info, uint table_changes)
{
    /* If only table options changed (not column types), data is compatible */
    if (table_changes == IS_EQUAL_YES) return COMPATIBLE_DATA_YES;
    return COMPATIBLE_DATA_NO;
}
