# Integração do SisTer Git com LABECO-ID

Nesta versão **não existe integração OIDC** no Git bare. A autenticidade dos pushes é controlada pelas chaves SSH autorizadas na conta Unix do servidor. Essa escolha é intencional: o projeto é apenas um serviço de repositórios.

O LABECO-ID/OIDC será integrado ao **MORFO web**, não ao comando `git push`. Registro de autoria em commits (`git config user.email`) não constitui autenticação. Para atribuição de autoria criptográfica futuramente, avaliar commits/tags assinados e gestão de chaves por usuário; não inferir autoria pela string do e-mail.

Nunca permitir que uma sessão web Google conceda, por si só, acesso shell ou push no servidor. São domínios de confiança distintos.
