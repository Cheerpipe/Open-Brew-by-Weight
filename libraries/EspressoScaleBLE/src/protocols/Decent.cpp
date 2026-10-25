#include "ScaleProtocol.h"

namespace {

static const uint8_t TARE_DECENT[7] =
    {0x03, 0x0f, 0x00, 0x00, 0x00, 0x01, 0x0d};
static const uint8_t HEARTBEAT_DECENT[7] =
    {0x03, 0x0a, 0x03, 0xff, 0xff, 0x00, 0x0a};

static const char *const kDecentPrefixes[] = {
    "Decent Scale",
    "EspressiScale"
};

static const ScaleFeatureSet kDecentFeatures = {
    ScaleFeatureWeight | ScaleFeatureTare | ScaleFeatureHeartbeat,
    0,
    0,
    // The scale drops a link after 5 s without a heartbeat; 80% leaves room
    // for worker tick and radio scheduling latency.
    4000,
    5000,
    0,
    380
};

bool decentSupportedPacketLength(int length) {
    return length == 7 || length == 10;
}

bool parseDecentWeight(const uint8_t *data, int length, float *weight) {
    if ((length != 7 && length != 10) || data[0] != 0x03 ||
        (data[1] != 0xca && data[1] != 0xce)) {
        return false;
    }
    // A zero check byte is accepted unverified, as in the original port;
    // the controller's slew filter and two-sample stop confirmation bound a
    // bad frame. Any other value must match the XOR.
    const uint8_t xorByte = data[length - 1];
    if (xorByte != 0 && scaleXorBytes(data, length - 1) != xorByte) {
        return false;
    }
    const int16_t raw = static_cast<int16_t>(
        (static_cast<uint16_t>(data[2]) << 8) | data[3]);
    *weight = static_cast<float>(raw) / 10.0f;
    return scaleValidWeight(*weight);
}

bool encodeDecentCommand(ScaleOp op, uint8_t arg, uint8_t *out, int *length) {
    (void)arg;
    switch (op) {
        case ScaleOp::Tare:
            return scaleCopyPayload(TARE_DECENT, sizeof(TARE_DECENT), out, length);
        case ScaleOp::Heartbeat:
            return scaleCopyPayload(HEARTBEAT_DECENT, sizeof(HEARTBEAT_DECENT), out,
                               length);
        default:
            break;
    }
    return false;
}

} // namespace

const ScaleProtocol kScaleProtocolDecent = {
    "decent",
    "Decent Scale detected",
    "fff4",
    "36f5",
    kDecentPrefixes,
    sizeof(kDecentPrefixes) / sizeof(kDecentPrefixes[0]),
    kDecentFeatures,
    &decentSupportedPacketLength,
    &parseDecentWeight,
    nullptr,
    nullptr,
    &encodeDecentCommand,
    nullptr,
    0,
    false
};
