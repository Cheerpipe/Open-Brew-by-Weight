const micraWeb = fs.readFileSync(
    path.join(sketchDir, 'network/OpenBrewByWeightLineaMicraWeb.inc'), 'utf8');
const micraStatus = fs.readFileSync(
    path.join(sketchDir, 'network/OpenBrewByWeightStatus.inc'), 'utf8') + fs.readFileSync(
    path.join(sketchDir, 'network/OpenBrewByWeightSettingsStream.inc'), 'utf8') +
    fs.readFileSync(
        path.join(sketchDir, 'network/OpenBrewByWeightHomeStream.inc'), 'utf8');
const micraService = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightMicraService.cpp'), 'utf8');
const micraTiming = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightMicraTiming.h'), 'utf8');
const micraTypes = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightLineaMicraTypes.h'), 'utf8');
const machineIntegration = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightLineaMicraIntegration.cpp'), 'utf8');
const controlCommands = fs.readFileSync(
    path.join(sketchDir, 'control/OpenBrewByWeightCommands.inc'), 'utf8');
const commandPersistence = fs.readFileSync(
    path.join(sketchDir, 'persistence/OpenBrewByWeightCommandPersistence.inc'), 'utf8');
const scaleEvents = fs.readFileSync(
    path.join(sketchDir, 'scale/OpenBrewByWeightScaleEvents.inc'), 'utf8');
const networkService = fs.readFileSync(
    path.join(sketchDir, 'network/OpenBrewByWeightNetworkService.inc'), 'utf8');
const micraSettingsHtml = rawPartialHtml.settings;
const micraDiagnosticHtml = rawPartialHtml.diagnostic;
{
  const assert = require('assert'), vm = require('vm');
  assert(rawPartialHtml.home.includes('<div class="metric micraOnly"><strong>{{webui:home.machine_power_state}}</strong><div id="homeMicraPower">'));
  const el = () => ({textContent: '', classList: {toggle() {}, contains: () => false}});
  const cleaning = el(), power = el(), rowState = el(), brew = el();
  const brewBoiler = el(), steamBoiler = el();
  const machineRow = {textContent: '', bad: false};
  machineRow.classList = {toggle: (_, on) => { machineRow.bad = on; }};
  let lastStatus = null;
  const rowSlots = {};
  const context = {R: {applyHomeStatus() {}, formatMicraCleaning:()=>'',
      homeStatusCache: () => lastStatus,
      $: id => ({homeMicraCleaning: cleaning, homeMicraPower: power, machineRowState: rowState, state: brew, machineRow,
        homeMicraBrewBoiler: brewBoiler, homeMicraSteamBoiler: steamBoiler}[id] ||
        (/Row(Value|Hint)$|^(scaleWeight|preferredScale|cupWeight)$/.test(id) ? (rowSlots[id] ||= el()) : null))},
    document: {querySelector: () => null}, __LINEA_MICRA__: true,
    Date: {now: () => 60000},
    __WEBUI_TEXT__: key => key === 'home.optimistic' ? 'Optimistic' : 'Unknown'};
  vm.runInNewContext(viewJs.home.replace(/import\s*\*\s*as\s+R\s+from\s*['"][^'"]+['"];?/, '').replace(/export /g, ''), context);
  // The boiler ticker reads the live cache; mirror what the stream apply does.
  const directApply = context.applyStatus;
  context.applyStatus = data => { lastStatus = data; directApply(data); };
  for (const [lineaMicra, expected] of [
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
    context.applyStatus({lineaMicra});
    assert.strictEqual(power.textContent, expected);
  }
  context.applyStatus({lineaMicra: {powerState: 'OFF', quality: 'current'}, state: 'READY'});
  assert.strictEqual(rowState.textContent, 'Turned off');
  assert.strictEqual(machineRow.bad, true);
  context.applyStatus({lineaMicra: {powerState: 'ON', quality: 'current'}, state: 'READY'});
  assert.strictEqual(rowState.textContent, 'Ready');
  assert.strictEqual(machineRow.bad, false);
  context.applyStatus({lineaMicra: {powerState: 'ON', quality: 'current'}, state: 'BREWING'});
  assert.strictEqual(machineRow.bad, true);
  assert.strictEqual(brew.textContent, '');
  // Boiler-readiness lamp: with boiler evidence the machine row claims Ready
  // only when both boilers are ready; otherwise it narrates the warm-up.
  const warming = {machineIntegration: 'linea_micra_cloud', state: 'READY',
    lineaMicra: {accountConfigured: true, observeState: true, powerState: 'ON', quality: 'current'}};
  for (const [readiness, expected, bad] of [
    ['warming_up', 'Heating up', false],
    ['waiting_for_steam', 'Steam heating up', false],
    ['needs_water', 'No water', true],
    ['ready', 'Ready', false],
    ['unknown', 'On', false],
  ]) {
    context.applyStatus({...warming, lineaMicra: {...warming.lineaMicra, readiness}});
    assert.strictEqual(rowState.textContent, expected);
    assert.strictEqual(machineRow.bad, bad);
  }
  // Headline value: minutes left while warming, brew target once ready.
  const boilers = (readiness, coffee) => ({...warming, lineaMicra: {...warming.lineaMicra, readiness, boiler: {coffee}}});
  context.applyStatus(boilers('warming_up', {state: 'heating_up', readyAtUtcSec: 1000, targetDeciC: 930}));
  assert.deepStrictEqual([rowState.textContent, rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent],
      ['Heating up', '16 min', 'Until ready']);
  context.applyStatus(boilers('ready', {state: 'ready', targetDeciC: 930}));
  assert.deepStrictEqual([rowState.textContent, rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent],
      ['Ready', '93°C', 'Brew temperature']);
  context.applyStatus({...warming, lineaMicra: {...warming.lineaMicra, powerState: 'OFF'}});
  assert.deepStrictEqual([rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent], ['—', 'Turn on to brew']);
  context.applyStatus(boilers('needs_water', {state: 'no_water'}));
  assert.deepStrictEqual([rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent], ['—', 'Refill the tank']);
  context.applyStatus({...warming, lineaMicra: {...warming.lineaMicra, powerState: 'OFF', quality: 'optimistic', optimisticOn: true, readiness: 'unknown'}});
  assert.deepStrictEqual([rowState.textContent, rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent], ['On', '—', 'Starting up']);
  context.applyStatus(boilers('unknown', {state: 'standby'}));
  assert.deepStrictEqual([rowSlots.machineRowValue.textContent, rowSlots.machineRowHint.textContent], ['—', '']);
  // Without the integration there is never a reading, so no dash either.
  context.applyStatus({lineaMicra: {powerState: 'OFF', quality: 'current'}, state: 'READY'});
  assert.strictEqual(rowSlots.machineRowValue.textContent, '');
  // Home boiler rows: estimate math (Date.now pinned to 60000 ms),
  // clamping, labels, and configuration gating.
  const frame = (coffee, steam) => ({machineIntegration: 'linea_micra_cloud', state: 'READY',
    lineaMicra: {accountConfigured: true, observeState: true, powerState: 'ON', quality: 'current',
      boiler: {coffee, steam}}});
  context.applyStatus(frame({state: 'heating_up', readyAtUtcSec: 1000},
      {state: 'heating_up', readyAtUtcSec: 1200, level: 'level_3'}));
  assert.strictEqual(brewBoiler.textContent, 'Heating up · ready in ~16 min');
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
  context.applyStatus({lineaMicra: {accountConfigured: false, observeState: false}});
  assert.strictEqual(brewBoiler.textContent, 'Not connected');
  context.applyStatus({lineaMicra: {accountConfigured: true, observeState: false}});
  assert.strictEqual(brewBoiler.textContent, 'Disabled');
  // Without the integration the controller state decides, as before.
  context.applyStatus({lineaMicra: {powerState: 'ON', quality: 'current'}, state: 'READY'});
  assert.strictEqual(rowState.textContent, 'Ready');
  assert(network.includes('delta.field("lineaMicra.powerState"'),
      'Home stream must project Micra power independently of REST status');
  // Cross-side contract: every firmware-emitted stream path must pass the
  // client delta gate (regression: 4-segment boiler paths were rejected and
  // killed the live snapshot on Micra builds).
  const gateSource = rawRuntimeJs.match(
      /\/(\^\[a-zA-Z\]\[a-zA-Z0-9\]\*\(\\\.\[a-zA-Z\]\[a-zA-Z0-9\]\*\)\{0,\d\}\$)\/\.test\(path\)/);
  assert(gateSource, 'client delta path gate must exist');
  const gate = new RegExp(gateSource[1]);
  const streamPaths = [...micraStatus.matchAll(/(?:delta|fields)\.field\("([^"]+)"/g)].map(m => m[1]);
  assert(streamPaths.length > 30, 'stream field extraction found no paths');
  for (const path of streamPaths)
    assert(gate.test(path), `firmware field path rejected by the client gate: ${path}`);
}
{
  const assert = require('assert');
  const vm = require('vm');
  const panelAt = micraDiagnosticHtml.indexOf('id="micraCloudDiagnostics"');
  assert(panelAt >= 0 && panelAt < micraDiagnosticHtml.indexOf('{{webui:diagnostic.misc}}'));
  assert(/<details class="cfgGroup micraOnly" data-g="marzocco-cloud"><summary>[^<]*<\/summary><div id="micraCloudDiagnostics" class="statusColumn">/.test(micraDiagnosticHtml));
  const ids = ['dMicraCloudEmail', 'dMicraCloudMachine', 'dMicraCloudTime',
    'dMicraCloudApi', 'dMicraCloudResult', 'dMicraCloudDuration'];
  const nodes = Object.fromEntries(ids.map(id => [id, {textContent: ''}]));
  const cloudUi = fs.readFileSync(path.join(sketchDir, 'web/js/diagnostic.js'), 'utf8');
  assert(cloudUi.includes('function ensureMicraRows()'));
  // Exercise row creation with a minimal DOM, including repeated status updates.
  const dom = {};
  const element = () => ({children: [], textContent: '',
    set id(value) { this._id = value; dom[value] = this; }, get id() { return this._id; },
    append(...children) { for (const child of children) { child.parentElement = this; this.children.push(child); } },
    insertBefore(child, anchor) { child.parentElement = this; const at = this.children.indexOf(anchor); this.children.splice(at < 0 ? this.children.length : at, 0, child); }});
  const state = element(), modeRow = element(), mode = element(), cloud = element(), heapRow = element(), heap = element();
  mode.id = 'dMicraMode'; cloud.id = 'micraCloudDiagnostics'; heap.id = 'hHeapLargest';
  state.append(modeRow); modeRow.append(mode); heapRow.append(heap);
  // The cleaning row is static markup inside the machine-state fieldset; the
  // synthetic DOM mirrors the generated diagnostic page.
  const machine = element(), cleaningRow = element(), cleaning = element(), cleaningHint = element();
  cleaning.id = 'dMicraCleaning'; cleaningHint.id = 'dMicraCleaningHint';
  machine.append(cleaningRow); cleaningRow.append(cleaning, cleaningHint);
  assert(/<details class="cfgGroup micraOnly" data-g="marzocco-micra"><summary>[^<]*<\/summary><div id="micraMachineDiagnostics" class="statusColumn">/.test(micraDiagnosticHtml));
  assert(micraDiagnosticHtml.includes('{{webui:diagnostic.marzocco_micra}}'));
  for (const id of ['dMicraReadiness', 'dMicraBrewBoiler', 'dMicraSteamBoiler', 'dMicraWater']) {
    assert(micraDiagnosticHtml.includes(`id="${id}"`), `machine section row missing: ${id}`);
  }
  const labels = {'diagnostic.cloud_titles': 'Email|Machine|Time|API|Result|Duration|Connection|Traffic|Planned|Unexpected|Cleaning',
    'diagnostic.cleaning_states': 'Inactive|Waiting for paddle|Cleaning', 'diagnostic.ws_links': 'live|parked|closed',
    // Unit and separator fragments read as the English catalog has them.
    ...Object.fromEntries(['runtime.s_2', 'runtime.ms_2', 'diagnostic.utc_suffix', 'diagnostic.http_prefix',
      'diagnostic.retry_prefix', 'diagnostic.ms_stop', 'diagnostic.bytes_per_s', 'diagnostic.bytes_per_min',
      'diagnostic.bytes_suffix', 'diagnostic.messages_sep', 'diagnostic.errors_suffix',
      'diagnostic.websocket_suffix', 'diagnostic.ws_prefix', 'diagnostic.psram_heap', 'diagnostic.failures_sep']
      .map((key) => [key, localeReference.strings[key]]))};
  const domContext = vm.createContext({$: id => dom[id], document: {createElement: element},
    __WEBUI_TEXT__: key => labels[key] || key, R: {formatWallTimeLocal: String}});
  const cleaningSource = rawRuntimeJs.slice(rawRuntimeJs.indexOf('function formatMicraCleaning('),
      rawRuntimeJs.search(/let\s+shotWs\s*=/)).replace('export ', '');
  domContext.R.formatMicraCleaning = new Function('__WEBUI_TEXT__', cleaningSource + ';return formatMicraCleaning;')(domContext.__WEBUI_TEXT__);
  vm.runInContext(cloudUi.slice(cloudUi.indexOf('function renderMicraCloudDiagnostic('),
      cloudUi.indexOf('function applyScaleCommands(')), domContext);
  const show = lm => { domContext.lm = lm; vm.runInContext('renderMicraCloudDiagnostic(lm)', domContext); };
  const socket = {state: 'streaming', nowMs: 5000, cleaningAvailable: true, cleaning: 'waiting_for_paddle', cleaningAtMs: 3000,
    rxBytes: 120, rxBytesPerSecond: 20, rxBytesPerMinute: 100, messages: 2,
    plannedConnections: 7, unexpectedConnections: 3};
  const linked = {accountConfigured: true, observeState: true, connectionType: 'websocket', websocket: socket};
  show(linked); show(linked);
  assert.strictEqual(cloud.children.length, 10, 'Status refresh must not duplicate dynamic rows');
  assert.strictEqual(dom.dMicraWsPlanned.textContent, '7');
  assert.strictEqual(dom.dMicraWsUnexpected.textContent, '3');
  assert.strictEqual(machine.children.length, 1, 'Cleaning stays a static row of the machine section');
  assert.strictEqual(dom.dMicraCleaning.textContent, 'Waiting for paddle');
  assert(dom.dMicraCleaningHint.textContent.includes('2.0 s'));
  assert(!dom.dMicraCleaningHint.textContent.includes('diagnostic.stale'));
  assert(dom.dMicraWsTraffic.textContent.includes('RX 20 B/s · 100 B/60s · 120 B'));
  show({...linked, websocket: {...socket, subscribed: true}});
  assert.strictEqual(dom.dMicraWsState.textContent, 'streaming · live');
  show({...linked, websocket: {...socket, state: 'paused', reason: 'shot', parked: true}});
  assert.strictEqual(dom.dMicraWsState.textContent, 'paused · shot · parked');
  show({...linked, websocket: {...socket, state: 'paused', reason: 'maintenance'}});
  assert.strictEqual(dom.dMicraWsState.textContent, 'paused · maintenance · closed');
  show({...linked, websocket: {...socket, state: 'paused', cleaning: 'unknown', cleaningLabel: '<b>Future</b>'}});
  assert.strictEqual(dom.dMicraCleaning.textContent, 'runtime.unknown_4 · <b>Future</b>');
  assert(dom.dMicraCleaningHint.textContent.includes('diagnostic.stale'));
  show({...linked, connectionType: 'api'});
  assert.strictEqual(dom.dMicraCleaning.textContent, 'diagnostic.cleaning_api');
  assert.strictEqual(dom.dMicraCleaningHint.textContent, '');
  show({...linked, observeState: false});
  assert.strictEqual(dom.dMicraCleaning.textContent, 'runtime.disabled');
  assert.strictEqual(dom.dMicraCleaningHint.textContent, '');
  show({...linked, accountConfigured: false});
  assert.strictEqual(dom.dMicraCleaning.textContent, 'runtime.not_connected');
  assert.strictEqual(dom.dMicraCleaningHint.textContent, '');
  show({...linked, websocket: {}});
  assert.strictEqual(dom.dMicraWsPlanned.textContent, '0');
  assert.strictEqual(dom.dMicraWsUnexpected.textContent, '0');
  assert.strictEqual(dom.dMicraCleaning.textContent, 'diagnostic.cleaning_no_update');
  assert(!dom.dMicraWsHeap.textContent.includes('undefined'));
  assert(dom.dMicraWsHeap.textContent.includes('internal Δ'));
  const context = vm.createContext({$: id => nodes[id],
    __WEBUI_TEXT__: key => key === 'diagnostic.cloud_results' ? ['success','canceled','http_error','transport_error','invalid_response','response_too_large','setup_error'].map(k=>'diagnostic.cloud_'+k).join('|') : labels[key] || key,
    R: {formatWallTimeLocal: String}});
  context.R.formatMicraCleaning = new Function('__WEBUI_TEXT__', cleaningSource + ';return formatMicraCleaning;')(context.__WEBUI_TEXT__);
  vm.runInContext(cloudUi.slice(cloudUi.indexOf('function renderMicraCloudDiagnostic('),
      cloudUi.indexOf('function applyScaleCommands(')), context);
  const render = data => {
    context.lm = data;
    vm.runInContext('renderMicraCloudDiagnostic(lm)', context);
  };
  render({});
  assert.strictEqual(nodes.dMicraCloudEmail.textContent, 'diagnostic.cloud_no_account');
  assert.strictEqual(nodes.dMicraCloudMachine.textContent, 'diagnostic.cloud_no_machine');
  assert.strictEqual(nodes.dMicraCloudTime.textContent, 'diagnostic.cloud_no_call');
  const selected = {accountConfigured: true, email: 'barista@example.test',
    selectedName: '<b>Kitchen Micra</b>', selectedSerial: 'ABC'};
  const call = {api: 'read_dashboard', method: 'GET', startedAtUtcSec: 1790726400,
    result: 'success', durationMs: 0, httpStatus: 200, transportStatus: 0};
  render({...selected, cloudCall: call});
  assert.strictEqual(nodes.dMicraCloudEmail.textContent, selected.email);
  assert.strictEqual(nodes.dMicraCloudMachine.textContent, selected.selectedName,
      'Account and machine text must never be interpreted as HTML');
  assert.strictEqual(nodes.dMicraCloudTime.textContent, '1790726400 UTC');
  assert.strictEqual(nodes.dMicraCloudApi.textContent, 'GET read_dashboard');
  assert.strictEqual(nodes.dMicraCloudResult.textContent, 'diagnostic.cloud_success · HTTP 200');
  assert.strictEqual(nodes.dMicraCloudDuration.textContent, '0 ms');
  for (const result of ['canceled', 'http_error', 'transport_error', 'invalid_response',
    'response_too_large', 'setup_error']) {
    render({...selected, cloudCall: {...call, result, httpStatus: 0, transportStatus: -1}});
    assert.strictEqual(nodes.dMicraCloudResult.textContent, `diagnostic.cloud_${result} · -1`);
  }
  for (const status of [401, 403, 429, 503]) {
    render({...selected, cloudCall: {...call, result: 'http_error', httpStatus: status}});
    assert(nodes.dMicraCloudResult.textContent.endsWith(`HTTP ${status}`));
  }
  render({cloudCall: call});
  assert.strictEqual(nodes.dMicraCloudMachine.textContent, 'diagnostic.cloud_no_machine',
      'Disconnect must clear the selected identity while retaining the last call');
  render({cloudCall: null});
  assert.strictEqual(nodes.dMicraCloudApi.textContent, 'diagnostic.cloud_no_call');
  assert.strictEqual(nodes.dMicraCloudDuration.textContent, 'diagnostic.cloud_no_call');
  assert(!micraStatus.includes('page == StatusPage::Settings ? safeMicraUsername : ""'));
  assert(micraStatus.includes('machineIntegrationCloudCall()'));
  assert(cloudUi.includes('renderMicraCloudDiagnostic(s.lineaMicra)'));
  const complete = micraService.slice(micraService.indexOf('void OpenBrewByWeightMicraService::releaseIoBuffer('),
      micraService.indexOf('void OpenBrewByWeightMicraService::releaseWorkBuffer('));
  assert(complete.includes('serialTraceCategoryf(success ? LogLevel::INFO'));
  assert(complete.includes('transient ? LogLevel::WARNING : LogLevel::ERROR'));
  assert(complete.includes('lineaMicraHttpRetryable(call.httpStatus)'));
  assert(complete.includes('publishedCloudCall_ = call;'));
  assert(complete.includes('call.result = "invalid_response"'));
  assert.strictEqual((micraService.match(/releaseIoBuffer\(ok\)/g) || []).length, 2,
      'Both token responses must be validated before reporting success');
  assert.strictEqual((micraService.match(/releaseIoBuffer\(false\)/g) || []).length, 1,
      'Invalid machine lists and dashboards must be reported as errors');
  assert(micraService.includes('releaseIoBuffer(decoded)'));
  assert(micraService.includes(': overflow ? "response_too_large"') &&
      micraService.includes('canceled, work_->responseOverflow, ok, work_->httpStatus'));
  assert(micraService.includes(': httpStatus != 0 ? "http_error" : "transport_error"'));
}
const deferObservation = micraService.slice(
  micraService.indexOf('void OpenBrewByWeightMicraService::deferObservation('),
  micraService.indexOf('void OpenBrewByWeightMicraService::fail('));
const terminalFailure = micraService.slice(
  micraService.indexOf('void OpenBrewByWeightMicraService::fail('),
  micraService.indexOf('void OpenBrewByWeightMicraService::scheduleAutomatic('));

if (!network.includes('/api/v1/machine/linea-micra') ||
    !micraWeb.includes('UNSUPPORTED_MACHINE') ||
    !micraWeb.includes('jsonHasOnlyUniqueFields') ||
    !micraWeb.includes('MICRA_STA_REQUIRED') ||
    !micraWeb.includes('queueMachineIntegrationConnect') ||
    !micraWeb.includes('queueMachineIntegrationRelist(requestId)') ||
    !micraWeb.includes('selectMachineIntegrationDevice') ||
    !micraWeb.includes('disconnectLineaMicra(candidate)') ||
    !micraWeb.includes('stagedLineaMicra_')) {
  throw new Error('Linea Micra API must be profile-gated, strict, STA-only, staged, and asynchronous');
}
const micraStatusFailures = [
  micraStatus.includes('\"password\"') && 'password exposed',
  micraStatus.includes('\"username\"') && 'username exposed',
  !micraStatus.includes('delta.field("lineaMicra.accountConfigured"') && 'account status missing',
  !micraStatus.includes('fields.field("lineaMicra.email"') && 'account email missing',
  !micraStatus.includes('fields.field("lineaMicra.machines"') && 'machine list missing',
  micraStatus.includes('accountMachineCount') && 'unread account machine count streamed',
  !micraStatus.includes('OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD') && 'compile gate missing',
  !micraStatus.includes('delta.field("machineIntegration"') && 'integration capability missing',
].filter(Boolean);
if (micraStatusFailures.length) {
  throw new Error('Linea Micra status contract: ' + micraStatusFailures.join(', '));
}
if (micraService.includes('save_client_session')) {
  throw new Error('Async Micra client must not resume TLS sessions: ESP-IDF re-applies them per handshake step');
}
if (!micraService.includes('esp_log_level_set("transport_ws", ESP_LOG_NONE);')) {
  throw new Error('SDK Upgrade write errors must not log signed authentication headers');
}
const micraPublishAt = networkService.indexOf(
    'publishMachineIntegrationNetworkState(');
const micraNetworkState = micraService.slice(
    micraService.indexOf('void OpenBrewByWeightMicraService::publishNetworkState('),
    micraService.indexOf('bool OpenBrewByWeightMicraService::queueConnect('));
if (!micraService.includes('config.is_async = true;') ||
    !micraService.includes('while (performed == ESP_ERR_HTTP_EAGAIN)') ||
    !micraService.includes('(void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));') ||
    !micraService.includes('millis() - requestStartedAtMs') ||
    micraPublishAt < 0 ||
    !micraNetworkState.includes('abortRequested_.store(true,') ||
    !micraNetworkState.includes('xTaskNotifyGive(task_)') ||
    micraNetworkState.includes('esp_http_client_') ||
    networkService.includes('serviceMachineIntegrationAbort') ||
    micraService.includes('esp_http_client_cancel_request') ||
    micraService.includes('activeClient_') || micraService.includes('clientMux_')) {
  throw new Error(
      'Linea Micra HTTPS cancellation must wake its sole owner without mutating live TLS from another task');
}
if (!micraTiming.includes('kStatePollMs = 30000') ||
    !micraTiming.includes('kStateFreshnessMs = kStatePollMs') ||
    !micraTiming.includes('kOptimisticOverlayMs = 2U * kStatePollMs') ||
    !micraTiming.includes('kExhaustedCooldownMs = kOptimisticOverlayMs') ||
    !micraTiming.includes('kPostWakeObservationDelayMs = 15000') ||
    !micraService.includes('const bool networkReady = networkEligible(observationGate)') ||
    !micraService.includes('pending_.present && networkReady && !localActivity &&') ||
    !micraService.includes('(!pendingObservation || observationReady)') ||
    !micraService.includes('observationSchedule_.armPostEvent(now)') ||
    !micraService.includes('? powerState_.noteStandbyCommandAccepted(published_,') ||
    !micraService.includes(': powerState_.notePowerOnCommandAccepted(published_,') ||
    !micraService.includes('observationSchedule_.armPostEvent(millis())') ||
    !micraService.includes('deferObservation(pending, status, gateError)') ||
    !micraService.includes('observationActive_.load(std::memory_order_acquire)') ||
    !micraService.includes('bool OpenBrewByWeightMicraService::observationCurrent(') ||
    !micraService.includes('pending_.request.type == LineaMicraRequestType::OBSERVE_STATE) {') ||
    !micraService.includes('!config_.accountConfigured ||\n      (config_.options & LINEA_MICRA_OBSERVE_STATE) == 0 ||') ||
    !micraService.includes('scheduleAutomatic(millis(), connecting)')) {
  throw new Error('Linea Micra observations must wait for readiness and the post-wake convergence deadline');
}
if (!micraStatus.includes('delta.field("lineaMicra.optimisticOn"') ||
    !micraStatus.includes('delta.field("lineaMicra.optimisticOff"') ||
    !deferObservation.includes('powerState_.hold(published_, true, millis());') ||
    deferObservation.includes('published_.powerState =') ||
    !micraService.includes('status.phase = LineaMicraPhase::BACKOFF;\n    publish(status);') ||
    !terminalFailure.includes('status.powerState = LineaMicraPowerState::UNKNOWN;') ||
    !terminalFailure.includes('status.effectiveOn = true;') ||
    !terminalFailure.includes('status.quality = LineaMicraObservationQuality::COMMUNICATION_ERROR;')) {
  throw new Error('Pending, retry, and canceled observations must preserve power until terminal failure publishes UNKNOWN');
}
if (!micraTypes.includes('APPLY_TEMPERATURE') ||
    !machineIntegration.includes('requestMachineIntegrationPresetTemperature') ||
    !commandPersistence.includes('requestActivePresetMachineTemperature()') ||
    !controlCommands.includes('applyMachineTemperature') ||
    !scaleEvents.includes('requestActivePresetMachineTemperature()') ||
    !networkService.includes('scaleConnecting_.load(std::memory_order_acquire)') ||
    !micraService.includes('CoffeeMachineSettingCoffeeBoilerTargetTemperature') ||
    !micraService.includes('{\\\"boilerIndex\\\":1,\\\"targetTemperature\\\":%u.%u}') ||
    !micraService.includes('verification.targetDeciC == request.targetDeciC') ||
    !micraService.includes('commandAccepted = desiredTemperature_.commandAccepted;') ||
    !micraService.includes('desiredTemperature_.commandAccepted = true;') ||
    !micraService.includes('lineaMicraHttpRetryable') ||
    !micraService.includes('lineaMicraTemperatureCycleRetryable') ||
    !micraService.includes('observationFence_.merge(published_, powerState_, update') ||
    !micraService.includes('desiredTemperature_.machineConfigGeneration = configGeneration;') ||
    !micraService.includes('abortRequested_.load(std::memory_order_acquire)') ||
    !micraService.includes('refreshToken(settings) || signIn(settings)') ||
    !micraService.includes('char authorization[kTokenCapacity + 8] = {};') ||
    !micraService.includes('snprintf(authorization, kTokenCapacity + 8,') ||
    !micraService.includes('esp_http_client_set_header(work_->client, "Authorization",\n                                               authorization) == ESP_OK') ||
    !micraService.includes('secureWipe(authorization, kTokenCapacity + 8);') ||
    micraService.includes('Bearer %s", work_->accessToken);\n    const bool ok = length > 0 &&\n                    static_cast<size_t>(length) < sizeof(io_->response)') ||
    !micraStatus.includes('delta.field("lineaMicra.temperatureState"') ||
    !micraStatus.includes('delta.field("lineaMicra.temperatureError"')) {
  throw new Error('Linea Micra preset temperature must coalesce, gate on scale/shot activity, reuse authentication, and confirm dashboard readback');
}
for (const id of ['lineaMicraSignIn', 'lineaMicraBusy', 'lineaMicraSteps', 'lineaMicraLegendHint',
  'lineaMicraChoose', 'lineaMicraConnected',
  'lineaMicraUsername', 'lineaMicraPassword', 'lineaMicraWifiNote', 'lineaMicraProgress',
  'lineaMicraConnectButton', 'lineaMicraMachines', 'lineaMicraSelectButton',
  'lineaMicraCancelButton', 'lineaMicraPowerDeps', 'lineaMicraLinkedNote',
  'lineaMicraApplyTemperature', 'lineaMicraObserveState',
  'lineaMicraBoostScaleDetectionWhenOn', 'lineaMicraReduceScaleScanningWhenOff',
  'lineaMicraRecognizeWake', 'lineaMicraShutdownWithScale',
  'lineaMicraShutdownGraceWrap', 'lineaMicraShutdownGrace',
  'lineaMicraDisconnectButton', 'lineaMicraChangeButton', 'lineaMicraBrewTargetC']) {
  if (!micraSettingsHtml.includes(`id="${id}"`)) {
    throw new Error(`Linea Micra settings control is missing: ${id}`);
  }
}
{
  const assert = require('assert'), vm = require('vm');
  const nodes = new Map();
  const context = {__LINEA_MICRA__: true, Date: {now: () => 3600100}, controlsMutable: true,
    $: id => {
      if (id === 'lineaMicraRefreshLink') return null;
      if (!nodes.has(id)) nodes.set(id, {textContent: ''});
      return nodes.get(id);
    }, __WEBUI_TEXT__: key => key === 'runtime.s_ago' ? ' s ago'
      : key === 'diagnostic.ready_in' ? 'ready in %1'
      : ['diagnostic.deg_c', 'diagnostic.utc_close', 'diagnostic.minutes_short', 'diagnostic.s',
        'diagnostic.level_word'].includes(key) ? localeReference.strings[key] : key};
  context.formatWallTimeLocal = seconds => 'T' + seconds;
  context.R = context;
  vm.createContext(context);
  vm.runInContext(blockAt(rawViewJs.diagnostic, 'function renderLineaMicraDiagnostic('), context);
  for (const policy of ['connection', 'time', undefined]) {
    for (const quality of ['current', 'stale', 'optimistic', 'unsupported']) {
      context.micraDiagnosticStatus = {sampleValid: true, sampleAgeMs: 0, receivedAtMs: 100,
        freshnessMs: 30000, freshnessPolicy: policy, powerState: 'OFF', quality};
      vm.runInContext('renderLineaMicraDiagnostic()', context);
      assert.strictEqual(nodes.get('dMicraPowerValue').textContent, 'OFF');
      assert.strictEqual(nodes.get('dMicraQuality').textContent,
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
  context.micraDiagnosticStatus = linked;
  vm.runInContext('renderLineaMicraDiagnostic()', context);
  assert.strictEqual(nodes.get('dMicraReadiness').textContent, 'waiting_for_steam');
  assert.strictEqual(nodes.get('dMicraBrewBoiler').textContent, 'ready · 93.5 °C · 4 s ago');
  assert.strictEqual(nodes.get('dMicraSteamBoiler').textContent,
      'heating up · level 3 · ready in 2m 0s (T3720 UTC) · 4 s ago');
  assert.strictEqual(nodes.get('dMicraWater').textContent, 'diagnostic.water_states'.split('|')[0]);
  context.micraDiagnosticStatus = {...linked,
    boiler: {...linked.boiler, coffee: {...linked.boiler.coffee, state: 'no_water'}}};
  vm.runInContext('renderLineaMicraDiagnostic()', context);
  assert.strictEqual(nodes.get('dMicraWater').textContent, 'diagnostic.water_states'.split('|')[1]);
  context.micraDiagnosticStatus = {accountConfigured: true, observeState: false};
  vm.runInContext('renderLineaMicraDiagnostic()', context);
  assert.strictEqual(nodes.get('dMicraReadiness').textContent, 'runtime.disabled');
  assert.strictEqual(nodes.get('dMicraWater').textContent, 'runtime.disabled');
  assert(micraStatus.includes('micraPowerSourceName(micra.powerSource)'));
  assert(micraStatus.includes('micra.connectionFreshness ? "connection" : "time"'));
  assert(network.includes('delta.field("lineaMicra.readiness"'),
      'Home stream must project the derived Micra readiness');
  assert(network.includes('delta.field("lineaMicra.boiler.coffee.state"') &&
         network.includes('delta.field("lineaMicra.boiler.steam.readyAtUtcSec"'),
      'Home stream must project the boiler states and estimates');
}
for (const id of ['dMicraPower', 'dMicraPowerValue', 'dMicraMode',
  'dMicraQuality', 'dMicraAge', 'lineaMicraRefreshLink']) {
  if (!micraDiagnosticHtml.includes(`id="${id}"`)) {
    throw new Error(`Linea Micra diagnostic control is missing: ${id}`);
  }
}
{
  const assert = require('assert');
  const vm = require('vm');
  const binding = viewJs.settings.match(/\$\(\s*['"]lineaMicraDisconnectButton['"]\s*\)\s*\.\s*onclick\s*=([\s\S]*?);\s*for\s*\(/);
  assert(binding, 'Micra Disconnect click binding is missing');
  let calls = 0, confirmed = false;
  const click = vm.runInNewContext(`(${binding[1]})`, {
    confirm: () => confirmed,
    __WEBUI_TEXT__: key => key,
    disconnectLineaMicra: () => { calls++; },
  });
  click();
  assert.strictEqual(calls, 0, 'Cancel must leave the account connected');
  confirmed = true;
  click();
  assert.strictEqual(calls, 1, 'Confirmed Disconnect must invoke the action');

  // The Micra settings code lives in the Settings module; its runtime and
  // shared-state references resolve to this harness's globals.
  const statusUi = rawViewJs.settings.slice(rawViewJs.settings.search(/const\s+MICRA_SWITCHES\s*=/),
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
  get('lineaMicraPowerDeps').children = ['RecognizeWake', 'ScaleOffWithMachine', 'BoostScaleDetectionWhenOn',
    'ReduceScaleScanningWhenOff'].map(id => get('lineaMicra' + id));
  const commands = [];
  let actionOk = true, polls = 0;
  get('lineaMicraSteps').children = [node(), node(), node()];
  const steps = () => get('lineaMicraSteps').children.map(li => li.className).join();
  const context = vm.createContext({__LINEA_MICRA__: true, $: get, controlsMutable: true,
    micraDirty: false, micraSignIn: null, micraChange: false,
    refreshStatus: () => {}, setTimeout: () => { polls++; return 1; },
    lineaMicraAction: async action => { commands.push({action}); return actionOk; },
    document: {documentElement: {classList: {toggle() {}}}, createElement() {
      const label = node();
      Object.defineProperty(label, 'innerHTML', {set() {
        label.radio = node(); label.firstChild = label.radio; label.b = node(); label.small = node();
      }});
      return label;
    }},
    command: async (url, body) => { commands.push({url, body: JSON.parse(JSON.stringify(body))}); return true; },
    __WEBUI_TEXT__: key => key});
  vm.runInContext(statusUi, context);
  const apply = lm => vm.runInContext(`applyLineaMicraStatus(${JSON.stringify({lineaMicra: lm})})`, context);
  const panes = () => ['SignIn', 'Busy', 'Choose', 'Connected'].filter(p => !get('lineaMicra' + p).hidden).join();
  const flush = () => new Promise(resolve => setImmediate(resolve));
  const run = async () => {
    const base = {accountConfigured: false, email: '', selectedName: '', selectedSerial: '', machines: [],
      phase: 'disabled', error: 'none', staConnected: true, apActive: false, observeState: true,
      applyTemperature: true, recognizeWakeGesture: true, shutdownWithScale: true, shutdownGraceSeconds: 15,
      scaleOffWithMachine: true, boostScaleDetectionWhenOn: true, reduceScaleScanningWhenOff: true,
      powerOnWithScale: false};
    apply(base);
    assert.strictEqual(panes(), 'SignIn', 'Only the sign-in form shows before an account is connected');
    assert.strictEqual(get('lineaMicraProgress').textContent, '');
    apply({...base, apActive: true});
    assert(!get('lineaMicraWifiNote').classes.has('hidden') && get('lineaMicraConnectButton').disabled,
        'Sign-in must explain and wait while the setup network is open');
    assert.strictEqual(get('lineaMicraLegendHint').textContent, 'runtime.not_connected');
    apply({...base, phase: 'authenticating'});
    assert.strictEqual(panes(), 'Busy', 'A running sign-in replaces the form with its progress steps');
    assert.strictEqual(steps(), 'on,,');
    assert.strictEqual(get('lineaMicraLegendHint').textContent, 'runtime.micra_state_connecting');
    assert.strictEqual(polls, 0, 'Sign-in progress arrives on the Settings stream; nothing polls');
    apply({...base, phase: 'listing'});
    assert.strictEqual(steps(), 'ok,on,', 'Finished steps turn green as the next one starts');
    apply({...base, phase: 'failed', error: 'invalid_auth'});
    assert.strictEqual(panes(), 'SignIn');
    assert(!get('lineaMicraConnectButton').classes.has('busy'));
    assert.strictEqual(get('lineaMicraProgress').textContent, 'runtime.micra_error_auth',
        'A rejected sign-in must read in plain language, never as an error code');
    assert(get('lineaMicraProgress').classes.has('error'));
    apply({...base, phase: 'failed', error: 'canceled'});
    assert.strictEqual(get('lineaMicraProgress').textContent, '', 'Routine cancellations stay silent');

    // A lone listed machine is saved with the shown options, whoever signed in.
    const lone = {...base, phase: 'confirmed', machines: [{serial: 'ABC', name: 'Micra'}]};
    apply(lone);
    await flush();
    assert.deepStrictEqual(commands[0], {url: '/api/v1/machine/linea-micra', body: {
      action: 'select', serial: 'ABC', applyTemperature: true, observeState: true,
      boostScaleDetectionWhenOn: true, reduceScaleScanningWhenOff: true,
      recognizeWakeGesture: true, powerOnWithScale: false, shutdownWithScale: true,
      scaleOffWithMachine: true, connectionType: 'websocket', shutdownGraceSeconds: 15}});
    apply(lone);
    assert.strictEqual(panes(), 'Busy', 'The lone machine is linked without a choice screen');
    assert.strictEqual(steps(), 'ok,ok,on');
    assert.strictEqual(commands.length, 1, 'The automatic selection is sent once');

    // A failed automatic selection falls back to the choice, without retrying.
    context.micraSignIn = 'failed';
    apply(lone);
    assert.strictEqual(panes(), 'Choose');
    context.micraSignIn = 'pending';
    apply({...lone, machines: [{serial: 'ABC', name: 'Micra'}, {serial: 'DEF', name: ''}]});
    assert.strictEqual(panes(), 'Choose', 'Several machines need an explicit choice');
    assert.strictEqual(get('lineaMicraLegendHint').textContent, 'runtime.micra_state_choose');
    assert.strictEqual(commands.length, 1);
    const machines = get('lineaMicraMachines').children;
    assert.deepStrictEqual(machines.map(m => [m.radio.value, m.b.textContent, m.small.textContent]),
        [['ABC', 'Micra', 'ABC'], ['DEF', 'DEF', 'DEF']]);
    assert(get('lineaMicraSelectButton').disabled, 'Use this machine waits for a choice');

    const connected = {...base, accountConfigured: true, email: 'user@example.test', selectedName: 'Micra',
      selectedSerial: 'ABC', phase: 'idle', powerState: 'ON', boiler: {coffee: {targetDeciC: 935}}};
    apply(connected);
    assert.strictEqual(panes(), 'Connected');
    assert.strictEqual(context.micraSignIn, null);
    assert.strictEqual(get('lineaMicraIdentity').innerText, 'Micra · ABC');
    assert.strictEqual(get('lineaMicraAccount').innerText, 'user@example.test');
    assert.strictEqual(get('lineaMicraStatus').textContent, 'runtime.micra_power_on · runtime.target 93.5 °C');
    assert.strictEqual(get('lineaMicraStatus').dataset.tone, 'on', 'An ON machine shows a green state pill');
    const pill = lm => { apply({...connected, ...lm}); const p = get('lineaMicraStatus'); return [p.textContent, p.dataset.tone]; };
    assert.deepStrictEqual(pill({readiness: 'ready'}), ['runtime.micra_pill_ready · runtime.target 93.5 °C', 'on']);
    assert.deepStrictEqual(pill({readiness: 'warming_up'}), ['runtime.micra_pill_heating · runtime.target 93.5 °C', 'warn']);
    assert.deepStrictEqual(pill({readiness: 'needs_water'}), ['runtime.micra_pill_no_water', 'error']);
    assert.deepStrictEqual(pill({powerState: 'OFF'}), ['runtime.micra_power_standby', ''], 'Standby stays neutral');
    assert.deepStrictEqual(pill({machineOffline: true}), ['runtime.micra_pill_offline', 'warn']);
    assert.deepStrictEqual(pill({error: 'communication_error'}), ['runtime.micra_pill_cloud', 'warn']);
    assert.strictEqual(get('lineaMicraStatus').title, 'runtime.micra_error_cloud', 'The pill explains the problem on hover');
    assert.deepStrictEqual(pill({error: 'invalid_auth'}), ['runtime.micra_pill_account', 'error']);
    assert.deepStrictEqual(pill({apActive: true}), ['runtime.ap_mode_paused', 'warn']);
    assert.deepStrictEqual(pill({observeState: false}), ['runtime.micra_pill_monitoring_off', '']);
    assert.deepStrictEqual(pill({shotPaused: true}), ['runtime.micra_power_on · runtime.target 93.5 °C · runtime.shot_paused', 'on']);
    apply(connected);
    context.micraDirty = true;
    get('lineaMicraObserveState').checked = false;
    get('lineaMicraApplyTemperature').checked = false;
    apply(connected);
    assert.strictEqual(get('lineaMicraApplyTemperature').checked, false, 'Status must not overwrite unsaved edits');
    assert(get('lineaMicraPowerDeps').classes.has('fieldOff') && get('lineaMicraRecognizeWake').disabled,
        'Options that need the power state follow Show power state');
    const all = vm.runInContext('micraPayload()', context);
    assert.strictEqual(all.applyTemperature, false);
    assert.strictEqual(all.shutdownGraceSeconds, 15);
    await vm.runInContext('saveLineaMicraSettings("save", "", {applyTemperature: false, observeState: false})', context);
    assert.deepStrictEqual(commands[1].body, {action: 'save', applyTemperature: false, observeState: false},
      'Autosave sends only the changed options, keeping the saved transport, without credentials');
    context.micraDirty = false;
    get('lineaMicraShutdownWithScale').checked = false;
    vm.runInContext('updateMicraDependentControls()', context);
    assert(get('lineaMicraShutdownGraceWrap').classes.has('hidden') && get('lineaMicraShutdownGrace').disabled);

    // Change machine is always offered; the account is listed only on demand.
    const handler = id => vm.runInContext('(' + rawViewJs.settings.match(
        new RegExp(`\\$\\("${id}"\\)\\.onclick = ([\\s\\S]*?);\\n\\s+if \\(\\$\\(`))[1]
        .replace(/\bR\.(?:uiState\.)?/g, '') + ')', context);
    commands.length = 0;
    apply({...connected, accountMachineCount: 0});
    apply({...connected, accountMachineCount: 1});
    assert.deepStrictEqual(commands, [], 'Opening Settings never lists the account on its own');
    assert(!get('lineaMicraChangeButton').hidden && !get('lineaMicraChangeButton').disabled);
    await handler('lineaMicraChangeButton')();
    apply({...connected, machineListError: 'none', machines: [{serial: 'ABC', name: 'Micra'}]});
    assert.strictEqual(panes(), 'Choose', 'A single-machine account still sees its machine listed');
    handler('lineaMicraCancelButton')();
    assert.strictEqual(panes(), 'Connected');
    const several = {...connected, accountMachineCount: 2, machineListError: 'none'},
      listed = {...several, machines: [{serial: 'ABC', name: 'Micra'}, {serial: 'DEF', name: 'Other'}]};
    apply(several);
    assert(!get('lineaMicraChangeButton').hidden && !get('lineaMicraChangeButton').disabled);
    await handler('lineaMicraChangeButton')();
    assert.deepStrictEqual(commands.at(-1), {action: 'relist'});
    apply({...several, machineListing: true});
    assert.strictEqual(panes(), 'Choose', 'Change machine reuses the choice screen');
    assert.strictEqual(get('lineaMicraProgress').textContent, 'runtime.micra_progress_listing');
    apply({...several, machineListError: 'invalid_auth'});
    assert.strictEqual(get('lineaMicraProgress').textContent, 'runtime.micra_error_auth');
    assert(get('lineaMicraProgress').classes.has('error'));
    apply(listed);
    assert.strictEqual(get('lineaMicraProgress').textContent, '');
    handler('lineaMicraCancelButton')();
    assert.strictEqual(panes(), 'Connected', 'Cancel returns to the saved machine');
    apply(listed);
    assert.strictEqual(panes(), 'Connected');
    assert(!commands.some(c => c.action === 'disconnect'), 'Cancel never disconnects the account');
    actionOk = false;
    await handler('lineaMicraChangeButton')();
    assert.strictEqual(panes(), 'Connected', 'A refused listing keeps the saved machine on screen');
    actionOk = true;
    await handler('lineaMicraChangeButton')();
    apply(listed);
    await vm.runInContext('selectLineaMicra("DEF")', context);
    assert.deepStrictEqual([commands.at(-1).body.action, commands.at(-1).body.serial], ['select', 'DEF']);
    assert.strictEqual(panes(), 'Connected', 'The chosen machine replaces the saved one in place');
    apply({...connected, accountConfigured: false});
    assert.strictEqual(context.micraChange, false);
  };
  run().catch(error => {console.error(error); process.exitCode = 1;});
}
if (!rawCss.includes('html:not(.lineaMicraIntegration) .micraOnly') ||
    !rawCss.includes('.micraDeps{') ||
    !rawCss.includes('[type=email]') ||
    !micraDiagnosticHtml.includes('<span id="dMicraPowerValue">{{webui:diagnostic.unknown}}</span> - <a id="lineaMicraRefreshLink" href="#" aria-disabled="true" tabindex="-1">({{webui:diagnostic.refresh_state}})</a>') ||
    micraDiagnosticHtml.includes('id="lineaMicraRefreshButton"') ||
    !codeIncludes(viewJs.diagnostic, "e.preventDefault();R.lineaMicraAction('refresh')") ||
    !codeIncludes(rawRuntimeJs, "s.machineIntegration==='linea_micra_cloud'") ||
    !codeIncludes(rawAllJs, "{action:'connect',username,password}") ||
    !codeIncludes(rawAllJs, "lineaMicraAction('disconnect')") ||
    codeIncludes(rawRuntimeJs, "['queued','authenticating','listing','running','backoff'].includes(m.phase)") ||
    codeIncludes(rawAllJs, 'lineaMicraConnectionType') ||
    codeIncludes(rawRuntimeJs, 'lineaMicraSaveButton') ||
    !codeIncludes(viewJs.settings, "el.closest('#lineaMicraSignIn,#lineaMicraChoose')") ||
    micraSettingsHtml.indexOf('id="lineaMicraSettings"') > micraSettingsHtml.indexOf('{{webui:settings.shared_settings}}') ||
    !micraSettingsHtml.includes('<option value="15" selected>') ||
    micraSettingsHtml.includes('id="lineaMicraPowerOnWithScale" type="checkbox" checked') ||
    !codeIncludes(rawViewJs.diagnostic, "$('dMicraPowerValue').textContent=power") ||
    !codeIncludes(rawViewJs.diagnostic, "$('dMicraAge').textContent=lm.temperatureState+'/'+lm.temperatureError+' · '") ||
    !codeIncludes(rawViewJs.diagnostic, "refresh.setAttribute('aria-disabled',String(disabled))") ||
    codeIncludes(rawViewJs.diagnostic, "expired?'UNKNOWN':lm.powerState") ||
    !codeIncludes(rawViewJs.diagnostic, 'power=lm.powerState') ||
    !codeIncludes(rawViewJs.diagnostic, "stale=lm.freshnessPolicy!=='connection'&&lm.quality==='current'&&age>=lm.freshnessMs") ||
    ['ApplyTemperature', 'ObserveState', 'RecognizeWake', 'ShutdownWithScale', 'ScaleOffWithMachine',
      'BoostScaleDetectionWhenOn', 'ReduceScaleScanningWhenOff'].some(id =>
      !micraSettingsHtml.includes(`id="lineaMicra${id}" type="checkbox" checked`))) {
  throw new Error('Linea Micra UI must implement account connection, selection, and quality-only freshness expiry');
}
if (micraService.includes('keep_alive_enable = true') ||
    (micraService.match(/esp_http_client_close\(work_->client\)/g) || []).length !== 1 ||
    !micraService.includes('pendingCloudCall_.idleMs >= micra_timing::kHttpReuseIdleMs') ||
    micraService.includes('char authorization[kTokenCapacity + 8];') ||
    (micraService.match(/"Accept",\s*"application\/json"/g) || []).length !== 1 ||
    (micraService.match(/"User-Agent",\s*\n?\s*"OpenBrewByWeight\/1"/g) || []).length !== 1 ||
    !micraService.includes('RequestStateGuard requestState{*this};') ||
    !micraService.includes('~RequestStateGuard() { service.clearRequestState(); }') ||
    !micraService.includes('esp_http_client_delete_header(work_->client, "Authorization")') ||
    !micraService.includes('esp_http_client_delete_header(work_->client, "X-Request-Proof")') ||
    !micraService.includes('esp_http_client_set_post_field(work_->client, nullptr, 0)') ||
    !micraService.includes('config.buffer_size = 4096;') ||
    !micraService.includes('config.buffer_size_tx = 4096;') ||
    !micraService.includes('struct OpenBrewByWeightMicraService::IoBuffer') ||
    !micraService.includes('union {') ||
    !micraService.includes('allocExternal(sizeof(WorkBuffer), AllocationOwner::NETWORK)') ||
    !micraService.includes('allocExternal(sizeof(IoBuffer), AllocationOwner::NETWORK)') ||
    !micraService.includes('if (connecting) releaseWorkBuffer();') ||
    !micraService.includes('if (!staConnected || apActive) {') ||
    !micraService.includes('releaseIoBuffer();') ||
    !micraService.includes('const bool networkReady = networkEligible(observationGate)') ||
    !micraService.includes('} else if (observationReady && config_.accountConfigured &&') ||
    !micraService.includes('const bool dashboardRequired = status.sampleAtMs == 0 ||') ||
    !micraService.includes('(sessionRenewed && !dashboardRequired) ||')) {
  throw new Error('Linea Micra polling must bound transient PSRAM/stack use, release idle sessions, wait for STA, reuse active HTTP sessions, and avoid continuous probes');
}
for (const file of ['OpenBrewByWeightMachinePaddleControl.h',
  'OpenBrewByWeightMachinePaddleInput.h', 'OpenBrewByWeightMachinePaddlePolicy.h',
  'OpenBrewByWeightMachinePaddleState.h']) {
  if (fs.readFileSync(path.join(sketchDir, file), 'utf8').includes('LineaMicra')) {
    throw new Error(`Paddle behavior must remain independent of Linea Micra: ${file}`);
  }
}

const micraScaleShutdown = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightMicraScaleShutdown.h'), 'utf8');
const micraScalePowerOn = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightMicraScalePowerOn.h'), 'utf8');
const micraMachinePower = fs.readFileSync(
    path.join(sketchDir, 'machine/OpenBrewByWeightMicraMachinePower.h'), 'utf8');
const entrypoints = fs.readFileSync(
    path.join(sketchDir, 'platform/OpenBrewByWeightEntrypoints.inc'), 'utf8');
const protocolGeneric = fs.readFileSync(
    path.join(sketchDir, '..', 'libraries', 'EspressoScaleBLE', 'src',
              'protocols', 'GenericFf11.cpp'),
    'utf8');
if (!micraScaleShutdown.includes('LINEA_MICRA_SCALE_EXPLICIT_DISCONNECT = 9') ||
    !micraScaleShutdown.includes('!scale.relayClosed') ||
    !micraScaleShutdown.includes('LINEA_MICRA_SHUTDOWN_WITH_SCALE') ||
    !micraScaleShutdown.includes('lineaMicraShutdownGraceSeconds') ||
    !micraService.includes('CoffeeMachineChangeMode') ||
    !micraService.includes('{\\"mode\\":\\"StandBy\\"}') ||
    !micraService.includes('bool OpenBrewByWeightMicraService::executePowerApplication(') ||
    !micraService.includes('void OpenBrewByWeightMicraService::deferPower(') ||
    !micraService.includes('deferPower(request, LineaMicraError::CANCELED, 0, false)') ||
    !micraService.includes('uint8_t powerOptionBit(LineaMicraRequestType type)') ||
    !micraService.includes('? LINEA_MICRA_POWER_ON_WITH_SCALE') ||
    !machineIntegration.includes('void serviceMachineIntegrationScaleLink(') ||
    !machineIntegration.includes('MicraScaleShutdownTracker') ||
    !entrypoints.includes('serviceMachineIntegrationScaleLink(') ||
    !micraWeb.includes('"shutdownWithScale"') ||
    !micraWeb.includes('"shutdownGraceSeconds"') ||
    !micraStatus.includes('fields.field("lineaMicra.shutdownWithScale"') ||
    !micraStatus.includes('fields.field("lineaMicra.shutdownGraceSeconds"')) {
  throw new Error('Scale power-off shutdown must be machine-side, explicit-disconnect only, relay-guarded, grace-cancelled, and cloud-gated');
}
if (!micraScalePowerOn.includes('LINEA_MICRA_POWER_ON_WITH_SCALE') ||
    !micraScalePowerOn.includes('LINEA_MICRA_SCALE_EXPLICIT_DISCONNECT') ||
    !micraScalePowerOn.includes('scale.relayClosed') ||
    !micraService.includes('SET_POWER_ON') ||
    !micraService.includes('{\\"mode\\":\\"BrewingMode\\"}') ||
    !machineIntegration.includes('MicraScalePowerOnTracker') ||
    !machineIntegration.includes('LineaMicraRequestType::SET_POWER_ON') ||
    !micraWeb.includes('"powerOnWithScale"') ||
    !micraStatus.includes('fields.field("lineaMicra.powerOnWithScale"')) {
  throw new Error('Scale power-on must fire only after the scale\'s own power-off, stay relay-guarded, and send BrewingMode through the cloud');
}
if (!micraMachinePower.includes('LINEA_MICRA_SCALE_OFF_WITH_MACHINE') ||
    !micraMachinePower.includes('LineaMicraObservationQuality::CURRENT') ||
    !machineIntegration.includes('MicraMachinePowerTracker') ||
    !machineIntegration.includes('requestScalePowerOff()') ||
    !machineIntegration.includes('machineEffectivelyOff()') ||
    !machineIntegration.includes('Scale power-off skipped: connected scale does not support power-off') ||
    !entrypoints.includes('serviceMachineIntegrationMachinePower(') ||
    !entrypoints.includes('loopScaleLink.features.has(ScaleFeaturePowerOff)') ||
    !scaleWorker.includes('void requestScalePowerOff()') ||
    !scaleWorker.includes('executeScalePowerOffCommand') ||
    !protocolGeneric.includes('ScaleFeaturePowerOff') ||
    !micraWeb.includes('"scaleOffWithMachine"') ||
    !micraStatus.includes('fields.field("lineaMicra.scaleOffWithMachine"') ||
    !micraWeb.includes('"boostScaleDetectionWhenOn"') ||
    !micraWeb.includes('"reduceScaleScanningWhenOff"') ||
    !micraStatus.includes('fields.field("lineaMicra.boostScaleDetectionWhenOn"') ||
    !micraStatus.includes('fields.field("lineaMicra.reduceScaleScanningWhenOff"')) {
  throw new Error('Scale-off-with-machine must fire once per confirmed ON→OFF edge, warn instead of writing to unsupported scales, and never re-trigger the shutdown cycle');
}

{
  // The per-loop machine-power service is the only production seam between
  // the resolved Micra state and the scale worker's discovery duty; it must
  // publish the override before the scale power-off early return.
  const integration = fs.readFileSync(
      path.join(sketchDir, 'machine', 'OpenBrewByWeightLineaMicraIntegration.cpp'), 'utf8');
  const serviceStart = integration.indexOf('void serviceMachineIntegrationMachinePower(');
  const seam = integration.slice(serviceStart, integration.indexOf('\n}', serviceStart));
  const earlyReturn = seam.indexOf('machinePower.service(');
  if (serviceStart < 0 || earlyReturn < 0 ||
      !seam.includes('const LineaMicraStatus status = service.statusPoll();') ||
      seam.indexOf('micraScanOverride(') < 0 ||
      seam.indexOf('applyLiveBleScanOverride(') > earlyReturn) {
    throw new Error(
        'serviceMachineIntegrationMachinePower must publish micraScanOverride via applyLiveBleScanOverride before the scale power-off early return');
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
if (!codeIncludes(viewJs.home, 'if(!document.hidden&&R.activeView===home)renderMicraBoilers()') ||
    !codeIncludes(viewJs.diagnostic, 'if(!document.hidden&&R.activeView===diagnostic)renderDiagClock()')) {
  throw new Error('Hidden Home and Diagnostic views must not keep painting their clocks');
}
