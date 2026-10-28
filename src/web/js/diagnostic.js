"use strict";
import * as R from "./runtime.js?v=__FW_ASSET_TAG__";
const $ = R.$;
function renderMicraCloudDiagnostic(lm, viaCompanion) {
  ensureMicraRows();
  const set = (id, value) => {
    const el = $("dMicra" + id);
    if (el) el.textContent = value;
  };
  const none = __WEBUI_TEXT__("diagnostic.cloud_no_call"),
    call = lm.cloudCall;
  set("CloudEmail", lm.email || __WEBUI_TEXT__("diagnostic.cloud_no_account"));
  set(
    "CloudMachine",
    lm.accountConfigured
      ? lm.selectedName || lm.selectedSerial
      : __WEBUI_TEXT__("diagnostic.cloud_no_machine"),
  );
  set(
    "CloudTime",
    call
      ? call.startedAtUtcSec
        ? R.formatWallTimeLocal(call.startedAtUtcSec) + __WEBUI_TEXT__("diagnostic.utc_suffix")
        : "—"
      : none,
  );
  set("CloudApi", call ? call.method + " " + call.api : none);
  const label = __WEBUI_TEXT__("diagnostic.cloud_results").split("|")[
    [
      "success",
      "canceled",
      "http_error",
      "transport_error",
      "invalid_response",
      "response_too_large",
      "setup_error",
    ].indexOf(call?.result)
  ];
  set(
    "CloudResult",
    call
      ? (label || call.result) +
          (call.httpStatus ? __WEBUI_TEXT__("diagnostic.http_prefix") + call.httpStatus : "") +
          (call.transportStatus ? " · " + call.transportStatus : "")
      : none,
  );
  set("CloudDuration", call ? call.durationMs + __WEBUI_TEXT__("runtime.ms_2") : none);
  renderMicraWebSocket(lm, set, viaCompanion);
}

// On the companion route the state comes from the companion's feed: the row
// says so, and the controller's own socket figures (traffic, pong, heap) hide.
function renderMicraWebSocket(lm, set, via) {
  const ws = lm.websocket || {},
    unknown = "—",
    api = lm.connectionType === "api",
    link = api ? 0 : ws.live ? 1 : ws.state === "paused" ? 2 : 0;
  const age = (value) =>
    value && ws.nowMs != null
      ? (((ws.nowMs - value) >>> 0) / 1000).toFixed(1) + __WEBUI_TEXT__("runtime.s_2")
      : unknown;
  set(
    "WsState",
    (via
      ? __WEBUI_TEXT__("diagnostic.ws_via_companion")
      : (api ? "API" : (ws.state || unknown).replace(/_/g, " ")) +
        (ws.reason && ws.reason !== "none" ? " · " + ws.reason.replace(/_/g, " ") : "")) +
      (link ? " · " + __WEBUI_TEXT__("diagnostic.ws_links").split("|")[link - 1] : ""),
  );
  const times = via ? ["message", "power"] : ["message", "power", "pong"];
  set(
    "WsTiming",
    times.map((k) => k + " " + age(ws[k + "AtMs"])).join(" · ") +
      (via
        ? ""
        : __WEBUI_TEXT__("diagnostic.retry_prefix") +
          (ws.retryRemainingMs || 0) +
          __WEBUI_TEXT__("diagnostic.ms_stop") +
          (ws.stoppedLatencyMs ?? "—") +
          " / " +
          (ws.maxStoppedLatencyMs ?? "—") +
          " ms"),
  );
  for (const el of [$("dMicraWsTraffic")?.parentElement, $("dMicraWsHeap")])
    if (el) el.hidden = !!via;
  set(
    "WsTraffic",
    ["rx", "tx"]
      .map(
        (k) =>
          k.toUpperCase() +
          " " +
          (ws[k + "BytesPerSecond"] || 0) +
          __WEBUI_TEXT__("diagnostic.bytes_per_s") +
          (ws[k + "BytesPerMinute"] || 0) +
          __WEBUI_TEXT__("diagnostic.bytes_per_min") +
          (ws[k + "Bytes"] || 0) +
          __WEBUI_TEXT__("diagnostic.bytes_suffix"),
      )
      .join("; ") +
      " · " +
      (ws.messages || 0) +
      __WEBUI_TEXT__("diagnostic.messages_sep") +
      (ws.errors || 0) +
      __WEBUI_TEXT__("diagnostic.errors_suffix"),
  );
  set("WsPlanned", String(ws.plannedConnections || 0));
  set("WsUnexpected", String(ws.unexpectedConnections || 0));
  set("Cleaning", R.formatMicraCleaning(lm));
  set(
    "CleaningHint",
    lm.accountConfigured && lm.observeState && !api && ws.cleaningAvailable
      ? __WEBUI_TEXT__("diagnostic.last_reported") +
          " " +
          age(ws.cleaningAtMs) +
          __WEBUI_TEXT__("diagnostic.websocket_suffix") +
          (!ws.live || (ws.machineConnectedKnown && !ws.machineConnected)
            ? " · " + __WEBUI_TEXT__("diagnostic.stale")
            : "")
      : "",
  );
  set(
    "WsHeap",
    __WEBUI_TEXT__("diagnostic.ws_prefix") +
      (ws.retainedBytes || 0) +
      __WEBUI_TEXT__("diagnostic.psram_heap") +
      ["connect", "stop"]
        .map(
          (k) =>
            k +
            " " +
            (ws[k + "FreeDelta"] ?? unknown) +
            "/" +
            (ws[k + "LargestDelta"] ?? unknown) +
            " B",
        )
        .join(", ") +
      __WEBUI_TEXT__("diagnostic.failures_sep") +
      (ws.allocationFailures || 0),
  );
}
function ensureMicraRows() {
  const cloud = $("micraCloudDiagnostics");
  const row = (id, title) => {
    if ($(id) || !cloud) return;
    const row = document.createElement("div"),
      label = document.createElement("strong"),
      v = document.createElement("div");
    row.className = "metric micraOnly";
    label.textContent = title;
    v.id = id;
    row.append(label, v);
    cloud.append(row);
  };
  const titles = __WEBUI_TEXT__("diagnostic.cloud_titles").split("|");
  [
    "Email",
    "Machine",
    "Time",
    "Api",
    "Result",
    "Duration",
    "WsState",
    "WsTraffic",
    "WsPlanned",
    "WsUnexpected",
  ].forEach((id, i) => row("dMicra" + (i < 6 ? "Cloud" : "") + id, titles[i]));
  for (const [id, anchor] of [
    ["dMicraWsTiming", "dMicraWsState"],
    ["dMicraWsHeap", "hHeapLargest"],
  ]) {
    if ($(id) || !$(anchor)) continue;
    const hint = document.createElement("small");
    hint.id = id;
    hint.className = "fieldHint micraOnly";
    $(anchor).parentElement.append(hint);
  }
}
// Status paints arrive every frame: a table or link row rebuilds only when its
// data changes, so clicks, focus, selection and live-region text survive.
function unchanged(el, data) {
  const key = JSON.stringify(data);
  return el._key === key || ((el._key = key), false);
}
function fillRows(body, rows) {
  if (unchanged(body, rows)) return;
  body.replaceChildren();
  for (const values of rows) {
    const tr = body.insertRow();
    for (const value of values) tr.insertCell().textContent = value;
  }
}
function applyScaleCommands(scale) {
  $("scaleCommandTable").hidden = !scale.supportedCommandsKnown;
  fillRows(
    $("scaleCommandRows"),
    (scale.supportedCommands || []).map((c) => [c.name, c.code]),
  );
  $("scaleCommandHint").textContent = !scale.supportedCommandsKnown
    ? __WEBUI_TEXT__("diagnostic.commands_unknown")
    : scale.model === "bookoo_ultra"
      ? __WEBUI_TEXT__("diagnostic.ultra_charging_note")
      : "";
}
function formatWallTime(unixSec, tz) {
  return R.formatWallTimeLocal(unixSec + tz * 60);
}
function formatDiagnosticTimezoneOffset(c) {
  if (c.timezoneOffsetKnown) return R.formatTzLabel(c.appliedTimezoneOffsetMinutes);
  if (c.timezoneResolution === "clock_unavailable") return "—";
  const reason = {
    unconfigured: __WEBUI_TEXT__("runtime.timezone_unconfigured"),
    unknown_zone: __WEBUI_TEXT__("runtime.timezone_unknown_zone"),
    rules_out_of_range: __WEBUI_TEXT__("runtime.timezone_rules_out_of_range"),
  }[c.timezoneResolution];
  return R.formatTzLabel(0) + (reason ? " — " + reason : "");
}
function clearLogView() {
  R.uiState.logEvents = [];
  R.uiState.logMissed = 0;
  const d = $("logDropped"),
    el = $("log");
  if (d) d.hidden = true;
  if (el) el.value = "";
}
function ntpStateLabel(t) {
  if (!t) return __WEBUI_TEXT__("runtime.not_available");
  return (
    {
      SYNCED: __WEBUI_TEXT__("runtime.synced"),
      STALE: __WEBUI_TEXT__("runtime.stale"),
      SYNCING: __WEBUI_TEXT__("runtime.syncing"),
      FAILED: __WEBUI_TEXT__("runtime.failed"),
      OFF: __WEBUI_TEXT__("runtime.not_synced_2"),
    }[t.state] || __WEBUI_TEXT__("runtime.waiting_for_network")
  );
}
const RR = __WEBUI_TEXT__("diagnostic.reset_reasons").split("|");
function formatUptime(ms) {
  let x = ~~(ms / 1e3),
    p = [],
    d = ~~(x / 86400);
  x %= 86400;
  const h = ~~(x / 3600);
  x %= 3600;
  const m = ~~(x / 60);
  if (d) p.push(d + "d");
  if (d || h) p.push(h + "h");
  if (d || h || m) p.push(m + "m");
  p.push((x % 60) + "s");
  return p.join(" ");
}
function updH(h, s) {
  function b(n, k = false) {
    return !Number.isFinite(n) || n < 0
      ? "—"
      : k
        ? (n / 1024).toFixed(1) + __WEBUI_TEXT__("diagnostic.kb_suffix")
        : n >= 1048576
          ? (n / 1048576).toFixed(1) + "M"
          : n >= 1024
            ? Math.round(n / 1024) + "K"
            : n + "b";
  }
  const kb = (n) => b(n, true);
  const t = (i, v) => ($(i).textContent = v || "—");
  const cpu = [
    "hCpu5s",
    "hCpu1m",
    "hCpu5m",
    "hCpuMhz",
    "hTemp",
    "hTPeak",
    "hRamT",
    "hRamU",
    "hRamF",
  ];
  if (!h) {
    cpu
      .concat([
        "hHeapMin",
        "hHeapLargest",
        "hPsramT",
        "hPsramF",
        "hPsramL",
        "hUptime",
        "hResetReason",
      ])
      .forEach((i) => t(i, "—"));
    return;
  }
  t("hUptime", typeof h.uptimeMs === "number" && h.uptimeMs >= 0 ? formatUptime(h.uptimeMs) : "—");
  t("hResetReason", s ? RR[s.resetReasonCode] || "?" : "");
  t("hHeapMin", kb(h.minimumFreeHeapBytes));
  t("hHeapLargest", kb(h.largestFreeHeapBlockBytes) + (h.heap ? " · " + h.heap : ""));
  t("hPsramT", b(h.psramSizeBytes));
  t("hPsramF", b(h.psramFreeBytes));
  t("hPsramL", kb(h.psramLargestFreeBlockBytes));
  const w = h.hwmon;
  if (!w) {
    cpu.forEach((i) => t(i, "—"));
    return;
  }
  const load = (v) => (w.cpuLoadValid && typeof v === "number" ? v.toFixed(2) : "");
  const split = (t, a, b) => {
    t = load(t);
    return t && typeof a === "number" && typeof b === "number"
      ? t + " (" + a.toFixed(2) + " + " + b.toFixed(2) + ")"
      : t;
  };
  t("hCpu5s", split(w.cpuLoad5s, w.cpu0Busy, w.cpu1Busy));
  t("hCpu1m", load(w.cpuLoad1m));
  t("hCpu5m", load(w.cpuLoad5m));
  t(
    "hCpuMhz",
    typeof w.cpuMhz === "number" && w.cpuMhz > 0 ? w.cpuMhz + __WEBUI_TEXT__("runtime.mhz") : "—",
  );
  t("hTemp", w.tempValid ? w.tempC.toFixed(1) + __WEBUI_TEXT__("runtime.c") : "");
  t("hTPeak", w.tempValid ? w.tempPeakC.toFixed(1) + __WEBUI_TEXT__("runtime.c") : "");
  t("hRamT", b(w.ramTotalBytes));
  t("hRamU", b(w.ramUsedBytes));
  t("hRamF", b(w.ramFreeBytes));
}
function formatExtractionGuard(d) {
  return !d.guardEnabled
    ? __WEBUI_TEXT__("runtime.off_2")
    : d.extended
      ? __WEBUI_TEXT__("runtime.ext") +
        (d.goal ?? "—") +
        __WEBUI_TEXT__("runtime.g_or") +
        R.ms(d.minBbwBrewRemainingMs, 1) +
        __WEBUI_TEXT__("runtime.s_left")
      : d.inShot
        ? __WEBUI_TEXT__("runtime.on_2")
        : __WEBUI_TEXT__("runtime.idle");
}
function formatSlowExtractionGuard(d) {
  return !d.guardEnabled
    ? __WEBUI_TEXT__("runtime.off_2")
    : d.extended
      ? __WEBUI_TEXT__("runtime.ext") + (d.goal ?? "—") + __WEBUI_TEXT__("runtime.g")
      : d.inShot
        ? __WEBUI_TEXT__("runtime.on_2")
        : __WEBUI_TEXT__("runtime.idle");
}
function formatAtmGuard(d) {
  return !d.atmEnabled
    ? __WEBUI_TEXT__("runtime.off_2")
    : d.atmEnforced
      ? __WEBUI_TEXT__("runtime.a_to_m_2") + R.ms(d.atmRemainingMs, 0) + __WEBUI_TEXT__("runtime.s")
      : d.atmArmed
        ? __WEBUI_TEXT__("runtime.armed")
        : __WEBUI_TEXT__("runtime.idle");
}
function formatNoScaleGuard(g) {
  if (!g || !g.enabled) return __WEBUI_TEXT__("runtime.off_2");
  if (!g.armed)
    return (
      __WEBUI_TEXT__("runtime.temporarily_allowed") +
      Math.ceil(g.cooldownRemainingMs / 6e4) +
      __WEBUI_TEXT__("runtime.min_2")
    );
  return g.mode === "require_scale"
    ? g.scaleUsable
      ? __WEBUI_TEXT__("runtime.ready_2")
      : __WEBUI_TEXT__("runtime.scale_required")
    : __WEBUI_TEXT__("runtime.armed");
}
function formatAccidentalTouch(d) {
  return !d.enabled
    ? __WEBUI_TEXT__("runtime.off_2")
    : d.holding
      ? __WEBUI_TEXT__("runtime.holding")
      : d.inShot
        ? __WEBUI_TEXT__("runtime.active")
        : __WEBUI_TEXT__("runtime.idle");
}
function formatCupProtection(d) {
  return !d.enabled
    ? __WEBUI_TEXT__("runtime.off_2")
    : d.inShot
      ? d.stopIfRemoved
        ? __WEBUI_TEXT__("runtime.armed")
        : __WEBUI_TEXT__("runtime.brew_allowed")
      : d.requireCupToStart && d.scaleUsable && !d.present
        ? __WEBUI_TEXT__("runtime.can_t_brew_no_cup")
        : d.aborted
          ? __WEBUI_TEXT__("runtime.shot_aborted")
          : __WEBUI_TEXT__("runtime.brew_allowed");
}
function renderDiagClock() {
  const t = (i) => (v) => {
    const e = $(i);
    if (e) e.textContent = v || "—";
  };
  const u =
      R.statusUtcAnchorSec > 0
        ? R.statusUtcAnchorSec + Math.floor((performance.now() - R.statusUtcAnchorAt) / 1000)
        : 0,
    utc = u && formatWallTime(u, 0),
    local = u && formatWallTime(u, R.statusTimezoneOffsetMinutes);
  t("ut")(utc && utc.slice(11));
  t("ud")(utc && utc.slice(0, 10));
  t("lt")(local && local.slice(11));
  t("ld")(local && local.slice(0, 10));
  renderLineaMicraDiagnostic();
}
let micraDiagnosticStatus = null;
// Facts about this browser, not the controller. The firmware serves plain HTTP
// only, so an https: page always came through a reverse proxy, and only there
// can the service worker keep the offline copy. The companion names itself at
// a same-origin path; a direct page never asks the controller for it.
let screenCompanion = "";
function renderScreen() {
  const t = __WEBUI_TEXT__("diagnostic.screen_states").split("|"),
    sw = navigator.serviceWorker;
  $("dUiMode").textContent =
    matchMedia("(display-mode: standalone)").matches || navigator.standalone ? t[1] : t[0];
  $("dUiAccess").textContent =
    (location.protocol !== "https:"
      ? t[2]
      : screenCompanion
        ? R.sub(t[7], [screenCompanion])
        : t[3]) +
    " · " +
    location.host;
  $("dUiOffline").textContent = t[sw ? (sw.controller ? 6 : 5) : 4];
}
// The paired companion as the controller sees it.
function renderCompanionDiagnostic(c) {
  const t = (id, v) => ($(id).textContent = (c.paired && v) || "—"),
    pair = (a, b) => (a ?? "—") + " / " + (b ?? "—");
  $("dCoName").textContent = c.paired ? c.name : __WEBUI_TEXT__("diagnostic.companion_not_paired");
  t("dCoAddress", c.address);
  // The protocol is known only while linked, or when this controller refused it.
  t(
    "dCoVersion",
    c.version +
      (c.protocol > 0 ? " · " + __WEBUI_TEXT__("diagnostic.companion_protocol") + c.protocol : "") +
      (c.peerProtocol > 0
        ? " · " + R.sub(__WEBUI_TEXT__("diagnostic.companion_protocol_refused"), [c.peerProtocol])
        : ""),
  );
  t("dCoLink", c.link + (c.linkAgeS >= 0 ? " · " + formatUptime(c.linkAgeS * 1e3) : ""));
  t("dCoLinkCounts", pair(c.reconnects, c.authFailures));
  t("dCoRoute", c.route + (c.reason ? " · " + c.reason.replace(/_/g, " ") : ""));
  t(
    "dCoLm",
    (c.lm ? __WEBUI_TEXT__("runtime.on_2") : __WEBUI_TEXT__("runtime.off_2")) +
      " · " +
      __WEBUI_TEXT__("diagnostic.companion_upstream").split("|")[c.upstream ? 0 : 1],
  );
  t("dCoCounts", pair(c.routeSwitches, c.commands));
}
function renderLineaMicraDiagnostic() {
  const lm = __LINEA_MICRA__ && micraDiagnosticStatus;
  if (!lm || !$("dMicraPowerValue")) return;
  const age = lm.sampleValid ? lm.sampleAgeMs + Math.max(0, Date.now() - lm.receivedAtMs) : 0,
    stale =
      lm.freshnessPolicy !== "connection" && lm.quality === "current" && age >= lm.freshnessMs,
    power = lm.powerState,
    source = {
      api: "API",
      websocket: "WebSocket",
      api_initial: __WEBUI_TEXT__("diagnostic.api_via_websocket"),
    }[lm.powerSource],
    busy = ["queued", "authenticating", "listing", "running", "backoff"].includes(lm.phase),
    unavailable = !lm.accountConfigured
      ? __WEBUI_TEXT__("runtime.not_connected")
      : !lm.observeState
        ? __WEBUI_TEXT__("runtime.disabled")
        : "";
  $("dMicraPowerValue").textContent = power || "—";
  const brief = $("dMicraPowerBrief");
  if (brief) brief.textContent = power || "—";
  $("dMicraMode").textContent = (lm.observedMode || "—") + (source ? " (" + source + ")" : "");
  $("dMicraQuality").textContent = (stale ? "stale" : lm.quality) || "—";
  $("dMicraAge").textContent =
    lm.temperatureState +
    "/" +
    lm.temperatureError +
    " · " +
    (lm.sampleValid
      ? Math.floor(age / 1000) + __WEBUI_TEXT__("runtime.s_ago")
      : __WEBUI_TEXT__("runtime.no_sample"));
  const boiler = lm.boiler || {},
    coffee = boiler.coffee || {},
    steam = boiler.steam || {},
    boilerAge = boiler.sampleAgeMs
      ? Math.floor((boiler.sampleAgeMs + Math.max(0, Date.now() - lm.receivedAtMs)) / 1000)
      : 0,
    readinessLabel = __WEBUI_TEXT__("diagnostic.readiness_states").split("|")[
      ["off", "warming_up", "waiting_for_steam", "ready", "needs_water", "unknown"].indexOf(
        lm.readiness,
      )
    ],
    waterLabel =
      __WEBUI_TEXT__("diagnostic.water_states").split("|")[
        [coffee.state, steam.state].includes("no_water")
          ? 1
          : [coffee.state, steam.state].some((x) => x && x !== "unknown" && x !== "unsupported")
            ? 0
            : 2
      ],
    // The absolute readyAt timestamp lets the remaining time tick locally
    // without stream frames; the sample age interpolates the same way.
    boilerRow = (b, qualifier) => {
      if (unavailable) return unavailable;
      const parts = [];
      if (b.state) parts.push(b.state.replace(/_/g, " "));
      if (qualifier) parts.push(qualifier);
      const left = Math.max(0, b.readyAtUtcSec ? b.readyAtUtcSec - R.deviceUtcSec() : 0);
      if ((b.state === "heating_up" || b.state === "eco") && b.readyAtUtcSec)
        parts.push(
          __WEBUI_TEXT__("diagnostic.ready_in").replace(
            "%1",
            Math.floor(left / 60) +
              __WEBUI_TEXT__("diagnostic.minutes_short") +
              (left % 60) +
              __WEBUI_TEXT__("diagnostic.s"),
          ) +
            " (" +
            R.formatWallTimeLocal(b.readyAtUtcSec) +
            __WEBUI_TEXT__("diagnostic.utc_close"),
        );
      if (boilerAge) parts.push(boilerAge + __WEBUI_TEXT__("runtime.s_ago"));
      return parts.length ? parts.join(" · ") : "—";
    };
  $("dMicraReadiness").textContent = unavailable || readinessLabel || lm.readiness || "—";
  $("dMicraBrewBoiler").textContent = boilerRow(
    coffee,
    coffee.targetDeciC
      ? (coffee.targetDeciC / 10).toFixed(1) + __WEBUI_TEXT__("diagnostic.deg_c")
      : "",
  );
  $("dMicraSteamBoiler").textContent = boilerRow(
    steam,
    steam.level && steam.level !== "unknown" && steam.level !== "unsupported"
      ? steam.level.replace("level_", __WEBUI_TEXT__("diagnostic.level_word"))
      : "",
  );
  $("dMicraWater").textContent = unavailable || waterLabel;
  const refresh = $("lineaMicraRefreshLink");
  if (refresh) {
    const disabled = !R.controlsMutable || !lm.observeState || !lm.accountConfigured || busy;
    refresh.setAttribute("aria-disabled", String(disabled));
    refresh.tabIndex = disabled ? -1 : 0;
  }
}
// Diagnostic sections are painted only from the owned WebSocket cache.
function applyDiagnosticLive(s) {
  const t = (i, v) => {
      const e = $(i);
      if (e) e.textContent = v || "—";
    },
    sf = s.safety || {},
    sc = s.scale || {},
    cp = s.cupPresence || {};
  t("dMachine", s.machineState);
  const lm = s.lineaMicra;
  if (lm) {
    micraDiagnosticStatus = { ...lm, receivedAtMs: Date.now() };
    renderLineaMicraDiagnostic();
  }
  t("dBrew", R.formatBackflushState(s) || s.state);
  t("dCup", cp.state);
  t(
    "dScaleName",
    sc.connectedMac ? sc.connectedFriendlyName || "—" : __WEBUI_TEXT__("runtime.not_connected"),
  );
  R.updateScaleRenameUi(
    "dScaleNameRenameWrap",
    sc.connectedMac || "",
    sc.connectedFriendlyName || "",
  );
  t("dCupWeight", R.formatCupWeight(s));
  t(
    "dActivator",
    s.physicalActivatorOn ? __WEBUI_TEXT__("runtime.on") : __WEBUI_TEXT__("runtime.off"),
  );
  if ($("dReed"))
    t("dReed", s.reedOn ? __WEBUI_TEXT__("runtime.on") : __WEBUI_TEXT__("runtime.off"));
  t("dRelay", s.relayClosed ? __WEBUI_TEXT__("runtime.on") : __WEBUI_TEXT__("runtime.off"));
  t("dSource", s.controlSource);
  t(
    "dSafety",
    sf.state +
      (s.backflush
        ? " · " +
          s.backflush.hardLimitMs / 1000 +
          __WEBUI_TEXT__("diagnostic.s_max") +
          Math.ceil(s.backflush.remainingMs / 1000) +
          __WEBUI_TEXT__("diagnostic.s_left")
        : ""),
  );
  t("dFault", sf.fault !== "NONE" ? sf.fault : s.backflush?.stopReason || sf.fault);
  t(
    "dWatchdog",
    sf.taskWatchdogReady ? __WEBUI_TEXT__("runtime.ready_3") : __WEBUI_TEXT__("runtime.fault"),
  );
  t(
    "dExternal",
    sf.externalHardware
      ? __WEBUI_TEXT__("runtime.present_2")
      : __WEBUI_TEXT__("runtime.not_configured"),
  );
  t("dStream", sc.streamState);
  t("dControl", sc.controlState);
  t("dScaleRssi", typeof sc.rssi === "number" ? sc.rssi + __WEBUI_TEXT__("runtime.dbm") : "");
  t("hRecoveredStales", String(sc.recoveredStaleCount));
  t(
    "hStaleTime",
    typeof sc.recoveredStaleMs === "number"
      ? sc.recoveredStaleMs + __WEBUI_TEXT__("runtime.ms_2")
      : "",
  );
  const wi = sc.weightUpdateIntervalMs;
  t(
    "hScaleRate",
    typeof wi === "number" && wi > 0
      ? (1000 / wi).toFixed(1) + __WEBUI_TEXT__("runtime.hz") + wi + __WEBUI_TEXT__("runtime.ms_3")
      : "",
  );
  t("hScaleGaps", String(sc.packetGaps));
  t("hScaleGapMax", sc.maxPacketGapMs + __WEBUI_TEXT__("runtime.ms_2"));
  t("hScaleRejected", String(sc.rejectedPackets));
  t("hScaleReconnects", String(sc.reconnects));
  t(
    "hLastDisconnect",
    sc.lastDisconnect?.summary ||
      sc.lastDisconnectReasonName ||
      __WEBUI_TEXT__("diagnostic.none_2"),
  );
  t("hScaleCommandFailure", sc.lastCommandFailure?.summary || __WEBUI_TEXT__("diagnostic.none_3"));
  t("hEventsDropped", String(sc.eventsDropped));
  t("dScaleWeight", R.formatScaleWeight(s));
  t("dScaleTimer", R.formatScaleTimer(s));
}
let lastCmdFailShown = 0;
function applyDiagnosticStatus(s) {
  if (!s) return;
  const u = !!s.adminUnlocked;
  R.syncAdminSessionUi(u);
  // Reads stay public; actions follow the firmware gate.
  for (const e of document.querySelectorAll(".diagGate"))
    e.inert = !u && !s.development && !R.diagnosticVisible;
  const t = (i, v) => ($(i).textContent = v || "—");
  t("hFirmware", s.firmwareVersion);
  t("hBoot", typeof s.bootId === "number" && s.bootId ? "#" + s.bootId : "");
  const m = s.maintenance;
  t(
    "maintenance",
    m.active
      ? __WEBUI_TEXT__("runtime.reserved")
      : m.persistFailed
        ? __WEBUI_TEXT__("runtime.save_failed")
        : m.persistPending
          ? __WEBUI_TEXT__("runtime.saving")
          : __WEBUI_TEXT__("runtime.idle"),
  );
  t("hLease", m.active ? String(m.leaseId) : "");
  t("hLoopMax", s.health.loopMaxGapMs + __WEBUI_TEXT__("runtime.ms_2"));
  updH(s.health, s.safety);
  const nv = s.nvs || {},
    nf = nv.lastFailure;
  t(
    "hNvsLayout",
    nv.layoutExpected ? __WEBUI_TEXT__("runtime.expected") : __WEBUI_TEXT__("runtime.mismatch"),
  );
  t(
    "hNvsSize",
    typeof nv.partitionBytes === "number"
      ? Math.round(nv.partitionBytes / 1024) + __WEBUI_TEXT__("runtime.kib")
      : "",
  );
  t(
    "hNvsEntries",
    nv.statsValid && typeof nv.usedEntries === "number"
      ? nv.usedEntries + " / " + nv.availableEntries
      : "",
  );
  t(
    "hNvsTotal",
    nv.statsValid && typeof nv.freeEntries === "number"
      ? nv.freeEntries + " / " + nv.totalEntries
      : "",
  );
  t(
    "hNvsNamespaces",
    nv.statsValid && typeof nv.namespaces === "number" ? String(nv.namespaces) : "",
  );
  t("hNvsFailures", typeof nv.failures === "number" ? String(nv.failures) : "");
  t(
    "hNvsLastFailure",
    nf
      ? nf.subsystem + " · " + nf.operation + " · " + nf.errorName
      : __WEBUI_TEXT__("runtime.none_2"),
  );
  t(
    "hNvsLockTimeouts",
    typeof nv.flashIoLockTimeouts === "number" ? String(nv.flashIoLockTimeouts) : "",
  );
  t(
    "lastCommand",
    s.lastCommand && s.lastCommand.requestId
      ? String(s.lastCommand.requestId)
      : __WEBUI_TEXT__("runtime.none"),
  );
  t("lastCommandState", s.lastCommand && s.lastCommand.requestId ? s.lastCommand.state || "" : "");
  if (
    s.lastCommand &&
    (s.lastCommand.state === "FAILED" || s.lastCommand.state === "CANCELED") &&
    s.lastCommand.requestId &&
    s.lastCommand.requestId !== lastCmdFailShown
  ) {
    lastCmdFailShown = s.lastCommand.requestId;
    R.message(
      __WEBUI_TEXT__("runtime.last_command") + s.lastCommand.state.toLowerCase() + ".",
      "error",
    );
  }
  const time = s.time;
  t("ntpStatus", ntpStateLabel(time));
  t("ntpServer", time && time.activeServer);
  t("zoneName", s.config.timezoneId || __WEBUI_TEXT__("runtime.timezone_pending"));
  t("uo", formatDiagnosticTimezoneOffset(s.config));
  t(
    "lastSync",
    time?.lastSyncUtcSec &&
      s.config.lastSyncOffsetKnown &&
      formatWallTime(time.lastSyncUtcSec, s.config.lastSyncAppliedTimezoneOffsetMinutes),
  );
  renderDiagClock();
  const n = s.network;
  t("hWifiState", n.staState);
  t("hWifiPs", n.wifiPs);
  t("hWifiCoex", n.wifiCoex);
  t("hSsid", n.ssid);
  t(
    "hWifiChannel",
    n.staState === "CONNECTED" && typeof n.channel === "number" && n.channel > 0
      ? String(n.channel)
      : "",
  );
  t("hWifiIp", n.staIp);
  t("hWifiSignal", typeof n.signalQualityPct === "number" ? n.signalQualityPct + "%" : "");
  t("hWifiRssi", typeof n.rssi === "number" ? n.rssi + __WEBUI_TEXT__("runtime.dbm") : "");
  t(
    "hWifiCreds",
    n.wifiConfigured ? __WEBUI_TEXT__("runtime.saved") : __WEBUI_TEXT__("runtime.none_2"),
  );
  t(
    "hWifiIpMode",
    n.ipMode === "static" ? __WEBUI_TEXT__("runtime.static_ip") : __WEBUI_TEXT__("runtime.dhcp"),
  );
  t(
    "hWifiConfirm",
    n.configState === "PENDING"
      ? __WEBUI_TEXT__("runtime.pending") +
          (n.confirmRemainingMs
            ? " " + Math.ceil(n.confirmRemainingMs / 1000) + __WEBUI_TEXT__("runtime.s")
            : "")
      : "",
  );
  t(
    "hApState",
    n.apActive ? __WEBUI_TEXT__("runtime.active_2") : __WEBUI_TEXT__("runtime.inactive"),
  );
  t("hApSsid", n.apSsid || __WEBUI_TEXT__("runtime.ap_ssid"));
  t("hApIp", n.apIp);
  t("hApClients", String(n.apClients));
  const ser = s.serial || {};
  t(
    "dSerialIo4",
    { closed: __WEBUI_TEXT__("runtime.closed"), open: __WEBUI_TEXT__("runtime.open_2") }[ser.io4] ||
      "—",
  );
  t(
    "dSerialState",
    {
      enabled_jtag: __WEBUI_TEXT__("runtime.enabled_compile_flag"),
      enabled_io4: __WEBUI_TEXT__("runtime.enabled_io04"),
      disabled: __WEBUI_TEXT__("runtime.disabled"),
    }[ser.state] || "—",
  );
  const f = s.compileFlags || {};
  t("dBz", f.buzzer);
  t(
    "dCircuit",
    f.remoteMachineControl
      ? __WEBUI_TEXT__("runtime.enabled")
      : __WEBUI_TEXT__("runtime.disabled_2"),
  );
  t(
    "dArch",
    [
      f.arch,
      __WEBUI_TEXT__("diagnostic.hw_prefix") + f.hardwareProfile,
      __WEBUI_TEXT__("diagnostic.machine_prefix") +
        f.machineBrand +
        " " +
        f.machineModel +
        " (" +
        f.machineProfile +
        ")",
    ].join(" · "),
  );
}
let ready = false;
const flags = (o, ...keys) => keys.map((k) => k + "=" + (o[k] ? "1" : "0")).join(" ");
function fmtTaskPct(n) {
  return Number.isFinite(n) ? n.toFixed(1) + "%" : "—";
}
function applyTaskProfiler(t) {
  const T = $("hTaskState"),
    E = $("hTaskElapsed"),
    B = $("taskTableBody"),
    H = $("taskTableHint"),
    A = $("taskProfilerStartButton"),
    P = $("taskProfilerStopButton"),
    own = !!R.webUiOwner;
  if (!t || typeof t.state != "string") {
    if (T) T.textContent = "—";
    if (E) E.textContent = "—";
    if (B) fillRows(B, []);
    if (H) H.textContent = "";
    if (A) A.disabled = !own;
    if (P) P.disabled = true;
    return;
  }
  const run = t.state === "running";
  if (T) {
    let s =
      {
        never: __WEBUI_TEXT__("diagnostic.idle"),
        running: __WEBUI_TEXT__("diagnostic.running"),
        stopped: __WEBUI_TEXT__("diagnostic.stopped"),
        failed: __WEBUI_TEXT__("diagnostic.failed"),
      }[t.state] || t.state;
    if (
      t.stopReason &&
      ((t.state === "failed" && t.stopReason !== "none") ||
        (t.state === "stopped" && t.stopReason === "timeout"))
    )
      s += " (" + t.stopReason + ")";
    T.textContent = s;
  }
  if (E)
    E.textContent = ["running", "stopped", "failed"].includes(t.state)
      ? Math.floor((t.elapsedMs || 0) / 1e3) + __WEBUI_TEXT__("diagnostic.s")
      : "—";
  if (A) A.disabled = run || !own;
  if (P) P.disabled = !run || !own;
  if (B)
    fillRows(
      B,
      (t.rows || [])
        // Phase and section rows publish names before any session records
        // samples; without samples they are placeholders, not measurements.
        .filter(
          (r) =>
            r.sampleCount ||
            !(r.name?.startsWith("loopTask/") || r.name?.startsWith("scaleWorker/")),
        )
        .map((r) => [
          r.name || "—",
          [0, 1].includes(r.core) ? r.core : "—",
          fmtTaskPct(r.currentCpuPct),
          fmtTaskPct(r.averageCpuPct),
          r.stackMinWords < 4294967295 ? (r.stackMinWords ?? "—") : "—",
        ]),
    );
  if (H) {
    // Before any session the published totals are placeholders too; keep the
    // section at the empty hint instead of a "Total now 0.0%" zero line.
    const session = ["running", "stopped", "failed"].includes(t.state);
    const total = __WEBUI_TEXT__("diagnostic.total_now");
    const p = t.truncated ? [__WEBUI_TEXT__("diagnostic.list_truncated")] : [];
    for (const [label, value] of [
      [__WEBUI_TEXT__("diagnostic.other_now"), t.unreportedCurrentCpuPct],
      [__WEBUI_TEXT__("diagnostic.other_avg"), t.unreportedAverageCpuPct],
      [total, t.currentTotalCpuPct],
    ])
      if (Number.isFinite(value) && (label === total ? session : value > 0.05))
        p.push(label + " " + fmtTaskPct(value));
    if (!p.length && !session) p.push(__WEBUI_TEXT__("diagnostic.tasks_empty"));
    H.textContent = p.join(" · ");
  }
}
// The guard switches are Home's config and noScaleShotGuard paths; the stop
// weight and early target are shared by both extraction guards.
function applyDiagnosticGuards(s) {
  const g = s && s.guards;
  if (!g) return;
  const c = s.config || {},
    h = s.noScaleShotGuard || {},
    shared = { targetReachedEarly: g.targetReachedEarly, activeStopWeightG: g.activeStopWeightG },
    live = !!s.machineRunning,
    b = !!c.brewByWeight,
    ns = { ...g.noScale, enabled: h.enabled, armed: h.armed, mode: c.noScaleBbwMode },
    atm = { ...g.atm, enabled: c.autoToManualGuardEnabled },
    slow = { ...g.slowExtraction, enabled: c.slowExtractionGuardEnabled, ...shared },
    fast = { ...g.fastExtraction, enabled: c.fastExtractionGuardEnabled, ...shared },
    touch = { ...g.accidentalTouch, enabled: c.avoidAccidentalTouchEnabled },
    cup = {
      ...g.cupProtection,
      enabled: c.cupProtectionEnabled,
      stopIfRemoved: c.stopIfCupRemoved,
      requireCupToStart: c.requireCupToStart,
      present: s.cupPresence?.present,
    },
    t = (i, v) => {
      const e = $(i);
      if (e) e.textContent = v || "—";
    };
  t(
    "dGuardNoScaleUser",
    formatNoScaleGuard(b ? { ...ns, scaleUsable: !!g.scaleUsable } : { enabled: false }),
  );
  t("dGuardNoScaleRaw", flags(ns, "enabled", "armed", "hold", "scaleWasAvailable"));
  t(
    "dGuardAtmUser",
    formatAtmGuard({
      atmEnabled: b && !!atm.enabled,
      atmEnforced: live && !!atm.enforced,
      atmArmed: live && !!atm.armed,
      atmRemainingMs: atm.remainingMs,
    }),
  );
  t(
    "dGuardAtmRaw",
    flags(atm, "enabled", "armed", "enforced") +
      __WEBUI_TEXT__("diagnostic.remainingms") +
      (atm.remainingMs || 0),
  );
  t(
    "dGuardSlowUser",
    formatSlowExtractionGuard({
      guardEnabled: b && !!slow.enabled,
      extended: live && !!slow.extended,
      goal: slow.activeStopWeightG,
      inShot: live,
    }),
  );
  t(
    "dGuardSlowRaw",
    flags(slow, "enabled", "extended", "targetReachedEarly") +
      __WEBUI_TEXT__("diagnostic.activestopweightg") +
      (typeof slow.activeStopWeightG === "number" ? slow.activeStopWeightG.toFixed(1) : "0.0"),
  );
  t(
    "dGuardFastUser",
    formatExtractionGuard({
      guardEnabled: b && !!fast.enabled,
      extended: live && !!fast.extended,
      goal: fast.activeStopWeightG,
      minBbwBrewRemainingMs: fast.minBbwBrewTimeRemainingMs,
      inShot: live,
    }),
  );
  t(
    "dGuardFastRaw",
    flags(fast, "enabled", "extended", "targetReachedEarly") +
      __WEBUI_TEXT__("diagnostic.activestopweightg") +
      (typeof fast.activeStopWeightG === "number" ? fast.activeStopWeightG.toFixed(1) : "0.0") +
      __WEBUI_TEXT__("diagnostic.minbbwbrewtimeremainingms") +
      (fast.minBbwBrewTimeRemainingMs || 0),
  );
  t(
    "dGuardTouchUser",
    formatAccidentalTouch({
      enabled: b && !!touch.enabled,
      holding: live && !!touch.holding,
      inShot: live,
    }),
  );
  t(
    "dGuardTouchRaw",
    flags(touch, "enabled", "holding") +
      __WEBUI_TEXT__("diagnostic.phase") +
      (touch.phase || __WEBUI_TEXT__("diagnostic.startup")) +
      __WEBUI_TEXT__("diagnostic.class") +
      (touch.class || __WEBUI_TEXT__("diagnostic.ok")) +
      __WEBUI_TEXT__("diagnostic.pendingcount") +
      (touch.pendingCount || 0),
  );
  t(
    "dGuardCupUser",
    formatCupProtection({
      enabled: b && !!cup.enabled,
      requireCupToStart: !!cup.requireCupToStart,
      stopIfRemoved: !!cup.stopIfRemoved,
      present: !!cup.present,
      scaleUsable: !!g.scaleUsable,
      inShot: live,
      aborted: !live && !!g.lastShotCupRemoved,
    }),
  );
  t(
    "dGuardCupRaw",
    flags(
      cup,
      "enabled",
      "stopIfRemoved",
      "requireCupToStart",
      "present",
      "startHold",
      "removedPending",
      "bbwProtectionActive",
      "bbwProtectionEnded",
    ),
  );
}
async function exportDebugData() {
  const btn = $("exportDebugDataButton");
  btn.disabled = true;
  R.message(__WEBUI_TEXT__("diagnostic.exporting"));
  try {
    const data = await R.api("/api/v1/debug/export", { timeoutMs: 3e4 });
    if (!data || typeof data.exportSchemaVersion !== "number")
      throw new Error(__WEBUI_TEXT__("diagnostic.invalid_export"));
    saveFile(
      new Blob([JSON.stringify(data, null, 2)], { type: "application/json" }),
      "debug-boot" +
        (typeof data.bootId === "number" ? data.bootId : R.bootId || 0) +
        "-" +
        new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19) +
        ".json",
    );
    R.message(__WEBUI_TEXT__("diagnostic.ok"), "ok");
    R.noteReachOk();
  } catch (e) {
    R.message(R.formatCommandError(__WEBUI_TEXT__("diagnostic.export_failed"), e), "error");
    R.noteReachFail(e);
  } finally {
    btn.disabled = false;
  }
}
function copyText(text) {
  // execCommand copies from a user gesture in every browser, including the
  // plain-HTTP device UI where the async clipboard API is unavailable.
  const ta = document.createElement("textarea");
  ta.value = text;
  ta.style.cssText = "position:fixed;opacity:0";
  document.body.append(ta);
  ta.select();
  const ok = document.execCommand("copy");
  ta.remove();
  if (ok) R.message(__WEBUI_TEXT__("diagnostic.copied"), "ok");
}
// Cleared data lands with a later device frame: hide it until it changes.
const seen = {},
  stale = {};
function shown(name, value, empty) {
  return (seen[name] = JSON.stringify(value)) === stale[name] ? empty : value;
}
async function hideUntilChanged(name, run) {
  stale[name] = seen[name];
  R.refreshStatus();
  (await run()) || delete stale[name];
}
let heapMapBusy = false;
// Internal-heap block maps of the startup stages plus a fresh view, as CSV.
async function downloadHeapMap() {
  if (heapMapBusy) return;
  heapMapBusy = true;
  await downloadText(
    R.apiBinary("/api/v1/diagnostic/heap-map"),
    "heap-map.csv",
    __WEBUI_TEXT__("diagnostic.heap_map_download_failed"),
  );
  heapMapBusy = false;
}
let auditBusy = false,
  auditHuntOn = false,
  auditItems = [],
  auditPage = 0,
  auditRenderedKey = "";
const AUDIT_PAGE_SIZE = 5;
function auditTime(wallSec) {
  if (!wallSec) return "—";
  const d = new Date(wallSec * 1000),
    now = new Date(),
    sameDay = d.toDateString() === now.toDateString();
  return sameDay ? d.toLocaleTimeString() : d.toLocaleDateString() + " " + d.toLocaleTimeString();
}
function applyAudit(a) {
  const body = $("auditBody");
  if (!body || !a) return;
  const items = shown("audit", a.items || [], []),
    huntUntil = a.verboseUntilUtcSec || 0,
    huntLeft = huntUntil ? huntUntil - Math.floor(Date.now() / 1000) : 0,
    huntOn = (auditHuntOn = huntLeft > 0);
  $("auditSummary").textContent =
    (a.state === "unsupported"
      ? __WEBUI_TEXT__("diagnostic.audit_ram_only")
      : (a.retained || 0) + " " + __WEBUI_TEXT__("diagnostic.audit_kept")) +
    (a.dropped || a.rateLimited
      ? " · " +
        __WEBUI_TEXT__("diagnostic.audit_dropped").replace("{d}", a.dropped || 0) +
        " / " +
        (a.rateLimited || 0)
      : "") +
    (huntOn
      ? " · " +
        __WEBUI_TEXT__("diagnostic.audit_hunt_left").replace(
          "{m}",
          String(Math.ceil(huntLeft / 60)),
        )
      : "");
  const verbose = $("auditVerboseButton"),
    clear = $("auditClearButton"),
    exportLog = $("auditExportButton");
  if (verbose)
    verbose.textContent = huntOn
      ? __WEBUI_TEXT__("diagnostic.audit_hunt_stop")
      : __WEBUI_TEXT__("diagnostic.audit_hunt_30m");
  if (verbose) verbose.disabled = auditBusy || !R.webUiOwner;
  for (const b of [clear, exportLog])
    if (b) b.disabled = auditBusy || !R.webUiOwner || !items.length;
  // Rebuilding identical rows on every status paint would destroy text
  // selection and any open detail row, so repaint only on real changes.
  const key = JSON.stringify(items);
  if (key === auditRenderedKey) return;
  auditRenderedKey = key;
  auditItems = items.slice().reverse();
  auditPage = Math.min(auditPage, Math.max(0, Math.ceil(auditItems.length / AUDIT_PAGE_SIZE) - 1));
  renderAuditPage();
}
function renderAuditPage() {
  const body = $("auditBody");
  if (!body) return;
  body.replaceChildren();
  const start = auditPage * AUDIT_PAGE_SIZE,
    page = auditItems.slice(start, start + AUDIT_PAGE_SIZE);
  for (const item of page) {
    const row = body.insertRow();
    row.className = "auditRow";
    row.tabIndex = 0;
    row.title = __WEBUI_TEXT__("diagnostic.audit_view");
    const toggle = () => toggleAuditDetail(row, item);
    row.onclick = toggle;
    row.onkeydown = (e) => {
      if (e.key === "Enter" || e.key === " ") {
        e.preventDefault();
        toggle();
      }
    };
    row.append(
      ...[auditTime(item.wallSec), item.level, item.code, item.text || "—"].map((text) =>
        Object.assign(document.createElement("td"), { textContent: text }),
      ),
    );
  }
  const pages = Math.max(1, Math.ceil(auditItems.length / AUDIT_PAGE_SIZE)),
    pager = $("auditPager");
  if (!pager) return;
  pager.classList.toggle("hidden", auditItems.length <= AUDIT_PAGE_SIZE);
  $("auditPrevButton").disabled = auditPage === 0;
  $("auditNextButton").disabled = auditPage >= pages - 1;
  $("auditPageLabel").textContent = __WEBUI_TEXT__("diagnostic.audit_page")
    .replace("{a}", String(auditItems.length ? start + 1 : 0))
    .replace("{b}", String(start + page.length))
    .replace("{c}", String(auditItems.length));
}
function toggleAuditDetail(row, item) {
  const open = row.nextElementSibling;
  if (open && open.classList.contains("auditDetailRow")) {
    open.remove();
    return;
  }
  const detail = document.createElement("tr");
  detail.className = "auditDetailRow";
  const cell = detail.insertCell();
  cell.colSpan = 4;
  const when = item.wallSec
    ? new Date(item.wallSec * 1000).toLocaleString()
    : __WEBUI_TEXT__("diagnostic.audit_no_clock");
  for (const text of [
    __WEBUI_TEXT__("diagnostic.audit_when") + ": " + when,
    __WEBUI_TEXT__("diagnostic.audit_level") + ": " + item.level + " · " + item.category,
    __WEBUI_TEXT__("diagnostic.audit_event") + ": " + item.code,
    __WEBUI_TEXT__("diagnostic.audit_detail") + ": " + (item.text || "—"),
  ]) {
    const line = document.createElement("div");
    line.textContent = text;
    cell.append(line);
  }
  row.after(detail);
}
const saveFile = (blob, name) => R.saveFile(blob, "openbrewbyweight-" + name);
// Shot storage: read when its section opens, never polled (the firmware
// walks every stored record for it), and refused while a shot runs.
let shotStorageBusy = false;
// Opened during a shot (or its drip), the section reads itself once it ends;
// the wait looks at Home's state only, no request.
function loadShotStorageAfterShot() {
  if (R.shotRunning()) setTimeout(loadShotStorageAfterShot, 1000);
  else loadShotStorage();
}
async function loadShotStorage() {
  if (shotStorageBusy || !$("shotStorage").open) return;
  shotStorageBusy = true;
  try {
    const s = await R.api("/api/v1/diagnostic/shot-storage", { timeoutMs: 15e3 });
    renderShotStorage(s);
    R.setEmptyState(
      "shotStorageState",
      s.ready ? null : __WEBUI_TEXT__("diagnostic.shot_storage_not_ready"),
    );
  } catch (e) {
    renderShotStorage(null);
    if (e.code === "SHOT_STORAGE_BUSY" && R.shotRunning()) loadShotStorageAfterShot();
    R.setEmptyState(
      "shotStorageState",
      e.code !== "SHOT_STORAGE_BUSY"
        ? R.formatCommandError(__WEBUI_TEXT__("diagnostic.shot_storage_failed"), e)
        : R.shotRunning()
          ? __WEBUI_TEXT__("diagnostic.shot_storage_after_shot")
          : __WEBUI_TEXT__("runtime.controller_busy"),
    );
  } finally {
    shotStorageBusy = false;
  }
}
// Without an answer (null) every figure goes back to "—".
function renderShotStorage(s) {
  if (!s) {
    for (const e of $("shotStorage").querySelectorAll(".metric > div")) e.textContent = "—";
    $("ssOrphansRow").hidden = true;
    return;
  }
  const size = (n) =>
      n >= 1048576 ? (n / 1048576).toFixed(1) + " MB" : (n / 1024).toFixed(1) + " KB",
    none = __WEBUI_TEXT__("diagnostic.none"),
    t = (id, text, values) => ($(id).textContent = values ? R.sub(text, values) : text);
  t("ssSpace", __WEBUI_TEXT__("diagnostic.shot_storage_space_value"), [
    size(s.liveBytes),
    size(s.poolBytes),
  ]);
  t("ssShots", __WEBUI_TEXT__("diagnostic.shot_storage_shots_value"), [s.good, s.bad]);
  t(
    "ssSince",
    s.sinceLocalSec
      ? R.formatHumanTime(s.sinceLocalSec)
      : s.sinceShotId
        ? "#" + s.sinceShotId
        : none,
  );
  // The history keeps at most shotCapacity shots, however many would fit.
  const fit = s.meanRecordBytes && Math.floor(s.poolBytes / s.meanRecordBytes);
  t(
    "ssRoom",
    !fit
      ? "—"
      : fit < s.shotCapacity
        ? __WEBUI_TEXT__("diagnostic.shot_storage_room_value")
        : __WEBUI_TEXT__("diagnostic.shot_storage_room_all"),
    fit && [size(s.meanRecordBytes), Math.min(fit, s.shotCapacity)],
  );
  t("ssTraces", __WEBUI_TEXT__("diagnostic.shot_storage_traces_value"), [s.traces, s.truncated]);
  t("ssOther", __WEBUI_TEXT__("diagnostic.shot_storage_other_value"), [s.traceOnly, s.interrupted]);
  t("ssReclaimable", size(s.reclaimableBytes));
  t(
    "ssOverhead",
    s.droppedRecords
      ? __WEBUI_TEXT__("diagnostic.shot_storage_overhead_value")
      : size(s.unindexedBytes),
    s.droppedRecords && [size(s.unindexedBytes), s.droppedRecords],
  );
  t("ssUnused", size(s.unusedBytes));
  t(
    "ssWaiting",
    s.pendingCurves || s.pendingTraces
      ? __WEBUI_TEXT__("diagnostic.shot_storage_waiting_value")
      : none,
    (s.pendingCurves || s.pendingTraces) && [s.pendingCurves, s.pendingTraces],
  );
  t("ssNotSaved", s.tracesDropped ? String(s.tracesDropped) : none);
  t("ssError", s.lastError === "none" ? none : s.lastError);
  // Traces without a shot, newest first; the firmware lists at most 16.
  const more = s.traceOnly + s.interrupted - s.orphanTraces.length;
  $("ssOrphansRow").hidden = !s.orphanTraces.length;
  $("ssOrphans").replaceChildren(
    ...s.orphanTraces.map(({ boot, end }) => {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "inlineAction";
      b.textContent = R.sub(
        end
          ? __WEBUI_TEXT__("diagnostic.shot_storage_orphan_refused")
          : __WEBUI_TEXT__("diagnostic.shot_storage_orphan_interrupted"),
        [boot],
      );
      b.onclick = () => downloadOrphanTrace(boot, end);
      return b;
    }),
    ...(more > 0 ? [R.sub(__WEBUI_TEXT__("diagnostic.shot_storage_orphans_more"), [more])] : []),
  );
}
// A trace without a shot is reached by its boot and end time: 0 when a
// restart interrupted it, the shot's end when the history refused the shot.
function downloadOrphanTrace(boot, end) {
  R.fetchShotTrace({ id: 0, bootId: boot, endedAtMs: end }).then(
    (data) =>
      data
        ? saveFile(new Blob([data]), `shot-trace-boot-${boot}${end ? "-" + end : ""}.bin`)
        : R.message(__WEBUI_TEXT__("stats.trace_none")),
    (e) =>
      R.message(
        R.formatCommandError(__WEBUI_TEXT__("diagnostic.shot_storage_download_failed"), e),
        "error",
      ),
  );
}
async function downloadText(load, name, failMsg) {
  R.message(__WEBUI_TEXT__("diagnostic.downloading"));
  try {
    saveFile(new Blob([await load], { type: "text/plain" }), name);
    R.message(__WEBUI_TEXT__("diagnostic.ok"), "ok");
  } catch (e) {
    R.message(R.formatCommandError(failMsg, e), "error");
  }
}
async function exportAudit() {
  if (auditBusy || !auditItems.length) return;
  auditBusy = true;
  await downloadText(
    R.apiBinary("/api/v1/diagnostic/audit"),
    "audit.txt",
    __WEBUI_TEXT__("diagnostic.audit_export_failed"),
  );
  auditBusy = false;
}
async function toggleAuditHunt() {
  if (auditBusy) return;
  const huntOn = auditHuntOn;
  auditBusy = true;
  try {
    await R.api("/api/v1/diagnostic/audit/verbose", {
      method: "POST",
      body: JSON.stringify({ enabled: !huntOn }),
    });
    R.message(
      huntOn
        ? __WEBUI_TEXT__("diagnostic.audit_hunt_stopped")
        : __WEBUI_TEXT__("diagnostic.audit_hunt_started"),
      "ok",
    );
  } catch (e) {
    R.message(R.formatCommandError(__WEBUI_TEXT__("diagnostic.audit_hunt_failed"), e), "error");
  } finally {
    auditBusy = false;
    await R.refreshStatus();
  }
}
async function emptyAudit() {
  if (!confirm(__WEBUI_TEXT__("diagnostic.audit_empty_confirm"))) return;
  auditBusy = true;
  await hideUntilChanged("audit", () =>
    R.api("/api/v1/diagnostic/audit/empty", {
      method: "POST",
      body: JSON.stringify({ confirm: "EMPTY_AUDIT_LOG" }),
      timeoutMs: 12e4,
    }).then(
      () => (R.message(__WEBUI_TEXT__("diagnostic.audit_cleared"), "ok"), true),
      (e) =>
        R.message(
          R.formatCommandError(__WEBUI_TEXT__("diagnostic.audit_clear_failed"), e),
          "error",
        ),
    ),
  );
  auditBusy = false;
  await R.refreshStatus();
}
const LOG_LEVELS = ["serialLogLevel", "ringRetainLogLevel"];
let logLevels = null;
// A level being saved keeps the user's pick until its command settles, then
// shows the device's value, which a failed save leaves unchanged.
function saveLogLevel(id) {
  const el = $(id);
  el.dataset.pending = 1;
  R.command("/api/v1/config", R.withBaseRev({ [id]: el.value || "none" })).finally(() => {
    delete el.dataset.pending;
    el.value = logLevels?.[id] || "none";
  });
}
// The serial log rides the owned socket (runtime.js registerStream): subscribe
// with the rendered tail's cursor; watermark-gated frames push new events
// without REST polling.
const LOG_EVENTS_CAPACITY = 500;
let logStreamWanted = false,
  logBootId = 0,
  lastLog = 0;
function startLogStream() {
  if (R.webUiPollingActive()) {
    if (!logStreamWanted) {
      logStreamWanted = true;
      R.sendUiOperation({ op: "log", on: true, after: lastLog });
    }
    return true;
  }
  return false;
}
function stopLogStream() {
  if (logStreamWanted) R.sendUiOperation({ op: "log", on: false });
  logStreamWanted = false;
}
function logStreamFrame(m) {
  if (
    m.v !== 1 ||
    !Number.isInteger(m.bootId) ||
    !Array.isArray(m.events) ||
    typeof m.hasMore !== "boolean" ||
    typeof m.cursorInvalid !== "boolean" ||
    ["historyOverwritten", "missedEvents", "serialDropped"].some((k) => !Number.isInteger(m[k]))
  )
    throw Error();
  for (const e of m.events)
    if (
      !e ||
      !Number.isInteger(e.sequence) ||
      !Number.isInteger(e.atMs) ||
      !Number.isInteger(e.wallSec) ||
      !Number.isInteger(e.localSec) ||
      typeof e.level !== "string" ||
      typeof e.category !== "string" ||
      typeof e.message !== "string"
    )
      throw Error();
  return m;
}
function applyLogFrame(m) {
  const b = +m.bootId;
  if ((logBootId && b && b !== logBootId) || m.cursorInvalid) {
    R.uiState.logEvents = [];
    lastLog = R.uiState.logMissed = 0;
  }
  logBootId = b;
  R.noteBootId(b);
  R.uiState.logMissed += m.missedEvents || 0;
  for (const e of m.events) {
    if (lastLog && ((e.sequence - lastLog) | 0) <= 0) continue;
    R.uiState.logEvents.push(e);
    lastLog = e.sequence;
  }
  if (R.uiState.logEvents.length > LOG_EVENTS_CAPACITY)
    R.uiState.logEvents.splice(0, R.uiState.logEvents.length - LOG_EVENTS_CAPACITY);
  updateLogHealth(m);
  R.paint(renderLog);
  R.updateFirmwareFooter();
  if (R.activeView === "diagnostic") R.noteReachOk();
}
function renderLog() {
  const f = $("logFilter").value,
    m = +$("logLevelFilter").value,
    r = { critical: 0, error: 1, warning: 2, info: 3, debug: 4 };
  let o = "";
  for (const e of R.uiState.logEvents) {
    if (f && e.category !== f) continue;
    if ((r[e.level] ?? 3) > m) continue;
    o += e.line ??=
      (e.wallSec ? R.formatWallTimeLocal(e.localSec) : "+" + (e.atMs / 1000).toFixed(3) + "s") +
      " " +
      e.level.toUpperCase() +
      " [" +
      e.category +
      "] " +
      e.message +
      "\n";
  }
  // New lines append (and the oldest drop) in place, so a selection or a
  // scrolled-back view survives; only a reader already at the end follows it.
  const el = $("log"),
    old = el.value,
    cut = old.indexOf(o.slice(0, o.indexOf("\n") + 1)),
    top = el.scrollTop,
    h = el.scrollHeight,
    stick = top + el.clientHeight >= h - 8;
  if (o === old) return;
  if (cut >= 0 && o.startsWith(old.slice(cut))) {
    el.setRangeText("", 0, cut);
    if (!stick) el.scrollTop = top - h + el.scrollHeight;
    el.setRangeText(o.slice(old.length - cut), el.value.length, el.value.length);
  } else el.value = o;
  if (stick) el.scrollTop = el.scrollHeight;
}
function updateLogHealth(d) {
  const a = [],
    e = $("logDropped");
  if (d.historyOverwritten)
    a.push(__WEBUI_TEXT__("runtime.history_rotated") + d.historyOverwritten);
  if (R.uiState.logMissed)
    a.push(__WEBUI_TEXT__("runtime.missed_while_disconnected") + R.uiState.logMissed);
  if (d.serialDropped) a.push(__WEBUI_TEXT__("runtime.serial_dropped") + d.serialDropped);
  if (!e) return;
  e.hidden = !a.length;
  e.textContent = a.join(" · ");
}
const logStream = {
  // Log frames carry their own cursor: a pending resync must not drop them.
  cursor: true,
  subscribe: () => (logStreamWanted ? { op: "log", on: true, after: lastLog } : null),
  frame(data) {
    if (logStreamWanted) applyLogFrame(logStreamFrame(data));
  },
  // A rebooted device restarts the log from its first event.
  reboot(boot) {
    if (!logStreamWanted || !logBootId || logBootId === boot) return;
    logBootId = boot;
    R.uiState.logEvents = [];
    lastLog = R.uiState.logMissed = 0;
    R.sendUiOperation({ op: "log", on: true, after: 0 });
  },
};
export function applyStatus(s) {
  applyDiagnosticStatus(s);
  logLevels = s?.config;
  for (const id of LOG_LEVELS)
    if ($(id) && !$(id).dataset.pending) $(id).value = logLevels?.[id] || "none";
  if (!s) return;
  if (__LINEA_MICRA__ && s.lineaMicra)
    renderMicraCloudDiagnostic(s.lineaMicra, s.companion?.route === "companion");
  if (__LINEA_MICRA__ && s.companion) renderCompanionDiagnostic(s.companion);
  const sc = s.scale || {};
  applyScaleCommands(sc);
  applyDiagnosticGuards(s);
  applyTaskProfiler(s.tasks);
  const p = s.scaleProfile;
  applyScaleProfile(shown("scaleProfile", p, p && { ...p, persistence: "invalidating" }));
  applyLoopTiming(s);
  applyAudit(s.audit);
  updateCrashRow({ ...s, crashCount: shown("crashes", s.crashCount, 0) });
  const b = $("hResetHistory");
  if (b)
    fillRows(
      b,
      shown("resetHistory", s.resetHistory || [], []).map((e) => [
        e.reason,
        formatUptime(e.uptimeMs),
      ]),
    );
  // A resume whose first snapshot failed never reached the route's log
  // subscription: the recovered stream restarts it (a no-op once wanted).
  startLogStream();
}
// The route starts the log with the view and stops it on every navigation.
export const start = startLogStream,
  stop = stopLogStream;
export function init() {
  if (ready) return;
  ready = true;
  R.registerViewStatus("diagnostic", applyStatus);
  R.registerViewStatus("diagnosticLive", applyDiagnosticLive);
  R.registerStream("log", logStream);
  setInterval(() => {
    if (!document.hidden && R.activeView === "diagnostic") renderDiagClock();
  }, 1000);
  $("diagnosticControls")?.classList.remove("hidden");
  R.initGroupRows($("diagnosticsPanel"), "dg-");
  renderScreen();
  navigator.serviceWorker?.addEventListener("controllerchange", renderScreen);
  if (location.protocol === "https:")
    fetch("/__companion/status", { cache: "no-store" })
      .then((r) => (r.ok ? r.json() : null))
      .then((j) => {
        screenCompanion = typeof j?.name === "string" ? j.name : "";
        renderScreen();
      })
      .catch(() => {});
  for (const section of $("diagnosticsPanel").querySelectorAll(".cfgGroup")) {
    const title = section.firstElementChild;
    // Press-and-hold copy: cancel on release, pointer exit, or a browser
    // gesture takeover (touch scroll), so nothing copies accidentally; a
    // completed hold does not also toggle the row.
    let hold = 0,
      copied = false;
    const cancel = () => clearTimeout(hold);
    title.onpointerdown = (e) => {
      cancel();
      copied = false;
      if (e.button === 0)
        hold = setTimeout(() => {
          copied = true;
          copyText(section.innerText.trim());
        }, 600);
    };
    title.onpointerup = title.onpointerleave = title.onpointercancel = cancel;
    title.onclick = (e) => void (copied && e.preventDefault());
  }
  for (const id of LOG_LEVELS) $(id).onchange = () => saveLogLevel(id);
  $("logFilter").onchange = renderLog;
  $("logLevelFilter").onchange = renderLog;
  $("copyLogButton").onclick = () => copyText($("log").value);
  $("clearLogButton").onclick = () => clearLogView();
  $("auditVerboseButton").onclick = () => toggleAuditHunt();
  $("auditClearButton").onclick = () => emptyAudit();
  $("auditExportButton").onclick = () => exportAudit();
  for (const [id, step] of [
    ["auditPrevButton", -1],
    ["auditNextButton", 1],
  ])
    $(id).onclick = () => {
      const pages = Math.max(1, Math.ceil(auditItems.length / AUDIT_PAGE_SIZE));
      auditPage = Math.min(pages - 1, Math.max(0, auditPage + step));
      renderAuditPage();
    };
  $("exportDebugDataButton").onclick = () => exportDebugData();
  if ($("lineaMicraRefreshLink"))
    $("lineaMicraRefreshLink").onclick = (e) => {
      e.preventDefault();
      R.lineaMicraAction("refresh");
    };
  const dScaleNameRename = $("dScaleNameRename");
  if (dScaleNameRename)
    dScaleNameRename.onclick = (e) => {
      e.preventDefault();
      if (dScaleNameRename.getAttribute("aria-disabled") === "true") return;
      R.renameScale();
    };
  $("clearResetHistoryButton").onclick = () =>
    confirm(__WEBUI_TEXT__("diagnostic.clear_resets")) &&
    hideUntilChanged("resetHistory", () =>
      R.command("/api/v1/diagnostic/reset-history", { confirm: "CLEAR_RESET_HISTORY" }),
    );
  for (const [id, path, field] of [
    ["taskProfiler", "profiler", "enabled"],
    ["scaleProfile", "scale-profile", "action"],
  ])
    for (const [suffix, on] of [
      ["Start", true],
      ["Stop", false],
    ])
      $(id + suffix + "Button").onclick = () =>
        R.command("/api/v1/diagnostic/" + path, {
          [field]: field === "enabled" ? on : on ? "start" : "stop",
        });
  $("scaleProfileDeleteButton").onclick = () =>
    confirm(__WEBUI_TEXT__("diagnostic.delete_profile_confirm")) &&
    hideUntilChanged("scaleProfile", () =>
      R.command("/api/v1/diagnostic/scale-profile", { action: "delete" }),
    );
  $("scaleProfileDownloadButton").onclick = () => downloadScaleProfile();
  $("heapMapDownload").onclick = () => (downloadHeapMap(), false);
  $("shotStorage").addEventListener("toggle", loadShotStorage);
  for (const [id, path] of [
    ["loopMax", "loop-max"],
    ["scaleGapMax", "scale-gap-max"],
  ])
    $(id + "ResetButton").onclick = () => R.command("/api/v1/diagnostic/" + path + "/reset", {});
}

let crashBusy = false;
function applyLoopTiming(s) {
  const tasks = s.tasks || {},
    rows = (tasks.rows || []).filter((r) => r.name?.startsWith("loopTask/"));
  const out = [];
  const ms = (us) => (Math.max(0, us) / 1000).toFixed(2) + " ms";
  const add = (values) => out.push(values);
  const recentReady = rows.length && Number.isFinite(tasks.recentGapMs);
  const peakReady = tasks.peakGapMs === (s.health.loopMaxGapMs || 0);
  $("hLoopGap").textContent = recentReady ? tasks.recentGapMs + " ms" : "—";
  let recentTotal = 0,
    peakTotal = 0;
  for (const r of rows) {
    const a = r.recentGapExecutionUs,
      b = r.peakGapExecutionUs;
    recentTotal += a;
    peakTotal += b;
    add([r.name.slice(9), recentReady ? ms(a) : "—", peakReady ? ms(b) : "—"]);
  }
  const tail = (k, total) => {
    const d = tasks[k + "DelayUs"],
      q = tasks[k + "DispatchUs"];
    return [d, q, tasks[k + "GapUs"] - total - d - q];
  };
  const a = tail("recent", recentTotal),
    b = tail("peak", peakTotal);
  [
    __WEBUI_TEXT__("diagnostic.delay_call"),
    __WEBUI_TEXT__("diagnostic.loop_dispatch"),
    __WEBUI_TEXT__("diagnostic.other_timing"),
  ].forEach((label, i) => add([label, recentReady ? ms(a[i]) : "—", peakReady ? ms(b[i]) : "—"]));
  fillRows($("loopTimingBody"), out);
}
let profileBusy = false;
function applyScaleProfile(p) {
  const set = (key, value) => {
      $("hProfile" + key).textContent = value;
    },
    valid = p && p.partitionAvailable;
  const error =
    __WEBUI_TEXT__("diagnostic.profile_errors").split("|")[
      [
        "no_partition",
        "allocation",
        "busy",
        "invalidate_failed",
        "save_failed",
        "index_failed",
      ].indexOf(p?.lastError)
    ] || "";
  ["Start", "Stop", "Delete", "Download"].forEach((key) => {
    $("scaleProfile" + key + "Button").disabled = !valid || !p["can" + key] || !R.webUiOwner;
  });
  if (!valid) {
    set("State", error || __WEBUI_TEXT__("diagnostic.profile_unavailable"));
    ["Elapsed", "Records", "Capacity", "Remaining"].forEach((key) => set(key, "—"));
    return;
  }
  const SN = __WEBUI_TEXT__("diagnostic.profile_state_names").split("|"),
    VN = __WEBUI_TEXT__("diagnostic.profile_saved_names").split("|"),
    si = ["empty", "preparing", "recording", "stopped", "saved"].indexOf(p.state),
    vi = ["none", "pending", "saving", "saved", "invalidating", "failed"].indexOf(p.persistence);
  set(
    "State",
    (SN[si] || p.state) +
      (p.stopReason && p.stopReason !== "none" ? " (" + p.stopReason + ")" : "") +
      (VN[vi] ? " · " + VN[vi] : "") +
      (error ? " · " + error : ""),
  );
  set("Elapsed", formatUptime(p.elapsedMs));
  set(
    "Capacity",
    p.recordCapacity
      ? Math.floor((100 * (p.recordCount + (p.reservedRecords || 0))) / p.recordCapacity) + "%"
      : "—",
  );
  const eta = p.estimatedRemainingMs;
  set(
    "Remaining",
    p.state === "recording"
      ? eta == null
        ? __WEBUI_TEXT__("diagnostic.profile_estimating")
        : "≈ " + formatUptime(Math.ceil(eta / 5e3) * 5e3)
      : "—",
  );
  set(
    "Records",
    p.recordCount +
      " · " +
      p.weightCount +
      __WEBUI_TEXT__("diagnostic.weights_short") +
      p.eventCount +
      __WEBUI_TEXT__("diagnostic.events_short") +
      (p.lostCount > 0
        ? " · " + __WEBUI_TEXT__("diagnostic.profile_incomplete").replace("{n}", p.lostCount)
        : ""),
  );
}
async function downloadScaleProfile() {
  if (profileBusy) return;
  profileBusy = true;
  await downloadText(
    R.apiBinary("/api/v1/diagnostic/scale-profile/download"),
    "scale-profile.txt",
    __WEBUI_TEXT__("diagnostic.profile_download_failed"),
  );
  profileBusy = false;
}
function updateCrashRow(s) {
  const misc = $("hResetHistory")?.closest(".statusColumn");
  if (!misc) return;
  let row = $("crashArchiveRow");
  if (!row) {
    row = document.createElement("div");
    row.id = "crashArchiveRow";
    row.className = "metric";
    const label = document.createElement("strong");
    label.textContent = __WEBUI_TEXT__("diagnostic.coredump");
    row.append(label);
    const value = document.createElement("div");
    value.id = "crashArchiveValue";
    value.setAttribute("aria-live", "polite");
    row.append(value);
    misc.append(row);
  }
  const value = $("crashArchiveValue");
  const count = s.crashCount || 0,
    supported = s.crashState !== 1,
    usable =
      !!R.webUiOwner &&
      (!!s.adminUnlocked || !!s.development) &&
      supported &&
      count > 0 &&
      !crashBusy;
  if (unchanged(value, [count, supported, usable, s.crashState > 1])) return;
  value.replaceChildren();
  const n = document.createElement("span");
  n.textContent = supported ? count : __WEBUI_TEXT__("diagnostic.unavailable");
  if (supported && count > 0) n.className = "stateFault";
  value.append(n);
  if (usable)
    for (const [label, action] of [
      [__WEBUI_TEXT__("diagnostic.download"), downloadCrashes],
      [__WEBUI_TEXT__("diagnostic.empty"), emptyCrashes],
    ]) {
      value.append(" - ");
      const l = document.createElement("a");
      l.href = "#";
      l.textContent = label;
      l.onclick = () => {
        if (!crashBusy) action();
        return false;
      };
      value.append(l);
    }
  if (s.crashState > 1)
    value.append(
      document.createTextNode(" (" + __WEBUI_TEXT__("diagnostic.crash_capture_error") + ")"),
    );
}
async function downloadCrashes() {
  crashBusy = true;
  await R.refreshStatus();
  R.message(__WEBUI_TEXT__("diagnostic.downloading"));
  try {
    let last = 0;
    const tar = await R.apiBinary("/api/v1/diagnostic/crashes", (bytes) => {
        if (bytes - last >= 65536) {
          last = bytes;
          R.message(
            __WEBUI_TEXT__("diagnostic.downloading") +
              " " +
              Math.floor(bytes / 1024) +
              __WEBUI_TEXT__("runtime.kib"),
          );
        }
      }),
      gzip = typeof CompressionStream === "function";
    if (gzip) R.message(__WEBUI_TEXT__("diagnostic.compressing"));
    const blob = gzip
      ? await new Response(tar.stream().pipeThrough(new CompressionStream("gzip"))).blob()
      : tar;
    saveFile(blob, "crashes.tar" + (gzip ? ".gz" : ""));
    R.message(__WEBUI_TEXT__("diagnostic.ok"), "ok");
  } catch (e) {
    R.message(R.formatCommandError(__WEBUI_TEXT__("diagnostic.crash_download_failed"), e), "error");
  } finally {
    crashBusy = false;
    await R.refreshStatus();
  }
}
async function emptyCrashes() {
  if (!confirm(__WEBUI_TEXT__("diagnostic.empty_crashes_confirm"))) return;
  crashBusy = true;
  await hideUntilChanged("crashes", () =>
    R.api("/api/v1/diagnostic/crashes/empty", {
      method: "POST",
      body: JSON.stringify({ confirm: "EMPTY_CRASH_ARCHIVE" }),
      timeoutMs: 12e4,
    }).then(
      () => (R.message(__WEBUI_TEXT__("diagnostic.crashes_emptied"), "ok"), true),
      (e) =>
        R.message(
          R.formatCommandError(__WEBUI_TEXT__("diagnostic.crash_empty_failed"), e),
          "error",
        ),
    ),
  );
  crashBusy = false;
  await R.refreshStatus();
}
