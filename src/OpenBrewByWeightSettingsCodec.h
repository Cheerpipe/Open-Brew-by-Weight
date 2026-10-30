#pragma once

#include "OpenBrewByWeightPersistedSettings.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>

namespace openbrewbyweight {

// Settings schema 2: a tagged encoding of PersistedSettings, so later firmware
// can add, retire, widen or move fields over OTA without discarding stored
// settings, and earlier firmware skips what it does not know.
//
// Blob (little-endian): header {magic u32, schema u16, body length u16,
// storage revision u32}, body, CRC-32 u32 over header and body. The body is a
// sequence of groups `tag, length, fields`; a field is `tag, length, value`.
// Tags and lengths are unsigned LEB128.
//
// Evolution rules:
// - every persisted struct member has one row below; a released tag keeps its
//   meaning forever and is never reused (retired tags are listed here: none);
// - a member whose type or meaning changes gets a new tag;
// - a missing field keeps the struct's default initializer and an unknown tag
//   is skipped; a malformed value (length, range, unterminated text) keeps the
//   default and makes the load a repair;
// - the encoded size never exceeds PERSISTED_SETTINGS_NVS_BUDGET, so earlier
//   firmware can always read a later blob.

constexpr uint16_t SETTINGS_BLOB_HEADER_BYTES = 12;
constexpr uint16_t SETTINGS_BLOB_MIN_BYTES = SETTINGS_BLOB_HEADER_BYTES + 4;

enum class SettingsFieldKind : uint8_t { UINT, BOOL, FLOAT, TEXT, BYTES };

struct SettingsField {
  uint16_t tag;
  uint16_t offset;
  uint16_t size;
  SettingsFieldKind kind;
};

template <typename T>
constexpr SettingsFieldKind settingsFieldKind() {
  if constexpr (std::is_same_v<T, bool>) {
    return SettingsFieldKind::BOOL;
  } else if constexpr (std::is_same_v<T, float>) {
    return SettingsFieldKind::FLOAT;
  } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
    return SettingsFieldKind::UINT;
  } else if constexpr (std::is_same_v<std::remove_extent_t<T>, char>) {
    return SettingsFieldKind::TEXT;
  } else {
    static_assert(std::is_same_v<std::remove_extent_t<T>, uint8_t>,
                  "unsupported persisted settings member type");
    return SettingsFieldKind::BYTES;
  }
}

#define OBBW_SETTINGS_FIELD(tag, Struct, member)                         \
  SettingsField {                                                        \
    tag, static_cast<uint16_t>(offsetof(Struct, member)),                \
        static_cast<uint16_t>(sizeof(Struct::member)),                   \
        settingsFieldKind<decltype(Struct::member)>()                    \
  }

#define SF(tag, member) OBBW_SETTINGS_FIELD(tag, RuntimeConfig, member)
inline constexpr SettingsField SETTINGS_RUNTIME_FIELDS[] = {
    SF(1, revision), SF(2, goalWeightDg), SF(3, powerManagementEnabled),
    SF(4, timezoneAutomatic), SF(5, weightOffsetG), SF(6, weightOffsetBaselineG),
    SF(7, autoTare), SF(8, touchStopFallbackEnabled), SF(9, postTareBaselineGraceMs),
    SF(10, timerOnly), SF(11, canTareStartTimer), SF(12, scaleTimerStopExtraDelayMs),
    SF(13, firstDropBeep), SF(14, paddleReturnReminderBeep), SF(15, soundAlertsMuted),
    SF(16, paddleReturnReminderIntervalMs), SF(17, paddleReturnReminderMaxDurationMs),
    SF(18, paddleMode), SF(19, buzzerScaleLostBeep), SF(20, buzzerAutoToManualGuardEndBeep),
    SF(21, buzzerManualNoScaleBeep), SF(22, buzzerScaleConnectedBeep),
    SF(23, scaleConnectedLed), SF(24, buzzerExtendedPulseRate),
    SF(25, buzzerSlowExtendedPulseRate), SF(26, alertOutputChannel),
    SF(27, rinseGestureMs), SF(28, rinseDurationMs), SF(29, autoRetare),
    SF(30, retareWindowMs), SF(31, minimumCupWeightG), SF(32, retareStabilitySamples),
    SF(33, retareStabilityToleranceG), SF(34, retareStabilityMaxGapMs),
    SF(35, retareStabilityMinDurationMs), SF(36, bbwProtectionMs),
    SF(37, operationalWallMs), SF(38, timezoneId), SF(39, firstTimezoneAutoUptimeUs),
    SF(40, firstTimezoneAutoUtcSec), SF(41, firstTimezoneAutoBootId),
    SF(42, timezoneSource), SF(43, firstTimezoneAutoClockQuality),
    SF(44, ntpServerPreset), SF(45, ntpServerCustom), SF(46, fastSideEnabled),
    SF(47, fastMarginDg), SF(48, fastBeforeMs), SF(49, slowSideEnabled),
    SF(50, slowMarginDg), SF(51, slowAfterMs), SF(52, scaleLossFixedMs),
    SF(53, scaleLossEnabled), SF(54, scaleMacCacheMode), SF(55, bookooMuteOnBuzzerOnly),
    SF(56, bookooConnectBeepLevel), SF(57, cupProtectionEnabled),
    SF(58, stopIfCupRemoved), SF(59, requireCupToStart),
    SF(60, avoidAccidentalTouchEnabled), SF(61, cupRemovedWeightG),
    SF(62, noScaleBbwMode), SF(63, lastShotCooldownMs), SF(64, serialLogLevel),
    SF(65, ringRetainLogLevel), SF(66, stopPulseTenMs), SF(67, maxSinglePressHundredMs),
    SF(68, dripDelayMs), SF(69, momentaryStartOnPress),
    SF(70, reedConfirmTimeoutHundredMs), SF(71, assumeIdleWhenScaleConnects),
    SF(72, shotReactTimeoutS), SF(73, rinseEnabled), SF(74, showDiagnosticPage),
    SF(75, autoTareOutsideBrew), SF(76, ntpSyncEnabled),
    SF(77, scheduledRestartEnabled), SF(78, scheduledRestartMinuteOfDay),
    SF(79, fastRampMs), SF(80, slowRampMs), SF(81, fastCurve), SF(82, fastCurveHardness),
    SF(83, slowCurve), SF(84, slowCurveHardness), SF(85, heroCardMode)};
#undef SF

#define SF(tag, member) OBBW_SETTINGS_FIELD(tag, ShotPreset, member)
inline constexpr SettingsField SETTINGS_PRESET_FIELDS[] = {
    SF(1, id), SF(2, name), SF(3, isFactory), SF(4, brewByWeight), SF(5, goalWeightDg),
    SF(6, fastMarginDg), SF(7, slowMarginDg), SF(8, doseDg), SF(9, fastBeforeMs),
    SF(10, slowAfterMs), SF(11, bbwProtectionMs), SF(12, operationalWallMs),
    SF(13, scaleLossFixedMs), SF(14, learningEpoch), SF(15, bbwEwmaOffsetG),
    SF(16, weightOffsetBaselineG), SF(17, fastSideEnabled), SF(18, slowSideEnabled),
    SF(19, grindHintsEnabled), SF(20, bbwEwmaAlpha), SF(21, bbwAlphaLearned),
    SF(22, bbwAlphaBaseline), SF(23, lineaMicraBrewTargetDeciC), SF(24, fastRampMs),
    SF(25, slowRampMs), SF(26, fastCurve), SF(27, fastCurveHardness), SF(28, slowCurve),
    SF(29, slowCurveHardness)};
#undef SF

// The bank's preset count is the number of preset groups.
inline constexpr SettingsField SETTINGS_PRESET_BANK_FIELDS[] = {
    OBBW_SETTINGS_FIELD(1, ShotPresetBank, activeId),
    OBBW_SETTINGS_FIELD(2, ShotPresetBank, nextId)};

inline constexpr SettingsField SETTINGS_BULLSEYE_FIELDS[] = {
    OBBW_SETTINGS_FIELD(1, BullseyeMelodyConfig, enabled),
    OBBW_SETTINGS_FIELD(2, BullseyeMelodyConfig, rtttl)};

#define SF(tag, member) OBBW_SETTINGS_FIELD(tag, PersistedSettings, member)
inline constexpr SettingsField SETTINGS_DEVICE_FIELDS[] = {
    SF(1, staConfigured), SF(2, staOpen), SF(3, staWifiSleep), SF(4, staSsid),
    SF(5, staPassword), SF(6, staIpMode), SF(7, staIp), SF(8, staNetmask),
    SF(9, staGateway), SF(10, staDns1), SF(11, staDns2), SF(12, staConfigState),
    SF(13, lkgValid), SF(14, lkgOpen), SF(15, lkgSsid), SF(16, lkgPassword),
    SF(17, lkgIpMode), SF(18, lkgIp), SF(19, lkgNetmask), SF(20, lkgGateway),
    SF(21, lkgDns1), SF(22, lkgDns2), SF(23, devicePassword),
    SF(24, preferredScaleMac), SF(25, preferredScaleName), SF(26, deviceName)};
#undef SF

inline constexpr SettingsField SETTINGS_SCALE_HISTORY_FIELDS[] = {
    OBBW_SETTINGS_FIELD(1, ScaleHistoryEntry, mac),
    OBBW_SETTINGS_FIELD(2, ScaleHistoryEntry, name),
    OBBW_SETTINGS_FIELD(3, ScaleHistoryEntry, friendlyName),
    OBBW_SETTINGS_FIELD(4, ScaleHistoryEntry, lastSeenSeq)};

#define SF(tag, member) OBBW_SETTINGS_FIELD(tag, WebhookConfig, member)
inline constexpr SettingsField SETTINGS_WEBHOOK_FIELDS[] = {
    SF(1, enabled), SF(2, brewState), SF(3, firstDrop), SF(4, end), SF(5, url),
    SF(6, deferDuringShot), SF(7, presetChanges)};
#undef SF

#define SF(tag, member) OBBW_SETTINGS_FIELD(tag, LineaMicraPersistedSettings, member)
inline constexpr SettingsField SETTINGS_MICRA_FIELDS[] = {
    SF(1, username), SF(2, password), SF(3, installationPrivateKey),
    SF(4, selectedSerial), SF(5, selectedName), SF(6, options), SF(7, scaleOptions),
    SF(8, accountConfigured), SF(9, connectionType)};
#undef SF

// Top-level group tags. Presets and scale-history entries repeat.
enum SettingsGroupTag : uint8_t {
  SETTINGS_GROUP_RUNTIME = 1,
  SETTINGS_GROUP_BULLSEYE = 2,
  SETTINGS_GROUP_DEVICE = 3,
  SETTINGS_GROUP_PRESET = 4,
  SETTINGS_GROUP_PRESET_BANK = 5,
  SETTINGS_GROUP_SCALE_HISTORY = 6,
  SETTINGS_GROUP_WEBHOOK = 7,
  SETTINGS_GROUP_MICRA = 8,
};

constexpr size_t settingsVarintBytes(size_t value) {
  size_t bytes = 1;
  for (; value >= 0x80; value >>= 7) ++bytes;
  return bytes;
}

constexpr size_t settingsValueMaxBytes(const SettingsField &field) {
  return field.kind == SettingsFieldKind::TEXT ? field.size - 1U : field.size;
}

template <size_t N>
constexpr size_t settingsFieldsMaxBytes(const SettingsField (&fields)[N]) {
  size_t bytes = 0;
  for (const SettingsField &field : fields) {
    const size_t value = settingsValueMaxBytes(field);
    bytes += settingsVarintBytes(field.tag) + settingsVarintBytes(value) + value;
  }
  return bytes;
}

template <size_t N>
constexpr size_t settingsGroupMaxBytes(uint8_t tag, const SettingsField (&fields)[N]) {
  const size_t body = settingsFieldsMaxBytes(fields);
  return settingsVarintBytes(tag) + settingsVarintBytes(body) + body;
}

// Bytes the table persists; the rest of the struct must be padding. A member
// added without a row changes the struct size and fails these checks.
template <size_t N>
constexpr size_t settingsFieldBytes(const SettingsField (&fields)[N]) {
  size_t bytes = 0;
  for (const SettingsField &field : fields) bytes += field.size;
  return bytes;
}

static_assert(settingsFieldBytes(SETTINGS_RUNTIME_FIELDS) + 24 == sizeof(RuntimeConfig),
              "every RuntimeConfig member needs a settings field row");
static_assert(settingsFieldBytes(SETTINGS_PRESET_FIELDS) + 1 == sizeof(ShotPreset),
              "every ShotPreset member needs a settings field row");
static_assert(settingsFieldBytes(SETTINGS_PRESET_BANK_FIELDS) +
                      sizeof(ShotPresetBank::count) + sizeof(ShotPresetBank::presets) + 1 ==
                  sizeof(ShotPresetBank),
              "every ShotPresetBank member needs a settings field row");
static_assert(settingsFieldBytes(SETTINGS_BULLSEYE_FIELDS) == sizeof(BullseyeMelodyConfig),
              "every BullseyeMelodyConfig member needs a settings field row");
static_assert(settingsFieldBytes(SETTINGS_SCALE_HISTORY_FIELDS) + 2 ==
                  sizeof(ScaleHistoryEntry),
              "every ScaleHistoryEntry member needs a settings field row");
static_assert(settingsFieldBytes(SETTINGS_WEBHOOK_FIELDS) == sizeof(WebhookConfig),
              "every WebhookConfig member needs a settings field row");
static_assert(settingsFieldBytes(SETTINGS_MICRA_FIELDS) ==
                  sizeof(LineaMicraPersistedSettings),
              "every LineaMicraPersistedSettings member needs a settings field row");
static_assert(settingsFieldBytes(SETTINGS_DEVICE_FIELDS) +
                      sizeof(PersistedSettings::storageRevision) + sizeof(RuntimeConfig) +
                      sizeof(BullseyeMelodyConfig) + sizeof(ShotPresetBank) +
                      sizeof(PersistedSettings::scaleHistory) + sizeof(WebhookConfig) +
                      sizeof(LineaMicraPersistedSettings) + 7 ==
                  sizeof(PersistedSettings),
              "every PersistedSettings member needs a settings field row or group");

constexpr size_t PERSISTED_SETTINGS_ENCODED_MAX_BYTES =
    SETTINGS_BLOB_MIN_BYTES +
    settingsGroupMaxBytes(SETTINGS_GROUP_RUNTIME, SETTINGS_RUNTIME_FIELDS) +
    settingsGroupMaxBytes(SETTINGS_GROUP_BULLSEYE, SETTINGS_BULLSEYE_FIELDS) +
    settingsGroupMaxBytes(SETTINGS_GROUP_DEVICE, SETTINGS_DEVICE_FIELDS) +
    MAX_SHOT_PRESETS * settingsGroupMaxBytes(SETTINGS_GROUP_PRESET, SETTINGS_PRESET_FIELDS) +
    settingsGroupMaxBytes(SETTINGS_GROUP_PRESET_BANK, SETTINGS_PRESET_BANK_FIELDS) +
    SCALE_HISTORY_CAPACITY *
        settingsGroupMaxBytes(SETTINGS_GROUP_SCALE_HISTORY, SETTINGS_SCALE_HISTORY_FIELDS) +
    settingsGroupMaxBytes(SETTINGS_GROUP_WEBHOOK, SETTINGS_WEBHOOK_FIELDS) +
    settingsGroupMaxBytes(SETTINGS_GROUP_MICRA, SETTINGS_MICRA_FIELDS);
static_assert(PERSISTED_SETTINGS_ENCODED_MAX_BYTES <= PERSISTED_SETTINGS_NVS_BUDGET,
              "Encoded settings exceed the NVS budget earlier firmware can read");

struct SettingsWriter {
  uint8_t *out = nullptr;
  size_t capacity = 0;
  size_t at = 0;
  bool ok = true;

  void bytes(const void *data, size_t length) {
    if (!ok || length > capacity - at) {
      ok = false;
      return;
    }
    memcpy(out + at, data, length);
    at += length;
  }
  void varint(size_t value) {
    do {
      const uint8_t byte = static_cast<uint8_t>((value & 0x7FU) | (value >= 0x80 ? 0x80U : 0U));
      bytes(&byte, 1);
      value >>= 7;
    } while (value != 0);
  }
};

struct SettingsReader {
  const uint8_t *at = nullptr;
  const uint8_t *end = nullptr;

  bool varint(size_t &value) {
    value = 0;
    for (unsigned shift = 0; shift < 32 && at < end; shift += 7) {
      const uint8_t byte = *at++;
      value |= static_cast<size_t>(byte & 0x7FU) << shift;
      if ((byte & 0x80U) == 0) return true;
    }
    return false;
  }
  // Next `tag, length` record; `body` covers its value.
  bool record(size_t &tag, SettingsReader &body) {
    size_t length = 0;
    if (!varint(tag) || !varint(length) || length > static_cast<size_t>(end - at)) return false;
    body = {at, at + length};
    at += length;
    return true;
  }
};

inline size_t settingsValueBytes(const SettingsField &field, const uint8_t *base) {
  return field.kind == SettingsFieldKind::TEXT
             ? strnlen(reinterpret_cast<const char *>(base + field.offset), field.size - 1U)
             : field.size;
}

[[gnu::noinline]] inline void encodeSettingsFields(SettingsWriter &writer, uint8_t tag,
                                                   const SettingsField *fields, size_t count,
                                                   const void *object) {
  const auto *base = static_cast<const uint8_t *>(object);
  size_t body = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t value = settingsValueBytes(fields[i], base);
    body += settingsVarintBytes(fields[i].tag) + settingsVarintBytes(value) + value;
  }
  writer.varint(tag);
  writer.varint(body);
  for (size_t i = 0; i < count; ++i) {
    const size_t value = settingsValueBytes(fields[i], base);
    writer.varint(fields[i].tag);
    writer.varint(value);
    writer.bytes(base + fields[i].offset, value);
  }
}

template <size_t N>
inline void encodeSettingsGroup(SettingsWriter &writer, uint8_t tag,
                                const SettingsField (&fields)[N], const void *object) {
  encodeSettingsFields(writer, tag, fields, N, object);
}

// Writes one value into a member that already holds its default; false
// leaves the default for a malformed value.
[[gnu::noinline]] inline bool decodeSettingsValue(const SettingsField &field,
                                                  const SettingsReader &value, uint8_t *base) {
  const size_t length = static_cast<size_t>(value.end - value.at);
  uint8_t *member = base + field.offset;
  switch (field.kind) {
    case SettingsFieldKind::UINT: {
      if (length == 0 || length > 8) return false;
      uint64_t number = 0;
      for (size_t i = 0; i < length; ++i) number |= static_cast<uint64_t>(value.at[i]) << (8U * i);
      if (field.size < 8 && (number >> (8U * field.size)) != 0) return false;
      memcpy(member, &number, field.size);
      return true;
    }
    case SettingsFieldKind::BOOL:
      if (length != 1 || value.at[0] > 1) return false;
      *reinterpret_cast<bool *>(member) = value.at[0] != 0;
      return true;
    case SettingsFieldKind::TEXT:
      if (length >= field.size || memchr(value.at, '\0', length) != nullptr) return false;
      memset(member, 0, field.size);
      memcpy(member, value.at, length);
      return true;
    case SettingsFieldKind::FLOAT:
    case SettingsFieldKind::BYTES:
      if (length != field.size) return false;
      memcpy(member, value.at, length);
      return true;
  }
  return false;
}

// Unknown tags are skipped; false only for a body that does not frame.
[[gnu::noinline]] inline bool decodeSettingsFields(SettingsReader group,
                                                   const SettingsField *fields, size_t count,
                                                   void *object, uint32_t &malformed) {
  auto *base = static_cast<uint8_t *>(object);
  while (group.at < group.end) {
    size_t tag = 0;
    SettingsReader value{};
    if (!group.record(tag, value)) return false;
    for (size_t i = 0; i < count; ++i) {
      if (fields[i].tag != tag) continue;
      if (!decodeSettingsValue(fields[i], value, base)) ++malformed;
      break;
    }
  }
  return true;
}

template <size_t N>
inline bool decodeSettingsGroup(SettingsReader group, const SettingsField (&fields)[N],
                                void *object, uint32_t &malformed) {
  return decodeSettingsFields(group, fields, N, object, malformed);
}

// Encodes `settings` (with `revision`) into `out`; returns the blob length,
// or 0 when it does not fit.
[[gnu::noinline]] inline size_t encodePersistedSettings(const PersistedSettings &settings,
                                                        uint32_t revision, uint8_t *out,
                                                        size_t capacity) {
  if (capacity < SETTINGS_BLOB_MIN_BYTES) return 0;
  SettingsWriter writer{out, capacity - 4U, SETTINGS_BLOB_HEADER_BYTES};
  encodeSettingsGroup(writer, SETTINGS_GROUP_RUNTIME, SETTINGS_RUNTIME_FIELDS, &settings.runtime);
  encodeSettingsGroup(writer, SETTINGS_GROUP_BULLSEYE, SETTINGS_BULLSEYE_FIELDS,
                      &settings.bullseyeMelody);
  encodeSettingsGroup(writer, SETTINGS_GROUP_DEVICE, SETTINGS_DEVICE_FIELDS, &settings);
  for (uint8_t i = 0; i < settings.presets.count && i < MAX_SHOT_PRESETS; ++i)
    encodeSettingsGroup(writer, SETTINGS_GROUP_PRESET, SETTINGS_PRESET_FIELDS,
                        &settings.presets.presets[i]);
  encodeSettingsGroup(writer, SETTINGS_GROUP_PRESET_BANK, SETTINGS_PRESET_BANK_FIELDS,
                      &settings.presets);
  for (const ScaleHistoryEntry &entry : settings.scaleHistory)
    encodeSettingsGroup(writer, SETTINGS_GROUP_SCALE_HISTORY, SETTINGS_SCALE_HISTORY_FIELDS,
                        &entry);
  encodeSettingsGroup(writer, SETTINGS_GROUP_WEBHOOK, SETTINGS_WEBHOOK_FIELDS, &settings.webhook);
  encodeSettingsGroup(writer, SETTINGS_GROUP_MICRA, SETTINGS_MICRA_FIELDS, &settings.lineaMicra);
  const size_t body = writer.at - SETTINGS_BLOB_HEADER_BYTES;
  if (!writer.ok || body > UINT16_MAX) return 0;
  const uint32_t magic = PERSISTED_SETTINGS_MAGIC;
  const uint16_t schema = static_cast<uint16_t>(CONFIG_SCHEMA_VERSION);
  const uint16_t bodyLength = static_cast<uint16_t>(body);
  memcpy(out, &magic, 4);
  memcpy(out + 4, &schema, 2);
  memcpy(out + 6, &bodyLength, 2);
  memcpy(out + 8, &revision, 4);
  const uint32_t crc = crc32(out, writer.at);
  memcpy(out + writer.at, &crc, 4);
  return writer.at + 4U;
}

// Magic, schema, framing and CRC of a stored blob; yields its revision.
inline bool settingsBlobIntact(const uint8_t *blob, size_t length, uint32_t &revision) {
  if (length < SETTINGS_BLOB_MIN_BYTES) return false;
  uint32_t magic = 0, crc = 0;
  uint16_t schema = 0, body = 0;
  memcpy(&magic, blob, 4);
  memcpy(&schema, blob + 4, 2);
  memcpy(&body, blob + 6, 2);
  memcpy(&revision, blob + 8, 4);
  memcpy(&crc, blob + length - 4U, 4);
  return magic == PERSISTED_SETTINGS_MAGIC && schema == CONFIG_SCHEMA_VERSION &&
         static_cast<size_t>(body) + SETTINGS_BLOB_MIN_BYTES == length &&
         crc == crc32(blob, length - 4U);
}

// Decodes an intact blob into `settings`, which must hold the defaults.
// `malformed` counts values that kept their default; false when the body
// does not frame.
[[gnu::noinline]] inline bool decodePersistedSettings(const uint8_t *blob, size_t length,
                                                      PersistedSettings &settings,
                                                      uint32_t &malformed) {
  SettingsReader body{blob + SETTINGS_BLOB_HEADER_BYTES, blob + length - 4U};
  uint8_t presets = 0, scales = 0;
  while (body.at < body.end) {
    size_t tag = 0;
    SettingsReader group{};
    if (!body.record(tag, group)) return false;
    bool framed = true;
    switch (tag) {
      case SETTINGS_GROUP_RUNTIME:
        framed = decodeSettingsGroup(group, SETTINGS_RUNTIME_FIELDS, &settings.runtime, malformed);
        break;
      case SETTINGS_GROUP_BULLSEYE:
        framed = decodeSettingsGroup(group, SETTINGS_BULLSEYE_FIELDS, &settings.bullseyeMelody,
                                     malformed);
        break;
      case SETTINGS_GROUP_DEVICE:
        framed = decodeSettingsGroup(group, SETTINGS_DEVICE_FIELDS, &settings, malformed);
        break;
      case SETTINGS_GROUP_PRESET:
        if (presets == MAX_SHOT_PRESETS) {
          ++malformed;
          break;
        }
        framed = decodeSettingsGroup(group, SETTINGS_PRESET_FIELDS,
                                     &settings.presets.presets[presets++], malformed);
        break;
      case SETTINGS_GROUP_PRESET_BANK:
        framed = decodeSettingsGroup(group, SETTINGS_PRESET_BANK_FIELDS, &settings.presets,
                                     malformed);
        break;
      case SETTINGS_GROUP_SCALE_HISTORY:
        if (scales == SCALE_HISTORY_CAPACITY) {
          ++malformed;
          break;
        }
        framed = decodeSettingsGroup(group, SETTINGS_SCALE_HISTORY_FIELDS,
                                     &settings.scaleHistory[scales++], malformed);
        break;
      case SETTINGS_GROUP_WEBHOOK:
        framed = decodeSettingsGroup(group, SETTINGS_WEBHOOK_FIELDS, &settings.webhook, malformed);
        break;
      case SETTINGS_GROUP_MICRA:
        framed = decodeSettingsGroup(group, SETTINGS_MICRA_FIELDS, &settings.lineaMicra,
                                     malformed);
        break;
      default:
        break;
    }
    if (!framed) return false;
  }
  settings.presets.count = presets;
  return true;
}

}  // namespace openbrewbyweight
