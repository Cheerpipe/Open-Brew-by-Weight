#pragma once

#include <stdbool.h>
#include <stdint.h>

enum class OpenBrewByWeightBleRuntimeState : uint8_t {
  Stopped,
  Starting,
  Ready,
  Unsynced,
  Failed,
  Stopping
};

struct OpenBrewByWeightBleHealth {
  OpenBrewByWeightBleRuntimeState state;
  int32_t lastError;
  int32_t lastResetReason;
  uint32_t syncGeneration;
  uint32_t resetCount;
  uint32_t hostTaskStackHighWaterBytes;
  uint32_t internalFreeBytes;
  uint32_t internalMinimumFreeBytes;
  uint32_t internalLargestBlockBytes;
  uint32_t psramFreeBytes;
  uint32_t psramMinimumFreeBytes;
  uint32_t psramLargestBlockBytes;
};

// Starts the single process-wide NimBLE host and waits for the sync callback.
// NVS must already be initialized by the firmware. Safe to call repeatedly.
bool openBrewByWeightBleRuntimeStart(uint32_t timeoutMs);

bool openBrewByWeightBleRuntimeReady();
uint8_t openBrewByWeightBleRuntimeOwnAddressType();
uint32_t openBrewByWeightBleRuntimeSyncGeneration();
// sampleMemory=false skips the heap walks; memory fields then keep the last
// sample (or the reset/sync capture).
OpenBrewByWeightBleHealth openBrewByWeightBleRuntimeHealth(bool sampleMemory = true);

// Intended for orderly reboot/shutdown only. Peripheral reconnects must never
// cycle the global host.
bool openBrewByWeightBleRuntimeStop(uint32_t timeoutMs);
