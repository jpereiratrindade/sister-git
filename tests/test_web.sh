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
SISTER_GIT_ROOT="$TMP/repos" SISTER_GIT_WEB_ROOT="$HERE/web" SISTER_GIT_WEB_TOKEN="test-token-123" SISTER_GIT_ADMIN_EMAIL="pesquisador@embrapa.br" "$HERE/build/sister-git-web" --port "$PORT_AUTH" --dev-auth >"$TMP/server_auth.log" 2>&1 & PID_AUTH=$!
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
[[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" -d '{"email":"intruso@embrapa.br"}' "http://127.0.0.1:$PORT_AUTH/api/v1/auth/request-code")" == 403 ]]
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
curl -fsS -X POST -H "Content-Type: application/json" \
  -d '{"email":"colega@embrapa.br","code":"123456"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/auth/verify-code" > "$TMP/colega_verify.json"
COLEGA_TOKEN="$(grep -o '"token":"[^"]*"' "$TMP/colega_verify.json" | cut -d'"' -f4)"
curl -fsS -H "Authorization: Bearer $OTP_TOKEN" -H "Content-Type: application/json" \
  -d '{"email":"editor@embrapa.br","name":"Editor Responsável","role":"researcher"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/admin/users" | grep -q '"ok":true'
curl -fsS -X POST -H "Content-Type: application/json" -d '{"email":"editor@embrapa.br"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/auth/request-code" >/dev/null
curl -fsS -X POST -H "Content-Type: application/json" \
  -d '{"email":"editor@embrapa.br","code":"123456"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/auth/verify-code" > "$TMP/editor_verify.json"
EDITOR_TOKEN="$(grep -o '"token":"[^"]*"' "$TMP/editor_verify.json" | cut -d'"' -f4)"
# Usuário autenticado não pode escrever em projeto público que não possui.
[[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Authorization: Bearer $COLEGA_TOKEN" -H "Content-Type: application/json" \
  -d '{"content":"intrusão","message":"não autorizado"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/MORFO/editorial/save")" == 403 ]]

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
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/MORFO/editorial" > "$TMP/morfo_ed.json"
grep -q '"is_editorial":true' "$TMP/morfo_ed.json"
grep -q '"has_manifest":true' "$TMP/morfo_ed.json"
grep -q 'ed/v1.0' "$TMP/morfo_ed.json"
grep -q 'Campos Nativos' "$TMP/morfo_ed.json"

# Manifesto inválido nunca pode corromper o JSON da resposta da API.
printf '{"project_type":"editorial",, "title":"Quebrado"}\n' > "$TMP/work/.sister-editorial.json"
git -C "$TMP/work" add .sister-editorial.json
git -C "$TMP/work" commit -qm "test: add malformed editorial manifest"
git -C "$TMP/work" push -q "$TMP/repos/MORFO.git" main
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/MORFO/editorial" > "$TMP/malformed_ed.json"
python3 -m json.tool "$TMP/malformed_ed.json" >/dev/null
grep -q '"manifest":{}' "$TMP/malformed_ed.json"
grep -q 'JSON inválido' "$TMP/malformed_ed.json"

# Testes interativos C++26 do SisTer Git Editorial
# 1. Criação de repositório editorial
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"name":"editorial-privado-padrao","type":"editorial","title":"Privado por padrão"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos" | grep -q '"ok":true'
[[ "$(git --git-dir="$TMP/repos/editorial-privado-padrao.git" config --get sister.visibility)" == private ]]
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"name":"visibilidade-invalida","type":"editorial","visibility":"aberta"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos")" == 400 ]]
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"name":"artigo-teste","description":"Artigo sobre ecologia","type":"editorial","visibility":"public","title":"Ecologia dos Campos"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos" > "$TMP/create_ed.json"
grep -q '"ok":true' "$TMP/create_ed.json"
grep -q '"type":"editorial"' "$TMP/create_ed.json"
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos" > "$TMP/repos_metadata.json"
grep -q '"title":"Ecologia dos Campos"' "$TMP/repos_metadata.json"
grep -q '"description":"Artigo sobre ecologia"' "$TMP/repos_metadata.json"
grep -q '"current_stage":"draft"' "$TMP/repos_metadata.json"
grep -q '"editorial_tags":0' "$TMP/repos_metadata.json"
git clone -q "$TMP/repos/artigo-teste.git" "$TMP/direct-push"
git -C "$TMP/direct-push" config user.name 'Tentativa Direta'
git -C "$TMP/direct-push" config user.email 'direto@example.invalid'
printf '\nAlteração fora da governança.\n' >> "$TMP/direct-push/manuscrito.md"
git -C "$TMP/direct-push" add manuscrito.md
git -C "$TMP/direct-push" commit -qm 'bypass por push direto'
if git -C "$TMP/direct-push" push origin main >"$TMP/direct-push.log" 2>&1; then echo '[ERRO] push direto alterou main editorial' >&2; exit 1; fi
grep -q 'Ref editorial protegida' "$TMP/direct-push.log"
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"email":"editor@embrapa.br","role":"editor"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/assign-member" | grep -q '"role":"editor"'
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"filename":".sister-editorial.json","content":"{\"current_stage\":\"accepted\"}","message":"bypass"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/save")" == 403 ]]
MANIFEST_B64="$(printf '{\"current_stage\":\"accepted\"}' | base64 -w0)"
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d "{\"filename\":\".sister-editorial.json\",\"content_base64\":\"$MANIFEST_B64\",\"commit_msg\":\"bypass\"}" \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/upload")" == 403 ]]
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"to":"accepted","reason":"salto proibido"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/transition")" == 409 ]]
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"tag":"ed/v0-invalida","title":"Inválida","notes":"sem aceite"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/publish")" == 409 ]]

# 2. Verifica se manuscrito e manifesto foram criados no repositório bare
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/blob?ref=main&path=manuscrito.md" | grep -q 'Ecologia dos Campos'
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" | grep -q '"is_editorial":true'

# 3. Salvar nova versão do manuscrito
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"content":"# Ecologia dos Campos\n\nTexto atualizado na revisão editorial.\n\nNovas descobertas.","message":"Revisão da introdução com novas fontes","author_name":"Dra. Ana","author_email":"ana@labeco.invalid"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/save" > "$TMP/save_ed.json"
grep -q '"ok":true' "$TMP/save_ed.json"
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/blob?ref=main&path=manuscrito.md" | grep -q 'Texto atualizado na revisão'
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/commits?ref=main" | grep -q 'Revisão da introdução com novas fontes'

# 4. Registrar parecer editorial
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"to":"submitted","reason":"Manuscrito pronto para avaliação"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/transition" | grep -q '"to":"submitted"'
git --git-dir="$TMP/repos/artigo-teste.git" for-each-ref --format='%(refname)' refs/sister/submissions | grep -q 'refs/sister/submissions/'
git --git-dir="$TMP/repos/artigo-teste.git" show 'HEAD:.sister-editorial.json' | grep -q '"transitions"'
curl -fsS -H "Authorization: Bearer $EDITOR_TOKEN" -H "Content-Type: application/json" \
  -d '{"to":"in_review","reason":"Revisor designado"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/transition" | grep -q '"to":"in_review"'
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $COLEGA_TOKEN" -H "Content-Type: application/json" \
  -d '{"recommendation":"accept","summary":"sem designação"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/review")" == 403 ]]
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"email":"colega@embrapa.br","role":"reviewer"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/assign-member" | grep -q '"role":"reviewer"'
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" -d '{"visibility":"private"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/visibility" >/dev/null
curl -fsS -H "Authorization: Bearer $COLEGA_TOKEN" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" | grep -q '"project_role":"reviewer"'
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial?token=test-token-123")" == 404 ]]
curl -fsS -H "Authorization: Bearer $COLEGA_TOKEN" -H "Content-Type: application/json" \
  -d '{"reviewer":"Identidade Falsa","recommendation":"accept_with_revisions","comments":"Artigo excelente, favor detalhar a seção 3."}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/review" > "$TMP/review_ed.json"
grep -q '"ok":true' "$TMP/review_ed.json"
REVIEW_ID="$(grep -o '"review_id":"[^"]*"' "$TMP/review_ed.json" | cut -d'"' -f4)"
curl -fsS -H "Authorization: Bearer $COLEGA_TOKEN" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" > "$TMP/after_review.json"
python3 -m json.tool "$TMP/after_review.json" | grep -q 'Dra. Colega'
if grep -q 'Identidade Falsa' "$TMP/after_review.json"; then echo '[ERRO] identidade declarada substituiu identidade autenticada' >&2; exit 1; fi
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d "{\"review_id\":\"$REVIEW_ID\",\"response\":\"Recomendações incorporadas na nova versão.\"}" \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/respond" | grep -q '"ok":true'
git --git-dir="$TMP/repos/artigo-teste.git" show 'HEAD:.sister-editorial.json' | grep -q '"responses"'
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" -d '{"visibility":"public"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/visibility" >/dev/null

# 5. Publicar edição documental (cria tag ed/v1.0-publicado)
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"to":"accepted","reason":""}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/transition")" == 400 ]]
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $COLEGA_TOKEN" -H "Content-Type: application/json" \
  -d '{"to":"accepted","reason":"revisor não decide"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/transition")" == 403 ]]
curl -fsS -H "Authorization: Bearer $EDITOR_TOKEN" -H "Content-Type: application/json" \
  -d '{"to":"accepted","reason":"Parecer favorável homologado"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/transition" | grep -q '"to":"accepted"'
git --git-dir="$TMP/repos/artigo-teste.git" show 'HEAD:.sister-editorial.json' | grep -q '"outcome":"accepted"'
git --git-dir="$TMP/repos/artigo-teste.git" show 'HEAD:.sister-editorial.json' | grep -q '"justification":"Parecer favorável homologado"'
curl -fsS -H "Authorization: Bearer $EDITOR_TOKEN" -H "Content-Type: application/json" \
  -d '{"tag_name":"ed/v1.0-publicado","notes":"Publicação da primeira edição com DOI e W3C PROV","publisher_name":"SisTer Editorial","publisher_email":"editorial@labeco.invalid"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/publish" > "$TMP/pub_ed.json"
grep -q '"ok":true' "$TMP/pub_ed.json"
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" | python3 -m json.tool | grep -q 'ed/v1.0-publicado'
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" > "$TMP/public_editorial.json"
if grep -Eq 'reviews|responses|transitions|decisions|owner' "$TMP/public_editorial.json"; then echo '[ERRO] metadados editoriais internos expostos publicamente' >&2; exit 1; fi
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/raw?ref=HEAD&path=manuscrito.md")" == 403 ]]
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/commits?ref=main")" == 403 ]]
git --git-dir="$TMP/repos/artigo-teste.git" tag -l | grep -q 'ed/v1.0-publicado'
git --git-dir="$TMP/repos/artigo-teste.git" show 'ed/v1.0-publicado:.sister-editorial.json' | grep -q '"current_stage": "published"'
git --git-dir="$TMP/repos/artigo-teste.git" show 'ed/v1.0-publicado:.sister-editorial.json' | grep -q '"editions"'

# Simular falha intermediária: commit publicado persistiu, mas tag não existe.
PUBLISHED_HEAD="$(git --git-dir="$TMP/repos/artigo-teste.git" rev-parse HEAD)"
git --git-dir="$TMP/repos/artigo-teste.git" tag -d 'ed/v1.0-publicado' >/dev/null
[[ "$(curl -s -o "$TMP/pub_conflict_recovery.json" -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"tag_name":"ed/v1.0-publicado","notes":"metadados conflitantes"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/publish")" == 409 ]]
[[ -z "$(git --git-dir="$TMP/repos/artigo-teste.git" tag -l 'ed/v1.0-publicado')" ]]
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"tag_name":"ed/v1.0-publicado","notes":"Publicação da primeira edição com DOI e W3C PROV"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/publish" > "$TMP/pub_recovered.json"
grep -q '"recovered":true' "$TMP/pub_recovered.json"
[[ "$(git --git-dir="$TMP/repos/artigo-teste.git" rev-parse HEAD)" == "$PUBLISHED_HEAD" ]]
[[ "$(git --git-dir="$TMP/repos/artigo-teste.git" rev-parse 'ed/v1.0-publicado^{commit}')" == "$PUBLISHED_HEAD" ]]
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"tag_name":"ed/v1.0-publicado","notes":"Publicação da primeira edição com DOI e W3C PROV"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/publish" | grep -q '"idempotent":true'
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"tag_name":"ed/v1.0-publicado","notes":"metadados conflitantes"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/publish")" == 409 ]]

# Nova edição evolui HEAD sem alterar conteúdo da tag publicada.
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"to":"draft","reason":"Início da segunda edição"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/transition" | grep -q '"to":"draft"'
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"content":"# Segunda edição\n\nConteúdo posterior à publicação.","message":"Inicia segunda edição"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/save" >/dev/null
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/raw?ref=ed%2Fv1.0-publicado&path=manuscrito.md" > "$TMP/manuscrito-publicado.md"
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/raw?ref=HEAD&path=manuscrito.md" > "$TMP/manuscrito-head.md"
grep -q 'Texto atualizado na revisão' "$TMP/manuscrito-publicado.md"
grep -q 'Segunda edição' "$TMP/manuscrito-head.md"

# 6. Testar download de arquivo compactado (ZIP e TAR.GZ)
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/archive?format=zip" > "$TMP/artigo.zip"
file "$TMP/artigo.zip" | grep -qi 'zip'
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/archive?format=tar.gz" > "$TMP/artigo.tar.gz"
file "$TMP/artigo.tar.gz" | grep -qi 'gzip'

# 7. Testar upload de documento (DOCX / ODT / TEX / MD) e download raw
printf 'PK\x03\x04teste-docx-binario\x00\xff' > "$TMP/artigo-original.docx"
B64_SAMPLE=$(base64 -w0 "$TMP/artigo-original.docx")
BEFORE_UPLOAD="$(git --git-dir="$TMP/repos/artigo-teste.git" rev-parse HEAD)"
curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d "{\"filename\":\"artigo-completo.docx\",\"content_base64\":\"$B64_SAMPLE\",\"commit_msg\":\"Upload do manuscrito original em DOCX\",\"is_main\":\"true\"}" \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/upload" > "$TMP/upload_resp.json"
grep -q '"ok":true' "$TMP/upload_resp.json"
AFTER_UPLOAD="$(git --git-dir="$TMP/repos/artigo-teste.git" rev-parse HEAD)"
[[ "$(git --git-dir="$TMP/repos/artigo-teste.git" rev-list --count "$BEFORE_UPLOAD..$AFTER_UPLOAD")" == 1 ]]
git --git-dir="$TMP/repos/artigo-teste.git" show "$AFTER_UPLOAD:.sister-editorial.json" | grep -q 'artigo-completo.docx'

# Cliente baseado em HEAD antigo recebe 409 e não altera a referência principal.
[[ "$(curl -s -o "$TMP/stale-write.json" -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d "{\"filename\":\"concorrente.md\",\"content\":\"versão obsoleta\",\"message\":\"gravação concorrente\",\"expected_head\":\"$BEFORE_UPLOAD\"}" \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial/save")" == 409 ]]
grep -q 'Conflito de concorrência' "$TMP/stale-write.json"
[[ "$(git --git-dir="$TMP/repos/artigo-teste.git" rev-parse HEAD)" == "$AFTER_UPLOAD" ]]
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" | grep -q "\"head\":\"$AFTER_UPLOAD\""

curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/raw?path=artigo-completo.docx" > "$TMP/artigo_baixado.docx"
cmp "$TMP/artigo-original.docx" "$TMP/artigo_baixado.docx"
[[ "$(sha256sum "$TMP/artigo-original.docx" | cut -d' ' -f1)" == "$(sha256sum "$TMP/artigo_baixado.docx" | cut -d' ' -f1)" ]]
git --git-dir="$TMP/repos/artigo-teste.git" show "HEAD:artigo-completo.docx" > "$TMP/artigo_blob.docx"
[[ "$(sha256sum "$TMP/artigo-original.docx" | cut -d' ' -f1)" == "$(sha256sum "$TMP/artigo_blob.docx" | cut -d' ' -f1)" ]]
curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/editorial" | grep -q 'artigo-completo.docx'
curl -fsS "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/archive?ref=ed%2Fv1.0-publicado&format=zip" > "$TMP/edicao-publicada.zip"
python3 - "$TMP/edicao-publicada.zip" <<'PY'
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
assert any(name.endswith('/manuscrito.md') for name in names)
assert not any(name.endswith('/artigo-completo.docx') for name in names)
PY
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"filename":"corrompido.pdf","content_base64":"%%%INVALIDO%%%","commit_msg":"deve falhar"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/upload")" == 400 ]]
[[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
  -d '{"filename":"vazio.pdf","content_base64":"","commit_msg":"deve rejeitar vazio"}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/upload")" == 400 ]]

# Formatos binários obrigatórios: original, blob Git e download devem ter o mesmo SHA-256.
printf 'PK\x03\x04conteudo-odt\x00\x80\xff' > "$TMP/amostra.odt"
printf '%%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\n\x00\xff' > "$TMP/amostra.pdf"
printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x80\xff' > "$TMP/amostra.png"
printf '\x00\x01\x02\x7f\x80\xfe\xfftexto\x00fim' > "$TMP/amostra.bin"
for SAMPLE in amostra.odt amostra.pdf amostra.png amostra.bin; do
  SAMPLE_B64="$(base64 -w0 "$TMP/$SAMPLE")"
  curl -fsS -H "Authorization: Bearer test-token-123" -H "Content-Type: application/json" \
    -d "{\"filename\":\"$SAMPLE\",\"content_base64\":\"$SAMPLE_B64\",\"commit_msg\":\"Teste de integridade $SAMPLE\",\"is_main\":false}" \
    "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/upload" > "$TMP/upload-$SAMPLE.json"
  grep -q '"ok":true' "$TMP/upload-$SAMPLE.json"
  git --git-dir="$TMP/repos/artigo-teste.git" show "HEAD:$SAMPLE" > "$TMP/blob-$SAMPLE"
  curl -fsS -H "Authorization: Bearer test-token-123" "http://127.0.0.1:$PORT_AUTH/api/v1/repos/artigo-teste/raw?path=$SAMPLE" > "$TMP/download-$SAMPLE"
  ORIGINAL_SHA="$(sha256sum "$TMP/$SAMPLE" | cut -d' ' -f1)"
  [[ "$ORIGINAL_SHA" == "$(sha256sum "$TMP/blob-$SAMPLE" | cut -d' ' -f1)" ]]
  [[ "$ORIGINAL_SHA" == "$(sha256sum "$TMP/download-$SAMPLE" | cut -d' ' -f1)" ]]
done

# Logout revoga a sessão no backend; reutilização do token não autentica.
curl -fsS -H "Authorization: Bearer $COLEGA_TOKEN" -H "Content-Type: application/json" -d '{}' \
  "http://127.0.0.1:$PORT_AUTH/api/v1/auth/logout" | grep -q '"ok":true'
curl -fsS -H "Authorization: Bearer $COLEGA_TOKEN" \
  "http://127.0.0.1:$PORT_AUTH/api/v1/auth/status" | grep -q '"authenticated":false'

if SISTER_GIT_ROOT="$TMP/repos" SISTER_GIT_WEB_ROOT="$HERE/web" "$HERE/build/sister-git-web" --bind 0.0.0.0 --port $((PORT+1)) >"$TMP/public.log" 2>&1; then echo '[ERRO] bind público aceito sem token' >&2; exit 1; fi
git --git-dir="$TMP/repos/MORFO.git" fsck --full >/dev/null
git --git-dir="$TMP/repos/artigo-teste.git" fsck --full >/dev/null
printf '[OK] SisTer Git Web: API, UI, vazio, binário, refs, diff, visibilidade pública/privada, modo editorial interativo C++26, erro e integridade git fsck\n'
