"use strict";
import * as R from "./runtime.js?v=__FW_ASSET_TAG__";
const $ = R.$;
let ready = false;
let quickWeight = null;
let quickWeightWanted = false;
let micraHintClosed = false,
  accessHintClosed = false;

// Boiler rows interpolate the warm-up estimate locally from the absolute
// readyAtUtcSec the stream delivers once; a stable estimate costs no frames.
function micraBoilerText(lm, boiler) {
  if (!lm || !lm.accountConfigured) return __WEBUI_TEXT__("runtime.not_connected");
  if (!lm.observeState) return __WEBUI_TEXT__("runtime.disabled");
  const data = lm.boiler ? lm.boiler[boiler] : null;
  const state = data ? data.state : "";
  if (state === "ready") return __WEBUI_TEXT__("runtime.ready_2");
  const label =
    __WEBUI_TEXT__("home.boiler_states").split("|")[
      ["heating_up", "no_water", "eco", "off", "standby"].indexOf(state)
    ];
  if (!label) return __WEBUI_TEXT__("runtime.unknown");
  return state === "heating_up" || state === "eco" ? label + readyIn(minsLeft(data)) : label;
}
// Whole minutes until a boiler's readyAtUtcSec; zero or less once it passed.
function minsLeft(boiler) {
  return Math.ceil(((boiler?.readyAtUtcSec || 0) * 1000 - Date.now()) / 60000);
}
function readyIn(mins) {
  return mins > 0 ? " · " + __WEBUI_TEXT__("home.ready_in").replace("%1", mins) : "";
}

// Equipment rows repeat the panel's key reading next to the state.
function setRowValue(row, value, hint) {
  $(row + "Value").textContent = value;
  $(row + "Hint").textContent = hint;
}

function renderMicraBoilers() {
  // Reads the live cache: once the stream is invalidated the cache is null
  // and the ticker leaves the reset placeholders alone.
  const st = R.homeStatusCache(),
    lm = st?.lineaMicra;
  renderReadyMsg(st, lm);
  if (st) renderMachineRow(st);
  if (!lm) return;
  const brew = $("homeMicraBrewBoiler"),
    steam = $("homeMicraSteamBoiler");
  if (brew) brew.textContent = micraBoilerText(lm, "coffee");
  if (steam) steam.textContent = micraBoilerText(lm, "steam");
}

export function applyStatus(s) {
  R.applyHomeStatus(s);
  setRowValue("scaleRow", $("scaleWeight").textContent, $("preferredScale").textContent);
  setRowValue(
    "cupRow",
    $("cupWeight").textContent,
    s.scale?.available ? "" : __WEBUI_TEXT__("home.needs_scale"),
  );
  const brewing = ["BREW", "RINSE", "MANUAL_NO_SCALE"].includes(s.state);
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
  const hint = $("micraCloudHint");
  if (hint)
    hint.hidden =
      micraHintClosed ||
      s.lineaMicra?.accountConfigured !== false ||
      s.machineIntegration !== "linea_micra_cloud" ||
      R.cardDismissed("obbwCard:micraCloud");
  // Lowest priority: it waits while the Micra or grind hint is showing.
  const access = $("accessHint");
  if (access)
    access.hidden =
      accessHintClosed ||
      !s.access?.notice ||
      (hint && !hint.hidden) ||
      !$("grindHintCard")?.hidden;
  // Builds without the Micra integration ship none of its rows.
  if (__LINEA_MICRA__) {
    $("homeMicraPower").textContent = micraPower(s.lineaMicra);
    $("homeMicraCleaning").textContent = R.formatMicraCleaning(s.lineaMicra);
  }
  renderMicraBoilers();
}
// The machine row ticks with the boiler rows so a warm-up countdown stays live.
function renderMachineRow(s) {
  const rs = $("machineRowState"),
    row = $("machineRow");
  if (!row) return;
  const lm = s.lineaMicra || {},
    power = micraPower(lm),
    micraActive =
      s.machineIntegration === "linea_micra_cloud" && lm.accountConfigured && lm.observeState,
    off = power.startsWith("OFF"),
    needsWater = lm.readiness === "needs_water",
    warming = !off && micraActive && ["warming_up", "waiting_for_steam"].includes(lm.readiness),
    // With boiler evidence the lamp claims Ready only when both boilers
    // are ready; without it (or without the integration) the controller
    // state decides, as before.
    rdy = !off && (micraActive ? lm.readiness === "ready" : s.state === "READY"),
    labels = {
      warming_up: __WEBUI_TEXT__("home.warming_up"),
      waiting_for_steam: __WEBUI_TEXT__("home.waiting_for_steam"),
      needs_water: __WEBUI_TEXT__("home.needs_water"),
    },
    // No live boiler reading: minutes left while warming, target once ready;
    // "ON - Optimistic" is a wake-up the cloud has not confirmed yet.
    mins = warming ? Math.max(minsLeft(lm.boiler?.coffee), minsLeft(lm.boiler?.steam)) : 0,
    target = rdy && micraActive ? Math.round((lm.boiler?.coffee?.targetDeciC || 0) / 10) : 0;
  rs.textContent = off
    ? __WEBUI_TEXT__("home.turned_off")
    : rdy
      ? __WEBUI_TEXT__("runtime.ready_2")
      : labels[lm.readiness] ||
        (power.indexOf("ON") === 0 ? __WEBUI_TEXT__("home.on") : $("state").textContent);
  setRowValue(
    "machineRow",
    ...(mins > 0
      ? [mins + " min", __WEBUI_TEXT__("home.until_ready")]
      : target > 0
        ? [target + "°C", __WEBUI_TEXT__("home.brew_temperature")]
        : micraActive
          ? [
              "—",
              off
                ? __WEBUI_TEXT__("home.turn_on_to_brew")
                : needsWater
                  ? __WEBUI_TEXT__("home.refill_tank")
                  : power.startsWith("ON ")
                    ? __WEBUI_TEXT__("home.starting_up")
                    : "",
            ]
          : ["", ""]),
  );
  row.classList.toggle("lampWarn", warming);
  row.classList.toggle("lampBad", !rdy && (!micraActive || off || needsWater));
}
export function init() {
  if (ready) return;
  ready = true;
  R.registerViewStatus("home", applyStatus);
  // The countdown ticks only while Home shows; entering Home repaints it.
  setInterval(() => {
    if (!document.hidden && R.activeView === "home") renderMicraBoilers();
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
    $("grindHintLater").onclick = () => R.dismissCard($("grindHintCard").dataset.key);
  if ($("accessHint")) {
    $("accessHintOpen").onclick = (e) => {
      e.preventDefault();
      history.pushState(null, "", e.currentTarget.href);
      R.renderRoute("/admin");
    };
    // The choice is kept on the controller, so other phones skip it too.
    $("accessHintDismiss").onclick = () => {
      accessHintClosed = $("accessHint").hidden = true;
      R.api("/api/v1/auth/notice", { method: "POST", body: "{}" }).catch(() => {});
    };
  }
  if ($("micraCloudHint")) {
    $("micraCloudHintOpen").onclick = (e) => {
      e.preventDefault();
      history.pushState(null, "", e.currentTarget.href);
      R.renderRoute("/settings");
    };
    // Dismiss hides it until the page reloads; the checkbox makes it permanent.
    $("micraCloudHintDismiss").onclick = () => {
      if ($("micraCloudHintMute").checked) R.dismissCard("obbwCard:micraCloud");
      micraHintClosed = $("micraCloudHint").hidden = true;
    };
  }
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
  const text = !idle
    ? ""
    : cfg.brewByWeight && !(sc.available && sc.streamState === "FRESH")
      ? !(g.enabled && g.armed)
        ? __WEBUI_TEXT__("home.no_scale_manual")
        : cfg.noScaleBbwMode === "require_scale"
          ? __WEBUI_TEXT__("home.no_scale_required")
          : __WEBUI_TEXT__("home.no_scale_warn")
      : s.machineIntegration === "linea_micra_cloud" &&
          lm?.accountConfigured &&
          lm.observeState &&
          lm.readiness === "warming_up"
        ? __WEBUI_TEXT__("home.machine_warming") + readyIn(minsLeft(lm.boiler?.coffee))
        : "";
  if (text && msg.textContent !== text) msg.textContent = text;
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
    zone.setAttribute("aria-valuetext", (value / 10).toFixed(1) + " g");
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
  sheet.addEventListener("keydown", (e) => {
    if (e.key === "Escape") close(true);
  });
  addEventListener("resize", () => {
    if (sheet.classList.contains("open")) paint();
  });
  return { open, close };
}
function micraPower(m) {
  const p = m?.quality === "optimistic" && (m.optimisticOn ? "ON" : m.optimisticOff ? "OFF" : "");
  return p
    ? p + " - " + __WEBUI_TEXT__("home.optimistic")
    : ["ON", "OFF"].includes(m?.powerState)
      ? m.powerState
      : __WEBUI_TEXT__("runtime.unknown");
}
