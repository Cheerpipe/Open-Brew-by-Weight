#define OPEN_BREW_BY_WEIGHT_HOST_TEST

#include <stdint.h>

extern uint32_t hostSafetyResetReasonCode;
extern bool hostSafetyResetReasonUnsafe;
extern bool hostSafetyResetReasonPowerOn;

#include "../OpenBrewByWeightResetGuard.h"

const volatile void *resetGuardAddressA() {
  return &openbrewbyweight::safetyResetRecordForHost();
}

void resetGuardWriteFromA() {
  openbrewbyweight::initializeSafetyResetRecord(7);
}
