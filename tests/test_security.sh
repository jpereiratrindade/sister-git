#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd -P)"
TMP="$(mktemp -d)"
PID=""
trap 'status=$?; [[ -n "$PID" ]] && kill "$PID" 2>/dev/null || :; rm -rf -- "$TMP"; exit $status' EXIT
mkdir -p "$TMP/repos"
PORT=$((30000 + RANDOM % 20000))
SISTER_GIT_ROOT="$TMP/repos" SISTER_GIT_WEB_ROOT="$HERE/web" \
  SISTER_GIT_ADMIN_EMAIL="admin@example.org" \
  "$HERE/build/sister-git-web" --port "$PORT" --dev-auth >"$TMP/server.log" 2>&1 &
PID=$!
for _ in {1..50}; do curl -fsS "http://127.0.0.1:$PORT/api/v1/health" >/dev/null && break; sleep .05; done

BASE="http://127.0.0.1:$PORT"
curl -fsS -D "$TMP/headers" "$BASE/api/v1/health" >/dev/null
grep -qi '^X-Frame-Options: SAMEORIGIN' "$TMP/headers"
grep -qi '^Referrer-Policy: no-referrer' "$TMP/headers"
grep -qi "frame-ancestors 'self'" "$TMP/headers"

curl -fsS -X POST -H 'Content-Type: application/json' \
  -d '{"email":"admin@example.org"}' "$BASE/api/v1/auth/request-code" >/dev/null
[[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
  -d '{"email":"admin@example.org"}' "$BASE/api/v1/auth/request-code")" == 429 ]]

for _ in {1..5}; do
  [[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
    -d '{"email":"admin@example.org","code":"000000"}' "$BASE/api/v1/auth/verify-code")" == 401 ]]
done
python3 - "$TMP/repos/.sister-auth.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
assert db.execute("select attempts from otps where email=?", ("admin@example.org",)).fetchone() == (5,)
PY

[[ "$(curl -s -o /dev/null -w '%{http_code}' -H 'Cookie: sister_token=fake' "$BASE/api/v1/auth/status")" == 200 ]]
curl -fsS -H 'Cookie: sister_token=fake' "$BASE/api/v1/auth/status" | grep -q '"authenticated":false'
[[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Length: 999999999' "$BASE/api/v1/repos")" == 413 ]]

echo '[OK] Controles de segurança web validados'
