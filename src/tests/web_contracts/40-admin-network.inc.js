{
  const machineFn = blockAt(viewJs.settings, 'function machinePayload(');
  const dateTimeFn = blockAt(viewJs.admin, 'function dateTimePayload(');
  if (!codeIncludes(machineFn, "scaleConnectedLed:$('scaleConnectedLed').checked") ||
      machineFn.includes('ntpServerPreset') ||
      machineFn.includes('timezoneOffsetMinutes') ||
      machineFn.includes('serialLogLevel') ||
      machineFn.includes('ringRetainLogLevel') ||
      machineFn.includes('goalWeightG') ||
      machineFn.includes('brewByWeight') ||
      !dateTimeFn.includes('timezoneId') ||
      !dateTimeFn.includes('ntpServerPreset') ||
      !dateTimeFn.includes('ntpServerCustom') ||
      dateTimeFn.includes('scaleConnectedLed') ||
      !codeIncludes(ui, "saveDateTimeButton').onclick=R.saveDateTimeConfig") ||
      codeIncludes(ui, "saveDateTimeButton').onclick=R.saveMachineConfig") ||
      (codeIncludes(ui, "function markConfigDirty(") &&
       blockAt(viewJs.settings, 'function markConfigDirty(').includes('dateTimeDirtyHint'))) {
    throw new Error(
        'Save machine must send only machine fields; Date & time has its own payload and dirty flag');
  }
}
if (firmware.indexOf('publishLogLevels(serialLogLevelFromRuntime(runtimeConfig)',
                     firmware.indexOf('persistenceReady = EEPROM.begin')) < 0 ||
    firmware.indexOf('publishLogLevels(serialLogLevelFromRuntime(runtimeConfig)',
                     firmware.indexOf('persistenceReady = EEPROM.begin')) >
        firmware.indexOf('BOOT_RESET_REASON') ||
    !codeIncludes(ui, "const LOG_LEVELS=['serialLogLevel','ringRetainLogLevel']") ||
    !codeIncludes(ui, "withBaseRev({[id]:el.value||'none'})") ||
    !codeIncludes(ui, 'for(const id of LOG_LEVELS)$(id).onchange=()=>saveLogLevel(id)') ||
    !codeIncludes(ui, 'baseRevision') ||
    !network.includes('ringRetainLogLevel') ||
    !html.includes('id="homePresetAcc"') ||
    html.includes('id="ruleChart"') ||
    html.indexOf('id="homePresetAcc"') < html.indexOf('id="presetPanel"') ||
    html.indexOf('id="homePresetAcc"') > html.indexOf('id="equipmentPanel"') ||
    html.includes('Extraction rules') ||
    !css.includes('.ruleChart') ||
    css.includes('.ruleSeg') || css.includes('.ruleChartTrack') ||
    css.includes('.ruleTable') ||
    !css.includes('.presetAccItem.open .presetAccDot') ||
    !css.includes('.presetAccPanel,.presetAccMore{display:grid;grid-template-rows:0fr') ||
    !css.includes('.presetAccItem.full .presetAccToggle svg{transform:rotate(180deg)}') ||
    !codeIncludes(ui, 'e.currentTarget.ariaExpanded = item.classList.toggle("full")') ||
    !codeIncludes(ui, 'if (!active) item.classList.remove("full")') ||
    !css.includes('.guardFast i{background:var(--gFast)}') ||
    !css.includes('.guardBbw i{background:var(--ok)}') ||
    !css.includes('.guardSlow i{background:var(--gSlow)}') ||
    !codeIncludes(ui, 'function buildRuleChartModel(') ||
    !codeIncludes(ui, 'function renderHomePresetAccordion(') ||
    !codeIncludes(ui, 'function guardRuleRows(') ||
    !codeIncludes(ui, 'function updateRuleChartFromStatus(') ||
    !codeIncludes(ui, 'fast=!!c.fastExtractionGuardEnabled') ||
    !codeIncludes(ui, 'slow=!!c.slowExtractionGuardEnabled') ||
    !codeIncludes(ui, "'timerOnly'") ||
    !codeIncludes(ui, "'active'") ||
    !codeIncludes(ui, 'function cutLine(t, m)') ||
    !codeIncludes(ui, 'bbwProtectionMs') ||
    !codeIncludes(runtimeJs, 'vector-effect="non-scaling-stroke"') ||
    !firmware.includes('SERIAL_DEBUG_ON') ||
    !firmware.includes('SERIAL_DEBUG_OFF') ||
    !firmware.includes('DEBUG_FULL') ||
    !firmware.includes('DEBUG_OFF') ||
    !firmware.includes('DEBUG_STATUS') ||
    !firmware.includes('WIFI_CONNECT') ||
    !firmware.includes('WIFI_DISCONNECT') ||
    !firmware.includes('WEBUI_STOP') ||
    !firmware.includes('LOG_DUMP') ||
    !firmware.includes('SCALE_STATUS') ||
    !firmware.includes('NTP_STATUS') ||
    !firmware.includes('NET_STATUS') ||
    !firmware.includes('WebCommandType::BUZZER_TEST') ||
    !firmware.includes('WebCommandType::BOOKOO_DEBUG') ||
    !firmware.includes('localBuzzer.request(command.buzzerPattern)') ||
    !firmware.includes('enqueueScaleDebugCommand') ||
    !firmware.includes('executeScaleDebugCommand')) {
  throw new Error('Ring/serial config, rule chart, CLI, and scale/buzzer command paths must remain');
}
{
  const assert = require('assert'), vm = require('vm');
  const strings = JSON.parse(fs.readFileSync(path.join(sketchDir, 'web/locales/en.json'), 'utf8')).strings;
  const node = () => ({children: [], append(...children) { this.children.push(...children); }, setAttribute() {}});
  const saved = {id: 1, brewByWeight: true, fastExtractionGuardEnabled: true,
    slowExtractionGuardEnabled: true, operationalWallMs: 50000, bbwProtectionMs: 12000,
    fastBeforeS: 27, slowAfterS: 34, goalWeightG: 36, fastMarginG: 4, slowMarginG: 3};
  const context = vm.createContext({__WEBUI_TEXT__: key => strings[key],
    document: {createElement: node, createTextNode: text => ({textContent: text})},
    presetState: {activeId: 1, items: [saved], cutRampS: 5}, renderHomePresetAccordion() {}});
  vm.runInContext([runtimeJs.match(/^const sub = \(t, vals\).*$/m)[0],
    ...['axisLabel', 'guardRuleRows', 'buildRuleChartModel',
      'updateRuleChartFromStatus'].map((name) => blockAt(runtimeJs, 'function ' + name + '('))]
      .join('\n'), context);
  // Each row names its time span and what the cut line does there.
  const rules = config => {
    context.status = {config};
    return Array.from(vm.runInContext('updateRuleChartFromStatus(status); guardRuleRows(liveRuleModel)', context),
      row => row.children[0].children[1] + ' · ' + row.children[1].textContent.replace(/\u00a0/g, ' '));
  };
  const home = {...saved}; delete home.bbwProtectionMs;
  assert.deepStrictEqual(rules(home), ['Before 27 s · Fast · up to 40 g',
    '27–34 s · On recipe · 36 g', 'After 34 s · Slow · down to 33 g']);
  assert.deepStrictEqual(rules({...home, goalWeightG: 40, fastMarginG: 7.5, slowMarginG: 2.5,
    fastBeforeS: 30, slowAfterS: 46}), ['Before 30 s · Fast · up to 47.5 g',
    '30–46 s · On recipe · 40 g', 'After 46 s · Slow · down to 37.5 g']);
  assert.strictEqual(rules({...home, fastExtractionGuardEnabled: false})[0], 'Before 27 s · Fast · off');
  assert.deepStrictEqual(rules({...home, brewByWeight: false}),
    ['Fast · off', 'On recipe · off', 'Slow · off']);
}
if (!codeIncludes(ui, 'id="staIpMode"') ||
    !codeIncludes(ui, 'id="staStaticIp"') ||
    !codeIncludes(ui, 'id="staNetmask"') ||
    !codeIncludes(ui, 'id="staGateway"') ||
    !codeIncludes(ui, 'id="staDns1"') ||
    !codeIncludes(ui, 'function networkSavePayload(') ||
    !codeIncludes(ui, "ipMode:$('staIpMode').value") ||
    !codeIncludes(ui, 'staticIpOpt') ||
    !codeIncludes(ui, 'pending confirm') ||
    !codeIncludes(ui, 'savedStaSsid') ||
    !codeIncludes(ui, 'Leave empty to keep the saved password') ||
    !codeIncludes(ui, 'keep=!!savedStaSsid') ||
    !codeIncludes(ui, "savedStaSsid=n.wifiConfigured&&n.ssid?n.ssid:''") ||
    !codeIncludes(ui, 'function formatNetworkStatus(n)') ||
    !codeIncludes(ui, "n.staState==='CONNECTED'&&typeof n.channel==='number'&&n.channel>0") ||
    !codeIncludes(ui, "' — channel '+n.channel") ||
    !codeIncludes(ui, "signalQualityPct") ||
    !codeIncludes(ui, "n.rssi") ||
    !codeIncludes(ui, 'signal ') ||
    !codeIncludes(ui, ' dBm)') ||
    !codeIncludes(ui, "$('networkStatus').textContent=formatNetworkStatus(s.network)") ||
    !codeIncludes(ui, "t('hSsid',n.ssid)") ||
    !codeIncludes(ui, "t('hWifiState',n.staState)") ||
    !codeIncludes(ui, "t('hWifiPs',n.wifiPs)") ||
    !codeIncludes(ui, "t('hWifiCoex',n.wifiCoex)") ||
    !codeIncludes(ui, "$('apStatus').textContent='AP: '+(s.network.apActive?'active':'inactive')") ||
    !codeIncludes(ui, "s.network.apSsid") ||
    !codeIncludes(ui, "n.apSsid") ||
    !codeIncludes(ui, "t('hApState',n.apActive?'active':'inactive'") ||
    !html.includes('<summary>WiFi</summary>') ||
    !html.includes('<strong>Sleep</strong><div id="hWifiPs">') ||
    !html.includes('<strong>Coex</strong><div id="hWifiCoex">') ||
    !html.includes('<strong>SSID</strong><div id="hSsid">') ||
    !html.includes('<summary>AP</summary>') ||
    !network.includes('WiFi.config(') ||
    !network.includes('confirmPendingNetwork') ||
    !network.includes('revertPendingNetwork') ||
    !network.includes('\\"ipMode\\"') ||
    !network.includes('\\"configState\\"') ||
    !network.includes('\\"ssid\\"') ||
    !network.includes('\\"apSsid\\":\\"%s\\"') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightPersistedNetwork.h'), 'utf8')
          .includes('formatSoftApSsid') ||
    !network.includes('fillSoftApSsid(apSsid') ||
    !network.includes('WiFi.softAP(apSsid, settings.devicePassword)') ||
    network.includes('WiFi.softAP(AP_SSID') ||
    !network.includes('\\"rssi\\"') ||
    !network.includes('\\"signalQualityPct\\"') ||
    !network.includes('\\"channel\\"') ||
    !network.includes('wifiRssiToSignalQualityPct') ||
    !network.includes('staLinkMetricsValid') ||
    !network.includes('WiFi.RSSI()') ||
    !network.includes('shouldReuseSavedWifiCredentials') ||
    !network.includes('or empty to keep the saved password.') ||
    !network.includes('StaIpMode::STATIC') ||
    !network.includes('STA_CONFIRM_TIMEOUT_MS') ||
    !network.includes('action must be \\"save\\", \\"forget\\", \\"confirm\\", \\"skip\\", \\"finish\\", \\"brew\\", or \\"discard\\".') ||
    !network.includes('No pending network configuration to confirm.') ||
    // Skip portal exit: setup-context action (no admin unlock, like confirm),
    // action only, gated on the SoftAP being up AND the request coming
    // from the SoftAP socket, arms probe success.
    !network.includes('strcmp(action, "confirm") != 0 && strcmp(action, "skip") != 0') ||
    !network.includes('Skip request must include only action=\\"skip\\".') ||
    !network.includes('No setup network to skip.') ||
    !network.includes('!socketServedBySoftAp(httpd_req_to_sockfd(request))') ||
    !network.includes('self.portalExitRequested_.store(true, std::memory_order_release)') ||
    !network.includes('sendJson(request, STATUS_OK, "{\\"skipped\\":true}")') ||
    // Finish (Done): ends the post-confirm grace early; valid only while the
    // deadline is actually running and the peer is on the SoftAP, arms probe
    // success, lingers before stop.
    !networkHeader.includes('SOFTAP_TEARDOWN_GRACE_MS = 60000') ||
    !networkHeader.includes('SOFTAP_FINISH_LINGER_MS = 3000') ||
    !networkHeader.includes('uint32_t apTeardownAtMs = 0;') ||
    !network.includes('Finish request must include only action=\\"finish\\".') ||
    !network.includes('No setup network to finish.') ||
    !network.includes('status_.apTeardownAtMs = now + SOFTAP_FINISH_LINGER_MS') ||
    !network.includes('sendJson(request, STATUS_OK, "{\\"finished\\":true}")') ||
    !network.includes('status_.apTeardownAtMs = millis() + SOFTAP_TEARDOWN_GRACE_MS') ||
    !network.includes('void OpenBrewByWeightNetwork::serviceSoftApTeardown(') ||
    // Legacy Windows NCSI (www.msftncsi.com) expects its own probe body.
    !network.includes('"Microsoft NCSI"')) {
  throw new Error('DHCP/static IP mode must be wired in UI, status, WiFi.config, and confirm/revert path');
}
if (!network.includes('restoreLkgToActive(next)') ||
    !network.includes('startStation(settings, now)')) {
  throw new Error('STA confirm timeout must reassociate last-known-good before SoftAP fallback');
}
{
  const powerPanelAt = html.indexOf('<details id="powerPanel"');
  const powerPanel = powerPanelAt >= 0
      ? html.slice(powerPanelAt, html.indexOf('</details>', powerPanelAt))
      : '';
  if (      !powerPanel.includes('id="staWifiSleep" type="checkbox" checked') ||
      !powerPanel.includes('Wi-Fi sleep<small') ||
      !powerPanel.includes('Puts the Wi-Fi radio into modem sleep') ||
      !codeIncludes(ui, 'wifiSleep:savedStaWifiSleep') ||
      !codeIncludes(ui, "savedStaWifiSleep=!!n.wifiSleep") ||
      !codeIncludes(ui, "if($('staWifiSleep')&&!powerDirty)$('staWifiSleep').checked=savedStaWifiSleep") ||
      !codeIncludes(ui, "if(n.wifiSleep)t+=' — sleep on'") ||
      !codeIncludes(ui, 'function setWifiSleep(') ||
      !codeIncludes(ui, '_noReconnectWait') ||
      !codeIncludes(ui, 'delete payload._noReconnectWait') ||
      !codeIncludes(ui, 'function updateWifiSleepState(') ||
      !codeIncludes(ui, '!controlsMutable||!savedStaSsid') ||
      !codeIncludes(ui, "R.setWifiSleep()") ||
      !codeIncludes(ui, "sectionDirty(power,") ||
      !codeIncludes(ui, 'if(noReconnect)savedStaWifiSleep=') ||
      !codeIncludes(ui, 'R.resetNetworkAddressLoaded()') ||
      !codeIncludes(ui, 'Wi-Fi sleep saved.') ||
      !network.includes('\\"wifiSleep\\":%s') ||
      !network.includes('jsonHasOnlyUniqueFields(root, saveFields, 13)') ||
      !network.includes('jsonBoolean(root, "wifiSleep", command.network.wifiSleep)') ||
      !network.includes('command.network.wifiSleepSpecified = true') ||
      !network.includes('void OpenBrewByWeightNetwork::applyWifiPowerSave(bool apStarting)') ||
      !network.includes('WIFI_PS_NONE') ||
      !network.includes('WIFI_PS_MIN_MODEM') ||
      /WiFi\.setSleep\(\s*WIFI_PS_MAX_MODEM\s*\)/.test(network) ||
      !network.includes('syncScaleLinkRf') ||
      !network.includes('syncScaleConnectingRf') ||
      !network.includes('syncScaleHuntRf') ||
      !firmware.includes('networkManager.syncScaleLinkRf') ||
      !firmware.includes('networkManager.syncScaleConnectingRf') ||
      !firmware.includes('networkManager.syncScaleHuntRf') ||
      !domainCore.includes('desiredWifiPowerSave') ||
      domainCore.includes('scaleConnectingOrUp') ||
      !domainCore.includes('staAssociated') ||
      !domainCore.includes('otaBusy')) {
    throw new Error(
        'Wi-Fi sleep must be wired in Admin UI, /network save, status, and power-save helper');
  }
  const applyStart = network.indexOf(
      'void OpenBrewByWeightNetwork::applyWifiPowerSave(bool apStarting)');
  const applyEnd = network.indexOf(
      'bool OpenBrewByWeightNetwork::beginStationConnect', applyStart);
  const applyBody = applyStart >= 0 && applyEnd > applyStart
      ? network.slice(applyStart, applyEnd)
      : '';
  if (!applyBody.includes('desiredWifiPowerSave') ||
      !applyBody.includes('OpenBrewByWeightOta::instance().busy()') ||
      !applyBody.includes('WIFI_PS_NONE') ||
      !applyBody.includes('WIFI_PS_MIN_MODEM') ||
      !applyBody.includes('WiFi.setSleep(desiredPs)') ||
      !applyBody.includes('esp_wifi_set_ps(desiredPs)') ||
      !applyBody.includes('apStarting') ||
      !applyBody.includes('WIFI_PS_RETRY_MS') ||
      !applyBody.includes('esp_wifi_get_ps') ||
      !applyBody.includes('mode == WIFI_OFF') ||
      applyBody.includes('WIFI_PS_MAX_MODEM') ||
      applyBody.includes('scaleConnectingOrUp') ||
      applyBody.includes('WiFi.STA.started()') ||
      /WiFi\.setSleep\(desiredPs\)\s*\|\|\s*esp_wifi_set_ps/.test(applyBody) ||
      /WiFi\.mode\(\s*WIFI_OFF\s*\)/.test(applyBody)) {
    throw new Error(
        'applyWifiPowerSave must set NONE/MIN_MODEM via setSleep then fallback set_ps on get_ps miss, honor apStarting, rate-limit same-desired writes, skip if driver off, never WIFI_OFF/MAX_MODEM, and not follow scale link state');
  }
  const saveStart = network.indexOf('case WebCommandType::SAVE_NETWORK:');
  const saveEnd = network.indexOf('case WebCommandType::FORGET_NETWORK', saveStart);
  const saveBody = saveStart >= 0 && saveEnd > saveStart
      ? network.slice(saveStart, saveEnd)
      : '';
  const prefsAt = saveBody.indexOf('sameCredentials &&');
  const copyLkgAt = saveBody.indexOf('copyActiveStaToLkg');
  const prefsOnly = prefsAt >= 0 && copyLkgAt > prefsAt
      ? saveBody.slice(prefsAt, copyLkgAt)
      : '';
  if (!saveBody.includes('applyWifiPsAfterPersist') ||
      saveBody.includes('wifiSleepSpecified && sameCredentials &&') ||
      prefsOnly.includes('restartPending_') ||
      !prefsOnly.includes('break;') ||
      !prefsOnly.includes('next.staWifiSleep != command.network.wifiSleep') ||
      !prefsOnly.includes('strcmp(next.deviceName, command.network.deviceName) != 0') ||
      !prefsOnly.includes('validDeviceName(command.network.deviceName)') ||
      !saveBody.includes('command.network.deviceNameSpecified')) {
    throw new Error(
        'Identical STA credentials must persist sleep and/or device name without restart, and no-op when unchanged');
  }
  const mdnsAddCount = (network.match(/mdns_service_add\(/g) || []).length;
  if (!network.includes('bool OpenBrewByWeightNetwork::ensureMdns()') ||
      !network.includes('mdns_init()') ||
      !network.includes('mdns_hostname_set(host)') ||
      !network.includes('mdns_service_add(instance, "_http", "_tcp", 80, nullptr, 0)') ||
      !network.includes('mdns_service_txt_item_set("_http", "_tcp", "obbw", "1")') ||
      !network.includes('void OpenBrewByWeightNetwork::stopMdns()') ||
      !network.includes('mdns_free()') ||
      !network.includes('ensureMdns();') ||
      !network.includes('applyMdnsName();') ||
      !network.includes('WiFi.setHostname(host)') ||
      !network.includes('\\"deviceName\\":\\"%s\\",\\"mdnsHost\\":\\"%s\\"') ||
      !codeIncludes(ui, 'function networkPreferencesOnly(') ||
      !codeIncludes(ui, "name:$('deviceName')") ||
      !codeIncludes(ui, "savedDeviceName=n.deviceName||''") ||
      !codeIncludes(ui, 'function validDeviceNameClient(') ||
      !codeIncludes(ui, 'id="deviceName"') ||
      network.includes('mdns_query_(') ||
      mdnsAddCount !== 1) {
    throw new Error(
        'mDNS must advertise hostname and _http._tcp with the obbw identification TXT from the persisted device name, stay network-task owned, never query, and never register a second service');
  }
}
if (!firmwareCore.includes('command.network.commitConfirmed = true') ||
    !network.includes('finalizeSavedStaCredentials(next, command.network.commitConfirmed)')) {
  throw new Error(
      'USB SET_WIFI must commit STA credentials; Web UI keeps the confirm window');
}
{
  if (network.includes('OpenBrewByWeightNetwork::loginHandler') ||
      network.includes('OpenBrewByWeightNetwork::logoutHandler') ||
      network.includes('loginRateLimited') ||
      network.includes('recordFailedLoginAttempt')) {
    throw new Error('Login handlers and rate limits must be removed; WebUI claim owns the session');
  }
}
{
  const oldParseStart = bleLibrary.indexOf('parseAcaiaOldWeight');
  const oldParseEnd = bleLibrary.indexOf('parseAcaiaOldTimer', oldParseStart);
  const oldParse = oldParseStart >= 0 && oldParseEnd > oldParseStart
      ? bleLibrary.slice(oldParseStart, oldParseEnd)
      : '';
  if (!oldParse.includes('length == 14') ||
      !oldParse.includes('scaleValidAcaiaChecksum')) {
    throw new Error('OLD 14-byte Acaia frames must validate checksum');
  }
}
{
  const startCmd = firmware.slice(
      firmware.indexOf('void executeScaleStartCommand'),
      firmware.indexOf('void executeScaleStopCommand'));
  if (!startCmd.includes('tareStartTimer(&admission)') ||
      !startCmd.includes('resetTimer(&admission)') ||
      !startCmd.includes('if (!event.writeSucceeded && allowSeparateStart)') ||
      !startCmd.includes('allowSeparateStart = result == ScaleCommandResult::Unsupported')) {
    throw new Error('Combined tare/start may fall back only when unsupported, never after uncertain writes');
  }
}
if (!/<script\s+type="module"\s+src="\/app\.js\?v=/.test(shellHtml) &&
    !shellHtml.includes('src="/app.js?v=__FW_ASSET_TAG__"')) {
  throw new Error('Web UI must load same-origin /app.js with a firmware version query');
}
if (!/<link\s+rel="stylesheet"\s+href="\/app\.css\?v=/.test(shellHtml) &&
    !shellHtml.includes('href="/app.css?v=__FW_ASSET_TAG__"')) {
  throw new Error('Web UI must load same-origin /app.css with a firmware version query');
}
if (!shellHtml.includes('name="color-scheme" content="light dark"')) {
  throw new Error('Web UI must declare color-scheme so Safari form controls follow dark mode');
}
if (shellHtml.includes('class="themeSel"') ||
    css.includes('.themeSel{') ||
    css.includes('.themeOpt') ||
    !css.includes('html.theme-light{') ||
    !css.includes('html.theme-dark{') ||
    !codeIncludes(runtimeJs, "THEME_KEY='obbwTh'") ||
    !codeIncludes(runtimeJs, "THEME_MODES=['auto','light','dark']") ||
    !codeIncludes(runtimeJs, "getElementById('uiTheme')") ||
    !codeIncludes(runtimeJs, 'localStorage.getItem(THEME_KEY)') ||
    !codeIncludes(runtimeJs, 'localStorage.setItem(THEME_KEY,m)') ||
    !codeIncludes(runtimeJs, "classList.toggle('theme-dark'") ||
    !codeIncludes(runtimeJs, "classList.toggle('theme-light'") ||
    !codeIncludes(appJsSource, 'R.paintTheme(R.themeMode())') ||
    !codeIncludes(appJsSource, 'R.paintTheme(e.target.value)') ||
    !html.includes('id="frontendPanel"') ||
    !html.includes('<summary>Frontend</summary>') ||
    !html.includes('id="uiTheme"') ||
    !html.includes('<option value="auto" selected>Auto</option>') ||
    !html.includes('<option value="light">Light</option>') ||
    !html.includes('<option value="dark">Dark</option>') ||
    html.indexOf('id="dateTimePanel"') > html.indexOf('id="frontendPanel"') ||
    html.indexOf('id="frontendPanel"') > html.indexOf('id="accessPanel"')) {
  throw new Error('Theme must be an Admin Frontend dropdown (Auto/Light/Dark), not a Home selector');
}
if (/<link\s+[^>]*href=["']https?:\/\//i.test(shellHtml) ||
    /cdn\.|unpkg\.|jsdelivr\./i.test(shellHtml)) {
  throw new Error('Web UI must not depend on CDN or third-party assets');
}

const expected = new Map([
  ['GET /', 'rootHandler'],
  ['GET /diagnostic', 'rootHandler'],
  ['GET /log', 'rootHandler'],
  ['GET /stats', 'rootHandler'],
  ['GET /admin', 'rootHandler'],
  ['GET /settings', 'rootHandler'],
  ...webUi.ASSET_PATHS.map((path) => ['GET ' + path, 'assetHandler']),
  ['POST /api/v1/ui/claim', 'claimHandler'],
  ['GET /api/v1/status/admin', 'ownedApiHandler'],
  ['GET /api/v1/debug/export', 'ownedApiHandler'],
  ['POST /api/v1/config', 'ownedApiHandler'],
  ['POST /api/v1/scale/preferred/clear', 'ownedApiHandler'],
  ['POST /api/v1/scale/preferred/select', 'ownedApiHandler'],
  ['POST /api/v1/presets', 'ownedApiHandler'],
  ['POST /api/v1/calibration/reset', 'ownedApiHandler'],
  ['POST /api/v1/calibration/reset-guard-samples', 'ownedApiHandler'],
  ['POST /api/v1/control/paddle', 'ownedApiHandler'],
  ['POST /api/v1/control/rinse', 'ownedApiHandler'],
  ['POST /api/v1/control/stop', 'ownedApiHandler'],
  ['POST /api/v1/control/state-override', 'ownedApiHandler'],
  ['POST /api/v1/control/restart', 'ownedApiHandler'],
  ['POST /api/v1/diagnostic/reset-history', 'ownedApiHandler'],
  ['POST /api/v1/factory-reset', 'ownedApiHandler'],
  ['POST /api/v1/stats/clear', 'ownedApiHandler'],
  ['POST /api/v1/stats/delete', 'ownedApiHandler'],
  ['POST /api/v1/stats/rate', 'ownedApiHandler'],
  ['POST /api/v1/last-shot/clear', 'ownedApiHandler'],
  ['POST /api/v1/time/sync', 'ownedApiHandler'],
  ['POST /api/v1/webhooks', 'ownedApiHandler'],
  ['POST /api/v1/network', 'ownedApiHandler'],
  ['POST /api/v1/network/scan', 'ownedApiHandler'],
  ['GET /api/v1/network/scan', 'ownedApiHandler'],
  ['POST /api/v1/device/password', 'ownedApiHandler'],
  ['POST /api/v1/admin/unlock', 'ownedApiHandler'],
  ['POST /api/v1/admin/lock', 'ownedApiHandler'],
  ['POST /api/v1/diagnostic/profiler', 'ownedApiHandler'],
  ['POST /api/v1/diagnostic/scale-profile', 'ownedApiHandler'],
  ['GET /api/v1/diagnostic/scale-profile/download', 'ownedApiHandler'],
  ['POST /api/v1/diagnostic/loop-max/reset', 'ownedApiHandler'],
  ['POST /api/v1/diagnostic/scale-gap-max/reset', 'ownedApiHandler'],
  // OTA authenticates with the device password instead of the exclusive
  // WebUI claim, so a command line client can update firmware without stealing
  // control from an open browser window.
  ['GET /api/v1/ota', 'otaStatusHandler'],
  ['POST /api/v1/ota', 'otaUploadHandler'],
  ['POST /api/v1/ota/flash', 'otaFlashHandler'],
  ['POST /api/v1/ota/abort', 'otaAbortHandler'],
]);

const maxSocketsMatch = network.match(/max_open_sockets\s*=\s*(\d+)/);
if (!maxSocketsMatch || Number(maxSocketsMatch[1]) !== 7) {
  throw new Error('HTTP server must reserve exactly 7 open sockets: WebUI stream and handshake, two reads, Home Assistant, and an OTA range plus its poll');
}
const lwipSocketsMatch = fs.readFileSync(path.join(sketchDir, '..', 'idf', 'sdkconfig.defaults'), 'utf8')
  .match(/^CONFIG_LWIP_MAX_SOCKETS=(\d+)$/m);
// httpd uses max_open_sockets + 3; mDNS, SNTP, DNS, webhook, Micra WSS and
// HTTPS need six more, and two stay spare.
if (!lwipSocketsMatch || Number(lwipSocketsMatch[1]) < Number(maxSocketsMatch[1]) + 3 + 6 + 2) {
  throw new Error('CONFIG_LWIP_MAX_SOCKETS must cover httpd, every outbound client, and two spare sockets');
}
if (!network.includes('backlog_conn = 4')) {
  throw new Error('HTTP server must queue the four-request asset/read burst without increasing its socket budget');
}
if (!network.includes('/api/v1/ui/claim') ||
    !network.includes('X-WebUI-Client') ||
    !network.includes('requireActiveWebUiClient') ||
    !network.includes('UI_CLAIM_REQUIRED') ||
    !network.includes('UI_TAKEN_OVER')) {
  throw new Error('WebUI APIs must enforce the exclusive client claim');
}

if (!webhookSource.includes('QueuedWebhook') ||
    !webhookSource.includes('queued.configGeneration != generation') ||
    !webhookSource.includes('++status_.staleConfigDropped')) {
  throw new Error('Queued webhooks must be discarded after their configuration changes');
}
if (!webhookSource.includes('xSemaphoreTake(lifecycleMutex_, 0)') ||
    !webhookSource.includes('WorkerState::STOPPING') ||
    !webhookSource.includes('releaseWorkerFromTask()')) {
  throw new Error('Webhook dispatch must remain non-blocking and release disabled worker resources');
}
if ((webhookSource.match(/"Content-Type", "application\/json"/g) || []).length !== 1 ||
    (webhookSource.match(/"User-Agent", "OpenBrewByWeight\/1"/g) || []).length !== 1 ||
    webhookSource.indexOf('esp_http_client_set_header(client, "Content-Type"') >
        webhookSource.indexOf('httpClient_.reset(client)')) {
  throw new Error('Webhook constant headers must be installed once before client ownership');
}
const networkWebhookBegin = network.indexOf('webhooks_.begin(settings.webhook)');
const networkPassiveInit = [
  network.indexOf('xQueueCreate(WEB_COMMAND_QUEUE_LENGTH'),
  network.indexOf('xSemaphoreCreateMutex()'),
  network.indexOf('allocExternal(sizeof(NetworkWorkBuf), AllocationOwner::NETWORK)'),
];
if (networkWebhookBegin < 0 ||
    networkPassiveInit.some(index => index < 0 || index > networkWebhookBegin) ||
    !network.includes('bool OpenBrewByWeightNetwork::stop()') ||
    !network.includes('xSemaphoreTake(taskStopped_') ||
    !network.includes('if (!webhooks_.stop()) return false;') ||
    !webhookSource.includes('bool WebhookDispatcher::stop()') ||
    !webhookSource.includes('xSemaphoreTake(workerStopped_')) {
  throw new Error(
      'Network/Webhook initialization must acquire passive resources first and provide joined rollback');
}
if (!webhookSource.includes('allocInternal(queueStorageBytes, AllocationOwner::WEBHOOK)') ||
    !webhookSource.includes('allocExternal(kWebhookPayloadCapacity, AllocationOwner::WEBHOOK)') ||
    !webhookSource.includes('xQueueCreateStatic') ||
    !webhookSource.includes('tskIDLE_PRIORITY') ||
    !webhookSource.includes('dispatchAllowed()') ||
    !webhookSource.includes('setControlCritical') ||
    !webhookSource.includes('bool haveQueued = false') ||
    !webhookSource.includes('if (haveQueued && dispatchAllowed() &&') ||
    !webhookSource.includes('ensureHttpClient(live.url)') ||
    !webhookSource.includes('sampleHeapCaps()') ||
    !webhookSource.includes('webhookClientMustRecreate') ||
    !webhookSource.includes('++status_.clientReuses') ||
    !webhookSource.includes('++status_.transportResets')) {
  throw new Error(
      'Webhook queue must be internal and payload external; delivery must recheck the explicit shot-deferral gate before HTTP');
}
if (webhookSource.includes('esp_http_client_cancel_request') ||
    webhookSource.includes('cancelActive_') ||
    webhookSource.includes('void WebhookDispatcher::serviceAbort()') ||
    network.includes('webhooks_.serviceAbort()') ||
    network.includes('webhooks_.setScaleConnecting(')) {
  throw new Error(
      'Webhook delivery must never be cancelled or gated by scale connection activity');
}
if (!webhookHeader.includes('UniqueResource<esp_http_client_handle_t') ||
    webhookHeader.includes('void *httpClient_') ||
    webhookSource.includes('else requestWorkerStop();') ||
    !webhookSource.includes('++status_.workerStarts') ||
    !webhookSource.includes('++status_.heapSamples')) {
  throw new Error(
      'Webhook worker/client ownership must remain stable and publish per-operation heap evidence');
}
if (!webhookSource.includes('\\"sentAtUptimeMs\\"') ||
    !firmwareCore.includes('webhookUnixSecAt(uint32_t occurredAtMs)') ||
    !firmwareCore.includes('baseWebhookEvent(WebhookEventType::END, snapshot.cycleId,\n                                        snapshot.endedAtMs)')) {
  throw new Error('Webhook payloads must preserve occurrence time separately from delivery time');
}
if (!firmwareCore.includes('if (brewEndIsAbandonedStart(reason) || endingRinseCycle(reason)) {\n    webhookBrewStartPending = false;') ||
    !firmwareCore.includes('!endingRinseCycle(reason) && !brewEndIsAbandonedStart(reason)')) {
  throw new Error('Abandoned starts and rinses must not emit misleading brew-state webhooks');
}
if (!codeIncludes(ui, 'function claimWebUiOwnership(claimed)') ||
    !codeIncludes(ui, 'function deactivateWebUi()') ||
    !codeIncludes(ui, 'function showInactiveOverlay()') ||
    !codeIncludes(ui, 'function hideInactiveOverlay()') ||
    !codeIncludes(ui, 'id="webUiReload"') ||
    !codeIncludes(ui, '>Reload<') ||
    !codeIncludes(ui, 'X-WebUI-Client')) {
  throw new Error('Inactive WebUI windows must become passive and offer Reload');
}
if (!codeIncludes(ui, 'const WEB_UI_INACTIVITY_MS=15*60*1000') ||
    !codeIncludes(ui, 'function resetWebUiInactivity()') ||
    !codeIncludes(ui, 'function webUiPollingActive()') ||
    !codeIncludes(ui, 'function noteWebUiInteraction(event)') ||
    !(codeIncludes(ui, "document.addEventListener('pointerdown',noteWebUiInteraction,true)") ||
      codeIncludes(ui, "document.addEventListener('pointerdown', R.noteWebUiInteraction, true)") ||
      codeIncludes(ui, "document.addEventListener('pointerdown',R.noteWebUiInteraction,true)")) ||
    !(codeIncludes(ui, "document.addEventListener('keydown',noteWebUiInteraction,true)") ||
      codeIncludes(ui, "document.addEventListener('keydown', R.noteWebUiInteraction, true)") ||
      codeIncludes(ui, "document.addEventListener('keydown',R.noteWebUiInteraction,true)")) ||
    codeIncludes(ui, "addEventListener('scroll',noteWebUiInteraction") ||
    codeIncludes(ui, "addEventListener('scroll', R.noteWebUiInteraction")) {
  throw new Error('WebUI inactivity must expire after 15 minutes of direct control interaction, never scrolling');
}
if (!codeIncludes(ui, 'Press Reload to enable this window again.') ||
    !codeIncludes(ui, 'id="webUiInactive"') ||
    !codeIncludes(ui, 'id="inactiveHint"') ||
    !codeIncludes(ui, 'id="inactiveError"') ||
    !css.includes('.inactiveOverlay') ||
    !css.includes('.inactiveOverlay.isVisible') ||
    !css.includes('opacity .5s') ||
    !network.includes('This WebUI window is inactive. Reactivate to continue.') ||
    codeIncludes(ui, 'Another WebUI window controls this device.') ||
    network.includes('Another WebUI window has taken control.') ||
    codeIncludes(ui, 'function ownershipBanner(') ||
    codeIncludes(ui, 'webUiOwnership') ||
    codeIncludes(ui, 'btnTakeControl') ||
    codeIncludes(ui, 'Reactivate')) {
  throw new Error('WebUI inactive notice must be a full-screen Reload overlay');
}
if (!codeIncludes(ui, "w.id='reconnectWait'") ||
    !codeIncludes(ui, "w.className='reconnectRing'") ||
    !codeIncludes(ui, 'reconnectSeconds') ||
    !codeIncludes(ui, 'function beginNetworkReconnectWait()') ||
    !codeIncludes(ui, 'function endNetworkReconnectWait()') ||
    !codeIncludes(ui, 'function pollNetworkReconnect()') ||
    !codeIncludes(ui, 'NETWORK_RECONNECT_WAIT_MS=180000') ||
    !codeIncludes(ui, 'setTimeout(pollNetworkReconnect,2e3)') ||
    !codeIncludes(ui, 'setInterval(updateReconnectCountdown,1e3)') ||
    !codeIncludes(ui, 'rec?4e3:8e3') ||
    !codeIncludes(ui, "value.action==='save'") ||
    !codeIncludes(ui, 'beginNetworkReconnectWait()') ||
    !codeIncludes(ui, 'claimWebUiOwnership()') ||
    !codeIncludes(ui, 'Waiting for the controller on this address.') ||
    !codeIncludes(ui, 'the previous network should return shortly.') ||
    !css.includes('.inactiveOverlay.isReconnectWait') ||
    !css.includes('.reconnectRing') ||
    !css.includes('conic-gradient') ||
    codeIncludes(ui, 'function heartbeat(') ||
    codeIncludes(ui, '/api/v1/heartbeat')) {
  throw new Error(
      'Wi-Fi save must show a 180s reconnect overlay that polls ui/claim even after 0s, without a heartbeat endpoint');
}
if (!codeIncludes(ui, 'clearTimeout(webUiInactivityTimer)') ||
    !codeIncludes(ui, 'clearTimeout(scanTimer)') ||
    !codeIncludes(ui, 'stopViewPolls()') ||
    !codeIncludes(ui, 'if(!webUiPollingActive())throw new Error')) {
  throw new Error('WebUI inactivity must cancel poll timers and block further API calls');
}
if (!codeIncludes(ui, 'setMutable(!!s.configMutable||!!s.webUiOverrideActive)') ||
    !codeIncludes(ui, 'webUiOverrideActive') ||
    !codeIncludes(ui, 'webUiOverrideRemainingMs') ||
    !codeIncludes(ui, "uiOverridePanel") ||
    !codeIncludes(ui, "uiOverrideButton") ||
    !codeIncludes(ui, 'UI Override') ||
    !codeIncludes(ui, "closest('#adminLockPanel,#uiOverridePanel')") ||
    !codeIncludes(ui, 'function ensureUiOverridePanel(') ||
    !codeIncludes(ui, 'if(developmentMode||$(\'uiOverridePanel\'))return;') ||
    !codeIncludes(ui, '/api/v1/ui/unlock') ||
    !codeIncludes(ui, 'UNSAFE_WEBUI_OVERRIDE') ||
    !network.includes('/api/v1/ui/unlock') ||
    !network.includes('UNSAFE_WEBUI_OVERRIDE') ||
    !network.includes('WEB_UI_OVERRIDE_MS') ||
    !network.includes('webUiOverrideUntilMs_') ||
    !network.includes('webUiOverrideRemainingMs') ||
    !network.includes('requireAdminUnlock(request)') ||
    network.includes('clearWebUiOverrideIfSafe')) {
  throw new Error('Web UI must honor configMutable/webUiOverrideActive with a timed admin-gated UI Override');
}
if (codeIncludes(ui, 'configLockBanner') || css.includes('configLockBanner') ||
    codeIncludes(ui, 'ensureConfigLockBanner') || codeIncludes(ui, 'renderConfigLockBanner') ||
    codeIncludes(ui, 'Controls locked:') || codeIncludes(ui, 'Unsafe WebUI override active')) {
  throw new Error('Web UI must not render the configuration-lock warning banner');
}
if (network.includes('return "safety_recovery"') ||
    network.includes('return "safety_lockout"') ||
    codeIncludes(ui, "safety_recovery:'safety recovery'") ||
    codeIncludes(ui, "safety_lockout:'safety lockout'")) {
  throw new Error('Safety recovery must not lock the WebUI');
}
if (!codeIncludes(ui, "relayStartReady=s.safety.state!=='LOCKOUT'") ||
    !codeIncludes(ui, 'admin&&remoteReady&&relayStartReady&&canControl') ||
    !codeIncludes(ui, "live?'Stop shot':'Start shot'") ||
    !codeIncludes(ui, "dataset.mode==='stop'") ||
    !codeIncludes(ui, "if(shot.dataset.mode==='stop'&&!live)shot.dataset.stoppedAt=Date.now()") ||
    !codeIncludes(ui, "Date.now()-(stopButton.dataset.stoppedAt||0)>2e3&&R.command('/api/v1/control/paddle'") ||
    !codeIncludes(ui, "/api/v1/control/paddle") ||
    !codeIncludes(ui, "/api/v1/control/stop") ||
    !codeIncludes(ui, 'show=!!unlocked&&!!remoteEnabled') ||
    !codeIncludes(ui, 'syncAdminSessionUi(admin,remoteReady)') ||
    !codeIncludes(ui, "id=\"actionsPanel\" class=\"hidden\"") ||
    !codeIncludes(ui, 'shot.disabled=!admin||(!live&&!(remoteReady&&relayStartReady&&canControl))')) {
  throw new Error('Circuit actions must require remote policy and Admin unlock, preserve Stop semantics, and ignore Start for 2 s after a stop');
}
if (!codeIncludes(ui, 'id="forcePulseButton"') ||
    !codeIncludes(ui, 'class="btnGlyph btnWarn momentaryOnly"') ||
    !codeIncludes(ui, 'Force switch press') ||
    !codeIncludes(ui, "R.command('/api/v1/control/force-pulse')") ||
    !codeIncludes(js, "'control/force-pulse':['Switch pulse sent.','send switch pulse'") ||
    !codeIncludes(runtimeJs, "force.disabled=!(admin&&remoteReady&&relayStartReady&&webUiOwner)") ||
    !css.includes('color:var(--ac);min-height:3rem;min-width:4.5rem;padding:.6rem .85rem') ||
    // Action bar buttons grow through the base .btnGlyph flex:1; the pulse stays narrower.
    !/\.btnGlyph\{display:inline-flex[^}]*;flex:1\}/.test(css) ||
    !css.includes('#actionsPanel .btnGlyph{border:1.5px solid') ||
    !css.includes('#actionsPanel .momentaryOnly{flex:.6;min-height:var(--tap);min-width:4.5rem}') ||
    !network.includes('"/api/v1/control/force-pulse"') ||
    !network.includes('OpenBrewByWeightNetwork::forcePulseHandler') ||
    !network.includes('WebCommandType::FORCE_SWITCH_PULSE') ||
    !network.includes('requireAdminUnlock(request)') ||
    !domain.includes('FORCE_SWITCH_PULSE') ||
    !firmware.includes('machineRequestForcedPulse()') ||
    !firmwareCore.includes('machineRequestWebStop()')) {
  throw new Error('Momentary Web controls must expose an admin-gated forced pulse and a dedicated Web STOP path');
}if (network.includes('/api/v1/status/home') ||
    network.includes('/api/v1/status/diagnostic') ||
    !network.includes('/api/v1/status/admin')) {
  throw new Error('Status API must expose only /api/v1/status/admin; Home, Settings and Diagnostic are WebSocket-only');
}

// Development builds compile out the unlock endpoints: administration is
// public there, the UI never calls them, and leaving them reachable would be
// dead attack surface. Release keeps the full password unlock flow.
{
  const unlockEndpoints = ['/api/v1/admin/unlock', '/api/v1/admin/lock', '/api/v1/ui/unlock'];
  for (const endpoint of unlockEndpoints) {
    if (!network.includes('"' + endpoint + '"') && !network.includes("'" + endpoint + "'")) {
      throw new Error(endpoint + ' route string vanished from the firmware sources entirely');
    }
  }
  const devGuards = network.split('#if OPEN_BREW_BY_WEIGHT_DEVELOPMENT == 1').length - 1;
  const releaseElse = network.split('#else').length - 1;
  if (devGuards < 4 || releaseElse < 3) {
    throw new Error('Unlock routes and handlers must be release-only via OPEN_BREW_BY_WEIGHT_DEVELOPMENT guards');
  }
  // Exactly one /api/v1/ui/unlock registration may exist in the sources; the
  // dev branch of its guard keeps the &&-chain shape intact.
  const uiUnlockRegistrations =
      (network.match(/registerHandler\(server_, "\/api\/v1\/ui\/unlock"/g) || []).length;
  if (uiUnlockRegistrations !== 1) {
    throw new Error('Exactly one /api/v1/ui/unlock registration must exist (release branch)');
  }
  // The dev branch keeps the server-start &&-chain valid with a plain true;
  // if someone removes the guard entirely, registration becomes unguarded.
  if (!network.includes('// Unlock routes are not served in development builds')) {
    throw new Error('Unlock route guards must carry the dev-branch rationale comment');
  }
}

if (!network.includes('statusResponseMux_') ||
    !network.includes('STATUS_BUSY')) {
  throw new Error(
      'Status handler must serialize the shared status buffers with a mutex');
}
if (!network.includes('"Connection"') || !network.includes('"close"')) {
  throw new Error(
      'API JSON responses must send Connection: close to avoid keep-alive socket pinning');
}
const sendJsonStart = network.indexOf('esp_err_t OpenBrewByWeightNetwork::sendJson(');
const sendJsonBody = network.slice(sendJsonStart, network.indexOf('\n}\n', sendJsonStart));
if (sendJsonBody.indexOf('httpd_sess_trigger_close(') < sendJsonBody.indexOf('sendBody(')) {
  throw new Error('sendJson must close the session after the body so the FIN flushes the Nagle-held tail');
}
const otaSnapshotStart = network.indexOf('esp_err_t OpenBrewByWeightNetwork::sendOtaSnapshot(');
const otaSnapshotBody = network.slice(otaSnapshotStart, network.indexOf('\n}\n', otaSnapshotStart));
if (!otaSnapshotBody.includes('AllocationOwner::OTA') ||
    otaSnapshotBody.indexOf('if (reply != nullptr) unlockWorkBuf();') >
        otaSnapshotBody.indexOf('sendJson(')) {
  throw new Error('sendOtaSnapshot must release the shared status lock before sending its private reply');
}
if (!network.includes('recv_wait_timeout = 5') ||
    !network.includes('send_wait_timeout = 5')) {
  throw new Error(
      'HTTP recv/send wait timeouts must stay short so LRU can free stalled sockets');
}
const maxReqHdrMatch = network.match(/max_req_hdr_len\s*=\s*(\d+)/);
if (!maxReqHdrMatch || Number(maxReqHdrMatch[1]) < 2048) {
  throw new Error(
      'HTTP server must allow at least 2048 request header bytes (Safari UA/Cookie 431)');
}
const maxRespHeadersMatch = network.match(/max_resp_headers\s*=\s*(\d+)/);
if (!maxRespHeadersMatch || Number(maxRespHeadersMatch[1]) < 12) {
  throw new Error('HTTP server must allow at least 12 response headers for gzip and ETag');
}
if (!codeIncludes(ui, 'function withPollGate(') ||
    !codeIncludes(ui, 'function withCommandGate(') ||
    !codeIncludes(ui, 'function armStatusTimer(') ||
    !codeIncludes(ui, 'function statusPollDue(') ||
    !codeIncludes(ui, 'commandBusy') ||
    !codeIncludes(ui, 'visibilitychange') ||
    !codeIncludes(ui, 'await refreshStatus()') ||
    !codeIncludes(ui, 'noteReachFail(') ||
    !codeIncludes(ui, 'function startView(') ||
    !codeIncludes(ui, 'function stopViewPolls(') ||
    !codeIncludes(ui, 'function renderRoute(') ||
    !codeIncludes(ui, 'armStatusTimer()') ||
    !codeIncludes(ui, 'AbortController') ||
    !codeIncludes(ui, 'Device timeout') ||
    !codeIncludes(ui, "new Error('Invalid response')") ||
    !codeIncludes(ui, "throw new Error('Invalid status')") ||
    !codeIncludes(ui, "await loadSettingsStatus():await api('/api/v1/status/admin')") ||
    !codeIncludes(ui, 'function ensureSettingsHydrated(') ||
    !codeIncludes(ui, 'function withBaseRev(') ||
    !codeIncludes(ui, 'formRev') ||
    !codeIncludes(ui, 'restoreSnapshot(edited)') ||
    !codeIncludes(ui, 'baseRevision') ||
    !codeIncludes(ui, 'command(path,value={},soft,okMsg,failMsg,busyId)') ||
    !codeIncludes(ui, '/api/v1/status/') ||
    !codeIncludes(ui, 'function statusPageOk(') ||
    !codeIncludes(ui, "new Error('Invalid response')") ||
