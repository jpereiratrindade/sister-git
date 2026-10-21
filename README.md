# SisTer Git — somente repositórios Git

**Versão:** 0.3.0 · **Princípio:** Sempre pronto. Sempre incompleto.

Um serviço mínimo de hospedagem de repositórios do ecossistema SisTer: **Git bare + SSH existente**. Não instala Gitea, GitLab, banco de dados, OAuth ou daemon Git. A etapa 2 acrescenta **redundância por Git nativo**. A etapa 3 acrescenta **execução simétrica em dois nós**, com fallback por comando e sincronização fast-forward. A etapa web acrescenta uma interface própria de consulta, sem substituir o Git/SSH nem modificar repositórios.

## Interface web (primeira entrega)

O SisTer Git Web oferece dashboard, navegação de código, commits, branches, tags e comparação entre revisões. Backend C++17 sem dependências externas; API versionada e somente leitura. Veja **[docs/SISTER-GIT-WEB.md](docs/SISTER-GIT-WEB.md)**.

```bash
make
SISTER_GIT_ROOT=/srv/git SISTER_GIT_WEB_ROOT="$PWD/web" ./build/sister-git-web
```

## Instalar

No nó local (ou após clonar o repositório):

```bash
scp -r sister-git usuario@servidor:~/
```

No servidor remoto (ex.: nó de hospedagem), na conta do usuário:

```bash
sudo dnf5 install -y git
bash ~/sister-git/install.sh
sister-git init MEUPROJETO
sister-git info MEUPROJETO
```

O repositório passa a existir em `/srv/git/MEUPROJETO.git`. Por padrão somente seu usuário Unix consegue acessá-lo. As chaves SSH já instaladas para ele continuam válidas. **Não confundir controle de Git/SSH com autenticação em camada de aplicação web.**

## Usar a partir do cliente/computador de desenvolvimento

```bash
cd /caminho/para/seu/projeto
git status -sb
git remote -v
git remote add labeco 'usuario@servidor:/srv/git/MEUPROJETO.git'
GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519 -o IdentitiesOnly=yes' git push labeco master
GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519 -o IdentitiesOnly=yes' git ls-remote labeco
```

Se o remoto `labeco` já existir: não crie outra entrada; consulte `git remote get-url labeco`. Se a branch for diferente de `master`, identifique-a antes de enviar. Sem force push.

## Redundância Git nativa (Etapa 2)

O computador e o servidor agora podem compartilhar branches e tags via `fetch`/`push --atomic`, sem `--force` e **sem exclusão de refs**. O `git bundle --all` permite gerar arquivos independentes com verificação Git e SHA-256. **Não é um espelhamento destrutivo com `git push --mirror`**: divergências interrompem a sincronização e precisam de decisão humana.

Na pasta do projeto SisTer-Git:

```bash
bash tests/test_redundancy.sh
GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519 -o IdentitiesOnly=yes' \
  ./bin/sister-git-redundancy sync /caminho/para/seu/projeto labeco
GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519 -o IdentitiesOnly=yes' \
  ./bin/sister-git-redundancy verify /caminho/para/seu/projeto labeco
```

Se o projeto usa `main`, depois do primeiro push ajuste o HEAD do servidor com `sister-git head MEUPROJETO main`, para que futuros clones encontrem a branch correta.

Os detalhes para instalação, geração de bundles, teste de recuperação e resolução de conflitos estão em **[docs/REDUNDANCIA-GIT.md](docs/REDUNDANCIA-GIT.md)**.

## Dois nós operacionais (PATCH-003)

**O SisTer-Git precisa ser instalado nos dois computadores.** Cada um pode operar
repositórios Git bare em `/srv/git`. O novo `sister-git-ha` seleciona um bare
acessível, clona a partir dele e sincroniza os dois lados quando online.
Apenas disponibilidade é garantida pelo `select`, não a cópia mais recente;
**não há failover automático de escrita nem resolução de split-brain**.

Após instalar nos **dois** nós e criar o bare local:

```bash
export GIT_SSH_COMMAND="ssh -i $HOME/.ssh/id_ed25519 -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=5"
PEER=usuario@servidor:/srv/git
sister-git-ha status sister-git "$PEER"
sister-git-ha sync sister-git "$PEER"
sister-git-ha select sister-git "$PEER"
```

Ver **[docs/HA-DOIS-NOS.md](docs/HA-DOIS-NOS.md)** para a instalação em cada máquina,
o sentido reverso de SSH, failover simulado e limites de consistência.

## Outros repositórios

No servidor: `sister-git init PROJETO_A`, `sister-git init PROJETO_B`, `sister-git list`, `sister-git check PROJETO_A`.

## Proteções e limites

- O comando recusa nomes com `/`, `..`, espaços, prefixos com ponto e sufixo `.git` duplicado.
- `git init --bare` é idempotente; nada apaga repositórios existentes.
- `receive.denyNonFastForwards` e `receive.denyDeletes` estão habilitados; reflogs e verificação de objetos recebidos também.
- A pasta `/srv/git` é privada (`0700`) e fica no volume raiz configurado; não subir blobs volumosos nem arquivos sigilosos desprotegidos.
- **A sincronização Git sozinha não é backup completo**. Bundles em outro disco são uma camada adicional. Manter cópias independentes em discos distintos e remotos secundários.
- Nada de expor `git://`, HTTP público sem controle ou abrir portas novas. Não subir segredos, tokens OAuth, `.env`, bancos ou dados sensíveis de pesquisa.
- Por simplicidade, o serviço usa **uma conta Unix existente**. Autorização individual de colaboradores poderá evoluir para contas dedicadas com permissões distintas.
- Crie tags/branches no repositório de trabalho normalmente. O bare remoto guarda histórico, não executa aplicativos.

## Testes locais

```bash
bash tests/test_sister_git.sh
bash tests/test_redundancy.sh
bash tests/test_ha.sh
```

## Reversão

A instalação modifica apenas `/usr/local/bin/sister-git`, `/usr/local/bin/sister-git-redundancy` e `/usr/local/bin/sister-git-ha` e cria/ajusta `/srv/git`. Para remover o comando: `sudo rm /usr/local/bin/sister-git`; mantenha `/srv/git` intacto, pois contém os dados Git. Não apagar repositórios para desinstalar o utilitário.
