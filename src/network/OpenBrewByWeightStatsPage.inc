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
