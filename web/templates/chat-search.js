// ── Chat search ──────────────────────────────────────────────
// Typing filters the sidebar by title (instant, client-side); Enter asks
// the server to search message text too and shows the results in place of
// the list until the box is cleared. Rules match pengy/core/chat_search.py.
(function () {
  const input = document.getElementById('chatSearch');
  if (!input) return;
  const resultsBox = document.getElementById('searchResults');
  const resultList = document.getElementById('searchResultList');
  const statusEl = document.getElementById('searchStatus');
  const chatList = document.getElementById('chatList');
  let controller = null;

  function parseQuery(q) {
    const terms = [];
    for (const m of q.matchAll(/"([^"]*)"|(\S+)/g)) {
      const t = (m[1] !== undefined ? m[1] : m[2]).trim().replace(/\s+/g, ' ').toLowerCase();
      if (t && !terms.includes(t)) terms.push(t);
    }
    return terms;
  }
  function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
      .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }
  // Spans are [start, end) in code points (Python str indexing), not
  // UTF-16 units, so an emoji before a match doesn't shift the bolding.
  function highlight(text, spans) {
    const chars = Array.from(text);
    const part = (a, b) => esc(chars.slice(a, b).join(''));
    let out = '', pos = 0;
    for (const [start, end] of spans) {
      if (start < pos) continue;
      out += part(pos, start) + '<b>' + part(start, end) + '</b>';
      pos = end;
    }
    return out + part(pos);
  }
  // Code-point spans of each word's first occurrence in a title.
  function titleSpans(title, terms) {
    const low = title.toLowerCase();
    if (Array.from(low).length !== Array.from(title).length) return [];
    const cp = i => Array.from(low.slice(0, i)).length;
    return terms.map(t => [low.indexOf(t), t])
      .filter(([i]) => i >= 0)
      .map(([i, t]) => [cp(i), cp(i) + Array.from(t).length])
      .sort((a, b) => a[0] - b[0]);
  }
  function showList() {
    if (controller) { controller.abort(); controller = null; }
    resultsBox.hidden = true;
    chatList.hidden = false;
  }
  function filterTitles() {
    const terms = parseQuery(input.value);
    for (const row of chatList.querySelectorAll('.chat-list-item')) {
      const title = (row.querySelector('.chat-title')?.textContent || '').toLowerCase();
      row.hidden = !terms.every(t => title.includes(t));
    }
  }
  async function runSearch() {
    const terms = parseQuery(input.value);
    if (!terms.length) return;
    if (controller) controller.abort();
    controller = new AbortController();
    resultList.innerHTML = '';
    statusEl.textContent = 'Searching…';
    chatList.hidden = true;
    resultsBox.hidden = false;
    try {
      const r = await fetch('/search?q=' + encodeURIComponent(input.value),
                            {signal: controller.signal});
      const data = await r.json();
      for (const res of data.results) {
        const a = document.createElement('a');
        a.className = 'search-result';
        a.href = '/chat/' + encodeURIComponent(res.id);
        if (res.hits.length) {
          // Jump to (and mark) the hit messages; see revealSearchHits in chat.html.
          a.href += '#hits=' + res.hits.map(h => h.index).join(',')
                  + '&terms=' + encodeURIComponent(JSON.stringify(terms));
        }
        const title = res.title || 'Untitled';
        let html = '<div class="result-title">' + highlight(title, titleSpans(title, terms)) + '</div>';
        for (const hit of res.hits) {
          html += '<div class="result-hit">' + (hit.role === 'user' ? 'You: ' : 'AI: ')
                + highlight(hit.snippet, hit.spans) + '</div>';
        }
        a.innerHTML = html;
        resultList.appendChild(a);
      }
      const n = data.results.length;
      statusEl.textContent = data.truncated
        ? `First ${n} chats shown. Add words to narrow the search. Esc to go back.`
        : (n ? `${n} chat${n === 1 ? '' : 's'} found. Esc to go back.` : 'No chats match. Esc to go back.');
    } catch (err) {
      if (err.name !== 'AbortError') statusEl.textContent = 'Search failed: ' + err;
    }
  }

  input.addEventListener('input', () => { showList(); filterTitles(); });
  input.addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); runSearch(); }
    else if (e.key === 'Escape') { input.value = ''; showList(); filterTitles(); }
  });
  document.addEventListener('keydown', e => {
    if ((e.ctrlKey || e.metaKey) && (e.key === 'k' || e.key === 'K' || (e.shiftKey && (e.key === 'F' || e.key === 'f')))) {
      e.preventDefault();
      const offcanvas = document.getElementById('sidebarOffcanvas');
      if (window.innerWidth < 768 && offcanvas) bootstrap.Offcanvas.getOrCreateInstance(offcanvas).show();
      input.focus();
      input.select();
    }
  });
})();
