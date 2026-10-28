// Included inside the facade's namespace after the WebSocket session code.
// Companion route (docs/COMPANION_LINK.md): the link calls the companion*
// entry points from the HTTP or network task; only the cloud task runs
// serviceCompanion() and companionCommand(). With no link serviceCompanion()
// returns at once, so every direct path runs exactly as without a companion.
namespace {
constexpr uint32_t kCompanionAliveMs = 30000;  // Ping missing or `up` false.
constexpr uint32_t kCompanionPendingMs = 30000;  // Full frame after `route`.
constexpr uint32_t kCompanionCommandMs = 10000;  // `res` after `cmd`.
constexpr uint32_t kCompanionFlapWindowMs = 600000;
constexpr uint32_t kCompanionFlapHoldMs = 300000;
constexpr uint32_t kCompanionLogPeriodMs = 10000;
}  // namespace

void OpenBrewByWeightMicraService::logCompanionRoute(MicraCompanionRoute from,
                                                     MicraCompanionRoute to,
                                                     const char *reason) {
  static constexpr const char *names[] = {"direct", "pending", "companion"};
  serialTraceCategoryf(to == MicraCompanionRoute::DIRECT ? LogLevel::WARNING : LogLevel::INFO,
                       DebugCategory::NETWORK, "Micra route %s -> %s reason=%s",
                       names[static_cast<uint8_t>(from)], names[static_cast<uint8_t>(to)],
                       reason);
}

bool OpenBrewByWeightMicraService::serviceCompanion() {
  if (!companionLink_.load(std::memory_order_acquire)) return false;
  const uint32_t now = millis();
  const bool hold = companionHold();
  // A quiet point: no shot, scale connection, maintenance or command, and a
  // usable network; cleaning and queued work are checked under the lock.
  LineaMicraError gate = LineaMicraError::NONE;
  const bool quiet = !hold && networkEligible(gate, true) && !shotActive_.load() &&
      !outboundShotActive.load() && !temperatureActive_.load() && !powerActive_.load();
  MicraCompanionRoute from, to;
  const char *reason;
  bool changed = false, release = false;
  {
    TaskLockGuard lock(mux_);
    auto &feed = companion_;
    from = feed.status.route;
    // The companion forwards nothing while the route is direct, so `hold`
    // goes out only with a route request (below) and while it stands.
    if (feed.outbox.hold != hold) {
      feed.outbox.hold = hold;
      if (from != MicraCompanionRoute::DIRECT) {
        ++feed.outbox.holdSequence;
        changed = true;
      }
    }
    if (feed.flapping && micra_timing::deadlineReached(now, feed.flapUntilMs))
      feed.flapping = false;
    if (from != MicraCompanionRoute::DIRECT) {
      // A basis is the full frame's subscription; without one the route
      // waits for a full frame (after `route` or an upstream reset).
      const bool basis = from == MicraCompanionRoute::COMPANION && websocketStatus_.subscribed;
      const char *lost = now - feed.pingAtMs >= kCompanionAliveMs ? "ping_missing"
          : !feed.up && now - feed.downSinceMs >= kCompanionAliveMs ? "upstream_down"
          : !basis && now - feed.requestedAtMs >= kCompanionPendingMs ? "full_missing"
          : nullptr;
      if (lost) leaveCompanionLocked(lost, true);
      else if (outboundMaintenance.load()) leaveCompanionLocked("maintenance", false);
      // Wi-Fi loss or AP mode ended the epoch the basis was stamped with.
      else if (basis && feed.stamp.epoch != observationFence_.epoch)
        leaveCompanionLocked("network", false);
    } else {
      const bool observing = config_.accountConfigured &&
          (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0;
      feed.status.reason = !feed.lmSwitch ? "switch_off"
          : !observing ? "not_observing"
          : !feed.status.credentialsConfirmed ? "credentials"
          : !feed.status.upstreamSubscribed || !feed.up ? "upstream"
          : feed.flapping ? "flapping"
          : !quiet || backflush_.status().busy() || desiredTemperature_.present ||
                  desiredPower_.present || pending_.present || active_ ? "busy"
          : "none";
      if (strcmp(feed.status.reason, "none") == 0) {
        feed.status.route = MicraCompanionRoute::PENDING;
        feed.outbox.routeCompanion = true;
        ++feed.outbox.routeSequence;
        ++feed.outbox.holdSequence;
        feed.requestedAtMs = feed.pingAtMs = now;
        feed.routeIntent = powerState_.generation();
      }
    }
    to = feed.status.route;
    reason = feed.status.reason;
    changed = changed || from != to;
    release = to == MicraCompanionRoute::COMPANION && feed.release;
    feed.release = false;
  }
  if (from != to) logCompanionRoute(from, to, reason);
  if (changed) notifyCompanion();
  if (to == MicraCompanionRoute::DIRECT) return false;
  // While the request is pending the owner leaves the direct socket as it is
  // (it keeps streaming on its own task), so the full frame can retire it
  // from the link task without racing a reconnection. Make before break:
  // once the full frame moved the source, the obsolete socket (WebSocket
  // mode) and the HTTPS session (both modes) close once; the client stays in
  // flash for the next failover, and a command re-allocates only its buffer.
  if (release) {
    stopWebSocket(true);
    releaseWorkBuffer();
    TaskLockGuard lock(mux_);
    websocketStatus_.retryAtMs = 0;
  }
  return true;
}

// Waits for the companion's `res`; a loss, timeout or cancellation reads as a
// transport failure, so retries and confirmation behave as for HTTPS.
bool OpenBrewByWeightMicraService::companionCommand(MicraCompanionCommandOp op,
                                                    uint16_t value) {
  static constexpr const char *names[] = {"none", "temp", "standby", "on", "refresh"};
  static uint32_t lastLogAtMs = 0;  // Cloud task only.
  if (work_ == nullptr) return false;
  work_->transportStatus = ESP_FAIL;
  work_->httpStatus = 0;
  work_->transportFailure = true;
  uint32_t id = 0;
  {
    TaskLockGuard lock(mux_);
    auto &feed = companion_;
    if (feed.status.route == MicraCompanionRoute::COMPANION) {
      if (++feed.lastCommand == 0) ++feed.lastCommand;
      id = feed.lastCommand;
      feed.outbox.commandId = id;
      feed.outbox.commandOp = op;
      feed.outbox.commandValue = value;
      feed.resultId = 0;
      ++feed.status.commands;
      recordCompanionIntentLocked(id);
    }
  }
  if (id == 0) return false;
  notifyCompanion();
  const uint32_t deadline = millis() + kCompanionCommandMs;
  bool done = false, accepted = false, routed = true;
  uint16_t http = 0;
  for (;;) {
    uint32_t remaining;
    {
      TaskLockGuard lock(mux_);
      auto &feed = companion_;
      const uint32_t now = millis();
      done = feed.resultId == id;
      accepted = done && feed.resultAccepted;
      http = done ? feed.resultHttp : 0;
      routed = feed.status.route == MicraCompanionRoute::COMPANION;
      if (done || !routed || abortRequested_.load(std::memory_order_acquire) ||
          micra_timing::deadlineReached(now, deadline)) {
        feed.outbox.commandId = 0;  // A later link session never replays it.
        break;
      }
      remaining = deadline - now;
    }
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(remaining));
  }
  if (done && (accepted || http != 0)) {
    work_->transportStatus = ESP_OK;
    work_->transportFailure = false;
    work_->httpStatus = accepted ? 200 : http;
  }
  if (!accepted && millis() - lastLogAtMs >= kCompanionLogPeriodMs) {
    lastLogAtMs = millis();
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "Micra companion command %s %s http=%u",
                         names[static_cast<uint8_t>(op)],
                         done ? "failed" : !routed ? "lost" : "unanswered",
                         static_cast<unsigned>(http));
  }
  return accepted;
}

void OpenBrewByWeightMicraService::leaveCompanionLocked(const char *reason, bool loss) {
  auto &feed = companion_;
  const uint32_t now = millis();
  feed.status.reason = reason;
  feed.release = false;
  if (feed.status.route == MicraCompanionRoute::DIRECT) return;
  if (feed.status.route == MicraCompanionRoute::COMPANION) {
    // The direct socket's stop steps, applied to the feed; the direct client
    // then reconnects as soon as its usual admission rule allows.
    dropCompanionFeedLocked();
    websocketStatus_.retryAtMs = 0;
    observationSchedule_.dueNow(now);
    ++feed.status.switches;
  }
  feed.status.route = MicraCompanionRoute::DIRECT;
  feed.status.routeSinceMs = now;
  companionRoute_.store(false, std::memory_order_release);
  feed.outbox.routeCompanion = false;
  ++feed.outbox.routeSequence;
  if (!loss) return;
  feed.losses[feed.nextLoss] = now;
  feed.nextLoss = static_cast<uint8_t>((feed.nextLoss + 1) % 4);
  if (feed.lossCount < 4) ++feed.lossCount;
  // More than three losses in ten minutes keep the route direct for five.
  if (feed.lossCount == 4 && now - feed.losses[feed.nextLoss] < kCompanionFlapWindowMs) {
    feed.flapping = true;
    feed.flapUntilMs = now + kCompanionFlapHoldMs;
  }
}

void OpenBrewByWeightMicraService::dropCompanionFeedLocked() {
  powerState_.hold(published_, config_.accountConfigured &&
      (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0, millis());
  websocketStatus_.state = MicraSocketState::STOPPING;
  websocketStatus_.subscribed = false;
  shotTraceNoteMicraSubscribed(false);
  backflush_.invalidate();
  snapshotPending_ = false;
  observationFence_.invalidate();
  ++observationFence_.epoch;
  companion_.requestedAtMs = millis();  // A full frame is due from now on.
}

bool OpenBrewByWeightMicraService::companionFeedLiveLocked() const {
  return websocketStatus_.subscribed && companion_.status.upstreamSubscribed && companion_.up &&
      companion_.stamp.identity == identityGeneration_ &&
      companion_.stamp.epoch == observationFence_.epoch && companionLink_.load() &&
      staConnected_.load() && !apActive_.load() && !outboundMaintenance.load();
}

// Power intent of a companion read: the generation recorded for command `ac`
// (the oldest kept when it is unknown), never older than the route request,
// so an observation taken before a power command cannot undo it.
uint32_t OpenBrewByWeightMicraService::companionIntentLocked(uint32_t afterCommand) const {
  const uint32_t start = companion_.routeIntent;
  if (afterCommand == 0) return start;
  bool any = false;
  uint32_t generation = 0;
  for (const auto &intent : companion_.intents) {
    if (intent.command == afterCommand) {
      generation = intent.generation;
      any = true;
      break;
    }
    if (intent.command != 0 &&
        (!any || static_cast<int32_t>(intent.generation - generation) < 0)) {
      generation = intent.generation;
      any = true;
    }
  }
  return any && static_cast<int32_t>(generation - start) > 0 ? generation : start;
}

void OpenBrewByWeightMicraService::recordCompanionIntentLocked(uint32_t command) {
  auto &feed = companion_;
  for (auto &intent : feed.intents) {
    if (intent.command == command) {
      intent.generation = powerState_.generation();
      return;
    }
  }
  feed.intents[feed.nextIntent] = {command, powerState_.generation()};
  feed.nextIntent = static_cast<uint8_t>((feed.nextIntent + 1) % 8);
}

void OpenBrewByWeightMicraService::refreshCompanionConfigLocked() {
  auto &feed = companion_;
  auto &box = feed.outbox;
  const bool observe = config_.accountConfigured &&
      (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0;
  const bool websocket =
      config_.connectionType == static_cast<uint8_t>(MicraConnectionType::WEBSOCKET);
  if (box.lmSwitch != feed.lmSwitch || box.observe != observe ||
      box.websocketConnection != websocket) {
    box.lmSwitch = feed.lmSwitch;
    box.observe = observe;
    box.websocketConnection = websocket;
    ++box.configSequence;
  }
  // Disconnect, or the switch turned off, tells the companion to erase them.
  const bool erase = !config_.accountConfigured || !feed.lmSwitch;
  if (box.credentialsErase != erase) {
    box.credentialsErase = erase;
    ++box.credentialsGeneration;
    feed.status.credentialsConfirmed = false;
  }
}

void OpenBrewByWeightMicraService::companionLink(bool ready, bool lmSwitch) {
  MicraCompanionRoute from, to;
  const char *reason;
  {
    TaskLockGuard lock(mux_);
    auto &feed = companion_;
    from = feed.status.route;
    feed.lmSwitch = lmSwitch;
    if (!ready || !lmSwitch) leaveCompanionLocked(ready ? "switch_off" : "link_lost", !ready);
    if (!ready) feed.up = feed.status.upstreamSubscribed = feed.status.credentialsConfirmed = false;
    refreshCompanionConfigLocked();
    companionLink_.store(ready, std::memory_order_release);
    to = feed.status.route;
    reason = feed.status.reason;
  }
  if (from != to) logCompanionRoute(from, to, reason);
  notifyCompanion();
  if (task_ != nullptr) xTaskNotifyGive(task_);  // Ends a command wait at once.
}

void OpenBrewByWeightMicraService::companionPing(bool upstreamAlive) {
  TaskLockGuard lock(mux_);
  auto &feed = companion_;
  const uint32_t now = millis();
  feed.pingAtMs = now;
  if (upstreamAlive) feed.upAtMs = now;
  else if (feed.up) feed.downSinceMs = now;
  feed.up = upstreamAlive;
}

void OpenBrewByWeightMicraService::companionUpstream(bool subscribed, uint32_t epoch,
                                                     uint32_t tokenRenewInS) {
  bool reset = false;
  {
    TaskLockGuard lock(mux_);
    auto &feed = companion_;
    feed.status.upstreamSubscribed = subscribed;
    feed.status.upstreamEpoch = epoch;
    feed.tokenRenewAtMs = (millis() + std::min<uint32_t>(tokenRenewInS, 86400) * 1000U) | 1U;
    // A new upstream epoch or a lost subscription ends the basis of the last
    // full frame; the next full frame rebuilds it, so no gap stays hidden.
    reset = feed.status.route == MicraCompanionRoute::COMPANION &&
        websocketStatus_.subscribed && (!subscribed || epoch != feed.epoch);
    if (reset) dropCompanionFeedLocked();
  }
  if (reset)
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "Micra companion feed reset subscribed=%u epoch=%lu",
                         static_cast<unsigned>(subscribed), static_cast<unsigned long>(epoch));
}

void OpenBrewByWeightMicraService::companionCredentials(uint32_t generation, bool ok) {
  MicraCompanionRoute from, to;
  {
    TaskLockGuard lock(mux_);
    auto &feed = companion_;
    if (generation == 0 || generation != feed.outbox.credentialsGeneration ||
        feed.outbox.credentialsErase)
      return;
    from = feed.status.route;
    feed.status.credentialsConfirmed = ok;
    if (!ok) leaveCompanionLocked("credentials", false);
    to = feed.status.route;
  }
  if (!ok)
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "Micra companion: La Marzocco rejected the account");
  if (from != to) logCompanionRoute(from, to, "credentials");
  if (from != to) notifyCompanion();
}

MicraCompanionIngress OpenBrewByWeightMicraService::companionIngress() {
  MicraCompanionIngress ingress;
  // Sampled before any mutex, like the socket callback, so contention cannot
  // retag a push taken before a cleaning edge.
  ingress.attempt = backflush_.ingressAttempt();
  TaskLockGuard lock(mux_);
  ingress.intent = powerState_.generation();
  return ingress;
}

bool OpenBrewByWeightMicraService::companionState(MicraCompanionStateKind kind,
                                                  uint32_t epoch, uint32_t afterCommand,
                                                  const cJSON *dashboard,
                                                  MicraCompanionIngress ingress) {
  static std::atomic<uint32_t> lastRejectAtMs{0};
  const bool push = kind == MicraCompanionStateKind::PUSH;
  MicraObservation update;
  // Cleaning is decoded only from pushes, exactly as on the direct socket.
  update.source = push ? MicraObservationSource::WEBSOCKET
      : kind == MicraCompanionStateKind::FULL ? MicraObservationSource::HTTP_INITIAL
      : MicraObservationSource::HTTP;
  if (!decodeMicraDashboard(dashboard, update) ||
      (!push && !update.powerPresent && !(update.connectedPresent && !update.connected))) {
    const uint32_t now = millis();
    if (now - lastRejectAtMs.load() >= kCompanionLogPeriodMs) {
      lastRejectAtMs.store(now);
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                           "Micra companion state rejected kind=%s",
                           push ? "push" : kind == MicraCompanionStateKind::FULL ? "full" : "read");
    }
    // Like a failed direct read, a rejected full or read frame is ignored (a
    // missing full frame then times out as full_missing); a rejected push
    // fails the feed, as it fails the socket.
    return !push;
  }
  update.receivedAtMs = millis();
  const bool admitted = staConnected_.load() && !apActive_.load() &&
      !outboundMaintenance.load() && !clearSessionRequested_.load();
  // Like a direct read, a read landing while the companion is told to hold
  // is dropped; pushes keep arriving as they do on the socket.
  const bool readDropped = kind == MicraCompanionStateKind::READ && companionHold();
  const bool quiet = !shotActive_.load() && !outboundShotActive.load() &&
      !scaleConnecting_.load() && !outboundScaleInhibited() &&
      !temperatureActive_.load() && !powerActive_.load();
  MicraCompanionRoute from, to;
  const char *reason;
  {
    TaskLockGuard lock(mux_);
    auto &feed = companion_;
    from = feed.status.route;
    const bool usable = admitted && config_.accountConfigured &&
        (config_.options & LINEA_MICRA_OBSERVE_STATE) != 0;
    if (usable && kind == MicraCompanionStateKind::FULL &&
        from != MicraCompanionRoute::DIRECT) {
      if (from == MicraCompanionRoute::PENDING &&
          (!quiet || backflush_.status().busy() || desiredTemperature_.present ||
           desiredPower_.present)) {
        leaveCompanionLocked("busy", false);
      } else {
        if (from == MicraCompanionRoute::PENDING) {
          // Make before break: ending the socket's epoch here drops its later
          // frames, so exactly one source feeds the fence from now on.
          feed.status.route = MicraCompanionRoute::COMPANION;
          feed.status.routeSinceMs = update.receivedAtMs;
          ++feed.status.switches;
          companionRoute_.store(true, std::memory_order_release);
          ++observationFence_.epoch;
          feed.release = true;
        }
        // A new subscription (as sendSubscription()) plus its initial read.
        backflush_.invalidate();
        observationFence_.invalidate();
        observationFence_.offline = false;
        websocketStatus_.pongAtMs = 0;
        websocketStatus_.subscribed = true;
        shotTraceNoteMicraSubscribed(true);
        websocketStatus_.machineConnectedKnown = false;
        websocketStatus_.state = MicraSocketState::WAITING_FOR_DATA;
        snapshotPending_ = false;
        feed.epoch = epoch;
        feed.stamp = observationFence_.stamp(identityGeneration_, powerState_.generation());
        update.stamp = feed.stamp;
        update.stamp.intent = companionIntentLocked(afterCommand);
        (void)mergeReadLocked(update);
      }
    } else if (usable && !readDropped && from == MicraCompanionRoute::COMPANION &&
               kind != MicraCompanionStateKind::FULL && epoch == feed.epoch &&
               websocketStatus_.subscribed && feed.stamp.epoch == observationFence_.epoch &&
               feed.stamp.identity == identityGeneration_) {
      if (push) {
        update.stamp = feed.stamp;
        update.stamp.intent = ingress.intent;  // At ingress, as on the socket.
        applyPushLocked(update, ingress.attempt);
      } else {
        update.stamp = observationFence_.stamp(identityGeneration_,
                                               companionIntentLocked(afterCommand));
        (void)mergeReadLocked(update);
      }
    }
    to = feed.status.route;
    reason = feed.status.reason;
  }
  if (from != to) {
    logCompanionRoute(from, to, reason);
    notifyCompanion();
  }
  return true;
}

void OpenBrewByWeightMicraService::companionCommandResult(uint32_t id, bool accepted,
                                                          uint16_t httpStatus) {
  {
    TaskLockGuard lock(mux_);
    auto &feed = companion_;
    if (id == 0 || id != feed.outbox.commandId) return;
    feed.resultId = id;
    feed.resultAccepted = accepted;
    feed.resultHttp = httpStatus;
  }
  if (task_ != nullptr) xTaskNotifyGive(task_);
}

MicraCompanionOutbox OpenBrewByWeightMicraService::companionOutbox() const {
  TaskLockGuard lock(mux_);
  return companion_.outbox;
}

MicraCompanionRouteStatus OpenBrewByWeightMicraService::companionRouteStatus() const {
  TaskLockGuard lock(mux_);
  MicraCompanionRouteStatus result = companion_.status;
  if (!companionLink_.load()) result.reason = "no_link";
  return result;
}

bool OpenBrewByWeightMicraService::companionCopyCredentials(
    uint32_t generation, char *email, size_t emailCapacity, char *password,
    size_t passwordCapacity, char *serial, size_t serialCapacity) {
  TaskLockGuard lock(mux_);
  const bool ok = generation != 0 && generation == companion_.outbox.credentialsGeneration &&
      !companion_.outbox.credentialsErase && config_.accountConfigured &&
      secureCopy(email, emailCapacity, config_.username) &&
      secureCopy(password, passwordCapacity, config_.password) &&
      secureCopy(serial, serialCapacity, config_.selectedSerial);
  if (!ok) {
    if (email && emailCapacity) secureWipe(email, emailCapacity);
    if (password && passwordCapacity) secureWipe(password, passwordCapacity);
    if (serial && serialCapacity) secureWipe(serial, serialCapacity);
  }
  return ok;
}
