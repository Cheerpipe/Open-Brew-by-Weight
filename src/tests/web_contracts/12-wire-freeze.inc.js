{
  // Frozen external wire contracts: EndReason ordinals and API names, stop
  // detail strings, quick-setting names, the webhook end shape and the Stats
  // CSV columns. Change a value only deliberately, with the fixture and docs;
  // the Home Assistant integration repository tests its copy of the fixture.
  const frozen = JSON.parse(fs.readFileSync(
    path.join(sketchDir, 'tests', 'fixtures', 'wire_contracts_v1.json'), 'utf8'));
  const same = (actual, expected, what) => {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
      throw new Error(`${what} changed: ${JSON.stringify(actual)} != ${JSON.stringify(expected)}`);
  };
  const brewTypes = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrewTypes.h'), 'utf8');
  const ordinals = {};
  for (const [, name, value] of
       brewTypes.match(/enum class EndReason[^{]*\{([^}]*)\}/)[1]
         .matchAll(/([A-Z_]+)\s*=\s*(\d+)/g)) ordinals[name] = value;
  const labels = {};
  for (const [, name, api] of
       brewTypes.matchAll(/\{EndReason::([A-Z_]+),\s*"([A-Z_]+)"/g)) labels[ordinals[name]] = api;
  same(labels, frozen.endReasons, 'EndReason API names');
  same(Object.fromEntries(Object.entries(ordinals).map(([n, v]) => [v, n])),
       frozen.endReasons, 'EndReason ordinals');

  const shotLogTypes = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightShotLogTypes.h'), 'utf8');
  const detailFn = shotLogTypes.match(/shotLogStopDetailName\([^)]*\)\s*\{([\s\S]*?)\n\}/)[1];
  same([...detailFn.matchAll(/return "([a-z_]+)";/g)].map((m) => m[1])
         .filter((name) => name !== 'unknown').sort(),
       [...frozen.stopDetails].sort(),
       'Firmware stopDetail strings (retired names are never emitted)');

  const fixture = (name) => JSON.parse(fs.readFileSync(
    path.join(sketchDir, 'tests', 'fixtures', name), 'utf8'));
  const end = fixture('webhook_end_v1.json');
  same(Object.keys(end), frozen.webhookEndKeys, 'Webhook end_v1 keys');
  if (!frozen.stopDetails.includes(end.stopDetail))
    throw new Error('Webhook end_v1 fixture uses an unknown stopDetail');
  const snapshot = fixture('integration_snapshot.json');
  for (const shot of [snapshot.lastShot, snapshot.lastGoodShot]) {
    if (!frozen.stopDetails.includes(shot.stopDetail))
      throw new Error('Integration snapshot uses an unknown stopDetail');
  }
  same(Object.keys(snapshot.quickSettings).filter((k) => k !== 'revision' && k !== 'activePresetId'),
       frozen.quickSettings, 'Integration snapshot quick settings');
  same(Object.keys(fixture('webhook_quick_settings_changed_v1.json'))
         .filter((k) => frozen.quickSettings.includes(k)),
       frozen.quickSettings, 'Webhook quick_settings_changed_v1 keys');
  const webhookCpp = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightWebhook.cpp'), 'utf8');
  const integrationCpp = fs.readFileSync(
    path.join(sketchDir, 'network', 'OpenBrewByWeightIntegrationApi.inc'), 'utf8');
  for (const key of [...frozen.quickSettings, 'stopDetail']) {
    if (!webhookCpp.includes(`\\"${key}\\"`) ||
        (key !== 'stopDetail' && !integrationCpp.includes(`\\"${key}\\"`)))
      throw new Error(`Firmware no longer emits the frozen wire key ${key}`);
  }

  const statsJs = fs.readFileSync(path.join(sketchDir, 'web', 'js', 'stats.js'), 'utf8');
  const header = statsJs.match(/async function exportShotsCsv\(\)[\s\S]*?const rows = \[\s*\[([\s\S]*?)\.\.\.Array/)[1];
  same([...header.matchAll(/"([a-z0-9_ ]+)"/g)].flatMap((m) => m[1].split(" ")).filter(Boolean),
       frozen.statsCsvColumns, 'Stats CSV columns');

  // Continuous hardness wire schema: GET/renders emit a percent number (one
  // decimal) and SET parses the number, still reading the legacy
  // soft/medium/hard strings for one firmware cycle.
  const networkCpp = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightNetwork.cpp'), 'utf8');
  const configurationInc = fs.readFileSync(
      path.join(sketchDir, 'network', 'OpenBrewByWeightConfiguration.inc'), 'utf8');
  if (!networkCpp.includes('\\"fastCurveHardness\\":%.1f') ||
      !networkCpp.includes('bool jsonRampHardness(') ||
      !networkCpp.includes('rampHardnessName(i)) == 0') ||
      !configurationInc.includes('jsonRampHardness(root, "fastCurveHardness"') ||
      !configurationInc.includes('jsonRampHardness(root, "slowCurveHardness"'))
    throw new Error('Curve hardness must be a percent number on the wire with a tolerant legacy-string SET');
}
