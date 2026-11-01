// Stats consistency model: thresholds as a share of each preset's average, units
// per data (seconds for one preset, percent for several), and a vertical scale
// with a floor, an outlier cap and tidy label steps.
{
  const assert = require('assert');
  const model = new Function(`${blockAt(viewJs.stats, 'const CONSISTENT = ')};return consistencyModel;`)();
  const near = (a, b, what) => assert.ok(Math.abs(a - b) < 1e-9, `${what}: ${a} != ${b}`);
  // The firmware sends the window's timed shots oldest first.
  const shots = (times, preset = () => 1) =>
    times.map((arrivalS, i) => ({id: 100 + i, presetId: preset(i), arrivalS}));
  const alternate = (lo, hi, n = 10) => Array.from({length: n}, (_, i) => (i % 2 ? hi : lo));

  // Identical shots: very consistent, scale at its floor of twice the 5 % band.
  const flat = model(shots(Array(10).fill(30)));
  assert.equal(flat.level, 'good');
  assert.equal(flat.sec, 30);
  near(flat.typical, 0, 'flat typical');
  near(flat.M * flat.sec, 3, 'flat scale in seconds');
  near(flat.spread, 0, 'flat spread');

  // Neighbour changes of 4 %, 8 % and 13.3 % of a 30 s average.
  assert.equal(model(shots(alternate(29.4, 30.6))).level, 'good');
  const fair = model(shots(alternate(28.8, 31.2)));
  assert.equal(fair.level, 'fair');
  near(fair.typical * fair.sec, 2.4, 'fair typical in seconds');
  assert.equal(model(shots(alternate(28, 32))).level, 'poor');

  // Fewer than five shots never get a verdict.
  const few = model(shots([30, 31, 29]));
  assert.equal(few.level, 'few');
  assert.equal(few.typical, null);
  assert.equal(model(shots([30])).level, 'few');
  assert.equal(model([]).rows.length, 0);

  // Every shot of the selected window counts, in the order it arrives.
  const twelve = model(shots(Array.from({length: 12}, (_, i) => 30 + (i % 3))));
  assert.deepEqual(twelve.rows.map((r) => r.id), Array.from({length: 12}, (_, i) => 100 + i));

  // Several presets: each shot against its own average, in percent, with 5 % label steps.
  const mixed = model(shots([20, 45, 20.4, 45.9, 19.6, 44.1, 20, 45], (i) => (i % 2 ? 2 : 1)));
  assert.equal(mixed.sec, 0);
  assert.equal(mixed.level, 'good');
  near(mixed.M, 0.1, 'mixed floor');
  near(mixed.rel[1], 0, 'a lungo at its own average');

  // One gushing shot cannot flatten the other nine: the scale stops at 30 %,
  // rounded up to whole seconds, and the shot is drawn past it as clipped.
  const outlier = model(shots([...Array(9).fill(30), 45]));
  near(outlier.sec, 31.5, 'outlier average');
  near(outlier.M * outlier.sec, 10, 'outlier scale in seconds');
  assert.ok(outlier.rel[9] > outlier.M);

  // Short shots round the floor to half seconds instead of whole ones.
  const short = model(shots(Array(10).fill(18)));
  near(short.M * short.sec, 2, 'short shot scale in seconds');
}
// Preset names are user text: the consistency pill escapes them before the
// pill writes its label as markup.
{
  const assert = require('assert');
  const escHtml = new Function(`${blockAt(runtimeJs, 'function escHtml(')};return escHtml;`)();
  assert.equal(escHtml('<b>"A&B"'), '&#60;b&#62;&#34;A&#38;B&#34;');
  assert.ok(codeIncludes(viewJs.stats, 'R.placePill(pill, R.escHtml(text),'),
      'The consistency pill must escape the preset name');
}
// The Stats averages resolve their element ids from fixed names, never from
// translated text, so every locale fills all five values.
{
  const assert = require('assert');
  const filled = [];
  const header = {avgDurationS: 1, avgYieldG: 2, shotsPerDay: 3, avgErrorPct: 4, avgFlowGps: 5,
    shotCount: 10};
  new Function('$', 'R', '__WEBUI_TEXT__', 'renderStatsDurChart', 'statsHeader', 'statsWindowChoices',
      `${blockAt(rawViewJs.stats, 'function renderShotStats(')};renderShotStats();`)(
      (id) => (filled.push(id), {classList: {toggle() {}}, querySelectorAll: () => []}),
      {uiState: {shotStats: header, statsWindow: 10}, sub: (t, v) => t + v},
      (key) => 'translated ' + key, () => {}, () => header, () => {});
  assert.deepEqual(filled, ['statsAvgDur', 'statsAvgWeight', 'statsAvgDaily', 'statsAvgErr',
    'statsAvgFlow', 'statsWindowHint']);
}
// S18: the window choices are every window smaller than the good shots the
// list holds, plus the smallest one covering them; none up to ten. The
// companion's merged header adds 500 and 1000; a hidden selection falls back.
{
  const assert = require('assert');
  const choices = (n, merged, selected) => {
    const buttons = [10, 50, 100, 500, 1000].map((v) => ({dataset: {v: String(v)}, hidden: false,
      setAttribute() {}}));
    const group = {hidden: false, querySelectorAll: () => buttons};
    const sent = [];
    const uiState = {statsWindow: selected};
    new Function('$', 'R', 'statsMergedGood', 'statsGood', 'statsMerged', 'statsSynced',
        'sendStatsWindow', `${blockAt(rawViewJs.stats, 'function statsWindowChoices(')};statsWindowChoices();`)(
        () => group, {uiState}, merged ? n : null, n, merged ? {} : null, true,
        () => sent.push(uiState.statsWindow));
    return {shown: group.hidden ? [] : buttons.filter((b) => !b.hidden).map((b) => +b.dataset.v),
      selected: uiState.statsWindow, sent};
  };
  assert.deepEqual(choices(1, false, 10).shown, []);
  assert.deepEqual(choices(10, false, 10).shown, []);
  assert.deepEqual(choices(11, false, 10).shown, [10, 50]);
  assert.deepEqual(choices(51, false, 10).shown, [10, 50, 100]);
  assert.deepEqual(choices(134, false, 10).shown, [10, 50, 100]);
  assert.deepEqual(choices(134, true, 10).shown, [10, 50, 100, 500]);
  assert.deepEqual(choices(501, true, 10).shown, [10, 50, 100, 500, 1000]);
  const fallen = choices(30, true, 500);
  assert.deepEqual([fallen.selected, fallen.sent], [50, [50]], 'a hidden window falls to the largest shown');
}
// Stats and History render cost: one curve model per shot row and CSV line, a
// rated star keeps keyboard focus, label layout sleeps off Stats, and History
// clones each type icon instead of re-parsing its SVG per row.
if (!codeIncludes(viewJs.stats, 'renderShotSpark(spark,r,m)') ||
    !codeIncludes(viewJs.stats, 'f=shotFlowCurveGS(r,m)') ||
    !codeIncludes(viewJs.stats, 'cut=shotCut(r,m,actual)') ||
    !codeIncludes(viewJs.stats, 'shotMaxFlowGS(r,m)') ||
    !codeIncludes(viewJs.stats, '.starRate`)?.children[star-1]?.focus()') ||
    !codeIncludes(viewJs.stats, 'new ResizeObserver(()=>R.activeView===stats&&layoutChartLabels())') ||
    !codeIncludes(viewJs.history, 'icon.append(histIcons[r.type].cloneNode(true))') ||
    codeIncludes(viewJs.history, 'icon.innerHTML=typeSvg')) {
  throw new Error('Stats and History must not rebuild per-row work they can reuse');
}
