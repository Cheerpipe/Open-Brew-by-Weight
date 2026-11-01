// Companion link's diagnostic log producer (network/OpenBrewByWeightCompanionLog.inc)
// against a real debug ring: `logsub` parsing, the boot mapping, paging inside
// the frame budget, worst-case text, gaps and an unresumable cursor, and the
// hold's queue-and-flush. The rest of the network class is a stand-in.

#define FW_VERSION "1.2.3-test"

#include "OpenBrewByWeightOutboundAdmission.h"
#include "OpenBrewByWeightTaskMutex.h"
#include "network/OpenBrewByWeightCompanionLink.h"

#include <cJSON.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                             \
    }                                                                           \
  } while (0)

namespace openbrewbyweight {

bool jsonBoolean(cJSON *object, const char *name, bool &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsBool(item)) return false;
  output = cJSON_IsTrue(item);
  return true;
}

bool jsonUint32(cJSON *object, const char *name, uint32_t &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 || item->valuedouble > UINT32_MAX ||
      std::floor(item->valuedouble) != item->valuedouble)
    return false;
  output = static_cast<uint32_t>(item->valuedouble);
  return true;
}

// The webhook's escaper, which the firmware build links: quotes, backslashes
// and control bytes, the latter as \u00XX.
bool escapeJsonString(const char *input, char *output, size_t capacity) {
  size_t used = 0;
  for (; *input != '\0'; ++input) {
    const unsigned char c = static_cast<unsigned char>(*input);
    char piece[7];
    const int length = c == '"' || c == '\\' ? std::snprintf(piece, sizeof(piece), "\\%c", c)
                       : c < 0x20            ? std::snprintf(piece, sizeof(piece), "\\u%04x", c)
                                             : std::snprintf(piece, sizeof(piece), "%c", c);
    if (used + static_cast<size_t>(length) >= capacity) return false;
    std::memcpy(output + used, piece, static_cast<size_t>(length));
    used += static_cast<size_t>(length);
  }
  output[used] = '\0';
  return true;
}

struct CompanionWorkspace {
  struct {
    char bodies[COMPANION_FRAME_BYTES];
  } job;
  char escape[DEBUG_EVENT_TEXT_CAPACITY * 6 + 1];
};
CompanionWorkspace workspace;
CompanionWorkspace &companionWs() { return workspace; }
CompanionState g_companion;
const CompanionLinkSlot *companionActiveLink() { return &g_companion.links[0]; }

DebugRingBuffer ring;
uint32_t bootId = 42;
struct Callbacks {
  size_t (*copyDebugEvents)(uint32_t, DebugEvent *, size_t, DebugLogReadMetadata *);
  uint32_t (*debugLogLatestSequence)();
  uint32_t (*debugLogLatestAlarmSequence)();
  uint32_t (*debugLogBootId)();
};

class OpenBrewByWeightNetwork {
 public:
  bool companionLogHoldActive() const;
  bool companionSyncHold() const;
  bool networkFlashWriteAllowed() const { return !flashGateClosed; }
  bool flashGateClosed = false;
  bool companionLinkLogSub(CompanionLinkSlot &slot, cJSON *frame);
  void companionLogService();
  void companionLogApplyLocked(CompanionLinkSlot &link);
  bool companionLogBody(uint8_t &count, const CompanionLinkSlot &sent, bool logSwitch, uint32_t &logLast,
                        bool &logMore, uint32_t &logMiss);
  static void companionOutboxChanged() { ++outboxChanges; }
  static inline int outboxChanges = 0;
  Callbacks callbacks_{
      [](uint32_t after, DebugEvent *out, size_t capacity, DebugLogReadMetadata *meta) {
        return ring.copyAfter(after, out, capacity, meta);
      },
      [] { return ring.latestSequence(); }, [] { return ring.latestAlarmSequence(); },
      [] { return bootId; }};
  mutable TaskMutex dataMux_;
  std::atomic<bool> scaleConnecting_{false}, companionNotified_{false};
  uint32_t companionLogBootId_ = 0;
};

}  // namespace openbrewbyweight

#include "network/OpenBrewByWeightCompanionLog.inc"

using namespace openbrewbyweight;

namespace {

OpenBrewByWeightNetwork network;
CompanionLinkSlot &link = g_companion.links[0];

void add(uint32_t count, const char *text = "an event", LogLevel level = LogLevel::INFO) {
  for (uint32_t i = 0; i < count; ++i)
    ring.add(1000 + i, 1789000000 + i, level, DebugCategory::STATE, DebugCode::STATE_TRANSITION, 0, 0, text);
}

bool subscribe(const char *json) {
  cJSON *frame = cJSON_Parse(json);
  const bool ok = network.companionLinkLogSub(link, frame);
  cJSON_Delete(frame);
  if (ok) {
    TaskLockGuard lock(network.dataMux_);
    network.companionLogApplyLocked(link);
  }
  return ok;
}

// One `log` body as the companion would parse it after the sealed prefix.
struct Page {
  bool built = false, more = false, reset = false;
  uint32_t last = 0, miss = 0, boot = 0;
  size_t bytes = 0;
  std::vector<uint32_t> sequences;
  std::vector<std::string> messages;
};

Page page() {
  Page result;
  uint8_t count = 0;
  std::memset(workspace.job.bodies, 0, sizeof(workspace.job.bodies));
  result.last = link.logSent;
  result.built = network.companionLogBody(count, link, true, result.last, result.more, result.miss);
  if (!result.built) return result;
  const std::string json = std::string("{\"q\":1,") + workspace.job.bodies;
  result.bytes = std::strlen(workspace.job.bodies);
  cJSON *frame = cJSON_Parse(json.c_str());
  CHECK(frame != nullptr);
  CHECK(std::string(cJSON_GetObjectItem(frame, "t")->valuestring) == "log");
  result.boot = static_cast<uint32_t>(cJSON_GetObjectItem(frame, "b")->valuedouble);
  result.reset = cJSON_IsTrue(cJSON_GetObjectItem(frame, "reset"));
  cJSON *events = cJSON_GetObjectItem(frame, "e");
  for (cJSON *event = events->child; event != nullptr; event = event->next) {
    CHECK(cJSON_GetArraySize(event) == 6);
    result.sequences.push_back(static_cast<uint32_t>(cJSON_GetArrayItem(event, 0)->valuedouble));
    result.messages.push_back(cJSON_GetArrayItem(event, 5)->valuestring);
  }
  cJSON_Delete(frame);
  // What companionPrepareSends commits once the send job is queued.
  link.logSent = result.last;
  link.logCatchUp = result.more;
  g_companion.logHeard = false;
  return result;
}

void reset() {
  ring.clear();
  link = CompanionLinkSlot{};
  link.fd = 3;
  link.phase = CompanionLinkPhase::READY;
  g_companion.stored.logSwitch = 1;
  g_companion.logHeard = false;
  outboundShotActive.store(false);
  outboundScaleConnected.store(false);
}

void testSubscriptionParsing() {
  reset();
  CHECK(!subscribe("{\"t\":\"logsub\",\"on\":true,\"boot\":42}"));
  CHECK(!subscribe("{\"t\":\"logsub\",\"on\":true,\"boot\":-1,\"after\":0}"));
  CHECK(!subscribe("{\"t\":\"logsub\",\"on\":\"yes\",\"boot\":42,\"after\":0}"));
  const int changes = OpenBrewByWeightNetwork::outboxChanges;
  CHECK(subscribe("{\"t\":\"logsub\",\"on\":true,\"boot\":42,\"after\":7}"));
  CHECK(OpenBrewByWeightNetwork::outboxChanges == changes + 1);
  CHECK(link.logSubscribed && link.logApplied && link.logCatchUp && link.logSent == 7);
  // Another boot's cursor restarts from the oldest retained event.
  CHECK(subscribe("{\"t\":\"logsub\",\"on\":true,\"boot\":41,\"after\":7}"));
  CHECK(link.logSent == 0);
  CHECK(subscribe("{\"t\":\"logsub\",\"on\":false,\"boot\":42,\"after\":7}"));
  CHECK(!link.logSubscribed && !link.logCatchUp);
}

// A full ring pages out in order, each frame inside the budget, nothing twice.
void testPagingWithinTheFrame() {
  reset();
  add(DEBUG_EVENT_CAPACITY + 88, "a fairly ordinary diagnostic line of some length");
  CHECK(subscribe("{\"t\":\"logsub\",\"on\":true,\"boot\":42,\"after\":0}"));
  std::vector<uint32_t> all;
  int frames = 0;
  for (Page p = page(); p.built; p = page(), ++frames) {
    CHECK(p.boot == 42 && !p.reset && p.miss == 0);
    CHECK(p.bytes + COMPANION_MAC_HEX + 40 <= COMPANION_FRAME_BYTES);
    all.insert(all.end(), p.sequences.begin(), p.sequences.end());
    if (!p.more) break;
  }
  CHECK(frames > 1);
  CHECK(all.size() == DEBUG_EVENT_CAPACITY);
  for (size_t i = 0; i < all.size(); ++i) CHECK(all[i] == 89 + i);
  CHECK(!page().built);  // Nothing pending: no frame.
}

// Text that escapes to six bytes a byte still fits, and parses back intact.
void testWorstCaseText() {
  reset();
  std::string text(DEBUG_EVENT_TEXT_CAPACITY - 1, '\x01');
  text[0] = '"';
  add(3, text.c_str());
  CHECK(subscribe("{\"t\":\"logsub\",\"on\":true,\"boot\":42,\"after\":0}"));
  const Page p = page();
  CHECK(p.built && !p.sequences.empty());
  CHECK(p.messages[0] == text);
  CHECK(p.bytes + COMPANION_MAC_HEX + 40 <= COMPANION_FRAME_BYTES);
}

// A cursor the ring overwrote reports the gap; one ahead of the ring (a
// reused boot id) says reset and refills from the oldest event.
void testGapAndReset() {
  reset();
  add(DEBUG_EVENT_CAPACITY + 20);
  CHECK(subscribe("{\"t\":\"logsub\",\"on\":true,\"boot\":42,\"after\":10}"));
  Page p = page();
  CHECK(p.built && p.miss == 10 && p.sequences.front() == 21);
  reset();
  add(5);
  CHECK(subscribe("{\"t\":\"logsub\",\"on\":true,\"boot\":42,\"after\":900}"));
  p = page();
  CHECK(p.built && p.reset && p.sequences.front() == 1 && p.sequences.back() == 5);
}

// A hold queues: nothing goes during a scaled shot, everything after it.
// Plain events wait for the companion's next frame; warnings go at once.
void testHoldsAndHeartbeat() {
  reset();
  add(3);
  CHECK(subscribe("{\"t\":\"logsub\",\"on\":true,\"boot\":42,\"after\":0}"));
  Page p = page();
  CHECK(p.built && p.sequences.size() == 3 && !p.more);
  add(2);
  network.companionNotified_.store(false);
  network.companionLogService();
  CHECK(!network.companionNotified_.load() && !page().built);  // Info waits for the heartbeat.
  outboundShotActive.store(true);
  outboundScaleConnected.store(true);
  add(1, "the scale link was lost", LogLevel::WARNING);
  network.companionLogService();
  CHECK(link.logCatchUp && !network.companionNotified_.load() && !page().built);
  outboundShotActive.store(false);
  network.companionLogService();
  CHECK(network.companionNotified_.load());
  p = page();
  CHECK(p.built && p.sequences.size() == 3 && p.sequences.front() == 4);
  add(1);
  CHECK(!page().built);
  g_companion.logHeard = true;
  p = page();
  CHECK(p.built && p.sequences.size() == 1);
  // S17: the flash-write gate's states (an unscaled shot, its drip, a rinse)
  // hold the log too, even for a warning; it all goes once the gate opens.
  network.flashGateClosed = true;
  add(1, "a rinse", LogLevel::WARNING);
  network.companionNotified_.store(false);
  network.companionLogService();
  CHECK(link.logCatchUp && !network.companionNotified_.load() && !page().built);
  network.flashGateClosed = false;
  network.companionLogService();
  CHECK(network.companionNotified_.load());
  p = page();
  CHECK(p.built && p.sequences.size() == 1);
  // The switch off sends nothing, whatever waits.
  add(1, "boom", LogLevel::ERROR);
  g_companion.stored.logSwitch = 0;
  network.companionNotified_.store(false);
  network.companionLogService();
  CHECK(!network.companionNotified_.load());
}

}  // namespace

int main() {
  testSubscriptionParsing();
  testPagingWithinTheFrame();
  testWorstCaseText();
  testGapAndReset();
  testHoldsAndHeartbeat();
  std::puts("Companion log producer: logsub, boot mapping, paging, text, gaps, reset, holds and heartbeat OK");
  return 0;
}
