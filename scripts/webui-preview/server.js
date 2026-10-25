'use strict';

// Localhost design-preview server for the embedded Web UI. The server itself
// is project tooling; every design asset it serves is a local, Git-ignored
// file under webui-mockups/ and never ships with the repository.

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const {renderSources} = require('../localize_web_ui.js');

const root = path.resolve(__dirname, '..', '..');
const mockupsDir = path.join(root, 'webui-mockups');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');
const mockup = file => fs.existsSync(path.join(mockupsDir, file))
  ? fs.readFileSync(path.join(mockupsDir, file), 'utf8') : '';
const types = {
  '.html': 'text/html; charset=utf-8', '.css': 'text/css',
  '.js': 'text/javascript', '.json': 'application/json',
  '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp',
  '.ico': 'image/x-icon', '.woff2': 'font/woff2',
};

function staticMockup(pathname) {
  const relative = path.normalize(decodeURIComponent(pathname).replace(/^\/+/, ''));
  if (!relative || relative === '..' || relative.startsWith('..' + path.sep) || path.isAbsolute(relative)) {
    return;
  }
  for (const suffix of ['', '.html']) {
    const file = path.join(mockupsDir, relative + suffix);
    if (fs.existsSync(file) && fs.statSync(file).isFile()) {
      return [file, types[path.extname(file)] || 'application/octet-stream'];
    }
  }
}

// Formatted sources may wrap a tag across lines, so view slots match loosely.
function fillView(html, name, content, hidden = '') {
  const slot = new RegExp(`<section\\s+id="view-${name}"\\s+class="view"\\s+data-view="${name}"\\s*>\\s*</section\\s*>`);
  return html.replace(slot, () => `<section id="view-${name}" class="view" data-view="${name}"${hidden}>${content}</section>`);
}

function renderHome() {
  const [shell, home] = renderSources(['shell', 'home'].map(name => ({
    file: `src/web/html/${name}.html`, type: 'html',
    content: read(`src/web/html/${name}.html`),
  })), {language: 'en', allowUnused: true, machineType: 'paddle'}).sources;
  return fillView(shell.content, 'home', home.content)
      .replace('</head>', '<link rel="stylesheet" href="/header.css"></head>')
      .replace(/<script\s+type="module"\s+src="\/app\.js\?v=__FW_ASSET_TAG__"\s*><\/script\s*>/,
          '<script src="/preview.js" defer></script>')
      .replace(/<link rel="(?:manifest|icon|apple-touch-icon|modulepreload)"[^>]*>/g, '')
      .replace(/__FW_(?:ASSET_TAG|RELEASE)__/g, 'design-preview');
}

// Header signals and Quick Settings run against an in-memory controller: the
// preview never connects to a device, so writes only change these values.
const PREVIEW_HEADER_PRELUDE = `window.$ ??= id => document.getElementById(id);
let controlsMutable = true, configRevision = 1;
const previewStatus = {config: {revision: 1, brewByWeight: true, noScaleBbwMode: 'warn_once',
  fastExtractionGuardEnabled: true, slowExtractionGuardEnabled: true, autoToManualGuardEnabled: true,
  avoidAccidentalTouchEnabled: true, cupProtectionEnabled: true}};
const uiState = {}, presetState = {activeId: 0};
const body = values => JSON.stringify(values), message = text => console.info(text);
async function api(path, options) {
  const {baseRevision, ...change} = JSON.parse(options.body);
  Object.assign(previewStatus.config, change, {revision: (configRevision = baseRevision + 1)});
  setTimeout(() => updateQuickSettings(structuredClone(previewStatus)));
}
const command = (path, value) => api(path, {body: body(value)});
const refreshStatus = async () => updateQuickSettings(structuredClone(previewStatus));
// Runs even if an optional webui-mockups/header.js fixture throws.
document.addEventListener('DOMContentLoaded', () => {
  if (!document.getElementById('quickPanel')) initHeaderSignals();
  updateQuickSettings(structuredClone(previewStatus));
});
`;

// A top-level function's source by name, wherever its module keeps it.
function functionSource(source, name) {
  const start = source.search(new RegExp(`^(?:export )?(?:async )?function ${name}\\(`, 'm'));
  if (start < 0) throw new Error(`Web UI function ${name} not found`);
  return source.slice(start, source.indexOf('\n}\n', start) + 3).replace(/^export /, '');
}

function renderSignals() {
  const runtime = read('src/web/js/runtime.js');
  const start = runtime.indexOf('function updateHeaderSignals('), end = runtime.search(/^let homeBootDone\b/m);
  if (start < 0 || end < start) throw new Error('runtime.js header signal block not found');
  const content = functionSource(runtime, 'escHtml') + runtime.slice(start, end).replace(/^export /gm, '');
  return PREVIEW_HEADER_PRELUDE + renderSources([{file: 'src/web/js/runtime.js', type: 'js', content}],
      {language: 'en', allowUnused: true}).sources[0].content;
}

function renderMobileMenu() {
  const views = ['stats', 'history', 'settings', 'diagnostic', 'admin'];
  const {sources} = renderSources(views.map(name => ({
    file: `src/web/html/${name}.html`, type: 'html',
    content: read(`src/web/html/${name}.html`),
  })), {language: 'en', allowUnused: true, machineType: 'paddle'});
  let html = renderHome();
  views.forEach((name, index) => {
    html = fillView(html, name, sources[index].content, ' hidden');
  });
  return html.replace('width=device-width,initial-scale=1', 'width=device-width,initial-scale=1,viewport-fit=cover')
      .replace('</head>', '<link rel="stylesheet" href="/mobile-menu.css"></head>')
      .replace('</body>', '<script src="/mobile-menu.js" defer></script></body>');
}

function renderMicra() {
  return renderMobileMenu()
      .replace('<link rel="stylesheet" href="/header.css">', '')
      .replace('<link rel="stylesheet" href="/mobile-menu.css">', '')
      .replace('<script src="/preview.js" defer></script>', '')
      .replace('<script src="/mobile-menu.js" defer></script>', '<script src="/micra-preview.js" defer></script>');
}

function renderMicraScript() {
  // The Micra settings code lives in the Settings module; the preview runs it
  // as plain globals, so module qualifiers are dropped.
  const settings = read('src/web/js/settings.js'), diagnostic = read('src/web/js/diagnostic.js');
  const runtime = read('src/web/js/runtime.js'), switches = settings.search(/const\s+MICRA_SWITCHES\s*=/);
  const switchesEnd = settings.indexOf('function loadBullseyeConfig(');
  if (switches < 0 || switchesEnd < switches) throw new Error('settings.js Micra block not found');
  const pick = (source, names) => names.map(name => functionSource(source, name)).join('');
  const content = (pick(runtime, ['formatMicraCleaning', 'formatWallTimeLocal', 'pad2']) +
      settings.slice(switches, switchesEnd) +
      pick(diagnostic, ['renderMicraCloudDiagnostic', 'renderMicraWebSocket', 'ensureMicraRows']))
      .replace(/\bR\.(?:uiState\.)?/g, '');
  const extra = mockup('micra.js');
  return 'const __LINEA_MICRA__ = true;\n' + renderSources([{file: 'src/web/js/settings.js', type: 'js', content}],
      {language: 'en', allowUnused: true}).sources[0].content + (extra ? '\n' + extra : '');
}

function createServer() {
  return http.createServer((req, res) => {
    const pathname = new URL(req.url, 'http://localhost').pathname;
    if (req.method !== 'GET' && req.method !== 'HEAD') {
      res.writeHead(405, {Allow: 'GET, HEAD'}).end();
      return;
    }
    try {
      const mobileMenu = /^\/mobile-menu(?:\/(?:stats|history|settings|diagnostic|admin))?\/?$/.test(pathname);
      const micraScript = pathname === '/micra-preview.js';
      const micra = pathname === '/micra' || micraScript;
      const signals = pathname === '/preview.js';
      const appCss = pathname === '/app.css';
      const home = pathname === '/';
      const file = !home && !mobileMenu && !micra && !signals && !appCss ? staticMockup(pathname) : undefined;
      if (!home && !mobileMenu && !micra && !signals && !appCss && !file) {
        res.writeHead(404).end('Preview resource not found');
        return;
      }
      const body = micraScript ? renderMicraScript() : micra ? renderMicra() : mobileMenu ? renderMobileMenu()
          : signals ? renderSignals() + (mockup('header.js') ? '\n' + mockup('header.js') : '')
          : appCss ? read('src/web/app.css') : file ? fs.readFileSync(file[0]) : renderHome();
      res.writeHead(200, {'Content-Type': micraScript || signals ? 'text/javascript' : appCss ? 'text/css'
          : file ? file[1] : 'text/html; charset=utf-8',
        'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff',
        'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'none'; frame-ancestors 'self'; form-action 'none'"});
      res.end(req.method === 'HEAD' ? undefined : body);
    } catch (error) {
      console.error(error.message);
      res.writeHead(500).end('Unable to render the design preview');
    }
  });
}

if (require.main === module) {
  const port = Number(process.env.PORT || 4173);
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error('PORT must be an integer from 1 to 65535');
  }
  const server = createServer();
  server.on('error', error => { console.error(error.message); process.exitCode = 1; });
  server.listen(port, '127.0.0.1', () => {
    console.log(`Web UI design preview: http://127.0.0.1:${port}/`);
    console.log('Mockups are served from webui-mockups/ (local design assets, not part of the repository)');
  });
}

module.exports = {createServer};
