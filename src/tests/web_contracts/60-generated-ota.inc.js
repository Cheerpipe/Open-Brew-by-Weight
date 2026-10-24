    !firmware.includes(
        'if (shotCompletionGetsLongBeep(reason)) {\n      // Completion LONG replaces the stop-timer SINGLE so ends are one cue.\n      requestCompletionAlert();\n    } else {\n      emitCircuitCycleAlert(AlertEvent::STOP_TIMER, true);') ||
    !firmware.includes('emitImmediateCommandAlertIfBuzzer(AlertEvent::TARE);\n  markRetareEnded') ||
    !firmware.includes('emitCircuitCycleAlert(AlertEvent::START_TIMER, true);')) {
  throw new Error('Command alerts must fire at circuit/paddle/retare, not after BLE');
}
if (firmware.includes('SCALE_COMPLETION_BEEP_DELAY_MS') ||
    firmware.includes('scheduleScaleCompletionBeep') ||
    firmware.includes('scaleCompletionBeepDueAtMs') ||
    !firmware.includes('void requestCompletionAlert()')) {
  throw new Error('Completion beep must fire at machine circuit open with no emission delay');
}
if (firmware.includes('maybeCaptureScaleStartLag') ||
    firmware.includes('scaleStartLagCaptured') ||
    firmware.includes('scaleTimerStopDelayMsForCycle') ||
    firmware.includes('setAdvertisingPaused(preferBluetooth)') ||
    !firmware.includes('remoteTimerStartSettled') ||
    !firmware.includes(
        'enqueueScaleCommand(command, session.remoteTimerStartSettled)')) {
  throw new Error('Scale timer stop must catch up live, wait for start, and not BLE.advertise() on machine circuit open');
}

(async () => {
const generated = await webUi.generate();
if (!generated.modules.diagnostic.includes('new CompressionStream("gzip")') ||
    !generated.modules.diagnostic.includes('"crashes.tar"') ||
    !generated.modules.diagnostic.includes('"openbrewbyweight-"+') ||
    !generated.modules.diagnostic.includes('?".gz":""')) {
  throw new Error('Crash download must gzip in the browser with a TAR fallback');
}
const tarFixture = Buffer.alloc(1024);
tarFixture.write('ustar', 257);
const browserGzip = await new Response(
    new Blob([tarFixture]).stream().pipeThrough(new CompressionStream('gzip'))
).arrayBuffer();
if (!zlib.gunzipSync(Buffer.from(browserGzip)).equals(tarFixture)) {
  throw new Error('Browser gzip must preserve TAR bytes');
}
if (!generated.assetTag || !generated.cacheVersion ||
    !generated.html.includes(`src="/app.js?v=${generated.assetTag}"`) ||
    generated.html.includes(`v=${generated.cacheVersion}`) ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightWebAssetsGzip.h'), 'utf8')
         .includes(`WEB_UI_ASSET_TAG[] = "${generated.assetTag}"`) ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightWebAssetsGzip.h'), 'utf8')
         .includes(`WEB_UI_ETAG[] = ${JSON.stringify('"' + generated.cacheVersion + '"')}`)) {
  throw new Error('Asset URLs must carry the content tag alone; the ETag embeds FW version + tag');
}
const normalizedEnglish = await webUi.generate({webUiLanguage: 'EN', write: false});
const regionalEnglish = await webUi.generate({webUiLanguage: 'En_en', write: false});
const developmentShell = await webUi.generate({developmentMode: true, write: false});
// Shipped bytes do not depend on how the HTML and CSS sources are formatted:
// printing them at two widths changes nothing the generator emits.
{
  const prettier = require('prettier');
  const files = [webUi.sourcePath, webUi.cssSourcePath,
    ...VIEW_NAMES.map((name) => path.join(webUi.htmlDir, `${name}.html`))];
  const printed = async (printWidth) => new Map(await Promise.all(files.map(async (file) => [file,
    await prettier.format(fs.readFileSync(file, 'utf8'), {filepath: file, printWidth})])));
  const widths = await Promise.all([40, 160].map(printed));
  const shipped = (r) => [r.html, r.css, ...Object.values(r.partials), ...Object.values(r.modules)];
  for (const options of [{}, {remoteControl: false, lineaMicra: false, machineType: 'paddle'}]) {
    const [narrow, wide] = await Promise.all(widths.map((sources) => webUi.generate(
        {...options, readSource: (file) => sources.get(file), versionOverride: 'format', write: false})));
    require('assert').deepStrictEqual(shipped(narrow), shipped(wide),
        'Web UI output must not depend on how its HTML and CSS sources are formatted');
  }
}
if (!developmentShell.html.includes('<body class="devBuild">')) {
  throw new Error(
      'developmentMode generation must bake the devBuild class into the shell body');
}
if (generated.html.includes('<body class="devBuild">') ||
    generated.html.includes('devBuild')) {
  throw new Error('release shell must not carry the devBuild class');
}
// Machine-type exclusive Web UI: typed generation strips other types'
// exclusive markup and CSS, and bakes the type class into <html>. JS keeps
// ids in every variant behind null-safe guards, so only markup/CSS are pinned.
const machineTypeExclusive = {
  paddle: ['momentaryStartEdge', 'stopPulseMs', 'maxSinglePressMs',
    'reedConfirmTimeoutS', 'dReed', 'forcePulseButton'],
  momentary: ['paddleMode', 'paddleReturnReminder',
    'paddleReturnReminderMaxDurationMin',
    'dReed'],
  momentary_reed: ['paddleMode', 'paddleReturnReminder',
    'paddleReturnReminderMaxDurationMin', 'shotReactTimeoutS', 'assumeIdleWhenScaleConnects',
    'overrideIdleLink'],
};
for (const [machineType, forbidden] of Object.entries(machineTypeExclusive)) {
  const variant =
      await webUi.generate({webUiLanguage: 'EN', machineType, write: false});
  const uiText =
      [variant.html, ...Object.values(variant.partials), variant.css].join('\n');
  const leaked = forbidden.filter((id) => uiText.includes(id));
  if (leaked.length) {
    throw new Error(`machine-type ${machineType} UI must not ship ${leaked.join(',')}`);
  }
  const expectedClass =
      {paddle: 'paddleBuild', momentary: 'momentaryBuild',
       momentary_reed: 'momentaryBuild reedBuild'}[machineType];
  if (!variant.html.includes(expectedClass)) {
    throw new Error(`machine-type ${machineType} shell must carry ${expectedClass}`);
  }
  // Stripping must remove whole balanced subtrees; an unbalanced regex once
  // left stray closers that ended the diagnostic panel before Relay..Recovery.
  const machineTypePartial = variant.partials.diagnostic;
  const divOpen = (machineTypePartial.match(/<div\b/g) || []).length;
  const divClose = (machineTypePartial.match(/<\/div>/g) || []).length;
  if (divOpen !== divClose) {
    throw new Error(`machine-type ${machineType} diagnostic partial has ` +
        `unbalanced div tags (${divOpen} open / ${divClose} close)`);
  }
  const ioPanelStart = machineTypePartial.indexOf('Machine I/O');
  const scalePanelStart = machineTypePartial.indexOf('Scale', ioPanelStart);
  const ioPanel = machineTypePartial.slice(ioPanelStart, scalePanelStart);
  for (const keptId of ['dActivator', 'dRelay', 'dSafety', 'dWatchdog']) {
    if (!ioPanel.includes(keptId)) {
      throw new Error(`machine-type ${machineType} diagnostic Machine I/O ` +
          `panel must keep ${keptId}`);
    }
  }
  if (ioPanel.includes('</fieldset></div>')) {
    throw new Error(`machine-type ${machineType} diagnostic Machine I/O ` +
        'panel must not close the diagnostics container early');
  }
  // Saving shared settings reads only fields the typed build still ships.
  const shippedIds = new Set([...variant.partials.settings.matchAll(/\bid="([^"]+)"/g)]
      .map((m) => m[1]));
  const field = (id) => (shippedIds.has(id) ? {value: '1', checked: false} : null);
  new Function('$', 'R', blockAt(viewJs.settings, 'function machinePayload(') +
      ';return machinePayload();')(field, {number: (id) => Number(field(id).value),
    sToMs: (id) => Number(field(id).value) * 1000, extRate: (v) => v});
  // Save validation reads only shipped fields too.
  const typeClasses = {paddle: [], momentary: ['momentaryMachine'],
    momentary_reed: ['momentaryMachine', 'reedMachine']}[machineType];
  new Function('$', 'R', 'document', blockAt(viewJs.settings, 'function validateMachineClient(') +
      ';return validateMachineClient();')(field, {rangeCheck: (id) => (field(id).value, ''),
    number: (id) => Number(field(id).value), formNumber: () => 1},
  {documentElement: {classList: {contains: (c) => typeClasses.includes(c)}}});
}
// Typed CSS drops only the selectors naming stripped markup, never the
// rules grouped with them.
for (const [machineType, remoteControl, kept] of [
  ['momentary_reed', true, ['html:not(.reedMachine) .reedOnly']],
  ['momentary', false, ['html:not(.momentaryMachine) .switchOnly']],
  ['paddle', false, ['html.momentaryMachine .paddleOnly',
    'html.theme-dark .qwSheet{box-shadow:0 -8px']]]) {
  const {css: typedCss} = await webUi.generate({machineType, remoteControl, write: false});
  const lost = kept.filter((needle) => !typedCss.includes(needle));
  if (lost.length) {
    throw new Error(`machine-type ${machineType} CSS lost grouped rules: ${lost.join(', ')}`);
  }
}
const typeAgnostic = await webUi.generate({webUiLanguage: 'EN', write: false});
const typeAgnosticText =
    [typeAgnostic.html, ...Object.values(typeAgnostic.partials),
     typeAgnostic.css].join('\n');
for (const id of [...new Set(Object.values(machineTypeExclusive).flat())]) {
  if (!typeAgnosticText.includes(id)) {
    throw new Error(`type-agnostic UI must keep every type's markup (${id})`);
  }
}
// Remote machine control is compile-time opt-in; builds without the flag must
// ship neither the fixed Home action bar nor the runtime helper driving it,
// while remote builds keep every piece.
for (const remoteControl of [false, true]) {
  const variant =
      await webUi.generate({webUiLanguage: 'EN', remoteControl, write: false});
  const uiText =
      [variant.html, ...Object.values(variant.partials), variant.css].join('\n');
  const forbidden = ['id="actionsPanel"', '#actionsPanel', 'homeAdminActions'];
  if (!/\.configSaveBar\{[^}]*bottom:var\(--nav-offset\)/.test(variant.css) ||
      !variant.css.includes('--nav-offset:calc(4.25rem + env(safe-area-inset-bottom))') ||
      !variant.css.includes('--nav-offset:0px')) {
    throw new Error('Every build must move save controls above bottom navigation and reset their offset for header navigation');
  }
  const leaked = forbidden.filter((n) => uiText.includes(n));
  if (remoteControl === false && leaked.length) {
    throw new Error(`no-remote-control UI must not ship ${leaked.join(',')}`);
  }
  if (remoteControl === true &&
      forbidden.filter((n) => !uiText.includes(n)).length) {
    throw new Error('remote-control UI must ship the Home action bar (markup and CSS)');
  }
  if (remoteControl === false &&
      variant.runtimeJs.length >=
      (await webUi.generate({webUiLanguage: 'EN', write: false})).runtimeJs.length) {
    throw new Error('no-remote-control runtime must drop the action-bar helper');
  }
}
// Only Linea Micra cloud builds ship the Micra status and diagnostics code.
const noMicra = await webUi.generate({webUiLanguage: 'EN', lineaMicra: false, write: false});
// The Diagnostic module ships the Micra markup in every build; its renderer folds away.
if (!codeIncludes(generated.modules.diagnostic, 'freshnessPolicy') ||
    codeIncludes(noMicra.modules.diagnostic, 'freshnessPolicy') ||
    /__LINEA_MICRA__|__REMOTE_CONTROL__/.test(noMicra.runtimeJs + generated.runtimeJs +
        noMicra.modules.diagnostic + generated.modules.diagnostic)) {
  throw new Error('Linea Micra code must fold away in builds without the integration');
}
// Builds without the integration also drop its markup and CSS; nested
// Micra rows go with their section, and the JS that binds them folds away.
{
  const noMicraUi = [noMicra.html, ...Object.values(noMicra.partials), noMicra.css].join('\n');
  const micraIds = ['lineaMicraSettings', 'lineaMicraConnectButton', 'lineaMicraBrewTargetC',
    'micraMachineDiagnostics', 'micraCloudDiagnostics', 'dMicraPowerBrief', 'homeMicraPower'];
  const leaked = [...micraIds.filter((id) => noMicraUi.includes(`id="${id}"`)),
    ...['.micraOnly', '.micraTempOpt'].filter((c) => noMicraUi.includes(c))];
  const missing = micraIds.filter((id) => ![generated.html, ...Object.values(generated.partials)]
      .join('\n').includes(`id="${id}"`));
  const diag = noMicra.partials.diagnostic;
  if (leaked.length || missing.length ||
      (diag.match(/<div\b/g) || []).length !== (diag.match(/<\/div>/g) || []).length ||
      (diag.match(/<fieldset\b/g) || []).length !== (diag.match(/<\/fieldset>/g) || []).length ||
      !diag.includes('id="hFirmware"') ||
      noMicra.modules.settings.includes('lineaMicraConnectButton') ||
      noMicra.js.includes('homeMicraPower')) {
    throw new Error(`Non-Micra builds must strip Micra UI (leaked ${leaked}, missing ${missing})`);
  }
}
localeAssert.equal(normalizedEnglish.resolvedLanguage, 'en');
localeAssert.equal(regionalEnglish.requestedLanguage, 'en-en');
localeAssert.equal(regionalEnglish.resolvedLanguage, 'en');
localeAssert.equal(normalizedEnglish.assetTag, generated.assetTag);
localeAssert.equal(regionalEnglish.assetTag, generated.assetTag);
localeAssert.deepEqual(normalizedEnglish.gzip, generated.gzip);

const metadataFixture = makeTestLocales((catalogs) => {
  catalogs.en.language = 'English test metadata';
});
try {
  const metadataOnly = await webUi.generate({
    webUiLanguage: 'en', localesDir: metadataFixture.dir, write: false,
  });
  localeAssert.equal(metadataOnly.assetTag, generated.assetTag);
} finally {
  metadataFixture.clean();
}

const changedFixture = makeTestLocales((catalogs) => {
  const key = Object.keys(catalogs.en.strings)[0];
  catalogs.en.strings[key] += ' changed';
});
try {
  const changed = await webUi.generate({
    webUiLanguage: 'en', localesDir: changedFixture.dir, write: false,
  });
  localeAssert.notEqual(changed.assetTag, generated.assetTag);
} finally {
  changedFixture.clean();
}

const generatedHeaderPath = path.join(sketchDir, 'OpenBrewByWeightWebAssetsGzip.h');
const generatedHeader = fs.readFileSync(generatedHeaderPath, 'utf8');
for (const forbidden of ['__WEBUI_', '{{webui:', 'schemaVersion', 'shell.open_brew']) {
  localeAssert.ok(!generatedHeader.includes(forbidden),
      `Generated firmware assets must not embed localization token ${forbidden}`);
}
await localeAssert.rejects(
    webUi.generate({webUiLanguage: 'es-CL'}), /no catalog for es-cl/);
localeAssert.equal(fs.readFileSync(generatedHeaderPath, 'utf8'), generatedHeader,
    'Rejected locale must not replace the generated header');
const escapedLocaleValue = `quote " apostrophe ' slash \\ newline\nUnicode café`;
const escapedLocaleJs = await webUi.minifyJs(
    `globalThis.__webUiLocaleTest=${webUiLocale.jsLiteral(escapedLocaleValue)}`);
new Function(escapedLocaleJs)();
localeAssert.equal(globalThis.__webUiLocaleTest, escapedLocaleValue);
delete globalThis.__webUiLocaleTest;
localeAssert.doesNotThrow(() => webUi.minifyCss(
    `x{content:${webUiLocale.cssLiteral(escapedLocaleValue)}}`));
const roundTrip = zlib.gunzipSync(generated.gzip).toString('utf8');
if (roundTrip !== generated.html) {
  throw new Error('Generated gzip Web UI does not round-trip to the minified HTML');
}
if (!generated.html.includes('id="view-home"') ||
    !generated.html.includes('id="stopButton"') ||
    !generated.html.includes('id="forcePulseButton"') ||
    !generated.html.includes('Start shot') ||
    generated.html.includes('virtualPaddle') ||
    generated.html.includes('<section id="view-home" class="view" data-view="home"></section>')) {
  throw new Error('Generated shell must embed home partial markup');
}
const jsRoundTrip = zlib.gunzipSync(generated.jsGzip).toString('utf8');
if (jsRoundTrip !== generated.js) {
  throw new Error('Generated gzip Web JS does not round-trip to the minified JS');
}
if (!codeIncludes(generated.js, 'import') ||
    !codeIncludes(generated.runtimeJs, 'export') ||
    !codeIncludes(generated.js, '/api/v1/control/paddle') ||
    codeIncludes(generated.js, '/js/home.js') ||
    !generated.modules.records.includes('export') ||
    !generated.modules.records.includes('views') ||
    !codeIncludes(appJsSource, '__homeModule') ||
    !codeIncludes(appJsSource, "BUNDLES={stats:'records',history:'records'}") ||
    codeIncludes(appJsSource, '/js/secondary.js')) {
  throw new Error('Generated Web UI JS must remain ES modules (shell + runtime + view modules)');
}
const runtimeRoundTrip = zlib.gunzipSync(generated.runtimeGzip).toString('utf8');
if (runtimeRoundTrip !== generated.runtimeJs) {
  throw new Error('Generated gzip runtime JS does not round-trip');
}
// The runtime bakes the firmware build id (VERSION + short commit SHA), which
// changes on every commit and shifts the compressed size without any UI change;
// the per-asset budget would then chase irrelevant SHA noise. Measure a
// sentinel-stamped generation instead; the combined 108200-byte flash cap still
// measures the real (version-baked) assets below.
const sentinel =
    await webUi.generate({write: false, versionOverride: '00000000'});
if (sentinel.version !== '00000000') {
  throw new Error('Sentinel runtime generation did not apply the fixed build id');
}
const sentinelRuntimeGzip = sentinel.runtimeGzip;
const otaImageRoundTrip =
    zlib.gunzipSync(generated.otaImageGzip).toString('utf8');
if (otaImageRoundTrip !== generated.otaImageJs) {
  throw new Error('Generated gzip OTA image module does not round-trip');
}
const makeOtaParser = new Function(
    otaImageJs.replace(/^['"]use strict['"];\s*/m, '')
        .replace(/export\s+/g, '') +
    '; return {otaFileIdentity,imageTagScanner,hash256};');
const otaParser = makeOtaParser();
const otaDigest = otaParser.hash256();
otaDigest.add(new TextEncoder().encode('a'));
otaDigest.add(new TextEncoder().encode('bc'));
if (otaDigest.end() !==
    'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') {
  throw new Error('Web UI incremental OTA SHA-256 is incorrect');
}
// Block-wise hashing and fork() must match a one-shot digest at every split.
for (const size of [0, 1, 55, 56, 63, 64, 65, 129, 4096, 70001]) {
  const bytes = crypto.randomBytes(size), want = crypto.createHash('sha256').update(bytes).digest('hex');
  for (const split of [0, 1, 63, 64, 65, size >> 1]) {
    const at = Math.min(split, size), head = otaParser.hash256();
    head.add(bytes.subarray(0, at));
    const fork = head.fork();
    head.add(bytes.subarray(at));
    fork.add(bytes.subarray(at));
    if (head.end() !== want || fork.end() !== want) {
      throw new Error(`Web UI OTA SHA-256 differs at size ${size}, split ${split}`);
    }
  }
}
function browserFile(buffer) {
  return {
    size: buffer.length,
    slice(start, end) {
      const bytes = buffer.subarray(start, end);
      return {arrayBuffer: async () =>
        bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)};
    },
  };
}
function otaFixture(tagOffset) {
  const tag = Buffer.from(
      'OPENBREWBYWEIGHT_FW_TAG_V2|arch=n16r8|hw=legacy|machine=legacy|ver=1.2.3+abcdef0|' +
      'packed=16909056|END', 'latin1');
  const size = Math.max(tagOffset + tag.length + 64, 4096);
  const image = Buffer.alloc(size, 0x5a);
  image[0] = 0xe9;
  image[12] = 0x09;
  image[13] = 0;
  image[23] = 1;
  image.writeUInt32LE(0xabcd5432, 32);
  image.fill(0, 80, 112);
  image.write('openbrewbyweight', 80, 'latin1');
  tag.copy(image, tagOffset);
  refreshOtaFixtureHash(image);
  return image;
}
function refreshOtaFixtureHash(image) {
  crypto.createHash('sha256').update(image.subarray(0, image.length - 32))
      .digest().copy(image, image.length - 32);
}
for (const tagOffset of [320, 65530, 262143, 262144, 270344]) {
  const fixture = otaFixture(tagOffset);
  const parsed = await otaParser.otaFileIdentity(browserFile(fixture));
  const nodeParsed = imageTag.findImageTag(fixture);
  if (parsed.tagOffset !== tagOffset || parsed.arch !== 'n16r8' ||
      parsed.version !== '1.2.3+abcdef0' || parsed.packed !== 16909056 ||
      !nodeParsed || nodeParsed.arch !== parsed.arch ||
      nodeParsed.ver !== parsed.version || Number(nodeParsed.packed) !== parsed.packed) {
    throw new Error(`Web/Node OTA identity mismatch at offset ${tagOffset}`);
  }
}
for (const badBody of [
  'arch=N16R8|arch=n16r8|hw=legacy|machine=legacy|ver=1.2.3|packed=1|END',
  'ignored=\0|arch=n16r8|hw=legacy|machine=legacy|ver=1.2.3|packed=1|END',
]) {
  const bad = Buffer.from('OPENBREWBYWEIGHT_FW_TAG_V2|' + badBody, 'latin1');
  const validOffset = 320 + bad.length + 8;
  const fixture = otaFixture(validOffset);
  bad.copy(fixture, 320);
  refreshOtaFixtureHash(fixture);
  const parsed = await otaParser.otaFileIdentity(browserFile(fixture));
  const nodeParsed = imageTag.findImageTag(fixture);
  if (parsed.tagOffset !== validOffset || !nodeParsed ||
      nodeParsed.tagOffset !== validOffset) {
    throw new Error('Web/Node parsers accepted a candidate rejected by C++');
  }
}
{
  const corrupt = otaFixture(270344);
  corrupt[1000] ^= 1;
  let rejected = false;
  try {
    await otaParser.otaFileIdentity(browserFile(corrupt));
  } catch (error) {
    rejected = /truncated or corrupt/.test(String(error && error.message));
  }
  if (!rejected) {
    throw new Error('Web OTA parser must reject a corrupt appended image hash');
  }
}
// Foreign, look-alike and pre-v1 Arduino project names are rejected by the
// Web UI and host tool.
for (const project of ['some-other-project', 'openbrewbyweightx', 'arduino-lib-builder']) {
  const foreign = otaFixture(320);
  foreign.fill(0, 80, 112);
  foreign.write(project, 80, 'latin1');
  refreshOtaFixtureHash(foreign);
  const dir = fs.mkdtempSync(path.join(require('os').tmpdir(), 'openbrewbyweight-ota-'));
  fs.writeFileSync(path.join(dir, 'image.bin'), foreign);
  const hostProblems = imageTag.inspectImage(path.join(dir, 'image.bin')).problems;
  fs.rmSync(dir, {recursive: true});
  let rejected = false;
  try {
    await otaParser.otaFileIdentity(browserFile(foreign));
  } catch (error) {
    rejected = /does not belong/.test(String(error && error.message));
  }
  if (!rejected || !hostProblems.some((problem) => problem.startsWith('unexpected project_name'))) {
    throw new Error(`OTA parsers must reject project name ${project}`);
  }
}
// An image carrying only a different tag version has no identity.
{
  const older = otaFixture(320);
  older.fill(0x5a, 320, 520);
  older.write('OPENBREWBYWEIGHT_FW_TAG_V1|arch=n16r8|hw=legacy|machine=legacy|ver=1.2.3|packed=1|END',
      320, 'latin1');
  refreshOtaFixtureHash(older);
  let rejected = false;
  try {
    await otaParser.otaFileIdentity(browserFile(older));
  } catch (error) {
    rejected = /does not contain a valid/.test(String(error && error.message));
  }
  if (!rejected || imageTag.findImageTag(older)) {
    throw new Error('OTA parsers must not accept a different image tag version');
  }
}
// Every WEB_ASSETS row round-trips to the bytes it names; PNG rows ship raw
// because the artwork is already compressed.
for (const asset of generated.assets) {
  const served = asset.gzip ? zlib.gunzipSync(asset.body) : asset.body;
  if (!served.equals(asset.raw) || asset.gzip !== (asset.contentType !== 'image/png')) {
    throw new Error('Generated asset does not round-trip: ' + asset.path);
  }
}
for (const [file, js] of Object.entries(generated.modules)) {
  if (zlib.gunzipSync(generated.moduleGzip[file]).toString('utf8') !== js) {
    throw new Error('Generated gzip view module does not round-trip: ' + file);
  }
}
const manifestRoundTrip =
    zlib.gunzipSync(generated.manifestGzip).toString('utf8');
if (manifestRoundTrip !== generated.manifest) {
  throw new Error('Generated gzip PWA manifest does not round-trip');
}
if (!generated.manifest.includes('"display":"standalone"') ||
    !generated.manifest
        .includes(`"/icons/icon-192.png?v=${generated.assetTag}"`) ||
    generated.manifest.includes('__FW_VERSION__')) {
  throw new Error('PWA manifest must be install-capable and version its icon URL');
}
// iOS composites the home-screen icon onto white and only rounds its own
// corners, so the 192 px asset must stay opaque and full-bleed: truecolour
// without an alpha channel, and the canvas corner in the tile colour instead
// of the light background the artwork used to carry.
const iconIdat = [];
for (let offset = 8; offset < generated.icon192Raw.length - 8;) {
  const length = generated.icon192Raw.readUInt32BE(offset);
  const type = generated.icon192Raw.toString('ascii', offset + 4, offset + 8);
  if (type === 'IDAT') {
    iconIdat.push(generated.icon192Raw.subarray(offset + 8, offset + 8 + length));
  }
  offset += 12 + length;
}
const iconRow0 = zlib.inflateSync(Buffer.concat(iconIdat));
if (generated.icon192Raw.readUInt32BE(16) !== 192 ||
    generated.icon192Raw.readUInt32BE(20) !== 192 ||
    generated.icon192Raw[25] !== 2 ||
    iconRow0[1] > 0xb0 || iconRow0[2] > 0xa0) {
  throw new Error('The 192 px icon must be an opaque 192x192 PNG with the tile colour at its canvas corner');
}
// A fixed release keeps this independent of the generated version header,
// which a fresh checkout lacks ("dev" folds the self-heal away). Only the
// revalidated shell carries the version; assets stay version-free so a
// firmware-only release keeps them cached under the same ?v= tag.
const release = await webUi.generate({versionOverride: '9.8.7+contract', write: false});
const otherRelease = await webUi.generate({versionOverride: '9.8.8+contract', write: false});
if (!release.html.includes('data-fw="9.8.7+contract"') ||
    codeIncludes(release.runtimeJs, '9.8.7+contract') ||
    codeIncludes(release.runtimeJs, '__FW_RELEASE__') ||
    !codeIncludes(release.runtimeJs, 'dataset.fw') ||
    !codeIncludes(release.runtimeJs, 'obbwFwReload') ||
    !codeIncludes(release.runtimeJs, 'location.replace(') ||
    release.assetTag !== otherRelease.assetTag ||
    release.etag === otherRelease.etag) {
  throw new Error('Shell must carry the firmware version for cached-shell self-heal; asset tags must ignore it');
}
// The ?fw= landing refreshes the plain paths' cached shell once, so the next
// open after an OTA loads the new shell directly instead of the stale copy.
{
  const vm = require('vm');
  const fn = runtimeJs.slice(runtimeJs.indexOf('let fwShellRefreshed'), runtimeJs.indexOf('function applyCommonStatus('));
  const fetched = [];
  const context = {SHELL_FW_VERSION: '2.0', uiState: {}, location: {search: '?fw=2.0', pathname: '/stats'},
    sessionStorage: {removeItem() {}, getItem() {}, setItem() {}},
    fetch: (url, init) => { fetched.push([url, init.cache]); return Promise.resolve(); }};
  vm.runInNewContext(fn + ';this.check=checkFirmwareReload;', context);
  context.check('2.0'); context.check('2.0');
  if (JSON.stringify(fetched) !== '[["/","reload"],["/stats","reload"]]')
    throw new Error('The ?fw= landing must refresh the root and current path shells exactly once');
  context.location.search = '';
  const plain = {...context, fwShellRefreshed: false};
  vm.runInNewContext(fn + ';this.check=checkFirmwareReload;', plain);
  plain.check('2.0');
  if (fetched.length !== 2) throw new Error('A plain open must not refetch the shell');
}
// A top-level name shared by two secondary view modules collides inside the
// concatenated bundle after mangling, and the browser then refuses the whole
// module when any secondary tab loads. Parse every generated module before
// it can ship; --check validates ESM when the temp file ends in .mjs.
{
  const spawnSync = require('child_process').spawnSync;
  const os = require('os');
  for (const [moduleName, code] of Object.entries({
    app: generated.js, runtime: generated.runtimeJs,
    'ota-image': generated.otaImageJs, ...generated.modules,
  })) {
    const file = path.join(os.tmpdir(), `openbrewbyweight-webui-${moduleName}-${process.pid}.mjs`);
    fs.writeFileSync(file, code);
    const parsed = spawnSync(process.execPath, ['--check', file]);
    fs.unlinkSync(file);
    if (parsed.status !== 0) {
      throw new Error(`Generated ${moduleName}.js must parse as a module: ${parsed.stderr}`);
    }
  }
}
const cssRoundTrip = zlib.gunzipSync(generated.cssGzip).toString('utf8');
if (cssRoundTrip !== generated.css) {
  throw new Error('Generated gzip Web CSS does not round-trip to the minified CSS');
}
// The minifier drops a shorthand transition carrying allow-discrete, so groups
// and Home equipment rows must keep their opening animation as longhands.
if (!/\.cfgGroup::details-content[,{][^}]*transition-property:block-size,content-visibility/.test(generated.css) ||
    !/\.cfgGroup::details-content[,{][^}]*transition-behavior:allow-discrete/.test(generated.css) ||
    !/\.lampRow::details-content\{[^}]*transition-property:block-size/.test(generated.css)) {
  throw new Error('Generated Web CSS must keep the group and equipment row opening transitions');
}
// The Home quick target-weight sheet inlines its dialog markup into the Home
// partial carried by the shell HTML (pencil button, inert sheet and backdrop):
// +303 measured bytes. Cap raised to 4200; firmware image and OTA limits stay
// fixed.
// The smooth-cut Home partial (pills-only hero, grind-hint and scale-loss
// discovery cards, accordion pencil) measures 4440 bytes, owner-authorized
// 2026-10-14. Cap raised to 4500.
// The Home "Stop the shot" card (segmented control, Protections row and
// panel, off-confirmation, ready notice, preset-lock note, sheet ratio)
// measures 4650 bytes, owner-authorized 2026-10-14. Cap raised to 4700.
// The optional Web UI login (sign-in form in the inactive overlay, Home
// Assistant pairing prompt, Home access notice) measures 5129 bytes,
// owner-requested 2026-10-22. Cap raised to 5140 for asset-tag gzip wobble.
// Sign out inline in the footer's firmware line measures 5165; cap 5180.
if (generated.gzip.length > 5180) {
  throw new Error(`Compressed Web UI shell HTML exceeds the 5180-byte gzip budget (${generated.gzip.length})`);
}
if (!generated.html.includes('rel="manifest" href="/manifest.webmanifest?v=' + generated.assetTag + '"') ||
    !generated.html
        .includes('href="/icons/icon-192.png?v=' + generated.assetTag + '"') ||
    !generated.html.includes('rel="apple-touch-icon"') ||
    !generated.html.includes('name="theme-color"') ||
    !generated.html.includes('name="apple-mobile-web-app-capable"')) {
  throw new Error('Generated shell must link the PWA manifest, icon, and home-screen metadata');
}
// Reallocate another 500 bytes of shell allowance to BBW readback/CSV.
// Complete, human-readable Settings help raises the reviewed combined budget.
// Reallocate 300 bytes from shell headroom to lazy Cloud diagnostics.
// Transfer 160 bytes to runtime request admission/hydration; total cap stays fixed.
// Transfer 100 bytes of shell headroom to the desktop Home scrolling header.
// Transfer 100 bytes of shell headroom to the approved frosted-glass sliding
// navigation pill (glass material, pill, hover ink); total cap stays fixed.
// Transfer 100 bytes of unused shell allowance to the no-scale local clock.
// The combined flash, firmware image and OTA limits remain unchanged.
// Transfer another 400 shell bytes to the no-scale finish transition.
// The Home quick target-weight sheet rides the eagerly inlined home module
// (pointer-drag controller with velocity-adaptive acceleration, tick painter,
// grab dismiss, keyboard stepping, hydrate→goal→save commit): +1231 measured
// bytes over the transferred baseline. Cap raised to 4450; the combined flash
// total stays fixed.
// The ?edit_weight=1 URL launcher (preset-ready wait, admin-lock gate,
// one-shot parameter strip, exposed open) adds +112 measured bytes on top.
// Cap raised to 4600.
// The Micra boiler readiness rows and lamp rollup (locally ticking warm-up
// estimates from absolute readyAt timestamps) add +405 measured bytes.
// Cap raised to 5050.
// The Diagnostic section-copy gesture ships no shell bytes (secondary view
// module and CSS only), but any web-source change permutes the content tag
// stamped into the shell JS cache buster; the raw minified shell stays
// byte-identical while its gzip shifts by tag-entropy noise (5051 measured).
// Cap raised to 5060; the combined flash total stays fixed.
// The quick-weight dial guard range (guard-derived editable bounds, limit
// pills, per-gram labeled ticks, zone tints) rides the same inlined home
// module: +285 measured bytes on top (5336 measured).
// Cap raised to 5400; the combined flash total stays fixed.
// The quick-weight dial fractional-drag fix (sub-tenth pending credit that
// survives slow drags plus the direction-reversal speed reset) rides the
// same inlined home module: 5408 measured.
// Cap raised to 5420; the combined flash total stays fixed.
// The Home "Stop the shot" card (Protections wiring with off-confirmation,
// no-scale and warm-up notice, target ratio, clean-form guard) rides the same
// inlined home module: 5947 measured, owner-authorized 2026-10-14.
// Cap raised to 5960.
// The target sheet's neutral ruler flanked by hold-to-repeat −/+ buttons and
// the dose/ratio pills measure 6071, owner-authorized 2026-10-14. Cap raised
// to 6090.
// Release inertia on the target ruler measures 6241, owner-authorized
// 2026-10-14. Cap raised to 6260.
// Dragging the navigation pill measures 6594, owner-authorized 2026-10-16.
// Cap raised to 6615: the stamped asset tag moves it by a few bytes.
// The mercury navigation pill (spring, teardrop SVG shape, settling loop)
// measures 7229, owner-authorized 2026-10-16. Cap raised to 7250.
// Loading the opening view during the claim measures 7310, requested with
// the load-latency plan 2026-10-16. Cap raised to 7330.
// The audit fixes (lazy quick-weight ruler, background-tab settle, view import
// retry, modified nav clicks) measure 7387, measured raise of the 2026-10-21 frontend audit remediation. Cap
// raised to 7400.
// The audit accessibility fixes (page titles, focus on navigation, modal
// quick-weight focus) measure 7492, measured raise of the 2026-10-21 frontend audit remediation. Cap raised to 7510.
// The Home access notice wiring in the inlined home module measures 7612,
// owner-requested 2026-10-22. Cap raised to 7630 for the stamped asset tag.
// − and + keys inside number fields measure 7840, owner-requested 2026-10-22; cap 7850.
if (generated.jsGzip.length > 7850) {
  throw new Error(`Compressed Web UI shell JS exceeds the 7850-byte gzip budget (${generated.jsGzip.length})`);
}
// Allow fixed chart grids and adaptive axes while retaining the combined cap.
// The activation-history table cards and type badges raise the cap to 7050.
// The Home boot splash surface, ring animation, and reduced-motion override
// raise the cap to 7300. The busy save-button spinner raises it to 7400.
// The approved wireless header adds 200 bytes of compressed CSS allowance.
// The desktop Home header uses 100 bytes transferred from the shell JS cap.
// Independent wireless detail panels add 100 bytes of approved allowance.
// The compact mobile navigation and continuous header resizing add CSS only;
// keep the combined asset and firmware image limits unchanged.
// The home preset radio cards and quick-settings button toggles raise the
// cap to 8700. The Brew by Weight hero accent card and the hidden guard
// card inputs raise the cap to 8900. The approved Home shot hero card
// (prototype 2) raises the cap to 9250. The Home preset accordion that
// replaces the card chips and rule table (radio dot, animated guard rows)
// raises the cap to 9400. The frosted-glass sliding navigation pill uses
// 100 bytes transferred from the shell JS cap. The Stats duration and yield
// metric icons (CSP-safe absolute positioning beside the metric values)
// raise the cap to 9550. The full-screen shot card presentation mode
// (fixed landscape card, portrait rotation, fluid vmin sizing, and the two
// corner icon buttons with safe-area-aware tap floors) raises the cap to 10300.
// The Home quick target-weight sheet frosted surface (backdrop, anchored
// sheet, grab row, conditional Reset pill, masked tick strip) adds +817
// measured bytes. Cap raised to 10950.
// The /setup onboarding view (menu-less shell, centered card, scan/form/
// connecting/success/error states, brand lockup) adds ~250 measured bytes,
// the countdown line ~15 more, and the password reveal toggle ~110 more.
// Cap raised to 11400.
// The History activation-type filter popover (fixed panel, count chip,
// active-state button, touch-sized type rows) adds +158 measured bytes.
// Cap raised to 11700 (pre-authorized raise, 2026-10-09).
// The quick-weight dial guard range (per-gram label row, guard-zone tick
// tints, range-limit pills, and the shared --gFast/--gSlow color vars)
// measures 11721. Cap raised to 11800.
// The audit viewer's clickable rows, focus/hover tint, and detail-row styles
// measure 11802. Cap raised to 11900.
// The smooth-cut styles (review graph, verdict pills, action bar, grind-hint
// and discovery cards) measure 13107 bytes, owner-authorized 2026-10-14.
// The Home "Stop the shot" card that replaces the Quick Settings grid
// (segmented control, Protections row, dots, panel, confirm bar, notice)
// measures 13397 bytes, owner-authorized 2026-10-14.
// The target sheet's −/+ buttons and dose/ratio pills measure 13474 bytes,
// owner-authorized 2026-10-14.
// The Stats legend, cursor handle, and shot-line styles measure 13593 bytes;
// owner-authorized 2026-10-14.
// Folding the Stats card details behind Show more measures 13717 bytes;
// owner-authorized 2026-10-15.
// The Quick Settings Duration selector and Reset now measure 13797 bytes;
// owner-authorized 2026-10-16.
// The grind hint v2 card (tag, confidence, window bar with band and level,
// four state colours) measures 14099 bytes; owner-authorized 2026-10-16.
// The Home hero cut-line chart, verdict colours and their cross-fade measure
// 14164 bytes; owner-authorized 2026-10-16.
// The draggable navigation pill measures 14204 bytes; owner-authorized 2026-10-16.
// The Home cup light warning tone measures 14236 bytes; owner-authorized 2026-10-16.
// The held touch reading legend measures 14250 bytes; owner-authorized 2026-10-16.
// The full-screen keep-awake video layer measures 14268 bytes; owner-authorized 2026-10-16.
// The wider gap before the header guard indicators (f5ccfc6f) measures 14269 bytes.
// The shot weight/flow legend below its chart measures 14284 bytes;
// owner-authorized 2026-10-16.
// The quick-settings duration selection styled as the mode card measures
// 14286 bytes; owner-authorized 2026-10-16.
// The header mode pill joined to the guard stack measures 14313 bytes;
// owner-authorized 2026-10-16.
// The gradual forecast hero tint (registered --tp blend percent) brings the CSS to 14384 bytes; owner-authorized 2026-10-17.
// The dark-mode floating-shadow variable (--popSh) brings the CSS to 14406 bytes; owner-authorized 2026-10-17.
// Homologating every dark floating panel onto the popup elevation shadow brings the CSS to 14464 bytes; owner-authorized 2026-10-17.
// The shot card header with a large faint metric glyph measures 14322 bytes;
// owner-authorized 2026-10-16.
// SoftAP onboarding P1 (/setup address rows, disabled Connect, Search again) measures 14479 bytes; owner-requested 2026-10-18.
// Onboarding brewing steps (mode and recipe choice cards, summary rows) measure 14621 bytes; owner-requested 2026-10-18.
// The shot chart's real-stop dot and its legend keys measure 14648 bytes; owner-requested 2026-10-18.
// The La Marzocco Cloud sign-in steps (pending, spinning, done) measure 14745 bytes; owner-requested 2026-10-18.
// The La Marzocco Cloud linked note restyled as a green confirmation measures 14775 bytes; owner-requested 2026-10-18.
// Styling the empty shot and activation history notes measures 14791 bytes; owner-requested 2026-10-18.
// The La Marzocco Cloud state pill and its four tones measure 14861 bytes; owner-requested 2026-10-18.
// Home Equipment device icons, status badges and headline values measure 15007 bytes; owner-requested 2026-10-18.
// The Home shot card's centred flow and its full-screen fit measure 15037 bytes; owner-requested 2026-10-19.
// The Stats shot card drip cell and its shifted grid measure 15049 bytes; owner-requested 2026-10-20.
// The drip compensation chart (bars, markers, line in use) measures 15432 bytes; owner-requested 2026-10-20.
// The Stats shot card cut and drip delay cells in a reflowed grid measure 15464 bytes; owner-requested 2026-10-20.
// The Stats consistency columns (band, columns, edges, level tones) measure 15681 bytes; owner-requested 2026-10-20.
// The Stats consistency clipped-column notch measures 15691 bytes; owner-requested 2026-10-20.
// The Home preset rules peek and its graph chevron measure 15742 bytes; owner-requested 2026-10-20.
// The optional Web UI login (sign-in form, Access panel rows and sheet, pairing code) measures 15825 bytes;
// owner-requested 2026-10-22.
// The footer Sign out link style measures 15837 bytes; owner-requested 2026-10-22.
// The Access signed-in device list (rows, icons, in-row confirmation, accordion count) measures 16019 bytes; owner-requested 2026-10-22.
// No separators around the device accordion and the inline rename field: 16082; owner-requested 2026-10-22.
// Settings group rows (icon, summary, chevron, animated opening): 16272; owner-requested 2026-10-22.
// Admin as one card of section rows: 16291; owner-requested 2026-10-22.
// Diagnostics section rows with icons: 16325, owner-requested 2026-10-22.
// Outlined History cup for shots without scale weight: 16372, owner-requested 2026-10-22.
// Fixed Administration unlock section: 16387, owner-requested 2026-10-22.
// Group switches, setting rows and the sunken open-group band: 16567, owner-requested 2026-10-22.
// A setting row closing the band meets its bottom edge, and the group opening transition
// survives minification, with a symmetric band: 16634, owner-requested 2026-10-22.
// Joined-box number keys, compact fields and the dropdown key: 16839, owner-requested 2026-10-22.
// Home equipment rows on the sunken band: 16868, owner-requested 2026-10-22.
// Group transition curve and the no-interpolate-size fallback switch: 16921, owner-requested
// 2026-10-22. Animated equipment rows without hover or text selection: 16932, owner-requested
// 2026-10-22. Compact History cards grouped under day headers: 16955, owner-requested
// 2026-10-23; cap 16955. Settings scope notes, the Account group and one-weight outlined
// History icons: 17073, owner-requested 2026-10-22; cap 17090. Unfilled outlined History icons
// and the La Marzocco scope note above Account: 17102, owner-requested 2026-10-22; cap 17120.
// List filter groups and result dots, the Stats window bar, and icon-only Shots actions on
// phones: 17134, owner-requested 2026-10-23; cap 17140. Diagnostics closing rule and the
// detached download button: 17146, owner-requested 2026-10-23; cap 17150. The device
// rename button and dimmed inert controls: 17154, owner-requested 2026-10-23; cap 17160.
// The red Access password note: 17163, owner-requested 2026-10-23; cap 17163.
// Shot-card Target temp, Dose, and Ratio grid rows with the temp-less
// fallback layout: 17337, owner-requested 2026-10-24; cap 17337.
if (generated.cssGzip.length > 17337) {
  throw new Error(`Compressed Web CSS exceeds the 17337-byte gzip budget (${generated.cssGzip.length})`);
}
// Include zero baselines and the first-drop marker without sacrificing legibility.
// Exporting the saved weight curve as per-shot CSV columns raises the cap by 100 bytes.
// Continuous smoothed flow-rate polylines raise the cap from 32600 to 32800 bytes.
// Activation-history paging, sorting, clear, and per-card delete raise it to 33700.
// The removed Admin idle-scan backoff select once raised this to 33900.
// Activation-history card type icons (inline coffee/rinse SVG) raise it to 35000.
// The Admin device-name validation, preference save, and .local status raise it to 35200.
// Humanized shot/activation time labels (relative day ladder) raise it to 35300.
// The outlined-cup shot icon (shots without registered weight) raises it to 35500.
// Weekday names, short dates, and the hover <time> wrapper raise it to 35600.
// Linea Micra cloud account selection, read-only state expiry, and
// preset-temperature wiring are part of the profile-gated runtime bundle.
// Rebranded user-visible strings (longer brand names and hints) raise it to 37000.
// The Home boot splash one-shot hide helper raises it to 37150.
// Machine-type-exclusive builds guard every stripped element access in the
// runtime (loads, save payload, validation, hydration) raising it to 37400.
// Measured with a fixed sentinel build id so commit-SHA noise cannot move it:
// the Admin BLE master-switch checkbox save handler measures 37490 bytes.
// The scale friendly-name rename flow (display helper, prompt, validation,
// command) raises it to 37900 bytes.
// The Settings Preferred scale (rename) link follows the selected history
// entry, raising it to 37965 bytes.
// The Home last-shot moment label (status wall-time passthrough plus the
// humanized render under the duration) raises it to 38000 bytes.
// The time-zone catalog, selected-zone preview, and first-use detection
// raise the measured source to 38780 bytes.
// The busy save-button spinner wiring raises it to 40100 bytes.
// Per-section revert (discard unsaved changes) snapshot helpers, handlers,
// and hydration-time baseline capture beside the save flows raise it to
// 40750 bytes.
// Deferred Admin BLE/Power/Frontend saving (dirty flags and markers, guarded
// hydration with panel baselines, relocated theme helpers) raises it to
// 41300 bytes.
// Compatibility-mode gating (status flag, nav chrome, firmware-mode save
// flow in command()) raises it to 41450 bytes.
// The rule-chart merged range labels (collision runs collapsing into one
// centered label such as "34–36 g" on the home preset axes) raise it to
// 41700 bytes.
// The in-panel loading wave (settlePanel fades plus load guards replacing
// the flashing Loading/empty table rows) raises it to 41900 bytes; the
// wrap-height settle animation that removes the final layout jump raises
// it to 41950 bytes.
// Mid-upload OTA image guarding (the per-range re-verification call and the
// changed-file error mapping in the runtime module) raises it to 42100 bytes.
// Shot stats Tare time and Scale (the tare chart marker, two table columns,
// CSV columns, and last-shot wiring) raise it to 42400 bytes.
// Paired read admission/hydration uses 160 bytes transferred from shell JS.
// Independent popup interactions and identity rendering add 400 approved bytes.
// Supervised backflush state/history labels and deadline diagnostics add 500 bytes.
// The Home shot hero card raises the sentinel runtime cap to 45500 bytes.
// The view-scoped diagnostic stream (frame validation, subscription lifecycle,
// and the shared live section renderer) raises the cap to 46100 bytes.
// The local timer receives the same 100 bytes transferred from shell JS.
// The finish transition receives the same 400 transferred shell bytes.
// The full-screen shot card state machine (enter/exit/back-consume, lazy
// button binding, and the shot-aware session keep-alive) raises the cap to
// 47200 bytes.
// The view-scoped history WebSocket (frame validation, subscription replay,
// and one-shot append fetches) raises the cap to 47800 bytes.
// The Stats stream (continuation frames, row/export validation) raises it to
// 48300 bytes. The Diagnostic full-WebSocket migration (log stream client,
// snapshot readiness, local clock ticker) adds ~150 compressed bytes while
// deleting the REST pull plumbing; source allowances above cover the rest.
// The dead-code cleanup moves the shared list-sentinel helper into the
// runtime while deleting the dead pagination helpers and the UTC wrapper
// hop; the minified runtime shrinks by ~130 raw bytes and the combined
// flash total by ~280, but terser's renamed-identifier entropy shifts the
// gzipped runtime by +30 bytes. Cap raised to 48550.
// Session-standing Stats/History subscriptions (cache-hit view entry,
// background model application, the immediate background ensure, and the
// per-stream sync markers) measure +68 compressed bytes on the sentinel
// build while deleting the per-view stop paths. Cap raised to 48700.
// Brew settings edited-field error attribution (rule table with per-side
// phrasing, baseline-diff edited-field detection, multi-error renderer)
// replaces the first-failure validator's shorter strings: +547 compressed
// bytes on the sentinel build. Cap raised to 49300.
// Live brew validation warnings (validator twin with the required-result
// filter, signature-skip renderer, scoped red clear, symmetric aria, and
// the conflict chip, plus the shared aria-rebuild helper and multi-warning
// describedby append) add +429 compressed bytes on the sentinel build
// (pre-authorized raise, 2026-10-09). Cap raised to 49740.
// Micra boiler readiness adds the diagnostic readiness/boiler/water
// renderers with locally interpolated estimates: +438 compressed bytes.
// Cap raised to 50250 (pre-authorized raise, 2026-10-09).
// The diagnostic States power row and decode-robustness rework measure
// 50249; headroom raised to 50400.
// History CSV export (streamed fetch windows over the standing subscription
// plus the download/CSV helper split out of the Stats export) and the
// multi-select activation-type filter (visible-records projection,
// progressive page fill, fixed-position popover with live filtering and
// quick reset) measure +814 compressed bytes. Cap raised to 51500
// (pre-authorized raise, 2026-10-09).
// The smooth-cut runtime (cut-line model, review graph, validation, hero and
// cards) measures 56658 bytes, owner-authorized 2026-10-14; the shot-statistics
// histogram and grind-hint wording bring it to 56903.
// The Home "Stop the shot" card (Protections summary, preset-lock note,
// clean-form guard) measures 57250 bytes, owner-authorized 2026-10-14.
// The Stats shot preset graph, More charts toggle, and the shared preset graph
// measure 57584 compressed bytes; owner-authorized 2026-10-14.
// The verdict-zone fill brings the runtime to 57622 compressed bytes; owner-authorized 2026-10-14.
// The Stats shot cursor and Home graphs bring the runtime to 58319 compressed bytes;
// owner-authorized 2026-10-14.
// The Stats legend and cursor bring the runtime to 58454 compressed bytes; owner-authorized 2026-10-14.
// The preset graph cut pill brings the runtime to 58655 compressed bytes; owner-authorized 2026-10-15.
// The first-drop pace start brings the runtime to 58877 compressed bytes; owner-authorized 2026-10-15.
// The Stats shot cursor pill brings the runtime to 58924 compressed bytes; owner-authorized 2026-10-15.
// Folding the Stats card details behind Show more brings the runtime to 59286 compressed bytes; owner-authorized 2026-10-15.
// The Stats arrival-time density curve brings the runtime to 59356 compressed bytes; owner-authorized 2026-10-16.
// The fast-to-slow Stats card fold with fading details and charts brings the runtime to 59457 compressed bytes; owner-authorized 2026-10-15.
// The Quick Settings Duration selector and revert note bring the runtime to 59524 compressed bytes; owner-authorized 2026-10-16.
// The grind hint v2 card (four states, size and confidence wording, window bar) brings the runtime to 60236 compressed bytes; owner-authorized 2026-10-16.
// The Home hero cut-line chart, verdict colours and fast forecast bring the runtime to 61520 compressed bytes; owner-authorized 2026-10-16.
// The Home hero drip tail brings the runtime to 61586 compressed bytes; owner-authorized 2026-10-16.
// The Home cup light states bring the runtime to 61656 compressed bytes; owner-authorized 2026-10-16.
// Held touch readings in the shot charts, Max flow and CSV bring the runtime to 61933 compressed bytes; owner-authorized 2026-10-16.
// The full-screen keep-awake (Wake Lock, inline video, Esc) brings the runtime to 62758 compressed bytes; owner-authorized 2026-10-16.
// Reading the build version from the shell brings the runtime to 62759 compressed bytes; frontend audit 2026-10-16.
// View-only code moves to the lazy view modules (frontend audit 2026-10-16).
// The shot weight/flow chart cursor raises the runtime to 48830 compressed bytes; owner-authorized 2026-10-16.
// Moving the Stats shot charts and list into the Stats module brings the runtime to 43906 compressed bytes (2026-10-16).
// Moving the History rows and type filter into the History module brings it to 41386 (2026-10-16).
// Moving the Diagnostic live renderer into the Diagnostic module brings it to 39982 (2026-10-16).
// Moving the Stats chart and shot-row helpers into the Stats module brings it to 38465 (2026-10-16).
// The gradual forecast hero tint (firmware-owned tintClass/tintPct, advisories, landing estimate) with the window-mode fast alert restored brings the runtime to 38895 compressed bytes; owner-authorized 2026-10-17.
// The iOS-only unmuted keep-awake track and its tap resume bring it to 38983; owner-requested 2026-10-17.
// The Linea Micra sign-in state and its Save/Revert pairing bring it to 39012; owner-requested 2026-10-18.
// The La Marzocco Cloud legend states (connected, connecting, choose) bring it to 39038; owner-requested 2026-10-18.
// The Home Cup accessory state line brings it to 39158; owner-requested 2026-10-18.
// The Home shot card flow (scale-reported or chart-computed, hidden during touch holds) brings it to 39296; owner-requested 2026-10-19.
// The Home shot card flow fading from live to its average when the shot finishes brings it to 39411; owner-requested 2026-10-19.
// The Home shot card flow line and fill with a dashed drip tail brings it to 39497; owner-requested 2026-10-19.
// The Home shot card smoothed flow area brings it to 39547; owner-requested 2026-10-19.
// The Home shot card live flow averaged over its last second brings it to 39579; owner-requested 2026-10-19.
// The Home shot card firmware clock in both scale modes with its ±0.1 s final swap brings it to 39655; owner-requested 2026-10-19.
// Shot card field deltas merged over the previous card bring it to 39668; owner-requested 2026-10-20.
// Shot curves decoded from the firmware's delta stream, with the minimal live envelope, bring it to 39974; owner-requested 2026-10-20.
// Home and Diagnostic increments continue the snapshot identity brings it to 40000; owner-requested 2026-10-21.
// Stream closed after 5 minutes hidden and resumed without snapshots brings it to 40068; owner-requested 2026-10-21.
// The ?fw= landing refreshes the cached shell after an OTA brings it to 40126; owner-requested 2026-10-21.
// A reconnect keeps the presets it already holds brings it to 40227; owner-requested 2026-10-21.
// Stats rows as column-ordered values brings it to 40259; owner-requested 2026-10-21.
// Review fixes for the hidden close, setup sessions and the 4002 reconnect brings it to 40297; owner-requested 2026-10-21.
// Browser storage keys renamed from the legacy ss prefix to obbw bring it to 40322; rename owner-requested 2026-10-21.
// The audit hardening (one shared download helper, CSV formula guard and BOM, timezone retry backoff,
// escaped attribute text) brings it to 40357; measured raise of the 2026-10-21 frontend audit remediation.
// The audit accessibility fixes (keyboard preset cards, named rename field, live message order) bring it
// to 40442; measured raise of the 2026-10-21 frontend audit remediation.
// Review fix RV-07 (trimming old log lines keeps a scrolled-back reader in place) brings it to 40444; cap 40460
// for gzip wobble.
// The optional Web UI login (session header, sign-in form, Home Assistant pairing prompt; shell-level, so it
// lives in the runtime) brings it to 41531, owner-requested 2026-10-22; cap 41540 for gzip wobble.
// Footer Sign out with its confirmation brings it to 41591, owner-requested 2026-10-22; cap 41610.
// The browser's device label sent at sign-in brings it to 41846, owner-requested 2026-10-22; cap 41860.
// The group-row helpers shared by Settings and Admin bring it to 42012, owner-requested 2026-10-22.
// The group animation fallback without interpolate-size brings it to 42215, owner-requested 2026-10-22.
// Equipment rows in the fallback and unfolded-only preset graphs: 42251, owner-requested 2026-10-22; cap 42260.
// The filter menu shared by the Shots and History lists brings it to 43116, owner-requested
// 2026-10-23; cap 43130.
// Rejected-op (4003) recovery that clears the list views brings it to 43169, owner-requested
// 2026-10-23; cap 43180.
// Stream render errors kept off the corrupt-frame path bring it to 43181, owner-requested
// 2026-10-23; cap 43200.
if (sentinelRuntimeGzip.length > 43200) {
  throw new Error(`Compressed Web UI runtime JS exceeds the 43200-byte gzip budget (${sentinelRuntimeGzip.length})`);
}
if (generated.otaImageGzip.length > 3072) {
  throw new Error('Compressed OTA image module exceeds the 3 KiB gzip budget');
}
// Continuous loop timing and the delay/dispatch breakdown live in Diagnostics.
// The Diagnostic scale-name rename link binding raises it to 7020 bytes.
// The connected scale command table adds bounded Diagnostic rendering.
// The busy save-button ids on the Admin network, password, and webhook
// saves raise it to 7200 bytes.
// The Admin network, webhook, and device-password revert wiring and the
// date-time revert handler raise it to 7420 bytes.
// Deferred Admin BLE/Power/Frontend save and revert handlers raise it to
// 7650 bytes.
// The Admin Firmware master-switch section (checkbox hydration, save/revert
// wiring, compatibility-mode section hiding) raises it to 7950 bytes.
// Cloud diagnostics use 300 bytes of the shell allowance; total cap is fixed.
// Scale profiling adds the Diagnostic state matrix, four controls, and the
// streamed TXT download (~540 gzip bytes); the lazy partial keeps it out of
// the always-loaded runtime and shell budgets.
// Initial power-source diagnostics measure 9840 bytes after reusing row/render
// logic; allow 9900 bytes. Firmware image and combined asset budgets stay fixed.
// The Diagnostics audit viewer (records table renderer, hunt-session toggle,
// explicit clear) measures 10287 bytes. Cap raised to 10350.
// The audit per-row full-detail viewer (row toggle with keyboard access) and
// the full-log Export download measure 10638. Cap raised to 10700.
// The audit table pagination and change-gated repaint (page state, pager
// renderer, selection-preserving fingerprint) measure 10839. Cap raised to 10900.
// Each lazy view module carries its logic and markup (frontend audit
// 2026-10-16); Stats/History measure 4403, Diagnostic 12370, Admin 14127,
// Settings 14074 and Setup 3552 gzip bytes. The Stats shot charts and list
// moved out of the runtime bring Stats/History to 9865 (2026-10-16).
// The History rows and type filter moved out of the runtime bring it to 12143 (2026-10-16).
// The Diagnostic live renderer moved out of the runtime brings Diagnostic to 13626 (2026-10-16).
// The Stats chart and shot-row helpers moved out of the runtime bring Stats/History to 13436 (2026-10-16).
// The gradual forecast Admin hint text adds a few bytes to admin.js (14226); owner-authorized 2026-10-17.
// Gzip padding wobble after the full-screen keep-awake layer measures admin.js at 14227; owner-authorized 2026-10-17.
// Gzip wobble from the Home hero corridor chart (admin.js unchanged) measures 14229; owner-requested 2026-10-18.
// SoftAP onboarding P1 (status-driven steps, blocked notice, copyable addresses) measures setup.js at 4216; owner-requested 2026-10-18.
// Hiding cleared Diagnostic data until the device confirms it measures 13743; owner-authorized 2026-10-18.
// Onboarding brewing steps (welcome, starting recipe with dose, yield and Recommended tags, summary with Change, their copy) measure setup.js at 5788; owner-requested 2026-10-18.
// The shot chart's real-stop dot and its two legend entries measure records.js at 13594; owner-requested 2026-10-18.
// Gzip wobble from the /setup stream guard (records.js unchanged) measures 13595; owner-reported 2026-10-18.
// Gzip wobble from the Home Micra cloud hint (records.js unchanged) measures 13606; owner-requested 2026-10-18.
// The Linea Micra sign-in, machine choice, auto-select, grouped options and batch save measure settings.js at 14827; owner-requested 2026-10-18.
// Gzip wobble from the Linea Micra settings section (records.js unchanged) measures 13607; owner-requested 2026-10-18.
// Linea Micra Change machine and its machine-count re-list after restart measure settings.js at 15024; owner-approved 2026-10-18.
// Restoring the La Marzocco Cloud sign-in steps, legend state and lone-machine auto-select measure settings.js at 15219; owner-requested 2026-10-18.
// The La Marzocco Cloud state pill and its machine and cloud states measure settings.js at 15470; owner-requested 2026-10-18.
// The La Marzocco Cloud Account label on its own line measures settings.js at 15483; owner-requested 2026-10-18.
// Gzip wobble from the La Marzocco Cloud state pill (setup.js unchanged) measures 5789; owner-requested 2026-10-18.
// The Diagnostic Micra WebSocket live/parked/closed link label measures diagnostic.js at 13800
// and adds 1 byte of gzip wobble to settings.js (15484, unchanged source); owner-requested 2026-10-18.
// Gzip wobble from the Home Equipment icons and values (records.js, diagnostic.js and settings.js unchanged) measures 13608, 13801 and 15485; owner-requested 2026-10-18.
// Gzip wobble from the Home hero always using the computed flow (settings.js unchanged) measures 15514; owner-authorized 2026-10-19.
// The Stats shot card drip cell measures records.js at 13660; owner-requested 2026-10-20.
// Gzip wobble from swapping Late cup and 1st drop on the Stats shot card (records.js and setup.js unchanged) measures 13664 and 5793; owner-requested 2026-10-20.
// The drip compensation redesign (chart in settings.js, text table shared with records.js) measures 13668 for records.js and 16800 for settings.js; owner-requested 2026-10-20.
// The Stats shot card cut and drip delay cells and their CSV columns measure records.js at 13809, with settings.js gzip wobble from the shared text table at 16804; owner-requested 2026-10-20.
// The Stats consistency columns (renderer, level header and scrub) measure records.js at 14826; owner-requested 2026-10-20.
// The drip chart ten-shot window (ten slots, bbw ring in the status snapshot) measures records.js at 14830; owner-requested 2026-10-20.
// Stats consistency thresholds as a share of the average (pure model, units per data, floor/cap scale, slots) measure records.js at 15041, with settings.js gzip wobble from the shared text table at 16807; owner-requested 2026-10-20.
// The Stats charts reading the firmware's newest-10 window measure records.js at 15096; owner-requested 2026-10-20.
// The Diagnostic Heap min waiting text measures diagnostic.js at 13827 and moves records.js to 15097 through shared text table gzip wobble; owner-requested 2026-10-20.
// The Diagnostic Heap min before and after the boot heap shaper release measures diagnostic.js at 13871; owner-requested 2026-10-20.
// Home and Diagnostic minimal increments move records.js to 15098 through shared text table gzip wobble; owner-requested 2026-10-21.
// The review fixes move diagnostic.js to 13872 through shared text table gzip wobble; owner-requested 2026-10-21.
// The audit F1 fixes (escaped consistency pill names, typed-build save guard, unique Settings rename id) measure
// records.js at 15103 and settings.js at 16808; measured raise of the 2026-10-21 frontend audit remediation.
// The audit F2 fixes (single CSV export, History filter toggle, locale-proof averages, rename Cancel) measure
// records.js at 15108 and settings.js at 16813; measured raise of the 2026-10-21 frontend audit remediation.
// The audit render fixes (rated star keeps focus, one curve model per row, keyed Diagnostic tables) measure
// records.js at 15237 and diagnostic.js at 13904; measured raise of the 2026-10-21 frontend audit remediation.
// The audit OTA fixes move records.js to 15245 through asset-tag gzip wobble (records.js unchanged); cap 15260, measured raise of the 2026-10-21 frontend audit remediation.
// The audit T4 refactors move diagnostic.js to 13906 through gzip wobble; measured raise of the 2026-10-21 frontend audit remediation.
// The Setup watchdog stop flag measures setup.js at 5799; measured raise of the 2026-10-21 frontend audit remediation.
// The audit accessibility and form fixes (Setup focus, labelled choices, single Skip) measure setup.js at 5855 (cap 5870 for asset-tag gzip wobble); measured raise of the 2026-10-21 frontend audit remediation.
// Review fixes RV-10 and RV-12 (escaped preset filter label, star radio keyboard pattern) measure records.js at
// 15345 and RV-08 (pending log level) diagnostic.js at 13919; caps 15360 and 13930 for asset-tag gzip wobble.
// The Admin Access panel (login switch and confirmation sheet, remembered devices, Home Assistant row,
// sign-out rows) measures admin.js with its markup at 15624, owner-requested 2026-10-22; cap 15640
// for asset-tag gzip wobble. Its deep link to Access after an unlock measures 15703; cap 15720.
// Showing one password form at a time measures 15761; cap 15780. The factory-password reminder on
// the unlock form measures 15824; cap 15840. Moving Sign out to the footer measures 15749; cap 15770.
// The signed-in device list with in-row sign out, as an accordion, measures 16685; with inline renaming 16914; cap 16930.
// Settings group rows (icon sprite and a live summary per group) measure settings.js at 18228,
// owner-requested 2026-10-22; cap 18250 for asset-tag gzip wobble. Admin as section rows (icon sprite,
// a live summary per section) measures admin.js at 17857, owner-requested 2026-10-22; cap 17880.
// Diagnostics section rows with icons (the page's icon sprite ships in the module): diagnostic.js at 14688, owner-requested 2026-10-22.
// Settings autosave (per-field patches, Save/Revert removed) measures settings.js at 18282, owner-requested 2026-10-22; cap 18300 for asset-tag gzip wobble.
// Compact History cards under day headers measure records.js at 15389, owner-requested 2026-10-23; cap 15420 for asset-tag gzip wobble.
// Settings scope notes and the Account group measure settings.js at 18413, and one-weight outlined
// History icons measure records.js at 15420, owner-requested 2026-10-22; caps 18430 and 15440.
// Localized, bounded Wi-Fi scan states and the keyboard rename button measure admin.js at 17947,
// owner-requested 2026-10-23; cap 17970. Access password notes: admin 18002,
// owner-requested 2026-10-23; cap 18002.
const viewModuleBudgets = {records: 15440, diagnostic: 14690, admin: 18002, settings: 18430, setup: 5870};
for (const [file, budget] of Object.entries(viewModuleBudgets)) {
  if (generated.moduleGzip[file].length > budget) {
    throw new Error(`Compressed ${file}.js exceeds the ${budget}-byte gzip budget (${generated.moduleGzip[file].length})`);
  }
}
// The PWA manifest and the 192 px home-screen icon are additive embedded
// assets; they raise the combined cap from 66500 bytes. The 48 px favicon
// served on direct /favicon.ico probes raises it further to 101000 bytes.
if (generated.manifestGzip.length > 1024) {
  throw new Error('Compressed PWA manifest exceeds the 1024-byte gzip budget');
}
if (generated.icon192Raw.length > 31500) {
  throw new Error('The 192 px icon exceeds its 31500-byte budget');
}
if (generated.icon48Raw.length > 3500) {
  throw new Error('The 48 px icon exceeds its 3500-byte budget');
}
// Continuous color-segment flow curves close each segment at its boundary,
// raising the combined cap from 66400 to 66500 bytes; the PWA manifest and
// icons raise it further to 101000 bytes; the activation-history view raises
// it to 103000 bytes; its card type icons raise it to 104500 bytes; the
// icon-proof class-based card layout raises it to 104600 bytes; the
// outlined-cup shot icon (shots without registered weight) raises it to
// 104800 bytes; the other-type card icon (with the per-icon fill attribute
// hoisted into shared CSS and shortened shot viewBoxes as offsets) raises
// it to 104900 bytes. The profile-gated Linea Micra cloud surface includes
// account connection, machine selection, read-only diagnostics, and one
// default-on wake-gesture option. Rebranded user-visible strings raise it
// to 107600 bytes.
// The Home boot splash (markup, styles, and the hide helper) raises it to
// 108200 bytes; the firmware image carries the same assets once, so the
// versioned image and rodata growth budgets stay untouched.
// Deferred Admin BLE/Power/Frontend saving (save/revert bars, dirty-state
// handling, and relocated theme helpers) raises it to 108800 bytes.
// The Admin Firmware master-switch section and compatibility-mode gating
// raise it to 109800 bytes; the firmware image carries the same assets
// once, so the versioned image and rodata growth budgets stay untouched.
// The in-panel loading wave overlay (the settling wave leaves the table
// flow so records paint at their final position under a fading veil)
// raises it to 109900 bytes.
// The Settings preset-card wave (same panelState slot wrapped around the
// preset cards grid, settled by ingestPresets) raises it to 110000 bytes.
// Mid-upload OTA image guarding (per-range re-verification in the runtime
// and image modules plus its changed-file error string) raises it to
// 110500 bytes.
// Shot stats Tare time and Scale (tare marker, table columns, and wiring)
// raise it to 110700 bytes; the firmware image carries the same assets
// once, so the versioned image and rodata growth budgets stay untouched.
// Six Cloud diagnostic fields and their lazy renderer add 500 bytes of
// reviewed allowance. Firmware image and OTA partition limits stay fixed.
// The Stats loading wave reuses the panel helper and adds 50 bytes of reviewed
// allowance. Firmware image and OTA partition limits stay fixed.
// The Micra-only Home power summary raises the compressed budget by 150 bytes.
// The approved wireless header adds 1 KB of reviewed Web asset allowance;
// firmware image, memory-region and OTA partition limits remain unchanged.
// The approved independent wireless popups raise the combined cap by 500 bytes.
// Reviewed WS assets measure 115211 bytes; independent per-asset caps remain.
// The supplied backflush icon and supervision UI add 1 KB of reviewed allowance.
// Firmware image, memory-region and OTA partition limits remain unchanged.
// The Brew by Weight hero accent card raises the combined cap to 117500 bytes;
// the Home shot hero card raises it to 118500 bytes; firmware image,
// memory-region and OTA partition limits remain unchanged.
// Full Home scalar deltas, socket recovery/liveness and shared cleaning state
// add 1500 bytes of allowance; firmware, memory and OTA limits remain fixed.
// The view-scoped diagnostic stream and the Stats metric icons raise the
// combined cap to 120700 bytes; firmware, memory and OTA limits stay fixed.
// The reviewed no-scale finish animation adds 200 combined asset bytes.
// The full-screen shot card (hero markup, safe-area-aware corner buttons,
// state machine, and contract-tested keep-alive) adds ~1.6 KB of reviewed
// combined allowance; per-asset caps above govern
// the split. Firmware image, memory placement and OTA partitions retain
// separate limits.
// The view-scoped history WebSocket adds its reviewed runtime allowance to
// the combined cap (123200 bytes); firmware image and OTA limits stay fixed.
// The Stats stream raises the combined cap to 123600 bytes; firmware image
// and OTA limits stay fixed. The Diagnostic full-WebSocket migration adds
// ~200 compressed bytes (log stream client, snapshot readiness, clock
// ticker) while deleting the REST pull plumbing.
// The 2026-10 readable-JS authoring reformat leaves every payload byte
// unchanged (verified per variant); the new cache-buster tag digits alone
// shift the gzipped total by +21 bytes, raising the measured combined to
// 123815. Cap raised to 123900; firmware image and OTA limits stay fixed.
// The Home quick target-weight sheet adds its reviewed combined allowance
// (sheet JS in the shell chunk, frosted CSS, dialog markup in the Home
// partial): 125976 measured. Cap raised to 126100; firmware image,
// memory placement and OTA partitions stay fixed.
// The /setup onboarding view adds its own lazy partial and view JS chunk
// plus the centered-card CSS and shell route wiring: 129350 measured.
// Cap raised to 129400; firmware image, memory placement and OTA
// partitions stay fixed.
// The connecting-state countdown (deadline text, tabular-numeric style)
// adds ~170 measured bytes, the password reveal toggle ~215 more, and the
// chain heartbeat with the pre-connect address line ~170 more.
// Cap raised to 130200; firmware image, memory placement and OTA
// partitions stay fixed.
// Brew settings edited-field error attribution adds its reviewed runtime
// allowance to the combined cap (130594 measured). Cap raised to 130700;
// firmware image, memory placement and OTA partitions stay fixed.
// The /setup skip exit and grace Done button (quiet form action,
// skipped-state card, probe-URL helper, skip/finish action flows) add their
// reviewed combined allowance: 130990 measured. Cap raised to 131000.
// Live brew validation warnings add their reviewed combined allowance
// (runtime engine + CSS twins + settings chip, plus the aria-rebuild
// round): 131502 measured (pre-authorized raise, 2026-10-09). Cap raised
// to 131600; firmware image, memory placement and OTA partitions stay fixed.
// Micra boiler readiness (Home rows and lamp, diagnostic machine section,
// locales) adds its reviewed combined allowance: 132502 measured
// (pre-authorized raise, 2026-10-09). Cap raised to 132600; firmware image,
// memory placement and OTA partitions stay fixed.
// The States-column power row and decode-robustness rework measure 132583;
// headroom raised to 132700.
// History CSV export and the activation-type filter add their reviewed
// combined allowance (runtime windows/filter logic, popover CSS, History
// partial buttons and funnel symbol, locales): 134023 measured
// (pre-authorized raise, 2026-10-09). Cap raised to 134200; firmware image
// and OTA partition limits stay fixed.
// The Diagnostic press-and-hold section copy (pointer hold/cancel wiring in
// the secondary bundle plus the legend selection-lock CSS) measures 134236.
// Cap raised to 134300; firmware image and OTA partition limits stay fixed.
// The quick-weight dial guard range (guard-derived editable bounds, limit
// pills, per-gram labeled ticks, zone tints in the inlined home module, plus
// the Home partial pills and the dial CSS) measures 134688, and the
// Diagnostics audit viewer adds ~730 more (panel markup, table CSS share,
// secondary JS, locales): 135418 combined.
// Cap raised to 135500; firmware image and OTA partition limits stay fixed.
// The decigram goal migration (0.1-step Settings validation branch, one-decimal
// displays, and the tenth-precision dial with statically positioned ticks moved
// by a single transform) measures 135568 combined.
// Cap raised to 135700; firmware image and OTA partition limits stay fixed.
// The audit viewer's row toggle and Export button plus the detail-row CSS
// measure 135959 combined. Cap raised to 136100; firmware image and OTA
// partition limits stay fixed.
// The audit table pagination and change-gated repaint (pager markup and
// styles, page-state renderer, selection-preserving fingerprint) measure
// 136315 combined. Cap raised to 136500; firmware image, memory placement,
// and OTA partition limits stay fixed.
// The smooth-cut Web UI (preset editor and review graph, Home pills and
// cards, Shared settings regrouping, locales) measures 143513 combined,
// owner-authorized 2026-10-14; the shot-statistics histogram and grind-hint
// wording bring it to 143767. Cap raised to 144000; the firmware image
// allowance is raised alongside, OTA partition limits stay fixed.
// The Home "Stop the shot" card that replaces Quick Settings measures 145174
// combined, owner-authorized 2026-10-14. Cap raised to 145300; OTA partition
// limits stay fixed.
// The target sheet's ruler with −/+ buttons and dose/ratio pills measures
// 145458 combined, owner-authorized 2026-10-14. Cap raised to 145500.
// The Stats shot preset graph and its toggle measure 145724 combined;
// owner-authorized 2026-10-14. Cap raised to 145800; partition limits stay fixed.
// The verdict-zone fill brings the combined gzip to 145881; owner-authorized 2026-10-14.
// Cap raised to 145900; partition limits stay fixed.
// Combined gzip is at this build's cap after the cursor and Home graphs; owner-authorized 2026-10-14.
// Partition limits stay fixed.
// The preset graph cut pill brings combined gzip to 147070; owner-authorized 2026-10-15.
// The first-drop pace start brings combined gzip to 147297; owner-authorized 2026-10-15.
// Partition limits stay fixed.
// What a cold Home load downloads before it can paint (shell, CSS, app and
// runtime) is the budget users feel; view-only code belongs in the lazy view
// modules. Record why a cap moves in the commit, not in comments here.
// Measured 73846 bytes after the frontend audit (2026-10-16).
const coldHomeGzip = generated.gzip.length + generated.cssGzip.length +
    generated.jsGzip.length + generated.runtimeGzip.length;
// The gradual forecast hero tint brings the cold Home download to 64613 gzip bytes with the window-mode fast alert restored; owner-authorized 2026-10-17.
// The onboarding choice-card and summary styles in the shared stylesheet bring it to 64906; owner-requested 2026-10-18.
// The shot chart's real-stop dot styles in the shared stylesheet bring it to 64931; owner-requested 2026-10-18.
// Keeping /setup off the UI stream and the compatibility redirect brings it to 64964; owner-reported 2026-10-18.
// The Home Micra cloud hint card, its copy and its Settings deep link bring it to 65263; owner-requested 2026-10-18.
// The Linea Micra sign-in state, Save/Revert pairing and its CSS rule bring it to 65311; owner-requested 2026-10-18.
// The La Marzocco Cloud sign-in steps CSS and legend state strings bring it to 65442; owner-requested 2026-10-18.
// The green La Marzocco Cloud linked note CSS and its shorter text bring it to 65473; owner-requested 2026-10-18.
// The La Marzocco Cloud state pill CSS and its state labels bring it to 65559; owner-requested 2026-10-18.
// The Home Cup accessory state line brings it to 65685; owner-requested 2026-10-18.
// The La Marzocco Cloud Account label string brings it to 65687; owner-requested 2026-10-18.
// The Diagnostic Micra WebSocket link label string brings it to 65695; owner-requested 2026-10-18.
// The Home Equipment device icons, status badges and headline values bring it to 65962; owner-requested 2026-10-18.
// The Micra machine row dash with Turn on to brew / Refill the tank notes brings it to 65986; owner-requested 2026-10-18.
// The Home Micra machine row Starting up note brings it to 66002; owner-requested 2026-10-18.
// The Home shot card flow, its full-screen fit and touch-hold gating bring it to 66188; owner-requested 2026-10-19.
// The Home shot card flow fading from live to its average brings it to 66304; owner-requested 2026-10-19.
// The Home shot card flow line and fill with a dashed drip tail brings it to 66389; owner-requested 2026-10-19.
// The Home shot card smoothed flow area and taller full-screen chart bring it to 66410; owner-requested 2026-10-19.
// The Home shot card live flow averaged over its last second brings it to 66445; owner-requested 2026-10-19.
// The Home shot card firmware clock in both scale modes with its ±0.1 s final swap brings it to 66520; owner-requested 2026-10-19.
// Shot card field deltas merged over the previous card bring it to 66535; owner-requested 2026-10-20.
// The Stats shot card drip cell's shared CSS brings it to 66549; owner-requested 2026-10-20.
// The drip compensation chart CSS, its renderer hook and review fixes bring it to 66803; owner-requested 2026-10-20.
// The Stats shot card cut and drip delay cells' shared CSS bring it to 66835; owner-requested 2026-10-20.
// The Stats consistency columns' shared CSS brings it to 67047 (67032 under test web); owner-requested 2026-10-20.
// The drip chart ten-slot window measures Cold Home at 67051; owner-requested 2026-10-20.
// The Stats consistency clipped-column notch CSS brings it to 67062; owner-requested 2026-10-20.
// The Home preset rules peek and its graph chevron bring it to 67235; owner-requested 2026-10-20.
// Shot curves decoded from the firmware's delta stream bring it to 67543; owner-requested 2026-10-20.
// Home and Diagnostic increments continue the snapshot identity brings it to 67571; owner-requested 2026-10-21.
// The build tag in the shell (gzip wobble) brings it to 67572; owner-requested 2026-10-21.
// Stream closed after 5 minutes hidden and resumed without snapshots brings it to 67640; owner-requested 2026-10-21.
// The ?fw= landing refreshes the cached shell after an OTA brings it to 67696; owner-requested 2026-10-21.
// A reconnect keeps the presets it already holds brings it to 67797; owner-requested 2026-10-21.
// Stats rows as column-ordered values brings it to 67829; owner-requested 2026-10-21.
// Review fixes for the hidden close, setup sessions and the 4002 reconnect brings it to 67869; owner-requested 2026-10-21.
// Browser storage keys renamed from the legacy ss prefix to obbw bring it to 67885; rename owner-requested 2026-10-21.
// Review fixes RV-03 (History sort pill look), RV-04 and RV-07 bring it to 67895.
// The optional Web UI login (shell sign-in form and pairing prompt, runtime session and sign-in code,
// Home notice, CSS) brings it to 70097, owner-requested 2026-10-22; cap 70120 for asset-tag gzip wobble.
// Footer Sign out (shell, runtime, CSS) brings it to 70203, owner-requested 2026-10-22; cap 70220.
// The device list's shared CSS and the sign-in device label bring it to 70660, owner-requested 2026-10-22; cap 70680.
// The device list rename and separator CSS bring it to 70707; cap 70730.
// The Settings group row CSS brings it to 70898, owner-requested 2026-10-22; cap 70920.
// The shared group-row helpers and the Admin card CSS bring it to 71080; the Diagnostic
// log-stream resume brings it to 71130, owner-requested 2026-10-22. The outlined History
// cup brings it to 71172, owner-requested 2026-10-22. The group switch, setting row and
// sunken open-group CSS brings it to 71371, owner-requested 2026-10-22. The band's bottom
// row fit and the minifier-safe group transition bring it to 71421. The joined-box
// number keys and dropdown key bring it to 71872, owner-requested 2026-10-22. The Home
// equipment rows on the sunken band bring it to 71901, owner-requested 2026-10-22. The group
// animation fallback (runtime and CSS) brings it to 72145, owner-requested 2026-10-22. Animated
// equipment rows and unfolded-only preset graphs bring it to 72174, owner-requested 2026-10-22; cap 72190.
// The Settings scope notes, Account group and outlined History icon CSS bring it to 72183,
// owner-requested 2026-10-22; cap 72210. The gap-only stream keepalive, hidden pause flag
// and held Stats/History rows bring it to 72254, owner-requested 2026-10-23; cap 72280. The
// shared list filter menu, its shell icon, and the Stats window bar bring it to 73206,
// owner-requested 2026-10-23; cap 73230.
// Rejected-op (4003) recovery: 73288, owner-requested 2026-10-23; cap 73300.
// Shot-card Target temp, Dose, and Ratio: 73373, owner-requested 2026-10-24;
// cap 73380.
if (coldHomeGzip > 73380) {
  throw new Error(`Cold Home download exceeds the 73380-byte gzip budget (${coldHomeGzip})`);
}
// The optional Web UI login brings every embedded asset together to 149453,
// owner-requested 2026-10-22; cap 149480 for asset-tag gzip wobble. The Admin
// deep link from Home's access note measures 149526; cap 149550. One password
// form at a time in Admin → Access measures 149591; cap 149610. The Admin
// factory-password reminder measures 149645; cap 149670.
// Footer Sign out, Admin row removed: 149686; cap 149700.
// The signed-in device list: 151068; cap 151090.
// Inline device renaming: 151355; cap 151380.
// Settings group rows (icons, live summaries, row CSS): 153049, owner-requested 2026-10-22; cap 153080.
// Admin section rows (icons, live summaries, card CSS): 154072, owner-requested 2026-10-22; cap 154100.
// Diagnostics section rows with icons: 154901; the Diagnostic log-stream resume brings it to
// 154942, owner-requested 2026-10-22.
// Fixed Administration unlock section: 155002, owner-requested 2026-10-22; cap 155010.
// Access password notes (the approved admin and CSS raises): 155040, owner-requested
// 2026-10-23; cap 155040.
// Shot-card Target temp, Dose, and Ratio (CSS and JS raises): 155359,
// owner-requested 2026-10-24; cap 155360. The audit's metric-typography
// range fix re-measured 155373; cap 155380.
if (generated.combined > 155380) {
  throw new Error(`Combined Web UI gzip exceeds the 155380-byte flash budget (${generated.combined})`);
}
if (!network.includes('#include "OpenBrewByWeightWebAssetsGzip.h"') ||
    network.includes('#include "OpenBrewByWeightWebAssets.h"')) {
  throw new Error('Firmware must embed the gzip Web UI, not the HTML source string');
}
if (!network.includes('OPEN_BREW_BY_WEIGHT_WEB_UI_GZIP') ||
    !network.includes('OPEN_BREW_BY_WEIGHT_WEB_UI_GZIP_LEN') ||
    !network.includes('for (const WebAsset &asset : WEB_ASSETS)') ||
    !generatedHeader.includes('const WebAsset WEB_ASSETS[] = {') ||
    generatedHeader.includes('_PARTIAL_') ||
    !network.includes('WEB_UI_ETAG') ||
    network.includes('OPEN_BREW_BY_WEIGHT_WEB_PARTIAL_HOME_GZIP') ||
    network.includes('OPEN_BREW_BY_WEIGHT_WEB_VIEW_HOME_GZIP') ||
    network.includes('formatWebUiEtag') ||
    network.includes('OPEN_BREW_BY_WEIGHT_WEB_LOGO_GZIP') ||
    !network.includes('"Content-Encoding"') ||
    !network.includes('"gzip"')) {
  throw new Error('GET / and the WEB_ASSETS table must send precompressed bodies without logo');
}
if (network.includes('zlib.h') || network.includes('miniz.h') ||
    /mz_compress|deflateInit|gzipCompress/.test(network)) {
  throw new Error('Firmware must not compress the Web UI at runtime');
}
if (!network.includes('sendBody(request, OPEN_BREW_BY_WEIGHT_WEB_UI_GZIP') ||
    !network.includes('return sendBody(request, asset.data, asset.length)') ||
    !network.includes('const esp_err_t sent = sendBody(request, json, length)') ||
    !network.includes(
        'return httpd_resp_send(request, static_cast<const char *>(data), length)') ||
    !network.includes(
        'return httpd_resp_send_chunk(request, static_cast<const char *>(data), length)') ||
    network.includes('HTTP_DRAM_BOUNCE_BYTES') ||
    network.includes('g_httpSendBounce') ||
    // The bounded write override is exclusive to authenticated WS sessions.
    !network.includes('httpd_sess_set_send_override(request->handle, session->fd, uiStreamSend)') ||
    !network.includes('allocExternal(sizeof(NetworkWorkBuf), AllocationOwner::NETWORK)') ||
    !psram.includes('inline void *allocExternal(size_t bytes,') ||
    !jsonArena.includes('parseJsonDocument') ||
    !jsonArena.includes('AllocationOwner::JSON')) {
  throw new Error(
      'Complete HTTP bodies must use HTTPD Content-Length responses; streamed bodies retain chunks and large work buffers live in PSRAM heap');
}
if (!network.includes('If-None-Match')) {
  throw new Error('GET / must honor If-None-Match for cached Web UI revalidation');
}
const rootHandlerStart = network.indexOf('esp_err_t OpenBrewByWeightNetwork::rootHandler');
const assetHandlerStart = network.indexOf('esp_err_t OpenBrewByWeightNetwork::assetHandler');
const notFoundHandlerStart = network.indexOf('esp_err_t OpenBrewByWeightNetwork::notFoundHandler');
const serveImmutableStart = network.indexOf('static esp_err_t serveWebAsset');
if (rootHandlerStart < 0 || assetHandlerStart < 0 || notFoundHandlerStart < 0 ||
    statusHandlerStart < 0 || serveImmutableStart < 0 ||
    network.includes('logoHandler') ||
    network.includes('loginHandler') ||
    !(serveImmutableStart < rootHandlerStart && rootHandlerStart < assetHandlerStart &&
      assetHandlerStart < notFoundHandlerStart && notFoundHandlerStart < statusHandlerStart)) {
  throw new Error('serveWebAsset/rootHandler/assetHandler/notFoundHandler order not found');
}
const rootHandler = network.slice(rootHandlerStart, assetHandlerStart);
const serveImmutable = network.slice(serveImmutableStart, rootHandlerStart);
const assetHandler = network.slice(assetHandlerStart, notFoundHandlerStart);
const notFoundHandler = network.slice(notFoundHandlerStart, statusHandlerStart);
if (rootHandler.includes('no-store') ||
    !rootHandler.includes('max-age=86400') ||
    !rootHandler.includes('stale-if-error=604800') ||
    !rootHandler.includes('STATUS_NOT_MODIFIED') ||
    !rootHandler.includes('ifNoneMatchEquals') ||
    !rootHandler.includes('ETag') ||
    !rootHandler.includes("style-src 'self'") ||
    !rootHandler.includes("script-src 'self'") ||
    rootHandler.includes('"Connection"') ||
    rootHandler.includes("script-src 'unsafe-inline'") ||
    rootHandler.includes('HTTPD_RESP_USE_STRLEN')) {
  throw new Error('GET / must serve a cacheable keep-alive shell with ETag/304, CSP script/style self, and gzip by length');
}
// Only the inline keep-awake video may load as media; no outside origin.
if (!rootHandler.includes("media-src data:;") || /media-src[^;]*(\*|https?:|'self')/.test(rootHandler))
  throw new Error('GET / may allow only data: media, for the full-screen keep-awake video');
if (!serveImmutable.includes('requestsCurrentAssetTag(request)') ||
    !network.includes('strcmp(query + 3, WEB_UI_ASSET_TAG) == 0') ||
    !serveImmutable.includes('current ? "public, max-age=31536000, immutable" : "no-store"') ||
    !serveImmutable.includes('max-age=31536000') ||
    !serveImmutable.includes('immutable') ||
    !serveImmutable.includes('STATUS_NOT_MODIFIED') ||
    serveImmutable.includes('"Connection"')) {
  throw new Error('Immutable gzip assets must long-cache only their current ?v= tag (else no-store) with ETag/304 on kept-alive connections');
}
// One exact route per WEB_ASSETS row, every row typed and only PNG rows raw;
// the icon probe rows serve the real artwork, never a redirect.
const assetRows = [...generatedHeader.matchAll(
    /^    \{"([^"]+)", "([^"]+)", (\w+), \w+_LEN, (true|false)\},$/gm)];
if (assetRows.map((row) => row[1]).join() !== webUi.ASSET_PATHS.join() ||
    assetRows.some((row) => (row[4] === 'true') === (row[2] === 'image/png')) ||
    webUi.ASSET_PATHS.some((assetPath) =>
      !network.includes(`registerHandler(server_, "${assetPath}", HTTP_GET, assetHandler)`)) ||
    (network.match(/HTTP_GET, assetHandler\)/g) || []).length !== webUi.ASSET_PATHS.length ||
    !assetHandler.includes('strcspn(request->uri, "?")') ||
    !assetHandler.includes("asset.path[pathLength - 1] == '*'") ||
    !assetHandler.includes('return serveWebAsset(request, asset)') ||
    !assetHandler.includes('return notFoundHandler(request, HTTPD_404_NOT_FOUND)') ||
    !serveImmutable.includes('if (asset.gzip)') ||
    assetHandler.includes('302 Found') || assetHandler.includes('Location')) {
  throw new Error('Web assets must be served from the generated WEB_ASSETS table on exact routes');
}
if (network.includes('logoHandler') || network.includes('OPEN_BREW_BY_WEIGHT_WEB_LOGO')) {
  throw new Error('Firmware must not serve /logo.svg');
}
if (!notFoundHandler.includes('302 Found') ||
    !notFoundHandler.includes('Location') ||
    !notFoundHandler.includes('"/api/"') ||
    !notFoundHandler.includes('STATUS_NOT_FOUND')) {
  throw new Error('Unknown non-API routes must 302 to /, while unknown /api/* stay JSON 404');
}
if (network.includes('sendJson') &&
    !network.slice(network.indexOf('esp_err_t OpenBrewByWeightNetwork::sendJson'),
                   network.indexOf('esp_err_t OpenBrewByWeightNetwork::sendError'))
        .includes('no-store')) {
  throw new Error('JSON API responses must remain Cache-Control: no-store');
}
if (!network.includes('requireActiveWebUiClient') ||
    network.includes('heartbeatHandler') ||
    network.includes('/api/v1/heartbeat') ||
    networkHeader.includes('WEB_PADDLE_HEARTBEAT_TIMEOUT_MS') ||
    network.includes('WEB_PADDLE_HEARTBEAT_TIMEOUT_MS') ||
    network.includes('heartbeatStopSent_')) {
  throw new Error(
      'WebUI claim must replace POST /heartbeat; web paddle heartbeat circuit timeout must be gone');
}
const wifiScanStatusStart =
    network.indexOf('esp_err_t OpenBrewByWeightNetwork::wifiScanStatusHandler');
if (wifiScanStatusStart < 0 ||
    !network.slice(wifiScanStatusStart).includes('"Connection"') ||
    !network.slice(wifiScanStatusStart, wifiScanStatusStart + 800)
         .includes('"close"')) {
  throw new Error('Chunked Wi-Fi scan status responses must send Connection: close');
}
if (!codeIncludes(js, 'withPollGate(async()=>{if(scanBusy||!webUiPollingActive())return;scanBusy=true') ||
    !codeIncludes(js, "withCommandGate(async()=>{try{await api('/api/v1/stats/clear'") ||
    !codeIncludes(js, "withCommandGate(async()=>{try{await api('/api/v1/stats/delete'") ||
    !codeIncludes(js, "await api('/api/v1/stats/rate'") ||
    codeIncludes(js, "withCommandGate(async()=>{try{await api('/api/v1/logout'")) {
  throw new Error('Wi-Fi scan and shot clear/delete must use poll/command gates without login');
}

{
  const start = js.indexOf('function buildRuleChartModel(');
  const end = js.indexOf('function updateRuleChartFromStatus(');
  if (start < 0 || end < 0 || end <= start) {
    throw new Error('Rule chart model helpers not found for matrix checks');
  }
  const presetState = {cutRampS: 5};
  const helpers = new Function('presetState', 'sub',
      js.slice(start, end) +
      ';return{buildRuleChartModel:buildRuleChartModel,cutLine:cutLine,paceStartNote:paceStartNote};')(
      presetState, (s, v) => s.replace('{0}', v[0]));
  const base = {
    brewByWeight: true,
    fastExtractionGuardEnabled: true,
    slowExtractionGuardEnabled: true,
    operationalWallMs: 50000,
    fastBeforeS: 28,
    slowAfterS: 44,
    bbwProtectionMs: 12000,
    goalWeightG: 36,
    fastMarginG: 6.5,
    slowMarginG: 2,
  };
  const off = helpers.buildRuleChartModel({...base, brewByWeight: false});
  if (off.mode !== 'timerOnly') {
    throw new Error('Rule chart: BBW off must be timerOnly');
  }
  const both = helpers.buildRuleChartModel(base);
  if (both.mode !== 'active' || !both.fast || !both.slow ||
      both.t1 !== 28 || both.t2 !== 44 || both.prot !== 12 || both.wall !== 50 ||
      both.goal !== 36 || both.floor !== 34 || both.ceil !== 42.5 || both.d !== 5) {
    throw new Error('Rule chart: every limit must come from settings, never baked in');
  }
  // The cut line holds the ceiling until t1 - D, ramps to the target at t1,
  // holds it through the window and ramps down to the floor by t2 + D.
  const line = (t) => helpers.cutLine(t, both);
  if (line(12) !== 42.5 || line(23) !== 42.5 || line(28) !== 36 || line(44) !== 36 ||
      line(49) !== 34 || line(50) !== 34 || !(line(25.5) < 42.5 && line(25.5) > 36) ||
      line(25.5) !== 39.2 || !(line(46.5) < 36 && line(46.5) > 34)) {
    throw new Error('Rule chart: the cut line must follow the firmware smoothstep law');
  }
  const none = helpers.buildRuleChartModel({
    ...base, fastExtractionGuardEnabled: false,
    slowExtractionGuardEnabled: false
  });
  if (none.fast !== false || none.slow !== false || none.mode !== 'active' ||
      none.ceil !== 36 || none.floor !== 36 || helpers.cutLine(20, none) !== 36) {
    throw new Error('Rule chart: both sides off must cut at the target');
  }
  // Paces start at the preset's usual first drop, at least 0.5 s before the
  // window, and the scrubber never picks an arrival before that start.
  const own = helpers.buildRuleChartModel({...base, firstDropS: 6.5, firstDropN: 4});
  const late = helpers.buildRuleChartModel({...base, firstDropS: 15});
  const past = helpers.buildRuleChartModel({...base, firstDropS: 31, firstDropN: 2});
  if (own.firstDrop !== 6.5 || own.scrubMin !== 12 || late.firstDrop !== 15 ||
      late.scrubMin !== 15.5 || past.firstDrop !== 27.5 || past.scrubMin !== 28 ||
      both.firstDrop !== 0 || both.scrubMin !== 12) {
    throw new Error('Rule chart: the pace start must follow the first drop, kept before the window');
  }
  const notes = [own, late, both].map(helpers.paceStartNote);
  if (!notes[0].includes('6.5') || !notes[1].includes('15.0') || notes[1] === notes[0] ||
      notes[2].includes('{0}') || new Set(notes).size !== 3) {
    throw new Error('Rule chart: the pace start note must name its source');
  }
  for (const bad of [{bbwProtectionMs: undefined}, {fastBeforeS: 12}, {slowAfterS: 28},
    {operationalWallMs: 44000}]) {
    if (helpers.buildRuleChartModel({...base, ...bad}).mode !== 'empty') {
      throw new Error('Rule chart: an incomplete or out-of-order recipe must render nothing');
    }
  }
  presetState.cutRampS = undefined;
  if (helpers.buildRuleChartModel(base).mode !== 'empty') {
    throw new Error('Rule chart: the ramp length comes from the firmware');
  }
  // The graph's cut line reproduces the firmware kernel's golden vectors
  // exactly. legacy_step (D = 0) is the old step oracle; the firmware always
  // serves its 5 s ramp to the UI.
  const vectors = JSON.parse(fs.readFileSync(
      path.join(sketchDir, 'tests/fixtures/corridor_vectors_v1.json'), 'utf8'));
  let compared = 0;
  for (const [name, p] of Object.entries(vectors.presets)) {
    if (!p.rampMs) continue;
    presetState.cutRampS = p.rampMs / 1000;
    const m = helpers.buildRuleChartModel({brewByWeight: true,
      fastExtractionGuardEnabled: p.fastOn, slowExtractionGuardEnabled: p.slowOn,
      goalWeightG: p.goalDg / 10, fastMarginG: p.fastMarginDg / 10,
      slowMarginG: p.slowMarginDg / 10, fastBeforeS: p.fastBeforeMs / 1000,
      slowAfterS: p.slowAfterMs / 1000, bbwProtectionMs: p.delayMs,
      operationalWallMs: p.wallMs});
    if (m.mode !== 'active') throw new Error(`Rule chart: vector preset ${name} must be drawable`);
    for (const [tMs, dg] of vectors.cutLine[name]) {
      if (tMs < p.delayMs || tMs > p.wallMs) continue;  // the graph starts at the delay
      const got = Math.round(helpers.cutLine(tMs / 1000, m) * 10);
      ++compared;
      if (got !== dg)
        throw new Error(`Rule chart: ${name} at ${tMs} ms draws ${got} dg, firmware ${dg} dg`);
    }
  }
  if (compared < 500) throw new Error(`Rule chart: only ${compared} golden points compared`);
  const empty = helpers.buildRuleChartModel(null);
  if (empty.mode !== 'empty') {
    throw new Error('Rule chart: no config must render nothing');
  }
}

{
  const start = runtimeJs.indexOf('function axisLabel(');
  const end = runtimeJs.indexOf('async function populateTimezoneOptions(');
  if (start < 0 || end < 0 || end <= start) {
    throw new Error('Shot spark helpers not found for matrix checks');
  }
  const helpers = new Function(
      '"use strict";' + runtimeJs.slice(start, end) + statsChartJs +
      ';const R={axisLabel,buildShotSparkModel}' +
      ';return{buildShotSparkModel:buildShotSparkModel,shotDisplayFlowGS:shotDisplayFlowGS,fillChartTicks:fillChartTicks};')();
  if (helpers.buildShotSparkModel(null) !== null) {
    throw new Error('Spark model must tolerate a null shot (idle Home panel)');
  }
  const kids = [];
  const tickEl = {
    replaceChildren(){kids.length=0},
    appendChild(n){kids.push(n)},
  };
  const prevDoc = global.document;
  global.document = {
    createElement(){return {className:'',style:{},textContent:''}},
  };
  try {
    helpers.fillChartTicks(tickEl, [[0, '0 g'], [18, '18 g'], [36, '36 g']], 36);
    const spaced = kids.map((n) => parseFloat(n.style.left));
    if (kids.length !== 3 || kids[0].textContent !== '0 g' ||
        kids.some((n) => n.className !== 'ruleTick') ||
        spaced[0] !== 0 || Math.abs(spaced[1] - 50) > 0.01 || spaced[2] !== 100) {
      throw new Error('fillChartTicks must place labels at exact values');
    }
    helpers.fillChartTicks(tickEl, [[0, '0 g'], [36, '36 g'], [18, '18 g']], 36);
    if (kids.map((n) => n.textContent).join('|') !== '0 g|18 g|36 g') {
      throw new Error('fillChartTicks must sort labels by position');
    }
    helpers.fillChartTicks(tickEl, [[0, '0 g'], [35.5, '35.5 g'], [36, '36 g']], 36);
    const close = kids.map((n) => parseFloat(n.style.left));
    if (kids.map((n) => n.textContent).join('|') !== '0 g|35.5 g|36 g' ||
        Math.abs(close[1] - 35.5 / 36 * 100) > 0.01 || close[2] !== 100) {
      throw new Error('fillChartTicks must keep close labels on exact values');
    }
  } finally {
    if (prevDoc === undefined) delete global.document;
    else global.document = prevDoc;
  }
  const assert = require('assert');
  const model = helpers.buildShotSparkModel;
  const close = (a,b) => assert(Math.abs(a-b)<1e-9, `${a} != ${b}`);
  const trace = (times,extra={}) => ({wAtMs:times,wCg:times.map(t=>t/5),durationS:times.at(-1)/1000,...extra});
  const rates = m => m.flowCurve.filter(v=>v!==null);
  for (const cadence of [50,100,137,200,250,300,500,750,1000,1500]) {
    const times=Array.from({length:Math.floor(60000/cadence)+1},(_,i)=>i*cadence);
    const shot=trace(times),m=model(shot),first=times.findIndex(t=>t>=1000);
    assert.equal(m.flowCurve.length,times.length);
    assert.equal(rates(m).length,times.length-first);
    rates(m).forEach(q=>close(q,2));close(m.maxFlow,2);
    close(m.flowSegs[0].pts[0].t,(times[first]-Math.max(1000,cadence)/2)/1000);
    assert.deepEqual(shot.wAtMs,times,'input observations must stay unchanged');
  }
  for (const times of [
    [0,90,210,490,810,1120,1441,1600,2010,2300],
    [0,50,50,51,99,100,499,500,999,1000,1001,1500,2000],
    [...Array.from({length:21},(_,i)=>i*100),2500,3000,3500,4000,4100,4200,4300,4400,4500,4600,4700,4800,4900,5000],
  ]) {
    const m=model(trace(times));
    rates(m).forEach(q=>close(q,2));close(m.maxFlow,2);
    m.flowSegs.flatMap(s=>s.pts).forEach(p=>close(p.cg,200));
  }
  // Independent boundary interpolation on a non-linear trace.
  const interpolated=model({wAtMs:[0,90,210,490,810,1120],wCg:[0,10,50,90,110,210],durationS:1.12});
  assert.deepEqual(interpolated.flowCurve,[null,null,null,null,null,1.9]);
  assert.deepEqual(interpolated.flowSegs[0].pts,[{t:.62,cg:190}]);
  const one=model(trace([0,500,1000]));
  assert.deepEqual(one.flowCurve,[null,null,2]);
  assert.deepEqual(one.flowSegs[0].pts,[{t:.5,cg:200}],'one estimate remains one point');
  assert.equal(model(trace([100,500,999])).maxFlow,null);
  assert.equal(model(trace([100,500,999])).flowSegs.length,0);
  const slow=model(trace([0,1500,3000,6500]));
  assert.deepEqual(slow.flowSegs[0].pts.map(p=>p.t),[.75,2.25,4.75]);
  assert.deepEqual(slow.flowCurve,[null,2,2,2]);
  const breakSlow=model(trace([0,1500,3000,4500],{wBreakBefore:[2]}));
  assert.deepEqual(breakSlow.flowCurve,[null,2,null,2]);
  assert.equal(breakSlow.flowSegs.length,2,'a slow valid span must not cross a marked break');
  const drop=model(trace([0,900,1100,1900,2100],{dropS:1,dropCg:5000}));
  assert.deepEqual(drop.flowCurve,[null,null,null,null,2],'interpolation left support must follow first drop');
  const dropExact=model(trace([0,1000,2000],{dropS:1,dropCg:9999}));
  assert.deepEqual(dropExact.flowCurve,[null,null,2],'event weight must never enter rate');
  const duplicate=model({wAtMs:[0,500,1000,1000,1500,2000],wCg:[0,100,200,300,400,500],durationS:2});
  assert.deepEqual(duplicate.flowCurve,[null,null,null,3,3,2]);
  const duplicateBreak=model(trace([0,500,1000,1000,1000,1500,2000],{wBreakBefore:[3]}));
  assert.deepEqual(duplicateBreak.flowCurve,[null,null,2,null,null,null,2]);
  assert.equal(duplicateBreak.flowSegs.length,2);
  const duplicateStart=model({wAtMs:[0,0,500,1000],wCg:[0,100,200,300],durationS:1});
  assert.deepEqual(duplicateStart.flowCurve,[null,null,null,2]);
  const recovery=model(trace([0,500,1000,1500,2000,2500,3000],{wBreakBefore:[3]}));
  assert.deepEqual(recovery.flowCurve,[null,null,2,null,null,2,2]);
  assert.equal(recovery.flowSegs.length,2,'rejection/tare/provenance markers restart full support');
  const invalid=model(trace([0,500,1000,1500,2000,2500,3000],{wCg:[0,100,200,NaN,400,500,600]}));
  assert.deepEqual(invalid.flowCurve,[null,null,2,null,null,null,2]);
  assert.equal(invalid.flowSegs.length,2);
  for(const malformed of [
    {wCg:[0,100],wAtMs:[0]}, {wCg:[0,100],wAtMs:[500,0]},
    {wCg:[0,100],wAtMs:[0,NaN]}, {wCg:[0,100],wAtMs:[0,100.5]},
    {wCg:[0,100],wAtMs:[0,60001]}, {wCg:[0,100],wAtMs:[0,1000],wBreakBefore:[2]},
    {wCg:[0,100],wAtMs:[0,1000],wBreakBefore:[1,1]},
    {wCg:[0,100],wDtS:1},
  ]) assert.equal(model({...malformed,durationS:2}),null,'no implicit legacy grid or reordered input');
  const final=model(trace([0,500,1000,1500,2000],{endS:1.5,endCg:10000,durationS:2}));
  assert.deepEqual(final.flowCurve,[null,null,2,2,2]);
  assert.equal(final.maxFlow,2);
  assert.equal(final.flowSegs[0].pts.at(-1).t,1.5);
  assert.equal(final.dripFlowSegs[0].pts[0].t,1.5);
  const preciseEnd=model(trace([0,500,1000,1550,1570,1571],{endS:1.57,endCg:9999,durationS:1.5}));
  assert.deepEqual(preciseEnd.flowCurve,[null,null,2,2,2,2],'exact cutoff keeps extraction and drip estimates in CSV');
  assert.equal(preciseEnd.dripFlowSegs.length,0,'no midpoint after cutoff means no invented drip line');
  assert.equal(preciseEnd.pts.some(p=>p.t===1.55),true);
  assert.deepEqual(model(trace([0,500,1001],{endS:1.001})).flowCurve,[null,null,2],
    'decimal seconds must round back to exact integer milliseconds');
  const preciseDrop=model(trace([0,1000,1050,2000,2050],{dropS:1.05,firstDropS:1}));
  assert.deepEqual(preciseDrop.flowCurve,[null,null,null,null,2],
    'exact curve drop takes priority over coarse scalar time and excludes earlier support');
  const decimalDrop=model(trace([2007,2507,3007],{dropS:2.007}));
  assert.deepEqual(decimalDrop.flowCurve,[null,null,2],'decimal rounding cannot reject support exactly at first drop');
  const falling=model({wAtMs:[0,500,1000,1500],wCg:[200,100,0,0],durationS:1.5});
  assert.deepEqual(falling.flowCurve,[null,null,0,0]);assert.equal(falling.maxFlow,0);
  const slope=model({wAtMs:[0,500,1000,1500,2000],wCg:[0,50,100,250,400],durationS:2});
  assert.deepEqual(slope.flowCurve,[null,null,1,2,3]);
  assert.deepEqual(slope.flowSegs[0].pts.map(p=>p.cg),[100,200,300],'no additional chart smoothing');
  const spike=model({wAtMs:[0,500,1000,1500,2000],wCg:[0,0,500,0,0],durationS:2});
  assert.deepEqual(spike.flowCurve,[null,null,5,0,0]);assert.equal(spike.maxFlow,5);
  // Weight is on-recipe green until the guard fires, then takes the guard's color;
  // flow is always on-recipe blue.
  for (const [verdict,color] of [['fast','var(--gFast)'],['slow','var(--gSlow)']]) {
    const colored=model(trace([0,500,1000,1500,2000,2500],{extendedS:1.2,extCg:9000,verdict}));
    assert.deepEqual(colored.flowCurve,[null,null,2,2,2,2]);
    assert.equal(colored.maxFlow,2);
    assert.ok(colored.flowSegs.length>0&&colored.flowSegs.every(s=>s.color==='var(--gFlow)'));
    colored.flowSegs.flatMap(s=>s.pts).forEach(p=>close(p.cg,200));
    const line=colored.segs.filter(s=>s.pts.length>1);
    assert.deepEqual(line.map(s=>s.color),['var(--ok)',color],'guard color starts at extendedS');
    close(line[1].pts[0].t,1.2);
    assert.ok(model(trace([0,500,1000,1500],{verdict})).segs.every(s=>s.color==='var(--ok)'),
      'no guard fired: the whole weight line stays on-recipe');
  }
  const atm=model(trace([0,500,1000,1500,2000,2500,3000,3500,4000],{atmS:1.2,atmCg:200,atmClearedS:2.2}));
  assert.deepEqual(atm.flowCurve,[null,null,2,null,null,null,null,2,2]);
  assert.equal(atm.flowSegs.length,2);
  const atmBetween=model(trace([0,1000,3000,4000],{atmS:1.2,atmClearedS:2.2}));
  assert.deepEqual(atmBetween.flowCurve,[null,2,null,2],'crossing A→M without interior readings still breaks support');
  const capacity=trace(Array.from({length:1201},(_,i)=>i*50));
  const full=model(capacity);assert.equal(rates(full).length,1181);assert.equal(full.truncated,false);
  assert.equal(full.pts.length,1201);close(full.flowSegs[0].pts.at(-1).t,59.5);
  const truncated=model({...capacity,wTruncated:true,endS:65,endCg:15000,durationS:65});
  assert.equal(truncated.truncated,true);assert.equal(truncated.maxFlow,2);
  close(truncated.flowSegs[0].pts.at(-1).t,59.5);
  assert.equal(model(trace(Array.from({length:1202},(_,i)=>i*49))),null);
  const flow = helpers.shotDisplayFlowGS({
    avgFlowGS: null,
    actualG: 43.7,
    durationS: 15.1,
    firstDropS: 4.5,
  });
  if (!(flow > 4.12 && flow < 4.13)) {
    throw new Error('History flow fallback must use actual/(duration-firstDrop)');
  }
}

// Over-the-air update safety contract. The controller lives inside a closed
// machine, so every rule below exists to keep a bad image from becoming the
// boot image, and a bad boot from becoming permanent.
{
  const ota = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightOta.cpp'), 'utf8');
  const otaHeader = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightOta.h'), 'utf8');
  const otaImage = fs.readFileSync(
    path.join(sketchDir, 'OpenBrewByWeightOtaImage.h'), 'utf8');
  // The generator, not its git-ignored output, so a fresh checkout is checked too.
  const version = fs.readFileSync(
    path.join(sketchDir, '..', 'scripts', 'gen_version.sh'), 'utf8');

  // The transfer must target the spare slot, never the one that is executing.
  if (!ota.includes('esp_ota_get_next_update_partition(nullptr)') ||
      !ota.includes('target != running')) {
    throw new Error('OTA must stage into the inactive slot only');
  }
  // Any early exit has to release the OTA handle; a leaked handle would keep
  // the flash driver's state machine open until the next reboot.
  if (!ota.includes('esp_ota_abort(') || !ota.includes('esp_ota_end(') ||
      !ota.includes('clearSession(true)')) {
    throw new Error('OTA must abort or end every esp_ota_begin handle');
  }
  // The boot selection changes in exactly one place, after re-reading the
  // staged identity straight from flash.
  if ((ota.match(/esp_ota_set_boot_partition\(/g) || []).length !== 1 ||
      !/if \(!reconfirmStagedTag\(\)\) \{[\s\S]{0,400}?esp_ota_set_boot_partition\(target\)/
          .test(ota)) {
    throw new Error(
      'OTA must re-verify the staged image immediately before switching the boot partition');
  }
  // Rolling back must never reboot on its own: machine circuit has to be opened first.
  if (ota.includes('esp_ota_mark_app_invalid_rollback_and_reboot') ||
      ota.includes('ESP.restart') || ota.includes('esp_restart')) {
    throw new Error('OTA must not restart the controller directly; machine circuit is opened first');
  }
  if (!ota.includes('rejected_ = true') ||
      !/if \(rejected_\) \{\s*return false;/.test(ota)) {
    throw new Error(
      'rejectRunningImage must record the rejection so confirmRunningImage cannot cancel it');
  }
  if (!css.includes('.hidden,[hidden]{display:none!important}')) {
    throw new Error('The shared hidden rule must override displayed controls including OTA progress');
  }
  if (!ota.includes('esp_ota_check_rollback_is_possible()')) {
    throw new Error(
      'OTA must confirm a bootable alternative exists before arming a rollback');
  }
  // The OTA object is spliced into the admin status, so an unclosed brace here
  // would make the whole Admin page unparseable, not just the OTA panel.
  if (!/const size_t tagCapacity = capacity - 1;/.test(network) ||
      !/if \(used \+ 2 > capacity\) \{[\s\S]{0,200}?"\{\\"otaProtocolVersion\\":%u,\\"available\\":false\}"/
        .test(network)) {
    throw new Error(
      'buildOtaJson must reserve room for its closing brace and fall back to a ' +
      'valid object when the OTA JSON does not fit');
  }
  if (!otaHeader.includes('OTA_PROTOCOL_VERSION = 3') ||
      !network.includes('\\"otaProtocolVersion\\":%u') ||
      !network.includes('\\"runningIdentityValid\\":%s') ||
      !network.includes('\\"sessionArch\\":\\"%s\\"') ||
      !network.includes('\\"sessionVersion\\":\\"%s\\"')) {
    throw new Error(
      'OTA status must expose protocol and complete resumable identity');
  }
  if (!codeIncludes(js, 'function otaRemoteMatches(') ||
      (js.match(/!(?:R\.)?otaRemoteMatches\(session,\s*status\)/g) || []).length < 2) {
    throw new Error(
      'Web OTA must bind session creation and reconciliation to the requested transferId');
  }
  if (!network.includes('sessionHardware') ||
      !network.includes('sessionMachine') ||
      !codeIncludes(js, 'status.sessionHardware===identity.hardware') ||
      !codeIncludes(js, 'status.sessionMachine===identity.machine')) {
    throw new Error(
      'OTA sessions must bind hardware and machine profile compatibility');
  }
  // Recovering an image too broken to run any firmware code is the bootloader's
  // job, so losing that configuration must break the build, not the machine.
  for (const symbol of ['CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE',
                        'CONFIG_APP_ROLLBACK_ENABLE',
                        'CONFIG_BOOTLOADER_WDT_ENABLE']) {
    if (!new RegExp(`#if !defined\\(${symbol}\\)\\s*\\n#error`).test(otaHeader)) {
      throw new Error(`OTA header must fail the build when ${symbol} is missing`);
    }
  }
  // A build that lost its identity marker could never be verified by the next
  // one, and would also accept a foreign image.
  if (!version.includes('FW_IMAGE_TAG_STRING') ||
      !version.includes('FW_BOARD_ARCH_STRING') ||
      !ota.includes('FW_IMAGE_TAG_STRING')) {      throw new Error('Firmware must embed the Open Brew by Weight OTA image tag');
  }
  // The needle is assembled at run time so a compiled image contains exactly
  // one contiguous copy of the prefix: its own tag, never the search pattern.
  if (otaImage.includes('"OPENBREWBYWEIGHT_FW_TAG_V2|"') ||
      !otaImage.includes('OTA_TAG_PREFIX_PART_1') ||
      !otaImage.includes('OTA_TAG_PREFIX_PART_2')) {
    throw new Error('OTA tag prefix must stay split so it is not embedded contiguously');
  }
  if (!otaHeader.includes('PENDING_VERIFY') ||
      !ota.includes('if (!confirmed_) {')) {
    throw new Error(
      'OTA must refuse to stage while the running image is still pending verification');
  }

  // The Arduino core would confirm the image inside initArduino(), before this
  // firmware has proven anything.
  if (!/extern "C" bool verifyRollbackLater\(\) \{\s*return true;/.test(firmware)) {
    throw new Error('Firmware must defer OTA rollback verification to the network layer');
  }
  if (!firmware.includes('OpenBrewByWeightOta::instance().begin()')) {
    throw new Error('Firmware must initialise the OTA module during setup');
  }

  // The unsafe WebUI override may relax settings, never a firmware update.
  const otaHandlers = network.slice(network.indexOf('OpenBrewByWeightNetwork::otaStatusHandler'));
  if (otaHandlers.includes('webUiOverrideAllowed') ||
      otaHandlers.includes('webUiConfigurationAllowed')) {
    throw new Error('OTA handlers must not honour the unsafe WebUI override');
  }
  if ((otaHandlers.match(/authorizeOtaRequest\(request\)/g) || []).length !== 6) {
    throw new Error('Every OTA route must authenticate the request');
  }
  if (ota.includes('OTA_SAFETY_CHECK_INTERVAL_BYTES') ||
      !/while \(failure == OtaResult::OK && rangeReceived < contentLength\) \{\s*\n\s*\/\/ Shot always wins[\s\S]{0,400}?if \(!io\.stillSafe\(io\.context\)\) \{/
          .test(ota)) {
    throw new Error(
        'OTA must abort on every chunk when the paddle moves, not on a byte interval');
  }
  if (!network.includes('otaTransferStillSafe') ||
      !/bool OpenBrewByWeightNetwork::otaTransferStillSafe[\s\S]{0,400}?controlAllowsConfiguration/
          .test(network)) {
    throw new Error(
        'OTA stillSafe must keep controlAllowsConfiguration so a paddle pull aborts the transfer');
  }
  const restartHandler = network.slice(
      network.indexOf('OpenBrewByWeightNetwork::restartHandler'),
      network.indexOf('OpenBrewByWeightNetwork::firmwareModeHandler'));
  if (restartHandler.includes('unsafeWebUiOverride') ||
      restartHandler.includes('webUiConfigurationAllowed') ||
      restartHandler.includes('CONFIG_LOCKED_DURING_ACTIVE_CYCLE')) {
    throw new Error(
        'Admin restart must queue during a shot and must not use the unsafe override to cut it');
  }
  if (!firmwareCore.includes('holdOrBeginPlannedRestart') ||
      !firmwareCore.includes('plannedRestartHeld') ||
      !firmwareCore.includes('servicePendingPlannedRestart')) {
    throw new Error(
        'Planned restart must be held until idle instead of machineRequestStop on an active shot');
  }
  {
    const rebootCase = firmwareCore.slice(
        firmwareCore.indexOf('case SerialCliVerb::REBOOT:'),
        firmwareCore.indexOf('case SerialCliVerb::UNKNOWN:'));
    if (!rebootCase.includes('serialCliQueueCommand') ||
        rebootCase.includes('serialCliQueueIfSafe')) {
      throw new Error(
          'Serial REBOOT must queue during a shot and wait, not reject as not-ready');
    }
  }
  if (!firmwareCore.includes('const bool faultRestart') ||
      !firmwareCore.includes(
          '!session.active && !relayCircuitClosed()')) {
    throw new Error(
        'Planned ESP.restart must wait for an open circuit; only fault restarts cut a shot');
  }
  {
    const rollback = network.slice(
        network.indexOf('void OpenBrewByWeightNetwork::serviceOtaRollback'));
    if (!/control\.activeCycle\s*\|\|\s*control\.machineRunning\s*\|\|\s*control\.physicalActivatorOn,\s*control\.relayClosed/.test(rollback) ||
        !otaSource.includes('if (activeCycle || relayClosed || busy_ || sessionActive_)')) {
      throw new Error(
          'OTA confirm/rollback must not write otadata while machine activity is present');
    }
  }
  if (!html.includes('Paddle during upload cancels it') ||
      !html.includes('A running shot finishes first')) {
    throw new Error(
        'Web UI must say the paddle aborts OTA and that restart waits for the shot');
  }
  if (!codeIncludes(js, "Flashed. Restart waits until the shot ends.") ||
      codeIncludes(runtimeJs, 'Locked while the machine is busy') ||
      !codeIncludes(js, 'Waiting for idle (') ||
      !codeIncludes(runtimeJs, 'Restart after the shot.')) {
    throw new Error(
        'OTA status must wait for the shot, not claim the machine is locked');
  }
  {
    const applyOta = runtimeJs.slice(
        runtimeJs.indexOf('function applyOtaStatus'),
        runtimeJs.indexOf('function otaSend'));
    if (applyOta.includes('stopButton') || applyOta.includes('rinseButton') ||
        applyOta.includes('paddleButton') || applyOta.includes('startButton')) {
      throw new Error('OTA busy must not disable Start or Rinse');
    }
  }
  if (!network.includes(
          'The paddle moved or a shot started during the upload')) {
    throw new Error(
        'SAFETY_LOST must tell the operator the paddle aborted the upload');
  }
  if (!network.includes('secretsMatch(password, expected)')) {
    throw new Error(
      'OTA authentication must compare the device password in constant time');
  }
  if (network.includes('tokenAuthorized = !factoryPassword && otaTokensMatch') ||
      network.includes('passwordAuthorized = !factoryPassword') ||
      (network.includes('authorizeOtaRequest') &&
       network.slice(network.indexOf('OpenBrewByWeightNetwork::authorizeOtaRequest'),
                     network.indexOf('OpenBrewByWeightNetwork::buildOtaJson'))
           .includes('if (factoryPassword)'))) {
    throw new Error(
      'OTA authentication must accept the factory default device password');
  }
  const currentDeviceAt = html.indexOf('id="currentDevicePassword"');
  const newDeviceAt = html.indexOf('id="newDevicePassword"');
  const confirmDeviceAt = html.indexOf('id="confirmDevicePassword"');
  if (currentDeviceAt >= 0) {
    throw new Error('Device password form must not ask for the current password after admin unlock');
  }
  if (newDeviceAt < 0 || confirmDeviceAt < 0 || !(newDeviceAt < confirmDeviceAt)) {
    throw new Error('Device password form must ask for the new password and confirmation');
  }
  if (!codeIncludes(js, "newPassword:$('newDevicePassword').value") ||
      !network.includes('"newPassword"') ||
      network.includes('"currentPassword"') ||
      !network.includes('DEVICE_PASSWORD_INVALID') ||
      !network.includes('requireAdminUnlock(request)')) {
    throw new Error('Web device password change must require admin unlock, not the current password');
  }
  if (!html.includes('id="adminLockPanel"') ||
      !html.includes('id="adminUnlockPassword"') ||
      !html.includes('id="adminUnlockButton"') ||
      !html.includes('Unlock administration') ||
      !html.includes('id="adminControls"') ||
      !html.includes('15 minutes after the last privileged action') ||
      !html.includes('Lock closes it now') ||
      !html.includes('id="adminLockButton"') ||
      html.includes('id="navAdminLock"') ||
      html.includes('id="homeAdminLock"') ||
      !html.includes('this window will confirm automatically') ||
      html.includes('unlock to confirm') ||
      codeIncludes(js, 'Unlock to confirm') ||
      codeIncludes(js, 'unlock to confirm') ||
      !codeIncludes(js, 'function lockAdminUi()') ||
      !codeIncludes(js, 'function lockAdmin()') ||
      !codeIncludes(js, "clearTimeout(scanTimer);scanTimer=0;api('/api/v1/admin/lock'") ||
      !codeIncludes(js, 'function syncAdminSessionUi(unlocked,remoteEnabled=false)') ||
      !codeIncludes(js, 'stopViewPolls();lockAdminUi();setMutable(false)') ||
      !codeIncludes(js, '/api/v1/admin/unlock') ||
      !codeIncludes(js, '/api/v1/admin/lock') ||
      !codeIncludes(js, "closest('#adminLockPanel") ||
      !css.includes('.hidden,[hidden]{display:none!important}') ||
      !css.includes('.textLock') ||
      !network.includes('/api/v1/admin/unlock') ||
      !network.includes('/api/v1/admin/lock') ||
      !network.includes('OpenBrewByWeightNetwork::adminLockHandler') ||
      !network.includes('ADMIN_LOCKED') ||
      !network.includes('ADMIN_UNLOCK_COOLDOWN') ||
      !network.includes('ADMIN_UNLOCK_IDLE_MS') ||
      !network.includes('grantAdminUnlock') ||
      !network.includes('secretsMatch(password, expected)') ||
      codeIncludes(js, 'loginHandler') ||
      network.includes('OpenBrewByWeightNetwork::loginHandler')) {
    throw new Error('Admin must gate behind a temporary device-password unlock on firmware and UI');
  }
  if (!html.includes('id="diagnosticControls"') ||
      !codeIncludes(js, 'showDiagnosticPage') ||
      !codeIncludes(js, 'diagnosticPublic') ||
      !codeIncludes(viewJs.admin, "saveDurableConfigKey('showDiagnosticPage','the Diagnostic page setting'") ||
      !codeIncludes(viewJs.admin, 'await waitSaved(a.requestId,label,p,key)') ||
      !codeIncludes(viewJs.admin, 'R.withBaseRev({[key]:wanted})') ||
      codeIncludes(viewJs.admin, 'waitDiagnosticPagePersisted') ||
      (viewJs.admin.match(/\bapplyStatus\b/g) || []).length !== 2 ||
      codeIncludes(js, "$('diagnosticUnlockButton').onclick") ||
      network.includes('DIAGNOSTIC_DISABLED') ||
      // Only diagnostic mutations (NET-04) may consult the toggle; the page
      // and its stream stay reachable either way.
      network.split('!self.diagnosticPageEnabled()').length !== 2 ||
      !network.includes('matched->diagnostic && !self.diagnosticPageEnabled()') ||
      !network.includes(
          'self.diagnosticPageEnabled() ? "true" : "false"') ||
      !network.includes('showDiagnosticPage') ||
      !firmwareCore.includes(
          'candidate.showDiagnosticPage = command.config.showDiagnosticPage')) {
    throw new Error(
        'Diagnostic must stay reachable at /diagnostic; the Admin-controlled toggle only hides the menu entry');
  }
  if (codeIncludes(runtimeJs, 'row.innerHTML') ||
      codeIncludes(runtimeJs, "shotType[0]!=='a'|y<1") ||
      codeIncludes(runtimeJs, 'pollChain=run.catch(()=>{})') ||
      !codeIncludes(runtimeJs, 'pollChain=run.catch(console.warn)')) {
    throw new Error(
        'Shot history must use DOM textContent, logical OR in stats, and must not swallow poll errors');
  }
  {
    const start = firmwareCore.indexOf('void completeBootRecovery(');
    const end = firmwareCore.indexOf('void resumePendingBootRecovery(');
    const body = start >= 0 && end > start ? firmwareCore.slice(start, end) : '';
    if (!body.includes('ensureRecoveryIntent') ||
        !body.includes('abandonFailedBootRecovery') ||
        !body.includes('(void)clearRecoveryIntent()') ||
        !body.includes('bootRecoveryShouldRestartAfterSuccess') ||
        firmwareCore.includes('void holdFailedBootRecovery') ||
        !firmwareCore.includes('void abandonMalformedRecoveryIntent()') ||
        !firmwareCore.includes('PendingRecoveryKind::MALFORMED')) {
      throw new Error(
          'Boot recovery must apply or abandon the latch and never hang');
    }
  }
  {
    const factoryFnStart = network.indexOf('OpenBrewByWeightNetwork::factoryResetHandler');
    const passwordFnStart = network.indexOf('OpenBrewByWeightNetwork::devicePasswordHandler');
    const restartFnStart = network.indexOf('OpenBrewByWeightNetwork::restartHandler');
    const bleFnStart = network.indexOf('OpenBrewByWeightNetwork::bleScanHandler');
    const timeFnStart = network.indexOf('OpenBrewByWeightNetwork::timeSyncHandler');
    for (const [label, start] of [
      ['factory-reset', factoryFnStart],
      ['device-password', passwordFnStart],
      ['restart', restartFnStart],
      ['ble-scan', bleFnStart],
      ['time-sync', timeFnStart],
      ['wifi-scan-start', network.indexOf('OpenBrewByWeightNetwork::wifiScanStartHandler')],
      ['wifi-scan-status', network.indexOf('OpenBrewByWeightNetwork::wifiScanStatusHandler')],
      ['paddle', network.indexOf('OpenBrewByWeightNetwork::paddleHandler')],
      ['rinse', network.indexOf('OpenBrewByWeightNetwork::rinseHandler')],
      ['stop', network.indexOf('OpenBrewByWeightNetwork::stopHandler')]
    ]) {
      if (start < 0 ||
          !network.slice(start, start + 500).includes('requireAdminUnlock(request)')) {
        throw new Error(label + ' must require admin unlock');
      }
    }
    const unlockFnStart = network.indexOf('OpenBrewByWeightNetwork::unlockHandler');
    if (unlockFnStart < 0 ||
        !network.slice(unlockFnStart, unlockFnStart + 600).includes('requireAdminUnlock(request)')) {
      throw new Error('ui/unlock must require admin unlock');
    }
    const networkFn = network.slice(
        network.indexOf('OpenBrewByWeightNetwork::networkHandler'),
        network.indexOf('OpenBrewByWeightNetwork::wifiScanStartHandler'));
    if (!networkFn.includes('requireAdminUnlock(request)') ||
        !networkFn.includes('strcmp(action, "confirm") != 0')) {
      throw new Error('Network save/forget must require admin unlock; confirm may stay ungated');
    }
    if (networkFn.includes('settingsCopy()')) {
      throw new Error(
          'networkHandler must not return PersistedSettings by value on the httpd stack');
    }
  }
  if ((statusFormat.match(/\\"adminUnlocked\\":%s/g) || []).length < 1) {
    throw new Error('status/admin must report adminUnlocked');
  }
  if (!network.includes('delta.field("adminUnlocked"') ||
      !network.includes('delta.field("diagnosticPublic"') ||
      !network.includes('delta.field("development"')) {
    throw new Error('the Diagnostic stream must identify public and development access');
  }
  if (!network.includes('delta.field("adminUnlocked"') ||
      !codeIncludes(ui, "v==='home'?!!(typeof s.adminUnlocked==='boolean'") ||
      !codeIncludes(js, 'function syncAdminSessionUi(unlocked,remoteEnabled=false)') ||
      !codeIncludes(js, 'syncAdminSessionUi(admin,remoteReady)') ||
      !codeIncludes(js, 'updateHomeAdminActions(on,remoteEnabled)')) {
    throw new Error('Home Actions must require adminUnlocked and remoteControlEnabled');
  }
  if (!network.includes('page == StatusPage::Admin') ||
      !network.includes('self.touchAdminUnlock()')) {
    throw new Error(
        'Home status must report adminUnlocked without sliding idle; Admin polls may renew');
  }
  {
    const start = network.indexOf('OpenBrewByWeightNetwork::adminLockHandler');
    const end = network.indexOf('bool OpenBrewByWeightNetwork::requireActiveWebUiClient');
    const body = start >= 0 && end > start ? network.slice(start, end) : '';
    if (!body.includes('clearAdminUnlock()') ||
        body.includes('requireAdminUnlock')) {
      throw new Error('Admin lock must clear unlock without requiring a still-valid unlock');
    }
  }
  if (!statusFormat.includes('!adminUnlocked') ||
      !codeIncludes(js, 's.adminUnlocked')) {
    throw new Error('Locked admin status must omit Wi-Fi/BLE/OTA bodies until unlocked');
  }
  if (!fs.readFileSync(path.join(__dirname, '../../scripts/localize_web_ui.js'), 'utf8')
          .includes('development-class') ||
      !fs.readFileSync(path.join(__dirname, '../../scripts/gen_web_ui.js'), 'utf8')
          .includes('developmentMode')) {
    throw new Error(
        'the generator must resolve the shell development-class marker from the developmentMode option');
  }
  if (!network.includes('serviceOtaRollback(now)') ||
      !otaSource.includes('decideOtaPendingVerify(') ||
      !network.includes(
          'scaleConnectingOrUp_.load(std::memory_order_relaxed)') ||
      !otaHeader.includes('decideOtaPendingVerify') ||
      !otaHeader.includes('bool flashWriteSafe = true') ||
      !otaHeader.includes('KEEP_RUNNING') ||
      !networkHeader.includes('OTA_CONFIRM_MIN_UPTIME_MS') ||
      !networkHeader.includes('OTA_CONFIRM_DEADLINE_MS')) {
    throw new Error(
        'Network service must confirm, roll back, or KEEP_RUNNING a pending OTA image without writing otadata during GATT');
  }
  if (!otaSource.includes('RejectResult::NO_ALTERNATIVE') ||
      !otaSource.includes('confirmRunningImageLocked()')) {
    throw new Error(
        'OTA must keep the running image when rollback is impossible');
  }
  for (const code of [
    'OTA_UPLOAD_STARTED', 'OTA_IMAGE_STAGED', 'OTA_UPLOAD_REJECTED',
    'OTA_FLASH_COMMITTED', 'OTA_IMAGE_CONFIRMED', 'OTA_ROLLBACK_ARMED',
    'OTA_ROLLBACK_FAILED'
  ]) {
    if (!domain.includes(`DebugCode::${code}`)) {
      throw new Error(`Diagnostic log must report OTA event: ${code}`);
    }
  }

  // Admin page layout: the update lives between Restart and Factory reset.
  const restartAt = html.indexOf('id="restartPanel"');
  const otaAt = html.indexOf('id="otaPanel"');
  const factoryAt = html.indexOf('id="factoryResetButton"');
  if (restartAt < 0 || otaAt < 0 || factoryAt < 0 ||
      !(restartAt < otaAt && otaAt < factoryAt)) {
    throw new Error('Admin OTA panel must sit between Restart and Factory reset');
  }
  for (const id of [
    'otaStatus', 'otaRunning', 'otaStaged', 'otaFile',
    'otaProgress', 'otaVerifyButton', 'otaFlashButton', 'otaDiscardButton'
  ]) {
    if (!html.includes(`id="${id}"`)) {
      throw new Error(`Admin OTA panel is missing control: ${id}`);
    }
  }
  if (html.includes('id="otaToken"') || codeIncludes(js, 'otaGuardToken') ||
      codeIncludes(js, 'X-OTA-Token') || codeIncludes(js, 'X-Device-Password') ||
      codeIncludes(js, 'OTA token') || codeIncludes(js, 'ota password')) {
    throw new Error(
      'Web UI OTA must not collect a separate password after admin unlock');
  }
  // OTA rides the shared API client, which carries the Web UI client and
  // session headers.
  if (!codeIncludes(js, 'options.headers=webUiHeaders(options.headers)') ||
      !codeIncludes(js, '{[WEB_UI_CLIENT_HEADER]:webUiClientId}') ||
      !codeIncludes(viewJs.admin, 'return R.api(path,{method,body,headers,timeoutMs})') ||
      !network.includes('adminUnlockAllowed(request)') ||
      !network.includes('secretsMatch(password, expected)') ||
      !network.includes('X-Device-Password')) {
    throw new Error(
      'OTA must accept an admin unlock session or the device password header');
  }
  // Two steps: verify writes the spare slot, a separate button flashes it.
  if (!codeIncludes(js, "otaSend('/api/v1/ota/session'") ||
      !codeIncludes(js, "otaSend('/api/v1/ota',chunk") ||
      !codeIncludes(js, "'PATCH',headers") || !codeIncludes(js, 'otaFileIdentity') ||
      !codeIncludes(js, "otaSend('/api/v1/ota/flash'") ||
      !codeIncludes(js, "otaSend('/api/v1/ota/abort'") ||
      !codeIncludes(js, "$('otaFlashButton').disabled=!ready||!staged")) {
    throw new Error('Web UI must upload/verify and flash as two separate steps');
  }
  if (!codeIncludes(js, '!otaBusy&&Date.now()-lastStatusAt')) {
    throw new Error('Web UI must pause status polling during a firmware transfer');
  }
}

console.log(
  `Embedded Web UI: modules valid, ${htmlBytes} bytes HTML / ${jsBytes} bytes JS source, ` +
  `${generated.gzip.length} bytes shell gzip, ${generated.jsGzip.length} bytes app.js gzip, ` +
  `${generated.runtimeGzip.length} bytes runtime gzip, ${generated.otaImageGzip.length} bytes OTA image gzip, ` +
  `view modules ${Object.entries(generated.moduleGzip).map(([f, b]) => f + ' ' + b.length).join(', ')} bytes gzip, ` +
  `${generated.cssGzip.length} bytes CSS gzip, combined ${generated.combined} bytes gzip, ` +
  `${expected.size} routes checked`
);
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
{
  const handler = (name) => {
    const start = network.indexOf('esp_err_t OpenBrewByWeightNetwork::' + name);
    return network.slice(start, network.indexOf('\nesp_err_t OpenBrewByWeightNetwork::', start + 1));
  };
  // Mutating OTA routes run on the worker; status reads never wait for it.
  for (const name of ['otaPatchHandler', 'otaFlashHandler', 'otaAbortHandler']) {
    if (!handler(name).includes('runOnOtaWorker(request, ')) {
      throw new Error(name + ' must run its OTA work on the OTA worker');
    }
  }
  if (!handler('otaSessionHandler').includes('HTTP_GET') ||
      !network.includes('ota.snapshot(false)') ||
      !/otaWorkerStopping_\.store\(true\);[\s\S]*otaJobsPending_\.load\(\) != 0[\s\S]*httpd_stop\(server_\)/.test(network) ||
      !network.includes('httpd_req_async_handler_complete(job.request)')) {
    throw new Error('OTA status must stay on httpd without blocking, and httpd_stop must wait for OTA jobs');
  }
  const session = handler('otaSessionWork');
  const abort = handler('otaAbortWork');
  if (session.indexOf('expireSession(') < 0 ||
      session.indexOf('expireSession(') > session.indexOf('sendOtaSnapshot(') ||
      !network.includes('ota.expireSession(now, controlAllowsConfiguration(control), false)') ||
      abort.indexOf('CONFIG_LOCKED_DURING_ACTIVE_CYCLE') < 0 ||
      abort.indexOf('controlAllowsConfiguration') > abort.indexOf('discard()')) {
    throw new Error('OTA sessions must expire on the polled session route and the network tick, ' +
                    'and abort must respect the control gate');
  }
}
