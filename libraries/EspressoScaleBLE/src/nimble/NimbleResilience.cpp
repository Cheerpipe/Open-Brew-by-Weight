#include "nimble/NimbleResilience.h"

#include <algorithm>
#include <iterator>

namespace {

constexpr uint32_t kBackoffDelaysMs[] = {50, 80, 100, 250, 500, 1000, 2000,
                                         5000};
constexpr uint32_t kBackoffJitterMs = 30;

}  // namespace

void NimbleBackoffPolicy::reset() {
  deadlineMs_ = 0;
  failureCount_ = 0;
  armed_ = false;
}

void NimbleBackoffPolicy::addEntropy(uint32_t entropy) {
  randomState_ ^= entropy + 0x9e3779b9U;
}

uint32_t NimbleBackoffPolicy::nextRandom() {
  randomState_ ^= randomState_ << 13;
  randomState_ ^= randomState_ >> 17;
  randomState_ ^= randomState_ << 5;
  return randomState_;
}

uint32_t NimbleBackoffPolicy::schedule(uint32_t nowMs) {
  constexpr size_t delayCount =
      sizeof(kBackoffDelaysMs) / sizeof(kBackoffDelaysMs[0]);
  const size_t index = failureCount_ < delayCount ? failureCount_
                                                  : delayCount - 1;
  if (failureCount_ != 0xff) {
    ++failureCount_;
  }
  const uint32_t jitter = nextRandom() % (kBackoffJitterMs + 1U);
  const uint32_t delayMs = kBackoffDelaysMs[index] +
                           (index + 1 == delayCount ? 0 : jitter);
  deadlineMs_ = nowMs + delayMs;
  armed_ = true;
  return delayMs;
}

void NimbleBackoffPolicy::clearDeadline() {
  deadlineMs_ = 0;
  armed_ = false;
}

bool NimbleBackoffPolicy::active(uint32_t nowMs) const {
  return armed_ && !nimbleTimeReached(nowMs, deadlineMs_);
}

uint32_t NimbleBackoffPolicy::remainingMs(uint32_t nowMs) const {
  return active(nowMs) ? static_cast<uint32_t>(deadlineMs_ - nowMs) : 0;
}

void NimbleNegativeCache::expire(uint32_t nowMs) {
  for (auto & entrie : entries_) {
    if (entrie.used &&
        nimbleTimeReached(nowMs, entrie.expiresAtMs)) {
      entrie.used = false;
    }
  }
}

bool NimbleNegativeCache::contains(const NimblePeerKey &key,
                                   uint32_t nowMs) {
  expire(nowMs);
  Entry *const end = std::end(entries_);
  Entry *const found = std::find_if(
      std::begin(entries_), end, [&key](const Entry &entry) {
        return entry.used && nimblePeerKeyEqual(entry.key, key);
      });
  if (found == end) return false;
  found->sequence = ++sequence_;
  return true;
}

void NimbleNegativeCache::insert(const NimblePeerKey &key, uint32_t nowMs,
                                 uint32_t cooldownMs) {
  expire(nowMs);
  Entry *replacement = &entries_[0];
  for (auto & entry : entries_) {
    if (entry.used && nimblePeerKeyEqual(entry.key, key)) {
      replacement = &entry;
      break;
    }
    if (!entry.used) {
      replacement = &entry;
      break;
    }
    if (entry.sequence < replacement->sequence) {
      replacement = &entry;
    }
  }
  replacement->key = key;
  replacement->expiresAtMs = nowMs + cooldownMs;
  replacement->sequence = ++sequence_;
  replacement->used = true;
}

void NimbleNegativeCache::erase(const NimblePeerKey &key) {
  for (auto & entrie : entries_) {
    if (entrie.used &&
        nimblePeerKeyEqual(entrie.key, key)) {
      entrie.used = false;
    }
  }
}

size_t NimbleNegativeCache::activeCount(uint32_t nowMs) const {
  return static_cast<size_t>(
      std::count_if(std::begin(entries_), std::end(entries_),
                    [nowMs](const Entry &entry) {
                      return entry.used &&
                             !nimbleTimeReached(nowMs, entry.expiresAtMs);
                    }));
}
