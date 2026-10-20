#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI="$HERE/bin/sister-git"
HA="$HERE/bin/sister-git-ha"
TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT
mkdir -p "$TMP/node-a" "$TMP/node-b"
SISTER_GIT_ROOT="$TMP/node-a" "$CLI" init sister-git >/dev/null
SISTER_GIT_ROOT="$TMP/node-b" "$CLI" init sister-git >/dev/null
mkdir -p "$TMP/work"
git -C "$TMP/work" init -q -b main
git -C "$TMP/work" -c user.name=Lab -c user.email=lab@example.invalid commit --allow-empty -qm start
git -C "$TMP/work" tag v0.1
GIT_A="$TMP/node-a/sister-git.git"
GIT_B="$TMP/node-b/sister-git.git"
git -C "$TMP/work" push -q "$GIT_A" main --tags

# Ida A -> B: bare remoto inicialmente vazio; ambos obtêm main e tag.
SISTER_GIT_ROOT="$TMP/node-a" "$HA" sync sister-git "$TMP/node-b"
[[ "$(git --git-dir="$GIT_B" rev-parse refs/heads/main)" == "$(git --git-dir="$GIT_A" rev-parse refs/heads/main)" ]]
git --git-dir="$GIT_A" show-ref --verify -q refs/tags/v0.1
git --git-dir="$GIT_B" show-ref --verify -q refs/tags/v0.1
# HEAD dos dois bare apontando para branch real (clones com checkout funcional).
SISTER_GIT_ROOT="$TMP/node-a" "$CLI" head sister-git main >/dev/null
SISTER_GIT_ROOT="$TMP/node-b" "$CLI" head sister-git main >/dev/null

# Peer avança; nó A deve fazer fast-forward na branch local, sem força.
git -C "$TMP/work" -c user.name=Lab -c user.email=lab@example.invalid commit --allow-empty -qm b-next
git -C "$TMP/work" push -q "$GIT_B" main
SISTER_GIT_ROOT="$TMP/node-a" "$HA" sync sister-git "$TMP/node-b"
[[ "$(git --git-dir="$GIT_B" rev-parse refs/heads/main)" == "$(git --git-dir="$GIT_A" rev-parse refs/heads/main)" ]]

# Local avança, B deve acompanhar; branch criada apenas em B deve ser incorporada.
git -C "$TMP/work" -c user.name=Lab -c user.email=lab@example.invalid commit --allow-empty -qm a-next
git -C "$TMP/work" push -q "$GIT_A" main
git -C "$TMP/work" checkout -q -b server-only
git -C "$TMP/work" push -q "$GIT_B" server-only
SISTER_GIT_ROOT="$TMP/node-a" "$HA" sync sister-git "$TMP/node-b"
[[ "$(git --git-dir="$GIT_B" rev-parse refs/heads/main)" == "$(git --git-dir="$GIT_A" rev-parse refs/heads/main)" ]]
[[ "$(git --git-dir="$GIT_B" rev-parse refs/heads/server-only)" == "$(git --git-dir="$GIT_A" rev-parse refs/heads/server-only)" ]]
# Execução também deve funcionar iniciada pelo segundo nó.
SISTER_GIT_ROOT="$TMP/node-b" "$HA" sync sister-git "$TMP/node-a"
[[ -z "$(git --git-dir="$GIT_A" for-each-ref --format='%(refname)' refs/sister-git-ha)" ]]
[[ -z "$(git --git-dir="$GIT_B" for-each-ref --format='%(refname)' refs/sister-git-ha)" ]]

# Históricos separados nos dois bare não podem ser reconciliados automaticamente.
git -C "$TMP/work" checkout -q main
git -C "$TMP/work" -c user.name=Lab -c user.email=lab@example.invalid commit --allow-empty -qm only-a
git -C "$TMP/work" push -q "$GIT_A" main
# Commit do lado B a partir do estado anterior (não apagar históricos).
mkdir -p "$TMP/independent"
git -C "$TMP/independent" init -q -b main
git -C "$TMP/independent" fetch -q "$GIT_B" main
git -C "$TMP/independent" checkout -q -B main FETCH_HEAD
git -C "$TMP/independent" -c user.name=Lab -c user.email=lab@example.invalid commit --allow-empty -qm only-b
git -C "$TMP/independent" push -q "$GIT_B" main
old_a="$(git --git-dir="$GIT_A" rev-parse refs/heads/main)"
old_b="$(git --git-dir="$GIT_B" rev-parse refs/heads/main)"
if SISTER_GIT_ROOT="$TMP/node-a" "$HA" sync sister-git "$TMP/node-b" >"$TMP/divergence.log" 2>&1; then
  echo '[ERRO] aceitou branches divergentes!' >&2; exit 1
fi
[[ "$(git --git-dir="$GIT_A" rev-parse refs/heads/main)" == "$old_a" ]]
[[ "$(git --git-dir="$GIT_B" rev-parse refs/heads/main)" == "$old_b" ]]

# Nó B indisponível: nó A continua capaz de selecionar bare local.
mv "$GIT_B" "$TMP/node-b/sister-git.offline"
[[ "$(SISTER_GIT_ROOT="$TMP/node-a" "$HA" select sister-git "$TMP/node-b")" == "$GIT_A" ]]
SISTER_GIT_ROOT="$TMP/node-a" "$HA" clone sister-git "$TMP/node-b" "$TMP/clone-offline" >"$TMP/clone.log" 2>&1
[[ -d "$TMP/clone-offline/.git" ]]
[[ "$(git -C "$TMP/clone-offline" branch --show-current)" == main ]]
# Nó local indisponível, mas B disponível: fallback remoto acessível.
mv "$TMP/node-b/sister-git.offline" "$GIT_B"
[[ "$(SISTER_GIT_ROOT="$TMP/no-local" "$HA" select sister-git "$TMP/node-b")" == "$GIT_B" ]]
# Reverse failover: A indisponível, B atende a operação sozinho.
mv "$GIT_A" "$TMP/node-a/sister-git.offline"
[[ "$(SISTER_GIT_ROOT="$TMP/node-b" "$HA" select sister-git "$TMP/node-a")" == "$GIT_B" ]]
mv "$TMP/node-a/sister-git.offline" "$GIT_A"
if SISTER_GIT_ROOT="$TMP/no-local" "$HA" select sister-git "$TMP/no-peer" >/dev/null 2>&1; then
  echo '[ERRO] aceitou ambos os nós indisponíveis!' >&2; exit 1
fi
if SISTER_GIT_ROOT="$TMP/node-a" "$HA" select '../escape' "$TMP/node-b" >/dev/null 2>&1; then
  echo '[ERRO] aceitou nome inválido!' >&2; exit 1
fi

git --git-dir="$GIT_A" fsck --full >/dev/null
git --git-dir="$GIT_B" fsck --full >/dev/null
printf '[OK] HA: dois nós, sincronização nos dois sentidos, fallback, divergência bloqueada e fsck.\n'
