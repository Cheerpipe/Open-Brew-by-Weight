"use strict";
// Shared by the Stats and History views of the records bundle: the generator
// evaluates this file at the bundle's module scope ahead of both views, so
// they read these names directly.
import * as R from "./runtime.js?v=__FW_ASSET_TAG__";
const $ = R.$;
const ICON_TRASH = "M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3";
// Arms a list-end sentinel once so its view loads the next page whenever
// the sentinel scrolls close enough to the viewport.
function armListSentinel(id, onLoad) {
  const el = $(id);
  if (!el || el.dataset.armed) return;
  el.dataset.armed = 1;
  new IntersectionObserver((es) => es.some((e) => e.isIntersecting) && onLoad(), {
    rootMargin: "240px",
  }).observe(el);
}
// A record list's filter menu, anchored to its toolbar button: checkbox
// groups under a Reset header. An empty group does not restrict and the
// button carries the selected count. The firmware filters the pages, so
// onChange(selection) only re-subscribes; groups() returns
// [{key, label?, options: [[value, text, iconHtml?]]}] and is read on every
// open, so the options follow the data.
// Returns the menu's Reset, which the view also runs when the firmware rejects
// a list subscription.
function createFilterMenu(buttonId, title, groups, onChange) {
  const sel = {};
  let panel = null;
  const count = () => Object.values(sel).reduce((n, s) => n + s.size, 0);
  const sync = () => {
    const b = $(buttonId),
      n = count(),
      chip = b?.querySelector(".histFilterCount");
    if (!b) return;
    b.setAttribute("aria-pressed", !!n);
    b.classList.toggle("btnFilterOn", !!n);
    if (chip) Object.assign(chip, { textContent: n, hidden: !n });
  };
  const changed = () => {
    if (panel)
      for (const box of panel.querySelectorAll("input"))
        box.checked = !!sel[box.name]?.has(box.value);
    sync();
    onChange(Object.fromEntries(Object.entries(sel).map(([k, s]) => [k, [...s]])));
  };
  const clear = () => {
    for (const k in sel) sel[k].clear();
    changed();
  };
  const popup = createPopup(title, (p) => {
    const head = document.createElement("div"),
      reset = document.createElement("button");
    panel = p;
    head.className = "histFilterHead";
    reset.type = "button";
    reset.className = "histFilterReset";
    reset.textContent = __WEBUI_TEXT__("history.filter_reset");
    reset.onclick = clear;
    head.append(Object.assign(document.createElement("span"), { textContent: title }), reset);
    p.append(head);
    for (const g of groups()) {
      if (g.label)
        p.append(
          Object.assign(document.createElement("div"), {
            className: "histFilterGroup",
            textContent: g.label,
          }),
        );
      for (const [value, text, icon] of g.options) {
        const item = document.createElement("label"),
          box = document.createElement("input");
        item.className = "histFilterItem";
        box.type = "checkbox";
        box.name = g.key;
        box.value = value;
        box.checked = !!sel[g.key]?.has(box.value);
        box.setAttribute("role", "menuitemcheckbox");
        item.append(box);
        if (icon != null) {
          const i = document.createElement("span");
          i.className = "histFilterIcon";
          i.setAttribute("aria-hidden", "true");
          i.innerHTML = icon;
          item.append(i);
        }
        item.append(Object.assign(document.createElement("span"), { textContent: text }));
        p.append(item);
      }
    }
    // Picks are the shown boxes, so a stale one never filters unseen.
    const pick = () => {
      for (const k in sel) sel[k].clear();
      for (const b of p.querySelectorAll(":checked")) (sel[b.name] ??= new Set()).add(b.value);
    };
    p.onchange = () => (pick(), changed());
    const n = count();
    pick();
    count() < n && changed();
  });
  $(buttonId).onclick = () => popup.open($(buttonId));
  sync();
  return clear;
}
// The popup panel shared by the filter and action menus: built by fill(panel,
// ...args) on every open, placed below its trigger (above when that fits
// better), and closed by a second click on the trigger, a click elsewhere,
// Escape, or the trigger leaving the page.
function createPopup(label, fill) {
  let panel = null,
    anchor = null;
  const shown = () => panel && !panel.hidden;
  const place = () => {
    const r = anchor.getBoundingClientRect(),
      below = innerHeight - r.bottom;
    panel.style.top =
      Math.max(
        8,
        Math.min(
          below >= panel.offsetHeight || below >= r.top
            ? r.bottom + 8
            : r.top - panel.offsetHeight - 8,
          innerHeight - panel.offsetHeight - 8,
        ),
      ) + "px";
    panel.style.left =
      Math.min(
        Math.max(8, r.left + r.width / 2 - panel.offsetWidth / 2),
        innerWidth - panel.offsetWidth - 8,
      ) + "px";
  };
  const close = (focus) => {
    if (!shown()) return;
    panel.hidden = true;
    anchor.setAttribute("aria-expanded", "false");
    if (focus) anchor.focus();
  };
  const open = (trigger, ...args) => {
    const again = shown() && anchor === trigger;
    close();
    if (again) return;
    panel?.remove();
    panel = document.createElement("div");
    panel.className = "histFilterPanel";
    panel.setAttribute("role", "menu");
    panel.setAttribute("aria-label", label);
    fill(panel, ...args);
    document.body.append(panel);
    anchor = trigger;
    anchor.setAttribute("aria-expanded", "true");
    place();
    // Focus the panel host, not an item: an item focused by script matches
    // :focus-visible after any keyboard use, ringing a pointer-opened menu.
    panel.tabIndex = -1;
    panel.focus();
  };
  document.addEventListener("click", (e) => {
    if (shown() && !panel.contains(e.target) && !anchor.contains(e.target)) close();
  });
  document.addEventListener("keydown", (e) => e.key === "Escape" && close(true));
  const reposition = () => shown() && (anchor.isConnected ? place() : close());
  window.addEventListener("scroll", reposition, { passive: true });
  window.addEventListener("resize", reposition, { passive: true });
  return { open, close };
}
// An action menu on the shared popup: a string starts a group, null draws a
// separator, and [icon path, title, hint, run, danger, disabled] is an item
// (a disabled one says why in its hint).
function actionMenu(label, items) {
  const menu = createPopup(label, (p, ...args) => {
    for (const item of items(...args)) {
      if (item === null) {
        p.append(document.createElement("hr"));
        continue;
      }
      if (typeof item === "string") {
        p.append(
          Object.assign(document.createElement("div"), {
            className: "histFilterGroup",
            textContent: item,
          }),
        );
        continue;
      }
      const [icon, title, hint, run, danger, off] = item,
        b = document.createElement("button");
      b.type = "button";
      b.disabled = !!off;
      b.className = "histFilterItem menuAction" + (danger ? " menuDanger" : "");
      b.setAttribute("role", "menuitem");
      b.innerHTML =
        '<span class="histFilterIcon" aria-hidden="true"><svg class="o" viewBox="0 0 24 24"><path d="' +
        icon +
        '"/></svg></span><span class="menuText"><b></b><small></small></span>';
      b.querySelector("b").textContent = title;
      b.querySelector("small").textContent = hint;
      b.onclick = () => (menu.close(), run());
      p.append(b);
    }
  });
  return menu;
}
// CSV for spreadsheets: a BOM marks it UTF-8, fields with quotes, commas or
// line breaks are quoted, and text a spreadsheet would run as a formula
// (=, +, -, @) gets a leading apostrophe; numbers stay as they are.
function downloadCsv(name, rows) {
  const cell = (v) => {
    let s = String(v);
    if (/^[=+\-@\t\r]/.test(s) && !Number.isFinite(+s)) s = "'" + s;
    return /[",\r\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
  };
  R.saveFile(
    new Blob(["\uFEFF" + rows.map((r) => r.map(cell).join(",")).join("\n")], { type: "text/csv" }),
    name,
  );
}
// The companion archive (plan 2A). Through the companion with the shot
// archive on, the companion's own pages are one history: the controller's
// rows and the records only the companion keeps (older ones the controller
// let go, and earlier histories from before a factory reset) arrive in one
// list, the latter marked archiveOnly (streamArchive). With the controller
// off (R.archiveView) the list is the archive alone, read here page by page:
// every record it copied, read-only. The companion answers only a screen
// the controller lets in.
const archiveSources = {};
// The archive answers for a store (its archive-only rows feed the list or,
// with the controller off and Shot archive in the companion on, every copied
// row) and for the whole-history Stats header (any copied shot).
function archiveReady(store) {
  const a = R.uiState.companionHere?.archive;
  return !!(
    a?.available &&
    (!R.archiveView || a.on) &&
    a[R.archiveView ? store + "Total" : store] > 0
  );
}
function archiveStatsReady() {
  const a = R.uiState.companionHere?.archive;
  return !!(a?.available && a.on && (a.shotsTotal || a.shots) > 0);
}
// After an archive change (a deletion, a rating): the companion's facts load
// again; its pages follow on their own, since the companion sends the open
// lists again once its copy caught up.
function archiveChanged(store, render) {
  R.companionHere(true).then(() => reloadArchive(store, render));
}
// Rows the companion alone keeps, on its pages: each is marked as kept by it
// (archiveMark) and the page names its histories and gaps (`archive`) for the
// lines between them (archiveHeadings).
const streamArchives = {};
function streamArchive(store, message, rows) {
  if (message.archive)
    streamArchives[store] = {
      gens: message.archive.generations || [],
      holes: message.archive.holes || [],
    };
  for (const r of rows)
    if (r?.archiveOnly && !r._archive)
      Object.defineProperty(r, "_archive", { value: streamArchives[store] || {} });
}
async function companionArchive(path, body) {
  const reply = await fetch("/__companion/archive/" + path, {
    cache: "no-store",
    ...(body && { method: "POST", body: JSON.stringify(body) }),
    headers: R.webUiHeaders(body && { "Content-Type": "application/json" }),
  });
  const data = await reply.json().catch(() => null);
  if (!reply.ok)
    throw Object.assign(Error(data?.message || __WEBUI_TEXT__("records.archive_try_again")), data);
  return data;
}
// The archive's rows for this view's filter and order (a change starts over).
const archiveFresh = (key, query) => ({
  key,
  query,
  rows: [],
  next: null,
  done: false,
  failures: 0,
});
function archiveSource(store, query) {
  if (!R.archiveView || !archiveReady(store)) return null;
  query = { ...query, all: 1 };
  const key = JSON.stringify(query);
  if (archiveSources[store]?.key !== key) archiveSources[store] = archiveFresh(key, query);
  return archiveSources[store];
}
async function archivePage(store, s) {
  const q = new URLSearchParams({ limit: store === "shots" ? 20 : 50, ...s.query });
  if (s.next) q.set("cursor", s.next);
  const j = await companionArchive(store + "?" + q);
  // Not enumerable: exports never see it.
  s.rows.push(...j.rows.map((r) => Object.defineProperty(r, "_archive", { value: s })));
  Object.assign(s, { next: j.next, done: !j.next, holes: j.holes || [] });
  Object.assign(s, { gens: j.generations || [], presets: j.presets || [] });
}
async function loadArchive(store, render) {
  const s = archiveSources[store];
  if (!s || s.busy || s.done) return;
  s.busy = true;
  try {
    await archivePage(store, s);
    s.failures = 0;
  } catch (e) {
    // A browser the companion has not seen signed in is told how to get in.
    // Otherwise what is loaded stays; the page is asked again a little later
    // each time, and after a few tries the list ends there.
    if (e.error === "AUTH_REQUIRED") {
      s.done = true;
      R.message(__WEBUI_TEXT__("records.archive_sign_in"), "error");
    } else if (++s.failures >= 4) s.done = true;
    else setTimeout(() => loadArchive(store, render), 2e3 * 2 ** s.failures);
  } finally {
    s.busy = false;
  }
  if (archiveSources[store] === s) render();
}
// The store's archive rows again, as deep as the list had loaded them; the
// loaded rows stay on screen until the new ones are whole, so the list never
// jumps back. A source that is gone (or new) loads with the next render.
async function reloadArchive(store, render) {
  const old = archiveSources[store];
  if (old && archiveReady(store)) {
    const s = archiveFresh(old.key, old.query);
    try {
      do await archivePage(store, s);
      while (!s.done && s.rows.length < old.rows.length);
    } catch (_) {
      return;
    }
    if (archiveSources[store] !== old) return;
    archiveSources[store] = s;
  } else delete archiveSources[store];
  render?.();
}
// The view's order over both sources, the controller's own: by date the
// history generation and then the id (ids restart with a generation); by
// rating unrated last, the rating in `dir`, then generation and id newest
// first. Controller rows belong to the companion's open generation.
function recordOrder(s, sort, dir) {
  const open = s?.gens?.find((g) => g.current)?.generation ?? Infinity,
    gen = (r) => (r._archive ? r.archiveGeneration : open),
    newest = (a, b) => gen(b) - gen(a) || b.id - a.id;
  if (sort !== "rating") return dir === "asc" ? (a, b) => newest(b, a) : newest;
  return (a, b) =>
    !a.rating - !b.rating ||
    (dir === "asc" ? a.rating - b.rating : b.rating - a.rating) ||
    newest(a, b);
}
// The merged list: every row up to the first one a source still has to send,
// and which source pages next (`need`).
function mergeRecords(own, ownMore, s, order) {
  if (!s || (!s.rows.length && !s.done))
    return { list: own, need: s && !s.done ? "archive" : null };
  // A record both hold shows the controller's row.
  const open = s.gens?.find((g) => g.current)?.generation,
    held = new Set(own.map((r) => r.id)),
    rows = s.rows.filter((r) => r.archiveGeneration !== open || !held.has(r.id)),
    list = own.concat(rows).sort(order),
    cut = (more, part) => (more && part.length ? list.indexOf(part.at(-1)) + 1 : list.length),
    ownCut = cut(ownMore, own),
    archiveCut = cut(!s.done, rows);
  return {
    list: list.slice(0, Math.min(ownCut, archiveCut)),
    need: ownMore && ownCut <= archiveCut ? "own" : s.done ? null : "archive",
  };
}
// Rows kept only by the companion are marked so, never by colour alone; in the
// archive view the companion names them (archiveOnly) among the rows the
// controller holds.
function archiveMark(el, r) {
  if (!r._archive || r.archiveOnly === false) return;
  // A closed history ended when the next existing one opened (its restartedAt).
  const before = (r._archive.gens || [])
    .filter((g) => g.generation > r.archiveGeneration)
    .sort((a, b) => a.generation - b.generation)[0]?.restartedAt;
  const kept =
    __WEBUI_TEXT__("records.archive_kept") +
    (before ? " · " + R.sub(__WEBUI_TEXT__("records.archive_before"), [archiveDate(before)]) : "");
  el.classList.add("archiveKept");
  el.title = (el.title ? el.title + " · " : "") + kept;
  // Read aloud too: who keeps it, never only its colour.
  el.append(
    Object.assign(document.createElement("span"), {
      className: "srText",
      textContent: ", " + kept,
    }),
  );
}
const archiveDate = (at) => new Date(at * 1e3).toLocaleDateString([], { dateStyle: "medium" });
// Date-ordered lists: a line where an earlier history begins and where the
// companion could not copy records. Returns whether it added one.
function archiveHeadings(body, list, i, columns, s, byDate) {
  const r = list[i],
    prev = list[i - 1],
    gen = (x) => (x._archive ? x.archiveGeneration : s?.gens?.find((g) => g.current)?.generation),
    head = (text) => {
      const row = body.insertRow();
      row.className = "histDay";
      row.append(
        Object.assign(document.createElement("th"), { colSpan: columns, textContent: text }),
      );
      return true;
    };
  if (!s || !byDate || !prev) return false;
  let added = false;
  if (gen(prev) !== gen(r)) {
    const newer = Math.max(gen(prev), gen(r)),
      at = s.gens?.find((g) => g.generation === newer)?.restartedAt;
    added = head(
      at
        ? R.sub(__WEBUI_TEXT__("records.archive_restarted"), [archiveDate(at)])
        : __WEBUI_TEXT__("records.archive_earlier"),
    );
  }
  for (const h of s.holes || [])
    if (
      Math.min(prev.id, r.id) <= h.below &&
      Math.max(prev.id, r.id) >= h.above &&
      gen(r) === h.generation &&
      gen(prev) === h.generation
    )
      added = head(R.sub(__WEBUI_TEXT__("records.archive_hole"), [h.count]));
  return added;
}
// The archive's switch as this screen sees it: through the companion, its
// own status; opened directly, the controller's Home model.
const archiveHere = () => !!R.uiState.companionHere?.archive?.on;
// Direct as companionHere decides it: the controller serves plain HTTP only.
const archiveDirect = () =>
  location.protocol !== "https:" && !!R.homeStatusCache()?.companion?.archive;
// A record's Delete, offered only while Admin is unlocked for this screen and
// never in the read-only archive view: one action wherever the record is kept.
const deleteItem = (label, run) =>
  R.archiveView || !R.adminUnlockedHere() ? [] : [[ICON_TRASH, label, "", run, true]];
// Deletes a record, after one confirmation. The controller deletes its own
// records and the companion's copy follows on its next sync; a record only
// the companion keeps (`_archive`) is deleted there. `drop()` removes the row
// from the controller's list, `render()` repaints; a refusal (Admin locked, a
// shot running) throws and leaves the list as it was.
async function removeRecord(store, r, path, drop, render) {
  if (r._archive)
    await companionArchive("delete", { store, id: r.id, generation: r.archiveGeneration });
  else {
    await R.api(path, { method: "POST", body: R.body({ id: r.id }) });
    drop();
  }
  if (archiveHere()) archiveChanged(store, render);
  render();
}
// Opened directly, the end of the list says the companion keeps a copy (S11).
const archiveListNote = (id, more) => ($(id).hidden = more || !archiveDirect());
