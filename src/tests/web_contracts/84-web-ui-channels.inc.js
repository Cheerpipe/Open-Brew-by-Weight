// Run the production claim, sign-in, Admin and stream code against a fake httpd:
// a browser at the controller's own address and the companion each keep their own
// claim and stream, and neither can take what the other holds.
{
  const assert = require('assert').strict, {spawnSync} = require('child_process');
  const read = (file) => fs.readFileSync(path.join(sketchDir, file), 'utf8');
  const fn = (text, signature) => {
    const start = text.indexOf(signature);
    assert(start >= 0, signature);
    return text.slice(start, text.indexOf('\n}', start) + 2);
  };
  const between = (text, from, to) => {
    const start = text.indexOf(from);
    assert(start >= 0, from);
    const end = text.indexOf(to, start);
    assert(end > start, to);
    return text.slice(start, end + to.length);
  };
  const owner = 'OpenBrewByWeightNetwork::';
  const auth = read('network/OpenBrewByWeightHttpAuthAssets.inc');
  const access = read('network/OpenBrewByWeightWebAccessHttp.inc');
  const relay = read('network/OpenBrewByWeightCompanionRelay.inc');
  const link = read('network/OpenBrewByWeightCompanionLink.h');
  const stream = read('network/OpenBrewByWeightUiStream.inc');
  const header = read('OpenBrewByWeightNetwork.h');

  const production = [
    fn(auth, 'esp_err_t ' + owner + 'claimHandler('),
    fn(auth, 'uint32_t ' + owner + 'webUiOverrideRemainingMs('),
    fn(auth, 'void ' + owner + 'clearAdminUnlock('),
    fn(auth, 'void ' + owner + 'grantAdminUnlock('),
    fn(auth, 'bool ' + owner + 'adminUnlockAllowed('),
    fn(auth, 'esp_err_t ' + owner + 'adminLockHandler('),
    fn(auth, 'bool ' + owner + 'requireActiveWebUiClient('),
    fn(access, 'bool ' + owner + 'readClaimSessionToken('),
    fn(access, 'void ' + owner + 'webAccessSignOut('),
    fn(access, 'esp_err_t ' + owner + 'authLoginHandler('),
    fn(access, 'esp_err_t ' + owner + 'authLogoutHandler('),
    fn(relay, 'void ' + owner + 'closeRelayedUiStreams('),
    fn(relay, 'bool ' + owner + 'webUiPrincipal('),
  ].join('\n');
  const dispatch = between(stream, '  const uint32_t claimNowMs = millis();',
      '      if (silent) session.bound = false;\n    }\n');
  const bindAllowed = between(stream, 'self.webUiClaim(session->relayed).clientId[0] &&',
      'self.webUiClaim(session->relayed).clientId) == 0');
  const bindSupersede = between(stream, 'for (auto &other : self.uiStreams_) {',
      'other.openedAtMs = millis() - kUiStreamBindMs;\n          }\n        }');

  const native = `
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "OpenBrewByWeightWebAccess.h"
using namespace openbrewbyweight;

#define OPEN_BREW_BY_WEIGHT_DEVELOPMENT 0
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
constexpr int STATUS_OK = 200, STATUS_BAD_REQUEST = 400, STATUS_UNAUTHORIZED = 401,
              STATUS_CONFLICT = 409;
constexpr size_t COMPANION_RELAY_SCREEN_TEXT = 25, COMPANION_RELAY_ADDR_TEXT = 48;
constexpr size_t WIFI_PASSWORD_CAPACITY = 64;
constexpr uint32_t kUiStreamBindMs = 5000, kUiStreamSilentMs = 330000,
                   kUiStreamDispatchBudgetMs = 800;
constexpr const char *WEB_UI_SESSION_HEADER = "X-WebUI-Session";
${between(link, 'struct CompanionPrincipal {', '\n};')}
${fn(link, 'inline bool companionPrincipalHolds(')}
${fn(link, 'inline bool companionPrincipalMove(')}
${fn(link, 'template <typename Session, size_t N>\nsize_t companionRelayedStreamsEnd(')}
struct httpd_req_t {
  const char *client = nullptr;
  const char *screen = "";
  const uint8_t *token = nullptr;
  const char *uri = "/";
};
struct httpd_handle_fake {};
int lastStatus = 0;
std::string lastError;
uint32_t clockMs = 1000;
uint32_t uiStreamDeadlineStartMs = 0, uiStreamDeadlineBudgetMs = UINT32_MAX;
uint8_t lastToken[WEB_ACCESS_TOKEN_BYTES] = {};
uint8_t tokenCounter = 0;
std::vector<int> closedFds;
WebAccessState g_webAccess;
uint32_t millis() { return clockMs++; }
struct { bool synced() const { return false; } uint32_t nowUtcSec(uint32_t) const { return 0; } } g_wallClock;
${fn(access, 'Instant webAccessNow(uint32_t nowMs)')}
bool firmwareCompatibilityMode() { return false; }
void esp_fill_random(uint8_t *out, size_t size) {
  ++tokenCounter;
  memset(out, tokenCounter, size);
  memcpy(lastToken, out, sizeof(lastToken));
}
void notePowerWebActivity(uint32_t, uint32_t) {}
int httpd_req_to_sockfd(httpd_req_t *) { return 3; }
int httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t) { return ESP_FAIL; }
int httpd_sess_trigger_close(int, int fd) { closedFds.push_back(fd); return ESP_OK; }
esp_err_t sendError(httpd_req_t *, int status, const char *code, const char *, const char *) {
  lastStatus = status;
  lastError = code;
  return ESP_OK;
}
esp_err_t sendJson(httpd_req_t *, int status, const char *) {
  lastStatus = status;
  lastError.clear();
  return ESP_OK;
}
struct TaskMutex { void lock() {} void unlock() {} };
struct TaskLockGuard { explicit TaskLockGuard(TaskMutex &) {} };

class OpenBrewByWeightNetwork {
 public:
  static constexpr size_t WEB_UI_CLIENT_ID_CAPACITY = 25;
  static constexpr uint32_t ADMIN_UNLOCK_IDLE_MS = 15 * 60 * 1000;
  struct UiStreamSession {
    int fd = -1;
    char clientId[WEB_UI_CLIENT_ID_CAPACITY] = {};
    bool bound = false, relayed = false;
    uint32_t openedAtMs = 0, heardAtMs = 0;
  };
  UiStreamSession uiStreams_[3];
  mutable TaskMutex dataMux_;
${between(header, '  struct WebUiClaim {', 'relayed ? companionClaim_ : directClaim_; }')}
  bool webUiOverrideActive_ = false;
  uint32_t webUiOverrideUntilMs_ = 0;
  CompanionPrincipal webUiOverride_{};
  bool adminUnlocked_ = false;
  CompanionPrincipal adminUnlock_{};
  uint32_t adminUnlockUntilMs_ = 0;
  struct { bool apActive = false; } status_;
  int server_ = 1;
  static OpenBrewByWeightNetwork *instance_;
  enum class PasswordCheck : uint8_t { OK, WRONG, COOLDOWN };
  struct WebAccessBody {
    char password[WIFI_PASSWORD_CAPACITY] = {};
    char action[24] = {};
    char device[WEB_ACCESS_LABEL_BYTES * 2] = {};
    bool remember = false;
  };
  static bool readWebUiClientId(httpd_req_t *request, char *out, size_t size) {
    if (!request->client) return false;
    snprintf(out, size, "%s", request->client);
    return true;
  }
  static bool readWebAccessToken(httpd_req_t *request, const char *, const char *, uint8_t *out) {
    if (!request->token) return false;
    memcpy(out, request->token, WEB_ACCESS_TOKEN_BYTES);
    return true;
  }
  bool relayIdentity(httpd_req_t *request, char *screen, char *) {
    if (!request->screen[0]) return false;
    snprintf(screen, COMPANION_RELAY_SCREEN_TEXT, "%s", request->screen);
    return true;
  }
  bool webAccessSessionAllowed(httpd_req_t *) { return true; }
  bool webAccessActive() const { return true; }
  bool socketServedBySoftAp(int) const { return false; }
  bool readWebAccessBody(httpd_req_t *, WebAccessBody &, esp_err_t &) { return true; }
  PasswordCheck checkDevicePassword(httpd_req_t *, const char *) { return PasswordCheck::OK; }
  static esp_err_t sendPasswordCheckError(httpd_req_t *, PasswordCheck) { return ESP_FAIL; }
  esp_err_t lockJsonBody(httpd_req_t *, void *, const char *) { return ESP_OK; }
  void unlockJsonBody() {}
  void lifecycleLog(const char *) {}
  static esp_err_t claimHandler(httpd_req_t *request);
  static esp_err_t adminLockHandler(httpd_req_t *request);
  static esp_err_t authLoginHandler(httpd_req_t *request);
  static esp_err_t authLogoutHandler(httpd_req_t *request);
  uint32_t webUiOverrideRemainingMs(httpd_req_t *request);
  void clearAdminUnlock(bool relayed, const char *screen = nullptr);
  void grantAdminUnlock(const CompanionPrincipal &holder, uint32_t now);
  bool adminUnlockAllowed(httpd_req_t *request);
  bool requireActiveWebUiClient(httpd_req_t *request);
  bool readClaimSessionToken(httpd_req_t *request, uint8_t out[WEB_ACCESS_TOKEN_BYTES]);
  void webAccessSignOut(const uint8_t *keep, bool everything = false);
  void closeRelayedUiStreams();
  bool webUiPrincipal(httpd_req_t *request, CompanionPrincipal &principal);

  struct Seen { const UiStreamSession *session; bool allowed; uint32_t deadlineStart; };
  std::vector<Seen> dispatchOnce(bool live) {
    OpenBrewByWeightNetwork &self = *this;
    std::vector<Seen> seen;
${dispatch}
      seen.push_back({&session, allowed, uiStreamDeadlineStartMs});
    }
    return seen;
  }
  // The bind op's own condition and supersede loop.
  bool bind(UiStreamSession *session, const char *id, int &superseded) {
    OpenBrewByWeightNetwork &self = *this;
    struct { const char *valuestring; } value{id}, *client = &value;
    if (!(${bindAllowed})) return false;
    snprintf(session->clientId, sizeof(session->clientId), "%s", id);
    ${bindSupersede}
    session->bound = true;
    return true;
  }
};
OpenBrewByWeightNetwork *OpenBrewByWeightNetwork::instance_ = nullptr;
using WebUiClaim = OpenBrewByWeightNetwork::WebUiClaim;
${production.replace(/\bWebUiClaim\{\}/g, 'OpenBrewByWeightNetwork::WebUiClaim{}')}

using N = OpenBrewByWeightNetwork;
N *net = nullptr;
void reset() {
  delete net;
  net = new N{};
  N::instance_ = net;
  g_webAccess = WebAccessState{};
  g_webAccess.stored.enabled = 1;
  closedFds.clear();
}
httpd_req_t direct(const char *id) { httpd_req_t r; r.client = id; return r; }
// Every companion screen sends the companion's one client id, signed with its own.
httpd_req_t screen(const char *id) { httpd_req_t r; r.client = "relay"; r.screen = id; return r; }
int claim(httpd_req_t r) { lastStatus = 0; N::claimHandler(&r); return lastStatus; }
bool gate(httpd_req_t r) { lastError.clear(); return net->requireActiveWebUiClient(&r); }
bool takenOver(httpd_req_t r) { return !gate(r) && lastError == "UI_TAKEN_OVER"; }
CompanionPrincipal principal(httpd_req_t r) {
  CompanionPrincipal p;
  assert(net->webUiPrincipal(&r, p));
  return p;
}
void unlock(httpd_req_t r) { net->grantAdminUnlock(principal(r), clockMs); }
bool admin(httpd_req_t r) { return net->adminUnlockAllowed(&r); }
void lock(httpd_req_t r) { N::adminLockHandler(&r); }
void setOverride(httpd_req_t r) {
  net->webUiOverrideActive_ = true;
  net->webUiOverrideUntilMs_ = clockMs + 60000;
  net->webUiOverride_ = principal(r);
}
bool override(httpd_req_t r) { return net->webUiOverrideRemainingMs(&r) != 0; }
void login(httpd_req_t r) { lastStatus = 0; N::authLoginHandler(&r); assert(lastStatus == 200); }
void logout(httpd_req_t r) { N::authLogoutHandler(&r); }
void openStream(int slot, const char *id, bool relayed) {
  auto &s = net->uiStreams_[slot];
  s = N::UiStreamSession{};
  s.fd = 10 + slot;
  s.relayed = relayed;
  s.bound = true;
  s.heardAtMs = clockMs;
  snprintf(s.clientId, sizeof(s.clientId), "%s", id);
}

int main() {
  const auto a = direct("window-a"), b = direct("window-b");
  const auto s1 = screen("screen-1"), s2 = screen("screen-2");

  // Each channel claims without touching the other; only its own is displaced.
  reset();
  assert(claim(a) == 200 && claim(s1) == 200);
  assert(gate(a) && gate(s1));
  assert(claim(b) == 200);
  assert(takenOver(a) && gate(b) && gate(s1));
  assert(claim(s2) == 200 && gate(s1) && gate(s2) && gate(b));

  // Admin held directly survives anything the companion does.
  reset();
  assert(claim(a) == 200);
  unlock(a);
  assert(admin(a) && !admin(s1));
  assert(claim(s1) == 200 && admin(a));
  assert(claim(s2) == 200 && admin(a));
  lock(s1);
  assert(admin(a));
  assert(claim(b) == 200 && !admin(a));

  // Admin held by a screen survives anything a direct browser does but unlocking.
  reset();
  assert(claim(s1) == 200);
  unlock(s1);
  assert(admin(s1) && !admin(a) && !admin(s2));
  assert(claim(a) == 200 && admin(s1));
  lock(a);
  assert(admin(s1));
  lock(s2);
  assert(admin(s1));
  assert(claim(s2) == 200 && admin(s1));
  lock(s1);
  assert(!admin(s1));
  unlock(s1);
  assert(claim(s1) == 200 && !admin(s1));

  // One holder across channels: a direct unlock replaces a screen's, and the reverse.
  reset();
  assert(claim(a) == 200 && claim(s1) == 200);
  unlock(s1);
  unlock(a);
  assert(admin(a) && !admin(s1));
  unlock(s1);
  assert(admin(s1) && !admin(a));

  // The unsafe override: the same channel rules, and it never reads across them.
  reset();
  assert(claim(a) == 200);
  setOverride(a);
  assert(override(a));
  assert(claim(s1) == 200 && override(a) && !override(s1));
  assert(claim(b) == 200 && !net->webUiOverrideActive_ && !override(b));
  reset();
  assert(claim(s1) == 200);
  setOverride(s1);
  assert(override(s1) && !override(s2));
  assert(claim(a) == 200 && override(s1) && !override(a));
  assert(claim(s2) == 200 && override(s1));
  assert(claim(s1) == 200 && !net->webUiOverrideActive_);

  // Signing in takes only the caller's channel, and Admin with it.
  reset();
  assert(claim(s1) == 200);
  login(a);
  assert(gate(a) && gate(s1) && admin(a) && !admin(s1));
  login(s2);
  assert(gate(a) && gate(s2) && admin(s2) && !admin(a));
  reset();
  assert(claim(s1) == 200);
  setOverride(s1);
  login(a);
  assert(override(s1));
  reset();
  assert(claim(a) == 200);
  setOverride(a);
  login(s1);
  assert(override(a) && net->webUiOverrideActive_);
  login(b);
  assert(!net->webUiOverrideActive_ && takenOver(a));

  // Signing out: a direct window ends its claim alone; a screen keeps the companion's.
  reset();
  assert(claim(a) == 200 && claim(s1) == 200);
  unlock(s1);
  logout(a);
  assert(!gate(a) && gate(s1) && admin(s1));
  logout(s1);
  assert(gate(s2) && !admin(s1));

  // Signing out everywhere ends both claims and Admin.
  reset();
  assert(claim(a) == 200 && claim(s1) == 200);
  unlock(s1);
  net->webAccessSignOut(nullptr);
  assert(!gate(a) && !gate(s1) && !admin(s1));

  // A revoked session closes the stream of its own channel only.
  reset();
  login(a);
  login(s1);
  uint8_t companionToken[WEB_ACCESS_TOKEN_BYTES];
  memcpy(companionToken, lastToken, sizeof(companionToken));
  openStream(0, "relay", true);
  openStream(2, "window-a", false);
  auto seen = net->dispatchOnce(false);
  assert(seen.size() == 2 && seen[0].allowed && seen[1].allowed);
  net->webAccessSignOut(companionToken);
  seen = net->dispatchOnce(false);
  assert(seen[0].session->relayed && seen[0].allowed && !seen[1].allowed);

  // Forget or the companion's Web UI off: only the companion's channel ends.
  reset();
  assert(claim(a) == 200 && claim(s1) == 200);
  openStream(0, "relay", true);
  openStream(2, "window-a", false);
  net->closeRelayedUiStreams();
  assert(closedFds.size() == 1 && closedFds[0] == 10);
  assert(!net->uiStreams_[0].bound && net->uiStreams_[2].bound);
  assert(gate(a) && takenOver(s1));
  reset();
  assert(claim(a) == 200);
  net->closeRelayedUiStreams();
  assert(closedFds.empty() && gate(a));

  // A window of an older, proxying companion moves across, and only that window.
  reset();
  assert(claim(direct("same-id")) == 200);
  unlock(direct("same-id"));
  assert(claim(screen("same-id")) == 200);
  assert(admin(screen("same-id")) && !admin(direct("same-id")));
  assert(takenOver(direct("same-id")) && gate(screen("same-id")));

  // The dispatch: each stream follows its own claim, the companion comes first, and
  // both share one budget however long the first takes.
  reset();
  assert(claim(a) == 200 && claim(s1) == 200);
  openStream(0, "relay", true);
  openStream(2, "window-a", false);
  seen = net->dispatchOnce(true);
  assert(seen.size() == 2 && seen[0].session == &net->uiStreams_[0] &&
         seen[1].session == &net->uiStreams_[2]);
  assert(seen[0].allowed && seen[1].allowed);
  assert(seen[0].deadlineStart == seen[1].deadlineStart);
  assert(claim(b) == 200);
  seen = net->dispatchOnce(true);
  assert(seen[0].allowed && !seen[1].allowed);
  reset();
  assert(claim(a) == 200 && claim(s1) == 200);
  openStream(0, "relay", true);
  openStream(2, "window-a", false);
  net->companionClaim_ = N::WebUiClaim{};
  seen = net->dispatchOnce(true);
  assert(!seen[0].allowed && seen[1].allowed);

  // Binding: a stream binds only its channel's claim and supersedes only its channel.
  reset();
  assert(claim(a) == 200 && claim(s1) == 200);
  int superseded = -1;
  auto newStream = [](int slot, bool relayed) {
    auto &s = net->uiStreams_[slot];
    s = N::UiStreamSession{};
    s.fd = 20 + slot;
    s.relayed = relayed;
    return &s;
  };
  assert(!net->bind(newStream(1, false), "relay", superseded));
  assert(!net->bind(newStream(0, true), "window-a", superseded));
  assert(net->bind(newStream(0, true), "relay", superseded) && superseded == -1);
  assert(net->bind(newStream(2, false), "window-a", superseded) && superseded == -1);
  assert(net->uiStreams_[0].bound && net->uiStreams_[2].bound);
  assert(claim(b) == 200);
  assert(net->bind(newStream(1, false), "window-b", superseded) && superseded == 22);
  assert(net->uiStreams_[0].bound && !net->uiStreams_[2].bound);
  superseded = -1;
  net->uiStreams_[2] = N::UiStreamSession{};
  assert(net->bind(newStream(2, true), "relay", superseded) && superseded == 20);
  assert(!net->uiStreams_[0].bound && net->uiStreams_[1].bound);
  return 0;
}
`;
  const directory = path.resolve(sketchDir, '..', 'temp', 'ai_temp_web_ui_channels');
  fs.mkdirSync(directory, {recursive: true});
  const binary = path.join(directory, 'channels-' + process.pid);
  const compiled = buildHostProgram(binary, native,
      ['-DOPEN_BREW_BY_WEIGHT_HOST_TEST', '-I' + sketchDir]);
  assert.equal(compiled.status, 0, compiled.error?.message || compiled.stderr);
  const run = spawnSync(binary, [], {encoding: 'utf8'});
  assert.equal(run.status, 0, String(run.signal) + ' ' + run.stderr);
  fs.unlinkSync(binary);
}
