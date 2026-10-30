// Companion link's diagnostic log stream (docs/COMPANION_LINK.md, plan 2B):
// the subscription, the flush scheduling and the `log` frames. httpd parses
// the companion's `logsub` and nothing else; the network task owns everything
// after — the sent cursor, the holds (queue-and-flush, never drop) and the
// piggyback heartbeat. The ring's watermarks are read through the debug-log
// bridge outside dataMux_ (never nested with it).

namespace openbrewbyweight {

// The log stream's hold predicate: exactly the link's own admission rule, so
// no log frame is transmitted during a scaled shot, a scale connection or
// discovery, or the BLE quiet interval (plan G, principle 9).
bool OpenBrewByWeightNetwork::companionLogHoldActive() const {
  return outboundScaleInhibited() ||
         scaleConnecting_.load(std::memory_order_acquire) ||
         (outboundScaleConnected.load(std::memory_order_acquire) &&
          outboundShotActive.load(std::memory_order_acquire));
}

// httpd: record the companion's log cursor for this controller. False (and the
// link closes) on a malformed frame; the network task applies it later.
bool OpenBrewByWeightNetwork::companionLinkLogSub(CompanionLinkSlot &slot, cJSON *frame) {
  bool on = false;
  uint32_t boot = 0, after = 0;
  if (!jsonBoolean(frame, "on", on) || !jsonUint32(frame, "boot", boot) ||
      !jsonUint32(frame, "after", after))
    return false;
  {
    TaskLockGuard lock(dataMux_);
    slot.logWant = on;
    slot.logWantBoot = boot;
    slot.logWantAfter = after;
    slot.logApplied = false;
  }
  companionOutboxChanged();
  return true;
}

// Network task: the due check runs every pass, so a hold's end flushes at once
// instead of waiting for the next ping. The watermarks are read outside
// dataMux_ (the ring's own mutex, never nested here).
void OpenBrewByWeightNetwork::companionLogService() {
  bool logArmed = false;
  {
    TaskLockGuard lock(dataMux_);
    const CompanionLinkSlot *slot = companionActiveLink();
    logArmed = slot != nullptr && !slot->closing && slot->logSubscribed &&
               slot->logApplied && g_companion.stored.logSwitch != 0;
  }
  if (!logArmed) return;
  const uint32_t latest =
      callbacks_.debugLogLatestSequence ? callbacks_.debugLogLatestSequence() : 0;
  const uint32_t urgent = callbacks_.debugLogLatestAlarmSequence
                              ? callbacks_.debugLogLatestAlarmSequence()
                              : 0;
  TaskLockGuard lock(dataMux_);
  CompanionLinkSlot *slot = const_cast<CompanionLinkSlot *>(companionActiveLink());
  if (slot == nullptr || slot->closing) return;
  const bool pendingLog = companionLogPending(latest, slot->logSent);
  const bool urgentPending = companionLogUrgent(urgent, latest, slot->logSent);
  const bool hold = companionLogHoldActive();
  if (pendingLog && hold) {
    // Queue-and-flush: everything in order when the hold ends.
    slot->logCatchUp = true;
  } else if (companionLogFlushDue(slot->logSubscribed, pendingLog, hold, urgentPending,
                                  slot->logCatchUp, g_companion.logHeard)) {
    companionNotified_.store(true, std::memory_order_release);
  }
}

// Network task, under dataMux_ (the caller of companionPrepareSends holds it):
// apply the companion's subscription against this boot's id — a cursor from
// another boot restarts from the oldest retained event.
void OpenBrewByWeightNetwork::companionLogApplyLocked(CompanionLinkSlot &link) {
  if (link.logApplied) return;
  if (companionLogBootId_ == 0 && callbacks_.debugLogBootId != nullptr)
    companionLogBootId_ = callbacks_.debugLogBootId();
  link.logSubscribed = link.logWant;
  link.logSent = link.logWant && link.logWantBoot == companionLogBootId_ ? link.logWantAfter : 0;
  link.logCatchUp = link.logWant;
  link.logApplied = true;
}

// Network task, no lock: append one `log` body — as many events as the
// remaining frame budget holds — to the send job. The rest pages on the next
// pass; the events are the formatted ones the Web UI log view renders.
bool OpenBrewByWeightNetwork::companionLogBody(uint8_t &count, const CompanionLinkSlot &sent,
                                               bool logSwitch, uint32_t &logLast, bool &logMore,
                                               uint32_t &logMiss) {
  if (!sent.logSubscribed || !logSwitch) return false;
  const uint32_t latest =
      callbacks_.debugLogLatestSequence ? callbacks_.debugLogLatestSequence() : 0;
  const uint32_t urgent = callbacks_.debugLogLatestAlarmSequence
                              ? callbacks_.debugLogLatestAlarmSequence()
                              : 0;
  const bool pendingLog = companionLogPending(latest, sent.logSent);
  const bool urgentPending = companionLogUrgent(urgent, latest, sent.logSent);
  if (!companionLogFlushDue(true, pendingLog, companionLogHoldActive(), urgentPending,
                            sent.logCatchUp, g_companion.logHeard))
    return false;
  DebugLogReadMetadata meta{};
  uint32_t cursor = sent.logSent;
  if (callbacks_.copyDebugEvents != nullptr)
    (void)callbacks_.copyDebugEvents(cursor, nullptr, 0, &meta);
  if (meta.cursorInvalid) cursor = 0;  // the ring was cleared: refill
  logMiss = meta.missedEvents;
  char head[72], missText[24] = "";
  if (logMiss != 0)
    snprintf(missText, sizeof(missText), "%s\"miss\":%lu",
             meta.cursorInvalid ? "," : "", static_cast<unsigned long>(logMiss));
  snprintf(head, sizeof(head), "{\"t\":\"log\",\"b\":%lu,%s%s\"e\":[",
           static_cast<unsigned long>(companionLogBootId_),
           meta.cursorInvalid ? "\"reset\":true" : "", missText);
  size_t start = 0;
  for (uint8_t index = 0; index < count; ++index)
    start += strlen(companionWs().job.bodies + start) + 1;
  char *out = companionWs().job.bodies + start;
  const size_t capacity = sizeof(companionWs().job.bodies) - start;
  size_t used = static_cast<size_t>(snprintf(out, capacity, "%s", head));
  bool first = true, full = false;
  while (callbacks_.copyDebugEvents != nullptr && !full) {
    DebugEvent event;
    if (callbacks_.copyDebugEvents(cursor, &event, 1, nullptr) == 0) break;
    const char *text = event.text;
    char formatted[DEBUG_EVENT_TEXT_CAPACITY];
    if (text[0] == '\0') {
      formatDebugEventMessage(event, formatted, sizeof(formatted), FW_VERSION);
      text = formatted;
    }
    if (!escapeJsonString(text, companionWs().frame, sizeof(companionWs().frame))) break;
    char piece[DEBUG_EVENT_TEXT_CAPACITY * 6 + 96];
    const int written = snprintf(
        piece, sizeof(piece), "%s[%lu,%lu,%lu,\"%c\",\"%s\",\"%s\"]", first ? "" : ",",
        static_cast<unsigned long>(event.sequence), static_cast<unsigned long>(event.atMs),
        static_cast<unsigned long>(event.wallSec), companionLogLevelLetter(event.level),
        debugCategoryName(event.category), companionWs().frame);
    if (written < 0 || used + static_cast<size_t>(written) + 3 >= capacity ||
        used + static_cast<size_t>(written) + COMPANION_MAC_HEX + 40 > COMPANION_FRAME_BYTES) {
      full = true;
      break;
    }
    memcpy(out + used, piece, static_cast<size_t>(written) + 1);
    used += static_cast<size_t>(written);
    first = false;
    cursor = event.sequence;
    logLast = cursor;
  }
  if (first) return false;
  memcpy(out + used, "]}", 3);
  ++count;
  logMore = latest != 0 && static_cast<int32_t>(latest - logLast) > 0;
  return true;
}

}  // namespace openbrewbyweight
