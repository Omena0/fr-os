document.addEventListener('DOMContentLoaded', () => {
  const STORAGE_KEY = 'pjb-sidebar-state';
  const sitePrefixRaw = (document.documentElement.getAttribute('data-site-prefix') || '').trim();
  const SITE_PREFIX = sitePrefixRaw
    ? ('/' + sitePrefixRaw.replace(/^\/+|\/+$/g, '') + '/')
    : '';

  // ── Helpers ──────────────────────────────────────────────────
  function toSiteHref(path) {
    if (typeof path !== 'string') return '';
    if (/^(?:[a-z]+:)?\/\//i.test(path) || path.startsWith('mailto:') || path.startsWith('tel:') || path.startsWith('javascript:')) {
      return path;
    }
    const clean = String(path || '').replace(/^\/+/, '');
    if (!SITE_PREFIX) return clean;
    const prefix = SITE_PREFIX.replace(/^\/+|\/+$/g, '');
    if (clean === prefix || clean.startsWith(`${prefix}/`)) return `/${clean}`;
    return SITE_PREFIX + clean;
  }

  function loadState() {
    try { return JSON.parse(localStorage.getItem(STORAGE_KEY)) || {}; } catch { return {}; }
  }
  function saveState(state) {
    try { localStorage.setItem(STORAGE_KEY, JSON.stringify(state)); } catch { }
  }

  // ── Restore sidebar collapsed/expanded state ────────────────
  const state = loadState();
  document.querySelectorAll('.sidebar-heading[data-section]').forEach(h => {
    const key = h.dataset.section;
    if (key in state) {
      h.classList.toggle('collapsed', state[key]);
    }
    // Measure natural height so CSS max-height animation works
    const ul = h.nextElementSibling;
    if (ul && ul.classList.contains('sidebar-links')) {
      if (!h.classList.contains('collapsed')) {
        ul.style.maxHeight = ul.scrollHeight + 'px';
      }
    }
  });

  // ── Highlight current page ──────────────────────────────────
  const currentPage = location.pathname.split('/').pop() || 'index.html';
  document.querySelectorAll('.sidebar-links a').forEach(a => {
    const href = a.getAttribute('href');
    if (href && !href.startsWith('#')) {
      if (href.split('/').pop() === currentPage) a.classList.add('active');
    }
  });

  // ── Sidebar search filter ───────────────────────────────────
  const input = document.getElementById('sidebar-search');
  if (input) {
    input.addEventListener('input', () => {
      const q = input.value.toLowerCase().trim();
      document.querySelectorAll('.sidebar-links a').forEach(a => {
        a.style.display = (!q || a.textContent.toLowerCase().includes(q)) ? '' : 'none';
      });
      document.querySelectorAll('.sidebar-section').forEach(sec => {
        const links = sec.querySelectorAll('.sidebar-links a');
        const anyVisible = Array.from(links).some(a => a.style.display !== 'none');
        sec.style.display = anyVisible || !q ? '' : 'none';
        if (q) {
          const heading = sec.querySelector('.sidebar-heading');
          if (heading) expandSection(heading);
        }
      });
    });
    input.addEventListener('keydown', e => {
      if (e.key === 'Enter') {
        const first = document.querySelector('.sidebar-links a:not([style*="display: none"])');
        if (first) { first.click(); }
      }
    });
  }

  // ── Collapsible sections with animation ─────────────────────
  function expandSection(heading) {
    heading.classList.remove('collapsed');
    const ul = heading.nextElementSibling;
    if (ul && ul.classList.contains('sidebar-links')) {
      ul.style.maxHeight = ul.scrollHeight + 'px';
    }
  }
  function collapseSection(heading) {
    heading.classList.add('collapsed');
    const ul = heading.nextElementSibling;
    if (ul && ul.classList.contains('sidebar-links')) {
      // Force a reflow so the transition starts from the current height
      ul.style.maxHeight = ul.scrollHeight + 'px';
      ul.offsetHeight; // reflow
      ul.style.maxHeight = '0';
    }
  }

  document.querySelectorAll('.sidebar-heading').forEach(h => {
    h.addEventListener('click', () => {
      const willCollapse = !h.classList.contains('collapsed');
      if (willCollapse) {
        collapseSection(h);
      } else {
        expandSection(h);
      }
      // Persist
      const key = h.dataset.section;
      if (key) {
        const s = loadState();
        s[key] = willCollapse;
        saveState(s);
      }
    });
  });

  // After expand animation ends, set max-height to none so dynamically
  // added content (e.g. search results) isn't clipped
  document.querySelectorAll('.sidebar-links').forEach(ul => {
    ul.addEventListener('transitionend', () => {
      const heading = ul.previousElementSibling;
      if (heading && !heading.classList.contains('collapsed')) {
        ul.style.maxHeight = 'none';
      }
    });
  });

  // ── Mobile menu toggle ──────────────────────────────────────
  const toggle = document.querySelector('.menu-toggle');
  const sidebar = document.querySelector('.sidebar');
  const overlay = document.querySelector('.sidebar-overlay');
  if (toggle && sidebar) {
    toggle.addEventListener('click', () => {
      sidebar.classList.toggle('open');
      if (overlay) overlay.classList.toggle('open');
    });
    if (overlay) {
      overlay.addEventListener('click', () => {
        sidebar.classList.remove('open');
        overlay.classList.remove('open');
      });
    }
  }

  // ── Back to top ─────────────────────────────────────────────
  const btn = document.querySelector('.back-to-top');
  if (btn) {
    window.addEventListener('scroll', () => {
      btn.classList.toggle('visible', window.scrollY > 400);
    });
    btn.addEventListener('click', () => window.scrollTo({ top: 0, behavior: 'smooth' }));
  }

  // ── Active sidebar link tracking + TOC sub-section collapse ──
  const sections = document.querySelectorAll('.api-section, h2[id]');
  const allLinks = document.querySelectorAll('.sidebar-links a');
  const tocSubs = document.querySelectorAll('.toc-sub');

  function expandTocSub(sub) {
    if (sub.classList.contains('expanded')) return;
    sub.classList.add('expanded');
    sub.style.maxHeight = sub.scrollHeight + 'px';
    sub.addEventListener('transitionend', function handler() {
      if (sub.classList.contains('expanded')) sub.style.maxHeight = 'none';
      sub.removeEventListener('transitionend', handler);
    });
  }
  function collapseTocSub(sub) {
    if (!sub.classList.contains('expanded')) return;
    sub.style.maxHeight = sub.scrollHeight + 'px';
    sub.offsetHeight; // reflow
    sub.classList.remove('expanded');
    sub.style.maxHeight = '0';
  }

  function updateActiveTocSub(activeH2Id) {
    tocSubs.forEach(sub => {
      if (sub.dataset.parent === activeH2Id) {
        expandTocSub(sub);
      } else {
        collapseTocSub(sub);
      }
    });
  }

  if (sections.length && allLinks.length) {
    const observer = new IntersectionObserver(entries => {
      entries.forEach(entry => {
        if (entry.isIntersecting) {
          const { id } = entry.target;
          allLinks.forEach(l => {
            l.classList.toggle('active', l.getAttribute('href') === '#' + id);
          });
          // Find the parent H2 for this section
          let h2Id = id;
          const el = entry.target;
          if (el.tagName === 'H3' || (el.tagName !== 'H2' && el.closest && !el.matches('h2'))) {
            // Walk backwards to find the owning H2
            let prev = el;
            while (prev) {
              prev = prev.previousElementSibling;
              if (prev && prev.tagName === 'H2' && prev.id) {
                h2Id = prev.id;
                break;
              }
            }
          }
          updateActiveTocSub(h2Id);
        }
      });
    }, { rootMargin: '-80px 0px -60% 0px', threshold: 0 });
    sections.forEach(s => { if (s.id) observer.observe(s); });

    // Also observe h3[id] elements for sub-section tracking
    document.querySelectorAll('h3[id]').forEach(s => observer.observe(s));
  }

  // ── Smooth scroll for sidebar links ─────────────────────────
  document.querySelectorAll('.sidebar-links a[href^="#"]').forEach(a => {
    a.addEventListener('click', e => {
      const target = document.querySelector(a.getAttribute('href'));
      if (target) {
        e.preventDefault();
        target.scrollIntoView({ behavior: 'smooth' });
        if (sidebar) sidebar.classList.remove('open');
        if (overlay) overlay.classList.remove('open');
        history.replaceState(null, '', a.getAttribute('href'));
      }
    });
  });

  // ── Decompress zstd search index (async, non-blocking) ──────
  let searchIndex = [];
  const SEARCH_INDEX_FILE = 'search_index.zst';
  const SEARCH_PRELOAD_DELAY_MS = 2000;
  const FZSTD_CDN_URL = 'https://cdn.jsdelivr.net/npm/fzstd@0.1.1/umd/index.min.js';
  let searchIndexPromise = null;
  let searchPreloadTimer = null;
  let fzstdPromise = null;

  function emitSearchReady() {
    if (searchIndex && searchIndex.length) {
      document.dispatchEvent(new CustomEvent('searchIndexLoaded'));
      return true;
    }
    return false;
  }

  async function ensureFzstd(maxWaitMs = 10000) {
    if (typeof fzstd !== 'undefined') return true;
    if (!fzstdPromise) {
      fzstdPromise = new Promise(resolve => {
        const existing = document.querySelector('script[data-pjb-fzstd]');
        if (existing) {
          const started = Date.now();
          const iv = setInterval(() => {
            if (typeof fzstd !== 'undefined') {
              clearInterval(iv);
              resolve(true);
              return;
            }
            if (Date.now() - started >= maxWaitMs) {
              clearInterval(iv);
              resolve(false);
            }
          }, 50);
          return;
        }

        const script = document.createElement('script');
        script.src = FZSTD_CDN_URL;
        script.async = true;
        script.defer = true;
        script.setAttribute('data-pjb-fzstd', '1');
        script.onload = () => resolve(typeof fzstd !== 'undefined');
        script.onerror = () => resolve(false);
        document.head.appendChild(script);

        setTimeout(() => resolve(typeof fzstd !== 'undefined'), maxWaitMs);
      });
    }
    return fzstdPromise;
  }

  async function loadCompressedJson(filename, fallback = null) {
    if (location.protocol === 'file:') return fallback;
    if (!await ensureFzstd()) return fallback;
    try {
      const response = await fetch(toSiteHref(filename), { cache: 'force-cache' });
      if (!response.ok) return fallback;
      const compressed = new Uint8Array(await response.arrayBuffer());
      return JSON.parse(new TextDecoder().decode(fzstd.decompress(compressed)));
    } catch (e) {
      return fallback;
    }
  }

  function loadInlineSearchFallback() {
    const el = document.getElementById('zstd-data');
    if (!el || !el.textContent.trim()) return false;
    try {
      const b64 = el.textContent.trim();
      const compressed = Uint8Array.from(atob(b64), c => c.charCodeAt(0));
      const decompressed = fzstd.decompress(compressed);
      const json = new TextDecoder().decode(decompressed);
      searchIndex = JSON.parse(json);
      return emitSearchReady();
    } catch (e) {
      console.error('Failed to load inline search index:', e);
      return false;
    }
  }

  async function loadSearchIndex() {
    if (searchIndex && searchIndex.length) return true;
    if (searchIndexPromise) return searchIndexPromise;

    searchIndexPromise = (async () => {
      const hasFzstd = await ensureFzstd();
      if (!hasFzstd) {
        console.error('Search disabled: fzstd failed to load');
        return false;
      }

      if (loadInlineSearchFallback()) {
        await Promise.all([loadSymbolIndex(), loadFileIndex()]);
        return true;
      }

      if (location.protocol === 'file:') {
        return false;
      }

      try {
        const r = await fetch(toSiteHref(SEARCH_INDEX_FILE), { cache: 'force-cache' });
        if (!r || !r.ok) throw new Error(`HTTP ${r ? r.status : 'error'}`);
        const compressed = new Uint8Array(await r.arrayBuffer());
        const decompressed = fzstd.decompress(compressed);
        const json = new TextDecoder().decode(decompressed);
        searchIndex = JSON.parse(json);

        // Also load source indexes used by search
        await Promise.all([loadSymbolIndex(), loadFileIndex()]);

        return emitSearchReady();
      } catch (e) {
        console.error('Failed to fetch/decompress search index:', e);
        return false;
      }
    })();

    return searchIndexPromise;
  }

  function triggerSearchIndexLoad() {
    if (searchPreloadTimer) {
      clearTimeout(searchPreloadTimer);
      searchPreloadTimer = null;
    }
    void loadSearchIndex();
  }

  function scheduleSearchIndexLoad() {
    if (searchPreloadTimer || searchIndexPromise || (searchIndex && searchIndex.length)) return;
    searchPreloadTimer = setTimeout(() => {
      searchPreloadTimer = null;
      void loadSearchIndex();
    }, SEARCH_PRELOAD_DELAY_MS);
  }
  scheduleSearchIndexLoad();


  searchIndex = null;
  let symbolIndex = null;
  let fileIndex = null;
  const searchConfig = (() => {
    const el = document.getElementById('search-config');
    try {
      return el ? JSON.parse(el.textContent) : {};
    } catch (e) {
      return {};
    }
  })();
  const searchIncludes = Object.assign({
    pages: true,
    titles: true,
    headings: true,
    content: true,
    symbols: true,
    files: true
  }, searchConfig);

  // Custom-tag config embedded by the builder (e.g. {"ext": {"display": "ext", "class": "ext-tag"}})
  const customTagsConfig = (() => {
    const el = document.getElementById('custom-tags-config');
    try {
      return el ? JSON.parse(el.textContent) : { ext: { display: 'ext', class: 'ext-tag' } };
    } catch (e) {
      return { ext: { display: 'ext', class: 'ext-tag' } };
    }
  })();

  function formatCustomTags(text) {
    if (!text) return text;
    const tagNames = Object.keys(customTagsConfig);
    if (!tagNames.length) return text;
    const pattern = new RegExp('\\[(' + tagNames.map(t => t.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')).join('|') + ')\\]', 'g');
    return text.replace(pattern, (m, tag) => {
      const spec = customTagsConfig[tag];
      if (!spec) return m;
      const cls = spec.class || 'ext-tag';
      const display = spec.display || tag;
      return `<span class="${cls}">${display}</span>`;
    });
  }

  // ── Load symbol index ──────────────────────────────────────────
  async function loadSymbolIndex() {
    if (symbolIndex !== null) return symbolIndex;
    try {
      symbolIndex = await loadCompressedJson('symbol_index.zst', []);
    } catch (e) {
      console.warn('Failed to load symbol index:', e);
      symbolIndex = [];
    }
    return symbolIndex;
  }

  async function loadFileIndex() {
    if (fileIndex !== null) return fileIndex;
    try {
      fileIndex = await loadCompressedJson('file_index.zst', []);
    } catch (e) {
      fileIndex = [];
    }
    return fileIndex;
  }

  // ── Header full-text search ─────────────────────────────────
  const headerSearch = document.getElementById('header-search');
  const searchResults = document.getElementById('search-results');

  function doSearch(query) {
    if (!searchIndex || !query.trim()) {
      searchResults.style.display = 'none';
      return;
    }
    const q = query.toLowerCase().trim();
    const tokens = q.split(/\s+/).filter(Boolean);
    const hits = [];

    function scorePage(page) {
      let score = 0;
      const title = (page.title || '').toLowerCase();
      if (searchIncludes.titles) {
        if (title === q) score += 200;
        else if (title.includes(q)) score += 120 - Math.min(100, title.indexOf(q));
      }

      for (const sec of page.sections || []) {
        const heading = (sec.heading || '').toLowerCase();
        const text = (sec.text || '').toLowerCase();
        if (searchIncludes.headings && heading.includes(q)) score += 40;
        if (searchIncludes.content && text.includes(q)) score += 20;
        for (const t of tokens) {
          if (searchIncludes.titles && title.includes(t)) score += 2;
          if (searchIncludes.headings && heading.includes(t)) score += 8;
          if (searchIncludes.content && text.includes(t)) score += 4;
        }
      }
      return score;
    }

    for (const page of searchIndex) {
      if (searchIncludes.pages) {
        const s = scorePage(page);
        if (s > 0) {
          let bestSec = null;
          for (const sec of page.sections || []) {
            for (const t of tokens) {
              if ((sec.text || '').toLowerCase().includes(t) || (sec.heading || '').toLowerCase().includes(t)) {
                bestSec = sec; break;
              }
            }
            if (bestSec) break;
          }
          hits.push({ url: page.url, title: page.title, text: bestSec ? bestSec.text : '', score: s, type: 'page' });
        }
      }
    }

    // Also search symbols
    if (searchIncludes.symbols && symbolIndex && symbolIndex.length > 0) {
      const qLower = q.toLowerCase();
      for (const sym of symbolIndex) {
        let score = 0;
        const name = (sym.name || '').toLowerCase();
        const file = (sym.file || '').toLowerCase();
        const type = (sym.type || '').toLowerCase();

        if (name === qLower) score += 150;
        else if (name.includes(qLower)) score += 80 - Math.min(50, name.indexOf(qLower));
        else {
          // Check tokens
          for (const t of tokens) {
            if (name.includes(t)) score += 10;
            if (type.includes(t)) score += 5;
          }
        }

        if (score > 0) {
          const hit = {
            url: `#coderef:${sym.file}:${sym.line}${sym.end_line ? '-' + sym.end_line : ''}`,
            title: `${sym.name} (${sym.type})`,
            text: `in ${sym.file}:${sym.line}${sym.end_line ? '-' + sym.end_line : ''} — ${sym.context || ''}`,
            score: score,
            type: 'symbol',
            symbol: sym
          };
          hits.push(hit);
        }
      }
    }

    if (searchIncludes.files && fileIndex && fileIndex.length > 0) {
      for (const file of fileIndex) {
        const name = (file.name || '').toLowerCase();
        const path = (file.file || '').toLowerCase();
        let score = 0;
        if (name === q) score += 140;
        else if (name.includes(q)) score += 70 - Math.min(40, name.indexOf(q));
        else {
          for (const token of tokens) {
            if (name.includes(token)) score += 8;
            if (path.includes(token)) score += 4;
          }
        }
        if (score > 0) {
          hits.push({
            url: `#coderef:${file.file}:1`,
            title: file.name,
            text: file.file,
            score,
            type: 'file'
          });
        }
      }
    }

    // Sort and dedupe
    hits.sort((a, b) => b.score - a.score);
    const seen = new Set();
    const unique = [];
    for (const h of hits) {
      const baseUrl = h.url.includes('#') ? h.url : h.url.split('#')[0];
      if (!seen.has(baseUrl) && unique.length < 12) {
        seen.add(baseUrl);
        unique.push(h);
      }
    }
    if (!unique.length) {
      searchResults.innerHTML = '<div class="search-no-results">No results</div>';
      searchResults.style.display = 'block';
      return;
    }
    searchResults.innerHTML = unique.map(h => {
      const snippet = h.text ? h.text.substring(0, 100) : '';
      const heading = h.heading ? ` › ${h.heading}` : '';
      const typeBadge = h.type === 'symbol' ? '<span class="ext-tag" style="margin-left:6px;background:var(--primary-soft);color:var(--primary)">symbol</span>' : h.type === 'file' ? '<span class="ext-tag" style="margin-left:6px;background:var(--primary-soft);color:var(--primary)">file</span>' : '';
      const fmtTitle = formatCustomTags(h.title + heading + typeBadge);
      const href = addVerToHref(h.url);
      return `<a class="search-hit" href="${href}"><strong>${fmtTitle}</strong><span>${snippet}</span></a>`;
    }).join('');
    searchResults.style.display = 'block';
  }

  // Click handler for search results (handles both regular links and coderef links)
  searchResults.addEventListener('click', async (e) => {
    const link = e.target.closest('.search-hit');
    if (!link) return;

    const href = link.getAttribute('href');
    if (href && href.startsWith('#coderef:')) {
      e.preventDefault();
      const match = href.match(/^#coderef:([^:]+):(\d+)(?:-(\d+))?$/);
      if (match) {
        const file = match[1];
        const line = parseInt(match[2], 10);
        const endLine = match[3] ? parseInt(match[3], 10) : null;
        const refs = parseCodeRefs();
        const ref = refs.find(r => r.file === file && r.line === line);
        // Ensure source files are loaded before showing panel
        await loadSourceFiles();
        if (ref) {
          if (endLine) ref.end_line = endLine;
          showCodePanel(ref);
        } else {
          showCodePanel({ file, line, end_line: endLine, id: 'search-result' });
        }
      }
    }
  });

  if (headerSearch) {
    const onSearchInteract = () => triggerSearchIndexLoad();
    headerSearch.addEventListener('pointerdown', onSearchInteract, { once: true });
    headerSearch.addEventListener('focus', onSearchInteract, { once: true });
    headerSearch.addEventListener('input', () => doSearch(headerSearch.value));
    headerSearch.addEventListener('keydown', e => {
      if (e.key === 'Escape') {
        headerSearch.value = '';
        searchResults.style.display = 'none';
        headerSearch.blur();
      }
      if (e.key === 'Enter') {
        const first = searchResults.querySelector('.search-hit');
        if (first) { window.location.href = first.getAttribute('href'); }
      }
    });
    document.addEventListener('click', e => {
      if (!headerSearch.contains(e.target) && !searchResults.contains(e.target)) {
        searchResults.style.display = 'none';
      }
    });
    // Ctrl+K shortcut
    document.addEventListener('keydown', e => {
      if ((e.ctrlKey || e.metaKey) && e.key === 'k') {
        e.preventDefault();
        headerSearch.focus();
        headerSearch.select();
      }
    });
  }

  // ── Code block copy buttons ─────────────────────────────────
  function addCopyButtons() {
    document.querySelectorAll('pre > code').forEach(codeEl => {
      const pre = codeEl.parentElement;
      if (!pre) return;
      if (pre.parentElement && pre.parentElement.classList.contains('code-wrap')) return;
      const wrap = document.createElement('div');
      wrap.className = 'code-wrap';
      pre.parentNode.insertBefore(wrap, pre);
      wrap.appendChild(pre);
      const btn = document.createElement('button');
      btn.className = 'copy-btn';
      btn.type = 'button';
      btn.setAttribute('aria-label', 'Copy code');
      btn.textContent = 'Copy';
      wrap.appendChild(btn);
      btn.addEventListener('click', async () => {
        try {
          const text = codeEl.innerText;
          await navigator.clipboard.writeText(text);
          btn.textContent = 'Copied';
          setTimeout(() => btn.textContent = 'Copy', 1500);
        } catch (e) {
          console.error('Copy failed', e);
          btn.textContent = 'Copy';
        }
      });
    });
  }
  // Run immediately for the current page
  try { addCopyButtons(); } catch (e) { /* ignore */ }

  // ── Inline copy commands ──────────────────────────────────────
  function initInlineCopyCommands() {
    document.body.addEventListener('click', async (e) => {
      const link = e.target.closest('a.copy-inline-command');
      if (!link) return;
      e.preventDefault();
      const cmd = link.dataset.copy || link.textContent.trim();
      try {
        await navigator.clipboard.writeText(cmd);
        showInlineToast(link, 'Copied');
      } catch (err) {
        console.error('Copy failed', err);
        showInlineToast(link, 'Failed');
      }
    });
  }

  function showInlineToast(el, text) {
    const existing = el.querySelector('.inline-toast');
    if (existing) existing.remove();
    const toast = document.createElement('span');
    toast.className = 'inline-toast';
    toast.textContent = text;
    el.appendChild(toast);
    requestAnimationFrame(() => toast.classList.add('show'));
    setTimeout(() => {
      toast.classList.remove('show');
      setTimeout(() => toast.remove(), 150);
    }, 1200);
  }

  // Run for current page
  try { initInlineCopyCommands(); } catch (e) { /* ignore */ }

  // ── Backlinks & Related pages ───────────────────────────────
  function renderBacklinksAndRelated() {
    if (!searchIndex || !searchIndex.length) return;
    const me = searchIndex.find(p => (p.url || '').split('/').pop() === currentPage);
    if (!me) return;
    const content = document.querySelector('.content');
    if (!content) return;
    if (document.getElementById('page-extras')) return; // only once

    const extras = document.createElement('div');
    extras.id = 'page-extras';
    extras.className = 'page-extras';

    // Related comes first (as requested). Render as an H2-equivalent section.
    if (me.related && me.related.length) {
      const hr = document.createElement('h2'); hr.textContent = 'Related';
      extras.appendChild(hr);
      const rel = document.createElement('div');
      rel.className = 'related';
      const ul = document.createElement('ul');
      me.related.slice(0, 12).forEach(slug => {
        const p = searchIndex.find(x => x.slug === slug);
        if (!p) return;
        const a = document.createElement('a');
        const hrefOrig = p.url || (slug === 'index' ? 'index.html' : slug + '.html');
        a.href = addVerToHref(hrefOrig);
        a.textContent = p.title || slug;
        const li = document.createElement('li'); li.appendChild(a); ul.appendChild(li);
      });
      rel.appendChild(ul);
      extras.appendChild(rel);
    }

    // Backlinks after Related. Collapsed by default using <details> so large lists don't overwhelm the page.
    if (me.backlinks && me.backlinks.length) {
      const hb = document.createElement('h2'); hb.textContent = 'Backlinks';
      extras.appendChild(hb);
      const det = document.createElement('details');
      det.className = 'backlinks-details';
      const summary = document.createElement('summary');
      summary.textContent = `Show ${me.backlinks.length} backlinks`;
      det.appendChild(summary);
      const ul = document.createElement('ul');
      me.backlinks.forEach(slug => {
        const p = searchIndex.find(x => x.slug === slug);
        if (!p) return;
        const a = document.createElement('a');
        const hrefOrig = p.url || (slug === 'index' ? 'index.html' : slug + '.html');
        a.href = addVerToHref(hrefOrig);
        a.textContent = p.title || slug;
        const li = document.createElement('li'); li.appendChild(a); ul.appendChild(li);
      });
      det.appendChild(ul);
      extras.appendChild(det);
    }

    if (extras.childElementCount) content.appendChild(extras);
  }

  // Ensure backlinks are rendered whenever the search index becomes available
  document.addEventListener('searchIndexLoaded', () => {
    try { renderBacklinksAndRelated(); } catch (e) { /* ignore */ }
    try { persistVerOnLinks(); } catch (e) { /* ignore */ }
    try {
      if (headerSearch && headerSearch.value.trim()) {
        doSearch(headerSearch.value);
      }
    } catch (e) { /* ignore */ }
  });

  // ── Versioning: ?ver= parameter support (resolve to commit, fetch historical HTML) ──
  async function loadGitMeta() {
    // In file:// mode, browsers commonly block local fetch/XHR.
    // Versioning is disabled in that mode.
    if (location.protocol === 'file:') {
      return null;
    }

    return await loadCompressedJson('git_meta.zst', null);
  }

  function resolveVerToCommit(ver, meta) {
    if (!meta) return null;
    const commits = meta.commits || [];
    // full or prefix hash
    if (/^[0-9a-f]{7,40}$/i.test(ver)) {
      const match = commits.find(c => c.startsWith(ver));
      if (match) return match;
      if (ver.length === 40) return ver; // accept full hash even if not in list
      return null;
    }
    // If a short numeric index was provided, treat it as an index into the commits
    if (/^\d+$/.test(ver)) {
      const i = parseInt(ver, 10);
      if (i >= 0 && i < commits.length) return commits[i];
      return null;
    }

    // If a version code like "1A" was provided, look it up in meta.versions
    if (meta.versions && Array.isArray(meta.versions)) {
      for (const v of meta.versions) {
        if (v.code && v.code.toLowerCase() === ver.toLowerCase()) return v.commit;
      }
    }

    // Fallback: check tags (legacy)
    if (meta.tags && meta.tags[ver]) return meta.tags[ver];
    for (const t in (meta.tags || {})) {
      if (t.toLowerCase() === ver.toLowerCase()) return meta.tags[t];
    }
    return null;
  }

  function applyPagesFilter(commit, meta) {
    try {
      if (!meta || !meta.pages_by_commit || !commit) return;
      const allowed = new Set(meta.pages_by_commit[commit] || []);
      if (!allowed.size) return;

      document.querySelectorAll('.sidebar-links a').forEach(a => {
        const href = a.getAttribute('href') || '';
        if (!href || href.startsWith('#')) return;
        const page = href.split('/').pop().split('?')[0].split('#')[0];
        const base = page.replace(/\.html$/, '');
        const li = a.closest('li') || a.parentElement;
        if (!allowed.has(base)) {
          if (li) li.style.display = 'none'; else a.style.display = 'none';
        }
        else if (li) li.style.display = '';
        else a.style.display = '';
      });

      // Hide entire sections with no visible links
      document.querySelectorAll('.sidebar-section').forEach(sec => {
        const links = sec.querySelectorAll('.sidebar-links a');
        const any = Array.from(links).some(l => {
          const li = l.closest('li') || l.parentElement;
          return li ? li.style.display !== 'none' : l.style.display !== 'none';
        });
        sec.style.display = any ? '' : 'none';
      });
    } catch (e) {
      console.error('applyPagesFilter failed:', e);
    }
  }

  async function handleVersionParam() {
    const params = new URLSearchParams(location.search);
    const ver = params.get('ver');
    if (!ver) return;
    const meta = await loadGitMeta();
    if (!meta) return;
    const commit = resolveVerToCommit(ver, meta);
    if (!commit) return;

    // Build raw.githubusercontent URL to fetch historical HTML. If the
    // generated HTML isn't present at that commit (404), fall back to
    // fetching the source markdown and render it client-side using
    // `marked` (included via CDN in the template).
    if (!meta.repo) return;
    const page = currentPage || 'index.html';
    // Use the site path embedded by the builder (e.g. "site", "docs/site",
    // or a custom build.out_dir value) instead of a hardcoded "docs/site".
    const sitePath = meta.site_path || 'docs/site';
    const rawHtmlUrl = `https://raw.githubusercontent.com/${meta.repo}/${commit}/${sitePath}/${page}`;
    try {
      const r = await fetch(rawHtmlUrl, { cache: 'no-store' });
      if (r && r.ok) {
        const html = await r.text();
        const parser = new DOMParser();
        const doc2 = parser.parseFromString(html, 'text/html');
        const newContent = doc2.querySelector('.content');
        if (!newContent) return;
        // Insert a banner to indicate this is a historical version
        const banner = document.createElement('div');
        banner.className = 'callout callout-info';
        const short = commit.slice(0, 8);
        banner.innerHTML = `<strong>Viewing version ${short}</strong> — <a href="${toSiteHref(page)}">View live</a>`;
        // Replace content
        const content = document.querySelector('.content');
        if (!content) return;
        content.innerHTML = '';
        content.appendChild(banner);
        // Append the historical content body
        Array.from(newContent.children).forEach(n => content.appendChild(document.importNode(n, true)));
        document.title = doc2.title || document.title;
        // Re-initialize copy buttons for the loaded content
        try { addCopyButtons(); } catch (e) { /* ignore */ }
        // Filter sidebar to pages that existed at this commit
        try { applyPagesFilter(commit, meta); } catch (e) { /* ignore */ }
        return;
      }
    } catch (e) {
      // network error or CORS — continue to markdown fallback
      console.error('Failed to fetch historical HTML page:', e);
    }

    // Markdown fallback: attempt to fetch the source markdown for this
    // page at that commit. The build embeds a `src_map` in `git_meta`
    // mapping page basenames to their docs/src path.
    try {
      const key = page.replace(/\.html$/, '');
      const srcPath = meta.src_map && meta.src_map[key];
      if (srcPath) {
        const mdUrl = `https://raw.githubusercontent.com/${meta.repo}/${commit}/${srcPath}`;
        try {
          const rm = await fetch(mdUrl, { cache: 'no-store' });
          if (rm && rm.ok) {
            const md = await rm.text();
            // Render markdown using `marked` (if available)
            let rendered = md;
            try {
              if (typeof marked === 'function') rendered = marked(md);
              else if (marked && typeof marked.parse === 'function') rendered = marked.parse(md);
            } catch (e) {
              console.error('Marked render failed:', e);
            }
            const content = document.querySelector('.content');
            if (!content) return;
            const banner = document.createElement('div');
            banner.className = 'callout callout-info';
            const short = commit.slice(0, 8);
            banner.innerHTML = `<strong>Viewing version ${short}</strong> — <a href="${toSiteHref(page)}">View live</a>`;
            content.innerHTML = '';
            content.appendChild(banner);
            // Insert rendered markdown (may be raw markdown if marked missing)
            const wrapper = document.createElement('div');
            wrapper.innerHTML = rendered;
            Array.from(wrapper.children).forEach(n => content.appendChild(document.importNode(n, true)));
            document.title = `${document.title} — ${short}`;
            try { addCopyButtons(); } catch (e) { /* ignore */ }
            // Filter sidebar to pages that existed at this commit
            try { applyPagesFilter(commit, meta); } catch (e) { /* ignore */ }
            return;
          }
        } catch (e) {
          console.error('Failed to fetch historical markdown:', e);
        }
      }
    } catch (e) {
      console.error('Markdown fallback error:', e);
    }
  }

  // Attempt to apply versioning if requested
  try { handleVersionParam(); } catch (e) { /* ignore */ }

  // Populate the version selector from git_meta.json
  async function populateVersionSelector() {
    const sel = document.getElementById('version-selector');
    if (!sel) return;
    const meta = await loadGitMeta();
    if (!meta) return;
    // Replace options with Live + commit-based versions detected in git commit messages
    sel.innerHTML = '';
    const live = document.createElement('option'); live.value = ''; live.textContent = 'Live';
    sel.appendChild(live);

    // If build emitted `meta.versions`, use that (commit messages like "1A - message")

    if (meta.versions && Array.isArray(meta.versions) && meta.versions.length) {
      // show newest first
      const list = meta.versions.slice().reverse();
      // Only include version codes that match digits+letter (e.g. 1A, 12B)
      const codeRE = /^[0-9]+[A-Za-z]$/;
      // If the builder provided pages_by_commit info, only include versions
      // whose commit contains the current page.
      const pagesByCommit = meta.pages_by_commit || {};
      const pageBase = (currentPage || 'index.html').replace(/\.html$/, '');
      const useFilter = pagesByCommit && Object.keys(pagesByCommit).length > 0;
      for (const v of list) {
        if (!v || !v.commit || !v.code) continue;
        if (!codeRE.test(String(v.code))) continue;
        if (useFilter) {
          const avail = pagesByCommit[v.commit] || pagesByCommit[v.commit.slice(0, 8)];
          if (!avail || !avail.includes(pageBase)) continue;
        }
        const opt = document.createElement('option');
        // Use the short version code as the option value so ?ver=1A is compact
        opt.value = v.code;
        // Display only the compact code in the select
        opt.textContent = v.code;
        if (v.label) opt.dataset.label = v.label;
        if (v.commit) opt.dataset.commit = v.commit.slice(0, 8);
        sel.appendChild(opt);
      }
    } else {
      // Fallback: show latest commit and recent commits (but filter by availability if possible)
      const commits = meta.commits || [];
      const pagesByCommit = meta.pages_by_commit || {};
      const pageBase = (currentPage || 'index.html').replace(/\.html$/, '');
      const useFilter = pagesByCommit && Object.keys(pagesByCommit).length > 0;
      if (commits.length) {
        const latest = commits[commits.length - 1];
        if (!useFilter || (pagesByCommit[latest] && pagesByCommit[latest].includes(pageBase))) {
          const opt = document.createElement('option'); opt.value = latest; opt.textContent = latest.slice(0, 8); opt.dataset.commit = latest.slice(0, 8); sel.appendChild(opt);
        }
        const recent = commits.slice(-10).reverse();
        for (const c of recent) {
          if (useFilter && (!(pagesByCommit[c] && pagesByCommit[c].includes(pageBase)))) continue;
          const o = document.createElement('option'); o.value = c; o.textContent = c.slice(0, 8); o.dataset.commit = c.slice(0, 8); sel.appendChild(o);
        }
      }
    }

    // No description shown under selector by design.

    // If a `ver` parameter exists in the URL, reflect it in the selector.
    try {
      const params = new URLSearchParams(location.search);
      const verParam = params.get('ver');
      if (verParam) {
        const v = verParam;
        // Try to find an exact option match (case-insensitive)
        let found = Array.from(sel.options).find(o => String(o.value).toLowerCase() === String(v).toLowerCase());
        if (!found) {
          // Try matching by short commit in data-commit
          const short = String(v).slice(0, 8).toLowerCase();
          found = Array.from(sel.options).find(o => o.dataset && o.dataset.commit && String(o.dataset.commit).toLowerCase() === short);
        }
        if (found) {
          sel.value = found.value;
        }
      }
    } catch (e) { /* ignore */ }

    sel.addEventListener('change', () => {
      const v = sel.value;
      // Navigate explicitly so behavior is consistent across environments.
      if (!v) {
        const params = new URLSearchParams(location.search);
        params.delete('ver');
        const qs = params.toString();
        const newUrl = qs ? `${location.pathname}?${qs}` : location.pathname;
        location.href = newUrl;
      } else {
        const newUrl = `${location.pathname}?ver=${encodeURIComponent(v)}`;
        location.href = newUrl;
      }
    });
    try { persistVerOnLinks(); } catch (e) { /* ignore */ }
  }

  // Helper: append current `ver` query param to an internal href if present.
  function addVerToHref(href) {
    try {
      if (!href) return href;
      // Skip special links; same-origin absolute HTTP(S) links are still internal.
      if (href.startsWith('mailto:') || href.startsWith('tel:') || href.startsWith('javascript:')) return href;
      if (href.startsWith('#')) return href;
      const params = new URLSearchParams(location.search);
      const ver = params.get('ver');
      if (!ver) return href;
      const u = new URL(href, location.href);
      if (u.origin !== location.origin) return href;
      u.searchParams.set('ver', ver);
      const isAbsolute = /^https?:\/\//i.test(href);
      return isAbsolute ? u.href : (u.pathname + u.search + u.hash);
    } catch (e) {
      return href;
    }
  }

  // Walk page links and persist `ver` for same-origin navigations.
  function persistVerOnLinks() {
    try {
      const params = new URLSearchParams(location.search);
      const ver = params.get('ver');
      if (!ver) return;
      document.querySelectorAll('a[href]').forEach(a => {
        const href = a.getAttribute('href');
        if (!href) return;
        if (href.startsWith('mailto:') || href.startsWith('tel:') || href.startsWith('javascript:')) return;
        if (href.startsWith('#')) return;
        try {
          const u = new URL(href, location.href);
          if (u.origin !== location.origin) return;
          u.searchParams.set('ver', ver);
          const isAbsolute = /^https?:\/\//i.test(href);
          a.setAttribute('href', isAbsolute ? u.href : (u.pathname + u.search + u.hash));
        } catch (e) { /* ignore */ }
      });
    } catch (e) { /* ignore */ }
  }

  try { populateVersionSelector(); } catch (e) { /* ignore */ }

  // ── Code Reference Panel ──────────────────────────────────────
  let sourceFiles = null;
  let sourceHighlights = null;
  let codeRefs = null;

  async function loadSourceFiles() {
    if (sourceFiles !== null) return sourceFiles;
    try {
      const [files, highlights] = await Promise.all([
        loadCompressedJson('source_files.zst', {}),
        loadCompressedJson('source_highlights.zst', {})
      ]);
      sourceFiles = files || {};
      sourceHighlights = highlights || {};
    } catch (e) {
      console.warn('Failed to load source files:', e);
      sourceFiles = {};
      sourceHighlights = {};
    }
    return sourceFiles;
  }

  function escapeSourceText(text) {
    return String(text || '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function highlightedSourceLine(filePath, lineNumber) {
    const lines = sourceHighlights && sourceHighlights[filePath];
    if (lines && lines[lineNumber - 1] !== undefined) return lines[lineNumber - 1];
    const content = sourceFiles && sourceFiles[filePath];
    if (!content) return '<span class="cm">(empty file)</span>';
    return escapeSourceText(content.split(/\r?\n/)[lineNumber - 1] || '');
  }

  function findFunctionRange(content, name) {
    const lines = content.split(/\r?\n/);
    const escapedName = String(name).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const patterns = [
      new RegExp(`^[^\S\n]*(?:async\\s+)?def\\s+${escapedName}\\s*\\(`),
      new RegExp(`^[^\S\n]*(?:export\\s+)?(?:async\\s+)?function\\s+${escapedName}\\s*\\(`),
      new RegExp(`^[^\S\n]*(?:export\\s+)?(?:const|let|var)\\s+${escapedName}\\s*=`)
    ];
    let definition = -1;
    for (let i = 0; i < lines.length; i++) {
      if (patterns.some(pattern => pattern.test(lines[i]))) {
        definition = i;
        break;
      }
    }
    if (definition < 0) return null;

    let start = definition;
    while (start > 0 && /^[^\S\n]*@/.test(lines[start - 1])) start--;
    let end = lines.length - 1;
    if (/^[^\S\n]*(?:async\s+)?def\s/.test(lines[definition])) {
      const indent = (lines[definition].match(/^[^\S\n]*/) || [''])[0].length;
      end = definition;
      for (let i = definition + 1; i < lines.length; i++) {
        if (lines[i].trim() && (lines[i].match(/^[^\S\n]*/) || [''])[0].length <= indent) {
          end = i - 1;
          break;
        }
      }
    } else {
      let depth = 0;
      let opened = false;
      for (let i = definition; i < lines.length; i++) {
        depth += (lines[i].match(/\{/g) || []).length;
        depth -= (lines[i].match(/\}/g) || []).length;
        opened ||= lines[i].includes('{');
        if (opened && depth <= 0) {
          end = i;
          break;
        }
      }
    }
    while (end > definition && !lines[end].trim()) end--;
    return { start: start + 1, end: end + 1 };
  }

  function referenceView(ref, content) {
    const lines = content.split(/\r?\n/);
    if (ref.function) {
      const range = findFunctionRange(content, ref.function);
      if (range) {
        return {
          start: Math.max(1, range.start - 1),
          end: Math.min(lines.length, range.end + 1),
          selectedStart: range.start,
          selectedEnd: range.end,
          range: true,
          functionName: ref.function
        };
      }
    }
    if (ref.line && ref.end_line) {
      return {
        start: Math.max(1, ref.line - 2),
        end: Math.min(lines.length, ref.end_line + 2),
        selectedStart: ref.line,
        selectedEnd: ref.end_line,
        range: true
      };
    }
    if (ref.line) {
      return {
        start: Math.max(1, ref.line - 2),
        end: Math.min(lines.length, ref.line + 2),
        selectedStart: ref.line,
        selectedEnd: ref.line,
        range: false
      };
    }
    return { start: 1, end: lines.length, selectedStart: null, selectedEnd: null, range: false };
  }

  function renderReferenceLines(ref, fileContent, fullFile = false) {
    if (ref.function && !fullFile) {
      const range = findFunctionRange(fileContent, ref.function);
      if (range) {
        let functionLines = '';
        for (let i = range.start; i <= range.end; i++) {
          const boundary = i === range.start || i === range.end;
          functionLines += `<div class="code-line${boundary ? ' highlight-target' : ''}" data-line="${i}"><span class="line-number">${i}</span>${highlightedSourceLine(ref.file, i)}</div>`;
        }
        return {
          start: range.start,
          end: range.end,
          selectedStart: range.start,
          selectedEnd: range.end,
          range: true,
          functionName: ref.function,
          highlightedLines: `<div class="code-line code-line-blank"><span class="line-number"></span>&nbsp;</div>${functionLines}<div class="code-line code-line-blank"><span class="line-number"></span>&nbsp;</div>`
        };
      }
    }
    const view = referenceView(ref, fileContent);
    if (fullFile) {
      view.start = 1;
      view.end = fileContent.split(/\r?\n/).length;
    }
    let highlightedLines = '';
    for (let i = view.start; i <= view.end; i++) {
      const selected = view.selectedStart !== null && i >= view.selectedStart && i <= view.selectedEnd;
      const boundary = selected && (i === view.selectedStart || i === view.selectedEnd || !view.range);
      const middle = view.range && selected && !boundary;
      const classes = ['code-line'];
      if (boundary) classes.push('highlight-target');
      if (middle) classes.push('range-middle');
      highlightedLines += `<div class="${classes.join(' ')}" data-line="${i}"><span class="line-number">${i}</span>${highlightedSourceLine(ref.file, i)}</div>`;
    }
    return { ...view, highlightedLines };
  }

  function parseCodeRefs() {
    if (codeRefs !== null) return codeRefs;
    const el = document.getElementById('code-refs-data');
    if (el && el.textContent.trim()) {
      try {
        codeRefs = JSON.parse(el.textContent);
      } catch (e) {
        console.warn('Failed to parse code refs:', e);
        codeRefs = [];
      }
    } else {
      codeRefs = [];
    }
    return codeRefs;
  }

  function createCodePanel(ref, fileContent) {
    const view = renderReferenceLines(ref, fileContent, true);
    const panel = document.createElement('div');
    panel.className = 'code-reference-panel';
    const location = ref.function
      ? `Function ${ref.function}`
      : ref.end_line
        ? `Lines ${ref.line}-${ref.end_line}`
        : ref.line
          ? `Line ${ref.line}`
          : 'Full file';
    panel.innerHTML = `
      <div class="code-panel-header">
        <span class="code-panel-file">${ref.file}</span>
        <span class="code-panel-line">${location}</span>
        <button class="code-panel-close" aria-label="Close">&times;</button>
      </div>
      <div class="code-panel-content">
        <div class="code-lines">${view.highlightedLines}</div>
      </div>
    `;

    // Add close handler
    panel.querySelector('.code-panel-close').addEventListener('click', () => {
      panel.remove();
      document.body.classList.remove('code-panel-open');
    });

    // Close on outside click
    panel.addEventListener('click', (e) => {
      if (e.target === panel) {
        panel.remove();
        document.body.classList.remove('code-panel-open');
      }
    });

    // Close on Escape
    const handleEscape = (e) => {
      if (e.key === 'Escape') {
        panel.remove();
        document.body.classList.remove('code-panel-open');
        document.removeEventListener('keydown', handleEscape);
      }
    };
    document.addEventListener('keydown', handleEscape);

    return panel;
  }

  function showCodePanel(ref) {
    const fileContent = sourceFiles[ref.file];
    if (!fileContent) {
      // Source files are already loaded from source_files.zst at build
      // time. If the file isn't there, it doesn't exist — don't try to
      // fetch it from the site root (that would produce a 404 like
      // /fr-docs/fr_docs/syntax.py).
      console.warn(`Source file not found in index: ${ref.file}`);
      return;
    }

    const panel = createCodePanel(ref, fileContent);
    document.body.appendChild(panel);
    document.body.classList.add('code-panel-open');
    // Scroll to target line
    requestAnimationFrame(() => {
      const targetLine = panel.querySelector('.highlight-target');
      if (targetLine) targetLine.scrollIntoView({ behavior: 'smooth', block: 'center' });
    });
  }

  function initCodeReferences() {
    if (!parseCodeRefs().length) return;

    loadSourceFiles().then(() => {
      // Use event delegation on document for code reference links
      // This handles both static and dynamically added links
      document.addEventListener('click', (e) => {
        // Handle code-reference links with data-coderef-id
        const codeRefLink = e.target.closest('a.code-reference[data-coderef-id]');
        if (codeRefLink) {
          e.preventDefault();
          const refId = codeRefLink.dataset.coderefId;
          const refs = parseCodeRefs();
          const ref = refs.find(r => r.id === refId);
          if (ref) {
            showCodePanel(ref);
          }
          return;
        }

        // Handle search result links with #coderef:file:line format
        const searchRefLink = e.target.closest('a[href^="#coderef:"]');
        if (searchRefLink) {
          console.log(`found coderef: ${searchRefLink}`);
          e.preventDefault();
          const href = searchRefLink.getAttribute('href');
          const match = href.match(/^#coderef:([^:]+):(\d+)(?:-(\d+))?$/);
          if (match) {
            const file = match[1];
            const line = parseInt(match[2], 10);
            const endLine = match[3] ? parseInt(match[3], 10) : null;
            const refs = parseCodeRefs();
            const ref = refs.find(r => r.file === file && r.line === line);
            if (ref) {
              if (endLine) ref.end_line = endLine;
              showCodePanel(ref);
            } else {
              showCodePanel({ file, line, end_line: endLine, id: 'search-result' });
            }
          }
          return;
        }
      });
    });
  }

  // Initialize code references
  initCodeReferences();

  // ── Link Hover Previews ────────────────────────────────────────
  let linkPreviewCache = new Map();
  let previewTooltip = null;
  let previewHideTimeout = null;

  function createPreviewTooltip() {
    if (previewTooltip) return previewTooltip;
    previewTooltip = document.createElement('div');
    previewTooltip.className = 'link-preview-tooltip';
    // width set outside
    previewTooltip.style.cssText = `
      position: fixed;
      z-index: 3000;
      background: var(--bg-soft);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      box-shadow: var(--shadow-md);
      padding: 12px;
      font-size: 0.85rem;
      line-height: 1.5;
      pointer-events: auto;
      opacity: 0;
      transition: opacity 0.15s ease;
    `;
    document.body.appendChild(previewTooltip);
    previewTooltip.addEventListener('mouseenter', () => {
      if (previewHideTimeout) clearTimeout(previewHideTimeout);
    });
    previewTooltip.addEventListener('mouseleave', () => {
      hidePreviewTooltip();
    });
    return previewTooltip;
  }

  async function fetchPagePreview(href) {
    const cacheKey = href.split('#')[0];
    if (linkPreviewCache.has(cacheKey)) {
      return linkPreviewCache.get(cacheKey);
    }

    try {
      const response = await fetch(toSiteHref(cacheKey), { cache: 'force-cache' });
      if (!response.ok) return null;
      const html = await response.text();
      const parser = new DOMParser();
      const doc = parser.parseFromString(html, 'text/html');

      const h1 = doc.querySelector('h1');
      const title = h1 ? h1.textContent.trim() : '';

      // Get first paragraph after h1
      let description = '';
      if (h1) {
        let next = h1.nextElementSibling;
        while (next && next.tagName !== 'H2' && next.tagName !== 'H3') {
          if (next.tagName === 'P' && next.textContent.trim()) {
            description = next.textContent.trim().substring(0, 300);
            break;
          }
          next = next.nextElementSibling;
        }
      }

      const preview = { title, description };
      linkPreviewCache.set(cacheKey, preview);
      return preview;
    } catch (e) {
      console.warn('Failed to fetch page preview:', e);
      return null;
    }
  }

  async function fetchCodePreview(filePath, line, endLine, functionName) {
    const cacheKey = `${filePath}:${line || 0}:${endLine || 0}:${functionName || ''}`;
    if (linkPreviewCache.has(cacheKey)) {
      return linkPreviewCache.get(cacheKey);
    }

    try {
      // Try to get from sourceFiles first
      await loadSourceFiles();
      let content = sourceFiles[filePath];

      if (!content) {
        const response = await fetch(toSiteHref(filePath), { cache: 'force-cache' });
        if (response.ok) {
          content = await response.text();
          sourceFiles[filePath] = content;
        }
      }

      if (!content) return null;

      const previewRef = { file: filePath, line, end_line: endLine, function: functionName };
      const view = renderReferenceLines(previewRef, content);
      const preview = {
        highlightedLines: view.highlightedLines,
        filePath,
        targetLine: line || null,
        endLine: endLine || null,
        functionName: functionName || null,
        startLine: view.start,
        end: view.end
      };
      linkPreviewCache.set(cacheKey, preview);
      return preview;
    } catch (e) {
      console.warn('Failed to fetch code preview:', e);
      return null;
    }
  }

  function showPreviewTooltip(x, y, content, isCode = false) {
    if (previewHideTimeout) clearTimeout(previewHideTimeout);
    const tooltip = createPreviewTooltip();
    tooltip.innerHTML = content;
    tooltip.style.left = `${x + 15}px`;
    tooltip.style.top = `${y + 15}px`;
    tooltip.style.opacity = '1';
    tooltip.style.minWidth = `${isCode ? 350 : 200}px`;
    tooltip.style.maxWidth = `${isCode ? 80 : 30}vw`;
  }

  function hidePreviewTooltip() {
    if (previewHideTimeout) clearTimeout(previewHideTimeout);
    previewHideTimeout = setTimeout(() => {
      if (previewTooltip && !previewTooltip.matches(':hover')) {
        previewTooltip.style.opacity = '0';
      }
    }, 200);
  }

  function initLinkPreviews() {
    if (!featureEnabled('link_preview')) return;

    const hasFinePointer = window.matchMedia('(any-pointer: fine)').matches;

    if (!hasFinePointer) {
      // No mouse available - skip hover initialization, but set up a listener to
      // re-initialize if mouse is detected later (e.g., touch-screen laptop).
      function handleFirstMouse() {
        if (document.querySelectorAll('a[href]').length === 0) {
          return;
        }
        const stillNoFine = window.matchMedia('(any-pointer: fine)').matches;
        if (!stillNoFine) {
          return;
        }
        document.removeEventListener('mousemove', handleFirstMouse);
        document.removeEventListener('mousedown', handleFirstMouse);
        initLinkPreviews();
      }
      document.addEventListener('mousemove', handleFirstMouse, { once: true });
      document.addEventListener('mousedown', handleFirstMouse, { once: true });
      return;
    }

    document.querySelectorAll('a[href]').forEach(link => {
      // Skip topbar/header navigation links - no hover preview needed
      if (link.closest('.header-nav')) return;

      const href = link.getAttribute('href');
      if (!href) return;

      // Skip external links, anchors, and special links
      if (href.startsWith('http') || href.startsWith('mailto:') || href.startsWith('tel:') || href.startsWith('javascript:')) return;

      // Check if it's a code reference link (data-coderef-id format or #coderef: format)
      const isCodeRef = link.classList.contains('code-reference');
      const isSearchCodeRef = href.startsWith('#coderef:');

      let hoverTimeout = null;
      let lastMouseX = 0;
      let lastMouseY = 0;

      link.addEventListener('mousemove', (e) => {
        lastMouseX = e.clientX;
        lastMouseY = e.clientY;
        if (previewTooltip && previewTooltip.style.opacity === '1') {
          previewTooltip.style.left = `${e.clientX + 15}px`;
          previewTooltip.style.top = `${e.clientY + 15}px`;
        }
      });

      link.addEventListener('mouseenter', async (e) => {
        lastMouseX = e.clientX;
        lastMouseY = e.clientY;
        hoverTimeout = setTimeout(async () => {
          if (isCodeRef) {
            const refId = link.dataset.coderefId;
            const refs = parseCodeRefs();
            const ref = refs.find(r => r.id === refId);
            if (ref) {
              const preview = await fetchCodePreview(ref.file, ref.line, ref.end_line, ref.function);
              if (preview) {
                const previewLabel = preview.functionName
                  ? `Function ${preview.functionName}`
                  : preview.endLine
                    ? `Lines ${preview.targetLine}-${preview.endLine}`
                    : preview.targetLine
                      ? `Line ${preview.targetLine}`
                      : 'Full file';
                const content = `
                  <div class="preview-header">
                    <span class="preview-file">${preview.filePath}</span>
                    <span class="preview-line">${previewLabel}</span>
                  </div>
                  <div class="preview-code">${preview.highlightedLines}</div>
                `;
                showPreviewTooltip(lastMouseX, lastMouseY, content, isCode = true);
              }
            }
          } else if (isSearchCodeRef) {
            // Handle search result code reference links
            const match = href.match(/^#coderef:([^:]+):(\d+)(?:-(\d+))?$/);
            if (match) {
              console.log(`opening coderef from fragment: ${match}`);
              const file = match[1];
              const line = parseInt(match[2], 10);
              const endLine = match[3] ? parseInt(match[3], 10) : null;
              const preview = await fetchCodePreview(file, line, endLine);
              if (preview) {
                const content = `
                  <div class="preview-header">
                    <span class="preview-file">${preview.filePath}</span>
                    ${preview.targetLine ? `<span class="preview-line">Line ${preview.targetLine}</span>` : ''}
                  </div>
                  <div class="preview-code">${preview.highlightedLines}</div>
                `;
                showPreviewTooltip(lastMouseX, lastMouseY, content, isCode = true);
              }
            }
          } else if (!href.startsWith('#')) {
            // Regular page link
            const preview = await fetchPagePreview(href);
            if (preview && (preview.title || preview.description)) {
              const content = `
                <div class="preview-header">${preview.title || ''}</div>
                <div class="preview-description">${preview.description || ''}</div>
              `;
              showPreviewTooltip(lastMouseX, lastMouseY, content, isCode = false);
            }
          }
        }, 300); // 300ms delay before showing preview
      });
    });
  }

  // Check if feature is enabled (from the config embedded by the builder)
  function featureEnabled(name) {
    const el = document.getElementById('features-config');
    if (!el) return true; // fallback: assume enabled if no config embedded
    try {
      const state = JSON.parse(el.textContent || '{}');
      return !!state[name];
    } catch (e) {
      return true;
    }
  }

  // Initialize link previews
  initLinkPreviews();

});
