"use strict";

const viewStatusHandlers = {};
function registerViewStatus(name, fn) {
  viewStatusHandlers[name] = fn;
}
let ensureViewHook = async () => {};
function setEnsureViewHook(fn) {
  ensureViewHook = fn;
}
// Stats installs its renderers on init; until then there is no Stats DOM to paint.
let renderShots = () => {},
  renderStatsDurChart = () => {},
  relayoutCharts = () => {};
function setStatsRenderers(r) {
  ({ renderShots, renderStatsDurChart, relayoutCharts } = r);
}
// Settings installs its drip chart and group summaries on init, and the brew
// form's validation and preset review that Home's saves reach through it;
// until then there is no Settings DOM.
let renderSettings = () => {},
  settingsForm = { validateBrewClient: () => [], renderPresetReview() {} };
function setSettingsRenderer(fn) {
  renderSettings = fn;
}
function setSettingsForm(form) {
  settingsForm = form;
}
// View-owned stream subscriptions ride the one socket. A view registers its
// frame type once on init: socket.onopen sends every subscribe() op, onmessage
// hands the type's frames to frame(), and the socket's close, a resync, a
// device reboot, a rejected op (close 4003) and leaving the active view reach
// reset(), resync(), reboot(boot), rejected() and leave(). `cursor` marks
// frames that carry their own cursor, which a pending resync never drops;
// `refresh` is the view's manual refresh.
const streamHandlers = {};
function registerStream(type, handler) {
  streamHandlers[type] = handler;
}
function eachStream(hook, ...args) {
  for (const h of Object.values(streamHandlers)) h[hook]?.(...args);
}
// Group rows: keyed top-level groups get an icon from the view's sprite (p),
// and every keyed group a summary that the view's table fills in.
function initGroupRows(root, p = "g-") {
  root.querySelectorAll("details[data-g]").forEach((d) => {
    if (!d.parentElement.closest(".cfgGroup"))
      d.firstElementChild.insertAdjacentHTML(
        "afterbegin",
        `<svg class="cfgIcon" aria-hidden="true"><use href="#${p}${d.dataset.g}"/></svg>`,
      );
    d.firstElementChild.insertAdjacentHTML("beforeend", '<span class="cfgSum"></span>');
  });
}
// Without interpolate-size (Safari) a group or Home equipment row cannot
// transition its height to auto, so open and close run here: both heights are
// measured once and the browser animates between them on the CSS curve.
if (!CSS.supports("interpolate-size", "allow-keywords"))
  document.addEventListener("click", (e) => {
    const d = e.target.closest(":is(.cfgGroup, .lampRow) > summary")?.parentElement;
    if (!d || matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    e.preventDefault();
    const from = d.offsetHeight,
      shut = d.anim ? !d.anim.shut : d.open;
    d.anim?.cancel();
    d.open = !shut;
    const to = d.offsetHeight;
    d.open = true;
    d.style.overflow = "clip";
    const a = (d.anim = d.animate(
      { height: [from + "px", to + "px"] },
      { duration: 250, easing: "cubic-bezier(0.2, 0, 0, 1)" },
    ));
    a.shut = shut;
    a.onfinish = () => {
      d.open = !shut;
      d.style.overflow = "";
      d.anim = null;
    };
  });
function renderGroupRows(root, summaries) {
  root.querySelectorAll(".cfgSum").forEach((el) => {
    el.textContent = summaries[el.closest("details").dataset.g]?.() || "";
  });
}
async function ensureSettingsDom() {
  await ensureViewHook("settings");
}
let activeView = "",
  viewReady = Promise.resolve(),
  viewSeq = 0;
let statusTimer = 0;
function setActiveView(name, ready = Promise.resolve()) {
  // A cold record entry abandoned by navigation must not keep holding the
  // serialized poll gate until its page lands; the view settles it. The
  // subscriptions themselves stay standing.
  if (activeView !== name) eachStream("leave");
  activeView = name || "";
  if (shotFsActive && activeView !== "home") exitShotFullScreen();
  viewReady = ready;
  viewSeq++;
  relayoutCharts();
}
let stopExtraPollsHook = () => {};
let startViewHook = () => {};
let routeRendererHook = () => {};
function setViewPollHooks(hooks) {
  stopExtraPollsHook = (hooks && hooks.stop) || (() => {});
  startViewHook = (hooks && hooks.start) || (() => {});
  routeRendererHook = (hooks && hooks.route) || (() => {});
}
function stopViewPolls() {
  clearCupWeights();
  clearTimeout(uiState.scanTimer);
  uiState.scanTimer = 0;
  clearInterval(statusTimer);
  statusTimer = 0;
  stopExtraPollsHook();
}
function startView(name) {
  startViewHook(name);
}
function renderRoute(pathname) {
  routeRendererHook(pathname);
}
let diagnosticVisible = false;
// Navigation visibility is Home-stream state now: applied from home deltas on
// every view (see the socket handler) instead of the removed status probe.
function applyDiagnosticNavigation(s) {
  if (s && typeof s.diagnosticPageVisible === "boolean")
    diagnosticVisible = s.diagnosticPageVisible;
  const link = document.querySelector('[data-route="/diagnostic"]');
  if (link) link.classList.toggle("hidden", !diagnosticVisible || compatMode);
}
function applyCompatibilityChrome() {
  document.body.classList.toggle("compatMode", compatMode);
  document.querySelectorAll(".pageNav a[data-route]").forEach((a) => {
    const r = a.getAttribute("data-route");
    if (r && r !== "/admin") a.classList.toggle("hidden", compatMode);
  });
  applyDiagnosticNavigation();
  if (compatMode && activeView && !["admin", "diagnostic", "setup"].includes(activeView))
    renderRoute("/admin");
}
function compatibilityModeOn() {
  return compatMode;
}

const DEVICE_MAX_INFLIGHT = 1;
const WEB_UI_CLIENT_HEADER = "X-WebUI-Client",
  WEB_UI_SESSION_HEADER = "X-WebUI-Session",
  SESSION_KEY = "obbwSession";
// Optional login: a remembered session lives in localStorage, a browser-only
// one in sessionStorage. Blocked storage keeps it for this page only.
let sessionMemory = "";
function sessionToken() {
  try {
    return (
      localStorage.getItem(SESSION_KEY) || sessionStorage.getItem(SESSION_KEY) || sessionMemory
    );
  } catch (_) {
    return sessionMemory;
  }
}
function storeSession(token, remember) {
  sessionMemory = token || "";
  try {
    localStorage.removeItem(SESSION_KEY);
    sessionStorage.removeItem(SESSION_KEY);
    if (token) (remember ? localStorage : sessionStorage).setItem(SESSION_KEY, token);
  } catch (_) {}
}
// A short name for Admin → Access, e.g. "iPhone · Safari"; display only.
function deviceLabel() {
  const ua = navigator.userAgent,
    device =
      /iPad/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1)
        ? "iPad"
        : /iPhone/.test(ua)
          ? "iPhone"
          : /Android/.test(ua)
            ? "Android"
            : /Mac OS X/.test(ua)
              ? "Mac"
              : /Windows/.test(ua)
                ? "Windows"
                : /CrOS/.test(ua)
                  ? "Chromebook"
                  : /Linux/.test(ua)
                    ? "Linux"
                    : "Browser",
    browser = /Edg\//.test(ua)
      ? "Edge"
      : /SamsungBrowser/.test(ua)
        ? "Samsung"
        : /Firefox|FxiOS/.test(ua)
          ? "Firefox"
          : /Chrome|CriOS/.test(ua)
            ? "Chrome"
            : /Safari/.test(ua)
              ? "Safari"
              : "";
  return browser ? device + " · " + browser : device;
}
function webUiHeaders(extra) {
  const token = sessionToken();
  return Object.assign(
    { [WEB_UI_CLIENT_HEADER]: webUiClientId },
    token ? { [WEB_UI_SESSION_HEADER]: token } : {},
    extra || {},
  );
}
let webUiPowerUntil = 0;
function noteWebUiPowerActivity() {
  if (webUiOwner && !document.hidden) webUiPowerUntil = performance.now() + 180e3;
}
function webUiPowerSeconds() {
  return document.hidden
    ? 0
    : Math.max(0, Math.min(30, Math.floor((webUiPowerUntil - performance.now()) / 1000)));
}
const WEB_UI_INACTIVITY_MS = 15 * 60 * 1000;
const NETWORK_RECONNECT_WAIT_MS = 180000;
const SHELL_FW_VERSION = document.documentElement.dataset.fw || "dev";
function randomHex(bytes) {
  const b = new Uint8Array(bytes);
  crypto.getRandomValues(b);
  return [...b].map((v) => v.toString(16).padStart(2, "0")).join("");
}
const webUiClientId = randomHex(12);
let webUiOwner = false,
  setupSession = false,
  webUiClaiming = false,
  webUiInactivityTimer = 0,
  webUiActiveUntil = 0,
  inactiveOverlayTimer = 0,
  networkReconnectWait = false,
  networkReconnectDeadline = 0,
  networkReconnectTimer = 0,
  networkReconnectPollTimer = 0,
  compatMode = false;
// Shared UI state that view modules update in place (an imported binding
// cannot be reassigned by its importer).
const uiState = {
  logEvents: [],
  logMissed: 0,
  shotSortDir: "desc",
  shotSort: "date",
  historyData: { bootId: 0, total: 0, hasMore: false, records: [] },
  historyExportWindow: null,
  historyNextRequest: 0,
  historyDir: "desc",
  // Firmware-side list filters (empty = everything) and the Stats window;
  // none of them outlives the page.
  historyTypes: [],
  shotFilter: {},
  statsWindow: 10,
  scanTimer: 0,
  configDirty: false,
  dateTimeDirty: false,
  bleDirty: false,
  powerDirty: false,
  frontendDirty: false,
  brewDirty: false,
  brewWarnSig: "",
  brewWarnSeq: 0,
  formBaseline: null,
  micraDirty: false,
  quickDirty: false,
  micraSignIn: null,
  dateTimeBaseline: null,
  scanBusy: false,
  networkAddressLoaded: false,
  savedStaSsid: "",
  savedStaOpen: false,
  savedStaWifiSleep: false,
  savedDeviceName: "",
  savedStaAddr: "",
  networkBaseline: null,
  statsFetchMark: null,
  statsExportRequest: 0,
  statsNextRequest: 0,
  statsExportResolve: null,
  savedTimezoneId: "",
  bbwReadback: null,
  bleBaseline: null,
  powerBaseline: null,
  frontendBaseline: null,
  timezonePreviewSync: 0,
  otaLastStatus: null,
  otaBusy: false,
  shotHistory: { bootId: 0, total: 0, hasMore: false, shots: [] },
  shotStats: {},
};
let bootId = 0,
  configRevision = 0,
  lastCommandStatus = null,
  formRev = 0,
  configLoaded = false,
  messageTimer = 0,
  controlsMutable = false,
  statusTimezoneOffsetMinutes = 0,
  statusUtcAnchorSec = 0,
  statusUtcAnchorAt = 0,
  firmwareVersion = "",
  reachFails = 0,
  pollChain = Promise.resolve(),
  commandChain = Promise.resolve(),
  commandBusy = false,
  lastStatusAt = 0,
  deviceInFlight = 0,
  deviceWaiters = [],
  diagnosticUnlocked = false,
  developmentMode = false,
  outOfReachOverlay = false;

const $ = (id) => document.getElementById(id);
function updateHeaderSignals(s = {}) {
  // Home names the scale link once, as scale.available.
  const c = s.snapshotStale ? {} : { bluetoothConnected: s.scale?.available, ...s.connections },
    levels = [
      __WEBUI_TEXT__("runtime.disconnected"),
      __WEBUI_TEXT__("runtime.signal_weak"),
      __WEBUI_TEXT__("runtime.signal_medium"),
      __WEBUI_TEXT__("runtime.signal_strong"),
    ],
    unavailable = __WEBUI_TEXT__("runtime.signal_unavailable");
  for (const [kind, name] of [
    ["wifi", __WEBUI_TEXT__("shell.wifi")],
    ["bluetooth", __WEBUI_TEXT__("shell.bluetooth")],
  ]) {
    const button = $(kind + "Signal");
    if (!button) continue;
    const connected = c[kind + "Connected"],
      signal = c[kind + "Level"],
      valid = Number.isInteger(signal) && signal >= 1 && signal <= 3;
    const level = connected === false ? 0 : connected === true && valid ? signal : "unknown";
    const label =
      level === "unknown"
        ? unavailable
        : level === 0
          ? kind === "bluetooth"
            ? __WEBUI_TEXT__("runtime.scale_disconnected")
            : levels[0]
          : levels[level];
    button.dataset.level = String(level);
    button.title = name + ": " + label;
    button.setAttribute("aria-label", button.title);
    const identity = $(kind + "Name"),
      detail = $(kind + "Detail");
    if (identity)
      identity.textContent =
        connected === true
          ? c[kind + "Name"] || "—"
          : connected === false
            ? __WEBUI_TEXT__("runtime.none")
            : "—";
    if (detail) detail.textContent = label;
  }
}
// Autosave: after a pause a section sends only the keys its edits changed (a
// patch); one request in flight per section, SAVE_GAP_MS apart, later edits
// follow as one trailing patch. A section supplies payload(), save(patch) and
// optionally valid(), and owns uiState[key + "Dirty"]: pending or in flight.
const SAVE_GAP_MS = 1000;
function autosaveSection(key, a) {
  return Object.assign(a, { key, last: null, base: {}, at: 0 });
}
// A key keeps the value its first edit replaced: edited back, it is not sent.
function noteEdit(a, waitMs) {
  const now = a.payload();
  for (const k in now)
    if (a.last && !(k in a.base) && !Object.is(now[k], a.last[k])) a.base[k] = a.last[k];
  a.last = now;
  uiState[a.key + "Dirty"] = true;
  clearTimeout(a.timer);
  a.timer = setTimeout(() => flushEdits(a), waitMs);
}
// True once nothing is left to save; `force` skips the gap and drains edits.
async function flushEdits(a, force) {
  while (a.busy) await a.busy;
  clearTimeout(a.timer);
  const now = a.payload(),
    patch = {};
  for (const k in a.base) if (!Object.is(now[k], a.base[k])) patch[k] = now[k];
  // Nothing changed, or the page dropped the edit (preset switch, disconnect).
  if (!uiState[a.key + "Dirty"] || !Object.keys(patch).length) {
    a.base = {};
    uiState[a.key + "Dirty"] = false;
    return true;
  }
  const gap = a.at + SAVE_GAP_MS - Date.now();
  if (!force && gap > 0) {
    a.timer = setTimeout(() => flushEdits(a), gap);
    return false;
  }
  if (a.valid && !a.valid()) return false;
  const sent = a.base;
  a.base = {};
  a.at = Date.now();
  const ok = await (a.busy = a.save(patch));
  a.busy = null;
  if (ok) return flushEdits(a, force);
  // A failed patch stays pending with its error shown; the next edit retries.
  Object.assign(a.base, sent);
  return false;
}
// Home Quick Settings in the header mirror the Integration API (and Home
// Assistant): every change is a session-only override on the controller,
// autosaved like Settings so a burst of taps travels as one patch.
const QUICK_GUARDS = [
  "slowExtractionGuardEnabled",
  "fastExtractionGuardEnabled",
  "noScaleBbwMode",
  "autoToManualGuardEnabled",
  "avoidAccidentalTouchEnabled",
  "cupProtectionEnabled",
];
const QUICK_ICONS = {
  weight: "M12 4v16M6 20h12M5 7h14M5 7l-3 6.5a3 3 0 0 0 6 0zM19 7l-3 6.5a3 3 0 0 0 6 0z",
  hand: "M8 13V6a1.5 1.5 0 0 1 3 0v5M11 10.5V4.5a1.5 1.5 0 0 1 3 0V11M14 10.5V6a1.5 1.5 0 0 1 3 0v8a7 7 0 0 1-7 7 6 6 0 0 1-5.2-3L3 14.5a1.5 1.5 0 0 1 2.5-1.6L8 15",
  shield: "M12 3 5 6v5c0 4.5 3 8.3 7 10 4-1.7 7-5.5 7-10V6z",
  fastExtractionGuardEnabled: "M13 3 5 14h6l-1 7 8-11h-6z",
  slowExtractionGuardEnabled: "M7 3h10M7 21h10M8 3v3l4 6-4 6v3M16 3v3l-4 6 4 6v3",
  noScaleBbwMode: "M4 19h16l-2.2-9H6.2zM12 10V5M9 5h6",
  autoToManualGuardEnabled: "M4 12a8 8 0 1 0 2.4-5.7M4 4v4.5h4.5M12 8v4l3 2",
  avoidAccidentalTouchEnabled:
    "M9 12V5.5a1.5 1.5 0 0 1 3 0V12M12 11V9.5a1.5 1.5 0 0 1 3 0V12M15 11.5a1.5 1.5 0 0 1 3 0V15a6 6 0 0 1-6 6h-1a6 6 0 0 1-5-2.7L4 15a1.5 1.5 0 0 1 2.5-1.6L9 16",
  cupProtectionEnabled:
    "M5 9h11v4a5 5 0 0 1-5 5h-1a5 5 0 0 1-5-5zM16 10.5h1.5a2.5 2.5 0 0 1 0 5H16M8 3.5v2.5M11.5 3.5v2.5",
};
const quickIcon = (k, cls = "") =>
  `<svg class="qsI ${cls}" viewBox="0 0 24 24" aria-hidden="true"><path d="${QUICK_ICONS[k]}"/></svg>`;
function buildQuickSettings() {
  const names = {
      fastExtractionGuardEnabled: __WEBUI_TEXT__("runtime.quick_extend_fast_shots"),
      slowExtractionGuardEnabled: __WEBUI_TEXT__("runtime.quick_shorten_slow_shots"),
      noScaleBbwMode: __WEBUI_TEXT__("runtime.quick_scale_guard"),
      autoToManualGuardEnabled: __WEBUI_TEXT__("runtime.quick_scale_loss_backup"),
      avoidAccidentalTouchEnabled: __WEBUI_TEXT__("runtime.quick_avoid_accidental_touch"),
      cupProtectionEnabled: __WEBUI_TEXT__("runtime.quick_cup_protection"),
    },
    modes = quickIcon("weight", "qsW") + quickIcon("hand", "qsM"),
    mode = (value, icon, name, hint) =>
      `<button type="button" role="radio" data-quick="brewByWeight" data-value="${value}">${quickIcon(icon)}<b>${name}</b><small>${hint}</small></button>`;
  $("modeSignal").innerHTML = modes;
  $("guardSignal").innerHTML =
    quickIcon("shield", "qsShield") +
    `<span class="qsStack">${QUICK_GUARDS.map((k) => `<i data-quick="${k}">${quickIcon(k)}</i>`).join("")}</span>`;
  const panel = document.createElement("section");
  panel.id = "quickPanel";
  panel.className = "signalDetails quickPanel";
  panel.hidden = true;
  panel.setAttribute("aria-labelledby", "quickTitle");
  panel.innerHTML =
    `<header><span class="qsIc">${modes}</span><span><strong id="quickTitle"><span class="qsW">${__WEBUI_TEXT__("runtime.quick_shots_stop_by_weight")}</span><span class="qsM">${__WEBUI_TEXT__("runtime.quick_shots_stop_manually")}</span></strong><small id="quickStatus"></small></span></header>` +
    `<div class="qsModes" role="radiogroup" aria-label="${escHtml($("modeSignal").dataset.label)}">${mode(true, "weight", __WEBUI_TEXT__("runtime.quick_by_weight_mode"), __WEBUI_TEXT__("runtime.quick_by_weight_mode_hint"))}${mode(false, "hand", __WEBUI_TEXT__("runtime.quick_manual_mode"), __WEBUI_TEXT__("runtime.quick_manual_mode_hint"))}</div>` +
    `<p class="qsSub">${$("guardSignal").dataset.label}</p><div class="qsGuards">` +
    QUICK_GUARDS.map(
      (k) =>
        `<button type="button" class="qsRow" role="switch" data-quick="${k}"><span class="qsGi">${quickIcon(k)}</span><span>${names[k]}</span><i class="qsSw" aria-hidden="true"></i></button>`,
    ).join("") +
    `</div><p class="qsSub">${__WEBUI_TEXT__("runtime.quick_duration")}</p>` +
    `<div class="qsRev" role="radiogroup" aria-label="${escHtml(__WEBUI_TEXT__("runtime.quick_duration"))}">` +
    QUICK_REVERT.map(
      (k, i) =>
        `<button type="button" role="radio" data-revert="${k}">${__WEBUI_TEXT__("runtime.quick_duration_options").split("|")[i]}</button>`,
    ).join("") +
    `</div><p class="qsNote"><span id="quickRevertNote"></span> <button type="button" class="qsReset" id="quickRevertNow" hidden>${__WEBUI_TEXT__("runtime.quick_reset_now")}</button></p>`;
  $("headerSignals").append(panel);
  panel.addEventListener("click", (e) => {
    const button = e.target.closest("button");
    if (!button || button.disabled) return;
    if (button.dataset.quick) setQuickSetting(button);
    else if (button.dataset.revert) editQuickSetting("revertAfter", button.dataset.revert);
    else if (button.id === "quickRevertNow") revertQuickSettingsNow();
  });
}
const QUICK_REVERT = ["next_shot", "15m", "30m", "60m", "keep"];
function quickRevertNote(r, i) {
  const when =
    r.overrides && i && i < 4
      ? __WEBUI_TEXT__("runtime.quick_revert_in").replace(
          "{n}",
          Math.max(1, Math.ceil((r.remainingS || 0) / 60)),
        )
      : __WEBUI_TEXT__("runtime.quick_revert_when").split("|")[i];
  return (
    r.overrides
      ? __WEBUI_TEXT__("runtime.quick_revert_pending")
      : __WEBUI_TEXT__("runtime.quick_revert_hint")
  ).replace("{when}", when);
}
let quickNoScaleMode = "warn_once",
  quickStatus = {},
  quickDraft = {};
const quickOn = (c, k) => (k === "noScaleBbwMode" ? !!c[k] && c[k] !== "off" : !!c[k]);
// The shown values: the controller's, with this page's pending choices on top.
const quickAutosave = autosaveSection("quick", {
  payload: () => {
    const c = { ...quickStatus.config, revertAfter: quickStatus.quickRevert?.after, ...quickDraft };
    return Object.fromEntries(
      ["brewByWeight", "revertAfter", ...QUICK_GUARDS].map((k) => [k, c[k]]),
    );
  },
  save: saveQuickSettings,
});
function updateQuickSettings(s = quickStatus) {
  const root = $("headerSignals");
  if (!root || typeof s.config?.brewByWeight !== "boolean") return;
  quickStatus = s;
  // A preset change or a cycle drops pending choices: the controller would
  // restore or refuse them. Settled, the shown values base the next edit.
  const a = quickAutosave;
  if (!a.busy && (!uiState.quickDirty || s.cycle?.active || presetState.activeId !== a.presetId)) {
    clearTimeout(a.timer);
    uiState.quickDirty = false;
    quickDraft = {};
    a.base = {};
    a.last = a.payload();
  }
  const c = { ...s.config, ...quickDraft };
  if (quickOn(c, "noScaleBbwMode")) quickNoScaleMode = c.noScaleBbwMode;
  const manual = !c.brewByWeight,
    on = QUICK_GUARDS.filter((k) => quickOn(c, k)).length,
    locked = !controlsMutable || !!s.cycle?.active,
    status = manual
      ? __WEBUI_TEXT__("runtime.quick_manual_status")
      : on === QUICK_GUARDS.length
        ? __WEBUI_TEXT__("runtime.quick_all_guards_on")
        : on
          ? __WEBUI_TEXT__("runtime.quick_some_guards_on").replace("{n}", on)
          : __WEBUI_TEXT__("runtime.quick_no_guards_on");
  root.dataset.mode = manual ? "manual" : "weight";
  $("quickSignal").hidden = false;
  $("guardSignal").dataset.level = on === QUICK_GUARDS.length ? "all" : on ? "some" : "none";
  $("quickSignal").title = $("quickStatus").textContent = status;
  root.querySelectorAll("[data-quick]").forEach((el) => {
    const k = el.dataset.quick,
      v = k === "brewByWeight" ? String(c.brewByWeight) === el.dataset.value : quickOn(c, k);
    el.classList.toggle("off", !v);
    if (el.tagName !== "BUTTON") return;
    el.setAttribute("aria-checked", String(v));
    el.disabled = locked || (manual && k !== "brewByWeight");
  });
  const r = s.quickRevert || {},
    after = quickDraft.revertAfter ?? r.after,
    i = QUICK_REVERT.includes(after) ? QUICK_REVERT.indexOf(after) : 4;
  root.querySelectorAll("[data-revert]").forEach((el) => {
    el.setAttribute("aria-checked", String(el.dataset.revert === QUICK_REVERT[i]));
    el.disabled = locked;
  });
  $("quickRevertNote").textContent = quickRevertNote(r, i);
  $("quickRevertNow").hidden = !r.overrides;
  $("quickRevertNow").disabled = locked;
}
function setQuickSetting(button) {
  const k = button.dataset.quick,
    on = button.getAttribute("aria-checked") === "true",
    value =
      k === "brewByWeight"
        ? button.dataset.value === "true"
        : k === "noScaleBbwMode"
          ? on
            ? "off"
            : quickNoScaleMode
          : !on;
  if (k === "brewByWeight" && on) return;
  editQuickSetting(k, value);
}
function editQuickSetting(k, value) {
  if (!uiState.quickDirty) quickAutosave.presetId = presetState.activeId;
  quickDraft[k] = value;
  noteEdit(quickAutosave, 300);
  updateQuickSettings();
}
// The settings go as one patch; revertAfter and revertNow go alone. A failed
// change drops the pending choices, so the panel shows what is live.
async function saveQuickSettings(patch) {
  const { revertAfter, ...fields } = patch;
  for (const change of [fields, { revertAfter }]) {
    if (Object.values(change).every((v) => v === undefined)) continue;
    for (let retry = true; ; retry = false)
      try {
        await command(
          "/api/v1/integration/quick-settings",
          { baseRevision: configRevision, ...change },
          1,
          "",
        );
        break;
      } catch (e) {
        // The patch holds only this page's choices: resend it once on a newer revision.
        if (retry && e.code === "STALE_REVISION") {
          await refreshStatus();
          continue;
        }
        quickDraft = {};
        uiState.quickDirty = false;
        message(e.message, "error");
        updateQuickSettings();
        return false;
      }
  }
  return true;
}
// Reset now drops the pending choices and goes out after any save in flight.
async function revertQuickSettingsNow() {
  const a = quickAutosave;
  clearTimeout(a.timer);
  quickDraft = {};
  uiState.quickDirty = false;
  updateQuickSettings();
  while (a.busy) await a.busy;
  await (a.busy = saveQuickSettings({ revertNow: true }));
  a.busy = null;
}
function initHeaderSignals() {
  if ($("modeSignal")) buildQuickSettings();
  const buttons = [...document.querySelectorAll(".signalIndicator[aria-controls]")];
  function close() {
    if (uiState.quickDirty) flushEdits(quickAutosave, true);
    for (const button of buttons) {
      $(button.getAttribute("aria-controls")).hidden = true;
      button.setAttribute("aria-expanded", "false");
    }
  }
  for (const button of buttons)
    button.addEventListener("click", () => {
      const panel = $(button.getAttribute("aria-controls")),
        open = panel.hidden;
      close();
      panel.hidden = !open;
      button.setAttribute("aria-expanded", String(open));
    });
  document.addEventListener("click", (e) => {
    if (!e.target.closest(".headerSignals")) close();
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      const button = buttons.find((b) => b.getAttribute("aria-expanded") === "true");
      close();
      if (button) button.focus();
    }
  });
  window.addEventListener("popstate", close);
}
let homeBootDone = false,
  fwReloading = false,
  bootSeq = 0,
  bootTimer = 0;
const THEME_KEY = "obbwTh",
  THEME_MODES = ["auto", "light", "dark"];
function themeMode() {
  try {
    const v = localStorage.getItem(THEME_KEY);
    return THEME_MODES.includes(v) ? v : "auto";
  } catch (_) {
    return "auto";
  }
}
function paintTheme(m) {
  const h = document.documentElement,
    c = m === "auto" ? "light dark" : m;
  h.classList.toggle("theme-dark", m === "dark");
  h.classList.toggle("theme-light", m === "light");
  h.style.colorScheme = c;
  const e = document.querySelector('meta[name="color-scheme"]');
  if (e) e.content = c;
  const b = document.getElementById("themeSignal");
  if (b) {
    b.dataset.mode = m;
    b.setAttribute(
      "aria-label",
      {
        auto: __WEBUI_TEXT__("shell.theme_auto"),
        light: __WEBUI_TEXT__("shell.theme_light"),
        dark: __WEBUI_TEXT__("shell.theme_dark"),
      }[m] || b.getAttribute("aria-label"),
    );
  }
}
function setTheme(m) {
  if (!THEME_MODES.includes(m)) m = "auto";
  try {
    localStorage.setItem(THEME_KEY, m);
  } catch (_) {}
  paintTheme(m);
}
function cycleTheme() {
  setTheme(THEME_MODES[(THEME_MODES.indexOf(themeMode()) + 1) % 3]);
}
// A running shot keeps the UI session alive (the Web UI doubles as a shot
// timer); every signal is read from the live WebSocket state so a frozen
// status copy cannot extend the session on non-polling views.
// Takeover by another client still deactivates immediately elsewhere.
function sessionShotLive() {
  const s = homeFrame && homeFrame.status;
  return !!(
    (shotFrame && shotFrame.card.valid && shotFrame.card.live && !shotStale) ||
    (!homeStale && s && (s.liveShot || s.machineRunning))
  );
}
// /setup (captive portal) needs no ownership: its endpoints are authorized by
// the SoftAP link, so it skips the claim, the UI stream and the inactivity
// lock, and another window (CNA sheet, browser, second phone) cannot
// deactivate it mid-setup.
function startSetupSession() {
  setupSession = webUiOwner = true;
  webUiActiveUntil = Infinity;
}
function resetWebUiInactivity() {
  if (!webUiOwner || setupSession) return;
  webUiActiveUntil = Date.now() + WEB_UI_INACTIVITY_MS;
  clearTimeout(webUiInactivityTimer);
  webUiInactivityTimer = setTimeout(() => {
    if (webUiOwner && Date.now() >= webUiActiveUntil)
      sessionShotLive() ? resetWebUiInactivity() : deactivateWebUi();
  }, WEB_UI_INACTIVITY_MS);
}
function webUiPollingActive() {
  if (!webUiOwner) return false;
  if (Date.now() < webUiActiveUntil) return true;
  if (sessionShotLive()) {
    resetWebUiInactivity();
    return true;
  }
  deactivateWebUi();
  return false;
}
function noteWebUiInteraction(event) {
  if (!webUiOwner || !event.isTrusted) return;
  noteWebUiPowerActivity();
  const target = event.target;
  if (
    !target ||
    !target.closest ||
    !target.closest(
      'button,a,input,select,textarea,label,summary,[role="button"],[role="switch"],.presetCard',
    )
  )
    return;
  resetWebUiInactivity();
}
function withPollGate(fn) {
  const run = pollChain.then(fn, fn);
  pollChain = run.catch(console.warn);
  return run;
}
function withCommandGate(fn) {
  const run = commandChain.then(fn, fn);
  commandChain = run.catch(console.warn);
  return run;
}
function drainDeviceSlots() {
  while (deviceWaiters.length && deviceInFlight < DEVICE_MAX_INFLIGHT) {
    deviceInFlight++;
    deviceWaiters.shift()();
  }
}
function acquireDeviceSlot() {
  return new Promise((resolve) => {
    deviceWaiters.push(resolve);
    drainDeviceSlots();
  });
}
function releaseDeviceSlot() {
  deviceInFlight--;
  drainDeviceSlots();
}
function statusPollDue() {
  return !commandBusy && !uiState.otaBusy && Date.now() - lastStatusAt >= 900;
}
// Admin only; every other view rides the owned UI stream (ARCHITECTURE.md).
function armStatusTimer() {
  clearInterval(statusTimer);
  if (!webUiPollingActive() || activeView !== "admin") return;
  statusTimer = setInterval(() => {
    if (webUiPollingActive() && statusPollDue()) refreshStatus();
  }, 1e4);
}
function pad2(n) {
  return String(n).padStart(2, "0");
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function formatTzLabel(min) {
  const sign = min >= 0 ? "+" : "-";
  const abs = Math.abs(min);
  return __WEBUI_TEXT__("runtime.utc") + sign + pad2(Math.floor(abs / 60)) + ":" + pad2(abs % 60);
}
function formatWallTimeLocal(unixLocalSec) {
  const d = new Date(0);
  d.setUTCSeconds(unixLocalSec);
  return (
    d.getUTCFullYear() +
    "-" +
    pad2(d.getUTCMonth() + 1) +
    "-" +
    pad2(d.getUTCDate()) +
    " " +
    pad2(d.getUTCHours()) +
    ":" +
    pad2(d.getUTCMinutes()) +
    ":" +
    pad2(d.getUTCSeconds())
  );
}
const HUMAN_WD = __WEBUI_TEXT__("runtime.weekdays").split(","),
  HUMAN_MON = __WEBUI_TEXT__("runtime.months_short").split(",");
// The controller's UTC now, from its last status (the phone's clock may be
// wrong); the phone's clock only until a status arrives.
function deviceUtcSec() {
  return statusUtcAnchorSec
    ? statusUtcAnchorSec + Math.floor((performance.now() - statusUtcAnchorAt) / 1000)
    : Math.floor(Date.now() / 1000);
}
function formatHumanTime(l) {
  const dt = new Date(l * 1e3),
    now = statusUtcAnchorSec ? deviceUtcSec() + statusTimezoneOffsetMinutes * 60 : 0,
    n = new Date(now * 1e3),
    d = Math.floor(now / 86400) - Math.floor(l / 86400),
    c = dt.toISOString().slice(11, 16),
    AT = __WEBUI_TEXT__("runtime.at");
  if (now && d >= 0 && d < 7)
    return (
      (d === 0
        ? __WEBUI_TEXT__("runtime.today")
        : d === 1
          ? __WEBUI_TEXT__("runtime.yesterday")
          : HUMAN_WD[dt.getUTCDay()]) +
      AT +
      c
    );
  if (now && d >= 7 && d < 28) {
    const w = (d / 7) | 0;
    return w + (w > 1 ? __WEBUI_TEXT__("runtime.weeks_ago") : __WEBUI_TEXT__("runtime.week_ago"));
  }
  return (
    HUMAN_MON[dt.getUTCMonth()] +
    " " +
    dt.getUTCDate() +
    (now && dt.getUTCFullYear() === n.getUTCFullYear() ? "" : " " + dt.getUTCFullYear())
  );
}
function wrapTimeEl(td, r) {
  if (!(r.hasWallTime && r.endedAtLocalSec)) return;
  const t = document.createElement("time"),
    offset =
      typeof r.timezoneOffsetMinutesAtCommit === "number"
        ? r.timezoneOffsetMinutesAtCommit
        : Math.round((r.endedAtLocalSec - r.endedAtUnixSec) / 60);
  t.dateTime = new Date(r.endedAtUnixSec * 1e3).toISOString();
  t.textContent = td.textContent;
  td.title = formatWallTimeLocal(r.endedAtLocalSec) + " (" + formatTzLabel(offset) + ")";
  td.replaceChildren(t);
}
function verdictLabel(v) {
  return (
    { fast: __WEBUI_TEXT__("runtime.verdict_fast"), slow: __WEBUI_TEXT__("runtime.verdict_slow") }[
      v
    ] || __WEBUI_TEXT__("runtime.verdict_on_recipe")
  );
}
function formatShotEnded(d, verdict) {
  const k = String(d || "")
    .toLowerCase()
    .replace(/_(failure|timeout)$/, "");
  if (/^(ext|fast_|slow_)/.test(k)) return verdictLabel(k[0] === "s" ? "slow" : "fast");
  return (
    {
      paddle: __WEBUI_TEXT__("runtime.manual"),
      activator: __WEBUI_TEXT__("runtime.manual"),
      physical_override: __WEBUI_TEXT__("runtime.manual"),
      normal_target: verdictLabel(verdict),
      scale_threshold: verdictLabel(verdict),
      auto_to_manual: __WEBUI_TEXT__("runtime.scale_loss_backup"),
      cup_removed: __WEBUI_TEXT__("runtime.cup"),
      web_stop: __WEBUI_TEXT__("runtime.web"),
      web_heartbeat: __WEBUI_TEXT__("runtime.heartbeat"),
      hard_limit: __WEBUI_TEXT__("runtime.hard_limit"),
      global_limit: __WEBUI_TEXT__("runtime.hard_limit"),
      wall_limit: __WEBUI_TEXT__("runtime.max_time"),
      configured_wall_limit: __WEBUI_TEXT__("runtime.max_time"),
      relay_safety: __WEBUI_TEXT__("runtime.relay"),
      weight_anomaly: __WEBUI_TEXT__("runtime.anomaly"),
      touch_weight_fallback: __WEBUI_TEXT__("runtime.touch_stop_fallback"),
    }[k] || "—"
  );
}
function axisLabel(v, u) {
  v = Math.round(+v * 10) / 10;
  return isFinite(v) ? v + " " + u : "—";
}
// Derive one estimate per distinct accepted endpoint, without resampling weights.
// Held readings (a hand on the cup) are what the scale showed, not coffee: they
// sit beside the weight line and no flow window may touch them.
function buildShotSparkModel(shot) {
  if (!shot) return null;
  const w = Array.isArray(shot.wCg) ? shot.wCg : [],
    at = shot.wAtMs,
    markers = shot.wBreakBefore ?? [],
    heldIndex = shot.wHeld ?? [],
    indices = (list) =>
      !Array.isArray(list) ||
      list.some(
        (v, i) => !Number.isInteger(v) || v < 0 || v >= w.length || (i && v <= list[i - 1]),
      );
  if (
    !Array.isArray(at) ||
    at.length !== w.length ||
    w.length > 1201 ||
    indices(markers) ||
    indices(heldIndex) ||
    at.some((t, i) => !Number.isInteger(t) || t < 0 || t > 60000 || (i && t < at[i - 1]))
  )
    return null;
  const breaks = new Set(markers),
    held = new Set(heldIndex),
    colors = { bbw: "var(--ok)", fast: "var(--gFast)", slow: "var(--gSlow)", atm: "var(--mu)" },
    num = (v) => (v == null || !Number.isFinite(+v) ? null : +v);
  const end = num(shot.endS),
    duration = num(shot.durationS),
    drop = num(shot.dropS) ?? num(shot.firstDropS),
    tare = num(shot.tareS),
    ext = num(shot.extendedS),
    atm = num(shot.atmS),
    clear = num(shot.atmClearedS),
    limit = Math.round((end ?? duration ?? Infinity) * 1000);
  // Weight stays on-recipe until a guard fires at extendedS; only then does it
  // take that guard's color. The flow line is always drawn on-recipe blue.
  const guard = shot.extractionExtended
      ? "fast"
      : shot.slowExtractionExtended
        ? "slow"
        : { fast: "fast", slow: "slow" }[shot.verdict] || "bbw",
    kind = (t) =>
      atm != null && t >= atm && (clear == null || t < clear)
        ? "atm"
        : ext != null && t >= ext
          ? guard
          : "bbw";
  const transitions = [ext, atm, clear].filter((t) => t != null).sort((a, b) => a - b),
    segs = [],
    dripSegs = [],
    pts = [],
    flowSegs = [],
    dripFlowSegs = [],
    flowCurve = Array(w.length).fill(null),
    observations = [],
    heldPts = [],
    // A contact's leading edge may pass as weight one sample early.
    touched = [];
  let maxFlow = null,
    plotMaxFlow = 0,
    segment = 0,
    weightPrevious = null;
  // Color splits interpolate only the drawn line; they are never measured rates.
  const append = (segments, p, previous, flow) => {
    const add = (q) => {
      const k = kind(q.t),
        s = segments.at(-1);
      if (!s || !previous || s.kind !== k)
        segments.push({ kind: k, color: flow ? "var(--gFlow)" : colors[k], pts: [q] });
      else s.pts.push(q);
      previous = q;
    };
    if (previous)
      for (const t of transitions)
        if (t > previous.t && t <= p.t && kind(t) !== kind(previous.t)) {
          const q = {
              t,
              cg: previous.cg + ((p.cg - previous.cg) * (t - previous.t)) / (p.t - previous.t),
            },
            s = segments.at(-1);
          s.pts.push(q);
          previous = q;
          add(q);
        }
    add(p);
  };
  for (let i = 0; i < w.length; i++) {
    if (breaks.has(i)) segment++;
    const drip = at[i] > limit;
    if (drip && end == null) continue;
    if (!Number.isFinite(w[i])) {
      segment++;
      weightPrevious = null;
      continue;
    }
    const p = { t: at[i] / 1000, cg: w[i] };
    if (held.has(i)) {
      if (breaks.has(i)) weightPrevious = null;
      if (touched.at(-1)?.[1] >= at[i] - 500) touched.at(-1)[1] = at[i];
      else touched.push([at[i] - 500, at[i]]);
      heldPts.push(p);
      continue;
    }
    if (breaks.has(i)) weightPrevious = null;
    if (drip && !dripSegs.length && weightPrevious) append(dripSegs, weightPrevious, null, false);
    append(drip ? dripSegs : segs, p, weightPrevious, false);
    weightPrevious = p;
    pts.push(p);
    const last = observations.at(-1),
      gray = atm != null && p.t >= atm && (clear == null || p.t < clear);
    if (gray) {
      segment++;
      continue;
    }
    if (last && atm != null && last.t < Math.round(atm * 1000) && at[i] >= Math.round(atm * 1000))
      segment++;
    if (
      last &&
      clear != null &&
      last.t < Math.round(clear * 1000) &&
      at[i] >= Math.round(clear * 1000)
    )
      segment++;
    const observation = { t: at[i], cg: w[i], index: i, segment };
    if (last && last.segment === segment && last.t === at[i])
      observations[observations.length - 1] = observation;
    else observations.push(observation);
  }
  let boundary = 0,
    previousFlow = null,
    flowStart = null;
  const dropSegment = observations.findLast((p) => p.t <= Math.round(drop * 1000))?.segment;
  for (let i = 0; i < observations.length; i++) {
    const p = observations[i],
      previous = observations[i - 1];
    if (!previous || p.segment !== previous.segment) {
      boundary = i;
      previousFlow = null;
      continue;
    }
    const span = Math.max(1000, p.t - previous.t),
      start = p.t - span;
    while (boundary + 1 < i && observations[boundary + 1].t <= start) boundary++;
    const a = observations[boundary],
      b = observations[boundary + 1];
    if (
      a.segment !== p.segment ||
      a.t > start ||
      (drop != null && a.t < Math.round(drop * 1000)) ||
      touched.some(([from, to]) => start <= to && p.t >= from)
    ) {
      previousFlow = null;
      continue;
    }
    const startCg = a.t === start ? a.cg : a.cg + ((b.cg - a.cg) * (start - a.t)) / (b.t - a.t);
    const rate = Math.max(0, ((p.cg - startCg) * 10) / span),
      point = { t: (p.t - span / 2) / 1000, cg: rate * 100 };
    const drip = p.t > limit,
      cutoff = limit / 1000;
    flowCurve[p.index] = rate;
    plotMaxFlow = Math.max(plotMaxFlow, rate);
    if (!drip) maxFlow = Math.max(maxFlow ?? 0, rate);
    if (!drip && !flowSegs.length && drop > 0 && p.segment === dropSegment) flowStart = point;
    if (drip && point.t < cutoff) {
      previousFlow = point;
      continue;
    }
    if (drip && !dripFlowSegs.length && previousFlow) {
      const q = {
        t: cutoff,
        cg:
          previousFlow.cg +
          ((point.cg - previousFlow.cg) * (cutoff - previousFlow.t)) / (point.t - previousFlow.t),
      };
      if (flowSegs.length) append(flowSegs, q, previousFlow, true);
      append(dripFlowSegs, q, null, true);
      previousFlow = q;
    }
    append(drip ? dripFlowSegs : flowSegs, point, previousFlow, true);
    previousFlow = point;
  }
  // Exact event annotations cannot supply estimator support.
  for (const [t, cg] of [
    [drop, num(shot.dropCg)],
    [ext, num(shot.extCg)],
    [atm, num(shot.atmCg)],
  ]) {
    if (t == null || t < 0 || Math.round(t * 1000) > limit || cg == null) continue;
    const p = { t, cg },
      k = kind(t);
    pts.push(p);
    segs.push({ kind: k, color: colors[k], pts: [p] });
  }
  const dur = Math.max(duration || 0, end || 0, ...pts.map((p) => p.t), 0);
  if (!pts.length || dur <= 0) return null;
  const rawMaxW = Math.max(shot.goalG || 0, ...pts.map((p) => p.cg / 100), 0);
  const lastDrip = dripSegs.at(-1)?.pts.at(-1),
    finalPoint = !shot.wTruncated && lastDrip?.cg === num(shot.endCg) ? lastDrip : null;
  return {
    observed: true,
    truncated: !!shot.wTruncated,
    dur,
    timeMax: Math.max(10, Math.ceil(dur / 10) * 10),
    firstDropS: drop > 0 ? drop : null,
    tareS: tare > 0 ? tare : null,
    segs,
    dripSegs,
    finalPoint,
    pts,
    held: heldPts,
    rawMaxW,
    maxW: Math.max(10, Math.ceil(rawMaxW / 10) * 10),
    flowSegs,
    dripFlowSegs,
    flowStart,
    flowCurve,
    maxFlow,
    flowMax: Math.max(0.5, Math.ceil(plotMaxFlow / 0.5) * 0.5),
  };
}
async function populateTimezoneOptions() {
  const select = $("timezoneId");
  if (!timezoneCatalogPromise)
    timezoneCatalogPromise = api("/api/v1/time/zones")
      .then((ids) => {
        if (!Array.isArray(ids) || !ids.includes("Etc/UTC")) throw Error();
        timezoneCatalog = ids;
        return ids;
      })
      .catch((error) => {
        timezoneCatalogPromise = null;
        throw error;
      });
  const ids = await timezoneCatalogPromise;
  if (!select || !select.isConnected || select.options.length > 1) return;
  const draft = uiState.dateTimeDirty ? select.value : uiState.savedTimezoneId;
  const fragment = document.createDocumentFragment();
  fragment.appendChild(new Option(__WEBUI_TEXT__("runtime.select_timezone"), ""));
  for (const id of ids) fragment.appendChild(new Option(id.replaceAll("_", " "), id));
  select.replaceChildren(fragment);
  if (ids.includes(draft)) select.value = draft;
  refreshTimezonePreview();
}

let timezoneCatalog = null,
  timezoneCatalogPromise = null;
let timezonePreviewSeq = 0,
  timezonePreviewAnchor = null,
  timezonePreviewPending = false,
  timezonePreviewLoad = null,
  autoInitPending = false,
  autoInitRetryAt = 0;
function browserTimezone() {
  try {
    return Intl.DateTimeFormat().resolvedOptions().timeZone || "";
  } catch (_) {
    return "";
  }
}

function renderTimezonePreview() {
  const preview = $("timezonePreview"),
    a = timezonePreviewAnchor;
  if (!preview || !a || document.hidden || activeView !== "admin" || !diagnosticUnlocked) return;
  const elapsed = Math.max(0, Math.floor((performance.now() - a.at) / 1000));
  if (
    a.timezoneId !== $("timezoneId")?.value ||
    elapsed >= 30 ||
    (a.nextTransitionUtcSec && a.utcSec + elapsed >= a.nextTransitionUtcSec)
  ) {
    refreshTimezonePreview();
    return;
  }
  preview.textContent =
    __WEBUI_TEXT__("runtime.timezone_preview_prefix") +
    formatWallTimeLocal(a.localSec + elapsed) +
    " (" +
    formatTzLabel(a.offsetMinutes) +
    ")" +
    (a.clockSource === "browser" ? __WEBUI_TEXT__("runtime.timezone_preview_browser") : "");
}

async function refreshTimezonePreview() {
  const select = $("timezoneId"),
    preview = $("timezonePreview");
  if (!select || !preview) return;
  const id = select.value,
    seq = ++timezonePreviewSeq;
  timezonePreviewAnchor = null;
  timezonePreviewPending = false;
  if (!id) {
    preview.textContent = __WEBUI_TEXT__("runtime.select_timezone");
    return;
  }
  preview.textContent = __WEBUI_TEXT__("runtime.timezone_preview_loading");
  if (!diagnosticUnlocked || document.hidden || activeView !== "admin") return;
  timezonePreviewPending = true;
  try {
    const result = await (timezonePreviewLoad = api("/api/v1/time/preview", {
      method: "POST",
      body: body({ timezoneId: id, browserUtcSec: Math.floor(Date.now() / 1000) }),
    }));
    if (
      seq !== timezonePreviewSeq ||
      select.value !== id ||
      !select.isConnected ||
      !diagnosticUnlocked
    )
      return;
    if (result.clockAvailable) {
      timezonePreviewAnchor = { ...result, at: performance.now() };
      renderTimezonePreview();
    } else preview.textContent = __WEBUI_TEXT__("runtime.timezone_preview_no_clock");
  } catch (error) {
    if (seq === timezonePreviewSeq)
      preview.textContent = formatCommandError(
        __WEBUI_TEXT__("runtime.timezone_preview_failed"),
        error,
      );
  } finally {
    if (seq === timezonePreviewSeq) timezonePreviewPending = false;
  }
}

function updateTimezoneControls() {
  const select = $("timezoneId");
  if (select) select.disabled = !controlsMutable || !!$("timezoneAutomatic")?.checked;
}
async function syncTimezone(c) {
  if (
    compatMode ||
    !c ||
    c.timezoneInitialized == null ||
    autoInitPending ||
    Date.now() < autoInitRetryAt ||
    !controlsMutable ||
    commandBusy ||
    (c.timezoneInitialized && (!c.timezoneAutomatic || uiState.dateTimeDirty))
  )
    return;
  const id = browserTimezone();
  if (!id || c.timezoneId === id) return;
  autoInitPending = true;
  try {
    await populateTimezoneOptions();
    if (
      !timezoneCatalog?.includes(id) ||
      !controlsMutable ||
      (c.timezoneInitialized && uiState.dateTimeDirty)
    )
      return;
    await command(
      "/api/v1/config",
      withBaseRev({
        timezoneId: id,
        [c.timezoneInitialized ? "timezoneDetected" : "timezoneAutoInit"]: true,
      }),
      1,
      "",
      "",
    );
  } catch (_) {
    // A rejected zone waits a minute instead of retrying on every frame.
    autoInitRetryAt = Date.now() + 6e4;
  } finally {
    autoInitPending = false;
  }
}
// A shot's own preset graph, from its stored snapshot (never the live preset).
// Timer-only shots draw every cut segment off.
function shotRuleModel(r) {
  const timer = (r.shotType || "").toLowerCase() === "timer_only";
  return buildRuleChartModel({
    goalWeightG: r.goalG,
    fastBeforeS: r.fastBeforeS,
    slowAfterS: r.slowAfterS,
    bbwProtectionMs: r.delayS * 1e3,
    operationalWallMs: r.wallS * 1e3,
    cutRampS: r.cutRampS,
    fastExtractionGuardEnabled: !timer && r.extractionGuardEnabled,
    slowExtractionGuardEnabled: !timer && r.slowExtractionGuardEnabled,
    fastMarginG: r.fastMarginG,
    slowMarginG: r.slowMarginG,
    firstDropS: r.firstDropS,
    brewByWeight: !timer,
  });
}
let shotPresetObserver = null;
// One shared observer re-lays out preset graphs once they get a width; detached hosts drop out.
function observePresetHost(host) {
  shotPresetObserver ??= new ResizeObserver((entries) => {
    for (const e of entries) {
      if (!e.target.isConnected) shotPresetObserver.unobserve(e.target);
      else if (e.target.clientWidth !== e.target._w) e.target._draw();
    }
  });
  shotPresetObserver.observe(host);
}
// Every filled chart fades vertically: its color at the top of the shape, gone at the base.
function fadeFill(id, color, top, base = 0) {
  return `<linearGradient id="${id}" x1="0" y1="0" x2="0" y2="1"><stop stop-color="${color}" stop-opacity="${top}"/><stop offset="1" stop-color="${color}" stop-opacity="${base}"/></linearGradient>`;
}
function setEmptyState(id, text) {
  const el = $(id);
  if (!el) return;
  el.textContent = text || "";
  el.hidden = !text;
}
function updateFirmwareFooter() {
  const fw = firmwareVersion || "—";
  const boot =
    bootId || uiState.shotHistory.bootId ? "#" + (bootId || uiState.shotHistory.bootId) : "—";
  const text = __WEBUI_TEXT__("runtime.firmware") + fw + " · Boot " + boot;
  const el = $("firmwareFooter");
  if (el) el.textContent = text;
  const inactive = $("inactiveFirmware");
  if (inactive) inactive.textContent = text;
}
function resetNetworkAddressLoaded() {
  uiState.networkAddressLoaded = false;
}
// Record pages and the log name the device boot their frames came from.
function noteBootId(b) {
  bootId = b;
}
// Arms a bar-top sentinel so the sticky toolbar after it carries the popup
// shadow only while detached from its in-flow slot. Re-armed when the
// --hdr breakpoint flips so the trigger keeps matching the sticky offset.
function armStickyBar(id) {
  const s = $(id),
    bar = s && s.nextElementSibling;
  if (!bar || s.dataset.armed) return;
  s.dataset.armed = 1;
  let io;
  const arm = () => {
    io?.disconnect();
    const cs = getComputedStyle(document.documentElement);
    // IO rootMargin accepts only px/%, so resolve --hdr (rem) against the root font.
    const hdrPx = parseFloat(cs.getPropertyValue("--hdr")) * parseFloat(cs.fontSize);
    io = new IntersectionObserver(
      (es) => es.forEach((e) => bar.classList.toggle("isStuck", !e.isIntersecting)),
      // Bare unitless zeros are rejected here too; every value needs a unit.
      { rootMargin: "-" + hdrPx + "px 0px 0px 0px" },
    );
    io.observe(s);
  };
  arm();
  window.matchMedia("(min-width: 700px)").addEventListener("change", arm);
}
// One save path for every download: the anchor joins the document for the
// click, and the object URL outlives it so a slow browser finishes reading.
function saveFile(blob, name) {
  const a = document.createElement("a"),
    url = URL.createObjectURL(blob);
  a.href = url;
  a.download = name;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 6e4);
}
function scaleDisplayName(sc) {
  const fr = (sc && sc.preferredFriendlyName && String(sc.preferredFriendlyName).trim()) || "";
  if (fr) return fr;
  return (sc && sc.preferredName && String(sc.preferredName).trim()) || "";
}
let scaleRename = { mac: "", current: "" };
function updateScaleRenameUi(wrapId, mac, current) {
  const w = $(wrapId);
  if (!w) return;
  scaleRename = { mac, current };
  w.classList.toggle("hidden", !mac);
  const a = w.querySelector("a");
  if (a) {
    const dis = !mac || !controlsMutable;
    a.classList.toggle("fieldOff", dis);
    a.setAttribute("aria-disabled", dis ? "true" : "false");
  }
}
function validScaleFriendlyNameClient(v) {
  const t = String(v || "").trim();
  if (!t || t.length > 30) return false;
  if (t[0] === "-" || t[t.length - 1] === "-") return false;
  return /^[A-Za-z0-9 -]+$/.test(t);
}
async function renameScale() {
  if (!scaleRename.mac) return;
  const raw = prompt(__WEBUI_TEXT__("runtime.scale_rename_prompt"), scaleRename.current);
  if (raw === null) return;
  const name = String(raw).trim();
  if (name && !validScaleFriendlyNameClient(name)) {
    message(__WEBUI_TEXT__("runtime.scale_rename_invalid"), "error");
    return;
  }
  return command(
    "/api/v1/scale/friendly-name",
    { mac: scaleRename.mac, name },
    false,
    __WEBUI_TEXT__("runtime.scale_rename_saved"),
    __WEBUI_TEXT__("runtime.scale_rename_failed"),
  );
}
function formatPreferredScale(s) {
  const pauseMs = (s.scale && s.scale.macCachePauseRemainingMs) || 0;
  if (pauseMs > 0)
    return (
      __WEBUI_TEXT__("runtime.waiting") +
      Math.ceil(pauseMs / 1000) +
      __WEBUI_TEXT__("runtime.s_before_looking_for_a_scale")
    );
  const name = scaleDisplayName(s.scale);
  if (name) return name;
  if (s.scale?.preferredMac) return __WEBUI_TEXT__("runtime.unknown_2");
  const mode = s.config && s.config.scaleMacCacheMode;
  return mode === "only" || mode === "prefer"
    ? __WEBUI_TEXT__("runtime.first_detected")
    : __WEBUI_TEXT__("runtime.none");
}
function formatScaleWeight(s) {
  if (!s.scale) return "—";
  const w =
    typeof s.scale.observedWeightG === "number"
      ? s.scale.observedWeightG
      : typeof s.scale.currentWeightG === "number"
        ? s.scale.currentWeightG
        : null;
  return w === null ? "—" : w.toFixed(1) + __WEBUI_TEXT__("runtime.g_2");
}
function formatScaleStatus(s) {
  const sc = s.scale || {};
  return sc.available
    ? {
        STALE: __WEBUI_TEXT__("runtime.stale"),
        NO_SAMPLE: __WEBUI_TEXT__("runtime.no_sample"),
        OVERLOAD: __WEBUI_TEXT__("runtime.overload"),
        ANOMALOUS: __WEBUI_TEXT__("runtime.anomalous"),
      }[sc.streamState] || __WEBUI_TEXT__("runtime.connected")
    : __WEBUI_TEXT__("runtime.disconnected");
}
function formatMachineState(s) {
  return (
    {
      IDLE: __WEBUI_TEXT__("runtime.idle"),
      RUNNING_UNCONFIRMED: __WEBUI_TEXT__("runtime.group_starting"),
      RUNNING: __WEBUI_TEXT__("runtime.group_running"),
      IDLE_UNCONFIRMED: __WEBUI_TEXT__("runtime.group_stopping"),
      UNKNOWN: __WEBUI_TEXT__("runtime.unknown_4"),
    }[s.machineState] || __WEBUI_TEXT__("runtime.unknown_4")
  );
}
function clearCupWeights() {
  ["cupWeight", "dCupWeight", "idleTareStatus", "cupAccessory"].forEach((id) => {
    const el = $(id);
    if (el) el.textContent = "—";
  });
}
function formatCupWeight(s) {
  const c = s.cupPresence || {},
    sc = s.scale || {};
  return !sc.available || sc.streamState !== "FRESH"
    ? "—"
    : c.present === false
      ? __WEBUI_TEXT__("runtime.absent")
      : c.present && c.weightValid === true && Number.isFinite(c.weightG)
        ? "≈ " + c.weightG.toFixed(1) + __WEBUI_TEXT__("runtime.g_2")
        : "—";
}
function formatCupAccessory(s) {
  const c = s.cupPresence || {};
  return s.scale?.available && s.scale.streamState === "FRESH"
    ? c.accessory === "present" && typeof c.accessoryG === "number"
      ? c.accessoryG.toFixed(1) + __WEBUI_TEXT__("runtime.g_2")
      : {
          off: __WEBUI_TEXT__("runtime.off_2"),
          absent: __WEBUI_TEXT__("runtime.absent"),
          settling: __WEBUI_TEXT__("runtime.accessory_settling"),
          not_tracked: __WEBUI_TEXT__("runtime.accessory_not_tracked"),
        }[c.accessory] || __WEBUI_TEXT__("runtime.unknown_4")
    : __WEBUI_TEXT__("runtime.unknown_4");
}
function formatCupState(s) {
  if (!s.scale?.available) return __WEBUI_TEXT__("runtime.unknown_4");
  const c = s.cupPresence || {},
    k = s.config || {},
    required = k.cupProtectionEnabled && k.requireCupToStart && k.brewByWeight !== false;
  return c.state === "PRESENT" || c.present
    ? __WEBUI_TEXT__("runtime.present")
    : required
      ? __WEBUI_TEXT__("runtime.absent_cup_required")
      : __WEBUI_TEXT__("runtime.absent");
}
function formatIdleTare(s) {
  const t = (s.cupPresence || {}).idleTare,
    d = (s.cupPresence || {}).anchorDeltaG,
    w = (s.cupPresence || {}).anchorWaitS,
    sc = s.scale || {};
  if (!t) return "—";
  if (t === "disabled") return __WEBUI_TEXT__("runtime.off_2");
  if (!sc.available || sc.streamState !== "FRESH") return formatScaleStatus(s);
  if (t === "empty" && typeof d === "number" && Math.abs(d) >= 0.5)
    return sub(
      w >= 1
        ? d > 0
          ? __WEBUI_TEXT__("runtime.idle_tare_empty_above_for")
          : __WEBUI_TEXT__("runtime.idle_tare_empty_below_for")
        : d > 0
          ? __WEBUI_TEXT__("runtime.idle_tare_empty_above")
          : __WEBUI_TEXT__("runtime.idle_tare_empty_below"),
      [Math.abs(d).toFixed(1), w],
    );
  return (
    {
      pending: __WEBUI_TEXT__("runtime.idle_tare_pending"),
      machine_not_off: __WEBUI_TEXT__("runtime.idle_tare_machine_off"),
      uncertain: __WEBUI_TEXT__("runtime.idle_tare_reference"),
      retry: __WEBUI_TEXT__("runtime.idle_tare_retry"),
      tared: __WEBUI_TEXT__("runtime.idle_tare_tared"),
      remove: __WEBUI_TEXT__("runtime.idle_tare_remove"),
      ready: __WEBUI_TEXT__("runtime.idle_tare_ready"),
      empty: __WEBUI_TEXT__("runtime.idle_tare_empty"),
    }[t] || __WEBUI_TEXT__("runtime.idle_tare_unavailable")
  );
}
function formatScaleTimer(s) {
  if (!s.scale || typeof s.scale.timerMs !== "number") return "—";
  const totalTenths = Math.floor(Math.max(0, s.scale.timerMs) / 100);
  return (
    Math.floor(totalTenths / 600) +
    ":" +
    pad2(Math.floor(totalTenths / 10) % 60) +
    "." +
    (totalTenths % 10)
  );
}
function clearFieldErrors() {
  // Only the generated messages go: Setup's static error lines share the class.
  document.querySelectorAll("small.fieldError").forEach((e) => e.remove());
  document.querySelectorAll(".invalid").forEach((e) => {
    e.classList.remove("invalid");
    // Red marks can share the input with live yellow warnings; keep their
    // aria pairing instead of wiping it with the red one.
    refreshFieldAria(e);
  });
}
function markFieldError(el, msg) {
  el.classList.add("invalid");
  el.setAttribute("aria-invalid", "true");
  const s = document.createElement("small");
  s.className = "fieldError";
  s.id = "fieldErr" + ++fieldErrSeq;
  s.textContent = msg;
  (el.closest("label") || el.parentElement).appendChild(s);
  const prev = el.getAttribute("aria-describedby");
  el.setAttribute("aria-describedby", (prev ? prev + " " : "") + s.id);
  let d = el.closest("details");
  while (d) {
    d.open = true;
    d = d.parentElement && d.parentElement.closest("details");
  }
}
let fieldErrSeq = 0;
function focusField(el) {
  try {
    el.focus({ preventScroll: true });
  } catch (_) {}
  el.scrollIntoView({ block: "center" });
}
function showFieldError(id, msg) {
  clearFieldErrors();
  const el = $(id);
  if (el) {
    markFieldError(el, msg);
    focusField(el);
  }
  message(msg, "error");
}
// Multi-field brew validation report: every offending field is marked with its
// own messages and the first one gets focus. Away from the Settings view (the
// Home quick-weight sheet saves through the same validator) the settings DOM
// is not visible, so the messages surface through the error banner instead.
function showFieldErrors(list) {
  clearFieldErrors();
  const text = list.map((e) => e.msg).join(" ");
  if (activeView !== "settings") {
    message(text, "error");
    return;
  }
  let focused = false;
  for (const { id, msg } of list) {
    const el = $(id);
    if (!el) continue;
    markFieldError(el, msg);
    if (!focused) {
      focusField(el);
      focused = true;
    }
  }
  message(text, "error");
}
// Top messages open or close their measured box while they fade, on one shared
// duration. The space leads on entry and trails on exit so the fade stays visible.
const REVEAL_BOX = [
  "height",
  "minHeight",
  "paddingTop",
  "paddingBottom",
  "borderTopWidth",
  "borderBottomWidth",
  "marginTop",
  "marginBottom",
];
function reveal(el, show, done) {
  if (!el) return;
  const run = el.getAnimations().find((a) => a.id.startsWith("reveal"));
  if ((run ? run.id === "reveal-in" : !el.hidden) === show) return run || done?.();
  if (window.matchMedia?.("(prefers-reduced-motion: reduce)")?.matches) {
    el.hidden = !show;
    return done?.();
  }
  const box = (c) => Object.fromEntries(REVEAL_BOX.map((k) => [k, c ? c[k] : "0px"])),
    cs = getComputedStyle(el),
    from = el.hidden ? box() : box(cs),
    fade = [{ opacity: el.hidden ? 0 : cs.opacity }, { opacity: +show }];
  el.getAnimations().forEach((a) => a.id.startsWith("reveal") && a.cancel());
  el.hidden = false;
  el.style.overflow = "hidden";
  const t = show
      ? [250, "cubic-bezier(.2,0,0,1)", "cubic-bezier(.7,0,.3,1)"]
      : [250, "cubic-bezier(.4,0,1,1)", "cubic-bezier(0,0,.2,1)"],
    id = show ? "reveal-in" : "reveal-out";
  el.animate(fade, { duration: t[0], easing: t[2], id });
  el.animate([from, show ? box(getComputedStyle(el)) : box()], {
    duration: t[0],
    easing: t[1],
    id,
  }).onfinish = () => {
    el.style.overflow = "";
    el.hidden = !show;
    done?.();
  };
}
function clearMessage() {
  clearTimeout(messageTimer);
  messageTimer = 0;
  const e = $("message");
  reveal(e, false, () => {
    const t = $("messageText");
    if (t) t.textContent = "";
    e.className = "";
  });
}
function message(text, kind = "") {
  if (kind === "error") hideHomeBoot();
  if (!text) return clearMessage();
  clearTimeout(messageTimer);
  messageTimer = 0;
  const e = $("message");
  if (!e) return;
  // The live region takes its politeness before the text changes, so screen
  // readers announce the new message with it.
  e.setAttribute("role", kind === "error" ? "alert" : "status");
  e.setAttribute("aria-live", kind === "error" ? "assertive" : "polite");
  const t = $("messageText");
  if (t) t.textContent = text;
  else e.textContent = text;
  e.className = kind;
  reveal(e, true);
  if (kind === "error") e.scrollIntoView({ block: "nearest" });
  const ms =
    kind === "ok"
      ? 5e3
      : kind === "warn" && !e.querySelector("button:not(#messageClose)")
        ? 15e3
        : 0;
  if (ms) messageTimer = setTimeout(clearMessage, ms);
}
function formatCommandError(fail, e) {
  const detail = e && e.message ? String(e.message) : "";
  if (!fail) return detail || __WEBUI_TEXT__("runtime.request_failed");
  if (!detail || fail.indexOf(detail) >= 0) return fail;
  return fail + " " + detail;
}
function cn(s) {
  return __WEBUI_TEXT__("runtime.could_not") + s + ".";
}
function commandOkMessage(path, v) {
  return commandMessages(path, v)[0];
}
function commandFailMessage(path, v) {
  return commandMessages(path, v)[1];
}
function commandMessages(path, v) {
  v = v || {};
  const hit = {
    "control/rinse": [
      __WEBUI_TEXT__("runtime.rinse_started"),
      __WEBUI_TEXT__("runtime.start_rinse"),
    ],
    "control/stop": [__WEBUI_TEXT__("runtime.shot_stopped"), __WEBUI_TEXT__("runtime.stop_shot")],
    "control/paddle": [
      __WEBUI_TEXT__("runtime.shot_started"),
      __WEBUI_TEXT__("runtime.start_shot"),
    ],
    "control/force-pulse": [
      __WEBUI_TEXT__("runtime.switch_pulse_sent"),
      __WEBUI_TEXT__("runtime.send_switch_pulse"),
    ],
    "control/restart": [
      __WEBUI_TEXT__("runtime.restart_after_the_shot"),
      __WEBUI_TEXT__("runtime.restart_the_controller"),
    ],
    "time/sync": [__WEBUI_TEXT__("runtime.time_sync_started"), __WEBUI_TEXT__("runtime.sync_time")],
    "scale/preferred/clear": [
      __WEBUI_TEXT__("runtime.preferred_scale_cleared"),
      __WEBUI_TEXT__("runtime.clear_preferred_scale"),
    ],
    "calibration/reset": [
      __WEBUI_TEXT__("runtime.learned_stop_offset_reset"),
      __WEBUI_TEXT__("runtime.reset_learned_stop_offset"),
    ],
    "diagnostic/reset-history": [
      __WEBUI_TEXT__("runtime.reset_history_cleared"),
      __WEBUI_TEXT__("runtime.clear_reset_history"),
    ],
    "factory-reset": [
      __WEBUI_TEXT__("runtime.factory_reset_started"),
      __WEBUI_TEXT__("runtime.restore_factory_settings_check_diagnostic"),
    ],
    "device/password": [
      __WEBUI_TEXT__("runtime.device_password_changed"),
      __WEBUI_TEXT__("runtime.change_device_password"),
    ],
  }[path.slice(8)];
  if (path.slice(8) === "control/state-override")
    return v.state === "on"
      ? [__WEBUI_TEXT__("runtime.inferred_brewing"), cn(__WEBUI_TEXT__("runtime.set_brewing"))]
      : [__WEBUI_TEXT__("runtime.inferred_idle"), cn(__WEBUI_TEXT__("runtime.set_idle"))];
  if (hit) return [hit[0], cn(hit[1])];
  if (path.endsWith("/diagnostic/profiler"))
    return v.enabled
      ? [
          __WEBUI_TEXT__("runtime.task_profiler_started"),
          cn(__WEBUI_TEXT__("runtime.start_the_task_profiler")),
        ]
      : [
          __WEBUI_TEXT__("runtime.task_profiler_stopped"),
          cn(__WEBUI_TEXT__("runtime.stop_the_task_profiler")),
        ];
  if (path.endsWith("/diagnostic/scale-profile")) {
    const m = {
      start: [
        __WEBUI_TEXT__("runtime.scale_profile_started"),
        cn(__WEBUI_TEXT__("runtime.start_the_scale_profile")),
      ],
      stop: [
        __WEBUI_TEXT__("runtime.scale_profile_stopped"),
        cn(__WEBUI_TEXT__("runtime.stop_the_scale_profile")),
      ],
      delete: [
        __WEBUI_TEXT__("runtime.scale_profile_deleted"),
        cn(__WEBUI_TEXT__("runtime.delete_the_scale_profile")),
      ],
    };
    const x = m[v.action];
    if (x) return x;
  }
  if (path.endsWith("/network"))
    return v.action === "forget"
      ? [
          __WEBUI_TEXT__("runtime.wi_fi_forgotten_restarting"),
          cn(__WEBUI_TEXT__("runtime.forget_wi_fi")),
        ]
      : [
          __WEBUI_TEXT__("runtime.wi_fi_settings_saved_restarting"),
          cn(__WEBUI_TEXT__("runtime.save_wi_fi_settings")),
        ];
  if (path.endsWith("/presets")) {
    const x = {
      save: [__WEBUI_TEXT__("runtime.preset_saved"), __WEBUI_TEXT__("runtime.save_preset")],
      apply: [__WEBUI_TEXT__("runtime.preset_applied"), __WEBUI_TEXT__("runtime.apply_preset")],
      new: [__WEBUI_TEXT__("runtime.preset_created"), __WEBUI_TEXT__("runtime.create_preset")],
      duplicate: [
        __WEBUI_TEXT__("runtime.preset_duplicated"),
        __WEBUI_TEXT__("runtime.duplicate_preset"),
      ],
      rename: [__WEBUI_TEXT__("runtime.preset_renamed"), __WEBUI_TEXT__("runtime.rename_preset")],
      delete: [__WEBUI_TEXT__("runtime.preset_deleted"), __WEBUI_TEXT__("runtime.delete_preset")],
      restore_factory_values: [
        __WEBUI_TEXT__("runtime.factory_values_restored"),
        __WEBUI_TEXT__("runtime.restore_factory_values"),
      ],
      new_coffee: [
        __WEBUI_TEXT__("runtime.shot_stats_restarted"),
        __WEBUI_TEXT__("runtime.restart_shot_stats"),
      ],
    }[v.action];
    if (x) return [x[0], cn(x[1])];
  }
  if (path.endsWith("/config")) {
    const keys = Object.keys(v).filter((k) => k !== "baseRevision");
    const level = {
      ringRetainLogLevel: [
        __WEBUI_TEXT__("runtime.log_retain_level_saved"),
        __WEBUI_TEXT__("runtime.save_log_retain_level"),
      ],
      serialLogLevel: [
        __WEBUI_TEXT__("runtime.serial_log_level_saved"),
        __WEBUI_TEXT__("runtime.save_serial_log_level"),
      ],
    }[keys.length === 1 && keys[0]];
    if (level) return [level[0], cn(level[1])];
    return [
      __WEBUI_TEXT__("runtime.shared_settings_saved"),
      cn(__WEBUI_TEXT__("runtime.save_shared_settings")),
    ];
  }
  return [__WEBUI_TEXT__("runtime.done"), cn(__WEBUI_TEXT__("runtime.complete_the_request"))];
}
{
  const b = $("messageClose");
  if (b) b.onclick = clearMessage;
}
{
  const r = $("webUiReload");
  if (r) r.onclick = () => claimWebUiOwnership();
}
function setInactiveError(text) {
  const el = $("inactiveError");
  if (!el) return;
  el.textContent = text || "";
  el.hidden = !text;
}
function setOverlayReconnectMode(on) {
  if (on && !$("reconnectWait")) {
    const b = $("webUiReload");
    if (b) {
      const w = document.createElement("div");
      w.id = "reconnectWait";
      w.className = "reconnectRing";
      w.hidden = true;
      w.innerHTML = '<span id="reconnectSeconds">' + "180" + "</span>";
      b.after(w);
    }
  }
  const el = $("webUiInactive");
  if (el) {
    el.classList.toggle("isReconnectWait", !!on);
    el.setAttribute("aria-labelledby", on ? "reconnectSeconds" : "webUiReload");
  }
  const w = $("reconnectWait");
  if (w) w.hidden = !on;
  const b = $("webUiReload");
  if (b) b.hidden = !!on;
  const h = $("inactiveHint");
  if (h) {
    if (!h.dataset.d) h.dataset.d = h.textContent;
    if (!on) h.textContent = h.dataset.d;
  }
}
function updateReconnectCountdown() {
  const n = Math.max(0, Math.ceil((networkReconnectDeadline - Date.now()) / 1e3));
  const s = $("reconnectSeconds");
  if (s) s.textContent = n;
  const w = $("reconnectWait");
  if (w) w.style.setProperty("--p", String(Math.round((n * 100) / 180)));
  const h = $("inactiveHint");
  if (h)
    h.textContent = n
      ? __WEBUI_TEXT__("runtime.waiting_for_the_controller_on_this_address")
      : __WEBUI_TEXT__("runtime.still_waiting_if_the_new_wi_fi");
}
function endNetworkReconnectWait() {
  networkReconnectWait = false;
  networkReconnectDeadline = 0;
  clearInterval(networkReconnectTimer);
  clearTimeout(networkReconnectPollTimer);
  networkReconnectTimer = networkReconnectPollTimer = 0;
  const el = $("webUiInactive");
  if (!el || !el.classList.contains("isVisible")) setOverlayReconnectMode(false);
}
function pollNetworkReconnect() {
  if (!networkReconnectWait) return;
  Promise.resolve(claimWebUiOwnership()).finally(() => {
    if (networkReconnectWait) networkReconnectPollTimer = setTimeout(pollNetworkReconnect, 2e3);
  });
}
function beginNetworkReconnectWait() {
  clearInterval(networkReconnectTimer);
  clearTimeout(networkReconnectPollTimer);
  networkReconnectWait = true;
  networkReconnectDeadline = Date.now() + NETWORK_RECONNECT_WAIT_MS;
  setOverlayReconnectMode(true);
  if (webUiOwner) deactivateWebUi();
  else showInactiveOverlay();
  updateReconnectCountdown();
  networkReconnectTimer = setInterval(updateReconnectCountdown, 1e3);
  networkReconnectPollTimer = setTimeout(pollNetworkReconnect, 2e3);
}
function showInactiveOverlay() {
  const el = $("webUiInactive");
  if (!el) return;
  hideHomeBoot();
  clearTimeout(inactiveOverlayTimer);
  inactiveOverlayTimer = 0;
  setInactiveError("");
  document.body.classList.add("webUiInactive");
  el.setAttribute("aria-hidden", "false");
  // Modal: the page behind is inert.
  for (const n of el.parentElement.children) n.inert = n !== el;
  if (!el.classList.contains("isVisible")) {
    void el.offsetWidth;
    el.classList.add("isVisible");
  }
  if (networkReconnectWait) return;
  const btn = $("webUiReload");
  if (btn) {
    try {
      btn.focus({ preventScroll: true });
    } catch (_) {
      btn.focus();
    }
  }
}
function hideInactiveOverlay() {
  const el = $("webUiInactive");
  if (!el || !el.classList.contains("isVisible")) return;
  el.classList.remove("isVisible");
  document.body.classList.remove("webUiInactive");
  for (const n of el.parentElement.children) n.inert = false;
  clearTimeout(inactiveOverlayTimer);
  inactiveOverlayTimer = setTimeout(() => {
    el.setAttribute("aria-hidden", "true");
    inactiveOverlayTimer = 0;
    setOverlayReconnectMode(false);
  }, 500);
}
function showPageBoot() {
  const seq = ++bootSeq;
  homeBootDone = false;
  clearTimeout(bootTimer);
  document.body.classList.add("pageLoading");
  const el = $("homeBoot");
  if (el) {
    el.style.zIndex = seq > 1 ? "20" : "39";
    el.classList.remove("hidden", "isDone");
    el.setAttribute("aria-hidden", "false");
  }
  return seq;
}
function hideHomeBoot(seq = bootSeq) {
  if (seq !== bootSeq || homeBootDone || fwReloading) return;
  const el = $("homeBoot");
  if (!el) return;
  homeBootDone = true;
  void el.offsetWidth;
  el.classList.add("isDone");
  bootTimer = setTimeout(() => {
    if (seq === bootSeq) {
      el.classList.add("hidden");
      el.setAttribute("aria-hidden", "true");
      document.body.classList.remove("pageLoading");
    }
  }, 250);
}
function setOverlayOutOfReach() {
  if (networkReconnectWait) return;
  outOfReachOverlay = true;
  const h = $("inactiveHint");
  if (h) h.textContent = __WEBUI_TEXT__("shell.out_of_reach_hint");
  showInactiveOverlay();
}
async function api(path, options = {}) {
  if (!webUiPollingActive())
    throw new Error(__WEBUI_TEXT__("runtime.this_window_is_inactive_reload_to_continue"));
  await acquireDeviceSlot();
  if (!webUiPollingActive()) {
    releaseDeviceSlot();
    throw new Error(__WEBUI_TEXT__("runtime.this_window_is_inactive_reload_to_continue"));
  }
  options.headers = webUiHeaders(options.headers);
  const powerSeconds = webUiPowerSeconds();
  if (powerSeconds) options.headers["X-WebUI-Activity"] = String(powerSeconds);
  noteUiSent(powerSeconds > 0);
  if (options.body) options.headers["Content-Type"] ??= "application/json";
  const ac = new AbortController(),
    to = setTimeout(() => ac.abort(), options.timeoutMs || 8e3);
  let response, txt;
  try {
    response = await fetch(path, Object.assign({}, options, { signal: ac.signal }));
    txt = await response.text();
  } catch (err) {
    const e = new Error(
      err && err.name === "AbortError"
        ? __WEBUI_TEXT__("runtime.device_timeout")
        : __WEBUI_TEXT__("runtime.device_unreachable"),
    );
    e.network = true;
    throw e;
  } finally {
    clearTimeout(to);
    releaseDeviceSlot();
  }
  let data = {};
  try {
    data = JSON.parse(txt);
  } catch (_) {
    throw Object.assign(new Error(__WEBUI_TEXT__("runtime.invalid_response")), {
      status: response.status,
    });
  }
  if (!response.ok) {
    if (data.error === "UI_TAKEN_OVER") deactivateWebUi();
    if (data.error === "AUTH_REQUIRED") showSignIn(true);
    const err = new Error(
      data.message || data.error || __WEBUI_TEXT__("runtime.http") + response.status,
    );
    err.code = data.error || "";
    err.status = response.status;
    throw err;
  }
  return data;
}
function noteReachOk() {
  if (reachFails) clearMessage();
  reachFails = 0;
  if (outOfReachOverlay) {
    outOfReachOverlay = false;
    hideInactiveOverlay();
  }
}
function noteReachFail(err, force) {
  clearCupWeights();
  updateHeaderSignals();
  reachFails++;
  if (force || reachFails >= 5) {
    if (err && err.network && !networkReconnectWait) setOverlayOutOfReach();
    else
      message(
        err && err.message ? err.message : __WEBUI_TEXT__("runtime.device_unreachable"),
        "error",
      );
  }
}
function syncAdminSessionUi(unlocked, remoteEnabled = false) {
  const on = !!unlocked || developmentMode;
  updateHomeAdminActions(on, remoteEnabled);
  document.body.classList.toggle("devBuild", developmentMode);
  const lock = $("adminLockPanel"),
    controls = $("adminControls");
  if (lock) lock.classList.toggle("hidden", on);
  if (controls) controls.classList.toggle("hidden", !on);
  diagnosticUnlocked = on;
}
function lockAdminUi() {
  if (!developmentMode) syncAdminSessionUi(false);
  const hint = $("adminConfirmHint");
  if (hint) hint.classList.add("hidden");
  [
    "adminUnlockPassword",
    "diagnosticUnlockPassword",
    "newDevicePassword",
    "confirmDevicePassword",
    "loginNewPassword",
    "loginRepeatPassword",
    "loginCurrentPassword",
    "staPassword",
  ].forEach((id) => {
    const el = $(id);
    if (el) el.value = "";
  });
  resetNetworkAddressLoaded();
}
function deactivateWebUi() {
  if (!webUiOwner || setupSession) return;
  webUiOwner = false;
  stopUiStream();
  webUiActiveUntil = 0;
  clearTimeout(webUiInactivityTimer);
  webUiInactivityTimer = 0;
  stopViewPolls();
  lockAdminUi();
  setMutable(false);
  showInactiveOverlay();
}
// Bounded like api(); null data means a non-JSON body.
async function fetchJson(path, options, ms = 8e3) {
  const ac = new AbortController(),
    to = setTimeout(() => ac.abort(), ms);
  try {
    const response = await fetch(path, { ...options, signal: ac.signal }),
      txt = await response.text();
    try {
      return [response, txt ? JSON.parse(txt) : {}];
    } catch (_) {
      return [response, null];
    }
  } finally {
    clearTimeout(to);
  }
}
// `claimed` is a sign-in response, which already claimed this window.
async function claimWebUiOwnership(claimed) {
  initShotFullScreen();
  if (webUiClaiming) return;
  webUiClaiming = true;
  const rec = networkReconnectWait;
  const btn = $("webUiReload");
  if (btn && !rec) btn.disabled = true;
  try {
    let data = claimed || {};
    if (!claimed) {
      const [response, json] = await fetchJson(
        "/api/v1/ui/claim",
        {
          method: "POST",
          headers: webUiHeaders({ "Content-Type": "application/json" }),
          body: "{}",
        },
        rec ? 4e3 : 8e3,
      );
      data = json;
      if (!data) throw new Error(__WEBUI_TEXT__("runtime.invalid_ownership_response"));
      if (data.error === "AUTH_REQUIRED") {
        endNetworkReconnectWait();
        showSignIn(!!sessionToken());
        return;
      }
      if (!response.ok)
        throw new Error(data.message || data.error || __WEBUI_TEXT__("runtime.unable_to_reload"));
    }
    endNetworkReconnectWait();
    webUiOwner = true;
    resetWebUiInactivity();
    startUiStream();
    noteWebUiPowerActivity();
    stopViewPolls();
    hideInactiveOverlay();
    if (typeof data.compatibilityMode === "boolean" && data.compatibilityMode !== compatMode) {
      compatMode = data.compatibilityMode;
      applyCompatibilityChrome();
    }
    if (activeView) startView(activeView);
    else renderRoute(location.pathname);
  } catch (e) {
    if (rec) return;
    webUiOwner = false;
    webUiActiveUntil = 0;
    clearTimeout(webUiInactivityTimer);
    webUiInactivityTimer = 0;
    stopViewPolls();
    setMutable(false);
    showInactiveOverlay();
    if (e && (e.network || e.name === "AbortError" || e instanceof TypeError))
      setOverlayOutOfReach();
    else setInactiveError(e && e.message ? e.message : __WEBUI_TEXT__("runtime.unable_to_reload"));
  } finally {
    webUiClaiming = false;
    if (btn) btn.disabled = false;
  }
}
function setSignInError(text) {
  const el = $("loginError");
  if (!el) return;
  el.textContent = text || "";
  el.hidden = !text;
}
// The sign-in form lives in the inactive overlay: same brand, footer and
// focus handling, with the Reload controls hidden.
async function showSignIn(signedOut) {
  const overlay = $("webUiInactive");
  if (!overlay || !$("webUiLogin")) return;
  if (signedOut) storeSession("", false);
  // Several requests can fail at once; one sign-in form is enough.
  if (overlay.classList.contains("isLogin") && overlay.classList.contains("isVisible")) return;
  if (webUiOwner) {
    webUiOwner = false;
    stopUiStream();
    stopViewPolls();
    lockAdminUi();
    setMutable(false);
  }
  overlay.classList.add("isLogin");
  showInactiveOverlay();
  setSignInError("");
  $("loginIntro").textContent = signedOut
    ? __WEBUI_TEXT__("runtime.signed_out_hint")
    : __WEBUI_TEXT__("shell.sign_in_intro");
  $("loginPassword").focus({ preventScroll: true });
  try {
    const [, data] = await fetchJson("/api/v1/auth", { headers: webUiHeaders() });
    $("loginDevice").textContent = $("loginUser").value = data.deviceName || "";
    // The login was turned off meanwhile, or this is the setup network.
    if (!data.loginRequired) {
      overlay.classList.remove("isLogin");
      claimWebUiOwnership();
    }
  } catch (_) {}
}
{
  const form = $("webUiLogin");
  if (form) {
    form.onsubmit = async (e) => {
      e.preventDefault();
      const password = $("loginPassword").value,
        submit = $("loginSubmit");
      if (!password) {
        setSignInError(__WEBUI_TEXT__("admin.device_password_is_required"));
        return;
      }
      submit.disabled = true;
      setSignInError("");
      try {
        const [response, json] = await fetchJson("/api/v1/auth/login", {
            method: "POST",
            headers: { [WEB_UI_CLIENT_HEADER]: webUiClientId, "Content-Type": "application/json" },
            body: body({ password, remember: $("loginRemember").checked, device: deviceLabel() }),
          }),
          data = json || {};
        if (data.error === "AUTH_NOT_REQUIRED") {
          $("webUiInactive").classList.remove("isLogin");
          return claimWebUiOwnership();
        }
        if (!response.ok) {
          setSignInError(
            data.error === "AUTH_COOLDOWN"
              ? __WEBUI_TEXT__("runtime.too_many_sign_in_attempts")
              : data.error === "DEVICE_PASSWORD_INVALID"
                ? __WEBUI_TEXT__("runtime.wrong_device_password")
                : data.message || __WEBUI_TEXT__("runtime.unable_to_reload"),
          );
          return;
        }
        storeSession(data.token, !!data.remember);
        $("loginPassword").value = "";
        $("webUiInactive").classList.remove("isLogin");
        await claimWebUiOwnership(data.active ? data : undefined);
      } catch (_) {
        setSignInError(__WEBUI_TEXT__("runtime.device_unreachable"));
      } finally {
        submit.disabled = false;
      }
    };
    $("loginShow").onclick = () => {
      const input = $("loginPassword"),
        reveal = input.type === "password";
      input.type = reveal ? "text" : "password";
      $("loginShow").textContent = reveal
        ? __WEBUI_TEXT__("shell.hide")
        : __WEBUI_TEXT__("shell.show");
      $("loginShow").setAttribute("aria-pressed", String(reveal));
    };
    $("loginForgot").onclick = () => {
      const help = $("loginHelp");
      help.hidden = !help.hidden;
      $("loginForgot").setAttribute("aria-expanded", String(!help.hidden));
    };
  }
}
// A pending Home Assistant pairing request, shown on every view while this
// window holds control; the code matches the one Home Assistant shows.
let pairShownId = 0,
  pairUntil = 0,
  pairTimer = 0;
function paintPairExpiry() {
  const left = Math.max(0, Math.ceil((pairUntil - Date.now()) / 1000));
  $("pairExpires").textContent =
    __WEBUI_TEXT__("runtime.pairing_expires_in") + Math.floor(left / 60) + ":" + pad2(left % 60);
}
function applyPairingPrompt(access) {
  const dialog = $("pairDialog");
  if (!dialog) return;
  const id = webUiOwner && access && access.login ? access.pairId || 0 : 0;
  if (!id) {
    if (dialog.open) dialog.close();
    clearInterval(pairTimer);
    pairShownId = 0;
    return;
  }
  if (id === pairShownId) return;
  pairShownId = id;
  pairUntil = Date.now() + (access.pairRemainingS || 0) * 1000;
  $("pairCode").replaceChildren(
    ...String(access.pairCode || "")
      .split("")
      .map((digit) => {
        const cell = document.createElement("span");
        cell.textContent = digit;
        return cell;
      }),
  );
  paintPairExpiry();
  clearInterval(pairTimer);
  pairTimer = setInterval(paintPairExpiry, 1000);
  if (!dialog.open) dialog.showModal();
}
async function decidePairing(allow) {
  const id = pairShownId;
  $("pairDialog").close();
  clearInterval(pairTimer);
  try {
    await api("/api/v1/auth/pairing", { method: "POST", body: body({ id, allow }) });
    message(
      allow
        ? __WEBUI_TEXT__("runtime.home_assistant_connected")
        : __WEBUI_TEXT__("runtime.home_assistant_denied"),
      "ok",
    );
  } catch (e) {
    message(
      formatCommandError(__WEBUI_TEXT__("runtime.could_not_answer_home_assistant"), e),
      "error",
    );
  }
}
// The device forgets this session; the reload then shows the sign-in page.
{
  const out = $("footerSignOut");
  if (out)
    out.onclick = async () => {
      if (
        !(await askConfirm(
          __WEBUI_TEXT__("runtime.sign_out_confirm"),
          __WEBUI_TEXT__("shell.sign_out"),
        ))
      )
        return;
      await api("/api/v1/auth/logout", { method: "POST", body: "{}" }).catch(() => {});
      storeSession("", false);
      location.reload();
    };
}
{
  const allow = $("pairAllow"),
    deny = $("pairDeny");
  if (allow) allow.onclick = () => decidePairing(true);
  if (deny) deny.onclick = () => decidePairing(false);
  // Escape only hides this request; the countdown stops with it.
  if (allow) $("pairDialog").oncancel = () => clearInterval(pairTimer);
}
function body(values) {
  return JSON.stringify(values);
}
function number(id) {
  return Number($(id).value);
}
function sToMs(id) {
  return Math.round(number(id) * 1000);
}
// Home frames call this several times a second: the control sweep runs only
// when mutability or ownership changed, or when a newly mounted view asks.
let sweptMutable = null;
function setMutable(enabled, mounted) {
  const canEdit = !!enabled && webUiOwner;
  const was = controlsMutable;
  controlsMutable = canEdit;
  if (!mounted && sweptMutable === canEdit + "/" + webUiOwner) return;
  sweptMutable = canEdit + "/" + webUiOwner;
  if (was !== canEdit) renderHomePresetAccordion();
  document.querySelectorAll("#app input,#app select,#app button").forEach((e) => {
    if (e.closest("#message")) return;
    if (e.classList.contains("textLock")) {
      e.disabled = !webUiOwner;
      return;
    }
    if (e.id === "shotFsClose") return;
    if (
      e.classList.contains("taskProfilerCtl") ||
      e.classList.contains("scaleProfileCtl") ||
      e.closest("#auditPanel>.btnBar")
    ) {
      // Data-painted controls: the diagnostic stream recomputes their
      // enabled state (ownership plus data preconditions such as a
      // non-empty audit log), so a mutability sweep may only disable them.
      if (!webUiOwner) e.disabled = true;
      return;
    }
    if (e.classList.contains("diagCtl")) {
      e.disabled = !webUiOwner;
      return;
    }
    if (e.closest("#adminLockPanel,#uiOverridePanel")) {
      e.disabled = !webUiOwner;
      return;
    }
    if (e.dataset.dirty != null) {
      e.disabled = !canEdit || e.dataset.dirty != 1;
      return;
    }
    if (!webUiOwner || !e.closest("#actionsPanel")) e.disabled = !canEdit;
  });
  [
    "workflowPanel",
    "dateTimePanel",
    "frontendPanel",
    "networkPanel",
    "accessPanel",
    "firmwareModePanel",
    "blePanel",
    "powerPanel",
  ].forEach((id) => {
    const el = $(id);
    if (el) el.classList.toggle("locked", !canEdit);
  });
  if ($("staPassword")) updateNetworkPasswordState();
  if ($("staIpMode")) updateStaticIpFieldsState();
  if ($("autoTare")) updateConfigGroups();
  updatePresetActionButtons();
  updateWifiSleepState();
  updateTimezoneControls();
  if (was !== canEdit && activeView === "stats" && $("shotRows")) renderShots();
}
const BREW_CONTROL_IDS = [
  "brewByWeight",
  "doseG",
  "goalWeightG",
  "operationalWallS",
  "bbwProtectionS",
  "fastExtractionGuardEnabled",
  "fastBeforeS",
  "fastMarginG",
  "slowExtractionGuardEnabled",
  "slowAfterS",
  "slowMarginG",
  "grindHintsEnabled",
  "scaleLossFixedS",
  "lineaMicraBrewTargetC",
];
function settingsSectionOf(id) {
  if (BREW_CONTROL_IDS.includes(id)) return "brew";
  return id.startsWith("lineaMicra") ? "micra" : "config";
}
function settingsSectionEls(section) {
  return [
    ...document.querySelectorAll(
      "#workflowPanel input,#workflowPanel select,#workflowPanel textarea",
    ),
  ].filter((el) => el.id && settingsSectionOf(el.id) === section);
}
function snapshotControls(els) {
  const s = {};
  els.forEach((el) => {
    if (el.id) s[el.id] = el.type === "checkbox" ? el.checked : el.value;
  });
  return s;
}
function restoreSnapshot(s) {
  if (!s) return;
  for (const id in s) {
    const el = $(id);
    if (!el) continue;
    if (el.type === "checkbox") el.checked = s[id];
    else el.value = s[id];
  }
}
function updateScalePreferenceOptions() {
  const sel = $("scalePreference");
  if (!sel) return;
  [...sel.options].forEach((o) => {
    o.disabled = !controlsMutable;
  });
  sel.disabled = !controlsMutable;
  const bootstrap = sel.value === "prefer" || sel.value === "only",
    preferred = $("preferredScaleSelect"),
    empty = preferred && [...preferred.options].find((o) => o.value === ""),
    hint = $("preferredScaleBootstrapHint");
  if (empty)
    empty.textContent = bootstrap
      ? __WEBUI_TEXT__("runtime.first_detected")
      : __WEBUI_TEXT__("runtime.no_preferred");
  if (hint)
    hint.textContent = bootstrap
      ? __WEBUI_TEXT__("runtime.first_detected_is_shown_until_a_compatible")
      : __WEBUI_TEXT__("runtime.first_available_does_not_save_a_preferred");
}
function clearBrewDirty() {
  uiState.brewDirty = false;
}
// Live brew warnings: the save validator's verdict, rendered in yellow while
// editing instead of red after Save. Same rules, same edited-field
// attribution — only the presentation differs, so the preview can never
// drift from what Save would reject.

// Re-point an input's aria pairing at whichever message smalls remain in its
// label — yellow survivors after a red clear, red survivors after a yellow
// clear, or nothing (attributes removed).
function refreshFieldAria(el) {
  const warnIds = [
    ...(el.closest("label") || el.parentElement).querySelectorAll(
      "small.fieldWarning,small.fieldError",
    ),
  ].map((s) => s.id);
  if (warnIds.length) {
    el.setAttribute("aria-invalid", "true");
    el.setAttribute("aria-describedby", warnIds.join(" "));
  } else {
    el.removeAttribute("aria-invalid");
    el.removeAttribute("aria-describedby");
  }
}
function clearBrewWarningsDom() {
  // Quick-override notes share the look but follow the status, not the form.
  document
    .querySelectorAll("#workflowPanel .fieldWarning:not(.qsOverride)")
    .forEach((e) => e.remove());
  document.querySelectorAll("#workflowPanel .fieldWarn").forEach((e) => {
    e.classList.remove("fieldWarn");
    refreshFieldAria(e);
  });
  $("brewConflictChip")?.classList.add("hidden");
}
function clearBrewWarnings() {
  uiState.brewWarnSig = "";
  clearBrewWarningsDom();
}
function invalidateSettingsHydration() {
  configLoaded = false;
  formRev = 0;
}
let bbwFormPresetId = 0,
  dripFieldsPresetId = 0;
function updateBbwControls() {
  if (!$("brewByWeight")) return;
  const on = !!$("brewByWeight").checked;
  document.querySelectorAll(".bbwLearning").forEach((el) => {
    const hidden = !on;
    el.classList.toggle("hidden", hidden);
    el.querySelectorAll("input,button,select").forEach(
      (input) => (input.disabled = hidden || !controlsMutable),
    );
  });
  const c =
    uiState.bbwReadback && uiState.bbwReadback.bbwPresetId === bbwFormPresetId
      ? uiState.bbwReadback
      : null;
  const use = c && c.bbwEwmaOffsetG;
  $("learnedOffsetG").textContent = typeof use === "number" ? use.toFixed(2) : "—";
  renderSettings(c);
  if (c && c.bbwPresetId !== dripFieldsPresetId) {
    dripFieldsPresetId = c.bbwPresetId;
    for (const id of ["dripSetG", "alphaSet"]) delete $(id).dataset.edited;
  }
  // The set fields follow the values in use until the owner edits them.
  for (const [id, v] of [
    ["dripSetG", use],
    ["alphaSet", c && c.bbwAlpha],
  ])
    if (typeof v === "number" && document.activeElement !== $(id) && !$(id).dataset.edited)
      $(id).value = v.toFixed(2);
  const blocked = !on || !controlsMutable || uiState.brewDirty || !c;
  for (const [id, button] of [
    ["dripSetG", "dripSetButton"],
    ["alphaSet", "alphaSetButton"],
  ])
    $(button).disabled = blocked || $(id).value.trim() === "" || !$(id).validity.valid;
}
function soundAlertsAreOn() {
  return !!$("soundAlertsEnabled")?.checked;
}
function updateBuzzerAlertVisibility(s) {
  document.querySelectorAll(".buzzerOpt").forEach((e) => {
    e.classList.toggle("hidden", !s);
    e.querySelectorAll("input,button,select").forEach((i) => {
      i.disabled = !s || !controlsMutable || !soundAlertsAreOn();
    });
  });
  updateScaleIncapableAlertControls(s);
}
function updateScaleIncapableAlertControls(buzzerOn) {
  const el = $("alertOutputChannel");
  const scaleOnly = !!buzzerOn && el && el.value === "scale_only";
  document.querySelectorAll(".scaleIncapableOpt").forEach((e) => {
    e.classList.toggle("fieldOff", scaleOnly);
    e.querySelectorAll("input,select").forEach((i) => {
      i.disabled = !buzzerOn || scaleOnly || !controlsMutable || !soundAlertsAreOn();
    });
  });
}
function updateBullseyeControls() {
  const wrap = document.querySelector(".bullseyeOpt"),
    check = $("bullseyeMelodyEnabled"),
    tune = $("bullseyeRtttl"),
    tuneWrap = $("bullseyeTuneWrap");
  if (!wrap || !check || !tune) return;
  const buzzerVisible = !wrap.classList.contains("hidden"),
    available =
      buzzerVisible &&
      controlsMutable &&
      soundAlertsAreOn() &&
      $("alertOutputChannel").value === "buzzer_only";
  wrap.classList.toggle("fieldOff", !available);
  check.disabled = !available;
  tune.readOnly = !available || !check.checked;
  if (tuneWrap) tuneWrap.classList.toggle("fieldOff", tune.readOnly);
}
function updateConfigGroups() {
  if (!$("autoTare")) return;
  updateScalePreferenceOptions();
  const alertBox = document.querySelector(".soundAlertOpt"),
    alertsOff = !soundAlertsAreOn();
  if (alertBox) {
    alertBox.classList.toggle("fieldOff", alertsOff);
    alertBox.querySelectorAll("input,select").forEach((i) => {
      i.disabled = !controlsMutable || alertsOff;
    });
  }
  const wrap = $("alertOutputChannel") && $("alertOutputChannel").closest(".buzzerOpt");
  const buzzerVisible = !!wrap && !wrap.classList.contains("hidden");
  updateScaleIncapableAlertControls(buzzerVisible);
  const apply = (cls, off) => {
    document.querySelectorAll("." + cls).forEach((e) => {
      e.classList.toggle("fieldOff", off);
      e.querySelectorAll("input,select").forEach((i) => {
        if (controlsMutable) i.disabled = off || (alertsOff && !!e.closest(".soundAlertOpt"));
      });
    });
  };
  apply("retareOpt", !$("autoRetare").checked);
  apply("tareOpt", !$("autoTare").checked);
  apply("paddleOpt", $("paddleReturnReminder")?.value === "0");
  apply("fastGuardOpt", !$("fastExtractionGuardEnabled").checked);
  apply("touchStopOpt", !$("brewByWeight").checked || !$("avoidAccidentalTouchEnabled").checked);
  apply("slowGuardOpt", !$("slowExtractionGuardEnabled").checked);
  apply("noScaleGuardOpt", $("noScaleBbwMode").value === "off");
  apply("cupProtectOpt", !$("cupProtectionEnabled") || !$("cupProtectionEnabled").checked);
  updateBullseyeControls();
  updateBbwControls();
}
function extRate(v) {
  return ["disabled", "slow", "medium", "fast", "rapid"].includes(v) ? v : "fast";
}
function formNumber(id) {
  const el = $(id);
  if (!el) return NaN;
  const v = Number(el.value);
  return Number.isFinite(v) ? v : NaN;
}
const sub = (t, vals) => t.replace(/\{(\d)\}/g, (_, i) => vals[+i]);
function updateStaticIpFieldsState() {
  if (!$("staIpMode")) return;
  const on = $("staIpMode").value === "static";
  document.querySelectorAll(".staticIpOpt").forEach((e) => {
    e.classList.toggle("fieldOff", !on);
    e.querySelectorAll("input").forEach((i) => {
      i.disabled = !controlsMutable || !on;
    });
  });
}
function updateStateTone(s) {
  const sr = $("scaleRow");
  if (sr) sr.classList.toggle("lampBad", !(s.scale && s.scale.available));
  const cr = $("cupRow");
  if (cr) {
    // Mirrors cupStartGuardWouldBlock(): red only when the firmware refuses
    // the shot, amber when a shot can start without a confirmed cup.
    const cp = s.cupPresence || {},
      k = s.config || {},
      scale = !!(s.scale && s.scale.available),
      present = scale && (cp.state === "PRESENT" || !!cp.present),
      blocks =
        scale &&
        !present &&
        !!k.cupProtectionEnabled &&
        !!k.requireCupToStart &&
        k.brewByWeight !== false;
    cr.classList.toggle("lampBad", blocks);
    cr.classList.toggle("lampWarn", !present && !blocks);
  }
}
function updateNetworkPasswordState() {
  if (!$("staPassword")) return;
  $("staPassword").disabled = !controlsMutable || $("staOpen").checked;
}
function updateWifiSleepState() {
  const el = $("staWifiSleep");
  if (el) el.disabled = !controlsMutable || !uiState.savedStaSsid;
}

// rev counts real preset changes, so renderers compare a number instead of
// re-serializing every preset on each stream frame.
let presetState = { activeId: 0, items: [], selectedId: 0, rev: 0 },
  presetsLoaded = false,
  presetsSeen = null,
  presetsJson = "",
  presetCardsSig = "";
function presetSummary(p) {
  const n = (v) => (Number.isFinite(+v) ? +(+v).toFixed(1) : "?");
  return sub(__WEBUI_TEXT__("runtime.preset_meta"), [
    n(p.doseG),
    n(p.goalWeightG),
    brewRatio(p.goalWeightG, p.doseG),
    n(p.fastBeforeS),
    n(p.slowAfterS),
  ]);
}
// Yield over dose, "2.0" for 36 g from 18 g; "?" while either is unknown.
function brewRatio(yieldG, doseG) {
  return yieldG > 0 && doseG > 0 ? (yieldG / doseG).toFixed(1) : "?";
}
function presetBadge(p) {
  if (!p.brewByWeight) return __WEBUI_TEXT__("runtime.manual");
  return p.fastExtractionGuardEnabled || p.slowExtractionGuardEnabled
    ? __WEBUI_TEXT__("runtime.badge_adaptive")
    : __WEBUI_TEXT__("runtime.badge_fixed");
}
function presetCardDot() {
  const dot = document.createElement("span");
  dot.className = "presetCardDot";
  dot.setAttribute("aria-hidden", "true");
  return dot;
}
function renderPresetCards(targetId) {
  const el = $(targetId);
  const sig = [presetState.rev, presetState.activeId, presetState.selectedId].join();
  // An inline rename in progress owns its card until it finishes.
  if (!el || el.querySelector(".presetRenameInline") || sig === presetCardsSig) return;
  presetCardsSig = sig;
  el.replaceChildren();
  for (const p of presetState.items) {
    const card = document.createElement("article");
    card.className =
      "presetCard" +
      (p.id === presetState.activeId ? " active" : "") +
      (p.id === presetState.selectedId ? " selected" : "");
    card.dataset.id = String(p.id);
    card.appendChild(presetCardDot());
    const titleRow = document.createElement("div");
    titleRow.className = "presetCardTitleRow";
    const title = document.createElement("button");
    title.type = "button";
    title.className = "presetCardTitle";
    title.textContent = p.name;
    title.title = __WEBUI_TEXT__("runtime.rename");
    title.onclick = (e) => {
      e.stopPropagation();
      startRenamePreset(p, title);
    };
    const badge = document.createElement("span");
    badge.className = "presetCardBadge";
    badge.textContent = presetBadge(p);
    titleRow.appendChild(title);
    titleRow.appendChild(badge);
    const meta = document.createElement("div");
    meta.className = "presetCardMeta";
    meta.textContent = presetSummary(p);
    card.appendChild(titleRow);
    card.appendChild(meta);
    card.onclick = () => applyPreset(p.id);
    // Keyboard users load a card like a click; the title keeps its own rename.
    card.tabIndex = 0;
    card.onkeydown = (e) => {
      if (e.target === card && (e.key === "Enter" || e.key === " ")) {
        e.preventDefault();
        applyPreset(p.id);
      }
    };
    el.appendChild(card);
  }
}
function guardRuleRows(m) {
  const on = !!m && m.mode === "active",
    off = __WEBUI_TEXT__("home.rule_off"),
    f = (v) => (Number.isFinite(v) ? axisLabel(v, "g") : "\u2014");
  return [
    [
      "guardFast",
      __WEBUI_TEXT__("runtime.verdict_fast"),
      on && sub(__WEBUI_TEXT__("home.rule_before"), [m.t1]),
      on && m.fast && sub(__WEBUI_TEXT__("home.rule_fast"), [f(m.ceil)]),
    ],
    [
      "guardBbw",
      __WEBUI_TEXT__("runtime.verdict_on_recipe"),
      on && sub(__WEBUI_TEXT__("home.rule_window"), [m.t1, m.t2]),
      on && f(m.goal),
    ],
    [
      "guardSlow",
      __WEBUI_TEXT__("runtime.verdict_slow"),
      on && sub(__WEBUI_TEXT__("home.rule_after"), [m.t2]),
      on && m.slow && sub(__WEBUI_TEXT__("home.rule_slow"), [f(m.floor)]),
    ],
  ]
    .map(([c, label, when, detail]) => [
      when || label,
      c,
      when ? label + " · " + (detail || off) : off,
    ])
    .map(([n, c, r]) => {
      const row = document.createElement("div");
      row.className = "guardRow " + c;
      const name = document.createElement("span");
      name.className = "guardName";
      const dot = document.createElement("i");
      dot.setAttribute("aria-hidden", "true");
      name.append(dot, n);
      const rule = document.createElement("span");
      rule.className = "guardRule";
      rule.textContent = r;
      row.append(name, rule);
      return row;
    });
}
function renderHomePresetAccordion() {
  const el = $("homePresetAcc");
  if (!el) return;
  const canSelect = controlsMutable,
    openId = homeAccPendingId || presetState.activeId,
    sig = JSON.stringify([openId, canSelect, liveRuleModel, presetState.rev]);
  if (sig === homeAccSig) return;
  homeAccSig = sig;
  const focusIdx = [...el.querySelectorAll(".presetAccHead")].indexOf(document.activeElement),
    have = new Map([...el.children].map((n) => [n.dataset.id, n])),
    nodes = [];
  for (const p of presetState.items) {
    const active = p.id === openId,
      key = String(p.id);
    let item = have.get(key);
    if (!item) {
      item = document.createElement("div");
      item.className = "presetAccItem";
      item.dataset.id = key;
      const head = document.createElement("button");
      head.type = "button";
      head.className = "presetAccHead";
      const dot = document.createElement("span");
      dot.className = "presetAccDot";
      dot.setAttribute("aria-hidden", "true");
      const name = document.createElement("span");
      name.className = "presetAccName";
      const badge = document.createElement("span");
      badge.className = "presetAccBadge";
      head.append(dot, name, badge);
      const panel = document.createElement("div");
      panel.className = "presetAccPanel";
      const panelIn = document.createElement("div");
      panelIn.className = "presetAccPanelIn";
      const rows = document.createElement("div");
      rows.className = "presetAccRows";
      rows.setAttribute("aria-live", "polite");
      const graph = document.createElement("div");
      graph.className = "presetAccGraph";
      graph.id = "presetAccGraph" + key;
      const readout = document.createElement("p");
      readout.className = "presetAccReadout";
      const start = document.createElement("p");
      start.className = "presetAccStart";
      // The graph, its readout and pace note stay folded under a chevron.
      const t = escHtml(__WEBUI_TEXT__("runtime.preset_graph"));
      panelIn.innerHTML = `<div class="presetAccMore"><div></div></div><button type="button" class="presetAccToggle" title="${t}" aria-label="${t}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></button>`;
      panelIn.firstChild.firstChild.append(graph, readout, start);
      panelIn.prepend(rows);
      panelIn.lastChild.onclick = (e) =>
        (e.currentTarget.ariaExpanded = item.classList.toggle("full")) && graph._draw();
      panel.appendChild(panelIn);
      item.append(head, panel);
      el.appendChild(item);
    }
    const head = item.firstChild;
    item.classList.toggle("open", active);
    if (!active) item.classList.remove("full");
    item.querySelector(".presetAccToggle").ariaExpanded = item.classList.contains("full");
    head.setAttribute("aria-expanded", active);
    head.disabled = !active && !canSelect;
    head.classList.toggle("locked", head.disabled);
    head.children[1].textContent = p.name;
    head.children[2].textContent = presetBadge(p);
    head.onclick = () => {
      if (p.id !== openId) applyPreset(p.id);
    };
    const saved = buildRuleChartModel(p),
      m =
        p.id === presetState.activeId && liveRuleModel && liveRuleModel.mode !== "empty"
          ? liveRuleModel
          : saved,
      // Past arrivals, as in Settings, while the target is the saved one they were judged against.
      arrivals = m.goal === saved.goal ? p.arrivalsS || [] : [];
    const rows = guardRuleRows(m);
    // The active preset's target opens the quick-weight sheet.
    if (p.id === presetState.activeId && m.mode === "active") {
      const pen = document.createElement("button");
      pen.type = "button";
      pen.className = "qeditPen presetAccEdit";
      pen.title = __WEBUI_TEXT__("runtime.change_target_yield");
      pen.setAttribute("aria-label", pen.title);
      pen.innerHTML =
        '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 20h4L19 9l-4-4L4 16v4zM13.5 6.5l4 4"/></svg>';
      rows[1].lastChild.append(pen);
    }
    item.lastChild.firstChild.firstChild.replaceChildren(...rows);
    const graph = item.querySelector(".presetAccGraph");
    graph._m = m;
    item.querySelector(".presetAccStart").textContent = m.mode === "empty" ? "" : paceStartNote(m);
    graph._draw = () => {
      if (graph._m.mode === "empty") graph.replaceChildren();
      else
        drawPresetGraph(
          graph,
          graph._m,
          { arrivals },
          item.querySelector(".presetAccReadout"),
          key,
        );
    };
    // Only an unfolded graph is drawn: every status poll re-renders the list,
    // and drawing hidden graphs stalled the open and close animations.
    if (item.classList.contains("full")) graph._draw();
    nodes.push(item);
  }
  if (![...el.children].every((n, i) => n === nodes[i])) el.replaceChildren(...nodes);
  if (focusIdx >= 0 && !el.contains(document.activeElement)) {
    const heads = el.querySelectorAll(".presetAccHead");
    if (heads[focusIdx]) heads[focusIdx].focus();
  }
}
function updateActiveBrewProfileHint() {
  const el = $("activeBrewProfileHint");
  if (!el) return;
  const p = presetState.items.find((x) => x.id === presetState.activeId);
  el.textContent = __WEBUI_TEXT__("runtime.current_preset") + (p && p.name ? p.name : "—");
}
function renderAllPresetUi() {
  renderPresetCards("presetCards");
  settingsForm.renderPresetReview();
  renderHomePresetAccordion();
  updatePresetActionButtons();
  updateActiveBrewProfileHint();
}
// Home writes reuse the Settings brew form: its pending autosave goes first,
// and a form that cannot be saved drops the Home change.
async function cleanBrewForm() {
  if (uiState.brewDirty && !(await uiState.flushSettings())) return false;
  await ensureSettingsHydrated();
  return true;
}
// The Home quick-weight sheet saves through the streamed Settings form.
async function ensureSettingsHydrated() {
  if (configLoaded) return;
  await ensureSettingsDom();
  const s = await loadSettingsStatus();
  if (!s) throw new Error(__WEBUI_TEXT__("runtime.invalid_status"));
  loadSettingsConfig(s);
}
function withBaseRev(p) {
  if (configRevision) p.baseRevision = configRevision;
  return p;
}
function recipeBrewByWeight() {
  const active = presetState.items.find((x) => x.id === presetState.activeId);
  return active ? !!active.brewByWeight : true;
}
function selectedPreset() {
  return presetState.items.find((x) => x.id === (presetState.selectedId || presetState.activeId));
}
function updatePresetActionButtons() {
  const p = selectedPreset(),
    factory = !!p?.isFactory,
    locked = !controlsMutable || !presetsLoaded;
  document.querySelectorAll(".presetActions button").forEach((el) => {
    el.disabled =
      locked ||
      (el.id === "presetDeleteBtn" && (!p || factory)) ||
      (el.id === "presetResetBtn" && !factory) ||
      (el.id === "presetNewCoffeeBtn" && !p?.shotStats?.total);
  });
  $("presetResetBtn")?.classList.toggle("hidden", !factory);
  $("presetDeleteBtn")?.classList.toggle("hidden", factory);
}
async function applyPreset(id) {
  id = +id;
  if (!id) return;
  if (id === presetState.activeId) {
    presetState.selectedId = id;
    renderAllPresetUi();
    return;
  }
  if (uiState.brewDirty && !(await uiState.flushSettings())) {
    presetState.selectedId = presetState.activeId;
    renderAllPresetUi();
    return;
  }
  presetState.selectedId = id;
  configLoaded = false;
  formRev = 0;
  const seq = ++homeAccApplySeq;
  homeAccPendingId = id;
  renderAllPresetUi();
  await command("/api/v1/presets", { action: "apply", id });
  if (seq === homeAccApplySeq) {
    homeAccPendingId = 0;
    renderHomePresetAccordion();
  }
}
// Saves a patch (only the changed recipe fields) onto the form's preset.
async function saveBrewPreset(patch, okMsg, failMsg) {
  const ok = okMsg ?? __WEBUI_TEXT__("runtime.preset_saved"),
    fail = failMsg || __WEBUI_TEXT__("runtime.could_not_save_preset");
  try {
    await ensureSettingsHydrated();
  } catch (e) {
    message(formatCommandError(fail, e), "error");
    return false;
  }
  const errs = settingsForm.validateBrewClient();
  if (errs.length) {
    // The red report replaces the yellow preview (same sentences): drop the
    // amber marks so each verdict reads exactly once until the next edit.
    clearBrewWarnings();
    showFieldErrors(errs);
    return false;
  }
  // A new window or a target moved by more than 1 g restarts the preset's
  // statistics: the notice follows the save.
  const saved = presetState.items.find((x) => x.id === bbwFormPresetId),
    restarts =
      saved &&
      (("fastBeforeS" in patch && patch.fastBeforeS !== saved.fastBeforeS) ||
        ("slowAfterS" in patch && patch.slowAfterS !== saved.slowAfterS) ||
        ("goalWeightG" in patch && Math.abs(patch.goalWeightG - saved.goalWeightG) > 1));
  clearFieldErrors();
  try {
    await command(
      "/api/v1/presets",
      { action: "save", id: bbwFormPresetId, ...patch },
      1,
      ok,
      fail,
    );
    if (restarts) message(__WEBUI_TEXT__("runtime.stats_reset_confirm"), "ok");
    clearBrewWarnings();
    return true;
  } catch (e) {
    message(formatCommandError(fail, e), "error");
    await refreshStatus();
    return false;
  }
}
function commitRenamePreset(p, name) {
  const t = (name || "").trim();
  if (!t) {
    message(__WEBUI_TEXT__("runtime.name_is_required"), "error");
    return;
  }
  if (presetState.items.some((x) => x.id !== p.id && x.name === t)) {
    message(__WEBUI_TEXT__("runtime.name_already_in_use"), "error");
    return;
  }
  command("/api/v1/presets", { action: "rename", id: p.id, name: t });
}
function startRenamePreset(p, titleEl) {
  const isNarrow = window.matchMedia("(max-width:640px)").matches;
  if (isNarrow && $("presetRenameDialog")) {
    $("presetRenameInput").value = p.name;
    $("presetRenameDialog").showModal();
    $("presetRenameForm").onsubmit = async (ev) => {
      ev.preventDefault();
      if (ev.submitter && ev.submitter.value === "cancel") {
        $("presetRenameDialog").close();
        return;
      }
      commitRenamePreset(p, $("presetRenameInput").value);
      $("presetRenameDialog").close();
    };
    return;
  }
  if (!titleEl) {
    const name = prompt(__WEBUI_TEXT__("runtime.rename_preset_2"), p.name);
    if (name === null) return;
    commitRenamePreset(p, name);
    return;
  }
  const input = document.createElement("input");
  input.type = "text";
  input.className = "presetRenameInline";
  input.setAttribute("aria-label", __WEBUI_TEXT__("runtime.preset_name"));
  input.maxLength = 23;
  input.value = p.name;
  // Removing the input blurs it, so Enter or Escape must not also commit on blur.
  let done = false;
  const finish = async (ok) => {
    if (done) return;
    done = true;
    const v = input.value;
    if (input.parentNode) input.replaceWith(titleEl);
    if (ok) commitRenamePreset(p, v);
    else renderAllPresetUi();
  };
  titleEl.replaceWith(input);
  input.focus();
  input.select();
  input.onkeydown = (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      finish(true);
    }
    if (e.key === "Escape") {
      e.preventDefault();
      finish(false);
    }
  };
  input.onblur = () => finish(true);
}
function ingestPresets(s) {
  if (!s.presets) return;
  presetsLoaded = true;
  if (s.presets !== presetsSeen) {
    presetsSeen = s.presets;
    const json = JSON.stringify(s.presets);
    if (json !== presetsJson) {
      presetsJson = json;
      presetState.rev++;
    }
  }
  const prev = presetState.activeId;
  presetState.activeId = s.presets.activeId || 0;
  presetState.items = s.presets.items || [];
  presetState.cutRampS = s.presets.cutRampS;
  if (
    !presetState.activeId ||
    !presetState.items.some((x) => x.id === presetState.selectedId) ||
    (prev && prev !== presetState.activeId)
  )
    presetState.selectedId = presetState.activeId;
  if (prev && prev !== presetState.activeId) {
    configLoaded = false;
    formRev = 0;
    clearBrewDirty();
    liveRuleModel = null;
  }
  renderAllPresetUi();
  if ($("brewByWeight") && !uiState.brewDirty) $("brewByWeight").checked = recipeBrewByWeight();
  const active = presetState.items.find((x) => x.id === presetState.activeId);
  if (
    $("lineaMicraBrewTargetC") &&
    !uiState.brewDirty &&
    active &&
    typeof active.lineaMicraBrewTargetC === "number"
  )
    $("lineaMicraBrewTargetC").value = active.lineaMicraBrewTargetC.toFixed(1);
}

// Cut line f(t), piecewise linear and non-increasing: the ceiling holds until
// D s before the fast threshold, ramps to the target, holds through the
// window, then ramps over D s to the floor. A side that is off is the target.
function buildRuleChartModel(c) {
  if (!c) return { mode: "empty" };
  const N = (v) => (Number.isFinite(+v) ? +v : NaN),
    goal = N(c.goalWeightG),
    t1 = N(c.fastBeforeS),
    t2 = N(c.slowAfterS),
    prot = N(c.bbwProtectionMs) / 1e3,
    wall = N(c.operationalWallMs) / 1e3,
    d = N(c.cutRampS ?? presetState.cutRampS),
    fast = !!c.fastExtractionGuardEnabled,
    slow = !!c.slowExtractionGuardEnabled,
    fastMargin = N(c.fastMarginG),
    slowMargin = N(c.slowMarginG),
    ceil = fast ? goal + fastMargin : goal,
    floor = slow ? goal - slowMargin : goal,
    // Paces start at the usual first drop, kept 0.5 s before the window.
    start = Math.max(0, Math.min(N(c.firstDropS) || 0, t1 - 0.5));
  if (
    ![goal, t1, t2, prot, wall, d, ceil, floor].every(Number.isFinite) ||
    !(prot < t1 && t1 < t2 && t2 < wall)
  )
    return { mode: "empty" };
  return {
    mode: c.brewByWeight === false ? "timerOnly" : "active",
    fast,
    slow,
    fastMargin,
    slowMargin,
    goal,
    ceil,
    floor,
    t1,
    t2,
    prot,
    wall,
    d,
    firstDrop: start,
    firstDropN: N(c.firstDropN) || 0,
    scrubMin: Math.max(prot, start + 0.5),
    pts: [
      [prot, ceil],
      [Math.max(prot, t1 - d), ceil],
      [t1, goal],
      [t2, goal],
      [Math.min(wall, t2 + d), floor],
      [wall, floor],
    ],
  };
}
// Cut weight (g) at time t (s), in the firmware's integer units (dg, ms):
// each ramp is a smooth S-curve between the held ceiling or floor and the
// target, which stays a straight line through the window. Products stay
// below 2^53, so Number math is exact.
function cutLine(t, m) {
  const ms = (v) => Math.round(v * 1000),
    dg = (v) => Math.round(v * 10),
    now = ms(Math.max(m.prot, Math.min(m.wall, t))),
    goal = dg(m.goal),
    a = ms(Math.max(m.prot, m.t1 - m.d)),
    t1 = ms(m.t1),
    t2 = ms(m.t2),
    b = ms(Math.min(m.wall, m.t2 + m.d));
  if (now < t1) {
    const up = m.fast ? dg(m.fastMargin) : 0,
      n = Math.max(0, now - a),
      d = t1 - a;
    return (goal + Math.floor((up * (d ** 3 - 3 * n * n * d + 2 * n ** 3)) / d ** 3)) / 10;
  }
  if (now > t2) {
    const down = m.slow ? dg(m.slowMargin) : 0,
      n = Math.min(now, b) - t2,
      d = b - t2;
    return (goal - Math.floor((down * (3 * n * n * d - 2 * n ** 3)) / d ** 3)) / 10;
  }
  return goal / 10;
}
// Where the paces start: this preset's first drops, else the others', else 0 s.
function paceStartNote(m) {
  return sub(
    m.firstDropN > 0
      ? __WEBUI_TEXT__("runtime.graph_start_preset")
      : m.firstDrop > 0
        ? __WEBUI_TEXT__("runtime.graph_start_others")
        : __WEBUI_TEXT__("runtime.graph_start_none"),
    [m.firstDrop.toFixed(1)],
  );
}
// Reference pace (illustrative shape): a shot that reaches the target at tArr
// with a soft start. It is the corridor design's quadratic Bézier (control
// point at PACE_SOFT of the target), so it draws exactly as one "Q" segment.
const PACE_SOFT = 0.18;
function referencePace(t, goal, tArr, t0 = 0) {
  const u = Math.max(0, t - t0) / (tArr - t0);
  return goal * (2 * PACE_SOFT * u + (1 - 2 * PACE_SOFT) * u * u);
}
// One preset graph for Settings (review, with scrubber) and for a Stats shot
// (compact, no scrubber, no no-cut hatch; Stats adds 10 s ticks). Laid out in CSS pixels
// from the measured width; null while the host is hidden.
function presetGraph(host, m, x = {}) {
  const {
      ghost,
      arrivals = [],
      last = [],
      drip = [],
      held = [],
      maxG = 0,
      maxT = 0,
      compact,
      tens,
    } = x,
    W = host.clientWidth;
  if (!W) return null;
  const u = host.id,
    H = Math.round(
      compact ? Math.min(180, Math.max(140, W * 0.48)) : Math.min(220, Math.max(180, W * 0.56)),
    ),
    L = 34,
    R = W - 10,
    T = 14,
    B = H - 18,
    top = Math.ceil(Math.max(m.ceil, ghost ? ghost.ceil : 0, maxG) + 2),
    step = top > 30 ? 10 : 5,
    tMax = Math.max(m.wall, ghost ? ghost.wall : 0, maxT),
    X = (t) => L + ((R - L) * t) / tMax,
    Y = (g) => B - ((B - T) * g) / top,
    n = (v) => +(+v).toFixed(1),
    f = (v) => v.toFixed(1),
    // Unrounded pixel coordinates: the curves are drawn exactly, never from
    // decigram-rounded samples (the displayed numbers keep the integer law).
    P = (t, g) => X(t).toFixed(2) + " " + Y(g).toFixed(2),
    label = (x, y, s, cls = "", anchor = "middle") =>
      `<text class="rvLbl ${cls}" x="${f(x)}" y="${f(y)}" text-anchor="${anchor}">${s}</text>`,
    a = Math.max(m.prot, m.t1 - m.d),
    b = Math.min(m.wall, m.t2 + m.d),
    // Holds and the window are straight; each S-ramp is one cubic Bézier with
    // control points (x0 + Δx/3, y0) and (x0 + 2Δx/3, y1): exactly smoothstep.
    ramp = (t0, y0, t1, y1) =>
      `C${P(t0 + (t1 - t0) / 3, y0)} ${P(t0 + (2 * (t1 - t0)) / 3, y1)} ${P(t1, y1)}`,
    edges = (x) => {
      const xa = Math.max(x.prot, x.t1 - x.d),
        xb = Math.min(x.wall, x.t2 + x.d);
      return [
        `L${P(xa, x.ceil)}` + ramp(xa, x.ceil, x.t1, x.goal),
        `L${P(x.t2, x.goal)}`,
        ramp(x.t2, x.goal, xb, x.floor) + `L${P(x.wall, x.floor)}`,
      ];
    },
    [fastEdge, windowEdge, slowEdge] = edges(m),
    // Reference paces as exact quadratic Béziers on [0, end], ending where
    // they leave the plot (or at the right edge).
    t0 = m.firstDrop,
    paceEnd = (ta) => {
      const c = PACE_SOFT,
        k = (top + 1) / m.goal;
      return Math.min(
        tMax,
        t0 + ((ta - t0) * (Math.sqrt(4 * c * c + 4 * (1 - 2 * c) * k) - 2 * c)) / (2 * (1 - 2 * c)),
      );
    },
    ctrl = (ta, end) => P(t0 + (end - t0) / 2, (m.goal * PACE_SOFT * (end - t0)) / (ta - t0)),
    endOf = (ta, end = paceEnd(ta)) => P(end, referencePace(end, m.goal, ta, t0)),
    paceTo = (ta, end = paceEnd(ta)) => `Q${ctrl(ta, end)} ${endOf(ta, end)}`,
    paceBack = (ta, end = paceEnd(ta)) => `Q${ctrl(ta, end)} ${P(t0, 0)}`,
    // Areas run along the baseline to the first drop; strokes start there.
    O = `M${P(0, 0)}L${P(t0, 0)}`,
    S = `M${P(t0, 0)}`,
    corner = `L${P(tMax, top + 1)}`,
    tA = Math.max(t0 + 0.5, m.t1 - m.d),
    tB = m.t2 + m.d,
    band = (cls, d, on) =>
      `<path class="${on ? cls : "rvBandOff"}" d="${d}" fill="url(#${u + cls})"/>`,
    curves = (lines, cls) =>
      lines
        .map(
          (pl) =>
            `<path class="${cls}" d="${pl
              .filter(([t]) => t <= tMax)
              .map(([t, g], i) => (i ? "L" : "M") + P(t, g))
              .join("")}"/>`,
        )
        .join(""),
    shown = arrivals.filter((t) => t <= tMax),
    targetAbove = compact || Y(m.goal) - 4 - T >= 16;
  let marks = "",
    grid = "",
    lastR = -99;
  for (let v = 0; v <= top; v += step) {
    grid += `<path class="rvGrid" d="M${L} ${f(Y(v))}H${R}"/>`;
    marks += label(L - 4, Y(v) + 3, v + __WEBUI_TEXT__("runtime.g_2"), "", "end");
  }
  (tens
    ? Array.from({ length: Math.floor(tMax / 10) + 1 }, (_, i) => i * 10)
    : [0, m.prot, m.t1, m.t2, m.wall]
  ).forEach((t, i, ts) => {
    // Every tick carries its unit. The zero tick starts at the y axis (clear
    // of "0 g"), like the shot charts; a tick that would spill past the SVG
    // ends at its edge; a tick that would overlap the previous one is dropped
    // (never the last). Widths are estimated at ~6.5 px per 11 px glyph.
    const s = n(t) + " s",
      w = s.length * 6.5,
      edge = X(t) + w / 2 > W - 1,
      x0 = edge ? W - 1 - w : i ? X(t) - w / 2 : X(t);
    if (x0 < lastR + 4 && i < ts.length - 1) return;
    lastR = x0 + w;
    if (tens) grid += `<path class="rvGrid" d="M${f(X(t))} ${T}V${B}"/>`;
    marks += label(edge ? W - 1 : X(t), B + 13, s, "", edge ? "end" : i ? "middle" : "start");
  });
  host.innerHTML =
    `<svg viewBox="0 0 ${W} ${H}" width="${W}" height="${H}">` +
    // Each band fades from twice its flat strength at the top to almost nothing at the base.
    `<defs>${[
      ["rvFastFull", "gFast", "Full"],
      ["rvFastPart", "gFast", "Part"],
      ["rvWindow", "ok", "Win"],
      ["rvSlowPart", "gSlow", "Part"],
      ["rvSlowFull", "gSlow", "Full"],
    ]
      .map(([c, k, b]) => fadeFill(u + c, `var(--${k})`, `calc(var(--band${b}) * 2.2)`, 0.03))
      .join(
        "",
      )}<pattern id="${u}Hatch" width="6" height="6" patternUnits="userSpaceOnUse" patternTransform="rotate(45)"><path class="rvHatchLine" d="M0 0V6"/></pattern>` +
    `<clipPath id="${u}Under"><path d="M${P(0, 0)}L${P(0, m.ceil)}L${P(m.prot, m.ceil)}${fastEdge}${windowEdge}${slowEdge}L${P(m.wall, 0)}Z"/></clipPath></defs>` +
    `<g role="img" aria-label="${escHtml(sub(__WEBUI_TEXT__("runtime.graph_summary"), [n(m.prot), n(m.ceil), n(a), n(m.goal), n(m.t1), n(m.t2), n(m.floor), n(b), n(m.wall)]) + " " + __WEBUI_TEXT__("runtime.graph_pace_note") + (compact ? "" : " " + paceStartNote(m)))}">` +
    grid +
    `<g clip-path="url(#${u}Under)">` +
    band("rvFastFull", O + paceTo(tA) + `L${P(paceEnd(tA), top + 1)}L${P(0, top + 1)}Z`, m.fast) +
    band("rvFastPart", O + paceTo(m.t1) + corner + `L${endOf(tA)}` + paceBack(tA) + "Z", m.fast) +
    band(
      "rvWindow",
      O + paceTo(m.t1, m.t1) + `L${P(m.t2, m.goal)}` + paceBack(m.t2, m.t2) + "Z",
      true,
    ) +
    band("rvSlowPart", O + paceTo(m.t2) + corner + `L${endOf(tB)}` + paceBack(tB) + "Z", m.slow) +
    band("rvSlowFull", O + paceTo(tB) + corner + `L${P(tMax, 0)}Z`, m.slow) +
    `<path class="rvPace" d="${[tA, m.t1, m.t2, tB].map((ta) => S + paceTo(ta)).join("")}"><title>${__WEBUI_TEXT__("runtime.graph_pace_title")}</title></path>` +
    "</g>" +
    marks +
    (compact
      ? ""
      : `<rect x="${L}" y="${T}" width="${f(X(m.prot) - L)}" height="${B - T}" fill="url(#${u}Hatch)"/>`) +
    `<path class="rvTarget" d="M${L} ${f(Y(m.goal))}H${R}"/><path class="rvMax" d="M${f(X(m.wall))} ${T}V${B}"/>` +
    // The On recipe window: dotted verticals at its start and end.
    `<path class="rvWin" d="M${f(X(m.t1))} ${T}V${B}M${f(X(m.t2))} ${T}V${B}"/>` +
    (ghost
      ? `<path class="rvGhost" d="M${P(ghost.prot, ghost.ceil)}${edges(ghost).join("")}"/>`
      : "") +
    // One cut line from the weight-cut delay to Max, drawn under the shot data it frames.
    `<path class="rvCut${m.mode === "timerOnly" ? " off" : ""}" d="M${P(m.prot, m.ceil)}${fastEdge}${windowEdge}${slowEdge}"/>` +
    curves(last, "rvLast") +
    curves(drip, "rvLast rvLastDrip") +
    held
      .map(
        ([t, g]) => `<circle cx="${f(X(t))}" cy="${f(Y(g))}" r="2" fill="var(--mu)" opacity=".6"/>`,
      )
      .join("") +
    shown
      .map((t) => `<circle class="rvArrival" cx="${f(X(t))}" cy="${f(Y(m.goal))}" r="3"/>`)
      .join("") +
    // The hatch keeps its inline name only where it fits; the legend names it too.
    (!compact && X(m.prot) - L >= 96
      ? label(
          (L + X(m.prot)) / 2,
          (T + B) / 2,
          __WEBUI_TEXT__("runtime.graph_legend_no_cut"),
          "rvHalo",
        )
      : "") +
    // Below the line when it would run into "Max" at the top (Settings only).
    label(
      R - 2,
      Y(m.goal) + (targetAbove ? -4 : 13),
      sub(__WEBUI_TEXT__("runtime.graph_target"), [n(m.goal)]),
      "rvHalo strong",
      "end",
    ) +
    (compact
      ? "</g></svg>"
      : label(X(m.wall), T - 4, sub(__WEBUI_TEXT__("runtime.graph_max"), [n(m.wall)]), "", "end") +
        `</g><g class="rvScrub" role="slider" tabindex="0" aria-label="${escHtml(__WEBUI_TEXT__("runtime.graph_scrub"))}" aria-valuemin="${m.scrubMin}" aria-valuemax="${m.wall}"><path class="rvGuide"/><path class="rvShot"/><circle class="rvHandle" r="5"/>${PILL}</g></svg>`);
  return {
    m,
    X,
    Y,
    T,
    B,
    pace: (ta, end) => S + paceTo(ta, end),
    tAt: (px) => ((px - L) * tMax) / (R - L),
    arrivals: shown.length,
  };
}
// While held, a pill left of the handle (clear of a right thumb) shows its point.
const PILL =
  '<g class="rvPill"><rect y="-9" rx="9" height="18"/><text class="rvLbl strong" x="8" y="4"/></g>';
// User text bound for innerHTML.
function escHtml(text) {
  return String(text).replace(/[&<>"]/g, (c) => "&#" + c.charCodeAt(0) + ";");
}
// Near the left edge it flips to the right of the handle instead of covering it.
function placePill(pill, text, x, y) {
  pill.lastChild.innerHTML = text;
  const pw = pill.lastChild.getBBox().width + 16;
  pill.firstChild.setAttribute("width", pw);
  pill.setAttribute("transform", `translate(${x - 11 - pw < 1 ? x + 11 : x - 11 - pw} ${y})`);
}
// Every chart cursor scrubs alike: any pointer grabs it at once and drags it
// anywhere on the chart, and arrow keys step it once focused. A vertical swipe
// still scrolls the page (touch-action: pan-y) and the browser cancels the
// pointer. The node is the slider unless a selector names one inside it.
function bindScrub(node, tAt, seek, now, slider) {
  const move = (e) => seek(tAt(e.clientX));
  node.addEventListener("pointerdown", (e) => {
    if (tAt(e.clientX) == null) return;
    e.preventDefault();
    node.setPointerCapture(e.pointerId);
    (slider ? node.querySelector(slider) : node)?.focus({ preventScroll: true });
    node.classList.add("rvHeld");
    move(e);
  });
  node.addEventListener("pointermove", (e) => node.hasPointerCapture(e.pointerId) && move(e));
  node.addEventListener("lostpointercapture", () => node.classList.remove("rvHeld"));
  node.addEventListener("keydown", (e) => {
    const step = {
      ArrowRight: 0.5,
      ArrowUp: 0.5,
      ArrowLeft: -0.5,
      ArrowDown: -0.5,
      PageUp: 5,
      PageDown: -5,
    }[e.key];
    if (!step) return;
    e.preventDefault();
    seek(now() + step);
  });
  if (slider) return;
  node.tabIndex = 0;
  node.setAttribute("role", "slider");
  node.setAttribute("aria-label", __WEBUI_TEXT__("runtime.shot_cursor_label"));
}
// One preset graph with its scrubber, used by the Settings review and by each
// Home preset panel: same drawing, axis, and scrub behavior. The scrub state
// lives on the host, so every graph scrubs on its own.
function drawPresetGraph(host, m, x, readout, key) {
  host._w = host.clientWidth;
  const view = presetGraph(host, m, x);
  if (!view) return null;
  if (host._key !== key || !(host._s >= m.scrubMin && host._s <= m.wall))
    host._s = Math.max(m.scrubMin, Math.round((m.t1 + m.t2) * 5) / 10);
  host._key = key;
  host._view = view;
  host._readout = readout;
  if (!host.dataset.bound) {
    host.dataset.bound = "1";
    observePresetHost(host);
    const scrub = (s) => {
      host._s = Math.max(
        host._view.m.scrubMin,
        Math.min(host._view.m.wall, Math.round(s * 10) / 10),
      );
      positionPresetScrub(host);
    };
    bindScrub(
      host,
      (x) => host._view.tAt(x - host.firstChild.getBoundingClientRect().left),
      scrub,
      () => host._s,
      ".rvScrub",
    );
  }
  positionPresetScrub(host);
  return view;
}
// The scrubber rides the cut line: it picks when a shot is cut, and so the
// weight it stops at. The reference pace through that point (inverting
// referencePace) gives when the shot would have reached the target.
function positionPresetScrub(host) {
  const v = host._view,
    g = host.querySelector(".rvScrub");
  if (!v || !g) return;
  const m = v.m,
    stop = host._s,
    w = cutLine(stop, m),
    c = PACE_SOFT,
    u = (Math.sqrt(4 * c * c + (4 * (1 - 2 * c) * w) / m.goal) - 2 * c) / (2 * (1 - 2 * c)),
    tArr = m.firstDrop + (stop - m.firstDrop) / u,
    verdict =
      stop >= m.wall
        ? __WEBUI_TEXT__("runtime.max_time")
        : verdictLabel(tArr < m.t1 - 0.5 ? "fast" : tArr > m.t2 + 0.5 ? "slow" : "on_recipe"),
    values = [+m.goal.toFixed(1), tArr.toFixed(1), verdict, w.toFixed(1), stop.toFixed(1)],
    text = sub(
      Math.abs(w - m.goal) < 0.05
        ? __WEBUI_TEXT__("runtime.graph_readout_target")
        : __WEBUI_TEXT__("runtime.graph_readout"),
      values,
    ),
    [guide, shot, handle, pill] = g.children,
    hx = v.X(stop);
  guide.setAttribute("d", `M${hx.toFixed(1)} ${v.T}V${v.B}`);
  shot.setAttribute("d", v.pace(tArr, stop));
  handle.setAttribute("cx", hx.toFixed(2));
  handle.setAttribute("cy", v.Y(w).toFixed(2));
  placePill(pill, sub(__WEBUI_TEXT__("runtime.graph_cut_pill"), values), hx, v.Y(w));
  g.setAttribute("aria-valuenow", stop.toFixed(1));
  g.setAttribute("aria-valuetext", text);
  if (host._readout) host._readout.textContent = text;
}
let liveRuleModel = null,
  homeAccSig = "",
  homeAccPendingId = 0,
  homeAccApplySeq = 0;
function updateRuleChartFromStatus(s) {
  liveRuleModel = buildRuleChartModel(
    s &&
      s.config && { ...presetState.items.find((p) => p.id === presetState.activeId), ...s.config },
  );
  renderHomePresetAccordion();
}

function ms(m, n) {
  return m != null ? (m / 1000).toFixed(n) : "—";
}
let homeFrame = null,
  homeStale = true,
  homeReady,
  homeResolve = () => {};
// `kept` is the status a reconnect replaces: a Home snapshot that omits the
// presets the page already holds (same presetsTag) keeps that copy.
function statusStreamFrame(previous, message, kept) {
  // An increment carries only its type and changes; the snapshot's identity continues.
  const snapshot = message.snapshot === true,
    boot = snapshot ? message.boot : previous?.boot;
  if ((snapshot && message.v !== 1) || !Number.isInteger(boot) || !message.changes) throw Error();
  const status = snapshot ? {} : previous.status;
  const keep = kept?.presets && kept.presetsTag === message.changes.presetsTag;
  if (snapshot && keep && !("presets" in message.changes)) status.presets = kept.presets;
  for (const [path, value] of Object.entries(message.changes)) {
    if (
      !/^[a-zA-Z][a-zA-Z0-9]*(\.[a-zA-Z][a-zA-Z0-9]*){0,3}$/.test(path) ||
      /(^|\.)(constructor|prototype)(\.|$)/.test(path) ||
      (typeof value === "number" && !Number.isFinite(value))
    )
      throw Error();
    const parts = path.split(".");
    let target = status;
    for (const key of parts.slice(0, -1)) target = target[key] || (target[key] = {});
    target[parts.at(-1)] = value;
  }
  if (
    message.type === "diagnostic"
      ? snapshot && !diagSnapshotOk(status)
      : message.type === "settings"
        ? snapshot && !settingsSnapshotOk(status)
        : !statusPageOk("home", status) || status.bootId !== boot
  )
    throw Error();
  return { boot, status };
}
function renderHomeStream() {
  if (homeStale || !homeFrame) return;
  const status = homeFrame.status;
  lastStatusAt = Date.now();
  applyCommonStatus(status);
  // Only the Home stream carries the pairing request; Admin's status has its
  // own access summary.
  applyPairingPrompt(status.access);
  const session = $("footerSession");
  if (session) session.hidden = !status.access?.login;
  if (status.snapshotStale) setMutable(false);
  // Frames on any view prove reachability, so the out-of-reach overlay
  // clears even off Home.
  noteReachOk();
  if (activeView !== "home") return;
  viewStatusHandlers.home?.(status);
  hideHomeBoot();
}
function invalidateHomeStream() {
  homeStale = true;
  homeResolve(false);
  setMutable(false);
  updateHeaderSignals();
  if (activeView !== "home") return;
  clearCupWeights();
  for (const id of [
    "scaleWeight",
    "scaleTimer",
    "machineState",
    "state",
    "homeMicraPower",
    "homeMicraBrewBoiler",
    "homeMicraSteamBoiler",
    "homeMicraCleaning",
    "machineRowState",
    "scale",
    "machineRowValue",
    "scaleRowValue",
    "cupRowValue",
  ])
    setHomeSub(id, "—");
  setHomeSub("machineRowHint", "");
  updateHomeAdminActions(false, false);
}
async function loadHomeStatus() {
  startUiStream();
  return homeStale && !(await homeReady) ? null : homeFrame?.status;
}
// Synchronous view for local tickers: null while the stream is invalidated,
// so painters stop repainting stale values over the reset placeholders.
// Whether the controller holds a last shot; null until the shot stream answers.
function shotRecorded() {
  return shotFrame ? !!shotFrame.card.valid : null;
}
function homeStatusCache() {
  return homeStale ? null : homeFrame?.status || null;
}
function formatMicraCleaning(lm) {
  const ws = lm?.websocket || {},
    label = __WEBUI_TEXT__("diagnostic.cleaning_states").split("|")[
      ["inactive", "waiting_for_paddle", "cleaning"].indexOf(ws.cleaning)
    ];
  return !lm?.accountConfigured
    ? __WEBUI_TEXT__("runtime.not_connected")
    : !lm.observeState
      ? __WEBUI_TEXT__("runtime.disabled")
      : lm.connectionType === "api"
        ? __WEBUI_TEXT__("diagnostic.cleaning_api")
        : !ws.cleaningAvailable
          ? __WEBUI_TEXT__("diagnostic.cleaning_no_update")
          : label ||
            __WEBUI_TEXT__("runtime.unknown_4") +
              (ws.cleaningLabel ? " · " + ws.cleaningLabel : "");
}
let shotWs = null,
  shotRetry = 0,
  shotTry = 0,
  shotFrame = null,
  shotSeen = null,
  shotResync = !1,
  shotResyncStrikes = 0,
  shotStale = !0,
  shotPaint = 0,
  shotSetup = 0,
  shotAlive = 0,
  shotDeadline = 0,
  shotGotHome = !1,
  shotGotShot = !1;
function resetUiStream() {
  // A closed socket ends every subscription generation: nothing stays synced
  // until socket.onopen replays the wanted subscriptions.
  eachStream("reset");
  shotStale = true;
  shotResync = false;
  invalidateHomeStream();
}
function stopUiStream() {
  clearTimeout(shotRetry),
    (shotRetry = 0),
    clearTimeout(shotSetup),
    clearTimeout(shotAlive),
    clearTimeout(shotDeadline);
  const e = shotWs;
  (shotWs = null), resetUiStream(), e && e.close(), paintUiStream();
}
function paintUiStream() {
  if (shotStale) cancelShotClockFinish();
  scheduleShotClock();
  if ("home" !== activeView) return;
  const e = $("shotHero");
  if (e) {
    if ((e.setAttribute("aria-busy", String(shotStale)), !shotFrame))
      return (
        (e.hidden = !1),
        setHomeSub("shotHeroState", __WEBUI_TEXT__("home.hero_loading")),
        setHomeSub("shotHeroWeight", "—"),
        ($("shotHeroFlow").hidden = !0),
        void setHomeSub("shotHeroElapsed", "—")
      );
    if (!shotFrame.card.valid)
      return (
        clearShotHero(),
        void (shotFsActive
          ? exitShotFullScreen()
          : shotStale &&
            ((e.hidden = !1), setHomeSub("shotHeroState", __WEBUI_TEXT__("home.hero_stale"))))
      );
    renderShotHero({
      ...shotFrame.card,
      ...shotFrame.curve,
      clockMs: shotClock || shotClockFinish ? shotClockElapsed() : shotFrame.card.elapsedMs,
    }),
      shotStale
        ? setHomeSub("shotHeroState", __WEBUI_TEXT__("home.hero_stale"))
        : "pending" === shotFrame.phase &&
          setHomeSub("shotHeroState", __WEBUI_TEXT__("home.hero_pending"));
  }
}
function scheduleShotPaint() {
  shotPaint ||
    (shotPaint = requestAnimationFrame(() => {
      (shotPaint = 0), paintUiStream();
    }));
}
function uiStreamFrame(e, t) {
  const o = (e, t = 4294967295, s = 0) => Number.isInteger(e) && e >= s && e <= t;
  // An increment carries its sequence, cursor and changes only: identity,
  // staleness and phase continue from the last frame, and its points from
  // the last cursor.
  if (t && !t.snapshot) {
    if (!e) throw Error();
    t = {
      ...t,
      v: e.v,
      boot: e.boot,
      cycle: e.cycle,
      stale: t.stale ?? e.stale,
      phase: t.phase ?? e.phase,
      snapshot: false,
      curveBase: e.cursor,
    };
  } else if (t) t = { ...t, curveBase: 0 };
  const n = t?.snapshot ? t.card : e && t?.card && { ...e.card, ...t.card };
  if (
    !(
      t &&
      1 === t.v &&
      o(t.boot) &&
      o(t.seq) &&
      o(t.cycle) &&
      o(t.cursor, 1201) &&
      o(t.curveBase, 1201) &&
      "boolean" == typeof t.snapshot &&
      ["idle", "active", "pending", "transient"].includes(t.phase) &&
      n &&
      "boolean" == typeof n.valid &&
      "boolean" == typeof n.live &&
      o(n.elapsedMs) &&
      (null === n.weight || Number.isFinite(n.weight)) &&
      (null === n.averageFlowGps || Number.isFinite(n.averageFlowGps)) &&
      (void 0 === n.scaleAvailable || "boolean" == typeof n.scaleAvailable) &&
      [n.firstDropMs, n.tareMs].every((e) => null === e || o(e, 6e4))
    )
  )
    throw Error();
  if (e && t.boot === e.boot && t.seq <= e.seq) return e;
  // A fragment leaves out an index list it has nothing to add to.
  const { wD, wRef, wBreakBefore = [], wHeld = [], ...c } = t.curve || {},
    s = (wRef === undefined ||
      (Array.isArray(wRef) && wRef.length === 2 && wRef.every(Number.isInteger))) && {
      ...c,
      wBreakBefore,
      wHeld,
      ...decodeCurveDeltas(wD, wRef),
    };
  if (
    !s ||
    !Array.isArray(s.wCg) ||
    !Array.isArray(s.wAtMs) ||
    !Array.isArray(s.wBreakBefore) ||
    s.wCg.length !== s.wAtMs.length ||
    s.wCg.length !== t.cursor - t.curveBase ||
    s.wCg.some((e) => !o(e, 32767, -32767)) ||
    s.wAtMs.some((e, t) => !o(e, 6e4) || (t && e < s.wAtMs[t - 1])) ||
    s.wBreakBefore.some((e) => !o(e, 1200) || 0 === e || e < t.curveBase || e >= t.cursor) ||
    !Array.isArray(s.wHeld) ||
    s.wHeld.some((e) => !o(e, 1200) || e < t.curveBase || e >= t.cursor)
  )
    throw Error();
  if (t.snapshot) {
    if (wRef) throw Error();
    return { ...t, curve: s };
  }
  // A fragment names the point it continues: it must be the last one held.
  if (
    t.seq !== e.seq + 1 ||
    (s.wCg.length && e.cursor
      ? !wRef || wRef[0] !== e.curve.wAtMs.at(-1) || wRef[1] !== e.curve.wCg.at(-1)
      : wRef)
  )
    throw Error();
  return {
    ...t,
    card: n,
    curve: {
      ...e.curve,
      ...s,
      wCg: [...e.curve.wCg, ...s.wCg],
      wAtMs: [...e.curve.wAtMs, ...s.wAtMs],
      wBreakBefore: [...e.curve.wBreakBefore, ...s.wBreakBefore],
      wHeld: [...e.curve.wHeld, ...s.wHeld],
    },
  };
}
// Curve fragment: base64 delta stream from ref [atMs, cg] (0/0 if none), one
// byte per step or a marker (0xFF time, 0x80 weight) plus the LE absolute.
function decodeCurveDeltas(text, ref) {
  let b;
  try {
    b = atob(text);
  } catch (_) {
    return null;
  }
  const wAtMs = [],
    wCg = [],
    u = (i) => (i + 2 > b.length ? NaN : b.charCodeAt(i) | (b.charCodeAt(i + 1) << 8));
  let [t, w] = ref || [0, 0];
  for (let i = 0, x; i < b.length; ) {
    x = b.charCodeAt(i++);
    t = x === 255 ? u((i += 2) - 2) : t + x;
    x = b.charCodeAt(i++);
    w = x === 128 ? (u((i += 2) - 2) << 16) >> 16 : w + ((x << 24) >> 24);
    if (!(t >= 0) || i > b.length || Number.isNaN(x)) return null;
    wAtMs.push(t);
    wCg.push(w);
  }
  return typeof text === "string" && { wAtMs, wCg };
}
// A page bug while drawing a valid frame must not read as a corrupt frame:
// that would resync, close and reconnect into the same error forever.
function paint(render) {
  try {
    render();
  } catch (e) {
    console.error(e);
  }
}
function startUiStream() {
  if (!webUiPollingActive() || shotWs) return;
  clearTimeout(shotRetry);
  shotRetry = 0;
  shotResyncStrikes = 0;
  shotStale = true;
  paintUiStream();
  const socket = new WebSocket(
    (location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/api/v1/ui/ui-stream",
  );
  shotWs = socket;
  shotGotShot = false;
  shotGotHome = false;
  homeReady = new Promise((resolve) => (homeResolve = resolve));
  shotSetup = setTimeout(() => {
    if (shotWs && (!shotGotShot || !shotGotHome)) shotWs.close();
  }, 8e3);
  socket.onopen = () => {
    if (shotWs !== socket || !webUiPollingActive()) {
      socket.close();
      return;
    }
    const known = homeFrame?.status?.presets && homeFrame.status.presetsTag;
    // The bind carries the subscriptions that fit the firmware's 640-byte
    // request buffer (kUiStreamRequestBytes); the rest follow as their own ops.
    const bind = {
        op: "bind",
        client: webUiClientId,
        ...(shotSeen || {}),
        ...(known ? { presetsTag: known } : {}),
        ops: [],
      },
      rest = [];
    for (const h of Object.values(streamHandlers)) {
      const op = h.subscribe();
      if (!op) continue;
      bind.ops.push(op);
      if (JSON.stringify(bind).length > 640) rest.push(bind.ops.pop());
    }
    socket.send(JSON.stringify(bind));
    for (const op of rest) sendUiOperation(op);
    shotActivity();
  };
  socket.onmessage = (event) => {
    if (shotWs !== socket || !webUiPollingActive()) return;
    try {
      if (typeof event.data !== "string" || event.data.length > 28672) throw Error();
      const data = JSON.parse(event.data);
      uiFrameAt = Date.now();
      clearTimeout(shotDeadline);
      if (data.type === "alive") return;
      // A subscribed type's frames go to its handler; a pending resync drops
      // all but snapshots and cursor-carrying frames (the log) until then.
      const stream = streamHandlers[data.type];
      if (stream) {
        if (stream.cursor || !shotResync || data.snapshot) stream.frame(data);
        return;
      }
      if (shotResync && !data.snapshot) return;
      if (data.type === "home") {
        const paddleOff =
          homeFrame?.status.physicalActivatorOn === true &&
          data.changes?.physicalActivatorOn === false;
        const boot = homeFrame?.boot;
        homeFrame = statusStreamFrame(shotGotHome ? homeFrame : null, data, homeFrame?.status);
        // A rebooted device invalidates the models that name their boot.
        if (boot !== homeFrame.boot) eachStream("reboot", homeFrame.boot);
        if ("timeUtcSec" in data.changes) {
          statusUtcAnchorSec = homeFrame.status.timeUtcSec;
          statusUtcAnchorAt = performance.now();
        }
        delete homeFrame.status.timeUtcSec;
        // One preset model for every view.
        if ("presets" in data.changes) ingestPresets(homeFrame.status);
        shotGotHome = true;
        homeStale = false;
        // Navigation visibility is a Home-stream field; applying it here keeps
        // the Diagnostic menu entry live on every view without a REST probe.
        if ("diagnosticPageVisible" in data.changes) applyDiagnosticNavigation(homeFrame.status);
        // The duration chart bands follow the active preset's window.
        if ("presets" in data.changes && activeView === "stats") paint(renderStatsDurChart);
        if (
          shotClock &&
          homeFrame.boot === shotClock.boot &&
          (homeFrame.status.cycle?.active === false ||
            (homeFrame.status.machineType === "paddle" && paddleOff))
        )
          finishShotClock();
        paint(renderHomeStream);
        if (activeView === "diagnostic") scheduleDiagPaint();
        // Live weight only moves the Settings cup hint.
        if (activeView === "settings")
          Object.keys(data.changes).some((k) => !SETTINGS_LIVE_PATHS.test(k))
            ? scheduleSettingsPaint()
            : paint(() => viewStatusHandlers.settingsLive?.(homeFrame.status));
        homeResolve(true);
      } else {
        if (!shotGotShot && !data.snapshot) throw Error();
        const next = uiStreamFrame(shotGotShot ? shotFrame : null, data);
        if (next === shotFrame) return;
        syncShotClock(next);
        // Home's recipe hint waits for the first answer and the first shot.
        const hintsStale =
          activeView === "home" && (!shotFrame || !shotFrame.card.valid !== !next.card.valid);
        shotFrame = next;
        if (hintsStale) viewStatusHandlers.homeHints?.(homeStatusCache());
        shotGotShot = true;
        shotResync = false;
        shotStale = !!next.stale;
        revealShotClockFinish();
        if (["active", "pending"].includes(next.phase))
          shotSeen = { cycle: next.cycle, boot: next.boot };
        else if (shotSeen && shotSeen.boot !== next.boot) shotSeen = null;
        scheduleShotPaint();
      }
      if (shotGotHome && shotGotShot) clearTimeout(shotSetup);
    } catch (_) {
      invalidateHomeStream();
      shotStale = true;
      scheduleShotPaint();
      // A frame the server keeps producing invalid must not ping-pong resync
      // forever: three bad frames on one connection degrade to the normal
      // close-and-retry error path instead of a silent churn loop. Strikes
      // reset only on a fresh connection, so recurring failures still count.
      if (shotResync || socket.readyState !== WebSocket.OPEN || ++shotResyncStrikes > 3)
        socket.close();
      else requestShotResync();
    }
  };
  socket.onclose = (event) => {
    if (shotWs !== socket) return;
    clearTimeout(shotSetup);
    clearTimeout(shotAlive);
    clearTimeout(shotDeadline);
    shotWs = null;
    resetUiStream();
    scheduleShotPaint();
    if (event?.code === 4001) {
      deactivateWebUi();
      return;
    }
    // 4002: the firmware closed a page it stopped hearing from; reconnect
    // quietly once visible (the visibility return covers a hidden page).
    if (event?.code === 4002) {
      if (!document.hidden && webUiPollingActive()) startUiStream();
      return;
    }
    // 4003: the firmware rejected an op. Clearing the free-form list views
    // makes the replayed subscriptions valid; the device is reachable.
    if (event?.code === 4003) eachStream("rejected");
    else if (webUiPollingActive()) noteReachFail({ network: true }, true);
    if (webUiPollingActive())
      shotRetry = setTimeout(
        startUiStream,
        Math.min(1e4, 500 * 2 ** Math.min(shotTry++, 5)) * (0.8 + 0.4 * Math.random()),
      );
  };
}
// Module-scoped keepalive and resync: a tab back from the background re-arms them after frozen page timers.
// Frames prove the socket and ops or API calls the page: the ping only renews
// an active user's power window, probes a quiet live shot, and stays inside
// the firmware's silence limit. `hidden` holds a hidden page's idle frames.
function shotActivity() {
  if (!shotWs || shotWs.readyState !== 1 || !webUiPollingActive()) return;
  clearTimeout(shotAlive);
  clearTimeout(shotDeadline);
  sendUiOperation({
    op: "activity",
    seconds: webUiPowerSeconds(),
    hidden: document.hidden && !setupSession,
  });
  shotDeadline = setTimeout(() => shotWs && shotWs.close(), 1e4);
  shotAlive = setTimeout(shotKeepalive, 2e4);
}
function shotKeepalive() {
  shotTry = 0;
  const now = Date.now();
  if (
    sessionShotLive()
      ? now - uiFrameAt >= 2e4
      : (webUiPowerSeconds() && now - uiPowerAt >= 19e3) || now - uiSentAt >= 12e4
  )
    shotActivity();
  else shotAlive = setTimeout(shotKeepalive, 2e4);
}
function requestShotResync() {
  if (!shotWs || shotWs.readyState !== 1) return;
  shotResync = true;
  shotGotHome = shotGotShot = false;
  eachStream("resync");
  homeResolve(false);
  homeReady = new Promise((r) => (homeResolve = r));
  clearTimeout(shotSetup);
  shotSetup = setTimeout(() => shotWs && shotWs.close(), 8e3);
  shotWs.send('{"op":"resync"}');
}
// Firmware shot time, run locally between frames. Delay only makes a frame
// late, so the earliest arrival is the best estimate; the clock slews to it.
let shotClock = null,
  shotClockPaint = 0,
  shotClockFinish = null;
const shotClockSilenceMs = 1500;
function syncShotClock(frame) {
  const card = frame.card,
    now = performance.now(),
    c = shotClock;
  if (
    shotClockFinish &&
    (frame.boot !== shotClockFinish.boot || frame.cycle !== shotClockFinish.cycle || !card.valid)
  )
    cancelShotClockFinish();
  if (!card.valid || !card.live) {
    if (card.valid) finishShotClock();
    else {
      shotClock = null;
      if (shotClockPaint) cancelAnimationFrame(shotClockPaint);
      shotClockPaint = 0;
    }
    return;
  }
  if (shotClockFinish) return;
  const id = card.scaleAvailable === false ? "Weight" : "Elapsed";
  if (
    !c ||
    c.boot !== frame.boot ||
    c.cycle !== frame.cycle ||
    shotStale ||
    now - c.seen >= shotClockSilenceMs
  ) {
    const offset = card.elapsedMs - now;
    shotClock = {
      boot: frame.boot,
      cycle: frame.cycle,
      id,
      offset,
      target: offset,
      at: now,
      seen: now,
      shown: card.elapsedMs,
    };
  } else {
    c.id = id;
    c.target = Math.max(c.target, card.elapsedMs - now);
    c.seen = now;
  }
}
function shotClockElapsed() {
  const c = shotClock;
  if (!c)
    return shotClockFinish && !shotClockFinish.revealed
      ? shotClockFinish.elapsed
      : shotFrame.card.elapsedMs;
  if (!shotStale) {
    const now = Math.min(performance.now(), c.seen + shotClockSilenceMs),
      step = (now - c.at) * 0.1;
    c.offset += Math.max(-step, Math.min(step, c.target - c.offset));
    c.at = now;
    c.shown = now + c.offset;
  }
  return c.shown;
}
// A stop signal freezes the clock until the final firmware frame.
function finishShotClock() {
  const c = shotClock;
  if (!c) return;
  const elapsed = shotClockElapsed();
  shotClock = null;
  if (shotClockPaint) cancelAnimationFrame(shotClockPaint);
  shotClockPaint = 0;
  if (
    activeView === "home" &&
    !document.hidden &&
    $("shotHero" + c.id)?.animate &&
    !window.matchMedia?.("(prefers-reduced-motion: reduce)")?.matches
  )
    shotClockFinish = { boot: c.boot, cycle: c.cycle, id: c.id, elapsed };
}
function cancelShotClockFinish() {
  shotClockFinish?.animation?.cancel();
  shotClockFinish = null;
}
// The final duration swaps in place within one shown tenth; a larger
// correction fades out, swaps at zero opacity, and fades back in.
function revealShotClockFinish() {
  const f = shotClockFinish,
    e = f && $("shotHero" + f.id);
  if (!f || f.revealed || shotFrame?.card.live) return;
  if (activeView !== "home" || document.hidden || shotStale || !e) return cancelShotClockFinish();
  const fade = (from, done) => {
    f.animation = e.animate([{ opacity: from }, { opacity: 1 - from }], {
      duration: 500,
      fill: "forwards",
      easing: "ease-in-out",
    });
    f.animation.finished.then(
      () => shotClockFinish === f && done(),
      () => {},
    );
  };
  if (!f.animation)
    return Math.abs(ms(f.elapsed, 1) - ms(shotFrame.card.elapsedMs, 1)) < 0.15
      ? void (shotClockFinish = null)
      : fade(1, revealShotClockFinish);
  if (f.animation.playState !== "finished") return;
  f.revealed = true;
  f.animation.cancel();
  fade(0, () => (shotClockFinish.animation.cancel(), (shotClockFinish = null)));
  paintUiStream();
}
function scheduleShotClock() {
  if (
    shotClockPaint ||
    !shotClock ||
    shotStale ||
    activeView !== "home" ||
    document.hidden ||
    !webUiPollingActive()
  )
    return;
  shotClockPaint = requestAnimationFrame(() => {
    shotClockPaint = 0;
    if (
      !shotClock ||
      shotStale ||
      activeView !== "home" ||
      document.hidden ||
      !webUiPollingActive()
    )
      return;
    const elapsed = shotClockElapsed();
    if (performance.now() - shotClock.seen >= shotClockSilenceMs) {
      shotStale = true;
      paintUiStream();
      return;
    }
    heroNum(shotClock.id, ms(elapsed, 1) + __WEBUI_TEXT__("runtime.s_2"));
    scheduleShotClock();
  });
}
// Full-screen pushes a history entry so Back closes the card. Route changes
// leave that entry intact; consuming it would undo the user's navigation.
let shotFsActive = false,
  shotFsPushed = false,
  shotFsConsumed = false,
  shotFsTimer = 0,
  shotFsExitEnd = null,
  shotFsLock = null,
  shotFsVideo = null;
// Keeps the phone screen on while the card is full screen. Plain HTTP has no
// Wake Lock API, so a looping video covering the card stands in.
function keepShotFsAwake(on) {
  if (!on) {
    if (shotFsLock) shotFsLock.release().catch(() => {});
    shotFsLock = null;
    if (shotFsVideo) shotFsVideo.pause();
    return;
  }
  if (navigator.wakeLock)
    return void navigator.wakeLock.request("screen").then(
      (l) => (shotFsActive ? (shotFsLock = l) : l.release()),
      () => {},
    );
  if (!shotFsVideo) {
    shotFsVideo = document.createElement("video");
    shotFsVideo.className = "heroAwake";
    // iOS WebKit keeps the display awake only during audible playback, so
    // the silent track plays unmuted there and muted everywhere else.
    shotFsVideo.muted = !(
      /iPhone|iPad|iPod/.test(navigator.userAgent) ||
      (navigator.maxTouchPoints > 1 && /Macintosh/.test(navigator.userAgent))
    );
    shotFsVideo.loop = shotFsVideo.playsInline = true;
    shotFsVideo.setAttribute("aria-hidden", "true");
    shotFsVideo.src =
      "data:video/mp4;base64,AAAAIGZ0eXBpc29tAAACAGlzb21pc28yYXZjMW1wNDEAAAMsbW9vdgAAAGxtdmhkAAAAAAAAAAAAAAAAAAAD6AAAB9AAAQAAAQAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAld0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAABAAAAAAAAB9AAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAABAAAAAQAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAAAAEAAAfQAAAAAAABAAAAAAHPbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAABAAAAAgABVxAAAAAAALWhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAAAABWaWRlb0hhbmRsZXIAAAABem1pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAATpzdGJsAAAAunN0c2QAAAAAAAAAAQAAAKphdmMxAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAABAAEABIAAAASAAAAAAAAAABFUxhdmM2MS4xOS4xMDEgbGlieDI2NAAAAAAAAAAAAAAAGP//AAAAMGF2Y0MBQsAK/+EAF2dCwAqmEXsBEAAAAwAQAAADACDxImEYAQAGaMhCDxMgAAAAEHBhc3AAAAABAAAAAQAAABRidHJ0AAAAAAAAAHgAAAAAAAAAGHN0dHMAAAAAAAAAAQAAAAIAAEAAAAAAFHN0c3MAAAAAAAAAAQAAAAEAAAAcc3RzYwAAAAAAAAABAAAAAQAAAAIAAAABAAAAHHN0c3oAAAAAAAAAAAAAAAIAAAATAAAACwAAABRzdGNvAAAAAAAAAAEAAANcAAAAYXVkdGEAAABZbWV0YQAAAAAAAAAhaGRscgAAAAAAAAAAbWRpcmFwcGwAAAAAAAAAAAAAAAAsaWxzdAAAACSpdG9vAAAAHGRhdGEAAAABAAAAAExhdmY2MS43LjEwMwAAAAhmcmVlAAAAJm1kYXQAAAAPZYiCAvnP//+HooAAq8/AAAAAB0GaHAXznUA=";
    $("shotHero").append(shotFsVideo);
  }
  shotFsVideo.play().catch(() => {});
}
function enterShotFullScreen() {
  const hero = $("shotHero");
  if (!hero || shotFsActive) return;
  clearTimeout(shotFsTimer);
  const body = hero.querySelector(".heroBody");
  if (body && shotFsExitEnd) {
    body.removeEventListener("animationend", shotFsExitEnd);
    shotFsExitEnd = null;
  }
  shotFsActive = true;
  hero.classList.remove("fsOut");
  hero.classList.add("fs");
  document.body.classList.add("shotFs");
  keepShotFsAwake(true);
  const close = $("shotFsClose");
  if (close) close.focus();
  shotFsPushed = false;
  try {
    history.pushState({ obbwShotFs: 1 }, "", location.href);
    shotFsPushed = true;
  } catch (_) {}
}
function exitShotFullScreen() {
  const hero = $("shotHero");
  if (!hero || !shotFsActive) return;
  shotFsActive = false;
  // Leaving full screen other than by Back also drops its history entry, so
  // Back is never a dead press; a route change has already moved past it.
  if (shotFsPushed && history.state?.obbwShotFs) {
    shotFsConsumed = true;
    history.back();
  }
  shotFsPushed = false;
  keepShotFsAwake(false);
  clearTimeout(shotFsTimer);
  const body = hero.querySelector(".heroBody"),
    finish = () => {
      clearTimeout(shotFsTimer);
      if (shotFsExitEnd) {
        body && body.removeEventListener("animationend", shotFsExitEnd);
        shotFsExitEnd = null;
      }
      hero.classList.remove("fs", "fsOut");
      document.body.classList.remove("shotFs");
      hero.focus();
    };
  if (
    !body ||
    document.hidden ||
    (window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches)
  ) {
    finish();
    return;
  }
  hero.classList.add("fsOut");
  shotFsExitEnd = function onEnd(e) {
    if (e.animationName !== "heroFsOut") return;
    body.removeEventListener("animationend", shotFsExitEnd);
    shotFsExitEnd = null;
    finish();
  };
  body.addEventListener("animationend", shotFsExitEnd);
  shotFsTimer = setTimeout(() => {
    if (hero.classList.contains("fs")) finish();
  }, 400);
}
// Returns true when a popstate only closed (or had already closed) the
// full-screen card, so the router must not re-render the current view.
function exitFullScreenOnPop() {
  if (shotFsActive) {
    shotFsPushed = false;
    exitShotFullScreen();
    shotFsConsumed = false;
    return true;
  }
  if (shotFsConsumed) {
    shotFsConsumed = false;
    return true;
  }
  shotFsPushed = false;
  return false;
}
function initShotFullScreen() {
  const hero = $("shotHero");
  if (!hero || hero.dataset.fsBound) return;
  hero.dataset.fsBound = "1";
  const close = $("shotFsClose");
  let tap = 0;
  // A double tap or double click toggles full screen; Enter opens it from the keyboard.
  hero.addEventListener("pointerup", (e) => {
    if (e.target.closest("a,button")) return;
    if (e.timeStamp - tap > 350) {
      // iOS resumes unmuted playback only from a gesture: a tap while full
      // screen restarts the keep-awake video if iOS paused it in the background.
      if (shotFsActive && shotFsVideo?.paused) keepShotFsAwake(true);
      return void (tap = e.timeStamp);
    }
    tap = 0;
    shotFsActive ? close.onclick() : enterShotFullScreen();
  });
  // Escape leaves full screen unless an open sheet or dialog takes it first.
  document.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && e.target === hero) enterShotFullScreen();
    if (e.key === "Escape" && shotFsActive && !e.target.closest("[role=dialog],dialog"))
      close?.onclick();
  });
  if (close) close.onclick = exitShotFullScreen;
  hero.addEventListener(
    "click",
    (e) => {
      if (shotFsActive && e.target.closest && e.target.closest("a[data-route]"))
        exitShotFullScreen();
    },
    true,
  );
}
// The diagnostic stream rides the owned shot socket as a view-scoped
// subscription: subscribe when the Diagnostic page appears, unsubscribe on
// every navigation so the device stops sending diagnostic frames.
let diagFrame = null,
  diagStreamWanted = false,
  diagPaint = 0,
  diagReady = Promise.resolve(false),
  diagResolve = null,
  diagSetup = 0;
function armDiagnosticReady() {
  clearTimeout(diagSetup);
  diagResolve?.(false);
  diagReady = new Promise((r) => (diagResolve = r));
  diagSetup = setTimeout(() => {
    if (diagStreamWanted && !diagnosticReady()) shotWs?.close();
  }, 8e3);
}
function diagnosticReady() {
  const s = diagFrame?.status;
  return !!(s?.health?.hwmon && s.nvs && s.tasks && s.scaleProfile);
}
function resolveDiagFrame() {
  if (diagnosticReady() && diagResolve) {
    const r = diagResolve;
    diagResolve = null;
    clearTimeout(diagSetup);
    r(true);
  }
}
function scheduleDiagPaint() {
  diagPaint ||
    (diagPaint = requestAnimationFrame(() => {
      diagPaint = 0;
      paintDiagnosticStream();
    }));
}
// Diagnostic reads the paths it does not own from the Home and Settings models.
function diagnosticStatus() {
  return mergeStatus(homeFrame?.status, diagFrame.status, settingsFrame?.status);
}
function paintDiagnosticStream() {
  if (activeView !== "diagnostic" || !diagFrame || homeStale) return;
  const s = diagnosticStatus();
  viewStatusHandlers.diagnosticLive?.(s);
  const apply = viewStatusHandlers["diagnostic"];
  if (apply && diagnosticReady()) apply(s);
}
async function loadDiagnosticStatus() {
  startUiStream();
  startDiagnosticStream();
  return (diagnosticReady() || (await diagReady)) && (await loadHomeStatus()) && diagnosticStatus();
}
function startDiagnosticStream() {
  if (!webUiPollingActive() || diagStreamWanted) return;
  diagStreamWanted = true;
  armDiagnosticReady();
  if (shotWs && shotWs.readyState === 1) shotWs.send('{"op":"diagnostic","on":true}');
}
// Every route change stops both streams: only a live subscription sends "off".
function stopDiagnosticStream() {
  clearTimeout(diagSetup);
  if (diagStreamWanted && shotWs && shotWs.readyState === 1)
    shotWs.send('{"op":"diagnostic","on":false}');
  diagStreamWanted = false;
  diagFrame = null;
  diagResolve && diagResolve(false);
  diagResolve = null;
}
registerStream("diagnostic", {
  subscribe() {
    if (!diagStreamWanted) return null;
    if (!diagResolve) armDiagnosticReady();
    return { op: "diagnostic", on: true };
  },
  frame(data) {
    if (!diagStreamWanted) return;
    const next = statusStreamFrame(diagFrame, data);
    if (next !== diagFrame) {
      diagFrame = next;
      scheduleDiagPaint();
    }
    resolveDiagFrame();
  },
  reset() {
    clearTimeout(diagSetup);
    diagFrame = null;
    diagResolve?.(false);
    diagResolve = null;
  },
});
// Standing Settings subscription (not in compatibility mode): its model holds
// the paths Home lacks and survives reconnects, named by its tag.
let settingsFrame = null,
  settingsStreamWanted = false,
  settingsPaint = 0,
  settingsReady = null,
  settingsResolve = null;
// Home paths the Settings form does not show.
const SETTINGS_LIVE_PATHS =
  /^(scale\.(observedWeightG|timerMs)$|timeUtcSec$|quickRevert\.remainingS$|cupPresence\.|connections\.)/;
const settingsSubscribeOp = () => ({
  op: "settings",
  on: true,
  ...(settingsFrame && { tag: settingsFrame.tag }),
});
function settingsSendSubscribe() {
  sendUiOperation(settingsSubscribeOp());
}
registerStream("settings", {
  subscribe: () => (settingsStreamWanted ? settingsSubscribeOp() : null),
  frame(data) {
    if (!settingsStreamWanted) return;
    if (!Number.isInteger(data.tag)) throw Error();
    settingsFrame = { ...statusStreamFrame(settingsFrame, data), tag: data.tag };
    settingsResolve?.(true);
    settingsResolve = settingsReady = null;
    scheduleSettingsPaint();
    // Diagnostic reads the Micra account paths from this model.
    if (activeView === "diagnostic") scheduleDiagPaint();
  },
  // The Settings model outlives the socket, but not the device boot.
  reset() {
    settingsResolve?.(false);
    settingsResolve = settingsReady = null;
  },
  reboot(boot) {
    if (settingsFrame && settingsFrame.boot !== boot) settingsFrame = null;
  },
});
function ensureSettingsStream() {
  if (settingsStreamWanted || compatMode) return;
  settingsStreamWanted = true;
  settingsSendSubscribe();
}
function settingsSnapshotOk(s) {
  return typeof s.config?.dripDelayMs === "number" && Array.isArray(s.scale?.history);
}
// One owner per path: objects several models hold merge key by key.
function mergeStatus(...models) {
  const out = {},
    plain = (v) => v?.constructor === Object;
  for (const model of models)
    for (const [k, v] of Object.entries(model || {}))
      out[k] = plain(out[k]) && plain(v) ? mergeStatus(out[k], v) : v;
  return out;
}
function settingsStatus() {
  return homeFrame && settingsFrame ? mergeStatus(homeFrame.status, settingsFrame.status) : null;
}
// A warm entry paints the cached model with no op.
async function loadSettingsStatus() {
  startUiStream();
  ensureSettingsStream();
  if (!settingsFrame) await (settingsReady ||= new Promise((r) => (settingsResolve = r)));
  return settingsFrame && (await loadHomeStatus()) && settingsStatus();
}
function scheduleSettingsPaint() {
  settingsPaint ||= requestAnimationFrame(() => {
    settingsPaint = 0;
    const s = !homeStale && settingsStatus();
    if (s && activeView === "settings") paint(() => viewStatusHandlers.settings?.(s));
  });
}
let uiSentAt = 0,
  uiPowerAt = 0,
  uiFrameAt = 0;
function noteUiSent(power) {
  uiSentAt = Date.now();
  if (power) uiPowerAt = uiSentAt;
}
function sendUiOperation(op) {
  if (shotWs?.readyState !== 1) return;
  shotWs.send(JSON.stringify(op));
  noteUiSent(op.seconds > 0);
}
function diagSnapshotOk(s) {
  return (
    typeof s.relayClosed === "boolean" &&
    typeof s.controlSource === "string" &&
    typeof s.safety?.fault === "string" &&
    typeof s.scale?.controlState === "string"
  );
}
// After the first view paints, subscribe the standing Settings stream in the
// background; compat mode has no Settings view. Stats and History subscribe
// on their first visit (their module owns the subscription) and then stay
// standing for the session.
function ensureBackgroundStreams() {
  if (!webUiPollingActive() || compatMode) return;
  ensureSettingsStream();
}
// A page hidden for 5 minutes drops its stream (a live shot rechecks every
// 30 s); phones freeze hidden timers, so the return also closes after such an
// absence. An open socket delivered every frame in order: coming back needs
// only the liveness ping. Setup sessions and the 15-minute inactivity
// deactivation are untouched.
const HIDDEN_CLOSE_MS = 3e5;
let hiddenAt = 0,
  hiddenPerf = 0,
  hiddenClose = 0;
function closeHiddenStream() {
  if (!document.hidden || setupSession) hiddenClose = 0;
  else if (sessionShotLive()) hiddenClose = setTimeout(closeHiddenStream, 3e4);
  else {
    hiddenClose = 0;
    stopUiStream();
  }
}
function onPageVisibility() {
  clearTimeout(hiddenClose);
  hiddenClose = 0;
  if (document.hidden) {
    hiddenAt = Date.now();
    hiddenPerf = performance.now();
    if (!setupSession) {
      hiddenClose = setTimeout(closeHiddenStream, HIDDEN_CLOSE_MS);
      shotActivity();
    }
    return;
  }
  const away = hiddenAt ? Date.now() - hiddenAt : 0,
    // A suspended page's monotonic clock may stand still: the wall-clock
    // anchor moves by the sleep it missed.
    missed = away - (performance.now() - hiddenPerf);
  if (hiddenAt && statusUtcAnchorAt && missed > 1e3) statusUtcAnchorAt -= missed;
  hiddenAt = 0;
  if (shotFsActive) keepShotFsAwake(true);
  if (!webUiPollingActive() || setupSession) return;
  if (away >= HIDDEN_CLOSE_MS && !sessionShotLive()) stopUiStream();
  shotWs && shotWs.readyState === 1 ? shotActivity() : startUiStream();
}
document.addEventListener("visibilitychange", onPageVisibility);
if (document.hidden) onPageVisibility();
window.addEventListener("pagehide", stopUiStream);
// Unloading the page drops every unsaved Save/Revert section (setSaveDirty
// marks them) and an autosave still pending or in flight.
// Unsaved edits anywhere: a dirty Save button or any autosave or draft flag.
function hasUnsavedEdits() {
  return !!(
    document.querySelector('[data-dirty="1"]') ||
    ["config", "brew", "dateTime", "ble", "power", "frontend", "micra", "quick"].some(
      (k) => uiState[k + "Dirty"],
    )
  );
}
window.addEventListener("beforeunload", (e) => {
  if (!fwReloading && hasUnsavedEdits()) e.preventDefault();
});
window.addEventListener("pageshow", () => {
  // A restored page starts fresh here; its visibility return must not redo it.
  hiddenAt = 0;
  if (webUiPollingActive() && !setupSession) startUiStream();
});
function setHomeSub(id, v) {
  const e = $(id);
  if (e && e.textContent !== v) e.textContent = v;
}
function renderShotHero(d) {
  const hero = $("shotHero");
  if (!hero) return;
  const t = (id, v) => ($("shotHero" + id).textContent = v),
    wt = typeof d.weight === "number" ? d.weight : null,
    goal = d.goal,
    // Scale-loss backup: a finished shot shows its estimated yield, a live
    // one its clock against the backup's stop time.
    sl = d.scaleLoss,
    est = !d.live && sl && sl.estimatedG != null ? Math.round(sl.estimatedG) : null,
    el = d.elapsedMs,
    drop = d.firstDropMs,
    model = buildShotSparkModel({
      wCg: d.wCg,
      wAtMs: d.wAtMs,
      wBreakBefore: d.wBreakBefore,
      wHeld: d.wHeld,
      wTruncated: d.wTruncated,
      durationS: typeof el === "number" ? el / 1000 : null,
      firstDropS: drop != null ? drop / 1000 : null,
      tareS: d.tareMs != null ? d.tareMs / 1000 : null,
      dropS: d.dropS,
      dropCg: d.dropCg,
      extendedS: d.extendedS,
      extCg: d.extCg,
      atmS: d.atmS,
      atmCg: d.atmCg,
      atmClearedS: d.atmClearedS,
      endS: d.endS,
      endCg: d.endCg,
      extractionExtended: d.extractionExtended,
      slowExtractionExtended: d.slowExtractionExtended,
      goalG: goal,
    });
  hero.hidden = false;
  hero.classList.toggle("live", !!d.live);
  t("Preset", d.presetName && d.presetName !== "—" ? " · " + d.presetName : "");
  const noScale = d.scaleAvailable === false;
  const label =
    heroNum(
      "Weight",
      est !== null
        ? "~" + est + __WEBUI_TEXT__("runtime.g_2")
        : sl && sl.state === "finished"
          ? __WEBUI_TEXT__("runtime.weight_unknown")
          : noScale
            ? ms(d.clockMs, 1) + __WEBUI_TEXT__("runtime.s_2")
            : wt === null
              ? "—"
              : wt.toFixed(1) + __WEBUI_TEXT__("runtime.g_2"),
    ) +
    t(
      "Goal",
      noScale && est === null
        ? d.live && sl && sl.state === "lost"
          ? " / ~" + +sl.stopAtS.toFixed(1) + __WEBUI_TEXT__("runtime.s_2")
          : ""
        : goal > 0
          ? " / " + goal.toFixed(1) + __WEBUI_TEXT__("runtime.g_2")
          : "",
    );
  const chip = (id, v) => {
    const e = $("shotHero" + id);
    e.hidden = !v;
    e.textContent = v || "";
  };
  heroNum(
    "Elapsed",
    (!noScale || est !== null) && ms(d.clockMs, 1) + __WEBUI_TEXT__("runtime.s_2"),
  );
  // Live flow: the chart's own, averaged over its last second; none during a
  // touch hold or once stale. A finished shot shows its average.
  const fi = model ? model.flowCurve.findLastIndex(Number.isFinite) : -1,
    flow =
      noScale || drop == null || (d.live && (wt === null || d.touchHold))
        ? null
        : !d.live
          ? d.averageFlowGps
          : fi >= 0 && d.wAtMs.at(-1) - d.wAtMs[fi] <= 1000
            ? model.flowCurve
                .filter((v, i) => v != null && d.wAtMs[fi] - d.wAtMs[i] <= 1000)
                .reduce((a, v, _, n) => a + v / n.length, 0)
            : null;
  // A finished card fades the live flow into its average.
  const fe = $("shotHeroFlow"),
    ft = Number.isFinite(flow) && Math.max(0, flow).toFixed(1) + __WEBUI_TEXT__("runtime.g_s");
  heroFadeSwap(fe, () => heroNum("Flow", ft), !d.live && ft !== (!fe.hidden && fe.textContent));
  chip("Touch", !noScale && d.live && d.touchHold && __WEBUI_TEXT__("home.hero_touch_hold"));
  // Pills say how the cut line ended the shot when not at the target.
  const kind = d.live ? "" : /wall_limit$/.test(d.endReason) ? "max_time" : d.verdict || "",
    cut =
      sl && sl.state !== "recovered"
        ? __WEBUI_TEXT__("home.hero_no_scale")
        : d.live
          ? ""
          : d.channeling
            ? __WEBUI_TEXT__("runtime.cut_channeling")
            : d.cutG > d.goal
              ? __WEBUI_TEXT__("runtime.cut_extended")
              : d.cutG > 0 && d.cutG < d.goal
                ? __WEBUI_TEXT__("runtime.cut_stopped_early")
                : "";
  chip("Cut", cut);
  const mode =
    d.live && sl && sl.state === "recovered"
      ? __WEBUI_TEXT__("runtime.scale_back")
      : noScale
        ? __WEBUI_TEXT__("home.hero_no_scale")
        : d.live
          ? {
              auto: __WEBUI_TEXT__("runtime.bbw"),
              timer_only: __WEBUI_TEXT__("runtime.timer_only"),
              manual: __WEBUI_TEXT__("runtime.manual"),
              rinse: __WEBUI_TEXT__("runtime.rinse"),
            }[(d.shotType || "").toLowerCase()] || ""
          : formatShotEnded(d.endReason, d.verdict);
  chip("Mode", !kind && !cut && mode !== "—" && mode !== __WEBUI_TEXT__("runtime.bbw") && mode);
  $("shotHeroPills").hidden = ["Cut", "Touch", "Mode"].every((id) => $("shotHero" + id).hidden);
  const m = d.live
      ? liveRuleModel
      : d.rule &&
        shotRuleModel({
          ...d.rule,
          goalG: d.goal,
          shotType: d.shotType,
          firstDropS: d.firstDropMs / 1e3,
        }),
    tintMode = heroTintMode(),
    tint = heroTint(d, m, kind, el, noScale, tintMode);
  hero.dataset.tint = tint;
  // Live forecast fades brand brown into the predicted colour; the firmware
  // owns the blend percent, the browser just renders it.
  hero.style.setProperty(
    "--tp",
    (d.live && tintMode === "forecast" && d.tintClass
      ? Math.max(0, Math.min(100, +d.tintPct || 0))
      : 100) + "%",
  );
  t(
    "State",
    d.live
      ? heroStateText(d, tint, m, el, tintMode) ||
          (tint === "fast"
            ? __WEBUI_TEXT__("home.hero_running_fast")
            : tint === "on_recipe"
              ? __WEBUI_TEXT__("home.hero_in_window")
              : tint === "slow"
                ? __WEBUI_TEXT__("home.hero_past_window")
                : __WEBUI_TEXT__("home.hero_brewing"))
      : kind === "max_time"
        ? __WEBUI_TEXT__("runtime.max_time")
        : kind
          ? verdictLabel(kind)
          : __WEBUI_TEXT__("home.hero_last_shot"),
  );
  renderHeroChart(
    $("shotHeroSpark"),
    model,
    !noScale && m && m.mode === "active" ? m : null,
    goal,
    label,
  );
}
// Hero numbers swap with a quick fade; a value arriving mid-fade replaces the pending one.
// Numbers keep their full text; a trailing unit is set smaller. Unchanged
// values (the per-frame clock) leave the DOM alone.
function heroNum(id, v) {
  const e = $("shotHero" + id),
    [, n, u = ""] = /^(.*\d)(\s?(?:g\/s|[gs]))$/.exec(v || "") || [0, v || ""];
  e.hidden = !v;
  if (e.textContent === n + u) return v;
  e.replaceChildren(n);
  if (u) e.append(Object.assign(document.createElement("small"), { textContent: u }));
  return v;
}
function heroFadeSwap(e, set, fade) {
  if (e.fade) return void (e.fade.set = set);
  if (!fade || !e.animate || window.matchMedia?.("(prefers-reduced-motion: reduce)")?.matches)
    return set();
  const out = e.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 100, fill: "forwards" });
  e.fade = { set };
  out.finished.then(
    () => {
      e.fade.set();
      e.fade = null;
      e.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 100 });
      out.cancel();
    },
    () => (e.fade = null),
  );
}
// Hero colour. In forecast mode the firmware's shot predictor owns the live
// colour class and blend percent (tintClass/tintPct on the card); window and
// brand modes keep their local rules.
const HERO_TINT_KEY = "obbwHeroTint",
  HERO_TINT_MODES = ["forecast", "window", "brand"];
let heroWindowRed = { el: 0, since: -1, on: false };
function heroTintMode() {
  try {
    const v = localStorage.getItem(HERO_TINT_KEY);
    return HERO_TINT_MODES.includes(v) ? v : "forecast";
  } catch (_) {
    return "forecast";
  }
}
function heroTint(d, m, kind, el, noScale, mode) {
  const v = ["fast", "on_recipe", "slow"].find((x) => x === kind || x === d.verdict);
  if (mode === "brand") return "brand";
  if (!d.live) return v || "neutral";
  if (mode === "forecast")
    return ["neutral", "fast", "on_recipe", "slow"].includes(d.tintClass) ? d.tintClass : "neutral";
  if (noScale || !m || m.mode !== "active" || (d.shotType || "").toLowerCase() !== "auto")
    return "neutral";
  if (el < heroWindowRed.el) heroWindowRed = { el: 0, since: -1, on: false };
  heroWindowRed.el = el;
  // Window mode keeps its local fast alert: the forecast held before the
  // window for 1.5 s turns the card red.
  const early =
    d.extractionExtended || (d.expectedEndMs > 0 && d.expectedEndMs < (m.t1 - 0.5) * 1000);
  if (!early) heroWindowRed.since = -1;
  else if (heroWindowRed.since < 0) heroWindowRed.since = el;
  heroWindowRed.on ||= !!d.extractionExtended || (early && el - heroWindowRed.since >= 1500);
  if (heroWindowRed.on) return "fast";
  const at = el / 1000;
  return at > m.t2 ? "slow" : mode === "window" && at >= m.t1 ? "on_recipe" : "neutral";
}
// Live state copy: names the predicted class; on slow adds the barista
// advisory (projected weight at the window end, or tracking-to for stalls).
function heroStateText(d, tint, m, el, mode) {
  const note = { recovered: __WEBUI_TEXT__("runtime.flow_rose") }[d.tintNote] || "";
  const suffix = note ? " · " + note : "";
  if (tint !== "slow") {
    if (tint === "on_recipe" && mode === "forecast" && d.tintPct >= 50 && d.expectedEndMs > 0)
      return "~" + ms(d.expectedEndMs, 0) + __WEBUI_TEXT__("runtime.s_2") + suffix;
    return null; // caller falls back to the stock labels
  }
  const q = d.averageFlowGps,
    at = el / 1000;
  if (mode === "forecast" && q > 0 && m && typeof d.weight === "number") {
    const stalled = d.tintNote === "stalled",
      until = stalled ? m.wall : m.t2,
      x = Math.round(d.weight + q * Math.max(0, until - at));
    return stalled
      ? sub(__WEBUI_TEXT__("runtime.stalled_tracking"), [x]) + suffix
      : sub(__WEBUI_TEXT__("runtime.slow_about_xg"), [x, m.t2]) + suffix;
  }
  return null;
}
// Drawn flow: a ±1 s mean (g/s) that evens out scale noise. Points are in time
// order, so one sliding window keeps it linear in the curve length.
function smoothFlow({ pts: p }) {
  let lo = 0,
    hi = 0,
    sum = 0;
  return p.map((q) => {
    for (; hi < p.length && p[hi].t <= q.t + 1; hi++) sum += p[hi].cg;
    for (; p[lo].t < q.t - 1; lo++) sum -= p[lo].cg;
    return [q.t, sum / (hi - lo) / 100];
  });
}
// Curve, cut line and window; overlays use CSSOM (the CSP blocks inline styles).
function renderHeroChart(host, model, m, goal, label) {
  if (!host) return;
  host.setAttribute("aria-label", label);
  const segs = model && model.segs.filter((s) => s.pts.length > 1);
  if (!segs || !segs.length) return void host.replaceChildren();
  // After the cut, the drip tail runs dashed to the last weight on the scale.
  const cut = segs.at(-1).pts.at(-1),
    drip = model.dripSegs,
    end = drip.at(-1)?.pts.at(-1) || cut,
    xMax = Math.max(model.timeMax, m ? Math.min(m.wall, m.t2 + m.d + 4) : 0),
    yMax = 1.15 * Math.max(model.maxW, m ? m.ceil : 0, goal > 0 ? goal : 0),
    X = (v) => (100 * Math.min(v, xMax)) / xMax,
    Y = (g) => 100 - (100 * Math.max(0, g)) / yMax,
    path = (pts, y = Y) =>
      pts.map((q, i) => (i ? "L" : "M") + X(q[0]).toFixed(2) + " " + y(q[1]).toFixed(2)).join(""),
    curve = (s) => path(s.pts.map((q) => [q.t, q.cg / 100])),
    flows = [model.flowSegs, model.dripFlowSegs].map((a) =>
      a.filter((s) => s.pts.length > 1).map(smoothFlow),
    ),
    // Flow keeps to the lower 32 % on its own scale.
    flowTop = Math.max(2.5, ...flows.flat(2).map((q) => 1.15 * q[1])),
    flowY = (r) => 100 - (32 * r) / flowTop,
    stroke = 'stroke="var(--on)" vector-effect="non-scaling-stroke"',
    put = (cls, text, x, y) => {
      const e = Object.assign(document.createElement("span"), {
        className: cls,
        textContent: text,
      });
      e.style.left = x.toFixed(2) + "%";
      if (y != null) e.style.top = y.toFixed(2) + "%";
      return e;
    };
  let svg =
    '<svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true"><defs>' +
    fadeFill("heroFade", "var(--on)", 0.38) +
    fadeFill("heroWin", "var(--on)", 0.3, 0.02) +
    fadeFill("heroFlow", "var(--on)", 0.26, 0.03) +
    // Nothing is drawn below the x axis.
    '</defs><clipPath id="heroPlot"><rect x="-10" y="-50" width="120" height="150"/></clipPath><g clip-path="url(#heroPlot)">' +
    // A soft flow area; its drip tail is fainter.
    flows
      .map((a, drip) =>
        a
          .map(
            (p) =>
              `<path d="${path(p, flowY)}V100H${X(p[0][0]).toFixed(2)}Z" fill=${drip ? '"var(--on)" fill-opacity=".06"' : '"url(#heroFlow)"'}/>`,
          )
          .join(""),
      )
      .join("");
  const over = [],
    hatch = m && put("noCut", "", 0);
  if (m) {
    // The preset chart's corridor paces under the cut line; On recipe filled.
    const t0 = m.firstDrop,
      ts = (a, n) => Array.from({ length: n + 1 }, (_, i) => a + ((xMax - a) * i) / n),
      pace = (ta) => ts(t0, 48).map((v) => [v, referencePace(v, m.goal, ta, t0)]),
      line = ts(0, 96).map((v) => [v, cutLine(v, m)]);
    svg +=
      `<clipPath id="heroUnder"><path d="${path([[0, 0], ...line, [xMax, 0]])}Z"/></clipPath>` +
      `<path d="M${X(m.t1).toFixed(2)} 0V100M${X(m.t2).toFixed(2)} 0V100" ${stroke} stroke-opacity=".35" stroke-dasharray="1.5 3" stroke-linecap="round"/>` +
      `<g clip-path="url(#heroUnder)"><path d="${path([...pace(m.t1), ...pace(m.t2).reverse()])}Z" fill="url(#heroWin)"/>` +
      [m.t1, m.t2]
        .map((ta) => `<path d="${path(pace(ta))}" fill="none" ${stroke} stroke-opacity=".4"/>`)
        .join("") +
      `</g><path d="${path(line)}" fill="none" ${stroke} stroke-opacity=".7" stroke-width="1.5" stroke-dasharray="4 3"/>`;
    // A faint hatch over the no-cut delay.
    hatch.style.width = X(m.prot).toFixed(2) + "%";
    over.push(
      put("xl", String(m.t1), X(m.t1)),
      put("xl", m.t2 + __WEBUI_TEXT__("runtime.s_2"), X(m.t2)),
    );
  }
  host.innerHTML =
    svg +
    segs
      .map((s) => {
        const p = curve(s);
        return (
          `<path d="${p}V100H${X(s.pts[0].t).toFixed(2)}Z" fill="url(#heroFade)"/>` +
          `<path d="${p}" fill="none" ${stroke} stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/>`
        );
      })
      .join("") +
    drip
      .map(
        (s) =>
          `<path d="${curve(s)}" fill="none" ${stroke} stroke-width="2.4" stroke-dasharray="3 3"/>`,
      )
      .join("") +
    (drip.length
      ? `<path d="${path([[cut.t, cut.cg / 100]])}h0" ${stroke} stroke-width="6" stroke-linecap="round"/>`
      : "") +
    `</g><line x1="0" y1="99.6" x2="100" y2="99.6" ${stroke} stroke-opacity=".3"/></svg>`;
  host.append(...over, put("end", "", X(end.t), Y(end.cg / 100)));
  if (hatch) host.prepend(hatch);
}
// Home cards: a grind hint when the active preset's shots drift. Dismissals
// stay in this browser.
function cardDismissed(key) {
  try {
    return localStorage.getItem(key) || "";
  } catch (_) {
    return "";
  }
}
function dismissCard(key, value = "1") {
  try {
    localStorage.setItem(key, value);
  } catch (_) {}
  renderHomeCards();
}
// Grind hint card: the firmware decides (state, direction, tier, size,
// level, band); this only words it and places it on the window bar.
function grindHintView(p) {
  const h = p && p.grindHintsEnabled && p.grindHint;
  if (!h) return null;
  const t1 = +p.fastBeforeS,
    t2 = +p.slowAfterS,
    c = (t1 + t2) / 2,
    level = c + +h.offsetS,
    n = (v) => +(+v).toFixed(1),
    at = (s) => Math.min(99, Math.max(1, 12 + (76 * (s - t1)) / (t2 - t1 || 1))),
    fine = h.direction === "finer",
    v = {
      // Ignore holds for this advice: preset, grind setting, direction, tier.
      key: ["obbwCard:grind", p.id, h.segmentStartId, h.direction || h.state, h.tier || ""].join(
        ":",
      ),
      cls: { checking: "isCheck", vary: "isSpread" }[h.state] || (fine ? "isFast" : "isSlow"),
      level: at(level),
      band: [at(c - h.bandS), 100 - at(c + h.bandS)],
      axis: [n(t1) + " s", n(c), n(t2) + " s"],
      shots:
        h.shots === 1
          ? __WEBUI_TEXT__("runtime.grind_shots_one")
          : sub(__WEBUI_TEXT__("runtime.grind_shots"), [h.shots]),
      actions: h.state !== "checking",
    };
  if (h.state === "vary")
    return Object.assign(v, {
      tag: __WEBUI_TEXT__("runtime.grind_tag_vary"),
      conf: "",
      dir: __WEBUI_TEXT__("runtime.grind_vary"),
      mag: "",
      text: sub(__WEBUI_TEXT__("runtime.grind_vary_sub"), [n(t1), n(t2)]),
    });
  if (h.state === "checking")
    return Object.assign(v, {
      tag: __WEBUI_TEXT__("runtime.grind_tag_checking"),
      conf: __WEBUI_TEXT__("runtime.grind_next_shot"),
      shots: "",
      dir: __WEBUI_TEXT__("runtime.grind_checking"),
      mag: sub(__WEBUI_TEXT__("runtime.grind_now"), [n(level)]),
      text: __WEBUI_TEXT__("runtime.grind_checking_sub"),
    });
  const secs = [Math.max(1, Math.round(Math.abs(h.offsetS)))];
  return Object.assign(v, {
    tag:
      h.state === "dial_in"
        ? __WEBUI_TEXT__("runtime.grind_tag_dial_in")
        : __WEBUI_TEXT__("runtime.grind_tag_hint"),
    conf: {
      tentative: __WEBUI_TEXT__("runtime.grind_tier_tentative"),
      likely: __WEBUI_TEXT__("runtime.grind_tier_likely"),
      confident: __WEBUI_TEXT__("runtime.grind_tier_confident"),
    }[h.tier],
    dir: (fine
      ? {
          small: __WEBUI_TEXT__("runtime.grind_finer_small"),
          medium: __WEBUI_TEXT__("runtime.grind_finer_medium"),
          large: __WEBUI_TEXT__("runtime.grind_finer_large"),
        }
      : {
          small: __WEBUI_TEXT__("runtime.grind_coarser_small"),
          medium: __WEBUI_TEXT__("runtime.grind_coarser_medium"),
          large: __WEBUI_TEXT__("runtime.grind_coarser_large"),
        })[h.bin],
    mag: fine
      ? sub(
          h.atLeast
            ? __WEBUI_TEXT__("runtime.grind_fast_at_least")
            : __WEBUI_TEXT__("runtime.grind_fast"),
          secs,
        )
      : sub(
          h.atLeast
            ? __WEBUI_TEXT__("runtime.grind_slow_at_least")
            : __WEBUI_TEXT__("runtime.grind_slow"),
          secs,
        ),
    text:
      h.shots === 1
        ? sub(__WEBUI_TEXT__("runtime.grind_text_one"), [n(p.goalWeightG), n(level), n(t1), n(t2)])
        : sub(__WEBUI_TEXT__("runtime.grind_text"), [h.shots, n(level), n(t1), n(t2), n(c)]),
  });
}
function renderHomeCards() {
  const v = grindHintView(presetState.items.find((x) => x.id === presetState.activeId)),
    card = $("grindHintCard");
  if (card) {
    reveal(card, !!v && !cardDismissed(v.key));
    if (v) {
      card.dataset.key = v.key;
      card.className = "homeCard ghCard " + v.cls;
      $("ghTag").textContent = v.tag;
      const b = document.createElement("b");
      b.textContent = v.shots;
      $("ghConf").replaceChildren(v.conf && v.shots ? v.conf + " · " : v.conf, b);
      $("ghDir").textContent = v.dir;
      $("ghMag").textContent = v.mag && "· " + v.mag;
      $("ghText").textContent = v.text;
      $("ghBand").style.left = v.band[0] + "%";
      $("ghBand").style.right = v.band[1] + "%";
      $("ghLevel").style.left = v.level + "%";
      $("ghAxis").replaceChildren(
        ...v.axis.map((t) => Object.assign(document.createElement("span"), { textContent: t })),
      );
      $("ghActions").hidden = !v.actions;
    }
  }
}
// Relearn (a new bag, dose, basket or grinder): starts the preset's statistics over.
async function startNewCoffee(id = presetState.activeId) {
  const p = presetState.items.find((x) => x.id === id),
    text =
      p && sub(__WEBUI_TEXT__("settings.new_coffee_confirm"), [p.name, p.shotStats?.total || 0]);
  if (p && (await askConfirm(text, __WEBUI_TEXT__("settings.new_coffee"))))
    command("/api/v1/presets", { action: "new_coffee", id });
}
// [Cancel][<ok>] confirmation in the shell dialog; resolves true on <ok>.
function askConfirm(text, ok, danger = false) {
  const d = $("confirmDialog"),
    b = $("confirmOk");
  $("confirmText").textContent = text;
  b.firstChild.textContent = ok;
  b.classList.toggle("btnInvert", !danger);
  b.classList.toggle("btnDanger", danger);
  d.returnValue = "";
  d.showModal();
  return new Promise((done) =>
    d.addEventListener("close", () => done(d.returnValue === "ok"), { once: true }),
  );
}
// "Last 10 shots: 28–33 s, median 30.4 s": arrival times since the preset's
// statistics last started over. Empty until the first shot.
function shotStatsLine(p) {
  const st = p && p.shotStats,
    n = (v) => +(+v).toFixed(1);
  return st && st.n >= 3
    ? sub(__WEBUI_TEXT__("runtime.shot_stats_line"), [st.n, n(st.minS), n(st.maxS), n(st.medianS)])
    : "";
}
function clearShotHero() {
  const hero = $("shotHero");
  if (hero) hero.hidden = true;
}
let fwShellRefreshed = false;
function checkFirmwareReload(version) {
  // Reload only when nothing unsaved would be lost; the next status retries.
  if (!version || SHELL_FW_VERSION === "dev" || hasUnsavedEdits()) return;
  if (version === SHELL_FW_VERSION) {
    try {
      sessionStorage.removeItem("obbwFwReload");
    } catch (_) {}
    // The ?fw= landing cached the new shell under its own key; refresh the
    // plain paths' cached copies too, so the next open loads it directly.
    if (!fwShellRefreshed && /[?&]fw=/.test(location.search)) {
      fwShellRefreshed = true;
      for (const p of new Set(["/", location.pathname]))
        fetch(p, { cache: "reload" })
          .then((r) => r.arrayBuffer())
          .catch(() => {});
    }
    return;
  }
  let reloaded = true;
  try {
    reloaded = !!sessionStorage.getItem("obbwFwReload");
    if (!reloaded) sessionStorage.setItem("obbwFwReload", "1");
  } catch (_) {}
  if (!reloaded) {
    fwReloading = true; // A fresh-cached navigation may be reopened without revalidating (WebKit
    // home-screen apps), so reload() can land on the stale copy again; a
    // changed URL is a new cache key and forces a request to the device.
    location.replace(location.pathname + "?fw=" + version);
  }
}
function applyCommonStatus(s) {
  if (s.connections || s.snapshotStale) updateHeaderSignals(s);
  applyDiagnosticNavigation(s);
  if ("development" in s) developmentMode = !!s.development;
  if ("compatibilityMode" in s && !!s.compatibilityMode !== compatMode) {
    compatMode = !!s.compatibilityMode;
    applyCompatibilityChrome();
  }
  setMutable(!!s.configMutable || !!s.webUiOverrideActive);
  if (s.firmwareVersion) {
    firmwareVersion = s.firmwareVersion;
    checkFirmwareReload(s.firmwareVersion);
  }
  if (typeof s.bootId === "number") bootId = s.bootId;
  updateFirmwareFooter();
  const c = s.config || {};
  syncTimezone(c);
  lastCommandStatus = s.lastCommand || null;
  if (typeof c.revision === "number") configRevision = c.revision;
  updateQuickSettings(s);
  if (typeof s.timeUtcSec === "number") {
    statusUtcAnchorSec = s.timeUtcSec;
    statusUtcAnchorAt = performance.now();
  }
  if (typeof c.appliedTimezoneOffsetMinutes === "number")
    statusTimezoneOffsetMinutes = c.appliedTimezoneOffsetMinutes;
  if (!uiState.dateTimeDirty) {
    if (c.ntpServerPreset != null && $("ntpServerPreset"))
      $("ntpServerPreset").value = c.ntpServerPreset || "pool";
    if (c.ntpServerCustom != null && $("ntpServerCustom"))
      $("ntpServerCustom").value = c.ntpServerCustom || "";
  }
  applyMachineTypeUi(s);
}
function applyMachineTypeUi(s) {
  const t = s.machineType;
  document.documentElement.classList.toggle("momentaryMachine", t !== "paddle");
  document.documentElement.classList.toggle("reedMachine", t === "momentary_reed");
  document.documentElement.classList.toggle(
    "lineaMicraIntegration",
    s.machineIntegration === "linea_micra_cloud",
  );
  if ($("dMt")) $("dMt").textContent = t;
}
// Each field follows the device unless it differs from the baseline (an edit).
// A Quick Settings override shows the saved value it hides; Home keeps the live one.
function loadSettingsConfig({ config: c, quickRevert }) {
  if (!c || typeof c.goalWeightG !== "number" || !$("goalWeightG")) return;
  const els = [...settingsSectionEls("config"), ...settingsSectionEls("brew")],
    base = uiState.formBaseline || {},
    edited = configLoaded ? snapshotControls(els) : {},
    active = presetState.items.find((x) => x.id === presetState.activeId) || {},
    focused = document.activeElement,
    // "38" and "38.0" are one value: the form writes canonical text.
    same = (a, b) => a === b || (a !== "" && b !== "" && +a === +b);
  // Never rewrite the field being typed in; catch up when it loses focus.
  if (configLoaded && els.includes(focused) && focused.type !== "checkbox")
    return focused.addEventListener("blur", scheduleSettingsPaint, { once: true });
  for (const id in edited) if (same(edited[id], base[id])) delete edited[id];
  c = {
    ...c,
    ...quickRevert?.saved,
    doseG: active.doseG,
    grindHintsEnabled: active.grindHintsEnabled,
  };
  bbwFormPresetId = c.bbwPresetId || 0;
  $("goalWeightG").value = c.goalWeightG.toFixed(1);
  [
    "rinseGesture",
    "rinseDuration",
    "operationalWall",
    "retareWindow",
    "postTareBaselineGrace",
    "bbwProtection",
    "retareStabilityMaxGap",
    "retareStabilityMinDuration",
  ].forEach((k) => ($(k + "S").value = String(c[k + "Ms"] / 1000)));
  const paddleReturnReminderLimit = $("paddleReturnReminderMaxDurationMin");
  if (paddleReturnReminderLimit)
    paddleReturnReminderLimit.value = String(c.paddleReturnReminderMaxDurationMs / 60000);
  $("lastShotCooldownMin").value = String((c.lastShotCooldownMs ?? 36e5) / 60000);
  [
    ["minimumCupWeightG", 10],
    ["cupRemovedWeightG", -3],
    ["retareStabilitySamples", 3],
    ["retareStabilityToleranceG", 2],
    ["scaleTimerStopExtraDelayMs", 0],
  ].forEach(([k, d]) => ($(k).value = String(c[k] ?? d)));
  $("dripDelayS").value = String((c.dripDelayMs ?? 3000) / 1000);
  [
    "autoTare",
    "autoTareOutsideBrew",
    "retareAccessoryOutsideBrew",
    "brewByWeight",
    "soundAlertsEnabled",
    "canTareStartTimer",
    "bookooMuteOnBuzzerOnly",
    "firstDropBeep",
    "scaleConnectedLed",
    "buzzerScaleLostBeep",
    "buzzerAutoToManualGuardEndBeep",
    "buzzerManualNoScaleBeep",
    "buzzerScaleConnectedBeep",
    "autoRetare",
    "fastExtractionGuardEnabled",
    "avoidAccidentalTouchEnabled",
    "touchStopFallbackEnabled",
    "slowExtractionGuardEnabled",
    "grindHintsEnabled",
    "autoToManualGuardEnabled",
    "cupProtectionEnabled",
    "stopIfCupRemoved",
    "requireCupToStart",
    "rinseEnabled",
    "noScaleAllowRinseWhileArmed",
  ].forEach((k) => {
    const el = $(k);
    if (el) el.checked = !!c[k];
  });
  $("noScaleBbwMode").value = c.noScaleBbwMode;
  $("buzzerExtendedPulseRate").value = extRate(c.buzzerExtendedPulseRate);
  $("buzzerSlowExtendedPulseRate").value = extRate(c.buzzerSlowExtendedPulseRate);
  $("alertOutputChannel").value = ["scale_only", "buzzer_only", "scale_priority"].includes(
    c.alertOutputChannel,
  )
    ? c.alertOutputChannel
    : "scale_priority";
  $("bookooConnectBeepLevel").value = String(
    [0, 1, 2, 3, 4, 5].includes(+c.bookooConnectBeepLevel) ? +c.bookooConnectBeepLevel : 4,
  );
  if ($("scalePreference")) {
    $("scalePreference").value = c.scaleMacCacheMode;
    updateScalePreferenceOptions();
  }
  if ($("paddleMode"))
    $("paddleMode").value = ["auto", "natural", "original"].includes(c.paddleMode)
      ? c.paddleMode
      : "natural";
  if ($("stopPulseMs")) $("stopPulseMs").value = String(c.stopPulseMs ?? 300);
  if ($("maxSinglePressMs")) $("maxSinglePressMs").value = String(c.maxSinglePressMs ?? 1000);
  if ($("momentaryStartEdge"))
    $("momentaryStartEdge").value = ["press", "release"].includes(c.momentaryStartEdge)
      ? c.momentaryStartEdge
      : "press";
  if ($("reedConfirmTimeoutS"))
    $("reedConfirmTimeoutS").value = String((c.reedConfirmTimeoutMs ?? 1000) / 1000);
  if ($("assumeIdleWhenScaleConnects"))
    $("assumeIdleWhenScaleConnects").checked = c.assumeIdleWhenScaleConnects !== false;
  if ($("shotReactTimeoutS")) $("shotReactTimeoutS").value = String(c.shotReactTimeoutS ?? 12);
  for (const k of ["doseG", "fastBeforeS", "slowAfterS", "fastMarginG", "slowMarginG"])
    $(k).value = (+c[k]).toFixed(1);
  $("scaleLossFixedS").value = c.scaleLossFixedS > 0 ? String(c.scaleLossFixedS) : "";
  // The baseline is the device; an edit the device now holds is settled.
  uiState.formBaseline = snapshotControls(els);
  for (const id in edited) if (same(edited[id], uiState.formBaseline[id])) delete edited[id];
  restoreSnapshot(edited);
  formRev = c.revision;
  configRevision = c.revision;
  configLoaded = true;
  updateConfigGroups();
}
function updateHomeAdminActions(unlocked, remoteEnabled) {
  const panel = __REMOTE_CONTROL__ && $("actionsPanel");
  if (!panel) return;
  const show = !!unlocked && !!remoteEnabled;
  panel.classList.toggle("hidden", !show);
  document.body.classList.toggle("homeAdminActions", show);
}
function formatBackflushState(s) {
  const b = s.backflush;
  if (!b) return "";
  if (s.state === "BACKFLUSH_CANDIDATE") return __WEBUI_TEXT__("runtime.backflush_checking");
  if (s.state === "BACKFLUSH_RUNNING") return __WEBUI_TEXT__("runtime.backflush_running");
  if (s.state === "REQUIRES_OFF" && b.stopReason !== "none")
    return b.stopReason === "unconfirmed_end"
      ? __WEBUI_TEXT__("runtime.backflush_other")
      : b.stopReason === "mode_ended"
        ? __WEBUI_TEXT__("runtime.backflush_return_off")
        : __WEBUI_TEXT__("runtime.backflush_interrupted");
  if (b.waiting && b.ready) return __WEBUI_TEXT__("runtime.backflush_waiting");
  return (b.waiting || b.unresolved) && !s.cycle?.active
    ? __WEBUI_TEXT__("runtime.backflush_restart")
    : "";
}
function applyHomeStatus(s) {
  const admin = !!s.adminUnlocked,
    canControl = controlsMutable,
    remoteReady = !!s.remoteControlEnabled,
    relayStartReady = s.safety.state !== "LOCKOUT",
    live = !!s.machineRunning,
    shot = $("stopButton"),
    force = $("forcePulseButton"),
    g = shot && shot.querySelector(".g"),
    t = shot && shot.querySelector(".t"),
    switchOnly = s.machineType === "momentary";
  syncAdminSessionUi(admin, remoteReady);
  // Stream frames repeat most values: unchanged text is not rewritten.
  setHomeSub("machineState", formatMachineState(s));
  setHomeSub("state", formatBackflushState(s) || s.stateLabel || "—");
  updateStateTone(s);
  setHomeSub("cupState", formatCupState(s));
  setHomeSub("cupWeight", formatCupWeight(s));
  setHomeSub("cupAccessory", formatCupAccessory(s));
  setHomeSub("idleTareStatus", formatIdleTare(s));
  setHomeSub("scale", formatScaleStatus(s));
  setHomeSub("preferredScale", formatPreferredScale(s));
  updateScaleRenameUi(
    "preferredScaleRenameWrap",
    s.scale?.preferredMac || "",
    scaleDisplayName(s.scale),
  );
  setHomeSub("scaleWeight", formatScaleWeight(s));
  setHomeSub("scaleTimer", formatScaleTimer(s));
  // Painted on the next frame, once this status has set the new cut model.
  scheduleShotPaint();
  const rinseButton = $("rinseButton");
  if (rinseButton)
    rinseButton.disabled = !(
      admin &&
      remoteReady &&
      relayStartReady &&
      canControl &&
      s.config.rinseEnabled === true
    );
  if (shot) {
    // A Stop tap landing just after the shot ends must not start one.
    if (shot.dataset.mode === "stop" && !live) shot.dataset.stoppedAt = Date.now();
    shot.dataset.mode = live ? "stop" : "start";
    shot.title = live
      ? __WEBUI_TEXT__("runtime.stop_shot_2")
      : __WEBUI_TEXT__("runtime.start_shot_2");
    shot.setAttribute("aria-label", shot.title);
    shot.classList.toggle("btnDanger", live);
    if (g) g.textContent = live ? "■" : "▶";
    if (t) t.textContent = shot.title;
    shot.disabled = !admin || (!live && !(remoteReady && relayStartReady && canControl));
    if (force) force.disabled = !(admin && remoteReady && relayStartReady && webUiOwner);
    ["overrideIdleLink", "overrideBrewingLink"].forEach((i) => {
      const a = $(i);
      if (!a) return;
      const off = !canControl && !switchOnly;
      a.classList.toggle("fieldOff", off);
      a.setAttribute("aria-disabled", off ? "true" : "false");
    });
  }
  ingestPresets(s);
  updateRuleChartFromStatus(s);
  renderHomeCards();
  renderHomePresetAccordion();
}
function lineaMicraAction(action) {
  return command(
    "/api/v1/machine/linea-micra",
    { action },
    false,
    "",
    __WEBUI_TEXT__("runtime.micra_action_failed"),
  );
}
function statusPageOk(v, s) {
  const c = s && s.config;
  if (!c || typeof s.configMutable !== "boolean") return !1;
  return v === "home"
    ? !!(
        typeof s.adminUnlocked === "boolean" &&
        typeof c.soundAlertsEnabled === "boolean" &&
        s.safety &&
        s.scale &&
        s.presets &&
        s.cycle &&
        s.noScaleShotGuard &&
        typeof s.machineState === "string" &&
        s.cupPresence
      )
    : v === "admin"
      ? !!(
          typeof s.adminUnlocked === "boolean" &&
          s.network &&
          (s.adminUnlocked
            ? s.bleScan &&
              typeof s.bleScan.scanIntensity === "string" &&
              typeof c.timezoneId === "string" &&
              c.ntpServerPreset != null &&
              s.ota &&
              typeof s.ota.available === "boolean" &&
              s.webhooks &&
              typeof s.webhooks.enabled === "boolean" &&
              s.lastCommand &&
              typeof s.lastCommand.requestId === "number"
            : typeof s.network.configState === "string")
        )
      : !1;
}
async function loadStatus() {
  if (document.hidden || !webUiPollingActive()) return;
  const seq = viewSeq;
  try {
    const v = activeView,
      s =
        v === "home"
          ? await loadHomeStatus()
          : v === "diagnostic"
            ? await loadDiagnosticStatus()
            : v === "settings"
              ? await loadSettingsStatus()
              : await api("/api/v1/status/admin");
    await viewReady;
    if (seq !== viewSeq || !webUiPollingActive()) return false;
    if (!s || (v !== "diagnostic" && v !== "settings" && !statusPageOk(v, s)))
      throw new Error(__WEBUI_TEXT__("runtime.invalid_status"));
    lastStatusAt = Date.now();
    if (v !== "diagnostic") applyCommonStatus(s);
    if (s.snapshotStale) setMutable(false);
    const apply = viewStatusHandlers[v];
    if (apply) apply(s);
    // Presets reach Settings only with a Home frame that changes them, so a
    // view mounted after that frame applies the model it already holds.
    if (v === "settings") ingestPresets(s);
    if (v === "admin" && s.adminUnlocked) {
      await timezoneCatalogPromise;
      await timezonePreviewLoad?.catch(() => {});
    }
    noteReachOk();
    armStatusTimer();
    return true;
  } catch (e) {
    if (seq === viewSeq) noteReachFail(e);
    return false;
  }
}
// A record page names its stream after the view and refreshes through it.
function refreshStatus() {
  if (activeView === "settings") return scheduleSettingsPaint();
  return withPollGate(streamHandlers[activeView]?.refresh || loadStatus);
}

async function command(path, value = {}, soft, okMsg, failMsg, busyId) {
  return withCommandGate(async () => {
    commandBusy = true;
    const sb = busyId ? $(busyId) : null;
    if (sb) sb.classList.add("busy");
    try {
      const noReconnect =
          path.endsWith("/network") && value.action === "save" && value._noReconnectWait,
        firmwareModeSave = path.endsWith("/firmware-mode"),
        presetOp = path.endsWith("/presets"),
        quickOp = path.endsWith("/quick-settings"),
        waitForRevision = presetOp || quickOp || path.endsWith("/config"),
        previousRevision = configRevision;
      const payload = value;
      delete payload._noReconnectWait;
      const accepted = await api(path, { method: quickOp ? "PUT" : "POST", body: body(payload) });
      clearFieldErrors();
      noteReachOk();
      if (noReconnect) uiState.savedStaWifiSleep = !!$("staWifiSleep").checked;
      if (path.endsWith("/network") && value.action === "save" && !noReconnect) {
        beginNetworkReconnectWait();
        return true;
      }
      if (firmwareModeSave) {
        beginNetworkReconnectWait();
        return true;
      }
      if (waitForRevision) {
        // Home frames refresh lastCommandStatus and configRevision; REST
        // backs off (250 ms to 1 s) only while the stream is stale.
        let applied = false,
          polled = false;
        for (let i = 0, next = 0, gap = 2.5; i < 40 && !applied; i++) {
          if (i) await sleep(100);
          if ((polled = homeStale && i >= next)) {
            await refreshStatus();
            next = i + gap;
            gap = Math.min(10, gap * 2);
          }
          const result = lastCommandStatus;
          if (result?.requestId !== accepted.requestId) continue;
          if (result.state === "FAILED" || result.state === "CANCELED")
            throw new Error(__WEBUI_TEXT__("runtime.device_did_not_apply_the_change"));
          // The device rejects a preset operation until the previous one is saved.
          // A duration or an unchanged Quick Setting keeps the revision.
          applied =
            (result.state === "PERSISTED" || (!presetOp && result.state === "APPLIED")) &&
            (quickOp || configRevision !== previousRevision);
        }
        if (!applied) throw new Error(__WEBUI_TEXT__("runtime.device_did_not_apply_the_change"));
        // Repaint the REST-backed Admin form once; other views follow streams.
        if (!polled && !quickOp) await refreshStatus();
      } else await refreshStatus();
      if (okMsg !== "")
        message(
          okMsg ||
            (noReconnect
              ? __WEBUI_TEXT__("runtime.network_preferences_saved")
              : commandOkMessage(path, value)),
          "ok",
        );
      return true;
    } catch (e) {
      if (soft) throw e;
      if (failMsg !== "")
        message(formatCommandError(failMsg || commandFailMessage(path, value), e), "error");
      await refreshStatus();
      return false;
    } finally {
      commandBusy = false;
      if (sb) sb.classList.remove("busy");
    }
  });
}
export {
  pad2,
  randomHex,
  sleep,
  diagnosticVisible,
  saveFile,
  resetWebUiInactivity,
  activeView,
  compatibilityModeOn,
  noteWebUiPowerActivity,
  initHeaderSignals,
  homeStatusCache,
  shotRecorded,
  formatMicraCleaning,
  $,
  webUiOwner,
  webUiPollingActive,
  noteWebUiInteraction,
  exitFullScreenOnPop,
  claimWebUiOwnership,
  startSetupSession,
  setMutable,
  controlsMutable,
  showPageBoot,
  hideHomeBoot,
  withPollGate,
  body,
  number,
  message,
  reveal,
  formatCommandError,
  showFieldError,
  clearFieldErrors,
  api,
  command,
  withCommandGate,
  withBaseRev,
  autosaveSection,
  noteEdit,
  flushEdits,
  registerViewStatus,
  setEnsureViewHook,
  setActiveView,
  setViewPollHooks,
  renderRoute,
  stopViewPolls,
  refreshStatus,
  loadStatus,
  armStatusTimer,
  stopDiagnosticStream,
  ensureBackgroundStreams,
  armStickyBar,
  setStatsRenderers,
  registerStream,
  setSettingsForm,
  paint,
  viewReady,
  shotResync,
  decodeCurveDeltas,
  updateFirmwareFooter,
  noteBootId,
  buildRuleChartModel,
  shotStatsLine,
  drawPresetGraph,
  paceStartNote,
  bbwFormPresetId,
  setSettingsRenderer,
  initGroupRows,
  renderGroupRows,
  formatHumanTime,
  deviceUtcSec,
  resetNetworkAddressLoaded,
  formatWallTimeLocal,
  applyHomeStatus,
  updateConfigGroups,
  updateScalePreferenceOptions,
  saveBrewPreset,
  cleanBrewForm,
  settingsSectionOf,
  snapshotControls,
  restoreSnapshot,
  lineaMicraAction,
  updateNetworkPasswordState,
  updateStaticIpFieldsState,
  renameScale,
  themeMode,
  paintTheme,
  heroTintMode,
  cycleTheme,
  populateTimezoneOptions,
  cardDismissed,
  dismissCard,
  startNewCoffee,
  invalidateSettingsHydration,
  clearBrewDirty,
  presetState,
  selectedPreset,
  askConfirm,
  sub,
  brewRatio,
  bootId,
  formRev,
  noteReachOk,
  noteReachFail,
  uiState,
  buildShotSparkModel,
  sendUiOperation,
  firmwareVersion,
  shotRuleModel,
  cutLine,
  placePill,
  escHtml,
  PILL,
  bindScrub,
  presetGraph,
  observePresetHost,
  fadeFill,
  axisLabel,
  formatShotEnded,
  wrapTimeEl,
  verdictLabel,
  setEmptyState,
  shotWs,
  acquireDeviceSlot,
  webUiHeaders,
  storeSession,
  deviceLabel,
  releaseDeviceSlot,
  formatTzLabel,
  ms,
  statusUtcAnchorSec,
  statusUtcAnchorAt,
  statusTimezoneOffsetMinutes,
  formatBackflushState,
  formatCupWeight,
  formatScaleWeight,
  formatScaleTimer,
  syncAdminSessionUi,
  refreshTimezonePreview,
  updateTimezoneControls,
  browserTimezone,
  timezoneCatalog,
  developmentMode,
  lockAdminUi,
  updateWifiSleepState,
  HERO_TINT_KEY,
  HERO_TINT_MODES,
  scheduleShotPaint,
  timezonePreviewAnchor,
  timezonePreviewPending,
  renderTimezonePreview,
  updateScaleRenameUi,
  clearBrewWarningsDom,
  settingsSectionEls,
  refreshFieldAria,
  formNumber,
  sToMs,
  extRate,
  updateBullseyeControls,
  updateBuzzerAlertVisibility,
  loadSettingsConfig,
  updateBbwControls,
  configLoaded,
};
