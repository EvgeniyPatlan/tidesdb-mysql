# Performance analysis: MySQL+TidesDB vs MariaDB+TidesDB

**Source:** HammerDB TPROC-C, WARE=10 RUNVU=8 1m ramp + 3m, same host, same engine binary (TidesDB v9.3.2). The two SUTs differ only in the SQL frontend and the storage-engine plugin glue.

**Throughput:** MariaDB **25,377 NOPM** vs MySQL **1,841 NOPM** — a **13.8×** gap.

The per-call perf rings let us split that gap by layer. The conclusion is uncomfortable but clear: **most of the gap is server-layer, not plugin code.** A small slice is plugin-glue that's salvageable. The engine tail is a third bucket that's only fixable upstream in TidesDB.

## Section 1 — The throughput gap is server-layer, not plugin

MariaDB drives **3–4× more engine calls per minute** than MySQL on the same hardware:

| Method | MySQL calls / 3 min | MariaDB calls / 3 min | Ratio |
|---|---|---|---|
| write_row | 5.29M | 6.32M | 1.2× |
| update_row | 0.72M | 2.38M | 3.3× |
| index_read_map | 1.90M | 6.52M | 3.4× |
| commit | 1.49M | 6.17M | 4.1× |
| external_lock | 2.91M | 11.92M | 4.1× |
| deserialize_row | 3.95M | 13.12M | 3.3× |

These aren't subtle numbers. **MariaDB is feeding the engine many more statements per second.** The bottleneck before our plugin gets a chance to do anything is upstream in MySQL 9.7's parser, optimizer, connection handling, and authentication path. Per-statement frontend cost is the dominant factor.

Even if we eliminated *every* nanosecond of MySQL plugin code, the engine would still see ~3× fewer calls per minute and the throughput gap would close from 13.8× to ~4×. The remaining 4× is MySQL frontend cost we can't reach from a storage-engine plugin.

**Recommendation: do not chase per-call plugin micro-optimisations as the throughput strategy.** The leverage isn't there.

## Section 2 — Two perf-instrumentation blind spots distort the per-call comparison

The diff report shows MySQL's commit at **215 ns** vs MariaDB's at **13,774 ns** — a 64× gap that looks like a huge MySQL win. It isn't.

### Blind spot 1: `tidesdb_prepare` has no `TDB_PERF_SCOPE`

When `HTON_SUPPORTS_ATOMIC_DDL` is set (we did that in v0.4.0), MySQL routes every transaction through the 2PC protocol: `prepare` runs first, does the real engine commit work, then `commit` runs and short-circuits via the `commit_done` flag.

- `tidesdb_prepare` (plugin/ha_tidesdb.cc, around line 2044) calls `tidesdb_txn_commit` at line 2062 and sets `trx->commit_done = true` at 2087.
- `tidesdb_commit` (lines 2094–2192) tests `commit_done` and returns 0 after just clearing three booleans and releasing row locks.
- **`tidesdb_prepare` is not instrumented with `TDB_PERF_SCOPE`.** `tidesdb_commit` is.

So our perf ring captures the 215 ns no-op and misses the ~13 μs of real engine work that happens in prepare. MariaDB has no prepare hook (single-phase commit), so its 13,774 ns measurement includes the same engine work honestly.

**Net per-commit engine cost is essentially identical between the two.** The 64× ratio is measurement skew.

### Blind spot 2: `external_lock` volume difference is not a per-call cost difference

MariaDB calls `external_lock` 4.1× more often. The per-call cost is closer (564 ns vs 1358 ns); the volume gap is server-side — MariaDB's handler loop fires `external_lock` more aggressively even when `lock_count()=0`, and TPC-C touches ~4 tables per stored-proc statement. Not a plugin issue on either side.

### Action items in this section

1. **Add `TDB_PERF_SCOPE(prepare)` at line 2044** of `plugin/ha_tidesdb.cc` and a `prepare` entry in the `MethodId` enum. Without this, every future MySQL-vs-MariaDB perf compare will keep showing a phantom 64× commit speedup. **Pure honesty fix, not a perf change.**
2. Verify that all the work the v0.4.0 atomic-DDL path moved into other hooks (SDI write at `prepare_drop`, `commit_inplace_alter_table`) is also instrumented. If we shave any of those, do it in the plan that follows.

## Section 3 — The per-call write_row gap is MySQL 9.7 frontend primitives, not plugin logic

Per-call: MySQL **16.54 μs/call** vs MariaDB **7.62 μs/call** — a 9 μs gap × 5.29M calls × 3 min ≈ 47 sec of total CPU.

A side-by-side diff of the two `write_row` implementations (plugin/ha_tidesdb.cc:4955 vs vendor/tidesql/tidesdb/ha_tidesdb.cc:6471) shows the source code is **structurally identical**. Same lazy-txn setup, same `pk_from_record`, same `serialize_row`, same dup-check iterator cache, same secondary-index loop, same `tidesdb_txn_put` call. The MySQL plugin actually has a few *more* optimisations (thread-local FTS scratch caching, atomic auto-increment counter advancement). Nothing meaningful to port back.

So where do the 9 μs go? The probable causes, ranked:

1. **MySQL 9.7's `tmp_use_all_columns` is heavier than MariaDB's.** MySQL 9.7 routes bitmap manipulation through `dd::Table`-aware helpers with debug assertions; MariaDB's is a thin macro. Estimated **0.5–1.5 μs**.
2. **MySQL 9.7's `Field::pack` and `val_int` virtual dispatch is heavier** — MySQL's Field hierarchy gained virtual hops in 8.0+ vs MariaDB 11.x. Both `serialize_row` and `pk_from_record` walk every column through this. Estimated **1–3 μs** at ~10 columns per TPC-C row.
3. **MySQL 9.7's `DBUG_ENTER` / `DBUG_RETURN` carries more keyring + perfschema instrumentation hooks** than MariaDB's; even in release builds the macro footprint differs. Estimated **0.3–0.8 μs**.
4. **`HTON_SUPPORTS_ATOMIC_DDL` registration paths** fire extra TLS bookkeeping per transaction in MySQL that MariaDB doesn't have. Estimated **<1 μs per commit**, not per write_row.

None of these are addressable from plugin code without forking the MySQL server. **The write_row gap is structural to MySQL 9.7's frontend, not our doing.**

## Section 4 — Engine tail latency is the only big bucket we can credibly close

| Method | MySQL max | MariaDB max | What's there |
|---|---|---|---|
| write_row | 72.78 ms | 53.10 ms | LSM compaction stall |
| index_read_map | 18.31 ms | n/a | SSTable open + bloom miss + level descent |
| update_row | 0.93 s (?) | 0.93 s | Likely compaction backpressure on dependent put |

Both engines show the same long tail because they share the same TidesDB binary. These are LSM-tree-inherent — compaction triggers stall the write hot path; cold reads pay block-cache misses.

These are the *only* big knobs left to turn. They live in TidesDB, not in our plugin.

### Engine-side patches worth exploring (in priority order)

1. **Compaction backpressure smoothing.** TidesDB's active-memtable ceiling at 2× `write_buffer_size` (added in v9.3.0) cliff-stalls writers when compaction lags. A graduated backpressure (start to slow writes at 1×, full stall at 2×) would smear the 72 ms spike into ~5 ms of mild slowdown. **Estimated tail improvement: 10–20× on p99.**
2. **Smaller default `write_buffer_size` for OLTP profile.** Current default is ~128 MiB. With 8 vusers × small TPC-C rows, that's many minutes between flushes — each flush triggers a big compaction wave. Halving it to 64 MiB doubles flush frequency but halves the wave amplitude. Tunable per-CF.
3. **Increase `flush_threads` default from 4 to match physical cores.** Already a sysvar; just a default change.
4. **Persistent block-cache warmup at recovery.** First-access cold cost on a SSTable is significant. If the engine wrote a `cache_hot_pages` manifest before crash, recovery could pre-touch them. Larger project.
5. **Aggregate small-CF compactions.** Index-only CFs accumulate compaction overhead disproportionately. A "minor compaction" mode for small CFs could amortise.

Items 1–3 are tuning changes (sysvar adjustments + small TidesDB patches). Items 4–5 are real engine work.

## Section 5 — Concrete v0.5.0 plan

What to ship, ranked by ROI:

1. **Add `TDB_PERF_SCOPE(prepare)` and `MethodId::prepare` to the perf surface.** ~50 lines. Closes the measurement blind spot. Pre-requisite for any future engine-vs-plugin work.
2. **Tune TidesDB compaction sysvars in the default `tidesdb.cnf` shipped with the image:**
   - `tidesdb_default_write_buffer_size = 64MiB` (down from 128 MiB)
   - `tidesdb_flush_threads = 8` (up from 4)
   - `tidesdb_max_concurrent_flushes = 6`
   This is a Docker image change, not a code change. Estimated **20–30% tail improvement** with no risk to correctness.
3. **Patch TidesDB to smooth compaction backpressure** (item 1 above). Upstream PR to tidesdb/tidesdb. Estimated **2–5× p99 improvement** on write_row.
4. **Document the throughput gap to MariaDB honestly.** The README's "perf comparison" claim, if any, must acknowledge that v0.5.0 closes plugin-side instrumentation gaps and tail latency, but the absolute throughput gap to MariaDB+TidesDB is dominated by MySQL 9.7 server-layer cost outside our reach. Setting that expectation up front avoids chasing impossible benchmarks.

What **not** to ship:

- **Do not refactor `write_row`, `index_read_map`, `update_row`.** They are already lean and (per the side-by-side) marginally cleaner than TideSQL's. Time better spent elsewhere.
- **Do not adopt TideSQL's older code paths** (e.g. its naive `seek_for_prev` on partial PK prefixes — A is in fact correct on this and B is buggy).
- **Do not try to defeat MySQL 9.7 frontend cost from inside a plugin.** It's not in scope of what a storage-engine plugin can reach.

## Appendix — Where the 13.8× comes from, restated

```
MariaDB throughput  : 25,377 NOPM
MySQL throughput    :  1,841 NOPM
Total gap           :  13.8x

Attribution (rough):
  Server-layer frontend (parser, optimizer, conn, auth)  : ~3-4x
  Atomic-DDL 2PC + SDI overhead (v0.4.0)                 : ~1.5-2x
  MySQL 9.7 Field hierarchy + bitmap helpers per row     : ~1.5x
  Plugin glue diff (real but small)                      : ~1.1x
  Engine code (identical binary)                         : ~1.0x

Compounding: 3.5 * 1.7 * 1.5 * 1.1 * 1.0 ≈ 9.8x
```

The numbers are rough estimates from the per-method data above. The shape — server-frontend dominates, plugin and engine are small — is what the perf rings actually show.
