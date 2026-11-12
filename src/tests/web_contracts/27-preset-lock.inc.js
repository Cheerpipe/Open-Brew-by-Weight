// Home's preset lock: locked, only a press-and-hold switches presets, a tap's
// hint shows on one row at a time, and the lock itself is a per-device hold.
{
  const assert = require('assert').strict, vm = require('vm');
  const accordion = blockAt(runtimeJs, 'function renderHomePresetAccordion(');
  assert.ok(codeIncludes(accordion, 'locked = !appSetting("presetUnlocked")') &&
      codeIncludes(accordion, 'sig = JSON.stringify([openId, canSelect, locked,') &&
      codeIncludes(accordion, 'if (locked) presetLockHint(item);') &&
      codeIncludes(accordion, '() => !appSetting("presetUnlocked") && !item.classList.contains("open") && !head.disabled'),
  'Locked taps must hint and only an inactive, enabled preset may be held');
  assert.ok(codeIncludes(blockAt(runtimeJs, 'const APP_SETTING_KEYS = {'), 'presetUnlocked: "obbwAppPresetUnlocked"'),
      'The lock is a This device setting that starts locked');
  const panel = partialHtml.home.slice(partialHtml.home.indexOf('id="presetPanel"'),
      partialHtml.home.indexOf('id="homePresetBlock"'));
  assert.ok(panel.includes('id="presetLockButton"'), 'The lock sits in the Presets legend');
  assert.ok(css.includes('conic-gradient(var(--ac) calc(var(--hold,0)*360deg)'),
      'A hold fills a ring around the lock icon');
  assert.ok(css.includes('width:calc(var(--hold,0)*100%);background:var(--pa)') &&
      codeIncludes(accordion, 'item.style.setProperty("--hold", v);') &&
      codeIncludes(accordion, 'item.classList.add("held");'),
  'Holding a row fills it like a progress bar in the selected preset colour');

  const timers = new Map();
  let timer = 0;
  const flush = () => { for (const [id, t] of [...timers]) { timers.delete(id); t(); } };
  const context = vm.createContext({
    performance: {now: () => 1000}, requestAnimationFrame: () => 1, cancelAnimationFrame() {},
    setTimeout: (fn) => (timers.set(++timer, fn), timer), clearTimeout: (id) => timers.delete(id),
  });
  vm.runInContext(runtimeJs.slice(runtimeJs.indexOf('const HOLD_MS'),
      runtimeJs.indexOf('function renderPresetLock(')) + blockAt(runtimeJs, 'function presetLockHint('), context);

  // A hold runs once its timer ends; an early release, a scroll or a list that
  // is not ready never does. Space released early is an ordinary click.
  const element = () => {
    const on = {};
    return {on, clicks: 0, click() { this.clicks++; },
      addEventListener: (type, fn) => (on[type] ||= []).push(fn)};
  };
  const fire = (el, type, extra = {}) => el.on[type].forEach((fn) =>
    fn({button: 0, clientX: 0, clientY: 0, key: ' ', repeat: false, preventDefault() {}, ...extra}));
  let ready = true, done = 0;
  const painted = [], el = element();
  context.pressAndHold(el, () => ready, (v) => painted.push(v), () => done++);
  fire(el, 'pointerdown');
  flush();
  assert.equal(done, 1);
  assert.equal(painted.at(-1), 0, 'the ring empties when the hold completes');
  assert.equal(vm.runInContext('holdClickGuard', context), 1600, 'the click ending a hold is swallowed');
  fire(el, 'pointerdown');
  fire(el, 'pointerup');
  fire(el, 'pointerdown');
  fire(el, 'pointermove', {clientX: 20});
  ready = false;
  fire(el, 'pointerdown');
  flush();
  assert.equal(done, 1, 'releasing, scrolling or an unlocked list never switches');
  ready = true;
  fire(el, 'keydown');
  fire(el, 'keyup');
  assert.equal(el.clicks, 1, 'a short Space press stays a tap');
  fire(el, 'keydown', {key: 'Enter'});
  flush();
  fire(el, 'keyup', {key: 'Enter'});
  assert.deepEqual([done, el.clicks], [2, 1], 'a held key switches without a click');

  // One hint at a time, cleared again by its timer.
  const classes = () => {
    const set = new Set();
    return {contains: (c) => set.has(c), add: (c) => set.add(c), remove: (c) => set.delete(c),
      toggle: (c, on) => (on ? set.add(c) : set.delete(c))};
  };
  const presets = [{id: 1, mode: 'A'}, {id: 2, mode: 'B'}, {id: 3, mode: 'C'}];
  const rows = presets.map((p) => {
    const badge = {classList: classes(), textContent: p.mode,
      replaceChildren(...nodes) { this.textContent = nodes.filter((n) => typeof n === 'string').join(''); }};
    return {dataset: {id: String(p.id)}, classList: classes(), offsetWidth: 1, querySelector: () => badge, badge};
  });
  Object.assign(context, {
    presetState: {items: presets}, presetBadge: (p) => p.mode,
    $: (id) => (id === 'homePresetAcc' ? {children: rows}
      : {querySelector: () => ({cloneNode: () => ({})})}),
  });
  const says = () => rows.map((r) => r.badge.textContent);
  context.presetLockHint(rows[1]);
  assert.deepEqual(says(), ['A', 'Locked · hold to switch', 'C']);
  assert.ok(rows[1].classList.contains('lockNudge'));
  context.presetLockHint(rows[2]);
  assert.deepEqual(says(), ['A', 'B', 'Locked · hold to switch'], 'the hint moves to the tapped row');
  flush();
  assert.deepEqual(says(), ['A', 'B', 'C']);
  assert.ok(rows.every((r) => !r.badge.classList.contains('lockSay')));
}
