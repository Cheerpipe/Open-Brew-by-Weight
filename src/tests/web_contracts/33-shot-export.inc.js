// Shot export: the v1 JSON carries every stored Stats column and validates
// against its published schema, with every shot, bad ones included, and each
// one's decoded trace (the decoder reads the firmware's own fixture),
// the Gaggiuino file reads the way Visualizer.coffee parses it, and the
// Beanconqueror link decodes within the brew handoff's limits. The toolbar
// menu exports everything; a card's menu acts on its own shot, holds Delete
// and opens the trace view; what reads the firmware waits for a shot to end.
(async () => {
  const assert = require('assert').strict;
  const vm = require('vm');
  const schemaDir = path.join(sketchDir, '..', 'docs', 'schemas');
  const schema = JSON.parse(fs.readFileSync(
      path.join(schemaDir, 'openbrewbyweight-shot-export-v1.schema.json'), 'utf8'));
  const example = JSON.parse(fs.readFileSync(
      path.join(schemaDir, 'openbrewbyweight-shot-export-v1.example.json'), 'utf8'));
  // One schema, version 1, and the companion link's shared vectors; the
  // retired files are gone.
  assert.deepEqual(fs.readdirSync(schemaDir).sort(), ['README.md', 'companion-link-v1.vectors.json',
    'openbrewbyweight-shot-export-v1.example.json', 'openbrewbyweight-shot-export-v1.schema.json']);
  assert.ok(example.shots.some((x) => x.trace), 'The example shows a trace');
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
    slowMarginG: 2, delayS: 0.4, wallS: 45, fastRampS: 0, slowRampS: 4, fastCurve: 'even',
    slowCurve: 'late', fastCurveHardness: 'soft', slowCurveHardness: 'hard', dripDelayS: 3,
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
  vm.runInContext('var R = this;' + blockAt(runtimeJs, 'function pad2(') +
      blockAt(runtimeJs, 'function verdictLabel(') +
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
  assert.deepEqual([doc.version, doc.$schema, example.version, example.$schema],
      [1, schema.$id, 1, schema.$id]);
  assert.ok(doc.shots.every((x) => !('trace' in x)), 'No trace unless one is given');
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

  // The trace decoder reads the firmware's own export (TRC25 writes and
  // checks the same fixture with the firmware layout).
  const fixtureHex = fs.readFileSync(
      path.join(sketchDir, 'tests', 'fixtures', 'shot_trace_v1_hex.txt'), 'utf8').replace(/\s/g, '');
  const fixture = new Uint8Array(fixtureHex.match(/../g).map((h) => parseInt(h, 16))).buffer;
  const vocab = {version: 1, profile: [], debug: [], signals: [], endReasons: [],
    weightSources: ['none', 'post_drip', 'last_known']};
  Object.assign(vocab.profile, {16: 'CUP_PLACED', 17: 'CUP_REMOVED', 21: 'FIRST_DROP',
    31: 'STATE_CHANGED'});
  Object.assign(vocab.debug, {2: 'machine circuit closed'});
  Object.assign(vocab.signals, {12: 'IDLE_ELIGIBILITY', 19: 'WEIGHT_CONTROL'});
  Object.assign(vocab.endReasons, {3: 'scale threshold'});
  Object.assign(context, {TextDecoder, TextEncoder, DataView, Uint8Array, setInterval, clearInterval,
    sub: (text, values) => values.reduce((t, v, i) => t.replace('{' + i + '}', v), text)});
  const traceJs = viewJs.stats.slice(viewJs.stats.indexOf('const TRACE_KINDS = ['),
      viewJs.stats.indexOf('let traceVocab = null;')) +
      ['function decodeShotTrace(', 'function traceName(', 'function traceDetail(',
        'function showShotTrace(', 'function drawShotTrace(']
          .map((m) => blockAt(viewJs.stats, m)).join('\n');
  vm.runInContext(traceJs, context);
  const trace = JSON.parse(JSON.stringify(context.decodeShotTrace(fixture, vocab)));
  check(schema.$defs.trace, trace, 'trace');
  assert.deepEqual([trace.shotId, trace.bootId, trace.anchorMs, trace.endedAtMs, trace.endReasonCode,
    trace.endReason, trace.vocabularyVersion], [12, 7, 10000, 52000, 3, 'scale threshold', 1]);
  assert.deepEqual(trace.flags, {verboseTruncated: false, decisionsTruncated: false,
    interrupted: false, tailDropped: false});
  assert.deepEqual([trace.snapshot.minimumCupG, trace.snapshot.emptyReferenceG,
    trace.snapshot.scaleName, trace.snapshot.stabilitySamples, trace.snapshot.flags.autoTare,
    trace.snapshot.flags.cupPresent, trace.snapshot.flags.cupTared], [50, null, 'Lunar', 5, true,
    true, false]);
  // A pre-shot argument wider than 16 bits keeps its high half.
  assert.deepEqual(trace.preShot.sort((x, y) => x.code - y.code), [{beforeMs: 900, type: 'profile',
    code: 16, name: 'CUP_PLACED', a: 800, b: 3, arg1: 3, arg2: 0, repeats: 1},
  {beforeMs: 500, type: 'profile', code: 17, name: 'CUP_REMOVED', a: 5, b: 70001 & 0xffff,
    arg1: 70001, arg2: 9, repeats: 0}]);
  assert.equal(trace.events.length, 11, 'Continuations merge into their record');
  const byName = (n) => trace.events.find((e) => e.name === n);
  assert.deepEqual(byName('FIRST_DROP'), {atMs: 6000, type: 'profile', code: 21,
    name: 'FIRST_DROP', a: 12, b: 70000 & 0xffff, ext: [1, 5, 0]});
  assert.deepEqual(byName('machine circuit closed'), {atMs: 8000, type: 'debug', code: 2,
    name: 'machine circuit closed', a: (100000 << 16) >> 16, b: 7, ext: [0, 1, 0]});
  assert.deepEqual(trace.events.filter((e) => e.signal === 'WEIGHT_CONTROL')
      .map((e) => [e.a, e.initial]), [[1, true]]);
  assert.deepEqual(trace.events.slice(-4).map((e) => [e.type, e.name, e.atMs]),
      [['trace', 'SLOW_WINDOW_END', 35000], ['trace', 'OVER_LINE', 36000],
        ['trace', 'STOP_REQUESTED', 40000], ['trace', 'FINAL_WEIGHT', 42000]]);
  // Readable names and details for each namespace; unknown ones stay generic.
  const detail = (e) => context.traceDetail(e, vocab);
  assert.equal(context.traceName('FIRST_DROP'), 'First drop');
  assert.equal(context.traceName('machine circuit closed'), 'Machine circuit closed');
  assert.equal(detail(byName('FIRST_DROP')), '1.2 g · 70000 · 5');
  assert.equal(detail(byName('machine circuit closed')), '100000 · 7');
  assert.equal(detail(trace.events.find((e) => e.signal === 'WEIGHT_CONTROL')),
      'Weight control = 1 · at the start');
  assert.equal(detail(byName('SLOW_WINDOW_END')),
      '35.1 g · cut line 34.0 g · offset 1.8 g · target reached');
  assert.equal(detail(byName('OVER_LINE')),
      '35.2 g · cut line 34.0 g · offset 1.8 g · 2 confirmations');
  assert.equal(detail({type: 'trace', name: 'TRACE_DISCARDED', a: 8, b: 2500}),
      '8 · after 2.5 s', 'Without a name the end reason stays a number');
  assert.equal(detail(byName('STOP_REQUESTED')), 'Scale threshold · from the Web UI');
  assert.equal(detail(byName('FINAL_WEIGHT')), '36.2 g · Last known');
  const generic = JSON.parse(JSON.stringify(context.decodeShotTrace(fixture, {...vocab, version: 2})));
  assert.deepEqual(generic.events.slice(0, 2).map((e) => e.name), ['profile_21', 'profile_31']);
  assert.ok(!('signal' in generic.events[1]), 'Signals come from the matching vocabulary');
  assert.equal(context.traceDetail(generic.events.at(-2), null), '3 · from the Web UI',
      'Another vocabulary never names its end reasons');
  assert.throws(() => context.decodeShotTrace(fixture.slice(0, 200), vocab), /could not be read/);
  const torn = new Uint8Array(fixture.slice(0));
  torn[0] ^= 1;
  assert.throws(() => context.decodeShotTrace(torn.buffer, vocab), /could not be read/);
  // More pre-shot entries than the record has room for is damage, not data.
  const overfull = new Uint8Array(fixture.slice(0));
  new DataView(overfull.buffer).setUint16(78, new DataView(fixture).getUint16(84, true) + 1, true);
  assert.throws(() => context.decodeShotTrace(overfull.buffer, vocab), /could not be read/);

  // The trace view: every event in time order with a mark on the curve, a
  // type filter and a search that narrow both, the flags spelled out.
  const fakeEl = (tag = 'div') => ({tag, children: [], dataset: {}, hidden: false, value: '',
    textContent: '', innerHTML: '', open: false,
    append(...c) {this.children.push(...c);}, replaceChildren(...c) {this.children = c;},
    showModal() {this.open = true;}, close() {this.open = false;}});
  const dom = {};
  const ids = ['shotTraceDialog', 'shotTraceMeta', 'shotTraceFlags', 'shotTraceChart',
    'shotTraceType', 'shotTraceSearch', 'shotTraceEvents', 'shotTraceEmpty', 'shotTracePre',
    'shotTraceSnapshot', 'shotTraceCounters', 'shotTraceDownload', 'shotTraceClose'];
  for (const id of ids) dom[id] = fakeEl();
  Object.assign(context, {$: (id) => dom[id], document: {createElement: (tag) => {
    const e = fakeEl(tag);
    Object.defineProperty(e, 'textContent', {
      get() {return this._t ?? this.children.map((c) => c.textContent).join('');},
      set(v) {this._t = v;}});
    return e;
  }}, formatShotTime: () => 'today', setEmptyState: (id, text) => Object.assign(dom[id],
    {textContent: text || '', hidden: !text})});
  const interrupted = {...trace, flags: {...trace.flags, interrupted: true, decisionsTruncated: true},
    counters: {...trace.counters, decisionsDropped: 4}};
  context.showShotTrace(base, interrupted, vocab, fixture);
  const rowText = (li) => li.children.map((c) => c.textContent).join(' ');
  const rows = dom.shotTraceEvents.children;
  assert.ok(dom.shotTraceDialog.open && rows.length === 11);
  assert.deepEqual(rows.map((li) => li.children[0].textContent).slice(0, 2), ['6.00 s', '7.00 s']);
  assert.equal(rowText(rows.find((li) => li.dataset.type === 'debug')),
      '8.00 s Machine circuit closed 100000 · 7');
  assert.ok(/restarted during this shot/.test(dom.shotTraceFlags.textContent) &&
      /\(0 scale events, 4 decisions\)/.test(dom.shotTraceFlags.textContent) &&
      !dom.shotTraceFlags.hidden);
  assert.match(dom.shotTraceMeta.textContent, /^today · .+ · 11 events$/);
  const marks = () => (dom.shotTraceChart.innerHTML.match(/class="traceMark/g) || []).length;
  assert.ok(marks() === 11 && dom.shotTraceChart.innerHTML.includes('class="traceCurve"'));
  dom.shotTraceType.value = 'trace';
  dom.shotTraceType.onchange();
  assert.deepEqual([rows.filter((li) => !li.hidden).length, marks()], [4, 4]);
  dom.shotTraceType.value = '';
  dom.shotTraceSearch.value = 'nothing like this';
  dom.shotTraceSearch.oninput();
  assert.ok(rows.every((li) => li.hidden) && !dom.shotTraceEmpty.hidden && marks() === 0);
  assert.ok(dom.shotTracePre.children.some((li) => rowText(li) === '−0.9 s ×2 Cup placed 80.0 g · 3 · 0'));
  assert.ok(dom.shotTracePre.children.some((li) => rowText(li) === '−0.5 s Cup removed 0.5 g · 70001 · 9'));
  assert.ok(dom.shotTraceSnapshot.children.some((c) => c.textContent === 'Minimum cup g'));
  // A vocabulary of another version names nothing: not even end reasons.
  context.showShotTrace(base, trace, {...vocab, version: 2}, fixture);
  assert.equal(rowText(dom.shotTraceEvents.children.find((li) =>
    li.textContent.includes('Stop requested'))), '40.00 s Stop requested 3 · from the Web UI');

  // Diagnostic Shot storage in words; the room never exceeds the shots the
  // history keeps, each listed trace without a shot (interrupted or refused)
  // downloads through the retrying fetch from a real button, the rest are
  // counted, and a failed read leaves no stale figure.
  {
    const ss = {};
    const diagContext = vm.createContext({$: (id) => (ss[id] ??= Object.assign(fakeEl(), {
      querySelectorAll: () => Object.entries(ss).filter(([k]) => k !== 'shotStorage' &&
          k !== 'ssOrphans' && k !== 'ssOrphansRow').map(([, e]) => e)})),
      document: {createElement: (tag) => fakeEl(tag)}, sub: context.sub,
      formatHumanTime: () => 'today'});
    vm.runInContext('var R = this;' + blockAt(viewJs.diagnostic, 'function renderShotStorage('),
        diagContext);
    const storage = {poolBytes: 5476352, liveBytes: 1048576, reclaimableBytes: 0,
      unindexedBytes: 0, unusedBytes: 0, droppedRecords: 0, good: 3, bad: 1, traceOnly: 16,
      interrupted: 2, traces: 4, truncated: 0, sinceShotId: 7, sinceLocalSec: 0,
      pendingCurves: 0, pendingTraces: 0, tracesDropped: 0, lastError: 'none',
      shotCapacity: 1000, orphanTraces: [{boot: 41, end: 0}, {boot: 40, end: 52000}]};
    diagContext.renderShotStorage({...storage, meanRecordBytes: 2048});
    assert.equal(ss.ssRoom.textContent, '2.0 KB · room for all 1000 shots the history keeps');
    diagContext.renderShotStorage({...storage, meanRecordBytes: 16384});
    assert.equal(ss.ssRoom.textContent, '16.0 KB · room for about 334 shots');
    assert.deepEqual([ss.ssShots.textContent, ss.ssSince.textContent], ['3 good · 1 bad', '#7']);
    const [crashed, refused, more] = ss.ssOrphans.children;
    assert.deepEqual([crashed.tag, crashed.type, crashed.textContent, refused.textContent, more],
        ['button', 'button', 'Startup 41 · interrupted', 'Startup 40 · not in history',
          '16 older not listed']);
    assert.ok(!ss.ssOrphansRow.hidden);
    assert.ok(codeIncludes(blockAt(viewJs.diagnostic, 'function downloadOrphanTrace('),
        'R.fetchShotTrace({ id: 0, bootId: boot, endedAtMs: end })'));
    diagContext.renderShotStorage(null);
    assert.deepEqual([ss.ssRoom.textContent, ss.ssShots.textContent, ss.ssOrphansRow.hidden],
        ['—', '—', true]);
    // Opened during a shot, the section reads itself once the shot ends.
    const load = blockAt(viewJs.diagnostic, 'async function loadShotStorage(');
    assert.ok(codeIncludes(load, 'renderShotStorage(null);') && codeIncludes(load,
        'if (e.code === "SHOT_STORAGE_BUSY" && R.shotRunning()) loadShotStorageAfterShot();'));
    const waits = [];
    let running = true, loads = 0;
    const waitContext = vm.createContext({R: {shotRunning: () => running},
      setTimeout: (fn, ms) => waits.push([fn, ms]), loadShotStorage: () => loads++});
    vm.runInContext(blockAt(viewJs.diagnostic, 'function loadShotStorageAfterShot('), waitContext);
    waitContext.loadShotStorageAfterShot();
    waits.shift()[0]();
    running = false;
    waits.shift()[0]();
    assert.deepEqual([loads, waits.length], [1, 0]);
  }

  // Exports: CSV reads the whole log of good shots; JSON carries every shot,
  // bad ones included, each with its trace, then the traces without a shot
  // Shot storage lists, one request at a time (the device slot),
  // gzip-streamed into the file.
  let saved, shown = [], window = [base, lost], inFlight = 0, maxInFlight = 0;
  const traced = new Set([412]);
  const storageWith = {orphanTraces: [{boot: 41, end: 0}, {boot: 40, end: 52000}]};
  Object.assign(context, {SHOTS_EXPORT_LIMIT: 1000, statsExportInFlight: null, uiState: {}, shotRunning: () => false, sleep: async () => {}, statsSendSubscribe() {},
    statsFrameWindow: async (...args) => {
      assert.deepEqual(args.slice(0, 5), [0, 1000, 'date', 'desc', 180e3]);
      context.lastFilter = JSON.stringify(args[5]);
      return window;
    },
    shotTraceVocabulary: async () => vocab,
    api: async (url) => (assert.equal(url, '/api/v1/diagnostic/shot-storage'), storageWith),
    fetchShotTrace: async (r) => {
      maxInFlight = Math.max(maxInFlight, ++inFlight);
      await new Promise((resolve) => setTimeout(resolve, 1));
      inFlight--;
      return traced.has(r.id) || (r.id === 0 && r.endedAtMs === 0) ? fixture : null;
    },
    saveFile: (blob, name) => (saved = {blob, name}),
    message: (text, kind = '') => shown.push([text, kind]),
    formatCommandError: (_, e) => e.message});
  // The `let exportJob` block runs through runExport.
  vm.runInContext(['function downloadJson(', 'let exportJob = null;', 'async function exportRows(',
    'async function shotTraceForFile(', 'async function orphanTracesJson(', 'function exportSink(',
    'function exportShotsJson(']
      .map((m) => blockAt(viewJs.stats, m)).join('\n'), context);
  const bad = {...base, id: 413, badReason: 'short', durationS: 5};
  window = [bad, base, lost];
  traced.add(413);
  await vm.runInContext('exportShotsJson()', context);
  assert.equal(context.lastFilter, '{"bad":true}', 'The export includes bad shots');
  assert.match(saved.name, /^openbrewbyweight-shots-diagnostics-\d{8}-\d{4}\.json\.gz$/);
  const diag = JSON.parse(zlib.gunzipSync(Buffer.from(await saved.blob.arrayBuffer())));
  check(schema, diag, 'diagnostics export');
  assert.deepEqual(diag.shots.map((x) => [x.id, x.outcome.badReason, !!x.trace]),
      [[413, 'short', true], [412, null, true], [7, null, false]]);
  assert.deepEqual(diag.shots[1].trace, trace);
  assert.equal(maxInFlight, 1, 'Traces are fetched one at a time');
  assert.deepEqual(diag.orphanTraces.map((x) => [x.bootId, x.endedAtMs, x.interrupted, !!x.trace,
    x.traceUnavailable ?? false]), [[41, 0, true, true, false], [40, 52000, false, false, true]]);
  assert.ok(shown.some(([text]) => text === 'Exporting shots with their traces… 3 of 3'));
  // A shot starting stops an export: the firmware exports nothing during a
  // shot, so the file is not written and the reason is said.
  saved = null;
  shown = [];
  context.shotRunning = () => true;
  context.statsFrameWindow = () => new Promise((resolve) => setTimeout(() => resolve(window), 700));
  await vm.runInContext('exportShotsJson()', context);
  assert.equal(saved, null);
  assert.deepEqual(shown, [['Export stopped: a shot started. Export again once it has finished.', '']]);
  context.shotRunning = () => false;
  // A trace that cannot be read now (a failed read, or the controller busy
  // with a rinse or the scale) leaves that shot marked and the file goes on;
  // only a shot starting stops it.
  context.statsFrameWindow = async () => window;
  shown = [];
  context.fetchShotTrace = async (r) => {
    if (r.id === 412) throw Object.assign(new Error('busy'), {code: 'SHOT_TRACE_BUSY'});
    if (r.id === 413) throw Object.assign(new Error('read'), {code: 'SHOT_TRACE_UNAVAILABLE'});
    return null;
  };
  context.shotTraceVocabulary = async () => {throw new Error('offline');};
  context.api = async () => {throw new Error('busy');};
  await vm.runInContext('exportShotsJson()', context);
  const partial = JSON.parse(zlib.gunzipSync(Buffer.from(await saved.blob.arrayBuffer())));
  check(schema, partial, 'partial diagnostics export');
  assert.ok(!('orphanTraces' in partial), 'Without Shot storage there is no orphan list');
  assert.deepEqual(partial.shots.map((x) => [x.id, x.traceUnavailable ?? false, 'trace' in x]),
      [[413, true, false], [412, true, false], [7, false, false]]);
  context.shotTraceVocabulary = async () => vocab;
  saved = null;
  context.shotRunning = () => true;
  await vm.runInContext('exportShotsJson()', context);
  assert.equal(saved, null);
  assert.deepEqual(shown.at(-1), ['Export stopped: a shot started. Export again once it has finished.', '']);
  context.shotRunning = () => false;
  // The card's JSON never fails for its trace.
  vm.runInContext(blockAt(viewJs.stats, 'async function downloadShotJson('), context);
  await vm.runInContext('downloadShotJson(this.badRow)', Object.assign(context, {badRow: bad}));
  const single = JSON.parse(await saved.blob.text());
  assert.deepEqual([single.shots.length, single.shots[0].traceUnavailable], [1, true]);
  vm.runInContext(blockAt(runtimeJs, 'async function fetchShotTrace('), context);
  let calls = 0;
  context.apiBinary = async (url, progress, cache) => {
    assert.deepEqual([url, progress, cache],
        ['/api/v1/diagnostic/shot-trace?id=412&boot=37&end=523311', null, 'default']);
    if (++calls < 3) throw Object.assign(new Error('saving'), {code: 'SHOT_TRACE_SAVING', status: 409});
    return {arrayBuffer: async () => fixture};
  };
  assert.equal(await context.fetchShotTrace(base), fixture);
  assert.equal(calls, 3);
  context.apiBinary = async () => {throw Object.assign(new Error('none'), {status: 404});};
  assert.equal(await context.fetchShotTrace(base), null);
  // A controller briefly busy without a shot is asked again, a bounded number
  // of times; a shot running ends it at once.
  calls = 0;
  context.apiBinary = async () => {
    calls++;
    throw Object.assign(new Error('busy'), {code: 'SHOT_TRACE_BUSY', status: 409});
  };
  await assert.rejects(context.fetchShotTrace(base), /busy/);
  assert.equal(calls, 5);
  calls = 0;
  context.shotRunning = () => true;
  await assert.rejects(context.fetchShotTrace(base), /busy/);
  assert.equal(calls, 1);
  context.shotRunning = () => false;

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
  assert.ok(codeIncludes(viewJs.stats, 'filter = {}'), 'Exports ignore the list filter');
  assert.ok(codeIncludes(blockAt(viewJs.stats, 'async function exportRows('),
      'statsFrameWindow(0, SHOTS_EXPORT_LIMIT, "date", "desc", 180e3, filter)'));
  assert.ok(codeIncludes(blockAt(viewJs.stats, 'async function exportShotsCsv('), 'exportRows(job, {})'));
  // While a shot runs, what reads the firmware is shown disabled with the
  // reason; a running export offers Cancel instead.
  assert.ok(codeIncludes(viewJs.stats, 'const afterShot = () => R.shotRunning() &&'));
  for (const item of ['exportShotsCsv, false, wait', 'exportShotsJson, false, wait',
    '() => openShotTrace(r), false, wait', '() => downloadShotJson(r), false, wait'])
    assert.ok(codeIncludes(viewJs.stats, item), `${item} waits for the shot`);
  assert.ok(codeIncludes(viewJs.stats, '? [[ICON.cancel,'));
  assert.ok(codeIncludes(blockAt(viewJs.records, 'function actionMenu('), 'b.disabled = !!off'));
  // "A shot runs" spans the drip delay: Home publishes the firmware's open
  // trace capture. A busy answer names the shot only when the UI knows one
  // runs; otherwise it is the neutral busy text.
  assert.ok(network.includes('delta.field("cycle.capturing", bool(control.traceCapturing));'));
  assert.ok(codeIncludes(blockAt(runtimeJs, 'function shotRunning('),
      'return sessionShotLive() || !!(s?.cycle?.active || s?.cycle?.capturing);'));
  for (const [source, marker] of [[viewJs.stats, 'async function openShotTrace('],
    [viewJs.diagnostic, 'async function loadShotStorage(']])
    assert.ok(codeIncludes(blockAt(source, marker), 'R.shotRunning()') &&
        blockAt(source, marker).includes('The controller is busy right now. Try again in a moment.'));
  // The offline copy keeps rows only and pauses while a shot runs.
  assert.ok(codeIncludes(blockAt(viewJs.records, 'async function syncOfflineStore('),
      'k < offsets.length && R.webUiPollingActive() && !R.shotRunning();'));
  assert.ok(!viewJs.records.includes('shot-trace') && !viewJs.records.includes('decodeShotTrace'));
  // Immutable records may come from the browser cache; the vocabulary is
  // asked once per firmware identity (a firmware-only update changes it),
  // and the route keeps it for good only under an identity no other build
  // shares.
  assert.ok(codeIncludes(viewJs.stats,
      '"/api/v1/diagnostic/shot-trace/vocabulary?fw=" + encodeURIComponent(R.SHELL_FW_VERSION)'));
  assert.ok(codeIncludes(runtimeJs, 'export { SHELL_FW_VERSION,'));
  assert.ok(network.includes('unique ? "private, max-age=31536000, immutable" : "no-cache"'));
  // A bad row in Stats shows only what it stored: nothing read from its curve.
  const statsRow = blockAt(viewJs.stats, 'function renderShots(');
  assert.ok(codeIncludes(statsRow, 'maxFlow = m && !bad ? m.maxFlow : null,') &&
      codeIncludes(statsRow, 'cut = bad ? {} : shotCut(r, m, actual),'));
  assert.ok(codeIncludes(blockAt(viewJs.records, 'function createFilterMenu('), 'createPopup('));
  assert.ok(codeIncludes(blockAt(viewJs.stats, 'function downloadJson('), 'R.saveFile('));
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
