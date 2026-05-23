#!/usr/bin/env bash
# ============================================================================
#  TPC-C THROUGHPUT profile -- tuned, DURABILITY OFF.
#
#  *** THIS IS NOT A DURABILITY TEST. ***  It deliberately disables per-commit
#  fsync (sync_mode=NONE) to measure raw storage-engine throughput/latency,
#  NOT crash safety. For the durability gate use recovery-test.sh, which runs
#  the engine in its safe (FULL) mode and SIGKILLs it.
#
#  Ports the tuning intent of tidesdb/hammer's my.cnf.example
#  (https://github.com/tidesdb/hammer/blob/master/my.cnf.example) from MariaDB
#  to MySQL 9.7, scaled DOWN from that file's 128 GB / 32-thread Threadripper
#  to this host. The alignment principle is preserved: the same cache budget,
#  durability-off, compression-off, READ-COMMITTED, binlog/perf_schema off are
#  applied to BOTH engines so a tidesdb-vs-innodb head-to-head is honest.
#
#  Tuning is injected via run-hammerdb.sh's DB_EXTRA_ARGS hook (extra mysqld
#  flags). Every tidesdb_* flag carries the loose_ prefix because the var is
#  unknown during the image's pre-plugin --initialize step.
#
#  Usage:
#    ./run-throughput.sh                    # tidesdb, scaled defaults
#    ENGINE=innodb ./run-throughput.sh      # aligned InnoDB baseline (A/B)
#    WARE=200 RUNVU=24 DUR=10 ./run-throughput.sh
# ============================================================================
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)

ENGINE=${ENGINE:-tidesdb}

# --- profile, scaled to a ~20-core / 62 GB host (env-overridable) ----------
# The reference invocation (tidesdb dev) was warehouses=1000, run-vu=64,
# rampup=7, duration=20 on a much bigger box. Scaled here to fit ~42 GB free.
WARE=${WARE:-100}            # ~10 GB dataset (vs 1000 ~= 100 GB upstream)
BUILDVU=${BUILDVU:-8}
RUNVU=${RUNVU:-16}           # vs 64 upstream
RAMP=${RAMP:-2}              # minutes (vs 7)
DUR=${DUR:-5}                # minutes (vs 20)
CPUS=${CPUS:-12}
MEM=${MEM:-32g}              # host has ~42 GB free; leave headroom

# Memory budget. The reference my.cnf.example ran max_memory_usage=0
# (unbounded) with a 32 GB cache on a 128 GB box. Unbounded is unsafe in a
# memory-capped container: under 16-VU write load the unified memtable's
# immutable-flush queue backs up and total RSS balloons past the cgroup
# limit -> mysqld OOM-killed. So we (a) cap the engine with a FINITE
# max_memory_usage so it applies write backpressure instead of OOMing, and
# (b) size cache + buffer + ceiling to sit well under MEM.
#   engine ceiling + cache  ~=  8 + 4 = 12 GiB, + mysqld/conn overhead,
#   comfortably inside the 32 GiB container.
BLOCK_CACHE=${BLOCK_CACHE:-$((4 * 1024 * 1024 * 1024))}   # 4 GiB
WRITE_BUFFER=${WRITE_BUFFER:-$((256 * 1024 * 1024))}      # 256 MiB
MAX_MEM=${MAX_MEM:-$((8 * 1024 * 1024 * 1024))}           # 8 GiB hard ceiling

# Flags applied to BOTH engines (held aligned across the comparison).
COMMON_ARGS="--skip-log-bin \
  --transaction-isolation=READ-COMMITTED \
  --performance-schema=OFF \
  --table-open-cache=16384 \
  --thread-cache-size=256"

case "$ENGINE" in
  tidesdb)
    # Durability OFF: unified memtable, no per-commit sync, no compression.
    ENGINE_ARGS="--loose_tidesdb_unified_memtable=ON \
      --loose_tidesdb_unified_memtable_write_buffer_size=$WRITE_BUFFER \
      --loose_tidesdb_unified_memtable_sync_mode=NONE \
      --loose_tidesdb_block_cache_size=$BLOCK_CACHE \
      --loose_tidesdb_max_memory_usage=$MAX_MEM \
      --loose_tidesdb_flush_threads=6 \
      --loose_tidesdb_compaction_threads=4 \
      --loose_tidesdb_max_open_sstables=1024 \
      --loose_tidesdb_default_sync_mode=NONE \
      --loose_tidesdb_default_compression=NONE \
      --loose_tidesdb_default_l0_queue_stall_threshold=24 \
      --loose_tidesdb_default_l1_file_count_trigger=4 \
      --loose_tidesdb_skip_unique_check=ON \
      --loose_tidesdb_log_level=NONE"
    ;;
  innodb)
    # Aligned InnoDB: same cache budget, durability off, no doublewrite,
    # uncompressed -- matches the my.cnf.example alignment for a fair A/B.
    ENGINE_ARGS="--innodb-buffer-pool-size=$BLOCK_CACHE \
      --innodb-flush-log-at-trx-commit=0 \
      --innodb-doublewrite=OFF \
      --innodb-flush-method=O_DIRECT_NO_FSYNC"
    ;;
  *)
    echo "[tput] ERROR: unsupported ENGINE=$ENGINE (use tidesdb or innodb)" >&2
    exit 2
    ;;
esac

echo "============================================================"
echo " TPC-C THROUGHPUT profile (DURABILITY OFF -- not a crash test)"
echo " engine=$ENGINE  WARE=$WARE buildvu=$BUILDVU runvu=$RUNVU"
echo " ramp=${RAMP}m dur=${DUR}m caps=${CPUS}cpu/${MEM}"
echo " cache=$((BLOCK_CACHE/1024/1024))MiB write_buffer=$((WRITE_BUFFER/1024/1024))MiB max_mem=$((MAX_MEM/1024/1024))MiB"
echo "============================================================"

export ENGINE WARE BUILDVU RUNVU RAMP DUR CPUS MEM
export DB_EXTRA_ARGS="$COMMON_ARGS $ENGINE_ARGS"

exec "$HERE/run-hammerdb.sh"
