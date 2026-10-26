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
// separator, and [icon path, title, hint, run, danger] is an item.
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
      const [icon, title, hint, run, danger] = item,
        b = document.createElement("button");
      b.type = "button";
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
