// Web UI Stats page projection: the shot rows and the header (the selected
// window's aggregate and the charts the page draws, built once per store
// epoch, window, or active preset). Included by the UI stream ahead of the
// Stats sender.

// Stats row columns, sent once per page; formatShotStatsRow emits the values
// in this order followed by the curve object.
constexpr char kStatsRowColumns[] =
    "id,bootId,endedAtMs,hasWallTime,endedAtLocalSec,endedAtUnixSec,"
    "timezoneOffsetMinutesAtCommit,durationS,presetId,presetName,scaleName,goalG,"
    "actualG,offsetG,bbwAlgorithm,bbwAlgorithmVersion,bbwAlpha,bbwLearningApplied,"
    "avgFlowGS,targetTempDeciC,doseG,ratio,firstDropS,tareS,shotType,cutType,"
    "extractionGuardEnabled,extractionExtended,slowExtractionGuardEnabled,"
    "slowExtractionExtended,stopDetail,cutLineG,arrivalS,verdict,channeling,"
    "scaleLossSource,estimatedG,blind,fastBeforeS,slowAfterS,fastMarginG,"
    "slowMarginG,delayS,wallS,fastRampS,slowRampS,fastCurve,slowCurve,fastCurveHardness,"
    "slowCurveHardness,dripDelayS,actualWeightSource,rating,badReason";

// The Stats row projection (extracted from the former REST shots handler):
// row values, then the curve object that formatShotCurveJsonBody fills with
// its inner fields. Writes into
// work.jsonItem using work.curveJson as scratch and returns the written
// length, 0 on overflow. A missing curve formats as an empty one in the
// Stats fetch scratch, which a failed fetch has already left unused.
size_t OpenBrewByWeightNetwork::formatShotStatsRow(NetworkWorkBuf &work,
                                              const ShotLogRecord &record,
                                              const ShotCurveRecord *curve) {
  char actual[16] = "null";
  char flow[16] = "null";
  char firstDrop[16] = "null";
  char tareAt[16] = "null";
  char cutLine[16] = "null", arrival[16] = "null", dripDelay[8] = "null";
  char bad[16] = "null";
  if (const char *reason = shotLogBadReason(record)) snprintf(bad, sizeof(bad), "\"%s\"", reason);
  char presetName[SHOT_PRESET_NAME_CAPACITY * 6] = {};
  char scaleNameJson[SCALE_FRIENDLY_NAME_MAX_LEN * 6 + 1] = {};
  escapeJsonString(record.presetName, presetName, sizeof(presetName));
  escapeJsonString(record.scaleName, scaleNameJson, sizeof(scaleNameJson));
  if (!shotLogWeightIsMissing(record.actualWeightCg)) {
    snprintf(actual, sizeof(actual), "%.2f",
             static_cast<double>(record.actualWeightCg) / 100.0);
  }
  if (record.avgFlowCgS != SHOT_LOG_METRIC_MISSING) {
    snprintf(flow, sizeof(flow), "%.2f",
             static_cast<double>(record.avgFlowCgS) / 100.0);
  }
  if (record.firstDropDs != SHOT_LOG_METRIC_MISSING) {
    snprintf(firstDrop, sizeof(firstDrop), "%.1f",
             static_cast<double>(record.firstDropDs) / 10.0);
  }
  if (record.tareAtDs != SHOT_LOG_METRIC_MISSING) {
    snprintf(tareAt, sizeof(tareAt), "%.1f",
             static_cast<double>(record.tareAtDs) / 10.0);
  }
  if (!shotLogWeightIsMissing(record.cutLineCg)) {
    snprintf(cutLine, sizeof(cutLine), "%.2f",
             static_cast<double>(record.cutLineCg) / 100.0);
  }
  if (record.arrivalDs != SHOT_LOG_METRIC_MISSING) {
    snprintf(arrival, sizeof(arrival), "%.1f",
             static_cast<double>(record.arrivalDs) / 10.0);
  }
  if (record.dripDelayCode) snprintf(dripDelay, sizeof(dripDelay), "%.1f", (record.dripDelayCode - 1) / 10.0);
  const bool scaleLoss = static_cast<ShotLogStopDetail>(record.stopDetail) ==
                         ShotLogStopDetail::AUTO_TO_MANUAL;
  ShotCurveRecord &emptyCurve = work.statsCache.fetchCurve;
  if (curve == nullptr) {
    resetShotCurveRecord(emptyCurve);
    curve = &emptyCurve;
  }
  char *const curveJson = work.curveJson;
  char alphaJson[5] = "null";
  const unsigned alpha = shotLogBbwAlpha(record);
  if (alpha) snprintf(alphaJson, sizeof(alphaJson), "%u.%02u", alpha / 100, alpha % 100);
  if (!formatShotCurveJsonBody(curveJson, SHOT_CURVE_JSON_CAPACITY, *curve) &&
      (resetShotCurveRecord(emptyCurve),
       !formatShotCurveJsonBody(curveJson, SHOT_CURVE_JSON_CAPACITY, emptyCurve))) {
    // Keep the row JSON parseable; an empty curve beats a broken page.
    curveJson[0] = '\0';
  }
  // Values in kStatsRowColumns order, then the curve body as one object.
  const int bytes = snprintf(work.jsonItem, NetworkWorkBuf::kJsonItem,
      "[%lu,%lu,%lu,%s,%lu,%lu,%d,%.1f,%u,\"%s\",\"%s\",%.1f,%s,%.2f,\"%s\",%s,"
      "%s,%s,%s,%u,%.1f,%.2f,%s,%s,\"%s\",\"%s\",%s,%s,%s,%s,\"%s\",%s,%s,%s,%s,"
      "%s,%s,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,\"%s\",\"%s\",%.1f,%.1f,%s,"
      "\"%s\",%u,%s,{%s}]",
      static_cast<unsigned long>(record.id),
      static_cast<unsigned long>(record.bootId),
      static_cast<unsigned long>(record.endedAtMs),
      record.hasWallTime ? "true" : "false",
      static_cast<unsigned long>(record.endedAtLocalSec),
      static_cast<unsigned long>(record.endedAtUnixSec),
      static_cast<int>(record.timezoneOffsetMinutesAtCommit),
      static_cast<double>(record.durationDs) / 10.0,
      static_cast<unsigned>(shotLogPresetId(record)),
      presetName, scaleNameJson,
      static_cast<double>(goalWeightDgToGrams(record.goalWeightDg)), actual,
      static_cast<double>(record.offsetUsedCg) / 100.0,
      shotLogBbwAlgorithm(record), shotLogBbwVersion(record),
      alphaJson, shotLogBbwLearningApplied(record), flow,
      static_cast<unsigned>(record.targetTempDeciC),
      static_cast<double>(record.doseDg) / 10.0,
      static_cast<double>(record.ratioX100) / 100.0,
      firstDrop, tareAt,
      shotLogTypeName(shotLogType(record)),
      shotLogCutName(shotLogCut(record)),
      shotLogFastGuardEnabled(record.sideFlags) ? "true" : "false",
      shotLogFastExtended(record.extractionExtended) ? "true" : "false",
      shotLogSlowGuardEnabled(record.sideFlags) ? "true" : "false",
      shotLogSlowExtended(record.extractionExtended) ? "true" : "false",
      shotLogStopDetailName(static_cast<ShotLogStopDetail>(record.stopDetail)),
      cutLine, arrival, shotLogVerdictJson(record.verdict),
      corridor::channelingOf(record.verdict) ? "true" : "false",
      scaleLoss ? shotLogScaleLossSourceJson(record.verdict) : "null",
      // Only a projected stop time lands near the cut line.
      scaleLoss && corridor::lossSourceOf(record.verdict) == corridor::LossSource::PROJECTION
          ? cutLine : "null",
      corridor::blindOf(record.verdict) ? "true" : "false",
      record.fastBeforeDs / 10.0, record.slowAfterDs / 10.0,
      record.fastMarginDg / 10.0, record.slowMarginDg / 10.0,
      record.delayDs / 10.0, record.wallDs / 10.0, record.cutRampDs / 10.0,
      shotLogSlowRampDs(record) / 10.0,
      rampCurveName(corridor::rampCurveTypeOf(record.fastCurve)),
      rampCurveName(corridor::rampCurveTypeOf(record.slowCurve)),
      // The stored snapshot keeps the nearest curve (shot-log schema).
      rampHardnessPctOf(corridor::rampCurveHardnessOf(record.fastCurve)) / 2.0,
      rampHardnessPctOf(corridor::rampCurveHardnessOf(record.slowCurve)) / 2.0, dripDelay,
      actualWeightSourceName(static_cast<ActualWeightSource>(record.actualWeightSource)),
      static_cast<unsigned>(shotLogRating(record.sideFlags)), bad,
      curveJson);
  if (bytes <= 0 || static_cast<size_t>(bytes) >= NetworkWorkBuf::kJsonItem)
    return 0;
  return static_cast<size_t>(bytes);
}

// One Stats density chart over the `n` timed shots in cache.scope: each
// arrival mapped into its own fast/slow window, scaled to the windows' mean
// width, then a Gaussian kernel sampled at 121 points and scaled to its own
// peak, so the curve looks the same however many shots share it. Heights
// are 0–100; the client interpolates the zone edges between samples. Terms
// beyond four bandwidths round to zero there and are skipped.
__attribute__((optimize("Os")))
bool appendStatsDensity(StatsStreamCache &cache, size_t n, size_t *used) {
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
// ids ("p", one number when they share one), for the consistency chart;
// and "facets", the presets and scales the good shots of the whole log
// hold, newest first, for the list filter (scales keyed by shotLogScaleKey).
__attribute__((optimize("Os")))
bool formatStatsHeader(StatsStreamCache &cache, size_t window, uint8_t presetId) {
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
    own += shotLogPresetId(record) == presetId;
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
  if (!jsonScratchAppend(out, capacity, &used, "%s},\"facets\":{\"presets\":[",
                         onePreset ? "" : "]"))
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
  return jsonScratchAppend(out, capacity, &used, "]}}");
}
