// Settings brew validation: every cross-field rule reports on the field the
// user edited, phrased from that side opening with the field's Settings form
// label ("Name (unit)", never the field's own current value), then the
// blocking limit, the source field, and its settings section; multiple edited
// fields in conflict yield one error each. The battery executes the real
// validator (rangeCheck, number helpers, and the rule table) against a stub
// form with a controlled brew baseline.
{
  const svAssert = require('assert').strict;
  const svCut = (start, end) => runtimeJs.slice(runtimeJs.indexOf(start),
    runtimeJs.indexOf(end));
  // The field validator lives in the Settings module and reads the runtime's
  // number helpers through R; the live-warning twin follows it.
  const svValidatorSrc =
    svCut('function number(', 'function sToMs(') +
    svCut('function formNumber(', 'const sub = (t, vals)') +
    svCut('const sub = (t, vals)', 'function updateStaticIpFieldsState(') +
    ';const R={number,formNumber,sub,uiState,settingsSectionEls};' +
    viewJs.settings.slice(viewJs.settings.indexOf('function rangeCheck('),
        viewJs.settings.indexOf('\n}', viewJs.settings.indexOf('function validateBrewClient(')) + 2) +
    ';R.validateBrewClient=validateBrewClient;' + blockAt(viewJs.settings, 'function brewWarningList(');
  // Field defaults mirror the Settings form: steps come from the shipped
  // inputs (asserted below) and values form one consistent stored state.
  const svFieldSpec = {
    doseG: ['number', '0.1', '18'],
    goalWeightG: ['number', '0.1', '36'],
    operationalWallS: ['number', '1', '45'],
    bbwProtectionS: ['number', '0.1', '12'],
    weightOffsetBaselineG: ['number', '0.01', '1.5'],
    bbwAlphaBaseline: ['number', '0.01', '0.3'],
    fastBeforeS: ['number', '0.5', '27'],
    slowAfterS: ['number', '0.5', '34'],
    fastMarginG: ['number', '0.1', '4'],
    slowMarginG: ['number', '0.1', '3'],
    fastRampS: ['number', '0.5', '5'],
    slowRampS: ['number', '0.5', '5'],
    scaleLossFixedS: ['number', '0.1', ''],
    retareWindowS: ['number', '0.1', '5'],
    fastExtractionGuardEnabled: ['checkbox', '', true],
    slowExtractionGuardEnabled: ['checkbox', '', true],
    autoRetare: ['checkbox', '', false],
  };
  // The live brew-warning engine spans the runtime (shared aria and clear
  // helpers) and the Settings module (signature-gated refresh and clears).
  const svEngineSrc = runtimeStateDecl +
    ['refreshFieldAria', 'clearBrewWarningsDom', 'clearBrewWarnings']
        .map((n) => blockAt(runtimeJs, `function ${n}(`)).join('\n') +
    ';const R={uiState,validateBrewClient,settingsSectionEls,refreshFieldAria,clearBrewWarningsDom};' +
    ['brewWarningList', 'refreshBrewWarnings', 'clearBrewFieldErrors']
        .map((n) => blockAt(viewJs.settings, `function ${n}(`)).join('\n');
  const svValidate = (edit, setup) => {
    const fields = {};
    for (const [id, [type, step, init]] of Object.entries(svFieldSpec))
      fields[id] = {id, type, step, value: type === 'checkbox' ? '' : init,
        checked: type === 'checkbox' ? init : false, disabled: false,
        validity: {valid: true}};
    if (setup) setup(fields);
    const baseline = {};
    for (const el of Object.values(fields))
      baseline[el.id] = el.type === 'checkbox' ? el.checked : el.value;
    if (edit) edit(fields);
    const api = new Function('$', 'document', 'settingsSectionEls', 'uiState',
      svValidatorSrc +
      ';return {validateBrewClient, brewWarningList, rules: BREW_CROSS_RULES};')(
      (id) => fields[id],
      {documentElement: {classList: {contains: () => false}}},
      (section) => (section === 'brew' ? Object.values(fields) : []),
      {formBaseline: baseline});
    return {errors: api.validateBrewClient(), warnings: api.brewWarningList(),
            rules: api.rules};
  };
  const svOne = (result, id, msg) => {
    svAssert.equal(result.errors.length, 1, JSON.stringify(result.errors));
    svAssert.equal(result.errors[0].id, id);
    svAssert.equal(result.errors[0].msg, msg);
  };

  // (a) The factory state is valid; a self-range violation is phrased with
  // the field's UI label and unit.
  svAssert.deepEqual(svValidate().errors, []);
  const svA = svValidate((f) => {
    f.goalWeightG.value = '250';
  });
  svOne(svA, 'goalWeightG', 'Target yield (g) must be from 10 to 200 g.');

  // (b) Tenths are the finest step the firmware stores; a finer value is
  // rejected with the step spelled out. An emptied field reports only its
  // own required-value error: cross rules never compare Number("") === 0.
  svOne(svValidate((f) => {
    f.goalWeightG.value = '36.55';
  }), 'goalWeightG', 'Target yield (g) must be from 10 to 200 g in 0.1 steps.');
  svOne(svValidate((f) => {
    f.fastBeforeS.value = '';
  }), 'fastBeforeS', 'Window start (s) is required (2.5–58 s).');

  // (c) The window starts at least 2 s after the earliest weight stop: the
  // edited side is blamed, phrased from its own side with the offset named.
  svOne(svValidate((f) => {
    f.fastBeforeS.value = '13';
  }), 'fastBeforeS',
  'Window start (s) must be at least 14.0 s (Earliest weight stop + 2 s, Time limits).');
  svOne(svValidate((f) => {
    f.bbwProtectionS.value = '26';
  }), 'bbwProtectionS',
  'Earliest weight stop (s) must not exceed 25.0 s (Window start − 2 s, Recipe).');

  // (d) The window ends at least 1 s after it starts and 1 s before the max
  // shot time; a stored conflict with nothing edited blames the rule's owner.
  svOne(svValidate((f) => {
    f.slowAfterS.value = '27.5';
  }), 'slowAfterS', 'The window must end at least 1 s after it starts (28 s or later).');
  svOne(svValidate((f) => {
    f.operationalWallS.value = '34';
  }), 'operationalWallS',
  'Max shot time (s) must be at least 35.0 s (Window end + 1 s, Recipe).');
  svOne(svValidate(null, (f) => {
    f.slowAfterS.value = '50';
  }), 'slowAfterS',
  'Window end (s) must not exceed 44.0 s (Max shot time − 1 s, Time limits).');

  // (e) Margins are relative to the target and checked only while their side
  // is on: extra yield up to a quarter of it, less yield leaving 5 g.
  svOne(svValidate((f) => {
    f.fastMarginG.value = '10';
  }), 'fastMarginG',
  'Extend by up to (g) must not exceed 9.0 g (25 % of Target yield, Recipe).');
  svAssert.deepEqual(svValidate((f) => {
    f.fastExtractionGuardEnabled.checked = false;
    f.fastMarginG.value = '50';
  }).errors, []);
  svOne(svValidate((f) => {
    f.fastExtractionGuardEnabled.checked = false;
    f.goalWeightG.value = '12';
    f.slowMarginG.value = '9';
  }), 'slowMarginG', 'Shorten by up to (g) must not exceed 7.0 g (Target yield − 5 g, Recipe).');

  // (f) The fixed scale-loss time is optional; when set it stays between the
  // earliest weight stop and the max shot time.
  svOne(svValidate((f) => {
    f.scaleLossFixedS.value = '5';
  }), 'scaleLossFixedS', 'Backup stop time (s) must be from 12 to 45 s.');

  // (h) The retare floor rule shows its computation when auto-retare is on and
  // states the bare bound when it is off.
  svOne(svValidate(null, (f) => {
    f.bbwProtectionS.value = '5';
    f.fastBeforeS.value = '7';
    f.autoRetare.checked = true;
  }), 'bbwProtectionS',
  'Earliest weight stop (s) must be at least 8.0 s (Late cup time 5.0 s + 3 s, Tare).');
  svOne(svValidate(null, (f) => {
    f.bbwProtectionS.value = '2';
  }), 'bbwProtectionS', 'Earliest weight stop (s) must be at least 3.0 s.');

  // (g) Rule-table completeness: three two-sided rules plus the single-sided
  // retare floor, every referenced field a real Settings input with the steps
  // the phrasing depends on, and every section a real section summary.
  svAssert.equal(svA.rules.length, 4);
  svAssert.ok(svA.rules.some((r) => r[1] === 'retareFloor'));
  for (const [id, [, step]] of Object.entries(svFieldSpec)) {
    if (!step || id === 'weightOffsetBaselineG' || id === 'bbwAlphaBaseline') continue;
    const input = settingsHtml.match(new RegExp('id="' + id + '"[^>]*'));
    svAssert.ok(input, 'Settings input missing: ' + id);
    svAssert.ok(input[0].includes('step="' + step + '"'),
      id + ' must keep step ' + step + ' for message precision');
  }
  for (const section of ['Recipe', 'Fast shots', 'Slow shots', 'Time limits', 'Tare'])
    svAssert.ok(settingsHtml.includes('>' + section + '</summary>'),
      'Settings section summary missing: ' + section);
  svAssert.ok(codeIncludes(runtimeJs, 'const errs = settingsForm.validateBrewClient(); if (errs.length) {') &&
      codeIncludes(runtimeJs, 'clearBrewWarnings(); showFieldErrors(errs); return false; }'),
    'saveBrewPreset must render the full violation list, replacing the yellow preview');
  svAssert.ok(codeIncludes(viewJs.settings,
      'formBaseline[el.id] !== (el.type === "checkbox" ? el.checked : el.value)'),
    'Attribution must diff the live form against the device baseline');
  svAssert.equal(
    (js.match(/formBaseline = (?:R\.)?snapshotControls\(/g) || []).length, 1,
    'The form baseline must be captured where the form is hydrated');

  // Multi-error UI: inline marking per field with focus on the first offender
  // inside Settings, and the joined banner fallback when Settings is not the
  // active view (the Home quick-weight sheet path).
  {
    const svUiSrc = runtimeJs.slice(runtimeJs.indexOf('function markFieldError('),
      runtimeJs.indexOf('function clearMessage('));
    const svUiRun = (view) => {
      const state = {classes: [], focused: 0, opened: [], banner: []};
      const details = {open: false, parentElement: {closest: () => null}};
      const field = {
        classList: {add: (c) => state.classes.push(c)},
        closest: (sel) => (sel === 'details' ? details : null),
        parentElement: {appendChild: () => {}},
        focus() { state.focused++; },
        scrollIntoView() {},
        attrs: {},
        setAttribute(k, v) { this.attrs[k] = v; },
        getAttribute() { return null; },
        removeAttribute() {},
      };
      const ui = new Function('$', 'document', 'message', 'clearFieldErrors',
        'activeView', svUiSrc + ';return {showFieldError, showFieldErrors};')(
        () => field, {createElement: () => ({className: '', textContent: ''})},
        (text, kind) => state.banner.push([text, kind]), () => {}, view);
      ui.showFieldErrors([
        {id: 'goalWeightG', msg: 'one.'},
        {id: 'minRecoveryWeightG', msg: 'two.'},
      ]);
      return {state, details, field};
    };
    const svAway = svUiRun('home');
    svAssert.deepEqual(svAway.state.banner, [['one. two.', 'error']]);
    svAssert.equal(svAway.state.classes.length, 0,
      'Away from Settings the fields must not be marked inline');
    const svIn = svUiRun('settings');
    svAssert.deepEqual(svIn.state.classes, ['invalid', 'invalid']);
    svAssert.equal(svIn.details.open, true, 'The section must unfold to reveal the fields');
    svAssert.equal(svIn.state.focused, 1, 'Only the first offender takes focus');
    svAssert.deepEqual(svIn.state.banner, [['one. two.', 'error']]);
    svAssert.equal(svIn.field.attrs['aria-invalid'], 'true',
      'Red errors must mark their input for assistive tech');
    svAssert.ok(/^fieldErr\d/.test(svIn.field.attrs['aria-describedby']),
      'Red errors must point aria-describedby at their message');
  }

  // Live warnings (yellow preview of the save rejection): the executable
  // cases prove the warning set is exactly the validator's output minus the
  // empty-field "is required" results, resolves whole-set, and respects
  // guard gating; the engine harness proves signature-skip rendering and the
  // conflict chip.
  {
    // (w1) Shrinking Target below four times the extra yield shows the same
    // single verdict the save would produce, on the margin it invalidated;
    // the empty-field result is suppressed.
    const w1 = svValidate((f) => {
      f.goalWeightG.value = '10';
    });
    svAssert.equal(w1.warnings.length, 1, JSON.stringify(w1.warnings));
    svAssert.equal(w1.warnings[0].msg,
      'Extend by up to (g) must not exceed 2.5 g (25 % of Target yield, Recipe).');
    const w1Empty = svValidate((f) => {
      f.goalWeightG.value = '';
    });
    svAssert.equal(w1Empty.errors.length, 1);
    svAssert.deepEqual(w1Empty.warnings, [],
      'The empty-field "is required" result must not warn live');

    // (w2) Resolving the other side clears the whole set without saving.
    const w2 = svValidate((f) => {
      f.goalWeightG.value = '10';
      f.fastMarginG.value = '2.5';
    });
    svAssert.deepEqual(w2.warnings, [], JSON.stringify(w2.warnings));

    // (w3) A disabled guard keeps producing no warnings at all.
    const w3 = svValidate((f) => {
      f.fastExtractionGuardEnabled.checked = false;
      f.fastMarginG.value = '50';
    });
    svAssert.deepEqual(w3.warnings, []);

    // (w4)/(w5) Engine harness: count DOM writes across two unchanged runs
    // (signature skip) and check the chip text/count and hidden-at-zero.
    const strings = JSON.parse(
      fs.readFileSync(path.join(sketchDir, 'web/locales/en.json'), 'utf8')).strings;
    const engineSrc = svEngineSrc;
    const mkField = (id) => {
      const el = {id, attrs: {},
        classList: {add: () => {}, remove: () => {}},
        setAttribute(k, v) { el.attrs[k] = v; },
        getAttribute(k) { return k in el.attrs ? el.attrs[k] : null; },
        removeAttribute(k) { delete el.attrs[k]; },
        closest: () => ({appendChild: () => harness.counts.appended++,
                         querySelectorAll: () => []})};
      return el;
    };
    const harness = {
      counts: {created: 0, appended: 0, chipTexts: []},
      chip: {textContent: '', hidden: true,
        classList: {add: () => {}, remove: () => {},
          toggle: (_c, on) => { harness.chip.hidden = on; }}},
      fields: {
        goalWeightG: null, minRecoveryWeightG: null, brewConflictChip: null,
      },
    };
    harness.fields.goalWeightG = mkField('goalWeightG');
    harness.fields.minRecoveryWeightG = mkField('minRecoveryWeightG');
    harness.fields.brewConflictChip = harness.chip;
    const script = [
      [{id: 'goalWeightG', msg: 'one.'}],
      [{id: 'goalWeightG', msg: 'one.'}],
      // Same field twice: the validator does emit two results for one input
      // (self-range + cross rule), and both smalls must stay aria-linked.
      [{id: 'goalWeightG', msg: 'one.'}, {id: 'goalWeightG', msg: 'two.'}],
      [],
    ];
    let call = 0;
    const engine = new Function('$', 'document', 'validateBrewClient',
      '__WEBUI_TEXT__', 'settingsSectionEls',
      engineSrc + ';return {refreshBrewWarnings, clearBrewWarnings};')(
      (id) => harness.fields[id],
      {querySelectorAll: () => [],
       createElement: () => { harness.counts.created++; return {}; }},
      () => script[Math.min(call, script.length - 1)],
      (k) => strings[k],
      () => []);
    engine.refreshBrewWarnings();
    const createdAfterFirst = harness.counts.created;
    call = 1;
    engine.refreshBrewWarnings();
    svAssert.equal(harness.counts.created, createdAfterFirst,
      'An unchanged warning signature must not write the DOM');
    call = 2;
    // The production clear (clearBrewWarningsDom) resets the pairing via
    // refreshFieldAria before re-rendering; the flat DOM stub cannot model
    // class queries, so reset the recorded attrs the same way here.
    harness.fields.goalWeightG.attrs = {};
    engine.refreshBrewWarnings();
    svAssert.equal(harness.counts.created, createdAfterFirst + 2);
    svAssert.equal(harness.chip.hidden, false);
    svAssert.equal(harness.chip.textContent, '2 conflicts to resolve before saving.');
    svAssert.match(harness.fields.goalWeightG.attrs['aria-describedby'],
      /^brewWarn\d+ brewWarn\d+$/,
      'Two warnings on one field must both stay aria-linked');
    call = 3;
    engine.refreshBrewWarnings();
    svAssert.equal(harness.chip.hidden, true, 'The chip must hide at zero conflicts');
    svAssert.ok(!engineSrc.match(/\bmessage\(/),
      'The live-warning path must stay inline-only (no banner)');
    svAssert.ok(strings['settings.conflict_will_block_saving'] &&
                strings['settings.conflicts_will_block_saving'],
      'Chip locale keys must exist');
  }

  // (w6) The aria rebuild branch: a red small that survives the brew-scoped
  // clear keeps its id in the rebuilt pairing (the flat w4 stub cannot
  // exercise refreshFieldAria's query, so drive it through the real
  // clearBrewFieldErrors with a host that reports one surviving red small).
  {
    const engineSrc = svEngineSrc;
    const survivor = {id: 'fieldErr7', remove: () => {}};
    const el = {id: 'goalWeightG', attrs: {},
      classList: {add: () => {}, remove: () => {}},
      setAttribute(k, v) { el.attrs[k] = v; },
      getAttribute(k) { return k in el.attrs ? el.attrs[k] : null; },
      removeAttribute(k) { delete el.attrs[k]; },
      closest: () => ({appendChild: () => {}, querySelectorAll: () => [survivor]})};
    el.attrs['aria-invalid'] = 'true';
    el.attrs['aria-describedby'] = 'fieldErr7 brewWarn9';
    const engine = new Function('$', 'document', 'validateBrewClient',
      '__WEBUI_TEXT__', 'settingsSectionEls',
      engineSrc + ';return {clearBrewFieldErrors};')(
      () => null, {querySelectorAll: () => [], createElement: () => ({})},
      () => [], (k) => strings[k], () => [el]);
    engine.clearBrewFieldErrors();
    svAssert.equal(el.attrs['aria-describedby'], 'fieldErr7',
      'The rebuild must re-point at the surviving red small');
    svAssert.equal(el.attrs['aria-invalid'], 'true');
  }

  // Structural pins: wiring, styles, chip, hooks, and symmetric aria.
  {
    const settingsJsSrc = viewJs.settings || '';
    const engineSrc = svEngineSrc;
    svAssert.ok(codeIncludes(settingsJsSrc,
      'R.clearBrewFieldErrors();R.refreshBrewWarnings();'),
      'The shared settings listener must clear stale red marks and refresh warnings');
    svAssert.ok(codeIncludes(blockAt(viewJs.settings, 'function applySettingsStatus(s) {'),
      'refreshBrewWarnings();'),
      'applySettingsStatus must refresh warnings after re-capturing baselines');
    svAssert.ok(codeIncludes(blockAt(runtimeJs, 'function saveBrewPreset('),
      'clearBrewWarnings();'),
      'A successful brew save must clear the warnings immediately');
    svAssert.ok(engineSrc.includes('aria-invalid') &&
                engineSrc.includes('aria-describedby'),
      'Yellow warnings must mark their input for assistive tech');
    svAssert.ok(blockAt(viewJs.settings, 'function clearBrewFieldErrors(')
        .includes('small.fieldError'),
      'The scoped red clear must remove the brew smalls, not just the class');
    svAssert.ok(css.includes('.fieldWarning{') && css.includes('.fieldWarn{') &&
                rawCss.includes('.fieldWarning:before{content:"!"'),
      'Warning twins must exist with the shared alert glyph');
    svAssert.ok(settingsHtml.includes('id="brewConflictChip"'),
      'The brew save bar must carry the conflict chip');
  }
}
// Quick-override notes follow the status: clearing the live brew warnings
// must not wipe them until the next poll.
if (!codeIncludes(runtimeJs, "querySelectorAll('#workflowPanel .fieldWarning:not(.qsOverride)')")) {
  throw new Error('Clearing brew warnings must keep the quick-override notes');
}
