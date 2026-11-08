#include "ScaleProtocol.h"

namespace {

static const uint8_t TARE_FELICITA[1] = {0x54};
static const uint8_t START_TIMER_FELICITA[1] = {0x52};
static const uint8_t STOP_TIMER_FELICITA[1] = {0x53};
static const uint8_t RESET_TIMER_FELICITA[1] = {0x43};
static const uint8_t WEIGHT_TIMER_MODE_FELICITA[1] = {0x32};

static const char *const kFelicitaPrefixes[] = {"FELIC"};

static const ScalePayload kFelicitaInitWrites[] = {
    {WEIGHT_TIMER_MODE_FELICITA,
     static_cast<int>(sizeof(WEIGHT_TIMER_MODE_FELICITA)), "weight_timer_mode"}
};

static const ScaleFeatureSet kFelicitaFeatures = {
    SCALE_CORE_FEATURES | ScaleFeatureCommandAudibleFeedback |
        ScaleFeatureTimerInWeightFrame | ScaleFeatureBatteryLevel,
    0,
    0,
    0,
    5000,
    0,
    500
};

bool felicitaSupportedPacketLength(int length) {
    return length == 18;
}

bool parseFelicitaWeight(const uint8_t *data, int length, float *weight) {
    if (length != 18 ||
        (data[2] != '-' && data[2] != '+' && data[2] != ' ' &&
         data[2] != 0x00)) {
        return false;
    }
    for (int i = 3; i <= 8; ++i) {
        if (data[i] < '0' || data[i] > '9') {
            return false;
        }
    }

    const uint32_t hundredths =
        static_cast<uint32_t>(data[3] - '0') * 100000UL +
        static_cast<uint32_t>(data[4] - '0') * 10000UL +
        static_cast<uint32_t>(data[5] - '0') * 1000UL +
        static_cast<uint32_t>(data[6] - '0') * 100UL +
        static_cast<uint32_t>(data[7] - '0') * 10UL +
        static_cast<uint32_t>(data[8] - '0');
    *weight = static_cast<float>(hundredths) / 100.0f;
    if (data[2] == '-') {
        *weight = -*weight;
    }
    return scaleValidWeight(*weight);
}

// Decodes a frame parseWeight already verified (ScaleFeatureTimerInWeightFrame).
bool parseFelicitaTimer(const uint8_t *data, int length, uint32_t *timerMs) {
    if (length != 18) return false;
    if (scaleFelicitaAsciiTimer(data, timerMs)) {
        return true;
    }
    if (scaleLooksLikeAcaiaTimer(data[9], data[10], data[11])) {
        *timerMs = scaleAcaiaTimerToMs(data[9], data[10], data[11]);
        return true;
    }
    return false;
}

// Byte 15 of a frame parseWeight accepted is a raw battery level from about
// 129 (empty) to 158 (full). Values below the range are rejected; a value
// above it reads as full.
bool parseFelicitaBattery(const uint8_t *data, int length, bool weightFrame,
                          uint8_t *percent) {
    static const uint8_t kEmpty = 129, kFull = 158;
    if (!weightFrame || length != 18 || data[15] < kEmpty) return false;
    const unsigned raw = data[15] > kFull ? kFull : data[15];
    *percent = static_cast<uint8_t>(((raw - kEmpty) * 100U + (kFull - kEmpty) / 2) /
                                    (kFull - kEmpty));
    return true;
}

bool encodeFelicitaCommand(ScaleOp op, uint8_t arg, uint8_t *out, int *length) {
    (void)arg;
    switch (op) {
        case ScaleOp::Tare:
            return scaleCopyPayload(TARE_FELICITA, sizeof(TARE_FELICITA), out,
                               length);
        case ScaleOp::StartTimer:
            return scaleCopyPayload(START_TIMER_FELICITA, sizeof(START_TIMER_FELICITA),
                               out, length);
        case ScaleOp::StopTimer:
            return scaleCopyPayload(STOP_TIMER_FELICITA, sizeof(STOP_TIMER_FELICITA),
                               out, length);
        case ScaleOp::ResetTimer:
            return scaleCopyPayload(RESET_TIMER_FELICITA, sizeof(RESET_TIMER_FELICITA),
                               out, length);
        default:
            break;
    }
    return false;
}

} // namespace

const ScaleProtocol kScaleProtocolFelicita = {
    "felicita",
    "Felicita Arc detected",
    "ffe1",
    "ffe1",
    kFelicitaPrefixes,
    sizeof(kFelicitaPrefixes) / sizeof(kFelicitaPrefixes[0]),
    kFelicitaFeatures,
    &felicitaSupportedPacketLength,
    &parseFelicitaWeight,
    &parseFelicitaTimer,
    nullptr,
    &parseFelicitaBattery,
    &encodeFelicitaCommand,
    kFelicitaInitWrites,
    sizeof(kFelicitaInitWrites) / sizeof(kFelicitaInitWrites[0]),
    false,
    {nullptr, nullptr}
};
