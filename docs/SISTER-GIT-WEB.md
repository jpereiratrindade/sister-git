# SisTer Git Web

Interface web de consulta, integrada diretamente aos repositórios bare existentes. Não altera o transporte Git/SSH e não oferece rotas de escrita.

## Compilar e executar

Requer C++17, Git e `make`:

```bash
make
SISTER_GIT_ROOT=/srv/git \
SISTER_GIT_WEB_ROOT="$PWD/web" \
./build/sister-git-web
```

Acesse `http://127.0.0.1:8787`. O bind local é o padrão seguro.

Para acesso na rede autorizada, defina um segredo longo e use um proxy HTTPS que envie o cabeçalho `Authorization: Bearer ...`:

```bash
SISTER_GIT_WEB_TOKEN='segredo-longo' ./build/sister-git-web --bind 0.0.0.0 --port 8787
```

O servidor recusa bind fora de loopback sem `SISTER_GIT_WEB_TOKEN`. O token não substitui HTTPS: sem TLS, ele trafega em texto claro. Não exponha o serviço à internet.

## API v1 somente leitura

- `GET /api/v1/health`
- `GET /api/v1/repos`
- `GET /api/v1/repos/{nome}/refs`
- `GET /api/v1/repos/{nome}/tree?ref=HEAD&path=`
- `GET /api/v1/repos/{nome}/blob?ref=HEAD&path=README.md`
- `GET /api/v1/repos/{nome}/commits?ref=HEAD`
- `GET /api/v1/repos/{nome}/compare?base=main~1&head=main`
- `GET /api/v1/ha`

Entradas são validadas. Git é chamado com `fork`/`execvp` e vetores de argumentos, nunca por interpolação em shell. Arquivos têm limite de saída de 4 MiB. POST e demais métodos são recusados.

O endpoint de HA informa `not_verified` até existir uma fonte real de telemetria. Autoria registrada em commit é metadado Git, não identidade autenticada.

## Testes e reversão

```bash
make test
```

O teste cria repositórios isolados em `mktemp`; não toca `/srv/git`. Para reverter, pare o processo e remova `build/sister-git-web`. Nenhum repositório, SSH, firewall ou configuração Git é alterado.
