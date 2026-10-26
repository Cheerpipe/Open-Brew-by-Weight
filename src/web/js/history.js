"use strict";
import * as R from "./runtime.js?v=__FW_ASSET_TAG__";
// createFilterMenu, actionMenu, downloadCsv, armListSentinel and ICON_TRASH come
// from the records bundle prelude (records.js).
const $ = R.$;
const historyTypeLabel = (t) =>
  ({
    shot: __WEBUI_TEXT__("runtime.type_shot"),
    rinse: __WEBUI_TEXT__("runtime.type_rinse"),
    other: __WEBUI_TEXT__("runtime.type_other"),
    backflush: __WEBUI_TEXT__("runtime.type_backflush"),
    power_on: __WEBUI_TEXT__("runtime.type_power_on"),
    no_scale_guard_aborted: __WEBUI_TEXT__("runtime.type_no_scale_guard_aborted"),
  })[t];
const HIST_TYPE_SVG = {
  rinse:
    '<svg viewBox="0 0 60 60"><path d="M34.1,45.5a6.3,6.3,0,0,1-4.9,1.5,6,6,0,0,1-4.9-3.9c-.88-2.5.354-4.9,2.2-8.4a58.1,58.1,0,0,0,3.5-7.6,56.3,56.3,0,0,0,3.5,7.6c1.4,2.7,2.5,4.8,2.5,6.7A5.5,5.5,0,0,1,34.1,45.5Z"/><path d="M3.8,35.4A44.9,44.9,0,0,0,6,30.5,45,45,0,0,0,8.2,35.4C9.2,37.3,10,38.8,10,40.2a3.7,3.7,0,0,1-1.3,2.8,4.1,4.1,0,0,1-3.2.98A4,4,0,0,1,2.2,41.3C1.6,39.6,2.5,37.9,3.8,35.4Z"/><path d="M14.8,49.4A44.9,44.9,0,0,0,17,44.5a45,45,0,0,0,2.2,4.8C20.2,51.3,21,52.8,21,54.2a3.7,3.7,0,0,1-1.3,2.8,4.1,4.1,0,0,1-3.2.98,4,4,0,0,1-3.3-2.7C12.6,53.6,13.5,51.9,14.8,49.4Z"/><path d="M57.8,41.3a4,4,0,0,1-3.3,2.7,4.1,4.1,0,0,1-3.2-.98A3.7,3.7,0,0,1,50,40.2c0-1.4.778-2.9,1.8-4.8A44.9,44.9,0,0,0,54,30.5a44.9,44.9,0,0,0,2.2,4.8C57.5,37.9,58.4,39.6,57.8,41.3Z"/><path d="M46.8,55.3a4,4,0,0,1-3.3,2.7,4.1,4.1,0,0,1-3.2-.98A3.7,3.7,0,0,1,39,54.2c0-1.4.778-2.9,1.8-4.8A44.9,44.9,0,0,0,43,44.5a44.9,44.9,0,0,0,2.2,4.8C46.5,51.9,47.4,53.6,46.8,55.3Z"/><path d="M32.7,15a4.1,4.1,0,0,1-3.2.98,4,4,0,0,1-3.3-2.7c-.593-1.7.268-3.4,1.6-5.9A44.9,44.9,0,0,0,30,2.5,45,45,0,0,0,32.2,7.4C33.2,9.3,34,10.8,34,12.2A3.7,3.7,0,0,1,32.7,15Z"/><path d="M4.1,8.5A50.4,50.4,0,0,0,7,2.3,50.4,50.4,0,0,0,9.9,8.5C11.1,10.8,12,12.6,12,14.2a4.6,4.6,0,0,1-1.6,3.5,5.2,5.2,0,0,1-4.1,1.2A5,5,0,0,1,2.2,15.7C1.5,13.5,2.6,11.5,4.1,8.5Z"/><path d="M48.2,15.7c-.736-2.1.311-4.2,1.9-7.2A50.4,50.4,0,0,0,53,2.3a50.4,50.4,0,0,0,2.9,6.2C57.1,10.8,58,12.6,58,14.2a4.6,4.6,0,0,1-1.6,3.5,5.2,5.2,0,0,1-4.1,1.2A5,5,0,0,1,48.2,15.7Z"/><path d="M15.8,22.4A44.9,44.9,0,0,0,18,17.5a45,45,0,0,0,2.2,4.8C21.2,24.3,22,25.8,22,27.2a3.7,3.7,0,0,1-1.3,2.8,4.1,4.1,0,0,1-3.2.98,4,4,0,0,1-3.3-2.7C13.6,26.6,14.5,24.9,15.8,22.4Z"/><path d="M45.8,28.3a4,4,0,0,1-3.3,2.7,4.1,4.1,0,0,1-3.2-.98A3.7,3.7,0,0,1,38,27.2c0-1.4.778-2.9,1.8-4.8A44.9,44.9,0,0,0,42,17.5a44.9,44.9,0,0,0,2.2,4.8C45.5,24.9,46.4,26.6,45.8,28.3Z"/></svg>',
  shot: '<svg viewBox="0 0 512 512"><path d="M416.3,314.7v-79.6c0,-8.3,-6.8,-15.1,-15.1,-15.1H16.2c-8.3,0,-15.1,6.8,-15.1,15.1v79.6c0,70.3,38.9,132.2,97.4,167.1H16.3c-8.3,0,-15.1,6.8,-15.1,15.1c0,8.3,6.8,15.1,15.1,15.1h385c8.3,0,15.1,-6.8,15.1,-15.1c0,-8.3,-6.8,-15.1,-15.1,-15.1h-82.3C377.4,446.9,416.3,385,416.3,314.7z"/><path d="M446.5,247.2v30.8c19.7,5.4,34.2,23.5,34.2,44.8c0,23.8,-18,43.5,-41.1,46.2c-2.7,10.5,-6.1,20.7,-10.3,30.5h4.8c42.3,0,76.7,-34.4,76.7,-76.7C510.8,284.7,482.9,253.1,446.5,247.2z"/><path d="M313.9,98.7c19.7,-15.9,24.1,-32.9,24.4,-44.4c0.7,-27.9,-21.4,-48.3,-23.9,-50.5c-6.2,-5.5,-15.8,-5,-21.3,1.2c-5.5,6.2,-5,15.8,1.2,21.3c0.1,0.1,14.1,13.2,13.8,27.3c-0.2,7.5,-4.5,14.6,-13.2,21.6c-19.7,15.9,-24.1,32.9,-24.4,44.4c-0.7,27.9,21.4,48.3,23.9,50.5c2.9,2.6,6.5,3.8,10.1,3.8c4.2,0,8.3,-1.7,11.3,-5c5.5,-6.2,5,-15.7,-1.2,-21.3c-4,-3.6,-14.2,-15.5,-13.8,-27.4C300.9,112.7,305.3,105.7,313.9,98.7z"/><path d="M218.2,98.7c19.7,-15.9,24.1,-32.9,24.4,-44.4c0.7,-27.9,-21.4,-48.3,-23.9,-50.5c-6.2,-5.5,-15.8,-5,-21.3,1.2c-5.5,6.2,-5,15.8,1.2,21.3c0.1,0.1,14.1,13.2,13.8,27.3c-0.2,7.5,-4.5,14.6,-13.2,21.6c-19.7,15.9,-24.1,32.9,-24.4,44.4c-0.7,27.9,21.4,48.3,23.9,50.5c2.9,2.6,6.5,3.8,10.1,3.8c4.2,0,8.3,-1.7,11.3,-5c5.5,-6.2,5,-15.7,-1.2,-21.3c-4,-3.6,-14.2,-15.5,-13.8,-27.4C205.2,112.7,209.6,105.7,218.2,98.7z"/><path d="M122.5,98.7c19.7,-15.9,24.1,-32.9,24.4,-44.4c0.7,-27.9,-21.4,-48.3,-23.9,-50.5c-6.2,-5.5,-15.8,-5,-21.3,1.2c-5.5,6.2,-5,15.8,1.2,21.3c0.1,0.1,14.1,13.2,13.8,27.3c-0.2,7.5,-4.5,14.6,-13.2,21.6c-19.7,15.9,-24.1,32.9,-24.4,44.4c-0.7,27.9,21.4,48.3,23.9,50.5c2.9,2.6,6.5,3.8,10,3.8c4.2,0,8.3,-1.7,11.3,-5.1c5.5,-6.2,5,-15.8,-1.2,-21.3c-0.1,-0.1,-14.1,-13.2,-13.8,-27.3C109.5,112.8,113.8,105.7,122.5,98.7z"/></svg>',
  other: '<svg class="o"><use href="#hOT"/></svg>',
};
HIST_TYPE_SVG.power_on = '<svg class="o"><use href="#hPW"/></svg>';
HIST_TYPE_SVG.backflush = '<svg viewBox="0 0 256 256"><use href="#hBF"/></svg>';
HIST_TYPE_SVG.no_scale_guard_aborted = '<svg class="o"><use href="#hNS"/></svg>';
// Parsed once per type, then cloned: rows never re-parse kilobytes of path data.
const histIcons = {};
const formatHistoryTime = (r) =>
  r.hasWallTime && r.endedAtLocalSec
    ? R.formatHumanTime(r.endedAtLocalSec)
    : __WEBUI_TEXT__("history.no_time");
const syncHistoryDirButton = () => {
  const i = $("historyDirButton");
  if (!i) return;
  const l =
    R.uiState.historyDir === "desc"
      ? __WEBUI_TEXT__("runtime.oldest_first")
      : __WEBUI_TEXT__("runtime.newest_first");
  i.setAttribute("aria-label", l);
  i.title = l;
  i.textContent =
    __WEBUI_TEXT__("stats.date") + " " + (R.uiState.historyDir === "desc" ? "↓" : "↑");
};
const toggleHistoryDir = () => {
  R.uiState.historyDir = R.uiState.historyDir === "desc" ? "asc" : "desc";
  syncHistoryDirButton();
  historySendSubscribe();
};
const updateHistorySentinel = () => {
  const el = $("historySentinel");
  if (el) {
    el.hidden = !R.uiState.historyData.hasMore;
    el.textContent = R.uiState.historyData.hasMore ? "…" : "";
  }
};
function renderHistory() {
  const body = $("historyRows");
  if (!body || !historyLoaded) return;
  body.replaceChildren();
  const rows = R.uiState.historyData.records;
  let day;
  for (const r of rows) {
    const time = document.createElement("td");
    time.className = "histTime";
    time.textContent = formatHistoryTime(r);
    // Entries sharing the label's day part share one header row.
    if (day !== (day = time.textContent.split(__WEBUI_TEXT__("runtime.at"))[0])) {
      const head = body.insertRow();
      head.className = "histDay";
      head.append(Object.assign(document.createElement("th"), { colSpan: 5, textContent: day }));
    }
    const row = body.insertRow();
    R.wrapTimeEl(time, r);
    const dur = document.createElement("td");
    dur.className = "shotDur";
    dur.innerHTML = (r.durationS || 0).toFixed(1) + `<small>${__WEBUI_TEXT__("runtime.s")}</small>`;
    const type = document.createElement("td");
    type.className = "histType";
    const badge = document.createElement("span");
    badge.className = "histBadge";
    badge.textContent = historyTypeLabel(r.type);
    type.append(badge);
    const del = document.createElement("td");
    del.className = "shotDel";
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "btnGlyph";
    btn.title = __WEBUI_TEXT__("history.entry_actions");
    btn.setAttribute("aria-label", __WEBUI_TEXT__("history.entry_actions"));
    btn.setAttribute("aria-haspopup", "true");
    btn.setAttribute("aria-expanded", "false");
    btn.innerHTML = '<span class="g" aria-hidden="true">' + "⋯" + "</span>";
    btn.onclick = () => entryMenu.open(btn, r);
    del.append(btn);
    const typeSvg = HIST_TYPE_SVG[r.type];
    if (typeSvg) {
      const icon = document.createElement("td");
      // A shot without scale weight draws an outlined cup.
      icon.className = r.type === "shot" && r.noWeight ? "histIcon histNoWeight" : "histIcon";
      icon.setAttribute("aria-hidden", "true");
      if (!histIcons[r.type]) {
        const t = document.createElement("template");
        t.innerHTML = typeSvg;
        histIcons[r.type] = t.content;
      }
      icon.append(histIcons[r.type].cloneNode(true));
      row.append(icon);
    }
    row.append(time, dur, type, del);
  }
  R.setEmptyState(
    "historyTableState",
    rows.length
      ? null
      : R.uiState.historyTypes.length
        ? __WEBUI_TEXT__("runtime.no_activations_match_filter")
        : __WEBUI_TEXT__("runtime.no_recorded_activations_yet"),
  );
  updateHistorySentinel();
}
let entryMenu;
async function deleteOneHistory(id) {
  if (!id || !confirm(__WEBUI_TEXT__("runtime.delete_this_history_record"))) return;
  return R.withCommandGate(async () => {
    try {
      await R.api("/api/v1/history/delete", { method: "POST", body: R.body({ id }) });
      R.uiState.historyData.records = R.uiState.historyData.records.filter((r) => r.id !== id);
      if (typeof R.uiState.historyData.total === "number" && R.uiState.historyData.total > 0)
        R.uiState.historyData.total--;
      R.uiState.historyData.hasMore =
        R.uiState.historyData.records.length < R.uiState.historyData.total;
      renderHistory();
      R.message(__WEBUI_TEXT__("runtime.history_record_deleted"), "ok");
    } catch (e) {
      R.message(
        R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_delete_history_record"), e),
        "error",
      );
    }
  });
}
// The firmware filters the pages by type; the menu only re-subscribes.
const HISTORY_TYPE_KEYS = [
  "shot",
  "rinse",
  "backflush",
  "power_on",
  "other",
  "no_scale_guard_aborted",
];
async function clearActivationHistory() {
  if (!confirm(__WEBUI_TEXT__("runtime.clear_all_recorded_activation_history_this"))) return;
  return R.withCommandGate(async () => {
    try {
      await R.api("/api/v1/history/clear", {
        method: "POST",
        body: R.body({ confirm: "CLEAR_HISTORY" }),
      });
      R.uiState.historyData = {
        bootId: R.uiState.historyData.bootId || 0,
        total: 0,
        hasMore: false,
        records: [],
      };
      renderHistory();
      R.message(__WEBUI_TEXT__("runtime.activation_history_cleared"), "ok");
    } catch (e) {
      R.message(
        R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_clear_history"), e),
        "error",
      );
    }
  });
}
// CSV export rides the standing subscription's one-shot fetch windows, like
// the Stats export: one window at a time, superseded (resolved empty) whenever
// a re-subscribe claims the session fetch slot.

function fetchHistoryWindow(offset) {
  return new Promise((resolve) => {
    const done = (page) => {
      clearTimeout(timer);
      R.uiState.historyExportWindow = null;
      resolve(page);
    };
    const timer = setTimeout(() => done(null), 12e3);
    R.uiState.historyExportWindow = { request: ++R.uiState.historyNextRequest, resolve: done };
    R.sendUiOperation({
      op: "history",
      on: true,
      fetch: true,
      request: R.uiState.historyExportWindow.request,
      offset,
      types: [],
    });
  });
}
async function exportActivationHistory() {
  if (R.uiState.historyExportWindow) return;
  try {
    if (!R.shotWs || R.shotWs.readyState !== 1)
      throw new Error(__WEBUI_TEXT__("runtime.invalid_status"));
    const rows = [["id", "local_time", "ended_at_unix_sec", "has_wall_time", "duration_s", "type"]];
    let offset = 0,
      total = Infinity,
      hasMore = true;
    while (hasMore && offset < total && rows.length - 1 < HISTORY_EXPORT_LIMIT) {
      const page = await fetchHistoryWindow(offset);
      if (!page) throw new Error(__WEBUI_TEXT__("runtime.invalid_status"));
      total = page.total;
      hasMore = page.hasMore;
      for (const r of page.records)
        rows.push([
          r.id,
          r.hasWallTime ? R.formatWallTimeLocal(r.endedAtLocalSec) : "",
          r.endedAtUnixSec,
          r.hasWallTime ? 1 : 0,
          r.durationS.toFixed(1),
          r.type,
        ]);
      offset += page.records.length;
      if (!page.records.length) break;
    }
    downloadCsv("activation-history.csv", rows);
  } catch (e) {
    R.message(R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_export_history"), e), "error");
  }
}
// The History page rides the owned socket (runtime.js registerStream): the
// standing subscription is session-long once ensured, and these handlers run
// from the runtime's socket callbacks.
const HISTORY_PAGE_SIZE = 20,
  HISTORY_EXPORT_LIMIT = 1000;
let historyLoaded = false,
  historyFilterReset = () => {};
function applyHistoryPage(d, mode) {
  historyLoaded = true;
  const a = d.history,
    t = d.total;
  R.noteBootId(d.bootId);
  if (mode === "append" && t) {
    const s = {};
    for (const x of R.uiState.historyData.records) s[x.id] = 1;
    let n = 0;
    for (const x of a)
      if (!s[x.id]) {
        s[x.id] = 1;
        R.uiState.historyData.records.push(x);
        n++;
      }
    R.uiState.historyData.total = t;
    R.uiState.historyData.hasMore = !!n && !!d.hasMore;
    return;
  }
  R.uiState.historyData = {
    bootId: d.bootId,
    total: t,
    hasMore: !!d.hasMore && !!t,
    records: t ? a : [],
  };
}
const historyViewActive = () => R.activeView === "history" && R.webUiPollingActive();
const loadMoreHistory = () => {
  if (
    R.uiState.historyData.hasMore &&
    historyViewActive() &&
    historyFetchOffset < 0 &&
    !R.uiState.historyExportWindow
  ) {
    historyFetchOffset = R.uiState.historyData.records.length;
    historyFetchRequest = ++R.uiState.historyNextRequest;
    R.sendUiOperation({
      op: "history",
      on: true,
      fetch: true,
      request: historyFetchRequest,
      offset: historyFetchOffset,
    });
  }
};
const refreshHistory = () =>
  historyViewActive() ? startHistoryStream(true) : Promise.resolve(false);
// Request IDs separate the standing History window from scroll fetches.
// historySynced marks a cached model that reflects a completed page
// from the current subscription generation, so warm view entries can paint
// from it; every subscribe or resync drops it until a page lands again.
let historyStreamWanted = false,
  historySynced = false,
  historyFetchOffset = -1,
  historyStreamBoot = 0,
  historyStreamEpoch = 0,
  historyRequest = 0,
  historyFetchRequest = 0,
  historyResolve = null;
// The subscribe op: a fresh request id separates its page from older frames.
function historySubscribeOp() {
  R.uiState.historyExportWindow?.resolve(null);
  historyFetchOffset = -1;
  historySynced = false;
  historyRequest = ++R.uiState.historyNextRequest;
  return {
    op: "history",
    on: true,
    request: historyRequest,
    offset: 0,
    limit: HISTORY_PAGE_SIZE,
    dir: R.uiState.historyDir,
    ...(R.uiState.historyTypes.length && { types: R.uiState.historyTypes }),
  };
}
function historySendSubscribe() {
  if (R.shotWs?.readyState === 1) R.sendUiOperation(historySubscribeOp());
}
// The History subscription is session-standing: once ensured it stays on for
// the whole UI session and socket.onopen replays it after a reconnect, so the
// cached page keeps refreshing regardless of the active view.
function ensureHistoryStream(resubscribe) {
  if (resubscribe || !historyStreamWanted) {
    historyStreamWanted = true;
    if (R.shotWs?.readyState === 1) historySendSubscribe();
  }
}
function startHistoryStream(force) {
  if (!R.webUiPollingActive()) return Promise.resolve(false);
  return R.viewReady.then(() => {
    if (!R.webUiPollingActive() || R.activeView !== "history") return false;
    // Warm entry: the standing subscription keeps the model current, so paint
    // it without an op or a snapshot wait. Forced refreshes, first opens,
    // reconnects, and pending resyncs take the subscribe-and-wait path.
    if (
      !force &&
      historyStreamWanted &&
      historySynced &&
      historyLoaded &&
      R.shotWs?.readyState === 1 &&
      !R.shotResync
    ) {
      renderHistory();
      R.updateFirmwareFooter();
      R.noteReachOk();
      return true;
    }
    historyResolve?.(false);
    const ready = new Promise((r) => (historyResolve = r));
    ensureHistoryStream(force);
    return ready;
  });
}
function historyStreamFrame(message) {
  if (
    message.v !== 1 ||
    !Number.isInteger(message.boot) ||
    typeof message.snapshot !== "boolean" ||
    !Number.isInteger(message.epoch) ||
    !Number.isInteger(message.request) ||
    !Number.isInteger(message.total) ||
    message.total < 0 ||
    !Number.isInteger(message.offset) ||
    message.offset < 0 ||
    !Number.isInteger(message.limit) ||
    message.limit < 1 ||
    message.limit > 120 ||
    typeof message.hasMore !== "boolean" ||
    !Array.isArray(message.records)
  )
    throw Error();
  if (!message.snapshot && message.boot !== historyStreamBoot) throw Error();
  // A standing refresh names records this page already holds by their id.
  const held = !message.snapshot && R.uiState.historyData?.records;
  message.records = message.records.map(
    (r) => (Number.isInteger(r) && held && held.find((x) => x.id === r)) || r,
  );
  if (
    message.offset + message.records.length > message.total ||
    message.hasMore !== message.offset + message.records.length < message.total
  )
    throw Error();
  for (const r of message.records) {
    if (
      !r ||
      !Number.isInteger(r.id) ||
      r.id <= 0 ||
      typeof r.type !== "string" ||
      typeof r.durationS !== "number" ||
      !Number.isFinite(r.durationS) ||
      typeof r.hasWallTime !== "boolean" ||
      !Number.isInteger(r.endedAtUnixSec) ||
      !Number.isInteger(r.endedAtLocalSec)
    )
      throw Error();
  }
  return message;
}
function applyHistoryStream(message) {
  if (R.uiState.historyExportWindow && message.request === R.uiState.historyExportWindow.request) {
    R.uiState.historyExportWindow.resolve(message);
    return;
  }
  const fetch = historyFetchOffset >= 0 && message.request === historyFetchRequest;
  if (fetch && message.epoch !== historyStreamEpoch) {
    historySendSubscribe();
    return;
  }
  const mode = fetch ? "append" : "replace";
  applyHistoryPage(
    {
      bootId: message.boot,
      total: message.total,
      hasMore: message.hasMore,
      history: message.records,
    },
    mode,
  );
  historyStreamBoot = message.boot;
  historyStreamEpoch = message.epoch;
  historyFetchOffset = -1;
  // The model updates on every view; only the hidden view's DOM work and
  // reachability signal wait until History is actually open.
  if (historyViewActive()) {
    R.paint(renderHistory);
    R.updateFirmwareFooter();
    R.noteReachOk();
  }
  if (mode !== "append") {
    historySynced = true;
    historyResolve?.(true);
  }
}
const historyStream = {
  subscribe: () => (historyStreamWanted ? historySubscribeOp() : null),
  frame(data) {
    if (
      !historyStreamWanted ||
      !(
        data.request === historyRequest ||
        (historyFetchOffset >= 0 && data.request === historyFetchRequest) ||
        (R.uiState.historyExportWindow && data.request === R.uiState.historyExportWindow.request)
      )
    )
      return;
    applyHistoryStream(historyStreamFrame(data));
  },
  reset() {
    historyResolve?.(false);
    this.resync();
  },
  resync() {
    historyFetchOffset = -1;
    historySynced = false;
  },
  leave() {
    historyResolve?.(false);
    historyResolve = null;
  },
  rejected: () => historyFilterReset(),
  refresh: refreshHistory,
};
let ready = false;
export function init() {
  if (ready) return;
  ready = true;
  R.registerStream("history", historyStream);
  entryMenu = actionMenu(__WEBUI_TEXT__("history.entry_actions"), (r) => [
    [ICON_TRASH, __WEBUI_TEXT__("history.delete_entry"), "", () => deleteOneHistory(r.id), true],
  ]);
  historyFilterReset = createFilterMenu(
    "historyFilterButton",
    __WEBUI_TEXT__("history.filter_types"),
    () => [
      {
        key: "type",
        options: HISTORY_TYPE_KEYS.map((t) => [t, historyTypeLabel(t), HIST_TYPE_SVG[t] || ""]),
      },
    ],
    (sel) => {
      R.uiState.historyTypes = sel.type || [];
      historySendSubscribe();
    },
  );
  $("clearHistoryButton").onclick = clearActivationHistory;
  $("exportHistoryButton").onclick = exportActivationHistory;
  const h = $("historySort");
  if (h) {
    $("historyDirButton").onclick = toggleHistoryDir;
    syncHistoryDirButton();
  }
  armListSentinel("historySentinel", loadMoreHistory);
  R.armStickyBar("historyStickySentinel");
}
// The route opens the page: a cold entry subscribes and waits for its page.
export const start = startHistoryStream;
