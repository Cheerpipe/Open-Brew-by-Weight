#pragma once

#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightPersistedNetwork.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightBootHeap.h"
#include "OpenBrewByWeightTime.h"
#include "OpenBrewByWeightUsbConsole.h"

namespace openbrewbyweight {

constexpr size_t SERIAL_CLI_LINE_CAPACITY = 160;
constexpr size_t SERIAL_CLI_MAX_BYTES_PER_LOOP = 8;
constexpr size_t SERIAL_CLI_VERB_CAPACITY = 24;
// The audit verbs (dump/clear/verbose) extend HELP past the previous 2560
// bytes; one bounded flush must still hold the whole listing.
constexpr size_t SERIAL_CLI_OUTPUT_CAPACITY = 2880;

enum class SerialCliVerb : uint8_t {
  NONE = 0,
  HELP,
  HELLO,
  REBOOT,
  FACTORY_RESET,
  RESET_DEVICE_PASSWORD,
  SET_DEVICE_PASSWORD,
  SET_WIFI,
  CLEAR_SHOTS,
  CLEAR_WIFI,
  RESET_NETWORK_AP,
  SERIAL_DEBUG_ON,
  SERIAL_DEBUG_OFF,
  DEBUG_FULL,
  DEBUG_OFF,
  DEBUG_STATUS,
  WIFI_CONNECT,
  WIFI_DISCONNECT,
  WIFI_RESTART,
  WIFI_STATUS,
  AP_START,
  AP_STOP,
  AP_STATUS,
  WEBUI_START,
  WEBUI_STOP,
  WEBUI_RESTART,
  WEBUI_STATUS,
  NET_STATUS,
  LOG_DUMP,
  HEALTH,
  SCALE_STATUS,
  NTP_STATUS,
  HEAP,
  BOOT_HEAP,
  TASK_PROFILE,
  AUDIT_DUMP,
  AUDIT_CLEAR,
  AUDIT_VERBOSE,
  UNKNOWN,
  LINE_TOO_LONG,
  INVALID_ARGS
};

struct SerialCliRequest {
  SerialCliVerb verb = SerialCliVerb::NONE;
  char arg1[WIFI_PASSWORD_CAPACITY] = {};
  char arg2[WIFI_PASSWORD_CAPACITY] = {};
  bool openNetwork = false;
  const char *error = nullptr;
};

struct SerialCliParser {
  char line[SERIAL_CLI_LINE_CAPACITY] = {};
  size_t length = 0;
  bool overflow = false;
};

inline const char *serialCliVerbName(SerialCliVerb verb) {
  switch (verb) {
    case SerialCliVerb::NONE: return "NONE";
    case SerialCliVerb::HELP: return "HELP";
    case SerialCliVerb::HELLO: return "HELLO";
    case SerialCliVerb::REBOOT: return "REBOOT";
    case SerialCliVerb::FACTORY_RESET: return "FACTORY_RESET";
    case SerialCliVerb::RESET_DEVICE_PASSWORD: return "RESET_DEVICE_PASSWORD";
    case SerialCliVerb::SET_DEVICE_PASSWORD: return "SET_DEVICE_PASSWORD";
    case SerialCliVerb::SET_WIFI: return "SET_WIFI";
    case SerialCliVerb::CLEAR_SHOTS: return "CLEAR_SHOTS";
    case SerialCliVerb::CLEAR_WIFI: return "CLEAR_WIFI";
    case SerialCliVerb::RESET_NETWORK_AP: return "RESET_NETWORK_AP";
    case SerialCliVerb::SERIAL_DEBUG_ON: return "SERIAL_DEBUG_ON";
    case SerialCliVerb::SERIAL_DEBUG_OFF: return "SERIAL_DEBUG_OFF";
    case SerialCliVerb::DEBUG_FULL: return "DEBUG_FULL";
    case SerialCliVerb::DEBUG_OFF: return "DEBUG_OFF";
    case SerialCliVerb::DEBUG_STATUS: return "DEBUG_STATUS";
    case SerialCliVerb::WIFI_CONNECT: return "WIFI_CONNECT";
    case SerialCliVerb::WIFI_DISCONNECT: return "WIFI_DISCONNECT";
    case SerialCliVerb::WIFI_RESTART: return "WIFI_RESTART";
    case SerialCliVerb::WIFI_STATUS: return "WIFI_STATUS";
    case SerialCliVerb::AP_START: return "AP_START";
    case SerialCliVerb::AP_STOP: return "AP_STOP";
    case SerialCliVerb::AP_STATUS: return "AP_STATUS";
    case SerialCliVerb::WEBUI_START: return "WEBUI_START";
    case SerialCliVerb::WEBUI_STOP: return "WEBUI_STOP";
    case SerialCliVerb::WEBUI_RESTART: return "WEBUI_RESTART";
    case SerialCliVerb::WEBUI_STATUS: return "WEBUI_STATUS";
    case SerialCliVerb::NET_STATUS: return "NET_STATUS";
    case SerialCliVerb::LOG_DUMP: return "LOG_DUMP";
    case SerialCliVerb::HEALTH: return "HEALTH";
    case SerialCliVerb::SCALE_STATUS: return "SCALE_STATUS";
    case SerialCliVerb::NTP_STATUS: return "NTP_STATUS";
    case SerialCliVerb::HEAP: return "HEAP";
    case SerialCliVerb::BOOT_HEAP: return "BOOT_HEAP";
    case SerialCliVerb::TASK_PROFILE: return "TASK_PROFILE";
    case SerialCliVerb::AUDIT_DUMP: return "AUDIT_DUMP";
    case SerialCliVerb::AUDIT_CLEAR: return "AUDIT_CLEAR";
    case SerialCliVerb::AUDIT_VERBOSE: return "AUDIT_VERBOSE";
    case SerialCliVerb::UNKNOWN: return "UNKNOWN";
    case SerialCliVerb::LINE_TOO_LONG: return "LINE_TOO_LONG";
    case SerialCliVerb::INVALID_ARGS: return "INVALID_ARGS";
  }
  return "UNKNOWN";
}

inline void serialCliResetParser(SerialCliParser &parser) {
  parser = SerialCliParser{};
}

inline void serialCliClearRequest(SerialCliRequest &request) {
  request = SerialCliRequest{};
}

inline bool serialCliIsSpace(char c) {
  return c == ' ' || c == '\t';
}

inline bool serialCliEqualsIgnoreCase(const char *left, const char *right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  while (*left != '\0' && *right != '\0') {
    const char a = (*left >= 'A' && *left <= 'Z')
                       ? static_cast<char>(*left - 'A' + 'a')
                       : *left;
    const char b = (*right >= 'A' && *right <= 'Z')
                       ? static_cast<char>(*right - 'A' + 'a')
                       : *right;
    if (a != b) {
      return false;
    }
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

enum class SerialCliTokenResult : uint8_t { END, OK, ERROR };

inline SerialCliTokenResult serialCliNextToken(const char *&cursor, char *out,
                                               size_t outSize,
                                               const char **error) {
  if (cursor == nullptr || out == nullptr || outSize < 2) {
    if (error != nullptr) {
      *error = "invalid arguments";
    }
    return SerialCliTokenResult::ERROR;
  }
  while (*cursor != '\0' && serialCliIsSpace(*cursor)) {
    ++cursor;
  }
  if (*cursor == '\0') {
    out[0] = '\0';
    return SerialCliTokenResult::END;
  }

  size_t length = 0;
  if (*cursor == '"') {
    ++cursor;
    while (*cursor != '\0' && *cursor != '"') {
      if (length + 1 >= outSize) {
        out[0] = '\0';
        if (error != nullptr) {
          *error = "argument too long";
        }
        return SerialCliTokenResult::ERROR;
      }
      out[length++] = *cursor++;
    }
    if (*cursor != '"') {
      out[0] = '\0';
      if (error != nullptr) {
        *error = "unclosed quote";
      }
      return SerialCliTokenResult::ERROR;
    }
    ++cursor;
    out[length] = '\0';
    return SerialCliTokenResult::OK;
  }

  while (*cursor != '\0' && !serialCliIsSpace(*cursor) && *cursor != '"') {
    if (length + 1 >= outSize) {
      out[0] = '\0';
      if (error != nullptr) {
        *error = "argument too long";
      }
      return SerialCliTokenResult::ERROR;
    }
    out[length++] = *cursor++;
  }
  out[length] = '\0';
  return SerialCliTokenResult::OK;
}

inline bool serialCliParseLine(const char *line, SerialCliRequest &request) {
  serialCliClearRequest(request);
  if (line == nullptr) {
    request.verb = SerialCliVerb::INVALID_ARGS;
    request.error = "invalid arguments";
    return true;
  }

  const char *cursor = line;
  char verb[SERIAL_CLI_VERB_CAPACITY] = {};
  const char *tokenError = nullptr;
  const SerialCliTokenResult verbResult =
      serialCliNextToken(cursor, verb, sizeof(verb), &tokenError);
  if (verbResult == SerialCliTokenResult::END) {
    return false;
  }
  if (verbResult != SerialCliTokenResult::OK) {
    request.verb = SerialCliVerb::INVALID_ARGS;
    request.error = tokenError != nullptr ? tokenError : "invalid arguments";
    return true;
  }

  char args[2][WIFI_PASSWORD_CAPACITY] = {};
  size_t argCount = 0;
  while (argCount < 2) {
    tokenError = nullptr;
    const SerialCliTokenResult result = serialCliNextToken(
        cursor, args[argCount], sizeof(args[argCount]), &tokenError);
    if (result == SerialCliTokenResult::END) {
      break;
    }
    if (result != SerialCliTokenResult::OK) {
      request.verb = SerialCliVerb::INVALID_ARGS;
      request.error = tokenError != nullptr ? tokenError : "invalid arguments";
      return true;
    }
    ++argCount;
  }

  char extra[4] = {};
  tokenError = nullptr;
  if (serialCliNextToken(cursor, extra, sizeof(extra), &tokenError) !=
      SerialCliTokenResult::END) {
    request.verb = SerialCliVerb::INVALID_ARGS;
    request.error = tokenError != nullptr ? tokenError : "too many arguments";
    return true;
  }

  auto requireNoArgs = [&](SerialCliVerb verbId) {
    if (argCount != 0) {
      request.verb = SerialCliVerb::INVALID_ARGS;
      request.error = "command takes no arguments";
      return false;
    }
    request.verb = verbId;
    return true;
  };

  if (serialCliEqualsIgnoreCase(verb, "help")) {
    return requireNoArgs(SerialCliVerb::HELP);
  }
  if (serialCliEqualsIgnoreCase(verb, "hello")) {
    return requireNoArgs(SerialCliVerb::HELLO);
  }
  if (serialCliEqualsIgnoreCase(verb, "reboot")) {
    return requireNoArgs(SerialCliVerb::REBOOT);
  }
  if (serialCliEqualsIgnoreCase(verb, "factory_reset")) {
    return requireNoArgs(SerialCliVerb::FACTORY_RESET);
  }
  if (serialCliEqualsIgnoreCase(verb, "reset_device_password")) {
    return requireNoArgs(SerialCliVerb::RESET_DEVICE_PASSWORD);
  }
  if (serialCliEqualsIgnoreCase(verb, "clear_shots")) {
    return requireNoArgs(SerialCliVerb::CLEAR_SHOTS);
  }
  if (serialCliEqualsIgnoreCase(verb, "clear_wifi")) {
    return requireNoArgs(SerialCliVerb::CLEAR_WIFI);
  }
  if (serialCliEqualsIgnoreCase(verb, "reset_network_ap")) {
    return requireNoArgs(SerialCliVerb::RESET_NETWORK_AP);
  }
  if (serialCliEqualsIgnoreCase(verb, "serial_debug_on")) {
    return requireNoArgs(SerialCliVerb::SERIAL_DEBUG_ON);
  }
  if (serialCliEqualsIgnoreCase(verb, "serial_debug_off")) {
    return requireNoArgs(SerialCliVerb::SERIAL_DEBUG_OFF);
  }
  if (serialCliEqualsIgnoreCase(verb, "debug_full")) {
    return requireNoArgs(SerialCliVerb::DEBUG_FULL);
  }
  if (serialCliEqualsIgnoreCase(verb, "debug_off")) {
    return requireNoArgs(SerialCliVerb::DEBUG_OFF);
  }
  if (serialCliEqualsIgnoreCase(verb, "debug_status")) {
    return requireNoArgs(SerialCliVerb::DEBUG_STATUS);
  }
  if (serialCliEqualsIgnoreCase(verb, "wifi_connect")) {
    return requireNoArgs(SerialCliVerb::WIFI_CONNECT);
  }
  if (serialCliEqualsIgnoreCase(verb, "wifi_disconnect")) {
    return requireNoArgs(SerialCliVerb::WIFI_DISCONNECT);
  }
  if (serialCliEqualsIgnoreCase(verb, "wifi_restart")) {
    return requireNoArgs(SerialCliVerb::WIFI_RESTART);
  }
  if (serialCliEqualsIgnoreCase(verb, "wifi_status")) {
    return requireNoArgs(SerialCliVerb::WIFI_STATUS);
  }
  if (serialCliEqualsIgnoreCase(verb, "ap_start")) {
    return requireNoArgs(SerialCliVerb::AP_START);
  }
  if (serialCliEqualsIgnoreCase(verb, "ap_stop")) {
    return requireNoArgs(SerialCliVerb::AP_STOP);
  }
  if (serialCliEqualsIgnoreCase(verb, "ap_status")) {
    return requireNoArgs(SerialCliVerb::AP_STATUS);
  }
  if (serialCliEqualsIgnoreCase(verb, "webui_start")) {
    return requireNoArgs(SerialCliVerb::WEBUI_START);
  }
  if (serialCliEqualsIgnoreCase(verb, "webui_stop")) {
    return requireNoArgs(SerialCliVerb::WEBUI_STOP);
  }
  if (serialCliEqualsIgnoreCase(verb, "webui_restart")) {
    return requireNoArgs(SerialCliVerb::WEBUI_RESTART);
  }
  if (serialCliEqualsIgnoreCase(verb, "webui_status")) {
    return requireNoArgs(SerialCliVerb::WEBUI_STATUS);
  }
  if (serialCliEqualsIgnoreCase(verb, "net_status")) {
    return requireNoArgs(SerialCliVerb::NET_STATUS);
  }
  if (serialCliEqualsIgnoreCase(verb, "log_dump")) {
    return requireNoArgs(SerialCliVerb::LOG_DUMP);
  }
  if (serialCliEqualsIgnoreCase(verb, "health")) {
    return requireNoArgs(SerialCliVerb::HEALTH);
  }
  if (serialCliEqualsIgnoreCase(verb, "scale_status")) {
    return requireNoArgs(SerialCliVerb::SCALE_STATUS);
  }
  if (serialCliEqualsIgnoreCase(verb, "ntp_status")) {
    return requireNoArgs(SerialCliVerb::NTP_STATUS);
  }
  if (serialCliEqualsIgnoreCase(verb, "heap")) {
    return requireNoArgs(SerialCliVerb::HEAP);
  }
  if (serialCliEqualsIgnoreCase(verb, "boot_heap")) {
    return requireNoArgs(SerialCliVerb::BOOT_HEAP);
  }
  if (serialCliEqualsIgnoreCase(verb, "audit_dump")) {
    if (argCount > 1) {
      request.verb = SerialCliVerb::INVALID_ARGS;
      request.error = "AUDIT_DUMP takes an optional record count";
      return true;
    }
    request.verb = SerialCliVerb::AUDIT_DUMP;
    if (argCount == 1) copyCString(request.arg1, sizeof(request.arg1), args[0]);
    return true;
  }
  if (serialCliEqualsIgnoreCase(verb, "audit_clear")) {
    return requireNoArgs(SerialCliVerb::AUDIT_CLEAR);
  }
  const bool auditVerbose = serialCliEqualsIgnoreCase(verb, "audit_verbose");
  if (auditVerbose || serialCliEqualsIgnoreCase(verb, "task_profile")) {
    if (argCount > 1 ||
        (argCount == 1 && !serialCliEqualsIgnoreCase(args[0], "on") &&
         !serialCliEqualsIgnoreCase(args[0], "off"))) {
      request.verb = SerialCliVerb::INVALID_ARGS;
      request.error = auditVerbose ? "AUDIT_VERBOSE takes an optional on/off argument"
                                   : "TASK_PROFILE takes an optional on/off argument";
      return true;
    }
    request.verb = auditVerbose ? SerialCliVerb::AUDIT_VERBOSE : SerialCliVerb::TASK_PROFILE;
    if (argCount == 1) copyCString(request.arg1, sizeof(request.arg1), args[0]);
    return true;
  }

  if (serialCliEqualsIgnoreCase(verb, "set_device_password")) {
    if (argCount != 1) {
      request.verb = SerialCliVerb::INVALID_ARGS;
      request.error = "SET_DEVICE_PASSWORD requires a password";
      return true;
    }
    if (!validDevicePassword(args[0])) {
      request.verb = SerialCliVerb::INVALID_ARGS;
      request.error = "Device password must be 8-63 characters";
      return true;
    }
    if (isFactoryDefaultPassword(args[0])) {
      request.verb = SerialCliVerb::INVALID_ARGS;
      request.error =
          "password cannot be the factory default; use RESET_DEVICE_PASSWORD";
      return true;
    }
    request.verb = SerialCliVerb::SET_DEVICE_PASSWORD;
    copyCString(request.arg1, sizeof(request.arg1), args[0]);
    return true;
  }

  if (serialCliEqualsIgnoreCase(verb, "set_wifi")) {
    if (argCount < 1 || argCount > 2) {
      request.verb = SerialCliVerb::INVALID_ARGS;
      request.error = "SET_WIFI requires SSID and optional password";
      return true;
    }
    if (!validWifiSsid(args[0])) {
      request.verb = SerialCliVerb::INVALID_ARGS;
      request.error = "Wi-Fi SSID must be 1-32 characters";
      return true;
    }
    const bool openNetwork = argCount == 1 || args[1][0] == '\0';
    if (!validWifiPassword(openNetwork ? "" : args[1], openNetwork)) {
      request.verb = SerialCliVerb::INVALID_ARGS;
      request.error = "Wi-Fi password must be 8-63 characters";
      return true;
    }
    request.verb = SerialCliVerb::SET_WIFI;
    copyCString(request.arg1, sizeof(request.arg1), args[0]);
    if (!openNetwork) {
      copyCString(request.arg2, sizeof(request.arg2), args[1]);
    }
    request.openNetwork = openNetwork;
    return true;
  }

  request.verb = SerialCliVerb::UNKNOWN;
  request.error = "unknown command; try HELP";
  return true;
}

inline void serialCliPrintHelp() {
  Serial.println(
      "Commands are case-insensitive. Destructive need paddle OFF, machine circuit open, "
      "Ready, no cycle.");
  Serial.println("HELP  list commands  e.g. HELP");
  Serial.println("HELLO  probe CLI  e.g. HELLO");
  Serial.println("REBOOT  restart firmware  e.g. REBOOT");
  Serial.println(
      "FACTORY_RESET  wipe Wi-Fi, settings, shots; device password ineedacoffee; "
      "restart  e.g. FACTORY_RESET");
  Serial.println(
      "RESET_DEVICE_PASSWORD  restore device password ineedacoffee (STA Wi-Fi "
      "unchanged)  e.g. RESET_DEVICE_PASSWORD");
  Serial.println(
      "SET_DEVICE_PASSWORD <password>  set device password (8-63 chars, not "
      "ineedacoffee; USB does not require the current password)  e.g. "
      "SET_DEVICE_PASSWORD password1234");
  Serial.println(
      "SET_WIFI <ssid> [password]  save STA Wi-Fi (DHCP), commit, and restart; "
      "omit password if open; quote spaces  e.g. SET_WIFI CafeLAN CafePass1");
  Serial.println("CLEAR_WIFI  forget STA Wi-Fi only; restart  e.g. CLEAR_WIFI");
  Serial.println("CLEAR_SHOTS  clear shot history  e.g. CLEAR_SHOTS");
  Serial.println(
      "RESET_NETWORK_AP  forget STA Wi-Fi and restore device password "
      "ineedacoffee; restart  e.g. RESET_NETWORK_AP");
  Serial.println(
      "SERIAL_DEBUG_ON  enable USB debug traces (any time)  e.g. "
      "SERIAL_DEBUG_ON");
  Serial.println(
      "SERIAL_DEBUG_OFF  disable USB debug traces; CLI replies stay on  e.g. "
      "SERIAL_DEBUG_OFF");
  Serial.println(
      "DEBUG_FULL  serial debug + ring DEBUG (persists, any time)  e.g. "
      "DEBUG_FULL");
  Serial.println(
      "DEBUG_OFF  serial off and ring none (persists, any time)  e.g. "
      "DEBUG_OFF");
  Serial.println(
      "DEBUG_STATUS  show serialLogLevel, ringRetainLogLevel  e.g. "
      "DEBUG_STATUS");
  Serial.println(
      "WIFI_CONNECT  associate saved STA (no NVS change)  e.g. WIFI_CONNECT");
  Serial.println(
      "WIFI_DISCONNECT  drop STA and hold reconnect  e.g. WIFI_DISCONNECT");
  Serial.println(
      "WIFI_RESTART  drop then reconnect saved STA  e.g. WIFI_RESTART");
  Serial.println("WIFI_STATUS  dump STA config and link  e.g. WIFI_STATUS");
  Serial.println(
      "AP_START  raise SoftAP (stays up with STA)  e.g. AP_START");
  Serial.println("AP_STOP  stop SoftAP and hold auto-raise  e.g. AP_STOP");
  Serial.println("AP_STATUS  dump SoftAP state  e.g. AP_STATUS");
  Serial.println("WEBUI_START  start HTTP server  e.g. WEBUI_START");
  Serial.println("WEBUI_STOP  stop HTTP and hold auto-start  e.g. WEBUI_STOP");
  Serial.println("WEBUI_RESTART  bounce HTTP server  e.g. WEBUI_RESTART");
  Serial.println("WEBUI_STATUS  dump HTTP/Web UI state  e.g. WEBUI_STATUS");
  Serial.println("NET_STATUS  WIFI + AP + WEBUI status  e.g. NET_STATUS");
  Serial.println(
      "LOG_DUMP  print RAM debug ring (deferred in brew/machine circuit)  e.g. LOG_DUMP");
  Serial.println("HEALTH  heap, loop gap, cpu load, task stacks  e.g. HEALTH");
  Serial.println("AUDIT_DUMP [n]  curated audit log, newest n (default 24)  e.g. AUDIT_DUMP 10");
  Serial.println("AUDIT_CLEAR  erase the durable audit log  e.g. AUDIT_CLEAR");
  Serial.println("AUDIT_VERBOSE [on|off]  30-minute verbose hunt session  e.g. AUDIT_VERBOSE on");
  Serial.println("SCALE_STATUS  BLE scale link dump  e.g. SCALE_STATUS");
  Serial.println("NTP_STATUS  wall clock and NTP dump  e.g. NTP_STATUS");
  Serial.println(
      "HEAP  internal heap free-block layout  e.g. HEAP");
  Serial.println("BOOT_HEAP  retained startup memory capture  e.g. BOOT_HEAP");
  Serial.println("TASK_PROFILE [on|off]  task stacks/CPU, loop-phase peaks  e.g. TASK_PROFILE on");
}

inline const char *serialCliWifiModeName(uint8_t mode) {
  switch (mode) {
    case 0: return "OFF";
    case 1: return "STA";
    case 2: return "AP";
    case 3: return "AP_STA";
    default: return "UNKNOWN";
  }
}

inline const char *serialCliWlStatusName(int32_t status) {
  switch (status) {
    case 0: return "IDLE";
    case 1: return "NO_SSID_AVAIL";
    case 2: return "SCAN_COMPLETED";
    case 3: return "CONNECTED";
    case 4: return "CONNECT_FAILED";
    case 5: return "CONNECTION_LOST";
    case 6: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

inline const char *serialCliStaStateName(uint8_t state) {
  switch (state) {
    case 0: return "NOT_CONFIGURED";
    case 1: return "CONNECTING";
    case 2: return "CONNECTED";
    case 3: return "FAILED";
    case 4: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

inline const char *serialCliScanStateName(uint8_t state) {
  switch (state) {
    case 0: return "IDLE";
    case 1: return "QUEUED";
    case 2: return "RUNNING";
    case 3: return "READY";
    case 4: return "FAILED";
    case 5: return "CANCELED";
    default: return "UNKNOWN";
  }
}

struct SerialCliNetworkDump {
  bool networkActive = false;
  bool apActive = false;
  bool httpActive = false;
  bool wifiConfigured = false;
  bool staOpen = false;
  bool staLinkMetricsValid = false;
  bool staReconnectHeld = false;
  bool apStartHeld = false;
  bool httpStartHeld = false;
  bool devicePasswordFactory = false;
  WifiPsLive wifiPs = WifiPsLive::UNKNOWN;
  bool ntpMayArm = false;
  uint8_t apClients = 0;
  uint8_t staState = 0;
  uint8_t staIpMode = 0;
  uint8_t staConfigState = 0;
  uint8_t wifiMode = 0;
  uint8_t channel = 0;
  uint8_t scanState = 0;
  uint8_t ntpState = 0;
  int8_t staRssi = 0;
  uint8_t staSignalQualityPct = 0;
  int32_t wifiStatus = 6;
  uint32_t confirmRemainingMs = 0;
  uint32_t taskAgeMs = 0;
  uint32_t taskStackMinBytes = UINT32_MAX;
  uint32_t startupFailures = 0;
  uint32_t lastCommandRequestId = 0;
  uint32_t staConnectAgeMs = 0;
  uint32_t staReconnectAgeMs = 0;
  uint32_t staReconnectDelayMs = 0;
  CommandResultState lastCommandState = CommandResultState::NONE;
  NetworkInterfaceSnapshot iface = {};
};

struct SerialCliHealthDump {
  uint32_t freeHeapBytes = 0;
  uint32_t minimumFreeHeapBytes = 0;
  uint32_t largestFreeHeapBlockBytes = 0;
  uint32_t internalHeapAllocatedBlocks = 0;
  uint32_t internalHeapFreeBlocks = 0;
  uint16_t internalHeapFragmentationPermille = 0;
  uint32_t psramSizeBytes = 0;
  uint32_t psramFreeBytes = 0;
  uint32_t psramLargestFreeBlockBytes = 0;
  uint8_t bleBackend = 0;
  uint8_t bleRuntimeState = 0;
  int32_t bleRuntimeLastError = 0;
  int32_t bleRuntimeLastResetReason = 0;
  uint32_t bleRuntimeSyncGeneration = 0;
  uint32_t bleRuntimeResetCount = 0;
  uint32_t bleRuntimeHostStackMinBytes = UINT32_MAX;
  bool workBufExternal = false;
  bool jsonArenaExternal = false;
  uint32_t allocExternalFallbackCount = 0;
  uint32_t loopMaxGapMs = 0;
  uint32_t healthIntervalMaxGapMs = 0;
  uint32_t loopStackMinBytes = UINT32_MAX;
  uint32_t scaleWorkerStackMinBytes = UINT32_MAX;
  uint32_t networkStackMinBytes = UINT32_MAX;
  bool heapAlertLatched = false;
  bool stackAlertLatched = false;
  bool loopGapAlertLatched = false;
  bool cpuLoadValid = false;
  bool tempValid = false;
  uint32_t cpuMhz = 0;
  char flashReadMode[7] = "";
  char flashReadModeBuilt[7] = "";
  float cpuLoad5s = 0.0f;
  float cpuLoad1m = 0.0f;
  float cpuLoad5m = 0.0f;
  float cpu0Busy = 0.0f;
  float cpu1Busy = 0.0f;
  float tempC = 0.0f;
  float tempPeakC = 0.0f;
};

struct SerialCliScaleDump {
  const char *state = "DISCONNECTED";
  const char *protocolName = "";
  char preferredMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  char preferredName[PREFERRED_SCALE_NAME_CAPACITY] = {};
  uint32_t disconnectSequence = 0;
  uint32_t connectionGeneration = 0;
  uint32_t packetSequence = 0;
  uint32_t packetGaps = 0;
  uint32_t rejectedPackets = 0;
  uint32_t reconnects = 0;
  uint32_t recoveredStaleCount = 0;
  uint32_t recoveredStaleMs = 0;
  uint8_t lastDisconnectReason = 0;
  bool rssiValid = false;
  int8_t rssi = 0;
  uint32_t workerAgeMs = 0;
  bool timerValid = false;
  uint32_t timerMs = 0;
  uint32_t timerAgeMs = 0;
  bool weightFresh = false;
  float currentWeightG = 0.0f;
  const char *scanIntensity = bleScanIntensityName(BLE_SCAN_FACTORY_INTENSITY);
};

struct SerialCliNtpDump {
  TimeSyncState state = TimeSyncState::OFF;
  uint32_t utcSec = 0;
  uint32_t lastSyncAgeMs = 0;
  uint32_t nextRetryInMs = 0;
  uint8_t consecutiveFailures = 0;
  char activeServer[NTP_SERVER_HOST_CAPACITY] = {};
  uint8_t ntpServerPreset = 0;
  char ntpServerCustom[NTP_SERVER_HOST_CAPACITY] = {};
  char timezoneId[TIMEZONE_ID_CAPACITY] = {};
  int16_t timezoneOffsetMinutes = 0;
  bool staUp = false;
};

inline void serialCliPrintWifiStatus(const SerialCliNetworkDump &dump) {
  Serial.println("WIFI_STATUS");
  Serial.print("configured=");
  Serial.println(dump.wifiConfigured ? "true" : "false");
  Serial.print("ssid=");
  Serial.println(dump.iface.staSsid[0] != '\0' ? dump.iface.staSsid : "-");
  Serial.print("security=");
  Serial.println(!dump.wifiConfigured ? "-" : (dump.staOpen ? "open" : "wpa"));
  Serial.print("staState=");
  Serial.println(serialCliStaStateName(dump.staState));
  Serial.print("wifiStatus=");
  Serial.print(dump.wifiStatus);
  Serial.print(" ");
  Serial.println(serialCliWlStatusName(dump.wifiStatus));
  Serial.print("wifiMode=");
  Serial.println(serialCliWifiModeName(dump.wifiMode));
  Serial.print("ipMode=");
  Serial.println(staIpModeName(dump.staIpMode));
  Serial.print("configState=");
  Serial.println(staConfigStateName(dump.staConfigState));
  Serial.print("staIp=");
  Serial.println(dump.iface.staIp[0] != '\0' ? dump.iface.staIp : "-");
  Serial.print("configuredIp=");
  Serial.println(dump.iface.configuredIp[0] != '\0' ? dump.iface.configuredIp : "-");
  Serial.print("netmask=");
  Serial.println(dump.iface.configuredNetmask[0] != '\0' ? dump.iface.configuredNetmask
                                                   : "-");
  Serial.print("gateway=");
  Serial.println(dump.iface.configuredGateway[0] != '\0' ? dump.iface.configuredGateway
                                                   : "-");
  Serial.print("dns1=");
  Serial.println(dump.iface.configuredDns1[0] != '\0' ? dump.iface.configuredDns1 : "-");
  Serial.print("dns2=");
  Serial.println(dump.iface.configuredDns2[0] != '\0' ? dump.iface.configuredDns2 : "-");
  Serial.print("rssi=");
  if (dump.staLinkMetricsValid) {
    Serial.print(static_cast<int>(dump.staRssi));
    Serial.print(" quality=");
    Serial.print(static_cast<unsigned>(dump.staSignalQualityPct));
    Serial.println("%");
  } else {
    Serial.println("-");
  }
  Serial.print("channel=");
  Serial.println(static_cast<unsigned>(dump.channel));
  Serial.print("staBssid=");
  Serial.println(dump.iface.staBssid[0] != '\0' ? dump.iface.staBssid : "-");
  Serial.print("staMac=");
  Serial.println(dump.iface.staMac[0] != '\0' ? dump.iface.staMac : "-");
  Serial.print("confirmRemainingMs=");
  Serial.println(static_cast<unsigned long>(dump.confirmRemainingMs));
  Serial.print("staConnectAgeMs=");
  Serial.println(static_cast<unsigned long>(dump.staConnectAgeMs));
  Serial.print("staReconnectAgeMs=");
  Serial.println(static_cast<unsigned long>(dump.staReconnectAgeMs));
  Serial.print("staReconnectDelayMs=");
  Serial.println(static_cast<unsigned long>(dump.staReconnectDelayMs));
  Serial.print("startupFailures=");
  Serial.println(static_cast<unsigned long>(dump.startupFailures));
  Serial.print("scanState=");
  Serial.println(serialCliScanStateName(dump.scanState));
  Serial.print("lastCommand=");
  Serial.print(static_cast<unsigned long>(dump.lastCommandRequestId));
  Serial.print(" ");
  Serial.println(commandResultStateName(dump.lastCommandState));
  Serial.print("ntpServer=");
  Serial.println(dump.iface.ntpActiveServer[0] != '\0' ? dump.iface.ntpActiveServer : "-");
  Serial.print("ntpMayArm=");
  Serial.println(dump.ntpMayArm ? "true" : "false");
  Serial.print("staReconnectHeld=");
  Serial.println(dump.staReconnectHeld ? "true" : "false");
}

inline void serialCliPrintApStatus(const SerialCliNetworkDump &dump) {
  Serial.println("AP_STATUS");
  Serial.print("active=");
  Serial.println(dump.apActive ? "true" : "false");
  Serial.print("ssid=");
  Serial.println(dump.iface.apSsid[0] != '\0' ? dump.iface.apSsid : SOFT_AP_SSID_PREFIX);
  Serial.print("ip=");
  Serial.println(dump.iface.apIp[0] != '\0' ? dump.iface.apIp : "192.168.4.1");
  Serial.print("clients=");
  Serial.println(static_cast<unsigned>(dump.apClients));
  Serial.print("apMac=");
  Serial.println(dump.iface.apMac[0] != '\0' ? dump.iface.apMac : "-");
  Serial.print("wifiMode=");
  Serial.println(serialCliWifiModeName(dump.wifiMode));
  Serial.print("passwordFactory=");
  Serial.println(dump.devicePasswordFactory ? "true" : "false");
  Serial.print("wifiPs=");
  Serial.println(wifiPsLiveName(dump.wifiPs));
  Serial.print("apStartHeld=");
  Serial.println(dump.apStartHeld ? "true" : "false");
}

inline void serialCliPrintWebuiStatus(const SerialCliNetworkDump &dump) {
  Serial.println("WEBUI_STATUS");
  Serial.print("httpActive=");
  Serial.println(dump.httpActive ? "true" : "false");
  Serial.print("networkActive=");
  Serial.println(dump.networkActive ? "true" : "false");
  Serial.print("bindSta=");
  Serial.println(dump.iface.staIp[0] != '\0' ? dump.iface.staIp : "-");
  Serial.print("bindAp=");
  Serial.println(dump.iface.apIp[0] != '\0' ? dump.iface.apIp : "192.168.4.1");
  Serial.print("lastCommand=");
  Serial.print(static_cast<unsigned long>(dump.lastCommandRequestId));
  Serial.print(" ");
  Serial.println(commandResultStateName(dump.lastCommandState));
  Serial.print("taskAgeMs=");
  Serial.println(static_cast<unsigned long>(dump.taskAgeMs));
  Serial.println("stackUnit=bytes stackUnavailable=4294967295");
  Serial.print("taskStackMinWords=");
  // Legacy keys retain their numeric byte values; name the unit explicitly.
  Serial.println(static_cast<unsigned long>(dump.taskStackMinBytes));
  Serial.print("startupFailures=");
  Serial.println(static_cast<unsigned long>(dump.startupFailures));
  Serial.print("httpStartHeld=");
  Serial.println(dump.httpStartHeld ? "true" : "false");
}

inline void serialCliPrintNetStatus(const SerialCliNetworkDump &dump) {
  serialCliPrintWifiStatus(dump);
  serialCliPrintApStatus(dump);
  serialCliPrintWebuiStatus(dump);
}

inline void serialCliPrintDebugStatus(LogLevel serialLevel,
                                      LogLevel ringLevel) {
  Serial.println("DEBUG_STATUS");
  Serial.print("serialLogLevel=");
  Serial.println(logLevelName(serialLevel));
  Serial.print("ringRetainLogLevel=");
  Serial.println(logLevelName(ringLevel));
}

inline void serialCliPrintHealth(const SerialCliHealthDump &dump) {
  Serial.println("HEALTH");
  Serial.println("stackUnit=bytes stackUnavailable=4294967295");
  Serial.print("heapFree=");
  Serial.println(static_cast<unsigned long>(dump.freeHeapBytes));
  Serial.print("heapMinFree=");
  Serial.println(static_cast<unsigned long>(dump.minimumFreeHeapBytes));
  Serial.print("heapLargest=");
  Serial.println(static_cast<unsigned long>(dump.largestFreeHeapBlockBytes));
  Serial.print("heapAllocatedBlocks=");
  Serial.println(static_cast<unsigned long>(dump.internalHeapAllocatedBlocks));
  Serial.print("heapFreeBlocks=");
  Serial.println(static_cast<unsigned long>(dump.internalHeapFreeBlocks));
  Serial.print("heapFragmentationPermille=");
  Serial.println(
      static_cast<unsigned>(dump.internalHeapFragmentationPermille));
  Serial.print("psramSize=");
  Serial.println(static_cast<unsigned long>(dump.psramSizeBytes));
  Serial.print("psramFree=");
  Serial.println(static_cast<unsigned long>(dump.psramFreeBytes));
  Serial.print("psramLargest=");
  Serial.println(static_cast<unsigned long>(dump.psramLargestFreeBlockBytes));
  Serial.print("bleBackend=");
  Serial.println("nimble");
  Serial.print("bleRuntimeState=");
  Serial.println(static_cast<unsigned>(dump.bleRuntimeState));
  Serial.print("bleRuntimeLastError=");
  Serial.println(static_cast<long>(dump.bleRuntimeLastError));
  Serial.print("bleRuntimeLastResetReason=");
  Serial.println(static_cast<long>(dump.bleRuntimeLastResetReason));
  Serial.print("bleRuntimeSyncGeneration=");
  Serial.println(static_cast<unsigned long>(dump.bleRuntimeSyncGeneration));
  Serial.print("bleRuntimeResetCount=");
  Serial.println(static_cast<unsigned long>(dump.bleRuntimeResetCount));
  Serial.print("bleRuntimeHostStackMinWords=");
  Serial.println(
      static_cast<unsigned long>(dump.bleRuntimeHostStackMinBytes));
  Serial.print("workBufExternal=");
  Serial.println(dump.workBufExternal ? "true" : "false");
  Serial.print("jsonArenaExternal=");
  Serial.println(dump.jsonArenaExternal ? "true" : "false");
  Serial.print("allocExternalFallback=");
  Serial.println(static_cast<unsigned long>(dump.allocExternalFallbackCount));
  Serial.print("loopMaxGapMs=");
  Serial.println(static_cast<unsigned long>(dump.loopMaxGapMs));
  Serial.print("loopIntervalGapMs=");
  Serial.println(static_cast<unsigned long>(dump.healthIntervalMaxGapMs));
  Serial.print("stackLoop=");
  Serial.println(static_cast<unsigned long>(dump.loopStackMinBytes));
  Serial.print("stackScale=");
  Serial.println(static_cast<unsigned long>(dump.scaleWorkerStackMinBytes));
  Serial.print("stackNetwork=");
  Serial.println(static_cast<unsigned long>(dump.networkStackMinBytes));
  Serial.print("alertHeap=");
  Serial.println(dump.heapAlertLatched ? "true" : "false");
  Serial.print("alertStack=");
  Serial.println(dump.stackAlertLatched ? "true" : "false");
  Serial.print("alertLoopGap=");
  Serial.println(dump.loopGapAlertLatched ? "true" : "false");
  Serial.print("cpuLoadValid=");
  Serial.println(dump.cpuLoadValid ? "true" : "false");
  Serial.print("cpuMhz=");
  Serial.println(static_cast<unsigned long>(dump.cpuMhz));
  Serial.print("flashReadMode=");
  Serial.println(dump.flashReadMode);
  Serial.print("flashReadModeBuilt=");
  Serial.println(dump.flashReadModeBuilt);
  Serial.print("cpuLoad5s=");
  Serial.println(dump.cpuLoad5s, 2);
  Serial.print("cpuLoad1m=");
  Serial.println(dump.cpuLoad1m, 2);
  Serial.print("cpuLoad5m=");
  Serial.println(dump.cpuLoad5m, 2);
  Serial.print("cpu0Busy=");
  Serial.println(dump.cpu0Busy, 2);
  Serial.print("cpu1Busy=");
  Serial.println(dump.cpu1Busy, 2);
  Serial.print("tempValid=");
  Serial.println(dump.tempValid ? "true" : "false");
  Serial.print("tempC=");
  Serial.println(dump.tempC, 1);
  Serial.print("tempPeakC=");
  Serial.println(dump.tempPeakC, 1);
}

inline void serialCliPrintHeap(const HeapCapSnapshot &snapshot,
                               const HeapFreeBlockList &blocks) {
  Serial.println("HEAP");
  Serial.println("freeBlockUnit=bytes addressUnit=hex");
  Serial.print("internalTotal=");
  Serial.println(static_cast<unsigned long>(snapshot.internalTotal));
  Serial.print("internalFree=");
  Serial.println(static_cast<unsigned long>(snapshot.internalFree));
  Serial.print("internalMinimum=");
  Serial.println(static_cast<unsigned long>(snapshot.internalMinimum));
  Serial.print("internalLargest=");
  Serial.println(static_cast<unsigned long>(snapshot.internalLargest));
  Serial.print("internalFreeBlocks=");
  Serial.println(static_cast<unsigned long>(snapshot.internalFreeBlocks));
  Serial.print("internalFragmentationPermille=");
  Serial.println(
      static_cast<unsigned>(snapshot.internalFragmentationPermille));
  Serial.print("freeBlocksWalked=");
  Serial.println(static_cast<unsigned long>(blocks.walkedFreeBlocks));
  for (uint32_t i = 0; i < blocks.listedCount &&
                       i < HEAP_FREE_BLOCK_SAMPLE_CAPACITY; ++i) {
    char start[11] = {};
    snprintf(start, sizeof(start), "0x%lx",
             static_cast<unsigned long>(blocks.blocks[i].startAddress));
    Serial.print("freeBlock");
    Serial.print(static_cast<unsigned long>(i));
    Serial.print("Size=");
    Serial.print(static_cast<unsigned long>(blocks.blocks[i].sizeBytes));
    Serial.print(" freeBlock");
    Serial.print(static_cast<unsigned long>(i));
    Serial.print("Start=");
    Serial.println(start);
  }
  if (blocks.walkedFreeBlocks > blocks.listedCount) {
    Serial.print("freeBlocksTruncated=");
    Serial.println(static_cast<unsigned long>(blocks.walkedFreeBlocks -
                                              blocks.listedCount));
  }
}

inline void serialCliPrintBootHeap() {
  BootHeapMinimum minima[2];
  uint32_t stages;
  bool complete;
  {
    TaskLockGuard lock(bootHeapMutex);
    stages = bootHeapCapture.stages;
    complete = bootHeapCapture.complete;
    minima[0] = bootHeapCapture.post;
    minima[1] = bootHeapCapture.ws;
  }
  Serial.println("BOOT_HEAP bytes=1 time=ms_since_boot");
  char line[192];
  snprintf(line, sizeof(line), "status=%s postWindowMs=%lu wsWindowMs=%lu",
           stages == 0 ? "not_started" : complete ? "complete" : "recording",
           static_cast<unsigned long>(BOOT_HEAP_WINDOW_MS),
           static_cast<unsigned long>(BOOT_HEAP_WINDOW_MS));
  Serial.println(line);
  Serial.println("stage,result,ms,free,largest,allocated,blocks,dmaFree,dmaLargest,psramFree,psramLargest");
  for (size_t i = 0; i < static_cast<size_t>(BootHeapStage::COUNT); ++i) {
    if (!(stages & (1U << i))) continue;
    BootHeapRecord record;
    {
      TaskLockGuard lock(bootHeapMutex);
      record = bootHeapCapture.records[i];
    }
    snprintf(line, sizeof(line), "%s,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
             bootHeapStageName(static_cast<BootHeapStage>(i)), bootHeapResultName(record.result),
             static_cast<unsigned long>(record.atMs),
             static_cast<unsigned long>(record.internal.freeBytes),
             static_cast<unsigned long>(record.internal.largestBlock),
             static_cast<unsigned long>(record.internal.allocatedBlocks),
             static_cast<unsigned long>(record.internal.freeBlocks),
             static_cast<unsigned long>(record.dmaFree), static_cast<unsigned long>(record.dmaLargest),
             static_cast<unsigned long>(record.psramFree), static_cast<unsigned long>(record.psramLargest));
    Serial.println(line);
  }
  constexpr const char *minimumNames[] = {"post_min", "ws_min"};
  for (size_t i = 0; i < 2; ++i) {
    snprintf(line, sizeof(line), "%s samples=%lu free=%lu largest=%lu dmaFree=%lu dmaLargest=%lu",
             minimumNames[i], static_cast<unsigned long>(minima[i].samples),
             static_cast<unsigned long>(minima[i].free), static_cast<unsigned long>(minima[i].largest),
             static_cast<unsigned long>(minima[i].dmaFree), static_cast<unsigned long>(minima[i].dmaLargest));
    Serial.println(line);
  }
}

inline void serialCliPrintScaleStatus(const SerialCliScaleDump &dump) {
  Serial.println("SCALE_STATUS");
  Serial.print("state=");
  Serial.println(dump.state != nullptr ? dump.state : "-");
  Serial.print("protocol=");
  Serial.println(dump.protocolName != nullptr && dump.protocolName[0] != '\0'
                     ? dump.protocolName
                     : "-");
  Serial.print("preferredMac=");
  Serial.println(dump.preferredMac[0] != '\0' ? dump.preferredMac : "-");
  Serial.print("preferredName=");
  Serial.println(dump.preferredName[0] != '\0' ? dump.preferredName : "-");
  Serial.print("reconnects=");
  Serial.println(static_cast<unsigned long>(dump.reconnects));
  Serial.print("recoveredStaleCount=");
  Serial.println(static_cast<unsigned long>(dump.recoveredStaleCount));
  Serial.print("recoveredStaleMs=");
  Serial.println(static_cast<unsigned long>(dump.recoveredStaleMs));
  Serial.print("packetGaps=");
  Serial.println(static_cast<unsigned long>(dump.packetGaps));
  Serial.print("rejectedPackets=");
  Serial.println(static_cast<unsigned long>(dump.rejectedPackets));
  Serial.print("packetSequence=");
  Serial.println(static_cast<unsigned long>(dump.packetSequence));
  Serial.print("disconnectSequence=");
  Serial.println(static_cast<unsigned long>(dump.disconnectSequence));
  Serial.print("connectionGeneration=");
  Serial.println(static_cast<unsigned long>(dump.connectionGeneration));
  Serial.print("lastDisconnectReason=");
  Serial.println(static_cast<unsigned>(dump.lastDisconnectReason));
  Serial.print("rssi=");
  if (dump.rssiValid) {
    Serial.println(static_cast<int>(dump.rssi));
  } else {
    Serial.println("-");
  }
  Serial.print("workerAgeMs=");
  Serial.println(static_cast<unsigned long>(dump.workerAgeMs));
  Serial.print("timerValid=");
  Serial.println(dump.timerValid ? "true" : "false");
  Serial.print("timerMs=");
  Serial.println(static_cast<unsigned long>(dump.timerMs));
  Serial.print("timerAgeMs=");
  Serial.println(static_cast<unsigned long>(dump.timerAgeMs));
  Serial.print("weightFresh=");
  Serial.println(dump.weightFresh ? "true" : "false");
  Serial.print("weightG=");
  char weight[16] = {};
  snprintf(weight, sizeof(weight), "%.2f",
           static_cast<double>(dump.currentWeightG));
  Serial.println(weight);
  Serial.print("scanIntensity=");
  Serial.println(dump.scanIntensity != nullptr ? dump.scanIntensity : "-");
}

inline void serialCliPrintNtpStatus(const SerialCliNtpDump &dump) {
  Serial.println("NTP_STATUS");
  Serial.print("state=");
  Serial.println(timeSyncStateName(dump.state));
  Serial.print("utcSec=");
  Serial.println(static_cast<unsigned long>(dump.utcSec));
  Serial.print("lastSyncAgeMs=");
  Serial.println(static_cast<unsigned long>(dump.lastSyncAgeMs));
  Serial.print("nextRetryInMs=");
  Serial.println(static_cast<unsigned long>(dump.nextRetryInMs));
  Serial.print("consecutiveFailures=");
  Serial.println(static_cast<unsigned>(dump.consecutiveFailures));
  Serial.print("activeServer=");
  Serial.println(dump.activeServer[0] != '\0' ? dump.activeServer : "-");
  Serial.print("preset=");
  Serial.println(ntpPresetHostname(dump.ntpServerPreset));
  Serial.print("custom=");
  Serial.println(dump.ntpServerCustom[0] != '\0' ? dump.ntpServerCustom : "-");
  Serial.print("timezoneOffsetMinutes=");
  Serial.println(static_cast<int>(dump.timezoneOffsetMinutes));
  Serial.print("timezoneId=");
  Serial.println(dump.timezoneId[0] != '\0' ? dump.timezoneId : "-");
  Serial.print("staUp=");
  Serial.println(dump.staUp ? "true" : "false");
  if (!dump.staUp) {
    Serial.println("ntp cannot arm without STA");
  }
}

inline void serialCliPrintLogDumpPreamble(size_t count, LogLevel retainLevel) {
  Serial.println("LOG_DUMP");
  Serial.print("ringRetainLogLevel=");
  Serial.println(logLevelName(retainLevel));
  if (retainLevel == LogLevel::NONE) {
    Serial.println("log ring retain is none");
  }
  if (count == 0) {
    Serial.println("log ring empty");
    return;
  }
  Serial.print("events=");
  Serial.println(static_cast<unsigned long>(count));
}

inline bool serialCliFeed(SerialCliParser &parser, char received,
                          SerialCliRequest &request) {
  serialCliClearRequest(request);
  if (received == '\r' || received == '\n') {
    if (parser.overflow) {
      serialCliResetParser(parser);
      request.verb = SerialCliVerb::LINE_TOO_LONG;
      request.error = "line too long";
      return true;
    }
    if (parser.length == 0) {
      return false;
    }
    parser.line[parser.length] = '\0';
    const bool ready = serialCliParseLine(parser.line, request);
    serialCliResetParser(parser);
    return ready;
  }
  if (parser.overflow) {
    return false;
  }
  if (parser.length + 1 >= SERIAL_CLI_LINE_CAPACITY) {
    parser.overflow = true;
    parser.length = 0;
    return false;
  }
  parser.line[parser.length++] = received;
  return false;
}

}  // namespace openbrewbyweight
