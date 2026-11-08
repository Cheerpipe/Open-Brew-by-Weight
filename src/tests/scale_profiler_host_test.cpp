#define OPEN_BREW_BY_WEIGHT_HOST_TEST

#include "../OpenBrewByWeightScaleProfiler.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

int failures = 0;

#define CHECK(condition)                                              \
  do {                                                                \
    if (!(condition)) {                                               \
      std::cerr << __func__ << ":" << __LINE__                        \
                << ": check failed: " << #condition << "\n";          \
      ++failures;                                                     \
      return;                                                         \
    }                                                                 \
  } while (false)

using namespace openbrewbyweight;

const uint32_t T0 = 100000;

void noteWeightAt(ScaleProfiler &profiler, uint32_t atMs, float weightG,
                  uint32_t captureSequence, uint32_t generation = 2) {
  profiler.noteWeight({{generation, captureSequence}, atMs, weightG}, false, atMs);
}

void testLifecycleCapturesWeightsAndEvents() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  CHECK(profiler.hostState() == ScaleProfilerState::RECORDING);
  CHECK(profiler.hostRecordCount() == 1);  // PROFILE_START marker
  noteWeightAt(profiler, T0 + 102, 125.375f, 41);
  noteWeightAt(profiler, T0 + 203, 0.025f, 42);
  noteWeightAt(profiler, T0 + 203, 0.025f, 43);  // duplicate stays
  noteWeightAt(profiler, T0 + 204, -3.5f, 44);   // negative stays
  profiler.noteEvent({.connectionGeneration = 2, .sequence = 40, .weightG = NAN, .arg1 = 7,
                      .arg2 = static_cast<uint32_t>(ScaleProfileTareOrigin::IDLE_CUP),
                      .kind = static_cast<uint16_t>(ScaleProfileEvent::TARE_REQUEST)},
                     T0 + 105);
  CHECK(profiler.hostRecordCount() == 6);
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 1500);
  CHECK(profiler.hostState() == ScaleProfilerState::STOPPED);
}

void testStopReasonUserIsLastRecord() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  noteWeightAt(profiler, T0 + 10, 1.0f, 1);
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 500);
  CHECK(profiler.hostState() == ScaleProfilerState::STOPPED);
  const uint32_t count = profiler.hostRecordCount();
  const ScaleProfileRecord terminal = profiler.hostRecord(count - 1);
  CHECK(terminal.kind ==
        static_cast<uint16_t>(ScaleProfileEvent::PROFILE_STOP));
  CHECK(terminal.arg1 == static_cast<uint32_t>(ScaleProfilerStopReason::USER));
  CHECK(terminal.relativeMs == 500);
  // Late producers cannot enter the closed generation.
  noteWeightAt(profiler, T0 + 600, 9.0f, 99);
  CHECK(profiler.hostRecordCount() == count);
}

void testPreEpochAndWrapAroundTimestamps() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  noteWeightAt(profiler, T0 - 5, 1.0f, 1);  // Before the epoch: rejected.
  CHECK(profiler.hostRecordCount() == 1);
  CHECK(profiler.hostLostCount() == 0);
  ScaleProfiler wrapped;
  const uint32_t wrapEpoch = 0xFFFFFF00U;
  CHECK(wrapped.hostStartNoFlash(wrapEpoch));
  wrapped.noteWeight({{1, 7}, 400U, 2.5f}, false, 400U);  // millis() wrapped.
  CHECK(wrapped.hostRecordCount() == 2);
  CHECK(wrapped.hostRecord(1).relativeMs == 0x290);
}

void testRingKeepsNewestAndCountsOverwritten() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  const uint32_t extra = 100;
  for (uint32_t i = 1; i <= SCALE_PROFILE_RECORD_CAPACITY + extra; ++i)
    noteWeightAt(profiler, T0 + i, 1.0f, i);
  profiler.hostServiceIdle(T0 + SCALE_PROFILE_RECORD_CAPACITY + extra + 1);
  // No capacity stop: the oldest records (start marker first) are overwritten.
  CHECK(profiler.hostState() == ScaleProfilerState::RECORDING);
  CHECK(profiler.hostRecordCount() == SCALE_PROFILE_RECORD_CAPACITY);
  CHECK(profiler.hostLostCount() == 0);
  const ScaleProfilerStatus status = profiler.status(T0 + 20000);
  CHECK(status.overwritten == extra + 1);
  CHECK(status.windowMs == SCALE_PROFILE_RECORD_CAPACITY - 1);
  CHECK(status.weightCount == SCALE_PROFILE_RECORD_CAPACITY + extra);
  CHECK(profiler.hostRecord(0).sequence == extra + 1);
  CHECK(profiler.hostRecord(SCALE_PROFILE_RECORD_CAPACITY - 1).sequence ==
        SCALE_PROFILE_RECORD_CAPACITY + extra);
  // Ordinals keep counting every append past the ring size.
  CHECK((profiler.hostRecord(0).ordinal & 0xFFFFFFU) == extra + 2);
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 20000);
  CHECK(profiler.hostRecord(SCALE_PROFILE_RECORD_CAPACITY - 1).kind ==
        static_cast<uint16_t>(ScaleProfileEvent::PROFILE_STOP));
  CHECK(profiler.hostHeader().omittedCount == extra + 2);
}

void testCaptureHasNoTimeLimit() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  noteWeightAt(profiler, T0 + 100, 1.0f, 1);
  for (const uint32_t elapsed : {180000U, 180040U, 300000U, 3600000U}) {
    profiler.hostServiceIdle(T0 + elapsed);
    CHECK(profiler.hostState() == ScaleProfilerState::RECORDING);
    CHECK(profiler.status(T0 + elapsed).elapsedMs == elapsed);
    CHECK(profiler.hostRecordCount() == 2);
  }
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 3600001);
  CHECK(profiler.hostHeader().durationMs == 3600001);
}

void testLongCaptureTimestampsAndExport() {
  ScaleProfiler profiler;
  const uint32_t start = 0xffffff00U;
  CHECK(profiler.hostStartNoFlash(start));
  for (uint64_t day = 1; day <= 60; ++day) {
    const uint64_t elapsed = day * 86400000;
    const uint32_t now = start + static_cast<uint32_t>(elapsed);
    profiler.hostServiceIdle(now);
    noteWeightAt(profiler, now, 1.0f, static_cast<uint32_t>(day));
    CHECK(profiler.status(now).elapsedMs == elapsed);
    CHECK(scaleProfileRecordTimeMs(profiler.hostRecord(day)) == elapsed);
  }
  // 24-bit append counter below the 8-bit time extension.
  CHECK((profiler.hostRecord(60).ordinal & 0xFFFFFFU) == 61);
  CHECK(profiler.hostRecord(60).ordinal >> SCALE_PROFILE_ORDINAL_BITS == 1);
  const uint64_t duration = 60ULL * 86400000;
  const uint32_t end = start + static_cast<uint32_t>(duration);
  profiler.noteWeight({{2, 61}, end - 10, 2.0f}, false, end);
  profiler.hostStop(ScaleProfilerStopReason::USER, end + 1);
  profiler.hostServiceIdle(end + 2);
  CHECK(scaleProfileDurationMs(profiler.hostHeader()) == duration + 1);
  CHECK(scaleProfileRecordTimeMs(profiler.hostExportRecord(60)) == duration - 10);
  CHECK(scaleProfileRecordTimeMs(profiler.hostExportRecord(61)) == duration);
  char line[192];
  CHECK(formatScaleProfileRow(line, sizeof(line), profiler.hostExportRecord(61)));
  CHECK(std::string(line).find("+5184000.000") == 0);
  CHECK(static_cast<uint32_t>(ScaleProfilerStopReason::BUFFER_FULL) == 3);
  CHECK(formatScaleProfileRow(line, sizeof(line), profiler.hostExportRecord(0)));
  CHECK(std::string(line).find("limit=rolling") != std::string::npos);
}

void testIndexOrdersByTimeThenOrdinal() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  noteWeightAt(profiler, T0 + 300, 3.0f, 30);
  noteWeightAt(profiler, T0 + 100, 1.0f, 10);
  // Same time as the first weight, later ordinal.
  profiler.noteEvent({.connectionGeneration = 2, .sequence = 10, .weightG = 1.0f, .arg1 = 5,
                      .kind = static_cast<uint16_t>(ScaleProfileEvent::WEIGHT_ACCEPTED)},
                     T0 + 100);
  noteWeightAt(profiler, T0 + 200, 2.0f, 20);
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 400);
  profiler.hostServiceIdle(T0 + 500);
  CHECK(profiler.hostExportRecord(1).sequence == 10);
  CHECK(profiler.hostExportRecord(1).kind ==
        static_cast<uint16_t>(ScaleProfileEvent::WEIGHT));
  CHECK(profiler.hostExportRecord(2).kind ==
        static_cast<uint16_t>(ScaleProfileEvent::WEIGHT_ACCEPTED));
  CHECK(profiler.hostExportRecord(2).ordinal ==
        profiler.hostExportRecord(1).ordinal + 1);
  CHECK(profiler.hostExportRecord(3).sequence == 20);
  CHECK(profiler.hostExportRecord(4).sequence == 30);
}

void testTxtRowShapeAndPrecision() {
  ScaleProfileRecord weight = {};
  weight.relativeMs = 102;
  weight.connectionGeneration = 2;
  weight.sequence = 41;
  weight.weightG = 125.375f;
  weight.kind = static_cast<uint16_t>(ScaleProfileEvent::WEIGHT);
  char line[160];
  CHECK(formatScaleProfileRow(line, sizeof(line), weight) > 0);
  CHECK(std::string(line) ==
        "+000000.102 \xE2\x80\x93 125.375|WEIGHT seq=41 connection=2");

  ScaleProfileRecord weightDisc = weight;
  weightDisc.flags = SCALE_PROFILE_FLAG_DISCONTINUITY;
  CHECK(formatScaleProfileRow(line, sizeof(line), weightDisc) > 0);
  CHECK(std::string(line).find("disc=1") != std::string::npos);

  ScaleProfileRecord event = {};
  event.relativeMs = 105;
  event.kind = static_cast<uint16_t>(ScaleProfileEvent::TARE_REQUEST);
  event.arg1 = 7;
  event.arg2 = static_cast<uint32_t>(ScaleProfileTareOrigin::IDLE_CUP);
  event.sequence = 40;
  CHECK(formatScaleProfileRow(line, sizeof(line), event) > 0);
  CHECK(std::string(line) ==
        "+000000.105 \xE2\x80\x93 \xE2\x80\x94|TARE_REQUEST request=7 origin=2 "
        "seq=40");

  // Nine significant digits round-trip the captured float32.
  ScaleProfileRecord precise = {};
  precise.relativeMs = 1;
  precise.weightG = 0.123456791f;
  precise.kind = static_cast<uint16_t>(ScaleProfileEvent::WEIGHT);
  CHECK(formatScaleProfileRow(line, sizeof(line), precise) > 0);
  float roundTrip = 0.0f;
  const char *field = std::strchr(line, '|');
  CHECK(field != nullptr);
  CHECK(std::sscanf(line, "+000000.001 \xE2\x80\x93 %g|", &roundTrip) == 1);
  CHECK(roundTrip == precise.weightG);
}

void testDownloadLeasePinsGeneration() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  noteWeightAt(profiler, T0 + 10, 1.0f, 1);
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 100);
  CHECK(profiler.acquireDownloadLease());
  CHECK(profiler.downloadLeaseGeneration() != 0);
  CHECK(profiler.downloadFromRam());
  CHECK(!profiler.downloadLive());
  CHECK(profiler.downloadRecordCount() == 3);
  // Start and Delete refuse while the lease pins the frozen trace.
  (void)profiler.hostService(ScaleProfilerRequest::START, T0 + 200);
  CHECK(profiler.hostState() == ScaleProfilerState::STOPPED);
  CHECK(profiler.hostLastError() == ScaleProfilerError::BUSY);
  (void)profiler.hostService(ScaleProfilerRequest::DELETE, T0 + 300);
  CHECK(profiler.hostState() == ScaleProfilerState::STOPPED);
  profiler.releaseDownloadLease();
  // Let the dispatched save finish; the trace becomes flash-backed.
  profiler.hostServiceIdle(T0 + 400);
  profiler.hostPublishFlashResult(true);
  profiler.hostServiceIdle(T0 + 500);
  CHECK(profiler.hostState() == ScaleProfilerState::SAVED);
  CHECK(profiler.acquireDownloadLease());
  (void)profiler.hostService(ScaleProfilerRequest::DELETE, T0 + 600);
  CHECK(profiler.hostState() == ScaleProfilerState::SAVED);
  profiler.releaseDownloadLease();
  (void)profiler.hostService(ScaleProfilerRequest::DELETE, T0 + 700);
  CHECK(profiler.hostState() == ScaleProfilerState::EMPTY);
}

void testSaveCompletingMidDownloadKeepsRamStream() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  noteWeightAt(profiler, T0 + 10, 1.0f, 1);
  noteWeightAt(profiler, T0 + 20, 2.0f, 2);
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 100);
  profiler.hostServiceIdle(T0 + 200);  // Dispatches the save.
  CHECK(profiler.acquireDownloadLease());
  // The save finishes while the export is mid-stream: the lease keeps the
  // RAM generation readable instead of fabricating zero rows.
  profiler.hostPublishFlashResult(true);
  profiler.hostServiceIdle(T0 + 300);
  CHECK(profiler.hostState() == ScaleProfilerState::SAVED);
  CHECK(profiler.downloadFromRam());
  CHECK(profiler.downloadRecordCount() == 4);
  ScaleProfileRecord rows[4];
  CHECK(profiler.downloadRecords(0, rows, 4) == 4);
  CHECK(rows[0].kind == static_cast<uint16_t>(ScaleProfileEvent::PROFILE_START));
  CHECK(rows[1].sequence == 1);
  CHECK(rows[2].sequence == 2);
  CHECK(rows[3].kind == static_cast<uint16_t>(ScaleProfileEvent::PROFILE_STOP));
  const ScaleProfileHeader header = profiler.downloadHeader();
  CHECK(header.weightCount == 2);
  CHECK(header.eventCount == 2);  // start + terminal
  CHECK(header.recordCount == 4);
  profiler.releaseDownloadLease();
  profiler.hostServiceIdle(T0 + 400);  // Reclaims the saved workspace.
  CHECK(!profiler.downloadFromRam());
}

void testSaveResultTransitionsAndFailureRetry() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  noteWeightAt(profiler, T0 + 10, 1.0f, 1);
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 100);
  profiler.hostServiceIdle(T0 + 200);
  // Persisted ack: the trace becomes SAVED and RAM is released.
  profiler.hostPublishFlashResult(true);
  profiler.hostServiceIdle(T0 + 300);
  CHECK(profiler.hostState() == ScaleProfilerState::SAVED);
  CHECK(profiler.hostPersistence() == ScaleProfilerPersistence::SAVED);
  CHECK(!profiler.downloadFromRam());
  CHECK(profiler.status(T0 + 300).savedRecordCount == 3);

  ScaleProfiler failing;
  CHECK(failing.hostStartNoFlash(T0));
  failing.hostStop(ScaleProfilerStopReason::USER, T0 + 100);
  failing.hostServiceIdle(T0 + 200);
  failing.hostPublishFlashResult(false);
  failing.hostServiceIdle(T0 + 300);
  CHECK(failing.hostPersistence() == ScaleProfilerPersistence::FAILED);
  CHECK(failing.hostLastError() == ScaleProfilerError::SAVE_FAILED);
  // A stopped, failed trace stays downloadable from RAM.
  CHECK(failing.acquireDownloadLease());
  CHECK(failing.downloadFromRam());
  failing.releaseDownloadLease();
}

void testStartRetryNeverRestartsActiveSession() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  noteWeightAt(profiler, T0 + 10, 1.0f, 1);
  (void)profiler.hostService(ScaleProfilerRequest::START, T0 + 20);
  CHECK(profiler.hostState() == ScaleProfilerState::RECORDING);
  CHECK(profiler.hostRecordCount() == 2);  // No second PROFILE_START marker.
  CHECK(profiler.hostHeader().sessionId != 0);
  const uint32_t firstSession = profiler.hostHeader().sessionId;
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 100);
  profiler.hostServiceIdle(T0 + 200);
  profiler.hostPublishFlashResult(true);
  profiler.hostServiceIdle(T0 + 300);
  // A new Start replaces even an unsaved session once it can proceed.
  CHECK(profiler.hostStartNoFlash(T0 + 1000));
  CHECK(profiler.hostState() == ScaleProfilerState::RECORDING);
  CHECK(profiler.hostHeader().sessionId == firstSession + 1);
}

void testHeaderContextProviderRunsAtStart() {
  static uint32_t calls = 0;
  calls = 0;
  const auto provider = +[](char *text, size_t capacity) -> uint32_t {
    ++calls;
    snprintf(text, capacity, "firmware=test\nscale=host\n");
    return 1760000000U;
  };
  ScaleProfiler profiler;
  profiler.hostSetContextProvider(provider);
  CHECK(profiler.hostStartNoFlash(T0));
  CHECK(calls == 1);
  CHECK(profiler.hostHeader().startWallUtcSec == 1760000000U);
  CHECK(std::string(profiler.hostHeader().context) == "firmware=test\nscale=host\n");
}

void testBudgetMatchesPlan() {
  CHECK(SCALE_PROFILE_RECORD_CAPACITY * SCALE_PROFILE_RECORD_BYTES == 512U * 1024U);
  CHECK(SCALE_PROFILE_INDEX_BYTES == 128U * 1024U);
  // The flash partition is unchanged; it holds the newest half of the ring.
  CHECK(SCALE_PROFILE_PARTITION_BYTES == 0x41000);
  CHECK(SCALE_PROFILE_PARTITION_BYTES - SCALE_PROFILE_PAYLOAD_OFFSET ==
        SCALE_PROFILE_SAVED_CAPACITY * SCALE_PROFILE_RECORD_BYTES);
  CHECK(sizeof(ScaleProfileHeader) <= 4096);
  CHECK(static_cast<uint32_t>(ScaleProfilerStopReason::BUFFER_FULL) == 3 &&
        static_cast<uint32_t>(ScaleProfilerStopReason::SUPERSEDED) == 5);
}

void fillRing(ScaleProfiler &profiler, uint32_t count) {
  for (uint32_t i = 1; i <= count; ++i) noteWeightAt(profiler, T0 + i, 1.0f, i);
}

void testRestartSupersedesWithoutSaving() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  noteWeightAt(profiler, T0 + 10, 1.0f, 1);
  const uint32_t session = profiler.hostHeader().sessionId;
  CHECK(profiler.hostService(ScaleProfilerRequest::RESTART, T0 + 20) ==
        ScaleProfilerWork::NONE);
  CHECK(profiler.hostState() == ScaleProfilerState::RECORDING);
  CHECK(profiler.hostHeader().sessionId == session + 1);
  CHECK(profiler.hostPersistence() == ScaleProfilerPersistence::NONE);
  CHECK(profiler.hostRecordCount() == 1);  // Only the new start marker.
  CHECK(profiler.hostService(ScaleProfilerRequest::NONE, T0 + 30) ==
        ScaleProfilerWork::NONE);  // The superseded session never saves.
  // A live download pins the session: the restart waits for its release.
  CHECK(profiler.acquireDownloadLease());
  (void)profiler.hostService(ScaleProfilerRequest::RESTART, T0 + 40);
  CHECK(profiler.hostHeader().sessionId == session + 1);
  CHECK(profiler.hostLastError() == ScaleProfilerError::BUSY);
  profiler.releaseDownloadLease();
  (void)profiler.hostService(ScaleProfilerRequest::RESTART, T0 + 50);
  CHECK(profiler.hostHeader().sessionId == session + 2);
  CHECK(profiler.hostLastError() == ScaleProfilerError::NONE);
  // A stopped trace is replaced as well, its pending save dropped.
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 60);
  CHECK(profiler.hostService(ScaleProfilerRequest::RESTART, T0 + 70) ==
        ScaleProfilerWork::NONE);
  CHECK(profiler.hostState() == ScaleProfilerState::RECORDING);
  CHECK(profiler.hostHeader().sessionId == session + 3);
}

void testLiveDownloadWindowAndOvertake() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  fillRing(profiler, SCALE_PROFILE_RECORD_CAPACITY + 500);
  CHECK(profiler.acquireDownloadLease());
  CHECK(profiler.downloadLive() && profiler.downloadFromRam());
  const uint32_t window = SCALE_PROFILE_RECORD_CAPACITY - SCALE_PROFILE_LIVE_HEADROOM;
  CHECK(profiler.downloadRecordCount() == window);
  const ScaleProfileHeader header = profiler.downloadHeader();
  CHECK(header.recordCount == window);
  CHECK(header.omittedCount == SCALE_PROFILE_RECORD_CAPACITY + 501 - window);
  // The frozen window is the newest records, oldest first.
  ScaleProfileRecord rows[32];
  CHECK(profiler.downloadRecords(0, rows, 32) == 32);
  const uint32_t first = SCALE_PROFILE_RECORD_CAPACITY + 500 - window + 1;
  for (uint32_t i = 0; i < 32; ++i) CHECK(rows[i].sequence == first + i);
  // Recording continues; newer records stay out of this download.
  noteWeightAt(profiler, T0 + 30000, 2.0f, 99999);
  CHECK(profiler.hostState() == ScaleProfilerState::RECORDING);
  CHECK(profiler.downloadRecordCount() == window);
  // The writer overtakes unsent records: the read stops at the first one.
  for (uint32_t i = 0; i < SCALE_PROFILE_LIVE_HEADROOM + 99; ++i)
    noteWeightAt(profiler, T0 + 30001 + i, 3.0f, 100000 + i);
  CHECK(profiler.downloadRecords(32, rows, 32) == 0);
  CHECK(profiler.downloadRecords(100, rows, 32) == 32);
  CHECK(rows[0].sequence == first + 100);
  // A Stop mid-download keeps the stream; the save waits for the release.
  CHECK(profiler.hostService(ScaleProfilerRequest::STOP, T0 + 40000) ==
        ScaleProfilerWork::NONE);
  CHECK(profiler.hostState() == ScaleProfilerState::STOPPED);
  CHECK(profiler.downloadRecords(window - 1, rows, 32) == 1);
  CHECK(profiler.hostService(ScaleProfilerRequest::NONE, T0 + 40100) ==
        ScaleProfilerWork::NONE);
  profiler.releaseDownloadLease();
  CHECK(!profiler.downloadLive());
  CHECK(profiler.hostService(ScaleProfilerRequest::NONE, T0 + 40200) ==
        ScaleProfilerWork::SAVE);
}

void testFlashSaveKeepsNewestRecords() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  fillRing(profiler, SCALE_PROFILE_RECORD_CAPACITY + 10);
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 30000);
  CHECK(profiler.hostService(ScaleProfilerRequest::NONE, T0 + 30100) ==
        ScaleProfilerWork::SAVE);
  CHECK(profiler.hostSavedRecordCount() == SCALE_PROFILE_SAVED_CAPACITY);
  // Chronological, newest that fit: the stop marker closes the payload.
  const uint32_t written = SCALE_PROFILE_RECORD_CAPACITY + 12;
  CHECK(profiler.hostSavedRecord(0).sequence ==
        SCALE_PROFILE_RECORD_CAPACITY + 10 - SCALE_PROFILE_SAVED_CAPACITY + 2);
  CHECK(profiler.hostSavedRecord(SCALE_PROFILE_SAVED_CAPACITY - 1).kind ==
        static_cast<uint16_t>(ScaleProfileEvent::PROFILE_STOP));
  profiler.hostPublishFlashResult(true);
  profiler.hostServiceIdle(T0 + 30200);
  const ScaleProfilerStatus status = profiler.status(T0 + 30200);
  CHECK(status.state == ScaleProfilerState::SAVED);
  CHECK(status.savedRecordCount == SCALE_PROFILE_SAVED_CAPACITY);
  CHECK(status.overwritten == written - SCALE_PROFILE_SAVED_CAPACITY);
}

ScaleProfileAutoReason tick(ScaleProfileAutoSession &session, bool connected,
                            uint32_t generation, uint8_t reason, uint32_t nowMs,
                            bool recording = true, bool brewing = false) {
  return session.observe(connected, generation, reason, recording, brewing, nowMs);
}

void testAutoSessionDecisions() {
  constexpr uint8_t NONE = 0, SCAN_TIMEOUT = 3, POWER_OFF = 9, SUPERVISION = 14;
  using R = ScaleProfileAutoReason;
  ScaleProfileAutoSession session;
  // Boot: nothing records yet, so the first connection starts a session.
  CHECK(tick(session, false, 0, NONE, 0) == R::NONE);
  CHECK(tick(session, true, 1, NONE, 100, false) == R::FIRST);
  CHECK(tick(session, true, 1, NONE, 200, false) == R::FIRST);  // Until served.
  session.served(1);
  CHECK(tick(session, true, 1, NONE, 300) == R::NONE);
  // Power-off: latched at the drop edge, kept over later scan failures.
  CHECK(tick(session, false, 1, POWER_OFF, 400) == R::NONE);
  CHECK(tick(session, false, 1, SCAN_TIMEOUT, 500) == R::NONE);
  CHECK(tick(session, true, 2, SCAN_TIMEOUT, 5000) == R::POWER_OFF);
  CHECK(session.downReason() == POWER_OFF);
  session.served(2);
  // A lost link continues the running session.
  CHECK(tick(session, false, 2, SUPERVISION, 6000) == R::NONE);
  CHECK(tick(session, true, 3, SUPERVISION, 9000) == R::LOST_LINK);
  session.served(3);
  // The same lost link after 30 minutes or more starts a new session.
  CHECK(tick(session, false, 3, SUPERVISION, 10000) == R::NONE);
  CHECK(tick(session, true, 4, SCAN_TIMEOUT, 10000 + SCALE_PROFILE_SESSION_GAP_MS) == R::GAP);
  session.served(4);
  // Never during a brew cycle, even after a power-off.
  CHECK(tick(session, false, 4, POWER_OFF, 20000000) == R::NONE);
  CHECK(tick(session, true, 5, POWER_OFF, 20001000, true, true) == R::BREWING);
  // ...unless nothing records: there is no session to protect.
  CHECK(tick(session, true, 5, POWER_OFF, 20001000, false, true) == R::FIRST);
  session.served(5);
  // A drop and reconnection inside one tick latch on reconnection.
  CHECK(tick(session, true, 6, POWER_OFF, 20002000) == R::POWER_OFF);
  session.served(6);
  CHECK(tick(session, true, 7, SUPERVISION, 20003000) == R::LOST_LINK);
}

void testIdleTarePresentation() {
  CupTareDiagnostics tare;
  CHECK(idleTarePresentationCode(tare, false) == IDLE_WAITING_FOR_SETTLE);
  tare.absentObserved = true;
  CHECK(idleTarePresentationCode(tare, false) == IDLE_READY_FOR_CUP);
  tare.emptyReferenceBlocked = true;
  CHECK(idleTarePresentationCode(tare, false) == IDLE_UNCERTAIN);
  tare.emptyReferenceBlocked = false;
  tare.referenceKnown = false;
  CHECK(idleTarePresentationCode(tare, false) == IDLE_UNCERTAIN);
  tare.requestId = 1;
  CHECK(idleTarePresentationCode(tare, false) == IDLE_PENDING);
  tare.eligibilityReason = static_cast<uint8_t>(IdleTareReason::MACHINE_NOT_OFF);
  CHECK(std::string(idleTarePresentationName(idleTarePresentationCode(tare, false))) == "machine_not_off");
  tare = CupTareDiagnostics{};
  CHECK(idleTarePresentationCode(tare, true) == IDLE_REMOVE);
  tare.lastTerminalRequestId = 1;
  tare.placementId = tare.requestPlacementId = 2;
  CHECK(idleTarePresentationCode(tare, true) == IDLE_RETRY);
  tare.tared = true;
  CHECK(idleTarePresentationCode(tare, true) == IDLE_TARED);
}

void testStateObservations() {
  ScaleProfiler profiler;
  using S = ScaleProfileSignal;
  ScaleProfileValue values[] = {{S::CUP_PRESENT, 1}, {S::IDLE_REQUEST_ID, 42}};
  CHECK(profiler.hostStartNoFlash(T0));
  profiler.noteValues(values, {7, 23}, T0 + 1);
  CHECK(profiler.hostRecordCount() == 3);
  CHECK(profiler.hostRecord(1).flags & SCALE_PROFILE_INITIAL);
  profiler.noteValues(values, {7, 24}, T0 + 2);
  CHECK(profiler.hostRecordCount() == 3); // New sample alone is not a state change.
  values[0].value = 0;
  profiler.noteValues(values, {7, 25}, T0 + 3);
  const auto removed = profiler.hostRecord(3);
  CHECK(removed.arg1 == 1 && removed.arg2 == 0 && removed.sequence == 25);
  CHECK(!(removed.flags & SCALE_PROFILE_INITIAL));
  char line[192];
  CHECK(formatScaleProfileRow(line, sizeof(line), removed) != 0);
  CHECK(std::string(line).find("CUP_PRESENT from=1 to=0 capture=25 connection=7") != std::string::npos);
  profiler.noteValues(values, {8, 1}, T0 + 4);
  CHECK(profiler.hostRecordCount() == 6); // Reconnection renews unchanged evidence.
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 5);
  profiler.noteValues(values, {8, 2}, T0 + 6);
  CHECK(profiler.hostRecordCount() == 7);
  CHECK(profiler.hostStartNoFlash(T0 + 10));
  profiler.noteValues(values, {8, 3}, T0 + 11);
  CHECK(profiler.hostRecordCount() == 3);
  CHECK(profiler.hostRecord(1).flags & SCALE_PROFILE_INITIAL);
  ScaleProfileRecord unknown;
  unknown.kind = 0xffff;
  CHECK(formatScaleProfileRow(line, sizeof(line), unknown) != 0);
  CHECK(std::string(line).find("UNKNOWN kind=65535") != std::string::npos);
  auto header = profiler.hostHeader();
  CHECK(header.schemaVersion == SCALE_PROFILE_SCHEMA_VERSION &&
        scaleProfileHeaderValid(header));
  for (uint32_t key = 0; key < static_cast<uint32_t>(S::COUNT); ++key) {
    ScaleProfileRecord state;
    state.kind = static_cast<uint16_t>(ScaleProfileEvent::STATE_CHANGED);
    state.flags = static_cast<uint16_t>(key);
    state.arg1 = state.arg2 = state.sequence = state.connectionGeneration = UINT32_MAX;
    CHECK(formatScaleProfileRow(line, sizeof(line), state) != 0);
  }
}

void testConcurrentStateAndWeightCapture() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  std::thread weights([&]() {
    for (uint32_t i = 1; i <= 100; ++i) noteWeightAt(profiler, T0 + i, 1.0f, i);
  });
  std::thread states([&]() {
    for (uint32_t i = 1; i <= 100; ++i) {
      const ScaleProfileValue value{ScaleProfileSignal::IDLE_REQUEST_ID, i};
      profiler.noteValues({&value, 1}, {2, i}, T0 + i);
    }
  });
  weights.join();
  states.join();
  CHECK(profiler.hostRecordCount() == 201 && profiler.hostLostCount() == 0);
  for (uint32_t i = 0; i < profiler.hostRecordCount(); ++i)
    CHECK(profiler.hostRecord(i).ordinal == i + 1);
  profiler.hostStop(ScaleProfilerStopReason::USER, T0 + 101);
  profiler.hostServiceIdle(T0 + 102);
  CHECK(profiler.hostHeader().recordCount == 202);
}

void runAll() {
  testIdleTarePresentation();
  testStateObservations();
  testConcurrentStateAndWeightCapture();
  testLifecycleCapturesWeightsAndEvents();
  testStopReasonUserIsLastRecord();
  testPreEpochAndWrapAroundTimestamps();
  testRingKeepsNewestAndCountsOverwritten();
  testCaptureHasNoTimeLimit();
  testLongCaptureTimestampsAndExport();
  testIndexOrdersByTimeThenOrdinal();
  testTxtRowShapeAndPrecision();
  testDownloadLeasePinsGeneration();
  testSaveCompletingMidDownloadKeepsRamStream();
  testSaveResultTransitionsAndFailureRetry();
  testStartRetryNeverRestartsActiveSession();
  testHeaderContextProviderRunsAtStart();
  testBudgetMatchesPlan();
  testRestartSupersedesWithoutSaving();
  testLiveDownloadWindowAndOvertake();
  testFlashSaveKeepsNewestRecords();
  testAutoSessionDecisions();
}

}  // namespace

int main() {
  runAll();
  if (failures != 0) {
    std::cerr << failures << " scale profiler check(s) failed\n";
    return 1;
  }
  std::cout << "scale profiler host tests passed\n";
  return 0;
}
