namespace {
// Formats a bounded non-negative stat, trimming to `decimals` places.
const char *formatStatFloat(char (&buffer)[32], double value, unsigned decimals) {
  snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  return buffer;
}
}  // namespace

// Pre-computed stats over the newest `window` shots (integration payload,
// always ten, and the Web UI Stats header, which leaves out the window, total
// and durations it never shows). External linkage with a header declaration:
// the shots-page handler is inlined into this translation unit before this include.
bool buildIntegrationStats(const ShotStatsView &stats, char *output,
                           size_t capacity, size_t window, bool durations) {
  char values[5][32], head[96];
  if (durations)
    snprintf(head, sizeof(head), "\"window\":%u,\"shotCount\":%lu,\"totalDurationS\":%.1f",
             static_cast<unsigned>(window), static_cast<unsigned long>(stats.shotCount),
             static_cast<double>(stats.durationDsSum) / 10.0);
  else
    snprintf(head, sizeof(head), "\"shotCount\":%lu", static_cast<unsigned long>(stats.shotCount));
  const int written = snprintf(
      output, capacity,
      "{%s,\"avgDurationS\":%s,"
      "\"avgYieldG\":%s,\"avgErrorPct\":%s,\"avgFlowGps\":%s,"
      "\"shotsPerDay\":%s}",
      head,
      stats.shotCount > 0
          ? formatStatFloat(values[0], static_cast<double>(stats.durationDsSum) /
                            (10.0 * stats.shotCount), 1)
          : "null",
      stats.shotCount > 0
          ? formatStatFloat(values[1], static_cast<double>(stats.actualCgSum) /
                            (100.0 * stats.shotCount), 1)
          : "null",
      stats.bbwCount > 0
          ? formatStatFloat(values[2], static_cast<double>(stats.errorPctTenthsSum) /
                            stats.bbwCount / 10.0, 1)
          : "null",
      stats.flowCount > 0
          ? formatStatFloat(values[3], static_cast<double>(stats.flowCgSx100Sum) /
                                stats.flowCount / 10000.0,
                            2)
          : "null",
      stats.timedCount > 0
          ? formatStatFloat(values[4],
                static_cast<double>(stats.timedCount) /
                    (static_cast<double>(stats.daysSpan) + 1.0),
                1)
          : "null");
  if (written <= 0 || static_cast<size_t>(written) >= capacity) return false;
  if (!durations) return true;
  size_t used = static_cast<size_t>(written) - 1U;
  int added = snprintf(output + used, capacity - used, ",\"durationsS\":[");
  if (added <= 0 || static_cast<size_t>(added) >= capacity - used) return false;
  used += static_cast<size_t>(added);
  for (uint32_t i = 0; i < stats.shotCount && i < SHOT_LOG_STATS_WINDOW; ++i) {
    added = snprintf(output + used, capacity - used, "%s%.1f",
                     i == 0 ? "" : ",", stats.durationsDs[i] / 10.0);
    if (added <= 0 || static_cast<size_t>(added) >= capacity - used) return false;
    used += static_cast<size_t>(added);
  }
  return capacity - used > 2 &&
         snprintf(output + used, capacity - used, "]}") == 2;
}
