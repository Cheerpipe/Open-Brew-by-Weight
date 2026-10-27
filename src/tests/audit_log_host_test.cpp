// Audit classification and RAM latch contracts (plan F1.A/F1.B): every ERROR
// is audit-worthy, WARNING codes enter through the curated allowlist, marked
// DEBUG codes only in verbose mode, and the latch ring rate-limits per code
// while common-ring behavior stays untouched.
#include "OpenBrewByWeightAuditLog.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace openbrewbyweight;

static void testClassification() {
  // ERROR and CRITICAL always qualify, regardless of the code.
  assert(auditWorthy(LogLevel::ERROR, DebugCode::CONFIG_ACCEPTED));
  assert(auditWorthy(LogLevel::CRITICAL, DebugCode::OTA_ROLLBACK_ARMED));
  // Allowlisted WARNING qualifies; an ordinary WARNING does not.
  assert(auditWorthy(LogLevel::WARNING, DebugCode::MACHINE_MUX_WAIT));
  assert(auditWorthy(LogLevel::WARNING, DebugCode::HEALTH_HEAP_LOW));
  assert(auditWorthy(LogLevel::WARNING, DebugCode::RELAY_GPIO_DESYNC));
  assert(!auditWorthy(LogLevel::WARNING, DebugCode::WEB_COMMAND_REJECTED));
  // INFO never qualifies; DEBUG only when marked and verbose is active.
  assert(!auditWorthy(LogLevel::INFO, DebugCode::MACHINE_MUX_WAIT));
  assert(!auditWorthy(LogLevel::DEBUG, DebugCode::MAINTENANCE_RESERVED));
  assert(auditWorthy(LogLevel::DEBUG, DebugCode::MAINTENANCE_RESERVED, true));
  assert(!auditWorthy(LogLevel::DEBUG, DebugCode::SCALE_SCAN_WAITING, true));
  // NONE is never audited even for allowlisted codes.
  assert(!auditWorthy(LogLevel::NONE, DebugCode::MACHINE_MUX_WAIT, true));
}

static void testLatchCaptureAndPaging() {
  AuditLogLatch latch;
  assert(latch.count() == 0);
  assert(latch.latestSequence() == 0);
  assert(latch.add(100, 1000, LogLevel::WARNING, DebugCategory::NETWORK,
                   DebugCode::MACHINE_MUX_WAIT, 850000, 12000,
                   "micra mutex wait while backflush poll held it"));
  assert(latch.add(400, 1300, LogLevel::ERROR, DebugCategory::CONFIG,
                   DebugCode::CONFIG_REJECTED, 7, 0, nullptr));
  AuditRecord page[8];
  assert(latch.copyAfter(0, page, 8) == 2);
  assert(page[0].code == static_cast<uint8_t>(DebugCode::MACHINE_MUX_WAIT));
  assert(page[0].sequence == 1);
  assert(page[0].argument1 == 850000);
  assert(page[0].textLen > 0 && page[0].textLen < AUDIT_EXCERPT_BYTES);
  assert(strncmp(page[0].text, "micra mutex wait", 16) == 0);
  assert(page[1].sequence == 2);
  assert(page[1].textLen == 0 && page[1].text[0] == '\0');
  // Sequence paging mirrors the common ring: after=1 returns only newer.
  assert(latch.copyAfter(1, page, 8) == 1);
  assert(page[0].sequence == 2);
  assert(latch.latestSequence() == 2);
  // Zero-capacity and null-output reads are safe no-ops.
  assert(latch.copyAfter(0, nullptr, 0) == 0);
  assert(latch.copyAfter(0, page, 0) == 0);
}

static void testExcerptTruncation() {
  AuditLogLatch latch;
  const char *longDetail =
      "0123456789012345678901234567890123456789012345678901234567890123";
  assert(latch.add(10, 10, LogLevel::ERROR, DebugCategory::SYSTEM,
                   DebugCode::INITIALIZATION_FAILED, 0, 0, longDetail));
  AuditRecord record;
  assert(latch.copyAfter(0, &record, 1) == 1);
  assert(record.textLen == AUDIT_EXCERPT_BYTES - 1);
  assert(record.text[AUDIT_EXCERPT_BYTES - 1] == '\0');
}

static void testPerCodeRateLimit() {
  AuditLogLatch latch;
  uint32_t atMs = 5000;
  for (uint8_t i = 0; i < AUDIT_RATE_LIMIT; ++i) {
    assert(latch.add(atMs, 100, LogLevel::WARNING, DebugCategory::NETWORK,
                     DebugCode::MACHINE_MUX_WAIT, i, 0, nullptr));
  }
  assert(latch.count() == AUDIT_RATE_LIMIT);
  // The 11th event of the same code inside the window is suppressed.
  assert(!latch.add(atMs + 100, 100, LogLevel::WARNING, DebugCategory::NETWORK,
                    DebugCode::MACHINE_MUX_WAIT, 10, 0, nullptr));
  assert(latch.count() == AUDIT_RATE_LIMIT);
  assert(latch.rateLimited() == 1);
  // A different code is unaffected by the saturated slot.
  assert(latch.add(atMs + 100, 100, LogLevel::WARNING, DebugCategory::RELAY,
                   DebugCode::RELAY_GPIO_DESYNC, 1, 0, nullptr));
  // After the window rolls over the same code is admitted again.
  atMs += AUDIT_RATE_WINDOW_MS + 1;
  assert(latch.add(atMs, 100, LogLevel::WARNING, DebugCategory::NETWORK,
                   DebugCode::MACHINE_MUX_WAIT, 20, 0, nullptr));
  assert(latch.rateLimited() == 1);
}

static void testRingWrapAndClear() {
  AuditLogLatch latch;
  // Spacing stays under ten events per rate window so every add is admitted.
  for (uint32_t i = 0; i < AUDIT_RECORD_CAPACITY + 10; ++i) {
    assert(latch.add(i * 6010, i, LogLevel::ERROR, DebugCategory::SYSTEM,
                     DebugCode::INITIALIZATION_FAILED, static_cast<int32_t>(i),
                     0, nullptr));
  }
  assert(latch.count() == AUDIT_RECORD_CAPACITY);
  assert(latch.overwritten() == 10);
  AuditRecord page[AUDIT_RECORD_CAPACITY];
  assert(latch.copyAfter(0, page, AUDIT_RECORD_CAPACITY) ==
         AUDIT_RECORD_CAPACITY);
  assert(page[0].argument1 == 10);
  assert(page[AUDIT_RECORD_CAPACITY - 1].argument1 ==
          static_cast<int32_t>(AUDIT_RECORD_CAPACITY + 9));
  latch.clear();
  assert(latch.count() == 0);
  assert(latch.overwritten() == 0);
  assert(latch.rateLimited() == 0);
  assert(latch.latestSequence() == 0);
}

// The JSON surfaces rebuild the full detail from the stored whole-gram args
// plus the emit-time excerpt, so even records already on flash (arg-only)
// render with meaningful text instead of an empty column.
static void testFormatAuditRecordDetail() {
  char detail[128];
  AuditRecord record;
  record.code = static_cast<uint8_t>(DebugCode::HEALTH_HEAP_LOW);
  record.argument1 = 31000;
  record.argument2 = 12000;
  formatAuditRecordDetail(record, detail, sizeof(detail));
  assert(strcmp(detail,
                "health heap low free=31000 largest=12000"
                " (limits 49152/16384 bytes)") == 0);

  record = AuditRecord{};
  record.code = static_cast<uint8_t>(DebugCode::HEALTH_LOOP_GAP);
  record.argument1 = 320;
  formatAuditRecordDetail(record, detail, sizeof(detail));
  assert(strcmp(detail, "health loop gap high max=320 ms (limit 200 ms)") == 0);
  record.argument2 = 1500;
  record.textLen = 22;
  memcpy(record.text, "idle=300000 us flash=1", 22);
  formatAuditRecordDetail(record, detail, sizeof(detail));
  assert(strcmp(detail, "health loop gap high max=320 ms (limit 200 ms)"
                        " loop cpu=1500 us idle=300000 us flash=1") == 0);

  record = AuditRecord{};
  record.code = static_cast<uint8_t>(DebugCode::MACHINE_MUX_WAIT);
  record.argument1 = 25000;
  record.argument2 = 30000;
  record.textLen = 10;
  memcpy(record.text, "holder=net", 10);
  formatAuditRecordDetail(record, detail, sizeof(detail));
  assert(strcmp(detail,
                "machine service mux wait=25000 us hold=30000 us"
                " holder=net") == 0);

  record = AuditRecord{};
  record.code = static_cast<uint8_t>(DebugCode::STA_FAILED);
  record.argument1 = 6;
  formatAuditRecordDetail(record, detail, sizeof(detail));
  assert(strcmp(detail, "station connection failed: disconnected (status 6)") ==
         0);
  record.argument1 = 42;
  formatAuditRecordDetail(record, detail, sizeof(detail));
  assert(strcmp(detail, "station connection failed: unknown (status 42)") == 0);
  record.argument1 = 5;
  record.argument2 = packStaDisconnectDetail(200, -79);
  formatAuditRecordDetail(record, detail, sizeof(detail));
  assert(strcmp(detail, "station connection failed: connection lost (status 5)"
                        " reason=200 beacon timeout rssi=-79 dBm") == 0);
  record.argument1 = 6;
  record.argument2 = packStaDisconnectDetail(23, -61);
  formatAuditRecordDetail(record, detail, sizeof(detail));
  assert(strcmp(detail, "station connection failed: disconnected (status 6)"
                        " reason=23 rssi=-61 dBm") == 0);

  // Codes without a dedicated formatter still expose their recorded args.
  record = AuditRecord{};
  record.code = static_cast<uint8_t>(DebugCode::RELAY_GPIO_DESYNC);
  record.argument1 = 7;
  record.argument2 = -2;
  formatAuditRecordDetail(record, detail, sizeof(detail));
  assert(strcmp(detail, "arg1=7 arg2=-2") == 0);

  // Nothing recorded beyond the classification: empty detail, not invented.
  record = AuditRecord{};
  record.code = static_cast<uint8_t>(DebugCode::SAFETY_LOCKOUT_ACTIVE);
  formatAuditRecordDetail(record, detail, sizeof(detail));
  assert(detail[0] == '\0');
  // A short output buffer truncates safely instead of overflowing.
  char small[16];
  record.code = static_cast<uint8_t>(DebugCode::HEALTH_LOOP_GAP);
  record.argument1 = 320;
  formatAuditRecordDetail(record, small, sizeof(small));
  assert(strlen(small) < sizeof(small));
}

int main() {
  testClassification();
  testLatchCaptureAndPaging();
  testExcerptTruncation();
  testPerCodeRateLimit();
  testRingWrapAndClear();
  testFormatAuditRecordDetail();
  printf("audit_log_host_test PASSED\n");
  return 0;
}
