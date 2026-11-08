#pragma once

// Home cards dismissed once for every browser and address: the firmware image
// and the rolled back image the owner acknowledged, and each preset's ignored
// grind advice, newest first. The network task owns it under dataMux_ and
// persists it as one blob; missing or invalid, nothing is dismissed. The
// Wi-Fi change result it dismisses lives in RAM (NetworkStatusSnapshot).

#include <cstdlib>

#include "OpenBrewByWeightPersistence.h"
#include "OpenBrewByWeightWebAccess.h"

namespace openbrewbyweight {

constexpr uint32_t CARD_DISMISSALS_MAGIC = 0x43445331U;  // "CDS1"
constexpr uint16_t CARD_DISMISSALS_VERSION = 1;
constexpr const char *CARD_DISMISSALS_KEY = "cardDismissals";
// The image digest prefix the status shows ("runningImage").
constexpr size_t CARD_IMAGE_CHARS = 16;
// "<preset>:<segment>:<advice>:<tier>", the Web UI's identity of one advice.
constexpr size_t CARD_GRIND_KEY_BYTES = 40;

struct CardDismissals {
  uint32_t magic = CARD_DISMISSALS_MAGIC;
  uint16_t version = CARD_DISMISSALS_VERSION;
  uint16_t structureSize = sizeof(CardDismissals);
  char image[CARD_IMAGE_CHARS + 1] = {};
  char rollback[CARD_IMAGE_CHARS + 1] = {};
  char grind[MAX_SHOT_PRESETS][CARD_GRIND_KEY_BYTES] = {};
  uint8_t reserved[2] = {};  // no padding: the checksum covers every byte
  uint32_t checksum = 0;
};
static_assert(sizeof(CardDismissals) == 368, "cardDismissals blob layout changed");

// Empty, or a lowercase hex digest prefix.
inline bool cardImageValid(const char *image) {
  const size_t length = strnlen(image, CARD_IMAGE_CHARS + 1);
  return (length == 0 || length == CARD_IMAGE_CHARS) &&
         strspn(image, "0123456789abcdef") == length;
}

// The preset an advice key names, 0 when the key is malformed. Lowercase
// words, digits, '_' and ':' only, so a key is JSON safe as it is.
inline unsigned cardGrindPreset(const char *key) {
  const size_t length = strnlen(key, CARD_GRIND_KEY_BYTES);
  if (length == CARD_GRIND_KEY_BYTES ||
      strspn(key, "0123456789abcdefghijklmnopqrstuvwxyz_:") != length) {
    return 0;
  }
  size_t colons = 0;
  for (size_t i = 0; i < length; ++i) colons += key[i] == ':';
  char *end = nullptr;
  const unsigned long preset = strtoul(key, &end, 10);
  return colons == 3 && end != key && *end == ':' && preset <= 255 ? preset : 0;
}

inline uint32_t cardDismissalsChecksum(const CardDismissals &dismissals) {
  return webAccessFnv(&dismissals, offsetof(CardDismissals, checksum));
}

inline bool validCardDismissals(const CardDismissals &dismissals) {
  if (dismissals.magic != CARD_DISMISSALS_MAGIC ||
      dismissals.version != CARD_DISMISSALS_VERSION ||
      dismissals.structureSize != sizeof(CardDismissals) ||
      dismissals.checksum != cardDismissalsChecksum(dismissals) ||
      !cardImageValid(dismissals.image) || !cardImageValid(dismissals.rollback)) {
    return false;
  }
  for (const char *key : dismissals.grind) {
    if (key[0] != '\0' && cardGrindPreset(key) == 0) return false;
  }
  return true;
}

// Acknowledges a valid image prefix; returns whether it changed.
template <size_t N>
inline bool cardDismissImage(char (&slot)[N], const char *image) {
  if (strcmp(slot, image) == 0) return false;
  const size_t length = strnlen(image, N - 1);
  memcpy(slot, image, length);
  slot[length] = '\0';
  return true;
}

// Ignores a valid advice key: it moves to the front, replacing that preset's
// older advice, or the oldest entry when every slot holds another preset (a
// deleted preset's entry ages out this way). Returns whether it changed.
inline bool cardIgnoreGrind(CardDismissals &dismissals, const char *key) {
  const unsigned preset = cardGrindPreset(key);
  size_t at = MAX_SHOT_PRESETS - 1;
  for (size_t i = 0; i < MAX_SHOT_PRESETS; ++i) {
    if (dismissals.grind[i][0] == '\0' || cardGrindPreset(dismissals.grind[i]) == preset) {
      at = i;
      break;
    }
  }
  if (at == 0 && strcmp(dismissals.grind[0], key) == 0) return false;
  memmove(dismissals.grind[1], dismissals.grind[0], at * CARD_GRIND_KEY_BYTES);
  strncpy(dismissals.grind[0], key, CARD_GRIND_KEY_BYTES);
  return true;
}

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                      \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
inline SettingsBlobLoad loadCardDismissals(CardDismissals &dismissals) {
  return loadSettingsBlob(NvsSubsystem::CARD_DISMISSALS, CARD_DISMISSALS_KEY, dismissals,
                          validCardDismissals);
}

inline bool saveCardDismissals(CardDismissals &dismissals) {
  dismissals.magic = CARD_DISMISSALS_MAGIC;
  dismissals.version = CARD_DISMISSALS_VERSION;
  dismissals.structureSize = sizeof(CardDismissals);
  dismissals.checksum = cardDismissalsChecksum(dismissals);
  return saveSettingsBlob(NvsSubsystem::CARD_DISMISSALS, CARD_DISMISSALS_KEY, dismissals);
}

inline bool eraseCardDismissals() {
  return eraseSettingsNvsKey(NvsSubsystem::CARD_DISMISSALS, CARD_DISMISSALS_KEY);
}
#endif

}  // namespace openbrewbyweight
