# Parity track (built first, per the parity-led plan)

Server-neutral SQL conformance corpus run against **both** SUTs, output
normalized and diffed into a feature matrix.

## Layout (target)

```
parity/
  shim/            per-server dialect mapping (one logical option set ->
                   MySQL ENGINE_ATTRIBUTE JSON vs MariaDB option grammar;
                   INSTALL PLUGIN ... SONAME 'ha_tidesdb.so' vs
                   INSTALL SONAME 'ha_tidesdb'; differing sysvar names)
  cases/           semantic .sql cases (shared body; shim renders the
                   server-specific preamble)
  expected/        per-axis expected semantics (not raw server output --
                   we compare normalized result semantics)
  run-parity.sh    run every case on sut-mysql:bench and sut-mariadb:bench,
                   normalize, classify, emit matrix.md
  matrix.md        GENERATED: supported / partial / wrong-result /
                   unsupported / error, per axis, per SUT
```

## Axes (see ../README.md §4.2 for the full list)

Headline divergence to test explicitly: `ALTER … ADD FULLTEXT` back-
populating existing rows (SUT A fixes this in v0.2.1 incl. the F-1
meta-counter fix; upstream behaviour to be characterized), plus the
drop+re-ADD identical-ranking and abort/retry cases.

## Status

Stub. Cases + shim authored after both SUT images exist (the MySQL-side
corpus is adapted from this repo's mysql-test-suite/ and the 37 hand-
rolled cases; the MariaDB side reuses upstream's mysql-test/suite/tidesdb
where names align).
