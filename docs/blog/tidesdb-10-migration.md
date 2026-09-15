# Four bugs that compiled cleanly: migrating a MySQL storage engine to TidesDB 10

We maintain a MySQL 9.7 storage engine plugin on top of TidesDB, an LSM engine.
TidesDB 10 came out. The plugin was on 9.3.2. This is what it took to move, and
what nearly shipped.

The short version: the engine got substantially faster, the migration surfaced
four API changes that the compiler could not see, and the biggest of them was
only caught because we ran a benchmark that the test suite said we didn't need.

## The part that compiles and is still wrong

A major version bump in a C library usually announces itself. Functions
disappear, structs change shape, the build breaks, you go and fix it. Ours did
that too: flipping the engine pin produced 98 compile errors, and working
through them is mechanical.

The dangerous changes are the other kind. Same function name. Same parameter
types. Same return type. Different meaning.

We hit four.

**TTL stopped expiring anything.** In TidesDB 9, `tidesdb_txn_put` took an
absolute expiry timestamp. In 10 it takes a lifetime in seconds and computes
the deadline itself. Both are `time_t`. Passing an absolute timestamp where a
lifetime is expected is not an error — it is a lifetime of however many seconds
have elapsed since 1970, so every row with a TTL was set to expire in about
fifty-six years. Nothing failed. Rows just stopped disappearing.

**Iterator keys started leaking.** TidesDB 9 returned a borrowed pointer:

```c
*key = iter->current->key;      /* nothing to free */
```

TidesDB 10 documents the same function as returning "newly allocated key the
caller frees". Our thirty call sites kept reading the key correctly and leaking
it, once per row, on every index scan. More on this below, because it is the
interesting one.

**A failed commit started aborting the transaction.** Our bulk-insert path
committed every 500 rows and retried on transient errors, which was safe on 9:
the error came back before the transaction was marked aborted, so the batch was
still there. On 10, every failure path past the write phase sets
`TDB_TXN_ABORTED` first. The retry then called commit on a finished
transaction, got `TDB_ERR_INVALID_ARGS`, and reported *that* — so a resource
conflict surfaced to operators as "invalid arguments", naming neither the cause
nor anything useful.

**Column-family names needed a different allocator.** `tidesdb_list_column_
families` documents freeing with `tidesdb_free`; we used libc `free` in ten
places. TidesDB can be built against jemalloc, mimalloc or tcmalloc, which
makes that heap corruption. It is inert today only because our build forces the
system allocator. The rule was written down in a comment twelve lines above one
of the violations.

## The leak

The second one deserves its own section, because of how it presented.

Symptom: HammerDB reports "Lost connection to MySQL server during query". The
server's error log ends at a clean startup line with nothing after it. No
signal, no assertion, no stack. The container exits 137.

Exit 137 is SIGKILL. The container had a 12 GiB memory limit and the server
walked into it: 470 MiB at start, 1.1 GiB after the schema build, 4.2 GiB a
minute into the run, then the cap. An OOM-killed process does not get to write
a log entry, which is why the log just stops.

The part that made this genuinely hard is that nothing reported the memory:

| source | reported |
|---|---|
| `performance_schema`, all events over 100 MB | 131 MB |
| `tidesdb_memtable_bytes` | 92 MB |
| `tidesdb_txn_memory_bytes` | 0 |
| actual RSS | 11.9 GiB |

Twelve gigabytes that no instrumented allocator knew about, because a buffer
the engine has handed to the caller is no longer the engine's to account for.

We went at it the wrong way several times. The first read was "the server
crashed", from client-side evidence alone. Then, looking at an early memory
sample of 470 MiB, "actually it didn't crash, that was a misdiagnosis" — which
was also wrong, because the memory was already climbing and one sample is not a
trend. Three separate mechanisms were proposed and eliminated: the new
two-phase commit path (ruled out by reproducing with the binlog disabled, where
that code never runs), transaction reset, and the statistics calls.

What finally worked was boring: put RSS next to the allocator counters and look
at the gap. That should have been the first measurement, not the fifth.

There is a lesson in the reproducers, too. We wrote standalone C programs to
exercise the engine directly — nine column families, four threads, two and a
half million transactions — and they were all perfectly flat. Of course they
were. We wrote them from the API documentation, so they freed what they were
given. They reproduced *correct* code. A reproducer written from the docs
cannot find a bug that consists of not following them.

## What the tests said

At the point the engine was exhausting 12 GiB on the smallest benchmark profile
that exists:

- MTR suite: 87/87 green
- TidesDB's own test suite: 54/54 green
- Plugin unit tests: green
- Smoke test, persistence test, 37-case plugin suite: all green

Every gate we had said ship it. The leak needed sustained concurrent load to
show up, and no functional test generates that. This is the argument for load
validation as a separate gate rather than a nice-to-have: it is the only thing
that found this.

We also found, while cleaning up, four tests that passed while asserting
nothing. One queried a status variable that had been removed, matched zero
rows, and recorded the empty result as expected output. Another was written to
run against MinIO and its own header said it "still passes using local storage"
without it — so in CI it did plain CRUD and kept passing long after the feature
it was named for had been deleted from the engine. A test that passes
vacuously is worse than a missing one, because it reports coverage you do not
have.

## Numbers

HammerDB TPROC-C, same host, 10 warehouses, 8 virtual users, one-minute ramp
plus three minutes measured. Three runs of each, interleaved rather than
batched so any drift in machine state hits both versions equally.

| | NOPM (3 runs) | median | conflicts logged |
|---|---|---|---|
| v0.4.1 (TidesDB 9.3.2) | 1873, 2073, 1838 | **1873** | 1, 1, 8 |
| v0.5.0 (TidesDB 10.0.1) | 4995, 5037, 5218 | **5037** | 4380, 4458, 4558 |

New orders per minute is TPC-C's headline metric, and the median is up 2.7x.
Every individual pairing of runs falls between 2.4x and 2.8x, so the result is
not resting on a lucky sample — which matters, because an earlier
single-warehouse comparison we ran varied by 35% between repeats of the same
build and would have supported almost any claim we wanted.

One caveat we are not going to bury. HammerDB also reports a raw MySQL
transactions-per-minute figure, and that one moved the other way: 21179 down to
12130 at the median, consistently across all three runs. We do not have a
confident explanation. One candidate is our own doing — the two-phase-commit
work changed how many server-level transactions a statement produces, so that
counter may not be comparable across these two versions the way NOPM is, since
NOPM is counted by the client from completed business transactions. If you
benchmark this yourself you will see both numbers, and we would rather tell you
about the one that looks bad than have you find it.

## The conflict tax

The other column in that table is the interesting one.

TidesDB 10 detects write-write conflicts with a first-committer-wins
reservation table: 2^20 slots indexed by the low bits of a key hash, each
holding a 16-bit fingerprint plus a commit sequence. Two unrelated keys can land
in one slot, and the fingerprint is what tells a genuine same-key writer from
that collision. But it is only consulted when the slot's current occupant can be
retired:

```c
if (cseq > read_base && (TDB_MVCC_RES_FP(cur) == myfp || cseq > min_snapshot))
    return 0;   /* conflict */
```

`min_snapshot` is the oldest snapshot any live transaction holds. Once several
connections are working, that floor sits well behind the newest commits, so most
occupants are not retirable, the `||` short circuits, and the fingerprint never
gets a say. A plain hash collision becomes a refused commit.

Around 4,400 of them in three minutes, reproducibly — 4380, 4458 and 4558
across the three runs, against 1, 1 and 8 on the baseline. They are spread
evenly across all eight connections and sustained for the whole run. Genuine TPC-C
contention would concentrate on the hot per-warehouse rows and skew by
connection; an even spread across every worker is what collisions look like.

Clients that retry absorb it — that run completed and still beat the baseline.
But it is wasted work, and a parallel bulk load fails outright, because a bulk
statement that takes a conflict mid-commit cannot be replayed. A four-loader
TPC-C schema build does not finish. A single-threaded one does.

We carry a patch for one half of this, which we have sent upstream: the
retirement floor also counted the committing transaction itself and read a value
maintained for the compaction garbage-collection floor, whose safe direction is
the opposite. That made a *single* connection abort against itself. The
remaining half needs a design change — a larger table, chained slots, or keeping
enough of the key to verify a collision — and we deliberately have not patched
it. The obvious fix, trusting the fingerprint, is worse: claiming a slot evicts
the record the colliding key's next writer needs, so you would trade a loud
spurious abort for a silent lost update.

## What we would do differently

Audit the contracts, not just the signatures. After finding the first two of
these by accident, we diffed the documentation of all 105 engine functions the
plugin calls, looking specifically for changes in ownership, units, and sentinel
values. It rediscovered both, and found the allocator bug we had not noticed.
An hour of that up front would have been worth more than the day we spent.

It has a limit worth knowing. It could not have found the fourth bug, because
"a failed commit now aborts the transaction" is not in the documentation — it
lives in the implementation's state transitions. Three of the four were found
by running things rather than reading them.

Keep the load test in the gate. It was the only thing that caught the leak, on
a build where every functional test was green.

And when something is using memory nobody can account for, measure RSS against
the allocator counters first. It is the dullest possible step and it would have
saved us most of a day.

---

*v0.5.0 is available as `perconalab/tidesdb-mysql:0.5.0`. It cannot read a
v0.4.x data directory — the on-disk format changed, and the engine refuses to
start rather than opening it as an empty database. The upgrade is a dump and
reload; the known issue above is documented in the repository.*
