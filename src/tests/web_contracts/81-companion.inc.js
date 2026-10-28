{
  // Open Brew by Weight Companion, phase 1 (Linea Micra builds): Admin pairs
  // and switches through one Admin-unlocked POST, Home announces an open
  // request, and Diagnostic → Companion shows this screen and the link.
  const assert = require('assert').strict, vm = require('vm');
  const admin = partialHtml.admin, adminJs = viewJs.admin;
  assert.ok(admin.includes('<details id="companionPanel" data-g="companion" class="cfgGroup micraOnly"><summary>Companion</summary>') &&
      admin.indexOf('id="accessPanel"') < admin.indexOf('id="companionPanel"') &&
      admin.indexOf('id="companionPanel"') < admin.indexOf('id="scheduledRestartPanel"') &&
      admin.includes('<symbol id="g-companion" class="micraOnly"'),
      'Admin → Companion follows Access and ships only in Linea Micra builds');
  for (const id of ['saveCompanionButton', 'revertCompanionButton'])
    assert.ok(new RegExp(`id="${id}"[^>]*data-dirty="0"[^>]*disabled`).test(admin),
        `${id} must start visibly disabled`);
  for (const needle of [
    "R.api('/api/v1/companion/admin',{method:'POST',body:R.body(body)})",
    "{action:'approve',code:$('companionCode').value}",
    "{action:'ignore'}",
    "{action:'unignore'}",
    "{action:'forget'}",
    "{action:'switches',lm:$('companionLm').checked}",
    "if(e.code==='COMPANION_CODE')R.showFieldError('companionCode',",
    "window.open(url,'_blank','noopener')",
    "saveCompanionButton:'revertCompanionButton'",
    "$('companionLm').onchange=()=>sectionDirty('companion',true)",
    "$('revertCompanionButton').onclick=()=>revertSection('companion')",
    "if(!R.uiState.companionDirty){$('companionLm').checked=!!c.lm;R.uiState.companionBaseline=R.snapshotControls([$('companionLm')])}",
    "if(__LINEA_MICRA__)applyCompanionStatus(s.companion)",
    "if(__LINEA_MICRA__)initCompanion()",
    "['blePanel','dateTimePanel','webhookPanel','companionPanel']",
  ])
    assert.ok(codeIncludes(adminJs, needle), 'Admin → Companion wiring: ' + needle);
  assert.ok(admin.includes('Connect to a companion at…') &&
      codeIncludes(adminJs, 'That code does not match.'),
      'A wrong code says so at the code field');
  {
    const run = vm.createContext({URL});
    vm.runInContext(blockAt(adminJs, 'function companionPairUrl(') +
        blockAt(adminJs, 'function companionRouteText('), run);
    assert.equal(run.companionPairUrl('192.168.1.20'), 'http://192.168.1.20/__companion/pair');
    assert.equal(run.companionPairUrl('https://home.local:8443/x'), 'https://home.local:8443/__companion/pair');
    assert.equal(run.companionPairUrl('user@host'), '');
    const route = (c) => run.companionRouteText(c);
    assert.equal(route({route: 'companion', lm: true}), 'Through the companion');
    assert.equal(route({route: 'pending', lm: true}), 'Moving to the companion');
    assert.equal(route({route: 'direct', lm: false}), 'Directly from the controller');
    assert.equal(route({route: 'direct', lm: true, link: 'down'}),
        "Directly from the controller · the companion isn't connected");
    assert.equal(route({route: 'direct', lm: true, link: 'open', credentials: true, upstream: false}),
        'Directly from the controller · the companion is connecting to La Marzocco');
  }

  // Home: one card at a time, keyed to nothing, so a new request always shows.
  assert.ok(partialHtml.home.includes('<section id="companionHint" class="homeCard infoHint micraOnly" hidden>') &&
      partialHtml.home.includes('href="/admin#companionPanel"') &&
      !partialHtml.home.includes('id="companionHintDismiss"') &&
      partialHtml.home.indexOf('id="companionHint"') < partialHtml.home.indexOf('id="accessHint"') &&
      codeIncludes(viewJs.home, "const request=__LINEA_MICRA__&&s.machineIntegration==='linea_micra_cloud'&&s.companion?.requestName") &&
      codeIncludes(viewJs.home, 's.companion.requestAddress') &&
      codeIncludes(viewJs.home, "['companionHint',!!request]") &&
      codeIncludes(viewJs.home, "for(const id of['companionHint','accessHint',"),
      'Home announces a companion request until it is answered or expires');

  // Diagnostic → Companion: this screen in every build, the link in Micra builds.
  const diag = partialHtml.diagnostic, diagJs = viewJs.diagnostic;
  assert.ok(diag.includes('<details class="cfgGroup" data-g="companion"><summary>Companion</summary><div class="statusColumn"><b class="blockHead">This screen</b>') &&
      diag.includes('<div class="statusColumn micraOnly"><b class="blockHead">Companion</b>') &&
      ['dUiMode', 'dUiAccess', 'dUiOffline', 'dCoName', 'dCoAddress', 'dCoVersion', 'dCoLink',
        'dCoLinkCounts', 'dCoRoute', 'dCoLm', 'dCoCounts'].every((id) => diag.includes(`id="${id}"`)) &&
      codeIncludes(diagJs, "if(location.protocol==='https:')fetch('/__companion/status',{cache:'no-store'})") &&
      codeIncludes(diagJs, "screenCompanion=typeof j?.name==='string'?j.name:''") &&
      codeIncludes(diagJs, 'if(__LINEA_MICRA__&&s.companion)renderCompanionDiagnostic(s.companion)') &&
      codeIncludes(diagJs, "c.paired?c.name:'Not paired'"),
      'Diagnostic → Companion shows this screen and the controller\'s companion link');

  // Other builds keep this screen and drop everything else.
  (async () => {
    const noMicra = await webUi.generate({webUiLanguage: 'EN', lineaMicra: false, write: false});
    const out = [noMicra.html, ...Object.values(noMicra.partials), ...Object.values(noMicra.modules)].join('\n');
    const leaked = ['id="companionPanel"', 'id="companionHint"', 'id="g-companion"', 'id="dCoName"',
      '/api/v1/companion/admin', '__companion/pair', 'requestName'].filter((s) => out.includes(s));
    assert.deepEqual(leaked, [], 'Companion pairing must stay out of builds without Linea Micra');
    assert.ok(out.includes('id="dUiAccess"') && out.includes('/__companion/status'),
        'Every build tells whether this screen came through the companion');
  })().catch((error) => {console.error(error); process.exitCode = 1;});
}
