# Integração do SisTer Git com LABECO-ID

Nesta arquitetura, mantemos **separação estrita entre domínios de confiança**:

1. **Camada Git Bare / SSH (Sistema Operacional)**:
   - A autenticidade e autorização dos comandos `git push`, `fetch` e clonagem direta via terminal são controladas **exclusivamente pelas chaves SSH autorizadas** na conta Unix do servidor (`~/.ssh/authorized_keys`).
   - Não há integração OIDC nem senhas de banco de dados no transporte Git bare nativo. Essa escolha é intencional: o SisTer Git preserva o protocolo Git em seu estado mais limpo e soberano.

2. **Camada Web & Editorial (Interface MORFO)**:
   - A interface web utiliza **autenticação passwordless via One-Time Password (OTP)**.
   - O pesquisador informa seu e-mail institucional (ex.: `@embrapa.br`).
   - O backend C++26 valida o e-mail contra a **allowlist de pesquisadores autorizados** (`/srv/git/.allowed_users` ou configuração interna).
   - Um código numérico temporário (6 dígitos) é gerado e despachado diretamente pelo servidor SMTP institucional.
   - Uma sessão web autenticada permite criar repositórios, subir manuscritos multiformato (Word, ODT, LaTeX, MD), emitir pareceres e publicar edições.

3. **Invariante Fundamental de Segurança**:
   - **Uma sessão web nunca concede, por si só, acesso shell nem privilégios de superusuário no servidor.**
   - O registro de autoria em commits (`git config user.email`) é metadado documental do Git, enquanto a autenticação OTP garante que apenas pesquisadores autorizados operem a interface editorial na rede.
   - Para proveniência jurídica plena, edições publicadas recebem tags anotadas (`ed/vX.Y`) preservadas na árvore Merkle.

