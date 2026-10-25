const VIEW_NAMES = webUi.VIEW_NAMES;

(async () => {
  const assert = require('assert').strict, generated = await webUi.generate();
  const runtime = 'data:text/javascript,' + encodeURIComponent('export const $=()=>null;');
  // The records bundle evaluates on its own and hands app.js each view's
  // init and markup; single-view modules export both at top level.
  const code = generated.modules.records.replace(/(['"])\/js\/runtime\.js\?[^'"]*\1/, JSON.stringify(runtime));
  const {views} = await import('data:text/javascript,' + encodeURIComponent(code));
  assert.deepEqual(Object.keys(views).sort(), [...webUi.VIEW_MODULES.records].sort());
  for (const name of webUi.VIEW_MODULES.records) {
    assert.equal(typeof views[name].init, 'function', name + ': lazy view must initialize');
    assert.equal(views[name].markup, generated.partials[name], name + ': lazy view must carry its markup');
  }
  for (const [file, [name]] of Object.entries(webUi.VIEW_MODULES).filter(([, n]) => n.length === 1)) {
    assert(/\bexport function init\(/.test(generated.modules[file]) &&
        /\bexport const markup=/.test(generated.modules[file]), file + '.js must export init and markup');
    assert(generated.modules[file].includes(generated.partials[name].slice(0, 40)),
        file + '.js must carry its view markup');
  }
})().catch(error => {console.error(error); process.exitCode = 1;});
const {partialHtml: rawPartialHtml, appJs: rawAppJsSource, runtimeJs: rawRuntimeJs,
  otaImageJs: rawOtaImageJs, viewJs: rawViewJs, css: rawCss} = authoring.raw;
const runtimeConsumers = rawAppJsSource + '\n' + Object.values(rawViewJs).join('\n');
// Raw (unlocalized) JS of every module, for presence checks that do not
// care which module owns the code.
const rawAllJs = [rawAppJsSource, rawRuntimeJs, ...Object.values(rawViewJs)].join('\n');
const runtimeReferences = new Set([...runtimeConsumers.matchAll(
  /\bR\.([A-Za-z_$][\w$]*)(?![\w$])/g)].map((match) => match[1]));
const runtimeExports = [
  ...rawRuntimeJs.matchAll(/export function\s+([A-Za-z_$][\w$]*)/g),
].map((match) => match[1]);
const runtimeExportBlock = rawRuntimeJs.match(/export\s*\{([\s\S]*?)\n\};/);
if (!runtimeExportBlock) throw new Error('Runtime module export block missing');
runtimeExports.push(...runtimeExportBlock[1].replace(/\s/g, '').split(',').filter(Boolean));
for (const name of runtimeExports) {
  if (!runtimeReferences.has(name)) {
    throw new Error(`Runtime export is not imported: ${name}`);
  }
}
// Structural contracts match authoring sources with whitespace-insensitive
// comparison so the readable formatting style cannot break them. Semicolons
// and quote characters are stripped from both haystack and needle: formatters
// add statement terminators the historic minified needles omit and normalize
// string quotes. The runtime module qualifier (R.) and the shared uiState
// container are stripped too: they only say which module owns a function or
// a flag, so code keeps matching when it moves between runtime and a view.
const compactSource = (source) =>
  source.replace(/\b(?:R\.|uiState\.)/g, '').replace(/[\s;'"]/g, '');
const codeIncludes = (source, needle) =>
  compactSource(source).includes(compactSource(needle));
// Extract a top-level declaration block from an authoring source by cutting
// at the next column-0 closing brace; valid for the formatted style whose
// top-level closers start a line.
const blockAt = (source, marker) => {
  const at = source.indexOf(marker);
  return at < 0 ? '' : source.slice(at, source.indexOf('\n}', at) + 2);
};
if (codeIncludes(rawAppJsSource, 'htmlCache') ||
    !compactSource(rawAppJsSource).includes(
        'if(viewLoads.has(name))returnviewLoads.get(name)') ||
    !compactSource(rawAppJsSource).includes('viewLoads.set(name,load)') ||
    !compactSource(rawAppJsSource).includes('finally{viewLoads.delete(name)') ||
    !compactSource(rawAppJsSource)
        .includes('if(jsMods.has(name))returnjsMods.get(name)')) {
  throw new Error('View loading must coalesce in flight and release promises after insertion');
}
// Every download, Diagnostic included, goes through the runtime saveFile.
const diagnosticCompact = compactSource(blockAt(rawRuntimeJs, 'function saveFile('));
const diagnosticDownload = diagnosticCompact.match(
  /const([A-Za-z_$][\w$]*)=document\.createElement\(a\)[\s\S]*?,([A-Za-z_$][\w$]*)=URL\.createObjectURL\(/);
const diagnosticClickAt = diagnosticDownload
  ? diagnosticCompact.indexOf(`${diagnosticDownload[1]}.click()`, diagnosticDownload.index)
  : -1;
const diagnosticHrefAt = diagnosticDownload
  ? diagnosticCompact.indexOf(
      `${diagnosticDownload[1]}.href=${diagnosticDownload[2]}`, diagnosticDownload.index)
  : -1;
const diagnosticRevokeAt = diagnosticDownload
  ? diagnosticCompact.indexOf(
      `URL.revokeObjectURL(${diagnosticDownload[2]})`, diagnosticDownload.index)
  : -1;
if (!diagnosticDownload || diagnosticHrefAt < 0 ||
    diagnosticClickAt < diagnosticHrefAt ||
    diagnosticRevokeAt < diagnosticClickAt) {
  throw new Error('Diagnostic download must revoke its object URL after use');
}
if (!codeIncludes(rawViewJs.diagnostic, 'const saveFile=(blob,name)=>R.saveFile(blob,openbrewbyweight-+name)') ||
    /createObjectURL/.test(rawViewJs.diagnostic)) {
  throw new Error('Diagnostic must reuse the runtime download helper');
}
const {shellHtml, partialHtml, appJs: appJsSource, runtimeJs, otaImageJs, viewJs, css} =
    authoring.localized;
const {allHtml, allJs} = authoring;
// Harnesses that run a slice of the runtime prepend its shared UI state.
const runtimeStateDecl = blockAt(runtimeJs, 'const uiState = {') + ';\n';
// Harnesses that keep state as vm globals alias uiState and R to the global
// object and seed the shared-state defaults they do not set themselves.
const runtimeStateGlobals = 'var uiState = this, R = this;for (const [k, v] of Object.entries(' +
    runtimeStateDecl.replace(/^const uiState = /, '').replace(/;\n$/, '') +
    ')) if (!(k in this)) this[k] = v;\n';
// The Stats export window lives in the Stats module; stream harnesses that
// already declare uiState append it with its two runtime dependencies.
const statsExportJs = '\nconst R = {uiState, sendUiOperation};\n' +
    viewJs.stats.slice(viewJs.stats.indexOf('let statsExportInFlight'),
        viewJs.stats.indexOf('async function clearShotHistory('));
// Stats renders its shots with runtime helpers; text contracts on that code read both.
const statsRuntimeJs = runtimeJs + '\n' + viewJs.stats;
// History renders its rows with runtime helpers; text contracts on that code read both.
const historyRuntimeJs = runtimeJs + '\n' + viewJs.history;
// The Stats chart and shot-row helpers open the Stats module; harnesses run them
// beside the runtime slices they used to share a range with.
const statsChartJs = viewJs.stats.slice(viewJs.stats.indexOf('function formatShotTime('),
    viewJs.stats.indexOf('function formatShotTimeCsv('));
// Localized values keep their emitted quote style: codeIncludes compares
// needles quote-insensitively, and a text-wide double-to-single rewrite would
// corrupt markup that embeds unescaped quotes (e.g. values like " class=").
// Most wiring checks look across shell + partials + all JS modules.
const html = allHtml;
const js = allJs;
const ui = allHtml + '\n' + allJs;
// Every view lives in one document: an id repeated across partials makes
// getElementById resolve to whichever view was injected first.
{
  const seenIds = new Set();
  for (const [, id] of allHtml.matchAll(/\sid="([^"]+)"/g)) {
    if (seenIds.has(id)) throw new Error(`Duplicate id across the Web UI document: ${id}`);
    seenIds.add(id);
  }
}
const settingsHtml = partialHtml.settings;
for (const field of settingsHtml.matchAll(
    /<(input|select|textarea)\b[^>]*\bid="([^"]+)"[^>]*>/g)) {
  const labelStart = settingsHtml.lastIndexOf('<label', field.index);
  const paragraphStart = settingsHtml.lastIndexOf('<p', field.index);
  const tag = labelStart > paragraphStart ? 'label' : 'p';
  const start = Math.max(labelStart, paragraphStart);
  const end = settingsHtml.indexOf(`</${tag}>`, field.index);
  const container = start >= 0 && end >= field.index
    ? settingsHtml.slice(start, end)
    : '';
  // The shot-window inputs share the hint that follows their pair.
  const pairEnd = settingsHtml.indexOf('</div>', field.index);
  const sharedHint = settingsHtml.lastIndexOf('<div class="windowPair">', field.index) >
      settingsHtml.lastIndexOf('</div>', field.index) &&
    settingsHtml.startsWith('<small class="fieldHint', pairEnd + 6);
  if (!container.includes('class="fieldHint') && !sharedHint) {
    throw new Error(`Settings field ${field[2]} must have a localized field hint`);
  }
}
for (const [id, names] of Object.entries({
  paddleMode: ['Natural:', 'Original:', 'Auto:'],
  momentaryStartEdge: ['Pressed:', 'Released:'],
  noScaleBbwMode: ['Brew manually:', 'Warn once, then allow:', 'Require a scale:'],
  scalePreference: ['Any scale:', 'My scale first:', 'Only my scale:'],
  alertOutputChannel: ['Scale first:', 'Buzzer only:', 'Scale only:'],
})) {
  const start = settingsHtml.indexOf(`id="${id}"`);
  const help = settingsHtml.slice(start, settingsHtml.indexOf('</label>', start));
  if (start < 0 || names.some((name) => !help.includes(`<strong>${name}</strong>`)))
    throw new Error(`Complex Settings selector ${id} must explain every option with a bold name`);
}
if (/<details\b[^>]*\bopen\b/i.test(allHtml)) {
  throw new Error('All collapsible <details> groups must start collapsed (no open attribute)');
}
if (css.includes('.brandLogo') || allHtml.includes('logo.svg') || allHtml.includes('brandLogo')) {
  throw new Error('Web UI must not embed a logo asset or .brandLogo styles');
}
if (!css.includes('.brand') || !css.includes('inline-flex') ||
    !css.includes('.brandMark') || !shellHtml.includes('class="brandMark"') ||
    !shellHtml.includes('<svg') ||    !shellHtml.includes('<small>Open Brew</small>by Weight') ||
    shellHtml.includes('logo.svg')) {
  throw new Error('Brand lockup must use inline SVG mark plus HTML wordmark');
}
if (shellHtml.split('mask="url(#brandCut)"').length !== 2 ||
    shellHtml.split('class="brandMark"').length !== 4 ||
    !shellHtml.includes('<svg class="spriteDefs" width="0" height="0" aria-hidden="true" focusable="false"><symbol id="brandMark" viewBox="12 4 40 56">') ||
    !shellHtml.includes('<use href="#brandMark"/>')) {
  throw new Error(
      'Brand mark must be defined once as an inline symbol sprite and referenced from the header, the loading view, and the inactive overlay');
}
if (!shellHtml.includes('class="pageNav"') ||
    shellHtml.includes('id="navToggle"') ||
    shellHtml.indexOf('class="pageNav"') > shellHtml.indexOf('id="app"') ||
    shellHtml.indexOf('class="topBar"') > shellHtml.indexOf('class="pageNav"') ||
    !css.includes('@media(min-width:700px)') ||
    !css.includes('[data-nav-layout=text] .pageNav svg{display:none}') ||
    !codeIncludes(appJsSource, 'function updateNavigationLayout()') ||
    !codeIncludes(appJsSource, "matchMedia('(max-width: 699px)')")) {
  throw new Error('Web UI must adapt a single navigation bar between header icons, header text and bottom icons');
}
if (!css.includes('.inactiveMain{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:1rem;max-width:24rem;width:100%;text-align:center}')) {
  throw new Error('Inactive Web UI must use a full-screen surface with centered content');
}
if (!shellHtml.includes('<div id="homeBoot" class="bootOverlay" role="status">') ||
    shellHtml.indexOf('id="homeBoot"') > shellHtml.indexOf('class="topBar"') ||
    !css.includes('.bootOverlay{position:fixed;inset:0;z-index:39') ||
    !css.includes('.inactiveOverlay{position:fixed;inset:0;z-index:40') ||
    !shellHtml.includes('id="homeBoot" class="bootOverlay" role="status"><div class="brand" aria-hidden="true">') ||
    !css.includes('.bootOverlay .brand,.inactiveOverlay .brand{') ||
    !css.includes('.bootOverlay .brandMark{width:2.85rem;height:3.8rem}') ||
    !css.includes('.brand span{display:flex;flex-direction:column;') ||
    !css.includes('.bootOverlay.isDone{opacity:0;pointer-events:none}') ||
    !css.includes('transition:opacity .25s}') ||
    !shellHtml.includes('class="bootWave" aria-hidden="true"><i></i><i></i><i></i><i></i><i></i>') ||
    !css.includes('.bootWave i{width:.55rem;height:100%;border-radius:.3rem;background:var(--ac);animation:bootWave 1.1s ease-in-out infinite}') ||
    !css.includes('@keyframes bootWave{0%,100%{transform:scaleY(.25)}50%{transform:scaleY(1)}}') ||
    !css.includes('.bootWave i,.shotHero.live .heroDot{animation:none}') ||
    !codeIncludes(runtimeJs, 'let homeBootDone=false,fwReloading=false') ||
    !codeIncludes(runtimeJs, 'function hideHomeBoot(seq=bootSeq){') ||
    !codeIncludes(runtimeJs, "bootTimer=setTimeout(()=>{if(seq===bootSeq)") ||
    !codeIncludes(runtimeJs, "function message(text,kind=''){if(kind==='error')hideHomeBoot();") ||
    codeIncludes(runtimeJs, 'function applyHomeStatus(s){hideHomeBoot();') ||
    !codeIncludes(runtimeJs, "function showInactiveOverlay(){const el=$('webUiInactive');if(!el)return;hideHomeBoot();") ||
    !codeIncludes(runtimeJs, "location.replace(location.pathname+'?fw='+version)") ||
    !codeIncludes(appJsSource, 'boot=R.showPageBoot();R.stopViewPolls();') ||
    !codeIncludes(appJsSource, 'if(ok){R.hideHomeBoot(boot);') ||
    !codeIncludes(appJsSource, 'if(activeView!=="setup")R.ensureBackgroundStreams();')) {
  throw new Error(
      'Every view must reuse the Home splash and fade immediately after data loads, preserving firmware reload and inactive overlay handoff');
}

{
  const assert = require('assert').strict, vm = require('vm');
  const deferred = () => {
    let resolve, reject;
    const promise = new Promise((yes, no) => {resolve = yes; reject = no;});
    return {promise, resolve, reject};
  };
  const flush = async () => {for (let i = 0; i < 12; i++) await Promise.resolve();};
  const overlaySource = rawRuntimeJs.slice(rawRuntimeJs.indexOf('function showPageBoot('),
    rawRuntimeJs.indexOf('function setOverlayOutOfReach('));
  const routeSource = appJsSource.slice(appJsSource.indexOf('function startView('),
    appJsSource.indexOf('function navigate('));
  const hooksSource = appJsSource.slice(appJsSource.indexOf('R.setViewPollHooks('),
    appJsSource.search(/document\.querySelectorAll\(\s*['"]a\[data-route\]['"]\s*\)/));
  (async () => {
    const classes = new Set(['hidden', 'isDone']), bodyClasses = new Set(), timers = [];
    const classList = set => ({add: (...names) => names.forEach(n => set.add(n)),
      remove: (...names) => names.forEach(n => set.delete(n)),
      toggle: (name, enabled) => enabled ? set.add(name) : set.delete(name)});
    let layouts = 0;
    const overlay = {style: {}, classList: classList(classes), setAttribute() {},
      get offsetWidth() {assert.equal(bodyClasses.has('pageLoading'), true); layouts++; return 1;}};
    const context = vm.createContext({bootSeq: 0, homeBootDone: false,
      bootTimer: 0, fwReloading: false,
      document: {body: {classList: classList(bodyClasses)}},
      $: id => {assert.equal(id, 'homeBoot', 'Do not query or await control animations'); return overlay;},
      requestAnimationFrame: () => {throw Error('Fade must not wait for render frames');},
      setTimeout: (fn, ms) => {assert.equal(ms, 250); timers.push(fn); return timers.length;},
      clearTimeout() {}});
    vm.runInContext(overlaySource, context);
    const token = context.showPageBoot();
    assert.equal(overlay.style.zIndex, '39', 'URL entry covers the header and content');
    assert.deepEqual([...classes], [], 'Page loading must appear immediately');
    assert.equal(bodyClasses.has('pageLoading'), true, 'Suppress control animations before loading');
    context.hideHomeBoot(token);
    assert.equal(classes.has('isDone'), true, 'Start fading synchronously when data is ready');
    assert.equal(layouts, 1, 'Apply static control positions before fading');
    assert.equal(bodyClasses.has('pageLoading'), true, 'Keep controls static throughout the fade');
    context.showPageBoot(); timers.shift()();
    assert.deepEqual([...classes], [], 'An old fade timer must not hide the next route');
    const headerZ = Number(css.match(/\.topBar\{[^}]*z-index:(\d+)/)[1]);
    assert.ok(Number(overlay.style.zIndex) < headerZ, 'Navigation leaves the complete responsive header and menu above loading');
    assert.equal(bodyClasses.has('pageLoading'), true, 'An old timer must not restore animations');
    context.hideHomeBoot(token);
    assert.deepEqual([...classes], [], 'An old route must not fade the current loading view');
    context.hideHomeBoot(context.bootSeq); timers.shift()();
    assert.equal(classes.has('hidden'), true);
    assert.equal(bodyClasses.has('pageLoading'), false, 'Restore normal animations after the fade');
    context.showPageBoot();
    context.fwReloading = true;
    await context.hideHomeBoot();
    assert.deepEqual([...classes], [], 'Keep loading visible while firmware reloads');

    const catalog = deferred(), preview = deferred(), statusMarkup = deferred(), applied = [];
    const statusContext = vm.createContext({activeView: 'admin',
      viewSeq: 1, viewReady: statusMarkup.promise,
      document: {hidden: false}, webUiPollingActive: () => true,
      statusPageOk: () => true,
      api: async () => ({adminUnlocked: true}), applyCommonStatus() {},
      viewStatusHandlers: {admin: () => applied.push('status')},
      timezoneCatalogPromise: catalog.promise, timezonePreviewLoad: preview.promise,
      noteReachOk: () => applied.push('ready'), noteReachFail() {}, armStatusTimer() {}});
    vm.runInContext(blockAt(rawRuntimeJs, 'async function loadStatus('), statusContext);
    const adminLoading = statusContext.loadStatus(); await flush();
    assert.deepEqual(applied, [], 'Fetch status immediately but defer hydration until markup is initialized');
    statusMarkup.resolve(); await flush();
    assert.deepEqual(applied, ['status']);
    catalog.resolve(); await flush();
    assert.deepEqual(applied, ['status'], 'Admin must still wait for its timezone preview');
    preview.resolve(); assert.equal(await adminLoading, true);
    assert.deepEqual(applied, ['status', 'ready']);
    const outdated = deferred(); statusContext.api = () => outdated.promise;
    const oldStatus = statusContext.loadStatus(); statusContext.activeView = 'settings'; statusContext.viewSeq++;
    outdated.resolve({adminUnlocked: true});
    assert.equal(await oldStatus, false, 'Discard a status response from the previous view');

    let rendered = 0;
    const logContext = vm.createContext({logBootId: 3, bootId: 3, logEvents: [],
      lastLog: 9, logMissed: 0, LOG_EVENTS_CAPACITY: 500, activeView: 'diagnostic',
      updateLogHealth() {}, renderLog: () => {rendered++;}, updateFirmwareFooter() {},
      noteReachOk() {}, noteReachFail() {}, paint: (fn) => fn()});
    const logFns = rawRuntimeJs.slice(rawRuntimeJs.indexOf('function logStreamFrame('),
        rawRuntimeJs.indexOf('function diagSnapshotOk('));
    vm.runInContext(runtimeStateGlobals + logFns, logContext);
    const event = sequence => ({sequence, atMs: 1, wallSec: 0, localSec: 0,
      level: 'info', category: 'web', message: 'm'});
    const frame = {v: 1, type: 'log', bootId: 3, historyOverwritten: 0,
      missedEvents: 2, serialDropped: 0, hasMore: false,
      cursorInvalid: false, events: [event(10), event(11)]};
    logContext.applyLogFrame(logContext.logStreamFrame(frame));
    for (const bad of [{...frame, missedEvents: null}, {...frame, events: [{...event(12), message: 4}]}])
      assert.throws(() => logContext.logStreamFrame(bad));
    assert.equal(rendered, 1);
    assert.equal(logContext.lastLog, 11);
    assert.equal(logContext.logMissed, 2);
    logContext.applyLogFrame(logContext.logStreamFrame({...frame, bootId: 4,
      missedEvents: 0, events: [event(1)]}));
    assert.equal(logContext.logEvents.length, 1,
        'a boot change must clear the rendered log');
    assert.equal(logContext.lastLog, 1);
    assert.equal(rendered, 2);

    const slotSource = ['function drainDeviceSlots(', 'function acquireDeviceSlot(',
      'function releaseDeviceSlot(', 'function sessionToken(', 'function webUiHeaders(',
      'async function api(']
        .map((marker) => blockAt(rawRuntimeJs, marker)).join('\n');
    for (const view of ['stats', 'history', 'home', 'settings', 'admin', 'diagnostic']) {
      const requests = [], responses = [];
      const slots = vm.createContext({activeView: view, deviceInFlight: 0, deviceWaiters: [],
        DEVICE_MAX_INFLIGHT: 1, webUiPollingActive: () => true,
        WEB_UI_CLIENT_HEADER: 'X-WebUI-Client', webUiClientId: 'test', webUiPowerSeconds: () => 0, noteUiSent() {},
        WEB_UI_SESSION_HEADER: 'X-WebUI-Session', SESSION_KEY: 'obbwSession', sessionMemory: '',
        AbortController, setTimeout: () => 1, clearTimeout() {}, __WEBUI_TEXT__: key => key,
        fetch: path => {const response = deferred(); requests.push(path); responses.push(response); return response.promise;}});
      vm.runInContext(slotSource, slots);
      const records = view === 'history' ? '/api/v1/log' : '/api/v1/status/admin';
      const one = slots.api(records), two = slots.api(records),
        three = slots.api(records);
      const finish = () => responses.shift().resolve({ok: true, text: async () => '{}'});
      await flush();
      assert.equal(requests.length, 1, view + ': one request at a time');
      const command = slots.api('/api/v1/config', {method: 'POST', body: '{}'});
      finish(); await flush();
      assert.equal(requests.length, 2, 'Release the request slot without exceeding its limit');
      assert.equal(requests.includes('/api/v1/config'), false, 'Commands wait for all reads');
      finish(); await flush(); finish(); await flush();
      assert.equal(requests.at(-1), '/api/v1/config');
      assert.equal(slots.deviceInFlight, 1, 'Command runs alone');
      const following = slots.api(records); await flush();
      assert.equal(requests.at(-1), '/api/v1/config', 'Reads cannot overlap a command');
      finish(); await flush(); finish();
      await Promise.all([one, two, three, command, following]);
      assert.equal(slots.deviceInFlight, 0);
      assert.equal(slots.deviceWaiters.length, 0);
      const failure = slots.api(records); await flush();
      responses.shift().reject(new Error('Unavailable'));
      await assert.rejects(failure);
      assert.equal(slots.deviceInFlight, 0, 'Network failure must release admission');
    }

    // Record pages have no REST reads left; the remaining per-view fetches
    // are the claim-gated status pages, exercised by the lifecycle suites.
    assert(!codeIncludes(rawAllJs, '/api/v1/stats?offset=') &&
           !codeIncludes(rawAllJs, '/api/v1/history?offset='),
        'record pages must subscribe over the owned WebSocket, not read REST');

    // Stats and History share one module; the browser's module map
    // deduplicates a URL, which importModule mirrors here.
    const records = deferred(), modules = new Map(), assetEvents = [], initialized = [], sections = {};
    for (const name of ['stats', 'history']) sections['view-' + name] = {
      set innerHTML(html) {assetEvents.push('insert-' + name + ':' + html);},
      querySelectorAll: () => [],
    };
    const assetContext = vm.createContext({jsMods: new Map(), viewLoads: new Map(),
      BUNDLES: {stats: 'records', history: 'records'}, bootFetch: null,
      assetQuery: '?v=test', document: {getElementById: id => sections[id]},
      __WEBUI_TEXT__: key => key,
      importModule: url => {
        if (!modules.has(url)) {assetEvents.push('js ' + url); modules.set(url, records.promise);}
        return modules.get(url);
      }});
    const ensureSource = appJsSource.slice(appJsSource.indexOf('function fetchView('),
      appJsSource.indexOf('R.setEnsureViewHook')).replace(/import\(/g, 'importModule(');
    vm.runInContext(ensureSource, assetContext);
    assetContext.bootFetch = ['history', assetContext.fetchView('history')];
    const firstStats = assetContext.ensureView('stats'), againStats = assetContext.ensureView('stats'),
      firstHistory = assetContext.ensureView('history');
    assert.deepEqual(assetEvents, ['js /js/records.js?v=test'],
        'The opening view prefetch and the shared module download once');
    assert.equal(assetContext.bootFetch, null, 'The opening view consumes its prefetch once');
    records.resolve({views: Object.fromEntries(['stats', 'history'].map(name => [name, {
      markup: '<div>' + name + '</div>',
      init: () => {assert.equal(assetEvents.includes('insert-' + name + ':<div>' + name + '</div>'), true); initialized.push(name);},
    }]))});
    await Promise.all([firstStats, againStats, firstHistory]);
    await assetContext.ensureView('stats'); await assetContext.ensureView('history');
    assert.deepEqual(initialized.sort(), ['history', 'stats'], 'Revisits reuse each initialized view');

    for (const view of ['home', 'settings', 'stats', 'history', 'diagnostic', 'admin']) {
      const events = [], markup = deferred(), status = deferred(), data = deferred(), bodyClasses = new Set();
      const path = view === 'home' ? '/' : '/' + view;
      const r = vm.createContext({activeView: '', routeSeq: 0, logTimer: 0,
        jsMods: new Map(),
        ROUTES: {[path]: view}, knownPath: () => path, viewToPath: () => path,
        location: {pathname: path}, history: {}, stopExtraPolls() {}, placePill() {}, markNavActive() {},
        ensureView: () => {events.push('markup'); return markup.promise;},
        document: {hidden: false, querySelectorAll: () => [], body: {classList: classList(bodyClasses)}}, setInterval: () => 1, setTimeout: () => 1,
        __WEBUI_TEXT__: key => key,
        R: {showPageBoot: () => {events.push('show'); return 1;}, stopViewPolls() {},
          api: async () => {throw new Error('record views must not probe REST status');}, compatibilityModeOn: () => false,
          withPollGate: fn => fn(), webUiPollingActive: () => true,
          setActiveView() {}, armStatusTimer() {}, applyDiagnosticNavigation() {},
          startDiagnosticStream() {}, stopDiagnosticStream() {},
          startStatsStream: () => {events.push('data'); return data.promise;},
          startHistoryStream: () => {events.push('data'); return data.promise;},
          ensureBackgroundStreams() {},
          loadStatus: () => {events.push('status'); return status.promise;},
          startLogStream: () => {events.push('data'); return data.promise;},
          hideHomeBoot: () => events.push('fade'), message: () => events.push('error')}});
      vm.runInContext(routeSource, r);
      const loading = r.renderRoute(path); await flush();
      const paired = ['stats', 'history'].includes(view);
      assert.deepEqual(events, paired ? ['show', 'markup', 'data'] :
        ['show', 'markup', 'status'], view + ': fetch data while lazy markup loads');
      markup.resolve(); await flush();
      assert.equal(bodyClasses.has('homeView'), view === 'home', 'Only Home uses the scrolling desktop header');
      assert.equal(events.includes('fade'), false, view + ': wait for initial data');
      status.resolve(true); await flush();
      if (['stats', 'history', 'diagnostic'].includes(view)) {
        assert.deepEqual(events, paired ? ['show', 'markup', 'data'] : ['show', 'markup', 'status', 'data']);
        data.resolve(true);
      }
      await loading;
      assert.equal(events.at(-1), 'fade', view + ': fade only after all initial data');

      r.ensureView = async () => {throw new Error('Markup unavailable');};
      const beforeFailure = events.length;
      await r.renderRoute(path);
      assert.equal(events.slice(beforeFailure).includes('error'), true, view + ': expose lazy-load failure');
      assert.equal(events.slice(beforeFailure).includes('fade'), false, view + ': failed markup cannot reveal the page');
      r.ensureView = async () => {};
      const loadName = view === 'stats' ? 'startStatsStream' : view === 'history' ? 'startHistoryStream' : 'loadStatus';
      r.R[loadName] = async () => false;
      await r.renderRoute(path);
      assert.equal(events.at(-1), 'error', view + ': expose initial data failure');
      const old = deferred(); r.R[loadName] = () => old.promise;
      const superseded = r.renderRoute(path); await flush();
      r.routeSeq++; old.resolve(true); await superseded;
      assert.notEqual(events.at(-1), 'fade', view + ': ignore superseded route readiness');
      r.R[loadName] = async () => true;
      r.ensureView = async () => {events.push('markup');};
      r.R.setViewPollHooks = hooks => {r.viewHooks = hooks;};
      vm.runInContext(hooksSource, r);
      const beforeReload = events.length;
      await r.viewHooks.start(view);
      assert.equal(events[beforeReload], 'show', view + ': Reload re-enters loading lifecycle');
      assert.equal(events[beforeReload + 1], 'markup', view + ': retry lazy markup after failure');
      assert.equal(events.at(-1), 'fade');
      let queued;
      r.R.withPollGate = fn => {queued = fn; return Promise.resolve();};
      r.startView(view); r.routeSeq++;
      const beforeQueued = events.length;
      assert.equal(await queued(), false);
      assert.equal(events.length, beforeQueued, view + ': discard queued work for an earlier visit to the same page');
    }
  })().catch(error => {console.error(error); process.exitCode = 1;});
}
{
  const fade = Number(css.match(/\.bootOverlay\{[^}]*transition:opacity ([\d.]+)s\}/)[1]);
  // The view and every descendant, each with its ::before and ::after.
  const suppression = css.match(/body\.pageLoading :is\(\.view,\.view \*\)[^}]*\}/)?.[0] || '';
  for (const suffix of ['', '::before', '::after']) {
    if (!suppression.includes('body.pageLoading :is(.view,.view *)' + suffix)) {
      throw new Error('Loading must suppress view and control pseudo-element animations');
    }
  }
  if (!suppression.includes('transition:none!important;animation:none!important') ||
      suppression.includes('bootWave')) {
    throw new Error('Loading must suppress control motion while preserving the loading wave');
  }
  if (fade !== .25 ||
      !codeIncludes(js, "document.body.classList.remove('pageLoading')}}," + Math.round(fade * 1000) + ')') ||
      !css.includes('transition:.01ms!important')) {
    throw new Error('Splash removal must follow the immediate 250 ms fade and restore control motion');
  }
}
if (!shellHtml.includes('type="module"') ||
    !shellHtml.includes('src="/app.js?v=__FW_ASSET_TAG__"') ||
    /<script(?![^>]*\bsrc=)[^>]*>\s*\S/i.test(shellHtml)) {
  throw new Error('Web UI must load same-origin /app.js as a module (no inline script body)');
}
// The firmware CSP (script-src/style-src 'self') blocks inline handlers and style attributes.
if (/<[^>]*\s(?:style|on[a-z]+)\s*=/i.test(allHtml)) {
  throw new Error('Web UI markup must not use inline style attributes or event handlers');
}
// The runtime downloads with app.js instead of after it, and the view the
// page opens on downloads while the ownership claim is in flight.
if (!shellHtml.includes('<link rel="modulepreload" href="/js/runtime.js?v=__FW_ASSET_TAG__">') ||
    !codeIncludes(appJsSource, 'import * as R from "/js/runtime.js?v=__FW_ASSET_TAG__";') ||
    !codeIncludes(appJsSource, 'bootFetch=[bootView,fetchView(bootView)];bootFetch[1].catch(()=>{});}if(bootView==="setup"){') ||
    !codeIncludes(appJsSource, 'const boot=bootFetch?.[0]===name&&bootFetch[1];if(boot)bootFetch=null;')) {
  throw new Error('Shell must preload the runtime app.js imports, and the boot view must load alongside the claim');
}
if (!shellHtml.includes('rel="manifest" href="/manifest.webmanifest?v=__FW_ASSET_TAG__"') ||
    !shellHtml.includes('rel="apple-touch-icon"') ||
    !shellHtml.includes('href="/icons/icon-192.png?v=__FW_ASSET_TAG__"') ||
    !shellHtml.includes('name="theme-color"') ||
    !shellHtml.includes('name="apple-mobile-web-app-capable"')) {
  throw new Error('Shell head must declare the PWA manifest, icon, and home-screen metadata');
}
for (const name of VIEW_NAMES) {
  if (!shellHtml.includes('id="view-' + name + '"') ||
      !shellHtml.includes('data-view="' + name + '"') ||
      !shellHtml.includes(`<section id="view-${name}" class="view" data-view="${name}"></section>`)) {
    throw new Error('Shell must keep empty placeholder for view: ' + name);
  }
}

if (!/lang="en"/.test(html) || !codeIncludes(ui, 'role="switch"') ||
    !codeIncludes(ui, 'id="dActivator"') || !codeIncludes(ui, 'firstDropBeep') ||
    !codeIncludes(ui, 'paddleReturnReminderBeep') ||
    !codeIncludes(ui, 'buzzerScaleLostBeep') ||
    !codeIncludes(ui, 'buzzerAutoToManualGuardEndBeep') ||
    !codeIncludes(ui, 'buzzerManualNoScaleBeep') ||
    !codeIncludes(ui, 'buzzerScaleConnectedBeep') ||
    !codeIncludes(ui, 'scaleConnectedLed') ||
    !codeIncludes(ui, 'buzzerExtendedPulseRate') ||
    !codeIncludes(ui, 'buzzerSlowExtendedPulseRate') ||
    !html.includes('id="buzzerExtendedPulseRate"') ||
    !html.includes('id="buzzerSlowExtendedPulseRate"') ||
    !html.includes('class="buzzerOpt scaleIncapableOpt">Fast-shot beeps<select id="buzzerExtendedPulseRate"') ||
    !html.includes('class="buzzerOpt scaleIncapableOpt">Slow-shot beeps<select id="buzzerSlowExtendedPulseRate"') ||
    !css.includes('color-scheme:light dark') ||
    // color-scheme is inherited, so the root carries it to every form control.
    !css.includes('html.theme-dark{color-scheme:dark}') ||
    !css.includes('html.theme-light{color-scheme:light}') ||
    !css.includes('--in:#fafafa') ||
    !css.includes('input:not([type=file],[type=checkbox]),select,textarea{') ||
    !css.includes('input[type=text],input[type=email],input[type=password],input[type=url]{appearance:none}') ||
    !css.includes('min-height:3rem') ||
    !css.includes('background:var(--in)') ||
    !css.includes('input:-webkit-autofill') ||
    !css.includes('-webkit-box-shadow:0 0 0 3rem var(--in) inset') ||
    css.includes('input[type=text],input[type=password],select{-webkit-appearance:none') ||
    html.includes('id="staSsid" type="number"') ||
    html.includes('id="ntpServerCustom" type="number"') ||
    !html.includes('id="staSsid" type="text" maxlength="32" autocomplete="off"') ||
    !html.includes('id="ntpServerCustom" type="text" maxlength="63" placeholder="e.g. ntp.example.com" autocomplete="off"') ||
    !html.includes('> Scale lost<small class="fieldHint">Warns when the scale disconnects') ||
    html.includes('Scale lost (BBW)') ||
    !html.includes('option value="fast" selected') ||
    !html.includes('option value="rapid">Rapid') ||
    html.includes('id="buzzerExtendedPulseBeep"') ||
    html.includes('20ms') ||
    html.includes('x segundo') ||
    codeIncludes(ui, 'querySelectorAll(\'.scaleIncapableOpt\').forEach(e=>{e.classList.toggle(\'fieldOff\',scaleOnly);e.querySelectorAll(\'input\').forEach') ||
    !codeIncludes(ui, 'alertOutputChannel') ||
    !codeIncludes(ui, 'buzzerSupported') ||
    !codeIncludes(ui, 'Play alerts on') ||
    !codeIncludes(ui, 'scale_priority') ||
    !codeIncludes(ui, 'Buzzer only') ||
    !codeIncludes(ui, 'class="fieldHint"') ||
    !codeIncludes(ui, 'id="bookooMuteOnBuzzerOnly"') ||
    !codeIncludes(ui, 'id="bookooConnectBeepLevel"') ||
    !html.includes('id="bookooMuteOnBuzzerOnly" type="checkbox" checked') ||
    !html.includes('id="buzzerScaleConnectedBeep" type="checkbox" checked') ||
    !html.includes('id="scaleConnectedLed" type="checkbox" checked') ||
    !html.includes('Blue light while connected') ||
    !html.includes('</div><label><input id="scaleConnectedLed"') ||
    !html.includes('class="buzzerOpt scaleIncapableOpt"><input id="buzzerScaleConnectedBeep"') ||
    html.includes('buzzerOnlyOpt') ||
    !html.includes('option value="4" selected') ||
    !html.includes('Keeps the Bookoo silent so alerts play on the buzzer') ||
    !html.includes('Alert volume on the Bookoo speaker (<strong>Scale first or Scale only</strong>).') ||
    !html.includes('<strong>Needs tare at shot start.</strong>') ||
    !html.includes('Applies when <strong>Buzzer only</strong> is selected.')) {
  throw new Error('Web UI must show paddle state, scale beep options, and buzzer alerts');
}
if (!codeIncludes(ui, 'id="operationalWallS" type="number" min="5" max="60"') ||
    !codeIncludes(ui, 'Max shot time (s)') ||
    !codeIncludes(ui, 'sToMs(') ||
    !codeIncludes(ui, 'rinseGestureMs:sToMs') ||
    !network.includes('Max shot time must be from 5 to 60 s.')) {
  throw new Error('Max shot time must be capped at 60 s in the UI and API messages');
}
if (!codeIncludes(ui, 'function rangeCheck(') ||
    !codeIncludes(ui, 'function showFieldError(') ||
    !codeIncludes(ui, 'samples × sample gap') ||
    !codeIncludes(ui, 'aria-live') ||
    !codeIncludes(ui, 'fieldError') ||
    !codeIncludes(ui, 'id="goalWeightG" type="number" min="10" max="200" step="0.1"') ||
    !codeIncludes(ui, 'validateNetworkClient') ||
    !codeIncludes(ui, 'validateDevicePasswordClient') ||
    !network.includes('Extra yield for fast shots must be 0.5 to 10 g') ||
    !network.includes('The shot time window must start at least 2 s') ||
    !network.includes('SSID must be 1–32 characters.') ||
    !network.includes('configValidationErrorName(error)')) {
  throw new Error('UI/API must expose specific validation ranges, inline errors, and field-aware config errors');
}
if (!network.includes('"firstDropBeep"') ||
    !network.includes('"soundAlertsEnabled"') ||
    !network.includes('"paddleReturnReminderBeep"') ||
    !network.includes('"buzzerScaleLostBeep"') ||
    !network.includes('"buzzerAutoToManualGuardEndBeep"') ||
    !network.includes('"buzzerManualNoScaleBeep"') ||
    !network.includes('"buzzerScaleConnectedBeep"') ||
    !network.includes('"scaleConnectedLed"') ||
    !network.includes('"buzzerExtendedPulseRate"') ||
    !network.includes('"buzzerSlowExtendedPulseRate"') ||
    !network.includes('"alertOutputChannel"') ||
    !network.includes('"bookooMuteOnBuzzerOnly"') ||
    !network.includes('"bookooConnectBeepLevel"') ||
    !network.includes('"baseRevision"') ||
    !network.includes('jsonFieldPresent') ||
    !network.includes('CONFIG_REVISION_STALE') ||
    !network.includes('settingFieldCount') ||
    !network.includes('allowedCount > kSeenWords * 64U') ||
    !network.includes('uint64_t seen[kSeenWords]') ||
    !network.includes('WEB_UI_ETAG') ||
    !network.includes('fields.field("buzzerSupported"') ||
    !network.includes('BUZZER_SUPPORT_ENABLED') ||
    !network.includes('"autoRetare"') ||
    !network.includes('"retareWindowMs"') ||
    !network.includes('"minimumCupWeightG"') ||
    !network.includes('"retareStabilitySamples"') ||
    !network.includes('"retareStabilityToleranceG"') ||
    !network.includes('"retareStabilityMaxGapMs"') ||
    !network.includes('"retareStabilityMinDurationMs"') ||
    !network.includes('"bbwProtectionMs"') ||
    !network.includes('"fastExtractionGuardEnabled"') ||
    !network.includes('"avoidAccidentalTouchEnabled"') ||
    !network.includes('"fastMarginG"') ||
    !network.includes('"fastBeforeS"') ||
    !network.includes('"slowExtractionGuardEnabled"') ||
    !network.includes('"slowMarginG"') ||
    !network.includes('"slowAfterS"') ||
    !network.includes('\\"extractionExtended\\"') ||
    !network.includes('\\"stopDetail\\"') ||
    !network.includes('"paddleReturnReminderIntervalMs"') ||
    !network.includes('"paddleReturnReminderMaxDurationMs"') ||
    !network.includes('"timezoneId"') ||
    !network.includes('"ntpServerPreset"') ||
    !network.includes('"ntpServerCustom"') ||
    !network.includes('\\"time\\":{') ||
    !codeIncludes(ui, 'id=ut') ||
    !codeIncludes(ui, 'id="ntpStatus"') ||
    !codeIncludes(ui, 'id="ntpServerPreset"') ||
    !codeIncludes(ui, 'id="ntpServerCustom"') ||
    !html.includes('id="staSsid" type="text"') ||
    !html.includes('id="ntpServerCustom" type="text"') ||
    !html.includes('id="presetRenameInput" type="text"') ||
    !codeIncludes(ui, 'id="syncTimeButton"') ||
    !codeIncludes(ui, 'id="autoRetare"') ||
    !codeIncludes(ui, 'id="fastExtractionGuardEnabled"') ||
    !codeIncludes(ui, 'id="avoidAccidentalTouchEnabled"') ||
    !codeIncludes(ui, 'id="fastMarginG"') ||
    !codeIncludes(ui, 'id="fastBeforeS"') ||
    !codeIncludes(ui, 'Fast shots') ||
    !codeIncludes(ui, 'id="slowExtractionGuardEnabled"') ||
    !codeIncludes(ui, 'id="slowMarginG"') ||
    !codeIncludes(ui, 'id="slowAfterS"') ||
    !codeIncludes(ui, 'Slow shots') ||
    !codeIncludes(ui, 'id="retareWindowS"') ||
    !codeIncludes(ui, 'id="minimumCupWeightG"') ||
    !codeIncludes(ui, 'id="retareStabilitySamples"') ||
    !codeIncludes(ui, 'id="retareStabilityToleranceG"') ||
    !codeIncludes(ui, 'id="retareStabilityMaxGapS"') ||
    !codeIncludes(ui, 'id="retareStabilityMinDurationS"') ||
    !codeIncludes(ui, 'Earliest weight stop (s)') ||
    !codeIncludes(ui, 'id="bbwProtectionS"') ||
    !codeIncludes(ui, 'Paddle reminder limit (min)') ||
    !codeIncludes(ui, 'id="paddleReturnReminderMaxDurationMin"') ||
    !codeIncludes(ui, 'paddleReturnReminderMaxDurationMs:reminder?Math.round(') ||
    codeIncludes(ui, 'Up to 120 shots') ||
    !codeIncludes(ui, '/api/v1/time/sync') ||
    !firmware.includes('session.config.firstDropBeep') ||
    !firmware.includes('candidate.buzzerScaleConnectedBeep') ||
    !firmware.includes('candidate.scaleConnectedLed') ||
    !firmware.includes('candidate.buzzerSlowExtendedPulseRate') ||
    !firmware.includes('localBuzzer') ||
    !firmware.includes('BUZZER_SUPPORT_ENABLED') ||
    !firmware.includes('BUZZER_GPIO') ||
    !firmware.includes('machineServiceReminders')) {
  throw new Error('Scale beep settings must be configurable end-to-end');
}
if (firmware.includes('OPEN_BREW_BY_WEIGHT_ENABLE_ALED') ||
    firmware.includes('WS2812') ||
    firmware.includes('rgbLedWrite') ||
    firmware.includes('status_indicator') ||
    domain.includes('OPEN_BREW_BY_WEIGHT_ENABLE_ALED') ||
    domain.includes('BOOT_SUBSYSTEM_INDICATORS')) {
  throw new Error('WS2812B/ALED support must be fully removed');
}
if (!codeIncludes(ui, 'id="bullseyeMelodyEnabled"') ||
    !codeIncludes(ui, 'id="bullseyeRtttl" maxlength="500"') ||
    !codeIncludes(ui, 'Bullseye melody') ||
    !codeIncludes(js, 'bullseyeMelodyEnabled') ||
    !codeIncludes(js, 'bullseyeRtttl') ||
    !network.includes('"bullseyeMelodyEnabled"') ||
    !network.includes('"bullseyeRtttl"') ||
    !network.includes('stageBullseyeConfig') ||
    !firmwareCore.includes('serviceBullseyeMelody') ||
    !firmwareCore.includes('AlertOutputChannel::BUZZER_ONLY') ||
    !firmwareCore.includes('BULLSEYE_STABILITY_MS')) {
  throw new Error('Bullseye RTTTL alert must be configurable end-to-end');
}
{
  const start = network.indexOf(
      'esp_err_t OpenBrewByWeightNetwork::bullseyeTestHandler');
  const end = network.indexOf(
      'esp_err_t OpenBrewByWeightNetwork::preferredScaleClearHandler', start);
  const handler = start >= 0 && end > start ? network.slice(start, end) : '';
  const unlocks = handler.match(/self\.unlockJsonBody\(\)/g) || [];
  if (unlocks.length < 2) {
    throw new Error(
        'Bullseye test handler must release the shared PSRAM/JSON workspace on success and parse failure');
  }
}
if (!codeIncludes(ui, "scaleConnectedLed:$('scaleConnectedLed').checked") ||
    !codeIncludes(ui, "'scaleConnectedLed'") ||
    !network.includes('fields.field("config.scaleConnectedLed"') ||
    !firmware.includes('serviceScaleConnectedLed') ||
    !firmware.includes('SCALE_CONNECTED_LED_GPIO') ||
    !domain.includes('bool scaleConnectedLed = true')) {
  throw new Error('Scale-connected GPIO LED must be wired through Settings, the Settings stream, and firmware');
}

if (!codeIncludes(ui, 'id="soundAlertsEnabled"') ||
    codeIncludes(ui, 'id="homeSoundAlertsEnabled"') ||
    codeIncludes(ui, 'id="homeAlertsSub"') ||
    codeIncludes(ui, "setHomeSub('homeAlertsSub'") ||
    codeIncludes(ui, 'function formatAlertsChannel(') ||
    !codeIncludes(ui, 'soundAlertsEnabled:$(\'soundAlertsEnabled\').checked') ||
    codeIncludes(ui, 'p.soundAlertsEnabled=$(\'homeSoundAlertsEnabled\').checked') ||
    codeIncludes(ui, "k!=='soundAlertsEnabled'") ||
    codeIncludes(js, "keys[0]==='soundAlertsEnabled'") ||
    !(codeIncludes(ui, "typeof c.soundAlertsEnabled==='boolean'") ||
      codeIncludes(ui, "'boolean'==typeof c.soundAlertsEnabled"))) {
  throw new Error('Sound alerts must be controlled from Settings, not Home Quick Settings');
}

if (html.indexOf('<summary>Tare</summary>') >
        html.indexOf('<summary>Cup detection</summary>') ||
    html.indexOf('<summary>Cup detection</summary>') >
        html.indexOf('<summary>Cup protection</summary>') ||
    !codeIncludes(ui, 'id="cupProtectionEnabled"') ||
    html.indexOf('id="cupProtectionEnabled"') >
        html.indexOf('id="stopIfCupRemoved"') ||
    html.indexOf('id="stopIfCupRemoved"') >
        html.indexOf('id="requireCupToStart"') ||
    html.indexOf('id="requireCupToStart"') >
        html.indexOf('<summary>Scale guard</summary>') ||
    !codeIncludes(ui, 'id="stopIfCupRemoved"') ||
    !codeIncludes(ui, 'id="requireCupToStart"') ||
    codeIncludes(ui, 'id="cupPresentWeightG"') ||
    html.includes('id="cupPresentWeightG"') ||
    html.indexOf('id="cupRemovedWeightG"') <
        html.indexOf('<summary>Cup detection</summary>') ||
    html.indexOf('id="cupRemovedWeightG"') >
        html.indexOf('<summary>Cup protection</summary>') ||
    html.includes('id="requireCupToStart" type="checkbox" checked') ||
    !codeIncludes(ui, 'id="cupProtectionEnabled" type="checkbox" checked> Cup protection') ||
    !codeIncludes(ui, 'cupProtectOpt') ||
    !codeIncludes(ui, 'Already on? Lift and replace it.') ||
    !network.includes('cupProtectionEnabled') ||
    !network.includes('stopIfCupRemoved') ||
    !network.includes('requireCupToStart') ||
    !network.includes('cupRemovedWeightG')) {
  throw new Error('Cup protection master must precede Stop if cup is removed and Require cup to start');
}

if (codeIncludes(ui, 'bleCompanionEnabled') ||
    codeIncludes(ui, 'bleCompanionPanel') ||
    codeIncludes(ui, '/api/v1/admin/ble-compat') ||
    codeIncludes(ui, 'Companion characteristics') ||
    codeIncludes(ui, 'BLE companion') ||
    codeIncludes(ui, 'ensureBleScanPanel') ||
    network.includes('bleCompanion') ||
    network.includes('WebCommandType::BLE_COMPAT') ||
    networkHeader.includes('bleCompatHandler') ||
    firmwareCore.includes('persistBleCompanionEnabled') ||
    !codeIncludes(ui, '<legend>') || !codeIncludes(ui, 'Power management') ||
    !codeIncludes(ui, 'ESP32 power management') ||
    !codeIncludes(ui, 'bleScanIntensity') ||
    !codeIncludes(ui, 'BLE scan mode') ||
    codeIncludes(ui, 'Aggressive 100%') ||
    codeIncludes(ui, 'Normal 50%') ||
    codeIncludes(ui, 'Light 25%') ||
    !codeIncludes(ui, 'How much radio time is spent searching for Bluetooth espresso scales') ||
    !codeIncludes(ui, "scanIntensity:wanted") ||
    codeIncludes(ui, 'bleScanBackoff') ||
    codeIncludes(ui, 'Idle scan backoff') ||
    codeIncludes(ui, "backoffMin:wanted") ||
    codeIncludes(ui, 'bleScanBoost') ||
    codeIncludes(ui, 'Scan boost on machine use') ||
    codeIncludes(ui, "boostMin:wanted") ||
    !codeIncludes(ui, 'bleEnabled') ||
    !codeIncludes(ui, 'Enable Bluetooth') ||
    !codeIncludes(ui, "enabled:wanted") ||
    !codeIncludes(ui, 'Master switch that enables or disables Bluetooth connections to scales.') ||
    !network.includes('BleScanCommandPayload::ENABLED') ||
    !firmwareCore.includes('void persistBleScanEnabled') ||
    !firmwareCore.includes('applyLiveBleEnabled') ||
    !codeIncludes(ui, '/api/v1/admin/ble-scan') ||
    !codeIncludes(ui, "method:'PUT'") ||
    !network.includes('scanIntensity') ||
    network.includes('backoffMin') ||
    network.includes('boostMin') ||
    !network.includes('WebCommandType::BLE_SCAN_INTENSITY') ||
    !network.includes('command.type = WebCommandType::BLE_SCAN_INTENSITY') ||
    !firmwareCore.includes('void persistBleScanIntensity') ||
    firmwareCore.includes('void persistBleScanBackoff') ||
    firmwareCore.includes('void persistBleScanBoost') ||
    !networkHeader.includes('bleScanHandler')) {
    throw new Error('Power management Admin controls must keep live scan mode and the BLE master switch with the timed backoff/boost controls removed');
}
{
  const persistStart = firmwareCore.indexOf(
      'void persistBleScanIntensity(BleScanIntensity intensity) {');
  const persistEnd = persistStart >= 0
      ? firmwareCore.indexOf('\n}', persistStart)
      : -1;
  const persist = persistStart >= 0 && persistEnd > persistStart
      ? firmwareCore.slice(persistStart, persistEnd + 2)
      : '';
  if (!persist.includes('applyLiveBleScanIntensity') ||
      persist.includes('restartRequired')) {
    throw new Error(
        'PUT scanIntensity must apply live and must not set restartRequired');
  }
}
if (!domain.includes('DebugCode::SCALE_SCAN_STARTED') ||
    !domain.includes('DebugCode::SCALE_GATT_CONNECTING') ||
    !domain.includes('DebugCode::SCALE_CONNECT_ATTEMPT_FAILED') ||
    !domain.includes('DebugCode::SCALE_CONNECT_FAILED') ||
    !domain.includes('scale connect failed: %s (step=%s)') ||
    !firmware.includes('logScaleScanStarted') ||
    !firmware.includes('static_cast<int32_t>(discoveryScanIntensity())') ||
    !firmware.includes('SCALE_GATT_CONNECTING') ||
    firmware.includes('addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_CONNECTING)')) {
  throw new Error(
      'Scale discovery debug must report the applied scan duty, GATT connect, attempt, and fail reason');
}
if (!domain.includes('inline bool scaleHistoryIdentityEqual') ||
    !domain.includes('inline ScaleMacNvsAction decideScaleMacNvsAction') ||
    !firmwareCore.includes('decideScaleMacNvsAction(') ||
    !firmwareCore.includes(
        'scaleHistoryIdentityEqual(persistedSettings.scaleHistory') ||
    firmwareCore.includes(
        'memcmp(persistedSettings.scaleHistory, history, sizeof(history))') ||
    firmwareCore.includes(
        'memcmp(persistedSettings.scaleHistory, liveHistory')) {
  throw new Error(
      'Scale MAC NVS must ignore lastSeenSeq and must not write flash while GATT is live');
}
if (!domain.includes('BUZZER_SUPPORT_ENABLED = OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER != 0') ||
    !domain.includes('OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER must be 0 (off) or 1 (passive RTTTL)') ||
    !domain.includes('compiledBuzzerModeId') ||
    !domain.includes('DEFAULT_ALERT_OUTPUT_CHANNEL') ||
    !domain.includes(
        'BUZZER_SUPPORT_ENABLED ? AlertOutputChannel::BUZZER_ONLY') ||
    !domain.includes('static_cast<uint8_t>(DEFAULT_ALERT_OUTPUT_CHANNEL)') ||
    !buzzer.includes('requestTone') ||
    !buzzerPassive.includes('ledcAttach') ||
    !rtttlParser.includes('parseRtttl') ||
    !buzzerPatterns.includes('BUZZER_ECHO_INVERTED_NOTES') ||
    buzzerPatterns.includes('buzzerActiveSetTone') ||
    !buzzerRtttl.includes('RTTTL_TARE') ||
    !buzzerRtttl.includes('RTTTL_START_TIMER') ||
    !buzzerRtttl.includes('RTTTL_STOP_TIMER') ||
    !buzzerRtttl.includes('RTTTL_TARE_START') ||
    !buzzerRtttl.includes('RTTTL_FIRST_DROP') ||
    !buzzerRtttl.includes('RTTTL_PADDLE_OFF') ||
    !buzzerRtttl.includes('RTTTL_SHOT_END') ||
    !buzzerRtttl.includes('RTTTL_SCALE_CONNECTED') ||
    !buzzerRtttl.includes('RTTTL_NO_CUP') ||
    !buzzerRtttl.includes('RTTTL_EXTENDED_PULSE_SLOW') ||
    !buzzerRtttl.includes('RTTTL_EXTENDED_PULSE_MEDIUM') ||
    !buzzerRtttl.includes('RTTTL_EXTENDED_PULSE_FAST') ||
    !buzzerRtttl.includes('RTTTL_EXTENDED_PULSE_RAPID') ||
    buzzerRtttl.includes('OpenBrewByWeightDomain.h') ||
    buzzerRtttl.includes('rtttlForExtendedPulseRate') ||
    buzzerPassive.includes('OpenBrewByWeightBuzzerRtttl.h') ||
    !buzzerRtttl.includes('RTTTL_SCALE_LOST') ||
    !buzzerRtttl.includes('RTTTL_GUARD_STOP') ||
    !buzzerRtttl.includes('RTTTL_NO_SCALE') ||
    !buzzerRtttl.includes('RTTTL_RECOVERY_START') ||
    !buzzerRtttl.includes('RTTTL_NETWORK_RESET_OK') ||
    !buzzerRtttl.includes('RTTTL_FACTORY_RESET_OK') ||
    !buzzerRtttl.includes('RTTTL_RECOVERY_ERROR') ||
    !alertChannel.includes('selectAlertSink') ||
    !alertChannel.includes('AlertKind::Recovery') ||
    !alertChannel.includes('AlertKind::CommandImmediate') ||
    !alertChannel.includes('AlertKind::CommandFallback') ||
    !alertTone.includes('deriveBuzzerTone') ||
    !alertTone.includes('buzzerCueForAlertEvent') ||
    !alertTone.includes('rtttlForExtendedPulseRate') ||
    alertTone.includes('buzzerPatternForCue') ||
    !buzzer.includes('startRtttl') ||
    buzzer.includes('BUZZER_ACTIVE_DRIVE') ||
    !buzzer.includes('memcpy(rtttlBuf') ||
    !buzzer.includes('stopExtendedPulse') ||
    !domain.includes('ECHO_INVERTED') ||
    !firmware.includes('startExtendedPulseTrain') ||
    !firmware.includes('startPulseTrain') ||
    !firmware.includes('stopPulseTrains') ||
    !firmware.includes('serviceExtendedPulseAlert') ||
    firmwareCore.includes('rtttlForExtendedPulseRate') ||
    firmwareCore.includes('BuzzerCue::ABNORMAL') ||
    !domain.includes('DEFAULT_EXTENDED_PULSE_RATE') ||
    !firmware.includes('localBuzzer.request(command.buzzerPattern)') ||
    !kconfig.includes('0=off, 1=passive RTTTL')) {
  throw new Error('Local buzzer must be compile-time off (0) or passive RTTTL (1)');
}
if (!domainCore.includes('#ifndef OPEN_BREW_BY_WEIGHT_DEVELOPMENT') ||
    !domainCore.includes('#define OPEN_BREW_BY_WEIGHT_DEVELOPMENT 0') ||
    !domainCore.includes('DEVELOPMENT_BUILD = OPEN_BREW_BY_WEIGHT_DEVELOPMENT == 1') ||
    !domainCore.includes('OPEN_BREW_BY_WEIGHT_DEVELOPMENT must be 0 or 1') ||
    !domainCore.includes('CONFIG_OPEN_BREW_BY_WEIGHT_DEVELOPMENT') ||
    !kconfig.includes('config OPEN_BREW_BY_WEIGHT_DEVELOPMENT') ||
    !kconfig.includes('bypass WebUI admin unlock') ||
    !network.includes('OPEN_BREW_BY_WEIGHT_DEVELOPMENT == 1') ||
    !network.includes('adminUnlockAllowed') ||
    // Every status page (home, admin, diagnostic, settings) reports the dev
    // flag so the UI can react on first poll; at most two raw JSON emissions
    // per page may share the literal (e.g. nested objects). A formatting
    // refactor must not turn this into a count of source lines.
    Object.values(network.match(/\\"development\\":%s/g) || [])
        .length > 8 ||
    !codeIncludes(js, 'developmentMode') ||
    !codeIncludes(js, "'development'in s") ||
    !codeIncludes(js, 'if(!developmentMode)syncAdminSessionUi(false)') ||
    !codeIncludes(js, 'if(developmentMode||$(\'uiOverridePanel\'))return;') ||
    !codeIncludes(js, "classList.toggle('devBuild',developmentMode)") ||
    // Session visibility is driven by the actual unlock state, and
    // development builds treat administration as always unlocked; assert the
    // behavior (visible when either condition holds), not minified names.
    !/const\s+on\s*=\s*!!unlocked\s*\|\|\s*developmentMode/.test(js) ||
    !codeIncludes(js, "if(R.developmentActive())return;") ||
    !rawShellHtml.includes('class="{{webui-meta:development-class}}"') ||
    !css.includes('body.devBuild #adminLockPanel')) {
  throw new Error(
      'OPEN_BREW_BY_WEIGHT_DEVELOPMENT must default off, make administration public by compile flag, and never flash the locked admin UI');
}
if (!domainCore.includes('#ifndef OPEN_BREW_BY_WEIGHT_ENABLE_JTAG') ||
    !domainCore.includes('#define OPEN_BREW_BY_WEIGHT_ENABLE_JTAG 0') ||
    !domainCore.includes(
        'OPEN_BREW_BY_WEIGHT_ENABLE_JTAG must be 0 (off) or 1 (USB Serial/JTAG)') ||
    !domainCore.includes('CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_JTAG') ||
    !domainCore.includes('enum class UsbSerialEnableSource') ||
    !domainCore.includes('OFF = 0') ||
    !domainCore.includes('COMPILE_FLAG = 1') ||
    !domainCore.includes('JUMPER = 2') ||
    domainCore.includes('DISABLED = 0') ||
    domainCore.includes('UsbSerialEnableSource::DISABLED') ||
    domainCore.includes('JTAG = 1') ||
    domainCore.includes('IO4 = 2') ||
    !domainCore.includes('usbSerialStateId') ||
    !domainCore.includes('usbConsoleIo4StateId') ||
    !kconfig.includes('config OPEN_BREW_BY_WEIGHT_ENABLE_JTAG') ||
    !kconfig.includes('USB Serial/JTAG (0=off, 1=on at boot)') ||
    !kconfig.includes('omitting the flag leaves JTAG off')) {
  throw new Error(
      'OPEN_BREW_BY_WEIGHT_ENABLE_JTAG must default off and map Kconfig onto the compile macro');
}
{
  const remoteKconfig = kconfig.slice(
      kconfig.indexOf('config OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL'),
      kconfig.indexOf('config OPEN_BREW_BY_WEIGHT_MACHINE_TYPE'));
  const cli = fs.readFileSync(
      path.resolve(sketchDir, '..', 'scripts', 'openbrewbyweight_cli.sh'), 'utf8');
  const defaultFlags = cli.match(/OBBW_CLI_DEFAULT_FLAGS='([^']*)'/);
  const paddleHandler = network.slice(
      network.indexOf('esp_err_t OpenBrewByWeightNetwork::paddleHandler'),
      network.indexOf('esp_err_t OpenBrewByWeightNetwork::rinseHandler'));
  const rinseHandler = network.slice(
      network.indexOf('esp_err_t OpenBrewByWeightNetwork::rinseHandler'),
      network.indexOf('esp_err_t OpenBrewByWeightNetwork::stopHandler'));
  const remoteOn = firmwareCore.slice(
      firmwareCore.indexOf('case WebCommandType::REMOTE_ON:'),
      firmwareCore.indexOf('case WebCommandType::RINSE:'));
  const webRinse = firmwareCore.slice(
      firmwareCore.indexOf('case WebCommandType::RINSE:'),
      firmwareCore.indexOf('case WebCommandType::APPLY_CONFIG:'));
  const lockdownTest = fs.readFileSync(
      path.join(sketchDir, 'tests', 'remote_lockdown_host_test.cpp'), 'utf8');
  const hostTests = fs.readFileSync(
      path.join(sketchDir, 'tests', 'CMakeLists.txt'), 'utf8');
  if (remoteKconfig.length < 80 ||
      !remoteKconfig.includes('default n') ||
      /\bdefault y\b/.test(remoteKconfig) ||
      sdkconfigDefaults.includes(
          'CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=y') ||
      !defaultFlags ||
      defaultFlags[1].includes('OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=1') ||
      !paddleHandler.includes('on && !REMOTE_MACHINE_CONTROL_ENABLED') ||
      !paddleHandler.includes('REMOTE_CONTROL_DISABLED') ||
      !rinseHandler.includes('!REMOTE_MACHINE_CONTROL_ENABLED') ||
      !rinseHandler.includes('REMOTE_CONTROL_DISABLED') ||
      !remoteOn.includes('!REMOTE_MACHINE_CONTROL_ENABLED') ||
      !webRinse.includes('!REMOTE_MACHINE_CONTROL_ENABLED') ||
      !lockdownTest.includes('#define OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL 0') ||
      !lockdownTest.includes('WebCommandType::REMOTE_ON') ||
      !lockdownTest.includes('WebCommandType::RINSE') ||
      !hostTests.includes('remote_lockdown_host_test')) {
    throw new Error(
        'Remote machine control must stay opt-in (Kconfig/sdkconfig/CLI default off) with HTTP and processWebCommand guards');
  }
}
if (codeIncludes(ui, 'authenticatedOnly') ||
    codeIncludes(ui, "s.setItem('openBrewByWeightToken'") ||
    codeIncludes(ui, 'pageNav authenticatedOnly') ||
    codeIncludes(ui, "authenticated()&&known") ||
    !codeIncludes(ui, 'function knownPath(') ||
    !codeIncludes(ui, 'class="brand"') ||
    !codeIncludes(ui, 'class="brandMark"') ||
    !codeIncludes(ui, '<small>Open Brew</small>by Weight') ||
    !codeIncludes(ui, 'href="/" data-route="/"') ||
    !codeIncludes(ui, "querySelectorAll('a[data-route]')") ||
    !codeIncludes(ui, 'ensureView') ||
    !codeIncludes(ui, 'section.innerHTML=mod.markup') ||
    codeIncludes(ui, '/partials/') ||
    !network.includes('HTTPD_404_NOT_FOUND') ||
    !network.includes('notFoundHandler')) {
  throw new Error('Web UI must expose public SPA routes and redirect unknown paths to /');
}
if (html.includes('id="rememberMe"') ||
    codeIncludes(js, 'rememberMe:r') ||
    codeIncludes(js, "s.setItem('openBrewByWeightToken'") ||
    codeIncludes(js, 'function clearAuth()') ||
    network.includes('jsonBoolean(root, "rememberMe", rememberMe)') ||
    network.includes('createSession(token, csrf, rememberMe)') ||
    network.includes('uiAuthenticated') ||
    networkHeader.includes('SESSION_REMEMBER_MS')) {
  throw new Error(
      'Web UI must not use login tokens; exclusive WebUI claim owns the session');
}
const statusSection = html.match(/<details id="machineRow" class="lampRow">([\s\S]*?)<\/details>/);
const scaleSection = html.match(/<details id="scaleRow" class="lampRow">([\s\S]*?)<\/details>/);
if (network.includes('Access-Control-Allow-Origin')) {
  throw new Error('LAN API responses must not grant cross-origin browser reads');
}
// The shared API client carries OTA too: it keeps a caller's Content-Type
// (chunks are binary) and reports the HTTP status of a failed or unreadable
// response, and a chunk timeout never waits out the whole OTA session.
if (!codeIncludes(runtimeJs, "if(options.body)options.headers['Content-Type']??='application/json'") ||
    !codeIncludes(runtimeJs, 'err.status=response.status') ||
    !codeIncludes(runtimeJs, '{status:response.status,}') ||
    !codeIncludes(viewJs.admin, 'const OTA_UPLOAD_TIMEOUT_MS=2*60*1000') ||
    codeIncludes(viewJs.admin, 'XMLHttpRequest')) {
  throw new Error('OTA must ride the shared API client with a bounded chunk timeout');
}
