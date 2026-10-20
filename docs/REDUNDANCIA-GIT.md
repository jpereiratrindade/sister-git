# SisTer-Git · Etapa 2 · Redundância Git nativa

**Versão 0.2.0 — laboratório experimental.** Este módulo usa apenas Git e SSH.
Não instala daemon, banco ou sistema de sincronização externo. **Não executa automação periódica nesta etapa** (isso é a etapa 3).

## Topologia proposta

```
Silverblue — /run/media/jpereiratrindade/labeco10T/dev/cpp/MORFO
  | repositório de trabalho, com branches e tags locais
  |  git fetch (refs/remotes/labeco/* + tags)
  |  git push --atomic (refs/heads/* + refs/tags/*; sem force)
  v
labeco-srv — /srv/git/MORFO.git
  | repositório bare privado e protegido por receive.denyDeletes,
  | receive.denyNonFastForwards, receive.fsckObjects e reflogs
  v
Cópias portáteis independentes — git bundle create --all
  | em discos distintos; sha256sum + git bundle verify
  v
Recuperação — git clone arquivo.bundle /destino
```

- O computador é a principal área de desenvolvimento. O servidor Git guarda branches e tags publicadas; não guarda alterações sem commit.
- `sync` primeiro busca branches remotas, sem prune (preservadas nas refs `refs/remotes/labeco/*` do PC), e tags remotas sem forçar mudanças; depois publica branches e tags locais em um único push atômico.
- Branches que só existem no servidor **não são apagadas**. Permanecem no servidor e são acompanhadas no computador em `refs/remotes/labeco/*`. Para trabalhar nelas, criar explicitamente uma branch local.
- Histórico divergente é tratado como **conflito**, não como convite para reescrever histórico. Não são executados `push --force`, `push --mirror`, `fetch --prune`, merges, resets ou limpezas automáticas.
- `verify` confirma os OIDs das branches e tags locais contra o servidor, incluindo a identificação de refs extras. **Não substitui `git fsck --full` em cada local.**
- `git bundle --all` inclui refs Git locais disponíveis, inclusive refs remotas. A verificação em bare vazio confirma que o bundle é independente de objetos pré-existentes. Arquivos ignorados, não commitados, objetos Git LFS externos, dados de submódulos, configuração e chaves privadas NÃO são copiados.

## 1. Aplicar atualização no PC

Dentro da pasta `sister-git`, depois de aplicar o patch, rode:

```bash
bash tests/test_sister_git.sh
bash tests/test_redundancy.sh
```

No Silverblue, execute a partir da própria pasta do código, sem precisar instalar nada no host:

```bash
# Ajuste a pasta se necessário
SG=/run/media/jpereiratrindade/labeco10T/dev/cpp/sister-git
chmod +x "$SG/bin/sister-git-redundancy"
"$SG/bin/sister-git-redundancy" help
```

## 2. Atualizar o comando no labeco-srv

A partir do computador (ajuste IP/nome se mudou):

```bash
scp -r /run/media/jpereiratrindade/labeco10T/dev/cpp/sister-git \
  jpereiratrindade@10.163.80.129:~/
```

No servidor:

```bash
cd ~/sister-git
bash install.sh                      # instala os dois comandos em /usr/local/bin
sister-git init MORFO               # idempotente; não apaga repositórios
sister-git info MORFO
sister-git check MORFO
```

**Atenção:** o servidor foi apresentado anteriormente como `10.163.80.129`, mas o IP deve ser confirmado antes de executar o `scp` ou alterar um `remote`. A conectividade e a disponibilidade dos discos não foram validadas neste pacote.

## 3. Sincronizar MORFO do PC para o labeco-srv

No Silverblue:

```bash
SG=/run/media/jpereiratrindade/labeco10T/dev/cpp/sister-git
MORFO=/run/media/jpereiratrindade/labeco10T/dev/cpp/MORFO
cd "$MORFO"
git status -sb
git branch --show-current
# Se o remote já existe, não repetir; confira com git remote -v
GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes' \
  "$SG/bin/sister-git-redundancy" sync "$MORFO" labeco
GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes' \
  "$SG/bin/sister-git-redundancy" verify "$MORFO" labeco
```

Se `labeco` não estiver configurado, cadastre a URL real apenas uma vez:

```bash
git remote add labeco 'jpereiratrindade@10.163.80.129:/srv/git/MORFO.git'
```

Se `GIT_SSH_COMMAND` estiver configurado via shell, script do sistema ou `~/.ssh/config`, não é necessário repeti-lo. O `-i ~/.ssh/...` é resolvido pelo shell que o SSH executa; caso a chave não esteja no local indicado, ajuste o caminho. Não altere o remoto sem validar o IP atual.

**Corrigir HEAD do servidor depois do primeiro push** (exemplo para branch `main`):

```bash
ssh -i ~/.ssh/id_ed25519_labeco jpereiratrindade@10.163.80.129 \
  'sister-git head MORFO main'
```

Isso só altera a branch padrão anunciada ao clonar, sem reescrever commits. Se usa `master`, mantenha `master`.

Para trabalhar em branch criada no servidor:

```bash
git fetch labeco
git switch --track -c server-only labeco/server-only
```

## 4. Criar terceiro exemplar recuperável (Git bundle)

Escolha **outro disco físico** para os bundles, não uma pasta dentro do repositório. Crie o diretório de destino (uma vez) e rode:

```bash
SG=/run/media/jpereiratrindade/labeco10T/dev/cpp/sister-git
mkdir -p "$HOME/sister-git-bundles"   # EXEMPLO, troque por disco distinto
"$SG/bin/sister-git-redundancy" bundle \
  /run/media/jpereiratrindade/labeco10T/dev/cpp/MORFO \
  "$HOME/sister-git-bundles"
```

No servidor, também é possível criar bundle de um bare:

```bash
mkdir -p "$HOME/sister-git-bundles"   # idealmente disco externo confiável
sister-git backup MORFO "$HOME/sister-git-bundles"
```

Faça verificação de checksum e recuperação experimental:

```bash
cd "$HOME/sister-git-bundles"
sha256sum -c MORFO-*.bundle.sha256   # para cada bundle do lote
# escolha um arquivo específico no teste real:
git clone MORFO-AAAAmmddTHHMMSSZ-HASH.bundle "$HOME/MORFO-restaurado-teste"
```

`clone` normalmente restaura as branches e tags publicadas. Se o bundle contiver refs especiais (por exemplo `refs/remotes/labeco/server-only`), elas poderão precisar ser buscadas explicitamente após o clone. Confira com `git bundle list-heads ARQUIVO.bundle`.

## 5. Falhas e interpretação

| Condição | Comportamento |
|---|---|
| Computador não chega ao servidor | `sync` falha; repositório local intacto |
| Servidor tem commit exclusivo em `main` | `fetch` recebe `labeco/main`; `push` rejeita não-fast-forward |
| Servidor tem branch sem equivalente local | `fetch` cria `labeco/NOME`; branch do servidor preservada |
| Tags divergentes | `fetch` ou `push` falha; não sobrescreve |
| Branch foi removida do computador | A branch no servidor é preservada |
| Há arquivos não commitados | Aviso: esses arquivos NÃO são replicados |
| Bundle não contém todos os objetos necessários | Verificação em repositório vazio falha e pacote não é publicado |
| Disco original e servidor estão no mesmo local físico | **Não há independência física**; criar bundle em terceiro disco |

## 6. Limites e evolução

O espelho é **não destrutivo**, e não bit-a-bit. O computador e o servidor podem conter conjuntos diferentes de refs; `verify` identifica quando a cópia da origem está presente no remoto e avisa sobre refs extras. Não é um espelho estrito via `git push --mirror` — a semântica estrita poderia excluir refs válidas, incompatível com a proteção atual.

O SisTer-Git **não substitui backup de arquivos de trabalho, imagens, bancos de dados, chaves SSH, objetos Git LFS ou submódulos**. O servidor anteriormente relatado tem pouco espaço disponível no disco raiz e indícios de degradação SMART: não confiar exclusivamente nele.

**Etapa 3 (futura):** systemd user timer e inventário de repositórios, locks, relatório de execução, retenção configurável de bundles e alerta de falha.

**Etapa 4 (futura):** leitura de status no SisTer, sem credenciais de escrita incorporadas ao navegador.

**Etapa 5 (futura):** eventos/hooks Git, commit/tag signing, Git protocol v2, interoperabilidade NEXO/CRIVO e MCP, com segregação de privilégios.
