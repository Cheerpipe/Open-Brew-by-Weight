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
  assert.ok(served.length >= 4 && vectors.cases.some((c) => !c.served), 'Served and unserved cases');
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
    assert.deepEqual([m.id, m.profile, m.timeS, m.yieldG, m.rating, m.doseG, m.tempC, m.points.length],
        [s.id, s.presetName, s.durationS, s.actualG, s.rating, s.doseG, s.targetTempDeciC / 10,
          s.wCg.length - s.wHeld.length], c.name);
  }
})();
