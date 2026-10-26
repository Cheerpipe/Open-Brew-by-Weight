/*
  EspressoScaleBLE.h - backend-neutral BLE client facade for espresso scales.

  Maintainer: Felipe Urzúa <cheerpipe@gmail.com>
  https://github.com/Cheerpipe/Open-Brew-by-Weight

  Felicita Arc support by Pio Baettig and A-TWJ.
  Protocol-driver refactor for multi-vendor scales.

  Known limitation: weights are reported in grams only.
*/
#ifndef EspressoScaleBLE_h
#define EspressoScaleBLE_h

#define HEARTBEAT_PERIOD_MS              2750UL
#define FIRST_PACKET_TIMEOUT_MS          5000UL
#define MAX_PACKET_PERIOD_MS             5000UL
#define SCALE_SCAN_TIMEOUT_MS            3000UL
#define BLE_OPERATION_TIMEOUT_MS          1000UL
#define BLE_CONNECT_TIMEOUT_MS            2000UL
#define BLE_DISCOVER_TIMEOUT_MS           3000UL
#define SCALE_CONNECT_SETTLE_MS           0UL
#define SCALE_DISCONNECT_SILENCE_MS       3000UL
// GAP scan duty while discovering. Connecting and GATT-up paths never start
// a scan. Intervals avoid 20/60/100/120 ms advertising harmonics.
// Relaxed 25% (28.75/115 ms), Balanced 50% (31.25/62.5 ms), Aggressive 100% (20/20).
#define BLE_SCAN_RELAXED_INTERVAL         0x00B8
#define BLE_SCAN_RELAXED_WINDOW           0x002E
#define BLE_SCAN_BALANCED_INTERVAL        0x0064
#define BLE_SCAN_BALANCED_WINDOW          0x0032
#define BLE_SCAN_AGGRESSIVE_INTERVAL      0x0020
#define BLE_SCAN_AGGRESSIVE_WINDOW        0x0020
#define SCALE_CONNECT_BUDGET_MS         10000UL
#define MAX_BLE_PACKET_LENGTH           20
#define MAX_CONSECUTIVE_REJECTED_PACKETS 8U
#define SCALE_MAC_CAPACITY               18U
#define SCALE_NAME_CAPACITY              32U
#define SCALE_LINK_RSSI_UNAVAILABLE      127

#include "ScaleBleBackend.h"
#include "ScaleBleTypes.h"
#include "ScaleFeatures.h"
#include "ScaleProtocol.h"
#include <stddef.h>
#include <stdint.h>

enum class ScaleDisconnectReason : uint8_t {
    NONE,
    USER_REQUEST,
    SCAN_START_FAILED,
    SCAN_TIMEOUT,
    CONNECT_FAILED,
    DISCOVERY_FAILED,
    UNSUPPORTED_SCALE,
    SUBSCRIBE_FAILED,
    INITIALIZATION_WRITE_FAILED,
    REMOTE_DISCONNECTED,
    FIRST_PACKET_TIMEOUT,
    PACKET_TIMEOUT,
    INVALID_PACKET_STREAM,
    COMMAND_WRITE_FAILED,
    SUPERVISION_TIMEOUT,
    CONNECTION_FAILED_TO_ESTABLISH,
    RX_QUEUE_OVERFLOW,
    EVENT_QUEUE_OVERFLOW,
    HOST_RESET,
    OPERATION_TIMEOUT,
    MBUF_ALLOCATION_FAILED
};

struct ScaleWeightSample {
    float weightG = 0.0f;
    uint32_t receivedAtMs = 0;
    uint32_t captureSequence = 0;
    // The scale's own flow from the same frame; only with ScaleFeatureFlowRate.
    bool flowRateValid = false;
    float flowRateGps = 0.0f;
};

// A notification frame the receiving path dropped before it could become a
// weight sample. Reported for diagnostics only: the frame stays ineligible
// for control and never changes protocol state.
enum class ScaleFrameDropReason : uint8_t {
    Stale,
    UnsupportedLength,
    Undecodable
};

struct ScaleDroppedFrame {
    const uint8_t *data = nullptr;  // Borrowed; valid only during the callback.
    uint32_t generation = 0;
    uint32_t captureSequence = 0;
    uint32_t receivedAtMs = 0;
    uint16_t length = 0;
    ScaleFrameDropReason reason = ScaleFrameDropReason::Undecodable;
    bool weightDecoded = false;
    float weightG = 0.0f;
};

// Task-context observer invoked from newWeightAvailable() when a queued
// frame is dropped as stale, unsupported, or undecodable. For stale frames
// the library attempts one pure protocol decode so a decodable-but-rejected
// reading stays observable. Implementations must not block, allocate, or
// touch flash.
typedef void (*ScaleDroppedFrameObserver)(const ScaleDroppedFrame &frame);

class EspressoScaleBLE {
    public:
        explicit EspressoScaleBLE(bool debug);
        ~EspressoScaleBLE();

        EspressoScaleBLE(const EspressoScaleBLE&) = delete;
        EspressoScaleBLE& operator=(const EspressoScaleBLE&) = delete;

        bool init(const char *mac = nullptr);

        bool startScan(const char *mac = nullptr, bool forceRestart = false,
                       uint16_t interval = BLE_SCAN_BALANCED_INTERVAL,
                       uint16_t window = BLE_SCAN_BALANCED_WINDOW,
                       bool addressScan = false);
        bool pollScan();
        bool isScanning() const;
        bool isConnecting() const;

        void disconnect();

        ScaleCommandResult tare();
        ScaleCommandResult tare(const ScaleCommandAdmission *admission);
        ScaleCommandResult startTimer();
        ScaleCommandResult startTimer(const ScaleCommandAdmission *admission);
        ScaleCommandResult stopTimer();
        ScaleCommandResult stopTimer(const ScaleCommandAdmission *admission);
        ScaleCommandResult resetTimer();
        ScaleCommandResult resetTimer(const ScaleCommandAdmission *admission);
        ScaleCommandResult tareStartTimer();
        ScaleCommandResult tareStartTimer(const ScaleCommandAdmission *admission);
        bool supportsTareStartTimer() const;

        ScaleCommandResult beep();
        bool supportsIndependentBeep() const;
        bool supportsCommandFeedback() const;
        ScaleCommandResult beepWithoutStateChange();
        ScaleCommandResult beepWithoutStateChange(const ScaleCommandAdmission *admission);
        ScaleCommandResult setBeepLevel(uint8_t level);
        ScaleCommandResult setBeepLevel(uint8_t level, const ScaleCommandAdmission *admission);

        ScaleCommandResult heartbeat();
        // Powers the scale down when the protocol supports it; the link then
        // drops from the remote side.
        ScaleCommandResult powerOff();
        bool supportsPowerOff();
        float getWeight() const;
        ScaleWeightSample getWeightSample() const;
        uint32_t notificationSequence() const;
        bool hasTimer() const;
        uint32_t getTimerMs() const;
        uint32_t lastTimerAgeMs() const;
        bool heartbeatRequired() const;
        bool isConnected();
        bool isLinkUp() const;
        bool communicationSilenced() const;
        uint32_t communicationSilenceRemainingMs() const;
        bool newWeightAvailable();
        void setDroppedFrameObserver(ScaleDroppedFrameObserver observer);
        // Registered before scanning. Runs outside critical sections, possibly
        // on the NimBLE host task: publish atomics/notifications, never block.
        // It may repeat a value; the last call reports the current state.
        void setAcquisitionObserver(void (*observer)(bool, uint32_t));
        // Called on the NimBLE host task right after a weight frame is queued
        // in the RX ring: wake the consumer, never block or parse here.
        void setFrameQueuedObserver(void (*observer)());
        // Called on the consumer's task after every connection and accepted
        // update with the live interval and, when the clamp overrode a peer
        // request, the interval the scale asked for (0 otherwise); 1.25 ms units.
        void setLinkParamsObserver(void (*observer)(uint16_t intervalUnits,
                                                    uint16_t peerIntervalUnits));
        ScaleFeatureSet features() const;
        ScaleModel model() const;
        // Borrowed until the connection changes; copy if retained.
        const char* defaultFriendlyName() const;
        bool supportedCommandAt(size_t index, ScaleCommandInfo *out) const;
        const char* connectedProtocolName() const;
        const char* address() const;
        const char* localName() const;
        bool isDirectedScan() const;
        bool takeSeenAdvertisement(char *macOut, size_t macCapacity,
                                   char *nameOut, size_t nameCapacity);

        ScaleDisconnectReason lastDisconnectReason() const;
        const char* lastDisconnectReasonName() const;
        uint8_t connectAttemptCount() const;
        uint8_t connectStepId() const;
        uint32_t lastValidPacketAgeMs() const;
        uint32_t rejectedPacketCount() const;
        uint32_t reconnectCount() const;
        ScaleBleTimingSnapshot timingSnapshot() const;
        // Native NimBLE status for diagnostics. Preserve the last ble_hs/HCI
        // value without collapsing it into a domain reason.
        int32_t lastBackendStatus() const;
        ScaleBleDiagnostics diagnostics() const;
        ScaleBleBackendHealth backendHealth() const;
        int linkRssi();
};

#endif
