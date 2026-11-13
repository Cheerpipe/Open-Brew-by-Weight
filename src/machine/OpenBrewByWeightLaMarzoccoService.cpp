#include "OpenBrewByWeightLaMarzoccoService.h"
#include "OpenBrewByWeightAuditLog.h"
#include "OpenBrewByWeightBuildProfile.h"
#include "OpenBrewByWeightFirmwareMode.h"
#include "OpenBrewByWeightOutboundAdmission.h"

#include "OpenBrewByWeightJsonArena.h"
#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightLaMarzoccoTiming.h"
#include "OpenBrewByWeightTaskProfiler.h"
#include "OpenBrewByWeightLaMarzoccoPublicIdentityCache.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightPersistedSettings.h"

#include <Arduino.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_websocket_client.h>
#include <esp_timer.h>
#include "OpenBrewByWeightLaMarzoccoStomp.h"
#include <mbedtls/base64.h>
#include <psa/crypto.h>
#include <sys/time.h>

#include <cstdio>
#include <cmath>
#include <cstring>
#include <new>
#include <algorithm>

void serialTraceCategoryf(openbrewbyweight::LogLevel level,
                          openbrewbyweight::DebugCategory category,
                          const char *fmt, ...);
void addDebugEvent(openbrewbyweight::DebugCategory category,
                   openbrewbyweight::DebugCode code, int32_t argument1,
                   int32_t argument2);
void addDebugEventDetail(openbrewbyweight::DebugCategory category,
                         openbrewbyweight::DebugCode code, int32_t argument1,
                         int32_t argument2, const char *detail);
void shotTraceNoteLaMarzoccoRx(uint32_t bytes);
void shotTraceNoteLaMarzoccoSubscribed(bool subscribed);

namespace openbrewbyweight {
namespace {

constexpr char kApiRoot[] = "https://lion.lamarzocco.io/api/customer-app";
constexpr char kRegisterUrl[] =
    "https://lion.lamarzocco.io/api/customer-app/auth/init";
constexpr char kSignInUrl[] =
    "https://lion.lamarzocco.io/api/customer-app/auth/signin";
constexpr char kRefreshUrl[] =
    "https://lion.lamarzocco.io/api/customer-app/auth/refreshtoken";
constexpr char kThingsUrl[] =
    "https://lion.lamarzocco.io/api/customer-app/things";
constexpr char kPolicyUrl[] =
    "https://lion.lamarzocco.io/api/customer-app/users/policy";
constexpr size_t kResponseCapacity = 16 * 1024;
constexpr size_t kResponseMaxValues = 1024;
constexpr size_t kTokenCapacity = 2048;
constexpr size_t kBodyCapacity = 4608;
constexpr size_t kWorkerStackBytes = 8192;

constexpr uint8_t kP256SpkiPrefix[] = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48,
    0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
    0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00};

bool secureCopy(char *destination, size_t capacity, const char *source) {
  if (destination == nullptr || source == nullptr || capacity == 0) return false;
  const size_t length = strnlen(source, capacity);
  if (length >= capacity) return false;
  memset(destination, 0, capacity);
  memcpy(destination, source, length);
  return true;
}

void secureWipe(void *memory, size_t size) {
  volatile uint8_t *bytes = static_cast<volatile uint8_t *>(memory);
  while (size-- > 0) *bytes++ = 0;
}

bool base64Encode(const uint8_t *input, size_t inputLength, char *output,
                  size_t capacity) {
  size_t written = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(output),
                            capacity, &written, input, inputLength) != 0 ||
      written >= capacity) {
    return false;
  }
  output[written] = '\0';
  return true;
}

bool sha256(const void *input, size_t length, uint8_t output[32]) {
  size_t written = 0;
  return psa_hash_compute(PSA_ALG_SHA_256,
                          static_cast<const uint8_t *>(input), length, output,
                          32, &written) == PSA_SUCCESS && written == 32;
}

void formatUuid(const uint8_t source[16], char output[37]) {
  uint8_t bytes[16];
  memcpy(bytes, source, sizeof(bytes));
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fU) | 0x80U);
  snprintf(output, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
           bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
           bytes[12], bytes[13], bytes[14], bytes[15]);
  secureWipe(bytes, sizeof(bytes));
}

bool installationId(const LaMarzoccoPersistedSettings &settings,
                    char output[37]) {
  uint8_t digest[32];
  const bool ok = sha256(settings.installationPrivateKey,
                         sizeof(settings.installationPrivateKey), digest);
  if (ok) formatUuid(digest, output);
  secureWipe(digest, sizeof(digest));
  return ok;
}

void setKeyAttributes(psa_key_attributes_t &attributes) {
  psa_set_key_type(&attributes,
                   PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attributes, 256);
  psa_set_key_usage_flags(&attributes,
                          PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
  psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
}

bool importPrivateKey(const uint8_t privateKey[32],
                      mbedtls_svc_key_id_t &key) {
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  setKeyAttributes(attributes);
  const psa_status_t status =
      psa_import_key(&attributes, privateKey, 32, &key);
  psa_reset_key_attributes(&attributes);
  return status == PSA_SUCCESS;
}

bool publicKeyDer(const LaMarzoccoPersistedSettings &settings,
                  uint8_t output[91]) {
  mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
  if (!importPrivateKey(settings.installationPrivateKey, key)) return false;
  uint8_t publicKey[65];
  size_t publicLength = 0;
  const psa_status_t status =
      psa_export_public_key(key, publicKey, sizeof(publicKey), &publicLength);
  (void)psa_destroy_key(key);
  if (status != PSA_SUCCESS || publicLength != sizeof(publicKey) ||
      publicKey[0] != 0x04) {
    secureWipe(publicKey, sizeof(publicKey));
    return false;
  }
  memcpy(output, kP256SpkiPrefix, sizeof(kP256SpkiPrefix));
  memcpy(output + sizeof(kP256SpkiPrefix), publicKey, sizeof(publicKey));
  secureWipe(publicKey, sizeof(publicKey));
  return true;
}

bool deriveInstallationSecret(const char *id, const char *publicB64,
                              uint8_t secret[32]) {
  uint8_t idHash[32];
  char idHashB64[48];
  char material[256];
  bool ok = sha256(id, strlen(id), idHash) &&
            base64Encode(idHash, sizeof(idHash), idHashB64,
                         sizeof(idHashB64));
  if (ok) {
    const int length = snprintf(material, sizeof(material), "%s.%s.%s", id,
                                publicB64, idHashB64);
    ok = length > 0 && static_cast<size_t>(length) < sizeof(material) &&
         sha256(material, static_cast<size_t>(length), secret);
  }
  secureWipe(idHash, sizeof(idHash));
  secureWipe(idHashB64, sizeof(idHashB64));
  secureWipe(material, sizeof(material));
  return ok;
}

bool requestProof(const char *input, const uint8_t secret[32],
                  char output[48]) {
  uint8_t work[32];
  memcpy(work, secret, sizeof(work));
  for (const uint8_t *cursor = reinterpret_cast<const uint8_t *>(input);
       *cursor != 0; ++cursor) {
    const uint8_t index = *cursor % 32U;
    const uint8_t shift = work[(index + 1U) % 32U] & 7U;
    const uint8_t value = static_cast<uint8_t>(*cursor ^ work[index]);
    work[index] = shift == 0
                      ? value
                      : static_cast<uint8_t>((value << shift) |
                                             (value >> (8U - shift)));
  }
  uint8_t digest[32];
  const bool ok = sha256(work, sizeof(work), digest) &&
                  base64Encode(digest, sizeof(digest), output, 48);
  secureWipe(work, sizeof(work));
  secureWipe(digest, sizeof(digest));
  return ok;
}

size_t encodeAsn1Integer(const uint8_t *raw, size_t rawLength,
                         uint8_t *output) {
  size_t start = 0;
  while (start + 1U < rawLength && raw[start] == 0) ++start;
  const bool pad = (raw[start] & 0x80U) != 0;
  const size_t valueLength = rawLength - start;
  output[0] = 0x02;
  output[1] = static_cast<uint8_t>(valueLength + (pad ? 1U : 0U));
  size_t offset = 2;
  if (pad) output[offset++] = 0;
  memcpy(output + offset, raw + start, valueLength);
  return offset + valueLength;
}

bool signRequest(const LaMarzoccoPersistedSettings &settings,
                 const char *message, char output[104]) {
  uint8_t digest[32];
  if (!sha256(message, strlen(message), digest)) return false;
  mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
  if (!importPrivateKey(settings.installationPrivateKey, key)) {
    secureWipe(digest, sizeof(digest));
    return false;
  }
  uint8_t raw[64];
  size_t rawLength = 0;
  const psa_status_t status = psa_sign_hash(
      key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest, sizeof(digest), raw,
      sizeof(raw), &rawLength);
  (void)psa_destroy_key(key);
  secureWipe(digest, sizeof(digest));
  if (status != PSA_SUCCESS || rawLength != sizeof(raw)) {
    secureWipe(raw, sizeof(raw));
    return false;
  }
  uint8_t der[72];
  const size_t rLength = encodeAsn1Integer(raw, 32, der + 2);
  const size_t sLength = encodeAsn1Integer(raw + 32, 32, der + 2 + rLength);
  der[0] = 0x30;
  der[1] = static_cast<uint8_t>(rLength + sLength);
  const bool ok = base64Encode(der, 2 + rLength + sLength, output, 104);
  secureWipe(raw, sizeof(raw));
  secureWipe(der, sizeof(der));
  return ok;
}

bool jsonEscape(const char *input, char *output, size_t capacity) {
  size_t used = 0;
  if (input == nullptr || output == nullptr || capacity == 0) return false;
  for (; *input != '\0'; ++input) {
    const char *escape = nullptr;
    if (*input == '"') escape = "\\\"";
    else if (*input == '\\') escape = "\\\\";
    else if (*input == '\b') escape = "\\b";
    else if (*input == '\f') escape = "\\f";
    else if (*input == '\n') escape = "\\n";
    else if (*input == '\r') escape = "\\r";
    else if (*input == '\t') escape = "\\t";
    const size_t count = escape == nullptr ? 1U : strlen(escape);
    if (used + count >= capacity) return false;
    if (escape == nullptr) output[used++] = *input;
    else {
      memcpy(output + used, escape, count);
      used += count;
    }
  }
  output[used] = '\0';
  return true;
}

const char *jsonText(const cJSON *object, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  return cJSON_IsString(item) && item->valuestring != nullptr
             ? item->valuestring
             : nullptr;
}

cJSON *parseResponse(const char *response) {
  return parseJsonDocumentWithinLimits(response, kResponseCapacity - 1,
                                       kResponseMaxValues);
}

// Until the account accepts La Marzocco's latest policy, the cloud answers
// every machine request with HTTP 403 and this code.
bool policyAcceptanceRequired(const char *response) {
  cJSON *root = parseResponse(response);
  const char *code = root == nullptr ? nullptr : jsonText(root, "code");
  const bool required = code != nullptr && strcmp(code, "USER_MUST_ACCEPT_POLICY") == 0;
  cJSON_Delete(root);
  return required;
}

// A command's outcome once its attempts are spent.
LaMarzoccoError commandFailure(bool accepted, uint16_t http, bool transport, bool policy) {
  return accepted ? LaMarzoccoError::UNCONFIRMED
      : policy ? LaMarzoccoError::POLICY_REQUIRED
      : http == 401 ? LaMarzoccoError::INVALID_AUTH
      : transport ? LaMarzoccoError::TRANSPORT
      : http != 0 && !laMarzoccoHttpRetryable(http) ? LaMarzoccoError::REJECTED
      : LaMarzoccoError::HTTP_ERROR;
}

// Field-wise identity for a queued command lane; the requestId makes an
// in-flight execution stale when the desired request is replaced.
bool sameCommandRequest(const LaMarzoccoRequest &left,
                        const LaMarzoccoRequest &right) {
  return left.requestId == right.requestId &&
         left.configGeneration == right.configGeneration &&
         left.targetDeciC == right.targetDeciC &&
         left.presetId == right.presetId && left.type == right.type;
}

// Persisted option bit that authorizes each power command direction.
uint8_t powerOptionBit(LaMarzoccoRequestType type) {
  return type == LaMarzoccoRequestType::SET_POWER_ON
             ? LA_MARZOCCO_POWER_ON_WITH_SCALE
             : LA_MARZOCCO_SHUTDOWN_WITH_SCALE;
}

bool sameSessionIdentity(const LaMarzoccoPersistedSettings &left,
                         const LaMarzoccoPersistedSettings &right) {
  return left.accountConfigured == right.accountConfigured &&
         strcmp(left.username, right.username) == 0 &&
         strcmp(left.password, right.password) == 0 &&
         memcmp(left.installationPrivateKey, right.installationPrivateKey,
                sizeof(left.installationPrivateKey)) == 0 &&
         strcmp(left.selectedSerial, right.selectedSerial) == 0;
}

// IDF aborts perform() on a 401 (auth support compiled out), so an HTTP
// status, not the transport result, names what the server answered.
const char *laMarzoccoCloudCallResult(bool canceled, bool overflow, bool ok,
                                 uint16_t httpStatus) {
  return canceled ? "canceled"
      : overflow ? "response_too_large"
      : ok ? "success"
      : httpStatus != 0 ? "http_error" : "transport_error";
}

}  // namespace

// The POST body and the response share storage: perform() sends the whole
// body before the first response byte. A redirect or an auth challenge would
// resend the body after the response began overwriting it.
#if defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_BASIC_AUTH) || \
    defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_DIGEST_AUTH)
#error "La Marzocco IoBuffer aliases body and response; HTTP client auth retries would corrupt the body"
#endif
struct OpenBrewByWeightLaMarzoccoService::IoBuffer {
  union {
    char response[kResponseCapacity];
    char body[kBodyCapacity];
  };

  IoBuffer() : response{} {}
};

struct OpenBrewByWeightLaMarzoccoService::WorkBuffer {
  LaMarzoccoPublicIdentityCache publicIdentity;
  char accessToken[kTokenCapacity] = {};
  char refreshToken[kTokenCapacity] = {};
  // Scratch for the formatted Authorization header. Lives beside the tokens in
  // external RAM so cloudRequest() never renders the header over the IoBuffer
  // union (which aliases response and body) and never adds a large stack frame.
  char authorization[kTokenCapacity + 8] = {};
  esp_http_client_handle_t client = nullptr;
  size_t responseUsed = 0;
  uint32_t accessTokenIssuedAtMs = 0;
  uint32_t lastRequestEndedAtMs = 0;
  uint16_t httpStatus = 0;
  int32_t transportStatus = 0;
  bool responseOverflow = false;
  bool transportFailure = false;
  bool policyRequired = false;
};

struct OpenBrewByWeightLaMarzoccoService::RequestStateGuard {
  OpenBrewByWeightLaMarzoccoService &service;
  ~RequestStateGuard() { service.clearRequestState(); }
};

#include "OpenBrewByWeightLaMarzoccoWebSocket.inc"
#include "OpenBrewByWeightLaMarzoccoCompanionRoute.inc"

bool OpenBrewByWeightLaMarzoccoService::begin() {
  initJsonParser();
  // The IDF WS transport prints signed Upgrade headers even on write errors.
  esp_log_level_set("transport_ws", ESP_LOG_NONE);
  if (task_ != nullptr || psa_crypto_init() != PSA_SUCCESS) {
    return false;
  }
  if (lockFlashIo()) {
    OpenBrewByWeightPreferences preferences(NvsSubsystem::SETTINGS);
    if (preferences.begin(SETTINGS_NAMESPACE, true)) {
      if (preferences.getBytesLength(LA_MARZOCCO_REJECTED_KEY) != sizeof(rejectedAccount_) ||
          preferences.getBytes(LA_MARZOCCO_REJECTED_KEY, &rejectedAccount_,
                               sizeof(rejectedAccount_)) != sizeof(rejectedAccount_))
        rejectedAccount_ = 0;
      preferences.end();
    }
    unlockFlashIo();
  }
  if (xTaskCreatePinnedToCore(taskEntry, "lm_cloud", kWorkerStackBytes, this,
                             tskIDLE_PRIORITY, &task_, 0) != pdPASS) {
    return false;
  }
  return true;
}

void OpenBrewByWeightLaMarzoccoService::publishConfig(
    const LaMarzoccoPersistedSettings &settings, uint32_t configGeneration) {
  // Compatibility mode parks the cloud client through the existing
  // unconfigured-account machinery: no session, no scheduling, in-flight HTTP
  // aborted. The persisted settings themselves stay untouched.
  LaMarzoccoPersistedSettings effective = settings;
  if (firmwareCompatibilityMode()) {
    effective = LaMarzoccoPersistedSettings{};
  }
  bool cancelTemperature = false;
  bool cancelPowerOff = false;
  bool cancelObservation = false;
  bool identityChanged = false;
  bool transportChanged = false;
  LaMarzoccoCompanionRoute companionFrom = LaMarzoccoCompanionRoute::DIRECT, companionTo;
  {
    TaskLockGuard lock(mux_);
    const bool wasObserving =
        config_.accountConfigured &&
        (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0;
    const bool configChanged =
        memcmp(&config_, &effective, sizeof(effective)) != 0;
    identityChanged = !sameSessionIdentity(config_, effective);
    transportChanged = config_.connectionType != effective.connectionType;
    // A new account, machine or transport, or no observation, ends the
    // companion route before the direct state below is reset.
    companionFrom = companion_.status.route;
    if (identityChanged || transportChanged || !effective.accountConfigured ||
        (effective.options & LA_MARZOCCO_OBSERVE_STATE) == 0)
      leaveCompanionLocked(identityChanged ? "account" : "settings", false);
    companionTo = companion_.status.route;
    if (identityChanged || transportChanged ||
        (effective.options & LA_MARZOCCO_OBSERVE_STATE) == 0)
      websocketUnexpectedReconnect_ = false;
    if (!identityChanged && (transportChanged ||
        (wasObserving && (effective.options & LA_MARZOCCO_OBSERVE_STATE) == 0))) {
      powerState_.hold(published_, wasObserving, millis());
      ++observationFence_.epoch;
      observationSchedule_.dueNow(millis());
    }
    config_ = effective;
    configGeneration_ = configGeneration;
    if (identityChanged) {
      ++identityGeneration_;
      ++observationFence_.epoch;
      published_ = {};
      mirrorPowerLocked();
      websocketStatus_.cleaningAvailable = false;
      websocketStatus_.cleaningAtMs = websocketStatus_.messageAtMs = websocketStatus_.powerAtMs = 0;
      websocketStatus_.connectedAtMs = websocketStatus_.pongAtMs = 0;
      websocketStatus_.machineConnectedKnown = false;
      websocketStatus_.cleaningLabel[0] = '\0';
      powerState_.reset();
      observationSchedule_ = {};
      wipeLaMarzoccoSettings(pending_.credentials);
      pending_.present = false;
      desiredTemperature_ = {};
      desiredPower_ = {};
      cancelTemperature = true;
      cancelPowerOff = true;
      ++companion_.outbox.credentialsGeneration;
      companion_.status.credentialsConfirmed = false;
    } else {
      if (desiredTemperature_.present) {
        desiredTemperature_.machineConfigGeneration = configGeneration;
      }
      if (desiredPower_.present &&
          (effective.options &
           powerOptionBit(desiredPower_.request.type)) != 0) {
        desiredPower_.machineConfigGeneration = configGeneration;
      }
    }
    refreshCompanionConfigLocked();
    published_.configGeneration = configGeneration;
    published_.connectionFreshness = effective.connectionType ==
        static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET);
    if (identityChanged || transportChanged || !effective.accountConfigured ||
        (effective.options & LA_MARZOCCO_OBSERVE_STATE) == 0) {
      backflush_.reset();
      snapshotPending_ = false;
      observationFence_.invalidate();
      observationFence_.offline = false;
    }
    published_.identityGeneration = identityGeneration_;
    published_.accountConfigured = effective.accountConfigured;
    const bool observing =
        effective.accountConfigured &&
        (effective.options & LA_MARZOCCO_OBSERVE_STATE) != 0;
    if (!configChanged) {
      wipeLaMarzoccoSettings(effective);
      return;
    }
    wipeLaMarzoccoSettings(candidate_);
    const uint8_t accountMachines =
        effective.accountConfigured ? discovery_.accountMachines : 0;
    discovery_ = {};
    discovery_.accountMachines = accountMachines;
    if (!effective.accountConfigured) {
      powerState_.reset();
      wipeLaMarzoccoSettings(pending_.credentials);
      pending_.present = false;
      desiredTemperature_ = {};
      desiredPower_ = {};
      published_.phase = LaMarzoccoPhase::Disabled;
      published_.quality = LaMarzoccoObservationQuality::UNCONFIGURED;
      published_.temperatureState = LaMarzoccoTemperatureState::Disabled;
      observationSchedule_ = {};
    } else if (identityChanged ||
               published_.phase == LaMarzoccoPhase::Disabled) {
      published_.phase = LaMarzoccoPhase::IDLE;
      published_.quality = LaMarzoccoObservationQuality::STALE;
      observationSchedule_.dueNow(millis());
    }
    if (!observing) {
      powerState_.reset();
      observationSchedule_ = {};
      if (wasObserving && effective.accountConfigured) {
        published_.phase = LaMarzoccoPhase::IDLE;
        published_.error = LaMarzoccoError::NONE;
      }
      if (pending_.present &&
          pending_.request.type == LaMarzoccoRequestType::OBSERVE_STATE) {
        wipeLaMarzoccoSettings(pending_.credentials);
        pending_.present = false;
      }
      cancelObservation = wasObserving;
    } else if (!wasObserving) {
      observationSchedule_.dueNow(millis());
    }
    if ((effective.options & LA_MARZOCCO_APPLY_TEMPERATURE) == 0) {
      desiredTemperature_ = {};
      published_.temperatureState = LaMarzoccoTemperatureState::Disabled;
      published_.temperatureError = LaMarzoccoError::NONE;
      published_.temperatureTransportStatus = 0;
      published_.temperatureHttpStatus = 0;
      published_.temperatureCommandAccepted = false;
      published_.temperatureRetryable = false;
      cancelTemperature = true;
    } else if (effective.accountConfigured &&
               published_.temperatureState ==
                   LaMarzoccoTemperatureState::Disabled) {
      published_.temperatureState = LaMarzoccoTemperatureState::IDLE;
    }
    if (desiredPower_.present &&
        (effective.options & powerOptionBit(desiredPower_.request.type)) == 0) {
      desiredPower_ = {};
      cancelPowerOff = true;
    }
  }
  const bool cloudDisabled =
      !effective.accountConfigured ||
      (effective.options & (LA_MARZOCCO_APPLY_TEMPERATURE |
                           LA_MARZOCCO_OBSERVE_STATE |
                           LA_MARZOCCO_SHUTDOWN_WITH_SCALE |
                           LA_MARZOCCO_POWER_ON_WITH_SCALE)) == 0;
  wipeLaMarzoccoSettings(effective);
  if (identityChanged || transportChanged || cloudDisabled) {
    if (identityChanged || transportChanged) {
      websocketRetryRequested_.store(true, std::memory_order_release);
      signInRetryAllowed_.store(true, std::memory_order_release);
    }
    clearSessionRequested_.store(true, std::memory_order_release);
    abortRequested_.store(true, std::memory_order_release);
  } else if ((cancelObservation &&
              observationActive_.load(std::memory_order_acquire)) ||
             (cancelTemperature &&
              temperatureActive_.load(std::memory_order_acquire)) ||
             (cancelPowerOff &&
              powerActive_.load(std::memory_order_acquire))) {
    abortRequested_.store(true, std::memory_order_release);
  }
  if (companionFrom != companionTo)
    logCompanionRoute(companionFrom, companionTo, identityChanged ? "account" : "settings");
  notifyCompanion();
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

void OpenBrewByWeightLaMarzoccoService::publishNetworkState(bool staConnected,
                                                  bool apActive,
                                                  bool shotActive,
                                                  bool scaleConnecting) {
  scaleConnecting = scaleConnecting || outboundScaleInhibited();
  shotActive = shotActive || outboundShotActive.load(std::memory_order_acquire);
  const bool wasSta = staConnected_.exchange(staConnected,
                                              std::memory_order_acq_rel);
  const bool wasAp = apActive_.exchange(apActive, std::memory_order_acq_rel);
  const bool wasActive = shotActive_.exchange(shotActive,
                                               std::memory_order_acq_rel);
  const bool wasScale = scaleConnecting_.exchange(
      scaleConnecting, std::memory_order_acq_rel);
  const bool scaleConnected = outboundScaleConnected.load(std::memory_order_acquire);
  const bool wasPaused = wasActive && scaleConnected;
  const bool paused = shotActive && scaleConnected;
  if (shotActive && !wasActive) {
    TaskLockGuard lock(mux_);
    desiredPower_ = {};
    if (powerActive_.load() || temperatureActive_.load())
      abortRequested_.store(true, std::memory_order_release);
  }
  if (scaleConnecting && !wasScale)
    abortRequested_.store(true, std::memory_order_release);
  const bool wasEligible = wasSta && !wasAp && !wasPaused && !wasScale;
  const bool eligible =
      staConnected && !apActive && !paused && !scaleConnecting;
  // A scaled-shot pause only cancels HTTP; the WS owner parks its subscription.
  if (paused && !wasPaused) abortRequested_.store(true, std::memory_order_release);
  if ((!staConnected && wasSta) || (apActive && !wasAp)) {
    {
      TaskLockGuard lock(mux_);
      if (!staConnected && wasSta && !apActive && wasEligible &&
          websocketStatus_.state >= LaMarzoccoSocketState::CONNECTING &&
          websocketStatus_.state <= LaMarzoccoSocketState::STREAMING)
        websocketUnexpectedReconnect_ = true;
      powerState_.hold(published_, config_.accountConfigured &&
          (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0, millis());
      ++observationFence_.epoch;
      if (!websocketStatus_.pauseAtMs) websocketStatus_.pauseAtMs = millis();
      observationFence_.invalidate();
      backflush_.invalidate();
      snapshotPending_ = false;
    }
    abortRequested_.store(true, std::memory_order_release);
    if (!staConnected || apActive) {
      clearSessionRequested_.store(true, std::memory_order_release);
    }
  } else if (eligible && !wasEligible) {
    TaskLockGuard lock(mux_);
    if (config_.accountConfigured &&
        (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0 &&
        published_.error != LaMarzoccoError::INVALID_AUTH) {
      observationSchedule_.dueNow(millis());
    }
    if (desiredTemperature_.present) {
      desiredTemperature_.retryAtMs = millis();
    }
    if (desiredPower_.present) {
      desiredPower_.retryAtMs = millis();
    }
  }
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

bool OpenBrewByWeightLaMarzoccoService::queueConnect(uint32_t requestId,
                                           const char *username,
                                           const char *password) {
  LaMarzoccoPersistedSettings credentials;
  if (!secureCopy(credentials.username, sizeof(credentials.username), username) ||
      !secureCopy(credentials.password, sizeof(credentials.password), password) ||
      !laMarzoccoBoundedText(credentials.username,
                             sizeof(credentials.username), false) ||
      strchr(credentials.username, '@') == nullptr ||
      !laMarzoccoBoundedText(credentials.password,
                             sizeof(credentials.password), false)) {
    wipeLaMarzoccoSettings(credentials);
    return false;
  }
  TaskLockGuard lock(mux_);
  if (pending_.present || active_ || task_ == nullptr) {
    wipeLaMarzoccoSettings(credentials);
    return false;
  }
  pending_ = {};
  pending_.request.requestId = requestId;
  pending_.request.configGeneration = configGeneration_;
  pending_.request.type = LaMarzoccoRequestType::CONNECT;
  signInRetryAllowed_.store(true, std::memory_order_release);
  pending_.credentials = credentials;
  pending_.identityGeneration = identityGeneration_;
  pending_.present = true;
  published_.requestId = requestId;
  published_.phase = LaMarzoccoPhase::QUEUED;
  published_.error = LaMarzoccoError::NONE;
  discovery_ = {};
  wipeLaMarzoccoSettings(candidate_);
  wipeLaMarzoccoSettings(credentials);
  xTaskNotifyGive(task_);
  return true;
}

// Lists the saved account's machines again so another one can be selected;
// the saved machine stays active until that selection is saved.
bool OpenBrewByWeightLaMarzoccoService::queueRelist(uint32_t requestId) {
  TaskLockGuard lock(mux_);
  if (pending_.present || active_ || task_ == nullptr ||
      !config_.accountConfigured) {
    return false;
  }
  pending_ = {};
  pending_.request.requestId = requestId;
  pending_.request.configGeneration = configGeneration_;
  pending_.request.type = LaMarzoccoRequestType::LIST_MACHINES;
  signInRetryAllowed_.store(true, std::memory_order_release);
  pending_.credentials = config_;
  pending_.identityGeneration = identityGeneration_;
  pending_.present = true;
  const uint8_t accountMachines = discovery_.accountMachines;
  discovery_ = {};
  discovery_.accountMachines = accountMachines;
  discovery_.listing = true;
  wipeLaMarzoccoSettings(candidate_);
  xTaskNotifyGive(task_);
  return true;
}

bool OpenBrewByWeightLaMarzoccoService::queue(const LaMarzoccoRequest &request) {
  // Edge-triggered mutations stay blocking, but the anomaly report must not
  // nest logging inside mux_: gate and gather under the lock, emit after it.
  const uint32_t muxLockStartedUs = micros();
  TaskLockGuard lock(mux_);
  const uint32_t waitedUs = micros() - muxLockStartedUs;
  static uint32_t lastMuxWaitReportAtMs = 0;
  const bool reportMuxWait =
      muxWaitAnomalyDue(waitedUs, lastMuxWaitReportAtMs, millis());
  char muxWaitHolder[16] = {};
  uint32_t muxWaitHoldUs = 0;
  if (reportMuxWait) {
    snprintf(muxWaitHolder, sizeof(muxWaitHolder), "%s",
             mux_.longestHoldOwnerName());
    muxWaitHoldUs = mux_.longestHoldUs();
  }
  mux_.resetLongestHold();
  const bool accepted = queueLocked(request);
  lock.unlock();
  if (reportMuxWait)
    reportMuxWaitAnomaly(waitedUs, muxWaitHolder, muxWaitHoldUs);
  return accepted;
}

bool OpenBrewByWeightLaMarzoccoService::queueLocked(const LaMarzoccoRequest &request) {
  if (request.type == LaMarzoccoRequestType::APPLY_TEMPERATURE) {
    if (task_ == nullptr || !config_.accountConfigured ||
        (config_.options & LA_MARZOCCO_APPLY_TEMPERATURE) == 0 ||
        request.presetId == 0 ||
        request.targetDeciC < LA_MARZOCCO_BREW_TARGET_MIN_DECI_C ||
        request.targetDeciC > LA_MARZOCCO_BREW_TARGET_MAX_DECI_C) {
      return false;
    }
    if (desiredTemperature_.present &&
        tickBefore(request.configGeneration, desiredTemperature_.request.configGeneration)) {
      return true;
    }
    desiredTemperature_.request = request;
    desiredTemperature_.request.requestId = nextAutomaticRequestId_++;
    desiredTemperature_.machineConfigGeneration = configGeneration_;
    desiredTemperature_.retryAtMs = millis();
    desiredTemperature_.cycles = 0;
    desiredTemperature_.present = true;
    desiredTemperature_.commandAccepted = false;
    published_.requestedTargetDeciC = request.targetDeciC;
    published_.temperatureState = LaMarzoccoTemperatureState::PENDING;
    published_.temperatureError = LaMarzoccoError::NONE;
    published_.temperatureTransportStatus = 0;
    published_.temperatureHttpStatus = 0;
    published_.temperatureCommandAccepted = false;
    published_.temperatureRetryable = true;
    if (temperatureActive_.load(std::memory_order_acquire)) {
      abortRequested_.store(true, std::memory_order_release);
    }
    xTaskNotifyGive(task_);
    return true;
  }
  if (request.type == LaMarzoccoRequestType::SET_STANDBY ||
      request.type == LaMarzoccoRequestType::SET_POWER_ON) {
    const uint32_t shotGeneration = shotGeneration_.load(std::memory_order_acquire);
    if (task_ == nullptr || !config_.accountConfigured ||
        shotActive_.load(std::memory_order_acquire) || outboundShotActive.load(std::memory_order_acquire) ||
        (config_.options & powerOptionBit(request.type)) == 0) {
      return false;
    }
    desiredPower_.request = request;
    desiredPower_.request.requestId = nextAutomaticRequestId_++;
    desiredPower_.machineConfigGeneration = configGeneration_;
    desiredPower_.shotGeneration = shotGeneration;
    desiredPower_.retryAtMs = millis();
    desiredPower_.cycles = 0;
    desiredPower_.present = true;
    desiredPower_.commandAccepted = false;
    if (powerActive_.load(std::memory_order_acquire)) {
      abortRequested_.store(true, std::memory_order_release);
    }
    xTaskNotifyGive(task_);
    return true;
  }
  if (pending_.present || active_ || task_ == nullptr ||
      !config_.accountConfigured ||
      (config_.options & LA_MARZOCCO_OBSERVE_STATE) == 0 ||
      (request.type != LaMarzoccoRequestType::OBSERVE_STATE &&
       request.type != LaMarzoccoRequestType::ACCEPT_POLICY)) {
    return false;
  }
  pending_ = {};
  pending_.request = request;
  pending_.identityGeneration = identityGeneration_;
  pending_.present = true;
  published_.requestId = request.requestId;
  websocketRetryRequested_.store(true, std::memory_order_release);
  signInRetryAllowed_.store(true, std::memory_order_release);
  published_.phase = LaMarzoccoPhase::QUEUED;
  published_.error = LaMarzoccoError::NONE;
  xTaskNotifyGive(task_);
  return true;
}

bool OpenBrewByWeightLaMarzoccoService::selectDiscoveredMachine(
    const char *serial, LaMarzoccoPersistedSettings &settings) const {
  TaskLockGuard lock(mux_);
  if (serial == nullptr || discovery_.count == 0 ||
      !laMarzoccoPrivateKeyConfigured(candidate_)) {
    return false;
  }
  for (uint8_t index = 0; index < discovery_.count; ++index) {
    if (strcmp(discovery_.machines[index].serial, serial) != 0) continue;
    settings = candidate_;
    if (!secureCopy(settings.selectedSerial, sizeof(settings.selectedSerial),
                    discovery_.machines[index].serial) ||
        !secureCopy(settings.selectedName, sizeof(settings.selectedName),
                    discovery_.machines[index].name)) {
      wipeLaMarzoccoSettings(settings);
      return false;
    }
    settings.accountConfigured = true;
    return validLaMarzoccoSettings(settings);
  }
  return false;
}

void OpenBrewByWeightLaMarzoccoService::clearDiscovery() {
  TaskLockGuard lock(mux_);
  wipeLaMarzoccoSettings(candidate_);
  discovery_ = {};
}

LaMarzoccoStatus OpenBrewByWeightLaMarzoccoService::currentStatus() const {
  TaskLockGuard lock(mux_);
  return statusLocked();
}

bool OpenBrewByWeightLaMarzoccoService::liveSubscriptionLocked() const {
  if (companion_.status.route == LaMarzoccoCompanionRoute::COMPANION) return companionFeedLiveLocked();
  return websocketStatus_.subscribed && websocket_ && !websocket_->failed.load() &&
         websocket_->stamp.epoch == observationFence_.epoch && staConnected_.load() &&
         !apActive_.load() && !outboundMaintenance.load();
}

// A live subscription pushes the machine's new state seconds after an accepted
// command, so wait for that push before paying for an HTTPS verification read.
bool OpenBrewByWeightLaMarzoccoService::pushConfirms(uint16_t targetDeciC,
                                                LaMarzoccoPowerState power) {
  const uint32_t deadline = millis() + la_marzocco_timing::kPushConfirmMs;
  do {
    {
      TaskLockGuard lock(mux_);
      // The companion delivers verification reads as well as pushes.
      if ((!published_.connectionFreshness && !companionRoute_.load()) ||
          observationFence_.offline || !liveSubscriptionLocked()) return false;
      if (targetDeciC ? observationFence_.temperatureTrusted && published_.targetValid &&
                            published_.targetDeciC == targetDeciC
                      : observationFence_.powerTrusted && published_.powerState == power)
        return true;
    }
    if (abortRequested_.load(std::memory_order_acquire)) return false;
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
  } while (!tickReached(millis(), deadline));
  return false;
}

LaMarzoccoStatus OpenBrewByWeightLaMarzoccoService::statusLocked() const {
  LaMarzoccoStatus result = published_;
  const uint32_t now = millis();
  const bool observing = config_.accountConfigured &&
                         (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0;
  result = powerState_.effectiveStatus(result, observing, now);
  if (result.connectionFreshness && result.quality == LaMarzoccoObservationQuality::CURRENT &&
      (!observationFence_.synchronized || !liveSubscriptionLocked()))
    result.quality = LaMarzoccoObservationQuality::STALE;
  result.assumedPower = laMarzoccoAssumedPower(result);
  result.staConnected = staConnected_.load(std::memory_order_acquire);
  result.apActive = apActive_.load(std::memory_order_acquire);
  result.shotPaused = shotTransportPaused();
  result.scalePaused = !result.connectionFreshness && scaleConnecting_.load(std::memory_order_acquire);
  return result;
}

LaMarzoccoStatus OpenBrewByWeightLaMarzoccoService::statusPoll() const {
  if (mux_.tryLock()) {
    statusCache_ = statusLocked();
    statusCacheWrittenAtMs_ = millis();
    mux_.unlock();
  }
  return statusCache_;
}

bool OpenBrewByWeightLaMarzoccoService::statusPollStale() const {
  // 250 ms is ~50 consecutive misses at the loop's 1-5 ms poll cadence:
  // genuinely anomalous contention, not a busy instant.
  constexpr uint32_t kStatusPollCacheStaleMs = 250;
  return millis() - statusCacheWrittenAtMs_ >= kStatusPollCacheStaleMs;
}

HeapLifecycleAggregate OpenBrewByWeightLaMarzoccoService::heapTelemetry() const {
  TaskLockGuard lock(mux_);
  return tlsHeap_.aggregate;
}

LaMarzoccoCloudCall OpenBrewByWeightLaMarzoccoService::cloudCall() const {
  TaskLockGuard lock(mux_);
  return publishedCloudCall_;
}

void OpenBrewByWeightLaMarzoccoService::qualifyBackflushLocked() {
  const uint32_t now = millis();
  const uint32_t aliveAt = websocketStatus_.pongAtMs ? websocketStatus_.pongAtMs
                                                   : websocketStatus_.connectedAtMs;
  const bool linkLost = !config_.accountConfigured || !(config_.options & LA_MARZOCCO_OBSERVE_STATE) ||
      config_.connectionType != static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET) ||
      // The companion feed qualifies like the socket: subscribed on the current
      // identity and epoch, `up`, alive by the route's own rule (a ping within
      // 30 s, none expected during a sync hold such as this closed relay) and a
      // known token.
      (companion_.status.route == LaMarzoccoCompanionRoute::COMPANION
           ? !companionFeedLiveLocked() ||
                 (!companion_.syncHold && now - companion_.pingAtMs >= la_marzocco_timing::kCompanionAliveMs) ||
                 companion_.tokenRenewAtMs == 0
           : !websocket_ || websocket_->failed.load() || !websocketStatus_.subscribed ||
                 !tokenAvailable_.load(std::memory_order_acquire) ||
                 websocket_->stamp.identity != identityGeneration_ ||
                 websocket_->stamp.epoch != observationFence_.epoch ||
                 now - aliveAt >= 30000) ||
      !websocketStatus_.machineConnectedKnown || !websocketStatus_.machineConnected ||
      observationFence_.offline || !staConnected_.load() || apActive_.load();
  if (linkLost || shotActive_.load() || outboundShotActive.load() || outboundMaintenance.load())
    backflush_.invalidate();
  // Only a recognized cloud status can resolve a cleaning episode. Without
  // one (link lost, unknown label, widget removed), once any program has
  // certainly ended, starts follow the ordinary caps; local activity pauses
  // supervision without hiding the machine's state.
  if (!linkLost && websocketStatus_.cleaning != LaMarzoccoCleaningState::UNKNOWN)
    backflushUnsupervisedAtMs_ = 0;
  else if (!backflushUnsupervisedAtMs_) backflushUnsupervisedAtMs_ = now ? now : 1;
  else if (now - backflushUnsupervisedAtMs_ >= BACKFLUSH_MAX_CIRCUIT_CLOSED_MS) backflush_.release();
}

bool OpenBrewByWeightLaMarzoccoService::backflushReadyLocked() const {
  const auto &state = backflush_.status();
  return state.valid && state.phase == MachineBackflushPhase::AWAITING &&
      !state.consumed &&
      (companion_.status.route == LaMarzoccoCompanionRoute::COMPANION
           ? companion_.tokenRenewAtMs != 0 &&
                 static_cast<int32_t>(companion_.tokenRenewAtMs - millis()) > 210000
           : tokenAvailable_.load(std::memory_order_acquire) &&
                 millis() - tokenIssuedAtMs_.load(std::memory_order_acquire) <
                     la_marzocco_timing::kAccessTokenRefreshAgeMs - 210000) &&
      observationFence_.synchronized && published_.powerState == LaMarzoccoPowerState::ON &&
      !temperatureActive_.load() && !powerActive_.load();
}

MachineBackflushSnapshot OpenBrewByWeightLaMarzoccoService::backflush(bool consume) {
  // An idle handoff cannot become ready without a mutation when it is invalid
  // or the machine is off (readiness requires power ON), so the control
  // loop's consuming poll reuses its last locked read until a mutation marks
  // a change or the power mirror flips back on.
  if (consume && !backflush_.changed() && !postedPending() &&
      (!idleBackflush_.valid || powerOff_.load(std::memory_order_acquire))) {
    laMarzoccoBackflushWaitStat().note(0);
    laMarzoccoBackflushHoldStat().note(0);
    return idleBackflush_;
  }
  // The control loop polls this every iteration and must not wait behind a
  // core-0 holder: the wait stat keeps recording the acquisition attempt
  // (microseconds uncontended), the hold stat the locked section.
  const uint32_t muxLockStartedUs = micros();
  if (!mux_.tryLock()) {
    laMarzoccoBackflushWaitStat().note(micros() - muxLockStartedUs);
    laMarzoccoBackflushHoldStat().note(0);
    // Busy, no new edges: transitions keep accumulating in the bounded ring
    // while validity and permits stay the last locked resolution's. The
    // idle handoff is never served here; mid-episode it is stale and would
    // trip spurious supervision aborts.
    MachineBackflushSnapshot missed = lastPollSnapshot_;
    missed.count = 0;
    return missed;
  }
  const uint32_t lockedAtUs = micros();
  laMarzoccoBackflushWaitStat().note(lockedAtUs - muxLockStartedUs);
  applyPostedLocked();
  qualifyBackflushLocked();
  auto result = consume ? backflush_.take() : backflush_.status();
  result.ready = backflushReadyLocked();
  if (consume && !result.busy() && result.count == 0 &&
      (!result.valid || published_.powerState == LaMarzoccoPowerState::OFF)) {
    idleBackflush_ = result;
    backflush_.settle();
  }
  laMarzoccoBackflushHoldStat().note(micros() - lockedAtUs);
  lastPollSnapshot_ = result;
  mux_.unlock();
  return result;
}

bool OpenBrewByWeightLaMarzoccoService::muxWaitAnomalyDue(
    uint32_t waitedUs, uint32_t &lastReportAtMs, uint32_t nowMs) {
  // Normal waits are microseconds; 2 ms is the abnormality bar for any
  // control-loop phase, so a wait at or past it names the holder and the
  // phase maximum can be attributed after the fact. Rate-limited to keep a
  // systematically stalled holder from flooding the log. Callers hold the
  // mutex, which also serializes the latch.
  constexpr uint32_t kMuxWaitReportUs = 2000;
  constexpr uint32_t kMuxWaitReportPeriodMs = 10000;
  if (waitedUs < kMuxWaitReportUs ||
      nowMs - lastReportAtMs < kMuxWaitReportPeriodMs) {
    return false;
  }
  lastReportAtMs = nowMs;
  return true;
}

void OpenBrewByWeightLaMarzoccoService::reportMuxWaitAnomaly(
    uint32_t waitedUs, const char *holder, uint32_t holdUs) {
  // Must run after unlock(): the logging path takes locks of its own, and
  // the longest-hold attribution was copied while mux_ was held (the waiter
  // resets it there, so each report covers the holds since its last lock).
  serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                       "La Marzocco mux wait=%lu us holder=%s hold=%lu us",
                       static_cast<unsigned long>(waitedUs), holder,
                       static_cast<unsigned long>(holdUs));
  // The audit excerpt keeps only what the packed args cannot carry: who held
  // the lock. Magnitudes rebuild from the arguments at the JSON surface.
  char excerpt[AUDIT_EXCERPT_BYTES] = {};
  snprintf(excerpt, sizeof(excerpt), "holder=%s", holder);
  addDebugEventDetail(DebugCategory::NETWORK, DebugCode::MACHINE_MUX_WAIT,
                      static_cast<int32_t>(waitedUs),
                      static_cast<int32_t>(holdUs), excerpt);
}

void OpenBrewByWeightLaMarzoccoService::finishBackflush(uint32_t attempt) {
  if (!mux_.tryLock()) {
    // finish()'s fence lands now; only a real permit release waits. The
    // task's own last resolution drops that permit at once so its contended
    // reads stay coherent with the release it requested. Attempts are only
    // admitted under the lock, after the pending release, so one slot holds.
    backflush_.fence();
    if (attempt == 0) return;
    postedFinishAttempt_ = attempt;
    if (lastPollSnapshot_.permit.attempt == attempt) lastPollSnapshot_.permit.attempt = 0;
    return;
  }
  applyPostedLocked();
  backflush_.finish(attempt);
  mux_.unlock();
}

MachinePhysicalStartDisposition OpenBrewByWeightLaMarzoccoService::physicalStart(MachineBackflushPermit *permit) {
  if (mux_.tryLock()) {
    applyPostedLocked();
    const auto disposition = physicalStartLocked(permit);
    mux_.unlock();
    return disposition;
  }
  // Contended edge, decided from the last locked backflush resolution
  // without waiting. Admission and wake recognition need the locked state,
  // so a busy cleaning episode refuses the edge (the barista presses again)
  // and anything else is a normal start. The fence still retires bytes in
  // flight, as start() does for every evaluated edge.
  if (permit) *permit = {};
  backflush_.fence();
  if (!lastPollSnapshot_.busy()) return MachinePhysicalStartDisposition::NORMAL;
  if (powerActive_.load() || temperatureActive_.load())
    abortRequested_.store(true, std::memory_order_release);
  return MachinePhysicalStartDisposition::BLOCKED_CLEANING;
}

void OpenBrewByWeightLaMarzoccoService::post(const LaMarzoccoRequest &request) {
  if (mux_.tryLock()) {
    applyPostedLocked();
    (void)queueLocked(request);
    mux_.unlock();
    return;
  }
  if (request.type == LaMarzoccoRequestType::APPLY_TEMPERATURE) {
    postedTemperature_ = request;
    postedTemperaturePresent_ = true;
  } else {
    postedPower_ = request;
    postedPowerPresent_ = true;
  }
}

void OpenBrewByWeightLaMarzoccoService::applyPostedLocked() {
  if (postedFinishAttempt_ != 0) backflush_.finish(postedFinishAttempt_);
  if (postedTemperaturePresent_) (void)queueLocked(postedTemperature_);
  if (postedPowerPresent_) (void)queueLocked(postedPower_);
  postedFinishAttempt_ = 0;
  postedTemperaturePresent_ = postedPowerPresent_ = false;
}

MachinePhysicalStartDisposition OpenBrewByWeightLaMarzoccoService::physicalStartLocked(
    MachineBackflushPermit *permit) {
  const uint32_t now = millis();
  qualifyBackflushLocked();
  const bool ready = backflushReadyLocked();
  const auto admitted = backflush_.start();
  if (permit) *permit = admitted;
  if (backflush_.status().busy()) {
    // Commands and power optimism cannot race cleaning admission. Session
    // renewal is refused near the boundary, never deferred past token expiry.
    desiredPower_ = {};
    if (powerActive_.load() || temperatureActive_.load())
      abortRequested_.store(true, std::memory_order_release);
    if (admitted.attempt && ready) {
      // Admission-time poll misses must serve the post-admission resolution.
      lastPollSnapshot_ = backflush_.status();
      return MachinePhysicalStartDisposition::BACKFLUSH_CANDIDATE;
    }
    backflush_.finish(admitted.attempt);
    if (permit) *permit = {};
    return MachinePhysicalStartDisposition::BLOCKED_CLEANING;
  }
  const bool observing = config_.accountConfigured &&
                         (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0;
  // Qualification first (the gesture is read against the state before the
  // press), then the press itself as evidence that the machine is on.
  const bool wake = powerState_.notePhysicalStart(
      published_, observing, (config_.options & LA_MARZOCCO_RECOGNIZE_WAKE) != 0, now);
  powerState_.notePhysicalEvidence(published_, observing, now);
  if (!wake) return MachinePhysicalStartDisposition::NORMAL;
  observationSchedule_.armPostEvent(now);
  return MachinePhysicalStartDisposition::WAKE_PASSTHROUGH;
}

LaMarzoccoDiscoverySnapshot OpenBrewByWeightLaMarzoccoService::discovery() const {
  TaskLockGuard lock(mux_);
  return discovery_;
}

void OpenBrewByWeightLaMarzoccoService::taskEntry(void *context) {
  static_cast<OpenBrewByWeightLaMarzoccoService *>(context)->taskLoop();
}

void OpenBrewByWeightLaMarzoccoService::taskLoop() {
  for (;;) {
    // Only the idle owner consumes cancellation, before selecting new work.
    abortRequested_.store(false, std::memory_order_release);
    if (clearSessionRequested_.exchange(false, std::memory_order_acq_rel)) {
      stopWebSocket(true);
      {
        TaskLockGuard lock(mux_);
        websocketStatus_.state = LaMarzoccoSocketState::Disabled;
        websocketStatus_.retryAtMs = 0;
      }
      releaseWorkBuffer();
    }
    if (!serviceCompanion()) serviceWebSocket();
    if (outboundMaintenance.load()) releaseWorkBuffer();
    const bool scaleInhibited = outboundScaleInhibited();
    bool websocketMode;
    {
      TaskLockGuard lock(mux_);
      websocketMode = config_.connectionType ==
          static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET);
    }
    // Idle owner closes any HTTP keepalive before acknowledging the pause. The
    // rare WebSocket-mode API calls share a connection only within one
    // operation; API polling keeps it for kHttpReuseIdleMs.
    if ((scaleInhibited || websocketMode) && work_ && work_->client) {
      esp_http_client_cleanup(work_->client);
      work_->client = nullptr;
    }
    if (scaleInhibited)
      completeOutboundAcquisitionPause(OutboundClient::LA_MARZOCCO_HTTP, millis());
    PendingRequest pending;
    bool haveRequest = false;
    bool haveTemperature = false;
    bool havePower = false;
    uint32_t temperatureMachineConfigGeneration = 0;
    uint32_t powerMachineConfigGeneration = 0;
    const uint32_t now = millis();
    LaMarzoccoError observationGate = LaMarzoccoError::NONE;
    const bool networkReady = networkEligible(observationGate);
    if (wasNetworkReady_ != networkReady) {
      wasNetworkReady_ = networkReady;
      if (networkReady) {
        // The readiness gates just started passing (clock sync or STA
        // recovery): a deferred command must not wait out a stale cooldown
        // before its next attempt.
        TaskLockGuard lock(mux_);
        if (desiredTemperature_.present) {
          desiredTemperature_.retryAtMs = now;
        }
        if (desiredPower_.present) {
          desiredPower_.retryAtMs = now;
        }
      }
    }
    const bool localActivity = shotTransportPaused();
    bool settledFromPush = false;
    LaMarzoccoBoilerState coffee, steam;
    uint32_t coffeeReadyAt, steamReadyAt;
    bool boilersChanged, cleaningChanged;
    char cleaning[sizeof(loggedCleaning_)] = {};
    {
      TaskLockGuard lock(mux_);
      // Boiler readiness is gathered here and logged after the lock, once per
      // change of a state or of the cloud's ready estimate.
      coffee = published_.coffeeBoiler;
      steam = published_.steamBoiler;
      coffeeReadyAt = published_.coffeeReadyAtUtcSec;
      steamReadyAt = published_.steamReadyAtUtcSec;
      boilersChanged = coffee != loggedCoffee_ || steam != loggedSteam_ ||
          coffeeReadyAt != loggedCoffeeReadyAt_ || steamReadyAt != loggedSteamReadyAt_;
      loggedCoffee_ = coffee;
      loggedSteam_ = steam;
      loggedCoffeeReadyAt_ = coffeeReadyAt;
      loggedSteamReadyAt_ = steamReadyAt;
      // An unrecognized cleaning status, logged once per change after the lock.
      if (websocketStatus_.cleaningAtMs && websocketStatus_.cleaning == LaMarzoccoCleaningState::UNKNOWN)
        copyLaMarzoccoText(cleaning, sizeof(cleaning),
                      websocketStatus_.cleaningAvailable ? websocketStatus_.cleaningLabel : "(removed)");
      cleaningChanged = strcmp(cleaning, loggedCleaning_) != 0;
      memcpy(loggedCleaning_, cleaning, sizeof(cleaning));
      // The companion feed keeps the state current: no polling and no reads.
      const bool companion = companion_.status.route == LaMarzoccoCompanionRoute::COMPANION;
      if (config_.connectionType == static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET) ||
          companion)
        observationSchedule_.suspendPeriodic();
      if (desiredPower_.present && desiredPower_.shotGeneration != shotGeneration_.load(std::memory_order_acquire))
        desiredPower_ = {};
      const bool pendingObservation =
          pending_.present &&
          pending_.request.type == LaMarzoccoRequestType::OBSERVE_STATE;
      const bool cleaningBusy = backflush_.status().busy();
      if (cleaningBusy) desiredPower_ = {};
      const bool observationReady =
          networkReady && !localActivity && !scaleConnecting_.load(std::memory_order_acquire) &&
          observationSchedule_.observationAllowed(now);
      if (pending_.present && networkReady && !localActivity &&
          !scaleConnecting_.load(std::memory_order_acquire) &&
          (!pendingObservation || observationReady)) {
        pending = pending_;
        wipeLaMarzoccoSettings(pending_.credentials);
        pending_.present = false;
        observationActive_.store(pendingObservation,
                                 std::memory_order_release);
        active_ = true;
        haveRequest = true;
      } else if (networkReady && !shotActive_.load(std::memory_order_acquire) &&
                 !outboundShotActive.load(std::memory_order_acquire) &&
                 !scaleConnecting_.load(std::memory_order_acquire) &&
                 !cleaningBusy && desiredTemperature_.present &&
                 tickReached(now, desiredTemperature_.retryAtMs)) {
        pending.request = desiredTemperature_.request;
        temperatureMachineConfigGeneration =
            desiredTemperature_.machineConfigGeneration;
        active_ = true;
        haveRequest = true;
        haveTemperature = true;
        temperatureActive_.store(true, std::memory_order_release);
      } else if (networkReady && !shotActive_.load(std::memory_order_acquire) &&
                 !outboundShotActive.load(std::memory_order_acquire) &&
                 !scaleConnecting_.load(std::memory_order_acquire) &&
                 desiredPower_.present &&
                 tickReached(now, desiredPower_.retryAtMs)) {
        pending.request = desiredPower_.request;
        powerMachineConfigGeneration =
            desiredPower_.machineConfigGeneration;
        active_ = true;
        haveRequest = true;
        havePower = true;
        powerActive_.store(true, std::memory_order_release);
      } else if (observationReady && config_.accountConfigured &&
                 (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0 &&
                 !snapshotPending_ && observationSchedule_.automaticDue(now) &&
                 (published_.connectionFreshness || companion) &&
                 observationFence_.powerTrusted && liveSubscriptionLocked()) {
        // A live subscription would already have pushed the wake: settle the
        // post-wake optimism from pushed evidence; HTTPS stays the fallback.
        (void)observationSchedule_.observationStarted(now);
        (void)powerState_.noteEvidence(powerState_.generation(), published_.powerState,
                                       now, true);
        settledFromPush = true;
      } else if (!companion && observationReady && config_.accountConfigured &&
                 (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0 &&
                 (snapshotPending_ || observationSchedule_.automaticDue(now))) {
        pending.request.requestId = nextAutomaticRequestId_++;
        pending.request.configGeneration = configGeneration_;
        pending.request.type = LaMarzoccoRequestType::OBSERVE_STATE;
        pending.identityGeneration = identityGeneration_;
        observationActive_.store(true, std::memory_order_release);
        active_ = true;
        haveRequest = true;
      }
      if (haveRequest && pending.request.type == LaMarzoccoRequestType::OBSERVE_STATE) {
        const bool reconcile = observationSchedule_.observationStarted(now);
        if (snapshotPending_) {
          // Reconciliation needs a request-time baseline, even if wake preceded subscription.
          pending.initialSnapshot = !reconcile && snapshotStamp_.intent == powerState_.generation();
          pending.snapshotStamp = snapshotStamp_;
          snapshotPending_ = false;
        } else if (reconcile) pending.initialSnapshot = false;
      }
    }
    if (settledFromPush)
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                           "La Marzocco post-wake state settled from WebSocket, read skipped");
    if (boilersChanged) {
      // Seconds until each cloud estimate; "none" without one or a clock.
      const time_t utcNow = time(nullptr);
      char coffeeEta[16] = "none", steamEta[16] = "none";
      if (utcNow >= 1700000000 && coffeeReadyAt)
        snprintf(coffeeEta, sizeof(coffeeEta), "%lds", long(coffeeReadyAt - utcNow));
      if (utcNow >= 1700000000 && steamReadyAt)
        snprintf(steamEta, sizeof(steamEta), "%lds", long(steamReadyAt - utcNow));
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                           "La Marzocco boilers brew=%s ready_in=%s steam=%s ready_in=%s",
                           laMarzoccoBoilerStateName(coffee), coffeeEta,
                           laMarzoccoBoilerStateName(steam), steamEta);
    }
    if (cleaningChanged && cleaning[0])
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                           "La Marzocco cleaning status \"%s\" not recognized: paddle starts "
                           "refused for up to 3 min", cleaning);
    if (haveRequest) {
      if (haveTemperature) {
        executeTemperatureApplication(pending.request,
                                      temperatureMachineConfigGeneration);
        temperatureActive_.store(false, std::memory_order_release);
      } else if (havePower) {
        executePowerApplication(pending.request,
                                   powerMachineConfigGeneration);
        powerActive_.store(false, std::memory_order_release);
      } else {
        execute(pending);
        observationActive_.store(false, std::memory_order_release);
      }
      wipeLaMarzoccoSettings(pending.credentials);
      TaskLockGuard lock(mux_);
      active_ = false;
      continue;
    }
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
  }
}

bool OpenBrewByWeightLaMarzoccoService::networkEligible(LaMarzoccoError &error, bool observation) const {
  if (outboundMaintenance.load(std::memory_order_acquire)) {
    error = LaMarzoccoError::CANCELED;
    return false;
  }
  if (!observation && outboundScaleInhibited()) {
    error = LaMarzoccoError::CANCELED;
    return false;
  }
  if (!observation && shotTransportPaused()) {
    error = LaMarzoccoError::CANCELED;
    return false;
  }
  if (apActive_.load(std::memory_order_acquire)) {
    error = LaMarzoccoError::AP_MODE;
    return false;
  }
  if (!staConnected_.load(std::memory_order_acquire)) {
    error = LaMarzoccoError::NO_STA;
    return false;
  }
  timeval now{};
  gettimeofday(&now, nullptr);
  if (now.tv_sec < 1700000000L) {
    error = LaMarzoccoError::CLOCK_UNSYNCED;
    return false;
  }
  error = LaMarzoccoError::NONE;
  return true;
}

bool OpenBrewByWeightLaMarzoccoService::temperatureEligible(
    LaMarzoccoError &error) const {
  if (!networkEligible(error)) return false;
  {
    TaskLockGuard lock(mux_);
    // An offline machine cannot apply a target: wait locally for it to come
    // back instead of spending cloud cycles that cannot confirm.
    if (backflush_.status().busy() || observationFence_.offline) {
      error = LaMarzoccoError::CANCELED;
      return false;
    }
  }
  if (shotActive_.load(std::memory_order_acquire) ||
      outboundShotActive.load(std::memory_order_acquire) ||
      scaleConnecting_.load(std::memory_order_acquire)) {
    error = LaMarzoccoError::CANCELED;
    return false;
  }
  return true;
}

void OpenBrewByWeightLaMarzoccoService::execute(PendingRequest &pending) {
  if (pending.request.type == LaMarzoccoRequestType::LIST_MACHINES) {
    executeRelist(pending);
    return;
  }
  LaMarzoccoStatus status;
  {
    TaskLockGuard lock(mux_);
    status = published_;
  }
  status.requestId = pending.request.requestId;
  status.configGeneration = pending.request.configGeneration;
  status.identityGeneration = pending.identityGeneration;
  status.error = LaMarzoccoError::NONE;
  status.transportStatus = 0;
  status.httpStatus = 0;
  LaMarzoccoError gateError = LaMarzoccoError::NONE;
  if (!networkEligible(gateError)) {
    if (pending.request.type == LaMarzoccoRequestType::OBSERVE_STATE) {
      deferObservation(pending, status, gateError);
      return;
    }
    fail(status, gateError);
    scheduleAutomatic(millis(), true);
    return;
  }
  if (pending.request.type == LaMarzoccoRequestType::OBSERVE_STATE &&
      shotTransportPaused()) {
    deferObservation(pending, status, LaMarzoccoError::CANCELED);
    return;
  }
  if (!ensureWorkBuffer()) {
    fail(status, LaMarzoccoError::HTTP_ERROR);
    scheduleAutomatic(
        millis(), pending.request.type == LaMarzoccoRequestType::CONNECT);
    return;
  }
  work_->transportStatus = 0;
  work_->httpStatus = 0;
  if (pending.request.type == LaMarzoccoRequestType::ACCEPT_POLICY) {
    LaMarzoccoPersistedSettings settings;
    {
      TaskLockGuard lock(mux_);
      settings = config_;
    }
    const bool accepted = ensureSession(settings, false) && acceptPolicy(settings);
    wipeLaMarzoccoSettings(settings);
    if (!accepted) {
      fail(status, classifyFailure());
      scheduleAutomatic(millis(), true);
      return;
    }
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                         "La Marzocco policy accepted (profiling and marketing declined)");
    pending.request.type = LaMarzoccoRequestType::OBSERVE_STATE;
  }
  const bool connecting = pending.request.type == LaMarzoccoRequestType::CONNECT;
  const bool success = connecting ? executeConnect(pending)
                                  : executeObservation(pending);
  if (!success) scheduleAutomatic(millis(), connecting);
  if (connecting) releaseWorkBuffer();
}

bool OpenBrewByWeightLaMarzoccoService::temperatureRequestCurrent(
    const LaMarzoccoRequest &request,
    uint32_t machineConfigGeneration) const {
  TaskLockGuard lock(mux_);
  return desiredTemperature_.present &&
         desiredTemperature_.machineConfigGeneration ==
             machineConfigGeneration &&
         sameCommandRequest(desiredTemperature_.request, request) &&
         configGeneration_ == machineConfigGeneration &&
         config_.accountConfigured &&
         (config_.options & LA_MARZOCCO_APPLY_TEMPERATURE) != 0;
}

bool OpenBrewByWeightLaMarzoccoService::powerRequestCurrent(
    const LaMarzoccoRequest &request,
    uint32_t machineConfigGeneration) const {
  TaskLockGuard lock(mux_);
  return !backflush_.status().busy() && desiredPower_.present &&
         desiredPower_.shotGeneration == shotGeneration_.load(std::memory_order_acquire) &&
         desiredPower_.machineConfigGeneration == machineConfigGeneration &&
         sameCommandRequest(desiredPower_.request, request) &&
         configGeneration_ == machineConfigGeneration &&
         config_.accountConfigured &&
         (config_.options & powerOptionBit(request.type)) != 0;
}

bool OpenBrewByWeightLaMarzoccoService::identityCurrent(
    uint32_t identityGeneration) const {
  TaskLockGuard lock(mux_);
  return identityGeneration == identityGeneration_;
}

bool OpenBrewByWeightLaMarzoccoService::observationCurrent(
    uint32_t identityGeneration) const {
  TaskLockGuard lock(mux_);
  return identityGeneration == identityGeneration_ &&
         config_.accountConfigured &&
         (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0;
}

void OpenBrewByWeightLaMarzoccoService::deferTemperature(
    const LaMarzoccoRequest &request, LaMarzoccoError error, uint32_t delayMs,
    bool retryable, bool exhausted) {
  TaskLockGuard lock(mux_);
  if (!desiredTemperature_.present ||
      !sameCommandRequest(desiredTemperature_.request, request)) {
    return;
  }
  if (exhausted && ++desiredTemperature_.cycles >= la_marzocco_timing::kMaxCommandCycles)
    retryable = false;
  published_.temperatureCommandAccepted =
      desiredTemperature_.commandAccepted;
  published_.temperatureRetryable = retryable;
  if (retryable) {
    desiredTemperature_.retryAtMs = millis() + delayMs;
  } else {
    desiredTemperature_ = {};
  }
  published_.temperatureState = error == LaMarzoccoError::CANCELED
                                    ? LaMarzoccoTemperatureState::CANCELED
                                    : LaMarzoccoTemperatureState::FAILED;
  published_.temperatureError = error;
  const uint16_t httpStatus = work_ != nullptr ? work_->httpStatus : 0;
  const int32_t transportStatus =
      work_ != nullptr ? work_->transportStatus : 0;
  published_.temperatureTransportStatus = transportStatus;
  published_.temperatureHttpStatus = httpStatus;
  lock.unlock();  // logging takes its own locks; never nest them in mux_
  if (error != LaMarzoccoError::CANCELED) {
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "La Marzocco temperature request failed preset=%u target=%u.%u error=%s http=%u raw=%ld",
                         static_cast<unsigned>(request.presetId),
                         static_cast<unsigned>(request.targetDeciC / 10U),
                         static_cast<unsigned>(request.targetDeciC % 10U),
                         laMarzoccoErrorName(error),
                         static_cast<unsigned>(httpStatus),
                         static_cast<long>(transportStatus));
  }
}

void OpenBrewByWeightLaMarzoccoService::deferPower(
    const LaMarzoccoRequest &request, LaMarzoccoError error, uint32_t delayMs,
    bool retryable, bool exhausted) {
  TaskLockGuard lock(mux_);
  if (!desiredPower_.present ||
      !sameCommandRequest(desiredPower_.request, request)) {
    return;
  }
  if (exhausted && ++desiredPower_.cycles >= la_marzocco_timing::kMaxCommandCycles)
    retryable = false;
  if (retryable) {
    desiredPower_.retryAtMs = millis() + delayMs;
  } else {
    desiredPower_ = {};
  }
  const uint16_t httpStatus = work_ != nullptr ? work_->httpStatus : 0;
  const int32_t transportStatus =
      work_ != nullptr ? work_->transportStatus : 0;
  lock.unlock();  // logging takes its own locks; never nest them in mux_
  serialTraceCategoryf(error == LaMarzoccoError::CANCELED ? LogLevel::INFO
                                                          : LogLevel::WARNING,
                       DebugCategory::NETWORK,
                       "La Marzocco scale power request %s type=%s error=%s http=%u raw=%ld",
                       retryable ? "deferred" : "dropped",
                       request.type == LaMarzoccoRequestType::SET_POWER_ON
                           ? "power_on"
                           : "standby",
                       laMarzoccoErrorName(error),
                       static_cast<unsigned>(httpStatus),
                       static_cast<long>(transportStatus));
}

bool OpenBrewByWeightLaMarzoccoService::executeTemperatureApplication(
    const LaMarzoccoRequest &request,
    uint32_t machineConfigGeneration) {
  LaMarzoccoPersistedSettings settings;
  bool commandAccepted = false;
  // What is sent and confirmed: the preset's target inside the range the
  // machine reported; 0 until that range is known.
  uint16_t target = 0;
  {
    TaskLockGuard lock(mux_);
    if (!desiredTemperature_.present ||
        desiredTemperature_.machineConfigGeneration !=
            machineConfigGeneration ||
        !sameCommandRequest(desiredTemperature_.request, request) ||
        configGeneration_ != machineConfigGeneration) {
      return true;
    }
    target = laMarzoccoSendableBrewTarget(published_, request.targetDeciC);
    const LaMarzoccoStatus current = statusLocked();
    const bool unchanged = target != 0 && current.connectionFreshness &&
        current.quality == LaMarzoccoObservationQuality::CURRENT &&
        observationFence_.temperatureTrusted && published_.targetValid &&
        published_.targetDeciC == target;
    published_.requestId = request.requestId;
    published_.requestedTargetDeciC = request.targetDeciC;
    published_.temperatureState = unchanged ? LaMarzoccoTemperatureState::CONFIRMED
                                            : LaMarzoccoTemperatureState::RUNNING;
    published_.temperatureError = LaMarzoccoError::NONE;
    published_.temperatureRetryable = !unchanged;
    if (unchanged) {
      // The subscription would have pushed any change: the machine already
      // holds this target, so no cloud command is needed.
      desiredTemperature_ = {};
      published_.appliedTargetDeciC = target;
      published_.temperatureTransportStatus = 0;
      published_.temperatureHttpStatus = 0;
      published_.temperatureCommandAccepted = false;
      lock.unlock();  // logging takes its own locks; never nest them in mux_
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                           "La Marzocco temperature %u.%u already set, command skipped",
                           static_cast<unsigned>(target / 10U),
                           static_cast<unsigned>(target % 10U));
      return true;
    }
    settings = config_;
    commandAccepted = desiredTemperature_.commandAccepted;
    published_.temperatureCommandAccepted = commandAccepted;
  }
  if (!ensureWorkBuffer()) {
    wipeLaMarzoccoSettings(settings);
    deferTemperature(request, LaMarzoccoError::HTTP_ERROR,
                     la_marzocco_timing::kExhaustedCooldownMs);
    return false;
  }

  bool commandAttempted = false;
  int32_t commandTransportStatus = 0;
  uint16_t commandHttpStatus = 0;
  for (size_t attempt = 0; attempt < la_marzocco_timing::kMaxAttempts; ++attempt) {
    LaMarzoccoError gateError = LaMarzoccoError::NONE;
    if (!temperatureRequestCurrent(request, machineConfigGeneration)) {
      wipeLaMarzoccoSettings(settings);
      return true;
    }
    if (!temperatureEligible(gateError)) {
      wipeLaMarzoccoSettings(settings);
      deferTemperature(request, gateError,
                       gateError == LaMarzoccoError::CANCELED
                           ? la_marzocco_timing::kGateRetryMs
                           : la_marzocco_timing::kExhaustedCooldownMs);
      return false;
    }

    LaMarzoccoStatus verification;
    {
      TaskLockGuard lock(mux_);
      verification = published_;
    }
    // On the companion route the companion signs and sends the command and
    // its state confirms it: no session, write or read from here.
    const bool companion = companionRoute_.load();
    bool success = false;
    if ((companion || ensureSession(settings, false)) &&
        temperatureRequestCurrent(request, machineConfigGeneration)) {
      // No frame has brought the machine's range yet: read it before sending.
      // The companion route has no read of its own and waits for its state.
      // A failed read retries like a failed command; a machine that reports
      // no range ends the cycle with nothing sent.
      const bool unread = target == 0 && !companion && !readDashboard(settings, verification);
      if (target == 0 && !unread)
        target = laMarzoccoSendableBrewTarget(verification, request.targetDeciC);
      if (target == 0 && !unread) break;
      if (target != 0 && !commandAccepted) {
        if (target != request.targetDeciC)
          serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                               "La Marzocco temperature preset=%u asks %u.%u outside the "
                               "machine's range: sending %u.%u",
                               static_cast<unsigned>(request.presetId),
                               static_cast<unsigned>(request.targetDeciC / 10U),
                               static_cast<unsigned>(request.targetDeciC % 10U),
                               static_cast<unsigned>(target / 10U),
                               static_cast<unsigned>(target % 10U));
        commandAttempted = true;
        commandAccepted = companion
            ? companionCommand(LaMarzoccoCompanionCommandOp::TEMPERATURE, target)
            : writeTemperature(settings, target);
        commandTransportStatus = work_->transportStatus;
        commandHttpStatus = work_->httpStatus;
        if (commandAccepted) {
          TaskLockGuard lock(mux_);
          if (desiredTemperature_.present &&
              sameCommandRequest(desiredTemperature_.request, request)) {
            desiredTemperature_.commandAccepted = true;
            published_.temperatureCommandAccepted = true;
          }
        }
      }
      success = target != 0 && commandAccepted &&
                temperatureRequestCurrent(request, machineConfigGeneration) &&
                (pushConfirms(target, LaMarzoccoPowerState::UNKNOWN) ||
                 (!companion && readDashboard(settings, verification) &&
                  verification.targetValid &&
                  verification.targetDeciC == target));
    }
    if (success) {
      wipeLaMarzoccoSettings(settings);
      TaskLockGuard lock(mux_);
      if (desiredTemperature_.present &&
          sameCommandRequest(desiredTemperature_.request, request)) {
        desiredTemperature_ = {};
        published_.appliedTargetDeciC = target;
        published_.temperatureState = LaMarzoccoTemperatureState::CONFIRMED;
        published_.temperatureError = LaMarzoccoError::NONE;
        published_.temperatureTransportStatus = work_->transportStatus;
        published_.temperatureHttpStatus = work_->httpStatus;
        published_.temperatureCommandAccepted = true;
        published_.temperatureRetryable = false;
      }
      return true;
    }

    if (!temperatureRequestCurrent(request, machineConfigGeneration)) {
      wipeLaMarzoccoSettings(settings);
      return true;
    }
    if (!temperatureEligible(gateError)) {
      wipeLaMarzoccoSettings(settings);
      deferTemperature(request, gateError,
                       la_marzocco_timing::kGateRetryMs);
      return false;
    }
    discardAccessToken();
    if (!commandAccepted && commandAttempted &&
        commandTransportStatus == ESP_OK && commandHttpStatus != 0 &&
        !laMarzoccoHttpRetryable(commandHttpStatus)) {
      break;
    }
    if (attempt + 1U >= la_marzocco_timing::kMaxAttempts) break;
    waitRetry(la_marzocco_timing::kRetryDelaysMs[attempt]);
  }

  wipeLaMarzoccoSettings(settings);
  if (target == 0) {
    // Nothing was sent: the machine has not reported the range it accepts.
    // The request waits a bounded number of cycles for that report.
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "La Marzocco temperature not sent: the machine has reported no "
                         "brew range");
    deferTemperature(request, LaMarzoccoError::REJECTED,
                     la_marzocco_timing::kExhaustedCooldownMs, true, true);
    return false;
  }
  const uint16_t failureHttpStatus =
      commandAttempted ? commandHttpStatus : work_->httpStatus;
  const LaMarzoccoError error = commandFailure(
      commandAccepted, failureHttpStatus, work_->transportFailure, work_->policyRequired);
  deferTemperature(request, error, la_marzocco_timing::kExhaustedCooldownMs,
                   laMarzoccoTemperatureCycleRetryable(error), true);
  return false;
}

bool OpenBrewByWeightLaMarzoccoService::executePowerApplication(
    const LaMarzoccoRequest &request,
    uint32_t machineConfigGeneration) {
  LaMarzoccoPersistedSettings settings;
  bool commandAccepted = false;
  const bool standby = request.type == LaMarzoccoRequestType::SET_STANDBY;
  const LaMarzoccoPowerState wantedState =
      standby ? LaMarzoccoPowerState::OFF : LaMarzoccoPowerState::ON;
  {
    TaskLockGuard lock(mux_);
    if (!desiredPower_.present ||
        desiredPower_.machineConfigGeneration != machineConfigGeneration ||
        !sameCommandRequest(desiredPower_.request, request) ||
        configGeneration_ != machineConfigGeneration) {
      return true;
    }
    // Measured on a live subscription (an optimistic overlay is not CURRENT):
    // the machine is already in the wanted mode, so no cloud command is needed.
    // An offline machine drops the command: powering it on whenever it later
    // reconnects would be a surprise, unlike a deferred temperature target.
    const LaMarzoccoStatus current = statusLocked();
    const bool offline = observationFence_.offline;
    if (offline || (current.connectionFreshness &&
        current.quality == LaMarzoccoObservationQuality::CURRENT &&
        observationFence_.powerTrusted && current.powerState == wantedState)) {
      desiredPower_ = {};
      lock.unlock();  // logging takes its own locks; never nest them in mux_
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                           offline ? "La Marzocco offline, power command dropped"
                           : standby ? "La Marzocco already in StandBy, command skipped"
                                     : "La Marzocco already in BrewingMode, command skipped");
      return !offline;
    }
    settings = config_;
    commandAccepted = desiredPower_.commandAccepted;
  }
  if (!ensureWorkBuffer()) {
    wipeLaMarzoccoSettings(settings);
    deferPower(request, LaMarzoccoError::HTTP_ERROR,
                  la_marzocco_timing::kExhaustedCooldownMs);
    return false;
  }

  bool commandAttempted = false;
  int32_t commandTransportStatus = 0;
  uint16_t commandHttpStatus = 0;
  for (size_t attempt = 0; attempt < la_marzocco_timing::kMaxAttempts; ++attempt) {
    if (!powerRequestCurrent(request, machineConfigGeneration)) {
      wipeLaMarzoccoSettings(settings);
      return true;
    }
    // A shot or rinse (closed relay) cancels the command outright; the
    // machine power state is never changed around an active cycle.
    if (shotActive_.load(std::memory_order_acquire) || outboundShotActive.load(std::memory_order_acquire)) {
      wipeLaMarzoccoSettings(settings);
      deferPower(request, LaMarzoccoError::CANCELED, 0, false);
      return false;
    }
    LaMarzoccoError gateError = LaMarzoccoError::NONE;
    if (scaleConnecting_.load(std::memory_order_acquire)) {
      gateError = LaMarzoccoError::CANCELED;
    } else {
      (void)networkEligible(gateError);
    }
    if (gateError != LaMarzoccoError::NONE) {
      wipeLaMarzoccoSettings(settings);
      deferPower(request, gateError,
                    gateError == LaMarzoccoError::CANCELED
                        ? la_marzocco_timing::kGateRetryMs
                        : la_marzocco_timing::kExhaustedCooldownMs);
      return false;
    }

    LaMarzoccoStatus verification;
    {
      TaskLockGuard lock(mux_);
      verification = published_;
    }
    const bool companion = companionRoute_.load();
    bool success = false;
    if ((companion || ensureSession(settings, false)) &&
        powerRequestCurrent(request, machineConfigGeneration)) {
      if (!commandAccepted) {
        commandAttempted = true;
        commandAccepted = companion
            ? companionCommand(standby ? LaMarzoccoCompanionCommandOp::STANDBY
                                       : LaMarzoccoCompanionCommandOp::POWER_ON, 0)
            : standby ? writeStandby(settings) : writePowerOn(settings);
        commandTransportStatus = work_->transportStatus;
        commandHttpStatus = work_->httpStatus;
        if (commandAccepted) {
          TaskLockGuard lock(mux_);
          if (desiredPower_.present &&
              desiredPower_.shotGeneration == shotGeneration_.load(std::memory_order_acquire) &&
              sameCommandRequest(desiredPower_.request, request)) {
            desiredPower_.commandAccepted = true;
            // The cloud accepted the mode change: assert the optimistic
            // overlay and delay reads until the cloud can have converged.
            const bool observing = config_.accountConfigured &&
                (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0;
            const bool overlaid =
                standby
                    ? powerState_.noteStandbyCommandAccepted(published_,
                                                             observing,
                                                             millis())
                    : powerState_.notePowerOnCommandAccepted(published_,
                                                             observing,
                                                             millis());
            if (overlaid) {
              observationSchedule_.armPostEvent(millis());
              // Companion reads ordered after this command carry its intent.
              if (companion) recordCompanionIntentLocked(companion_.lastCommand);
            }
          }
        }
      }
      success = commandAccepted &&
                powerRequestCurrent(request, machineConfigGeneration) &&
                (pushConfirms(0, wantedState) ||
                 (!companion && readDashboard(settings, verification) &&
                  verification.powerState == wantedState));
    }
    if (success) {
      wipeLaMarzoccoSettings(settings);
      TaskLockGuard lock(mux_);
      if (desiredPower_.present &&
          sameCommandRequest(desiredPower_.request, request)) {
        desiredPower_ = {};
      }
      lock.unlock();  // logging takes its own locks; never nest them in mux_
      serialTraceCategoryf(
          LogLevel::INFO, DebugCategory::NETWORK,
          standby
              ? "La Marzocco scale shutdown confirmed: machine is in StandBy"
              : "La Marzocco scale power-on confirmed: machine is in BrewingMode");
      return true;
    }

    if (!powerRequestCurrent(request, machineConfigGeneration)) {
      wipeLaMarzoccoSettings(settings);
      return true;
    }
    discardAccessToken();
    if (!commandAccepted && commandAttempted &&
        commandTransportStatus == ESP_OK && commandHttpStatus != 0 &&
        !laMarzoccoHttpRetryable(commandHttpStatus)) {
      break;
    }
    if (attempt + 1U >= la_marzocco_timing::kMaxAttempts) break;
    waitRetry(la_marzocco_timing::kRetryDelaysMs[attempt]);
  }

  wipeLaMarzoccoSettings(settings);
  const uint16_t failureHttpStatus =
      commandAttempted ? commandHttpStatus : work_->httpStatus;
  const LaMarzoccoError error = commandFailure(
      commandAccepted, failureHttpStatus, work_->transportFailure, work_->policyRequired);
  deferPower(request, error, la_marzocco_timing::kExhaustedCooldownMs,
                laMarzoccoTemperatureCycleRetryable(error), true);
  return false;
}

bool OpenBrewByWeightLaMarzoccoService::executeConnect(PendingRequest &pending) {
  LaMarzoccoStatus status;
  {
    TaskLockGuard lock(mux_);
    status = published_;
  }
  status.requestId = pending.request.requestId;
  status.identityGeneration = pending.identityGeneration;
  status.phase = LaMarzoccoPhase::AUTHENTICATING;
  status.error = LaMarzoccoError::NONE;
  publish(status);
  if (!identityCurrent(pending.identityGeneration)) return true;
  if (!generateInstallationKey(pending.credentials)) {
    fail(status, LaMarzoccoError::HTTP_ERROR);
    return false;
  }
  if (!ensureSession(pending.credentials, true)) {
    LaMarzoccoError error = LaMarzoccoError::NONE;
    if (networkEligible(error)) {
      error = classifyFailure();
    }
    fail(status, error);
    return false;
  }
  if (!identityCurrent(pending.identityGeneration)) return true;
  status.phase = LaMarzoccoPhase::LISTING;
  publish(status);
  LaMarzoccoDiscoverySnapshot found;
  found.requestId = pending.request.requestId;
  if (!listMachines(pending.credentials, found)) {
    LaMarzoccoError error = LaMarzoccoError::NONE;
    if (networkEligible(error)) {
      error = classifyFailure();
    }
    fail(status, error);
    return false;
  }
  if (!identityCurrent(pending.identityGeneration)) return true;
  if (found.count == 0) {
    fail(status, LaMarzoccoError::NO_MACHINES);
    return false;
  }
  {
    TaskLockGuard lock(mux_);
    if (pending.identityGeneration != identityGeneration_) return true;
    candidate_ = pending.credentials;
    candidate_.options = config_.options;
    discovery_ = found;
    discovery_.accountMachines = found.count;
    ++discovery_.generation;
  }
  status.phase = LaMarzoccoPhase::CONFIRMED;
  status.error = LaMarzoccoError::NONE;
  status.quality = LaMarzoccoObservationQuality::UNCONFIGURED;
  publish(status);
  return true;
}

// Reuses the saved installation key and live session: unlike a connect it
// registers nothing and publishes no status, so observation keeps running.
void OpenBrewByWeightLaMarzoccoService::executeRelist(PendingRequest &pending) {
  LaMarzoccoDiscoverySnapshot found;
  LaMarzoccoError error = LaMarzoccoError::NONE;
  // Same 3/6/9 s backoff as other cloud calls, so one dropped connection does
  // not leave the Change list empty.
  for (size_t attempt = 0; attempt < la_marzocco_timing::kMaxAttempts; ++attempt) {
    found = {};
    found.requestId = pending.request.requestId;
    error = LaMarzoccoError::NONE;
    if (!networkEligible(error) ||
        (ensureWorkBuffer() && ensureSession(pending.credentials, false) &&
         listMachines(pending.credentials, found))) {
      break;
    }
    error = classifyFailure();
    discardAccessToken();
    const uint16_t http = work_ == nullptr ? 0 : work_->httpStatus;
    if (attempt + 1U >= la_marzocco_timing::kMaxAttempts || http == 401 ||
        !laMarzoccoHttpRetryable(http) ||
        abortRequested_.load(std::memory_order_acquire) ||
        !identityCurrent(pending.identityGeneration)) {
      break;
    }
    waitRetry(la_marzocco_timing::kRetryDelaysMs[attempt]);
  }
  if (error == LaMarzoccoError::NONE && found.count == 0) {
    error = LaMarzoccoError::NO_MACHINES;
  }
  TaskLockGuard lock(mux_);
  if (pending.identityGeneration != identityGeneration_) return;
  if (error == LaMarzoccoError::NONE) {
    candidate_ = pending.credentials;
    discovery_ = found;
    discovery_.accountMachines = found.count;
    ++discovery_.generation;
  }
  discovery_.listing = false;
  discovery_.error = error;
}

bool OpenBrewByWeightLaMarzoccoService::executeObservation(const PendingRequest &pending) {
  if (!observationCurrent(pending.identityGeneration)) return true;
  LaMarzoccoPersistedSettings settings;
  LaMarzoccoStatus status;
  {
    TaskLockGuard lock(mux_);
    settings = config_;
    status = published_;
  }
  status.requestId = pending.request.requestId;
  status.identityGeneration = pending.identityGeneration;
  status.phase = LaMarzoccoPhase::RUNNING;
  status.error = LaMarzoccoError::NONE;
  publish(status);
  bool success = false;
  bool sessionRenewed = false;
  // Session renewal must not consume a WS initialization or explicit read.
  const bool dashboardRequired = status.sampleAtMs == 0 ||
      settings.connectionType == static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET);
  for (size_t attempt = 0; attempt < la_marzocco_timing::kMaxAttempts; ++attempt) {
    if (pending.initialSnapshot) {
      TaskLockGuard lock(mux_);
      if (pending.snapshotStamp.epoch != observationFence_.epoch ||
          pending.snapshotStamp.connectionRevision != observationFence_.connectionRevision ||
          !websocketStatus_.subscribed || observationFence_.offline) {
        if (published_.requestId == pending.request.requestId)
          published_.phase = LaMarzoccoPhase::IDLE;
        wipeLaMarzoccoSettings(settings);
        return true;
      }
    }
    if (!observationCurrent(pending.identityGeneration)) {
      wipeLaMarzoccoSettings(settings);
      return true;
    }
    if (shotTransportPaused()) {
      wipeLaMarzoccoSettings(settings);
      deferObservation(pending, status, LaMarzoccoError::CANCELED);
      return true;
    }
    LaMarzoccoError gateError = LaMarzoccoError::NONE;
    if (!networkEligible(gateError)) {
      wipeLaMarzoccoSettings(settings);
      deferObservation(pending, status, gateError);
      return true;
    }
    // On the companion route a refresh asks the companion for one read.
    success = companionRoute_.load()
        ? companionCommand(LaMarzoccoCompanionCommandOp::REFRESH, 0)
        : ensureSession(settings, false, &sessionRenewed) &&
              ((sessionRenewed && !dashboardRequired) ||
               readDashboard(settings, status,
                   pending.initialSnapshot ? &pending.snapshotStamp : nullptr));
    if (success) break;
    if (!observationCurrent(pending.identityGeneration)) {
      wipeLaMarzoccoSettings(settings);
      return true;
    }
    if (abortRequested_.load(std::memory_order_acquire)) {
      wipeLaMarzoccoSettings(settings);
      deferObservation(pending, status, LaMarzoccoError::CANCELED);
      return true;
    }
    if (shotTransportPaused()) continue;
    if (!networkEligible(gateError)) {
      wipeLaMarzoccoSettings(settings);
      deferObservation(pending, status, gateError);
      return true;
    }
    // Only accepting the policy unblocks the account: retrying cannot.
    if (classifyFailure() == LaMarzoccoError::POLICY_REQUIRED) break;
    discardAccessToken();
    if (attempt + 1U >= la_marzocco_timing::kMaxAttempts) break;
    status.phase = LaMarzoccoPhase::BACKOFF;
    publish(status);
    waitRetry(la_marzocco_timing::kRetryDelaysMs[attempt]);
    status.phase = LaMarzoccoPhase::RUNNING;
    publish(status);
  }
  wipeLaMarzoccoSettings(settings);
  if (!observationCurrent(pending.identityGeneration)) return true;
  if (!success) {
    LaMarzoccoError error = LaMarzoccoError::NONE;
    if (networkEligible(error)) {
      error = classifyFailure();
    }
    fail(status, error);
    return false;
  }
  if (sessionRenewed && !dashboardRequired) {
    status.phase = LaMarzoccoPhase::IDLE;
    status.error = LaMarzoccoError::NONE;
    publish(status);
    scheduleAutomatic(millis(), false);
    return true;
  }
  status.phase = LaMarzoccoPhase::CONFIRMED;
  status.error = LaMarzoccoError::NONE;
  status.quality = status.observedMode == LaMarzoccoObservedMode::UNSUPPORTED
                       ? LaMarzoccoObservationQuality::UNSUPPORTED
                       : LaMarzoccoObservationQuality::CURRENT;
  publish(status);
  scheduleAutomatic(status.sampleAtMs, false);
  return true;
}

void OpenBrewByWeightLaMarzoccoService::publish(const LaMarzoccoStatus &status) {
  TaskLockGuard lock(mux_);
  if (status.identityGeneration != identityGeneration_) return;
  published_.requestId = status.requestId;
  published_.phase = status.phase;
  published_.error = status.error;
  published_.httpStatus = status.httpStatus;
  published_.transportStatus = status.transportStatus;
  published_.transportFailure = status.transportFailure;
  published_.accountConfigured = config_.accountConfigured;
}

void OpenBrewByWeightLaMarzoccoService::deferObservation(
    const PendingRequest &pending, LaMarzoccoStatus status,
    LaMarzoccoError reason) {
  TaskLockGuard lock(mux_);
  if (pending.identityGeneration != identityGeneration_ ||
      !config_.accountConfigured ||
      (config_.options & LA_MARZOCCO_OBSERVE_STATE) == 0) {
    return;
  }
  if (!pending_.present) pending_ = pending;
  pending_.present = true;
  if (reason == LaMarzoccoError::CANCELED)
    powerState_.hold(published_, true, millis());
  status.phase = reason == LaMarzoccoError::CANCELED
                     ? LaMarzoccoPhase::PAUSED
                     : LaMarzoccoPhase::QUEUED;
  status.error = LaMarzoccoError::NONE;
  status.shotPaused = reason == LaMarzoccoError::CANCELED;
  published_.phase = status.phase;
  published_.error = status.error;
  published_.shotPaused = status.shotPaused;
  published_.accountConfigured = config_.accountConfigured;
}

LaMarzoccoError OpenBrewByWeightLaMarzoccoService::classifyFailure() const {
  if (companionRoute_.load() && companionPolicy_.load()) return LaMarzoccoError::POLICY_REQUIRED;
  if (work_ != nullptr) {
    if (work_->policyRequired) return LaMarzoccoError::POLICY_REQUIRED;
    if (work_->httpStatus == 401) return LaMarzoccoError::INVALID_AUTH;
    if (work_->transportFailure) return LaMarzoccoError::TRANSPORT;
  }
  return LaMarzoccoError::HTTP_ERROR;
}

void OpenBrewByWeightLaMarzoccoService::fail(LaMarzoccoStatus &status,
                                   LaMarzoccoError error) {
  const LaMarzoccoPhase failedStage = status.phase;
  status.phase = LaMarzoccoPhase::FAILED;
  status.error = error;
  if (work_ != nullptr) {
    status.httpStatus = work_->httpStatus;
    status.transportStatus = work_->transportStatus;
  }
  publish(status);
  {
    // API mode has no socket to stop: the exhausted cycle holds the last
    // reading as an assumption (kStaleRetentionMs, then unknown) instead of
    // publishing unknown at once. The error itself stays in the status.
    TaskLockGuard lock(mux_);
    if (status.identityGeneration == identityGeneration_ &&
        config_.connectionType == static_cast<uint8_t>(LaMarzoccoConnectionType::API)) {
      powerState_.hold(published_,
                       config_.accountConfigured &&
                           (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0,
                       millis());
    }
  }
  serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                       "La Marzocco cloud request failed stage=%s error=%s http=%u raw=%ld",
                       laMarzoccoPhaseName(failedStage),
                       laMarzoccoErrorName(error),
                       static_cast<unsigned>(status.httpStatus),
                       static_cast<long>(status.transportStatus));
}

void OpenBrewByWeightLaMarzoccoService::scheduleAutomatic(uint32_t now, bool failed) {
  TaskLockGuard lock(mux_);
  // Rejected credentials latch like the WebSocket lane: retrying would only
  // repeat sign-ins until the vendor locks the account. An account edit or a
  // manual refresh re-arms observation. A pending policy keeps the slow
  // failure cadence instead, one read per check, so an acceptance made in the
  // La Marzocco app is noticed without any action here.
  if (config_.connectionType == static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET) ||
      published_.error == LaMarzoccoError::INVALID_AUTH) {
    observationSchedule_.suspendPeriodic();
  } else if (failed) {
    observationSchedule_.dueNow(now + la_marzocco_timing::kExhaustedCooldownMs);
  } else {
    observationSchedule_.scheduleNext(now);
  }
}

void OpenBrewByWeightLaMarzoccoService::waitRetry(uint32_t delayMs) {
  const uint32_t due = millis() + delayMs;
  while (!tickReached(millis(), due)) {
    LaMarzoccoError error;
    if (!networkEligible(error) || abortRequested_.load(std::memory_order_acquire)) {
      if (!websocketAdmitted()) stopWebSocket();
      return;
    }
    const uint32_t now = millis();
    if (!tickReached(now, due))
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::min(uint32_t(250), due - now)));
  }
}

bool OpenBrewByWeightLaMarzoccoService::generateInstallationKey(
    LaMarzoccoPersistedSettings &settings) {
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  setKeyAttributes(attributes);
  mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
  const psa_status_t generated = psa_generate_key(&attributes, &key);
  psa_reset_key_attributes(&attributes);
  if (generated != PSA_SUCCESS) return false;
  size_t length = 0;
  const psa_status_t exported =
      psa_export_key(key, settings.installationPrivateKey,
                     sizeof(settings.installationPrivateKey), &length);
  (void)psa_destroy_key(key);
  return exported == PSA_SUCCESS &&
         length == sizeof(settings.installationPrivateKey);
}

// A 401/403 answer rejects the token itself, not the request: discard it so
// the next attempt re-acquires a session instead of reusing it by age. The
// WebSocket lane reaches the same outcome through its handshake auth failure.
// A pending policy's 403 keeps the token: the session is valid.
void OpenBrewByWeightLaMarzoccoService::discardAccessToken() {
  if (work_ == nullptr || work_->policyRequired ||
      (work_->httpStatus != 401 && work_->httpStatus != 403))
    return;
  const uint32_t ageMs = work_->accessTokenIssuedAtMs != 0
                             ? millis() - work_->accessTokenIssuedAtMs : 0;
  tokenAvailable_.store(false, std::memory_order_release);
  secureWipe(work_->accessToken, sizeof(work_->accessToken));
  work_->accessTokenIssuedAtMs = 0;
  serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                       "La Marzocco access token discarded http=%u age=%lu ms",
                       static_cast<unsigned>(work_->httpStatus),
                       static_cast<unsigned long>(ageMs));
}

bool OpenBrewByWeightLaMarzoccoService::ensureSession(
    const LaMarzoccoPersistedSettings &settings, bool registerKey, bool *renewed) {
  if (renewed != nullptr) *renewed = false;
  if (work_ == nullptr) return false;
  if (registerKey) {
    clearSession();
    return registerInstallation(settings) && signIn(settings);
  }
  const uint32_t age = millis() - work_->accessTokenIssuedAtMs;
  if (work_->accessToken[0] != '\0' &&
      !la_marzocco_timing::accessTokenRefreshDue(age)) return true;
  tokenAvailable_.store(false, std::memory_order_release);
  const bool ok = work_->refreshToken[0] != '\0'
                      ? refreshToken(settings) || signIn(settings)
                      : signIn(settings);
  if (ok && renewed != nullptr) *renewed = true;
  return ok;
}

bool OpenBrewByWeightLaMarzoccoService::registerInstallation(
    const LaMarzoccoPersistedSettings &settings) {
  if (!ensureIoBuffer()) return false;
  const bool ok =
      cloudRequest(settings, kRegisterUrl, HTTP_METHOD_POST, nullptr, false,
                   {"register_installation", "/auth/init"}, true);
  releaseIoBuffer();
  return ok;
}

bool OpenBrewByWeightLaMarzoccoService::signIn(
    const LaMarzoccoPersistedSettings &settings) {
  const uint32_t account = laMarzoccoAccountKey(settings);
  if (account == rejectedAccount_ &&
      !signInRetryAllowed_.exchange(false, std::memory_order_acq_rel)) {
    // Reads as the refusal it repeats, so both lanes keep their auth latch.
    work_->httpStatus = 401;
    work_->transportFailure = work_->policyRequired = false;
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "La Marzocco sign-in skipped: La Marzocco refused this account; "
                         "edit it or refresh to try again");
    return false;
  }
  if (!ensureIoBuffer()) return false;
  char username[2 * LA_MARZOCCO_USERNAME_CAPACITY];
  char password[2 * LA_MARZOCCO_PASSWORD_CAPACITY];
  if (!jsonEscape(settings.username, username, sizeof(username)) ||
      !jsonEscape(settings.password, password, sizeof(password))) {
    secureWipe(username, sizeof(username));
    secureWipe(password, sizeof(password));
    releaseIoBuffer();
    return false;
  }
  const int length = snprintf(io_->body, sizeof(io_->body),
                              "{\"username\":\"%s\",\"password\":\"%s\"}",
                              username, password);
  secureWipe(username, sizeof(username));
  secureWipe(password, sizeof(password));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(io_->body)) {
    releaseIoBuffer();
    return false;
  }
  if (!cloudRequest(settings, kSignInUrl, HTTP_METHOD_POST, io_->body, false,
                    {"sign_in", "/auth/signin"})) {
    releaseIoBuffer();
    if (work_->httpStatus == 401 && !work_->policyRequired) rememberSignIn(account, true);
    return false;
  }
  cJSON *root = parseResponse(io_->response);
  const char *access = root == nullptr ? nullptr : jsonText(root, "accessToken");
  const char *refresh = root == nullptr ? nullptr : jsonText(root, "refreshToken");
  const bool ok = access != nullptr && refresh != nullptr &&
                  secureCopy(work_->accessToken, sizeof(work_->accessToken),
                             access) &&
                  secureCopy(work_->refreshToken, sizeof(work_->refreshToken),
                             refresh);
  if (root != nullptr) cJSON_Delete(root);
  work_->responseUsed = 0;
  releaseIoBuffer(ok);
  if (!ok) {
    secureWipe(work_->accessToken, sizeof(work_->accessToken));
    secureWipe(work_->refreshToken, sizeof(work_->refreshToken));
    return false;
  }
  work_->accessTokenIssuedAtMs = millis();
  tokenIssuedAtMs_.store(work_->accessTokenIssuedAtMs, std::memory_order_release);
  tokenAvailable_.store(true, std::memory_order_release);
  rememberSignIn(account, false);
  return true;
}

// Cloud owner. A sign-in never runs during a shot (networkEligible and the
// WebSocket admission), so this small write stays out of the shot window.
void OpenBrewByWeightLaMarzoccoService::rememberSignIn(uint32_t account, bool rejected) {
  const uint32_t next = rejected ? account : 0;
  if (next == rejectedAccount_) return;
  bool saved = false;
  if (lockFlashIo()) {
    OpenBrewByWeightPreferences preferences(NvsSubsystem::SETTINGS);
    if (preferences.begin(SETTINGS_NAMESPACE, false)) {
      saved = preferences.putBytes(LA_MARZOCCO_REJECTED_KEY, &next, sizeof(next)) == sizeof(next);
      preferences.end();
    }
    unlockFlashIo();
  }
  rejectedAccount_ = next;
  if (!saved)
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "La Marzocco sign-in result not saved; a restart may sign in again");
}

bool OpenBrewByWeightLaMarzoccoService::refreshToken(
    const LaMarzoccoPersistedSettings &settings) {
  if (work_->refreshToken[0] == '\0') return false;
  if (!ensureIoBuffer()) return false;
  cJSON *body = cJSON_CreateObject();
  const bool bodyOk = body != nullptr &&
      cJSON_AddStringToObject(body, "username", settings.username) != nullptr &&
      cJSON_AddStringToObject(body, "refreshToken", work_->refreshToken) != nullptr &&
      cJSON_PrintPreallocated(body, io_->body, sizeof(io_->body), false);
  if (body != nullptr) cJSON_Delete(body);
  if (!bodyOk) {
    releaseIoBuffer();
    return false;
  }
  if (!cloudRequest(settings, kRefreshUrl, HTTP_METHOD_POST, io_->body, false,
                    {"refresh_token", "/auth/refreshtoken"})) {
    releaseIoBuffer();
    return false;
  }
  cJSON *root = parseResponse(io_->response);
  const char *access = root == nullptr ? nullptr : jsonText(root, "accessToken");
  const char *newRefresh =
      root == nullptr ? nullptr : jsonText(root, "refreshToken");
  const bool ok = access != nullptr && newRefresh != nullptr &&
                  secureCopy(work_->accessToken, sizeof(work_->accessToken),
                             access) &&
                  secureCopy(work_->refreshToken, sizeof(work_->refreshToken),
                             newRefresh);
  if (root != nullptr) cJSON_Delete(root);
  work_->responseUsed = 0;
  releaseIoBuffer(ok);
  if (ok) {
    work_->accessTokenIssuedAtMs = millis();
    tokenIssuedAtMs_.store(work_->accessTokenIssuedAtMs, std::memory_order_release);
    tokenAvailable_.store(true, std::memory_order_release);
  }
  return ok;
}

bool OpenBrewByWeightLaMarzoccoService::listMachines(
    const LaMarzoccoPersistedSettings &settings,
    LaMarzoccoDiscoverySnapshot &result) {
  if (!ensureIoBuffer()) return false;
  if (!cloudRequest(settings, kThingsUrl, HTTP_METHOD_GET, nullptr, true,
                    {"list_machines", "/things"})) {
    releaseIoBuffer();
    return false;
  }
  cJSON *root = parseResponse(io_->response);
  // The Web UI log names what the listing left out, so an account whose
  // machine does not appear can be diagnosed there; never the serial.
  const bool listed = decodeLaMarzoccoThings(
      root, OPEN_BREW_BY_WEIGHT_MACHINE_CLOUD_MODEL, result, [](const char *model) {
        if (model == nullptr)
          serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                               "La Marzocco account machine skipped: serial not usable");
        else
          serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                               "La Marzocco account machine skipped: model %.23s, "
                               "firmware built for %s",
                               model, OPEN_BREW_BY_WEIGHT_MACHINE_CLOUD_MODEL);
      });
  if (root != nullptr) cJSON_Delete(root);
  work_->responseUsed = 0;
  releaseIoBuffer(listed);
  return listed;
}

bool OpenBrewByWeightLaMarzoccoService::readDashboard(
    const LaMarzoccoPersistedSettings &settings, LaMarzoccoStatus &result,
    const LaMarzoccoObservationStamp *snapshot) {
  if (!ensureIoBuffer()) return false;
  LaMarzoccoObservation update;
  const uint32_t disconnectGeneration = disconnectGeneration_.load(std::memory_order_acquire);
  const uint32_t acquisitionGeneration = outboundAcquisitionGeneration.load(std::memory_order_acquire);
  {
    TaskLockGuard lock(mux_);
    update.stamp = snapshot ? *snapshot :
        observationFence_.stamp(identityGeneration_, powerState_.generation());
    update.stamp.intent = powerState_.generation();
    if (snapshot) update.source = LaMarzoccoObservationSource::HTTP_INITIAL;
  }
  char url[sizeof(kApiRoot) + LA_MARZOCCO_SERIAL_CAPACITY + 24];
  const int length = snprintf(url, sizeof(url), "%s/things/%s/dashboard",
                              kApiRoot, settings.selectedSerial);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(url) ||
      !cloudRequest(settings, url, HTTP_METHOD_GET, nullptr, true,
                    {"read_dashboard", "/things/{serial}/dashboard"})) {
    releaseIoBuffer();
    return false;
  }
  cJSON *root = parseResponse(io_->response);
  const bool decoded = decodeLaMarzoccoDashboard(root, update) &&
      (update.powerPresent || (update.connectedPresent && !update.connected));
  cJSON_Delete(root);
  // A transport-level success with no observation sample means the payload
  // was rejected; the excerpt names the offending shape on the serial log.
  if (!decoded)
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "La Marzocco dashboard decode rejected: %.96s",
                         io_->response);
  work_->responseUsed = 0;
  releaseIoBuffer(decoded);
  if (!decoded) return false;
  update.receivedAtMs = millis();
  TaskLockGuard lock(mux_);
  if (abortRequested_.load(std::memory_order_acquire) || outboundScaleInhibited() ||
      shotTransportPaused() || scaleConnecting_.load(std::memory_order_acquire) ||
      acquisitionGeneration != outboundAcquisitionGeneration.load(std::memory_order_acquire) ||
      disconnectGeneration != disconnectGeneration_.load(std::memory_order_acquire)) return false;
  if (!mergeReadLocked(update)) return false;
  result = published_;
  return snapshot != nullptr || !observationFence_.offline;
}

// Merges one dashboard read under mux_ with post-wake reconciliation. The
// direct HTTPS read and the companion's full and read frames share this body.
bool OpenBrewByWeightLaMarzoccoService::mergeReadLocked(const LaMarzoccoObservation &update) {
  const bool observing = config_.accountConfigured &&
      (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0;
  const uint32_t onlineRevision = observationFence_.onlineRevision;
  if (!observationFence_.merge(published_, powerState_, update, observing, true)) return false;
  mirrorPowerLocked();
  if (observationFence_.onlineRevision != onlineRevision) {
    websocketStatus_.machineConnectedKnown = true;
    websocketStatus_.machineConnected = !observationFence_.offline;
  }
  if (observationFence_.offline || published_.powerState == LaMarzoccoPowerState::OFF)
    backflush_.invalidate();
  return true;
}

bool OpenBrewByWeightLaMarzoccoService::writeTemperature(
    const LaMarzoccoPersistedSettings &settings, uint16_t targetDeciC) {
  if (!ensureIoBuffer()) return false;
  char url[sizeof(kApiRoot) + LA_MARZOCCO_SERIAL_CAPACITY + 80];
  const int urlLength = snprintf(
      url, sizeof(url),
      "%s/things/%s/command/"
      "CoffeeMachineSettingCoffeeBoilerTargetTemperature",
      kApiRoot, settings.selectedSerial);
  const int bodyLength = snprintf(
      io_->body, sizeof(io_->body),
      "{\"boilerIndex\":1,\"targetTemperature\":%u.%u}",
      static_cast<unsigned>(targetDeciC / 10U),
      static_cast<unsigned>(targetDeciC % 10U));
  work_->transportStatus = 0;
  work_->httpStatus = 0;
  const bool ok = urlLength > 0 &&
                  static_cast<size_t>(urlLength) < sizeof(url) &&
                  bodyLength > 0 &&
                  static_cast<size_t>(bodyLength) < sizeof(io_->body) &&
                  cloudRequest(settings, url, HTTP_METHOD_POST, io_->body, true,
                               {"set_boiler_temperature",
                                "/things/{serial}/command/"
                                "CoffeeMachineSettingCoffeeBoilerTargetTemperature"});
  const bool accepted =
      ok || (work_->transportStatus == ESP_OK && work_->httpStatus >= 200 &&
             work_->httpStatus < 300);
  releaseIoBuffer();
  return accepted;
}

// Standby is the La Marzocco app's name for the machine power-off state; the
// cloud command is the mode change used by pylamarzocco's set_power(False).
bool OpenBrewByWeightLaMarzoccoService::writeStandby(
    const LaMarzoccoPersistedSettings &settings) {
  if (!ensureIoBuffer()) return false;
  char url[sizeof(kApiRoot) + LA_MARZOCCO_SERIAL_CAPACITY + 80];
  const int urlLength = snprintf(
      url, sizeof(url), "%s/things/%s/command/CoffeeMachineChangeMode",
      kApiRoot, settings.selectedSerial);
  work_->transportStatus = 0;
  work_->httpStatus = 0;
  const bool ok = urlLength > 0 &&
                  static_cast<size_t>(urlLength) < sizeof(url) &&
                  cloudRequest(settings, url, HTTP_METHOD_POST,
                               "{\"mode\":\"StandBy\"}", true,
                               {"set_standby",
                                "/things/{serial}/command/CoffeeMachineChangeMode"});
  releaseIoBuffer();
  return ok;
}

// The power-on counterpart sends the BrewingMode the dashboard reports while
// the machine is on; the same command pylamarzocco's set_power(True) uses.
bool OpenBrewByWeightLaMarzoccoService::writePowerOn(
    const LaMarzoccoPersistedSettings &settings) {
  if (!ensureIoBuffer()) return false;
  char url[sizeof(kApiRoot) + LA_MARZOCCO_SERIAL_CAPACITY + 80];
  const int urlLength = snprintf(
      url, sizeof(url), "%s/things/%s/command/CoffeeMachineChangeMode",
      kApiRoot, settings.selectedSerial);
  work_->transportStatus = 0;
  work_->httpStatus = 0;
  const bool ok = urlLength > 0 &&
                  static_cast<size_t>(urlLength) < sizeof(url) &&
                  cloudRequest(settings, url, HTTP_METHOD_POST,
                               "{\"mode\":\"BrewingMode\"}", true,
                               {"set_power_on",
                                "/things/{serial}/command/CoffeeMachineChangeMode"});
  releaseIoBuffer();
  return ok;
}

// Accepts only the mandatory policy: the optional profiling and marketing
// consents La Marzocco asks alongside it are always sent as refused.
bool OpenBrewByWeightLaMarzoccoService::acceptPolicy(
    const LaMarzoccoPersistedSettings &settings) {
  if (!ensureIoBuffer()) return false;
  const bool ok = cloudRequest(settings, kPolicyUrl, HTTP_METHOD_PUT,
                               "{\"profilePolicy\":false,\"commercialPolicy\":false}",
                               true, {"accept_policy", "/users/policy"});
  releaseIoBuffer();
  return ok;
}

bool OpenBrewByWeightLaMarzoccoService::applySignedHeaders(
    const LaMarzoccoPersistedSettings &settings, char *headers, size_t capacity) {
  LaMarzoccoPublicIdentityCache &identity = work_->publicIdentity;
  if (!identity.ensure([&](char *id, char *publicB64) {
        uint8_t publicDer[91];
        const bool ok = installationId(settings, id) &&
                        publicKeyDer(settings, publicDer) &&
                        base64Encode(publicDer, sizeof(publicDer), publicB64,
                                     sizeof(identity.publicKeyBase64));
        secureWipe(publicDer, sizeof(publicDer));
        return ok;
      })) {
    return false;
  }
  uint8_t secret[32];
  if (!deriveInstallationSecret(identity.id, identity.publicKeyBase64, secret)) {
    secureWipe(secret, sizeof(secret));
    return false;
  }
  uint8_t nonceBytes[16];
  esp_fill_random(nonceBytes, sizeof(nonceBytes));
  char nonce[37];
  formatUuid(nonceBytes, nonce);
  secureWipe(nonceBytes, sizeof(nonceBytes));
  timeval current{};
  gettimeofday(&current, nullptr);
  const unsigned long long timestamp =
      static_cast<unsigned long long>(current.tv_sec) * 1000ULL +
      static_cast<unsigned long long>(current.tv_usec / 1000);
  char proofInput[96];
  const int proofInputLength = snprintf(proofInput, sizeof(proofInput),
                                        "%s.%s.%llu", identity.id, nonce,
                                        timestamp);
  char proof[48];
  char signatureInput[160];
  bool ok = proofInputLength > 0 &&
            static_cast<size_t>(proofInputLength) < sizeof(proofInput) &&
            requestProof(proofInput, secret, proof);
  const int signatureLength =
      ok ? snprintf(signatureInput, sizeof(signatureInput), "%s.%s", proofInput,
                    proof)
         : -1;
  char signature[104];
  ok = ok && signatureLength > 0 &&
       static_cast<size_t>(signatureLength) < sizeof(signatureInput) &&
       signRequest(settings, signatureInput, signature);
  char timestampText[24];
  snprintf(timestampText, sizeof(timestampText), "%llu", timestamp);
  if (ok && headers != nullptr) {
    const int length = snprintf(headers, capacity,
        "X-App-Installation-Id: %s\r\nX-Timestamp: %s\r\nX-Nonce: %s\r\n"
        "X-Request-Signature: %s\r\n", identity.id, timestampText, nonce, signature);
    ok = length > 0 && static_cast<size_t>(length) < capacity;
  } else if (ok) {
    ok = esp_http_client_set_header(work_->client, "X-App-Installation-Id",
                                    identity.id) == ESP_OK &&
         esp_http_client_set_header(work_->client, "X-Timestamp", timestampText) ==
             ESP_OK &&
         esp_http_client_set_header(work_->client, "X-Nonce", nonce) == ESP_OK &&
         esp_http_client_set_header(work_->client, "X-Request-Signature",
                                    signature) == ESP_OK;
  }
  secureWipe(secret, sizeof(secret));
  secureWipe(proofInput, sizeof(proofInput));
  secureWipe(proof, sizeof(proof));
  secureWipe(signatureInput, sizeof(signatureInput));
  secureWipe(signature, sizeof(signature));
  return ok;
}

bool OpenBrewByWeightLaMarzoccoService::cloudRequest(
    const LaMarzoccoPersistedSettings &settings, const char *url,
    esp_http_client_method_t method, const char *body, bool authenticated,
    CloudCall call, bool installationInit) {
  pendingCloudCall_ = {};
  pendingCloudCall_.api = call.purpose;
  pendingCloudCall_.method = method == HTTP_METHOD_POST ? "POST"
      : method == HTTP_METHOD_PUT ? "PUT" : "GET";
  pendingCloudCall_.startedAtMs = millis();
  const time_t now = time(nullptr);
  pendingCloudCall_.startedAtUtcSec =
      now >= 1700000000 ? static_cast<uint32_t>(now) : 0;
  if (work_ == nullptr || io_ == nullptr || url == nullptr) return false;
  pendingCloudCall_.clientReused = work_->client != nullptr;
  if (pendingCloudCall_.clientReused) {
    pendingCloudCall_.idleMs =
        pendingCloudCall_.startedAtMs - work_->lastRequestEndedAtMs;
    if (pendingCloudCall_.idleMs >= la_marzocco_timing::kHttpReuseIdleMs) {
      (void)esp_http_client_close(work_->client);
      pendingCloudCall_.clientReused = false;
    }
  }
  if (work_->client == nullptr) {
    esp_http_client_config_t config{};
    config.url = kApiRoot;
    config.timeout_ms = static_cast<int>(la_marzocco_timing::kHttpTimeoutMs);
    config.disable_auto_redirect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = httpEvent;
    config.user_data = this;
    // Above ALWAYSINTERNAL=2048: ordinary SDK malloc prefers PSRAM.
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.is_async = true;
    work_->client = esp_http_client_init(&config);
    if (work_->client == nullptr) return false;
    if (esp_http_client_set_header(work_->client, "Accept", "application/json") !=
            ESP_OK ||
        esp_http_client_set_header(work_->client, "User-Agent",
                                   "OpenBrewByWeight/1") != ESP_OK) {
      esp_http_client_cleanup(work_->client);
      work_->client = nullptr;
      return false;
    }
  }
  // The perform state machine sends the complete POST body before its response
  // callback runs, so those mutually exclusive phases share storage.
  const bool bodySharesResponse = body == io_->body;
  work_->responseUsed = 0;
  if (!bodySharesResponse) io_->response[0] = '\0';
  work_->responseOverflow = false;
  work_->httpStatus = 0;
  work_->transportStatus = 0;
  work_->transportFailure = false;
  work_->policyRequired = false;
  clearRequestState();
  RequestStateGuard requestState{*this};
  if (esp_http_client_set_url(work_->client, url) != ESP_OK ||
      esp_http_client_set_method(work_->client, method) != ESP_OK) {
    return false;
  }
  if (installationInit) {
    char id[37];
    char publicB64[128];
    uint8_t secret[32];
    uint8_t publicDer[91];
    uint8_t publicHash[32];
    char publicHashB64[48];
    char base[224];
    char proof[48];
    bool ok = installationId(settings, id) &&
              publicKeyDer(settings, publicDer) &&
              base64Encode(publicDer, sizeof(publicDer), publicB64,
                           sizeof(publicB64)) &&
              deriveInstallationSecret(id, publicB64, secret) &&
              sha256(publicDer, sizeof(publicDer), publicHash) &&
              base64Encode(publicHash, sizeof(publicHash), publicHashB64,
                           sizeof(publicHashB64));
    const int baseLength =
        ok ? snprintf(base, sizeof(base), "%s.%s", id, publicHashB64) : -1;
    ok = ok && baseLength > 0 && static_cast<size_t>(baseLength) < sizeof(base) &&
         requestProof(base, secret, proof);
    const int bodyLength =
        ok ? snprintf(io_->body, sizeof(io_->body), "{\"pk\":\"%s\"}",
                      publicB64)
           : -1;
    ok = ok && bodyLength > 0 &&
         static_cast<size_t>(bodyLength) < sizeof(io_->body) &&
         esp_http_client_set_header(work_->client, "X-App-Installation-Id", id) ==
             ESP_OK &&
         esp_http_client_set_header(work_->client, "X-Request-Proof", proof) ==
             ESP_OK;
    secureWipe(secret, sizeof(secret));
    secureWipe(publicDer, sizeof(publicDer));
    secureWipe(publicHash, sizeof(publicHash));
    secureWipe(publicHashB64, sizeof(publicHashB64));
    secureWipe(base, sizeof(base));
    secureWipe(proof, sizeof(proof));
    if (!ok) {
      secureWipe(io_->body, sizeof(io_->body));
      return false;
    }
    body = io_->body;
  } else if (!applySignedHeaders(settings)) {
    return false;
  }
  if (authenticated) {
    char *authorization = work_->authorization;
    const int length = snprintf(authorization, kTokenCapacity + 8,
                                "Bearer %s", work_->accessToken);
    const bool ok = length > 0 &&
                    static_cast<size_t>(length) < kTokenCapacity + 8 &&
                    esp_http_client_set_header(work_->client, "Authorization",
                                               authorization) == ESP_OK;
    secureWipe(authorization, kTokenCapacity + 8);
    if (!ok) return false;
  }
  if (body != nullptr &&
      (esp_http_client_set_header(work_->client, "Content-Type",
                                  "application/json") != ESP_OK ||
       esp_http_client_set_post_field(work_->client, body, static_cast<int>(strlen(body))) !=
           ESP_OK)) {
    return false;
  }
  LaMarzoccoError gateError = LaMarzoccoError::NONE;
  const auto requestAllowed = [&]() {
    return networkEligible(gateError) &&
           !shotTransportPaused() &&
           (!(shotActive_.load() || outboundShotActive.load()) ||
            (!powerActive_.load() && !temperatureActive_.load())) &&
           !scaleConnecting_.load(std::memory_order_acquire) &&
           !abortRequested_.load(std::memory_order_acquire);
  };
  if (!requestAllowed()) {
    pendingCloudCall_.result = "canceled";
    if (installationInit) secureWipe(io_->body, sizeof(io_->body));
    return false;
  }
  const HeapCapSnapshot heapBefore = sampleHeapCaps();
  {
    TaskLockGuard lock(mux_);
    beginHeapLifecycle(tlsHeap_, HeapLifecycleEvent::TLS_REQUEST, heapBefore);
  }
  const uint32_t requestStartedAtMs = millis();
  serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                       "La Marzocco HTTP start id=%lu %s %s",
                       static_cast<unsigned long>(pendingCloudCall_.startedAtMs),
                       pendingCloudCall_.method, call.endpoint);
  esp_err_t performed = ESP_ERR_HTTP_EAGAIN;
  bool canceled = false;
  while (performed == ESP_ERR_HTTP_EAGAIN) {
    if (!requestAllowed()) {
      if (!websocketAdmitted()) stopWebSocket();
      canceled = true;
      performed = ESP_FAIL;
      break;
    }
    if (static_cast<uint32_t>(millis() - requestStartedAtMs) >=
        la_marzocco_timing::kHttpTimeoutMs) {
      performed = ESP_ERR_HTTP_READ_TIMEOUT;
      break;
    }
    performed = esp_http_client_perform(work_->client);
    if (!requestAllowed()) {
      if (!websocketAdmitted()) stopWebSocket();
      canceled = true;
      performed = ESP_FAIL;
      break;
    }
    // The deadline bounds waiting only: a complete late response is used.
    // Every cancel source notifies this task, so a longer wait adds no
    // cancellation latency while sparing ~1000 wakeups/s during TLS setup.
    if (performed == ESP_ERR_HTTP_EAGAIN) {
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
  }
  const HeapCapSnapshot heapAfter = sampleHeapCaps();
  {
    TaskLockGuard lock(mux_);
    (void)finishHeapLifecycle(
        tlsHeap_, performed == ESP_OK
                      ? HeapLifecycleResult::SUCCESS
                      : (canceled
                             ? HeapLifecycleResult::CANCELLED
                             : HeapLifecycleResult::FAILURE),
        heapAfter);
  }
  recordBootHeap(BootHeapStage::CLOUD_DONE,
                 performed == ESP_OK ? BootHeapResult::OK : BootHeapResult::FAILED,
                 millis(), heapAfter);
  work_->transportStatus = performed;
  work_->transportFailure = performed != ESP_OK;
  work_->lastRequestEndedAtMs = millis();
  if (work_->transportFailure) {
    pendingCloudCall_.socketErrno = esp_http_client_get_errno(work_->client);
    int tlsError = 0;
    int tlsFlags = 0;
    (void)esp_http_client_get_and_clear_last_tls_error(work_->client, &tlsError,
                                                       &tlsFlags);
    pendingCloudCall_.tlsError = tlsError;
    pendingCloudCall_.tlsFlags = tlsFlags;
    // A failed perform typically leaves a dead cached socket; rebuild the
    // session on the next attempt instead of reusing it for all retries.
    esp_http_client_cleanup(work_->client);
    work_->client = nullptr;
    if (canceled) completeOutboundAcquisitionPause(OutboundClient::LA_MARZOCCO_HTTP, millis());
  }
  if (work_->responseUsed < sizeof(io_->response)) {
    io_->response[work_->responseUsed] = '\0';
  }
  const bool ok = performed == ESP_OK && !work_->responseOverflow &&
                  work_->httpStatus >= 200 && work_->httpStatus < 300;
  work_->policyRequired = performed == ESP_OK && work_->httpStatus == 403 &&
                          policyAcceptanceRequired(io_->response);
  pendingCloudCall_.httpStatus = work_->httpStatus;
  pendingCloudCall_.transportStatus = performed;
  pendingCloudCall_.result = laMarzoccoCloudCallResult(
      canceled, work_->responseOverflow, ok, work_->httpStatus);
  if (installationInit) {
    secureWipe(io_->response, sizeof(io_->response));
    work_->responseUsed = 0;
  }
  return ok;
}

void OpenBrewByWeightLaMarzoccoService::clearRequestState() {
  if (work_ == nullptr || work_->client == nullptr) return;
  (void)esp_http_client_delete_header(work_->client, "Authorization");
  (void)esp_http_client_delete_header(work_->client, "Content-Type");
  (void)esp_http_client_delete_header(work_->client,
                                      "X-App-Installation-Id");
  (void)esp_http_client_delete_header(work_->client, "X-Timestamp");
  (void)esp_http_client_delete_header(work_->client, "X-Nonce");
  (void)esp_http_client_delete_header(work_->client,
                                      "X-Request-Signature");
  (void)esp_http_client_delete_header(work_->client, "X-Request-Proof");
  (void)esp_http_client_set_post_field(work_->client, nullptr, 0);
}

bool OpenBrewByWeightLaMarzoccoService::ensureWorkBuffer() {
  if (work_ != nullptr) return true;
  work_ = static_cast<WorkBuffer *>(
      allocExternal(sizeof(WorkBuffer)));
  if (work_ == nullptr) return false;
  new (work_) WorkBuffer{};
  return true;
}

bool OpenBrewByWeightLaMarzoccoService::ensureIoBuffer() {
  if (io_ != nullptr) return true;
  io_ = static_cast<IoBuffer *>(
      allocExternal(sizeof(IoBuffer)));
  if (io_ == nullptr) return false;
  new (io_) IoBuffer{};
  return true;
}

void OpenBrewByWeightLaMarzoccoService::clearSession() {
  tokenAvailable_.store(false, std::memory_order_release);
  releaseIoBuffer();
  if (work_ == nullptr) return;
  secureWipe(work_->accessToken, sizeof(work_->accessToken));
  secureWipe(work_->refreshToken, sizeof(work_->refreshToken));
  work_->publicIdentity.clear();
  work_->responseUsed = 0;
  work_->accessTokenIssuedAtMs = 0;
  work_->httpStatus = 0;
  work_->transportStatus = 0;
  work_->responseOverflow = false;
  if (work_->client != nullptr) {
    esp_http_client_cleanup(work_->client);
    work_->client = nullptr;
  }
}

void OpenBrewByWeightLaMarzoccoService::releaseIoBuffer(bool responseValid) {
  if (pendingCloudCall_.api != nullptr) {
    LaMarzoccoCloudCall call = pendingCloudCall_;
    pendingCloudCall_ = {};
    call.durationMs = millis() - call.startedAtMs;
    if (!responseValid) call.result = "invalid_response";
    {
      TaskLockGuard lock(mux_);
      publishedCloudCall_ = call;
    }
    const bool success = strcmp(call.result, "success") == 0;
    const bool transient = strcmp(call.result, "canceled") == 0 ||
        strcmp(call.result, "transport_error") == 0 ||
        (strcmp(call.result, "http_error") == 0 &&
         laMarzoccoHttpRetryable(call.httpStatus));
    serialTraceCategoryf(success ? LogLevel::INFO
                                : transient ? LogLevel::WARNING : LogLevel::ERROR,
                         DebugCategory::NETWORK,
                         "La Marzocco id=%lu %s %s %s http=%u raw=%ld ms=%lu "
                         "reused=%u idle=%lu errno=%ld tls=%ld/0x%lx",
                         static_cast<unsigned long>(call.startedAtMs),
                         call.method, call.api, call.result,
                         static_cast<unsigned>(call.httpStatus),
                         static_cast<long>(call.transportStatus),
                         static_cast<unsigned long>(call.durationMs),
                         static_cast<unsigned>(call.clientReused),
                         static_cast<unsigned long>(call.idleMs),
                         static_cast<long>(call.socketErrno),
                         static_cast<long>(call.tlsError),
                         static_cast<unsigned long>(call.tlsFlags));
  }
  if (io_ == nullptr) return;
  secureWipe(io_->response, sizeof(io_->response));
  io_->~IoBuffer();
  heapCapsFree(io_);
  io_ = nullptr;
}

void OpenBrewByWeightLaMarzoccoService::releaseWorkBuffer() {
  clearSession();
  if (work_ == nullptr) return;
  work_->~WorkBuffer();
  heapCapsFree(work_);
  work_ = nullptr;
}

esp_err_t OpenBrewByWeightLaMarzoccoService::httpEvent(esp_http_client_event_t *event) {
  if (event == nullptr || event->user_data == nullptr) return ESP_FAIL;
  auto *self = static_cast<OpenBrewByWeightLaMarzoccoService *>(event->user_data);
  // IDF keeps the previous response's status across close() and a failed
  // reconnect, so only this request's status line may set it.
  if (event->event_id == HTTP_EVENT_ON_HEADERS_COMPLETE && self->work_ != nullptr) {
    self->work_->httpStatus =
        static_cast<uint16_t>(esp_http_client_get_status_code(event->client));
  }
  if (event->event_id != HTTP_EVENT_ON_DATA || event->data == nullptr ||
      event->data_len <= 0 || self->work_ == nullptr || self->io_ == nullptr) {
    return ESP_OK;
  }
  WorkBuffer &work = *self->work_;
  IoBuffer &io = *self->io_;
  const size_t length = static_cast<size_t>(event->data_len);
  if (length >= sizeof(io.response) - work.responseUsed) {
    work.responseOverflow = true;
    return ESP_FAIL;
  }
  memcpy(io.response + work.responseUsed, event->data, length);
  work.responseUsed += length;
  io.response[work.responseUsed] = '\0';
  return ESP_OK;
}

}  // namespace openbrewbyweight
