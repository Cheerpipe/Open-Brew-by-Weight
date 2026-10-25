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
const rawPartialHtml = {};
for (const name of VIEW_NAMES) {
  rawPartialHtml[name] = await compactMarkup(fs.readFileSync(
      path.join(sketchDir, 'web', 'html', name + '.html'), 'utf8'));
}
const rawAppJsSource = fs.readFileSync(path.join(sketchDir, 'web', 'app.js'), 'utf8');
const rawRuntimeJs = fs.readFileSync(path.join(sketchDir, 'web', 'js', 'runtime.js'), 'utf8');
const rawOtaImageJs = fs.readFileSync(
  path.join(sketchDir, 'web', 'js', 'ota-image.js'), 'utf8');
const rawViewJs = {};
for (const name of VIEW_NAMES) {
  rawViewJs[name] = fs.readFileSync(
      path.join(sketchDir, 'web', 'js', name + '.js'), 'utf8');
}
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
const rawCss = compactCss(fs.readFileSync(path.join(sketchDir, 'web', 'app.css'), 'utf8'));
const localizedSources = webUiLocale.renderSources([
  {file: webUi.sourcePath, type: 'html', content: rawShellHtml},
  ...VIEW_NAMES.map((name) => ({file: name + '.html', type: 'html',
    content: rawPartialHtml[name]})),
  {file: 'app.js', type: 'js', content: rawAppJsSource},
  {file: 'runtime.js', type: 'js', content: rawRuntimeJs},
  {file: 'ota-image.js', type: 'js', content: rawOtaImageJs},
  ...VIEW_NAMES.map((name) => ({file: name + '.js', type: 'js',
    content: rawViewJs[name]})),
  {file: 'app.css', type: 'css', content: rawCss},
], {language: 'en'}).sources;
let localizedAt = 0;
const shellHtml = localizedSources[localizedAt++].content;
const partialHtml = {};
for (const name of VIEW_NAMES) partialHtml[name] = localizedSources[localizedAt++].content;
const appJsSource = localizedSources[localizedAt++].content;
const runtimeJs = localizedSources[localizedAt++].content;
// Harnesses that run a slice of the runtime prepend its shared UI state.
const runtimeStateDecl = blockAt(runtimeJs, 'const uiState = {') + ';\n';
// Harnesses that keep state as vm globals alias uiState and R to the global
// object and seed the shared-state defaults they do not set themselves.
const runtimeStateGlobals = 'var uiState = this, R = this;for (const [k, v] of Object.entries(' +
    runtimeStateDecl.replace(/^const uiState = /, '').replace(/;\n$/, '') +
    ')) if (!(k in this)) this[k] = v;\n';
const otaImageJs = localizedSources[localizedAt++].content;
const viewJs = {};
for (const name of VIEW_NAMES) viewJs[name] = localizedSources[localizedAt++].content;
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
const css = localizedSources[localizedAt].content;
const allHtml = shellHtml.replace(
    /<section id="view-([a-z]+)" class="view" data-view="\1"><\/section>/g,
    (_, name) => {
      if (!partialHtml[name]) {
        throw new Error('Missing partial for shell placeholder: ' + name);
      }
      return `<section id="view-${name}" class="view" data-view="${name}">${
          partialHtml[name]}</section>`;
    });
let allJs = [appJsSource, runtimeJs, otaImageJs,
  ...VIEW_NAMES.map((n) => viewJs[n])].join('\n');
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

const htmlBytes = Buffer.byteLength(allHtml, 'utf8');
const jsBytes = Buffer.byteLength(allJs, 'utf8');
// Complete, human-readable settings help is part of the UI contract. Adding a
// setting must raise this allowance when needed; hints must not be cut to fit it.
// The activation-history view (nav link, partial, runtime helpers) adds
// ~1.4 KB of HTML source allowance.
// The grouped Admin Power management panel (ESP32 power management, Wi-Fi
// sleep, BLE scan mode) replaces two JS-built controls with static markup
// and fuller hints, adding ~1.7 KB of HTML source allowance.
// The Admin Network device-name label and hint add ~0.1 KB of HTML source
// allowance.
// Profile-gated Linea Micra cloud account selection, per-preset temperature,
// and read-only machine-state diagnostics add labeled setup help. Wake-gesture
// recognition adds one default-on machine option. Scale-triggered shutdown
// adds one default-off machine option plus a grace-delay select.
// The Home boot splash adds one full-screen status surface plus its label to
// the shell markup; no partial, view, or control markup changes.
// The Home loading view reuses the header brand mark; the repeated inline
// artwork adds ~0.5 KB of shell source but only bytes of compressed payload.
// Scale↔machine power linkage adds two default-off Linea Micra options with
// setup help: power-on with the scale and scale-off with the machine.
// The Diagnostic standalone tare button adds 200 bytes of HTML allowance;
// compressed asset and firmware budgets remain unchanged.
// The Admin BLE master switch's rendered help needs 67 more HTML bytes than
// its original allowance; retain the complete wording.
// Continuous loop timing adds a phase table and a Reset control to Diagnostics.
// Last NTP sync adds one timestamp cell to the Diagnostics grid.
// The Settings Preferred scale row gains its own (rename) link, matching
// Home and Diagnostic: +128 bytes of HTML allowance; compressed asset and
// firmware budgets remain unchanged.
// The scale command inventory adds a small two-column Diagnostic table.
// Per-section revert buttons beside every dirty-tracked save bar (Settings
// brew/machine/Micra, Admin network/date-time/webhooks/device password) add
// ~1.6 KB of HTML source allowance; compressed asset budgets stay fixed.
// Save+revert bars for the Admin BLE, Power, and Frontend groups add ~1 KB
// of HTML source allowance; compressed asset budgets stay fixed.
// The Admin Automatic date & time toggle (NTP sync master switch) adds
// ~0.2 KB of HTML source allowance; compressed asset budgets stay fixed.
// The Admin Firmware master-switch section adds ~1 KB of HTML source
// allowance; compressed asset and firmware budgets stay unchanged.
// Shot stats gain a Tare time and a Scale field: two Home card fields plus
// two Stats table headers add ~0.1 KB of HTML source allowance; the
// compressed asset and firmware budgets stay effectively unchanged.
// Six La Marzocco Cloud diagnostic fields add 0.8 KB of HTML allowance.
// The Stats loading overlay reuses the panel wave and adds ~0.2 KB of HTML source.
// The Scale profiling section adds ~1 KB of HTML allowance (state rows, four
// controls, and the replacement hint); compressed asset budgets stay fixed.
// Independent wireless popups add their two identity and signal rows.
// The supplied four-path Backflush symbol adds 2 KiB of HTML source allowance.
// Stats duration/yield metric icons and the Diagnostic Scale timer metric add
// ~1 KB of HTML source allowance; compressed asset budgets stay fixed.
// Firmware image, memory-region and OTA partition limits remain unchanged.
// The Home full-screen shot card adds two icon buttons, the hero body wrapper,
// and their labels: ~0.8 KB of HTML source allowance; compressed asset and
// firmware budgets stay fixed.
// The Home quick target-weight sheet adds the hero pencil button and the
// inert dialog markup (grab row, stacked preset/label header, value row with
// Reset, tick-scale zone): ~1.4 KB of HTML source allowance; compressed asset
// and firmware budgets stay fixed.
// The /setup onboarding view adds its shell placeholder plus the menu-less
// card partial (brand, scan/form/connecting/success/error/locked states):
// ~1.1 KB of HTML source allowance; compressed asset and firmware budgets
// stay fixed.
// The connecting-state countdown line under the 20 s bound adds ~70 bytes
// of HTML source allowance; compressed asset and firmware budgets stay
// fixed.
// The low-profile password reveal toggle adds its label row and button:
// ~160 bytes of HTML source allowance; compressed asset and firmware
// budgets stay fixed.
// The /setup skip exit adds the quiet form action and the skipped-state card
// with its probe link, and the grace adds the success-screen Done button:
// ~400 bytes of HTML source allowance; compressed asset and firmware budgets
// stay fixed.
// The La Marzocco Micra diagnostic section (readiness, power, mode, brew and
// steam boiler rows, water, cleaning, communication) plus the Home boiler
// rows add ~1.1 KB of HTML source allowance; compressed asset and firmware
// budgets stay fixed.
// The Diagnostics audit panel (summary, hunt/clear buttons, records table)
// adds ~0.6 KB of HTML source allowance; compressed asset and firmware
// budgets remain fixed.
// The audit viewer adds an Export button plus a fuller panel hint:
// +161 measured HTML bytes (91761); compressed budgets re-measured below.
// The unified Stats/History sort controls give History the "Sort" label span
// and a pressed-state Date button: +53 measured HTML bytes (91814); compressed
// budgets re-measured below.
// The audit table pager (prev/next buttons, page label) adds +290 measured
// HTML bytes (92104); compressed budgets re-measured below.
// The smooth-cut preset editor and Home (window pair, review graph host,
// preset action bar with New coffee, scale-loss backup group, pills-only hero,
// grind-hint and discovery cards) add +1011 measured HTML bytes (93115),
// owner-authorized 2026-10-14; compressed budgets re-measured below.
// The Admin webhooks and Diagnostic page option, the Stats sort buttons and the RTTTL test link move from
// JS-built strings into the partials: 93774, measured raise of the 2026-10-21 frontend audit remediation.
// Audit accessibility and form markup (hidden decorative glyphs, named groups and fields, live statuses, unlock form): 94884,
// measured raise of the 2026-10-21 frontend audit remediation.
// The lint gate's markup fixes (quoted attributes, explicit button types, an html-validate directive): 95117,
// measured raise of the 2026-10-21 frontend audit remediation.
// Review fix RV-05 (Admin unlock form leaves validation to the app): measured 95128.
// The budget counts the compact markup the fixtures derive, so formatting the
// sources cannot move it: re-baselined to the measured 94713.
// Diagnostic heap-map download row: measured 94810, owner-requested 2026-10-22.
// Optional Web UI login (sign-in form, Access panel, Home notice, Home
// Assistant pairing prompt): measured 99499, owner-requested 2026-10-22.
// One password form at a time in Admin → Access: measured 99534, owner-requested 2026-10-22.
// Admin unlock reminder while the factory password is in use: measured 99696, owner-requested 2026-10-22.
// Sign out moved to the footer line (Admin row removed): measured 99675, owner-requested 2026-10-22.
// Signed-in device list in Admin → Access (rows built in JS), as an accordion: measured 99526, owner-requested 2026-10-22.
// Settings group rows (icon sprite and group keys for the summaries): measured 102184, owner-requested 2026-10-22.
// Admin sections as collapsible group rows (icon sprite, details for the panels): measured 104225, owner-requested 2026-10-22.
// Diagnostics sections as collapsible rows with icons (sprite, details markup): measured 107310, owner-requested 2026-10-22.
// Shots Export as a CSV/JSON menu (popup trigger attributes): +43, owner-requested 2026-10-25.
// Admin Scheduled restart section (switch, 24-hour time pair, status, save/revert): measured 108304, owner-requested 2026-10-25.
if (htmlBytes > 108304) {
  throw new Error(`Web UI HTML source exceeds the authoring budget (${htmlBytes} > 108304)`);
}
// Resumable OTA hashes File slices incrementally in a lazy module so it never
// retains a full firmware image or charges the normal runtime path for it.
// Historical BLE disconnect/command diagnostics add display formatters. This
// source allowance does not change the compressed asset or firmware budgets.
// Opt-in power control/activity leases add 1 KiB of authoring allowance after
// sharing the Admin toggle persistence path.
// Dirty save-button feedback adds 300 bytes through one shared state helper.
// Zero baselines and the first-drop icon add 1,000 bytes of source allowance.
// Fixed chart grids, adaptive axes, and derived peak flow add 1,200 bytes.
// Continuous smoothed flow-rate curves add 273 source bytes.
// PWA manifest/icon head metadata and the cached-shell version self-heal add
// ~1.1 KB of combined source allowance.
// Activation-history paging, sorting, clear, and per-card delete add
// ~6 KB of JS source allowance.
// The removed Admin idle-scan backoff and machine-use scan boost selects
// once added ~1.7 KB of combined source allowance.
// Activation-history type icons (inline coffee/rinse SVG paths, coordinates
// rounded to one decimal) and their card wiring add ~3.2 KB of JS+combined
// source allowance.
// The Admin device-name field adds client validation, the no-reconnect
// preference save, and the .local status suffix: ~0.7 KB of JS and ~1 KB of
// combined source allowance.
// Humanized shot/activation time labels (relative day ladder plus seven
// locale strings) add ~0.4 KB of JS and combined source allowance.
// The second shot icon (outlined cup for shots without registered weight,
// split out of the coffee/rinse inline set) adds ~2.4 KB of JS+combined
// source allowance.
// Weekday names, short dates, and the hover <time> wrapper add ~0.2 KB of
// combined source allowance.
// The No Scale Guard abort icon adds 600 bytes of History HTML, 100 bytes of
// runtime wiring, and 700 combined authoring bytes. Flash budgets stay fixed.
// Linea Micra account connection, machine selection, settings, refresh, and
// browser-side state expiry add the profile-gated cloud workflow.
// The Home boot splash one-shot hide helper and its non-home boot route keep
// the added runtime and shell logic inside 200 bytes. Painting the splash
// before it fades, holding it through a pending firmware reload, and releasing
// it to the inactive overlay raise that allowance to 194500.
// Machine-type-exclusive builds strip other types' markup at generation, so
// the runtime guards every read, write, validation, and save of a stripped
// element: ~1 KB of null-safe JS that ships in every variant.
// Crash-archive download and confirmed deletion add a bounded browser-only
// path without changing the normal polling payload or firmware compressor.
// The Admin BLE master switch checkbox adds its save handler, status sync,
// and revert-on-error path to the runtime module.
// Matching recent/lifetime gap columns replace the peak-gap label breakdown.
// Rendering the current scale command inventory adds a bounded status update.
// IANA zone loading, selected-zone preview, and first-use detection add ~3 KiB.
// Per-section revert (discard unsaved changes) baselines, snapshot helpers,
// handlers, and button wiring across Settings and Admin add ~5 KB of JS
// source allowance.
// Deferred Admin BLE/Power/Frontend saving moves theme persistence behind
// Save and adds dirty flags, guarded hydration, and save/revert handlers for
// ~4 KB of JS source allowance.
// The firmware master switch adds the Admin Firmware section wiring plus
// compatibility-mode tab/section gating for ~2.5 KB of JS source allowance.
// Mid-upload image guarding records per-4 KiB-block digests during the OTA
// identity scan and re-verifies every range before it is sent, adding
// ~1.1 KB of JS source allowance.
// Shot stats gain Tare time and Scale: the tare chart marker, two table
// columns, CSV columns, and last-shot wiring add ~1.1 KB of JS source
// allowance.
// Cloud call rendering uses 0.5 KB beyond the previous JS allowance.
// Serialized requests and deferred hydration share one record-page loader.
// Allow 1 KiB of source wiring; compressed assets and firmware limits stay fixed.
// Scale profiling adds ~3.4 KB of JS source allowance (state-matrix
// rendering plus the streamed TXT download); compressed assets and firmware
// limits stay fixed.
// Backflush state/history labels and live safety diagnostics add 1.5 KiB source.
// Preferred-scale draft/readback joins Machine Save/Revert (+300 measured bytes).
// Floating header geometry adds 256 bytes of source allowance; gzip limits stay fixed.
// The header theme mode button adds ~0.5 KB of JS source allowance: the cycle
// handler, glyph mode state, and localized label wiring. Compressed asset and
// firmware limits stay fixed.
// The hero curve gains the prototype's solid area and baseline (+250 bytes).
// Home field deltas, initial/reconnect recovery and socket liveness add 6 KiB
// of authoring allowance. Firmware image and OTA limits stay fixed.
// The view-scoped diagnostic WebSocket (frame merge validation, subscribe
// lifecycle, and the shared States/Machine I/O/Scale live renderer) adds
// ~1.5 KB of JS source allowance; compressed asset and firmware limits stay
// fixed.
// The no-scale local clock adds 1.5 KB of source allowance; compressed
// per-asset, combined flash, firmware image and OTA limits remain fixed.
// The no-scale finish transition and early paddle edge add 2.1 KB of source allowance.
// The full-screen shot card state machine (enter/exit/back-consume, session
// keep-alive during a live shot, and the router popstate hook) adds ~3.3 KB of
// JS source allowance; compressed per-asset and firmware limits stay fixed.
// The view-scoped history WebSocket (op subscription and replay, one-shot
// append fetches, strict frame validation, and the navigation visibility move
// onto Home deltas) adds ~2.6 KB of JS source allowance while deleting the
// REST pull plumbing; compressed asset and firmware limits stay fixed.
// The Stats stream (continuation assembly, export window, row validation)
// adds ~4 KB more; compressed asset and firmware limits stay fixed.
// The record-view UI freshness on Home deltas and the single-flight export
// add ~0.6 KB more; compressed asset and firmware limits stay fixed.
// The backpressure-paced owned socket (module keepalive/resync helpers, the
// visibilitychange resync, and the raised frame guard) adds ~0.5 KB more;
// compressed asset and firmware limits stay fixed.
// The Diagnostic full-WebSocket migration (log stream subscription and
// validation, the snapshot-readiness wait, and the local clock/age ticker)
// adds ~0.7 KB of JS source allowance while deleting the REST status and
// log pull plumbing; compressed asset and firmware limits stay fixed.
// Web UI JS sources are authored in formatted, human-readable style since the
// 2026-10 reformat; this allowance was re-measured once for that re-baselining
// (332.1 KB of localized JS) with fixed headroom, and generated assets stayed
// byte-identical modulo the cache-buster tag. New features raise it again per
// the entries below.
// The Home quick target-weight sheet adds the pointer-drag controller with
// velocity-adaptive acceleration, the tick-scale painter, grab dismiss,
// keyboard stepping, and the hydrate→goal→save commit path in home.js plus
// the hero pencil gate in runtime.js: ~6.3 KB of JS source allowance;
// compressed asset and firmware limits stay fixed.
// The /setup onboarding view adds its own module (scan picker with signal
// and lock glyphs, live-apply save, bounded connect poll with resume and
// pre-connect grace, success/error cards) plus the shell route wiring in
// app.js: ~8 KB of JS source allowance; compressed asset and firmware
// limits stay fixed.
// The connecting-state countdown painter adds ~700 bytes of JS source
// allowance, the first-scan retry gating ~160 more, and the chain
// heartbeat/pageshow self-heal with the pre-connect address line ~1.6 KB
// more; compressed asset and firmware limits stay fixed.
// Edited-field error attribution for the brew settings (the declarative
// cross-field rule table with per-side phrasing, the baseline-diff
// edited-field detector, and the multi-error renderer with its banner
// fallback) replaces the first-failure validator: ~3.5 KB of JS source
// allowance; compressed asset and firmware limits stay fixed.
// The /setup skip exit and grace Done button contribute the probe-URL
// helper plus the skip/finish action flows: ~1.5 KB of JS source allowance;
// compressed asset and firmware limits stay fixed.
// Live brew validation warnings add the warning engine (validator twin with
// the required-result filter, signature-skip renderer, scoped red clear,
// symmetric aria) plus the settings listener wiring and conflict-chip
// rendering plus the shared aria-rebuild helper: ~4.2 KB of JS source
// allowance (pre-authorized raise, 2026-10-09); compressed asset and
// firmware limits stay fixed.
// Micra boiler readiness adds the Home lamp rollup with locally ticking
// warm-up estimates from the absolute readyAt timestamps, plus the
// diagnostic readiness/boiler/water renderers sharing the cached stream
// snapshot: ~4.5 KB of JS source allowance (pre-authorized raise, 2026-10-09).
// History CSV export (streamed one-shot fetch windows over the standing
// subscription, plus the shared download/CSV helper split out of the Stats
// export) and the multi-select activation-type filter (visible-records
// projection, progressive page fill, and the fixed-position popover with
// live filtering and quick reset) add ~8 KB of JS source allowance
// (pre-authorized raise, 2026-10-09); compressed asset and firmware limits
// stay fixed.
// The Diagnostic press-and-hold section copy (pointer hold/cancel wiring and
// the shared copy helper in diagnostic.js) adds ~0.6 KB of JS source
// allowance; compressed asset and firmware budgets stay fixed.
// The quick-weight dial guard range adds the editable-range bounds from the
// active preset's guards, the limit pills, and the per-gram labeled zone
// ruler to home.js: ~2.1 KB of JS source allowance; compressed asset and
// firmware budgets stay fixed.
// The Diagnostics audit viewer (records table, hunt-session toggle, explicit
// clear) adds ~3.1 KB of JS source allowance; compressed budgets stay fixed.
// Keeping the quick-weight grab bar's pointer capture from swallowing taps on
// its interactive children (the Done button) adds a guard to the shared drag
// helper: ~0.3 KB of JS source allowance; compressed budgets stay fixed.
// Decigram target weights add the 0.1-step Settings validation branch and the
// reworked quick-weight ruler (statically positioned major/medium/minor ticks
// moved by one transform, decigram value model) to runtime.js and home.js:
// ~1.6 KB of JS source allowance; compressed budgets stay fixed.
// The audit per-row full-detail viewer and the full-log Export download add
// the toggle renderer plus the apiBinary download flow: +1.8 KB measured
// (387214). Compressed budgets re-measured in the generated-asset checks.
// The audit table pagination and change-gated repaint (page state, pager
// renderer, selection-preserving fingerprint) add +1.4 KB measured (388629);
// compressed budgets re-measured below.
// The smooth-cut UI (integer cut-line model shared by the review graph, Home
// preset rows and Quick Settings subtitles; responsive review graph with
// scrubber; relative-margin validation; preset action bar dialogs; verdict
// pills, grind-hint and scale-loss cards) adds +19964 measured JS bytes
// (408593), owner-authorized 2026-10-14; compressed budgets re-measured below.
// Shot statistics (per-verdict stacked histogram with the preset filter,
// grind-hint wording outside the window, median-based "Not now") add +1234
// measured JS bytes (409827); the segmented preset filter and the hidden
// empty chart bring it to 410360; the audit follow-ups (Home dial reset
// notice, scale-back and weight-unknown hero copy, window lines in the review
// graph) to 411004.
// The Home "Stop the shot" card that replaces Quick Settings (By weight |
// Manual control, Protections summary with off-confirmation, no-scale and
// warm-up notice, target ratio, clean-form guard for Home writes) adds
// +3987 measured JS bytes (414991), owner-authorized 2026-10-14; compressed
// budgets re-measured below.
// Release inertia on the target ruler (friction glide that stops at the
// preset limits and yields to any other input) adds +1592 measured JS bytes
// (416596), owner-authorized 2026-10-14.
// The Stats shot card's own preset graph with its "More charts" toggle, and the
// shared preset graph that Settings now draws through it, add +2360 measured JS
// bytes (419060), owner-authorized 2026-10-14.
// The verdict-zone fill on the Stats preset chart adds 178 bytes (419278);
// owner-authorized 2026-10-14.
// The Home preset-card graphs (compact, one per preset) add 596 bytes (419874);
// owner-authorized 2026-10-14.
// The Stats shot-curve cursor (tap, drag, keys, readout) adds 2563 bytes (422437);
// owner-authorized 2026-10-14.
// The compact Stats legend and cursor styles add 885 bytes (423322); owner-authorized 2026-10-14.
// Home preset graphs become full copies of the Settings graph (same axis, scrubber, readout) and each graph keeps its own scrub state: +656 bytes (423978), owner-authorized 2026-10-15.
// The preset graph cut pill shown while the scrubber is held: +527 bytes (424505), owner-authorized 2026-10-15.
// Preset graph paces start at the usual first drop, with its source note and the restart copy: +1082 bytes (425587), owner-authorized 2026-10-15.
// The Stats shot cursor pill, sharing the cut pill helper: +33 bytes (425620), owner-authorized 2026-10-15.
// Show more also folds the Stats card details, with a 0.25 s expand and fade: +1089 bytes (426776), owner-authorized 2026-10-15.
// The Stats card fold settles fast-to-slow with fading details and charts, and keeps the rating visible: 426981 bytes, owner-authorized 2026-10-15.
// Grind hints v2 Home card (four states, window bar, Relearn/Ignore) replaces the median card: 427586 bytes, owner-authorized 2026-10-16 (grind hint v2 card).
// Top messages open and close their measured space while they fade: +1483 bytes (428893), owner-authorized 2026-10-16.
// Home hero card with the cut-line chart, verdict colours, the fast forecast and its Admin colour choice: 432551 bytes, owner-authorized 2026-10-16.
// The Home hero drip tail, from a cut dot to the settled weight: +468 bytes (433028), owner-authorized 2026-10-16.
// Dragging the navigation pill between sections: +2095 bytes (435123), owner-authorized 2026-10-16.
// The Home cup light coloured by whether a shot can start: +518 bytes (435641), owner-authorized 2026-10-16.
// Held touch readings off the weight line and out of flow, their markers, legend and CSV column: +1631 bytes (437272), owner-authorized 2026-10-16.
// Keeping the screen on in full screen (Wake Lock or an inline keep-awake video) and Esc to leave it: +2411 bytes (439683), owner-authorized 2026-10-16.
// The mercury navigation pill (spring, teardrop SVG shape, settling loop): +1962 bytes (441645), owner-authorized 2026-10-16.
// Loading the opening view during the claim and delaying background record subscribes: +419 bytes (442064), requested with the load-latency plan 2026-10-16.
// Reading the build version from the revalidated shell instead of baking it into the runtime: +26 bytes (442090), frontend audit 2026-10-16.
// Compile-time remote-control and Linea Micra guards: +60 bytes (442150), frontend audit 2026-10-16.
// Moving view-only code into the view modules adds module qualifiers (R.,
// uiState.): +2548 bytes (444698), frontend audit 2026-10-16.
// The shot weight/flow chart cursor (shared-time dots, two-value pill, shared scrub helper): +2475 bytes (447173), owner-requested 2026-10-16.
// Moving the Stats shot charts and list into the Stats module adds its qualifiers and runtime exports: +720 bytes (447893), 2026-10-16.
// Moving the History rows and type filter into the History module adds its qualifiers and runtime exports: +289 bytes (448182), 2026-10-16.
// Moving the Diagnostic live renderer into the Diagnostic module adds its qualifiers and runtime exports: +147 bytes (448329), 2026-10-16.
// The gradual forecast hero tint with the window-mode fast alert restored: 451129 bytes, owner-authorized 2026-10-17.
// The iOS-only unmuted keep-awake track and its tap resume: +516 bytes (451645), owner-requested 2026-10-17.
// SoftAP onboarding P1 (status-driven /setup steps, blocked notice, copyable addresses, claim-less setup session): +2842 bytes (454487), owner-requested 2026-10-18.
// Onboarding brewing steps (mode, starting recipe, summary with Change, brewing draft on the setup actions): +3970 bytes (458457), owner-requested 2026-10-18.
// The shot chart's real-stop dot and its two legend entries: +433 bytes (458890), owner-requested 2026-10-18.
// Keeping the /setup session off the UI stream and out of the compatibility redirect: +35 bytes (458925), owner-reported 2026-10-18.
// The Home Micra cloud hint (dismiss or never show again) and the Settings deep link to its group: +908 bytes (459833), owner-requested 2026-10-18.
// The Linea Micra sign-in, machine choice, auto-select, grouped options and batch save: +1737 bytes (461570), owner-requested 2026-10-18.
// Linea Micra Change machine and its machine-count re-list after restart: +1199 bytes (462769), owner-approved 2026-10-18.
// Restoring the La Marzocco Cloud sign-in steps, legend state and lone-machine auto-select: +831 bytes (463600), owner-requested 2026-10-18.
// Clearing the La Marzocco Cloud linked note when Change machine starts: +47 bytes (463647), owner-requested 2026-10-18.
// The La Marzocco Cloud state pill (account, link, offline, water, heating, ready; colour per state): +1105 bytes (464752), owner-requested 2026-10-18.
// The Home Cup accessory state line: +553 bytes (465305), owner-requested 2026-10-18.
// The La Marzocco Cloud Account label on its own line: +38 bytes (465343), owner-requested 2026-10-18.
// The Diagnostic Micra WebSocket live/parked/closed link label: +174 bytes (465517), owner-requested 2026-10-18.
// The Home Micra machine row Starting up note: +112 bytes (465629), owner-requested 2026-10-18.
// The Home shot card flow (scale-reported or chart-computed, hidden during touch holds, finished average): +757 bytes (466386), owner-requested 2026-10-19.
// The Home shot card flow fading from live to its average when the shot finishes: +647 bytes (467033), owner-requested 2026-10-19.
// The Home shot card flow line and fill with a dashed drip tail: +749 bytes (467782), owner-requested 2026-10-19.
// The Home shot card live flow averaged over its last second: +64 bytes (467846), owner-requested 2026-10-19.
// The Home shot card firmware clock run locally in both scale modes, with its ±0.1 s final swap: +312 bytes (468158), owner-requested 2026-10-19.
// Shot card field deltas merged over the previous card: +69 bytes (468227), owner-requested 2026-10-20.
// The Stats shot card drip gain (settled yield minus the weight at the stop): +356 bytes (468583), owner-requested 2026-10-20.
// The drip compensation redesign (recent-shots chart with set and quick start markers, Set drip and Set learning speed, with its review fixes): +3974 bytes (472557), owner-requested 2026-10-20.
// The Stats shot card cut weight/time and drip delay, plus their four CSV columns: +671 bytes (473228), owner-requested 2026-10-20.
// The Stats consistency columns (newest 10 shots against their preset average, level header, scrub): +4391 bytes (477619), owner-requested 2026-10-20.
// Stats consistency thresholds as a share of each preset's average (pure model, units per data, floor/cap scale, fixed slots, clipped outliers): +1072 bytes (478691), owner-requested 2026-10-20.
// The Stats charts reading the firmware's newest-10 window (compact recent list mapped to chart rows): +362 bytes (479053), owner-requested 2026-10-20.
// Diagnostic Heap min waiting text while the boot heap shaper holds its block: +82 bytes (479135), owner-requested 2026-10-20.
// Diagnostic Heap min showing the minimum before and after the boot heap shaper release: +146 bytes (479281), owner-requested 2026-10-20.
// The Home preset rules peek with the graph folded under a chevron: +450 bytes (479731), owner-requested 2026-10-20.
// Shot curves decoded from the firmware's delta stream (one decoder for Home and Stats, minimal live envelope): +1581 bytes (481312), owner-requested 2026-10-20.
// Home and Diagnostic increments continue the snapshot identity: 481504, owner-requested 2026-10-21.
// Stream closed after 5 minutes hidden and resumed without snapshots: 482161, owner-requested 2026-10-21.
// The ?fw= landing refreshes the cached shell after an OTA: 482547, owner-requested 2026-10-21.
// A reconnect keeps the presets it already holds: 482997, owner-requested 2026-10-21.
// Review fixes for the hidden close, setup sessions and the 4002 reconnect: 483596, owner-requested 2026-10-21.
// Audit fixes for typed-build saves, log frames during a resync and escaped pill names: 483639, measured raise of the 2026-10-21 frontend audit remediation.
// Audit fixes for the History filter toggle and a single Stats CSV export: 483654, measured raise of the 2026-10-21 frontend audit remediation.
// Audit render fixes (rename kept across polls, keyed Diagnostic tables, append-only log, lazy ruler): 484544, measured raise of the 2026-10-21 frontend audit remediation.
// Audit state fixes (background tab load, view import retry, saved-value override reset, full-screen history): 485892, measured raise of the 2026-10-21 frontend audit remediation.
// Audit OTA fixes (shared API transport, bounded chunk timeout with backoff, profile mismatch text, settled commit records, block-wise image hashing): 486631, measured raise of the 2026-10-21 frontend audit remediation.
// Audit T4 refactors (static Admin/Settings/Stats markup in partials, one Settings change listener, shared Admin section helpers): 481871, measured raise of the 2026-10-21 frontend audit remediation.
// Audit hardening (shared download helper, CSV formula guard, timezone backoff, Setup stop flag, escaped attribute text): 482689, measured raise of the 2026-10-21 frontend audit remediation.
// Audit accessibility, i18n and form fixes (localized Diagnostic and network text, keyboard and focus support): 484816, measured raise of the 2026-10-21 frontend audit remediation.
// Review fix RV-01 (OTA commit expectation kept when the device stops answering after flash): measured 484909.
// Review fixes RV-04, RV-07, RV-08, RV-10, RV-12 (no view error on a loaded page, log scroll anchor, pending log
// level, escaped view text, star radio keys): measured 485695.
// Prettier re-formats JS that drifted from its style after the audit merge: measured 490907.
// Diagnostic heap-map download (with the shaper Heap min text removed): measured 491070, owner-requested 2026-10-22.
// Optional Web UI login (session header, sign-in, Access panel, Home notice,
// Home Assistant pairing prompt): measured 504669, owner-requested 2026-10-22.
// The Admin deep link from Home's access note: measured 504952, owner-requested 2026-10-22.
// One password form at a time in Admin → Access: measured 505298, owner-requested 2026-10-22.
// Admin unlock reminder while the factory password is in use: measured 505382, owner-requested 2026-10-22.
// Sign out in the footer line with a confirmation: measured 505653, owner-requested 2026-10-22.
// Signed-in device list with in-row sign out and the browser's device label: measured 510376, owner-requested 2026-10-22.
// Inline device renaming: measured 511495, owner-requested 2026-10-22.
// Settings group rows with a live summary per group: measured 515255, owner-requested 2026-10-22.
// Admin section rows with live summaries, sharing the group-row helpers: measured 516957, owner-requested 2026-10-22.
// Diagnostics section rows (shared row helper, hold-to-copy on the row title): measured 517152; the
// Diagnostic log-stream resume brings it to 517339, owner-requested 2026-10-22.
// Outlined History cup for shots without scale weight: measured 517450, owner-requested 2026-10-22.
// − and + keys inside number fields: measured 518524, owner-requested 2026-10-22.
// Settings group animation fallback for browsers without interpolate-size: measured 519506,
// owner-requested 2026-10-22. Home equipment rows in the fallback and unfolded-only preset
// graphs: measured 519725, owner-requested 2026-10-22.
// Stream traffic economy (gap-only keepalive, hidden pause flag, held Stats/History
// rows by id): measured 520759, owner-requested 2026-10-23.
// Firmware-side Shots and History filters with one shared filter menu, the
// 10/50/100 Stats window, and firmware-computed charts: measured 523207,
// owner-requested 2026-10-23. Rejected-op (4003) recovery that clears the list
// views: measured 523596, owner-requested 2026-10-23. Stream render errors kept off
// the corrupt-frame path: measured 523859, owner-requested 2026-10-23. v1 Web UI
// fixes (stream-confirmed commands, bounded sign-in and scan, keyed scale list,
// modal overlay, Diagnostic action gate): measured 524387, owner-requested 2026-10-23.
// Shot-record Target temp, Dose, and Ratio: three Stats card cells with a
// conditional temp layout plus their CSV columns: measured 524984,
// owner-requested 2026-10-24. Quick Settings autosave on the engine shared with
// Settings (draft, one patch per pause, stale resend): measured 527437,
// owner-requested 2026-10-24.
// Shot export (v1 JSON, Gaggiuino file, Beanconqueror link, the shared popup
// shell, and the card's actions menu): measured 539923, owner-requested 2026-10-25.
// Admin Scheduled restart section (status text, two-digit wrapping time
// fields, save/revert): +3943 (measured 543866), owner-requested 2026-10-25.
if (jsBytes > 543866) {
  throw new Error(`Web UI JS source exceeds the authoring budget (${jsBytes} > 543866)`);
}
// Sharing the brand wordmark selectors between the header, the loading view,
// and the inactive overlay pays for the added shell markup.
// The loop timing view adds only source allowance; compressed limits stay fixed.
// The per-section revert buttons and their wiring add ~6.7 KB of combined
// source allowance.
// The Linea Micra connect spinner (glyph spans plus the phase-gated busy
// toggle) adds ~0.25 KB of combined source allowance.
// Deferred Admin BLE/Power/Frontend saving adds ~5 KB of combined source
// allowance (bars in HTML, dirty/save/revert logic in JS).
// The Admin Automatic date & time toggle adds ~0.4 KB of combined source
// allowance (checkbox row in HTML, payload/load/dirty hooks in JS).
// The Admin Automatic date & time toggle adds ~0.4 KB of combined source
// allowance (checkbox row in HTML, payload/load/dirty hooks in JS).
// The firmware master switch adds ~6.5 KB of combined source allowance
// (Firmware section in HTML; section wiring and compatibility-mode gating
// in JS).
// Mid-upload OTA image guarding adds ~1.1 KB of combined JS source allowance.
// Shot stats Tare time and Scale add ~1.2 KB of combined source allowance
// (card fields and table headers in HTML; marker, columns, and wiring in JS).
// Cloud diagnostics add 1.3 KB of combined source allowance.
// Paired reads and deferred hydration add 1 KiB of source allowance only.
// Scale profiling adds ~4.4 KB of combined source allowance (diagnostic
// section in HTML; matrix rendering and download in JS).
// Selectable Micra transport and bounded WS diagnostics add 1.5 KB after
// sharing option payloads and diagnostic row creation. Flash limits stay fixed.
// Backflush contributes the 3.5 KiB source allowances described above.
// Include the same 256-byte floating-header source allowance.
// The theme button adds ~1.1 KB of combined source: three glyph states in the
// shell markup plus the JS cycle, state, and label wiring described above.
// The hero solid curve area and baseline add the same 250-byte JS allowance.
// The diagnostic WebSocket (frame validation, view-scoped subscription, and
// the shared live renderer) plus the Stats metric icons add ~1.3 KB of
// combined source allowance; compressed budgets stay fixed.
// Include the same 1.5 KB local-clock source allowance.
// Include the same 2.1 KB finish-transition source allowance.
// Include the same ~3.4 KB full-screen shot card allowance (buttons and hero
// wrapper in HTML; state machine, session keep-alive, and popstate hook in JS).
// Include the same ~2.6 KB history WebSocket allowance described above; the
// deleted REST probe and pull plumbing return part of it.
// Include the same ~4 KB Stats stream allowance described above.
// Include the same ~0.5 KB owned-socket pacing allowance described above.
// Include the same ~0.7 KB Diagnostic full-WebSocket allowance described above.
// The 2026-10 JS reformat re-baselining described above also applies here:
// 417.0 KB measured, headroom fixed at +6 KB.
// The Home quick target-weight sheet contributes the same ~6.3 KB of JS and
// ~1.4 KB of HTML source allowance described above; compressed budgets stay
// fixed.
// The /setup onboarding view contributes the same ~1.1 KB of HTML and ~8 KB
// of JS source allowance described above; compressed budgets stay fixed.
// The connecting-state countdown adds ~0.8 KB more (line + painter), and
// the heartbeat/where-line work ~1.8 KB more.
// The brew settings edited-field error attribution contributes the same
// ~3.5 KB of JS source allowance described above (rule table, multi-error
// renderer, and the hydration-time brew baseline capture); compressed
// budgets stay fixed.
// The /setup skip exit and grace Done button contribute the same ~400 B of
// HTML and ~1.5 KB of JS source allowance described above; the live brew
// warnings add ~3.4 KB of JS and the conflict-chip element (~60 B of HTML),
// pre-authorized 2026-10-09; compressed budgets stay fixed.
// History CSV export and the activation-type filter contribute the same
// ~8 KB of JS source allowance described above plus ~0.6 KB of History HTML
// (filter/export buttons and the funnel symbol), pre-authorized 2026-10-09;
// compressed budgets stay fixed.
// The quick-weight dial guard range contributes the same ~2.2 KB of combined
// source allowance (~2.1 KB of JS plus the two Home range-limit pills)
// described above; compressed budgets stay fixed.
// The Diagnostics audit viewer adds its ~3.7 KB of combined source allowance
// (~3.1 KB of JS plus the audit panel markup) described above.
// The quick-weight Target-tap guard (active-row check keyed off the
// stream-maintained activeId) adds +160 measured JS bytes; compressed budgets
// stay fixed.
// The Diagnostics audit viewer and Export contribute their ~2 KB of combined
// source allowance (~1.8 KB of JS plus the Export button markup).
// The audit table pagination and change-gated repaint add their ~1.7 KB of
// combined source allowance (pager markup plus the fingerprinted renderer).
// The smooth-cut UI adds its ~21 KB of combined source allowance (501708
// measured) described above, plus the shot-statistics JS and audit
// follow-ups (504126 measured).
// The Home "Stop the shot" card adds its ~4 KB of combined source allowance
// (508250 measured) described above.
// The target ruler's release inertia adds its ~1.1 KB (509705 measured).
// The Stats preset graph and its toggle (same +2360 JS bytes as above) bring it
// to 512167; owner-authorized 2026-10-14.
// The verdict-zone fill (+178 bytes) brings it to 512363; owner-authorized 2026-10-14.
// Home preset graphs bring the combined source to 512959; owner-authorized 2026-10-14.
// The Stats cursor brings the combined source to 515522; owner-authorized 2026-10-14.
// Combined source at 516407 with the legend; owner-authorized 2026-10-14.
// Combined source at 517073 with the copied Home graphs; owner-authorized 2026-10-15.
// Combined source with the preset graph cut pill; owner-authorized 2026-10-15.
// Combined source at 518682 with the first-drop pace start; owner-authorized 2026-10-15.
// Combined source at 520468 with the draggable navigation pill; owner-authorized 2026-10-16.
// Combined source at 520986 with the Home cup light states; owner-authorized 2026-10-16.
// Combined source at 522617 with the held touch readings; owner-authorized 2026-10-16.
// Combined source at 525028 with the full-screen keep-awake; owner-authorized 2026-10-16.
// Combined source at 526990 with the mercury navigation pill; owner-authorized 2026-10-16.
// Combined source at 527476 with the runtime preload and opening-view load; requested with the load-latency plan 2026-10-16.
// The shell's build version and versioned manifest link: 527554, frontend audit 2026-10-16.
// Compile-time remote-control and Linea Micra guards: 527614, frontend audit 2026-10-16.
// View-only code moved into the view modules: 530162, frontend audit 2026-10-16.
// The shot weight/flow chart cursor: 532630, owner-requested 2026-10-16.
// The Stats shot charts and list moved into the Stats module: 533350, 2026-10-16.
// The History rows and type filter moved into the History module: 533639, 2026-10-16.
// The Diagnostic live renderer moved into the Diagnostic module: 533786, 2026-10-16.
// The gradual forecast hero tint with the window-mode fast alert restored: 536959, owner-authorized 2026-10-17.
// Combined source at 537475 with the iOS-only unmuted keep-awake; owner-requested 2026-10-17.
// SoftAP onboarding P1 (/setup steps, notices, addresses, claim-less session): 540424, owner-requested 2026-10-18.
// Onboarding brewing steps (mode and recipe choices with dose, yield and Recommended tags, summary, brewing draft): 547580, owner-requested 2026-10-18.
// The shot chart's real-stop dot and its two legend entries: 548013, owner-requested 2026-10-18.
// The /setup session kept off the UI stream and the compatibility redirect: 548048, owner-reported 2026-10-18.
// The Home Micra cloud hint card and its Settings deep link: 549599, owner-requested 2026-10-18.
// The Linea Micra sign-in, machine choice, grouped options and batch save section: 552540, owner-requested 2026-10-18.
// Linea Micra Change machine and its machine-count re-list after restart: 553863, owner-approved 2026-10-18.
// Restoring the La Marzocco Cloud sign-in steps pane and legend state: 554916, owner-requested 2026-10-18.
// The La Marzocco Cloud linked note as a cleared status confirmation: 554935, owner-requested 2026-10-18.
// The La Marzocco Cloud state pill and its machine and cloud states: 556038, owner-requested 2026-10-18.
// The Home Cup accessory state line: 556675, owner-requested 2026-10-18.
// The La Marzocco Cloud Account label on its own line: 556797, owner-requested 2026-10-18.
// The Diagnostic Micra WebSocket live/parked/closed link label: 556971, owner-requested 2026-10-18.
// Home Equipment device icons with status badges and headline values: 557451, owner-requested 2026-10-18.
// The Micra machine row dash with Turn on to brew / Refill the tank notes: 557554, owner-requested 2026-10-18.
// The Home Micra machine row Starting up note: 557667, owner-requested 2026-10-18.
// The Home shot card flow (scale-reported or chart-computed, hidden during touch holds, finished average): 558473, owner-requested 2026-10-19.
// The Home shot card flow fading from live to its average when the shot finishes: 559120, owner-requested 2026-10-19.
// The Home shot card flow line and fill with a dashed drip tail: 559869, owner-requested 2026-10-19.
// The Home shot card firmware clock run locally in both scale modes, with its ±0.1 s final swap: 560168, owner-requested 2026-10-19.
// Shot card field deltas merged over the previous card: 560237, owner-requested 2026-10-20.
// The Stats shot card drip gain (settled yield minus the weight at the stop): 560606, owner-requested 2026-10-20.
// The drip compensation redesign (recent-shots chart, Set drip and Set learning speed, with its review fixes): 564703, owner-requested 2026-10-20.
// The Stats shot card cut weight/time and drip delay, plus their four CSV columns: 565408, owner-requested 2026-10-20.
// The Stats consistency columns (newest 10 shots against their preset average, level header, scrub): 569857, owner-requested 2026-10-20.
// Stats consistency thresholds as a share of each preset's average: 570929, owner-requested 2026-10-20.
// The Stats charts reading the firmware's newest-10 window: 571291, owner-requested 2026-10-20.
// Diagnostic Heap min waiting text while the boot heap shaper holds its block: 571359, owner-requested 2026-10-20.
// Diagnostic Heap min before and after the boot heap shaper release: 571519, owner-requested 2026-10-20.
// The Home preset rules peek with the graph folded under a chevron: 571969, owner-requested 2026-10-20.
// Shot curves decoded from the firmware's delta stream: 573550, owner-requested 2026-10-20.
// Home and Diagnostic increments continue the snapshot identity: 573742, owner-requested 2026-10-21.
// Stream closed after 5 minutes hidden and resumed without snapshots: 574399, owner-requested 2026-10-21.
// The ?fw= landing refreshes the cached shell after an OTA: 574785, owner-requested 2026-10-21.
// A reconnect keeps the presets it already holds: 575235, owner-requested 2026-10-21.
// Review fixes for the hidden close, setup sessions and the 4002 reconnect: 575834, owner-requested 2026-10-21.
// Audit render fixes (rename kept across polls, keyed Diagnostic tables, append-only log, lazy ruler): 576149, measured raise of the 2026-10-21 frontend audit remediation.
// Audit state fixes (background tab load, view import retry, saved-value override reset, full-screen history): 577497, measured raise of the 2026-10-21 frontend audit remediation.
// Audit OTA fixes (shared API transport, bounded chunk timeout with backoff, profile mismatch text, settled commit records, block-wise image hashing): 578236, measured raise of the 2026-10-21 frontend audit remediation.
// Audit T4 refactors (static Admin/Settings/Stats markup in partials, one Settings change listener, shared Admin section helpers): 575645, measured raise of the 2026-10-21 frontend audit remediation.
// Audit hardening (shared download helper, CSV formula guard, timezone backoff, Setup stop flag, escaped attribute text): 576463, measured raise of the 2026-10-21 frontend audit remediation.
// Audit accessibility, i18n and form fixes (localized Diagnostic and network text, keyboard and focus support): 579700, measured raise of the 2026-10-21 frontend audit remediation.
// The lint gate's markup and lint fixes: 579931, measured raise of the 2026-10-21 frontend audit remediation.
// Review fix RV-01 (OTA commit expectation kept when the device stops answering after flash): measured 580026.
// Review fixes RV-03 to RV-12: measured 580823.
// Compact HTML measurement plus the Prettier re-format of drifted JS: measured 585620.
// Diagnostic heap-map download (with the shaper Heap min text removed): measured 585880, owner-requested 2026-10-22.
// Optional Web UI login: measured 604168, owner-requested 2026-10-22.
// The Admin deep link from Home's access note: measured 604451, owner-requested 2026-10-22.
// One password form at a time in Admin → Access: measured 604832, owner-requested 2026-10-22.
// Admin unlock reminder while the factory password is in use: measured 605078, owner-requested 2026-10-22.
// Sign out in the footer line: measured 605328, owner-requested 2026-10-22.
// Signed-in device list: measured 609902, owner-requested 2026-10-22.
// Inline device renaming: measured 611021, owner-requested 2026-10-22.
// Settings group rows with icons and live summaries: measured 617439, owner-requested 2026-10-22.
// Admin section rows with icons and live summaries: measured 621182, owner-requested 2026-10-22.
// Diagnostics section rows with icons: measured 624462; the Diagnostic log-stream resume brings it
// to 624649, owner-requested 2026-10-22.
// − and + keys inside number fields: measured 625771, owner-requested 2026-10-22.
// Settings group animation fallback: measured 626753, owner-requested 2026-10-22.
// Equipment rows in the fallback and unfolded-only preset graphs: measured 626972, owner-requested 2026-10-22.
// Firmware-side list filters, the Stats window, and firmware-computed charts: measured 629623,
// owner-requested 2026-10-23. Rejected-op (4003) recovery: measured 630012, owner-requested 2026-10-23.
// Stream render errors kept off the corrupt-frame path: measured 630275, owner-requested 2026-10-23.
// v1 Web UI fixes: measured 630821, owner-requested 2026-10-23.
// Shot-record Target temp, Dose, and Ratio cells plus their table headers:
// measured 631575, owner-requested 2026-10-24. Quick Settings autosave: measured
// 634028, owner-requested 2026-10-24.
// Shot export menus and formats: measured 646557, owner-requested 2026-10-25.
// Admin Scheduled restart section: +5656 (measured 652170), owner-requested 2026-10-25.
if (htmlBytes + jsBytes > 652170) {
  throw new Error(`Web UI HTML+JS source exceeds the combined authoring budget (${htmlBytes + jsBytes} > 652170)`);
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
