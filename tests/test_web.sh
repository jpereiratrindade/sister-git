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
curl -fsS "http://127.0.0.1:$PORT/" > "$TMP/index.html" && grep -q 'SisTer Git' "$TMP/index.html"
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos" > "$TMP/repos.json"
grep -q '"name":"MORFO"' "$TMP/repos.json"; grep -q '"name":"empty"' "$TMP/repos.json"
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/tree?ref=main" | grep -q '"name":"src"'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/tree?ref=main&path=src" | grep -q 'main.cpp'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/blob?ref=main&path=README.md" | grep -q 'Interface de teste'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/blob?ref=main&path=image.bin" | grep -q '"binary":true'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/commits?ref=main" | grep -q 'Evolui código'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/refs" | grep -q 'v0.1.0'
curl -fsS "http://127.0.0.1:$PORT/api/v1/repos/MORFO/compare?base=main~1&head=main" | grep -q 'segunda'
[[ "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "http://127.0.0.1:$PORT/api/v1/repos")" == 405 ]]
[[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/api/v1/repos")" == 401 ]]

# Teste de visibilidade: privado vs publico com token e OTP
git --git-dir="$TMP/repos/empty.git" config sister.visibility private
git --git-dir="$TMP/repos/MORFO.git" config sister.visibility public
PORT_AUTH=$((PORT + 2))
SISTER_GIT_ROOT="$TMP/repos" SISTER_GIT_WEB_ROOT="$HERE/web" SISTER_GIT_WEB_TOKEN="test-token-123" "$HERE/build/sister-git-web" --port "$PORT_AUTH" --dev-auth >"$TMP/server_auth.log" 2>&1 & PID_AUTH=$!
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

# Teste Autenticação OTP Passwordless Institucional
curl -fsS -X POST -H "Content-Type: application/json" \
  -d '{"email":"pesquisador@embrapa.br"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/auth/request-code" | grep -q '"ok":true'
[[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" -d '{"email":"pesquisador@embrapa.br","code":"000000"}' "http://127.0.0.1:$PORT_AUTH/api/v1/auth/verify-code")" == 401 ]]
curl -fsS -X POST -H "Content-Type: application/json" \
  -d '{"email":"pesquisador@embrapa.br","code":"123456"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/auth/verify-code" > "$TMP/otp_verify.json"
grep -q '"ok":true' "$TMP/otp_verify.json"
grep -q '"email":"pesquisador@embrapa.br"' "$TMP/otp_verify.json"
OTP_TOKEN="$(grep -o '"token":"[^"]*"' "$TMP/otp_verify.json" | cut -d'"' -f4)"
curl -fsS -H "Authorization: Bearer $OTP_TOKEN" "http://127.0.0.1:$PORT_AUTH/api/v1/auth/status" | grep -q '"authenticated":true'

# Teste de Gestão de Usuários Autorizados (Admin Whitelist)
curl -fsS -H "Authorization: Bearer $OTP_TOKEN" "http://127.0.0.1:$PORT_AUTH/api/v1/admin/users" | grep -q 'pesquisador@embrapa.br'
curl -fsS -H "Authorization: Bearer $OTP_TOKEN" -H "Content-Type: application/json" \
  -d '{"email":"colega@embrapa.br","name":"Dra. Colega","role":"researcher"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/admin/users" | grep -q '"ok":true'
# Usuário não cadastrado recebe 403 ao pedir OTP
[[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" -d '{"email":"desconhecido@embrapa.br"}' "http://127.0.0.1:$PORT_AUTH/api/v1/auth/request-code")" == 403 ]]
# Usuário cadastrado recebe 200
curl -fsS -X POST -H "Content-Type: application/json" -d '{"email":"colega@embrapa.br"}' "http://127.0.0.1:$PORT_AUTH/api/v1/auth/request-code" | grep -q '"ok":true'

# Teste de alternância de visibilidade via API
curl -fsS -H "Authorization: Bearer $OTP_TOKEN" -H "Content-Type: application/json" \
  -d '{"visibility":"public"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/empty/visibility" | grep -q '"visibility":"public"'

# Teste SisTer Git Editorial (manifesto e tags ed/)
printf '{"project_type":"editorial","title":"Campos Nativos","current_stage":"in_review"}\n' > "$TMP/work/.sister-editorial.json"
git -C "$TMP/work" add .sister-editorial.json
git -C "$TMP/work" commit -qm "feat(editorial): add editorial manifest"
git -C "$TMP/work" tag ed/v1.0 -m "Primeira versao editorial"
git -C "$TMP/work" push -q "$TMP/repos/MORFO.git" main --tags
git --git-dir="$TMP/repos/MORFO.git" config sister.type editorial

curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos" > "$TMP/repos_ed.json"
grep -q '"type":"editorial"' "$TMP/repos_ed.json"
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/MORFO/editorial" > "$TMP/morfo_ed.json"
grep -q '"is_editorial":true' "$TMP/morfo_ed.json"
grep -q '"has_manifest":true' "$TMP/morfo_ed.json"
grep -q 'ed/v1.0' "$TMP/morfo_ed.json"
grep -q 'Campos Nativos' "$TMP/morfo_ed.json"

# Manifesto inválido nunca pode corromper o JSON da resposta da API.
printf '{"project_type":"editorial",, "title":"Quebrado"}\n' > "$TMP/work/.sister-editorial.json"
git -C "$TMP/work" add .sister-editorial.json
git -C "$TMP/work" commit -qm "test: add malformed editorial manifest"
git -C "$TMP/work" push -q "$TMP/repos/MORFO.git" main
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/MORFO/editorial" > "$TMP/malformed_ed.json"
python3 -m json.tool "$TMP/malformed_ed.json" >/dev/null
grep -q '"manifest":{}' "$TMP/malformed_ed.json"
grep -q 'JSON inválido' "$TMP/malformed_ed.json"

# Testes interativos C++26 do SisTer Git Editorial
# 1. Criação de repositório editorial
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"name":"artigo-teste","description":"Artigo sobre ecologia","type":"editorial","visibility":"public","title":"Ecologia dos Campos"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos" > "$TMP/create_ed.json"
grep -q '"ok":true' "$TMP/create_ed.json"
grep -q '"type":"editorial"' "$TMP/create_ed.json"

# 2. Verifica se manuscrito e manifesto foram criados no repositório bare
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/blob?ref=main&path=manuscrito.md" | grep -q 'Ecologia dos Campos'
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" | grep -q '"is_editorial":true'

# 3. Salvar nova versão do manuscrito
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"content":"# Ecologia dos Campos\n\nTexto atualizado na revisão editorial.\n\nNovas descobertas.","message":"Revisão da introdução com novas fontes","author_name":"Dra. Ana","author_email":"ana@labeco.invalid"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/save" > "$TMP/save_ed.json"
grep -q '"ok":true' "$TMP/save_ed.json"
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/blob?ref=main&path=manuscrito.md" | grep -q 'Texto atualizado na revisão'
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/commits?ref=main" | grep -q 'Revisão da introdução com novas fontes'

# 4. Registrar parecer editorial
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"reviewer":"Prof. Carlos","recommendation":"accept_with_revisions","comments":"Artigo excelente, favor detalhar a seção 3."}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/review" > "$TMP/review_ed.json"
grep -q '"ok":true' "$TMP/review_ed.json"
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" | python3 -m json.tool | grep -q 'Prof. Carlos'

# 5. Publicar edição documental (cria tag ed/v1.0-publicado)
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"tag_name":"ed/v1.0-publicado","notes":"Publicação da primeira edição com DOI e W3C PROV","publisher_name":"SisTer Editorial","publisher_email":"editorial@labeco.invalid"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/publish" > "$TMP/pub_ed.json"
grep -q '"ok":true' "$TMP/pub_ed.json"
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" | python3 -m json.tool | grep -q 'ed/v1.0-publicado'
git --git-dir="$TMP/repos/artigo-teste.git" tag -l | grep -q 'ed/v1.0-publicado'

# 6. Testar download de arquivo compactado (ZIP e TAR.GZ)
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/archive?format=zip" > "$TMP/artigo.zip"
file "$TMP/artigo.zip" | grep -qi 'zip'
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/archive?format=tar.gz" > "$TMP/artigo.tar.gz"
file "$TMP/artigo.tar.gz" | grep -qi 'gzip'

# 7. Testar upload de documento (DOCX / ODT / TEX / MD) e download raw
B64_SAMPLE=$(printf 'PK\x03\x04teste-docx-binario' | base64 -w0)
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d "{\"filename\":\"artigo-completo.docx\",\"content_base64\":\"$B64_SAMPLE\",\"commit_msg\":\"Upload do manuscrito original em DOCX\",\"is_main\":\"true\"}" \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/upload" > "$TMP/upload_resp.json"
grep -q '"ok":true' "$TMP/upload_resp.json"

curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/raw?path=artigo-completo.docx" > "$TMP/artigo_baixado.docx"
grep -q 'teste-docx-binario' "$TMP/artigo_baixado.docx"
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" | grep -q 'artigo-completo.docx'

if SISTER_GIT_ROOT="$TMP/repos" SISTER_GIT_WEB_ROOT="$HERE/web" "$HERE/build/sister-git-web" --bind 0.0.0.0 --port $((PORT+1)) >"$TMP/public.log" 2>&1; then echo '[ERRO] bind público aceito sem token' >&2; exit 1; fi
git --git-dir="$TMP/repos/MORFO.git" fsck --full >/dev/null
git --git-dir="$TMP/repos/artigo-teste.git" fsck --full >/dev/null
printf '[OK] SisTer Git Web: API, UI, vazio, binário, refs, diff, visibilidade pública/privada, modo editorial interativo C++26, erro e integridade git fsck\n'
