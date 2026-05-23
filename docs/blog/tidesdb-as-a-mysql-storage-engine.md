# I made TidesDB work as a MySQL 9.7 storage engine

This is not an official release. It is a tool I built as a user, and I
want to share it with other users.

For some time, the main way to use TidesDB with SQL was **TideSQL** —
TidesDB loaded into **MariaDB** as the `ha_tidesdb` storage engine.
That works well and is the supported path.

But I use MySQL, and I wanted to use TidesDB there too. So I worked on
an integration that lets **TidesDB run as a storage engine inside
MySQL 9.7**. I call it `tidesdb-mysql`. The engine under the hood is
the same TidesDB (**v9.2.0**) — only the SQL server on top is
different. I tagged the current state as `v0.2.1` so it is easy to
pull and try, but please see it as a community tool, not an official
product.

I am sharing it because maybe other users want the same thing. If you
use MySQL, this gives you a way to try TidesDB without changing how you
think about your data.

## What it is

`tidesdb-mysql` is a loadable plugin (`ha_tidesdb.so`). It is the same
plugin shape as the MariaDB version: the engine is built separately and
loaded into the server at runtime. It implements the MySQL handler API
and connects TidesDB column families to MySQL tables and indexes.

So now you can choose: TidesDB on MariaDB, or TidesDB on MySQL. The
data model stays the same on both.

## How to run it

I published a ready Docker image, so you only need two commands:

```bash
docker pull evgeniypatlan/test-images:mysql-9.7-tidesdb-v0.2.1

docker run --rm -d --name tidesdb \
  -e MYSQL_ALLOW_EMPTY_PASSWORD=1 -p 3306:3306 \
  evgeniypatlan/test-images:mysql-9.7-tidesdb-v0.2.1
```

The plugin is already loaded in this image. You can check the engine
is there:

```sql
SELECT engine, support
FROM information_schema.engines
WHERE engine = 'TidesDB';
-- TidesDB | YES
```

## Examples

Here are a few simple examples of how to use it.

### 1. Create a table and add data

You create a table on TidesDB with `ENGINE=TIDESDB`. Everything else
is normal SQL:

```sql
CREATE TABLE products (
  id    INT PRIMARY KEY AUTO_INCREMENT,
  name  VARCHAR(64) NOT NULL,
  price DECIMAL(10,2) NOT NULL,
  body  TEXT,
  KEY idx_price (price)
) ENGINE=TIDESDB;

INSERT INTO products (name, price, body) VALUES
  ('Widget', 9.99,  'a small reliable widget'),
  ('Gadget', 24.50, 'a deluxe gadget');

SELECT id, name, price FROM products ORDER BY id;
```

### 2. Use a secondary index

The `idx_price` index above works like a normal index. A range query
uses it:

```sql
SELECT name, price
FROM products
WHERE price BETWEEN 5 AND 20
ORDER BY price;
```

### 3. Transactions

Normal transaction commands work:

```sql
START TRANSACTION;
INSERT INTO products (name, price) VALUES ('Gizmo', 4.25);
ROLLBACK;          -- the row is not saved

START TRANSACTION;
UPDATE products SET price = price + 1 WHERE name = 'Widget';
COMMIT;            -- the change is saved
```

### 4. Set TidesDB options per table

MySQL does not have the MariaDB option grammar like
`COMPRESSION='LZ4' BLOOM_FILTER=1`. Instead, MySQL uses a JSON field
called `ENGINE_ATTRIBUTE`. The options are the same, only the syntax is
different:

```sql
CREATE TABLE events (
  id  BIGINT PRIMARY KEY AUTO_INCREMENT,
  msg TEXT
) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"compression":"LZ4","bloom_filter":true}';
```

### 5. Full-text search

You can create a FULLTEXT index and search text:

```sql
CREATE TABLE docs (
  id   INT PRIMARY KEY AUTO_INCREMENT,
  body TEXT,
  FULLTEXT KEY ft (body)
) ENGINE=TIDESDB;

INSERT INTO docs (body) VALUES
  ('the quick brown fox'),
  ('a slow brown dog'),
  ('quick and quiet');

SELECT id, body
FROM docs
WHERE MATCH(body) AGAINST('quick')
ORDER BY MATCH(body) AGAINST('quick') DESC;
```

You can also add a FULLTEXT index later, after the table already has
rows:

```sql
ALTER TABLE products ADD FULLTEXT INDEX ft_body (body);

SELECT id, name
FROM products
WHERE MATCH(body) AGAINST('widget');
```

### 6. Online schema changes

You can add or drop a secondary index without a full table rebuild:

```sql
ALTER TABLE products ADD INDEX idx_name (name), ALGORITHM=INPLACE;
ALTER TABLE products DROP INDEX idx_name, ALGORITHM=INPLACE;
```

## Is it consistent with the MariaDB version?

This was important to me, because the same engine runs under two
different servers. So I made a small test set of normal SQL cases and
ran the same cases on both: TidesDB on MySQL and TidesDB on MariaDB.
Both use the same TidesDB v9.2.0.

The result is good: the behaviour is consistent. Here is a short
comparison:

| SQL feature | TidesDB on MySQL | TidesDB on MariaDB |
|---|---|---|
| Basic CRUD (INSERT / UPDATE / DELETE / SELECT) | same result | same result |
| Secondary index range scan | same result | same result |
| Transactions (COMMIT / ROLLBACK) | same result | same result |
| Online DDL (add / drop index, instant add column) | same result | same result |
| Full-text query (NATURAL / BOOLEAN mode) | same result | same result |
| `REPLACE` / `INSERT … ON DUPLICATE KEY UPDATE` | same result | same result |
| Per-table TidesDB options | same result | same result |

So if you already model your data for TidesDB on MariaDB, the same SQL
works the same way on MySQL. The only thing to remember is the syntax
difference shown above: per-table options use `ENGINE_ATTRIBUTE` JSON
on MySQL instead of the MariaDB option grammar — the options and the
result are the same, only the way you write them is different.

## Try it

`tidesdb-mysql` v0.2.1 is available as a Docker image:

```bash
docker pull evgeniypatlan/test-images:mysql-9.7-tidesdb-v0.2.1
```

This is a community tool, made by a user for other users. It is not
official and it is still young, so feedback is very welcome. If you use
MySQL and want to try TidesDB, pull the image, create a table with
`ENGINE=TIDESDB`, and use it like any other table. The source code is
in the `tidesdb-mysql` repository. If you try it, please tell me what
works and what does not.
