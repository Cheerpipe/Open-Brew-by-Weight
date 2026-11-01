{
  // API error rendering: every error body carries a stable `reason` code
  // (docs/INTEGRATION_API.md); the Web UI renders locale[reason] first, then
  // locale[error], then the server `message` prose, so unmapped sentences
  // keep today's bytes. The err.* catalog is the single editable copy of the
  // sentences (the firmware table is generated from it at build time), so
  // this contract pins map<->catalog agreement and the render chain — never
  // the sentence values themselves.
  const assert = require('assert').strict;
  const vm = require('vm');
  const catalog = JSON.parse(
      fs.readFileSync(path.join(sketchDir, 'web', 'locales', 'en.json'), 'utf8')).strings;
  const errKeys = Object.keys(catalog).filter((key) => key.startsWith('err.'));
  assert.equal(errKeys.length, 240, 'the err.* section holds the 240 frozen reason codes');

  // The map in runtime.js references each err.* key exactly once, and every
  // referenced key exists (the localize check enforces the same at build).
  const referenced = [...rawRuntimeJs.matchAll(/__WEBUI_TEXT__\(\s*"(err\.[a-z0-9_]+)"/g)]
      .map((match) => match[1]);
  assert.equal(referenced.length, errKeys.length, 'one map entry per err.* key');
  assert.deepEqual([...new Set(referenced)].sort(), [...errKeys].sort(),
      'the runtime map and the err.* catalog agree');

  // The render chain itself, executed against the authoring source.
  const run = vm.createContext({
    __WEBUI_TEXT__: (key) => catalog[key],
  });
  vm.runInContext(
      blockAt(rawRuntimeJs, 'const errorReasonTexts = {') + '\n' +
          blockAt(rawRuntimeJs, 'function errorText(data'),
      run);
  assert.equal(run.errorText({reason: 'CRASH_DELETE_NOT_CONFIRMED', message: 'anything'}),
      catalog['err.crash_delete_not_confirmed'], 'a mapped reason renders the locale text');
  assert.equal(run.errorText({reason: 'SHOT_STORAGE_BUSY', error: 'SHOT_TRACE_BUSY',
    message: catalog['err.shot_storage_busy']}),
      catalog['err.shot_storage_busy'], 'the reason tier wins over error and message');
  assert.equal(run.errorText({error: 'AUTH_COOLDOWN', message: 'server prose'}),
      catalog['err.auth_cooldown'], 'without a reason the legacy error tier applies');
  assert.equal(run.errorText({reason: 'SOME_FUTURE_CODE', message: 'Plain server prose.'}),
      'Plain server prose.', 'an unknown code falls back to the message');
  assert.equal(run.errorText({reason: 'SOME_FUTURE_CODE', error: 'ALSO_UNKNOWN', message: 'Prose.'}),
      'Prose.', 'unknown reason and error fall back to the message');
  assert.equal(run.errorText({}, 'Static fallback'), 'Static fallback',
      'no codes and no message leave the fallback');
  assert.equal(run.errorText(null, 'Fallback'), 'Fallback', 'no payload leaves the fallback');

  // The api() layer resolves through the chain and exposes the reason.
  assert.ok(codeIncludes(runtimeJs, 'new Error(errorText(data,'),
      'api() resolves error text through the render chain');
  assert.ok(codeIncludes(runtimeJs, 'err.reason = data.reason || ""'),
      'api() carries the reason on the error object');
  assert.ok(codeIncludes(runtimeJs, 'reason: data.reason || ""'),
      'apiBinary() carries the reason on the error object');
  assert.ok(codeIncludes(runtimeJs, 'const detail = errorText(e)'),
      'formatCommandError renders through the chain');
}
