"use strict";
// First-use onboarding (/setup) on the controller's setup network (SoftAP
// captive portal), in three parts: (1) how to brew and the owner password,
// saved together; (2) Wi-Fi, where Connect only tests a network and Save Wi-Fi
// stores it; (3) a summary of what is saved. Joining the setup network proved
// the device password, so the firmware serves these routes to SoftAP peers
// without the admin unlock or a UI claim. Every step is rebuilt from firmware
// status, so a reload, a second window, or a phone that dropped off the setup
// network lands on the step the device is in.
import * as R from "/js/runtime.js?v=__FW_ASSET_TAG__";
const $ = R.$;
const CONNECT_WAIT_MS = 60000;
// Mirrors the firmware's LIVE_APPLY_ATTEMPT_MS onboarding bound: the device
// fails a wrong-password attempt by 20 s, so the countdown tells the user
// how long this can take before they see a result.
const ATTEMPT_WINDOW_MS = 20000;
// A phase left over from an earlier attempt is only a verdict once the
// device reported this attempt running, or after this long.
const VERDICT_GRACE_MS = 8000;
const POLL_MS = 1000,
  IDLE_POLL_MS = 2500,
  SCAN_POLL_MS = 500,
  SCAN_WAIT_MS = 30000;
// Mirrors DEFAULT_DEVICE_PASSWORD: the public factory password is never kept.
const FACTORY_PASSWORD = "ineedacoffee";
// Wildcard DNS resolves the OS probe hosts to the device itself, so pointing
// the browser at one triggers the portal verdict: after "skip"/"finish" the
// device answers success and the captive window closes as "logged in".
const PROBE_URL = /Android/i.test(navigator.userAgent)
  ? "http://connectivitycheck.gstatic.com/generate_204"
  : "http://captive.apple.com/hotspot-detect.html";
const STATES = [
  "welcome",
  "mode",
  "recipe",
  "protect",
  "saving",
  "scanning",
  "form",
  "connecting",
  "connected",
  "success",
  "locked",
];
const PART = {
  mode: 1,
  recipe: 1,
  protect: 1,
  scanning: 2,
  form: 2,
  connecting: 2,
  connected: 2,
  success: 3,
};
const BLOCKED = {
  activator_on: __WEBUI_TEXT__("setup.blocked_paddle"),
  active_shot: __WEBUI_TEXT__("setup.blocked_brewing"),
  machine_running: __WEBUI_TEXT__("setup.blocked_brewing"),
  maintenance: __WEBUI_TEXT__("setup.blocked_saving"),
};
let bound = false,
  pollTimer = 0,
  // Set by start() and cleared by stop(): the watchdog only revives a chain
  // the view still wants.
  running = false,
  pollToken = 0,
  busyToken = -1,
  state = "idle",
  connectError = false,
  errorTitle = "",
  blocked = "",
  rescanWhenFree = false,
  lastNetworks = null,
  lastPhase = "",
  lastNet = {},
  scanStartedAt = 0,
  statusAt = 0,
  waitStartedAt = 0,
  sawAttempt = false,
  savedWifiSleep = true,
  deviceHost = "",
  // Firmware off: part one also asks how to brew.
  compat = false,
  brewRestored = false,
  lastBrew = "none",
  savedPreset = "255",
  apSsid = "",
  // Factory (public) device password: part one asks for the owner's own.
  factoryPassword = false,
  // This session: what is being changed from the summary, the note shown
  // after a save, and where Wi-Fi stands ("joined" or "skipped").
  editing = "",
  savingPart = 1,
  saving = false,
  banner = "",
  savedPass = "",
  wifiState = "",
  wifiSsid = "",
  pendingSsid = "",
  confirmEndsAt = 0,
  closingAt = 0,
  closeLater = "";

const formLike = (name) => name === "form" || name === "scanning";
const polling = () => formLike(state) || state === "connecting" || state === "connected";
const ticking = () => polling() || (state === "success" && closingAt > 0);
const byWeight = () => document.querySelector("input[name=setupMode]:checked").value === "weight";
const recipeInput = () => document.querySelector("input[name=setupRecipe]:checked");
const recipeName = (value = recipeInput().value) =>
  document
    .querySelector(`input[name=setupRecipe][value="${value}"]`)
    .closest("label")
    .querySelector("b").textContent;
const sub = (text, values) => text.replace(/\{(\w)\}/g, (m, k) => values[k] ?? m);
const minutes = (ms) => {
  const s = Math.max(0, Math.ceil(ms / 1000));
  return Math.floor(s / 60) + ":" + R.pad2(s % 60);
};
const brewPayload = () =>
  byWeight()
    ? { brewByWeight: true, presetId: Number(recipeInput().value) }
    : { brewByWeight: false };
// Part one is done once the owner's password is set and, firmware off, the
// brewing choice is saved.
const part1Done = () => !factoryPassword && (!compat || lastBrew !== "none");
const part1Steps = () => [
  ...(compat ? ["mode"] : []),
  ...(compat && byWeight() ? ["recipe"] : []),
  ...(compat || factoryPassword ? ["protect"] : []),
];
const needPassword = () => editing === "password" || (factoryPassword && editing !== "brew");

function heading(name) {
  if (name === "welcome")
    return [
      __WEBUI_TEXT__("setup.welcome_title"),
      compat ? __WEBUI_TEXT__("setup.welcome_intro") : __WEBUI_TEXT__("setup.welcome_intro_on"),
    ];
  if (name === "mode")
    return [__WEBUI_TEXT__("setup.mode_title"), __WEBUI_TEXT__("setup.mode_later")];
  if (name === "recipe")
    return [__WEBUI_TEXT__("setup.recipe_title"), __WEBUI_TEXT__("setup.recipe_intro")];
  if (name === "protect")
    return editing === "password"
      ? [
          __WEBUI_TEXT__("setup.change_password_title"),
          __WEBUI_TEXT__("setup.change_password_intro"),
        ]
      : needPassword()
        ? [__WEBUI_TEXT__("setup.password_title"), __WEBUI_TEXT__("setup.password_intro")]
        : [__WEBUI_TEXT__("setup.choices_title"), ""];
  if (formLike(name)) {
    if (connectError) return [errorTitle, ""];
    return [
      __WEBUI_TEXT__("setup.title"),
      editing === "wifi" && wifiState === "joined"
        ? sub(__WEBUI_TEXT__("setup.replace_intro"), { x: wifiSsid })
        : !compat
          ? __WEBUI_TEXT__("setup.intro")
          : byWeight()
            ? __WEBUI_TEXT__("setup.wifi_intro_weight")
            : __WEBUI_TEXT__("setup.wifi_intro_usual"),
    ];
  }
  if (name === "connecting") return [__WEBUI_TEXT__("setup.title_connecting"), ""];
  if (name === "connected")
    return [sub(__WEBUI_TEXT__("setup.connected_title"), { x: pendingSsid }), ""];
  if (name === "success") return [__WEBUI_TEXT__("setup.title_saved"), ""];
  return [__WEBUI_TEXT__("setup.title_locked"), ""];
}

function show(name) {
  state = name;
  for (const key of STATES) $("setup" + key[0].toUpperCase() + key.slice(1)).hidden = key !== name;
  // "Saving…" keeps the heading of the step it saves.
  if (name !== "saving") {
    const [title, intro] = heading(name);
    $("setupTitle").textContent = title;
    $("setupIntro").textContent = intro;
  }
  $("setupIntro").hidden = name === "saving" || !$("setupIntro").textContent;
  $("setupError").hidden = !$("setupErrorText").textContent || !formLike(name);
  $("setupBlocked").hidden = !blocked || !formLike(name);
  // A saved note stays with the part it opens, until the next step.
  if (!["scanning", "form", "success"].includes(name)) banner = "";
  $("setupSaved").textContent = banner;
  $("setupSaved").hidden = !banner;
  renderProgress();
  // A step change that hid the focused control hands focus to the heading.
  if (document.activeElement?.closest("[hidden]")) $("setupTitle").focus({ preventScroll: true });
}

// Three parts, each line filling with its own: one share per part-one step
// and per Wi-Fi stage, full on each part's save.
function renderProgress() {
  const part = editing ? 0 : state === "saving" ? savingPart : PART[state] || 0;
  const steps = part1Steps(),
    n = steps.indexOf(state),
    savingOf = (p) => state === "saving" && savingPart === p;
  const fill = [
    0,
    part > 1 || savingOf(1) ? 1 : Math.max(n, 0) / steps.length,
    part > 2 || savingOf(2) ? 1 : { connecting: 1 / 3, connected: 2 / 3 }[state] || 0,
  ];
  $("setupProgress").hidden = !part;
  [...$("setupProgress").children].forEach((li, i) => {
    const done = i + 1 < part,
      current = i + 1 === part;
    li.style.setProperty("--fill", fill[i]);
    li.dataset.state = done ? "done" : current ? "current" : "todo";
    if (current) li.setAttribute("aria-current", "step");
    else li.removeAttribute("aria-current");
    li.firstElementChild.textContent = done ? "✓" : i + 1;
  });
  $("setupStep").hidden = !!editing || n < 0 || steps.length < 2;
  $("setupStep").textContent = sub(__WEBUI_TEXT__("setup.step_of"), {
    n: n + 1,
    m: steps.length,
  });
}

// One visible verdict above the form: focused and announced, so it is seen
// on a small captive sheet; Connect is the single retry path.
function showError(text, connect, title = __WEBUI_TEXT__("setup.title_error")) {
  connectError = connect;
  errorTitle = title;
  $("setupErrorText").textContent = text;
  $("setupErrorHint").hidden = !connect;
  showForm();
  const box = $("setupError");
  box.scrollIntoView({ block: "start" });
  box.focus({ preventScroll: true });
}

// The Wi-Fi form, searching first when this page has no list yet (it
// resumed on a later step after a reload or a dropped phone).
function showForm() {
  if (!lastNetworks) return startScan(true);
  renderPicker(lastNetworks);
  show("form");
  kick();
}

function clearError() {
  connectError = false;
  $("setupErrorText").textContent = "";
}

function setBlocked(reason) {
  blocked =
    reason && reason !== "none" ? BLOCKED[reason] || __WEBUI_TEXT__("setup.blocked_busy") : "";
  $("setupBlocked").textContent = blocked;
  $("setupBlocked").hidden = !blocked || !formLike(state);
  $("setupConnect").disabled = $("setupSearchAgain").disabled = !!blocked;
  if (!blocked && rescanWhenFree && state === "form") {
    rescanWhenFree = false;
    startScan(connectError);
  }
}

function setFieldError(id, text) {
  const el = $(id);
  el.hidden = !text;
  el.textContent = text || "";
}

function signalGlyph(rssi) {
  return rssi >= -60 ? "▂▄▆" : rssi >= -80 ? "▂▄" : "▂";
}

function selectedNetwork() {
  const open = $("setupNetwork").selectedOptions[0];
  return open && open.value ? open : null;
}

function syncPasswordVisibility() {
  const network = selectedNetwork(),
    manual = $("setupHiddenToggle").checked;
  const open = manual ? false : network !== null && network.dataset.open === "true";
  $("setupPasswordWrap").hidden = open;
  $("setupPassword").disabled = open;
}

function renderPicker(networks) {
  const select = $("setupNetwork");
  const previous = select.value;
  select.replaceChildren();
  const prompt = document.createElement("option");
  prompt.value = "";
  prompt.textContent = networks.length
    ? __WEBUI_TEXT__("setup.choose_network")
    : __WEBUI_TEXT__("setup.no_networks_found");
  select.appendChild(prompt);
  for (const n of networks) {
    const option = document.createElement("option");
    option.value = n.ssid;
    option.dataset.open = String(n.open);
    option.textContent = n.open
      ? signalGlyph(n.rssi) + "  " + n.ssid
      : signalGlyph(n.rssi) + "  " + n.ssid + "  🔒";
    select.appendChild(option);
  }
  if (previous && [...select.options].some((o) => o.value === previous)) select.value = previous;
  syncPasswordVisibility();
  // Changing a saved network from the summary returns there instead of skipping.
  const change = editing === "wifi";
  $("setupSkip").hidden = change;
  $("setupFormBack").hidden = !change;
  $("setupFormBack").textContent =
    wifiState === "joined"
      ? sub(__WEBUI_TEXT__("setup.keep_network"), { x: wifiSsid })
      : __WEBUI_TEXT__("setup.back");
}

// While configuration is blocked the scan cannot run: keep the form, explain
// why in the card, and search as soon as the controller is free again.
function deferScan() {
  rescanWhenFree = true;
  renderPicker(lastNetworks || []);
  show("form");
}

async function startScan(keepError = false) {
  if (!keepError) clearError();
  if (blocked) return deferScan();
  scanStartedAt = 0;
  show("scanning");
  try {
    await R.api("/api/v1/network/scan", { method: "POST", body: "{}" });
  } catch (e) {
    if (e && e.code === "ADMIN_LOCKED") return show("locked");
    if (e && e.code === "CONFIG_LOCKED_DURING_ACTIVE_CYCLE") return deferScan();
    return scanFailed(e);
  }
  scanStartedAt = Date.now();
  kick();
}

async function pollScan() {
  let d;
  try {
    d = await R.api("/api/v1/network/scan");
  } catch (e) {
    if (state !== "scanning") return;
    return e && e.code === "ADMIN_LOCKED" ? show("locked") : scanFailed(e);
  }
  if (state !== "scanning") return;
  if (d.state === "READY") {
    lastNetworks = d.networks || [];
    renderPicker(lastNetworks);
    return show("form");
  }
  if (d.state === "CANCELED" && blocked) return deferScan();
  // Page-side watchdog: whatever the device does, a scan never outlives
  // this bound on the onboarding card.
  if (d.state === "FAILED" || d.state === "CANCELED" || Date.now() - scanStartedAt >= SCAN_WAIT_MS)
    scanFailed(null);
}

function scanFailed(error) {
  lastNetworks = lastNetworks || [];
  showError(
    error
      ? R.formatCommandError(__WEBUI_TEXT__("setup.scan_failed"), error)
      : __WEBUI_TEXT__("setup.scan_failed"),
    false,
  );
}

async function pollStatus() {
  let s;
  try {
    s = await R.api("/api/v1/status/admin");
  } catch (e) {
    return null; /* transient poll failure: keep waiting within the bounded window */
  }
  statusAt = Date.now();
  applyStatus(s);
  return s;
}

// The device's onboarding phase decides the Wi-Fi step: a running attempt is
// "connecting", a joined one waits on "Connected" for Save Wi-Fi, a confirmed
// one is saved, and a finished attempt is a verdict only for the attempt this
// page is waiting on.
function applyStatus(s) {
  if (s.adminUnlocked === false) return show("locked");
  factoryPassword = s.factoryPassword === true;
  const n = (lastNet = s.network || {});
  compat = s.compatibilityMode === true;
  lastBrew = n.setupBrew || "none";
  apSsid = n.apSsid || "";
  if (compat && !brewRestored) restoreBrew(n);
  if (typeof n.wifiSleep === "boolean") savedWifiSleep = n.wifiSleep;
  deviceHost = n.mdnsHost || n.deviceName || "";
  lastPhase = n.setup || "";
  setBlocked(s.configMutable === false ? s.configLockReason : "");
  if (state !== "connecting" && state !== "connected") return;
  if (lastPhase === "confirmed") return wifiSaved(n);
  if (lastPhase === "joined")
    return state === "connected" ? noteConfirmWindow(n) : showConnected(n);
  if (lastPhase === "queued" || lastPhase === "connecting") {
    if (state !== "connecting") showConnecting(n.ssid || "");
    sawAttempt = true;
    return;
  }
  // The tested network was dropped unsaved (its save window ran out).
  if (state === "connected")
    return showError(
      __WEBUI_TEXT__("setup.error_not_saved"),
      true,
      __WEBUI_TEXT__("setup.title_not_saved"),
    );
  const waited = Date.now() - waitStartedAt;
  if (sawAttempt || waited >= VERDICT_GRACE_MS)
    showError(
      n.wifiConfigured === false
        ? __WEBUI_TEXT__("setup.error_reverted")
        : __WEBUI_TEXT__("setup.error_could_not_connect"),
      true,
    );
}

// A reload or second phone picks up the choice already saved on the device.
function restoreBrew(n) {
  brewRestored = true;
  if (lastBrew !== "usual" && lastBrew !== "weight") return;
  if (lastBrew === "weight") savedPreset = String(n.setupPresetId);
  resetChoices();
}

// Back to the saved choice after a change from the summary is cancelled.
function resetChoices() {
  document.querySelector(
    `input[name=setupMode][value=${lastBrew === "weight" ? "weight" : "usual"}]`,
  ).checked = true;
  const recipe = document.querySelector(`input[name=setupRecipe][value="${savedPreset}"]`);
  if (recipe) recipe.checked = true;
}

function showSaving(part, text) {
  savingPart = part;
  $("setupSavingText").textContent = text;
  show("saving");
}

function renderWelcome() {
  $("setupPart1Hint").textContent = compat
    ? factoryPassword
      ? __WEBUI_TEXT__("setup.part1_hint")
      : __WEBUI_TEXT__("setup.part1_hint_choices")
    : __WEBUI_TEXT__("setup.part1_hint_password");
  $("setupPart3Hint").textContent = compat
    ? __WEBUI_TEXT__("setup.part3_hint")
    : __WEBUI_TEXT__("setup.part3_hint_on");
  show("welcome");
}

// The save step of part one: the owner password (when needed) and, firmware
// off, a list of exactly what the button stores.
function showProtect() {
  const change = editing === "password",
    pass = needPassword(),
    weight = compat && byWeight();
  $("setupNewPassWrap").hidden = !pass;
  $("setupSaveBox").hidden = change;
  $("setupSaveModeRow").hidden = !compat;
  $("setupSaveMode").textContent = byWeight()
    ? __WEBUI_TEXT__("setup.brewing_weight")
    : __WEBUI_TEXT__("setup.brewing_usual");
  $("setupSaveRecipeRow").hidden = !weight;
  $("setupSaveRecipe").textContent = recipeName();
  $("setupSavePassRow").hidden = !pass;
  $("setupSaveNote").textContent = weight
    ? __WEBUI_TEXT__("setup.note_weight_saved")
    : pass
      ? __WEBUI_TEXT__("setup.note_phone_connected")
      : "";
  $("setupSave").textContent = change
    ? __WEBUI_TEXT__("setup.save_password")
    : editing
      ? __WEBUI_TEXT__("setup.save")
      : __WEBUI_TEXT__("setup.save_continue");
  $("setupProtectBack").textContent = change
    ? __WEBUI_TEXT__("setup.cancel")
    : __WEBUI_TEXT__("setup.back");
  setFieldError("setupProtectError", "");
  validatePassword();
  show("protect");
}

function validatePassword(blurred) {
  const np = $("setupNewPassword").value,
    cp = $("setupConfirmPassword").value;
  const ok = {
    len: np.length >= 8 && np.length <= 63,
    factory: np.length > 0 && np !== FACTORY_PASSWORD,
    match: cp.length > 0 && np === cp,
  };
  for (const li of $("setupPassRules").children) li.dataset.ok = ok[li.dataset.rule];
  const mismatch = cp.length > 0 && !ok.match && (blurred || cp.length >= np.length);
  $("setupConfirmError").hidden = !mismatch;
  $("setupConfirmPassword").setAttribute("aria-invalid", String(mismatch));
  $("setupNewPassword").setAttribute("aria-invalid", String(np === FACTORY_PASSWORD));
  $("setupSave").disabled = needPassword() && !(ok.len && ok.factory && ok.match);
}

// The password command runs on the network task; its result (PERSISTED or
// FAILED) is published as lastCommand in the status this page already reads.
async function setPassword(newPassword) {
  const accepted = await R.api("/api/v1/device/password", {
    method: "POST",
    body: R.body({ newPassword }),
  });
  let saved = false;
  for (let i = 0; i < 40; i++) {
    await R.sleep(250);
    const s = await pollStatus(),
      c = s?.lastCommand;
    if (c?.requestId === accepted.requestId) {
      if (c.state === "FAILED" || c.state === "CANCELED") break;
      saved ||= c.state === "PERSISTED" || c.state === "APPLIED";
    }
    // The result is published just before the controller releases its
    // maintenance lock: leaving earlier would find the Wi-Fi scan locked.
    if (saved && s && s.configLockReason !== "maintenance") return;
  }
  if (!saved) throw new Error(__WEBUI_TEXT__("setup.password_not_saved"));
}

// Part one, or a change made from the summary. Password first: should the
// brewing save fail, the same button retries both, and a secured controller
// is the safer half to keep.
async function savePart1() {
  if (saving || $("setupSave").disabled) return;
  saving = true;
  const first = !part1Done(),
    pass = needPassword(),
    brew = compat && editing !== "password",
    password = $("setupNewPassword").value;
  showSaving(1, __WEBUI_TEXT__("setup.saving"));
  try {
    if (pass) await setPassword(password);
    if (brew)
      await R.api("/api/v1/network", {
        method: "POST",
        body: R.body({ action: "brew", ...brewPayload() }),
      });
  } catch (e) {
    saving = false;
    if (e && e.code === "ADMIN_LOCKED") return show("locked");
    show("protect");
    setFieldError(
      "setupProtectError",
      R.formatCommandError(__WEBUI_TEXT__("setup.save_failed_part1"), e),
    );
    return;
  }
  saving = false;
  if (brew) {
    lastBrew = byWeight() ? "weight" : "usual";
    if (byWeight()) savedPreset = recipeInput().value;
  }
  if (pass) {
    savedPass = password;
    factoryPassword = false;
    $("setupNewPassword").value = $("setupConfirmPassword").value = "";
  }
  const joinHint = pass ? " " + sub(__WEBUI_TEXT__("setup.saved_join_hint"), { x: apSsid }) : "";
  banner =
    (!first
      ? pass
        ? __WEBUI_TEXT__("setup.saved_new_password")
        : __WEBUI_TEXT__("setup.saved_choice")
      : !compat
        ? __WEBUI_TEXT__("setup.saved_part1_on")
        : pass
          ? __WEBUI_TEXT__("setup.saved_part1_password")
          : __WEBUI_TEXT__("setup.saved_part1_choices")) + joinHint;
  editing = "";
  afterPart1();
}

// On to Wi-Fi (reusing the last scan when there is one), or back to the
// summary once Wi-Fi is settled.
function afterPart1() {
  if (wifiState && editing !== "wifi") return showSummary();
  clearError();
  showForm();
}

function updateConnectingCountdown() {
  const el = $("setupConnectingCountdown");
  const remaining = Math.ceil((waitStartedAt + ATTEMPT_WINDOW_MS - Date.now()) / 1000);
  el.hidden = false;
  el.textContent =
    remaining > 0
      ? __WEBUI_TEXT__("setup.connecting_deadline").replace("{n}", remaining)
      : __WEBUI_TEXT__("setup.connecting_slow");
}

function showConnecting(ssid) {
  waitStartedAt = Date.now();
  sawAttempt = false;
  $("setupConnectingText").textContent = __WEBUI_TEXT__("setup.connecting_to").replace(
    "{x}",
    () => ssid,
  );
  const where = $("setupConnectingWhere");
  where.hidden = !deviceHost;
  if (deviceHost)
    where.textContent = __WEBUI_TEXT__("setup.connecting_where").replace(
      "{x}",
      "http://" + deviceHost + ".local",
    );
  show("connecting");
  updateConnectingCountdown();
}

// Joined but not saved: the device keeps the test only for its confirm
// window, which this note counts down.
function noteConfirmWindow(n) {
  if (n.confirmRemainingMs > 0) confirmEndsAt = Date.now() + n.confirmRemainingMs;
  updateConnectedNote();
}

function updateConnectedNote() {
  $("setupConnectedNote").textContent = sub(
    wifiState === "joined"
      ? __WEBUI_TEXT__("setup.connected_keep")
      : __WEBUI_TEXT__("setup.connected_forget"),
    { t: minutes(confirmEndsAt - Date.now()), x: pendingSsid, y: wifiSsid },
  );
}

function showConnected(n) {
  pendingSsid = n.ssid || pendingSsid;
  setFieldError("setupConnectedError", "");
  noteConfirmWindow(n);
  show("connected");
  kick();
}

function payloadFromForm() {
  const network = selectedNetwork(),
    manual = $("setupHiddenToggle").checked;
  const ssid = manual ? $("setupManualSsid").value.trim() : network ? network.value : "";
  const open = manual ? false : network !== null && network.dataset.open === "true";
  return {
    action: "save",
    ssid,
    password: open ? "" : $("setupPassword").value,
    open,
    wifiSleep: savedWifiSleep,
    ipMode: "dhcp",
    apply: "live",
  };
}

// Connect only tests the network: the device joins it and keeps it pending
// until Save Wi-Fi.
async function connect() {
  const payload = payloadFromForm();
  const manual = $("setupHiddenToggle").checked;
  if (!payload.ssid) {
    setFieldError(
      "setupFieldError",
      manual
        ? __WEBUI_TEXT__("setup.network_name_required")
        : __WEBUI_TEXT__("setup.choose_network"),
    );
    return;
  }
  if (!payload.open && payload.password.length < 8) {
    setFieldError("setupFieldError", __WEBUI_TEXT__("setup.password_required"));
    return;
  }
  setFieldError("setupFieldError", "");
  clearError();
  pendingSsid = payload.ssid;
  showConnecting(payload.ssid);
  try {
    await R.api("/api/v1/network", { method: "POST", body: R.body(payload) });
  } catch (e) {
    if (e && e.code === "ADMIN_LOCKED") return show("locked");
    // The factory password came back (a second window, or a reset): part one first.
    if (e && e.code === "DEVICE_PASSWORD_REQUIRED") {
      factoryPassword = true;
      return renderWelcome();
    }
    show("form");
    setFieldError("setupFieldError", R.formatCommandError(__WEBUI_TEXT__("setup.save_failed"), e));
    return;
  }
  kick();
}

// Save Wi-Fi: the confirm action persists the joined network; the device
// then publishes the confirmed phase.
async function saveWifi() {
  if (saving) return;
  saving = true;
  showSaving(2, __WEBUI_TEXT__("setup.saving"));
  let error = null;
  try {
    await R.api("/api/v1/network", { method: "POST", body: R.body({ action: "confirm" }) });
    for (let i = 0; i < 40 && lastPhase === "joined"; i++) {
      await R.sleep(250);
      await pollStatus();
    }
  } catch (e) {
    error = e;
  }
  saving = false;
  if (error?.code === "ADMIN_LOCKED") return show("locked");
  if (lastPhase === "confirmed") return wifiSaved(lastNet);
  if (lastPhase !== "joined")
    return showError(
      __WEBUI_TEXT__("setup.error_not_saved"),
      true,
      __WEBUI_TEXT__("setup.title_not_saved"),
    );
  showConnected(lastNet);
  setFieldError(
    "setupConnectedError",
    error
      ? R.formatCommandError(__WEBUI_TEXT__("setup.save_wifi_failed"), error)
      : __WEBUI_TEXT__("setup.wifi_not_saved"),
  );
}

async function copyAddress(text, button) {
  let copied = false;
  try {
    await navigator.clipboard.writeText(text.textContent);
    copied = true;
  } catch (_) {
    // Plain http (the setup network) has no async clipboard.
    getSelection().selectAllChildren(text);
    try {
      copied = document.execCommand("copy");
    } catch (_) {
      /* the address stays selected for a manual copy */
    }
  }
  button.textContent = copied ? __WEBUI_TEXT__("setup.copied") : __WEBUI_TEXT__("setup.copy");
}

function wifiSaved(n) {
  const name = n.mdnsHost || n.deviceName || "",
    links = $("setupSuccessLinks");
  wifiState = "joined";
  wifiSsid = n.ssid || pendingSsid;
  editing = "";
  closingAt = n.apClosingMs > 0 ? Date.now() + n.apClosingMs : 0;
  $("setupNextStep").textContent = __WEBUI_TEXT__("setup.next_step").replace("{x}", () => wifiSsid);
  links.replaceChildren();
  for (const url of [name && "http://" + name + ".local", n.staIp && "http://" + n.staIp]) {
    if (!url) continue;
    const row = document.createElement("div"),
      text = document.createElement("span"),
      copy = document.createElement("button");
    row.className = "setupAddress";
    text.textContent = url;
    copy.type = "button";
    copy.className = "textLock";
    copy.textContent = __WEBUI_TEXT__("setup.copy");
    copy.onclick = () => copyAddress(text, copy);
    row.append(text, copy);
    links.appendChild(row);
  }
  banner = sub(__WEBUI_TEXT__("setup.saved_wifi"), { x: wifiSsid });
  showSummary();
}

function updateCloseHint() {
  const left = closingAt - Date.now();
  $("setupCloseHint").textContent =
    closingAt > 0 && left <= 0
      ? __WEBUI_TEXT__("setup.close_now")
      : (closingAt > 0 ? sub(__WEBUI_TEXT__("setup.close_in"), { t: minutes(left) }) + " " : "") +
        closeLater;
}

// Part three: everything listed here is already saved; Change edits one item
// and saves it again, and Finish only ends the session early.
function showSummary() {
  const joined = wifiState === "joined",
    weight = compat && lastBrew === "weight",
    finish = !compat
      ? __WEBUI_TEXT__("setup.done")
      : weight
        ? __WEBUI_TEXT__("setup.finish_restart")
        : __WEBUI_TEXT__("setup.finish");
  $("setupDoneLead").textContent = !compat
    ? __WEBUI_TEXT__("setup.lead_on")
    : weight
      ? __WEBUI_TEXT__("setup.lead_weight")
      : __WEBUI_TEXT__("setup.lead_usual");
  $("setupSumModeRow").hidden = !compat;
  $("setupSumMode").textContent = weight
    ? __WEBUI_TEXT__("setup.brewing_weight")
    : __WEBUI_TEXT__("setup.brewing_usual");
  $("setupSumRecipeRow").hidden = !weight;
  $("setupSumRecipe").textContent = recipeName(savedPreset);
  $("setupSumPass").textContent = __WEBUI_TEXT__("setup.saved");
  $("setupRevealPass").hidden = !savedPass;
  $("setupRevealPass").textContent = __WEBUI_TEXT__("setup.show");
  $("setupRevealPass").setAttribute("aria-pressed", "false");
  $("setupSumWifiRow").dataset.saved = String(joined);
  $("setupSumWifi").textContent = joined ? wifiSsid : __WEBUI_TEXT__("setup.wifi_none");
  $("setupChangeWifi").textContent = joined
    ? __WEBUI_TEXT__("setup.change")
    : __WEBUI_TEXT__("setup.set_up");
  for (const id of ["setupNextStep", "setupSuccessLinks"]) $(id).hidden = !joined;
  const later = !compat
    ? ""
    : weight
      ? joined
        ? ""
        : __WEBUI_TEXT__("setup.note_weight_no_wifi")
      : joined
        ? __WEBUI_TEXT__("setup.note_usual_wifi")
        : __WEBUI_TEXT__("setup.note_usual_no_wifi").replace("{x}", () => apSsid);
  $("setupLaterNote").textContent = later;
  $("setupLaterNote").hidden = !later;
  $("setupPassNote").textContent = sub(__WEBUI_TEXT__("setup.pass_note"), { x: apSsid });
  closeLater = sub(
    weight ? __WEBUI_TEXT__("setup.close_later_restart") : __WEBUI_TEXT__("setup.close_later"),
    { x: finish },
  );
  updateCloseHint();
  $("setupFinish").textContent = finish;
  $("setupFinish").disabled = false;
  show("success");
  kick();
}

// Finish: end the setup network early. The device arms probe success first,
// then drops the setup network a few seconds later, so navigating to the
// probe closes the captive window cleanly. When the device refuses (the
// network is already closing), stay here instead of bouncing back into a
// fresh setup. Firmware on without Wi-Fi, "skip" already armed the exit.
async function finishSetup() {
  $("setupFinish").disabled = true;
  let finished = !compat && wifiState !== "joined";
  if (!finished)
    try {
      const d = await R.api("/api/v1/network", {
        method: "POST",
        body: R.body({ action: "finish" }),
      });
      finished = d.finished === true;
    } catch (e) {
      /* the setup network still closes on its own */
    }
  if (finished) window.location.href = PROBE_URL;
  else $("setupCloseHint").textContent = __WEBUI_TEXT__("setup.close_now");
}

// "Keep" or "Continue without Wi-Fi": drop an unsaved test now, so the last
// saved network (or none) is back before the summary; should this fail, the
// test's own window still drops it.
async function discardTest() {
  const testing = () => ["connecting", "failed", "joined"].includes(lastPhase);
  await pollStatus();
  if (!testing()) return;
  try {
    await R.api("/api/v1/network", { method: "POST", body: R.body({ action: "discard" }) });
  } catch (_) {
    return;
  }
  for (let i = 0; i < 12 && testing(); i++) {
    await R.sleep(250);
    await pollStatus();
  }
}

let skipping = false;
async function skipSetup() {
  if (skipping) return;
  skipping = true;
  setFieldError("setupFieldError", "");
  try {
    await discardTest();
    await R.api("/api/v1/network", { method: "POST", body: R.body({ action: "skip" }) });
  } catch (e) {
    if (e && e.code === "ADMIN_LOCKED") return show("locked");
    setFieldError("setupFieldError", R.formatCommandError(__WEBUI_TEXT__("setup.skip_failed"), e));
    return;
  } finally {
    skipping = false;
  }
  wifiState = "skipped";
  showSummary();
}

// One live poll chain at a time: stop() bumps the token so a pending tick
// cannot re-arm, and start() relaunches the chain for the current state.
function arm(ms) {
  clearTimeout(pollTimer);
  const token = pollToken;
  pollTimer = setTimeout(() => {
    pollTimer = 0;
    if (token === pollToken) tick();
  }, ms);
}

function kick() {
  arm(0);
}

async function tick() {
  if (busyToken === pollToken || !ticking()) return;
  const token = (busyToken = pollToken);
  try {
    if (state === "scanning" && scanStartedAt) await pollScan();
    if (state === "connecting") updateConnectingCountdown();
    if (state === "connected") updateConnectedNote();
    if (state === "success") updateCloseHint();
    const statusEvery = state === "connecting" ? POLL_MS : IDLE_POLL_MS;
    if (token === pollToken && polling() && Date.now() - statusAt >= statusEvery - 100)
      await pollStatus();
    if (state === "connecting" && Date.now() - waitStartedAt >= CONNECT_WAIT_MS)
      showError(__WEBUI_TEXT__("setup.error_could_not_connect"), true);
  } finally {
    if (busyToken === token) busyToken = -1;
  }
  if (token === pollToken && ticking())
    arm(state === "scanning" ? SCAN_POLL_MS : state === "form" ? IDLE_POLL_MS : POLL_MS);
}

// First paint: rebuild the step from the device instead of starting over.
async function resume() {
  showSaving(0, __WEBUI_TEXT__("setup.loading"));
  await pollStatus();
  if (state === "locked") return;
  const n = lastNet;
  if (lastPhase === "joined") return showConnected(n);
  if (lastPhase === "confirmed" || (n.apClosingMs > 0 && n.staState === "CONNECTED"))
    return wifiSaved(n);
  if (lastPhase === "queued" || lastPhase === "connecting") {
    showConnecting(n.ssid || "");
    return kick();
  }
  if (!part1Done()) return renderWelcome();
  if (lastPhase === "failed")
    return showError(__WEBUI_TEXT__("setup.error_could_not_connect"), true);
  await startScan();
}

// A change from the summary: its first step offers Cancel back to the summary.
function editBrew(step) {
  editing = "brew";
  $("setupModeBack").textContent =
    step === "mode" ? __WEBUI_TEXT__("setup.cancel") : __WEBUI_TEXT__("setup.back");
  show(step);
}

function togglePassword(button, inputs) {
  const show = inputs[0].type === "password";
  for (const input of inputs) input.type = show ? "text" : "password";
  button.textContent = show ? __WEBUI_TEXT__("setup.hide") : __WEBUI_TEXT__("setup.show");
  button.setAttribute("aria-pressed", String(show));
}

function bind() {
  $("setupStart").onclick = () => {
    const first = part1Steps()[0];
    if (!first) return afterPart1();
    first === "protect" ? showProtect() : show(first);
  };
  $("setupModeNext").onclick = () => (byWeight() ? show("recipe") : showProtect());
  // Picking "by weight" adds the recipe step to the count.
  $("setupMode").onchange = renderProgress;
  $("setupModeBack").onclick = () => {
    if (!editing) return renderWelcome();
    editing = "";
    resetChoices();
    showSummary();
  };
  $("setupRecipeNext").onclick = showProtect;
  $("setupRecipeBack").onclick = () => show("mode");
  $("setupProtectBack").onclick = () =>
    editing === "password"
      ? ((editing = ""), showSummary())
      : compat
        ? show(byWeight() ? "recipe" : "mode")
        : renderWelcome();
  $("setupProtect").onsubmit = (e) => {
    e.preventDefault();
    savePart1();
  };
  $("setupProtect").oninput = () => validatePassword(false);
  $("setupConfirmPassword").onblur = () => validatePassword(true);
  $("setupNewPassShow").onclick = () =>
    togglePassword($("setupNewPassShow"), [$("setupNewPassword"), $("setupConfirmPassword")]);
  $("setupSearchAgain").onclick = () => {
    setFieldError("setupFieldError", "");
    startScan();
  };
  $("setupSkip").onclick = skipSetup;
  $("setupFormBack").onclick = async () => {
    $("setupFormBack").disabled = true;
    await discardTest();
    $("setupFormBack").disabled = false;
    editing = "";
    showSummary();
  };
  $("setupSaveWifi").onclick = saveWifi;
  // The tested choice stays unsaved; the next Connect replaces it.
  $("setupOther").onclick = () => {
    clearError();
    showForm();
  };
  $("setupFinish").onclick = finishSetup;
  $("setupChangeMode").onclick = () => editBrew("mode");
  $("setupChangeRecipe").onclick = () => editBrew("recipe");
  $("setupChangePass").onclick = () => {
    editing = "password";
    showProtect();
  };
  $("setupChangeWifi").onclick = () => {
    editing = "wifi";
    banner = "";
    afterPart1();
  };
  $("setupRevealPass").onclick = () => {
    const show = $("setupRevealPass").getAttribute("aria-pressed") !== "true";
    $("setupSumPass").textContent = show ? savedPass : __WEBUI_TEXT__("setup.saved");
    $("setupRevealPass").textContent = show
      ? __WEBUI_TEXT__("setup.hide")
      : __WEBUI_TEXT__("setup.show");
    $("setupRevealPass").setAttribute("aria-pressed", String(show));
  };
  $("setupHiddenToggle").onchange = () => {
    const manual = $("setupHiddenToggle").checked;
    $("setupManualSsid").hidden = !manual;
    $("setupManualLabel").hidden = !manual;
    $("setupNetwork").disabled = manual;
    syncPasswordVisibility();
  };
  $("setupNetwork").onchange = syncPasswordVisibility;
  $("setupPasswordShow").onclick = () =>
    togglePassword($("setupPasswordShow"), [$("setupPassword")]);
  $("setupForm").onsubmit = (e) => {
    e.preventDefault();
    connect();
  };
}

export function stop() {
  running = false;
  clearTimeout(pollTimer);
  pollTimer = 0;
  pollToken++;
}

export function start() {
  // Also resumes after the tab was hidden or another view was open: the
  // chain is paused by stop(), never abandoned.
  running = true;
  if (state === "idle") return resume().then(() => true);
  tick();
  return true;
}

function chainWatchdog() {
  if (running && bound && !document.hidden && pollTimer === 0 && busyToken !== pollToken) tick();
}

export function init() {
  if (bound) return;
  bound = true;
  setInterval(chainWatchdog, 2500);
  window.addEventListener("pageshow", () => {
    if (!document.hidden) start();
  });
  bind();
  if (!document.hidden && R.webUiPollingActive()) start();
}
