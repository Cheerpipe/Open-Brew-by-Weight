#pragma once

// Boundary between the companion link (network side, docs/COMPANION_LINK.md)
// and the La Marzocco service (Linea Micra builds only). The link owns the
// socket, the pairing secret and the frame codec; the service owns the route,
// the observation fence and every command. Neither side blocks on the other:
// inbound calls take the service mutex briefly, and outbound frames are
// pulled by the link from a snapshot the service keeps current.

#include <cstddef>
#include <cstdint>

struct cJSON;

namespace openbrewbyweight {

enum class MicraCompanionStateKind : uint8_t { FULL, PUSH, READ };
enum class MicraCompanionCommandOp : uint8_t { NONE, TEMPERATURE, STANDBY, POWER_ON, REFRESH };
enum class MicraCompanionRoute : uint8_t { DIRECT, PENDING, COMPANION };

// What the service wants the link to send. The link resends the whole
// snapshot after every new authenticated session and sends a field again
// whenever its sequence changes.
struct MicraCompanionOutbox {
  uint32_t configSequence = 0;  // cfg frame
  bool lmSwitch = false;
  bool observe = false;
  bool websocketConnection = true;  // false: API polling
  uint32_t credentialsGeneration = 0;  // creds frame (0: nothing to send)
  bool credentialsErase = false;
  uint32_t routeSequence = 0;  // route frame
  bool routeCompanion = false;
  uint32_t holdSequence = 0;  // hold frame
  bool hold = false;
  uint32_t commandId = 0;  // cmd frame; 0 or an id already sent: none
  MicraCompanionCommandOp commandOp = MicraCompanionCommandOp::NONE;
  uint16_t commandValue = 0;  // tenths of a degree for TEMPERATURE
};

// Published by the service for Admin and Diagnostic.
struct MicraCompanionRouteStatus {
  MicraCompanionRoute route = MicraCompanionRoute::DIRECT;
  bool upstreamSubscribed = false;
  bool credentialsConfirmed = false;
  uint32_t upstreamEpoch = 0;
  uint32_t routeSinceMs = 0;
  uint32_t switches = 0;  // route changes since boot
  uint32_t commands = 0;  // commands sent through the companion since boot
  const char *reason = "none";  // why the route is direct
};

#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
// Link side → service. All may run on the HTTP or network task.
void machineIntegrationCompanionLink(bool ready, bool lmSwitch);
void machineIntegrationCompanionPing(bool upstreamAlive);
void machineIntegrationCompanionUpstream(bool subscribed, uint32_t epoch,
                                         uint32_t tokenRenewInS);
void machineIntegrationCompanionCredentials(uint32_t generation, bool ok);
// `dashboard` is the `d` object of an `st` frame; false rejects the frame
// (the link closes on a malformed frame, as the direct socket does).
bool machineIntegrationCompanionState(MicraCompanionStateKind kind,
                                      uint32_t epoch, uint32_t afterCommand,
                                      const cJSON *dashboard);
void machineIntegrationCompanionCommandResult(uint32_t id, bool accepted,
                                              uint16_t httpStatus);
// Service → link.
MicraCompanionOutbox machineIntegrationCompanionOutbox();
MicraCompanionRouteStatus machineIntegrationCompanionRouteStatus();
// Copies the account for the creds frame; the caller encrypts and wipes it.
bool machineIntegrationCompanionCopyCredentials(uint32_t generation, char *email,
                                                size_t emailCapacity, char *password,
                                                size_t passwordCapacity, char *serial,
                                                size_t serialCapacity);
// Wakes the link owner when the outbox changed (set by the network side).
void setMachineIntegrationCompanionNotify(void (*notify)());
#endif

}  // namespace openbrewbyweight
