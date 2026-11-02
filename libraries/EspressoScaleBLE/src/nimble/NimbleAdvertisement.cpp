#include "nimble/NimbleAdvertisement.h"

#include "ScaleProtocol.h"

#include <string.h>

void nimbleAccumulateAdvertisementName(const uint8_t *value, size_t length,
                                       bool complete,
                                       NimbleAdvertisementData &out) {
  if (value == nullptr || length == 0 || (out.nameComplete && !complete)) {
    return;
  }
  size_t copyLength = length;
  if (copyLength >= sizeof(out.name)) {
    copyLength = sizeof(out.name) - 1;
  }
  memcpy(out.name, value, copyLength);
  out.name[copyLength] = '\0';
  out.namePresent = true;
  out.nameComplete = complete;
}

void nimbleAccumulateAdvertisementUuid16(uint16_t uuid,
                                         NimbleAdvertisementData &out) {
  if (scaleUuid16AllowsNamelessConnect(uuid)) {
    out.compatibleUuid16 = true;
  }
}

bool nimbleAdvertisementIsCompatible(const NimbleAdvertisementData &data) {
  return data.compatibleUuid16 ||
         (data.namePresent && scaleNameIsCompatible(data.name));
}
