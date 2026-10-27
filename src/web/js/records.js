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
    panel.querySelector('[role^="menuitem"]')?.focus();
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
