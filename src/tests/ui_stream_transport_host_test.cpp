#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>

namespace {
using httpd_handle_t = void *;
struct Result {
  int value;
  uint32_t delay = 0;
  int error = 0;
};
uint32_t now = 0;
std::deque<Result> readiness, writes;
char payload[24000]{};
size_t accepted = 0;
std::string wire;
std::deque<size_t> chunks;
unsigned logs = 0;
std::string lastLog;
uint32_t millis() { return now; }

int testSelect(int count, fd_set *, fd_set *writable, fd_set *, timeval *wait) {
  assert(count == 2 && FD_ISSET(1, writable));
  const auto budget = static_cast<uint32_t>(wait->tv_sec * 1000 + wait->tv_usec / 1000);
  assert(budget <= 500);
  if (readiness.empty()) return 1;
  const Result next = readiness.front();
  readiness.pop_front();
  if (next.value == 0 || next.delay > budget) {
    now += budget;
    return 0;
  }
  now += next.delay;
  errno = next.error;
  return next.value;
}

int testSend(int fd, const char *data, size_t length, int flags) {
  assert(fd == 1 && (flags & MSG_DONTWAIT));
  Result next{static_cast<int>(length)};
  if (!writes.empty()) { next = writes.front(); writes.pop_front(); }
  now += next.delay;
  errno = next.error;
  if (next.value > 0) {
    assert(static_cast<size_t>(next.value) <= length);
    accepted += static_cast<size_t>(next.value);
    wire.append(data, static_cast<size_t>(next.value));
    chunks.push_back(static_cast<size_t>(next.value));
  }
  return next.value;
}

void testLog(const char *format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  lastLog = line;
  ++logs;
  errno = EIO;  // Logging must not overwrite the socket failure returned upstream.
}

#define select testSelect
#define send testSend
#define ESP_LOGW(tag, ...) testLog(__VA_ARGS__)
#define OPEN_BREW_BY_WEIGHT_PSRAM_BSS
#include "network/OpenBrewByWeightUiStreamTransport.inc"
#undef ESP_LOGW
#undef send
#undef select

void reset() {
  now += 1000;
  readiness.clear(); writes.clear(); accepted = 0; errno = 0;
  wire.clear(); chunks.clear();
  uiStreamSendKind = "stats";
}
// Raw write loop: headers are uncommitted, payloads follow a header on the wire.
int transmit(size_t length) {
  return uiStreamWrite(1, payload, length, length > 14);
}
// One WebSocket frame through the coalescing send_fn, as IDF issues it.
int frame(const char *body, size_t length) {
  char header[10] = {static_cast<char>(0x81)};
  size_t bytes = 2;
  if (length < 126) header[1] = static_cast<char>(length);
  else { header[1] = 126; header[2] = static_cast<char>(length >> 8);
         header[3] = static_cast<char>(length); bytes = 4; }
  if (uiStreamSend(nullptr, 1, header, bytes, 0) < 0) return -1;
  return length ? uiStreamSend(nullptr, 1, body, length, 0) : 0;
}
}  // namespace

int main() {
  reset();
  writes = {{2}, {2}};
  assert(transmit(4) == 4 && accepted == 4);

  reset();
  readiness = {{0}};
  uint32_t started = now;
  assert(transmit(4) == -1 && now - started == 100 && uiStreamSendCleanAbort);
  assert(errno == EAGAIN && lastLog.find("stats fd=1 bytes=0/4 ms=100") != std::string::npos);

  reset();
  readiness = {{1}, {0}}; writes = {{2}};
  assert(transmit(4) == -1 && accepted == 2 && !uiStreamSendCleanAbort);

  // A large single curve can cross the TCP buffer size and an ACK sleep gap.
  reset();
  readiness = {{1}, {1, 150}}; writes = {{16380}};
  started = now;
  assert(transmit(20480) == 20480 && accepted == 20480 && now - started == 150);
  assert(!uiStreamSendCleanAbort);

  reset();
  readiness = {{1, 400}};
  assert(transmit(8192) == 8192);

  reset();
  readiness = {{0}};
  started = now;
  assert(transmit(512) == -1 && now - started == 500 && !uiStreamSendCleanAbort);
  assert(errno == EAGAIN);  // Even zero payload progress follows a committed header.

  reset();
  readiness = {{1, 300}, {1, 300}}; writes = {{100}};
  started = now;
  assert(transmit(8192) == -1 && now - started == 500 && accepted == 100);
  assert(!uiStreamSendCleanAbort);  // Progress must not renew the deadline.

  // The dispatch deadline caps each wait: a header wait ends at the deadline
  // and stays cleanly deferrable, a committed one ends there as torn, progress
  // does not extend it, and a spent deadline fails before the first wait.
  reset(); readiness = {{0}}; started = now;
  uiStreamDeadlineStartMs = now - 760; uiStreamDeadlineBudgetMs = 800;
  assert(transmit(4) == -1 && now - started == 40 && uiStreamSendCleanAbort);
  reset(); readiness = {{0}}; started = now;
  uiStreamDeadlineStartMs = now - 760;
  assert(transmit(512) == -1 && now - started == 40 && !uiStreamSendCleanAbort);
  reset(); readiness = {{1, 30}, {0}}; writes = {{100}}; started = now;
  uiStreamDeadlineStartMs = now - 700;
  assert(transmit(8192) == -1 && now - started == 100 && accepted == 100);
  reset(); readiness = {{0}}; started = now;
  uiStreamDeadlineStartMs = now - 800;
  assert(transmit(4) == -1 && now == started && readiness.size() == 1 && uiStreamSendCleanAbort);
  assert(uiStreamDeadlineLeft() == 0);
  uiStreamDeadlineBudgetMs = UINT32_MAX;  // Outside a dispatch the stage limits alone apply.
  reset(); readiness = {{0}}; started = now;
  assert(transmit(512) == -1 && now - started == 500);

  for (int error : {EPIPE, ECONNRESET, ENOTCONN, EBADF, ENOTSOCK}) {
    reset(); writes = {{-1, 0, error}};
    assert(transmit(4) == -1 && errno == error && !uiStreamSendCleanAbort);
  }
  for (int error : {EINVAL, ENOMEM}) {
    reset(); readiness = {{-1, 0, error}};
    assert(transmit(4) == -1 && errno == error && !uiStreamSendCleanAbort);
  }

  reset();
  readiness = {{-1, 30, EINTR}, {1}, {1, 120}};
  writes = {{-1, 0, EAGAIN}};
  started = now;
  assert(transmit(8192) == 8192 && now - started == 150);

  reset(); writes = {{-1, 20, EINTR}, {-1, 20, EWOULDBLOCK}};
  assert(transmit(512) == 512);

  reset(); writes = {{0, 0, EAGAIN}};
  assert(transmit(512) == -1 && errno == ECONNRESET && !uiStreamSendCleanAbort);

  reset(); now = UINT32_MAX - 50; started = now;
  readiness = {{1, 400}};
  assert(transmit(512) == 512 && now - started == 400);

  reset(); readiness = {{0}};
  assert(transmit(512) == -1);
  const unsigned logged = logs;
  readiness = {{0}};
  assert(transmit(4) == -1 && logs == logged);
  now += 1000; readiness = {{0}};
  assert(transmit(4) == -1 && logs == logged + 1);
  assert(transmit(0) == 0 && !uiStreamSendCleanAbort);

  // A small frame leaves as one write: header and payload in one segment.
  reset(); memcpy(payload, "{\"a\":1}", 7);
  assert(frame(payload, 7) == 7 && chunks.size() == 1 && wire.size() == 9);
  assert(wire.compare(2, 7, payload, 7) == 0 && !uiStreamAwaitingPayload);
  // An empty frame (header only) is not held back.
  reset(); assert(frame(payload, 0) == 0 && chunks.size() == 1 && wire.size() == 2);
  // Nothing written yet: the coalesced frame is still cleanly deferrable.
  reset(); readiness = {{0}}; started = now;
  assert(frame(payload, 7) == -1 && uiStreamSendCleanAbort && now - started == 100);
  assert(uiStreamPendingBytes == 0 && !uiStreamAwaitingPayload && accepted == 0);
  // Once part of it is out, the frame is committed.
  reset(); readiness = {{1}, {0}}; writes = {{3}};
  assert(frame(payload, 7) == -1 && !uiStreamSendCleanAbort && accepted == 3);
  // A payload larger than one segment follows its already flushed header.
  reset();
  assert(frame(payload, 2000) == 2000 && chunks.size() == 2 && chunks[0] == 4);
  reset(); readiness = {{1}, {0}};
  assert(frame(payload, 2000) == -1 && !uiStreamSendCleanAbort && accepted == 4);
  // Bytes that are not a server frame header are written as-is.
  reset(); payload[1] = static_cast<char>(0x80);
  assert(uiStreamSend(nullptr, 1, payload, 2, 0) == 2 && chunks.size() == 1);
  payload[1] = 0;

  // A dispatch batch sends Home and the shot card in one write.
  reset(); uiStreamBatchBegin();
  assert(frame(payload, 100) == 100 && frame(payload, 900) == 900 && accepted == 0);
  assert(uiStreamBatchEnd(true) && chunks.size() == 1 && wire.size() == 2 + 100 + 4 + 900);
  // Overflow flushes the batch so far with the next header; its payload follows.
  reset(); uiStreamBatchBegin();
  assert(frame(payload, 800) == 800 && frame(payload, 800) == 800 && chunks.size() == 2);
  assert(chunks[0] == 4 + 800 + 4 && uiStreamBatchEnd(true) && chunks.size() == 2);
  assert(wire.size() == 2 * (4 + 800));
  // A cleanly dropped batch reports failure with nothing on the wire.
  reset(); uiStreamBatchBegin(); readiness = {{0}};
  assert(frame(payload, 100) == 100 && frame(payload, 200) == 200);
  assert(!uiStreamBatchEnd(true) && uiStreamSendCleanAbort && accepted == 0 && !uiStreamBatching);
  // A mid-batch failure after partial output is broken; nothing more is written.
  reset(); uiStreamBatchBegin(); readiness = {{1}, {0}}; writes = {{10}};
  assert(frame(payload, 800) == 800 && frame(payload, 800) == -1 && !uiStreamSendCleanAbort);
  assert(frame(payload, 100) == -1 && accepted == 10);
  assert(!uiStreamBatchEnd(true) && !uiStreamSendCleanAbort);
  // A payload that no longer fits follows its flushed header out: committed.
  reset(); uiStreamBatchBegin(); readiness = {{1}, {0}};
  assert(frame(payload, 600) == 600 && frame(payload, 900) == -1 && !uiStreamSendCleanAbort);
  assert(accepted == 604 + 4 && !uiStreamBatchEnd(true) && !uiStreamSendCleanAbort);
  // A large Home goes out; a cleanly dropped card leaves no partial frame.
  reset(); uiStreamBatchBegin();
  assert(frame(payload, 2000) == 2000 && frame(payload, 100) == 100 && accepted == 2004);
  readiness = {{0}};
  assert(!uiStreamBatchEnd(true) && uiStreamSendCleanAbort && accepted == 2004);
  // A socket about to close drops the batch instead of writing it.
  reset(); uiStreamBatchBegin();
  assert(frame(payload, 100) == 100 && uiStreamBatchEnd(false) && accepted == 0);
  assert(uiStreamPendingBytes == 0 && !uiStreamAwaitingPayload);
  // 16- and 64-bit length headers; masked or misframed bytes are not headers.
  const char wide[10] = {static_cast<char>(0x81), 127, 0, 0, 0, 0, 0, 1, 0x11, 0x70};
  assert(uiStreamHeaderPayload(wide, 10) == 70000 && uiStreamHeaderPayload(wide, 9) == -1);
  const char mid[4] = {static_cast<char>(0x81), 126, 0x07, static_cast<char>(0xd0)};
  assert(uiStreamHeaderPayload(mid, 4) == 2000 && uiStreamHeaderPayload(mid, 2) == -1);
  const char masked[2] = {static_cast<char>(0x81), static_cast<char>(0x85)};
  assert(uiStreamHeaderPayload(masked, 2) == -1);
  puts("UI stream transport: bounded writes, modem-sleep gaps, partial failures, errno, log throttling, frame and batch coalescing passed");
}
