// Offline cache for one complete, versioned release. A new release is
// downloaded completely before it activates; older caches are then removed.
const RELEASE = '__RELEASE__';
const BASE = new URL('./', self.location.href);
const PREFIX = `ir-conv-${BASE.pathname}-`;
const CACHE = `${PREFIX}${RELEASE}`;
const INVENTORY = new URL(`releases/${RELEASE}/offline-resources.json`, BASE).href;
const COMPLETE = new URL(`.offline-complete-${RELEASE}`, BASE).href;
const INDEX = new URL(`releases/${RELEASE}/index.html`, BASE).href;
const REQUIRED = ['index.html', 'app.js', 'app.css', 'worker.js', 'engine.wasm', 'XCC-LICENSE.txt',
  'site.webmanifest', 'favicon.ico', 'favicon-16.png', 'favicon-32.png', 'favicon-48.png',
  'apple-touch-icon-152.png', 'apple-touch-icon-167.png', 'apple-touch-icon.png',
  'icon-192.png', 'icon-512.png', 'icon-1024.png', 'icon-maskable-512.png', 'icon-maskable-1024.png'];
const ALIASES = ['site.webmanifest', 'favicon.ico', 'apple-touch-icon.png'];

function validateInventory(inventory) {
  if (inventory?.release !== RELEASE || !Array.isArray(inventory.resources)) throw new Error('Mixed offline release');
  const paths = new Set();
  for (const item of inventory.resources) {
    if (!item || typeof item.path !== 'string' || !Number.isSafeInteger(item.size) || item.size < 1 ||
        !item.path.startsWith(`releases/${RELEASE}/`) || !/^[a-zA-Z0-9./_-]+$/.test(item.path) || item.path.includes('..'))
      throw new Error('Invalid offline inventory');
    paths.add(item.path);
  }
  if (REQUIRED.some(name => !paths.has(`releases/${RELEASE}/${name}`))) throw new Error('Incomplete offline inventory');
  return inventory;
}
async function complete() {
  try {
    const cache = await caches.open(CACHE);
    if (!await cache.match(COMPLETE)) return false;
    const response = await cache.match(INVENTORY);
    if (!response) return false;
    const inventory = validateInventory(await response.json());
    const present = await Promise.all(inventory.resources.map(async item => {
      const r = await cache.match(new URL(item.path, BASE));
      return r?.ok && (await r.arrayBuffer()).byteLength === item.size;
    }));
    return present.every(Boolean);
  } catch { return false; }
}
async function download() {
  const cache = await caches.open(CACHE);
  await cache.delete(COMPLETE);
  const response = await fetch(INVENTORY, {cache: 'no-store'});
  if (!response.ok) throw new Error('Missing offline inventory');
  const inventory = validateInventory(await response.clone().json());
  await Promise.all(inventory.resources.map(async item => {
    const url = new URL(item.path, BASE);
    const result = await fetch(url, {cache: 'no-store'});
    if (!result.ok) throw new Error(`Missing ${item.path}`);
    const bytes = await result.clone().arrayBuffer();
    if (bytes.byteLength !== item.size) throw new Error(`Incomplete ${item.path}`);
    if (url.href === INDEX && !new TextDecoder().decode(bytes).includes(`const DOCUMENT_RELEASE = '${RELEASE}'`))
      throw new Error('HTML belongs to another release');
    if (item.path.endsWith('.wasm') && new Uint8Array(bytes, 0, 4).join(',') !== '0,97,115,109')
      throw new Error('Invalid WASM resource');
    await cache.put(url, result);
  }));
  await cache.put(INVENTORY, response);
  await cache.put(COMPLETE, new Response(RELEASE));
}

self.addEventListener('install', event => event.waitUntil((async () => {
  try { await download(); await self.skipWaiting(); }
  catch (error) { await caches.delete(CACHE); throw error; }
})()));
self.addEventListener('activate', event => event.waitUntil((async () => {
  if (!await complete()) throw new Error('Incomplete offline package');
  // Running pages keep their already loaded app; older releases are removed.
  for (const name of await caches.keys()) if (name.startsWith(PREFIX) && name !== CACHE) await caches.delete(name);
  await self.clients.claim();
  for (const client of await self.clients.matchAll()) client.postMessage({type: 'offline-ready', release: RELEASE});
})()));
self.addEventListener('message', event => {
  if (event.data?.type !== 'status' || !event.ports[0]) return;
  event.waitUntil((async () => {
    let ready = await complete();
    if (!ready && navigator.onLine) { try { await download(); ready = await complete(); } catch (e) { console.debug('IR-conv repair:', e.message); } }
    event.ports[0].postMessage({ready, release: RELEASE});
  })());
});
self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);
  if (event.request.method !== 'GET' || url.origin !== BASE.origin || !url.pathname.startsWith(BASE.pathname)) return;
  event.respondWith((async () => {
    const cache = await caches.open(CACHE);
    if (event.request.mode === 'navigate') return await cache.match(INDEX) || fetch(event.request);
    const alias = ALIASES.find(name => url.pathname === new URL(name, BASE).pathname);
    if (alias) {
      const response = await cache.match(new URL(`releases/${RELEASE}/${alias}`, BASE));
      // Fresh response so relative manifest URLs resolve against the stable URL.
      if (response) return new Response(response.body, {status: response.status, statusText: response.statusText, headers: response.headers});
      return fetch(event.request);
    }
    return await cache.match(event.request, {ignoreSearch: true}) || fetch(event.request);
  })());
});
