// Beanconqueror machine endpoints: the shared vectors' flow is the Stats
// page's own (buildShotSparkModel), so the controller's and the companion's
// ports draw what Stats draws, and the Settings preview decodes each served
// shot back to the row it came from.
(async () => {
  const assert = require('assert').strict;
  const vm = require('vm');
  const vectors = JSON.parse(fs.readFileSync(
      path.join(sketchDir, '..', 'docs', 'schemas', 'beanconqueror-v1.vectors.json'), 'utf8'));
  const context = vm.createContext({TextDecoder, DataView, Uint8Array});
  vm.runInContext(blockAt(runtimeJs, 'function buildShotSparkModel(') +
      blockAt(viewJs.settings, 'async function gaggiuinoPreview(') +
      blockAt(viewJs.settings, 'async function gaggimatePreview('), context);
  const view = (bytes) => new DataView(Uint8Array.from(bytes).buffer);
  const served = vectors.cases.filter((c) => c.served);
  assert.ok(served.length >= 6 && vectors.cases.some((c) => !c.served), 'Served and unserved cases');
  for (const c of served) {
    const s = c.shot;
    const flow = context.buildShotSparkModel(s)?.flowCurve ?? s.wCg.map(() => null);
    flow.forEach((v, i) => assert.ok(v === null ? c.flowGS[i] === null : Math.abs(v - c.flowGS[i]) < 1e-9,
        `${c.name}: flow ${i} is the Stats page's`));
    const g = await context.gaggiuinoPreview(async (p) =>
      (p.endsWith('/latest') ? [{lastShotId: s.id}] : c.gaggiuino.shot));
    assert.deepEqual([g.id, g.profile, g.tempC, g.points.length],
        [s.id, s.presetName, s.targetTempDeciC / 10, s.wCg.length - s.wHeld.length], c.name);
    const header = Buffer.alloc(32);
    header.writeUInt32LE(0x58444953, 0);
    header.writeUInt16LE(1, 4);
    header.writeUInt16LE(128, 6);
    header.writeUInt32LE(1, 8);
    const index = Buffer.concat([header, Buffer.from(c.gaggimate.indexEntry, 'base64')]);
    const m = await context.gaggimatePreview(async (p) =>
      (p.startsWith('/api/history/recent.bin') ? view(index)
        : p.endsWith('.slog') ? view(Buffer.from(c.gaggimate.slog, 'base64')) : c.gaggimate.notes));
    // The yield is the last reading's, as Beanconqueror takes it; the settled one without a curve.
    const last = s.wCg.filter((_, i) => !s.wHeld.includes(i)).at(-1);
    const yieldG = last === undefined ? s.actualG : Math.floor((Math.max(0, last) + 5) / 10) / 10;
    assert.deepEqual([g.yieldG, m.yieldG], [last === undefined ? null : yieldG, yieldG], c.name);
    assert.deepEqual([m.id, m.profile, m.timeS, m.rating, m.doseG, m.tempC, m.points.length],
        [s.id, s.presetName, s.durationS, s.rating, s.doseG, s.targetTempDeciC / 10,
          s.wCg.length - s.wHeld.length], c.name);
    // A format that is not served (404) is not an empty history.
    assert.equal(await context.gaggiuinoPreview(async () => null), undefined);
    assert.equal(await context.gaggimatePreview(async () => null), undefined);
  }

  // The Settings row: nothing is read while it is closed, before the
  // controller holds the choice, or during a shot; a format that is not
  // answered yet hides the preview and asks again; a failure does not.
  const dom = {}, asked = [];
  const text = (key) => errorCatalog['settings.beanconqueror_' + key];
  const el = (id) => (dom[id] ||= {textContent: '', hidden: false, dataset: {}, open: true,
    replaceChildren(...kids) { this.kids = kids; }, closest() { return dom.row; }});
  let saved = 'off', running = false, answer = () => null;
  const row = vm.createContext({$: el, location: {host: 'openbrew.local', hostname: 'openbrew.local'},
    JSON,
    document: {querySelectorAll: () => [], createElement: () => ({})},
    R: {controlsMutable: true, sub: (text, args) => text + ':' + args.join(','),
      companionHere: async () => null, shotRunning: () => running,
      settingsStatus: () => ({config: {beanconquerorEndpoint: saved}, network: {staIp: '192.168.1.9'}}),
      apiBinary: async (url) => { asked.push(url); return answer(url); }}});
  vm.runInContext(blockAt(viewJs.settings, 'const BC_FORMATS = {') + ';let bcShown = "";' +
      ['async function beanconquerorAddress(', 'async function renderBeanconqueror(',
        'async function loadBeanconquerorPreview(', 'async function gaggiuinoPreview(',
        'async function gaggimatePreview('].map((name) => blockAt(viewJs.settings, name)).join('') +
      'function renderBeanconquerorPreview(box) { box.drawn = true; }', row);
  const paint = async () => { await row.renderBeanconqueror(); await new Promise(setImmediate); };
  const refuse = (status) => () => { throw Object.assign(new Error('refused'), {status}); };
  el('row');
  el('beanconquerorEndpoint').value = 'gaggiuino';
  dom.row.open = false;
  saved = 'gaggiuino';
  await paint();
  assert.deepEqual([asked.length, dom.beanconquerorUrl], [0, undefined], 'A closed row draws nothing');
  dom.row.open = true;
  saved = 'off';
  await paint();
  assert.deepEqual([asked.length, dom.beanconquerorUrl.textContent, dom.beanconquerorAlt.textContent],
      [0, 'http://openbrew.local/beanconqueror',
        text('alt') + ':http://192.168.1.9/beanconqueror'], 'The address, before the save');
  saved = 'gaggiuino';
  running = true;
  await paint();
  assert.deepEqual([asked.length, dom.beanconquerorPreview.kids[0].textContent],
      [0, text('busy')], 'Nothing is read during a shot');
  running = false;
  answer = refuse(404);
  await paint();
  assert.deepEqual([asked, dom.beanconquerorPreview.hidden],
      [['/beanconqueror/api/shots/latest'], true], 'A format not answered yet hides the preview');
  answer = refuse(503);
  await paint();
  assert.deepEqual([asked.length, dom.beanconquerorPreview.kids[0].textContent],
      [2, text('busy')], 'Busy outside a shot');
  answer = refuse(500);
  await paint();
  await paint();
  assert.deepEqual([asked.length, dom.beanconquerorPreview.kids[0].textContent],
      [3, text('failed')], 'A failure is not asked again by itself');
  running = true;
  await paint();
  running = false;
  const shot = vectors.cases[0].gaggiuino.shot;
  answer = (url) => ({text: async () => JSON.stringify(url.endsWith('/latest') ? [{lastShotId: shot.id}] : shot)});
  await paint();
  await paint();
  assert.deepEqual([asked.slice(3), dom.beanconquerorPreview.drawn],
      [['/beanconqueror/api/shots/latest', '/beanconqueror/api/shots/' + shot.id], true],
      'The preview reads again after a shot, once');
})();
