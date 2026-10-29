#include "machine/OpenBrewByWeightLineaMicraSettings.h"
#include "machine/OpenBrewByWeightLineaMicraTypes.h"
#include "machine/OpenBrewByWeightMicraMachinePower.h"
#include "machine/OpenBrewByWeightMicraPowerState.h"
#include "machine/OpenBrewByWeightMicraPublicIdentityCache.h"
#include "machine/OpenBrewByWeightMicraScalePowerOn.h"
#include "machine/OpenBrewByWeightMicraScaleShutdown.h"
#include "machine/OpenBrewByWeightMicraTiming.h"

#include <cassert>
#include <cstring>

int main() {
  using namespace openbrewbyweight;
  auto *unterminated = new LineaMicraPersistedSettings;
  std::memset(unterminated, 'x', sizeof(*unterminated));
  unterminated->options = LINEA_MICRA_DEFAULT_OPTIONS;
  unterminated->scaleOptions = LINEA_MICRA_SCALE_OFF_WITH_MACHINE;
  unterminated->connectionType = static_cast<uint8_t>(MicraConnectionType::API);
  unterminated->accountConfigured = true;
  assert(!validLineaMicraSettings(*unterminated));
  delete unterminated;
  MicraPublicIdentityCache identity;
  int derivations = 0;
  const char *activeId = "installation-a";
  const auto derive = [&](char *id, char *publicKey) {
    ++derivations;
    std::strcpy(id, activeId);
    std::strcpy(publicKey, "public-key");
    return true;
  };
  assert(identity.ensure(derive));
  assert(identity.ensure(derive));
  assert(derivations == 1);
  identity.clear();  // Identity replacement and session teardown.
  activeId = "installation-b";
  assert(identity.ensure(derive));
  assert(std::strcmp(identity.id, activeId) == 0);
  assert(derivations == 2);
  identity.clear();  // Disconnect and reconnect.
  assert(identity.ensure(derive));
  assert(derivations == 3);
  identity.clear();
  assert(!identity.ensure([](char *id, char *publicKey) {
    std::strcpy(id, "partial");
    std::strcpy(publicKey, "partial");
    return false;
  }));
  for (char byte : identity.id) assert(byte == 0);
  for (char byte : identity.publicKeyBase64) assert(byte == 0);
  assert(identity.ensure(derive));
  assert(derivations == 4);
  LineaMicraPersistedSettings settings;
  // Defaults: every option on except remote power-on; standby waits 15 s.
  assert(settings.options == LINEA_MICRA_DEFAULT_OPTIONS);
  assert(settings.scaleOptions == LINEA_MICRA_KNOWN_SCALE_OPTIONS);
  assert((LINEA_MICRA_DEFAULT_OPTIONS & LINEA_MICRA_POWER_ON_WITH_SCALE) ==
         0);
  assert(LINEA_MICRA_DEFAULT_OPTIONS ==
         ((LINEA_MICRA_KNOWN_OPTIONS & ~LINEA_MICRA_POWER_ON_WITH_SCALE &
           ~LINEA_MICRA_SHUTDOWN_GRACE_MASK) |
          (2U << LINEA_MICRA_SHUTDOWN_GRACE_SHIFT)));
  assert(lineaMicraShutdownGraceSeconds(LINEA_MICRA_DEFAULT_OPTIONS) == 15);
  assert(LINEA_MICRA_KNOWN_OPTIONS ==
         (LINEA_MICRA_APPLY_TEMPERATURE | LINEA_MICRA_OBSERVE_STATE |
          LINEA_MICRA_RECOGNIZE_WAKE | LINEA_MICRA_SHUTDOWN_WITH_SCALE |
          LINEA_MICRA_POWER_ON_WITH_SCALE | LINEA_MICRA_SHUTDOWN_GRACE_MASK));
  std::strcpy(settings.username, "barista@example.com");
  std::strcpy(settings.password, "correct horse battery staple");
  std::memset(settings.installationPrivateKey, 0x5a,
              sizeof(settings.installationPrivateKey));
  std::strcpy(settings.selectedSerial, "MR123456");
  std::strcpy(settings.selectedName, "Kitchen Micra");
  settings.accountConfigured = true;
  setLineaMicraOptions(settings, true, true, true, false, false, 0,
                       false, false, false);
  assert(validLineaMicraSettings(settings));
  assert((settings.options & LINEA_MICRA_SHUTDOWN_WITH_SCALE) == 0);
  assert((settings.options & LINEA_MICRA_POWER_ON_WITH_SCALE) == 0);
  assert(settings.scaleOptions == 0);
  assert(lineaMicraShutdownGraceSeconds(settings.options) == 0);
  setLineaMicraOptions(settings, true, true, true, true, false, 4,
                       false, false, false);
  assert((settings.options & LINEA_MICRA_SHUTDOWN_WITH_SCALE) != 0);
  assert((settings.options & LINEA_MICRA_POWER_ON_WITH_SCALE) == 0);
  assert(lineaMicraShutdownGraceSeconds(settings.options) == 60);
  assert(lineaMicraShutdownGraceCode(settings.options) == 4);
  assert(validLineaMicraSettings(settings));
  setLineaMicraOptions(settings, true, true, true, true, true, 5, true,
                       false, false);
  assert(lineaMicraShutdownGraceCode(settings.options) == 0);
  assert((settings.options & LINEA_MICRA_POWER_ON_WITH_SCALE) != 0);
  assert(settings.scaleOptions == LINEA_MICRA_SCALE_OFF_WITH_MACHINE);
  assert(validLineaMicraSettings(settings));
  // Both scan options store as independent positive bits, default off.
  setLineaMicraOptions(settings, true, true, true, true, true, 5, false,
                       true, false);
  assert(settings.scaleOptions == LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON);
  assert(validLineaMicraSettings(settings));
  setLineaMicraOptions(settings, true, true, true, true, true, 5, false,
                       false, true);
  assert(settings.scaleOptions == LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF);
  assert(validLineaMicraSettings(settings));
  setLineaMicraOptions(settings, true, true, true, true, true, 5, true,
                       true, true);
  assert(settings.scaleOptions == LINEA_MICRA_KNOWN_SCALE_OPTIONS);
  assert(validLineaMicraSettings(settings));
  // Bits beyond the known scan options stay rejected; account-disconnect
  // keeps the scan preferences like every other option.
  settings.scaleOptions |= static_cast<uint8_t>(1U << 3);
  assert(!validLineaMicraSettings(settings));
  settings.scaleOptions = LINEA_MICRA_KNOWN_SCALE_OPTIONS;
  disconnectLineaMicra(settings);
  assert(!settings.accountConfigured);
  assert(settings.scaleOptions == LINEA_MICRA_KNOWN_SCALE_OPTIONS);
  settings.scaleOptions = 0;
  settings.scaleOptions = LINEA_MICRA_SCALE_OFF_WITH_MACHINE;
  settings.options |= static_cast<uint8_t>(5U)
                      << LINEA_MICRA_SHUTDOWN_GRACE_SHIFT;
  assert(!validLineaMicraSettings(settings));
  settings.options &= ~LINEA_MICRA_SHUTDOWN_GRACE_MASK;
  setLineaMicraOptions(settings, true, true, true, true, false, 2,
                         false, false, false);
  assert(validLineaMicraSettings(settings));
  disconnectLineaMicra(settings);
  assert(validLineaMicraSettings(settings));
  assert(settings.options ==
         (LINEA_MICRA_DEFAULT_OPTIONS | LINEA_MICRA_SHUTDOWN_WITH_SCALE |
          (2U << LINEA_MICRA_SHUTDOWN_GRACE_SHIFT)));
  assert(settings.username[0] == '\0');
  assert(settings.password[0] == '\0');
  assert(settings.selectedSerial[0] == '\0');
  wipeLineaMicraSettings(settings);
  for (uint8_t byte : settings.installationPrivateKey) assert(byte == 0);
  assert(sizeof(LineaMicraRequest) <= 16);
  assert(sizeof(LineaMicraPersistedSettings) == 312);
  assert(settings.connectionType == static_cast<uint8_t>(MicraConnectionType::WEBSOCKET));
  settings.connectionType = static_cast<uint8_t>(MicraConnectionType::API);
  disconnectLineaMicra(settings);
  assert(settings.connectionType == static_cast<uint8_t>(MicraConnectionType::API));
  settings.connectionType = 2;
  assert(!validLineaMicraSettings(settings));
  LineaMicraRequest temperatureRequest;
  temperatureRequest.type = LineaMicraRequestType::APPLY_TEMPERATURE;
  temperatureRequest.configGeneration = 7;
  temperatureRequest.presetId = 2;
  temperatureRequest.targetDeciC = 935;
  assert(temperatureRequest.type == LineaMicraRequestType::APPLY_TEMPERATURE);
  assert(temperatureRequest.targetDeciC == 935);
  LineaMicraRequest standbyRequest;
  standbyRequest.type = LineaMicraRequestType::SET_STANDBY;
  assert(standbyRequest.type == LineaMicraRequestType::SET_STANDBY);
  assert(std::strcmp(lineaMicraTemperatureStateName(
                         LineaMicraTemperatureState::PENDING),
                     "pending") == 0);
  assert(std::strcmp(lineaMicraErrorName(LineaMicraError::REJECTED),
                     "rejected") == 0);
  assert(std::strcmp(lineaMicraErrorName(LineaMicraError::UNCONFIRMED),
                     "unconfirmed") == 0);
  assert(lineaMicraHttpRetryable(0));
  assert(lineaMicraHttpRetryable(401));
  assert(lineaMicraHttpRetryable(408));
  assert(lineaMicraHttpRetryable(425));
  assert(lineaMicraHttpRetryable(429));
  assert(lineaMicraHttpRetryable(500));
  assert(!lineaMicraHttpRetryable(200));
  assert(!lineaMicraHttpRetryable(302));
  assert(!lineaMicraHttpRetryable(400));
  assert(!lineaMicraHttpRetryable(403));
  assert(!lineaMicraHttpRetryable(422));
  assert(lineaMicraTemperatureCycleRetryable(LineaMicraError::HTTP_ERROR));
  assert(lineaMicraTemperatureCycleRetryable(LineaMicraError::UNCONFIRMED));
  assert(!lineaMicraTemperatureCycleRetryable(LineaMicraError::INVALID_AUTH));
  assert(!lineaMicraTemperatureCycleRetryable(LineaMicraError::REJECTED));
  assert(lineaMicraPowerStateForMode(LineaMicraObservedMode::STANDBY) ==
         LineaMicraPowerState::OFF);
  assert(lineaMicraPowerStateForMode(LineaMicraObservedMode::BREWING) ==
         LineaMicraPowerState::ON);
  assert(lineaMicraPowerStateForMode(LineaMicraObservedMode::ECO) ==
         LineaMicraPowerState::UNKNOWN);
  assert(lineaMicraPowerStateForMode(LineaMicraObservedMode::UNSUPPORTED) ==
         LineaMicraPowerState::UNKNOWN);
  assert(micra_timing::kMaxAttempts == 4);
  assert(micra_timing::kRetryDelaysMs[0] == 3000);
  assert(micra_timing::kRetryDelaysMs[1] == 6000);
  assert(micra_timing::kRetryDelaysMs[2] == 9000);
  assert(micra_timing::kWsReconnectDelaysMs[0] == 0);
  assert(micra_timing::kWsReconnectDelaysMs[1] == 1000);
  assert(micra_timing::kWsReconnectDelaysMs[2] == 3000);
  assert(micra_timing::kWsReconnectDelaysMs[3] == 6000);
  assert(micra_timing::kWsReconnectDelaysMs[4] == 9000);
  assert(micra_timing::kWsMaxAttempts == 6);
  assert(micra_timing::kStatePollMs == 30000);
  assert(micra_timing::kStateFreshnessMs == micra_timing::kStatePollMs);
  assert(micra_timing::kOptimisticOverlayMs ==
         2U * micra_timing::kStatePollMs);
  assert(micra_timing::kExhaustedCooldownMs ==
         micra_timing::kOptimisticOverlayMs);
  assert(micra_timing::kPostWakeObservationDelayMs == 15000);
  assert(micra_timing::kPostWakeObservationDelayMs <
         micra_timing::kOptimisticOverlayMs);
  LineaMicraStatus scanStatus;
  scanStatus.powerState = LineaMicraPowerState::UNKNOWN;
  scanStatus.effectiveOn = true;  // UNKNOWN keeps effectiveOn true; never enough.
  assert(micraScanOverride(scanStatus, LINEA_MICRA_KNOWN_SCALE_OPTIONS) ==
         MicraScanOverride::NONE);
  scanStatus.powerState = LineaMicraPowerState::ON;
  assert(micraScanOverride(scanStatus, 0) == MicraScanOverride::NONE);
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON) ==
         MicraScanOverride::AGGRESSIVE);
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF) ==
         MicraScanOverride::NONE);
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_KNOWN_SCALE_OPTIONS) ==
         MicraScanOverride::AGGRESSIVE);
  scanStatus.powerState = LineaMicraPowerState::OFF;
  scanStatus.effectiveOn = false;
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON) ==
         MicraScanOverride::NONE);
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF) ==
         MicraScanOverride::RELAXED);
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_KNOWN_SCALE_OPTIONS) ==
         MicraScanOverride::RELAXED);
  // Retained (stale) states qualify through their power state; quality
  // alone never selects a duty.
  scanStatus.powerState = LineaMicraPowerState::ON;
  scanStatus.quality = LineaMicraObservationQuality::STALE;
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON) ==
         MicraScanOverride::AGGRESSIVE);
  // A live optimistic overlay leaves the confirmed value in powerState and
  // reports itself only through its flags: the overlay replaces the
  // confirmed state, in both directions, exclusively.
  scanStatus = LineaMicraStatus{};
  scanStatus.powerState = LineaMicraPowerState::OFF;  // Confirmed OFF...
  scanStatus.quality = LineaMicraObservationQuality::OPTIMISTIC;
  scanStatus.effectiveOn = true;
  scanStatus.optimisticOn = true;  // ...but the machine is waking now.
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON) ==
         MicraScanOverride::AGGRESSIVE);
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF) ==
         MicraScanOverride::NONE);
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_KNOWN_SCALE_OPTIONS) ==
         MicraScanOverride::AGGRESSIVE);
  scanStatus = LineaMicraStatus{};
  scanStatus.powerState = LineaMicraPowerState::ON;  // Confirmed ON...
  scanStatus.quality = LineaMicraObservationQuality::OPTIMISTIC;
  scanStatus.effectiveOn = false;
  scanStatus.optimisticOff = true;  // ...but standby was accepted.
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF) ==
         MicraScanOverride::RELAXED);
  assert(micraScanOverride(
             scanStatus, LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON) ==
         MicraScanOverride::NONE);
  scanStatus.powerState = LineaMicraPowerState::UNKNOWN;
  scanStatus.optimisticOn = scanStatus.optimisticOff = false;
  scanStatus.effectiveOn = true;
  for (LineaMicraObservationQuality quality :
       {LineaMicraObservationQuality::CURRENT,
        LineaMicraObservationQuality::OPTIMISTIC,
        LineaMicraObservationQuality::STALE,
        LineaMicraObservationQuality::COMMUNICATION_ERROR,
        LineaMicraObservationQuality::UNSUPPORTED}) {
    scanStatus.quality = quality;
    assert(micraScanOverride(scanStatus, LINEA_MICRA_KNOWN_SCALE_OPTIONS) ==
           MicraScanOverride::NONE);
  }

  assert(!micra_timing::accessTokenRefreshDue(50U * 60U * 1000U - 1U));
  assert(micra_timing::accessTokenRefreshDue(50U * 60U * 1000U));
  assert(micra_timing::kAccessTokenLifetimeMs == 60U * 60U * 1000U);

  micra_timing::ObservationSchedule schedule;
  schedule.dueNow(100);
  assert(schedule.automaticDue(100));
  schedule.armPostEvent(200);
  assert(!schedule.observationAllowed(15199));
  schedule.scheduleNext(300);  // A pre-edge completion preserves the deadline.
  assert(schedule.observationAllowed(15200));
  assert(schedule.automaticDue(15200));
  schedule.observationStarted(15200);
  schedule.scheduleNext(15200);
  assert(!schedule.automaticDue(45199));
  assert(schedule.automaticDue(45200));

  LineaMicraPowerStateTracker power;
  LineaMicraStatus authoritative;
  authoritative.sampleAtMs = 100;
  authoritative.powerState = LineaMicraPowerState::OFF;
  authoritative.quality = LineaMicraObservationQuality::CURRENT;
  authoritative.effectiveOn = false;
  const uint32_t preEdgeGeneration = power.generation();
  assert(power.notePhysicalStart(authoritative, true, true, 200));
  const uint32_t edgeGeneration = power.generation();
  assert(edgeGeneration != preEdgeGeneration);
  LineaMicraStatus effective = power.effectiveStatus(authoritative, true, 200);
  assert(effective.powerState == LineaMicraPowerState::OFF);
  assert(effective.optimisticOn);
  assert(effective.effectiveOn);
  assert(effective.quality == LineaMicraObservationQuality::OPTIMISTIC);
  assert(!power.notePhysicalStart(authoritative, true, true, 201));
  assert(power.generation() == edgeGeneration);
  assert(!power.acceptAuthoritative(preEdgeGeneration));
  assert(power.effectiveStatus(authoritative, true, 202).optimisticOn);
  assert(power.acceptAuthoritative(edgeGeneration));
  assert(power.effectiveStatus(authoritative, true, 203).powerState ==
         LineaMicraPowerState::OFF);

  authoritative.sampleAtMs = 1000;
  assert(!power.notePhysicalStart(authoritative, true, false, 1100));
  const uint32_t disabledWakeGeneration = power.generation();
  effective = power.effectiveStatus(authoritative, true, 1100);
  assert(effective.powerState == LineaMicraPowerState::OFF);
  assert(!effective.optimisticOn);
  assert(!effective.effectiveOn);

  const uint32_t freshness = micra_timing::kStateFreshnessMs;
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness - 1);
  assert(effective.powerState == LineaMicraPowerState::OFF);
  assert(effective.quality == LineaMicraObservationQuality::CURRENT);
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness);
  assert(effective.powerState == LineaMicraPowerState::OFF);
  assert(effective.quality == LineaMicraObservationQuality::STALE);
  assert(!effective.effectiveOn);
  // A stale sample keeps the last confirmed state: stale OFF still qualifies
  // the wake gesture and starts the optimistic ON overlay.
  assert(power.notePhysicalStart(authoritative, true, true,
                                 1000 + freshness + 1));
  assert(power.generation() != disabledWakeGeneration);
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness + 1);
  assert(effective.powerState == LineaMicraPowerState::OFF);
  assert(effective.optimisticOn);
  assert(effective.effectiveOn);
  assert(effective.quality == LineaMicraObservationQuality::OPTIMISTIC);
  assert(!power.notePhysicalStart(authoritative, true, true,
                                  1000 + freshness + 2));
  power.reset();

  authoritative.powerState = LineaMicraPowerState::ON;
  authoritative.effectiveOn = true;
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness + 3);
  assert(effective.powerState == LineaMicraPowerState::ON);
  assert(effective.quality == LineaMicraObservationQuality::STALE);
  // Stale ON qualifies the opposite overlay too: an accepted scale shutdown
  // over a stale confirmed ON asserts optimistic OFF.
  assert(power.noteStandbyCommandAccepted(authoritative, true,
                                          1000 + freshness + 4));
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness + 4);
  assert(effective.powerState == LineaMicraPowerState::ON);
  assert(effective.optimisticOff);
  assert(!effective.effectiveOn);
  assert(effective.quality == LineaMicraObservationQuality::OPTIMISTIC);
  power.reset();
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness + 4);
  assert(effective.powerState == LineaMicraPowerState::ON);
  assert(effective.quality == LineaMicraObservationQuality::STALE);
  assert(effective.effectiveOn);

  // Optimistic OFF mirrors the wake overlay: an accepted scale-shutdown
  // command over a fresh confirmed ON asserts effective OFF until the first
  // authoritative read accepted by generation replaces it.
  authoritative.sampleAtMs = 2000;
  const uint32_t preShutdownGeneration = power.generation();
  assert(power.noteStandbyCommandAccepted(authoritative, true, 2100));
  const uint32_t shutdownGeneration = power.generation();
  assert(shutdownGeneration != preShutdownGeneration);
  effective = power.effectiveStatus(authoritative, true, 2100);
  assert(effective.powerState == LineaMicraPowerState::ON);
  assert(effective.optimisticOff);
  assert(!effective.optimisticOn);
  assert(!effective.effectiveOn);
  assert(effective.quality == LineaMicraObservationQuality::OPTIMISTIC);
  assert(!power.noteStandbyCommandAccepted(authoritative, true, 2101));
  assert(power.generation() == shutdownGeneration);
  assert(!power.acceptAuthoritative(preShutdownGeneration));
  assert(power.effectiveStatus(authoritative, true, 2102).optimisticOff);
  assert(power.acceptAuthoritative(shutdownGeneration));
  effective = power.effectiveStatus(authoritative, true, 2103);
  assert(effective.powerState == LineaMicraPowerState::ON);
  assert(!effective.optimisticOff);
  assert(effective.effectiveOn);
  const uint32_t overlayMs = micra_timing::kOptimisticOverlayMs;
  authoritative.sampleAtMs = 2104;
  assert(power.noteStandbyCommandAccepted(authoritative, true, 2200));
  effective = power.effectiveStatus(authoritative, true, 2200 + overlayMs);
  assert(!effective.optimisticOff);
  assert(effective.effectiveOn);
  // The overlay expires without a successful read: the confirmed ON shows
  // again, already stale because no observation ran during the overlay.
  assert(effective.quality == LineaMicraObservationQuality::STALE);
  // Clear the expired overlay so later preconditions see a clean tracker.
  power.reset();

  // Optimistic ON mirrors the standby overlay: a cloud-accepted power-on
  // command over a fresh confirmed OFF asserts effective ON until the first
  // authoritative read accepted by generation replaces it.
  authoritative.sampleAtMs = 3000;
  authoritative.powerState = LineaMicraPowerState::OFF;
  authoritative.quality = LineaMicraObservationQuality::CURRENT;
  const uint32_t prePowerOnGeneration = power.generation();
  assert(power.notePowerOnCommandAccepted(authoritative, true, 3100));
  const uint32_t powerOnGeneration = power.generation();
  assert(powerOnGeneration != prePowerOnGeneration);
  effective = power.effectiveStatus(authoritative, true, 3101);
  assert(effective.powerState == LineaMicraPowerState::OFF);
  assert(effective.optimisticOn);
  assert(effective.effectiveOn);
  assert(effective.quality == LineaMicraObservationQuality::OPTIMISTIC);
  // A confirmed ON no longer accepts a second ON-direction overlay.
  authoritative.powerState = LineaMicraPowerState::ON;
  assert(!power.notePowerOnCommandAccepted(authoritative, true, 3102));
  assert(power.acceptAuthoritative(powerOnGeneration));
  power.reset();

  authoritative.sampleAtMs = 0;
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness);
  assert(effective.powerState == LineaMicraPowerState::UNKNOWN);
  assert(effective.effectiveOn);
  authoritative.quality = LineaMicraObservationQuality::UNCONFIGURED;
  effective = power.effectiveStatus(authoritative, false, 1000 + freshness);
  assert(effective.powerState == LineaMicraPowerState::UNKNOWN);
  assert(effective.quality == LineaMicraObservationQuality::UNCONFIGURED);
  authoritative.sampleAtMs = 1000;
  authoritative.powerState = LineaMicraPowerState::UNKNOWN;
  authoritative.quality = LineaMicraObservationQuality::UNSUPPORTED;
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness);
  assert(effective.powerState == LineaMicraPowerState::UNKNOWN);
  assert(effective.quality == LineaMicraObservationQuality::UNSUPPORTED);
  assert(!power.notePhysicalStart(authoritative, false, true, 1101));

  const uint8_t shutdownOff = LINEA_MICRA_APPLY_TEMPERATURE |
                              LINEA_MICRA_OBSERVE_STATE |
                              LINEA_MICRA_RECOGNIZE_WAKE;
  const uint8_t shutdownOptions15s =
      shutdownOff | LINEA_MICRA_SHUTDOWN_WITH_SCALE |
      static_cast<uint8_t>(2U << LINEA_MICRA_SHUTDOWN_GRACE_SHIFT);
  const uint8_t shutdownOptionsImmediate =
      shutdownOff | LINEA_MICRA_SHUTDOWN_WITH_SCALE;
  MicraScaleShutdownTracker::Snapshot scale;
  MicraScaleShutdownTracker shutdown;

  // Option off: the explicit power-off is ignored.
  scale.disconnectSequence = 1;
  scale.disconnectReason = LINEA_MICRA_SCALE_EXPLICIT_DISCONNECT;
  assert(!shutdown.service(1000, scale, shutdownOff, true));
  assert(!shutdown.service(60000, scale, shutdownOff, true));
  // No cloud account: ignored even with the option on.
  assert(!shutdown.service(1000, scale, shutdownOptions15s, false));
  assert(!shutdown.service(60000, scale, shutdownOptions15s, false));
  // Radio silence (supervision timeout) never triggers the shutdown.
  scale.disconnectSequence = 2;
  scale.disconnectReason = 14;
  assert(!shutdown.service(2000, scale, shutdownOptions15s, true));
  assert(!shutdown.service(60000, scale, shutdownOptions15s, true));
  // Grace 15 s: armed, held before the deadline, fires once after it.
  scale.disconnectSequence = 3;
  scale.disconnectReason = LINEA_MICRA_SCALE_EXPLICIT_DISCONNECT;
  assert(shutdown.pending() == false);
  assert(!shutdown.service(10000, scale, shutdownOptions15s, true));
  assert(shutdown.pending());
  assert(!shutdown.service(24999, scale, shutdownOptions15s, true));
  assert(shutdown.pending());
  assert(shutdown.service(25000, scale, shutdownOptions15s, true));
  assert(!shutdown.pending());
  assert(!shutdown.service(25001, scale, shutdownOptions15s, true));
  // Grace OFF: fires immediately on the observed event.
  scale.disconnectSequence = 4;
  assert(shutdown.service(30000, scale, shutdownOptionsImmediate, true));
  assert(!shutdown.service(30001, scale, shutdownOptionsImmediate, true));
  // Scale back online inside the window cancels the shutdown.
  scale.disconnectSequence = 5;
  assert(!shutdown.service(40000, scale, shutdownOptions15s, true));
  scale.linkUp = true;
  assert(!shutdown.service(45000, scale, shutdownOptions15s, true));
  assert(!shutdown.pending());
  scale.linkUp = false;
  assert(!shutdown.service(60000, scale, shutdownOptions15s, true));
  assert(!shutdown.pending());
  // Relay closed at the event: ignored completely, never deferred.
  scale.disconnectSequence = 6;
  scale.relayClosed = true;
  assert(!shutdown.service(70000, scale, shutdownOptions15s, true));
  scale.relayClosed = false;
  assert(!shutdown.service(90000, scale, shutdownOptions15s, true));
  assert(!shutdown.pending());
  // Relay closing during the grace window cancels at fire time.
  scale.disconnectSequence = 7;
  assert(!shutdown.service(100000, scale, shutdownOptions15s, true));
  scale.relayClosed = true;
  assert(!shutdown.service(115000, scale, shutdownOptions15s, true));
  assert(!shutdown.pending());
  scale.relayClosed = false;
  // Option disabled mid-grace cancels.
  scale.disconnectSequence = 8;
  assert(!shutdown.service(120000, scale, shutdownOptions15s, true));
  assert(!shutdown.service(121000, scale, shutdownOff, true));
  assert(!shutdown.pending());
  assert(!shutdown.service(140000, scale, shutdownOptions15s, true));

  const uint8_t powerOnOptions =
      LINEA_MICRA_DEFAULT_OPTIONS | LINEA_MICRA_POWER_ON_WITH_SCALE;
  MicraScalePowerOnTracker::Snapshot link;
  MicraScalePowerOnTracker powerOn;

  // First link-up after boot never wakes the machine: no explicit power-off
  // was observed.
  link.linkUp = true;
  link.lastDisconnectReason = 0;
  assert(!powerOn.service(link, powerOnOptions, true));
  link.linkUp = false;
  (void)powerOn.service(link, powerOnOptions, true);
  // Reconnect after radio silence (supervision timeout) never wakes it.
  link.lastDisconnectReason = 14;
  link.linkUp = true;
  assert(!powerOn.service(link, powerOnOptions, true));
  link.linkUp = false;
  (void)powerOn.service(link, powerOnOptions, true);
  // Option off: the trigger is consumed.
  link.lastDisconnectReason = LINEA_MICRA_SCALE_EXPLICIT_DISCONNECT;
  link.linkUp = true;
  assert(!powerOn.service(link, LINEA_MICRA_DEFAULT_OPTIONS, true));
  link.linkUp = false;
  (void)powerOn.service(link, powerOnOptions, true);
  // No account: consumed as well.
  link.linkUp = true;
  assert(!powerOn.service(link, powerOnOptions, false));
  link.linkUp = false;
  (void)powerOn.service(link, powerOnOptions, true);
  // Relay closed at the edge: consumed, never deferred.
  link.relayClosed = true;
  link.linkUp = true;
  assert(!powerOn.service(link, powerOnOptions, true));
  link.linkUp = false;
  link.relayClosed = false;
  (void)powerOn.service(link, powerOnOptions, true);
  // The scale returning from its own power-off fires exactly once.
  link.linkUp = true;
  assert(powerOn.service(link, powerOnOptions, true));
  assert(!powerOn.service(link, powerOnOptions, true));
  // Link held: no new edge, no new trigger.
  assert(!powerOn.service(link, powerOnOptions, true));
  // A later non-explicit reconnect still does not fire.
  link.linkUp = false;
  (void)powerOn.service(link, powerOnOptions, true);
  link.lastDisconnectReason = 14;
  link.linkUp = true;
  assert(!powerOn.service(link, powerOnOptions, true));

  const uint8_t scaleOffOptions = LINEA_MICRA_SCALE_OFF_WITH_MACHINE;
  LineaMicraStatus machine;
  MicraMachinePowerTracker machinePower;

  // Stale, optimistic, unknown, and failed observations never arm or fire.
  machine.quality = LineaMicraObservationQuality::STALE;
  machine.powerState = LineaMicraPowerState::ON;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.quality = LineaMicraObservationQuality::OPTIMISTIC;
  machine.powerState = LineaMicraPowerState::OFF;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.quality = LineaMicraObservationQuality::CURRENT;
  machine.powerState = LineaMicraPowerState::UNKNOWN;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  // Confirmed ON arms; a stale OFF in between is ignored; the confirmed OFF
  // fires exactly once. A stale ON without a prior confirmed ON never fires.
  machine.powerState = LineaMicraPowerState::ON;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.quality = LineaMicraObservationQuality::STALE;
  machine.powerState = LineaMicraPowerState::OFF;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.quality = LineaMicraObservationQuality::CURRENT;
  machine.powerState = LineaMicraPowerState::OFF;
  assert(machinePower.service(machine, scaleOffOptions, true));
  assert(!machinePower.service(machine, scaleOffOptions, true));
  // A second ON -> OFF cycle fires again; with the option off it is consumed.
  machine.powerState = LineaMicraPowerState::ON;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.powerState = LineaMicraPowerState::OFF;
  assert(machinePower.service(machine, scaleOffOptions, true));
  machine.powerState = LineaMicraPowerState::ON;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.powerState = LineaMicraPowerState::OFF;
  assert(!machinePower.service(machine, 0, true));
  return 0;
}
