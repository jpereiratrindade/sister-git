#!/usr/bin/env bash
# Instala/atualiza SisTer-Git nos DOIS nós sem sobrescrever repositórios Git.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
if [[ "${EUID}" -ne 0 ]]; then
  # Caminho absoluto evita o erro: sudo: install.sh: comando não encontrado.
  exec sudo -- /usr/bin/env bash "$SCRIPT_DIR/install.sh" "$@"
fi
owner="${SUDO_USER:-}"
[[ -n "$owner" && "$owner" != root ]] || {
  echo '[ERRO] Execute como usuário normal: bash install.sh (usa sudo internamente)' >&2; exit 1;
}
group="$(id -gn "$owner")"
command -v git >/dev/null || {
  echo '[ERRO] Git não instalado. Execute: sudo dnf5 install -y git' >&2; exit 1;
}
if [[ -L /srv/git ]]; then
  echo '[ERRO] /srv/git é link simbólico: instalação recusada' >&2; exit 1
fi
if [[ -e /srv/git && ! -d /srv/git ]]; then
  echo '[ERRO] /srv/git não é diretório' >&2; exit 1
fi
if [[ ! -e /srv/git ]]; then
  install -d -o "$owner" -g "$group" -m 0700 /srv/git
else
  # Não trocar proprietário/permissões de uma instalação já existente.
  if ! sudo -u "$owner" -- test -r /srv/git || ! sudo -u "$owner" -- test -w /srv/git; then
    echo '[ERRO] Usuário Git sem leitura/escrita em /srv/git (não alterei permissões).' >&2
    exit 1
  fi
fi
install -o root -g root -m 0755 "$SCRIPT_DIR/bin/sister-git" /usr/local/bin/sister-git
install -o root -g root -m 0755 "$SCRIPT_DIR/bin/sister-git-redundancy" /usr/local/bin/sister-git-redundancy
install -o root -g root -m 0755 "$SCRIPT_DIR/bin/sister-git-ha" /usr/local/bin/sister-git-ha
printf '[OK] SisTer-Git + HA instalados/atualizados neste nó; usuário inicial: %s\n' "$owner"
printf '[INFO] Nenhum repositório foi removido; SSH/firewall/systemd não foram alterados.\n'
