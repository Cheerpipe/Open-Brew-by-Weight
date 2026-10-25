"use strict";
// First-use Wi-Fi onboarding (/setup): menu-less card that scans, saves with
// live apply, and follows the device's onboarding phase. Reached through the
// SoftAP captive portal, where the WPA2 passphrase is the device password, so
// the admin unlock is bypassed per request by the firmware (peer on the
// SoftAP). Every step is rebuilt from firmware status, so a reload or a second
// window lands on the step the device is in; the device confirms a joined
// network by itself.
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
// Wildcard DNS resolves the OS probe hosts to the device itself, so pointing
// the browser at one triggers the portal verdict: after "skip"/"finish" the
// device answers success and the captive window closes as "logged in".
const PROBE_URL = /Android/i.test(navigator.userAgent)
  ? "http://connectivitycheck.gstatic.com/generate_204"
  : "http://captive.apple.com/hotspot-detect.html";
const STATES = ["mode", "recipe", "scanning", "form", "connecting", "success", "skipped", "locked"];
const HEADINGS = {
  mode: [__WEBUI_TEXT__("setup.welcome_title"), __WEBUI_TEXT__("setup.welcome_intro")],
  recipe: [__WEBUI_TEXT__("setup.recipe_title"), __WEBUI_TEXT__("setup.recipe_intro")],
  form: [__WEBUI_TEXT__("setup.title"), __WEBUI_TEXT__("setup.intro")],
  error: [__WEBUI_TEXT__("setup.title_error"), ""],
  connecting: [__WEBUI_TEXT__("setup.title_connecting"), ""],
  success: [__WEBUI_TEXT__("setup.title_success"), ""],
  skipped: [__WEBUI_TEXT__("setup.title_skipped"), ""],
  locked: [__WEBUI_TEXT__("setup.title_locked"), ""],
};
const BLOCKED = {
  activator_on: __WEBUI_TEXT__("setup.blocked_paddle"),
  active_shot: __WEBUI_TEXT__("setup.blocked_brewing"),
  machine_running: __WEBUI_TEXT__("setup.blocked_brewing"),
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
  blocked = "",
  rescanWhenFree = false,
  lastNetworks = null,
  lastPhase = "",
  scanStartedAt = 0,
  statusAt = 0,
  waitStartedAt = 0,
  sawAttempt = false,
  savedWifiSleep = true,
  deviceHost = "",
  // Firmware off: onboarding also asks how to brew (draft kept by the device).
  compat = false,
  brewRestored = false,
  lastBrew = "none",
  apSsid = "",
  wifiDone = "",
  // Factory (public) device password: setup asks for the owner's own.
  factoryPassword = false,
  configMutable = true;

const formLike = (name) => name === "form" || name === "scanning";
const polling = () => formLike(state) || state === "connecting";
const byWeight = () => document.querySelector("input[name=setupMode]:checked").value === "weight";
const recipeInput = () => document.querySelector("input[name=setupRecipe]:checked");
const brewPayload = () =>
  !compat
    ? {}
    : byWeight()
      ? { brewByWeight: true, presetId: Number(recipeInput().value) }
      : { brewByWeight: false };

function show(name) {
  state = name;
  for (const key of STATES) $("setup" + key[0].toUpperCase() + key.slice(1)).hidden = key !== name;
  let [title, intro] = HEADINGS[formLike(name) ? (connectError ? "error" : "form") : name];
  if (compat && name === "success") title = __WEBUI_TEXT__("setup.title_all_set");
  if (compat && formLike(name) && !connectError)
    intro = byWeight()
      ? __WEBUI_TEXT__("setup.wifi_intro_weight")
      : __WEBUI_TEXT__("setup.wifi_intro_usual");
  $("setupFormBack").hidden = !compat;
  $("setupTitle").textContent = title;
  $("setupIntro").textContent = intro;
  $("setupIntro").hidden = !intro;
  $("setupError").hidden = !$("setupErrorText").textContent || !formLike(name);
  $("setupBlocked").hidden = !blocked || !formLike(name);
  // A step change that hid the focused control hands focus to the heading.
  if (document.activeElement?.closest("[hidden]")) $("setupTitle").focus({ preventScroll: true });
}

// One visible verdict above the form: focused and announced, so it is seen
// on a small captive sheet; Connect is the single retry path.
function showError(text, connect) {
  connectError = connect;
  $("setupErrorText").textContent = text;
  $("setupErrorHint").hidden = !connect;
  show("form");
  const box = $("setupError");
  box.scrollIntoView({ block: "start" });
  box.focus({ preventScroll: true });
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

function setStatusError(text) {
  const el = $("setupFieldError");
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
  renderPicker(lastNetworks);
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
    return; /* transient poll failure: keep waiting within the bounded window */
  }
  statusAt = Date.now();
  applyStatus(s);
}

// The device's onboarding phase decides the step: a joined/confirmed network
// (or a recovery network that came back while this phone is still on the
// setup network) is success, a running attempt is "connecting", and a
// finished attempt is a verdict only for the attempt this page is waiting on.
function applyStatus(s) {
  if (s.adminUnlocked === false) return show("locked");
  factoryPassword = s.factoryPassword === true;
  configMutable = s.configMutable !== false;
  $("setupDeviceWrap").hidden = !factoryPassword;
  const n = s.network || {};
  compat = s.compatibilityMode === true;
  lastBrew = n.setupBrew || "none";
  apSsid = n.apSsid || "";
  if (compat && !brewRestored) restoreBrew(n);
  if (typeof n.wifiSleep === "boolean") savedWifiSleep = n.wifiSleep;
  deviceHost = n.mdnsHost || n.deviceName || "";
  lastPhase = n.setup || "";
  setBlocked(s.configMutable === false ? s.configLockReason : "");
  if (!polling()) return;
  if (
    lastPhase === "joined" ||
    lastPhase === "confirmed" ||
    (n.apClosingMs > 0 && n.staState === "CONNECTED")
  )
    return showSuccess(n);
  if (lastPhase === "queued" || lastPhase === "connecting") {
    if (state !== "connecting") showConnecting(n.ssid || "");
    sawAttempt = true;
    return;
  }
  if (state !== "connecting") return;
  const waited = Date.now() - waitStartedAt;
  if (sawAttempt || waited >= VERDICT_GRACE_MS)
    showError(
      n.wifiConfigured === false
        ? __WEBUI_TEXT__("setup.error_reverted")
        : __WEBUI_TEXT__("setup.error_could_not_connect"),
      true,
    );
}

// A reload or second phone picks up the choice already sent to the device.
function restoreBrew(n) {
  brewRestored = true;
  if (lastBrew !== "usual" && lastBrew !== "weight") return;
  document.querySelector(`input[name=setupMode][value=${lastBrew}]`).checked = true;
  const recipe = document.querySelector(`input[name=setupRecipe][value="${n.setupPresetId}"]`);
  if (recipe) recipe.checked = true;
}

// After the brewing steps: back to the summary once Wi-Fi is settled, else on
// to the Wi-Fi step (reusing the last scan when there is one).
function afterBrewChoice() {
  if (wifiDone) return showSummary();
  if (!lastNetworks) return startScan();
  renderPicker(lastNetworks);
  show("form");
  kick();
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
    ...brewPayload(),
  };
}

// The factory password is public, so setup ends only after the owner picks
// their own. The setup network keeps running; the new password protects
// Admin and updates now, and the setup network from its next start.
async function protectDevice() {
  if (!factoryPassword) return true;
  const password = $("setupDevicePassword").value;
  if (password.length < 8) {
    setStatusError(__WEBUI_TEXT__("setup.device_password_required"));
    return false;
  }
  try {
    await R.api("/api/v1/device/password", {
      method: "POST",
      body: R.body({ newPassword: password }),
    });
    for (let i = 0; i < 40 && (factoryPassword || !configMutable); i++) {
      await new Promise((resolve) => setTimeout(resolve, 250));
      await pollStatus();
    }
  } catch (e) {
    setStatusError(R.formatCommandError(__WEBUI_TEXT__("setup.device_password_failed"), e));
    return false;
  }
  if (!factoryPassword) return true;
  setStatusError(__WEBUI_TEXT__("setup.device_password_failed"));
  return false;
}

async function connect() {
  const payload = payloadFromForm();
  const manual = $("setupHiddenToggle").checked;
  if (!payload.ssid) {
    setStatusError(
      manual
        ? __WEBUI_TEXT__("setup.network_name_required")
        : __WEBUI_TEXT__("setup.choose_network"),
    );
    return;
  }
  if (!payload.open && payload.password.length < 8) {
    setStatusError(__WEBUI_TEXT__("setup.password_required"));
    return;
  }
  setStatusError("");
  clearError();
  if (!(await protectDevice())) return;
  showConnecting(payload.ssid);
  try {
    await R.api("/api/v1/network", { method: "POST", body: R.body(payload) });
  } catch (e) {
    if (e && e.code === "ADMIN_LOCKED") return show("locked");
    show("form");
    setStatusError(R.formatCommandError(__WEBUI_TEXT__("setup.save_failed"), e));
    return;
  }
  kick();
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

function showSuccess(n) {
  const ssid = n.ssid || "",
    name = n.mdnsHost || n.deviceName || "",
    links = $("setupSuccessLinks");
  $("setupSuccessText").textContent = __WEBUI_TEXT__("setup.success_text").replace(
    "{x}",
    () => ssid,
  );
  $("setupSuccessText").dataset.ssid = ssid;
  $("setupNextStep").textContent = __WEBUI_TEXT__("setup.next_step").replace("{x}", () => ssid);
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
  wifiDone = "joined";
  showSummary();
}

// Firmware off: success and "Continue without Wi-Fi" end on one summary with
// Change links; Finish sends the final choice.
function showSummary() {
  const joined = wifiDone === "joined",
    weight = compat && byWeight();
  $("setupSummary").hidden = !compat;
  $("setupSumMode").textContent = weight
    ? __WEBUI_TEXT__("setup.brewing_weight")
    : __WEBUI_TEXT__("setup.brewing_usual");
  $("setupSumRecipeRow").hidden = !weight;
  $("setupSumRecipe").textContent = recipeInput().closest("label").querySelector("b").textContent;
  $("setupSumWifi").textContent = joined
    ? $("setupSuccessText").dataset.ssid
    : __WEBUI_TEXT__("setup.wifi_none");
  $("setupRestartNote").hidden = !weight;
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
  for (const id of ["setupSuccessText", "setupNextStep", "setupSuccessLinks", "setupCloseHint"])
    $(id).hidden = !joined;
  $("setupFinish").textContent = !compat
    ? __WEBUI_TEXT__("setup.done")
    : weight
      ? __WEBUI_TEXT__("setup.finish_restart")
      : __WEBUI_TEXT__("setup.finish");
  show("success");
}

// "Done" on the success screen: end the setup network early. The device
// arms probe success first, then drops the setup network a few seconds later,
// so navigating to the probe closes the captive window cleanly. When the
// device refuses (the network is already closing), stay here instead of
// bouncing back into a fresh setup.
async function finishSetup() {
  $("setupFinish").disabled = true;
  let finished = false;
  try {
    const d = await R.api("/api/v1/network", {
      method: "POST",
      body: R.body({ action: "finish", ...brewPayload() }),
    });
    finished = d.finished === true;
  } catch (e) {
    /* the setup network still closes on its own */
  }
  if (finished) window.location.href = PROBE_URL;
  else $("setupCloseHint").textContent = __WEBUI_TEXT__("setup.close_now");
}

let skipping = false;
async function skipSetup() {
  if (skipping) return;
  skipping = true;
  setStatusError("");
  try {
    if (!(await protectDevice())) return;
    await R.api("/api/v1/network", {
      method: "POST",
      body: R.body({ action: "skip", ...brewPayload() }),
    });
  } catch (e) {
    if (e && e.code === "ADMIN_LOCKED") return show("locked");
    setStatusError(R.formatCommandError(__WEBUI_TEXT__("setup.skip_failed"), e));
    return;
  } finally {
    skipping = false;
  }
  $("setupSkipDone").href = PROBE_URL;
  if (!compat) return show("skipped");
  wifiDone = "skipped";
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
  if (busyToken === pollToken || !polling()) return;
  const token = (busyToken = pollToken);
  try {
    if (state === "scanning" && scanStartedAt) await pollScan();
    if (state === "connecting") updateConnectingCountdown();
    const statusEvery = state === "connecting" ? POLL_MS : IDLE_POLL_MS;
    if (token === pollToken && polling() && Date.now() - statusAt >= statusEvery - 100)
      await pollStatus();
    if (state === "connecting" && Date.now() - waitStartedAt >= CONNECT_WAIT_MS)
      showError(__WEBUI_TEXT__("setup.error_could_not_connect"), true);
  } finally {
    if (busyToken === token) busyToken = -1;
  }
  if (token === pollToken && polling())
    arm(state === "scanning" ? SCAN_POLL_MS : state === "connecting" ? POLL_MS : IDLE_POLL_MS);
}

// First paint: rebuild the step from the device instead of starting over.
async function resume() {
  show("scanning");
  await pollStatus();
  if (state !== "scanning") return;
  if (compat && lastBrew === "none" && lastPhase === "none") return show("mode");
  if (lastPhase === "failed") showError(__WEBUI_TEXT__("setup.error_could_not_connect"), true);
  await startScan(lastPhase === "failed");
}

function bind() {
  $("setupSearchAgain").onclick = () => {
    setStatusError("");
    startScan();
  };
  $("setupSkip").onclick = skipSetup;
  $("setupFinish").onclick = finishSetup;
  $("setupModeNext").onclick = () => (byWeight() ? show("recipe") : afterBrewChoice());
  $("setupRecipeNext").onclick = afterBrewChoice;
  $("setupRecipeBack").onclick = () => show("mode");
  $("setupFormBack").onclick = () => show(byWeight() ? "recipe" : "mode");
  $("setupChangeMode").onclick = () => show("mode");
  $("setupChangeRecipe").onclick = () => show("recipe");
  $("setupHiddenToggle").onchange = () => {
    const manual = $("setupHiddenToggle").checked;
    $("setupManualSsid").hidden = !manual;
    $("setupManualLabel").hidden = !manual;
    $("setupNetwork").disabled = manual;
    syncPasswordVisibility();
  };
  $("setupNetwork").onchange = syncPasswordVisibility;
  const passToggle = $("setupPasswordShow");
  const passShowText = __WEBUI_TEXT__("setup.show");
  const passHideText = __WEBUI_TEXT__("setup.hide");
  passToggle.onclick = () => {
    const input = $("setupPassword");
    const show = input.type === "password";
    input.type = show ? "text" : "password";
    passToggle.textContent = show ? passHideText : passShowText;
    passToggle.setAttribute("aria-pressed", String(show));
  };
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
