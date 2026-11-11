#pragma once

#include "OpenBrewByWeightLaMarzoccoTypes.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightLaMarzoccoWebSocket.h"

namespace openbrewbyweight {

bool queueMachineIntegrationConnect(uint32_t requestId, const char *username,
                                    const char *password);
bool queueMachineIntegrationRelist(uint32_t requestId);
bool queueMachineIntegrationRequest(const LaMarzoccoRequest &request);
bool selectMachineIntegrationDevice(const char *serial,
                                    LaMarzoccoPersistedSettings &settings);
void clearMachineIntegrationDiscovery();
LaMarzoccoStatus machineIntegrationStatus();
LaMarzoccoCloudCall machineIntegrationCloudCall();
LaMarzoccoWebSocketStatus machineIntegrationWebSocketStatus(bool includeTraffic = true);
HeapLifecycleAggregate machineIntegrationHeapTelemetry();
LaMarzoccoDiscoverySnapshot machineIntegrationDiscovery();

}  // namespace openbrewbyweight
