#include "OpenBrewByWeightMicraService.h"
#include "OpenBrewByWeightAuditLog.h"
#include "OpenBrewByWeightFirmwareMode.h"
#include "OpenBrewByWeightOutboundAdmission.h"

#include "OpenBrewByWeightJsonArena.h"
#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightMicraTiming.h"
#include "OpenBrewByWeightTaskProfiler.h"
#include "OpenBrewByWeightMicraPublicIdentityCache.h"
#include "OpenBrewByWeightPsram.h"

#include <Arduino.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_websocket_client.h>
#include <esp_timer.h>
#include "OpenBrewByWeightMicraStomp.h"
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

bool installationId(const LineaMicraPersistedSettings &settings,
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

bool publicKeyDer(const LineaMicraPersistedSettings &settings,
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

bool signRequest(const LineaMicraPersistedSettings &settings,
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

// Field-wise identity for a queued command lane; the requestId makes an
// in-flight execution stale when the desired request is replaced.
bool sameCommandRequest(const LineaMicraRequest &left,
                        const LineaMicraRequest &right) {
  return left.requestId == right.requestId &&
         left.configGeneration == right.configGeneration &&
         left.targetDeciC == right.targetDeciC &&
         left.presetId == right.presetId && left.type == right.type;
}

// Persisted option bit that authorizes each power command direction.
uint8_t powerOptionBit(LineaMicraRequestType type) {
  return type == LineaMicraRequestType::SET_POWER_ON
             ? LINEA_MICRA_POWER_ON_WITH_SCALE
             : LINEA_MICRA_SHUTDOWN_WITH_SCALE;
}

bool sameSessionIdentity(const LineaMicraPersistedSettings &left,
                         const LineaMicraPersistedSettings &right) {
  return left.accountConfigured == right.accountConfigured &&
         strcmp(left.username, right.username) == 0 &&
         strcmp(left.password, right.password) == 0 &&
         memcmp(left.installationPrivateKey, right.installationPrivateKey,
                sizeof(left.installationPrivateKey)) == 0 &&
         strcmp(left.selectedSerial, right.selectedSerial) == 0;
}

// IDF aborts perform() on a 401 (auth support compiled out), so an HTTP
// status, not the transport result, names what the server answered.
const char *micraCloudCallResult(bool canceled, bool overflow, bool ok,
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
#error "Micra IoBuffer aliases body and response; HTTP client auth retries would corrupt the body"
#endif
struct OpenBrewByWeightMicraService::IoBuffer {
  union {
    char response[kResponseCapacity];
    char body[kBodyCapacity];
  };

  IoBuffer() : response{} {}
};

struct OpenBrewByWeightMicraService::WorkBuffer {
  MicraPublicIdentityCache publicIdentity;
  char accessToken[kTokenCapacity] = {};
  char refreshToken[kTokenCapacity] = {};
  // Scratch for the formatted Authorization header. Lives beside the tokens in
  // external RAM so request() never renders the header over the IoBuffer union
  // (which aliases response and body) and never adds a large stack frame.
  char authorization[kTokenCapacity + 8] = {};
  esp_http_client_handle_t client = nullptr;
  size_t responseUsed = 0;
  uint32_t accessTokenIssuedAtMs = 0;
  uint32_t lastRequestEndedAtMs = 0;
  uint16_t httpStatus = 0;
  int32_t transportStatus = 0;
  bool responseOverflow = false;
  bool transportFailure = false;
};

struct OpenBrewByWeightMicraService::RequestStateGuard {
  OpenBrewByWeightMicraService &service;
  ~RequestStateGuard() { service.clearRequestState(); }
};

#include "OpenBrewByWeightMicraWebSocket.inc"

bool OpenBrewByWeightMicraService::begin() {
  initJsonParser();
  // The IDF WS transport prints signed Upgrade headers even on write errors.
  esp_log_level_set("transport_ws", ESP_LOG_NONE);
  if (task_ != nullptr || psa_crypto_init() != PSA_SUCCESS) {
    return false;
  }
  if (xTaskCreatePinnedToCore(taskEntry, "micra_cloud", kWorkerStackBytes, this,
                             tskIDLE_PRIORITY, &task_, 0) != pdPASS) {
    return false;
  }
  return true;
}

void OpenBrewByWeightMicraService::publishConfig(
    const LineaMicraPersistedSettings &settings, uint32_t configGeneration) {
  // Compatibility mode parks the cloud client through the existing
  // unconfigured-account machinery: no session, no scheduling, in-flight HTTP
  // aborted. The persisted settings themselves stay untouched.
  LineaMicraPersistedSettings effective = settings;
  if (firmwareCompatibilityMode()) {
    effective = LineaMicraPersistedSettings{};
  }
  bool cancelTemperature = false;
  bool cancelPowerOff = false;
  bool cancelObservation = false;
  bool identityChanged = false;
  bool transportChanged = false;
  {
    TaskLockGuard lock(mux_);
    const bool wasObserving =
        config_.accountConfigured &&
        (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0;
    const bool configChanged =
        memcmp(&config_, &effective, sizeof(effective)) != 0;
    identityChanged = !sameSessionIdentity(config_, effective);
    transportChanged = config_.connectionType != effective.connectionType;
    if (identityChanged || transportChanged ||
        (effective.options & LINEA_MICRA_OBSERVE_STATE) == 0)
      websocketUnexpectedReconnect_ = false;
    if (!identityChanged && (transportChanged ||
        (wasObserving && (effective.options & LINEA_MICRA_OBSERVE_STATE) == 0))) {
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
      wipeLineaMicraSettings(pending_.credentials);
      pending_.present = false;
      desiredTemperature_ = {};
      desiredPower_ = {};
      cancelTemperature = true;
      cancelPowerOff = true;
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
    published_.configGeneration = configGeneration;
    published_.connectionFreshness = effective.connectionType ==
        static_cast<uint8_t>(MicraConnectionType::WEBSOCKET);
    if (identityChanged || transportChanged || !effective.accountConfigured ||
        (effective.options & LINEA_MICRA_OBSERVE_STATE) == 0) {
      backflush_.reset();
      snapshotPending_ = false;
      observationFence_.invalidate();
      observationFence_.offline = false;
    }
    published_.identityGeneration = identityGeneration_;
    published_.accountConfigured = effective.accountConfigured;
    const bool observing =
        effective.accountConfigured &&
        (effective.options & LINEA_MICRA_OBSERVE_STATE) != 0;
    if (!configChanged) {
      wipeLineaMicraSettings(effective);
      return;
    }
    wipeLineaMicraSettings(candidate_);
    const uint8_t accountMachines =
        effective.accountConfigured ? discovery_.accountMachines : 0;
    discovery_ = {};
    discovery_.accountMachines = accountMachines;
    if (!effective.accountConfigured) {
      powerState_.reset();
      wipeLineaMicraSettings(pending_.credentials);
      pending_.present = false;
      desiredTemperature_ = {};
      desiredPower_ = {};
      published_.phase = LineaMicraPhase::Disabled;
      published_.quality = LineaMicraObservationQuality::UNCONFIGURED;
      published_.temperatureState = LineaMicraTemperatureState::Disabled;
      observationSchedule_ = {};
    } else if (identityChanged ||
               published_.phase == LineaMicraPhase::Disabled) {
      published_.phase = LineaMicraPhase::IDLE;
      published_.quality = LineaMicraObservationQuality::STALE;
      observationSchedule_.dueNow(millis());
    }
    if (!observing) {
      powerState_.reset();
      observationSchedule_ = {};
      if (wasObserving && effective.accountConfigured) {
        published_.phase = LineaMicraPhase::IDLE;
        published_.error = LineaMicraError::NONE;
      }
      if (pending_.present &&
          pending_.request.type == LineaMicraRequestType::OBSERVE_STATE) {
        wipeLineaMicraSettings(pending_.credentials);
        pending_.present = false;
      }
      cancelObservation = wasObserving;
    } else if (!wasObserving) {
      observationSchedule_.dueNow(millis());
    }
    if ((effective.options & LINEA_MICRA_APPLY_TEMPERATURE) == 0) {
      desiredTemperature_ = {};
      published_.temperatureState = LineaMicraTemperatureState::Disabled;
      published_.temperatureError = LineaMicraError::NONE;
      published_.temperatureTransportStatus = 0;
      published_.temperatureHttpStatus = 0;
      published_.temperatureCommandAccepted = false;
      published_.temperatureRetryable = false;
      cancelTemperature = true;
    } else if (effective.accountConfigured &&
               published_.temperatureState ==
                   LineaMicraTemperatureState::Disabled) {
      published_.temperatureState = LineaMicraTemperatureState::IDLE;
    }
    if (desiredPower_.present &&
        (effective.options & powerOptionBit(desiredPower_.request.type)) == 0) {
      desiredPower_ = {};
      cancelPowerOff = true;
    }
  }
  const bool cloudDisabled =
      !effective.accountConfigured ||
      (effective.options & (LINEA_MICRA_APPLY_TEMPERATURE |
                           LINEA_MICRA_OBSERVE_STATE |
                           LINEA_MICRA_SHUTDOWN_WITH_SCALE |
                           LINEA_MICRA_POWER_ON_WITH_SCALE)) == 0;
  wipeLineaMicraSettings(effective);
  if (identityChanged || transportChanged || cloudDisabled) {
    if (identityChanged || transportChanged)
      websocketRetryRequested_.store(true, std::memory_order_release);
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
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

void OpenBrewByWeightMicraService::publishNetworkState(bool staConnected,
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
          websocketStatus_.state >= MicraSocketState::CONNECTING &&
          websocketStatus_.state <= MicraSocketState::STREAMING)
        websocketUnexpectedReconnect_ = true;
      powerState_.hold(published_, config_.accountConfigured &&
          (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0, millis());
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
        (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0 &&
        published_.error != LineaMicraError::INVALID_AUTH) {
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

bool OpenBrewByWeightMicraService::queueConnect(uint32_t requestId,
                                           const char *username,
                                           const char *password) {
  LineaMicraPersistedSettings credentials;
  if (!secureCopy(credentials.username, sizeof(credentials.username), username) ||
      !secureCopy(credentials.password, sizeof(credentials.password), password) ||
      !lineaMicraBoundedText(credentials.username,
                             sizeof(credentials.username), false) ||
      strchr(credentials.username, '@') == nullptr ||
      !lineaMicraBoundedText(credentials.password,
                             sizeof(credentials.password), false)) {
    wipeLineaMicraSettings(credentials);
    return false;
  }
  TaskLockGuard lock(mux_);
  if (pending_.present || active_ || task_ == nullptr) {
    wipeLineaMicraSettings(credentials);
    return false;
  }
  pending_ = {};
  pending_.request.requestId = requestId;
  pending_.request.configGeneration = configGeneration_;
  pending_.request.type = LineaMicraRequestType::CONNECT;
  pending_.credentials = credentials;
  pending_.identityGeneration = identityGeneration_;
  pending_.present = true;
  published_.requestId = requestId;
  published_.phase = LineaMicraPhase::QUEUED;
  published_.error = LineaMicraError::NONE;
  discovery_ = {};
  wipeLineaMicraSettings(candidate_);
  wipeLineaMicraSettings(credentials);
  xTaskNotifyGive(task_);
  return true;
}

// Lists the saved account's machines again so another one can be selected;
// the saved machine stays active until that selection is saved.
bool OpenBrewByWeightMicraService::queueRelist(uint32_t requestId) {
  TaskLockGuard lock(mux_);
  if (pending_.present || active_ || task_ == nullptr ||
      !config_.accountConfigured) {
    return false;
  }
  pending_ = {};
  pending_.request.requestId = requestId;
  pending_.request.configGeneration = configGeneration_;
  pending_.request.type = LineaMicraRequestType::LIST_MACHINES;
  pending_.credentials = config_;
  pending_.identityGeneration = identityGeneration_;
  pending_.present = true;
  const uint8_t accountMachines = discovery_.accountMachines;
  discovery_ = {};
  discovery_.accountMachines = accountMachines;
  discovery_.listing = true;
  wipeLineaMicraSettings(candidate_);
  xTaskNotifyGive(task_);
  return true;
}

bool OpenBrewByWeightMicraService::queue(const LineaMicraRequest &request) {
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

bool OpenBrewByWeightMicraService::queueLocked(const LineaMicraRequest &request) {
  if (request.type == LineaMicraRequestType::APPLY_TEMPERATURE) {
    if (task_ == nullptr || !config_.accountConfigured ||
        (config_.options & LINEA_MICRA_APPLY_TEMPERATURE) == 0 ||
        request.presetId == 0 ||
        request.targetDeciC < LINEA_MICRA_BREW_TARGET_MIN_DECI_C ||
        request.targetDeciC > LINEA_MICRA_BREW_TARGET_MAX_DECI_C) {
      return false;
    }
    if (desiredTemperature_.present &&
        static_cast<int32_t>(request.configGeneration -
                             desiredTemperature_.request.configGeneration) < 0) {
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
    published_.temperatureState = LineaMicraTemperatureState::PENDING;
    published_.temperatureError = LineaMicraError::NONE;
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
  if (request.type == LineaMicraRequestType::SET_STANDBY ||
      request.type == LineaMicraRequestType::SET_POWER_ON) {
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
      (config_.options & LINEA_MICRA_OBSERVE_STATE) == 0 ||
      request.type != LineaMicraRequestType::OBSERVE_STATE) {
    return false;
  }
  pending_ = {};
  pending_.request = request;
  pending_.identityGeneration = identityGeneration_;
  pending_.present = true;
  published_.requestId = request.requestId;
  websocketRetryRequested_.store(true, std::memory_order_release);
  published_.phase = LineaMicraPhase::QUEUED;
  published_.error = LineaMicraError::NONE;
  xTaskNotifyGive(task_);
  return true;
}

bool OpenBrewByWeightMicraService::selectDiscoveredMachine(
    const char *serial, LineaMicraPersistedSettings &settings) {
  TaskLockGuard lock(mux_);
  if (serial == nullptr || discovery_.count == 0 ||
      !lineaMicraPrivateKeyConfigured(candidate_)) {
    return false;
  }
  for (uint8_t index = 0; index < discovery_.count; ++index) {
    if (strcmp(discovery_.machines[index].serial, serial) != 0) continue;
    settings = candidate_;
    if (!secureCopy(settings.selectedSerial, sizeof(settings.selectedSerial),
                    discovery_.machines[index].serial) ||
        !secureCopy(settings.selectedName, sizeof(settings.selectedName),
                    discovery_.machines[index].name)) {
      wipeLineaMicraSettings(settings);
      return false;
    }
    settings.accountConfigured = true;
    return validLineaMicraSettings(settings);
  }
  return false;
}

void OpenBrewByWeightMicraService::clearDiscovery() {
  TaskLockGuard lock(mux_);
  wipeLineaMicraSettings(candidate_);
  discovery_ = {};
}

LineaMicraStatus OpenBrewByWeightMicraService::status() const {
  TaskLockGuard lock(mux_);
  return statusLocked();
}

LineaMicraStatus OpenBrewByWeightMicraService::statusLocked() const {
  LineaMicraStatus result = published_;
  const uint32_t now = millis();
  const bool observing = config_.accountConfigured &&
                         (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0;
  result = powerState_.effectiveStatus(result, observing, now);
  if (result.connectionFreshness && result.quality == LineaMicraObservationQuality::CURRENT &&
      (!observationFence_.synchronized || !websocketStatus_.subscribed || !websocket_ ||
       websocket_->failed.load() || websocket_->stamp.epoch != observationFence_.epoch ||
       !staConnected_.load() || apActive_.load() || outboundMaintenance.load()))
    result.quality = LineaMicraObservationQuality::STALE;
  result.staConnected = staConnected_.load(std::memory_order_acquire);
  result.apActive = apActive_.load(std::memory_order_acquire);
  result.shotPaused = shotTransportPaused();
  result.scalePaused = !result.connectionFreshness && scaleConnecting_.load(std::memory_order_acquire);
  return result;
}

LineaMicraStatus OpenBrewByWeightMicraService::statusPoll() const {
  if (mux_.tryLock()) {
    statusCache_ = statusLocked();
    statusCacheWrittenAtMs_ = millis();
    mux_.unlock();
  }
  return statusCache_;
}

bool OpenBrewByWeightMicraService::statusPollStale() const {
  // 250 ms is ~50 consecutive misses at the loop's 1-5 ms poll cadence:
  // genuinely anomalous contention, not a busy instant.
  constexpr uint32_t kStatusPollCacheStaleMs = 250;
  return millis() - statusCacheWrittenAtMs_ >= kStatusPollCacheStaleMs;
}

HeapLifecycleAggregate OpenBrewByWeightMicraService::heapTelemetry() const {
  TaskLockGuard lock(mux_);
  return tlsHeap_.aggregate;
}

LineaMicraCloudCall OpenBrewByWeightMicraService::cloudCall() const {
  TaskLockGuard lock(mux_);
  return publishedCloudCall_;
}

void OpenBrewByWeightMicraService::qualifyBackflushLocked() {
  const uint32_t now = millis();
  const uint32_t aliveAt = websocketStatus_.pongAtMs ? websocketStatus_.pongAtMs
                                                   : websocketStatus_.connectedAtMs;
  if (!config_.accountConfigured || !(config_.options & LINEA_MICRA_OBSERVE_STATE) ||
      config_.connectionType != static_cast<uint8_t>(MicraConnectionType::WEBSOCKET) ||
      !websocket_ || websocket_->failed.load() || !websocketStatus_.subscribed ||
      !tokenAvailable_.load(std::memory_order_acquire) ||
      websocket_->stamp.identity != identityGeneration_ ||
      websocket_->stamp.epoch != observationFence_.epoch ||
      !websocketStatus_.machineConnectedKnown || !websocketStatus_.machineConnected ||
      observationFence_.offline || now - aliveAt >= 30000 ||
      !staConnected_.load() || apActive_.load() || shotActive_.load() ||
      outboundShotActive.load() || outboundMaintenance.load())
    backflush_.invalidate();
}

bool OpenBrewByWeightMicraService::backflushReadyLocked() const {
  const auto &state = backflush_.status();
  return state.valid && state.phase == MachineBackflushPhase::AWAITING &&
      !state.consumed && tokenAvailable_.load(std::memory_order_acquire) &&
      millis() - tokenIssuedAtMs_.load(std::memory_order_acquire) <
          micra_timing::kAccessTokenRefreshAgeMs - 210000 &&
      observationFence_.synchronized && published_.powerState == LineaMicraPowerState::ON &&
      !temperatureActive_.load() && !powerActive_.load();
}

MachineBackflushSnapshot OpenBrewByWeightMicraService::backflush(bool consume) {
  // An idle handoff cannot become ready without a mutation when it is invalid
  // or the machine is off (readiness requires power ON), so the control
  // loop's consuming poll reuses its last locked read until a mutation marks
  // a change or the power mirror flips back on.
  if (consume && !backflush_.changed() && !postedPending() &&
      (!idleBackflush_.valid || powerOff_.load(std::memory_order_acquire))) {
    noteMutexWaitUs(micraBackflushWaitLastUsStat(), micraBackflushWaitMaxUsStat(), 0);
    noteMutexWaitUs(micraBackflushHoldLastUsStat(), micraBackflushHoldMaxUsStat(), 0);
    return idleBackflush_;
  }
  // The control loop polls this every iteration and must not wait behind a
  // core-0 holder: the wait stat keeps recording the acquisition attempt
  // (microseconds uncontended), the hold stat the locked section.
  const uint32_t muxLockStartedUs = micros();
  if (!mux_.tryLock()) {
    noteMutexWaitUs(micraBackflushWaitLastUsStat(), micraBackflushWaitMaxUsStat(),
                    micros() - muxLockStartedUs);
    noteMutexWaitUs(micraBackflushHoldLastUsStat(), micraBackflushHoldMaxUsStat(), 0);
    // Busy, no new edges: transitions keep accumulating in the bounded ring
    // while validity and permits stay the last locked resolution's. The
    // idle handoff is never served here; mid-episode it is stale and would
    // trip spurious supervision aborts.
    MachineBackflushSnapshot missed = lastPollSnapshot_;
    missed.count = 0;
    return missed;
  }
  const uint32_t lockedAtUs = micros();
  noteMutexWaitUs(micraBackflushWaitLastUsStat(), micraBackflushWaitMaxUsStat(),
                  lockedAtUs - muxLockStartedUs);
  applyPostedLocked();
  qualifyBackflushLocked();
  auto result = consume ? backflush_.take() : backflush_.status();
  result.ready = backflushReadyLocked();
  if (consume && !result.busy() && result.count == 0 &&
      (!result.valid || published_.powerState == LineaMicraPowerState::OFF)) {
    idleBackflush_ = result;
    backflush_.settle();
  }
  noteMutexWaitUs(micraBackflushHoldLastUsStat(), micraBackflushHoldMaxUsStat(),
                  micros() - lockedAtUs);
  lastPollSnapshot_ = result;
  mux_.unlock();
  return result;
}

bool OpenBrewByWeightMicraService::muxWaitAnomalyDue(
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

void OpenBrewByWeightMicraService::reportMuxWaitAnomaly(
    uint32_t waitedUs, const char *holder, uint32_t holdUs) {
  // Must run after unlock(): the logging path takes locks of its own, and
  // the longest-hold attribution was copied while mux_ was held (the waiter
  // resets it there, so each report covers the holds since its last lock).
  serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                       "Micra mux wait=%lu us holder=%s hold=%lu us",
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

void OpenBrewByWeightMicraService::finishBackflush(uint32_t attempt) {
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

MachinePhysicalStartDisposition OpenBrewByWeightMicraService::physicalStart(MachineBackflushPermit *permit) {
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

void OpenBrewByWeightMicraService::post(const LineaMicraRequest &request) {
  if (mux_.tryLock()) {
    applyPostedLocked();
    (void)queueLocked(request);
    mux_.unlock();
    return;
  }
  if (request.type == LineaMicraRequestType::APPLY_TEMPERATURE) {
    postedTemperature_ = request;
    postedTemperaturePresent_ = true;
  } else {
    postedPower_ = request;
    postedPowerPresent_ = true;
  }
}

void OpenBrewByWeightMicraService::applyPostedLocked() {
  if (postedFinishAttempt_ != 0) backflush_.finish(postedFinishAttempt_);
  if (postedTemperaturePresent_) (void)queueLocked(postedTemperature_);
  if (postedPowerPresent_) (void)queueLocked(postedPower_);
  postedFinishAttempt_ = 0;
  postedTemperaturePresent_ = postedPowerPresent_ = false;
}

MachinePhysicalStartDisposition OpenBrewByWeightMicraService::physicalStartLocked(
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
                         (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0;
  if (!powerState_.notePhysicalStart(
          published_, observing,
          (config_.options & LINEA_MICRA_RECOGNIZE_WAKE) != 0, now)) {
    return MachinePhysicalStartDisposition::NORMAL;
  }
  observationSchedule_.armPostEvent(now);
  return MachinePhysicalStartDisposition::WAKE_PASSTHROUGH;
}

LineaMicraDiscoverySnapshot OpenBrewByWeightMicraService::discovery() const {
  TaskLockGuard lock(mux_);
  return discovery_;
}

void OpenBrewByWeightMicraService::taskEntry(void *context) {
  static_cast<OpenBrewByWeightMicraService *>(context)->taskLoop();
}

void OpenBrewByWeightMicraService::taskLoop() {
  for (;;) {
    // Only the idle owner consumes cancellation, before selecting new work.
    abortRequested_.store(false, std::memory_order_release);
    if (clearSessionRequested_.exchange(false, std::memory_order_acq_rel)) {
      stopWebSocket(true);
      {
        TaskLockGuard lock(mux_);
        websocketStatus_.state = MicraSocketState::Disabled;
        websocketStatus_.retryAtMs = 0;
      }
      releaseWorkBuffer();
    }
    serviceWebSocket();
    if (outboundMaintenance.load()) releaseWorkBuffer();
    const bool scaleInhibited = outboundScaleInhibited();
    bool websocketMode;
    {
      TaskLockGuard lock(mux_);
      websocketMode = config_.connectionType ==
          static_cast<uint8_t>(MicraConnectionType::WEBSOCKET);
    }
    // Idle owner closes any HTTP keepalive before acknowledging the pause. The
    // rare WebSocket-mode API calls share a connection only within one
    // operation; API polling keeps it for kHttpReuseIdleMs.
    if ((scaleInhibited || websocketMode) && work_ && work_->client) {
      esp_http_client_cleanup(work_->client);
      work_->client = nullptr;
    }
    if (scaleInhibited)
      completeOutboundAcquisitionPause(OutboundClient::MICRA_HTTP, millis());
    PendingRequest pending;
    bool haveRequest = false;
    bool haveTemperature = false;
    bool havePower = false;
    uint32_t temperatureMachineConfigGeneration = 0;
    uint32_t powerMachineConfigGeneration = 0;
    const uint32_t now = millis();
    LineaMicraError observationGate = LineaMicraError::NONE;
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
    {
      TaskLockGuard lock(mux_);
      if (config_.connectionType == static_cast<uint8_t>(MicraConnectionType::WEBSOCKET))
        observationSchedule_.suspendPeriodic();
      if (desiredPower_.present && desiredPower_.shotGeneration != shotGeneration_.load(std::memory_order_acquire))
        desiredPower_ = {};
      const bool pendingObservation =
          pending_.present &&
          pending_.request.type == LineaMicraRequestType::OBSERVE_STATE;
      const bool cleaningBusy = backflush_.status().busy();
      if (cleaningBusy) desiredPower_ = {};
      const bool observationReady =
          networkReady && !localActivity && !scaleConnecting_.load(std::memory_order_acquire) &&
          observationSchedule_.observationAllowed(now);
      if (pending_.present && networkReady && !localActivity &&
          !scaleConnecting_.load(std::memory_order_acquire) &&
          (!pendingObservation || observationReady)) {
        pending = pending_;
        wipeLineaMicraSettings(pending_.credentials);
        pending_.present = false;
        observationActive_.store(pendingObservation,
                                 std::memory_order_release);
        active_ = true;
        haveRequest = true;
      } else if (networkReady && !shotActive_.load(std::memory_order_acquire) &&
                 !outboundShotActive.load(std::memory_order_acquire) &&
                 !scaleConnecting_.load(std::memory_order_acquire) &&
                 !cleaningBusy && desiredTemperature_.present &&
                 static_cast<int32_t>(now - desiredTemperature_.retryAtMs) >= 0) {
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
                 static_cast<int32_t>(now - desiredPower_.retryAtMs) >= 0) {
        pending.request = desiredPower_.request;
        powerMachineConfigGeneration =
            desiredPower_.machineConfigGeneration;
        active_ = true;
        haveRequest = true;
        havePower = true;
        powerActive_.store(true, std::memory_order_release);
      } else if (observationReady && config_.accountConfigured &&
                 (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0 &&
                 (snapshotPending_ || observationSchedule_.automaticDue(now))) {
        pending.request.requestId = nextAutomaticRequestId_++;
        pending.request.configGeneration = configGeneration_;
        pending.request.type = LineaMicraRequestType::OBSERVE_STATE;
        pending.identityGeneration = identityGeneration_;
        observationActive_.store(true, std::memory_order_release);
        active_ = true;
        haveRequest = true;
      }
      if (haveRequest && pending.request.type == LineaMicraRequestType::OBSERVE_STATE) {
        const bool reconcile = observationSchedule_.observationStarted(now);
        if (snapshotPending_) {
          // Reconciliation needs a request-time baseline, even if wake preceded subscription.
          pending.initialSnapshot = !reconcile && snapshotStamp_.intent == powerState_.generation();
          pending.snapshotStamp = snapshotStamp_;
          snapshotPending_ = false;
        } else if (reconcile) pending.initialSnapshot = false;
      }
    }
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
      wipeLineaMicraSettings(pending.credentials);
      TaskLockGuard lock(mux_);
      active_ = false;
      continue;
    }
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
  }
}

bool OpenBrewByWeightMicraService::networkEligible(LineaMicraError &error, bool observation) const {
  if (outboundMaintenance.load(std::memory_order_acquire)) {
    error = LineaMicraError::CANCELED;
    return false;
  }
  if (!observation && outboundScaleInhibited()) {
    error = LineaMicraError::CANCELED;
    return false;
  }
  if (!observation && shotTransportPaused()) {
    error = LineaMicraError::CANCELED;
    return false;
  }
  if (apActive_.load(std::memory_order_acquire)) {
    error = LineaMicraError::AP_MODE;
    return false;
  }
  if (!staConnected_.load(std::memory_order_acquire)) {
    error = LineaMicraError::NO_STA;
    return false;
  }
  timeval now{};
  gettimeofday(&now, nullptr);
  if (now.tv_sec < 1700000000L) {
    error = LineaMicraError::CLOCK_UNSYNCED;
    return false;
  }
  error = LineaMicraError::NONE;
  return true;
}

bool OpenBrewByWeightMicraService::temperatureEligible(
    LineaMicraError &error) const {
  if (!networkEligible(error)) return false;
  {
    TaskLockGuard lock(mux_);
    // An offline machine cannot apply a target: wait locally for it to come
    // back instead of spending cloud cycles that cannot confirm.
    if (backflush_.status().busy() || observationFence_.offline) {
      error = LineaMicraError::CANCELED;
      return false;
    }
  }
  if (shotActive_.load(std::memory_order_acquire) ||
      outboundShotActive.load(std::memory_order_acquire) ||
      scaleConnecting_.load(std::memory_order_acquire)) {
    error = LineaMicraError::CANCELED;
    return false;
  }
  return true;
}

void OpenBrewByWeightMicraService::execute(PendingRequest &pending) {
  if (pending.request.type == LineaMicraRequestType::LIST_MACHINES) {
    executeRelist(pending);
    return;
  }
  LineaMicraStatus status;
  {
    TaskLockGuard lock(mux_);
    status = published_;
  }
  status.requestId = pending.request.requestId;
  status.configGeneration = pending.request.configGeneration;
  status.identityGeneration = pending.identityGeneration;
  status.error = LineaMicraError::NONE;
  status.transportStatus = 0;
  status.httpStatus = 0;
  LineaMicraError gateError = LineaMicraError::NONE;
  if (!networkEligible(gateError)) {
    if (pending.request.type == LineaMicraRequestType::OBSERVE_STATE) {
      deferObservation(pending, status, gateError);
      return;
    }
    fail(status, gateError);
    scheduleAutomatic(millis(), true);
    return;
  }
  if (pending.request.type == LineaMicraRequestType::OBSERVE_STATE &&
      shotTransportPaused()) {
    deferObservation(pending, status, LineaMicraError::CANCELED);
    return;
  }
  if (!ensureWorkBuffer()) {
    fail(status, LineaMicraError::HTTP_ERROR);
    scheduleAutomatic(
        millis(), pending.request.type == LineaMicraRequestType::CONNECT);
    return;
  }
  work_->transportStatus = 0;
  work_->httpStatus = 0;
  const bool connecting = pending.request.type == LineaMicraRequestType::CONNECT;
  const bool success = connecting ? executeConnect(pending)
                                  : executeObservation(pending);
  if (!success) scheduleAutomatic(millis(), connecting);
  if (connecting) releaseWorkBuffer();
}

bool OpenBrewByWeightMicraService::temperatureRequestCurrent(
    const LineaMicraRequest &request,
    uint32_t machineConfigGeneration) const {
  TaskLockGuard lock(mux_);
  return desiredTemperature_.present &&
         desiredTemperature_.machineConfigGeneration ==
             machineConfigGeneration &&
         sameCommandRequest(desiredTemperature_.request, request) &&
         configGeneration_ == machineConfigGeneration &&
         config_.accountConfigured &&
         (config_.options & LINEA_MICRA_APPLY_TEMPERATURE) != 0;
}

bool OpenBrewByWeightMicraService::powerRequestCurrent(
    const LineaMicraRequest &request,
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

bool OpenBrewByWeightMicraService::identityCurrent(
    uint32_t identityGeneration) const {
  TaskLockGuard lock(mux_);
  return identityGeneration == identityGeneration_;
}

bool OpenBrewByWeightMicraService::observationCurrent(
    uint32_t identityGeneration) const {
  TaskLockGuard lock(mux_);
  return identityGeneration == identityGeneration_ &&
         config_.accountConfigured &&
         (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0;
}

void OpenBrewByWeightMicraService::deferTemperature(
    const LineaMicraRequest &request, LineaMicraError error, uint32_t delayMs,
    bool retryable, bool exhausted) {
  TaskLockGuard lock(mux_);
  if (!desiredTemperature_.present ||
      !sameCommandRequest(desiredTemperature_.request, request)) {
    return;
  }
  if (exhausted && ++desiredTemperature_.cycles >= micra_timing::kMaxCommandCycles)
    retryable = false;
  published_.temperatureCommandAccepted =
      desiredTemperature_.commandAccepted;
  published_.temperatureRetryable = retryable;
  if (retryable) {
    desiredTemperature_.retryAtMs = millis() + delayMs;
  } else {
    desiredTemperature_ = {};
  }
  published_.temperatureState = error == LineaMicraError::CANCELED
                                    ? LineaMicraTemperatureState::CANCELED
                                    : LineaMicraTemperatureState::FAILED;
  published_.temperatureError = error;
  const uint16_t httpStatus = work_ != nullptr ? work_->httpStatus : 0;
  const int32_t transportStatus =
      work_ != nullptr ? work_->transportStatus : 0;
  published_.temperatureTransportStatus = transportStatus;
  published_.temperatureHttpStatus = httpStatus;
  lock.unlock();  // logging takes its own locks; never nest them in mux_
  if (error != LineaMicraError::CANCELED) {
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "Micra temperature request failed preset=%u target=%u.%u error=%s http=%u raw=%ld",
                         static_cast<unsigned>(request.presetId),
                         static_cast<unsigned>(request.targetDeciC / 10U),
                         static_cast<unsigned>(request.targetDeciC % 10U),
                         lineaMicraErrorName(error),
                         static_cast<unsigned>(httpStatus),
                         static_cast<long>(transportStatus));
  }
}

void OpenBrewByWeightMicraService::deferPower(
    const LineaMicraRequest &request, LineaMicraError error, uint32_t delayMs,
    bool retryable, bool exhausted) {
  TaskLockGuard lock(mux_);
  if (!desiredPower_.present ||
      !sameCommandRequest(desiredPower_.request, request)) {
    return;
  }
  if (exhausted && ++desiredPower_.cycles >= micra_timing::kMaxCommandCycles)
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
  serialTraceCategoryf(error == LineaMicraError::CANCELED ? LogLevel::INFO
                                                          : LogLevel::WARNING,
                       DebugCategory::NETWORK,
                       "Micra scale power request %s type=%s error=%s http=%u raw=%ld",
                       retryable ? "deferred" : "dropped",
                       request.type == LineaMicraRequestType::SET_POWER_ON
                           ? "power_on"
                           : "standby",
                       lineaMicraErrorName(error),
                       static_cast<unsigned>(httpStatus),
                       static_cast<long>(transportStatus));
}

bool OpenBrewByWeightMicraService::executeTemperatureApplication(
    const LineaMicraRequest &request,
    uint32_t machineConfigGeneration) {
  LineaMicraPersistedSettings settings;
  bool commandAccepted = false;
  {
    TaskLockGuard lock(mux_);
    if (!desiredTemperature_.present ||
        desiredTemperature_.machineConfigGeneration !=
            machineConfigGeneration ||
        !sameCommandRequest(desiredTemperature_.request, request) ||
        configGeneration_ != machineConfigGeneration) {
      return true;
    }
    const LineaMicraStatus current = statusLocked();
    const bool unchanged = current.connectionFreshness &&
        current.quality == LineaMicraObservationQuality::CURRENT &&
        observationFence_.temperatureTrusted && published_.targetValid &&
        published_.targetDeciC == request.targetDeciC;
    published_.requestId = request.requestId;
    published_.requestedTargetDeciC = request.targetDeciC;
    published_.temperatureState = unchanged ? LineaMicraTemperatureState::CONFIRMED
                                            : LineaMicraTemperatureState::RUNNING;
    published_.temperatureError = LineaMicraError::NONE;
    published_.temperatureRetryable = !unchanged;
    if (unchanged) {
      // The subscription would have pushed any change: the machine already
      // holds this target, so no cloud command is needed.
      desiredTemperature_ = {};
      published_.appliedTargetDeciC = request.targetDeciC;
      published_.temperatureTransportStatus = 0;
      published_.temperatureHttpStatus = 0;
      published_.temperatureCommandAccepted = false;
      lock.unlock();  // logging takes its own locks; never nest them in mux_
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                           "Micra temperature %u.%u already set, command skipped",
                           static_cast<unsigned>(request.targetDeciC / 10U),
                           static_cast<unsigned>(request.targetDeciC % 10U));
      return true;
    }
    settings = config_;
    commandAccepted = desiredTemperature_.commandAccepted;
    published_.temperatureCommandAccepted = commandAccepted;
  }
  if (!ensureWorkBuffer()) {
    wipeLineaMicraSettings(settings);
    deferTemperature(request, LineaMicraError::HTTP_ERROR,
                     micra_timing::kExhaustedCooldownMs);
    return false;
  }

  bool commandAttempted = false;
  int32_t commandTransportStatus = 0;
  uint16_t commandHttpStatus = 0;
  for (size_t attempt = 0; attempt < micra_timing::kMaxAttempts; ++attempt) {
    LineaMicraError gateError = LineaMicraError::NONE;
    if (!temperatureRequestCurrent(request, machineConfigGeneration)) {
      wipeLineaMicraSettings(settings);
      return true;
    }
    if (!temperatureEligible(gateError)) {
      wipeLineaMicraSettings(settings);
      deferTemperature(request, gateError,
                       gateError == LineaMicraError::CANCELED
                           ? micra_timing::kGateRetryMs
                           : micra_timing::kExhaustedCooldownMs);
      return false;
    }

    LineaMicraStatus verification;
    {
      TaskLockGuard lock(mux_);
      verification = published_;
    }
    bool success = false;
    if (ensureSession(settings, false) &&
        temperatureRequestCurrent(request, machineConfigGeneration)) {
      if (!commandAccepted) {
        commandAttempted = true;
        commandAccepted = writeTemperature(settings, request.targetDeciC);
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
      success = commandAccepted &&
                temperatureRequestCurrent(request, machineConfigGeneration) &&
                readDashboard(settings, verification) &&
                verification.targetValid &&
                verification.targetDeciC == request.targetDeciC;
    }
    if (success) {
      wipeLineaMicraSettings(settings);
      TaskLockGuard lock(mux_);
      if (desiredTemperature_.present &&
          sameCommandRequest(desiredTemperature_.request, request)) {
        desiredTemperature_ = {};
        published_.appliedTargetDeciC = request.targetDeciC;
        published_.temperatureState = LineaMicraTemperatureState::CONFIRMED;
        published_.temperatureError = LineaMicraError::NONE;
        published_.temperatureTransportStatus = work_->transportStatus;
        published_.temperatureHttpStatus = work_->httpStatus;
        published_.temperatureCommandAccepted = true;
        published_.temperatureRetryable = false;
      }
      return true;
    }

    if (!temperatureRequestCurrent(request, machineConfigGeneration)) {
      wipeLineaMicraSettings(settings);
      return true;
    }
    if (!temperatureEligible(gateError)) {
      wipeLineaMicraSettings(settings);
      deferTemperature(request, gateError,
                       micra_timing::kGateRetryMs);
      return false;
    }
    if (work_->httpStatus == 401) {
      tokenAvailable_.store(false, std::memory_order_release);
      secureWipe(work_->accessToken, sizeof(work_->accessToken));
      work_->accessTokenIssuedAtMs = 0;
    }
    if (!commandAccepted && commandAttempted &&
        commandTransportStatus == ESP_OK && commandHttpStatus != 0 &&
        !lineaMicraHttpRetryable(commandHttpStatus)) {
      break;
    }
    if (attempt + 1U >= micra_timing::kMaxAttempts) break;
    waitRetry(micra_timing::kRetryDelaysMs[attempt]);
  }

  wipeLineaMicraSettings(settings);
  const uint16_t failureHttpStatus =
      commandAttempted ? commandHttpStatus : work_->httpStatus;
  const LineaMicraError error =
      commandAccepted
          ? LineaMicraError::UNCONFIRMED
          : failureHttpStatus == 401
                ? LineaMicraError::INVALID_AUTH
                : work_->transportFailure
                      ? LineaMicraError::TRANSPORT
                      : failureHttpStatus != 0 &&
                                !lineaMicraHttpRetryable(
                                    failureHttpStatus)
                            ? LineaMicraError::REJECTED
                            : LineaMicraError::HTTP_ERROR;
  deferTemperature(request, error, micra_timing::kExhaustedCooldownMs,
                   lineaMicraTemperatureCycleRetryable(error), true);
  return false;
}

bool OpenBrewByWeightMicraService::executePowerApplication(
    const LineaMicraRequest &request,
    uint32_t machineConfigGeneration) {
  LineaMicraPersistedSettings settings;
  bool commandAccepted = false;
  const bool standby = request.type == LineaMicraRequestType::SET_STANDBY;
  const LineaMicraPowerState wantedState =
      standby ? LineaMicraPowerState::OFF : LineaMicraPowerState::ON;
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
    const LineaMicraStatus current = statusLocked();
    const bool offline = observationFence_.offline;
    if (offline || (current.connectionFreshness &&
        current.quality == LineaMicraObservationQuality::CURRENT &&
        observationFence_.powerTrusted && current.powerState == wantedState)) {
      desiredPower_ = {};
      lock.unlock();  // logging takes its own locks; never nest them in mux_
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                           offline ? "Micra offline, power command dropped"
                           : standby ? "Micra already in StandBy, command skipped"
                                     : "Micra already in BrewingMode, command skipped");
      return !offline;
    }
    settings = config_;
    commandAccepted = desiredPower_.commandAccepted;
  }
  if (!ensureWorkBuffer()) {
    wipeLineaMicraSettings(settings);
    deferPower(request, LineaMicraError::HTTP_ERROR,
                  micra_timing::kExhaustedCooldownMs);
    return false;
  }

  bool commandAttempted = false;
  int32_t commandTransportStatus = 0;
  uint16_t commandHttpStatus = 0;
  for (size_t attempt = 0; attempt < micra_timing::kMaxAttempts; ++attempt) {
    if (!powerRequestCurrent(request, machineConfigGeneration)) {
      wipeLineaMicraSettings(settings);
      return true;
    }
    // A shot or rinse (closed relay) cancels the command outright; the
    // machine power state is never changed around an active cycle.
    if (shotActive_.load(std::memory_order_acquire) || outboundShotActive.load(std::memory_order_acquire)) {
      wipeLineaMicraSettings(settings);
      deferPower(request, LineaMicraError::CANCELED, 0, false);
      return false;
    }
    LineaMicraError gateError = LineaMicraError::NONE;
    if (scaleConnecting_.load(std::memory_order_acquire)) {
      gateError = LineaMicraError::CANCELED;
    } else {
      (void)networkEligible(gateError);
    }
    if (gateError != LineaMicraError::NONE) {
      wipeLineaMicraSettings(settings);
      deferPower(request, gateError,
                    gateError == LineaMicraError::CANCELED
                        ? micra_timing::kGateRetryMs
                        : micra_timing::kExhaustedCooldownMs);
      return false;
    }

    LineaMicraStatus verification;
    {
      TaskLockGuard lock(mux_);
      verification = published_;
    }
    bool success = false;
    if (ensureSession(settings, false) &&
        powerRequestCurrent(request, machineConfigGeneration)) {
      if (!commandAccepted) {
        commandAttempted = true;
        commandAccepted =
            standby ? writeStandby(settings) : writePowerOn(settings);
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
                (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0;
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
            }
          }
        }
      }
      success = commandAccepted &&
                powerRequestCurrent(request, machineConfigGeneration) &&
                readDashboard(settings, verification) &&
                verification.powerState == wantedState;
    }
    if (success) {
      wipeLineaMicraSettings(settings);
      TaskLockGuard lock(mux_);
      if (desiredPower_.present &&
          sameCommandRequest(desiredPower_.request, request)) {
        desiredPower_ = {};
      }
      lock.unlock();  // logging takes its own locks; never nest them in mux_
      serialTraceCategoryf(
          LogLevel::INFO, DebugCategory::NETWORK,
          standby
              ? "Micra scale shutdown confirmed: machine is in StandBy"
              : "Micra scale power-on confirmed: machine is in BrewingMode");
      return true;
    }

    if (!powerRequestCurrent(request, machineConfigGeneration)) {
      wipeLineaMicraSettings(settings);
      return true;
    }
    if (work_->httpStatus == 401) {
      tokenAvailable_.store(false, std::memory_order_release);
      secureWipe(work_->accessToken, sizeof(work_->accessToken));
      work_->accessTokenIssuedAtMs = 0;
    }
    if (!commandAccepted && commandAttempted &&
        commandTransportStatus == ESP_OK && commandHttpStatus != 0 &&
        !lineaMicraHttpRetryable(commandHttpStatus)) {
      break;
    }
    if (attempt + 1U >= micra_timing::kMaxAttempts) break;
    waitRetry(micra_timing::kRetryDelaysMs[attempt]);
  }

  wipeLineaMicraSettings(settings);
  const uint16_t failureHttpStatus =
      commandAttempted ? commandHttpStatus : work_->httpStatus;
  const LineaMicraError error =
      commandAccepted
          ? LineaMicraError::UNCONFIRMED
          : failureHttpStatus == 401
                ? LineaMicraError::INVALID_AUTH
                : work_->transportFailure
                      ? LineaMicraError::TRANSPORT
                      : failureHttpStatus != 0 &&
                                !lineaMicraHttpRetryable(
                                    failureHttpStatus)
                            ? LineaMicraError::REJECTED
                            : LineaMicraError::HTTP_ERROR;
  deferPower(request, error, micra_timing::kExhaustedCooldownMs,
                lineaMicraTemperatureCycleRetryable(error), true);
  return false;
}

bool OpenBrewByWeightMicraService::executeConnect(PendingRequest &pending) {
  LineaMicraStatus status;
  {
    TaskLockGuard lock(mux_);
    status = published_;
  }
  status.requestId = pending.request.requestId;
  status.identityGeneration = pending.identityGeneration;
  status.phase = LineaMicraPhase::AUTHENTICATING;
  status.error = LineaMicraError::NONE;
  publish(status);
  if (!identityCurrent(pending.identityGeneration)) return true;
  if (!generateInstallationKey(pending.credentials)) {
    fail(status, LineaMicraError::HTTP_ERROR);
    return false;
  }
  if (!ensureSession(pending.credentials, true)) {
    LineaMicraError error = LineaMicraError::NONE;
    if (networkEligible(error)) {
      error = classifyFailure();
    }
    fail(status, error);
    return false;
  }
  if (!identityCurrent(pending.identityGeneration)) return true;
  status.phase = LineaMicraPhase::LISTING;
  publish(status);
  LineaMicraDiscoverySnapshot found;
  found.requestId = pending.request.requestId;
  if (!listMachines(pending.credentials, found)) {
    LineaMicraError error = LineaMicraError::NONE;
    if (networkEligible(error)) {
      error = classifyFailure();
    }
    fail(status, error);
    return false;
  }
  if (!identityCurrent(pending.identityGeneration)) return true;
  if (found.count == 0) {
    fail(status, LineaMicraError::NO_MACHINES);
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
  status.phase = LineaMicraPhase::CONFIRMED;
  status.error = LineaMicraError::NONE;
  status.quality = LineaMicraObservationQuality::UNCONFIGURED;
  publish(status);
  return true;
}

// Reuses the saved installation key and live session: unlike a connect it
// registers nothing and publishes no status, so observation keeps running.
void OpenBrewByWeightMicraService::executeRelist(PendingRequest &pending) {
  LineaMicraDiscoverySnapshot found;
  LineaMicraError error = LineaMicraError::NONE;
  // Same 3/6/9 s backoff as other cloud calls, so one dropped connection does
  // not leave the Change list empty.
  for (size_t attempt = 0; attempt < micra_timing::kMaxAttempts; ++attempt) {
    found = {};
    found.requestId = pending.request.requestId;
    error = LineaMicraError::NONE;
    if (!networkEligible(error) ||
        (ensureWorkBuffer() && ensureSession(pending.credentials, false) &&
         listMachines(pending.credentials, found))) {
      break;
    }
    error = classifyFailure();
    const uint16_t http = work_ == nullptr ? 0 : work_->httpStatus;
    if (attempt + 1U >= micra_timing::kMaxAttempts || http == 401 ||
        !lineaMicraHttpRetryable(http) ||
        abortRequested_.load(std::memory_order_acquire) ||
        !identityCurrent(pending.identityGeneration)) {
      break;
    }
    waitRetry(micra_timing::kRetryDelaysMs[attempt]);
  }
  if (error == LineaMicraError::NONE && found.count == 0) {
    error = LineaMicraError::NO_MACHINES;
  }
  TaskLockGuard lock(mux_);
  if (pending.identityGeneration != identityGeneration_) return;
  if (error == LineaMicraError::NONE) {
    candidate_ = pending.credentials;
    discovery_ = found;
    discovery_.accountMachines = found.count;
    ++discovery_.generation;
  }
  discovery_.listing = false;
  discovery_.error = error;
}

bool OpenBrewByWeightMicraService::executeObservation(PendingRequest &pending) {
  if (!observationCurrent(pending.identityGeneration)) return true;
  LineaMicraPersistedSettings settings;
  LineaMicraStatus status;
  {
    TaskLockGuard lock(mux_);
    settings = config_;
    status = published_;
  }
  status.requestId = pending.request.requestId;
  status.identityGeneration = pending.identityGeneration;
  status.phase = LineaMicraPhase::RUNNING;
  status.error = LineaMicraError::NONE;
  publish(status);
  bool success = false;
  bool sessionRenewed = false;
  // Session renewal must not consume a WS initialization or explicit read.
  const bool dashboardRequired = status.sampleAtMs == 0 ||
      settings.connectionType == static_cast<uint8_t>(MicraConnectionType::WEBSOCKET);
  for (size_t attempt = 0; attempt < micra_timing::kMaxAttempts; ++attempt) {
    if (pending.initialSnapshot) {
      TaskLockGuard lock(mux_);
      if (pending.snapshotStamp.epoch != observationFence_.epoch ||
          pending.snapshotStamp.connectionRevision != observationFence_.connectionRevision ||
          !websocketStatus_.subscribed || observationFence_.offline) {
        if (published_.requestId == pending.request.requestId)
          published_.phase = LineaMicraPhase::IDLE;
        wipeLineaMicraSettings(settings);
        return true;
      }
    }
    if (!observationCurrent(pending.identityGeneration)) {
      wipeLineaMicraSettings(settings);
      return true;
    }
    if (shotTransportPaused()) {
      wipeLineaMicraSettings(settings);
      deferObservation(pending, status, LineaMicraError::CANCELED);
      return true;
    }
    LineaMicraError gateError = LineaMicraError::NONE;
    if (!networkEligible(gateError)) {
      wipeLineaMicraSettings(settings);
      deferObservation(pending, status, gateError);
      return true;
    }
    success = ensureSession(settings, false, &sessionRenewed) &&
              ((sessionRenewed && !dashboardRequired) ||
               readDashboard(settings, status,
                   pending.initialSnapshot ? &pending.snapshotStamp : nullptr));
    if (success) break;
    if (!observationCurrent(pending.identityGeneration)) {
      wipeLineaMicraSettings(settings);
      return true;
    }
    if (abortRequested_.load(std::memory_order_acquire)) {
      wipeLineaMicraSettings(settings);
      deferObservation(pending, status, LineaMicraError::CANCELED);
      return true;
    }
    if (shotTransportPaused()) continue;
    if (!networkEligible(gateError)) {
      wipeLineaMicraSettings(settings);
      deferObservation(pending, status, gateError);
      return true;
    }
    if (work_ != nullptr && work_->httpStatus == 401) {
      tokenAvailable_.store(false, std::memory_order_release);
      secureWipe(work_->accessToken, sizeof(work_->accessToken));
      work_->accessTokenIssuedAtMs = 0;
    }
    if (attempt + 1U >= micra_timing::kMaxAttempts) break;
    status.phase = LineaMicraPhase::BACKOFF;
    publish(status);
    waitRetry(micra_timing::kRetryDelaysMs[attempt]);
    status.phase = LineaMicraPhase::RUNNING;
    publish(status);
  }
  wipeLineaMicraSettings(settings);
  if (!observationCurrent(pending.identityGeneration)) return true;
  if (!success) {
    LineaMicraError error = LineaMicraError::NONE;
    if (networkEligible(error)) {
      error = classifyFailure();
    }
    fail(status, error);
    return false;
  }
  if (sessionRenewed && !dashboardRequired) {
    status.phase = LineaMicraPhase::IDLE;
    status.error = LineaMicraError::NONE;
    publish(status);
    scheduleAutomatic(millis(), false);
    return true;
  }
  status.phase = LineaMicraPhase::CONFIRMED;
  status.error = LineaMicraError::NONE;
  status.quality = status.observedMode == LineaMicraObservedMode::UNSUPPORTED
                       ? LineaMicraObservationQuality::UNSUPPORTED
                       : LineaMicraObservationQuality::CURRENT;
  publish(status);
  scheduleAutomatic(status.sampleAtMs, false);
  return true;
}

void OpenBrewByWeightMicraService::publish(const LineaMicraStatus &status) {
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

void OpenBrewByWeightMicraService::deferObservation(
    const PendingRequest &pending, LineaMicraStatus status,
    LineaMicraError reason) {
  TaskLockGuard lock(mux_);
  if (pending.identityGeneration != identityGeneration_ ||
      !config_.accountConfigured ||
      (config_.options & LINEA_MICRA_OBSERVE_STATE) == 0) {
    return;
  }
  if (!pending_.present) pending_ = pending;
  pending_.present = true;
  if (reason == LineaMicraError::CANCELED)
    powerState_.hold(published_, true, millis());
  status.phase = reason == LineaMicraError::CANCELED
                     ? LineaMicraPhase::PAUSED
                     : LineaMicraPhase::QUEUED;
  status.error = LineaMicraError::NONE;
  status.shotPaused = reason == LineaMicraError::CANCELED;
  published_.phase = status.phase;
  published_.error = status.error;
  published_.shotPaused = status.shotPaused;
  published_.accountConfigured = config_.accountConfigured;
}

LineaMicraError OpenBrewByWeightMicraService::classifyFailure() const {
  if (work_ != nullptr) {
    if (work_->httpStatus == 401) return LineaMicraError::INVALID_AUTH;
    if (work_->transportFailure) return LineaMicraError::TRANSPORT;
  }
  return LineaMicraError::HTTP_ERROR;
}

void OpenBrewByWeightMicraService::fail(LineaMicraStatus &status,
                                   LineaMicraError error) {
  const LineaMicraPhase failedStage = status.phase;
  status.phase = LineaMicraPhase::FAILED;
  status.error = error;
  status.quality = LineaMicraObservationQuality::COMMUNICATION_ERROR;
  status.powerState = LineaMicraPowerState::UNKNOWN;
  status.effectiveOn = true;
  if (work_ != nullptr) {
    status.httpStatus = work_->httpStatus;
    status.transportStatus = work_->transportStatus;
  }
  publish(status);
  {
    TaskLockGuard lock(mux_);
    if (status.identityGeneration == identityGeneration_ && !powerState_.retained() &&
        config_.connectionType == static_cast<uint8_t>(MicraConnectionType::API)) {
      published_.quality = status.quality;
      published_.powerState = status.powerState;
      mirrorPowerLocked();
      published_.effectiveOn = status.effectiveOn;
    }
  }
  serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                       "Micra cloud request failed stage=%s error=%s http=%u raw=%ld",
                       lineaMicraPhaseName(failedStage),
                       lineaMicraErrorName(error),
                       static_cast<unsigned>(status.httpStatus),
                       static_cast<long>(status.transportStatus));
}

void OpenBrewByWeightMicraService::scheduleAutomatic(uint32_t now, bool failed) {
  TaskLockGuard lock(mux_);
  // Rejected credentials latch like the WebSocket lane: retrying would only
  // repeat sign-ins until the vendor locks the account. An account edit or a
  // manual refresh re-arms observation.
  if (config_.connectionType == static_cast<uint8_t>(MicraConnectionType::WEBSOCKET) ||
      published_.error == LineaMicraError::INVALID_AUTH) {
    observationSchedule_.suspendPeriodic();
  } else if (failed) {
    observationSchedule_.dueNow(now + micra_timing::kExhaustedCooldownMs);
  } else {
    observationSchedule_.scheduleNext(now);
  }
}

void OpenBrewByWeightMicraService::waitRetry(uint32_t delayMs) {
  const uint32_t due = millis() + delayMs;
  while (!micra_timing::deadlineReached(millis(), due)) {
    LineaMicraError error;
    if (!networkEligible(error) || abortRequested_.load(std::memory_order_acquire)) {
      if (!websocketAdmitted()) stopWebSocket();
      return;
    }
    const uint32_t now = millis();
    if (!micra_timing::deadlineReached(now, due))
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::min(uint32_t(250), due - now)));
  }
}

bool OpenBrewByWeightMicraService::generateInstallationKey(
    LineaMicraPersistedSettings &settings) {
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

bool OpenBrewByWeightMicraService::ensureSession(
    LineaMicraPersistedSettings &settings, bool registerKey, bool *renewed) {
  if (renewed != nullptr) *renewed = false;
  if (work_ == nullptr) return false;
  if (registerKey) {
    clearSession();
    return registerInstallation(settings) && signIn(settings);
  }
  const uint32_t age = millis() - work_->accessTokenIssuedAtMs;
  if (work_->accessToken[0] != '\0' &&
      !micra_timing::accessTokenRefreshDue(age)) return true;
  tokenAvailable_.store(false, std::memory_order_release);
  const bool ok = work_->refreshToken[0] != '\0'
                      ? refreshToken(settings) || signIn(settings)
                      : signIn(settings);
  if (ok && renewed != nullptr) *renewed = true;
  return ok;
}

bool OpenBrewByWeightMicraService::registerInstallation(
    const LineaMicraPersistedSettings &settings) {
  if (!ensureIoBuffer()) return false;
  const bool ok =
      request(settings, kRegisterUrl, HTTP_METHOD_POST, nullptr, false,
              "register_installation", "/auth/init", true);
  releaseIoBuffer();
  return ok;
}

bool OpenBrewByWeightMicraService::signIn(
    const LineaMicraPersistedSettings &settings) {
  if (!ensureIoBuffer()) return false;
  char username[2 * LINEA_MICRA_USERNAME_CAPACITY];
  char password[2 * LINEA_MICRA_PASSWORD_CAPACITY];
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
  if (!request(settings, kSignInUrl, HTTP_METHOD_POST, io_->body, false,
               "sign_in", "/auth/signin")) {
    releaseIoBuffer();
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
  return true;
}

bool OpenBrewByWeightMicraService::refreshToken(
    const LineaMicraPersistedSettings &settings) {
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
  if (!request(settings, kRefreshUrl, HTTP_METHOD_POST, io_->body, false,
               "refresh_token", "/auth/refreshtoken")) {
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

bool OpenBrewByWeightMicraService::listMachines(
    const LineaMicraPersistedSettings &settings,
    LineaMicraDiscoverySnapshot &result) {
  if (!ensureIoBuffer()) return false;
  if (!request(settings, kThingsUrl, HTTP_METHOD_GET, nullptr, true,
               "list_machines", "/things")) {
    releaseIoBuffer();
    return false;
  }
  cJSON *root = parseResponse(io_->response);
  if (!cJSON_IsArray(root)) {
    if (root != nullptr) cJSON_Delete(root);
    work_->responseUsed = 0;
    releaseIoBuffer(false);
    return false;
  }
  const cJSON *thing = nullptr;
  cJSON_ArrayForEach(thing, root) {
    if (result.count >= LINEA_MICRA_MAX_ACCOUNT_MACHINES ||
        !cJSON_IsObject(thing)) {
      continue;
    }
    const char *type = jsonText(thing, "type");
    const char *model = jsonText(thing, "modelCode");
    const char *serial = jsonText(thing, "serialNumber");
    const char *name = jsonText(thing, "name");
    if (type == nullptr || strcmp(type, "CoffeeMachine") != 0 ||
        model == nullptr || strcmp(model, "LINEAMICRA") != 0 ||
        serial == nullptr || !validLineaMicraSerial(serial)) {
      continue;
    }
    LineaMicraMachineSummary &machine = result.machines[result.count];
    if (!secureCopy(machine.serial, sizeof(machine.serial), serial)) continue;
    copyMicraText(machine.name, sizeof(machine.name),
                  name == nullptr || name[0] == '\0' ? serial : name);
    ++result.count;
  }
  cJSON_Delete(root);
  work_->responseUsed = 0;
  releaseIoBuffer();
  return true;
}

bool OpenBrewByWeightMicraService::readDashboard(
    const LineaMicraPersistedSettings &settings, LineaMicraStatus &result,
    const MicraObservationStamp *snapshot) {
  if (!ensureIoBuffer()) return false;
  MicraObservation update;
  const uint32_t disconnectGeneration = disconnectGeneration_.load(std::memory_order_acquire);
  const uint32_t acquisitionGeneration = outboundAcquisitionGeneration.load(std::memory_order_acquire);
  {
    TaskLockGuard lock(mux_);
    update.stamp = snapshot ? *snapshot :
        observationFence_.stamp(identityGeneration_, powerState_.generation());
    update.stamp.intent = powerState_.generation();
    if (snapshot) update.source = MicraObservationSource::HTTP_INITIAL;
  }
  char url[sizeof(kApiRoot) + LINEA_MICRA_SERIAL_CAPACITY + 24];
  const int length = snprintf(url, sizeof(url), "%s/things/%s/dashboard",
                              kApiRoot, settings.selectedSerial);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(url) ||
      !request(settings, url, HTTP_METHOD_GET, nullptr, true,
               "read_dashboard", "/things/{serial}/dashboard")) {
    releaseIoBuffer();
    return false;
  }
  cJSON *root = parseResponse(io_->response);
  const bool decoded = decodeMicraDashboard(root, update) &&
      (update.powerPresent || (update.connectedPresent && !update.connected));
  cJSON_Delete(root);
  // A transport-level success with no observation sample means the payload
  // was rejected; the excerpt names the offending shape on the serial log.
  if (!decoded)
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "Micra dashboard decode rejected: %.96s",
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
  const bool observing = config_.accountConfigured &&
      (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0;
  const uint32_t onlineRevision = observationFence_.onlineRevision;
  if (!observationFence_.merge(published_, powerState_, update, observing, true)) return false;
  mirrorPowerLocked();
  if (observationFence_.onlineRevision != onlineRevision) {
    websocketStatus_.machineConnectedKnown = true;
    websocketStatus_.machineConnected = !observationFence_.offline;
  }
  if (observationFence_.offline || published_.powerState == LineaMicraPowerState::OFF)
    backflush_.invalidate();
  result = published_;
  return snapshot != nullptr || !observationFence_.offline;
}

bool OpenBrewByWeightMicraService::writeTemperature(
    const LineaMicraPersistedSettings &settings, uint16_t targetDeciC) {
  if (!ensureIoBuffer()) return false;
  char url[sizeof(kApiRoot) + LINEA_MICRA_SERIAL_CAPACITY + 80];
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
                  request(settings, url, HTTP_METHOD_POST, io_->body, true,
                          "set_boiler_temperature",
                          "/things/{serial}/command/"
                          "CoffeeMachineSettingCoffeeBoilerTargetTemperature");
  const bool accepted =
      ok || (work_->transportStatus == ESP_OK && work_->httpStatus >= 200 &&
             work_->httpStatus < 300);
  releaseIoBuffer();
  return accepted;
}

// Standby is the La Marzocco app's name for the machine power-off state; the
// cloud command is the mode change used by pylamarzocco's set_power(False).
bool OpenBrewByWeightMicraService::writeStandby(
    const LineaMicraPersistedSettings &settings) {
  if (!ensureIoBuffer()) return false;
  char url[sizeof(kApiRoot) + LINEA_MICRA_SERIAL_CAPACITY + 80];
  const int urlLength = snprintf(
      url, sizeof(url), "%s/things/%s/command/CoffeeMachineChangeMode",
      kApiRoot, settings.selectedSerial);
  work_->transportStatus = 0;
  work_->httpStatus = 0;
  const bool ok = urlLength > 0 &&
                  static_cast<size_t>(urlLength) < sizeof(url) &&
                  request(settings, url, HTTP_METHOD_POST,
                          "{\"mode\":\"StandBy\"}", true, "set_standby",
                          "/things/{serial}/command/CoffeeMachineChangeMode");
  releaseIoBuffer();
  return ok;
}

// The power-on counterpart sends the BrewingMode the dashboard reports while
// the machine is on; the same command pylamarzocco's set_power(True) uses.
bool OpenBrewByWeightMicraService::writePowerOn(
    const LineaMicraPersistedSettings &settings) {
  if (!ensureIoBuffer()) return false;
  char url[sizeof(kApiRoot) + LINEA_MICRA_SERIAL_CAPACITY + 80];
  const int urlLength = snprintf(
      url, sizeof(url), "%s/things/%s/command/CoffeeMachineChangeMode",
      kApiRoot, settings.selectedSerial);
  work_->transportStatus = 0;
  work_->httpStatus = 0;
  const bool ok = urlLength > 0 &&
                  static_cast<size_t>(urlLength) < sizeof(url) &&
                  request(settings, url, HTTP_METHOD_POST,
                          "{\"mode\":\"BrewingMode\"}", true, "set_power_on",
                          "/things/{serial}/command/CoffeeMachineChangeMode");
  releaseIoBuffer();
  return ok;
}

bool OpenBrewByWeightMicraService::applySignedHeaders(
    const LineaMicraPersistedSettings &settings, char *headers, size_t capacity) {
  MicraPublicIdentityCache &identity = work_->publicIdentity;
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

bool OpenBrewByWeightMicraService::request(
    const LineaMicraPersistedSettings &settings, const char *url,
    esp_http_client_method_t method, const char *body, bool authenticated,
    const char *purpose, const char *endpoint, bool installationInit) {
  pendingCloudCall_ = {};
  pendingCloudCall_.api = purpose;
  pendingCloudCall_.method = method == HTTP_METHOD_POST ? "POST" : "GET";
  pendingCloudCall_.startedAtMs = millis();
  const time_t now = time(nullptr);
  pendingCloudCall_.startedAtUtcSec =
      now >= 1700000000 ? static_cast<uint32_t>(now) : 0;
  if (work_ == nullptr || io_ == nullptr || url == nullptr) return false;
  pendingCloudCall_.clientReused = work_->client != nullptr;
  if (pendingCloudCall_.clientReused) {
    pendingCloudCall_.idleMs =
        pendingCloudCall_.startedAtMs - work_->lastRequestEndedAtMs;
    if (pendingCloudCall_.idleMs >= micra_timing::kHttpReuseIdleMs) {
      (void)esp_http_client_close(work_->client);
      pendingCloudCall_.clientReused = false;
    }
  }
  if (work_->client == nullptr) {
    esp_http_client_config_t config{};
    config.url = kApiRoot;
    config.timeout_ms = static_cast<int>(micra_timing::kHttpTimeoutMs);
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
       esp_http_client_set_post_field(work_->client, body, strlen(body)) !=
           ESP_OK)) {
    return false;
  }
  LineaMicraError gateError = LineaMicraError::NONE;
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
                       "Micra HTTP start id=%lu %s %s",
                       static_cast<unsigned long>(pendingCloudCall_.startedAtMs),
                       method == HTTP_METHOD_POST ? "POST" : "GET", endpoint);
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
        micra_timing::kHttpTimeoutMs) {
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
    if (canceled) completeOutboundAcquisitionPause(OutboundClient::MICRA_HTTP, millis());
  }
  if (work_->responseUsed < sizeof(io_->response)) {
    io_->response[work_->responseUsed] = '\0';
  }
  const bool ok = performed == ESP_OK && !work_->responseOverflow &&
                  work_->httpStatus >= 200 && work_->httpStatus < 300;
  pendingCloudCall_.httpStatus = work_->httpStatus;
  pendingCloudCall_.transportStatus = performed;
  pendingCloudCall_.result = micraCloudCallResult(
      canceled, work_->responseOverflow, ok, work_->httpStatus);
  if (installationInit) {
    secureWipe(io_->response, sizeof(io_->response));
    work_->responseUsed = 0;
  }
  return ok;
}

void OpenBrewByWeightMicraService::clearRequestState() {
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

bool OpenBrewByWeightMicraService::ensureWorkBuffer() {
  if (work_ != nullptr) return true;
  work_ = static_cast<WorkBuffer *>(
      allocExternal(sizeof(WorkBuffer), AllocationOwner::NETWORK));
  if (work_ == nullptr) return false;
  new (work_) WorkBuffer{};
  return true;
}

bool OpenBrewByWeightMicraService::ensureIoBuffer() {
  if (io_ != nullptr) return true;
  io_ = static_cast<IoBuffer *>(
      allocExternal(sizeof(IoBuffer), AllocationOwner::NETWORK));
  if (io_ == nullptr) return false;
  new (io_) IoBuffer{};
  return true;
}

void OpenBrewByWeightMicraService::clearSession() {
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

void OpenBrewByWeightMicraService::releaseIoBuffer(bool responseValid) {
  if (pendingCloudCall_.api != nullptr) {
    LineaMicraCloudCall call = pendingCloudCall_;
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
         lineaMicraHttpRetryable(call.httpStatus));
    serialTraceCategoryf(success ? LogLevel::INFO
                                : transient ? LogLevel::WARNING : LogLevel::ERROR,
                         DebugCategory::NETWORK,
                         "Micra id=%lu %s %s %s http=%u raw=%ld ms=%lu "
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

void OpenBrewByWeightMicraService::releaseWorkBuffer() {
  clearSession();
  if (work_ == nullptr) return;
  work_->~WorkBuffer();
  heapCapsFree(work_);
  work_ = nullptr;
}

esp_err_t OpenBrewByWeightMicraService::httpEvent(esp_http_client_event_t *event) {
  if (event == nullptr || event->user_data == nullptr) return ESP_FAIL;
  auto *self = static_cast<OpenBrewByWeightMicraService *>(event->user_data);
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
