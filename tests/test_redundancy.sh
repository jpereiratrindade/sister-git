#!/usr/bin/env bash
# Teste isolado: não usa SSH nem altera diretórios fora de mktemp.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI="$HERE/bin/sister-git"
REDUNDANCY="$HERE/bin/sister-git-redundancy"
TEMP="$(mktemp -d)"
trap 'rm -rf -- "$TEMP"' EXIT
export SISTER_GIT_ROOT="$TEMP/server"
mkdir -p "$SISTER_GIT_ROOT" "$TEMP/bundles"
"$CLI" init MORFO >/dev/null
repo="$SISTER_GIT_ROOT/MORFO.git"
work="$TEMP/MORFO"
git init -q --initial-branch=main "$work"
git -C "$work" config user.name 'SisTer Lab'
git -C "$work" config user.email 'lab@example.test'
printf 'SisTer\n' > "$work/README.md"
git -C "$work" add README.md
git -C "$work" commit -qm 'Inicial'
git -C "$work" tag -a -m 'marco de teste' v0.1.0
git -C "$work" remote add labeco "$repo"
"$REDUNDANCY" sync "$work" labeco
"$CLI" head MORFO main >/dev/null
[[ "$(git --git-dir="$repo" symbolic-ref HEAD)" == refs/heads/main ]]
"$REDUNDANCY" verify "$work" labeco >/dev/null
[[ "$(git --git-dir="$repo" rev-parse refs/heads/main)" == "$(git -C "$work" rev-parse HEAD)" ]]
[[ "$(git --git-dir="$repo" rev-parse refs/tags/v0.1.0)" == "$(git -C "$work" rev-parse refs/tags/v0.1.0)" ]]

# Branch só no servidor: fetch cria uma ref de acompanhamento no PC.
server_work="$TEMP/server-clone"
git clone -q "$repo" "$server_work"
git -C "$server_work" config user.name 'Lab remoto'
git -C "$server_work" config user.email 'server@example.test'
git -C "$server_work" switch -q -c server-only
git -C "$server_work" commit --allow-empty -qm 'Outra branch criada no servidor'
git -C "$server_work" push -q origin server-only
"$REDUNDANCY" sync "$work" labeco
[[ "$(git -C "$work" rev-parse refs/remotes/labeco/server-only)" == "$(git --git-dir="$repo" rev-parse refs/heads/server-only)" ]]

# Bundle é autossuficiente; todas as refs são verificáveis sem o original.
"$REDUNDANCY" bundle "$work" "$TEMP/bundles" > "$TEMP/bundle.log"
bundle="$(find "$TEMP/bundles" -maxdepth 1 -type f -name '*.bundle' -print -quit)"
[[ -s "$bundle" ]]
(cd "$TEMP/bundles" && sha256sum -c "$(basename "$bundle").sha256" >/dev/null)
empty="$TEMP/empty.git"
git init --bare -q "$empty"
git -C "$empty" bundle verify "$bundle" >/dev/null
restored="$TEMP/restored"
git clone -q "$bundle" "$restored"
[[ "$(git -C "$restored" rev-parse HEAD)" == "$(git -C "$work" rev-parse HEAD)" ]]
git -C "$restored" show-ref --verify --quiet refs/tags/v0.1.0
git -C "$work" bundle list-heads "$bundle" | grep -q 'refs/remotes/labeco/server-only'
git -C "$restored" fetch -q "$bundle" \
  'refs/remotes/labeco/server-only:refs/remotes/labeco/server-only'
git -C "$restored" show-ref --verify --quiet refs/remotes/labeco/server-only

# Divergência não é resolvida à força e mantém histórico remoto.
git -C "$server_work" switch -q main
git -C "$server_work" commit --allow-empty -qm 'Commit exclusivo no servidor'
git -C "$server_work" push -q origin main
git -C "$work" commit --allow-empty -qm 'Commit exclusivo no computador'
old_server="$(git --git-dir="$repo" rev-parse refs/heads/main)"
if "$REDUNDANCY" sync "$work" labeco >"$TEMP/diverge.log" 2>&1; then
  echo '[ERRO] Divergência deveria impedir o sync' >&2
  exit 1
fi
[[ "$old_server" == "$(git --git-dir="$repo" rev-parse refs/heads/main)" ]]
if "$REDUNDANCY" verify "$work" labeco >"$TEMP/verify-failed.log" 2>&1; then
  echo '[ERRO] Hash divergente não detectado' >&2
  exit 1
fi
# Nenhuma branch é eliminada pela falta de uma branch local correspondente.
git --git-dir="$repo" show-ref --verify --quiet refs/heads/server-only

# Backup do servidor também funciona pelo comando sister-git backup.
PATH="$HERE/bin:$PATH" "$CLI" backup MORFO "$TEMP/bundles" > "$TEMP/server-bundle.log"
[[ "$(find "$TEMP/bundles" -name '*.bundle' | wc -l)" -eq 2 ]]
git --git-dir="$repo" fsck --full >/dev/null
printf '[OK] Testes de redundância: sync, tags anotadas, refs remotas, bundle, recuperação, divergência e fsck\n'
