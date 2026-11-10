#pragma once

#include "OpenBrewByWeightDomain.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace openbrewbyweight {

// The last warnings and errors before a reset, in PSRAM that no boot clears:
// a crash, a watchdog or a planned restart leaves them for the next boot to
// put back in the Web UI log. Each line has its own magic and checksum, so a
// torn, stale or never-written one is dropped; the boot empties the store.
constexpr size_t LAST_WORDS_CAPACITY = 8;
constexpr size_t LAST_WORDS_TEXT_BYTES = 104;
constexpr uint32_t LAST_WORDS_MAGIC = 0x4C535457U;  // "LSTW"

struct LastWordsLine {
  uint32_t magic;
  uint32_t sequence;  // From 1 each boot: orders the lines.
  uint32_t atMs;
  uint32_t wallSec;
  int32_t argument1;
  int32_t argument2;
  uint8_t level;
  uint8_t category;
  uint8_t code;
  char text[LAST_WORDS_TEXT_BYTES];
  uint32_t checksum;
};
static_assert(offsetof(LastWordsLine, checksum) == 132 && sizeof(LastWordsLine) == 136,
              "last words line layout changed");

struct LastWords {
  LastWordsLine lines[LAST_WORDS_CAPACITY];
};

inline uint32_t lastWordsChecksum(const LastWordsLine &line) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&line);
  uint32_t hash = 2166136261U;
  for (size_t index = 0; index < offsetof(LastWordsLine, checksum); ++index)
    hash = (hash ^ bytes[index]) * 16777619U;
  return hash;
}

// Writes over the oldest line; returns it for the caller's cache write-back.
inline const LastWordsLine &noteLastWords(LastWords &store, uint32_t sequence, Instant at,
                                          LogLevel level, DebugCategory category,
                                          DebugCode code, int32_t argument1,
                                          int32_t argument2, const char *text) {
  LastWordsLine &line = store.lines[(sequence - 1U) % LAST_WORDS_CAPACITY];
  line = LastWordsLine{};
  line.sequence = sequence;
  line.atMs = at.monotonicMs;
  line.wallSec = at.utcSec;
  line.argument1 = argument1;
  line.argument2 = argument2;
  line.level = static_cast<uint8_t>(level);
  line.category = static_cast<uint8_t>(category);
  line.code = static_cast<uint8_t>(code);
  if (text != nullptr) copyCString(line.text, sizeof(line.text), text);
  line.magic = LAST_WORDS_MAGIC;
  line.checksum = lastWordsChecksum(line);
  return line;
}

// Boot: copies the valid lines oldest first and empties the store; returns
// how many. `kept` is false after a reset that loses PSRAM.
inline size_t collectLastWords(LastWords &store, bool kept,
                               LastWordsLine (&out)[LAST_WORDS_CAPACITY]) {
  size_t count = 0;
  for (const LastWordsLine &line : store.lines) {
    if (kept && line.magic == LAST_WORDS_MAGIC && line.sequence != 0 &&
        line.level <= static_cast<uint8_t>(LogLevel::WARNING) &&
        memchr(line.text, '\0', sizeof(line.text)) != nullptr &&
        line.checksum == lastWordsChecksum(line))
      out[count++] = line;
  }
  std::sort(out, out + count, [](const LastWordsLine &a, const LastWordsLine &b) {
    return a.sequence < b.sequence;
  });
  memset(&store, 0, sizeof(store));
  return count;
}

}  // namespace openbrewbyweight
