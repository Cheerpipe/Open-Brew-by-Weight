#define OPEN_BREW_BY_WEIGHT_HOST_TEST
#include "machine/OpenBrewByWeightLaMarzoccoSettings.h"
#include "machine/OpenBrewByWeightLaMarzoccoTypes.h"
#include "machine/OpenBrewByWeightLaMarzoccoMachinePower.h"
#include "machine/OpenBrewByWeightLaMarzoccoPowerState.h"
#include "machine/OpenBrewByWeightLaMarzoccoPublicIdentityCache.h"
#include "machine/OpenBrewByWeightLaMarzoccoScalePowerOn.h"
#include "machine/OpenBrewByWeightLaMarzoccoScaleShutdown.h"
#include "machine/OpenBrewByWeightLaMarzoccoTiming.h"

#include <cassert>
#include <cstring>

int main() {
  using namespace openbrewbyweight;
  auto *unterminated = new LaMarzoccoPersistedSettings;
  std::memset(unterminated, 'x', sizeof(*unterminated));
  unterminated->options = LA_MARZOCCO_DEFAULT_OPTIONS;
  unterminated->scaleOptions = LA_MARZOCCO_SCALE_OFF_WITH_MACHINE;
  unterminated->connectionType = static_cast<uint8_t>(LaMarzoccoConnectionType::API);
  unterminated->accountConfigured = true;
  assert(!validLaMarzoccoSettings(*unterminated));
  assert(laMarzoccoAccountKey(*unterminated) != 0);  // bounded, never "none"
  delete unterminated;
  // The rejected-account key follows the username and the password.
  LaMarzoccoPersistedSettings account;
  std::strcpy(account.username, "barista@example.com");
  std::strcpy(account.password, "old-password");
  const uint32_t rejected = laMarzoccoAccountKey(account);
  assert(rejected != 0 && rejected == laMarzoccoAccountKey(account));
  std::strcpy(account.password, "new-password");
  assert(laMarzoccoAccountKey(account) != rejected);
  std::strcpy(account.password, "old-password");
  std::strcpy(account.username, "other@example.com");
  assert(laMarzoccoAccountKey(account) != rejected);
  LaMarzoccoPublicIdentityCache identity;
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
  LaMarzoccoPersistedSettings settings;
  // Defaults: every option on except remote power-on; standby waits 15 s.
  assert(settings.options == LA_MARZOCCO_DEFAULT_OPTIONS);
  assert(settings.scaleOptions == LA_MARZOCCO_KNOWN_SCALE_OPTIONS);
  assert((LA_MARZOCCO_DEFAULT_OPTIONS & LA_MARZOCCO_POWER_ON_WITH_SCALE) ==
         0);
  assert(LA_MARZOCCO_DEFAULT_OPTIONS ==
         ((LA_MARZOCCO_KNOWN_OPTIONS & ~LA_MARZOCCO_POWER_ON_WITH_SCALE &
           ~LA_MARZOCCO_SHUTDOWN_GRACE_MASK) |
          (2U << LA_MARZOCCO_SHUTDOWN_GRACE_SHIFT)));
  assert(laMarzoccoShutdownGraceSeconds(LA_MARZOCCO_DEFAULT_OPTIONS) == 15);
  assert(LA_MARZOCCO_KNOWN_OPTIONS ==
         (LA_MARZOCCO_APPLY_TEMPERATURE | LA_MARZOCCO_OBSERVE_STATE |
          LA_MARZOCCO_RECOGNIZE_WAKE | LA_MARZOCCO_SHUTDOWN_WITH_SCALE |
          LA_MARZOCCO_POWER_ON_WITH_SCALE | LA_MARZOCCO_SHUTDOWN_GRACE_MASK));
  std::strcpy(settings.username, "barista@example.com");
  std::strcpy(settings.password, "correct horse battery staple");
  std::memset(settings.installationPrivateKey, 0x5a,
              sizeof(settings.installationPrivateKey));
  std::strcpy(settings.selectedSerial, "MR123456");
  std::strcpy(settings.selectedName, "Kitchen Micra");
  settings.accountConfigured = true;
  setLaMarzoccoOptions(settings, true, true, true, false, false, 0,
                       false, false, false);
  assert(validLaMarzoccoSettings(settings));
  assert((settings.options & LA_MARZOCCO_SHUTDOWN_WITH_SCALE) == 0);
  assert((settings.options & LA_MARZOCCO_POWER_ON_WITH_SCALE) == 0);
  assert(settings.scaleOptions == 0);
  assert(laMarzoccoShutdownGraceSeconds(settings.options) == 0);
  setLaMarzoccoOptions(settings, true, true, true, true, false, 4,
                       false, false, false);
  assert((settings.options & LA_MARZOCCO_SHUTDOWN_WITH_SCALE) != 0);
  assert((settings.options & LA_MARZOCCO_POWER_ON_WITH_SCALE) == 0);
  assert(laMarzoccoShutdownGraceSeconds(settings.options) == 60);
  assert(laMarzoccoShutdownGraceCode(settings.options) == 4);
  assert(validLaMarzoccoSettings(settings));
  setLaMarzoccoOptions(settings, true, true, true, true, true, 5, true,
                       false, false);
  assert(laMarzoccoShutdownGraceCode(settings.options) == 0);
  assert((settings.options & LA_MARZOCCO_POWER_ON_WITH_SCALE) != 0);
  assert(settings.scaleOptions == LA_MARZOCCO_SCALE_OFF_WITH_MACHINE);
  assert(validLaMarzoccoSettings(settings));
  // Both scan options store as independent positive bits, default off.
  setLaMarzoccoOptions(settings, true, true, true, true, true, 5, false,
                       true, false);
  assert(settings.scaleOptions == LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON);
  assert(validLaMarzoccoSettings(settings));
  setLaMarzoccoOptions(settings, true, true, true, true, true, 5, false,
                       false, true);
  assert(settings.scaleOptions == LA_MARZOCCO_SCALE_SCAN_RELAX_WHEN_OFF);
  assert(validLaMarzoccoSettings(settings));
  setLaMarzoccoOptions(settings, true, true, true, true, true, 5, true,
                       true, true);
  assert(settings.scaleOptions == LA_MARZOCCO_KNOWN_SCALE_OPTIONS);
  assert(validLaMarzoccoSettings(settings));
  // Bits beyond the known scan options stay rejected; account-disconnect
  // keeps the scan preferences like every other option.
  settings.scaleOptions |= static_cast<uint8_t>(1U << 3);
  assert(!validLaMarzoccoSettings(settings));
  settings.scaleOptions = LA_MARZOCCO_KNOWN_SCALE_OPTIONS;
  disconnectLaMarzocco(settings);
  assert(!settings.accountConfigured);
  assert(settings.scaleOptions == LA_MARZOCCO_KNOWN_SCALE_OPTIONS);
  settings.scaleOptions = 0;
  settings.scaleOptions = LA_MARZOCCO_SCALE_OFF_WITH_MACHINE;
  settings.options |= static_cast<uint8_t>(5U)
                      << LA_MARZOCCO_SHUTDOWN_GRACE_SHIFT;
  assert(!validLaMarzoccoSettings(settings));
  settings.options &= ~LA_MARZOCCO_SHUTDOWN_GRACE_MASK;
  setLaMarzoccoOptions(settings, true, true, true, true, false, 2,
                         false, false, false);
  assert(validLaMarzoccoSettings(settings));
  disconnectLaMarzocco(settings);
  assert(validLaMarzoccoSettings(settings));
  assert(settings.options ==
         (LA_MARZOCCO_DEFAULT_OPTIONS | LA_MARZOCCO_SHUTDOWN_WITH_SCALE |
          (2U << LA_MARZOCCO_SHUTDOWN_GRACE_SHIFT)));
  assert(settings.username[0] == '\0');
  assert(settings.password[0] == '\0');
  assert(settings.selectedSerial[0] == '\0');
  wipeLaMarzoccoSettings(settings);
  for (uint8_t byte : settings.installationPrivateKey) assert(byte == 0);
  assert(sizeof(LaMarzoccoRequest) <= 16);
  assert(sizeof(LaMarzoccoPersistedSettings) == 312);
  assert(settings.connectionType == static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET));
  settings.connectionType = static_cast<uint8_t>(LaMarzoccoConnectionType::API);
  disconnectLaMarzocco(settings);
  assert(settings.connectionType == static_cast<uint8_t>(LaMarzoccoConnectionType::API));
  settings.connectionType = 2;
  assert(!validLaMarzoccoSettings(settings));
  LaMarzoccoRequest temperatureRequest;
  temperatureRequest.type = LaMarzoccoRequestType::APPLY_TEMPERATURE;
  temperatureRequest.configGeneration = 7;
  temperatureRequest.presetId = 2;
  temperatureRequest.targetDeciC = 935;
  assert(temperatureRequest.type == LaMarzoccoRequestType::APPLY_TEMPERATURE);
  assert(temperatureRequest.targetDeciC == 935);
  LaMarzoccoRequest standbyRequest;
  standbyRequest.type = LaMarzoccoRequestType::SET_STANDBY;
  assert(standbyRequest.type == LaMarzoccoRequestType::SET_STANDBY);
  assert(std::strcmp(laMarzoccoTemperatureStateName(
                         LaMarzoccoTemperatureState::PENDING),
                     "pending") == 0);
  assert(std::strcmp(laMarzoccoErrorName(LaMarzoccoError::REJECTED),
                     "rejected") == 0);
  assert(std::strcmp(laMarzoccoErrorName(LaMarzoccoError::UNCONFIRMED),
                     "unconfirmed") == 0);
  assert(std::strcmp(laMarzoccoErrorName(LaMarzoccoError::POLICY_REQUIRED),
                     "policy_required") == 0);
  assert(!laMarzoccoTemperatureCycleRetryable(LaMarzoccoError::POLICY_REQUIRED));
  assert(laMarzoccoHttpRetryable(0));
  assert(laMarzoccoHttpRetryable(401));
  assert(laMarzoccoHttpRetryable(408));
  assert(laMarzoccoHttpRetryable(425));
  assert(laMarzoccoHttpRetryable(429));
  assert(laMarzoccoHttpRetryable(500));
  assert(!laMarzoccoHttpRetryable(200));
  assert(!laMarzoccoHttpRetryable(302));
  assert(!laMarzoccoHttpRetryable(400));
  assert(!laMarzoccoHttpRetryable(403));
  assert(!laMarzoccoHttpRetryable(422));
  assert(laMarzoccoTemperatureCycleRetryable(LaMarzoccoError::HTTP_ERROR));
  assert(laMarzoccoTemperatureCycleRetryable(LaMarzoccoError::UNCONFIRMED));
  assert(!laMarzoccoTemperatureCycleRetryable(LaMarzoccoError::INVALID_AUTH));
  assert(!laMarzoccoTemperatureCycleRetryable(LaMarzoccoError::REJECTED));
  assert(laMarzoccoPowerStateForMode(LaMarzoccoObservedMode::STANDBY) ==
         LaMarzoccoPowerState::OFF);
  assert(laMarzoccoPowerStateForMode(LaMarzoccoObservedMode::BREWING) ==
         LaMarzoccoPowerState::ON);
  assert(laMarzoccoPowerStateForMode(LaMarzoccoObservedMode::ECO) ==
         LaMarzoccoPowerState::UNKNOWN);
  assert(laMarzoccoPowerStateForMode(LaMarzoccoObservedMode::UNSUPPORTED) ==
         LaMarzoccoPowerState::UNKNOWN);
  assert(la_marzocco_timing::kMaxAttempts == 4);
  assert(la_marzocco_timing::kRetryDelaysMs[0] == 3000);
  assert(la_marzocco_timing::kRetryDelaysMs[1] == 6000);
  assert(la_marzocco_timing::kRetryDelaysMs[2] == 9000);
  assert(la_marzocco_timing::kWsReconnectDelaysMs[0] == 0);
  assert(la_marzocco_timing::kWsReconnectDelaysMs[1] == 1000);
  assert(la_marzocco_timing::kWsReconnectDelaysMs[2] == 3000);
  assert(la_marzocco_timing::kWsReconnectDelaysMs[3] == 6000);
  assert(la_marzocco_timing::kWsReconnectDelaysMs[4] == 9000);
  assert(la_marzocco_timing::kWsMaxAttempts == 6);
  assert(la_marzocco_timing::kStatePollMs == 30000);
  assert(la_marzocco_timing::kStateFreshnessMs == la_marzocco_timing::kStatePollMs);
  assert(la_marzocco_timing::kOptimisticOverlayMs ==
         2U * la_marzocco_timing::kStatePollMs);
  assert(la_marzocco_timing::kExhaustedCooldownMs ==
         la_marzocco_timing::kOptimisticOverlayMs);
  assert(la_marzocco_timing::kPostWakeObservationDelayMs == 15000);
  assert(la_marzocco_timing::kPostWakeObservationDelayMs <
         la_marzocco_timing::kOptimisticOverlayMs);
  assert(la_marzocco_timing::kStaleRetentionMs == 15U * 60U * 1000U);
  // The resolved assumed state is the only input of every consumer.
  const auto resolve = [](LaMarzoccoStatus s) {
    s.assumedPower = laMarzoccoAssumedPower(s);
    return s;
  };
  LaMarzoccoStatus scanStatus;
  scanStatus.powerState = LaMarzoccoPowerState::UNKNOWN;
  scanStatus.effectiveOn = true;  // UNKNOWN keeps effectiveOn true; never enough.
  assert(laMarzoccoAssumedPower(scanStatus) == LaMarzoccoAssumedPower::UNKNOWN);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_KNOWN_SCALE_OPTIONS) ==
         LaMarzoccoScanOverride::NONE);
  scanStatus.powerState = LaMarzoccoPowerState::ON;
  scanStatus.quality = LaMarzoccoObservationQuality::CURRENT;
  assert(laMarzoccoAssumedPower(scanStatus) == LaMarzoccoAssumedPower::ON);
  assert(laMarzoccoScanOverride(resolve(scanStatus), 0) == LaMarzoccoScanOverride::NONE);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON) ==
         LaMarzoccoScanOverride::AGGRESSIVE);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_SCALE_SCAN_RELAX_WHEN_OFF) ==
         LaMarzoccoScanOverride::NONE);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_KNOWN_SCALE_OPTIONS) ==
         LaMarzoccoScanOverride::AGGRESSIVE);
  scanStatus.powerState = LaMarzoccoPowerState::OFF;
  scanStatus.effectiveOn = false;
  assert(laMarzoccoAssumedPower(scanStatus) == LaMarzoccoAssumedPower::OFF);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON) ==
         LaMarzoccoScanOverride::NONE);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_SCALE_SCAN_RELAX_WHEN_OFF) ==
         LaMarzoccoScanOverride::RELAXED);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_KNOWN_SCALE_OPTIONS) ==
         LaMarzoccoScanOverride::RELAXED);
  // Retained (stale) states qualify through their power state; quality
  // alone never selects a duty.
  scanStatus.powerState = LaMarzoccoPowerState::ON;
  scanStatus.quality = LaMarzoccoObservationQuality::STALE;
  assert(laMarzoccoAssumedPower(scanStatus) == LaMarzoccoAssumedPower::ON_STALE);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON) ==
         LaMarzoccoScanOverride::AGGRESSIVE);
  scanStatus.quality = LaMarzoccoObservationQuality::COMMUNICATION_ERROR;
  assert(laMarzoccoAssumedPower(scanStatus) == LaMarzoccoAssumedPower::ON_STALE);
  // A live optimistic overlay leaves the confirmed value in powerState and
  // reports itself only through its flags: the overlay replaces the
  // confirmed state, in both directions, exclusively.
  scanStatus = LaMarzoccoStatus{};
  scanStatus.powerState = LaMarzoccoPowerState::OFF;  // Confirmed OFF...
  scanStatus.quality = LaMarzoccoObservationQuality::OPTIMISTIC;
  scanStatus.effectiveOn = true;
  scanStatus.optimisticOn = true;  // ...but the machine is waking now.
  assert(laMarzoccoAssumedPower(scanStatus) == LaMarzoccoAssumedPower::ON_STALE);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON) ==
         LaMarzoccoScanOverride::AGGRESSIVE);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_SCALE_SCAN_RELAX_WHEN_OFF) ==
         LaMarzoccoScanOverride::NONE);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_KNOWN_SCALE_OPTIONS) ==
         LaMarzoccoScanOverride::AGGRESSIVE);
  scanStatus = LaMarzoccoStatus{};
  scanStatus.powerState = LaMarzoccoPowerState::ON;  // Confirmed ON...
  scanStatus.quality = LaMarzoccoObservationQuality::OPTIMISTIC;
  scanStatus.effectiveOn = false;
  scanStatus.optimisticOff = true;  // ...but standby was accepted.
  assert(laMarzoccoAssumedPower(scanStatus) == LaMarzoccoAssumedPower::OFF_STALE);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_SCALE_SCAN_RELAX_WHEN_OFF) ==
         LaMarzoccoScanOverride::RELAXED);
  assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON) ==
         LaMarzoccoScanOverride::NONE);
  scanStatus.powerState = LaMarzoccoPowerState::UNKNOWN;
  scanStatus.optimisticOn = scanStatus.optimisticOff = false;
  scanStatus.effectiveOn = true;
  for (LaMarzoccoObservationQuality quality :
       {LaMarzoccoObservationQuality::CURRENT,
        LaMarzoccoObservationQuality::OPTIMISTIC,
        LaMarzoccoObservationQuality::STALE,
        LaMarzoccoObservationQuality::COMMUNICATION_ERROR,
        LaMarzoccoObservationQuality::UNSUPPORTED}) {
    scanStatus.quality = quality;
    assert(laMarzoccoAssumedPower(scanStatus) == LaMarzoccoAssumedPower::UNKNOWN);
    assert(laMarzoccoScanOverride(resolve(scanStatus), LA_MARZOCCO_KNOWN_SCALE_OPTIONS) ==
           LaMarzoccoScanOverride::NONE);
  }
  // Readiness decides from the same value: OFF either way is off, an
  // optimistic ON is neutral, a retained ON keeps its boiler evidence.
  LaMarzoccoStatus lamp;
  lamp.coffeeBoiler = lamp.steamBoiler = LaMarzoccoBoilerState::READY;
  lamp.powerState = LaMarzoccoPowerState::ON;
  lamp.quality = LaMarzoccoObservationQuality::OPTIMISTIC;
  lamp.optimisticOff = true;
  assert(laMarzoccoReadiness(resolve(lamp)) == LaMarzoccoReadiness::OFF);
  lamp.optimisticOff = false;
  lamp.powerState = LaMarzoccoPowerState::OFF;
  lamp.optimisticOn = true;
  assert(laMarzoccoReadiness(resolve(lamp)) == LaMarzoccoReadiness::UNKNOWN);
  lamp.optimisticOn = false;
  lamp.quality = LaMarzoccoObservationQuality::STALE;
  assert(laMarzoccoReadiness(resolve(lamp)) == LaMarzoccoReadiness::OFF);
  lamp.powerState = LaMarzoccoPowerState::ON;
  assert(laMarzoccoReadiness(resolve(lamp)) == LaMarzoccoReadiness::READY);
  lamp.powerState = LaMarzoccoPowerState::UNKNOWN;
  assert(laMarzoccoReadiness(resolve(lamp)) == LaMarzoccoReadiness::UNKNOWN);
  static_assert(sizeof(LaMarzoccoStatus) == 72, "assumedPower must fill the alignment gap");

  assert(!la_marzocco_timing::accessTokenRefreshDue(50U * 60U * 1000U - 1U));
  assert(la_marzocco_timing::accessTokenRefreshDue(50U * 60U * 1000U));
  assert(la_marzocco_timing::kAccessTokenLifetimeMs == 60U * 60U * 1000U);

  la_marzocco_timing::ObservationSchedule schedule;
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

  LaMarzoccoPowerStateTracker power;
  LaMarzoccoStatus authoritative;
  authoritative.sampleAtMs = 100;
  authoritative.powerState = LaMarzoccoPowerState::OFF;
  authoritative.quality = LaMarzoccoObservationQuality::CURRENT;
  authoritative.effectiveOn = false;
  const uint32_t preEdgeGeneration = power.generation();
  assert(power.notePhysicalStart(authoritative, true, true, 200));
  const uint32_t edgeGeneration = power.generation();
  assert(edgeGeneration != preEdgeGeneration);
  LaMarzoccoStatus effective = power.effectiveStatus(authoritative, true, 200);
  assert(effective.powerState == LaMarzoccoPowerState::OFF);
  assert(effective.optimisticOn);
  assert(effective.effectiveOn);
  assert(effective.quality == LaMarzoccoObservationQuality::OPTIMISTIC);
  assert(!power.notePhysicalStart(authoritative, true, true, 201));
  assert(power.generation() == edgeGeneration);
  assert(!power.noteEvidence(preEdgeGeneration, LaMarzoccoPowerState::ON, 202, false));
  assert(power.effectiveStatus(authoritative, true, 202).optimisticOn);
  assert(power.noteEvidence(edgeGeneration, LaMarzoccoPowerState::ON, 203, false));
  assert(!power.effectiveStatus(authoritative, true, 203).optimisticOn);
  assert(power.effectiveStatus(authoritative, true, 203).powerState ==
         LaMarzoccoPowerState::OFF);

  authoritative.sampleAtMs = 1000;
  assert(!power.notePhysicalStart(authoritative, true, false, 1100));
  const uint32_t disabledWakeGeneration = power.generation();
  effective = power.effectiveStatus(authoritative, true, 1100);
  assert(effective.powerState == LaMarzoccoPowerState::OFF);
  assert(!effective.optimisticOn);
  assert(!effective.effectiveOn);

  const uint32_t freshness = la_marzocco_timing::kStateFreshnessMs;
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness - 1);
  assert(effective.powerState == LaMarzoccoPowerState::OFF);
  assert(effective.quality == LaMarzoccoObservationQuality::CURRENT);
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness);
  assert(effective.powerState == LaMarzoccoPowerState::OFF);
  assert(effective.quality == LaMarzoccoObservationQuality::STALE);
  assert(!effective.effectiveOn);
  // A stale sample keeps the last confirmed state: stale OFF still qualifies
  // the wake gesture and starts the optimistic ON overlay.
  assert(power.notePhysicalStart(authoritative, true, true,
                                 1000 + freshness + 1));
  assert(power.generation() != disabledWakeGeneration);
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness + 1);
  assert(effective.powerState == LaMarzoccoPowerState::OFF);
  assert(effective.optimisticOn);
  assert(effective.effectiveOn);
  assert(effective.quality == LaMarzoccoObservationQuality::OPTIMISTIC);
  assert(!power.notePhysicalStart(authoritative, true, true,
                                  1000 + freshness + 2));
  power.reset();

  authoritative.powerState = LaMarzoccoPowerState::ON;
  authoritative.effectiveOn = true;
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness + 3);
  assert(effective.powerState == LaMarzoccoPowerState::ON);
  assert(effective.quality == LaMarzoccoObservationQuality::STALE);
  // Stale ON qualifies the opposite overlay too: an accepted scale shutdown
  // over a stale confirmed ON asserts optimistic OFF.
  assert(power.noteStandbyCommandAccepted(authoritative, true,
                                          1000 + freshness + 4));
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness + 4);
  assert(effective.powerState == LaMarzoccoPowerState::ON);
  assert(effective.optimisticOff);
  assert(!effective.effectiveOn);
  assert(effective.quality == LaMarzoccoObservationQuality::OPTIMISTIC);
  power.reset();
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness + 4);
  assert(effective.powerState == LaMarzoccoPowerState::ON);
  assert(effective.quality == LaMarzoccoObservationQuality::STALE);
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
  assert(effective.powerState == LaMarzoccoPowerState::ON);
  assert(effective.optimisticOff);
  assert(!effective.optimisticOn);
  assert(!effective.effectiveOn);
  assert(effective.quality == LaMarzoccoObservationQuality::OPTIMISTIC);
  assert(!power.noteStandbyCommandAccepted(authoritative, true, 2101));
  assert(power.generation() == shutdownGeneration);
  assert(!power.noteEvidence(preShutdownGeneration, LaMarzoccoPowerState::OFF, 2102, false));
  assert(power.effectiveStatus(authoritative, true, 2102).optimisticOff);
  assert(power.noteEvidence(shutdownGeneration, LaMarzoccoPowerState::OFF, 2103, false));
  effective = power.effectiveStatus(authoritative, true, 2103);
  assert(effective.powerState == LaMarzoccoPowerState::ON);
  assert(!effective.optimisticOff);
  assert(effective.effectiveOn);
  const uint32_t overlayMs = la_marzocco_timing::kOptimisticOverlayMs;
  authoritative.sampleAtMs = 2104;
  assert(power.noteStandbyCommandAccepted(authoritative, true, 2200));
  effective = power.effectiveStatus(authoritative, true, 2200 + overlayMs);
  assert(!effective.optimisticOff);
  assert(effective.effectiveOn);
  // The overlay expires without a successful read: the confirmed ON shows
  // again, already stale because no observation ran during the overlay.
  assert(effective.quality == LaMarzoccoObservationQuality::STALE);
  // Clear the expired overlay so later preconditions see a clean tracker.
  power.reset();

  // Optimistic ON mirrors the standby overlay: a cloud-accepted power-on
  // command over a fresh confirmed OFF asserts effective ON until the first
  // authoritative read accepted by generation replaces it.
  authoritative.sampleAtMs = 3000;
  authoritative.powerState = LaMarzoccoPowerState::OFF;
  authoritative.quality = LaMarzoccoObservationQuality::CURRENT;
  const uint32_t prePowerOnGeneration = power.generation();
  assert(power.notePowerOnCommandAccepted(authoritative, true, 3100));
  const uint32_t powerOnGeneration = power.generation();
  assert(powerOnGeneration != prePowerOnGeneration);
  effective = power.effectiveStatus(authoritative, true, 3101);
  assert(effective.powerState == LaMarzoccoPowerState::OFF);
  assert(effective.optimisticOn);
  assert(effective.effectiveOn);
  assert(effective.quality == LaMarzoccoObservationQuality::OPTIMISTIC);
  // A confirmed ON no longer accepts a second ON-direction overlay.
  authoritative.powerState = LaMarzoccoPowerState::ON;
  assert(!power.notePowerOnCommandAccepted(authoritative, true, 3102));
  assert(power.noteEvidence(powerOnGeneration, LaMarzoccoPowerState::ON, 3102, false));
  power.reset();

  authoritative.sampleAtMs = 0;
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness);
  assert(effective.powerState == LaMarzoccoPowerState::UNKNOWN);
  assert(effective.effectiveOn);
  authoritative.quality = LaMarzoccoObservationQuality::UNCONFIGURED;
  effective = power.effectiveStatus(authoritative, false, 1000 + freshness);
  assert(effective.powerState == LaMarzoccoPowerState::UNKNOWN);
  assert(effective.quality == LaMarzoccoObservationQuality::UNCONFIGURED);
  authoritative.sampleAtMs = 1000;
  authoritative.powerState = LaMarzoccoPowerState::UNKNOWN;
  authoritative.quality = LaMarzoccoObservationQuality::UNSUPPORTED;
  effective = power.effectiveStatus(authoritative, true, 1000 + freshness);
  assert(effective.powerState == LaMarzoccoPowerState::UNKNOWN);
  assert(effective.quality == LaMarzoccoObservationQuality::UNSUPPORTED);
  assert(!power.notePhysicalStart(authoritative, false, true, 1101));

  const uint8_t shutdownOff = LA_MARZOCCO_APPLY_TEMPERATURE |
                              LA_MARZOCCO_OBSERVE_STATE |
                              LA_MARZOCCO_RECOGNIZE_WAKE;
  const uint8_t shutdownOptions15s =
      shutdownOff | LA_MARZOCCO_SHUTDOWN_WITH_SCALE |
      static_cast<uint8_t>(2U << LA_MARZOCCO_SHUTDOWN_GRACE_SHIFT);
  const uint8_t shutdownOptionsImmediate =
      shutdownOff | LA_MARZOCCO_SHUTDOWN_WITH_SCALE;
  LaMarzoccoScaleShutdownTracker::Snapshot scale;
  LaMarzoccoScaleShutdownTracker shutdown;

  // Option off: the explicit power-off is ignored.
  scale.disconnectSequence = 1;
  scale.disconnectReason = LA_MARZOCCO_SCALE_EXPLICIT_DISCONNECT;
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
  scale.disconnectReason = LA_MARZOCCO_SCALE_EXPLICIT_DISCONNECT;
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
      LA_MARZOCCO_DEFAULT_OPTIONS | LA_MARZOCCO_POWER_ON_WITH_SCALE;
  LaMarzoccoScalePowerOnTracker::Snapshot link;
  LaMarzoccoScalePowerOnTracker powerOn;

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
  link.lastDisconnectReason = LA_MARZOCCO_SCALE_EXPLICIT_DISCONNECT;
  link.linkUp = true;
  assert(!powerOn.service(link, LA_MARZOCCO_DEFAULT_OPTIONS, true));
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

  const uint8_t scaleOffOptions = LA_MARZOCCO_SCALE_OFF_WITH_MACHINE;
  LaMarzoccoStatus machine;
  LaMarzoccoMachinePowerTracker machinePower;

  // Stale, optimistic, unknown, and failed observations never arm or fire.
  machine.quality = LaMarzoccoObservationQuality::STALE;
  machine.powerState = LaMarzoccoPowerState::ON;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.quality = LaMarzoccoObservationQuality::OPTIMISTIC;
  machine.powerState = LaMarzoccoPowerState::OFF;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.quality = LaMarzoccoObservationQuality::CURRENT;
  machine.powerState = LaMarzoccoPowerState::UNKNOWN;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  // Confirmed ON arms; a stale OFF in between is ignored; the confirmed OFF
  // fires exactly once. A stale ON without a prior confirmed ON never fires.
  machine.powerState = LaMarzoccoPowerState::ON;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.quality = LaMarzoccoObservationQuality::STALE;
  machine.powerState = LaMarzoccoPowerState::OFF;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.quality = LaMarzoccoObservationQuality::CURRENT;
  machine.powerState = LaMarzoccoPowerState::OFF;
  assert(machinePower.service(machine, scaleOffOptions, true));
  assert(!machinePower.service(machine, scaleOffOptions, true));
  // A second ON -> OFF cycle fires again; with the option off it is consumed.
  machine.powerState = LaMarzoccoPowerState::ON;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.powerState = LaMarzoccoPowerState::OFF;
  assert(machinePower.service(machine, scaleOffOptions, true));
  machine.powerState = LaMarzoccoPowerState::ON;
  assert(!machinePower.service(machine, scaleOffOptions, true));
  machine.powerState = LaMarzoccoPowerState::OFF;
  assert(!machinePower.service(machine, 0, true));
  return 0;
}
