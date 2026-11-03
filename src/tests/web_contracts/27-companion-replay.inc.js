// Per-session fields of the streams the companion replays from its merged
// models: a value computed for one stream session is stale when the companion
// hands its copy to a later screen. src/tests/fixtures/companion_replay_v1.json names
// each one with the companion's treatment, and the companion's tests hold its
// code to a copy. A session value read through a local variable is not seen.
{
  const assert = require('assert').strict;
  const read = (file) => fs.readFileSync(path.join(sketchDir, file), 'utf8');
  const source = read('network/OpenBrewByWeightHomeStream.inc') +
      read('network/OpenBrewByWeightSettingsStream.inc');
  const senders = {home: 'sendHomeStream', diagnostic: 'sendDiagnosticStream', settings: 'sendSettingsStream'};
  const fixture = JSON.parse(fs.readFileSync(
      path.join(sketchDir, 'tests', 'fixtures', 'companion_replay_v1.json'), 'utf8'));
  assert.equal(fixture.version, 1);
  assert.deepEqual(Object.keys(fixture.sessionFields).filter((s) => !(s in senders)), [],
      'companion_replay_v1.json names only replayed streams');
  for (const [stream, sender] of Object.entries(senders)) {
    const start = source.indexOf(`bool OpenBrewByWeightNetwork::${sender}(`);
    assert(start >= 0, `${sender} not found`);
    const found = [...source.slice(start, source.indexOf('\n}\n', start)).matchAll(/\.field\(\s*"([^"]+)"([\s\S]*?)\);/g)]
        .filter((m) => /\bsession\./.test(m[2])).map((m) => m[1]).sort();
    const listed = fixture.sessionFields[stream] || {};
    assert.deepEqual(Object.keys(listed).sort(), found,
        `${stream} per-session fields match src/tests/fixtures/companion_replay_v1.json (and the companion handles them)`);
    for (const [field, how] of Object.entries(listed))
      assert.equal(how, 'clock', `${stream}.${field}: the companion only ages clocks`);
  }
}
