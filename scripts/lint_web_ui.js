#!/usr/bin/env node
'use strict';

// Validates the Web UI markup as browsers receive it: the English shell with
// every view partial injected, so ids and structure are checked across views
// (html-validate's no-dup-id; the Web contracts enforce the same rule).
const fs = require('fs');
const path = require('path');
const {HtmlValidate} = require('html-validate');
const {renderSources} = require('./localize_web_ui.js');
const webUi = require('./gen_web_ui.js');

const repoRoot = path.resolve(__dirname, '..');

function assembledDocument(machineIcon) {
  // The localizer checks the catalog against every source, so all of them go in.
  const html = [webUi.sourcePath, ...webUi.VIEW_NAMES.map((name) =>
    path.join(webUi.htmlDir, `${name}.html`))];
  const js = [webUi.appJsPath, ...['runtime', 'ota-image', 'records', ...webUi.VIEW_NAMES].map((name) =>
    path.join(webUi.jsDir, `${name}.js`))];
  const read = (type) => (file) => ({file, type, content: fs.readFileSync(file, 'utf8')});
  const [shell, ...partials] = renderSources([...html.map(read('html')), ...js.map(read('js')),
    {file: 'app.css', type: 'css',
      content: webUi.cssSourcePaths.map((file) => fs.readFileSync(file, 'utf8')).join('')}],
    {language: 'en', machineIcon}).sources
      .slice(0, html.length).map((source) => source.content);
  const views = Object.fromEntries(webUi.VIEW_NAMES.map((name, i) => [name, partials[i]]));
  return shell.split('{{webui-meta:development-class}}').join('')
      .replace(/(<section\s+id="view-([a-z]+)"\s+class="view"\s+data-view="\2"\s*>)\s*(<\/section\s*>)/g,
          (_, open, name, close) => open + views[name] + close);
}

async function main() {
  const failures = [];
  const config = JSON.parse(fs.readFileSync(
      path.join(repoRoot, 'scripts', 'lint', 'htmlvalidate.json'), 'utf8'));
  const validator = new HtmlValidate(config);
  // The fallback glyph and the reference machine's glyph both ship in real
  // builds, so both baked forms must validate.
  for (const machineIcon of [undefined, 'linea-micra']) {
    const doc = assembledDocument(machineIcon);
    const report = await validator.validateString(doc, 'webui.html');
    for (const result of report.results) {
      for (const m of result.messages) {
        const context = doc.slice(Math.max(0, m.offset - 40), m.offset + 60).replace(/\s+/g, ' ');
        failures.push(`${machineIcon || 'default'}: ${m.ruleId}: ${m.message} … ${context}`);
      }
    }
  }
  if (failures.length) {
    console.error(failures.join('\n'));
    console.error(`Web UI markup: ${failures.length} problem(s)`);
    process.exitCode = 1;
    return;
  }
  console.log('Web UI markup: html-validate clean');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
