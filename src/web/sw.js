"use strict";
// Precaches the whole frontend so it opens instantly, even while the
// controller is unreachable, wherever browsers allow service workers (HTTPS,
// e.g. behind a reverse proxy). Only the shell and the ?v= assets are served
// from the cache; API, ui-stream, and OTA requests are never intercepted.
// The bytes name the firmware release and asset tag, so every update installs
// a new worker that replaces the old cache.
const CACHE = "obbw-__FW_RELEASE__.__FW_ASSET_TAG__";
// Every path rootHandler answers with the same shell.
const SHELL_PATHS = [
  "/",
  "/diagnostic",
  "/log",
  "/stats",
  "/history",
  "/admin",
  "/settings",
  "/setup",
];
const ASSETS = __SW_ASSETS__.map((path) => path + "?v=__FW_ASSET_TAG__");

self.addEventListener("install", (event) => {
  // One request at a time spares the controller's few sockets; the HTTP cache
  // may still hold the previous release's shell, so that one revalidates.
  event.waitUntil(
    caches
      .open(CACHE)
      .then(async (cache) => {
        await cache.add(new Request("/", { cache: "no-cache" }));
        for (const url of ASSETS) await cache.add(url);
      })
      .then(() => self.skipWaiting()),
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches
      .keys()
      .then((keys) =>
        Promise.all(
          keys
            .filter((key) => key.startsWith("obbw-") && key !== CACHE)
            .map((key) => caches.delete(key)),
        ),
      )
      .then(() => self.clients.claim()),
  );
});

self.addEventListener("fetch", (event) => {
  const request = event.request;
  const url = new URL(request.url);
  if (request.method !== "GET" || url.origin !== location.origin) return;
  // ?fw= is the page asking the controller for its new shell after an update.
  const shell =
    request.mode === "navigate" &&
    SHELL_PATHS.includes(url.pathname) &&
    !url.searchParams.has("fw");
  if (!shell && !ASSETS.includes(url.pathname + url.search)) return;
  event.respondWith(caches.match(shell ? "/" : request).then((hit) => hit || fetch(request)));
});
