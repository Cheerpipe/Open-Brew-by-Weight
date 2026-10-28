{
  // Open Brew by Weight Companion on every build: Admin pairs and switches
  // through one Admin-unlocked POST, Home announces an open request or offers
  // to pair the companion a page came through, and Diagnostic → Companion
  // shows this screen and the link. The La Marzocco parts stay in Linea Micra
  // builds.
  const assert = require('assert').strict, vm = require('vm');
  const admin = partialHtml.admin, adminJs = viewJs.admin;
  assert.ok(admin.includes('<details id="companionPanel" data-g="companion" class="cfgGroup"><summary>Companion</summary>') &&
      admin.indexOf('id="accessPanel"') < admin.indexOf('id="companionPanel"') &&
      admin.indexOf('id="companionPanel"') < admin.indexOf('id="scheduledRestartPanel"'),
      'Admin → Companion follows Access in every build');
  // The glyph ships once in the shell sprite defs: the header indicator draws
  // it on every view and the Admin device list reuses it through <use>.
  assert.ok(rawShellHtml.includes('<symbol id="g-companion" viewBox') &&
      codeIncludes(adminJs, "companion: '<use href=\"#g-companion\"/>'") &&
      rawShellHtml.includes('id="companionSignal"') &&
      rawShellHtml.indexOf('id="companionSignal"') < rawShellHtml.indexOf('id="wifiSignal"') &&
      rawShellHtml.includes('id="companionDetails"'),
      'The shell pairs the header companion indicator with the one companion glyph');
  // The header is shared by every view, and the Home stream is the one every
  // view keeps receiving: the indicator's four fields ride it beside the
  // Wi-Fi and scale rows, while the full link report stays diagnostic-only.
  {
    const homeStreamSource = fs.readFileSync(
        path.join(sketchDir, 'network/OpenBrewByWeightHomeStream.inc'), 'utf8');
    const homeRegion = homeStreamSource.slice(
        homeStreamSource.indexOf('bool OpenBrewByWeightNetwork::sendHomeStream'),
        homeStreamSource.indexOf('bool OpenBrewByWeightNetwork::sendUiStreamFrame'));
    const companionPaths = [...homeRegion.matchAll(/delta\.(?:hashed)?[fF]ield\("(companion\.[^"]+)"/g)]
        .map((m) => m[1]).sort();
    assert.deepEqual(companionPaths,
        ['companion.link', 'companion.name', 'companion.paired', 'companion.peerProtocol',
         'companion.requestAddress', 'companion.requestName'],
        'The Home stream carries the header indicator fields, nothing more');
    assert(homeRegion.indexOf('companion.link') <
            homeRegion.indexOf('delta.field("cupPresence.state"'),
        'The companion rows sit with the connection rows the header reads');
  }
  for (const id of ['saveCompanionButton', 'revertCompanionButton'])
    assert.ok(new RegExp(`id="${id}"[^>]*data-dirty="0"[^>]*disabled`).test(admin),
        `${id} must start visibly disabled`);
  for (const needle of [
    "R.api('/api/v1/companion/admin',{method:'POST',body:R.body(body)})",
    "{action:'approve',code:$('companionCode').value,...(__LINEA_MICRA__&&{lm:$('companionApproveLm').checked}),}",
    "$('companionCode').value='';if(__LINEA_MICRA__)$('companionApproveLm').checked=true",
    "!link&&c.peerProtocol>0?' · '+",
    "{action:'ignore'}",
    "{action:'unignore'}",
    "{action:'forget'}",
    "{action:'switches',ui,...(__LINEA_MICRA__&&{lm:$('companionLm').checked})}",
    "if(e.code==='COMPANION_CODE')R.showFieldError('companionCode',",
    "window.open(url,'_blank','noopener')",
    "saveCompanionButton:'revertCompanionButton'",
    "constcompanionSwitches=()=>document.querySelectorAll('#companionPaired input')",
    "for(const el of companionSwitches())el.onchange=()=>sectionDirty('companion',true)",
    "$('revertCompanionButton').onclick=()=>revertSection('companion')",
    "if(!R.uiState.companionDirty){$('companionUi').checked=c.ui!==false;if(__LINEA_MICRA__)$('companionLm').checked=!!c.lm;R.uiState.companionBaseline=R.snapshotControls(companionSwitches())}",
    "if(__LINEA_MICRA__)$('companionRouteState').textContent=companionRouteText(c)",
    "applyCompanionStatus(s.companion)",
    "initCompanion()",
    "['blePanel','dateTimePanel','webhookPanel','companionPanel']",
    // Pairing ends at step 3 once the relay or the link is up.
    "p.stage=!c.paired?1:c.link!=='open'&&c.relay!=='ready'?2:3",
  ])
    assert.ok(codeIncludes(adminJs, needle), 'Admin → Companion wiring: ' + needle);
  assert.ok(!/__LINEA_MICRA__\)\s*(applyCompanionStatus|initCompanion)/.test(adminJs) &&
      !adminJs.includes('...(__LINEA_MICRA__ && { companion'),
      'Admin → Companion runs in every build');
  assert.ok(/<form id="companionRequest"[^]*<label class="micraOnly"><input id="companionApproveLm" type="checkbox" role="switch" checked[^]*id="companionApproveButton"/.test(admin),
      'The approval panel chooses the La Marzocco switch before the account is handed over, on by default, in Micra builds');
  assert.ok(/<div id="companionPaired"[^]*<div class="accessRow micraOnly"><span><b>La Marzocco[^]*<input id="companionUi" type="checkbox" role="switch" checked[^]*<label class="micraOnly"><input id="companionLm"/.test(admin),
      'Paired: the Web UI switch in every build, the La Marzocco route and switch in Micra builds');
  assert.ok(admin.includes('Connect to a companion at…') &&
      codeIncludes(adminJs, 'That code does not match.'),
      'A wrong code says so at the code field');
  // Turning the Web UI off from a companion screen warns and names the
  // controller's direct address.
  const save = blockAt(adminJs, 'async function saveCompanionSettings(');
  assert.ok(codeIncludes(save, "if(!ui&&companionState?.ui!==false&&R.uiState.companionHere&&!confirm(R.sub(") &&
      codeIncludes(save, "['http://'+companionDirect]))") &&
      codeIncludes(blockAt(adminJs, 'function applyAdminStatus('),
          "companionDirect=s.network.staIp||s.network.mdnsHost+'.local'"),
      'Turning the Web UI off from a companion screen warns with the direct address');
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
    assert.equal(route({route: 'direct', lm: true, link: 'open', credentials: true, upstream: true,
      refused: true}),
    'Directly from the controller · the companion is connecting to La Marzocco');
    assert.equal(route({route: 'direct', lm: true, link: 'open', credentials: true, upstream: true,
      refused: false}),
    'Directly from the controller · it moves over when the machine is idle');
  }

  // Entry point (c): an https: page asks its companion's same-origin status;
  // an unpaired one may be asked to pair (JSON POST), then the code it shows
  // is typed in Admin, never filled in.
  {
    const calls = [];
    let status = {name: 'nas', paired: false, pairing: {state: 'unpaired'}};
    const run = vm.createContext({location: {protocol: 'https:'}, uiState: {companionHere: null},
      sleep: async () => {},
      fetchJson: async (path, options) => {
        calls.push([path, options?.method || 'GET', options?.headers?.['Content-Type']]);
        if (path === '/__companion/pair') {
          status = {...status, pairing: {state: 'requesting'}};
          return [{ok: true}, {state: 'unpaired'}];
        }
        const answer = [{ok: true}, status];
        if (status.pairing.state === 'requesting') status = {...status, pairing: {state: 'pending', code: null}};
        else if (status.pairing.state === 'pending') status = {...status, pairing: {state: 'pending', code: '123456'}};
        return answer;
      },
      __WEBUI_TEXT__: (key) => key});
    vm.runInContext(rawRuntimeJs.slice(rawRuntimeJs.indexOf('let companionHereLoad = null;'),
        rawRuntimeJs.indexOf('// `claimed` is a sign-in response')), run);
    (async () => {
      assert.equal((await run.companionHere()).name, 'nas');
      assert.equal(run.companionPairable(), true);
      await run.companionHere();
      assert.equal(calls.length, 1, 'the status is asked once per page unless fresh');
      await run.pairCompanionHere();
      assert.deepEqual(calls[1], ['/__companion/pair', 'POST', 'application/json']);
      assert.deepEqual(run.uiState.companionHere.pairing, {state: 'pending', code: '123456'},
          'pairing waits until the code is ready');
      assert.equal(run.companionPairable(), false);
      for (const state of ['already_paired', 'ignored', 'requesting']) {
        run.uiState.companionHere = {paired: false, pairing: {state}};
        assert.equal(run.companionPairable(), false, state);
      }
      run.uiState.companionHere = {paired: true, pairing: {state: 'paired'}};
      assert.equal(run.companionPairable(), false);
      run.location.protocol = 'http:';
      assert.equal(await run.companionHere(true), null, 'a direct page never asks');
    })().catch((error) => {console.error(error); process.exitCode = 1;});
  }
  assert.ok(admin.includes('<div id="companionHere" class="accessRow" hidden>') &&
      admin.indexOf('id="companionHere"') < admin.indexOf('id="companionAddress"') &&
      codeIncludes(adminJs, "$('companionHere').hidden=!R.companionPairable()") &&
      codeIncludes(rawViewJs.admin, "code?R.sub(__WEBUI_TEXT__('admin.companion_code_here'),[code])") &&
      !codeIncludes(adminJs, "$('companionCode').value=code") &&
      codeIncludes(adminJs, 'await R.pairCompanionHere().catch('),
      'Admin → Companion offers to pair the companion this page came through and shows its code');

  // Home: one card at a time, keyed to nothing, so a new request always shows.
  assert.ok(partialHtml.home.includes('<section id="companionHint" class="homeCard infoHint" hidden>') &&
      partialHtml.home.includes('href="/admin#companionPanel"') &&
      !partialHtml.home.includes('id="companionHintDismiss"') &&
      partialHtml.home.includes('<section id="companionPairHint" class="homeCard infoHint" hidden>') &&
      partialHtml.home.includes('id="companionPairHintDismiss"') &&
      partialHtml.home.indexOf('id="companionHint"') < partialHtml.home.indexOf('id="companionPairHint"') &&
      partialHtml.home.indexOf('id="companionPairHint"') < partialHtml.home.indexOf('id="accessHint"') &&
      codeIncludes(viewJs.home, 'const request=s.companion?.requestName,') &&
      codeIncludes(viewJs.home, 's.companion.requestAddress') &&
      codeIncludes(viewJs.home, "['companionHint',!!request]") &&
      codeIncludes(viewJs.home, "['companionPairHint',!request&&R.companionPairable()]") &&
      codeIncludes(viewJs.home, "for(const id of['companionHint','companionPairHint','accessHint',"),
      'Home announces a companion request until it is answered or expires, and offers to pair');

  // Diagnostic → Companion: this screen and the controller's link in every
  // build, the La Marzocco rows in Micra builds.
  const diag = partialHtml.diagnostic, diagJs = viewJs.diagnostic;
  assert.ok(diag.includes('<details class="cfgGroup" data-g="companion"><summary>Companion</summary><div class="statusColumn"><b class="blockHead">This screen</b>') &&
      diag.includes('<div class="statusColumn"><b class="blockHead">Companion</b>') &&
      ['dUiMode', 'dUiAccess', 'dUiOffline', 'dCoName', 'dCoAddress', 'dCoVersion', 'dCoLink',
        'dCoLinkCounts', 'dCoUi', 'dCoRoute', 'dCoLm', 'dCoCounts'].every((id) => diag.includes(`id="${id}"`)) &&
      ['dCoRoute', 'dCoLm', 'dCoCounts'].every((id) =>
        new RegExp(`<div class="metric micraOnly"><strong>[^<]*</strong><div id="${id}">`).test(diag)) &&
      codeIncludes(diagJs, 'R.companionHere(true).then(renderScreen)') &&
      codeIncludes(diagJs, 'screens=Array.isArray(c?.screens)?c.screens.length:c?.screens') &&
      codeIncludes(rawViewJs.diagnostic, "screens>0?' · '+R.sub(__WEBUI_TEXT__('diagnostic.screen_count'),[screens]):''") &&
      codeIncludes(diagJs, 'if(s.companion)renderCompanionDiagnostic(s.companion)') &&
      codeIncludes(diagJs, "c.paired?c.name:'Not paired'") &&
      codeIncludes(diagJs, "c.linkAgeS>=0?' · '+formatUptime(c.linkAgeS*1e3):''") &&
      codeIncludes(diagJs, "on(c.ui!==false)+(c.relay?' · '+c.relay:'')") &&
      codeIncludes(diagJs, 'if(!__LINEA_MICRA__)return;') &&
      codeIncludes(diagJs, "c.protocol>0?") && codeIncludes(diagJs, "c.peerProtocol>0?"),
      'Diagnostic → Companion shows this screen and the controller\'s companion link');
  // On the companion route the WebSocket row names its source and drops the
  // direct socket's figures; cleaning is "not live" only without supervision.
  assert.ok(codeIncludes(diagJs, "renderMicraCloudDiagnostic(s.lineaMicra,s.companion?.route==='companion')") &&
      codeIncludes(diagJs, "link=api?0:ws.live?1:") &&
      codeIncludes(diagJs, "if(el)el.hidden=!!via") &&
      codeIncludes(diagJs, "(!ws.live||(ws.machineConnectedKnown&&!ws.machineConnected)"),
      'The WebSocket row tells the companion feed from the controller\'s own socket');

  // Other builds keep the companion and drop only the La Marzocco parts.
  (async () => {
    const noMicra = await webUi.generate({webUiLanguage: 'EN', lineaMicra: false, write: false});
    const out = [noMicra.html, noMicra.js, noMicra.runtimeJs, ...Object.values(noMicra.partials),
      ...Object.values(noMicra.modules)].join('\n');
    const leaked = ['id="companionLm"', 'id="companionApproveLm"', 'id="companionRouteState"', 'id="dCoRoute"',
      'id="dCoLm"', 'id="dCoCounts"', 'id="micraSignal"', 'id="micraDetails"', 'dg-marzocco-cloud'].filter((s) => out.includes(s));
    assert.deepEqual(leaked, [], 'The La Marzocco companion parts stay out of builds without Linea Micra');
    const missing = ['id="companionPanel"', 'id="companionHint"', 'id="companionPairHint"', 'id="companionUi"',
      'id="g-companion"', 'id="dCoName"', 'id="dCoUi"', 'id="dUiAccess"', '/api/v1/companion/admin',
      '/__companion/pair', '/__companion/status', 'requestName'].filter((s) => !out.includes(s));
    assert.deepEqual(missing, [], 'Every build pairs a companion and uses the Web UI through it');
  })().catch((error) => {console.error(error); process.exitCode = 1;});
}
