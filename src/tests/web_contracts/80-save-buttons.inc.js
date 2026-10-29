{
  const assert = require('assert');
  const vm = require('vm');
  const elements = new Map();
  const element = (id, initial = []) => {
    const classes = new Set(initial);
    const node = {disabled: false, dataset: {}, classList: {
      add: name => classes.add(name),
      contains: name => classes.has(name),
      toggle: (name, on) => on ? classes.add(name) : classes.delete(name)}};
    elements.set(id, node);
    return node;
  };
  const button = element('save'), hint = element('hint');
  button.dataset.dirty = '0';
  const revert = element('revertDateTimeButton');
  revert.dataset.dirty = '0';
  const fwRevert = element('revertFirmwareModeButton');
  fwRevert.dataset.dirty = '0';
  const context = vm.createContext({$: id => elements.get(id), controlsMutable: true});
  const helper = [blockAt(viewJs.admin, 'function setSaveDirty('),
    blockAt(viewJs.admin, 'const REVERT_BUTTONS')];
  assert(helper.every(Boolean), 'Missing Admin dirty-save helper and revert pairing map');
  vm.runInContext('var R = this;' + helper.join('\n'), context);
  vm.runInContext("setSaveDirty('save','hint',true)", context);
  assert(!button.disabled && button.dataset.dirty == 1 &&
         !hint.classList.contains('hidden'));
  vm.runInContext("setSaveDirty('save','hint',false)", context);
  assert(button.disabled && hint.classList.contains('hidden'));
  vm.runInContext("setSaveDirty('saveDateTimeButton','hint',true)", context);
  assert(!revert.disabled && revert.dataset.dirty == 1,
      'The paired revert button must mirror its save button dirty state');
  vm.runInContext("setSaveDirty('saveDateTimeButton','hint',false)", context);
  assert(revert.disabled && revert.dataset.dirty == 0);
  vm.runInContext("setSaveDirty('saveFirmwareModeButton','hint',true)", context);
  assert(!fwRevert.disabled && fwRevert.dataset.dirty == 1,
      'Dirtying the firmware-mode toggle must enable its paired revert button');
  vm.runInContext("setSaveDirty('saveFirmwareModeButton','hint',false)", context);
  assert(fwRevert.disabled && fwRevert.dataset.dirty == 0);
  context.controlsMutable = false;
  vm.runInContext("setSaveDirty('save','hint',true)", context);
  assert(button.disabled, 'Locked configuration must keep dirty save buttons disabled');
  vm.runInContext("setSaveDirty('saveDateTimeButton','hint',true)", context);
  assert(revert.disabled, 'Locked configuration must keep revert buttons disabled');

  const adminUi = viewJs.admin, normalizedUi = ui.replace(/\\"/g, '"');
  for (const id of ['saveWebhookButton',
    'saveNetworkButton', 'saveDateTimeButton', 'changeDevicePasswordButton',
    'saveFirmwareModeButton', 'saveBleButton', 'savePowerButton', 'saveFrontendButton',
    'saveScheduledRestartButton'])
    assert(new RegExp(`id="${id}"[^>]*data-dirty="0"[^>]*disabled`).test(normalizedUi),
        `${id} must start visibly disabled`);
  for (const id of ['revertWebhookButton', 'revertNetworkButton', 'revertDateTimeButton', 'revertDevicePasswordButton',
    'revertFirmwareModeButton', 'revertBleButton', 'revertPowerButton', 'revertFrontendButton',
    'revertScheduledRestartButton'])
    assert(new RegExp(`id="${id}"[^>]*data-dirty="0"[^>]*disabled`).test(normalizedUi),
        `${id} must start visibly disabled next to its save button`);
  // Settings saves every edit by itself: no Save/Revert button or unsaved hint.
  assert(!/id="(saveConfigButton|saveBrewPresetButton|lineaMicraSaveButton|revert(Config|BrewPreset|LineaMicra)Button|(config|brew|lineaMicra)DirtyHint)"/
      .test(partialHtml.settings), 'Settings must not offer Save/Revert buttons or unsaved hints');
  assert(codeIncludes(viewJs.settings,
      "if(['presetRenameInput','dripSetG','alphaSet'].includes(el.id)||el.closest('#lineaMicraSignIn,#lineaMicraChoose,[data-g=app]'))return"),
      'The preset rename dialog, the drip set fields, Micra sign-in inputs and the browser-local app group must not dirty a settings section');
  assert(codeIncludes(js, 'e.dataset.dirty!=null') &&
         codeIncludes(js, "setSaveDirty('saveDateTimeButton','dateTimeDirtyHint',false)") &&
         codeIncludes(js, 'await refreshStatus();return false') &&
         codeIncludes(adminUi, "const networkChanged=()=>R.setSaveDirty('saveNetworkButton','',true)") &&
         codeIncludes(adminUi, "R.setSaveDirty('saveWebhookButton','webhookDirtyHint',true)") &&
         codeIncludes(adminUi, "R.setSaveDirty('changeDevicePasswordButton','',false)") &&
         codeIncludes(adminUi, "R.command('/api/v1/network',payload,undefined,undefined,undefined,'saveNetworkButton',).then((ok)=>{if(!ok)return") &&
         codeIncludes(adminUi, "R.command('/api/v1/device/password',{newPassword:") &&
         // Every Admin section marks itself dirty through one named helper.
         codeIncludes(adminUi, "for(const name of['ble','power','frontend','scheduledRestart'])sectionChanged('#'+name+'Panel',()=>sectionDirty(name,true))") &&
         codeIncludes(adminUi, "R.setSaveDirty('save'+name[0].toUpperCase()+name.slice(1)+'Button',name+'DirtyHint',on)") &&
         codeIncludes(adminUi, "const b=$('savePowerButton');b.classList.add('busy')") &&
         css.includes('.btnGlyph:disabled{opacity:.4;cursor:not-allowed}'),
  'Save actions must enable on edits, disable only after success, and look disabled');
  assert(codeIncludes(js, 'function snapshotControls(') &&
         codeIncludes(js, 'function restoreSnapshot(') &&
         codeIncludes(js, 'formBaseline=snapshotControls(els)') &&
         // An edit the device now holds is settled: its field follows the device again.
         codeIncludes(js, 'for(const id in edited)if(same(edited[id],uiState.formBaseline[id]))delete edited[id];restoreSnapshot(edited)') &&
         codeIncludes(js, 'if(!dateTimeDirty)dateTimeBaseline=snapshotControls(') &&
         codeIncludes(js, 'if(!bleDirty)bleBaseline=snapshotControls(') &&
         codeIncludes(js, 'if(!powerDirty)powerBaseline=snapshotControls(') &&
         codeIncludes(js, 'if(!frontendDirty)frontendBaseline=snapshotControls(') &&
         codeIncludes(js, 'networkBaseline=snapshotControls(networkControls())') &&
         codeIncludes(adminUi, 'webhookBaseline=R.snapshotControls(WEBHOOK_IDS.map((id)=>$(id)))') &&
         codeIncludes(js, 'confirm("Discard unsaved changes?")') &&
         codeIncludes(adminUi, 'confirm("Discard unsaved changes?")') &&
         css.includes('.btnGlyph.btnRevert{'),
    'Revert buttons must confirm, then restore the snapshot taken when the section was last hydrated clean; the Settings baseline follows the device');

  assert(codeIncludes(js, 'command(path,value={},soft,okMsg,failMsg,busyId)') &&
         codeIncludes(js, 'busyId?$(busyId):null') &&
         !codeIncludes(js, 'function busyBtn') &&
         codeIncludes(js, "classList.remove('busy')") &&
         css.includes('.btnGlyph.busy{pointer-events:none}') &&
         css.includes('.btnGlyph.busy .g:after') &&
         css.includes('@keyframes busySpin') &&
         css.includes('.btnGlyph.busy .g:after{animation:none}'),
    'Busy save buttons must spin in the glyph slot, block repeat clicks, and respect reduced motion');
  const busyCalls = [
    [adminUi, '/api/v1/config', 'saveDateTimeButton'],
    [adminUi, '/api/v1/network', 'saveNetworkButton'],
    [adminUi, '/api/v1/device/password', 'changeDevicePasswordButton']];
  for (const [source, endpoint, id] of busyCalls)
    assert(new RegExp(`command\\(\\s*['"]${endpoint}[\\s\\S]*?['"]${id}['"]`).test(source),
        `saving via ${endpoint} must mark ${id} busy`);
  assert(codeIncludes(adminUi, "$('saveWebhookButton').classList.add('busy')") &&
         // The in-flight flag also stops a keyboard repeat from saving twice.
         codeIncludes(adminUi, "finally{webhookSaving=false;$('saveWebhookButton').classList.remove('busy')}") &&
         codeIncludes(adminUi, 'if(webhookSaving)return;'),
    'Webhook saving must keep its dedicated busy feedback');
  // Admin sections saved whole carry the revision their form was filled from
  // (several screens through the companion): never the latest one, never one
  // the browser made up, and a refused save shows the saved values again.
  for (const [fill, send, clean] of [
    ['formRevisions.dateTime=c.revision', '{...payload,baseRevision:formRevisions.dateTime}',
      "formStale(e,()=>sectionDirty('dateTime',false))"],
    ['formRevisions.scheduledRestart=c.revision', 'baseRevision:formRevisions.scheduledRestart,',
      "formStale(e,()=>sectionDirty('scheduledRestart',false))"],
    ['formRevisions.webhooks=w.revision', 'baseRevision:formRevisions.webhooks,',
      'formStale(e,()=>(webhookDirty=false))'],
    ['formRevisions.network=n.revision', 'baseRevision:formRevisions.network,', ''],
  ])
    assert(codeIncludes(adminUi, fill) && codeIncludes(adminUi, send) && codeIncludes(adminUi, clean),
        'Admin form revision: ' + send);
  assert(!codeIncludes(adminUi, 'withBaseRev({...payload})') &&
         !codeIncludes(adminUi, 'withBaseRev({scheduledRestartEnabled') &&
         codeIncludes(blockAt(adminUi, 'function loadNetworkAddress('), 'formRevisions.network=n.revision') &&
         codeIncludes(adminUi, "elseif($('saveNetworkButton').dataset.dirty!=='1')R.resetNetworkAddressLoaded()"),
    'Admin forms save against their own revision, and a Wi-Fi sleep save reloads a clean network form');
  {
    const run = vm.createContext({R: {message: (text, kind) => { run.shown = [text, kind]; }},
      __WEBUI_TEXT__: (key) => key});
    vm.runInContext(blockAt(rawViewJs.admin, 'function formStale('), run);
    let cleaned = 0;
    assert.equal(run.formStale({status: 409, code: 'CONFIG_REVISION_STALE'}, () => cleaned++), true);
    assert.deepEqual([cleaned, ...run.shown], [1, 'admin.changed_elsewhere', 'error']);
    assert.equal(run.formStale({status: 409, code: 'UI_TAKEN_OVER'}, () => cleaned++), false);
    assert.equal(run.formStale({status: 400, code: 'INVALID_FIELD'}, () => cleaned++), false);
    assert.equal(cleaned, 1, 'only a stale revision drops the edits');
  }
}

// Execute the Settings autosave with the preferred-scale choice: only the
// changed keys travel, a pause coalesces edits, failures stay pending, edits
// made during a save follow as one trailing patch, and a stale revision
// resends the same patch once.
{
  const assert = require('assert').strict, vm = require('vm');
  const run = async () => {
    const a = 'AA:BB:CC:DD:EE:01', b = 'AA:BB:CC:DD:EE:02';
    let rebuilds = 0;
    const markup = options => JSON.stringify(options.map(o => [o.value, o.textContent, o.dataset.name]));
    const sel = {id: 'preferredScaleSelect', dataset: {}, options: [], selected: '',
      get innerHTML() {return markup(this.options);},
      set innerHTML(html) {
        rebuilds++; this.selected = '';
        this.options = JSON.parse(html).map(([value, textContent, name]) => ({value, textContent, dataset: {name}}));
      },
      set value(v) {this.selected = this.options.some(o => o.value === v) ? v : '';},
      get value() {return this.selected;},
      get selectedOptions() {return this.options.filter(o => o.value === this.value);}};
    const errors = [], requests = [], failures = [];
    const machine = {scaleMacCacheMode: 'only', autoTare: true};
    const bullseye = {bullseyeMelodyEnabled: false, bullseyeRtttl: ''};
    let applied = a, editDuringSave = null;
    const context = vm.createContext({$: id => id === sel.id ? sel : null,
      document: {activeElement: null, createElement: tag => tag === 'select'
        ? {options: [], appendChild(o) {this.options.push(o);}, get innerHTML() {return markup(this.options);}}
        : {dataset: {}},
        querySelectorAll: () => [sel]},
      controlsMutable: true, __LINEA_MICRA__: false,
      settingsSectionOf: () => 'config', updateScalePreferenceOptions() {},
      updateScaleRenameUi: (_, mac) => {context.renameMac = mac;},
      validateMachineClient: () => null, validateBullseyeClient: () => null,
      machinePayload: () => ({...machine, preferredScaleMac: sel.value}),
      addBullseyePayload: p => Object.assign(p, bullseye), showFieldError: id => errors.push(id),
      brewPayload: () => ({}), validateBrewClient: () => [],
      withBaseRev: p => ({...p, baseRevision: 7}),
      homeStatusCache: () => ({config: {revision: 8}, scale: {preferredMac: applied}}),
      formatCommandError: (_, e) => e.message, message: e => errors.push(e),
      setTimeout: () => 0, clearTimeout() {},
      refreshStatus: async () => context.updatePreferredScaleSelect(status()),
      command: async (path, payload) => {
        requests.push({path, payload});
        const code = failures.shift();
        if (code) throw Object.assign(new Error('rejected'), {code});
        if ('preferredScaleMac' in payload) applied = payload.preferredScaleMac;
        if (editDuringSave) editDuringSave = editDuringSave();
        await context.refreshStatus();
        return true;
      }});
    const status = (history = [a, b]) => ({config: {scaleMacCacheMode: 'only'},
      scale: {preferredMac: applied, history: history.map(mac => ({mac, name: mac}))}});
    const take = (start, end) => runtimeJs.slice(runtimeJs.indexOf(start), runtimeJs.indexOf(end));
    const settingsTake = (start, end) => viewJs.settings.slice(
        viewJs.settings.indexOf(start), viewJs.settings.indexOf(end));
    vm.runInContext([
      runtimeStateGlobals,
      take('function settingsSectionEls(', 'function updateScalePreferenceOptions('),
      settingsTake('function scaleHistoryLabel(', 'function brewWarningList('),
      blockAt(viewJs.settings, 'function selectPreferredScale('),
      blockAt(viewJs.settings, 'async function saveMachineConfig('),
      take('const SAVE_GAP_MS', 'const QUICK_GUARDS'),
      settingsTake('const autosave = {', 'let ready = false;'),
    ].join('\n'), context);
    const autosave = vm.runInContext('autosave', context);
    // applySettingsStatus takes the shown values as the next edit's base.
    const settle = () => vm.runInContext('autosave.config.last = autosave.config.payload()', context);
    const edit = () => context.noteEdit(autosave.config, 300);
    const flush = () => context.flushEdits(autosave.config, true);
    context.updatePreferredScaleSelect(status()); settle();
    context.updatePreferredScaleSelect(status());
    assert.equal(rebuilds, 1, 'An unchanged scale list is not rebuilt, so an open list stays open');
    sel.value = b; context.selectPreferredScale(); edit();
    assert.equal(requests.length, 0, 'An edit waits for its pause before saving');
    assert(context.configDirty); assert.equal(context.renameMac, b);
    context.updatePreferredScaleSelect(status([a]));
    assert.equal(sel.value, b, 'Polling and disappearing history preserve the pending choice');
    failures.push('INVALID_FIELD');
    assert.equal(await flush(), false);
    assert.equal(sel.value, b); assert(context.configDirty, 'A rejected patch stays pending');
    assert.equal(errors.at(-1), 'rejected');
    assert.equal(await flush(), true);
    assert.deepEqual(requests.at(-1), {path: '/api/v1/config',
      payload: {preferredScaleMac: b, baseRevision: 7}}, 'Only the changed setting travels');
    assert(!context.configDirty); assert.equal(sel.dataset.pending, '0');
    settle();
    machine.autoTare = false; edit(); machine.scaleMacCacheMode = 'prefer'; edit();
    await flush();
    assert.deepEqual(requests.at(-1).payload, {autoTare: false, scaleMacCacheMode: 'prefer',
      baseRevision: 7}, 'Edits within one pause coalesce into one patch of only those keys');
    settle();
    machine.autoTare = true; edit(); machine.autoTare = false; edit();
    let sent = requests.length;
    assert.equal(await flush(), true);
    assert.equal(requests.length, sent, 'A setting edited back to its saved value is not sent');
    machine.autoTare = true; edit();
    sent = requests.length;
    assert.equal(await context.flushEdits(autosave.config), false);
    assert.equal(requests.length, sent, 'Saves of one section stay SAVE_GAP_MS apart');
    editDuringSave = () => {machine.autoTare = false; edit();};
    assert.equal(await flush(), true);
    assert.deepEqual(requests.slice(-2).map(r => r.payload.autoTare), [true, false],
        'An edit made during a save follows as one trailing patch');
    settle();
    machine.autoTare = true; edit(); failures.push('CONFIG_REVISION_STALE');
    assert.equal(await flush(), true);
    assert.deepEqual(requests.slice(-2).map(r => r.payload), [{autoTare: true, baseRevision: 7},
      {autoTare: true, baseRevision: 7}], 'A stale revision resends the same patch once');
    settle();
    sel.value = ''; context.selectPreferredScale(); edit(); await flush();
    assert.equal(requests.at(-1).payload.preferredScaleMac, '', 'Explicit empty selection clears preference');
    settle();
    bullseye.bullseyeMelodyEnabled = true; edit();
    sent = requests.length;
    assert.equal(await flush(), true);
    assert.equal(requests.length, sent, 'Bullseye on without a tune waits quietly for its tune');
    bullseye.bullseyeRtttl = 'done:d=8,o=5,b=120:c,e,g'; edit(); await flush();
    assert.deepEqual(requests.at(-1).payload, {bullseyeMelodyEnabled: true,
      bullseyeRtttl: 'done:d=8,o=5,b=120:c,e,g', baseRevision: 7}, 'The toggle and its tune save together');
    settle();
    bullseye.bullseyeRtttl = 'not a tune'; edit(); failures.push('INVALID_FIELD');
    assert.equal(await flush(), false);
    assert.equal(errors.at(-1), 'bullseyeRtttl', 'A rejected tune is marked on its field, not saved');
  };
  run().catch(error => {console.error(error); process.exitCode = 1;});
}
// Execute the header Quick Settings autosave: a switch shows its choice at
// once, a pause sends only the switches that changed as one patch (one
// changed switch alone), a switch toggled back sends nothing, saves stay
// SAVE_GAP_MS apart with a trailing patch, a stale revision resends once, the
// duration goes alone, a failure shows the live values, a preset change or a
// cycle drops pending choices, and Reset now cancels them.
{
  const assert = require('assert').strict, vm = require('vm');
  const run = async () => {
    const requests = [], failures = [], messages = [];
    const live = {brewByWeight: true, noScaleBbwMode: 'warn_once', autoToManualGuardEnabled: true,
      slowExtractionGuardEnabled: true, fastExtractionGuardEnabled: true,
      avoidAccidentalTouchEnabled: true, cupProtectionEnabled: true, revision: 7};
    let after = 'keep', editDuringSave = null;
    const status = (extra = {}) => ({config: {...live}, quickRevert: {after, overrides: 0}, ...extra});
    const els = {};
    const context = vm.createContext({
      $: id => els[id] ??= {dataset: {}, classList: {toggle() {}}, setAttribute() {}, querySelectorAll: () => []},
      controlsMutable: true, configRevision: 7, presetState: {activeId: 1},
      message: m => messages.push(m), setTimeout: () => 0, clearTimeout() {},
      refreshStatus: async () => {context.configRevision = live.revision;},
      command: async (path, payload) => {
        assert.equal(path, '/api/v1/integration/quick-settings');
        requests.push({...payload});
        const code = failures.shift();
        if (code) throw Object.assign(new Error('rejected'), {code});
        if (editDuringSave) [editDuringSave] = [null, editDuringSave()];
        const {baseRevision, revertAfter, revertNow, ...fields} = payload;
        if (Object.keys(fields).length) Object.assign(live, fields, {revision: live.revision + 1});
        if (revertAfter) after = revertAfter;
        // The stream frame that reports the result carries the new revision.
        context.configRevision = live.revision;
        context.updateQuickSettings(status());
        return true;
      }});
    const take = (start, end) => runtimeJs.slice(runtimeJs.indexOf(start), runtimeJs.indexOf(end));
    vm.runInContext(runtimeStateGlobals + take('const SAVE_GAP_MS', 'function initHeaderSignals('), context);
    const quick = vm.runInContext('quickAutosave', context);
    const shown = k => quick.payload()[k];
    const edit = (k, v) => context.editQuickSetting(k, v);
    const flush = () => context.flushEdits(quick, true);
    context.updateQuickSettings(status());

    edit('cupProtectionEnabled', false);
    assert.equal(shown('cupProtectionEnabled'), false, 'A switch shows its choice at once');
    assert(context.quickDirty); assert.equal(requests.length, 0, 'A choice waits for its pause');
    context.updateQuickSettings(status());
    assert.equal(shown('cupProtectionEnabled'), false, 'A stream frame keeps a pending choice');
    edit('cupProtectionEnabled', true);
    assert.equal(await flush(), true);
    assert.equal(requests.length, 0, 'A switch toggled back to its live value is not sent');

    edit('cupProtectionEnabled', false); edit('avoidAccidentalTouchEnabled', false);
    edit('autoToManualGuardEnabled', false);
    assert.equal(await flush(), true);
    assert.deepEqual(requests, [{baseRevision: 7, cupProtectionEnabled: false,
      avoidAccidentalTouchEnabled: false, autoToManualGuardEnabled: false}],
      'Choices within one pause travel as one patch of only those switches');
    assert(!context.quickDirty);

    edit('fastExtractionGuardEnabled', false);
    assert.equal(await context.flushEdits(quick), false);
    assert.equal(requests.length, 1, 'Saves stay SAVE_GAP_MS apart');
    editDuringSave = () => edit('slowExtractionGuardEnabled', false);
    assert.equal(await flush(), true);
    assert.deepEqual(requests.slice(1), [{baseRevision: 8, fastExtractionGuardEnabled: false},
      {baseRevision: 9, slowExtractionGuardEnabled: false}],
      'One changed switch travels alone; a choice made during a save follows as a trailing patch');

    edit('brewByWeight', false); failures.push('STALE_REVISION'); context.configRevision = 3;
    assert.equal(await flush(), true);
    assert.deepEqual(requests.slice(-2), [{baseRevision: 3, brewByWeight: false},
      {baseRevision: 10, brewByWeight: false}], 'A stale revision resends the same patch once, refreshed');

    edit('revertAfter', '15m'); edit('noScaleBbwMode', 'off');
    assert.equal(await flush(), true);
    assert.deepEqual(requests.slice(-2), [{baseRevision: 11, noScaleBbwMode: 'off'},
      {baseRevision: 12, revertAfter: '15m'}], 'The duration goes alone after the settings patch');

    edit('cupProtectionEnabled', true); failures.push('INVALID_REQUEST');
    assert.equal(await flush(), false);
    assert.equal(messages.at(-1), 'rejected');
    assert.equal(shown('cupProtectionEnabled'), false, 'A failed patch shows the live value again');
    assert(!context.quickDirty);

    let sent = requests.length;
    edit('cupProtectionEnabled', true); context.presetState.activeId = 2;
    context.updateQuickSettings(status());
    edit('avoidAccidentalTouchEnabled', true);
    context.updateQuickSettings(status({cycle: {active: true}}));
    assert.equal(await flush(), true);
    assert.equal(requests.length, sent, 'A preset change or a cycle drops pending choices');
    assert.equal(shown('cupProtectionEnabled'), false);

    edit('cupProtectionEnabled', true);
    await context.revertQuickSettingsNow();
    assert.equal(await flush(), true);
    assert.deepEqual(requests.slice(sent), [{baseRevision: 12, revertNow: true}],
      'Reset now cancels pending choices and goes out alone');
  };
  run().catch(error => {console.error(error); process.exitCode = 1;});
}
// Admin switches take their saved values before the clean baselines are
// snapshotted; "Use saved setting" sends the saved value, not a draft; a
// stored paddle reminder the list lacks is shown as is without dirtying.
{
  const admin = compactSource(viewJs.admin);
  if (admin.indexOf('el.checked=v') < 0 ||
      admin.indexOf('el.checked=v') > admin.indexOf('if(!powerDirty)powerBaseline=snapshotControls(') ||
      admin.indexOf('el.checked=v') > admin.indexOf('if(!frontendDirty)frontendBaseline=snapshotControls(')) {
    throw new Error('Admin power and interface switches must be applied before their baselines');
  }
  const settings = compactSource(viewJs.settings);
  if (!settings.includes('[input.id]:saved??(input.type===checkbox?input.checked:input.value)') ||
      !settings.includes('saved=formBaseline?.[input.id]')) {
    throw new Error('Use saved setting must send the saved baseline value');
  }
  const reminder = blockAt(viewJs.settings, 'export function applyStatus(');
  if (!codeIncludes(reminder, 'reminder.add(new Option(v,v))') || reminder.includes('markConfigDirty()')) {
    throw new Error('A stored reminder interval must display unchanged without marking the form edited');
  }
}
// One shared #workflowPanel listener autosaves sections and refreshes the
// groups; per-field duplicates would run every handler twice. Static panels
// live in the partials instead of JS-built markup strings.
{
  if (/\$\("(autoTare|autoRetare|noScaleBbwMode|soundAlertsEnabled|alertOutputChannel|bullseyeMelodyEnabled|fastExtractionGuardEnabled|slowExtractionGuardEnabled|avoidAccidentalTouchEnabled|cupProtectionEnabled)"\)\.onchange/
      .test(rawViewJs.settings) || rawViewJs.settings.includes('$("operationalWallS").addEventListener')) {
    throw new Error('Settings fields must not repeat the shared change listener');
  }
  if (!partialHtml.admin.includes('<details id="webhookPanel" data-g="webhooks" class="cfgGroup"><summary>Webhooks</summary>') ||
      !partialHtml.admin.includes('<input id="showDiagnosticPage" type="checkbox" role="switch">') ||
      !partialHtml.settings.includes('<a id="bullseyeTestLink" class="rtttlTest" href="#">Test it</a></label>') ||
      /webhookPanel\.innerHTML|diagnosticOption|createElement\("a"\)/.test(rawViewJs.admin + rawViewJs.settings)) {
    throw new Error('Static Admin and Settings markup must live in the partials');
  }
}
// Enter in a credential or Save card clicks its primary button: each one is
// the submit button of an .enterForm that app.js keeps from submitting.
{
  for (const [view, id] of [['settings', 'lineaMicraConnectButton'], ['admin', 'loginConfirmButton'],
    ['admin', 'changeDevicePasswordButton'], ['admin', 'saveNetworkButton'], ['admin', 'saveDateTimeButton'],
    ['admin', 'saveWebhookButton'], ['admin', 'saveScheduledRestartButton'],
    ['settings', 'dripSetButton'], ['settings', 'alphaSetButton'], ['settings', 'lineaMicraSelectButton']]) {
    const html = partialHtml[view], at = html.indexOf(`<button id="${id}"`),
      form = html.lastIndexOf('<form', at), tag = html.slice(at, html.indexOf('>', at));
    if (at < 0 || !/type="submit"/.test(tag) || html.lastIndexOf('</form>', at) > form ||
        !/^<form[^>]*class="[^"]*enterForm[^"]*"[^>]*novalidate/.test(html.slice(form))) {
      throw new Error(`${id} must be the submit button of a novalidate .enterForm`);
    }
  }
  if (!codeIncludes(rawAppJsSource, 'document.addEventListener("submit", (e) => e.target.matches(".enterForm") && e.preventDefault())')) {
    throw new Error('An .enterForm must never submit or reload the page');
  }
}
