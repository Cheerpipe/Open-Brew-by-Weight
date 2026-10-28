"use strict";
import * as R from "./runtime.js?v=__FW_ASSET_TAG__";
const $ = R.$;
// Save/Revert pairs: a dirty section enables both buttons and shows its hint.
const REVERT_BUTTONS = {
  saveNetworkButton: "revertNetworkButton",
  saveDateTimeButton: "revertDateTimeButton",
  saveWebhookButton: "revertWebhookButton",
  changeDevicePasswordButton: "revertDevicePasswordButton",
  saveFirmwareModeButton: "revertFirmwareModeButton",
  saveBleButton: "revertBleButton",
  savePowerButton: "revertPowerButton",
  saveFrontendButton: "revertFrontendButton",
  saveCompanionButton: "revertCompanionButton",
};
function setSaveDirty(id, hintId, dirty) {
  const b = $(id);
  if (b) {
    b.dataset.dirty = +dirty;
    b.disabled = !R.controlsMutable || !dirty;
  }
  const r = REVERT_BUTTONS[id] && $(REVERT_BUTTONS[id]);
  if (r) {
    r.dataset.dirty = +dirty;
    r.disabled = !R.controlsMutable || !dirty;
  }
  if (hintId) $(hintId)?.classList.toggle("hidden", !dirty);
}
// Sections saved whole carry the revision their form was filled from: a save
// over a change made meanwhile (another screen through the companion) is
// refused, and the form shows the saved values again.
const formRevisions = {};
function formStale(e, clean) {
  if (e?.status !== 409 || !/REVISION/.test(e.code || "")) return false;
  clean();
  R.message(__WEBUI_TEXT__("admin.changed_elsewhere"), "error");
  return true;
}
function timeZoneSelectionChanged() {
  markDateTimeDirty();
  R.refreshTimezonePreview();
}
async function changeTimezoneMode() {
  markDateTimeDirty();
  R.updateTimezoneControls();
  if (!$("timezoneAutomatic").checked) return;
  const select = $("timezoneId");
  try {
    await R.populateTimezoneOptions();
  } catch (_) {}
  if (!$("timezoneAutomatic").checked) return;
  const id = R.browserTimezone();
  select.value = R.timezoneCatalog?.includes(id) ? id : "";
  if (!select.value) R.message(__WEBUI_TEXT__("runtime.timezone_detection_unavailable"), "error");
  R.refreshTimezonePreview();
}
function lockAdmin() {
  if (R.developmentMode) return;
  clearTimeout(R.uiState.scanTimer);
  R.uiState.scanTimer = 0;
  R.api("/api/v1/admin/lock", { method: "POST", body: R.body({}) })
    .then(() => {
      R.lockAdminUi();
      R.noteReachOk();
      R.message(__WEBUI_TEXT__("runtime.administration_locked"), "ok");
      return R.refreshStatus();
    })
    .catch((e) =>
      R.message(
        R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_lock_administration"), e),
        "error",
      ),
    );
}
// Each Admin section keeps a dirty flag, a baseline, and a save button and
// hint, all named after the section (ble -> bleDirty, saveBleButton, ...).
function sectionDirty(name, on) {
  R.uiState[name + "Dirty"] = on;
  setSaveDirty("save" + name[0].toUpperCase() + name.slice(1) + "Button", name + "DirtyHint", on);
}
function markDateTimeDirty() {
  sectionDirty("dateTime", true);
}
function revertSection(name, after) {
  if (!R.uiState[name + "Dirty"]) return;
  if (!confirm(__WEBUI_TEXT__("admin.discard_unsaved_changes"))) return;
  R.restoreSnapshot(R.uiState[name + "Baseline"]);
  after?.();
  sectionDirty(name, false);
  R.clearFieldErrors();
  R.refreshStatus();
}
function revertDateTimeConfig() {
  if (!R.uiState.dateTimeDirty) return;
  if (!confirm(__WEBUI_TEXT__("runtime.discard_unsaved_changes"))) return;
  R.restoreSnapshot(R.uiState.dateTimeBaseline);
  R.uiState.dateTimeBaseline = null;
  R.uiState.dateTimeDirty = false;
  setSaveDirty("saveDateTimeButton", "dateTimeDirtyHint", false);
  R.clearFieldErrors();
  R.refreshTimezonePreview();
  R.updateTimezoneControls();
  R.refreshStatus();
}
function networkControls() {
  return [...document.querySelectorAll("#networkPanel input,#networkPanel select")].filter(
    (el) => el.id && el.id !== "staNetwork",
  );
}
function revertNetworkConfig() {
  if ($("saveNetworkButton").dataset.dirty !== "1") return;
  if (!confirm(__WEBUI_TEXT__("runtime.discard_unsaved_changes"))) return;
  R.restoreSnapshot(R.uiState.networkBaseline);
  setSaveDirty("saveNetworkButton", "", false);
  R.clearFieldErrors();
  R.updateNetworkPasswordState();
  R.updateStaticIpFieldsState();
  R.updateWifiSleepState();
  R.resetNetworkAddressLoaded();
  R.refreshStatus();
}
function validNtpHostnameClient(host) {
  const t = String(host || "");
  if (!t || t.length > 63) return false;
  if (t[0] === "-" || t[0] === "." || t[t.length - 1] === "-" || t[t.length - 1] === ".")
    return false;
  return /^[A-Za-z0-9.-]+$/.test(t);
}
function validDeviceNameClient(name) {
  const t = String(name || "").trim();
  if (!t || t.length > 32) return false;
  if (t[0] === "-" || t[t.length - 1] === "-") return false;
  return /^[A-Za-z0-9 -]+$/.test(t);
}
function validateDateTimeClient() {
  const tz = $("timezoneId").value;
  if (
    (tz && !R.timezoneCatalog?.includes(tz)) ||
    (!tz && (R.uiState.savedTimezoneId || $("timezoneAutomatic").checked))
  )
    return { id: "timezoneId", msg: __WEBUI_TEXT__("runtime.select_timezone") };
  const preset = $("ntpServerPreset").value;
  if (!["pool", "google", "cloudflare", "nist"].includes(preset))
    return { id: "ntpServerPreset", msg: __WEBUI_TEXT__("runtime.select_a_valid_ntp_server") };
  const custom = $("ntpServerCustom").value.trim();
  if (custom && !validNtpHostnameClient(custom))
    return { id: "ntpServerCustom", msg: __WEBUI_TEXT__("runtime.custom_ntp_must_be_empty_or_a") };
  return null;
}
function dateTimePayload() {
  const auto = $("timezoneAutomatic").checked,
    value = {
      timezoneAutomatic: auto,
      ntpSyncEnabled: $("ntpSyncEnabled").checked,
      ntpServerPreset: $("ntpServerPreset").value,
      ntpServerCustom: $("ntpServerCustom").value.trim(),
    };
  const zone = $("timezoneId").value;
  if (zone) {
    value.timezoneId = zone;
    if (auto) value.timezoneDetected = true;
  }
  return value;
}
function parseIpv4Client(text) {
  const m = String(text || "")
    .trim()
    .match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/);
  if (!m) return null;
  const p = m.slice(1).map(Number);
  return p.some((n) => n > 255) ? null : p;
}
function ipv4ToInt(p) {
  return ((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]) >>> 0;
}
function validNetmaskClient(p) {
  const m = ipv4ToInt(p);
  if (!m) return false;
  const i = ~m >>> 0;
  return (i & (i + 1)) === 0;
}
function validateNetworkClient() {
  const dn = $("deviceName") ? $("deviceName").value.trim() : "";
  if ($("deviceName") && !validDeviceNameClient(dn))
    return { id: "deviceName", msg: __WEBUI_TEXT__("runtime.device_name_must_be_1_32_letters") };
  const ssid = $("staSsid").value,
    open = $("staOpen").checked,
    pw = $("staPassword").value,
    keep =
      !!R.uiState.savedStaSsid &&
      ssid === R.uiState.savedStaSsid &&
      !pw &&
      open === R.uiState.savedStaOpen;
  if (!ssid || ssid.length > 32)
    return { id: "staSsid", msg: __WEBUI_TEXT__("runtime.ssid_must_be_1_32_characters") };
  if (open && pw)
    return {
      id: "staPassword",
      msg: __WEBUI_TEXT__("runtime.open_network_password_must_be_empty"),
    };
  if (!open && !keep && (pw.length < 8 || pw.length > 63))
    return { id: "staPassword", msg: __WEBUI_TEXT__("runtime.wi_fi_password_must_be_8_63") };
  if ($("staIpMode").value !== "static") return null;
  const ip = parseIpv4Client($("staStaticIp").value),
    mask = parseIpv4Client($("staNetmask").value),
    gw = parseIpv4Client($("staGateway").value),
    dns1 = parseIpv4Client($("staDns1").value),
    d2 = $("staDns2").value.trim(),
    dns2 = d2 ? parseIpv4Client(d2) : [0, 0, 0, 0];
  if (!ip) return { id: "staStaticIp", msg: __WEBUI_TEXT__("runtime.enter_a_valid_ipv4_address") };
  if (!mask || !validNetmaskClient(mask))
    return { id: "staNetmask", msg: __WEBUI_TEXT__("runtime.enter_a_valid_subnet_mask") };
  if (!gw)
    return { id: "staGateway", msg: __WEBUI_TEXT__("runtime.enter_a_valid_gateway_address") };
  if (!dns1) return { id: "staDns1", msg: __WEBUI_TEXT__("runtime.enter_a_valid_primary_dns") };
  if (d2 && !dns2)
    return { id: "staDns2", msg: __WEBUI_TEXT__("runtime.enter_a_valid_secondary_dns_or_leave") };
  if (ip[0] === 192 && ip[1] === 168 && ip[2] === 4)
    return { id: "staStaticIp", msg: __WEBUI_TEXT__("runtime.sta_must_not_use_softap_subnet_192") };
  if (gw[0] === 192 && gw[1] === 168 && gw[2] === 4)
    return {
      id: "staGateway",
      msg: __WEBUI_TEXT__("runtime.gateway_must_not_use_softap_subnet_192"),
    };
  if (ipv4ToInt(ip) === ipv4ToInt(gw))
    return { id: "staStaticIp", msg: __WEBUI_TEXT__("runtime.ip_and_gateway_must_differ") };
  const m = ipv4ToInt(mask);
  if ((ipv4ToInt(ip) & m) !== (ipv4ToInt(gw) & m))
    return { id: "staGateway", msg: __WEBUI_TEXT__("runtime.ip_and_gateway_must_share_a_subnet") };
  const h = ipv4ToInt(ip) & (~m >>> 0);
  if (!h || h === ~m >>> 0)
    return { id: "staStaticIp", msg: __WEBUI_TEXT__("runtime.ip_must_not_be_network_broadcast") };
  return null;
}
function networkSavePayload() {
  const p = {
    action: "save",
    ssid: $("staSsid").value,
    password: $("staPassword").value,
    open: $("staOpen").checked,
    wifiSleep: R.uiState.savedStaWifiSleep,
    name: $("deviceName") ? $("deviceName").value.trim() : "",
    ipMode: $("staIpMode").value,
    baseRevision: formRevisions.network,
  };
  if (p.ipMode === "static") {
    p.ip = $("staStaticIp").value.trim();
    p.netmask = $("staNetmask").value.trim();
    p.gateway = $("staGateway").value.trim();
    p.dns1 = $("staDns1").value.trim();
    const d = $("staDns2").value.trim();
    if (d) p.dns2 = d;
  }
  return p;
}
function networkPreferencesOnly(p) {
  return (
    !!R.uiState.savedStaSsid &&
    p.ssid === R.uiState.savedStaSsid &&
    !p.password &&
    p.open === R.uiState.savedStaOpen &&
    p.ipMode === R.uiState.savedStaAddr.split("|")[0] &&
    netAddrKey() === R.uiState.savedStaAddr
  );
}
function netAddrKey() {
  const m = $("staIpMode").value;
  return m !== "static"
    ? m
    : m +
        "|" +
        $("staStaticIp").value.trim() +
        "|" +
        $("staNetmask").value.trim() +
        "|" +
        $("staGateway").value.trim() +
        "|" +
        $("staDns1").value.trim() +
        "|" +
        $("staDns2").value.trim();
}
function validateDevicePasswordClient(
  newId = "newDevicePassword",
  confirmId = "confirmDevicePassword",
) {
  const n = $(newId).value,
    c = $(confirmId).value;
  if (n.length < 8 || n.length > 63)
    return {
      id: newId,
      msg: __WEBUI_TEXT__("runtime.new_device_password_must_be_8_63"),
    };
  if (n === "ineedacoffee")
    return {
      id: newId,
      msg: __WEBUI_TEXT__("runtime.new_device_password_cannot_be_the_factory"),
    };
  if (n !== c)
    return {
      id: confirmId,
      msg: __WEBUI_TEXT__("runtime.the_new_device_passwords_do_not_match"),
    };
  return null;
}
// Admin → Access. The switch only opens a confirmation sheet; the login
// changes when the device accepts the password typed there.
let accessState = {},
  loginSheetMode = "";
const DEVICE_ICONS = {
  phone: '<rect x="7" y="2.5" width="10" height="19" rx="2.2"/><path d="M11 18.5h2"/>',
  tablet: '<rect x="4.5" y="3" width="15" height="18" rx="2.2"/><path d="M11 18h2"/>',
  laptop: '<rect x="5" y="5" width="14" height="10" rx="1.5"/><path d="M3 19h18"/>',
  home: '<path d="m3 11 9-7 9 7v9H3z"/><circle cx="12" cy="14" r="2"/>',
  companion: '<use href="#g-companion"/>',
};
// Remembered sessions carry a daily stamp, so they show days; browser-only
// sessions are tracked to the second.
function deviceAgo(s, daily) {
  if (s < 0) return "";
  if (daily)
    return s < 86400
      ? " · " + __WEBUI_TEXT__("admin.ago_today")
      : s < 172800
        ? " · " + __WEBUI_TEXT__("admin.ago_yesterday")
        : " · " + Math.floor(s / 86400) + __WEBUI_TEXT__("admin.ago_days");
  return s < 90
    ? " · " + __WEBUI_TEXT__("admin.ago_just_now")
    : s < 3600
      ? " · " + Math.round(s / 60) + __WEBUI_TEXT__("admin.ago_minutes")
      : " · " + Math.round(s / 3600) + __WEBUI_TEXT__("admin.ago_hours");
}
// The row asks in place; `done` runs the sign out once confirmed.
// Tapping a device name edits it in place: Enter or leaving saves, Escape cancels.
function editDeviceName(title, name, rename) {
  const input = document.createElement("input");
  input.className = "devNameInput";
  input.maxLength = 23;
  input.value = name;
  input.setAttribute("aria-label", __WEBUI_TEXT__("admin.device_name"));
  let finished = false;
  const finish = (save) => {
    if (finished) return;
    finished = true;
    const next = input.value.trim();
    input.replaceWith(title);
    if (save && next && next !== name) rename(next);
  };
  input.onkeydown = (e) => {
    if (e.key === "Enter") finish(true);
    if (e.key === "Escape") finish(false);
  };
  input.onblur = () => finish(true);
  title.replaceWith(input);
  input.focus();
  input.select();
}
function deviceRow(icon, name, detail, self, actionLabel, askText, done, rename) {
  const li = document.createElement("li"),
    kind = rename ? "button" : "b";
  li.className = "dev";
  li.innerHTML = `<span class="devIc"><svg viewBox="0 0 24 24">${DEVICE_ICONS[icon]}</svg></span><span class="devTx"><${kind}></${kind}><small></small></span>`;
  const title = li.querySelector(kind),
    small = li.querySelector("small");
  title.textContent = name;
  small.textContent = detail;
  if (rename) {
    title.type = "button";
    title.className = "devName inlineAction";
    title.title = __WEBUI_TEXT__("admin.rename_device");
    title.onclick = () => editDeviceName(title, name, rename);
  }
  if (self) {
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.textContent = __WEBUI_TEXT__("admin.this_device");
    title.append(tag);
  }
  if (!actionLabel) return li;
  const button = document.createElement("button");
  button.type = "button";
  button.className = "textLock";
  button.textContent = actionLabel;
  button.onclick = () => {
    li.classList.add("confirm");
    small.textContent = askText;
    button.hidden = true;
    const box = document.createElement("span");
    box.className = "devAsk";
    box.innerHTML =
      '<button type="button" class="btnGlyph btnWarn"><span class="t"></span></button><button type="button" class="btnGlyph ghost"><span class="t"></span></button>';
    const [yes, no] = box.querySelectorAll("button");
    yes.firstChild.textContent = actionLabel;
    no.firstChild.textContent = __WEBUI_TEXT__("shell.cancel");
    no.onclick = () => {
      li.classList.remove("confirm");
      small.textContent = detail;
      button.hidden = false;
      box.remove();
    };
    yes.onclick = () => {
      li.classList.remove("confirm");
      done();
    };
    li.append(box);
  };
  li.append(button);
  return li;
}
// Status polls keep the list fresh, except while a row is asking.
function renderAccessDevices(a) {
  if ($("devList").querySelector(".confirm, input")) return;
  const sessions = [...(a.sessions || [])].sort((x, y) => y.self - x.self || x.id - y.id),
    rows = sessions.map((s) =>
      deviceRow(
        /iPhone|Android/.test(s.label) ? "phone" : /iPad/.test(s.label) ? "tablet" : "laptop",
        s.label || __WEBUI_TEXT__("admin.unnamed_device"),
        (s.remembered
          ? __WEBUI_TEXT__("admin.remembered")
          : __WEBUI_TEXT__("admin.until_browser_closes")) +
          (s.self ? " · " + __WEBUI_TEXT__("admin.active_now") : deviceAgo(s.agoS, s.remembered)),
        s.self,
        s.self ? "" : __WEBUI_TEXT__("shell.sign_out"),
        __WEBUI_TEXT__("admin.device_needs_password_again"),
        () =>
          accessQuickAction(
            "signOut",
            __WEBUI_TEXT__("admin.device_signed_out"),
            __WEBUI_TEXT__("admin.could_not_sign_out_device"),
            { id: s.id },
          ),
        (device) =>
          accessQuickAction(
            "rename",
            __WEBUI_TEXT__("admin.device_renamed"),
            __WEBUI_TEXT__("admin.could_not_rename_device"),
            { id: s.id, device },
          ),
      ),
    );
  const since = a.homeAssistantSinceUtcSec
    ? new Date(a.homeAssistantSinceUtcSec * 1000).toLocaleDateString(undefined, {
        day: "numeric",
        month: "short",
      })
    : "";
  rows.push(
    deviceRow(
      "home",
      __WEBUI_TEXT__("admin.home_assistant"),
      a.homeAssistantConnected
        ? since
          ? __WEBUI_TEXT__("admin.connected_since") + since
          : __WEBUI_TEXT__("admin.home_assistant_connected")
        : __WEBUI_TEXT__("admin.home_assistant_not_connected"),
      false,
      a.homeAssistantConnected ? __WEBUI_TEXT__("admin.disconnect") : "",
      __WEBUI_TEXT__("admin.home_assistant_asks_again"),
      () =>
        accessQuickAction(
          "forgetHomeAssistant",
          __WEBUI_TEXT__("admin.home_assistant_disconnected"),
          __WEBUI_TEXT__("admin.could_not_disconnect_home_assistant"),
        ),
    ),
  );
  $("devList").replaceChildren(...rows);
  R.lockControls($("devList"));
  $("devCount").textContent = "(" + sessions.length + ")";
  $("signOutOthersButton").hidden = !sessions.some((s) => !s.self);
}
function applyAccessStatus(a) {
  if (!a) return;
  accessState = a;
  const on = !!a.loginEnabled;
  if (!loginSheetMode) $("loginEnabled").checked = on;
  $("loginEnabledHint").textContent = on
    ? __WEBUI_TEXT__("admin.login_security_note")
    : __WEBUI_TEXT__("admin.login_off_hint");
  $("accessDevices").classList.toggle("hidden", !on);
  if (on) renderAccessDevices(a);
  $("devicePasswordKind").textContent = a.factoryPassword
    ? __WEBUI_TEXT__("admin.factory_default")
    : __WEBUI_TEXT__("admin.custom");
  $("devicePasswordKind").classList.toggle("warn", !!a.factoryPassword);
  $("passwordChangeLoginHint").classList.toggle("hidden", !on);
}
// One device password: the sheet and the change form are never shown together.
function openLoginSheet(mode) {
  loginSheetMode = mode;
  $("devicePasswordForm").classList.add("hidden");
  const enable = mode === "enable",
    factory = enable && !!accessState.factoryPassword;
  $("loginSheetText").textContent = !enable
    ? __WEBUI_TEXT__("admin.disable_login_text")
    : factory
      ? __WEBUI_TEXT__("admin.enable_login_factory_text")
      : __WEBUI_TEXT__("admin.enable_login_text");
  for (const id of ["loginNewWrap", "loginRepeatWrap"]) $(id).classList.toggle("hidden", !factory);
  $("loginCurrentWrap").classList.toggle("hidden", factory);
  $("loginSecurityNote").classList.toggle("hidden", !enable);
  $("loginHomeAssistantWarning").classList.toggle(
    "hidden",
    !enable || !accessState.homeAssistantRecent,
  );
  const confirm = $("loginConfirmButton");
  confirm.querySelector(".t").textContent = enable
    ? __WEBUI_TEXT__("admin.turn_on")
    : __WEBUI_TEXT__("admin.turn_off");
  confirm.classList.toggle("btnInvert", enable);
  confirm.classList.toggle("btnWarn", !enable);
  $("loginSheet").classList.remove("hidden");
  $(factory ? "loginNewPassword" : "loginCurrentPassword").focus();
}
function closeLoginSheet() {
  loginSheetMode = "";
  $("loginSheet").classList.add("hidden");
  $("devicePasswordForm").classList.remove("hidden");
  for (const id of ["loginNewPassword", "loginRepeatPassword", "loginCurrentPassword"])
    $(id).value = "";
  R.clearFieldErrors();
  $("loginEnabled").checked = !!accessState.loginEnabled;
}
async function accessAction(action, fields = {}) {
  const data = await R.api("/api/v1/auth/access", {
    method: "POST",
    body: R.body({ action, ...fields }),
  });
  if (data.token) R.storeSession(data.token, true);
  return data;
}
async function confirmLoginSheet() {
  const mode = loginSheetMode,
    confirm = $("loginConfirmButton");
  let password = $("loginCurrentPassword").value;
  if (mode === "enable" && accessState.factoryPassword) {
    const err = validateDevicePasswordClient("loginNewPassword", "loginRepeatPassword");
    if (err) return R.showFieldError(err.id, err.msg);
    password = $("loginNewPassword").value;
  } else if (!password) {
    return R.showFieldError(
      "loginCurrentPassword",
      __WEBUI_TEXT__("admin.device_password_is_required"),
    );
  }
  R.clearFieldErrors();
  confirm.disabled = true;
  try {
    // The factory password is replaced first; the device applies it once
    // the machine is idle, so wait for it before turning the login on.
    if (mode === "enable" && accessState.factoryPassword) {
      if (!(await R.command("/api/v1/device/password", { newPassword: password }))) return;
      for (let i = 0; i < 40 && accessState.factoryPassword; i++) {
        await R.sleep(500);
        await R.refreshStatus();
      }
      if (accessState.factoryPassword)
        return R.message(__WEBUI_TEXT__("admin.new_password_not_saved_yet"), "error");
    }
    await accessAction(mode, { password, device: R.deviceLabel() });
    const enable = mode === "enable";
    R.message(
      enable ? __WEBUI_TEXT__("admin.login_turned_on") : __WEBUI_TEXT__("admin.login_turned_off"),
      "ok",
    );
    await R.refreshStatus();
    closeLoginSheet();
    $("loginOnNote").hidden = !enable;
    $("loginOffNote").hidden = enable;
  } catch (e) {
    R.message(R.formatCommandError(__WEBUI_TEXT__("admin.could_not_change_login"), e), "error");
  } finally {
    confirm.disabled = false;
  }
}
async function accessQuickAction(action, okText, failText, fields) {
  try {
    await accessAction(action, fields);
    R.message(okText, "ok");
    R.refreshStatus();
  } catch (e) {
    R.message(R.formatCommandError(failText, e), "error");
  }
}
// Admin → Companion. The controller never calls out: every action is one POST,
// and connecting by address only opens the companion's own page here. A page
// that came through an unpaired companion can ask it to pair from here.
let companionState = null,
  companionAsked = false,
  companionDirect = "";
// Approval runs in the background on the controller: show its three steps
// until the link opens instead of a form that looks untouched.
let companionPairing = null;
async function companionAction(body, okText, button) {
  button?.classList.add("busy");
  try {
    await R.api("/api/v1/companion/admin", { method: "POST", body: R.body(body) });
    R.message(okText, "ok");
    return true;
  } catch (e) {
    if (e.code === "COMPANION_CODE")
      R.showFieldError("companionCode", __WEBUI_TEXT__("admin.companion_code_wrong"));
    else
      R.message(
        R.formatCommandError(__WEBUI_TEXT__("admin.could_not_update_companion"), e),
        "error",
      );
    return false;
  } finally {
    button?.classList.remove("busy");
  }
}
function companionPairUrl(text) {
  try {
    const u = new URL(/^https?:\/\//i.test(text) ? text : "http://" + text);
    return u.hostname && !u.username && !u.password ? u.origin + "/__companion/pair" : "";
  } catch (_) {
    return "";
  }
}
// "since 14:32" today, "since 3 Nov" before that.
function companionSince(ageS) {
  const at = new Date(Date.now() - ageS * 1000);
  return ageS < 86400
    ? at.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" })
    : at.toLocaleDateString(undefined, { day: "numeric", month: "short" });
}
function companionRouteText(c) {
  const [through, moving, direct] = __WEBUI_TEXT__("admin.companion_routes").split("|"),
    waits = __WEBUI_TEXT__("admin.companion_waits").split("|");
  if (c.route === "companion") return through;
  if (c.route === "pending") return moving;
  if (!c.lm) return direct;
  return (
    direct +
    " · " +
    waits[c.link !== "open" ? 0 : !c.credentials ? 1 : !c.upstream || c.refused ? 2 : 3]
  );
}
function showCompanionPairing(c) {
  const p = companionPairing;
  if (p && p.stage > 0 && c) {
    p.stage = !c.paired ? 1 : c.link !== "open" && c.relay !== "ready" ? 2 : 3;
    if (p.stage === 3 || Date.now() - p.startedAt > 45000) {
      companionPairing = null;
      R.message(
        p.stage === 3
          ? __WEBUI_TEXT__("admin.companion_approved")
          : __WEBUI_TEXT__("admin.companion_paired_waiting"),
        "ok",
      );
    } else setTimeout(R.refreshStatus, 1000);
  }
  [...$("companionSteps").children].forEach(
    (li, i) => (li.className = !p ? "" : i < p.stage ? "ok" : i > p.stage ? "" : "on"),
  );
  $("companionBusy").hidden = !companionPairing;
  return !!companionPairing;
}
function applyCompanionStatus(c) {
  if (!c || !$("companionPanel")) return;
  companionState = c;
  const busy = showCompanionPairing(c);
  const r = !c.paired && c.request,
    here = R.uiState.companionHere,
    code = here?.pairing?.state === "pending" && here.pairing.code;
  if (!!r !== companionAsked) {
    companionAsked = !!r;
    R.companionHere(true).then(() => applyCompanionStatus(companionState));
  }
  $("companionHere").hidden = !R.companionPairable();
  if (here)
    $("companionHereText").textContent = R.sub(__WEBUI_TEXT__("home.companion_pair_text"), [
      here.name,
    ]);
  $("companionConnect").classList.toggle("hidden", busy || c.paired || !!r);
  $("companionRequest").classList.toggle("hidden", busy || !r);
  $("companionPaired").classList.toggle("hidden", busy || !c.paired);
  $("companionIgnored").classList.toggle("hidden", !(c.ignored > 0));
  $("companionIgnoredText").textContent = R.sub(__WEBUI_TEXT__("admin.companion_ignored"), [
    c.ignored,
  ]);
  if (r) {
    $("companionRequestText").textContent = R.sub(__WEBUI_TEXT__("admin.companion_request"), [
      r.name,
      r.address,
      r.version,
      Math.max(1, Math.ceil(r.expiresInS / 60)),
    ]);
    $("companionCode").disabled = !r.codeReady;
    // The code this page's companion shows is typed, never filled in.
    $("companionCodeHint").textContent = !r.codeReady
      ? __WEBUI_TEXT__("admin.companion_code_waiting")
      : code
        ? R.sub(__WEBUI_TEXT__("admin.companion_code_here"), [code])
        : __WEBUI_TEXT__("admin.companion_code_hint");
    $("companionApproveButton").disabled =
      !r.codeReady || !/^\d{6}$/.test($("companionCode").value);
  } else {
    $("companionCode").value = "";
    if (__LINEA_MICRA__) $("companionApproveLm").checked = true;
  }
  if (!c.paired) return;
  // Status polls keep the row fresh, except while it is asking.
  if (!$("companionDev").querySelector(".confirm"))
    $("companionDev").replaceChildren(
      deviceRow(
        "companion",
        c.name,
        [c.address, c.version].filter(Boolean).join(" · "),
        false,
        __WEBUI_TEXT__("admin.companion_forget"),
        __WEBUI_TEXT__("admin.companion_forget_ask"),
        () =>
          companionAction({ action: "forget" }, __WEBUI_TEXT__("admin.companion_forgotten")).then(
            R.refreshStatus,
          ),
      ),
    );
  const link = c.link === "open";
  $("companionLinkState").textContent =
    (link
      ? __WEBUI_TEXT__("admin.companion_link_open")
      : __WEBUI_TEXT__("admin.companion_link_down")) +
    (c.linkAgeS >= 0
      ? " · " + R.sub(__WEBUI_TEXT__("admin.companion_since"), [companionSince(c.linkAgeS)])
      : "") +
    (!link && c.peerProtocol > 0 ? " · " + __WEBUI_TEXT__("admin.companion_protocol") : "");
  if (__LINEA_MICRA__) $("companionRouteState").textContent = companionRouteText(c);
  if (!R.uiState.companionDirty) {
    $("companionUi").checked = c.ui !== false;
    if (__LINEA_MICRA__) $("companionLm").checked = !!c.lm;
    R.uiState.companionBaseline = R.snapshotControls(companionSwitches());
  }
}
const companionSwitches = () => document.querySelectorAll("#companionPaired input");
async function saveCompanionSettings() {
  if (!R.uiState.companionDirty) return;
  const ui = $("companionUi").checked;
  // Turning it off from a screen that came through the companion cuts this
  // screen off too: say where the controller answers directly.
  if (
    !ui &&
    companionState?.ui !== false &&
    R.uiState.companionHere &&
    !confirm(R.sub(__WEBUI_TEXT__("admin.companion_ui_off_ask"), ["http://" + companionDirect]))
  )
    return;
  if (
    await companionAction(
      { action: "switches", ui, ...(__LINEA_MICRA__ && { lm: $("companionLm").checked }) },
      __WEBUI_TEXT__("admin.setting_saved"),
      $("saveCompanionButton"),
    )
  )
    sectionDirty("companion", false);
  R.refreshStatus();
}
function initCompanion() {
  R.companionHere().then(() => companionState && applyCompanionStatus(companionState));
  $("companionPairButton").onclick = async ({ currentTarget: b }) => {
    b.disabled = true;
    await R.pairCompanionHere().catch((e) => R.message(e.message, "error"));
    b.disabled = false;
    R.refreshStatus();
  };
  $("companionConnectButton").onclick = () => {
    const url = companionPairUrl($("companionAddress").value.trim());
    if (!url)
      return R.showFieldError("companionAddress", __WEBUI_TEXT__("admin.companion_address_wrong"));
    R.clearFieldErrors();
    window.open(url, "_blank", "noopener");
  };
  $("companionCode").oninput = () => applyCompanionStatus(companionState);
  $("companionApproveButton").onclick = async (event) => {
    event.preventDefault();
    if (companionPairing) return;
    companionPairing = { stage: 0, startedAt: Date.now() };
    applyCompanionStatus(companionState);
    try {
      await R.api("/api/v1/companion/admin", {
        method: "POST",
        body: R.body({
          action: "approve",
          code: $("companionCode").value,
          ...(__LINEA_MICRA__ && { lm: $("companionApproveLm").checked }),
        }),
      });
      companionPairing.stage = 1;
    } catch (e) {
      companionPairing = null;
      if (e.code === "COMPANION_CODE")
        R.showFieldError("companionCode", __WEBUI_TEXT__("admin.companion_code_wrong"));
      else
        R.message(
          R.formatCommandError(__WEBUI_TEXT__("admin.could_not_update_companion"), e),
          "error",
        );
    }
    R.refreshStatus();
  };
  $("companionIgnoreButton").onclick = () =>
    companionAction({ action: "ignore" }, __WEBUI_TEXT__("admin.companion_ignored_now")).then(
      R.refreshStatus,
    );
  $("companionUnignoreButton").onclick = () =>
    companionAction({ action: "unignore" }, __WEBUI_TEXT__("admin.companion_unignored")).then(
      R.refreshStatus,
    );
  for (const el of companionSwitches()) el.onchange = () => sectionDirty("companion", true);
  $("saveCompanionButton").onclick = saveCompanionSettings;
  $("revertCompanionButton").onclick = () => revertSection("companion");
}
function selectDetectedNetwork() {
  const o = $("staNetwork").selectedOptions[0];
  if (!o || !o.value) return;
  $("staSsid").value = o.value;
  $("staOpen").checked = o.dataset.open === "true";
  $("staPassword").value = "";
  R.updateNetworkPasswordState();
}
function showScanResults(d) {
  const select = $("staNetwork");
  select.replaceChildren();
  const prompt = document.createElement("option");
  prompt.value = "";
  prompt.textContent = d.networks.length
    ? __WEBUI_TEXT__("runtime.select_network")
    : __WEBUI_TEXT__("runtime.no_networks_found");
  select.appendChild(prompt);
  for (const n of d.networks) {
    const o = document.createElement("option");
    o.value = n.ssid;
    o.dataset.open = String(n.open);
    o.textContent =
      n.ssid +
      " (" +
      n.rssi +
      __WEBUI_TEXT__("runtime.dbm_channel") +
      n.channel +
      (n.open ? __WEBUI_TEXT__("runtime.open") : __WEBUI_TEXT__("runtime.secured")) +
      ")";
    select.appendChild(o);
  }
}
async function refreshWifiScan() {
  if (R.uiState.scanBusy || !R.webUiPollingActive()) return;
  return R.withPollGate(async () => {
    if (R.uiState.scanBusy || !R.webUiPollingActive()) return;
    R.uiState.scanBusy = true;
    try {
      const d = await R.api("/api/v1/network/scan"),
        waiting =
          (d.state === "QUEUED" || d.state === "RUNNING") && Date.now() - scanStartedAt < 3e4;
      $("scanStatus").textContent =
        d.state === "READY"
          ? __WEBUI_TEXT__("runtime.ready") +
            d.networks.length +
            __WEBUI_TEXT__("runtime.network_s")
          : waiting
            ? __WEBUI_TEXT__("setup.searching_networks")
            : __WEBUI_TEXT__("setup.scan_failed");
      if (d.state === "READY") showScanResults(d);
      if (R.webUiPollingActive() && waiting)
        R.uiState.scanTimer = setTimeout(refreshWifiScan, 1000);
    } catch (e) {
      R.message(e.message, "error");
    } finally {
      R.uiState.scanBusy = false;
    }
  });
}
// Setup's page-side bound: a scan is not polled past 30 s.
let scanStartedAt = 0;
async function startWifiScan() {
  if (R.uiState.scanBusy) return;
  scanStartedAt = Date.now();
  clearTimeout(R.uiState.scanTimer);
  $("scanStatus").textContent = __WEBUI_TEXT__("runtime.requesting");
  return R.withPollGate(async () => {
    try {
      await R.api("/api/v1/network/scan", { method: "POST", body: "{}" });
      R.message(__WEBUI_TEXT__("runtime.wi_fi_scan_started"), "ok");
      R.uiState.scanTimer = setTimeout(refreshWifiScan, 100);
    } catch (e) {
      R.message(
        R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_start_wi_fi_scan"), e),
        "error",
      );
      $("scanStatus").textContent = __WEBUI_TEXT__("runtime.error");
    }
  });
}
function loadNetworkAddress(n) {
  if (!n || R.uiState.networkAddressLoaded) return;
  R.uiState.networkAddressLoaded = true;
  formRevisions.network = n.revision;
  R.uiState.savedStaSsid = n.wifiConfigured && n.ssid ? n.ssid : "";
  R.uiState.savedStaOpen = !!(n.wifiConfigured && n.open);
  R.uiState.savedStaWifiSleep = !!n.wifiSleep;
  R.uiState.savedDeviceName = n.deviceName || "";
  if ($("deviceName")) $("deviceName").value = R.uiState.savedDeviceName;
  if ($("staWifiSleep") && !R.uiState.powerDirty)
    $("staWifiSleep").checked = R.uiState.savedStaWifiSleep;
  if (R.uiState.savedStaSsid) {
    $("staSsid").value = R.uiState.savedStaSsid;
    $("staOpen").checked = R.uiState.savedStaOpen;
    R.updateNetworkPasswordState();
  }
  $("staIpMode").value = n.ipMode === "static" ? "static" : "dhcp";
  if (n.ipMode === "static") {
    $("staStaticIp").value = n.configuredIp || "";
    $("staNetmask").value = n.configuredNetmask || "";
    $("staGateway").value = n.configuredGateway || "";
    $("staDns1").value = n.configuredDns1 || "";
    $("staDns2").value = n.configuredDns2 || "";
  }
  R.uiState.savedStaAddr = netAddrKey();
  R.updateStaticIpFieldsState();
  R.updateWifiSleepState();
  R.uiState.networkBaseline = R.snapshotControls(networkControls());
}
async function setWifiSleep() {
  const el = $("staWifiSleep");
  if (!el || !R.uiState.savedStaSsid) return;
  const a = R.uiState.savedStaAddr.split("|"),
    p = {
      action: "save",
      ssid: R.uiState.savedStaSsid,
      password: "",
      open: R.uiState.savedStaOpen,
      wifiSleep: el.checked,
      ipMode: a[0],
    };
  if (p.ipMode === "static") {
    p.ip = a[1];
    p.netmask = a[2];
    p.gateway = a[3];
    p.dns1 = a[4];
    if (a[5]) p.dns2 = a[5];
  }
  p._noReconnectWait = true;
  const ok = await R.command(
    "/api/v1/network",
    p,
    false,
    __WEBUI_TEXT__("runtime.wi_fi_sleep_saved"),
  );
  if (!ok) el.checked = R.uiState.savedStaWifiSleep;
  // The save moved the network revision: a clean form takes it up again.
  else if ($("saveNetworkButton").dataset.dirty !== "1") R.resetNetworkAddressLoaded();
  return ok;
}
function formatNetworkStatus(n) {
  let t = __WEBUI_TEXT__("admin.net_sta") + n.staState;
  if (n.ssid) t += " — " + n.ssid;
  if (n.staState === "CONNECTED" && typeof n.channel === "number" && n.channel > 0)
    t += __WEBUI_TEXT__("admin.net_channel") + n.channel;
  if (n.staIp) t += " — " + n.staIp;
  if (n.mdnsHost) t += " — " + n.mdnsHost + ".local";
  if (typeof n.signalQualityPct === "number" && typeof n.rssi === "number")
    t += R.sub(__WEBUI_TEXT__("admin.net_signal"), [n.signalQualityPct, n.rssi]);
  t +=
    (n.wifiConfigured ? __WEBUI_TEXT__("runtime.credentials_saved") : "") +
    " — " +
    (n.ipMode === "static" ? __WEBUI_TEXT__("runtime.static_ip") : __WEBUI_TEXT__("admin.dhcp"));
  if (n.wifiSleep) t += __WEBUI_TEXT__("runtime.sleep_on");
  if (n.configState === "PENDING")
    t +=
      __WEBUI_TEXT__("runtime.pending_confirm") +
      (n.confirmRemainingMs
        ? " " + Math.ceil(n.confirmRemainingMs / 1000) + __WEBUI_TEXT__("runtime.s")
        : "");
  return t;
}
async function saveDateTimeConfig() {
  const fail = __WEBUI_TEXT__("runtime.could_not_save_date_and_time_settings");
  const err = validateDateTimeClient();
  if (err) {
    R.showFieldError(err.id, err.msg);
    return;
  }
  R.clearFieldErrors();
  const payload = dateTimePayload();
  try {
    await R.command(
      "/api/v1/config",
      { ...payload, baseRevision: formRevisions.dateTime },
      1,
      __WEBUI_TEXT__("runtime.date_and_time_settings_saved"),
      fail,
      "saveDateTimeButton",
    );
    if (JSON.stringify(dateTimePayload()) === JSON.stringify(payload)) {
      R.uiState.dateTimeDirty = false;
      setSaveDirty("saveDateTimeButton", "dateTimeDirtyHint", false);
    }
    await R.refreshStatus();
  } catch (e) {
    if (!formStale(e, () => sectionDirty("dateTime", false)))
      R.message(R.formatCommandError(fail, e), "error");
    await R.refreshStatus();
  }
}
function setHeroTintMode(v) {
  try {
    localStorage.setItem(R.HERO_TINT_KEY, R.HERO_TINT_MODES.includes(v) ? v : "forecast");
  } catch (_) {}
  R.scheduleShotPaint();
}
function developmentActive() {
  return R.developmentMode;
}
function loadAdminConfig(c) {
  if (!c || !$("timezoneId")) return;
  const select = $("timezoneId"),
    previous = select.value;
  R.uiState.savedTimezoneId = c.timezoneId || "";
  R.populateTimezoneOptions().catch(() => {});
  if (!R.uiState.dateTimeDirty) {
    select.value = c.timezoneId || "";
    $("timezoneAutomatic").checked = !!c.timezoneAutomatic;
    $("ntpSyncEnabled").checked = c.ntpSyncEnabled !== false;
    setSaveDirty("saveDateTimeButton", "dateTimeDirtyHint", false);
    formRevisions.dateTime = c.revision;
  }
  const state = $("timezoneSaveState");
  if (state)
    state.textContent = c.persistFailed
      ? __WEBUI_TEXT__("runtime.timezone_save_failed")
      : c.persistPending
        ? __WEBUI_TEXT__("runtime.timezone_saving")
        : c.timezoneId
          ? __WEBUI_TEXT__("runtime.timezone_saved") + c.timezoneId
          : __WEBUI_TEXT__("runtime.timezone_pending");
  if (previous !== select.value || (!R.timezonePreviewAnchor && !R.timezonePreviewPending))
    R.refreshTimezonePreview();
  R.updateTimezoneControls();
}
function applyAdminStatus(s) {
  const unlocked = R.adminUnlockedFor(s);
  R.syncAdminSessionUi(unlocked);
  const hint = $("adminConfirmHint");
  if (hint) {
    const pending = s.network && s.network.configState === "PENDING";
    hint.classList.toggle("hidden", unlocked || !pending);
    if (pending && !unlocked) {
      const sec = s.network.confirmRemainingMs ? Math.ceil(s.network.confirmRemainingMs / 1000) : 0;
      hint.textContent =
        __WEBUI_TEXT__("runtime.pending_wi_fi_confirm_this_window_will") +
        (sec ? ". " + sec + __WEBUI_TEXT__("runtime.s_remaining") : ".");
    }
  }
  if (!unlocked) return;
  ensureUiOverridePanel();
  {
    const op = $("uiOverridePanel"),
      ob = $("uiOverrideButton"),
      oh = $("uiOverrideHint");
    if (op) op.classList.toggle("hidden", !!s.configMutable);
    if (ob) {
      const r = typeof s.webUiOverrideRemainingMs === "number" ? s.webUiOverrideRemainingMs : 0,
        a = R.webUiOverrideFor(s) && r > 0,
        n = a ? Math.ceil(r / 1e3) : 0,
        t = ob.querySelector(".t");
      if (t)
        t.textContent = a
          ? n + __WEBUI_TEXT__("runtime.s")
          : __WEBUI_TEXT__("runtime.unlock_1_min");
      if (oh)
        oh.textContent = a
          ? n + __WEBUI_TEXT__("runtime.s_restart")
          : __WEBUI_TEXT__("runtime.config_lock_override_for_1_min");
    }
  }
  $("networkStatus").textContent = formatNetworkStatus(s.network);
  loadNetworkAddress(s.network);
  applyAccessStatus(s.access);
  companionDirect = s.network.staIp || s.network.mdnsHost + ".local";
  applyCompanionStatus(s.companion);
  // Deep link (Home's access note): its panel exists only once Admin is unlocked.
  const section = location.hash && $(location.hash.slice(1));
  if (section?.offsetParent) {
    history.replaceState(null, "", location.pathname);
    section.open = true;
    section.scrollIntoView({ block: "start" });
  }
  $("apStatus").textContent =
    __WEBUI_TEXT__("runtime.ap") +
    (s.network.apActive ? __WEBUI_TEXT__("runtime.active_2") : __WEBUI_TEXT__("runtime.inactive")) +
    " — " +
    (s.network.apSsid || __WEBUI_TEXT__("runtime.ap_ssid")) +
    " — " +
    s.network.apIp +
    " — " +
    s.network.apClients +
    __WEBUI_TEXT__("runtime.client_s");
  if (!R.uiState.powerDirty) {
    if ($("bleScanIntensity"))
      $("bleScanIntensity").value = (s.bleScan && s.bleScan.scanIntensity) || "balanced";
  }
  if (!R.uiState.bleDirty && $("bleEnabled"))
    $("bleEnabled").checked = !(s.bleScan && s.bleScan.enabled === false);
  if (!R.uiState.bleDirty)
    R.uiState.bleBaseline = R.snapshotControls(
      document.querySelectorAll("#blePanel input,#blePanel select"),
    );
  // The switches take their saved values before the baselines are taken, so
  // flipping one and back before the next poll reads as clean.
  for (const id of ["powerManagementEnabled", "showDiagnosticPage"]) {
    const el = $(id),
      v = s.config && s.config[id];
    if (
      el &&
      typeof v === "boolean" &&
      !(id === "powerManagementEnabled" ? R.uiState.powerDirty : R.uiState.frontendDirty)
    )
      el.checked = v;
  }
  if (!R.uiState.powerDirty)
    R.uiState.powerBaseline = R.snapshotControls(
      document.querySelectorAll("#powerPanel input,#powerPanel select"),
    );
  if (!R.uiState.frontendDirty)
    R.uiState.frontendBaseline = R.snapshotControls(
      document.querySelectorAll("#frontendPanel input,#frontendPanel select"),
    );
  $("restartButton").disabled = !R.controlsMutable;
  applyOtaStatus(s.ota);
  loadAdminConfig(s.config);
  loadScheduledRestart(s.config);
  if (!R.uiState.dateTimeDirty)
    R.uiState.dateTimeBaseline = R.snapshotControls(
      document.querySelectorAll(
        "#dateTimePanel input,#dateTimePanel select,#dateTimePanel textarea",
      ),
    );
  const sync = s.time?.lastSyncUtcSec || 0;
  if (sync && sync !== R.uiState.timezonePreviewSync) {
    R.uiState.timezonePreviewSync = sync;
    R.refreshTimezonePreview();
  } else R.renderTimezonePreview();
}
// A 64 KiB chunk lands in seconds; a dead link must not hold the page for the
// whole 15-minute session lifetime.
const OTA_UPLOAD_TIMEOUT_MS = 2 * 60 * 1000;
const OTA_COMMAND_TIMEOUT_MS = 60 * 1000;
function otaTagText(t) {
  return t ? t.version + " · " + t.arch : "—";
}
function otaKib(n) {
  return typeof n === "number" && isFinite(n)
    ? Math.round(n / 1024) + " KiB"
    : __WEBUI_TEXT__("runtime.unknown_size");
}
function otaStatusText(o) {
  const pending = otaCommitStored();
  if (pending) return otaCheckBoot(pending, o);
  if (!o.available) return __WEBUI_TEXT__("runtime.this_controller_has_no_spare_firmware_slot");
  if (o.restartPending) return __WEBUI_TEXT__("runtime.flashed_restart_waits_until_the_shot_ends");
  if (!o.confirmed)
    return (
      __WEBUI_TEXT__("runtime.firmware_confirmation_pending") +
      (o.confirmBlockReason || __WEBUI_TEXT__("runtime.waiting_for_startup")) +
      ")." +
      (o.confirmLastError ? __WEBUI_TEXT__("runtime.esp_idf_error") + o.confirmLastError + "." : "")
    );
  if (o.state === "receiving") return __WEBUI_TEXT__("runtime.receiving_a_firmware_image");
  if (o.state === "staged") return __WEBUI_TEXT__("runtime.verified_and_waiting_for_you_to_flash");
  if (!o.safe) return __WEBUI_TEXT__("runtime.waiting_for_idle") + o.lockReason + ").";
  return __WEBUI_TEXT__("runtime.ready_for_a_firmware_image");
}
function applyOtaStatus(o) {
  if (!$("otaPanel")) return;
  if (!o) {
    $("otaStatus").textContent = __WEBUI_TEXT__("runtime.firmware_updates_are_unavailable");
    ["otaVerifyButton", "otaFlashButton", "otaDiscardButton", "otaFile"].forEach((id) => {
      if ($(id)) $(id).disabled = true;
    });
    return;
  }
  R.uiState.otaLastStatus = o;
  const staged = o.state === "staged" && o.staged ? o.staged : null,
    active = !!o.sessionActive;
  $("otaStatus").textContent =
    otaStatusText(o) +
    (active
      ? __WEBUI_TEXT__("runtime.resumable_transfer") +
        otaKib(o.nextOffset || 0) +
        " / " +
        otaKib(o.expectedBytes || 0)
      : "");
  $("otaRunning").textContent =
    __WEBUI_TEXT__("runtime.running") +
    otaTagText(o.running) +
    __WEBUI_TEXT__("runtime.update_slot") +
    otaKib(o.slotBytes);
  $("otaStaged").textContent = staged
    ? __WEBUI_TEXT__("runtime.verified_image") +
      otaTagText(staged) +
      " — " +
      otaKib(o.receivedBytes)
    : active
      ? __WEBUI_TEXT__("runtime.transfer") +
        (o.transferId || "") +
        __WEBUI_TEXT__("runtime.expires_in") +
        Math.ceil((o.sessionExpiresInMs || 0) / 6e4) +
        __WEBUI_TEXT__("runtime.min_3")
      : __WEBUI_TEXT__("runtime.no_verified_image");
  const ready =
    R.controlsMutable &&
    o.available &&
    o.safe &&
    o.confirmed &&
    !o.restartPending &&
    !R.uiState.otaBusy;
  $("otaFile").disabled = !ready;
  $("otaVerifyButton").disabled = !ready;
  $("otaFlashButton").disabled = !ready || !staged;
  $("otaDiscardButton").disabled = !ready || (!staged && !active);
  if (active && o.expectedBytes)
    $("otaProgress").value = Math.round((100 * (o.nextOffset || 0)) / o.expectedBytes);
}
// The OTA steps ride the shared API client. Each one counts as activity, so a
// long upload is not cut off by the Web UI inactivity timeout.
function otaSend(path, body, timeoutMs, method = "POST", headers = {}) {
  R.resetWebUiInactivity();
  return R.api(path, { method, body, headers, timeoutMs });
}
function otaBeginBusy() {
  R.uiState.otaBusy = true;
  applyOtaStatus(R.uiState.otaLastStatus);
}
function otaEndBusy(data, fallbackText, kind) {
  R.uiState.otaBusy = false;
  $("otaProgress").classList.add("hidden");
  $("otaProgress").value = 0;
  if (data) applyOtaStatus(data);
  else applyOtaStatus(R.uiState.otaLastStatus);
  if (fallbackText) R.message(fallbackText, kind || "");
  R.refreshStatus();
}
function otaStored() {
  try {
    return JSON.parse(localStorage.getItem("obbwOtaSession") || "null");
  } catch (_) {
    return null;
  }
}
function otaStore(v) {
  try {
    if (v) localStorage.setItem("obbwOtaSession", JSON.stringify(v));
    else localStorage.removeItem("obbwOtaSession");
  } catch (_) {}
}
const OTA_PROTOCOL_VERSION = 3;
async function otaFileIdentity(file) {
  return (await import("/js/ota-image.js?v=__FW_ASSET_TAG__")).otaFileIdentity(file);
}
function otaRemoteMatches(identity, status) {
  return !!(
    status &&
    status.transferId &&
    (!identity.transferId || status.transferId === identity.transferId) &&
    status.sha256 === identity.sha256 &&
    status.expectedBytes === identity.size &&
    status.sessionArch === identity.arch &&
    status.sessionHardware === identity.hardware &&
    status.sessionMachine === identity.machine &&
    status.sessionVersion === identity.version
  );
}
function otaSessionIdentity(identity, status) {
  if (!status || status.otaProtocolVersion !== OTA_PROTOCOL_VERSION)
    throw new Error(__WEBUI_TEXT__("runtime.this_controller_does_not_support_the_resumable"));
  if (status.runningIdentityValid !== true)
    throw new Error(__WEBUI_TEXT__("runtime.the_running_firmware_has_no_usable_image"));
  if (!status.running)
    throw new Error(__WEBUI_TEXT__("runtime.the_running_firmware_has_no_usable_image"));
  if (identity.hardware !== status.running.hardware || identity.machine !== status.running.machine)
    throw new Error(
      R.sub(__WEBUI_TEXT__("runtime.ota_image_for_another_profile"), [
        identity.hardware,
        identity.machine,
        status.running.hardware,
        status.running.machine,
      ]),
    );
  const occupied = status.sessionActive || status.state === "staged";
  if (occupied) {
    if (!otaRemoteMatches(identity, status))
      throw new Error(__WEBUI_TEXT__("runtime.another_firmware_image_owns_the_update_slot"));
    return { ...identity, transferId: status.transferId };
  }
  const saved = otaStored(),
    same =
      saved &&
      saved.size === identity.size &&
      saved.sha256 === identity.sha256 &&
      saved.arch === identity.arch &&
      saved.hardware === identity.hardware &&
      saved.machine === identity.machine &&
      saved.version === identity.version &&
      saved.transferId;
  return { ...identity, transferId: same ? saved.transferId : R.randomHex(18) };
}
function otaSessionBody(session) {
  const { size, sha256, arch, hardware, machine, version, transferId } = session;
  return JSON.stringify({ size, sha256, arch, hardware, machine, version, transferId });
}
function otaValidOffset(offset, size) {
  return (
    Number.isInteger(offset) &&
    offset >= 0 &&
    offset <= size &&
    (offset === size || offset % 4096 === 0)
  );
}
function otaRecoverable(session, status) {
  return (
    otaRemoteMatches(session, status) &&
    status.sessionActive === true &&
    status.state === "receiving" &&
    otaValidOffset(status.nextOffset, session.size)
  );
}
function otaUploadErrorText(session, status, error) {
  if (error.otaFileChanged) return error.message;
  if (
    error.code === "OTA_SESSION_EXPIRED" ||
    (status && status.lastResult === "OTA_SESSION_EXPIRED")
  )
    return __WEBUI_TEXT__("runtime.the_firmware_upload_session_expired_choose_the");
  if (error.status >= 400 && error.status < 500 && error.code !== "RECEIVE_FAILED")
    return __WEBUI_TEXT__("runtime.the_firmware_upload_was_rejected");
  if (session && otaRecoverable(session, status))
    return __WEBUI_TEXT__("runtime.the_firmware_upload_paused_choose_the_same");
  if (session && otaRemoteMatches(session, status) && status.state === "staged")
    return __WEBUI_TEXT__("runtime.the_firmware_is_verified_refresh_its_status");
  if (error.status) return __WEBUI_TEXT__("runtime.the_firmware_upload_was_rejected");
  if (session && !status) return __WEBUI_TEXT__("runtime.the_upload_result_could_not_be_checked");
  if (session && status && !status.sessionActive)
    return __WEBUI_TEXT__("runtime.the_firmware_upload_session_no_longer_exists");
  return __WEBUI_TEXT__("runtime.the_firmware_upload_failed");
}
async function otaUpload() {
  const files = $("otaFile").files,
    file = files && files[0];
  if (!file) {
    R.showFieldError("otaFile", __WEBUI_TEXT__("runtime.choose_the_firmware_bin_file_to_upload"));
    return;
  }
  R.clearFieldErrors();
  const slot = R.uiState.otaLastStatus && R.uiState.otaLastStatus.slotBytes;
  if (typeof slot === "number" && file.size > slot) {
    R.showFieldError(
      "otaFile",
      __WEBUI_TEXT__("runtime.that_file_is") +
        otaKib(file.size) +
        __WEBUI_TEXT__("runtime.larger_than_the") +
        otaKib(slot) +
        __WEBUI_TEXT__("runtime.update_slot_2"),
    );
    return;
  }
  const bar = $("otaProgress");
  bar.value = 0;
  bar.classList.remove("hidden");
  otaBeginBusy();
  let session = null,
    status = null;
  try {
    R.message(__WEBUI_TEXT__("runtime.checking_firmware_identity"));
    const identity = await otaFileIdentity(file),
      remote = await otaSend("/api/v1/ota/session", null, OTA_COMMAND_TIMEOUT_MS, "GET");
    session = otaSessionIdentity(identity, remote);
    status = await otaSend(
      "/api/v1/ota/session",
      otaSessionBody(session),
      OTA_COMMAND_TIMEOUT_MS,
      "POST",
      { "Content-Type": "application/json" },
    );
    if (!otaRemoteMatches(session, status))
      throw new Error(__WEBUI_TEXT__("runtime.the_controller_returned_a_different_or_incomplete"));
    let offset = status.nextOffset,
      highWater = offset,
      failures = 0;
    if (!otaValidOffset(offset, file.size))
      throw new Error(__WEBUI_TEXT__("runtime.the_controller_reported_an_invalid_firmware_offset"));
    otaStore(session);
    otaCommitStore(null);
    while (offset < file.size) {
      const end = Math.min(file.size, offset + 65536);
      if (!(await session.verifyRange(offset, end))) {
        const changed = new Error(__WEBUI_TEXT__("runtime.the_firmware_file_changed_while_it_was"));
        changed.otaFileChanged = true;
        throw changed;
      }
      const chunk = file.slice(offset, end),
        headers = {
          "X-OTA-Transfer": session.transferId,
          "X-OTA-Offset": String(offset),
          "X-OTA-Length": String(file.size),
          "Content-Range": "bytes " + offset + "-" + (end - 1) + "/" + file.size,
          "Content-Type": "application/octet-stream",
        };
      let reconciled = false;
      try {
        status = await otaSend("/api/v1/ota", chunk, OTA_UPLOAD_TIMEOUT_MS, "PATCH", headers);
      } catch (e) {
        failures++;
        // Back off before asking where the transfer stands, so a short blip
        // does not spend the whole retry budget at once.
        await R.sleep(1000 * failures);
        status = await otaSend("/api/v1/ota/session", null, OTA_COMMAND_TIMEOUT_MS, "GET");
        if (
          !otaRemoteMatches(session, status) ||
          (!otaRecoverable(session, status) && status.state !== "staged")
        )
          throw e;
        if (failures >= 3 && status.nextOffset !== file.size) throw e;
        reconciled = true;
      }
      const next = status.nextOffset;
      if (
        !otaRemoteMatches(session, status) ||
        !otaValidOffset(next, file.size) ||
        next > end ||
        (!reconciled && next !== end)
      )
        throw new Error(__WEBUI_TEXT__("runtime.the_controller_did_not_confirm_a_valid"));
      // Recovery may go backwards to the last durable checkpoint. Only new
      // progress beyond the high-water mark resets the bounded retry budget.
      if (next > highWater) {
        highWater = next;
        failures = 0;
      }
      offset = next;
      bar.value = Math.round((100 * offset) / file.size);
    }
    if (
      status.state !== "staged" ||
      !status.staged ||
      status.staged.arch !== identity.arch ||
      status.staged.version !== identity.version ||
      status.staged.packed !== identity.packed
    )
      throw new Error(__WEBUI_TEXT__("runtime.the_controller_did_not_report_the_expected"));
    otaEndBusy(
      status,
      __WEBUI_TEXT__("runtime.firmware_verified") +
        otaTagText(status.staged) +
        __WEBUI_TEXT__("runtime.firmware_verified_review"),
      "ok",
    );
  } catch (e) {
    let current = null;
    if (session) {
      try {
        current = await otaSend("/api/v1/ota/session", null, OTA_COMMAND_TIMEOUT_MS, "GET");
      } catch (_) {}
    }
    otaEndBusy(current, R.formatCommandError(otaUploadErrorText(session, current, e), e), "error");
  }
}
function otaCommitStored() {
  try {
    return JSON.parse(localStorage.getItem("obbwOtaCommit") || "null");
  } catch (_) {
    return null;
  }
}
function otaCommitStore(value) {
  try {
    if (value) localStorage.setItem("obbwOtaCommit", JSON.stringify(value));
    else localStorage.removeItem("obbwOtaCommit");
  } catch (_) {}
}
function otaCheckBoot(expected, status) {
  if (
    !Number.isInteger(expected.bootId) ||
    !Number.isInteger(status.bootId) ||
    !expected.imageSha256 ||
    !status.running ||
    !status.running.imageSha256
  )
    return __WEBUI_TEXT__("runtime.update_result_unverified_firmware_does_not_expose");
  if (status.bootId === expected.bootId)
    return status.restartPending || status.state === "committed"
      ? __WEBUI_TEXT__("runtime.flashed_waiting_for_the_controller_to_restart")
      : __WEBUI_TEXT__("runtime.commit_not_yet_verified_refresh_status_before");
  // Another boot settles the expectation either way: report it once, then the
  // status line shows the controller as it really is.
  if (status.running.imageSha256 !== expected.imageSha256) {
    otaCommitStore(null);
    R.message(__WEBUI_TEXT__("runtime.the_controller_restarted_into_another_image_the"), "warn");
    return __WEBUI_TEXT__("runtime.the_controller_restarted_into_another_image_the");
  }
  if (!status.confirmed)
    return (
      __WEBUI_TEXT__("runtime.expected_firmware_booted_confirmation_pending") +
      (status.confirmBlockReason || __WEBUI_TEXT__("runtime.startup_reason")) +
      ")." +
      (status.confirmLastError
        ? __WEBUI_TEXT__("runtime.esp_idf_error") + status.confirmLastError + "."
        : "")
    );
  otaCommitStore(null);
  otaStore();
  R.message(__WEBUI_TEXT__("runtime.ota_confirmed_by_the_rebooted_firmware"), "ok");
  return __WEBUI_TEXT__("runtime.ota_confirmed_by_the_rebooted_firmware");
}
async function otaFlash() {
  const current = R.uiState.otaLastStatus,
    staged = current && current.state === "staged" && current.staged;
  if (!staged) {
    R.message(__WEBUI_TEXT__("runtime.upload_and_verify_a_firmware_image_first"), "warn");
    return;
  }
  if (
    !confirm(
      __WEBUI_TEXT__("runtime.flash") +
        otaTagText(staged) +
        __WEBUI_TEXT__("runtime.and_restart_the_controller"),
    )
  )
    return;
  const saved = otaStored(),
    session = {
      size: current.expectedBytes,
      sha256: current.sha256,
      arch: current.sessionArch,
      hardware: current.sessionHardware,
      machine: current.sessionMachine,
      version: current.sessionVersion,
      transferId: current.transferId,
    },
    expected = {
      ...session,
      bootId: current.bootId,
      imageSha256:
        saved && otaRemoteMatches(saved, current) ? saved.imageSha256 : staged.imageSha256 || "",
    };
  otaBeginBusy();
  let data = null;
  try {
    data = await otaSend("/api/v1/ota/session", null, OTA_COMMAND_TIMEOUT_MS, "GET");
    if (!otaRemoteMatches(session, data) || data.state !== "staged")
      throw new Error(__WEBUI_TEXT__("runtime.the_staged_transfer_changed_before_flash"));
    expected.bootId = data.bootId;
    otaCommitStore(expected);
    for (let attempt = 1; attempt <= 3; attempt++) {
      data = null;
      try {
        data = await otaSend("/api/v1/ota/flash", null, OTA_COMMAND_TIMEOUT_MS);
        if (!otaRemoteMatches(session, data))
          throw new Error(__WEBUI_TEXT__("runtime.the_commit_response_did_not_identify_the"));
        break;
      } catch (error) {
        data = await otaSend("/api/v1/ota/session", null, OTA_COMMAND_TIMEOUT_MS, "GET");
        const rebooted =
          Number.isInteger(expected.bootId) &&
          Number.isInteger(data.bootId) &&
          data.bootId !== expected.bootId;
        if (rebooted) break;
        if (!otaRemoteMatches(session, data)) throw error;
        if (data.restartPending || data.state === "committed") break;
        if (data.state !== "staged" || attempt === 3) throw error;
      }
    }
    otaEndBusy(data, otaCheckBoot(expected, data), "");
  } catch (e) {
    // This attempt's status from the same boot with nothing committed means
    // the flash did not happen: keep no expectation that would later read as
    // an unverified commit. A device that stopped answering keeps it.
    if (data?.bootId === expected.bootId && !data.restartPending && data.state !== "committed")
      otaCommitStore(null);
    otaEndBusy(
      data,
      R.formatCommandError(
        __WEBUI_TEXT__("runtime.the_flash_result_is_unverified_reconnect_to"),
        e,
      ),
      "error",
    );
  }
}
function otaDiscard() {
  otaBeginBusy();
  otaSend("/api/v1/ota/abort", null, OTA_COMMAND_TIMEOUT_MS)
    .then((data) => {
      otaStore();
      otaCommitStore(null);
      $("otaFile").value = "";
      otaEndBusy(data, __WEBUI_TEXT__("runtime.the_verified_image_was_discarded"), "ok");
    })
    .catch((e) => {
      otaEndBusy(
        null,
        R.formatCommandError(__WEBUI_TEXT__("runtime.the_image_could_not_be_discarded"), e),
        "error",
      );
    });
}
async function setBleScanIntensity() {
  const sel = $("bleScanIntensity");
  if (!sel) return;
  const wanted = sel.value;
  try {
    await R.api("/api/v1/admin/ble-scan", {
      method: "PUT",
      body: R.body({ scanIntensity: wanted }),
    });
    R.noteReachOk();
    R.message(__WEBUI_TEXT__("runtime.ble_scan_mode_saved"), "ok");
    await R.refreshStatus();
    return true;
  } catch (x) {
    R.message(
      R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_save_ble_scan_mode"), x),
      "error",
    );
    await R.refreshStatus();
    return false;
  }
}
async function setBleEnabled() {
  const el = $("bleEnabled");
  if (!el) return;
  const wanted = el.checked;
  try {
    await R.api("/api/v1/admin/ble-scan", { method: "PUT", body: R.body({ enabled: wanted }) });
    R.noteReachOk();
    R.message(
      wanted
        ? __WEBUI_TEXT__("runtime.ble_enabled_saved")
        : __WEBUI_TEXT__("runtime.ble_disabled_saved"),
      "ok",
    );
    await R.refreshStatus();
    return true;
  } catch (x) {
    el.checked = !wanted;
    R.message(
      R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_save_ble_setting"), x),
      "error",
    );
    await R.refreshStatus();
    return false;
  }
}
function ensureUiOverridePanel() {
  if (R.developmentMode || $("uiOverridePanel")) return;
  const p = document.createElement("details");
  p.id = "uiOverridePanel";
  p.className = "cfgGroup";
  p.innerHTML =
    '<summary><svg class="cfgIcon" aria-hidden="true"><use href="#g-override"/></svg>' +
    __WEBUI_TEXT__("runtime.ui_override") +
    '</summary><div class="btnBar"><button id="uiOverrideButton" class="btnGlyph btnWarn" type="button"><span class="g" aria-hidden="true">' +
    "⏱" +
    '</span><span class="t">' +
    __WEBUI_TEXT__("runtime.unlock_1_min") +
    '</span></button></div><small id="uiOverrideHint"></small>';
  const s = document.querySelector("#adminControls .adminSession");
  if (s) s.after(p);
  const b = $("uiOverrideButton");
  if (b && !b.dataset.bound) {
    b.dataset.bound = "1";
    b.onclick = () =>
      R.api("/api/v1/ui/unlock", {
        method: "POST",
        body: R.body({ confirm: "UNSAFE_WEBUI_OVERRIDE" }),
      })
        .then(() => {
          R.noteReachOk();
          R.message(__WEBUI_TEXT__("runtime.override_on"), "ok");
          return R.refreshStatus();
        })
        .catch((e) =>
          R.message(R.formatCommandError(__WEBUI_TEXT__("runtime.override_failed"), e), "error"),
        );
  }
}
let ready = false;
let webhookDirty = false,
  webhookSaving = false,
  webhookBaseline = null;
let firmwareModeDirty = false;
const WEBHOOK_IDS = [
  "webhookEnabled",
  "webhookUrl",
  "webhookDeferDuringShot",
  "webhookBrewState",
  "webhookFirstDrop",
  "webhookEnd",
];
export function applyStatus(s) {
  applyAdminStatus(s);
  const compat = !!s.compatibilityMode;
  ["blePanel", "dateTimePanel", "webhookPanel", "companionPanel"].forEach((id) => {
    const el = $(id);
    if (el) el.classList.toggle("hidden", compat);
  });
  const diagToggle = $("showDiagnosticPage");
  if (diagToggle && diagToggle.closest("label"))
    diagToggle.closest("label").classList.toggle("hidden", compat);
  $("bleScanIntensity")?.closest("label")?.classList.toggle("hidden", compat);
  if (!firmwareModeDirty && $("firmwareModeEnabled")) {
    const enabled = !compat;
    if ($("firmwareModeEnabled").checked !== enabled) {
      $("firmwareModeEnabled").checked = enabled;
      setSaveDirty("saveFirmwareModeButton", "firmwareModeDirtyHint", false);
    }
  }
  const p = s.power;
  if (p && $("powerStatus"))
    $("powerStatus").textContent =
      p.profile +
      " · " +
      p.cpuMhz +
      __WEBUI_TEXT__("admin.mhz_range") +
      p.minMhz +
      "–" +
      p.maxMhz +
      ")" +
      (p.clockError || p.bleError
        ? __WEBUI_TEXT__("admin.apply_error") + p.clockError + "/" + p.bleError
        : "");
  const w = s && s.webhooks;
  if (!w) return;
  if (!webhookDirty) {
    $("webhookEnabled").checked = !!w.enabled;
    $("webhookUrl").value = w.url || "";
    $("webhookBrewState").checked = !!w.brewState;
    $("webhookFirstDrop").checked = !!w.firstDrop;
    $("webhookEnd").checked = !!w.end;
    $("webhookDeferDuringShot").checked = !!w.deferDuringShot;
    webhookBaseline = R.snapshotControls(WEBHOOK_IDS.map((id) => $(id)));
    setSaveDirty("saveWebhookButton", "webhookDirtyHint", false);
    formRevisions.webhooks = w.revision;
  }
  const result = w.sending
    ? __WEBUI_TEXT__("admin.sending")
    : w.lastAttemptAtMs
      ? w.lastSuccess
        ? __WEBUI_TEXT__("admin.delivered") +
          (w.lastHttpStatus ? " (HTTP " + w.lastHttpStatus + ")" : "")
        : __WEBUI_TEXT__("admin.delivery_failed") +
          (w.lastHttpStatus ? " (HTTP " + w.lastHttpStatus + ")" : "") +
          (w.lastError ? " · error " + w.lastError : "")
      : __WEBUI_TEXT__("admin.no_deliveries_yet");
  $("webhookStatus").textContent =
    result +
    __WEBUI_TEXT__("admin.sent") +
    (w.sent || 0) +
    __WEBUI_TEXT__("admin.dropped") +
    (w.dropped || 0) +
    (w.staleConfigDropped
      ? " (" + w.staleConfigDropped + __WEBUI_TEXT__("admin.after_configuration_changes")
      : "") +
    (w.workerStartFailures
      ? __WEBUI_TEXT__("admin.worker_start_failures") + w.workerStartFailures
      : "") +
    ".";
}
function validWebhookUrl(url) {
  if (!url.startsWith("http://") || /[\s#@"\\]/.test(url)) return false;
  try {
    const parsed = new URL(url);
    return parsed.protocol === "http:" && !parsed.username && !parsed.password && !!parsed.hostname;
  } catch (_) {
    return false;
  }
}
// Waits for the device to persist one request. command() settles on APPLIED;
// these saves also need the flash write, so they keep their own wait. The
// label names the setting for people; key is the config field it checks.
async function waitSaved(requestId, label = __WEBUI_TEXT__("admin.webhook_settings"), config, key) {
  const deadline = Date.now() + 8e3;
  while (Date.now() < deadline) {
    await R.sleep(150);
    const s = await R.api("/api/v1/status/admin");
    const c = s && s.lastCommand;
    if (c && c.requestId === requestId) {
      const p = s.config,
        applied =
          config && c.state === "APPLIED" && p && ((p.revision - config.baseRevision) | 0) > 0;
      if (
        config
          ? applied && p[key] === config[key] && p.persistPending === false && !p.persistFailed
          : c.state === "PERSISTED"
      )
        return;
      if (c.state === "FAILED" || c.state === "CANCELED" || (applied && p.persistFailed))
        throw new Error(__WEBUI_TEXT__("admin.could_not_save") + label);
    } else if (
      c &&
      c.requestId > requestId &&
      (c.state === "PERSISTED" || c.state === "FAILED" || c.state === "CANCELED")
    ) {
      throw new Error(__WEBUI_TEXT__("admin.save_superseded"));
    }
  }
  throw new Error(__WEBUI_TEXT__("admin.timeout_saving") + label);
}
function controlChanged(id, base) {
  const el = $(id);
  return (
    !!el && !!base && id in base && (el.type === "checkbox" ? el.checked : el.value) !== base[id]
  );
}
async function saveDurableConfigKey(key, label) {
  const el = $(key),
    wanted = el.checked;
  return R.withCommandGate(async () => {
    const p = R.withBaseRev({ [key]: wanted }),
      a = await R.api("/api/v1/config", { method: "POST", body: R.body(p) });
    await waitSaved(a.requestId, label, p, key);
    R.message(__WEBUI_TEXT__("admin.setting_saved"), "ok");
  });
}
async function saveBleSettings() {
  if (!R.uiState.bleDirty) return;
  const b = $("saveBleButton");
  b.classList.add("busy");
  let ok = true;
  try {
    if (controlChanged("bleEnabled", R.uiState.bleBaseline) && !(await setBleEnabled())) ok = false;
    if (ok) sectionDirty("ble", false);
  } finally {
    b.classList.remove("busy");
  }
  return ok;
}
async function savePowerSettings() {
  if (!R.uiState.powerDirty) return;
  const b = $("savePowerButton");
  b.classList.add("busy");
  let ok = true;
  try {
    if (controlChanged("powerManagementEnabled", R.uiState.powerBaseline)) {
      try {
        await saveDurableConfigKey(
          "powerManagementEnabled",
          __WEBUI_TEXT__("admin.power_management_setting"),
        );
      } catch (e) {
        R.message(e.message, "error");
        ok = false;
      }
    }
    if (
      controlChanged("bleScanIntensity", R.uiState.powerBaseline) &&
      !(await setBleScanIntensity())
    )
      ok = false;
    if (controlChanged("staWifiSleep", R.uiState.powerBaseline) && !(await setWifiSleep()))
      ok = false;
    if (ok) sectionDirty("power", false);
  } finally {
    b.classList.remove("busy");
  }
  return ok;
}
async function saveFrontendSettings() {
  if (!R.uiState.frontendDirty) return;
  const b = $("saveFrontendButton");
  b.classList.add("busy");
  let ok = true;
  try {
    if (controlChanged("showDiagnosticPage", R.uiState.frontendBaseline)) {
      try {
        await saveDurableConfigKey(
          "showDiagnosticPage",
          __WEBUI_TEXT__("admin.diagnostic_page_setting"),
        );
      } catch (e) {
        R.message(e.message, "error");
        ok = false;
      }
    }
    if (controlChanged("heroTint", R.uiState.frontendBaseline))
      setHeroTintMode($("heroTint").value);
    if (ok) sectionDirty("frontend", false);
  } finally {
    b.classList.remove("busy");
  }
  return ok;
}
// Scheduled restart: two 24-hour fields stored as one minute of the day.
const restartMinute = () =>
  Number($("scheduledRestartHour").value) * 60 + Number($("scheduledRestartMinute").value);
const restartTime = (m) => R.pad2(Math.floor(m / 60)) + ":" + R.pad2(m % 60);
const RESTART_WAIT = {
  shot: __WEBUI_TEXT__("admin.scheduled_restart_wait_shot"),
  update: __WEBUI_TEXT__("admin.scheduled_restart_wait_update"),
  update_unconfirmed: __WEBUI_TEXT__("admin.scheduled_restart_wait_update"),
  busy: __WEBUI_TEXT__("admin.scheduled_restart_wait_busy"),
};
function scheduledRestartStatus(c) {
  if (!c.scheduledRestartEnabled) return "";
  const at = restartTime(c.scheduledRestartMinuteOfDay);
  switch (c.scheduledRestartState) {
    case "armed":
      return R.sub(__WEBUI_TEXT__("admin.scheduled_restart_next"), [at]);
    case "pending":
      return R.sub(RESTART_WAIT[c.scheduledRestartCause] || RESTART_WAIT.busy, [at]);
    case "restarting":
      return __WEBUI_TEXT__("admin.scheduled_restart_now");
    case "no_timezone":
      return __WEBUI_TEXT__("admin.scheduled_restart_no_timezone");
    default:
      return __WEBUI_TEXT__("admin.scheduled_restart_no_clock");
  }
}
function loadScheduledRestart(c) {
  if (!c || !$("scheduledRestartEnabled") || typeof c.scheduledRestartMinuteOfDay !== "number")
    return;
  if (!R.uiState.scheduledRestartDirty) {
    $("scheduledRestartEnabled").checked = !!c.scheduledRestartEnabled;
    $("scheduledRestartHour").value = R.pad2(Math.floor(c.scheduledRestartMinuteOfDay / 60));
    $("scheduledRestartMinute").value = R.pad2(c.scheduledRestartMinuteOfDay % 60);
    R.uiState.scheduledRestartBaseline = R.snapshotControls(
      document.querySelectorAll("#scheduledRestartPanel input"),
    );
    formRevisions.scheduledRestart = c.revision;
  }
  $("scheduledRestartStatus").textContent = R.uiState.scheduledRestartDirty
    ? ""
    : scheduledRestartStatus(c);
}
async function saveScheduledRestart() {
  if (!R.uiState.scheduledRestartDirty) return;
  for (const id of ["scheduledRestartHour", "scheduledRestartMinute"]) {
    const el = $(id);
    if (!el.checkValidity() || el.value === "") {
      R.message(__WEBUI_TEXT__("admin.scheduled_restart_invalid_time"), "error");
      el.focus();
      return false;
    }
  }
  const b = $("saveScheduledRestartButton");
  b.classList.add("busy");
  try {
    await R.withCommandGate(async () => {
      const p = {
          scheduledRestartEnabled: $("scheduledRestartEnabled").checked,
          scheduledRestartMinuteOfDay: restartMinute(),
          baseRevision: formRevisions.scheduledRestart,
        },
        a = await R.api("/api/v1/config", { method: "POST", body: R.body(p) });
      await waitSaved(
        a.requestId,
        __WEBUI_TEXT__("admin.scheduled_restart_setting"),
        p,
        "scheduledRestartMinuteOfDay",
      );
      R.message(__WEBUI_TEXT__("admin.setting_saved"), "ok");
    });
    sectionDirty("scheduledRestart", false);
    return true;
  } catch (e) {
    if (!formStale(e, () => sectionDirty("scheduledRestart", false))) R.message(e.message, "error");
    return false;
  } finally {
    b.classList.remove("busy");
    R.refreshStatus();
  }
}
// Each Admin section row summarizes its current values; keys are the
// sections' data-g names.
const onOff = (id) =>
  $(id)?.checked ? __WEBUI_TEXT__("runtime.on_2") : __WEBUI_TEXT__("runtime.off_2");
const ADMIN_SUMMARIES = {
  firmware: () => onOff("firmwareModeEnabled"),
  ble: () => onOff("bleEnabled"),
  network: () => $("staSsid").value.trim() || __WEBUI_TEXT__("admin.sum_not_set"),
  energy: () =>
    onOff("powerManagementEnabled") +
    ($("staWifiSleep").checked ? " · " + __WEBUI_TEXT__("admin.wi_fi_sleep") : ""),
  clock: () => ($("timezoneId").value || R.uiState.savedTimezoneId || "").replaceAll("_", " "),
  frontend: () => $("heroTint").selectedOptions[0].text,
  schedule: () =>
    $("scheduledRestartEnabled").checked
      ? restartTime(restartMinute())
      : onOff("scheduledRestartEnabled"),
  webhooks: () => onOff("webhookEnabled"),
  events: () => {
    const n = ["webhookBrewState", "webhookFirstDrop", "webhookEnd"].filter(
      (id) => $(id).checked,
    ).length;
    return n === 3
      ? __WEBUI_TEXT__("settings.sum_all_on")
      : R.sub(__WEBUI_TEXT__("settings.sum_n_of_m"), [n, 3]);
  },
  access: () =>
    $("loginEnabled").checked
      ? __WEBUI_TEXT__("admin.sum_password_required")
      : __WEBUI_TEXT__("admin.sum_no_password"),
  update: () => document.documentElement.dataset.fw,
  companion: () => {
    const c = companionState;
    if (!c?.paired)
      return c?.request
        ? R.sub(__WEBUI_TEXT__("admin.sum_companion_request"), [c.request.name])
        : __WEBUI_TEXT__("admin.sum_not_paired");
    return R.sub(
      c.link === "open"
        ? __WEBUI_TEXT__("admin.sum_companion_connected")
        : c.peerProtocol > 0
          ? __WEBUI_TEXT__("admin.sum_companion_update")
          : __WEBUI_TEXT__("admin.sum_companion_disconnected"),
      [c.name],
    );
  },
};
const renderAdminRows = () => R.renderGroupRows($("view-admin"), ADMIN_SUMMARIES);
export function init() {
  if (ready) return;
  ready = true;
  R.registerViewStatus("admin", (s) => {
    applyStatus(s);
    renderAdminRows();
  });
  R.initGroupRows($("view-admin"));
  $("view-admin").addEventListener("input", renderAdminRows);
  $("view-admin").addEventListener("change", renderAdminRows);
  const sectionChanged = (sel, mark) => {
    document.querySelectorAll(sel + " input," + sel + " select").forEach((el) => {
      el.addEventListener("input", mark);
      el.addEventListener("change", mark);
    });
  };
  for (const name of ["ble", "power", "frontend", "scheduledRestart"])
    sectionChanged("#" + name + "Panel", () => sectionDirty(name, true));
  // Two-digit hours and minutes once a value settles (stepper keys or blur).
  for (const id of ["scheduledRestartHour", "scheduledRestartMinute"])
    $(id).addEventListener("change", (e) => {
      if (e.target.value !== "" && e.target.checkValidity())
        e.target.value = R.pad2(e.target.value);
    });
  $("saveScheduledRestartButton").onclick = saveScheduledRestart;
  $("revertScheduledRestartButton").onclick = () => revertSection("scheduledRestart");
  $("saveBleButton").onclick = saveBleSettings;
  $("revertBleButton").onclick = () => revertSection("ble");
  $("savePowerButton").onclick = savePowerSettings;
  $("revertPowerButton").onclick = () => revertSection("power");
  $("saveFrontendButton").onclick = saveFrontendSettings;
  $("revertFrontendButton").onclick = () => revertSection("frontend");
  initCompanion();
  const firmwareModeChanged = () => {
    firmwareModeDirty = true;
    setSaveDirty("saveFirmwareModeButton", "firmwareModeDirtyHint", true);
  };
  const fwEl = $("firmwareModeEnabled");
  if (fwEl) {
    fwEl.addEventListener("input", firmwareModeChanged);
    fwEl.addEventListener("change", firmwareModeChanged);
  }
  $("saveFirmwareModeButton").onclick = () => {
    const wanted = $("firmwareModeEnabled").checked;
    if (
      !confirm(
        wanted
          ? __WEBUI_TEXT__("admin.firmware_mode_enable_confirm")
          : __WEBUI_TEXT__("admin.firmware_mode_disable_confirm"),
      )
    )
      return;
    R.command(
      "/api/v1/firmware-mode",
      { enabled: wanted },
      undefined,
      undefined,
      __WEBUI_TEXT__("admin.could_not_save_firmware_mode"),
      "saveFirmwareModeButton",
    );
  };
  $("revertFirmwareModeButton").onclick = () => {
    if (!firmwareModeDirty) return;
    if (!confirm(__WEBUI_TEXT__("admin.discard_unsaved_changes"))) return;
    firmwareModeDirty = false;
    setSaveDirty("saveFirmwareModeButton", "firmwareModeDirtyHint", false);
    R.refreshStatus();
  };
  // A form lets password managers fill the field; Enter submits it once.
  let unlocking = false;
  const unlock = () => {
    if (developmentActive()) return;
    const pw = $("adminUnlockPassword").value;
    if (unlocking) return;
    if (!pw) {
      R.showFieldError("adminUnlockPassword", __WEBUI_TEXT__("admin.device_password_is_required"));
      return;
    }
    R.clearFieldErrors();
    unlocking = true;
    R.api("/api/v1/admin/unlock", { method: "POST", body: R.body({ password: pw }) })
      .then(() => {
        $("adminUnlockPassword").value = "";
        R.noteReachOk();
        R.message(__WEBUI_TEXT__("admin.administration_unlocked"), "ok");
        return R.refreshStatus();
      })
      .catch((e) =>
        R.message(
          R.formatCommandError(__WEBUI_TEXT__("admin.could_not_unlock_administration"), e),
          "error",
        ),
      )
      .finally(() => (unlocking = false));
  };
  $("adminUnlockForm").onsubmit = (e) => {
    e.preventDefault();
    unlock();
  };
  $("adminLockButton").onclick = lockAdmin;
  $("restartButton").onclick = () => {
    if (confirm(__WEBUI_TEXT__("admin.restart_the_controller")))
      R.command("/api/v1/control/restart");
  };
  $("scanNetworkButton").onclick = startWifiScan;
  $("staNetwork").onchange = selectDetectedNetwork;
  $("staOpen").onchange = R.updateNetworkPasswordState;
  $("staIpMode").onchange = R.updateStaticIpFieldsState;
  $("saveDateTimeButton").onclick = saveDateTimeConfig;
  $("revertDateTimeButton").onclick = revertDateTimeConfig;
  $("timezoneId").addEventListener("change", timeZoneSelectionChanged);
  $("timezoneAutomatic").onchange = changeTimezoneMode;
  $("ntpSyncEnabled").onchange =
    $("ntpServerPreset").onchange =
    $("ntpServerCustom").oninput =
      markDateTimeDirty;
  const networkChanged = () => setSaveDirty("saveNetworkButton", "", true);
  document.querySelectorAll("#networkPanel input,#networkPanel select").forEach((el) => {
    el.addEventListener("input", networkChanged);
    el.addEventListener("change", networkChanged);
  });
  const passwordChanged = () =>
    setSaveDirty(
      "changeDevicePasswordButton",
      "",
      !!$("newDevicePassword").value || !!$("confirmDevicePassword").value,
    );
  ["newDevicePassword", "confirmDevicePassword"].forEach((id) => {
    $(id).addEventListener("input", passwordChanged);
    $(id).addEventListener("change", passwordChanged);
  });
  $("syncTimeButton").onclick = () => R.command("/api/v1/time/sync");
  const webhookChanged = () => {
    webhookDirty = true;
    setSaveDirty("saveWebhookButton", "webhookDirtyHint", true);
  };
  WEBHOOK_IDS.forEach((id) => {
    const el = $(id);
    el.addEventListener("input", webhookChanged);
    el.addEventListener("change", webhookChanged);
  });
  $("saveWebhookButton").onclick = () => {
    // .busy only blocks pointer clicks; a keyboard repeat must not save twice.
    if (webhookSaving) return;
    const url = $("webhookUrl").value.trim(),
      enabled = $("webhookEnabled").checked;
    if ((enabled || url) && !validWebhookUrl(url)) {
      R.showFieldError("webhookUrl", __WEBUI_TEXT__("admin.enter_a_lowercase_http_url_https_is"));
      return;
    }
    R.clearFieldErrors();
    webhookSaving = true;
    return R.withCommandGate(async () => {
      try {
        $("saveWebhookButton").classList.add("busy");
        R.message(__WEBUI_TEXT__("admin.saving_webhooks"));
        const accepted = await R.api("/api/v1/webhooks", {
          method: "POST",
          body: R.body({
            action: "save",
            enabled: enabled,
            url: url,
            deferDuringShot: $("webhookDeferDuringShot").checked,
            brewState: $("webhookBrewState").checked,
            firstDrop: $("webhookFirstDrop").checked,
            end: $("webhookEnd").checked,
            baseRevision: formRevisions.webhooks,
          }),
        });
        await waitSaved(accepted.requestId);
        webhookDirty = false;
        setSaveDirty("saveWebhookButton", "webhookDirtyHint", false);
        R.message(__WEBUI_TEXT__("admin.webhook_settings_saved"), "ok");
      } catch (e) {
        if (!formStale(e, () => (webhookDirty = false)))
          R.message(
            R.formatCommandError(__WEBUI_TEXT__("admin.could_not_save_webhook_settings"), e),
            "error",
          );
        await R.refreshStatus();
      } finally {
        webhookSaving = false;
        $("saveWebhookButton").classList.remove("busy");
      }
    });
  };
  $("testWebhookButton").onclick = () =>
    R.command(
      "/api/v1/webhooks",
      { action: "test" },
      false,
      __WEBUI_TEXT__("admin.webhook_test_queued"),
    );
  $("saveNetworkButton").onclick = () => {
    const err = validateNetworkClient();
    if (err) {
      R.showFieldError(err.id, err.msg);
      return;
    }
    R.clearFieldErrors();
    const payload = networkSavePayload(),
      prefsOnly = networkPreferencesOnly(payload);
    if (!prefsOnly) {
      const staticMode = $("staIpMode").value === "static";
      if (
        !confirm(
          staticMode
            ? __WEBUI_TEXT__("admin.save_static_ip_and_restart_open_the")
            : __WEBUI_TEXT__("admin.save_wi_fi_and_restart"),
        )
      )
        return;
    } else payload._noReconnectWait = true;
    R.command(
      "/api/v1/network",
      payload,
      undefined,
      undefined,
      undefined,
      "saveNetworkButton",
    ).then((ok) => {
      if (!ok) return;
      $("staPassword").value = "";
      setSaveDirty("saveNetworkButton", "", false);
      R.resetNetworkAddressLoaded();
    });
  };
  $("forgetNetworkButton").onclick = () => {
    if (confirm(__WEBUI_TEXT__("admin.forget_the_sta_network_and_restart")))
      R.command("/api/v1/network", { action: "forget" }).finally(() =>
        R.resetNetworkAddressLoaded(),
      );
  };
  $("changeDevicePasswordButton").onclick = () => {
    const err = validateDevicePasswordClient();
    if (err) {
      R.showFieldError(err.id, err.msg);
      return;
    }
    R.clearFieldErrors();
    R.command(
      "/api/v1/device/password",
      { newPassword: $("newDevicePassword").value },
      undefined,
      undefined,
      undefined,
      "changeDevicePasswordButton",
    ).then((ok) => {
      if (!ok) return;
      ["newDevicePassword", "confirmDevicePassword"].forEach((id) => ($(id).value = ""));
      setSaveDirty("changeDevicePasswordButton", "", false);
    });
  };
  $("revertWebhookButton").onclick = () => {
    if (!webhookDirty) return;
    if (!confirm(__WEBUI_TEXT__("admin.discard_unsaved_changes"))) return;
    R.restoreSnapshot(webhookBaseline);
    webhookDirty = false;
    setSaveDirty("saveWebhookButton", "webhookDirtyHint", false);
    R.clearFieldErrors();
    R.refreshStatus();
  };
  $("revertNetworkButton").onclick = revertNetworkConfig;
  // Switching back to the saved state cancels the open confirmation.
  $("loginEnabled").onchange = () =>
    $("loginEnabled").checked === !!accessState.loginEnabled
      ? closeLoginSheet()
      : openLoginSheet(accessState.loginEnabled ? "disable" : "enable");
  $("loginConfirmButton").onclick = confirmLoginSheet;
  $("loginCancelButton").onclick = closeLoginSheet;
  $("signOutOthersButton").onclick = () =>
    accessQuickAction(
      "signOutOthers",
      __WEBUI_TEXT__("admin.other_devices_signed_out"),
      __WEBUI_TEXT__("admin.could_not_sign_out_other_devices"),
    );
  $("revertDevicePasswordButton").onclick = () => {
    if (!$("newDevicePassword").value && !$("confirmDevicePassword").value) return;
    if (!confirm(__WEBUI_TEXT__("admin.discard_unsaved_changes"))) return;
    ["newDevicePassword", "confirmDevicePassword"].forEach((id) => ($(id).value = ""));
    setSaveDirty("changeDevicePasswordButton", "", false);
  };
  $("otaVerifyButton").onclick = otaUpload;
  $("otaFlashButton").onclick = otaFlash;
  $("otaDiscardButton").onclick = otaDiscard;
  $("factoryResetButton").onclick = () => {
    if (confirm(__WEBUI_TEXT__("admin.restore_all_factory_settings_this_erases_wi")))
      R.command("/api/v1/factory-reset", { confirm: "ERASE_ALL_SETTINGS" });
  };
  R.populateTimezoneOptions().catch(() => {});
  $("heroTint").value = R.heroTintMode();
}
