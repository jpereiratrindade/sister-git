const app = document.querySelector('#app');

const api = async path => {
  const r = await fetch(path);
  if (!r.ok) {
    let e = await r.json().catch(() => ({ error: r.statusText }));
    throw Error(e.error || 'Erro na requisição');
  }
  return r.json();
};

const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
}[c]));

const fmt = b => {
  if (b === null || b === undefined) return '0 B';
  if (b < 1024) return `${b} B`;
  if (b < 1048576) return `${(b / 1024).toFixed(1)} KiB`;
  return `${(b / 1048576).toFixed(1)} MiB`;
};

const date = s => {
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
    return date(s);
  } catch (_) {
    return date(s);
  }
};

// Parse location hash: #/repo/tab?ref=foo&path=bar
function parseHash() {
  const raw = location.hash.replace(/^#\/?/, '');
  const [routePart, queryPart] = raw.split('?');
  const segments = (routePart || '').split('/').map(decodeURIComponent).filter(Boolean);
  const q = new URLSearchParams(queryPart || '');
  return {
    repo: segments[0] || '',
    tab: segments[1] || 'code',
    legacyRest: segments.slice(2).join('/'),
    ref: q.get('ref') || '',
    path: q.get('path') || segments.slice(2).join('/'),
    base: q.get('base') || '',
    head: q.get('head') || ''
  };
}

function link(repo, tab = 'code', extra = {}) {
  let u = `#/${encodeURIComponent(repo)}/${tab}`;
  const q = new URLSearchParams();
  if (extra.ref) q.set('ref', extra.ref);
  if (extra.path) q.set('path', extra.path);
  if (extra.base) q.set('base', extra.base);
  if (extra.head) q.set('head', extra.head);
  const qs = q.toString();
  return qs ? `${u}?${qs}` : u;
}

window.copyText = function(text, btn) {
  navigator.clipboard.writeText(text).then(() => {
    const orig = btn.innerHTML;
    btn.innerHTML = '✓ Copiado!';
    btn.classList.add('copied');
    setTimeout(() => {
      btn.innerHTML = orig;
      btn.classList.remove('copied');
    }, 1600);
  }).catch(() => {
    alert('Não foi possível copiar automaticamente.');
  });
};

function frame(repo, tab, content, ref = '', branches = [], tags = []) {
  const branchCount = branches.length;
  const tagCount = tags.length;

  return `
    <header class="repo-header">
      <div class="repo-meta-bar">
        <div class="repo-breadcrumb">
          <svg class="octicon" viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M2 2.5A2.5 2.5 0 0 1 4.5 0h8.75a.75.75 0 0 1 .75.75v12.5a.75.75 0 0 1-.75.75h-2.5a.75.75 0 0 1 0-1.5h1.75v-2h-8a1 1 0 0 0-.714 1.7.75.75 0 1 1-1.072 1.05A2.495 2.495 0 0 1 2 11.5Zm10.5-1h-8a1 1 0 0 0-1 1v6.708A2.486 2.486 0 0 1 4.5 9h8ZM5 12.25a.25.25 0 0 1 .25-.25h6.5a.25.25 0 0 1 .25.25v1.5a.25.25 0 0 1-.25.25h-6.5a.25.25 0 0 1-.25-.25Z"></path></svg>
          <a href="#/">repositórios</a>
          <span class="sep">/</span>
          <a class="repo-name" href="${link(repo, 'code')}"><b>${esc(repo)}</b></a>
          <span class="badge badge-public">Laboratório (Local)</span>
        </div>
      </div>
      <nav class="repo-tabs">
        <a class="tab-item ${tab === 'code' ? 'active' : ''}" href="${link(repo, 'code', { ref })}">
          <svg class="octicon" viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="m11.28 3.22 4.25 4.25a.75.75 0 0 1 0 1.06l-4.25 4.25a.749.749 0 0 1-1.275-.326.749.749 0 0 1 .215-.734L13.94 8l-3.72-3.72a.749.749 0 0 1 .326-1.275.749.749 0 0 1 .734.215Zm-6.56 0a.751.751 0 0 1 1.06 0 .75.75 0 0 1 0 1.06L2.06 8l3.72 3.72a.749.749 0 0 1-.326 1.275.749.749 0 0 1-.734-.215L.47 8.53a.75.75 0 0 1 0-1.06l4.25-4.25a.75.75 0 0 1 1.06 0Z"></path></svg>
          <span>Código</span>
        </a>
        <a class="tab-item ${tab === 'commits' ? 'active' : ''}" href="${link(repo, 'commits', { ref })}">
          <svg class="octicon" viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M11.93 8.5a4.002 4.002 0 0 1-7.86 0H.75a.75.75 0 0 1 0-1.5h3.32a4.002 4.002 0 0 1 7.86 0h3.32a.75.75 0 0 1 0 1.5Zm-1.43-.75a2.5 2.5 0 1 0-5 0 2.5 2.5 0 0 0 5 0Z"></path></svg>
          <span>Commits</span>
        </a>
        <a class="tab-item ${tab === 'refs' ? 'active' : ''}" href="${link(repo, 'refs')}">
          <svg class="octicon" viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M9.5 3.25a2.25 2.25 0 1 1 3 2.122V6A2.5 2.5 0 0 1 10 8.5H6a1 1 0 0 0-1 1v1.128a2.251 2.251 0 1 1-1.5 0V5.372a2.25 2.25 0 1 1 1.5 0v1.836A2.493 2.493 0 0 1 6 7h4a1 1 0 0 0 1-1v-.628A2.25 2.25 0 0 1 9.5 3.25Z"></path></svg>
          <span>Branches & tags</span>
          <span class="count-pill">${branchCount + tagCount}</span>
        </a>
        <a class="tab-item ${tab === 'compare' ? 'active' : ''}" href="${link(repo, 'compare')}">
          <svg class="octicon" viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M6.5 1.75a.75.75 0 0 0-1.5 0V3H3.75A1.75 1.75 0 0 0 2 4.75v6.5C2 12.216 2.784 13 3.75 13H5v1.25a.75.75 0 0 0 1.5 0V13h3v1.25a.75.75 0 0 0 1.5 0V13h1.25c.966 0 1.75-.784 1.75-1.75v-6.5A1.75 1.75 0 0 0 12.25 3H11V1.75a.75.75 0 0 0-1.5 0V3h-3ZM3.5 4.75a.25.25 0 0 1 .25-.25h8.5a.25.25 0 0 1 .25.25v6.5a.25.25 0 0 1-.25.25h-8.5a.25.25 0 0 1-.25-.25Z"></path></svg>
          <span>Comparar</span>
        </a>
      </nav>
    </header>
    ${content}
  `;
}

// Markdown parser
function renderMarkdown(src) {
  if (!src) return '<p class="empty-md">Documento vazio.</p>';

  const lines = src.split('\n');
  let html = [];
  let inCode = false;
  let codeLang = '';
  let codeBuf = [];
  let inList = false;
  let inTable = false;
  let tableHeader = true;

  const flushList = () => { if (inList) { html.push('</ul>'); inList = false; } };
  const flushTable = () => { if (inTable) { html.push('</tbody></table></div>'); inTable = false; tableHeader = true; } };

  const inlineFormat = t => {
    return esc(t)
      .replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
      .replace(/\*([^*]+)\*/g, '<em>$1</em>')
      .replace(/`([^`]+)`/g, '<code class="inline-code">$1</code>')
      .replace(/\[([^\]]+)\]\(([^)]+)\)/g, (m, txt, url) => {
        const clean = url.trim();
        if (/^(https?:\/\/|\/|#)/i.test(clean)) {
          return `<a href="${esc(clean)}" target="_blank" rel="noopener">${txt}</a>`;
        }
        return txt;
      });
  };

  for (let i = 0; i < lines.length; i++) {
    const rawLine = lines[i];
    const line = rawLine.trim();

    // Code block
    if (line.startsWith('```')) {
      if (inCode) {
        html.push(`<pre class="md-codeblock"><code>${esc(codeBuf.join('\n'))}</code></pre>`);
        codeBuf = [];
        inCode = false;
      } else {
        flushList();
        flushTable();
        inCode = true;
        codeLang = line.slice(3).trim();
      }
      continue;
    }

    if (inCode) {
      codeBuf.push(rawLine);
      continue;
    }

    // Horizontal rule
    if (/^(\*\*\*|---|___)$/.test(line)) {
      flushList();
      flushTable();
      html.push('<hr class="md-hr">');
      continue;
    }

    // Table
    if (line.startsWith('|') && line.endsWith('|')) {
      flushList();
      if (/^\|[\s\-:|]+\|$/.test(line)) {
        // Separator line
        tableHeader = false;
        continue;
      }
      const cells = line.slice(1, -1).split('|').map(c => c.trim());
      if (!inTable) {
        inTable = true;
        tableHeader = true;
        html.push('<div class="md-table-wrap"><table class="md-table">');
        html.push('<thead><tr>' + cells.map(c => `<th>${inlineFormat(c)}</th>`).join('') + '</tr></thead><tbody>');
      } else {
        html.push('<tr>' + cells.map(c => `<td>${inlineFormat(c)}</td>`).join('') + '</tr>');
      }
      continue;
    } else {
      flushTable();
    }

    // Headings
    if (line.startsWith('#')) {
      flushList();
      const m = line.match(/^(#{1,6})\s+(.*)$/);
      if (m) {
        const lvl = m[1].length;
        const text = m[2];
        html.push(`<h${lvl} class="md-h${lvl}">${inlineFormat(text)}</h${lvl}>`);
        continue;
      }
    }

    // Blockquote & Alerts
    if (line.startsWith('>')) {
      flushList();
      let bq = line.replace(/^>\s?/, '');
      let alertClass = 'md-quote';
      let alertPrefix = '';
      if (bq.startsWith('[!NOTE]')) {
        alertClass = 'md-alert md-alert-note';
        alertPrefix = '<strong>Nota:</strong> ';
        bq = bq.slice(7).trim();
      } else if (bq.startsWith('[!WARNING]') || bq.startsWith('[!CAUTION]')) {
        alertClass = 'md-alert md-alert-warn';
        alertPrefix = '<strong>Atenção:</strong> ';
        bq = bq.slice(bq.indexOf(']') + 1).trim();
      } else if (bq.startsWith('[!TIP]')) {
        alertClass = 'md-alert md-alert-tip';
        alertPrefix = '<strong>Dica:</strong> ';
        bq = bq.slice(6).trim();
      }
      html.push(`<div class="${alertClass}">${alertPrefix}${inlineFormat(bq)}</div>`);
      continue;
    }

    // List item
    if (/^[-*]\s+/.test(line)) {
      if (!inList) {
        html.push('<ul class="md-list">');
        inList = true;
      }
      html.push(`<li>${inlineFormat(line.replace(/^[-*]\s+/, ''))}</li>`);
      continue;
    } else {
      flushList();
    }

    // Empty line
    if (!line) {
      continue;
    }

    // Paragraph
    html.push(`<p class="md-p">${inlineFormat(line)}</p>`);
  }

  flushList();
  flushTable();
  return html.join('\n');
}

// Diff Parser & Renderer
function renderDiff(diffText) {
  if (!diffText || !diffText.trim()) {
    return '<div class="notice">Sem diferenças entre as revisões especificadas.</div>';
  }

  const lines = diffText.split('\n');
  const files = [];
  let currentFile = null;

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (line.startsWith('diff --git ')) {
      const parts = line.split(' ');
      const pathB = parts[3] ? parts[3].replace(/^b\//, '') : parts[2];
      currentFile = { name: pathB, adds: 0, dels: 0, lines: [] };
      files.push(currentFile);
      continue;
    }

    if (!currentFile) {
      // Preface / stat lines
      continue;
    }

    if (line.startsWith('+++') || line.startsWith('---') || line.startsWith('index ')) {
      currentFile.lines.push({ type: 'meta', text: line });
    } else if (line.startsWith('+')) {
      currentFile.adds++;
      currentFile.lines.push({ type: 'add', text: line });
    } else if (line.startsWith('-')) {
      currentFile.dels++;
      currentFile.lines.push({ type: 'del', text: line });
    } else if (line.startsWith('@@')) {
      currentFile.lines.push({ type: 'hunk', text: line });
    } else {
      currentFile.lines.push({ type: 'ctx', text: line });
    }
  }

  if (files.length === 0) {
    return `<div class="codebox"><pre>${esc(diffText)}</pre></div>`;
  }

  let totalAdds = files.reduce((acc, f) => acc + f.adds, 0);
  let totalDels = files.reduce((acc, f) => acc + f.dels, 0);

  let out = `
    <div class="diff-summary-card">
      <div class="diff-summary-title">
        <b>${files.length}</b> arquivo(s) modificado(s) com 
        <span class="diff-stat-add">+${totalAdds}</span> adições e 
        <span class="diff-stat-del">-${totalDels}</span> exclusões
      </div>
      <div class="diff-file-toc">
        ${files.map(f => `
          <div class="toc-item">
            <span class="toc-name">${esc(f.name)}</span>
            <span class="toc-stat"><span class="diff-stat-add">+${f.adds}</span> <span class="diff-stat-del">-${f.dels}</span></span>
          </div>
        `).join('')}
      </div>
    </div>
  `;

  for (const f of files) {
    out += `
      <div class="diff-file-box">
        <div class="diff-file-header">
          <div class="diff-file-title">
            <svg class="octicon" viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M2 1.75C2 .784 2.784 0 3.75 0h6.586c.464 0 .909.184 1.237.513l2.914 2.914c.329.328.513.773.513 1.237v9.586A1.75 1.75 0 0 1 13.25 16h-9.5A1.75 1.75 0 0 1 2 14.25Zm1.75-.25a.25.25 0 0 0-.25.25v12.5c0 .138.112.25.25.25h9.5a.25.25 0 0 0 .25-.25V6h-2.75A1.75 1.75 0 0 1 9 4.25V1.5Zm6.75.793V4.25c0 .138.112.25.25.25h2.707Z"></path></svg>
            <span>${esc(f.name)}</span>
          </div>
          <div class="diff-file-actions">
            <span class="diff-stat-add">+${f.adds}</span>
            <span class="diff-stat-del">-${f.dels}</span>
          </div>
        </div>
        <div class="diff-table">
          ${f.lines.map(l => {
            const cls = l.type === 'add' ? 'diff-line-add' : l.type === 'del' ? 'diff-line-del' : l.type === 'hunk' ? 'diff-line-hunk' : 'diff-line-ctx';
            return `<div class="diff-row ${cls}"><span class="diff-marker">${esc(l.text.charAt(0))}</span><span class="diff-content">${esc(l.text.slice(1) || ' ')}</span></div>`;
          }).join('')}
        </div>
      </div>
    `;
  }

  return out;
}

// Branch Selector Component
function renderBranchSelector(repo, currentRef, branches, currentPath, tab = 'code') {
  return `
    <div class="branch-select-wrap">
      <span class="branch-select-label">
        <svg class="octicon" viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M9.5 3.25a2.25 2.25 0 1 1 3 2.122V6A2.5 2.5 0 0 1 10 8.5H6a1 1 0 0 0-1 1v1.128a2.251 2.251 0 1 1-1.5 0V5.372a2.25 2.25 0 1 1 1.5 0v1.836A2.493 2.493 0 0 1 6 7h4a1 1 0 0 0 1-1v-.628A2.25 2.25 0 0 1 9.5 3.25Z"></path></svg>
        <select class="branch-select" onchange="location.href='${link(repo, tab, { path: currentPath })}&ref=' + encodeURIComponent(this.value)">
          ${branches.map(b => `<option value="${esc(b.name)}" ${b.name === currentRef ? 'selected' : ''}>${esc(b.name)}</option>`).join('')}
        </select>
      </span>
    </div>
  `;
}

// Views
async function home() {
  const d = await api('/api/v1/repos');
  app.innerHTML = `
    <section class="hero-github">
      <div class="hero-content">
        <div class="eyebrow">Laboratório de Ecologia e Conservação</div>
        <h1>SisTer Git</h1>
        <p class="hero-desc">Hospedagem segura e navegação visual de repositórios Git, executando diretamente sobre o cluster local sem dependências externas.</p>
      </div>
      <div class="hero-badges">
        <span class="status-pill status-active">● Sistema Ativo</span>
        <span class="meta-tag">${d.repositories.length} repositórios</span>
      </div>
    </section>

    <div class="repo-filter-bar">
      <h2>Repositórios do Laboratório</h2>
    </div>

    <div class="repo-grid">
      ${d.repositories.map(r => `
        <article class="repo-card">
          <div class="repo-card-top">
            <div class="repo-card-title">
              <svg class="octicon repo-icon" viewBox="0 0 16 16" width="18" height="18"><path fill="currentColor" d="M2 2.5A2.5 2.5 0 0 1 4.5 0h8.75a.75.75 0 0 1 .75.75v12.5a.75.75 0 0 1-.75.75h-2.5a.75.75 0 0 1 0-1.5h1.75v-2h-8a1 1 0 0 0-.714 1.7.75.75 0 1 1-1.072 1.05A2.495 2.495 0 0 1 2 11.5Zm10.5-1h-8a1 1 0 0 0-1 1v6.708A2.486 2.486 0 0 1 4.5 9h8ZM5 12.25a.25.25 0 0 1 .25-.25h6.5a.25.25 0 0 1 .25.25v1.5a.25.25 0 0 1-.25.25h-6.5a.25.25 0 0 1-.25-.25Z"></path></svg>
              <a class="repo-link" href="${link(r.name)}">${esc(r.name)}</a>
              <span class="badge badge-branch">${esc(r.default_branch || 'main')}</span>
            </div>
          </div>
          <div class="repo-card-body">
            ${r.empty ? '<span class="empty-repo">Repositório vazio (sem commits)</span>' : `
              <div class="last-commit-msg">${esc(r.last_commit.subject)}</div>
              <div class="last-commit-meta">
                <span class="mono-hash">${esc(r.last_commit.short_hash)}</span> por <strong>${esc(r.last_commit.author)}</strong> · ${timeAgo(r.last_commit.date)}
              </div>
            `}
          </div>
          <div class="repo-card-footer">
            <span class="stat-item"><svg class="octicon" viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" d="M9.5 3.25a2.25 2.25 0 1 1 3 2.122V6A2.5 2.5 0 0 1 10 8.5H6a1 1 0 0 0-1 1v1.128a2.251 2.251 0 1 1-1.5 0V5.372a2.25 2.25 0 1 1 1.5 0v1.836A2.493 2.493 0 0 1 6 7h4a1 1 0 0 0 1-1v-.628A2.25 2.25 0 0 1 9.5 3.25Z"></path></svg> <b>${r.branches}</b> branches</span>
            <span class="stat-item"><svg class="octicon" viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" d="M1 7.775V2.75C1 1.784 1.784 1 2.75 1h5.025c.464 0 .91.184 1.238.513l6.25 6.25a1.75 1.75 0 0 1 0 2.474l-5.026 5.026a1.75 1.75 0 0 1-2.474 0l-6.25-6.25A1.75 1.75 0 0 1 1 7.775Zm1.5 0c0 .066.026.13.073.177l6.25 6.25a.25.25 0 0 0 .354 0l5.025-5.025a.25.25 0 0 0 0-.354l-6.25-6.25a.25.25 0 0 0-.177-.073H2.75a.25.25 0 0 0-.25.25ZM6 5a1 1 0 1 1 0-2 1 1 0 0 1 0 2Z"></path></svg> <b>${r.tags}</b> tags</span>
            <span class="stat-item">${fmt(r.size_bytes)}</span>
          </div>
        </article>
      `).join('')}
    </div>
  `;
}

async function code(repo, path = '', reqRef = '') {
  const refsData = await api(`/api/v1/repos/${encodeURIComponent(repo)}/refs`);
  const branches = refsData.branches || [];
  const tags = refsData.tags || [];
  const branch = reqRef || branches[0]?.name || 'main';

  const tree = await api(`/api/v1/repos/${encodeURIComponent(repo)}/tree?ref=${encodeURIComponent(branch)}&path=${encodeURIComponent(path)}`);

  // Path Breadcrumbs
  const segments = path.split('/').filter(Boolean);
  let crumbsHtml = `<a class="crumb-link" href="${link(repo, 'code', { ref: branch })}"><b>${esc(repo)}</b></a>`;
  let accum = '';
  segments.forEach((seg, idx) => {
    accum += (accum ? '/' : '') + seg;
    crumbsHtml += ` <span class="crumb-sep">/</span> <a class="crumb-link" href="${link(repo, 'code', { ref: branch, path: accum })}">${esc(seg)}</a>`;
  });

  // Sort entries: directories first, then alphabetical
  const sorted = (tree.entries || []).sort((a, b) => {
    if (a.type === 'tree' && b.type !== 'tree') return -1;
    if (a.type !== 'tree' && b.type === 'tree') return 1;
    return a.name.localeCompare(b.name);
  });

  // Check for README
  const readmeEntry = sorted.find(e => e.type === 'blob' && /^readme(\.md|\.txt)?$/i.test(e.name));

  // Rows
  const rows = sorted.map(e => {
    const isDir = e.type === 'tree';
    const itemPath = path ? `${path}/${e.name}` : e.name;
    const itemUrl = isDir ? link(repo, 'code', { ref: branch, path: itemPath }) : link(repo, 'blob', { ref: branch, path: itemPath });
    const icon = isDir ? `
      <svg class="octicon file-icon dir-icon" viewBox="0 0 16 16" width="16" height="16"><path fill="#54aeff" d="M1.75 1A1.75 1.75 0 0 0 0 2.75v10.5C0 14.216.784 15 1.75 15h12.5A1.75 1.75 0 0 0 16 13.25v-8.5A1.75 1.75 0 0 0 14.25 3H7.5a.25.25 0 0 1-.2-.1l-.9-1.2C6.07 1.26 5.55 1 5 1H1.75Z"></path></svg>
    ` : `
      <svg class="octicon file-icon" viewBox="0 0 16 16" width="16" height="16"><path fill="#656d76" d="M2 1.75C2 .784 2.784 0 3.75 0h6.586c.464 0 .909.184 1.237.513l2.914 2.914c.329.328.513.773.513 1.237v9.586A1.75 1.75 0 0 1 13.25 16h-9.5A1.75 1.75 0 0 1 2 14.25Zm1.75-.25a.25.25 0 0 0-.25.25v12.5c0 .138.112.25.25.25h9.5a.25.25 0 0 0 .25-.25V6h-2.75A1.75 1.75 0 0 1 9 4.25V1.5Zm6.75.793V4.25c0 .138.112.25.25.25h2.707Z"></path></svg>
    `;

    return `
      <div class="tree-row">
        <div class="tree-name-col">
          ${icon}
          <a class="tree-name-link ${isDir ? 'dir-name' : ''}" href="${itemUrl}">${esc(e.name)}</a>
        </div>
        <div class="tree-hash-col">
          <span class="mono-hash">${esc(e.hash.slice(0, 7))}</span>
        </div>
      </div>
    `;
  }).join('');

  let readmeHtml = '';
  if (readmeEntry) {
    const readmePath = path ? `${path}/${readmeEntry.name}` : readmeEntry.name;
    const blobData = await api(`/api/v1/repos/${encodeURIComponent(repo)}/blob?ref=${encodeURIComponent(branch)}&path=${encodeURIComponent(readmePath)}`);
    readmeHtml = `
      <article class="readme-card">
        <header class="readme-card-header">
          <div class="readme-title">
            <svg class="octicon" viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M0 1.75A.75.75 0 0 1 .75 1h4.253c1.227 0 2.317.59 3 1.501A3.743 3.743 0 0 1 11.006 1h4.244a.75.75 0 0 1 .75.75v10.5a.75.75 0 0 1-.75.75h-4.244a2.25 2.25 0 0 0-1.762.85.75.75 0 0 1-1.188 0A2.25 2.25 0 0 0 6.003 13H1.75a.75.75 0 0 1-.75-.75Zm7.25 9.99V3.25c0-.69-.56-1.25-1.25-1.25H1.5v9h4.503c.427 0 .83.155 1.247.74Zm1.5 0c.418-.585.82-.74 1.247-.74h4.503v-9h-4.244c-.69 0-1.25.56-1.25 1.25Z"></path></svg>
            <span>${esc(readmeEntry.name)}</span>
          </div>
          <button class="btn-copy" onclick="copyText(${esc(JSON.stringify(blobData.content || ''))}, this)">Copiar raw</button>
        </header>
        <div class="readme-content markdown-body">
          ${renderMarkdown(blobData.content || '')}
        </div>
      </article>
    `;
  }

  const toolbar = `
    <div class="code-toolbar">
      <div class="code-toolbar-left">
        ${renderBranchSelector(repo, branch, branches, path, 'code')}
        <nav class="breadcrumb-trail">${crumbsHtml}</nav>
      </div>
      <div class="code-toolbar-right">
        <span class="file-count">${sorted.length} item(ns)</span>
      </div>
    </div>
  `;

  app.innerHTML = frame(repo, 'code', `
    ${toolbar}
    <div class="tree-table">
      <div class="tree-table-header">
        <span>Nome</span>
        <span>Último commit</span>
      </div>
      ${rows || '<div class="empty-state">Diretório vazio.</div>'}
    </div>
    ${readmeHtml}
  `, branch, branches, tags);
}

async function blob(repo, path, reqRef = '') {
  const refsData = await api(`/api/v1/repos/${encodeURIComponent(repo)}/refs`);
  const branches = refsData.branches || [];
  const tags = refsData.tags || [];
  const branch = reqRef || branches[0]?.name || 'main';

  const d = await api(`/api/v1/repos/${encodeURIComponent(repo)}/blob?ref=${encodeURIComponent(branch)}&path=${encodeURIComponent(path)}`);

  // Path segments
  const segments = path.split('/').filter(Boolean);
  let crumbsHtml = `<a class="crumb-link" href="${link(repo, 'code', { ref: branch })}"><b>${esc(repo)}</b></a>`;
  let accum = '';
  segments.forEach((seg, idx) => {
    accum += (accum ? '/' : '') + seg;
    const isLast = idx === segments.length - 1;
    if (isLast) {
      crumbsHtml += ` <span class="crumb-sep">/</span> <span class="crumb-current">${esc(seg)}</span>`;
    } else {
      crumbsHtml += ` <span class="crumb-sep">/</span> <a class="crumb-link" href="${link(repo, 'code', { ref: branch, path: accum })}">${esc(seg)}</a>`;
    }
  });

  const lines = (d.content || '').split('\n');
  const lineCount = lines.length;

  const codeRows = lines.map((l, i) => `
    <tr class="blob-line">
      <td class="blob-num" data-line="${i + 1}">${i + 1}</td>
      <td class="blob-code">${esc(l)}</td>
    </tr>
  `).join('');

  const toolbar = `
    <div class="code-toolbar">
      <div class="code-toolbar-left">
        ${renderBranchSelector(repo, branch, branches, path, 'code')}
        <nav class="breadcrumb-trail">${crumbsHtml}</nav>
      </div>
    </div>
  `;

  const blobBox = `
    <div class="blob-card">
      <div class="blob-header">
        <div class="blob-info">
          <span>${lineCount} linhas</span>
          <span class="sep-dot">·</span>
          <span>${fmt(d.size)}</span>
        </div>
        <div class="blob-actions">
          ${!d.binary ? `<button class="btn-copy" onclick="copyText(${esc(JSON.stringify(d.content))}, this)">Copiar conteúdo</button>` : ''}
        </div>
      </div>
      ${d.binary ? `
        <div class="empty-state">Arquivo binário. Visualização em texto indisponível.</div>
      ` : `
        <div class="blob-content-wrap">
          <table class="blob-table">${codeRows}</table>
        </div>
      `}
    </div>
  `;

  app.innerHTML = frame(repo, 'code', `${toolbar}${blobBox}`, branch, branches, tags);
}

async function commits(repo, reqRef = '') {
  const refsData = await api(`/api/v1/repos/${encodeURIComponent(repo)}/refs`);
  const branches = refsData.branches || [];
  const tags = refsData.tags || [];
  const branch = reqRef || branches[0]?.name || 'main';

  const d = await api(`/api/v1/repos/${encodeURIComponent(repo)}/commits?ref=${encodeURIComponent(branch)}`);

  const toolbar = `
    <div class="code-toolbar">
      <div class="code-toolbar-left">
        ${renderBranchSelector(repo, branch, branches, '', 'commits')}
      </div>
      <div class="code-toolbar-right">
        <span class="file-count">${d.commits.length} commits</span>
      </div>
    </div>
  `;

  const list = `
    <div class="commit-list-box">
      ${d.commits.map(c => `
        <div class="commit-item">
          <div class="commit-main">
            <div class="commit-subject">${esc(c.subject)}</div>
            <div class="commit-author-row">
              <span class="commit-author-name">${esc(c.author)}</span>
              <span class="sep-dot">·</span>
              <span class="commit-date">${date(c.date)}</span>
            </div>
          </div>
          <div class="commit-hash-actions">
            <button class="btn-copy btn-hash" onclick="copyText(${esc(JSON.stringify(c.hash))}, this)" title="Copiar hash completo">${esc(c.short_hash)}</button>
            <a class="btn-tree-link" href="${link(repo, 'code', { ref: c.hash })}" title="Navegar no estado deste commit">
              <svg class="octicon" viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="m11.28 3.22 4.25 4.25a.75.75 0 0 1 0 1.06l-4.25 4.25a.749.749 0 0 1-1.275-.326.749.749 0 0 1 .215-.734L13.94 8l-3.72-3.72a.749.749 0 0 1 .326-1.275.749.749 0 0 1 .734.215Zm-6.56 0a.751.751 0 0 1 1.06 0 .75.75 0 0 1 0 1.06L2.06 8l3.72 3.72a.749.749 0 0 1-.326 1.275.749.749 0 0 1-.734-.215L.47 8.53a.75.75 0 0 1 0-1.06l4.25-4.25a.75.75 0 0 1 1.06 0Z"></path></svg>
            </a>
          </div>
        </div>
      `).join('') || '<div class="empty-state">Nenhum commit encontrado nesta revisão.</div>'}
    </div>
  `;

  app.innerHTML = frame(repo, 'commits', `${toolbar}${list}`, branch, branches, tags);
}

async function refs(repo) {
  const d = await api(`/api/v1/repos/${encodeURIComponent(repo)}/refs`);
  const branches = d.branches || [];
  const tags = d.tags || [];

  const renderSection = (title, items, isBranch = true) => `
    <div class="refs-section">
      <div class="refs-header">
        <h3>${title} (${items.length})</h3>
      </div>
      <div class="refs-table">
        ${items.map(r => `
          <div class="refs-row">
            <div class="refs-left">
              <svg class="octicon" viewBox="0 0 16 16" width="16" height="16"><path fill="currentColor" d="M9.5 3.25a2.25 2.25 0 1 1 3 2.122V6A2.5 2.5 0 0 1 10 8.5H6a1 1 0 0 0-1 1v1.128a2.251 2.251 0 1 1-1.5 0V5.372a2.25 2.25 0 1 1 1.5 0v1.836A2.493 2.493 0 0 1 6 7h4a1 1 0 0 0 1-1v-.628A2.25 2.25 0 0 1 9.5 3.25Z"></path></svg>
              <a class="ref-name" href="${link(repo, 'code', { ref: r.name })}">${esc(r.name)}</a>
              ${r.name === 'main' ? '<span class="badge badge-default">Padrão</span>' : ''}
            </div>
            <div class="refs-right">
              <span class="mono-hash">${esc(r.hash.slice(0, 7))}</span>
              <span class="ref-date">${date(r.date)}</span>
              ${isBranch && r.name !== 'main' ? `
                <a class="btn-compare-link" href="${link(repo, 'compare', { base: 'main', head: r.name })}">Comparar com main</a>
              ` : ''}
            </div>
          </div>
        `).join('') || '<div class="empty-state">Nenhuma referência encontrada.</div>'}
      </div>
    </div>
  `;

  app.innerHTML = frame(repo, 'refs', `
    <div class="refs-grid">
      ${renderSection('Branches', branches, true)}
      ${renderSection('Tags', tags, false)}
    </div>
  `, 'main', branches, tags);
}

async function compare(repo, reqBase = '', reqHead = '') {
  const refsData = await api(`/api/v1/repos/${encodeURIComponent(repo)}/refs`);
  const branches = refsData.branches || [];
  const tags = refsData.tags || [];

  const defaultBase = reqBase || 'main~1';
  const defaultHead = reqHead || 'main';

  app.innerHTML = frame(repo, 'compare', `
    <div class="compare-card">
      <div class="compare-top">
        <h2>Comparação de Revisões</h2>
        <p class="compare-desc">Analise as alterações entre dois ramos, tags ou commits do repositório.</p>
      </div>

      <form class="compare-form" id="compareForm">
        <div class="compare-fields">
          <div class="compare-field">
            <label>Base (revisão de origem):</label>
            <input class="input-text" name="base" value="${esc(defaultBase)}" placeholder="ex.: main~1 ou feat/..." required>
          </div>
          <div class="compare-arrow">←</div>
          <div class="compare-field">
            <label>Destino (alterações a aplicar):</label>
            <input class="input-text" name="head" value="${esc(defaultHead)}" placeholder="ex.: main ou feat/..." required>
          </div>
          <div class="compare-action">
            <button class="btn-primary" type="submit">Comparar alterações</button>
          </div>
        </div>
      </form>

      ${branches.length > 1 ? `
        <div class="quick-compare-chips">
          <span class="chips-label">Atalhos rápidos:</span>
          ${branches.filter(b => b.name !== 'main').map(b => `
            <a class="chip-btn" href="${link(repo, 'compare', { base: 'main', head: b.name })}">main ← ${esc(b.name)}</a>
          `).join('')}
          <a class="chip-btn" href="${link(repo, 'compare', { base: 'main~1', head: 'main' })}">main~1 ← main</a>
        </div>
      ` : ''}

      <div id="diffOutput" class="diff-output-container">
        <div class="loading-state">Carregando comparação…</div>
      </div>
    </div>
  `, 'main', branches, tags);

  const form = document.querySelector('#compareForm');
  const out = document.querySelector('#diffOutput');

  const doCompare = async (baseVal, headVal) => {
    out.innerHTML = '<div class="loading-state">Calculando diff…</div>';
    try {
      const d = await api(`/api/v1/repos/${encodeURIComponent(repo)}/compare?base=${encodeURIComponent(baseVal)}&head=${encodeURIComponent(headVal)}`);
      out.innerHTML = renderDiff(d.diff);
    } catch (err) {
      out.innerHTML = `<div class="error-state"><strong>Erro na comparação:</strong> ${esc(err.message)}</div>`;
    }
  };

  form.onsubmit = e => {
    e.preventDefault();
    const fd = new FormData(form);
    const b = fd.get('base').trim();
    const h = fd.get('head').trim();
    location.hash = link(repo, 'compare', { base: b, head: h });
    doCompare(b, h);
  };

  // Run initial compare
  doCompare(defaultBase, defaultHead);
}

async function infra() {
  const d = await api('/api/v1/ha');
  app.innerHTML = `
    <section class="hero-github">
      <div class="hero-content">
        <div class="eyebrow">Observabilidade e Alta Disponibilidade</div>
        <h1>Infraestrutura do Cluster</h1>
        <p class="hero-desc">Monitoramento de integridade dos nós e sincronização dos repositórios do SisTer Git.</p>
      </div>
      <div class="hero-badges">
        <span class="status-pill status-warn">● Monitoramento Inicial</span>
      </div>
    </section>

    <div class="infra-panel">
      <div class="infra-banner">
        <b>Topologia Ativa:</b> Servidor de produção com réplica planejada. Nenhuma operação perigosa é permitida nesta interface.
      </div>

      <div class="nodes-grid">
        ${d.nodes.map(n => `
          <div class="node-card">
            <div class="node-header">
              <span class="status-badge">${esc(n.status)}</span>
              <h3>${esc(n.name)}</h3>
            </div>
            <div class="node-body">
              <div class="node-row"><span class="label">Função:</span> <span>${esc(n.role)}</span></div>
              <div class="node-row"><span class="label">Protocolo:</span> <span>Git via SSH (somente chaves Ed25519)</span></div>
            </div>
          </div>
        `).join('')}
      </div>
    </div>
  `;
}

async function render() {
  app.innerHTML = '<div class="loading-state">Consultando SisTer Git…</div>';
  try {
    const p = parseHash();
    if (!p.repo) {
      if (p.tab === 'infra') return await infra();
      return await home();
    }
    if (p.repo === 'infra') return await infra();

    if (p.tab === 'code') return await code(p.repo, p.path, p.ref);
    if (p.tab === 'blob') return await blob(p.repo, p.path, p.ref);
    if (p.tab === 'commits') return await commits(p.repo, p.ref);
    if (p.tab === 'refs') return await refs(p.repo);
    if (p.tab === 'compare') return await compare(p.repo, p.base, p.head);

    throw Error('Página ou guia inexistente');
  } catch (err) {
    app.innerHTML = `
      <div class="error-container">
        <h2>Não foi possível carregar</h2>
        <p>${esc(err.message)}</p>
        <a class="btn-primary" href="#/">Voltar aos repositórios</a>
      </div>
    `;
  }
}

window.addEventListener('hashchange', render);
render();
