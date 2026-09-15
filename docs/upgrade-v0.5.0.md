# Upgrading to v0.5.0 (TidesDB 10)

**v0.5.0 bundles TidesDB 10, which changed the on-disk format. A v0.4.x data
directory cannot be read by v0.5.0 and there is no in-place conversion.**
Upgrading means dumping your TidesDB tables from the old server and loading
them into a new one. Plan for the dump and reload; there is no shortcut.

Your data is not at risk from *trying*. If you point v0.5.0 at a v0.4.x data
directory it refuses to start the engine and changes nothing on disk — see
[What happens if you upgrade in place](#what-happens-if-you-upgrade-in-place).

---

## The short version

```sql
-- 1. On the OLD server (v0.4.x), dump the TidesDB tables.
--    From the shell:
--    mysqldump --databases mydb > tidesdb-dump.sql
```

```sql
-- 2. On the NEW server (v0.5.0), with a FRESH data directory:
SET GLOBAL tidesdb_legacy_compat = 'warn';
```

```bash
# 3. Load the dump. Retired ENGINE_ATTRIBUTE keys are ignored with a warning
#    each, instead of failing the statement.
mysql mydb < tidesdb-dump.sql
```

```sql
-- 4. Put the guard back, so new DDL carrying a retired key is refused.
SET GLOBAL tidesdb_legacy_compat = 'strict';
```

Step 2 matters because `mysqldump` reproduces `ENGINE_ATTRIBUTE` verbatim. A
dump taken from v0.4.x carries per-table options that TidesDB 10 no longer has,
and under the default `strict` setting each `CREATE TABLE` carrying one fails.
`warn` is what makes the documented upgrade path work; it is not a way to keep
using the old options, which are ignored either way.

---

## What happens if you upgrade in place

Nothing is lost, and the server tells you what is wrong:

```
[ERROR] [tidesdb] /var/lib/mysql/.tidesdb holds a TidesDB 9 data directory
        (found 'test__t1/config.ini', the per-column-family layout).
        TidesDB 10 stores data differently and cannot read it.
[ERROR] [tidesdb] Your data has not been modified. Start the previous release
        against this directory, mysqldump the TidesDB tables, then load the
        dump into this one. See docs/upgrade-v0.5.0.md.
[ERROR] [tidesdb] Refusing to start rather than opening it as an empty
        database, which would leave every table readable but empty.
```

The server itself starts; the TidesDB engine does not register, so
`SHOW ENGINES` does not list it and any query against a TidesDB table fails
loudly. Go back to the previous release, take the dump, and follow the sequence
above.

**Why the check exists.** Without it the outcome is worse than an error.
TidesDB 10 does not recognise the older layout, so it concludes the directory
is a brand-new database, writes its own manifest alongside your files, and
opens successfully. The server would start perfectly with every TidesDB table
present and empty — no crash, no message, nothing to notice until someone runs
a query. `scripts/test-cross-version.sh` asserts the refusal, and also that the
old files are still readable by the old release afterwards.

---

## Retired `ENGINE_ATTRIBUTE` keys

Under `tidesdb_legacy_compat=strict` (the default) these are refused with a
message naming what to do instead. Under `warn` they are ignored with that same
message as a warning.

### Moved to a server variable

The setting still exists, but it is database-wide rather than per table.

| retired key | set this instead |
|---|---|
| `write_buffer_size` | `@@tidesdb_unified_memtable_write_buffer_size` |
| `sync_mode` | `@@tidesdb_unified_memtable_sync_mode` |
| `sync_interval_us` | `@@tidesdb_unified_memtable_sync_interval` |

### Gone, with nothing to point at

The behaviour these selected no longer exists, so there is no replacement.

| retired key | why |
|---|---|
| `use_btree` | a key log is always a btree in TidesDB 10, and that btree is the index |
| `block_indexes` | block-format key logs are gone; the btree per key log is the index |
| `block_index_prefix_len` | block indexes no longer exist, so there is no prefix to size |
| `index_sample_ratio` | block indexes no longer exist, so there is nothing to sample |
| `min_disk_space` | the engine no longer reserves a per-family disk floor |
| `klog_value_threshold` | value separation is database-wide; a family either follows that threshold or sets `keep_values_inline` |
| `skip_list_max_level` | the memtable is database-wide, so its skip list is not per family |
| `skip_list_probability` | as above |
| `l0_queue_stall_threshold` | L0 admission is governed database-wide |
| `object_lazy_compaction` | object-store mode was removed |
| `object_prefetch_compaction` | object-store mode was removed |

---

## Removed system variables

### Object store

Object-store mode was removed from the engine in TidesDB 10. These stay
registered so an existing `my.cnf` gets an explanation rather than "unknown
variable", but they do nothing:

`tidesdb_object_store_backend`, `tidesdb_s3_endpoint`, `tidesdb_s3_bucket`,
`tidesdb_s3_prefix`, `tidesdb_s3_access_key`, `tidesdb_s3_secret_key`,
`tidesdb_s3_region`, `tidesdb_s3_path_style`,
`tidesdb_objstore_wal_sync_on_commit`, `tidesdb_replica_mode`

If any of them is set, the server **refuses to start** under
`tidesdb_legacy_compat=strict` and names the variable. Remove them from your
configuration, or set `tidesdb_legacy_compat=warn` to start anyway and get a
warning per variable.

`tidesdb_promote_primary` is refused outright: it drove object-store
replication, which is gone.

### Checkpoints

`tidesdb_checkpoint_dir` is refused whatever you set it to. TidesDB 10 takes
checkpoints in place and produces no snapshot directory. Use
`tidesdb_backup_dir` for a copy on disk — note that is a full backup, not a
hard-link snapshot, so size and duration are different.

---

## Two ways to get a v0.4.x `my.cnf` to start

1. **`tidesdb_legacy_compat=warn`** — the engine-specific hatch. Removed
   variables are accepted and ignored with a warning naming each one. Prefer
   this during the upgrade, then set `strict` once the configuration is clean.
2. **MySQL's `loose_` prefix** — `loose_tidesdb_s3_bucket=...` makes the server
   skip a variable it does not recognise. This is the standard mechanism and
   works for variables that were removed entirely, but it silences the
   explanation as well, so you get no reminder that the setting stopped doing
   anything.

---

## What you get in return

- **Two-phase commit that survives a crash.** Through TidesDB 9 the engine had
  no durable prepare, so the plugin committed during the prepare hook: a crash
  between prepare and commit left writes durable in the engine and absent from
  the binlog, and a binlog flush that failed after a successful prepare left
  the engine holding data the server had discarded. v0.5.0 prepares durably and
  resolves in-doubt transactions from the binlog at startup.
- **Value-log statistics**, exposed as `tidesdb_vlog_live_bytes`,
  `tidesdb_vlog_dead_bytes`, `tidesdb_vlog_segments` and
  `tidesdb_vlog_segments_drainable`.
- **Write-stall visibility**: `tidesdb_writes_throttled`,
  `tidesdb_writes_blocked`, `tidesdb_write_stall_us`,
  `tidesdb_write_stall_ceiling_hits`.
- **Better optimizer input.** Row and size estimates now include unflushed
  keys, which previously read as zero on a table whose writes had not yet been
  flushed, pushing the optimizer onto full scans.
