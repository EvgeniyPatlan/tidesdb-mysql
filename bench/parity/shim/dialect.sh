#!/usr/bin/env bash
# Per-server dialect shim + client transport for the parity harness.
#
# A "logical case" is mostly server-neutral SQL. The only unavoidable
# dialect difference is per-table TidesDB options, isolated behind one
# token:
#
#   {{TABLE_OPTS:compression=LZ4,bloom_filter=1,encrypted=1}}
#
#   -> MySQL  : ENGINE_ATTRIBUTE='{"compression":"LZ4","bloom_filter":true,"encrypted":true}'
#   -> MariaDB: COMPRESSION='LZ4' BLOOM_FILTER=1 ENCRYPTED=1
#
# Everything else (ENGINE=TidesDB, DDL, DML, MATCH, spatial, ALTER ...
# ALGORITHM=INPLACE) is identical grammar on both servers.
#
# Sourced by run-parity.sh. No side effects on source.
set -euo pipefail

# ---- option-token rendering -------------------------------------------------

# render_table_opts <server> <csv>   csv like "compression=LZ4,bloom_filter=1"
render_table_opts() {
    local server="$1" csv="$2" out="" k v
    [ -z "$csv" ] && { printf ''; return; }
    if [ "$server" = mysql ]; then
        local json=""
        IFS=',' read -ra PAIRS <<<"$csv"
        for kv in "${PAIRS[@]}"; do
            k="${kv%%=*}"; v="${kv#*=}"
            case "$v" in
                1|true|TRUE)  v='true' ;;
                0|false|FALSE) v='false' ;;
                *) v="\"$v\"" ;;
            esac
            json="${json:+${json},}\"${k}\":${v}"
        done
        out="ENGINE_ATTRIBUTE='{${json}}'"
    else
        IFS=',' read -ra PAIRS <<<"$csv"
        for kv in "${PAIRS[@]}"; do
            k="${kv%%=*}"; v="${kv#*=}"
            local K
            K=$(printf '%s' "$k" | tr '[:lower:]' '[:upper:]')
            case "$v" in
                [0-9]*) out="${out:+${out} }${K}=${v}" ;;
                *)      out="${out:+${out} }${K}='${v}'" ;;
            esac
        done
    fi
    printf '%s' "$out"
}

# render_case <server> <case-file>   -> rendered SQL on stdout (directives stripped)
render_case() {
    local server="$1" file="$2" line token csv
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            '-- @'*) continue ;;                 # directives are metadata, not SQL
        esac
        while [[ "$line" == *'{{TABLE_OPTS:'* ]]; do
            token="${line#*\{\{TABLE_OPTS:}"; token="${token%%\}\}*}"
            csv="$token"
            local rendered; rendered="$(render_table_opts "$server" "$csv")"
            line="${line/\{\{TABLE_OPTS:${token}\}\}/$rendered}"
        done
        printf '%s\n' "$line"
    done <"$file"
    return 0
}

# ---- client transport -------------------------------------------------------

# sut_client <server> <container>   echoes the docker-exec client prefix
# Batch + skip-column-names => deterministic tab-separated rows.
sut_client() {
    local server="$1" cid="$2"
    if [ "$server" = mysql ]; then
        printf 'docker exec -i %s mysql -uroot -N -B' "$cid"
    else
        printf 'docker exec -i %s mariadb --socket=/tmp/mariadb.sock -uroot -N -B' "$cid"
    fi
}

# sut_wait_ready <server> <container> [tries]
# Requires REAL query success via the SAME client path the cases use,
# and N consecutive hits -- the official mysql image runs a temporary
# init server then restarts the real one, so a single early success is
# not proof the durable server is listening (caused ERROR 2002 mid-
# restart on the first case).
sut_wait_ready() {
    local server="$1" cid="$2" tries="${3:-90}" i=0 streak=0 need=3
    while [ "$i" -lt "$tries" ]; do
        local ok=1 got
        if [ "$server" = mysql ]; then
            got=$(docker exec "$cid" mysql -uroot -N -B -e "SELECT 1" 2>/dev/null) || ok=0
        else
            got=$(docker exec "$cid" mariadb --socket=/tmp/mariadb.sock -uroot \
                  -N -B -e "SELECT 1" 2>/dev/null) || ok=0
        fi
        if [ "$ok" = 1 ] && [ "$got" = "1" ]; then
            streak=$((streak+1))
            [ "$streak" -ge "$need" ] && return 0
        else
            streak=0
        fi
        i=$((i+1)); sleep 2
    done
    return 1
}

# sut_run <server> <container>   reads SQL on stdin, prints raw client output
sut_run() {
    local server="$1" cid="$2"
    eval "$(sut_client "$server" "$cid")"
}

# normalize   stdin -> canonical form for A-vs-B and expectation compare.
# Strips client chatter, trims, drops blank lines. (Row ordering is the
# case author's responsibility: use ORDER BY, or mark -- @unordered which
# run-parity.sh honors by sort.)
normalize() {
    sed -E '/^Query OK/d; /^Records:/d; /^[[:space:]]*$/d' \
        | sed -E 's/[[:space:]]+$//'
}
