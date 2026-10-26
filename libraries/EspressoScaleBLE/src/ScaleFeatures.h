#ifndef ScaleFeatures_h
#define ScaleFeatures_h

#include <stdint.h>

enum ScaleFeature {
    ScaleFeatureWeight = 1u << 0,
    ScaleFeatureTare = 1u << 1,
    ScaleFeatureStartTimer = 1u << 2,
    ScaleFeatureStopTimer = 1u << 3,
    ScaleFeatureResetTimer = 1u << 4,
    ScaleFeatureCombinedTareStart = 1u << 5,
    ScaleFeatureIndependentBeep = 1u << 6,
    ScaleFeatureVolume = 1u << 7,
    ScaleFeatureCommandAudibleFeedback = 1u << 8,
    ScaleFeatureHeartbeat = 1u << 9,
    ScaleFeaturePowerOff = 1u << 10,
    // Weight frames also carry the scale's own flow rate (parseFlowRate).
    ScaleFeatureFlowRate = 1u << 11,
    // The timer rides inside the weight frame: parseTimer is called only on
    // frames parseWeight accepted and skips its own integrity check.
    ScaleFeatureTimerInWeightFrame = 1u << 12
};

struct ScaleFeatureSet {
    uint32_t flags;
    uint8_t volumeMin;
    uint8_t volumeMax;
    uint16_t heartbeatPeriodMs;
    uint16_t maxPacketSilenceMs;
    uint16_t minimumCommandIntervalMs;
    // Prior estimate of this protocol's weight-transport lag (reporting +
    // internal filtering); the brew-by-weight cut leads its flow by it.
    // Refine from FIRST_DROP telemetry.
    uint16_t sensorLagMs;

    bool has(ScaleFeature feature) const {
        return (flags & static_cast<uint32_t>(feature)) != 0;
    }
};

inline ScaleFeatureSet scaleFeatureSetNone() {
    return {};
}

enum class ScaleCommandResult : uint8_t {
    Ok,
    Unsupported,
    NotConnected,
    InvalidArgument,
    WriteFailed,
    Deferred
};

// Owner-only preflight after command spacing, outside all BLE locks. Returning
// false defers without writing. Tares also validate the returned RX boundary
// under the final radio-admission lock, so newer buffered data cannot pass it.
struct ScaleCommandAdmission {
    bool (*approve)(void *context, uint32_t &captureBoundary) = nullptr;
    void *context = nullptr;
};

inline bool scaleCommandOk(ScaleCommandResult result) {
    return result == ScaleCommandResult::Ok;
}

enum class ScaleOp : uint8_t {
    Tare,
    StartTimer,
    StopTimer,
    ResetTimer,
    CombinedTareStart,
    Heartbeat,
    SetVolume,
    PowerOff
};

enum class ScaleModel : uint8_t { Unknown, BookooMini, BookooUltra };

#endif
