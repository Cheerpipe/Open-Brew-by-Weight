// Companion status reporting (docs/COMPANION_LINK.md): the Admin status
// JSON, the Diagnostic stream's snapshot and the Home pairing hint — the
// read-only views of the pairing, the link and its feature switches.

namespace openbrewbyweight {

bool OpenBrewByWeightNetwork::appendCompanionStatus(size_t *used) {
  const MicraCompanionRouteStatus route = machineIntegrationCompanionRouteStatus();
  const uint32_t now = millis();
  TaskLockGuard lock(dataMux_);
  companionPairTick(g_companion.pairing, now);
  const CompanionStored &stored = g_companion.stored;
  const CompanionLinkSlot *link = companionActiveLink();
  bool ok = statusJsonAppend(
      used,
      ",\"companion\":{\"paired\":%s,\"name\":\"%s\",\"address\":\"%s\",\"version\":\"%s\","
      "\"link\":\"%s\",\"linkAgeS\":%ld,\"peerProtocol\":%lu,\"ui\":%s,\"log\":%s,"
      "\"relay\":\"%s\",\"relayRejects\":%lu,",
      stored.paired ? "true" : "false", stored.name, stored.address, stored.companionVersion,
      link != nullptr ? "open" : "down", static_cast<long>(companionLinkAgeS(g_companion, now)),
      static_cast<unsigned long>(link != nullptr ? 0 : g_companion.peerProtocol),
      stored.uiOff ? "false" : "true", stored.logSwitch ? "true" : "false",
      g_companion.relay.ready ? "ready" : "down",
      static_cast<unsigned long>(g_companion.relay.rejects));
  if (kCompanionLm)
    ok = ok && statusJsonAppend(
        used, "\"route\":\"%s\",\"lm\":%s,\"upstream\":%s,\"refused\":%s,\"credentials\":%s,",
        companionRouteName(route.route), stored.lmSwitch ? "true" : "false",
        route.upstreamSubscribed ? "true" : "false",
        route.upstreamRefused ? "true" : "false", route.credentialsConfirmed ? "true" : "false");
  ok = ok && statusJsonAppend(used, "\"request\":");
  const CompanionRequest &request = g_companion.pairing.request;
  if (request.state == CompanionPairState::PENDING)
    ok = ok && statusJsonAppend(
        used, "{\"name\":\"%s\",\"address\":\"%s\",\"version\":\"%s\",\"expiresInS\":%lu,"
              "\"codeReady\":%s}",
        request.name, request.address, request.version,
        static_cast<unsigned long>((request.untilMs - now + 999U) / 1000U),
        companionCodeReady(request) ? "true" : "false");
  else
    ok = ok && statusJsonAppend(used, "null");
  return ok && statusJsonAppend(used, ",\"ignored\":%u}",
                                static_cast<unsigned>(companionIgnoredCount(g_companion.pairing)));
}

// Diagnostic → Companion: the pairing, its link and the La Marzocco route.
void OpenBrewByWeightNetwork::companionSnapshot(CompanionSnapshot &out) {
  const MicraCompanionRouteStatus route = machineIntegrationCompanionRouteStatus();
  out.route = companionRouteName(route.route);
  out.reason = route.reason != nullptr ? route.reason : "none";
  out.upstream = route.upstreamSubscribed;
  out.routeSwitches = route.switches;
  out.commands = route.commands;
  const uint32_t now = millis();
  TaskLockGuard lock(dataMux_);
  const CompanionStored &stored = g_companion.stored;
  const CompanionLinkSlot *link = companionActiveLink();
  out.paired = stored.paired != 0;
  out.lmSwitch = stored.lmSwitch != 0;
  out.ui = stored.uiOff == 0;
  out.logSwitch = stored.logSwitch != 0;
  out.relayReady = g_companion.relay.ready;
  out.relayRejects = g_companion.relay.rejects;
  out.linkOpen = link != nullptr;
  out.protocol = link != nullptr ? link->protocol : 0;
  out.linkAgeS = companionLinkAgeS(g_companion, now);
  out.peerProtocol = link != nullptr ? 0 : g_companion.peerProtocol;
  if (out.peerProtocol != 0) out.reason = "protocol";
  out.reconnects = g_companion.ups > 1 ? g_companion.ups - 1 : 0;
  out.authFailures = g_companion.authFailures;
  out.logCursor = link != nullptr ? link->logSent : 0;
  out.logGaps = g_companion.logGaps;
  out.logFlushS = g_companion.logFlushAtMs == 0
                      ? -1
                      : static_cast<int32_t>((now - g_companion.logFlushAtMs) / 1000U);
  memcpy(out.name, stored.name, sizeof(out.name));
  memcpy(out.address, stored.address, sizeof(out.address));
  memcpy(out.version, stored.companionVersion, sizeof(out.version));
}

// Home's hint while a request waits (empty strings otherwise).
void OpenBrewByWeightNetwork::companionRequestLabels(char (&name)[COMPANION_NAME_TEXT],
                                                     char (&address)[COMPANION_ADDRESS_TEXT]) {
  const uint32_t now = millis();
  TaskLockGuard lock(dataMux_);
  companionPairTick(g_companion.pairing, now);
  const CompanionRequest &request = g_companion.pairing.request;
  memcpy(name, request.name, sizeof(name));
  memcpy(address, request.address, sizeof(address));
  if (request.state != CompanionPairState::PENDING) name[0] = address[0] = '\0';
}

// Admin → Companion. Names, versions and addresses are sanitized on store.
}  // namespace openbrewbyweight
