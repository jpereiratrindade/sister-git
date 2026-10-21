#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
TMP="$(mktemp -d)"; trap 'status=$?; [[ -n "${PID:-}" ]] && kill "$PID" 2>/dev/null || :; if ((status)); then cat "$TMP/server.log" 2>/dev/null || :; fi; rm -rf -- "$TMP"; exit $status' EXIT
mkdir -p "$TMP/repos" "$TMP/work/src"
git init -q --bare --initial-branch=main "$TMP/repos/MORFO.git"
git init -q --bare --initial-branch=main "$TMP/repos/empty.git"
git -C "$TMP/work" init -q -b main
git -C "$TMP/work" config user.name 'LABECO Teste'
git -C "$TMP/work" config user.email 'teste@example.invalid'
printf '# MORFO\n\nInterface de teste.\n' > "$TMP/work/README.md"
printf 'int main() { return 0; }\n' > "$TMP/work/src/main.cpp"
printf '\0binario' > "$TMP/work/image.bin"
git -C "$TMP/work" add . && git -C "$TMP/work" commit -qm 'Primeira versão'
git -C "$TMP/work" tag v0.1.0
git -C "$TMP/work" push -q "$TMP/repos/MORFO.git" main --tags
printf '// segunda\n' >> "$TMP/work/src/main.cpp"
git -C "$TMP/work" commit -qam 'Evolui código'
git -C "$TMP/work" push -q "$TMP/repos/MORFO.git" main
PORT=$((20000 + RANDOM % 20000))
SISTER_GIT_ROOT="$TMP/repos" SISTER_GIT_WEB_ROOT="$HERE/web" "$HERE/build/sister-git-web" --port "$PORT" >"$TMP/server.log" 2>&1 & PID=$!
for _ in {1..50}; do curl -fsS "http://127.0.0.1:$PORT/api/v1/health" >/dev/null && break; sleep .05; done
curl -fsS "http://127.0.0.1:$PORT/" | grep -q 'SisTer Git'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos" > "$TMP/repos.json"
grep -q '"name":"MORFO"' "$TMP/repos.json"; grep -q '"name":"empty"' "$TMP/repos.json"
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/tree?ref=main" | grep -q '"name":"src"'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/tree?ref=main&path=src" | grep -q 'main.cpp'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/blob?ref=main&path=README.md" | grep -q 'Interface de teste'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/blob?ref=main&path=image.bin" | grep -q '"binary":true'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/commits?ref=main" | grep -q 'Evolui código'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/refs" | grep -q 'v0.1.0'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/compare?base=main~1&head=main" | grep -q 'segunda'
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/v1/repos/MORFO/blob?ref=nao-existe&path=x")" == 404 ]]
[[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/api/v1/repos")" == 405 ]]

# Teste de visibilidade: privado vs publico com token
git --git-dir="$TMP/repos/empty.git" config sister.visibility private
git --git-dir="$TMP/repos/MORFO.git" config sister.visibility public
PORT_AUTH=$((PORT + 2))
SISTER_GIT_ROOT="$TMP/repos" SISTER_GIT_WEB_ROOT="$HERE/web" SISTER_GIT_WEB_TOKEN="test-token-123" "$HERE/build/sister-git-web" --port "$PORT_AUTH" >"$TMP/server_auth.log" 2>&1 & PID_AUTH=$!
trap 'status=$?; [[ -n "${PID:-}" ]] && kill "$PID" 2>/dev/null || :; [[ -n "${PID_AUTH:-}" ]] && kill "$PID_AUTH" 2>/dev/null || :; if ((status)); then cat "$TMP/server.log" "$TMP/server_auth.log" 2>/dev/null || :; fi; rm -rf -- "$TMP"; exit $status' EXIT
for _ in {1..50}; do curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/health" >/dev/null && break; sleep .05; done

# Anonimo: so ve MORFO (publico), nao ve empty (privado)
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos" > "$TMP/anon_repos.json"
grep -q '"name":"MORFO"' "$TMP/anon_repos.json"
if grep -q '"name":"empty"' "$TMP/anon_repos.json"; then echo '[ERRO] Repositório privado vazou para usuário anônimo' >&2; exit 1; fi
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT_AUTH/api/v1/repos/empty/tree")" == 404 ]]

# Autenticado com token: ve ambos MORFO e empty
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos" > "$TMP/auth_repos.json"
grep -q '"name":"MORFO"' "$TMP/auth_repos.json"
grep -q '"name":"empty"' "$TMP/auth_repos.json"
grep -q '"visibility":"private"' "$TMP/auth_repos.json"

if SISTER_GIT_ROOT="$TMP/repos" SISTER_GIT_WEB_ROOT="$HERE/web" "$HERE/build/sister-git-web" --bind 0.0.0.0 --port $((PORT+1)) >"$TMP/public.log" 2>&1; then echo '[ERRO] bind público aceito sem token' >&2; exit 1; fi
git --git-dir="$TMP/repos/MORFO.git" fsck --full >/dev/null
printf '[OK] SisTer Git Web: API, UI, vazio, binário, refs, diff, visibilidade pública/privada, erro e read-only\n'
