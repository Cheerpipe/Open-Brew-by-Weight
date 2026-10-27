// Record streams are session-standing: cold views must not subscribe before
// their markup can accept a page, warm entries paint from the cached model
// without any op, and background frames keep that model fresh on every view.
{
  const assert = require('assert').strict, vm = require('vm');
  const source = runtimeStateDecl + 'let homeFrame=null;' + rawRuntimeJs.slice(rawRuntimeJs.search(/let\s+shotWs\s*=/),
      rawRuntimeJs.indexOf('function setHomeSub(')) + recordsStreamJs + statsExportJs;
  function fixture(view) {
    let mount;
    const sockets = [], timers = [], applied = [];
    class Socket {
      static OPEN = 1;
      constructor() { this.readyState = 0; this.sent = []; sockets.push(this); }
      send(value) { const m = JSON.parse(value); this.sent.push(m, ...(m.ops || [])); }
      open() { this.readyState = 1; this.onopen(); }
      close() { this.readyState = 3; this.onclose?.({code: 1006}); }
    }
    const context = vm.createContext({
      WebSocket: Socket, location: {protocol: 'http:', host: 'device.local'},
      document: {hidden: false, addEventListener() {}}, window: {addEventListener() {}},
      setTimeout: fn => { timers.push(fn); return timers.length; }, clearTimeout() {},
      requestAnimationFrame: () => 1, performance: {now: () => 0},
      webUiPollingActive: () => true, webUiClientId: 'test', webUiPowerSeconds: () => 30,
      activeView: view, viewReady: new Promise(resolve => { mount = resolve; }),
      invalidateHomeStream() {}, noteReachFail() {}, statusPageOk: () => true, compatMode: false,
      homeResolve() {}, homeReady: Promise.resolve(),
      shotStatsViewActive: () => context.activeView === 'stats',
      historyViewActive: () => context.activeView === 'history',
      shotsLoaded: false, historyLoaded: false, historyExportWindow: null,
      shotFilterReset: () => applied.push('reset-stats'), historyFilterReset: () => applied.push('reset-history'),
      applyCommonStatus() {},
      applyShotPage: () => { context.shotsLoaded = true; applied.push('stats'); },
      scheduleShotPaint() {},
      applyHistoryPage: () => { context.historyLoaded = true; applied.push('history'); },
      renderShots: () => applied.push('render-stats'), renderHistory: () => applied.push('render-history'),
      paint: (fn) => fn(),
      updateFirmwareFooter() {}, noteReachOk() {},
      shotSort: 'date', shotSortDir: 'desc', historyDir: 'desc',
      SHOTS_PAGE_SIZE: 10, SHOTS_EXPORT_LIMIT: 1000, HISTORY_PAGE_SIZE: 20,
    });
    vm.runInContext(source, context);
    vm.runInContext('paintUiStream=()=>{};scheduleShotPaint=()=>{};', context);
    context.startUiStream();
    return {context, sockets, timers, applied, mount,
      start: force => context[view === 'stats' ? 'startStatsStream' : 'startHistoryStream'](force),
      startStats: force => context.startStatsStream(force),
      startHistory: force => context.startHistoryStream(force),
      ops: socket => socket.sent.filter(message => message.op === view && message.on)};
  }
  const statsRow = id => ({id, bootId: 9, endedAtMs: 30000 + id, hasWallTime: false,
    endedAtLocalSec: 0, endedAtUnixSec: 0, durationS: 28, rating: 0,
    wCg: [100, 200], wAtMs: [1000, 2000], wBreakBefore: [], wHeld: []});
  const statsFrame = extras => Object.assign({v: 1, type: 'stats', boot: 9, snapshot: true,
    epoch: 5, seq: 1, request: 1, ui: {}, total: 2, offset: 0, limit: 10, hasMore: false,
    rows: [statsRow(2), statsRow(1)], rowBase: 0, more: false}, extras);
  const historyFrame = extras => Object.assign({v: 1, type: 'history', boot: 9, snapshot: true,
    epoch: 5, request: 1, ui: {}, total: 1, offset: 0, limit: 20, hasMore: false,
    records: [{id: 2, type: 'shot', durationS: 28, hasWallTime: false,
      endedAtUnixSec: 0, endedAtLocalSec: 0}]}, extras);
  const deliver = (socket, view, extras) => socket.onmessage(
      {data: JSON.stringify((view === 'stats' ? statsFrame : historyFrame)(extras))});
  (async () => {
    // Cold entry waits for markup and an open socket, subscribes exactly once,
    // settles on its completed page, and reconnect replays the subscription.
    for (const view of ['stats', 'history']) {
      for (const socketFirst of [true, false]) {
        const f = fixture(view), pending = f.start(), socket = f.sockets[0];
        if (socketFirst) {
          socket.open();
          assert.equal(f.ops(socket).length, 0, view + ' must wait for markup');
        }
        f.mount();
        await Promise.resolve();
        if (!socketFirst) {
          assert.equal(f.ops(socket).length, 0, view + ' must wait for an open socket');
          socket.open();
        }
        assert.equal(f.ops(socket).length, 1, view + ' must subscribe exactly once');
        assert.equal(f.ops(socket)[0].request, 1);
        deliver(socket, view, {});
        assert.equal(await pending, true, 'the completed page settles the view');
        socket.close();
        f.context.startUiStream();
        f.sockets[1].open();
        assert.equal(f.ops(f.sockets[1]).length, 1, 'the standing view resubscribes on reconnect');
        assert.equal(f.ops(f.sockets[1])[0].request, 2);
      }
    }
    // Warm entry: a standing, synced subscription paints from the cached model
    // and re-renders it without sending any op (AC1).
    for (const view of ['stats', 'history']) {
      const f = fixture(view), cold = f.start(), socket = f.sockets[0];
      socket.open(); f.mount(); await Promise.resolve();
      deliver(socket, view, {});
      assert.equal(await cold, true);
      const renders = f.applied.filter(entry => entry.startsWith('render')).length;
      assert.equal(await f.start(), true, view + ': warm entry resolves from cache');
      assert.equal(f.ops(socket).length, 1, view + ': warm entry sends no op');
      assert.equal(f.applied.filter(entry => entry.startsWith('render')).length, renders + 1,
          view + ': warm entry re-renders the cached model');
    }
    // Background frames keep the model fresh while another view is active
    // without rendering into the hidden view (AC2).
    for (const view of ['stats', 'history']) {
      const f = fixture(view), cold = f.start(), socket = f.sockets[0];
      socket.open(); f.mount(); await Promise.resolve();
      deliver(socket, view, {});
      await cold;
      f.context.activeView = 'home';
      f.applied.length = 0;
      deliver(socket, view, view === 'stats' ? {snapshot: false, epoch: 6, seq: 2}
          : {snapshot: false, epoch: 6});
      assert.deepEqual(f.applied, [view], view + ': background frames update only the model');
    }
    // A record page subscribes on its first visit, not from the background
    // ensure, and a rejected op (close 4003) clears its filters (AC3).
    {
      const f = fixture('stats'), cold = f.start(), socket = f.sockets[0];
      socket.open(); f.mount(); await Promise.resolve();
      deliver(socket, 'stats', {});
      await cold;
      f.context.ensureBackgroundStreams();
      assert.equal(socket.sent.filter(message => message.op === 'history' && message.on).length, 0,
          'the background ensure never subscribes an unvisited record page');
      assert.equal(socket.sent.filter(message => message.op === 'stats' && message.on).length, 1,
          'the already-standing page is not re-subscribed');
      f.context.activeView = 'home';
      f.context.ensureBackgroundStreams();
      assert.equal(socket.sent.filter(message => message.op === 'history' && message.on).length, 0,
          'a later ensure still leaves the unvisited page alone');
      f.applied.length = 0;
      vm.runInContext('uiState.statsWindow = 50; eachStream("rejected")', f.context);
      assert.deepEqual(f.applied, ['reset-stats', 'reset-history'], 'a rejected op resets every list filter');
      assert.equal(vm.runInContext('uiState.statsWindow', f.context), 10, 'and the Stats window');
    }
    // With both pages standing, navigating between record views sends no new
    // subscribe ops at all (AC1).
    {
      const f = fixture('home'), socket = f.sockets[0];
      socket.open();
      vm.runInContext('ensureStatsStream();ensureHistoryStream()', f.context);
      deliver(socket, 'stats', {});
      deliver(socket, 'history', {});
      const ops = socket.sent.length;
      f.mount();
      f.context.activeView = 'stats';
      assert.equal(await f.startStats(), true);
      f.context.activeView = 'history';
      assert.equal(await f.startHistory(), true);
      f.context.activeView = 'stats';
      assert.equal(await f.startStats(), true);
      assert.equal(socket.sent.length, ops, 'navigating between record views sends no ops');
    }
    // A dropped socket or a pending resync defers warm entry to the backend
    // snapshot instead of trusting the cache (reconnect caveat).
    {
      const f = fixture('stats'), socket = f.sockets[0];
      socket.open();
      vm.runInContext('ensureStatsStream()', f.context);
      deliver(socket, 'stats', {});
      socket.close();
      f.context.activeView = 'stats';
      f.mount();
      const pending = f.startStats();
      await Promise.resolve();
      assert.equal(socket.sent.filter(message => message.op === 'stats' && message.on).length, 1,
          'warm entry over a dropped socket sends no op; the replay will resubscribe');
      f.context.startUiStream();
      const next = f.sockets[1];
      next.open();
      assert.equal(next.sent.filter(message => message.op === 'stats' && message.on).length, 1,
          'the reconnect replays the standing subscription');
      deliver(next, 'stats', {request: 2, snapshot: true});
      assert.equal(await pending, true, 'the replayed snapshot settles the deferred entry');
      // The production resync path must clear the sync marker; while the
      // resync snapshot is in flight the entry waits instead of painting the
      // stale cache. statsSynced is forced back up so the waiting can only be
      // explained by the pending resync itself.
      f.context.requestShotResync();
      assert(next.sent.some(message => message.op === 'resync'), 'the resync op was sent');
      assert.equal(vm.runInContext('statsSynced', f.context), false,
          'requestShotResync drops the sync marker');
      vm.runInContext('statsSynced=true', f.context);
      let settled = false;
      const resyncEntry = f.startStats().then(value => { settled = true; return value; });
      const renders = f.applied.filter(entry => entry === 'render-stats').length;
      await Promise.resolve();
      await Promise.resolve();
      assert.equal(settled, false, 'a pending resync defers the entry past the cache gate');
      assert.equal(f.applied.filter(entry => entry === 'render-stats').length, renders,
          'the deferred entry renders nothing while it waits');
      assert.equal(next.sent.filter(message => message.op === 'stats' && message.on).length, 1,
          'a pending resync needs no extra op; its snapshot is already in flight');
      vm.runInContext('shotResync=false', f.context);
      deliver(next, 'stats', {request: 2, seq: 2});
      assert.equal(await resyncEntry, true, 'the resync snapshot settles the deferred entry');
    }
    // A socket drop during a cold load settles the pending entry false, and a
    // superseding entry settles the older promise false and takes the page.
    for (const view of ['stats', 'history']) {
      const f = fixture(view), pending = f.start(), socket = f.sockets[0];
      socket.open(); f.mount(); await Promise.resolve();
      socket.close();
      assert.equal(await pending, false, view + ': a dropped socket settles the pending cold entry');
    }
    for (const view of ['stats', 'history']) {
      const f = fixture(view), socket = f.sockets[0];
      socket.open(); f.mount(); await Promise.resolve();
      const first = f.start();
      await Promise.resolve();
      const second = f.start();
      await Promise.resolve();
      assert.equal(f.ops(socket).length, 1, view + ': a superseding entry sends no extra op');
      assert.equal(await first, false, view + ': the superseded entry settles false');
      deliver(socket, view, {});
      assert.equal(await second, true, view + ': the replacement entry gets the page');
    }
    // Deliver a real split Stats page through the production socket callback.
    const f = fixture('stats'), pending = f.start(), socket = f.sockets[0];
    socket.open(); f.mount(); await Promise.resolve();
    const frame = {v: 1, type: 'stats', boot: 9, snapshot: true, epoch: 5, seq: 1, request: 1,
      ui: {}, bootId: 9, total: 2, offset: 0, limit: 10, hasMore: false,
      rows: [statsRow(2)], rowBase: 0, more: true};
    socket.onmessage({data: JSON.stringify(frame)});
    assert.equal(vm.runInContext('statsPage.rows.length', f.context), 1);
    // Export can be requested while the standing page is still arriving.
    const exported = f.context.statsFrameWindow(0, 1000, 'date', 'desc', 180000);
    assert.equal(vm.runInContext('statsPage.rows.length', f.context), 1);
    socket.onmessage({data: JSON.stringify({...frame, snapshot: false,
      rows: [statsRow(1)], rowBase: 1, more: false})});
    assert.equal(await pending, true);
    const request = socket.sent.find(message => message.fetch).request;
    socket.onmessage({data: JSON.stringify({...frame, snapshot: false, seq: 2,
      request, limit: 1000})});
    socket.onmessage({data: JSON.stringify({...frame, snapshot: false, seq: 2,
      request, limit: 1000, rows: [statsRow(1)], rowBase: 1, more: false})});
    assert.deepEqual(Array.from(await exported, entry => entry.id), [2, 1]);
    assert(!socket.sent.some(message => message.op === 'resync'));
  })().catch(error => { console.error(error); process.exitCode = 1; });
}
