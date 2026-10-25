"use strict";
import * as R from "./runtime.js?v=__FW_ASSET_TAG__";
const $ = R.$;
function scaleHistoryLabel(e) {
  const m = (e && e.mac) || "";
  if (!m) return "";
  return (
    ((e.friendlyName && String(e.friendlyName).trim()) ||
      (e.name && String(e.name).trim()) ||
      __WEBUI_TEXT__("runtime.unknown_2")) +
    __WEBUI_TEXT__("runtime.unknown_3") +
    m
  );
}
function updatePreferredScaleSelect(s) {
  const sel = $("preferredScaleSelect"),
    hint = $("preferredScalePauseHint");
  if (!sel) return;
  const pauseMs = (s.scale && s.scale.macCachePauseRemainingMs) || 0;
  if (hint)
    hint.textContent =
      pauseMs > 0
        ? __WEBUI_TEXT__("runtime.waiting") +
          Math.ceil(pauseMs / 1000) +
          __WEBUI_TEXT__("runtime.s_before_looking_for_a_scale")
        : "";
  sel.disabled = !R.controlsMutable;
  const history = s.scale && Array.isArray(s.scale.history) ? s.scale.history : [],
    preferred = (s.scale && s.scale.preferredMac) || "",
    prev = sel.value,
    keep = sel.dataset.pending === "1",
    draft = sel.selectedOptions && sel.selectedOptions[0],
    list = document.createElement("select");
  if (!preferred || (keep && !prev)) {
    const first = document.createElement("option"),
      mode = s.config && s.config.scaleMacCacheMode;
    first.value = "";
    first.textContent =
      mode === "only" || mode === "prefer"
        ? __WEBUI_TEXT__("runtime.first_detected")
        : __WEBUI_TEXT__("runtime.no_preferred");
    list.appendChild(first);
  }
  const seen = {};
  history.forEach((e) => {
    const mac = e && e.mac;
    if (!mac || seen[mac]) return;
    seen[mac] = 1;
    const o = document.createElement("option");
    o.value = mac;
    o.dataset.name = (e.name && String(e.name).trim()) || "";
    o.textContent = scaleHistoryLabel(e);
    list.appendChild(o);
  });
  if (preferred && !seen[preferred]) {
    const o = document.createElement("option");
    o.value = preferred;
    o.dataset.name =
      (s.scale && s.scale.preferredName && String(s.scale.preferredName).trim()) || "";
    o.textContent =
      ((s.scale && s.scale.preferredName && String(s.scale.preferredName).trim()) ||
        __WEBUI_TEXT__("runtime.unknown_2")) +
      __WEBUI_TEXT__("runtime.unknown_3") +
      preferred;
    list.appendChild(o);
  }
  if (keep && prev && !seen[prev] && prev !== preferred && draft) {
    const o = document.createElement("option");
    o.value = prev;
    o.dataset.name = draft.dataset.name || "";
    o.textContent = draft.textContent;
    list.appendChild(o);
  }
  // Rebuild only on change: a poll must not close an open list.
  if (sel.innerHTML !== list.innerHTML) sel.innerHTML = list.innerHTML;
  sel.value = keep ? prev : preferred || "";
  R.updateScalePreferenceOptions();
  const opt = sel.selectedOptions && sel.selectedOptions[0];
  R.updateScaleRenameUi(
    "preferredScaleSelectRenameWrap",
    sel.value || "",
    opt && opt.dataset ? String(opt.dataset.name || "") : "",
  );
}
function brewWarningList() {
  // Suppress only the empty-field "is required" result: it fires mid-typing
  // and is self-evident. Everything else is a true statement about the
  // pending set as a whole and self-clears as typing completes.
  return R.validateBrewClient().filter((e) => !e.required);
}
function refreshBrewWarnings() {
  if (!$("goalWeightG")) return;
  const warnings = brewWarningList(),
    sig = warnings.map((e) => e.id + "\u0001" + e.msg).join("\u0002");
  // Unchanged signature ⇒ zero DOM writes: poll refreshes never flicker the
  // warnings and never disturb red save errors on other fields.
  if (sig === R.uiState.brewWarnSig) return;
  R.uiState.brewWarnSig = sig;
  R.clearBrewWarningsDom();
  for (const { id, msg } of warnings) {
    const el = $(id);
    if (!el) continue;
    el.classList.add("fieldWarn");
    el.setAttribute("aria-invalid", "true");
    const s = document.createElement("small");
    s.className = "fieldWarning";
    s.id = "brewWarn" + ++R.uiState.brewWarnSeq;
    s.textContent = msg;
    (el.closest("label") || el.parentElement).appendChild(s);
    // One field can carry several warnings; link them all like markFieldError.
    const prev = el.getAttribute("aria-describedby");
    el.setAttribute("aria-describedby", (prev ? prev + " " : "") + s.id);
  }
  const chip = $("brewConflictChip");
  if (chip) {
    chip.classList.toggle("hidden", !warnings.length);
    chip.textContent =
      warnings.length === 1
        ? __WEBUI_TEXT__("settings.conflict_will_block_saving").replace("{0}", 1)
        : warnings.length > 1
          ? __WEBUI_TEXT__("settings.conflicts_will_block_saving").replace("{0}", warnings.length)
          : "";
  }
}
// After a failed save, red brew errors hand over to the live yellow state on
// the next edit anywhere in the panel. Scoped to brew fields so machine
// config red errors survive brew keystrokes.

function clearBrewFieldErrors() {
  for (const el of R.settingsSectionEls("brew")) {
    el.classList.remove("invalid");
    const host = el.closest("label") || el.parentElement;
    host.querySelectorAll("small.fieldError").forEach((s) => s.remove());
    // Rebuild the aria pairing from the smalls that survive the handover:
    // the signature-skip renderer will not rewrite an unchanged set.
    R.refreshFieldAria(el);
  }
}
function validateMachineClient() {
  const m = document.documentElement.classList.contains("momentaryMachine");
  const reedMachine = m && document.documentElement.classList.contains("reedMachine");
  let e =
    R.rangeCheck("rinseGestureS", 0.1, 5, __WEBUI_TEXT__("runtime.rinse_gesture"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    R.rangeCheck("rinseDurationS", 0.5, 10, __WEBUI_TEXT__("runtime.rinse_duration"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    (m
      ? R.rangeCheck("stopPulseMs", 50, 1000, __WEBUI_TEXT__("runtime.auto_stop_pulse"), {
          int: 1,
        }) ||
        R.rangeCheck("maxSinglePressMs", 100, 5000, __WEBUI_TEXT__("runtime.single_press_limit"), {
          int: 1,
        }) ||
        (reedMachine
          ? R.rangeCheck(
              "reedConfirmTimeoutS",
              0.2,
              5,
              __WEBUI_TEXT__("runtime.reed_confirm_timeout"),
              { unit: __WEBUI_TEXT__("runtime.s") },
            )
          : "") ||
        // Reed builds ship no switch-only fields.
        ($("shotReactTimeoutS") &&
          R.rangeCheck(
            "shotReactTimeoutS",
            3,
            30,
            __WEBUI_TEXT__("runtime.shot_reaction_timeout"),
            {
              int: 1,
              unit: __WEBUI_TEXT__("runtime.s"),
            },
          ))
      : R.rangeCheck(
          "paddleReturnReminderMaxDurationMin",
          1,
          60,
          __WEBUI_TEXT__("runtime.paddle_limit"),
          { int: 1, unit: __WEBUI_TEXT__("runtime.min") },
        )) ||
    R.rangeCheck("retareWindowS", 0.5, 10, __WEBUI_TEXT__("runtime.retare_window"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    R.rangeCheck("postTareBaselineGraceS", 0.5, 10, __WEBUI_TEXT__("runtime.post_tare_grace"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    R.rangeCheck("minimumCupWeightG", 1, 500, __WEBUI_TEXT__("runtime.min_cup_weight"), {
      unit: __WEBUI_TEXT__("runtime.g"),
    }) ||
    R.rangeCheck("cupRemovedWeightG", -50, -0.1, __WEBUI_TEXT__("runtime.cup_removed"), {
      unit: __WEBUI_TEXT__("runtime.g"),
    }) ||
    R.rangeCheck("retareStabilitySamples", 2, 10, __WEBUI_TEXT__("runtime.retare_samples"), {
      int: 1,
    }) ||
    R.rangeCheck("retareStabilityToleranceG", 0.1, 20, __WEBUI_TEXT__("runtime.retare_tolerance"), {
      unit: __WEBUI_TEXT__("runtime.g"),
    }) ||
    R.rangeCheck("retareStabilityMaxGapS", 0.1, 5, __WEBUI_TEXT__("runtime.retare_sample_gap"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    R.rangeCheck(
      "retareStabilityMinDurationS",
      0,
      2,
      __WEBUI_TEXT__("runtime.retare_min_stable_time"),
      { unit: __WEBUI_TEXT__("runtime.s") },
    ) ||
    R.rangeCheck(
      "scaleTimerStopExtraDelayMs",
      0,
      1000,
      __WEBUI_TEXT__("runtime.scale_timer_stop_extra_delay"),
      { int: 1, unit: __WEBUI_TEXT__("runtime.ms") },
    ) ||
    R.rangeCheck("dripDelayS", 0, 10, __WEBUI_TEXT__("runtime.drip_delay"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    R.rangeCheck("lastShotCooldownMin", 5, 240, __WEBUI_TEXT__("runtime.last_shot_cooldown"), {
      int: 1,
      unit: __WEBUI_TEXT__("runtime.min"),
    });
  if (e) return e;
  const rg = R.number("rinseGestureS"),
    rd = R.number("rinseDurationS"),
    rw = R.number("retareWindowS"),
    wall = R.formNumber("operationalWallS"),
    samples = R.number("retareStabilitySamples"),
    gap = R.number("retareStabilityMaxGapS"),
    minStab = R.number("retareStabilityMinDurationS");
  if (Number.isFinite(wall)) {
    if (!m) {
      if (!(rg < wall))
        return {
          id: "rinseGestureS",
          msg:
            __WEBUI_TEXT__("runtime.rinse_gesture_must_be_machine_circuit_limit") + wall + " s).",
        };
      if (rd > wall)
        return {
          id: "rinseDurationS",
          msg:
            __WEBUI_TEXT__("runtime.rinse_duration_must_be_machine_circuit_limit") + wall + " s).",
        };
    }
    if (rw > wall)
      return {
        id: "retareWindowS",
        msg: __WEBUI_TEXT__("runtime.retare_window_must_be_machine_circuit_limit") + wall + " s).",
      };
  }
  if (minStab > rw)
    return {
      id: "retareStabilityMinDurationS",
      msg: __WEBUI_TEXT__("runtime.stable_time_must_fit_the_retare_window"),
    };
  if (minStab > 0 && minStab > samples * gap)
    return {
      id: "retareStabilityMinDurationS",
      msg: __WEBUI_TEXT__("runtime.stable_time_must_fit_samples_sample_gap"),
    };
  if ($("canTareStartTimer").checked && !$("autoTare").checked)
    return {
      id: "canTareStartTimer",
      msg: __WEBUI_TEXT__("runtime.bookoo_combination_requires_shot_start_tare"),
    };
  if (!m) {
    const iv = +$("paddleReturnReminder").value || 0,
      lim = $("paddleReturnReminderMaxDurationMin")
        ? R.number("paddleReturnReminderMaxDurationMin")
        : 0;
    if (lim * 60 < iv)
      return {
        id: "paddleReturnReminderMaxDurationMin",
        msg: __WEBUI_TEXT__("runtime.paddle_limit_must_be_reminder_interval") + iv + " s).",
      };
  }
  return null;
}
function machinePayload() {
  const reminder = $("paddleReturnReminder");
  return {
    preferredScaleMac: $("preferredScaleSelect")?.value,
    rinseEnabled: $("rinseEnabled").checked,
    rinseGestureMs: R.sToMs("rinseGestureS"),
    rinseDurationMs: R.sToMs("rinseDurationS"),
    retareWindowMs: R.sToMs("retareWindowS"),
    minimumCupWeightG: R.number("minimumCupWeightG"),
    cupRemovedWeightG: R.number("cupRemovedWeightG"),
    retareStabilitySamples: R.number("retareStabilitySamples"),
    retareStabilityToleranceG: R.number("retareStabilityToleranceG"),
    retareStabilityMaxGapMs: R.sToMs("retareStabilityMaxGapS"),
    retareStabilityMinDurationMs: R.sToMs("retareStabilityMinDurationS"),
    autoTare: $("autoTare").checked,
    autoTareOutsideBrew: $("autoTareOutsideBrew").checked,
    retareAccessoryOutsideBrew: $("retareAccessoryOutsideBrew").checked,
    postTareBaselineGraceMs: R.sToMs("postTareBaselineGraceS"),
    scaleMacCacheMode: ["first", "prefer", "only"].includes($("scalePreference")?.value)
      ? $("scalePreference").value
      : "only",
    paddleMode: $("paddleMode")
      ? ["auto", "natural", "original"].includes($("paddleMode").value)
        ? $("paddleMode").value
        : "natural"
      : undefined,
    stopPulseMs: $("stopPulseMs") ? R.number("stopPulseMs") : undefined,
    maxSinglePressMs: $("maxSinglePressMs") ? R.number("maxSinglePressMs") : undefined,
    momentaryStartEdge: $("momentaryStartEdge")
      ? ["press", "release"].includes($("momentaryStartEdge").value)
        ? $("momentaryStartEdge").value
        : "press"
      : undefined,
    reedConfirmTimeoutMs: $("reedConfirmTimeoutS") ? R.sToMs("reedConfirmTimeoutS") : undefined,
    assumeIdleWhenScaleConnects: $("assumeIdleWhenScaleConnects")?.checked ?? true,
    shotReactTimeoutS: $("shotReactTimeoutS") ? R.number("shotReactTimeoutS") || 0 : undefined,
    canTareStartTimer: $("canTareStartTimer").checked,
    bookooMuteOnBuzzerOnly: $("bookooMuteOnBuzzerOnly").checked,
    bookooConnectBeepLevel: Number($("bookooConnectBeepLevel").value),
    scaleTimerStopExtraDelayMs: R.number("scaleTimerStopExtraDelayMs"),
    dripDelayMs: R.sToMs("dripDelayS"),
    soundAlertsEnabled: $("soundAlertsEnabled").checked,
    firstDropBeep: $("firstDropBeep").checked,
    scaleConnectedLed: $("scaleConnectedLed").checked,
    paddleReturnReminderBeep: reminder ? !!+reminder.value : undefined,
    buzzerScaleLostBeep: $("buzzerScaleLostBeep").checked,
    buzzerAutoToManualGuardEndBeep: $("buzzerAutoToManualGuardEndBeep").checked,
    buzzerManualNoScaleBeep: $("buzzerManualNoScaleBeep").checked,
    buzzerScaleConnectedBeep: $("buzzerScaleConnectedBeep").checked,
    buzzerExtendedPulseRate: R.extRate($("buzzerExtendedPulseRate").value),
    buzzerSlowExtendedPulseRate: R.extRate($("buzzerSlowExtendedPulseRate").value),
    alertOutputChannel: ["scale_only", "buzzer_only", "scale_priority"].includes(
      $("alertOutputChannel").value,
    )
      ? $("alertOutputChannel").value
      : "scale_priority",
    autoRetare: $("autoRetare").checked,
    paddleReturnReminderIntervalMs: reminder ? (+reminder.value || 30) * 1000 : undefined,
    paddleReturnReminderMaxDurationMs: reminder
      ? Math.round(R.number("paddleReturnReminderMaxDurationMin") * 60000)
      : undefined,
    noScaleBbwMode: $("noScaleBbwMode").value,
    noScaleAllowRinseWhileArmed: $("noScaleAllowRinseWhileArmed").checked,
    lastShotCooldownMs: Math.round(R.number("lastShotCooldownMin") * 6e4),
    ...Object.fromEntries(
      [
        "cupProtectionEnabled",
        "stopIfCupRemoved",
        "requireCupToStart",
        "avoidAccidentalTouchEnabled",
        "touchStopFallbackEnabled",
        "autoToManualGuardEnabled",
      ].map((k) => [k, $(k).checked]),
    ),
  };
}
function addBullseyePayload(p) {
  p.bullseyeMelodyEnabled = $("bullseyeMelodyEnabled").checked;
  p.bullseyeRtttl = $("bullseyeRtttl").value;
  return p;
}
function validateBullseyeClient() {
  const text = $("bullseyeRtttl").value;
  if (text.length > 500)
    return {
      id: "bullseyeRtttl",
      msg: __WEBUI_TEXT__("runtime.rtttl_must_be_at_most_500_characters"),
    };
  return null;
}
async function saveMachineConfig(patch) {
  const fail = __WEBUI_TEXT__("runtime.could_not_save_shared_settings"),
    sel = $("preferredScaleSelect");
  for (let retry = true; ; retry = false)
    try {
      await R.command("/api/v1/config", R.withBaseRev(patch), 1, "", fail);
      if (patch.preferredScaleMac !== undefined) {
        const applied = R.homeStatusCache()?.scale?.preferredMac || "";
        if (applied.toUpperCase() !== patch.preferredScaleMac.toUpperCase())
          throw new Error(__WEBUI_TEXT__("runtime.device_did_not_apply_the_change"));
        sel.dataset.pending = "0";
      }
      return true;
    } catch (e) {
      // The patch holds only this page's edits: resend it once on the newer
      // revision the Home stream brings (within 2 s).
      if (retry && e.code === "CONFIG_REVISION_STALE") {
        for (let i = 0; i < 20 && R.homeStatusCache()?.config.revision === patch.baseRevision; i++)
          await new Promise((r) => setTimeout(r, 100));
        continue;
      }
      // Only the firmware parses RTTTL: a rejected tune is marked, not kept.
      if (e.code === "INVALID_FIELD" && "bullseyeRtttl" in patch)
        R.showFieldError("bullseyeRtttl", __WEBUI_TEXT__("settings.rtttl_not_saved"));
      else R.message(R.formatCommandError(fail, e), "error");
      return false;
    }
}
const MICRA_SWITCHES = [
  "ApplyTemperature",
  "ObserveState",
  "BoostScaleDetectionWhenOn",
  "ReduceScaleScanningWhenOff",
  "RecognizeWake",
  "PowerOnWithScale",
  "ShutdownWithScale",
  "ScaleOffWithMachine",
];
function micraOptionField(id) {
  return id[0].toLowerCase() + id.slice(1) + (id === "RecognizeWake" ? "Gesture" : "");
}
// Shots and scale activity cancel cloud requests routinely: not an error.
function micraErrorText(error) {
  if (error === "none" || error === "canceled") return "";
  return (
    {
      invalid_auth: __WEBUI_TEXT__("runtime.micra_error_auth"),
      no_machines: __WEBUI_TEXT__("runtime.micra_error_no_machines"),
      clock_unsynchronized: __WEBUI_TEXT__("runtime.micra_error_clock"),
    }[error] || __WEBUI_TEXT__("runtime.micra_error_cloud")
  );
}
function renderMicraMachines(machines, canEdit) {
  const list = $("lineaMicraMachines"),
    key = machines.map((x) => x.serial).join();
  if (list.dataset.key !== key) {
    list.dataset.key = key;
    list.replaceChildren(
      ...machines.map((x) => {
        const label = document.createElement("label");
        label.className = "setupChoice";
        label.innerHTML =
          '<input type="radio" name="lineaMicraMachine"><span><b></b><small></small></span>';
        label.firstChild.value = x.serial;
        label.querySelector("b").textContent = x.name || x.serial;
        label.querySelector("small").textContent = x.serial;
        return label;
      }),
    );
  }
  list.querySelectorAll("input").forEach((r) => (r.disabled = !canEdit));
  $("lineaMicraSelectButton").disabled = !canEdit || !list.querySelector("input:checked");
}
function applyLineaMicraStatus(s) {
  const m = __LINEA_MICRA__ && s.lineaMicra;
  if (!m) return;
  document.documentElement.classList.toggle(
    "micraTemperatureEnabled",
    m.applyTemperature && m.accountConfigured,
  );
  const machines = m.machines || [],
    canEdit = R.controlsMutable,
    connected = m.accountConfigured,
    signingIn = !connected && ["queued", "authenticating", "listing"].includes(m.phase),
    changing = connected && R.uiState.micraChange,
    offline = !m.staConnected || m.apActive,
    error = micraErrorText(m.error),
    listing = changing && m.machineListing,
    shown = changing ? micraErrorText(m.machineListError) : connected ? "" : error;
  if (!R.uiState.micraDirty) {
    for (const id of MICRA_SWITCHES) $("lineaMicra" + id).checked = !!m[micraOptionField(id)];
    $("lineaMicraShutdownGrace").value = String(m.shutdownGraceSeconds || 0);
  }
  R.uiState.micraChange = changing;
  // A lone listed machine is saved with the options above, once per page;
  // if that fails the choice pane takes over.
  if (connected) R.uiState.micraSignIn = null;
  else if (
    !signingIn &&
    machines.length === 1 &&
    !["linking", "failed"].includes(R.uiState.micraSignIn)
  ) {
    R.uiState.micraSignIn = "linking";
    selectLineaMicra(machines[0].serial);
  } else if (R.uiState.micraSignIn === "pending" && !signingIn && error) {
    R.uiState.micraSignIn = null;
    $("lineaMicraPassword").focus();
  }
  const linking = R.uiState.micraSignIn === "linking" && !connected && machines.length === 1,
    busy = signingIn || linking,
    choosing = changing || (!connected && !busy && machines.length > 0),
    stage = linking ? 2 : m.phase === "listing" ? 1 : 0,
    target = m.boiler?.coffee?.targetDeciC;
  $("lineaMicraLegendHint").textContent =
    connected && !changing
      ? __WEBUI_TEXT__("runtime.micra_state_connected")
      : busy
        ? __WEBUI_TEXT__("runtime.micra_state_connecting")
        : choosing
          ? __WEBUI_TEXT__("runtime.micra_state_choose")
          : __WEBUI_TEXT__("runtime.not_connected");
  [...$("lineaMicraSteps").children].forEach(
    (li, i) => (li.className = i < stage ? "ok" : i > stage ? "" : "on"),
  );
  $("lineaMicraBusy").hidden = !busy;
  $("lineaMicraSignIn").hidden = connected || choosing || busy;
  $("lineaMicraChoose").hidden = !choosing;
  $("lineaMicraConnected").hidden = !connected || changing;
  $("lineaMicraWifiNote").classList.toggle("hidden", !offline);
  $("lineaMicraConnectButton").disabled = !canEdit || offline;
  $("lineaMicraConnectButton").classList.toggle("busy", busy);
  $("lineaMicraProgress").textContent = listing
    ? __WEBUI_TEXT__("runtime.micra_progress_listing")
    : shown;
  $("lineaMicraProgress").classList.toggle("error", !listing && !!shown);
  renderMicraMachines(machines, canEdit);
  $("lineaMicraCancelButton").disabled = !canEdit;
  $("lineaMicraIdentity").innerText = connected ? m.selectedName + " · " + m.selectedSerial : "";
  $("lineaMicraAccount").innerText = connected ? m.email : "";
  // Most actionable first: account, link, then the machine itself.
  const pill = $("lineaMicraStatus"),
    power = m.optimisticOn ? "ON" : m.optimisticOff ? "OFF" : m.powerState,
    [label, tone, warm] = ["invalid_auth", "no_machines"].includes(m.error)
      ? [__WEBUI_TEXT__("runtime.micra_pill_account"), "error"]
      : m.apActive
        ? [__WEBUI_TEXT__("runtime.ap_mode_paused"), "warn"]
        : m.error === "clock_unsynchronized"
          ? [__WEBUI_TEXT__("runtime.micra_pill_clock"), "warn"]
          : error || m.quality === "communication_error"
            ? [__WEBUI_TEXT__("runtime.micra_pill_cloud"), "warn"]
            : !m.observeState
              ? [__WEBUI_TEXT__("runtime.micra_pill_monitoring_off"), ""]
              : m.machineOffline
                ? [__WEBUI_TEXT__("runtime.micra_pill_offline"), "warn"]
                : m.readiness === "needs_water"
                  ? [__WEBUI_TEXT__("runtime.micra_pill_no_water"), "error"]
                  : power === "OFF"
                    ? [__WEBUI_TEXT__("runtime.micra_power_standby"), ""]
                    : power !== "ON"
                      ? [__WEBUI_TEXT__("runtime.micra_power_unknown"), ""]
                      : {
                          warming_up: [__WEBUI_TEXT__("runtime.micra_pill_heating"), "warn", 1],
                          waiting_for_steam: [
                            __WEBUI_TEXT__("runtime.micra_pill_steam"),
                            "warn",
                            1,
                          ],
                          ready: [__WEBUI_TEXT__("runtime.micra_pill_ready"), "on", 1],
                        }[m.readiness] || [__WEBUI_TEXT__("runtime.micra_power_on"), "on", 1];
  pill.textContent = [
    label,
    warm && target ? __WEBUI_TEXT__("runtime.target") + " " + (target / 10).toFixed(1) + " °C" : "",
    m.shotPaused ? __WEBUI_TEXT__("runtime.shot_paused") : "",
  ]
    .filter(Boolean)
    .join(" · ");
  pill.dataset.tone = tone;
  pill.title = error;
  $("lineaMicraDisconnectButton").disabled = !canEdit;
  $("lineaMicraChangeButton").disabled = !canEdit || offline;
  for (const id of MICRA_SWITCHES) $("lineaMicra" + id).disabled = !canEdit;
  updateMicraDependentControls();
}
function updateMicraDependentControls() {
  const canEdit = R.controlsMutable,
    deps = $("lineaMicraPowerDeps"),
    observe = $("lineaMicraObserveState").checked,
    on = $("lineaMicraShutdownWithScale").checked;
  deps.classList.toggle("fieldOff", !observe);
  deps.querySelectorAll("input").forEach((i) => (i.disabled = !canEdit || !observe));
  $("lineaMicraShutdownGrace").disabled = !canEdit || !on;
  $("lineaMicraShutdownGraceWrap").classList.toggle("hidden", !on);
}
async function connectLineaMicra() {
  const username = $("lineaMicraUsername").value.trim(),
    password = $("lineaMicraPassword").value;
  if (!username || !$("lineaMicraUsername").validity.valid) {
    R.showFieldError("lineaMicraUsername", __WEBUI_TEXT__("runtime.enter_valid_email"));
    return false;
  }
  if (!password) {
    R.showFieldError("lineaMicraPassword", __WEBUI_TEXT__("runtime.enter_password"));
    return false;
  }
  $("lineaMicraConnectButton").classList.add("busy");
  const ok = await R.command(
    "/api/v1/machine/linea-micra",
    { action: "connect", username, password },
    false,
    __WEBUI_TEXT__("runtime.micra_connection_queued"),
    __WEBUI_TEXT__("runtime.micra_action_failed"),
  );
  $("lineaMicraPassword").value = "";
  R.uiState.micraSignIn = ok ? "pending" : null;
  return ok;
}
async function selectLineaMicra(serial) {
  const ok = await saveLineaMicraSettings("select", serial);
  if (ok) $("lineaMicraLinkedNote").hidden = false;
  else R.uiState.micraSignIn = "failed";
  if (ok && R.uiState.micraChange) endMicraChange();
  return ok;
}
// Back to the saved machine; nothing was changed on the device.
function endMicraChange() {
  R.uiState.micraChange = false;
  $("lineaMicraChoose").hidden = !($("lineaMicraConnected").hidden = false);
  $("lineaMicraProgress").textContent = "";
}
function micraPayload() {
  return {
    ...Object.fromEntries(
      MICRA_SWITCHES.map((id) => [micraOptionField(id), $("lineaMicra" + id).checked]),
    ),
    shutdownGraceSeconds: Number($("lineaMicraShutdownGrace").value) || 0,
  };
}
// Select sends every option with the chosen machine; save is a patch that
// keeps the saved transport.
function saveLineaMicraSettings(action, serial, options = micraPayload()) {
  return R.command(
    "/api/v1/machine/linea-micra",
    { action, ...options, ...(action === "select" ? { serial, connectionType: "websocket" } : {}) },
    false,
    action === "select" ? __WEBUI_TEXT__("runtime.micra_settings_queued") : "",
    __WEBUI_TEXT__("runtime.could_not_save_micra"),
  );
}
async function disconnectLineaMicra() {
  const ok = await R.lineaMicraAction("disconnect");
  if (ok) {
    $("lineaMicraUsername").value = "";
    $("lineaMicraPassword").value = "";
    R.uiState.micraDirty = false;
  }
  return ok;
}
function loadBullseyeConfig(c) {
  if (!c || R.uiState.configDirty || !$("bullseyeMelodyEnabled")) return;
  // A toggle switched on keeps waiting while its tune is empty.
  if (c.bullseyeMelodyEnabled || $("bullseyeRtttl").value.trim())
    $("bullseyeMelodyEnabled").checked = !!c.bullseyeMelodyEnabled;
  $("bullseyeRtttl").value = typeof c.bullseyeRtttl === "string" ? c.bullseyeRtttl : "";
  R.updateBullseyeControls();
}
function applySettingsStatus(s) {
  R.uiState.bbwReadback = { ...s.config, ...s.config.bbwDrip };
  updatePreferredScaleSelect(s);
  const sc = s.scale || {};
  if ($("forgetPairedScale"))
    $("forgetPairedScale").disabled = !(
      R.controlsMutable &&
      (sc.preferredMac || sc.macCachePauseRemainingMs > 0)
    );
  R.loadSettingsConfig(s.config);
  loadBullseyeConfig(s.config);
  applyLineaMicraStatus(s);
  R.updateBbwControls();
  const sl = s.scaleLoss,
    active = R.presetState.items.find((x) => x.id === R.presetState.activeId);
  if (sl && active)
    $("scaleLossProjection").textContent = R.sub(__WEBUI_TEXT__("runtime.scale_loss_projection"), [
      active.name,
      +sl.projectedS.toFixed(1),
      {
        history: __WEBUI_TEXT__("runtime.scale_loss_from_history"),
        window: __WEBUI_TEXT__("runtime.scale_loss_from_window"),
        fixed: __WEBUI_TEXT__("runtime.scale_loss_from_fixed"),
      }[sl.source],
    ]);
  // Every section bases its next edit on the shown values; a settled one
  // also drops what it had pending.
  if (R.configLoaded)
    for (const [sec, a] of Object.entries(autosave))
      if (!a.busy) {
        if (!R.uiState[sec + "Dirty"]) {
          clearTimeout(a.timer);
          a.base = {};
        }
        a.last = a.payload();
      }
  refreshBrewWarnings();
}
async function forgetPairedScale() {
  if (!confirm(__WEBUI_TEXT__("runtime.clear_the_preferred_scale_discovery_pauses_for"))) return;
  await R.command("/api/v1/scale/preferred/clear", {});
}
function selectPreferredScale() {
  const sel = $("preferredScaleSelect");
  if (!sel) return;
  const mac = sel.value || "";
  sel.dataset.pending = "1";
  const opt = sel.selectedOptions && sel.selectedOptions[0];
  R.updateScaleRenameUi(
    "preferredScaleSelectRenameWrap",
    mac,
    opt && opt.dataset ? String(opt.dataset.name || "") : "",
  );
}
// Each section autosaves its patch (R.noteEdit) for the firmware to merge and persist.
const autosave = {
  config: {
    // The toggle and its tune save together: on without a tune is a draft
    // that saves with its first tune.
    payload() {
      const p = addBullseyePayload(machinePayload());
      p.bullseyeMelodyEnabled &&= !!p.bullseyeRtttl.trim();
      return p;
    },
    valid() {
      const err = validateMachineClient() || validateBullseyeClient();
      if (err) R.showFieldError(err.id, err.msg);
      return !err;
    },
    save: saveMachineConfig,
  },
  // Conflicts already show as live warnings; the patch waits for them.
  brew: {
    payload: R.brewPayload,
    valid: () => !R.validateBrewClient().length,
    save: (patch) => R.saveBrewPreset(patch, ""),
  },
  ...(__LINEA_MICRA__ && {
    micra: {
      payload: micraPayload,
      save: (patch) => saveLineaMicraSettings("save", "", patch),
    },
  }),
};
for (const [sec, a] of Object.entries(autosave)) R.autosaveSection(sec, a);
async function flushSettings() {
  const done = await Promise.all(Object.values(autosave).map((a) => R.flushEdits(a, true)));
  if (done.every(Boolean)) return true;
  R.message(__WEBUI_TEXT__("settings.settings_not_saved_yet"), "error");
  return false;
}
let ready = false;
export function applyStatus(s) {
  R.updateBuzzerAlertVisibility(s.buzzerSupported);
  applySettingsStatus(s);
  updateSettingsOverrides(s);
  updateCupMinGuidance(s);
  const reminder = $("paddleReturnReminder");
  if (reminder && !R.uiState.configDirty && !R.uiState.brewDirty) {
    // A stored interval the list lacks gets its own entry instead of being
    // snapped to another value, which would also mark the form edited.
    const v = String(
      s.config.paddleReturnReminderBeep ? s.config.paddleReturnReminderIntervalMs / 1000 : 0,
    );
    if (![...reminder.options].some((o) => o.value === v)) reminder.add(new Option(v, v));
    reminder.value = v;
    if (R.uiState.formBaseline) R.uiState.formBaseline.paddleReturnReminder = v;
  }
  const link = document.querySelector('[data-route="/diagnostic"]');
  if (link && typeof s.diagnosticPageVisible === "boolean")
    link.classList.toggle("hidden", !s.diagnosticPageVisible);
  // Deep link (Home's Micra hint): open the group once it is laid out.
  const group = location.hash && document.getElementById(location.hash.slice(1));
  if (group?.offsetParent) {
    history.replaceState(null, "", location.pathname);
    group.open = true;
    group.scrollIntoView({ block: "start" });
  }
}

// Settings shows saved values; a note flags the ones a header-panel override
// currently hides (mask bits follow the firmware QuickSettingField order).
const QUICK_FIELDS = [
  "brewByWeight",
  "noScaleBbwMode",
  "autoToManualGuardEnabled",
  "slowExtractionGuardEnabled",
  "fastExtractionGuardEnabled",
  "avoidAccidentalTouchEnabled",
  "cupProtectionEnabled",
];
let overrideStatus = {};
function updateSettingsOverrides(s = overrideStatus) {
  overrideStatus = s;
  const mask = s.quickRevert?.overrides | 0,
    activePreset = (R.presetState.selectedId || R.presetState.activeId) === R.presetState.activeId;
  QUICK_FIELDS.forEach((k, bit) => {
    const label = $(k)?.closest("label");
    if (!label) return;
    let note = label.nextElementSibling?.classList.contains("qsOverride")
      ? label.nextElementSibling
      : null;
    const presetField = bit === 0 || bit === 3 || bit === 4;
    if (!(mask & (1 << bit)) || (presetField && !activePreset)) return note?.remove();
    if (!note) {
      note = document.createElement("p");
      note.className = "fieldWarning qsOverride";
      note.innerHTML = `${R.escHtml(__WEBUI_TEXT__("settings.quick_override_note"))} <button type="button" class="qsReset" data-saved="${k}">${R.escHtml(__WEBUI_TEXT__("settings.quick_override_use_saved"))}</button>`;
      label.after(note);
    }
    note.querySelector("button").disabled = !R.controlsMutable || !!s.cycle?.active;
  });
}
// Sending the saved value (the baseline, not a draft edit) drops that override.
document.addEventListener("click", async (e) => {
  const button = e.target.closest?.(".qsOverride button[data-saved]");
  if (!button) return;
  const input = $(button.dataset.saved),
    saved = R.uiState.formBaseline?.[input.id];
  button.disabled = true;
  try {
    await R.api("/api/v1/integration/quick-settings", {
      method: "PUT",
      body: R.body({
        baseRevision: overrideStatus.config?.revision,
        [input.id]: saved ?? (input.type === "checkbox" ? input.checked : input.value),
      }),
    });
  } catch (err) {
    R.message(err.message, "error");
    button.disabled = false;
  }
});

// Setup help: something rests on the platform but reads below the configured
// cup minimum, so cup detection and late tare will not see it as a cup. It
// follows the live weight on the Home stream.
function updateCupMinGuidance(s) {
  const hint = $("cupMinGuidance");
  if (!hint) return;
  const observed = typeof s.scale?.observedWeightG === "number" ? s.scale.observedWeightG : null,
    min = R.number("minimumCupWeightG");
  const show = observed !== null && Number.isFinite(min) && observed > 0.5 && observed < min;
  hint.hidden = !show;
  if (show)
    hint.textContent = __WEBUI_TEXT__("settings.cup_min_guidance")
      .replaceAll("{x}", observed.toFixed(1))
      .replace("{n}", min.toFixed(1));
}
// What each newest shot needed against the drip in use, oldest bar first.
// Markers show where the owner set the drip and where quick start decided.
function renderDripChart(c) {
  const dripText = (g) => (g > 0 ? g.toFixed(2) : "0") + __WEBUI_TEXT__("runtime.g_2"),
    dripRange = (v) =>
      (Math.min(...v) > 0 ? Math.min(...v).toFixed(2) : "0") + "–" + dripText(Math.max(...v));
  const recent = (c && c.bbwRecentG) || [],
    q = (c && c.bbwQuickStart) || {},
    n = recent.length,
    use = (c && c.bbwEwmaOffsetG) || 0,
    armed = q.state === "armed",
    afterSet =
      c && c.bbwRecentAfterSet >= 0 && (c.bbwRecentAfterSet < n || n < 10)
        ? c.bbwRecentAfterSet
        : -1,
    afterQuick =
      c && c.bbwRecentAfterQuick >= 0 && c.bbwRecentAfterQuick < n ? c.bbwRecentAfterQuick : -1,
    mode = armed
      ? "quick"
      : afterQuick < 0
        ? "learning"
        : q.state === "applied"
          ? "applied"
          : "varied",
    marked = mode !== "learning",
    shots = recent
      .slice(0, armed ? 9 : 10)
      .map((g, i) => ({ g, i }))
      .reverse(),
    counted = (s) =>
      marked && s.g != null && (afterSet < 0 || s.i < afterSet) && s.i >= Math.max(afterQuick, 0),
    old = (s) => marked && afterSet >= 0 && s.i >= afterSet,
    pad = 10 - shots.length - (armed ? 1 : 0),
    top = Math.max(...recent.filter((g) => g != null), use, 0.5) * 1.3,
    h = (g) => (Math.max(0, g) / top) * 100 + "%",
    slots = [...Array(pad).fill({}), ...shots, ...(armed ? [{ next: true }] : [])],
    at = (after) => pad + shots.length - Math.min(after, shots.length),
    marks = [];
  if (marked && afterSet >= 0)
    marks.push([at(afterSet), __WEBUI_TEXT__("runtime.drip_manual_set") + dripText(q.setG), ""]);
  if (mode === "applied")
    marks.push([
      at(afterQuick),
      __WEBUI_TEXT__("runtime.drip_quick_set") + dripText(q.quickG),
      " end",
    ]);
  // Built with element.style: the firmware CSP drops style attributes in markup.
  const el = (tag, cls, ...kids) => {
    const e = document.createElement(tag);
    e.className = cls;
    e.append(...kids);
    return e;
  };
  const bars = $("dripBars");
  bars.classList.toggle("twoMarks", marks.length > 1);
  bars.replaceChildren(
    ...slots.map((s) => {
      const bar = el(
        "div",
        "dcBar" + (old(s) ? " old" : "") + (counted(s) ? " qs" : "") + (s.next ? " next" : ""),
      );
      if (s.next || s.g != null) {
        const fill = el("i", "");
        if (!s.next) fill.style.height = h(s.g);
        bar.append(fill);
      }
      return bar;
    }),
    ...marks.map(([k, label, end], i) => {
      const mark = el(
        "div",
        "dcMark" + end + (marks.length > 1 && !i ? " up" : ""),
        el("span", "", label),
      );
      mark.style.left = `calc((100% + .5rem) * ${k} / 10 - .25rem)`;
      return mark;
    }),
  );
  if (c) {
    const line = el(
      "div",
      "dcUse",
      el("span", "", __WEBUI_TEXT__("runtime.drip_in_use"), el("br", ""), dripText(use)),
    );
    line.style.bottom = h(use);
    bars.append(line);
  }
  const last = slots.length - 1 - (armed ? 1 : 0);
  $("dripBarX").innerHTML = slots
    .map(
      (s, k) =>
        `<span class="${old(s) ? "old" : ""}">` +
        (s.g != null
          ? `<b>${dripText(s.g)}</b>`
          : "i" in s
            ? __WEBUI_TEXT__("runtime.drip_skipped")
            : s.next
              ? __WEBUI_TEXT__("runtime.drip_next")
              : "") +
        (k === last && "i" in s ? __WEBUI_TEXT__("runtime.drip_last_shot") : "") +
        "</span>",
    )
    .join("");
  const needed = recent.filter((g, i) => g != null && (mode === "learning" || counted({ g, i })));
  const sorted = [...needed].sort((a, b) => a - b);
  const lead = __WEBUI_TEXT__("runtime.drip_last_needed");
  const verdict = !needed.length
    ? ""
    : mode === "quick"
      ? __WEBUI_TEXT__("runtime.drip_need_about") + dripText(sorted[(sorted.length - 1) >> 1])
      : mode === "varied"
        ? __WEBUI_TEXT__("runtime.drip_varied") +
          dripRange(needed) +
          __WEBUI_TEXT__("runtime.drip_varied_end")
        : mode === "applied"
          ? ""
          : use >= sorted[0] - 0.15 && use <= sorted[sorted.length - 1] + 0.15
            ? __WEBUI_TEXT__("runtime.drip_on_track") + lead + dripRange(needed)
            : lead.charAt(0).toUpperCase() + lead.slice(1) + dripRange(needed);
  $("dripVerdict").textContent = verdict;
  $("dripVerdict").hidden = !verdict;
  $("dripHint").textContent = !n
    ? __WEBUI_TEXT__("runtime.drip_hint_none")
    : {
        learning: __WEBUI_TEXT__("runtime.drip_hint_learning"),
        quick: __WEBUI_TEXT__("runtime.drip_hint_quick"),
        applied: __WEBUI_TEXT__("runtime.drip_hint_applied"),
        varied: __WEBUI_TEXT__("runtime.drip_hint_varied"),
      }[mode];
  $("dripStatus").className = "dcStatus" + (armed || mode === "applied" ? " quick" : "");
  $("dripStatus").lastElementChild.textContent = armed
    ? __WEBUI_TEXT__("runtime.drip_status_quick") +
      (q.count || 0) +
      __WEBUI_TEXT__("runtime.drip_status_quick_of")
    : mode === "applied"
      ? __WEBUI_TEXT__("runtime.drip_status_applied")
      : __WEBUI_TEXT__("runtime.drip_status_learning");
}
// Each group row summarizes its current values and follows the form while it
// is edited; keys are the groups' data-g names.
const on = (id) => !!$(id)?.checked,
  val = (id) => ($(id)?.value.trim() ? String(+$(id).value) : "?"),
  picked = (id) => $(id)?.selectedOptions[0]?.textContent.trim() || "",
  OFF = __WEBUI_TEXT__("settings.disabled"),
  ON = __WEBUI_TEXT__("runtime.on_2");
const GROUP_SUMMARIES = {
  recipe: () =>
    on("brewByWeight")
      ? R.sub(__WEBUI_TEXT__("settings.sum_recipe"), [
          val("doseG"),
          val("goalWeightG"),
          R.brewRatio(R.number("goalWeightG"), R.number("doseG")),
        ])
      : __WEBUI_TEXT__("runtime.manual"),
  fast: () =>
    on("fastExtractionGuardEnabled")
      ? R.sub(__WEBUI_TEXT__("settings.sum_extend"), [val("fastMarginG")])
      : OFF,
  slow: () =>
    on("slowExtractionGuardEnabled")
      ? R.sub(__WEBUI_TEXT__("settings.sum_shorten"), [val("slowMarginG")])
      : OFF,
  limits: () => R.sub(__WEBUI_TEXT__("settings.sum_max_s"), [val("operationalWallS")]),
  drip: () => {
    const g = $("learnedOffsetG").textContent;
    if (!on("brewByWeight")) return __WEBUI_TEXT__("settings.sum_not_used");
    return Number.isFinite(parseFloat(g)) ? g + __WEBUI_TEXT__("runtime.g_2") : "";
  },
  account: () => __WEBUI_TEXT__("settings.sum_signed_in"),
  power: () => {
    if (!on("lineaMicraObserveState")) return OFF;
    const deps = [...$("lineaMicraPowerDeps").querySelectorAll("input")],
      n = deps.filter((i) => i.checked).length;
    return n === deps.length
      ? __WEBUI_TEXT__("settings.sum_all_on")
      : R.sub(__WEBUI_TEXT__("settings.sum_n_of_m"), [n + 1, deps.length + 1]);
  },
  auto: () =>
    [
      on("lineaMicraPowerOnWithScale") && __WEBUI_TEXT__("settings.sum_wake"),
      on("lineaMicraShutdownWithScale") && __WEBUI_TEXT__("settings.sum_standby"),
    ]
      .filter(Boolean)
      .join(" + ") || OFF,
  micraPresets: () =>
    on("lineaMicraApplyTemperature") ? __WEBUI_TEXT__("settings.sum_sets_temperature") : OFF,
  paddle: () => picked("paddleMode"),
  switch: () =>
    $("momentaryStartEdge").value === "release"
      ? __WEBUI_TEXT__("settings.sum_on_release")
      : __WEBUI_TEXT__("settings.sum_on_press"),
  rinse: () =>
    on("rinseEnabled")
      ? R.sub(__WEBUI_TEXT__("settings.sum_seconds"), [val("rinseDurationS")])
      : OFF,
  scale: () => picked("scalePreference"),
  bookoo: () =>
    $("bookooConnectBeepLevel").value === "0"
      ? __WEBUI_TEXT__("settings.sum_silent")
      : R.sub(__WEBUI_TEXT__("settings.sum_volume"), [$("bookooConnectBeepLevel").value]),
  tare: () => {
    const cup = on("autoTareOutsideBrew"),
      start = on("autoTare");
    if (cup && start) return __WEBUI_TEXT__("settings.sum_tare_both");
    if (cup) return __WEBUI_TEXT__("settings.sum_tare_cup");
    return start ? __WEBUI_TEXT__("settings.sum_tare_start") : OFF;
  },
  cup: () => R.sub(__WEBUI_TEXT__("settings.sum_min_g"), [val("minimumCupWeightG")]),
  protect: () => {
    const touch = on("avoidAccidentalTouchEnabled");
    if (on("cupProtectionEnabled")) return touch ? __WEBUI_TEXT__("settings.sum_on_touch") : ON;
    return touch ? __WEBUI_TEXT__("settings.sum_touch_only") : OFF;
  },
  noScale: () =>
    ({
      off: __WEBUI_TEXT__("settings.allow_manual_brewing"),
      warn_once: __WEBUI_TEXT__("settings.sum_warn_once"),
      require_scale: __WEBUI_TEXT__("settings.require_a_scale"),
    })[$("noScaleBbwMode").value],
  finish: () => (on("autoToManualGuardEnabled") ? ON : OFF),
  alerts: () => {
    const channel = $("alertOutputChannel");
    if (!on("soundAlertsEnabled")) return OFF;
    if (channel.closest(".buzzerOpt.hidden")) return ON;
    return channel.value === "scale_priority"
      ? __WEBUI_TEXT__("settings.sum_scale_then_buzzer")
      : picked("alertOutputChannel");
  },
};
export function init() {
  if (ready) return;
  ready = true;
  R.initGroupRows($("workflowPanel"));
  R.registerViewStatus("settings", applyStatus);
  R.registerViewStatus("settingsLive", updateCupMinGuidance);
  $("presetCards")?.addEventListener("click", () => setTimeout(updateSettingsOverrides));
  R.uiState.flushSettings = flushSettings;
  if ($("presetNewBtn"))
    $("presetNewBtn").onclick = async () => {
      if (!(await flushSettings())) return;
      R.invalidateSettingsHydration();
      R.command("/api/v1/presets", { action: "new" });
    };
  if ($("presetDupBtn"))
    $("presetDupBtn").onclick = async () => {
      if (!(await flushSettings())) return;
      R.invalidateSettingsHydration();
      R.command("/api/v1/presets", {
        action: "duplicate",
        id: R.presetState.selectedId || R.presetState.activeId,
      });
    };
  if ($("presetResetBtn"))
    $("presetResetBtn").onclick = async () => {
      const p = R.selectedPreset();
      if (!p || !p.isFactory) return;
      if (
        !(await R.askConfirm(
          R.sub(__WEBUI_TEXT__("settings.restore_confirm"), [p.name]),
          __WEBUI_TEXT__("settings.restore"),
        ))
      )
        return;
      if (R.uiState.brewDirty) R.clearBrewDirty();
      R.invalidateSettingsHydration();
      R.command("/api/v1/presets", { action: "restore_factory_values", id: p.id });
    };
  if ($("presetNewCoffeeBtn"))
    $("presetNewCoffeeBtn").onclick = () => R.startNewCoffee(R.selectedPreset().id);
  if ($("presetDeleteBtn"))
    $("presetDeleteBtn").onclick = async () => {
      const p = R.selectedPreset();
      if (!p || p.isFactory) return;
      if (
        await R.askConfirm(
          R.sub(__WEBUI_TEXT__("settings.delete_confirm"), [p.name]),
          __WEBUI_TEXT__("settings.delete"),
          true,
        )
      )
        R.command("/api/v1/presets", { action: "delete", id: p.id });
    };
  if ($("scalePreference")) $("scalePreference").onchange = R.updateScalePreferenceOptions;
  $("forgetPairedScale").onclick = forgetPairedScale;
  if ($("preferredScaleSelect"))
    $("preferredScaleSelect").onchange = () => {
      selectPreferredScale();
      R.updateScalePreferenceOptions();
    };
  const preferredScaleRename = $("preferredScaleRename");
  if (preferredScaleRename)
    preferredScaleRename.onclick = (e) => {
      e.preventDefault();
      if (preferredScaleRename.getAttribute("aria-disabled") === "true") return;
      R.renameScale();
    };
  // Builds without the Micra integration ship none of its controls.
  if (__LINEA_MICRA__) {
    if ($("lineaMicraConnectButton")) $("lineaMicraConnectButton").onclick = connectLineaMicra;
    if ($("lineaMicraSelectButton"))
      $("lineaMicraSelectButton").onclick = () =>
        selectLineaMicra($("lineaMicraMachines").querySelector("input:checked")?.value);
    if ($("lineaMicraMachines"))
      $("lineaMicraMachines").onchange = () => ($("lineaMicraSelectButton").disabled = false);
    if ($("lineaMicraCancelButton"))
      $("lineaMicraCancelButton").onclick = () =>
        R.uiState.micraChange ? endMicraChange() : R.lineaMicraAction("disconnect");
    if ($("lineaMicraChangeButton"))
      $("lineaMicraChangeButton").onclick = async () => {
        $("lineaMicraLinkedNote").hidden = R.uiState.micraChange = true;
        if (!(await R.lineaMicraAction("relist"))) endMicraChange();
      };
    if ($("lineaMicraDisconnectButton"))
      $("lineaMicraDisconnectButton").onclick = () =>
        confirm(__WEBUI_TEXT__("settings.disconnect_account_confirm")) && disconnectLineaMicra();
    for (const id of ["lineaMicraObserveState", "lineaMicraShutdownWithScale"])
      if ($(id)) $(id).onchange = updateMicraDependentControls;
  }
  // Every #workflowPanel field autosaves its section and refreshes the
  // dependent groups through the shared listener below.
  $("bullseyeTestLink").onclick = (e) => {
    e.preventDefault();
    if (!R.controlsMutable) return;
    R.command(
      "/api/v1/bullseye/test",
      { bullseyeRtttl: $("bullseyeRtttl").value },
      false,
      __WEBUI_TEXT__("settings.playing_rtttl_test"),
      __WEBUI_TEXT__("settings.could_not_play_rtttl_test"),
    );
  };
  // The device backup's per-preset fixed time lives in Shot limits.
  $("scaleLossFixedLink").onclick = () => {
    const field = $("scaleLossFixedS");
    field.closest("details").open = true;
    field.scrollIntoView({ block: "center" });
    field.focus({ preventScroll: true });
  };
  document
    .querySelectorAll("#workflowPanel input,#workflowPanel select,#workflowPanel textarea")
    .forEach((el) => {
      if (
        ["presetRenameInput", "dripSetG", "alphaSet"].includes(el.id) ||
        el.closest("#lineaMicraSignIn,#lineaMicraChoose")
      )
        return;
      // Typing waits for a pause; leaving the field (a trusted change) saves
      // now. The number keys' synthetic change counts as typing.
      const typed = el.tagName === "TEXTAREA" || ["text", "number"].includes(el.type);
      const fn = (e) => {
        const sec = R.settingsSectionOf(el.id);
        if (sec === "micra") $("lineaMicraLinkedNote").hidden = true;
        R.noteEdit(autosave[sec], !typed ? 300 : e.type === "change" && e.isTrusted ? 0 : 1200);
        R.updateConfigGroups();
        // Failed-save red marks on brew fields hand over to the live yellow
        // preview, which re-runs over the pending set as a whole.
        clearBrewFieldErrors();
        refreshBrewWarnings();
        R.renderPresetReview();
      };
      el.addEventListener("input", fn);
      el.addEventListener("change", fn);
    });
  // Set drip and Set learning speed apply at once; the field keeps the
  // owner's value until the readback confirms it.
  for (const [input, button, key] of [
    ["dripSetG", "dripSetButton", "dripG"],
    ["alphaSet", "alphaSetButton", "alpha"],
  ]) {
    $(input).addEventListener("input", () => {
      $(input).dataset.edited = "1";
      R.updateBbwControls();
    });
    $(button).onclick = () => {
      if ($(button).disabled) return;
      delete $(input).dataset.edited;
      R.command("/api/v1/calibration/reset", { [key]: R.number(input), baseRevision: R.formRev });
    };
  }
  R.setSettingsRenderer((c) => {
    renderDripChart(c);
    R.renderGroupRows($("workflowPanel"), GROUP_SUMMARIES);
  });
}
