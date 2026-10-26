// httpd is the sole caller/owner, including automatic WS control replies and
// the coalescing buffer below.
constexpr uint32_t kUiStreamHeaderWaitMs = 100, kUiStreamPayloadWaitMs = 500;
constexpr uint32_t kStatsStreamPaceMs = 100;
constexpr uint32_t kUiStreamDispatchBudgetMs = 800;
// The dispatch arms a per-session deadline that caps every wait below, so a
// stalled peer cannot hold the httpd task past the budget; outside a dispatch
// (op replies, control frames) there is none.
uint32_t uiStreamDeadlineStartMs = 0, uiStreamDeadlineBudgetMs = UINT32_MAX;
uint32_t uiStreamDeadlineLeft() {
  const uint32_t used = millis() - uiStreamDeadlineStartMs;
  return used < uiStreamDeadlineBudgetMs ? uiStreamDeadlineBudgetMs - used : 0;
}
// One TCP segment (lwIP MSS 1440): a frame's header and payload, and a
// dispatch's Home + shot card pair, leave in one write so the radio shared
// with the BLE scale transmits once instead of once per piece.
constexpr size_t kUiStreamCoalesceBytes = 1436;
bool uiStreamSendCleanAbort = false;
const char *uiStreamSendKind = "control";
OPEN_BREW_BY_WEIGHT_PSRAM_BSS char uiStreamPending[kUiStreamCoalesceBytes];
size_t uiStreamPendingBytes = 0;
int uiStreamPendingFd = -1;
bool uiStreamAwaitingPayload = false, uiStreamBatching = false;
// A failed write drops the whole batch; clean means none of it was written.
bool uiStreamBatchFailed = false, uiStreamBatchClean = true;

int uiStreamSendFailed(int fd, size_t done, size_t length, uint32_t started,
                       bool committed) {
  const int error = errno;
  uiStreamSendCleanAbort = done == 0 && !committed &&
      (error == EAGAIN || error == EWOULDBLOCK);
  uiStreamPendingBytes = 0;
  uiStreamAwaitingPayload = false;
  if (uiStreamBatching) {
    uiStreamBatchFailed = true;
    uiStreamBatchClean = uiStreamBatchClean && uiStreamSendCleanAbort;
  }
  static uint32_t failures = 0, loggedAt = 0;
  const uint32_t now = millis();
  if (++failures == 1 || now - loggedAt >= 1000) {
    loggedAt = now;
    ESP_LOGW("ui_stream", "%s fd=%d bytes=%u/%u ms=%lu errno=%d failures=%lu",
             uiStreamSendKind, fd, static_cast<unsigned>(done),
             static_cast<unsigned>(length), static_cast<unsigned long>(now - started),
             error, static_cast<unsigned long>(failures));
  }
  errno = error;
  return -1;
}

// Bounded nonblocking write that accepts short sends. Until its first byte is
// out an uncommitted write stays deferrable; once anything of the frame is on
// the wire, allow modem-sleep ACK gaps. The dispatch deadline only shortens a
// wait: a timed-out write fails the same way at either limit.
int uiStreamWrite(int fd, const char *data, size_t length, bool committed) {
  uiStreamSendCleanAbort = false;
  const uint32_t started = millis();
  size_t done = 0;
  while (done < length) {
    const uint32_t budget = committed || done ? kUiStreamPayloadWaitMs
                                              : kUiStreamHeaderWaitMs;
    const uint32_t age = millis() - started, left = uiStreamDeadlineLeft();
    uint32_t wait = age < budget ? budget - age : 0;
    if (wait > left) wait = left;
    if (wait == 0) {
      errno = EAGAIN;
      break;
    }
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    timeval timeout = {0, static_cast<suseconds_t>(wait * 1000)};
    const int ready = select(fd + 1, nullptr, &writable, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) {
      errno = EAGAIN;
      break;
    }
    const int sent = send(fd, data + done, length - done, MSG_DONTWAIT);
    if (sent > 0) done += static_cast<size_t>(sent);
    else if (sent == 0) {
      errno = ECONNRESET;
      break;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
  }
  return done == length ? static_cast<int>(done)
                        : uiStreamSendFailed(fd, done, length, started, committed);
}

int uiStreamFlush() {
  const size_t length = uiStreamPendingBytes;
  uiStreamPendingBytes = 0;
  return length ? uiStreamWrite(uiStreamPendingFd, uiStreamPending, length, false) : 0;
}

// Payload length announced by an unmasked server frame header of exactly
// `length` bytes, or -1 when the bytes are not such a header.
int64_t uiStreamHeaderPayload(const char *data, size_t length) {
  if (length < 2 || (data[1] & 0x80) != 0) return -1;
  const uint8_t code = static_cast<uint8_t>(data[1]);
  if (code < 126) return length == 2 ? code : -1;
  const size_t bytes = code == 126 ? 2 : 8;
  if (length != 2 + bytes) return -1;
  uint64_t value = 0;
  for (size_t index = 0; index < bytes; ++index)
    value = (value << 8) | static_cast<uint8_t>(data[2 + index]);
  return value > INT64_MAX ? -1 : static_cast<int64_t>(value);
}

// IDF calls send_fn with each frame's header, then its payload unless empty.
// Small pieces are held and flushed at the frame end (or the batch end). A
// payload that does not fit follows its header out, so it is committed.
int uiStreamSend(httpd_handle_t, int fd, const char *data, size_t length, int) {
  if (uiStreamBatching && uiStreamBatchFailed) {
    // Nothing more after a failed write: the batch outcome stays exact.
    uiStreamSendCleanAbort = uiStreamBatchClean;
    return -1;
  }
  if (uiStreamPendingBytes && fd != uiStreamPendingFd && uiStreamFlush() < 0) return -1;
  const bool payload = uiStreamAwaitingPayload;
  const int64_t announced = payload ? 0 : uiStreamHeaderPayload(data, length);
  if (announced < 0) {
    if (uiStreamFlush() < 0) return -1;
    return uiStreamWrite(fd, data, length, true);
  }
  uiStreamAwaitingPayload = !payload && announced > 0;
  const bool split = uiStreamPendingBytes + length > kUiStreamCoalesceBytes;
  if (split && uiStreamFlush() < 0) return -1;
  if (split && payload) return uiStreamWrite(fd, data, length, true);
  memcpy(uiStreamPending + uiStreamPendingBytes, data, length);
  uiStreamPendingBytes += length;
  uiStreamPendingFd = fd;
  uiStreamSendCleanAbort = false;
  if (uiStreamAwaitingPayload || uiStreamBatching) return static_cast<int>(length);
  return uiStreamFlush() < 0 ? -1 : static_cast<int>(length);
}

// The dispatch's Home + shot card frames share one write.
void uiStreamBatchBegin() {
  uiStreamBatching = true;
  uiStreamBatchFailed = false;
  uiStreamBatchClean = true;
}

// Returns false when any write of the batch failed; uiStreamSendCleanAbort
// then tells whether the failure left no partial frame on the wire. A socket about to close
// drops its pending bytes instead of spending the dispatch budget on them.
bool uiStreamBatchEnd(bool deliver) {
  if (deliver) (void)uiStreamFlush();
  uiStreamPendingBytes = 0;
  uiStreamAwaitingPayload = false;
  uiStreamBatching = false;
  uiStreamSendCleanAbort = uiStreamBatchClean;
  return !uiStreamBatchFailed;
}
