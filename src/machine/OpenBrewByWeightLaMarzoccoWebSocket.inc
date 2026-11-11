// Included inside the facade's namespace after its session buffer definition.
// The IDF client cannot switch pings off (0 means its default), so a parked
// subscription pings once an hour: never within a shot.
constexpr size_t kPingSec = 15, kParkedPingSec = 3600;
struct OpenBrewByWeightLaMarzoccoService::WebSocketBuffer {
  LaMarzoccoStompDecoder decoder;
  LaMarzoccoObservationStamp stamp;
  esp_websocket_client_handle_t client = nullptr;
  char headers[1024] = {};
  char connect[kTokenCapacity + 160] = {};
  char destination[128] = {};
  char subscription[40] = {};
  struct RateBin { uint64_t second = 0; uint32_t rx = 0, tx = 0; } rates[60];
  HeapLifecycleTracker heap;
  HeapLifecycleTracker stopHeap;
  std::atomic<bool> failed{false};
  std::atomic<bool> authFailed{false};
  std::atomic<uint32_t> partialAtMs{0};
  std::atomic<bool> partialPending{false};
  std::atomic<bool> parked{false};  // UNSUBSCRIBEd for a scaled shot, socket kept
  uint32_t setupAtMs = 0, renewRetryAtMs = 0;
  uint32_t frameIntent = 0;
  int payloadOffset = 0;
  int payloadLength = 0;
  bool continuation = false;
  uint8_t attempts = 0;
};

LaMarzoccoWebSocketStatus OpenBrewByWeightLaMarzoccoService::websocketStatus(bool includeTraffic) const {
  TaskLockGuard lock(mux_);
  LaMarzoccoWebSocketStatus result = websocketStatus_;
  result.live = liveSubscriptionLocked();
  if (includeTraffic && websocket_ != nullptr) {
    const uint64_t second = static_cast<uint64_t>(esp_timer_get_time()) / 1000000;
    for (const auto &bin : websocket_->rates) {
      if (second - bin.second < 60) {
        result.receivePerMinute += bin.rx;
        result.transmitPerMinute += bin.tx;
      }
      if (second == bin.second) {
        result.receivePerSecond = bin.rx;
        result.transmitPerSecond = bin.tx;
      }
    }
    result.heap = websocket_->heap.aggregate;
  }
  return result;
}

bool OpenBrewByWeightLaMarzoccoService::websocketAdmitted(bool starting) const {
  if (starting && (outboundScaleInhibited() || scaleConnecting_.load(std::memory_order_acquire) ||
                   shotTransportPaused())) return false;
  if (outboundMaintenance.load(std::memory_order_acquire)) return false;
  if (!staConnected_.load(std::memory_order_acquire) ||
      apActive_.load(std::memory_order_acquire) ||
      clearSessionRequested_.load(std::memory_order_acquire)) return false;
  TaskLockGuard lock(mux_);
  return config_.accountConfigured &&
      (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0 &&
      config_.connectionType == static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET);
}

void OpenBrewByWeightLaMarzoccoService::stopWebSocket(bool release) {
  WebSocketBuffer *buffer = websocket_;
  if (!buffer) {
    return;
  }
  if (buffer->client) {
    const auto before = sampleHeapCaps();
    recordBootHeap(BootHeapStage::WS_DONE, BootHeapResult::FAILED, millis(), before);
    {
      TaskLockGuard lock(mux_);
      if (buffer->stamp.identity == identityGeneration_ && config_.accountConfigured &&
          (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0 &&
          config_.connectionType == static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET))
        websocketUnexpectedReconnect_ = websocketUnexpectedReconnect_ || buffer->failed.load() ||
            (!staConnected_.load() && !apActive_.load());
      // On the companion route this socket is already obsolete; the feed
      // that replaced it keeps its own subscription and supervision.
      if (companion_.status.route != LaMarzoccoCompanionRoute::COMPANION) {
        if (buffer->stamp.identity == identityGeneration_)
          powerState_.hold(published_, config_.accountConfigured &&
              (config_.options & LA_MARZOCCO_OBSERVE_STATE) != 0, millis());
        websocketStatus_.state = LaMarzoccoSocketState::STOPPING;
        websocketStatus_.subscribed = false;
        shotTraceNoteLaMarzoccoSubscribed(false);
        backflush_.invalidate();
      }
      if (buffer->heap.pending) finishHeapLifecycle(buffer->heap, HeapLifecycleResult::FAILURE, before);
      beginHeapLifecycle(buffer->stopHeap, HeapLifecycleEvent::HTTP_STOP, before);
    }
    // stop joins actual socket shutdown; never called from an event or mux.
    (void)esp_websocket_client_stop(buffer->client);
    (void)esp_websocket_unregister_events(buffer->client, WEBSOCKET_EVENT_ANY,
                                         websocketEvent);
    (void)esp_websocket_client_destroy(buffer->client);
    buffer->client = nullptr;
    const auto after = sampleHeapCaps();
    TaskLockGuard lock(mux_);
    if (companion_.status.route != LaMarzoccoCompanionRoute::COMPANION) {
      snapshotPending_ = false;
      observationFence_.invalidate();
    }
    finishHeapLifecycle(buffer->stopHeap, HeapLifecycleResult::SUCCESS, after);
    websocketStatus_.heap = buffer->heap.aggregate;
    websocketStatus_.stopHeap = buffer->stopHeap.aggregate;
    websocketStatus_.internalMinimum = after.internalMinimum;
    websocketStatus_.psramMinimum = after.psramMinimum;
    websocketStatus_.psramFree = after.psramFree;
    websocketStatus_.psramLargest = after.psramLargest;
    if (websocketStatus_.pauseAtMs) {
      websocketStatus_.stoppedLatencyMs = millis() - websocketStatus_.pauseAtMs;
      websocketStatus_.maxStoppedLatencyMs = std::max(
          websocketStatus_.maxStoppedLatencyMs, websocketStatus_.stoppedLatencyMs);
    }
  }
  buffer->decoder.reset();
  buffer->partialAtMs.store(0, std::memory_order_release);
  buffer->partialPending.store(false, std::memory_order_release);
  buffer->continuation = false;
  secureWipe(buffer->connect, sizeof(buffer->connect));
  secureWipe(buffer->headers, sizeof(buffer->headers));
  if (release) {
    // Quiescence precedes pointer removal: diagnostic readers share mux_.
    {
      TaskLockGuard lock(mux_);
      websocket_ = nullptr;
      websocketStatus_.retainedBytes = 0;
      websocketStatus_.workspaceExternal = false;
    }
    buffer->~WebSocketBuffer();
    heapCapsFree(buffer);
  }
}

// One SUBSCRIBE per connection: the broker pushes only changes, so the first
// queues one initial dashboard read. A scaled shot parks it (`on` false: an
// UNSUBSCRIBE, socket kept) until the relay opens; that resumed SUBSCRIBE
// reads nothing, since the cloud sends the machine's state seconds after a shot.
bool OpenBrewByWeightLaMarzoccoService::sendSubscription(bool on) {
  WebSocketBuffer &buffer = *websocket_;
  char frame[256];
  const int length = on ? snprintf(frame, sizeof(frame),
      "SUBSCRIBE\ndestination:%s\nack:auto\nid:%s\ncontent-length:0\n\n",
      buffer.destination, buffer.subscription)
      : snprintf(frame, sizeof(frame), "UNSUBSCRIBE\nid:%s\n\n", buffer.subscription);
  if (length < 0 || static_cast<size_t>(length + 1) >= sizeof(frame)) return false;
  const int sent = esp_websocket_client_send_text(buffer.client, frame, length + 1,
                                                  pdMS_TO_TICKS(1000));
  const bool admitted = websocketAdmitted();
  TaskLockGuard lock(mux_);
  auto &status = websocketStatus_;
  status.transmitBytes += std::max(0, sent);
  const uint64_t second = static_cast<uint64_t>(esp_timer_get_time()) / 1000000;
  auto &bin = buffer.rates[second % 60];
  if (bin.second != second) bin = {second, 0, 0};
  bin.tx += std::max(0, sent);
  if (sent != length + 1) return false;
  if (status.subscribed) {  // Park or resume: liveness counts from now.
    buffer.parked.store(!on, std::memory_order_release);
    status.pongAtMs = millis();
    return true;
  }
  if (!admitted || buffer.stamp.epoch != observationFence_.epoch ||
      buffer.stamp.identity != identityGeneration_) return true;
  backflush_.invalidate();
  observationFence_.invalidate();
  status.pongAtMs = 0;
  status.subscribed = true;
  shotTraceNoteLaMarzoccoSubscribed(true);
  status.machineConnectedKnown = false;
  observationFence_.offline = false;
  snapshotStamp_ = observationFence_.stamp(identityGeneration_, powerState_.generation());
  snapshotPending_ = true;
  status.state = LaMarzoccoSocketState::WAITING_FOR_DATA;
  return true;
}

bool OpenBrewByWeightLaMarzoccoService::stompFrame(void *context, const LaMarzoccoStompFrame &frame) {
  auto &self = *static_cast<OpenBrewByWeightLaMarzoccoService *>(context);
  WebSocketBuffer &buffer = *self.websocket_;
  if (!self.websocketAdmitted()) return true;
  bool subscribed;
  {
    TaskLockGuard lock(self.mux_);
    subscribed = self.websocketStatus_.subscribed;
  }
  if (strcmp(frame.command, "CONNECTED") == 0) {
    if ((strcmp(frame.version, "1.2") != 0 && strcmp(frame.version, "1.1") != 0) ||
        subscribed) return false;
    // CONNECT advertises 0,0: no STOMP heartbeat is negotiated. WS ping remains.
    if (!self.sendSubscription()) return false;
    TaskLockGuard lock(self.mux_);
    if (!self.websocketStatus_.subscribed) return true;
    ++(self.websocketUnexpectedReconnect_ ? self.websocketStatus_.unexpectedConnections
                                         : self.websocketStatus_.plannedConnections);
    self.websocketUnexpectedReconnect_ = false;
    return true;
  }
  if (strcmp(frame.command, "ERROR") == 0) {
    // Protocol ERROR is a transport failure. Only HTTP 401/403 proves auth failure.
    return false;
  }
  if (strcmp(frame.command, "MESSAGE") != 0) return false;
  if (strcmp(frame.destination, buffer.destination) != 0 ||
      strcmp(frame.subscription, buffer.subscription) != 0) return false;
  // A push already in flight when a scaled shot parked the subscription.
  if (buffer.parked.load(std::memory_order_acquire)) return true;
  if (!subscribed) return false;
  // JSON cannot contain an embedded raw NUL, even with STOMP content-length.
  if (memchr(frame.body, '\0', frame.bodyLength)) return false;
  const uint32_t receivedAt = millis();
  LaMarzoccoObservation update;
  update.source = LaMarzoccoObservationSource::WEBSOCKET;
  update.receivedAtMs = receivedAt;
  {
    TaskLockGuard lock(self.mux_);
    update.stamp = buffer.stamp;
    // Intent is captured at ingress, before parsing, not at handshake time.
    update.stamp.intent = buffer.frameIntent;
  }
  cJSON *root = parseJsonDocumentWithinLimits(frame.body, LaMarzoccoStompDecoder::kBodyLimit,
                                             1024);
  const bool decoded = root && decodeLaMarzoccoDashboard(root, update);
  if (root) cJSON_Delete(root);
  if (!decoded) {
    // Names the offending push shape on the serial log instead of failing
    // the subscription silently. The body is length-delimited, not
    // NUL-terminated: copy a bounded excerpt before printing.
    char excerpt[97];
    const int excerptLength = frame.bodyLength < 96 ? frame.bodyLength : 96;
    memcpy(excerpt, frame.body, excerptLength);
    excerpt[excerptLength] = '\0';
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "La Marzocco dashboard push rejected: %s", excerpt);
    return false;
  }
  if (!self.websocketAdmitted()) return true;
  TaskLockGuard lock(self.mux_);
  if (buffer.stamp.epoch != self.observationFence_.epoch ||
      buffer.stamp.identity != self.identityGeneration_) return true;
  self.applyPushLocked(update, frame.ingressAttempt);
  return true;
}

// Publishes one decoded dashboard push under mux_. The direct socket and the
// companion feed share this body, so both routes publish identically.
void OpenBrewByWeightLaMarzoccoService::applyPushLocked(const LaMarzoccoObservation &update,
                                                   uint32_t ingressAttempt) {
  const uint32_t receivedAt = update.receivedAtMs;
  if (!observationFence_.merge(published_, powerState_, update, true)) return;
  mirrorPowerLocked();
  // The broker's online push carries the full dashboard, so going back online
  // needs no HTTPS read; an offline machine makes a pending one useless.
  if (observationFence_.offline) snapshotPending_ = false;
  auto &status = websocketStatus_;
  status.state = LaMarzoccoSocketState::STREAMING;
  status.reason = "none";
  status.messageAtMs = receivedAt;
  ++status.messages;
  if (update.powerPresent && !observationFence_.offline) status.powerAtMs = receivedAt;
  if (update.connectedPresent) {
    status.machineConnectedKnown = true;
    status.machineConnected = update.connected;
  }
  if (observationFence_.offline ||
      (update.powerPresent && update.mode != LaMarzoccoObservedMode::BREWING))
    backflush_.invalidate();
  if (update.cleaningPresent) {
    status.cleaning = update.cleaning;
    status.cleaningAvailable = update.cleaningAvailable;
    status.cleaningAtMs = receivedAt;
    memcpy(status.cleaningLabel, update.cleaningLabel, sizeof(status.cleaningLabel));
    const auto phase = update.cleaning == LaMarzoccoCleaningState::OFF
        ? MachineBackflushPhase::INACTIVE : update.cleaning == LaMarzoccoCleaningState::REQUESTED
        ? MachineBackflushPhase::AWAITING : update.cleaning == LaMarzoccoCleaningState::CLEANING
        ? MachineBackflushPhase::ACTIVE : MachineBackflushPhase::UNKNOWN;
    // A recognized status restarts the unsupervised clock even between polls.
    if (!observationFence_.offline && status.machineConnectedKnown &&
        status.machineConnected) {
      backflush_.observe(phase, ingressAttempt);
      if (phase != MachineBackflushPhase::UNKNOWN) backflushUnsupervisedAtMs_ = 0;
    } else if (phase != MachineBackflushPhase::INACTIVE)
      backflush_.observe(MachineBackflushPhase::UNKNOWN, ingressAttempt);
    if (backflush_.status().busy()) {
      desiredPower_ = {};
      if (temperatureActive_.load() || powerActive_.load())
        abortRequested_.store(true, std::memory_order_release);
    }
  }
  status.publishLatencyMs = millis() - receivedAt;
  status.maxPublishLatencyMs = std::max(status.maxPublishLatencyMs, status.publishLatencyMs);
}

void OpenBrewByWeightLaMarzoccoService::websocketEvent(void *context, const char *,
                                            int32_t event, void *data) {
  auto &self = *static_cast<OpenBrewByWeightLaMarzoccoService *>(context);
  const uint32_t ingressAttempt = self.backflush_.ingressAttempt();
  auto &buffer = *self.websocket_;
  if (!data || (event != WEBSOCKET_EVENT_CONNECTED && event != WEBSOCKET_EVENT_DATA &&
                event != WEBSOCKET_EVENT_DISCONNECTED && event != WEBSOCKET_EVENT_CLOSED &&
                event != WEBSOCKET_EVENT_ERROR)) return;
  auto &message = *static_cast<esp_websocket_event_data_t *>(data);
  if (event == WEBSOCKET_EVENT_CONNECTED || event == WEBSOCKET_EVENT_DATA) {
    TaskLockGuard lock(self.mux_);
    if (buffer.stamp.epoch != self.observationFence_.epoch ||
        buffer.stamp.identity != self.identityGeneration_) return;
  }
  if (event == WEBSOCKET_EVENT_DISCONNECTED || event == WEBSOCKET_EVENT_CLOSED ||
      event == WEBSOCKET_EVENT_ERROR) {
    if (event == WEBSOCKET_EVENT_DISCONNECTED)
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
          "La Marzocco WS disconnected http=%d tls=%d stack=%d verify=%u errno=%d",
          message.error_handle.esp_ws_handshake_status_code,
          int(message.error_handle.esp_tls_last_esp_err), message.error_handle.esp_tls_stack_err,
          unsigned(message.error_handle.esp_tls_cert_verify_flags), message.error_handle.esp_transport_sock_errno);
    if (event == WEBSOCKET_EVENT_ERROR &&
        (message.error_handle.esp_ws_handshake_status_code == 401 ||
         message.error_handle.esp_ws_handshake_status_code == 403)) {
      buffer.authFailed.store(true, std::memory_order_release);
    }
    buffer.failed.store(true, std::memory_order_release);
  } else if (event == WEBSOCKET_EVENT_CONNECTED && self.websocketAdmitted()) {
    const size_t length = strlen(buffer.connect) + 1;
    const int sent = esp_websocket_client_send_text(message.client, buffer.connect, length,
                                                   pdMS_TO_TICKS(1000));
    const bool admitted = self.websocketAdmitted();
    {
      TaskLockGuard lock(self.mux_);
      self.websocketStatus_.transmitBytes += std::max(0, sent);
      const uint64_t second = static_cast<uint64_t>(esp_timer_get_time()) / 1000000;
      auto &bin = buffer.rates[second % 60];
      if (bin.second != second) bin = {second, 0, 0};
      bin.tx += std::max(0, sent);
    }
    if (sent != static_cast<int>(length)) {
      buffer.failed.store(true, std::memory_order_release);
    } else if (admitted) {
      const auto heap = sampleHeapCaps();
      recordBootHeap(BootHeapStage::WS_DONE, BootHeapResult::OK, millis(), heap);
      TaskLockGuard lock(self.mux_);
      if (buffer.stamp.epoch == self.observationFence_.epoch &&
          buffer.stamp.identity == self.identityGeneration_) {
        finishHeapLifecycle(buffer.heap, HeapLifecycleResult::SUCCESS, heap);
        self.websocketStatus_.internalMinimum = heap.internalMinimum;
        self.websocketStatus_.psramMinimum = heap.psramMinimum;
        self.websocketStatus_.psramFree = heap.psramFree;
        self.websocketStatus_.psramLargest = heap.psramLargest;
        self.websocketStatus_.state = LaMarzoccoSocketState::STOMP_CONNECTING;
        self.websocketStatus_.connectedAtMs = millis();
      }
    }
    secureWipe(buffer.connect, sizeof(buffer.connect));
  } else if (event == WEBSOCKET_EVENT_DATA) {
    if (message.op_code == 0xA) {
      TaskLockGuard lock(self.mux_);
      self.websocketStatus_.pongAtMs = millis();
    } else if (message.op_code <= 2) {
      {
        TaskLockGuard lock(self.mux_);
        self.websocketStatus_.receiveBytes += std::max(0, message.data_len);
        const uint64_t second = static_cast<uint64_t>(esp_timer_get_time()) / 1000000;
        auto &bin = buffer.rates[second % 60];
        if (bin.second != second) bin = {second, 0, 0};
        bin.rx += std::max(0, message.data_len);
      }
      shotTraceNoteLaMarzoccoRx(static_cast<uint32_t>(std::max(0, message.data_len)));
      if (!self.websocketAdmitted()) return;
      bool valid = message.data_len >= 0 && message.payload_len >= 0 &&
          message.payload_offset >= 0 && message.payload_offset <= message.payload_len &&
          message.data_len <= message.payload_len - message.payload_offset;
      if (message.payload_offset == 0) {
        valid = valid && (message.op_code == 0 ? buffer.continuation : !buffer.continuation);
        buffer.payloadOffset = 0;
        buffer.payloadLength = message.payload_len;
      }
      valid = valid && buffer.payloadOffset == message.payload_offset &&
          buffer.payloadLength == message.payload_len;
      if (valid) {
        if (!buffer.decoder.pending()) {
          TaskLockGuard lock(self.mux_);
          buffer.frameIntent = self.powerState_.generation();
        }
        buffer.payloadOffset += message.data_len;
        if (buffer.payloadOffset == buffer.payloadLength) buffer.continuation = !message.fin;
        valid = buffer.decoder.feed(message.data_ptr, message.data_len, millis(), stompFrame, &self, ingressAttempt);
        if (buffer.decoder.incompleteExpired(millis())) valid = false;
        if (buffer.decoder.pending())
          buffer.partialAtMs.store(buffer.decoder.pendingSince(), std::memory_order_release);
        buffer.partialPending.store(buffer.decoder.pending(), std::memory_order_release);
      }
      if (!valid) buffer.failed.store(true, std::memory_order_release);
      // Yield only after parsing/publication and outside all application locks.
      taskYIELD();
    }
  }
  if (buffer.failed.load(std::memory_order_acquire)) {
    TaskLockGuard lock(self.mux_);
    if (buffer.stamp.identity == self.identityGeneration_ &&
        buffer.stamp.epoch == self.observationFence_.epoch) {
      self.observationFence_.invalidate();
      self.backflush_.invalidate();
      self.snapshotPending_ = false;
      self.powerState_.hold(self.published_, true, millis());
    }
  }
  if (self.task_) xTaskNotifyGive(self.task_);
}

void OpenBrewByWeightLaMarzoccoService::serviceWebSocket() {
  static_assert(sizeof(WebSocketBuffer) <= 24 * 1024);
  LaMarzoccoPersistedSettings settings;
  {
    TaskLockGuard lock(mux_);
    settings = config_;
  }
  const bool selected = settings.accountConfigured &&
      (settings.options & LA_MARZOCCO_OBSERVE_STATE) != 0 &&
      settings.connectionType == static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET);
  LaMarzoccoError gate = LaMarzoccoError::NONE;
  const bool maintenance = outboundMaintenance.load();
  const bool admitted = selected && websocketAdmitted() && networkEligible(gate, true);
  if (!admitted) {
    {
      TaskLockGuard lock(mux_);
      if (maintenance) {
        powerState_.hold(published_, settings.accountConfigured &&
            (settings.options & LA_MARZOCCO_OBSERVE_STATE) != 0, millis());
        if (websocket_ && websocket_->client && websocket_->stamp.epoch == observationFence_.epoch)
          ++observationFence_.epoch;
        if (!websocketStatus_.pauseAtMs) websocketStatus_.pauseAtMs =
            outboundAcquisitionHeld.load() ? outboundAcquisitionAtMs.load() : millis();
      }
    }
    stopWebSocket(!selected || maintenance || clearSessionRequested_.load(std::memory_order_acquire));
    TaskLockGuard lock(mux_);
    websocketStatus_.state = !selected ? LaMarzoccoSocketState::Disabled
        : maintenance ? LaMarzoccoSocketState::PAUSED
        : gate == LaMarzoccoError::CLOCK_UNSYNCED ? LaMarzoccoSocketState::WAITING_FOR_TIME
        : LaMarzoccoSocketState::WAITING_FOR_NETWORK;
    websocketStatus_.reason = !selected ? "disabled" : maintenance ? "maintenance"
        : laMarzoccoErrorName(gate);
    wipeLaMarzoccoSettings(settings);
    return;
  }
  uint32_t now = millis();
  if (websocketRetryRequested_.exchange(false, std::memory_order_acq_rel)) {
    websocketAuthRecoveries_ = 0;
    if (websocket_) websocket_->attempts = 0;
    TaskLockGuard lock(mux_);
    websocketStatus_.retryAtMs = 0;
    if (websocketStatus_.state == LaMarzoccoSocketState::AUTH_ERROR) websocketStatus_.state = LaMarzoccoSocketState::BACKOFF;
  }
  if (websocket_ && websocket_->client) {
    const bool partialPending = websocket_->partialPending.load(std::memory_order_acquire);
    const uint32_t partial = websocket_->partialAtMs.load(std::memory_order_acquire);
    bool subscribed, streaming, obsolete;
    uint32_t pongAtMs, connectedAtMs;
    {
      TaskLockGuard lock(mux_);
      subscribed = websocketStatus_.subscribed;
      streaming = websocketStatus_.state == LaMarzoccoSocketState::STREAMING;
      pongAtMs = websocketStatus_.pongAtMs;
      connectedAtMs = websocketStatus_.connectedAtMs;
      obsolete = websocket_->stamp.epoch != observationFence_.epoch;
    }
    now = millis();  // Callback timestamps in the snapshot can postdate the first sample.
    const bool parked = websocket_->parked.load(std::memory_order_acquire);
    // The STOMP handshake gets 10 s from transport connect; before that the
    // SDK's own 10 s network timeout plus DNS bounds the connect itself.
    const bool timeout = (!subscribed && (connectedAtMs
        ? now - connectedAtMs >= 10000 : now - websocket_->setupAtMs >= 25000)) ||
        (partialPending && now - partial >= 10000) ||
        (subscribed && !parked && now - (pongAtMs ? pongAtMs : connectedAtMs) >= 30000);
    if (!websocket_->failed.load(std::memory_order_acquire) && !timeout && !obsolete) {
      // From a scaled shot's start until its relay opens the radio is the
      // scale's: the subscription parks and pings stop, the socket stays, and
      // both resume at once so the cloud's post-shot state frame lands.
      const bool park = shotTransportPaused();
      if (subscribed && park != parked) {
        (void)esp_websocket_client_set_ping_interval_sec(websocket_->client,
                                                         park ? kParkedPingSec : kPingSec);
        if (!sendSubscription(!park)) websocket_->failed.store(true, std::memory_order_release);
      }
      // The open subscription survives token renewal: only HTTPS needs the new
      // token, so renew in place instead of reconnecting and re-reading.
      if (!park && work_ && la_marzocco_timing::accessTokenRefreshDue(now - work_->accessTokenIssuedAtMs) &&
          tickReached(now, websocket_->renewRetryAtMs) &&
          !ensureSession(settings, false))
        websocket_->renewRetryAtMs = millis() + la_marzocco_timing::kExhaustedCooldownMs;
      if (streaming && now - connectedAtMs >= 30000) {
        websocket_->attempts = 0;
        websocketAuthRecoveries_ = 0;
      }
      wipeLaMarzoccoSettings(settings);
      return;
    }
    const bool auth = websocket_->authFailed.load(std::memory_order_acquire);
    if (timeout && !obsolete) {
      TaskLockGuard lock(mux_);
      websocketUnexpectedReconnect_ = true;
    }
    stopWebSocket();
    if (auth) clearSession();
    now = millis();  // Backoff begins after transport/session cleanup completes.
    TaskLockGuard lock(mux_);
    ++observationFence_.epoch;
    powerState_.hold(published_, true, now);
    if (!obsolete) ++websocketStatus_.errors;
    websocketStatus_.reason = auth ? "authentication" : obsolete ? "obsolete"
        : timeout ? "frame_timeout" : "transport";
    if (!obsolete) {
      if (auth) ++websocketAuthRecoveries_;
      ++websocket_->attempts;
      websocketStatus_.retryAtMs = now + (websocket_->attempts < la_marzocco_timing::kWsMaxAttempts
          ? la_marzocco_timing::kWsReconnectDelaysMs[websocket_->attempts - 1]
          : la_marzocco_timing::kExhaustedCooldownMs);
      websocketStatus_.state = websocketAuthRecoveries_ > 1
          ? LaMarzoccoSocketState::AUTH_ERROR : LaMarzoccoSocketState::BACKOFF;
      if (websocket_->attempts >= la_marzocco_timing::kWsMaxAttempts) websocket_->attempts = 0;
    } else {
      websocketStatus_.retryAtMs = 0;
      websocketStatus_.state = LaMarzoccoSocketState::BACKOFF;
    }
  }
  uint32_t retryAtMs;
  {
    TaskLockGuard lock(mux_);
    retryAtMs = websocketStatus_.retryAtMs;
  }
  if (websocketAuthRecoveries_ > 1) {
    TaskLockGuard lock(mux_);
    websocketStatus_.state = LaMarzoccoSocketState::AUTH_ERROR;
    websocketStatus_.reason = "authentication";
    wipeLaMarzoccoSettings(settings);
    return;
  }
  if (retryAtMs && !tickReached(now, retryAtMs)) {
    wipeLaMarzoccoSettings(settings);
    return;
  }
  if (!websocketAdmitted(true)) {
    TaskLockGuard lock(mux_);
    websocketStatus_.state = LaMarzoccoSocketState::PAUSED;
    websocketStatus_.reason = shotTransportPaused() ? "shot" : "scale";
    wipeLaMarzoccoSettings(settings);
    return;
  }
  if (!websocket_) {
    auto *buffer = static_cast<WebSocketBuffer *>(allocExternal(sizeof(WebSocketBuffer)));
    if (!buffer) {
      TaskLockGuard lock(mux_);
      ++websocketStatus_.allocationFailures;
      websocketStatus_.state = LaMarzoccoSocketState::BACKOFF;
      websocketStatus_.reason = "allocation_failed";
      websocketStatus_.retryAtMs = millis() + la_marzocco_timing::kExhaustedCooldownMs;
      wipeLaMarzoccoSettings(settings);
      return;
    }
    new (buffer) WebSocketBuffer{};
    TaskLockGuard lock(mux_);
    websocket_ = buffer;
    websocketStatus_.retainedBytes = sizeof(WebSocketBuffer);
    websocketStatus_.workspaceExternal = pointerIsExternal(buffer);
  }
  {
    TaskLockGuard lock(mux_);
    websocketStatus_.state = LaMarzoccoSocketState::AUTHENTICATING;
  }
  bool renewed = false;
  if (!ensureWorkBuffer() || !ensureSession(settings, false, &renewed) || !websocketAdmitted(true)) {
    TaskLockGuard lock(mux_);
    if (classifyFailure() == LaMarzoccoError::INVALID_AUTH) websocketAuthRecoveries_ = 2;
    websocketStatus_.state = websocketAuthRecoveries_ > 1
        ? LaMarzoccoSocketState::AUTH_ERROR : LaMarzoccoSocketState::BACKOFF;
    websocketStatus_.reason = "session_unavailable";
    const uint8_t attempt = ++websocket_->attempts;
    websocketStatus_.retryAtMs = millis() + (attempt < la_marzocco_timing::kWsMaxAttempts
        ? la_marzocco_timing::kWsReconnectDelaysMs[attempt - 1] : la_marzocco_timing::kExhaustedCooldownMs);
    if (attempt >= la_marzocco_timing::kWsMaxAttempts) websocket_->attempts = 0;
    wipeLaMarzoccoSettings(settings);
    return;
  }
  auto &buffer = *websocket_;
  buffer.failed.store(false);
  buffer.authFailed.store(false);
  buffer.parked.store(false);
  buffer.setupAtMs = millis();
  {
    TaskLockGuard lock(mux_);
    buffer.stamp = observationFence_.stamp(identityGeneration_, powerState_.generation());
    websocketStatus_.epoch = buffer.stamp.epoch;
    websocketStatus_.pauseAtMs = 0;
    websocketStatus_.connectedAtMs = 0;
    snprintf(buffer.subscription, sizeof(buffer.subscription), "micra-%lu", static_cast<unsigned long>(buffer.stamp.epoch));
  }
  const int destinationLength = snprintf(buffer.destination, sizeof(buffer.destination),
      "/ws/sn/%s/dashboard", settings.selectedSerial);
  const int connectLength = snprintf(buffer.connect, sizeof(buffer.connect),
      "CONNECT\nhost:lion.lamarzocco.io\naccept-version:1.2,1.1\nheart-beat:0,0\nAuthorization:Bearer %s\n\n", work_->accessToken);
  bool ok = destinationLength > 0 && static_cast<size_t>(destinationLength) < sizeof(buffer.destination) &&
      connectLength > 0 && static_cast<size_t>(connectLength) < sizeof(buffer.connect) &&
      strpbrk(work_->accessToken, "\r\n") == nullptr &&
      strpbrk(settings.selectedSerial, "\r\n:\\") == nullptr &&
      applySignedHeaders(settings, buffer.headers, sizeof(buffer.headers));
  esp_websocket_client_config_t config{};
  config.uri = "wss://lion.lamarzocco.io/ws/connect";
  config.headers = buffer.headers;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.disable_auto_reconnect = true;
  config.buffer_size = 4096;  // Above the internal-first malloc threshold.
  config.task_prio = 1;  // Zero selects SDK default 5, rather than idle priority.
  config.task_stack = 6144;
  config.task_core_id_set = true;
  config.task_core_id = 0;
  config.network_timeout_ms = 10000;  // SDK default; includes the initial TLS handshake.
  config.ping_interval_sec = kPingSec;
  config.pingpong_timeout_sec = 30;
  const auto before = sampleHeapCaps();
  recordBootHeap(BootHeapStage::WS_START, BootHeapResult::OK, millis(), before);
  if (ok && websocketAdmitted(true)) buffer.client = esp_websocket_client_init(&config);
  recordBootHeap(BootHeapStage::WS_INIT,
                 buffer.client ? BootHeapResult::OK : BootHeapResult::FAILED, millis());
  ok = buffer.client && esp_websocket_register_events(buffer.client, WEBSOCKET_EVENT_ANY,
                                                     websocketEvent, this) == ESP_OK;
  {
    TaskLockGuard lock(mux_);
    beginHeapLifecycle(buffer.heap, HeapLifecycleEvent::TLS_REQUEST, before);
    websocketStatus_.state = LaMarzoccoSocketState::CONNECTING;
    ++websocketStatus_.reconnects;
  }
  ok = ok && websocketAdmitted(true) && esp_websocket_client_start(buffer.client) == ESP_OK;
  recordBootHeap(BootHeapStage::WS_TASK, ok ? BootHeapResult::OK : BootHeapResult::FAILED, millis());
  if (!ok) {
    recordBootHeap(BootHeapStage::WS_DONE, BootHeapResult::FAILED, millis());
    const bool unexpected = websocketAdmitted(true);
    stopWebSocket();
    buffer.failed.store(true);
    TaskLockGuard lock(mux_);
    websocketUnexpectedReconnect_ = websocketUnexpectedReconnect_ ||
        (unexpected && buffer.stamp.epoch == observationFence_.epoch);
    websocketStatus_.state = LaMarzoccoSocketState::BACKOFF;
    websocketStatus_.reason = "start_failed";
    const uint8_t attempt = ++buffer.attempts;
    websocketStatus_.retryAtMs = millis() + (attempt < la_marzocco_timing::kWsMaxAttempts
        ? la_marzocco_timing::kWsReconnectDelaysMs[attempt - 1] : la_marzocco_timing::kExhaustedCooldownMs);
    if (attempt >= la_marzocco_timing::kWsMaxAttempts) buffer.attempts = 0;
  }
  wipeLaMarzoccoSettings(settings);
}
