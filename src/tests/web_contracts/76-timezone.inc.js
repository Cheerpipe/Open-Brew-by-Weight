// This standalone chunk follows the complete power contracts, not a split expression.
// Run real timezone UI functions with controlled async responses and clocks.
{
  const config = network.slice(network.indexOf('OpenBrewByWeightNetwork::configHandler('));
  if (!config.includes('dateTimePatch && !browserTimezonePatch && !self.requireAdminUnlock(request)') ||
      !config.includes('"baseRevision", "timezoneId", "timezoneAutoInit", "timezoneDetected"') ||
      !config.includes('jsonHasOnlyUniqueFields(root, browserFields, 4)') ||
      !config.includes('timezoneAutoInit && !durableTimezoneSaved().load()') ||
      !config.includes('timezoneDetected && candidate.timezoneAutomatic')) {
    throw new Error('Browser detection must only bypass Admin for a restricted, eligible zone patch');
  }
}
{
  // Compatibility mode freezes Date & time: the whole patch is rejected even
  // though the browser zone patch normally bypasses the Admin session.
  const config = network.slice(network.indexOf('OpenBrewByWeightNetwork::configHandler('));
  if (!config.includes('dateTimePatch && firmwareCompatibilityMode()') ||
      !config.includes('"DATE_TIME_DISABLED"')) {
    throw new Error('Compatibility mode must reject Date & time config patches');
  }
}
{
  // Automatic sync off must still let one explicit Sync now complete: the
  // disabled gate may not abort an in-flight manual attempt, and any RF-gate
  // abort path must requeue the request instead of dropping it.
  const service = network.slice(network.indexOf('void OpenBrewByWeightNetwork::serviceNtp('));
  const failure = network.slice(
      network.indexOf('void OpenBrewByWeightNetwork::handleNtpFailure('),
      network.indexOf('void OpenBrewByWeightNetwork::serviceNtp('));
  const abortHelper = network.slice(
      network.indexOf('void OpenBrewByWeightNetwork::abortInFlightNtp()'),
      network.indexOf('void OpenBrewByWeightNetwork::abortNtpForRfGate('));
  const rfGate = service.slice(
      service.indexOf('if (!ntpMayArm(now, staConnected))'),
      service.indexOf('if (g_wallClock.applyPendingSync'));
  if (!networkHeader.includes('bool ntpManualAttempt_ = false;') ||
      !service.includes('!ntpSyncEnabled && !ntpManualSyncPending_ && !ntpManualAttempt_') ||
      !service.includes('ntpManualAttempt_ = manualRequest;') ||
      !service.includes('ntpManualAttempt_ = false;') ||
      !failure.includes('ntpManualAttempt_ = false;') ||
      !abortHelper.includes('ntpManualSyncPending_ = true;') ||
      !abortHelper.includes('ntpManualAttempt_ = false;') ||
      !rfGate.includes('abortInFlightNtp();') ||
      !network.slice(
          network.indexOf('void OpenBrewByWeightNetwork::abortNtpForRfGate('),
          network.indexOf('bool OpenBrewByWeightNetwork::armNtp('))
          .includes('abortInFlightNtp();')) {
    throw new Error('Disabled automatic sync must keep an explicit Sync now working end to end');
  }
}
{
  const assert = require('assert').strict, vm = require('vm');
  const slice = (a, b) => rawRuntimeJs.slice(rawRuntimeJs.indexOf(a), rawRuntimeJs.indexOf(b));
  // The timezone helpers stay in the runtime; the Admin module owns the
  // selector handlers, config load, and save. Shared state and runtime
  // references resolve to this harness's globals.
  const source = runtimeStateGlobals +
      // Top-level lets become globals so R.<binding> reads them like the
      // module namespace does.
      slice('async function populateTimezoneOptions(', 'function setEmptyState(')
          .replace(/^let /gm, 'var ') +
      ['function timeZoneSelectionChanged(', 'async function changeTimezoneMode(',
        'function loadAdminConfig(', 'async function saveDateTimeConfig(']
          .map((marker) => blockAt(rawViewJs.admin, marker)).join('\n');
  const flush = async () => { for (let i = 0; i < 12; ++i) await Promise.resolve(); };
  const deferred = () => { let resolve; const promise = new Promise(r => { resolve = r; }); return {promise, resolve}; };
  function harness() {
    const select = {value: '', isConnected: true, options: [{}],
      replaceChildren(f) { this.options = f.children; this.value = ''; }};
    const els = {timezoneId: select, timezoneAutomatic: {checked: true}, timezonePreview: {}, timezoneSaveState: {},
      ntpSyncEnabled: {checked: true}, ntpServerPreset: {value: 'pool'}, ntpServerCustom: {value: ''}};
    const calls = [], state = {zone: 'America/Santiago', commands: 0, detections: 0, payloads: []};
    const c = vm.createContext({$: id => els[id], Date, performance: {now: () => 1000},
      document: {hidden: false, createDocumentFragment: () => ({children: [], appendChild(o) { this.children.push(o); }})},
      Option: function(label, value) { this.value = value; },
      Intl: {DateTimeFormat: () => { ++state.detections; return {resolvedOptions: () => ({timeZone: state.zone})}; }},
      __WEBUI_TEXT__: key => key, formatWallTimeLocal: String, formatTzLabel: String,
      formatCommandError: (_, error) => error.message, body: JSON.stringify,
      setSaveDirty() {}, message() {}, clearFieldErrors() {}, showFieldError() {},
      validateDateTimeClient: () => null, withBaseRev: p => p,
      dateTimePayload: () => ({timezoneId: select.value, ntpServerPreset: 'pool', ntpServerCustom: ''}),
      markDateTimeDirty() { c.dateTimeDirty = true; }, refreshStatus: async () => {},
      compatMode: false, dateTimeDirty: false, controlsMutable: true, commandBusy: false, diagnosticUnlocked: true, activeView: 'admin',
      api: async (url, options) => {
        if (url.endsWith('/zones')) return ['Etc/UTC', 'America/Santiago', 'Asia/Tokyo'];
        if (!c.diagnosticUnlocked) throw new Error('Admin locked');
        const {timezoneId} = JSON.parse(options.body); calls.push(timezoneId);
        return {timezoneId, clockAvailable: true, clockSource: 'device', utcSec: 1800000000,
          localSec: 1800000000, offsetMinutes: 0, nextTransitionUtcSec: 0};
      },
      command: async (_, payload) => { ++state.commands; state.payloads.push(payload); }
    });
    vm.runInContext(source, c);
    return {c, select, els, calls, state};
  }
  const cases = [
    ['enabling automatic reads the current browser zone and locks the selector', async () => {
      const h = harness(); await h.c.populateTimezoneOptions();
      h.state.zone = 'Asia/Tokyo'; await h.c.changeTimezoneMode();
      assert.equal(h.select.value, 'Asia/Tokyo');
      assert.equal(h.select.disabled, true);
      h.els.timezoneAutomatic.checked = false; await h.c.changeTimezoneMode();
      assert.equal(h.select.disabled, false);
      assert.equal(h.state.detections, 1);
    }],
    ['status changes refresh the selected-zone preview', async () => {
      const h = harness(); await h.c.populateTimezoneOptions(); await flush();
      h.c.loadAdminConfig({timezoneId: 'Asia/Tokyo'}); await flush();
      assert.equal(h.select.value, 'Asia/Tokyo');
      assert.equal(h.calls.at(-1), 'Asia/Tokyo');
    }],
    ['late previews cannot replace the newest selection', async () => {
      const h = harness(); await h.c.populateTimezoneOptions(); await flush();
      const old = deferred(), api = h.c.api;
      h.c.api = async (url, options) => {
        const result = await api(url, options);
        if (result.timezoneId === 'America/Santiago') await old.promise;
        return result;
      };
      h.select.value = 'America/Santiago'; const first = h.c.refreshTimezonePreview();
      h.select.value = 'Asia/Tokyo'; await h.c.refreshTimezonePreview();
      old.resolve(); await first;
      assert.equal(vm.runInContext('timezonePreviewAnchor.timezoneId', h.c), 'Asia/Tokyo');
    }],
    ['preview recovers after Admin unlock without a zone edit', async () => {
      const h = harness(); h.c.diagnosticUnlocked = false;
      await h.c.populateTimezoneOptions(); await flush();
      h.c.diagnosticUnlocked = true; h.c.loadAdminConfig({timezoneId: 'America/Santiago'});
      await flush(); assert.equal(h.calls.at(-1), 'America/Santiago');
    }],
    ['initial detection serializes catalog waiters', async () => {
      const h = harness(), gate = deferred(), api = h.c.api;
      h.c.api = async (url, options) => { if (url.endsWith('/zones')) await gate.promise; return api(url, options); };
      const status = {timezoneId: '', timezoneInitialized: false};
      const a = h.c.syncTimezone(status);
      const b = h.c.syncTimezone(status);
      gate.resolve(); await Promise.all([a, b]); assert.equal(h.state.commands, 1);
    }],
    ['a lost initial save can be retried in the same browser', async () => {
      const h = harness(); await h.c.syncTimezone({timezoneId: '', timezoneInitialized: false});
      // The device rebooted before the flash write and again reports no zone.
      await h.c.syncTimezone({timezoneId: '', timezoneInitialized: false});
      assert.equal(h.state.commands, 2);
    }],
    ['initialization works on Home without Admin or its DOM, even in manual mode', async () => {
      const h = harness(); delete h.els.timezoneId; delete h.els.timezoneAutomatic;
      h.c.activeView = 'home'; h.c.diagnosticUnlocked = false;
      await h.c.syncTimezone({timezoneId: '', timezoneInitialized: false, timezoneAutomatic: false});
      assert.equal(h.state.payloads[0].timezoneAutoInit, true);
    }],
    ['manual mode performs no browser detection after a durable save', async () => {
      const h = harness();
      await h.c.syncTimezone({timezoneId: 'Asia/Tokyo', timezoneInitialized: true, timezoneAutomatic: false});
      assert.equal(h.state.detections, 0); assert.equal(h.state.commands, 0);
    }],
    ['compatibility mode performs no detection and sends no zone patch', async () => {
      const h = harness(), status = {timezoneId: '', timezoneInitialized: false, timezoneAutomatic: true};
      h.c.compatMode = true;
      await h.c.syncTimezone(status);
      assert.equal(h.state.detections, 0); assert.equal(h.state.commands, 0);
    }],
    ['automatic mode follows zone changes and avoids unchanged writes', async () => {
      const h = harness(), status = {timezoneId: 'America/Santiago', timezoneInitialized: true, timezoneAutomatic: true};
      await h.c.syncTimezone(status); assert.equal(h.state.commands, 0);
      h.state.zone = 'Asia/Tokyo'; await h.c.syncTimezone(status);
      assert.equal(h.state.payloads[0].timezoneDetected, true);
      assert.equal(h.state.payloads[0].timezoneId, 'Asia/Tokyo');
    }],
    ['initialization retries after a busy controller or a rejected command', async () => {
      const h = harness(), status = {timezoneId: '', timezoneInitialized: false};
      h.c.controlsMutable = false; await h.c.syncTimezone(status);
      assert.equal(h.state.detections, 0);
      h.c.controlsMutable = true;
      const command = h.c.command;
      h.c.command = async () => { throw Error('Busy'); };
      await h.c.syncTimezone(status);
      // A rejection backs off instead of posting again on the next frame.
      h.c.command = command; await h.c.syncTimezone(status);
      assert.equal(h.state.commands, 0);
      h.c.autoInitRetryAt = 0; await h.c.syncTimezone(status);
      assert.equal(h.state.commands, 1);
    }],
    ['an applied but unsaved zone leaves initialization active', async () => {
      const h = harness(); h.state.zone = 'Asia/Tokyo';
      await h.c.syncTimezone({timezoneId: 'America/Santiago', timezoneInitialized: false, timezoneAutomatic: false});
      assert.equal(h.state.payloads[0].timezoneAutoInit, true);
      await h.c.syncTimezone({timezoneId: 'Asia/Tokyo', timezoneInitialized: false, timezoneAutomatic: false});
      assert.equal(h.state.commands, 1); // The persistence worker retries this same zone.
    }],
    ['disabling automatic while the catalog loads cancels detection', async () => {
      const h = harness(), gate = deferred(), api = h.c.api;
      h.c.api = async (url, options) => { if (url.endsWith('/zones')) await gate.promise; return api(url, options); };
      const detection = h.c.changeTimezoneMode();
      h.els.timezoneAutomatic.checked = false; await h.c.changeTimezoneMode();
      gate.resolve(); await detection;
      assert.equal(h.state.detections, 0); assert.equal(h.select.disabled, false);
    }],
    ['a failed pending detection cannot erase a later manual selection', async () => {
      const h = harness(), gate = deferred();
      h.c.api = async () => { await gate.promise; throw Error('Unavailable'); };
      const detection = h.c.changeTimezoneMode();
      h.els.timezoneAutomatic.checked = false; await h.c.changeTimezoneMode();
      h.select.value = 'Asia/Tokyo'; gate.resolve(); await detection;
      assert.equal(h.select.value, 'Asia/Tokyo'); assert.equal(h.state.detections, 0);
    }],
    ['saving a prior draft retains edits made while it was pending', async () => {
      const h = harness(), gate = deferred(); h.c.command = () => gate.promise;
      h.select.value = 'America/Santiago'; h.c.dateTimeDirty = true;
      const save = h.c.saveDateTimeConfig();
      h.select.value = 'Asia/Tokyo'; h.c.timeZoneSelectionChanged();
      gate.resolve(); await save; assert.equal(h.c.dateTimeDirty, true);
    }],
    ['a command rejection cannot be acknowledged by an unrelated revision', async () => {
      const code = rawRuntimeJs.match(/const sleep = .*\n/)[0] + blockAt(rawRuntimeJs, 'async function command(');
      let polls = 0;
      const c = vm.createContext({configRevision: 10, lastCommandStatus: null, homeStale: true,
        withCommandGate: fn => fn(), api: async () => ({requestId: 7}), body: JSON.stringify,
        clearFieldErrors() {}, noteReachOk() {}, message() {}, commandBusy: false,
        __WEBUI_TEXT__: key => key, setTimeout: fn => { fn(); },
        refreshStatus: async () => {
          ++polls; c.configRevision = 11;
          c.lastCommandStatus = polls === 1 ? {requestId: 6, state: 'APPLIED'} : {requestId: 7, state: 'FAILED'};
        }});
      vm.runInContext(code, c);
      await assert.rejects(c.command('/api/v1/config', {timezoneId: 'Asia/Tokyo'}, true, '', ''));
      assert.equal(polls, 2);
    }],
    ['command success waits for its own applied result', async () => {
      const code = rawRuntimeJs.match(/const sleep = .*\n/)[0] + blockAt(rawRuntimeJs, 'async function command(');
      let polls = 0;
      const c = vm.createContext({configRevision: 10, lastCommandStatus: null, homeStale: true,
        withCommandGate: fn => fn(), api: async () => ({requestId: 7}), body: JSON.stringify,
        clearFieldErrors() {}, noteReachOk() {}, message() {}, commandBusy: false,
        __WEBUI_TEXT__: key => key, setTimeout: fn => { fn(); },
        refreshStatus: async () => {
          ++polls; c.configRevision = 11;
          c.lastCommandStatus = {requestId: 7, state: polls === 1 ? 'QUEUED' : 'APPLIED'};
        }});
      vm.runInContext(code, c);
      assert.equal(await c.command('/api/v1/config', {timezoneId: 'Asia/Tokyo'}, true, '', ''), true);
      assert.equal(polls, 2);
    }],
    ['a live Home stream confirms a preset save without status polls', async () => {
      const code = rawRuntimeJs.match(/const sleep = .*\n/)[0] + blockAt(rawRuntimeJs, 'async function command(');
      let polls = 0, ticks = 0;
      const c = vm.createContext({configRevision: 10, lastCommandStatus: null, homeStale: false,
        withCommandGate: fn => fn(), api: async () => ({requestId: 7}), body: JSON.stringify,
        clearFieldErrors() {}, noteReachOk() {}, message() {}, commandBusy: false,
        __WEBUI_TEXT__: key => key,
        setTimeout: fn => {
          if (++ticks === 3) { c.configRevision = 11; c.lastCommandStatus = {requestId: 7, state: 'PERSISTED'}; }
          fn();
        },
        refreshStatus: async () => { ++polls; }});
      vm.runInContext(code, c);
      assert.equal(await c.command('/api/v1/presets', {op: 'save'}, true, '', ''), true);
      assert.equal(polls, 1, 'Home frames confirm the save; REST only repaints the view once');
    }]
  ];
  (async () => {
    let failures = 0;
    for (const [name, run] of cases) {
      try { await run(); } catch (error) { ++failures; console.error(name, error); }
    }
    if (failures) throw new Error(`${failures} timezone regressions failed`);
  })().catch(error => { console.error(error); process.exitCode = 1; });
}
