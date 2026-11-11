{
  const assert = require('assert').strict, vm = require('vm');
  const events = {}, buttons = ['wifi', 'bluetooth'].map(kind => ({
    dataset: {}, attributes: {'aria-controls': kind + 'Details', 'aria-expanded': 'false'},
    setAttribute(name, value) { this.attributes[name] = value; },
    getAttribute(name) { return this.attributes[name]; },
    addEventListener(name, fn) { this[name] = fn; }, focus() { this.focused = true; },
  }));
  const companionButton = {dataset: {}, hidden: true, attributes: {},
    setAttribute(name, value) { this.attributes[name] = value; },
    getAttribute(name) { return this.attributes[name]; }};
  const laMarzoccoButton = {dataset: {}, hidden: true, attributes: {},
    setAttribute(name, value) { this.attributes[name] = value; },
    getAttribute(name) { return this.attributes[name]; }};
  const elements = Object.fromEntries([...['wifi', 'bluetooth'].flatMap((kind, i) => [
    [kind + 'Signal', buttons[i]], [kind + 'Details', {hidden: true}],
    [kind + 'Name', {}], [kind + 'Detail', {}],
  ]), ['companionSignal', companionButton], ['companionName', {}], ['companionDetail', {}], ['companionRelay', {}],
    ['laMarzoccoSignal', laMarzoccoButton], ['laMarzoccoName', {}], ['laMarzoccoSelected', {}], ['laMarzoccoDetail', {}],
    ['laMarzoccoRoute', {textContent: '—'}]]);
  const start = runtimeJs.indexOf('function updateHeaderSignals(');
  assert(start >= 0, 'Shared header must render cached signal status');
  const context = vm.createContext({$: id => elements[id], __LA_MARZOCCO__: true,
    document: {querySelectorAll: () => buttons, querySelector: () => null,
      addEventListener: (name, fn) => {events[name] = fn;}},
    window: {addEventListener: (name, fn) => {events[name] = fn;}},
  });
  const regionEnd = runtimeJs.slice(start).search(/\nlet\s+homeBootDone\s*=/);
  vm.runInContext(runtimeStateDecl + runtimeJs.match(/const sub\s*=[^;]+;/)[0] + runtimeJs.slice(start, regionEnd < 0 ? undefined : regionEnd + start).replace(/^export /gm, ''), context);
  for (const [signal, level] of [[1, '1'], [2, '2'], [3, '3'], [null, 'unknown'], [NaN, 'unknown'], [Infinity, 'unknown'], [-1, 'unknown'], [0, 'unknown'], [4, 'unknown'], [1.5, 'unknown'], ['3', 'unknown']]) {
    context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: signal, bluetoothConnected: true, bluetoothLevel: signal}});
    for (const kind of ['wifi', 'bluetooth']) assert.equal(elements[kind + 'Signal'].dataset.level, level, String(signal));
  }
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3, bluetoothConnected: false, bluetoothLevel: 3}});
  assert.equal(elements.wifiSignal.dataset.level, '3');
  assert.equal(elements.bluetoothSignal.dataset.level, '0', 'Disconnect overrides an old reading');
  assert.equal(elements.bluetoothSignal.attributes['aria-label'], 'Scale disconnected');
  context.updateHeaderSignals({connections: {wifiConnected: false, wifiLevel: 3}});
  assert.equal(elements.wifiSignal.dataset.level, '0', 'STA disconnected does not imply a strong AP signal');
  context.updateHeaderSignals({snapshotStale: true, connections: {wifiConnected: true, wifiLevel: 3, bluetoothConnected: true, bluetoothLevel: 3}});
  assert.equal(elements.wifiSignal.dataset.level, 'unknown');
  assert.equal(elements.bluetoothSignal.dataset.level, 'unknown');
  context.updateHeaderSignals();
  assert.equal(elements.wifiSignal.title, 'Wi-Fi: Signal unavailable', 'Transport failures clear the displayed measurement');
  assert.equal(elements.bluetoothSignal.title, 'Scale signal unavailable');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3, bluetoothConnected: true, bluetoothLevel: 2}});
  assert.equal(elements.wifiSignal.title, 'Wi-Fi: Strong');
  assert.equal(elements.bluetoothSignal.title, 'Scale signal: Medium');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: null, wifiName: 'Cafe "A"\\B', bluetoothConnected: true, bluetoothLevel: 3, bluetoothName: '<img src=x onerror=alert(1)>'}});
  assert.equal(elements.wifiName.textContent, 'Cafe "A"\\B', 'Connected names survive missing signal level');
  assert.equal(elements.bluetoothName.textContent, '<img src=x onerror=alert(1)>', 'Identity is rendered as text');
  assert.equal(elements.wifiDetail.textContent, 'Signal unavailable');
  // The companion indicator exists only while a companion is paired; an open
  // link inks it and anything else grays it like the radios' OFF states.
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}});
  assert.equal(elements.companionSignal.hidden, true, 'No companion status shows nothing');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, companion: {paired: false, link: 'down'}});
  assert.equal(elements.companionSignal.hidden, true, 'An unpaired companion shows nothing');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, companion: {paired: true, link: 'down', name: 'nas', peerProtocol: 0}});
  assert.equal(elements.companionSignal.hidden, false, 'A paired companion shows even while its link is down');
  assert.equal(elements.companionSignal.dataset.level, 'off');
  assert.equal(elements.companionSignal.attributes['aria-label'], 'Companion: Link: Not connected \u00b7 Relay: Not used by this page');
  assert.equal(elements.companionName.textContent, 'nas', 'The paired name stays while the link is down');
  assert.equal(elements.companionDetail.textContent, 'Not connected');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, companion: {paired: true, link: 'down', name: 'nas', peerProtocol: 2}});
  assert.equal(elements.companionDetail.textContent,
      'Not connected · the companion and this controller use different link versions; update the older one',
      'A refused link version names the connection problem');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, companion: {paired: true, link: 'open', name: 'nas'}});
  assert.equal(elements.companionSignal.dataset.level, 'on');
  assert.equal(elements.companionSignal.attributes['aria-label'], 'Companion: Link: Connected \u00b7 Relay: Not used by this page');
  vm.runInContext('uiState.companionHere = {paired: true}', context);
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, companion: {paired: true, link: 'down', name: 'nas'}});
  assert.equal(elements.companionSignal.dataset.level, 'partial', 'A page served by the companion keeps one rack lit while the link is down');
  assert.equal(elements.companionSignal.attributes['aria-label'], 'Companion: Link: Not connected \u00b7 Relay: Serving this page');
  assert.equal(elements.companionDetail.textContent, 'Not connected', 'The popup keeps the link row');
  assert.equal(elements.companionRelay.textContent, 'Serving this page', 'The popup has a relay row beside the link row');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, companion: {paired: true, link: 'open', name: 'nas'}});
  assert.equal(elements.companionSignal.dataset.level, 'on', 'An open link lights both racks even through the companion');
  context.updateHeaderSignals({snapshotStale: true, connections: {}, companion: {paired: true, link: 'down', name: 'nas'}});
  assert.equal(elements.companionSignal.dataset.level, 'off', 'A stale snapshot grays the companion even when it serves the page');
  vm.runInContext('uiState.companionHere = null', context);
  context.updateHeaderSignals({snapshotStale: true, connections: {}, companion: {paired: true, link: 'open', name: 'nas'}});
  assert.equal(elements.companionSignal.dataset.level, 'off', 'A stale snapshot grays the paired companion');
  assert.equal(elements.companionSignal.hidden, false, 'A stale snapshot does not unpair the companion');
  assert.equal(elements.companionDetail.textContent, 'Signal unavailable');
  assert.equal(elements.companionName.textContent, 'nas', 'The stale server keeps its last known name');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, companion: {paired: true, link: 'open', name: 'nas'}});
  assert.equal(elements.companionSignal.dataset.level, 'on', 'A fresh frame re-inks the open link');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3, bluetoothConnected: true, bluetoothLevel: 2}});
  assert.equal(elements.companionSignal.dataset.level, 'on', 'Frames without companion changes keep the indicator');
  // The La Marzocco cloud indicator exists only on the micra integration and
  // follows the feed's observation quality: live inks, everything else grays.
  const laMarzoccoFrame = (laMarzocco, companion) => context.updateHeaderSignals(
    {connections: {wifiConnected: true, wifiLevel: 3}, machineIntegration: 'la_marzocco_cloud', laMarzocco, companion});
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}});
  assert.equal(elements.laMarzoccoSignal.hidden, true, 'Other integrations show no cloud icon');
  laMarzoccoFrame({quality: 'current', email: 'barista@example.test', selectedName: 'Kitchen Micra'});
  assert.equal(elements.laMarzoccoSignal.hidden, false, 'The micra integration shows the cloud icon');
  assert.equal(elements.laMarzoccoSignal.dataset.level, 'on');
  assert.equal(elements.laMarzoccoSignal.attributes['aria-label'], 'La Marzocco cloud: Connected');
  assert.equal(elements.laMarzoccoName.textContent, 'barista@example.test');
  assert.equal(elements.laMarzoccoSelected.textContent, 'Kitchen Micra');
  assert.equal(elements.laMarzoccoDetail.textContent, 'Connected');
  // The route row names where the machine's data runs, with Admin's wording;
  // a frame without a companion model keeps the last route it named.
  assert.equal(elements.laMarzoccoRoute.textContent, '—', 'Before any companion model the row holds its placeholder');
  laMarzoccoFrame({quality: 'current'}, {route: 'direct'});
  assert.equal(elements.laMarzoccoRoute.textContent, 'Directly from the controller');
  laMarzoccoFrame({quality: 'current'}, {route: 'companion'});
  assert.equal(elements.laMarzoccoRoute.textContent, 'Through the companion');
  laMarzoccoFrame({quality: 'current'}, {route: 'pending'});
  assert.equal(elements.laMarzoccoRoute.textContent, 'Moving to the companion');
  laMarzoccoFrame({quality: 'current'});
  assert.equal(elements.laMarzoccoRoute.textContent, 'Moving to the companion',
      'A companion-less frame keeps the last route');
  laMarzoccoFrame({quality: 'optimistic', email: 'barista@example.test', selectedName: 'Kitchen Micra'});
  assert.equal(elements.laMarzoccoSignal.dataset.level, 'on', 'A commanded wake-up the cloud has yet to confirm stays inked');
  for (const [quality, label] of [['stale', 'Connection problems'],
      ['communication_error', 'Connection problems'], ['unconfigured', 'Not connected'],
      ['disabled', 'Disabled'], ['unsupported', 'Connection problems']]) {
    laMarzoccoFrame({quality, email: '', selectedName: ''});
    assert.equal(elements.laMarzoccoSignal.dataset.level, 'off', quality);
    assert.equal(elements.laMarzoccoDetail.textContent, label, quality);
  }
  assert.equal(elements.laMarzoccoName.textContent, 'No account saved', 'No account says so');
  assert.equal(elements.laMarzoccoSelected.textContent, 'No machine selected', 'No machine says so');
  context.updateHeaderSignals({snapshotStale: true, connections: {}, machineIntegration: 'la_marzocco_cloud',
    laMarzocco: {quality: 'current', email: 'barista@example.test', selectedName: 'Kitchen Micra'}});
  assert.equal(elements.laMarzoccoSignal.dataset.level, 'off', 'A stale snapshot grays the cloud icon');
  assert.equal(elements.laMarzoccoDetail.textContent, 'Signal unavailable');
  assert.equal(elements.laMarzoccoName.textContent, 'barista@example.test', 'The stale account keeps its last known email');
  laMarzoccoFrame({quality: 'current'});
  assert.equal(elements.laMarzoccoSignal.dataset.level, 'on', 'A fresh frame re-inks the cloud icon');
  // The Admin poll names the integration but carries no micra model; it must
  // leave the indicator on its last Home frame instead of graying it.
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, machineIntegration: 'la_marzocco_cloud'});
  assert.equal(elements.laMarzoccoSignal.dataset.level, 'on', 'A model-less envelope keeps the cloud icon');
  assert.equal(elements.laMarzoccoDetail.textContent, 'Connected');
  // Without a companion the one cloud only turns on and off, whatever the
  // route; a paired companion adds the state that names a direct route.
  laMarzoccoFrame({quality: 'current'}, {paired: false, route: 'direct'});
  assert.equal(elements.laMarzoccoSignal.dataset.paired, 'false');
  assert.equal(elements.laMarzoccoSignal.dataset.level, 'on', 'No companion: a live link is fully on');
  assert.equal(elements.laMarzoccoSignal.attributes['aria-label'], 'La Marzocco cloud: Connected');
  for (const [route, level, name] of [['direct', 'partial', 'Directly from the controller'],
      ['pending', 'partial', 'Moving to the companion'], ['companion', 'on', 'Through the companion']]) {
    laMarzoccoFrame({quality: 'current'}, {paired: true, route});
    assert.equal(elements.laMarzoccoSignal.dataset.paired, 'true');
    assert.equal(elements.laMarzoccoSignal.dataset.level, level, route);
    assert.equal(elements.laMarzoccoSignal.attributes['aria-label'], 'La Marzocco cloud: Connected · ' + name);
  }
  laMarzoccoFrame({quality: 'current'}, {paired: true, route: 'direct'});
  laMarzoccoFrame({quality: 'current'});
  assert.equal(elements.laMarzoccoSignal.dataset.level, 'partial', 'A companion-less frame keeps the pairing and route');
  laMarzoccoFrame({quality: 'stale'}, {paired: true, route: 'direct'});
  assert.equal(elements.laMarzoccoSignal.dataset.level, 'off', 'A paired companion does not light a dead feed');
  context.updateHeaderSignals({snapshotStale: true, connections: {}, machineIntegration: 'la_marzocco_cloud',
    laMarzocco: {quality: 'current'}, companion: {paired: false}});
  assert.equal(elements.laMarzoccoSignal.dataset.paired, 'true', 'A stale snapshot keeps the last known pairing');
  assert.equal(elements.laMarzoccoSignal.dataset.level, 'off');
  laMarzoccoFrame({quality: 'current'}, {paired: false, route: 'direct'});
  assert.equal(elements.laMarzoccoSignal.dataset.level, 'on', 'Unpairing returns to the one cloud');
  // The scale battery rides the scale button under one level rule; the
  // header draws the cell, the percent goes to the popover, the label and,
  // only with this device's setting, the text beside the cell.
  for (const [pct, level] of [[100, 'ok'], [16, 'ok'], [15, 'low'], [11, 'low'], [10, 'critical'],
      [6, 'critical'], [5, 'empty'], [0, 'empty'], [null, null], [undefined, null], [-1, null],
      [101, null], [1.5, null], ['50', null], [NaN, null]])
    assert.equal(context.scaleBatteryLevel(pct), level, String(pct));
  const attrs = () => ({attrs: {}, setAttribute(name, value) { this.attrs[name] = value; }});
  const fill = attrs(), glyph = attrs();
  Object.assign(elements, {bluetoothBatteryFill: fill, bluetoothBatteryLine: {hidden: true}, bluetoothBattery: {}, bluetoothBatteryPct: {}});
  buttons[1].querySelector = selector => (assert.equal(selector, 'svg'), glyph);
  const battery = (batteryPct, extra) => context.updateHeaderSignals(
    {scale: {available: true, batteryPct}, connections: {bluetoothLevel: 3}, ...extra});
  battery(62);
  assert.equal(buttons[1].dataset.batt, 'ok');
  assert.equal(glyph.attrs.viewBox, '0 0 51.2 24', 'The cell widens the glyph');
  assert.equal(fill.attrs.height, '6.08');
  assert.equal(fill.attrs.y, '10.82', 'The fill rises from the cell floor');
  assert.equal(elements.bluetoothBatteryPct.textContent, '62%');
  assert.equal(buttons[1].attributes['aria-label'], 'Scale signal: Strong · battery 62%');
  assert.equal(elements.bluetoothBatteryLine.hidden, false);
  assert.equal(elements.bluetoothBattery.textContent, '62%');
  battery(12);
  assert.equal(buttons[1].dataset.batt, 'low');
  assert.equal(elements.bluetoothBattery.textContent, '12% · low');
  assert.equal(buttons[1].attributes['aria-label'], 'Scale signal: Strong · battery 12% · low');
  battery(9);
  assert.equal(buttons[1].dataset.batt, 'critical');
  battery(0);
  assert.equal(buttons[1].dataset.batt, 'empty');
  assert.equal(fill.attrs.height, '1.40', 'An empty cell keeps a visible sliver');
  assert.equal(elements.bluetoothBattery.textContent, '0% · almost empty');
  context.updateHeaderSignals({connections: {bluetoothConnected: true, bluetoothLevel: 3}});
  assert.equal(buttons[1].dataset.batt, 'empty', 'An envelope without a scale model (Admin) keeps the cell');
  assert.equal(buttons[1].attributes['aria-label'], 'Scale signal: Strong · battery 0% · almost empty');
  for (const hidden of [() => battery(null), () => battery(40, {snapshotStale: true}),
      () => context.updateHeaderSignals({scale: {available: false, batteryPct: 40}, connections: {bluetoothLevel: 3}})]) {
    battery(40);
    hidden();
    assert.equal(buttons[1].dataset.batt, undefined, 'No reading, a stale frame or no scale hides the cell');
    assert.equal(glyph.attrs.viewBox, '0 0 34.5 24');
    assert.equal(elements.bluetoothBatteryLine.hidden, true);
    assert(!buttons[1].attributes['aria-label'].includes('battery'));
  }
  assert(rawShellHtml.includes('id="bluetoothBatteryFill" class="battFill"') &&
      rawShellHtml.includes('id="bluetoothBatteryLine" hidden'), 'The cell and its popover line ship hidden');
  assert(css.includes('#bluetoothSignal[data-batt=low] :is(.battGlyph,.battPct){color:var(--lw)}') &&
      css.includes('#bluetoothSignal:is([data-batt=critical],[data-batt=empty]) :is(.battGlyph,.battPct){color:var(--live)}'),
      'Only the cell and its percent take the level colour');
  assert(/\.battGlyph,\.battPct\{display:none\}/.test(css) &&
      css.includes('.showBattPct #bluetoothSignal[data-batt] .battPct{display:inline}'),
      'The percent text stays hidden until this device opts in');
  assert(/prefers-reduced-motion:reduce\)\{#bluetoothSignal\[data-batt=empty\] \.battFill\{animation:none\}/.test(css),
      'The empty blink stops under reduced motion');
  context.initHeaderSignals();
  buttons[0].click();
  assert.equal(elements.wifiDetails.hidden, false);
  assert.equal(elements.bluetoothDetails.hidden, true);
  assert.equal(buttons[0].attributes['aria-expanded'], 'true');
  events.click({target: {closest: () => elements.wifiDetails}});
  assert.equal(elements.wifiDetails.hidden, false, 'Clicks inside the popup retain it');
  buttons[1].click();
  assert.equal(elements.wifiDetails.hidden, true);
  assert.equal(buttons[0].attributes['aria-expanded'], 'false');
  assert.equal(elements.bluetoothDetails.hidden, false, 'Each icon opens its own popup');
  context.updateHeaderSignals({connections: {bluetoothConnected: false, bluetoothName: 'Old scale', bluetoothLevel: 3}});
  assert.equal(elements.bluetoothName.textContent, 'None');
  assert.equal(elements.bluetoothDetail.textContent, 'Scale disconnected', 'Open popup refreshes on disconnect');
  let prevented = 0;
  events.keydown({key: 'Escape', preventDefault: () => ++prevented});
  assert.equal(elements.bluetoothDetails.hidden, true);
  assert.equal(buttons[1].focused, true, 'Escape returns focus to the trigger');
  events.keydown({key: 'Escape', preventDefault: () => ++prevented});
  assert.equal(prevented, 1, 'Only an Escape that closed a popover is consumed, so the live view stays open');
  buttons[0].click();buttons[0].click();
  assert.equal(elements.wifiDetails.hidden, true, 'Repeated tap closes the popup');
  buttons[0].click();events.click({target: {closest: () => null}});
  assert.equal(elements.wifiDetails.hidden, true, 'Outside click closes');
  buttons[0].click();events.popstate();
  assert.equal(elements.wifiDetails.hidden, true, 'History navigation closes');
  context.updateHeaderSignals({snapshotStale: true, connections: {wifiConnected: true, wifiName: 'Old SSID'}});
  assert.equal(elements.wifiName.textContent, '—');
  assert.equal(elements.wifiDetail.textContent, 'Signal unavailable');
  assert(codeIncludes(appJsSource, 'R.initHeaderSignals();'));
  assert(codeIncludes(rawRuntimeJs, 'applyCommonStatus(s){if(s.connections||s.snapshotStale)updateHeaderSignals(s);'),
      'record-only envelopes must not erase live header signals');
  assert(codeIncludes(rawRuntimeJs, 'noteReachFail(err){clearCupWeights();updateHeaderSignals();'));
  // Record pages take their header signals from the Home stream on the same socket.
  assert(!fs.readFileSync(path.join(sketchDir, 'diagnostics/OpenBrewByWeightNetworkDiagnostics.inc'), 'utf8').includes('appendRecordPageUi'));
  for (const file of ['network/OpenBrewByWeightStatus.inc']) {
    const source = fs.readFileSync(path.join(sketchDir, file), 'utf8');
    for (const field of ['wifiConnected', 'wifiLevel', 'bluetoothConnected', 'bluetoothLevel', 'wifiName', 'bluetoothName']) assert(source.includes('\\"' + field + '\\"'), file + ': ' + field);
    assert(source.includes('escapeConnectedScaleName(control, g_work->scaleHistory,'), 'Both envelopes use the connected identity resolver');
    assert(source.includes('escapeJsonString(network.staState == StaState::CONNECTED ? network.iface.staSsid : ""'), 'Preserve escaped SSID only while connected');
  }
  const network = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightNetwork.cpp'), 'utf8');
  assert(network.replace(/\s+/g, ' ').includes('entry.friendlyName[0] ? entry.friendlyName : scaleDefaultFriendlyName(entry.name)'), 'Friendly name precedes the advertised BLE name and its derived default');
  assert(network.includes('preferredScaleMacEqual(entry.mac, control.connectedScaleMac)'), 'Name must match the connected scale');
}
// The brand drops its text only while the measured row would wrap, and every
// decision starts from the restored lockup so it can recover on its own.
{
  const assert = require('assert').strict, vm = require('vm');
  const doc = {dataset: {}, style: {setProperty: (name, value) => {if (name === '--barH') doc.barH = value;}}}, reads = [];
  const el = (width) => ({getBoundingClientRect() {reads.push(width); return {width};}});
  const bar = {clientWidth: 120, offsetHeight: 96,
    querySelector: (s) => s === '.brand' ? el(40) : s === '.headerSignals' ? el(90) : null};
  const context = vm.createContext({document: {querySelector: () => bar, documentElement: doc},
    getComputedStyle: () => ({paddingLeft: '10px', paddingRight: '10px', columnGap: '4px'})});
  vm.runInContext(blockAt(runtimeJs, 'function updateBrandFit('), context);
  doc.dataset.brandCompact = '1';
  context.updateBrandFit();
  assert.deepEqual(reads, [40, 90], 'fit is measured with the text restored');
  assert.equal(doc.dataset.brandCompact, '1', '40 + 4 + 90 does not fit 100 available');
  assert.equal(doc.barH, '96px', 'the measured bar height anchors the sticky message banner');
  bar.clientWidth = 200;
  context.updateBrandFit();
  assert.equal(doc.dataset.brandCompact, undefined, 'a fitting row brings the text back');
  assert(codeIncludes(appJsSource, 'R.updateBrandFit();'),
      'resize, fonts and breakpoint changes re-measure the brand fit');
}
{
  const assert = require('assert').strict, vm = require('vm'), hidden = new Set(['hidden']);
  const link = {classList: {toggle: (name, on) => on ? hidden.add(name) : hidden.delete(name)}};
  const context = vm.createContext({compatMode: false,
    document: {querySelector: selector => {assert.equal(selector, '[data-route="/diagnostic"]'); return link;}}});
  const start = rawRuntimeJs.search(/let\s+diagnosticVisible\s*=\s*false\s*;/);
  vm.runInContext(rawRuntimeJs.slice(start, rawRuntimeJs.indexOf('function applyCompatibilityChrome(', start))
    .replace(/^export /gm, ''), context);
  context.applyDiagnosticNavigation();
  assert(hidden.has('hidden'), 'Diagnostic stays hidden before configuration arrives');
  context.applyDiagnosticNavigation({diagnosticPageVisible: true});
  assert(!hidden.has('hidden'), 'Admin enables the shared mobile/desktop Diagnostic link');
  context.applyDiagnosticNavigation({});
  assert(!hidden.has('hidden'), 'Unrelated status envelopes preserve visibility');
  context.compatMode = true; context.applyDiagnosticNavigation();
  assert(hidden.has('hidden'), 'Compatibility mode hides Diagnostic even when enabled');
  context.compatMode = false; context.applyDiagnosticNavigation();
  assert(!hidden.has('hidden'), 'Returning from compatibility mode restores saved visibility');
  context.applyDiagnosticNavigation({diagnosticPageVisible: false});
  assert(hidden.has('hidden'), 'Admin disables the same link without leaving a second copy');
  assert.equal((rawShellHtml.match(/data-route="\/diagnostic"/g) || []).length, 1);
  const nav = rawShellHtml.match(/<nav class="pageNav"[\s\S]*?<\/nav>/)[0];
  assert.equal((nav.match(/aria-hidden="true"/g) || []).length, 6, 'Every icon retains a translated text label');
  assert(nav.includes('data-route="/diagnostic" class="hidden"'), 'Hidden until device configuration is known');

  // Scrolling no longer restyles the header: phones hold their compact size
  // and desktops keep the full bar pinned and opaque over the content.
  assert(!css.includes('--header-progress') && !css.includes('--compact'),
      'The phone header keeps a fixed compact size with no scroll-driven shrink');
  assert(!css.includes('data-scrolled') && !css.includes('--menu-offset'),
      'The desktop header stays pinned and opaque with no scroll-driven float');
}
{
  const assert = require('assert').strict, vm = require('vm');
  const geometry = {}, header = {}, root = {dataset: {}, style: {setProperty: (name, value) => {geometry[name] = value;}}}, app = {clientWidth: 760}, callbacks = {};
  const links = [42, 34, 48, 60, 78, 46].map(width => ({
    hidden: false, suppressed: false, width, textContent: 'Section', offsetLeft: 2, offsetWidth: 130, offsetTop: 2, offsetHeight: 44,
  }));
  const nav = {offsetTop: 72, setAttribute() {}, appended: [], classes: [], append(el) {this.appended.push(el);},
    listeners: {}, addEventListener(name, fn) {this.listeners[name] = fn;},
    classList: {add: name => nav.classes.push(name)},
    querySelector: selector => {assert.equal(selector, 'a.active'); return links[0];},
    querySelectorAll: () => links,
    get clientWidth() {return app.clientWidth - 2;},
    get scrollWidth() {
      const required = 4 + links.filter(link => !link.hidden && !link.suppressed)
        .reduce((width, link) => width + link.width + 16 + (root.dataset.navLayout === 'icons' ? 32 : 0), 0);
      return Math.max(this.clientWidth, required);
    },
  };
  const mobile = {matches: false, addEventListener: (name, fn) => {
    assert.equal(name, 'change'); callbacks.breakpoint = fn;
  }};
  const svg = () => ({attrs: {}, setAttribute(name, value) {this.attrs[name] = value;}, appendChild: child => child});
  const still = {matches: false};
  const context = vm.createContext({root, nav, mobile, still, __WEBUI_TEXT__: key => key,
    R: {updateBrandFit() {}},
    document: {documentElement: root, createElementNS: (ns, tag) => (assert.equal(ns, 'http://www.w3.org/2000/svg'), svg()),
      querySelector: selector => {assert.equal(selector, '.topBar'); return header;}, getElementById: id => {assert.equal(id, 'app'); return app;},
      fonts: {ready: {then: fn => {callbacks.fonts = fn;}}}},
    requestAnimationFrame() {},
    ResizeObserver: class {constructor(fn) {this.fn = fn;} observe(target) {
      assert(target === app || target === nav || links.includes(target));
      callbacks[target === app ? 'resize' : 'pill'] = this.fn;
    }},
    MutationObserver: class {constructor(fn) {callbacks.visibility = fn;} observe(target, options) {
      assert.equal(target, nav); assert.equal(options.subtree, true);
      assert.deepEqual(Array.from(options.attributeFilter), ['class', 'hidden']);
    }},
  });
  const start = appJsSource.indexOf('function updateNavigationLayout()');
  const msgElAt = appJsSource.slice(start).search(/const\s+msgEl\s*=/);
  vm.runInContext(appJsSource.slice(start, msgElAt < 0 ? undefined : msgElAt + start), context);
  assert.equal(root.dataset.navLayout, 'icons', 'Wide header retains section icons and names');
  assert.deepEqual(nav.appended.map(el => el.attrs.class), ['pill'], 'Script enhancement adds the liquid selection pill');
  assert.deepEqual(nav.classes, ['pillNav'], 'Pill navigation takes over the active background');
  assert.deepEqual(Object.keys(nav.listeners).sort(),
    ['click', 'pointercancel', 'pointerdown', 'pointermove', 'pointerup'],
    'The pill can be dragged between sections');
  const goo = () => Array.from(vm.runInContext('[goo.c, goo.w, goo.tc]', context));
  callbacks.fonts();
  assert.deepEqual(goo(), [67, 130, 67], 'Font settling re-anchors the pill instantly on the active destination');
  assert.match(vm.runInContext('pillPath.attrs.d', context), /^M19\.8,2\.8(Q[^Q]+){32}$/, 'The pill is drawn as one closed shape');
  links[0].offsetWidth = 0; context.placePill(false);
  assert.deepEqual(goo(), [67, 130, 67], 'An unlaid-out active link keeps the last drawn pill');
  links[0].offsetWidth = 130; links[0].offsetLeft = 525; context.placePill(true);
  assert.deepEqual(goo(), [67, 130, 590], 'Animated placement springs toward the active destination');
  still.matches = true; context.placePill(true);
  assert.deepEqual(goo(), [590, 130, 590], 'Reduced motion places the pill without the liquid travel');
  for (const [width, expected] of [[602, 'icons'], [601, 'text'], [410, 'text'], [409, 'bottom'], [760, 'icons']]) {
    app.clientWidth = width; callbacks.resize();
    assert.equal(root.dataset.navLayout, expected, 'No wrapping at width ' + width);
  }
  app.clientWidth = 390; callbacks.resize();
  assert.equal(root.dataset.navLayout, 'bottom');
  links[4].suppressed = true; callbacks.visibility();
  assert.equal(root.dataset.navLayout, 'text', 'Hidden Diagnostics frees room without a second navigation');
  links.forEach((link, index) => {link.suppressed = index !== 5;}); callbacks.visibility();
  assert.equal(root.dataset.navLayout, 'icons', 'Compatibility visibility measures only the Admin link');
  links.forEach(link => {link.suppressed = false;}); callbacks.visibility();
  assert.equal(root.dataset.navLayout, 'bottom', 'Restoring permissions restores the width requirement');
  links[4].hidden = true; callbacks.visibility();
  assert.equal(root.dataset.navLayout, 'text', 'The hidden attribute also removes a destination from measurement');
  links[0].width += 100; callbacks.fonts();
  assert.equal(root.dataset.navLayout, 'bottom', 'Changed font metrics are measured again');
  links[0].width -= 100;
  for (const width of [320, 390, 440, 699]) {
    app.clientWidth = width; mobile.matches = true; callbacks.breakpoint();
    assert.equal(root.dataset.navLayout, 'bottom', 'Phone width ' + width + ' always uses bottom icons');
    links.forEach((link, index) => {link.suppressed = index !== 5;}); callbacks.visibility();
    assert.equal(root.dataset.navLayout, 'bottom', 'Only Admin visible still uses bottom icons');
    callbacks.fonts(); callbacks.resize();
    assert.equal(root.dataset.navLayout, 'bottom', 'Font and resize events preserve phone navigation');
    links.forEach(link => {link.suppressed = false;});
  }
  app.clientWidth = 760; mobile.matches = false; callbacks.breakpoint();
  assert.equal(root.dataset.navLayout, 'icons', 'Leaving the mobile breakpoint restores header navigation');
  app.clientWidth = 440; callbacks.resize();
  assert.equal(root.dataset.navLayout, 'text', 'A narrow desktop content area still measures header text');
  assert(css.includes('bottom:var(--navOffset)'), 'Save and action bars follow the navigation clearance');
  assert(css.includes('position:sticky;top:0'), 'The whole header pins at the top over the scrolling content');
  assert(css.includes('.topBar::before') && css.includes('bottom:calc(var(--tap) / 2)'),
      'The painted header ends at the menu\u2019s middle; its lower half floats over the content');
  assert(css.includes('#message{position:sticky;top:var(--barH,var(--hdr))'),
      'The message banner sticks under the measured header height, clear of the menu');
}
// A disconnected scale icon dims its drawing like the Wi-Fi arcs and carries
// no slash, which only added visual noise next to the dimmed bars.
if (!css.includes('#bluetoothSignal[data-level="0"] .scaleGlyph{opacity:.12}') ||
    /#bluetoothSignal[^{]*svg>g/.test(css) || /id="bluetoothSignal"[^]*?signalSlash[^]*?<\/button>/.test(html)) {
  throw new Error('The disconnected scale icon must dim as a whole without a slash');
}
// The companion and cloud icons need the radios' OFF dimming too: the muted
// color alone still reads as active in dark mode.
if (!css.includes('#companionSignal[data-level=off] svg,#laMarzoccoSignal[data-level=off] svg{opacity:.12}')) {
  throw new Error('The disconnected companion and cloud icons must dim like the radios');
}
// The menu opens a page fresh; Back and Forward return it as it was left.
{
  const assert = require('assert').strict, vm = require('vm');
  const views = {
    diagnostic: [{tagName: 'DETAILS', open: false}, {tagName: 'DETAILS', open: true}],
    settings: [{tagName: 'DETAILS', open: true}],
    home: [{tagName: 'BUTTON', ariaExpanded: 'true', click() { this.ariaExpanded = 'false'; }}],
  };
  const history = {state: {y: 9}, pushed: [],
    replaceState(state) { this.state = state; },
    pushState(state, _, url) { this.pushed.push(url); this.state = state; location.pathname = url; }};
  const location = {pathname: '/diagnostic'}, scrolled = [];
  const context = vm.createContext({history, location, scrollY: 300, activeView: 'diagnostic',
    knownPath: p => p, scrollTo: (_, y) => scrolled.push(y),
    renderRoute: p => { context.activeView = p.slice(1) || 'home'; },
    document: {querySelectorAll: s => views[s.match(/#view-(\w+)/)[1]]}});
  const start = appJsSource.indexOf('const pageFolds');
  vm.runInContext(appJsSource.slice(start, appJsSource.indexOf('history.scrollRestoration', start))
    .replace(/^const /m, 'var '), context);
  context.navigate('/settings');
  assert.equal(JSON.stringify(history.state), '{}', 'a menu entry starts with nothing to restore');
  assert.equal(views.settings[0].open, false, 'the menu closes the groups of the page it opens');
  assert.deepEqual(scrolled, [], 'renderRoute scrolls a page that was hidden');
  history.state = {y: 300, open: [1]};
  context.showFolds('diagnostic', history.state.open);
  assert.equal(views.diagnostic[1].open, true, 'Back reopens what the page had open');
  context.activeView = 'diagnostic'; location.pathname = '/diagnostic';
  context.scrollY = 120; history.state = {};
  context.rememberPage();
  assert.equal(JSON.stringify(history.state), '{"y":120,"open":[1]}', 'each entry records its scroll and folds');
  context.navigate('/diagnostic');
  assert.equal(history.pushed.length, 1, 'the page already showing adds no history entry');
  assert.equal(views.diagnostic[1].open, false, 'the menu also resets the page already showing');
  assert.deepEqual(scrolled, [0], 'and returns it to the top');
  context.navigate('/home');
  assert.equal(views.home[0].ariaExpanded, 'false', 'the preset graph chevron folds too');
  assert(codeIncludes(appJsSource, 'if(history.state?.open)showFolds(view,history.state.open);'),
      'a page shown from history restores its folds');
  assert(codeIncludes(appJsSource, 'document.addEventListener("toggle",rememberPage,true);'),
      'a group toggled after returning updates its entry');
}
