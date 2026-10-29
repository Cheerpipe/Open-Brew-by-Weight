'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const crypto = require('crypto');
// Contracts compare many Web UI variants: each distinct set of options is
// generated once per run. Injected sources bypass the cache.
const webUi = {...require('../../scripts/gen_web_ui.js')};
const generateUncached = webUi.generate;
const generatedVariants = new Map();
webUi.generate = (options = {}) => {
  if (options.readSource) return generateUncached(options);
  const key = JSON.stringify(options);
  if (!generatedVariants.has(key)) {
    generatedVariants.set(key, generateUncached(options).catch((error) => {
      generatedVariants.delete(key);
      throw error;
    }));
  }
  return generatedVariants.get(key);
};
const webUiLocale = require('../../scripts/localize_web_ui.js');
const imageTag = require('../../scripts/image_tag.js');

const sketchDir = path.resolve(__dirname, '..');
function readSources(files) {
  return files.map((file) => fs.readFileSync(path.join(sketchDir, file), 'utf8'))
    .join('\n');
}
// The size-budget step reads the same compact, localized authoring sources.
const authoring = await require('./check_web_budgets.js').authoringSources();
const rawShellHtml = authoring.raw.shellHtml;
const network = readSources([
  'OpenBrewByWeightNetwork.cpp',
  'network/OpenBrewByWeightNetworkService.inc',
  'network/OpenBrewByWeightWifi.inc',
  'network/OpenBrewByWeightHttpLifecycle.inc',
  'network/OpenBrewByWeightHttpAuthAssets.inc',
  'network/OpenBrewByWeightWebAccessHttp.inc',
  'network/OpenBrewByWeightStatus.inc',
  'network/OpenBrewByWeightUiStream.inc',
  'network/OpenBrewByWeightUiStreamTransport.inc',
  'network/OpenBrewByWeightHomeStream.inc',
  'network/OpenBrewByWeightStatsPage.inc',
  'network/OpenBrewByWeightAuditStream.inc',
  'network/OpenBrewByWeightSettingsStream.inc',
  'network/OpenBrewByWeightIntegrationApi.inc',
  'diagnostics/OpenBrewByWeightNetworkDiagnostics.inc',
  'diagnostics/OpenBrewByWeightShotTraceRoutes.inc',
  'network/OpenBrewByWeightConfiguration.inc',
  'network/OpenBrewByWeightLineaMicraWeb.inc',
  'network/OpenBrewByWeightNetworkOta.inc',
]);
const networkHeader = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightNetwork.h'), 'utf8');
const otaSource = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightOta.cpp'), 'utf8');
const otaHeader = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightOta.h'), 'utf8');
const webhookSource = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightWebhook.cpp'), 'utf8');
const webhookHeader = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightWebhook.h'), 'utf8');
const firmwareCore = readSources([
  'openBrewByWeight.cpp',
  'control/OpenBrewByWeightCycleRuntime.inc',
  'control/OpenBrewByWeightCycleFinalize.inc',
  'control/OpenBrewByWeightIdleTare.inc',
  'control/OpenBrewByWeightBbwFinalize.inc',
  'scale/OpenBrewByWeightScaleEvents.inc',
  'control/OpenBrewByWeightControlStateMachine.inc',
  'persistence/OpenBrewByWeightCommandPersistence.inc',
  'control/OpenBrewByWeightCommands.inc',
  'diagnostics/OpenBrewByWeightSerialOutput.inc',
  'diagnostics/OpenBrewByWeightSerialLog.inc',
  'diagnostics/OpenBrewByWeightDiagnostics.inc',
  'platform/OpenBrewByWeightEntrypoints.inc',
]);
const idfMain = fs.readFileSync(
  path.resolve(sketchDir, '..', 'idf', 'main', 'main.cpp'), 'utf8');
const scaleWorker = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightScaleWorker.cpp'), 'utf8');
const firmware = [
  firmwareCore,
  scaleWorker,
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightScaleWorker.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightScaleLink.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightHardware.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachine.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightRfCoex.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineRelay.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineActivatorSample.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddleInput.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddleControl.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddleState.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddlePolicy.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddleConfig.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryInput.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryConfig.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryControl.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryReedState.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryOnlyState.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightScaleSense.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightCupPresence.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrew.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightRinse.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightShotCurveTypes.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightAlert.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightAlertChannel.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightAlertTone.h'), 'utf8'),
].join('\n');
const activationStoresIo = fs.readFileSync(
    path.join(sketchDir, 'OpenBrewByWeightActivationStores.h'), 'utf8');
const shotLogIo = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightShotLog.h'), 'utf8');
const shotCurveIo = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightShotCurve.h'), 'utf8');
const dualSlotFlashIo = fs.readFileSync(
    path.join(sketchDir, 'OpenBrewByWeightDualSlotFlashLog.h'), 'utf8');
const lastShotIo = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightLastShot.h'), 'utf8');
const jsonArena = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightJsonArena.h'), 'utf8');
const wallClock = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightTime.h'), 'utf8');
const domainCore = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightDomain.h'), 'utf8');
const domain = [
  domainCore,
  fs.readFileSync(path.join(sketchDir, 'domain/OpenBrewByWeightControlStatusTypes.inc'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineTypes.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightScaleTypes.h'), 'utf8'),
  fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrewTypes.h'), 'utf8'),
].join('\n');
const serialCli = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightSerialCli.h'), 'utf8');
const buzzer = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBuzzer.h'), 'utf8');
const buzzerPatterns = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBuzzerPatterns.h'), 'utf8');
const buzzerPassive = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBuzzerPassive.h'), 'utf8');
const rtttlParser = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightRtttl.h'), 'utf8');
const buzzerRtttl = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBuzzerRtttl.h'), 'utf8');
const alertChannel = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightAlertChannel.h'), 'utf8');
const alertTone = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightAlertTone.h'), 'utf8');
const kconfig = fs.readFileSync(
  path.resolve(sketchDir, '..', 'idf', 'main', 'Kconfig.projbuild'), 'utf8');
const bleSrcDir = path.resolve(sketchDir, '..', 'libraries', 'EspressoScaleBLE', 'src');
function readTree(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).map((entry) => {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      return readTree(full);
    }
    if (/\.(cpp|h)$/.test(entry.name)) {
      return fs.readFileSync(full, 'utf8');
    }
    return '';
  }).join('\n');
}
const bleLibrary = readTree(bleSrcDir);
const companionSources = [
  'OpenBrewByWeightBleCompanion.h',
  'OpenBrewByWeightBleCompanionPersistence.h',
  path.join('ble', 'OpenBrewByWeightBleCompanionNimble.cpp'),
  path.join('ble', 'OpenBrewByWeightBleCompanionNimble.h'),
  path.join('ble', 'OpenBrewByWeightBleCompanionProtocol.h'),
  path.join('ble', 'OpenBrewByWeightBleRadioPolicy.h'),
  path.join('tests', 'ble_companion_protocol_host_test.cpp'),
];
for (const rel of companionSources) {
  if (fs.existsSync(path.join(sketchDir, rel))) {
    throw new Error('BLE Companion source must be absent: ' + rel);
  }
}
const taskProfiler = fs.readFileSync(
  path.join(sketchDir, 'OpenBrewByWeightTaskProfiler.h'), 'utf8');
const sdkconfigDefaults = fs.readFileSync(
  path.resolve(sketchDir, '..', 'idf', 'sdkconfig.defaults'), 'utf8');
const sdkconfigMicra = fs.readFileSync(
  path.resolve(sketchDir, '..', 'idf', 'sdkconfig.defaults.micra'), 'utf8');
const sdkconfigNimble = fs.readFileSync(
  path.resolve(sketchDir, '..', 'idf', 'sdkconfig.defaults.nimble'), 'utf8');
const bleComponentCmake = fs.readFileSync(
  path.resolve(sketchDir, '..', 'idf', 'components', 'EspressoScaleBLE',
               'CMakeLists.txt'), 'utf8');
const idfBuildScript = fs.readFileSync(
  path.resolve(sketchDir, '..', 'scripts', 'internal', 'build-idf'), 'utf8');
const idfHelpers = fs.readFileSync(
  path.resolve(sketchDir, '..', 'scripts', 'openbrewbyweight_idf.sh'), 'utf8');
const bleRuntime = fs.readFileSync(
  path.resolve(sketchDir, '..', 'idf', 'components',
               'OpenBrewByWeightBleRuntime', 'OpenBrewByWeightBleRuntime.cpp'),
  'utf8');
const flashIoScratch = fs.readFileSync(
  path.join(sketchDir, 'OpenBrewByWeightFlashIoScratch.h'), 'utf8');
if (!idfMain.includes('bool bleInUse(void) { return true; }')) {
  throw new Error(
      'Native NimBLE must keep BLE controller memory from being released by initArduino');
}
if (firmware.includes('ble_gatts_add_svcs(') ||
    firmware.includes('companionAdvertisingShouldPause') ||
    firmware.includes('syncCompanionAdvertisingForScaleLink') ||
    firmware.includes('BleCompanion') ||
    firmware.includes('BLE_COMPAT_')) {
  throw new Error('Firmware must not register Companion GATTS or Companion command paths');
}
if (!taskProfiler.includes('allocExternal(sizeof(ActiveWorkspace), AllocationOwner::PROFILER)') ||
    !taskProfiler.includes('heapCapsFree(workspace_)') ||
    taskProfiler.includes('allocExternalOrInternal(sizeof(ActiveWorkspace))') ||
    taskProfiler.includes('calloc(') ||
    /\bfree\(workspace_\)/.test(taskProfiler) ||
    /\bfree\(next\)/.test(taskProfiler)) {
  throw new Error(
      'TaskProfiler workspace must use allocExternal/heapCapsFree, not calloc/free or internal fallback');
}
if (taskProfiler.includes('task.xCoreID') ||
    !taskProfiler.includes('xTaskGetCoreID(task.xHandle)')) {
  throw new Error(
      'TaskProfiler must read core pin via xTaskGetCoreID, not TaskStatus_t.xCoreID');
}
if (!sdkconfigDefaults.includes('CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y') ||
    !sdkconfigDefaults.includes('CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y')) {
  throw new Error(
      'sdkconfig.defaults must enable run-time stats and vTaskList core IDs');
}
if ([
  'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8',
  'CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32',
  'CONFIG_ESP_WIFI_RX_BA_WIN=16',
  'CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16',
  'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32',
  'CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384',
  'CONFIG_LWIP_TCP_WND_DEFAULT=16384',
  'CONFIG_LWIP_TCP_RECVMBOX_SIZE=16',
].some(setting => !sdkconfigDefaults.split('\n').includes(setting) ||
    !idfHelpers.includes(`"${setting}"`))) {
  throw new Error(
      'IDF defaults and effective-configuration verification must agree on bounded Wi-Fi pools and 16 KiB TCP capacity');
}
if (!sdkconfigNimble.includes('CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y') ||
    !sdkconfigNimble.includes('CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096') ||
    !sdkconfigNimble.includes('CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=23') ||
    !sdkconfigNimble.includes('CONFIG_BT_NIMBLE_ATT_MAX_PREP_ENTRIES=4') ||
    !sdkconfigNimble.includes('CONFIG_BT_NIMBLE_GATT_MAX_PROCS=2') ||
    !sdkconfigNimble.includes('CONFIG_BT_NIMBLE_MAX_CCCDS=4')) {
  throw new Error(
      'Production NimBLE defaults must retain the qualified pools, stack, MTU and external allocator');
}
if (!bleComponentCmake.includes('EspressoScaleBLENimble.cpp') ||
    bleComponentCmake.includes('ESPRESSO_SCALE_BLE_BACKEND') ||
    bleComponentCmake.includes('ArduinoBLE') ||
    idfBuildScript.includes('OPENBREWBYWEIGHT_BLE_BACKEND') ||
    idfBuildScript.includes('OPENBREWBYWEIGHT_NIMBLE_ALLOCATOR') ||
    fs.existsSync(path.resolve(sketchDir, '..', 'scripts',
                               'patch_arduinoble.sh')) ||
    fs.existsSync(path.resolve(sketchDir, '..', 'idf', 'components',
                               'ArduinoBLE')) ||
    fs.existsSync(path.resolve(sketchDir, '..', 'idf',
                               'sdkconfig.defaults.arduinoble')) ||
    fs.existsSync(path.resolve(sketchDir, '..', 'idf',
                               'sdkconfig.defaults.nimble-internal'))) {
  throw new Error(
      'Production must build native NimBLE only, without backend selectors or ArduinoBLE infrastructure');
}
for (const service of [
  'PROX', 'ANS', 'CTS', 'HTP', 'IPSS', 'TPS',
  'IAS', 'LLS', 'SPS', 'HR', 'BAS', 'DIS'
]) {
  if (!sdkconfigNimble.includes(
          `# CONFIG_BT_NIMBLE_${service}_SERVICE is not set`)) {
    throw new Error(`Unused NimBLE ${service} service must stay disabled`);
  }
}
for (const feature of ['DTM_MODE_TEST', 'SM_SIGN_CNT', 'CPFD_CAFD']) {
  if (!sdkconfigNimble.includes(
          `# CONFIG_BT_NIMBLE_${feature} is not set`)) {
    throw new Error(`Unused NimBLE ${feature} feature must stay disabled`);
  }
}
if (!bleRuntime.includes('gHostTask = xTaskGetCurrentTaskHandle()') ||
    !bleRuntime.includes('uxTaskGetStackHighWaterMark(') ||
    !bleRuntime.includes('hostTaskHandle')) {
  throw new Error(
      'NimBLE HEALTH must sample the live host-task stack watermark');
}
if (!sdkconfigDefaults.includes('CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768') ||
    !sdkconfigDefaults.includes('CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM=y') ||
    !sdkconfigDefaults.includes('CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y') ||
    !sdkconfigDefaults.includes('CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y') ||
    !sdkconfigDefaults.includes('xTaskCreate still allocates internal stacks') ||
    sdkconfigDefaults.includes('CONFIG_FREERTOS_PLACE_TASK_STACKS_IN_EXT_RAM=y')) {
  throw new Error(
      'sdkconfig.defaults must place only mDNS runtime memory in PSRAM and keep other task stacks internal');
}
if (!idfHelpers.includes('sdkconfig.defaults.micra') ||
    !idfHelpers.includes('obbw_idf_config_fingerprint') ||
    !idfBuildScript.includes('obbw_idf_refresh_config') ||
    !sdkconfigMicra.includes('# CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC is not set') ||
    !sdkconfigMicra.includes('# CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC is not set') ||
    !sdkconfigMicra.includes('CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y') ||
    !sdkconfigMicra.includes('CONFIG_MBEDTLS_DYNAMIC_BUFFER=y') ||
    !sdkconfigMicra.includes('CONFIG_WS_BUFFER_SIZE=4096')) {
  throw new Error(
      'Micra-only mbedTLS must use PSRAM and dynamic record buffers');
}
if (sdkconfigDefaults.includes('CONFIG_FREERTOS_USE_TICKLESS_IDLE=y') ||
    !sdkconfigDefaults.includes('CONFIG_PM_ENABLE=y') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightPowerManagement.cpp'), 'utf8')
      .includes('config.light_sleep_enable = false')) {
  throw new Error(
      'PM requires DFS support while automatic light sleep remains disabled');
}
if (sdkconfigDefaults.includes('CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y') ||
    !sdkconfigDefaults.includes('CONFIG_ESP_CONSOLE_NONE=y') ||
    !sdkconfigDefaults.includes('CONFIG_ESP_CONSOLE_SECONDARY_NONE=y') ||
    !sdkconfigDefaults.includes('CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=0')) {
  throw new Error(
      'IDF console must default to NONE; JTAG stays off unless ENABLE_JTAG=1');
}
{
  const sdkconfigJtag = fs.readFileSync(
      path.resolve(sketchDir, '..', 'idf', 'sdkconfig.defaults.jtag'), 'utf8');
  if (!sdkconfigJtag.includes('CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y') ||
      !sdkconfigJtag.includes('CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1') ||
      !sdkconfigJtag.includes('# CONFIG_ESP_CONSOLE_NONE is not set')) {
    throw new Error(
        'sdkconfig.defaults.jtag must turn USB Serial/JTAG on for ENABLE_JTAG=1');
  }
}
if (sdkconfigDefaults.includes('CONFIG_BT_LE_SLEEP_ENABLE=y') ||
    !sdkconfigDefaults.includes('CONFIG_BT_CTRL_MODEM_SLEEP=y') ||
    !sdkconfigDefaults.includes('CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightScaleWorker.cpp'), 'utf8')
      .includes('sleep ? esp_bt_sleep_enable() : esp_bt_sleep_disable()')) {
  throw new Error(
      'S3 controller sleep requires XTAL and reversible worker-owned control');
}
{
  const cmakeLists = fs.readFileSync(
      path.resolve(sketchDir, '..', 'idf', 'CMakeLists.txt'), 'utf8');
  const bleHeader = fs.readFileSync(
      path.resolve(sketchDir, '..', 'libraries', 'EspressoScaleBLE', 'src',
                   'EspressoScaleBLE.h'),
      'utf8');
  if (!cmakeLists.includes('ARDUINO_USB_CDC_ON_BOOT=${_obbw_cdc_on_boot}') ||
      !cmakeLists.includes('set(_obbw_cdc_on_boot 0)') ||
      !cmakeLists.includes('-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1') ||
      cmakeLists.includes('COMPILE_DEFINITIONS "ARDUINO_USB_CDC_ON_BOOT=0"') ||
      cmakeLists.includes('COMPILE_DEFINITIONS "ARDUINO_USB_CDC_ON_BOOT=1"')) {
    throw new Error(
        'IDF CMakeLists must default Arduino CDC off and turn it on only for ENABLE_JTAG=1');
  }
  if (!bleHeader.includes('#define BLE_SCAN_RELAXED_INTERVAL         0x00B8') ||
      !bleHeader.includes('#define BLE_SCAN_RELAXED_WINDOW           0x002E') ||
      !bleHeader.includes('#define BLE_SCAN_BALANCED_INTERVAL        0x0064') ||
      !bleHeader.includes('#define BLE_SCAN_BALANCED_WINDOW          0x0032') ||
      !bleHeader.includes('#define BLE_SCAN_AGGRESSIVE_INTERVAL      0x0020') ||
      !bleHeader.includes('#define BLE_SCAN_AGGRESSIVE_WINDOW        0x0020')) {
    throw new Error(
        'BLE scan presets must be Relaxed 25% (28.75/115), Balanced 50% (31.25/62.5), Aggressive 100% (20/20)');
  }
  if (!firmware.includes('USB_CONSOLE_GPIO') ||
      !firmware.includes('OPEN_BREW_BY_WEIGHT_USB_CONSOLE_GPIO 4') ||
      !firmwareCore.includes('usbConsoleJumperPresent()') ||
      !firmwareCore.includes('UsbSerialEnableSource::COMPILE_FLAG') ||
      !firmwareCore.includes('UsbSerialEnableSource::JUMPER') ||
      !firmwareCore.includes('Serial.begin(SERIAL_BAUD)') ||
      !firmwareCore.includes(
          'usbSerialEnableSource = UsbSerialEnableSource::COMPILE_FLAG') ||
      !firmwareCore.includes(
          'usbSerialEnableSource = UsbSerialEnableSource::JUMPER')) {
    throw new Error(
        'USB CDC must start for ENABLE_JTAG=1 or when the GPIO4 jumper is held at boot');
  }
  const usbConsole = fs.readFileSync(
      path.join(sketchDir, 'OpenBrewByWeightUsbConsole.h'), 'utf8');
  const serialCli = fs.readFileSync(
      path.join(sketchDir, 'OpenBrewByWeightSerialCli.h'), 'utf8');
  if (!serialCli.includes('#include "OpenBrewByWeightUsbConsole.h"') ||
      !usbConsole.includes('OPEN_BREW_BY_WEIGHT_USB_CONSOLE_OWN_HWCDC') ||
      !usbConsole.includes('#define Serial ::openBrewByWeightUsbConsole') ||
      !firmwareCore.includes('HWCDC openBrewByWeightUsbConsole')) {
    throw new Error(
        'GPIO4 jumper must start USB Serial/JTAG HWCDC, not UART0 Serial0');
  }
  const buildIdf = fs.readFileSync(
      path.resolve(sketchDir, '..', 'scripts', 'internal', 'build-idf'), 'utf8');
  if (!idfHelpers.includes('obbw_idf_prepare_set_target') ||
      !idfHelpers.includes('obbw_idf_commit_extra_flags_stamp') ||
      !idfHelpers.includes('obbw_idf_jtag_enabled') ||
      !idfHelpers.includes('obbw_idf_config_fingerprint') ||
      !idfHelpers.includes('sdkconfig.defaults.jtag') ||
      !buildIdf.includes('obbw_idf_prepare_set_target') ||
      !buildIdf.includes('obbw_idf_commit_extra_flags_stamp') ||
      !buildIdf.includes('obbw_idf_refresh_config') ||
      !idfHelpers.includes('Preparing empty IDF build tree for set-target')) {
    throw new Error(
        'dev build must empty a non-CMake tree before idf.py set-target fullclean');
  }
  if (!buildIdf.includes('OpenBrewByWeightBuildProfileGenerated.h') ||
      !buildIdf.includes('OPEN_BREW_BY_WEIGHT_MACHINE_TYPE') ||
      !buildIdf.includes('--machine-type paddle') ||
      !buildIdf.includes('--machine-type momentary') ||
      !buildIdf.includes('--machine-type momentary_reed') ||
      !buildIdf.includes('gen_web_ui.js')) {
    throw new Error(
        'IDF builds must bake the machine-type-exclusive Web UI by forwarding the resolved machine type to gen_web_ui.js');
  }
  if (!buildIdf.includes('OPEN_BREW_BY_WEIGHT_MACHINE_WEB_ICON') ||
      !buildIdf.includes('--machine-icon')) {
    throw new Error(
        'IDF builds must forward the machine profile icon to gen_web_ui.js so Home bakes the machine row glyph');
  }
  if (!buildIdf.includes('OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL') ||
      !buildIdf.includes('--no-remote-control')) {
    throw new Error(
        'IDF builds without remote machine control must forward --no-remote-control to gen_web_ui.js');
  }
}
// The machine profile's named icon is the only glyph baked into Home's
// Equipment machine row: the catalog sprite and the marker are scaffolding
// that must never ship, and an unnamed machine keeps the default glyph.
{
  const [defaultUi, micraUi] = await Promise.all([
    webUi.generate({webUiLanguage: 'EN', write: false}),
    webUi.generate({webUiLanguage: 'EN', machineIcon: 'linea-micra', write: false}),
  ]);
  const generic = 'M4 3h16v8H4', micra = 'M4 2.5h16a1 1 0 0 1';
  for (const generated of [defaultUi, micraUi]) {
    if (generated.html.includes('data-machine-icons') ||
        generated.html.includes('{{webui-meta:machine-icon}}')) {
      throw new Error('machine-icon scaffolding must not ship in generated markup');
    }
  }
  if (!defaultUi.html.includes(generic) || defaultUi.html.includes(micra) ||
      !micraUi.html.includes(micra) || micraUi.html.includes(generic)) {
    throw new Error('generate() must bake exactly the machine profile icon into the Home machine row');
  }
}
if (!taskProfiler.includes('void copySnapshot(TaskProfilerSnapshot &out) const') ||
    !taskProfiler.includes('TaskLockGuard lock(reportMutex_)') ||
    taskProfiler.includes('__atomic_load_n(&seq_') ||
    !taskProfiler.includes('beginSnapshotWrite_()')) {
  throw new Error(
      'TaskProfiler snapshot copies must use a task mutex; uxTaskGetSystemState must not run under it');
}
if (!firmwareCore.includes('#include "OpenBrewByWeightTaskProfiler.h"') ||
    !firmwareCore.includes('TaskProfiler taskProfiler') ||
    !firmwareCore.includes('taskProfiler.service(now)') ||
    !firmwareCore.includes('void copyTaskProfiler(TaskProfilerSnapshot &output)') ||
    !firmwareCore.includes('taskProfiler.copySnapshot(output)') ||
    !firmwareCore.includes('WebCommandType::TASK_PROFILER_START') ||
    !firmwareCore.includes('WebCommandType::TASK_PROFILER_STOP') ||
    !firmwareCore.includes('callbacks.copyTaskProfiler = copyTaskProfiler') ||
    (firmwareCore.match(/taskProfiler\.start\(/g) || []).length !== 1) {
  throw new Error(
      'TaskProfiler must be wired as opt-in Diagnostic telemetry with copyTaskProfiler');
}
{
  const setupBody = firmwareCore.slice(
      firmwareCore.indexOf('void setup()'), firmwareCore.indexOf('void loop()'));
  const publishBody = firmwareCore.slice(
      firmwareCore.indexOf('void publishControlStatus()'),
      firmwareCore.indexOf('void resetSerialCliState()'));
  if (setupBody.includes('taskProfiler.start') ||
      publishBody.includes('taskProfiler') ||
      publishBody.includes('TaskProfiler')) {
    throw new Error(
        'Task profiler must not auto-start on boot and must not ride ControlStatusSnapshot');
  }
}
if (!network.includes('copyTaskProfiler') ||
    !network.includes('statusJsonAppendTaskProfiler') ||
    !network.includes('\\"tasks\\"') ||
    !network.includes('\\"currentTotalCpuPct\\"') ||
    !network.includes('\\"unreportedCurrentCpuPct\\"') ||
    !network.includes('\\"name\\":\\"loopTask/%s\\"') ||
    !network.includes('\\"averageExecutionUs\\"') ||
    !network.includes('\\"lastExecutionUs\\"') ||
    !network.includes('\\"peakGapExecutionUs\\"') ||
    !network.includes('\\"recentGapMs\\"') ||
    !network.includes('\\"recentGapExecutionUs\\"') ||
    !network.includes('/api/v1/diagnostic/profiler') ||
    !network.includes('taskProfilerHandler') ||
    !network.includes('static constexpr size_t kStatusJson = 40960') ||
    !networkHeader.includes('void (*copyTaskProfiler)(TaskProfilerSnapshot &out)') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightDebugExport.h'), 'utf8')
        .includes('DEBUG_EXPORT_SCHEMA_VERSION = 4')) {
  throw new Error(
      'Diagnostic status, POST /api/v1/diagnostic/profiler, and debug export must expose tasks');
}
const psram = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightPsram.h'), 'utf8');
if (!psram.includes('#define OPEN_BREW_BY_WEIGHT_PSRAM_BSS EXT_RAM_BSS_ATTR') ||
    !firmwareCore.includes('OPEN_BREW_BY_WEIGHT_PSRAM_BSS ActivationStores activationStores') ||
    !firmwareCore.includes('HistoryLog &historyLog = activationStores.historyLog') ||
    !firmwareCore.includes(
        'OPEN_BREW_BY_WEIGHT_PSRAM_BSS PersistedSettings persistedSettings') ||
    !firmwareCore.includes(
        'OPEN_BREW_BY_WEIGHT_PSRAM_BSS SettingsPersistRequest settingsPersistRequest') ||
    firmwareCore.includes(
        'OPEN_BREW_BY_WEIGHT_PSRAM_BSS SettingsPersistRequest settingsPersistReceive') ||
    !firmwareCore.includes(
        'xQueueReceive(settingsPersistQueue, &work') ||
    !firmwareCore.includes(
        'xQueueCreate(PERSISTENCE_WORK_QUEUE_DEPTH, sizeof(PersistenceWork))') ||
    !firmwareCore.includes(
        'constexpr uint32_t SETTINGS_PERSIST_TASK_STACK_SIZE = 5120') ||
    !network.includes(
        'static OPEN_BREW_BY_WEIGHT_PSRAM_BSS WifiScanSnapshot g_wifiScan') ||
    !network.includes(
        'static OPEN_BREW_BY_WEIGHT_PSRAM_BSS PersistedSettings g_networkSettings') ||
    !networkHeader.includes('PersistedSettings &settings_') ||
    !firmwareCore.includes(
        'OPEN_BREW_BY_WEIGHT_PSRAM_BSS DebugRingBuffer debugLog') ||
    !firmwareCore.includes(
        'OPEN_BREW_BY_WEIGHT_PSRAM_BSS RuntimeConfig publishedRuntimeConfig') ||
    !firmwareCore.includes(
        'OPEN_BREW_BY_WEIGHT_PSRAM_BSS ShotPresetBank publishedPresetBank') ||
    firmwareCore.includes('stagingControlStatus') ||
    !flashIoScratch.includes('allocInternal(FLASH_IO_SCRATCH_BYTES, AllocationOwner::FLASH_IO)') ||
    !firmwareCore.includes('OPEN_BREW_BY_WEIGHT_PSRAM_BSS ControlStatusCurve publishedControlCurve') ||
    firmwareCore.includes('OPEN_BREW_BY_WEIGHT_PSRAM_BSS ControlStatusFields') ||
    firmwareCore.includes('OPEN_BREW_BY_WEIGHT_PSRAM_BSS ControlStatusSnapshot')) {
  throw new Error(
      'History/settings/debug-ring/recipe/curve payloads use PSRAM; flash scratch and control scalars stay internal');
}
if (!firmwareCore.includes('uint8_t *serialLogQueueBytes = nullptr') ||
    !firmwareCore.includes(
      'allocInternal(queueBytes, AllocationOwner::SERIAL_LOG)') ||
    !firmwareCore.includes('esp_ptr_internal(serialLogQueueBytes)') ||
    !firmwareCore.includes('SERIAL_LOG_QUEUE_DEPTH = 8') ||
    !serialCli.includes('SERIAL_CLI_OUTPUT_CAPACITY = 2880') ||
    !firmwareCore.includes(
      'allocExternal(SERIAL_CLI_OUTPUT_CAPACITY') ||
    !firmwareCore.includes('releaseSerialLogStorage()') ||
    !firmwareCore.includes('drainSerialCliOutput()') ||
    !firmwareCore.includes('Print &serialCliOutput()') ||
    !firmwareCore.includes('serialLogQueueTruncated') ||
    !firmwareCore.includes('serialCliOutput().println(message)') ||
    !firmwareCore.includes('heapCapsFree(serialLogQueueBytes)') ||
    !firmwareCore.includes('heapCapsFree(serialCliOutputBytes)') ||
    firmwareCore.includes(
      'uint8_t serialLogQueueBytes[SERIAL_LOG_QUEUE_DEPTH * sizeof(SerialLogLine)]')) {
  throw new Error(
    'USB log queue must stay internal while CLI output uses PSRAM with complete startup rollback');
}
if (!flashIoScratch.includes('FlashStoreTransaction') ||
    !flashIoScratch.includes('FLASH_IO_SECTOR_BYTES') ||
    !flashIoScratch.includes('Phase::COMMIT') ||
    !flashIoScratch.includes('flashIoForbiddenTaskSlot() == xTaskGetCurrentTaskHandle()') ||
    !activationStoresIo.includes('serviceStep(') ||
    !firmwareCore.includes('forbidFlashIoFromCurrentTask();')) {
  throw new Error(
    'Partition stores must advance one body-first transaction step at a time and forbid runtime control flash ownership');
}
if (!buzzer.includes('struct RtttlCatalog') ||
    !buzzer.includes('RtttlCatalog *rtttlCatalog') ||
    !buzzer.includes('allocInternal(sizeof(RtttlCatalog), AllocationOwner::BUZZER)') ||
    !buzzer.includes('RtttlNote rtttlBuf[BULLSEYE_RTTTL_MAX_NOTES]') ||
    !firmwareCore.includes('TaskMutex debugLogMutex') ||
    !firmwareCore.includes(
        'debugLog.copyAfter(afterSequence, output, capacity, metadata)') ||
    firmwareCore.includes('portENTER_CRITICAL(&debugLogMux)') ||
    !firmwareCore.includes('debugLogDroppedSnapshot') ||
    firmwareCore.includes('debugLogContentionDropped') ||
    firmwareCore.includes('__atomic_add_fetch(&debugLogDroppedSnapshot')) {
  throw new Error(
      'RTTTL catalog and active playback buffer must stay internal RAM — playback starts from the esp_timer task under a spinlock, and PSRAM is unreachable while flash writes disable the cache');
}
if (network.includes('ControlStatusSnapshot status;') ||
    network.includes('ControlStatusSnapshot control;') ||
    firmwareCore.includes('ControlStatusSnapshot status;') ||
    !network.includes('ControlGateSnapshot OpenBrewByWeightNetwork::controlGate()') ||
    !firmwareCore.includes('void copyControlGate(ControlGateSnapshot &output)')) {
  throw new Error(
      'Network/httpd gate checks must copy ControlGateSnapshot, not a stack ControlStatusSnapshot');
}
if (firmwareCore.includes('portENTER_CRITICAL(&webStatusMux)') ||
    firmwareCore.includes('next.presets = presetBank') ||
    firmwareCore.includes('copyScaleHistory(next.scaleHistory)') ||
    !firmwareCore.includes('TaskMutex controlStatusMutex') ||
    !firmwareCore.includes('TaskMutex controlGateMutex') ||
    !firmwareCore.includes('TaskMutex recipeMutex') ||
    firmwareCore.includes('controlStatusSeq') ||
    firmwareCore.includes('controlGateSeq') ||
    firmwareCore.includes('recipeSeq') ||
    !network.includes('callbacks_.copyPresetBank(&g_work->presetBank)') ||
    !network.includes('self.callbacks_.copyScaleHistory(g_work->scaleHistory)')) {
  throw new Error(
      'Status and recipe snapshots must use task mutexes and fill presets/history on demand');
}
if (network.includes('composeEffectiveConfig(candidate, status.presets)') ||
    network.includes('status.presets') ||
    !network.includes('self.callbacks_.copyPresetBank(&livePresets)') ||
    !network.includes('composeEffectiveConfig(candidate, livePresets)')) {
  throw new Error(
      'configHandler must composeEffectiveConfig against copyPresetBank, not ControlGateSnapshot.presets');
}
if (!flashIoScratch.includes('inline void feedFlashIoWatchdog()') ||
    !flashIoScratch.includes('esp_task_wdt_status(nullptr) == ESP_OK') ||
    !flashIoScratch.includes('(void)esp_task_wdt_reset()')) {
  throw new Error(
      'Flash I/O watchdog feed must reset TWDT only when the current task is subscribed');
}
if (!bleLibrary.includes('length == 0 || length > MAX_BLE_PACKET_LENGTH') ||
    !bleLibrary.includes('os_mbuf_copydata(buffer, 0, length, frame.data)')) {
  throw new Error('Native NimBLE reads must bound and copy mbuf payloads');
}
if (!bleLibrary.includes('params.filter_duplicates = 0') ||
    !bleLibrary.includes('const bool useAddressScan = filtered && addressScan') ||
    !bleLibrary.includes('nimbleAdvertisementIsCompatible') ||
    !bleLibrary.includes('scaleUuid16AllowsNamelessConnect') ||
    !firmware.includes(
        'currentScaleMacCacheMode() == ScaleMacCacheMode::ONLY') ||
    !firmware.includes(
        'scale.startScan(mac, forceRestart, interval, window, addressScan)')) {
  throw new Error(
      'Native GAP scan must retain duplicate reports, UUID matching and ONLY address filtering');
}
if (!firmware.includes(
        'vTaskPrioritySet(nullptr, tskIDLE_PRIORITY + CONTROL_TASK_PRIORITY_OFFSET)') ||
    !scaleWorker.includes('tskIDLE_PRIORITY + SCALE_WORKER_TASK_PRIORITY_OFFSET') ||
    !firmware.includes('tskIDLE_PRIORITY + HEALTH_TASK_PRIORITY_OFFSET')) {
  throw new Error('Task priorities must come from the schedule contract');
}
const firmwareModeWrite = network.slice(
    network.indexOf('OpenBrewByWeightNetwork::firmwareModeHandler('),
    network.indexOf('openbrewbyweight::saveFirmwareMode('));
if (!firmwareModeWrite.includes('networkFlashWriteAllowed()')) {
  throw new Error('Firmware mode must persist only under the durable-write gate');
}
if (!/waitForShotStoreWriteGate\(true\);[\s\S]{0,160}savePersistedSettings\(settingsPersistRequest\.blob\)/
        .test(firmwareCore)) {
  throw new Error('SETTINGS writes must recheck the durable-write gate on the worker');
}
if (!firmware.includes('scaleWorkerTickDelayMs()') ||
    !firmware.includes('controlLoopTickDelayMs(loopScaleLink)') ||
    !firmware.includes('controlHousekeepingDue(loopStartedAtMs)') ||
    !firmware.includes('SCALE_WORKER_NO_SCALE_DELAY_MS') ||
    !firmware.includes('SCALE_STREAM_GAP_MS') ||
    !firmwareCore.includes('void refreshControlStatus()') ||
    !firmwareCore.includes('void serviceControlStatusPublish()') ||
    !firmwareCore.includes('void publishControlGate()') ||
    !firmwareCore.includes('vTaskDelay(pdMS_TO_TICKS(1))') ||
    firmwareCore.includes('CONTROL_STATUS_PUBLISH_MS') ||
    firmwareCore.includes('CONTROL_STATUS_PUBLISH_NO_SCALE_MS')) {
  throw new Error(
      'No-scale idle must relax worker/loop; status snapshot is GET-driven, not 50 ms');
}
if (/\bruntimeConfig\b/.test(scaleWorker) ||
    /\bfirmwareInitializationComplete\b/.test(scaleWorker) ||
    /\bresetCupPresence\s*\(/.test(scaleWorker) ||
    /\bemitAlert\s*\(/.test(scaleWorker) ||
    !firmwareCore.includes('publishScaleWorkerPolicy(runtimeConfig,') ||
    !firmwareCore.includes('void processScaleLinkTransitions()')) {
  throw new Error(
      'Scale worker must consume published policy/facts; only control may mutate cup state or emit alerts');
}
{
  const loopBody = firmwareCore.slice(firmwareCore.indexOf('void loop()'));
  const drainCount = (loopBody.match(/processScaleWorkerEvents\(\)/g) || []).length;
  if (drainCount < 2 ||
      !network.includes('bool OpenBrewByWeightNetwork::lockWorkBufForStatus') ||
      !network.includes('void OpenBrewByWeightNetwork::loadControlStatus') ||
      !network.includes('callbacks_.refreshControlStatus != nullptr') ||
      (network.split('lockWorkBufForStatus()').length - 1) < 4 ||
      !firmwareCore.includes('callbacks.refreshControlStatus = refreshControlStatus') ||
      !firmwareCore.includes(
          '__atomic_store_n(&controlStatusPublishRequested, false')) {
    throw new Error(
        'Weight mailbox must drain twice per loop; GET status must wait for a committed snapshot');
  }
}
{
  const reedState = fs.readFileSync(
      path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryReedState.h'), 'utf8');
  const momentaryInput = fs.readFileSync(
      path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryInput.h'), 'utf8');
  if (!momentaryInput.includes('inline uint32_t machineElapsedMs() {') ||
      reedState.includes('inline uint32_t machineElapsedMs')) {
    throw new Error(
        'machineElapsedMs must be defined once in the shared momentary input');
  }
  const getterNames = [
    ['bool machineAllowsFirmwareStopPulse', 'inline bool machineRunningElapsed'],
    ['inline bool machineRunningElapsed', 'inline bool machineIsRunning'],
    ['inline bool machineIsRunning', 'inline MachineRunState machineRunState'],
    ['inline MachineRunState machineRunState', 'inline void machineFillInferenceStatus'],
  ];
  for (const [startName, endName] of getterNames) {
    const start = reedState.indexOf(startName);
    const end = reedState.indexOf(endName);
    if (start < 0 || end <= start || reedState.slice(start, end).includes('sampleReed()')) {
      throw new Error('Reed getters must use the last sampled state, not sampleReed()');
    }
  }
  if (!reedState.includes('void serviceReedAssumeWindow() {\n  sampleReed();') ||
      !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachine.h'), 'utf8')
           .includes('sampleReed();')) {
    throw new Error('Reed must be sampled from input/service, not from getters');
  }
}
{
  const hwmon = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightHwmon.h'), 'utf8');
  if (!hwmon.includes('const HeapCapSnapshot *heap = nullptr') ||
      !hwmon.includes('heap != nullptr ? *heap : sampleHeapCaps()') ||
      !firmwareCore.includes('sample.hwmon = hwmon.sample(') ||
      !firmwareCore.includes('&sample.heap)')) {
    throw new Error('Hwmon must reuse the 5 s HeapCapSnapshot instead of sampling heap twice');
  }
}
{
  // The four input sub-phase boundaries must partition the retired aggregate
  // phase chronologically around their call groups (checked within the loop
  // body so definitions elsewhere in the concatenation cannot match).
  const loopBody = firmwareCore.slice(
      firmwareCore.indexOf('recordResetUptime(loopStartedAtMs)'));
  const marks = [
    'machineIntegrationBackflush(true)',
    'finishLoopPhase(LoopPhase::INPUT_BACKFLUSH)',
    'pushActivatorDrivePermission(loopScaleLink)',
    'finishLoopPhase(LoopPhase::INPUT_DRIVE_PERMISSION)',
    'processScaleWorkerEvents();',
    'finishLoopPhase(LoopPhase::INPUT_SCALE_EVENTS)',
    'observeMachineSenseFromSession(loopScaleLink)',
    'finishLoopPhase(LoopPhase::INPUT_MACHINE_SERVICE)',
  ].map((m) => loopBody.indexOf(m));
  if (marks.some((at) => at < 0) ||
      marks.some((at, i) => i > 0 && at <= marks[i - 1])) {
    throw new Error(
        'Input sub-phase boundaries must stay in chronological order around their call groups');
  }
}
if (!domain.includes('HEALTH_HEAP_LOW_RESTART_MS') ||
    !firmwareCore.includes('DebugCode::HEALTH_HEAP_RESTART') ||
    !firmwareCore.includes('controlAllowsConfigurationNow() && !circuitClosed')) {
  throw new Error(
      'Sustained heap-low must request a safe restart only from Ready with the circuit open');
}
if (!bleLibrary.includes('BLE_CONNECT_TIMEOUT_MS + kConnectCallbackMarginMs') ||
    !bleLibrary.includes('ble_gap_connect(openBrewByWeightBleRuntimeOwnAddressType()') ||
    !bleLibrary.includes('beginOperation(CallbackDomain::Link)') ||
    !bleLibrary.includes('event.generation != generation_') ||
    !bleLibrary.includes('finishLink(true')) {
  throw new Error('Native GAP/GATT operations must be deadline- and generation-bounded');
}
if (/#define\s+BLE_CONNECT_TIMEOUT_MS\s+4000UL/.test(bleLibrary)) {
  throw new Error(
      'GAP connect timeout must stay under the 5 s task watchdog (not 4 s)');
}
if (!bleLibrary.includes(
        'return clientFromStorage(g_clientStorage).isLinkUp()') ||
    !bleLibrary.includes(
        'return clientFromStorage(g_clientStorage).newWeightAvailable()')) {
  throw new Error(
      'EspressoScaleBLE facade must delegate link and packet state to native NimBLE storage');
}
if (firmware.includes('if (scaleLinked || changed)')) {
  throw new Error(
      'Scale-linked status must not force a publish every tick');
}
if (firmware.includes('companionAdvertisingShouldPause') ||
    firmware.includes('syncCompanionAdvertisingForScaleLink') ||
    (firmware.split('syncCompanionAdvertisingForScaleLink();').length - 1) !== 0 ||
    !firmware.includes('scale.isConnecting()') ||
    !firmware.includes('SCALE_HUNT_RF_CLEAR_MS') ||
    !firmware.includes('scaleHuntRfClearActive') ||
    !firmware.includes('syncScaleSoftApRadio') ||
    !firmware.includes('applySoftApDiscoveryYield') ||
    firmware.includes('BLE.poll(') ||
    !scaleWorker.includes(
        'ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(tickDelayMs))') ||
    (scaleWorker.split('syncScaleRadioCoex();').length - 1) < 2 ||
    !scaleWorker.includes('GAP/GATT setup must not wait behind')) {
  throw new Error(
      'Companion advertising must be absent; scale worker must still block on HCI and yield SoftAP discovery');
}
if (firmware.includes('SCALE_LINK_COEX_BT_MS') ||
    firmware.includes('scaleLinkCoexHadLink') ||
    firmware.includes('setRfCoexClaim') ||
    firmware.includes('rfCoexWinner') ||
    firmware.includes('preferStaWifiCoex') ||
    !firmware.includes('scale.isConnecting() || scale.isLinkUp()') ||
    !firmware.includes('ensureRfCoexBt') ||
    !firmware.includes('ESP_COEX_PREFER_BT') ||
    firmware.includes('applyRfCoexPreference') ||
    firmware.includes('ESP_COEX_PREFER_WIFI') ||
    firmware.includes('ESP_COEX_PREFER_BALANCE') ||
    firmware.includes('RfCoexPreference::WIFI') ||
    firmware.includes('RfCoexPreference::BALANCE') ||
    !firmware.includes('serviceScaleScanIntensity') ||
    firmware.includes('serviceScaleScanDuty') ||
    firmware.includes('SCALE_SCAN_BURST_MS') ||
    firmware.includes('nextScaleConnectRetryMs') ||
    firmware.includes('SCALE_CONNECT_RETRY_MS') ||
    !firmware.includes('applyLiveBleScanIntensity')) {
  throw new Error(
      'RF coex must always prefer BT (ensureRfCoexBt); claims and STA WIFI preference are gone; scan uses live intensity, not idle/burst');
}
if (otaSource.includes('OTA_PROGRESS_INTERVAL_BYTES') ||
    otaSource.includes('runningPartition_') ||
    otaSource.includes('stagedSizeBytes_') ||
    otaHeader.includes('runningPartition_') ||
    otaHeader.includes('stagedSizeBytes_')) {
  throw new Error('OTA must not retain the unused F-22 progress constant or inert partition/size state');
}
if (/setScaleLinkState\(ScaleLinkState::CONNECTED\);\s*applyBookooConnectBeepPolicy/.test(
        firmwareCore)) {
  throw new Error(
      'Bookoo connect volume must be deferred off the GATT success path');
}
if (!network.includes('delta.field("scale.lastDisconnectReasonName"') ||
    !network.includes('delta.field("scale.macCachePauseRemainingMs"')) {
  throw new Error(
      'Home status must include lastDisconnectReasonName on scale');
}
