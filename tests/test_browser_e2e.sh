#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
DRIVER="${GECKODRIVER:-$HERE/build/tools/geckodriver}"
[[ -x "$DRIVER" ]] || { echo "geckodriver ausente: $DRIVER" >&2; exit 1; }
TMP="$(mktemp -d)"
PORT=$((30000 + RANDOM % 10000))
trap 'status=$?; [[ -n "${PID:-}" ]] && kill "$PID" 2>/dev/null || :; ((status)) && cat "$TMP/server.log" >&2 || :; rm -rf -- "$TMP"; exit $status' EXIT
mkdir -p "$TMP/repos"
SISTER_GIT_ROOT="$TMP/repos" SISTER_GIT_WEB_ROOT="$HERE/web" SISTER_GIT_WEB_TOKEN="e2e-token" \
SISTER_GIT_ADMIN_EMAIL="admin@embrapa.br" "$HERE/build/sister-git-web" --port "$PORT" --dev-auth >"$TMP/server.log" 2>&1 & PID=$!
for _ in {1..100}; do curl -fsS "http://127.0.0.1:$PORT/api/v1/health" >/dev/null && break; sleep .05; done
python3 "$HERE/tests/test_browser_e2e.py" "http://127.0.0.1:$PORT/" "e2e-token" "$DRIVER"
git --git-dir="$TMP/repos/e2e-editorial.git" show 'ed/v1.0:.sister-editorial.json' | grep -q '"current_stage": "published"'
git --git-dir="$TMP/repos/e2e-editorial.git" show 'ed/v1.0:manuscrito.md' | grep -q 'Conteúdo pelo Firefox'
git --git-dir="$TMP/repos/e2e-editorial.git" fsck --full >/dev/null
printf '[OK] Fluxo editorial completo executado no Firefox headless\n'
