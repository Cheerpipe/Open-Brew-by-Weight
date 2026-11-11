"use strict";
// Precaches the whole frontend so it opens instantly, even while the
// controller is unreachable, wherever browsers allow service workers (HTTPS,
// e.g. behind a reverse proxy). Only the shell and the ?v= assets are served
// from the cache; API, ui-stream, and OTA requests are never intercepted.
// The bytes name the firmware release and asset tag, so every update installs
// a new worker that replaces the old cache.
const CACHE = "obbw-__FW_RELEASE__.__FW_ASSET_TAG__";
// Every path rootHandler answers with the same shell (app.js ROUTES).
const SHELL_PATHS = __SHELL_PATHS__;
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

// Notifications: the companion delivers an event unread, so its wording and its
// tap target are this firmware's. A push always shows something, since Safari
// can revoke the permission of one that shows nothing.
const PUSH_STATE = "push-state"; // Not "obbw-": it outlives a release's cache.
function pushNotification(message) {
  // Home's card says what to do about La Marzocco's terms, so a tap opens Home.
  if (message?.topic === "la_marzocco_policy" && message.d?.v === 1) {
    const title = __WEBUI_TEXT__("push.terms_title");
    return {
      title: message.d.test ? __WEBUI_TEXT__("push.title_test").replace("{0}", title) : title,
      options: {
        body: __WEBUI_TEXT__("push.terms_body"),
        data: { url: "/" },
        ...(typeof message.key === "string" && { tag: message.key }),
      },
    };
  }
  const shot = message?.topic === "shot_result" && message.d?.v === 1 ? message.d : null;
  if (!Number.isFinite(shot?.s))
    return {
      title: __WEBUI_TEXT__("push.generic_title"),
      options: { body: __WEBUI_TEXT__("push.generic_body") },
    };
  const titles = {
      on_recipe: __WEBUI_TEXT__("push.title_on_recipe"),
      fast: __WEBUI_TEXT__("push.title_fast"),
      slow: __WEBUI_TEXT__("push.title_slow"),
    },
    title = (
      Object.hasOwn(titles, shot.r) ? titles[shot.r] : __WEBUI_TEXT__("push.title_shot")
    ).replace("{0}", shot.s.toFixed(1)),
    grams = Number.isFinite(shot.g) ? shot.g.toFixed(1) : null,
    body =
      grams === null
        ? __WEBUI_TEXT__("push.no_yield")
        : (shot.w === "e"
            ? __WEBUI_TEXT__("push.estimated_yield")
            : __WEBUI_TEXT__("push.yield")
          ).replace("{0}", grams),
    target =
      shot.id > 0 &&
      Number.isInteger(shot.id) &&
      Number.isInteger(shot.b) &&
      Number.isInteger(shot.e)
        ? `/stats?shot=${shot.id}&boot=${shot.b}&end=${shot.e}`
        : "/stats";
  return {
    title: shot.test ? __WEBUI_TEXT__("push.title_test").replace("{0}", title) : title,
    options: {
      body,
      data: { url: target },
      ...(typeof message.key === "string" && { tag: message.key }),
      ...(shot.at > 0 && { timestamp: shot.at * 1000 }),
    },
  };
}
const pushRemember = async (name, value) =>
  (await caches.open(PUSH_STATE)).put(name, new Response(JSON.stringify(value)));
// The receipt: posted now, or by the page on its next open away from home.
async function pushReceipt(message) {
  const receipt = { at: Date.now(), key: message?.key, mid: message?.mid };
  await pushRemember("/received", receipt);
  const subscription = await self.registration.pushManager.getSubscription();
  const reply = await fetch("/__companion/push/ack", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ...receipt, endpoint: subscription?.endpoint }),
  });
  if (reply.ok) await pushRemember("/received", { ...receipt, acked: true });
}
self.addEventListener("push", (event) => {
  let message = null;
  try {
    message = event.data?.json();
  } catch {
    // An unreadable message still shows the generic notification.
  }
  const { title, options } = pushNotification(message);
  event.waitUntil(
    self.registration
      .showNotification(title, options)
      .then(() => pushReceipt(message).catch(() => {})),
  );
});
self.addEventListener("notificationclick", (event) => {
  event.notification.close();
  const url = event.notification.data?.url || "/stats";
  event.waitUntil(
    (async () => {
      await pushRemember("/tapped", { at: Date.now() }).catch(() => {});
      const [open] = await self.clients.matchAll({ type: "window" });
      try {
        await (await open.focus()).navigate(url);
      } catch {
        await self.clients.openWindow(url);
      }
    })(),
  );
});
// A browser that replaced its subscription: the holder of the old endpoint moves it.
self.addEventListener("pushsubscriptionchange", (event) => {
  const old = event.oldSubscription,
    applicationServerKey = old?.options?.applicationServerKey;
  if (!applicationServerKey) return;
  event.waitUntil(
    (async () => {
      const subscription =
        event.newSubscription ||
        (await self.registration.pushManager.subscribe({
          userVisibleOnly: true,
          applicationServerKey,
        }));
      const saved = await (await caches.open(PUSH_STATE)).match("/topics");
      await fetch("/__companion/push/subscribe", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          subscription,
          topics: (await saved?.json()) ?? ["shot_result"],
          label: "",
          old: old.endpoint,
        }),
      });
    })().catch(() => {}),
  );
});
