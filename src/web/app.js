"use strict";
import * as R from "/js/runtime.js?v=__FW_ASSET_TAG__";
R.paintTheme(R.themeMode());
// Browsers expose service workers only over HTTPS (e.g. a reverse proxy); a
// fixed URL lets each update check fetch the current worker uncached.
if ("serviceWorker" in navigator) navigator.serviceWorker.register("/sw.js").catch(() => {});
document.addEventListener("click", (e) => {
  if (e.target && e.target.closest("#themeSignal")) R.cycleTheme();
});
const assetQuery = new URL(import.meta.url).search;
const jsMods = new Map();
const viewLoads = new Map();
let bootFetch = null;
// Stats and History share one module; every other view is its own module.
const BUNDLES = { stats: "records", history: "records" };
let routeSeq = 0;
let activeView = "";
const baseTitle = document.title;
const ROUTES = {
  "/": "home",
  "/settings": "settings",
  "/stats": "stats",
  "/history": "history",
  "/admin": "admin",
  "/diagnostic": "diagnostic",
  "/log": "diagnostic",
  "/setup": "setup",
};
function knownPath(pathname) {
  const p = (pathname || "/").replace(/\/+$/, "") || "/";
  return Object.prototype.hasOwnProperty.call(ROUTES, p) ? p : null;
}
function viewToPath(view) {
  for (const [p, v] of Object.entries(ROUTES)) {
    if (v === view) return p;
  }
  return "/";
}
// A view module carries its own markup, so a view loads in one request. A
// failed import stays failed for its URL, so a retry asks for a fresh one.
function fetchView(name) {
  const file = BUNDLES[name] || name;
  return import("/js/" + file + ".js" + assetQuery + (viewFails[file] || "")).then(
    (m) => (m.views ? m.views[name] : m),
    (e) => {
      viewFails[file] = "&retry=" + Math.random();
      throw e;
    },
  );
}
const viewFails = {};
// Number fields get − and + keys for touch and mouse; a tap (not a scroll)
// steps on click; holding one repeats. data-wrap fields (clock hours and
// minutes) roll over from max to min and back instead of stopping.
// The field keeps its arrow keys, so the keys stay out of the tab order.
function addNumberKeys(root) {
  root.querySelectorAll('input[type="number"]').forEach((f) => {
    const box = document.createElement("span");
    box.className = "numBox";
    f.replaceWith(box);
    box.append(f);
    for (const up of [false, true]) {
      const b = document.createElement("button");
      let timer,
        repeated = false;
      const step = () => {
        if (f.dataset.wrap != null && +f.value === +(up ? f.max : f.min))
          f.value = up ? f.min : f.max;
        else up ? f.stepUp() : f.stepDown();
        f.dispatchEvent(new Event("input", { bubbles: true }));
        f.dispatchEvent(new Event("change", { bubbles: true }));
      };
      const stop = () => clearTimeout(timer);
      Object.assign(b, { type: "button", tabIndex: -1, textContent: up ? "+" : "−" });
      b.setAttribute("aria-hidden", "true");
      // A second touch restarts the repeat instead of orphaning it, and the
      // click that ends a repeating hold adds no extra step.
      b.addEventListener("pointerdown", (e) => {
        e.preventDefault();
        stop();
        repeated = false;
        timer = setTimeout(() => (timer = setInterval(() => ((repeated = true), step()), 90)), 400);
      });
      b.onclick = () => (repeated ? (repeated = false) : step());
      ["pointerup", "pointerleave", "pointercancel"].forEach((ev) => b.addEventListener(ev, stop));
      box.append(b);
    }
  });
}
async function ensureView(name) {
  if (jsMods.has(name)) return jsMods.get(name);
  if (viewLoads.has(name)) return viewLoads.get(name);
  const load = (async () => {
    const section = document.getElementById("view-" + name);
    if (!section) throw new Error(__WEBUI_TEXT__("shell.missing_view_section") + name);
    let mod = name === "home" && __homeModule;
    if (!mod) {
      const boot = bootFetch?.[0] === name && bootFetch[1];
      if (boot) bootFetch = null;
      mod = await (boot || fetchView(name));
      if (!mod) throw new Error(__WEBUI_TEXT__("shell.missing_secondary_view") + name);
      section.innerHTML = mod.markup;
      addNumberKeys(section);
    }
    if (mod.init) mod.init();
    R.setMutable(R.controlsMutable, true);
    jsMods.set(name, mod);
    return mod;
  })();
  viewLoads.set(name, load);
  try {
    return await load;
  } finally {
    viewLoads.delete(name);
  }
}
R.setEnsureViewHook(ensureView);
// Every route change stops the view-owned polls and streams (the Diagnostic
// log, the onboarding scan); only a live subscription sends "off".
function stopExtraPolls() {
  R.stopDiagnosticStream();
  for (const mod of jsMods.values()) mod.stop?.();
}
// A view that owns a stream or poll (records pages, the log, onboarding)
// exports start(); it runs once the view's module is in and settles false
// when the page cannot load.
function startView(name, seq = routeSeq) {
  return R.withPollGate(async () => {
    if (!R.webUiPollingActive() || name !== activeView || seq !== routeSeq) return false;
    stopExtraPolls();
    let ok = true;
    if (["home", "settings", "admin", "diagnostic"].includes(name)) {
      ok = await R.loadStatus();
      R.armStatusTimer();
    }
    if (!ok || name !== activeView || seq !== routeSeq) return false;
    const { start } = await ensureView(name);
    if (name !== activeView || seq !== routeSeq) return false;
    return !start || (await start()) !== false;
  });
}
// A user navigation moves focus to the new page, so screen readers start there.
async function renderRoute(pathname, focus) {
  const seq = ++routeSeq,
    boot = R.showPageBoot();
  R.stopViewPolls();
  let target = knownPath(pathname) || "/";
  if (R.compatibilityModeOn() && !["admin", "diagnostic", "setup"].includes(ROUTES[target]))
    target = "/admin";
  const view = ROUTES[target];
  if (location.pathname !== target) history.replaceState({}, "", target);
  activeView = view;
  const ready = ensureView(view).then(() => {
    if (seq !== routeSeq) return;
    document.body.classList.toggle("homeView", view === "home");
    document.body.classList.toggle("setupView", view === "setup");
    let changed = false;
    document.querySelectorAll(".view").forEach((el) => {
      if (el.dataset.view === view) changed = el.classList.contains("hidden");
      el.classList.toggle("hidden", el.dataset.view !== view);
    });
    // Each page keeps its own scroll: a new page opens at the top, Back returns
    // to where it was left, and a deep link scrolls to its own section.
    if (changed && !location.hash) scrollTo(0, history.state?.y || 0);
    markNavActive(viewToPath(view));
    const section = focus && document.getElementById("view-" + view);
    if (section) {
      section.tabIndex = -1;
      section.focus({ preventScroll: true });
    }
  });
  R.setActiveView(view, ready);
  try {
    const [, ok] = await Promise.all([ready, startView(view)]);
    if (seq === routeSeq) settleView(ok, boot);
  } catch (e) {
    // The browser's own import error is not for the reader.
    if (seq === routeSeq) R.message(__WEBUI_TEXT__("shell.unable_to_load_view"), "error");
  }
}
// Settings and the record pages subscribe in the background as soon as the
// active view has loaded, so they stay fresh without delaying first paint. Setup (captive
// portal) skips them: sockets are scarce on the SoftAP. A tab opened in the
// background loads nothing until it shows, which is not a failure; once the
// page has loaded, a failed refresh is left to the reachability debounce.
function settleView(ok, boot) {
  if (ok) {
    R.hideHomeBoot(boot);
    if (activeView !== "setup") R.ensureBackgroundStreams();
  } else if (
    R.webUiPollingActive() &&
    !document.hidden &&
    document.body.classList.contains("pageLoading")
  )
    R.message(__WEBUI_TEXT__("shell.unable_to_load_view"), "error");
}
function navigate(path, replace) {
  const target = knownPath(path) || "/";
  if (location.pathname !== target) {
    if (!replace) history.replaceState({ ...history.state, y: scrollY }, "");
    history[replace ? "replaceState" : "pushState"]({}, "", target);
  }
  renderRoute(target, true);
}
history.scrollRestoration = "manual";
R.setViewPollHooks({
  stop: stopExtraPolls,
  start: () => renderRoute(location.pathname),
  route: (pathname) => renderRoute(pathname || location.pathname),
});
document.querySelectorAll("a[data-route]").forEach((a) => {
  a.addEventListener("click", (e) => {
    // Modified clicks keep the browser's own handling (new tab or window).
    if (e.button || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
    e.preventDefault();
    const path = a.getAttribute("data-route") || "/";
    markNavActive(path);
    navigate(path);
  });
});
const root = document.documentElement,
  nav = document.querySelector(".pageNav"),
  mobile = window.matchMedia("(max-width: 699px)"),
  still = window.matchMedia("(prefers-reduced-motion: reduce)");
function updateNavigationLayout() {
  const data = root.dataset;
  for (const mode of mobile.matches ? ["bottom"] : ["icons", "text", "bottom"]) {
    data.navLayout = mode;
    if (nav.scrollWidth <= nav.clientWidth) break;
  }
  sizeHeader();
}
function sizeHeader() {
  root.style.setProperty("--menu-offset", nav.offsetTop + "px");
}
// The selection pill is a drop of mercury: one spring carries its centre and
// width, it stretches into a teardrop while moving and squashes as it brakes.
let pillPlaced = false,
  drag = null,
  dragged = false,
  pillFrame = 0;
const goo = { c: 0, w: 0, tc: 0, tw: 0, sq: 0, cv: 0, wv: 0, sqv: 0, dir: 1, t: 0 },
  svgEl = (tag) => document.createElementNS("http://www.w3.org/2000/svg", tag),
  pill = nav ? svgEl("svg") : null,
  pillPath = pill && pill.appendChild(svgEl("path"));
if (pill) {
  pill.setAttribute("class", "pill");
  nav.append(pill);
  nav.classList.add("pillNav");
}
function drawPill() {
  const sp = Math.min(1, Math.abs(goo.cv) / 1100),
    len = goo.w * (1 + sp * 0.3 + goo.sq),
    r = Math.min(goo.h * Math.min(1, Math.max(0.7, (goo.tw / len) ** 0.5)), len / 2),
    x0 = goo.c - len / 2,
    a = x0 + r,
    b = x0 + len - r,
    p = [];
  for (let k = 0; k < 2; k++)
    for (let i = 0; i < 16; i++) {
      const t = (k - 0.5 + (i - 8) / 8) * Math.PI,
        x =
          i <= 8
            ? k
              ? b + ((a - b) * i) / 8
              : a + ((b - a) * i) / 8
            : (k ? a : b) + r * Math.cos(t),
        u = (goo.dir > 0 ? x - x0 : x0 + len - x) / len;
      p.push([
        x,
        goo.cy +
          (i <= 8 ? (k ? r : -r) : r * Math.sin(t)) *
            (1 -
              sp * 0.45 * Math.max(0, 1 - u) ** 1.6 +
              sp * 0.12 * Math.exp(-(((u - 0.8) / 0.18) ** 2))),
      ]);
    }
  const mid = (i) => p[i % 32].map((v, j) => ((v + p[(i + 1) % 32][j]) / 2).toFixed(1));
  pillPath.setAttribute(
    "d",
    p.reduce((d, q, i) => `${d}Q${q.map((v) => v.toFixed(1))} ${mid(i)}`, `M${mid(31)}`),
  );
}
function settlePill() {
  Object.assign(goo, { c: goo.tc, w: goo.tw, cv: 0, wv: 0, sq: 0, sqv: 0, t: 0 });
}
function pillTick(now) {
  const dt = goo.t ? Math.min(1 / 30, (now - goo.t) / 1000) : 0;
  goo.t = now;
  for (let n = Math.ceil(dt * 240), i = 0; i < n; i++) {
    const h = dt / n,
      v0 = goo.cv;
    if (Math.abs(goo.tc - goo.c) > 2) goo.dir = Math.sign(goo.tc - goo.c);
    for (const k of ["c", "w"]) {
      goo[k + "v"] += ((goo["t" + k] - goo[k]) * 520 - goo[k + "v"] * 31) * h;
      goo[k] += goo[k + "v"] * h;
    }
    const acc = (goo.cv - v0) / h;
    goo.sqv +=
      (-goo.sq * 400 - goo.sqv * 8.8 + Math.abs(acc) * (acc * goo.cv < 0 ? -3.5e-5 : 3.5e-5)) * h;
    goo.sq = Math.min(0.25, Math.max(-0.2, goo.sq + goo.sqv * h));
  }
  const rest =
    Math.abs(goo.tc - goo.c) + Math.abs(goo.tw - goo.w) < 0.1 &&
    Math.abs(goo.cv) + Math.abs(goo.wv) < 2 &&
    Math.abs(goo.sq) + Math.abs(goo.sqv) < 0.002;
  if (rest) settlePill();
  drawPill();
  pillFrame = rest ? 0 : requestAnimationFrame(pillTick);
}
function placePill(animate) {
  const a = pill && nav.querySelector("a.active");
  if (!a?.offsetWidth || (drag && dragged)) return;
  Object.assign(goo, {
    tc: a.offsetLeft + a.offsetWidth / 2,
    tw: a.offsetWidth,
    cy: a.offsetTop + a.offsetHeight / 2,
    h: a.offsetHeight / 2,
  });
  if (pillPlaced && animate && !still.matches) pillFrame ||= requestAnimationFrame(pillTick);
  else {
    pillPlaced = true;
    settlePill();
    drawPill();
  }
}
// The tab title names the page, so history and screen readers tell pages apart.
function markNavActive(path) {
  document.querySelectorAll(".pageNav a").forEach((a) => {
    const current = a.getAttribute("data-route") === path;
    a.classList.toggle("active", current);
    if (!current) return a.removeAttribute("aria-current");
    a.setAttribute("aria-current", "page");
    document.title = path === "/" ? baseTitle : a.textContent + " · " + baseTitle;
  });
  placePill(true);
}
if (nav) {
  nav.setAttribute("aria-label", __WEBUI_TEXT__("shell.primary"));
  nav.querySelectorAll("a").forEach((a) => (a.title = a.textContent));
  nav.ondragstart = () => false;
  // Dragging the pill sideways opens each link it settles on; links pull it
  // toward their centre (slope 0.35 there, 1 at their edges, so it never jumps).
  nav.addEventListener("pointerdown", (e) => {
    if (!e.isPrimary || e.button || !e.target.closest("a.active")) return;
    drag = { x: e.clientX, c: goo.tc };
    nav.setPointerCapture(e.pointerId);
  });
  nav.addEventListener("pointermove", (e) => {
    const dx = drag ? e.clientX - drag.x : 0;
    if (!drag || (!dragged && Math.abs(dx) < 6)) return;
    dragged = true;
    nav.classList.add("dragging");
    const links = [...nav.querySelectorAll("a")]
        .filter((l) => l.offsetWidth)
        .sort((p, q) => p.offsetLeft - q.offsetLeft),
      lo = links[0].offsetLeft,
      hi = links.at(-1).offsetLeft + links.at(-1).offsetWidth,
      c0 = Math.min(Math.max(drag.c + dx, lo), hi - 1),
      i = links.findIndex((l) => c0 < l.offsetLeft + l.offsetWidth),
      h = links[i],
      hw = h.offsetWidth / 2,
      t = (c0 - h.offsetLeft - hw) / hw,
      n = links[i + Math.sign(t)] || h,
      w = h.offsetWidth + ((n.offsetWidth - h.offsetWidth) * t * t) / 2,
      c = h.offsetLeft + hw + t * hw * (0.35 + 0.65 * t * t);
    goo.tc = Math.min(Math.max(c - w / 2, lo), hi - w) + w / 2;
    goo.tw = w;
    pillFrame ||= requestAnimationFrame(pillTick);
    if (h.classList.contains("active")) return;
    markNavActive(h.dataset.route);
    navigate(h.dataset.route, drag.moved);
    drag.moved = true;
  });
  const endDrag = () => {
    if (!drag) return;
    drag = null;
    nav.classList.remove("dragging");
    placePill(true);
    setTimeout(() => (dragged = false));
  };
  nav.addEventListener("pointerup", endDrag);
  nav.addEventListener("pointercancel", endDrag);
  nav.addEventListener(
    "click",
    (e) => dragged && (e.preventDefault(), e.stopImmediatePropagation()),
    true,
  );
  new ResizeObserver(updateNavigationLayout).observe(document.getElementById("app"));
  new ResizeObserver(sizeHeader).observe(document.querySelector(".topBar"));
  const pillAnchor = new ResizeObserver(() => {
    const a = nav.querySelector("a.active"); // Bold active labels resize their links; placement already targets the post-toggle geometry, so only real resizes re-anchor
    if (
      a &&
      goo.tw === a.offsetWidth &&
      goo.tc === a.offsetLeft + a.offsetWidth / 2 &&
      goo.h === a.offsetHeight / 2
    )
      return;
    placePill(false);
  });
  pillAnchor.observe(nav);
  nav.querySelectorAll("a").forEach((a) => pillAnchor.observe(a));
  new MutationObserver(updateNavigationLayout).observe(nav, {
    subtree: true,
    attributes: true,
    attributeFilter: ["class", "hidden"],
  });
  document.fonts?.ready.then(() => {
    updateNavigationLayout();
    placePill(false);
  });
  updateNavigationLayout();
  mobile.addEventListener("change", updateNavigationLayout);
}
const msgEl = document.getElementById("message");
if (msgEl) {
  msgEl.setAttribute("role", "status");
  msgEl.setAttribute("aria-live", "polite");
}
R.initHeaderSignals();
function scrollHeader() {
  root.toggleAttribute("data-scrolled", window.scrollY > 0);
  if (mobile.matches)
    document.body.style.setProperty(
      "--header-progress",
      Math.min(1, Math.max(0, window.scrollY / 120)),
    );
}
window.addEventListener("scroll", scrollHeader, { passive: true });
mobile.addEventListener("change", scrollHeader);
scrollHeader();
window.addEventListener("popstate", () => {
  if (R.exitFullScreenOnPop()) return;
  renderRoute(location.pathname, true);
});
document.addEventListener("visibilitychange", () => {
  R.noteWebUiPowerActivity();
  const seq = routeSeq;
  if (document.hidden) R.stopViewPolls();
  else startView(activeView, seq).then((ok) => seq === routeSeq && settleView(ok));
});
document.addEventListener("wheel", R.noteWebUiInteraction, { capture: true, passive: true });
document.addEventListener("touchmove", R.noteWebUiInteraction, { capture: true, passive: true });
document.addEventListener("pointerdown", R.noteWebUiInteraction, true);
document.addEventListener("click", R.noteWebUiInteraction, true);
document.addEventListener("input", R.noteWebUiInteraction, true);
document.addEventListener("change", R.noteWebUiInteraction, true);
document.addEventListener("keydown", R.noteWebUiInteraction, true);
R.setMutable(false);
// The opening view downloads while the ownership claim is in flight.
const bootView = ROUTES[knownPath(location.pathname)];
if (bootView && bootView !== "home") {
  bootFetch = [bootView, fetchView(bootView)];
  bootFetch[1].catch(() => {});
}
if (bootView === "setup") {
  R.startSetupSession();
  renderRoute(location.pathname);
} else R.claimWebUiOwnership();
