import { QS } from './config.js';
import { Module } from './engine.js';

export { idbGet, idbPut, idbPutChecked, idbDel, clearTiles, fetchTile, initTiles };

// --- indexeddb byte cache -------------------------------------------------
// 'tiles' rocktree bytes, 'tilemeta' their size and last use, 'app' the
// page's files too big for localstorage
// never await the open: an upgrade waits on every other tab, indefinitely.
// until it lands db is null, every helper fails its try/catch, and tiles
// come off the network
let db = null;
// where the origin's storage stands, for a warning that may be about quota
const warnUsage = () => {
  if (navigator.storage && navigator.storage.estimate)
    navigator.storage.estimate().then((e) => console.warn(
      `idb: origin is using ${Math.round((e.usage || 0) / 1e6)} MB`
      + ` of ${Math.round((e.quota || 0) / 1e6)} MB`));
};
const dbOpen = new Promise((res) => {
  // a blocked upgrade or wedged storage never settles, silently
  let settled = false;
  setTimeout(() => {
    if (settled) return;
    console.warn('idb: open has been pending for 8s — no cache, and nothing'
      + ' the page stores will persist. if this sticks, clear this site\'s'
      + ' storage (the origin may be out of quota) and reload');
    warnUsage();
  }, 8000);
  const tOpen = performance.now();
  try {
    const r = indexedDB.open('earth-cache', 3);
    r.onupgradeneeded = (e) => {
      const d = r.result;
      if (!d.objectStoreNames.contains('tiles')) d.createObjectStore('tiles');
      if (!d.objectStoreNames.contains('app')) d.createObjectStore('app');
      if (!d.objectStoreNames.contains('tilemeta')) d.createObjectStore('tilemeta');
      // older tiles have no size and must go, but not here: clearing
      // gigabytes in a versionchange transaction holds open() for minutes.
      // purgeUnbookkept does it in the background
    };
    r.onblocked = () => console.warn('idb: the upgrade is blocked by another'
      + ' tab with this page open — close it. running without the cache meanwhile');
    r.onsuccess = () => {
      settled = true;
      const d = r.result;
      console.log(`idb: open took ${Math.round(performance.now() - tOpen)} ms`);
      // let another tab's upgrade through
      d.onversionchange = () => { db = null; d.close(); };
      db = d;
      res(d);
    };
    r.onerror = () => {
      settled = true;
      console.warn('idb: open failed', r.error);
      res(null);
    };
  } catch (e) { settled = true; console.warn('idb: open threw', e); res(null); }
});
// profiling: ?idb=0 no tile cache, ?idb=ro lookups but no writes
const IDB_OFF = QS.get('idb') === '0';
const IDB_RO = QS.get('idb') === 'ro';
const idbGet = (key, store = 'tiles') => new Promise((res) => {
  if (IDB_OFF) { res(undefined); return; }
  try {
    const t = db.transaction(store).objectStore(store).get(key);
    t.onsuccess = () => res(t.result);
    t.onerror = () => res(undefined);
  } catch (e) { res(undefined); }
});
const idbPut = (key, bytes, store = 'tiles') => {
  try { db.transaction(store, 'readwrite').objectStore(store).put(bytes, key); } catch (e) {}
};
// for the files the page restores from: a silently failed put leaves the
// previous file, and the reload looks like it ignored you. the failures are
// events, not throws, so try/catch alone misses them
const idbPutChecked = (key, value, store = 'app') => new Promise((res) => {
  const fail = (why, err) => {
    console.warn(`idb: ${why} storing '${key}'`, err || '');
    warnUsage();
    res(false);
  };
  if (!db) {
    console.warn(`idb: no database open, '${key}' not saved`);
    res(false);
    return;
  }
  try {
    const tx = db.transaction(store, 'readwrite');
    const rq = tx.objectStore(store).put(value, key);
    rq.onerror = () => fail('put failed', rq.error);
    tx.onabort = () => fail('transaction aborted', tx.error);
    tx.oncomplete = () => res(true);
  } catch (e) { fail('put threw', e); }
});
const idbDel = (key, store = 'tiles') => {
  try { db.transaction(store, 'readwrite').objectStore(store).delete(key); } catch (e) {}
};

// --- tile cache lru -------------------------------------------------------
// unbounded, the cache fills the quota and the gpx and photo writes in this
// same database start failing. sizes are kept per tile in memory and in
// 'tilemeta', since indexeddb can't report them without reading the values.
// a tile averages ~4 KB, so the 512 MiB floor is ~100k of them
const TILE_BUDGET_SHARE = 0.25;
const TILE_BUDGET_MIN = 512 * 1024 * 1024;
const TILE_BUDGET_MAX = 4 * 1024 * 1024 * 1024;
let tileBudget = TILE_BUDGET_MIN; // until the estimate comes back
// never evict against the provisional floor: that evicts real tiles in a
// slow readwrite transaction every lookup queues behind
let budgetKnown = false;
const TILE_LOW_WATER = 0.85;   // an eviction pass clears down to this much
const TILE_TOUCH_MS = 300000;  // rewrite a tile's last-use at most this often
const tileMeta = new Map();    // path -> { n: bytes, t: last use, w: last written }
let tileBytes = 0, evicting = false, quotaWarned = false, purging = false;

dbOpen.then((d) => {
  if (!d) return;
  try {
    const tx = d.transaction('tilemeta');
    const st = tx.objectStore('tilemeta');
    const keys = st.getAllKeys(), vals = st.getAll();
    tx.oncomplete = () => {
      const k = keys.result || [], v = vals.result || [];
      for (let i = 0; i < k.length; i++) {
        if (!v[i] || !(v[i].n > 0)) continue;
        tileMeta.set(k[i], { n: v[i].n, t: v[i].t || 0, w: v[i].t || 0 });
        tileBytes += v[i].n;
      }
      try {
        const cq = d.transaction('tiles').objectStore('tiles').count();
        cq.onsuccess = () => console.log(`tile cache: ${cq.result} tiles in the store,`
          + ` ${tileMeta.size} of them accounted for (${Math.round(tileBytes / 1e6)} MB)`);
      } catch (e) {}
      // no sizeTileBudget() until the purge is done: evicting during it
      // drops the only tiles the bookkeeping can see
      purgeUnbookkept();
    };
  } catch (e) {}
});

// `hard` is the quota-error path: the accounting says there is room but the
// browser disagrees, and the browser is the one that counts
const evictTiles = async (hard) => {
  if (evicting || !db || (!hard && !budgetKnown)) return;
  evicting = true;
  try {
    let target = tileBudget * TILE_LOW_WATER;
    if (hard) target = Math.min(target, tileBytes * 0.7);
    if (tileBytes <= target) return;
    const oldest = [...tileMeta.entries()].sort((a, b) => a[1].t - b[1].t);
    const doomed = [];
    let freed = 0;
    for (const [path, m] of oldest) {
      if (tileBytes - freed <= target) break;
      doomed.push(path);
      freed += m.n;
    }
    if (!doomed.length) return;
    await new Promise((res) => {
      try {
        const tx = db.transaction(['tiles', 'tilemeta'], 'readwrite');
        const tiles = tx.objectStore('tiles'), meta = tx.objectStore('tilemeta');
        for (const p of doomed) { tiles.delete(p); meta.delete(p); }
        tx.oncomplete = tx.onabort = () => res();
      } catch (e) { res(); }
    });
    for (const p of doomed) tileMeta.delete(p);
    tileBytes -= freed;
    console.log(`tile cache: evicted ${doomed.length} tiles (${Math.round(freed / 1e6)} MB),`
      + ` now ${Math.round(tileBytes / 1e6)} MB`);
  } finally { evicting = false; }
};

// the 'tilemeta' write is rate-limited: one per hit costs more than the
// eviction order it buys
const noteTileUse = (path, n) => {
  const now = Date.now();
  const m = tileMeta.get(path);
  if (m) {
    if (m.n !== n) { tileBytes += n - m.n; m.n = n; }
    m.t = now;
    if (now - m.w < TILE_TOUCH_MS) return;
    m.w = now;
  } else {
    tileMeta.set(path, { n, t: now, w: now });
    tileBytes += n;
  }
  metaQueue.set(path, { n, t: now });
  scheduleFlush();
};

// tiles with no meta record, from before the budget. a tile cached mid-purge
// can go before its meta lands, which costs one re-download
const purgeUnbookkept = async () => {
  if (!db) return;
  const CHUNK = 2000;
  let after = null, gone = 0, said = false;
  purging = true;
  for (;;) {
    const keys = await new Promise((res) => {
      try {
        // undefined, never null: null is an invalid key range and throws
        // DataError, which a swallowed catch turns into "nothing to purge"
        const range = after === null ? undefined : IDBKeyRange.lowerBound(after, true);
        const rq = db.transaction('tiles').objectStore('tiles').getAllKeys(range, CHUNK);
        rq.onsuccess = () => res(rq.result || []);
        rq.onerror = () => { console.warn('tile purge: read failed', rq.error); res([]); };
      } catch (e) { console.warn('tile purge: read threw', e); res([]); }
    });
    if (!keys.length) break;
    after = keys[keys.length - 1];
    const doomed = keys.filter((k) => !tileMeta.has(k));
    if (doomed.length) {
      await new Promise((res) => {
        try {
          const tx = db.transaction('tiles', 'readwrite');
          const st = tx.objectStore('tiles');
          for (const k of doomed) st.delete(k);
          tx.oncomplete = () => res();
          tx.onabort = () => { console.warn('tile purge: delete aborted', tx.error); res(); };
        } catch (e) { console.warn('tile purge: delete threw', e); res(); }
      });
      gone += doomed.length;
      if (!said) {
        said = true;
        console.log('tile cache: clearing tiles left over from before the budget…');
      } else if (gone % 20000 < CHUNK) {
        console.log(`tile cache: ${gone} leftover tiles cleared so far…`);
      }
    }
    await new Promise((r) => setTimeout(r, 30)); // let the frame loop breathe
  }
  purging = false;
  if (gone) console.log(`tile cache: cleared ${gone} leftover tiles`);
  sizeTileBudget();
};

// --- batched tile i/o -----------------------------------------------------
// a lookup queues behind every write transaction created before it. one
// write per tile took the toronto horizon view (tools/loadprof.mjs) to
// 60–90 s to settle, against 8 s uncached. so writes commit in batches with
// relaxed durability, and a frame's lookups share one readonly transaction
const FLUSH_MS = 300;
let tileQueue = new Map(), metaQueue = new Map(), flushTimer = null;
const scheduleFlush = () => {
  if (!flushTimer) flushTimer = setTimeout(flushWrites, FLUSH_MS);
};
const flushWrites = () => {
  flushTimer = null;
  if (!tileQueue.size && !metaQueue.size) return;
  if (!db) {
    if (tileQueue.size > 256) tileQueue.clear();
    scheduleFlush();
    return;
  }
  const tiles = tileQueue, metas = metaQueue;
  tileQueue = new Map(); metaQueue = new Map();
  try {
    const tx = db.transaction(['tiles', 'tilemeta'], 'readwrite', { durability: 'relaxed' });
    const ts = tx.objectStore('tiles'), ms = tx.objectStore('tilemeta');
    for (const [path, bytes] of tiles) ts.put(bytes, path);
    for (const [path, rec] of metas) ms.put(rec, path);
    tx.oncomplete = () => {
      for (const [path, bytes] of tiles) noteTileUse(path, bytes.length);
      if (tileBytes > tileBudget) evictTiles(false);
    };
    tx.onabort = () => {
      if (!quotaWarned) {
        quotaWarned = true;
        console.warn('tile write failed (quota?)', tx.error || '');
      }
      // mid-purge the room is coming anyway, and evicting would drop the
      // only tiles the bookkeeping can see
      if (!purging) evictTiles(true);
    };
  } catch (e) {}
};
const putTile = (path, bytes) => {
  if (IDB_OFF || IDB_RO) return;
  tileQueue.set(path, bytes);
  scheduleFlush();
};
// one readonly transaction per task's lookups, with a deadline: firefox's
// first lookup can take a minute (it initialises a multi-GB origin), and the
// root bulk is in it. past LOOKUP_MS the batch goes to the network and the
// cache is bypassed until it answers. loose on purpose: lookups of a few
// seconds happen while a write batch commits
const LOOKUP_MS = 3000;
let readQueue = [], stalled = 0;
const getTile = (path) => new Promise((res) => {
  if (IDB_OFF || stalled) { res(undefined); return; }
  // downloaded but not yet committed: no need to ask the store
  const queued = tileQueue.get(path);
  if (queued) { res(queued); return; }
  if (!readQueue.length) queueMicrotask(flushReads);
  readQueue.push({ path, res });
});
const flushReads = () => {
  const batch = readQueue;
  readQueue = [];
  let open = batch.length;
  const timer = setTimeout(() => {
    if (!open) return;
    stalled++;
    console.warn(`tile cache: a lookup of ${batch.length} tiles has taken`
      + ` ${LOOKUP_MS} ms; going to the network until the cache answers`);
    for (const r of batch) if (r.res) { r.res(undefined); r.res = null; }
  }, LOOKUP_MS);
  const settle = (r, v) => {
    if (r.res) { r.res(v); r.res = null; }
    if (--open) return;
    clearTimeout(timer);
    if (stalled && r.late) { stalled--; console.log('tile cache: the cache answered; back on it'); }
  };
  try {
    const st = db.transaction('tiles').objectStore('tiles');
    for (const r of batch) {
      const rq = st.get(r.path);
      rq.onsuccess = () => { r.late = !r.res; settle(r, rq.result); };
      rq.onerror = () => { r.late = !r.res; settle(r, undefined); };
    }
  } catch (e) { clearTimeout(timer); for (const r of batch) settle(r, undefined); }
};

const clearTiles = () => new Promise((res) => {
  if (!db) { res(false); return; }
  try {
    const tx = db.transaction(['tiles', 'tilemeta'], 'readwrite');
    tx.objectStore('tiles').clear();
    tx.objectStore('tilemeta').clear();
    tx.oncomplete = () => {
      tileMeta.clear();
      tileBytes = 0;
      console.log('tile cache cleared; tiles will re-download as you fly');
      res(true);
    };
    tx.onabort = () => { console.warn('tile cache clear aborted', tx.error); res(false); };
  } catch (e) { console.warn('tile cache clear threw', e); res(false); }
});

// ignores the estimate's usage on purpose: firefox pools it across every
// localhost port, so it counts other projects' storage
function sizeTileBudget() {
  if (!navigator.storage || !navigator.storage.estimate) return;
  navigator.storage.estimate().then((e) => {
    if (!e.quota) return;
    tileBudget = Math.min(TILE_BUDGET_MAX,
      Math.max(TILE_BUDGET_MIN, e.quota * TILE_BUDGET_SHARE));
    budgetKnown = true;
    const mb = (b) => Math.round(b / 1e6) + ' MB';
    console.log(`tile cache: ${mb(tileBytes)} in ${tileMeta.size} tiles,`
      + ` budget ${mb(tileBudget)} of a ${mb(e.quota)} quota`
      + ` (${mb(e.usage || 0)} used across everything stored under this host)`);
    if ((e.usage || 0) > e.quota * 0.9)
      console.warn('idb: this host is near its storage limit, so writes may fail —'
        + ' and by the numbers above, most of that is not this page');
    if (tileBytes > tileBudget) evictTiles(false);
  });
}

// --- the fetch hook -------------------------------------------------------
// planetoid metadata ('P...') is not cached, matching the native client
const stats = window.__stats = {
  issued: 0, delivered: 0, failed: 0, inflight: 0,
  idbHits: 0, netHits: 0, maxMs: 0, log: [],
  // per kind, cache and network waits apart: a bulk gates a whole subtree,
  // so a slow deep zoom means asking which queue the bulk is in
  kinds: {},
  // ?fetchtrace: per request, when issued, cached, fetched, delivered
  trace: QS.has('fetchtrace') ? [] : null,
};
// 'B' bulk metadata, 'N' node data, 'P' the planetoid header
const statKind = (path) => {
  const k = stats.kinds[path[0]] || (stats.kinds[path[0]] = {
    n: 0, idb: 0, net: 0, fail: 0, inflight: 0, idbMs: 0, netMs: 0, maxMs: 0 });
  return k;
};
const fetchTile = async (path, id) => {
  const t0 = performance.now();
  stats.issued++;
  stats.inflight++;
  const k = statKind(path);
  k.n++;
  k.inflight++;
  const tr = stats.trace ? { p: path, t0, idb: 0, net: 0, done: 0, n: 0 } : null;
  if (tr) stats.trace.push(tr);
  try {
    Module.noteFetchPhase(id, 1); // cache lookup
    let bytes = path[0] !== 'P' ? await getTile(path) : undefined;
    k.idbMs += performance.now() - t0;
    if (tr) tr.idb = performance.now();
    // also re-registers a tile whose meta record went missing
    if (bytes) { stats.idbHits++; k.idb++; noteTileUse(path, bytes.length); }
    if (!bytes) {
      const t1 = performance.now();
      Module.noteFetchPhase(id, 2); // network
      const resp = await fetch('https://kh.google.com/rt/earth/' + path);
      if (!resp.ok) throw new Error('http ' + resp.status);
      bytes = new Uint8Array(await resp.arrayBuffer());
      k.netMs += performance.now() - t1;
      if (tr) tr.net = performance.now();
      stats.netHits++;
      k.net++;
      if (path[0] !== 'P') putTile(path, bytes);
    }
    stats.delivered++;
    if (tr) { tr.n = bytes.length; tr.done = performance.now(); }
    Module.deliverFetch(id, true, bytes);
  } catch (e) {
    stats.failed++;
    k.fail++;
    stats.log.push(path + ' -> ' + e.message);
    Module.deliverFetch(id, false, null);
  } finally {
    stats.inflight--;
    k.inflight--;
    const ms = performance.now() - t0;
    stats.maxMs = Math.max(stats.maxMs, ms);
    k.maxMs = Math.max(k.maxMs, ms);
  }
};
const initTiles = () => {
  // pairs with the engine's own 2s line; ?fetchlog=0 turns it off
  if (QS.get('fetchlog') !== '0') setInterval(() => {
    const parts = [];
    for (const [kind, k] of Object.entries(stats.kinds)) {
      if (!k.n) continue;
      const avg = (ms, n) => n ? Math.round(ms / n) : 0;
      parts.push(`${kind}: ${k.n} (${k.idb} cached, ${k.net} net, ${k.fail} failed)`
        + ` idb ${avg(k.idbMs, k.n)}ms net ${avg(k.netMs, k.net)}ms`
        + ` max ${Math.round(k.maxMs)}ms inflight ${k.inflight}`);
      k.maxMs = 0; // a running max says nothing after the first stall
    }
    if (parts.length) console.log('fetch · ' + parts.join(' · '));
  }, 2000);
};
