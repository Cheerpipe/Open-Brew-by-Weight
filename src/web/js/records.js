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
      (below >= panel.offsetHeight || below >= r.top
        ? r.bottom + 8
        : Math.max(8, r.top - panel.offsetHeight - 8)) + "px";
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
// The offline copy (runtime.js syncOfflineCopy): each store's records in
// IndexedDB, tagged with the revision the firmware's record index names, and
// the store epoch they match under "meta". The index lists [id, revision]
// pairs, newest first, only when the epoch moved, so an unchanged store costs
// one small frame and a changed one fetches only its new or changed records.
let offlineDbOpen = null;
function offlineDb() {
  return (offlineDbOpen ||= new Promise((resolve, reject) => {
    const q = indexedDB.open("obbw-records", 1);
    q.onupgradeneeded = () => {
      for (const s of ["shots", "history"]) q.result.createObjectStore(s, { keyPath: "id" });
      q.result.createObjectStore("meta");
    };
    q.onsuccess = () => {
      // Signing out deletes the database; this connection must not hold it.
      q.result.onversionchange = () => {
        q.result.close();
        offlineDbOpen = null;
      };
      resolve(q.result);
    };
    q.onerror = () => {
      offlineDbOpen = null;
      reject(q.error);
    };
  }));
}
// One transaction; resolves with the result of the request `work` returns.
function offlineTx(stores, mode, work) {
  return offlineDb().then(
    (db) =>
      new Promise((resolve, reject) => {
        const tx = db.transaction(stores, mode),
          q = work(tx);
        tx.oncomplete = () => resolve(q?.result);
        tx.onerror = tx.onabort = () => reject(tx.error);
      }),
  );
}
function offlineRecords(store) {
  return offlineTx(store, "readonly", (tx) => tx.objectStore(store).getAll()).then(
    (rows) => rows.sort((a, b) => b.id - a.id),
    () => [],
  );
}
function offlineMeta(store) {
  return offlineTx("meta", "readonly", (tx) => tx.objectStore("meta").get(store)).then(
    (m) => m || {},
    () => ({}),
  );
}
const recordsIndexWait = {};
function recordsIndex(store, epoch) {
  R.registerStream("records", {
    subscribe: () => null,
    cursor: true,
    frame: (d) => recordsIndexWait[d.store]?.(d),
  });
  return new Promise((resolve) => {
    const done = (d) => {
      clearTimeout(timer);
      delete recordsIndexWait[store];
      resolve(d);
    };
    const timer = setTimeout(() => done(null), 15e3);
    recordsIndexWait[store] = done;
    R.sendUiOperation({ op: "records", [store]: epoch });
  });
}
// fetchWindow(offset, count) resolves with records from `offset` in the
// view's fetch order (falsy on failure), place(i, total) maps index position
// i to that order, and meta() adds what the offline page shows besides rows.
// An interrupted sync keeps what it fetched and leaves the epoch for the next;
// a shot starting pauses it that way (the firmware serves no exports then).
// The copy keeps shot rows only, never their traces.
const offlineSyncing = {};
async function syncOfflineStore(store, fetchWindow, place, meta = () => ({})) {
  if (offlineSyncing[store]) return;
  offlineSyncing[store] = true;
  try {
    const held = await offlineMeta(store),
      index = await recordsIndex(store, held.epoch || 0),
      rows = index?.rows;
    if (!index || index.v !== 1 || !Number.isInteger(index.epoch)) return;
    if (!rows) return void (index.epoch === held.epoch && R.noteOfflineCopy());
    if (!Array.isArray(rows) || rows.length % 2 || !rows.every(Number.isInteger)) return;
    const rev = new Map(),
      have = new Map((await offlineRecords(store)).map((r) => [r.id, r._rev])),
      wanted = new Set(),
      offsets = [],
      got = [];
    for (let i = 0; i < rows.length; i += 2) {
      rev.set(rows[i], rows[i + 1]);
      if (have.get(rows[i]) === rows[i + 1]) continue;
      wanted.add(rows[i]);
      offsets.push(place(i / 2, rows.length / 2));
    }
    offsets.sort((a, b) => a - b);
    for (let k = 0; k < offsets.length && R.webUiPollingActive() && !R.shotRunning(); ) {
      const page = await fetchWindow(offsets[k], offsets.at(-1) - offsets[k] + 1);
      if (!page) break;
      for (const r of page)
        if (wanted.delete(r.id)) got.push(Object.assign(r, { _rev: rev.get(r.id) }));
      const end = offsets[k] + Math.max(page.length, 1);
      while (offsets[k] < end) k++;
    }
    await offlineTx([store, "meta"], "readwrite", (tx) => {
      const s = tx.objectStore(store);
      for (const r of got) s.put(r);
      for (const id of have.keys()) if (!rev.has(id)) s.delete(id);
      if (!wanted.size) tx.objectStore("meta").put({ epoch: index.epoch, ...meta() }, store);
    });
    if (!wanted.size) R.noteOfflineCopy();
  } catch (_) {
    /* the next page load or finished activation retries */
  } finally {
    offlineSyncing[store] = false;
  }
}
// The companion archive (plan 2A). Through the companion a list is one
// history: the controller's rows and the records only the companion keeps
// (older ones, earlier histories before a Clear or a factory reset, records
// removed from the controller but kept), filtered and ordered alike on both
// sides and merged page by page. The companion answers only a screen the
// controller lets in.
const archiveSources = {};
// The archive answers for a store (its archive-only rows feed the list) and
// for the whole-history Stats header (any copied shot).
function archiveReady(store) {
  const a = R.uiState.companionHere?.archive;
  return !!(a?.available && a[store] > 0);
}
function archiveStatsReady() {
  const a = R.uiState.companionHere?.archive;
  return !!(a?.available && a.on && (a.shotsTotal || a.shots) > 0);
}
// After an archive change (a removal, an erase, a Clear, a rating in rating
// order) or a moved store epoch: the companion's facts and the store's
// archive rows load again, in place, and once more when the companion's next
// sync pass has ended, since it copies a change only after the controller's
// next frame (D5, S17).
function archiveChanged(store, render) {
  const since = R.uiState.companionHere?.archive?.syncedAt ?? 0;
  R.companionHere(true).then(() => reloadArchive(store, render));
  if (archiveWatch[store]) return;
  // Polls the companion (never the controller) until its pass ended; the
  // notice that starts one comes within about 2.5 minutes (D5).
  let tries = 36;
  archiveWatch[store] = setInterval(async () => {
    const a = (await R.companionHere(true))?.archive;
    if (--tries > 0 && !(a && !a.syncing && (a.syncedAt ?? 0) > since)) return;
    clearInterval(archiveWatch[store]);
    delete archiveWatch[store];
    if (a) reloadArchive(store, render);
  }, 5e3);
}
const archiveWatch = {};
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
  if (!archiveReady(store)) return null;
  const key = JSON.stringify(query);
  if (archiveSources[store]?.key !== key) archiveSources[store] = archiveFresh(key, query);
  return archiveSources[store];
}
async function archivePage(store, s) {
  const q = new URLSearchParams({ limit: store === "shots" ? 20 : 50, ...s.query });
  if (s.next) q.set("cursor", s.next);
  const j = await companionArchive(store + "?" + q);
  // Not enumerable: exports and the offline copy never see it.
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
  } catch (_) {
    // What is loaded stays; the page is asked again a little later each
    // time, and after a few tries the list ends there.
    if (++s.failures >= 4) s.done = true;
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
  // A kept record is archive-only from the moment it is kept; while the
  // controller still lists it (its delete on the way), its own row stands.
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
// Rows kept only by the companion are marked so, never by colour alone.
function archiveMark(el, r) {
  if (!r._archive) return;
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
// One record's deletion scopes for its row menu: an archive row is the
// companion's alone; through the companion a record the controller holds can
// also leave one place only (S5).
function deleteScopes(r, label, run) {
  if (r._archive)
    return [
      [ICON_TRASH, __WEBUI_TEXT__("records.delete_archive_row"), "", () => run("archive"), true],
    ];
  if (!archiveHere()) return [[ICON_TRASH, label, "", () => run("everywhere"), true]];
  return [
    [ICON_TRASH, __WEBUI_TEXT__("records.delete_everywhere"), "", () => run("everywhere"), true],
    [
      ICON_TRASH,
      __WEBUI_TEXT__("records.delete_keep"),
      __WEBUI_TEXT__("records.delete_keep_hint"),
      () => run("controller"),
      true,
    ],
    [
      ICON_TRASH,
      __WEBUI_TEXT__("records.delete_archive_only"),
      __WEBUI_TEXT__("records.delete_archive_only_hint"),
      () => run("archive"),
      true,
    ],
  ];
}
function confirmRemoval(r, scope, plain) {
  const text = r._archive
    ? __WEBUI_TEXT__("records.delete_archive_row_confirm")
    : scope === "archive"
      ? __WEBUI_TEXT__("records.delete_archive_only_confirm")
      : scope === "controller"
        ? __WEBUI_TEXT__("records.delete_keep_confirm")
        : plain +
          (archiveHere()
            ? " " + __WEBUI_TEXT__("records.delete_everywhere_note")
            : archiveDirect()
              ? " " + __WEBUI_TEXT__("records.delete_direct_note")
              : "");
  return R.askConfirm(text, __WEBUI_TEXT__("records.delete"), true);
}
// Runs a scope on a record. The controller goes first, so a refused delete (a
// shot running) leaves the archive as it was; `drop()` removes the row from
// the controller's list, `render()` repaints.
async function removeRecord(store, r, scope, path, drop, render) {
  const archive = (action) =>
    companionArchive(action, {
      store,
      id: r.id,
      ...(r._archive && action === "delete" && { generation: r.archiveGeneration }),
    });
  if (scope === "archive")
    try {
      await archive("delete");
      archiveChanged(store, render);
      render();
      return R.message(__WEBUI_TEXT__("records.archive_removed"), "ok");
    } catch (e) {
      return R.message(R.sub(__WEBUI_TEXT__("records.archive_failed"), [e.message]), "error");
    }
  // Kept first, so the archive never infers a deletion; a record it has not
  // copied yet cannot be kept and stays on the controller (the refusal says so).
  if (scope === "controller") await archive("keep");
  try {
    await R.api(path, { method: "POST", body: R.body({ id: r.id }) });
  } catch (e) {
    if (scope === "controller")
      await companionArchive("keep", { store, id: r.id, kept: false }).catch(() => {});
    throw e;
  }
  if (scope === "everywhere" && archiveHere()) await archive("delete").catch(() => {});
  if (archiveHere()) archiveChanged(store, render);
  drop();
  render();
}
// Clear history's confirmation: through the companion it may also delete the
// companion's copy of the current history (unchecked); opened directly it
// says the companion keeps its copy (S7). Resolves false, true, or with the
// option the copy to delete: the companion's history open at this moment, so
// a restart the companion notes before the erase arrives cannot redirect it.
async function confirmClear(plain, store) {
  if (archiveHere()) {
    const open = (R.uiState.companionHere?.archive?.generations?.[store] || []).find(
        (g) => g.current,
      ),
      answer = await R.askConfirm(
        plain + " " + __WEBUI_TEXT__("records.clear_kept_here"),
        __WEBUI_TEXT__("records.clear"),
        true,
        __WEBUI_TEXT__("records.clear_with_copy"),
      );
    return answer === "option" ? { copy: true, generation: open?.generation } : answer;
  }
  return R.askConfirm(
    plain + (archiveDirect() ? " " + __WEBUI_TEXT__("records.clear_kept_direct") : ""),
    __WEBUI_TEXT__("records.clear"),
    true,
  );
}
async function clearArchiveCopy(store, answer, render) {
  if (answer?.copy)
    await companionArchive(
      "erase",
      Number.isInteger(answer.generation)
        ? { store, generation: answer.generation }
        : { store, current: true },
    ).catch((e) =>
      R.message(R.sub(__WEBUI_TEXT__("records.archive_failed"), [e.message]), "error"),
    );
  if (archiveHere()) archiveChanged(store, render);
}
// Opened directly, the end of the list says the companion keeps a copy (S11).
const archiveListNote = (id, more) => ($(id).hidden = more || !archiveDirect());
