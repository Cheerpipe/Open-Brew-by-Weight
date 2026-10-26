'use strict';

// Every Web UI size budget in one pass: authoring source bytes, shipped gzip
// bytes, and raw icons against config/web-asset-budgets.json. All overruns are
// listed together. OPENBREWBYWEIGHT_BUDGET_DEFER=1 reports them without
// failing so a gate can run to the end and judge every budget at once; with
// OPENBREWBYWEIGHT_RUN_DIR set, the measurements are kept for `budget update`.
const fs = require('fs');
const path = require('path');
const webUi = require('../../scripts/gen_web_ui.js');
const webUiLocale = require('../../scripts/localize_web_ui.js');

const budgetPath = path.resolve(__dirname, '..', '..', 'config', 'web-asset-budgets.json');
const {VIEW_NAMES, JS_FILES} = webUi;

// Contracts read HTML and CSS in one compact form, so how the sources are
// formatted cannot change what a needle matches or what a budget counts.
// Markup collapses like the shipped build; CSS drops comments and the
// spacing, quotes, leading zeros, and last semicolons a formatter adds.
const compactMarkup = (source) => require('html-minifier-terser').minify(source,
    {collapseWhitespace: true, collapseBooleanAttributes: true});
const compactCss = (source) => source.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\s+/g, ' ')
    .replace(/\s*([{};,]|!important)\s*/g, '$1')
    .replace(/([(:])\s+|\s+\)/g, (_, open) => open || ')')
    .replace(/[^{}]+\{/g, (prelude) => prelude.replace(/\s*([>+~])\s*/g, '$1'))
    .replace(/\{[^{}]*\}/g, (block) => block.replace(/\s*\*\s*/g, '*'))
    .replace(/;}/g, '}')
    .replace(/@media \(/g, '@media(')
    .replace(/(^|[^\w.])0\.(\d)/g, '$1.$2')
    .replace(/\[([-\w]+)([~|^$*]?=)"(-?[_a-zA-Z][-\w]*)"\]/g, '[$1$2$3]')
    .replace(/'([^'"]*)'/g, '"$1"')
    .trim();

// The authoring sources as the contracts read them: raw (compact, unlocalized)
// and English-localized, plus the shell with every partial injected.
async function authoringSources() {
  const read = (file) => fs.readFileSync(file, 'utf8');
  const raw = {shellHtml: await compactMarkup(read(webUi.sourcePath)), partialHtml: {},
    appJs: read(webUi.appJsPath), runtimeJs: read(path.join(webUi.jsDir, 'runtime.js')),
    otaImageJs: read(path.join(webUi.jsDir, 'ota-image.js')), viewJs: {},
    css: compactCss(read(webUi.cssSourcePath))};
  for (const name of VIEW_NAMES) {
    raw.partialHtml[name] = await compactMarkup(read(path.join(webUi.htmlDir, name + '.html')));
  }
  for (const name of JS_FILES) raw.viewJs[name] = read(path.join(webUi.jsDir, name + '.js'));
  const sources = webUiLocale.renderSources([
    {file: webUi.sourcePath, type: 'html', content: raw.shellHtml},
    ...VIEW_NAMES.map((name) => ({file: name + '.html', type: 'html', content: raw.partialHtml[name]})),
    {file: 'app.js', type: 'js', content: raw.appJs},
    {file: 'runtime.js', type: 'js', content: raw.runtimeJs},
    {file: 'ota-image.js', type: 'js', content: raw.otaImageJs},
    ...JS_FILES.map((name) => ({file: name + '.js', type: 'js', content: raw.viewJs[name]})),
    {file: 'app.css', type: 'css', content: raw.css},
  ], {language: 'en'}).sources.map((source) => source.content);
  const localized = {shellHtml: sources.shift(), partialHtml: {}, viewJs: {}};
  for (const name of VIEW_NAMES) localized.partialHtml[name] = sources.shift();
  [localized.appJs, localized.runtimeJs, localized.otaImageJs] = sources.splice(0, 3);
  for (const name of JS_FILES) localized.viewJs[name] = sources.shift();
  localized.css = sources.shift();
  const allHtml = localized.shellHtml.replace(
      /<section id="view-([a-z]+)" class="view" data-view="\1"><\/section>/g, (_, name) => {
        if (!localized.partialHtml[name]) {
          throw new Error('Missing partial for shell placeholder: ' + name);
        }
        return `<section id="view-${name}" class="view" data-view="${name}">${
          localized.partialHtml[name]}</section>`;
      });
  const allJs = [localized.appJs, localized.runtimeJs, localized.otaImageJs,
    ...JS_FILES.map((name) => localized.viewJs[name])].join('\n');
  return {raw, localized, allHtml, allJs};
}

async function measure() {
  const {allHtml, allJs} = await authoringSources();
  const generated = await webUi.generate({write: false});
  // The runtime bakes the build id (VERSION + short commit SHA), which moves its
  // compressed size on every commit; a fixed sentinel id keeps that noise out of
  // the runtime budget, while the combined budget measures the real assets.
  const sentinel = await webUi.generate({write: false, versionOverride: '00000000'});
  if (sentinel.version !== '00000000') throw new Error('Sentinel generation ignored the fixed build id');
  const html = Buffer.byteLength(allHtml, 'utf8');
  const js = Buffer.byteLength(allJs, 'utf8');
  return {
    // Complete, human-readable settings help is part of the UI contract:
    // adding a setting raises this budget when needed; hints are never cut to fit.
    'source.html': html,
    'source.js': js,
    'source.htmlJs': html + js,
    'gzip.shellHtml': generated.gzip.length,
    'gzip.shellJs': generated.jsGzip.length,
    'gzip.css': generated.cssGzip.length,
    'gzip.runtime': sentinel.runtimeGzip.length,
    'gzip.otaImage': generated.otaImageGzip.length,
    ...Object.fromEntries(Object.entries(generated.moduleGzip)
        .map(([file, bytes]) => ['gzip.module.' + file, bytes.length])),
    'gzip.manifest': generated.manifestGzip.length,
    'raw.icon192': generated.icon192Raw.length,
    'raw.icon48': generated.icon48Raw.length,
    // What a cold Home load downloads before it can paint is the budget users
    // feel; view-only code belongs in the lazy view modules.
    'gzip.coldHome': generated.gzip.length + generated.cssGzip.length +
        generated.jsGzip.length + generated.runtimeGzip.length,
    'gzip.combined': generated.combined,
  };
}

if (require.main === module) {
  measure().then((metrics) => {
    const {limits} = JSON.parse(fs.readFileSync(budgetPath, 'utf8'));
    const unknown = Object.keys(limits).filter((name) => !(name in metrics));
    if (unknown.length) throw new Error('Budgets without a measurement: ' + unknown.join(', '));
    const rows = Object.entries(limits).map(([name, limit]) => ({name, actual: metrics[name], limit}));
    for (const {name, actual, limit} of rows) {
      console.log(`${actual > limit ? 'BUDGET EXCEEDED ' : ''}${name}: ${actual} / ${limit} ` +
          `(${actual - limit >= 0 ? '+' : ''}${actual - limit})`);
    }
    if (process.env.OPENBREWBYWEIGHT_RUN_DIR) {
      const directory = path.join(process.env.OPENBREWBYWEIGHT_RUN_DIR, 'budgets');
      fs.mkdirSync(directory, {recursive: true});
      fs.writeFileSync(path.join(directory, 'web.json'),
          JSON.stringify({kind: 'web', rows}, null, 2) + '\n');
    }
    const over = rows.filter((row) => row.actual > row.limit).length;
    console.log(`Web UI budgets: ${rows.length} checked, ${over} exceeded`);
    if (over && process.env.OPENBREWBYWEIGHT_BUDGET_DEFER !== '1') process.exitCode = 1;
  }).catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}

module.exports = {compactMarkup, compactCss, authoringSources};
