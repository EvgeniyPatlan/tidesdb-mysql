# TidesDB-MySQL Plugin — Deep Code Review Report

**Date:** 2026-05-12
**Scope:** `plugin/` only (4 files, ~11.7k LOC)
  - `ha_tidesdb.cc` (10,147 lines)
  - `ha_tidesdb.h` (964 lines)
  - `tidesdb_compat.h` (483 lines)
  - `tidesdb_keyring_compat.cc` (144 lines)
**Out of scope:** `vendor/mysql-server/`, `vendor/tidesdb/` — upstream code.
**Method:** Four specialist review passes run in parallel (C++ correctness/concurrency,
security, performance, architecture), then highest-impact claims spot-checked against
the actual source.

Findings marked **[verified]** were confirmed by direct read of the cited lines;
**[agent]** means the claim is reported as the reviewer described it without
independent verification (typically because verification would require running the
code or reading a wider call graph than was practical here). Every finding has a
file:line citation so you can verify any of them yourself.

---

## Executive summary

The plugin passes 37/37 hand-rolled tests and 50/50 MTR — functionally it works
for the workloads exercised so far. The issues found here are about what happens
under conditions the tests don't cover: shutdown races, encryption-key failure
modes, sustained FTS write load, adversarial spatial coordinates, and SQL-level
attacks by privileged users.

**Two findings are critical and should block any production deployment:**

1. **Encryption silently emits ciphertext with uninitialized stack bytes as the
   key or IV when `encryption_key_get`/`my_random_bytes` fail.** Data is
   unrecoverable; IV reuse breaks confidentiality. (See C-1.)
2. **`tdb_global` is a plain pointer mutated during shutdown without any
   synchronization with handler threads still running DML.** Trivial null-deref
   crash on shutdown under load. (See C-2.)

Beyond those, a meaningful list of **HIGH** items: a missed deadlock re-check
after cond-wait wakeup that turns deadlocks into hangs; FTS doc/word counters
lost-update under any non-serializable isolation (silently degrades BM25
ranking over time); the spatial-index Hilbert encoder corrupts roughly half its
keys on the first loop iteration because of an operator-precedence bug. Plus a
broken Index Condition Pushdown stub that means ICP is advertised but never
actually filters.

Architecturally, the plugin is one 10k-line `.cc` file. Three subsystems
(row-lock manager, FTS, spatial) are testable in isolation only after extraction.
MySQL 9.7 atomic-DDL participation (SE-private data callbacks) is completely
absent — there's no `sdi_set`/`sdi_get`/`dict_*` wiring; the `dd::Table*` params
on the inplace ALTER virtuals are all `[[maybe_unused]]`.

---

## CRITICAL — block production

### C-1. Encryption silently writes ciphertext under uninitialized key or IV
**Where:** `ha_tidesdb.cc:5134-5162` (encrypt), `ha_tidesdb.cc:5167-5196` (decrypt) — **[verified]**

`tidesdb_encrypt_row_into` declares `unsigned char key[TIDESDB_ENC_KEY_LEN]`
and `unsigned char iv[TIDESDB_ENC_IV_LEN]` on the stack, then calls
`encryption_key_get(...)` and `my_random_bytes(...)` discarding their return
values. Both can fail:

- `encryption_key_get` returns `-1` when the master key is not loaded
  (`tidesdb_keyring_compat.cc:101-111`). After failure, `key[]` is whatever
  uninitialized stack content the caller frame held.
- `my_random_bytes` (`tidesdb_compat.h:259-270`) returns `1` if it cannot
  read 16 bytes from `/dev/urandom`. After failure, `iv[]` is uninitialized.

`encryption_crypt` does internally gate on `g_master_key_loaded` and refuses
to encrypt when the key is unloaded, so today the first failure mode is
caught downstream — but it's caught **with** uninitialized bytes already
passed to it. Any future refactor that changes `encryption_crypt`'s gating
re-opens the door. The second failure mode (IV) is fully exploitable today:
a low-entropy `/dev/urandom` read failure produces silently-reused IVs in
CBC-mode AES, which breaks IND-CPA. Same flaw mirrored in the decrypt path.

**Fix:**
```cpp
if (encryption_key_get(key_id, key_version, key, &klen) != 0) return false;
if (my_random_bytes(iv, TIDESDB_ENC_IV_LEN) != 0) return false;
```
…and zero `key[]` before return with `OPENSSL_cleanse(key, sizeof(key))` to
limit key-disclosure window (see H-7).

### C-2. `tdb_global` shutdown race causes null-deref crashes
**Where:** `ha_tidesdb.cc:194` (declaration), `ha_tidesdb.cc:~2786` (read in `get_or_create_trx`), `ha_tidesdb.cc:~4049` (clear in `tidesdb_hton_panic`) — **[agent]**

`static tidesdb_t *tdb_global = NULL;` is a plain pointer with no atomic
qualification and no protecting mutex. Handler entry points dereference it
without a null-check; `tidesdb_hton_panic` sets it to NULL during server
shutdown. MySQL's shutdown does **not** guarantee all handler threads are
quiesced before `panic` is invoked. Any concurrent DML statement that wins
the race sees a null pointer and crashes mysqld.

**Fix:** Promote to `std::atomic<tidesdb_t *>` with acquire/release semantics
and a null-check at every read site that bails with `HA_ERR_NOT_IN_LOCK_TABLES`
or `HA_ERR_TABLE_NOT_LOCKED` if the engine is shutting down. Better, wrap in
an `EngineContext` with explicit lifecycle (see R-3 in Refactor section).

---

## HIGH

### H-1. Deadlock detector never re-runs after the condition-wait wakeup
**Where:** `ha_tidesdb.cc:397-412` — **[verified]**

`row_lock_acquire` runs `tdb_lock_would_deadlock` once before the wait
(line 373), but the `while (...) mysql_cond_wait(...)` loop only re-checks
`thd_killed(thd)`. If a new wait-for cycle forms *after* the initial check
but *before* the cond_wait wakes (a common scenario under contention),
the cycle is never detected. The acquiring thread waits indefinitely, or
until `lock_wait_timeout`. To the user this looks like a hung query rather
than `ER_LOCK_DEADLOCK`.

**Fix:** Re-run `tdb_lock_would_deadlock(trx, lock)` at the top of each
loop iteration, return `HA_ERR_LOCK_DEADLOCK` on positive.

### H-2. FTS doc/word counters lost-update under concurrent writes
**Where:** `ha_tidesdb.cc:656-677` (`fts_update_meta`), called from write_row (5827), update_row (6967), delete_row (7207) — **[agent]**

`fts_update_meta` does `fts_load_meta` → mutate in C++ → `tidesdb_txn_put`,
all within the caller's transaction. Under `READ_COMMITTED` or `SNAPSHOT`
isolation (the engine's defaults for non-serializable sessions), two
concurrent INSERTs each read the same `total_docs`, both increment by 1,
both commit. One increment is lost. BM25 ranking uses these counters; over
time, ranking quality degrades silently — no error, no warning.

**Fix:** Either (a) make the FTS meta CF SSI-isolated regardless of session
isolation, (b) store deltas rather than absolute counts and accumulate at
commit, or (c) serialize FTS meta writes through a per-table mutex held
across read-modify-write.

### H-3. Suspected use-after-free in deadlock graph walk
**Where:** `ha_tidesdb.cc:304-318` (walker), `:435-457` (release_all) — **[agent, partially verified]**

`tdb_lock_would_deadlock` atomically loads `target_lock->owner_trx`, then
dereferences the loaded pointer (`cur->waiting_on.load(...)`) without
holding any partition mutex on the target's partition. The walker comment
(lines 299-303) explicitly acknowledges racy state can produce stale
answers, claiming "neither outcome corrupts memory." But that holds only
if the `tidesdb_trx_t` struct outlives every walker that could observe a
pointer to it. The connection-close path frees the trx struct shortly
after `row_locks_release_all` clears `owner_trx` to NULL on all held
entries — there is no read-side critical section, no epoch protection,
no reference count. A walker on another thread that loaded the non-NULL
`owner_trx` *before* the release can dereference it *after* the free.

I didn't read the close path itself, so the lifetime claim is the agent's
assertion. If `tidesdb_close_connection` indeed `my_free`s the trx struct
without first quiescing all walkers, this is exploitable for memory
corruption from any concurrent DML.

**Fix:** Either (a) refcount `tidesdb_trx_t` (the walker holds a ref for
the duration of the walk), or (b) acquire the target partition mutex
before each deref, accepting the contention.

### H-4. Hilbert encoder corrupts spatial keys via operator-precedence bug
**Where:** `ha_tidesdb.cc:1280-1291`, specifically line 1288 — **[verified]**

```cpp
hilbert_rot((uint32_t)s << 1, &x, &y, rx, ry);
```

Operator precedence: cast first, then shift. `s` starts at `HILBERT_N >> 1`
= `2^31`. `(uint32_t)s` = `0x80000000`. `0x80000000 << 1` overflows the
32-bit type, yielding `0`. `hilbert_rot` receives `n = 0` and proceeds with
`n - 1 = UINT32_MAX`, then `n - 1 - *x` = `~*x`. The first iteration
encodes wrong coordinates — and the first iteration of a 32-level Hilbert
curve handles the highest bit, which is precisely the bit that determines
which top-level quadrant the point lives in. The code's own comment at
line 1278 ("The `s << 1` doubles s so hilbert_rot receives the full
sub-grid size for this level") describes the intended behavior — but the
implementation truncates before doubling.

Roughly half of all input coordinate pairs will be assigned wrong Hilbert
values, causing spatial range queries to miss them or return the wrong
matches. No error, no warning — just silently incorrect results.

**Fix:** Either widen `hilbert_rot`'s `n` parameter to `uint64_t`
(preferred — eliminates the truncation entirely), or shift first then cast:
`(uint32_t)(s << 1)`.

### H-5. S3 secret key visible to anyone with SYSTEM_VARIABLES_ADMIN
**Where:** `ha_tidesdb.cc:2033-2035`, also `:1997` (master key file path), `:2029` (access key) — **[verified]**

```cpp
static MYSQL_SYSVAR_STR(s3_secret_key, srv_s3_secret_key,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 secret access key", NULL, NULL, NULL);
```

`PLUGIN_VAR_READONLY` only prevents writes; it does not hide the value
from `SHOW GLOBAL VARIABLES`. Any DBA with `SYSTEM_VARIABLES_ADMIN`
(commonly granted in dev/staging environments) can read the secret in
plaintext. Same exposure on `srv_master_key_file` (path leaks key file
location), `srv_s3_access_key`. Combined with C-1's master-key handling,
this enables S3 + key-file exfiltration through purely SQL-level access.

**Fix:** Add `PLUGIN_VAR_NOSYSVAR` to hide from `SHOW VARIABLES`, or
register with a custom `SHOW_FUNC` returning `"***"`. Move the actual
value into a non-sysvar storage location once read at startup.

### H-6. Path traversal via `tidesdb_backup_dir` / `tidesdb_checkpoint_dir`
**Where:** `ha_tidesdb.cc:2133-2188` (backup check), `:2213-2249` (checkpoint check) — **[agent]**

The sysvar update callbacks pass the user-supplied path string directly
to `tidesdb_backup()` / `tidesdb_checkpoint()` with no canonicalization
and no allowed-root check. `SET GLOBAL tidesdb_backup_dir = '/etc'`
triggers a backup of the entire TidesDB data directory to `/etc` (writable
by the `mysql` OS user on default Ubuntu setups). Effective copy-anywhere
primitive available to any session with `SUPER` or `SYSTEM_VARIABLES_ADMIN`.

Related: `tidesdb_compat.h:246-253` `my_rmtree` wraps `nftw` with no path
confinement. Called from `force_remove_cf_dir` with a `cf_name` derived
from MySQL's path-to-cf-name mapping. The agent flagged a SUSPECTED
traversal risk if `cf_name` can ever contain `..`; I haven't verified
the sanitization in `path_to_cf_name`.

**Fix:** In both check callbacks, `realpath()` the destination and verify
it starts with an operator-configured allowed-backup-root prefix. Reject
paths containing `..` outright. For `force_remove_cf_dir`, audit
`path_to_cf_name` to confirm `..` cannot survive the transformation; add
a `realpath`-prefix check defensively.

### H-7. Encryption key bytes never zeroed after use
**Where:** `ha_tidesdb.cc:5137-5161` (encrypt), `:5175-5196` (decrypt) — **[verified]**

The 32-byte master key sits in a stack local until the function returns,
then is left on the stack frame for the next caller to potentially overlay
or for any memory-disclosure vulnerability to read. No `explicit_bzero`,
no `OPENSSL_cleanse`, no `memset` with `volatile` — even a plain `memset`
would be optimized away.

Functions run on every encrypted-table row write and read, so the key
material is repeatedly placed on the stack at the same depth in the call
chain. A core dump after any crash on an encrypted-table workload will
contain the key.

**Fix:** Add `OPENSSL_cleanse(key, sizeof(key))` on every exit path. If
not linking OpenSSL directly, use `explicit_bzero` (glibc/BSD) or a
volatile-pointer memset.

### H-8. Sysvar update callback mutates a connection thread's transaction
**Where:** `ha_tidesdb.cc:2133-2188` (`tidesdb_backup_dir_check`) — **[agent]**

The update-check callback for `tidesdb_backup_dir` reaches into a target
THD's `tidesdb_trx_t`, calls `tidesdb_txn_rollback(trx->txn)`, then sets
`trx->txn = NULL`. The target connection may be simultaneously executing
DML that touches `trx->txn` in `write_row`, `external_lock`, etc.
Unconditional data race on `trx->txn` per the C++ memory model: UB,
possible double-free if both sides call rollback.

This is architectural, not a missed lock — sysvar update callbacks
shouldn't touch per-connection transaction state at all.

**Fix:** Remove the forced rollback from the check callback. If the change
genuinely requires draining active transactions, set a flag the connection
poll at its next safe point (e.g., next `external_lock` entry).

### H-9. ENGINE_ATTRIBUTE JSON parser has no depth limit — stack overflow
**Where:** `ha_tidesdb.cc:2416-2418` (`tidesdb_engine_attribute_to_options`) — **[agent]**

`doc.Parse(attr.str, attr.length)` uses default rapidjson flags: recursive
descent, no depth cap (default 500). MySQL's DD persists ENGINE_ATTRIBUTE
to disk and reloads on every `OPEN TABLE`. A DBA who can create a table
with a 500-level-nested JSON attribute (e.g., `{"a":{"a":{"a":...}}}`)
causes a stack overflow during the next OPEN — and the OPEN happens on
every connection that touches the table, so the table is now
unrecoverable without dropping it via raw FS access.

**Fix:** Use `rapidjson::kParseIterativeFlag` to eliminate recursion, or
`SetMaxNestingDepth(32)` on the Document.

### H-10. ICP advertised but the check stub returns 0 unconditionally
**Where:** `tidesdb_compat.h:207`, called from `ha_tidesdb.cc:4667` (`icp_check_secondary`), advertised via `index_flags` — **[verified for stub, agent for advertise/consume]**

```cpp
inline int handler_index_cond_check(void * /*opaque*/) { return 0; }
```

The plugin's `index_flags` advertises `HA_DO_INDEX_COND_PUSHDOWN` (per the
perf reviewer; I did not verify). The optimizer therefore pushes index
conditions down to the engine. `icp_check_secondary` calls this stub on
every secondary-index entry — and the stub always returns 0, which the
plugin interprets as `CHECK_POS` ("accept"). Result: ICP appears to work
but filters nothing; every secondary-index entry triggers a full PK
fetch. For a `SELECT ... FROM t WHERE secondary_idx_col BETWEEN ... AND
... AND not_in_idx_col = 'x'`, ICP should have rejected most rows
without the PK lookup. Today every row pays the PK lookup.

**Fix:** Wire the stub to MySQL 9.7's actual ICP check (typically
`handler::pushed_idx_cond->val_int()` against the current record), or
remove `HA_DO_INDEX_COND_PUSHDOWN` from `index_flags` and tear out
`icp_check_secondary` so the optimizer stops pushing conditions.

---

## MEDIUM

### M-1. TLS table-options slot aliased across nested handler calls
**Where:** `ha_tidesdb.cc:2507-2520` — **[verified]**

`tidesdb_opts_for_table` returns a pointer into a single per-thread
`static thread_local ha_table_option_struct tls;`. The doc-comment
(lines 2500-2504) explicitly acknowledges this:

> Same-thread back-to-back calls for different tables refresh the buffer;
> this is fine because each TDB_TABLE_OPTIONS use site consumes the
> pointer immediately.

That invariant holds today but is unverifiable structurally. Any future
call site that obtains the pointer, makes a recursive handler call (e.g.
`prepare_inplace_alter_table` calling `TDB_TABLE_OPTIONS` for the new
table while a pointer to the old table's opts is still in a local), then
re-reads through the pointer will silently see the wrong options. The
compiler can't help.

**Fix:** Return the struct by value. Callers store it in an automatic
local — same cost, no aliasing risk.

### M-2. Hot-path `my_random_bytes` opens `/dev/urandom` per row
**Where:** `tidesdb_compat.h:256-271` — **[verified]**

Encrypted-table writes spend three syscalls per row (`open`, `read`,
`close`) just to fetch 16 bytes of IV. Correctness-wise fine, but this
is a measurable per-row tax — at 10k inserts/s it's 30k extra syscalls/s,
plus the kernel's per-open `/dev/urandom` accounting.

**Fix:** Linux `getrandom(buf, n, 0)` is a single syscall and exists on
Ubuntu 24.04. Or open `/dev/urandom` once at plugin init and keep the
fd as a file-static. Or thread-local ChaCha20-PRNG reseeded once.

### M-3. `schema_cf_rename` frees TidesDB-allocated memory with `free()`
**Where:** `ha_tidesdb.cc:~3551` — **[agent]**

Every other site in the file uses `tidesdb_free()` on values returned by
`tidesdb_txn_get`; this one site uses `free()`. With the default
allocator, harmless. With `TIDESDB_WITH_MIMALLOC` /
`TIDESDB_WITH_JEMALLOC` / `TIDESDB_WITH_TCMALLOC`, this is heap
corruption — `free()` from libc operates on the wrong heap.

**Fix:** Replace `free(val)` with `tidesdb_free(val)`. Grep for any
other lone `free()` calls on TidesDB-returned pointers.

### M-4. FTS metadata read-modify-write per row in bulk insert
**Where:** `ha_tidesdb.cc:656-677` (per-row `fts_update_meta`), called from 5827, 6967, 7207 — **[agent]**

Each FTS-indexed write_row/update_row/delete_row reads the doc/word
counter through the LSM (`tidesdb_txn_get`), increments, writes back
(`tidesdb_txn_put`). A 1000-row bulk INSERT fires 1000 point reads on
a single key, plus the lost-update issue under non-serializable
isolation (H-2). On a cold cache over a large SSTable set this read is
not free.

**Fix:** Accumulate the delta in a per-handler `int64_t` pair during
bulk insert, flush once at `end_bulk_insert`. This also incidentally
narrows the H-2 race window.

### M-5. FTS tokenization heap-allocates per token, per row
**Where:** `ha_tidesdb.cc:5809-5827` (write_row), `:7192-7207` (delete_row), `:6888-6960` (update_row) — **[agent]**

Each DML on an FTS-indexed table builds a fresh
`std::vector<fts_token_t>` (each element a `std::string`) and a
`std::unordered_map<std::string, uint16>`. No `reserve()`. A 100-word
document = 100+ `new`/`malloc` calls inside the row hotpath. Update_row
builds two of each.

**Fix:** `thread_local` scratch buffers cleared (not freed) between
rows. `fts_extract_and_tokenize` takes a pre-cleared
`vector<fts_token_t>&`. For documents with <512 unique terms, a sorted
small-vector is faster than an unordered_map and zero-alloc.

### M-6. Serialize/deserialize loop dispatches Field::pack virtually per field
**Where:** `ha_tidesdb.cc:5256-5261` (serialize), `:5375-5389` (deserialize) — **[agent]**

20-int-column table = 20 virtual dispatches per row write, 60 per row
read (the deserialize path also makes two `move_field_offset` calls
per field). For fixed-width non-null fields, `pack()` is essentially
`memcpy(pos, src, 4)` wrapped in a vtable lookup.

**Fix:** At `open()` time, set a `bool all_fixed_width` on the share.
If all fixed-width and not BLOB-heavy, do a single `memcpy` of the
fixed payload, bypass virtual dispatch entirely. Mirrors InnoDB's
fixed-row optimization.

### M-7. `key_copy_to_comparable` writes into `table->record[1]` per seek
**Where:** `ha_tidesdb.cc:4373-4390`, called from `index_read_map`:6199, `multi_range_read_init`:6638 — **[agent]**

Every secondary-index seek pollutes `record[1]` to unpack the
key_copy. For inner-loop joins this is per-probe overhead and shares
cache lines with the previous row buffer.

**Fix:** Use a per-handler dedicated buffer (`uchar
key_unpack_buf_[MAX_KEY_LENGTH]`) instead of `record[1]`. Add a
single-part-integer fast path that byte-swaps directly without
`key_restore`.

### M-8. `encryption_key_get` takes a mutex on every decrypt call
**Where:** `tidesdb_keyring_compat.cc:101-111` — **[agent]**

Once `g_master_key_loaded` is true, the master key is immutable for
the process lifetime — yet `encryption_key_get` unconditionally takes
`g_master_key_mu` and memcpys 32 bytes. On read-heavy encrypted-table
workloads this serializes all connections through one mutex.

**Fix:** Once `g_master_key_loaded` reads true (with acquire ordering),
cache the key bytes in a `thread_local unsigned char tls_key[32]`,
skip the mutex on subsequent calls.

### M-9. MRR builds `std::string` per range key
**Where:** `ha_tidesdb.cc:7641-7644` (`multi_range_read_init`) — **[agent]**

`WHERE col IN (1000 values)` = 1000 `std::string` allocations in the
MRR init path (most PKs exceed libstdc++'s 15-byte SSO).

**Fix:** Inline `uchar comp_key[MAX_KEY_LENGTH]; uint comp_len;` in
`tdb_mrr_entry`; comparator works on raw bytes.

### M-10. Lock-table chain entries never recycled
**Where:** `ha_tidesdb.cc:265-291` (`tdb_lock_find_or_create`), `:435-457` (release_all) — **[verified for design]**

`row_locks_release_all` clears `owner_txn_id`/`owner_trx` to zero/NULL
but does not unlink or free the entry. Entries accumulate forever in
the partition's hash chain. Long-running servers handling
high-cardinality unique PKs slowly grow the lock table without bound.
The chain walk per acquire is O(chain length).

**Fix:** Bound chain length; evict free uncontested entries when chain
exceeds some threshold. Or replace chaining with open-addressing per
partition. Pool entry allocations to eliminate double-malloc.

### M-11. Stopwords table-load holds the FTS rwlock across the scan
**Where:** `ha_tidesdb.cc:~5880-5930` (`tdb_load_stopwords_from_table_spec`) — **[agent]**

Acquires the FTS write-lock before opening a cursor and scans the
entire user-supplied stopwords table holding the write-lock. Concurrent
FTS reads on any FTS-indexed table block for the duration.

**Fix:** Build the new `unordered_set<string>` outside the lock, take
the write-lock only to swap the populated set into place.

### M-12. Suspected privilege bypass via FTS stopword table
**Where:** `ha_tidesdb.cc:777-870` (`tdb_load_stopwords_from_table_spec`) — **[agent, SUSPECTED]**

`SET GLOBAL tidesdb_ft_stopword_table = 'somedb/sometable'` causes the
plugin to open the named CF and read every row's value field into the
in-memory stopword set. The agent flags that this does *not* check
SELECT privilege on the table. If accurate, any user with
`SYSTEM_VARIABLES_ADMIN` can exfiltrate row contents from any
TidesDB-backed table — including across database boundaries — through
the stopword side channel.

**Fix:** Before opening the CF, run `check_table_access()` (or the
9.7 equivalent) for the current THD against the named table. Reject
if not authorized.

### M-13. `tidesdb_opts_for_table` re-parses JSON on every call
**Where:** `ha_tidesdb.cc:2507-2520` — **[verified for impl, agent for call frequency]**

Each call invokes `tidesdb_seed_opts_from_session` (25+ THDVAR reads)
plus a full rapidjson Parse of `engine_attribute`. Acceptable in
`create()`; expensive if called inside per-statement paths (the agent
didn't fully verify which paths invoke it).

**Fix:** Cache the parsed result in `TidesDB_share` at `open()` time
(when `TABLE_SHARE::engine_attribute` is immutable). The TLS-and-parse
path is only correct in `create()` anyway, before the share exists.

---

## LOW

### L-1. `LOCK_global_system_variables` mutex defined zero-initialized
**Where:** `tidesdb_compat.h:218` — **[verified, downgraded from agent's MEDIUM]**

```cpp
inline mysql_mutex_t LOCK_global_system_variables{};
```

Zero-init of an underlying `pthread_mutex_t` is platform-implementation-defined.
On Linux+glibc it happens to work; on macOS or BSD it is UB. **But** the
comment at lines 213-216 explicitly states "Currently unused in the MySQL
port" — kept only so any future ported call site compiles. A latent trap,
not an active bug.

**Fix:** Delete it. If a future port needs it, add proper `mysql_mutex_init`
during plugin init.

### L-2. `encryption_crypt` ignores its `iv_len` parameter
**Where:** `tidesdb_keyring_compat.cc:122-143` — **[agent]**

Parameter is `/*iv_len*/` (commented out). AES-CBC IV is hardcoded
internally. A future caller passing a non-16-byte IV will have the tail
silently treated as garbage.

**Fix:** `static_assert(TIDESDB_ENC_IV_LEN == 16, ...)` and document the
contract.

### L-3. Integer overflow in `encryption_encrypted_length` for huge rows
**Where:** `tidesdb_keyring_compat.cc:119`, called at `ha_tidesdb.cc:5144` — **[agent]**

`((src_len / 16) + 1) * 16` overflows `unsigned int` when
`src_len >= 0xFFFFFFF0`. The caller casts `plain.size()` (`size_t`) to
`unsigned int` without bounds check. A 4 GB+ row would wrap to a small
value, leading to heap buffer overflow.

In practice TidesDB block size caps row size below this, so unreachable
today.

**Fix:** Guard with `if (plain.size() > 0xEFFFFFFF) return false;` at the
crypto layer.

### L-4. S3 bucket/endpoint logged at INFO on every startup
**Where:** `ha_tidesdb.cc:3939-3940` — **[agent]**

`sql_print_information("[TIDESDB] S3 connector created (endpoint=%s, bucket=%s, ...)", ...)`.
Error logs commonly forward to CloudWatch/ELK. Bucket name + access key
(readable via H-5) lowers the bar for targeted S3 attacks.

**Fix:** Drop the bucket name, or redact to first 4 chars.

### L-5. `master_key_load` keeps key in process memory for server lifetime
**Where:** `tidesdb_keyring_compat.cc:64-85` and `g_master_key` global — **[agent]**

Temporary `buf` is zeroed correctly. `g_master_key` itself is cleared
only by `tidesdb_master_key_clear()`, documented as "test-only". Long-lived
key exposure window.

**Fix:** `mlock()` + `MADV_DONTDUMP` the page holding `g_master_key` so
it never appears in core dumps or swap. Implement runtime key rotation.

### L-6. `bulk_delete_min_pk_` / `max_pk_` allocate per deleted row
**Where:** `ha_tidesdb.cc:7152-7163` — **[agent]**

`std::string this_key(...)` constructed and two assignments per row
during bulk DELETE. 1M-row delete = 1-3M allocations purely for range
bookkeeping.

**Fix:** Fixed-size `uchar[DATA_KEY_BUF_LEN]` buffers + length. `memcmp`
for ordering, `memcpy` for updates.

### L-7. `update_row` rebuilds PK from new_data even when PK unchanged
**Where:** `ha_tidesdb.cc:6782, :6820` — **[agent]**

`pk_from_record(new_data, ...)` runs unconditionally. For
`UPDATE ... SET non_pk_col = ... WHERE pk = ...` (the common UPDATE
shape), this is wasted work — the PK didn't change.

**Fix:** Check `bitmap_is_set(table->write_set, pk_field)` for each PK
part first; if no PK field is in the write set, skip the rebuild.

### L-8. `fts_extract_and_tokenize` concatenates fields into one big string
**Where:** `ha_tidesdb.cc:1045-1063` — **[agent]**

Multi-part FULLTEXT index? `doc += field.val_str()` for each part,
allocating a flat buffer just to scan it once. A 64KB TEXT field =
64KB heap allocation per row.

**Fix:** Tokenizer accepts a callback; emit synthetic separators between
fields.

---

## Architecture and maintainability

The plugin is one 10,147-line translation unit. The architect's full
report (run as a separate pass) identified several extractable subsystems
and dead-code corners. Summarized here:

### A-1. Row-lock manager is testable in isolation only after extraction
**Where:** `ha_tidesdb.cc:80-460` (~450 lines)

The partitioned hash table, deadlock walker, and acquire/release are
correctness-critical (see H-1, H-3, M-10) but cannot be unit-tested
today — they require a full MySQL handlerton to stand up.

**Direction:** Extract to `tidesdb_row_lock.{h,cc}` exposing an opaque
`RowLockManager` class. Hide `tdb_row_lock_t` from the handler header.
A googletest target can then exercise multi-threaded contention
scenarios that today require MTR.

### A-2. FTS and spatial subsystems inlined into the handler TU
**Where:** FTS at `ha_tidesdb.cc:475-1216` (~740 lines), spatial at `:1218-1636` (~420 lines)

Each is its own product surface (tokenizer, BM25, boolean parser for FTS;
Hilbert encoding, WKB parser, MBR predicates for spatial), tangled with
handler dispatch through inline `if (ki->algorithm == HA_KEY_ALG_FULLTEXT)`
branches in write_row, update_row, delete_row.

**Direction:** Extract `tidesdb_fts.{h,cc}` and `tidesdb_spatial.{h,cc}`
with `FtsIndex` / `SpatialIndex` classes. Handler dispatch collapses to
`share->index(i).on_write(txn, pk, record, ttl)`. Spatial scan state moves
off the handler into a lazy `unique_ptr<SpatialScanState>`. Hilbert and
WKB parsing become standalone fuzz-testable units — relevant given H-4.

### A-3. `tidesdb_t*` and `tidesdb_column_family_t*` leak through the handler
**Where:** ~50+ direct `tidesdb_txn_put/get/delete/iter_new` call sites in handler methods

The handler is a thin shim over the TidesDB C API. Error reporting splits
across three channels (`tdb_rc_to_ha`, `my_error + DBUG_RETURN`, raw
`sql_print_error + return`). Mocking TidesDB for handler unit tests is
infeasible.

**Direction:** A thin `TidesStore` / `TidesTable` wrapper with verb methods
returning a unified `Result` type that already maps TidesDB rc → MySQL
handler code. Forces every error path through one helper. This is also
the natural seat for the MySQL 9.7 atomic-DDL participation discussed
in A-5.

### A-4. Global mutable state with no context object
**Where:** `tdb_global` (line 194), `schema_cf`, `tdb_path`, all `srv_*` and `srv_stat_*` (1638-2336, 9996-10127)

50+ call sites dereference globals directly. C-2 is one consequence;
the inability to stand up two engine instances for testing is another.
Initialization order is implicit (`tidesdb_init_func` populates, nothing
prevents access before/after).

**Direction:** Introduce `EngineContext` owning the engine handle, the
row-lock manager, the FTS stopword registry, master-key state, sysvar
block. One `static EngineContext* g_ctx` set by init. Subsystems take
`EngineContext&`. Migrate incrementally.

### A-5. MySQL 9.7 atomic-DDL participation is absent
**Where:** Handlerton init at `ha_tidesdb.cc:3802-4007` — no `sdi_set`, `sdi_get`, `sdi_get_keys`, `dict_init`, `dict_recover` callbacks. Inplace ALTER virtuals at `:9215, :9308, :9588` accept `dd::Table*` parameters and mark them all `[[maybe_unused]]`.

MySQL 9.7 expects storage engines to participate in atomic DDL by writing
SE-private data into the data dictionary. Without this, crash-during-DDL
behavior is undefined: TidesDB CF list and the MySQL DD can diverge,
leaving orphan CFs or orphan DD rows. There's also no `discover` path —
the three `discover_*` hooks are all wrapped in `#if 0` (lines 3572-3724,
~190 lines of dead code).

**Direction:** Short term — implement `sdi_set` to record CF list + table
options into `dd::Table::se_private_data` during `create()`, `sdi_get` to
read it back on `open()`. Medium term — route ALTER through the existing
inplace virtuals' `new_table_def` parameter as part of an
`InplaceAlterPlan` (see A-7).

### A-6. Dead corners from the MariaDB-port heritage
**Where:** `#if 0` blocks at 2523-2599, 3572-3724, 3826-3849 (~250 lines).
Plus methods marked "override removed: MariaDB-only signature" (e.g.
`multi_range_read_info_const` at 7555, `keyread_time`/`rnd_pos_time`,
`tdb_end_bulk_update`).

These are method bodies MySQL never calls. The `multi_range_read_info_const`
member computes cost estimates that nothing reads — a grep won't tell you
it's dead.

**Direction:** Delete. The MariaDB-port story belongs in a one-paragraph
file-header comment, not in 250 lines of `#if 0`.

### A-7. Inplace ALTER has no explicit state machine
**Where:** `ha_tidesdb.cc:9126-9759`, `ha_tidesdb.h:421-435`

The four virtuals share a `ha_tidesdb_inplace_ctx` holding three parallel
vectors. Failure-path cleanup (e.g. `inplace_alter_table` succeeded but
`commit_inplace_alter_table` is called with `commit=false`) depends on
implicit knowledge.

**Direction:** Extract `tidesdb_inplace_alter.{h,cc}` with an
`InplaceAlterPlan { phase: PREPARED|POPULATED|COMMITTED|ROLLED_BACK }`.
The four handler virtuals become 10-line dispatchers calling `plan->...`.
Natural seat for A-5's SDI work.

### A-8. `tidesdb_compat.h` mixes load-bearing and dead shims
**Where:** Entire 483-line file

Mix of:
- **Active, load-bearing:** `my_rmtree` (real `nftw`-based impl); the
  stderr `sql_print_*` redirect (with detailed comment explaining why
  structured logging won't work in MODULE_ONLY).
- **Inactive trap:** `LOCK_global_system_variables` zero-init (L-1),
  `handler_index_cond_check` returns 0 (H-10), `ha_create_table_option
  { int dummy; }`.
- **Drift candidates:** `IO_AND_CPU_COST` and the `keyread_time` /
  `rnd_pos_time` helpers that use it — referenced only by non-override
  members nothing calls.

**Direction:** Audit. Delete the inactive traps and `IO_AND_CPU_COST`.
Keep the active shims, rename them to `tidesdb_log_*` and move to
`tidesdb_log.{h,cc}` so they're not overriding MySQL symbols.

---

## Top three refactors (in order)

If you tackle nothing else from this report, do these three. They land
the file at roughly 6500-7000 lines, give you unit-testable seams for
the most failure-prone subsystems, and create the structural prerequisite
for atomic-DDL work.

1. **Extract the row-lock manager** (A-1). Highest correctness stakes
   (H-1, H-3, M-10), smallest extraction surface, easiest to unit-test.

2. **Extract FTS and spatial into their own TUs** (A-2). Removes ~1200
   lines from the handler, gets fuzz tests on Hilbert (H-4) and WKB
   parsing, and isolates two independent product surfaces.

3. **Introduce `TidesStore` and route every TidesDB call through
   `tdb_rc_to_ha`** (A-3). Single error-handling channel, mocking seam
   for handler unit tests, and the natural foundation for the MySQL 9.7
   atomic-DDL work (A-5).

---

## Findings summary table

| ID | Severity | Where | Issue |
|---|---|---|---|
| C-1 | CRITICAL | `ha_tidesdb.cc:5134-5196` | Encrypt/decrypt discard return values → uninitialized key or IV |
| C-2 | CRITICAL | `ha_tidesdb.cc:194, ~2786, ~4049` | `tdb_global` shutdown race → null deref |
| H-1 | HIGH | `ha_tidesdb.cc:397-412` | Deadlock not re-checked after cond_wait wakeup |
| H-2 | HIGH | `ha_tidesdb.cc:656-677` | FTS doc/word counter lost-update; degrades BM25 silently |
| H-3 | HIGH | `ha_tidesdb.cc:304-318, 435-457` | Suspected UAF in deadlock walker |
| H-4 | HIGH | `ha_tidesdb.cc:1280-1291` | Hilbert encoder: `(uint32_t)s << 1` overflows; corrupts spatial keys |
| H-5 | HIGH | `ha_tidesdb.cc:2033-2035, :1997, :2029` | S3 secret/master-key-path visible in `SHOW VARIABLES` |
| H-6 | HIGH | `ha_tidesdb.cc:2133-2188, :2213-2249` | `tidesdb_backup_dir` / `_checkpoint_dir` accept any filesystem path |
| H-7 | HIGH | `ha_tidesdb.cc:5137-5196` | Master-key stack bytes never zeroed; survives in core dumps |
| H-8 | HIGH | `ha_tidesdb.cc:2133-2188` | Sysvar update callback races connection thread on `trx->txn` |
| H-9 | HIGH | `ha_tidesdb.cc:2416-2418` | ENGINE_ATTRIBUTE JSON parser: unbounded recursion → stack overflow |
| H-10 | HIGH | `tidesdb_compat.h:207` | `handler_index_cond_check` always returns 0; ICP advertised but broken |
| M-1 | MEDIUM | `ha_tidesdb.cc:2507-2520` | TLS opts struct aliased if nested handler calls happen |
| M-2 | MEDIUM | `tidesdb_compat.h:256-271` | `my_random_bytes` opens /dev/urandom per row |
| M-3 | MEDIUM | `ha_tidesdb.cc:~3551` | `free()` on TidesDB-allocated buffer — heap corruption with mimalloc/jemalloc |
| M-4 | MEDIUM | `ha_tidesdb.cc:656-677` | FTS meta RMW per row; expensive in bulk insert |
| M-5 | MEDIUM | `ha_tidesdb.cc:5809-5827, 6888-6960, 7192-7207` | FTS tokenization allocates per token per row |
| M-6 | MEDIUM | `ha_tidesdb.cc:5256-5261, 5375-5389` | Field::pack virtual-dispatched per field per row |
| M-7 | MEDIUM | `ha_tidesdb.cc:4373-4390` | `key_copy_to_comparable` writes to `record[1]` per seek |
| M-8 | MEDIUM | `tidesdb_keyring_compat.cc:101-111` | Mutex taken on every decrypt despite key being immutable |
| M-9 | MEDIUM | `ha_tidesdb.cc:7641-7644` | MRR `std::string` per range key |
| M-10 | MEDIUM | `ha_tidesdb.cc:265-291, 435-457` | Lock-table entries never recycled; unbounded growth |
| M-11 | MEDIUM | `ha_tidesdb.cc:~5880-5930` | Stopword load holds FTS write-lock across table scan |
| M-12 | MEDIUM | `ha_tidesdb.cc:777-870` | SUSPECTED: stopword-table-load bypasses table-level grants |
| M-13 | MEDIUM | `ha_tidesdb.cc:2507-2520` | `tidesdb_opts_for_table` re-parses JSON on every call |
| L-1 | LOW | `tidesdb_compat.h:218` | Zero-init mysql_mutex_t; UB on macOS/BSD but currently unused |
| L-2 | LOW | `tidesdb_keyring_compat.cc:122-143` | `encryption_crypt` ignores `iv_len` parameter |
| L-3 | LOW | `tidesdb_keyring_compat.cc:119` | Integer overflow on >4GB rows in encrypted_length |
| L-4 | LOW | `ha_tidesdb.cc:3939-3940` | S3 bucket/endpoint logged at INFO on startup |
| L-5 | LOW | `tidesdb_keyring_compat.cc:64-85` | Master key never `mlock`'d; survives in swap and core files |
| L-6 | LOW | `ha_tidesdb.cc:7152-7163` | `std::string` per row in bulk-delete range tracking |
| L-7 | LOW | `ha_tidesdb.cc:6782, :6820` | `update_row` rebuilds PK even for non-PK updates |
| L-8 | LOW | `ha_tidesdb.cc:1045-1063` | FTS field concatenation allocates flat doc buffer |

| ID | Refactor (architecture) |
|---|---|
| A-1 | Extract row-lock manager to its own TU |
| A-2 | Extract FTS and spatial subsystems |
| A-3 | `TidesStore` abstraction; single error channel through `tdb_rc_to_ha` |
| A-4 | `EngineContext` to replace global mutables |
| A-5 | MySQL 9.7 atomic-DDL participation (SDI callbacks) |
| A-6 | Delete `#if 0` blocks and MariaDB-only methods |
| A-7 | Explicit state machine for inplace ALTER |
| A-8 | Audit `tidesdb_compat.h`; delete inactive traps |
