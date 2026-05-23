# Deep Review — Architectural Extractions (A-1…A-8) + FTS Back-Populate Fix

Date: 2026-05-18
Scope: commits `9ce5ce2`…`30a3eab` (master-key/row-lock/engine-context/FTS/
spatial/portability/inplace-alter extractions + `ADD FULLTEXT` back-populate fix).
Method: three parallel specialist agents (cpp / security / general), every
finding re-verified against the source by hand.

Verdict: **the extractions are behavior-preserving and the FTS back-populate
fix is functionally correct.** One real defect was *introduced* by the
back-populate fix (F-1, HIGH). The rest are pre-existing issues the review
surfaced; the recent code merely matched established (sometimes flawed)
conventions.

Legend: **[NEW]** introduced by recent work · **[PRE]** pre-existing,
recent code is consistent with it · severity CRITICAL/HIGH/MEDIUM/LOW.

---

## F-1 — [NEW] HIGH — FTS meta counters survive an aborted `ADD FULLTEXT`

`ha_tidesdb::inplace_alter_table` calls `fts_update_meta(share, txn,
share->cf, key_num, …)` **per row**. Those increments land in the table's
**data CF** (`share->cf`), and the scan commits in batches of
`TIDESDB_INDEX_BUILD_BATCH` rows.

If the build aborts after ≥1 committed batch (KILL, duplicate-key on a
UNIQUE add in the same ALTER, put error → `DBUG_RETURN(true)`), MySQL calls
`commit_inplace_alter_table(commit=false)`, which only drops the new index
CFs (`tidesdb_inplace_alter.cc:604-610`). It does **not** touch the FTS
meta key in `share->cf`. The inflated `total_docs` / `total_words` persist.
A subsequent `ADD FULLTEXT` for the same key number reads the stale base
and double-counts → skewed BM25 IDF / avgdl (wrong ranking; result *rows*
still correct).

Verified: `commit_inplace_alter_table` rollback branch (lines 604-610)
drops only `ctx->add_cf_names`; `share->cf` is the live table CF and is
never dropped on ALTER rollback.

Fix (recommended): mirror the existing bulk-insert M-4 design — accumulate
`doc`/`word` deltas in a local during the scan and issue a single
`fts_update_meta` **after** the final successful commit, in its own txn.
On any abort path nothing is written to meta. Bonus: removes the per-row
`fts_meta_mutex` acquire (see X-5).

---

## F-2 — [PRE] MEDIUM — inplace-built index entries ignore row TTL

`write_row` writes FTS/secondary entries with the row's computed
`row_ttl` (`ha_tidesdb.cc:4755`). The inplace populate path —
**both** the pre-existing non-FTS branch and the new FTS branch — hardcodes
`TIDESDB_TTL_NONE`. On a TTL-enabled table, back-filled index entries never
expire after the data row does → phantom index hits until compaction.

This is a pre-existing defect of the whole populate loop; the FTS fix
copied the surrounding convention rather than `write_row`'s. Proper fix is
loop-wide: derive per-row TTL after `deserialize_row` and pass it to every
populate `put`. Out of scope for a pure FTS fix; flagged for a follow-up.

---

## F-3 — [PRE] MEDIUM — populate `put` failures are "best effort, continue"

The new FTS `put` and the pre-existing non-FTS `put` log and continue on
`tidesdb_txn_put != TDB_SUCCESS`. `write_row` aborts the row (`goto err`).
A mid-build storage error therefore yields an ALTER that "succeeds" with a
silently incomplete index. Pre-existing populate-path policy; the FTS code
matched it. Recommend making populate `put` failure fatal (rollback +
`my_error` + `DBUG_RETURN(true)`) for both branches together.

---

## F-4 — [PRE] MEDIUM — `uint16` term-frequency can wrap silently

`fts_tf` (inplace) and `WriteRowFtsScratch::tf_map` (write_row) both use
`std::unordered_map<std::string,uint16>`; `++` wraps at 65535 and
`fts_build_value` stores the wrapped value. A pathological document
corrupts its own BM25 tf. Present identically in old and new code — fix
both: clamp to `UINT16_MAX` (or widen accumulator, truncate at build).

---

## F-5 — [PRE] MEDIUM — `blend_chars` / `ft_stopword_table` update callbacks log unsanitized value

`tidesdb_fts.cc:235` and `:~423` log the raw sysvar `new_val` via
`sql_print_*`. Every other site in the file routes through
`tdb_sanitize_for_log` (lines 288/318/339). A `SYSTEM_VARIABLES_ADMIN`
holder can embed newlines/escapes and forge error-log lines. Moved
verbatim in the A-2 extraction (pre-existing), but a cheap, isolated
consistency fix. Apply `tdb_sanitize_for_log` to `new_val` at both sites.

---

## F-6 — [PRE] LOW/MEDIUM — encrypted-flag change allowed INPLACE

`commit_inplace_alter_table` updates `share->encrypted` for an INPLACE
`CHANGE_CREATE_OPTION` that flips `ENCRYPTED`. Old rows stay
ciphertext/plaintext while new rows use the other form → `deserialize_row`
mis-handles the mismatched set. Pre-existing (moved verbatim in A-7).
Recommend: detect an encrypted-flag delta in
`check_if_supported_inplace_alter` and return
`HA_ALTER_INPLACE_NOT_SUPPORTED` (forces ALGORITHM=COPY).

---

## F-7 — [PRE] LOW — FTS rwlock PSI keys never registered

`tdb_blend_lock_key` / `tdb_stopword_lock_key` are zero-inited
`PSI_rwlock_key` with no `mysql_rwlock_register`. Locks omitted from
`performance_schema`. Pre-existing (same in original ha_tidesdb.cc).
Register them in `tdb_fts_init` (pattern: `g_trx_lifecycle_lock_key`).

---

## F-8 — [PRE] LOW — duplicate `"the"` in default stop-word list

`tdb_default_stopwords[]` lists `"the"` twice. Harmless (loaded into a
set); moved verbatim. Remove the duplicate.

---

## F-9 — [PRE] LOW — `strxnmov` reserves one NUL per argument

`tidesdb_portability.cc` `strxnmov` recomputes `avail = n-1` per argument,
under-using the buffer; can truncate up to 3 bytes for an exact-fit
caller. Sole current caller passes `size-1` so it's masked. Pre-existing
shim semantics.

---

## Not a bug (verified, dismissed)

- **keynr remap on drop+add in one ALTER** (agent HIGH): `ctx->add_key_nums`
  comes from `altered_table` indices, which already reflect the *final*
  post-ALTER key numbering; `commit_inplace_alter_table` rebuilds
  `idx_is_fts[i]` over the same `altered_table->s->keys`, and `ft_init_ext`
  uses the reopened table's key number == that index. Consistent. The
  earlier live test (docs table) confirmed `MATCH` + relevance work after
  `ADD FULLTEXT`. Worth a drop+add-in-same-ALTER regression test, but no
  defect.
- **Field-pointer rebase in the FTS back-populate** (`altered_table` +
  `table->record[0]`): verified correct — yields
  `ptrdiff = table->record[0] - altered_table->record[0]`, exactly the
  rebase the surrounding secondary-index code uses. All three agents and a
  hand-trace agree.
- **ODR / linkage of the static→extern promotions and header-moved
  `inline constexpr tdb_isolation_map[]` / `tdb_empty_val` / option
  structs**: sound. `inline constexpr` = one canonical definition (C++17);
  `schema_cf_store_frm` default args correctly live only in the header
  declaration. No duplicate-definition risk.
- **Extraction fidelity**: bodies moved verbatim, include chains complete,
  `tidesdb_compat.h` is now genuinely renames/`#define`-as-0/aliases only,
  `(extracted)`/`(moved)` pointer comments accurate.

---

## Recommended action order

1. **F-1** (HIGH, introduced this session) — accumulate FTS meta deltas,
   single post-commit write. Add a drop+add-in-one-ALTER regression test
   while here (covers the dismissed keynr concern too).
2. **F-5** (MED, cheap, isolated) — sanitize the two log sites.
3. **F-3 / F-2** (MED, pre-existing, populate-loop-wide) — make populate
   `put` failure fatal and thread per-row TTL through; do both branches
   together in one change.
4. **F-4 / F-6 / F-7 / F-8 / F-9** (lower, pre-existing) — batch as a
   hygiene pass.

Tests remain green through all reviewed commits: 37/37 hand-rolled, 58/59
MTR (the one failure is the documented `tidesdb_ttl` timing flake — passes
in isolation). Image rebuilt at `30a3eab`, smoke test clean, nothing pushed.
