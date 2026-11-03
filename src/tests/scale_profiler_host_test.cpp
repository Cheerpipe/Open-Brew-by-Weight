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

void testCapacityReservesTerminalAndCountsLoss() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  for (uint32_t i = 0; i < SCALE_PROFILE_ORDINARY_CAPACITY + 25; ++i) {
    noteWeightAt(profiler, T0 + i, 1.0f, i + 1);
  }
  CHECK(profiler.hostRecordCount() == SCALE_PROFILE_ORDINARY_CAPACITY);
  CHECK(profiler.hostLostCount() ==
        25 + 1);  // 8191 appends fit after the PROFILE_START marker.
  profiler.hostServiceIdle(T0 + 2000);
  CHECK(profiler.hostState() == ScaleProfilerState::STOPPED);
  CHECK(profiler.hostStopReason() == ScaleProfilerStopReason::BUFFER_FULL);
  CHECK(profiler.hostRecordCount() == SCALE_PROFILE_RECORD_CAPACITY);
  const ScaleProfileRecord terminal =
      profiler.hostRecord(SCALE_PROFILE_RECORD_CAPACITY - 1);
  CHECK(terminal.kind ==
        static_cast<uint16_t>(ScaleProfileEvent::PROFILE_STOP));
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

void testFullCaptureStopsWithoutRejectedAppend() {
  ScaleProfiler profiler;
  CHECK(profiler.hostStartNoFlash(T0));
  for (uint32_t i = 1; i < SCALE_PROFILE_ORDINARY_CAPACITY; ++i)
    noteWeightAt(profiler, T0 + i, 1.0f, i);
  CHECK(profiler.status(T0 + 9000).estimatedRemainingMs == 0);
  profiler.hostServiceIdle(T0 + 9000);
  CHECK(profiler.hostRecordCount() == SCALE_PROFILE_RECORD_CAPACITY);
  CHECK(profiler.hostStopReason() == ScaleProfilerStopReason::BUFFER_FULL);
  CHECK(profiler.hostLostCount() == 0);
  noteWeightAt(profiler, T0 + 9001, 1.0f, 9999);
  profiler.hostServiceIdle(T0 + 9002);
  CHECK(profiler.hostRecordCount() == SCALE_PROFILE_RECORD_CAPACITY);
  CHECK(profiler.hostLostCount() == 0);
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
  CHECK(std::string(line).find("limit=capacity") != std::string::npos);
}

void recordSeconds(ScaleProfiler &profiler, uint32_t &now, uint32_t rate,
                   uint32_t seconds, bool events = false) {
  for (uint32_t second = 0; second < seconds; ++second) {
    for (uint32_t i = 1; i <= rate; ++i) {
      const uint32_t at = now + 1000 * i / rate;
      noteWeightAt(profiler, at, 1.0f, i);
      if (events)
        profiler.noteEvent(
            {.connectionGeneration = 2, .sequence = i, .weightG = 1.0f,
             .kind = static_cast<uint16_t>(ScaleProfileEvent::WEIGHT_ACCEPTED)},
            at);
    }
    now += 1000;
    profiler.hostServiceIdle(now);
  }
}

double estimatedRate(const ScaleProfilerStatus &status) {
  return 1000.0 * (SCALE_PROFILE_ORDINARY_CAPACITY - status.recordCount) /
         status.estimatedRemainingMs;
}

void testEstimateRatesAndLifecycle() {
  for (const uint32_t interval : {50U, 100U, 200U, 500U}) {
    ScaleProfiler profiler;
    uint32_t now = T0;
    CHECK(profiler.hostStartNoFlash(now));
    profiler.hostServiceIdle(now);
    recordSeconds(profiler, now, 1000 / interval, 4);
    CHECK(profiler.status(now).estimatedRemainingMs == UINT32_MAX);
    recordSeconds(profiler, now, 1000 / interval, 26);
    const auto status = profiler.status(now);
    CHECK(std::abs(estimatedRate(status) / (1000.0 / interval) - 1) < 0.05);
    // Reads do not sample the estimator, even with different poll times.
    CHECK(profiler.status(now + 200).estimatedRemainingMs == status.estimatedRemainingMs);
    recordSeconds(profiler, now, 1000 / interval, 15, true);
    CHECK(std::abs(estimatedRate(profiler.status(now)) / (2000.0 / interval) - 1) < 0.1);
    recordSeconds(profiler, now, 0, 5);
    CHECK(profiler.status(now).estimatedRemainingMs == UINT32_MAX);
    CHECK(profiler.recording());
    recordSeconds(profiler, now, 1000 / interval, 6);
    CHECK(profiler.status(now).estimatedRemainingMs != UINT32_MAX);
    profiler.hostStop(ScaleProfilerStopReason::USER, now);
    CHECK(profiler.status(now).estimatedRemainingMs == UINT32_MAX);
    const uint32_t captured = profiler.hostRecordCount();
    profiler.hostServiceIdle(++now);
    profiler.hostPublishFlashResult(true);
    profiler.hostServiceIdle(++now);
    CHECK(profiler.status(now).savedRecordCount == captured);
    CHECK(profiler.status(now).estimatedRemainingMs == UINT32_MAX);
    profiler.hostService(ScaleProfilerRequest::DELETE, ++now);
    CHECK(profiler.status(now).recordCount == 0);
    CHECK(profiler.status(now).elapsedMs == 0);
    CHECK(profiler.hostStartNoFlash(++now));
    CHECK(profiler.status(now).estimatedRemainingMs == UINT32_MAX);
  }
}

void testEstimateJitterAndRateChanges() {
  ScaleProfiler profiler;
  uint32_t now = T0;
  CHECK(profiler.hostStartNoFlash(now));
  // A large startup burst consumes space, but must not bias steady-rate ETA.
  for (uint32_t i = 0; i < 200; ++i) noteWeightAt(profiler, now, 1.0f, i);
  profiler.hostServiceIdle(now);
  recordSeconds(profiler, now, 20, 30);
  double squaredError = 0;
  for (uint32_t second = 0; second < 60; ++second) {
    recordSeconds(profiler, now, second % 2 ? 16 : 24, 1);
    const double error = estimatedRate(profiler.status(now)) - 20;
    squaredError += error * error;
  }
  CHECK(std::sqrt(squaredError / 60) < 4 * 0.3); // At least 70% less RMS jitter.
  for (const uint32_t rate : {40U, 20U, 2U, 20U}) {
    recordSeconds(profiler, now, rate, rate == 40 ? 15 : 25);
    const double eta = profiler.status(now).estimatedRemainingMs;
    const double expected = 1000.0 *
        (SCALE_PROFILE_ORDINARY_CAPACITY - profiler.hostRecordCount()) / rate;
    CHECK(std::abs(eta / expected - 1) < 0.1);
  }
}

void testEstimateIrregularServiceAndClockWrap() {
  ScaleProfiler regular, delayed;
  uint32_t now = 0xfffff000U;
  CHECK(regular.hostStartNoFlash(now));
  CHECK(delayed.hostStartNoFlash(now));
  regular.hostServiceIdle(now);
  delayed.hostServiceIdle(now);
  for (uint32_t tick = 1; tick <= 600; ++tick) {
    now += 100;
    noteWeightAt(regular, now, 1.0f, tick);
    noteWeightAt(delayed, now, 1.0f, tick);
    regular.hostServiceIdle(now);
    if (tick % 17 == 0) delayed.hostServiceIdle(now);
  }
  delayed.hostServiceIdle(now);
  CHECK(std::abs(estimatedRate(regular.status(now)) - 10) < 0.01);
  CHECK(std::abs(estimatedRate(delayed.status(now)) - 10) < 0.01);
  CHECK(regular.status(now).elapsedMs == 60000);
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
  CHECK(profiler.downloadRecordAt(0).kind ==
        static_cast<uint16_t>(ScaleProfileEvent::PROFILE_START));
  CHECK(profiler.downloadRecordAt(1).sequence == 1);
  CHECK(profiler.downloadRecordAt(2).sequence == 2);
  CHECK(profiler.downloadRecordAt(3).kind ==
        static_cast<uint16_t>(ScaleProfileEvent::PROFILE_STOP));
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
  CHECK(profiler.downloadRecordCount() == 3);

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
  CHECK(SCALE_PROFILE_RECORD_CAPACITY * SCALE_PROFILE_RECORD_BYTES ==
        256U * 1024U);
  CHECK(SCALE_PROFILE_PARTITION_BYTES == 0x41000);
  CHECK(SCALE_PROFILE_PARTITION_BYTES - SCALE_PROFILE_PAYLOAD_OFFSET ==
        SCALE_PROFILE_RECORD_CAPACITY * SCALE_PROFILE_RECORD_BYTES);
  CHECK(sizeof(ScaleProfileHeader) <= 4096);
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
  testCapacityReservesTerminalAndCountsLoss();
  testCaptureHasNoTimeLimit();
  testFullCaptureStopsWithoutRejectedAppend();
  testLongCaptureTimestampsAndExport();
  testEstimateRatesAndLifecycle();
  testEstimateJitterAndRateChanges();
  testEstimateIrregularServiceAndClockWrap();
  testIndexOrdersByTimeThenOrdinal();
  testTxtRowShapeAndPrecision();
  testDownloadLeasePinsGeneration();
  testSaveCompletingMidDownloadKeepsRamStream();
  testSaveResultTransitionsAndFailureRetry();
  testStartRetryNeverRestartsActiveSession();
  testHeaderContextProviderRunsAtStart();
  testBudgetMatchesPlan();
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
