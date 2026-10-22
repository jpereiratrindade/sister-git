# SisTer Git Web & Editorial

Interface web integrada diretamente aos repositórios bare existentes em `/srv/git`, combinando visualização de código com governança editorial e proveniência documental institucional (C++26 + Frontend Vanilla puro).

---

## 1. Principais Recursos

- **Navegação de Código e Repositórios**: árvore de diretórios, blob com syntax highlighting/numeração de linhas, histórico de commits com paginação e diffs coloridos unificados.
- **Módulo Editorial e Proveniência**:
  - Upload multiformato de manuscritos: Word (`.docx`), LibreOffice (`.odt`), LaTeX (`.tex`), Markdown (`.md`), texto puro (`.txt`) e relatórios em PDF.
  - Download raw de artefatos preservando integridade de binários e formatos abertos.
  - Rastreabilidade de ciclo de vida documental em 5 estágios: *Elaboração*, *Submissão*, *Avaliação por Pares*, *Ajustes Requeridos* e *Publicado / Homologado*.
  - Emissão de pareceres técnicos com tags imutáveis anotadas (`ed/vX.Y`).
- **Autenticação Segura sem Senhas Fixas**:
  - Código único temporário (OTP via SMTP institucional) enviado diretamente ao e-mail institucional do autor.
  - Permite bind em rede (`0.0.0.0`) mantendo controle de acesso por allowlist de pesquisadores cadastrados.
- **Deploy de Produção via systemd**: execução permanente como daemon resiliente, independente de sessões SSH abertas.

---

## 2. Compilar e Testar Localmente

Requer compilador com suporte a C++26 (GCC 14+), SQLite3 e `make`:

```bash
make
make test
```

Execução em modo de desenvolvimento:
```bash
SISTER_GIT_ROOT=/srv/git \
SISTER_GIT_WEB_ROOT="$PWD/web" \
./build/sister-git-web --bind 127.0.0.1 --port 8787
```

---

## 3. Operação em Produção com systemd

O SisTer Git Web roda como serviço nativo de sistema no nó de hospedagem (`labeco-srv`):

1. **Variáveis de Ambiente (`/etc/sister-git-web.env`, `chmod 600`)**:
   ```bash
   SISTER_GIT_ROOT=/srv/git
   SISTER_GIT_WEB_ROOT=/usr/local/share/sister-git/web
   SISTER_BIND=0.0.0.0
   SISTER_PORT=8787
   SISTER_SMTP_USER=seu-email@gmail.com
   SISTER_SMTP_PASS=sua-senha-de-app
   SISTER_SMTP_FROM=seu-email@gmail.com
   SISTER_SMTP_HOST=smtp.gmail.com
   SISTER_SMTP_PORT=587
   ```

2. **Unidade do systemd (`/etc/systemd/system/sister-git-web.service`)**:
   Ver modelo em [`deploy/systemd/sister-git-web.service`](../deploy/systemd/sister-git-web.service) e [`deploy/systemd/10-paths.conf`](../deploy/systemd/10-paths.conf).

3. **Gerenciamento do Serviço**:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable --now sister-git-web.service
   systemctl status sister-git-web.service
   ```

---

## 4. Endpoints da API REST (v1)

- `GET /api/v1/health`: status da aplicação, suporte a C++26 e status do SMTP.
- `GET /api/v1/repos`: listagem de repositórios bare públicos e privados autorizados.
- `GET /api/v1/repos/{nome}/refs`: branches e tags do repositório.
- `GET /api/v1/repos/{nome}/tree?ref=HEAD&path=`: árvore de diretórios.
- `GET /api/v1/repos/{nome}/blob?ref=HEAD&path=...`: conteúdo textual com codificação UTF-8.
- `GET /api/v1/repos/{nome}/raw?ref=HEAD&path=...`: download binário do arquivo original.
- `GET /api/v1/repos/{nome}/commits?ref=HEAD&page=1`: histórico de commits.
- `GET /api/v1/repos/{nome}/compare?base=...&head=...`: comparação e diff entre revisões.
- `GET /api/v1/repos/{nome}/editorial`: histórico editorial consolidado e ciclo documental.
- `POST /api/v1/repos/{nome}/upload`: upload de manuscrito multiformato com commit automático.
- `POST /api/v1/auth/request-code`: solicitação de código OTP por e-mail institucional.
- `POST /api/v1/auth/verify-code`: validação de OTP e emissão de sessão autenticada.

