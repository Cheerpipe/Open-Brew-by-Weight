#pragma once

// UTF-8 validity for text that leaves as a WebSocket text frame, which a client
// must drop whole on one invalid sequence.

#include <cstddef>

namespace openbrewbyweight {

// The length of the valid sequence at `text` (1 to 4 bytes), or 0 when it is
// not one: a stray continuation, an overlong form, a surrogate, past U+10FFFF
// or cut short by the terminator.
inline size_t utf8SequenceBytes(const char *text) {
  const unsigned char lead = static_cast<unsigned char>(text[0]);
  if (lead < 0x80) return 1;
  const size_t width = lead > 0xF4 ? 0 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC2 ? 2 : 0;
  if (width == 0) return 0;
  const unsigned char second = static_cast<unsigned char>(text[1]);
  if (second < (lead == 0xE0 ? 0xA0 : lead == 0xF0 ? 0x90 : 0x80) ||
      second > (lead == 0xED ? 0x9F : lead == 0xF4 ? 0x8F : 0xBF)) {
    return 0;
  }
  for (size_t index = 2; index < width; ++index) {
    if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) return 0;
  }
  return width;
}

}  // namespace openbrewbyweight
