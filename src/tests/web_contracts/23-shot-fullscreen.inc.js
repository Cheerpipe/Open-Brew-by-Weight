// The live shot view and the shot-aware session keep-alive are the
// two behavioral contracts added with the mobile presentation mode. Both are
// regression-prone state machines, so they get pure vm-slice coverage here:
// the keep-alive must read fresh stream state (a frozen liveness copy once
// kept dead sessions alive), and the back-consume protocol must swallow
// exactly one history pop per exit.
{
  const assert = require('assert').strict;
  const vm = require('vm');

  // Session keep-alive: resetWebUiInactivity / webUiPollingActive /
  // sessionShotLive over controllable time, timers, and stream state.
  {
    let now = 0;
    let deactivated = 0;
    let pendingTimer = null;
    const api = new Function('Date', 'setTimeout', 'clearTimeout', 'deactivateWebUi',
        'let webUiOwner=true,setupSession=false,webUiActiveUntil=0,webUiInactivityTimer=0' +
        ',WEB_UI_INACTIVITY_MS=1000,shotFrame=null,shotStale=true,webUiPwaClass=()=>false' +
        ',homeFrame=null,homeStale=true,statusLiveShot=false;' +
        runtimeJs.slice(runtimeJs.indexOf('function sessionShotLive('),
            runtimeJs.indexOf('function noteWebUiInteraction(')) +
        ';return{reset:resetWebUiInactivity,polling:webUiPollingActive' +
        ',setFrame:v=>shotFrame=v,setStale:v=>shotStale=v' +
        ',setHome:(stale,status)=>{homeStale=stale;homeFrame={status}}' +
        ',setOwner:v=>webUiOwner=v,until:()=>webUiActiveUntil}')(
        {now: () => now},
        fn => { pendingTimer = fn; return 1; },
        () => {}, () => deactivated++);

    // A live ui-stream card re-arms the expired session instead of killing it.
    api.setStale(false);
    api.setFrame({card: {valid: true, live: true}});
    now = 5000;
    assert.equal(api.polling(), true, 'a live shot must keep the session');
    assert.equal(deactivated, 0);
    assert.ok(api.until() > now, 'the deadline must be re-armed');

    // A fresh home stream reporting an active cycle keeps it on any view.
    api.setFrame(null);
    api.setHome(false, {liveShot: true, cycle: {active: true}});
    now = 9000;
    assert.equal(api.polling(), true, 'a live cycle on the home stream must keep the session');

    // Stale streams must let the session die (frozen liveness regression).
    api.setHome(true, {liveShot: true});
    api.reset();
    now += 1001;
    pendingTimer();
    pendingTimer = null;
    assert.equal(deactivated, 1, 'stale live data must let the session expire');
    assert.equal(api.polling(), false);

    // Losing ownership stops the re-arm chain entirely.
    api.setOwner(false);
    api.setFrame({card: {valid: true, live: true}});
    api.setHome(false, {machineRunning: true});
    assert.equal(api.polling(), false, 'a dead session must not revive for a shot');
  }

  // Back-consume protocol: enter pushes one history entry; every exit path
  // consumes exactly one pop, and unrelated pops reach the router.
  {
    const viewClasses = new Set(), bodyClasses = new Set();
    const history = {pushed: 0, back: 0};
    const stubClasses = set => ({
      add: (...c) => c.forEach(x => set.add(x)),
      remove: (...c) => c.forEach(x => set.delete(x)),
      contains: c => set.has(c),
      toggle() {},
    });
    const handlers = {};
    const element = () => ({onclick: null, dataset: {}, focus() {},
      addEventListener: (type, fn) => { handlers[type] = fn; }, querySelector: () => null});
    const appended = [];
    const hero = element();
    const view = Object.assign(element(), {hidden: true, classList: stubClasses(viewClasses),
      append: el => appended.push(el), removeEventListener() {}});
    const open = () => !view.hidden;
    const docHandlers = {}, nav = {}, video = {plays: 0, pauses: 0, attrs: {}, paused: false,
      play() { this.plays++; this.paused = false; return Promise.resolve(); },
      pause() { this.pauses++; this.paused = true; },
      setAttribute(k, v) { this.attrs[k] = v; }};
    const els = {shotHero: hero, liveShot: view, liveShotClose: element(),
      liveShotOpen: element(), liveShotGraph: {}};
    const card = {closest: () => null}, link = {closest: () => ({})};
    let clock = 1000;
    const tap = (target = card, gap = 200) => handlers.pointerup({target, timeStamp: clock += gap});
    const doubleTap = () => { tap(card, 1000); tap(); };
    const api = new Function('$', 'history', 'location', 'document', 'setTimeout', 'clearTimeout',
        'navigator', 'paintUiStream', 'renderLiveShot', 'observePresetHost',
        runtimeJs.slice(runtimeJs.search(/let\s+shotFsActive\s*=/),
            runtimeJs.indexOf('// The diagnostic stream rides')) +
        ';return{enter:enterShotFullScreen,exit:exitShotFullScreen' +
        ',pop:exitFullScreenOnPop,init:initShotFullScreen,awake:keepShotFsAwake}')(
        id => els[id],
        {pushState(state) { history.pushed++; this.state = state; },
          back() { history.back++; this.state = null; }},
        {href: '/'},
        {body: {classList: stubClasses(bodyClasses)},
          addEventListener: (type, fn) => { docHandlers[type] = fn; },
          createElement: tag => { assert.equal(tag, 'video'); return video; }},
        () => 0, () => {}, nav, () => {}, () => {}, () => {});
    const key = (k, target) => docHandlers.keydown({key: k, target});
    const plain = {closest: () => null}, sheet = {closest: () => ({})};

    api.init();
    assert.equal(api.pop(), false, 'a pop without full screen reaches the router');

    tap(card, 1000); tap(card, 400);
    assert.ok(!open(), 'two slow taps must not open full screen');
    tap(link, 1000); tap(link);
    assert.ok(!open(), 'taps on links and buttons keep their own action');
    doubleTap();
    assert.ok(open() && bodyClasses.has('shotFs'), 'a double tap on the card opens the live view');
    assert.equal(history.pushed, 1, 'entering pushes one history entry');
    assert.equal(api.pop(), true, 'the back gesture exits full screen');
    assert.ok(!open() && !bodyClasses.has('shotFs'));

    // The X button exits via history.back(); that pop must be swallowed once.
    key('Enter', hero);
    assert.ok(open(), 'Enter on the focused card opens full screen');
    els.liveShotClose.onclick();
    assert.ok(!open(), 'the X button exits full screen');
    assert.equal(history.back, 1, 'the X exit consumes its pushed entry');
    assert.equal(api.pop(), true, 'the X-driven pop is consumed');
    assert.equal(api.pop(), false, 'the next pop reaches the router');
    // A double tap inside full screen exits like the X.
    doubleTap();
    doubleTap();
    assert.ok(!open() && history.back === 2, 'a second double tap leaves full screen');
    assert.equal(api.pop(), true, 'the double-tap exit consumes its pushed entry');
    // Leaving full screen without Back (the card turned invalid) drops its entry
    // too, so the next Back is not a dead press.
    doubleTap();
    api.exit();
    assert.ok(!open() && history.back === 3, 'a programmatic exit consumes its entry');
    assert.equal(api.pop(), true, 'its pop is swallowed once');
    history.back = 2;

    // Escape leaves full screen from anywhere, consuming its history entry,
    // but an open sheet or dialog takes it first.
    key('Escape', plain);
    assert.equal(history.back, 2, 'Escape outside full screen does nothing');
    key('Enter', plain);
    assert.ok(!open(), 'Enter opens full screen only from the focused card');
    doubleTap();
    key('Escape', sheet);
    assert.ok(open(), 'Escape inside a sheet or dialog keeps full screen');
    key('Escape', plain);
    assert.ok(!open() && history.back === 3, 'Escape with focus on the page exits');
    assert.equal(api.pop(), true, 'the Escape exit consumes its pushed entry');

    // Plain HTTP has no Wake Lock API: a looping video keeps the screen on
    // outside iOS (muted), invisible and untouchable, and pauses on exit.
    const plays = video.plays;
    doubleTap();
    assert.equal(appended.length, 1, 'the keep-awake video is added once');
    assert.ok(video.className === 'heroAwake' && video.muted && video.loop && video.playsInline &&
        video.attrs['aria-hidden'] === 'true' && /^data:video\/mp4;base64,/.test(video.src));
    assert.equal(video.plays, plays + 1, 'entering plays the keep-awake video');
    els.liveShotClose.onclick();
    assert.ok(video.pauses >= 1, 'leaving pauses the keep-awake video');
    api.pop();
    // iOS resumes unmuted playback only from a gesture: a tap while full
    // screen must restart the video after iOS paused it in the background.
    doubleTap();
    video.paused = true;
    const resumed = video.plays;
    tap(card, 700);
    assert.equal(video.plays, resumed + 1, 'a tap in full screen resumes the paused video');
    els.liveShotClose.onclick();
    api.pop();

    // The header pill opens the live view from any page.
    const pushed = history.pushed;
    handlers.click();
    assert.ok(open() && history.pushed === pushed + 1, 'the header pill opens the live view');
    els.liveShotClose.onclick();
    assert.ok(!open(), 'the X closes the view opened from the pill');
    api.pop();
  }

  // iOS WebKit ignores muted video for display sleep: the silent keep-awake
  // track plays unmuted on iPhone/iPad (and the desktop-class iPadOS UA),
  // muted everywhere else.
  for (const [ua, touch, unmuted] of [
    ['(iPhone; CPU iPhone OS 18_1 like Mac OS X)', 5, true],
    ['(iPad; CPU OS 18_1 like Mac OS X)', 5, true],
    ['Macintosh; Intel Mac OS X 10_15_7', 5, true],
    ['Macintosh; Intel Mac OS X 10_15_7', 0, false],
    ['Linux; Android 15; Pixel 8', 5, false],
  ]) {
    const video = {muted: false, play() { return Promise.resolve(); }, setAttribute() {}};
    const hero = {classList: {add() {}, remove() {}}, querySelector: () => null,
      focus() {}, append() {}};
    const api = new Function('$', 'history', 'location', 'document', 'setTimeout',
        'clearTimeout', 'navigator', 'paintUiStream', 'renderLiveShot',
        runtimeJs.slice(runtimeJs.search(/let\s+shotFsActive\s*=/),
            runtimeJs.indexOf('// The diagnostic stream rides')) +
        ';return{enter:enterShotFullScreen}')(
        () => hero, {pushState() {}, back() {}}, {href: '/'},
        {body: {classList: {add() {}, remove() {}}}, createElement: () => video},
        () => 0, () => {}, {userAgent: ua, maxTouchPoints: touch}, () => {}, () => {});
    api.enter();
    assert.equal(video.muted, !unmuted,
        `${ua} (touch ${touch}) must ${unmuted ? 'play unmuted' : 'stay muted'}`);
    assert.ok(video.loop && video.playsInline);
  }

  // With the Wake Lock API the screen lock is taken on entry, released on
  // exit, re-taken when the page returns, and never kept after a late grant.
  (async () => {
    let requests = 0, releases = 0;
    const grants = [], grantAll = () => grants.splice(0).forEach(grant => grant());
    const lock = {release: () => { releases++; return Promise.resolve(); }};
    const els = {liveShot: {classList: {add() {}, remove() {}}, focus() {}},
      liveShotClose: {focus() {}}};
    const api = new Function('$', 'history', 'location', 'document', 'setTimeout', 'clearTimeout',
        'navigator', 'paintUiStream', 'renderLiveShot',
        runtimeJs.slice(runtimeJs.search(/let\s+shotFsActive\s*=/),
            runtimeJs.indexOf('// The diagnostic stream rides')) +
        ';return{enter:enterShotFullScreen,exit:exitShotFullScreen,awake:keepShotFsAwake}')(
        id => els[id], {pushState() {}, back() {}}, {href: '/'},
        {body: {classList: {add() {}, remove() {}}}, createElement() { throw Error('no video'); }},
        () => 0, () => {},
        {wakeLock: {request: type => { assert.equal(type, 'screen'); requests++;
          return new Promise(resolve => grants.push(() => resolve(lock))); }}},
        () => {}, () => {});
    const settle = () => new Promise(resolve => setImmediate(resolve));
    api.enter();
    grantAll(); await settle();
    assert.equal(requests, 1, 'entering requests a screen wake lock');
    api.exit();
    assert.equal(releases, 1, 'exiting releases the wake lock');
    api.enter();
    api.awake(true);  // The page became visible again while full screen.
    assert.equal(requests, 3, 'returning to the page re-takes the lock');
    grantAll(); await settle();
    assert.equal(releases, 2, 'overlapping grants keep one lock, releasing the older');
    api.exit();
    assert.equal(releases, 3, 'exiting releases the only lock still held');
    api.enter();
    api.exit();
    grantAll(); await settle();
    assert.equal(releases, 4, 'a lock granted after exit is released at once');
    const visibilityHandler = blockAt(runtimeJs, 'function onPageVisibility() {');
    assert.ok(codeIncludes(visibilityHandler, 'if (shotFsActive) keepShotFsAwake(true);') &&
        visibilityHandler.indexOf('if (document.hidden)') < visibilityHandler.indexOf('keepShotFsAwake(true)') &&
        codeIncludes(runtimeJs, 'document.addEventListener("visibilitychange", onPageVisibility);'),
        'the visibility handler re-takes the lock in full screen once visible');

    // The "Keep the screen on" app setting holds the same lock from shot
    // start to a minute past the end — alone, and alongside the live view.
    {
      let now = 0, requests = 0, releases = 0;
      const grants = [], grantAll = () => grants.splice(0).forEach(g => g());
      const timers = [], store = {obbwAppAwake: '1'};
      const lock = {release: () => { releases++; return Promise.resolve(); }};
      const els = {liveShot: {classList: {add() {}, remove() {}}, focus() {}, hidden: true},
        liveShotClose: {focus() {}}};
      const api = new Function('$', 'history', 'location', 'document', 'setTimeout',
          'clearTimeout', 'navigator', 'paintUiStream', 'renderLiveShot', 'Date', 'localStorage',
          runtimeJs.slice(runtimeJs.search(/let\s+shotFsActive\s*=/),
              runtimeJs.indexOf('// The diagnostic stream rides')) +
          ';return{enter:enterShotFullScreen,exit:exitShotFullScreen,shotAwake:keepShotAwake}')(
          id => els[id], {pushState() {}, back() {}}, {href: '/'},
          {body: {classList: {add() {}, remove() {}}}, hidden: false,
            createElement() { throw Error('no video'); }},
          fn => timers.push(fn), () => {},
          {wakeLock: {request: type => { assert.equal(type, 'screen'); requests++;
            return new Promise(resolve => grants.push(() => resolve(lock))); }}},
          () => {}, () => {}, {now: () => now},
          {getItem: k => store[k] || null, setItem: (k, v) => { store[k] = String(v); },
            removeItem: k => { delete store[k]; }});
      delete store.obbwAppAwake;
      api.shotAwake(true);
      assert.equal(requests, 0, 'with the setting off a shot holds nothing');
      store.obbwAppAwake = '1';
      api.shotAwake(true);
      assert.equal(requests, 1, 'a shot with the setting on takes the lock');
      grantAll(); await settle();
      api.shotAwake(false);
      assert.equal(releases, 0, 'the shot ending holds the screen one more minute');
      now = 60050;
      timers.splice(0).forEach(fire => fire());
      assert.equal(releases, 1, 'the grace minute past, the lock is released');
      // The live view keeps its own hold; leaving it inside the grace minute
      // re-takes the lock instead of dropping it, and the tail still ends it.
      requests = 0; releases = 0; timers.length = 0;
      api.enter();
      api.shotAwake(true);
      assert.equal(requests, 1, 'the live view requests its own lock');
      grantAll(); await settle();
      api.shotAwake(false);
      assert.equal(timers.length, 1, 'the grace tail is armed under the live view');
      api.exit();
      assert.equal(requests, 2, 'exiting inside the grace minute re-takes the lock');
      grantAll(); await settle();
      assert.equal(releases, 1, 'the re-take replaced the live view lock');
      now = 120100;
      timers.splice(0).forEach(fire => fire());
      assert.equal(releases, 2, 'the tail releases the lock after the grace minute');
      // A tail whose check fires under an open live view re-arms itself, so
      // leaving the view after the grace minute still releases the lock.
      requests = 0; releases = 0; timers.length = 0;
      api.shotAwake(true);
      grantAll(); await settle();
      api.enter();
      grantAll(); await settle();
      assert.equal(releases, 1, 'the live view entry replaced the shot lock');
      api.shotAwake(false);
      now = 240300;  // the grace minute passed with the live view open
      timers.splice(0).forEach(fire => fire());
      assert.equal(timers.length, 1, 'a blocked tail re-arms its check');
      assert.equal(releases, 1, 'the live view hold survives the blocked check');
      api.exit();
      assert.equal(releases, 2, 'leaving past the grace minute releases the lock');
      timers.splice(0).forEach(fire => fire());
      assert.equal(releases, 2, 'the re-armed check finds nothing left to release');
      assert.ok(
          codeIncludes(runtimeJs,
              'else if (Date.now() < shotAwakeUntil && appSetting("awake")) keepShotFsAwake(true);'),
          'returning visible re-takes the shot hold');
      // An interrupted shot (invalid card) or a dead stream must not hold the
      // screen forever: both paths drop the hold to its grace minute, and a
      // new clock re-arms it.
      assert.ok(
          codeIncludes(blockAt(runtimeJs, 'function syncShotClock(frame) {'), 'keepShotAwake(true);') &&
              codeIncludes(blockAt(runtimeJs, 'function syncShotClock(frame) {'), 'keepShotAwake(false);') &&
              codeIncludes(blockAt(runtimeJs, 'function resetUiStream() {'), 'keepShotAwake(false);'),
          'an interrupted shot or dead stream releases the shot hold');
    }
  })().catch(error => {console.error(error); process.exitCode = 1;});
}
// Safari only honours the prefixed property: the shot card must not select
// text while it is held for scrubbing or full screen.
if (!/\.shotHero\{[^}]*-webkit-user-select:none/.test(css)) {
  throw new Error('The shot card must disable text selection in Safari too');
}
