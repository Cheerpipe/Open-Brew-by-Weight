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
      body(network, 'bool OpenBrewByWeightNetwork::requireActiveWebUiClient(').indexOf('activeWebUiClientId_'),
      'Every claimed API route checks the session before the claim');
  assert.ok(body(network, 'esp_err_t OpenBrewByWeightNetwork::claimHandler(')
      .includes('webAccessSessionAllowed(request)'), 'Claiming control needs a session');
  for (const route of ['{"/api/v1/auth", HTTP_GET, authStatusHandler, false, true}',
                       '{"/api/v1/auth/login", HTTP_POST, authLoginHandler, false, true}',
                       '{"/api/v1/auth/logout", HTTP_POST, authLogoutHandler, false, true}',
                       '{"/api/v1/auth/access", HTTP_POST, authAccessHandler}',
                       '{"/api/v1/auth/notice", HTTP_POST, authNoticeHandler}',
                       '{"/api/v1/auth/pairing", HTTP_POST, authPairingHandler}']) {
    assert.ok(network.includes(route), `Missing access route ${route}`);
  }
  assert.ok(network.includes('if (matched->open) return matched->handler(request);'));
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
}
