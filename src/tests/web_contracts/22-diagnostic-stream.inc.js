// The Diagnostic page subscribes to a live section projection on the owned
// socket only while the view is shown; every navigation unsubscribes.
{
  const assert = require('assert').strict, vm = require('vm');
  const source = runtimeStateDecl + 'let homeFrame=null;' + runtimeJs.slice(runtimeJs.indexOf('function statusStreamFrame('), runtimeJs.indexOf('function renderHomeStream(')) + runtimeJs.slice(runtimeJs.search(/let\s+shotWs\s*=/),
    runtimeJs.indexOf('function setHomeSub('));
  const sockets = [], timers = new Map(); let timer = 0, owner = true;
  const applied = [];
  class Socket {
    static OPEN = 1;
    constructor(url) { this.url = url; this.readyState = 1; this.sent = []; sockets.push(this); }
    send(value) { const m = JSON.parse(value); this.sent.push(m, ...(m.ops || [])); }
    close() { this.readyState = 3; this.onclose?.({}); }
  }
  const context = {
    WebSocket: Socket, location: {protocol: 'http:', host: 'device.local'},
    document: {hidden: false, addEventListener() {}}, window: {addEventListener() {}},
    setTimeout: (fn, delay) => { timers.set(++timer, {fn, delay}); return timer; },
    clearTimeout: id => timers.delete(id), requestAnimationFrame: fn => { fn(); return 1; },
    webUiPollingActive: () => owner, webUiClientId: '0123456789abcdef', webUiPowerSeconds: () => 30,
    activeView: 'diagnostic', viewSeq: 1, viewReady: Promise.resolve(), lastStatusAt: 0,
    performance: {now: () => 1234}, invalidateHomeStream() {}, noteReachFail() {}, homeResolve() {},
    homeStale: false, $: () => null, shotFsActive: false,
    viewStatusHandlers: {diagnostic: () => {}, diagnosticLive: status => applied.push(status)},
  };
  vm.runInNewContext(source, context);
  // Diagnostic owns only the paths Home does not carry; the page reads the
  // machine, cup, safety state and scale weight/timer from the Home model.
  const changes = {
    'relayClosed': false, 'controlSource': 'none', 'reedOn': false,
    'backflush.hardLimitMs': 0, 'safety.fault': 'NONE', 'safety.taskWatchdogReady': true,
    'scale.controlState': 'ACTIVE', 'scale.rssi': -52,
    'scale.weightUpdateIntervalMs': 100, 'scale.maxPacketGapMs': 240,
    'scale.lastDisconnect.summary': '',
  };
  const snapshot = {v: 1, type: 'diagnostic', boot: 7, snapshot: true, changes: {...changes}};
  const snapshotFrame = context.statusStreamFrame(null, snapshot);
  assert.equal(snapshotFrame.status.scale.maxPacketGapMs, 240);
  const patch = context.statusStreamFrame(snapshotFrame,
      {v: 1, type: 'diagnostic', boot: 7, snapshot: false,
       changes: {'scale.maxPacketGapMs': 260, 'relayClosed': true}});
  assert.equal(patch.status.relayClosed, true, 'deltas patch the live cache');
  assert.equal(patch.status.scale.rssi, -52, 'unchanged fields survive a patch');
  for (const bad of [{...patch, boot: 8},
      {...patch, changes: {'scale.maxPacketGapMs': Infinity}},
      {...patch, changes: {'scale.constructor.prototype': {poisoned: true}}},
      {...patch, snapshot: true, changes: {'scale.rssi': -50}}]) {
    assert.throws(() => context.statusStreamFrame(patch, bad),
        'invalid diagnostic frames must fail closed');
  }

  context.startUiStream();
  assert.equal(sockets.length, 1);
  sockets[0].onopen();
  assert.equal(sockets[0].sent[0].op, 'bind');
  assert(!sockets[0].sent.some(m => m.op === 'diagnostic'),
      'the diagnostic subscription starts detached');
  context.startDiagnosticStream();
  assert.deepEqual(sockets[0].sent.at(-1), {op: 'diagnostic', on: true});
  sockets[0].onmessage({data: JSON.stringify(snapshot)});
  assert.equal(applied.at(-1).scale.maxPacketGapMs, 240, 'live frames render the three sections');
  vm.runInContext("activeView='settings'", context);
  const paints = applied.length;
  sockets[0].onmessage({data: JSON.stringify({
    v: 1, type: 'diagnostic', boot: 7, snapshot: false, changes: {'scale.maxPacketGapMs': 300}})});
  assert.equal(applied.length, paints,
      'frames on another page must not render diagnostics');
  vm.runInContext("activeView='diagnostic'", context);
  context.paintDiagnosticStream();
  assert.equal(applied.length, paints + 1);
  assert.equal(applied.at(-1).scale.maxPacketGapMs, 300,
      'returning to the page renders the updated cache');
  sockets[0].onmessage({data: JSON.stringify(
      {v: 1, type: 'diagnostic', boot: 7, snapshot: false, changes: {'__proto__.x': 1}})});
  assert.equal(sockets[0].sent.at(-1).op, 'resync', 'poisoned frames request a resync');
  context.stopDiagnosticStream();
  assert.deepEqual(sockets[0].sent.at(-1), {op: 'diagnostic', on: false});
  sockets[0].close();
  const retry = [...timers.values()].find(t => t.delay >= 400 && t.delay <= 600);
  assert(retry, 'socket loss must schedule a reconnect');
  retry.fn();
  sockets[1].onopen();
  assert(!sockets[1].sent.some(m => m.op === 'diagnostic'),
      'a closed subscription must not resubscribe on reconnect');
  context.startDiagnosticStream();
  assert.deepEqual(sockets[1].sent.at(-1), {op: 'diagnostic', on: true});
  sockets[1].close();
  const again = [...timers.values()].find(t => t.delay >= 700 && t.delay <= 1300);
  again.fn();
  sockets[2].onopen();
  assert.deepEqual(sockets[2].sent.find(m => m.op === 'diagnostic'), {op: 'diagnostic', on: true},
      'an open subscription resubscribes after reconnect');

  // Both regressions turned Diagnostic into a reconnect loop under a
  // permanent claim overlay: the server's audit frame shipped invalid JSON,
  // and a bind from a superseded client dropped the socket like a network
  // failure instead of a takeover notice.
  const uiStreamSrc = fs.readFileSync(path.join(sketchDir, 'network/OpenBrewByWeightUiStream.inc'), 'utf8');
  const closeFrame = uiStreamSrc.slice(uiStreamSrc.indexOf('void sendUiStreamClose'),
    uiStreamSrc.indexOf('#include "OpenBrewByWeightUiStreamTransport.inc"'));
  assert(closeFrame.includes('HTTPD_WS_TYPE_CLOSE') && uiStreamSrc.replace(/\s+/g, ' ').includes(
      'kUiStreamCloseTakeover = 4001, kUiStreamCloseSilent = 4002, kUiStreamCloseRejected = 4003;'),
      'the close frames must carry WS codes 4001 (takeover), 4002 (silent page) and 4003 (rejected op)');
  const rejected = uiStreamSrc.slice(uiStreamSrc.indexOf('if (!accepted)'), uiStreamSrc.indexOf('if (activity)'))
      .replace(/\s+/g, ' ');
  assert(rejected.includes('if (bindRejected || owner) sendUiStreamClose(request->handle, session->fd, ' +
      'bindRejected ? kUiStreamCloseTakeover : kUiStreamCloseRejected);'),
      'a rejected bind must close with 4001 so the page deactivates; a rejected owner op with 4003');
  // A `records` op from an older cached page is accepted and ignored, never
  // closed: mixed versions cannot loop on 4003, and no session asks for a
  // record index any more.
  assert(uiStreamSrc.replace(/\s+/g, ' ').includes('} else if (cJSON_IsString(op) && session->bound && ' +
      'strcmp(op->valuestring, "records") == 0) { // Older pages asked for a record index here: the request is ' +
      '// ignored, never closed, so mixed versions cannot loop on 4003. accepted = true; }') &&
      !uiStreamSrc.includes('sendRecordsIndex') && !uiStreamSrc.includes('recordsKey'),
      'a stray records op must be ignored without a 4003 close or an index answer');
  // The rejection reaches the Web UI log as a warning (" failed" sets the level).
  assert(uiStreamSrc.includes('owner = session->bound && strcmp(session->clientId, self.activeWebUiClientId_) == 0;') &&
      uiStreamSrc.includes('"UI stream op %.12s failed validation; the page resets its list views"'));
  assert(/uiStreamSendKind = "close";\s*sendUiStreamClose\(self\.server_, session\.fd, kUiStreamCloseTakeover\)/
      .test(uiStreamSrc.replace(/\n\s+/g, ' ')),
      'the dispatch path must reuse the same takeover close');
  // The silent close (byte layout: 4002 big-endian) uses its own code.
  assert(/uiStreamSendKind = "close";\s*sendUiStreamClose\(self\.server_, session\.fd, kUiStreamCloseSilent\)/
      .test(uiStreamSrc.replace(/\n\s+/g, ' ')));
  const auditSrc = fs.readFileSync(path.join(sketchDir, 'network/OpenBrewByWeightAuditStream.inc'), 'utf8');
  assert(auditSrc.includes('statusJsonAppend(&used, "]}}}")'),
      'the audit frame must close items, audit, changes and the frame root');
  // The audit table's Detail column is composed firmware-side from the stored
  // whole-gram arguments plus the emit-time excerpt, so even records already
  // on flash render with meaningful text; the UI adds a per-row viewer and a
  // full-log export backed by the text download endpoint.
  const diagnosticSrc = fs.readFileSync(path.join(sketchDir, 'web/js/diagnostic.js'), 'utf8');
  assert(auditSrc.includes('formatAuditRecordDetail'),
      'the audit frame must serialize the composed record detail');
  assert(diagnosticSrc.includes('function toggleAuditDetail') &&
         diagnosticSrc.includes('auditDetailRow'),
      'audit rows must expand to their full detail');
  assert(partialHtml.diagnostic.includes('id="auditExportButton"') &&
         codeIncludes(diagnosticSrc, "R.apiBinary(\"/api/v1/diagnostic/audit\")"),
      'the audit export must download the full text log');
  const routes = fs.readFileSync(
      path.join(sketchDir, 'network/OpenBrewByWeightHttpAuthAssets.inc'), 'utf8');
  assert(routes.includes('{"/api/v1/diagnostic/audit", HTTP_GET, auditLogExportHandler}'),
      'the audit export route must be registered');
  const localeStrings = JSON.parse(fs.readFileSync(
      path.join(sketchDir, 'web', 'locales', 'en.json'), 'utf8')).strings;
  for (const key of ['diagnostic.download_trace', 'diagnostic.audit_export_failed',
                     'diagnostic.audit_no_clock', 'diagnostic.audit_view']) {
    assert(localeStrings[key], 'missing audit locale key ' + key);
  }
}

{
  // The blink regression: value validity is number-vs-null in both
  // transports; the formatters must not require socket-only fields such as
  // scale.available (absent from the REST diagnostic payload). runtimeJs is
  // already localized, so expectations use the baked-in English literals.
  const assert = require('assert').strict;
  const first = runtimeJs.indexOf('function formatScaleWeight(');
  const timerAt = runtimeJs.indexOf('function formatScaleTimer(');
  const last = runtimeJs.indexOf('\n}', timerAt) + 2;
  const formatters = new Function('pad2',
    runtimeJs.slice(first, last) + ';return {formatScaleWeight,formatScaleTimer};')(
    n => String(n).padStart(2, '0'));
  assert.equal(formatters.formatScaleTimer({scale: {timerMs: 65430}}), '1:05.4');
  assert.equal(formatters.formatScaleTimer({scale: {timerMs: null}}), '—');
  assert.equal(formatters.formatScaleTimer({scale: {}}), '—',
      'no REST available field may blank the timer');
  assert.equal(formatters.formatScaleWeight({scale: {observedWeightG: 12.34}}), '12.3 g');
  assert.equal(formatters.formatScaleWeight({scale: {currentWeightG: -0.5}}), '-0.5 g');
  assert.equal(formatters.formatScaleWeight({scale: {observedWeightG: null}}), '—');
  // The stream is the only writer: the view handler rides the socket cache
  // and the REST fallback painter is gone along with the route.
  assert(!codeIncludes(js, "api('/api/v1/status/diagnostic'"),
      'the Diagnostic view must not read REST status');
  assert(!codeIncludes(js, "api('/api/v1/log'"),
      'the Diagnostic view must not poll the REST log');
  assert(codeIncludes(runtimeJs, 'if(apply&&diagnosticReady())apply(s);') &&
      codeIncludes(runtimeJs, 'mergeStatus(homeFrame?.status,diagFrame.status,settingsFrame?.status)') &&
      codeIncludes(runtimeJs, "if(activeView==='diagnostic')scheduleDiagPaint()"),
      'the full view waits for all diagnostic sections, reads Home and Settings paths from their models, and repaints on Home frames');
  assert(codeIncludes(runtimeJs, 'shotWs=null;resetUiStream();'),
      'socket loss must drop the cache');
}

// Log controls hydrate from Diagnostic's config, independently of Home state.
{
  const assert = require('assert').strict;
  const source = fs.readFileSync(path.join(sketchDir, 'web/js/diagnostic.js'), 'utf8');
  const controls = {serialLogLevel: {dataset: {}}, ringRetainLogLevel: {dataset: {}}};
  let settle, sent;
  const R = {withBaseRev: (v) => v,
    command: (url, v) => { sent = v; return {finally: (f) => { settle = f; }}; }};
  // Only the log-level hydration at the top of applyStatus runs here.
  const [apply, save] = new Function('applyDiagnosticStatus', '$', 'R', source.slice(source.indexOf('const LOG_LEVELS'),
      source.indexOf('export function init(')).replace(/export /g, '').replace('if (!s) return;', 'return;') +
      ';return [applyStatus, saveLogLevel];')(
      () => {}, id => controls[id], R);
  apply({config: {serialLogLevel: 'warning', ringRetainLogLevel: 'debug'}});
  assert.equal(controls.serialLogLevel.value, 'warning');
  assert.equal(controls.ringRetainLogLevel.value, 'debug');
  // A paint must not overwrite the level being saved.
  controls.serialLogLevel.value = 'debug';
  save('serialLogLevel');
  assert.deepEqual(sent, {serialLogLevel: 'debug'});
  apply({config: {serialLogLevel: 'warning', ringRetainLogLevel: 'info'}});
  assert.equal(controls.serialLogLevel.value, 'debug');
  assert.equal(controls.ringRetainLogLevel.value, 'info');
  // RV-08: once a failed save settles, the still-focused select shows the device's value.
  settle();
  assert.equal(controls.serialLogLevel.value, 'warning');
  assert.equal(controls.serialLogLevel.dataset.pending, undefined);
  apply({config: {serialLogLevel: 'error', ringRetainLogLevel: 'info'}});
  assert.equal(controls.serialLogLevel.value, 'error');
}

// Firmware projection: bounded field budget, view-scoped dispatch, and wiring.
{
  const assert = require('assert').strict;
  const stream = fs.readFileSync(path.join(sketchDir, 'network/OpenBrewByWeightUiStream.inc'), 'utf8') +
      fs.readFileSync(path.join(sketchDir, 'network/OpenBrewByWeightUiStreamTransport.inc'), 'utf8');
  const homeStream = (fs.readFileSync(path.join(sketchDir, 'network/OpenBrewByWeightHomeStream.inc'), 'utf8') +
      fs.readFileSync(path.join(sketchDir, 'network/OpenBrewByWeightRecordStreams.inc'), 'utf8'));
  assert(stream.includes('strcmp(op->valuestring, "diagnostic")'));
  assert(stream.includes('session->diagnostic = cJSON_IsTrue(on)'));
  assert(stream.includes('if (open && session.diagnostic)') &&
      stream.includes('within() && sendDiagnosticStream(session, control, network)'),
      'the diagnostic delta rides the existing dispatch under the status workspace');
  // Record-stream no-change bookkeeping: the suppression path must adopt a
  // moved epoch, and every standing-window send (snapshots included) must
  // store epoch+fingerprint — otherwise idle ticks rebuild pages forever.
  assert.equal((homeStream.match(/session\.historyEpoch = epoch;/g) || []).length, 2,
      'history suppression and standing sends must both store the epoch');
  assert.equal((homeStream.match(/session\.statsEpoch = epoch;/g) || []).length, 2,
      'stats suppression and standing sends must both store the epoch');
  assert.equal((homeStream.match(/if \(!fetch\) \{/g) || []).length, 2,
      'snapshot sends must store the standing fingerprint');
  // Backpressure triage: a failed frame send must not tear down the owned
  // socket when nothing of the frame reached the wire. Only a partial
  // payload, a dead peer, or lost ownership may close the session.
  assert(stream.includes('uiStreamSendCleanAbort'),
      'the send override must classify clean header aborts');
  assert(homeStream.includes('session.homeResync = true;') &&
         homeStream.includes('deferArm = true;') &&
         (homeStream.match(/sendUiStreamFrame\(session, [a-zA-Z.]+, session\.diagResync\)/g) || []).length === 4,
      'deferred Home/diagnostic sends must re-arm their snapshot');
  assert(homeStream.includes('if (deferred) return true;') &&
         homeStream.includes('    session.statsSent = sentBase;\n    return true;'),
      'deferred record pages keep their retry state instead of closing');
  assert(homeStream.includes('session.statsSent = sentBase;'),
      'a deferred stats frame must roll its buffered rows back');
  // Dispatch order: the client's setup window waits for Home and the shot
  // card, so record pages — which may pace across dispatch ticks — go last.
  const homeCall = stream.indexOf('bool open = sendHomeStream(session, control,');
  const cardCall = stream.indexOf('within() && sendShotCard(session, control,');
  const diagCall = stream.indexOf('within() && sendDiagnosticStream(session, control, network)');
  const historyCall = stream.indexOf('within() && sendHistoryStream(session, control)');
  const statsCall = stream.indexOf('within() && sendStatsStream(session, control)');
  assert(homeCall >= 0 && cardCall > homeCall && diagCall > cardCall &&
         historyCall > diagCall && statsCall > historyCall,
      'dispatch must send home, card, diagnostic, history, stats in order');
  // Bulk continuations leave ACK headroom while live Home can dispatch sooner.
  assert(homeStream.includes('session.statsPageEpoch != epoch') &&
         homeStream.includes('session.statsSent = 0;'),
      'stats pages must capture identity at page start and resume by epoch');
  assert(homeStream.includes('uiStreamUrgent_.store(true, std::memory_order_release);\n    return true;'),
      'a continued page must flag another dispatch');
  assert(stream.includes('kStatsStreamFrameBytes = 8192') &&
         homeStream.includes('millis() - session.statsSentAtMs < kStatsStreamPaceMs') &&
         homeStream.includes('session.statsSentAtMs = millis();'),
      'Stats must cap row batches and pace continuations after delivery');
  const diagRegion = homeStream.slice(homeStream.indexOf('sendDiagnosticStream'));
  const diagSlots = Number(/kDiagFields\s*=\s*(\d+)/.exec(networkHeader)[1]);
  const diagCalls = (diagRegion.match(/\bdelta\.field\(/g) || []).length;
  assert(diagCalls > 0 && diagCalls <= diagSlots,
      `Diagnostic projection ${diagCalls} fields exceeds ${diagSlots} fingerprint slots`);
  for (const field of ['"backflush.remainingMs"', '"relayClosed"', '"controlSource"',
      '"safety.taskWatchdogReady"', '"scale.maxPacketGapMs"',
      '"scale.lastDisconnect.summary"',
      '"scale.lastCommandFailure.summary"', '"scale.connectedMac"']) {
    assert(diagRegion.includes('delta.field(' + field), 'missing diagnostic field ' + field);
  }
  // Home owns the shared live state; Diagnostic reads it from the Home model.
  for (const field of ['"state"', '"machineState"', '"cupPresence.weightG"', '"physicalActivatorOn"',
      '"scale.streamState"', '"scale.timerMs"', '"scale.observedWeightG"', '"lineaMicra.powerState"'])
    assert(!diagRegion.includes('delta.field(' + field), 'Home-owned path repeated by Diagnostic: ' + field);
  assert(homeStream.includes('control.scaleAvailable && control.currentTimerValid') &&
      homeStream.includes('control.currentTimerMs / 100 * 100'),
      'the timer keeps the Home projection 0.1 s quantization');
  assert(diagRegion.includes('static_cast<uint32_t>(millis() - micra.sampleAtMs) / 1000 * 1000'),
      'the Micra sample age must quantize to whole seconds so an idle stream'
      + ' does not emit one frame per dispatch tick');
  assert(diagRegion.includes('session.diagBoot = control.bootId'));
  assert(codeIncludes(viewJs.diagnostic, "t('dScaleTimer',R.formatScaleTimer(s))") &&
         codeIncludes(viewJs.diagnostic, "t('dScaleWeight',R.formatScaleWeight(s))"),
      'the shared live renderer paints the Scale timer and weight');
  assert(codeIncludes(appJsSource, 'R.stopDiagnosticStream()'),
      'leaving the view must unsubscribe');
  assert(codeIncludes(runtimeJs, 'loadDiagnosticStatus'),
      'entering the view loads the stream snapshot');
  assert(codeIncludes(viewJs.diagnostic, 'export const start=startLogStream,stop=stopLogStream') &&
         codeIncludes(appJsSource, 'const{start}=await ensureView(name)') &&
         codeIncludes(appJsSource, 'for(const mod of jsMods.values())mod.stop?.()') &&
         !codeIncludes(appJsSource, 'R.loadLog()') && !codeIncludes(appJsSource, 'logTimer=setInterval'),
      'the log view subscribes the stream through its module instead of REST polling');
  assert(partialHtml.diagnostic.includes('id="dScaleTimer"'));
  assert(partialHtml.diagnostic.includes('id="dScaleWeight"'));
  assert(JSON.parse(fs.readFileSync(
      path.join(sketchDir, 'web', 'locales', 'en.json'), 'utf8'))
      .strings['diagnostic.weight'] === 'Weight');
}
// The Diagnostic log renders one element per line and appends new lines in
// place: a selection or a scrolled-back reader keeps their place, and only a
// reader at the end follows new lines. Copy takes the shown lines' text.
{
  const assert = require('assert').strict;
  const rows = (v) => v.match(/<div[^>]*>.*?<\/div>/g) || [];
  const box = {rows: [], sets: 0, scrollTop: 0, clientHeight: 20,
    get scrollHeight() { return this.rows.length * 20; },
    get firstChild() { return {remove: () => this.rows.shift()}; },
    set innerHTML(v) { this.rows = rows(v); this.sets++; },
    insertAdjacentHTML(at, v) { this.rows[at === 'afterbegin' ? 'unshift' : 'push'](...rows(v)); }};
  const ui = {logEvents: []}, count = {}, state = {shown: []},
    nodes = {log: box, logLines: box, logCount: count, logFilter: {value: ''}, logLevelFilter: {value: '4'}};
  const lineOf = viewJs.diagnostic.slice(viewJs.diagnostic.indexOf('const logLineOf'),
      viewJs.diagnostic.indexOf('];\n', viewJs.diagnostic.indexOf('const logLineOf')) + 2);
  const makeRender = (logHistory, logBootId, zoneOffset = () => 0, diagConfig = null,
                      logJumpAt = null, logAnchor = null, nearLogTop = () => {}, logMarks = []) =>
      new Function('$', 'R', 'logHistory', 'logBootId', 'logZoneOffsetMinutes', 'diagConfig',
      '__WEBUI_TEXT__', 'logJumpAt', 'logAnchor', 'nearLogTop', 'logMarks', 'state',
      `${blockAt(viewJs.diagnostic, 'function logWallText(')};${lineOf};let logShown=state.shown;
      const logPast=()=>logJumpAt!=null;
      ${blockAt(viewJs.diagnostic, 'function renderLog(')};
      return () => { renderLog(); state.shown = logShown; };`)(
      (id) => nodes[id], {uiState: ui, sub: (t, v) => t.replace('{0}', String(v[0])),
                          formatWallTimeLocal: String,
                          escHtml: (t) => t.replace(/&/g, '&amp;').replace(/</g, '&lt;')},
      logHistory, logBootId, zoneOffset, diagConfig, (key) => key, logJumpAt, logAnchor,
      nearLogTop, logMarks, state);
  const text = () => state.shown.join('\n');
  const renderLog = makeRender([], 0);
  const ev = (n, level = 'info') =>
    ({sequence: n, atMs: n * 1000, wallSec: 0, level, category: 'web', message: 'm' + n});
  ui.logEvents = [ev(1), ev(2)];
  renderLog();
  assert.equal(text(), '· Boot 0 ·\n+1.000s INFO [web] m1\n+2.000s INFO [web] m2',
      "the boot's first event follows a restart marker");
  assert.equal(count.textContent, '2 lines');
  box.scrollTop = 0;
  ui.logEvents.push(ev(3));
  renderLog();
  assert.equal(box.sets, 1, 'new lines must append without replacing the log');
  assert.equal(box.rows.length, 4);
  assert.equal(box.scrollTop, 0, 'a scrolled-back reader keeps their place');
  box.scrollTop = 40;
  ui.logEvents.shift();
  ui.logEvents.push(ev(4));
  renderLog();
  assert.equal(text(), '+2.000s INFO [web] m2\n+3.000s INFO [web] m3\n+4.000s INFO [web] m4');
  assert.equal(box.sets, 1, 'dropping the oldest line must not replace the log');
  assert.equal(box.scrollTop, 0, 'RV-07: dropping the oldest line keeps the reader on the same lines');
  box.scrollTop = 40;
  ui.logEvents.push(ev(5));
  renderLog();
  assert.equal(box.scrollTop, 80, 'a reader at the end follows new lines');
  // D5: the severity is a tag the view colours, and a message cannot inject
  // markup.
  ui.logEvents = [{...ev(6, 'error'), message: '<b>x</b>'}];
  renderLog();
  assert.equal(box.rows.at(-1), '<div>+6.000s <b class="lve">ERROR</b> [web] &lt;b>x&lt;/b></div>');
  // A mark sits after the newest event when it was made, in this boot only,
  // and Copy takes its text; it does not count as a log line.
  ui.logEvents = [ev(5), ev(6)];
  makeRender([], 7, () => 0, null, null, null, () => {},
      [{after: 5, boot: 7, text: 'Mark · t'}, {after: 6, boot: 3, text: 'old boot'}])();
  assert.equal(text(), '+5.000s INFO [web] m5\n· Mark · t ·\n+6.000s INFO [web] m6');
  assert.equal(box.rows[1], '<div class="logMark">Mark · t</div>');
  assert.equal(count.textContent, '2 lines');
  // The companion's stored history renders above the ring, deduplicated against
  // it, with its markers as lines where the companion placed them: the
  // restart into this boot, a gap, and the same boot's overlap with the live
  // events shown once.
  ui.logEvents = [ev(5), ev(6)];
  makeRender([
    {b: 6, s: 9, ms: 900, w: 0, l: 'i', c: 'web', m: 'old9'},
    {g: 'boot', boot: 7},
    {b: 7, s: 1, ms: 100, w: 0, l: 'i', c: 'web', m: 'kept1'},
    {g: 'gap', count: 2},
    {b: 7, s: 4, ms: 400, w: 0, l: 'i', c: 'web', m: 'kept4'},
    {b: 7, s: 5, ms: 500, w: 0, l: 'i', c: 'web', m: 'overlap5'},
  ], 7)();
  assert.equal(text(),
      '+0.900s INFO [web] old9\n· Boot 7 ·\n+0.100s INFO [web] kept1\n'
      + '· 2 events could not be copied while the companion was away ·\n'
      + '+0.400s INFO [web] kept4\n+5.000s INFO [web] m5\n+6.000s INFO [web] m6',
      'history pages render with markers, and the live copy of an event wins');
  assert.equal(box.rows[1], '<div class="logMk logBoot">Boot 7</div>',
      'a restart is a boot row the view pins while its lines scroll');
  ui.logEvents = [ev(1), ev(2)];
  makeRender([{g: 'boot', boot: 7}], 7)();
  assert.equal(text(), '· Boot 7 ·\n+1.000s INFO [web] m1\n+2.000s INFO [web] m2',
      'a restart the history already marks is not marked twice');
  // A new day on the controller's clock starts with a divider, never at the
  // top of the view.
  ui.logEvents = [];
  makeRender([{b: 7, s: 1, ms: 1, w: 1000000000, l: 'i', c: 'web', m: 'a'},
              {b: 7, s: 2, ms: 2, w: 1000000001, l: 'i', c: 'web', m: 'b'}], 7)();
  assert.equal(text(), '1000000000 INFO [web] a\n· New day · 1000000001 ·\n1000000001 INFO [web] b');
  ui.logEvents = [ev(5), ev(6)];
  // Stored wall-clock seconds are UTC: the view shifts each stored event by
  // the controller zone's offset at that instant before formatting, so the
  // history reads in the same local time as the live ring.
  const shifted = [];
  makeRender([{b: 7, s: 8, ms: 800, w: 1793380832, l: 'i', c: 'web', m: 'utc8'}], 7,
      (zone, w) => (shifted.push([zone, w]), -180), {timezoneId: 'America/Santiago'})();
  assert.deepEqual(shifted, [['America/Santiago', 1793380832]],
      'a stored event asks for its zone offset at its own instant');
  assert.equal(text(), '1793370032 INFO [web] utc8\n+5.000s INFO [web] m5\n+6.000s INFO [web] m6',
      'history renders at wallSec shifted by the zone offset');
  // D16: an older page prepends with the lines in view held in place, every
  // render asks whether the reader is near the top, and a jump hides the
  // live ring so the moment shows alone, at the end of the view.
  let asked = 0;
  makeRender([{b: 7, s: 4, ms: 400, w: 0, l: 'i', c: 'web', m: 'kept4'}], 7)();
  box.scrollTop = 30;
  const before = box.scrollHeight, sets = box.sets;
  makeRender([{b: 7, s: 2, ms: 200, w: 0, l: 'i', c: 'web', m: 'kept2'},
              {b: 7, s: 4, ms: 400, w: 0, l: 'i', c: 'web', m: 'kept4'}], 7, () => 0, null, null,
             {h: before, top: 30}, () => asked++)();
  assert.equal(box.sets, sets, 'an older page prepends without replacing the log');
  assert.equal(box.scrollTop, 30 + box.scrollHeight - before, 'a prepended page keeps the view in place');
  assert.equal(asked, 1, 'each render checks whether older history should load');
  makeRender([{b: 7, s: 2, ms: 200, w: 0, l: 'i', c: 'web', m: 'kept2'}], 7, () => 0, null, 1793380832,
             'end', () => {}, [{after: 5, boot: 7, text: 'Mark · t'}])();
  assert.equal(text(), '+0.200s INFO [web] kept2', 'a jump shows the stored moment without the live ring or marks');
  assert.equal(box.scrollTop, box.scrollHeight, 'a jump opens at the moment, at the end of the view');
  assert(codeIncludes(viewJs.diagnostic, '$("copyLogButton").onclick=()=>copyText(logShown.join("\\n"))'),
      'Copy takes the lines the view shows');
}
// The stored history goes out with this screen's Web UI session (the companion
// answers only a browser the controller lets open the Web UI), for paging and
// for Export, which shows only when the page came through the companion.
{
  const assert = require('assert').strict;
  assert(codeIncludes(runtimeJs, 'async function companionLog(') &&
         codeIncludes(runtimeJs, 'headers:webUiHeaders(body&&{"Content-Type":"application/json"})'),
      'history requests carry the Web UI session');
  // codeIncludes strips the R. qualifier, and runtime.js has no R to qualify with.
  assert(!/\bR\.\w/.test(runtimeJs), 'the runtime module never reads its own exports through R');
  assert(codeIncludes(viewJs.diagnostic, 'if(!logPast()&&e.b===logBootId&&live.has(e.s))continue'),
      'a jump hides the live ring, so the history keeps the events both hold');
  assert(codeIncludes(viewJs.diagnostic, '?(e)=>e.g==="boot"&&e.boot===boot:day?(e)=>e.w&&e.w<dayStart:shown') &&
         codeIncludes(viewJs.diagnostic, 'logAnchor=mark?{mark}:jumped?"end"'),
      'a boot or day step pages until its row and opens on it');
  assert(codeIncludes(viewJs.diagnostic, 'if(run!==logHistoryRun)return') &&
         codeIncludes(viewJs.diagnostic, 'logHistoryRun++'),
      'a restart abandons the pages still on their way');
  assert(codeIncludes(viewJs.diagnostic, 'await(await companionLog(logHistoryQuery())).json()') &&
         codeIncludes(viewJs.diagnostic, 'companionLog("/__companion/log/export"+(range?"?"+range:""))'),
      'paging and Export both read the companion through that request');
  assert(codeIncludes(viewJs.diagnostic, '$("logExportButton").hidden=!here') &&
         codeIncludes(viewJs.diagnostic, '$("logNav").hidden=!here'),
      'Export and the time steps show only through a paired companion');
  // The strip names what the view shows: live, up to a moment (Back to live,
  // Mark hidden), or nothing at that moment, with the cap or the companion
  // note; a live chip shows its filter and a × away from the default.
  {
    const el = (extra = {}) => ({hidden: false, textContent: '', className: '', ...extra});
    const steps = [el({value: 'day:-1'}), el({value: 'day:1'}), el({value: 'boot:-1'}), el({value: 'boot:1'})];
    const n = {logStrip: el(), logStripTitle: el(), logStripNote: el(), logLiveButton: el(),
      logExportButton: el(), logMarkButton: el(), logNav: el({querySelectorAll: () => steps})};
    const controls = (logJumpAt, logHistory, logHistoryDone, here, c = null, logJumpBoot = null, logJumpDay = null) =>
      new Function('$', 'R', '__WEBUI_TEXT__', 'logHistoryAvailable', 'logHistoryFull', 'logJumpAt',
          'logHistory', 'logHistoryDone', 'companionDiagnosticStatus', 'logWallText', 'logJumpBoot',
          'logBoots', 'loadLogBoots', 'logStepTarget', 'logJumpDay',
          `const logPast=()=>logJumpAt!=null||logJumpBoot!=null;
          ${blockAt(viewJs.diagnostic, 'function logHistoryControls(')};logHistoryControls();`)(
          (id) => n[id], {sub: (t, v) => t.replace('{0}', v[0])}, (k) => k, () => here, () => false,
          logJumpAt, logHistory, logHistoryDone, c, String, logJumpBoot, [], () => {},
          (kind, dir) => (kind === 'boot' && dir < 0 ? undefined : null), logJumpDay);
    controls(null, [], false, true);
    assert.deepEqual([n.logStrip.className, n.logStripTitle.textContent, n.logStripNote.textContent,
      n.logNav.hidden, n.logLiveButton.hidden, n.logMarkButton.hidden],
      ['logStrip', 'Live · newest at the bottom', 'Scroll up for older events.', false, true, false]);
    assert.deepEqual(steps.map((b) => b.disabled), [false, true, true, true],
        'live: only a step back, and only where there is one');
    controls(1793380832, [{s: 1}], false, true);
    assert.deepEqual([n.logStrip.className, n.logStripTitle.textContent,
      n.logLiveButton.hidden, n.logMarkButton.hidden],
      ['logStrip past', 'Showing up to 1793380832', false, true]);
    assert.deepEqual(steps.map((b) => b.disabled), [false, false, true, false]);
    controls(null, [{s: 1}], false, true, null, {boot: 40});
    assert.equal(n.logStripTitle.textContent, 'Showing boot 40');
    controls(1793380832, [{s: 1}], false, true, null, null, '2026-11-01');
    assert.equal(n.logStripTitle.textContent, 'Showing 2026-11-01');
    controls(1793380832, [{g: 'boot'}], true, true);
    assert.deepEqual([n.logStrip.className, n.logStripTitle.textContent],
      ['logStrip warn', 'Nothing logged at or before 1793380832']);
    controls(null, [], false, false, {paired: true, log: true});
    assert.equal(n.logNav.hidden && n.logExportButton.hidden, true);
    assert.match(n.logStripNote.textContent, /companion keeps the older history/);
  }
  // Day steps name the day before or after the view's day (from inside a day,
  // its own first), paging from its end; boot steps the boot before
  // or after, from inside a boot its own end first, and stepping into the
  // live boot goes back to live.
  {
    const T = (iso) => Date.parse(iso) / 1000;
    const boots = [
      {boot: 38, from: T('2026-10-10T08:00Z'), to: T('2026-10-12T09:00Z'), end: {boot: 1}},
      {boot: 39, from: T('2026-10-12T09:05Z'), to: T('2026-10-25T18:00Z'), end: {boot: 2}},
      {boot: 41, from: T('2026-10-25T18:05Z'), to: T('2026-10-30T07:00Z'), end: {boot: 3}},
    ];
    const step = (logJumpAt, logJumpBoot, kind, dir) => new Function('diagConfig',
        'logZoneOffsetMinutes', 'logBoots', 'logBootId', 'logJumpAt', 'logJumpBoot',
        `const logPast=()=>logJumpAt!=null||logJumpBoot!=null;` +
        ['function logLocalIso(', 'function logWallSec(', 'function logBootIndex(', 'function logStepTarget(']
          .map((m) => blockAt(viewJs.diagnostic, m)).join(';') + ';return logStepTarget;')(
        null, () => 0, boots, 41, logJumpAt, logJumpBoot)(kind, dir);
    assert.deepEqual(step(null, null, 'boot', -1), {at: boots[1].to, boot: boots[1]}, 'live: back to the boot before');
    assert.equal(step(null, boots[1], 'boot', 1), null, 'into the live boot: back to live');
    assert.deepEqual(step(null, boots[1], 'boot', -1), {at: boots[0].to, boot: boots[0]});
    assert.equal(step(null, boots[0], 'boot', -1), undefined, 'no boot before the oldest');
    assert.deepEqual(step(T('2026-10-20T10:00Z'), null, 'boot', 1), {at: boots[1].to, boot: boots[1]},
        'inside a boot, forward reaches its own end first');
    assert.deepEqual(step(T('2026-10-20T10:00Z'), null, 'day', -1), {at: T('2026-10-19T23:59:59Z'), day: '2026-10-19'});
    assert.deepEqual(step(T('2026-10-20T10:00Z'), null, 'day', 1), {at: T('2026-10-20T23:59:59Z'), day: '2026-10-20'});
    assert.deepEqual(step(T('2026-10-20T23:59:59Z'), null, 'day', 1), {at: T('2026-10-21T23:59:59Z'), day: '2026-10-21'});
    assert.equal(step(T('2026-10-10T12:00Z'), null, 'day', -1), undefined, 'no day before the history');
  }
  // A jump in the future is refused inline; a custom export range that ends
  // before it starts disables Download, and an empty end means until now.
  {
    const f = {logJumpDate: {value: '2999-01-01'}, logJumpTime: {value: ''}, logJumpError: {}, logJumpGo: {},
      logFromDate: {value: '2026-11-03'}, logFromTime: {value: '10:00'}, logToDate: {value: '2026-11-03'},
      logToTime: {value: '09:00'}, logCustom: {}, logRangeError: {}, logExportGo: {}};
    let pick = 'custom';
    f.logExportDialog = {id: 'logExportDialog', querySelectorAll: () => [],
      querySelector: () => ({value: pick})};
    f.logJumpDialog = {id: 'logJumpDialog', querySelectorAll: () => []};
    const run = new Function('$', 'diagConfig', 'logZoneOffsetMinutes', 'logViewRange',
        ['function logWallSec(', 'function logFieldsSec(', 'function checkLogDialog(', 'function logExportRange(']
          .map((m) => blockAt(viewJs.diagnostic, m)).join(';') +
        ';return {checkLogDialog, logExportRange};')((id) => f[id], null, () => 0, [5, null]);
    assert.equal(run.checkLogDialog(f.logJumpDialog), Date.parse('2999-01-01T23:59:59Z') / 1000,
        'an empty time reads as the end of that day');
    assert.equal(f.logJumpError.hidden, false);
    assert.equal(f.logJumpGo.disabled, true, 'a future moment cannot be shown');
    run.checkLogDialog(f.logExportDialog);
    assert.equal(f.logRangeError.hidden, false);
    assert.equal(f.logExportGo.disabled, true, 'an end before the start blocks Download');
    f.logToTime.value = '';
    run.checkLogDialog(f.logExportDialog);
    assert.equal(f.logExportGo.disabled, false);
    assert.deepEqual(run.logExportRange(), [Date.parse('2026-11-03T10:00Z') / 1000,
        Date.parse('2026-11-03T23:59:59Z') / 1000]);
    pick = 'view';
    assert.deepEqual(run.logExportRange(), [5, null], 'the default range is what the view shows');
    pick = 'all';
    assert.deepEqual(run.logExportRange(), [null, null]);
  }
  assert(partialHtml.diagnostic.includes('id="logExportButton"') &&
         !partialHtml.diagnostic.includes('id="logOlderButton"'),
      'older history loads near the top: Show older is gone');
  // D16: pages carry the level and subsystem filters and the jump, and
  // the cap points to Export.
  const query = (c, page, next, at, boot = null) => new Function('$', 'LOG_HISTORY_PAGE',
      'logHistoryNext', 'logJumpAt', 'logJumpBoot',
      `${blockAt(viewJs.diagnostic, 'function logHistoryQuery(')};return logHistoryQuery;`)(c, page, next, at, boot);
  const controls = {logLevelFilter: {value: '2'}, logFilter: {value: 'ble'}};
  assert.equal(query((id) => controls[id], 500, null, 1793380832)(),
      '/__companion/log?limit=500&level=cew&category=ble&at=1793380832');
  assert.equal(query((id) => controls[id], 500, {boot: 3, before: 9}, 1793380832)(),
      '/__companion/log?limit=500&level=cew&category=ble&boot=3&before=9', 'a cursor continues the jump');
  assert.equal(query((id) => controls[id], 500, null, 1793380832, {end: {boot: 4, before: 4294967295}})(),
      '/__companion/log?limit=500&level=cew&category=ble&boot=4&before=4294967295',
      "a boot step starts at the end cursor the companion's boot index gives");
  assert(codeIncludes(viewJs.diagnostic, 'LOG_HISTORY_PAGE=2000,LOG_HISTORY_CAP=40000') &&
         codeIncludes(viewJs.diagnostic, 'logHistory.length>=LOG_HISTORY_CAP'),
      'the view stops loading past the cap');
  // D14: Clear log history sits with the other record clears in Admin, shows
  // only through the companion, asks first, posts through the shared Web UI
  // session helper, and has Diagnostic start its view over on its next show.
  assert(partialHtml.admin.includes('id="clearLogHistoryButton" class="btnGlyph mutable btnInvert" type="button" hidden') &&
         !partialHtml.diagnostic.includes('logDeleteButton') &&
         codeIncludes(viewJs.admin, '$("clearLogHistoryButton").hidden=!here?.paired') &&
         codeIncludes(viewJs.admin, 'await R.companionLog("/__companion/log/delete",{});R.uiState.logHistoryCleared=true') &&
         codeIncludes(runtimeJs, 'headers:webUiHeaders(body&&{"Content-Type":"application/json"})') &&
         codeIncludes(viewJs.diagnostic, 'if(R.uiState.logHistoryCleared){R.uiState.logHistoryCleared=false;jumpLog(null)}'),
      'Clear log history lives in Admin and follows the companion');
}
// logZoneOffsetMinutes answers the zone's offset at each instant: January and
// July of a DST zone differ, and a missing zone or one Intl rejects falls
// back to the last clock sync's applied offset.
{
  const assert = require('assert').strict;
  const offset = new Function('R', 'logZoneFormatter', 'logZoneFormatterId',
      `${blockAt(viewJs.diagnostic, 'function logZoneOffsetMinutes(')};return logZoneOffsetMinutes;`)(
      {statusTimezoneOffsetMinutes: 777}, null, '');
  assert.equal(offset('America/New_York', Date.UTC(2026, 0, 15, 12) / 1000), -300);
  assert.equal(offset('America/New_York', Date.UTC(2026, 6, 15, 12) / 1000), -240);
  assert.equal(offset('', 0), 777);
  assert.equal(offset('Not/A_Zone', 0), 777);
}
