#pragma once

// Drip quick start: right after the drip is set (or in a fresh preset), the
// next three learning-eligible shots set the drip to their median at once
// when they agree within QUICK_START_SPREAD_CG; otherwise normal learning
// carries on. The record lives in its own NVS key, outside the settings blob;
// an absent record (fresh install, factory reset) arms every preset.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "OpenBrewByWeightPresets.h"

namespace openbrewbyweight {

constexpr uint8_t QUICK_START_SHOTS = 3;
constexpr int16_t QUICK_START_SPREAD_CG = 40;
constexpr uint32_t QUICK_START_MAGIC = 0x51535442;  // "BTSQ"
constexpr uint16_t QUICK_START_VERSION = 1;
constexpr const char *QUICK_START_KEY = "bbwQs";

enum class QuickStartState : uint8_t { OFF = 0, ARMED, APPLIED, DISAGREED };

struct QuickStartEntry {
  uint8_t presetId = 0;
  uint8_t state = 0;  // QuickStartState
  uint8_t count = 0;
  uint8_t reserved = 0;
  int16_t observedCg[QUICK_START_SHOTS] = {};
  int16_t setCg = 0;    // Drip the owner set; meaningful with setAtId.
  int16_t quickCg = 0;  // Drip the quick start chose.
  uint32_t setAtId = 0;    // First shot-log id after the set; 0 = no marker.
  uint32_t quickAtId = 0;  // Shot-log id of the deciding shot.
};

struct QuickStartRecord {
  uint32_t magic = QUICK_START_MAGIC;
  uint16_t version = QUICK_START_VERSION;
  uint16_t size = 0;
  QuickStartEntry entries[MAX_SHOT_PRESETS];
  uint32_t checksum = 0;
};
static_assert(sizeof(QuickStartRecord) == 204, "quick start record layout changed");

inline uint32_t quickStartChecksum(const QuickStartRecord &record) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < offsetof(QuickStartRecord, checksum); ++i) {
    hash = (hash ^ bytes[i]) * 16777619U;
  }
  return hash;
}

inline void finalizeQuickStart(QuickStartRecord &record) {
  record.magic = QUICK_START_MAGIC;
  record.version = QUICK_START_VERSION;
  record.size = sizeof(QuickStartRecord);
  record.checksum = quickStartChecksum(record);
}

inline bool validQuickStart(const QuickStartRecord &record) {
  return record.magic == QUICK_START_MAGIC &&
         record.version == QUICK_START_VERSION &&
         record.size == sizeof(QuickStartRecord) &&
         record.checksum == quickStartChecksum(record);
}

// The preset's entry; with create, a free slot or one of a deleted preset.
inline QuickStartEntry *quickStartEntry(QuickStartRecord &record,
                                        const ShotPresetBank &bank,
                                        uint8_t presetId, bool create) {
  for (QuickStartEntry &entry : record.entries)
    if (entry.presetId == presetId && presetId != 0) return &entry;
  if (!create) return nullptr;
  for (QuickStartEntry &entry : record.entries) {
    if (entry.presetId == 0 || findShotPresetIndex(bank, entry.presetId) < 0) {
      entry = QuickStartEntry{};
      entry.presetId = presetId;
      return &entry;
    }
  }
  return nullptr;
}

inline void armQuickStart(QuickStartRecord &record, const ShotPresetBank &bank,
                          uint8_t presetId, float dripG, uint32_t setAtId) {
  QuickStartEntry *entry = quickStartEntry(record, bank, presetId, true);
  if (entry == nullptr) return;
  *entry = QuickStartEntry{};
  entry->presetId = presetId;
  entry->state = static_cast<uint8_t>(QuickStartState::ARMED);
  entry->setCg = static_cast<int16_t>(dripG * 100.0f + 0.5f);
  entry->setAtId = setAtId;
}

// A fresh or factory-reset controller: every brew-by-weight preset quick
// starts from its factory drip (no "set" marker).
inline QuickStartRecord armedQuickStart(const ShotPresetBank &bank) {
  QuickStartRecord record;
  for (uint8_t i = 0; i < bank.count; ++i)
    if (bank.presets[i].brewByWeight)
      armQuickStart(record, bank, bank.presets[i].id,
                    bank.presets[i].bbwEwmaOffsetG, 0);
  return record;
}

// Boot: fresh settings or no valid stored record (fresh install, factory
// reset) quick starts every factory preset and returns true (the record must
// be saved); otherwise the stored record is kept.
inline bool bootQuickStart(QuickStartRecord &out, bool freshSettings,
                           const ShotPresetBank &bank,
                           const QuickStartRecord *stored) {
  const bool arm = freshSettings || stored == nullptr;
  out = arm ? armedQuickStart(bank) : *stored;
  return arm;
}

// Records one eligible observation (offset used + final − cut line, raw) of an
// armed preset and returns true; jumpCg becomes the median when the third one
// agrees with the other two (the deciding shot then skips its EWMA step).
inline bool observeQuickStart(QuickStartRecord &record, const ShotPresetBank &bank,
                              uint8_t presetId, int16_t observedCg,
                              uint32_t recordId, int16_t &jumpCg) {
  QuickStartEntry *entry = quickStartEntry(record, bank, presetId, false);
  if (entry == nullptr || entry->count >= QUICK_START_SHOTS ||
      entry->state != static_cast<uint8_t>(QuickStartState::ARMED)) return false;
  entry->observedCg[entry->count++] = observedCg;
  if (entry->count < QUICK_START_SHOTS) return true;
  const int16_t *o = entry->observedCg;
  const int16_t lo = o[0] < o[1] ? (o[0] < o[2] ? o[0] : o[2]) : (o[1] < o[2] ? o[1] : o[2]);
  const int16_t hi = o[0] > o[1] ? (o[0] > o[2] ? o[0] : o[2]) : (o[1] > o[2] ? o[1] : o[2]);
  entry->quickAtId = recordId;
  if (hi - lo > QUICK_START_SPREAD_CG) {
    entry->state = static_cast<uint8_t>(QuickStartState::DISAGREED);
    return true;
  }
  const int16_t median = static_cast<int16_t>(o[0] + o[1] + o[2] - lo - hi);
  const int16_t maxCg = static_cast<int16_t>(MAX_OFFSET_G * 100.0f);
  jumpCg = median < 0 ? 0 : median > maxCg ? maxCg : median;
  entry->quickCg = jumpCg;
  entry->state = static_cast<uint8_t>(QuickStartState::APPLIED);
  return true;
}

// The drip chart fields of the settings status: quick start, marker positions
// (-1 = none), the newest needed drips (null = no computable value) and the
// bitmask of shots that did not train the drip (bit i matches bbwRecentG[i];
// skipped shots with a value draw hatched in the chart). Returns the length
// written, or 0 when the buffer is too small.
inline size_t formatDripChartJson(char *out, size_t cap, const ControlStatusFields &c) {
  static const char *const states[] = {"off", "armed", "applied", "disagreed"};
  size_t used = 0;
  const auto put = [&](int n) {
    if (n < 0 || used + static_cast<size_t>(n) >= cap) used = cap;
    else used += static_cast<size_t>(n);
  };
  put(snprintf(out, cap,
               ",\"bbwQuickStart\":{\"state\":\"%s\",\"count\":%u,\"setG\":%.2f,"
               "\"quickG\":%.2f},\"bbwRecentAfterSet\":%d,\"bbwRecentAfterQuick\":%d,"
               "\"bbwRecentG\":[",
               states[c.bbwQuickState < 4 ? c.bbwQuickState : 0], c.bbwQuickCount,
               c.bbwQuickSetCg / 100.0, c.bbwQuickCg / 100.0,
               c.bbwRecentAfterSet == 0xFF ? -1 : c.bbwRecentAfterSet,
               c.bbwRecentAfterQuick == 0xFF ? -1 : c.bbwRecentAfterQuick));
  for (uint8_t i = 0; used < cap && i < c.bbwRecentCount && i < 10; ++i)
    put(((c.bbwRecentSkipped >> i) & 1U) && c.bbwRecentCg[i] == 0
            ? snprintf(out + used, cap - used, "%snull", i ? "," : "")
            : snprintf(out + used, cap - used, "%s%.2f", i ? "," : "",
                       c.bbwRecentCg[i] / 100.0));
  if (used < cap)
    put(snprintf(out + used, cap - used, "],\"bbwRecentSkipped\":%u",
                 static_cast<unsigned>(c.bbwRecentCount < 10
                     ? c.bbwRecentSkipped & ((1U << c.bbwRecentCount) - 1U)
                     : c.bbwRecentSkipped)));
  return used < cap ? used : 0;
}

}  // namespace openbrewbyweight

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                      \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include "OpenBrewByWeightPersistence.h"
#include "OpenBrewByWeightPreferences.h"

namespace openbrewbyweight {

// False for an absent, unreadable or invalid record.
inline bool loadQuickStart(QuickStartRecord &record) {
  QuickStartRecord read;
  bool ok = false;
  if (lockSettingsNvs()) {
    OpenBrewByWeightPreferences preferences(NvsSubsystem::SETTINGS);
    if (preferences.begin(SETTINGS_NAMESPACE, true)) {
      ok = preferences.getBytesLength(QUICK_START_KEY) == sizeof(read) &&
           preferences.getBytes(QUICK_START_KEY, &read, sizeof(read)) ==
               sizeof(read) &&
           validQuickStart(read);
      preferences.end();
    }
    unlockSettingsNvs();
  }
  if (ok) record = read;
  return ok;
}

// Writes and read-back verifies the record. NVS replaces a key atomically,
// so a power cut leaves either the previous or the new record.
inline bool saveQuickStart(QuickStartRecord record) {
  finalizeQuickStart(record);
  if (!lockSettingsNvs()) return false;
  OpenBrewByWeightPreferences preferences(NvsSubsystem::SETTINGS);
  if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
    unlockSettingsNvs();
    return false;
  }
  QuickStartRecord verified;
  const bool saved =
      preferences.putBytes(QUICK_START_KEY, &record, sizeof(record)) ==
          sizeof(record) &&
      preferences.getBytes(QUICK_START_KEY, &verified, sizeof(verified)) ==
          sizeof(verified) &&
      memcmp(&verified, &record, sizeof(record)) == 0;
  preferences.end();
  unlockSettingsNvs();
  return saved;
}

}  // namespace openbrewbyweight
#endif
