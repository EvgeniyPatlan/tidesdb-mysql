# Parity matrix

Generated: 20260518T152853Z

SUT A = MySQL 9.7 + tidesdb-mysql v0.2.1 `sut-mysql:bench`
SUT B = MariaDB 12.3.1 + upstream tidesql `sut-mariadb:bench`
Shared core: TidesDB v9.2.0 (both).

| Axis | Case | SUT A (MySQL) | SUT B (MariaDB) | Parity | Note |
|------|------|---------------|-----------------|--------|------|
| core | engine_basic_crud | OK | OK | SAME | - |
| indexes | secondary_index_range | OK | OK | SAME | - |
| transactions | txn_commit_rollback | OK | OK | SAME | - |
| online-ddl | inplace_add_drop_index | OK | OK | SAME | - |
| online-ddl | instant_add_column_backfill | OK | OK | SAME | - |
| fulltext | fulltext_natural_boolean | OK | OK | SAME | - |
| fulltext | fulltext_add_backpopulate_existing_rows | OK | WRONG | DIFF (A=OK B=WRONG) | HEADLINE: SUT A back-populates existing rows on ADD FULLTEXT (v0.2.1 + F-1); upstream B does not. |
| spatial | spatial_mbr_contains | OK | UNSUPPORTED | DIFF (A=OK B=UNSUPPORTED) | ARTIFACT: case uses MySQL-only `POINT ... SRID 0` DDL; needs a per-dialect spatial shim before any capability claim. |
| dml | replace_and_iodku | OK | OK | SAME | - |
| options | table_options_compression_bloom | OK | OK | SAME | - |
| encryption | at_rest_encryption_roundtrip | ERROR | ERROR | DIFF (both ERROR) | INCONCLUSIVE: both SUTs need a server keyring/master-key prerequisite absent in the base images; not an A-vs-B divergence. |

**8 / 11 cases fully identical (SAME).**

Raw client output per case+SUT: `results/20260518T152853Z/raw/` (gitignored).
