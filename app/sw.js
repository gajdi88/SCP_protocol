// Offline support. Online, the app is fetched fresh so a published update arrives
// on the next launch; offline, or when the network is too slow to answer, the
// copy saved on the last visit is served instead. Bluetooth needs no internet,
// so the saved copy is a fully working app.
const CACHE = 'scp-app';
const FILES = ['./', './index.html', './manifest.webmanifest', './icon-192.png', './icon-512.png'];
const TIMEOUT = 3000;   // a weak signal must not leave the screen blank

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(FILES)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));

self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== location.origin) return;

  // no-store skips GitHub Pages' 10-minute HTTP cache, so online means current.
  const net = fetch(req, { cache: 'no-store' }).then(async res => {
    if (res.ok) await (await caches.open(CACHE)).put(req, res.clone());
    return res;
  });
  e.waitUntil(net.catch(() => {}));   // keep refreshing the saved copy even if it lost the race

  e.respondWith((async () => {
    try {
      const res = await Promise.race([net, new Promise(r => setTimeout(r, TIMEOUT))]);
      if (res) return res;
    } catch (err) { /* offline */ }
    const saved = await caches.match(req, { ignoreSearch: true });
    return saved || net;              // nothing saved yet: wait for the network after all
  })());
});
