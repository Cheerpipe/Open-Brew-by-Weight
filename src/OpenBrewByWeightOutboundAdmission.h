#pragma once
#include <atomic>
#include <cstdint>

namespace openbrewbyweight {
// Discovery owns this publication. Network owners consume it; none can clear it.
inline std::atomic<bool> outboundAcquisitionHeld{false};
// Control owns shot admission; rinse demotion clears it immediately.
inline std::atomic<bool> outboundShotActive{false};
// The scale worker owns live connection state, independently of shot history.
inline std::atomic<bool> outboundScaleConnected{false};
inline std::atomic<bool> outboundMaintenance{false};
inline std::atomic<bool> outboundScaleSetup{false};
inline std::atomic<bool> outboundBleQuiet{false};
inline bool outboundScaleInhibited() {
  return outboundAcquisitionHeld.load(std::memory_order_acquire) ||
      outboundScaleSetup.load(std::memory_order_acquire) ||
      outboundBleQuiet.load(std::memory_order_acquire);
}
inline std::atomic<uint32_t> outboundAcquisitionGeneration{0};
inline std::atomic<uint32_t> outboundAcquisitionAtMs{0};
enum class OutboundClient : uint8_t { LA_MARZOCCO_HTTP, WEBHOOK, NTP, COUNT };
struct OutboundPauseCompletion {
  std::atomic<uint32_t> generation{0}, latencyMs{0};
};
inline OutboundPauseCompletion outboundPauseCompletions[static_cast<unsigned>(OutboundClient::COUNT)];
// Each client owner reports only after its transport is actually quiescent.
// This does not certify resolver/RF silence; an already-started DNS query may finish.
inline void completeOutboundAcquisitionPause(OutboundClient client, uint32_t now) {
  if (!outboundAcquisitionHeld.load(std::memory_order_acquire)) return;
  auto &completion = outboundPauseCompletions[static_cast<unsigned>(client)];
  const uint32_t generation = outboundAcquisitionGeneration.load(std::memory_order_acquire);
  if (completion.generation.load(std::memory_order_acquire) == generation) return;
  completion.latencyMs.store(now - outboundAcquisitionAtMs.load(), std::memory_order_release);
  completion.generation.store(generation, std::memory_order_release);
}
inline void publishOutboundAcquisition(bool held, uint32_t now) {
  if (held) {
    if (outboundAcquisitionHeld.exchange(true, std::memory_order_acq_rel)) return;
  } else if (!outboundAcquisitionHeld.load(std::memory_order_acquire)) return;
  outboundAcquisitionAtMs.store(now, std::memory_order_release);
  outboundAcquisitionGeneration.fetch_add(1, std::memory_order_acq_rel);
  // Discovery is the single writer: fence old ingress before opening admission.
  if (!held) outboundAcquisitionHeld.store(false, std::memory_order_release);
}
}  // namespace openbrewbyweight
