# Governança leve do sister-git

Esta política liga código, serviço implantado e evidência operacional sem apagar a autonomia experimental do projeto.

## Fluxo mínimo

Uma mudança relevante usa Issue, branch e Pull Request. A Issue registra problema, resultado esperado, impacto de compatibilidade, critérios de aceite e evidências. O PR registra comandos e resultados, versão e situação da implantação.

## Estados independentes

- Engenharia: `proposto` → `implementado` → `testado` → `publicado`.
- Ciência: `não se aplica`, `hipótese`, `verificado computacionalmente`, `validado empiricamente` ou `limitado/refutado`.

## Definição de concluído

Issue e PR estão ligados; critérios de aceite foram satisfeitos; `make test` passou; suítes adicionais afetadas foram executadas; compatibilidade foi analisada; documentação e limitações foram atualizadas; e, se houve implantação, ambiente, versão e commit foram registrados.

## Três identidades de versão

1. `VERSION` é a fonte da versão do código.
2. O serviço MCP anuncia exatamente essa versão no artefato compilado.
3. Cada implantação registra ambiente, versão e commit; ela pode permanecer atrás do código, mas a diferença deve ser visível.

Tags usam `vMAJOR.MINOR.PATCH`. Versões de esquema editorial ou de documentos são independentes da versão do aplicativo.

## Etiquetas recomendadas

Criar no GitHub: `status:proposto`, `status:implementado`, `status:testado`, `status:publicado`, `tipo:bug`, `tipo:funcionalidade`, `tipo:operacao`, `compatibilidade`, `implantacao`, `ciencia:hipotese`, `ciencia:computacional`, `ciencia:validado`, `ciencia:limitado` e `bloqueado`.

