// Exercise activity timing independently of automatic polls and session expiry.
{
  const assert = require('assert').strict;
  const source = runtimeJs.slice(runtimeJs.search(/let\s+webUiPowerUntil\s*=/),
      runtimeJs.search(/const\s+WEB_UI_INACTIVITY_MS\s*=/));
  let now = 100, owner = true;
  const doc = {hidden: false};
  const make = new Function('performance', 'document', 'getOwner',
      source.replace(/export /g, '').replace(/webUiOwner/g, 'getOwner()') +
      ';return {activity:noteWebUiPowerActivity,seconds:webUiPowerSeconds}');
  const lease = make({now: () => now}, doc, () => owner);
  lease.activity();
  assert.equal(lease.seconds(), 30);
  now += 160000;
  assert.equal(lease.seconds(), 20);
  // Polls obtain a header but never advance the human deadline.
  for (let i = 0; i < 100; ++i) assert.equal(lease.seconds(), 20);
  doc.hidden = true;
  assert.equal(lease.seconds(), 0);
  lease.activity();
  doc.hidden = false;
  now += 20000;
  assert.equal(lease.seconds(), 0);
  lease.activity();
  assert.equal(lease.seconds(), 30);
  owner = false;
  now += 180000;
  lease.activity();
  assert.equal(lease.seconds(), 0);
  assert(network.includes('"powerManagementEnabled"'));
  const handler = network.slice(network.indexOf('const bool diagnosticPagePatch'),
      network.indexOf('const bool diagnosticPagePatch') + 200);
  assert(handler.includes('powerManagementEnabled'));
  assert(network.includes('diagnosticPagePatch && !self.requireAdminUnlock(request)'));
  assert(network.includes('notePowerWebActivity(millis()'));
  assert(codeIncludes(runtimeJs, "options.headers['X-WebUI-Activity']"));
}

// Real config waiter: APPLY_CONFIG remains APPLIED after asynchronous saving.
{
  const assert = require('assert').strict;
  const admin = viewJs.admin;
  const source = admin.slice(admin.indexOf('async function waitSaved('),
      admin.indexOf('function controlChanged('));
  const run = async () => {
    let now = 0, snapshots = [];
    const waiter = new Function('R', 'Date', source + ';return waitSaved')({
      api: async () => {
        assert(snapshots.length, 'unexpected extra poll after durable config');
        return snapshots.shift();
      },
      sleep: async () => { now += 150; }
    }, {now: () => now});
    for (const key of ['powerManagementEnabled', 'showDiagnosticPage']) {
      for (const value of [true, false]) {
        const snapshot = (state, revision, pending, failed = false, actual = value) => ({
          lastCommand: {requestId: 7, state},
          config: {revision, persistPending: pending, persistFailed: failed, [key]: actual}
        });
        snapshots = [snapshot('QUEUED', 12, false, true), snapshot('APPLIED', 12, false, true),
          snapshot('APPLIED', 13, true), snapshot('APPLIED', 13, false, false, !value),
          snapshot('APPLIED', 14, false)]; // Coalesced later revision is durable too.
        await waiter(7, 'the setting', {baseRevision: 12, [key]: value}, key);
        assert.equal(snapshots.length, 0);
        snapshots = [snapshot('APPLIED', 1, false)];
        await waiter(7, 'the setting', {baseRevision: 0xffffffff, [key]: value}, key);
        snapshots = [snapshot('APPLIED', 13, true, true)];
        await assert.rejects(waiter(7, 'the setting', {baseRevision: 12, [key]: value}, key),
            /Could not save the setting/);
        for (const state of ['FAILED', 'CANCELED']) {
          snapshots = [snapshot(state, 12, false)];
          await assert.rejects(waiter(7, 'the setting', {baseRevision: 12, [key]: value}, key),
            /Could not save the setting/);
        }
        snapshots = Array.from({length: 54}, () => snapshot('APPLIED', 13, true));
        await assert.rejects(waiter(7, 'the setting', {baseRevision: 12, [key]: value}, key),
            /Timeout saving the setting/);
      }
    }
  };
  run().catch(error => { console.error(error); process.exitCode = 1; });
}

// Shared durable config save: revisioned request, durable acknowledgement;
// failures reject so section Save keeps its dirty draft for retry or revert.
{
  const assert = require('assert').strict;
  const admin = viewJs.admin;
  const source = admin.slice(admin.indexOf('async function saveDurableConfigKey('),
      admin.indexOf('async function saveBleSettings('));
  const run = async () => {
    const el = {checked: true};
    let reject = false, posted, waited = 0;
    const handler = new Function('R', 'waitSaved', '$', source + ';return saveDurableConfigKey')(
        {
          withCommandGate: fn => fn(),
          withBaseRev: fields => ({...fields, baseRevision: 12}),
          body: JSON.stringify,
          api: async (_, options) => {
            posted = JSON.parse(options.body);
            if (reject) throw new Error('Revision conflict');
            return {requestId: 5};
          }, message: () => {}
        },
        async (id, label, config, key) => {
          assert.equal(id, 5);
          assert.equal(label, 'power management');
          assert.equal(key, 'powerManagementEnabled');
          assert.deepEqual(config, posted);
          ++waited;
        }, () => el);
    await handler('powerManagementEnabled', 'power management');
    assert.deepEqual(posted, {powerManagementEnabled: true, baseRevision: 12});
    assert.equal(waited, 1);
    assert.equal(el.checked, true);
    reject = true;
    await assert.rejects(handler('powerManagementEnabled', 'power management'), /Revision conflict/);
  };
  run().catch(error => { console.error(error); process.exitCode = 1; });
}
