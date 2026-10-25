import { QS } from './config.js';
import { Module } from './engine.js';

export { idbGet, idbPut, idbPutChecked, idbDel, clearTiles, fetchTile, initTiles };

// --- indexeddb byte cache -------------------------------------------------
// 'tiles' holds downloaded rocktree bytes, 'tilemeta' their size and last
// use (see the lru below), and 'app' holds page state too big for
// localstorage (the reference photo's blob, the dropped gpx)
// nothing here is allowed to hold up the page. opening this can take
// arbitrarily long — a version upgrade waits for every other tab still
// holding the old connection, and the one below has several gigabytes to
// clear — and the whole app used to sit behind that await, which shows up
// as two blank panes and an empty console. so the connection arrives when
// it arrives: until then every helper below sees a null db, fails its
// try/catch, and the page runs straight off the network
let db = null;
// where the origin's storage stands, for a warning that may be about quota
const warnUsage = () => {
  if (navigator.storage && navigator.storage.estimate)
    navigator.storage.estimate().then((e) => console.warn(
      `idb: origin is using ${Math.round((e.usage || 0) / 1e6)} MB`
      + ` of ${Math.round((e.quota || 0) / 1e6)} MB`));
};
const dbOpen = new Promise((res) => {
  // an open that neither succeeds nor fails is a real state (a blocked
  // upgrade, an origin whose storage is wedged), and without this a silent
  // one. the page runs regardless, so this is a diagnosis, not a bail
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
      // tiles from before this version have no bookkeeping and no knowable
      // size, so the budget can't see them and they have to go. that does
      // NOT happen here: clearing several gigabytes inside a versionchange
      // transaction holds the whole open() until it finishes, which on a
      // full cache is minutes of a page that cannot start and says nothing.
      // purgeUnbookkept below does it in the background instead
    };
    // another tab still has the old version open: it has to let go before
    // this upgrade can run, and until it does nothing happens at all
    r.onblocked = () => console.warn('idb: the upgrade is blocked by another'
      + ' tab with this page open — close it. running without the cache meanwhile');
    r.onsuccess = () => {
      settled = true;
      const d = r.result;
      console.log(`idb: open took ${Math.round(performance.now() - tOpen)} ms`);
      // and be the tab that lets go, next time it is someone else's upgrade
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
// ?idb=0: run without the tile cache at all (profiling — what the cache
// itself costs against the network)
const IDB_OFF = QS.get('idb') === '0';
// ?idb=ro: look tiles up but never write them (profiling — how much of the
// cache's cost is the writes queueing in front of the reads)
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
// the same write, but reporting whether it actually landed. the fire-and-
// forget version above is right for tiles (a lost tile is re-fetched next
// frame and nobody is worse off) and wrong for the files the page restores
// itself from: a put that fails silently leaves the *previous* file in the
// store, so the next reload comes back to it and the page looks like it
// ignored you rather than like it hit an error. note the try/catch alone
// can't see this — a rejected request and an aborted transaction are
// events, not throws. quota is the usual reason, and the tile cache in
// this same origin is what eats it, so failures say where storage stands
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
// an unbounded tile cache eventually eats the origin's whole quota (firefox
// caps a group at 10 GiB), and then *every* write fails — including the
// dropped gpx and the reference photo, which share this database. that
// shows up as the page quietly restoring the file before last, which is a
// miserable thing to debug. so the tiles get a byte budget and the oldest
// go first.
//
// the bookkeeping is one small record per tile rather than a scan: sizes
// can't be read back from indexeddb without reading the values themselves,
// which is the thing being avoided. it is held in memory for the session
// and mirrored to 'tilemeta' so the next session starts knowing the size of
// the cache rather than having to measure it.
// a quarter of what the browser offers, held between 512 MiB and 4 GiB.
// measured rather than guessed: a tile averages about 4 KB, so even the
// floor is a hundred thousand of them, and past the ceiling the extra hits
// are places you flew over months ago. what it does NOT do is reason about
// how full the storage already is — see sizeTileBudget
const TILE_BUDGET_SHARE = 0.25;
const TILE_BUDGET_MIN = 512 * 1024 * 1024;
const TILE_BUDGET_MAX = 4 * 1024 * 1024 * 1024;
let tileBudget = TILE_BUDGET_MIN; // until the estimate comes back
// no eviction against the provisional floor: a session used to start by
// evicting a hundred mb because the store held more than the floor and
// the real budget (five times it) hadn't been read yet — a slow readwrite
// transaction that every lookup then queued behind
let budgetKnown = false;
const TILE_LOW_WATER = 0.85;   // an eviction pass clears down to this much
const TILE_TOUCH_MS = 300000;  // rewrite a tile's last-use at most this often
const tileMeta = new Map();    // path -> { n: bytes, t: last use, w: last written }
let tileBytes = 0, evicting = false, quotaWarned = false, purging = false;

// pick up where the last session left off, so the budget spans sessions
// instead of resetting to zero on every reload. deliberately not awaited:
// until it lands the cache simply looks empty, which costs an eviction pass
// that runs a little late, not correctness
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
      // what the store holds against what the bookkeeping knows about: the
      // gap is the leftovers, and seeing it stated is the difference
      // between "there was nothing to purge" and "the purge did nothing"
      try {
        const cq = d.transaction('tiles').objectStore('tiles').count();
        cq.onsuccess = () => console.log(`tile cache: ${cq.result} tiles in the store,`
          + ` ${tileMeta.size} of them accounted for (${Math.round(tileBytes / 1e6)} MB)`);
      } catch (e) {}
      // deliberately no sizeTileBudget() here: until the purge below has
      // run, the origin's usage is mostly tiles this bookkeeping cannot see,
      // and subtracting them as "someone else's" collapses the budget to
      // its floor. the provisional value stands until the truth is known
      purgeUnbookkept();
    };
  } catch (e) {}
});

// oldest-first until the cache is back under the low-water mark. one sort
// per pass, and a pass only comes round again once the cache has grown all
// the way back to the budget, so the cost is amortised into nothing.
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

// a tile was just read or written: it is the most recently used one now.
// the mirror write is rate-limited — an exact last-use is worth nothing
// here, and one small write per cache hit would cost more than the eviction
// order it buys
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

// delete the tiles the bookkeeping doesn't know about — everything written
// before the budget existed. a key cursor in chunks, with a gap between
// transactions, so several gigabytes go away without the page noticing;
// interrupting it just means the next session picks up where this stopped.
// a tile cached *during* the purge is spared as soon as its meta record
// lands, and in the sliver before that it can be deleted again — which
// costs one re-download and nothing else
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
      // this can run for minutes on a cache that filled the origin, and a
      // silent minutes-long background job is indistinguishable from a
      // broken one, so it says what it is doing
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
  // only now is the origin's usage a fair description of what is actually
  // there, so this is where the budget can be sized honestly
  sizeTileBudget();
};

// --- batched tile i/o -----------------------------------------------------
// indexeddb runs transactions on a store in the order they were created,
// and a lookup can't start while a write ahead of it holds the store. one
// readwrite transaction per downloaded tile (and another for its meta
// record) put thousands of them in front of the lookups a settling view
// issues, and the queue grew faster than it drained: lookups that took
// 100 ms at the start took two seconds a minute in, and every one of the
// engine's download slots was spent waiting in it. measured on the toronto
// horizon view (tools/loadprof.mjs): 60–90 s to settle with the cache,
// 8 s without it. so writes are collected and committed a few hundred ms
// at a time in one transaction, with relaxed durability (no fsync per
// commit — a lost tile re-downloads), and the lookups a frame issues go
// out together in one readonly transaction
const FLUSH_MS = 300;
let tileQueue = new Map(), metaQueue = new Map(), flushTimer = null;
const scheduleFlush = () => {
  if (!flushTimer) flushTimer = setTimeout(flushWrites, FLUSH_MS);
};
const flushWrites = () => {
  flushTimer = null;
  if (!tileQueue.size && !metaQueue.size) return;
  if (!db) {
    // no connection yet (or none at all): keep the metas, they're small,
    // but don't let the tiles pile up forever
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
      // while the purge is running, the room is already on its way: evicting
      // here would throw away the newly cached tiles — the only ones the
      // bookkeeping can see — to make space the purge is about to free
      if (!purging) evictTiles(true);
    };
  } catch (e) {}
};
// the tile write, with the accounting and the quota escape hatch attached
const putTile = (path, bytes) => {
  if (IDB_OFF || IDB_RO) return;
  tileQueue.set(path, bytes);
  scheduleFlush();
};
// every lookup issued in the same task — the engine schedules a frame's
// downloads in one go — is answered from one readonly transaction.
//
// with a deadline. a lookup that fails falls through to the network, but
// one that simply doesn't answer used to hold the engine's request for as
// long as it took — and the first one of a session took a minute in
// firefox, which initialises a multi-gigabyte origin before the first
// transaction runs. the root bulk was in that lookup, and nothing draws
// without it. so a batch that hasn't answered in LOOKUP_MS is given up on
// (its tiles come from the network; a late answer is dropped) and the
// cache is bypassed until that batch does answer. the deadline is loose
// on purpose: firefox lookups of a few seconds happen while a write batch
// commits, and giving up on those re-downloads tiles that are in the store
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

// the budget is a share of the quota, and deliberately ignores `usage`.
// firefox pools storage by group rather than by origin — every localhost
// port reports the same usage — so on a dev machine `usage` describes
// whatever else that browser profile has stored under this host, which is
// nothing this page can see, evict, or reason about. subtracting it once
// pinned this cache to its floor because an unrelated project was sitting
// on ten gigabytes. the quota is still worth reading: it is what the
// browser is willing to offer at all
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
// the fetch hook: the renderer asks for a path, we answer with bytes.
// planetoid metadata ('P...') is not cached, matching the native client
const stats = window.__stats = {
  issued: 0, delivered: 0, failed: 0, inflight: 0,
  idbHits: 0, netHits: 0, maxMs: 0, log: [],
  // the same counts split by what was asked for, with the two waits kept
  // apart: the cache lookup and the network. the engine schedules bulks and
  // nodes separately and a bulk gates a whole subtree, so when a deep zoom
  // is slow the question is which of these two queues a bulk is sitting in
  kinds: {},
  // ?fetchtrace: one record per request — when it was issued, when the
  // cache answered, when the network did, when it was handed to the engine
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
    // a hit is a use: it moves the tile to the young end of the lru, and
    // re-registers one whose bookkeeping went missing (a meta write that
    // failed, or a store carried over from before the budget existed)
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
  // alongside the engine's own 2s line, so the two can be read together.
  // ?fetchlog=0 turns it off
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
