{
  // SoftAP onboarding: captive portal + admin bypass + /setup live apply.
  const unlockBlock = blockAt(network, 'bool OpenBrewByWeightNetwork::adminUnlockAllowed(');
  const socketBlock =
      blockAt(network, 'bool OpenBrewByWeightNetwork::socketServedBySoftAp(');
  const handshake =
      network.slice(network.indexOf('esp_err_t OpenBrewByWeightNetwork::uiStreamHandshake('),
                    network.indexOf('void OpenBrewByWeightNetwork::uiStreamFree'));
  const ensureApBlock =
      blockAt(network, 'bool OpenBrewByWeightNetwork::ensureAccessPoint(');
  const stopSoftApBlock =
      blockAt(network, 'void OpenBrewByWeightNetwork::stopSoftAp(bool stopHttp) {');
  const startStationBlock =
      blockAt(network, 'void OpenBrewByWeightNetwork::startStation(');
  const stopNetworkBlock =
      blockAt(network, 'void OpenBrewByWeightNetwork::stopNetwork() {');
  const notFoundBlock =
      blockAt(network, 'esp_err_t OpenBrewByWeightNetwork::notFoundHandler(');
  const serviceBlock = blockAt(network, 'void OpenBrewByWeightNetwork::service() {');
  const staStateBlock =
      blockAt(network, 'void OpenBrewByWeightNetwork::serviceStaState(');
  const confirmBlock =
      blockAt(network, 'bool OpenBrewByWeightNetwork::confirmPendingNetwork(');
  const requestConfirmBlock = blockAt(
      network, 'void OpenBrewByWeightNetwork::requestPendingNetworkConfirm(bool fromOwner) {');
  const revertBlock =
      blockAt(network, 'bool OpenBrewByWeightNetwork::revertPendingNetwork(');
  const snapshotBlock =
      blockAt(network, 'NetworkStatusSnapshot OpenBrewByWeightNetwork::snapshot() {');
  const networkHandlerBlock =
      blockAt(network, 'esp_err_t OpenBrewByWeightNetwork::networkHandler(');
  const saveCommandBlock = network.slice(
      network.indexOf('case WebCommandType::SAVE_NETWORK:'),
      network.indexOf('case WebCommandType::FORGET_NETWORK:'));

  // Admin unlock bypass: SoftAP peers are pre-authenticated by the WPA2
  // passphrase, per client, and only while the AP is up.
  if (!networkHeader.includes('static bool socketServedBySoftAp(int socketFd);') ||
      !socketBlock.includes('getsockname') || !socketBlock.includes('getpeername') ||
      !socketBlock.includes('0xC0A80401u') || !socketBlock.includes('0xFFFFFF00u') ||
      !codeIncludes(unlockBlock, 'apActive&&socketServedBySoftAp(httpd_req_to_sockfd(request))')) {
    throw new Error(
        'SoftAP admin bypass must match the AP interface address and peer subnet, gated on apActive');
  }
  if (!codeIncludes(handshake, 'session.fromSoftAp=socketServedBySoftAp(session.fd)') ||
      (network.match(/unlocked \|= session\.fromSoftAp/g) || []).length !== 1) {
    throw new Error(
        'The Home stream must OR the latched SoftAP session flag into adminUnlocked, which Diagnostic reads from it');
  }

  // Captive portal: wildcard DNS owned by the AP lifecycle, DHCP offers the
  // device as DNS, probe hosts 302 to /setup only while the AP is up.
  if (!sdkconfigDefaults.split('\n').includes('CONFIG_ARDUINO_SELECTIVE_DNSServer=y')) {
    throw new Error('The Arduino DNSServer component must be enabled for the captive portal');
  }
  if (!codeIncludes(network, 'captiveDns_.start(53,"*",ip)') ||
      !networkHeader.includes('DNSServer captiveDns_;') ||
      !codeIncludes(ensureApBlock, 'stopCaptivePortalDns()') ||
      !codeIncludes(ensureApBlock, 'startCaptivePortalDns()') ||
      !stopSoftApBlock.includes('stopCaptivePortalDns()') ||
      !startStationBlock.includes('stopCaptivePortalDns()') ||
      !stopNetworkBlock.includes('stopCaptivePortalDns()') ||
      !revertBlock.includes('stopCaptivePortalDns()')) {
    throw new Error(
        'Captive DNS must be wildcard, network-task owned, and paired with every AP teardown/raise path');
  }
  if (!codeIncludes(ensureApBlock,
                    'WiFi.softAPConfig(ip,ip,IPAddress(255,255,255,0),(uint32_t)0,ip)')) {
    throw new Error('SoftAP DHCP must offer the device IP as the DNS server (option 6)');
  }
  const probeHosts = [
    '"captive.apple.com"', '"connectivitycheck.gstatic.com"',
    '"connectivitycheck.android.com"', '"clients3.google.com"',
    '"www.msftconnecttest.com"', '"www.msftncsi.com"'];
  const startDnsBlock =
      blockAt(network, 'void OpenBrewByWeightNetwork::startCaptivePortalDns() {');
  if (probeHosts.some((host) => !network.includes(host)) ||
      !notFoundBlock.includes('CAPTIVE_PORTAL_URL') ||
      !codeIncludes(notFoundBlock,
                    'instance_->snapshot().apActive?captiveProbeHost(request):nullptr') ||
      !codeIncludes(notFoundBlock,
                    'instance_->portalExitRequested_.load(std::memory_order_acquire)') ||
      !notFoundBlock.includes('serveProbeSuccess(request, probe)') ||
      !notFoundBlock.includes('"/api/"') || !notFoundBlock.includes('STATUS_NOT_FOUND') ||
      !network.includes('constexpr const char CAPTIVE_PORTAL_URL[] = "http://192.168.4.1/setup";') ||
      network.indexOf('constexpr const char CAPTIVE_PORTAL_URL[]') <
          network.indexOf('constexpr const char *AP_IP') ||
      !networkHeader.includes('std::atomic<bool> portalExitRequested_{false}') ||
      !codeIncludes(startDnsBlock,
                    'portalExitRequested_.store(false,std::memory_order_release)') ||
      !network.includes('<TITLE>Success</TITLE>') ||
      !network.includes('"204 No Content"') ||
      !network.includes('"Microsoft Connect Test"')) {
    throw new Error(
        'Captive probes must 302 to /setup only while the AP is up, answer the OS success bodies when a portal exit is armed, and reset the arming per DNS session; API 404s stay JSON');
  }

  // Live apply: /setup saves connect without a restart on the network task.
  if (!domainCore.includes('bool applyLive = false;') ||
      !codeIncludes(networkHandlerBlock, 'jsonHasOnlyUniqueFields(root,saveFields,13)') ||
      !networkHandlerBlock.includes('"apply"') ||
      !codeIncludes(networkHandlerBlock, 'strcmp(applyMode,"live")!=0') ||
      !codeIncludes(networkHandlerBlock, '!self.snapshot().apActive') ||
      !codeIncludes(networkHandlerBlock, 'command.network.applyLive=true') ||
      !codeIncludes(saveCommandBlock, 'if(command.network.applyLive){') ||
      !codeIncludes(saveCommandBlock, 'advanceSetupPhase(SetupEvent::APPLY_SAVED)') ||
      !codeIncludes(saveCommandBlock, '}else{restartPending_=true;}')) {
    throw new Error(
        'apply:"live" must be schema-validated, accepted only while the AP is up, and must not schedule a restart');
  }
  if (!codeIncludes(serviceBlock, 'setupPhase()==SetupPhase::QUEUED') ||
      !codeIncludes(serviceBlock, 'beginStationConnect(networkSettingsSnapshot(),now)') ||
      !codeIncludes(serviceBlock, 'advanceSetupPhase(SetupEvent::ATTEMPT_STARTED)') ||
      !codeIncludes(serviceBlock, 'advanceSetupPhase(SetupEvent::APPLY_RESTARTS)') ||
      !codeIncludes(serviceBlock, 'armPendingConfirmWindow(now)')) {
    throw new Error('The network task must consume the queued live apply and arm the confirm window');
  }
  // One explicit onboarding phase (host-tested in OpenBrewByWeightNetworkSetup.h)
  // replaces the keep/live-apply booleans; the network task is its only
  // writer and httpd reads the published copy under the lock.
  if (!networkHeader.includes('#include "OpenBrewByWeightNetworkSetup.h"') ||
      !networkHeader.includes('SetupPhase setupPhase = SetupPhase::NONE;') ||
      /apKeepRequested_|liveApplyPending_|apKeepActive|softApTeardownDue/.test(
          network + networkHeader)) {
    throw new Error('SoftAP onboarding must be driven by the published SetupPhase only');
  }
  // Confirm is bound to the live attempt: status polls and the confirm
  // action only persist a network this attempt joined (F-04). Onboarding
  // tests the network first: the join on the saved SSID waits for the
  // owner's "Save Wi-Fi" (the confirm action) for the longer onboarding
  // window, status polls and the UI stream never confirm it, and a stale
  // grace from an earlier saved network cannot close the SoftAP mid-attempt;
  // a join on another SSID fails the attempt; the unconfirmed window reverts
  // while the SoftAP stays the fallback.
  if (!codeIncludes(requestConfirmBlock, 'setupConfirmAllowed(status_.setupPhase,status_.apActive)') ||
      !codeIncludes(snapshotBlock, '(!status_.apActive||setupKeepsSoftAp(status_.setupPhase))') ||
      !codeIncludes(confirmBlock, '!setupConfirmAllowed(status.setupPhase,status.apActive)') ||
      !codeIncludes(networkHandlerBlock, '!setupConfirmAllowed(network.setupPhase,network.apActive)') ||
      !codeIncludes(confirmBlock,
                    'status_.setupPhase=nextSetupPhase(status_.setupPhase,SetupEvent::CONFIRMED)') ||
      !codeIncludes(staStateBlock, 'esp_wifi_sta_get_ap_info(&ap)') ||
      !codeIncludes(staStateBlock, 'joined?SetupEvent::JOINED:SetupEvent::JOINED_OTHER') ||
      staStateBlock.includes('pendingConfirmRequest_ = true') ||
      !codeIncludes(requestConfirmBlock, '(fromOwner||!setupAwaitsOwnerSave(status_.setupPhase,status_.apActive))') ||
      !codeIncludes(networkHandlerBlock, 'self.requestPendingNetworkConfirm(true);') ||
      !codeIncludes(staStateBlock, 'setupKeepsSoftAp(status.setupPhase)?SETUP_CONFIRM_TIMEOUT_MS:STA_CONFIRM_TIMEOUT_MS') ||
      !networkHeader.includes('SETUP_CONFIRM_TIMEOUT_MS = 600000') ||
      !codeIncludes(network, 'if(setupKeepsSoftAp(status_.setupPhase))status_.apTeardownAtMs=0;') ||
      // "Keep" / "Continue without Wi-Fi" drop an unsaved test now through
      // the same expiry path (disconnect, revert, retry on a closed gate).
      !codeIncludes(networkHandlerBlock, 'elseif(parsed&&strcmp(action,"discard")==0){') ||
      !codeIncludes(networkHandlerBlock, 'self.requestPendingNetworkDiscard();') ||
      !codeIncludes(blockAt(network, 'void OpenBrewByWeightNetwork::requestPendingNetworkDiscard() {'),
                    'if(staConfirmArmed_&&status_.apActive&&setupDiscardAllowed(status_.setupPhase)){staConfirmDeadlineMs_=millis();}') ||
      // Post-confirm grace: confirm schedules the SoftAP teardown (keeping an
      // earlier "finish" linger); with the station up the AP stops at the
      // deadline, and a recovery AP with phones associated gets the grace
      // too (F-10). Every teardown/re-raise clears the deadline.
      !codeIncludes(confirmBlock,
                    'if(status.apActive&&status_.apTeardownAtMs==0){status_.apTeardownAtMs=millis()+SOFTAP_TEARDOWN_GRACE_MS;}') ||
      (staStateBlock.match(/serviceSoftApTeardown\(status, now\);/g) || []).length !== 2 ||
      !codeIncludes(network, 'softApTeardownStep(status.setupPhase,status.apClients,status.apTeardownAtMs,now)') ||
      !stopSoftApBlock.includes('status_.apTeardownAtMs = 0;') ||
      !codeIncludes(stopSoftApBlock, 'nextSetupPhase(status_.setupPhase,SetupEvent::AP_STOPPED)') ||
      !codeIncludes(revertBlock, 'nextSetupPhase(status_.setupPhase,SetupEvent::REVERTED)') ||
      !codeIncludes(revertBlock, 'status_.staState=StaState::NOT_CONFIGURED') ||
      !codeIncludes(staStateBlock,
                    'status.apActive&&setupKeepsSoftAp(status.setupPhase)&&staConfirmArmed_') ||
      !codeIncludes(staStateBlock, 'revertPendingNetwork(now,"confirm timeout")')) {
    throw new Error(
        'Live-apply confirm must be bound to the joined attempt, saved only by the owner, and revert to the SoftAP fallback');
  }
  // "finish" ends a saved session early (an unsaved joined attempt is not
  // finished), and status/admin publishes the phase, the AP closing time,
  // and the confirm window for /setup.
  if (!codeIncludes(networkHandlerBlock,
                    'grace=setupFinishAllowed(self.status_.setupPhase,self.status_.apActive,self.status_.apTeardownAtMs,now)') ||
      !network.includes('\\"setup\\":\\"%s\\",\\"setupBrew\\":\\"%s\\",\\"setupPresetId\\":%u,') ||
      !network.includes('\\"apClosingMs\\":%lu,') ||
      !codeIncludes(network, 'setupPhaseName(network.setupPhase),setupBrewName(network.setupBrew)')) {
    throw new Error('finish must follow the teardown grace and status/admin must publish setup, the brewing choice, and apClosingMs');
  }

  // Scan pipeline: the HTTP handler's optimistic QUEUED must not be canceled
  // by serviceWifiScan while its own maintenance lease is still landing
  // (every /setup or Admin reload logged a spurious cancel before the scan
  // started); only task-owned (requested) or on-radio (RUNNING) scans cancel,
  // and a dropped scan command retires the optimistic state itself.
  const serviceWifiScanBlock =
      blockAt(network, 'void OpenBrewByWeightNetwork::serviceWifiScan(');
  if ((serviceWifiScanBlock.match(
          /canceled = requested \|\| state == WifiScanState::RUNNING \|\|/g) || [])
          .length !== 0 ||
      (serviceWifiScanBlock.match(
          /canceled = requested \|\| state == WifiScanState::RUNNING;/g) || [])
          .length !== 2 ||
      !network.includes('abortWifiScan(now, false);') ||
      !codeIncludes(network,
          'if(acceptedCommand_.type==WebCommandType::START_WIFI_SCAN){abortWifiScan(now,false);}')) {
    throw new Error(
        'Unsafe-control scan cancels must skip the optimistic handler QUEUED; dropped scan commands retire it');
  }

  // Onboarding attempts must fail fast and stop the retry loop: a wrong
  // password can leave the driver mid-status (AUTH_EXPIRE keeps the last
  // status), so the attempt ages at 20 s instead of 60 s, and the failed
  // attempt holds reconnects (each retry aborted every UI scan, leaving an
  // empty picker after Try again) until a new save, confirm, or revert.
  if (!networkHeader.includes('LIVE_APPLY_ATTEMPT_MS = 20000') ||
      !codeIncludes(staStateBlock,
          '(liveOnboarding?LIVE_APPLY_ATTEMPT_MS:STA_RECOVERY_ATTEMPT_MS)') ||
      !codeIncludes(staStateBlock,
          'advanceSetupPhase(SetupEvent::ATTEMPT_FAILED)') ||
      !codeIncludes(staStateBlock,
          'reconnectHeld=staReconnectHeld_||setupHoldsReconnect(status.setupPhase)') ||
      !codeIncludes(serviceBlock, 'staReconnectHeld_=false') ||
      !codeIncludes(confirmBlock, 'staReconnectHeld_=false') ||
      !codeIncludes(revertBlock, 'staReconnectHeld_=false')) {
    throw new Error(
        'Live-apply attempts must age at 20 s and hold reconnects on failure until the next save/confirm/revert');
  }

  // /setup web view: route, menu-less shell without the single-browser claim,
  // live-apply payload, status-driven steps, bounded connect wait, the skip
  // exit, and the LAN ADMIN_LOCKED escape hatch.
  const setupJs = viewJs.setup || '';
  const setupHtml = partialHtml.setup || '';
  if (!codeIncludes(appJsSource, 'if(bootView==="setup"){R.startSetupSession();renderRoute(location.pathname);}else R.claimWebUiOwnership();') ||
      !codeIncludes(blockAt(runtimeJs, 'function deactivateWebUi(reason) {'), 'if(!webUiOwner||setupSession)return;') ||
      !codeIncludes(blockAt(runtimeJs, 'function resetWebUiInactivity() {'), 'if(!webUiOwner||setupSession)return;') ||
      !codeIncludes(runtimeJs, 'if(!webUiPollingActive()||setupSession)return;') ||
      !codeIncludes(runtimeJs, 'if(webUiPollingActive()&&!setupSession)startUiStream();') ||
      !codeIncludes(blockAt(runtimeJs, 'function applyCompatibilityChrome() {'), '!["admin","diagnostic","setup"].includes(activeView)') ||
      !codeIncludes(blockAt(runtimeJs, 'function startSetupSession() {'), 'webUiActiveUntil=Infinity') ||
      // Steps come from the firmware phase (a joined test waits on
      // "Connected" for Save Wi-Fi, which confirms it), errors sit above the
      // form and are focused/announced, blocked configuration is explained,
      // and the summary names the next step with copyable addresses.
      !codeIncludes(setupJs, 'R.body({action:"confirm"})') ||
      !codeIncludes(setupJs, 'if(lastPhase==="confirmed")returnwifiSaved(n);') ||
      !codeIncludes(setupJs, 'if(lastPhase==="joined")returnstate==="connected"?noteConfirmWindow(n):showConnected(n);') ||
      !codeIncludes(setupJs, 'if(lastPhase==="joined")returnshowConnected(n);') ||
      !codeIncludes(setupJs, 'n.apClosingMs>0&&n.staState==="CONNECTED"') ||
      !codeIncludes(setupJs, 's.configMutable===false?s.configLockReason:""') ||
      !codeIncludes(setupJs, 'box.focus({preventScroll:true})') ||
      !codeIncludes(setupJs, 'navigator.clipboard.writeText(text.textContent)') ||
      !codeIncludes(setupJs, 'if(finished)window.location.href=PROBE_URL;') ||
      // The public factory password never survives setup: part one sets the
      // owner's own (typed twice, never the factory one) before the brewing
      // choice, waiting for the device to persist it, without restarting the
      // setup network; save and skip there still require it.
      !codeIncludes(setupJs, 'if (pass) await setPassword(password); if (brew) await R.api("/api/v1/network"') ||
      !codeIncludes(setupJs, 'saved||=c.state==="PERSISTED"||c.state==="APPLIED";') ||
      // ...and for the controller to release its lock, so Wi-Fi opens on a scan.
      !codeIncludes(setupJs, 'if(saved&&s&&s.configLockReason!=="maintenance")return;') ||
      !codeIncludes(setupJs, 'maintenance:"Saving your settings. Setup continues in a moment."') ||
      !setupHtml.includes('id="setupNewPassword"') ||
      !setupHtml.includes('id="setupConfirmPassword"') ||
      !networkHandlerBlock.includes('"DEVICE_PASSWORD_REQUIRED"') ||
      !network.includes('command.network.applyLive = socketServedBySoftAp(httpd_req_to_sockfd(request));') ||
      !network.includes('apRestartPending_ = snapshot().apActive && !command.network.applyLive;') ||
      !setupHtml.includes('id="setupBlocked"') ||
      !setupHtml.includes('id="setupErrorText" class="fieldError" role="alert"') ||
      setupHtml.indexOf('id="setupError"') > setupHtml.indexOf('<form id="setupForm"') ||
      !setupHtml.includes('id="setupSearchAgain"') ||
      !setupHtml.includes('id="setupNextStep"') ||
      !setupHtml.includes('id="setupTitle"') ||
      setupHtml.includes('id="setupRetry"') ||
      !codeIncludes(appJsSource, '"/setup":"setup"') ||
      !codeIncludes(appJsSource, 'setupView') ||
      !codeIncludes(appJsSource, 'for(const mod of jsMods.values())mod.stop?.()') ||
      !partialHtml.setup ||
      !codeIncludes(setupJs, 'apply:"live"') ||
      !codeIncludes(setupJs, '"/api/v1/status/admin"') ||
      !codeIncludes(setupJs, 'e.code==="ADMIN_LOCKED"') ||
      !setupJs.includes('CONNECT_WAIT_MS') ||
      // The visible 20 s attempt countdown must mirror the firmware bound.
      !codeIncludes(setupJs, 'ATTEMPT_WINDOW_MS=20000') ||
      !codeIncludes(setupJs, 'This usually takes less than') ||
      !partialHtml.setup.includes('id="setupConnectingCountdown"') ||
      !partialHtml.setup.includes('id="setupHiddenToggle"') ||
      !partialHtml.setup.includes('id="setupPasswordShow"') ||
      // Skip exit: quiet action on the form, confirmation state that keeps
      // the device address visible, and a probe-URL link that closes the
      // captive window after the device arms portal-exit success.
      !partialHtml.setup.includes('id="setupSkip"') ||
      !codeIncludes(setupJs, 'captive.apple.com/hotspot-detect.html') ||
      !codeIncludes(setupJs, 'R.body({action:"skip"})') ||
      !codeIncludes(setupJs, 'await discardTest(); await R.api("/api/v1/network", { method: "POST", body: R.body({ action: "skip" }) })') ||
      !codeIncludes(setupJs, 'R.body({action:"discard"})') ||
      !codeIncludes(setupJs, 'wifiState="skipped";showSummary();') ||
      !codeIncludes(setupJs, 'connectivitycheck.gstatic.com/generate_204') ||
      !codeIncludes(setupJs, 'letfinished=!compat&&wifiState!=="joined";') ||
      !codeIncludes(setupJs, '$("setupSkip").onclick=skipSetup') ||
      // Post-confirm grace: the success screen stays readable and its Done
      // button ends the window early (finish arms probe success, then the
      // device lingers briefly before dropping the setup network).
      !partialHtml.setup.includes('id="setupFinish"') ||
      !codeIncludes(setupJs, 'body:R.body({action:"finish"})') ||
      !codeIncludes(setupJs, '$("setupFinish").onclick=finishSetup') ||
      !codeIncludes(setupJs, 'input.type=show?"text":"password"') ||
      !codeIncludes(setupJs, 'button.setAttribute("aria-pressed"') ||
      !codeIncludes(setupJs, 'setInterval(chainWatchdog,2500)') ||
      !codeIncludes(setupJs, 'window.addEventListener("pageshow"') ||
      !codeIncludes(setupJs, '"http://"+deviceHost+".local"') ||
      !partialHtml.setup.includes('id="setupConnectingWhere"') ||
      !partialHtml.setup.includes('autocapitalize="off"') ||
      !partialHtml.setup.includes('2.4 GHz') ||
      !css.includes('body.setupView .topBar,body.setupView .pageNav{display:none}') ||
      !css.includes('.setupCard{') ||
      !css.includes('#setupSkip,.setupBack{')) {
    throw new Error(
        '/setup must be a routed menu-less view with the live-apply save pipeline, the skip portal exit, and captive-portal fallbacks');
  }
  // Brewing onboarding (firmware off only): part one saves the choice with its
  // own "brew" action as an intent unbound to any Wi-Fi (no restart there), so
  // a Wi-Fi revert drops only a legacy bound intent; save/skip/finish carry no
  // brewing fields; a saved "by weight" choice restarts the controller once
  // the setup SoftAP stops; full mode lets the activator close the SoftAP.
  const intentBlock = blockAt(network, 'bool OpenBrewByWeightNetwork::saveSetupBrew(');
  const idleBlock = blockAt(network, 'void OpenBrewByWeightNetwork::serviceSoftApIdle(');
  if (!codeIncludes(network, 'if(!firmwareCompatibilityMode()){return"The brewing choice is part of setup only while the firmware is off.";}') ||
      !codeIncludes(networkHandlerBlock, 'elseif(parsed&&strcmp(action,"brew")==0){') ||
      !codeIncludes(networkHandlerBlock, ':!setupPeer?"No setup network to save the brewing choice.":parseSetupBrew(root,brew,brewPresetId);') ||
      !codeIncludes(networkHandlerBlock, '!self.saveSetupBrew(brew,brewPresetId)') ||
      !codeIncludes(networkHandlerBlock, 'parsed=jsonHasOnlyUniqueFields(root,actionFields,1);if(!parsed){networkError="Skip request must include only action=\\"skip\\".";}') ||
      !codeIncludes(networkHandlerBlock, 'parsed=jsonHasOnlyUniqueFields(root,actionFields,1);if(!parsed){networkError="Finish request must include only action=\\"finish\\".";}') ||
      /persistSetupIntent|keepSetupBrew|intent\.bound = true/.test(network + networkHeader) ||
      !codeIncludes(confirmBlock, 'if(loadSetupIntent(intent)){setupRestartPending_.store(true,std::memory_order_release);') ||
      !codeIncludes(revertBlock, 'if(loadSetupIntent(intent)&&intent.bound)(void)clearSetupIntent();') ||
      !codeIncludes(intentBlock, '!(brew==SetupBrew::USUAL?clearSetupIntent():saveSetupIntent(intent))') ||
      !codeIncludes(intentBlock, 'setupRestartPending_.store(brew==SetupBrew::BY_WEIGHT,std::memory_order_release);') ||
      !codeIncludes(stopSoftApBlock, 'status_.setupBrew=SetupBrew::NONE;') ||
      !codeIncludes(stopSoftApBlock, 'if(setupRestartPending_.load(std::memory_order_acquire)){requestSetupRestart();}') ||
      !codeIncludes(networkHandlerBlock, 'constboolskipped=firmwareCompatibilityMode()&&self.status_.apActive&&self.status_.setupPhase==SetupPhase::NONE;') ||
      !codeIncludes(idleBlock, 'constboolbrewing=activatorOn&&!firmwareCompatibilityMode();') ||
      !codeIncludes(idleBlock, 'if(!brewing&&clients>0){') ||
      !codeIncludes(network, 'serviceSoftApIdle(now,networkSnapshot.apActive,apClients,controlGate().physicalActivatorOn);') ||
      !codeIncludes(setupJs, 'constpart1Done=()=>!factoryPassword&&(!compat||lastBrew!=="none");') ||
      !codeIncludes(setupJs, 'if(!part1Done())returnrenderWelcome();') ||
      !codeIncludes(setupJs, 'body:R.body({action:"brew",...brewPayload()})') ||
      codeIncludes(setupJs, 'apply:"live",...brewPayload()') ||
      !partialHtml.setup.includes('name="setupRecipe" value="255" checked')) {
    throw new Error(
        'Brewing onboarding must stay firmware-off only, be saved by part one unbound to Wi-Fi, restart after the setup SoftAP, and let the activator close it only in full mode');
  }
  // The starting-recipe cards print the factory dose and yield: keep the copy
  // tied to the firmware constants it describes.
  const setupStrings = JSON.parse(fs.readFileSync(path.join(sketchDir, 'web', 'locales', 'en.json'), 'utf8')).strings;
  const brewTypes = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrewTypes.h'), 'utf8');
  const domainHeader = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightDomain.h'), 'utf8');
  const grams = (source, name) => Number((source.match(new RegExp(name + ' = (\\d+);')) || [])[1]) / 10;
  const recipeCopy = (dose, yieldG) => `Dose ${dose} g · Yield ${yieldG} g · 1:${yieldG / dose}`;
  if (setupStrings['setup.recipe_double_numbers'] !==
          recipeCopy(grams(brewTypes, 'DEFAULT_DOSE_DG'), grams(brewTypes, 'DEFAULT_GOAL_WEIGHT_DG')) ||
      setupStrings['setup.recipe_single_numbers'] !==
          recipeCopy(grams(domainHeader, 'FACTORY_SINGLE_DOSE_DG'), grams(domainHeader, 'FACTORY_SINGLE_GOAL_WEIGHT_DG'))) {
    throw new Error('Onboarding recipe dose/yield copy must match the factory preset constants');
  }
  // The new-password check refuses exactly the firmware's factory password.
  const persistedSettings = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightPersistedSettings.h'), 'utf8');
  const factoryPassword = (persistedSettings.match(/DEFAULT_DEVICE_PASSWORD = "([^"]+)"/) || [])[1];
  if (!factoryPassword || !codeIncludes(setupJs, `FACTORY_PASSWORD="${factoryPassword}"`)) {
    throw new Error('Setup FACTORY_PASSWORD must mirror DEFAULT_DEVICE_PASSWORD');
  }
  // /setup is claim-less: only its status, scan, network, and owner-password
  // routes skip the UI claim, and only for a SoftAP peer while the AP is up.
  const ownedBlock = blockAt(network, 'esp_err_t OpenBrewByWeightNetwork::ownedApiHandler(');
  const setupRoutes = (ownedBlock.match(/HTTP_(?:GET|POST|PUT|DELETE), \w+, true\}/g) || []).length;
  if (setupRoutes !== 5 ||
      !codeIncludes(ownedBlock, '{"/api/v1/device/password",HTTP_POST,devicePasswordHandler,true}') ||
      !codeIncludes(ownedBlock, '{"/api/v1/status/admin",HTTP_GET,statusHandler,true}') ||
      !codeIncludes(ownedBlock, '{"/api/v1/network",HTTP_POST,networkHandler,true}') ||
      !codeIncludes(ownedBlock, 'constboolsetupPeer=matched->setup&&self.snapshot().apActive&&socketServedBySoftAp(httpd_req_to_sockfd(request));') ||
      !codeIncludes(ownedBlock, 'if(!setupPeer&&!self.requireActiveWebUiClient(request)){returnESP_OK;}')) {
    throw new Error('Only the /setup routes may skip the UI claim, and only for a SoftAP peer');
  }
}
// Clearing form errors removes only the generated messages, never Setup's
// static error lines that share the class; a stopped Setup chain stays stopped.
{
  const removed = [];
  new Function('document', 'refreshFieldAria', `${blockAt(runtimeJs, 'function clearFieldErrors(')};clearFieldErrors();`)(
      {querySelectorAll: (sel) => sel === 'small.fieldError' ? [{remove: () => removed.push(sel)}] :
        sel === '.invalid' ? [] : [{remove: () => removed.push(sel)}]}, () => {});
  if (removed.join() !== 'small.fieldError' || !partialHtml.setup.includes('<p id="setupErrorText" class="fieldError"')) {
    throw new Error('clearFieldErrors must keep Setup\'s static error lines');
  }
  if (!codeIncludes(rawViewJs.setup, 'export function stop() { running = false;') ||
      !codeIncludes(rawViewJs.setup, 'if (running && bound && !document.hidden && pollTimer === 0 && busyToken !== pollToken) tick()')) {
    throw new Error('The Setup watchdog must not restart a stopped poll chain');
  }
}
// Translated text placed in attributes of runtime-built markup is escaped.
for (const needle of ['aria-label="${escHtml($("modeSignal").dataset.label)}"',
  'aria-label="${escHtml(__WEBUI_TEXT__("runtime.quick_duration"))}"',
  'const t = escHtml(__WEBUI_TEXT__("runtime.preset_graph"))',
  'aria-label="${escHtml(__WEBUI_TEXT__("runtime.graph_scrub"))}"']) {
  if (!rawRuntimeJs.includes(needle)) throw new Error('Unescaped attribute text: ' + needle);
}
// RV-10: the same holds for the Stats preset filter and the Settings override note.
if (!rawViewJs.stats.includes('R.escHtml(__WEBUI_TEXT__("runtime.stats_preset_filter"))') ||
    !rawViewJs.settings.includes('${R.escHtml(__WEBUI_TEXT__("settings.quick_override_note"))} <button') ||
    !rawViewJs.settings.includes('>${R.escHtml(__WEBUI_TEXT__("settings.quick_override_use_saved"))}</button>')) {
  throw new Error('Translated text in view-built markup must be escaped');
}
// Accessibility, i18n and form contracts from the frontend audit (F7).
{
  const failed = [];
  const need = (ok, what) => { if (!ok) failed.push(what); };
  const all = Object.values(partialHtml).join('\n') + shellHtml;
  // FE-M36: keyboard, names, states, focus and motion.
  need(codeIncludes(rawRuntimeJs, 'card.tabIndex=0;card.onkeydown=(e)=>{if(e.target===card&&(e.key===Enter||e.key=== )){'), 'preset cards by keyboard');
  need(css.includes('.presetCard:focus-visible,.presetCardTitle:focus-visible{outline:3px solid var(--ac)'), 'preset card focus ring');
  need(partialHtml.settings.includes('<select id="preferredScaleSelect" aria-label="My scale">'), 'scale select name');
  need(codeIncludes(rawRuntimeJs, 'input.setAttribute("aria-label", __WEBUI_TEXT__("runtime.preset_name"))'), 'rename input name');
  need(codeIncludes(rawViewJs.stats, 'b.setAttribute("role", "radio");b.setAttribute("aria-checked", i === n)'), 'star radios');
  // RV-12: the stars are one tab stop, and arrow keys move to and select a star.
  const fillStars = new Function('document', 'R', '__WEBUI_TEXT__',
      `${blockAt(rawViewJs.stats, 'function fillStarRate(')};return fillStarRate;`)(
      {createElement: () => ({setAttribute() {}})}, {sub: (t) => t}, (k) => k);
  const stars = {classList: {add() {}}, setAttribute() {}, dataset: {}, children: [],
    replaceChildren() { this.children = []; }, appendChild(b) { this.children.push(b); }};
  const picks = [];
  let prevented = 0;
  const key = (k, i) => stars.onkeydown({key: k, target: stars.children[i], preventDefault: () => prevented++});
  fillStars(stars, 3, false, (n, star) => picks.push([n, star]));
  need(stars.children.map((b) => b.tabIndex).join() === '-1,-1,0,-1,-1', 'star tab stop');
  key('ArrowRight', 2); key('ArrowLeft', 2); key('ArrowUp', 0); key('Tab', 2);
  fillStars(stars, 5, false, (n, star) => picks.push([n, star]));
  key('ArrowDown', 4);
  need(JSON.stringify(picks) === '[[4,4],[2,2],[1,1]]' && prevented === 4, 'star arrow keys');
  fillStars(stars, 0, false, () => {});
  need(stars.children.map((b) => b.tabIndex).join() === '0,-1,-1,-1,-1', 'unrated star tab stop');
  need(codeIncludes(rawViewJs.home, 'if (sheet.classList.contains("open") && !sheet.contains(e.relatedTarget)) zone.focus()'), 'sheet focus');
  need(codeIncludes(rawAppJsSource, 'document.title = path === "/" ? baseTitle : a.textContent + " · " + baseTitle'), 'page titles');
  need(codeIncludes(rawAppJsSource, 'renderRoute(target, true)') && codeIncludes(rawAppJsSource, 'section.focus({ preventScroll: true })'), 'route focus');
  need(partialHtml.diagnostic.includes('<textarea id="log" readonly aria-label="Log">'), 'log name');
  need(partialHtml.admin.includes('<span id="scanStatus" role="status">') && partialHtml.admin.includes('<div id="otaStatus" role="status">'), 'live statuses');
  need(codeIncludes(rawViewJs.setup, 'if (document.activeElement?.closest("[hidden]")) $("setupTitle").focus({ preventScroll: true })') &&
      partialHtml.setup.includes('<h1 id="setupTitle" tabindex="-1">'), 'setup step focus');
  // FE-M38: no hard-coded English or $-pattern SSID substitution.
  need(!/\.replace\("\{x\}", (ssid|apSsid)\)/.test(rawViewJs.setup), 'literal SSID replacement');
  need(!/"Power ON"|" \(recorded\)"|"STA: "|" dBm\)"|"startup"\)/.test(rawViewJs.history + rawViewJs.stats + rawViewJs.admin), 'English literals');
  need(!/" UTC"|" B\/s · "|"HW: "|"Machine: "|" s left"|"\?\|Pwr\|Ext/.test(rawViewJs.diagnostic), 'Diagnostic English literals');
  need(!rawViewJs.stats.includes('__WEBUI_TEXT__("runtime.s") : ""'), 'plural by unit suffix');
  // FE-L06: valid structure and names.
  need(!/<menu\b/.test(all), 'buttons inside menu');
  need(['id="headerSignals" class="headerSignals" role="group"', 'id="shotSort" class="shotSort" role="group"',
    'id="historySort" class="shotSort" role="group"'].every((s) => all.includes(s)), 'labelled groups');
  need((partialHtml.setup.match(/<fieldset class="setupChoices" aria-labelledby="setupTitle">/g) || []).length === 2, 'setup choice groups');
  need(!all.includes('<span class="g">') && !/<span class="g">/.test(rawViewJs.history + rawViewJs.stats + rawViewJs.admin), 'decorative glyphs hidden');
  need(shellHtml.includes('aria-labelledby="inactiveHint">') && !shellHtml.includes('aria-labelledby="webUiReload"'), 'inactive dialog name');
  // RV-03: the only History criterion keeps the selected look without a static pressed state.
  need(!partialHtml.history.includes('id="historyDirButton" type="button" aria-pressed') &&
      css.includes('.shotSort button[aria-pressed=true],#historyDirButton{background:var(--pa)'), 'direction button state');
  // FE-L07: form semantics and one submit per action.
  need(partialHtml.admin.includes('id="staPassword" type="password" maxlength="63" autocomplete="current-password"') &&
      partialHtml.setup.includes('id="setupPassword" autocomplete="current-password"') &&
      partialHtml.admin.includes('id="confirmDevicePassword" type="password" minlength="8" maxlength="63" required autocomplete="new-password"'), 'password autocomplete');
  need(partialHtml.admin.includes('<form id="adminUnlockForm" novalidate><label>') && codeIncludes(rawViewJs.admin, '$("adminUnlockForm").onsubmit'), 'unlock form');
  need(codeIncludes(rawViewJs.admin, 'if (unlocking) return;') && codeIncludes(rawViewJs.setup, 'if (skipping) return;'), 'single submit');
  // FE-L13: politeness set before the text changes.
  const message = blockAt(rawRuntimeJs, 'function message(');
  const live = message.indexOf('setAttribute("aria-live"');
  need(live >= 0 && live < message.indexOf('t.textContent = text'), 'live region order');
  if (failed.length) throw new Error('Audit F7 contracts failed: ' + failed.join(', '));
}
