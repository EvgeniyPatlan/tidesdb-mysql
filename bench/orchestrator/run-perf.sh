#!/usr/bin/env bash
# Performance orchestrator. Drives sysbench from one pinned client
# container against both SUTs under identical resource caps, recording
# throughput/latency plus the efficiency metrics that matter for the
# article (CPU-seconds per 1k txn, peak RSS, on-disk size / space-amp).
#
# Fairness: identical --cpus/--memory, identical TidesDB v9.2.0 default
# CF config (engine forced via sysbench --mysql-storage-engine=TidesDB),
# same client, same seeds (sysbench default), warmup excluded, N reps.
#
# errexit OFF in the measured loop by design (we classify failures from
# output), same lesson as the parity harness.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)
. "$BENCH/parity/shim/dialect.sh"           # reuse sut_wait_ready
set +e                                       # dialect.sh sets -e on source;
                                             # this harness classifies its own
                                             # failures and must not abort.
SPEC="${1:-$BENCH/workloads/oltp_smoke.yaml}"
LOCK="$BENCH/images/versions.lock"

val() { grep -E "^$1:" "$2" | head -1 | sed -E "s/^$1:[[:space:]]*//; s/[[:space:]]*(#.*)?$//"; }

CPUS=$(val cpus "$SPEC");           MEM=$(val memory "$SPEC")
TABLES=$(val tables "$SPEC")
# Contention sweep: table_sizes (comma list) is the OCC story for an
# LSM/MVCC engine. Back-compat: fall back to a single table_size.
TSIZES=$(val table_sizes "$SPEC"); [ -z "$TSIZES" ] && TSIZES=$(val table_size "$SPEC")
ENGINE=$(val mysql_storage_engine "$SPEC")
WLS=$(val workloads "$SPEC");       THREADS=$(val threads "$SPEC")
REPS=$(val repetitions "$SPEC");    WARM=$(val warmup_seconds "$SPEC")
RUNS=$(val run_seconds "$SPEC")
DB=$(val bench_db "$SPEC");         BU=$(val bench_user "$SPEC"); BP=$(val bench_pass "$SPEC")
NAME=$(val name "$SPEC")

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$BENCH/results/perf-$TS"; mkdir -p "$OUT/raw"
CSV="$OUT/results.csv"
echo "sut,workload,table_size,threads,rep,tps,qps,p95_ms,cpu_sec,mem_peak_mb,datadir_bytes" >"$CSV"

NET="benchnet-$$"
SB="perfsb-$$"
docker network create "$NET" >/dev/null 2>&1 || true
docker rm -f "$SB" >/dev/null 2>&1 || true
docker run -d --name "$SB" --network "$NET" sut-sysbench:bench >/dev/null

cleanup() {
    docker rm -f "$SB" perf-mysql-$$ perf-mariadb-$$ >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT

sb() { docker exec "$SB" sysbench "$@"; }            # sysbench in client ctr

# sut_alive <server> <ctr>: 0 iff the DB answers SELECT 1 via the same
# client path the cells use (real liveness, not just container status).
sut_alive() {
    if [ "$1" = mysql ]; then
        docker exec "$2" mysql -uroot -N -B -e "SELECT 1" 2>/dev/null | grep -q '^1$'
    else
        docker exec "$2" mariadb --socket=/tmp/mariadb.sock -uroot -N -B \
            -e "SELECT 1" 2>/dev/null | grep -q '^1$'
    fi
}

# preserve_death <server> <ctr> <where>: dump everything needed for a
# post-mortem BEFORE the cleanup trap removes the container. Called once
# when a SUT is detected dead mid-run.
preserve_death() {
    local s="$1" c="$2" where="$3" f="$OUT/raw/${s}.DEATH.txt"
    {
        echo "==== SUT DEATH: $s at $where  $(date -u +%FT%TZ) ===="
        docker inspect "$c" --format \
          'state={{.State.Status}} oom={{.State.OOMKilled}} exit={{.State.ExitCode}} err="{{.State.Error}}" started={{.State.StartedAt}} finished={{.State.FinishedAt}}' 2>&1
        echo "-- cgroup memory.events / peak --"
        docker exec "$c" sh -c 'cat /sys/fs/cgroup/memory.events 2>/dev/null; echo peak=$(cat /sys/fs/cgroup/memory.peak 2>/dev/null)' 2>&1
        echo "-- df / datadir --"
        docker exec "$c" sh -c 'df -h 2>/dev/null|tail -3; du -sh /var/lib/mysql /usr/local/mariadb/data/default 2>/dev/null' 2>&1
        echo "-- host dmesg (oom/segfault) --"
        dmesg 2>/dev/null | grep -iE 'oom|killed process|segfault|mysqld|mariadbd' | tail -15
        echo "-- mysqld/.err tail --"
        docker exec "$c" sh -c 'tail -60 /var/lib/mysql/*.err 2>/dev/null' 2>&1
        echo "-- docker logs tail (200) --"
        docker logs --tail 200 "$c" 2>&1
    } >"$f" 2>&1
    echo "[perf] *** $s DIED at $where -- evidence: $f ***"
}
cg_cpu_usec() { docker exec "$1" sh -c 'cat /sys/fs/cgroup/cpu.stat 2>/dev/null | awk "/^usage_usec/{print \$2}"' 2>/dev/null; }
cg_mem_peak() { docker exec "$1" sh -c 'cat /sys/fs/cgroup/memory.peak 2>/dev/null || cat /sys/fs/cgroup/memory.current 2>/dev/null' 2>/dev/null; }
datadir_bytes() {                                    # <ctr> <server>
    local p; [ "$2" = mysql ] && p=/var/lib/mysql || p=/usr/local/mariadb/data/default
    docker exec "$1" sh -c "du -sb '$p' 2>/dev/null | awk '{print \$1}'" 2>/dev/null
}

provision() {                                        # <server> <ctr>
    local s="$1" c="$2"
    if [ "$s" = mysql ]; then
        docker exec -i "$c" mysql -uroot <<SQL
CREATE DATABASE IF NOT EXISTS $DB;
CREATE USER IF NOT EXISTS '$BU'@'%' IDENTIFIED BY '$BP';
GRANT ALL PRIVILEGES ON *.* TO '$BU'@'%'; FLUSH PRIVILEGES;
SQL
    else
        docker exec -i "$c" mariadb --socket=/tmp/mariadb.sock -uroot <<SQL
CREATE DATABASE IF NOT EXISTS $DB;
CREATE USER IF NOT EXISTS '$BU'@'%' IDENTIFIED BY '$BP';
GRANT ALL PRIVILEGES ON *.* TO '$BU'@'%'; FLUSH PRIVILEGES;
SQL
    fi
}

sbargs() {                                           # <host> <wl> <table_size> -> common args
    local host="$1" wl="$2" tsz="$3"
    # --mysql-ignore-errors: TidesDB is an optimistic-concurrency
    # engine; under write contention it returns transient conflict,
    # surfaced as 1213 (deadlock) / 1180 (commit conflict, MySQL side)
    # / 1205 (lock wait timeout, MariaDB upstream handler maps it
    # differently). A fair write benchmark must tolerate these instead
    # of aborting. Applied to BOTH SUTs identically; the residual
    # conflict rate at a given contention level is itself a
    # characterization, not a failure.
    printf '%s --db-driver=mysql --mysql-host=%s --mysql-port=3306 --mysql-user=%s --mysql-password=%s --mysql-db=%s --tables=%s --table-size=%s --mysql-storage-engine=%s --mysql-ignore-errors=1213,1180,1205,1969' \
        "/usr/share/sysbench/${wl}.lua" "$host" "$BU" "$BP" "$DB" "$TABLES" "$tsz" "$ENGINE"
}

# sysbench pads with variable whitespace and uses "( <num> per sec.)";
# awk on the paren fields is whitespace-robust.
parse_tps()  { awk -F'[()]' '/^[[:space:]]*transactions:/{split($2,a," ");print a[1];exit}'; }
parse_qps()  { awk -F'[()]' '/^[[:space:]]*queries:/{split($2,a," ");print a[1];exit}'; }
parse_p95()  { awk '/95th percentile:/{print $NF;exit}'; }

run_sut() {                                          # <server> <image>
    local s="$1" img="$2"
    local c="perf-${s}-$$" host="perf-${s}-$$"
    echo "[perf] === SUT $s ($img) ==="
    docker rm -f "$c" >/dev/null 2>&1 || true
    local envflag=""
    [ "$s" = mysql ] && envflag="-e MYSQL_ALLOW_EMPTY_PASSWORD=1"
    # Benchmarking server config applied IDENTICALLY to both SUTs
    # (fairness control, disclosed in README §3): stock hardening must
    # not throttle a load generator. The upstream MariaDB image's
    # my.cnf ships max_connect_errors=10 + 3s connect/net timeouts,
    # which permanently host-blocked sysbench mid-sweep; MySQL's image
    # has laxer defaults. Both entrypoints pass extra args straight to
    # the server, so the same flags normalize both.
    local SRVOPTS="--max-connections=512 --max-connect-errors=1000000 --connect-timeout=30 --net-read-timeout=120 --net-write-timeout=120"
    # Optional per-MariaDB statement-time cap. Under extreme write
    # contention upstream tidesql holds row/lock waits ~100s, so a
    # nominal 15s sysbench rep takes minutes of wall time. A server-side
    # max_statement_time kills the hung statement fast (sysbench sees a
    # bounded error instead of a 100s hang). It does NOT change the
    # throughput conclusion (MariaDB write tps under contention is ~0
    # either way) -- it only bounds run time + p95. MySQL never hits
    # this (its conflicts return in ms), so the cap is non-effective
    # there and the existing MySQL data stays comparable; disclosed in
    # FINDINGS. Set via spec key `mariadb_extra_opts`.
    local MOPTS=""
    [ "$s" = mariadb ] && MOPTS=$(val mariadb_extra_opts "$SPEC")
    docker run -d --name "$c" --network "$NET" --cpus "$CPUS" --memory "$MEM" \
        $envflag "$img" $SRVOPTS $MOPTS >/dev/null
    echo "[perf] waiting for $s"
    sut_wait_ready "$s" "$c" 150 || { echo "[perf] $s never ready" >&2; docker rm -f "$c" >/dev/null 2>&1; return 1; }
    provision "$s" "$c" >/dev/null 2>&1

    local wl thr rep
    IFS=',' read -ra WL_ARR <<<"$WLS"
    IFS=',' read -ra TH_ARR <<<"$THREADS"
    IFS=',' read -ra TS_ARR <<<"$TSIZES"
    local wl tsz thr rep
    for wl in "${WL_ARR[@]}"; do
      for tsz in "${TS_ARR[@]}"; do
        echo "[perf] $s/$wl ts=$tsz prepare"
        # shellcheck disable=SC2046
        sb $(sbargs "$host" "$wl" "$tsz") cleanup >/dev/null 2>&1
        sb $(sbargs "$host" "$wl" "$tsz") prepare \
            >"$OUT/raw/${s}.${wl}.ts${tsz}.prepare.out" 2>&1
        if ! sut_alive "$s" "$c"; then
            preserve_death "$s" "$c" "${wl} ts=${tsz} PREPARE"
            echo "[perf] aborting SUT $s (died during prepare)"
            return 1
        fi
        for thr in "${TH_ARR[@]}"; do
            # warmup (discarded)
            sb $(sbargs "$host" "$wl" "$tsz") --threads="$thr" --time="$WARM" run \
                >/dev/null 2>&1
            for rep in $(seq 1 "$REPS"); do
                local c0 c1 cpu_sec memk rawf
                c0=$(cg_cpu_usec "$c")
                rawf="$OUT/raw/${s}.${wl}.ts${tsz}.t${thr}.r${rep}.out"
                sb $(sbargs "$host" "$wl" "$tsz") --threads="$thr" --time="$RUNS" run \
                    >"$rawf" 2>&1
                c1=$(cg_cpu_usec "$c")
                memk=$(cg_mem_peak "$c")
                local tps qps p95 dd
                tps=$(parse_tps <"$rawf"); qps=$(parse_qps <"$rawf"); p95=$(parse_p95 <"$rawf")
                cpu_sec=$(awk -v a="${c0:-0}" -v b="${c1:-0}" 'BEGIN{printf "%.3f",(b-a)/1e6}')
                local memmb; memmb=$(awk -v m="${memk:-0}" 'BEGIN{printf "%.1f",m/1048576}')
                dd=$(datadir_bytes "$c" "$s")
                echo "${s},${wl},${tsz},${thr},${rep},${tps:-NA},${qps:-NA},${p95:-NA},${cpu_sec},${memmb},${dd:-NA}" >>"$CSV"
                printf '[perf] %-9s %-18s ts=%-7s t=%-2s r=%s tps=%-9s p95=%-7s cpu=%ss\n' \
                    "$s" "$wl" "$tsz" "$thr" "$rep" "${tps:-NA}" "${p95:-NA}" "$cpu_sec"

                # In-situ death detection: if this cell produced no TPS
                # AND the SUT no longer answers SELECT 1, the server
                # died in the real sequence that triggers it. Preserve
                # logs NOW (before the EXIT trap nukes the container)
                # and abort THIS SUT -- do not emit dozens of blind NA
                # rows. The other SUT still runs (top-level sequential
                # calls; errexit is off here).
                if [ "${tps:-NA}" = NA ] && ! sut_alive "$s" "$c"; then
                    preserve_death "$s" "$c" "${wl} ts=${tsz} t=${thr} r=${rep}"
                    echo "[perf] aborting SUT $s (remaining cells skipped)"
                    return 1
                fi
            done
        done
      done
    done
    docker rm -f "$c" >/dev/null 2>&1 || true
}

# `suts` spec key (default both) selects which SUTs to run -- lets us
# re-run a single SUT (e.g. MariaDB write workloads only) and merge.
SUTS=$(val suts "$SPEC"); [ -z "$SUTS" ] && SUTS="mysql,mariadb"
case ",$SUTS," in *,mysql,*)   run_sut mysql   "sut-mysql:bench"   ;; esac
case ",$SUTS," in *,mariadb,*) run_sut mariadb "sut-mariadb:bench" ;; esac

cp "$LOCK" "$OUT/versions.lock"
cp "$SPEC" "$OUT/$(basename "$SPEC")"
echo "[perf] results -> $CSV"
wc -l "$CSV"
