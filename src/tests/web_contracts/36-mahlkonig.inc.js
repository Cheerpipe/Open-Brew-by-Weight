// Mahlkönig Grind-by-Sync: Settings' section (collapsed groups, the legend's
// states, the Untested badge, the cadence for the four models the controller
// must ask), the E64 WS address rule shared with Beanconqueror (D11), and
// Admin's Integrations switches with their note and the companion switch.
(async () => {
  const assert = require('assert').strict;
  const vm = require('vm');
  const settingsHtml = partialHtml.settings, adminHtml = partialHtml.admin;
  const section = settingsHtml.slice(settingsHtml.indexOf('id="mahlkonigSettings"'),
      settingsHtml.indexOf('id="coffeeAppsSettings"'));
  // Collapsed like La Marzocco: two groups, never open by default.
  assert.match(settingsHtml, /<fieldset id="mahlkonigSettings" hidden>/);
  assert.match(section, /<details data-g="mahlkonigGrinder" class="cfgGroup">/);
  assert.match(section, /<details id="gbsSync" data-g="mahlkonigSync" class="cfgGroup" hidden>/);
  assert.ok(!/<details[^>]*\bopen\b/.test(section), 'no group is forced open');
  assert.ok(section.includes('class="presetCardBadge gbsBadge">Untested</span>'), 'Untested badge');
  for (const model of ['e64ws', 'e65w', 'e65t', 'e80w', 'e80t'])
    assert.ok(section.includes(`name="gbsModel" value="${model}"`), model + ' is listed');
  assert.ok(!section.includes('name="gbsModel" value="none"'), 'off lives in Admin, not here');
  // D12: three cadences, 20 s by default, with the companion recommended.
  assert.match(section, /<option value="20" selected><\/option>/);
  for (const s of ['15', '20', '25']) assert.ok(section.includes(`name="gbsCadence" value="${s}"`));
  assert.ok(section.includes(errorCatalog['settings.gbs_cadence_tip']), 'the companion tip is shown');
  assert.ok(errorCatalog['settings.gbs_cadence_tip'].includes('every 2 seconds'));
  assert.ok(section.indexOf('id="gbsCadence"') > section.indexOf('id="gbsStatus"'),
      'the cadence belongs to the Grinder group');

  // Admin → Integrations: Beanconqueror on, Mahlkönig off, the note under it.
  const integrations = adminHtml.slice(adminHtml.indexOf('id="integrationsPanel"'),
      adminHtml.indexOf('id="webhookPanel"'));
  assert.ok(adminHtml.indexOf('id="integrationsPanel"') < adminHtml.indexOf('id="webhookPanel"'));
  assert.match(integrations, /id="integrationBeanconqueror" type="checkbox" role="switch" checked/);
  assert.match(integrations, /id="integrationMahlkonig" type="checkbox" role="switch"\s*\/?>/);
  const note = errorCatalog['admin.integration_gbs_note'];
  for (const words of ['Not tested with a real grinder', 'E65W, E65T, E80W and E80T', 'every 2 seconds',
    '15 to 25 seconds', '20 by default', 'works well without the companion', 'strongly recommended'])
    assert.ok(note.includes(words), 'the note says: ' + words);
  assert.ok(integrations.includes(note) && integrations.includes('gbsBadge'));
  assert.ok(!/\b(push|pull)\b/i.test(note + errorCatalog['admin.integration_gbs_hint']),
      'user copy describes behavior, not protocol roles');
  for (const id of ['companionGbs', 'companionApproveGbs'])
    assert.match(adminHtml, new RegExp(`id="${id}" type="checkbox" role="switch"`));

  // The section's logic, as Settings runs it.
  const source = viewJs.settings;
  const logic = source.slice(source.indexOf('const GBS_MODELS ='), source.indexOf('async function gbsAction('));
  const dom = {};
  const el = (id) => (dom[id] ||= {id, textContent: '', hidden: false, value: '', dataset: {},
    classList: {toggle() {}}, replaceChildren(...kids) { this.kids = kids; }});
  const radios = {gbsModel: [], gbsCadence: ['15', '20', '25'].map((value) => ({value}))};
  let here = null, status = {};
  const context = vm.createContext({
    $: el, location: {hostname: 'openbrew.local'}, Date, Math, String,
    document: {querySelectorAll: (q) => radios[/name=(\w+)/.exec(q)[1]], createElement: () => ({})},
    R: {controlsMutable: true, sub: (t, a) => a.reduce((s, v, i) => s.replace(`{${i}}`, v), t),
      companionHere: async () => here, settingsStatus: () => status,
      presetState: {activeId: 1, items: [{id: 1, name: 'Classic espresso', fastBeforeS: 25, slowAfterS: 35}]}},
  });
  vm.runInContext(logic + ';this.api={gbsAddress,gbsPhase,gbsLegend,renderMahlkonig};', context);
  const {gbsAddress, gbsPhase, renderMahlkonig} = context.api;

  // D11: the controller's address, unless the companion answers the grinder.
  const s = {network: {staIp: '192.168.1.42'}, companion: {gbs: true}};
  assert.deepEqual({...(await gbsAddress(s))}, {addr: '192.168.1.42'}, 'no companion');
  here = {paired: true, ip: '192.168.1.20', httpPort: 80};
  assert.equal((await gbsAddress(s)).note, errorCatalog['settings.gbs_note_off'], 'an older companion');
  here = {paired: true, ip: '192.168.1.20', httpPort: 80, gbs: true};
  assert.equal((await gbsAddress({...s, companion: {gbs: false}})).note, errorCatalog['settings.gbs_note_off'],
      'its switch off');
  const via = await gbsAddress(s);
  assert.deepEqual([via.addr, via.alt, via.note], ['192.168.1.20',
    errorCatalog['settings.gbs_cant_connect'].replace('{0}', '192.168.1.42'),
    errorCatalog['settings.gbs_note_companion']], 'the companion answers, the controller is the way back');
  here = {paired: true, ip: '192.168.1.20', httpPort: 8080, gbs: true};
  const port = await gbsAddress(s);
  assert.deepEqual([port.addr, port.note],
      ['192.168.1.42', errorCatalog['settings.gbs_note_port'].replace('{0}', 8080)], 'port 80 only');

  // The phases the legend and the groups follow.
  const phase = (config, gbs) => gbsPhase({config: {integrationMahlkonig: true, ...config}, gbs});
  assert.equal(phase({mahlkonigModel: 'none'}, {}), 'choose');
  assert.equal(phase({mahlkonigModel: 'e64ws'}, {seenAgeS: -1}), 'pull-setup');
  assert.equal(phase({mahlkonigModel: 'e64ws'}, {seenAgeS: 3}), 'synced');
  assert.equal(phase({mahlkonigModel: 'e64ws'}, {seenAgeS: 700}), 'lost');
  assert.equal(phase({mahlkonigModel: 'e64ws'}, {pull: 'armed', seenAgeS: 2}), 'armed');
  assert.equal(phase({mahlkonigModel: 'e64ws'}, {route: 'companion', companion: 'seen', seenAgeS: -1}), 'synced');
  assert.equal(phase({mahlkonigModel: 'e65w'}, {paired: false}), 'push-setup');
  assert.equal(phase({mahlkonigModel: 'e65w'}, {pairing: 'accepting', code: 73}), 'push-code');
  assert.equal(phase({mahlkonigModel: 'e65w'}, {pairing: 'need_id'}), 'push-id');
  assert.equal(phase({mahlkonigModel: 'e80t'}, {paired: true, link: 'ok'}), 'synced');
  assert.equal(phase({mahlkonigModel: 'e80t'}, {paired: true, link: 'unpaired'}), 'unpaired');
  assert.equal(phase({mahlkonigModel: 'e80t'}, {paired: true, link: 'identity'}), 'identity');
  assert.equal(phase({mahlkonigModel: 'e80t'}, {paired: true, route: 'companion', companion: 'unreach'}), 'unreach');

  // Off hides the section (and Beanconqueror off hides Coffee apps); the
  // cadence shows only for the four models the controller must ask, keeps
  // the stored choice, and gives way to the companion's 2 s.
  const render = async (config, gbs) => {
    status = {network: {staIp: '192.168.1.42'}, config: {integrationMahlkonig: true, ...config}, gbs};
    el('mahlkonigPushCadenceS').value = String(config.mahlkonigPushCadenceS ?? 20);
    await renderMahlkonig(status);
  };
  await render({integrationMahlkonig: false, integrationBeanconqueror: false}, {});
  assert.ok(dom.mahlkonigSettings.hidden && dom.coffeeAppsSettings.hidden);
  await render({mahlkonigModel: 'e65w'}, {paired: true, link: 'ok', host: '192.168.1.87', okAgeS: 4});
  assert.ok(!dom.mahlkonigSettings.hidden && !dom.gbsCadence.hidden && dom.gbsCadenceCompanion.hidden);
  assert.deepEqual(radios.gbsCadence.map((r) => [r.checked, r.disabled]), [[false, false], [true, false], [false, false]],
      'the default 20 s is chosen');
  assert.equal(dom.gbsCheckedBy.textContent, errorCatalog['settings.gbs_checked_controller'].replace('{0}', '20'));
  assert.equal(dom.gbsLegendHint.textContent, 'E65W · Synced');
  await render({mahlkonigModel: 'e65w', mahlkonigPushCadenceS: 15},
      {paired: true, link: 'ok', route: 'companion', companion: 'ok', host: '192.168.1.87'});
  assert.ok(!dom.gbsCadence.hidden && !dom.gbsCadenceCompanion.hidden);
  assert.deepEqual(radios.gbsCadence.map((r) => [r.checked, r.disabled]), [[true, true], [false, true], [false, true]],
      'through the companion the stored choice is kept and set aside');
  await render({mahlkonigModel: 'e64ws'}, {seenAgeS: 2, last: {id: 7, timeS: 27.4, g: 36, ageS: 120,
    success: true, delivered: true}});
  assert.ok(dom.gbsCadence.hidden, 'the E64 WS checks in by itself');
  assert.equal(dom.gbsDelivery.textContent, errorCatalog['settings.gbs_received']);
  assert.ok(dom.gbsBrewTime.textContent.includes('30 s'), 'the Brew Time tip is the window middle');
  await render({mahlkonigModel: 'e64ws'}, {seenAgeS: -1});
  assert.ok(!dom.gbsPullSetup.hidden && dom.gbsStatus.hidden && dom.gbsSync.hidden);
  assert.equal(dom.gbsAddr.textContent, '192.168.1.42');
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
