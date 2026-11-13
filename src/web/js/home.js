"use strict";
import * as R from "./runtime.js?v=__FW_ASSET_TAG__";
const $ = R.$;
let ready = false;
let quickWeight = null;
let quickWeightWanted = false;
let accessHintClosed = false,
  recipeTag = 0;
// Controller states in which water is flowing.
const BREWING_STATES = ["BREW", "RINSE", "MANUAL_NO_SCALE"];

// Boiler rows interpolate the warm-up estimate locally from the absolute
// readyAtUtcSec the stream delivers once; a stable estimate costs no frames.
function laMarzoccoBoilerText(lm, boiler) {
  if (!lm || !lm.accountConfigured) return __WEBUI_TEXT__("runtime.not_connected");
  if (!lm.observeState) return __WEBUI_TEXT__("runtime.disabled");
  const data = lm.boiler ? lm.boiler[boiler] : null;
  const state = data ? data.state : "";
  if (state === "ready") return __WEBUI_TEXT__("runtime.ready_2");
  const label =
    __WEBUI_TEXT__("home.boiler_states").split("|")[
      ["heating_up", "no_water", "eco", "off", "standby"].indexOf(state)
    ];
  if (!label) return "—";
  return state === "heating_up" || state === "eco" ? label + readyIn(minsLeft(data)) : label;
}
// Whole minutes until a boiler's readyAtUtcSec; zero or less once it passed.
function minsLeft(boiler) {
  return Math.ceil(((boiler?.readyAtUtcSec || 0) - R.deviceUtcSec()) / 60);
}
function readyIn(mins) {
  return mins > 0 ? " · " + __WEBUI_TEXT__("home.ready_in").replace("%1", mins) : "";
}

// Equipment rows repeat the panel's key reading next to the state.
function setRowValue(row, value, hint) {
  $(row + "Value").textContent = value;
  $(row + "Hint").textContent = hint;
}

function renderLaMarzoccoBoilers() {
  // Reads the live cache: once the stream is invalidated the cache is null
  // and the ticker leaves the reset placeholders alone.
  const st = R.homeStatusCache(),
    lm = st?.laMarzocco;
  renderReadyMsg(st, lm);
  if (st) renderMachineRow(st);
  if (!lm) return;
  const brew = $("homeLaMarzoccoBrewBoiler"),
    steam = $("homeLaMarzoccoSteamBoiler");
  if (brew) brew.textContent = laMarzoccoBoilerText(lm, "coffee");
  if (steam) steam.textContent = laMarzoccoBoilerText(lm, "steam");
}

export function applyStatus(s) {
  R.applyHomeStatus(s);
  setRowValue("scaleRow", $("scaleWeight").textContent, $("preferredScale").textContent);
  setRowValue(
    "cupRow",
    $("cupWeight").textContent,
    !s.scale?.available
      ? __WEBUI_TEXT__("home.needs_scale")
      : R.cupNeedsRetare(s)
        ? __WEBUI_TEXT__("home.cup_not_tared")
        : "",
  );
  const brewing = BREWING_STATES.includes(s.state);
  if (quickWeight && brewing) quickWeight.close();
  // URL launcher: ?edit_weight=1 opens the sheet without the Presets row.
  // Wait for the active preset and the admin lock to settle, then open once
  // and drop the parameter.
  if (quickWeightWanted && quickWeight && !brewing) {
    const preset = R.presetState.items.find((x) => x.id === R.presetState.activeId);
    if (preset && preset.goalWeightG && R.controlsMutable) {
      quickWeightWanted = false;
      history.replaceState(null, "", location.pathname);
      quickWeight.open();
    }
  }
  renderHints(s);
  // Builds without the Micra integration ship none of its rows.
  if (__LA_MARZOCCO__) {
    $("homeLaMarzoccoPower").textContent = laMarzoccoPower(s.laMarzocco);
    $("homeLaMarzoccoCleaning").textContent = R.formatLaMarzoccoCleaning(s.laMarzocco);
  }
  renderLaMarzoccoBoilers();
}
// A card shows whenever its condition holds, independently of the others.
// Dismissals stay in this browser, except the access one,
// which the controller keeps for every phone. A companion's pairing request
// has no dismissal: it lasts ten minutes at most, Admin's Ignore is the
// lasting answer, and a key could not tell a new request from an old one.
// A page that came through an unpaired companion offers to pair it; a request
// coming or going refreshes that companion's own status (its code, its state).
const cardKey = (id) => "obbwCard:" + id.replace(/Hint$/, "");
let companionAsked = false;
function renderHints(s) {
  if (!s) return;
  // A preset change means the recipe has been looked at.
  if (recipeTag && s.presetsTag !== recipeTag) R.dismissCard(cardKey("recipeHint"));
  recipeTag = s.presetsTag;
  // A refused La Marzocco account blocks the machine link: shown first, no
  // dismissal, gone once the account works again.
  const problem = s.laMarzocco?.accountProblem,
    policy = $("laMarzoccoPolicyHint"),
    account = $("laMarzoccoAccountHint");
  if (policy) {
    policy.hidden = problem !== "policy";
    $("laMarzoccoPolicyHintAccept").disabled = !R.controlsMutable;
    account.hidden = problem !== "auth" && problem !== "access";
    const auth = problem === "auth";
    $("laMarzoccoAccountHintTitle").textContent = auth
      ? __WEBUI_TEXT__("home.la_marzocco_auth_title")
      : __WEBUI_TEXT__("home.la_marzocco_access_title");
    $("laMarzoccoAccountHintText").textContent = auth
      ? __WEBUI_TEXT__("home.la_marzocco_auth_text")
      : __WEBUI_TEXT__("home.la_marzocco_access_text");
  }
  const request = s.companion?.requestName,
    here = R.uiState.companionHere,
    code = here?.pairing?.state === "pending" && here.pairing.code;
  if (!!request !== companionAsked) {
    companionAsked = !!request;
    R.companionHere(true).then(() => renderHints(R.homeStatusCache()));
  }
  if (request) {
    $("companionHintTitle").textContent = R.sub(__WEBUI_TEXT__("home.companion_hint_title"), [
      request,
    ]);
    $("companionHintText").textContent = code
      ? R.sub(__WEBUI_TEXT__("home.companion_hint_code"), [code])
      : R.sub(__WEBUI_TEXT__("home.companion_hint_text"), [s.companion.requestAddress]);
  }
  if (here)
    $("companionPairHintText").textContent = R.sub(__WEBUI_TEXT__("home.companion_pair_text"), [
      here.name,
    ]);
  for (const [id, want] of [
    ["companionHint", !!request],
    ["companionPairHint", !request && R.companionPairable()],
    ["accessHint", !accessHintClosed && s.access?.notice],
    [
      "laMarzoccoCloudHint",
      s.machineIntegration === "la_marzocco_cloud" && s.laMarzocco?.accountConfigured === false,
    ],
    // Until the first shot is saved; unknown until the shot stream answers.
    ["recipeHint", R.shotRecorded() === false],
    // My scale first, with a scale already saved: it may still accept another.
    ["scaleOnlyHint", s.config?.scaleMacCacheMode === "prefer" && !!s.scale?.preferredMac],
  ]) {
    const el = $(id);
    if (!el) continue;
    el.hidden = !(want && !R.cardDismissed(cardKey(id)));
  }
  if ($("scaleOnlyHintUse")) $("scaleOnlyHintUse").disabled = !R.controlsMutable;
  // One card per recording: Stop while it runs, Delete once it is held.
  // Download or Delete hides it until the next recording; Stop and Delete
  // follow the Diagnostic page's gate, Download only this window's ownership.
  const rec = $("scaleRecCard");
  if (!rec) return;
  const id = String(s.scaleRecordingId || ""),
    running = s.scaleRecording === "recording",
    locked = !R.webUiOwner || !(R.adminUnlockedFor(s) || s.development || R.diagnosticVisible);
  if (!id && R.cardDismissed(cardKey("scaleRec"))) R.dismissCard(cardKey("scaleRec"), "");
  rec.hidden = !id || R.cardDismissed(cardKey("scaleRec")) === id;
  $("scaleRecTitle").textContent = running
    ? __WEBUI_TEXT__("home.scale_rec_title")
    : __WEBUI_TEXT__("home.scale_rec_held_title");
  $("scaleRecText").textContent = running
    ? __WEBUI_TEXT__("home.scale_rec_text")
    : __WEBUI_TEXT__("home.scale_rec_held_text");
  $("scaleRecStop").hidden = !running;
  $("scaleRecDelete").hidden = running;
  $("scaleRecStop").disabled = locked;
  $("scaleRecDelete").disabled = locked || s.scaleRecording !== "held";
  $("scaleRecDownload").disabled = !R.webUiOwner;
}
// The machine row ticks with the boiler rows so a warm-up countdown stays live.
function renderMachineRow(s) {
  const rs = $("machineRowState"),
    row = $("machineRow");
  if (!row) return;
  const lm = s.laMarzocco || {},
    power = laMarzoccoPower(lm),
    laMarzoccoActive =
      s.machineIntegration === "la_marzocco_cloud" && lm.accountConfigured && lm.observeState,
    off = power.startsWith("OFF"),
    needsWater = lm.readiness === "needs_water",
    warming =
      !off && laMarzoccoActive && ["warming_up", "waiting_for_steam"].includes(lm.readiness),
    // With boiler evidence the lamp claims Ready only when both boilers
    // are ready; without it (or without the integration) the controller
    // state decides, as before.
    rdy = !off && (laMarzoccoActive ? lm.readiness === "ready" : s.state === "READY"),
    labels = {
      warming_up: __WEBUI_TEXT__("home.warming_up"),
      waiting_for_steam: __WEBUI_TEXT__("home.waiting_for_steam"),
      needs_water: __WEBUI_TEXT__("home.needs_water"),
    },
    // No live boiler reading: minutes left while warming, target once ready;
    // "ON - Optimistic" is a wake-up the cloud has not confirmed yet.
    mins = warming ? Math.max(minsLeft(lm.boiler?.coffee), minsLeft(lm.boiler?.steam)) : 0,
    target = rdy && laMarzoccoActive ? Math.round((lm.boiler?.coffee?.targetDeciC || 0) / 10) : 0;
  rs.textContent = off
    ? __WEBUI_TEXT__("home.turned_off")
    : rdy
      ? __WEBUI_TEXT__("runtime.ready_2")
      : labels[lm.readiness] ||
        (power.indexOf("ON") === 0 ? __WEBUI_TEXT__("home.on") : $("state").textContent);
  const [value, hint] =
      mins > 0
        ? [mins + __WEBUI_TEXT__("runtime.min_2"), __WEBUI_TEXT__("home.until_ready")]
        : target > 0
          ? [target + "°C", __WEBUI_TEXT__("home.brew_temperature")]
          : laMarzoccoActive
            ? [
                "—",
                lm.accountProblem
                  ? R.accountProblemLabel(lm.accountProblem)
                  : off
                    ? __WEBUI_TEXT__("home.turn_on_to_brew")
                    : needsWater
                      ? __WEBUI_TEXT__("home.refill_tank")
                      : power.startsWith("ON ")
                        ? __WEBUI_TEXT__("home.starting_up")
                        : "",
              ]
            : ["", ""],
    bad = !rdy && (!laMarzoccoActive || off || needsWater);
  setRowValue("machineRow", value, hint);
  row.classList.toggle("lampWarn", warming);
  row.classList.toggle("lampBad", bad);
  // Home's machine heading repeats the row's state and its key reading; a
  // shot in progress is not a fault, so its dot never turns red while brewing.
  $("machineId").dataset.state = warming
    ? "warn"
    : bad && !BREWING_STATES.includes(s.state)
      ? "bad"
      : "ok";
  $("machineIdState").textContent = rs.textContent;
  $("machineIdDetail").textContent =
    mins > 0 ? __WEBUI_TEXT__("home.ready_in").replace("%1", mins) : value === "—" ? hint : value;
  if (__LA_MARZOCCO__) renderMachinePower(s, lm, power, laMarzoccoActive, warming);
}
// Admin → Machine control's power button. A hold turns the machine on or
// off; from the keyboard a first press arms it and a second within 3 s acts.
// Neutral while off, yellow with the warm-up ring while heating, red (turn
// off) once on; a command shows as busy only once the controller took it.
let powerPending = null, // {on, until, sent}: the last command this page sent
  powerArmed = null, // {turn, until}: a first key press waiting for the second
  warmRing = { since: 0, p: 0 };
function renderMachinePower(s, lm, power, active, warming) {
  const b = $("machinePower");
  if (!b) return;
  b.hidden = !(s.machineControl?.power && active);
  if (b.hidden) return;
  const on = power.startsWith("ON"),
    overlay = lm.quality === "optimistic" && (lm.optimisticOn || lm.optimisticOff);
  if (powerPending && (on === powerPending.on || Date.now() > powerPending.until))
    powerPending = null;
  const busy = overlay || !!powerPending?.sent,
    target = overlay ? !!lm.optimisticOn : powerPending ? powerPending.on : !on,
    armed = powerArmed?.turn === (target ? "on" : "off") && Date.now() < powerArmed.until,
    // The firmware's own gate (no shot, rinse, backflush or other cycle).
    reason = !s.configMutable
      ? __WEBUI_TEXT__("home.power_when_idle")
      : lm.accountProblem
        ? R.accountProblemLabel(lm.accountProblem)
        : power === "—"
          ? __WEBUI_TEXT__("home.power_unreachable")
          : !R.adminUnlockedFor(s)
            ? __WEBUI_TEXT__("home.power_unlock_admin")
            : !R.controlsMutable
              ? __WEBUI_TEXT__("home.power_unavailable")
              : "",
    label = busy
      ? target
        ? __WEBUI_TEXT__("home.power_turning_on")
        : __WEBUI_TEXT__("home.power_turning_off")
      : reason
        ? (target ? __WEBUI_TEXT__("home.power_turn_on") : __WEBUI_TEXT__("home.power_turn_off")) +
          " — " +
          reason
        : armed
          ? target
            ? __WEBUI_TEXT__("home.power_again_on")
            : __WEBUI_TEXT__("home.power_again_off")
          : target
            ? __WEBUI_TEXT__("home.power_hold_on")
            : __WEBUI_TEXT__("home.power_hold_off");
  b.dataset.tone = busy ? "busy" : !on ? "idle" : warming ? "warm" : "stop";
  b.dataset.turn = target ? "on" : "off";
  b.disabled = busy || !!powerPending || !!reason;
  b.classList.toggle("armed", armed && !b.disabled);
  b.setAttribute("aria-busy", busy);
  b.setAttribute("aria-label", label);
  b.title = label;
  // The warm-up ring fills on the device clock between the firmware's start
  // of this warm-up and the later boiler's estimate; a re-estimate slows it
  // down but never pulls it back.
  const since = lm.warmingSinceUtcSec || 0,
    readyAt = Math.max(lm.boiler?.coffee?.readyAtUtcSec || 0, lm.boiler?.steam?.readyAtUtcSec || 0),
    ring = warming && !busy && since > 0 && readyAt > since;
  if (warmRing.since !== since) warmRing = { since, p: 0 };
  if (ring)
    warmRing.p = Math.max(
      warmRing.p,
      Math.min(1, Math.max(0, (R.deviceUtcSec() - since) / (readyAt - since))),
    );
  b.classList.toggle("ringOn", ring);
  $("machinePowerWarm").style.strokeDashoffset = 100 * (1 - warmRing.p);
}
// Acts only if the button still offers the action the person started on:
// a state change or a lock during the hold or between key presses cancels.
async function sendMachinePower(turn) {
  const b = $("machinePower");
  if (b.disabled || b.dataset.turn !== turn) return;
  const pending = (powerPending = { on: turn === "on", until: Date.now() + 30000 });
  powerArmed = null;
  renderLaMarzoccoBoilers();
  pending.sent = await R.command(
    "/api/v1/machine/la-marzocco",
    { action: "power", on: pending.on },
    false,
    "",
    __WEBUI_TEXT__("home.power_failed"),
  );
  if (!pending.sent && powerPending === pending) powerPending = null;
  renderLaMarzoccoBoilers();
}
function initMachinePower() {
  const b = $("machinePower");
  if (!b) return;
  // The ring fills by CSS while the timer runs; letting go first cancels.
  let timer = 0;
  const release = () => {
    clearTimeout(timer);
    b.classList.remove("holding");
  };
  b.addEventListener("pointerdown", (e) => {
    if (b.disabled || e.button > 0) return;
    try {
      b.setPointerCapture(e.pointerId);
    } catch {
      // A synthetic pointer has nothing to capture.
    }
    b.classList.add("holding");
    const turn = b.dataset.turn;
    timer = setTimeout(() => sendMachinePower(turn), 900);
  });
  b.addEventListener("pointerup", release);
  b.addEventListener("pointercancel", release);
  b.addEventListener("click", (e) => {
    if (e.detail !== 0 || b.disabled) return;
    if (Date.now() < powerArmed?.until) sendMachinePower(powerArmed.turn);
    else {
      powerArmed = { turn: b.dataset.turn, until: Date.now() + 3000 };
      renderLaMarzoccoBoilers();
    }
  });
}
export function init() {
  if (ready) return;
  ready = true;
  R.registerViewStatus("home", applyStatus);
  if (__LA_MARZOCCO__) initMachinePower();
  // The countdown ticks only while Home shows; entering Home repaints it.
  setInterval(() => {
    if (!document.hidden && R.activeView === "home") renderLaMarzoccoBoilers();
  }, 1000);
  const rinseButton = $("rinseButton");
  if (rinseButton) rinseButton.onclick = () => R.command("/api/v1/control/rinse");
  const stopButton = $("stopButton");
  if (stopButton)
    stopButton.onclick = () =>
      stopButton.dataset.mode === "stop"
        ? R.command("/api/v1/control/stop")
        : Date.now() - (stopButton.dataset.stoppedAt || 0) > 2e3 &&
          R.command("/api/v1/control/paddle", { on: true });
  const forcePulseButton = $("forcePulseButton");
  if (forcePulseButton) forcePulseButton.onclick = () => R.command("/api/v1/control/force-pulse");
  [
    ["overrideIdleLink", "off"],
    ["overrideBrewingLink", "on"],
  ].forEach(([i, s]) => {
    const a = $(i);
    if (!a) return;
    a.onclick = (e) => {
      e.preventDefault();
      if (a.getAttribute("aria-disabled") === "true") return;
      R.command("/api/v1/control/state-override", { state: s });
    };
  });
  const renameScaleLink = $("renameScaleLink");
  if (renameScaleLink)
    renameScaleLink.onclick = (e) => {
      e.preventDefault();
      if (renameScaleLink.getAttribute("aria-disabled") === "true") return;
      R.renameScale();
    };
  if ($("grindHintNewBeans")) $("grindHintNewBeans").onclick = () => R.startNewCoffee();
  if ($("grindHintLater"))
    $("grindHintLater").onclick = () =>
      R.dismissOnController("grind", $("grindHintCard").dataset.key);
  for (const id of [
    "companionHint",
    "companionPairHint",
    "accessHint",
    "laMarzoccoCloudHint",
    "recipeHint",
    "scaleOnlyHint",
  ]) {
    const open = $(id + "Open"),
      dismiss = $(id + "Dismiss");
    if (open)
      open.onclick = (e) => {
        e.preventDefault();
        history.pushState(null, "", open.href);
        R.renderRoute(open.pathname);
      };
    if (!dismiss) continue;
    dismiss.onclick = () => {
      // The access choice is kept on the controller, so other phones skip it too.
      if (id === "accessHint") {
        accessHintClosed = true;
        R.api("/api/v1/auth/notice", { method: "POST", body: "{}" }).catch(() => {});
      } else R.dismissCard(cardKey(id));
      renderHints(R.homeStatusCache());
    };
  }
  if ($("laMarzoccoPolicyHintAccept"))
    $("laMarzoccoPolicyHintAccept").onclick = () => R.laMarzoccoAction("acceptPolicy");
  if ($("scaleRecStop")) {
    const handled = (ok) =>
      ok && R.dismissCard(cardKey("scaleRec"), String(R.homeStatusCache()?.scaleRecordingId));
    $("scaleRecStop").onclick = () =>
      R.command("/api/v1/diagnostic/scale-profile", { action: "stop" });
    $("scaleRecDelete").onclick = async () =>
      confirm(__WEBUI_TEXT__("diagnostic.delete_profile_confirm")) &&
      handled(await R.command("/api/v1/diagnostic/scale-profile", { action: "delete" }));
    $("scaleRecDownload").onclick = async () => handled(await R.downloadScaleProfile());
  }
  if ($("scaleOnlyHintUse"))
    $("scaleOnlyHintUse").onclick = () =>
      R.command(
        "/api/v1/config",
        R.withBaseRev({ scaleMacCacheMode: "only" }),
        1,
        "",
        __WEBUI_TEXT__("runtime.could_not_save_shared_settings"),
      );
  $("companionPairHintPair").onclick = async ({ currentTarget: b }) => {
    b.disabled = true;
    await R.pairCompanionHere().catch((e) => R.message(e.message, "error"));
    b.disabled = false;
    renderHints(R.homeStatusCache());
  };
  R.companionHere().then(() => renderHints(R.homeStatusCache()));
  R.registerViewStatus("homeHints", renderHints);
  quickWeight = initQuickWeight();
  quickWeightWanted = new URLSearchParams(location.search).has("edit_weight");
}
function renderReadyMsg(s, lm) {
  const msg = $("homeReadyMsg"),
    cfg = s?.config || {},
    sc = s?.scale || {},
    g = s?.noScaleShotGuard || {},
    idle = !!s && !["BREW", "RINSE", "MANUAL_NO_SCALE"].includes(s.state);
  if (!msg) return;
  // Priority: no scale, an empty or critical battery (red), the Micra
  // warming up, then a low battery.
  const batt = sc.available ? R.scaleBatteryLevel(sc.batteryPct) : null,
    urgent =
      batt === "empty" || batt === "critical"
        ? R.sub(
            batt === "empty"
              ? __WEBUI_TEXT__("home.battery_empty")
              : __WEBUI_TEXT__("home.battery_critical"),
            [sc.batteryPct],
          )
        : "";
  const text = !idle
    ? ""
    : cfg.brewByWeight && !(sc.available && sc.streamState === "FRESH")
      ? !(g.enabled && g.armed)
        ? __WEBUI_TEXT__("home.no_scale_manual")
        : cfg.noScaleBbwMode === "require_scale"
          ? __WEBUI_TEXT__("home.no_scale_required")
          : __WEBUI_TEXT__("home.no_scale_warn")
      : urgent ||
        (s.machineIntegration === "la_marzocco_cloud" &&
        lm?.accountConfigured &&
        lm.observeState &&
        lm.readiness === "warming_up"
          ? __WEBUI_TEXT__("home.machine_warming") + readyIn(minsLeft(lm.boiler?.coffee))
          : batt === "low"
            ? R.sub(__WEBUI_TEXT__("home.battery_low"), [sc.batteryPct])
            : "");
  if (text && msg.textContent !== text) msg.textContent = text;
  if (text) msg.classList.toggle("isErr", text === urgent);
  R.reveal(msg, !!text);
}
function initQuickWeight() {
  const sheet = $("qwSheet"),
    zone = $("qwZone"),
    grab = $("qwGrab"),
    head = $("qwHead");
  if (!sheet || !zone || !grab || !head) return null;
  const backdrop = $("qwBackdrop"),
    scale = $("qwScale"),
    num = $("qwNum"),
    def = $("qwDef"),
    resetBtn = $("qwReset"),
    presetEl = $("qwPreset"),
    doneBtn = $("qwDone"),
    limitMin = $("qwLimitMin"),
    limitMax = $("qwLimitMax"),
    ratio = $("qwRatio");
  // Firmware preset goal: decigrams, 10.0–200.0 g (settings validation).
  const MIN = 10,
    MAX = 200,
    PX_PER_DG = 6, // 60 px per gram: tenths read clearly
    BASE_DG_PER_PX = 1 / PX_PER_DG, // the ruler tracks the finger 1:1
    SPEED_CAP = 3, // gentle gearing: only quick flicks go past 1:1
    SPEED_K = 1.5;
  const inner = document.createElement("div");
  inner.className = "qwScaleIn";
  const ticks = [];
  // Ticks run 15 g past both ends so the strip stays full (760 px sheet);
  // majors label whole grams, mediums halves, minors tenths.
  const base = (MIN - 15) * 10;
  // About 2,400 nodes: built on the first open, not on every Home start.
  const build = () => {
    for (let d = base; d <= (MAX + 15) * 10; d++) {
      const tick = document.createElement("i");
      let label = null;
      if (d % 10 === 0) {
        tick.className = "big";
        if (d >= MIN * 10 && d <= MAX * 10) {
          label = document.createElement("b");
          label.textContent = d / 10;
          label.style.left = ((d - base) * PX_PER_DG).toFixed(1) + "px";
          inner.appendChild(label);
        }
      } else if (d % 5 === 0) tick.className = "med";
      tick.style.left = ((d - base) * PX_PER_DG).toFixed(1) + "px";
      inner.appendChild(tick);
      ticks.push({ d, node: tick, label });
    }
    scale.appendChild(inner);
  };
  // Editable range [lo, hi] in decigrams mirrors the Settings rules for the
  // relative margins, so the dial can never offer a target Save would reject.
  let lo = MIN * 10,
    hi = MAX * 10,
    value = MIN * 10,
    openValue = MIN * 10,
    dose = 0,
    opener = null;
  const clamp = (d) => Math.max(lo, Math.min(hi, d));
  const speedMul = (v) => 1 + Math.min(SPEED_CAP - 1, Math.abs(v || 0) * SPEED_K);
  let nowIdx = -1,
    outLo = -1,
    outHi = -1;
  // Ticks sit at fixed offsets; one translate slides the whole ruler and only
  // the "now" tick and the readouts change per frame, so a drag never
  // restyles the strip.
  const paint = () => {
    const width = scale.getBoundingClientRect().width;
    inner.style.transform =
      "translateX(" + (width / 2 - (value - base) * PX_PER_DG).toFixed(1) + "px)";
    if (ticks[nowIdx]) ticks[nowIdx].node.classList.remove("now");
    nowIdx = value - base;
    ticks[nowIdx].node.classList.add("now");
    num.textContent = (value / 10).toFixed(1);
    ratio.hidden = !dose;
    ratio.firstChild.textContent = __WEBUI_TEXT__("home.dose_in").replace("%1", dose.toFixed(1));
    ratio.lastChild.textContent = "1:" + (value / 10 / dose).toFixed(1);
    zone.setAttribute("aria-valuenow", (value / 10).toFixed(1));
    zone.setAttribute("aria-valuetext", (value / 10).toFixed(1) + __WEBUI_TEXT__("runtime.g_2"));
    limitMin.hidden = value > lo;
    limitMax.hidden = value < hi;
    def.textContent = (openValue / 10).toFixed(1);
    resetBtn.classList.toggle("show", value !== openValue);
    doneBtn.disabled = value === openValue;
  };
  const open = () => {
    const preset = R.presetState.items.find((x) => x.id === R.presetState.activeId);
    if (!preset || !preset.goalWeightG) return;
    // Margins are relative and follow the target: an enabled slow margin
    // keeps 5 g in the cup, an enabled fast margin stays within a quarter
    // of the target and the extra yield within the 200 g maximum.
    const slow = !!preset.slowExtractionGuardEnabled && Number.isFinite(+preset.slowMarginG),
      fast = !!preset.fastExtractionGuardEnabled && Number.isFinite(+preset.fastMarginG);
    lo = Math.ceil(
      Math.max(MIN, slow ? +preset.slowMarginG + 5 : 0, fast ? 4 * preset.fastMarginG : 0) * 10 -
        1e-6,
    );
    hi = Math.floor((MAX - (fast ? +preset.fastMarginG : 0)) * 10 + 1e-6);
    if (hi < lo) {
      lo = MIN * 10;
      hi = MAX * 10;
    } // a recipe with no valid target must not lock the dial
    if (!ticks.length) build();
    if (lo !== outLo || hi !== outHi) {
      outLo = lo;
      outHi = hi;
      for (const { d, node, label } of ticks)
        for (const el of label ? [node, label] : [node])
          el.classList.toggle("out", d < lo || d > hi);
    }
    limitMin.classList.toggle("toneSlow", slow);
    limitMax.classList.toggle("toneFast", fast);
    value = openValue = clamp(Math.round(+preset.goalWeightG * 10));
    dose = +preset.doseG || 0;
    presetEl.textContent = preset.name || "";
    zone.setAttribute("aria-valuemin", (lo / 10).toFixed(1));
    zone.setAttribute("aria-valuemax", (hi / 10).toFixed(1));
    paint();
    sheet.inert = false;
    backdrop.inert = false;
    sheet.classList.add("open");
    backdrop.classList.add("show");
    document.body.classList.add("qwOpen");
    opener = document.activeElement;
    zone.focus();
  };
  // The open sheet is modal: focus that tabs out of it returns to the dial.
  sheet.addEventListener("focusout", (e) => {
    if (sheet.classList.contains("open") && !sheet.contains(e.relatedTarget)) zone.focus();
  });
  const commit = async (dg) => {
    try {
      if (!(await R.cleanBrewForm())) return;
      const field = $("goalWeightG"),
        rejected = String(field.value);
      field.value = (dg / 10).toFixed(1);
      // A rejected value must not linger in the form as a phantom edit: put
      // the previous value back so the brew baseline and the live warnings
      // keep describing the real saved state.
      if (!(await R.saveBrewPreset({ goalWeightG: dg / 10 }))) field.value = rejected;
    } catch (e) {
      R.message(R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_save_preset"), e), "error");
    }
  };
  const close = (save) => {
    if (!sheet.classList.contains("open")) return;
    halt();
    stop();
    sheet.classList.remove("open");
    backdrop.classList.remove("show");
    sheet.inert = true;
    backdrop.inert = true;
    document.body.classList.remove("qwOpen");
    opener?.focus?.();
    if (save && value !== openValue) commit(value);
  };
  const drag = (node, handlers) => {
    node.addEventListener("pointerdown", (event) => {
      if (event.pointerType === "mouse" && event.button !== 0) return;
      // Pointer capture would retarget the click of interactive children
      // (Done lives inside the grab bar) onto the drag node, swallowing it.
      if (event.target?.closest("button,a,input")) return;
      event.preventDefault();
      try {
        node.setPointerCapture(event.pointerId);
      } catch {
        /* synthetic pointers cannot be captured */
      }
      const ctx = handlers.start ? handlers.start(event) : {};
      const move = (ev) => handlers.move && handlers.move(ev, ctx);
      const end = (ev) => {
        node.removeEventListener("pointermove", move);
        node.removeEventListener("pointerup", end);
        node.removeEventListener("pointercancel", end);
        handlers.end && handlers.end(ev, ctx);
      };
      node.addEventListener("pointermove", move);
      node.addEventListener("pointerup", end);
      node.addEventListener("pointercancel", end);
    });
  };
  const vel = { t: 0, v: 0 };
  let pending = 0,
    fling = 0, // signed release speed, decigrams per ms
    glide = 0;
  const halt = () => globalThis.cancelAnimationFrame?.(glide);
  // Inertia: a released flick keeps sliding and eases out with friction
  // (about 200 ms of travel at release speed), stopping at either limit.
  const coast = () => {
    let last = performance.now();
    const run = (now) => {
      const dt = now - last;
      last = now;
      fling *= Math.pow(0.995, dt);
      pending += fling * dt;
      const next = clamp(value + Math.trunc(pending));
      pending %= 1;
      if (next === value && (value === lo || value === hi)) fling = 0;
      value = next;
      paint();
      if (Math.abs(fling) > 0.002) glide = requestAnimationFrame(run);
    };
    if (Math.abs(fling) > 0.02) glide = requestAnimationFrame(run);
  };
  drag(zone, {
    start: (e) => {
      halt();
      vel.t = performance.now();
      vel.v = 0;
      fling = 0;
      pending = 0;
      return { x: e.clientX, dx: 0 };
    },
    move: (e, ctx) => {
      const dx = e.clientX - ctx.x;
      ctx.x = e.clientX;
      const now = performance.now(),
        dt = Math.max(4, now - vel.t);
      // A reversal restarts the speed estimate: stale speed from the outgoing
      // gesture would sling the value past the intended stop.
      if (dx * ctx.dx < 0) vel.v = 0;
      ctx.dx = dx;
      // Slow gestures stay surgical; sustained speed gears up to SPEED_CAP×
      // so one drag can cross the range without releasing.
      vel.v = 0.75 * vel.v + 0.25 * (Math.abs(dx) / dt);
      vel.t = now;
      // Sub-tenth motion accrues in pending instead of rounding away per
      // event, so slow drags still move the dial a tenth at a time.
      // Direct manipulation: the finger carries the ruler, so dragging left
      // brings higher grams under the needle.
      const step = -dx * BASE_DG_PER_PX * speedMul(vel.v);
      fling = 0.75 * fling + (0.25 * step) / dt;
      pending += step;
      value = clamp(value + Math.trunc(pending));
      pending %= 1;
      paint();
    },
    end: () => {
      // A finger that rested before lifting leaves no inertia.
      if (performance.now() - vel.t > 80) fling = 0;
      else if (globalThis.requestAnimationFrame) coast();
    },
  });
  const dismissDrag = {
    start: (e) => {
      sheet.classList.add("nodrag");
      return { y: e.clientY, dy: 0 };
    },
    move: (e, ctx) => {
      ctx.dy = Math.max(0, e.clientY - ctx.y);
      sheet.style.transform = "translateY(" + ctx.dy + "px)";
    },
    end: (e, ctx) => {
      sheet.classList.remove("nodrag");
      if (ctx.dy > 90) {
        sheet.style.transition = "transform .3s ease-in";
        sheet.style.transform = "translateY(105%)";
        setTimeout(() => {
          sheet.style.transition = "";
          sheet.style.transform = "";
          close(true);
        }, 290);
      } else sheet.style.transform = "";
    },
  };
  drag(grab, dismissDrag);
  drag(head, dismissDrag);
  const nudge = (d) => {
    halt();
    value = clamp(value + d);
    paint();
  };
  zone.addEventListener("keydown", (e) => {
    const step = {
      ArrowUp: 1,
      ArrowRight: 1,
      ArrowDown: -1,
      ArrowLeft: -1,
      PageUp: 10,
      PageDown: -10,
    }[e.key];
    if (!step) return;
    e.preventDefault();
    nudge(step);
  });
  let hold = 0;
  const stop = () => clearTimeout(hold);
  for (const [id, dir] of [
    ["qwMinus", -1],
    ["qwPlus", 1],
  ]) {
    const btn = $(id);
    btn.onpointerdown = (e) => {
      e.preventDefault();
      stop();
      let n = 0;
      const step = () => {
        nudge(dir * (n > 20 ? 10 : 1));
        hold = setTimeout(step, n++ ? 70 : 400);
      };
      step();
    };
    btn.onpointerup = btn.onpointerleave = btn.onpointercancel = stop;
    btn.onkeydown = (e) => (e.key === "Enter" || e.key === " ") && (e.preventDefault(), nudge(dir));
  }
  addEventListener("click", (e) => e.target.closest(".presetAccEdit") && open());
  doneBtn.onclick = () => close(true);
  resetBtn.onclick = () => {
    halt();
    value = openValue;
    paint();
  };
  backdrop.onclick = () => close(true);
  // Escape cancels; Done, a backdrop tap, and a swipe down keep the value.
  sheet.addEventListener("keydown", (e) => {
    if (e.key === "Escape") close(false);
  });
  addEventListener("resize", () => {
    if (sheet.classList.contains("open")) paint();
  });
  return { open, close };
}
function laMarzoccoPower(m) {
  const p = m?.quality === "optimistic" && (m.optimisticOn ? "ON" : m.optimisticOff ? "OFF" : "");
  return p
    ? p + " - " + __WEBUI_TEXT__("home.optimistic")
    : ["ON", "OFF"].includes(m?.powerState)
      ? m.powerState
      : "—";
}
