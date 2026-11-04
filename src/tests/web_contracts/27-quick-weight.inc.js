// Quick target-weight sheet: the active preset row's pencil and the one-shot
// ?edit_weight URL are the invocation points, and
// closing the sheet commits through the proven settings-hydrate → preset-save
// pipeline with the firmware's decigram 10.0–200.0 g preset goal contract.
{
  const qwAssert = require('assert').strict;
  const qwHomeHtml = partialHtml.home,
    qwHomeJs = viewJs.home;
  const qwCssRule = (name) => css.match(new RegExp('\\.' + name + '\\{[^}]*\\}'))?.[0] || '';
  const qwFirst = (needle) => qwHomeJs.indexOf(needle);

  qwAssert.ok(!qwHomeHtml.includes('qeditPen') && !runtimeJs.includes('shotGoalEdit'),
    'The hero card must not offer a target-weight pencil');
  qwAssert.ok(/<div id="qwBackdrop" class="qwBackdrop" inert>/.test(qwHomeHtml) &&
      /<section id="qwSheet" class="qwSheet" role="dialog" aria-modal="true"/.test(qwHomeHtml),
    'Sheet and backdrop must ship inert until opened');
  for (const id of ['qwGrab', 'qwDone', 'qwHead', 'qwPreset', 'qwNum', 'qwRatio', 'qwDef',
    'qwReset', 'qwZone', 'qwScale', 'qwMinus', 'qwPlus'])
    qwAssert.ok(qwHomeHtml.includes('id="' + id + '"'), 'Sheet element missing: ' + id);
  qwAssert.ok(/id="qwPreset".*<\/p><p class="qwLabel"/.test(qwHomeHtml) &&
      qwHomeHtml.indexOf('id="qwPreset"') < qwHomeHtml.indexOf('class="qwLabel"'),
    'The preset name must stack above the Target weight label');
  qwAssert.ok(/id="qwZone"[^>]*aria-valuemin="10\.0"[^>]*aria-valuemax="200\.0"/.test(qwHomeHtml),
    'The scale zone must declare the firmware goal bounds');
  qwAssert.ok(/<p class="qwLimit" id="qwLimitMin" hidden>/.test(qwHomeHtml) &&
      /<p class="qwLimit" id="qwLimitMax" hidden>/.test(qwHomeHtml),
    'Both range-limit pills must ship hidden next to the value');

  // The ruler stays neutral: marks outside the editable range only dim, and
  // −/+ flank it for exact tenth steps.
  qwAssert.ok(css.includes('.qwScaleIn .out{opacity:.3}') && !/\.qwScaleIn [ib]\.z/.test(css) &&
      qwHomeHtml.indexOf('id="qwMinus"') < qwHomeHtml.indexOf('id="qwZone"') &&
      qwHomeHtml.indexOf('id="qwZone"') < qwHomeHtml.indexOf('id="qwPlus"'),
    'The ruler must be neutral, dim out-of-range marks, and sit between − and +');
  qwAssert.ok(codeIncludes(qwHomeJs, 'nudge(dir * (n > 20 ? 10 : 1));') &&
      codeIncludes(qwHomeJs, 'hold = setTimeout(step, n++ ? 70 : 400);'),
    'Holding −/+ must repeat a tenth at a time, then speed up to whole grams');
  qwAssert.ok(/border-radius:2rem/.test(qwCssRule('qwLimit')) &&
      qwCssRule('guardFast i').includes('var(--gFast)') &&
      qwCssRule('guardSlow i').includes('var(--gSlow)'),
    'Limit pills use the chip format; guard dots share the zone color source');

  // The slider bounds must match the settings field the commit writes and the
  // constants the drag clamps to.
  const qwSettingsGoal = settingsHtml.match(
      /id="goalWeightG"[^>]*\bmin="(\d+)"[^>]*\bmax="(\d+)"/);
  qwAssert.ok(qwSettingsGoal, 'Settings goalWeightG input must declare min/max');
  qwAssert.ok(codeIncludes(qwHomeJs, 'const MIN = 10, MAX = 200') &&
      qwSettingsGoal[1] === '10' && qwSettingsGoal[2] === '200',
    'Sheet bounds must equal the firmware preset goal bounds');

  // Commit order: flush any pending Settings autosave and hydrate the form,
  // write the goal, then save a patch carrying only the goal.
  // A rejected save must put the previous value back so no phantom edit
  // lingers in the form (the live warnings and the brew baseline would
  // otherwise describe a value the device never accepted).
  const qwHydrate = qwFirst('if (!(await R.cleanBrewForm())) return;'),
    qwWrite = qwFirst('field.value = (dg / 10).toFixed(1)'),
    qwSave = qwFirst('R.saveBrewPreset({ goalWeightG: dg / 10 })'),
    qwRestore = qwFirst('field.value = rejected;');
  qwAssert.ok(0 <= qwHydrate && qwHydrate < qwWrite && qwWrite < qwSave,
    'Closing must hydrate, then write the goal field, then save the preset');
  qwAssert.ok(qwRestore > qwSave,
    'A rejected quick-weight save must restore the previous goal value');
  qwAssert.ok((runtimeJs.match(/export\s*\{[\s\S]*?\n\};/) || [''])[0]
      .replace(/\s/g, '').includes('cleanBrewForm'),
    'Runtime must export cleanBrewForm for the commit path');

  qwAssert.ok(codeIncludes(qwHomeJs, 'const step = -dx * BASE_DG_PER_PX * speedMul(vel.v);') &&
      codeIncludes(qwHomeJs, 'value = clamp(value + Math.trunc(pending));') &&
      codeIncludes(qwHomeJs, 'pending %= 1;') &&
      codeIncludes(qwHomeJs, 'if (dx * ctx.dx < 0) vel.v = 0;') &&
      codeIncludes(qwHomeJs, 'BASE_DG_PER_PX = 1 / PX_PER_DG') &&
      codeIncludes(qwHomeJs, 'SPEED_CAP = 3') && codeIncludes(qwHomeJs, 'SPEED_K = 1.5'),
    'The drag must accrue sub-tenth motion, reverse cleanly, track the finger 1:1, and gear up gently to ×3');
  qwAssert.ok(codeIncludes(qwHomeJs, 'PX_PER_DG = 6'),
    'The ruler must spend 6 px per 0.1 g so tenths read clearly');
  qwAssert.ok(codeIncludes(qwHomeJs,
      'if (d % 10 === 0) { tick.className = "big"; if (d >= MIN * 10 && d <= MAX * 10) {') &&
      codeIncludes(qwHomeJs, '} else if (d % 5 === 0) tick.className = "med";') &&
      /\.qwScaleIn i\.med\{/.test(css),
    'Ticks grade by tenth: whole-gram majors labeled in-bounds, half-gram mediums, tenth minors');
  qwAssert.ok(codeIncludes(qwHomeJs,
      'inner.style.transform = "translateX(" + (width / 2 - (value - base) * PX_PER_DG).toFixed(1) + "px)"'),
    'A drag must slide the pre-built ruler with one transform, not per-tick style writes');
  qwAssert.ok(codeIncludes(qwHomeJs, 'resetBtn.classList.toggle("show", value !== openValue)'),
    'Reset must appear only while the value differs from the open value');
  qwAssert.ok(codeIncludes(qwHomeJs,
      '["BREW", "RINSE", "MANUAL_NO_SCALE"].includes(s.state)'),
    'A machine shot must close the sheet');

  // URL launcher: ?edit_weight=1 opens the sheet without the Presets row. It must wait for the active preset and the admin lock,
  // open once, and strip the one-shot parameter from the address bar.
  qwAssert.ok(codeIncludes(qwHomeJs,
      'new URLSearchParams(location.search).has("edit_weight")'),
    'The edit_weight URL parameter must arm the quick-weight launcher');
  qwAssert.ok(codeIncludes(qwHomeJs, 'quickWeight.open()') &&
      codeIncludes(qwHomeJs, 'preset.goalWeightG && R.controlsMutable'),
    'The armed launcher must open only once the preset is ready and unlocked');
  qwAssert.ok(codeIncludes(qwHomeJs,
      'history.replaceState(null, "", location.pathname)'),
    'Opening from the URL must strip the one-shot parameter');
  qwAssert.ok(codeIncludes(qwHomeJs, 'return { open, close };'),
    'The controller must expose open for the URL launcher');
  qwAssert.ok(codeIncludes(runtimeJs,
      'if (p.id === presetState.activeId && m.mode === "active") {') &&
      codeIncludes(qwHomeJs, 'e.target.closest(".presetAccEdit") && open()'),
    'Only the already-active preset row may offer the quick-weight pencil');

  qwAssert.ok(/position:fixed[^}]*bottom:0[^}]*background:var\(--sf\)/.test(qwCssRule('qwSheet')) &&
      !/\.qwSheet\{[^}]*backdrop-filter/.test(css),
    'The sheet anchors to the viewport bottom on a solid surface, not frosted glass');
  qwAssert.ok(qwCssRule('qwSheet').includes('translateY(calc(100% + 4rem))') &&
      qwCssRule('qwSheet').includes('box-shadow:0 -1.1rem 2.6rem'),
    'Closed, the sheet sits past its own 3.7rem shadow so none falls on the page');
  qwAssert.ok(/touch-action:none/.test(qwCssRule('qwZone')),
    'The tick zone must own touch gestures');
  qwAssert.ok(/body\.qwOpen\{overflow:hidden\}/.test(css),
    'Page scroll must lock while the sheet is open');
  qwAssert.ok(/\.qeditPen:disabled\{display:none\}/.test(css),
    'A locked UI must hide the pencil');

  // Execute the real sheet controller against a stub DOM: open, pointer drag
  // with the accepted acceleration, clamping, Reset, commit-on-close, and the
  // drag-to-dismiss grab row.
  (async () => {
    const qwNode = (extra) => {
      const node = Object.assign({
        className: '',
        innerHTML: '',
        hidden: false,
        inert: false,
        value: '',
        children: [],
        style: {},
        classes: new Set(),
        listeners: {},
        appendChild(child) { this.children.push(child); },
        setAttribute() {},
        addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); },
        removeEventListener(type, fn) {
          const at = (this.listeners[type] || []).indexOf(fn);
          if (at >= 0) this.listeners[type].splice(at, 1);
        },
        setPointerCapture() {},
        focus() { this.focused = true; },
        getBoundingClientRect() { return {width: 320}; },
        classList: null,
        _text: '',
      }, extra);
      Object.defineProperty(node, 'textContent', {
        get() { return node._text; },
        set(v) { node._text = String(v); },
      });
      return node;
    };
    const qwClassSet = (node) => ({
      add: (...c) => c.forEach((x) => node.classes.add(x)),
      remove: (...c) => c.forEach((x) => node.classes.delete(x)),
      contains: (c) => node.classes.has(c),
      toggle: (c, on) => { on === undefined
        ? node.classes.has(c) ? node.classes.delete(c) : node.classes.add(c)
        : on ? node.classes.add(c) : node.classes.delete(c); },
    });
    const qwIds = ['qwSheet', 'qwBackdrop', 'qwZone', 'qwScale', 'qwMinus', 'qwPlus', 'qwNum', 'qwRatio', 'qwDef',
      'qwReset', 'qwPreset', 'qwDone', 'qwGrab', 'qwHead',
      'goalWeightG', 'qwLimitMin', 'qwLimitMax'];
    const qwEls = Object.fromEntries(qwIds.map((id) => {
      const node = qwNode();
      node.classList = qwClassSet(node);
      return [id, node];
    }));
    qwEls.qwRatio.firstChild = qwNode();
    qwEls.qwRatio.lastChild = qwNode();
    let qwNow = 0;
    const qwRuntime = {
      presetState: {activeId: 2, items: [{id: 2, name: 'Espresso', goalWeightG: 36, doseG: 18}]},
      cleanBrewForm: async () => { qwRuntime.hydrated++; return true; },
      saveBrewPreset: async () => { qwRuntime.saved++; return true; },
      message: () => {},
      formatCommandError: (m) => m,
      hydrated: 0,
      saved: 0,
    };
    const qwAccListeners = {};
    const qwInit = new Function('R', '$', 'document', 'addEventListener', 'performance',
      viewJs.home.slice(viewJs.home.indexOf('function initQuickWeight(')) +
      ';return initQuickWeight;')(
        qwRuntime, (id) => qwEls[id],
        {body: (() => { const body = qwNode(); body.classList = qwClassSet(body); return body; })(),
        createElement: () => {
          const node = qwNode();
          node.classList = qwClassSet(node);
          return node;
        }},
        (type, fn) => { (qwAccListeners[type] ||= []).push(fn); },
        {now: () => qwNow});
    const qwFire = (node, type, event) =>
      (node.listeners[type] || []).forEach((fn) => fn(Object.assign(
        {pointerType: 'mouse', button: 0, pointerId: 1, preventDefault() {}}, event)));
    const qwDragZone = (moves) => {
      qwFire(qwEls.qwZone, 'pointerdown', {clientX: 100});
      for (const [dx, dt] of moves) {
        qwNow += dt;
        qwFire(qwEls.qwZone, 'pointermove', {clientX: (qwDragZone.x = (qwDragZone.x || 100) + dx)});
      }
      qwFire(qwEls.qwZone, 'pointerup', {});
      qwDragZone.x = 0;
    };

    const qwCtl = qwInit();
    qwCtl.open();
    qwAssert.ok(qwEls.qwSheet.classes.has('open'), 'Opening must show the sheet');
    qwAssert.equal(qwEls.qwNum.textContent, '36.0', 'The sheet opens at the active preset goal');
    qwAssert.equal(qwEls.qwPreset.textContent, 'Espresso', 'The header shows the preset name');
    qwAssert.ok(!qwEls.qwRatio.hidden && qwEls.qwRatio.firstChild.textContent === '18.0 g in' &&
        qwEls.qwRatio.lastChild.textContent === '1:2.0',
      'The sheet shows dose, then ratio, as two pills');
    qwAssert.ok(!qwEls.qwReset.classes.has('show'), 'Reset stays hidden at the preset goal');
    qwAssert.ok(qwEls.qwZone.focused && qwEls.qwDone.disabled,
      'Opening focuses the ruler and keeps Save disabled until the value changes');

    // Slow drags stay surgical; fast drags gear up. Replicate the accepted
    // accumulator model to pin the exact deltas (values are decigrams).
    const qwModel = (moves) => {
      let value = 360, pending = 0, v = 0, prev = 0;
      for (const [dx, dt] of moves) {
        if (dx * prev < 0) v = 0;
        prev = dx;
        v = 0.75 * v + 0.25 * (Math.abs(dx) / dt);
        pending -= dx * (1 / 6) * (1 + Math.min(2, Math.abs(v) * 1.5));
        const step = Math.trunc(pending);
        pending -= step;
        value = Math.max(100, Math.min(2000, value + step));
      }
      return (value / 10).toFixed(1);
    };
    const qwSlow = [[10, 100], [10, 100], [10, 100], [10, 100], [10, 100], [10, 100]];
    qwDragZone(qwSlow);
    qwAssert.equal(qwEls.qwNum.textContent, qwModel(qwSlow),
      'A slow drag must follow the accepted fine sensitivity');
    qwAssert.ok(qwEls.qwReset.classes.has('show'), 'Editing away from the goal reveals Reset');
    qwAssert.equal(qwEls.qwDone.disabled, false, 'A changed value enables Save');
    qwAssert.equal(qwEls.qwDef.textContent, '36.0', 'Reset offers the open value');
    qwEls.qwReset.onclick();
    qwAssert.equal(qwEls.qwNum.textContent, '36.0', 'Reset restores the open value');
    qwAssert.ok(!qwEls.qwReset.classes.has('show'), 'Reset hides again at the open value');
    qwAssert.equal(qwEls.qwDone.disabled, true, 'Save disables again at the saved value');

    // A crawling drag (1 px per event, far below one tenth) must still move
    // the dial: sub-tenth credit accrues instead of dying in rounding.
    const qwCrawl = Array.from({length: 40}, () => [-1, 25]);
    qwDragZone(qwCrawl);
    qwAssert.equal(qwEls.qwNum.textContent, qwModel(qwCrawl),
      'A crawling drag must accumulate tenths');
    qwAssert.ok(Number(qwEls.qwNum.textContent) > 36,
      'A crawling drag must move the dial at all');
    qwEls.qwReset.onclick();

    const qwFastMoves = [[50, 10], [50, 10], [50, 10], [50, 10], [50, 10]];
    qwDragZone(qwFastMoves);
    qwAssert.equal(qwEls.qwNum.textContent, qwModel(qwFastMoves),
      'A fast drag must engage the acceleration gear');

    // Inertia: a flick released mid-motion keeps sliding and eases to a stop;
    // a finger that rests before lifting leaves none.
    {
      const frames = [];
      globalThis.requestAnimationFrame = (fn) => frames.push(fn);
      globalThis.cancelAnimationFrame = () => { frames.length = 0; };
      qwEls.qwReset.onclick();
      qwFire(qwEls.qwZone, 'pointerdown', {clientX: 300});
      for (let i = 1; i <= 4; i++) {
        qwNow += 10;
        qwFire(qwEls.qwZone, 'pointermove', {clientX: 300 - 30 * i});
      }
      qwFire(qwEls.qwZone, 'pointerup', {});
      const released = Number(qwEls.qwNum.textContent);
      for (let n = 0; frames.length && n < 400; n++) { qwNow += 16; frames.shift()(qwNow); }
      const settled = Number(qwEls.qwNum.textContent);
      qwAssert.ok(settled > released && !frames.length,
        'A released flick must coast further in the drag direction and stop');
      qwFire(qwEls.qwZone, 'pointerdown', {clientX: 300});
      qwNow += 10;
      qwFire(qwEls.qwZone, 'pointermove', {clientX: 270});
      qwNow += 200;
      qwFire(qwEls.qwZone, 'pointerup', {});
      qwAssert.equal(frames.length, 0, 'A resting finger must not leave inertia');
      delete globalThis.requestAnimationFrame;
      delete globalThis.cancelAnimationFrame;
    }

    // Bounds: the firmware goal range clamps however far the drag runs.
    qwEls.qwReset.onclick();
    qwDragZone([[-600, 5], [-600, 5], [-600, 5], [-600, 5], [-600, 5], [-600, 5], [-600, 5]]);
    qwAssert.equal(qwEls.qwNum.textContent, '200.0', 'Dragging left clamps at the 200 g ceiling');
    qwEls.qwReset.onclick();
    qwDragZone([[600, 5], [600, 5], [600, 5], [600, 5]]);
    qwAssert.equal(qwEls.qwNum.textContent, '10.0', 'The drag clamps at the 10 g floor');

    // −/+ taps step a tenth; Enter on a button does the same.
    qwEls.qwReset.onclick();
    qwEls.qwPlus.onpointerdown({preventDefault() {}});
    qwEls.qwPlus.onpointerup();
    qwAssert.equal(qwEls.qwNum.textContent, '36.1', 'A + tap steps up 0.1 g');
    qwEls.qwMinus.onkeydown({key: 'Enter', preventDefault() {}});
    qwEls.qwMinus.onkeydown({key: 'Enter', preventDefault() {}});
    qwAssert.equal(qwEls.qwNum.textContent, '35.9', 'Enter on − steps down 0.1 g');

    // Keyboard stepping works without pointers; tenths are the finest step.
    qwEls.qwReset.onclick();
    qwFire(qwEls.qwZone, 'keydown', {key: 'ArrowRight', preventDefault() {}});
    qwAssert.equal(qwEls.qwNum.textContent, '36.1', 'Arrow keys step by 0.1 g');
    qwFire(qwEls.qwZone, 'keydown', {key: 'ArrowLeft', preventDefault() {}});

    // Closing without changes must not hit the save pipeline.
    qwFire(qwEls.qwSheet, 'keydown', {key: 'Escape'});
    qwAssert.ok(!qwEls.qwSheet.classes.has('open'), 'Escape closes the sheet');
    await new Promise((r) => setTimeout(r, 0));
    qwAssert.equal(qwRuntime.hydrated, 0, 'An unchanged close must not hydrate or save');

    // The URL launcher path: open() reopens at the preset goal without a hero.
    qwCtl.open();
    qwAssert.ok(qwEls.qwSheet.classes.has('open'),
      'The exposed open must launch the sheet without the hero pencil');
    qwAssert.equal(qwEls.qwNum.textContent, '36.0', 'A programmatic open starts at the preset goal');
    qwFire(qwEls.qwSheet, 'keydown', {key: 'Escape'});
    await new Promise((r) => setTimeout(r, 0));

    // The pencil on the active row's target is the in-page invocation point;
    // only the active row renders it, and taps elsewhere never open the sheet.
    const qwAccClick = (target) =>
      (qwAccListeners.click || []).forEach((fn) => fn({target}));
    qwAccClick({closest: () => null});
    qwAssert.ok(!qwEls.qwSheet.classes.has('open'),
      'Taps off the pencil must not open the sheet');
    qwAccClick({closest: (sel) => (sel === '.presetAccEdit' ? {} : null)});
    qwAssert.ok(qwEls.qwSheet.classes.has('open'),
      'The active row pencil must open the sheet');
    qwAssert.equal(qwEls.qwNum.textContent, '36.0',
      'The sheet still opens at the active preset goal');
    qwFire(qwEls.qwSheet, 'keydown', {key: 'Escape'});
    await new Promise((r) => setTimeout(r, 0));

    // Done commits the changed value through hydrate → goal field → save.
    qwCtl.open();
    qwDragZone([[50, 10], [50, 10], [50, 10], [50, 10], [50, 10]]);
    const qwEdited = qwEls.qwNum.textContent;
    qwEls.qwDone.onclick();
    qwAssert.ok(!qwEls.qwSheet.classes.has('open'), 'Done closes the sheet');
    await new Promise((r) => setTimeout(r, 0));
    qwAssert.equal(qwRuntime.hydrated, 1, 'Done must hydrate the settings form');
    qwAssert.equal(qwEls.goalWeightG.value, qwEdited,
      'Done must write the one-decimal goal into goalWeightG');
    qwAssert.equal(qwRuntime.saved, 1, 'Done must save the brew preset');

    // A long grab-row drag dismisses and still commits an edited value.
    qwCtl.open();
    qwDragZone([[50, 10], [50, 10], [50, 10], [50, 10], [50, 10]]);
    const qwDismissed = qwEls.qwNum.textContent;
    qwFire(qwEls.qwGrab, 'pointerdown', {clientY: 500});
    qwFire(qwEls.qwGrab, 'pointermove', {clientY: 660});
    qwFire(qwEls.qwGrab, 'pointerup', {});
    await new Promise((r) => setTimeout(r, 320));
    qwAssert.ok(!qwEls.qwSheet.classes.has('open'),
      'A long grab drag must dismiss the sheet');
    qwAssert.equal(qwRuntime.saved, 2, 'Dismiss must commit the pending edit');
    qwAssert.equal(qwEls.goalWeightG.value, qwDismissed,
      'Dismiss must write the edited goal before saving');

    // A short grab wiggle never dismisses.
    qwCtl.open();
    qwFire(qwEls.qwGrab, 'pointerdown', {clientY: 500});
    qwFire(qwEls.qwGrab, 'pointermove', {clientY: 540});
    qwFire(qwEls.qwGrab, 'pointerup', {});
    await new Promise((r) => setTimeout(r, 0));
    qwAssert.ok(qwEls.qwSheet.classes.has('open'),
      'A short grab drag must keep the sheet open');
    qwAssert.equal(qwRuntime.saved, 2, 'An unchanged sheet must not save on wiggle');

    // Margin-bounded range: with both sides on the dial offers only targets
    // the firmware accepts for these margins (target ≥ 4 × extra yield and
    // ≥ less yield + 5 g; target + extra yield ≤ 200 g), the limit pills mark
    // the two ends, and the ruler tints reference zones outside the range.
    qwRuntime.presetState.items = [{
      id: 2, name: 'Espresso', goalWeightG: 36,
      slowExtractionGuardEnabled: true, slowMarginG: 3,
      fastExtractionGuardEnabled: true, fastMarginG: 8,
    }];
    const qwZoneAttrs = {};
    qwEls.qwZone.setAttribute = (k, v) => { qwZoneAttrs[k] = String(v); };
    qwCtl.open();
    qwAssert.equal(qwEls.qwNum.textContent, '36.0', 'The sheet opens at the in-range goal');
    qwAssert.equal(qwZoneAttrs['aria-valuemin'], '32.0',
      'The slider floor keeps the extra yield within a quarter of the target');
    qwAssert.equal(qwZoneAttrs['aria-valuemax'], '192.0',
      'The slider ceiling keeps target plus extra yield within 200 g');
    qwAssert.equal(qwEls.qwLimitMin.hidden, true, 'The floor pill starts hidden');
    qwAssert.equal(qwEls.qwLimitMax.hidden, true, 'The ceiling pill starts hidden');
    const qwStrip = qwEls.qwScale.children[0].children;
    const qwLabel = (v) => qwStrip.find((n) => n._text === String(v));
    qwAssert.ok(qwLabel('40') && !qwLabel('40').classes.has('out'),
      'In-range gram labels stay at full strength');
    qwAssert.ok(qwLabel('30').classes.has('out') && qwLabel('195').classes.has('out'),
      'Labels outside the range dim without guard colors');
    qwAssert.ok(!qwLabel('5') && !qwLabel('205'),
      'Filler ticks past the firmware bounds stay unlabeled');
    qwAssert.ok(qwStrip.some((n) => !n._text && n.classes.has('out')) &&
        qwStrip.some((n) => !n._text && !n.classes.has('out')),
      'Minor ticks dim outside the range only');
    qwDragZone([[600, 5], [600, 5], [600, 5], [600, 5]]);
    qwAssert.equal(qwEls.qwNum.textContent, '32.0',
      'The drag clamps at the lowest target the margins allow');
    qwAssert.equal(qwEls.qwLimitMin.hidden, false,
      'The floor pill appears at the bottom of the target range');
    qwAssert.ok(qwEls.qwLimitMin.classes.has('toneSlow') &&
        qwEls.qwLimitMax.classes.has('toneFast'),
      'Limit pills adopt the tint of the side that bounds them');
    qwEls.qwReset.onclick();
    qwFire(qwEls.qwSheet, 'keydown', {key: 'Escape'});
    await new Promise((r) => setTimeout(r, 0));
    qwAssert.equal(qwRuntime.saved, 2,
      'A guard-bounded session closed without edits must not save');
  })().catch((error) => { console.error(error); process.exitCode = 1; });
}
// The dial buttons keep line-height:1: a later font shorthand would reset it.
if (!/\.qwDial button\{[^}]*font:inherit;[^}]*line-height:1[;}]/.test(css)) {
  throw new Error('Quick-weight dial buttons must set line-height after the font shorthand');
}
// The ~2,400-node ruler is built on the first open and its out-of-range ticks
// are restyled only when the range changes.
if (!codeIncludes(viewJs.home, 'if(!ticks.length)build()') ||
    !codeIncludes(viewJs.home, 'if(lo!==outLo||hi!==outHi){')) {
  throw new Error('The quick-weight ruler must be built lazily and restyled only on range changes');
}
