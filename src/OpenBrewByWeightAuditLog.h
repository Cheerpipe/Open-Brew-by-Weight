#pragma once

#include "OpenBrewByWeightDomain.h"

namespace openbrewbyweight {

// Curated audit classification for the durable audit latch: rare, high-signal
// events that must survive common-ring rotation while hunting unattended
// anomalies. Every ERROR/CRITICAL is audit-worthy; WARNING codes enter through
// the allowlist below; marked DEBUG codes join only while a verbose audit
// session is active (rate limits still apply).
constexpr DebugCode AUDIT_WARNING_ALLOWLIST[] = {
    DebugCode::MACHINE_MUX_WAIT,
    DebugCode::HEALTH_HEAP_LOW,
    DebugCode::HEALTH_HEAP_RESTART,
    DebugCode::HEALTH_STACK_LOW,
    DebugCode::BOOT_RECOVERY_STACK,
    DebugCode::BLE_LINK_PARAMS_OVERRIDDEN,
    DebugCode::SCALE_LATE_TARE_RECOGNIZED,
    DebugCode::SCALE_LATE_TARE_UNDONE,
    DebugCode::HEALTH_LOOP_GAP,
    DebugCode::SYSTEM_LOG_OVERRUN,
    DebugCode::RELAY_GPIO_DESYNC,
    DebugCode::SAFETY_LOCKOUT_ACTIVE,
    DebugCode::CIRCUIT_ARM_FAILED,
    DebugCode::INITIALIZATION_FAILED,
    DebugCode::OTA_UPLOAD_STARTED,
    DebugCode::OTA_IMAGE_STAGED,
    DebugCode::OTA_UPLOAD_REJECTED,
    DebugCode::OTA_FLASH_COMMITTED,
    DebugCode::OTA_ROLLBACK_ARMED,
    DebugCode::OTA_ROLLBACK_FAILED,
    DebugCode::SCHEDULED_RESTART_FIRED,
    DebugCode::SCHEDULED_RESTART_SKIPPED,
};

// Rare DEBUG events admitted only during an opt-in verbose audit session:
// maintenance-lease window transitions, useful when hunting unattended
// stalls that coincide with reserved periods. Common DEBUG chatter (scan
// waiting, connect attempts) never enters, verbose or not.
constexpr DebugCode AUDIT_VERBOSE_ALLOWLIST[] = {
    DebugCode::MAINTENANCE_RESERVED,
    DebugCode::MAINTENANCE_COMPLETED,
    DebugCode::MAINTENANCE_CANCELED,
};

constexpr bool auditListed(const DebugCode *list, size_t count, DebugCode code) {
  for (size_t i = 0; i < count; ++i) {
    if (list[i] == code) return true;
  }
  return false;
}

constexpr bool auditListHasNoDuplicates(const DebugCode *list, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = i + 1; j < count; ++j) {
      if (list[i] == list[j]) return false;
    }
  }
  return true;
}

static_assert(auditListHasNoDuplicates(
                  AUDIT_WARNING_ALLOWLIST,
                  sizeof(AUDIT_WARNING_ALLOWLIST) / sizeof(DebugCode)),
              "audit WARNING allowlist must not repeat a code");
static_assert(auditListHasNoDuplicates(
                  AUDIT_VERBOSE_ALLOWLIST,
                  sizeof(AUDIT_VERBOSE_ALLOWLIST) / sizeof(DebugCode)),
              "audit verbose allowlist must not repeat a code");

constexpr bool auditWorthy(LogLevel level, DebugCode code, bool verbose = false) {
  if (level == LogLevel::ERROR || level == LogLevel::CRITICAL) return true;
  if (level == LogLevel::WARNING &&
      auditListed(AUDIT_WARNING_ALLOWLIST,
                  sizeof(AUDIT_WARNING_ALLOWLIST) / sizeof(DebugCode), code)) {
    return true;
  }
  return verbose && level == LogLevel::DEBUG &&
         auditListed(AUDIT_VERBOSE_ALLOWLIST,
                     sizeof(AUDIT_VERBOSE_ALLOWLIST) / sizeof(DebugCode), code);
}

// Compact audit record: whole-gram diagnostics values plus a short sanitized
// excerpt of the emitter's detail text. Field order mirrors the packed flash
// encoding (phase 2); keep both in sync.
constexpr size_t AUDIT_RECORD_CAPACITY = 64;
constexpr size_t AUDIT_EXCERPT_BYTES = 24;
// Anti-flood defense on top of producer rate limits: at most this many
// admitted records per code inside the rolling window.
constexpr uint32_t AUDIT_RATE_WINDOW_MS = 60000;
constexpr uint8_t AUDIT_RATE_LIMIT = 10;
constexpr uint8_t AUDIT_RATE_SLOTS = 8;

struct AuditRecord {
  uint32_t sequence = 0;
  uint32_t wallSec = 0;
  uint32_t atMs = 0;
  int32_t argument1 = 0;
  int32_t argument2 = 0;
  uint8_t level = 0;     // LogLevel
  uint8_t category = 0;  // DebugCategory
  uint8_t code = 0;      // DebugCode
  uint8_t textLen = 0;
  char text[AUDIT_EXCERPT_BYTES] = {};
};

// Composes the full human-readable detail for one audit record at the JSON
// surface: the per-code formatters rebuild the message from the persisted
// whole-gram arguments (so records already on flash become interpretable),
// the emit-time excerpt adds what arguments cannot carry (e.g. a mutex
// holder), and unstructured codes fall back to their raw arguments instead
// of an empty text.
inline void formatAuditRecordDetail(const AuditRecord &record, char *output,
                                    size_t capacity) {
  if (output == nullptr || capacity == 0) {
    return;
  }
  output[0] = '\0';
  DebugEvent event;
  event.code = static_cast<DebugCode>(record.code);
  event.argument1 = record.argument1;
  event.argument2 = record.argument2;
  char base[96] = {};
  if (!(formatLifecycleDebugMessage(event, base, sizeof(base)) ||
        formatScaleSampleDebugMessage(event, base, sizeof(base)) ||
        formatPersistDebugMessage(event, base, sizeof(base))) &&
      (record.argument1 != 0 || record.argument2 != 0)) {
    snprintf(base, sizeof(base), "arg1=%ld arg2=%ld",
             static_cast<long>(record.argument1),
             static_cast<long>(record.argument2));
  }
  size_t used = 0;
  if (base[0] != '\0') {
    used = snprintf(output, capacity, "%s", base);
    if (used >= capacity) used = capacity - 1;
  }
  if (record.textLen > 0 && used + 1 < capacity) {
    char excerpt[AUDIT_EXCERPT_BYTES] = {};
    const size_t length =
        record.textLen < AUDIT_EXCERPT_BYTES ? record.textLen
                                             : AUDIT_EXCERPT_BYTES - 1;
    for (size_t i = 0; i < length; ++i) {
      excerpt[i] = record.text[i];
    }
    snprintf(output + used, capacity - used, "%s%s", used > 0 ? " " : "",
             excerpt);
  }
}

// Ring latch for classified audit events only; common debug traffic never
// enters. Callers serialize access with the same mutex as the common ring.
class AuditLogLatch {
 public:
  void clear() {
    nextSequence_ = 1;
    count_ = 0;
    writeIndex_ = 0;
    overwritten_ = 0;
    rateLimited_ = 0;
    rateCount_ = 0;
    pendingAfter_ = 0;
  }

  // Returns false when the per-code rate limit suppressed the event.
  bool add(uint32_t atMs, uint32_t wallSec, LogLevel level,
           DebugCategory category, DebugCode code, int32_t argument1,
           int32_t argument2, const char *detail) {
    if (!admitRate(static_cast<uint8_t>(code), atMs)) {
      ++rateLimited_;
      return false;
    }
    if (count_ == AUDIT_RECORD_CAPACITY) {
      ++overwritten_;
    } else {
      ++count_;
    }
    AuditRecord &record = events_[writeIndex_];
    record = AuditRecord{};
    record.sequence = nextSequence_++;
    record.atMs = atMs;
    record.wallSec = wallSec;
    record.level = static_cast<uint8_t>(level);
    record.category = static_cast<uint8_t>(category);
    record.code = static_cast<uint8_t>(code);
    record.argument1 = argument1;
    record.argument2 = argument2;
    if (detail != nullptr && detail[0] != '\0') {
      size_t length = 0;
      while (length + 1 < AUDIT_EXCERPT_BYTES && detail[length] != '\0') {
        record.text[length] = detail[length];
        ++length;
      }
      record.textLen = static_cast<uint8_t>(length);
    }
    writeIndex_ = (writeIndex_ + 1) % AUDIT_RECORD_CAPACITY;
    return true;
  }

  size_t copyAfter(uint32_t afterSequence, AuditRecord *output,
                   size_t capacity) const {
    if (output == nullptr || capacity == 0) return 0;
    size_t copied = 0;
    for (size_t index = count_; index > 0 && copied < capacity; --index) {
      const AuditRecord &record =
          events_[(writeIndex_ + AUDIT_RECORD_CAPACITY - index) %
                  AUDIT_RECORD_CAPACITY];
      if (record.sequence > afterSequence) output[copied++] = record;
    }
    return copied;
  }

  uint32_t latestSequence() const {
    return nextSequence_ > 1 ? nextSequence_ - 1 : 0;
  }
  size_t count() const { return count_; }
  uint32_t overwritten() const { return overwritten_; }
  uint32_t rateLimited() const { return rateLimited_; }

  // Persistence seam (durable store phase): records adopted from flash keep
  // their original sequence and count as already durable; a fresh add() is
  // pending until markFlushedThrough() advances the watermark past it.
  bool adopt(const AuditRecord &record) {
    if (record.sequence == 0) return false;
    if (count_ == AUDIT_RECORD_CAPACITY) {
      ++overwritten_;
    } else {
      ++count_;
    }
    events_[writeIndex_] = record;
    writeIndex_ = (writeIndex_ + 1) % AUDIT_RECORD_CAPACITY;
    if (record.sequence >= nextSequence_) nextSequence_ = record.sequence + 1;
    if (record.sequence > pendingAfter_) pendingAfter_ = record.sequence;
    return true;
  }

  size_t copyPending(size_t capacity, AuditRecord *output) const {
    return copyAfter(pendingAfter_, output, capacity);
  }

  size_t pendingCount() const {
    size_t pending = 0;
    for (size_t index = count_; index > 0; --index) {
      const AuditRecord &record =
          events_[(writeIndex_ + AUDIT_RECORD_CAPACITY - index) %
                  AUDIT_RECORD_CAPACITY];
      if (record.sequence > pendingAfter_) ++pending;
    }
    return pending;
  }

  void markFlushedThrough(uint32_t sequence) {
    if (sequence > pendingAfter_) pendingAfter_ = sequence;
  }

  uint32_t pendingAfter() const { return pendingAfter_; }
  bool hasPending() const { return latestSequence() > pendingAfter_; }

 private:
  bool admitRate(uint8_t code, uint32_t atMs) {
    for (uint8_t i = 0; i < rateCount_; ++i) {
      if (rateSlots_[i].code != code) continue;
      if (static_cast<uint32_t>(atMs - rateSlots_[i].windowStartMs) <
          AUDIT_RATE_WINDOW_MS) {
        if (rateSlots_[i].admitted >= AUDIT_RATE_LIMIT) return false;
        ++rateSlots_[i].admitted;
        return true;
      }
      rateSlots_[i].windowStartMs = atMs;
      rateSlots_[i].admitted = 1;
      return true;
    }
    if (rateCount_ < AUDIT_RATE_SLOTS) {
      rateSlots_[rateCount_++] = {code, atMs, 1};
      return true;
    }
    // Recycle the oldest window; a burst across more codes than slots loses
    // accounting precision, never correctness.
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < rateCount_; ++i) {
      if (static_cast<uint32_t>(atMs - rateSlots_[i].windowStartMs) >
          static_cast<uint32_t>(atMs - rateSlots_[oldest].windowStartMs)) {
        oldest = i;
      }
    }
    rateSlots_[oldest] = {code, atMs, 1};
    return true;
  }

  struct RateSlot {
    uint8_t code = 0;
    uint32_t windowStartMs = 0;
    uint8_t admitted = 0;
  };

  AuditRecord events_[AUDIT_RECORD_CAPACITY] = {};
  RateSlot rateSlots_[AUDIT_RATE_SLOTS] = {};
  uint32_t nextSequence_ = 1;
  uint32_t overwritten_ = 0;
  uint32_t rateLimited_ = 0;
  uint32_t pendingAfter_ = 0;
  size_t count_ = 0;
  size_t writeIndex_ = 0;
  uint8_t rateCount_ = 0;
};

}  // namespace openbrewbyweight
