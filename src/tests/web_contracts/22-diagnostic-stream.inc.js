// The Diagnostic page subscribes to a live section projection on the owned
// socket only while the view is shown; every navigation unsubscribes.
{
  const assert = require('assert').strict, vm = require('vm');
  const source = runtimeStateDecl + 'let homeFrame=null;' + runtimeJs.slice(runtimeJs.indexOf('function statusStreamFrame('), runtimeJs.indexOf('function renderHomeStream(')) + runtimeJs.slice(runtimeJs.search(/let\s+shotWs\s*=/),
    runtimeJs.indexOf('function setHomeSub('));
  const sockets = [], timers = new Map(); let timer = 0, owner = true;
  const applied = [], frames = [];
  class Socket {
    static OPEN = 1;
    constructor(url) { this.url = url; this.readyState = 1; this.sent = []; sockets.push(this); }
    send(value) { this.sent.push(JSON.parse(value)); }
    close() { this.readyState = 3; this.onclose?.({}); }
  }
  const listeners = {};
  const context = {
    WebSocket: Socket, location: {protocol: 'http:', host: 'device.local'},
    document: {hidden: false, addEventListener() {}}, window: {addEventListener() {}},
    setTimeout: (fn, delay) => { timers.set(++timer, {fn, delay}); return timer; },
    clearTimeout: id => timers.delete(id), requestAnimationFrame: fn => { fn(); return 1; },
    webUiPollingActive: () => owner, webUiClientId: '0123456789abcdef', webUiPowerSeconds: () => 30,
    activeView: 'diagnostic', viewSeq: 1, viewReady: Promise.resolve(), lastStatusAt: 0,
    performance: {now: () => 1234}, invalidateHomeStream() {}, noteReachFail() {}, homeResolve() {},
    homeStale: false,
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
  assert(codeIncludes(runtimeJs, 'if(!apply||!diagnosticReady())return;apply(s);') &&
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
      source.indexOf('export function init(')).replace('export ', '').replace('if (!s) return;', 'return;') +
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
  const homeStream = fs.readFileSync(path.join(sketchDir, 'network/OpenBrewByWeightHomeStream.inc'), 'utf8');
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
  assert(appJsSource.indexOf('R.startLogStream()') >= 0 &&
         !codeIncludes(appJsSource, 'R.loadLog()') && !codeIncludes(appJsSource, 'logTimer=setInterval'),
      'the log view subscribes the stream instead of REST polling');
  assert(partialHtml.diagnostic.includes('id="dScaleTimer"'));
  assert(partialHtml.diagnostic.includes('id="dScaleWeight"'));
  assert(JSON.parse(fs.readFileSync(
      path.join(sketchDir, 'web', 'locales', 'en.json'), 'utf8'))
      .strings['diagnostic.weight'] === 'Weight');
}
// The Diagnostic log appends new lines in place: a selection or a scrolled-back
// reader keeps their place, and only a reader at the end follows new lines.
{
  const assert = require('assert').strict;
  const ta = {v: '', sets: 0, scrollTop: 0, clientHeight: 20,
    get scrollHeight() { return (this.v.split('\n').length - 1) * 20; },
    get value() { return this.v; }, set value(v) { this.v = v; this.sets++; },
    setRangeText(t, a, b) { this.v = this.v.slice(0, a) + t + this.v.slice(b); }};
  const ui = {logEvents: []}, nodes = {log: ta, logFilter: {value: ''}, logLevelFilter: {value: '4'}};
  const renderLog = new Function('$', 'uiState', 'formatWallTimeLocal',
      `${blockAt(runtimeJs, 'function renderLog(')};return renderLog;`)((id) => nodes[id], ui, String);
  const ev = (n) => ({sequence: n, atMs: n * 1000, wallSec: 0, level: 'info', category: 'web', message: 'm' + n});
  ui.logEvents = [ev(1), ev(2)];
  renderLog();
  assert.equal(ta.value, '+1.000s INFO [web] m1\n+2.000s INFO [web] m2\n');
  ta.scrollTop = 0;
  ui.logEvents.push(ev(3));
  renderLog();
  assert.equal(ta.sets, 1, 'new lines must append without replacing the text');
  assert.equal(ta.scrollTop, 0, 'a scrolled-back reader keeps their place');
  ta.scrollTop = 20;
  ui.logEvents.shift();
  ui.logEvents.push(ev(4));
  renderLog();
  assert.equal(ta.value, '+2.000s INFO [web] m2\n+3.000s INFO [web] m3\n+4.000s INFO [web] m4\n');
  assert.equal(ta.sets, 1, 'dropping the oldest line must not replace the text');
  assert.equal(ta.scrollTop, 0, 'RV-07: dropping the oldest line keeps the reader on the same lines');
  ta.scrollTop = 40;
  ui.logEvents.push(ev(5));
  renderLog();
  assert.equal(ta.scrollTop, 80, 'a reader at the end follows new lines');
}
