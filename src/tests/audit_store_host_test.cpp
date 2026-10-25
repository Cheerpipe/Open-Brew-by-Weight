// Durable audit store contracts: packed roundtrip, torn record terminal, wrap
// by sector erase, header-free boot scan, and clear — exercised against the RAM partition fake with deterministic I/O
// failure injection. OPEN_BREW_BY_WEIGHT_AUDIT_HOST_TEST comes from CMake.
#include "../OpenBrewByWeightAuditStore.cpp"

#include <cstdlib>
#include <iostream>

using namespace openbrewbyweight;

static AuditRecord makeRecord(uint32_t sequence, int32_t arg1) {
  AuditRecord record;
  record.sequence = sequence;
  record.wallSec = 1700000000 + sequence * 10;
  record.atMs = sequence * 1000;
  record.level = static_cast<uint8_t>(LogLevel::WARNING);
  record.category = static_cast<uint8_t>(DebugCategory::NETWORK);
  record.code = static_cast<uint8_t>(DebugCode::MACHINE_MUX_WAIT);
  record.argument1 = arg1;
  record.argument2 = 0;
  const char *text = "mux wait while holder kept the lock";
  for (size_t i = 0; i + 1 < sizeof(record.text) && text[i]; ++i) {
    record.text[i] = text[i];
    ++record.textLen;
  }
  return record;
}

static void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

static void resetStore() {
  std::fill(fakeAuditLog.bytes.begin(), fakeAuditLog.bytes.end(), 0xFF);
  fakeAuditLogPresent = true;
  fakeAuditOps = 0;
  fakeAuditFailReadAt = fakeAuditFailWriteAt = fakeAuditFailEraseAt = 0;
  g_status = AuditStoreStatus{};
}

static void testRoundtripAndPaging() {
  resetStore();
  AuditLogLatch latch;
  uint32_t pendingAfter = 0;
  auditStoreBootOpen(latch, pendingAfter);
  check(auditStoreStatus().state == AuditStoreState::READY,
        "empty store must open ready");
  check(pendingAfter == 0 && auditStoreStatus().retained == 0,
        "empty store must retain nothing");
  for (uint32_t seq = 1; seq <= 20; ++seq) {
    check(auditStoreAppend(makeRecord(seq, static_cast<int32_t>(seq))),
          "append failed");
  }
  check(auditStoreStatus().seqHigh == 20 && auditStoreStatus().retained == 20,
        "cursor accounting is wrong after appends");
  AuditRecord page[7];
  check(auditStoreCopyAfter(0, page, 7) == 7 && page[0].sequence == 1 &&
            page[6].sequence == 7,
        "first page must start at the oldest retained record");
  check(auditStoreCopyAfter(7, page, 7) == 7 && page[0].sequence == 8 &&
            page[6].sequence == 14,
        "paging after a sequence must resume at the next record");
  check(auditStoreCopyAfter(18, page, 7) == 2 && page[1].sequence == 20,
        "the final page must return only newer records");
  check(auditStoreCopyAfter(20, page, 7) == 0, "nothing follows seqHigh");
}

static void testBootBackfill() {
  AuditLogLatch latch;
  uint32_t pendingAfter = 12345;
  auditStoreBootOpen(latch, pendingAfter);
  check(latch.count() == 20, "boot must backfill the whole retained window");
  check(pendingAfter == 20,
        "backfilled records must be marked already durable");
  check(latch.latestSequence() == 20,
        "the latch sequence must continue from the store");
  check(latch.add(999, 999, LogLevel::WARNING, DebugCategory::NETWORK,
                  DebugCode::MACHINE_MUX_WAIT, 5, 0, nullptr),
        "post-boot add must be admitted");
  check(latch.latestSequence() == 21, "new records continue the sequence");
  check(latch.hasPending(), "the new record must start pending");
  latch.markFlushedThrough(21);
  check(!latch.hasPending(), "marking through the sequence clears pending");
}

static void testTornRecordTerminal() {
  resetStore();
  AuditLogLatch latch;
  uint32_t pendingAfter = 0;
  auditStoreBootOpen(latch, pendingAfter);
  for (uint32_t seq = 1; seq <= 5; ++seq) {
    check(auditStoreAppend(makeRecord(seq, 1)), "append failed");
  }
  // Tear the newest record: an unreadable version byte fails the decode.
  const size_t offset = 4 * AUDIT_STORE_RECORD_BYTES;
  fakeAuditLog.bytes[offset] = 0x02;
  AuditLogLatch recovered;
  auditStoreBootOpen(recovered, pendingAfter);
  check(auditStoreStatus().retained == 4,
        "a torn record must terminate the window");
  AuditRecord page[8];
  check(auditStoreCopyAfter(0, page, 8) == 4 && page[3].sequence == 4,
        "the torn record must not be readable");
}

static void testFullWrap() {
  resetStore();
  AuditLogLatch latch;
  uint32_t pendingAfter = 0;
  auditStoreBootOpen(latch, pendingAfter);
  for (uint32_t seq = 1; seq <= AUDIT_STORE_RECORD_CAPACITY + 150; ++seq) {
    check(auditStoreAppend(makeRecord(seq, static_cast<int32_t>(seq))),
          "append failed during wrap");
  }
  check(auditStoreStatus().retained <= AUDIT_STORE_RECORD_CAPACITY &&
            auditStoreStatus().retained >=
                AUDIT_STORE_RECORD_CAPACITY - kRecordsPerSector + 1,
        "retention must cap at one window");
  AuditRecord page[8];
  const size_t count = auditStoreCopyAfter(0, page, 8);
  const uint32_t oldest = auditStoreStatus().seqHigh -
                          auditStoreStatus().retained + 1;
  check(count == 8 && page[0].sequence == oldest &&
            page[7].sequence == oldest + 7,
        "the wrap must retire the oldest records");
  AuditLogLatch recovered;
  auditStoreBootOpen(recovered, pendingAfter);
  check(auditStoreStatus().retained >= AUDIT_STORE_RECORD_CAPACITY - kRecordsPerSector + 1 &&
            auditStoreStatus().seqHigh == AUDIT_STORE_RECORD_CAPACITY + 150,
        "boot after wrap must restore the full window");
  check(auditStoreCopyAfter(AUDIT_STORE_RECORD_CAPACITY + 143, page, 8) == 7,
        "the newest records survive the wrap boot");
}

// Full ring with the cursor on a not-yet-erased sector: every slot decodes,
// so the scan must rotate the window to the sequence drop.
static void testFullRingScanFindsOldest() {
  resetStore();
  AuditLogLatch latch;
  uint32_t pendingAfter = 0;
  auditStoreBootOpen(latch, pendingAfter);
  const uint32_t total = AUDIT_STORE_RECORD_CAPACITY + 2 * kRecordsPerSector;
  for (uint32_t seq = 1; seq <= total; ++seq) {
    check(auditStoreAppend(makeRecord(seq, 1)), "append failed");
  }
  AuditLogLatch recovered;
  auditStoreBootOpen(recovered, pendingAfter);
  AuditRecord page[2];
  check(auditStoreStatus().retained == AUDIT_STORE_RECORD_CAPACITY &&
            auditStoreCopyAfter(0, page, 2) == 2 &&
            page[0].sequence == total - AUDIT_STORE_RECORD_CAPACITY + 1,
        "a full-ring scan must start the window at the oldest record");
  check(auditStoreAppend(makeRecord(total + 1, 1)) &&
            auditStoreCopyAfter(total, page, 2) == 1,
        "the next append must land after the newest record");
}

// Stale bytes in the first lap's next sector must not retire the window.
static void testFirstLapStaleSectorKeepsWindow() {
  resetStore();
  std::fill(fakeAuditLog.bytes.begin() + slotOffset(kRecordsPerSector),
            fakeAuditLog.bytes.begin() + slotOffset(2 * kRecordsPerSector),
            0x00);
  AuditLogLatch latch;
  uint32_t pendingAfter = 0;
  auditStoreBootOpen(latch, pendingAfter);
  for (uint32_t seq = 1; seq <= kRecordsPerSector + 1; ++seq) {
    check(auditStoreAppend(makeRecord(seq, 1)), "append failed");
  }
  AuditRecord page[2];
  check(auditStoreStatus().retained == kRecordsPerSector + 1 &&
            auditStoreCopyAfter(0, page, 2) == 2 && page[0].sequence == 1,
        "erasing a stale first-lap sector must keep the window");
}

static void testAbsentPartitionAndClear() {
  resetStore();
  fakeAuditLogPresent = false;
  AuditLogLatch latch;
  uint32_t pendingAfter = 0;
  auditStoreBootOpen(latch, pendingAfter);
  check(auditStoreStatus().state == AuditStoreState::UNSUPPORTED,
        "an absent partition must degrade to RAM mode");
  check(!auditStoreAppend(makeRecord(1, 1)), "append without a store fails");
  check(auditStoreEraseAll(), "clear without a store succeeds");
  fakeAuditLogPresent = true;
  auditStoreBootOpen(latch, pendingAfter);
  for (uint32_t seq = 1; seq <= 5; ++seq) {
    check(auditStoreAppend(makeRecord(seq, 1)), "append failed");
  }
  check(auditStoreEraseAll(),
        "clear must succeed on a live store");
  check(auditStoreStatus().retained == 0 && auditStoreStatus().seqHigh == 0,
        "clear must reset the window");
}

static void testIoFailureLeavesWindowConsistent() {
  resetStore();
  AuditLogLatch latch;
  uint32_t pendingAfter = 0;
  auditStoreBootOpen(latch, pendingAfter);
  for (uint32_t seq = 1; seq <= 4; ++seq) {
    check(auditStoreAppend(makeRecord(seq, 1)), "append failed");
  }
  fakeAuditFailWriteAt = fakeAuditOps + 1;
  check(!auditStoreAppend(makeRecord(5, 1)),
        "a failed program must report failure");
  fakeAuditFailWriteAt = 0;
  AuditLogLatch recovered;
  auditStoreBootOpen(recovered, pendingAfter);
  check(auditStoreStatus().retained == 4,
        "a failed program must not corrupt the window");
  check(auditStoreAppend(makeRecord(6, 1)), "append works after the failure");
  AuditRecord page[8];
  check(auditStoreCopyAfter(0, page, 8) == 5 && page[4].sequence == 6,
        "post-failure appends must be retained");
}

// Torn slots never move the cursor behind the newest record: many tears in
// one lap stay readable around them, and a tear at the body end followed by a
// wrap keeps the post-wrap records as the newest window.
static void testTornSlotsKeepNewestWindow() {
  resetStore();
  AuditLogLatch latch;
  uint32_t pendingAfter = 0;
  auditStoreBootOpen(latch, pendingAfter);
  for (uint32_t seq = 1; seq <= 40; ++seq) {
    check(auditStoreAppend(makeRecord(seq, 1)), "append failed");
  }
  for (uint32_t slot = 3; slot <= 30; slot += 3) {
    fakeAuditLog.bytes[slotOffset(slot)] = 0x02;
  }
  AuditLogLatch recovered;
  auditStoreBootOpen(recovered, pendingAfter);
  AuditRecord page[64];
  check(auditStoreStatus().state == AuditStoreState::READY &&
            auditStoreStatus().seqHigh == 40 &&
            auditStoreCopyAfter(0, page, 64) == 30,
        "torn slots must be skipped, not end the store");
  check(auditStoreAppend(makeRecord(41, 1)) &&
            auditStoreCopyAfter(39, page, 64) == 2 && page[1].sequence == 41,
        "the next append must follow the newest record");

  resetStore();
  auditStoreBootOpen(latch, pendingAfter);
  const uint32_t last = AUDIT_STORE_RECORD_CAPACITY - 1;
  for (uint32_t seq = 1; seq <= last; ++seq) {
    check(auditStoreAppend(makeRecord(seq, 1)), "append failed");
  }
  std::fill(fakeAuditLog.bytes.begin() + slotOffset(last),
            fakeAuditLog.bytes.begin() + slotOffset(last) + AUDIT_STORE_RECORD_BYTES,
            0x00);
  auditStoreBootOpen(recovered, pendingAfter);
  for (uint32_t seq = last + 1; seq <= last + 6; ++seq) {
    check(auditStoreAppend(makeRecord(seq, 1)), "append after the tear failed");
  }
  auditStoreBootOpen(recovered, pendingAfter);
  check(auditStoreStatus().seqHigh == last + 6 &&
            auditStoreCopyAfter(last + 3, page, 64) == 3,
        "a tear at the body end must not hide the post-wrap records");
  check(auditStoreAppend(makeRecord(last + 7, 1)) &&
            auditStoreCopyAfter(last + 6, page, 64) == 1,
        "appends after the wrap boot must continue after the newest record");
}

int main() {
  testRoundtripAndPaging();
  testBootBackfill();
  testTornRecordTerminal();
  testFullWrap();
  testFullRingScanFindsOldest();
  testFirstLapStaleSectorKeepsWindow();
  testAbsentPartitionAndClear();
  testIoFailureLeavesWindowConsistent();
  testTornSlotsKeepNewestWindow();
  std::cout << "audit_store_host_test PASSED\n";
  return 0;
}
