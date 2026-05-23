# Running TidesDB as a MySQL 9.7 storage engine: an experimental build you can try today

This is a community tool, not an official release. We built it for
ourselves and figured other people might want the same thing: a way to
run [TidesDB](https://github.com/tidesdb/tidesdb), an LSM-tree
key/value engine, as a storage engine inside MySQL 9.7. You create a
table with `ENGINE=TIDESDB` and from then on it behaves like any other
table. The project is called `tidesdb-mysql`, and the build we're
sharing now is **v0.2.5**.

## Why we made it

TidesDB already had a SQL home. It's called **TideSQL**, and it loads
TidesDB into **MariaDB** as the `ha_tidesdb` engine. That path works
well and it's the supported one. The catch, for us, was that we run
MySQL. So we ported the integration over to MySQL 9.7.

MariaDB and MySQL look like cousins, but they've drifted apart under
the hood. The handler API and the data-dictionary layer are different
enough that this ended up being a rewrite-by-replay rather than a
copy-paste. None of that touches the engine itself: it's still plain
TidesDB **v9.2.0** doing the storage work. Only the SQL server sitting
on top changes. The upshot is that you can now pick where TidesDB
runs, MariaDB or MySQL, and your tables look the same either way.

## What it actually is

`tidesdb-mysql` is a loadable plugin, `ha_tidesdb.so`. The engine gets
built on its own and loaded into the server at runtime, the same shape
as the MariaDB version. It speaks the MySQL handler API and wires MySQL
tables and indexes onto TidesDB column families. After it loads,
`TidesDB` sits right next to `InnoDB` in `SHOW ENGINES` and you choose
it per table.

## Your data survives a crash

Here's the property we cared about more than anything else. If the
server goes down the hard way, a crash, a yanked power cable, a
`kill -9`, then whatever you committed has to be there when it comes
back up. That turned out to be where most of our time went.

Hammering it with write load surfaced four distinct bugs in TidesDB's
write-ahead log and recovery path. Stripped of the jargon:

1. The engine was told to sync every write to disk, but a flipped
   piece of logic meant it never actually synced anything.
2. A few of the spots that opened the write-ahead log ignored the
   sync-mode setting on the way in.
3. At startup the log got truncated before it was replayed, so writes
   that had genuinely been saved were discarded on the way back.
4. Once a table's data spanned more than one on-disk file, a full scan
   after a crash returned only a slice of the rows, because the read
   cursor leapt past the rest of the file.

We've patched all four in the engine that ships inside the v0.2.5
image, and we'll drop those patches once the fixes are upstream. The
defaults lean toward safety now too. Bulk loads, for instance, won't
quietly drop rows anymore when a write hits a transient error. With
that in place we can run a TPC-C-style workload, `docker kill -9` the
container in the middle of writing, restart it on the same volume, and
find every committed row still sitting there. You can reproduce that in
about thirty seconds, and there's a recipe for it further down.

## Getting started

All you need is Docker. Pull the image and start it:

```bash
docker pull evgeniypatlan/test-images:mysql-9.7-tidesdb-v0.2.5

docker run -d --name tidesdb \
  -e MYSQL_ROOT_PASSWORD=secret \
  -p 3306:3306 \
  evgeniypatlan/test-images:mysql-9.7-tidesdb-v0.2.5
```

The plugin is baked into this image and loaded on boot, so there's no
`INSTALL PLUGIN` step to remember. Confirm the engine is live:

```bash
docker exec tidesdb mysql -uroot -psecret \
  -e "SELECT engine, support FROM information_schema.engines WHERE engine='TidesDB';"
# TidesDB | YES
```

Now make a table and treat it like any other:

```sql
CREATE DATABASE shop;
USE shop;

CREATE TABLE products (
  id    INT PRIMARY KEY AUTO_INCREMENT,
  name  VARCHAR(64) NOT NULL,
  price DECIMAL(10,2) NOT NULL,
  KEY idx_price (price)
) ENGINE=TIDESDB;

INSERT INTO products (name, price) VALUES ('Widget', 9.99), ('Gadget', 24.50);
SELECT * FROM products WHERE price < 20;
```

Transactions, secondary indexes, the usual SQL, it all behaves:

```sql
START TRANSACTION;
UPDATE products SET price = price + 1 WHERE name = 'Widget';
COMMIT;
```

Per-table TidesDB options ride along in MySQL's `ENGINE_ATTRIBUTE` JSON
field. MySQL doesn't have MariaDB's `COMPRESSION=...` grammar, so the
options are identical but you write them differently:

```sql
CREATE TABLE events (
  id  BIGINT PRIMARY KEY AUTO_INCREMENT,
  msg TEXT
) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"compression":"ZSTD","bloom_filter":true}';
```

Compression accepts `NONE`, `SNAPPY`, `LZ4`, `ZSTD`, or `LZ4_FAST`.
Server-wide knobs live in system variables such as
`tidesdb_default_compression`, `tidesdb_block_cache_size`,
`tidesdb_compaction_threads`, and `tidesdb_flush_threads`. The full
list is in [`docs/build-and-load.md`](../build-and-load.md).

## Prove the crash recovery yourself

Write a handful of rows, kill the server with no clean shutdown, bring
it back, and count what's left:

```bash
# 1. Write rows inside a transaction and COMMIT.
docker exec -i tidesdb mysql -uroot -psecret <<'SQL'
CREATE DATABASE IF NOT EXISTS t;
CREATE TABLE IF NOT EXISTS t.kv (k INT PRIMARY KEY, v VARCHAR(32)) ENGINE=TIDESDB;
BEGIN;
INSERT INTO t.kv VALUES (1,'a'),(2,'b'),(3,'c'),(4,'d'),(5,'e');
COMMIT;
SELECT COUNT(*) AS before_crash FROM t.kv;   -- 5
SQL

# 2. Hard-kill the server (no graceful shutdown) and restart it.
docker kill -s KILL tidesdb
docker start tidesdb
until docker exec tidesdb mysql -uroot -psecret -e 'SELECT 1' >/dev/null 2>&1; do sleep 2; done

# 3. The committed rows are still there.
docker exec tidesdb mysql -uroot -psecret \
  -e "SELECT COUNT(*) AS after_crash FROM t.kv;"   -- 5
```

`after_crash` should come back equal to `before_crash`.

## A few more things to try

Compression is the one people ask about first, so here's a table that
leans on it. We generate a couple thousand rows of repetitive text,
which is exactly the shape ZSTD likes:

```sql
CREATE TABLE logs (
  id    BIGINT PRIMARY KEY AUTO_INCREMENT,
  level VARCHAR(8) NOT NULL,
  body  TEXT,
  KEY idx_level (level)
) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"compression":"ZSTD","bloom_filter":true}';

INSERT INTO logs (level, body)
SELECT IF(RAND() < 0.2, 'warn', 'info'),
       REPEAT('the quick brown fox jumps over the lazy dog ', 40)
FROM information_schema.columns
LIMIT 2000;

SELECT level, COUNT(*) AS rows FROM logs GROUP BY level;
SELECT id, LEFT(body, 30) AS preview FROM logs WHERE id = 1000;
```

The rows go in compressed and come back out as the original text, so
queries don't change at all. If you want to confirm the option
actually landed on the table rather than being silently dropped, ask
the server what it stored:

```sql
SHOW CREATE TABLE logs\G
-- ENGINE=TIDESDB ... ENGINE_ATTRIBUTE='{"compression":"ZSTD","bloom_filter":true}'
```

The bloom filter from that same attribute is what keeps point lookups
cheap once the data has compacted down into several on-disk files:

```sql
SELECT id, level FROM logs WHERE id = 1500;
```

A JSON column behaves the way you'd expect, including the `->>`
extraction operator:

```sql
CREATE TABLE kv (k VARCHAR(64) PRIMARY KEY, v JSON) ENGINE=TIDESDB;

INSERT INTO kv VALUES
  ('en', JSON_OBJECT('lang','English', 'msg','hello')),
  ('es', JSON_OBJECT('lang','Spanish', 'msg','hola')),
  ('fr', JSON_OBJECT('lang','French',  'msg','bonjour'));

SELECT k, v->>'$.lang' AS language, v->>'$.msg' AS greeting
FROM kv
ORDER BY k;
```

And the secondary index on `products` from earlier is a real index, not
decoration. A range query uses it, and `EXPLAIN` will show `idx_price`
in the `key` column:

```sql
SELECT name, price FROM products WHERE price BETWEEN 5 AND 20 ORDER BY price;

EXPLAIN SELECT name, price FROM products WHERE price BETWEEN 5 AND 20;
```

## What works, and what doesn't yet

Quite a bit works. The common column types are all there, primary keys
single and composite, `AUTO_INCREMENT`, secondary indexes with
index-condition pushdown, `COMMIT`/`ROLLBACK`, `REPLACE` and
`INSERT … ON DUPLICATE KEY UPDATE`, online add/drop index, instant add
column, full-text search, spatial indexes, per-row TTL, per-table
compression and bloom filters, at-rest encryption, and mixed-engine
transactions where a TidesDB table and an InnoDB table share one
`BEGIN … COMMIT`. The functional test suite, which we lifted from
TideSQL and then extended, passes 58 of 58 executed tests.

A few things you should know about before you lean on it:

- Native partitioning and the MySQL 9 vector column type aren't
  implemented. Those two test cases are skipped deliberately.
- Atomic, crash-safe DDL (the data-dictionary integration) is wired up
  but we haven't driven it end-to-end yet. Your data writes are
  crash-safe; schema changes during a crash are next on the list.
- Replication, foreign keys, and nested savepoints aren't in scope at
  the moment.

Treat v0.2.5 as a serious experiment. It's solid enough that committed
data rides through a crash, and it's not something we'd point
production traffic at yet.

## Try it, then tell us

```bash
docker pull evgeniypatlan/test-images:mysql-9.7-tidesdb-v0.2.5
```

That's the whole setup. Spin up a table with `ENGINE=TIDESDB`, run the
crash demo, and point your own SQL at it. The source, the build
scripts, and the engine patches all live in the
[`tidesdb-mysql`](https://github.com/EvgeniyPatlan/tidesdb-mysql)
repository, and the durability fixes are written up in
[`KNOWN-ISSUES.md`](../../KNOWN-ISSUES.md). This is a tool made by users
for users, so if you give it a spin, we'd genuinely like to hear what
held up and what fell over.
