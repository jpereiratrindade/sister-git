# SisTer Git Editorial (SGE)

**Gestão editorial, proveniência e evolução documental baseada em Git**  
*Laboratório de Ecologia e Conservação (LABECO) · Outubro de 2026*

> *"Versionar um documento significa registrar o que mudou.*  
> *Gerir um processo editorial significa registrar como, por que, por quem e sob quais critérios o documento evoluiu."*

---

## 1. Visão e Motivação

O **SisTer Git Editorial** estende o núcleo seguro do SisTer Git para a produção, revisão, aprovação e preservação de documentos científicos, relatórios técnicos, livros e protocolos de pesquisa.

Diferente de sistemas de edição colaborativa síncrona (como Google Docs ou Word) ou de versionamento de software puro:
1. **O Git é a base de integridade:** Toda versão é uma árvore de Merkle imutável com hash criptográfico.
2. **Decisões são registradas:** Não basta saber que uma frase mudou; é essencial registrar o parecer que motivou a mudança, a justificativa apresentada pelo autor e a decisão do editor.
3. **Alinhamento com Proveniência (W3C PROV):** Relaciona **Entidades** (versões de documentos, dados, figuras), **Atividades** (submissão, parecer, revisão, aprovação) e **Agentes** (autores, revisores, editores).
4. **Independência de formatos:** Acolhe Markdown, Quarto (`.qmd`), LaTeX (`.tex`), documentos estruturados e relatórios.

---

## 2. Ciclo de Vida Editorial

```mermaid
stateDiagram-v2
    [*] --> Rascunho : Criação do Manuscrito
    Rascunho --> EmRevisao : Submissão para Avaliação
    EmRevisao --> AjustesSolicitados : Parecer Emitido
    AjustesSolicitados --> Rascunho : Incorporação de Melhorias
    EmRevisao --> Aprovado : Decisão Editorial Favorável
    Aprovado --> Publicado : Fixação de Tag Imutável (ed/vX.Y)
    Publicado --> [*] : Preservação Permanente
    Publicado --> Rascunho : Nova Edição / Derivação
```

| Estágio | Identificador | Significado |
| :--- | :--- | :--- |
| **Elaboração** | `draft` | Manuscrito em redação pelos autores. Versões de trabalho. |
| **Em Revisão** | `review` | Versão congelada submetida a pareceristas e colaboradores. |
| **Ajustes** | `revision` | Atendimento das recomendações e registro de respostas. |
| **Aprovado** | `approved` | Decisão formal do editor vinculada a um commit específico. |
| **Publicado** | `published` | Versão final fixada por tag editorial imutável (ex.: `ed/v1.0`). |

---

## 3. Estrutura do Manifesto de Proveniência (`.sister-editorial.json`)

Para que o processo editorial seja imutável e viaje junto com o repositório, cada projeto editorial mantém um manifesto na raiz de sua branch principal:

```json
{
  "$schema": "https://sister-git.labeco.org/schemas/editorial-v1.json",
  "project_type": "editorial",
  "title": "Dinâmica dos Campos Nativos no Bioma Pampa",
  "document_main": "manuscrito.md",
  "abstract": "Estudo sobre a evolução temporal e conservação da vegetação campestre nativa.",
  "authors": [
    { "name": "José Pedro Trindade", "role": "Autor Principal", "affiliation": "Embrapa / LABECO" }
  ],
  "current_stage": "in_review",
  "reviews": [
    {
      "id": "REV-001",
      "commit": "edbd8733abad84a0c584a56a074fd4a9f4cb1b6c",
      "reviewer": "Revisor Convidado 1",
      "date": "2026-10-21T10:00:00-03:00",
      "recommendation": "adjustments",
      "summary": "Recomenda detalhar o método de amostragem no tópico 2 e justificar a exclusão de áreas degradadas.",
      "resolution": {
        "resolved_in_commit": "c3b2764123...",
        "author_response": "Adicionado parágrafo detalhando o delineamento amostral com base em Trindade et al. (2024)."
      }
    }
  ],
  "decisions": [
    {
      "decision_type": "acceptance",
      "editor": "Editor Científico",
      "date": "2026-10-21T11:00:00-03:00",
      "target_commit": "c3b2764123...",
      "notes": "Todas as recomendações do parecer REV-001 foram devidamente sanadas no texto."
    }
  ],
  "editions": [
    {
      "tag": "ed/v1.0",
      "commit": "c3b2764123...",
      "title": "Primeira Edição — Versão Submetida ao Comitê",
      "date": "2026-10-21T12:00:00-03:00"
    }
  ]
}
```

---

## 4. Configuração do Repositório Git

Para identificar um repositório como editorial no SisTer Git:

```bash
# Definir o repositório como editorial no servidor
git --git-dir=/srv/git/PROJETO.git config sister.type editorial

# Ou manter como repositório de software (padrão)
git --git-dir=/srv/git/SOFTWARE.git config sister.type software
```

---

## 5. Integração com o SisTer Git Web

1. **Badge `📖 Editorial`**: Exibido no card do repositório no dashboard inicial.
2. **Aba "Editorial"**: Permite navegar entre o documento formatado, a árvore de decisões, os pareceres históricos e as edições publicadas.
3. **Leitura Acadêmica**: Renderizador focado em legibilidade de textos longos com tipografia editorial.
4. **Rastreabilidade**: Cada parecer ou decisão possui um link direto para o diff e o estado do repositório no momento da avaliação.
