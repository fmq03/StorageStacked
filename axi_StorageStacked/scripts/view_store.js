// Local script chunks avoid file:// fetch restrictions. Keep at most eight chunks.
window.TraceStore = (() => {
  const cache = new Map(), pending = new Map();
  function receive(key, rows) {
    cache.delete(key); cache.set(key, rows);
    while (cache.size > 8) cache.delete(cache.keys().next().value);
  }
  async function load(key) {
    if (cache.has(key)) {
      const rows = cache.get(key); cache.delete(key); cache.set(key, rows); return rows;
    }
    if (pending.has(key)) return pending.get(key);
    const promise = new Promise((resolve, reject) => {
      const script = document.createElement('script'); script.src = key;
      script.onload = () => { script.remove(); pending.delete(key);
        if (cache.has(key)) resolve(cache.get(key)); else reject(new Error('数据块未注册：' + key)); };
      script.onerror = () => { script.remove(); pending.delete(key); reject(new Error('无法加载 ' + key + '，请保留 HTML 旁的数据目录。')); };
      document.head.append(script);
    });
    pending.set(key, promise); return promise;
  }
  // Cursor pagination visits only matching chunks and renders at most 100 rows.
  function pager(manifest, host, render, count = 100) {
    const bar = document.createElement('div');
    const prev = document.createElement('button'), next = document.createElement('button'), status = document.createElement('span');
    prev.textContent = '上一页'; next.textContent = '下一页'; bar.append(prev, next, status); (host.closest('.scroll') || host).before(bar);
    let history = [[0, 0]], page = 0, predicate = () => true, eligible = () => true, generation = 0, following = null;
    async function draw() {
      const token = ++generation; prev.disabled = next.disabled = true; status.textContent = '加载中…';
      let [ci, ri] = history[page], rows = [], cursor = null;
      try {
        for (; ci < manifest.length; ci++, ri = 0) {
          if (!eligible(manifest[ci])) continue;
          const data = await load(manifest[ci].file);
          if (token !== generation) return;
          for (; ri < data.length; ri++) if (predicate(data[ri])) {
            if (rows.length === count) { cursor = [ci, ri]; break; }
            rows.push(data[ri]);
          }
          if (cursor) break;
        }
        if (token !== generation) return;
        following = cursor; render(rows);
        status.textContent = `第 ${page + 1} 页 · 本页 ${rows.length} 条${cursor ? '' : ' · 已到末尾'}`;
        prev.disabled = page === 0; next.disabled = !cursor;
      } catch (error) { if (token === generation) { status.textContent = error.message; host.replaceChildren(); } }
    }
    prev.onclick = () => { if (page) { page--; draw(); } };
    next.onclick = () => { if (following) { history[++page] = following; draw(); } };
    return {reset(test, skip) { predicate = test; eligible = skip; history = [[0, 0]]; page = 0; return draw(); }};
  }
  return {receive, load, pager, cacheSize: () => cache.size};
})();
