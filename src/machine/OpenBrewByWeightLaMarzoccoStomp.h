#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace openbrewbyweight {

// One PSRAM-owned accumulator. Network spans never escape the receive callback.
struct LaMarzoccoStompFrame {
  char command[16] = {};
  char destination[128] = {};
  char subscription[40] = {};
  char version[8] = {};
  char heartbeat[32] = {};
  char *body = nullptr;
  size_t bodyLength = 0;
  uint32_t ingressAttempt = 0;
};

class LaMarzoccoStompDecoder {
 public:
  static constexpr size_t kHeaderLimit = 1024;
  static constexpr size_t kBodyLimit = 16384;
  static constexpr size_t kCapacity = kHeaderLimit + kBodyLimit + 2;
  using Consumer = bool (*)(void *, const LaMarzoccoStompFrame &);

  void reset() { used_ = bodyOffset_ = 0; startedAtMs_ = 0; hasLength_ = false; frame_ = {}; }
  uint32_t pendingSince() const { return used_ ? startedAtMs_ : 0; }
  bool pending() const { return used_ != 0; }
  bool incompleteExpired(uint32_t now) const {
    return used_ != 0 && now - startedAtMs_ >= 10000;
  }
  bool feed(const char *data, size_t length, uint32_t now,
            Consumer consume, void *context, uint32_t ingressAttempt = 0) {
    if (incompleteExpired(now)) return false;
    size_t frames = 0;
    while (length != 0) {
      if (used_ == 0) {
        while (length && (*data == '\n' || *data == '\r')) { ++data; --length; }
        if (!length) return true;
        startedAtMs_ = now;
        frame_.ingressAttempt = ingressAttempt;
      }
      if (bodyOffset_) {
        const size_t remaining = (hasLength_ ? frame_.bodyLength : kBodyLimit) - (used_ - bodyOffset_);
        size_t count = length < remaining ? length : remaining;
        if (!hasLength_) {
          const auto *end = static_cast<const char *>(memchr(data, '\0', count));
          if (end) count = static_cast<size_t>(end - data);
        }
        memcpy(bytes_ + used_, data, count);
        used_ += count;
        data += count;
        length -= count;
        if (!length) continue;
        if (*data++ != '\0') return false;
        bytes_[used_++] = '\0';
        --length;
        frame_.bodyLength = used_ - bodyOffset_ - 1;
        if (++frames > 32 || !consume(context, frame_)) return false;
        reset();
        continue;
      }
      if (used_ == kHeaderLimit) return false;
      bytes_[used_++] = *data++;
      --length;
      if (bytes_[used_ - 1] == '\0') return false;
      if (bytes_[used_ - 1] == '\n' &&
          ((used_ >= 2 && bytes_[used_ - 2] == '\n') ||
           (used_ >= 3 && bytes_[used_ - 2] == '\r' && bytes_[used_ - 3] == '\n'))) {
        if (!parseHeaders()) return false;
        bodyOffset_ = used_;
        frame_.body = bytes_ + bodyOffset_;
      }
    }
    return !incompleteExpired(now);
  }

 private:
  static bool unescape(char *text, size_t length) {
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
      char c = text[i];
      if (c == '\0' || c == '\r') return false;
      if (c == '\\') {
        if (++i == length) return false;
        switch (text[i]) {
          case 'n': c = '\n'; break;
          case 'r': c = '\r'; break;
          case 'c': c = ':'; break;
          case '\\': c = '\\'; break;
          default: return false;
        }
      }
      text[used++] = c;
    }
    text[used] = '\0';
    return true;
  }

  bool parseHeaders() {
    auto &frame = frame_;
    size_t cursor = 0;
    const char *newline = static_cast<const char *>(memchr(bytes_, '\n', used_));
    if (!newline) return false;
    size_t size = static_cast<size_t>(newline - bytes_);
    if (size && bytes_[size - 1] == '\r') --size;
    if (!size || size >= sizeof(frame.command)) return false;
    if (memchr(bytes_, '\0', size) || memchr(bytes_, '\r', size)) return false;
    memcpy(frame.command, bytes_, size);
    cursor = static_cast<size_t>(newline - bytes_) + 1;
    for (;;) {
      newline = static_cast<const char *>(memchr(bytes_ + cursor, '\n', used_ - cursor));
      if (!newline) return false;
      size = static_cast<size_t>(newline - (bytes_ + cursor));
      if (size && bytes_[cursor + size - 1] == '\r') --size;
      const size_t next = static_cast<size_t>(newline - bytes_) + 1;
      if (next > kHeaderLimit) return false;
      if (!size) return true;
      char *colon = static_cast<char *>(memchr(bytes_ + cursor, ':', size));
      if (!colon) return false;
      char *key = bytes_ + cursor, *value = colon + 1;
      const size_t keySize = static_cast<size_t>(colon - (bytes_ + cursor));
      if (!keySize || !unescape(key, keySize) ||
          !unescape(value, size - keySize - 1)) return false;
      char *target = nullptr;
      size_t capacity = 0;
      if (strcmp(key, "destination") == 0) { target = frame.destination; capacity = sizeof(frame.destination); }
      else if (strcmp(key, "subscription") == 0) { target = frame.subscription; capacity = sizeof(frame.subscription); }
      else if (strcmp(key, "version") == 0) { target = frame.version; capacity = sizeof(frame.version); }
      else if (strcmp(key, "heart-beat") == 0) { target = frame.heartbeat; capacity = sizeof(frame.heartbeat); }
      else if (strcmp(key, "content-length") == 0) {
        if (hasLength_ || !value[0]) return false;
        hasLength_ = true;
        for (const char *p = value; *p; ++p) {
          if (*p < '0' || *p > '9' || frame.bodyLength > (kBodyLimit - (*p - '0')) / 10) return false;
          frame.bodyLength = frame.bodyLength * 10 + (*p - '0');
        }
      }
      if (target) {
        if (!value[0] || target[0] || strlen(value) >= capacity) return false;
        memcpy(target, value, strlen(value) + 1);
      }
      cursor = next;
    }
  }

  size_t used_ = 0, bodyOffset_ = 0;
  uint32_t startedAtMs_ = 0;
  bool hasLength_ = false;
  LaMarzoccoStompFrame frame_;
  char bytes_[kCapacity] = {};
};
}  // namespace openbrewbyweight
