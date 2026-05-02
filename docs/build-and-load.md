# TIDES — build and smoke-test runbook

How to build `ha_tidesdb.so` and load it into a freshly-initialized MySQL 9.7 instance. Everything runs in Docker; nothing pollutes the host.

## Quick reference — one-line commands

```bash
./scripts/build-all.sh             # full pipeline: image + tidesdb + plugin (incremental)
./scripts/build-all.sh --clean     # full rebuild from scratch
./scripts/clean.sh                 # wipe all build outputs (handles root-owned files)
./scripts/build-tidesdb.sh         # rebuild just libtidesdb.a (run inside container)
./scripts/build-plugin.sh          # rebuild just the plugin (run inside container)
```

`build-all.sh` and `clean.sh` are run from the host. `build-tidesdb.sh` and `build-plugin.sh` are designed to run inside the `tides-builder` container (they're invoked by `build-all.sh` automatically).

The everything-from-scratch recipe:

```bash
./scripts/clean.sh && ./scripts/build-all.sh
```

The rest of this document covers the manual / step-by-step path that the wrapper scripts automate.

---

## One-time prerequisites

You must be in the `docker` group:
```bash
sudo usermod -aG docker $USER && newgrp docker
docker info >/dev/null && echo OK    # should print OK
```

## One-time: builder image (~5 min, cached afterwards)

```bash
docker build -t tides-builder /home/corvin/TIDES/docker
```

This produces an Ubuntu 24.04 image with all MySQL 9.7 + TidesDB compile dependencies preinstalled. Rebuild only when `docker/Dockerfile` changes.

All `docker run` invocations below pass `--user "$(id -u):$(id -g)"` so build outputs (in `mysql-server/build/`, `tidesdb/build-static/`, `tidesdb-prefix/`) are owned by your host user. Without this flag, files are written as root and you can't `rm` them without sudo or a docker-run cleanup.

## Step 1 — Build TidesDB static archive (~1 min)

```bash
docker run --rm --user "$(id -u):$(id -g)" -v /home/corvin/TIDES:/work tides-builder \
    /work/scripts/build-tidesdb.sh
```

Outputs:
- `tidesdb-prefix/lib/libtidesdb.a` — static, PIC, ~700 KB.
- `tidesdb-prefix/include/tidesdb/*.h` — 19 public headers.

The script forces `BUILD_SHARED_LIBS=OFF` and disables alternate allocators (jemalloc / mimalloc / tcmalloc) — the latter avoids the static-TLS plugin-load failure documented in `tidesql-install-sh-analysis.md`.

## Step 2 — Configure MySQL + build the plugin (~3 min config + 10–30 min first plugin build)

```bash
docker run --rm --user "$(id -u):$(id -g)" -v /home/corvin/TIDES:/work tides-builder \
    /work/scripts/build-plugin.sh
```

What this does:
1. CMake configure of `mysql-server/` with engine-skips (`PLUGIN_ROCKSDB=NO`, `PLUGIN_NDB=NO`, `PLUGIN_FEDERATED=NO`, `PLUGIN_ARCHIVE=NO`, `PLUGIN_BLACKHOLE=NO`, `PLUGIN_EXAMPLE=NO`) and `CMAKE_PREFIX_PATH=$REPO/tidesdb-prefix` so our plugin's `FIND_LIBRARY(tidesdb)` resolves.
2. Build target `tidesdb` only — produces `mysql-server/build/plugin_output_directory/ha_tidesdb.so`.

Subsequent edits to `storage/tidesdb/ha_tidesdb.cc` rebuild in seconds (the convenience libs stay cached). Force a full reconfigure by deleting `mysql-server/build/`.

The full transcript is teed to `build-plugin.log` at the repo root for analysis.

## Step 3 — Smoke test (Phase 1 exit criterion)

You need a `mysqld` binary too — that's a one-time ~30 min full server build:

```bash
docker run --rm -v /home/corvin/TIDES:/work tides-builder \
    bash -c 'cmake --build /work/mysql-server/build --target mysqld -j$(nproc)'
```

Then bootstrap a data directory and start mysqld with the plugin loaded. Note: `--no-defaults` on bootstrap is load-bearing — see the install.sh analysis.

```bash
DD=/tmp/tidesdb-data && rm -rf "$DD" && mkdir -p "$DD"

docker run --rm -v /home/corvin/TIDES:/work tides-builder bash -c "
    /work/mysql-server/build/runtime_output_directory/mysqld \
        --initialize-insecure --no-defaults \
        --basedir=/work/mysql-server/build \
        --datadir=$DD
"

# Start mysqld in background; expose 3307 for the client
docker run -d --name tidesdb-mysqld -v /home/corvin/TIDES:/work -p 3307:3307 tides-builder bash -c "
    /work/mysql-server/build/runtime_output_directory/mysqld --no-defaults \
        --basedir=/work/mysql-server/build \
        --datadir=$DD \
        --plugin-dir=/work/mysql-server/build/plugin_output_directory \
        --port=3307 --socket=/tmp/mysqld.sock
"

# Connect & run the smoke SQL
docker exec -it tidesdb-mysqld /work/mysql-server/build/runtime_output_directory/mysql \
    -S /tmp/mysqld.sock -uroot <<'SQL'
INSTALL PLUGIN tidesdb SONAME 'ha_tidesdb.so';
SHOW ENGINES;                                                      -- expect TIDESDB row
CREATE DATABASE t;
CREATE TABLE t.t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
SQL

docker stop tidesdb-mysqld && docker rm tidesdb-mysqld
```

**Exit criterion for Phase 1 stub**: the `INSTALL PLUGIN`, `SHOW ENGINES`, and `CREATE TABLE` all succeed without errors.

**Exit criterion for Phase 2 (TideSQL port)**: also `INSERT INTO t.t VALUES (1,'a'); SELECT * FROM t.t;` returns the row.

## Inner dev loop (after the first full build)

```bash
# Edit storage/tidesdb/ha_tidesdb.cc on host
vim mysql-server/storage/tidesdb/ha_tidesdb.cc

# Rebuild only the plugin (~30s typical)
docker run --rm -v /home/corvin/TIDES:/work tides-builder \
    bash -c 'cmake --build /work/mysql-server/build --target tidesdb -j$(nproc)'

# Restart mysqld + reload plugin (or just UNINSTALL/INSTALL if mysqld is still running)
```

## Troubleshooting

- **"TidesDB: library not found"** during configure → `tidesdb-prefix/` is missing. Re-run `build-tidesdb.sh`.
- **"cannot allocate memory in static TLS block"** on plugin load → TidesDB was built with an alternate allocator. Confirm `build-tidesdb.sh` ran with the system allocator (the script forces this; only an issue if someone edited it).
- **"Plugin uses unsupported protocol version X"** → trying to load a MariaDB-built `.so` into MySQL. See `mariadb-vs-mysql.md`.
- **Permission errors writing into `mysql-server/build/`** → Docker writes as root. Either `sudo chown -R $USER:$USER mysql-server/build` once, or add `--user "$(id -u):$(id -g)"` to the `docker run` invocations.
