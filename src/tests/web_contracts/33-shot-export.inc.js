// Shot export: the v1 JSON carries every stored Stats column and validates
// against its published schema, the Gaggiuino file reads the way
// Visualizer.coffee parses it, and the Beanconqueror link decodes within the
// brew handoff's limits. The toolbar menu exports everything; a card's menu
// acts on its own shot and now holds Delete.
(async () => {
  const assert = require('assert').strict;
  const vm = require('vm');
  const schemaDir = path.join(sketchDir, '..', 'docs', 'schemas');
  const schema = JSON.parse(fs.readFileSync(
      path.join(schemaDir, 'openbrewbyweight-shot-export-v1.schema.json'), 'utf8'));
  const example = JSON.parse(fs.readFileSync(
      path.join(schemaDir, 'openbrewbyweight-shot-export-v1.example.json'), 'utf8'));
  // The schema stays inside a keyword subset any reader can implement, and
  // every key the exporter writes must be documented in it.
  const keywords = new Set(['$schema', '$id', '$defs', '$ref', 'title', 'description', 'type',
    'required', 'properties', 'items', 'enum', 'const', 'minimum']);
  const typeOf = (v) => v === null ? 'null' : Array.isArray(v) ? 'array'
    : Number.isInteger(v) ? 'integer' : typeof v;
  const check = (s, v, at) => {
    if (s.$ref) return check(schema.$defs[s.$ref.split('/').pop()], v, at);
    for (const k of Object.keys(s)) assert.ok(keywords.has(k), `${at}: schema keyword ${k}`);
    if ('const' in s) assert.deepEqual(v, s.const, at);
    if (s.enum) assert.ok(s.enum.includes(v), `${at}: ${v} is not one of ${s.enum}`);
    if (s.type) {
      const types = [].concat(s.type), t = typeOf(v);
      assert.ok(types.includes(t) || (t === 'integer' && types.includes('number')),
          `${at}: ${t} is not ${types}`);
    }
    if (typeof v === 'number' && s.minimum != null) assert.ok(v >= s.minimum, `${at}: ${v}`);
    if (Array.isArray(v)) return s.items && v.forEach((x, i) => check(s.items, x, `${at}[${i}]`));
    if (v === null || typeof v !== 'object') return;
    for (const k of s.required || []) assert.ok(k in v, `${at}.${k} is required`);
    for (const [k, x] of Object.entries(v)) {
      assert.ok(s.properties && k in s.properties, `${at}.${k} is not in the schema`);
      check(s.properties[k], x, `${at}.${k}`);
    }
  };

  const base = {id: 412, bootId: 37, endedAtMs: 523311, hasWallTime: true,
    endedAtLocalSec: 1792840364 - 10800, endedAtUnixSec: 1792840364,
    timezoneOffsetMinutesAtCommit: -180, durationS: 28.4, presetId: 3, presetName: 'Daily',
    scaleName: 'Lunar', goalG: 36, actualG: 36.2, offsetG: 1.8, bbwAlgorithm: 'corridor',
    bbwAlgorithmVersion: 1, bbwAlpha: 0.35, bbwLearningApplied: true, avgFlowGS: 1.62,
    targetTempDeciC: 930, doseG: 18, ratio: 2.01, firstDropS: 6.1, tareS: null,
    shotType: 'auto', cutType: 'auto', extractionGuardEnabled: true,
    extractionExtended: false, slowExtractionGuardEnabled: true,
    slowExtractionExtended: false, stopDetail: 'normal_target', cutLineG: 35.1,
    arrivalS: 27.9, verdict: 'on_recipe', channeling: false, scaleLossSource: null,
    estimatedG: null, blind: false, fastBeforeS: 25, slowAfterS: 33, fastMarginG: 2,
    slowMarginG: 2, delayS: 0.4, wallS: 45, cutRampS: 0, dripDelayS: 3,
    actualWeightSource: 'post_drip', rating: 4,
    wCg: [0, 0, 120, 900, 2400, 2600, 3300, 3510, 3620],
    wAtMs: [100, 5000, 6200, 9000, 18000, 18500, 26000, 28400, 31400],
    wBreakBefore: [6], wHeld: [5], wTruncated: false, dropS: 6.1, dropCg: 20,
    extendedS: null, extCg: null, atmS: null, atmCg: null, atmClearedS: null,
    endS: 28.4, endCg: 3510};
  const lost = {...base, id: 7, hasWallTime: false, endedAtLocalSec: 0, endedAtUnixSec: 0,
    actualG: null, estimatedG: 30.5, stopDetail: 'auto_to_manual', cutType: 'auto',
    scaleLossSource: 'projection', blind: true, rating: 0, scaleName: '', doseG: 0, ratio: 0,
    targetTempDeciC: 0, bbwAlgorithm: 'unknown', bbwAlgorithmVersion: null, bbwAlpha: null,
    bbwLearningApplied: null, verdict: null, arrivalS: null, actualWeightSource: 'none',
    wCg: [], wAtMs: [], wBreakBefore: [], wHeld: [], dropS: null, dropCg: null,
    endS: null, endCg: null};

  const context = vm.createContext({location: {hostname: 'openbrew-kitchen.local'},
    firmwareVersion: '1.2.3', CompressionStream, Response, Blob, btoa});
  vm.runInContext(runtimeJs.slice(runtimeJs.indexOf('function axisLabel('),
      runtimeJs.indexOf('async function populateTimezoneOptions(')) + statsChartJs, context);
  const builders = ['function isoAt(', 'function shotWallTimes(', 'function shotsJson(',
    'function shotJson(', 'function coffeeSamples(', 'function shotGaggiuino(',
    'async function shotBeanconquerorLink(', 'function shotVisualizer('].map((m) => blockAt(viewJs.stats, m));
  vm.runInContext('var R = this;' + blockAt(runtimeJs, 'function verdictLabel(') +
      blockAt(runtimeJs, 'function formatShotEnded(') +
      blockAt(viewJs.stats, 'function shotCut(') +
      blockAt(viewJs.stats, 'function shotFlowCurveGS(') + builders.join('\n'), context);

  // Every Stats row column and curve key has a home in the export.
  const columns = network.match(/kStatsRowColumns\[\] =\s*((?:"[^"]*"\s*)+);/)[1]
      .replace(/"\s*"/g, '').replace(/"/g, '').split(',');
  const exporter = builders.slice(1, 4).join('\n');
  for (const key of [...columns, 'wCg', 'wAtMs', 'wHeld', 'wBreakBefore', 'wTruncated', 'dropS',
    'dropCg', 'extendedS', 'extCg', 'atmS', 'atmCg', 'atmClearedS', 'endS', 'endCg']) {
    assert.ok(exporter.includes('r.' + key), `The JSON export leaves out the Stats field ${key}`);
  }

  const doc = JSON.parse(JSON.stringify(context.shotsJson([base, lost])));
  check(schema, doc, 'export');
  check(schema, example, 'example');
  const [a, b] = doc.shots;
  assert.equal(doc.source.firmwareVersion, '1.2.3');
  assert.deepEqual(a.time, {startedAt: '2026-10-24T08:12:15-03:00',
    endedAt: '2026-10-24T08:12:44-03:00', utcOffsetMinutes: -180, endedAtUptimeMs: 523311});
  assert.deepEqual(a.recipe, {targetYieldG: 36, doseG: 18, targetTemperatureC: 93});
  assert.deepEqual(a.curve.timeS, base.wAtMs.map((t) => t / 1000));
  assert.deepEqual(a.curve.weightG, base.wCg.map((cg) => cg / 100));
  assert.deepEqual([a.curve.heldIndices, a.curve.gapBeforeIndices], [[5], [6]]);
  assert.deepEqual(a.curve.events.end, {atS: 28.4, weightG: 35.1});
  assert.equal(a.derived.flowGS.length, base.wCg.length);
  assert.equal(a.derived.flowGS[5], null, 'A held reading has no flow');
  assert.deepEqual([a.derived.errorVsTargetG, a.derived.dripG], [0.2, 1.1]);
  // Unknown values are null, never sentinels.
  assert.deepEqual(b.time, {startedAt: null, endedAt: null, utcOffsetMinutes: null,
    endedAtUptimeMs: 523311});
  assert.deepEqual([b.rating, b.scale, b.recipe.doseG, b.recipe.targetTemperatureC,
    b.outcome.ratio, b.outcome.yieldG, b.control.brewByWeight.algorithm,
    b.derived.errorVsTargetG], [null, null, null, null, null, null, null, null]);
  assert.deepEqual([b.outcome.estimatedYieldG, b.stop.scaleLossSource, b.stop.blind],
      [30.5, 'projection', true]);

  // The toolbar's JSON export reads the same whole-log window as the CSV.
  let saved;
  Object.assign(context, {SHOTS_EXPORT_LIMIT: 500, statsExportInFlight: null,
    statsFrameWindow: async (...args) => {
      assert.deepEqual(args, [0, 500, 'date', 'desc', 90e3]);
      return [base, lost];
    },
    saveFile: (blob, name) => (saved = {blob, name}),
    message: (text) => {throw new Error(text);}, formatCommandError: (_, e) => e.message});
  vm.runInContext(blockAt(viewJs.stats, 'function downloadJson(') +
      blockAt(viewJs.stats, 'async function exportShotsJson('), context);
  await vm.runInContext('exportShotsJson()', context);
  assert.match(saved.name, /^openbrewbyweight-shots-\d{8}-\d{4}\.json$/);
  assert.equal(saved.blob.type, 'application/json');
  assert.deepEqual(JSON.parse(await saved.blob.text()).shots.map((x) => x.id), [412, 7]);

  // Gaggiuino, as Visualizer.coffee reads it: datapoints with ms times, the
  // drink weight from the last point, nothing the controller never measured.
  const g = JSON.parse(JSON.stringify(context.shotGaggiuino(base)));
  assert.deepEqual([g.id, g.duration, g.timestamp], [412, 284, 1792840336]);
  assert.equal(g.datapoints.length, base.wCg.length - 1, 'Held readings are not coffee');
  for (const [i, p] of g.datapoints.entries()) {
    assert.deepEqual(Object.keys(p), ['timeInShot', 'shotWeight', 'weightFlow',
      'targetTemperature']);
    assert.ok(Number.isInteger(p.timeInShot) && (!i || p.timeInShot >= g.datapoints[i - 1].timeInShot));
  }
  assert.equal(g.datapoints.at(-1).shotWeight, 36.2);
  assert.deepEqual(g.profile, {id: 3, name: 'Daily', phases: [],
    globalStopConditions: {weight: 36}, waterTemperature: 93, recipe: {}});
  const gl = JSON.parse(JSON.stringify(context.shotGaggiuino(lost)));
  assert.deepEqual([gl.timestamp, gl.datapoints, 'waterTemperature' in gl.profile], [0, [], false]);

  // Visualizer.coffee's default Decent JSON parser: none of the keys that
  // route a file to another parser, series aligned with `elapsed` and free of
  // nulls, settings in its units, and the rest of the metadata in the notes.
  const routedAway = (j) => ['mill', 'brewFlow', 'datapoints', 'samples'].some((k) => k in j) ||
      ('profile_name' in j && 'data' in j);
  const v = JSON.parse(JSON.stringify(context.shotVisualizer(base)));
  assert.ok(!routedAway(v));
  assert.deepEqual([v.version, v.timestamp, v.profile], [2, 1792840336, {title: 'Daily'}]);
  assert.deepEqual(v.elapsed, [0.1, 5, 6.2, 9, 18, 26, 28.4, 31.4], 'Held readings are not coffee');
  assert.deepEqual(v.totals.weight, [0, 0, 1.2, 9, 24, 33, 35.1, 36.2]);
  assert.deepEqual(v.state_change, [1, 1, 2, 2, 2, 2, 2, 3]);
  for (const series of [v.flow.by_weight, v.temperature.goal]) {
    assert.ok(series.length === v.elapsed.length && series.every((x) => typeof x === 'number'));
  }
  const {espresso_notes: notes, ...settings} = v.app.data.settings;
  assert.deepEqual([v.app.app_name, v.app.app_version], ['openbrewbyweight', '1.2.3']);
  assert.deepEqual(settings, {profile_title: 'Daily', drink_weight: 36.2, bean_weight: 18,
    final_desired_shot_weight: 36, espresso_temperature: 93, espresso_enjoyment: 80});
  for (const line of ['- Yield vs target: +0.2 g (+0.6 %)', '- First drop: 6.1 s',
    '- Cut at 35.1 g · 28.4 s', '- Drip: +1.1 g', '- Scale: Lunar',
    '- Brew by weight: corridor v1, offset 1.8 g', '- Exported from Open Brew by Weight 1.2.3']) {
    assert.ok(notes.split('\n').includes(line), `Visualizer notes miss "${line}"`);
  }
  const before = Math.round(Date.now() / 1e3);
  const vl = JSON.parse(JSON.stringify(context.shotVisualizer(lost)));
  assert.ok(!routedAway(vl) && vl.timestamp >= before, 'No wall time falls back to now');
  assert.deepEqual([vl.elapsed, vl.state_change, 'temperature' in vl, vl.profile.title],
      [[], [], false, 'Daily']);
  assert.deepEqual(Object.keys(vl.app.data.settings), ['profile_title', 'drink_weight',
    'final_desired_shot_weight', 'espresso_notes']);
  assert.equal(vl.app.data.settings.drink_weight, 30.5);
  assert.ok(vl.app.data.settings.espresso_notes.includes('- Yield estimated: 30.5 g'));
  assert.ok(vl.app.data.settings.espresso_notes.includes('- Scale lost, stop by projection'));
  assert.ok(!vl.app.data.settings.espresso_notes.includes('Brew by weight'));

  // Beanconqueror's ADD_BREW handoff: len, 400-character slices, gzip JSON,
  // and the bounds its decoder enforces.
  const decode = (url) => {
    assert.ok(url.startsWith('beanconqueror://ADD_BREW?'));
    const params = new URLSearchParams(url.split('?')[1]);
    const parts = [...params].filter(([k]) => /^shareBrew\d+$/.test(k));
    parts.forEach(([k, v], i) => assert.ok(k === 'shareBrew' + i && v.length <= 400));
    const data = parts.map(([, v]) => v).join('');
    assert.ok(/^[A-Za-z0-9_-]+$/.test(data) && data.length === +params.get('len'));
    return JSON.parse(zlib.gunzipSync(Buffer.from(data, 'base64url')));
  };
  const e = decode(await context.shotBeanconquerorLink(base));
  assert.deepEqual([e.v, e.app, e.imported], [1, {name: 'Open Brew by Weight', version: '1.2.3'},
    {source: 'openbrewbyweight', sourceName: 'Open Brew by Weight',
      device: 'openbrew-kitchen.local', schema: 1}]);
  assert.match(e.brew.date, /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,3})?(?:Z|[+-]\d{2}:\d{2})$/);
  assert.deepEqual([e.brew.doseIn, e.brew.waterIn, e.brew.beverageOut],
      [{value: 18, unit: 'g'}, {value: 0, unit: 'ml'}, {value: 36.2, unit: 'g'}]);
  assert.deepEqual([e.brew.brewTime, e.brew.temperature, e.brew.firstDripTime, e.brew.rating,
    e.brew.preparationMethod, e.brew.preparationType], [28.4, 93, 6.1, 4, 'Espresso', 'PORTAFILTER']);
  const {t, weight, waterDispensed} = e.flow, sum = (list) => list.reduce((s, v) => s + v, 0);
  assert.ok(t.length === weight.length && t.length === waterDispensed.length && t.length === 8);
  assert.ok(t.every((v) => Number.isInteger(v) && v >= 0) && sum(t) === 31400);
  assert.equal(sum(weight), 362, 'Weight deltas add up to the last reading in decigrams');
  const el = decode(await context.shotBeanconquerorLink(lost));
  assert.ok(!('flow' in el) && !('rating' in el.brew) && !('doseIn' in el.brew));
  assert.equal(el.brew.beverageOut.value, 30.5);

  // The toolbar menu exports every shot; a card's ⋯ menu acts on its own shot
  // and holds Delete, as does an Activation history entry's.
  assert.ok(partialHtml.stats.includes('id="exportShotsButton" class="btnGlyph" type="button" aria-haspopup="true" aria-expanded="false"'));
  assert.ok(codeIncludes(viewJs.stats, '$("exportShotsButton").onclick = () => exportMenu.open($("exportShotsButton"))'));
  assert.ok(codeIncludes(viewJs.stats, 'btn.onclick = () => shotMenu.open(btn, r)'));
  assert.ok(codeIncludes(viewJs.stats, '() => deleteOneShot(r.id), true]'));
  assert.ok(!codeIncludes(viewJs.stats, 'btnGlyph btnDanger'));
  assert.ok(!codeIncludes(viewJs.history, 'btnGlyph btnDanger'));
  assert.ok(codeIncludes(viewJs.history, 'btn.onclick = () => entryMenu.open(btn, r)'));
  assert.ok(codeIncludes(viewJs.history, '() => deleteOneHistory(r.id), true]'));
  assert.ok(codeIncludes(viewJs.stats, 'filter: {}'), 'Exports ignore the list filter');
  assert.ok(codeIncludes(blockAt(viewJs.stats, 'async function exportShotsJson('),
      'statsFrameWindow(0, R.SHOTS_EXPORT_LIMIT, "date", "desc", 90e3)'));
  assert.ok(codeIncludes(blockAt(runtimeJs, 'function createFilterMenu('), 'createPopup('));
  assert.ok(codeIncludes(blockAt(viewJs.stats, 'function downloadJson('), 'R.saveFile('));
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
