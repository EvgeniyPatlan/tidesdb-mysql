# Why TidesDB plugs into MariaDB but not into MySQL 9.7 (yet)

A plain-language explanation. No code in this doc.

## They look the same. They aren't.

MariaDB started as a fork of MySQL in 2009. For a while the two were almost identical inside. They are not anymore. Today they share a name (`mysqld`), a wire protocol, and most SQL — but the *internal C/C++ API that storage engines plug into* has drifted apart for over a decade. Code written against one tree will not compile against the other without surgery.

This is the same kind of drift you see between, say, Linux and FreeBSD: both are "Unix," both run `bash`, but a kernel module written for one will not load into the other.

## What a "storage engine" actually is

A MySQL/MariaDB storage engine is a shared library (a `.so` file) that the server loads at runtime. The library provides two things:

1. A **handlerton** — a small struct that describes the engine to the server: name, what kind of transactions it supports, callbacks for `COMMIT`/`ROLLBACK`, etc.
2. A **handler** — a C++ class with one method per row-level operation: insert this row, fetch the next row, look up by key, etc.

The server calls the handler, the handler calls TidesDB, TidesDB stores the bytes. That's the whole picture.

The handler/handlerton contract is what differs between MariaDB and MySQL. Everything in this document is about that contract.

## Five concrete things that differ

### 1. The data dictionary

MariaDB stores table metadata (column names, types, indexes) in `.frm` files on disk, one per table. The storage engine doesn't have to know anything about it — the server reads the `.frm`, hands the engine an in-memory table description, and the engine stores rows.

MySQL 8.0 and later removed `.frm` files. All table metadata now lives in **the data dictionary**, which is a set of internal tables stored inside InnoDB. When you `CREATE TABLE … ENGINE=TIDESDB`, MySQL writes the schema into InnoDB-backed system tables, and it expects every storage engine to **co-operate with that process** — the engine must hand back a serialized blob of its own metadata (called SDI, "serialized dictionary information") that the DD layer stores alongside the schema.

A MariaDB engine has nothing to say about this because `.frm` files don't exist on its side. A MySQL engine that doesn't know about the DD will fail at `CREATE TABLE` time. This is the single biggest reason TideSQL won't compile against MySQL.

### 2. Atomic DDL

MariaDB: `CREATE TABLE` and `DROP TABLE` are best-effort. If the server crashes mid-way, you may be left with a `.frm` file but no engine data, or the reverse. You clean it up by hand.

MySQL 8.0+: `CREATE TABLE` and `DROP TABLE` are guaranteed atomic. Either the table fully exists everywhere (DD + engine) or it fully doesn't, even if the server is killed mid-statement. To uphold that promise the engine must:

- declare a flag `HTON_SUPPORTS_ATOMIC_DDL` in its handlerton,
- implement `pre_ddl` / `post_ddl` callbacks the server calls before and after,
- participate in a 2-phase-commit-style protocol with the DD layer.

A MariaDB engine has none of this code because the contract didn't exist. TidesDB, the library, has all the right primitives to implement it (atomic manifest updates, crash-safe column-family create/drop), but the glue code in the plugin doesn't exist.

### 3. The handler virtual functions changed

The handler base class is a C++ class with virtual methods like `write_row`, `update_row`, `delete_row`, `rnd_next`, `index_read`. The *names* are the same in both trees. The *signatures* (argument types, what the caller guarantees, what the callee must guarantee) have diverged.

Examples:

- The way a transaction context is passed in is different. MariaDB and MySQL both have something called a `THD`, but its internal layout, the slots an engine uses to attach its own data, and the locking rules around it are not the same.
- The metadata-locking layer (MDL) MySQL inserted around DDL doesn't exist in MariaDB in the same form.
- The way row data is buffered and how the engine returns "no more rows" differs.

So a `.cc` file from TideSQL would fail to compile against MySQL 9.7's `handler.h` with dozens of "unknown member," "wrong number of arguments," "incompatible type" errors.

### 4. The plugin registration macros and CMake glue differ

Both projects ship a CMake helper called `MYSQL_ADD_PLUGIN`, but it takes different option names and produces slightly different output. The `st_mysql_plugin` struct that registers a plugin in the running server has different fields. The macros for declaring system variables and per-table options (`MYSQL_SYSVAR_*`, `MYSQL_THDVAR_*`) work in both, but with different surrounding requirements.

So even the *build description* (the `CMakeLists.txt`) for the plugin needs rewriting, not just porting.

### 5. The install command is different SQL

A small thing, but visible to users:

- MariaDB: `INSTALL SONAME 'ha_tidesdb';`
- MySQL:   `INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so';`

Same idea, different keyword. Documentation, install scripts, and tests all have to use the right one.

## What is and isn't reusable

| Layer | Reusable from TideSQL? |
|---|---|
| TidesDB library itself (`libtidesdb.a`) | **Yes, fully.** It's just C — both MySQL and MariaDB are happy callers. |
| The mental model: one column family per table, primary key bytes as the TidesDB key, row image as the value | **Yes.** The mapping is the same idea on both sides. |
| `tidesdb_*` system-variable names, per-table options like `COMPRESSION='LZ4'` | **Yes for the names**, no for the registration code. Re-declare them with MySQL's macros so users see the same knobs. |
| Row encoder (turning a MySQL row into bytes) | **Mostly.** If TideSQL's encoder was written cleanly it can be lifted across; if it leans on MariaDB-specific field types it needs editing. |
| `ha_tidesdb.cc` — the handler class | **No.** Has to be rewritten against MySQL's `handler.h`. |
| `tidesdb_handlerton.cc` — the handlerton + plugin registration | **No.** Has to be rewritten, plus add data-dictionary and atomic-DDL hooks. |
| The CMake plugin file | **No.** Use MySQL's `MYSQL_ADD_PLUGIN` shape, modeled on `mysql-server/storage/example/`. |

## So what does it take to make it work on MySQL 9.7?

In one sentence: write a new MySQL plugin (`storage/tidesdb/` inside the MySQL tree) whose handlerton + handler are coded against MySQL 9.7's API, link it to the existing `libtidesdb.a`, and use TideSQL only as a design reference for the user-facing surface (variable names, table options, data layout).

That's the whole project.

## TL;DR

> MariaDB and MySQL share SQL but not the storage-engine API. TideSQL works on MariaDB because TideSQL was *written for* MariaDB's API. The TidesDB library underneath is fine — the engine plugin is what needs to be rewritten.
