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
  const micraButton = {dataset: {}, hidden: true, attributes: {},
    setAttribute(name, value) { this.attributes[name] = value; },
    getAttribute(name) { return this.attributes[name]; }};
  const elements = Object.fromEntries([...['wifi', 'bluetooth'].flatMap((kind, i) => [
    [kind + 'Signal', buttons[i]], [kind + 'Details', {hidden: true}],
    [kind + 'Name', {}], [kind + 'Detail', {}],
  ]), ['companionSignal', companionButton], ['companionName', {}], ['companionDetail', {}],
    ['micraSignal', micraButton], ['micraName', {}], ['micraMachine', {}], ['micraDetail', {}]]);
  const start = runtimeJs.indexOf('function updateHeaderSignals(');
  assert(start >= 0, 'Shared header must render cached signal status');
  const context = vm.createContext({$: id => elements[id], __LINEA_MICRA__: true,
    document: {querySelectorAll: () => buttons, querySelector: () => null,
      addEventListener: (name, fn) => {events[name] = fn;}},
    window: {addEventListener: (name, fn) => {events[name] = fn;}},
  });
  const regionEnd = runtimeJs.slice(start).search(/\nlet\s+homeBootDone\s*=/);
  vm.runInContext(runtimeStateDecl + runtimeJs.slice(start, regionEnd < 0 ? undefined : regionEnd + start).replace(/^export /gm, ''), context);
  for (const [signal, level] of [[1, '1'], [2, '2'], [3, '3'], [null, 'unknown'], [NaN, 'unknown'], [Infinity, 'unknown'], [-1, 'unknown'], [0, 'unknown'], [4, 'unknown'], [1.5, 'unknown'], ['3', 'unknown']]) {
    context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: signal, bluetoothConnected: true, bluetoothLevel: signal}});
    for (const kind of ['wifi', 'bluetooth']) assert.equal(elements[kind + 'Signal'].dataset.level, level, String(signal));
  }
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3, bluetoothConnected: false, bluetoothLevel: 3}});
  assert.equal(elements.wifiSignal.dataset.level, '3');
  assert.equal(elements.bluetoothSignal.dataset.level, '0', 'Disconnect overrides an old reading');
  assert.equal(elements.bluetoothSignal.attributes['aria-label'], 'Bluetooth: Scale disconnected');
  context.updateHeaderSignals({connections: {wifiConnected: false, wifiLevel: 3}});
  assert.equal(elements.wifiSignal.dataset.level, '0', 'STA disconnected does not imply a strong AP signal');
  context.updateHeaderSignals({snapshotStale: true, connections: {wifiConnected: true, wifiLevel: 3, bluetoothConnected: true, bluetoothLevel: 3}});
  assert.equal(elements.wifiSignal.dataset.level, 'unknown');
  assert.equal(elements.bluetoothSignal.dataset.level, 'unknown');
  context.updateHeaderSignals();
  assert.equal(elements.wifiSignal.title, 'Wi-Fi: Signal unavailable', 'Transport failures clear the displayed measurement');
  assert.equal(elements.bluetoothSignal.title, 'Bluetooth: Signal unavailable');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3, bluetoothConnected: true, bluetoothLevel: 2}});
  assert.equal(elements.wifiSignal.title, 'Wi-Fi: Strong');
  assert.equal(elements.bluetoothSignal.title, 'Bluetooth: Medium');
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
  assert.equal(elements.companionSignal.attributes['aria-label'], 'Companion: Not connected');
  assert.equal(elements.companionName.textContent, 'nas', 'The paired name stays while the link is down');
  assert.equal(elements.companionDetail.textContent, 'Not connected');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, companion: {paired: true, link: 'down', name: 'nas', peerProtocol: 2}});
  assert.equal(elements.companionDetail.textContent,
      'Not connected · the companion and this controller use different link versions; update the older one',
      'A refused link version names the connection problem');
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, companion: {paired: true, link: 'open', name: 'nas'}});
  assert.equal(elements.companionSignal.dataset.level, 'on');
  assert.equal(elements.companionSignal.attributes['aria-label'], 'Companion: Connected');
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
  const micraFrame = (lineaMicra) => context.updateHeaderSignals(
      {connections: {wifiConnected: true, wifiLevel: 3}, machineIntegration: 'linea_micra_cloud', lineaMicra});
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}});
  assert.equal(elements.micraSignal.hidden, true, 'Other integrations show no cloud icon');
  micraFrame({quality: 'current', email: 'barista@example.test', selectedName: 'Kitchen Micra'});
  assert.equal(elements.micraSignal.hidden, false, 'The micra integration shows the cloud icon');
  assert.equal(elements.micraSignal.dataset.level, 'on');
  assert.equal(elements.micraSignal.attributes['aria-label'], 'La Marzocco cloud: Connected');
  assert.equal(elements.micraName.textContent, 'barista@example.test');
  assert.equal(elements.micraMachine.textContent, 'Kitchen Micra');
  assert.equal(elements.micraDetail.textContent, 'Connected');
  micraFrame({quality: 'optimistic', email: 'barista@example.test', selectedName: 'Kitchen Micra'});
  assert.equal(elements.micraSignal.dataset.level, 'on', 'A commanded wake-up the cloud has yet to confirm stays inked');
  for (const [quality, label] of [['stale', 'Connection problems'],
      ['communication_error', 'Connection problems'], ['unconfigured', 'Not connected'],
      ['disabled', 'Disabled'], ['unsupported', 'Connection problems']]) {
    micraFrame({quality, email: '', selectedName: ''});
    assert.equal(elements.micraSignal.dataset.level, 'off', quality);
    assert.equal(elements.micraDetail.textContent, label, quality);
  }
  assert.equal(elements.micraName.textContent, 'No account saved', 'No account says so');
  assert.equal(elements.micraMachine.textContent, 'No machine selected', 'No machine says so');
  context.updateHeaderSignals({snapshotStale: true, connections: {}, machineIntegration: 'linea_micra_cloud',
    lineaMicra: {quality: 'current', email: 'barista@example.test', selectedName: 'Kitchen Micra'}});
  assert.equal(elements.micraSignal.dataset.level, 'off', 'A stale snapshot grays the cloud icon');
  assert.equal(elements.micraDetail.textContent, 'Signal unavailable');
  assert.equal(elements.micraName.textContent, 'barista@example.test', 'The stale account keeps its last known email');
  micraFrame({quality: 'current'});
  assert.equal(elements.micraSignal.dataset.level, 'on', 'A fresh frame re-inks the cloud icon');
  // The Admin poll names the integration but carries no micra model; it must
  // leave the indicator on its last Home frame instead of graying it.
  context.updateHeaderSignals({connections: {wifiConnected: true, wifiLevel: 3}, machineIntegration: 'linea_micra_cloud'});
  assert.equal(elements.micraSignal.dataset.level, 'on', 'A model-less envelope keeps the cloud icon');
  assert.equal(elements.micraDetail.textContent, 'Connected');
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
  events.keydown({key: 'Escape'});
  assert.equal(elements.bluetoothDetails.hidden, true);
  assert.equal(buttons[1].focused, true, 'Escape returns focus to the trigger');
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
  assert(codeIncludes(rawRuntimeJs, 'noteReachFail(err,force){clearCupWeights();updateHeaderSignals();'));
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
  const doc = {dataset: {}}, reads = [];
  const el = (width) => ({getBoundingClientRect() {reads.push(width); return {width};}});
  const bar = {clientWidth: 120,
    querySelector: (s) => s === '.brand' ? el(40) : s === '.headerSignals' ? el(90) : null};
  const context = vm.createContext({document: {querySelector: () => bar, documentElement: doc},
    getComputedStyle: () => ({paddingLeft: '10px', paddingRight: '10px', columnGap: '4px'})});
  vm.runInContext(blockAt(runtimeJs, 'function updateBrandFit('), context);
  doc.dataset.brandCompact = '1';
  context.updateBrandFit();
  assert.deepEqual(reads, [40, 90], 'fit is measured with the text restored');
  assert.equal(doc.dataset.brandCompact, '1', '40 + 4 + 90 does not fit 100 available');
  bar.clientWidth = 200;
  context.updateBrandFit();
  assert.equal(doc.dataset.brandCompact, undefined, 'a fitting row brings the text back');
  assert(codeIncludes(appJsSource, 'sizeHeader();R.updateBrandFit();'),
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

  const events = {}, mobile = {matches: true, addEventListener: (name, fn) => {events.resize = fn;}};
  let scrolled = false;
  const header = vm.createContext({root: {toggleAttribute: (name, value) => {assert.equal(name, 'data-scrolled'); scrolled = value;}},
    window: {scrollY: 0, matchMedia: () => mobile, addEventListener: (name, fn) => {events[name] = fn;}}});
  const headerStart = appJsSource.indexOf('function scrollHeader()');
  header.mobile = mobile;
  vm.runInContext(appJsSource.slice(headerStart, (() => { const at = appJsSource.slice(headerStart).search(/window\.addEventListener\(\s*['"]popstate['"]/); return at < 0 ? undefined : at + headerStart; })()), header);
  // The phone header holds its compact size; only the floating surface state
  // follows the scroll.
  for (const scroll of [0, 30, 60, 120, 300, -20]) {
    header.window.scrollY = scroll; events.scroll();
    assert.equal(scrolled, scroll > 0, 'Gray surface returns exactly at the top, including overscroll');
  }
  mobile.matches = false; header.window.scrollY = 60; events.scroll();
  assert.equal(scrolled, true, 'Desktop scrolling still floats header navigation');
  header.window.scrollY = 0; events.scroll(); assert.equal(scrolled, false, 'Returning to the top restores desktop chrome');
  header.window.scrollY = 60;
  mobile.matches = true; events.resize();
  assert.equal(scrolled, true, 'Switching to mobile keeps the scrolled surface state');
  assert(!css.includes('--header-progress') && !css.includes('--compact'),
      'The phone header keeps a fixed compact size with no scroll-driven shrink');
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
      assert(target === app || target === header || target === nav || links.includes(target));
      callbacks[target === app ? 'resize' : target === header ? 'geometry' : 'pill'] = this.fn;
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
  assert.equal(geometry['--menu-offset'], '72px', 'Initial header measures its actual menu row');
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
  nav.offsetTop = 80; callbacks.geometry();
  assert.equal(geometry['--menu-offset'], '80px', 'Header resizing updates the floating offset');
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
    nav.offsetTop = 999; callbacks.geometry();
    assert.equal(root.dataset.navLayout, 'bottom', 'Geometry observation cannot promote phone navigation');
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
  assert(css.includes('bottom:var(--nav-offset)'), 'Save and action bars follow the navigation clearance');
  assert(!css.includes('top:-3.85rem'), 'Floating header avoids a fixed branding-height assumption');
  assert(css.includes('top:calc(.5rem - var(--menu-offset))'), 'Sticky top follows measured geometry');
  assert(css.includes('[data-scrolled] .topBar{background:transparent;pointer-events:none}'), 'Floating surroundings expose and do not intercept content');
  assert(css.includes('[data-scrolled] .pageNav{pointer-events:auto}'), 'Floating links remain interactive');
}
// A disconnected Bluetooth icon dims its rune like the Wi-Fi arcs and carries
// no slash, which only added visual noise next to the dimmed bars.
if (!css.includes('#bluetoothSignal[data-level="0"] path{opacity:.12}') ||
    /#bluetoothSignal[^{]*svg>g/.test(css) || /id="bluetoothSignal"[^]*?signalSlash[^]*?<\/button>/.test(html)) {
  throw new Error('The disconnected Bluetooth icon must dim as a whole without a slash');
}
