// Settings reads the owned socket: a standing `settings` subscription made in
// the background carries only the paths the Home frame lacks; the page reads
// both models, keeps its own across reconnects, and makes no REST read.
{
  const assert = require('assert').strict, vm = require('vm');
  const settingsSender = fs.readFileSync(
      path.join(sketchDir, 'network/OpenBrewByWeightSettingsStream.inc'), 'utf8');
  const uiStreamSource = fs.readFileSync(
      path.join(sketchDir, 'network/OpenBrewByWeightUiStream.inc'), 'utf8');
  const homeStreamSource = fs.readFileSync(
      path.join(sketchDir, 'network/OpenBrewByWeightHomeStream.inc'), 'utf8');
  const homeRegion = homeStreamSource.slice(homeStreamSource.indexOf('bool OpenBrewByWeightNetwork::sendHomeStream'),
      homeStreamSource.indexOf('bool OpenBrewByWeightNetwork::sendUiStreamFrame'));
  const homePaths = [...homeRegion.matchAll(/delta\.(?:hashed)?[fF]ield\("([^"]+)"/g)].map((m) => m[1]);
  const settingsPaths = [...settingsSender.matchAll(/fields\.field\("([^"]+)"/g)].map((m) => m[1]);
  assert(homePaths.length > 80 && settingsPaths.length > 60, 'stream path extraction failed');

  // Firmware: one bounded projection, gated, tagged, batched with Home.
  const slots = Number(networkHeader.match(/kSettingsFields = (\d+)/)?.[1]);
  assert(settingsPaths.length <= slots, `Settings slots ${settingsPaths.length} exceed kSettingsFields ${slots}`);
  assert.equal(new Set(settingsPaths).size, settingsPaths.length, 'each Settings path has one slot');
  assert(settingsSender.includes('HomeStreamDelta fields{hashes.settingsHashes, UiStreamFingerprints::kSettingsFields') &&
      settingsSender.includes('if (!initial && gate == session.settingsGate) return true;') &&
      settingsSender.includes('const bool kept = initial && tag == session.settingsKnownTag;') &&
      settingsSender.includes('"},\\"tag\\":%lu}"') &&
      settingsSender.includes('session.settingsResync = true;'),
  'the Settings sender must gate idle dispatches, send its tag, keep a matching model, and repair a deferred frame');
  const dispatch = uiStreamSource.slice(uiStreamSource.indexOf('void OpenBrewByWeightNetwork::sendUiStream('),
      uiStreamSource.indexOf('bool OpenBrewByWeightNetwork::sendShotCard('));
  const order = ['sendHomeStream(', 'sendShotCard(', 'sendSettingsStream(', 'sendDiagnosticStream(',
    'sendHistoryStream(', 'sendStatsStream(', 'sendLogStream('].map((name) => dispatch.indexOf(name));
  assert(order.every((at, i) => at > 0 && (!i || at > order[i - 1])),
      'dispatch order is home < card < settings < diagnostic < history < stats < log');
  assert(dispatch.indexOf('sendSettingsStream(') < dispatch.indexOf('uiStreamBatchEnd(') &&
      dispatch.includes('memcpy(workBuf_->batchSettingsHashes, hashes.settingsHashes') &&
      dispatch.includes('memcpy(hashes.settingsHashes, workBuf_->batchSettingsHashes'),
  'Settings rides the Home + shot-card batch and rolls back with it');
  const op = uiStreamSource.slice(uiStreamSource.indexOf('strcmp(op->valuestring, "settings")'),
      uiStreamSource.indexOf('strcmp(op->valuestring, "log")'));
  assert(op.includes('{"op", "on", "tag"}') && op.includes('uiStreamKnownFields(message, kFields, 3)') &&
      op.includes('cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "on"))') &&
      op.includes('uiStreamIntInRange(tag, 0, UINT32_MAX)') && op.includes('uiStreamUrgent_.store(true') &&
      op.includes('session->settingsOn = !firmwareCompatibilityMode();'),
  'the settings op takes an exact field whitelist, any tag the sender can emit, stays inert in compatibility mode, and arms a dispatch');
  assert(uiStreamSource.includes('session->settingsResync = true;\n      session->settingsKnownTag = 0;') &&
      uiStreamSource.includes('failed validation; the page resets its list views') &&
      uiStreamSource.includes('kUiStreamCloseRejected'),
  'resync always sends a snapshot; a malformed op is logged and closed with 4003');

  // One owner per path (Home > Settings > Diagnostic): no projection repeats,
  // or nests inside, another's paths. The only overlap is the compatibility
  // fallback: Diagnostic keeps four Micra slots that Settings owns and skips
  // their emission while the session holds the Settings subscription.
  const diagRegion = homeStreamSource.slice(homeStreamSource.indexOf('bool OpenBrewByWeightNetwork::sendDiagnosticStream'),
      homeStreamSource.indexOf('bool OpenBrewByWeightNetwork::sendLogStream'));
  const diagPaths = [...diagRegion.matchAll(/(?:delta|telemetry)\.field\("([^"]+)"/g)].map((m) => m[1]);
  const overlaps = (a, b) => a.filter((p) => b.some((q) => p === q || p.startsWith(q + '.') || q.startsWith(p + '.')));
  assert.deepEqual(overlaps(settingsPaths, homePaths), [], 'Home and Settings paths must be disjoint');
  assert.deepEqual(overlaps(diagPaths, homePaths), [], 'Diagnostic must not repeat Home paths');
  const shared = ['lineaMicra.phase', 'lineaMicra.email', 'lineaMicra.selectedSerial', 'lineaMicra.selectedName'];
  assert.deepEqual(overlaps(diagPaths, settingsPaths).sort(), [...shared].sort(),
      'Diagnostic shares only the compatibility-mode Micra paths with Settings');
  for (const p of shared)
    assert(new RegExp(`delta\\.skipNext = session\\.settingsOn;\\s*delta\\.field\\("${p.replace('.', '\\.')}"`).test(diagRegion),
        p + ' must be skipped by Diagnostic while Settings is subscribed');
  assert(codeIncludes(runtimeJs, 'mergeStatus(homeFrame?.status,diagFrame.status,settingsFrame?.status)'),
      'Diagnostic reads the shared Micra paths from Settings when it exists');
  // Every Settings path has a reader in the Settings view or the runtime it uses.
  const readers = (rawViewJs.settings + rawRuntimeJs).toLowerCase();
  const unread = settingsPaths.filter((p) => {
    const leaf = p.split('.').pop();
    return ![leaf, leaf.replace(/(Ms|Gesture)$/, '')].some((k) => readers.includes(k.toLowerCase()));
  });
  assert.deepEqual(unread, [], 'every Settings path needs a Web UI reader');

  // No REST Settings read survives anywhere in the tree.
  const retired = '/api/v1/status/' + 'settings';
  const scan = (dir) => fs.readdirSync(dir, {withFileTypes: true}).flatMap((entry) => {
    const file = path.join(dir, entry.name);
    if (entry.isDirectory()) return ['plans', 'audits', 'node_modules'].includes(entry.name) ? [] : scan(file);
    return /\.(js|cpp|h|inc|md|html|json|py|sh)$/.test(entry.name) && !entry.name.endsWith('Gzip.h') &&
        fs.readFileSync(file, 'utf8').includes(retired) ? [file] : [];
  });
  const repoRoot = path.resolve(sketchDir, '..');
  assert.deepEqual(['src', 'docs', 'scripts'].flatMap((dir) => scan(path.join(repoRoot, dir))), [],
      'nothing may reference the retired Settings status route');

  // Runtime: background subscribe, cached model across reconnects, composed reads.
  const source = runtimeStateDecl + runtimeJs.slice(runtimeJs.search(/let\s+homeFrame\s*=/),
      runtimeJs.indexOf('function setHomeSub(')).replace(/export /g, '');
  const validator = runtimeJs.slice(runtimeJs.indexOf('function statusPageOk('),
      runtimeJs.indexOf('async function loadStatus('));
  const sockets = [], painted = [];
  class Socket {
    static OPEN = 1;
    constructor() { this.readyState = 1; this.sent = []; sockets.push(this); }
    send(value) { this.sent.push(JSON.parse(value)); }
    close(code = 1006) { this.readyState = 3; this.onclose?.({code}); }
  }
  const context = {
    WebSocket: Socket, location: {protocol: 'http:', host: 'device.local'},
    document: {hidden: false, addEventListener() {}}, window: {addEventListener() {}},
    setTimeout: () => 0, clearTimeout() {}, requestAnimationFrame: (fn) => { fn(); return 0; },
    webUiPollingActive: () => true, webUiClientId: '0123456789abcdef', webUiPowerSeconds: () => 30,
    activeView: 'home', viewSeq: 1, viewReady: Promise.resolve(), lastStatusAt: 0, compatMode: false,
    SHOTS_PAGE_SIZE: 10, HISTORY_PAGE_SIZE: 20,
    performance: {now: () => 1}, statusUtcAnchorSec: 0, statusUtcAnchorAt: 0,
    viewStatusHandlers: {home() {}, settings: (s) => painted.push(s)},
    applyCommonStatus() {}, applyPairingPrompt() {}, setMutable() {}, noteReachOk() {}, noteReachFail() {},
    hideHomeBoot() {}, clearCupWeights() {}, updateHeaderSignals() {}, updateHomeAdminActions() {},
    setHomeSub() {}, ingestPresets() {}, deactivateWebUi() {}, $: () => null, console: {error() {}},
  };
  vm.runInNewContext(validator + source, context);
  const home = (boot, extra = {}) => ({v: 1, type: 'home', boot, snapshot: true, changes: {
    bootId: boot, configMutable: true, adminUnlocked: false, 'config.soundAlertsEnabled': true,
    'config.revision': 3, 'config.goalWeightG': 36, 'safety.state': 'READY', 'scale.available': true,
    presets: {activeId: 1, items: []}, presetsTag: 9, 'cycle.active': false,
    'noScaleShotGuard.enabled': false, machineState: 'IDLE', 'cupPresence.present': false, ...extra}});
  const settings = {v: 1, type: 'settings', boot: 7, snapshot: true, tag: 77, changes: {
    buzzerSupported: true, 'config.dripDelayMs': 3000, 'config.autoTare': true,
    'scale.history': [{mac: 'AA'}], 'scaleLoss.projectedS': 30}};
  const ops = (socket) => socket.sent.filter((m) => m.op === 'settings');

  context.startUiStream(); sockets[0].onopen();
  sockets[0].onmessage({data: JSON.stringify(home(7))});
  assert.equal(ops(sockets[0]).length, 0, 'nothing subscribes Settings before the first view settles');
  context.ensureBackgroundStreams(); context.ensureBackgroundStreams();
  assert.deepEqual(ops(sockets[0]), [{op: 'settings', on: true}], 'the background subscribe is sent once');
  sockets[0].onmessage({data: JSON.stringify(settings)});
  const composed = context.settingsStatus();
  assert.equal(composed.config.revision, 3, 'Home-owned config keys read from the Home model');
  assert.equal(composed.config.dripDelayMs, 3000, 'Settings-only keys read from the Settings model');
  assert.equal(JSON.stringify(composed.scale.history), '[{"mac":"AA"}]', 'arrays are never merged');
  assert.equal(composed.scale.available, true, 'objects both models hold merge key by key');
  // A patch updates only its path; the tag follows every frame.
  sockets[0].onmessage({data: JSON.stringify({type: 'settings', tag: 78, changes: {'config.autoTare': false}})});
  assert.equal(context.settingsStatus().config.autoTare, false);
  assert.equal(context.settingsStatus().config.dripDelayMs, 3000);
  // Home streams the effective Quick Settings, Settings the saved values they
  // hide; a new saved object replaces the old one whole.
  sockets[0].onmessage({data: JSON.stringify({type: 'home', changes: {
    'config.cupProtectionEnabled': false, 'quickRevert.overrides': 64}})});
  sockets[0].onmessage({data: JSON.stringify({type: 'settings', tag: 79, changes: {
    'quickRevert.saved': {cupProtectionEnabled: true}}})});
  const overridden = context.settingsStatus();
  assert(overridden.config.cupProtectionEnabled === false && overridden.quickRevert.overrides === 64 &&
      overridden.quickRevert.saved.cupProtectionEnabled === true, 'the header reads effective, the form saved');
  sockets[0].onmessage({data: JSON.stringify({type: 'settings', tag: 80, changes: {'quickRevert.saved': {}}})});
  assert.deepEqual(Object.keys(context.settingsStatus().quickRevert.saved), []);
  assert(settingsSender.includes('fields.field("quickRevert.saved", "{%s}", g_work->quickSaved);') &&
      settingsSender.includes('gate = gateOver(gate, g_work->quickSaved, strlen(g_work->quickSaved));') &&
      codeIncludes(runtimeJs, 'function loadSettingsConfig({config:c,quickRevert}){') &&
      codeIncludes(runtimeJs, 'c={...c,...quickRevert?.saved,doseG:active.doseG,') &&
      codeIncludes(viewJs.settings, 'R.loadSettingsConfig(s);') &&
      codeIncludes(viewJs.settings, 'saved=R.uiState.formBaseline?.[input.id]'),
  'the form shows the saved value an override hides, so Use saved setting sends it and drops the override');
  // A warm Settings entry paints the cached model without an op.
  vm.runInNewContext("activeView='settings'", context);
  const sentBefore = sockets[0].sent.length;
  assert.equal((await context.loadSettingsStatus()).config.dripDelayMs, 3000);
  assert.equal(sockets[0].sent.length, sentBefore, 'a warm entry sends no op');
  // Home frames repaint Settings unless they only move live weight.
  const paints = painted.length;
  sockets[0].onmessage({data: JSON.stringify({type: 'home', changes: {'scale.observedWeightG': 4.2}})});
  assert.equal(painted.length, paints, 'live weight alone does not repaint the Settings form');
  sockets[0].onmessage({data: JSON.stringify({type: 'home', changes: {'config.revision': 4}})});
  assert.equal(painted.length, paints + 1, 'a Home-owned setting repaints Settings');
  // A reconnect keeps the model and names it by tag; a frame without a tag resyncs.
  sockets[0].close();
  assert.equal(context.settingsStatus().config.autoTare, false, 'the model survives the socket');
  context.startUiStream(); sockets[1].onopen();
  assert.deepEqual(ops(sockets[1]), [{op: 'settings', on: true, tag: 80}], 'the replayed subscribe carries the tag');
  sockets[1].onmessage({data: JSON.stringify(home(7))});
  sockets[1].onmessage({data: JSON.stringify({type: 'settings', changes: {'config.autoTare': true}})});
  assert.equal(sockets[1].sent.at(-1).op, 'resync', 'a frame without its tag fails closed');
  // A new boot drops the old model; the next subscribe asks for a snapshot.
  sockets[1].onmessage({data: JSON.stringify(home(8))});
  assert.equal(context.settingsStatus(), null, 'a boot change clears the Settings model');
  sockets[1].close(); context.startUiStream(); sockets[2].onopen();
  assert.deepEqual(ops(sockets[2]), [{op: 'settings', on: true}]);
  // Compatibility mode has no Settings view and never subscribes.
  const compat = {...context, compatMode: true};
  vm.runInNewContext(validator + source, compat);
  compat.ensureBackgroundStreams(); compat.startUiStream(); sockets.at(-1).onopen();
  assert.equal(ops(sockets.at(-1)).length, 0, 'compatibility mode skips the Settings subscription');
  assert.equal(compat.settingsStatus(), null);

  // Settings never polls or reads REST; command results land on the streams.
  assert(!codeIncludes(rawRuntimeJs, "api('/api/v1/status/'+v)") &&
      codeIncludes(runtimeJs, "if(activeView==='settings')return scheduleSettingsPaint()") &&
      !codeIncludes(viewJs.settings, 'micraPoll') && !codeIncludes(viewJs.settings, 'dataset.applied'),
  'Settings repaints from the stream: no REST status, no poll, no 1 s Micra re-poll');
  // The form never rewrites the field being typed in, and "38" equals "38.0".
  assert(codeIncludes(runtimeJs, "if(configLoaded&&els.includes(focused)&&focused.type!=='checkbox')return focused.addEventListener('blur',scheduleSettingsPaint,{once:true})") &&
      codeIncludes(runtimeJs, "same=(a,b)=>a===b||(a!==''&&b!==''&&+a===+b)"),
  'Settings re-hydration must skip the focused field and compare numbers by value');
  assert(codeIncludes(blockAt(runtimeJs, 'registerStream("settings", {'), "scheduleSettingsPaint();//DiagnosticreadstheMicraaccountpathsfromthismodel.if(activeView==='diagnostic')scheduleDiagPaint();}"),
      'a Settings frame repaints Diagnostic, which reads the Micra account paths from it');
  assert(codeIncludes(viewJs.settings, "if(retry&&e.code==='CONFIG_REVISION_STALE'){for(let i=0;i<20&&R.homeStatusCache()?.config.revision===patch.baseRevision;i++)"),
      'only a stale-revision save waits for the newer streamed revision before its one retry');
}
