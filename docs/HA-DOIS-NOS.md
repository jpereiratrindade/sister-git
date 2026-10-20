# PATCH-003 — dois nós SisTer-Git (laboratório)

**Princípio:** Sempre pronto. Sempre incompleto!

**Objetivo restrito:** o **mesmo SisTer-Git instalado nos dois computadores**, com Git bare independente em cada um. Se um nó estiver desligado, o outro **continua atendendo Git** onde for alcançável. Seleção de endpoint é realizada **a cada comando `select`/`clone`**. Não há daemon HA, VIP, eleição, consenso ou roteamento transparente de clientes nesta etapa.

## Como funciona

| Função | Silverblue (SB) | Fedora Server (labeco-srv) |
|---|---|---|
| Executáveis | sister-git, sister-git-redundancy, sister-git-ha | **os mesmos três** |
| Repositório `sister-git` | `/srv/git/sister-git.git` (bare; diferente da pasta de desenvolvimento) | `/srv/git/sister-git.git` (bare) |
| Operações Git | `git` local + SSH já existente | `git` local + SSH já existente |
| Se outro nó cai | local continua operando | local continua operando |
| Replicação | `sister-git-ha sync` **quando ambos estão acessíveis** | `sister-git-ha sync` **quando ambos estão acessíveis** |

`/run/media/jpereiratrindade/labeco10T/dev/cpp/sister-git` é o **diretório de trabalho** no SB. `/srv/git/sister-git.git` é o **bare de hospedagem**, em cada computador. Não confundir os dois.

## 1. Atualizar no Silverblue

Após aplicar o PATCH-003 e passar os testes:

```bash
cd /run/media/jpereiratrindade/labeco10T/dev/cpp/sister-git
bash tests/test_sister_git.sh
bash tests/test_redundancy.sh
bash tests/test_ha.sh
bash install.sh                 # solicita sudo; instala /usr/local/bin e cria /srv/git se faltar
sister-git init sister-git     # cria bare independente no SB, NÃO altera a pasta dev/cpp
```

**Cuidado:** execute `sister-git init sister-git` **somente após** confirmar que `/srv/git` é o local desejado; se já houver um bare com esse nome, será preservado.

O IP do servidor que já testamos é `10.163.80.129`:

```bash
export GIT_SSH_COMMAND="ssh -i $HOME/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=5"
PEER='jpereiratrindade@10.163.80.129:/srv/git'
sister-git-ha status sister-git "$PEER"
sister-git-ha sync sister-git "$PEER"     # importa main/tags do labeco-srv ao bare do SB
sister-git head sister-git main           # HEAD do bare local deve apontar para uma branch existente
sister-git-ha status sister-git "$PEER"
```

Se o peer cair, `sister-git-ha select sister-git "$PEER"` ainda retornará `/srv/git/sister-git.git` e permitirá clonar localmente. Esse fallback **não seleciona necessariamente a cópia mais recente**; consulte os logs do `sync` antes de operações de escrita após uma interrupção.

## 2. Atualizar no labeco-srv (obrigatório — instalar nos dois)

A partir do terminal **SB**, transfira os arquivos novos para a instalação existente:

```bash
cd /run/media/jpereiratrindade/labeco10T/dev/cpp/sister-git
scp -i ~/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes \
  bin/sister-git-ha jpereiratrindade@10.163.80.129:~/sister-git/bin/
scp -i ~/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes \
  install.sh jpereiratrindade@10.163.80.129:~/sister-git/
ssh -i ~/.ssh/id_ed25519_labeco -o IdentitiesOnly=yes \
  jpereiratrindade@10.163.80.129
```

Dentro do **labeco-srv**:

```bash
cd ~/sister-git
bash install.sh                 # instalador corrigido: funciona quando invocado por bash
sister-git list                # MORFO.git e sister-git.git permanecem
sister-git info sister-git
sister-git-ha help
```

Para o servidor acessar o Silverblue, configure **SSH independente na direção inversa**: `sshd` funcionando no SB, endereço alcançável pela rede, chave **pública** autorizada no SB e validação de host. Não copie chaves privadas entre computadores. Descubra o endereço real do SB antes de substituir `ENDERECO_DO_SB` no exemplo:

```bash
PEER='jpereiratrindade@ENDERECO_DO_SB:/srv/git'
sister-git-ha status sister-git "$PEER"
sister-git-ha sync sister-git "$PEER"    # só quando o SSH reverso estiver funcionando
```

Se SSH reverso ainda não estiver pronto, o SisTer-Git do servidor **continua operando localmente**. Apenas a replicação iniciada pelo servidor ficará pendente. Na primeira etapa, o `sync` iniciado pelo SB já pode copiar dados para ambos os bare.

## 3. Simular queda **sem desligar** equipamentos

No SB, com os dois bare já sincronizados, selecione um destino:

```bash
PEER='jpereiratrindade@10.163.80.129:/srv/git'
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
