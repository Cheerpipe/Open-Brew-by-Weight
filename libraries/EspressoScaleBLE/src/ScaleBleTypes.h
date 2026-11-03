#ifndef ScaleBleTypes_h
#define ScaleBleTypes_h

#include <stddef.h>
#include <stdint.h>

static const uint16_t SCALE_BLE_INVALID_CONNECTION_HANDLE = 0xffffU;

enum class ScaleBleDisconnectOrigin : uint8_t { None, Local, Gap, HostReset };

inline const char *scaleBleOriginName(ScaleBleDisconnectOrigin origin) {
    switch (origin) {
        case ScaleBleDisconnectOrigin::Local: return "local";
        case ScaleBleDisconnectOrigin::Gap: return "gap";
        case ScaleBleDisconnectOrigin::HostReset: return "host_reset";
        default: return "none";
    }
}

inline const char *scaleBleStatusDomain(int32_t status, ScaleBleDisconnectOrigin origin) {
    if (status == 0) return "none";
    if (status >= 0x100 && status < 0x200) return "att";
    if ((status >= 0x200 && status < 0x300) ||
        (origin == ScaleBleDisconnectOrigin::Gap && status < 0x100)) return "hci";
    if (status >= 0x300 && status < 0x400) return "l2cap";
    if (status >= 0x400 && status < 0x500) return "sm_local";
    if (status >= 0x500 && status < 0x600) return "sm_peer";
    if (status >= 0x600) return "hardware";
    return "host";
}

inline const char *scaleBleCommandName(uint8_t command) {
    switch (command) {
        case 0: return "tare";
        case 1: return "start_timer";
        case 2: return "stop_timer";
        case 3: return "reset_timer";
        case 4: return "tare_start_timer";
        case 5: return "heartbeat";
        case 6: return "volume";
        case 7: return "power_off";
        default: return "none";
    }
}

// Historical event records, owned by the scale task. Later commands and
// teardown errors must not overwrite the original disconnect status.
struct ScaleBleDiagnostics {
    uint32_t disconnectSequence = 0;
    uint32_t disconnectAtMs = 0;
    uint32_t disconnectGeneration = 0;
    int32_t disconnectStatus = 0;
    int32_t teardownStatus = 0;
    uint32_t disconnectCommandElapsedMs = 0;
    uint8_t disconnectReason = 0;
    uint8_t disconnectCommand = 0xff;
    ScaleBleDisconnectOrigin disconnectOrigin = ScaleBleDisconnectOrigin::None;
    uint32_t commandFailureSequence = 0;
    uint32_t commandFailureAtMs = 0;
    uint32_t commandElapsedMs = 0;
    int32_t commandStatus = 0;
    uint8_t command = 0xff;
    uint32_t silenceSequence = 0;
    uint32_t silenceStartedAtMs = 0;
    uint32_t silenceBlockedProcedures = 0;
    uint8_t silenceTrigger = 0;
};

enum class ScaleBleAddressType : uint8_t {
    Public = 0,
    Unknown = 0xff
};

struct ScaleBleAddress {
    uint8_t bytes[6];
    ScaleBleAddressType type;
};

enum class ScaleBleEventType : uint8_t {
    StartRequested,
    ScanStarted,
    CompatibleAdvertisement,
    ConnectIssued,
    Connected,
    DiscoveryComplete,
    SubscriptionComplete,
    InitializationComplete,
    Disconnected,
    OperationFailed,
    DeadlineExpired,
    BackoffExpired,
    StopRequested
};

// Backend-neutral event DTO. It owns no buffers and never allocates. Raw status
// preserves the NimBLE/HCI value for diagnosis instead of collapsing errors.
struct ScaleBleEvent {
    ScaleBleEventType type;
    uint32_t generation;
    uint32_t operationId;
    uint16_t connectionHandle;
    int32_t rawStatus;
};

enum ScaleBleTimingFlag : uint8_t {
    ScaleBleTimingScanStarted = 1U << 0,
    ScaleBleTimingFirstCompatibleAdvertisement = 1U << 1,
    ScaleBleTimingConnectIssued = 1U << 2,
    ScaleBleTimingReady = 1U << 3,
    ScaleBleTimingFirstWeight = 1U << 4
};

// Millisecond timestamps use unsigned subtraction and therefore remain valid
// across millis() wraparound. recordedFlags distinguishes a real zero timestamp
// from a milestone that has not happened.
struct ScaleBleTimingSnapshot {
    uint32_t firstCompatibleAdvertisementMs;
    uint32_t connectIssuedMs;
    uint32_t readyMs;
    uint32_t firstWeightMs;
    uint8_t recordedFlags;

    bool has(ScaleBleTimingFlag flag) const {
        return (recordedFlags & static_cast<uint8_t>(flag)) != 0;
    }
};

// Fixed-size native NimBLE diagnostics. The adapter fills every counter
// without exposing host types to the firmware.
struct ScaleBleBackendHealth {
    uint32_t generation;
    uint32_t operationId;
    uint32_t stateAgeMs;
    uint32_t advertisementsSeen;
    uint32_t compatibleAdvertisements;
    uint32_t discardedAdvertisements;
    uint32_t malformedAdvertisements;
    uint32_t negativeCacheHits;
    uint32_t negativeCacheInsertions;
    uint32_t scanStarts;
    uint32_t scanCancels;
    uint32_t scanRestarts;
    uint32_t connectAttempts;
    uint32_t connectionFailures;
    uint32_t discoveryFailures;
    uint32_t subscriptionFailures;
    uint32_t writeFailures;
    uint32_t staleCallbacks;
    uint32_t criticalEventDrops;
    uint32_t controlEventDrops;
    uint32_t rxDrops;
    uint32_t mbufFailures;
    uint32_t cleanupCount;
    uint32_t duplicateCleanups;
    uint32_t teardownFailures;
    // NimBLE callback contexts: starts refused because every context of the
    // domain was armed, and callbacks whose arg was no armed context.
    uint32_t callbackContextExhausted;
    uint32_t callbackContextViolations;
    uint32_t backoffCount;
    uint32_t lastAdvertisementToConnectMs;
    uint32_t lastAdvertisementToReadyMs;
    uint32_t lastAdvertisementToFirstWeightMs;
    uint32_t lastReadyToFirstWeightMs;
    uint16_t criticalEventHighWater;
    uint16_t controlEventHighWater;
    uint16_t rxHighWater;
    uint8_t negativeCacheEntries;
    uint8_t state;
    uint8_t backoffFailures;
    uint8_t scanContextsArmed;
    uint8_t linkContextsArmed;
    uint8_t gattContextsArmed;
};

#endif
