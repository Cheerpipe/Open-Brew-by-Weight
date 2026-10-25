{
  const integrationApi = fs.readFileSync(
    path.join(sketchDir, 'network', 'OpenBrewByWeightIntegrationApi.inc'), 'utf8');
  for (const route of [
    '/api/v1/integration', '/api/v1/integration/request',
    '/api/v1/integration/webhook', '/api/v1/integration/webhook/test',
    '/api/v1/integration/presets', '/api/v1/integration/presets/active',
    '/api/v1/integration/quick-settings', '/api/v1/integration/restart',
    '/api/v1/integration/shots/last',
  ]) {
    if (!integrationApi.includes(route))
      throw new Error(`Missing Home Assistant integration route: ${route}`);
  }
  // Open like the Web UI while its optional login is off; the login gate in
  // OpenBrewByWeightWebAccessHttp.inc answers first and asks for a token only
  // while that login is on.
  if (/Authorization|Bearer|pairing|management.token|requireIntegrationAuth/i.test(integrationApi) ||
      !integrationApi.includes('if (self.integrationAccessRoute(request, accessResult)) return accessResult;') ||
      codeIncludes(viewJs.admin, '/api/v1/integration/pairing/open')) {
    throw new Error('The integration API must stay open unless the optional Web UI login is on');
  }
  if (!integrationApi.includes('\\"wifiMac\\"') ||
      !integrationApi.includes('\\"bluetoothMac\\"')) {
    throw new Error('The integration snapshot must announce the WiFi and Bluetooth MAC addresses');
  }
  if (!webhookHeader.includes('bool presetChanges = false') ||
      !webhookHeader.includes('PRESETS_CHANGED') ||
      !webhookHeader.includes('QUICK_SETTINGS_CHANGED') ||
      !webhookHeader.includes('CONTROLLER_STARTED') ||
      !webhookSource.includes('"presets_changed"') ||
      !webhookSource.includes('escapeJsonString') ||
      !firmware.includes('activePresetName') ||
      !firmware.includes('pendingPresetPersistence')) {
    throw new Error('Webhook v1 must preserve preset provenance and emit persisted preset snapshots');
  }
  if (integrationApi.includes('/start') || integrationApi.includes('/stop') ||
      integrationApi.includes('relayClose') || integrationApi.includes('REMOTE_START')) {
    throw new Error('The Home Assistant API must not expose machine actuation');
  }
  if (!integrationApi.includes('parseIntegrationQuickSettingRequest(root, parsedRequest)') ||
      !integrationApi.includes('parseIntegrationRestartRequest(root)') ||
      !integrationApi.includes('rollbackIntegrationStagedRequest(self.stagedPresetRequestId_') ||
      !integrationApi.includes('STATUS_BAD_REQUEST, "INVALID_REQUEST"')) {
    throw new Error('Integration mutations must use the host-tested bounded parsers and HTTP 400 contract');
  }
  // The cards draw what the Web UI draws: the presets route is the Web UI
  // preset render and the last shot is one Stats row, with the fixtures in step.
  const statsPage = fs.readFileSync(
    path.join(sketchDir, 'network', 'OpenBrewByWeightStatsPage.inc'), 'utf8');
  const networkCpp = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightNetwork.cpp'), 'utf8');
  const fixtureJson = (name) => JSON.parse(fs.readFileSync(
    path.join(sketchDir, 'tests', 'fixtures', name), 'utf8'));
  const rowColumns = statsPage.match(/kStatsRowColumns\[\] =\s*((?:"[^"]*"\s*)+);/)[1]
    .replace(/"\s*"/g, '').replace(/"/g, '');
  const lastShot = fixtureJson('integration_last_shot.json');
  const presetItem = fixtureJson('integration_presets.json').items[0];
  if (!integrationApi.includes('buildSlimPresetsJson(lead)') ||
      !integrationApi.includes('self.formatShotStatsRow(work, latest,') ||
      !integrationApi.includes('\\"preset_rules_v1\\",\\"last_shot_curve_v1\\"') ||
      lastShot.cols !== rowColumns || lastShot.shot.length !== rowColumns.split(',').length + 1 ||
      Object.keys(presetItem).some((key) => !networkCpp.includes(`\\"${key}\\"`))) {
    throw new Error('The card data routes must reuse the Web UI preset render and Stats row');
  }
  for (const fixture of [
    'integration_snapshot.json', 'integration_presets.json', 'integration_last_shot.json',
    'webhook_end_v1.json', 'webhook_presets_changed_v1.json',
    'webhook_quick_settings_changed_v1.json',
    'webhook_controller_started_v1.json', 'webhook_ip_changed_v1.json',
  ]) {
    JSON.parse(fs.readFileSync(path.join(sketchDir, 'tests', 'fixtures', fixture), 'utf8'));
  }
}
