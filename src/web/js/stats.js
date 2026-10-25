"use strict";
import * as R from "./runtime.js?v=__FW_ASSET_TAG__";
const $ = R.$;
function formatShotTime(r) {
  if (r.hasWallTime && r.endedAtLocalSec) return R.formatHumanTime(r.endedAtLocalSec);
  return "#" + r.bootId + __WEBUI_TEXT__("runtime.no_time");
}
function lastCurveWeightG(w) {
  if (!Array.isArray(w)) return null;
  for (let i = w.length - 1; i >= 0; i--) {
    if (typeof w[i] === "number" && w[i] !== 0) return w[i] / 100;
  }
  return null;
}
let chartFrame = 0,
  chartObserver;
function layoutChartLabels() {
  if (chartFrame) return;
  chartFrame = requestAnimationFrame(() => {
    chartFrame = 0;
    for (const e of document.querySelectorAll("[data-chart-axis]")) {
      const b = e.getBoundingClientRect();
      if (!b.width || !b.height) continue;
      const tk = [];
      for (const o of e.querySelectorAll("[data-p]")) o.dataset.m ? o.remove() : tk.push(o);
      const rect = (n) => {
        if (!n.dataset.m) n.style.transform = "";
        let r = n.getBoundingClientRect();
        if (n.classList.contains("ruleTick") && (r.left < b.left - 1 || r.right > b.right + 1)) {
          n.style.transform = r.left < b.left ? "translateX(0)" : "translateX(-100%)";
          r = n.getBoundingClientRect();
        }
        return r;
      };
      if (e.dataset.chartMerge !== undefined && tk.length > 1) {
        const R = tk.map(rect),
          runs = [];
        let s = 0;
        for (let i = 1; i <= tk.length; i++)
          if (i === tk.length || R[i].left >= R[i - 1].right + 4) {
            if (i - s > 1) runs.push(tk.slice(s, i));
            s = i;
          }
        for (const ru of runs) {
          const f = ru[0],
            l = ru.at(-1),
            tx = ru.map((x) => x.textContent),
            mm = tx.map((t) => t.split(/ (?=\S+$)/)),
            u = mm[0][1],
            n = document.createElement("span");
          n.className = "ruleTick";
          n.dataset.m = "1";
          n.dataset.p = Math.max(...ru.map((x) => +x.dataset.p)) + 1;
          n.dataset.k = "m" + f.dataset.k + "|" + l.dataset.k;
          n.textContent =
            u && mm.every((p) => p[1] === u)
              ? mm.map((p) => p[0]).join("–") + " " + u
              : tx.join(" – ");
          n.style.left = (parseFloat(f.style.left) + parseFloat(l.style.left)) / 2 + "%";
          n.style.transform = "translateX(-50%)";
          e.appendChild(n);
          tk.push(n);
        }
      }
      const used = [],
        next = new Set(),
        old = e._shown;
      for (const n of tk.sort((a, b) => b.dataset.p - a.dataset.p)) {
        if (n.dataset.k === "tare") {
          const t = n.parentElement;
          t.style.bottom = "";
          // The drop is rendered first; both markers share bottom:10%.
          if (tk.length > 1) {
            const d = e.firstElementChild.getBoundingClientRect(),
              r = t.getBoundingClientRect();
            if (r.left < d.right && d.left < r.right)
              t.style.bottom = "calc(10% + " + (d.height + 8) + "px)";
          }
        }
        const r = rect(n),
          gap = old && !old.has(n.dataset.k) ? 8 : 4,
          ok =
            r.left >= b.left - 1 &&
            r.right <= b.right + 1 &&
            used.every(
              (q) =>
                r.right + gap <= q.left ||
                q.right + gap <= r.left ||
                r.bottom + gap <= q.top ||
                q.bottom + gap <= r.top,
            );
        if (ok) {
          used.push(r);
          next.add(n.dataset.k);
        }
        n.style.visibility = ok ? "visible" : "hidden";
      }
      e._shown = next;
    }
  });
}
function watchChartLabels(e) {
  e.dataset.chartAxis = "";
  if (!chartObserver) {
    // Off Stats the labels are hidden; entering Stats lays them out again.
    chartObserver = new ResizeObserver(() => R.activeView === "stats" && layoutChartLabels());
    chartObserver.observe($("app"));
    document.fonts?.ready.then(layoutChartLabels);
  }
  layoutChartLabels();
}
function fillChartTicks(e, l, m, avoidOverlap, mg) {
  if (!e) return;
  e.replaceChildren();
  if (!(m > 0)) {
    e.removeAttribute("role");
    e.removeAttribute("aria-label");
    delete e.dataset.chartAxis;
    delete e.dataset.chartMerge;
    delete e._shown;
    return;
  }
  const s = {},
    raw = [];
  for (const t of l || []) {
    const a = +t[0];
    if (!isFinite(a)) continue;
    const k = avoidOverlap ? a + "|" + t[1] : ~~(a * 10);
    if (s[k]) continue;
    s[k] = 1;
    raw.push([Math.max(0, Math.min(1, a / m)), "" + t[1], t[2] || 0, t[3] || "" + t[1]]);
  }
  raw.sort((a, b) => a[0] - b[0]);
  if (avoidOverlap) {
    e.setAttribute("role", "img");
    e.setAttribute("aria-label", raw.map((t) => t[1]).join(", "));
  }
  for (const t of raw) {
    const n = document.createElement("span");
    n.className = "ruleTick";
    n.textContent = t[1];
    n.style.left = t[0] * 100 + "%";
    if (avoidOverlap) {
      n.dataset.p = t[2];
      n.dataset.k = t[3];
      n.style.visibility = "hidden";
    }
    e.appendChild(n);
  }
  if (mg) e.dataset.chartMerge = "";
  if (avoidOverlap) watchChartLabels(e);
}
function fixedChartTicks(max, step, unit) {
  const ticks = [];
  for (let v = 0; v <= max + 1e-6; v += step) ticks.push([v, R.axisLabel(v, unit)]);
  return ticks;
}
function shotDisplayActualG(actual, wCg) {
  if (typeof actual === "number" && actual !== 0) return actual;
  const fromCurve = lastCurveWeightG(wCg);
  return fromCurve !== null ? fromCurve : typeof actual === "number" ? actual : null;
}
function shotDisplayFlowGS(shot) {
  if (!shot) return null;
  if (typeof shot.avgFlowGS === "number" && Number.isFinite(shot.avgFlowGS)) return shot.avgFlowGS;
  const actual = shotDisplayActualG(shot.actualG, shot.wCg);
  const dur = typeof shot.durationS === "number" ? shot.durationS : null;
  const drop =
    typeof shot.firstDropS === "number"
      ? shot.firstDropS
      : typeof shot.dropS === "number"
        ? shot.dropS
        : null;
  if (actual == null || dur == null || drop == null || !(dur - drop > 0.5) || !(actual > 0))
    return null;
  return actual / (dur - drop);
}
function fillStarRate(h, r, off, fn) {
  if (!h) return;
  const n = Math.max(0, Math.min(5, +r || 0));
  h.classList.add("starRate");
  h.setAttribute("role", "radiogroup");
  h.setAttribute("aria-label", __WEBUI_TEXT__("runtime.shot_rating"));
  h.dataset.rating = "" + n;
  h.replaceChildren();
  // Radio keyboard pattern: one tab stop, arrows move to and select a star.
  h.onkeydown = (e) => {
    const d = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key],
      j = Math.max(1, Math.min(5, [...h.children].indexOf(e.target) + 1 + d));
    if (!d) return;
    e.preventDefault();
    if (j !== n) fn(j, j);
  };
  for (let i = 1; i <= 5; i++) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = i <= n ? "on" : "";
    b.disabled = !!off;
    b.tabIndex = i === (n || 1) ? 0 : -1;
    b.setAttribute("role", "radio");
    b.setAttribute("aria-checked", i === n);
    b.setAttribute(
      "aria-label",
      i > 1 ? R.sub(__WEBUI_TEXT__("runtime.n_stars"), [i]) : __WEBUI_TEXT__("runtime.one_star"),
    );
    b.innerHTML =
      '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 2.6l2.7 6.2 6.7.7-5.1 4.4 1.5 6.6L12 17.2 6.2 20.5l1.5-6.6L2.6 9.5l6.7-.7z"/></svg>';
    b.onclick = () => fn && fn(i === n ? 0 : i, i);
    h.appendChild(b);
  }
}
function shotMaxFlowGS(shot, model = R.buildShotSparkModel(shot)) {
  return model ? model.maxFlow : null;
}
function shotPresetName(shot) {
  return (shot && shot.presetName) || __WEBUI_TEXT__("runtime.unknown");
}
function formatShotTimeCsv(r) {
  if (r.hasWallTime && r.endedAtLocalSec) return R.formatWallTimeLocal(r.endedAtLocalSec);
  return "";
}
// The stop: its time, the reading latched there and, against a post-drip yield, the drip.
function shotCut(r, m, actual) {
  const p = m && r.endS != null && (m.dripSegs[0]?.pts[0] ?? m.pts.at(-1)),
    g = p && p.cg / 100;
  return p
    ? {
        s: +r.endS,
        g,
        drip: actual !== null && r.actualWeightSource === "post_drip" ? actual - g : null,
      }
    : {};
}
function shotFlowCurveGS(shot, model = R.buildShotSparkModel(shot)) {
  return model ? model.flowCurve : [];
}
// The value of a time-sorted curve at t, linear between samples and held at its ends.
function curveAt(curve, t) {
  let i = 0;
  while (i < curve.length - 2 && curve[i + 1].t < t) i++;
  const a = curve[i],
    b = curve[i + 1] ?? a,
    k = b.t > a.t ? Math.min(1, Math.max(0, (t - a.t) / (b.t - a.t))) : 0;
  return (a.cg + (b.cg - a.cg) * k) / 100;
}
// Both shot charts scrub alike: drag anywhere on the chart, or arrow keys once focused.
function bindShotScrub(node, tAt, seek, now) {
  const move = (e) => seek(tAt(e.clientX));
  node.addEventListener("pointerdown", (e) => {
    if (tAt(e.clientX) == null) return;
    e.preventDefault();
    node.setPointerCapture(e.pointerId);
    node.focus({ preventScroll: true });
    node.classList.add("rvHeld");
    move(e);
  });
  node.addEventListener("pointermove", (e) => node.hasPointerCapture(e.pointerId) && move(e));
  node.addEventListener("lostpointercapture", () => node.classList.remove("rvHeld"));
  node.addEventListener("keydown", (e) => {
    const step = {
      ArrowRight: 0.5,
      ArrowUp: 0.5,
      ArrowLeft: -0.5,
      ArrowDown: -0.5,
      PageUp: 5,
      PageDown: -5,
    }[e.key];
    if (!step) return;
    e.preventDefault();
    seek(now() + step);
  });
  node.tabIndex = 0;
  node.setAttribute("role", "slider");
  node.setAttribute("aria-label", __WEBUI_TEXT__("runtime.shot_cursor_label"));
}
function renderShotSpark(h, shot, m = R.buildShotSparkModel(shot)) {
  if (!h) return null;
  h.replaceChildren();
  if (!m) {
    h.hidden = true;
    return null;
  }
  h.hidden = false;
  const rule = R.shotRuleModel(shot);
  if (rule.mode === "empty") return fillShotCharts(h, m);
  // Both charts span this shot's own max time, or longer when its data runs past it.
  m.timeMax = Math.max(rule.wall, m.dur);
  const preset = document.createElement("div"),
    readout = document.createElement("p"),
    combo = document.createElement("div"),
    // The cursor rides this shot's own curve from its last reading; a drip tail adds a top dot at the real stop.
    curve = m.pts.slice().sort((a, b) => a.t - b.t),
    stop = m.dripSegs.length ? +shot.endS : null;
  let cursorT = curve.at(-1).t;
  const place = () => {
    const v = preset._view,
      g = preset.querySelector(".shotCursor");
    if (!v || !g) return;
    const t = cursorT,
      w = curveAt(curve, t),
      cut =
        rule.mode === "active"
          ? R.cutLine(t, rule).toFixed(1) + __WEBUI_TEXT__("runtime.g_2")
          : "\u2014";
    g.children[0].setAttribute("d", `M${v.X(t).toFixed(1)} ${v.T}V${v.B}`);
    g.children[1].setAttribute("cx", v.X(t).toFixed(2));
    g.children[1].setAttribute("cy", v.Y(w).toFixed(2));
    R.placePill(
      g.children[2],
      R.sub(__WEBUI_TEXT__("runtime.shot_cursor_pill"), [t.toFixed(1), w.toFixed(1)]),
      v.X(t),
      v.Y(w),
    );
    preset.setAttribute("aria-valuenow", t.toFixed(1));
    readout.textContent = R.sub(__WEBUI_TEXT__("runtime.shot_cursor"), [
      t.toFixed(1),
      w.toFixed(1),
      cut,
    ]);
    preset.setAttribute("aria-valuetext", readout.textContent);
  };
  const seek = (t) => {
    cursorT = Math.round(Math.max(curve[0].t, Math.min(curve.at(-1).t, t)) * 10) / 10;
    place();
  };
  const draw = () => {
    preset._w = preset.clientWidth;
    preset._view = R.presetGraph(preset, rule, {
      compact: true,
      tens: true,
      last: m.segs.map((s) => s.pts.map((q) => [q.t, q.cg / 100])),
      drip: m.dripSegs.map((s) => s.pts.map((q) => [q.t, q.cg / 100])),
      held: m.held.map((q) => [q.t, q.cg / 100]),
      maxG: m.rawMaxW,
      maxT: m.timeMax,
    });
    // The weight/flow chart below matches this plot's height, so grams read alike in both.
    if (preset._view) h.style.setProperty("--shot-plot-h", preset._view.B - preset._view.T + "px");
    const v = preset._view;
    preset
      .querySelector("svg")
      ?.insertAdjacentHTML(
        "beforeend",
        `<g class="shotCursor"><path class="rvGuide"/><circle class="rvHandle" r="5"/>${R.PILL}</g>` +
          (stop == null || !v
            ? ""
            : `<circle class="shotStop" r="3" cx="${v.X(stop)}" cy="${v.Y(curveAt(curve, stop))}"/>`),
      );
    place();
  };
  bindShotScrub(
    preset,
    (x) => preset._view?.tAt(x - preset.firstChild.getBoundingClientRect().left),
    seek,
    () => cursorT,
  );
  preset.className = "shotPresetChart";
  preset.setAttribute("aria-valuemin", curve[0].t.toFixed(1));
  preset.setAttribute("aria-valuemax", curve.at(-1).t.toFixed(1));
  readout.className = "shotCursorText";
  // Only the zone the shot ended in keeps its fill (CSS); timer-only shots keep none.
  preset.dataset.zone =
    (shot.shotType || "").toLowerCase() === "timer_only"
      ? "none"
      : shot.verdict === "fast" || shot.verdict === "slow"
        ? shot.verdict
        : "on_recipe";
  // Compact legend (D5): the cut line, only the zone this shot ended in, the shot, and held weight.
  const zone = preset.dataset.zone,
    legend = document.createElement("div");
  legend.className = "shotLegend";
  legend.innerHTML =
    '<span class="rvLeg rvLegCut"><i></i>' +
    __WEBUI_TEXT__("runtime.graph_legend_cut") +
    "</span>" +
    (zone === "fast"
      ? '<span class="rvLeg rvLegFast"><i></i><i></i>' +
        __WEBUI_TEXT__("runtime.graph_legend_fast") +
        "</span>"
      : zone === "slow"
        ? '<span class="rvLeg rvLegSlow"><i></i><i></i>' +
          __WEBUI_TEXT__("runtime.graph_legend_slow") +
          "</span>"
        : zone === "on_recipe"
          ? '<span class="rvLeg rvLegWindow"><i></i>' +
            __WEBUI_TEXT__("runtime.graph_legend_corridor") +
            "</span>"
          : "") +
    '<span class="rvLeg rvLegLast"><i></i>' +
    __WEBUI_TEXT__("runtime.graph_legend_last") +
    "</span>" +
    (stop == null
      ? ""
      : '<span class="rvLeg rvLegStop"><i></i>' +
        __WEBUI_TEXT__("runtime.graph_legend_stopped") +
        '</span><span class="rvLeg rvLegFinal"><i></i>' +
        __WEBUI_TEXT__("runtime.graph_legend_final") +
        "</span>") +
    (m.held.length
      ? '<span class="rvLeg rvLegHeld"><i></i>' +
        __WEBUI_TEXT__("runtime.graph_legend_held") +
        "</span>"
      : "");
  combo.className = "shotComboHost";
  h.append(preset, readout, legend, combo);
  fillShotCharts(combo, m);
  preset._draw = draw;
  R.observePresetHost(preset);
  draw();
}
let fadeSeq = 0;
// One chart for both series: weight on the left axis, flow on the right one,
// sharing the weight grid (10 g per line; flow takes the smallest step that fits).
function fillShotCharts(h, m) {
  const W = 240,
    H = 36,
    p = 1.5,
    X = (t) => p + (W - 2 * p) * Math.min(1, t / m.timeMax),
    Y = (v, max) => H - p - (H - 2 * p) * Math.max(0, Math.min(1, v / max)),
    xt = fixedChartTicks(m.timeMax, 10, "s"),
    yt = fixedChartTicks(m.maxW, 10, __WEBUI_TEXT__("runtime.g")),
    n = m.maxW / 10,
    step = [0.25, 0.5, 1, 2, 5].find((s) => s * n >= m.flowMax) ?? Math.ceil(m.flowMax / n),
    u = "sc" + ++fadeSeq,
    colors = [...new Set(m.segs.filter((s) => s.pts.length > 1).map((s) => s.color))];
  let defs = "";
  function series(segs, drips, max, flow) {
    function path(a, c, drip) {
      const d =
        a
          .map((q, i) => (i ? "L" : "M") + X(q.t).toFixed(1) + " " + Y(q.cg / 100, max).toFixed(1))
          .join(" ") + (a.length === 1 ? "h0" : "");
      let fill = "";
      if (!drip && a.length > 1 && a.some((q) => q.cg > 0)) {
        const id = u + "-" + defs.length;
        defs += R.fadeFill(id, c, flow ? 0.42 : 0.36, flow ? 0.02 : 0);
        fill =
          '<path d="' +
          d +
          "V" +
          (H - p) +
          "H" +
          X(a[0].t).toFixed(1) +
          'Z" fill="url(#' +
          id +
          ')"/>';
      }
      return (
        fill +
        '<path class="shotTrace' +
        (drip ? " shotDripTrace" : "") +
        '" d="' +
        d +
        '" fill="none" stroke="' +
        c +
        '" stroke-width="1.35" stroke-linejoin="round" stroke-linecap="round"' +
        (drip ? ' stroke-dasharray="3 3"' : "") +
        "/>"
      );
    }
    let svg = "";
    if (m.firstDropS > 0)
      svg += path(
        [
          { t: 0, cg: 0 },
          { t: m.firstDropS, cg: 0 },
          ...(flow && m.flowStart ? [m.flowStart] : []),
        ],
        flow ? "var(--gFlow)" : "var(--ok)",
      );
    for (const s of segs) svg += path(s.pts, s.color);
    for (const s of drips) svg += path(s.pts, s.color, true);
    if (!flow && m.held.length)
      svg +=
        '<path class="shotHeld" d="' +
        m.held
          .map((q) => "M" + X(q.t).toFixed(1) + " " + Y(q.cg / 100, max).toFixed(1) + "h0")
          .join("") +
        '" stroke="var(--mu)" stroke-width="2" stroke-linecap="round" opacity=".6" vector-effect="non-scaling-stroke"/>';
    return svg;
  }
  const grid = xt
      .map((q) => "M" + X(q[0]).toFixed(1) + " " + p + "V" + (H - p))
      .concat(
        yt.map(
          (q, i) => "M" + p + " " + (H - p - ((H - 2 * p) * i) / n).toFixed(1) + "H" + (W - p),
        ),
      )
      .join(""),
    ticks = (labels) =>
      labels
        .slice()
        .reverse()
        .map((q) => '<span class="shotYTick">' + q + "</span>")
        .join(""),
    // Flow is drawn first so the weight curve and its fill stay on top.
    body = series(m.flowSegs, m.dripFlowSegs, step * n, true) + series(m.segs, m.dripSegs, m.maxW);
  h.innerHTML =
    '<div class="shotCurve"><div class="shotSparkHost"><div class="shotSparkY">' +
    ticks(yt.map((q) => q[1])) +
    '</div><svg class="shotSpark" viewBox="0 0 ' +
    W +
    " " +
    H +
    '" preserveAspectRatio="none"><defs>' +
    defs +
    '</defs><path class="shotGrid" d="' +
    grid +
    '"/>' +
    body +
    // The cursor lives in its own unstretched layer, so its dots stay round.
    '</svg><svg class="shotSpark shotSparkCursor" aria-hidden="true"><g class="shotCursor"><line class="rvGuide" y1="0" y2="100%"/><circle class="rvHandle" r="5"/><circle class="rvHandle" r="5"/>' +
    R.PILL +
    '</g></svg><div class="shotSparkY shotSparkFlowY">' +
    ticks(yt.map((q, i) => +(i * step).toFixed(2) + " g/s")) +
    "</div>" +
    (m.finalPoint ? '<div class="shotDropOverlay"></div>' : "") +
    '<div class="ruleChartTicks"></div></div><div class="shotLegend">' +
    '<span class="rvLeg"><i class="shotKey"></i>' +
    __WEBUI_TEXT__("home.weight_g") +
    "</span>" +
    '<span class="rvLeg"><i class="shotKey"></i>' +
    __WEBUI_TEXT__("runtime.flow_rate_g_s") +
    "</span>" +
    (m.dripSegs.length || m.dripFlowSegs.length
      ? '<span class="rvLeg shotLegDrip"><i></i>' +
        __WEBUI_TEXT__("runtime.graph_legend_drip") +
        "</span>"
      : "") +
    (m.held.length
      ? '<span class="rvLeg rvLegHeld"><i></i>' +
        __WEBUI_TEXT__("runtime.graph_legend_held") +
        "</span>"
      : "") +
    (m.truncated
      ? '<span class="rvLeg">' + __WEBUI_TEXT__("runtime.graph_legend_incomplete") + "</span>"
      : "") +
    "</div></div>";
  // The firmware CSP drops style attributes parsed from markup; CSSOM writes still apply.
  const c = h.querySelector(".shotSparkHost"),
    [kw, kf] = h.querySelectorAll(".shotKey");
  kw.style.background =
    colors.length > 1
      ? `linear-gradient(90deg,${colors[0]} 50%,${colors.at(-1)} 50%)`
      : colors[0] || "var(--ok)";
  kf.style.background = "var(--gFlow)";
  c.style.setProperty("--shot-plot-min", (n * 1.1).toFixed(2) + "rem");
  for (const axis of c.querySelectorAll(".shotSparkY")) {
    const y = axis.querySelectorAll(".shotYTick");
    // Each axis is as wide as its longest label (about .85ch per glyph), at least the preset chart's 30px.
    axis.style.width = `max(30px,${Math.max(...[...y].map((e) => e.textContent.length)) * 0.85}ch)`;
    y.forEach((e, i) => {
      e.style.top = (i / n) * 100 + "%";
      e.dataset.p = i === n ? 80 : i === 0 ? 70 : 20;
      e.dataset.k = e.textContent;
      e.style.visibility = "hidden";
    });
    axis.setAttribute("role", "img");
    axis.setAttribute("aria-label", [...y].map((e) => e.textContent).join(", "));
    watchChartLabels(axis);
  }
  fillChartTicks(
    c.querySelector(".ruleChartTicks"),
    xt.map((q, i) => [...q, i === 0 ? 80 : i === xt.length - 1 ? 70 : 20, "time-" + q[0]]),
    m.timeMax,
    true,
  );
  if (m.finalPoint) {
    const d = document.createElement("span");
    d.className = "shotFinalPoint";
    d.style.left = (X(m.finalPoint.t) / W) * 100 + "%";
    d.style.top = (Y(m.finalPoint.cg / 100, m.maxW) / H) * 100 + "%";
    Object.assign(d.style, {
      position: "absolute",
      width: "6px",
      height: "6px",
      boxSizing: "border-box",
      border: "1.35px solid " + m.dripSegs.at(-1).color,
      borderRadius: "50%",
      background: "var(--sf)",
      transform: "translate(-50%,-50%)",
      pointerEvents: "none",
    });
    d.setAttribute("aria-hidden", "true");
    h.querySelector(".shotDropOverlay").appendChild(d);
  }
  // Weight and flow dots share the cursor's time; the pill reads both, framed in the weight color.
  const cur = c.querySelector(".shotSparkCursor"),
    [guide, dotW, dotF, pill] = cur.firstChild.children,
    weight = m.pts.slice().sort((a, b) => a.t - b.t),
    flow = m.flowSegs
      .concat(m.dripFlowSegs)
      .flatMap((s) => s.pts)
      .sort((a, b) => a.t - b.t),
    attrs = (e, o) => Object.entries(o).forEach(([k, v]) => e.setAttribute(k, v));
  let cursorT = weight.at(-1).t;
  dotF.style.setProperty("--c", "var(--gFlow)");
  const seek = (t, held = true) => {
    cursorT = t = Math.round(Math.max(weight[0].t, Math.min(weight.at(-1).t, t)) * 10) / 10;
    const w = curveAt(weight, t),
      f = t >= flow[0]?.t && t <= flow.at(-1).t ? curveAt(flow, t) : null,
      x = (X(t) / W) * 100 + "%",
      yW = Y(w, m.maxW) / H,
      yF = f == null ? 1 : Y(f, step * n) / H,
      color =
        m.segs.concat(m.dripSegs).findLast((s) => s.pts[1] && s.pts[0].t <= t)?.color ??
        "var(--ok)",
      text = (a, b) =>
        R.sub(__WEBUI_TEXT__("runtime.shot_flow_pill"), [
          t.toFixed(1),
          a,
          w.toFixed(1),
          b,
          f?.toFixed(1) ?? "\u2014",
        ]),
      dot = (c) => `<tspan fill="${c}">\u25CF</tspan>`;
    attrs(guide, { x1: x, x2: x });
    attrs(dotW, { cx: x, cy: yW * 100 + "%" });
    attrs(dotF, { cx: x, cy: yF * 100 + "%", visibility: f == null ? "hidden" : "visible" });
    attrs(c, { "aria-valuenow": t.toFixed(1), "aria-valuetext": text("\u00B7", "\u00B7") });
    cur.style.setProperty("--c", color);
    if (!held) return;
    const box = cur.getBoundingClientRect();
    R.placePill(
      pill,
      text(dot(color), dot("var(--gFlow)")),
      (X(t) / W) * box.width,
      Math.min(yW, yF) * box.height,
    );
  };
  bindShotScrub(
    c,
    (x) =>
      ((((x - cur.getBoundingClientRect().left) / cur.clientWidth) * W - p) / (W - 2 * p)) *
      m.timeMax,
    seek,
    () => cursorT,
  );
  attrs(c, {
    "aria-valuemin": weight[0].t.toFixed(1),
    "aria-valuemax": weight.at(-1).t.toFixed(1),
  });
  seek(cursorT, false);
  return m;
}
let statsPresetFilter = "all";
function renderStatsDurChart() {
  const root = $("statsDurChart");
  if (!root) return;
  if (!root.dataset.ready) {
    root.className = "ruleChart";
    root.innerHTML =
      '<div id="statsPresetFilter" class="shotSort" role="group" aria-label="' +
      R.escHtml(__WEBUI_TEXT__("runtime.stats_preset_filter")) +
      '"><span class="sortLbl">' +
      __WEBUI_TEXT__("runtime.preset") +
      '</span><button type="button" data-v="all">' +
      __WEBUI_TEXT__("runtime.all_presets") +
      '</button><button type="button" data-v="active">' +
      __WEBUI_TEXT__("runtime.this_preset") +
      '</button></div><div id="statsDurChartPlot"></div>';
    root.dataset.ready = "1";
    for (const b of $("statsPresetFilter").querySelectorAll("button"))
      b.onclick = () => {
        statsPresetFilter = b.dataset.v;
        renderStatsDurChart();
      };
  }
  const host = $("statsDurChartPlot");
  if (!host) return;
  const st = R.uiState.shotStats || {},
    presets = R.homeStatusCache()?.presets || R.presetState,
    only = statsPresetFilter === "all" ? 0 : presets.activeId,
    // The firmware's charts over the panel's window, whatever the table's
    // order or scroll: density curves for all presets and the active one
    // (true = the same curve), and the timed arrivals oldest first.
    charts = st.dens || [],
    d = only ? (charts[1] === true ? charts[0] : charts[1]) : charts[0],
    names = new Map(st.facets?.presets || []),
    p = st.cons?.p,
    timed = (st.cons?.t || []).map((t, i) => {
      const presetId = Array.isArray(p) ? p[i] : p;
      return { arrivalS: t / 10, presetId, presetName: names.get(presetId) };
    }),
    shots = timed.filter((r) => !only || r.presetId === only);
  // No timed shots: no chart at all; none for this preset: only the filter.
  root.hidden = !timed.length;
  host.hidden = !d;
  for (const b of $("statsPresetFilter").querySelectorAll("button"))
    b.setAttribute("aria-pressed", String(b.dataset.v === statsPresetFilter));
  renderStatsConsistency(shots);
  if (!d) {
    host.innerHTML = "";
    return;
  }
  // Heights (0–100 of the peak) are sampled at 121 points from lo to hi;
  // the zone edges fall between samples.
  const { lo, hi, a, b, shared, h } = d,
    x = (t) => (((t - lo) / (hi - lo)) * 100).toFixed(2),
    dens = (t) => {
      const k = ((t - lo) / (hi - lo)) * 120,
        i = Math.max(0, Math.min(119, Math.floor(k)));
      return h[i] + (h[i + 1] - h[i]) * (k - i);
    },
    xs = Array.from({ length: 121 }, (_, i) => lo + ((hi - lo) * i) / 120),
    peak = 100,
    n = { fast: d.n[0], on_recipe: d.n[1], slow: d.n[2] },
    total = d.n[0] + d.n[1] + d.n[2],
    pct = {},
    L = R.axisLabel;
  // Rounding the running total keeps the shares adding up to 100%.
  let sum = 0,
    done = 0;
  for (const k in n) done += pct[k] = Math.round((100 * (sum += n[k])) / total) - done;
  const zones = [
    ["fast", lo, a, "var(--gFast)", __WEBUI_TEXT__("runtime.fast")],
    ["on_recipe", a, b, "var(--ok)", __WEBUI_TEXT__("runtime.verdict_on_recipe")],
    ["slow", b, hi, "var(--gSlow)", __WEBUI_TEXT__("runtime.slow")],
  ];
  let svg = "",
    tops = "";
  for (const [k, t0, t1, c, name] of zones) {
    const line = [t0, ...xs.filter((t) => t > t0 && t < t1), t1]
      .map((t) => x(t) + " " + (50 - (46 * dens(t)) / peak).toFixed(2))
      .join(" L");
    svg +=
      R.fadeFill("sd" + k, c, 0.45, 0.02) +
      '<path d="M' +
      x(t0) +
      " 50 L" +
      line +
      " L" +
      x(t1) +
      ' 50Z" fill="url(#sd' +
      k +
      ')"/><path d="M' +
      line +
      '" fill="none" stroke="' +
      c +
      '" stroke-width="1.35" vector-effect="non-scaling-stroke"/>';
    tops += "<span><b>" + pct[k] + "%</b>" + n[k] + " " + name + "</span>";
  }
  // Ticks step out from the window edges: seconds when shared, offsets otherwise.
  const ticks = [
    [a, shared ? L(a, "s") : __WEBUI_TEXT__("runtime.window_start"), 80],
    [b, shared ? L(b, "s") : __WEBUI_TEXT__("runtime.window_end"), 80],
  ];
  for (let d = 5; d < hi - lo; d += 5)
    ticks.push(
      [a - d, shared ? L(a - d, "s") : "−" + L(d, "s"), 20],
      [b + d, shared ? L(b + d, "s") : "+" + L(d, "s"), 20],
    );
  host.style.setProperty("--zf", x(a) + "%");
  host.style.setProperty("--zo", x(b) - x(a) + "%");
  host.innerHTML =
    '<div class="statsDensZones">' +
    tops +
    '</div><svg class="statsDensPlot" viewBox="0 0 100 50" preserveAspectRatio="none" aria-hidden="true">' +
    svg +
    '</svg><div class="ruleChartTicks"></div>';
  fillChartTicks(
    host.lastChild,
    ticks.filter(([t]) => t > lo && t < hi).map(([t, s, p]) => [t - lo, s, p]),
    hi - lo,
    true,
  );
}
// Consistency of the window's timed shots, oldest first: each shot's time to
// target as a share of its own preset's average, so recipes of any length
// compare fairly.
// The level is the median change between neighbouring shots, so one odd shot
// cannot decide it; the band uses the same 5 %.
const CONSISTENT = 0.05,
  VARIED = 0.1;
function consistencyModel(rows) {
  const n = rows.length,
    avg = {};
  for (const r of rows) (avg[r.presetId] ??= []).push(r.arrivalS);
  for (const k in avg) avg[k] = avg[k].reduce((a, v) => a + v, 0) / avg[k].length;
  const rel = rows.map((r) => r.arrivalS / avg[r.presetId] - 1),
    j = rel
      .slice(1)
      .map((v, i) => Math.abs(v - rel[i]))
      .sort((a, b) => a - b),
    typical = n < 5 ? null : (j[(j.length - 1) >> 1] + j[j.length >> 1]) / 2,
    // One preset reads in seconds (sec is its average), several in percent.
    sec = Object.keys(avg).length === 1 ? avg[rows[0].presetId] : 0,
    // Never narrower than twice the band, never so wide one outlier flattens
    // the rest; then up to whole (or half) seconds or 5 % steps for the labels.
    raw = Math.min(
      6 * CONSISTENT,
      Math.max(2 * CONSISTENT, Math.max(0, ...rel.map(Math.abs)) * 1.15),
    ),
    step = sec ? (raw * sec < 2 ? 0.5 : 1) / sec : 0.05;
  return {
    rows,
    rel,
    sec,
    typical,
    M: Math.ceil(raw / step - 1e-9) * step,
    level:
      typical == null
        ? "few"
        : typical <= CONSISTENT
          ? "good"
          : typical <= VARIED
            ? "fair"
            : "poor",
    spread: n ? Math.max(...rel) - Math.min(...rel) : 0,
  };
}
function renderStatsConsistency(shots) {
  const root = $("statsConsistency");
  if (!root) return;
  const m = consistencyModel(shots),
    { rows, rel, sec, M } = m,
    n = rows.length,
    // A slot per shot shown (at least ten), latest on the right.
    W = Math.max(10, n),
    gap = 94 / (W - 1),
    half = gap * 0.27;
  root.hidden = !n;
  if (!n) return;
  const L = (v) =>
      sec
        ? R.axisLabel(v * sec, __WEBUI_TEXT__("runtime.s"))
        : +(v * 100).toFixed(1) + __WEBUI_TEXT__("runtime.symbol_4"),
    signed = (v, k = Math.round(v * (sec || 100) * 10)) =>
      (k > 0 ? "+" : k < 0 ? "−" : "") + L(Math.abs(v)),
    X = (i) => 3 + (W - n + i) * gap,
    Y = (v) => 25 - (23 * Math.max(-M, Math.min(M, v))) / M,
    out = (i) => Math.abs(rel[i]) > CONSISTENT,
    bar = (v, i) => {
      const x = (X(i) - half).toFixed(2),
        h = Math.abs(Y(v) - 25),
        y = h < 0.6 ? 24.7 : Math.min(Y(v), 25);
      return (
        '<rect class="consBar' +
        (out(i) ? " out" : "") +
        '" x="' +
        x +
        '" width="' +
        (2 * half).toFixed(2) +
        '" y="' +
        y.toFixed(2) +
        '" height="' +
        Math.max(h, 0.6).toFixed(2) +
        '"/>' +
        (Math.abs(v) > M
          ? '<path class="consOver" d="M' +
            x +
            " " +
            (v > 0 ? 1 : 49) +
            "h" +
            (2 * half).toFixed(2) +
            '"/>'
          : "")
      );
    };
  root.dataset.level = m.level;
  root.innerHTML =
    '<div class="statsDensZones"><span><b>' +
    (m.typical == null ? "—" : "±" + L(m.typical)) +
    "</b>" +
    {
      good: __WEBUI_TEXT__("runtime.consistency_good"),
      fair: __WEBUI_TEXT__("runtime.consistency_fair"),
      poor: __WEBUI_TEXT__("runtime.consistency_poor"),
      few: __WEBUI_TEXT__("runtime.consistency_few"),
    }[m.level] +
    "</span><span><b>" +
    L(m.spread) +
    "</b>" +
    __WEBUI_TEXT__("runtime.consistency_spread") +
    "</span><span><b>" +
    n +
    "</b>" +
    __WEBUI_TEXT__("runtime.consistency_shots") +
    '</span></div><div class="consPlot"><svg class="statsDensPlot" viewBox="0 0 100 50" preserveAspectRatio="none" aria-hidden="true">' +
    '<rect class="consBand" x="0" width="100" y="' +
    Y(CONSISTENT) +
    '" height="' +
    (Y(-CONSISTENT) - Y(CONSISTENT)) +
    '"/>' +
    rel.map(bar).join("") +
    '<path class="consAvg" d="M0 25H100"/></svg><span class="consEdge">+' +
    L(M) +
    '</span><span class="consEdge">' +
    (sec ? L(1) : __WEBUI_TEXT__("runtime.consistency_avg")) +
    '</span><span class="consEdge">−' +
    L(M) +
    '</span><svg class="consCursor" aria-hidden="true"><g class="shotCursor"><line class="rvGuide" y1="0" y2="100%"/><circle class="rvHandle" r="5"/>' +
    R.PILL +
    '</g></svg></div><div class="ruleChartTicks"><span class="ruleTick">' +
    __WEBUI_TEXT__("runtime.consistency_oldest") +
    '</span><span class="ruleTick">' +
    __WEBUI_TEXT__("runtime.consistency_latest") +
    "</span></div>";
  // The firmware CSP drops markup style attributes; CSSOM writes still apply.
  const plot = root.querySelector(".consPlot"),
    cur = plot.lastChild,
    [guide, dot, pill] = cur.firstChild.children,
    [t0, t1] = root.querySelectorAll(".ruleTick");
  [Y(M), 25, Y(-M)].forEach(
    (y, k) => (plot.querySelectorAll(".consEdge")[k].style.top = y * 2 + "%"),
  );
  t0.style.left = X(0) - half + "%";
  t0.hidden = n < 4;
  t1.style.left = "100%";
  let at = n - 1;
  const seek = (v, held = true) => {
    at = Math.max(0, Math.min(n - 1, v > at ? Math.ceil(v) : Math.floor(v)));
    const x = X(at) + "%",
      r = rows[at],
      text = R.sub(__WEBUI_TEXT__("runtime.consistency_pill"), [
        at + 1,
        sec ? "" : " · " + shotPresetName(r),
        R.axisLabel(r.arrivalS, __WEBUI_TEXT__("runtime.s")),
        signed(rel[at]),
      ]);
    guide.setAttribute("x1", x);
    guide.setAttribute("x2", x);
    dot.setAttribute("cx", x);
    dot.setAttribute("cy", Y(rel[at]) * 2 + "%");
    cur.style.setProperty("--c", out(at) ? "var(--zl)" : "var(--zs)");
    plot.setAttribute("aria-valuenow", at + 1);
    plot.setAttribute("aria-valuetext", text);
    if (held)
      R.placePill(
        pill,
        R.escHtml(text),
        (X(at) / 100) * cur.clientWidth,
        (Y(rel[at]) / 50) * cur.clientHeight,
      );
  };
  bindShotScrub(
    plot,
    (cx) => {
      const b = cur.getBoundingClientRect();
      return Math.round((((cx - b.left) / b.width) * 100 - 3) / gap) - (W - n);
    },
    seek,
    () => at,
  );
  plot.setAttribute("aria-label", __WEBUI_TEXT__("runtime.consistency_cursor"));
  plot.setAttribute("aria-valuemin", 1);
  plot.setAttribute("aria-valuemax", n);
  seek(at, false);
}
function renderShotStats() {
  const z = "—",
    s = R.uiState.shotStats || {},
    q = (i, v, c) => {
      const e = $("statsAvg" + i);
      if (e) {
        e.textContent = v;
        if (c != null) {
          e.classList.toggle("shotErrHi", c > 0);
          e.classList.toggle("shotErrLo", c < 0);
        }
      }
    },
    f = (v, p, u) => (v == null ? z : v.toFixed(p) + u);
  q("Dur", f(s.avgDurationS, 1, __WEBUI_TEXT__("runtime.s")));
  q("Weight", f(s.avgYieldG, 1, __WEBUI_TEXT__("runtime.g")));
  q("Daily", s.shotsPerDay == null ? z : s.shotsPerDay.toFixed(1));
  q("Err", f(s.avgErrorPct, 1, __WEBUI_TEXT__("runtime.symbol_4")), s.avgErrorPct);
  q("Flow", f(s.avgFlowGps, 2, __WEBUI_TEXT__("runtime.g_s")));
  const hint = $("statsWindowHint"),
    c = s.shotCount || 0;
  if (hint) {
    hint.hidden = !c;
    hint.textContent =
      c === 1
        ? __WEBUI_TEXT__("runtime.stats_window_one")
        : R.sub(__WEBUI_TEXT__("runtime.stats_window_hint"), [c]);
  }
  for (const b of $("statsWindow")?.querySelectorAll("button") || [])
    b.setAttribute("aria-pressed", String(+b.dataset.v === R.uiState.statsWindow));
  renderStatsDurChart();
}
// Results always; scales and presets only when the log holds more than one.
function shotFilterGroups() {
  const f = R.uiState.shotStats?.facets || {},
    presets = R.homeStatusCache()?.presets || R.presetState,
    dot = (k) =>
      '<svg class="histFilterDot" data-kind="' +
      k +
      '" viewBox="0 0 10 10"><circle cx="5" cy="5" r="3.5"/></svg>';
  return [
    {
      key: "result",
      label: __WEBUI_TEXT__("runtime.filter_result"),
      options: ["fast", "slow", "on_recipe"].map((k) => [k, R.verdictLabel(k), dot(k)]),
    },
    f.scales?.length > 1 && {
      key: "scale",
      label: __WEBUI_TEXT__("runtime.scale"),
      options: f.scales,
    },
    f.presets?.length > 1 && {
      key: "preset",
      label: __WEBUI_TEXT__("runtime.preset"),
      options: f.presets.map(([id, name]) => [
        id,
        presets.items?.find((p) => p.id === id)?.name || name,
      ]),
    },
  ].filter(Boolean);
}
function renderShots() {
  renderShotStats();
  if (!R.shotsLoaded) return;
  const body = $("shotRows");
  if (!body) return;
  body.replaceChildren();
  const td = (p, l, v, c) => {
    const e = document.createElement("td");
    if (l) e.dataset.label = l;
    if (c) e.className = c;
    e.textContent = v;
    if (c === "shotDur" || c === "shotActual")
      e.innerHTML =
        '<span class="shotVal">' +
        e.innerHTML +
        '</span><svg class="shotMetricIcon" width="1.85rem" height="1.85rem" fill="currentColor" aria-hidden="true"><use href="#shotMetric' +
        (c === "shotDur" ? "Time" : "Weight") +
        '"/></svg>';
    p.appendChild(e);
    return e;
  };
  const rows = R.uiState.shotHistory.shots;
  if (rows.length) {
    const labels = [
      __WEBUI_TEXT__("runtime.time"),
      __WEBUI_TEXT__("runtime.dur"),
      __WEBUI_TEXT__("runtime.goal"),
      __WEBUI_TEXT__("runtime.yield"),
      __WEBUI_TEXT__("runtime.err"),
      __WEBUI_TEXT__("runtime.drip"),
      __WEBUI_TEXT__("runtime.avg_flow"),
      __WEBUI_TEXT__("runtime.max_flow"),
      __WEBUI_TEXT__("runtime.target_temp"),
      __WEBUI_TEXT__("runtime.dose"),
      __WEBUI_TEXT__("runtime.ratio"),
      __WEBUI_TEXT__("runtime.1st_drop"),
      __WEBUI_TEXT__("runtime.tare_time"),
      __WEBUI_TEXT__("runtime.ended"),
      __WEBUI_TEXT__("runtime.shot"),
      __WEBUI_TEXT__("runtime.preset"),
      __WEBUI_TEXT__("runtime.scale"),
      __WEBUI_TEXT__("runtime.cut_at"),
      __WEBUI_TEXT__("runtime.drip_delay"),
    ];
    const G = __WEBUI_TEXT__("runtime.g"),
      S = __WEBUI_TEXT__("runtime.s");
    for (const r of rows) {
      const row = document.createElement("tr");
      row.dataset.id = r.id;
      // A scale-loss shot has no weighed yield: only its estimate, no error.
      const estimated = r.actualG == null && r.estimatedG != null,
        actual = estimated ? null : shotDisplayActualG(r.actualG, r.wCg),
        planned = r.cutLineG > 0 ? r.cutLineG : r.goalG;
      const errN = actual === null || !planned ? null : ((actual - planned) / planned) * 100;
      const err = errN === null ? "—" : errN.toFixed(1) + "%";
      // Drip: the settled yield minus the reading at the stop.
      const flow = shotDisplayFlowGS(r),
        m = R.buildShotSparkModel(r),
        maxFlow = m ? m.maxFlow : null,
        cut = shotCut(r, m, actual),
        drip = cut.drip ?? null;
      // Without a machine-reported target the temp cell is skipped outright,
      // so Dose and Ratio slide into its slot.
      const tempC = r.targetTempDeciC > 0 ? r.targetTempDeciC / 10 : null;
      const vals = [
        formatShotTime(r),
        r.durationS.toFixed(1) + S,
        r.goalG.toFixed(1) + G,
        estimated
          ? R.sub(__WEBUI_TEXT__("runtime.yield_estimated"), [Math.round(r.estimatedG)])
          : actual === null
            ? "—"
            : actual.toFixed(1) + G,
        err,
        drip === null ? "—" : (drip < 0 ? "" : "+") + drip.toFixed(1) + G,
        flow === null ? "—" : flow.toFixed(2) + __WEBUI_TEXT__("runtime.g_s"),
        maxFlow === null
          ? "—"
          : maxFlow.toFixed(2) +
            __WEBUI_TEXT__("runtime.g_s") +
            (r.wTruncated ? __WEBUI_TEXT__("runtime.recorded_suffix") : ""),
        tempC === null ? null : tempC.toFixed(1) + "°C",
        r.doseG > 0 ? r.doseG.toFixed(1) + G : "—",
        r.ratio > 0 ? "1:" + r.ratio.toFixed(1) : "—",
        r.firstDropS === null ? "—" : r.firstDropS.toFixed(1) + S,
        r.tareS == null ? __WEBUI_TEXT__("runtime.none") : r.tareS.toFixed(1) + S,
        R.formatShotEnded(r.stopDetail, r.verdict),
        r.shotType,
        shotPresetName(r),
        r.scaleName || __WEBUI_TEXT__("runtime.none"),
        cut.g == null ? "—" : cut.g.toFixed(1) + G + " · " + cut.s.toFixed(1) + S,
        r.dripDelayS == null ? "—" : r.dripDelayS.toFixed(1) + S,
      ];
      const cls = [
        "",
        "shotDur",
        "",
        "shotActual",
        errN > 0 ? "shotErrHi" : errN < 0 ? "shotErrLo" : "",
      ];
      if (tempC === null) row.classList.add("noTemp");
      vals.forEach((v, i) => {
        if (v !== null) td(row, labels[i], v, cls[i]);
      });
      R.wrapTimeEl(row.cells[0], r);
      // The verdict sits beside the time so each card says at a glance how the shot landed.
      if (r.arrivalS > 0 && ["fast", "on_recipe", "slow"].includes(r.verdict))
        row.cells[0].insertAdjacentHTML(
          "beforeend",
          '<span class="heroPill" data-kind="' +
            r.verdict +
            '">' +
            R.verdictLabel(r.verdict) +
            "</span>",
        );
      const rate = document.createElement("td");
      rate.className = "shotRateCell";
      rate.dataset.label = __WEBUI_TEXT__("runtime.rate");
      const rateHost = document.createElement("div");
      rate.appendChild(rateHost);
      fillStarRate(rateHost, r.rating || 0, !R.controlsMutable, (n, star) =>
        rateHistoryShot(r.id, n, star),
      );
      row.appendChild(rate);
      const spark = document.createElement("td");
      spark.className = "shotSparkCell";
      renderShotSpark(spark, r, m);
      if (spark.hidden) row.classList.add("noSpark");
      else row.appendChild(spark);
      const del = document.createElement("td");
      del.className = "shotDel";
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "btnGlyph";
      btn.title = __WEBUI_TEXT__("stats.shot_actions");
      btn.setAttribute("aria-label", __WEBUI_TEXT__("stats.shot_actions"));
      btn.setAttribute("aria-haspopup", "true");
      btn.setAttribute("aria-expanded", "false");
      btn.innerHTML =
        '<span class="g" aria-hidden="true">' + __WEBUI_TEXT__("stats.more") + "</span>";
      btn.onclick = () => shotMenu.open(btn, r);
      del.appendChild(btn);
      row.appendChild(del);
      body.appendChild(row);
    }
  }
  R.setEmptyState(
    "shotTableState",
    rows.length
      ? null
      : Object.keys(R.uiState.shotFilter).length
        ? __WEBUI_TEXT__("runtime.no_shots_match_filter")
        : __WEBUI_TEXT__("runtime.no_recorded_shots_yet"),
  );
  updateShotLogSentinel();
}
function updateShotLogSentinel() {
  const el = $("shotLogSentinel");
  if (el) {
    el.hidden = !R.uiState.shotHistory.hasMore;
    el.textContent = R.uiState.shotHistory.hasMore ? __WEBUI_TEXT__("runtime.more") : "";
  }
}
async function deleteOneShot(id) {
  if (!id || !confirm(__WEBUI_TEXT__("runtime.delete_this_shot_record"))) return;
  return R.withCommandGate(async () => {
    try {
      await R.api("/api/v1/stats/delete", { method: "POST", body: R.body({ id }) });
      R.uiState.shotHistory.shots = R.uiState.shotHistory.shots.filter((s) => s.id !== id);
      if (typeof R.uiState.shotHistory.total === "number" && R.uiState.shotHistory.total > 0)
        R.uiState.shotHistory.total--;
      R.uiState.shotHistory.hasMore =
        R.uiState.shotHistory.shots.length < R.uiState.shotHistory.total;
      renderShots();
      R.message(__WEBUI_TEXT__("runtime.shot_deleted"), "ok");
    } catch (e) {
      R.message(R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_delete_shot"), e), "error");
    }
  });
}
async function postShotRating(p, undo) {
  return R.withCommandGate(async () => {
    try {
      await R.api("/api/v1/stats/rate", { method: "POST", body: R.body(p) });
      R.noteReachOk();
      return true;
    } catch (e) {
      if (undo) undo();
      R.message(
        R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_save_shot_rating"), e),
        "error",
      );
      return false;
    }
  });
}
function rateHistoryShot(id, n, star) {
  const s = R.uiState.shotHistory.shots.find((x) => x && x.id === id),
    p = s && s.rating;
  if (s) s.rating = n;
  renderShots();
  // The rebuilt row replaces the clicked star: keyboard focus moves to its twin.
  $("shotRows")?.querySelector(`tr[data-id="${id}"] .starRate`)?.children[star - 1]?.focus();
  return postShotRating({ id, rating: n }, () => {
    if (s) s.rating = p;
    renderShots();
  }).then((ok) => ok && R.uiState.shotSort === "rating" && R.refreshShots());
}
function syncShotSortButtons() {
  const d = $("sortDateButton"),
    r = $("sortRatingButton"),
    desc = R.uiState.shotSortDir === "desc",
    arrow = desc ? __WEBUI_TEXT__("runtime.down") : __WEBUI_TEXT__("runtime.symbol_5"),
    // Each label names the order pressing that button will produce: the active
    // criterion flips the direction, the other one switches keeping it.
    sync = (b, on, name, after) => {
      b.setAttribute("aria-pressed", on);
      b.textContent = on ? name + " " + arrow : name;
      if (!on) {
        b.removeAttribute("aria-label");
        b.removeAttribute("title");
      } else {
        b.setAttribute("aria-label", after);
        b.title = after;
      }
    };
  if (d)
    sync(
      d,
      R.uiState.shotSort === "date",
      __WEBUI_TEXT__("stats.date"),
      desc ? __WEBUI_TEXT__("runtime.oldest_first") : __WEBUI_TEXT__("runtime.newest_first"),
    );
  if (r)
    sync(
      r,
      R.uiState.shotSort === "rating",
      __WEBUI_TEXT__("stats.rating"),
      desc ? __WEBUI_TEXT__("runtime.lowest_rating") : __WEBUI_TEXT__("runtime.highest_rating"),
    );
}
function setShotSort(field) {
  if (field !== "date" && field !== "rating") return;
  if (R.uiState.shotSort === field)
    R.uiState.shotSortDir = R.uiState.shotSortDir === "desc" ? "asc" : "desc";
  else R.uiState.shotSort = field;
  syncShotSortButtons();
  R.statsSendSubscribe();
}
const loadMoreShots = () => {
  if (
    R.uiState.shotHistory.hasMore &&
    R.shotStatsViewActive() &&
    !R.uiState.statsFetchMark &&
    !statsExportInFlight
  ) {
    R.uiState.statsFetchMark = {
      request: ++R.uiState.statsNextRequest,
      offset: R.uiState.shotHistory.shots.length,
    };
    R.sendUiOperation({
      op: "stats",
      on: true,
      fetch: true,
      request: R.uiState.statsFetchMark.request,
      offset: R.uiState.statsFetchMark.offset,
      limit: R.SHOTS_PAGE_SIZE,
      sort: R.uiState.shotSort,
      dir: R.uiState.shotSortDir,
    });
  }
};
async function exportShotsCsv() {
  if (statsExportInFlight) return;
  try {
    const list = await statsFrameWindow(0, R.SHOTS_EXPORT_LIMIT, "date", "desc", 90e3);
    if (!list) throw new Error(__WEBUI_TEXT__("runtime.invalid_status"));
    const maxW = list.reduce((m, r) => Math.max(m, r.wCg?.length || 0), 0);
    const rows = [
      [
        ...(
          "id boot_id local_time has_wall_time ended_at_unix tz_off duration_s goal_g " +
          "yield_g error_g error_pct offset_g avg_flow_g_s first_drop_s tare_s fast_side " +
          "cut_raised slow_side cut_lowered stop verdict channeling scale_loss_source " +
          "cut_line_g arrival_s fast_before_s slow_after_s fast_margin_g slow_margin_g " +
          "delay_s wall_s cut_ramp_s shot_type cut_type yield_source rating ended_at_ms " +
          "bbw_algorithm bbw_algorithm_version bbw_alpha bbw_learning_applied preset_id " +
          "scale_name max_flow_g_s target_temp_c dose_g ratio curve_truncated curve_break_before curve_held cut_s cut_weight_g " +
          "drip_g drip_delay_s"
        ).split(" "),
        ...Array.from({ length: maxW }, (_, i) => [
          "sample_" + (i + 1) + "_time_s",
          "sample_" + (i + 1) + "_weight_g",
          "sample_" + (i + 1) + "_flow_g_s",
        ]).flat(),
      ],
    ];
    for (const r of list) {
      // One curve model per shot feeds the flow column, the cut and the maximum.
      const w = Array.isArray(r.wCg) ? r.wCg : [],
        m = R.buildShotSparkModel(r),
        f = shotFlowCurveGS(r, m),
        actual = shotDisplayActualG(r.actualG, r.wCg),
        cut = shotCut(r, m, actual);
      const errorG = actual === null || r.goalG == null ? null : actual - r.goalG;
      const errorPct = errorG === null || !r.goalG ? null : (errorG / r.goalG) * 100;
      rows.push([
        r.id,
        r.bootId,
        formatShotTimeCsv(r),
        r.hasWallTime ? "1" : "0",
        r.endedAtUnixSec || "",
        r.hasWallTime ? (r.timezoneOffsetMinutesAtCommit ?? "") : "",
        r.durationS,
        r.goalG.toFixed(1),
        actual ?? "",
        errorG ?? "",
        errorPct ?? "",
        r.offsetG,
        shotDisplayFlowGS(r) ?? "",
        r.firstDropS ?? "",
        r.tareS ?? "",
        ...(
          "extractionGuardEnabled extractionExtended slowExtractionGuardEnabled slowExtractionExtended " +
          "stopDetail verdict channeling scaleLossSource cutLineG arrivalS fastBeforeS slowAfterS " +
          "fastMarginG slowMarginG delayS wallS cutRampS"
        )
          .split(" ")
          .map((k) => (typeof r[k] == "boolean" ? +r[k] : (r[k] ?? ""))),
        r.shotType,
        r.cutType,
        r.actualWeightSource ?? "",
        r.rating ?? 0,
        r.endedAtMs,
        r.bbwAlgorithm ?? "",
        r.bbwAlgorithmVersion ?? "",
        r.bbwAlpha == null ? "" : Number(r.bbwAlpha).toFixed(2),
        r.bbwLearningApplied == null ? "" : r.bbwLearningApplied ? "1" : "0",
        r.presetId || "",
        r.scaleName || "",
        shotMaxFlowGS(r, m) ?? "",
        r.targetTempDeciC > 0 ? (r.targetTempDeciC / 10).toFixed(1) : "-1",
        r.doseG > 0 ? r.doseG.toFixed(1) : "",
        r.ratio > 0 ? r.ratio.toFixed(2) : "",
        r.wTruncated ? "1" : "0",
        (r.wBreakBefore || []).join(";"),
        (r.wHeld || []).join(";"),
        cut.s ?? "",
        cut.g ?? "",
        cut.drip?.toFixed(2) ?? "",
        r.dripDelayS ?? "",
        ...Array.from({ length: maxW }, (_, i) =>
          i < w.length
            ? [
                r.wAtMs?.[i] == null ? "" : r.wAtMs[i] / 1000,
                w[i] / 100,
                f[i] == null ? "" : f[i].toFixed(2),
              ]
            : ["", "", ""],
        ).flat(),
      ]);
    }
    R.downloadCsv("shot-history.csv", rows);
  } catch (e) {
    R.message(
      R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_export_shot_history"), e),
      "error",
    );
  }
}
async function exportShotsJson() {
  if (statsExportInFlight) return;
  try {
    const list = await statsFrameWindow(0, R.SHOTS_EXPORT_LIMIT, "date", "desc", 90e3);
    if (!list) throw new Error(__WEBUI_TEXT__("runtime.invalid_status"));
    // Local time as YYYYMMDD-HHMM ("sv" formats dates ISO-like).
    const stamp = new Date().toLocaleString("sv").slice(0, 16).replace(/\D/g, "");
    downloadJson(
      `openbrewbyweight-shots-${stamp.slice(0, 8)}-${stamp.slice(8)}.json`,
      shotsJson(list),
    );
  } catch (e) {
    R.message(
      R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_export_shot_history"), e),
      "error",
    );
  }
}
function downloadJson(name, value) {
  R.saveFile(new Blob([JSON.stringify(value)], { type: "application/json" }), name);
}
// ISO 8601 in the shot's own offset (minutes east of UTC).
function isoAt(unix, offset) {
  const a = Math.abs(offset),
    p = (n) => String(n).padStart(2, "0");
  return (
    new Date((unix + offset * 60) * 1e3).toISOString().slice(0, 19) +
    (offset < 0 ? "-" : "+") +
    p((a / 60) | 0) +
    ":" +
    p(a % 60)
  );
}
function shotWallTimes(r) {
  if (!(r.hasWallTime && r.endedAtUnixSec > 0)) return null;
  const offset =
    typeof r.timezoneOffsetMinutesAtCommit === "number"
      ? r.timezoneOffsetMinutesAtCommit
      : Math.round((r.endedAtLocalSec - r.endedAtUnixSec) / 60);
  return [isoAt(r.endedAtUnixSec - r.durationS, offset), isoAt(r.endedAtUnixSec, offset), offset];
}
// Shot export v1 (docs/schemas/openbrewbyweight-shot-export-v1.schema.json):
// what the firmware stored, then under `derived` what this page computes.
function shotsJson(rows) {
  return {
    $schema:
      "https://raw.githubusercontent.com/Cheerpipe/Open-Brew-by-Weight/main/docs/schemas/openbrewbyweight-shot-export-v1.schema.json",
    format: "openbrewbyweight.shots",
    version: 1,
    exportedAt: new Date().toISOString(),
    source: {
      app: "openbrewbyweight",
      firmwareVersion: R.firmwareVersion || null,
      host: location.hostname || null,
    },
    shots: rows.map(shotJson),
  };
}
function shotJson(r) {
  const w = Array.isArray(r.wCg) ? r.wCg : [],
    m = R.buildShotSparkModel(r),
    flow = shotFlowCurveGS(r, m),
    estimated = r.actualG == null && r.estimatedG != null,
    actual = estimated ? null : shotDisplayActualG(r.actualG, r.wCg),
    cut = shotCut(r, m, actual),
    planned = r.cutLineG > 0 ? r.cutLineG : r.goalG,
    time = shotWallTimes(r),
    n = (v) => (typeof v === "number" && Number.isFinite(v) ? Math.round(v * 100) / 100 : null),
    pos = (v) => (v > 0 ? v : null),
    event = (s, cg) => (s == null || cg == null ? null : { atS: s, weightG: cg / 100 }),
    guard = (enabled, marginG, extended) => ({ enabled: !!enabled, marginG, extended: !!extended });
  return {
    id: r.id,
    bootId: r.bootId,
    time: {
      startedAt: time?.[0] ?? null,
      endedAt: time?.[1] ?? null,
      utcOffsetMinutes: time?.[2] ?? null,
      endedAtUptimeMs: r.endedAtMs,
    },
    type: r.shotType,
    rating: r.rating || null,
    preset: { id: r.presetId, name: r.presetName || null },
    scale: r.scaleName ? { name: r.scaleName } : null,
    recipe: {
      targetYieldG: r.goalG,
      doseG: pos(r.doseG),
      targetTemperatureC: r.targetTempDeciC > 0 ? r.targetTempDeciC / 10 : null,
    },
    outcome: {
      durationS: r.durationS,
      yieldG: r.actualG ?? null,
      yieldSource: r.actualWeightSource ?? null,
      estimatedYieldG: r.estimatedG ?? null,
      ratio: pos(r.ratio),
      averageFlowGS: r.avgFlowGS ?? null,
      firstDropS: r.firstDropS ?? null,
      tareS: r.tareS ?? null,
      arrivalS: r.arrivalS ?? null,
      verdict: r.verdict ?? null,
      channeling: !!r.channeling,
    },
    stop: {
      cut: r.cutType,
      reason: r.stopDetail,
      cutLineG: r.cutLineG ?? null,
      scaleLossSource: r.scaleLossSource ?? null,
      blind: !!r.blind,
    },
    control: {
      brewByWeight: {
        algorithm: r.bbwAlgorithm === "unknown" ? null : r.bbwAlgorithm || null,
        algorithmVersion: r.bbwAlgorithmVersion ?? null,
        alpha: r.bbwAlpha ?? null,
        learningApplied: r.bbwLearningApplied ?? null,
        offsetG: r.offsetG,
      },
      window: { fastBeforeS: r.fastBeforeS, slowAfterS: r.slowAfterS },
      fastGuard: guard(r.extractionGuardEnabled, r.fastMarginG, r.extractionExtended),
      slowGuard: guard(r.slowExtractionGuardEnabled, r.slowMarginG, r.slowExtractionExtended),
      cutDelayS: r.delayS,
      cutRampS: r.cutRampS,
      maxTimeS: r.wallS,
      dripDelayS: r.dripDelayS ?? null,
    },
    curve: {
      truncated: !!r.wTruncated,
      timeS: w.map((_, i) => r.wAtMs[i] / 1000),
      weightG: w.map((cg) => cg / 100),
      heldIndices: r.wHeld || [],
      gapBeforeIndices: r.wBreakBefore || [],
      events: {
        firstDrop: event(r.dropS, r.dropCg),
        extended: event(r.extendedS, r.extCg),
        scaleLoss: event(r.atmS, r.atmCg),
        scaleLossClearedS: r.atmClearedS ?? null,
        end: event(r.endS, r.endCg),
      },
    },
    derived: {
      flowGS: w.map((_, i) => n(flow[i])),
      maxFlowGS: n(shotMaxFlowGS(r, m)),
      errorVsTargetG: actual === null || !r.goalG ? null : n(actual - r.goalG),
      errorVsTargetPct:
        actual === null || !r.goalG ? null : n(((actual - r.goalG) / r.goalG) * 100),
      errorVsPlannedStopPct:
        actual === null || !planned ? null : n(((actual - planned) / planned) * 100),
      cut: cut.s == null ? null : { atS: cut.s, weightG: cut.g },
      dripG: n(cut.drip),
    },
  };
}
// The curve's samples as the scale weighed coffee: held readings (a hand on
// the cup) left out, as [ms, cg] pairs.
function coffeeSamples(r) {
  const held = new Set(r.wHeld);
  return (r.wCg || []).flatMap((cg, i) => (held.has(i) ? [] : [[r.wAtMs[i], cg, i]]));
}
// One shot in the Gaggiuino format Visualizer.coffee imports. The controller
// weighs only, so pressure, pump flow and water temperature are left out.
function shotGaggiuino(r) {
  const flow = shotFlowCurveGS(r),
    tempC = r.targetTempDeciC > 0 ? r.targetTempDeciC / 10 : undefined,
    start = shotWallTimes(r) && Math.round(r.endedAtUnixSec - r.durationS);
  return {
    id: r.id,
    timestamp: start || 0,
    duration: Math.round(r.durationS * 10),
    datapoints: coffeeSamples(r).map(([ms, cg, i]) => ({
      timeInShot: ms,
      shotWeight: Math.round(cg / 10) / 10,
      weightFlow: Math.round((flow[i] ?? 0) * 10) / 10,
      targetTemperature: tempC,
    })),
    profile: {
      id: r.presetId,
      name: r.presetName || "",
      phases: [],
      globalStopConditions: { weight: r.goalG },
      waterTemperature: tempC,
      recipe: {},
    },
  };
}
// One shot in the Decent JSON shape Visualizer.coffee imports by default: the
// weighed curve, a stage change at the first drop and at the cut, the recipe,
// and as Markdown notes what Visualizer has no field for.
function shotVisualizer(r) {
  const m = R.buildShotSparkModel(r),
    flow = shotFlowCurveGS(r, m),
    s = coffeeSamples(r),
    estimated = r.actualG == null && r.estimatedG != null,
    actual = estimated ? null : shotDisplayActualG(r.actualG, r.wCg),
    cut = shotCut(r, m, actual),
    tempC = r.targetTempDeciC > 0 ? r.targetTempDeciC / 10 : undefined,
    g = (v, sign) => (sign && v >= 0 ? "+" : "") + v.toFixed(1) + " g",
    error = actual !== null && r.goalG ? actual - r.goalG : null,
    notes = [
      "Ended: " + R.formatShotEnded(r.stopDetail, r.verdict),
      `Shot: ${r.shotType} · ${r.cutType} cut`,
      r.channeling && "Channeling detected",
      error !== null &&
        `Yield vs target: ${g(error, 1)} (${error >= 0 ? "+" : ""}${((error / r.goalG) * 100).toFixed(1)} %)`,
      estimated && "Yield estimated: " + g(r.estimatedG),
      r.scaleLossSource && "Scale lost, stop by " + r.scaleLossSource,
      r.firstDropS != null && `First drop: ${r.firstDropS.toFixed(1)} s`,
      cut.s != null && `Cut at ${g(cut.g)} · ${cut.s.toFixed(1)} s`,
      cut.drip != null && "Drip: " + g(cut.drip, 1),
      r.scaleName && "Scale: " + r.scaleName,
      r.bbwAlgorithm &&
        r.bbwAlgorithm !== "unknown" &&
        `Brew by weight: ${r.bbwAlgorithm} v${r.bbwAlgorithmVersion}, offset ${g(r.offsetG)}`,
      (r.extractionExtended || r.slowExtractionExtended) &&
        `Extended by the ${r.slowExtractionExtended ? "slow" : "fast"}-shot guard`,
      r.wTruncated && "Curve truncated",
      ("Exported from Open Brew by Weight " + (R.firmwareVersion || "")).trim(),
    ];
  return {
    version: 2,
    timestamp:
      (shotWallTimes(r) && Math.round(r.endedAtUnixSec - r.durationS)) ||
      Math.round(Date.now() / 1e3),
    elapsed: s.map(([ms]) => ms / 1e3),
    totals: { weight: s.map(([, cg]) => cg / 100) },
    flow: { by_weight: s.map(([, , i]) => Math.round((flow[i] ?? 0) * 100) / 100) },
    temperature: tempC && { goal: s.map(() => tempC) },
    // Visualizer draws a stage line wherever this value changes.
    state_change: s.map(([ms]) =>
      ms < r.firstDropS * 1e3 ? 1 : r.endS != null && ms > r.endS * 1e3 ? 3 : 2,
    ),
    profile: { title: r.presetName || undefined },
    app: {
      app_name: "openbrewbyweight",
      app_version: R.firmwareVersion || undefined,
      data: {
        settings: {
          profile_title: r.presetName || undefined,
          drink_weight: actual ?? r.estimatedG ?? undefined,
          bean_weight: r.doseG > 0 ? r.doseG : undefined,
          final_desired_shot_weight: r.goalG,
          espresso_temperature: tempC,
          espresso_enjoyment: r.rating > 0 ? r.rating * 20 : undefined,
          espresso_notes: notes
            .filter(Boolean)
            .map((l) => "- " + l)
            .join("\n"),
        },
      },
    },
  };
}
// Beanconqueror's brew handoff: the envelope gzipped, then unpadded base64url
// split into 400-character shareBrew<n> parameters behind its total length.
async function shotBeanconquerorLink(r) {
  const s = coffeeSamples(r).map(([ms, cg]) => [ms, Math.round(cg / 10)]),
    delta = (k) => s.map((q, i) => q[k] - (i ? s[i - 1][k] : 0)),
    actual = r.actualG ?? r.estimatedG ?? 0,
    envelope = {
      v: 1,
      app: { name: "Open Brew by Weight", version: R.firmwareVersion || undefined },
      brew: {
        date: shotWallTimes(r)?.[0] ?? new Date().toISOString(),
        doseIn: r.doseG > 0 ? { value: r.doseG, unit: "g" } : undefined,
        waterIn: { value: 0, unit: "ml" },
        beverageOut: { value: actual, unit: "g" },
        brewTime: r.durationS,
        temperature: r.targetTempDeciC > 0 ? r.targetTempDeciC / 10 : undefined,
        ratio: r.ratio > 0 ? r.ratio : undefined,
        preparationMethod: "Espresso",
        preparationType: "PORTAFILTER",
        firstDripTime: r.firstDropS ?? undefined,
        rating: r.rating || undefined,
        note: [r.presetName, r.scaleName].filter(Boolean).join(" · "),
      },
      flow: s.length
        ? {
            fidelity: "full",
            t: delta(0),
            weight: delta(1),
            waterDispensed: s.map(() => 0),
          }
        : undefined,
      imported: {
        source: "openbrewbyweight",
        sourceName: "Open Brew by Weight",
        device: location.hostname || undefined,
        schema: 1,
      },
    };
  const gz = new Uint8Array(
    await new Response(
      new Blob([JSON.stringify(envelope)]).stream().pipeThrough(new CompressionStream("gzip")),
    ).arrayBuffer(),
  );
  let bin = "";
  for (const b of gz) bin += String.fromCharCode(b);
  const data = btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  return (
    "beanconqueror://ADD_BREW?len=" +
    data.length +
    data
      .match(/.{1,400}/g)
      .map((part, i) => `&shareBrew${i}=${part}`)
      .join("")
  );
}
async function openInBeanconqueror(r) {
  try {
    if (typeof CompressionStream !== "function")
      throw new Error(__WEBUI_TEXT__("stats.beanconqueror_unsupported"));
    location.assign(await shotBeanconquerorLink(r));
  } catch (e) {
    R.message(
      R.formatCommandError(__WEBUI_TEXT__("stats.could_not_open_beanconqueror"), e),
      "error",
    );
  }
}
// An action menu on the shared popup: a string starts a group, null draws a
// separator, and [icon path, title, hint, run, danger] is an item.
function actionMenu(label, items) {
  const menu = R.createPopup(label, (p, ...args) => {
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
const ICON = {
  csv: "M4 5h16v14H4zM4 10h16M4 15h16M10 5v14",
  json: "M8 3H7a2 2 0 0 0-2 2v4a2 2 0 0 1-2 2 2 2 0 0 1 2 2v4a2 2 0 0 0 2 2h1M16 3h1a2 2 0 0 1 2 2v4a2 2 0 0 0 2 2 2 2 0 0 0-2 2v4a2 2 0 0 1-2 2h-1",
  chart: "M3 20h18M4 16l4-6 4 3 4-7 4 5",
  visualizer: "M3 3v18h18M7 15l3-4 3 2 5-7",
  bean: "M17 4c3 3 2 9-2 13s-10 5-13 2 0-9 4-13 8-5 11-2zM15 5c-1 3-3 4-5 6s-3 5-4 8",
  trash: "M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3",
};
let shotMenu;
// The pending export window: a second export and scroll paging wait for it
// instead of orphaning it to its timeout.
let statsExportInFlight = null;
function statsFrameWindow(offset, limit, sort, dir, timeoutMs) {
  R.uiState.statsFetchMark = null;
  R.uiState.statsExportRequest = ++R.uiState.statsNextRequest;
  const pending = (statsExportInFlight = new Promise((resolve) => {
    const timer = setTimeout(() => R.uiState.statsExportResolve?.(null), timeoutMs);
    R.uiState.statsExportResolve = (rows) => {
      clearTimeout(timer);
      R.uiState.statsExportResolve = null;
      R.uiState.statsExportRequest = 0;
      resolve(rows);
    };
    R.sendUiOperation({
      op: "stats",
      on: true,
      fetch: true,
      request: R.uiState.statsExportRequest,
      offset,
      limit,
      sort,
      dir,
      filter: {},
    });
  }));
  pending.then(() => {
    statsExportInFlight = null;
  });
  return pending;
}
async function clearShotHistory() {
  if (!confirm(__WEBUI_TEXT__("runtime.clear_all_recorded_shot_history_this_cannot"))) return;
  return R.withCommandGate(async () => {
    try {
      await R.api("/api/v1/stats/clear", {
        method: "POST",
        body: R.body({ confirm: "CLEAR_SHOT_LOG" }),
      });
      R.uiState.shotHistory = {
        bootId: R.uiState.shotHistory.bootId || 0,
        total: 0,
        hasMore: false,
        shots: [],
      };
      R.uiState.shotStats = {};
      renderShots();
      R.message(__WEBUI_TEXT__("runtime.shot_history_cleared"), "ok");
    } catch (e) {
      R.message(
        R.formatCommandError(__WEBUI_TEXT__("runtime.could_not_clear_shot_history"), e),
        "error",
      );
    }
  });
}
let ready = false;
export function init() {
  if (ready) return;
  ready = true;
  R.setStatsRenderers({
    renderShots,
    renderStatsDurChart,
    relayoutCharts: () => chartObserver && layoutChartLabels(),
  });
  // The toolbar exports every stored shot, whatever the filter; a card's menu
  // acts on its own shot only.
  const exportMenu = actionMenu(__WEBUI_TEXT__("stats.export_all_shots"), () => [
    __WEBUI_TEXT__("stats.export_all_shots"),
    [ICON.csv, "CSV", __WEBUI_TEXT__("stats.csv_hint"), exportShotsCsv],
    [ICON.json, "JSON", __WEBUI_TEXT__("stats.json_all_hint"), exportShotsJson],
  ]);
  $("exportShotsButton").onclick = () => exportMenu.open($("exportShotsButton"));
  shotMenu = actionMenu(__WEBUI_TEXT__("stats.shot_actions"), (r) => [
    __WEBUI_TEXT__("stats.open_in"),
    [
      ICON.bean,
      "Beanconqueror",
      __WEBUI_TEXT__("stats.beanconqueror_hint"),
      () => openInBeanconqueror(r),
    ],
    __WEBUI_TEXT__("stats.download"),
    [
      ICON.json,
      "JSON",
      __WEBUI_TEXT__("stats.json_shot_hint"),
      () => downloadJson(`openbrewbyweight-shot-${r.id}.json`, shotsJson([r])),
    ],
    [
      ICON.chart,
      "Gaggiuino",
      __WEBUI_TEXT__("stats.gaggiuino_hint"),
      () => downloadJson(`openbrewbyweight-shot-${r.id}-gaggiuino.json`, shotGaggiuino(r)),
    ],
    [
      ICON.visualizer,
      "Visualizer",
      __WEBUI_TEXT__("stats.visualizer_hint"),
      () => downloadJson(`openbrewbyweight-shot-${r.id}-visualizer.json`, shotVisualizer(r)),
    ],
    null,
    [ICON.trash, __WEBUI_TEXT__("stats.delete_shot"), "", () => deleteOneShot(r.id), true],
  ]);
  $("clearShotsButton").onclick = clearShotHistory;
  if ($("shotSort")) {
    $("sortDateButton").onclick = () => setShotSort("date");
    $("sortRatingButton").onclick = () => setShotSort("rating");
    syncShotSortButtons();
  }
  for (const b of $("statsWindow").querySelectorAll("button"))
    b.onclick = () => {
      // Only the header changes: the firmware refreshes it and keeps the rows.
      R.uiState.statsWindow = +b.dataset.v;
      renderShotStats();
      R.sendUiOperation({ op: "stats", on: true, window: R.uiState.statsWindow });
    };
  R.createFilterMenu(
    "shotFilterButton",
    __WEBUI_TEXT__("history.filter_types"),
    shotFilterGroups,
    (sel) => {
      R.uiState.shotFilter = Object.fromEntries(
        Object.entries(sel)
          .filter(([, v]) => v.length)
          .map(([k, v]) => [k, k === "result" ? v : v.map(Number)]),
      );
      R.statsSendSubscribe();
    },
  );
  R.armListSentinel("shotLogSentinel", loadMoreShots);
  R.armStickyBar("shotStickySentinel");
  renderStatsDurChart();
}
