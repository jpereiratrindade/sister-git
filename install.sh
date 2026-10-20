#!/usr/bin/env bash
# Instala somente o comando SisTer Git e diretório /srv/git.
# Deve ser executado no labeco-srv por usuário com sudo.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${EUID}" -ne 0 ]]; then
  exec sudo -- "$0" "$@"
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
install -d -o "$owner" -g "$group" -m 0700 /srv/git
install -o root -g root -m 0755 "$SCRIPT_DIR/bin/sister-git" /usr/local/bin/sister-git
install -o root -g root -m 0755 "$SCRIPT_DIR/bin/sister-git-redundancy" /usr/local/bin/sister-git-redundancy
printf '[OK] SisTer Git + redundância Git instalados; dono dos repositórios: %s\n' "$owner"
printf '[INFO] Acesse como esse usuário e execute: sister-git init MORFO\n'
printf '[INFO] Nenhum repositório foi sobrescrito; sshd/firewall não foram alterados.\n'
