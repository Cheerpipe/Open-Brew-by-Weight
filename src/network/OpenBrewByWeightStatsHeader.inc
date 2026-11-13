// The Web UI Stats header (formatStatsHeader): the aggregate, the density
// charts, the consistency series, the filter facets and the chosen preset's
// cut series over a newest-first store copy. Self-contained so the host test runs it over the shared golden
// vectors (docs/schemas/stats-header-v1.vectors.json) that the companion's
// port of it reproduces. Included by OpenBrewByWeightNetwork.cpp ahead of the
// Stats stream cache, which extends StatsHeaderWork.

struct StatsHeaderWork {
  ShotStatsRecordsSnapshot snapshot{};
  // The selected window's timed shots, newest first, and the density
  // chart's working values, off the dispatch stack.
  const ShotLogRecord *timed[SHOT_LOG_STATS_WINDOW_MAX] = {};
  const ShotLogRecord *scope[SHOT_LOG_STATS_WINDOW_MAX] = {};
  float mapped[SHOT_LOG_STATS_WINDOW_MAX] = {}, heights[121] = {};
  // Aggregate (~0.3 KB), two density charts (~1.2 KB), the consistency
  // series (~0.8 KB at 100 shots), the filter facets (~0.6 KB typical) and
  // the cut series (~0.8 KB at 100 shots).
  char header[6144]{};
};

bool jsonScratchAppend(char *buf, size_t cap, size_t *used, const char *fmt,
                       ...) {
  if (buf == nullptr || used == nullptr || *used >= cap) {
    return false;
  }
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf + *used, cap - *used, fmt, args);
  va_end(args);
  if (n < 0 || static_cast<size_t>(n) >= cap - *used) {
    return false;
  }
  *used += static_cast<size_t>(n);
  return true;
}

// The guard that decided a shot's stop: 1 the fast-shot guard extended it,
// 2 the slow-shot guard shortened it, 0 none.
uint8_t statsGuardStop(const ShotLogRecord &record) {
  switch (static_cast<ShotLogStopDetail>(record.stopDetail)) {
    case ShotLogStopDetail::EXTENDED_MAX_WEIGHT:
    case ShotLogStopDetail::EXTENDED_MIN_TIME:
      return 1;
    case ShotLogStopDetail::SLOW_MAX_TIME:
    case ShotLogStopDetail::SLOW_MIN_WEIGHT:
      return 2;
    default:
      return 0;
  }
}

// `value` over the first `n` shots of cache.scope, oldest first, after
// `open` (such as `,"g":[`); nothing when `onlyIfSet` and every value is zero.
template <typename Value>
bool appendStatsSeries(StatsHeaderWork &cache, size_t n, size_t *used, const char *open,
                       bool onlyIfSet, Value value) {
  bool set = !onlyIfSet;
  for (size_t i = 0; i < n && !set; ++i) set = value(*cache.scope[i]) != 0;
  if (!set) return true;
  if (!jsonScratchAppend(cache.header, sizeof(cache.header), used, "%s", open)) return false;
  for (size_t i = n; i-- > 0;)
    if (!jsonScratchAppend(cache.header, sizeof(cache.header), used, "%s%ld", i + 1 < n ? "," : "",
                           static_cast<long>(value(*cache.scope[i]))))
      return false;
  return jsonScratchAppend(cache.header, sizeof(cache.header), used, "]");
}

// One Stats density chart over the `n` timed shots in cache.scope: each
// arrival mapped into its own fast/slow window, scaled to the windows' mean
// width, then a Gaussian kernel sampled at 121 points and scaled to its own
// peak, so the curve looks the same however many shots share it. Heights
// are 0–100; the client interpolates the zone edges between samples. Terms
// beyond four bandwidths round to zero there and are skipped.
__attribute__((optimize("Os")))
bool appendStatsDensity(StatsHeaderWork &cache, size_t n, size_t *used) {
  const ShotLogRecord *const *shots = cache.scope;
  bool shared = true;
  float width = 0;
  unsigned counts[3] = {};
  for (size_t i = 0; i < n; ++i) {
    shared = shared && shots[i]->fastBeforeDs == shots[0]->fastBeforeDs &&
             shots[i]->slowAfterDs == shots[0]->slowAfterDs;
    width += (shots[i]->slowAfterDs - shots[i]->fastBeforeDs) / 10.0f;
    ++counts[shotLogResult(*shots[i])];
  }
  const float a = shared ? shots[0]->fastBeforeDs / 10.0f : 0.0f,
              b = shared ? shots[0]->slowAfterDs / 10.0f : roundf(width / n);
  float low = a - 5, high = b + 5;
  for (size_t i = 0; i < n; ++i) {
    const float t = shots[i]->arrivalDs / 10.0f, f = shots[i]->fastBeforeDs / 10.0f,
                s = shots[i]->slowAfterDs / 10.0f;
    const float x = t < f ? a + t - f : t > s ? b + t - s
                    : s > f ? a + (t - f) / (s - f) * (b - a) : a;
    cache.mapped[i] = x;
    low = fminf(low, x);
    high = fmaxf(high, x);
  }
  const int lo = static_cast<int>(floorf(low - 1)), hi = static_cast<int>(ceilf(high + 1));
  const float span = static_cast<float>(hi - lo), bandwidth = fmaxf(0.7f, span / 28);
  float peak = 0;
  for (size_t k = 0; k < 121; ++k) {
    const float x = lo + span * k / 120;
    float sum = 0;
    for (size_t i = 0; i < n; ++i) {
      const float z = (x - cache.mapped[i]) / bandwidth;
      if (fabsf(z) < 4) sum += expf(-0.5f * z * z);
    }
    cache.heights[k] = sum;
    peak = fmaxf(peak, sum);
  }
  if (!jsonScratchAppend(cache.header, sizeof(cache.header), used,
          "{\"lo\":%d,\"hi\":%d,\"a\":%.1f,\"b\":%.1f,\"shared\":%s,"
          "\"n\":[%u,%u,%u],\"h\":[",
          lo, hi, a, b, shared ? "true" : "false", counts[1], counts[0], counts[2]))
    return false;
  for (size_t k = 0; k < 121; ++k)
    if (!jsonScratchAppend(cache.header, sizeof(cache.header), used, "%s%ld", k ? "," : "",
                           lroundf(100 * cache.heights[k] / peak)))
      return false;
  return jsonScratchAppend(cache.header, sizeof(cache.header), used, "]}");
}

// The Web UI Stats header over the newest `window` eligible shots: the
// aggregate; "dens", the density charts for all presets and for `presetId`
// (null without its timed shots, true when it equals the first); "cons",
// the timed arrivals oldest first in tenths of a second with their preset
// ids ("p", one number when they share one) and, when any guard decided a
// stop, the guard per shot ("g", statsGuardStop), for the consistency chart;
// "facets", the presets and scales the good shots of the whole log hold,
// newest first, for the list filter (scales keyed by shotLogScaleKey); and
// "cut", for the cut accuracy chart: `presetId`'s newest `window` good weight
// cuts with a settled weight, oldest first, as their targets ("t", tenths of
// a gram, one number when they share one), the settled weight's miss from
// the target ("e", hundredths of a gram) and "g" as above; "preset", the
// `presetId` they follow, so a page can tell a header built for an earlier
// choice. `presetId` 0 is All: no preset chart and no "cut", as for a preset
// without such shots.
__attribute__((optimize("Os")))
bool formatStatsHeader(StatsHeaderWork &cache, size_t window, uint8_t presetId) {
  const ShotStatsRecordsSnapshot &snapshot = cache.snapshot;
  char *out = cache.header;
  const size_t capacity = sizeof(cache.header);
  const ShotStatsView stats = shotLogStatsView(snapshot.count, window,
      [&](size_t n) -> const ShotLogRecord & { return snapshot.records[n]; });
  if (!buildIntegrationStats(stats, out, capacity, window, false)) return false;
  size_t used = strlen(out) - 1;  // Reopen the aggregate over its closing brace.
  size_t timed = 0, own = 0;
  bool onePreset = true;
  for (size_t i = 0, found = 0; i < snapshot.count && found < window; ++i) {
    const ShotLogRecord &record = snapshot.records[i];
    if (!shotLogRecordEligible(record)) continue;
    ++found;
    if (shotLogResult(record) > 2) continue;
    onePreset = onePreset && (!timed || shotLogPresetId(record) == shotLogPresetId(*cache.timed[0]));
    cache.timed[timed++] = &record;
    own += presetId && shotLogPresetId(record) == presetId;
  }
  if (!jsonScratchAppend(out, capacity, &used, ",\"dens\":[")) return false;
  if (timed) {
    for (size_t i = 0; i < timed; ++i) cache.scope[i] = cache.timed[i];
    if (!appendStatsDensity(cache, timed, &used)) return false;
  } else if (!jsonScratchAppend(out, capacity, &used, "null")) {
    return false;
  }
  if (own && own < timed) {
    size_t n = 0;
    for (size_t i = 0; i < timed; ++i)
      if (shotLogPresetId(*cache.timed[i]) == presetId) cache.scope[n++] = cache.timed[i];
    if (!jsonScratchAppend(out, capacity, &used, ",") || !appendStatsDensity(cache, n, &used))
      return false;
  } else if (!jsonScratchAppend(out, capacity, &used, own ? ",true" : ",null")) {
    return false;
  }
  if (!jsonScratchAppend(out, capacity, &used, "],\"cons\":{\"t\":[")) return false;
  for (size_t i = timed; i-- > 0;)
    if (!jsonScratchAppend(out, capacity, &used, "%s%u", i + 1 < timed ? "," : "",
                           static_cast<unsigned>(cache.timed[i]->arrivalDs)))
      return false;
  if (!jsonScratchAppend(out, capacity, &used, onePreset ? "],\"p\":%u" : "],\"p\":[",
                         timed ? static_cast<unsigned>(shotLogPresetId(*cache.timed[0])) : 0U))
    return false;
  for (size_t i = timed; !onePreset && i-- > 0;)
    if (!jsonScratchAppend(out, capacity, &used, "%s%u", i + 1 < timed ? "," : "",
                           static_cast<unsigned>(shotLogPresetId(*cache.timed[i]))))
      return false;
  if (!onePreset && !jsonScratchAppend(out, capacity, &used, "]")) return false;
  for (size_t i = 0; i < timed; ++i) cache.scope[i] = cache.timed[i];
  if (!appendStatsSeries(cache, timed, &used, ",\"g\":[", true, statsGuardStop) ||
      !jsonScratchAppend(out, capacity, &used, "},\"facets\":{\"presets\":["))
    return false;
  uint8_t presets[SHOT_LIST_FILTER_MAX];
  uint32_t scales[SHOT_LIST_FILTER_MAX];
  size_t presetCount = 0, scaleCount = 0;
  char name[SCALE_FRIENDLY_NAME_MAX_LEN * 6 + 1];
  for (size_t i = 0; i < snapshot.count && presetCount < SHOT_LIST_FILTER_MAX; ++i) {
    const uint8_t id = shotLogPresetId(snapshot.records[i]);
    bool seen = !shotLogRecordEligible(snapshot.records[i]);
    for (size_t k = 0; k < presetCount; ++k) seen = seen || presets[k] == id;
    if (seen) continue;
    presets[presetCount] = id;
    escapeJsonString(snapshot.records[i].presetName, name, sizeof(name));
    if (!jsonScratchAppend(out, capacity, &used, "%s[%u,\"%s\"]", presetCount++ ? "," : "",
                           static_cast<unsigned>(id), name))
      return false;
  }
  if (!jsonScratchAppend(out, capacity, &used, "],\"scales\":[")) return false;
  for (size_t i = 0; i < snapshot.count && scaleCount < SHOT_LIST_FILTER_MAX; ++i) {
    const ShotLogRecord &record = snapshot.records[i];
    const uint32_t key = shotLogScaleKey(record);
    bool seen = !record.scaleName[0] || !shotLogRecordEligible(record);
    for (size_t k = 0; k < scaleCount; ++k) seen = seen || scales[k] == key;
    if (seen) continue;
    scales[scaleCount] = key;
    escapeJsonString(record.scaleName, name, sizeof(name));
    if (!jsonScratchAppend(out, capacity, &used, "%s[%lu,\"%s\"]", scaleCount++ ? "," : "",
                           static_cast<unsigned long>(key), name))
      return false;
  }
  if (!jsonScratchAppend(out, capacity, &used, "]},\"preset\":%u", static_cast<unsigned>(presetId)))
    return false;
  size_t cuts = 0;
  bool oneTarget = true;
  for (size_t i = 0; presetId && i < snapshot.count && cuts < window; ++i) {
    const ShotLogRecord &record = snapshot.records[i];
    if (shotLogPresetId(record) != presetId || !shotLogRecordEligible(record) ||
        !shotLogBbwErrorEligible(record) || record.actualWeightCg == SHOT_LOG_WEIGHT_MISSING)
      continue;
    oneTarget = oneTarget && (!cuts || record.goalWeightDg == cache.scope[0]->goalWeightDg);
    cache.scope[cuts++] = &record;
  }
  if (cuts) {
    const auto target = [](const ShotLogRecord &r) { return r.goalWeightDg; };
    const auto miss = [](const ShotLogRecord &r) {
      return static_cast<int32_t>(r.actualWeightCg) - 10 * static_cast<int32_t>(r.goalWeightDg);
    };
    if (!jsonScratchAppend(out, capacity, &used, ",\"cut\":{") ||
        !(oneTarget ? jsonScratchAppend(out, capacity, &used, "\"t\":%u",
                                        static_cast<unsigned>(cache.scope[0]->goalWeightDg))
                    : appendStatsSeries(cache, cuts, &used, "\"t\":[", false, target)) ||
        !appendStatsSeries(cache, cuts, &used, ",\"e\":[", false, miss) ||
        !appendStatsSeries(cache, cuts, &used, ",\"g\":[", true, statsGuardStop) ||
        !jsonScratchAppend(out, capacity, &used, "}"))
      return false;
  }
  return jsonScratchAppend(out, capacity, &used, "}");
}
