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
    " — " +
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
      " — " +
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
  return validateBrewClient().filter((e) => !e.required);
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
    rangeCheck("rinseGestureS", 0.1, 5, __WEBUI_TEXT__("runtime.rinse_gesture"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    rangeCheck("rinseDurationS", 0.5, 10, __WEBUI_TEXT__("runtime.rinse_duration"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    (m
      ? rangeCheck("stopPulseMs", 50, 1000, __WEBUI_TEXT__("runtime.auto_stop_pulse"), {
          int: 1,
        }) ||
        rangeCheck("maxSinglePressMs", 100, 5000, __WEBUI_TEXT__("runtime.single_press_limit"), {
          int: 1,
        }) ||
        (reedMachine
          ? rangeCheck(
              "reedConfirmTimeoutS",
              0.2,
              5,
              __WEBUI_TEXT__("runtime.reed_confirm_timeout"),
              { unit: __WEBUI_TEXT__("runtime.s") },
            )
          : "") ||
        // Reed builds ship no switch-only fields.
        ($("shotReactTimeoutS") &&
          rangeCheck("shotReactTimeoutS", 3, 30, __WEBUI_TEXT__("runtime.shot_reaction_timeout"), {
            int: 1,
            unit: __WEBUI_TEXT__("runtime.s"),
          }))
      : rangeCheck(
          "paddleReturnReminderMaxDurationMin",
          1,
          60,
          __WEBUI_TEXT__("runtime.paddle_limit"),
          { int: 1, unit: __WEBUI_TEXT__("runtime.min") },
        )) ||
    rangeCheck("retareWindowS", 0.5, 10, __WEBUI_TEXT__("runtime.retare_window"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    rangeCheck("postTareBaselineGraceS", 0.5, 10, __WEBUI_TEXT__("runtime.post_tare_grace"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    rangeCheck("minimumCupWeightG", 1, 500, __WEBUI_TEXT__("runtime.min_cup_weight"), {
      unit: __WEBUI_TEXT__("runtime.g"),
    }) ||
    rangeCheck("cupRemovedWeightG", -50, -0.1, __WEBUI_TEXT__("runtime.cup_removed"), {
      unit: __WEBUI_TEXT__("runtime.g"),
    }) ||
    rangeCheck("retareStabilitySamples", 2, 10, __WEBUI_TEXT__("runtime.retare_samples"), {
      int: 1,
    }) ||
    rangeCheck("retareStabilityToleranceG", 0.1, 20, __WEBUI_TEXT__("runtime.retare_tolerance"), {
      unit: __WEBUI_TEXT__("runtime.g"),
    }) ||
    rangeCheck("retareStabilityMaxGapS", 0.1, 5, __WEBUI_TEXT__("runtime.retare_sample_gap"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    rangeCheck(
      "retareStabilityMinDurationS",
      0,
      2,
      __WEBUI_TEXT__("runtime.retare_min_stable_time"),
      { unit: __WEBUI_TEXT__("runtime.s") },
    ) ||
    rangeCheck(
      "scaleTimerStopExtraDelayMs",
      0,
      1000,
      __WEBUI_TEXT__("runtime.scale_timer_stop_extra_delay"),
      { int: 1, unit: __WEBUI_TEXT__("runtime.ms") },
    ) ||
    rangeCheck("dripDelayS", 0, 10, __WEBUI_TEXT__("runtime.drip_delay"), {
      unit: __WEBUI_TEXT__("runtime.s"),
    }) ||
    rangeCheck("lastShotCooldownMin", 5, 240, __WEBUI_TEXT__("runtime.last_shot_cooldown"), {
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
          await R.sleep(100);
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
  syncScaleForgetKey(!(R.controlsMutable && (sc.preferredMac || sc.macCachePauseRemainingMs > 0)));
  R.loadSettingsConfig(s);
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
// The Forget key lives inside the My scale dropdown: present only while a
// saved scale is the selected value, armed by a first tap (warm wash) and
// fired by the second. Outside taps, Escape, a dropdown change or a
// status-driven disable disarm it.
let forgetArmed;
function disarmForgetKey() {
  clearTimeout(forgetArmed);
  $("forgetPairedScale")?.classList.remove("arming");
}
function syncScaleForgetKey(disabled) {
  const key = $("forgetPairedScale"),
    sel = $("preferredScaleSelect");
  if (!key || !sel) return;
  if (disabled !== undefined) key.disabled = disabled;
  sel.closest(".selBox").toggleAttribute("data-paired", sel.value !== "");
  if (key.disabled) disarmForgetKey();
}
async function forgetPairedScale() {
  const key = $("forgetPairedScale");
  if (!key.classList.contains("arming")) {
    key.classList.add("arming");
    forgetArmed = setTimeout(disarmForgetKey, 2500);
    return;
  }
  disarmForgetKey();
  // A pending select draft would re-add the forgotten scale as a ghost
  // option; the explicit forget voids it.
  const sel = $("preferredScaleSelect");
  if (sel) sel.dataset.pending = "0";
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
    payload: brewPayload,
    valid: () => !validateBrewClient().length,
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
// Field validation and the brew form payload; Home's quick-weight saves reach
// validateBrewClient and renderPresetReview through R.setSettingsForm.
function rangeCheck(id, min, max, label, opts) {
  const raw = $(id).value.trim(),
    v = Number(raw),
    unit = opts && opts.unit ? " " + opts.unit : "";
  if (raw === "" || !Number.isFinite(v))
    return {
      id,
      required: true,
      msg: label + __WEBUI_TEXT__("runtime.is_required") + min + "–" + max + unit + ").",
    };
  if (opts && opts.int && !Number.isInteger(v))
    return {
      id,
      msg:
        label + __WEBUI_TEXT__("runtime.must_be_an_integer_from") + min + " to " + max + unit + ".",
    };
  if (opts && opts.step && Math.abs(v / opts.step - Math.round(v / opts.step)) > 1e-6)
    return {
      id,
      msg:
        label +
        __WEBUI_TEXT__("runtime.must_be_from") +
        min +
        " to " +
        max +
        unit +
        __WEBUI_TEXT__("runtime.in_steps_of").replace("%1", opts.step) +
        ".",
    };
  if (v < min || v > max)
    return {
      id,
      msg: label + __WEBUI_TEXT__("runtime.must_be_from") + min + " to " + max + unit + ".",
    };
  return null;
}
// Brew validation message values: formatted per the input's step so every
// R.number shows the precision the field itself uses ("34.0 g", never "34 g"
// for a 0.1-step field, and never a rounded-off typed value).
function brewFieldValue(id) {
  const raw = $(id).value.trim(),
    v = Number(raw);
  if (raw === "" || !Number.isFinite(v)) return "";
  const step = Number($(id).step),
    dec = step > 0 && step < 1 ? (String(step).split(".")[1] || "").length : 0,
    fixed = v.toFixed(dec);
  return Number(fixed) === v ? fixed : String(v);
}
const BREW_FIELD_TEXT = {
    doseG: [__WEBUI_TEXT__("runtime.dose"), __WEBUI_TEXT__("runtime.g"), "recipe"],
    goalWeightG: [__WEBUI_TEXT__("runtime.target_yield"), __WEBUI_TEXT__("runtime.g"), "recipe"],
    operationalWallS: [
      __WEBUI_TEXT__("runtime.max_bbw_time"),
      __WEBUI_TEXT__("runtime.s"),
      "limits",
    ],
    bbwProtectionS: [
      __WEBUI_TEXT__("runtime.bbw_protection"),
      __WEBUI_TEXT__("runtime.s"),
      "limits",
    ],
    lineaMicraBrewTargetC: [__WEBUI_TEXT__("runtime.brew_temperature"), "°C"],
    fastBeforeS: [__WEBUI_TEXT__("runtime.window_start"), __WEBUI_TEXT__("runtime.s"), "recipe"],
    fastMarginG: [__WEBUI_TEXT__("runtime.fast_margin"), __WEBUI_TEXT__("runtime.g"), "fast"],
    slowAfterS: [__WEBUI_TEXT__("runtime.window_end"), __WEBUI_TEXT__("runtime.s"), "recipe"],
    slowMarginG: [__WEBUI_TEXT__("runtime.slow_margin"), __WEBUI_TEXT__("runtime.g"), "slow"],
    fastRampS: [__WEBUI_TEXT__("settings.fast_ramp"), __WEBUI_TEXT__("runtime.s"), "fast"],
    slowRampS: [__WEBUI_TEXT__("settings.slow_ramp"), __WEBUI_TEXT__("runtime.s"), "slow"],
    fastCurveHardness: [__WEBUI_TEXT__("settings.ramp_hardness"), "%", "fast"],
    slowCurveHardness: [__WEBUI_TEXT__("settings.ramp_hardness"), "%", "slow"],
    scaleLossFixedS: [
      __WEBUI_TEXT__("runtime.scale_loss_override"),
      __WEBUI_TEXT__("runtime.s"),
      "limits",
    ],
  },
  BREW_SECTION_TEXT = {
    recipe: __WEBUI_TEXT__("settings.recipe"),
    fast: __WEBUI_TEXT__("settings.fast_shots"),
    slow: __WEBUI_TEXT__("settings.slow_shots"),
    limits: __WEBUI_TEXT__("settings.shot_limits"),
  },
  BREW_CROSS_TEXT = {
    gt: __WEBUI_TEXT__("settings.err_greater"),
    lt: __WEBUI_TEXT__("settings.err_less"),
    ge: __WEBUI_TEXT__("settings.err_at_least"),
    le: __WEBUI_TEXT__("settings.err_not_exceed"),
  },
  BREW_REL = {
    ">": (a, b) => a > b,
    "<": (a, b) => a < b,
  },
  // Cross-field rules as [fieldA, fieldB, violation(fieldA, fieldB + offset),
  // messageA, messageB, offset]: each message is phrased from its own field's
  // side and names the other field's value, label, and settings section. The
  // retare floor entry is single-sided: its bound is computed, not an edited field.
  BREW_CROSS_RULES = [
    ["bbwProtectionS", "operationalWallS", ">", "le", "ge"],
    ["bbwProtectionS", "retareFloor", "<", "ge"],
    ["fastBeforeS", "bbwProtectionS", "<", "ge", "le", 2],
    ["slowAfterS", "operationalWallS", ">", "le", "ge", -1],
  ];
// Message label for a brew field: the Settings form labels carry the unit in
// parentheses, so every error and warning opens with the exact field name the
// user sees, never the field's own current value.
const brewFieldLabel = (id) => {
  const [label, unit] = BREW_FIELD_TEXT[id];
  return label + " (" + unit + ")";
};
function brewCrossErr(kind, id, other, off = 0) {
  const [ref, refUnit, refSection] = BREW_FIELD_TEXT[other],
    limit = (off ? (R.number(other) + off).toFixed(1) : brewFieldValue(other)) + " " + refUnit,
    named = off ? ref + (off > 0 ? " + " : " − ") + Math.abs(off) + " " + refUnit : ref;
  return R.sub(BREW_CROSS_TEXT[kind], [
    brewFieldLabel(id),
    limit,
    named,
    BREW_SECTION_TEXT[refSection],
  ]);
}
// The fields the user changed: the baseline follows the device for every
// field without a pending edit, so programmatic writes (the Home quick-weight
// sheet) count as edits too.
function editedBrewFields() {
  const edited = new Set();
  if (!R.uiState.formBaseline) return edited;
  for (const el of R.settingsSectionEls("brew"))
    if (el.id && R.uiState.formBaseline[el.id] !== (el.type === "checkbox" ? el.checked : el.value))
      edited.add(el.id);
  return edited;
}
function validateBrewClient() {
  const errors = [],
    edited = editedBrewFields(),
    fastOn = $("fastExtractionGuardEnabled").checked,
    slowOn = $("slowExtractionGuardEnabled").checked,
    brewRange = (id, min, max, opts) =>
      rangeCheck(id, min, max, brewFieldLabel(id), {
        unit: BREW_FIELD_TEXT[id][1],
        ...opts,
      }),
    add = (e) => e && errors.push(e);
  add(brewRange("goalWeightG", 10, 200, { step: 0.1 }));
  add(brewRange("operationalWallS", 5, 60, { int: 1 }));
  add(brewRange("bbwProtectionS", 0.5, 30));
  if (document.documentElement.classList.contains("micraTemperatureEnabled"))
    add(brewRange("lineaMicraBrewTargetC", 80, 100));
  add(brewRange("doseG", 5, 30, { step: 0.1 }));
  add(brewRange("fastBeforeS", 2.5, 58, { step: 0.5 }));
  add(brewRange("slowAfterS", 3.5, 59, { step: 0.5 }));
  if (fastOn) add(brewRange("fastMarginG", 0.5, 10, { step: 0.1 }));
  if (slowOn) add(brewRange("slowMarginG", 0.5, 10, { step: 0.1 }));
  if (fastOn) add(brewRange("fastRampS", 0, 10, { step: 0.5 }));
  if (slowOn) add(brewRange("slowRampS", 0, 10, { step: 0.5 }));
  if (fastOn) add(brewRange("fastCurveHardness", 0, 100, { step: 0.5 }));
  if (slowOn) add(brewRange("slowCurveHardness", 0, 100, { step: 0.5 }));
  if (brewFieldValue("scaleLossFixedS") !== "")
    add(
      brewRange(
        "scaleLossFixedS",
        R.formNumber("bbwProtectionS"),
        R.formNumber("operationalWallS"),
      ),
    );
  const rw = R.formNumber("retareWindowS"),
    withRetare = !!$("autoRetare") && $("autoRetare").checked && Number.isFinite(rw),
    floor = (withRetare ? rw : 0) + 3;
  for (const [a, b, rel, kindA, kindB, off = 0] of BREW_CROSS_RULES) {
    const retareRule = b === "retareFloor";
    // An empty or non-numeric participant already produced its self-range
    // error; cross rules only compare well-formed values (Number("") is 0).
    if (brewFieldValue(a) === "" || (!retareRule && brewFieldValue(b) === "")) continue;
    const x = R.number(a),
      y = retareRule ? floor : R.number(b) + off;
    if (!BREW_REL[rel](x, y)) continue;
    if (retareRule) {
      const unit = BREW_FIELD_TEXT[a][1],
        fixed = floor.toFixed(1),
        bound = (Number(fixed) === floor ? fixed : String(floor)) + " " + unit;
      errors.push({
        id: a,
        msg: withRetare
          ? R.sub(BREW_CROSS_TEXT.ge, [
              brewFieldLabel(a),
              bound,
              R.sub(__WEBUI_TEXT__("settings.err_retare_sum"), [brewFieldValue("retareWindowS")]),
              __WEBUI_TEXT__("settings.tare"),
            ])
          : R.sub(__WEBUI_TEXT__("settings.err_at_least_plain"), [brewFieldLabel(a), bound]),
      });
      continue;
    }
    const blamed = [a, b].filter((id) => edited.has(id));
    for (const id of blamed.length ? blamed : [a])
      errors.push({
        id,
        msg: brewCrossErr(id === a ? kindA : kindB, id, id === a ? b : a, id === a ? off : -off),
      });
  }
  if (
    brewFieldValue("fastBeforeS") !== "" &&
    brewFieldValue("slowAfterS") !== "" &&
    R.number("slowAfterS") < R.number("fastBeforeS") + 1
  )
    errors.push({
      id: "slowAfterS",
      msg: R.sub(__WEBUI_TEXT__("runtime.window_min_length"), [
        +(R.number("fastBeforeS") + 1).toFixed(1),
      ]),
    });
  // Margins are relative to the target: extra yield at most a quarter of it,
  // and an early stop that still leaves 5 g in the cup.
  const goal = brewFieldValue("goalWeightG") === "" ? NaN : R.number("goalWeightG"),
    recipe = BREW_SECTION_TEXT.recipe;
  if (fastOn && brewFieldValue("fastMarginG") !== "" && R.number("fastMarginG") > goal / 4)
    errors.push({
      id: "fastMarginG",
      msg: R.sub(BREW_CROSS_TEXT.le, [
        brewFieldLabel("fastMarginG"),
        (goal / 4).toFixed(1) + __WEBUI_TEXT__("runtime.g_2"),
        __WEBUI_TEXT__("runtime.quarter_of_target"),
        recipe,
      ]),
    });
  if (slowOn && brewFieldValue("slowMarginG") !== "" && R.number("slowMarginG") > goal - 5)
    errors.push({
      id: "slowMarginG",
      msg: R.sub(BREW_CROSS_TEXT.le, [
        brewFieldLabel("slowMarginG"),
        (goal - 5).toFixed(1) + __WEBUI_TEXT__("runtime.g_2"),
        BREW_FIELD_TEXT.goalWeightG[0] + " − 5 g",
        recipe,
      ]),
    });
  return errors;
}
function brewPayload() {
  return {
    ...Object.fromEntries([
      ...["doseG", "goalWeightG", "fastBeforeS", "slowAfterS", "fastMarginG", "slowMarginG"].map(
        (k) => [k, R.number(k)],
      ),
      ...["operationalWall", "bbwProtection"].map((k) => [k + "Ms", R.sToMs(k + "S")]),
      ...[
        "brewByWeight",
        "fastExtractionGuardEnabled",
        "slowExtractionGuardEnabled",
        "grindHintsEnabled",
      ].map((k) => [k, $(k).checked]),
    ]),
    // Empty means "use your recent shots": the firmware stores it as 0.
    scaleLossFixedS: R.number("scaleLossFixedS"),
    ...Object.fromEntries(
      ["fast", "slow"].flatMap((s) => [
        [s + "RampS", R.number(s + "RampS")],
        [s + "Curve", $(s + "Curve").value],
        [s + "CurveHardness", R.number(s + "CurveHardness")],
      ]),
    ),
    ...(document.documentElement.classList.contains("micraTemperatureEnabled")
      ? { lineaMicraBrewTargetC: R.number("lineaMicraBrewTargetC") }
      : {}),
  };
}
// Each side's transition hint follows where the curve is steepest, which
// reads differently per side; the cards themselves carry static names.
const RAMP_CURVE_TEXT = {
  fast: {
    early: __WEBUI_TEXT__("settings.ramp_fast_early"),
    even: __WEBUI_TEXT__("settings.ramp_fast_even"),
    late: __WEBUI_TEXT__("settings.ramp_fast_late"),
  },
  slow: {
    early: __WEBUI_TEXT__("settings.ramp_slow_early"),
    even: __WEBUI_TEXT__("settings.ramp_slow_even"),
    late: __WEBUI_TEXT__("settings.ramp_slow_late"),
  },
};
// The curve radios and the hardness slider stand in for their hidden fields,
// which keep the form's edit, autosave and revert behavior. Each curve label
// draws and names the curve it picks at the current hardness.
function rampIcon(curve) {
  let d = "M0 3H8";
  for (let i = 1; i <= 24; i++) d += `L${8 + i} ${(3 + 14 * R.rampEase(curve, i / 24)).toFixed(2)}`;
  return d + "H40";
}
// The slider's live percent readout: one decimal only when the value has one.
function paintHardnessSlider(side, value) {
  const range = $(side + "CurveHardnessRange"),
    pct = $(side + "CurveHardnessPct");
  if (!range) return;
  range.style.setProperty("--p", value + "%");
  if (pct) pct.textContent = (value % 1 ? value.toFixed(1) : value.toFixed(0)) + "%";
}
function syncRampChoice(side) {
  const type = $(side + "Curve"),
    hardness = $(side + "CurveHardness"),
    box = type.closest(".rampChoice");
  if (!box._bound) {
    box._bound = true;
    box.addEventListener("change", (e) => {
      const radio = e.target.closest("input[data-ramp]"),
        select = radio && $(radio.dataset.ramp);
      if (!select || select.disabled) return;
      select.value = radio.value;
      select.dispatchEvent(new Event("change"));
    });
  }
  // The slider writes the hidden field through the shared change pipeline and
  // paints the rail. A pointer drag owns the thumb: while it lasts, a config
  // sync must not snap the value back (focus is unreliable on touch ranges).
  const range = $(side + "CurveHardnessRange");
  if (range && !range._bound) {
    range._bound = true;
    range.addEventListener("pointerdown", () => {
      range._dragging = true;
    });
    for (const end of ["pointerup", "pointercancel"])
      range.addEventListener(end, () => {
        range._dragging = false;
      });
    range.addEventListener("input", () => {
      const v = Math.min(100, Math.max(0, +range.value));
      if (!hardness.disabled) {
        hardness.value = String(v);
        hardness.dispatchEvent(new Event("change"));
      }
      paintHardnessSlider(side, v);
    });
  }
  if (range && !range._dragging && document.activeElement !== range) {
    range.value = String(Math.round((+hardness.value || 0) * 2) / 2);
    paintHardnessSlider(side, +range.value || 0);
  }
  for (const radio of box.querySelectorAll("input[data-ramp]")) {
    const select = $(radio.dataset.ramp);
    radio.checked = radio.value === select.value;
    radio.disabled = select.disabled;
    if (select !== type) continue;
    const curve = R.rampCurveIndex(radio.value, hardness.value),
      label = radio.closest("label");
    label.querySelector("path").setAttribute("d", rampIcon(curve));
  }
}
// The page header shrinks while scrolling on phones: its measured bottom
// edge is where the pinned Fast or Slow header starts.
let rampTopObserver = null;
function pinRampSections() {
  if (rampTopObserver) return;
  const bar = document.querySelector(".topBar"),
    set = () =>
      document.body.style.setProperty("--rampTop", bar.getBoundingClientRect().bottom + "px");
  rampTopObserver = new ResizeObserver(set);
  rampTopObserver.observe(bar);
  set();
}
// Each side's transition, drawn from the cut law on a fixed 12.5 s span with
// at least 6 g of margin room and room under the lower level, in the review
// graph's language: zones under the cut line, dotted target and held levels,
// the scrub marker and its pill. A side that is off draws flat and dimmed.
function renderRampPreviews(m) {
  for (const side of ["fast", "slow"]) {
    const host = $(side + "RampPreview"),
      fast = side === "fast";
    syncRampChoice(side);
    $(side + "CurveHint").textContent = RAMP_CURVE_TEXT[side][$(side + "Curve").value] || "";
    const pad = parseFloat(getComputedStyle(host).paddingLeft) || 0,
      W = host.clientWidth && host.clientWidth - 2 * pad,
      group = host.closest("details");
    // Drawn when its group opens or resizes, like the review graph.
    host._draw = renderPresetReview;
    host._w = host.clientWidth;
    R.observePresetHost(host);
    pinRampSections();
    if (!W) continue;
    group.style.setProperty("--rampSum", group.querySelector("summary").offsetHeight + "px");
    const H = 124,
      L = 46,
      Rt = W - 8,
      T = 26,
      B = H - 22,
      f = (v) => v.toFixed(1),
      n = (v) => +(+v).toFixed(1),
      edge = fast ? m.t1 : m.t2,
      t0 = fast ? edge - 11 : edge - 1.5,
      span = 12.5,
      room = Math.max(6, (fast ? m.ceil - m.goal : m.goal - m.floor) + 1),
      X = (t) => L + ((Rt - L) * (t - t0)) / span,
      // The lower level sits above the base, so the zones under it have height.
      lo = T + (B - T) * 0.64,
      Y = (g) =>
        fast ? lo - ((lo - T) * (g - m.goal)) / room : T + ((lo - T) * (m.goal - g)) / room,
      a = fast ? Math.max(m.prot, m.t1 - m.dFast) : m.t2,
      b = fast ? m.t1 : Math.min(m.wall, m.t2 + m.dSlow),
      from = Math.max(t0, m.prot),
      to = Math.min(t0 + span, m.wall),
      held = fast ? m.ceil : m.floor,
      gy = Y(m.goal),
      hy = Y(held),
      on = fast ? m.fast : m.slow,
      curve = fast ? m.curveFast : m.curveSlow,
      P = (t, g) => f(X(t)) + " " + f(Y(g)),
      // Exact curve, like the review graph: hold, transition, hold.
      [g0, g1] = on ? (fast ? [m.ceil, m.goal] : [m.goal, m.floor]) : [m.goal, m.goal],
      lineAt = (t) =>
        t <= a ? g0 : t >= b ? g1 : g0 + (g1 - g0) * R.rampEase(curve, (t - a) / (b - a));
    let line = `M${P(from, g0)}L${P(a, g0)}`;
    for (let i = 1; i <= 60; i++)
      line += `L${P(a + ((b - a) * i) / 60, g0 + (g1 - g0) * R.rampEase(curve, i / 60))}`;
    line += `L${P(to, g1)}`;
    const zones = fast
        ? [
            ["rvFastFull", from, a, on],
            ["rvFastPart", a, m.t1, on],
            ["rvWindow", m.t1, to, true],
          ]
        : [
            ["rvWindow", from, m.t2, true],
            ["rvSlowPart", m.t2, b, on],
            ["rvSlowFull", b, to, on],
          ],
      zone = ([cls, z0, z1, lit]) => {
        if (z1 <= z0) return "";
        let d = `M${P(z0, lineAt(z0))}`;
        for (let i = 1; i <= 40; i++) {
          const t = z0 + ((z1 - z0) * i) / 40;
          d += `L${P(t, lineAt(t))}`;
        }
        return `<path class="${lit ? cls : "rvBandOff"}" d="${d}L${f(X(z1))} ${B}L${f(X(z0))} ${B}Z" fill="url(#${side}Ramp${cls})"/>`;
      },
      fills = [
        ["rvFastFull", "gFast", 0.36, 0],
        ["rvFastPart", "gFast", 0.18, 0],
        ["rvWindow", "ok", 0.42, 0.02],
        ["rvSlowPart", "gSlow", 0.18, 0],
        ["rvSlowFull", "gSlow", 0.36, 0],
      ]
        .map(([c, k, top, base]) => R.fadeFill(side + "Ramp" + c, `var(--${k})`, top, base))
        .join(""),
      label = (x, y, text, cls = "", anchor = "middle") =>
        `<text class="rvLbl ${cls}" x="${f(x)}" y="${f(y)}" text-anchor="${anchor}">${text}</text>`,
      apart = Math.abs(hy - gy) >= 12,
      length = b - a,
      // No weight cut before the earliest weight stop or after the max shot time.
      limited = fast ? m.t1 - m.dFast < m.prot : m.t2 + m.dSlow > m.wall,
      noCut = fast ? Math.min(Rt, X(m.prot)) - L : Rt - Math.max(L, X(m.wall));
    host.innerHTML =
      `<svg width="${W}" height="${H}" viewBox="0 0 ${W} ${H}">` +
      `<defs>${fills}<pattern id="${side}RampHatch" width="6" height="6" patternUnits="userSpaceOnUse" patternTransform="rotate(45)"><path class="rvHatchLine" d="M0 0V6"/></pattern></defs>` +
      zones.map(zone).join("") +
      (noCut > 0
        ? `<rect x="${f(fast ? L : X(m.wall))}" y="${T - 4}" width="${f(noCut)}" height="${B - T + 8}" fill="url(#${side}RampHatch)"/>`
        : "") +
      `<path class="rvTarget" d="M${L} ${f(gy)}H${Rt}"/>` +
      // The held Fast or Slow level gets the same dotted line, under the cut line.
      (on && held !== m.goal ? `<path class="rvTarget" d="M${L} ${f(hy)}H${Rt}"/>` : "") +
      `<path class="rvWin" d="M${f(X(edge))} ${T - 4}V${B + 4}"/>` +
      `<path class="rvCut${on ? "" : " off"}" d="${line}"/>` +
      (length > 0
        ? `<path class="rampSpan" d="M${f(X(a))} ${T - 8}v-5H${f(X(b))}v5"/>` +
          // The bracket end away from the window drops to its time label.
          `<path class="rampGuide" d="M${f(X(fast ? a : b))} ${T - 8}V${H - 16}"/>`
        : "") +
      label(
        (X(a) + X(b)) / 2,
        T - 17,
        length > 0 ? n(length) + " s" : __WEBUI_TEXT__("settings.ramp_step"),
        "strong",
      ) +
      label(L - 6, gy + 4, n(m.goal) + " g", "strong", "end") +
      (apart ? label(L - 6, hy + 4, n(held) + " g", "", "end") : "") +
      label(X(fast ? a : edge), H - 4, n(fast ? a : edge) + " s") +
      (X(b) - X(a) >= 34 ? label(X(fast ? edge : b), H - 4, n(fast ? edge : b) + " s") : "") +
      (noCut >= 84
        ? label(
            fast ? L + noCut / 2 : X(m.wall) + noCut / 2,
            (T + B) / 2 + 4,
            __WEBUI_TEXT__("runtime.graph_legend_no_cut"),
            "rvHalo",
          )
        : "") +
      `<g class="rvScrub" role="slider" tabindex="0" aria-label="${R.escHtml(__WEBUI_TEXT__("runtime.graph_scrub"))}" aria-valuemin="${f(from)}" aria-valuemax="${f(to)}"><path class="rvGuide"/><circle class="rvHandle" r="5"/>${R.PILL}</g>` +
      "</svg>" +
      (limited
        ? `<small class="fieldHint">${R.sub(
            fast
              ? __WEBUI_TEXT__("settings.ramp_limited_fast")
              : __WEBUI_TEXT__("settings.ramp_limited_slow"),
            [n(length), n(fast ? m.prot : m.wall)],
          )}</small>`
        : "");
    host._view = { m, X, Y, T, B, from, to, tAt: (px) => t0 + ((px - L) * span) / (Rt - L) };
    if (!(host._s >= from && host._s <= to)) host._s = Math.round((a + b) * 5) / 10;
    if (!host.dataset.scrub) {
      host.dataset.scrub = "1";
      R.bindScrub(
        host,
        (x) => host._view.tAt(x - host.querySelector("svg").getBoundingClientRect().left),
        (t) => {
          const v = host._view;
          host._s = Math.max(v.from, Math.min(v.to, Math.round(t * 10) / 10));
          positionRampScrub(host);
        },
        () => host._s,
        ".rvScrub",
      );
    }
    positionRampScrub(host);
  }
}
// The marker rides the side's cut line like the review graph's scrubber;
// held, the same pill reads when and at what weight the shot would be cut.
function positionRampScrub(host) {
  const v = host._view,
    g = host.querySelector(".rvScrub");
  if (!v || !g) return;
  const stop = host._s,
    w = R.cutLine(stop, v.m),
    [guide, handle, pill] = g.children,
    hx = v.X(stop),
    text = R.sub(__WEBUI_TEXT__("runtime.graph_cut_pill"), [
      0,
      0,
      "",
      w.toFixed(1),
      stop.toFixed(1),
    ]);
  guide.setAttribute("d", `M${hx.toFixed(1)} ${v.T - 4}V${v.B + 4}`);
  handle.setAttribute("cx", hx.toFixed(2));
  handle.setAttribute("cy", v.Y(w).toFixed(2));
  R.placePill(pill, text, hx, v.Y(w));
  g.setAttribute("aria-valuenow", stop.toFixed(1));
  g.setAttribute("aria-valuetext", text);
}
// Review graph of the preset under review: pace curves fan out from the
// origin into corridors whose boundaries are the preset's thresholds, colored
// only under the cut line. Unsaved edits redraw live over the ghosted saved line.
function renderPresetReview() {
  const host = $("presetReviewGraph");
  // Settings redraws it on entry; hidden, it would redraw on every Home frame.
  if (!host || R.activeView !== "settings") return;
  const goalNow = R.formNumber("goalWeightG"),
    grams = (v) => (Number.isFinite(v) ? v.toFixed(1) : "\u2014");
  for (const [id, key, w] of [
    [
      "fastMarginResult",
      __WEBUI_TEXT__("runtime.margin_up_to"),
      goalNow + R.formNumber("fastMarginG"),
    ],
    [
      "slowMarginResult",
      __WEBUI_TEXT__("runtime.margin_down_to"),
      goalNow - R.formNumber("slowMarginG"),
    ],
  ])
    $(id).textContent = R.sub(key, [grams(w), R.brewRatio(w, R.formNumber("doseG"))]);
  const id = R.presetState.selectedId || R.presetState.activeId,
    saved = R.presetState.items.find((x) => x.id === id),
    savedModel = R.buildRuleChartModel(saved),
    draft =
      R.configLoaded && R.uiState.brewDirty && R.bbwFormPresetId === id
        ? R.buildRuleChartModel({ ...saved, ...brewPayload() })
        : null,
    m = draft && draft.mode !== "empty" ? draft : savedModel,
    shape = (x) => JSON.stringify([x.pts, x.curveFast, x.curveSlow]),
    ghost = shape(m) !== shape(savedModel) ? savedModel : null;
  $("presetReview").hidden = savedModel.mode === "empty";
  const statsLine = R.shotStatsLine(saved);
  $("windowStatsText").textContent = statsLine || __WEBUI_TEXT__("runtime.no_shots_yet");
  if (savedModel.mode === "empty") return;
  renderRampPreviews(m);
  // Past arrivals were judged against the saved target: hidden while it is edited.
  const arrivals = m.goal === savedModel.goal ? saved.arrivalsS || [] : [];
  host._draw = renderPresetReview;
  const view = R.drawPresetGraph(host, m, { ghost, arrivals }, $("presetReviewReadout"), id);
  if (!view) return;
  const legend = $("presetReviewLegend");
  if (!legend.childElementCount)
    legend.innerHTML =
      '<span class="rvLeg rvLegCut"><i></i>' +
      __WEBUI_TEXT__("runtime.graph_legend_cut") +
      "</span>" +
      '<span class="rvLeg rvLegPace"><i></i>' +
      __WEBUI_TEXT__("runtime.graph_legend_pace") +
      "</span>" +
      '<span class="rvLeg rvLegExample"><i></i>' +
      __WEBUI_TEXT__("runtime.graph_legend_example") +
      "</span>" +
      '<span class="rvLeg rvLegArrivals"><i></i>' +
      __WEBUI_TEXT__("runtime.graph_legend_arrivals") +
      "</span>" +
      '<span class="rvLeg rvLegWindow"><i></i>' +
      __WEBUI_TEXT__("runtime.graph_legend_corridor") +
      "</span>" +
      '<span class="rvLeg rvLegFast"><i></i><i></i>' +
      __WEBUI_TEXT__("runtime.graph_legend_fast") +
      "</span>" +
      '<span class="rvLeg rvLegSlow"><i></i><i></i>' +
      __WEBUI_TEXT__("runtime.graph_legend_slow") +
      "</span>" +
      '<span class="rvLeg rvLegNoCut"><i></i>' +
      __WEBUI_TEXT__("runtime.graph_legend_no_cut") +
      "</span>" +
      '<span class="rvLeg rvLegSaved"><i></i>' +
      __WEBUI_TEXT__("runtime.graph_legend_saved") +
      "</span>" +
      '<small class="rvNote"></small>';
  legend.lastChild.textContent =
    __WEBUI_TEXT__("runtime.graph_pace_note") + " " + R.paceStartNote(m);
  legend.classList.toggle("ghosted", !!ghost);
  legend.classList.toggle("hasArrivals", view.arrivals > 0);
  legend.classList.toggle("noFast", !m.fast);
  legend.classList.toggle("noSlow", !m.slow);
}
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
// Shot card colors drive only the Colorful hero; Technical tints its own
// graph zones, so the selector is read-only while Technical is resolved.
function syncHeroTintAvailability() {
  if ($("appHeroTint")) $("appHeroTint").disabled = R.heroCardMode() === "technical";
}
// What each newest shot needed against the drip in use, oldest bar first.
// Markers show where the owner set the drip and where quick start decided.
function renderDripChart(c) {
  const dripText = (g) => (g > 0 ? g.toFixed(2) : "0") + __WEBUI_TEXT__("runtime.g_2"),
    dripRange = (v) =>
      (Math.min(...v) > 0 ? Math.min(...v).toFixed(2) : "0") + "–" + dripText(Math.max(...v));
  const recent = c?.bbwRecentG || [],
    skip = (i) => (c?.bbwRecentSkipped >> i) & 1,
    q = c?.bbwQuickStart || {},
    n = recent.length,
    use = c?.bbwEwmaOffsetG || 0,
    armed = q.state === "armed",
    afterSet =
      c?.bbwRecentAfterSet >= 0 && (c.bbwRecentAfterSet < n || n < 10) ? c.bbwRecentAfterSet : -1,
    afterQuick =
      c?.bbwRecentAfterQuick >= 0 && c.bbwRecentAfterQuick < n ? c.bbwRecentAfterQuick : -1,
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
      marked &&
      s.g != null &&
      !skip(s.i) &&
      (afterSet < 0 || s.i < afterSet) &&
      s.i >= Math.max(afterQuick, 0),
    old = (s) => marked && afterSet >= 0 && s.i >= afterSet,
    pad = 10 - shots.length - (armed ? 1 : 0),
    // Only trained shots size the chart; a wild skipped value would squash
    // them, and its own bar clamps at full height instead of overflowing.
    top = Math.max(...recent.filter((g, i) => g != null && !skip(i)), use, 0.5) * 1.3,
    h = (g) => Math.min(100, (Math.max(0, g) / top) * 100) + "%",
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
        "dcBar" +
          (old(s) ? " old" : "") +
          (counted(s) ? " qs" : "") +
          (skip(s.i) ? " skip" : "") +
          (s.next ? " next" : ""),
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
  const needed = recent.filter(
    (g, i) => g != null && !skip(i) && (mode === "learning" || counted({ g, i })),
  );
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
  R.registerViewStatus("settings", (s) => {
    applyStatus(s);
    syncHeroTintAvailability();
  });
  R.setSettingsForm({ validateBrewClient, renderPresetReview });
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
  document.addEventListener("pointerdown", (e) => {
    if (!e.target.closest(".selBox")) disarmForgetKey();
  });
  document.addEventListener("keydown", (e) => e.key === "Escape" && disarmForgetKey());
  if ($("preferredScaleSelect"))
    $("preferredScaleSelect").onchange = () => {
      disarmForgetKey();
      syncScaleForgetKey();
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
        el.closest("#lineaMicraSignIn,#lineaMicraChoose,[data-g=app]")
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
        renderPresetReview();
      };
      el.addEventListener("input", fn);
      el.addEventListener("change", fn);
    });
  // Set drip and Set learning speed apply at once; the field keeps the
  // owner's value until the readback confirms it. Two keys live inside the
  // field, left of the − key, and show only while an edit is pending
  // (input[data-edited]): Set applies (as the form's submit button it also
  // answers Enter); the undo key puts the value in use back without
  // applying anything.
  for (const [input, button, undo, key, label] of [
    ["dripSetG", "dripSetButton", "dripUndoButton", "dripG", __WEBUI_TEXT__("settings.set_drip")],
    [
      "alphaSet",
      "alphaSetButton",
      "alphaUndoButton",
      "alpha",
      __WEBUI_TEXT__("settings.set_learning_speed"),
    ],
  ]) {
    const apply = document.createElement("button");
    Object.assign(apply, { type: "submit", className: "numSet", id: button });
    apply.setAttribute("aria-label", label);
    apply.innerHTML =
      '<svg class="g" viewBox="0 0 24 24" aria-hidden="true"><path d="M4.5 12.5l4.7 4.7L19.5 6.9" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/></svg><span class="t">' +
      __WEBUI_TEXT__("settings.set") +
      "</span>";
    const restore = document.createElement("button");
    Object.assign(restore, { type: "button", className: "numUndo", id: undo });
    restore.setAttribute("aria-label", __WEBUI_TEXT__("settings.undo"));
    restore.innerHTML =
      '<svg class="g" viewBox="0 0 24 24" aria-hidden="true"><path d="M2.5 5v6h5.5M5.2 16a8.6 8.6 0 1 0 2-9.1L2.5 11" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/></svg>';
    $(input).closest(".numBox").append(apply, restore);
    $(input).addEventListener("input", () => {
      $(input).dataset.edited = "1";
      R.updateBbwControls();
    });
    apply.onclick = () => {
      if (apply.disabled) return;
      delete $(input).dataset.edited;
      R.command("/api/v1/calibration/reset", { [key]: R.number(input), baseRevision: R.formRev });
    };
    restore.onclick = () => {
      if (restore.disabled) return;
      delete $(input).dataset.edited;
      $(input).blur();
      R.updateBbwControls();
    };
  }
  // The "This app" group is browser-local: every control applies at once and
  // nothing in it reaches the controller. The installed app is its only home.
  for (const [id, key] of [
    ["appKeepAwake", "awake"],
    ["appAutoLive", "live"],
    ["appVibrate", "vibrate"],
    ["appOrient", "orient"],
  ]) {
    if (!$(id)) continue;
    $(id).checked = R.appSetting(key);
    $(id).onchange = () => R.setAppSetting(key, $(id).checked);
  }
  if ($("appHeroTint")) {
    $("appHeroTint").value = R.heroTintMode();
    $("appHeroTint").onchange = () => R.setHeroTintMode($("appHeroTint").value);
  }
  if ($("appHeroCard")) {
    $("appHeroCard").value = R.heroCardOverride() || "default";
    $("appHeroCard").onchange = () => {
      R.setHeroCardOverride($("appHeroCard").value);
      syncHeroTintAvailability();
    };
  }
  syncHeroTintAvailability();
  const updateAppOffline = () => {
    const box = $("appOfflineBox");
    if (!box) return;
    const st = R.offlineCopyState();
    box.classList.toggle("hidden", !st);
    if (st && $("appOfflineSince"))
      $("appOfflineSince").textContent = __WEBUI_TEXT__("settings.app_offline_since").replace(
        "{0}",
        new Date(st.at).toLocaleString([], { dateStyle: "medium", timeStyle: "short" }),
      );
  };
  if ($("appOfflineDelete"))
    $("appOfflineDelete").onclick = () => {
      if (!confirm(__WEBUI_TEXT__("settings.app_offline_confirm"))) return;
      R.dropOfflineCopy();
      updateAppOffline();
      R.message(__WEBUI_TEXT__("settings.app_offline_deleted"));
    };
  R.setSettingsRenderer((c) => {
    renderDripChart(c);
    R.renderGroupRows($("workflowPanel"), GROUP_SUMMARIES);
    updateAppOffline();
  });
}
