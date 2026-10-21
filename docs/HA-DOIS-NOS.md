# PATCH-003 — dois nós SisTer-Git (laboratório)

**Princípio:** Sempre pronto. Sempre incompleto!

**Objetivo restrito:** o **mesmo SisTer-Git instalado nos dois computadores**, com Git bare independente em cada um. Se um nó estiver desligado, o outro **continua atendendo Git** onde for alcançável. Seleção de endpoint é realizada **a cada comando `select`/`clone`**. Não há daemon HA, VIP, eleição, consenso ou roteamento transparente de clientes nesta etapa.

## Como funciona

| Função | Nó Local (Nó 1) | Nó Remoto (Nó 2) |
|---|---|---|
| Executáveis | sister-git, sister-git-redundancy, sister-git-ha | **os mesmos três** |
| Repositório `sister-git` | `/srv/git/sister-git.git` (bare; diferente da pasta de desenvolvimento) | `/srv/git/sister-git.git` (bare) |
| Operações Git | `git` local + SSH já existente | `git` local + SSH já existente |
| Se outro nó cai | local continua operando | local continua operando |
| Replicação | `sister-git-ha sync` **quando ambos estão acessíveis** | `sister-git-ha sync` **quando ambos estão acessíveis** |

`/caminho/para/sister-git` é o **diretório de trabalho**. `/srv/git/sister-git.git` é o **bare de hospedagem**, em cada computador. Não confundir os dois.

## 1. Atualizar no Nó Local

Após passar os testes:

```bash
cd /caminho/para/sister-git
bash tests/test_sister_git.sh
bash tests/test_redundancy.sh
bash tests/test_ha.sh
bash install.sh                 # solicita sudo; instala /usr/local/bin e cria /srv/git se faltar
sister-git init sister-git     # cria bare independente no nó local, NÃO altera a pasta de trabalho
```

**Cuidado:** execute `sister-git init sister-git` **somente após** confirmar que `/srv/git` é o local desejado; se já houver um bare com esse nome, será preservado.

Defina o endpoint do peer (ex.: `usuario@servidor:/srv/git`):

```bash
export GIT_SSH_COMMAND="ssh -i $HOME/.ssh/id_ed25519 -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=5"
PEER='usuario@servidor:/srv/git'
sister-git-ha status sister-git "$PEER"
sister-git-ha sync sister-git "$PEER"     # importa main/tags do peer ao bare local
sister-git head sister-git main           # HEAD do bare local deve apontar para uma branch existente
sister-git-ha status sister-git "$PEER"
```

Se o peer cair, `sister-git-ha select sister-git "$PEER"` ainda retornará `/srv/git/sister-git.git` e permitirá clonar localmente. Esse fallback **não seleciona necessariamente a cópia mais recente**; consulte os logs do `sync` antes de operações de escrita após uma interrupção.

## 2. Atualizar no Nó Remoto (obrigatório — instalar nos dois)

A partir do terminal local, transfira os arquivos novos para a instalação existente:

```bash
cd /caminho/para/sister-git
scp -i ~/.ssh/id_ed25519 -o IdentitiesOnly=yes \
  bin/sister-git-ha usuario@servidor:~/sister-git/bin/
scp -i ~/.ssh/id_ed25519 -o IdentitiesOnly=yes \
  install.sh usuario@servidor:~/sister-git/
ssh -i ~/.ssh/id_ed25519 -o IdentitiesOnly=yes \
  usuario@servidor
```

Dentro do **nó remoto**:

```bash
cd ~/sister-git
bash install.sh                 # instala utilitários em /usr/local/bin
sister-git list                # repositórios existentes permanecem
sister-git info sister-git
sister-git-ha help
```

Para o nó remoto acessar o nó local, configure **SSH independente na direção inversa**: `sshd` funcionando no nó local, endereço alcançável pela rede, chave **pública** autorizada no nó local e validação de host. Não copie chaves privadas entre computadores. Descubra o endereço real antes de substituir `ENDERECO_DO_NO1` no exemplo:

```bash
PEER='usuario@ENDERECO_DO_NO1:/srv/git'
sister-git-ha status sister-git "$PEER"
sister-git-ha sync sister-git "$PEER"    # só quando o SSH reverso estiver funcionando
```

Se SSH reverso ainda não estiver pronto, o SisTer-Git do nó remoto **continua operando localmente**. Apenas a replicação iniciada por ele ficará pendente.

## 3. Simular queda **sem desligar** equipamentos

No nó local, com os dois bare já sincronizados, selecione um destino:

```bash
PEER='usuario@servidor:/srv/git'
sister-git-ha status sister-git "$PEER"
sister-git-ha select sister-git "$PEER"
sister-git-ha clone sister-git "$PEER" /tmp/ha-sister-git-clone
```

Para simular indisponibilidade, use uma URL SSH de teste para host inacessível com timeout curto ou execute o teste isolado automatizado `bash tests/test_ha.sh`, que simula a queda **sem tocar** nos repositórios reais. **Não desligue o servidor antes de validar a cópia local.**

## 4. Segurança e limites

- `sync` importa branches/tags que só existam no par, avança branches apenas por **fast-forward** e envia referências com `push --atomic`. Não usa `--force`, `--mirror` nem exclusões remotas.
- Tags alteradas ou branches divergentes são sinalizadas como **conflito**, sem merge automático. A checagem de divergência ocorre antes de modificar refs reais; uma falha de rede ou corrida durante o push pode deixar a sincronização parcial, que deve ser repetida.
- `status`, `select` e `clone` priorizam bare local; selecionam o peer somente se o bare local não existir. **Não é garantia de frescor dos dados**. Um Git bare local disponível mas atrasado será escolhido.
- Não existe *fencing*, *quorum*, *split-brain prevention*, alta disponibilidade automática de escrita, DNS virtual, proxy ou recuperação automática entre duas falhas. Para escrita automática segura, isso exigirá etapa adicional e testes específicos.
- Commits não enviados, árvore de trabalho, `.env`, arquivos ignorados e configurações externas a Git **não são replicados**.
- Serviços de terceiros, aplicações web e banco de dados não se tornam redundantes só porque Git está replicado.
- A instalação preserva os repositórios e a propriedade da raiz existente; não altera rede, SSH, firewall ou serviços do systemd.

## 5. Testes

```bash
bash tests/test_sister_git.sh
bash tests/test_redundancy.sh
bash tests/test_ha.sh
```

`test_ha.sh` cria DOIS nós temporários, testa replicação nas duas direções, novos refs, fast-forward, conflito, failover de leitura/clone e integridade Git, sem acessar `/srv/git` real.
