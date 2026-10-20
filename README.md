# SisTer Git — somente repositórios Git

**Versão:** 0.3.0 · **Princípio:** Sempre pronto. Sempre incompleto.

Um serviço mínimo de hospedagem de repositórios do ecossistema SisTer: **Git bare + SSH existente**. Não instala Gitea, GitLab, serviço web, banco de dados, OAuth ou daemon Git. Não substitui GitHub nem GitLab e não tem Pull Requests/Issues/UI. A etapa 2 acrescenta **redundância por Git nativo**. A etapa 3 acrescenta **execução simétrica em dois nós**, com fallback por comando e sincronização fast-forward, preservando a arquitetura mínima.

## Instalar

No **Silverblue**, depois de aplicar o patch em `/run/media/jpereiratrindade/labeco10T/dev/cpp`:

```bash
scp -r sister-git jpereiratrindade@10.163.80.129:~/
```

No **labeco-srv**, na conta `jpereiratrindade`:

```bash
sudo dnf5 install -y git
bash ~/sister-git/install.sh
sister-git init MORFO
sister-git info MORFO
```

O repositório passa a existir em `/srv/git/MORFO.git`. Por padrão somente seu usuário Unix consegue acessá-lo. As chaves SSH já instaladas para ele continuam válidas. **Não confundir controle de Git/SSH com login Google/OIDC do MORFO web.**

## Usar no Silverblue

```bash
cd /run/media/jpereiratrindade/labeco10T/dev/cpp/MORFO
git status -sb
git remote -v
git remote add labeco 'jpereiratrindade@10.163.80.129:/srv/git/MORFO.git'
GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes' git push labeco master
GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes' git ls-remote labeco
```

Se `labeco` já existir: não crie outra entrada; consulte `git remote get-url labeco`. Se a branch for diferente de `master`, identifique-a antes de enviar. Sem force push.

## Redundância Git nativa (Etapa 2)

O computador e o `labeco-srv` agora podem compartilhar branches e tags via `fetch`/`push --atomic`, sem `--force` e **sem exclusão de refs**. O `git bundle --all` permite gerar arquivos independentes com verificação Git e SHA-256. **Não é um espelhamento destrutivo com `git push --mirror`**: divergências interrompem a sincronização e precisam de decisão humana.

No Silverblue, na pasta do projeto SisTer-Git:

```bash
bash tests/test_redundancy.sh
GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes' \
  ./bin/sister-git-redundancy sync /run/media/jpereiratrindade/labeco10T/dev/cpp/MORFO labeco
GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes' \
  ./bin/sister-git-redundancy verify /run/media/jpereiratrindade/labeco10T/dev/cpp/MORFO labeco
```

Se o MORFO usa `main`, depois do primeiro push ajuste o HEAD do servidor com `sister-git head MORFO main`, para que futuros clones encontrem a branch correta.

Os detalhes para instalação, geração de bundles, teste de recuperação e resolução de conflitos estão em **[docs/REDUNDANCIA-GIT.md](docs/REDUNDANCIA-GIT.md)**.

## Dois nós operacionais (PATCH-003)

**O SisTer-Git precisa ser instalado nos dois computadores.** Cada um pode operar
repositórios Git bare em `/srv/git`. O novo `sister-git-ha` seleciona um bare
acessível, clona a partir dele e sincroniza os dois lados quando online.
Apenas disponibilidade é garantida pelo `select`, não a cópia mais recente;
**não há failover automático de escrita nem resolução de split-brain**.

No SB, após instalar nos **dois** nós e criar o bare local:

```bash
export GIT_SSH_COMMAND="ssh -i $HOME/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=5"
PEER=jpereiratrindade@10.163.80.129:/srv/git
sister-git-ha status sister-git "$PEER"
sister-git-ha sync sister-git "$PEER"
sister-git-ha select sister-git "$PEER"
```

Ver **[docs/HA-DOIS-NOS.md](docs/HA-DOIS-NOS.md)** para a instalação em cada máquina,
o sentido reverso de SSH, failover simulado e limites de consistência.

## Outros repositórios

No servidor: `sister-git init NEXO`, `sister-git init SisTer`, `sister-git list`, `sister-git check MORFO`.

## Proteções e limites

- O comando recusa nomes com `/`, `..`, espaços, prefixos com ponto e sufixo `.git` duplicado.
- `git init --bare` é idempotente; nada apaga repositórios existentes.
- `receive.denyNonFastForwards` e `receive.denyDeletes` estão habilitados; reflogs e verificação de objetos recebidos também.
- A pasta `/srv/git` é privada (`0700`) e fica no volume raiz atual, de 15 GiB; não subir blobs volumosos nem arquivos científicos privados.
- **A sincronização Git sozinha não é backup completo**. Bundles em outro disco são uma camada adicional. O HD Hitachi do Dell tem sinais SMART de degradação; manter cópias em `labeco10T` e GitHub quando permitido.
- Nada de expor `git://`, HTTP público ou abrir portas novas. Não subir segredos, tokens OAuth, `.env`, bancos ou dados de pesquisa.
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
