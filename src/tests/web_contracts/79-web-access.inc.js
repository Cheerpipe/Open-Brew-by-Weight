{
  // Optional Web UI login: off by default, effective only with a non-factory
  // device password, sessions separate from the WebUI claim, and the
  // Integration API gated only while the login is on.
  const assert = require('assert').strict, vm = require('vm');
  const access = fs.readFileSync(
      path.join(sketchDir, 'network', 'OpenBrewByWeightWebAccessHttp.inc'), 'utf8');
  const core = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightWebAccess.h'), 'utf8');
  const body = (source, marker) => {
    const start = source.indexOf(marker);
    assert.ok(start >= 0, `missing ${marker}`);
    return source.slice(start, source.indexOf('\n}\n', start));
  };

  assert.ok(body(access, 'bool OpenBrewByWeightNetwork::webAccessActive()')
      .includes('g_webAccess.stored.enabled != 0 && !passwordIsFactoryDefault(settings_)'),
      'The login must need a stored opt-in and a non-factory device password');
  const sessionGate = body(access, 'bool OpenBrewByWeightNetwork::webAccessSessionAllowed(');
  assert.ok(sessionGate.includes('if (!webAccessActive() || softApPeer(request)) return true;') &&
      body(access, 'bool OpenBrewByWeightNetwork::softApPeer(')
          .includes('socketServedBySoftAp(httpd_req_to_sockfd(request))'),
      'Login off and SoftAP setup peers need no session');
  assert.ok(body(network, 'bool OpenBrewByWeightNetwork::requireActiveWebUiClient(')
      .indexOf('webAccessSessionAllowed(request)') <
      body(network, 'bool OpenBrewByWeightNetwork::requireActiveWebUiClient(').indexOf('webUiClaim(caller.relayed)'),
      'Every claimed API route checks the session before the claim');
  assert.ok(body(network, 'esp_err_t OpenBrewByWeightNetwork::claimHandler(')
      .includes('webAccessSessionAllowed(request)'), 'Claiming control needs a session');
  for (const route of ['{"/api/v1/auth", HTTP_GET, authStatusHandler, false, true}',
                       '{"/api/v1/auth/login", HTTP_POST, authLoginHandler, false, true}',
                       '{"/api/v1/auth/logout", HTTP_POST, authLogoutHandler, false, true}',
                       '{"/api/v1/auth/access", HTTP_POST, authAccessHandler}',
                       '{"/api/v1/auth/pairing", HTTP_POST, authPairingHandler}']) {
    assert.ok(network.includes(route), `Missing access route ${route}`);
  }
  assert.ok(network.includes('if (matched->open) return matched->handler(request);'));
  // The open-access note is one more Home dismissal. Restore brings back every
  // note and ignored advice, alone in its body; acknowledged images stay.
  const dismiss = body(access, 'esp_err_t OpenBrewByWeightNetwork::cardDismissHandler(');
  const restore = dismiss.slice(dismiss.indexOf('if (restore) {'), dismiss.indexOf('} else if', dismiss.indexOf('if (restore) {')));
  assert.ok(!network.includes('/api/v1/auth/notice') &&
      dismiss.includes('jsonBoolean(root, "restore", restore) && restore && cJSON_GetArraySize(root) == 1') &&
      restore.includes('g_cardDismissals.notes = 0;') &&
      restore.includes('memset(g_cardDismissals.grind, 0, sizeof(g_cardDismissals.grind));') &&
      restore.includes('g_webAccess.stored.noticeDismissed = 0;') && !restore.includes('image') &&
      dismiss.includes('} else if (strcmp(card, "access") == 0) {') &&
      network.includes('",\\"access\\":%s}"'), 'Home dismissals cover the access note and restore');
  // NET-02: every registered handler passes the DNS-rebinding Host gate
  // first; captive-probe hosts pass only while the SoftAP is up.
  const timed = body(network, 'esp_err_t timedHandler(');
  assert.ok(network.includes('descriptor.handler = timedHandler;') &&
      timed.includes('rejectForeignHost(request)') &&
      timed.indexOf('rejectForeignHost(request)') < timed.indexOf('request->user_ctx'));
  const hostGate = body(network, 'bool OpenBrewByWeightNetwork::rejectForeignHost(');
  assert.ok(hostGate.includes('webAccessHostAllowed(host, network.mdnsHost)') &&
      hostGate.includes('(network.apActive && captiveProbeHost(request) != nullptr)') &&
      hostGate.includes('logRefusedHost(host, network.mdnsHost)') &&
      hostGate.includes('"HOST_NOT_ALLOWED"'), 'Foreign Hosts are refused and logged');
  // NET-04: diagnostic mutations need Admin or the public Diagnostic page.
  const owned = body(network, 'esp_err_t OpenBrewByWeightNetwork::ownedApiHandler(');
  for (const handler of ['heapMapDownloadHandler', 'auditVerboseHandler', 'clearResetHistoryHandler',
                         'taskProfilerHandler', 'scaleProfileHandler', 'loopMaxResetHandler',
                         'scaleGapMaxResetHandler', 'scaleTareHandler']) {
    assert.match(owned, new RegExp(`${handler},\\s*false, false,\\s*true\\}`), handler);
  }
  assert.ok(codeIncludes(owned, 'if(matched->diagnostic&&!self.diagnosticPageEnabled()&&' +
      '!self.requireAdminUnlock(request)){'), 'Diagnostic routes check Admin or the public page');
  assert.ok(body(access, 'esp_err_t OpenBrewByWeightNetwork::authAccessHandler(')
      .includes('if (!self.requireAdminUnlock(request)) return ESP_OK;'),
      'Turning the login on or off stays inside an unlocked Admin');
  assert.ok(body(access, 'esp_err_t OpenBrewByWeightNetwork::authAccessHandler(')
      .includes('"FACTORY_PASSWORD"'), 'The factory password can never back a login');
  // One failure counter and cooldown for every device-password check.
  for (const handler of ['authLoginHandler', 'authAccessHandler', 'integrationAccessRoute']) {
    assert.ok(body(access, `OpenBrewByWeightNetwork::${handler}(`).includes('checkDevicePassword('));
  }
  assert.ok(body(network, 'esp_err_t OpenBrewByWeightNetwork::adminUnlockHandler(')
      .includes('self.checkDevicePassword(request, password)'));
  // OTA keeps its device-password and Admin-unlock paths; the password header
  // shares the sign-in cooldown and a browser also needs its sign-in.
  const otaAuth = body(network, 'bool OpenBrewByWeightNetwork::authorizeOtaRequest(');
  assert.ok(otaAuth.includes('checkDevicePassword(request, password) == PasswordCheck::OK') &&
      otaAuth.includes('adminUnlockAllowed(request) && webAccessSessionAllowed(request)'));

  // Factory-password restores turn the login off; a password change keeps
  // the session that asked for it and Home Assistant's token.
  assert.ok(network.includes('if (passwordIsFactoryDefault(settings_)) webAccessDisable(g_webAccess);'));
  assert.ok(network.includes('webAccessSignOut(keepValid ? keep : nullptr, false);'));
  assert.ok(network.includes("self.keepPasswordChangeSession(request, command.requestId);"));
  assert.ok(/RESET_NETWORK_AP\) \{\s*webAccessSignOut\(nullptr, true\);/.test(network));
  assert.ok(network.includes('g_webAccess = WebAccessState{};  // the durable reset erased its record'));
  const service = body(access, 'void OpenBrewByWeightNetwork::serviceWebAccess()');
  assert.ok(network.includes('serviceWebAccess();') && service.includes('networkFlashWriteAllowed()'),
      'The network task persists access state only through the shared flash-write gate');
  assert.ok(service.includes('webAccessRetryAtMs_ != 0 &&') && service.includes('webAccessRetryAtMs_ = 0;'),
      'A save retry must not stall saving once millis() passes 2^31');
  assert.ok(network.includes('passwordKeepRequestId_ == command.requestId'),
      'Only the request that captured a session keeps it');
  assert.ok(fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightDurableStores.h'), 'utf8')
      .includes('!eraseWebAccessStored()'), 'Every factory reset erases the access record');

  // Tokens stay out of logs and URLs.
  assert.ok(!access.includes('lifecycleLogf('), 'Access logs never format values');
  assert.ok(!/session=|token=/.test(rawRuntimeJs), 'Session tokens never travel in a URL');
  assert.ok(core.includes('static_assert(sizeof(WebAccessStored) == 388'));

  // Integration API: info, exchange and pairing answer first; every other
  // route needs the Home Assistant token only while the login is on.
  const integration = body(access, 'bool OpenBrewByWeightNetwork::integrationAccessRoute(');
  for (const route of ['/api/v1/integration/info', '/api/v1/integration/auth',
                       '/api/v1/integration/pair']) {
    assert.ok(integration.includes(route), `Missing integration access route ${route}`);
  }
  assert.ok(integration.indexOf('if (!active) return false;') <
      integration.indexOf('readWebAccessToken(request, "Authorization", "Bearer ", token)'));
  // The Web UI's own Quick Settings calls use its session, not a bearer token.
  assert.ok(integration.includes('if (webAccessSessionAllowed(request)) return false;'));
  assert.ok(codeIncludes(rawRuntimeJs, 'command("/api/v1/integration/quick-settings"'));

  // The Home stream drives the notice and the pairing prompt.
  for (const field of ['access.login', 'access.notice', 'access.pairId', 'access.pairCode',
                       'access.pairRemainingS']) {
    assert.ok(network.includes(`delta.field("${field}"`), `Home stream misses ${field}`);
  }
  for (const id of ['id="webUiLogin"', 'id="loginRemember"', 'autocomplete="username"',
                    'id="pairDialog"', 'id="accessHint"', 'id="accessPanel"',
                    'id="loginEnabled"', 'id="loginSheet"']) {
    assert.ok(codeIncludes(ui, id), `Missing ${id}`);
  }
  assert.ok(codeIncludes(rawRuntimeJs, 'if (data.error === "AUTH_REQUIRED") showSignIn(true);'));
  // One device password: the confirmation sheet hides the change form, and
  // flipping the switch back cancels it.
  assert.ok(blockAt(viewJs.admin, 'function openLoginSheet(').includes('$("devicePasswordForm").classList.add("hidden")') &&
      blockAt(viewJs.admin, 'function closeLoginSheet(').includes('$("devicePasswordForm").classList.remove("hidden")') &&
      codeIncludes(viewJs.admin, '$("loginEnabled").checked === !!accessState.loginEnabled'));
  // Admin → Access lists every signed-in device by id (never by token), the
  // browser sends a label at sign-in, and one device signs out at a time.
  const accessStatus = body(access, 'bool OpenBrewByWeightNetwork::appendWebAccessStatus(');
  assert.ok(accessStatus.includes('\\"sessions\\":[') && accessStatus.includes('webAccessSessionId(g_webAccess, own)') &&
      !accessStatus.includes('formatWebAccessToken'));
  assert.ok(body(access, 'esp_err_t OpenBrewByWeightNetwork::authAccessHandler(')
      .includes('webAccessRevokeId(g_webAccess, body.id)'));
  assert.ok(core.includes('copyWebAccessLabel(stored.remembered[target].label, label)') &&
      core.includes('copyWebAccessLabel(target->label, label)'));
  assert.ok(codeIncludes(rawRuntimeJs, 'remember: $("loginRemember").checked, device: deviceLabel()'));
  const deviceRow = blockAt(viewJs.admin, 'function deviceRow(');
  assert.ok(deviceRow.includes('title.textContent = name') && deviceRow.includes('small.textContent = detail'),
      'Device labels are rendered as text');
  assert.ok(codeIncludes(ui, '<details id="accessDevices" class="cfgGroup hidden">'),
      'The device list is a collapsible accordion');
  assert.ok(body(access, 'esp_err_t OpenBrewByWeightNetwork::authAccessHandler(')
      .includes('webAccessRenameId(g_webAccess, body.id, body.device)') &&
      blockAt(viewJs.admin, 'function editDeviceName(').includes('input.replaceWith(title)'),
      'Device names are edited in place');
  assert.ok(blockAt(viewJs.admin, 'function renderAccessDevices(').includes('querySelector(".confirm, input")'),
      'Polls do not wipe a row that is asking for confirmation');
  // Sign out sits inline in the footer's firmware line, shown only with the
  // login on, and keeps the line height (no tap-size min-height there).
  assert.ok(codeIncludes(ui, 'id="footerSignOut"') &&
      blockAt(rawRuntimeJs, 'function renderHomeStream(').includes('session.hidden = !status.access?.login') &&
      css.includes('.pageFooter .textLock{min-width:0;min-height:0'));
  // The locked Admin page never announces the public factory password.
  assert.ok(!codeIncludes(ui, 'adminFactoryHint') && !codeIncludes(viewJs.admin, 'adminFactoryHint'));
  // Home's "Set up" lands on the Access panel once Admin is unlocked.
  assert.ok(codeIncludes(ui, 'href="/admin#accessPanel"') &&
      blockAt(viewJs.admin, 'function applyAdminStatus(').includes('location.hash'));
  assert.ok(blockAt(rawRuntimeJs, 'function renderHomeStream(').includes('applyPairingPrompt(status.access)') &&
      !blockAt(rawRuntimeJs, 'function applyCommonStatus(').includes('applyPairingPrompt'),
      'Only the Home stream drives the pairing prompt');

  // A remembered session goes to localStorage, a browser-only one to
  // sessionStorage, and every API request carries it.
  const store = () => {
    const values = new Map();
    return {getItem: (k) => values.get(k) ?? null, setItem: (k, v) => values.set(k, String(v)),
            removeItem: (k) => values.delete(k), values};
  };
  const context = vm.createContext({localStorage: store(), sessionStorage: store(),
    WEB_UI_CLIENT_HEADER: 'X-WebUI-Client', WEB_UI_SESSION_HEADER: 'X-WebUI-Session',
    SESSION_KEY: 'obbwSession', sessionMemory: '', webUiClientId: 'abc'});
  vm.runInContext(['function sessionToken(', 'function storeSession(', 'function webUiHeaders(']
      .map((marker) => blockAt(rawRuntimeJs, marker)).join('\n'), context);
  assert.deepEqual({...context.webUiHeaders()}, {'X-WebUI-Client': 'abc'});
  context.storeSession('t1', true);
  assert.equal(context.localStorage.values.get('obbwSession'), 't1');
  assert.equal({...context.webUiHeaders({a: 1})}['X-WebUI-Session'], 't1');
  context.storeSession('t2', false);
  assert.equal(context.localStorage.values.size, 0);
  assert.equal(context.sessionStorage.values.get('obbwSession'), 't2');
  context.storeSession('', false);
  assert.deepEqual({...context.webUiHeaders()}, {'X-WebUI-Client': 'abc'});
  // Through the companion every screen shares one session: Home names the
  // screen holding the Admin unlock or the override by the 8-hex FNV-1a of
  // its client id, and only that screen unlocks; the direct flags still win.
  vm.runInContext(rawRuntimeJs.slice(rawRuntimeJs.indexOf('const webUiScreen ='),
      rawRuntimeJs.indexOf('let webUiOwner =')).replace(/^const /gm, 'var '), context);
  assert.equal(context.webUiScreen, '1a47e90b', 'FNV-1a of "abc", lowercase and zero-padded');
  for (const [s, admin, override] of [
    [{adminUnlocked: true, webUiOverrideActive: true}, true, true],
    [{relay: {adminScreen: '1a47e90b', overrideScreen: ''}}, true, false],
    [{relay: {adminScreen: '00000000', overrideScreen: '1a47e90b'}}, false, true],
    [{relay: {adminScreen: '', overrideScreen: ''}}, false, false],
    [{}, false, false],
  ]) {
    assert.equal(context.adminUnlockedFor(s), admin, JSON.stringify(s));
    assert.equal(context.webUiOverrideFor(s), override, JSON.stringify(s));
  }
  for (const [source, needle] of [
    [rawRuntimeJs, 'const admin=adminUnlockedFor(s),'],
    [rawRuntimeJs, '(adminUnlockedFor(s)?s.bleScan'],
    [rawRuntimeJs, "if(v==='admin'&&adminUnlockedFor(s)){"],
    [viewJs.admin, 'const unlocked=R.adminUnlockedFor(s);'],
    [viewJs.admin, 'a=R.webUiOverrideFor(s)&&r>0,'],
    [viewJs.diagnostic, 'const u=R.adminUnlockedFor(s);'],
    [viewJs.diagnostic, '(R.adminUnlockedFor(s)||!!s.development)&&'],
  ])
    assert.ok(codeIncludes(source, needle), 'Admin unlock follows this screen: ' + needle);
  // A companion screen set aside for a direct page says so, with Reload.
  assert.ok(codeIncludes(rawRuntimeJs, 'deactivateWebUi(event.reason);') &&
      codeIncludes(blockAt(rawRuntimeJs, 'function deactivateWebUi(reason) {'),
          "if(reason==='direct')setInactiveHint(__WEBUI_TEXT__('runtime.opened_directly'));showInactiveOverlay();") &&
      codeIncludes(blockAt(rawRuntimeJs, 'function setInactiveHint(text) {'), 'h.dataset.d||=h.textContent'),
      'The direct takeover explains itself and the overlay keeps its own text');
  // Home cards show independently, each whenever its own condition holds, and
  // none waits for another or for the grind hint. The controller keeps every
  // dismissal; one left in the browser under a legacy card key is handed over.
  // A close (×) leaves an Undo strip and commits only when it ends.
  const order = ['laMarzoccoPolicyHint', 'laMarzoccoAccountHint', 'companionHint', 'companionPairHint', 'accessHint', 'laMarzoccoCloudHint',
    'recipeHint', 'scaleOnlyHint'];
  const nodes = {grindHintCard: {hidden: true}};
  for (const id of order) nodes[id] = {hidden: true};
  const $ = (id) => nodes[id] ||= {textContent: '', hidden: false, dataset: {},
    classList: {toggle() {}, add() {}, remove() {}, contains: () => false}};
  const cards = new Map(), here = {companionHere: null}, posted = [];
  let recorded = false, pairable = false, cycle = '1/7';
  const closes = [];
  const home = {R: {$, applyHomeStatus() {}, reveal() {}, homeStatusCache: () => null, presetState: {items: []},
    cardDismissed: (k) => cards.get(k) || '', dismissCard: (k, v = '1') => cards.set(k, v),
    dismissOnController: (card) => posted.push(card), shotRecorded: () => recorded,
    shotCycle: () => cycle, closeCard: (card, text, commit) => closes.push({card, text, commit}),
    controlsMutable: true, uiState: here, companionHere: () => Promise.resolve(here.companionHere),
    companionPairable: () => pairable, sub: (t, a) => t + ':' + a.join(',')},
    document: {}, __LA_MARZOCCO__: false, __WEBUI_TEXT__: (key) => key};
  vm.runInNewContext(viewJs.home.replace(/import\s*\*\s*as\s+R\s+from\s*['"][^'"]+['"];?/, '')
      .replace(/export /g, ''), home);
  const fresh = {access: {notice: true}, machineIntegration: 'la_marzocco_cloud', presetsTag: 7,
    laMarzocco: {accountConfigured: false}, config: {scaleMacCacheMode: 'prefer'}, scale: {preferredMac: 'AA'}};
  const shown = (s) => (home.applyStatus(s), order.filter((id) => !nodes[id].hidden));
  assert.deepEqual(shown(fresh), ['accessHint', 'laMarzoccoCloudHint', 'recipeHint', 'scaleOnlyHint']);
  assert.deepEqual(shown({...fresh, access: {notice: false}}), ['laMarzoccoCloudHint', 'recipeHint', 'scaleOnlyHint']);
  assert.deepEqual(shown({...fresh, dismissed: {access: true}}), ['laMarzoccoCloudHint', 'recipeHint', 'scaleOnlyHint'],
      'The access note follows its controller dismissal');
  const noCloud = {...fresh, access: {notice: false}, laMarzocco: {accountConfigured: true}};
  assert.deepEqual(shown(noCloud), ['recipeHint', 'scaleOnlyHint']);
  recorded = true;
  assert.deepEqual(shown(noCloud), ['scaleOnlyHint']);
  assert.deepEqual(shown({...noCloud, config: {scaleMacCacheMode: 'only'}}), []);
  assert.deepEqual(shown({...noCloud, scale: {preferredMac: ''}}), []);
  // A refused La Marzocco account shows with no
  // dismissal: pending terms offer Accept, which follows the controls lock;
  // a refused sign-in or access names its cause and sends to Settings.
  const refused = (accountProblem) => ({...noCloud, laMarzocco: {accountConfigured: true, accountProblem}});
  const causes = ['auth', 'access'].map((problem) => {
    assert.deepEqual(shown(refused(problem)), ['laMarzoccoAccountHint', 'scaleOnlyHint']);
    return nodes.laMarzoccoAccountHintTitle.textContent + nodes.laMarzoccoAccountHintText.textContent;
  });
  assert.ok(causes[0] && causes[1] && causes[0] !== causes[1], 'Each refusal names its own cause');
  assert.deepEqual(shown(refused('')), ['scaleOnlyHint']);
  const blocked = refused('policy');
  assert.deepEqual(shown(blocked), ['laMarzoccoPolicyHint', 'scaleOnlyHint']);
  assert.strictEqual(nodes.laMarzoccoPolicyHintAccept.disabled, false);
  home.R.controlsMutable = false;
  shown(blocked);
  assert.strictEqual(nodes.laMarzoccoPolicyHintAccept.disabled, true);
  home.R.controlsMutable = true;
  assert.ok(partialHtml.home.includes('id="laMarzoccoPolicyHintAccept"') &&
      !partialHtml.home.includes('laMarzoccoPolicyHintDismiss'), 'The terms card offers Accept, no dismissal');
  assert.ok(partialHtml.home.includes('id="laMarzoccoAccountHint"') &&
      !partialHtml.home.includes('laMarzoccoAccountHintDismiss'), 'The account card sends to Settings, no dismissal');
  // A preset change retires the recipe hint for good, in every browser.
  recorded = false;
  shown({...noCloud, presetsTag: 8});
  assert.deepEqual(posted.splice(0), ['recipe']);
  const gone = {recipe: true, scaleOnly: true, companionPair: false, laMarzoccoCloud: false};
  assert.deepEqual(shown({...noCloud, presetsTag: 9, dismissed: gone}), []);
  assert.deepEqual(posted, [], 'A dismissed note is not sent again');
  // A note dismissed in this browser before the controller kept it is handed over.
  cards.set('obbwCard:laMarzoccoCloud', '1');
  assert.deepEqual(shown({...fresh, dismissed: gone}), ['accessHint']);
  assert.deepEqual(posted.splice(0), ['laMarzoccoCloud']);
  cards.delete('obbwCard:laMarzoccoCloud');
  assert.ok(codeIncludes(viewJs.home, 'R.closeCard($(id),"You won’t see this again.",()=>{R.dismissOnController(cardName(id));renderHints(R.homeStatusCache());});') &&
      !viewJs.home.includes('/api/v1/auth/notice'), 'Closing a note is sent to the controller once its Undo ends');
  for (const id of order.slice(3))
    assert.ok(codeIncludes(partialHtml.home, `<button id="${id}Dismiss" class="cardClose" type="button" aria-label="Dismiss">×</button>`), id);
  for (const id of order.slice(0, 3))
    assert.ok(!partialHtml.home.includes(id + 'Dismiss'), id + ' has no close');
  fresh.dismissed = gone;
  // A page that came through an unpaired companion offers to pair it; the
  // request it sends then replaces that offer, with the code typed in Admin.
  pairable = true;
  here.companionHere = {name: 'nas', paired: false, pairing: {state: 'unpaired'}};
  assert.deepEqual(shown(fresh), ['companionPairHint', 'accessHint', 'laMarzoccoCloudHint']);
  assert.match(nodes.companionPairHintText.textContent, /^This page comes through the companion .*:nas$/);
  const asking = {...fresh, companion: {requestName: 'nas', requestAddress: '192.168.1.20'}};
  assert.deepEqual(shown(asking), ['companionHint', 'accessHint', 'laMarzoccoCloudHint']);
  assert.match(nodes.companionHintText.textContent, /^It is asking from .*:192\.168\.1\.20$/);
  here.companionHere = {name: 'nas', paired: false, pairing: {state: 'pending', code: '123456'}};
  assert.deepEqual(shown(asking), ['companionHint', 'accessHint', 'laMarzoccoCloudHint']);
  assert.match(nodes.companionHintText.textContent, /^Its code is .*:123456$/);
  fresh.dismissed = {...gone, companionPair: true};
  assert.deepEqual(shown(fresh), ['accessHint', 'laMarzoccoCloudHint'], 'the pairing offer can be dismissed for every browser');
  pairable = false;
  nodes.grindHintCard.hidden = false;
  assert.deepEqual(shown(fresh), ['accessHint', 'laMarzoccoCloudHint'], 'the grind hint holds no card back');
  // A scale recording shows its card: Stop while it runs, Delete once it is
  // held. Stop and Delete follow the Diagnostic page's gate, Download only
  // this window's ownership.
  Object.assign(home.R, {webUiOwner: true, diagnosticVisible: false,
    adminUnlockedFor: (s) => !!s.adminUnlocked});
  const recording = {...noCloud, scaleRecording: 'recording', scaleRecordingId: 4};
  shown(recording);
  assert.equal(nodes.scaleRecCard.hidden, false);
  assert.ok(!nodes.scaleRecStop.hidden && nodes.scaleRecDelete.hidden);
  assert.equal(nodes.scaleRecTitle.textContent, 'Scale recording in progress.');
  assert.equal(nodes.scaleRecStop.disabled, true);
  assert.equal(nodes.scaleRecDownload.disabled, false);
  for (const gate of [{adminUnlocked: true}, {development: true}]) {
    shown({...recording, ...gate});
    assert.equal(nodes.scaleRecStop.disabled, false, JSON.stringify(gate));
  }
  home.R.diagnosticVisible = true;
  shown(recording);
  assert.equal(nodes.scaleRecStop.disabled, false);
  home.R.webUiOwner = false;
  shown(recording);
  assert.ok(nodes.scaleRecStop.disabled && nodes.scaleRecDownload.disabled);
  home.R.webUiOwner = true;
  const held = {...recording, scaleRecording: 'held'};
  shown(held);
  assert.ok(nodes.scaleRecStop.hidden && !nodes.scaleRecDelete.hidden && !nodes.scaleRecDelete.disabled);
  assert.equal(nodes.scaleRecTitle.textContent, 'Scale recording ready.');
  shown({...held, scaleRecording: 'busy'});
  assert.ok(nodes.scaleRecDelete.disabled && !nodes.scaleRecDownload.disabled,
      'Delete waits for the save; Download stays available');
  // A handled recording stays hidden until the next one; no recording clears it.
  // Its close hides it until the next shot or a change of state.
  cards.set('obbwCard:scaleRec', nodes.scaleRecCard.dataset.snooze);
  assert.equal(nodes.scaleRecCard.dataset.snooze, '4h1/7');
  shown(held);
  assert.equal(nodes.scaleRecCard.hidden, true);
  shown({...held, scaleRecording: 'busy'});
  assert.equal(nodes.scaleRecCard.hidden, true, 'A save in progress is the same held state');
  cycle = '1/8';
  shown(held);
  assert.equal(nodes.scaleRecCard.hidden, false, 'The next shot brings it back');
  cards.set('obbwCard:scaleRec', nodes.scaleRecCard.dataset.snooze);
  shown(recording);
  assert.equal(nodes.scaleRecCard.hidden, false, 'A change of state brings it back');
  cards.set('obbwCard:scaleRec', '4');
  shown(held);
  assert.equal(nodes.scaleRecCard.hidden, true);
  shown({...held, scaleRecordingId: 5});
  assert.equal(nodes.scaleRecCard.hidden, false);
  shown({...held, scaleRecording: '', scaleRecordingId: 0});
  assert.ok(nodes.scaleRecCard.hidden && cards.get('obbwCard:scaleRec') === '');
  assert.ok(codeIncludes(rawPartialHtml.home, '<section id="scaleRecCard" class="homeCard warnHint" hidden>') &&
      codeIncludes(partialHtml.home, '<button id="scaleRecDismiss" class="cardClose" type="button" aria-label="Hide for now">×</button>') &&
      codeIncludes(viewJs.home, 'R.closeCard($("scaleRecCard"),"Hidden until your next shot.",()=>{R.dismissCard(cardKey("scaleRec"),key);') &&
      codeIncludes(viewJs.home, 'R.command("/api/v1/diagnostic/scale-profile",{action:"stop"})') &&
      codeIncludes(viewJs.home, '"Delete this scale recording? It cannot be recovered.")&&handled(await R.command("/api/v1/diagnostic/scale-profile",{action:"delete"}))') &&
      codeIncludes(viewJs.home, '$("scaleRecDownload").onclick=async()=>handled(await R.downloadScaleProfile());') &&
      codeIncludes(rawRuntimeJs, 'apiBinary("/api/v1/diagnostic/scale-profile/download")'),
      'The recording card stops and deletes through the Diagnostic actions and downloads without stopping');
  assert.ok(codeIncludes(rawPartialHtml.home, 'href="/settings#recipeSettings"') &&
      codeIncludes(rawPartialHtml.settings, 'id="recipeSettings"') &&
      !codeIncludes(rawPartialHtml.home, 'type="checkbox"'));
  // A close hides its card behind its own Undo strip. Undo brings it back and
  // commits nothing; its strip's end or a hidden page commits. Closes in a row
  // keep independent strips, each with its own countdown.
  const closeSrc = rawRuntimeJs.slice(rawRuntimeJs.indexOf('const closing = new Map();'),
      rawRuntimeJs.indexOf('function firmwareResult('));
  const timers = [], hideListeners = [], el = () => ({remove() { this.removed = true; },
    setAttribute() {}, append(...c) { this.kids = c; }, focus() {},
    style: {setProperty(k, v) { this[k] = v; }}});
  const card = () => {
    const c = new Set();
    return {classList: {add: (x) => c.add(x), remove: (x) => c.delete(x), has: (x) => c.has(x)},
      after(strip) { this.strip = strip; }, querySelector: () => ({focus() {}})};
  };
  const close = {__WEBUI_TEXT__: (k) => k, clearTimeout() {}, Map,
    getComputedStyle: () => ({borderLeftColor: 'rgb(1, 2, 3)'}),
    setTimeout: (f, ms, ...a) => timers.push(() => f(...a)),
    document: {createElement: el, addEventListener: (t, f) => hideListeners.push(f), hidden: false}};
  vm.runInNewContext(closeSrc + ';this.closeCard = closeCard;', close);
  const commits = [], a = card(), b = card();
  close.closeCard(a, 'gone', () => commits.push('a'));
  assert.ok(a.classList.has('closing') && a.strip.kids[0] === 'gone' && commits.length === 0);
  assert.equal(a.strip.style['--cardAc'], 'rgb(1, 2, 3)', 'The strip takes its card\'s accent');
  a.strip.kids[1].onclick();
  assert.ok(!a.classList.has('closing') && a.strip.removed && commits.length === 0, 'Undo commits nothing');
  close.closeCard(a, 'gone', () => commits.push('a'));
  close.closeCard(b, 'gone', () => commits.push('b'));
  assert.ok(commits.length === 0 && !a.strip.removed && !b.strip.removed,
      'Another close keeps the first strip and its Undo');
  b.strip.kids[1].onclick();
  assert.ok(commits.length === 0 && !b.classList.has('closing') && a.classList.has('closing'),
      'Each Undo restores only its own card');
  timers.at(-2)();
  assert.deepEqual(commits, ['a'], 'Each strip commits when its own countdown ends');
  close.closeCard(a, 'gone', () => commits.push('a2'));
  close.closeCard(b, 'gone', () => commits.push('b2'));
  close.document.hidden = true;
  hideListeners.forEach((f) => f());
  assert.deepEqual(commits, ['a', 'a2', 'b2'], 'A hidden page commits every open strip');
  // Admin brings every hidden note back, on the controller and in this browser.
  assert.ok(codeIncludes(partialHtml.admin, '<button id="restoreNotesButton" class="btnGlyph mutable" type="button">') &&
      codeIncludes(viewJs.admin, 'if(k.startsWith("obbwCard:"))localStorage.removeItem(k);') &&
      codeIncludes(viewJs.admin, 'R.command("/api/v1/home/dismiss",{restore:true},false,"","Could not bring back the hidden notes."') &&
      codeIncludes(viewJs.admin, 'Object.values(d).filter((v)=>v===true).length+(d.grind?.length||0)'),
      'Admin restores hidden notes');
}
