#include "ScaleProtocol.h"

namespace {

static const uint8_t GENERIC_PRODUCT = 0x03;
static const uint8_t GENERIC_TYPE = 0x0a;

static const char *const kGenericPrefixes[] = {"BOOKO"};

static const ScaleFeatureSet kGenericFeatures = {
    SCALE_CORE_FEATURES | ScaleFeatureCombinedTareStart |
        ScaleFeatureIndependentBeep | ScaleFeatureVolume |
        ScaleFeatureCommandAudibleFeedback | ScaleFeaturePowerOff |
        ScaleFeatureFlowRate | ScaleFeatureTimerInWeightFrame,
    0,
    5,
    0,
    8000,
    100,
    500
};

void fillGenericCommand(uint8_t out[6], uint8_t data1, uint8_t data2, uint8_t data3) {
    out[0] = GENERIC_PRODUCT;
    out[1] = GENERIC_TYPE;
    out[2] = data1;
    out[3] = data2;
    out[4] = data3;
    out[5] = static_cast<uint8_t>(out[0] ^ out[1] ^ out[2] ^ out[3] ^ out[4]);
}

bool genericSupportedPacketLength(int length) {
    return length == 20;
}

// Weight and flow share the sign convention; the spec leaves its bytes open.
bool genericSign(uint8_t sign) {
    return sign == '-' || sign == '+' || sign == ' ' || sign == 0x00;
}

bool parseGenericWeight(const uint8_t *data, int length, float *weight) {
    if (length != 20 || data[0] != 0x03 || data[1] != 0x0b ||
        !genericSign(data[6]) || scaleXorBytes(data, 19) != data[19]) {
        return false;
    }
    const uint32_t raw = (static_cast<uint32_t>(data[7]) << 16) |
                         (static_cast<uint32_t>(data[8]) << 8) |
                         data[9];
    *weight = static_cast<float>(raw) / 100.0f;
    if (data[6] == '-') {
        *weight = -*weight;
    }
    return scaleValidWeight(*weight);
}

// Timer and flow decode frames parseWeight already verified
// (ScaleFeatureTimerInWeightFrame); only the bounds are rechecked.
bool parseGenericTimer(const uint8_t *data, int length, uint32_t *timerMs) {
    if (length != 20) return false;
    *timerMs = (static_cast<uint32_t>(data[2]) << 16) |
               (static_cast<uint32_t>(data[3]) << 8) | data[4];
    return true;
}

// Byte 10 is the flow sign, bytes 11-12 the flow x100 (big endian).
bool parseGenericFlowRate(const uint8_t *data, int length, float *gramsPerSecond) {
    if (length != 20 || !genericSign(data[10])) return false;
    const float rate =
        static_cast<float>((static_cast<uint16_t>(data[11]) << 8) | data[12]) / 100.0f;
    *gramsPerSecond = data[10] == '-' ? -rate : rate;
    return true;
}

bool encodeGenericCommand(ScaleOp op, uint8_t arg, uint8_t *out, int *length) {
    const uint8_t code = scaleBookooOpcode(op);
    if (code == 0) return false;
    uint8_t command[6];
    fillGenericCommand(command, code, 0x00,
                       op == ScaleOp::SetVolume ? arg : 0);
    return scaleCopyPayload(command, sizeof(command), out, length);
}

} // namespace

const ScaleProtocol kScaleProtocolGenericFf11 = {
    "bookoo_generic",
    "Generic scale detected",
    "ff11",
    "ff12",
    kGenericPrefixes,
    sizeof(kGenericPrefixes) / sizeof(kGenericPrefixes[0]),
    kGenericFeatures,
    &genericSupportedPacketLength,
    &parseGenericWeight,
    &parseGenericTimer,
    &parseGenericFlowRate,
    &encodeGenericCommand,
    nullptr,
    0,
    false
};
