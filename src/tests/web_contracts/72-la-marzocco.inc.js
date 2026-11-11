const laMarzoccoWeb = fs.readFileSync(
    path.join(sketchDir, 'network/OpenBrewByWeightLaMarzoccoWeb.inc'), 'utf8');
const laMarzoccoStatusSource = fs.readFileSync(
    path.join(sketchDir, 'network/OpenBrewByWeightStatus.inc'), 'utf8') + fs.readFileSync(
    path.join(sketchDir, 'network/OpenBrewByWeightSettingsStream.inc'), 'utf8') +
    (fs.readFileSync(path.join(sketchDir, 'network/OpenBrewByWeightHomeStream.inc'), 'utf8') +
      fs.readFileSync(path.join(sketchDir, 'network/OpenBrewByWeightRecordStreams.inc'), 'utf8'));
const laMarzoccoService = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightLaMarzoccoService.cpp'), 'utf8');
const laMarzoccoTiming = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightLaMarzoccoTiming.h'), 'utf8');
const laMarzoccoTypes = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightLaMarzoccoTypes.h'), 'utf8');
const machineIntegration = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightLaMarzoccoIntegration.cpp'), 'utf8');
const controlCommands = fs.readFileSync(
    path.join(sketchDir, 'control/OpenBrewByWeightCommands.inc'), 'utf8');
const commandPersistence = fs.readFileSync(
    path.join(sketchDir, 'persistence/OpenBrewByWeightCommandPersistence.inc'), 'utf8');
const scaleEvents = fs.readFileSync(
    path.join(sketchDir, 'scale/OpenBrewByWeightScaleEvents.inc'), 'utf8');
const networkService = fs.readFileSync(
    path.join(sketchDir, 'network/OpenBrewByWeightNetworkService.inc'), 'utf8');
const laMarzoccoSettingsHtml = rawPartialHtml.settings;
const laMarzoccoDiagnosticHtml = rawPartialHtml.diagnostic;
{
  const assert = require('assert'), vm = require('vm');
  assert(rawPartialHtml.home.includes('<div class="metric laMarzoccoOnly"><strong>{{webui:home.machine_power_state}}</strong><div id="homeLaMarzoccoPower">'));
  const el = () => ({textContent: '', classList: {toggle() {}, contains: () => false}});
  const cleaning = el(), power = el(), rowState = el(), brew = el();
  const brewBoiler = el(), steamBoiler = el();
  const machineRow = {textContent: '', bad: false};
  machineRow.classList = {toggle: (_, on) => { machineRow.bad = on; }};
  let lastStatus = null;
  const rowSlots = {};
  const context = {R: {applyHomeStatus() {}, formatLaMarzoccoCleaning:()=>'', deviceUtcSec: () => 60,
      homeStatusCache: () => lastStatus, shotRecorded: () => null, uiState: {companionHere: null},
      companionPairable: () => false, companionHere: () => Promise.resolve(null),
      $: id => ({homeLaMarzoccoCleaning: cleaning, homeLaMarzoccoPower: power, machineRowState: rowState, state: brew, machineRow,
        homeLaMarzoccoBrewBoiler: brewBoiler, homeLaMarzoccoSteamBoiler: steamBoiler}[id] ||
        (/Row(Value|Hint)$|^(scaleWeight|preferredScale|cupWeight)$/.test(id) ? (rowSlots[id] ||= el()) : null))},
    document: {querySelector: () => null}, __LA_MARZOCCO__: true,
    Date: {now: () => 60000},
    __WEBUI_TEXT__: key => key === 'home.optimistic' ? 'Optimistic' : 'Unknown'};
  vm.runInNewContext(viewJs.home.replace(/import\s*\*\s*as\s+R\s+from\s*['"][^'"]+['"];?/, '').replace(/export /g, ''), context);
  // The boiler ticker reads the live cache; mirror what the stream apply does.
  const directApply = context.applyStatus;
  context.applyStatus = data => { lastStatus = data; directApply(data); };
  for (const [laMarzocco, expected] of [
    [{powerState: 'ON', quality: 'current'}, 'ON'],
    [{powerState: 'OFF', quality: 'current'}, 'OFF'],
    [{powerState: 'ON', quality: 'stale', optimisticOff: true}, 'ON'],
    [{powerState: 'OFF', quality: 'stale', optimisticOn: true}, 'OFF'],
    [{powerState: 'OFF', quality: 'optimistic', optimisticOn: true}, 'ON - Optimistic'],
    [{powerState: 'ON', quality: 'optimistic', optimisticOff: true}, 'OFF - Optimistic'],
    [{powerState: 'ON', quality: 'current', optimisticOff: true}, 'ON'],
    [{powerState: 'OFF', quality: 'current', optimisticOn: true}, 'OFF'],
    [{powerState: 'UNKNOWN'}, '—'],
    [undefined, '—'],
  ]) {
    context.applyStatus({laMarzocco});
    assert.strictEqual(power.textContent, expected);
  }
  context.applyStatus({laMarzocco: {powerState: 'OFF', quality: 'current'}, state: 'READY'});
  assert.strictEqual(rowState.textContent, 'Turned off');
  assert.strictEqual(machineRow.bad, true);
  context.applyStatus({laMarzocco: {powerState: 'ON', quality: 'current'}, state: 'READY'});
  assert.strictEqual(rowState.textContent, 'Ready');
  assert.strictEqual(machineRow.bad, false);
  context.applyStatus({laMarzocco: {powerState: 'ON', quality: 'current'}, state: 'BREWING'});
  assert.strictEqual(machineRow.bad, true);
  assert.strictEqual(brew.textContent, '');
  // Boiler-readiness lamp: with boiler evidence the machine row claims Ready
  // only when both boilers are ready; otherwise it narrates the warm-up.
  const warming = {machineIntegration: 'la_marzocco_cloud', state: 'READY',
    laMarzocco: {accountConfigured: true, observeState: true, powerState: 'ON', quality: 'current'}};
  for (const [readiness, expected, bad] of [
    ['warming_up', 'Heating up', false],
    ['waiting_for_steam', 'Steam heating up', false],
    ['needs_water', 'No water', true],
    ['ready', 'Ready', false],
    ['unknown', 'On', false],
  ]) {
    context.applyStatus({...warming, laMarzocco: {...warming.laMarzocco, readiness}});
    assert.strictEqual(rowState.textContent, expected);
    assert.strictEqual(machineRow.bad, bad);
  }
  // Headline value: minutes left while warming, brew target once ready.
  const boilers = (readiness, coffee) => ({...warming, laMarzocco: {...warming.laMarzocco, readiness, boiler: {coffee}}});
  context.applyStatus(boilers('warming_up', {state: 'heating_up', readyAtUtcSec: 1000, targetDeciC: 930}));
  assert.deepStrictEqual([rowState.textContent, rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent],
      ['Heating up', '16 min', 'Until ready']);
  context.applyStatus(boilers('ready', {state: 'ready', targetDeciC: 930}));
  assert.deepStrictEqual([rowState.textContent, rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent],
      ['Ready', '93°C', 'Brew temperature']);
  context.applyStatus({...warming, laMarzocco: {...warming.laMarzocco, powerState: 'OFF'}});
  assert.deepStrictEqual([rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent], ['—', 'Turn on to brew']);
  context.applyStatus(boilers('needs_water', {state: 'no_water'}));
  assert.deepStrictEqual([rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent], ['—', 'Refill the tank']);
  context.applyStatus({...warming, laMarzocco: {...warming.laMarzocco, powerState: 'OFF', quality: 'optimistic', optimisticOn: true, readiness: 'unknown'}});
  assert.deepStrictEqual([rowState.textContent, rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent], ['On', '—', 'Starting up']);
  context.applyStatus(boilers('unknown', {state: 'standby'}));
  assert.deepStrictEqual([rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent], ['—', '']);
  // Without the integration there is never a reading, so no dash either.
  context.applyStatus({laMarzocco: {powerState: 'OFF', quality: 'current'}, state: 'READY'});
  assert.strictEqual(rowSlots.machineRowValue.textContent, '');
  // Home boiler rows: estimate math (Date.now pinned to 60000 ms),
  // clamping, labels, and configuration gating.
  const frame = (coffee, steam) => ({machineIntegration: 'la_marzocco_cloud', state: 'READY',
    laMarzocco: {accountConfigured: true, observeState: true, powerState: 'ON', quality: 'current',
      boiler: {coffee, steam}}});
  context.applyStatus(frame({state: 'heating_up', readyAtUtcSec: 1000},
      {state: 'heating_up', readyAtUtcSec: 1200, level: 'level_3'}));
  assert.strictEqual(brewBoiler.textContent, 'Heating up · ready in ~16 min');
  assert.strictEqual(steamBoiler.textContent, 'Heating up · ready in ~19 min');
  // A steam boiler set by temperature reports no level: the row needs none.
  context.applyStatus(frame({state: 'ready'},
      {state: 'heating_up', readyAtUtcSec: 1200, level: 'unknown'}));
  assert.strictEqual(steamBoiler.textContent, 'Heating up · ready in ~19 min');
  context.applyStatus(frame({state: 'heating_up', readyAtUtcSec: 10}, {state: 'heating_up'}));
  assert.strictEqual(brewBoiler.textContent, 'Heating up');  // Expired estimate: no countdown.
  assert.strictEqual(steamBoiler.textContent, 'Heating up');  // No estimate given.
  context.applyStatus(frame({state: 'ready'}, {state: 'no_water'}));
  assert.strictEqual(brewBoiler.textContent, 'Ready');
  assert.strictEqual(steamBoiler.textContent, 'No water');
  context.applyStatus(frame({state: 'eco'}, {state: 'standby'}));
  assert.strictEqual(brewBoiler.textContent, 'Eco');
  assert.strictEqual(steamBoiler.textContent, 'Standby');
  context.applyStatus(frame({state: 'off'}, {state: 'unknown'}));
  assert.strictEqual(brewBoiler.textContent, 'Off');
  assert.strictEqual(steamBoiler.textContent, '—');
  context.applyStatus({laMarzocco: {accountConfigured: false, observeState: false}});
  assert.strictEqual(brewBoiler.textContent, 'Not connected');
  context.applyStatus({laMarzocco: {accountConfigured: true, observeState: false}});
  assert.strictEqual(brewBoiler.textContent, 'Disabled');
  // Without the integration the controller state decides, as before.
  context.applyStatus({laMarzocco: {powerState: 'ON', quality: 'current'}, state: 'READY'});
  assert.strictEqual(rowState.textContent, 'Ready');
  assert(network.includes('delta.field("laMarzocco.powerState"'),
      'Home stream must project Micra power independently of REST status');
  // Cross-side contract: every firmware-emitted stream path must pass the
  // client delta gate (regression: 4-segment boiler paths were rejected and
  // killed the live snapshot on Micra builds).
  const gateSource = rawRuntimeJs.match(
      /\/(\^\[a-zA-Z\]\[a-zA-Z0-9\]\*\(\\\.\[a-zA-Z\]\[a-zA-Z0-9\]\*\)\{0,\d\}\$)\/\.test\(path\)/);
  assert(gateSource, 'client delta path gate must exist');
  const gate = new RegExp(gateSource[1]);
  const streamPaths = [...laMarzoccoStatusSource.matchAll(/(?:delta|fields)\.field\("([^"]+)"/g)].map(m => m[1]);
  assert(streamPaths.length > 30, 'stream field extraction found no paths');
  for (const path of streamPaths)
    assert(gate.test(path), `firmware field path rejected by the client gate: ${path}`);
}
{
  const assert = require('assert');
  const vm = require('vm');
  const panelAt = laMarzoccoDiagnosticHtml.indexOf('id="laMarzoccoCloudDiagnostics"');
  assert(panelAt >= 0 && panelAt < laMarzoccoDiagnosticHtml.indexOf('{{webui:diagnostic.misc}}'));
  assert(/<details class="cfgGroup laMarzoccoOnly" data-g="marzocco-cloud"><summary>[^<]*<\/summary><div id="laMarzoccoCloudDiagnostics" class="statusColumn">/.test(laMarzoccoDiagnosticHtml));
  const ids = ['dLaMarzoccoCloudEmail', 'dLaMarzoccoCloudMachine', 'dLaMarzoccoCloudTime',
    'dLaMarzoccoCloudApi', 'dLaMarzoccoCloudResult', 'dLaMarzoccoCloudDuration'];
  const nodes = Object.fromEntries(ids.map(id => [id, {textContent: ''}]));
  const cloudUi = fs.readFileSync(path.join(sketchDir, 'web/js/diagnostic.js'), 'utf8');
  assert(cloudUi.includes('function ensureLaMarzoccoRows()'));
  // Exercise row creation with a minimal DOM, including repeated status updates.
  const dom = {};
  const element = () => ({children: [], textContent: '',
    set id(value) { this._id = value; dom[value] = this; }, get id() { return this._id; },
    append(...children) { for (const child of children) { child.parentElement = this; this.children.push(child); } },
    insertBefore(child, anchor) { child.parentElement = this; const at = this.children.indexOf(anchor); this.children.splice(at < 0 ? this.children.length : at, 0, child); }});
  const state = element(), modeRow = element(), mode = element(), cloud = element(), heapRow = element(), heap = element();
  mode.id = 'dLaMarzoccoMode'; cloud.id = 'laMarzoccoCloudDiagnostics'; heap.id = 'hHeapLargest';
  state.append(modeRow); modeRow.append(mode); heapRow.append(heap);
  // The cleaning row is static markup inside the machine-state fieldset; the
  // synthetic DOM mirrors the generated diagnostic page.
  const machine = element(), cleaningRow = element(), cleaning = element(), cleaningHint = element();
  cleaning.id = 'dLaMarzoccoCleaning'; cleaningHint.id = 'dLaMarzoccoCleaningHint';
  machine.append(cleaningRow); cleaningRow.append(cleaning, cleaningHint);
  assert(/<details class="cfgGroup laMarzoccoOnly" data-g="marzocco-micra"><summary>[^<]*<\/summary><div id="laMarzoccoMachineDiagnostics" class="statusColumn">/.test(laMarzoccoDiagnosticHtml));
  assert(laMarzoccoDiagnosticHtml.includes('{{webui:diagnostic.la_marzocco}}'));
  for (const id of ['dLaMarzoccoReadiness', 'dLaMarzoccoBrewBoiler', 'dLaMarzoccoSteamBoiler', 'dLaMarzoccoWater']) {
    assert(laMarzoccoDiagnosticHtml.includes(`id="${id}"`), `machine section row missing: ${id}`);
  }
  const labels = {'diagnostic.cloud_titles': 'Email|Machine|Time|API|Result|Duration|Connection|Traffic|Planned|Unexpected|Cleaning',
    'diagnostic.cleaning_states': 'Inactive|Waiting for paddle|Cleaning', 'diagnostic.ws_links': 'live|closed',
    // Unit and separator fragments read as the English catalog has them.
    ...Object.fromEntries(['runtime.s_2', 'runtime.ms_2', 'diagnostic.utc_suffix', 'diagnostic.http_prefix',
      'diagnostic.retry_prefix', 'diagnostic.ms_stop', 'diagnostic.bytes_per_s', 'diagnostic.bytes_per_min',
      'diagnostic.bytes_suffix', 'diagnostic.messages_sep', 'diagnostic.errors_suffix',
      'diagnostic.websocket_suffix', 'diagnostic.ws_prefix', 'diagnostic.psram_heap', 'diagnostic.failures_sep']
      .map((key) => [key, localeReference.strings[key]]))};
  const domContext = vm.createContext({$: id => dom[id], document: {createElement: element},
    __WEBUI_TEXT__: key => labels[key] || key, R: {formatWallTimeLocal: String}});
  const cleaningSource = rawRuntimeJs.slice(rawRuntimeJs.indexOf('function formatLaMarzoccoCleaning('),
      rawRuntimeJs.search(/let\s+shotWs\s*=/)).replace('export ', '');
  domContext.R.formatLaMarzoccoCleaning = new Function('__WEBUI_TEXT__', cleaningSource + ';return formatLaMarzoccoCleaning;')(domContext.__WEBUI_TEXT__);
  vm.runInContext(cloudUi.slice(cloudUi.indexOf('function renderLaMarzoccoCloudDiagnostic('),
      cloudUi.indexOf('function applyScaleCommands(')), domContext);
  const show = (lm, via = false) => {
    Object.assign(domContext, {lm, via});
    vm.runInContext('renderLaMarzoccoCloudDiagnostic(lm, via)', domContext);
  };
  const socket = {state: 'streaming', live: true, nowMs: 5000, cleaningAvailable: true, cleaning: 'waiting_for_paddle', cleaningAtMs: 3000,
    rxBytes: 120, rxBytesPerSecond: 20, rxBytesPerMinute: 100, messages: 2,
    plannedConnections: 7, unexpectedConnections: 3};
  const linked = {accountConfigured: true, observeState: true, connectionType: 'websocket', websocket: socket};
  show(linked); show(linked);
  assert.strictEqual(cloud.children.length, 10, 'Status refresh must not duplicate dynamic rows');
  assert.strictEqual(dom.dLaMarzoccoWsPlanned.textContent, '7');
  assert.strictEqual(dom.dLaMarzoccoWsUnexpected.textContent, '3');
  assert.strictEqual(machine.children.length, 1, 'Cleaning stays a static row of the machine section');
  assert.strictEqual(dom.dLaMarzoccoCleaning.textContent, 'Waiting for paddle');
  assert(dom.dLaMarzoccoCleaningHint.textContent.includes('2.0 s'));
  assert(!dom.dLaMarzoccoCleaningHint.textContent.includes('diagnostic.stale'));
  assert(dom.dLaMarzoccoWsTraffic.textContent.includes('RX 20 B/s · 100 B/60s · 120 B'));
  assert.strictEqual(dom.dLaMarzoccoWsState.textContent, 'streaming · live');
  show({...linked, websocket: {...socket, live: false, state: 'paused', reason: 'maintenance'}});
  assert.strictEqual(dom.dLaMarzoccoWsState.textContent, 'paused · maintenance · closed');
  // On the companion route the row names its source, drops the controller's
  // own socket figures, and cleaning stays live while it is supervised, even
  // while the feed waits for its first push.
  show({...linked, websocket: {...socket, state: 'waiting_for_data'}}, true);
  assert.strictEqual(dom.dLaMarzoccoWsState.textContent, 'diagnostic.ws_via_companion · live');
  assert(!dom.dLaMarzoccoWsTiming.textContent.includes('pong') && dom.dLaMarzoccoWsTraffic.parentElement.hidden &&
      dom.dLaMarzoccoWsHeap.hidden);
  assert(!dom.dLaMarzoccoCleaningHint.textContent.includes('diagnostic.stale'));
  show(linked);
  assert(!dom.dLaMarzoccoWsTraffic.parentElement.hidden && !dom.dLaMarzoccoWsHeap.hidden);
  show({...linked, websocket: {...socket, live: false, state: 'paused', cleaning: 'unknown', cleaningLabel: '<b>Future</b>'}});
  assert.strictEqual(dom.dLaMarzoccoCleaning.textContent, 'runtime.unknown_4 · <b>Future</b>');
  assert(dom.dLaMarzoccoCleaningHint.textContent.includes('diagnostic.stale'));
  show({...linked, connectionType: 'api'});
  assert.strictEqual(dom.dLaMarzoccoCleaning.textContent, 'diagnostic.cleaning_api');
  assert.strictEqual(dom.dLaMarzoccoCleaningHint.textContent, '');
  show({...linked, observeState: false});
  assert.strictEqual(dom.dLaMarzoccoCleaning.textContent, 'runtime.disabled');
  assert.strictEqual(dom.dLaMarzoccoCleaningHint.textContent, '');
  show({...linked, accountConfigured: false});
  assert.strictEqual(dom.dLaMarzoccoCleaning.textContent, 'runtime.not_connected');
  assert.strictEqual(dom.dLaMarzoccoCleaningHint.textContent, '');
  show({...linked, websocket: {}});
  assert.strictEqual(dom.dLaMarzoccoWsPlanned.textContent, '0');
  assert.strictEqual(dom.dLaMarzoccoWsUnexpected.textContent, '0');
  assert.strictEqual(dom.dLaMarzoccoCleaning.textContent, 'diagnostic.cleaning_no_update');
  assert(!dom.dLaMarzoccoWsHeap.textContent.includes('undefined'));
  assert(dom.dLaMarzoccoWsHeap.textContent.includes('internal Δ'));
  const context = vm.createContext({$: id => nodes[id],
    __WEBUI_TEXT__: key => key === 'diagnostic.cloud_results' ? ['success','canceled','http_error','transport_error','invalid_response','response_too_large','setup_error'].map(k=>'diagnostic.cloud_'+k).join('|') : labels[key] || key,
    R: {formatWallTimeLocal: String}});
  context.R.formatLaMarzoccoCleaning = new Function('__WEBUI_TEXT__', cleaningSource + ';return formatLaMarzoccoCleaning;')(context.__WEBUI_TEXT__);
  vm.runInContext(cloudUi.slice(cloudUi.indexOf('function renderLaMarzoccoCloudDiagnostic('),
      cloudUi.indexOf('function applyScaleCommands(')), context);
  const render = data => {
    context.lm = data;
    vm.runInContext('renderLaMarzoccoCloudDiagnostic(lm)', context);
  };
  render({});
  assert.strictEqual(nodes.dLaMarzoccoCloudEmail.textContent, 'diagnostic.cloud_no_account');
  assert.strictEqual(nodes.dLaMarzoccoCloudMachine.textContent, 'diagnostic.cloud_no_machine');
  assert.strictEqual(nodes.dLaMarzoccoCloudTime.textContent, 'diagnostic.cloud_no_call');
  const selected = {accountConfigured: true, email: 'barista@example.test',
    selectedName: '<b>Kitchen Micra</b>', selectedSerial: 'ABC'};
  const call = {api: 'read_dashboard', method: 'GET', startedAtUtcSec: 1790726400,
    result: 'success', durationMs: 0, httpStatus: 200, transportStatus: 0};
  render({...selected, cloudCall: call});
  assert.strictEqual(nodes.dLaMarzoccoCloudEmail.textContent, selected.email);
  assert.strictEqual(nodes.dLaMarzoccoCloudMachine.textContent, selected.selectedName,
      'Account and machine text must never be interpreted as HTML');
  assert.strictEqual(nodes.dLaMarzoccoCloudTime.textContent, '1790726400 UTC');
  assert.strictEqual(nodes.dLaMarzoccoCloudApi.textContent, 'GET read_dashboard');
  assert.strictEqual(nodes.dLaMarzoccoCloudResult.textContent, 'diagnostic.cloud_success · HTTP 200');
  assert.strictEqual(nodes.dLaMarzoccoCloudDuration.textContent, '0 ms');
  for (const result of ['canceled', 'http_error', 'transport_error', 'invalid_response',
    'response_too_large', 'setup_error']) {
    render({...selected, cloudCall: {...call, result, httpStatus: 0, transportStatus: -1}});
    assert.strictEqual(nodes.dLaMarzoccoCloudResult.textContent, `diagnostic.cloud_${result} · -1`);
  }
  for (const status of [401, 403, 429, 503]) {
    render({...selected, cloudCall: {...call, result: 'http_error', httpStatus: status}});
    assert(nodes.dLaMarzoccoCloudResult.textContent.endsWith(`HTTP ${status}`));
  }
  render({cloudCall: call});
  assert.strictEqual(nodes.dLaMarzoccoCloudMachine.textContent, 'diagnostic.cloud_no_machine',
      'Disconnect must clear the selected identity while retaining the last call');
  render({cloudCall: null});
  assert.strictEqual(nodes.dLaMarzoccoCloudApi.textContent, 'diagnostic.cloud_no_call');
  assert.strictEqual(nodes.dLaMarzoccoCloudDuration.textContent, 'diagnostic.cloud_no_call');
  assert(!laMarzoccoStatusSource.includes('page == StatusPage::Settings ? safeLaMarzoccoUsername : ""'));
  assert(laMarzoccoStatusSource.includes('machineIntegrationCloudCall()'));
  assert(cloudUi.includes('renderLaMarzoccoCloudDiagnostic(s.laMarzocco, s.companion?.route === "companion")'));
  const complete = laMarzoccoService.slice(laMarzoccoService.indexOf('void OpenBrewByWeightLaMarzoccoService::releaseIoBuffer('),
      laMarzoccoService.indexOf('void OpenBrewByWeightLaMarzoccoService::releaseWorkBuffer('));
  assert(complete.includes('serialTraceCategoryf(success ? LogLevel::INFO'));
  assert(complete.includes('transient ? LogLevel::WARNING : LogLevel::ERROR'));
  assert(complete.includes('laMarzoccoHttpRetryable(call.httpStatus)'));
  assert(complete.includes('publishedCloudCall_ = call;'));
  assert(complete.includes('call.result = "invalid_response"'));
  assert.strictEqual((laMarzoccoService.match(/releaseIoBuffer\(ok\)/g) || []).length, 2,
      'Both token responses must be validated before reporting success');
  for (const validated of ['releaseIoBuffer(listed);', 'releaseIoBuffer(decoded);']) {
    assert(laMarzoccoService.includes(validated),
        'Invalid machine lists and dashboards must be reported as errors');
  }
  assert(laMarzoccoService.includes('releaseIoBuffer(decoded)'));
  assert(laMarzoccoService.includes(': overflow ? "response_too_large"') &&
      laMarzoccoService.includes('canceled, work_->responseOverflow, ok, work_->httpStatus'));
  assert(laMarzoccoService.includes(': httpStatus != 0 ? "http_error" : "transport_error"'));
}
const deferObservation = laMarzoccoService.slice(
  laMarzoccoService.indexOf('void OpenBrewByWeightLaMarzoccoService::deferObservation('),
  laMarzoccoService.indexOf('void OpenBrewByWeightLaMarzoccoService::fail('));
const terminalFailure = laMarzoccoService.slice(
  laMarzoccoService.indexOf('void OpenBrewByWeightLaMarzoccoService::fail('),
  laMarzoccoService.indexOf('void OpenBrewByWeightLaMarzoccoService::scheduleAutomatic('));

if (!network.includes('/api/v1/machine/la-marzocco') ||
    !laMarzoccoWeb.includes('UNSUPPORTED_MACHINE') ||
    !laMarzoccoWeb.includes('jsonHasOnlyUniqueFields') ||
    !laMarzoccoWeb.includes('LA_MARZOCCO_STA_REQUIRED') ||
    !laMarzoccoWeb.includes('queueMachineIntegrationConnect') ||
    !laMarzoccoWeb.includes('queueMachineIntegrationRelist(requestId)') ||
    !laMarzoccoWeb.includes('selectMachineIntegrationDevice') ||
    !laMarzoccoWeb.includes('disconnectLaMarzocco(candidate)') ||
    !laMarzoccoWeb.includes('stagedLaMarzocco_')) {
  throw new Error('Linea Micra API must be profile-gated, strict, STA-only, staged, and asynchronous');
}
const laMarzoccoStatusFailures = [
  laMarzoccoStatusSource.includes('\"password\"') && 'password exposed',
  laMarzoccoStatusSource.includes('\"username\"') && 'username exposed',
  !laMarzoccoStatusSource.includes('delta.field("laMarzocco.accountConfigured"') && 'account status missing',
  !laMarzoccoStatusSource.includes('delta.field("laMarzocco.email"') && 'account email missing',
  !laMarzoccoStatusSource.includes('fields.field("laMarzocco.machines"') && 'machine list missing',
  laMarzoccoStatusSource.includes('accountMachineCount') && 'unread account machine count streamed',
  !laMarzoccoStatusSource.includes('OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LA_MARZOCCO_CLOUD') && 'compile gate missing',
  !laMarzoccoStatusSource.includes('delta.field("machineIntegration"') && 'integration capability missing',
].filter(Boolean);
if (laMarzoccoStatusFailures.length) {
  throw new Error('Linea Micra status contract: ' + laMarzoccoStatusFailures.join(', '));
}
// The shared header's cloud indicator: its identity fields ride the Home
// stream alone (one owner per path), and the shell ships the button, popup
// and glyph as micra-only markup the generator strips from other builds.
{
  const fail = (what) => { throw new Error('Linea Micra header indicator: ' + what); };
  const homeRegion = laMarzoccoStatusSource.slice(laMarzoccoStatusSource.indexOf('bool OpenBrewByWeightNetwork::sendHomeStream'),
      laMarzoccoStatusSource.indexOf('bool OpenBrewByWeightNetwork::sendUiStreamFrame'));
  for (const path of ['laMarzocco.email', 'laMarzocco.selectedName'])
    if (!homeRegion.includes(`delta.field("${path}"`)) fail(`the Home stream carries ${path}`);
  const settingsRegion = laMarzoccoStatusSource.slice(laMarzoccoStatusSource.indexOf('bool OpenBrewByWeightNetwork::sendSettingsStream'));
  for (const path of ['fields.field("laMarzocco.email"', 'fields.field("laMarzocco.selectedName"'])
    if (settingsRegion.includes(path)) fail(`Settings drops ${path}: Home owns it`);
  const diagRegion = laMarzoccoStatusSource.slice(laMarzoccoStatusSource.indexOf('bool OpenBrewByWeightNetwork::sendDiagnosticStream'),
      laMarzoccoStatusSource.indexOf('bool OpenBrewByWeightNetwork::sendLogStream'));
  if (diagRegion.includes('delta.field("laMarzocco.email"') ||
      diagRegion.includes('delta.field("laMarzocco.selectedName"'))
    fail('Diagnostic drops the identity fields: Home owns them');
  if (!(rawShellHtml.includes('<symbol id="dg-marzocco-cloud" class="laMarzoccoOnly" viewBox') &&
        rawShellHtml.includes('<symbol id="dg-marzocco-clouds" class="laMarzoccoOnly" viewBox') &&
        rawShellHtml.indexOf('id="companionSignal"') < rawShellHtml.indexOf('id="laMarzoccoSignal"') &&
        rawShellHtml.indexOf('id="laMarzoccoSignal"') < rawShellHtml.indexOf('id="wifiSignal"') &&
        rawShellHtml.includes('id="laMarzoccoDetails"') &&
        !laMarzoccoDiagnosticHtml.includes('<symbol id="dg-marzocco-cloud')))
    fail('the header cloud indicator sits right of companion with its one and two cloud glyphs');
}
if (laMarzoccoService.includes('save_client_session')) {
  throw new Error('Async Micra client must not resume TLS sessions: ESP-IDF re-applies them per handshake step');
}
if (!laMarzoccoService.includes('esp_log_level_set("transport_ws", ESP_LOG_NONE);')) {
  throw new Error('SDK Upgrade write errors must not log signed authentication headers');
}
const laMarzoccoPublishAt = networkService.indexOf(
    'publishMachineIntegrationNetworkState(');
const laMarzoccoNetworkState = laMarzoccoService.slice(
    laMarzoccoService.indexOf('void OpenBrewByWeightLaMarzoccoService::publishNetworkState('),
    laMarzoccoService.indexOf('bool OpenBrewByWeightLaMarzoccoService::queueConnect('));
if (!laMarzoccoService.includes('config.is_async = true;') ||
    !laMarzoccoService.includes('while (performed == ESP_ERR_HTTP_EAGAIN)') ||
    !laMarzoccoService.includes('(void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));') ||
    !laMarzoccoService.includes('millis() - requestStartedAtMs') ||
    laMarzoccoPublishAt < 0 ||
    !laMarzoccoNetworkState.includes('abortRequested_.store(true,') ||
    !laMarzoccoNetworkState.includes('xTaskNotifyGive(task_)') ||
    laMarzoccoNetworkState.includes('esp_http_client_') ||
    networkService.includes('serviceMachineIntegrationAbort') ||
    laMarzoccoService.includes('esp_http_client_cancel_request') ||
    laMarzoccoService.includes('activeClient_') || laMarzoccoService.includes('clientMux_')) {
  throw new Error(
      'Linea Micra HTTPS cancellation must wake its sole owner without mutating live TLS from another task');
}
if (!laMarzoccoTiming.includes('kStatePollMs = 30000') ||
    !laMarzoccoTiming.includes('kStateFreshnessMs = kStatePollMs') ||
    !laMarzoccoTiming.includes('kOptimisticOverlayMs = 2U * kStatePollMs') ||
    !laMarzoccoTiming.includes('kExhaustedCooldownMs = kOptimisticOverlayMs') ||
    !laMarzoccoTiming.includes('kPostWakeObservationDelayMs = 15000') ||
    !laMarzoccoTiming.includes('kStaleRetentionMs = 15U * 60U * 1000U') ||
    !laMarzoccoService.includes('const bool networkReady = networkEligible(observationGate)') ||
    !laMarzoccoService.includes('pending_.present && networkReady && !localActivity &&') ||
    !laMarzoccoService.includes('(!pendingObservation || observationReady)') ||
    !laMarzoccoService.includes('observationSchedule_.armPostEvent(now)') ||
    !laMarzoccoService.includes('? powerState_.noteStandbyCommandAccepted(published_,') ||
    !laMarzoccoService.includes(': powerState_.notePowerOnCommandAccepted(published_,') ||
    !laMarzoccoService.includes('observationSchedule_.armPostEvent(millis())') ||
    !laMarzoccoService.includes('deferObservation(pending, status, gateError)') ||
    !laMarzoccoService.includes('observationActive_.load(std::memory_order_acquire)') ||
    !laMarzoccoService.includes('bool OpenBrewByWeightLaMarzoccoService::observationCurrent(') ||
    !laMarzoccoService.includes('pending_.request.type == LaMarzoccoRequestType::OBSERVE_STATE) {') ||
    !laMarzoccoService.includes('!config_.accountConfigured ||\n      (config_.options & LA_MARZOCCO_OBSERVE_STATE) == 0 ||') ||
    !laMarzoccoService.includes('scheduleAutomatic(millis(), connecting)')) {
  throw new Error('Linea Micra observations must wait for readiness and the post-wake convergence deadline');
}
if (!laMarzoccoStatusSource.includes('delta.field("laMarzocco.optimisticOn"') ||
    !laMarzoccoStatusSource.includes('delta.field("laMarzocco.optimisticOff"') ||
    !deferObservation.includes('powerState_.hold(published_, true, millis());') ||
    deferObservation.includes('published_.powerState =') ||
    !laMarzoccoService.includes('status.phase = LaMarzoccoPhase::BACKOFF;\n    publish(status);') ||
    !terminalFailure.includes('powerState_.hold(published_,') ||
    terminalFailure.includes('published_.powerState =') ||
    terminalFailure.includes('LaMarzoccoPowerState::UNKNOWN')) {
  throw new Error('Pending, retry, and canceled observations must preserve power; a terminal API failure holds the last reading as a stale assumption');
}
if (!laMarzoccoTypes.includes('APPLY_TEMPERATURE') ||
    !machineIntegration.includes('requestMachineIntegrationPresetTemperature') ||
    !commandPersistence.includes('requestActivePresetMachineTemperature()') ||
    !controlCommands.includes('applyMachineTemperature') ||
    !scaleEvents.includes('requestActivePresetMachineTemperature()') ||
    !networkService.includes('scaleConnecting_.load(std::memory_order_acquire)') ||
    !laMarzoccoService.includes('CoffeeMachineSettingCoffeeBoilerTargetTemperature') ||
    !laMarzoccoService.includes('{\\\"boilerIndex\\\":1,\\\"targetTemperature\\\":%u.%u}') ||
    !laMarzoccoService.includes('verification.targetDeciC == target') ||
    // The preset's target never leaves as it is: what is sent is that target
    // inside the range the machine reported, and nothing without that range.
    !laMarzoccoService.includes('target = laMarzoccoSendableBrewTarget(published_, request.targetDeciC);') ||
    !laMarzoccoService.includes('if (target == 0 && !unread) break;') ||
    !laMarzoccoService.includes(': writeTemperature(settings, target);') ||
    laMarzoccoService.includes('writeTemperature(settings, request.targetDeciC)') ||
    laMarzoccoService.includes('LaMarzoccoCompanionCommandOp::TEMPERATURE, request.targetDeciC') ||
    !laMarzoccoService.includes('commandAccepted = desiredTemperature_.commandAccepted;') ||
    !laMarzoccoService.includes('desiredTemperature_.commandAccepted = true;') ||
    !laMarzoccoService.includes('laMarzoccoHttpRetryable') ||
    !laMarzoccoService.includes('laMarzoccoTemperatureCycleRetryable') ||
    !laMarzoccoService.includes('observationFence_.merge(published_, powerState_, update') ||
    !laMarzoccoService.includes('desiredTemperature_.machineConfigGeneration = configGeneration;') ||
    !laMarzoccoService.includes('abortRequested_.load(std::memory_order_acquire)') ||
    !laMarzoccoService.includes('refreshToken(settings) || signIn(settings)') ||
    !laMarzoccoService.includes('char authorization[kTokenCapacity + 8] = {};') ||
    !laMarzoccoService.includes('snprintf(authorization, kTokenCapacity + 8,') ||
    !laMarzoccoService.includes('esp_http_client_set_header(work_->client, "Authorization",\n                                               authorization) == ESP_OK') ||
    !laMarzoccoService.includes('secureWipe(authorization, kTokenCapacity + 8);') ||
    laMarzoccoService.includes('Bearer %s", work_->accessToken);\n    const bool ok = length > 0 &&\n                    static_cast<size_t>(length) < sizeof(io_->response)') ||
    !laMarzoccoStatusSource.includes('delta.field("laMarzocco.temperatureState"') ||
    !laMarzoccoStatusSource.includes('delta.field("laMarzocco.temperatureError"')) {
  throw new Error('Linea Micra preset temperature must coalesce, gate on scale/shot activity, reuse authentication, and confirm dashboard readback');
}
for (const id of ['laMarzoccoSignIn', 'laMarzoccoBusy', 'laMarzoccoSteps', 'laMarzoccoLegendHint',
  'laMarzoccoChoose', 'laMarzoccoConnected',
  'laMarzoccoUsername', 'laMarzoccoPassword', 'laMarzoccoWifiNote', 'laMarzoccoProgress',
  'laMarzoccoConnectButton', 'laMarzoccoMachines', 'laMarzoccoSelectButton',
  'laMarzoccoCancelButton', 'laMarzoccoPowerDeps', 'laMarzoccoLinkedNote',
  'laMarzoccoApplyTemperature', 'laMarzoccoObserveState',
  'laMarzoccoBoostScaleDetectionWhenOn', 'laMarzoccoReduceScaleScanningWhenOff',
  'laMarzoccoRecognizeWake', 'laMarzoccoShutdownWithScale',
  'laMarzoccoShutdownGraceWrap', 'laMarzoccoShutdownGrace',
  'laMarzoccoDisconnectButton', 'laMarzoccoChangeButton', 'laMarzoccoBrewTargetC']) {
  if (!laMarzoccoSettingsHtml.includes(`id="${id}"`)) {
    throw new Error(`Linea Micra settings control is missing: ${id}`);
  }
}
{
  const assert = require('assert'), vm = require('vm');
  const nodes = new Map();
  const context = {__LA_MARZOCCO__: true, Date: {now: () => 3600100}, controlsMutable: true,
    $: id => {
      if (id === 'laMarzoccoRefreshLink') return null;
      if (!nodes.has(id)) nodes.set(id, {textContent: ''});
      return nodes.get(id);
    }, __WEBUI_TEXT__: key => key === 'runtime.s_ago' ? ' s ago'
      : key === 'diagnostic.ready_in' ? 'ready in %1'
      : ['diagnostic.deg_c', 'diagnostic.utc_close', 'diagnostic.minutes_short', 'diagnostic.s',
        'diagnostic.level_word'].includes(key) ? localeReference.strings[key] : key};
  context.formatWallTimeLocal = seconds => 'T' + seconds;
  context.deviceUtcSec = () => Math.floor(context.Date.now() / 1000);
  context.R = context;
  vm.createContext(context);
  vm.runInContext(blockAt(rawViewJs.diagnostic, 'function renderLaMarzoccoDiagnostic('), context);
  for (const policy of ['connection', 'time', undefined]) {
    for (const quality of ['current', 'stale', 'optimistic', 'unsupported']) {
      context.laMarzoccoDiagnosticStatus = {sampleValid: true, sampleAgeMs: 0, receivedAtMs: 100,
        freshnessMs: 30000, freshnessPolicy: policy, powerState: 'OFF', quality};
      vm.runInContext('renderLaMarzoccoDiagnostic()', context);
      assert.strictEqual(nodes.get('dLaMarzoccoPowerValue').textContent, 'OFF');
      assert.strictEqual(nodes.get('dLaMarzoccoQuality').textContent,
          quality === 'current' && policy !== 'connection' ? 'stale' : quality);
    }
  }
  // Boiler readiness rendering: raw state, qualifier, locally computed
  // remaining estimate, and derived water. Date.now() is fixed at 3600100 ms,
  // so a readyAt 3720100 ms is 120 s ahead.
  const linked = {accountConfigured: true, observeState: true, powerState: 'ON',
    receivedAtMs: 3600100, readiness: 'waiting_for_steam', boiler: {sampleAgeMs: 4000,
      coffee: {state: 'ready', targetDeciC: 935, readyAtUtcSec: 0},
      steam: {state: 'heating_up', level: 'level_3', readyAtUtcSec: 3720}}};
  context.laMarzoccoDiagnosticStatus = linked;
  vm.runInContext('renderLaMarzoccoDiagnostic()', context);
  assert.strictEqual(nodes.get('dLaMarzoccoReadiness').textContent, 'waiting_for_steam');
  assert.strictEqual(nodes.get('dLaMarzoccoBrewBoiler').textContent, 'ready · 93.5 °C · 4 s ago');
  assert.strictEqual(nodes.get('dLaMarzoccoSteamBoiler').textContent,
      'heating up · level 3 · ready in 2m 0s (T3720 UTC) · 4 s ago');
  assert.strictEqual(nodes.get('dLaMarzoccoWater').textContent, 'diagnostic.water_states'.split('|')[0]);
  context.laMarzoccoDiagnosticStatus = {...linked,
    boiler: {...linked.boiler, coffee: {...linked.boiler.coffee, state: 'no_water'}}};
  vm.runInContext('renderLaMarzoccoDiagnostic()', context);
  assert.strictEqual(nodes.get('dLaMarzoccoWater').textContent, 'diagnostic.water_states'.split('|')[1]);
  context.laMarzoccoDiagnosticStatus = {accountConfigured: true, observeState: false};
  vm.runInContext('renderLaMarzoccoDiagnostic()', context);
  assert.strictEqual(nodes.get('dLaMarzoccoReadiness').textContent, 'runtime.disabled');
  assert.strictEqual(nodes.get('dLaMarzoccoWater').textContent, 'runtime.disabled');
  assert(laMarzoccoStatusSource.includes('laMarzoccoPowerSourceName(micra.powerSource)'));
  assert(laMarzoccoStatusSource.includes('micra.connectionFreshness ? "connection" : "time"'));
  assert(network.includes('delta.field("laMarzocco.readiness"'),
      'Home stream must project the derived Micra readiness');
  assert(network.includes('delta.field("laMarzocco.boiler.coffee.state"') &&
         network.includes('delta.field("laMarzocco.boiler.steam.readyAtUtcSec"'),
      'Home stream must project the boiler states and estimates');
}
for (const id of ['dLaMarzoccoPower', 'dLaMarzoccoPowerValue', 'dLaMarzoccoMode',
  'dLaMarzoccoQuality', 'dLaMarzoccoAge', 'laMarzoccoRefreshLink']) {
  if (!laMarzoccoDiagnosticHtml.includes(`id="${id}"`)) {
    throw new Error(`Linea Micra diagnostic control is missing: ${id}`);
  }
}
{
  const assert = require('assert');
  const vm = require('vm');
  const binding = viewJs.settings.match(/\$\(\s*['"]laMarzoccoDisconnectButton['"]\s*\)\s*\.\s*onclick\s*=([\s\S]*?);\s*for\s*\(/);
  assert(binding, 'Micra Disconnect click binding is missing');
  let calls = 0, confirmed = false;
  const click = vm.runInNewContext(`(${binding[1]})`, {
    confirm: () => confirmed,
    __WEBUI_TEXT__: key => key,
    disconnectLaMarzocco: () => { calls++; },
  });
  click();
  assert.strictEqual(calls, 0, 'Cancel must leave the account connected');
  confirmed = true;
  click();
  assert.strictEqual(calls, 1, 'Confirmed Disconnect must invoke the action');

  // The Micra settings code lives in the Settings module; its runtime and
  // shared-state references resolve to this harness's globals.
  const statusUi = rawViewJs.settings.slice(rawViewJs.settings.search(/const\s+LA_MARZOCCO_SWITCHES\s*=/),
      rawViewJs.settings.indexOf('function loadBullseyeConfig(')).replace(/\bR\.(?:uiState\.)?/g, '');
  const nodes = new Map();
  const node = () => {
    const n = {disabled: false, checked: false, hidden: false, value: '', textContent: '', innerText: '',
      dataset: {}, classes: new Set(), children: [], focused: false,
      classList: {toggle(name, on) { on ? n.classes.add(name) : n.classes.delete(name); }, add(name) { n.classes.add(name); }},
      focus() { n.focused = true; },
      replaceChildren(...c) { n.children = c; },
      querySelectorAll: () => n.children.flatMap(c => c.radio ? [c.radio] : [c]),
      querySelector: sel => sel === 'input:checked' ? n.children.map(c => c.radio).find(r => r && r.checked) || null
        : sel === 'b' ? n.b : sel === 'small' ? n.small : null};
    return n;
  };
  const get = id => { if (!nodes.has(id)) nodes.set(id, node()); return nodes.get(id); };
  get('laMarzoccoPowerDeps').children = ['RecognizeWake', 'ScaleOffWithMachine', 'BoostScaleDetectionWhenOn',
    'ReduceScaleScanningWhenOff'].map(id => get('laMarzocco' + id));
  const commands = [];
  const rootClasses = new Set();
  let actionOk = true, polls = 0;
  get('laMarzoccoSteps').children = [node(), node(), node()];
  const steps = () => get('laMarzoccoSteps').children.map(li => li.className).join();
  const context = vm.createContext({__LA_MARZOCCO__: true, $: get, controlsMutable: true,
    laMarzoccoDirty: false, laMarzoccoSignInState: null, laMarzoccoChange: false,
    refreshStatus: () => {}, setTimeout: () => { polls++; return 1; },
    laMarzoccoAction: async action => { commands.push({action}); return actionOk; },
    document: {documentElement: {classList: {toggle(name, on) {
      on ? rootClasses.add(name) : rootClasses.delete(name);
    }}}, createElement() {
      const label = node();
      Object.defineProperty(label, 'innerHTML', {set() {
        label.radio = node(); label.firstChild = label.radio; label.b = node(); label.small = node();
      }});
      return label;
    }},
    command: async (url, body) => { commands.push({url, body: JSON.parse(JSON.stringify(body))}); return true; },
    __WEBUI_TEXT__: key => key});
  vm.runInContext(statusUi, context);
  const apply = lm => vm.runInContext(`applyLaMarzoccoStatus(${JSON.stringify({laMarzocco: lm})})`, context);
  const panes = () => ['SignIn', 'Busy', 'Choose', 'Connected'].filter(p => !get('laMarzocco' + p).hidden).join();
  const flush = () => new Promise(resolve => setImmediate(resolve));
  const run = async () => {
    const base = {accountConfigured: false, email: '', selectedName: '', selectedSerial: '', machines: [],
      phase: 'disabled', error: 'none', staConnected: true, apActive: false, observeState: true,
      applyTemperature: true, recognizeWakeGesture: true, shutdownWithScale: true, shutdownGraceSeconds: 15,
      scaleOffWithMachine: true, boostScaleDetectionWhenOn: true, reduceScaleScanningWhenOff: true,
      powerOnWithScale: false};
    apply(base);
    assert.strictEqual(panes(), 'SignIn', 'Only the sign-in form shows before an account is connected');
    assert.strictEqual(get('laMarzoccoProgress').textContent, '');
    apply({...base, apActive: true});
    assert(!get('laMarzoccoWifiNote').classes.has('hidden') && get('laMarzoccoConnectButton').disabled,
        'Sign-in must explain and wait while the setup network is open');
    assert.strictEqual(get('laMarzoccoLegendHint').textContent, 'runtime.not_connected');
    apply({...base, phase: 'authenticating'});
    assert.strictEqual(panes(), 'Busy', 'A running sign-in replaces the form with its progress steps');
    assert.strictEqual(steps(), 'on,,');
    assert.strictEqual(get('laMarzoccoLegendHint').textContent, 'runtime.la_marzocco_state_connecting');
    assert.strictEqual(polls, 0, 'Sign-in progress arrives on the Settings stream; nothing polls');
    apply({...base, phase: 'listing'});
    assert.strictEqual(steps(), 'ok,on,', 'Finished steps turn green as the next one starts');
    apply({...base, phase: 'failed', error: 'invalid_auth'});
    assert.strictEqual(panes(), 'SignIn');
    assert(!get('laMarzoccoConnectButton').classes.has('busy'));
    assert.strictEqual(get('laMarzoccoProgress').textContent, 'runtime.la_marzocco_error_auth',
        'A rejected sign-in must read in plain language, never as an error code');
    assert(get('laMarzoccoProgress').classes.has('error'));
    apply({...base, phase: 'failed', error: 'canceled'});
    assert.strictEqual(get('laMarzoccoProgress').textContent, '', 'Routine cancellations stay silent');

    // A lone listed machine is saved with the shown options, whoever signed in.
    const lone = {...base, phase: 'confirmed', machines: [{serial: 'ABC', name: 'Micra'}]};
    apply(lone);
    await flush();
    assert.deepStrictEqual(commands[0], {url: '/api/v1/machine/la-marzocco', body: {
      action: 'select', serial: 'ABC', applyTemperature: true, observeState: true,
      boostScaleDetectionWhenOn: true, reduceScaleScanningWhenOff: true,
      recognizeWakeGesture: true, powerOnWithScale: false, shutdownWithScale: true,
      scaleOffWithMachine: true, connectionType: 'websocket', shutdownGraceSeconds: 15}});
    apply(lone);
    assert.strictEqual(panes(), 'Busy', 'The lone machine is linked without a choice screen');
    assert.strictEqual(steps(), 'ok,ok,on');
    assert.strictEqual(commands.length, 1, 'The automatic selection is sent once');

    // A failed automatic selection falls back to the choice, without retrying.
    context.laMarzoccoSignInState = 'failed';
    apply(lone);
    assert.strictEqual(panes(), 'Choose');
    context.laMarzoccoSignInState = 'pending';
    apply({...lone, machines: [{serial: 'ABC', name: 'Micra'}, {serial: 'DEF', name: ''}]});
    assert.strictEqual(panes(), 'Choose', 'Several machines need an explicit choice');
    assert.strictEqual(get('laMarzoccoLegendHint').textContent, 'runtime.la_marzocco_state_choose');
    assert.strictEqual(commands.length, 1);
    const machines = get('laMarzoccoMachines').children;
    assert.deepStrictEqual(machines.map(m => [m.radio.value, m.b.textContent, m.small.textContent]),
        [['ABC', 'Micra', 'ABC'], ['DEF', 'DEF', 'DEF']]);
    assert(get('laMarzoccoSelectButton').disabled, 'Use this machine waits for a choice');

    const connected = {...base, accountConfigured: true, email: 'user@example.test', selectedName: 'Micra',
      selectedSerial: 'ABC', phase: 'idle', powerState: 'ON', boiler: {coffee: {targetDeciC: 935}}};
    apply(connected);
    assert.strictEqual(panes(), 'Connected');
    assert.strictEqual(context.laMarzoccoSignInState, null);
    // The preset temperature field follows the machine's own range, which
    // only a cloud session reads: without it there is nothing to set.
    assert(!rootClasses.has('laMarzoccoTemperatureEnabled'), 'No range yet: no brew temperature field');
    const ranged = {...connected, boiler: {coffee: {targetDeciC: 935, targetMinDeciC: 850, targetMaxDeciC: 1040}}};
    apply(ranged);
    assert(rootClasses.has('laMarzoccoTemperatureEnabled'), 'A reported range opens the field');
    assert.deepStrictEqual([get('laMarzoccoBrewTargetC').min, get('laMarzoccoBrewTargetC').max], [85, 104]);
    for (const off of [{applyTemperature: false}, {accountConfigured: false},
      {boiler: {coffee: {targetMinDeciC: 0, targetMaxDeciC: 0}}},
      {boiler: {coffee: {targetMinDeciC: 900, targetMaxDeciC: 900}}}]) {
      apply({...ranged, ...off});
      assert(!rootClasses.has('laMarzoccoTemperatureEnabled'), JSON.stringify(off));
    }
    apply(connected);
    assert.strictEqual(get('laMarzoccoIdentity').innerText, 'Micra · ABC');
    assert.strictEqual(get('laMarzoccoAccount').innerText, 'user@example.test');
    assert.strictEqual(get('laMarzoccoStatus').textContent, 'runtime.la_marzocco_power_on · runtime.target 93.5 °C');
    assert.strictEqual(get('laMarzoccoStatus').dataset.tone, 'on', 'An ON machine shows a green state pill');
    const pill = lm => { apply({...connected, ...lm}); const p = get('laMarzoccoStatus'); return [p.textContent, p.dataset.tone]; };
    assert.deepStrictEqual(pill({readiness: 'ready'}), ['runtime.la_marzocco_pill_ready · runtime.target 93.5 °C', 'on']);
    assert.deepStrictEqual(pill({readiness: 'warming_up'}), ['runtime.la_marzocco_pill_heating · runtime.target 93.5 °C', 'warn']);
    assert.deepStrictEqual(pill({readiness: 'needs_water'}), ['runtime.la_marzocco_pill_no_water', 'error']);
    assert.deepStrictEqual(pill({powerState: 'OFF'}), ['runtime.la_marzocco_power_standby', ''], 'Standby stays neutral');
    assert.deepStrictEqual(pill({machineOffline: true}), ['runtime.la_marzocco_pill_offline', 'warn']);
    assert.deepStrictEqual(pill({error: 'communication_error'}), ['runtime.la_marzocco_pill_cloud', 'warn']);
    assert.strictEqual(get('laMarzoccoStatus').title, 'runtime.la_marzocco_error_cloud', 'The pill explains the problem on hover');
    assert.deepStrictEqual(pill({error: 'invalid_auth'}), ['runtime.la_marzocco_pill_account', 'error']);
    assert.deepStrictEqual(pill({apActive: true}), ['runtime.ap_mode_paused', 'warn']);
    assert.deepStrictEqual(pill({observeState: false}), ['runtime.la_marzocco_pill_monitoring_off', '']);
    assert.deepStrictEqual(pill({shotPaused: true}), ['runtime.la_marzocco_power_on · runtime.target 93.5 °C · runtime.shot_paused', 'on']);
    apply(connected);
    context.laMarzoccoDirty = true;
    get('laMarzoccoObserveState').checked = false;
    get('laMarzoccoApplyTemperature').checked = false;
    apply(connected);
    assert.strictEqual(get('laMarzoccoApplyTemperature').checked, false, 'Status must not overwrite unsaved edits');
    assert(get('laMarzoccoPowerDeps').classes.has('fieldOff') && get('laMarzoccoRecognizeWake').disabled,
        'Options that need the power state follow Show power state');
    const all = vm.runInContext('laMarzoccoPayload()', context);
    assert.strictEqual(all.applyTemperature, false);
    assert.strictEqual(all.shutdownGraceSeconds, 15);
    await vm.runInContext('saveLaMarzoccoSettings("save", "", {applyTemperature: false, observeState: false})', context);
    assert.deepStrictEqual(commands[1].body, {action: 'save', applyTemperature: false, observeState: false},
      'Autosave sends only the changed options, keeping the saved transport, without credentials');
    context.laMarzoccoDirty = false;
    get('laMarzoccoShutdownWithScale').checked = false;
    vm.runInContext('updateLaMarzoccoDependentControls()', context);
    assert(get('laMarzoccoShutdownGraceWrap').classes.has('hidden') && get('laMarzoccoShutdownGrace').disabled);

    // Change machine is always offered; the account is listed only on demand.
    const handler = id => vm.runInContext('(' + rawViewJs.settings.match(
        new RegExp(`\\$\\("${id}"\\)\\.onclick = ([\\s\\S]*?);\\n\\s+if \\(\\$\\(`))[1]
        .replace(/\bR\.(?:uiState\.)?/g, '') + ')', context);
    commands.length = 0;
    apply({...connected, accountMachineCount: 0});
    apply({...connected, accountMachineCount: 1});
    assert.deepStrictEqual(commands, [], 'Opening Settings never lists the account on its own');
    assert(!get('laMarzoccoChangeButton').hidden && !get('laMarzoccoChangeButton').disabled);
    await handler('laMarzoccoChangeButton')();
    apply({...connected, machineListError: 'none', machines: [{serial: 'ABC', name: 'Micra'}]});
    assert.strictEqual(panes(), 'Choose', 'A single-machine account still sees its machine listed');
    handler('laMarzoccoCancelButton')();
    assert.strictEqual(panes(), 'Connected');
    const several = {...connected, accountMachineCount: 2, machineListError: 'none'},
      listed = {...several, machines: [{serial: 'ABC', name: 'Micra'}, {serial: 'DEF', name: 'Other'}]};
    apply(several);
    assert(!get('laMarzoccoChangeButton').hidden && !get('laMarzoccoChangeButton').disabled);
    await handler('laMarzoccoChangeButton')();
    assert.deepStrictEqual(commands.at(-1), {action: 'relist'});
    apply({...several, machineListing: true});
    assert.strictEqual(panes(), 'Choose', 'Change machine reuses the choice screen');
    assert.strictEqual(get('laMarzoccoProgress').textContent, 'runtime.la_marzocco_progress_listing');
    apply({...several, machineListError: 'invalid_auth'});
    assert.strictEqual(get('laMarzoccoProgress').textContent, 'runtime.la_marzocco_error_auth');
    assert(get('laMarzoccoProgress').classes.has('error'));
    apply(listed);
    assert.strictEqual(get('laMarzoccoProgress').textContent, '');
    handler('laMarzoccoCancelButton')();
    assert.strictEqual(panes(), 'Connected', 'Cancel returns to the saved machine');
    apply(listed);
    assert.strictEqual(panes(), 'Connected');
    assert(!commands.some(c => c.action === 'disconnect'), 'Cancel never disconnects the account');
    actionOk = false;
    await handler('laMarzoccoChangeButton')();
    assert.strictEqual(panes(), 'Connected', 'A refused listing keeps the saved machine on screen');
    actionOk = true;
    await handler('laMarzoccoChangeButton')();
    apply(listed);
    await vm.runInContext('selectLaMarzocco("DEF")', context);
    assert.deepStrictEqual([commands.at(-1).body.action, commands.at(-1).body.serial], ['select', 'DEF']);
    assert.strictEqual(panes(), 'Connected', 'The chosen machine replaces the saved one in place');
    apply({...connected, accountConfigured: false});
    assert.strictEqual(context.laMarzoccoChange, false);
  };
  run().catch(error => {console.error(error); process.exitCode = 1;});
}
if (!rawCss.includes('html:not(.laMarzoccoIntegration) .laMarzoccoOnly') ||
    !rawCss.includes('.laMarzoccoDeps{') ||
    !rawCss.includes('[type=email]') ||
    !laMarzoccoDiagnosticHtml.includes('<span id="dLaMarzoccoPowerValue">—</span> - <a id="laMarzoccoRefreshLink" href="#" aria-disabled="true" tabindex="-1">({{webui:diagnostic.refresh_state}})</a>') ||
    laMarzoccoDiagnosticHtml.includes('id="laMarzoccoRefreshButton"') ||
    !codeIncludes(viewJs.diagnostic, "e.preventDefault();R.laMarzoccoAction('refresh')") ||
    !codeIncludes(rawRuntimeJs, "s.machineIntegration==='la_marzocco_cloud'") ||
    !codeIncludes(rawAllJs, "{action:'connect',username,password}") ||
    !codeIncludes(rawAllJs, "laMarzoccoAction('disconnect')") ||
    codeIncludes(rawRuntimeJs, "['queued','authenticating','listing','running','backoff'].includes(m.phase)") ||
    codeIncludes(rawAllJs, 'laMarzoccoConnectionType') ||
    codeIncludes(rawRuntimeJs, 'laMarzoccoSaveButton') ||
    !codeIncludes(viewJs.settings, "el.closest('#laMarzoccoSignIn,#laMarzoccoChoose,[data-g=app]')") ||
    laMarzoccoSettingsHtml.indexOf('id="laMarzoccoSettings"') > laMarzoccoSettingsHtml.indexOf('{{webui:settings.shared_settings}}') ||
    !laMarzoccoSettingsHtml.includes('<option value="15" selected>') ||
    laMarzoccoSettingsHtml.includes('id="laMarzoccoPowerOnWithScale" type="checkbox" checked') ||
    !codeIncludes(rawViewJs.diagnostic, "$('dLaMarzoccoPowerValue').textContent=power") ||
    !codeIncludes(rawViewJs.diagnostic, "$('dLaMarzoccoAge').textContent=lm.temperatureState+'/'+lm.temperatureError+' · '") ||
    !codeIncludes(rawViewJs.diagnostic, "refresh.setAttribute('aria-disabled',String(disabled))") ||
    codeIncludes(rawViewJs.diagnostic, "expired?'UNKNOWN':lm.powerState") ||
    !codeIncludes(rawViewJs.diagnostic, 'power=lm.powerState') ||
    !codeIncludes(rawViewJs.diagnostic, "stale=lm.freshnessPolicy!=='connection'&&lm.quality==='current'&&age>=lm.freshnessMs") ||
    ['ApplyTemperature', 'ObserveState', 'RecognizeWake', 'ShutdownWithScale', 'ScaleOffWithMachine',
      'BoostScaleDetectionWhenOn', 'ReduceScaleScanningWhenOff'].some(id =>
      !laMarzoccoSettingsHtml.includes(`id="laMarzocco${id}" type="checkbox" checked`))) {
  throw new Error('Linea Micra UI must implement account connection, selection, and quality-only freshness expiry');
}
if (laMarzoccoService.includes('keep_alive_enable = true') ||
    (laMarzoccoService.match(/esp_http_client_close\(work_->client\)/g) || []).length !== 1 ||
    !laMarzoccoService.includes('pendingCloudCall_.idleMs >= la_marzocco_timing::kHttpReuseIdleMs') ||
    laMarzoccoService.includes('char authorization[kTokenCapacity + 8];') ||
    (laMarzoccoService.match(/"Accept",\s*"application\/json"/g) || []).length !== 1 ||
    (laMarzoccoService.match(/"User-Agent",\s*\n?\s*"OpenBrewByWeight\/1"/g) || []).length !== 1 ||
    !laMarzoccoService.includes('RequestStateGuard requestState{*this};') ||
    !laMarzoccoService.includes('~RequestStateGuard() { service.clearRequestState(); }') ||
    !laMarzoccoService.includes('esp_http_client_delete_header(work_->client, "Authorization")') ||
    !laMarzoccoService.includes('esp_http_client_delete_header(work_->client, "X-Request-Proof")') ||
    !laMarzoccoService.includes('esp_http_client_set_post_field(work_->client, nullptr, 0)') ||
    !laMarzoccoService.includes('config.buffer_size = 4096;') ||
    !laMarzoccoService.includes('config.buffer_size_tx = 4096;') ||
    !laMarzoccoService.includes('struct OpenBrewByWeightLaMarzoccoService::IoBuffer') ||
    !laMarzoccoService.includes('union {') ||
    !laMarzoccoService.includes('allocExternal(sizeof(WorkBuffer))') ||
    !laMarzoccoService.includes('allocExternal(sizeof(IoBuffer))') ||
    !laMarzoccoService.includes('if (connecting) releaseWorkBuffer();') ||
    !laMarzoccoService.includes('if (!staConnected || apActive) {') ||
    !laMarzoccoService.includes('releaseIoBuffer();') ||
    !laMarzoccoService.includes('const bool networkReady = networkEligible(observationGate)') ||
    !laMarzoccoService.includes('} else if (observationReady && config_.accountConfigured &&') ||
    !laMarzoccoService.includes('const bool dashboardRequired = status.sampleAtMs == 0 ||') ||
    !laMarzoccoService.includes('(sessionRenewed && !dashboardRequired) ||')) {
  throw new Error('Linea Micra polling must bound transient PSRAM/stack use, release idle sessions, wait for STA, reuse active HTTP sessions, and avoid continuous probes');
}
for (const file of ['OpenBrewByWeightMachinePaddleControl.h',
  'OpenBrewByWeightMachinePaddleInput.h', 'OpenBrewByWeightMachinePaddlePolicy.h',
  'OpenBrewByWeightMachinePaddleState.h']) {
  if (fs.readFileSync(path.join(sketchDir, file), 'utf8').includes('LaMarzocco')) {
    throw new Error(`Paddle behavior must remain independent of Linea Micra: ${file}`);
  }
}

const laMarzoccoScaleShutdown = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightLaMarzoccoScaleShutdown.h'), 'utf8');
const laMarzoccoScalePowerOn = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightLaMarzoccoScalePowerOn.h'), 'utf8');
const laMarzoccoMachinePower = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightLaMarzoccoMachinePower.h'), 'utf8');
const entrypoints = fs.readFileSync(
    path.join(sketchDir, 'platform/OpenBrewByWeightEntrypoints.inc'), 'utf8');
const protocolGeneric = fs.readFileSync(
    path.join(sketchDir, '..', 'libraries', 'EspressoScaleBLE', 'src',
              'protocols', 'GenericFf11.cpp'),
    'utf8');
if (!laMarzoccoScaleShutdown.includes('LA_MARZOCCO_SCALE_EXPLICIT_DISCONNECT = 9') ||
    !laMarzoccoScaleShutdown.includes('!observed.relayClosed') ||
    !laMarzoccoScaleShutdown.includes('LA_MARZOCCO_SHUTDOWN_WITH_SCALE') ||
    !laMarzoccoScaleShutdown.includes('laMarzoccoShutdownGraceSeconds') ||
    !laMarzoccoService.includes('CoffeeMachineChangeMode') ||
    !laMarzoccoService.includes('{\\"mode\\":\\"StandBy\\"}') ||
    !laMarzoccoService.includes('bool OpenBrewByWeightLaMarzoccoService::executePowerApplication(') ||
    !laMarzoccoService.includes('void OpenBrewByWeightLaMarzoccoService::deferPower(') ||
    !laMarzoccoService.includes('deferPower(request, LaMarzoccoError::CANCELED, 0, false)') ||
    !laMarzoccoService.includes('uint8_t powerOptionBit(LaMarzoccoRequestType type)') ||
    !laMarzoccoService.includes('? LA_MARZOCCO_POWER_ON_WITH_SCALE') ||
    !machineIntegration.includes('void serviceMachineIntegrationScaleLink(') ||
    !machineIntegration.includes('LaMarzoccoScaleShutdownTracker') ||
    !entrypoints.includes('serviceMachineIntegrationScaleLink(') ||
    !laMarzoccoWeb.includes('"shutdownWithScale"') ||
    !laMarzoccoWeb.includes('"shutdownGraceSeconds"') ||
    !laMarzoccoStatusSource.includes('fields.field("laMarzocco.shutdownWithScale"') ||
    !laMarzoccoStatusSource.includes('fields.field("laMarzocco.shutdownGraceSeconds"')) {
  throw new Error('Scale power-off shutdown must be machine-side, explicit-disconnect only, relay-guarded, grace-cancelled, and cloud-gated');
}
if (!laMarzoccoScalePowerOn.includes('LA_MARZOCCO_POWER_ON_WITH_SCALE') ||
    !laMarzoccoScalePowerOn.includes('LA_MARZOCCO_SCALE_EXPLICIT_DISCONNECT') ||
    !laMarzoccoScalePowerOn.includes('observed.relayClosed') ||
    !laMarzoccoService.includes('SET_POWER_ON') ||
    !laMarzoccoService.includes('{\\"mode\\":\\"BrewingMode\\"}') ||
    !machineIntegration.includes('LaMarzoccoScalePowerOnTracker') ||
    !machineIntegration.includes('LaMarzoccoRequestType::SET_POWER_ON') ||
    !laMarzoccoWeb.includes('"powerOnWithScale"') ||
    !laMarzoccoStatusSource.includes('fields.field("laMarzocco.powerOnWithScale"')) {
  throw new Error('Scale power-on must fire only after the scale\'s own power-off, stay relay-guarded, and send BrewingMode through the cloud');
}
if (!laMarzoccoMachinePower.includes('LA_MARZOCCO_SCALE_OFF_WITH_MACHINE') ||
    !laMarzoccoMachinePower.includes('LaMarzoccoObservationQuality::CURRENT') ||
    !machineIntegration.includes('LaMarzoccoMachinePowerTracker') ||
    !machineIntegration.includes('requestScalePowerOff()') ||
    !machineIntegration.includes('machineEffectivelyOff()') ||
    !machineIntegration.includes('Scale power-off skipped: connected scale does not support power-off') ||
    !entrypoints.includes('serviceMachineIntegrationMachinePower(') ||
    !entrypoints.includes('loopScaleLink.features.has(ScaleFeaturePowerOff)') ||
    !scaleWorker.includes('void requestScalePowerOff()') ||
    !scaleWorker.includes('executeScalePowerOffCommand') ||
    !protocolGeneric.includes('ScaleFeaturePowerOff') ||
    !laMarzoccoWeb.includes('"scaleOffWithMachine"') ||
    !laMarzoccoStatusSource.includes('fields.field("laMarzocco.scaleOffWithMachine"') ||
    !laMarzoccoWeb.includes('"boostScaleDetectionWhenOn"') ||
    !laMarzoccoWeb.includes('"reduceScaleScanningWhenOff"') ||
    !laMarzoccoStatusSource.includes('fields.field("laMarzocco.boostScaleDetectionWhenOn"') ||
    !laMarzoccoStatusSource.includes('fields.field("laMarzocco.reduceScaleScanningWhenOff"')) {
  throw new Error('Scale-off-with-machine must fire once per confirmed ON→OFF edge, warn instead of writing to unsupported scales, and never re-trigger the shutdown cycle');
}

{
  // The per-loop machine-power service is the only production seam between
  // the resolved Micra state and the scale worker's discovery duty; it must
  // publish the override before the scale power-off early return.
  const integration = fs.readFileSync(
      path.join(sketchDir, 'machine', 'OpenBrewByWeightLaMarzoccoIntegration.cpp'), 'utf8');
  const serviceStart = integration.indexOf('void serviceMachineIntegrationMachinePower(');
  const seam = integration.slice(serviceStart, integration.indexOf('\n}', serviceStart));
  const earlyReturn = seam.indexOf('machinePower.service(');
  if (serviceStart < 0 || earlyReturn < 0 ||
      !seam.includes('const LaMarzoccoStatus status = service.statusPoll();') ||
      seam.indexOf('laMarzoccoScanOverride(') < 0 ||
      seam.indexOf('applyLiveBleScanOverride(') > earlyReturn) {
    throw new Error(
        'serviceMachineIntegrationMachinePower must publish laMarzoccoScanOverride via applyLiveBleScanOverride before the scale power-off early return');
  }
}

{
  // The effectively-off shortcut must consume the non-blocking poll and let
  // a stale cache count as unknown (not off), so a stretched holder fails
  // toward queueing the redundant, server-revalidated StandBy command.
  const gateStart = machineIntegration.indexOf('bool machineEffectivelyOff()');
  const gate = machineIntegration.slice(gateStart, machineIntegration.indexOf('\n}', gateStart));
  if (gateStart < 0 ||
      !gate.includes('service.statusPoll()') ||
      !gate.includes('!service.statusPollStale()')) {
    throw new Error(
        'machineEffectivelyOff must poll without waiting and treat a stale poll cache as unknown, never off');
  }
}
// One-second tickers paint only the view that shows them.
if (!codeIncludes(viewJs.home, 'if(!document.hidden&&R.activeView===home)renderLaMarzoccoBoilers()') ||
    !codeIncludes(viewJs.diagnostic, 'if(!document.hidden&&R.activeView===diagnostic)renderDiagClock()')) {
  throw new Error('Hidden Home and Diagnostic views must not keep painting their clocks');
}
