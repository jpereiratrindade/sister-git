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
const setToken = t => {
  if (t) {
    localStorage.setItem('sister_token', t.trim());
    document.cookie = `sister_token=${encodeURIComponent(t.trim())}; path=/; SameSite=Lax; max-age=2592000`;
  } else {
    localStorage.removeItem('sister_token');
    document.cookie = 'sister_token=; path=/; max-age=0';
  }
};

window.downloadFile = async function(url, suggestedFilename) {
  const token = getToken();
  const finalUrl = url;
  showToast('⏳ Preparando download...');
  try {
    const headers = {};
    if (token) headers['Authorization'] = 'Bearer ' + token;
    const res = await fetch(finalUrl, { headers });
    if (!res.ok) {
      const err = await res.json().catch(() => ({ error: `HTTP ${res.status}` }));
      throw new Error(err.error || `Erro HTTP ${res.status}`);
    }
    const blob = await res.blob();
    const blobUrl = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = blobUrl;
    let filename = suggestedFilename || 'download';
    const cd = res.headers.get('Content-Disposition');
    if (cd && cd.includes('filename=')) {
      const match = cd.match(/filename="?([^"]+)"?/);
      if (match && match[1]) filename = match[1];
    }
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    setTimeout(() => {
      document.body.removeChild(a);
      URL.revokeObjectURL(blobUrl);
    }, 1500);
    showToast('✓ Download concluído!');
  } catch (e) {
    console.error('Download falhou:', e);
    showToast('❌ Falha no download: ' + e.message);
  }
};

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

let currentUser = null;
let currentOtpEmail = '';

async function updateAuthStatus() {
  if (!sidebarAuth) return;
  try {
    const s = await api('/api/v1/auth/status').catch(() => ({ authenticated: false, auth_required: false, user: null }));
    if (s.authenticated && s.user) {
      currentUser = s.user;
      const name = s.user.name || s.user.email.split('@')[0];
      const initials = (name || 'P')
        .split(' ')
        .map(w => w[0])
        .filter(Boolean)
        .slice(0, 2)
        .join('')
        .toUpperCase();

      sidebarAuth.innerHTML = `
        <div class="sidebar-user-chip" onclick="openAuthModal()" title="Conectado como ${esc(s.user.email)} (${s.user.role || 'pesquisador'})">
          <div class="user-avatar-sm">${esc(initials)}</div>
          <div class="sidebar-user-details">
            <span class="sidebar-user-name">${esc(name)}</span>
            <span class="sidebar-user-email">${esc(s.user.email)}</span>
          </div>
          <button type="button" class="btn-auth-action" onclick="event.stopPropagation(); logout()" title="Encerrar sessão autenticada">Sair</button>
        </div>
      `;
    } else {
      currentUser = null;
      sidebarAuth.innerHTML = `
        <div class="auth-badge-status" title="Acesso público somente leitura">
          <span class="dot-anon"></span>
          <span>Público</span>
        </div>
        <button type="button" class="btn-auth-action" onclick="openAuthModal()" title="Autenticar como pesquisador">Entrar</button>
      `;
    }
  } catch (_) {
    currentUser = null;
    sidebarAuth.innerHTML = `
      <div class="auth-badge-status"><span class="dot-anon"></span><span>Público</span></div>
    `;
  }
}

window.logout = async function() {
  setToken('');
  currentUser = null;
  await apiPost('/api/v1/auth/logout', {}).catch(() => {});
  closeModal('modalAuth');
  showToast('Sessão encerrada.');
  await updateAuthStatus();
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
  if (workspace === 'home') {
    document.querySelector('#nav-home')?.classList.add('active');
    crumbCurrent.textContent = 'Início';
  } else if (workspace === 'editorial') {
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
  // #/ or empty or #/home -> { mode: 'home' }
  // #/editorial -> { mode: 'editorial-dashboard' }
  // #/editorial/:repo -> { mode: 'editorial-doc', repo: ':repo', tab: 'manuscript' }
  // #/editorial/:repo/:tab -> { mode: 'editorial-doc', repo: ':repo', tab: ':tab' }
  // #/repos -> { mode: 'repos-dashboard' }
  // #/infra -> { mode: 'infra' }
  // #/r/:repo/:tab or #/:repo/:tab -> { mode: 'repo-code', repo, tab, ref, path, base, head }

  // Default: Home Page Profissional (#/ ou vazio ou #/home)
  if (!segs.length || segs[0] === 'home' || location.hash === '#/' || location.hash === '#/home' || !location.hash) {
    return { mode: 'home' };
  }

  // Se for explicitamente repositórios (#/repos)
  if (segs[0] === 'repos') {
    return { mode: 'repos-dashboard' };
  }

  // Se for explicitamente editorial (#/editorial)
  if (segs[0] === 'editorial' && segs.length === 1) {
    return { mode: 'editorial-dashboard' };
  }

  if (segs[0] === 'editorial' && segs.length >= 2) {
    return {
      mode: 'editorial-doc',
      repo: segs[1],
      tab: segs[2] || 'manuscript',
      base: q.get('base') || '',
      head: q.get('head') || '',
      view: q.get('view') || ''
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
    head: q.get('head') || '',
    view: q.get('view') || ''
  };
}

async function render() {
  const route = parseRoute();
  toggleSidebar(false);

  try {
    if (route.mode === 'home') {
      updateNavHighlight('home');
      await renderHomePage();
    } else if (route.mode === 'editorial-dashboard') {
      updateNavHighlight('editorial');
      await renderEditorialDashboard();
    } else if (route.mode === 'editorial-doc') {
      updateNavHighlight('editorial');
      await renderEditorialDoc(route.repo, route.tab, route);
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
// VIEW 0: HOME PAGE (PÁGINA PRINCIPAL PROFISSIONAL)
// ============================================================================

async function renderHomePage() {
  const data = await api('/api/v1/repos').catch(() => ({ repositories: [] }));
  const ha = await api('/api/v1/ha').catch(() => ({ nodes: [] }));
  allReposCache = data.repositories || [];

  const editorials = allReposCache.filter(r => r.type === 'editorial');
  const software = allReposCache.filter(r => r.type !== 'editorial');
  const published = editorials.reduce((acc, r) => acc + (r.editorial_tags || 0), 0);
  const activeNodes = (ha.nodes || []).filter(n => n.status === 'active').length;

  const recent = allReposCache.slice(0, 6);

  app.innerHTML = `
    <div class="home-container">
      <!-- HERO PRINCIPAL -->
      <header class="home-hero">
        <div class="home-hero-header">
          <div>
            <div class="home-hero-kicker-row">
              <span class="home-hero-kicker">ECOSSISTEMA SISTERN · LABECO EMBRAPA</span>
              <span class="home-hero-badge">● Servidor Ativo (labeco-srv) · C++26 Nativo</span>
            </div>
            <h1 class="home-hero-title">SisTer Git</h1>
            <p class="home-hero-subtitle">Governança Editorial, Gestão do Conhecimento &amp; Repositórios Científicos</p>
          </div>
          <div style="background:rgba(255,255,255,0.08); border:1px solid rgba(255,255,255,0.15); border-radius:16px; padding:12px; display:grid; place-items:center;">
            <svg viewBox="0 0 44 44" width="56" height="56" fill="none" stroke="currentColor" stroke-width="2.3" stroke-linecap="round" stroke-linejoin="round" style="color:var(--sky);">
              <g stroke="currentColor" stroke-width="2.5">
                <path d="M12 7v30"/>
                <path d="M32 14v23"/>
                <path d="M12 14c0 6 20 2 20 9"/>
                <path d="M12 23c0 6 20 2 20 9"/>
                <path d="M12 7c0 5 20 1 20 7"/>
              </g>
              <circle cx="12" cy="7" r="3.2" fill="#2ea8e8"/>
              <circle cx="12" cy="14" r="2.8" fill="currentColor"/>
              <circle cx="12" cy="23" r="2.8" fill="currentColor"/>
              <circle cx="12" cy="37" r="3.2" fill="#087f8c"/>
              <circle cx="32" cy="14" r="3.2" fill="#2ea8e8"/>
              <circle cx="32" cy="23" r="2.8" fill="currentColor"/>
              <circle cx="32" cy="32" r="2.8" fill="currentColor"/>
              <circle cx="32" cy="37" r="3.2" fill="#087f8c"/>
            </svg>
          </div>
        </div>

        <p class="home-hero-desc">
          Ambiente integrado de pesquisa que conecta a escrita científica com a rastreabilidade imutável do Git. Permite redigir manuscritos acadêmicos, registrar pareceres de revisão por pares e versionar código e dados ecológicos com alta disponibilidade e governança institucional.
        </p>

        <div class="home-hero-actions">
          <a href="#/editorial" class="home-btn-primary">
            <span>📖 Espaço Editorial</span>
            <span style="font-size: 0.75rem; background: rgba(0,0,0,0.2); padding: 2px 7px; border-radius: 10px;">${editorials.length}</span>
          </a>
          <a href="#/repos" class="home-btn-secondary">
            <span>💻 Repositórios Git</span>
            <span style="font-size: 0.75rem; background: rgba(255,255,255,0.15); padding: 2px 7px; border-radius: 10px;">${software.length}</span>
          </a>
          <a href="#/infra" class="home-btn-secondary">
            <span>⚡ Alta Disponibilidade</span>
            <span style="font-size: 0.75rem; color:#facc15;">${ha.replication === 'synchronized' ? 'Sincronizado' : 'Não verificado'}</span>
          </a>
          <button type="button" class="home-btn-secondary" style="cursor:pointer;" onclick="openNewProjectModal()">
            <span>+ Novo Projeto</span>
          </button>
        </div>
      </header>

      <!-- MÉTRICAS DO SISTEMA -->
      <section class="statgrid">
        <div class="stat-card">
          <span>Manuscritos &amp; Artigos</span>
          <strong>${editorials.length}</strong>
          <small>Documentos em governança editorial</small>
        </div>
        <div class="stat-card">
          <span>Software &amp; Dados</span>
          <strong>${software.length}</strong>
          <small>Repositórios bare gerenciados</small>
        </div>
        <div class="stat-card">
          <span>Edições Homologadas</span>
          <strong>${published}</strong>
          <small>Versões publicadas com tags Git</small>
        </div>
        <div class="stat-card">
          <span>Tolerância a Falhas</span>
          <strong>${activeNodes} Nós Ativos</strong>
          <small>Replicação bidirecional contínua</small>
        </div>
      </section>

      <!-- OS 3 PILARES DO SISTEMA -->
      <section>
        <div class="home-section-head">
          <div>
            <h2 class="home-section-title">Pilares do SisTer Git</h2>
            <p class="home-section-desc">Uma plataforma unificada para governança do conhecimento, código e continuidade operacional.</p>
          </div>
        </div>

        <div class="home-pillars-grid">
          <!-- PILAR 1 -->
          <div class="pillar-card pillar-editorial">
            <div>
              <div class="pillar-icon-box" style="color: #e5b94f;">
                <svg viewBox="0 0 24 24" width="26" height="26" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <path d="M3 19c3-1.5 6-1.5 9 1 3-2.5 6-2.5 9-1V7c-3-1.5-6-1.5-9 1-3-2.5-6-2.5-9-1z"/>
                  <path d="M12 20V9"/>
                  <path d="M12 9c-.8-2 0-4 2.5-6 0 2.5-1.2 4.5-2.5 6z" fill="currentColor"/>
                </svg>
              </div>
              <span class="pillar-badge badge-gold">Módulo Editorial C++26</span>
              <h3 class="pillar-title">SisTer Editorial</h3>
              <p class="pillar-desc">
                Transforma o Git em infraestrutura de gestão documental científica. Redija manuscritos com formatação instantânea, emita pareceres formais e homologue edições imutáveis com proveniência W3C PROV.
              </p>
            </div>
            <a href="#/editorial" class="pillar-link">Entrar no Espaço Editorial →</a>
          </div>

          <!-- PILAR 2 -->
          <div class="pillar-card pillar-git">
            <div>
              <div class="pillar-icon-box" style="color: var(--blue);">
                <svg viewBox="0 0 24 24" width="26" height="26" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <line x1="6" y1="3" x2="6" y2="21"/>
                  <circle cx="6" cy="6" r="3"/>
                  <circle cx="6" cy="18" r="3"/>
                  <path d="M18 9a9 9 0 0 1-9 9"/>
                  <circle cx="18" cy="9" r="3"/>
                </svg>
              </div>
              <span class="pillar-badge badge-blue">Gestão Bare Git</span>
              <h3 class="pillar-title">Repositórios de Código</h3>
              <p class="pillar-desc">
                Controle de versão puro e eficiente para scripts analíticos em R/Python, rotinas ecológicas e dados brutos. Navegue pela árvore de arquivos, compare diffs e audite commits em microssegundos.
              </p>
            </div>
            <a href="#/repos" class="pillar-link">Explorar Repositórios Git →</a>
          </div>

          <!-- PILAR 3 -->
          <div class="pillar-card pillar-ha">
            <div>
              <div class="pillar-icon-box" style="color: var(--green);">
                <svg viewBox="0 0 24 24" width="26" height="26" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                  <rect x="2" y="3" width="20" height="7" rx="2"/>
                  <rect x="2" y="14" width="20" height="7" rx="2"/>
                  <line x1="6" y1="6" x2="6.01" y2="6"/>
                  <line x1="6" y1="17" x2="6.01" y2="17"/>
                </svg>
              </div>
              <span class="pillar-badge badge-green">Nós Gêmeos (HA)</span>
              <h3 class="pillar-title">Resiliência Operacional</h3>
              <p class="pillar-desc">
                Infraestrutura preparada para redundância entre o servidor central (<code>labeco-srv</code>) e nós réplica. O estado só é exibido como sincronizado após verificação operacional.
              </p>
            </div>
            <a href="#/infra" class="pillar-link">Ver Topologia de Alta Disponibilidade →</a>
          </div>
        </div>
      </section>

      <!-- PROJETOS E ARTIGOS RECENTES -->
      ${recent.length ? `
        <section>
          <div class="home-section-head">
            <div>
              <h2 class="home-section-title">Artigos e Repositórios Recentes</h2>
              <p class="home-section-desc">Projetos ativos hospedados na infraestrutura do LABECO.</p>
            </div>
            <a href="#/repos" style="font-size: 0.84rem; font-weight: 700; color: var(--blue);">Ver todos (${allReposCache.length}) →</a>
          </div>

          <div class="grid-repo">
            ${recent.map(r => {
              const isEd = r.type === 'editorial';
              const link = isEd ? `#/editorial/${encodeURIComponent(r.name)}/manuscript` : `#/r/${encodeURIComponent(r.name)}/code`;
              const visLabel = r.visibility === 'private' ? '🔒 Privado' : '🌐 Público';
              const visClass = r.visibility === 'private' ? 'badge-private' : 'badge-public';
              return `
                <article class="repo-card" data-search="${esc(r.name)} ${esc(r.visibility || '')}">
                  <div class="repo-card-head">
                    <span class="repo-name" title="${esc(r.name)}">
                      <a href="${link}">${esc((isEd && r.title) || r.name)}</a>
                    </span>
                    <div style="display: flex; gap: 6px; align-items: center;">
                      <span class="badge ${visClass}">${visLabel}</span>
                      <span class="badge ${isEd ? 'badge-editorial' : 'badge-primary'}">${isEd ? '📖 Editorial' : '💻 Código'}</span>
                    </div>
                  </div>
                  <p class="repo-desc">${esc(r.description || (isEd ? 'Projeto editorial científico sob governança Git.' : 'Repositório de software sob controle de versão Git.'))}</p>
                  <div class="repo-meta">
                    <span>${r.branches || 1} ramo(s)</span>
                    <span>${isEd ? (r.editorial_tags || 0) : (r.tags || 0)} edição/tag(s)</span>
                    <a href="${link}" style="margin-left: auto; font-weight: 700;">Abrir →</a>
                  </div>
                </article>
              `;
            }).join('')}
          </div>
        </section>
      ` : ''}

      <!-- MANIFESTO LABECO -->
      <section class="home-manifesto-card">
        <div>
          <div class="home-manifesto-quote">“Sempre pronto. Sempre incompleto!”</div>
          <p class="home-manifesto-text">
            No Laboratório de Estudos em AgroEcologia e Recursos Naturais (LABECO), consideramos que o conhecimento científico e o código não são arquivos isolados, mas o resultado contínuo de um processo rigoroso de elaboração, diálogo entre pares, revisão e homologação.
          </p>
        </div>
        <div>
          <button type="button" class="btn-primary" style="white-space: nowrap;" onclick="openNewProjectModal()">+ Novo Artigo / Projeto</button>
        </div>
      </section>
    </div>
  `;
}

// ============================================================================
// VIEW 1: ESPAÇO EDITORIAL (DASHBOARD)
// ============================================================================

async function renderEditorialDashboard() {
  const data = await api('/api/v1/repos');
  allReposCache = data.repositories || [];

  const editorials = allReposCache.filter(r => r.type === 'editorial');
  const software = allReposCache.filter(r => r.type !== 'editorial');
  const drafts = editorials.filter(r => r.current_stage === 'draft').length;
  const published = editorials.reduce((acc, r) => acc + (r.editorial_tags || 0), 0);

  app.innerHTML = `
    <header class="hero-editorial">
      <div style="display:flex; justify-content:space-between; align-items:flex-start;">
        <div>
          <div class="hero-kicker">GOVERNANÇA EDITORIAL CIENTÍFICA · C++26</div>
          <h1 class="hero-title">SisTer Git Editorial</h1>
        </div>
        <div style="background:rgba(255,255,255,0.08); border:1px solid rgba(255,255,255,0.15); border-radius:12px; padding:8px 10px; display:grid; place-items:center;">
          <svg viewBox="0 0 44 44" width="44" height="44" fill="none" stroke="currentColor" stroke-width="2.3" stroke-linecap="round" stroke-linejoin="round" style="color:var(--sky);">
            <path d="M8 34c5-2.5 10-2.5 14 1 4-3.5 9-3.5 14-1v-17c-5-2.5-10-2.5-14 1-4-3.5-9-3.5-14-1z"/>
            <path d="M22 35V16"/>
            <path d="M14 23c-3-2-5-5-5-10"/>
            <path d="M30 23c3-2 5-5 5-10"/>
            <path d="M22 16c-1.5-3.5 0-7 4.5-10.5 0 4.5-2 8-4.5 10.5z" fill="currentColor"/>
            <circle cx="9" cy="13" r="3" fill="#e5b94f"/>
            <circle cx="35" cy="13" r="3" fill="#2ea8e8"/>
            <circle cx="22" cy="35" r="2.4" fill="currentColor"/>
          </svg>
        </div>
      </div>
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

  const cardStageLabels = { draft: 'Rascunho', submitted: 'Submetido', in_review: 'Em revisão', revision_required: 'Revisões requeridas', resubmitted: 'Ressubmetido', accepted: 'Aceito', rejected: 'Rejeitado', withdrawn: 'Retirado', published: 'Publicado' };
  const tagBadge = r.editorial_tags > 0
    ? `<span class="badge-edition-tag">${r.editorial_tags} edição(ões)</span>`
    : `<span class="badge-stage stage-draft">${esc(cardStageLabels[r.current_stage] || 'Rascunho')}</span>`;

  const lastCommitInfo = r.last_commit
    ? `Versão salva ${timeAgo(r.last_commit.date)} por <strong>${esc(r.last_commit.author)}</strong>`
    : 'Aguardando primeiro manuscrito';

  return `
    <article class="article-card" data-search="${esc((r.name + ' ' + (r.title || '') + ' ' + (r.last_commit?.subject || '')).toLowerCase())}">
      <div class="article-card-head">
        <a href="#/editorial/${encodeURIComponent(r.name)}" class="article-title-link">
          ${esc(r.title || r.name)}
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

function visBadge(v, repo = '', owner = '') {
  const isPrivate = v === 'private';
  const label = isPrivate ? '🔒 Privado' : '🌐 Público';
  const canToggle = repo && currentUser && (currentUser.role === 'admin' || !owner || owner === currentUser.email);
  if (canToggle) {
    const nextVis = isPrivate ? 'public' : 'private';
    return `
      <button type="button" class="btn-vis-toggle ${isPrivate ? 'is-private' : 'is-public'}" onclick="toggleRepoVisibility('${esc(repo)}', '${nextVis}')" title="Clique para alterar a visibilidade para ${nextVis === 'public' ? 'Público' : 'Privado'}">
        <span>${label}</span>
        <span style="font-size:0.68rem; opacity:0.8;">▾</span>
      </button>
    `;
  }
  return isPrivate
    ? `<span class="badge-vis vis-private" title="Apenas o autor e administrador têm acesso">${label}</span>`
    : `<span class="badge-vis vis-public" title="Acesso livre para leitura">${label}</span>`;
}

window.toggleRepoVisibility = async function(repoName, newVisibility) {
  const targetLabel = newVisibility === 'public' ? 'PÚBLICO (visível a todos na rede local)' : 'PRIVADO (visível apenas a você e ao administrador)';
  if (!confirm(`Deseja alterar a visibilidade de '${repoName}' para ${targetLabel}?`)) return;
  try {
    await apiPost(`/api/v1/repos/${encodeURIComponent(repoName)}/visibility`, { visibility: newVisibility });
    showToast(`✓ Visibilidade alterada para ${newVisibility === 'public' ? 'Público 🌐' : 'Privado 🔒'}`);
    render();
  } catch (err) {
    alert('Erro ao alterar visibilidade: ' + err.message);
  }
};

// ============================================================================
// VIEW 2: ESPAÇO DOCUMENTAL (LEITURA, EDITOR, PARECERES, PUBLICAÇÃO)
// ============================================================================

function formatIcon(ext) {
  switch (ext) {
    case 'docx': case 'doc': return '📘';
    case 'odt': return '📙';
    case 'pdf': return '📕';
    case 'tex': return '📐';
    case 'md': case 'markdown': return '📄';
    case 'txt': return '📝';
    case 'csv': case 'xlsx': case 'ods': return '📊';
    case 'png': case 'jpg': case 'jpeg': case 'svg': return '🖼️';
    default: return '📁';
  }
}

function formatLabel(ext) {
  switch (ext) {
    case 'docx': case 'doc': return 'Microsoft Word (.docx)';
    case 'odt': return 'LibreOffice Writer (.odt)';
    case 'pdf': return 'Documento PDF (.pdf)';
    case 'tex': return 'LaTeX (.tex)';
    case 'md': case 'markdown': return 'Markdown (.md)';
    case 'txt': return 'Texto Simples (.txt)';
    case 'csv': return 'Planilha / Dados CSV (.csv)';
    default: return (ext || 'ARQUIVO').toUpperCase();
  }
}

// ============================================================================
// COMPARAÇÃO DE REVISÕES & DIFF EDITORIAL
// ============================================================================

window.updateCompareRoute = function(repo, base, head, view, isEditorial) {
  const v = view || (isEditorial ? 'editorial' : 'unified');
  if (isEditorial) {
    location.hash = `#/editorial/${encodeURIComponent(repo)}/diff?base=${encodeURIComponent(base)}&head=${encodeURIComponent(head)}&view=${encodeURIComponent(v)}`;
  } else {
    location.hash = `#/r/${encodeURIComponent(repo)}/compare?base=${encodeURIComponent(base)}&head=${encodeURIComponent(head)}&view=${encodeURIComponent(v)}`;
  }
};

window.switchCompareViewMode = function(repo, base, head, view, isEditorial) {
  window.updateCompareRoute(repo, base, head, view, isEditorial);
};

window.swapCompareRevisions = function(repo, view, isEditorial) {
  const b = document.querySelector('#compareBaseSelect')?.value || 'HEAD~1';
  const h = document.querySelector('#compareHeadSelect')?.value || 'HEAD';
  window.updateCompareRoute(repo, h, b, view, isEditorial);
};

function renderEditorialProseDiff(lines) {
  if (!lines || !lines.length) {
    return `<div style="color:var(--muted); font-style:italic;">Nenhuma alteração neste documento.</div>`;
  }
  let html = '';
  for (const l of lines) {
    if (l.type === 'hunk') {
      html += `<div class="editorial-hunk-divider"><span>Trecho Alterado do Manuscrito</span></div>`;
    } else if (l.type === 'add') {
      html += `
        <div class="editorial-line-block editorial-added">
          <span style="font-weight:700; font-family:var(--font-sans); font-size:0.7rem; text-transform:uppercase; background:#bbf7d0; color:#14532d; padding:2px 6px; border-radius:3px; margin-right:8px; vertical-align:middle;">+ Inserido</span>
          <span>${esc(l.text)}</span>
        </div>`;
    } else if (l.type === 'del') {
      html += `
        <div class="editorial-line-block editorial-deleted">
          <span style="font-weight:700; font-family:var(--font-sans); font-size:0.7rem; text-transform:uppercase; background:#fecaca; color:#7f1d1d; padding:2px 6px; border-radius:3px; margin-right:8px; vertical-align:middle; text-decoration:none; display:inline-block;">- Removido</span>
          <span>${esc(l.text)}</span>
        </div>`;
    } else {
      if (!l.text || !l.text.trim()) {
        html += `<div style="height:12px;"></div>`;
      } else {
        html += `<div class="editorial-line-block" style="color:#334155;">${esc(l.text)}</div>`;
      }
    }
  }
  return html;
}

function renderSplitDiff(lines, baseLabel, headLabel) {
  if (!lines || !lines.length) {
    return `<div style="color:var(--muted); padding:20px; text-align:center;">Sem alterações.</div>`;
  }
  const rows = [];
  let i = 0;
  while (i < lines.length) {
    const l = lines[i];
    if (l.type === 'hunk') {
      rows.push({ type: 'hunk', text: l.text });
      i++;
    } else if (l.type === 'ctx') {
      rows.push({ type: 'ctx', left: l.text, right: l.text });
      i++;
    } else {
      const dels = [];
      const adds = [];
      while (i < lines.length && (lines[i].type === 'del' || lines[i].type === 'add')) {
        if (lines[i].type === 'del') dels.push(lines[i].text);
        else adds.push(lines[i].text);
        i++;
      }
      const maxLen = Math.max(dels.length, adds.length);
      for (let k = 0; k < maxLen; k++) {
        rows.push({
          type: 'change',
          left: k < dels.length ? dels[k] : null,
          right: k < adds.length ? adds[k] : null
        });
      }
    }
  }

  return `
    <table class="diff-split-table">
      <thead>
        <tr>
          <th>Versão Base: <code>${esc(baseLabel)}</code></th>
          <th>Versão Alvo: <code>${esc(headLabel)}</code></th>
        </tr>
      </thead>
      <tbody>
        ${rows.map(r => {
          if (r.type === 'hunk') {
            return `<tr><td colspan="2" class="diff-hunk" style="padding:6px 12px; font-weight:600;">${esc(r.text)}</td></tr>`;
          }
          if (r.type === 'ctx') {
            return `<tr><td style="color:#64748b;">${esc(r.left)}</td><td style="color:#64748b;">${esc(r.right)}</td></tr>`;
          }
          const leftCls = r.left !== null ? 'split-del' : 'split-empty';
          const rightCls = r.right !== null ? 'split-add' : 'split-empty';
          return `
            <tr>
              <td class="${leftCls}">${r.left !== null ? esc(r.left) : ''}</td>
              <td class="${rightCls}">${r.right !== null ? esc(r.right) : ''}</td>
            </tr>
          `;
        }).join('')}
      </tbody>
    </table>
  `;
}

function renderUnifiedDiff(lines) {
  return `
    <div>
      ${lines.map(l => {
        const cls = l.type === 'add' ? 'diff-line-add' : (l.type === 'del' ? 'diff-line-del' : (l.type === 'hunk' ? 'diff-hunk' : ''));
        return `<div class="diff-line ${cls}">${esc(l.text)}</div>`;
      }).join('')}
    </div>
  `;
}

async function buildCompareViewHtml({ repo, base, head, viewMode, isEditorial, mainDoc }) {
  const [commitsData, refsData] = await Promise.all([
    api(`/api/v1/repos/${encodeURIComponent(repo)}/commits?ref=HEAD`).catch(() => ({ commits: [] })),
    api(`/api/v1/repos/${encodeURIComponent(repo)}/refs`).catch(() => ({ branches: [], tags: [] }))
  ]);

  const commits = commitsData.commits || [];
  const tags = refsData.tags || [];

  const rootCommit = commits.length > 0 ? commits[commits.length - 1] : null;

  let effectiveBase = base;
  let effectiveHead = head || 'HEAD';

  if (!effectiveBase) {
    if (isEditorial && rootCommit && commits.length > 1) {
      effectiveBase = rootCommit.short_hash;
    } else {
      effectiveBase = 'HEAD~1';
    }
  }

  const effectiveView = viewMode || (isEditorial ? 'editorial' : 'unified');

  const diffData = await api(`/api/v1/repos/${encodeURIComponent(repo)}/compare?base=${encodeURIComponent(effectiveBase)}&head=${encodeURIComponent(effectiveHead)}`).catch(() => ({ files: [] }));
  const files = diffData.files || [];

  const isMetaFile = (p) => p === '.sister-editorial.json' || p === '.gitignore';
  const docFiles = files.filter(f => !isMetaFile(f.path));
  const metaFiles = files.filter(f => isMetaFile(f.path));

  docFiles.sort((a, b) => {
    if (a.path === mainDoc) return -1;
    if (b.path === mainDoc) return 1;
    return a.path.localeCompare(b.path);
  });

  const totalAdd = files.reduce((acc, f) => acc + (f.additions || 0), 0);
  const totalDel = files.reduce((acc, f) => acc + (f.deletions || 0), 0);

  let html = `
    <div class="compare-control-card">
      <div style="display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px; margin-bottom:14px;">
        <div>
          <div style="font-size:0.95rem; font-weight:700; color:var(--navy); display:flex; align-items:center; gap:8px;">
            <span>⇄ Comparador de Versões e Auditoria de Texto</span>
            ${isEditorial ? '<span class="badge-edition-tag">Espaço Editorial</span>' : ''}
          </div>
          <div style="font-size:0.78rem; color:var(--muted); margin-top:2px;">
            Comparando <strong>${esc(effectiveBase)}</strong> ➔ <strong>${esc(effectiveHead)}</strong> · ${files.length} arquivo(s) modificado(s) (${totalAdd} adições, ${totalDel} remoções)
          </div>
        </div>

        <div class="compare-view-modes">
          <button type="button" class="compare-mode-btn ${effectiveView === 'editorial' ? 'active' : ''}" onclick="switchCompareViewMode('${esc(repo)}', '${esc(effectiveBase)}', '${esc(effectiveHead)}', 'editorial', ${isEditorial})">
            👁️ Visual Editorial
          </button>
          <button type="button" class="compare-mode-btn ${effectiveView === 'split' ? 'active' : ''}" onclick="switchCompareViewMode('${esc(repo)}', '${esc(effectiveBase)}', '${esc(effectiveHead)}', 'split', ${isEditorial})">
            ⚖ Lado a Lado
          </button>
          <button type="button" class="compare-mode-btn ${effectiveView === 'unified' ? 'active' : ''}" onclick="switchCompareViewMode('${esc(repo)}', '${esc(effectiveBase)}', '${esc(effectiveHead)}', 'unified', ${isEditorial})">
            💻 Patch Git
          </button>
        </div>
      </div>

      <div class="compare-selector-row">
        <div style="flex:1; min-width:240px;">
          <label style="display:block; font-size:0.75rem; font-weight:700; color:var(--muted); margin-bottom:4px;">
            VERSÃO BASE (ORIGEM / ANTERIOR):
          </label>
          <select id="compareBaseSelect" class="compare-rev-select" onchange="updateCompareRoute('${esc(repo)}', this.value, document.querySelector('#compareHeadSelect').value, '${esc(effectiveView)}', ${isEditorial})">
            <optgroup label="Referências Especiais">
              <option value="HEAD~1" ${effectiveBase === 'HEAD~1' ? 'selected' : ''}>HEAD~1 (Penúltimo commit)</option>
              ${rootCommit ? `<option value="${esc(rootCommit.short_hash)}" ${effectiveBase === rootCommit.short_hash || effectiveBase === rootCommit.hash ? 'selected' : ''}>Origem / Primeiro Commit (${esc(rootCommit.short_hash)})</option>` : ''}
            </optgroup>
            ${tags.length ? `
              <optgroup label="Tags &amp; Edições Publicadas">
                ${tags.map(t => `<option value="${esc(t.name)}" ${effectiveBase === t.name ? 'selected' : ''}>🏷️ ${esc(t.name)}</option>`).join('')}
              </optgroup>
            ` : ''}
            <optgroup label="Histórico de Commits Recentes">
              ${commits.map(c => `
                <option value="${esc(c.short_hash)}" ${effectiveBase === c.short_hash || effectiveBase === c.hash ? 'selected' : ''}>
                  ${esc(c.short_hash)} · ${esc(c.subject.substr(0, 48))} (${timeAgo(c.date)})
                </option>
              `).join('')}
            </optgroup>
          </select>
        </div>

        <button type="button" class="compare-swap-btn" title="Inverter revisões" onclick="swapCompareRevisions('${esc(repo)}', '${esc(effectiveView)}', ${isEditorial})">
          ⇄
        </button>

        <div style="flex:1; min-width:240px;">
          <label style="display:block; font-size:0.75rem; font-weight:700; color:var(--muted); margin-bottom:4px;">
            VERSÃO ALVO (DESTINO / ATUAL):
          </label>
          <select id="compareHeadSelect" class="compare-rev-select" onchange="updateCompareRoute('${esc(repo)}', document.querySelector('#compareBaseSelect').value, this.value, '${esc(effectiveView)}', ${isEditorial})">
            <optgroup label="Referências Especiais">
              <option value="HEAD" ${effectiveHead === 'HEAD' ? 'selected' : ''}>HEAD (Versão Mais Recente)</option>
            </optgroup>
            ${tags.length ? `
              <optgroup label="Tags &amp; Edições Publicadas">
                ${tags.map(t => `<option value="${esc(t.name)}" ${effectiveHead === t.name ? 'selected' : ''}>🏷️ ${esc(t.name)}</option>`).join('')}
              </optgroup>
            ` : ''}
            <optgroup label="Histórico de Commits">
              ${commits.map(c => `
                <option value="${esc(c.short_hash)}" ${effectiveHead === c.short_hash || effectiveHead === c.hash ? 'selected' : ''}>
                  ${esc(c.short_hash)} · ${esc(c.subject.substr(0, 48))} (${timeAgo(c.date)})
                </option>
              `).join('')}
            </optgroup>
          </select>
        </div>
      </div>

      <div class="compare-shortcuts-row">
        <span style="font-weight:600;">Atalhos rápidos:</span>
        ${rootCommit ? `
          <button type="button" class="compare-shortcut-btn" onclick="updateCompareRoute('${esc(repo)}', '${esc(rootCommit.short_hash)}', 'HEAD', '${esc(effectiveView)}', ${isEditorial})">
            📌 Origem ➔ Versão Atual
          </button>
        ` : ''}
        ${commits.length > 1 ? `
          <button type="button" class="compare-shortcut-btn" onclick="updateCompareRoute('${esc(repo)}', 'HEAD~1', 'HEAD', '${esc(effectiveView)}', ${isEditorial})">
            ⏱️ Último Commit (HEAD~1 ➔ HEAD)
          </button>
        ` : ''}
        ${tags.map(t => `
          <button type="button" class="compare-shortcut-btn" onclick="updateCompareRoute('${esc(repo)}', '${esc(t.name)}', 'HEAD', '${esc(effectiveView)}', ${isEditorial})">
            🏷️ ${esc(t.name)} ➔ Atual
          </button>
        `).join('')}
      </div>
    </div>
  `;

  if (!files.length) {
    html += `
      <div class="empty-state" style="background:#fff; border:1px solid var(--line); border-radius:var(--radius-lg); padding:40px; text-align:center;">
        <div style="font-size:2rem; margin-bottom:8px;">✓</div>
        <h3 style="color:var(--navy); margin:0 0 6px;">Nenhuma diferença encontrada</h3>
        <p style="color:var(--muted); margin:0;">Os conteúdos de <code>${esc(effectiveBase)}</code> e <code>${esc(effectiveHead)}</code> são exatamente idênticos.</p>
      </div>
    `;
    return html;
  }

  for (const f of docFiles) {
    if (effectiveView === 'editorial') {
      html += `
        <div class="editorial-diff-card">
          <div class="editorial-diff-header">
            <div class="editorial-diff-doc-title">
              <span>📄</span>
              <span>${esc(f.path)}</span>
              ${f.path === mainDoc ? '<span class="badge-edition-tag">Documento Principal</span>' : ''}
            </div>
            <div style="display:flex; align-items:center; gap:8px;">
              <span class="editorial-diff-stat-add">+${f.additions || 0} adições</span>
              <span class="editorial-diff-stat-del">-${f.deletions || 0} remoções</span>
            </div>
          </div>
          <div class="editorial-prose-container">
            ${renderEditorialProseDiff(f.lines)}
          </div>
        </div>
      `;
    } else if (effectiveView === 'split') {
      html += `
        <div class="diff-box">
          <div class="diff-file-head" style="display:flex; justify-content:space-between; align-items:center;">
            <span>📄 ${esc(f.path)} ${f.path === mainDoc ? '<span class="badge-edition-tag">Documento Principal</span>' : ''}</span>
            <span style="font-size:0.75rem;"><strong style="color:var(--diff-add-text);">+${f.additions || 0}</strong> / <strong style="color:var(--diff-del-text);">-${f.deletions || 0}</strong></span>
          </div>
          ${renderSplitDiff(f.lines, effectiveBase, effectiveHead)}
        </div>
      `;
    } else {
      html += `
        <div class="diff-box">
          <div class="diff-file-head" style="display:flex; justify-content:space-between; align-items:center;">
            <span>${esc(f.path)}</span>
            <span style="font-size:0.75rem;"><strong style="color:var(--diff-add-text);">+${f.additions || 0}</strong> / <strong style="color:var(--diff-del-text);">-${f.deletions || 0}</strong></span>
          </div>
          ${renderUnifiedDiff(f.lines)}
        </div>
      `;
    }
  }

  for (const f of metaFiles) {
    html += `
      <details class="diff-box" style="margin-top:20px;">
        <summary class="metadata-diff-summary">
          <div style="display:flex; align-items:center; gap:8px;">
            <span>⚙</span>
            <strong>Metadados do Sistema (${esc(f.path)})</strong>
            <span style="font-size:0.75rem; color:var(--muted);">(Arquivo de infraestrutura e manifesto editorial)</span>
          </div>
          <div style="display:flex; align-items:center; gap:8px;">
            <span class="editorial-diff-stat-add">+${f.additions || 0}</span>
            <span class="editorial-diff-stat-del">-${f.deletions || 0}</span>
            <span style="font-size:0.78rem; color:var(--blue); font-weight:600;">Ver detalhes técnicos ▾</span>
          </div>
        </summary>
        <div style="background:#f8fafc; border-top:1px solid var(--line); padding:10px 0;">
          ${renderUnifiedDiff(f.lines)}
        </div>
      </details>
    `;
  }

  return html;
}

async function renderEditorialDoc(repo, tab = 'manuscript', options = {}) {
  crumbCurrent.innerHTML = `<a href="#/editorial">Espaço Editorial</a> / <strong>${esc(repo)}</strong>`;

  const [edData, treeData, refsData] = await Promise.all([
    api(`/api/v1/repos/${encodeURIComponent(repo)}/editorial`),
    api(`/api/v1/repos/${encodeURIComponent(repo)}/tree?ref=HEAD`).catch(() => ({ entries: [] })),
    api(`/api/v1/repos/${encodeURIComponent(repo)}/refs`).catch(() => ({ branches: [], tags: [] }))
  ]);

  const manifest = edData.manifest || {};
  window.sisterEditorialHeads = window.sisterEditorialHeads || {};
  window.sisterEditorialHeads[repo] = edData.head || '';
  const entries = treeData.entries || [];
  const title = manifest.title || repo;
  const authors = manifest.authors && manifest.authors.length
    ? manifest.authors.map(a => a.name).join(', ')
    : 'Pesquisador LABECO';
  const editions = (edData.tags || []);
  let stage = manifest.current_stage;
  if (!stage) {
    const hasPublishedTag = editions.some(t => /publicad|publish|release/i.test(t.name));
    const hasSubmissionTag = editions.some(t => /submis|review/i.test(t.name));
    if (hasPublishedTag) {
      stage = 'published';
    } else if (hasSubmissionTag) {
      stage = 'in_review';
    } else {
      stage = 'draft';
    }
  }
  const reviews = manifest.reviews || [];
  const responses = manifest.responses || [];
  const canWrite = currentUser && (currentUser.role === 'admin' || edData.project_role === 'owner' || edData.project_role === 'editor');
  const canDecide = currentUser && (currentUser.role === 'admin' || edData.project_role === 'editor');

  // Detect main document
  let mainDoc = manifest.document_main;
  if (!mainDoc || !entries.some(e => e.name === mainDoc)) {
    const candidate = entries.find(e => /\.(docx|odt|tex|md|txt)$/i.test(e.name));
    mainDoc = candidate ? candidate.name : (entries.length ? entries[0].name : 'manuscrito.md');
  }
  const ext = (mainDoc.split('.').pop() || '').toLowerCase();
  const isTextDoc = /^(md|markdown|txt|tex)$/i.test(ext);

  const blobData = await api(`/api/v1/repos/${encodeURIComponent(repo)}/blob?ref=HEAD&path=${encodeURIComponent(mainDoc)}`).catch(() => null);
  const rawMarkdown = blobData && !blobData.binary ? blobData.content : '# ' + title + '\n\nInicie a escrita do seu manuscrito.';

  const stageLabels = {
    draft: 'Rascunho Ativo', submitted: 'Submetido', in_review: 'Em Revisão por Pares',
    revision_required: 'Revisões Requeridas', resubmitted: 'Ressubmetido',
    accepted: 'Aceito para Publicação', rejected: 'Rejeitado', withdrawn: 'Retirado', published: '✓ Publicado'
  };
  const stageClass = stage === 'published' ? 'stage-published' : (stage === 'draft' ? 'stage-draft' : 'stage-in_review');
  const stageBadge = `<span class="badge-stage ${stageClass}">${esc(stageLabels[stage] || stage)}</span>`;
  const transitionButtons = currentUser ? ({
    draft: `<button class="btn-primary" onclick="handleEditorialTransition('${esc(repo)}','submitted')">Submeter para avaliação</button>`,
    submitted: canDecide ? `<button class="btn-primary" onclick="handleEditorialTransition('${esc(repo)}','in_review')">Iniciar revisão</button>` : '',
    in_review: canDecide ? `<button class="btn-cancel" onclick="handleEditorialTransition('${esc(repo)}','revision_required')">Solicitar revisões</button><button class="btn-primary" onclick="handleEditorialTransition('${esc(repo)}','accepted')">Aceitar manuscrito</button><button class="btn-cancel" onclick="handleEditorialTransition('${esc(repo)}','rejected')">Rejeitar</button>` : '',
    revision_required: `<button class="btn-primary" onclick="handleEditorialTransition('${esc(repo)}','resubmitted')">Ressubmeter versão revisada</button>`,
    resubmitted: canDecide ? `<button class="btn-primary" onclick="handleEditorialTransition('${esc(repo)}','in_review')">Abrir nova rodada</button>` : '',
    accepted: canDecide ? `<a class="btn-primary" href="#/editorial/${encodeURIComponent(repo)}/publish">Homologar publicação</a>` : '',
    published: `<button class="btn-primary" onclick="handleEditorialTransition('${esc(repo)}','draft')">Iniciar nova edição</button>`
  }[stage] || '') : '';

  let tabContent = '';

  if (tab === 'manuscript') {
    tabContent = `
      <div class="manuscript-file-card">
        <div class="manuscript-file-info">
          <div class="manuscript-icon-box">${formatIcon(ext)}</div>
          <div>
            <div style="display:flex; align-items:center; gap:8px;">
              <h3 style="margin:0; font-size:1.15rem; color:var(--navy);">${esc(mainDoc)}</h3>
              <span class="badge-edition-tag">Documento Principal</span>
            </div>
            <p style="margin:3px 0 0; color:var(--muted); font-size:0.8rem;">
              Formato: <strong>${formatLabel(ext)}</strong> · Tamanho: ${fmt(blobData?.size || 0)}
            </p>
          </div>
        </div>
        <div style="display:flex; gap:10px; align-items:center; flex-wrap:wrap;">
          <a href="/api/v1/repos/${encodeURIComponent(repo)}/raw?path=${encodeURIComponent(mainDoc)}" onclick="event.preventDefault(); downloadFile(this.href, '${esc(mainDoc)}')" download="${esc(mainDoc)}" class="btn-primary" style="padding:8px 16px; font-size:0.84rem; text-decoration:none;" title="Baixar arquivo original na íntegra">
            ⬇ Baixar Arquivo Original (.${esc(ext)})
          </a>
          <a href="#/editorial/${encodeURIComponent(repo)}/upload" class="btn-cancel" style="padding:8px 16px; font-size:0.84rem; text-decoration:none; background:#fff;">
            📤 Enviar Nova Versão (.docx, .odt...)
          </a>
        </div>
      </div>

      <div style="display:flex; justify-content:space-between; align-items:center; background:#f0fdf4; border:1px solid #bbf7d0; border-radius:var(--radius-md); padding:10px 16px; margin-bottom:16px; font-size:0.83rem; flex-wrap:wrap; gap:8px;">
        <span style="color:#166534;">
          <strong>Auditoria Editorial:</strong> Deseja acompanhar as mudanças de redação, inclusões e correções entre versões?
        </span>
        <a href="#/editorial/${encodeURIComponent(repo)}/diff" class="btn-cancel" style="background:#fff; color:#166534; border-color:#86efac; font-size:0.78rem; padding:4px 10px; text-decoration:none; font-weight:600;">
          ⇄ Comparar Alterações do Manuscrito
        </a>
      </div>

      ${isTextDoc && blobData && !blobData.binary ? `
        <article class="manuscript-reader markdown-body">
          ${renderMarkdown(rawMarkdown)}
          <hr>
          <footer style="font-size:0.8rem; color:var(--muted); text-align:center; padding-top:10px;">
            <em>Preservado sob governança SisTer Git Editorial · LABECO · Sempre pronto. Sempre incompleto!</em>
          </footer>
        </article>
      ` : `
        <div style="background:#fff; border:1px solid var(--line); border-radius:var(--radius-lg); padding:40px 32px; text-align:center; max-width:820px; margin:0 auto 30px; box-shadow:var(--shadow-sm);">
          <span style="font-size:3.2rem; display:block; margin-bottom:14px;">${formatIcon(ext)}</span>
          <h2 style="color:var(--navy); margin:0 0 10px; font-size:1.3rem;">Manuscrito em Formato ${formatLabel(ext)}</h2>
          <p style="color:var(--muted); font-size:0.92rem; max-width:580px; margin:0 auto 24px; line-height:1.65;">
            Este documento é redigido externamente no seu editor de texto preferido (Word ou LibreOffice). Baixe o arquivo original abaixo, faça suas revisões localmente e envie a nova versão na aba <strong>Upload &amp; Arquivos</strong> para registrar o histórico com integridade e proveniência Git.
          </p>
          <div style="display:flex; justify-content:center; gap:12px; flex-wrap:wrap;">
            <a href="/api/v1/repos/${encodeURIComponent(repo)}/raw?path=${encodeURIComponent(mainDoc)}" onclick="event.preventDefault(); downloadFile(this.href, '${esc(mainDoc)}')" download="${esc(mainDoc)}" class="btn-primary" style="padding:10px 22px; font-size:0.9rem; text-decoration:none;">
              ⬇ Baixar ${esc(mainDoc)} (${fmt(blobData?.size || 0)})
            </a>
            <a href="#/editorial/${encodeURIComponent(repo)}/upload" class="btn-cancel" style="padding:10px 22px; font-size:0.9rem; text-decoration:none; background:#fff;">
              📤 Enviar Nova Versão Revisada
            </a>
          </div>
        </div>
      `}
    `;
  } else if (tab === 'upload') {
    tabContent = `
      <div style="max-width:960px; margin:0 auto 30px;">
        <div class="reviews-toolbar" style="margin-bottom:20px;">
          <div>
            <h3 style="margin:0; color:var(--navy); font-size:1.15rem;">Upload &amp; Custódia de Arquivos do Projeto</h3>
            <p style="margin:3px 0 0; color:var(--muted); font-size:0.82rem;">Envie manuscritos em Word (.docx), LibreOffice (.odt), LaTeX (.tex) ou Markdown (.md) diretamente para a árvore Git</p>
          </div>
        </div>

        ${!canWrite ? '<div class="notice-box"><strong>Modo somente leitura</strong><p>Upload disponível apenas ao autor responsável ou editor.</p></div>' : ''}
        <form id="formUploadDoc" onsubmit="handleUploadDocument(event, '${esc(repo)}')" style="${canWrite ? '' : 'display:none;'} background:#fff; border:1px solid var(--line); border-radius:var(--radius-lg); padding:28px; box-shadow:var(--shadow-sm);">
          <div class="upload-dropzone" onclick="document.querySelector('#uploadFileInput').click()" ondragover="event.preventDefault()" ondrop="handleDropFile(event)">
            <span class="upload-dropzone-icon">📥</span>
            <strong style="color:var(--navy); font-size:1rem; display:block;">Clique ou arraste o arquivo aqui para enviar</strong>
            <span style="font-size:0.8rem; color:var(--muted); display:block; margin-top:4px;">Suporta processadores de texto, código e anexos científicos</span>
            <div class="upload-formats-row">
              <span class="format-chip">📘 .docx</span>
              <span class="format-chip">📙 .odt</span>
              <span class="format-chip">📐 .tex</span>
              <span class="format-chip">📄 .md</span>
              <span class="format-chip">📝 .txt</span>
              <span class="format-chip">📕 .pdf</span>
              <span class="format-chip">📊 .csv</span>
              <span class="format-chip">🖼️ .png/.jpg</span>
            </div>
            <input type="file" id="uploadFileInput" style="display:none;" onchange="handleFileSelected(this)">
            <div id="uploadSelectedFileName" style="margin-top:14px; font-weight:700; color:var(--blue); font-size:0.9rem;"></div>
          </div>

          <div style="display:grid; grid-template-columns:1fr auto; gap:12px; align-items:flex-end;">
            <label class="field-label" style="margin:0;">
              <span>Descrição das Alterações / Mensagem da Versão Git <small>Obrigatório</small></span>
              <input type="text" id="uploadCommitMsg" required placeholder="Ex.: Versão 2 incorporando apontamentos dos revisores (DOCX)">
            </label>
            <div style="padding-bottom:6px;">
              <label style="display:flex; align-items:center; gap:6px; font-size:0.8rem; font-weight:600; color:var(--navy); cursor:pointer;">
                <input type="checkbox" id="uploadIsMain" checked>
                <span>Definir como manuscrito principal</span>
              </label>
            </div>
          </div>

          <div id="uploadFeedback" class="form-feedback hidden" style="margin-top:12px;"></div>

          <div style="margin-top:16px; display:flex; justify-content:flex-end;">
            <button type="submit" class="btn-primary" id="btnSubmitUpload" style="padding:9px 22px;">
              Gravar Nova Versão no Git ⎇
            </button>
          </div>
        </form>

        <!-- LISTA DE ARQUIVOS NO REPOSITÓRIO -->
        <div class="file-list-card">
          <div class="file-list-header">
            <span>Arquivos Armazenados no Nó LABECO (${entries.length})</span>
            <a href="#/r/${encodeURIComponent(repo)}/code" style="font-size:0.78rem; color:var(--blue); text-decoration:none;">Ver Árvore Git Completa ➔</a>
          </div>
          <table class="tree-table" style="border:none; border-radius:0; box-shadow:none;">
            <thead>
              <tr>
                <th>Arquivo</th>
                <th>Tipo</th>
                <th>SHA</th>
                <th style="text-align:right;">Ações</th>
              </tr>
            </thead>
            <tbody>
              ${entries.length === 0 ? `
                <tr><td colspan="4" style="text-align:center; padding:24px; color:var(--muted);">Nenhum arquivo no repositório. Envie o primeiro arquivo acima!</td></tr>
              ` : entries.map(e => {
                const eExt = (e.name.split('.').pop() || '').toLowerCase();
                const isMain = e.name === mainDoc;
                return `
                  <tr>
                    <td>
                      <span style="font-size:1.1rem; margin-right:6px;">${formatIcon(eExt)}</span>
                      <strong>${esc(e.name)}</strong>
                      ${isMain ? '<span class="badge-edition-tag" style="margin-left:6px;">★ Principal</span>' : ''}
                    </td>
                    <td><span style="font-size:0.75rem; color:var(--muted);">${formatLabel(eExt)}</span></td>
                    <td><code style="font-size:0.75rem; color:var(--muted);">${esc(e.hash.substr(0, 8))}</code></td>
                    <td style="text-align:right;">
                      <div style="display:inline-flex; gap:6px;">
                        <a href="/api/v1/repos/${encodeURIComponent(repo)}/raw?path=${encodeURIComponent(e.name)}" onclick="event.preventDefault(); downloadFile(this.href, '${esc(e.name)}')" download="${esc(e.name)}" class="btn-cancel" style="padding:3px 9px; font-size:0.75rem; background:#fff; text-decoration:none;" title="Baixar arquivo original">⬇ Baixar</a>
                        <a href="#/r/${encodeURIComponent(repo)}/code?path=${encodeURIComponent(e.name)}" class="btn-cancel" style="padding:3px 9px; font-size:0.75rem; background:#fff; text-decoration:none;">Git</a>
                      </div>
                    </td>
                  </tr>
                `;
              }).join('')}
            </tbody>
          </table>
        </div>
      </div>
    `;
  } else if (tab === 'editor') {
    let draftRecord = null;
    try {
      const storedDraft = localStorage.getItem('sister_draft_' + repo);
      if (storedDraft) {
        try { draftRecord = JSON.parse(storedDraft); }
        catch (_) { draftRecord = { content: storedDraft, base_head: '', legacy: true }; }
      }
    } catch (_) {}
    const draftContent = draftRecord && typeof draftRecord.content === 'string' ? draftRecord.content : '';
    const draftDiverged = !!draftContent && (!draftRecord.base_head || draftRecord.base_head !== edData.head);
    const textToShow = draftContent && !draftDiverged ? draftContent : rawMarkdown;

    tabContent = !isTextDoc ? `
      <div class="empty-state" style="background:#fff; padding:40px; text-align:center;">
        <h3>Editor rápido indisponível para ${esc(formatLabel(ext))}</h3>
        <p>Baixe o arquivo original, edite no aplicativo nativo e envie nova versão pela aba Upload &amp; Arquivos.</p>
      </div>
    ` : !canWrite ? `
      <div class="empty-state" style="background:#fff; padding:40px; text-align:center;"><h3>Modo somente leitura</h3><p>Editor disponível apenas ao autor responsável ou editor.</p></div>
    ` : `
      <div class="split-editor-container">
        ${draftDiverged ? `
          <div class="notice-box" id="divergentDraftNotice" style="margin-bottom:16px;">
            <strong>Rascunho local baseado em outra versão</strong>
            <p>O conteúdo local foi preservado. Escolha conscientemente antes de editar.</p>
            <div style="display:flex; gap:8px; flex-wrap:wrap;">
              <button type="button" class="btn-primary" onclick="recoverLocalDraft('${esc(repo)}')">Recuperar rascunho</button>
              <button type="button" class="btn-cancel" onclick="toggleDraftComparison()">Comparar com Git</button>
              <button type="button" class="btn-cancel" onclick="discardLocalDraft('${esc(repo)}')">Descartar rascunho</button>
            </div>
            <div id="draftComparison" class="hidden" style="display:none; grid-template-columns:1fr 1fr; gap:12px; margin-top:14px;">
              <div><strong>Rascunho local</strong><pre style="white-space:pre-wrap; max-height:280px; overflow:auto;">${esc(draftContent)}</pre></div>
              <div><strong>Versão Git atual</strong><pre style="white-space:pre-wrap; max-height:280px; overflow:auto;">${esc(rawMarkdown)}</pre></div>
            </div>
          </div>
        ` : ''}
        <div class="split-editor-grid">
          <div class="editor-pane">
            <div class="pane-header">
              <span>Editor de Texto / Markdown (${esc(mainDoc)})</span>
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

        <form class="editor-commit-bar" onsubmit="handleSaveManuscript(event, '${esc(repo)}', '${esc(mainDoc)}')">
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
          ${edData.project_role === 'reviewer' || currentUser?.role === 'admin' ? `<button type="button" class="btn-primary" onclick="openNewReviewModal('${esc(repo)}')">+ Emitir Parecer Editorial</button>` : ''}
          ${currentUser?.role === 'admin' ? `<button type="button" class="btn-cancel" onclick="assignProjectReviewer('${esc(repo)}')">Designar Revisor</button>` : ''}
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
            ${responses.filter(resp => resp.review_id === r.id).map(resp => `
              <div class="review-resolution-box">
                <strong>Resposta do Autor (${esc(resp.author || '')}):</strong>
                <div>${esc(resp.response || '')}</div>
                <div style="margin-top:4px; font-size:0.75rem; color:var(--muted);">${formatDate(resp.date)} · commit ${esc((resp.commit || '').substr(0, 8))}</div>
              </div>`).join('')}
            ${currentUser && (currentUser.role === 'admin' || edData.owner === currentUser.email) ? `<button type="button" class="btn-cancel" onclick="respondToReview('${esc(repo)}','${esc(r.id || '')}')">Responder ao parecer</button>` : ''}
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
          ${stage === 'accepted' && canDecide
            ? `<button type="button" class="btn-primary" onclick="openPublishModal('${esc(repo)}')">+ Publicar Nova Edição</button>`
            : `<span style="font-size:0.8rem; color:var(--muted);">Publicação disponível após decisão editorial <code>accepted</code>.</span>`}
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
              Objeto Git anotado: <code>${esc(ed.hash.substr(0, 10))}</code>
            </div>
            <div style="display:flex; gap:8px; margin-top:12px; flex-wrap:wrap;">
              <a class="btn-primary" style="font-size:0.76rem; padding:5px 10px; text-decoration:none;" href="/api/v1/repos/${encodeURIComponent(repo)}/archive?ref=${encodeURIComponent(ed.name)}&format=zip" onclick="event.preventDefault(); downloadFile(this.href, '${esc(repo)}-${esc(ed.name.replaceAll('/', '-'))}.zip')">⬇ Baixar edição ZIP</a>
              <a class="btn-cancel" style="font-size:0.76rem; padding:5px 10px; text-decoration:none;" href="#/editorial/${encodeURIComponent(repo)}/diff?base=${encodeURIComponent(ed.name)}&head=HEAD&view=editorial">Comparar com atual</a>
            </div>
          </div>
        `).join('') : `
          <div class="empty-state" style="background:#fff; border:1px solid var(--line); border-radius:var(--radius-lg); padding:36px; text-align:center;">
            <p style="color:var(--muted); margin:0 0 16px;">Nenhuma edição formal publicada ainda.</p>
            ${stage === 'accepted' && canDecide ? `<button class="btn-primary" onclick="openPublishModal('${esc(repo)}')">Publicar Versão v1.0 🚀</button>` : ''}
          </div>
        `}
      </div>
    `;
  } else if (tab === 'diff') {
    tabContent = await buildCompareViewHtml({
      repo,
      base: options.base,
      head: options.head,
      viewMode: options.view,
      isEditorial: true,
      mainDoc
    });
  }

  app.innerHTML = `
    <header class="editorial-doc-header">
      <div class="doc-kicker-bar">
        <span class="doc-kicker">SisTer Git Editorial · Identidade Documental Persistente</span>
        <div class="badges-row">
          ${visBadge(edData.visibility || 'public', repo, edData.owner || '')}
          ${stageBadge}
        </div>
      </div>

      <h1 class="doc-title-main">${esc(title)}</h1>
      <div class="doc-authors-line">
        <strong>${esc(authors)}</strong> · <em>Laboratório de Estudos em AgroEcologia e Recursos Naturais (LABECO)</em>
      </div>

      <div class="doc-provenance-box">
        <span>Identificador: <strong>${esc(repo)}</strong> (refs/heads/main)</span>
        <span>Código Git: <a href="#/r/${encodeURIComponent(repo)}/code" style="color:var(--blue); text-decoration:underline;">Ver arquivos brutos ➔</a></span>
        ${transitionButtons ? `<span style="display:flex; gap:8px; flex-wrap:wrap;">${transitionButtons}</span>` : ''}
      </div>
    </header>

    <nav class="doc-tabs-bar" aria-label="Navegação do Artigo">
      <a href="#/editorial/${encodeURIComponent(repo)}/manuscript" class="doc-tab ${tab === 'manuscript' ? 'active' : ''}">
        <span>📖 Manuscrito &amp; Leitura</span>
      </a>
      <a href="#/editorial/${encodeURIComponent(repo)}/diff" class="doc-tab ${tab === 'diff' ? 'active' : ''}">
        <span>⇄ Comparar Versões</span>
      </a>
      <a href="#/editorial/${encodeURIComponent(repo)}/upload" class="doc-tab ${tab === 'upload' ? 'active' : ''}">
        <span>📤 Upload &amp; Arquivos</span>
        <span class="tab-badge-count">${entries.length}</span>
      </a>
      <a href="#/editorial/${encodeURIComponent(repo)}/editor" class="doc-tab ${tab === 'editor' ? 'active' : ''}">
        <span>✎ Editor Rápido (MD)</span>
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

  if (tab === 'editor' && isTextDoc && canWrite) {
    handleEditorInput(document.querySelector('#liveEditorTextarea')?.value || '', '', false);
  }
}

window.handleEditorInput = function(val, repo, persist = true) {
  const p = document.querySelector('#livePreviewPane');
  const count = document.querySelector('#charCount');
  if (p) p.innerHTML = renderMarkdown(val);
  if (count) count.textContent = `${val.length} caracteres`;
  if (repo && persist) {
    try {
      localStorage.setItem('sister_draft_' + repo, JSON.stringify({
        content: val,
        base_head: (window.sisterEditorialHeads || {})[repo] || '',
        updated_at: new Date().toISOString()
      }));
    } catch (_) {}
  }
};

window.recoverLocalDraft = function(repo) {
  try {
    const raw = localStorage.getItem('sister_draft_' + repo);
    if (!raw) return;
    let record;
    try { record = JSON.parse(raw); } catch (_) { record = { content: raw }; }
    const textarea = document.querySelector('#liveEditorTextarea');
    if (!textarea || typeof record.content !== 'string') return;
    textarea.value = record.content;
    handleEditorInput(record.content, repo);
    document.querySelector('#divergentDraftNotice')?.remove();
    showToast('Rascunho local recuperado. Revise antes de salvar no Git.');
  } catch (_) {}
};

window.discardLocalDraft = function(repo) {
  if (!confirm('Descartar definitivamente o rascunho local divergente?')) return;
  localStorage.removeItem('sister_draft_' + repo);
  document.querySelector('#divergentDraftNotice')?.remove();
  showToast('Rascunho local descartado; versão Git mantida.');
};

window.toggleDraftComparison = function() {
  const comparison = document.querySelector('#draftComparison');
  if (!comparison) return;
  comparison.classList.toggle('hidden');
  comparison.style.display = comparison.classList.contains('hidden') ? 'none' : 'grid';
};

window.handleEditorialTransition = async function(repo, nextStage) {
  const reason = prompt('Justificativa para esta transição editorial:');
  if (reason === null) return;
  if (!reason.trim()) {
    alert('Informe uma justificativa para preservar a trilha editorial.');
    return;
  }
  try {
    await apiPost(`/api/v1/repos/${encodeURIComponent(repo)}/editorial/transition`, {
      to: nextStage,
      reason: reason.trim()
    });
    showToast('✓ Estado editorial atualizado com proveniência Git.');
    await render();
  } catch (err) {
    alert('Erro na transição editorial: ' + err.message);
  }
};

window.handleSaveManuscript = async function(e, repo, filename) {
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
      filename,
      content,
      author: author || 'Pesquisador LABECO',
      message: message || 'Atualização editorial do manuscrito',
      expected_head: (window.sisterEditorialHeads || {})[repo] || ''
    });
    if (res.commit) window.sisterEditorialHeads[repo] = res.commit;
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

window.handleFileSelected = function(input) {
  const el = document.querySelector('#uploadSelectedFileName');
  const file = input && input.files && input.files[0];
  if (!el) return;
  if (file) {
    el.innerHTML = `📄 Arquivo selecionado: <span style="text-decoration:underline;">${esc(file.name)}</span> (${(file.size / 1024).toFixed(1)} KB)`;
    const msgInput = document.querySelector('#uploadCommitMsg');
    if (msgInput && !msgInput.value.trim()) {
      msgInput.value = `Atualização de arquivo: ${file.name}`;
    }
  } else {
    el.innerHTML = '';
  }
};

window.handleDropFile = function(evt) {
  evt.preventDefault();
  evt.stopPropagation();
  const dt = evt.dataTransfer;
  if (!dt || !dt.files || dt.files.length === 0) return;
  const input = document.querySelector('#uploadFileInput');
  if (input) {
    input.files = dt.files;
    window.handleFileSelected(input);
  }
};

window.handleUploadDocument = async function(evt, repo) {
  evt.preventDefault();
  const input = document.querySelector('#uploadFileInput');
  const msgInput = document.querySelector('#uploadCommitMsg');
  const isMainCheck = document.querySelector('#uploadIsMain');
  const fb = document.querySelector('#uploadFeedback');
  const btn = document.querySelector('#btnSubmitUpload');

  if (!input || !input.files || input.files.length === 0) {
    if (fb) {
      fb.className = 'form-feedback error';
      fb.textContent = 'Por favor, selecione um arquivo para upload (.docx, .odt, .tex, .md, .txt, .pdf, etc.)';
      fb.classList.remove('hidden');
    }
    return;
  }

  const file = input.files[0];
  const message = (msgInput ? msgInput.value.trim() : '') || `Upload: ${file.name}`;
  const isMain = isMainCheck ? isMainCheck.checked : true;

  if (fb) {
    fb.className = 'form-feedback';
    fb.textContent = `Lendo e codificando ${file.name}...`;
    fb.classList.remove('hidden');
  }
  if (btn) {
    btn.disabled = true;
    btn.textContent = 'Enviando ao Git...';
  }

  try {
    const arrayBuffer = await file.arrayBuffer();
    const bytes = new Uint8Array(arrayBuffer);
    let binary = '';
    const chunkSize = 8192;
    for (let i = 0; i < bytes.byteLength; i += chunkSize) {
      const chunk = bytes.subarray(i, i + chunkSize);
      binary += String.fromCharCode.apply(null, chunk);
    }
    const b64 = btoa(binary);
    if (file.size > 0 && (!b64 || b64.length === 0)) {
      throw new Error('Falha ao codificar o conteúdo do arquivo.');
    }

    if (fb) fb.textContent = `Gravando commit no repositório ${repo}...`;

    const res = await apiPost(`/api/v1/repos/${encodeURIComponent(repo)}/upload`, {
      filename: file.name,
      content_base64: b64,
      commit_msg: message,
      is_main: isMain,
      expected_head: (window.sisterEditorialHeads || {})[repo] || ''
    });
    if (res.commit) window.sisterEditorialHeads[repo] = res.commit;

    if (res && res.error) {
      throw new Error(res.error);
    }

    showToast(`✓ Arquivo "${file.name}" gravado com sucesso no Git!`);
    if (fb) {
      fb.className = 'form-feedback success';
      fb.textContent = `✓ Versão gravada (Commit ${res.commit ? res.commit.substr(0, 8) : 'OK'})`;
    }

    setTimeout(() => {
      render();
    }, 800);
  } catch (err) {
    console.error('Upload error:', err);
    if (err.message && (err.message.includes('Autenticação') || err.message.includes('Token') || err.message.includes('401'))) {
      openAuthModal();
      const authFb = document.querySelector('#authModalFeedback');
      if (authFb) {
        authFb.textContent = 'Autenticação necessária para enviar arquivos ao Git. Digite o token de pesquisador.';
        authFb.classList.remove('hidden');
      }
    }
    if (fb) {
      fb.className = 'form-feedback error';
      fb.textContent = 'Erro ao enviar arquivo: ' + (err.message || err);
      fb.classList.remove('hidden');
    }
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.textContent = 'Gravar Nova Versão no Git ⎇';
    }
  }
};

// ============================================================================
// VIEW 3: REPOSITÓRIOS DE SOFTWARE (GIT BARE WORKSPACE)
// ============================================================================

async function renderReposDashboard() {
  const data = await api('/api/v1/repos');
  allReposCache = data.repositories || [];

  app.innerHTML = `
    <header class="hero-editorial" style="background:linear-gradient(130deg, #071e38, #0e375d);">
      <div style="display:flex; justify-content:space-between; align-items:flex-start;">
        <div>
          <div class="hero-kicker">INFRAESTRUTURA DISTRIBUÍDA &amp; REPOSITÓRIOS</div>
          <h1 class="hero-title">SisTer Git · Repositórios</h1>
          <p class="hero-desc" style="margin-bottom:0;">Custódia descentralizada de código-fonte, árvores Merkle e branches sob governança do nó LABECO.</p>
        </div>
        <div style="background:rgba(255,255,255,0.08); border:1px solid rgba(255,255,255,0.15); border-radius:12px; padding:8px 10px; display:grid; place-items:center;">
          <svg viewBox="0 0 44 44" width="44" height="44" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" style="color:var(--sky);">
            <path d="M12 7v30"/>
            <path d="M32 14v23"/>
            <path d="M12 14c0 6 20 2 20 9"/>
            <path d="M12 23c0 6 20 2 20 9"/>
            <path d="M12 7c0 5 20 1 20 7"/>
            <circle cx="12" cy="7" r="3.2" fill="#2ea8e8"/>
            <circle cx="12" cy="37" r="3.2" fill="#087f8c"/>
            <circle cx="32" cy="14" r="3.2" fill="#2ea8e8"/>
            <circle cx="32" cy="37" r="3.2" fill="#087f8c"/>
          </svg>
        </div>
      </div>
    </header>

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
            <div style="display:flex; gap:6px; align-items:center; margin-left:8px;">
              <button type="button" class="btn-cancel" onclick="event.stopPropagation(); openCloneModal('${esc(r.name)}', '')" style="padding:4px 9px; font-size:0.75rem; background:#fff; cursor:pointer;" title="Ver comandos para clonar via SSH">📥 Clonar</button>
              <a href="/api/v1/repos/${encodeURIComponent(r.name)}/archive?format=zip" class="btn-cancel" style="padding:4px 9px; font-size:0.75rem; background:#fff; text-decoration:none;" title="Baixar ZIP com o código">⬇ ZIP</a>
              <a href="#/r/${encodeURIComponent(r.name)}/code" class="btn-primary" style="padding:4px 10px; font-size:0.75rem; text-decoration:none;">Abrir ➔</a>
            </div>
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
  const rInfo = allReposCache.find(r => r.name === repo) || {};
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
            <div style="display:flex; gap:8px;">
              <a href="/api/v1/repos/${encodeURIComponent(repo)}/raw?ref=${encodeURIComponent(ref || 'HEAD')}&path=${encodeURIComponent(path)}" onclick="event.preventDefault(); downloadFile(this.href, '${esc(path.split('/').pop())}')" download="${esc(path.split('/').pop())}" class="btn-primary" style="padding:4px 10px; font-size:0.75rem; text-decoration:none;">⬇ Baixar Raw</a>
              <a href="#/r/${encodeURIComponent(repo)}/code?ref=${encodeURIComponent(ref || '')}" class="btn-cancel" style="padding:4px 8px; font-size:0.75rem;">Voltar à pasta</a>
            </div>
          </div>
          ${blob.binary ? `<div style="padding:40px; text-align:center; color:var(--muted);">Arquivo binário não editável diretamente</div>` : `
            <pre class="blob-code"><code>${esc(blob.content)}</code></pre>
          `}
        </div>
      `;
    } else {
      // Directory tree
      const entries = treeData?.entries || [];

      // Check for README in root directory
      let readmeHtml = '';
      if (!path && entries.length) {
        const readmeEntry = entries.find(e => /^readme(\.md)?$/i.test(e.name));
        if (readmeEntry) {
          try {
            const readmeBlob = await api(`/api/v1/repos/${encodeURIComponent(repo)}/blob?ref=${encodeURIComponent(ref || 'HEAD')}&path=${encodeURIComponent(readmeEntry.name)}`);
            if (readmeBlob && !readmeBlob.binary) {
              readmeHtml = `
                <div class="readme-card" style="margin-top:24px; background:var(--surface); border:1px solid var(--line); border-radius:var(--radius-lg); overflow:hidden; box-shadow:var(--shadow-sm);">
                  <div class="readme-card-head" style="padding:12px 20px; background:var(--surface-alt); border-bottom:1px solid var(--line); display:flex; align-items:center; justify-content:space-between; font-size:0.84rem; font-weight:700; color:var(--navy);">
                    <div style="display:flex; align-items:center; gap:8px;">
                      <span style="font-size:1.1rem;">📖</span>
                      <span>${esc(readmeEntry.name)}</span>
                    </div>
                    <span style="font-size:0.75rem; color:var(--muted); font-weight:normal;">${fmt(readmeBlob.size_bytes)}</span>
                  </div>
                  <div class="readme-card-body markdown-body" style="padding:28px 32px; line-height:1.65; background:#fff;">
                    ${renderMarkdown(readmeBlob.content)}
                  </div>
                </div>
              `;
            }
          } catch (_) {}
        }
      }

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
                <td colspan="3"><a href="#/r/${encodeURIComponent(repo)}/code?ref=${encodeURIComponent(ref)}&path=${encodeURIComponent(path.split('/').slice(0,-1).join('/'))}">📁 .. (diretório anterior)</a></td>
              </tr>
            ` : ''}
            ${entries.length === 0 ? `
              <tr>
                <td colspan="3" style="text-align:center; padding:32px; color:var(--muted);">
                  Repositório vazio ou nenhum arquivo encontrado nesta pasta.
                </td>
              </tr>
            ` : entries.map(e => `
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
        ${readmeHtml}
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
    const isEd = rInfo.type === 'editorial';
    innerContent = await buildCompareViewHtml({
      repo,
      base,
      head,
      viewMode: route.view,
      isEditorial: isEd,
      mainDoc: ''
    });
  }

  app.innerHTML = `
    <header class="editorial-doc-header" style="padding:20px 24px;">
      <div style="display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px;">
        <div>
          <div style="display:flex; align-items:center; gap:10px;">
            <h1 style="font-size:1.4rem; color:var(--navy); margin:0;">${esc(repo)}</h1>
            ${visBadge(rInfo.visibility || 'public', repo, rInfo.owner || '')}
            ${rInfo.type === 'editorial' ? '<span class="badge-edition-tag">Editorial</span>' : ''}
          </div>
          <p style="margin:4px 0 0; color:var(--muted); font-size:0.8rem;">Repositório bare sob governança e custódia do nó LABECO</p>
        </div>
        <div style="display:flex; gap:8px; align-items:center; flex-wrap:wrap;">
          <button type="button" class="btn-cancel" onclick="openCloneModal('${esc(repo)}', '${esc(ref || '')}')" style="display:flex; align-items:center; gap:6px; font-weight:600; font-size:0.82rem; padding:7px 14px; background:#fff; cursor:pointer;" title="Ver comandos para clonar via SSH">
            <span>📥</span> Clonar
          </button>
          <a href="/api/v1/repos/${encodeURIComponent(repo)}/archive?ref=${encodeURIComponent(ref || 'HEAD')}&format=zip" class="btn-cancel" style="display:flex; align-items:center; gap:6px; font-weight:600; font-size:0.82rem; padding:7px 14px; background:#fff; text-decoration:none;" title="Baixar arquivo ZIP com o código">
            <span>⬇</span> Baixar ZIP
          </a>
          ${rInfo.type === 'editorial' ? `
            <a href="#/editorial/${encodeURIComponent(repo)}" class="btn-primary" style="font-size:0.82rem; padding:7px 14px;">
              Ir para Espaço Editorial ➔
            </a>
          ` : ''}
        </div>
      </div>

      <div style="display:flex; justify-content:space-between; align-items:center; margin-top:16px; padding-top:14px; border-top:1px solid var(--line); flex-wrap:wrap; gap:10px;">
        <div style="display:flex; align-items:center; gap:10px;">
          <label style="display:flex; align-items:center; gap:6px; font-size:0.82rem; font-weight:600; color:var(--navy); margin:0;">
            <span>🌿 Branch:</span>
            <select onchange="window.location.hash='#/r/${encodeURIComponent(repo)}/${tab}?ref=' + encodeURIComponent(this.value)" style="padding:4px 8px; font-size:0.8rem; border-radius:var(--radius-sm); border:1px solid var(--line); background:#fff; font-weight:600;">
              ${(refsData.branches || []).map(b => `<option value="${esc(b.name)}" ${b.name === (ref || 'master') || (!ref && b.name === (rInfo.default_branch || 'master')) ? 'selected' : ''}>${esc(b.name)}</option>`).join('')}
              ${(refsData.tags || []).length ? `<optgroup label="Tags">${(refsData.tags || []).map(t => `<option value="${esc(t.name)}" ${t.name === ref ? 'selected' : ''}>🏷️ ${esc(t.name)}</option>`).join('')}</optgroup>` : ''}
            </select>
          </label>
          ${path ? `
            <span style="font-size:0.82rem; color:var(--muted);">/</span>
            <span style="font-size:0.82rem; font-family:var(--font-mono); color:var(--navy); font-weight:600;">${esc(path)}</span>
          ` : ''}
        </div>

        <div style="display:flex; align-items:center; gap:14px; font-size:0.78rem; color:var(--muted);">
          <a href="#/r/${encodeURIComponent(repo)}/commits?ref=${encodeURIComponent(ref || '')}" style="color:var(--muted); text-decoration:none; display:flex; align-items:center; gap:4px;">
            <strong>⎇</strong> <span>${rInfo.branches || (refsData.branches || []).length} branches</span>
          </a>
          <span>🏷️ ${(refsData.tags || []).length} tags</span>
          ${rInfo.last_commit ? `
            <span title="${esc(rInfo.last_commit.subject)}">Último commit: <code style="font-size:0.75rem;">${esc(rInfo.last_commit.short_hash)}</code> (${timeAgo(rInfo.last_commit.date)})</span>
          ` : ''}
        </div>
      </div>
    </header>

    <nav class="doc-tabs-bar">
      <a href="#/r/${encodeURIComponent(repo)}/code${ref ? `?ref=${encodeURIComponent(ref)}` : ''}" class="doc-tab ${tab === 'code' ? 'active' : ''}">📄 Código</a>
      <a href="#/r/${encodeURIComponent(repo)}/commits${ref ? `?ref=${encodeURIComponent(ref)}` : ''}" class="doc-tab ${tab === 'commits' ? 'active' : ''}">⎇ Commits</a>
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
  const visibility = document.querySelector('#newProjVisibility');
  if (visibility) visibility.value = type === 'editorial' ? 'private' : 'public';
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
  document.querySelector('#reviewReviewer').value = currentUser?.name || currentUser?.email || '';
  openModal('modalNewReview');
};

window.assignProjectReviewer = async function(repo) {
  const email = prompt('E-mail institucional do revisor:');
  if (!email) return;
  try {
    await apiPost(`/api/v1/repos/${encodeURIComponent(repo)}/editorial/assign-member`, { email: email.trim(), role: 'reviewer' });
    showToast('✓ Revisor designado para este projeto.');
  } catch (err) {
    alert('Erro ao designar revisor: ' + err.message);
  }
};

window.respondToReview = async function(repo, reviewId) {
  const response = prompt('Resposta formal do autor ao parecer:');
  if (!response) return;
  try {
    await apiPost(`/api/v1/repos/${encodeURIComponent(repo)}/editorial/respond`, { review_id: reviewId, response: response.trim() });
    showToast('✓ Resposta do autor registrada no Git.');
    await renderEditorialDoc(repo, 'reviews');
  } catch (err) {
    alert('Erro ao responder parecer: ' + err.message);
  }
};

window.handleCreateReview = async function(e) {
  e.preventDefault();
  const repo = document.querySelector('#reviewRepoName').value;
  const reviewer = document.querySelector('#reviewReviewer').value.trim();
  const rec = document.querySelector('#reviewRecommendation').value;
  const summary = document.querySelector('#reviewSummary').value.trim();
  const btn = document.querySelector('#btnSubmitReview');

  btn.disabled = true;
  btn.textContent = 'Registrando...';

  try {
    await apiPost(`/api/v1/repos/${encodeURIComponent(repo)}/editorial/review`, {
      reviewer,
      recommendation: rec,
      summary
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

window.switchAuthTab = function(tab) {
  const isEmail = tab === 'email';
  document.querySelector('#tabBtnEmail')?.classList.toggle('active', isEmail);
  document.querySelector('#tabBtnToken')?.classList.toggle('active', !isEmail);
  document.querySelector('#panelAuthEmail')?.classList.toggle('hidden', !isEmail);
  document.querySelector('#panelAuthToken')?.classList.toggle('hidden', isEmail);
  document.querySelector('#panelAuthConnected')?.classList.add('hidden');
};

window.switchAuthStep = function(step) {
  const isStep1 = step === 1;
  document.querySelector('#formAuthStep1')?.classList.toggle('hidden', !isStep1);
  document.querySelector('#formAuthStep2')?.classList.toggle('hidden', isStep1);
  const fb1 = document.querySelector('#authEmailFeedback');
  const fb2 = document.querySelector('#authCodeFeedback');
  if (fb1) fb1.classList.add('hidden');
  if (fb2) fb2.classList.add('hidden');
  if (!isStep1) {
    setTimeout(() => document.querySelector('#authCodeInput')?.focus(), 120);
  } else {
    setTimeout(() => document.querySelector('#authEmailInput')?.focus(), 120);
  }
};

window.openAuthModal = function() {
  if (currentUser) {
    document.querySelector('#authTabsNav')?.classList.add('hidden');
    document.querySelector('#panelAuthEmail')?.classList.add('hidden');
    document.querySelector('#panelAuthToken')?.classList.add('hidden');
    const pConn = document.querySelector('#panelAuthConnected');
    if (pConn) {
      pConn.classList.remove('hidden');
      const name = currentUser.name || currentUser.email.split('@')[0];
      const initials = (name || 'P')
        .split(' ')
        .map(w => w[0])
        .filter(Boolean)
        .slice(0, 2)
        .join('')
        .toUpperCase();
      document.querySelector('#authConnectedAvatar').textContent = initials;
      document.querySelector('#authConnectedName').textContent = name;
      document.querySelector('#authConnectedEmail').textContent = currentUser.email;
      document.querySelector('#authConnectedRole').textContent = currentUser.role === 'admin' ? 'Administrador' : 'Pesquisador Embrapa';

      const adminBox = document.querySelector('#adminUserManagementBox');
      if (adminBox) {
        adminBox.classList.toggle('hidden', currentUser.role !== 'admin');
      }
    }
  } else {
    document.querySelector('#authTabsNav')?.classList.remove('hidden');
    document.querySelector('#panelAuthConnected')?.classList.add('hidden');
    switchAuthTab('email');
    switchAuthStep(1);
    const lastEmail = localStorage.getItem('sister_last_email') || '';
    const emailInput = document.querySelector('#authEmailInput');
    if (emailInput && !emailInput.value) emailInput.value = lastEmail;
    const tokenInput = document.querySelector('#authTokenInput');
    if (tokenInput) tokenInput.value = getToken();
  }
  openModal('modalAuth');
};

window.openUsersModal = async function() {
  closeModal('modalAuth');
  openModal('modalUsers');
  await reloadAllowedUsers();
};

async function reloadAllowedUsers() {
  const container = document.querySelector('#allowedUsersList');
  if (!container) return;
  container.innerHTML = '<div style="padding: 16px; text-align: center; color: var(--muted);">Carregando pesquisadores autorizados...</div>';
  try {
    const res = await api('/api/v1/admin/users');
    const users = res.users || [];
    if (!users.length) {
      container.innerHTML = '<div style="padding: 16px; text-align: center; color: var(--muted);">Nenhum usuário cadastrado na lista restrita. (Modo aberto ativo).</div>';
      return;
    }
    container.innerHTML = users.map(u => `
      <div class="user-row">
        <div>
          <strong style="color: var(--navy); display: block;">${esc(u.name || u.email)}</strong>
          <span style="font-size: 0.75rem; color: var(--muted);">${esc(u.email)}</span>
        </div>
        <div style="display: flex; align-items: center; gap: 8px;">
          <span class="user-role-tag ${u.role === 'admin' ? 'role-admin' : 'role-researcher'}">${u.role === 'admin' ? 'Administrador' : 'Pesquisador'}</span>
          ${currentUser && currentUser.email !== u.email ? `
            <button type="button" class="btn-cancel" style="padding: 3px 8px; font-size: 0.72rem; color: #cf222e; border-color: #fca5a5; cursor:pointer;" onclick="handleRemoveAllowedUser('${esc(u.email)}')">Remover</button>
          ` : '<span style="font-size: 0.72rem; color: var(--muted);">(Você)</span>'}
        </div>
      </div>
    `).join('');
  } catch (err) {
    container.innerHTML = `<div style="padding: 16px; color: #cf222e;">Erro: ${esc(err.message)}</div>`;
  }
}

window.handleAddAllowedUser = async function(e) {
  e.preventDefault();
  const email = document.querySelector('#newUserEmail').value.trim().toLowerCase();
  const name = document.querySelector('#newUserName').value.trim();
  const role = document.querySelector('#newUserRole').value;
  const fb = document.querySelector('#addUserFeedback');
  if (fb) fb.classList.add('hidden');

  try {
    await apiPost('/api/v1/admin/users', { email, name, role });
    showToast(`✓ Pesquisador '${name}' autorizado com sucesso!`);
    document.querySelector('#newUserEmail').value = '';
    document.querySelector('#newUserName').value = '';
    await reloadAllowedUsers();
  } catch (err) {
    if (fb) {
      fb.textContent = err.message;
      fb.classList.remove('hidden');
    }
  }
};

window.handleRemoveAllowedUser = async function(email) {
  if (!confirm(`Tem certeza que deseja remover a autorização do pesquisador '${email}'?`)) return;
  try {
    await apiPost('/api/v1/admin/users/delete', { email });
    showToast(`Pesquisador '${email}' removido da lista.`);
    await reloadAllowedUsers();
  } catch (err) {
    alert('Erro ao remover usuário: ' + err.message);
  }
};

window.handleRequestOtp = async function(e) {
  e.preventDefault();
  const emailInput = document.querySelector('#authEmailInput');
  const email = emailInput?.value.trim().toLowerCase();
  const fb = document.querySelector('#authEmailFeedback');
  const btn = document.querySelector('#btnRequestOtp');
  if (!email) return;

  if (fb) fb.classList.add('hidden');
  btn.disabled = true;
  btn.textContent = 'Enviando código... ⏳';

  try {
    const res = await apiPost('/api/v1/auth/request-code', { email });
    currentOtpEmail = email;
    localStorage.setItem('sister_last_email', email);
    document.querySelector('#authTargetEmailDisplay').textContent = email;
    const codeInput = document.querySelector('#authCodeInput');
    if (codeInput) codeInput.value = '';
    switchAuthStep(2);
    showToast('✉ Código de 6 dígitos enviado para seu e-mail!');
    if (res.dev_code && codeInput) {
      codeInput.value = res.dev_code;
    }
  } catch (err) {
    if (fb) {
      fb.textContent = err.message || 'Erro ao enviar código por e-mail.';
      fb.classList.remove('hidden');
    }
  } finally {
    btn.disabled = false;
    btn.textContent = 'Enviar Código por E-mail ✉';
  }
};

window.handleVerifyOtp = async function(e) {
  e.preventDefault();
  const codeInput = document.querySelector('#authCodeInput');
  const code = codeInput?.value.trim();
  const fb = document.querySelector('#authCodeFeedback');
  const btn = document.querySelector('#btnVerifyOtp');
  if (!code || !currentOtpEmail) return;

  if (fb) fb.classList.add('hidden');
  btn.disabled = true;
  btn.textContent = 'Autenticando... ⏳';

  try {
    const res = await apiPost('/api/v1/auth/verify-code', { email: currentOtpEmail, code });
    if (res.token) {
      setToken(res.token);
      closeModal('modalAuth');
      showToast(`✓ Bem-vindo, ${res.user?.name || res.user?.email || 'Pesquisador'}!`);
      await updateAuthStatus();
      const route = parseRoute();
      if (!(route.mode === 'editorial-doc' && route.tab === 'editor')) {
        render();
      }
    } else {
      throw Error('Token de sessão não recebido do servidor.');
    }
  } catch (err) {
    if (fb) {
      fb.textContent = err.message || 'Código incorreto ou expirado.';
      fb.classList.remove('hidden');
    }
  } finally {
    btn.disabled = false;
    btn.textContent = 'Confirmar e Entrar ✓';
  }
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
    showToast('✓ Token autenticado com sucesso!');
    await updateAuthStatus();
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
  currentUser = null;
  closeModal('modalAuth');
  showToast('Token removido.');
  updateAuthStatus();
  render();
};

window.openCloneModal = function(repo, ref) {
  const host = window.location.hostname || '10.163.80.129';
  const sshClone = `git clone jpereiratrindade@${host}:/srv/git/${repo}.git`;
  const gitRemote = `git remote add labeco jpereiratrindade@${host}:/srv/git/${repo}.git\ngit push -u labeco master`;

  const sshInput = document.querySelector('#cloneSshInput');
  const remoteInput = document.querySelector('#cloneRemoteInput');
  const zipLink = document.querySelector('#cloneDownloadZip');
  const tarLink = document.querySelector('#cloneDownloadTar');
  const title = document.querySelector('#modalCloneTitle');

  if (title) title.textContent = `Clonar ou Baixar: ${repo}`;
  if (sshInput) sshInput.value = sshClone;
  if (remoteInput) remoteInput.value = gitRemote;
  const zipUrl = `/api/v1/repos/${encodeURIComponent(repo)}/archive?ref=${encodeURIComponent(ref || 'HEAD')}&format=zip`;
  const tarUrl = `/api/v1/repos/${encodeURIComponent(repo)}/archive?ref=${encodeURIComponent(ref || 'HEAD')}&format=tar.gz`;
  const zipName = `${repo}-${(ref || 'HEAD').replace(/\//g, '-')}.zip`;
  const tarName = `${repo}-${(ref || 'HEAD').replace(/\//g, '-')}.tar.gz`;

  if (zipLink) {
    zipLink.href = zipUrl;
    zipLink.setAttribute('download', zipName);
    zipLink.onclick = (e) => {
      e.preventDefault();
      downloadFile(zipUrl, zipName);
    };
  }
  if (tarLink) {
    tarLink.href = tarUrl;
    tarLink.setAttribute('download', tarName);
    tarLink.onclick = (e) => {
      e.preventDefault();
      downloadFile(tarUrl, tarName);
    };
  }

  openModal('modalClone');
};

window.copyCloneCommand = async function(inputId) {
  const input = document.querySelector('#' + inputId);
  if (!input) return;
  input.select();
  try {
    await navigator.clipboard.writeText(input.value);
    showToast('✓ Comando copiado para a área de transferência!');
  } catch (_) {
    document.execCommand('copy');
    showToast('✓ Comando copiado!');
  }
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
  const t = getToken();
  if (t && !document.cookie.includes('sister_token=')) {
    document.cookie = `sister_token=${encodeURIComponent(t)}; path=/; SameSite=Lax; max-age=2592000`;
  }
  updateAuthStatus();
  render();
});
