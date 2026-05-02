# What TideSQL's `install.sh` actually does — and what changes for MySQL

Source: <https://github.com/tidesdb/tidesql/blob/master/install.sh> (~48 KB) plus `tidesdb/CMakeLists.txt` in the same repo.

Goal of this doc: confirm whether our MySQL plan is missing anything that TideSQL gets right, and capture the operational gotchas worth borrowing.

## What `install.sh` actually does (in order)

1. **Parse CLI flags.** Defaults: latest TidesDB tag, latest MariaDB tag, `--allocator system`, no `--pgo`, no `--s3`, no engine skips. Notable flags: `--tidesdb-version`, `--mariadb-version`, `--skip-engines <csv>`, `--allocator {system|jemalloc|mimalloc|tcmalloc}`, `--mariadb-prefix`, `--pgo`, `--s3`, `--jobs`.
2. **Clone the two upstream trees** (shallow, version-pinned):
   ```
   git clone --depth 1 --branch ${TIDESDB_VERSION}  https://github.com/tidesdb/tidesdb.git    ${tidesdb_src}
   git clone --depth 1 --branch ${MARIADB_VERSION}  https://github.com/MariaDB/server.git    ${mariadb_src}
   ```
3. **Build & install TidesDB** to `${TIDESDB_PREFIX}` (default `/usr/local`). Allocator choice picks one of `-DTIDESDB_WITH_JEMALLOC=ON` / `…TCMALLOC` / `…MIMALLOC`. `--s3` adds `-DTIDESDB_WITH_S3=ON`.
4. **Drop the plugin into the MariaDB tree** by file copy — *no patches to MariaDB itself*:
   ```
   cp -r ${SCRIPT_DIR}/tidesdb            ${mariadb_src}/storage/
   cp -r ${SCRIPT_DIR}/mysql-test/suite/tidesdb  ${mariadb_src}/mysql-test/suite/
   ```
   That's it. No `sed`. No edits to `sql/handler.h`, `sql/handler.cc`, `sql/sql_yacc.yy`, `cmake/plugin.cmake`, or anything in the server tree.
5. **Configure MariaDB CMake** with:
   ```
   -DCMAKE_BUILD_TYPE=RelWithDebInfo
   -DCMAKE_INSTALL_PREFIX=${MARIADB_PREFIX}
   -DWITH_UNIT_TESTS=OFF
   -DWITH_MARIABACKUP=ON
   -DCMAKE_PREFIX_PATH=${TIDESDB_PREFIX}
   # Plus -DPLUGIN_<NAME>=NO for each engine in --skip-engines
   ```
   `TIDESDB_ROOT` env var is exported so the plugin's `FIND_LIBRARY` / `FIND_PATH` can locate the installed TidesDB.
6. **Build and install MariaDB.**
7. **Initialize the data directory** with `mariadb-install-db --no-defaults --basedir=… --datadir=… --skip-test-db`. The `--no-defaults` flag is load-bearing — see gotcha #1 below.
8. **Generate `my.cnf`** containing `plugin_load_add = ha_tidesdb.so`, `plugin_maturity = gamma`, and the `tidesdb_*` system-variable defaults. On Unix with a non-system allocator: also write `[mysqld_safe] malloc-lib = /path/to/lib<allocator>.so.2`.
9. **Run MTR** if `--pgo` is set (training run for profile-guided rebuild) or if explicitly requested.

## TideSQL's plugin CMakeLists in full

This is the single most important file in the whole TideSQL tree for our purposes. Stripped of comments and platform conditionals, it boils down to:

```cmake
FIND_LIBRARY(TIDESDB_LIB    NAMES tidesdb        HINTS ENV TIDESDB_ROOT  PATHS /usr/local/lib /usr/lib …)
FIND_PATH   (TIDESDB_INCLUDE_DIR NAMES tidesdb/db.h HINTS ENV TIDESDB_ROOT  PATHS /usr/local/include …)
FIND_LIBRARY(ZSTD_LIBRARY   NAMES zstd zstd_static …)
FIND_LIBRARY(LZ4_LIBRARY    NAMES lz4 …)
FIND_LIBRARY(SNAPPY_LIBRARY NAMES snappy …)

INCLUDE_DIRECTORIES(${TIDESDB_INCLUDE_DIR})
SET(TIDESDB_LIBS ${TIDESDB_LIB} ${ZSTD_LIBRARY} ${LZ4_LIBRARY} ${SNAPPY_LIBRARY})

MYSQL_ADD_PLUGIN(tidesdb ha_tidesdb.cc
                 STORAGE_ENGINE
                 MODULE_ONLY
                 LINK_LIBRARIES ${TIDESDB_LIBS})
```

That is **exactly the shape we planned for MySQL** — same macro, same `STORAGE_ENGINE MODULE_ONLY` flags, same external-link-against-installed-TidesDB pattern.

There is also an optional `TIDESDB_WITH_ASAN`/`TIDESDB_WITH_UBSAN` block that adds `-fsanitize=address,undefined` to the plugin target only — useful for local dev without rebuilding the whole server with sanitizers.

## Are we missing anything? Honest answer.

### Structurally — no.
- `MYSQL_ADD_PLUGIN(... STORAGE_ENGINE MODULE_ONLY)` ✓ (our plan)
- Plugin lives at `storage/tidesdb/` ✓
- Link to TidesDB via FIND_LIBRARY ✓
- Use `MYSQL_SYSVAR_*` macros for the `tidesdb_*` knobs ✓
- Per-table options via `ha_create_table_option` (not visible in install.sh, but inferable from the my.cnf reference to `tidesdb_default_compression`)

The `MYSQL_ADD_PLUGIN` macro exists in MySQL too (it's in `mysql-server/cmake/plugin.cmake`). The `STORAGE_ENGINE MODULE_ONLY` shape compiles in both trees. We can lift the plugin's `CMakeLists.txt` essentially verbatim.

### Implementation — yes, but it's not "missing," it's "the actual project."
TideSQL has a 386 KB `ha_tidesdb.cc` and a 41 KB `ha_tidesdb.h`. That's a *real, complete* MariaDB engine — not a stub. Reading it is the single biggest accelerator we have:
- Row-encoder logic for every MySQL field type.
- Per-table option parsing (`COMPRESSION`, `BLOOM_FILTER`).
- All `tidesdb_*` system-variable declarations.
- Transaction integration shape.
- Index encoding scheme.

For Phase 2 onward we should pull `ha_tidesdb.h` + `ha_tidesdb.cc` from TideSQL into a *reference* directory (not into the build), read them carefully, and port piece-by-piece into our `mysql-server/storage/tidesdb/ha_tidesdb.cc`, fixing each MariaDB→MySQL API divergence as the compiler points to it. That is *much* faster than greenfield.

### Operational gotchas worth incorporating into our scripts.

These are real production lessons baked into install.sh that we'd otherwise rediscover the hard way:

1. **Bootstrap with `--no-defaults`.** When initializing the data directory the very first time, don't read `my.cnf`. If `plugin_load_add = ha_tidesdb.so` is already in `my.cnf`, the bootstrap mariadbd will dlopen the plugin during `--initialize`, which (a) is unnecessary and (b) drags in the TidesDB allocator before any TLS slots exist, causing the "cannot allocate memory in static TLS block" failure. **MySQL equivalent**: `mysqld --initialize-insecure --no-defaults --basedir=… --datadir=…`.
2. **Allocator + plugin = TLS landmine.** If TidesDB is built with `-DTIDESDB_WITH_JEMALLOC=ON` / `MIMALLOC` / `TCMALLOC`, the resulting `libtidesdb.so` has a `DT_NEEDED` on the allocator. Loading the plugin late (via `dlopen`) into a server that already started up fails with the static-TLS error. TideSQL's two workarounds: either preload the allocator via `mysqld_safe`'s `malloc-lib`, or build TidesDB with the system allocator. **For our Phase 1 Docker build**: ship TidesDB with the system allocator. Skip the alternate-allocator question entirely. We can revisit in Phase 4.
3. **Engine-skip list cuts MySQL build time massively.** TideSQL passes `-DPLUGIN_<NAME>=NO` for each engine in `--skip-engines`. Same flag works in MySQL. Realistic skip list for our Docker build script: `ROCKSDB`, `NDB`, `FEDERATED`, `ARCHIVE`, `BLACKHOLE`, `EXAMPLE` (we don't need it after the rename). `INNOBASE` cannot be skipped (it backs the data dictionary).
4. **`CMAKE_PREFIX_PATH=<tidesdb-prefix>` + `TIDESDB_ROOT=<tidesdb-prefix>` env var** is how the plugin finds TidesDB. Our build script needs to either install TidesDB to a known prefix first, or point both at TidesDB's *build tree* via the same vars (TideSQL's CMakeLists already honors `ENV TIDESDB_ROOT` in HINTS).
5. **Plugin-only sanitizers.** `-DTIDESDB_WITH_ASAN=ON` patterns the sanitizer flags onto the plugin target only, not the whole server. Useful when chasing memory bugs in our handler without rebuilding mysqld.

### What is *not* portable from install.sh:
- `mariadb-install-db` → MySQL has `mysqld --initialize`/`--initialize-insecure`. Different command, different bootstrap flow.
- `mariadbd-safe` and `[mysqld_safe] malloc-lib = …` → MySQL deprecated `mysqld_safe` years ago in favor of systemd. The "preload allocator" trick has to use `LD_PRELOAD` in the systemd unit, or just avoid the alternate allocator.
- `plugin_maturity = gamma` → MariaDB-only config knob, ignored by MySQL.
- `INSTALL SONAME 'ha_tidesdb';` → MySQL syntax is `INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so';`.
- The `Makefile`, `Makefile.am`, `Makefile.in`, and `plug.in` files in TideSQL's `tidesdb/` directory are autotools artifacts. **Do not copy them.** MySQL is CMake-only.

## Concrete adjustments to our Phase 1 plan

1. **Our plugin `CMakeLists.txt` should be lifted from TideSQL's** with one edit: nothing. It's already MySQL-compatible because `MYSQL_ADD_PLUGIN` exists in both trees with compatible flags. Drop it in unchanged.
2. **Build script (`scripts/build-plugin.sh`) should**:
   - First build TidesDB to a known prefix (e.g. `/work/tidesdb-prefix`) with `-DBUILD_SHARED_LIBS=OFF` (we still want the static archive — TideSQL went .so but `.a` keeps `ha_tidesdb.so` self-contained for distribution; this is the one place we depart from TideSQL on purpose).
   - Then run MySQL CMake configure with `-DPLUGIN_ROCKSDB=NO -DPLUGIN_NDB=NO -DPLUGIN_FEDERATED=NO -DPLUGIN_ARCHIVE=NO -DPLUGIN_BLACKHOLE=NO -DPLUGIN_EXAMPLE=NO -DWITH_UNIT_TESTS=OFF -DDOWNLOAD_BOOST=1 -DWITH_BOOST=/work/boost -DTIDESDB_ROOT=/work/tidesdb-prefix -DCMAKE_PREFIX_PATH=/work/tidesdb-prefix`.
   - Build *only* the plugin target: `cmake --build build --target tidesdb` (note: MySQL's `MYSQL_ADD_PLUGIN(tidesdb …)` produces a target named `tidesdb`, not `ha_tidesdb`).
3. **Smoke-test step** uses MySQL's bootstrap, not MariaDB's:
   ```
   mysqld --initialize-insecure --no-defaults --basedir=… --datadir=…
   mysqld --no-defaults --basedir=… --datadir=… --plugin-dir=… --port=…
   mysql … -e "INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so'; SHOW ENGINES;"
   ```

## What I would *do* with TideSQL's source

For Phase 1: **ignore** `ha_tidesdb.cc` / `ha_tidesdb.h` entirely. The stub we're building from `storage/example/` is enough to prove the load path.

For Phase 2: **clone TideSQL into a `vendor/tidesql-reference/`** (gitignored or marked read-only). When implementing each handler method, open both files side-by-side: TideSQL's working MariaDB version on the left, our empty MySQL stub on the right, and port one method at a time. The compiler will tell you what doesn't fit.

For Phase 3: lift `mysql-test/suite/tidesdb/` from TideSQL into `mysql-server/mysql-test/suite/tidesdb/` and see how many tests just pass. Most `.test` files use vanilla SQL and should — the `.result` golden files may need regeneration.
