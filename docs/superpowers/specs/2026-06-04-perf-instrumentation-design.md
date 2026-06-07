# Perf instrumentation — layer-by-layer timing (design spec)

| | |
|---|---|
| **Author** | brainstorm session, 2026-06-04 |
| **Status** | Design — awaiting implementation plan |
| **Target release** | v0.4.1 (infrastructure only) |
| **Motivation** | Identify the plugin-side fraction of the MySQL+TidesDB vs MariaDB+TidesDB performance gap |
| **Predecessor work** | atomic-DDL participation (v0.4.0, shipped 2026-06-04) |

## 1. Summary

Build the instrumentation primitive that tells us, with cycle-accurate fidelity, where time goes inside the TidesDB-MySQL plugin during a real workload. Then port the same primitive to TideSQL (the MariaDB plugin) and run identical workloads side-by-side. The output is a `(method, calls, ns_per_call)` table per SUT and a diff that ranks methods by how much wallclock MariaDB saves vs us. That diff drives the next release's optimisation targets — but **this spec is infrastructure-only; no optimisations are part of v0.4.1**.

The instrumentation uses a thread-local ring buffer of `rdtsc`-sampled spans (one sample = `{method_id, enter_tsc, exit_tsc}`), flushed to per-method binary files by a 1 Hz background thread. An offline Python tool converts the binaries to histograms + side-by-side reports. Cost-when-off is one branch (~3 ns); cost-when-on is one rdtsc pair plus a ring slot write (~30 ns).

## 2. Why now

v0.4.0 shipped the atomic-DDL contract — a correctness release, not a perf release. The HammerDB v0.3.1 vs v0.4.0 head-to-head confirmed no DML regression (1877 NOPM vs 1784 NOPM, within ±10-15% single-iteration noise). What it did not tell us is *where* in the plugin code path the time goes, or how that compares to MariaDB+TidesDB on the same engine and same hardware.

Earlier session-level investigation (memory: [[mwbench-engine-coupling]] + prior MySQL-vs-MariaDB perf work) suggested the MySQL 9.7 server layer has roughly 4× overhead vs MariaDB on an identical engine. Most of that gap is structural to MySQL (per-statement parser cost, binlog group commit, the data-dictionary layer). But some non-zero fraction is in our plugin. **The point of v0.4.1 is to quantify which fraction is ours, so the next release can attack it specifically.**

## 3. Scope

**In scope for v0.4.1 (infrastructure release):**

1. Thread-local ring buffer + `Sample` primitive.
2. `TDB_PERF_SCOPE(MethodId)` RAII macro, compiled in under `-DTIDESDB_PERF=1`.
3. Background flusher thread with per-method binary output files.
4. Instrumentation sites at **32 plugin entry points** (full handler vtable + inplace-alter virtuals + 6 deeper helpers).
5. Calibration step that captures and reports the per-scope overhead floor.
6. Four new sysvars: `tidesdb_perf_capture` (BOOL, default OFF, runtime), `tidesdb_perf_output_dir` (STR, runtime), `tidesdb_perf_ring_capacity_pow2` (INT, server-start-only), `tidesdb_perf_flush_interval_ms` (INT, runtime).
7. Offline tool `tools/tidesdb-perf-analyze` (Python; 150 lines target).
8. Side-by-side TideSQL port: same primitive + macro vendored into the TideSQL source tree; same 32 method ids; same offline tool consumes both.
9. Bench harness `bench/perf/run-perf-capture.sh` that runs HammerDB + extracts data + produces side-by-side diff.
10. 8 unit tests + 7 MTR tests + an integration test that produces a real diff report.

**Out of scope (deferred to v0.4.2 / v0.5.0):**

- Any actual performance optimisation — those come from reading this release's diff report.
- `performance_schema` integration (a future release if histograms become more useful than the offline tool).
- Flamegraph integration / `perf record` plumbing.
- Per-CPU-core attribution (we'll bind the flusher; we won't track which core each sample came from).
- Sample upload to a remote analytics service.
- Histograms for the deep engine path inside TidesDB itself — that's engine work, not plugin work.

## 4. Architecture

Five units. Each has one clear purpose and a well-defined interface.

| Unit | Role |
|---|---|
| `plugin/tidesdb_perf_ring.h` | `Sample`, `MethodId`, `TLS_Ring`, `g_rings_head`, `g_capture_active` |
| `plugin/tidesdb_perf_ring.cc` | Ring allocation + tombstoning + flusher thread |
| `plugin/tidesdb_perf_scope.h` | `PerfScope` RAII + `TDB_PERF_SCOPE` macro |
| `plugin/ha_tidesdb.cc` + sibling TUs | 32 `TDB_PERF_SCOPE(...)` sites, one per method id |
| `tools/tidesdb-perf-analyze` | Python offline analyser; reads binary files, emits markdown + CSV |

The macro `TDB_PERF_SCOPE(id)` is gated by **both** a compile-time flag (`-DTIDESDB_PERF=1`, default ON in debug builds, OFF in release builds) **and** a runtime sysvar (`tidesdb_perf_capture`). The runtime sysvar is the operator-facing switch; the compile-time flag is the build-time decision for whether to ship the instrumentation at all. For perf measurement runs we ship a dedicated image `tidesdb/mysql:9.7-perf`.

Side-by-side TideSQL is the same five units vendored into TideSQL's source tree under a dedicated header — same `Sample`, same `MethodId` enum, same `TLS_Ring`, same `PerfScope`. The same offline tool reads from both.

## 5. Components

### `Sample` and `MethodId`

```cpp
enum class MethodId : uint8_t {
    // Handler vtable — DML row hotpath (8)
    write_row = 1, update_row, delete_row,
    index_read_map, index_next, index_prev, rnd_next, rnd_pos,
    // Handler vtable — txn control (8)
    external_lock, start_stmt, store_lock,
    commit, rollback,
    savepoint_set, savepoint_release, savepoint_rollback,
    // Handler vtable — table lifecycle (6)
    open, close, info, table_flags_cache_init, create, delete_table,
    // Inplace ALTER (4)
    check_if_supported_inplace_alter,
    prepare_inplace_alter_table,
    inplace_alter_table,
    commit_inplace_alter_table,
    // Plugin-private deeper helpers (6)
    serialize_row, deserialize_row,
    key_copy_to_comparable, pk_from_record,
    encrypt_row_into, decrypt_row,
};
constexpr uint8_t kMethodCount = 32;

struct Sample {
    uint8_t  method_id;
    uint8_t  thread_id;   // lower 8 bits of pthread_self() hash
    uint16_t reserved;    // alignment padding
    uint64_t enter_tsc;
    uint64_t exit_tsc;
};
static_assert(sizeof(Sample) == 24);
```

### `TLS_Ring`

```cpp
struct TLS_Ring {
    static constexpr size_t kCapacity = 64 * 1024;   // 1.5 MiB per thread
    alignas(64) std::atomic<uint64_t> write_idx;
    alignas(64) std::atomic<uint64_t> read_idx;      // flusher-owned
    alignas(64) Sample slots[kCapacity];
    std::atomic<TLS_Ring *> next;
    std::atomic<bool> tombstoned;
    std::atomic<uint32_t> wrap_count;
    uint64_t owner_tid;
};

thread_local TLS_Ring *t_ring = nullptr;
std::atomic<TLS_Ring *> g_rings_head;
std::atomic<bool> g_capture_active;
```

### `TDB_PERF_SCOPE`

```cpp
#if TIDESDB_PERF
class PerfScope {
    MethodId m_id;
    uint64_t m_enter;
public:
    explicit PerfScope(MethodId id) : m_id(id), m_enter(__rdtsc()) {}
    ~PerfScope() {
        if (!g_capture_active.load(std::memory_order_relaxed)) return;
        uint64_t exit = __rdtsc();
        if (!t_ring) t_ring = ring_alloc_for_this_thread();
        if (t_ring == reinterpret_cast<TLS_Ring *>(0x1)) return;  // OOM sentinel
        uint64_t w = t_ring->write_idx.fetch_add(1, std::memory_order_relaxed);
        Sample &s = t_ring->slots[w & (TLS_Ring::kCapacity - 1)];
        s.method_id = static_cast<uint8_t>(m_id);
        s.thread_id = thread_id_for(t_ring->owner_tid);
        s.enter_tsc = m_enter;
        s.exit_tsc  = exit;
    }
};
#define TDB_PERF_SCOPE(id) PerfScope _tdb_perf_##__LINE__{MethodId::id}
#else
#define TDB_PERF_SCOPE(id) ((void)0)
#endif
```

### Flusher thread

Started by `tidesdb_perf_init()` (called from `tidesdb_init_func` when the sysvar is ON at server start). Ticks at `tidesdb_perf_flush_interval_ms` (default 1000 ms). For each ring in `g_rings_head`: snapshot `(W, R)`, bucket samples by method_id into a small stack-local array of 32 vectors, `pwritev` per method to `<output_dir>/<pid>-<method_id_zero_padded>-<method_name>.bin`, CAS-bump `read_idx` to W. Drains tombstoned rings completely before unlinking.

Backpressure on the writer side is non-blocking: ring wrap overwrites oldest samples; `wrap_count` increments; flusher logs once per minute per method.

### Offline tool

```
$ tools/tidesdb-perf-analyze <output_dir>/*.bin \
    --meta <output_dir>/<pid>-meta.json \
    --output report.md

$ tools/tidesdb-perf-analyze --compare \
    mysql_capture/    mariadb_capture/ \
    --output diff.md
```

Reads `.bin` files. For each method-id: total calls, total ns, mean, median, p95, p99, max. Emits a markdown table + CSV. `--compare` produces a side-by-side `(MySQL, MariaDB, Δ)` table sorted by Δ.

### New sysvars

| Name | Type | Default | Lifetime | Behaviour |
|---|---|---|---|---|
| `tidesdb_perf_capture` | BOOL | `OFF` | Runtime | Master switch: capture on/off |
| `tidesdb_perf_output_dir` | STR | `/var/lib/mysql/tidesdb-perf` | Runtime | Where the flusher writes |
| `tidesdb_perf_ring_capacity_pow2` | INT | `16` (= 64K samples = 1.5 MiB) | Server-start | Ring size per thread |
| `tidesdb_perf_flush_interval_ms` | INT | `1000` | Runtime | Flusher tick interval |

### File-size targets

| File | Lines target |
|---|---|
| `plugin/tidesdb_perf_ring.h` | ~120 |
| `plugin/tidesdb_perf_ring.cc` | ~250 (flusher + alloc + tombstone + meta.json write) |
| `plugin/tidesdb_perf_scope.h` | ~50 |
| `plugin/ha_tidesdb.cc` | +~32 instrumentation lines |
| `plugin/tidesdb_inplace_alter.cc` | +~4 instrumentation lines |
| `plugin/tidesdb_fts.cc` | +~2 instrumentation lines |
| `tools/tidesdb-perf-analyze` | ~150 (Python) |
| TideSQL vendored equivalents | ~400 lines mirrored across 3 files |

## 6. Data flow

### Per call (the hot path)

```
ha_tidesdb::write_row(buf)
  ├─ TDB_PERF_SCOPE(write_row)    ← ctor: m_enter = rdtsc()
  ├─ pk_from_record(...)
  │   └─ TDB_PERF_SCOPE(pk_from_record)   [nested sample]
  ├─ serialize_row(...)
  │   └─ TDB_PERF_SCOPE(serialize_row)    [nested sample]
  ├─ tidesdb_txn_put(...)         ← engine work, not instrumented
  └─ dtor: rdtsc + push to ring    [parent sample]
```

Each nested scope is its own sample. The offline tool reconstructs the call tree from `enter_tsc` ordering within a thread — nested intervals are fully contained in their parents, so "time exclusive of instrumented children" is recoverable by subtraction.

### Per second (the flusher)

```
flusher thread tick (every flush_interval_ms)
  for each ring in g_rings_head:
    W = ring->write_idx.load()
    R = ring->read_idx.load()
    if W == R: continue
    snapshot_size = min(W - R, kCapacity)
    bucket samples by method_id
    for each method_id with samples: pwritev to per-method file
    ring->read_idx.store(W)
  sleep_until(next tick)
```

### File layout

```
<output_dir>/
├── 12345-01-write_row.bin
├── 12345-02-update_row.bin
├── ...
├── 12345-32-decrypt_row.bin
├── 12345-calibration.bin
└── 12345-meta.json
```

`meta.json` contains `tsc_ghz`, `kCapacity`, `kMethodCount`, `format_version: 1`. The offline tool uses `tsc_ghz` to convert cycle deltas to nanoseconds.

### Benchmark run cycle

```
1. Start mysqld with --tidesdb_perf_capture=ON
2. Run HammerDB workload (WARE=10, RUNVU=8, RAMP=1m, DUR=3m)
3. Stop mysqld (graceful — flusher gets final tick)
4. Copy <output_dir> out via docker cp
5. tools/tidesdb-perf-analyze captures/*.bin --meta captures/*-meta.json --output mysql.md
6. Repeat 1-5 for MariaDB+TidesDB SUT → mariadb.md
7. tools/tidesdb-perf-analyze --compare captures-mysql/ captures-mariadb/ --output diff.md
```

### Reporting

Markdown table per SUT:

```
| method                          | calls   | total_ms | mean_us | p50_us | p95_us | p99_us | max_us |
|---------------------------------|---------|----------|---------|--------|--------|--------|--------|
| write_row                       | 1234567 | 8950     | 7.25    | 4.1    | 22.3   | 89.7   | 4200   |
| ...                             |         |          |         |        |        |        |        |
```

Diff table (sorted by Δ descending):

```
| method                          | MySQL ns/call | MariaDB ns/call | Δ ns   | calls   | Δ total (ms) |
|---------------------------------|---------------|-----------------|--------|---------|--------------|
| serialize_row                   | 830           | 410             | +420   | 1234567 | +518         |
| key_copy_to_comparable          | 1260          | 980             | +280   | 9876543 | +2765        |
| ...                             |               |                 |        |         |              |
```

The biggest values of `Δ total (ms)` are our optimisation targets for the next release.

## 7. Error handling

### Failure modes

| Stage | Failure | Recovery | Visible outcome |
|---|---|---|---|
| `PerfScope` ctor | None possible (single intrinsic) | n/a | n/a |
| `PerfScope` dtor (capture OFF) | None — short-circuit branch | n/a | ~3 ns branch cost |
| `PerfScope` dtor first call on thread | Ring alloc fails (1.5 MiB) | `t_ring = sentinel(0x1)`; subsequent calls short-circuit; log once | Thread loses data; others continue |
| `PerfScope` dtor, ring full | Writer outpaces flusher | Oldest samples overwritten; `wrap_count` increments | Lossy; flusher logs once per minute |
| Flusher tick | `pwritev` fails (ENOSPC / EIO) | Mark that method's fd as broken; log once; keep flushing others | One method's data missing |
| Flusher thread crash | Assert / bad data | Thread exits; capture continues to RAM; never lands on disk | Data lost after crash; mysqld unaffected |
| Sysvar ON→OFF | Capture stops | Rings persist; flusher idles after 2 s of no samples | Memory held until thread exit |
| Sysvar OFF→ON | First-call-on-thread allocates ring | One-time ~10 µs spike | Acceptable; documented |
| Server crash mid-capture | In-RAM samples lost | Per-pid file naming preserves earlier runs | Lossy; previous run intact |
| File system full | ENOSPC | Stop writing; log ERROR once; rings wrap | mysqld unaffected |
| TSC non-invariant | Crash on `rdtsc` migration | Fall back to `CLOCK_MONOTONIC_RAW`; INFO once | ~3× slower per sample; still usable |
| Offline tool: corrupt binary | Wrong header / partial write | Skip file with warning; `[no data]` row | Partial report |
| Format version mismatch | Future `Sample` struct change | `format_version: 1` in meta.json; tool refuses unknown versions | Forces tool + binary versions to track together |

### Calibration

Before the first per-method file is written, the flusher writes a calibration sample: 10,000 back-to-back empty `PerfScope` constructions on one thread, captured into `calibration.bin`. The offline tool reads this first and computes the per-scope overhead floor (median TSC delta). All real per-call numbers are reported as-is with a footnote stating the floor — so we never claim a method takes 50 ns when 30 ns is the instrument itself.

### Edge cases

1. **Threads created before plugin init**: connection-handler threads exist before the plugin loads. Their first `TDB_PERF_SCOPE` allocates and self-registers via `g_rings_head.compare_exchange` (race-free).
2. **Plugin unload**: `g_capture_active = false` is the first action; subsequent scope calls short-circuit without dereferencing the dangling TLS ring pointer.
3. **Per-method fds**: 32 + 1 (meta + calibration) = 33 fds open the whole run. Reserve 64 at `tidesdb_perf_init` to fail-fast if `nofile` is too low.
4. **Clock skew across cores**: rely on invariant TSC (Nehalem+); fall back to `CLOCK_MONOTONIC_RAW` if `cpuid` reports non-invariant. Flusher pinned to one core.

### Logging discipline

All log lines prefixed `[TIDESDB-PERF]`. Per-thread and per-method failures: WARNING, at-most-once per minute via `std::atomic<int64_t> last_log_ts`. Flusher crash: ERROR. Calibration fallback: INFO once.

## 8. Testing

### Unit tests

| Test | What it verifies |
|---|---|
| `PerfRing.PushReadRoundTrip` | Push N samples, read back, same content + order |
| `PerfRing.WrapBehaviour` | Push kCapacity + 100 samples; first 100 lost, rest intact, `wrap_count == 1` |
| `PerfRing.ConcurrentPushSingleReader` | 4 producers push 100k each; flusher drains; TSAN-clean; exact total observed |
| `PerfScope.NoCaptureZeroAllocation` | 1M scopes with capture OFF; zero ring allocations (mocked allocator) |
| `PerfScope.NestedScopes` | Outer + inner; inner.enter > outer.enter and inner.exit < outer.exit |
| `PerfRing.TombstoneDrained` | Thread exits; flusher drains and unlinks; ring memory freed |
| `OfflineTool.HistogramFromSamples` | Python: synthetic samples → expected p50/p95/p99 |
| `OfflineTool.MultiFileMerge` | Two `.bin` files merge; totals add correctly |

### MTR tests

| Test | What it verifies |
|---|---|
| `tidesdb_perf_sysvar_basic` | Sysvars accept declared types/ranges; SET GLOBAL persists; SHOW VARIABLES surfaces them |
| `tidesdb_perf_off_no_files` | Default OFF: 100 INSERTs leave the output dir empty |
| `tidesdb_perf_on_files_appear` | ON: 100 INSERTs produce non-empty `<pid>-01-write_row.bin` |
| `tidesdb_perf_capture_off_stops_growth` | ON→OFF: files stop growing within 2 s |
| `tidesdb_perf_meta_json_valid` | `<pid>-meta.json` parses; `tsc_ghz ∈ [0.5, 6.0]`; constants match |
| `tidesdb_perf_overflow_logged` | Tight loop overruns small ring; expect WARNING in error log |
| `tidesdb_perf_no_dml_regression` | ON: 1000-INSERT loop overhead ≤ 5% vs OFF baseline |

### Integration

`bench/perf/run-perf-capture.sh`:

1. Start mysqld in a fresh container with `tidesdb_perf_capture=ON`.
2. Run HammerDB TPC-C `WARE=10, RUNVU=8, RAMP=1m, DUR=3m`.
3. Stop mysqld gracefully.
4. `docker cp` the output dir.
5. Run `tools/tidesdb-perf-analyze` on the captured data.
6. Write markdown report.
7. Repeat for the MariaDB+TidesDB SUT.
8. Emit `diff.md` via `--compare`.

### Acceptance criteria for v0.4.1

1. All 8 unit tests + 7 MTR tests pass.
2. `tidesdb_perf_capture=OFF`: DML throughput within 1% of v0.4.0 baseline.
3. `tidesdb_perf_capture=ON`: calibration floor ≤ 50 ns per scope.
4. Integration harness produces a comparison diff report on both SUTs without manual intervention.
5. Diff report ranks methods by `Δ total (ms)` so the top 3-5 rows are next release's optimisation targets.

## 9. Known limitations + follow-ups

- **Per-CPU-core attribution is not in scope.** We pin the flusher and rely on invariant TSC; we don't track which core each sample came from. If a workload's per-core distribution matters, future work.
- **Engine-internal time is not attributed.** The samples include time spent in TidesDB's C API (e.g. `tidesdb_txn_put`) — that's "engine path" time which is shared between MySQL and MariaDB SUTs. Differential is what we care about.
- **Build variants.** Release builds default to `-DTIDESDB_PERF=0`; perf-image builds default ON. Two image variants to maintain.
- **TideSQL port maintenance.** The 32 method-id enum must stay in sync between TidesDB-MySQL and TidesDB-MariaDB plugins. A header-only copy + diff check in CI is feasible but deferred.

## 10. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Calibration floor is higher than 50 ns on slow hosts | medium | Reports overstate per-method cost | Compare relatives, not absolutes; report floor in every output |
| `rdtsc` non-invariant on the test host | low | Crash or huge per-sample drift | Detect via `cpuid`; fall back to `CLOCK_MONOTONIC_RAW` |
| Ring size too small for high-VU runs | medium | Lossy samples, biased histograms | `wrap_count` surfaces this; bump `ring_capacity_pow2` and rerun |
| File-system fills mid-run | low | Capture stops mid-workload | Per-run output dir; sized for the expected run duration |
| MySQL 9.7 sysvar API differs from what we assumed | low | Plugin won't load with new sysvars | Mirror the existing 4 sysvars added in v0.4.0 (verified working pattern) |
| TideSQL port introduces drift in `Sample` format | medium | Cross-SUT comparison breaks silently | Embed `format_version` in `meta.json`; tool refuses unknown versions |
| Compile-time `-DTIDESDB_PERF=1` adds non-negligible cost even when sysvar OFF | medium | Production builds want OFF default | Release builds compile out the instrumentation entirely; the perf image is a separate Docker tag |

## 11. References

- `docs/code-review-report.md` — M-2, M-5, M-6, M-7, M-8, M-9, L-6, L-7, L-8 findings: candidates for v0.4.2 optimisation work
- `docs/v0.4.0-validation-report.md` — current baseline (1784 NOPM / 20697 TPM at WARE=10)
- `bench/hammerdb/run-hammerdb.sh` — workload harness this builds on
- TideSQL repo: https://github.com/tidesdb/tidesql — MariaDB+TidesDB plugin (port target)

## 12. Implementation sequencing (for the plan)

1. Skeleton: `tidesdb_perf_ring.{h,cc}` + `tidesdb_perf_scope.h`; build flag `-DTIDESDB_PERF=1`; plugin links cleanly with zero instrumentation calls; **no behaviour change**.
2. Sysvars: 4 new declarations + storage cells + registration; sysvars surface via `SHOW VARIABLES`.
3. `Sample` + `MethodId` enum + `TLS_Ring` struct + the gtest cases `PerfRing.PushReadRoundTrip`, `WrapBehaviour`, `TombstoneDrained`.
4. `ring_alloc_for_this_thread` + `g_rings_head` lock-free push + `PerfScope.NoCaptureZeroAllocation` test.
5. `PerfScope` macro implementation + the `NestedScopes` test.
6. Flusher thread: tick, snapshot, bucket-by-method, `pwritev`, `wrap_count` logging.
7. Calibration step + `meta.json` writer.
8. Instrumentation sites: 32 `TDB_PERF_SCOPE(...)` calls across `ha_tidesdb.cc` + `tidesdb_inplace_alter.cc` + `tidesdb_fts.cc`.
9. MTR tests `tidesdb_perf_sysvar_basic`, `_off_no_files`, `_on_files_appear`, `_capture_off_stops_growth`, `_meta_json_valid`, `_overflow_logged`, `_no_dml_regression`.
10. Offline tool `tools/tidesdb-perf-analyze`: read .bin, build DataFrame, emit single-SUT report + `--compare` diff.
11. `bench/perf/run-perf-capture.sh` integration harness.
12. TideSQL port: vendored header drop + 32 instrumentation sites in TideSQL's `ha_tidesdb.cc`; rebuild SUT image as `sut-mariadb-tidesdb:9.3.0-perf`.
13. End-to-end measurement: run the full integration harness; produce `diff.md`; this is the deliverable that feeds the next optimisation release.
14. Validation report (`docs/v0.4.1-validation-report.md`); release `v0.4.1`.
