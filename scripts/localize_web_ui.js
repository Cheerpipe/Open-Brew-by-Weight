#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..');
const defaultLocalesDir = path.join(repoRoot, 'src', 'web', 'locales');
const KEY_RE = /^[a-z0-9_]+(?:\.[a-z0-9_]+)+$/;
const LANGUAGE_RE = /^[a-z]{2,3}(?:-[a-z0-9]{2,8})*$/;
const HTML_MARKER_RE = /\{\{webui:([a-z0-9_.]+)\}\}/g;
const META_MARKER_RE =
    /\{\{webui-meta:(locale|direction-attribute|development-class|machine-type-class)\}\}/g;
const MACHINE_ICON_MARKER = '{{webui-meta:machine-icon}}';
const MACHINE_ICON_SPRITE_RE = /<svg\b[^>]*\bdata-machine-icons\b[^>]*>([\s\S]*?)<\/svg\s*>/;
const MACHINE_ICON_SYMBOL_RE = /<symbol\b[^>]*\bid="machine-icon-([a-z0-9-]+)"[^>]*>([\s\S]*?)<\/symbol\s*>/g;
const JS_MARKER_RE = /__WEBUI_TEXT__\("([a-z0-9_.]+)"\)/g;
const CSS_MARKER_RE = /__WEBUI_CSS_TEXT__\(\s*"([a-z0-9_.]+)"\s*\)/g;

function fail(message) {
  throw new Error(`Web UI localization: ${message}`);
}

function normalizeLanguageCode(input) {
  if (typeof input !== 'string' || !input.trim()) fail('language code is empty');
  const normalized = input.trim().replace(/_/g, '-').toLowerCase();
  if (!LANGUAGE_RE.test(normalized)) {
    fail(`invalid language code ${JSON.stringify(input)}`);
  }
  return normalized;
}

function readJson(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (error) {
    fail(`cannot read ${file}: ${error.message}`);
  }
}

function validateCatalog(catalog, file, referenceKeys) {
  if (!catalog || Array.isArray(catalog) || typeof catalog !== 'object') {
    fail(`${file}: catalog root must be an object`);
  }
  if (catalog.schemaVersion !== 1) fail(`${file}: schemaVersion must be 1`);
  const expectedLocale = path.basename(file, '.json').toLowerCase();
  if (!LANGUAGE_RE.test(expectedLocale)) fail(`${file}: invalid catalog filename`);
  if (catalog.locale !== expectedLocale) {
    fail(`${file}: locale must match filename ${expectedLocale}`);
  }
  if (typeof catalog.language !== 'string' || !catalog.language.trim()) {
    fail(`${file}: language must be a non-empty string`);
  }
  if (catalog.direction !== 'ltr' && catalog.direction !== 'rtl') {
    fail(`${file}: direction must be ltr or rtl`);
  }
  if (!catalog.strings || Array.isArray(catalog.strings) ||
      typeof catalog.strings !== 'object') {
    fail(`${file}: strings must be an object`);
  }
  const keys = Object.keys(catalog.strings);
  for (const key of keys) {
    if (!KEY_RE.test(key)) fail(`${file}: invalid string key ${JSON.stringify(key)}`);
    if (typeof catalog.strings[key] !== 'string') {
      fail(`${file}: ${key} must contain a string`);
    }
    // Glyphs, numbers and punctuation are not translatable: keep them in the source.
    if (!/\p{L}/u.test(catalog.strings[key])) fail(`${file}: ${key} has no words to translate`);
  }
  const sortedKeys = [...keys].sort();
  if (keys.some((key, index) => key !== sortedKeys[index])) {
    fail(`${file}: strings keys must be sorted`);
  }
  if (referenceKeys) {
    const wanted = new Set(referenceKeys);
    const actual = new Set(keys);
    for (const key of referenceKeys) if (!actual.has(key)) fail(`${file}: missing key ${key}`);
    for (const key of keys) if (!wanted.has(key)) fail(`${file}: extra key ${key}`);
  }
  return keys.sort();
}

function loadCatalog(language, options = {}) {
  const localesDir = options.localesDir || defaultLocalesDir;
  const inputLanguage = language || 'en';
  const requestedLanguage = normalizeLanguageCode(inputLanguage);
  if (!fs.existsSync(localesDir)) fail(`missing catalog directory ${localesDir}`);
  const files = fs.readdirSync(localesDir).filter((name) => name.endsWith('.json')).sort();
  const referenceFile = path.join(localesDir, 'en.json');
  if (!files.includes('en.json')) fail(`missing reference catalog ${referenceFile}`);
  const reference = readJson(referenceFile);
  const referenceKeys = validateCatalog(reference, referenceFile);
  const catalogs = new Map([['en', reference]]);
  for (const name of files) {
    if (name === 'en.json') continue;
    const file = path.join(localesDir, name);
    const catalog = readJson(file);
    validateCatalog(catalog, file, referenceKeys);
    catalogs.set(path.basename(name, '.json').toLowerCase(), catalog);
  }
  const base = requestedLanguage.split('-')[0];
  const resolvedLanguage = catalogs.has(requestedLanguage) ? requestedLanguage : base;
  if (!catalogs.has(resolvedLanguage)) {
    fail(`no catalog for ${requestedLanguage} (tried ${requestedLanguage} and ${base})`);
  }
  return {inputLanguage, requestedLanguage, resolvedLanguage,
    catalog: catalogs.get(resolvedLanguage), referenceKeys};
}

function htmlEscape(value) {
  return value.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

function jsLiteral(value) {
  return JSON.stringify(value).replace(/\u2028/g, '\\u2028').replace(/\u2029/g, '\\u2029');
}

function cssLiteral(value) {
  let out = '"';
  for (const char of value) {
    const code = char.codePointAt(0);
    if (char === '"' || char === '\\') out += `\\${char}`;
    else if (code === 0) out += '\\fffd ';
    else if (code < 0x20 || code === 0x7f) out += `\\${code.toString(16)} `;
    else out += char;
  }
  return out + '"';
}

function assertHtmlContext(source, marker, offset, meta) {
  const inTag = source.lastIndexOf('<', offset) > source.lastIndexOf('>', offset);
  if (!inTag) {
    if (meta) fail('metadata marker is only valid in a quoted HTML attribute');
    return;
  }
  const start = source.lastIndexOf('<', offset);
  const end = source.indexOf('>', offset);
  const tag = source.slice(start, end < 0 ? source.length : end + 1);
  const escaped = marker.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const attrs = meta ? '(?:lang|dir)' : '(?:title|placeholder|aria-label|alt|data-label)';
  if (!new RegExp(`\\b${attrs}=(['"])${escaped}\\1`).test(tag)) {
    fail(`marker ${marker} is not a complete allowed quoted HTML attribute value`);
  }
}

// One glyph per machine ships: home.html carries the catalog (one hidden
// <symbol id="machine-icon-<id>"> per machine) next to the marker that shows
// it, the render's machineIcon option selects the glyph (default: the generic
// machine), and both scaffolding pieces drop here.
function bakeMachineIcon(source, file, options) {
  const sprite = source.match(MACHINE_ICON_SPRITE_RE);
  if (!sprite) {
    if (source.includes(MACHINE_ICON_MARKER)) {
      fail(`${file}: ${MACHINE_ICON_MARKER} requires the data-machine-icons catalog sprite`);
    }
    return source;
  }
  if (!source.includes(MACHINE_ICON_MARKER)) {
    fail(`${file}: the machine-icons catalog sprite requires ${MACHINE_ICON_MARKER}`);
  }
  const id = options.machineIcon || 'default';
  const glyph = [...sprite[1].matchAll(MACHINE_ICON_SYMBOL_RE)]
      .find((match) => match[1] === id);
  if (!glyph) fail(`${file}: no machine-icon-${id} glyph in the catalog sprite`);
  return source.replace(MACHINE_ICON_SPRITE_RE, '')
      .split(MACHINE_ICON_MARKER).join(glyph[2].trim());
}

function renderHtml(source, file, state) {
  // The machine profile's own Home icon bakes in before marker rendering: the
  // glyph catalog travels beside the marker in home.html's hidden
  // data-machine-icons sprite, a build's machineIcon option selects the glyph,
  // and the scaffolding (sprite and marker) never survives a render.
  let rendered = bakeMachineIcon(source, file, state.options);
  rendered = rendered.replace(HTML_MARKER_RE, (marker, key, offset) => {
    assertHtmlContext(rendered, marker, offset, false);
    return htmlEscape(useString(file, key, state));
  });
  rendered = rendered.replace(META_MARKER_RE, (marker, name, offset) => {
    if (name === 'direction-attribute') {
      const inTag = rendered.lastIndexOf('<', offset) > rendered.lastIndexOf('>', offset);
      if (!inTag) fail(`${file}: ${marker} must be inside the html start tag`);
      return state.catalog.direction === 'rtl' ? ' dir="rtl"' : '';
    }
    // Static build-mode marker, resolved from the render options rather than
    // the locale catalog: a development Web UI only ships in a build whose
    // generator ran with developmentMode enabled.
    if (name === 'development-class') {
      const inTag = rendered.lastIndexOf('<', offset) > rendered.lastIndexOf('>', offset);
      if (!inTag) fail(`${file}: ${marker} must be inside the html start tag`);
      return state.options.developmentMode === true ? 'devBuild' : '';
    }
    // Static build-mode marker for the compiled machine type; lets CSS and
    // contracts select on the type without any runtime status round-trip.
    if (name === 'machine-type-class') {
      const inTag = rendered.lastIndexOf('<', offset) > rendered.lastIndexOf('>', offset);
      if (!inTag) fail(`${file}: ${marker} must be inside the html start tag`);
      const classByType = {paddle: 'paddleBuild', momentary: 'momentaryBuild',
        momentary_reed: 'momentaryBuild reedBuild'};
      return classByType[state.options.machineType] || '';
    }
    assertHtmlContext(rendered, marker, offset, true);
    return htmlEscape(state.catalog.locale);
  });
  return rendered;
}

function useString(file, key, state) {
  if (!Object.prototype.hasOwnProperty.call(state.catalog.strings, key)) {
    fail(`${file}: missing key ${key} in ${state.catalog.locale}`);
  }
  state.used.add(key);
  return state.catalog.strings[key];
}

function renderJavaScript(source, file, state) {
  return source.replace(JS_MARKER_RE, (_marker, key) => jsLiteral(useString(file, key, state)));
}

function renderCss(source, file, state) {
  return source.replace(CSS_MARKER_RE, (marker, key, offset) => {
    if (!/\bcontent\s*:\s*$/.test(source.slice(Math.max(0, offset - 40), offset))) {
      fail(`${file}: ${marker} is only valid as a CSS content value`);
    }
    return cssLiteral(useString(file, key, state));
  });
}

function assertNoMarkers(source, file) {
  if (/\{\{webui(?:-meta)?:|__WEBUI_(?:TEXT|CSS_TEXT)__/.test(source)) {
    fail(`${file}: unresolved or invalid localization marker`);
  }
}

function renderSources(sources, options = {}) {
  const loaded = loadCatalog(options.language || process.env.OPENBREWBYWEIGHT_WEBUI_LANGUAGE || 'en', options);
  const state = {catalog: loaded.catalog, used: new Set(), options};
  const rendered = sources.map((source) => {
    let content;
    if (source.type === 'html') content = renderHtml(source.content, source.file, state);
    else if (source.type === 'js') content = renderJavaScript(source.content, source.file, state);
    else if (source.type === 'css') content = renderCss(source.content, source.file, state);
    else fail(`${source.file}: unknown source type ${source.type}`);
    assertNoMarkers(content, source.file);
    return {...source, content};
  });
  if (!options.allowUnused) {
    for (const key of loaded.referenceKeys) {
      if (!state.used.has(key)) fail(`reference catalog has unused key ${key}`);
    }
  }
  return {...loaded, sources: rendered, usedKeys: [...state.used].sort()};
}

function projectSources() {
  const webDir = path.join(repoRoot, 'src', 'web');
  const files = [
    ['html/shell.html', 'html'], ['html/home.html', 'html'],
    ['html/stats.html', 'html'], ['html/history.html', 'html'],
    ['html/diagnostic.html', 'html'],
    ['html/settings.html', 'html'], ['html/admin.html', 'html'],
    ['html/setup.html', 'html'],
    ['app.css', 'css'], ['app.js', 'js'], ['js/runtime.js', 'js'],
    ['js/ota-image.js', 'js'], ['js/home.js', 'js'], ['js/stats.js', 'js'],
    ['js/history.js', 'js'], ['js/records.js', 'js'],
    ['js/diagnostic.js', 'js'], ['js/settings.js', 'js'], ['js/admin.js', 'js'],
    ['js/setup.js', 'js'],
  ];
  return files.map(([name, type]) => ({file: `src/web/${name}`, type,
    content: fs.readFileSync(path.join(webDir, name), 'utf8')}));
}

function parseCli(argv) {
  let language;
  let check = false;
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--check') check = true;
    else if (arg === '--webui-language') {
      if (++i >= argv.length) fail('--webui-language requires a value');
      language = argv[i];
    } else if (arg.startsWith('--webui-language=')) language = arg.slice(17);
    else fail(`unknown argument ${JSON.stringify(arg)}`);
  }
  if (!check) fail('only --check mode is supported');
  return {language: language || process.env.OPENBREWBYWEIGHT_WEBUI_LANGUAGE || 'en'};
}

module.exports = {normalizeLanguageCode, validateCatalog, loadCatalog, htmlEscape,
  jsLiteral, cssLiteral, renderSources, projectSources, parseCli, defaultLocalesDir};

if (require.main === module) {
  try {
    const options = parseCli(process.argv.slice(2));
    const result = renderSources(projectSources(), options);
    console.log(`Web UI locale ${result.inputLanguage} resolved to ` +
      `${result.resolvedLanguage}; ${result.usedKeys.length} strings checked.`);
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
