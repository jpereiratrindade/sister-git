#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI="$HERE/bin/sister-git"
TEMP="$(mktemp -d)"
trap 'rm -rf -- "$TEMP"' EXIT
export SISTER_GIT_ROOT="$TEMP/repos"
mkdir -p "$SISTER_GIT_ROOT"
"$CLI" init MORFO
"$CLI" init MORFO | grep -q 'preservado'
"$CLI" list >"$TEMP/list.txt"
grep -qx 'MORFO.git' "$TEMP/list.txt"
"$CLI" info MORFO >"$TEMP/info.txt"
grep -q 'refs/heads/master' "$TEMP/info.txt"
for bad in '../bad' 'MORFO/../../tmp' '.hidden' 'x..y' 'a.git'; do
  if "$CLI" init "$bad" >/dev/null 2>&1; then echo "[ERRO] nome inválido aceito: $bad" >&2; exit 1; fi
done
mkdir -p "$TEMP/work"
cd "$TEMP/work"
git init -q --initial-branch=master
git -c user.name=Lab -c user.email=lab@example.test commit --allow-empty -qm 'first'
git remote add labeco "$SISTER_GIT_ROOT/MORFO.git"
git push -q labeco master
"$CLI" check MORFO
[[ "$(git --git-dir="$SISTER_GIT_ROOT/MORFO.git" rev-parse refs/heads/master)" == "$(git rev-parse HEAD)" ]]
git -c user.name=Lab -c user.email=lab@example.test commit --allow-empty -qm 'second'
git push -q labeco master
git reset -q --hard HEAD~1
if git push -q --force labeco master >"$TEMP/rejected.log" 2>&1; then
  echo '[ERRO] Force-push deveria ser negado' >&2; exit 1
fi
if git push -q labeco --delete master >"$TEMP/delete.log" 2>&1; then
  echo '[ERRO] Exclusão de branch deveria ser negada' >&2; exit 1
fi
"$CLI" check MORFO
printf '[OK] Testes SisTer Git: init idempotente, push, fsck, force-push e delete bloqueados\n'
