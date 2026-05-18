# Parity findings (first corpus, 11 cases)

Run: `results/20260518T152853Z/` · SUT A = MySQL 9.7 + tidesdb-mysql
v0.2.1 · SUT B = MariaDB 12.3.1 + upstream tidesql · shared TidesDB
v9.2.0. **8/11 identical.**

These are interpretations of the generated matrix; the matrix +
per-case raw client output are the primary evidence.

## 1. Real product divergence (the article's headline)

**`ADD FULLTEXT INDEX` on a table that already has rows.**

- SUT A (tidesdb-mysql v0.2.1): back-populates existing rows; `MATCH`
  returns them ranked correctly (`4,1,2`), and the drop/re-ADD cycle
  returns the identical ranking — the v0.2.1 back-populate feature plus
  the F-1 meta-counter fix, confirmed end-to-end against the shipped
  image.
- SUT B (upstream tidesql on MariaDB): returns **empty** — existing
  rows are not indexed when the FULLTEXT index is added after the data.

This is a genuine behavioural difference attributable to the
integration glue (TidesDB core is identical on both), and it is exactly
the kind of result the article should lead with. Both engines agree on
FULLTEXT created *with* the table (`fulltext_natural_boolean` = SAME),
so the divergence is specifically the back-populate path.

## 2. Strong parity elsewhere (8 SAME)

Core CRUD, secondary-index range scans, txn commit/rollback, inplace
add/drop index, instant add-column backfill, FULLTEXT natural/boolean
query, REPLACE / INSERT…ON DUPLICATE KEY UPDATE, and the per-table
option round-trip (through the dialect shim: MySQL `ENGINE_ATTRIBUTE`
JSON vs MariaDB option grammar) all produce byte-identical results.
That the option shim case is SAME validates the shim approach for the
rest of the corpus.

## 3. Not yet conclusive — corpus/setup work, NOT product claims

- **spatial_mbr_contains** — SUT B failed at parse time on
  `POINT NOT NULL SRID 0` (MySQL-8.0+ column syntax MariaDB rejects).
  This is a **case-portability artifact**, not evidence that upstream
  lacks spatial. Needs a per-dialect spatial-DDL shim entry (MariaDB
  spatial column declaration differs) before any spatial capability
  comparison can be made. Do not cite this as a gap.
- **at_rest_encryption_roundtrip** — **both** SUTs ERROR identically
  (CREATE with the encryption option fails → table absent). Encryption
  needs a server keyring / master-key prerequisite that neither base
  image provisions. This is a setup gap affecting both equally, not an
  A-vs-B divergence; it needs the keyring configured in both SUT images
  before it can be measured.

## 4. Corrections applied during review (process integrity)

- `secondary_index_range` initially showed DIFF (both WRONG) — that was
  a wrong `@expect` in the case (`4,1` vs the correct ascending `1,4`),
  not a product issue. Fixed; now SAME. Logged here because the article
  must distinguish real divergences from harness/author error, and this
  is the audit trail.
- Two harness bugs were fixed before the numbers were trusted: errexit
  aborting on benign helper non-zero, and a MySQL readiness race (the
  official image's init→restart) that produced a spurious `ERROR 2002`
  on the first case. The matrix was only committed after these were
  resolved and a clean run reproduced.

## 5. Next corpus increments

- Spatial DDL dialect shim → re-enable case 08 as a fair comparison.
- Keyring/master-key in both SUT images → make case 11 conclusive.
- Expand FULLTEXT axis (phrase, stopwords, blend chars, the F-1
  abort/retry scenario) since that is where the real divergence lives.
- Add isolation-level, TTL-expiry (deterministic), and crash-recovery
  cases per `../README.md` §4.2.
