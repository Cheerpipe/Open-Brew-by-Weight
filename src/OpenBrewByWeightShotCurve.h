#pragma once

// The PSRAM curve cache: the newest SHOT_CURVE_CAPACITY curves, oldest first.
// Curves live in the shot trace pool, one curve section per shot record
// (docs/SHOT_TRACE.md). The pending curves, those not in the pool yet, are
// always the newest ones: the settings_persist worker saves them oldest
// first, and after boot it fills the cache's oldest end back from the pool.
// Older curves are read from the pool on demand. Every call runs under
// shotStoreMutex; nothing here touches flash.
#include "OpenBrewByWeightShotCurveTypes.h"

namespace openbrewbyweight {

// Curve section payload: count (u16), flags (u8, bit 0 truncated), 0 (u8),
// the 18 fixed bytes (atmClearedMs and the four events), the delta stream as
// captured, then the break and held bitmaps of count bits each, zero padded.
// The stream length is what the section length leaves.
constexpr size_t SHOT_CURVE_SECTION_FIXED_BYTES = 22;
constexpr size_t SHOT_CURVE_SECTION_MAX_BYTES =
    SHOT_CURVE_SECTION_FIXED_BYTES + SHOT_CURVE_STREAM_BYTES + 2U * SHOT_CURVE_BREAK_BYTES;

// Bits past the count are stale record bytes, never observations.
inline void clearShotCurveTailBits(uint8_t *breakBefore, uint8_t *held, uint16_t count) {
  if (count % 8U == 0U) return;
  const uint8_t tail = static_cast<uint8_t>((1U << (count % 8U)) - 1U);
  breakBefore[count / 8U] &= tail;
  held[count / 8U] &= tail;
}

inline size_t encodeShotCurveSection(const ShotCurveRecord &curve, uint8_t *out) {
  const size_t bits = (curve.count + 7U) / 8U;
  out[0] = static_cast<uint8_t>(curve.count);
  out[1] = static_cast<uint8_t>(curve.count >> 8);
  out[2] = curve.truncated ? 1U : 0U;
  out[3] = 0;
  memcpy(out + 4, &curve.atmClearedMs, 18);
  uint8_t *p = out + SHOT_CURVE_SECTION_FIXED_BYTES;
  memcpy(p, curve.stream, curve.bytes);
  p += curve.bytes;
  memcpy(p, curve.breakBefore, bits);
  memcpy(p + bits, curve.held, bits);
  clearShotCurveTailBits(p, p + bits, curve.count);
  return static_cast<size_t>(p + 2U * bits - out);
}

// A section of `bytes` read through read(offset, out, count), straight into
// the record; false unless it is a valid curve.
template <typename Read>
inline bool decodeShotCurveSection(size_t bytes, uint32_t shotId, ShotCurveRecord &curve,
                                   Read &&read) {
  uint8_t fixed[SHOT_CURVE_SECTION_FIXED_BYTES];
  resetShotCurveRecord(curve);
  if (bytes < sizeof(fixed) || !read(0, fixed, sizeof(fixed))) return false;
  curve.shotId = shotId;
  curve.count = static_cast<uint16_t>(fixed[0] | fixed[1] << 8);
  curve.truncated = fixed[2] != 0;
  memcpy(&curve.atmClearedMs, fixed + 4, 18);
  const size_t bits = (curve.count + 7U) / 8U, framing = sizeof(fixed) + 2U * bits;
  if (curve.count > SHOT_CURVE_MAX_POINTS || fixed[2] > 1U || fixed[3] != 0U ||
      bytes < framing || bytes - framing > SHOT_CURVE_STREAM_BYTES)
    return false;
  curve.bytes = static_cast<uint16_t>(bytes - framing);
  ShotCurveReader walk;
  if (!read(sizeof(fixed), curve.stream, curve.bytes) ||
      !read(sizeof(fixed) + curve.bytes, curve.breakBefore, bits) ||
      !read(sizeof(fixed) + curve.bytes + bits, curve.held, bits) ||
      !shotCurveSeek(curve, curve.count, walk))
    return false;
  curve.lastAtMs = walk.atMs;
  curve.lastWeightCg = walk.weightCg;
  clearShotCurveTailBits(curve.breakBefore, curve.held, curve.count);
  return validShotCurveRecord(curve);
}

class ShotCurveCache {
 public:
  // A finished shot's curve, the newest, pending its pool record. A full
  // cache drops its oldest curve. Retrying an append is safe.
  bool append(const ShotCurveRecord &curve) {
    if (!validShotCurveRecord(curve)) return false;
    if (containsShotId(curve.shotId)) return true;
    copyShotCurve(store_.records[store_.header.writeIndex], curve);
    store_.header.writeIndex = (store_.header.writeIndex + 1U) % SHOT_CURVE_CAPACITY;
    if (store_.header.count < SHOT_CURVE_CAPACITY) ++store_.header.count;
    if (pending_ < store_.header.count) ++pending_;
    return true;
  }

  // Cache load after boot: a curve read back from the pool joins the oldest
  // end while there is room.
  bool prependSaved(const ShotCurveRecord &curve) {
    if (full() || !validShotCurveRecord(curve) || containsShotId(curve.shotId)) return false;
    copyShotCurve(newestSlot(store_.header.count), curve);
    ++store_.header.count;
    return true;
  }

  bool removeById(uint32_t id) {
    if (!containsShotId(id)) return false;
    compactShotCurveStore(store_);
    for (size_t i = 0; i < store_.header.count; ++i) {
      if (store_.records[i].shotId != id) continue;
      if (i + pending_ >= store_.header.count) --pending_;
      memmove(store_.records + i, store_.records + i + 1U,
              (store_.header.count - i - 1U) * sizeof(ShotCurveRecord));
      --store_.header.count;
      store_.header.writeIndex = store_.header.count % SHOT_CURVE_CAPACITY;
      return true;
    }
    return false;
  }

  // Clear all and factory reset; the generation tells the pool worker that
  // a curve it holds belongs to the cleared history.
  void clear() {
    resetShotCurveStore(store_);
    pending_ = 0;
    ++generation_;
  }

  // The pool worker saves the pending curves oldest first.
  const ShotCurveRecord *oldestPending() const {
    return pending_ == 0 ? nullptr : &newestSlot(pending_ - 1U);
  }
  void markSaved(uint32_t id) {
    if (pending_ != 0 && newestSlot(pending_ - 1U).shotId == id) --pending_;
  }
  size_t pending() const { return pending_; }
  uint32_t generation() const { return generation_; }
  bool full() const { return store_.header.count == SHOT_CURVE_CAPACITY; }
  size_t count() const { return store_.header.count; }

  const ShotCurveRecord *findNewestById(uint32_t id) const {
    if (id == 0) return nullptr;
    for (size_t n = 0; n < store_.header.count; ++n)
      if (newestSlot(n).shotId == id) return &newestSlot(n);
    return nullptr;
  }
  bool containsShotId(uint32_t id) const { return findNewestById(id) != nullptr; }
  bool copyByShotId(uint32_t id, ShotCurveRecord &out) const {
    const ShotCurveRecord *record = findNewestById(id);
    if (record != nullptr) copyShotCurve(out, *record);
    return record != nullptr;
  }
  size_t copyNewestFirst(ShotCurveRecord *out, size_t capacity) const {
    const size_t count = store_.header.count < capacity ? store_.header.count : capacity;
    for (size_t n = 0; out != nullptr && n < count; ++n) copyShotCurve(out[n], newestSlot(n));
    return out == nullptr ? 0 : count;
  }

 private:
  // n-th newest slot of the ring (0 = newest); n == count is the free slot
  // before the oldest.
  ShotCurveRecord &newestSlot(size_t n) {
    return store_.records[(store_.header.writeIndex + 2U * SHOT_CURVE_CAPACITY - 1U - n) %
                          SHOT_CURVE_CAPACITY];
  }
  const ShotCurveRecord &newestSlot(size_t n) const {
    return const_cast<ShotCurveCache *>(this)->newestSlot(n);
  }

  ShotCurveStore store_{};
  uint16_t pending_ = 0;
  uint32_t generation_ = 0;
};

}  // namespace openbrewbyweight
