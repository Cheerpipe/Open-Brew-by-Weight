// Grind hint card (grind hints v2): one fixture per firmware state, the bar
// geometry on a 27-36 s window, and the Ignore key of decision D6.
{
  const assert = require('assert');
  const view = new Function(
      `const sub=(t,v)=>t.replace(/\\{(\\d)\\}/g,(_,i)=>v[+i]);${
        blockAt(runtimeJs, 'function grindHintView(')};return grindHintView;`)();
  const preset = (grindHint, extra = {}) => Object.assign(
      {id: 3, grindHintsEnabled: true, fastBeforeS: 27, slowAfterS: 36, goalWeightG: 36, grindHint}, extra);
  const hint = (extra) => Object.assign({state: 'maintenance', direction: 'coarser', tier: 'likely',
    bin: 'small', shots: 4, offsetS: 3, bandS: 2.25, atLeast: false, segmentStartId: 41}, extra);

  assert.equal(view(undefined), null);
  assert.equal(view(preset(null)), null);
  assert.equal(view(preset(hint(), {grindHintsEnabled: false})), null);

  const maintenance = view(preset(hint()));
  assert.deepEqual([maintenance.cls, maintenance.tag, maintenance.conf, maintenance.shots,
    maintenance.dir, maintenance.mag, maintenance.relearn],
  ['isSlow', 'Grind hint', 'Likely', '4 shots', 'Grind a touch coarser', 'about 3 s slow', true]);
  assert.equal(maintenance.text,
      'Your last 4 shots on this setting average 34.5 s; the middle of your 27–36 s window is 31.5 s.');
  // Window at 12-88 %: the 31.5 +- 2.25 s band sits at 31 % from each edge.
  assert.deepEqual(maintenance.band.map(Math.round), [31, 31]);
  assert.equal(Math.round(maintenance.level), 75);
  assert.deepEqual(maintenance.axis, ['27 s', 31.5, '36 s']);

  const dialIn = view(preset(hint({state: 'dial_in', direction: 'finer', tier: 'tentative',
    bin: 'large', shots: 1, offsetS: -9.5, atLeast: true})));
  assert.deepEqual([dialIn.cls, dialIn.tag, dialIn.conf, dialIn.shots, dialIn.dir, dialIn.mag],
      ['isFast', 'Dialling in', 'Early read', '1 shot', 'Grind a lot finer', 'at least 10 s fast']);
  assert.equal(dialIn.text, 'Your first shot on this setting reached 36 g at 22 s; your window is 27–36 s.');
  assert.equal(dialIn.level, 1);

  const checking = view(preset(hint({state: 'checking', offsetS: 1})));
  assert.deepEqual([checking.cls, checking.tag, checking.conf, checking.shots, checking.dir,
    checking.mag, checking.relearn],
  ['isCheck', 'Checking your change', 'Next shot tells', '', 'Moving the right way', 'now 32.5 s', false]);

  const vary = view(preset(hint({state: 'vary', direction: null, tier: null, bin: null, shots: 6,
    offsetS: 0}), {fastBeforeS: 30, slowAfterS: 32}));
  assert.deepEqual([vary.cls, vary.tag, vary.conf, vary.shots, vary.dir, vary.mag],
      ['isSpread', 'Shots vary', '', '6 shots', 'Shots vary more than this window allows', '']);
  assert(vary.text.includes('30–32 s window'));

  // Ignore holds until the advice changes: same preset, grind setting,
  // direction and tier keep it; a tier escalation or a new segment clears it.
  const key = (extra) => view(preset(hint(extra))).key;
  assert.equal(key({offsetS: 3.4, shots: 5, bin: 'medium'}), key({}));
  assert.notEqual(key({tier: 'confident'}), key({}));
  assert.notEqual(key({segmentStartId: 42}), key({}));
  assert.notEqual(key({direction: 'finer'}), key({}));
  assert.notEqual(view(preset(hint(), {id: 4})).key, key({}));
  assert.equal(key({state: 'vary', direction: null, tier: null}), 'obbwCard:grind:3:41:vary:');

  if (!codeIncludes(viewJs.home, '$("grindHintLater").onclick = () => R.dismissCard($("grindHintCard").dataset.key)') ||
      !codeIncludes(viewJs.home, '$("grindHintNewBeans").onclick = () => R.startNewCoffee()') ||
      !partialHtml.home.includes('<button id="grindHintLater" class="btnGlyph ghost" type="button"><span class="t">Ignore</span>') ||
      !partialHtml.home.includes('<span class="t">Relearn</span>') ||
      !codeIncludes(runtimeJs, 'reveal(card, !!v && !cardDismissed(v.key))') ||
      !codeIncludes(runtimeJs, '$("grindHintNewBeans").hidden = !v.relearn'))
    throw new Error('Grind hint card must offer Relearn and Ignore, and Ignore must hold per advice (D6)');
}
// Inline preset rename: the 4 s Settings poll leaves a card being renamed alone,
// and Enter or Escape settle it once even though removing the input blurs it.
{
  const assert = require('assert').strict;
  const node = () => ({parentNode: {}, focus() {}, select() {}, setAttribute() {},
    replaceWith(next) { this.parentNode = null; next.parentNode = {}; this.onblur?.(); }});
  const run = (key) => {
    const commits = [], renders = [], input = node(), title = node();
    new Function('window', '$', 'document', 'commitRenamePreset', 'renderAllPresetUi',
        `${blockAt(runtimeJs, 'function startRenamePreset(')};return startRenamePreset;`)(
        {matchMedia: () => ({matches: false})}, () => null, {createElement: () => input},
        (p, v) => commits.push(v), () => renders.push(1))({id: 2, name: 'Old'}, title);
    input.value = 'New';
    input.onkeydown({key, preventDefault() {}});
    return {commits, renders};
  };
  assert.deepEqual(run('Enter'), {commits: ['New'], renders: []}, 'Enter commits once');
  assert.deepEqual(run('Escape'), {commits: [], renders: [1]}, 'Escape cancels without committing');
  const cards = {querySelector: () => ({}), replaceChildren() { throw new Error('rebuilt'); }};
  new Function('$', 'presetState', `let presetCardsSig = "";${blockAt(runtimeJs, 'function renderPresetCards(')}` +
      ';return renderPresetCards;')(() => cards, {rev: 1, activeId: 1, selectedId: 1, items: []})('presetCards');
}
