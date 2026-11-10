#pragma once

// Beanconqueror machine endpoints (docs/schemas/README.md): one good shot as a
// Gaggiuino or a GaggiMate serves it. Pure, allocation-free, and pinned by
// docs/schemas/beanconqueror-v1.vectors.json, which the companion reproduces.

#include "OpenBrewByWeightShotCurveTypes.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace openbrewbyweight {

constexpr uint32_t BEANCONQUEROR_NONE = UINT32_MAX;
constexpr size_t BEANCONQUEROR_INDEX_HEADER = 32, BEANCONQUEROR_INDEX_ENTRY = 128;
constexpr size_t BEANCONQUEROR_SLOG_HEADER = 512, BEANCONQUEROR_SLOG_SAMPLE = 30;

// What both formats send; built from a good record only.
struct BeanconquerorShot {
  uint32_t id = 0, startUnix = 0, durationDs = 0;
  int32_t yieldCg = 0;
  uint16_t doseDg = 0, targetTempDeciC = 0, avgFlowCgS = 0;
  uint16_t firstDropDs = SHOT_LOG_METRIC_MISSING;  // the flow's fallback first drop
  uint8_t presetId = 0, rating = 0;
  char presetName[SHOT_PRESET_NAME_CAPACITY] = {};
};

inline bool beanconquerorShotOf(const ShotLogRecord &r, BeanconquerorShot &out) {
  if (shotLogBadReason(r) != nullptr) return false;
  out = {};
  out.id = r.id;
  out.durationDs = r.durationDs;
  if (r.hasWallTime && r.endedAtUnixSec > 0)
    out.startUnix = static_cast<uint32_t>((uint64_t{r.endedAtUnixSec} * 10U - r.durationDs + 5U) / 10U);
  out.yieldCg = r.actualWeightCg;  // a good shot always has its measured yield
  out.doseDg = r.doseDg;
  out.targetTempDeciC = r.targetTempDeciC;
  out.avgFlowCgS = r.avgFlowCgS == SHOT_LOG_METRIC_MISSING ? 0 : r.avgFlowCgS;
  out.presetId = shotLogPresetId(r);
  out.rating = shotLogRating(r.sideFlags);
  out.firstDropDs = r.firstDropDs;
  memcpy(out.presetName, r.presetName, sizeof(out.presetName));
  out.presetName[sizeof(out.presetName) - 1] = '\0';
  return true;
}

inline uint16_t beanconquerorDg(int32_t cg) { return static_cast<uint16_t>((cg < 0 ? 0 : cg + 5) / 10); }

// The Stats page's weight flow (runtime.js buildShotSparkModel().flowCurve),
// quantized per reading: tenths and hundredths of g/s, 0 where it has none.
// Each step is its own statement so no compiler fuses it: the JS and Python
// ports compute the same IEEE doubles.
struct BeanconquerorFlowWork {
  struct Observation {
    uint16_t t, index, segment;
    int16_t cg;
  } obs[SHOT_CURVE_MAX_POINTS];
  int32_t touched[128][2];
  int16_t tenths[SHOT_CURVE_MAX_POINTS], hundredths[SHOT_CURVE_MAX_POINTS];
};

inline void beanconquerorFlow(const ShotCurveRecord &curve, const BeanconquerorShot &shot,
                              BeanconquerorFlowWork &w) {
  memset(w.tenths, 0, sizeof(w.tenths));
  memset(w.hundredths, 0, sizeof(w.hundredths));
  const auto at = [](uint16_t ms) { return ms == SHOT_LOG_METRIC_MISSING ? BEANCONQUEROR_NONE : uint32_t{ms}; };
  const uint32_t end = at(curve.ended.atMs), atm = at(curve.atm.atMs), clear = at(curve.atmClearedMs);
  const uint32_t limit = end != BEANCONQUEROR_NONE ? end : shot.durationDs * 100U;
  const uint32_t drop = curve.firstDrop.atMs != SHOT_LOG_METRIC_MISSING ? curve.firstDrop.atMs
                        : shot.firstDropDs != SHOT_LOG_METRIC_MISSING  ? shot.firstDropDs * 100U
                                                                       : BEANCONQUEROR_NONE;
  size_t observations = 0, windows = 0;
  uint16_t segment = 0;
  ShotCurveReader reader{curve.stream, curve.bytes};
  for (uint16_t i = 0; i < curve.count && reader.next(); ++i) {
    if (shotCurveBreakBefore(curve, i)) segment++;
    const uint32_t t = reader.atMs;
    if (t > limit && end == BEANCONQUEROR_NONE) continue;
    if (shotCurveHeld(curve, i)) {
      const int32_t from = static_cast<int32_t>(t) - 500;
      if (windows && w.touched[windows - 1][1] >= from) w.touched[windows - 1][1] = static_cast<int32_t>(t);
      else if (windows < 128) w.touched[windows][0] = from, w.touched[windows++][1] = static_cast<int32_t>(t);
      else w.touched[windows - 1][1] = static_cast<int32_t>(t);  // never reached: ≤ 121 windows in 60 s
      continue;
    }
    if (atm != BEANCONQUEROR_NONE && t >= atm && (clear == BEANCONQUEROR_NONE || t < clear)) {
      segment++;
      continue;
    }
    const BeanconquerorFlowWork::Observation *last = observations ? &w.obs[observations - 1] : nullptr;
    if (last && atm != BEANCONQUEROR_NONE && last->t < atm && t >= atm) segment++;
    if (last && clear != BEANCONQUEROR_NONE && last->t < clear && t >= clear) segment++;
    const BeanconquerorFlowWork::Observation o{static_cast<uint16_t>(t), i, segment, reader.weightCg};
    if (last && last->segment == segment && last->t == t) w.obs[observations - 1] = o;
    else w.obs[observations++] = o;
  }
  size_t boundary = 0;
  for (size_t i = 0; i < observations; ++i) {
    const auto &p = w.obs[i];
    if (!i || p.segment != w.obs[i - 1].segment) {
      boundary = i;
      continue;
    }
    const int32_t span = p.t - w.obs[i - 1].t > 1000 ? p.t - w.obs[i - 1].t : 1000;
    const int32_t start = p.t - span;
    while (boundary + 1 < i && w.obs[boundary + 1].t <= start) boundary++;
    const auto &a = w.obs[boundary], &b = w.obs[boundary + 1];
    bool skip = a.segment != p.segment || a.t > start ||
                (drop != BEANCONQUEROR_NONE && a.t < drop);
    for (size_t k = 0; k < windows && !skip; ++k)
      skip = start <= w.touched[k][1] && p.t >= w.touched[k][0];
    if (skip) continue;
    double startCg = a.cg;
    if (a.t != start) {
      const double scaled = static_cast<double>(int64_t{b.cg - a.cg} * (start - a.t));
      const double step = scaled / (b.t - a.t);
      startCg = a.cg + step;
    }
    const double gain = p.cg - startCg;
    const double gain10 = gain * 10;
    double rate = gain10 / span;
    if (rate < 0) rate = 0;
    const double r10 = rate * 10, r100 = rate * 100;
    w.tenths[p.index] = static_cast<int16_t>(floor(r10 + 0.5));
    const double h = floor(r100 + 0.5);
    w.hundredths[p.index] = static_cast<int16_t>(h > 32767 ? 32767 : h);
  }
}

// Bytes and text into a caller buffer. With `flush`, a full buffer is handed
// on (an HTTP chunk) and reused, so an answer has no size bound; without it,
// `ok` turns false on overflow.
struct BeanconquerorOut {
  uint8_t *data = nullptr;
  size_t capacity = 0, used = 0;
  bool (*flush)(void *context, const uint8_t *bytes, size_t length) = nullptr;
  void *context = nullptr;
  bool ok = true;
  bool room(size_t n) {
    if (ok && used + n > capacity && flush) {
      ok = flush(context, data, used);
      used = 0;
    }
    if (ok && used + n > capacity) ok = false;
    return ok;
  }
  void bytes(const void *src, size_t n) {
    if (room(n)) memcpy(data + used, src, n), used += n;
  }
  void zeros(size_t n) {
    for (; n > 64 && ok; n -= 64) zeros(64);
    if (room(n)) memset(data + used, 0, n), used += n;
  }
  void u8(uint8_t v) { bytes(&v, 1); }
  void u16(uint16_t v) { const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; bytes(b, 2); }
  void u32(uint32_t v) { const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; bytes(b, 4); }
  void text(const char *format, ...) __attribute__((format(printf, 2, 3))) {
    char piece[96];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(piece, sizeof(piece), format, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(piece)) ok = false;
    else bytes(piece, static_cast<size_t>(n));
  }
  // The rest of the answer; false if any part failed.
  bool finish() {
    if (ok && flush && used) ok = flush(context, data, used), used = 0;
    return ok;
  }
  // A JSON string body: quote, backslash and control bytes escaped, UTF-8 kept.
  void jsonText(const char *s) {
    for (; *s; ++s) {
      const uint8_t c = static_cast<uint8_t>(*s);
      if (c == '"' || c == '\\') text("\\%c", c);
      else if (c < 0x20) text("\\u%04x", c);
      else bytes(&c, 1);
    }
  }
};

// Each non-held reading, in order: emit(index, atMs, weightCg). A shot without
// one gets a single closing reading (its time, its yield, index 0 with no flow),
// since Beanconqueror cannot import a shot with no samples.
template <typename Emit>
inline void beanconquerorSamples(const BeanconquerorShot &s, const ShotCurveRecord &curve,
                                 Emit &&emit) {
  ShotCurveReader reader{curve.stream, curve.bytes};
  bool any = false;
  for (uint16_t i = 0; i < curve.count && reader.next(); ++i)
    if (!shotCurveHeld(curve, i)) emit(i, reader.atMs, reader.weightCg), any = true;
  if (!any)
    emit(uint16_t{0}, static_cast<uint16_t>(s.durationDs * 100U > 65535U ? 65535U : s.durationDs * 100U),
         static_cast<int16_t>(s.yieldCg > 32767 ? 32767 : s.yieldCg));
}

inline void beanconquerorGaggiuinoShot(const BeanconquerorShot &s, const ShotCurveRecord &curve,
                                       const BeanconquerorFlowWork &w, BeanconquerorOut &out) {
  out.text("{\"id\":%lu,", static_cast<unsigned long>(s.id));
  if (s.startUnix) out.text("\"timestamp\":%lu,", static_cast<unsigned long>(s.startUnix));
  out.text("\"duration\":%lu,\"datapoints\":{", static_cast<unsigned long>(s.durationDs));
  const char *comma = "";
  const auto series = [&](const char *key, auto value) {
    out.text("%s\"%s\":[", comma, key);
    comma = ",";
    bool first = true;
    beanconquerorSamples(s, curve, [&](uint16_t i, uint16_t ms, int16_t cg) {
      out.text(first ? "%d" : ",%d", value(i, ms, cg));
      first = false;
    });
    out.text("]");
  };
  const int temp = s.targetTempDeciC;
  series("timeInShot", [](uint16_t, uint16_t ms, int16_t) { return (ms + 50) / 100; });
  series("pressure", [](uint16_t, uint16_t, int16_t) { return 0; });
  series("pumpFlow", [&](uint16_t i, uint16_t, int16_t) { return int{w.tenths[i]}; });
  series("weightFlow", [&](uint16_t i, uint16_t, int16_t) { return int{w.tenths[i]}; });
  series("temperature", [&](uint16_t, uint16_t, int16_t) { return temp; });
  series("shotWeight", [](uint16_t, uint16_t, int16_t cg) { return int{beanconquerorDg(cg)}; });
  series("waterPumped", [](uint16_t, uint16_t, int16_t) { return 0; });
  series("targetTemperature", [&](uint16_t, uint16_t, int16_t) { return temp; });
  series("targetPumpFlow", [](uint16_t, uint16_t, int16_t) { return 0; });
  series("targetPressure", [](uint16_t, uint16_t, int16_t) { return 0; });
  out.text("},\"profile\":{\"id\":%u,\"name\":\"", static_cast<unsigned>(s.presetId));
  out.jsonText(s.presetName);
  out.text("\",\"waterTemperature\":%d.%d}}", temp / 10, temp % 10);
}

// A C string into a fixed field: at most `max` bytes, never splitting a
// UTF-8 character, zero-padded to `field`.
inline void beanconquerorCString(BeanconquerorOut &out, const char *text, size_t max, size_t field) {
  size_t n = strlen(text);  // always terminated: the record and a local id
  if (n > max) {
    n = max;
    while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) n--;
  }
  out.bytes(text, n);
  out.zeros(field - n);
}

inline void beanconquerorIndexHeader(BeanconquerorOut &out, uint32_t count, uint32_t nextId) {
  out.u32(0x58444953);  // 'SIDX'
  out.u16(1);
  out.u16(BEANCONQUEROR_INDEX_ENTRY);
  out.u32(count);
  out.u32(nextId);
  out.zeros(16);
}

inline void beanconquerorIndexEntry(BeanconquerorOut &out, const BeanconquerorShot &s) {
  char id[8];
  snprintf(id, sizeof(id), "%u", static_cast<unsigned>(s.presetId));
  out.u32(s.id);
  out.u32(s.startUnix);
  out.u32(s.durationDs * 100U);
  out.u16(beanconquerorDg(s.yieldCg));
  out.u8(s.rating);
  out.u8(0x01 | (s.doseDg ? 0x04 : 0));  // completed, has notes
  beanconquerorCString(out, id, 31, 32);
  beanconquerorCString(out, s.presetName, 47, 48);
  out.u16(0);  // avgTemp: not measured
  out.u16(0);  // maxPressure: not measured
  out.u16(s.avgFlowCgS);
  out.zeros(26);
}

inline void beanconquerorSlog(BeanconquerorOut &out, const BeanconquerorShot &s,
                              const ShotCurveRecord &curve, const BeanconquerorFlowWork &w) {
  uint32_t count = 0;
  beanconquerorSamples(s, curve, [&](uint16_t, uint16_t, int16_t) { count++; });
  char id[8];
  snprintf(id, sizeof(id), "%u", static_cast<unsigned>(s.presetId));
  out.u32(0x544F4853);  // 'SHOT'
  out.u8(7);
  out.u8(BEANCONQUEROR_SLOG_SAMPLE);
  out.u16(BEANCONQUEROR_SLOG_HEADER);
  out.u16(50);  // nominal interval: the curve's finest step
  out.u16(0);
  out.u32(0x3FFF);
  out.u32(count);
  out.u32(s.durationDs * 100U);
  out.u32(s.startUnix);
  beanconquerorCString(out, id, 31, 32);
  beanconquerorCString(out, s.presetName, 47, 48);
  out.u16(beanconquerorDg(s.yieldCg));
  out.zeros(BEANCONQUEROR_SLOG_HEADER - 110);  // no phase transitions
  beanconquerorSamples(s, curve, [&](uint16_t i, uint16_t ms, int16_t cg) {
    out.u32(ms);
    out.u16(s.targetTempDeciC);
    out.zeros(12);  // ct, tp, cp, fl, tf, pf
    out.u16(static_cast<uint16_t>(w.hundredths[i]));
    out.u16(beanconquerorDg(cg));
    out.zeros(8);  // ev, pr, si, wp
  });
}

}  // namespace openbrewbyweight
