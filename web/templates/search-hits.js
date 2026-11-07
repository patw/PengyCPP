// ── Search hits ───────────────────────────────────────────────
// A sidebar search result links here as #hits=3,7&terms=["nginx"]: the hit
// messages' indexes in chat.messages (the msg-N ids) and the words to mark.
// Marks the words in those messages and scrolls to the first one.

function markTerms(root, terms) {
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  const nodes = [];
  while (walker.nextNode()) nodes.push(walker.currentNode);
  for (const node of nodes) {
    const text = node.nodeValue;
    const low = text.toLowerCase();
    if (low.length !== text.length) continue;  // positions would not line up
    const ranges = [];
    for (const term of terms) {
      for (let i = low.indexOf(term); i >= 0; i = low.indexOf(term, i + term.length)) {
        ranges.push([i, i + term.length]);
      }
    }
    if (!ranges.length) continue;
    ranges.sort((a, b) => a[0] - b[0]);
    const frag = document.createDocumentFragment();
    let pos = 0;
    for (const [start, end] of ranges) {
      if (start < pos) continue;  // overlaps the previous word
      frag.append(text.slice(pos, start));
      const mark = document.createElement('mark');
      mark.className = 'search-hit';
      mark.textContent = text.slice(start, end);
      frag.append(mark);
      pos = end;
    }
    frag.append(text.slice(pos));
    node.replaceWith(frag);
  }
}

function revealSearchHits() {
  const params = new URLSearchParams(location.hash.slice(1));
  if (!params.has('hits')) return false;
  let terms = [];
  try { terms = JSON.parse(params.get('terms') || '[]'); } catch (e) { /* none */ }
  terms = terms.filter(t => typeof t === 'string' && t);
  document.querySelectorAll('mark.search-hit').forEach(m => {
    const parent = m.parentNode;
    m.replaceWith(m.textContent);
    parent.normalize();
  });
  const hits = params.get('hits').split(',')
    .map(n => document.getElementById('msg-' + n)).filter(Boolean);
  // Drop the hash so a reload goes back to following the chat.
  history.replaceState(null, '', location.pathname + location.search);
  if (!hits.length) return false;
  // A result for this same chat only changes the hash, so the phone sidebar
  // would otherwise stay open over the hit.
  const offcanvas = document.getElementById('sidebarOffcanvas');
  if (offcanvas) bootstrap.Offcanvas.getInstance(offcanvas)?.hide();
  for (const el of hits) markTerms(el, terms);
  hits[0].scrollIntoView({block: 'start'});
  if (typeof updateJumpBtn === 'function') updateJumpBtn();
  return true;
}
