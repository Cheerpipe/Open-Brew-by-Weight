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
  const nodes = {};
  const render = (statsMerged, ready, archiveView = false) =>
    new Function('$', 'R', '__WEBUI_TEXT__', 'renderStatsDurChart', 'statsHeader', 'statsWindowChoices',
        'statsMerged', 'archiveStatsReady',
        `${blockAt(rawViewJs.stats, 'function renderShotStats(')};renderShotStats();`)(
        (id) => (filled.push(id), nodes[id] ??= {classList: {toggle() {}}, querySelectorAll: () => []}),
        {uiState: {shotStats: header, statsWindow: 10}, sub: (t, v) => t + v, archiveView},
        (key) => 'translated ' + key, () => {}, () => header, () => {}, statsMerged, () => ready);
  render(null, false);
  assert.deepEqual(filled, ['statsAvgDur', 'statsAvgWeight', 'statsAvgDaily', 'statsAvgErr',
    'statsAvgFlow', 'statsWindowHint', 'statsHeaderNote']);
  // D12: opened directly no note; through the companion without its header the
  // controller's figures are labelled; the archive view says none are there.
  assert.equal(nodes.statsHeaderNote.hidden, true, 'opened directly, Stats need no note');
  render(null, true);
  assert.deepEqual([nodes.statsHeaderNote.hidden, nodes.statsHeaderNote.textContent],
      [false, 'translated stats.header_controller_only']);
  render(header, true);
  assert.equal(nodes.statsHeaderNote.hidden, true, 'the note leaves when the merged header arrives');
  render(null, false, true);
  assert.deepEqual([nodes.statsHeaderNote.hidden, nodes.statsHeaderNote.textContent],
      [false, 'translated stats.header_unavailable']);
  const shown = new Function('R', 'statsMerged',
      `${rawViewJs.stats.match(/const statsHeader = .*\n/)[0]};return statsHeader();`);
  assert.deepEqual(shown({archiveView: true, uiState: {shotStats: header}}, null), {},
      'the archive view never shows a stale controller header');
  assert.equal(shown({archiveView: false, uiState: {shotStats: header}}, null), header);
}
// S18: the window choices are every window smaller than the good shots the
// list holds, plus the smallest one covering them; none up to ten. The
// companion's merged header adds 500 and 1000; a hidden selection falls back.
{
  const assert = require('assert');
  const choices = (n, merged, selected, standIn = false) => {
    const pressed = [];
    const buttons = [10, 50, 100, 500, 1000].map((v) => ({dataset: {v: String(v)}, hidden: false,
      setAttribute: (k, on) => on === 'true' && pressed.push(v)}));
    const group = {hidden: false, querySelectorAll: () => buttons};
    const sent = [];
    const uiState = {statsWindow: selected};
    let headers = 0;
    new Function('$', 'R', 'statsMergedGood', 'statsGood', 'statsMerged', 'statsSynced',
        'sendStatsWindow', 'refreshMergedHeader', 'archiveStatsReady',
        `${blockAt(rawViewJs.stats, 'function statsWindowChoices(')};statsWindowChoices();`)(
        () => group, {uiState}, merged ? n : null, n, merged ? {} : null, true,
        () => sent.push(uiState.statsWindow), () => headers++, () => standIn);
    return {shown: group.hidden ? [] : buttons.filter((b) => !b.hidden).map((b) => +b.dataset.v),
      selected: uiState.statsWindow, sent, headers, pressed};
  };
  assert.deepEqual(choices(1, false, 10).shown, []);
  assert.deepEqual(choices(10, false, 10).shown, []);
  assert.deepEqual(choices(11, false, 10).shown, [10, 50]);
  assert.deepEqual(choices(51, false, 10).shown, [10, 50, 100]);
  assert.deepEqual(choices(134, false, 10).shown, [10, 50, 100]);
  assert.deepEqual(choices(134, true, 10).shown, [10, 50, 100, 500]);
  assert.deepEqual(choices(501, true, 10).shown, [10, 50, 100, 500, 1000]);
  assert.deepEqual(choices(50, false, 10).shown, [10, 50]);
  assert.deepEqual(choices(50, true, 10).shown, [10, 50]);
  assert.deepEqual(choices(500, true, 10).shown, [10, 50, 100, 500]);
  assert.deepEqual(choices(501, false, 10).shown, [10, 50, 100]);
  for (const n of [1, 10]) assert.deepEqual(choices(n, true, 10).shown, []);
  assert.deepEqual(choices(11, true, 10).shown, [10, 50]);
  assert.deepEqual(choices(51, true, 10).shown, [10, 50, 100]);
  const fallen = choices(30, true, 500);
  assert.deepEqual([fallen.selected, fallen.sent], [50, [50]], 'a hidden window falls to the largest shown');
  // 1000 -> 500 leaves the controller's window at 100 (the stub records the
  // choice): the merged header follows itself.
  const deep = choices(300, true, 1000);
  assert.deepEqual([deep.selected, deep.sent, deep.headers], [500, [500], 1]);
  assert.equal(choices(30, false, 100).headers, 0, 'the direct path rebuilds no merged header');
  // D12: while the merged header is missing the controller's windows show, the
  // pressed one is the window that header covers, and the choice of 500 is
  // kept for when the merged header returns.
  const fallback = choices(134, false, 500);
  assert.deepEqual([fallback.shown, fallback.pressed, fallback.selected, fallback.sent],
      [[10, 50, 100], [100], 500, []]);
  assert.deepEqual(choices(134, true, 500).pressed, [500]);
  // A controller holding few shots (after a factory reset) presses the largest
  // window shown and keeps the 500 for when the merged header returns.
  const few = choices(30, false, 500, true);
  assert.deepEqual([few.shown, few.pressed, few.selected, few.sent], [[10, 50], [50], 500, []]);
}
// The companion archive merge (S9, S12): one comparator over both sources, a
// cut at the first row a source still has to send, a record both hold shown
// once, live inserts re-merged; a failed page keeps what is loaded and is
// retried; a reload keeps the loaded depth. One Delete (D6): the controller
// deletes its own records, the companion only those it alone holds, and
// Delete shows only with Admin unlocked outside the archive view (D2).
{
  const assert = require('assert');
  const merge = new Function(`${blockAt(rawViewJs.records, 'function recordOrder(')}
      ${blockAt(rawViewJs.records, 'function mergeRecords(')}; return {recordOrder, mergeRecords};`)();
  const archived = (gen, id, extra = {}) => Object.defineProperty({id, archiveGeneration: gen, ...extra},
      '_archive', {value: true});
  const source = (rows, done, gens = [{generation: 2, current: true}, {generation: 1}]) => ({rows, done, gens});
  const own = [{id: 9}, {id: 8}, {id: 7}];
  const s = source([archived(2, 3), archived(1, 40), archived(1, 39)], false);
  const order = merge.recordOrder(s, 'date', 'desc');
  let m = merge.mergeRecords(own, true, s, order);
  assert.deepEqual(m.list.map((r) => r.id), [9, 8, 7], 'the controller pages next while its rows sort first');
  assert.equal(m.need, 'own');
  m = merge.mergeRecords(own, false, s, order);
  assert.deepEqual(m.list.map((r) => [r.archiveGeneration ?? 2, r.id]), [[2, 9], [2, 8], [2, 7], [2, 3], [1, 40],
    [1, 39]]);
  assert.equal(m.need, 'archive', 'the archive pages on when the controller is done');
  const kept = source([archived(2, 8), archived(1, 5)], true);
  m = merge.mergeRecords(own, false, kept, merge.recordOrder(kept, 'date', 'desc'));
  assert.deepEqual(m.list.map((r) => r.id), [9, 8, 7, 5], 'a record both hold shows once');
  assert(!m.list[1]._archive, 'the controller\'s row stands for a record both hold');
  m = merge.mergeRecords([{id: 10}, ...own], false, kept, merge.recordOrder(kept, 'date', 'desc'));
  assert.equal(m.list[0].id, 10, 'a live insert merges in');
  const rated = source([archived(1, 2, {rating: 5}), archived(1, 4, {rating: 0})], true);
  m = merge.mergeRecords([{id: 9, rating: 3}, {id: 8, rating: 0}], false, rated,
      merge.recordOrder(rated, 'rating', 'desc'));
  assert.deepEqual(m.list.map((r) => r.id), [2, 9, 8, 4], 'rating order: unrated last, then newest first');

  const pages = [];
  const timers = [];
  const sources = {shots: {rows: [], done: false, failures: 0}};
  const load = new Function('archiveSources', 'archivePage', 'setTimeout',
      `${blockAt(rawViewJs.records, 'async function loadArchive(')}; return loadArchive;`)(
      sources, async () => { pages.push(1); throw Error('down'); }, (fn, ms) => timers.push(ms));
  for (let n = 0; n < 4; n++) await load('shots', () => {});
  assert.deepEqual([pages.length, timers, sources.shots.done], [4, [4e3, 8e3, 16e3], true],
      'a failed page is asked again, later each time, then the list ends there');
  const old = {key: 'k', query: {}, rows: [1, 2, 3, 4, 5], done: false};
  const live = {shots: old};
  let rendered = 0;
  const reload = new Function('archiveSources', 'archiveReady', 'archiveFresh', 'archivePage',
      `${blockAt(rawViewJs.records, 'async function reloadArchive(')}; return reloadArchive;`)(
      live, () => true, (key, query) => ({key, query, rows: [], done: false}),
      async (_, next) => { next.rows.push(1, 2); next.done = next.rows.length >= 6; });
  await reload('shots', () => rendered++);
  assert.deepEqual([live.shots.rows.length, rendered], [6, 1], 'a reload keeps the loaded depth');
  const calls = [];
  let refuse = false;
  const remove = new Function('companionArchive', 'archiveHere', 'archiveChanged', 'R',
      `${blockAt(rawViewJs.records, 'async function removeRecord(')}; return removeRecord;`)(
      async (path, body) => calls.push([path, body]), () => true, (store) => calls.push(['changed', store]),
      {api: async (path, options) => {
        if (refuse) throw Object.assign(Error('locked'), {code: 'ADMIN_LOCKED'});
        calls.push([path, options.body]);
      }, body: (b) => JSON.stringify(b)});
  let dropped = 0, renders = 0;
  await remove('shots', {id: 4}, '/api/v1/stats/delete', () => dropped++, () => renders++);
  await remove('shots', archived(1, 40), '/api/v1/stats/delete', () => dropped++, () => renders++);
  assert.deepEqual(calls, [['/api/v1/stats/delete', '{"id":4}'], ['changed', 'shots'],
    ['delete', {store: 'shots', id: 40, generation: 1}], ['changed', 'shots']]);
  assert.deepEqual([dropped, renders], [1, 2], 'only the controller\'s row drops at once');
  refuse = true;
  await assert.rejects(remove('shots', {id: 3}, '/api/v1/stats/delete', () => dropped++, () => renders++));
  assert.deepEqual([dropped, renders, calls.length], [1, 2, 4], 'a refusal leaves the list as it was');
  const itemSource = rawViewJs.records.slice(rawViewJs.records.indexOf('const deleteItem ='),
      rawViewJs.records.indexOf(';\n', rawViewJs.records.indexOf('const deleteItem =')) + 1);
  const item = (archiveView, unlocked) => new Function('R', 'ICON_TRASH',
      `${itemSource}return deleteItem('Delete shot', () => {});`)(
      {archiveView, adminUnlockedHere: () => unlocked}, 'icon');
  assert.equal(item(false, false).length, 0, 'locked: no Delete');
  assert.equal(item(true, true).length, 0, 'the archive view never deletes');
  assert.deepEqual(item(false, true).map((i) => [i[1], i[4]]), [['Delete shot', true]]);
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
// AC15 (D13): the whole-history header stays in Stats. No other module asks
// the companion for it, and Stats never writes it into the shared models
// Home and the presets read.
{
  const assert = require('assert');
  const others = Object.entries(rawViewJs).filter(([name]) => name !== 'stats')
      .concat([['runtime', rawRuntimeJs], ['app', rawAppJsSource]]);
  for (const [name, source] of others)
    assert(!source.includes('statsMerged') && !source.includes('"stats?"'),
        `${name} must not read the companion's whole-history header`);
  assert(!/(uiState|presetState)[\w.]*\s*=\s*statsMerged/.test(rawViewJs.stats) &&
         !/=\s*reply\?\.stats[^;]*;[^\n]*uiState/.test(rawViewJs.stats),
      'Stats keeps the merged header to itself');
}
// D1, AC5: Admin holds both Clears in a records group right before Factory
// reset; opened directly with a companion keeping a copy, the confirmation
// says its older records go too, and the shared record models empty.
{
  const assert = require('assert');
  const admin = partialHtml.admin;
  assert(admin.indexOf('id="clearShotHistoryButton"') > 0 &&
         admin.indexOf('id="clearActivationHistoryButton"') > admin.indexOf('id="clearShotHistoryButton"') &&
         admin.indexOf('data-g="records"') < admin.indexOf('data-g="reset"') &&
         admin.indexOf('id="factoryResetButton"') > admin.indexOf('id="clearActivationHistoryButton"'));
  const asked = [], sent = [];
  let reset = 0;
  const clear = new Function('R', 'location', '__WEBUI_TEXT__',
      `${blockAt(rawViewJs.admin, 'async function clearRecords(')};return clearRecords;`);
  const run = (protocol, archive) => clear({
    homeStatusCache: () => ({companion: {archive}}),
    askConfirm: async (text) => (asked.push(text), true),
    withCommandGate: (fn) => fn(),
    api: async (path, options) => sent.push([path, options.body]),
    body: (b) => JSON.stringify(b), message() {}, formatCommandError: (f) => f,
  }, {protocol}, (k) => k)('/api/v1/stats/clear', 'CLEAR_SHOT_LOG', 'Clear?', 'done', 'fail', () => reset++);
  await run('http:', true);
  await run('https:', true);
  await run('http:', false);
  assert.deepEqual(asked, ['Clear? records.clear_companion_too', 'Clear?', 'Clear?']);
  assert.deepEqual(sent[0], ['/api/v1/stats/clear', '{"confirm":"CLEAR_SHOT_LOG"}']);
  assert.equal(reset, 3);
}
