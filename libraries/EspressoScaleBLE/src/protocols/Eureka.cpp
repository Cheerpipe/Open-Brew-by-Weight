#include "ScaleProtocol.h"

namespace {

static const uint8_t TARE_EUREKA[6] =
    {0xaa, 0x02, 0x31, 0x31, 0x00, 0x00};
static const uint8_t START_TIMER_EUREKA[6] =
    {0xaa, 0x02, 0x33, 0x33, 0x00, 0x00};
static const uint8_t STOP_TIMER_EUREKA[6] =
    {0xaa, 0x02, 0x34, 0x34, 0x00, 0x00};
static const uint8_t RESET_TIMER_EUREKA[6] =
    {0xaa, 0x02, 0x35, 0x35, 0x00, 0x00};

static const char *const kEurekaPrefixes[] = {"CFS-9002", "LSJ-001"};

static const ScaleFeatureSet kEurekaFeatures = {
    ScaleFeatureWeight | ScaleFeatureTare | ScaleFeatureStartTimer |
        ScaleFeatureStopTimer | ScaleFeatureResetTimer,
    0,
    0,
    0,
    5000,
    0,
    350
};

bool eurekaSupportedPacketLength(int length) {
    return length == 11;
}

bool parseEurekaWeight(const uint8_t *data, int length, float *weight) {
    if (length != 11 || data[0] != 0xaa || data[1] != 0x09 ||
        data[2] != 0x41 || data[6] > 1) {
        return false;
    }
    uint16_t raw = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[8]) << 8) | data[7]);
    *weight = static_cast<float>(raw) * 0.1f;
    if (data[6] != 0) {
        *weight = -*weight;
    }
    return scaleValidWeight(*weight);
}

bool encodeEurekaCommand(ScaleOp op, uint8_t arg, uint8_t *out, int *length) {
    (void)arg;
    switch (op) {
        case ScaleOp::Tare:
            return scaleCopyPayload(TARE_EUREKA, sizeof(TARE_EUREKA), out, length);
        case ScaleOp::StartTimer:
            return scaleCopyPayload(START_TIMER_EUREKA, sizeof(START_TIMER_EUREKA),
                               out, length);
        case ScaleOp::StopTimer:
            return scaleCopyPayload(STOP_TIMER_EUREKA, sizeof(STOP_TIMER_EUREKA),
                               out, length);
        case ScaleOp::ResetTimer:
            return scaleCopyPayload(RESET_TIMER_EUREKA, sizeof(RESET_TIMER_EUREKA),
                               out, length);
        default:
            break;
    }
    return false;
}

} // namespace

const ScaleProtocol kScaleProtocolEureka = {
    "eureka",
    "Eureka Precisa detected",
    "fff1",
    "fff2",
    kEurekaPrefixes,
    sizeof(kEurekaPrefixes) / sizeof(kEurekaPrefixes[0]),
    kEurekaFeatures,
    &eurekaSupportedPacketLength,
    &parseEurekaWeight,
    nullptr,
    nullptr,
    nullptr,
    &encodeEurekaCommand,
    nullptr,
    0,
    true
};
