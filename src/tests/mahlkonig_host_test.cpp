// Mahlkönig Grind-by-Sync: the shared golden vectors (docs/schemas/
// mahlkonig-gbs-v1.vectors.json and the `gbs` cases of the companion link
// vectors), the E64 WS state machine, the push client's decisions, malformed
// input, hold gating, the lock-free pull snapshot and the rule that grinder
// input never reaches an actuation path.
#include "../OpenBrewByWeightMahlkonig.h"

#include <cJSON.h>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>

using namespace openbrewbyweight;

#define CHECK(condition)                                                                 \
  do {                                                                                   \
    if (!(condition)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                      \
    }                                                                                    \
  } while (0)

namespace {

std::string readFile(const char *path) {
  FILE *file = std::fopen(path, "rb");
  CHECK(file != nullptr);
  std::string text;
  char chunk[4096];
  for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) text.append(chunk, n);
  std::fclose(file);
  return text;
}

const cJSON *item(const cJSON *object, const char *key) {
  const cJSON *found = cJSON_GetObjectItemCaseSensitive(object, key);
  CHECK(found != nullptr);
  return found;
}
std::string text(const cJSON *object, const char *key) { return item(object, key)->valuestring; }
uint32_t number(const cJSON *object, const char *key) {
  return static_cast<uint32_t>(item(object, key)->valuedouble);
}

void hexBytes(const std::string &hex, uint8_t *out) {
  for (size_t i = 0; i < hex.size() / 2; ++i)
    out[i] = static_cast<uint8_t>(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
}

GbsShotFacts factsOf(const cJSON *row) {
  GbsShotFacts facts;
  const std::string type = text(row, "shotType"), stop = text(row, "stopDetail"),
                    source = text(row, "actualWeightSource");
  for (uint8_t t = 0; t < 3; ++t)
    if (type == shotLogTypeName(static_cast<ShotLogType>(t))) facts.type = static_cast<ShotLogType>(t);
  for (unsigned d = 0; d <= 255; ++d)
    if (stop == shotLogStopDetailName(static_cast<ShotLogStopDetail>(d)))
      facts.stop = static_cast<ShotLogStopDetail>(d);
  for (uint8_t w = 0; w < ACTUAL_WEIGHT_SOURCE_COUNT; ++w)
    if (source == actualWeightSourceName(static_cast<ActualWeightSource>(w)))
      facts.source = static_cast<ActualWeightSource>(w);
  facts.channeling = cJSON_IsTrue(item(row, "channeling"));
  facts.bad = !cJSON_IsNull(item(row, "badReason"));
  facts.durationDs = static_cast<uint16_t>(std::lround(item(row, "durationS")->valuedouble * 10));
  const cJSON *g = item(row, "actualG");
  facts.yieldCg = cJSON_IsNull(g) ? SHOT_LOG_WEIGHT_MISSING
                                  : static_cast<int16_t>(std::lround(g->valuedouble * 100));
  return facts;
}

GbsRequest requestOf(const std::string &name) {
  for (uint8_t r = 0; r <= static_cast<uint8_t>(GbsRequest::BREW_DONE); ++r)
    if (name == gbsRequestName(static_cast<GbsRequest>(r))) return static_cast<GbsRequest>(r);
  CHECK(false);
  return GbsRequest::NONE;
}

GbsIdentity vectorIdentity(const cJSON *push) {
  const cJSON *id = item(push, "identity");
  GbsIdentity identity;
  std::snprintf(identity.brewDeviceId, sizeof(identity.brewDeviceId), "%s", text(id, "brewDeviceId").c_str());
  identity.pairingId = number(id, "pairingId");
  identity.paired = 1;
  return identity;
}

void testVectors() {
  const std::string source = readFile(GBS_VECTORS);
  cJSON *root = cJSON_Parse(source.c_str());
  CHECK(root != nullptr && number(root, "version") == 1);
  const cJSON *pull = item(root, "pull");
  char body[GBS_BODY_CAPACITY];
  const cJSON *machine = item(pull, "machine");
  CHECK(gbsMachineBody(body, sizeof(body), text(machine, "deviceId").c_str()) > 0);
  CHECK(text(machine, "body") == body);
  const cJSON *c = nullptr;
  cJSON_ArrayForEach(c, item(pull, "mako")) {
    GbsPull state;
    const std::string name = text(c, "state");
    for (uint8_t s = 0; s <= static_cast<uint8_t>(GbsPullState::ABORTED); ++s)
      if (name == gbsPullStateName(static_cast<GbsPullState>(s))) state.state = static_cast<GbsPullState>(s);
    state.armedAtMs = number(c, "armedAtMs");
    state.timeMs = number(c, "timeMs");
    state.volumeDg = number(c, "volumeDg");
    CHECK(gbsMakoBody(body, sizeof(body), state, number(c, "nowMs"), number(c, "unixS")) > 0);
    CHECK(text(c, "body") == body);
  }
  cJSON_ArrayForEach(c, item(pull, "numbers")) {
    const std::string b = text(c, "body");
    double value = 0;
    const bool found = gbsJsonNumber(b.data(), b.size(), text(c, "key").c_str(), value);
    const cJSON *expected = item(c, "value");
    CHECK(found == !cJSON_IsNull(expected));
    if (found) CHECK(value == expected->valuedouble);
    CHECK((found && value == 9.0 && text(c, "key") == "ID") == cJSON_IsTrue(item(c, "arms")));
  }
  GbsShotFacts flush;
  flush.durationDs = MIN_SHOT_LOG_DURATION_MS / 100U;
  CHECK(!gbsIsShot(flush));  // a flush after grinding leaves the grind for the next shot
  ++flush.durationDs;
  CHECK(gbsIsShot(flush));
  cJSON_ArrayForEach(c, item(root, "report")) {
    const cJSON *bad = item(item(c, "row"), "badReason");
    CHECK(gbsIsShot(factsOf(item(c, "row"))) == !(cJSON_IsString(bad) && std::string(bad->valuestring) == "short"));
    const GbsShotReport report = gbsReport(factsOf(item(c, "row")));
    CHECK(report.success == (text(c, "result") == "Success"));
    CHECK(report.timeMs == number(c, "durationMs"));
    CHECK(report.massMg == number(c, "massMg"));
    GbsPull armed;
    armed.recipeDg = 0;
    CHECK(gbsPullExecute(armed, 1000, 0));
    CHECK(gbsPullShot(armed, report, 2000, 40000, 0));
    char volume[16];
    std::snprintf(volume, sizeof(volume), "%lu.%lu", static_cast<unsigned long>(armed.volumeDg / 10),
                  static_cast<unsigned long>(armed.volumeDg % 10));
    CHECK(text(c, "volume") == volume);
    CHECK((armed.state == GbsPullState::FINISHING) == report.success);
  }
  cJSON_ArrayForEach(c, item(root, "attribution")) {
    const uint32_t base = 0xFFFF0000U;  // across the millis() wrap
    CHECK(gbsAttributable(base + number(c, "priorS") * 1000, base + number(c, "seenS") * 1000,
                          base + number(c, "startS") * 1000, base + number(c, "endS") * 1000) ==
          cJSON_IsTrue(item(c, "result")));
  }
  const cJSON *push = item(root, "push");
  const cJSON *uuid = item(push, "uuid");
  uint8_t random[16];
  hexBytes(text(uuid, "random"), random);
  char made[37];
  gbsUuidV4(random, made);
  CHECK(text(uuid, "text") == made);
  const GbsIdentity identity = vectorIdentity(push);
  cJSON_ArrayForEach(c, item(push, "requests")) {
    const GbsRequest request = requestOf(text(c, "request"));
    CHECK(text(c, "path") == gbsRequestPath(request));
    GbsPush state;
    state.applicationId = number(item(push, "identity"), "applicationId");
    if (request == GbsRequest::BREW_ACCEPT) {
      state.hasJob = true;
      std::snprintf(state.job.uuid, sizeof(state.job.uuid), "%s", text(c, "grindUuid").c_str());
      state.job.doseType = number(c, "doseType");
    } else if (request == GbsRequest::BREW_DONE) {
      state.result.due = true;
      std::snprintf(state.result.uuid, sizeof(state.result.uuid), "%s", text(c, "grindUuid").c_str());
      std::snprintf(state.result.eventUuid, sizeof(state.result.eventUuid), "%s",
                    text(c, "brewEventUuid").c_str());
      state.result.report.success = text(c, "result") == "Success";
      state.result.report.timeMs = number(c, "durationMs");
      state.result.report.massMg = number(c, "massMg");
    }
    CHECK(gbsRequestBody(body, sizeof(body), request, state, identity) > 0);
    CHECK(text(c, "body") == body);
  }
  cJSON_ArrayForEach(c, item(push, "responses")) {
    const std::string b = text(c, "body");
    const GbsResponse reply =
        gbsParseResponse(requestOf(text(c, "request")), static_cast<int>(number(c, "http")), b.data(), b.size());
    CHECK(text(c, "status") == gbsResponseStatusName(reply.status));
    if (cJSON_GetObjectItemCaseSensitive(c, "applicationId"))
      CHECK(reply.applicationId == number(c, "applicationId") && reply.pairingCode == number(c, "pairingCode"));
    if (cJSON_GetObjectItemCaseSensitive(c, "accepted")) CHECK(reply.accepted == cJSON_IsTrue(item(c, "accepted")));
    if (cJSON_GetObjectItemCaseSensitive(c, "pairingId")) CHECK(reply.pairingId == number(c, "pairingId"));
    if (const cJSON *job = cJSON_GetObjectItemCaseSensitive(c, "job")) {
      CHECK(reply.hasJob == !cJSON_IsNull(job));
      if (reply.hasJob)
        CHECK(text(job, "grindUuid") == reply.job.uuid && reply.job.doseType == number(job, "doseType") &&
              reply.job.durationMs == number(job, "durationMs") && reply.job.weightMg == number(job, "weightMg"));
    }
  }
  cJSON_Delete(root);

  // cfg.gbs and the companion's `gbs` frames (companion-link-v1.vectors.json).
  const std::string link = readFile(COMPANION_VECTORS);
  root = cJSON_Parse(link.c_str());
  CHECK(root != nullptr);
  const cJSON *gbs = item(root, "gbs");
  cJSON_ArrayForEach(c, item(gbs, "cfg")) {
    GbsIdentity id;
    uint8_t model = GBS_MODEL_NONE;
    for (uint8_t m = 0; m < GBS_MODEL_COUNT; ++m)
      if (text(c, "model") == gbsModelId(m)) model = m;
    std::snprintf(id.brewDeviceId, sizeof(id.brewDeviceId), "%s", text(c, "brewDeviceId").c_str());
    id.pairingId = number(c, "pairingId");
    id.paired = cJSON_IsTrue(item(c, "paired"));
    id.pinned = !text(c, "pin").empty();
    if (id.pinned) hexBytes(text(c, "pin"), id.pin);
    gbsCompanionConfig(body, sizeof(body), model, text(c, "host").c_str(), id);
    CHECK(text(c, "gbs") == body);
  }
  cJSON_ArrayForEach(c, item(gbs, "status")) {
    CHECK(static_cast<uint8_t>(gbsCompanionStateOf(text(c, "st").c_str())) == number(c, "state"));
  }
  cJSON_Delete(root);
}

// A2: every transition and exit of the E64 WS state machine.
void testPull() {
  GbsPull p;
  uint32_t polls = 0;
  CHECK(!gbsPullRelease(p, 10, polls));
  CHECK(gbsPullExecute(p, 1000, polls));
  CHECK(!gbsPullExecute(p, 1500, polls));  // the grinder's retries
  CHECK(p.state == GbsPullState::ARMED && p.armedAtMs == 1000);
  // A rinse while armed: brewing for its cycle, armed again once it left no shot.
  CHECK(gbsPullTick(p, 2000, polls, true) && p.state == GbsPullState::BREWING);
  CHECK(!gbsPullExecute(p, 2100, polls));
  CHECK(!gbsPullTick(p, 2000 + GBS_RESULT_CAP_MS - 1, polls, false));
  CHECK(gbsPullTick(p, 2000 + GBS_RESULT_CAP_MS, polls, false) && p.state == GbsPullState::ARMED);
  // The window closes 180 s after execute with an abort.
  CHECK(!gbsPullTick(p, 1000 + GBS_PULL_WINDOW_MS - 1, polls, false));
  CHECK(gbsPullTick(p, 1000 + GBS_PULL_WINDOW_MS, polls, false) && p.state == GbsPullState::ABORTED);
  CHECK(p.timeMs == GBS_PULL_WINDOW_MS && p.volumeDg == 0);
  // Status 4 holds 6 s and one poll, at most 60 s.
  const uint32_t at = 1000 + GBS_PULL_WINDOW_MS;
  CHECK(!gbsPullTick(p, at + GBS_RESULT_HOLD_MS, polls, false));
  ++polls;
  CHECK(!gbsPullTick(p, at + GBS_RESULT_HOLD_MS - 1, polls, false));
  CHECK(gbsPullTick(p, at + GBS_RESULT_HOLD_MS, polls, false) && p.state == GbsPullState::READY);
  CHECK(gbsPullExecute(p, 500000, polls) && gbsPullTick(p, 500100, polls, true));
  CHECK(gbsPullTick(p, 500000 + 60000 + GBS_RESULT_CAP_MS, polls, false));
  CHECK(p.state == GbsPullState::ARMED);
  p = GbsPull{};
  CHECK(gbsPullExecute(p, 600000, polls));
  CHECK(gbsPullTick(p, 600000 + GBS_PULL_WINDOW_MS, polls, false));
  CHECK(gbsPullTick(p, 600000 + GBS_PULL_WINDOW_MS + GBS_RESULT_CAP_MS, polls, false));  // no poll at all
  CHECK(p.state == GbsPullState::READY);

  // The measured shot, a shot started before execute, Release.
  GbsShotFacts facts;
  facts.stop = ShotLogStopDetail::NORMAL_TARGET;
  facts.source = ActualWeightSource::POST_DRIP;
  facts.bad = false;
  facts.durationDs = 284;
  facts.yieldCg = 3621;
  const GbsShotReport good = gbsReport(facts);
  p = GbsPull{};
  CHECK(gbsPullExecute(p, 10000, polls));
  CHECK(!gbsPullShot(p, good, 9000, 40000, polls) && p.state == GbsPullState::ARMED);
  CHECK(gbsPullShot(p, good, 12000, 45000, polls) && p.state == GbsPullState::FINISHING);
  CHECK(p.timeMs == 28400 && p.volumeDg == 362);
  CHECK(!gbsPullShot(p, good, 50000, 80000, polls));  // already settled
  CHECK(gbsPullExecute(p, 46000, polls) && p.state == GbsPullState::ARMED);  // a new grind in status 4
  CHECK(gbsPullRelease(p, 56000, polls) && p.state == GbsPullState::ABORTED && p.timeMs == 10000);
  // No weight falls back to the recipe weight; neither refuses.
  GbsShotReport light = good;
  light.massMg = 0;
  p = GbsPull{};
  p.recipeDg = 365;
  CHECK(gbsPullExecute(p, 1000, polls) && gbsPullShot(p, light, 2000, 30000, polls));
  CHECK(p.state == GbsPullState::FINISHING && p.volumeDg == 365);
  p = GbsPull{};
  CHECK(gbsPullExecute(p, 1000, polls) && gbsPullShot(p, light, 2000, 30000, polls));
  CHECK(p.state == GbsPullState::ABORTED);
  // Every disqualifier is an abort: hand stop, channeling, scale lost, a short shot.
  for (const auto change : {+[](GbsShotFacts &f) { f.stop = ShotLogStopDetail::WEB_STOP; },
                            +[](GbsShotFacts &f) { f.channeling = true; },
                            +[](GbsShotFacts &f) { f.stop = ShotLogStopDetail::AUTO_TO_MANUAL; },
                            +[](GbsShotFacts &f) { f.source = ActualWeightSource::LAST_KNOWN; },
                            +[](GbsShotFacts &f) { f.bad = true; }}) {
    GbsShotFacts bad = facts;
    change(bad);
    p = GbsPull{};
    CHECK(gbsPullExecute(p, 1000, polls) && gbsPullShot(p, gbsReport(bad), 2000, 30000, polls));
    CHECK(p.state == GbsPullState::ABORTED && p.timeMs == 28400 && p.volumeDg == 0);
  }
  // A record rebuilt from its stored fields gives the same facts.
  ShotLogRecord record{};
  record.stopDetail = static_cast<uint8_t>(ShotLogStopDetail::SLOW_MAX_TIME);
  record.actualWeightSource = static_cast<uint8_t>(ActualWeightSource::POST_DRIP);
  record.durationDs = 380;
  record.actualWeightCg = 3140;
  record.verdict = corridor::packVerdict(corridor::Verdict::SLOW, false, corridor::LossSource::PROJECTION, false);
  const GbsShotReport stored = gbsReport(gbsShotFactsOf(record));
  CHECK(stored.success && stored.timeMs == 38000 && stored.massMg == 31400);
  record.verdict = corridor::packVerdict(corridor::Verdict::SLOW, true, corridor::LossSource::PROJECTION, false);
  CHECK(!gbsReport(gbsShotFactsOf(record)).success);
}

GbsResponse reply(GbsRequest request, int http, const char *body) {
  return gbsParseResponse(request, http, body, std::strlen(body));
}

void deliver(GbsPush &push, GbsIdentity &id, GbsRequest request, int http, const char *body, uint32_t now) {
  (void)gbsPushReply(push, id, request, true, false, reply(request, http, body), now);
}

constexpr uint32_t kPoll = GBS_CADENCE_DEFAULT_S * 1000U;
const uint8_t kRandom[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr const char *kJob = "{\"brewJobs\":[{\"grindUuid\":\"job-1\",\"doseTypeExpected\":2}],\"status\":\"Ok\"}";
constexpr const char *kJob2 = "{\"brewJobs\":[{\"grindUuid\":\"job-2\",\"doseTypeExpected\":1}],\"status\":\"Ok\"}";
constexpr const char *kNoJob = "{\"brewJobs\":[],\"status\":\"Ok\"}";
constexpr const char *kOk = "{\"status\":\"Ok\"}";

void testPushPairing() {
  GbsPush push;
  GbsIdentity id;
  std::snprintf(id.brewDeviceId, sizeof(id.brewDeviceId), "3f2b8c1e-5d4a-4e6f-9a7b-1c2d3e4f5a6b");
  CHECK(gbsPushNext(push, id, false, true, 0) == GbsRequest::NONE);  // never paired
  gbsPushPair(push, 1000);
  CHECK(gbsPushNext(push, id, true, true, 1000) == GbsRequest::NONE);  // not during a hold
  CHECK(gbsPushNext(push, id, false, false, 1000) == GbsRequest::PAIR_APPLY);  // pairing is the controller's
  deliver(push, id, GbsRequest::PAIR_APPLY, 200, "{\"status\":\"Ok\",\"applicationId\":12,\"pairingCode\":73}", 1100);
  CHECK(push.phase == GbsPairPhase::ACCEPTING && push.pairingCode == 73);
  CHECK(gbsPushNext(push, id, false, true, 1100) == GbsRequest::NONE);
  CHECK(gbsPushNext(push, id, false, true, 1100 + GBS_PAIR_STATUS_MS) == GbsRequest::PAIR_STATUS);
  deliver(push, id, GbsRequest::PAIR_STATUS, 200, "{\"accepted\":false}", 6200);
  // The grinder freezes and restarts after accepting: failures are tolerated.
  for (uint32_t t = 11300; t < 1100 + 90000; t += GBS_PAIR_STATUS_MS) {
    CHECK(!gbsPushReply(push, id, GbsRequest::PAIR_STATUS, false, false, GbsResponse{}, t));
    CHECK(push.phase == GbsPairPhase::ACCEPTING);
  }
  // A grinder that forgot the application: apply again.
  deliver(push, id, GbsRequest::PAIR_STATUS, 200, "{\"status\":\"ClientErrorPairingAppIdNotFound\"}", 95000);
  CHECK(push.phase == GbsPairPhase::APPLYING && gbsPushNext(push, id, false, true, 95000) == GbsRequest::PAIR_APPLY);
  deliver(push, id, GbsRequest::PAIR_APPLY, 200, "{\"status\":\"Ok\",\"applicationId\":13,\"pairingCode\":5}", 95100);
  CHECK(gbsPushReply(push, id, GbsRequest::PAIR_STATUS, true, false,
                     reply(GbsRequest::PAIR_STATUS, 200, "{\"accepted\":true,\"pairingId\":4821}"), 100200));
  CHECK(id.paired && id.pairingId == 4821 && push.phase == GbsPairPhase::IDLE);
  CHECK(gbsPushNext(push, id, false, true, 100200) == GbsRequest::BREW_JOBS);

  // Already paired: the person types the Pairing ID.
  GbsPush other;
  GbsIdentity fresh = id;
  fresh.paired = 0;
  gbsPushPair(other, 0);
  deliver(other, fresh, GbsRequest::PAIR_APPLY, 200, "{\"status\":\"ClientErrorDevAlreadyPaired\"}", 100);
  CHECK(other.phase == GbsPairPhase::NEED_ID && gbsPushNext(other, fresh, false, true, 100) == GbsRequest::NONE);
  // Not accepted within 3 minutes; an unreachable address fails at once.
  gbsPushPair(other, 0);
  deliver(other, fresh, GbsRequest::PAIR_APPLY, 200, "{\"status\":\"Ok\",\"applicationId\":1,\"pairingCode\":2}", 0);
  deliver(other, fresh, GbsRequest::PAIR_STATUS, 200, "{\"accepted\":false}", GBS_PAIR_LIMIT_MS);
  CHECK(other.phase == GbsPairPhase::FAILED);
  gbsPushPair(other, 0);
  CHECK(!gbsPushReply(other, fresh, GbsRequest::PAIR_APPLY, false, false, GbsResponse{}, 10));
  CHECK(other.phase == GbsPairPhase::FAILED);
}

void testPushJobs() {
  GbsIdentity id;
  std::snprintf(id.brewDeviceId, sizeof(id.brewDeviceId), "3f2b8c1e-5d4a-4e6f-9a7b-1c2d3e4f5a6b");
  id.paired = 1;
  id.pairingId = 4821;
  GbsPush push;
  // 25 s cadence; through the companion the controller asks nothing.
  CHECK(gbsPushNext(push, id, false, false, 0) == GbsRequest::NONE);
  CHECK(gbsPushNext(push, id, false, true, 0) == GbsRequest::BREW_JOBS);
  deliver(push, id, GbsRequest::BREW_JOBS, 200, kNoJob, 0);
  CHECK(gbsPushNext(push, id, false, true, kPoll - 1) == GbsRequest::NONE);
  CHECK(gbsPushNext(push, id, false, true, kPoll) == GbsRequest::BREW_JOBS);
  // Back-off 25, 60, 120 s while unreachable; one answer restores 25 s.
  uint32_t now = kPoll;
  for (const uint32_t wait : {kPoll, 60000U, 120000U, 120000U}) {
    CHECK(!gbsPushReply(push, id, GbsRequest::BREW_JOBS, false, false, GbsResponse{}, now));
    CHECK(push.link == GbsLink::UNREACHABLE && push.dueAtMs == now + wait);
    now += wait;
  }
  deliver(push, id, GbsRequest::BREW_JOBS, 200, kNoJob, now);
  CHECK(push.link == GbsLink::OK && push.dueAtMs == now + kPoll && push.failures == 0);
  // A job seen: take it, then a shot after it is delivered after the hold.
  now += kPoll;
  deliver(push, id, GbsRequest::BREW_JOBS, 200, kJob, now);
  CHECK(push.hasJob && !push.jobTaken && push.jobSeenAtMs == now);
  CHECK(gbsPushNext(push, id, true, true, now) == GbsRequest::NONE);
  CHECK(gbsPushNext(push, id, false, true, now) == GbsRequest::BREW_ACCEPT);
  deliver(push, id, GbsRequest::BREW_ACCEPT, 200, kOk, now + 500);
  CHECK(push.jobTaken);
  // Handed out again: taken again.
  deliver(push, id, GbsRequest::BREW_JOBS, 200, kJob, now + 50000);
  CHECK(push.hasJob && !push.jobTaken && push.jobSeenAtMs == now);
  deliver(push, id, GbsRequest::BREW_ACCEPT, 200, kOk, now + 50500);
  GbsShotFacts facts;
  facts.stop = ShotLogStopDetail::NORMAL_TARGET;
  facts.source = ActualWeightSource::POST_DRIP;
  facts.bad = false;
  facts.durationDs = 284;
  facts.yieldCg = 3621;
  const GbsShotReport good = gbsReport(facts);
  CHECK(!gbsPushShot(push, good, now - 30000, now - 2000, kRandom, now + 60000));  // started before the poll that lacked the job
  CHECK(gbsPushShot(push, good, now + 52000, now + 80400, kRandom, now + 95000));
  CHECK(push.result.due && !push.result.retake && !push.hasJob);
  // A hold keeps it; its end sends the result first, then one poll.
  CHECK(gbsPushNext(push, id, true, true, now + 95000) == GbsRequest::NONE);
  CHECK(gbsPushNext(push, id, false, true, now + 95000) == GbsRequest::BREW_DONE);
  char body[GBS_BODY_CAPACITY];
  CHECK(gbsRequestBody(body, sizeof(body), GbsRequest::BREW_DONE, push, id) > 0);
  CHECK(std::strstr(body, "\"durationFinalMs\":28400,\"massFinalMg\":36210") != nullptr);
  // The grinder dropped the job: take it again, then send again.
  deliver(push, id, GbsRequest::BREW_DONE, 200, "{\"status\":\"ClientErrorOngoingJobNotExisting\"}", now + 95500);
  CHECK(push.result.due && push.result.retake);
  CHECK(gbsPushNext(push, id, false, true, now + 95500 + GBS_RETRY_MS - 1) != GbsRequest::BREW_ACCEPT);
  CHECK(gbsPushNext(push, id, false, true, now + 95500 + GBS_RETRY_MS) == GbsRequest::BREW_ACCEPT);
  CHECK(gbsRequestBody(body, sizeof(body), GbsRequest::BREW_ACCEPT, push, id) > 0);
  CHECK(std::strstr(body, "\"grindUuid\":\"job-1\"") != nullptr);
  deliver(push, id, GbsRequest::BREW_ACCEPT, 200, kOk, now + 101000);
  CHECK(gbsPushNext(push, id, false, true, now + 101000) == GbsRequest::BREW_DONE);
  deliver(push, id, GbsRequest::BREW_DONE, 200, kOk, now + 101500);
  CHECK(!push.result.due && gbsPushNext(push, id, false, true, now + 101500) == GbsRequest::BREW_JOBS);
  // The job reappearing while its result waits is not a new job.
  now += 200000;
  deliver(push, id, GbsRequest::BREW_JOBS, 200, kJob2, now);
  CHECK(gbsPushShot(push, good, now + 1000, now + 30000, kRandom, now + 40000));
  deliver(push, id, GbsRequest::BREW_JOBS, 200, kJob2, now + 41000);
  CHECK(!push.hasJob && push.result.due);
  // Result retries end after 120 s (20 s for a cancellation).
  CHECK(!gbsPushReply(push, id, GbsRequest::BREW_DONE, false, false, GbsResponse{}, now + 40000 + GBS_RESULT_RETRY_MS - 1));
  CHECK(push.result.due && push.result.retryAtMs == now + 40000 + GBS_RESULT_RETRY_MS - 1 + GBS_RETRY_MS);
  CHECK(!gbsPushReply(push, id, GbsRequest::BREW_DONE, false, false, GbsResponse{}, now + 40000 + GBS_RESULT_RETRY_MS));
  CHECK(!push.result.due);
  // A newer job supersedes the one in hand; a taken job with no shot is cancelled.
  now += 300000;
  deliver(push, id, GbsRequest::BREW_JOBS, 200, kJob, now);
  deliver(push, id, GbsRequest::BREW_JOBS, 200, kJob2, now + kPoll);
  CHECK(push.hasJob && std::strcmp(push.job.uuid, "job-2") == 0 && push.jobSeenAtMs == now + kPoll);
  CHECK(!gbsPushExpire(push, kRandom, now + kPoll + GBS_JOB_WINDOW_MS - 1));
  CHECK(gbsPushExpire(push, kRandom, now + kPoll + GBS_JOB_WINDOW_MS));
  CHECK(push.result.due && !push.result.report.success && push.result.retake);
  CHECK(!gbsPushReply(push, id, GbsRequest::BREW_DONE, false, false, GbsResponse{},
                      now + kPoll + GBS_JOB_WINDOW_MS + GBS_CANCEL_RETRY_MS));
  CHECK(!push.result.due);

  // 25 s attribution: a job first seen after its shot (absent before it).
  GbsPush late;
  deliver(late, id, GbsRequest::BREW_JOBS, 200, kNoJob, 1000000);
  deliver(late, id, GbsRequest::BREW_JOBS, 200, kJob, 1000000 + kPoll);
  CHECK(late.jobPriorAtMs == 1000000);
  CHECK(gbsPushShot(late, good, 1005000, 1020000, kRandom, 1000000 + kPoll));
  CHECK(late.result.retake);  // never taken: take it, then deliver
  GbsPush old;
  deliver(old, id, GbsRequest::BREW_JOBS, 200, kNoJob, 0);
  deliver(old, id, GbsRequest::BREW_JOBS, 200, kJob, 100000);
  CHECK(!gbsPushShot(old, good, 5000, 39000, kRandom, 100000));  // ended 61 s before

  // 403 for 60 s means unpaired (shorter refusals are a grinder booting).
  GbsPush refused;
  deliver(refused, id, GbsRequest::BREW_JOBS, 403, "", 0);
  deliver(refused, id, GbsRequest::BREW_JOBS, 403, "", 25000);
  CHECK(refused.link != GbsLink::UNPAIRED);
  deliver(refused, id, GbsRequest::BREW_JOBS, 200, kNoJob, 50000);
  deliver(refused, id, GbsRequest::BREW_JOBS, 403, "", 75000);
  deliver(refused, id, GbsRequest::BREW_JOBS, 403, "", 75000 + GBS_UNPAIRED_MS - 1);
  CHECK(refused.link != GbsLink::UNPAIRED);
  deliver(refused, id, GbsRequest::BREW_JOBS, 403, "", 75000 + GBS_UNPAIRED_MS);
  CHECK(refused.link == GbsLink::UNPAIRED && gbsPushNext(refused, id, false, true, 200000) == GbsRequest::NONE);

  // The certificate pin: captured unpinned, then compared; a mismatch stops
  // every request until the person trusts the grinder again.
  uint8_t seen[32] = {1}, other[32] = {2};
  GbsIdentity pinned = id;
  CHECK(gbsPinAccepts(pinned, seen));
  std::memcpy(pinned.pin, seen, sizeof(seen));
  pinned.pinned = 1;
  CHECK(gbsPinAccepts(pinned, seen) && !gbsPinAccepts(pinned, other));
  GbsPush identity;
  CHECK(!gbsPushReply(identity, pinned, GbsRequest::BREW_JOBS, false, true, GbsResponse{}, 0));
  CHECK(identity.link == GbsLink::IDENTITY && gbsPushNext(identity, pinned, false, true, 99999) == GbsRequest::NONE);
  pinned.pinned = 0;  // Trust this grinder: the pairing is kept
  identity.link = GbsLink::UNKNOWN;
  CHECK(pinned.paired && gbsPushNext(identity, pinned, false, true, 99999) == GbsRequest::BREW_JOBS);
  char config[GBS_BODY_CAPACITY];
  CHECK(gbsCompanionConfig(config, sizeof(config), GBS_MODEL_E65W, "192.168.1.87", pinned) == 0);
}

// The glue's order after a hold (S3): the shot's record lands, then the
// requests gbsPushNext hands out one by one; a job first seen at the poll
// right after the shot is matched there (mahlkonigFinishRequest).
struct PostHold {
  GbsIdentity id;
  GbsPush push;
  GbsShotReport report;
  uint32_t start = 0, end = 0;
  bool unclaimed = false;
  PostHold() {
    std::snprintf(id.brewDeviceId, sizeof(id.brewDeviceId), "3f2b8c1e-5d4a-4e6f-9a7b-1c2d3e4f5a6b");
    id.paired = 1;
    id.pairingId = 4821;
    report.success = true;
    report.timeMs = 28400;
    report.massMg = 36210;
  }
  void shot(uint32_t startMs, uint32_t endMs, uint32_t now) {
    start = startMs, end = endMs;
    unclaimed = !gbsPushShotEnded(push, report, start, end, kRandom, now);
  }
  GbsRequest next(uint32_t now, bool hold = false) const { return gbsPushNext(push, id, hold, true, now); }
  void answer(GbsRequest request, const char *body, uint32_t now) {
    deliver(push, id, request, 200, body, now);
    if (request == GbsRequest::BREW_JOBS && unclaimed && gbsPushShot(push, report, start, end, kRandom, now))
      unclaimed = false;
  }
};

// A grinder that keeps refusing or dropping a job never makes the client loop:
// a failed take waits for the next poll, a job the grinder no longer has ends
// its result, and OngoingJobNotExisting is retried at the result's pace and
// within its limit.
void testNoRetryLoop() {
  GbsIdentity id;
  id.paired = 1;
  id.pairingId = 4821;
  const char *gone = "{\"status\":\"ClientErrorOngoingJobNotExisting\"}";
  GbsPush push;
  deliver(push, id, GbsRequest::BREW_JOBS, 200, kJob, 1000);
  CHECK(gbsPushNext(push, id, false, true, 1000) == GbsRequest::BREW_ACCEPT);
  CHECK(!gbsPushReply(push, id, GbsRequest::BREW_ACCEPT, false, false, GbsResponse{}, 1100));
  CHECK(push.hasJob && gbsPushNext(push, id, false, true, 1100) == GbsRequest::NONE);
  CHECK(gbsPushNext(push, id, false, true, 1100 + kPoll) == GbsRequest::BREW_JOBS);
  deliver(push, id, GbsRequest::BREW_JOBS, 200, kJob, 1100 + kPoll);
  CHECK(gbsPushNext(push, id, false, true, 1100 + kPoll) == GbsRequest::BREW_ACCEPT);
  // A take refused while another result waits drops the job.
  push.result.due = true;
  push.result.retryAtMs = 999999;
  deliver(push, id, GbsRequest::BREW_ACCEPT, 200, gone, 30000);
  CHECK(!push.hasJob && gbsPushNext(push, id, false, true, 30000) != GbsRequest::BREW_ACCEPT);
  // A retake the grinder refuses ends the result; one poll follows.
  GbsPush late;
  deliver(late, id, GbsRequest::BREW_JOBS, 200, kJob, 0);
  GbsShotReport good{true, 28400, 36210};
  CHECK(gbsPushShot(late, good, 5000, 33400, kRandom, 40000) && late.result.retake);
  CHECK(gbsPushNext(late, id, false, true, 40000) == GbsRequest::BREW_ACCEPT);
  deliver(late, id, GbsRequest::BREW_ACCEPT, 200, gone, 40500);
  CHECK(!late.result.due && gbsPushNext(late, id, false, true, 40500) == GbsRequest::BREW_JOBS);
  // OngoingJobNotExisting after every take: paced, and over after 120 s.
  GbsPush stuck;
  deliver(stuck, id, GbsRequest::BREW_JOBS, 200, kJob, 0);
  deliver(stuck, id, GbsRequest::BREW_ACCEPT, 200, kOk, 100);
  CHECK(gbsPushShot(stuck, good, 5000, 33400, kRandom, 40000) && !stuck.result.retake);
  uint32_t now = 40000, requests = 0;
  while (stuck.result.due && requests < 1000) {
    const GbsRequest next = gbsPushNext(stuck, id, false, true, now);
    if (next == GbsRequest::BREW_DONE || next == GbsRequest::BREW_ACCEPT) {
      ++requests;
      deliver(stuck, id, next, 200, next == GbsRequest::BREW_DONE ? gone : kOk, now);
    }
    now += 500;
  }
  CHECK(!stuck.result.due && now - 40000 <= GBS_RESULT_RETRY_MS + GBS_RETRY_MS + 500);
  CHECK(requests <= 2 * (GBS_RESULT_RETRY_MS / GBS_RETRY_MS + 1));
}

// D12: the direct cadence is 15, 20 (default) or 25 s; the first retry
// waits one cadence, then 60 and 120 s; the post-shot poll ignores it.
void testCadence() {
  CHECK(RuntimeConfig{}.mahlkonigPushCadenceS == 20 && GbsPush{}.pollMs == 20000);
  for (const uint32_t s : {15U, 20U, 25U}) {
    GbsIdentity id;
    id.paired = 1;
    GbsPush push;
    push.pollMs = s * 1000U;
    deliver(push, id, GbsRequest::BREW_JOBS, 200, kNoJob, 1000);
    CHECK(gbsPushNext(push, id, false, true, 1000 + s * 1000U - 1) == GbsRequest::NONE);
    CHECK(gbsPushNext(push, id, false, true, 1000 + s * 1000U) == GbsRequest::BREW_JOBS);
    CHECK(!gbsPushReply(push, id, GbsRequest::BREW_JOBS, false, false, GbsResponse{}, 50000));
    CHECK(push.dueAtMs == 50000 + s * 1000U);
    CHECK(!gbsPushReply(push, id, GbsRequest::BREW_JOBS, false, false, GbsResponse{}, 90000));
    CHECK(push.dueAtMs == 90000 + 60000);
    deliver(push, id, GbsRequest::BREW_JOBS, 200, kNoJob, 200000);
    GbsShotReport report;
    CHECK(!gbsPushShotEnded(push, report, 210000, 238000, kRandom, 240000));
    CHECK(gbsPushNext(push, id, false, true, 240000) == GbsRequest::BREW_JOBS);  // not at 200 s + cadence
    gbsPushPair(push, 300000);
    CHECK(push.pollMs == s * 1000U);  // pairing keeps the chosen cadence
  }
}

void testPostHold() {
  // A job ground just before the shot, first seen at the post-hold poll: synced.
  PostHold a;
  a.answer(GbsRequest::BREW_JOBS, kNoJob, 100000);  // the last poll before the shot
  a.shot(110000, 138000, 150000);                    // its record, after the drip
  CHECK(a.unclaimed && a.next(150000) == GbsRequest::BREW_JOBS);  // at once, not at 125 s
  a.answer(GbsRequest::BREW_JOBS, kJob, 150500);
  CHECK(!a.unclaimed && a.push.result.due && a.push.result.retake);
  CHECK(a.push.dueAtMs == 150500 + kPoll);  // the cadence restarts from that poll
  CHECK(a.next(150500) == GbsRequest::BREW_ACCEPT);
  a.answer(GbsRequest::BREW_ACCEPT, kOk, 151000);
  CHECK(a.next(151000) == GbsRequest::BREW_DONE);
  a.answer(GbsRequest::BREW_DONE, kOk, 151500);
  CHECK(!a.push.result.due && a.next(151500) == GbsRequest::BREW_JOBS);
  a.answer(GbsRequest::BREW_JOBS, kNoJob, 152000);
  CHECK(a.next(152000) == GbsRequest::NONE && a.next(152000 + kPoll) == GbsRequest::BREW_JOBS);
  // A job seen before the shot: its result goes first after the hold.
  PostHold b;
  b.answer(GbsRequest::BREW_JOBS, kNoJob, 0);
  b.answer(GbsRequest::BREW_JOBS, kJob, 25000);
  b.answer(GbsRequest::BREW_ACCEPT, kOk, 25500);
  b.shot(40000, 68000, 80000);
  CHECK(!b.unclaimed && b.next(80000) == GbsRequest::BREW_DONE);
  b.answer(GbsRequest::BREW_DONE, kOk, 80500);
  CHECK(b.next(80500) == GbsRequest::BREW_JOBS);
  // A job first seen more than 60 s after its shot ended is not that shot's.
  PostHold c;
  c.answer(GbsRequest::BREW_JOBS, kNoJob, 0);
  c.shot(10000, 38000, 50000);
  c.answer(GbsRequest::BREW_JOBS, kNoJob, 50500);  // the post-hold poll: nothing yet
  c.answer(GbsRequest::BREW_JOBS, kJob, 98001);    // 60.001 s after the shot ended
  CHECK(c.unclaimed && !c.push.result.due && c.push.hasJob && !c.push.jobTaken);
  // The grinder already dropped the job: the shot is simply not synced.
  PostHold d;
  d.answer(GbsRequest::BREW_JOBS, kNoJob, 0);
  d.shot(10000, 90000, 100000);
  d.answer(GbsRequest::BREW_JOBS, kNoJob, 100500);
  CHECK(!d.push.result.due && !d.push.hasJob && d.push.link == GbsLink::OK && d.push.failures == 0);
  // Another hold during the sequence: nothing goes until it ends, then the
  // same request resumes (the glue tears down the one in flight).
  PostHold e;
  e.answer(GbsRequest::BREW_JOBS, kNoJob, 0);
  e.answer(GbsRequest::BREW_JOBS, kJob, 25000);
  e.answer(GbsRequest::BREW_ACCEPT, kOk, 25500);
  e.shot(40000, 68000, 80000);
  CHECK(e.next(80000, true) == GbsRequest::NONE && e.next(95000, true) == GbsRequest::NONE);
  CHECK(e.next(120000) == GbsRequest::BREW_DONE && e.push.result.due);
}

// No input sequence makes the client transmit during a hold, and no body
// ever carries a byte the grinder or a JSON reader could misread.
void testProperties() {
  std::mt19937 rng(20261122);
  GbsIdentity id;
  std::snprintf(id.brewDeviceId, sizeof(id.brewDeviceId), "3f2b8c1e-5d4a-4e6f-9a7b-1c2d3e4f5a6b");
  id.paired = 1;
  GbsPush push;
  GbsPull pull;
  const char *bodies[] = {kJob, kJob2, kNoJob, kOk, "", "{", "{\"status\":\"ClientErrorOngoingJobNotExisting\"}",
                          "{\"accepted\":true,\"pairingId\":7}", "{\"status\":\"Ok\",\"applicationId\":1,\"pairingCode\":2}"};
  uint32_t now = 0, polls = 0;
  for (int step = 0; step < 200000; ++step) {
    now += rng() % 7000;
    const GbsRequest request = static_cast<GbsRequest>(1 + rng() % 5);
    const char *b = bodies[rng() % (sizeof(bodies) / sizeof(bodies[0]))];
    switch (rng() % 8) {
      case 0: gbsPushPair(push, now); break;
      case 1: (void)gbsPushShot(push, GbsShotReport{(rng() & 1) != 0, rng() % 90000, rng() % 60000}, now - rng() % 90000,
                               now - rng() % 30000, kRandom, now);
        break;
      case 2: (void)gbsPushExpire(push, kRandom, now); break;
      default:
        (void)gbsPushReply(push, id, request, (rng() & 3) != 0, (rng() % 50) == 0,
                           reply(request, (rng() % 9) == 0 ? 403 : 200, b), now);
    }
    CHECK(gbsPushNext(push, id, true, (rng() & 1) != 0, now) == GbsRequest::NONE);
    const GbsRequest next = gbsPushNext(push, id, false, true, now);
    char body[GBS_BODY_CAPACITY];
    const size_t n = next == GbsRequest::NONE ? 0 : gbsRequestBody(body, sizeof(body), next, push, id);
    for (size_t i = 0; i < n; ++i) CHECK(body[i] >= 0x20 && body[i] != '\\');
    if (n) {
      cJSON *parsed = cJSON_Parse(body);
      CHECK(cJSON_IsObject(parsed));
      cJSON_Delete(parsed);
    }
    if (rng() % 3 == 0) ++polls;
    switch (rng() % 6) {
      case 0: (void)gbsPullExecute(pull, now, polls); break;
      case 1: (void)gbsPullRelease(pull, now, polls); break;
      case 2: (void)gbsPullShot(pull, GbsShotReport{(rng() & 1) != 0, rng() % 90000, rng() % 60000},
                               now - rng() % 200000, now, polls);
        break;
      default: (void)gbsPullTick(pull, now, polls, (rng() & 1) != 0);
    }
    CHECK(gbsMakoBody(body, sizeof(body), pull, now, now / 1000) > 0);
  }
}

// Malformed and oversized bodies never crash the parsers (run under ASan).
void testFuzz() {
  std::mt19937 rng(7);
  const std::string alphabet = "{}[]\":,.-+eE0123456789 \\tnulrsfaID_\x01\xff";
  for (int round = 0; round < 50000; ++round) {
    std::string body;
    const size_t length = rng() % (round % 100 == 0 ? 4096 : 64);
    for (size_t i = 0; i < length; ++i) body.push_back(alphabet[rng() % alphabet.size()]);
    double value = 0;
    (void)gbsJsonNumber(body.data(), body.size(), "ID", value);
    (void)gbsJsonNumber(body.data(), body.size(), "SYNC_BEVERAGE_WEIGHT", value);
    for (uint8_t r = 1; r <= 5; ++r)
      (void)gbsParseResponse(static_cast<GbsRequest>(r), 200, body.data(), body.size());
  }
  // A body cut at every length.
  const std::string whole = "{\"brewJobs\":[{\"grindUuid\":\"x\",\"groupHeadId\":1}],\"status\":\"Ok\"}";
  for (size_t n = 0; n < whole.size(); ++n) {
    const GbsResponse cut = gbsParseResponse(GbsRequest::BREW_JOBS, 200, whole.data(), n);
    CHECK(cut.status == GbsResponse::MALFORMED);
    double value = 0;
    CHECK(!gbsJsonNumber("{\"ID\":9}", n < 8 ? n : 7, "ID", value));
  }
  CHECK(!gbsHostValid("") && !gbsHostValid("a b") && !gbsHostValid("x/y") && !gbsHostValid("-x"));
  CHECK(gbsHostValid("192.168.1.87") && gbsHostValid("grinder.local"));
  CHECK(!gbsHostValid(std::string(GBS_HOST_CAPACITY, 'a').c_str()));
  CHECK(!gbsUuidValid("a\"b") && !gbsUuidValid(std::string(GBS_UUID_TEXT, 'a').c_str()));
}

// S4: httpd copies the answer while the network task republishes it.
void testSnapshot() {
  GbsSnapshot snapshot;
  char out[GBS_BODY_CAPACITY];
  CHECK(snapshot.read(out, sizeof(out)) == 0);
  const std::string a(200, 'a'), b(301, 'b');
  snapshot.publish(a.data(), a.size());
  std::thread writer([&] {
    for (int i = 0; i < 20000; ++i) {
      const std::string &next = i & 1 ? a : b;
      snapshot.publish(next.data(), next.size());
    }
  });
  for (int i = 0; i < 20000; ++i) {
    const size_t n = snapshot.read(out, sizeof(out));
    if (n == 0) continue;
    CHECK(n == a.size() || n == b.size());
    CHECK(std::string(out, n) == (n == a.size() ? a : b));
  }
  writer.join();
  CHECK(snapshot.read(out, 10) == 0);  // never past the reader's buffer
}

// Grinder input is a signal only: the GbS sources name no actuation path.
void testNoActuation() {
  for (const char *path : {GBS_HEADER, GBS_SERVICE, GBS_HTTP}) {
    const std::string source = readFile(path);
    for (const char *forbidden : {"Relay", "relay(", "enqueueWebCommand", "WebCommand", "machineIntegrationCommand",
                                  "machineIntegrationPower", "paddle", "Paddle", "forcePulse", "requestSafeRestart",
                                  "submitCommand"})
      CHECK(source.find(forbidden) == std::string::npos);
  }
}

}  // namespace

int main() {
  testVectors();
  testPull();
  testPushPairing();
  testPushJobs();
  testPostHold();
  testNoRetryLoop();
  testCadence();
  testProperties();
  testFuzz();
  testSnapshot();
  testNoActuation();
  std::puts("mahlkonig_host_test: ok");
  return 0;
}
