// Serial/ESP-log sink and debug-event pipeline: ESP-log tag and level
// mapping, the queue-backed serial writer, ring retention policy, and the
// logEmit/addDebugEvent entry points. Part of the openBrewByWeight control TU;
// included at the position of the original definitions.

// Retains one text line in the WebUI ring when its level passes retention.
void retainLogText(LogLevel level, DebugCategory category, const char *message) {
  if (!logLevelAtMost(level, currentRingRetainLogLevel())) return;
  const uint32_t atMs = millis();
  const uint32_t wallSec = g_wallClock.nowUtcSec(atMs);
  TaskLockGuard lock(debugLogMutex);
  debugLog.add(atMs, wallSec, level, category, DebugCode::LOG_TEXT, 0, 0,
               message);
  __atomic_store_n(&debugLogDroppedSnapshot, debugLog.overwritten(),
                   __ATOMIC_RELAXED);
}

// Bluetooth stack tags go to ble; every other ESP-IDF component stays system.
// body is the retained "TAG: text"; the colon keeps prefixes from colliding.
DebugCategory espLogCategory(const char *body) {
  static const char *const bleTags[] = {
      "NimBLE", "BLE_INIT", "BT_INIT", "BLE_LOG", "ble_log", "bt_hci_log",
      "HCI_UTIL", "hci_uart", "bt_osi_config", "ble_svc_gap", "scale.nimble"};
  for (const char *tag : bleTags) {
    const size_t length = strlen(tag);
    if (strncmp(body, tag, length) == 0 && body[length] == ':') {
      return DebugCategory::BLE;
    }
  }
  return DebugCategory::SYSTEM;
}

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
esp_log_level_t espLogLevel(LogLevel level) {
  switch (level) {
    // ESP-IDF has no critical/fatal level. Keep that distinction in the
    // application record and mark it in the emitted message.
    case LogLevel::CRITICAL:
    case LogLevel::ERROR: return ESP_LOG_ERROR;
    case LogLevel::WARNING: return ESP_LOG_WARN;
    case LogLevel::INFO: return ESP_LOG_INFO;
    case LogLevel::DEBUG: return ESP_LOG_DEBUG;
    case LogLevel::NONE: return ESP_LOG_NONE;
  }
  return ESP_LOG_NONE;
}

// ESP-IDF component lines (esp-tls, HTTP_CLIENT, ...) reach only this hook and
// production has no USB console, so the WebUI ring retains them too. obbw.*
// lines are already there. Log v1 format: "E (1234) tag: text\n".
void retainEspLogLine(const char *text) {
  const char *body = strstr(text, ") ");
  if (text[1] != ' ' || text[2] != '(' || body == nullptr ||
      strncmp(body + 2, "obbw.", 5) == 0 || !xPortCanYield()) {
    return;
  }
  char message[DEBUG_EVENT_TEXT_CAPACITY];
  copyCString(message, sizeof(message), body + 2);
  message[strcspn(message, "\r\n")] = '\0';
  retainLogText(text[0] == 'E'   ? LogLevel::ERROR
                : text[0] == 'W' ? LogLevel::WARNING
                : text[0] == 'I' ? LogLevel::INFO
                                 : LogLevel::DEBUG,
                espLogCategory(message), message);
}

int openBrewByWeightEspLogVprintf(const char *format, va_list args) {
  if (format == nullptr) {
    return 0;
  }
  // esp_log may invoke this callback concurrently. Never perform USB I/O in
  // the caller: CDC can block indefinitely when the host stops draining.
  SerialLogLine line;
  va_list copy;
  va_copy(copy, args);
  const int formatted = vsnprintf(line.text, sizeof(line.text), format, copy);
  va_end(copy);
  if (formatted <= 0) {
    return formatted;
  }
  // Benign IDF lines. BLE_INIT warns that light sleep is blocked, but this
  // firmware never enters light sleep. A client resetting its socket
  // (ECONNRESET: tab closed, phone asleep) is a normal disconnect that httpd
  // already closes, as is the WS header-byte read that follows it on a live
  // WebSocket, so both stay DEBUG traces on both sinks. A bound UI
  // stream sends through uiStreamSend, whose rate-limited ui_stream warning
  // already reports each failed WS send with size, timing, and errno.
  if (strstr(line.text,
             "HTTP_CLIENT: Connection timed out before data was ready!") !=
          nullptr ||
      strstr(line.text, "BLE_INIT: light sleep mode will not be able") !=
          nullptr) {
    return 0;
  }
  if (strstr(line.text, "httpd_sock_err: error in recv : 104") != nullptr ||
      strstr(line.text, "httpd_ws_get_frame_type: Failed to read header") !=
          nullptr ||
      strstr(line.text, "httpd_ws_send_frame_async: Failed to send WS") !=
          nullptr) {
    line.text[0] = 'D';
  }
  retainEspLogLine(line.text);
  // Without the USB sink, keep IDF's default console output, if any.
  if (serialLogQueue == nullptr) {
#if CONFIG_ESP_CONSOLE_NONE
    return formatted;
#else
    return vprintf(format, args);
#endif
  }
  if (!__atomic_load_n(&serialLogSinkEnabled, __ATOMIC_ACQUIRE)) {
    return 0;
  }
  size_t length = static_cast<size_t>(formatted);
  if (length >= sizeof(line.text)) {
    length = sizeof(line.text) - 1;
    (void)__atomic_add_fetch(&serialLogQueueTruncated, 1U, __ATOMIC_RELAXED);
  }
  line.length = static_cast<uint16_t>(length);
  if (xQueueSend(serialLogQueue, &line, 0) != pdTRUE) {
    (void)__atomic_add_fetch(&serialLogQueueDropped, 1U, __ATOMIC_RELAXED);
    return 0;
  }
  return static_cast<int>(length);
}

void serialLogTask(void *) {
  SerialLogLine line;
  for (;;) {
    if (drainSerialCliOutput()) continue;
    if (xQueueReceive(serialLogQueue, &line, pdMS_TO_TICKS(10)) == pdTRUE &&
        line.length > 0) {
      (void)Serial.write(reinterpret_cast<const uint8_t *>(line.text),
                         line.length);
    }
  }
}

bool initializeSerialLogSink() {
  if (serialLogQueue != nullptr && serialLogTaskHandle != nullptr) {
    return true;
  }
  constexpr size_t queueBytes = SERIAL_LOG_QUEUE_DEPTH * sizeof(SerialLogLine);
  serialLogQueueBytes = static_cast<uint8_t *>(
      allocInternal(queueBytes, AllocationOwner::SERIAL_LOG));
  if (serialLogQueueBytes == nullptr || !esp_ptr_internal(serialLogQueueBytes)) {
    heapCapsFree(serialLogQueueBytes);
    serialLogQueueBytes = nullptr;
    return false;
  }
  serialLogQueue = xQueueCreateStatic(
      SERIAL_LOG_QUEUE_DEPTH, sizeof(SerialLogLine), serialLogQueueBytes,
      &serialLogQueueStorage);
  if (serialLogQueue == nullptr) {
    releaseSerialLogStorage();
    return false;
  }
  serialCliOutputBytes = static_cast<uint8_t *>(
      allocExternal(SERIAL_CLI_OUTPUT_CAPACITY, AllocationOwner::SERIAL_LOG));
  if (serialCliOutputBytes == nullptr) {
    releaseSerialLogStorage();
    return false;
  }
  if (xTaskCreatePinnedToCore(serialLogTask, "serial_log", 3072, nullptr,
                             tskIDLE_PRIORITY, &serialLogTaskHandle, 0) !=
      pdPASS) {
    releaseSerialLogStorage();
    serialLogTaskHandle = nullptr;
    return false;
  }
  return true;
}

// Installed in every build so the WebUI ring sees IDF lines; the USB queue
// exists only when the console is enabled.
void installEspLogSink(bool usbSerial) {
  if (usbSerial) (void)initializeSerialLogSink();
  if (!serialLogSinkInstalled) {
    (void)esp_log_set_vprintf(openBrewByWeightEspLogVprintf);
    serialLogSinkInstalled = true;
  }
}

void configureEspLogRuntime() {
  const LogLevel applicationLevel = currentSerialLogLevel();
  const bool enabled = applicationLevel != LogLevel::NONE;
  // Close the sink first when disabling so another task cannot emit under the
  // old tag level while this update is in progress.
  if (!enabled) {
    __atomic_store_n(&serialLogSinkEnabled, false, __ATOMIC_RELEASE);
  }
  const esp_log_level_t level = espLogLevel(applicationLevel);
  static const char *const tags[] = {
      "obbw.activator", "obbw.relay", "obbw.state", "obbw.scale", "obbw.config",
      "obbw.network", "obbw.security", "obbw.web", "obbw.boot", "obbw.system",
      "obbw.ble"};
  for (const char *tag : tags) {
    esp_log_level_set(tag, level);
  }
  if (enabled) {
    __atomic_store_n(&serialLogSinkEnabled, true, __ATOMIC_RELEASE);
  }
}
#else
void installEspLogSink(bool) {}
void configureEspLogRuntime() {}
#endif

void latchControlCriticalLogging() {
  controlCriticalForLogging.store(true, std::memory_order_release);
}

void publishControlCriticalLoggingState() {
  const bool critical = session.active || relayCircuitClosed();
  controlCriticalForLogging.store(critical, std::memory_order_release);
}

bool serialApplicationLogAllowed() {
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // Portable tests directly manipulate the control-owned fixtures.
  return !session.active && !circuitClosed;
#elif CONFIG_ESP_CONSOLE_NONE
  // No USB sink and no console: an obbw line would be formatted for nothing;
  // the WebUI ring already holds it.
  return serialLogQueue != nullptr &&
         !controlCriticalForLogging.load(std::memory_order_acquire);
#else
  return !controlCriticalForLogging.load(std::memory_order_acquire);
#endif
}

void emitEspLog(LogLevel level, DebugCategory category, const char *message) {
  if (message == nullptr || level == LogLevel::NONE) {
    return;
  }
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // espLogTag was removed: the tag is debugCategoryName() prefixed with
  // "obbw.". Derive it here so the category names have a single source.
  char tag[24];
  copyCString(tag, sizeof(tag), "obbw.");
  copyCString(tag + 5, sizeof(tag) - 5, debugCategoryName(category));
  const char *criticalPrefix =
      level == LogLevel::CRITICAL ? "[CRITICAL] " : "";
  // ESP_LOG_LEVEL accepts a runtime level without the ESP_LOGx local-level
  // compile-out and adds the standard severity, timestamp, tag and newline.
  ESP_LOG_LEVEL(espLogLevel(level), tag, "%s%s", criticalPrefix, message);
#else
  (void)category;
  Serial.println(message);
#endif
}

void writeSerialLogLine(const DebugEvent &event) {
  char message[128];
  formatDebugEventMessage(event, message, sizeof(message), FW_VERSION);
  emitEspLog(event.level, event.category, message);
}

void logText(LogLevel level, DebugCategory category, const char *message) {
  if (message == nullptr || level == LogLevel::NONE) {
    return;
  }
  retainLogText(level, category, message);
  if (logLevelAtMost(level, currentSerialLogLevel()) &&
      serialApplicationLogAllowed()) {
    emitEspLog(level, category, message);
  }
}

// Ad-hoc diagnostics are application logs too: retain them in the WebUI ring
// and forward them to esp_log. Keep this compatibility wrapper for existing
// call sites while they are gradually made category-specific.
void serialTrace(LogLevel level, const char *message) {
  logText(level, DebugCategory::SYSTEM, message);
}

void serialTraceCategory(LogLevel level, DebugCategory category,
                         const char *message) {
  logText(level, category, message);
}

extern "C" void openBrewByWeightScaleLog(uint8_t severity, bool transport,
                                         const char *message) {
  LogLevel level = LogLevel::DEBUG;
  if (severity == 4) {
    level = LogLevel::INFO;
  } else if (severity == 1) {
    level = LogLevel::WARNING;
  } else if (severity >= 2) {
    level = LogLevel::ERROR;
  }
  serialTraceCategory(
      level, transport ? DebugCategory::BLE : DebugCategory::SCALE, message);
}

// Formats only when logText would keep the line: the per-sample DEBUG traces
// would otherwise pay soft-double %f formatting for a discarded line.
void logTextv(LogLevel level, DebugCategory category, const char *fmt,
              va_list args) {
  if (fmt == nullptr ||
      (!logLevelAtMost(level, currentRingRetainLogLevel()) &&
       !(logLevelAtMost(level, currentSerialLogLevel()) &&
         serialApplicationLogAllowed()))) {
    return;
  }
  char line[DEBUG_EVENT_TEXT_CAPACITY];
  vsnprintf(line, sizeof(line), fmt, args);
  logText(level, category, line);
}

void serialTracef(LogLevel level, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  logTextv(level, DebugCategory::SYSTEM, fmt, args);
  va_end(args);
}

void serialTraceCategoryf(LogLevel level, DebugCategory category,
                          const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  logTextv(level, category, fmt, args);
  va_end(args);
}

// Verbose audit hunt session (Diagnostics toggle): marked DEBUG codes join
// the latch while the wall-clock deadline holds. The session lives in RAM
// like a profiler run — a reboot ends the hunt, the records it captured stay.
constexpr uint32_t AUDIT_VERBOSE_SESSION_SEC = 30 * 60;
uint32_t auditVerboseUntilUtcSec = 0;
bool auditVerboseActive() {
  return auditVerboseUntilUtcSec != 0 &&
         g_wallClock.nowUtcSec(millis()) < auditVerboseUntilUtcSec;
}
bool setAuditVerbose(bool on) {
  if (!on) {
    auditVerboseUntilUtcSec = 0;
    return true;
  }
  const uint32_t now = g_wallClock.nowUtcSec(millis());
  if (now == 0) return false;  // a hunt deadline needs a synced wall clock
  auditVerboseUntilUtcSec = now + AUDIT_VERBOSE_SESSION_SEC;
  serialTraceCategoryf(LogLevel::INFO, DebugCategory::SYSTEM,
                       "audit verbose session armed for %u s", AUDIT_VERBOSE_SESSION_SEC);
  return true;
}
uint32_t auditVerboseDeadline() {
  return auditVerboseActive() ? auditVerboseUntilUtcSec : 0;
}

void logEmitDetail(LogLevel level, DebugCategory category, DebugCode code,
                   int32_t argument1, int32_t argument2,
                   const char *detail) {
  if (level == LogLevel::NONE) {
    return;
  }
  const bool toSerial =
      logLevelAtMost(level, currentSerialLogLevel());
  const bool toRing =
      logLevelAtMost(level, currentRingRetainLogLevel());
  // Audit capture is level-independent: curated events enter the latch even
  // when neither serial nor ring retains them.
  const bool toAudit = auditWorthy(level, code, auditVerboseActive());
  if (!toSerial && !toRing && !toAudit) {
    return;
  }
  const uint32_t atMs = millis();
  const uint32_t wallSec = g_wallClock.nowUtcSec(atMs);

  {
    TaskLockGuard lock(debugLogMutex);
    if (toRing) {
      debugLog.add(atMs, wallSec, level, category, code, argument1, argument2,
                   detail);
      __atomic_store_n(&debugLogDroppedSnapshot, debugLog.overwritten(),
                       __ATOMIC_RELAXED);
    }
    if (toAudit) {
      auditLog.add(atMs, wallSec, level, category, code, argument1, argument2,
                   detail);
    }
  }

  // Target builds enqueue in the bounded serial sink; host builds emit
  // synchronously into the test stub.
  if (toSerial && serialApplicationLogAllowed()) {
    DebugEvent event;
    event.atMs = atMs;
    event.wallSec = wallSec;
    event.level = level;
    event.category = category;
    event.code = code;
    event.argument1 = argument1;
    event.argument2 = argument2;
    if (detail != nullptr) {
      copyCString(event.text, sizeof(event.text), detail);
    }
    writeSerialLogLine(event);
  }
}

void logEmit(LogLevel level, DebugCategory category, DebugCode code,
             int32_t argument1 = 0, int32_t argument2 = 0) {
  logEmitDetail(level, category, code, argument1, argument2, nullptr);
}

// Every debug event also reaches the shot trace while its window is open,
// whatever the log levels retain.
void addDebugEvent(DebugCategory category, DebugCode code,
                   int32_t argument1 = 0, int32_t argument2 = 0) {
  if (shotTrace().capturing()) shotTrace().noteDebug(code, argument1, argument2, millis());
  logEmit(debugCodeDefaultLevel(code), category, code, argument1, argument2);
}

void addDebugEventDetail(DebugCategory category, DebugCode code,
                         int32_t argument1, int32_t argument2,
                         const char *detail) {
  if (shotTrace().capturing()) shotTrace().noteDebug(code, argument1, argument2, millis());
  logEmitDetail(debugCodeDefaultLevel(code), category, code, argument1,
                argument2, detail);
}

// Scale profile note path sinks (declared in OpenBrewByWeightScaleProfiler.h).
void openbrewbyweight::shotTraceNoteProfileEvent(ScaleProfileEvent kind, uint32_t nowMs,
                                                 float weightG, uint32_t arg1,
                                                 uint32_t arg2) {
  shotTrace().noteProfile(kind, nowMs, weightG, arg1, arg2);
}
void openbrewbyweight::shotTraceNoteDroppedFrame(bool stale) {
  shotTrace().noteDroppedFrame(stale);
}

// Linea Micra WebSocket task hooks (declared in OpenBrewByWeightMicraService.cpp).
void shotTraceNoteMicraRx(uint32_t bytes) { shotTrace().noteMicraRx(bytes); }
void shotTraceNoteMicraSubscribed(bool subscribed) {
  shotTrace().noteMicraSubscribed(subscribed);
}

uint32_t debugLogLatestSequence() {
  TaskLockGuard lock(debugLogMutex);
  return debugLog.latestSequence();
}

size_t copyDebugEvents(uint32_t afterSequence, DebugEvent *output,
                       size_t capacity,
                       DebugLogReadMetadata *metadata = nullptr) {
  TaskLockGuard lock(debugLogMutex);
  const size_t copied =
      debugLog.copyAfter(afterSequence, output, capacity, metadata);
  if (metadata != nullptr) {
    metadata->serialDropped = serialLogDroppedCount();
    metadata->serialTruncated = serialLogTruncatedCount();
  }
  return copied;
}

size_t copyAuditEvents(uint32_t afterSequence, AuditRecord *output,
                       size_t capacity, uint32_t *dropped = nullptr,
                       uint32_t *rateLimited = nullptr) {
  {
    TaskLockGuard lock(debugLogMutex);
    if (dropped != nullptr) *dropped = auditLog.overwritten();
    if (rateLimited != nullptr) *rateLimited = auditLog.rateLimited();
  }
  // Boot adoption mirrors every flushed record in the latch, so the durable
  // window only contributes records older than the latch's oldest page item;
  // both sources come back in ascending sequence order.
  AuditRecord probe[1];
  uint32_t latchOldest = 0;
  {
    TaskLockGuard lock(debugLogMutex);
    if (auditLog.copyAfter(afterSequence, probe, 1) > 0) {
      latchOldest = probe[0].sequence;
    }
  }
  size_t filled = 0;
  if (latchOldest == 0 || latchOldest > afterSequence + 1) {
    AuditRecord durablePage[8];
    uint32_t durableAfter = afterSequence;
    uint32_t resumeSlot = 0;
    while (filled < capacity) {
      const size_t count =
          auditStoreCopyAfter(durableAfter, durablePage, 8, &resumeSlot);
      if (count == 0) break;
      for (size_t index = 0; index < count && filled < capacity; ++index) {
        if (latchOldest != 0 && durablePage[index].sequence >= latchOldest) {
          continue;  // already mirrored in the latch page
        }
        output[filled++] = durablePage[index];
      }
      durableAfter = durablePage[count - 1].sequence;
      if (count < 8) break;
    }
  }
  {
    TaskLockGuard lock(debugLogMutex);
    if (filled < capacity) {
      filled += auditLog.copyAfter(afterSequence, output + filled,
                                   capacity - filled);
    }
  }
  return filled;
}

// Explicit clear (endpoint / factory reset): erase the durable store first,
// then drop the latch so both surfaces agree the history is empty.
uint32_t auditLogLatestSequence() {
  TaskLockGuard lock(debugLogMutex);
  return auditLog.latestSequence();
}

void copyAuditStoreStatus(AuditStoreStatus &output) { output = auditStoreStatus(); }

void clearAuditLatch() {
  TaskLockGuard lock(debugLogMutex);
  auditLog.clear();
}

// Explicit clear (endpoint / factory reset): erase the durable store first,
// then drop the latch so both surfaces agree the history is empty. Declared
// in OpenBrewByWeightAuditStore.h; this TU fragment is its single definition.
namespace openbrewbyweight {
bool clearAuditStores() {
  if (!auditStoreEraseAll()) return false;
  clearAuditLatch();
  return true;
}
}  // namespace openbrewbyweight
