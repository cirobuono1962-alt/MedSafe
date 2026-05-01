const CACHE = 'meddispenser-v2';
self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(['./', './index.html'])).catch(() => {}));
  self.skipWaiting();
});
self.addEventListener('activate', e => {
  e.waitUntil(clients.claim());
});
self.addEventListener('fetch', e => {
  // Ignora richieste non-HTTP e cross-origin
  if (!e.request.url.startsWith('http')) return;
  e.respondWith(
    fetch(e.request).catch(() => caches.match(e.request).then(r => r || new Response('', {status: 408})))
  );
});
