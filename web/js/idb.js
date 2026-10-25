export { db, dbOpen, IDB_OFF, IDB_RO, idbGet, idbPut, idbPutChecked, idbDel };

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
const dbOpen = new Promise((res) => {
  // an open that neither succeeds nor fails is a real state (a blocked
  // upgrade, an origin whose storage is wedged) and it used to be a silent
  // one. the page runs regardless now, so this is a diagnosis, not a bail
  let settled = false;
  setTimeout(() => {
    if (settled) return;
    console.warn('idb: open has been pending for 8s — no cache, and nothing'
      + ' the page stores will persist. if this sticks, clear this site\'s'
      + ' storage (the origin may be out of quota) and reload');
    if (navigator.storage && navigator.storage.estimate)
      navigator.storage.estimate().then((e) => console.warn(
        `idb: origin is using ${Math.round((e.usage || 0) / 1e6)} MB`
        + ` of ${Math.round((e.quota || 0) / 1e6)} MB`));
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
const IDB_OFF = new URLSearchParams(location.search).get('idb') === '0';
// ?idb=ro: look tiles up but never write them (profiling — how much of the
// cache's cost is the writes queueing in front of the reads)
const IDB_RO = new URLSearchParams(location.search).get('idb') === 'ro';
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
    if (navigator.storage && navigator.storage.estimate)
      navigator.storage.estimate().then((e) => console.warn(
        `idb: origin is using ${Math.round((e.usage || 0) / 1e6)} MB`
        + ` of ${Math.round((e.quota || 0) / 1e6)} MB`));
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
