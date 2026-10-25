#!/usr/bin/env node
'use strict';
// The design preview slices live Web UI sources into plain scripts. Render
// every reference page in-process (no listening socket) and require each
// served script to compile and to define the Web UI functions it calls, so a
// moved or renamed source function cannot silently break the preview.
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const root = path.resolve(__dirname, '..', '..');
const {createServer} = require(path.join(root, 'scripts/webui-preview/server.js'));

const server = createServer();
function get(url) {
  let status = 0, body = '';
  server.emit('request', {method: 'GET', url}, {
    writeHead(code) { status = code; return this; },
    end(content) { body = String(content ?? ''); },
  });
  assert.equal(status, 200, url + ' must render');
  return body;
}
// Local mockup fixtures are appended when present; check only the firmware part.
function withoutMockup(body, fixture) {
  const file = path.join(root, 'webui-mockups', fixture);
  const extra = fs.existsSync(file) ? '\n' + fs.readFileSync(file, 'utf8') : '';
  return extra && body.endsWith(extra) ? body.slice(0, -extra.length) : body;
}

const jsDir = path.join(root, 'src/web/js');
const web = fs.readdirSync(jsDir).filter((name) => name.endsWith('.js'))
  .map((name) => fs.readFileSync(path.join(jsDir, name), 'utf8')).join('\n');
const webFunctions = new Set([...web.matchAll(/^(?:export )?(?:async )?function ([\w$]+)\(/gm)]
  .map((match) => match[1]));
// Module-level state (`uiState.x`, `presetState.y`) must be defined too.
const webState = new Set([...web.matchAll(/^(?:export )?(?:let|const) ([\w$]+)\s*=/gm)]
  .map((match) => match[1]));
function checkScript(url, fixture, required, fixtureProvided = []) {
  const script = withoutMockup(get(url), fixture);
  assert.doesNotThrow(() => new Function(script), url + ' must compile');
  const defined = new Set([...script.matchAll(/function\s+([\w$]+)\s*\(|(?:const|let|var|,)\s*([\w$]+)\s*=/g)]
    .map((match) => match[1] || match[2]));
  for (const name of required) assert.ok(defined.has(name), url + ' must define ' + name);
  const missing = [...new Set([...script.matchAll(/(?<![.\w$])([A-Za-z_$][\w$]*)\s*\(/g)]
    .map((match) => match[1]))]
    .filter((name) => webFunctions.has(name) && !defined.has(name) && !fixtureProvided.includes(name));
  assert.deepEqual(missing, [], url + ' calls Web UI functions it does not define');
  const missingState = [...new Set([...script.matchAll(/(?<![.\w$])([A-Za-z_$][\w$]*)\s*\./g)]
    .map((match) => match[1]))]
    .filter((name) => webState.has(name) && !defined.has(name) && !fixtureProvided.includes(name));
  assert.deepEqual(missingState, [], url + ' reads Web UI state it does not define');
}

for (const url of ['/', '/mobile-menu', '/micra', '/app.css']) get(url);
checkScript('/preview.js', 'header.js',
  ['updateHeaderSignals', 'initHeaderSignals', 'updateQuickSettings', 'buildQuickSettings', 'escHtml']);
// The Micra controller actions come from the local webui-mockups/micra.js fixture.
checkScript('/micra-preview.js', 'micra.js',
  ['formatMicraCleaning', 'renderMicraCloudDiagnostic', 'renderMicraWebSocket', 'ensureMicraRows',
    'applyLineaMicraStatus'],
  ['api', 'command', 'lineaMicraAction', 'refreshStatus', 'setSaveDirty', 'showFieldError']);
console.log('Web UI design preview: reference pages and preview scripts OK');
