#include "../OpenBrewByWeightHttpHeaders.h"
#include "../OpenBrewByWeightWebAccess.h"

#include <cassert>
#include <initializer_list>
#include <cstring>

using namespace openbrewbyweight;

namespace {

void token(uint8_t seed, uint8_t out[WEB_ACCESS_TOKEN_BYTES]) {
  for (size_t index = 0; index < WEB_ACCESS_TOKEN_BYTES; ++index) {
    out[index] = static_cast<uint8_t>(seed + index * 7U);
  }
}

constexpr uint32_t DAY = 24UL * 3600UL;
constexpr uint32_t T0 = 1767225600UL;  // 2026-01-01, a synced wall clock

void tokenTextRoundTrips() {
  uint8_t value[WEB_ACCESS_TOKEN_BYTES], parsed[WEB_ACCESS_TOKEN_BYTES] = {};
  token(0xA0, value);
  char text[WEB_ACCESS_TOKEN_TEXT] = {};
  formatWebAccessToken(value, text);
  assert(std::strlen(text) == 32);
  assert(parseWebAccessToken(text, parsed));
  assert(webAccessTokenEqual(value, parsed));
  text[3] = 'A';
  assert(!parseWebAccessToken(text, parsed));  // lowercase only
  text[3] = '0';
  text[31] = '\0';
  assert(!parseWebAccessToken(text, parsed));
  assert(!parseWebAccessToken("", parsed));
  assert(!parseWebAccessToken(nullptr, parsed));
}

void browserSessionsExpireAndWrap() {
  WebAccessState state;
  uint8_t a[WEB_ACCESS_TOKEN_BYTES], b[WEB_ACCESS_TOKEN_BYTES];
  token(1, a);
  token(2, b);
  const uint32_t start = 0xFFFFFF00UL;  // millis wraps during the session
  webAccessIssueSession(state, a, false, start, 0, "Test");
  assert(!state.dirty);  // RAM only
  assert(webAccessSessionValid(state, a, start + WEB_ACCESS_BROWSER_MS - 1, 0));
  assert(!webAccessSessionValid(state, b, start, 0));
  assert(!webAccessSessionValid(state, a, start + WEB_ACCESS_BROWSER_MS, 0));
  assert(!webAccessSessionValid(state, a, start, 0));  // expired slot cleared
  for (uint8_t seed = 10; seed < 10 + WEB_ACCESS_BROWSER_SLOTS + 1; ++seed) {
    uint8_t t[WEB_ACCESS_TOKEN_BYTES];
    token(seed, t);
    webAccessIssueSession(state, t, false, seed, 0, "Test");
  }
  uint8_t first[WEB_ACCESS_TOKEN_BYTES], last[WEB_ACCESS_TOKEN_BYTES];
  token(10, first);
  token(10 + WEB_ACCESS_BROWSER_SLOTS, last);
  assert(!webAccessSessionValid(state, first, 20, 0));  // closest to expiry evicted
  assert(webAccessSessionValid(state, last, 20, 0));
}

void rememberedSessionsAgeRenewAndEvict() {
  WebAccessState state;
  uint8_t a[WEB_ACCESS_TOKEN_BYTES];
  token(1, a);
  webAccessIssueSession(state, a, true, 0, T0, "Test");
  assert(state.dirty && state.stored.rememberedMask == 1);
  state.dirty = false;
  assert(webAccessSessionValid(state, a, 0, T0 + 3600));
  assert(!state.dirty);  // renewal at most daily
  assert(webAccessSessionValid(state, a, 0, T0 + DAY));
  assert(state.dirty && state.stored.remembered[0].usedUtc == T0 + DAY);
  // Without a synced clock a remembered session never ages out.
  assert(webAccessSessionValid(state, a, 0, 0));
  assert(!webAccessSessionValid(state, a, 0, T0 + DAY + WEB_ACCESS_REMEMBER_SEC));
  assert(state.stored.rememberedMask == 0);

  WebAccessState full;
  for (uint8_t slot = 0; slot < WEB_ACCESS_REMEMBERED_SLOTS; ++slot) {
    uint8_t t[WEB_ACCESS_TOKEN_BYTES];
    token(static_cast<uint8_t>(20 + slot), t);
    webAccessIssueSession(full, t, true, 0, T0 + slot, "Test");
  }
  assert(full.stored.rememberedMask == 0xFF);
  uint8_t newest[WEB_ACCESS_TOKEN_BYTES], oldest[WEB_ACCESS_TOKEN_BYTES];
  token(99, newest);
  token(20, oldest);
  webAccessIssueSession(full, newest, true, 0, T0 + 100, "Test");
  assert(!webAccessSessionValid(full, oldest, 0, T0 + 100));
  assert(webAccessSessionValid(full, newest, 0, T0 + 100));
  assert(full.stored.rememberedMask == 0xFF);
}

void revocationKeepsOnlyWhatItShould() {
  WebAccessState state;
  uint8_t keep[WEB_ACCESS_TOKEN_BYTES], other[WEB_ACCESS_TOKEN_BYTES],
      browser[WEB_ACCESS_TOKEN_BYTES], ha[WEB_ACCESS_TOKEN_BYTES];
  token(1, keep);
  token(2, other);
  token(3, browser);
  token(4, ha);
  webAccessIssueSession(state, keep, true, 0, T0, "Test");
  webAccessIssueSession(state, other, true, 0, T0, "Test");
  webAccessIssueSession(state, browser, false, 0, T0, "Test");
  webAccessSetHomeAssistant(state, ha, T0);
  webAccessRevokeSessions(state, keep);
  assert(webAccessSessionValid(state, keep, 1, T0));
  assert(!webAccessSessionValid(state, other, 1, T0));
  assert(!webAccessSessionValid(state, browser, 1, T0));
  assert(webAccessHomeAssistantValid(state, ha));  // separate from sessions
  webAccessRevokeSessions(state, keep, true);
  assert(!webAccessSessionValid(state, keep, 1, T0));

  state.stored.enabled = 1;
  state.stored.noticeDismissed = 1;
  webAccessIssueSession(state, keep, true, 0, T0, "Test");
  assert(webAccessStartPairing(state, 7, 1234, 0));
  webAccessDisable(state);
  assert(state.stored.enabled == 0 && state.stored.noticeDismissed == 1);
  assert(state.stored.rememberedMask == 0 && state.stored.homeAssistant == 0);
  assert(!webAccessHomeAssistantValid(state, ha));
  assert(state.pairing.state == WebAccessPairing::NONE);
}

void pairingApprovesOnceDeniesAndExpires() {
  WebAccessState state;
  uint8_t issued[WEB_ACCESS_TOKEN_BYTES], out[WEB_ACCESS_TOKEN_BYTES] = {};
  token(5, issued);
  assert(!webAccessStartPairing(state, 0, 1, 0));  // id 0 is reserved
  assert(webAccessStartPairing(state, 42, 7, 1000));
  assert(std::strcmp(state.pairing.code, "0007") == 0);
  assert(!webAccessStartPairing(state, 43, 8, 1001));  // one at a time
  assert(webAccessPairingStatus(state, 43, 1002, out) == WebAccessPairing::NONE);
  assert(webAccessPairingStatus(state, 42, 1002, out) == WebAccessPairing::PENDING);
  assert(!webAccessDecidePairing(state, 43, true, issued, 1003, T0));
  assert(webAccessDecidePairing(state, 42, true, issued, 1003, T0));
  assert(webAccessHomeAssistantValid(state, issued));
  assert(state.stored.homeAssistantSinceUtc == T0 && state.dirty);
  assert(webAccessPairingStatus(state, 42, 1004, out) == WebAccessPairing::APPROVED);
  assert(webAccessTokenEqual(out, issued));
  assert(webAccessPairingStatus(state, 42, 1005, out) == WebAccessPairing::NONE);

  assert(webAccessStartPairing(state, 50, 9999, 2000));
  assert(webAccessDecidePairing(state, 50, false, issued, 2001, T0));
  assert(webAccessPairingStatus(state, 50, 2002, out) == WebAccessPairing::DENIED);
  assert(!webAccessStartPairing(state, 51, 1, 2003));  // result window
  assert(webAccessStartPairing(state, 51, 1, 2001 + WEB_ACCESS_PAIRING_RESULT_MS));

  const uint32_t started = 2001 + WEB_ACCESS_PAIRING_RESULT_MS;
  const uint32_t expired = started + WEB_ACCESS_PAIRING_MS;
  assert(webAccessPairingStatus(state, 51, expired, out) == WebAccessPairing::EXPIRED);
  assert(!webAccessDecidePairing(state, 51, true, issued, expired + 1, T0));
  assert(!webAccessStartPairing(state, 52, 1, expired + 1));
  assert(webAccessStartPairing(state, 52, 1, expired + WEB_ACCESS_PAIRING_RESULT_MS));

  // A password exchange closes the open request: approving it later must not
  // replace the token Home Assistant just received.
  token(6, out);
  webAccessSetHomeAssistant(state, out, T0);
  assert(state.pairing.state == WebAccessPairing::NONE);
  assert(!webAccessDecidePairing(state, 52, true, issued, expired + 1, T0));
  assert(webAccessHomeAssistantValid(state, out));
}

void storedBlobChecksum() {
  WebAccessStored stored;
  assert(!validWebAccessStored(stored));  // never finalized
  stored.enabled = 1;
  finalizeWebAccessStored(stored);
  assert(validWebAccessStored(stored));
  stored.rememberedMask = 3;
  assert(!validWebAccessStored(stored));
  finalizeWebAccessStored(stored);
  stored.enabled = 2;
  stored.checksum = webAccessChecksum(stored);
  assert(!validWebAccessStored(stored));
}

void labelsAreSanitizedAndWhole() {
  char label[WEB_ACCESS_LABEL_BYTES];
  copyWebAccessLabel(label, "iPhone \xC2\xB7 Safari");
  assert(std::strcmp(label, "iPhone \xC2\xB7 Safari") == 0);
  copyWebAccessLabel(label, "a\"b\\c<d>e\nf\x7Fg");
  assert(std::strcmp(label, "abcdefg") == 0);
  // 22 ASCII bytes leave room for one more byte only: the 2-byte "·" is dropped.
  copyWebAccessLabel(label, "0123456789012345678901\xC2\xB7");
  assert(std::strcmp(label, "0123456789012345678901") == 0);
  copyWebAccessLabel(label, "\xB7ok\xC2");  // stray continuation and truncated lead
  assert(std::strcmp(label, "ok") == 0);
  copyWebAccessLabel(label, nullptr);
  assert(label[0] == '\0');
}

void sessionsAreListedAndRevokedById() {
  WebAccessState state;
  uint8_t mac[WEB_ACCESS_TOKEN_BYTES], ipad[WEB_ACCESS_TOKEN_BYTES], other[WEB_ACCESS_TOKEN_BYTES];
  token(1, mac);
  token(2, ipad);
  token(3, other);
  webAccessIssueSession(state, mac, true, 0, T0, "MacBook \xC2\xB7 Chrome");
  webAccessIssueSession(state, ipad, false, 500, T0, "iPad");
  const int macId = webAccessSessionId(state, mac), ipadId = webAccessSessionId(state, ipad);
  assert(macId == 0 && ipadId == WEB_ACCESS_REMEMBERED_SLOTS);
  assert(webAccessSessionId(state, other) == -1);
  assert(std::strcmp(state.stored.remembered[0].label, "MacBook \xC2\xB7 Chrome") == 0);
  assert(std::strcmp(state.browser[0].label, "iPad") == 0 && state.browser[0].seenMs == 500);
  assert(webAccessSessionValid(state, ipad, 900, T0) && state.browser[0].seenMs == 900);
  state.dirty = false;
  assert(webAccessRevokeId(state, static_cast<uint32_t>(macId)) && state.dirty);
  assert(!webAccessSessionValid(state, mac, 901, T0));
  assert(!webAccessRevokeId(state, static_cast<uint32_t>(macId)));
  assert(webAccessRevokeId(state, static_cast<uint32_t>(ipadId)));
  assert(!webAccessSessionValid(state, ipad, 902, T0));
  assert(!webAccessRevokeId(state, WEB_ACCESS_SESSION_IDS));
  webAccessIssueSession(state, mac, true, 0, T0, "Mac");
  assert(webAccessRenameId(state, 0, "Kitchen <iPad>") && state.dirty);
  assert(std::strcmp(state.stored.remembered[0].label, "Kitchen iPad") == 0);
  assert(!webAccessRenameId(state, 1, "x") && !webAccessRenameId(state, WEB_ACCESS_SESSION_IDS, "x"));
}

// NET-02: only Hosts that name this controller pass; a rebinding attacker
// controls only public DNS names.
void hostAllowlistRefusesRebindingNames() {
  for (const char *host : {"192.168.1.40", "192.168.1.40:80", "[fe80::1]", "[2001:db8::7]:8080",
                           "cafe-bar", "Cafe-Bar.local", "cafe-bar.local:80", "cafe-bar.lan",
                           "cafe-bar.home.arpa", "cafe-bar.internal", "cafe-bar-2.local", ""}) {
    assert(webAccessHostAllowed(host, "cafe-bar"));
  }
  for (const char *host : {"evil.example", "cafe-bar.evil.example", "cafe-bar.local.evil.example",
                           "192.168.1.40.nip.io", "cafe-barx.local", "cafe.local", "localhost",
                           "openbrewbyweight.local", "cafe-bar-.local", "cafe-bar-2x.local"}) {
    assert(!webAccessHostAllowed(host, "cafe-bar"));
  }
  assert(webAccessHostAllowed(nullptr, "cafe-bar") && webAccessHostAllowed("10.0.0.2", ""));
  assert(!webAccessHostAllowed("cafe-bar.local", ""));
}

// NET-07: each peer has its own failure budget; a busy table refuses newcomers.
void passwordCooldownIsPerPeer() {
  WebAccessPasswordPeer peers[WEB_ACCESS_PASSWORD_PEERS] = {};
  WebAccessPasswordPeer *attacker = webAccessPasswordSlot(peers, 3, 100);
  for (uint8_t i = 1; i < WEB_ACCESS_PASSWORD_FAILURES; ++i) {
    assert(!webAccessPasswordAttempt(*attacker, false, 100 + i));
  }
  assert(webAccessPasswordAttempt(*attacker, false, 200));
  assert(webAccessPasswordCooling(webAccessPasswordSlot(peers, 3, 201), 201));
  WebAccessPasswordPeer *owner = webAccessPasswordSlot(peers, 5, 202);
  assert(owner != attacker && !webAccessPasswordCooling(owner, 202));
  assert(!webAccessPasswordAttempt(*owner, true, 202) && owner->peer == 0);
  assert(!webAccessPasswordCooling(webAccessPasswordSlot(peers, 3, 200 + WEB_ACCESS_PASSWORD_COOLDOWN_MS),
                                   200 + WEB_ACCESS_PASSWORD_COOLDOWN_MS));
  for (uint32_t peer = 7; peer <= 11; peer += 2) {  // fills the table with peer 3
    WebAccessPasswordPeer *slot = webAccessPasswordSlot(peers, peer, 300);
    assert(slot != nullptr);
    webAccessPasswordAttempt(*slot, false, 300);
  }
  assert(webAccessPasswordCooling(webAccessPasswordSlot(peers, 13, 301), 301));
  assert(webAccessPasswordSlot(peers, 13, 300 + WEB_ACCESS_PASSWORD_COOLDOWN_MS) != nullptr);
}

// The cached shell revalidates through proxies that weaken or list ETags.
void ifNoneMatchAcceptsWeakAndListedTags() {
  const char *etag = "\"1.2.3.0abc1234\"";
  for (const char *header : {"\"1.2.3.0abc1234\"", "W/\"1.2.3.0abc1234\"",
                             "\"old\", \"1.2.3.0abc1234\"", "\"old\",W/\"1.2.3.0abc1234\"",
                             "W/\"1.2.3.0abc1234\", \"other\""}) {
    assert(ifNoneMatchContains(header, etag));
  }
  for (const char *header : {"", "\"1.2.3.0abc1234-gzip\"", "\"1.2.3.0abc123\"",
                             "1.2.3.0abc1234", "W/", "\"old\", \"older\""}) {
    assert(!ifNoneMatchContains(header, etag));
  }
}

}  // namespace

int main() {
  ifNoneMatchAcceptsWeakAndListedTags();
  tokenTextRoundTrips();
  browserSessionsExpireAndWrap();
  rememberedSessionsAgeRenewAndEvict();
  revocationKeepsOnlyWhatItShould();
  pairingApprovesOnceDeniesAndExpires();
  storedBlobChecksum();
  labelsAreSanitizedAndWhole();
  sessionsAreListedAndRevokedById();
  hostAllowlistRefusesRebindingNames();
  passwordCooldownIsPerPeer();
  return 0;
}
