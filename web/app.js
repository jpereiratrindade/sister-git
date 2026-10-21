// SisTer Git Web — Espaço Editorial C++26 & Repositórios Git
// Identidade Visual MORFO · Preservação e Proveniência Documental

const app = document.querySelector('#app');
const sidebarAuth = document.querySelector('#sidebarAuth');
const crumbCurrent = document.querySelector('#crumbCurrent');
const globalSearch = document.querySelector('#globalSearch');

let allReposCache = [];
let currentWorkspace = 'editorial'; // 'editorial' | 'repos' | 'infra'

// ============================================================================
// TOKEN & AUTHENTICATION
// ============================================================================

const getToken = () => localStorage.getItem('sister_token') || '';
const setToken = t => t ? localStorage.setItem('sister_token', t.trim()) : localStorage.removeItem('sister_token');

const api = async (path, options = {}) => {
  const headers = options.headers || {};
  const token = getToken();
  if (token) headers['Authorization'] = 'Bearer ' + token;
  if (options.body && typeof options.body === 'object' && !(options.body instanceof FormData)) {
    headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(options.body);
  }
  options.headers = headers;

  const r = await fetch(path, options);
  if (!r.ok) {
    if (r.status === 401 && token) {
      setToken('');
      updateAuthStatus();
      throw Error('Token inválido ou não autorizado pelo servidor.');
    }
    const err = await r.json().catch(() => ({ error: r.statusText }));
    throw Error(err.error || `Erro HTTP ${r.status}`);
  }
  return r.json();
};

const apiPost = (path, data) => api(path, { method: 'POST', body: data });

async function updateAuthStatus() {
  if (!sidebarAuth) return;
  try {
    const s = await api('/api/v1/auth/status').catch(() => ({ authenticated: false, auth_required: false }));
    if (s.authenticated) {
      sidebarAuth.innerHTML = `
        <div class="auth-badge-status" title="Sessão autenticada no LABECO">
          <span class="dot-auth"></span>
          <span>Pesquisador</span>
        </div>
        <button type="button" class="btn-auth-action" onclick="logout()" title="Encerrar sessão autenticada">Sair</button>
      `;
    } else {
      sidebarAuth.innerHTML = `
        <div class="auth-badge-status" title="Acesso público somente leitura">
          <span class="dot-anon"></span>
          <span>Público</span>
        </div>
        <button type="button" class="btn-auth-action" onclick="openAuthModal()" title="Autenticar como pesquisador">Entrar</button>
      `;
    }
  } catch (_) {
    sidebarAuth.innerHTML = `
      <div class="auth-badge-status"><span class="dot-anon"></span><span>Público</span></div>
    `;
  }
}

window.logout = function() {
  setToken('');
  showToast('Sessão encerrada.');
  updateAuthStatus();
  render();
};

// ============================================================================
// UTILITIES & MARKDOWN RENDERER
// ============================================================================

const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
}[c]));

const fmt = b => {
  if (b === null || b === undefined) return '0 B';
  if (b < 1024) return `${b} B`;
  if (b < 1048576) return `${(b / 1024).toFixed(1)} KiB`;
  return `${(b / 1048576).toFixed(1)} MiB`;
};

const formatDate = s => {
  if (!s) return '—';
  try {
    const d = new Date(s);
    return new Intl.DateTimeFormat('pt-BR', { dateStyle: 'medium', timeStyle: 'short' }).format(d);
  } catch (_) {
    return s;
  }
};

const timeAgo = s => {
  if (!s) return '—';
  try {
    const diff = Math.floor((Date.now() - new Date(s).getTime()) / 1000);
    if (diff < 60) return 'agora mesmo';
    if (diff < 3600) return `há ${Math.floor(diff / 60)} min`;
    if (diff < 86400) return `há ${Math.floor(diff / 3600)} h`;
    if (diff < 2592000) return `há ${Math.floor(diff / 86400)} d`;
    return formatDate(s);
  } catch (_) {
    return formatDate(s);
  }
};

function renderMarkdown(md) {
  if (!md) return '';
  let html = esc(md);

  // Fenced Code blocks
  html = html.replace(/```([a-zA-Z0-9_-]*)\n([\s\S]*?)```/g, (_, lang, code) => {
    return `<pre><code class="language-${lang}">${code}</code></pre>`;
  });

  // Inline code
  html = html.replace(/`([^`]+)`/g, '<code>$1</code>');

  // Headings
  html = html.replace(/^#### (.*$)/gim, '<h4>$1</h4>');
  html = html.replace(/^### (.*$)/gim, '<h3>$1</h3>');
  html = html.replace(/^## (.*$)/gim, '<h2>$1</h2>');
  html = html.replace(/^# (.*$)/gim, '<h1>$1</h1>');

  // Blockquotes
  html = html.replace(/^\> (.*$)/gim, '<blockquote>$1</blockquote>');

  // Horizontal Rule
  html = html.replace(/^---$/gim, '<hr>');

  // Bold & Italic
  html = html.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
  html = html.replace(/\*([^*]+)\*/g, '<em>$1</em>');

  // Unordered list items
  html = html.replace(/^\s*-\s+(.*$)/gim, '<li>$1</li>');
  html = html.replace(/(<li>.*<\/li>)/gim, '<ul>$1</ul>');
  html = html.replace(/<\/ul>\s*<ul>/g, '');

  // Line breaks & paragraphs
  const paragraphs = html.split(/\n\n+/);
  return paragraphs.map(p => {
    p = p.trim();
    if (!p) return '';
    if (p.startsWith('<h') || p.startsWith('<pre') || p.startsWith('<blockquote') || p.startsWith('<ul') || p.startsWith('<hr')) {
      return p;
    }
    return `<p>${p.replace(/\n/g, '<br>')}</p>`;
  }).join('\n');
}

function showToast(msg) {
  const t = document.querySelector('#toast');
  if (!t) return;
  t.textContent = msg;
  t.classList.add('visible');
  setTimeout(() => t.classList.remove('visible'), 3200);
}

// ============================================================================
// SIDEBAR & SHELL CONTROLS
// ============================================================================

window.toggleSidebar = function(force) {
  const s = document.querySelector('#sidebar');
  const scrim = document.querySelector('#mobile-scrim');
  const open = force !== undefined ? force : !s.classList.contains('open');
  s.classList.toggle('open', open);
  scrim.classList.toggle('shown', open);
};

function updateNavHighlight(workspace) {
  currentWorkspace = workspace;
  document.querySelectorAll('.nav-item').forEach(el => el.classList.remove('active'));
  if (workspace === 'editorial') {
    document.querySelector('#nav-editorial')?.classList.add('active');
    crumbCurrent.textContent = 'Espaço Editorial';
  } else if (workspace === 'repos') {
    document.querySelector('#nav-repos')?.classList.add('active');
    crumbCurrent.textContent = 'Repositórios de Código';
  } else if (workspace === 'infra') {
    document.querySelector('#nav-infra')?.classList.add('active');
    crumbCurrent.textContent = 'Infraestrutura & HA';
  }
}

// ============================================================================
// ROUTING
// ============================================================================

function parseRoute() {
  const hash = location.hash.replace(/^#\/?/, '');
  const [routePart, queryPart] = hash.split('?');
  const segs = (routePart || '').split('/').map(decodeURIComponent).filter(Boolean);
  const q = new URLSearchParams(queryPart || '');

  // Cases:
  // #/editorial -> { mode: 'editorial-dashboard' }
  // #/editorial/:repo -> { mode: 'editorial-doc', repo: ':repo', tab: 'manuscript' }
  // #/editorial/:repo/:tab -> { mode: 'editorial-doc', repo: ':repo', tab: ':tab' }
  // #/infra -> { mode: 'infra' }
  // #/ or empty -> { mode: 'repos-dashboard' }
  // #/r/:repo/:tab or #/:repo/:tab -> { mode: 'repo-code', repo, tab, ref, path, base, head }

  // Se for explicitamente repositórios (#/repos ou #/)
  if (segs[0] === 'repos' || location.hash === '#/' || location.hash === '#/repos') {
    return { mode: 'repos-dashboard' };
  }

  // Raiz vazia sem hash ou #/editorial
  if (!segs.length || (segs[0] === 'editorial' && segs.length === 1)) {
    return { mode: 'editorial-dashboard' };
  }

  if (segs[0] === 'editorial' && segs.length >= 2) {
    return {
      mode: 'editorial-doc',
      repo: segs[1],
      tab: segs[2] || 'manuscript'
    };
  }

  if (segs[0] === 'infra') {
    return { mode: 'infra' };
  }

  const repo = segs[0] === 'r' ? segs[1] : segs[0];
  const tab = (segs[0] === 'r' ? segs[2] : segs[1]) || 'code';

  return {
    mode: 'repo-code',
    repo,
    tab,
    ref: q.get('ref') || '',
    path: q.get('path') || '',
    base: q.get('base') || '',
    head: q.get('head') || ''
  };
}

async function render() {
  const route = parseRoute();
  toggleSidebar(false);

  try {
    if (route.mode === 'editorial-dashboard') {
      updateNavHighlight('editorial');
      await renderEditorialDashboard();
    } else if (route.mode === 'editorial-doc') {
      updateNavHighlight('editorial');
      await renderEditorialDoc(route.repo, route.tab);
    } else if (route.mode === 'repos-dashboard') {
      updateNavHighlight('repos');
      await renderReposDashboard();
    } else if (route.mode === 'infra') {
      updateNavHighlight('infra');
      await renderInfra();
    } else if (route.mode === 'repo-code') {
      updateNavHighlight('repos');
      await renderRepoCode(route);
    }
  } catch (err) {
    app.innerHTML = `
      <div class="empty-state" style="padding:40px; text-align:center;">
        <h2 style="color:var(--navy);">Ocorreu um erro</h2>
        <p style="color:var(--muted);">${esc(err.message)}</p>
        <button class="btn-primary" onclick="location.hash='#/editorial'">Voltar ao Espaço Editorial</button>
      </div>
    `;
  }
}

// ============================================================================
// VIEW 1: ESPAÇO EDITORIAL (DASHBOARD)
// ============================================================================

async function renderEditorialDashboard() {
  const data = await api('/api/v1/repos');
  allReposCache = data.repositories || [];

  const editorials = allReposCache.filter(r => r.type === 'editorial');
  const software = allReposCache.filter(r => r.type !== 'editorial');
  const drafts = editorials.filter(r => r.empty || !r.tags).length;
  const published = editorials.reduce((acc, r) => acc + (r.tags || 0), 0);

  app.innerHTML = `
    <header class="hero-editorial">
      <div class="hero-kicker">GOVERNANÇA EDITORIAL CIENTÍFICA · C++26</div>
      <h1 class="hero-title">SisTer Git Editorial</h1>
      <p class="hero-desc">
        Ambiente integrado para elaboração de artigos, revisão por pares com proveniência W3C PROV e homologação de edições imutáveis diretamente sobre repositórios Git, sem necessidade de comandos de terminal.
      </p>
      <div class="statgrid">
        <div class="stat-card">
          <span>Artigos &amp; Relatórios</span>
          <strong>${editorials.length}</strong>
          <small>Documentos em governança</small>
        </div>
        <div class="stat-card">
          <span>Em Elaboração</span>
          <strong>${drafts}</strong>
          <small>Versões em evolução ativa</small>
        </div>
        <div class="stat-card">
          <span>Edições Homologadas</span>
          <strong>${published}</strong>
          <small>Tags anotadas <code>ed/*</code></small>
        </div>
        <div class="stat-card">
          <span>Software &amp; Código</span>
          <strong>${software.length}</strong>
          <small><a href="#/repos" style="color:#a8cae2; text-decoration:underline;">Ver repositórios</a></small>
        </div>
      </div>
    </header>

    <div class="section-head-bar">
      <div>
        <h2 class="section-title">Artigos e Projetos Documentais</h2>
        <p class="section-sub">Selecione um documento para ler, revisar ou salvar uma nova versão</p>
      </div>
      <button type="button" class="btn-primary" onclick="openNewProjectModal('editorial')">
        + Novo Artigo Científico
      </button>
    </div>

    <div class="editorial-grid" id="editorialCardsGrid">
      ${editorials.length ? editorials.map(renderEditorialCard).join('') : `
        <div class="empty-state" style="grid-column:1/-1; background:#fff; border:1px solid var(--line); border-radius:var(--radius-lg); padding:40px; text-align:center;">
          <h3 style="color:var(--navy); margin-bottom:8px;">Nenhum artigo editorial cadastrado</h3>
          <p style="color:var(--muted); margin-bottom:20px;">Comece criando o primeiro artigo sob governança editorial do SisTer Git.</p>
          <button type="button" class="btn-primary" onclick="openNewProjectModal('editorial')">+ Criar Primeiro Artigo</button>
        </div>
      `}
    </div>
  `;
}

function renderEditorialCard(r) {
  const vis = r.visibility === 'private'
    ? `<span class="badge-vis vis-private">🔒 Privado</span>`
    : `<span class="badge-vis vis-public">🌐 Público</span>`;

  const tagBadge = r.tags > 0
    ? `<span class="badge-edition-tag">ed/${r.tags} edições</span>`
    : `<span class="badge-stage stage-draft">Rascunho Ativo</span>`;

  const lastCommitInfo = r.last_commit
    ? `Versão salva ${timeAgo(r.last_commit.date)} por <strong>${esc(r.last_commit.author)}</strong>`
    : 'Aguardando primeiro manuscrito';

  return `
    <article class="article-card" data-search="${esc((r.name + ' ' + (r.last_commit?.subject || '')).toLowerCase())}">
      <div class="article-card-head">
        <a href="#/editorial/${encodeURIComponent(r.name)}" class="article-title-link">
          ${esc(r.name)}
        </a>
        <div class="badges-row">
          ${visBadge(r.visibility)}
          ${tagBadge}
        </div>
      </div>

      <div class="article-authors">
        ${r.last_commit ? `Última alteração: <em>"${esc(r.last_commit.subject)}"</em>` : 'Documento inicial'}
      </div>

      <p class="article-abstract">
        ${esc(r.description || 'Artigo científico sob rastreabilidade e proveniência Git do ecossistema SisTer.')}
      </p>

      <div class="article-meta-foot">
        <span>${lastCommitInfo}</span>
        <a href="#/editorial/${encodeURIComponent(r.name)}" class="btn-commit-save" style="padding:4px 10px; font-size:0.75rem; text-decoration:none;">
          Abrir ➔
        </a>
      </div>
    </article>
  `;
}

function visBadge(v) {
  return v === 'private'
    ? `<span class="badge-vis vis-private">🔒 Privado</span>`
    : `<span class="badge-vis vis-public">🌐 Público</span>`;
}

// ============================================================================
// VIEW 2: ESPAÇO DOCUMENTAL (LEITURA, EDITOR, PARECERES, PUBLICAÇÃO)
// ============================================================================

async function renderEditorialDoc(repo, tab = 'manuscript') {
  crumbCurrent.innerHTML = `<a href="#/editorial">Espaço Editorial</a> / <strong>${esc(repo)}</strong>`;

  const [edData, blobData, refsData] = await Promise.all([
    api(`/api/v1/repos/${encodeURIComponent(repo)}/editorial`),
    api(`/api/v1/repos/${encodeURIComponent(repo)}/blob?ref=HEAD&path=manuscrito.md`).catch(() => null),
    api(`/api/v1/repos/${encodeURIComponent(repo)}/refs`).catch(() => ({ branches: [], tags: [] }))
  ]);

  const manifest = edData.manifest || {};
  const title = manifest.title || repo;
  const authors = manifest.authors && manifest.authors.length
    ? manifest.authors.map(a => a.name).join(', ')
    : 'Pesquisador LABECO';
  const stage = manifest.current_stage || (edData.tags && edData.tags.length ? 'published' : 'draft');
  const reviews = manifest.reviews || [];
  const editions = (edData.tags || []);
  const rawMarkdown = blobData && !blobData.binary ? blobData.content : '# ' + title + '\n\nInicie a escrita do seu manuscrito.';

  const stageBadge = stage === 'published'
    ? `<span class="badge-stage stage-published">✓ Publicado</span>`
    : (stage === 'in_review'
      ? `<span class="badge-stage stage-in_review">✎ Em Revisão por Pares</span>`
      : `<span class="badge-stage stage-draft">Rascunho Ativo</span>`);

  let tabContent = '';

  if (tab === 'manuscript') {
    tabContent = `
      <article class="manuscript-reader markdown-body">
        ${renderMarkdown(rawMarkdown)}
        <hr>
        <footer style="font-size:0.8rem; color:var(--muted); text-align:center; padding-top:10px;">
          <em>Preservado sob governança SisTer Git Editorial · LABECO · Sempre pronto. Sempre incompleto!</em>
        </footer>
      </article>
    `;
  } else if (tab === 'editor') {
    const localDraft = localStorage.getItem('sister_draft_' + repo);
    const textToShow = (localDraft && localDraft.trim().length > 0) ? localDraft : rawMarkdown;

    tabContent = `
      <div class="split-editor-container">
        <div class="split-editor-grid">
          <div class="editor-pane">
            <div class="pane-header">
              <span>Manuscrito Markdown (manuscrito.md)</span>
              <span id="charCount">0 caracteres</span>
            </div>
            <textarea class="editor-textarea" id="liveEditorTextarea" oninput="handleEditorInput(this.value, '${esc(repo)}')">${esc(textToShow)}</textarea>
          </div>
          <div class="preview-pane">
            <div class="pane-header">
              <span>Visualização Renderizada em Tempo Real</span>
              <span style="color:var(--teal);">Leitura Científica</span>
            </div>
            <div class="preview-body markdown-body" id="livePreviewPane">
              ${renderMarkdown(textToShow)}
            </div>
          </div>
        </div>

        <form class="editor-commit-bar" onsubmit="handleSaveManuscript(event, '${esc(repo)}')">
          <div class="commit-input-group">
            <label style="font-weight:700; font-size:0.8rem; color:var(--navy);">Autor:</label>
            <input type="text" id="commitAuthor" required value="${esc(authors.split(',')[0].trim())}" placeholder="Nome do autor">
          </div>
          <div class="commit-input-group" style="flex:2;">
            <label style="font-weight:700; font-size:0.8rem; color:var(--navy);">Alterações:</label>
            <input type="text" id="commitMsg" required placeholder="Ex.: Revisão da metodologia e citação de novas fontes">
          </div>
          <button type="submit" class="btn-commit-save" id="btnSaveManuscript">
            <span>Salvar Versão no Git ⎇</span>
          </button>
        </form>
      </div>
    `;
  } else if (tab === 'reviews') {
    tabContent = `
      <div class="reviews-container">
        <div class="reviews-toolbar">
          <div>
            <h3 style="margin:0; color:var(--navy); font-size:1.1rem;">Rodadas de Pareceres e Avaliação por Pares</h3>
            <p style="margin:2px 0 0; color:var(--muted); font-size:0.82rem;">Rastreabilidade W3C PROV: cada parecer fica registrado no histórico imutável</p>
          </div>
          <button type="button" class="btn-primary" onclick="openNewReviewModal('${esc(repo)}')">
            + Emitir Parecer Editorial
          </button>
        </div>

        ${reviews.length ? reviews.map(r => `
          <div class="review-card">
            <div class="review-card-head">
              <span class="reviewer-name">${esc(r.reviewer || 'Avaliador')}</span>
              <div class="badges-row">
                <span class="review-badge-rec rec-${esc(r.recommendation || 'accept')}">
                  ${esc(r.recommendation === 'accept' ? '✓ Aceito' : (r.recommendation === 'accept_with_revisions' ? '✎ Aceito com Revisões' : (r.recommendation === 'revise_and_resubmit' ? '↻ Revisar e Ressubmeter' : '✗ Rejeitado')))}
                </span>
                <span class="review-date">${formatDate(r.date)}</span>
              </div>
            </div>
            <div class="review-text">${esc(r.summary || r.comments || '')}</div>
            ${r.resolution ? `
              <div class="review-resolution-box">
                <strong>Resposta do Autor / Resolução:</strong>
                <div>${esc(r.resolution.author_response || 'Alterações incorporadas.')}</div>
                ${r.resolution.resolved_in_commit ? `<div style="margin-top:4px; font-family:var(--font-mono); font-size:0.75rem; color:var(--blue);">Commit: ${esc(r.resolution.resolved_in_commit.substr(0, 8))}</div>` : ''}
              </div>
            ` : ''}
          </div>
        `).join('') : `
          <div class="empty-state" style="background:#fff; border:1px solid var(--line); border-radius:var(--radius-lg); padding:36px; text-align:center;">
            <p style="color:var(--muted); margin:0 0 16px;">Nenhum parecer formal registrado para este manuscrito.</p>
            <button class="btn-primary" onclick="openNewReviewModal('${esc(repo)}')">+ Emitir Primeiro Parecer</button>
          </div>
        `}
      </div>
    `;
  } else if (tab === 'publish') {
    tabContent = `
      <div class="editions-container">
        <div class="reviews-toolbar">
          <div>
            <h3 style="margin:0; color:var(--navy); font-size:1.1rem;">Edições Publicadas &amp; Proveniência Git</h3>
            <p style="margin:2px 0 0; color:var(--muted); font-size:0.82rem;">Versões com tags anotadas <code>ed/*</code> garantem imutabilidade e citação científica</p>
          </div>
          <button type="button" class="btn-primary" onclick="openPublishModal('${esc(repo)}')">
            + Publicar Nova Edição
          </button>
        </div>

        ${editions.length ? editions.map(ed => `
          <div class="edition-card">
            <div class="edition-head">
              <span class="edition-tag-name">${esc(ed.name)}</span>
              <span style="font-size:0.78rem; color:var(--muted);">${formatDate(ed.date)}</span>
            </div>
            <div class="edition-notes">
              <strong>${esc(ed.subject || 'Edição Homologada')}</strong>
            </div>
            <div style="font-family:var(--font-mono); font-size:0.75rem; color:var(--muted);">
              Assinatura Git: <code>${esc(ed.hash.substr(0, 10))}</code>
            </div>
          </div>
        `).join('') : `
          <div class="empty-state" style="background:#fff; border:1px solid var(--line); border-radius:var(--radius-lg); padding:36px; text-align:center;">
            <p style="color:var(--muted); margin:0 0 16px;">Nenhuma edição formal publicada ainda.</p>
            <button class="btn-primary" onclick="openPublishModal('${esc(repo)}')">Publicar Versão v1.0 🚀</button>
          </div>
        `}
      </div>
    `;
  }

  app.innerHTML = `
    <header class="editorial-doc-header">
      <div class="doc-kicker-bar">
        <span class="doc-kicker">SisTer Git Editorial · Identidade Documental Persistente</span>
        <div class="badges-row">
          ${visBadge(edData.visibility || 'public')}
          ${stageBadge}
        </div>
      </div>

      <h1 class="doc-title-main">${esc(title)}</h1>
      <div class="doc-authors-line">
        <strong>${esc(authors)}</strong> · <em>Laboratório de Ecologia e Conservação (LABECO)</em>
      </div>

      <div class="doc-provenance-box">
        <span>Identificador: <strong>${esc(repo)}</strong> (refs/heads/main)</span>
        <span>Código Git: <a href="#/r/${encodeURIComponent(repo)}/code" style="color:var(--blue); text-decoration:underline;">Ver arquivos brutos ➔</a></span>
      </div>
    </header>

    <nav class="doc-tabs-bar" aria-label="Navegação do Artigo">
      <a href="#/editorial/${encodeURIComponent(repo)}/manuscript" class="doc-tab ${tab === 'manuscript' ? 'active' : ''}">
        <span>📖 Leitura &amp; Manuscrito</span>
      </a>
      <a href="#/editorial/${encodeURIComponent(repo)}/editor" class="doc-tab ${tab === 'editor' ? 'active' : ''}">
        <span>✎ Editor Interativo</span>
      </a>
      <a href="#/editorial/${encodeURIComponent(repo)}/reviews" class="doc-tab ${tab === 'reviews' ? 'active' : ''}">
        <span>⚖ Pareceres &amp; Revisão</span>
        <span class="tab-badge-count">${reviews.length}</span>
      </a>
      <a href="#/editorial/${encodeURIComponent(repo)}/publish" class="doc-tab ${tab === 'publish' ? 'active' : ''}">
        <span>🚀 Edições &amp; Publicação</span>
        <span class="tab-badge-count">${editions.length}</span>
      </a>
    </nav>

    <div id="docTabHost">
      ${tabContent}
    </div>
  `;

  if (tab === 'editor') {
    handleEditorInput(rawMarkdown);
  }
}

window.handleEditorInput = function(val, repo) {
  const p = document.querySelector('#livePreviewPane');
  const count = document.querySelector('#charCount');
  if (p) p.innerHTML = renderMarkdown(val);
  if (count) count.textContent = `${val.length} caracteres`;
  if (repo) {
    try { localStorage.setItem('sister_draft_' + repo, val); } catch (_) {}
  }
};

window.handleSaveManuscript = async function(e, repo) {
  e.preventDefault();
  const textarea = document.querySelector('#liveEditorTextarea');
  const author = document.querySelector('#commitAuthor')?.value.trim();
  const message = document.querySelector('#commitMsg')?.value.trim();
  const btn = document.querySelector('#btnSaveManuscript');

  if (!textarea || !btn) return;
  const content = textarea.value;

  btn.disabled = true;
  btn.innerHTML = 'Salvando no Git...';

  try {
    const res = await apiPost(`/api/v1/repos/${encodeURIComponent(repo)}/editorial/save`, {
      filename: 'manuscrito.md',
      content,
      author: author || 'Pesquisador LABECO',
      message: message || 'Atualização editorial do manuscrito'
    });
    localStorage.removeItem('sister_draft_' + repo);
    showToast(`✓ Nova versão salva no Git! Commit: ${res.short_hash || ''}`);
    btn.innerHTML = '✓ Versão Salva!';
    setTimeout(() => {
      btn.disabled = false;
      btn.innerHTML = '<span>Salvar Versão no Git ⎇</span>';
    }, 1800);
  } catch (err) {
    if (err.message.includes('Autenticação') || err.message.includes('Token') || err.message.includes('401')) {
      openAuthModal();
      const fb = document.querySelector('#authModalFeedback');
      if (fb) {
        fb.textContent = 'Autenticação necessária para salvar no Git. Digite o token de pesquisador.';
        fb.classList.remove('hidden');
      }
    } else {
      alert('Erro ao salvar versão: ' + err.message);
    }
    btn.disabled = false;
    btn.innerHTML = '<span>Salvar Versão no Git ⎇</span>';
  }
};

// ============================================================================
// VIEW 3: REPOSITÓRIOS DE SOFTWARE (GIT BARE WORKSPACE)
// ============================================================================

async function renderReposDashboard() {
  const data = await api('/api/v1/repos');
  allReposCache = data.repositories || [];

  app.innerHTML = `
    <div class="section-head-bar">
      <div>
        <h2 class="section-title">Repositórios Git</h2>
        <p class="section-sub">Repositórios de código e infraestrutura técnica sob custódia do nó LABECO</p>
      </div>
      <button type="button" class="btn-primary" onclick="openNewProjectModal('software')">
        + Novo Repositório
      </button>
    </div>

    <div class="repo-table-container">
      ${allReposCache.map(r => `
        <div class="repo-row" data-search="${esc(r.name.toLowerCase())}">
          <div class="repo-left-meta">
            <span style="font-size:1.3rem;">${r.type === 'editorial' ? '📖' : '📁'}</span>
            <div>
              <a href="#/r/${encodeURIComponent(r.name)}/code" class="repo-name-link">
                ${esc(r.name)}
              </a>
              <div class="repo-desc-sub">
                ${r.last_commit ? esc(r.last_commit.subject) : 'Repositório inicializado'}
              </div>
            </div>
          </div>
          <div class="repo-stats-right">
            ${visBadge(r.visibility)}
            ${r.type === 'editorial' ? '<span class="badge-edition-tag">Editorial</span>' : ''}
            <span>${r.branches} branches</span>
            <span>${r.tags} tags</span>
            <span>${fmt(r.size_bytes)}</span>
            <span>${timeAgo(r.last_commit?.date)}</span>
          </div>
        </div>
      `).join('')}
    </div>
  `;
}

// ============================================================================
// VIEW 4: REPO CODE, COMMITS, REFS, COMPARE (GITHUB-LIKE)
// ============================================================================

async function renderRepoCode(route) {
  const { repo, tab, ref, path, base, head } = route;
  crumbCurrent.innerHTML = `<a href="#/repos">Repositórios</a> / <strong>${esc(repo)}</strong>`;

  const [refsData, treeData] = await Promise.all([
    api(`/api/v1/repos/${encodeURIComponent(repo)}/refs`),
    tab === 'code' ? api(`/api/v1/repos/${encodeURIComponent(repo)}/tree?ref=${encodeURIComponent(ref || 'HEAD')}&path=${encodeURIComponent(path || '')}`).catch(() => null) : null
  ]);

  let innerContent = '';

  if (tab === 'code') {
    if (path && treeData && !treeData.entries) {
      // Blob view
      const blob = await api(`/api/v1/repos/${encodeURIComponent(repo)}/blob?ref=${encodeURIComponent(ref || 'HEAD')}&path=${encodeURIComponent(path)}`);
      innerContent = `
        <div class="blob-viewer">
          <div class="blob-head">
            <span><strong>${esc(path)}</strong> (${blob.binary ? 'Arquivo binário' : fmt(blob.size_bytes)})</span>
            <a href="#/r/${encodeURIComponent(repo)}/code?ref=${encodeURIComponent(ref || '')}" class="btn-cancel" style="padding:4px 8px; font-size:0.75rem;">Voltar à pasta</a>
          </div>
          ${blob.binary ? `<div style="padding:40px; text-align:center; color:var(--muted);">Arquivo binário não editável diretamente</div>` : `
            <pre class="blob-code"><code>${esc(blob.content)}</code></pre>
          `}
        </div>
      `;
    } else {
      // Directory tree
      const entries = treeData?.entries || [];
      innerContent = `
        <table class="tree-table">
          <thead>
            <tr>
              <th>Nome</th>
              <th>Tipo</th>
              <th>SHA</th>
            </tr>
          </thead>
          <tbody>
            ${path ? `
              <tr>
                <td colspan="3"><a href="#/r/${encodeURIComponent(repo)}/code?ref=${encodeURIComponent(ref)}&path=${encodeURIComponent(path.split('/').slice(0,-1).join('/'))}">.. (diretório anterior)</a></td>
              </tr>
            ` : ''}
            ${entries.map(e => `
              <tr>
                <td>
                  <a href="#/r/${encodeURIComponent(repo)}/code?ref=${encodeURIComponent(ref)}&path=${encodeURIComponent((path ? path + '/' : '') + e.name)}">
                    ${e.type === 'tree' ? '📁 ' : '📄 '} <strong>${esc(e.name)}</strong>
                  </a>
                </td>
                <td><span style="font-size:0.75rem; color:var(--muted);">${esc(e.type)}</span></td>
                <td><code style="font-size:0.75rem; color:var(--muted);">${esc(e.hash.substr(0, 8))}</code></td>
              </tr>
            `).join('')}
          </tbody>
        </table>
      `;
    }
  } else if (tab === 'commits') {
    const commitsData = await api(`/api/v1/repos/${encodeURIComponent(repo)}/commits?ref=${encodeURIComponent(ref || 'HEAD')}`);
    const commits = commitsData.commits || [];
    innerContent = `
      <div class="repo-table-container">
        ${commits.map(c => `
          <div class="repo-row">
            <div>
              <strong>${esc(c.subject)}</strong>
              <div style="font-size:0.75rem; color:var(--muted); margin-top:2px;">
                ${esc(c.author)} &lt;${esc(c.email)}&gt; · ${timeAgo(c.date)}
              </div>
            </div>
            <div>
              <code style="background:var(--surface-alt); padding:3px 7px; border-radius:4px; font-size:0.75rem;">${esc(c.short_hash)}</code>
            </div>
          </div>
        `).join('')}
      </div>
    `;
  } else if (tab === 'compare') {
    const b = base || 'HEAD~1';
    const h = head || 'HEAD';
    const diffData = await api(`/api/v1/repos/${encodeURIComponent(repo)}/compare?base=${encodeURIComponent(b)}&head=${encodeURIComponent(h)}`).catch(() => ({ files: [] }));
    const files = diffData.files || [];
    innerContent = `
      <div style="margin-bottom:16px;">
        <span style="font-size:0.9rem; font-weight:700; color:var(--navy);">Comparando ${esc(b)} ➔ ${esc(h)} (${diffData.total_files || files.length} arquivos alterados)</span>
      </div>
      ${files.map(f => `
        <div class="diff-box">
          <div class="diff-file-head">${esc(f.path)}</div>
          <div>
            ${f.lines.map(l => {
              const cls = l.type === 'add' ? 'diff-line-add' : (l.type === 'del' ? 'diff-line-del' : (l.type === 'hunk' ? 'diff-hunk' : ''));
              return `<div class="diff-line ${cls}">${esc(l.text)}</div>`;
            }).join('')}
          </div>
        </div>
      `).join('')}
    `;
  }

  app.innerHTML = `
    <header class="editorial-doc-header" style="padding:20px 24px;">
      <div style="display:flex; justify-content:space-between; align-items:center;">
        <div>
          <h1 style="font-size:1.4rem; color:var(--navy); margin:0;">${esc(repo)}</h1>
          <p style="margin:2px 0 0; color:var(--muted); font-size:0.8rem;">Repositório bare gerenciado pelo nó local SisTer Git</p>
        </div>
        <a href="#/editorial/${encodeURIComponent(repo)}" class="btn-primary" style="font-size:0.8rem;">
          Ir para Espaço Editorial ➔
        </a>
      </div>
    </header>

    <nav class="doc-tabs-bar">
      <a href="#/r/${encodeURIComponent(repo)}/code" class="doc-tab ${tab === 'code' ? 'active' : ''}">📄 Código</a>
      <a href="#/r/${encodeURIComponent(repo)}/commits" class="doc-tab ${tab === 'commits' ? 'active' : ''}">⎇ Commits</a>
      <a href="#/r/${encodeURIComponent(repo)}/compare" class="doc-tab ${tab === 'compare' ? 'active' : ''}">⇄ Comparar Diffs</a>
    </nav>

    <div>
      ${innerContent}
    </div>
  `;
}

// ============================================================================
// VIEW 5: INFRAESTRUTURA & HA
// ============================================================================

async function renderInfra() {
  const ha = await api('/api/v1/ha').catch(() => ({ nodes: [] }));
  app.innerHTML = `
    <header class="hero-editorial" style="background:linear-gradient(130deg, #071e38, #0e3d5c);">
      <div class="hero-kicker">ALTA DISPONIBILIDADE E REPLICAÇÃO</div>
      <h1 class="hero-title">Infraestrutura SisTer Git</h1>
      <p class="hero-desc">
        Topologia de dois nós com custódia distribuída e sincronização de repositórios Git entre LABECO-SRV e réplicas parceiras.
      </p>
    </header>

    <div class="editorial-grid">
      ${ha.nodes.map(n => `
        <div class="article-card">
          <h3 style="color:var(--navy); margin:0 0 6px;">${esc(n.name)}</h3>
          <p style="color:var(--muted); font-size:0.82rem; margin:0 0 12px;">Papel: <strong>${esc(n.role)}</strong></p>
          <div class="badges-row">
            <span class="badge-stage stage-published">Ativo</span>
            <span class="badge-vis vis-public">C++26</span>
          </div>
        </div>
      `).join('')}
    </div>
  `;
}

// ============================================================================
// MODALS LOGIC
// ============================================================================

window.openModal = id => document.querySelector('#' + id)?.classList.remove('hidden');
window.closeModal = id => document.querySelector('#' + id)?.classList.add('hidden');

window.openNewProjectModal = function(defaultType = 'editorial') {
  const sel = document.querySelector('#newProjType');
  if (sel) sel.value = defaultType;
  toggleProjectFields(defaultType);
  openModal('modalNewProject');
};

window.toggleProjectFields = function(type) {
  const f = document.querySelector('#editorialSpecificFields');
  if (f) f.style.display = type === 'editorial' ? 'block' : 'none';
};

window.handleCreateProject = async function(e) {
  e.preventDefault();
  const name = document.querySelector('#newProjName').value.trim();
  const type = document.querySelector('#newProjType').value;
  const title = document.querySelector('#newProjTitle')?.value.trim();
  const author = document.querySelector('#newProjAuthor')?.value.trim();
  const desc = document.querySelector('#newProjDesc')?.value.trim();
  const vis = document.querySelector('#newProjVisibility')?.value;
  const fb = document.querySelector('#newProjFeedback');
  const btn = document.querySelector('#btnSubmitProject');

  btn.disabled = true;
  btn.textContent = 'Criando no Git...';

  try {
    await apiPost('/api/v1/repos', {
      name,
      type,
      title: title || name,
      author: author || 'Pesquisador LABECO',
      description: desc,
      visibility: vis
    });
    closeModal('modalNewProject');
    showToast(`✓ Projeto '${name}' criado com sucesso no SisTer Git!`);
    if (type === 'editorial') {
      location.hash = `#/editorial/${encodeURIComponent(name)}/editor`;
    } else {
      location.hash = `#/r/${encodeURIComponent(name)}/code`;
    }
  } catch (err) {
    if (fb) {
      fb.textContent = err.message;
      fb.classList.remove('hidden');
    }
    btn.disabled = false;
    btn.textContent = 'Criar Projeto Git ✓';
  }
};

window.openNewReviewModal = function(repo) {
  document.querySelector('#reviewRepoName').value = repo;
  openModal('modalNewReview');
};

window.handleCreateReview = async function(e) {
  e.preventDefault();
  const repo = document.querySelector('#reviewRepoName').value;
  const reviewer = document.querySelector('#reviewReviewer').value.trim();
  const rec = document.querySelector('#reviewRecommendation').value;
  const summary = document.querySelector('#reviewSummary').value.trim();
  const authorResp = document.querySelector('#reviewAuthorResp')?.value.trim();
  const btn = document.querySelector('#btnSubmitReview');

  btn.disabled = true;
  btn.textContent = 'Registrando...';

  try {
    await apiPost(`/api/v1/repos/${encodeURIComponent(repo)}/editorial/review`, {
      reviewer,
      recommendation: rec,
      summary,
      author_response: authorResp
    });
    closeModal('modalNewReview');
    showToast('✓ Parecer registrado no Git com proveniência!');
    renderEditorialDoc(repo, 'reviews');
  } catch (err) {
    alert('Erro ao registrar parecer: ' + err.message);
    btn.disabled = false;
    btn.textContent = 'Registrar Parecer no Git ✓';
  }
};

window.openPublishModal = function(repo) {
  document.querySelector('#publishRepoName').value = repo;
  openModal('modalPublish');
};

window.handlePublishEdition = async function(e) {
  e.preventDefault();
  const repo = document.querySelector('#publishRepoName').value;
  const tag = document.querySelector('#publishTag').value.trim();
  const title = document.querySelector('#publishTitle').value.trim();
  const notes = document.querySelector('#publishNotes').value.trim();
  const btn = document.querySelector('#btnSubmitPublish');

  btn.disabled = true;
  btn.textContent = 'Publicando...';

  try {
    await apiPost(`/api/v1/repos/${encodeURIComponent(repo)}/editorial/publish`, {
      tag,
      title,
      notes
    });
    closeModal('modalPublish');
    showToast(`🚀 Edição '${tag}' publicada e homologada no Git!`);
    renderEditorialDoc(repo, 'publish');
  } catch (err) {
    alert('Erro ao homologar edição: ' + err.message);
    btn.disabled = false;
    btn.textContent = 'Publicar Edição 🚀';
  }
};

window.openAuthModal = function() {
  document.querySelector('#authTokenInput').value = getToken();
  openModal('modalAuth');
};

window.handleSaveToken = async function(e) {
  e.preventDefault();
  const t = document.querySelector('#authTokenInput').value.trim();
  const fb = document.querySelector('#authModalFeedback');
  setToken(t);

  try {
    const s = await api('/api/v1/auth/status');
    if (s.auth_required && !s.authenticated) {
      setToken('');
      if (fb) {
        fb.textContent = 'Token recusado pelo servidor.';
        fb.classList.remove('hidden');
      }
      return;
    }
    closeModal('modalAuth');
    showToast('✓ Token autenticado com sucesso! Agora clique em Salvar Versão no Git.');
    updateAuthStatus();
    const route = parseRoute();
    if (!(route.mode === 'editorial-doc' && route.tab === 'editor')) {
      render();
    }
  } catch (err) {
    setToken('');
    if (fb) {
      fb.textContent = err.message;
      fb.classList.remove('hidden');
    }
  }
};

window.handleClearToken = function() {
  setToken('');
  closeModal('modalAuth');
  showToast('Token removido.');
  updateAuthStatus();
  render();
};

window.handleSearch = function(q) {
  const query = q.trim().toLowerCase();
  document.querySelectorAll('[data-search]').forEach(el => {
    const text = el.getAttribute('data-search') || '';
    el.style.display = !query || text.includes(query) ? '' : 'none';
  });
};

// ============================================================================
// INITIALIZATION
// ============================================================================

window.addEventListener('hashchange', render);
window.addEventListener('DOMContentLoaded', () => {
  updateAuthStatus();
  render();
});
