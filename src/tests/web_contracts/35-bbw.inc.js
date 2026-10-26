{
  const assert = require('assert');
  const vm = require('vm');
  // The cutoff-algorithm selector is gone: one algorithm, one alpha baseline.
  assert(!html.includes('id="bbwAlgorithm"'));
  assert(!html.includes('Linear prediction + adaptive EWMA'));
  // Drip compensation: one value, a recent-shots chart, Set drip and Set
  // learning speed; the starting values and the two resets are gone.
  for (const id of ['resetCalibrationButton', 'resetEwmaButton', 'weightOffsetBaselineG', 'bbwAlphaBaseline'])
    assert(!html.includes('id="' + id + '"'), id);
  for (const id of ['dripSetButton', 'alphaSetButton'])
    assert(html.includes('id="' + id + '" class="btnGlyph mutable"'));
  assert(html.includes('id="dripSetG" type="number" min="0" max="5" step="0.01"'));
  assert(html.includes('id="alphaSet" type="number" min="0.01" max="1" step="0.01"'));
  assert(codeIncludes(viewJs.settings, '["dripSetG", "dripSetButton", "dripG"]'));
  assert(codeIncludes(viewJs.settings, '"/api/v1/calibration/reset", { [key]: R.number(input), baseRevision: R.formRev }'));
  const node = (classes = []) => {
    const names = new Set(classes);
    const n = {value: '', checked: false, disabled: false, hidden: false, textContent: '', innerHTML: '',
      dataset: {}, style: {}, children: [], validity: {valid: true},
      classList: {contains: name => names.has(name),
        toggle: (name, on) => on ? names.add(name) : names.delete(name)},
      get className() { return [...names].join(' '); },
      set className(v) { names.clear(); String(v).split(' ').filter(Boolean).forEach(x => names.add(x)); },
      append(...kids) { n.children.push(...kids); },
      replaceChildren(...kids) { n.children = kids; },
      querySelectorAll: () => []};
    return n;
  };
  const elements = new Map();
  for (const id of ['brewByWeight', 'learnedOffsetG', 'dripVerdict', 'dripHint', 'dripBars', 'dripBarX',
    'dripSetG', 'dripSetButton', 'alphaSet', 'alphaSetButton']) elements.set(id, node());
  const status = node(); status.lastElementChild = node(); elements.set('dripStatus', status);
  const learning = node(['bbwLearning']);
  learning.querySelectorAll = () => ['dripSetG', 'dripSetButton', 'alphaSet', 'alphaSetButton'].map(id => elements.get(id));
  const document = {activeElement: null, querySelectorAll: () => [learning],
    createElement: tag => Object.assign(node(), {tag})};
  const context = vm.createContext({$: id => elements.get(id), controlsMutable: true,
    uiState: {brewDirty: false, configDirty: false, bbwReadback: null}, configLoaded: true, formRev: 1, document});
  const start = runtimeJs.search(/let\s+bbwFormPresetId\s*=/);
  const end = runtimeJs.indexOf('function soundAlertsAreOn', start);
  vm.runInContext(runtimeJs.slice(start, end), context);
  vm.runInContext(blockAt(viewJs.settings, 'function renderDripChart(') + 'var renderSettings=renderDripChart;', context);
  const refresh = (readback) => {
    if (readback) vm.runInContext('bbwFormPresetId=' + readback.bbwPresetId + ';uiState.bbwReadback=' +
      JSON.stringify(readback), context);
    vm.runInContext('updateBbwControls()', context);
  };
  const bars = () => elements.get('dripBars').children.filter(e => e.tag === 'div' && e.className.startsWith('dcBar'));
  const marks = () => elements.get('dripBars').children.filter(e => e.className.startsWith('dcMark'));
  elements.get('brewByWeight').checked = true;
  // Learning: ten slots oldest first, a skipped shot, the line in use.
  refresh({bbwPresetId: 2, bbwEwmaOffsetG: 1.56, bbwAlpha: 0.3, bbwRecentG: [0.5, 0.48, null],
    bbwRecentAfterSet: -1, bbwRecentAfterQuick: -1, bbwQuickStart: {state: 'off', count: 0, setG: 0, quickG: 0}});
  assert.equal(elements.get('learnedOffsetG').textContent, '1.56');
  assert.equal(elements.get('dripStatus').lastElementChild.textContent, 'Learning from your shots');
  assert.equal(elements.get('dripVerdict').textContent, 'Your last shots needed 0.48–0.50 g');
  assert.equal(bars().length, 10);
  assert.equal(marks().length, 0);
  assert(elements.get('dripBars').children.some(e => e.className === 'dcUse'));
  assert(elements.get('dripBarX').innerHTML.includes('skipped'));
  assert.equal(elements.get('dripSetG').value, '1.56');
  assert.equal(elements.get('alphaSet').value, '0.30');
  assert(!elements.get('dripSetButton').disabled && !elements.get('alphaSetButton').disabled);
  // An edited field keeps the owner's value across refreshes.
  elements.get('dripSetG').value = '0.50';
  elements.get('dripSetG').dataset.edited = '1';
  refresh();
  assert.equal(elements.get('dripSetG').value, '0.50');
  // A full window of ten shots fills every slot.
  refresh({bbwPresetId: 2, bbwEwmaOffsetG: 1.56, bbwAlpha: 0.3, bbwRecentG: Array(10).fill(0.5),
    bbwRecentAfterSet: -1, bbwRecentAfterQuick: -1, bbwQuickStart: {state: 'off', count: 0, setG: 0, quickG: 0}});
  assert.equal(bars().length, 10);
  assert.equal(bars().filter(b => b.children.length).length, 10);
  // ...but not across a switch to another preset.
  refresh({bbwPresetId: 3, bbwEwmaOffsetG: 0.8, bbwAlpha: 0.3, bbwRecentG: [], bbwRecentAfterSet: -1,
    bbwRecentAfterQuick: -1, bbwQuickStart: {state: 'off', count: 0, setG: 0, quickG: 0}});
  assert.equal(elements.get('dripSetG').value, '0.80');
  assert.equal(elements.get('dripHint').textContent, 'Pull a shot with this preset to see what it needed.');
  // Quick start after Set drip: earlier shots fade, counted shots stand out.
  refresh({bbwPresetId: 2, bbwEwmaOffsetG: 1.2, bbwAlpha: 0.3, bbwRecentG: [0.52, 0.48, 0.55, 0.63],
    bbwRecentAfterSet: 2, bbwRecentAfterQuick: -1, bbwQuickStart: {state: 'armed', count: 2, setG: 1.2, quickG: 0}});
  assert.equal(elements.get('dripStatus').lastElementChild.textContent, 'Quick start · 2 of 3 shots');
  assert.equal(elements.get('dripVerdict').textContent, 'Your shots need about 0.48 g');
  assert.equal(bars().filter(b => b.className.includes('qs')).length, 2);
  assert.equal(bars().filter(b => b.className.includes('old')).length, 2);
  assert.equal(bars().filter(b => b.className.includes('next')).length, 1);
  assert.equal(marks().length, 1);
  assert.equal(marks()[0].children[0].children[0], 'Manually set to 1.20 g');
  // Applied: the quick start marker joins the manual one.
  refresh({bbwPresetId: 2, bbwEwmaOffsetG: 0.5, bbwAlpha: 0.3, bbwRecentG: [0.5, 0.52, 0.48, 0.55, 0.63],
    bbwRecentAfterSet: 3, bbwRecentAfterQuick: 0, bbwQuickStart: {state: 'applied', count: 3, setG: 1.2, quickG: 0.5}});
  assert.equal(elements.get('dripStatus').lastElementChild.textContent, 'Set from 3 shots that agreed');
  assert.equal(marks().length, 2);
  assert(elements.get('dripVerdict').hidden);
  // Unsaved recipe edits, read-only access or another preset block the sets.
  context.uiState.brewDirty = true;
  refresh();
  assert(elements.get('dripSetButton').disabled && elements.get('alphaSetButton').disabled);
  context.uiState.brewDirty = false;
  vm.runInContext('uiState.bbwReadback.bbwPresetId=1', context);
  refresh();
  assert.equal(elements.get('learnedOffsetG').textContent, '—');
  assert(elements.get('dripSetButton').disabled);
  elements.get('brewByWeight').checked = false;
  refresh();
  assert(learning.classList.contains('hidden'));

  // The preset save no longer carries drip or speed values.
  const payload = blockAt(viewJs.settings, 'function brewPayload(');
  const makePayload = new Function('$', 'R', 'document', payload + ';return brewPayload();');
  const nonMicraDocument = {documentElement: {classList: {contains: () => false}}};
  const fields = makePayload(id => elements.get(id) || {checked: true, value: 'auto'},
    {number: () => 36, sToMs: () => 30000, presetState: {activeId: 1}, bbwFormPresetId: 2}, nonMicraDocument);
  assert(!('id' in fields) && codeIncludes(blockAt(runtimeJs, 'async function saveBrewPreset('),
      "{action:'save',id:bbwFormPresetId,...patch}"),
      'A selection change must not retarget the rendered recipe');
  for (const key of ['bbwAlgorithm', 'weightOffsetBaselineG', 'bbwAlpha', 'bbwAlphaBaseline'])
    assert(!(key in fields), key);
}

(async () => {
  const assert = require('assert');
  const vm = require('vm');
  let blob;
  const records = Array.from({length: 100}, (_, i) => ({id: i + 1, bootId: 1,
    goalG: 36, actualG: 36.2, offsetG: i ? 1.5 : 0, durationS: 30,
    bbwAlgorithm: i % 2 ? 'legacy' : 'linear_ewma', bbwAlgorithmVersion: i % 2 ? 1 : 2,
    bbwAlpha: i % 2 ? 1 : .37, bbwLearningApplied: i ? true : null, presetId: i ? 255 : 0}));
  records[1].wCg = [0, 1520, 3105, 3620]; records[1].wAtMs = [0, 450, 900, 1600];
  records[2].wCg = [0, 800]; records[2].wAtMs = [0, 500];
  Object.assign(records[2], {endS: .5, actualWeightSource: 'post_drip', dripDelayS: 3});
  records[3].wAtMs = [0,137,1001,1138];
  records[3].wCg = records[3].wAtMs.slice();
  const context = vm.createContext({
    statsFrameWindow: async (offset, limit, sort, dir) => {
      assert.equal(`${offset}/${limit}/${sort}/${dir}`, '0/500/date/desc');
      return records;
    },
    SHOTS_EXPORT_LIMIT: 500, statsExportInFlight: null,
    formatShotTimeCsv: () => '', shotDisplayActualG: weight => weight,
    Blob,
    URL: {createObjectURL: value => {blob = value; return 'blob:test';}, revokeObjectURL() {}},
    document: {createElement: () => ({click() {}, remove() {}}), body: {append() {}}},
    setTimeout: () => 0,
    message: message => {throw new Error(message);}, formatCommandError: (_, e) => e.message
  });
  vm.runInContext(runtimeJs.slice(runtimeJs.indexOf('function axisLabel('),
    runtimeJs.indexOf('async function populateTimezoneOptions(')) + statsChartJs, context);
  vm.runInContext('var R = this;' + blockAt(runtimeJs, 'function saveFile(') +
      blockAt(viewJs.records, 'function downloadCsv('), context);
  vm.runInContext('var R = this;' + blockAt(viewJs.stats, 'function shotCut(') +
    blockAt(viewJs.stats, 'function shotFlowCurveGS('), context);
  vm.runInContext(blockAt(viewJs.stats, 'async function exportShotsCsv('), context);
  // A second click while an export window is pending saves nothing more.
  context.statsExportInFlight = Promise.resolve(records);
  await vm.runInContext('exportShotsCsv()', context);
  assert.equal(blob, undefined, 'A pending export must not be saved twice');
  context.statsExportInFlight = null;
  await vm.runInContext('exportShotsCsv()', context);
  const lines = (await blob.text()).split('\n').map(line => line.split(','));
  assert.equal(lines.length, 101);
  assert.equal(lines[0][11], 'offset_g');
  assert.equal(lines[1][11], '0');
  assert.deepEqual(lines[0].slice(-22), ['target_temp_c', 'dose_g', 'ratio', 'curve_truncated', 'curve_break_before', 'curve_held', 'cut_s', 'cut_weight_g', 'drip_g', 'drip_delay_s', ...Array.from({length:4}, (_,i)=>['sample_'+(i+1)+'_time_s','sample_'+(i+1)+'_weight_g','sample_'+(i+1)+'_flow_g_s']).flat()]);
  assert.deepEqual(lines[1].slice(-22), ['-1', '', '', '0', '', '', ...Array(16).fill('')]);
  assert.deepEqual(lines[2].slice(-22), ['-1', '', '', '0', '', '', '', '', '', '', '0', '0', '', '0.45', '15.2', '', '0.9', '31.05', '', '1.6', '36.2', '15.72']);
  assert.equal(+lines[2][43], 15.716666666666665);
  // The cut: stop time, the reading latched there, the drip to the settled yield, the drip delay.
  assert.deepEqual(lines[3].slice(-22), ['-1', '', '', '0', '', '', '0.5', '8', '28.20', '3', '0', '0', '', '0.5', '8', '', ...Array(6).fill('')]);
  // Without a drip tail the cut is the curve's latest weight, never an event
  // annotation such as the first drop the model appends after the readings.
  const noTail = {endS: 2, durationS: 2, actualWeightSource: 'post_drip', dropS: 0.5, dropCg: 20,
    wAtMs: [0, 1000, 2000], wCg: [0, 1000, 2000]};
  assert.deepEqual(JSON.parse(JSON.stringify(context.shotCut(noTail, context.buildShotSparkModel(noTail), 20.5))),
    {s: 2, g: 20, drip: 0.5});
  assert.deepEqual(lines[4].slice(-12), ['0','0','','0.137','1.37','','1.001','10.01','10.00','1.138','11.38','10.00']);
  records[1].wAtMs=Array.from({length:1201},(_,i)=>i*50);
  records[1].wCg=records[1].wAtMs.map(t=>t/5);
  records[1].durationS=60;
  await vm.runInContext('exportShotsCsv()',context);
  const complete=(await blob.text()).split('\n')[2].split(',');
  assert.equal(complete[47],'0');
  assert.equal(complete[43],'2');
  assert.equal(complete.at(-3),'60');
  assert.equal(complete.at(-1),'2.00');
  assert.equal(complete.slice(54).filter((v,i)=>i%3===2&&v==='2.00').length,1181);
  for (const record of records) {
    record.wCg = Array(1201).fill(1234);
    record.wAtMs = Array.from({length:1201}, (_,i)=>i*50);
    record.wBreakBefore = Array.from({length:1200}, (_,i)=>i+1);
    record.wHeld = [7, 9];
    record.wTruncated = true;
  }
  await vm.runInContext('exportShotsCsv()', context);
  const maximum = (await blob.text()).split('\n').map(line=>line.split(','));
  assert.equal(maximum.length, 101);
  assert.equal(maximum[0].length, 3657);
  assert.equal(maximum[0].at(-3), 'sample_1201_time_s');
  for (const row of maximum.slice(1)) {
    assert.equal(row.length, maximum[0].length);
    assert.equal(row[47], '1');
    assert.equal(row[48].split(';').length, 1200);
    assert.equal(row[49], '7;9');
    assert.equal(row.at(-3), '60');
    assert.equal(row.at(-2), '12.34');
  }
})().catch(error => {console.error(error); process.exitCode = 1;});
// CSV cells: UTF-8 BOM, quoted quotes/commas/line breaks, and a leading
// apostrophe on text a spreadsheet would evaluate; numbers stay numbers.
(async () => {
  const assert = require('assert').strict, vm = require('vm');
  let saved;
  const context = vm.createContext({Blob, setTimeout: () => 0,
    URL: {createObjectURL: (blob) => { saved = blob; return 'blob:x'; }, revokeObjectURL() {}},
    document: {createElement: () => ({click() {}, remove() {}}), body: {append() {}}}});
  vm.runInContext('var R = this;' + blockAt(runtimeJs, 'function saveFile(') +
      blockAt(viewJs.records, 'function downloadCsv('), context);
  context.downloadCsv('x.csv', [['=1+1', '-3.5', '@x', 'a\r\nb', 'x,"y"', 5, -2, '+cmd', '']]);
  const bytes = Buffer.from(await saved.arrayBuffer());
  assert.deepEqual([...bytes.subarray(0, 3)], [0xef, 0xbb, 0xbf]);
  assert.equal(bytes.subarray(3).toString('utf8'), "'=1+1,-3.5,'@x,\"a\r\nb\",\"x,\"\"y\"\"\",5,-2,'+cmd,");
})().catch((error) => { console.error(error); process.exitCode = 1; });
