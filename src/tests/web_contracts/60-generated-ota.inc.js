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
  new Function('$', 'R', 'document', 'rangeCheck', blockAt(viewJs.settings, 'function validateMachineClient(') +
      ';return validateMachineClient();')(field, {
    number: (id) => Number(field(id).value), formNumber: () => 1},
  {documentElement: {classList: {contains: (c) => typeClasses.includes(c)}}}, (id) => (field(id).value, ''));
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
  const context = {SHELL_FW_VERSION: '2.0', uiState: {}, hasUnsavedEdits: () => false,
    location: {search: '?fw=2.0', pathname: '/stats'},
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
if (!generated.html.includes('rel="manifest" href="/manifest.webmanifest?v=' + generated.assetTag + '"') ||
    !generated.html
        .includes('href="/icons/icon-192.png?v=' + generated.assetTag + '"') ||
    !generated.html.includes('rel="apple-touch-icon"') ||
    !generated.html.includes('name="theme-color"') ||
    !generated.html.includes('name="apple-mobile-web-app-capable"')) {
  throw new Error('Generated shell must link the PWA manifest, icon, and home-screen metadata');
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
    !compactSource(serveImmutable).includes(compactSource(
        'worker ? "no-cache" : current ? "public, max-age=31536000, immutable" : "no-store"')) ||
    !serveImmutable.includes('const bool worker = strcmp(asset.path, "/sw.js") == 0;') ||
    !serveImmutable.includes('const bool current = !worker && requestsCurrentAssetTag(request);') ||
    !serveImmutable.includes('max-age=31536000') ||
    !serveImmutable.includes('immutable') ||
    !serveImmutable.includes('STATUS_NOT_MODIFIED') ||
    serveImmutable.includes('"Connection"')) {
  throw new Error('Immutable gzip assets must long-cache only their current ?v= tag (else no-store), /sw.js must revalidate (no-cache), both with ETag/304 on kept-alive connections');
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
      // Diagnostic reads are always public: no flag gates them.
      codeIncludes(js, 'diagnosticPublic') || network.includes('diagnosticPublic') ||
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
    if (networkFn.includes('workingSettings()')) {
      throw new Error(
          'networkHandler must not touch the network task working settings from httpd');
    }
  }
  if ((statusFormat.match(/\\"adminUnlocked\\":%s/g) || []).length < 1) {
    throw new Error('status/admin must report adminUnlocked');
  }
  if (!network.includes('delta.field("adminUnlocked"') ||
      !network.includes('delta.field("development"')) {
    throw new Error('the Home stream, which Diagnostic reads, must identify unlocked and development access');
  }
  if (!network.includes('delta.field("adminUnlocked"') ||
      !codeIncludes(ui, "v==='home'?!!(typeof s.adminUnlocked==='boolean'") ||
      !codeIncludes(js, 'function syncAdminSessionUi(unlocked,remoteEnabled=false)') ||
      !codeIncludes(js, 'syncAdminSessionUi(admin,remoteReady)') ||
      !codeIncludes(js, 'updateHomeAdminActions(on,remoteEnabled)')) {
    throw new Error('Home Actions must require adminUnlocked and remoteControlEnabled');
  }
  if (!network.includes('statusPageIsAdmin(request->uri)') ||
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
  `Embedded Web UI: modules valid, ${expected.size} routes checked`);
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
