#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t NIMBLE_SCALE_ADV_NAME_CAPACITY = 32;

struct NimbleAdvertisementData {
  char name[NIMBLE_SCALE_ADV_NAME_CAPACITY];
  bool namePresent;
  bool nameComplete;
  bool compatibleUuid16;
};

// Accumulation primitives the native NimBLE adapter applies to the fields
// ble_hs_adv_parse_fields() returns. Repeated calls merge ADV and
// scan-response payloads for one address.
void nimbleAccumulateAdvertisementName(const uint8_t *value, size_t length,
                                       bool complete,
                                       NimbleAdvertisementData &out);
void nimbleAccumulateAdvertisementUuid16(uint16_t uuid,
                                         NimbleAdvertisementData &out);

bool nimbleAdvertisementIsCompatible(const NimbleAdvertisementData &data);
