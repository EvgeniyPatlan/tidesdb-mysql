#!/usr/bin/env bash
# Smoke-test the tidesdb/mysql Docker image by spinning up a container,
# verifying the engine is loaded, exercising basic CRUD, and tearing down.
#
# Usage:
#   ./docker/runtime/smoke-test.sh                  # uses tidesdb/mysql:9.7
#   ./docker/runtime/smoke-test.sh some/other:tag   # arbitrary image tag
set -euo pipefail

IMAGE="${1:-tidesdb/mysql:9.7}"
CONTAINER="${CONTAINER:-tidesdb-smoke}"
PASSWORD="${PASSWORD:-smoketest}"

echo "[smoke] using image $IMAGE"

cleanup () {
    docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
}
trap cleanup EXIT

cleanup

echo "[smoke] starting container"
docker run -d --name "$CONTAINER" \
    -e MYSQL_ROOT_PASSWORD="$PASSWORD" \
    "$IMAGE" >/dev/null

wait_ready () {
    local n
    for n in $(seq 1 60); do
        if docker exec "$CONTAINER" mysql -uroot -p"$PASSWORD" -e "SELECT 1" >/dev/null 2>&1; then
            return 0
        fi
        sleep 2
    done
    echo "FAIL: mysqld did not become ready within 120s"
    docker logs "$CONTAINER" | tail -30
    return 1
}

echo "[smoke] waiting for mysqld to accept connections (incl. init scripts)"
wait_ready

run () { docker exec -i "$CONTAINER" mysql -uroot -p"$PASSWORD" --batch --silent "$@"; }

echo "[smoke] verifying engine is registered"
ENGINE=$(run -Nse "SELECT engine FROM information_schema.engines WHERE engine LIKE 'TIDESDB';")
[ -n "$ENGINE" ] || { echo "FAIL: TIDESDB engine missing"; exit 1; }
echo "  engine: $ENGINE  ✓"

echo "[smoke] CRUD round-trip on a fresh table"
run <<'SQL'
DROP DATABASE IF EXISTS smoke;
CREATE DATABASE smoke;
USE smoke;
CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(64)) ENGINE=TIDESDB;
INSERT INTO t VALUES (1,'one'),(2,'two'),(3,'three');
UPDATE t SET v='TWO' WHERE id=2;
DELETE FROM t WHERE id=3;
SQL
ROWS=$(run -Nse "SELECT COUNT(*) FROM smoke.t")
[ "$ROWS" = "2" ] || { echo "FAIL: expected 2 rows, got $ROWS"; exit 1; }
VAL=$(run -Nse "SELECT v FROM smoke.t WHERE id=2")
[ "$VAL" = "TWO" ] || { echo "FAIL: expected v='TWO' for id=2, got '$VAL'"; exit 1; }
echo "  CRUD round-trip  ✓"

echo "[smoke] composite PK + AUTO_INCREMENT trailing"
run <<'SQL'
USE smoke;
CREATE TABLE t_ai (
  tenant INT NOT NULL,
  id INT NOT NULL AUTO_INCREMENT,
  label VARCHAR(20),
  PRIMARY KEY (tenant, id)
) ENGINE=TIDESDB;
INSERT INTO t_ai (tenant, label) VALUES (1,'a'),(1,'b'),(2,'c');
SQL
COUNT=$(run -Nse "SELECT COUNT(*) FROM smoke.t_ai")
[ "$COUNT" = "3" ] || { echo "FAIL: expected 3 rows in t_ai, got $COUNT"; exit 1; }
echo "  composite PK + AUTO_INCREMENT  ✓"

echo "[smoke] composite UNIQUE rejects duplicates"
run <<'SQL'
USE smoke;
CREATE TABLE t_uq (
  id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  region VARCHAR(8) NOT NULL,
  sku INT NOT NULL,
  UNIQUE KEY uk (region, sku)
) ENGINE=TIDESDB;
INSERT INTO t_uq (region, sku) VALUES ('us', 1);
SQL
if run <<'SQL' >/dev/null 2>&1
USE smoke;
INSERT INTO t_uq (region, sku) VALUES ('us', 1);
SQL
then
    echo "FAIL: duplicate INSERT into composite UNIQUE was accepted"
    exit 1
fi
echo "  composite UNIQUE enforced  ✓"

echo "[smoke] persistence across container restart"
run -e "INSERT INTO smoke.t VALUES (99, 'persisted');" >/dev/null
docker restart "$CONTAINER" >/dev/null
echo "[smoke] waiting for mysqld to come back"
wait_ready
PERSISTED=$(run -Nse "SELECT v FROM smoke.t WHERE id=99")
[ "$PERSISTED" = "persisted" ] || { echo "FAIL: expected 'persisted' after restart, got '$PERSISTED'"; exit 1; }
echo "  persistence  ✓"

echo
echo "[smoke] all checks passed for $IMAGE"
