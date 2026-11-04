#!/usr/bin/env node
'use strict';

const fs = require('fs');
const {execFileSync} = require('child_process');
const path = require('path');
const crypto = require('crypto');
const zlib = require('zlib');
const {minify: terserMinify} = require('terser');
const {minify: htmlMinify} = require('html-minifier-terser');
const CleanCSS = require('clean-css');
const {transform: cssTransform} = require('lightningcss');
const zopfli = require('@gfx/zopfli');
const {renderSources, CSS_SOURCES} = require('./localize_web_ui.js');

const repoRoot = path.resolve(__dirname, '..');
const jsDir = path.join(repoRoot, 'src', 'web', 'js');
const htmlDir = path.join(repoRoot, 'src', 'web', 'html');
const sourcePath = path.join(htmlDir, 'shell.html');
const appJsPath = path.join(repoRoot, 'src', 'web', 'app.js');
const otaImageJsPath = path.join(jsDir, 'ota-image.js');
const launchJsPath = path.join(jsDir, 'launch.js');
const swPath = path.join(repoRoot, 'src', 'web', 'sw.js');
const cssDir = path.join(repoRoot, 'src', 'web', 'css');
// CSS_SOURCES (localize_web_ui.js) owns the layer order; this map is the
// shipped order. The guard below fails the build when src/web/css holds a
// file outside the list, so no stylesheet can sit in the tree yet silently
// never ship.
const cssSourcePaths = CSS_SOURCES.map((name) => path.join(cssDir, name));

function assertCssSourcesComplete() {
  const present = fs.readdirSync(cssDir, {recursive: true})
      .filter((name) => name.endsWith('.css')).sort().join(',');
  if (present !== [...CSS_SOURCES].sort().join(',')) {
    throw new Error(`src/web/css contents [${present}] do not match the CSS_SOURCES list`);
  }
}
const manifestPath = path.join(repoRoot, 'src', 'web', 'manifest.webmanifest');
const icon192Path = path.join(repoRoot, 'resources', 'icons', 'android',
    'launchericon-192x192.png');
// The browser-tab favicon is the brand mark on a transparent background, so
// tab bars show the drop itself instead of a square plate. A thin cream rim
// keeps enough contrast that Safari's dark tab bar does not add its own light
// plate behind the low-luminance terracotta. The opaque 192 px artwork stays
// reserved for home-screen icons.
const icon48Path = path.join(repoRoot, 'resources', 'icons', 'favicon-48.png');
const versionPath = path.join(repoRoot, 'src', 'OpenBrewByWeightVersion.h');
// Firmware builds pass --output inside their tree; the default serves the
// Web contracts and stays out of src/, whose .cpp files are all firmware units.
const outputPath =
    path.join(repoRoot, 'build-host', 'webui', 'OpenBrewByWeightWebAssetsGzip.h');

const VIEW_NAMES = ['home', 'stats', 'history', 'diagnostic', 'settings', 'admin', 'setup'];
// Each lazy view ships as one module that carries its own markup, so opening
// a view costs one request. Stats and History are small and visited
// together, so they share the records bundle; the rest load on their own.
const VIEW_MODULES = {
  records: ['stats', 'history'],
  diagnostic: ['diagnostic'],
  admin: ['admin'],
  settings: ['settings'],
  setup: ['setup'],
};
// A bundle of several views carries a shared prelude, js/<bundle>.js, that the
// generator places at the bundle's module scope ahead of its scoped views.
const SHARED_JS = Object.keys(VIEW_MODULES).filter((file) => VIEW_MODULES[file].length > 1);
const JS_FILES = [...VIEW_NAMES, ...SHARED_JS];
const JS_TYPE = 'application/javascript; charset=utf-8';
// Served paths of the WEB_ASSETS rows, in table order; the firmware registers
// one exact route per path.
const ASSET_PATHS = ['/app.js', '/app.css', '/js/runtime.js', '/js/ota-image.js', '/js/launch.js',
  ...Object.keys(VIEW_MODULES).map((file) => `/js/${file}.js`), '/sw.js',
  '/manifest.webmanifest', '/icons/icon-192.png', '/favicon.ico', '/apple-touch-icon*'];
const WEB_UI_CSP = "default-src 'self'; script-src 'self'; style-src 'self'; " +
    "connect-src 'self'; media-src data:; frame-ancestors 'none'; " +
    "form-action 'self'; base-uri 'none'";
// Paths the firmware answers with the shell: the keys of app.js ROUTES, which
// the service worker also matches navigations against.
const SHELL_PATHS = [...fs.readFileSync(appJsPath, 'utf8')
    .match(/^const ROUTES = \{\n([\s\S]*?)^\};/m)[1].matchAll(/^ {2}"(\/[a-z]*)":/gm)]
    .map((m) => m[1]);

// Machine-type exclusive markup classes. Sources stay type-agnostic and hide
// excluded elements via CSS at runtime; typed builds strip them at generation
// so a build only ships the UI of the machine type it was compiled for.
const MACHINE_TYPE_EXCLUSIONS = {
  paddle: ['momentaryOnly', 'reedOnly', 'switchOnly'],
  momentary: ['paddleOnly', 'reedOnly'],
  momentary_reed: ['paddleOnly', 'switchOnly'],
};
// Builds without the Linea Micra cloud integration drop its markup and CSS;
// the JS that drives it folds away with __LINEA_MICRA__.
const LINEA_MICRA_EXCLUSIONS = ['micraOnly', 'micraTempOpt'];

// Classes whose markup and CSS a build strips, for its options.
function excludedClasses(options) {
  return [...(MACHINE_TYPE_EXCLUSIONS[options.machineType] || []),
    ...(options.lineaMicra === false ? LINEA_MICRA_EXCLUSIONS : [])];
}

function stripClassMarkup(html, excluded) {
  let out = html;
  for (const cls of excluded) {
    const openRe = new RegExp(
        `<([a-z]+)\\b[^>]*\\bclass="[^\"]*\\b${cls}\\b[^\"]*"[^>]*>`, 'g');
    const removals = [];
    let match;
    // Remove each matching element's whole subtree by scanning to the tag's
    // matching close; a first-close regex leaves stray closers that break the
    // surrounding layout.
    while ((match = openRe.exec(out))) {
      const tokenRe = new RegExp(`<(/?)${match[1]}\\b[^>]*>`, 'gi');
      tokenRe.lastIndex = match.index + match[0].length;
      let depth = 1;
      let token;
      while ((token = tokenRe.exec(out))) {
        depth += token[1] === '/' ? -1 : 1;
        if (depth === 0) break;
      }
      if (depth === 0 && token) {
        removals.push([match.index, tokenRe.lastIndex - match.index]);
        // A nested match lies inside this subtree and goes with it.
        openRe.lastIndex = tokenRe.lastIndex;
      }
    }
    for (let i = removals.length - 1; i >= 0; i--) {
      out = out.slice(0, removals[i][0]) +
          out.slice(removals[i][0] + removals[i][1]);
    }
  }
  return out;
}

// Drop only the selectors that name stripped markup, including entries of an
// :is() list, so a rule grouped with them keeps its other selectors.
function keepSelectors(list, re) {
  return list.split(/,(?![^()]*\))/)
      .filter((selector) => !re.test(selector.replace(/:is\([^()]*\)/g, '')))
      .map((selector) => selector.replace(/:is\(([^()]*)\)/g,
          (_, inner) => `:is(${keepSelectors(inner, re).join(',')})`))
      .filter((selector) => !selector.includes(':is()'));
}

function stripCssSelectors(css, re) {
  const rules = css.replace(/\/\*[\s\S]*?\*\//g, '');
  return rules.replace(/([^{}]+)(\{[^{}]*\})/g, (rule, prelude, body) => {
    if (!re.test(prelude)) return rule;
    const kept = keepSelectors(prelude, re);
    return kept.length ? kept.join(',') + body : '';
  });
}

function stripClassCss(css, excluded) {
  if (!excluded.length) return css;
  return stripCssSelectors(css, new RegExp(`\\.(?:${excluded.join('|')})\\b`));
}

// Remote machine control is opt-in at compile time; builds without the flag
// must not embed the fixed Home action bar (rinse/start/stop/force pulse) or
// its layout CSS, which only ever shows for remote-enabled firmware.
const REMOTE_CONTROL_CSS_RE = /#actionsPanel|homeAdminActions/;

function stripRemoteControlMarkup(html) {
  return html.replace(
      /<fieldset\b[^>]*\bid="actionsPanel"[^>]*>[\s\S]*?<\/fieldset\s*>/g, '');
}

function stripRemoteControlCss(css) {
  return stripCssSelectors(css, REMOTE_CONTROL_CSS_RE);
}

function readFirmwareVersion() {
  if (!fs.existsSync(versionPath)) {
    return 'dev';
  }
  const source = fs.readFileSync(versionPath, 'utf8');
  const match = source.match(/FW_VERSION_STRING\s*"([^"]+)"/) ||
      source.match(/FW_VERSION\[\]\s*=\s*"([^"]+)"/);
  return match ? match[1] : 'dev';
}

async function minifyHtml(html) {
  return htmlMinify(html, {
    collapseWhitespace: true,
    removeComments: true,
    collapseBooleanAttributes: true,
    removeRedundantAttributes: true,
    minifyCSS: false,
    minifyJS: false,
  });
}

// The oldest browsers the Web UI supports are the first with :has(). For them
// Lightning CSS adds the vendor prefixes the sources leave out and lowers newer
// syntax (range media queries, light-dark()).
const CSS_TARGETS = {safari: (15 << 16) | (4 << 8), ios_saf: (15 << 16) | (4 << 8),
  chrome: 105 << 16, firefox: 121 << 16};

// Lightning CSS re-serializes every token, so the shipped bytes do not depend
// on how the source is formatted; clean-css level 2 then restructures that
// canonical form. Values it cannot parse (custom properties, var() math) keep
// a formatter's space after commas, which is never significant outside strings.
function minifyCss(css) {
  const {code, warnings} = cssTransform({filename: 'app.css', code: Buffer.from(css), minify: true,
    targets: CSS_TARGETS});
  const tight = code.toString().replace(/("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')|,\s+/g,
      (match, string) => string || ',');
  const out = new CleanCSS({level: 2}).minify(tight);
  const errors = [...warnings.map((w) => w.message), ...out.errors];
  if (errors.length) {
    throw new Error('CSS minify failed: ' + errors.join('; '));
  }
  return out.styles;
}

// Build-profile flags are compile-time constants: Terser folds the guarded
// branches away, so a build ships no code for features it was compiled
// without (remote machine control, the Linea Micra cloud integration).
async function minifyJs(js, flags = {}) {
  const result = await terserMinify(js, {
    module: true,
    compress: {passes: 2, global_defs: {
      __REMOTE_CONTROL__: flags.remoteControl !== false,
      __LINEA_MICRA__: flags.lineaMicra !== false,
    }},
    mangle: true,
    format: {comments: false},
    ecma: 2018,
  });
  if (!result || !result.code) {
    throw new Error('Terser failed to minify Web UI JavaScript');
  }
  return result.code;
}

// Zopfli takes most of a generation, and builds and Web contract variants
// recompress mostly unchanged assets, so its output is cached by input bytes,
// options and zopfli version. A hit is used only if it inflates to the input.
const ZOPFLI_OPTIONS = {numiterations: 15};
// Worktrees share the main checkout's cache: its .git is their common dir.
function sharedRoot() {
  try {
    return path.dirname(path.resolve(repoRoot, execFileSync(
        'git', ['rev-parse', '--git-common-dir'], {cwd: repoRoot, encoding: 'utf8'}).trim()));
  } catch {
    return repoRoot;
  }
}
const zopfliCacheDir = path.join(sharedRoot(), 'build-host', 'zopfli-cache');
const zopfliCacheSalt = `${require('@gfx/zopfli/package.json').version}\0` +
    `${JSON.stringify(ZOPFLI_OPTIONS)}\0`;

function inflatesTo(gzip, buffer) {
  try {
    return zlib.gunzipSync(gzip).equals(buffer);
  } catch {
    return false;
  }
}

function gzipBuffer(buffer) {
  const key = crypto.createHash('sha256').update(zopfliCacheSalt).update(buffer).digest('hex');
  const file = path.join(zopfliCacheDir, `${key}.gz`);
  const cached = fs.existsSync(file) && fs.readFileSync(file);
  if (cached && inflatesTo(cached, buffer)) return Promise.resolve(cached);
  return new Promise((resolve, reject) => {
    zopfli.gzip(buffer, ZOPFLI_OPTIONS, (err, out) => {
      if (err) return reject(err);
      const result = Buffer.from(out);
      // Builds and contracts run in parallel: publish each entry atomically.
      fs.mkdirSync(zopfliCacheDir, {recursive: true});
      const temporary = `${file}.${process.pid}.tmp`;
      fs.writeFileSync(temporary, result);
      fs.renameSync(temporary, file);
      resolve(result);
    });
  });
}

function formatByteArray(buffer) {
  const lines = [];
  for (let offset = 0; offset < buffer.length; offset += 12) {
    const slice = buffer.subarray(offset, offset + 12);
    const body = Array.from(slice, (byte) =>
      `0x${byte.toString(16).padStart(2, '0')}`
    ).join(', ');
    const suffix = offset + 12 < buffer.length ? ',' : '';
    lines.push(`    ${body}${suffix}`);
  }
  return lines.join('\n');
}

const ASSET_TAG_PLACEHOLDER = '__FW_ASSET_TAG__';

function emitGzipConst(name, buffer) {
  return `constexpr size_t ${name}_LEN = ${buffer.length};
const uint8_t ${name}[] PROGMEM = {
${formatByteArray(buffer)}
};

static_assert(sizeof(${name}) == ${name}_LEN,
              "gzip length mismatch for ${name}");
`;
}

function injectHomePartial(shellHtml, homePartial) {
  const re =
      /(<section id="view-home" class="view" data-view="home">)(<\/section>)/;
  if (!re.test(shellHtml)) {
    throw new Error('Shell missing empty view-home placeholder for home inject');
  }
  return shellHtml.replace(re, `$1${homePartial}$2`);
}

// Give a view module its own scope inside a bundle: top-level names stay
// private to the view, and the bundle supplies the runtime import.
// A view or prelude source minus its "use strict" and runtime import: inside a
// bundle the module's single import serves every file.
function stripModuleHeader(name, src) {
  const body = src
      .replace(/^\(?['"]use strict['"]\)?;\s*/m, '')
      .replace(/import\s*\*\s*as\s+R\s+from\s*['"][^'"]+['"];\s*/m, '')
      .replace(/import\s*\{[^}]*\}\s*from\s*['"]\.\/records\.js[^'"]*['"];\s*/m, '');
  if (/\bimport\s*[*{]/.test(body)) {
    throw new Error(`View ${name} retained its runtime import`);
  }
  return body;
}
// The scope returns the view's own exports plus the names the bundle adds.
function scopedView(name, src, exports) {
  const names = [...exports, ...src.matchAll(/^export\s+(?:function|const|let)\s+(\w+)/gm)
      .map((m) => m[1])];
  const body = stripModuleHeader(name, src).replace(/export\s+(function|const|let)\s+/g, '$1 ');
  // `export async function` or a second name in `export const a, b` would
  // leave a syntax error or drop an export inside the scope.
  if (/^\s*export\s/m.test(body) || /^export\s+(?:const|let)\s+\w+[^;]*,\s*\w+\s*=/m.test(src))
    throw new Error(`${name}.js: export form not supported by scopedView`);
  return `const ${name}=(()=>{${body}\nreturn{${names.join(',')}};})();`;
}

function inlineHomeModule(appSrc, homeSrc) {
  if (!appSrc.includes('__homeModule')) {
    throw new Error('app.js must reference __homeModule for home cold path');
  }
  const marker = /import\s*\*\s*as\s+R\s+from\s*['"][^'"]+['"];\s*/;
  if (!marker.test(appSrc)) {
    throw new Error('app.js runtime import not found');
  }
  return appSrc.replace(marker, (m) =>
    m + scopedView('__homeModule', homeSrc, []) + '\n');
}

// A single-view module exports its markup next to init; the records bundle
// exposes each of its views under `views`, after their shared prelude.
function buildViewModule(file, views, viewJsRaw, partials) {
  if (views.length === 1) {
    return `${viewJsRaw[views[0]]}\nexport const markup=${JSON.stringify(partials[views[0]])};`;
  }
  return [
    `import * as R from '/js/runtime.js?v=${ASSET_TAG_PLACEHOLDER}';`,
    stripModuleHeader(file, viewJsRaw[file]).replace(/^export\s+/gm, ''),
    ...views.map((name) => scopedView(`__${name}`,
        `${viewJsRaw[name]}\nconst markup=${JSON.stringify(partials[name])};`,
        ['markup'])),
    `export const views={${views.map((name) => `${name}:__${name}`).join(',')}};`,
  ].join('\n');
}

async function generate(options = {}) {
  const webUiLanguage = options.webUiLanguage ||
      process.env.OPENBREWBYWEIGHT_WEBUI_LANGUAGE || 'en';
  // Contracts pass readSource to generate from reformatted HTML and CSS.
  const read = options.readSource || ((file) => fs.readFileSync(file, 'utf8'));
  assertCssSourcesComplete();
  const source = read(sourcePath).replace(/\r?\n$/, '');
  const cssSource = cssSourcePaths.map((file) => read(file)).join('');
  const manifestObject = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  const icon192Raw = fs.readFileSync(icon192Path);
  const icon48Raw = fs.readFileSync(icon48Path);
  const version = options.versionOverride || readFirmwareVersion();
  if (options.machineType && options.machineType !== 'all' &&
      !MACHINE_TYPE_EXCLUSIONS[options.machineType]) {
    throw new Error(`Unknown --machine-type: ${options.machineType}`);
  }
  if (options.machineIcon !== undefined && !options.machineIcon) {
    throw new Error('--machine-icon requires a value');
  }

  let shellHtmlRaw = source;
  const partialsRaw = {};
  for (const name of VIEW_NAMES) {
    const partialPath = path.join(htmlDir, `${name}.html`);
    if (!fs.existsSync(partialPath)) {
      throw new Error(`Missing HTML partial: ${partialPath}`);
    }
    partialsRaw[name] = read(partialPath);
  }

  const appJsRaw = fs.readFileSync(appJsPath, 'utf8');
  const runtimeRaw = fs.readFileSync(path.join(jsDir, 'runtime.js'), 'utf8');
  const otaImageRaw = fs.readFileSync(otaImageJsPath, 'utf8');
  const viewJsRaw = {};
  for (const name of JS_FILES) {
    const viewPath = path.join(jsDir, `${name}.js`);
    if (!fs.existsSync(viewPath)) {
      throw new Error(`Missing JS view module: ${viewPath}`);
    }
    viewJsRaw[name] = fs.readFileSync(viewPath, 'utf8');
  }

  const localized = renderSources([
    {file: sourcePath, type: 'html', content: shellHtmlRaw},
    ...VIEW_NAMES.map((name) => ({file: path.join(htmlDir, `${name}.html`),
      type: 'html', content: partialsRaw[name]})),
    {file: appJsPath, type: 'js', content: appJsRaw},
    {file: path.join(jsDir, 'runtime.js'), type: 'js', content: runtimeRaw},
    {file: otaImageJsPath, type: 'js', content: otaImageRaw},
    {file: swPath, type: 'js', content: fs.readFileSync(swPath, 'utf8')},
    ...JS_FILES.map((name) => ({file: path.join(jsDir, `${name}.js`),
      type: 'js', content: viewJsRaw[name]})),
    {file: 'app.css', type: 'css', content: cssSource},
  ], {language: webUiLanguage, localesDir: options.localesDir,
    developmentMode: options.developmentMode === true,
    machineType: options.machineType, machineIcon: options.machineIcon});
  let at = 0;
  shellHtmlRaw = localized.sources[at++].content;
  for (const name of VIEW_NAMES) partialsRaw[name] = localized.sources[at++].content;
  const localizedAppJs = localized.sources[at++].content;
  const localizedRuntime = localized.sources[at++].content;
  const localizedOtaImage = localized.sources[at++].content;
  const localizedSw = localized.sources[at++].content;
  for (const name of JS_FILES) viewJsRaw[name] = localized.sources[at++].content;
  const localizedCss = localized.sources[at].content;
  const excluded = excludedClasses(options);
  // The shell may carry build-gated markup too (the header's Micra cloud
  // indicator); typed builds strip it exactly like the view partials.
  shellHtmlRaw = stripClassMarkup(shellHtmlRaw, excluded);
  for (const name of VIEW_NAMES) {
    partialsRaw[name] = stripClassMarkup(partialsRaw[name], excluded);
  }
  if (options.remoteControl === false) {
    for (const name of VIEW_NAMES) {
      partialsRaw[name] = stripRemoteControlMarkup(partialsRaw[name]);
    }
  }

  const partials = {};
  for (const name of VIEW_NAMES) {
    partials[name] = await minifyHtml(partialsRaw[name]);
  }
  const out = {
    shellHtml: injectHomePartial(await minifyHtml(shellHtmlRaw
        .split('{{webui-meta:development-class}}')
        .join(options.developmentMode === true ? 'devBuild' : '')), partials.home),
    appJs: await minifyJs(inlineHomeModule(localizedAppJs, viewJsRaw.home), options),
    runtimeJs: await minifyJs(localizedRuntime, options),
    otaImageJs: await minifyJs(localizedOtaImage, options),
    // The launch gate carries no strings, so it skips localization.
    launchJs: await minifyJs(fs.readFileSync(launchJsPath, 'utf8'), options),
    css: minifyCss(stripClassCss(options.remoteControl === false
        ? stripRemoteControlCss(localizedCss) : localizedCss, excluded)),
    manifest: JSON.stringify(manifestObject),
    // The worker precaches every ?v= row; probe paths and itself stay out.
    swJs: await minifyJs(localizedSw
        .split('__SHELL_PATHS__').join(JSON.stringify(SHELL_PATHS))
        .split('__SW_ASSETS__').join(
            JSON.stringify(ASSET_PATHS.filter((p) => !p.endsWith('*') && p !== '/sw.js'))), options),
  };
  const modules = {};
  for (const [file, views] of Object.entries(VIEW_MODULES)) {
    modules[file] = await minifyJs(buildViewModule(file, views, viewJsRaw, partials), options);
  }

  // The tag fingerprints the exact bytes served (with the placeholder still
  // in place), so every ?v= URL names one immutable body. The firmware
  // version lives only in the revalidated shell, so firmware-only releases
  // keep every cached asset.
  const hash = crypto.createHash('sha256');
  for (const text of Object.values(out)) hash.update(text);
  for (const text of Object.values(modules)) hash.update(text);
  hash.update(icon192Raw);
  hash.update(icon48Raw);
  const assetTag = hash.digest('hex').slice(0, 8);
  for (const key of Object.keys(out)) {
    out[key] = out[key].split(ASSET_TAG_PLACEHOLDER).join(assetTag);
  }
  for (const key of Object.keys(modules)) {
    modules[key] = modules[key].split(ASSET_TAG_PLACEHOLDER).join(assetTag);
  }
  out.shellHtml = out.shellHtml.split('__FW_RELEASE__').join(version);
  out.swJs = out.swJs.split('__FW_RELEASE__').join(version);

  return finish({
    ...out,
    modules,
    partials,
    icon192Raw,
    icon48Raw,
    assetTag,
    version,
    requestedLanguage: localized.requestedLanguage,
    inputLanguage: localized.inputLanguage,
    resolvedLanguage: localized.resolvedLanguage,
    write: options.write !== false,
    output: options.output || outputPath,
  });
}

// Every asset except the shell is one row of the WEB_ASSETS table the
// firmware serves from. PNG artwork is already compressed, so it ships raw.
async function finish({shellHtml, partials, appJs, runtimeJs, otaImageJs, launchJs, modules, swJs,
                       css, manifest, icon192Raw, icon48Raw, assetTag,
                       version, inputLanguage, requestedLanguage,
                       resolvedLanguage, write, output}) {
  const text = (path, contentType, value) =>
    ({path, contentType, text: value, raw: Buffer.from(value, 'utf8'), gzip: true});
  const assets = [
    text('/app.js', JS_TYPE, appJs),
    text('/app.css', 'text/css; charset=utf-8', css),
    text('/js/runtime.js', JS_TYPE, runtimeJs),
    text('/js/ota-image.js', JS_TYPE, otaImageJs),
    text('/js/launch.js', JS_TYPE, launchJs),
    ...Object.entries(modules).map(([file, js]) => text(`/js/${file}.js`, JS_TYPE, js)),
    text('/sw.js', JS_TYPE, swJs),
    text('/manifest.webmanifest', 'application/manifest+json', manifest),
    {path: '/icons/icon-192.png', contentType: 'image/png', raw: icon192Raw, gzip: false},
    {path: '/favicon.ico', contentType: 'image/png', raw: icon48Raw, gzip: false},
    // iOS probes size-specific /apple-touch-icon* paths that ignore the
    // shell's <link>; the trailing '*' row prefix-matches every variant.
    {path: '/apple-touch-icon*', contentType: 'image/png', raw: icon192Raw, gzip: false},
  ];
  if (assets.map((asset) => asset.path).join() !== ASSET_PATHS.join()) {
    throw new Error('WEB_ASSETS rows must follow ASSET_PATHS');
  }
  const symbols = new Map();
  for (const asset of assets) {
    asset.body = asset.gzip ? await gzipBuffer(asset.raw) : asset.raw;
    if (!symbols.has(asset.raw)) {
      symbols.set(asset.raw, 'WEB_ASSET_' +
          asset.path.replace(/\*$/, '').replace(/[^A-Za-z0-9]+/g, '_').replace(/^_|_$/g, '').toUpperCase());
    }
    asset.symbol = symbols.get(asset.raw);
  }
  const shellGzip = await gzipBuffer(Buffer.from(shellHtml, 'utf8'));
  const emitted = new Set();
  const arrays = assets.filter((asset) => !emitted.has(asset.symbol) && emitted.add(asset.symbol))
      .map((asset) => emitGzipConst(asset.symbol, asset.body)).join('\n');
  const rows = assets.map((asset) =>
    `    {"${asset.path}", "${asset.contentType}", ${asset.symbol}, ${asset.symbol}_LEN, ${asset.gzip}},`)
      .join('\n');

  const cacheVersion = `${version}.${assetTag}`;
  const sha256 = (body) => crypto.createHash('sha256').update(body).digest('base64');
  const assetList = JSON.stringify({
    v: 1,
    tag: assetTag,
    etag: `"${cacheVersion}"`,
    csp: WEB_UI_CSP,
    shell: [sha256(shellGzip), shellGzip.length],
    shellPaths: SHELL_PATHS,
    assets: assets.map((asset) =>
      [asset.path, sha256(asset.body), asset.body.length, asset.contentType, asset.gzip]),
  });
  // The header holds only what never changes with the Web UI or the version,
  // so content and release changes recompile the generated source alone.
  const header = `#pragma once

#include <stddef.h>
#include <stdint.h>

namespace openbrewbyweight {

// Generated by scripts/gen_web_ui.js — do not edit. The content lives in the
// generated OpenBrewByWeightWebAssetsGzip.cpp beside this header.

struct WebAsset {
  const char *path;
  const char *contentType;
  const uint8_t *data;
  size_t length;
  bool gzip;
};

extern const char WEB_UI_ASSET_TAG[];
extern const char WEB_UI_ETAG[];
extern const char WEB_UI_ASSET_ETAG[];
extern const char WEB_UI_CSP[];
extern const char WEB_ASSET_LIST_JSON[];
extern const size_t OPEN_BREW_BY_WEIGHT_WEB_UI_GZIP_LEN;
extern const uint8_t OPEN_BREW_BY_WEIGHT_WEB_UI_GZIP[];
extern const WebAsset WEB_ASSETS[${assets.length}];

}  // namespace openbrewbyweight
`;
  const source = `#include <pgmspace.h>

#include "OpenBrewByWeightWebAssetsGzip.h"

namespace openbrewbyweight {

// Generated by scripts/gen_web_ui.js — do not edit.

constexpr char WEB_UI_ASSET_TAG[] = "${assetTag}";
// The shell and the worker name the release too; ?v= assets only their bytes.
constexpr char WEB_UI_ETAG[] = ${JSON.stringify('"' + cacheVersion + '"')};
constexpr char WEB_UI_ASSET_ETAG[] = ${JSON.stringify('"' + assetTag + '"')};
constexpr char WEB_UI_CSP[] = ${JSON.stringify(WEB_UI_CSP)};
// What a paired companion verifies each file against: the exact bytes served.
constexpr char WEB_ASSET_LIST_JSON[] = ${JSON.stringify(assetList)};

${emitGzipConst('OPEN_BREW_BY_WEIGHT_WEB_UI_GZIP', shellGzip)}
${arrays}
const WebAsset WEB_ASSETS[${assets.length}] = {
${rows}
};

}  // namespace openbrewbyweight
`;

  // Rewrite only on change so unchanged builds do not recompile.
  const sourceOutput = output.replace(/\.h$/, '.cpp');
  for (const [file, body] of [[output, header], [sourceOutput, source]]) {
    if (write && !(fs.existsSync(file) && fs.readFileSync(file, 'utf8') === body)) {
      fs.mkdirSync(path.dirname(file), {recursive: true});
      fs.writeFileSync(`${file}.tmp`, body);
      fs.renameSync(`${file}.tmp`, file);
    }
  }

  const byPath = (path) => assets.find((asset) => asset.path === path).body;
  const moduleGzip = Object.fromEntries(Object.keys(modules).map(
      (file) => [file, byPath(`/js/${file}.js`)]));
  const combined = shellGzip.length + [...new Set(assets.map((asset) => asset.body))]
      .reduce((sum, bytes) => sum + bytes.length, 0);

  return {
    html: shellHtml,
    js: appJs,
    runtimeJs,
    otaImageJs,
    launchJs,
    modules,
    swJs,
    css,
    manifest,
    icon192Raw,
    icon48Raw,
    partials,
    assets,
    assetList,
    gzip: shellGzip,
    jsGzip: byPath('/app.js'),
    runtimeGzip: byPath('/js/runtime.js'),
    otaImageGzip: byPath('/js/ota-image.js'),
    moduleGzip,
    manifestGzip: byPath('/manifest.webmanifest'),
    cssGzip: byPath('/app.css'),
    assetTag,
    cacheVersion,
    etag: `"${cacheVersion}"`,
    assetEtag: `"${assetTag}"`,
    combined,
    outputPath: output,
    sourceOutputPath: sourceOutput,
    sourcePath,
    appJsPath,
    cssSourcePaths,
    version,
    requestedLanguage,
    inputLanguage,
    resolvedLanguage,
    VIEW_NAMES,
    VIEW_MODULES,
  };
}

module.exports = {
  minifyJs,
  minifyCss,
  generate,
  sourcePath,
  appJsPath,
  swPath,
  cssSourcePaths,
  VIEW_NAMES,
  VIEW_MODULES,
  JS_FILES,
  ASSET_PATHS,
  SHELL_PATHS,
  htmlDir,
  jsDir,
};

if (require.main === module) {
  Promise.resolve().then(() => {
    let webUiLanguage;
    let developmentMode;
    let machineType;
    let remoteControl;
    let lineaMicra;
    let machineIcon;
    let versionOverride;
    let output;
    for (let i = 2; i < process.argv.length; i++) {
      const arg = process.argv[i];
      if (arg === '--webui-language') {
        if (++i >= process.argv.length) throw new Error('--webui-language requires a value');
        webUiLanguage = process.argv[i];
      } else if (arg.startsWith('--webui-language=')) {
        webUiLanguage = arg.slice('--webui-language='.length);
      } else if (arg === '--no-remote-control') {
        remoteControl = false;
      } else if (arg === '--no-linea-micra') {
        lineaMicra = false;
      } else if (arg === '--machine-icon') {
        if (++i >= process.argv.length) throw new Error('--machine-icon requires a value');
        machineIcon = process.argv[i];
      } else if (arg.startsWith('--machine-icon=')) {
        machineIcon = arg.slice('--machine-icon='.length);
      } else if (arg === '--development') {
        developmentMode = true;
      } else if (arg === '--machine-type') {
        if (++i >= process.argv.length) throw new Error('--machine-type requires a value');
        machineType = process.argv[i];
      } else if (arg.startsWith('--machine-type=')) {
        machineType = process.argv[i].slice('--machine-type='.length);
      } else if (arg === '--version') {
        if (++i >= process.argv.length) throw new Error('--version requires a value');
        versionOverride = process.argv[i];
      } else if (arg.startsWith('--version=')) {
        versionOverride = process.argv[i].slice('--version='.length);
      } else if (arg === '--output') {
        if (++i >= process.argv.length) throw new Error('--output requires a value');
        output = process.argv[i];
      } else {
        throw new Error(`Unknown argument: ${arg}`);
      }
    }
    return generate({webUiLanguage, developmentMode, machineType, remoteControl,
      lineaMicra, machineIcon, versionOverride, output});
  })
      .then((result) => {
        const parts = [`shell ${result.gzip.length} B`,
          ...result.assets.map((asset) => `${asset.path} ${asset.body.length} B`)];
        console.log(
            `Generated ${result.outputPath} ` +
                `(${parts.join(', ')}, combined ${result.combined} B gzip, ` +
                `v=${result.cacheVersion}, locale=${result.inputLanguage}` +
                `->${result.resolvedLanguage})`);
      })
      .catch((error) => {
        console.error(error.message);
        process.exitCode = 1;
      });
}
