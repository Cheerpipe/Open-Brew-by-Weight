// Execute the UI formatters against historical disconnect and command records.
{
  const assert = require('assert').strict;
  const rows = {values: [], replaceChildren() {this.values = [];}, insertRow() {
    const cells = []; this.values.push(cells);
    return {insertCell: () => {const cell = {}; cells.push(cell); return cell;}};
  }};
  const elements = {scaleCommandRows: rows, scaleCommandTable: {}, scaleCommandHint: {}};
  const apply = new Function('$', blockAt(viewJs.diagnostic, 'function unchanged(') + blockAt(viewJs.diagnostic, 'function fillRows(') +
    blockAt(viewJs.diagnostic, 'function applyScaleCommands(') +
    ';return applyScaleCommands;')(id => elements[id]);
  const commands = ['Tare', 'Volume', 'Start timer', 'Stop timer', 'Reset timer',
    'Tare and start timer', 'Power off'].map((name, i) => ({name,
      code: ['0x01', '0x02', '0x04', '0x05', '0x06', '0x07', '0x15'][i]}));
  apply({model: 'bookoo_ultra', supportedCommandsKnown: true, supportedCommands: commands});
  assert.equal(rows.values.length, 7); assert(!elements.scaleCommandTable.hidden);
  assert.equal(rows.values[6][1].textContent, '0x15');
  assert(elements.scaleCommandHint.textContent.includes('charging'));
  apply({model: 'bookoo_mini', supportedCommandsKnown: true, supportedCommands: commands.slice(0, 6)});
  assert.equal(rows.values.length, 6); assert.equal(elements.scaleCommandHint.textContent, '');
  apply({model: 'unknown', supportedCommandsKnown: false, supportedCommands: []});
  assert(elements.scaleCommandTable.hidden); assert.equal(rows.values.length, 0);
  assert(elements.scaleCommandHint.textContent.includes('unknown'));
}
{
  const source = runtimeJs;
  const first = source.indexOf('function formatScaleStatus(');
  const last = source.indexOf('function formatScaleTimer(', first);
  const elements = {cupWeight: {textContent: 'old'}, dCupWeight: {textContent: 'old'}, idleTareStatus: {textContent: 'old'},
    cupAccessory: {textContent: 'old'}};
  const helpers = new Function('$', 'sub', source.slice(first, last) +
    ';return {formatCupWeight,clearCupWeights,formatIdleTare,formatCupAccessory};')(id => elements[id],
    (t, vals) => t.replace(/\{(\d)\}/g, (_, i) => vals[+i]));
  const good = {scale: {available: true, streamState: 'FRESH'},
    cupPresence: {present: true, weightValid: true, weightG: 80, placementId: 1}};
  if (helpers.formatCupWeight(good) !== '≈ 80.0 g') throw new Error('Cup weight format');
  for (const weight of [null, undefined, NaN, Infinity, '80']) {
    if (helpers.formatCupWeight({...good, cupPresence: {...good.cupPresence, weightG: weight}}) !== '—')
      throw new Error('Invalid cup weight must be unavailable');
  }
  if (helpers.formatCupWeight({...good, cupPresence: {present: false}}) !== 'Absent') throw new Error('Absent cup');
  for (const status of [{}, {...good, cupPresence: {}},
    {...good, cupPresence: {...good.cupPresence, present: false}, scale: {available: true, streamState: 'STALE'}},
    {...good, cupPresence: {...good.cupPresence, weightValid: false}},
    {...good, scale: {available: false, streamState: 'FRESH'}},
    {...good, scale: {available: true, streamState: 'STALE'}}]) {
    if (helpers.formatCupWeight(status) !== '—') throw new Error('Unavailable cup provenance');
  }
  for (const [accessory, accessoryG, expected] of [
    ['present', 42.2, '42.2 g'], ['absent', null, 'Absent'], ['settling', null, 'Settling…'],
    ['not_tracked', null, 'Not tracked - lift the cup and set it down again'], ['off', null, 'Off'],
    ['present', null, 'Unknown']]) {
    if (helpers.formatCupAccessory({...good, cupPresence: {accessory, accessoryG}}) !== expected)
      throw new Error('Accessory state: ' + expected);
  }
  if (helpers.formatCupAccessory({...good, scale: {available: true, streamState: 'STALE'},
    cupPresence: {accessory: 'present', accessoryG: 42}}) !== 'Unknown') throw new Error('Stale accessory');
  helpers.clearCupWeights();
  if (Object.values(elements).some(el => el.textContent !== '—')) throw new Error('Stale cup UI');
  for (const [state, expected] of [
    ['empty', 'Empty scale must settle'],
    ['ready', 'Ready for a cup'],
    ['pending', 'Taring — waiting for zero'],
    ['machine_not_off', 'Waiting for machine off'],
    ['disabled', 'Off'],
    ['uncertain', 'Tare the empty scale, then reconnect it'],
  ]) {
    const s = {...good, cupPresence: {present: false, idleTare: state}};
    if (helpers.formatIdleTare(s) !== expected) throw new Error('Idle tare readiness: ' + expected);
  }
  for (const [delta, expected, waitS] of [
    [52, 'Empty scale must settle (reads 52.0 g above empty)'],
    [-12.04, 'Empty scale must settle (reads 12.0 g below empty)'],
    [0.2, 'Empty scale must settle'], [null, 'Empty scale must settle'],
    [52, 'Empty scale must settle (reads 52.0 g above empty for 4 s)', 4],
    [-12.04, 'Empty scale must settle (reads 12.0 g below empty for 61 s)', 61],
    [0.2, 'Empty scale must settle', 9],
  ]) {
    const s = {...good, cupPresence: {present: false, idleTare: 'empty', anchorDeltaG: delta, anchorWaitS: waitS}};
    if (helpers.formatIdleTare(s) !== expected) throw new Error('Idle tare empty offset: ' + expected);
  }
  if (!network.includes('delta.field("cupPresence.anchorDeltaG"') ||
      !network.includes('delta.field("cupPresence.anchorWaitS"')) {
    throw new Error('Home cup delta must carry the empty-reference offset and wait');
  }
  for (const reason of ['remove', 'retry']) {
    const s = {...good, cupPresence: {present: true, idleTare: reason}};
    const text = helpers.formatIdleTare(s);
    if (!text.toLowerCase().includes('remove the cup')) throw new Error('Idle tare recovery');
  }
  if (helpers.formatIdleTare(good) !== '—') throw new Error('Older firmware idle tare fallback');
  for (const [available, streamState, expected] of [[false, 'FRESH', 'Disconnected'],
    [true, 'STALE', 'Stale'], [true, 'NO_SAMPLE', 'No sample']]) {
    const s = {scale: {available, streamState}, cupPresence: {idleTare: 'ready'}};
    if (helpers.formatIdleTare(s) !== expected) throw new Error('Idle tare must show stream health: ' + expected);
  }
  const home = partialHtml.home;
  const diagnostic = partialHtml.diagnostic;
  const cup = home.match(/<details id="cupRow" class="lampRow">([\s\S]*?)<\/details>/);
  if (!cup || !cup[1].includes('id="cupState"') || !cup[1].includes('id="cupWeight"') || !cup[1].includes('id="idleTareStatus"') ||
      !cup[1].includes('id="cupAccessory"') ||
      !/<details id="scaleRow" class="lampRow">[\s\S]*?<\/details><details id="cupRow" class="lampRow">/.test(home) ||
      !/<summary>Scale<\/summary>[\s\S]*?id="dCupWeight"/.test(diagnostic) ||
      !diagnostic.includes('id="dCup"') || diagnostic.includes('id="scaleTareButton"') ||
      !codeIncludes(source, "setHomeSub('cupWeight',formatCupWeight(s))") ||
      !codeIncludes(viewJs.diagnostic, "t('dCupWeight',R.formatCupWeight(s))") ||
      !codeIncludes(source, 'function noteReachFail(err,force){clearCupWeights();') ||
      !codeIncludes(source, 'function stopViewPolls(){clearCupWeights();')) {
    throw new Error('Home and Diagnostic cup panel contract');
  }
  // Parse the actual adjacent C++ format literals for both status paths.
  const status = fs.readFileSync(path.join(sketchDir, 'network/OpenBrewByWeightStatus.inc'), 'utf8');
  // The shared C++ projection is exercised natively by scale_profiler_host_test.
  // Keep the Web-specific legacy labels and shared projection binding explicit.
  if (!network.includes('idleTarePresentationCode(tare, control.cupPresent)') ||
      !network.includes('idleCode == IDLE_WAITING_FOR_SETTLE ? "empty"') ||
      !network.includes('idleCode == IDLE_READY_FOR_CUP ? "ready"') ||
      !network.includes('idleTarePresentationName(idleCode)')) {
    throw new Error('Idle tare Web projection must preserve its public labels');
  }
  const homeStream = fs.readFileSync(
      path.join(sketchDir, 'network/OpenBrewByWeightHomeStream.inc'), 'utf8');
  const diagSender = homeStream.slice(
      homeStream.indexOf('bool OpenBrewByWeightNetwork::sendDiagnosticStream'),
      homeStream.indexOf('bool OpenBrewByWeightNetwork::sendLogStream'));
  for (const field of ['delta.field("cupPresence.state"',
                       'delta.field("cupPresence.present"',
                       'delta.field("cupPresence.weightG"',
                       'delta.field("cupPresence.weightValid"']) {
    // Home owns the cup state; the Diagnostic page reads it from the Home model.
    if (diagSender.includes(field) || !network.includes(field)) {
      throw new Error('Home cup deltas required, without a Diagnostic copy: ' + field);
    }
  }
  if (!network.includes('const bool cupWeightValid = control.cupPresent')) {
    throw new Error('Cup weight gating must stay in the Home projection');
  }
}

{
  // Historical disconnect/command summaries render through the shared live
  // renderer (diagnostic.js applyDiagnosticLive), shared with the socket stream.
  const runtimeSource = viewJs.diagnostic;
  const first = runtimeSource.indexOf('function applyDiagnosticLive(');
  const last = runtimeSource.indexOf('\n}', first) + 2;
  if (first < 0 || last < first) throw new Error('Missing scale diagnostic formatters');
  const elements = {};
  const live = new Function('$', '__WEBUI_TEXT__', 'R', 'renderLineaMicraDiagnostic',
    runtimeSource.slice(first, last) + ';return applyDiagnosticLive;')(
    id => elements[id] || (elements[id] = {textContent: ''}),
    key => ({'diagnostic.none_2': 'NONE', 'diagnostic.none_3': 'none'}[key] || key),
    {formatBackflushState: () => '', formatCupWeight: () => '', formatScaleWeight: () => '',
      formatScaleTimer: () => '', updateScaleRenameUi() {}}, () => {});
  const record = lastDisconnect => live({lineaMicra: null, safety: {}, scale: {
    lastDisconnect, lastDisconnectReasonName: lastDisconnect ? undefined : 'NONE',
    lastCommandFailure: {sequence: 1, summary: 'volume · status 15 · 0ms'},
    cupPresence: {}, backflush: {}}, cupPresence: {}});
  record({summary: 'SUPERVISION_TIMEOUT · gap · hci 520 · 5s ago'});
  const historical = elements.hLastDisconnect.textContent;
  for (const expected of ['SUPERVISION_TIMEOUT', 'gap', 'hci 520', '5s ago']) {
    if (!historical.includes(expected)) throw new Error('Missing disconnect detail: ' + expected);
  }
  record(null);
  if (elements.hLastDisconnect.textContent !== 'NONE') {
    throw new Error('Legacy scale status compatibility failed');
  }
  if (elements.hScaleCommandFailure.textContent !== 'volume · status 15 · 0ms')
    throw new Error('Command failure must be separate from disconnect');
  live({lineaMicra: null, safety: {}, scale: {lastCommandFailure: {sequence: 0}, cupPresence: {}}, cupPresence: {}});
  if (elements.hScaleCommandFailure.textContent !== 'none') throw new Error('Missing-event rendering');
}
