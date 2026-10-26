#ifndef ScaleProtocol_h
#define ScaleProtocol_h

#include "ScaleFeatures.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SCALE_MAX_PACKET_LENGTH 20
#define SCALE_MAX_COMMAND_LENGTH 20
#define SCALE_MAX_WEIGHT_GRAMS 10000.0f
#define SCALE_CORE_FEATURES                                                    \
    (ScaleFeatureWeight | ScaleFeatureTare | ScaleFeatureStartTimer |          \
     ScaleFeatureStopTimer | ScaleFeatureResetTimer)

struct ScalePayload {
    const uint8_t *data;
    int length;
    const char *label;
};

struct ScaleProtocol {
    const char *id;
    const char *debugLabel;
    const char *readUuid;
    const char *writeUuid;
    const char *const *namePrefixes;
    size_t namePrefixCount;
    ScaleFeatureSet features;
    bool (*supportedPacketLength)(int length);
    // parseWeight is the frame's integrity check. parseTimer validates its own
    // frame unless ScaleFeatureTimerInWeightFrame is declared, in which case
    // it and parseFlowRate only see frames parseWeight already accepted and
    // may decode without re-checking them.
    bool (*parseWeight)(const uint8_t *data, int length, float *weight);
    bool (*parseTimer)(const uint8_t *data, int length, uint32_t *timerMs);
    // Signed grams per second from a frame parseWeight accepted; set only by
    // protocols that declare ScaleFeatureFlowRate.
    bool (*parseFlowRate)(const uint8_t *data, int length, float *gramsPerSecond);
    bool (*encodeCommand)(ScaleOp op, uint8_t arg, uint8_t *out, int *length);
    const ScalePayload *initWrites;
    size_t initWriteCount;
    bool requireAdvertisedName;
};

// Advertisement identity is a hint for display; command filtering also
// requires the selected Bookoo protocol on the current connection.
inline ScaleModel scaleModelForAdvertisement(const char *name) {
    if (name == nullptr) return ScaleModel::Unknown;
    const bool ultraPrefix = strncmp(name, "BOOKOO_SC_U ", 12) == 0;
    if (!ultraPrefix && strncmp(name, "BOOKOO_SC ", 10) != 0) {
        return ScaleModel::Unknown;
    }
    const char *serial = name + (ultraPrefix ? 12 : 10);
    const bool spacedUltra = !ultraPrefix && serial[0] == 'U' && serial[1] == ' ';
    if (spacedUltra) serial += 2;
    if (*serial == '\0') return ScaleModel::Unknown;
    for (const char *p = serial; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return ScaleModel::Unknown;
    }
    return ultraPrefix || spacedUltra ? ScaleModel::BookooUltra : ScaleModel::BookooMini;
}

// The raw-name fallback is borrowed from the caller; copy it if retained.
inline const char *scaleDefaultFriendlyName(const char *name) {
    switch (scaleModelForAdvertisement(name)) {
        case ScaleModel::BookooMini: return "BOOKOO Themis Mini";
        case ScaleModel::BookooUltra: return "BOOKOO Themis Ultra";
        default: return name == nullptr ? "" : name;
    }
}

inline const char *scaleModelName(ScaleModel model) {
    switch (model) {
        case ScaleModel::BookooMini: return "bookoo_mini";
        case ScaleModel::BookooUltra: return "bookoo_ultra";
        default: return "unknown";
    }
}

inline uint8_t scaleBookooOpcode(ScaleOp op) {
    switch (op) {
        case ScaleOp::Tare: return 0x01;
        case ScaleOp::SetVolume: return 0x02;
        case ScaleOp::StartTimer: return 0x04;
        case ScaleOp::StopTimer: return 0x05;
        case ScaleOp::ResetTimer: return 0x06;
        case ScaleOp::CombinedTareStart: return 0x07;
        case ScaleOp::PowerOff: return 0x15;
        default: return 0;
    }
}

struct ScaleCommandInfo {
    const char *name;
    uint8_t code;
};

inline bool scaleBookooCommandAt(ScaleModel model, size_t index,
                                 ScaleCommandInfo *out) {
    static const ScaleOp ops[] = {ScaleOp::Tare, ScaleOp::SetVolume,
                                  ScaleOp::StartTimer, ScaleOp::StopTimer,
                                  ScaleOp::ResetTimer, ScaleOp::CombinedTareStart,
                                  ScaleOp::PowerOff};
    static const char *const names[] = {"Tare", "Volume", "Start timer",
                                         "Stop timer", "Reset timer",
                                         "Tare and start timer", "Power off"};
    const size_t count = model == ScaleModel::BookooUltra ? 7 :
                         model == ScaleModel::BookooMini ? 6 : 0;
    if (out == nullptr || index >= count) return false;
    *out = {names[index], scaleBookooOpcode(ops[index])};
    return true;
}

const ScaleProtocol *scaleProtocolAt(size_t index);
size_t scaleProtocolCount();
bool scaleNameIsCompatible(const char *name);
bool scaleNameMatchesProtocol(const char *name, const ScaleProtocol *protocol);
bool scaleParseUuid16(const char *uuid, uint16_t *out);
bool scaleUuid16AllowsNamelessConnect(uint16_t uuid);

bool scaleValidWeight(float weight);
bool scaleCopyPayload(const uint8_t *command, int commandLength, uint8_t *out,
                      int *length);
uint8_t scaleXorBytes(const uint8_t *data, int length);
uint32_t scaleReadUint32LittleEndian(const uint8_t *data);
float scaleDecimalDivisor(uint8_t exponent);
bool scaleLooksLikeAcaiaTimer(uint8_t minutes, uint8_t seconds, uint8_t tenths);
uint32_t scaleAcaiaTimerToMs(uint8_t minutes, uint8_t seconds, uint8_t tenths);
bool scaleValidAcaiaChecksum(const uint8_t *data, int length);
bool scaleFelicitaAsciiTimer(const uint8_t *data, uint32_t *timerMs);

extern const ScaleProtocol kScaleProtocolAcaiaLegacy;
extern const ScaleProtocol kScaleProtocolAcaia;
extern const ScaleProtocol kScaleProtocolGenericFf11;
extern const ScaleProtocol kScaleProtocolFelicita;
extern const ScaleProtocol kScaleProtocolEclair;
extern const ScaleProtocol kScaleProtocolDecent;
extern const ScaleProtocol kScaleProtocolDifluid;
extern const ScaleProtocol kScaleProtocolMyscale;
extern const ScaleProtocol kScaleProtocolWeighMyBru;
extern const ScaleProtocol kScaleProtocolVaria;
extern const ScaleProtocol kScaleProtocolEureka;

inline ScaleFeatureSet scaleFeaturesForModel(const ScaleProtocol *protocol,
                                             ScaleModel model) {
    ScaleFeatureSet result = protocol == nullptr ? scaleFeatureSetNone()
                                                : protocol->features;
    if (protocol == &kScaleProtocolGenericFf11) {
        if (model == ScaleModel::BookooMini) {
            result.flags &= ~static_cast<uint32_t>(ScaleFeaturePowerOff);
        } else if (model == ScaleModel::BookooUltra) {
            result.volumeMax = 3;
        }
    }
    return result;
}

#endif
