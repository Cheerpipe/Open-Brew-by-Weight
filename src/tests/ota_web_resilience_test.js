#!/usr/bin/env node
'use strict';
// Exercise the production OTA client with a simulated HTTP transport and DOM.
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');
const jsDir = path.join(__dirname, '../web/js');
// The OTA client lives in the Admin view module and reaches the shared
// runtime through its R namespace; the harness below provides that
// namespace instead of the real runtime.js imports.
const admin = fs.readFileSync(path.join(jsDir, 'admin.js'), 'utf8');
const locale = JSON.parse(fs.readFileSync(
    path.join(jsDir, '../locales/en.json'), 'utf8')).strings;
const otaSource = admin.slice(admin.search(/const\s+OTA_UPLOAD_TIMEOUT_MS\s*=/),
    admin.indexOf('async function setBleScanIntensity('));
const runtime = fs.readFileSync(path.join(jsDir, 'runtime.js'), 'utf8');
const restartSource = runtime.slice(runtime.indexOf('// Only the firmware\'s own status with another boot'),
    runtime.indexOf('// The firmware says how the last network change ended'));
const wifiSource = runtime.slice(runtime.indexOf('// The firmware says how the last network change ended'),
    runtime.indexOf('// Each card is moved to the view showing it'));
const parser = new Function(fs.readFileSync(path.join(jsDir, 'ota-image.js'), 'utf8')
    .replace(/export\s+/g, '') + ';return otaFileIdentity;')();

function fixture() {
  const image = Buffer.alloc(131104, 0x5a);
  image[0] = 0xe9; image[12] = 9; image[13] = 0; image[23] = 1;
  image.writeUInt32LE(0xabcd5432, 32);
  image.fill(0, 80, 112); image.write('openbrewbyweight', 80);
  image.write('OPENBREWBYWEIGHT_FW_TAG_V2|arch=n16r8|hw=legacy|machine=legacy|ver=1.2.3|packed=123|END', 320);
  crypto.createHash('sha256').update(image.subarray(0, -32)).digest().copy(image, image.length - 32);
  return new Blob([image]);
}

async function harness(mode) {
  const file = fixture(), identity = await parser(file), items = new Map(), nodes = new Map();
  // 'changed' mutates the backing bytes mid-upload, like a parallel build
  // rewriting the image while the browser re-reads each range from disk.
  const source = mode === 'changed' ? Buffer.from(await file.arrayBuffer()) : null;
  const uploadFile = source ?
      {size: source.length, slice: (a, b) => new Blob([source.subarray(a, b)])} : file;
  const messages = [], patches = [], payloads = [], delays = [], waits = [], flashes = [];
  let offset = 0, posts = 0, commits = 0;
  const session = {...identity, transferId: 'test-transfer'};
  const status = () => ({otaProtocolVersion: 3, available: true, safe: true,
    confirmed: true, runningIdentityValid: true, bootId: 7,
    running: {arch: identity.arch, hardware: identity.hardware,
      machine: identity.machine, version: identity.version,
      imageSha256: identity.imageSha256},
    state: offset === file.size ? 'staged' : 'receiving', sessionActive: true,
    transferId: session.transferId, expectedBytes: file.size, sha256: identity.sha256,
    sessionArch: identity.arch, sessionHardware: identity.hardware,
    sessionMachine: identity.machine, sessionVersion: identity.version, nextOffset: offset,
    staged: {arch: identity.arch, hardware: identity.hardware,
      machine: identity.machine, version: identity.version, packed: identity.packed,
      imageSha256: identity.imageSha256}});
  const R = {uiState: {}, controlsMutable: true,
    message: text => messages.push(text), clearFieldErrors() {}, showFieldError() {},
    beginFirmwareRestartWait: (...args) => waits.push(args), noteFirmwareFlash: note => flashes.push(note),
    formatCommandError: (text, error) => text + ' ' + error.message, refreshStatus() {},
    sleep: async (ms) => { delays.push(ms); },
    sub: (text, values) => text.replace(/\{(\d)\}/g, (_, i) => values[i])};
  const context = vm.createContext({console, crypto: crypto.webcrypto,
  __WEBUI_TEXT__: key => locale[key] || key, localStorage: {
    getItem: key => items.get(key) || null,
    setItem: (key, value) => items.set(key, value), removeItem: key => items.delete(key)},
  $: key => {
    if (!nodes.has(key)) nodes.set(key, {files: [uploadFile], value: 0, classList: {add() {}, remove() {}}});
    return nodes.get(key);
  }, confirm: () => true, R});
  vm.runInContext(otaSource, context);
  context.otaFileIdentity = parser;
  const send = async (url, body, timeout, method = 'POST', headers = {}) => {
    if (method === 'POST' && url.endsWith('/session')) {
      posts++;
      const request = JSON.parse(body); payloads.push(request);
      assert.deepEqual(Object.keys(request).sort(),
        ['arch', 'hardware', 'machine', 'sha256', 'size', 'transferId', 'version']);
      if (mode === 'refused') throw Object.assign(new Error('PENDING_VERIFY'), {status: 409});
      return status();
    }
    if (method === 'PATCH') {
      const at = Number(headers['X-OTA-Offset']); patches.push(at);
      if (mode === 'changed' && patches.length === 1) source[70000] ^= 0xff;
      assert.deepEqual(Buffer.from(await body.arrayBuffer()),
          Buffer.from(await file.slice(at, at + body.size).arrayBuffer()));
      if (mode === 'rewind' && patches.length === 2) {offset = 4096; throw new Error('Device unreachable');}
      if (mode === 'stalled' || mode === 'expired') throw new Error('Device unreachable');
      offset = at + body.size;
      if (mode === 'lost' && patches.length === 1) throw new Error('Device unreachable');
      return status();
    }
    if (method === 'POST' && url.endsWith('/flash')) {
      commits++;
      throw new Error('Device unreachable');
    }
    if (method === 'GET') {
      if (mode === 'expired' && patches.length) return {...status(), sessionActive: false, state: 'idle', transferId: ''};
      if (commits) {
        if (mode === 'wrong-commit') return {...status(), state: 'committed', restartPending: true, transferId: 'other'};
        if (mode === 'lost-commit') return {...status(), state: 'committed', restartPending: true};
        if (mode === 'booted-commit') return {...status(), state: 'idle', bootId: 8, transferId: ''};
        if (mode === 'refused-flash') return status();
        if (mode === 'offline-flash' || (mode === 'offline-retry' && commits > 1))
          throw new Error('Device unreachable');
      }
      return status();
    }
    throw new Error('Unexpected request');
  };
  context.otaSend = send;
  return {context, session, identity, items, messages, patches, payloads, file, delays, waits, flashes,
    status, setOffset: value => {offset = value;}, commits: () => commits};
}

// The wait after a flash, against the answers each route really gives.
function restartContext(answers, pages = () => ({ok: true})) {
  const log = {landed: [], claims: 0, posted: 0, messages: [], polled: 0, flashes: []};
  const context = vm.createContext({console, Date, Number, JSON,
    localStorage: {getItem: () => null, setItem() {}, removeItem() {}},
    __WEBUI_TEXT__: key => key, webUiHeaders: () => ({}),
    fetchJson: async (url, options) => {
      if (url === '/api/v1/ui/claim') {
        assert.equal(options.method, 'POST');
        log.posted++;
        return [{ok: true}, {active: true}];
      }
      assert.equal(url, '/api/v1/status/admin');
      const answer = answers[log.polled++];
      if (answer === 'offline') throw new TypeError('Failed to fetch');
      return [{ok: answer.status === 200}, answer.body];
    },
    fetch: async url => pages(url), setTimeout: fn => { log.next = fn; return 1; },
    beginNetworkReconnectWait() {}, endNetworkReconnectWait: () => { log.ended = true; },
    claimWebUiOwnership: () => { log.claims++; }, message: text => log.messages.push(text),
    sub: (text, values) => text + ':' + values.join(','),
    location: {replace: url => log.landed.push(url)}, networkReconnectPollTimer: 0,
    networkReconnectDeadline: 0});
  vm.runInContext('let firmwareRestart = null, fwReloading = false;' +
      restartSource.replace(/^const FW_RESULT_KEY/m, 'var FW_RESULT_KEY'), context);
  vm.runInContext('beginFirmwareRestartWait(7, "2.0"); globalThis.peek = () => firmwareRestart;', context);
  log.poll = async () => {
    vm.runInContext('pollFirmwareRestart()', context);
    for (let i = 0; i < 6; i++) await new Promise(resolve => setImmediate(resolve));
  };
  log.context = context;
  return log;
}

async function restartWaitChecks() {
  const ok = body => ({status: 200, body});
  // Down, directly and through a companion, and the old boot still answering.
  let w = restartContext(['offline', {status: 502, body: {error: 'CONTROLLER_UNREACHABLE'}},
    {status: 503, body: {error: 'NO_CONTROLLER'}}, ok({bootId: 7, configMutable: true}),
    {status: 200, body: null}, ok({bootId: 8, firmwareVersion: '2.0'})]);
  for (let i = 0; i < 5; i++) {
    await w.poll();
    assert.equal(w.landed.length, 0, 'answer ' + i + ' does not end the wait');
    assert.notEqual(vm.runInContext('peek()', w.context), null);
  }
  await w.poll();
  assert.deepEqual(w.landed, ['/?fw=2.0']);
  assert.equal(vm.runInContext('peek()', w.context), null);
  assert.equal(w.posted, 0, 'no claim while the status answers: it would end the Admin unlock');
  // A restarted controller holds no claim, and refuses the status until there is one.
  w = restartContext([{status: 409, body: {error: 'UI_TAKEN_OVER'}}, ok({bootId: 8, firmwareVersion: '2.0'})]);
  await w.poll();
  assert.equal(w.posted, 1);
  assert.deepEqual(w.landed, ['/?fw=2.0']);
  // The page itself still failing keeps the wait until a later poll finds it.
  let pages = 0;
  w = restartContext([ok({bootId: 8, firmwareVersion: '2.0'}), ok({bootId: 8, firmwareVersion: '2.0'})],
      () => ({ok: pages++ > 0}));
  await w.poll();
  assert.equal(w.landed.length, 0);
  await w.poll();
  assert.deepEqual(w.landed, ['/?fw=2.0']);
  // A rollback lands on the version that really runs; a sign-in on the expected one.
  w = restartContext([ok({bootId: 8, firmwareVersion: '1.0'})]);
  await w.poll();
  assert.deepEqual(w.landed, ['/?fw=1.0']);
  w = restartContext([{status: 401, body: {error: 'AUTH_REQUIRED'}}]);
  await w.poll();
  assert.deepEqual(w.landed, ['/?fw=2.0']);
  // A shot holds the count and slows the polls.
  w = restartContext([ok({bootId: 7, configMutable: false})]);
  await w.poll();
  assert.equal(vm.runInContext('peek().shot', w.context), true);
  // No restart coming: back to the panel, saying which case it is.
  for (const [state, text] of [['committed', 'never_came'], ['staged', 'was_not_installed']]) {
    w = restartContext([ok({bootId: 7, configMutable: true, ota: {restartPending: false, state}})]);
    await w.poll();
    assert.equal(vm.runInContext('peek()', w.context), null);
    assert.equal(w.claims, 1);
    assert.ok(w.ended);
    assert.match(w.messages[0], new RegExp(text));
  }
}

function firmwareResultChecks() {
  const store = new Map(), posted = [];
  // The controller's record of dismissed cards, as each status carries it.
  let seen = {image: '', rollback: '', grind: []};
  const context = vm.createContext({JSON, Number, __WEBUI_TEXT__: key => key,
    localStorage: {getItem: key => store.get(key) ?? null,
      setItem: (key, value) => store.set(key, value), removeItem: key => store.delete(key)},
    api: async (url, options) => {
      assert.equal(url, '/api/v1/home/dismiss');
      const body = JSON.parse(options.body);
      posted.push(body);
      if (body.key) seen = {...seen, [body.card]: body.key};
    },
    renderHomeCards() {}, sub: (text, values) => text + ':' + values.join(',')});
  vm.runInContext('let firmwareRestart = null, fwReloading = false;' + restartSource + wifiSource, context);
  const result = status => {
    context.status = status;
    return vm.runInContext('dismissed = status.dismissed; firmwareResult(status)', context);
  };
  const flash = {version: '2.0', imageSha256: 'abcdef0123456789' + 'f'.repeat(48), bootId: 7};
  const note = () => {
    context.flash = flash;
    vm.runInContext('noteFirmwareFlash(flash)', context);
  };
  const image = flash.imageSha256.slice(0, 16), oldImage = '1'.repeat(16);
  const run = (bootId, firmwareVersion, runningImage, rolledBackImage = '') =>
    result({bootId, firmwareVersion, runningImage, rolledBackImage, dismissed: seen});
  // With nothing acknowledged yet, the running image is news once.
  let card = run(8, '1.0', oldImage);
  assert.equal(card.failed, false);
  card.dismiss();
  assert.deepEqual(posted.at(-1), {card: 'image', key: oldImage});
  assert.equal(run(9, '1.0', oldImage), null);
  // An update from anywhere reaches every browser until one dismisses it.
  card = run(9, '2.0', image);
  assert.match(card.text, /firmware_updated_text:2\.0/);
  store.clear();
  assert.ok(run(9, '2.0', image), 'another browser');
  card.dismiss();
  assert.equal(run(10, '2.0', image), null, 'dismissed for every browser');
  // A reinstall from this browser reports here, from the next boot on.
  note();
  assert.equal(run(7, '2.0', image), null, 'same boot');
  card = run(8, '2.0', image);
  assert.match(card.text, /firmware_updated_text:2\.0/);
  card.dismiss();
  assert.equal(store.has('obbwFwResult'), false, 'dismissing ends this flash');
  assert.equal(run(9, '2.0', image), null);
  // A rollback overrides success, including a reinstall of the same version,
  // which only the firmware's record can tell.
  note();
  card = run(9, '2.0', image, image);
  assert.equal(card.failed, true);
  assert.match(card.text, /firmware_rollback_named:2\.0,2\.0/);
  card = run(9, '1.0', oldImage, image);
  assert.match(card.text, /firmware_rollback_named:2\.0,1\.0/);
  // Dismissed stays dismissed everywhere, until this browser flashes it again.
  card.dismiss();
  assert.deepEqual(posted.at(-1), {card: 'rollback', key: image});
  assert.equal(run(10, '1.0', oldImage, image), null);
  note();
  assert.equal(run(10, '1.0', oldImage, image).failed, true);
  // Another image's rollback, as after a CLI update, names no version of ours.
  assert.match(run(10, '1.0', oldImage, '0123456789abcdef').text, /firmware_rollback_text:1\.0/);
  // A command line update after a Web UI one, with that card dismissed or not.
  for (const dismissed of [true, false]) {
    note();
    card = run(8, '2.0', image);
    if (dismissed) card.dismiss();
    const next = (dismissed ? '3' : '4').repeat(16), version = dismissed ? '3.0' : '4.0';
    card = run(11, version, next);
    assert.equal(card.failed, false, 'CLI after a Web UI update');
    assert.match(card.text, new RegExp('firmware_updated_text:' + version));
    assert.equal(store.has('obbwFwResult'), false, 'that flash is over');
    card.dismiss();
    assert.equal(run(12, version, next), null);
  }
  // Two builds with the same version, as two of a dirty tree, are still an update,
  // and the card says when this one was built once the firmware reports it.
  card = run(13, '4.0', '5'.repeat(16));
  assert.match(card.text, /firmware_updated_text:4\.0$/);
  card = result({bootId: 13, firmwareVersion: '4.0', runningImage: '5'.repeat(16), rolledBackImage: '',
    firmwareBuiltAt: 1793900000, dismissed: seen});
  assert.match(card.text, /firmware_updated_text:4\.0 shell\.firmware_built_text:.*2026/);
  card.dismiss();
  // A Wi-Fi change waits for the firmware's word, which a name could not give
  // for a wrong password on the same network; every browser shows it, and
  // Dismiss ends it for all of them.
  const wifi = status => {
    context.status = status;
    return vm.runInContext('wifiResult(status)', context);
  };
  assert.equal(wifi({wifiChangeResult: '', wifiChangeSsid: 'Home'}), null, 'pending');
  assert.equal(wifi({wifiChangeResult: 'CONFIRMED', wifiChangeSsid: 'Home'}).failed, false);
  const reverted = {wifiChangeResult: 'REVERTED', wifiChangeSsid: 'Home'};
  card = wifi(reverted);
  assert.equal(card.failed, true);
  assert.match(card.text, /wifi_failed_text:Home/);
  card.dismiss();
  assert.deepEqual(posted.at(-1), {card: 'wifi'});
  assert.equal(wifi(reverted), null, 'hidden at once');
  // Saving starts the result over, and only the two endings set it.
  const firmware = fs.readFileSync(path.join(__dirname, '../network/OpenBrewByWeightWifi.inc'), 'utf8');
  for (const [fn, value] of [['::armPendingConfirmWindow', ''], ['::confirmPendingNetwork', 'CONFIRMED'],
    ['::revertPendingNetwork', 'REVERTED']]) {
    const start = firmware.indexOf(fn), body = firmware.slice(start, firmware.indexOf('\n}\n', start));
    assert.ok(body.includes(`status_.staChangeResult = "${value}";`), fn);
    assert.equal(body.includes('copyCString(status_.staChangeSsid, sizeof(status_.staChangeSsid), settings_.staSsid);'),
        !value, fn + ': the network on trial is named when the window opens');
  }
}

(async () => {
  for (const mode of ['normal', 'lost', 'rewind']) {
    const h = await harness(mode);
    await vm.runInContext('otaUpload()', h.context);
    assert.equal(h.payloads.length, 1);
    assert.match(h.messages.at(-1), /Firmware verified/);
    assert.deepEqual(h.patches, mode === 'rewind' ? [0, 65536, 4096, 69632] : [0, 65536, 131072]);
    const saved = JSON.parse(h.items.get('obbwOtaSession'));
    assert.equal(saved.imageSha256, h.identity.imageSha256);
    assert.notEqual(saved.imageSha256, saved.sha256);
  }
  for (const mode of ['refused', 'stalled', 'expired']) {
    const h = await harness(mode);
    await vm.runInContext('otaUpload()', h.context);
    if (mode === 'stalled') {
      assert.equal(h.patches.length, 3);
      assert.deepEqual(h.delays, [1000, 2000, 3000], 'retries back off before reconciling');
      assert.match(h.messages.at(-1), /upload paused/);
    } else {
      assert.doesNotMatch(h.messages.at(-1), /upload paused/);
      assert.equal(h.patches.length, mode === 'refused' ? 0 : 1);
    }
  }
  {
    const h = await harness('changed');
    await vm.runInContext('otaUpload()', h.context);
    assert.equal(h.patches.length, 1);
    assert.match(h.messages.at(-1), /changed while it was being uploaded/);
    assert.doesNotMatch(h.messages.at(-1), /upload paused|rejected/);
  }
  // A flash the device keeps refusing leaves the same boot staged: no commit
  // expectation may linger to report an unverified update later.
  {
    const h = await harness('refused-flash');
    h.setOffset(h.file.size);
    h.items.set('obbwOtaSession', JSON.stringify(h.session));
    h.context.R.uiState.otaLastStatus = h.status();
    await vm.runInContext('otaFlash()', h.context);
    assert.equal(h.commits(), 3);
    assert.equal(h.items.has('obbwOtaCommit'), false);
    assert.equal(h.waits.length, 0);
    assert.equal(h.flashes.at(-1), null, 'no result card reports a flash that did not happen');
  }
  // A device that stops answering after the flash request may be restarting
  // into the new image: stale pre-flash status must not drop the expectation.
  for (const [mode, flashes] of [['offline-flash', 1], ['offline-retry', 2]]) {
    const h = await harness(mode);
    h.setOffset(h.file.size);
    h.items.set('obbwOtaSession', JSON.stringify(h.session));
    h.context.R.uiState.otaLastStatus = h.status();
    await vm.runInContext('otaFlash()', h.context);
    assert.equal(h.commits(), flashes);
    assert.deepEqual(h.waits, [[7, h.identity.version]], mode + ' waits for the restart');
    assert.equal(h.items.has('obbwOtaCommit'), true, mode + ' keeps the commit expectation');
  }
  // The real transport is the shared API client, and each step keeps the UI active.
  {
    const h = await harness('normal'), calls = [];
    const context = vm.createContext({R: {resetWebUiInactivity: () => calls.push('active'),
      api: (...args) => calls.push(args)}, __WEBUI_TEXT__: (key) => key});
    vm.runInContext(otaSource, context);
    context.otaSend('/api/v1/ota', 'chunk', 5, 'PATCH', {'X-OTA-Offset': '0'});
    assert.equal(JSON.stringify(calls), JSON.stringify(['active', ['/api/v1/ota',
      {method: 'PATCH', body: 'chunk', headers: {'X-OTA-Offset': '0'}, timeoutMs: 5}]]));
    // An image for another hardware or machine profile names both profiles.
    h.context.identity = {...h.identity, machine: 'other-machine'};
    h.context.remote = h.status();
    assert.throws(() => vm.runInContext('otaSessionIdentity(identity,remote)', h.context),
        /built for legacy on other-machine, but this controller runs legacy on legacy/);
  }
  for (const mode of ['lost-commit', 'wrong-commit', 'booted-commit']) {
    const h = await harness(mode);
    h.setOffset(h.file.size);
    h.items.set('obbwOtaSession', JSON.stringify(h.session));
    h.context.R.uiState.otaLastStatus = h.status();
    await vm.runInContext('otaFlash()', h.context);
    assert.equal(h.commits(), 1);
    assert.equal(h.items.has('obbwOtaCommit'), mode !== 'booted-commit');
    if (mode === 'wrong-commit') {
      assert.match(h.messages.at(-1), /unverified/);
      assert.equal(h.waits.length, 0);
      assert.equal(h.flashes.at(-1), null);
    } else {
      assert.deepEqual(h.waits, [[7, h.identity.version]]);
      assert.deepEqual({...h.flashes[0]}, {version: h.identity.version,
        imageSha256: h.identity.imageSha256, bootId: 7});
    }
  }
  const h = await harness('normal');
  h.context.expected = {...h.session, bootId: 7};
  // A boot into another image settles the commit expectation after one report.
  h.items.set('obbwOtaCommit', JSON.stringify(h.context.expected));
  h.context.remote = {...h.status(), bootId: 8, running: {...h.status().running, imageSha256: 'b'.repeat(64)}};
  assert.match(vm.runInContext('otaCheckBoot(expected,remote)', h.context), /another image/);
  assert.equal(h.items.has('obbwOtaCommit'), false);
  for (const [change, pattern] of [
    [{bootId: 7}, /not yet verified/],
    [{bootId: 8, confirmed: false, confirmBlockReason: 'ble-active'}, /confirmation pending \(ble-active\)/],
    [{bootId: 8, running: {...h.status().running, imageSha256: 'a'.repeat(64)}}, /another image/],
    [{bootId: undefined}, /unverified/],
    [{bootId: 8}, /OTA confirmed/],
  ]) {
    h.context.remote = {...h.status(), ...change};
    assert.match(vm.runInContext('otaCheckBoot(expected,remote)', h.context), pattern);
  }
  // Reload/unlock uses the persisted expectation when Admin applies OTA status.
  h.items.set('obbwOtaCommit', JSON.stringify({...h.session, bootId: 7}));
  h.context.remote = {...h.status(), bootId: 8};
  assert.match(vm.runInContext('otaStatusText(remote)', h.context), /OTA confirmed/);
  assert.equal(h.items.has('obbwOtaCommit'), false);
  await restartWaitChecks();
  firmwareResultChecks();
  console.log('OTA Web resilience: strict sessions, recovery, commit and boot identity OK');
})().catch(error => {console.error(error); process.exitCode = 1;});
