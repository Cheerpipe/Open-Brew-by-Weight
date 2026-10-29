#pragma once

// Optional Web UI login: browser sessions, the Home Assistant token and its
// pairing request. Pure state with no I/O, clock or randomness of its own:
// the network task owns it under dataMux_, feeds times and random bytes, and
// persists `stored`. The login is effective only while the device password is
// not the published factory default; the network applies that rule.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <strings.h>

namespace openbrewbyweight {

constexpr size_t WEB_ACCESS_TOKEN_BYTES = 16;
constexpr size_t WEB_ACCESS_TOKEN_TEXT = WEB_ACCESS_TOKEN_BYTES * 2 + 1;
constexpr uint8_t WEB_ACCESS_REMEMBERED_SLOTS = 8;
constexpr uint8_t WEB_ACCESS_BROWSER_SLOTS = 4;
// Remembered sessions age on the wall clock and survive power cycles; the
// last-use stamp moves at most daily to bound flash writes. Without a synced
// clock a remembered session does not expire by age.
constexpr uint32_t WEB_ACCESS_REMEMBER_SEC = 90UL * 24UL * 3600UL;
constexpr uint32_t WEB_ACCESS_RENEW_SEC = 24UL * 3600UL;
constexpr uint32_t WEB_ACCESS_BROWSER_MS = 12UL * 3600UL * 1000UL;
constexpr uint32_t WEB_ACCESS_PAIRING_MS = 120000;
// A decided or expired request stays readable this long; no new request may
// start meanwhile, which also rate-limits pairing attempts.
constexpr uint32_t WEB_ACCESS_PAIRING_RESULT_MS = 30000;
constexpr uint32_t WEB_ACCESS_MAGIC = 0x57414343U;  // "WACC"
constexpr uint16_t WEB_ACCESS_VERSION = 1;
// Device label shown in Admin → Access, e.g. "iPhone · Safari" (NUL included).
constexpr size_t WEB_ACCESS_LABEL_BYTES = 24;
// Admin lists every session by an opaque id, never by its token: remembered
// slots first, then browser-only slots.
constexpr uint8_t WEB_ACCESS_SESSION_IDS =
    WEB_ACCESS_REMEMBERED_SLOTS + WEB_ACCESS_BROWSER_SLOTS;

struct WebAccessRemembered {
  uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
  uint32_t usedUtc = 0;  // 0 = never stamped on a synced clock
  char label[WEB_ACCESS_LABEL_BYTES] = {};
};

// One NVS blob. Missing or invalid means "login off, nothing remembered".
struct WebAccessStored {
  uint32_t magic = WEB_ACCESS_MAGIC;
  uint16_t version = WEB_ACCESS_VERSION;
  uint16_t structureSize = sizeof(WebAccessStored);
  uint8_t enabled = 0;
  uint8_t noticeDismissed = 0;
  uint8_t homeAssistant = 0;
  uint8_t rememberedMask = 0;
  uint32_t homeAssistantSinceUtc = 0;
  uint8_t homeAssistantToken[WEB_ACCESS_TOKEN_BYTES] = {};
  WebAccessRemembered remembered[WEB_ACCESS_REMEMBERED_SLOTS] = {};
  uint32_t checksum = 0;
};
static_assert(sizeof(WebAccessStored) == 388, "webAccess blob layout changed");

enum class WebAccessPairing : uint8_t { NONE, PENDING, APPROVED, DENIED, EXPIRED };

struct WebAccessState {
  WebAccessStored stored = {};
  struct Browser {
    uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
    uint32_t untilMs = 0;
    uint32_t seenMs = 0;
    char label[WEB_ACCESS_LABEL_BYTES] = {};
    bool live = false;
  } browser[WEB_ACCESS_BROWSER_SLOTS] = {};
  struct Pairing {
    WebAccessPairing state = WebAccessPairing::NONE;
    uint32_t id = 0;
    uint32_t untilMs = 0;
    char code[5] = {};
    uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
  } pairing = {};
  bool dirty = false;
};

inline uint32_t webAccessFnv(const void *data, size_t length) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  uint32_t hash = 2166136261U;
  for (size_t index = 0; index < length; ++index) {
    hash ^= bytes[index];
    hash *= 16777619U;
  }
  return hash;
}

inline uint32_t webAccessChecksum(const WebAccessStored &stored) {
  return webAccessFnv(&stored, offsetof(WebAccessStored, checksum));
}

inline void finalizeWebAccessStored(WebAccessStored &stored) {
  stored.magic = WEB_ACCESS_MAGIC;
  stored.version = WEB_ACCESS_VERSION;
  stored.structureSize = sizeof(WebAccessStored);
  stored.checksum = webAccessChecksum(stored);
}

inline bool validWebAccessStored(const WebAccessStored &stored) {
  return stored.magic == WEB_ACCESS_MAGIC &&
         stored.version == WEB_ACCESS_VERSION &&
         stored.structureSize == sizeof(WebAccessStored) &&
         stored.enabled <= 1 && stored.noticeDismissed <= 1 &&
         stored.homeAssistant <= 1 &&
         stored.checksum == webAccessChecksum(stored);
}

// Copies a browser-supplied label: control characters, quotes, backslashes and
// angle brackets are dropped, and only whole UTF-8 characters fit, so the
// result is safe to embed in JSON and HTML text as is.
template <size_t N>
inline void copyWebAccessLabel(char (&label)[N], const char *text) {
  size_t used = 0;
  for (size_t index = 0; text != nullptr && text[index] != '\0'; ++index) {
    const unsigned char c = static_cast<unsigned char>(text[index]);
    if (c < 0x20 || c == 0x7F || c == '"' || c == '\\' || c == '<' || c == '>') continue;
    const size_t width = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (width == 0) continue;  // a stray continuation byte
    if (used + width >= N) break;
    size_t next = 1;
    while (next < width && (static_cast<unsigned char>(text[index + next]) & 0xC0) == 0x80) ++next;
    if (next < width) continue;  // a truncated sequence
    memcpy(label + used, text + index, width);
    used += width;
    index += width - 1;
  }
  memset(label + used, 0, N - used);
}

// Tokens travel as exactly 32 lowercase hex digits.
inline bool parseWebAccessToken(const char *text,
                                uint8_t token[WEB_ACCESS_TOKEN_BYTES]) {
  if (text == nullptr) return false;
  for (size_t index = 0; index < WEB_ACCESS_TOKEN_BYTES * 2; ++index) {
    const char c = text[index];
    const int nibble = c >= '0' && c <= '9' ? c - '0'
                       : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (nibble < 0) return false;
    if ((index & 1U) == 0) token[index / 2] = static_cast<uint8_t>(nibble << 4);
    else token[index / 2] |= static_cast<uint8_t>(nibble);
  }
  return text[WEB_ACCESS_TOKEN_BYTES * 2] == '\0';
}

inline void formatWebAccessToken(const uint8_t token[WEB_ACCESS_TOKEN_BYTES],
                                 char text[WEB_ACCESS_TOKEN_TEXT]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t index = 0; index < WEB_ACCESS_TOKEN_BYTES; ++index) {
    text[index * 2] = digits[token[index] >> 4];
    text[index * 2 + 1] = digits[token[index] & 0x0F];
  }
  text[WEB_ACCESS_TOKEN_BYTES * 2] = '\0';
}

inline bool webAccessTokenEqual(const uint8_t *left, const uint8_t *right) {
  uint8_t difference = 0;
  for (size_t index = 0; index < WEB_ACCESS_TOKEN_BYTES; ++index) {
    difference |= left[index] ^ right[index];
  }
  return difference == 0;
}

inline bool webAccessRememberedLive(const WebAccessStored &stored, uint8_t slot,
                                    uint32_t nowUtc) {
  if ((stored.rememberedMask & (1U << slot)) == 0) return false;
  // A stamp ahead of the clock (NTP moved it back) counts as fresh.
  const uint32_t used = stored.remembered[slot].usedUtc;
  return nowUtc == 0 || nowUtc <= used || nowUtc - used < WEB_ACCESS_REMEMBER_SEC;
}

// nowUtc is 0 while the wall clock is not synced.
inline bool webAccessSessionValid(WebAccessState &state,
                                  const uint8_t token[WEB_ACCESS_TOKEN_BYTES],
                                  uint32_t nowMs, uint32_t nowUtc) {
  for (auto &browser : state.browser) {
    if (browser.live && webAccessTokenEqual(browser.token, token)) {
      if (static_cast<int32_t>(nowMs - browser.untilMs) < 0) {
        browser.seenMs = nowMs;
        return true;
      }
      browser = WebAccessState::Browser{};
      return false;
    }
  }
  WebAccessStored &stored = state.stored;
  for (uint8_t slot = 0; slot < WEB_ACCESS_REMEMBERED_SLOTS; ++slot) {
    if ((stored.rememberedMask & (1U << slot)) == 0 ||
        !webAccessTokenEqual(stored.remembered[slot].token, token)) {
      continue;
    }
    if (!webAccessRememberedLive(stored, slot, nowUtc)) {
      stored.rememberedMask &= static_cast<uint8_t>(~(1U << slot));
      stored.remembered[slot] = WebAccessRemembered{};
      state.dirty = true;
      return false;
    }
    uint32_t &used = stored.remembered[slot].usedUtc;
    if (nowUtc != 0 && (used == 0 || (nowUtc > used && nowUtc - used >= WEB_ACCESS_RENEW_SEC))) {
      used = nowUtc;
      state.dirty = true;
    }
    return true;
  }
  return false;
}

// The ui-stream a claim holds may keep flowing only while the session token
// behind that claim is still valid: revoking or aging out the session must
// close the stream, not just fail later token checks. A claim made without a
// session (login off, SoftAP onboarding) has no token and stays live.
inline bool webAccessClaimSessionLive(WebAccessState &state,
                                      const uint8_t *claimToken,
                                      bool claimTokenPresent, uint32_t nowMs,
                                      uint32_t nowUtc) {
  return !claimTokenPresent ||
         webAccessSessionValid(state, claimToken, nowMs, nowUtc);
}

// Full tables evict the session closest to expiry.
inline void webAccessIssueSession(WebAccessState &state,
                                  const uint8_t token[WEB_ACCESS_TOKEN_BYTES],
                                  bool remember, uint32_t nowMs,
                                  uint32_t nowUtc, const char *label) {
  if (!remember) {
    WebAccessState::Browser *target = &state.browser[0];
    for (auto &browser : state.browser) {
      if (!browser.live) { target = &browser; break; }
      if (static_cast<int32_t>(browser.untilMs - target->untilMs) < 0) target = &browser;
    }
    memcpy(target->token, token, WEB_ACCESS_TOKEN_BYTES);
    target->untilMs = nowMs + WEB_ACCESS_BROWSER_MS;
    target->seenMs = nowMs;
    copyWebAccessLabel(target->label, label);
    target->live = true;
    return;
  }
  WebAccessStored &stored = state.stored;
  uint8_t target = 0;
  for (uint8_t slot = 0; slot < WEB_ACCESS_REMEMBERED_SLOTS; ++slot) {
    if (!webAccessRememberedLive(stored, slot, nowUtc)) { target = slot; break; }
    if (stored.remembered[slot].usedUtc < stored.remembered[target].usedUtc) target = slot;
  }
  memcpy(stored.remembered[target].token, token, WEB_ACCESS_TOKEN_BYTES);
  stored.remembered[target].usedUtc = nowUtc;
  copyWebAccessLabel(stored.remembered[target].label, label);
  stored.rememberedMask |= static_cast<uint8_t>(1U << target);
  state.dirty = true;
}

// The Admin list id of a token's session, or -1.
inline int webAccessSessionId(const WebAccessState &state,
                              const uint8_t token[WEB_ACCESS_TOKEN_BYTES]) {
  for (uint8_t slot = 0; slot < WEB_ACCESS_REMEMBERED_SLOTS; ++slot) {
    if ((state.stored.rememberedMask & (1U << slot)) != 0 &&
        webAccessTokenEqual(state.stored.remembered[slot].token, token)) {
      return slot;
    }
  }
  for (uint8_t slot = 0; slot < WEB_ACCESS_BROWSER_SLOTS; ++slot) {
    if (state.browser[slot].live && webAccessTokenEqual(state.browser[slot].token, token)) {
      return WEB_ACCESS_REMEMBERED_SLOTS + slot;
    }
  }
  return -1;
}

inline bool webAccessRenameId(WebAccessState &state, uint32_t id, const char *label) {
  if (id < WEB_ACCESS_REMEMBERED_SLOTS) {
    if ((state.stored.rememberedMask & (1U << id)) == 0) return false;
    copyWebAccessLabel(state.stored.remembered[id].label, label);
    state.dirty = true;
    return true;
  }
  if (id >= WEB_ACCESS_SESSION_IDS || !state.browser[id - WEB_ACCESS_REMEMBERED_SLOTS].live) {
    return false;
  }
  copyWebAccessLabel(state.browser[id - WEB_ACCESS_REMEMBERED_SLOTS].label, label);
  return true;
}

inline bool webAccessRevokeId(WebAccessState &state, uint32_t id) {
  if (id < WEB_ACCESS_REMEMBERED_SLOTS) {
    if ((state.stored.rememberedMask & (1U << id)) == 0) return false;
    state.stored.rememberedMask &= static_cast<uint8_t>(~(1U << id));
    state.stored.remembered[id] = WebAccessRemembered{};
    state.dirty = true;
    return true;
  }
  if (id >= WEB_ACCESS_SESSION_IDS || !state.browser[id - WEB_ACCESS_REMEMBERED_SLOTS].live) {
    return false;
  }
  state.browser[id - WEB_ACCESS_REMEMBERED_SLOTS] = WebAccessState::Browser{};
  return true;
}

// Revokes Web UI sessions: just `token` when `only`, otherwise every one but
// `token` (nullable). The Home Assistant token is separate and survives.
inline void webAccessRevokeSessions(WebAccessState &state, const uint8_t *token,
                                    bool only = false) {
  const auto revoke = [&](const uint8_t *candidate) {
    return only == (token != nullptr && webAccessTokenEqual(candidate, token));
  };
  for (auto &browser : state.browser) {
    if (browser.live && revoke(browser.token)) browser = WebAccessState::Browser{};
  }
  WebAccessStored &stored = state.stored;
  for (uint8_t slot = 0; slot < WEB_ACCESS_REMEMBERED_SLOTS; ++slot) {
    if ((stored.rememberedMask & (1U << slot)) != 0 &&
        revoke(stored.remembered[slot].token)) {
      stored.rememberedMask &= static_cast<uint8_t>(~(1U << slot));
      stored.remembered[slot] = WebAccessRemembered{};
      state.dirty = true;
    }
  }
}

inline bool webAccessHomeAssistantValid(const WebAccessState &state,
                                        const uint8_t token[WEB_ACCESS_TOKEN_BYTES]) {
  return state.stored.homeAssistant != 0 &&
         webAccessTokenEqual(state.stored.homeAssistantToken, token);
}

// One integration slot: a new token replaces the previous one. It also closes
// a still-pending pairing request, whose later approval would replace it.
inline void webAccessSetHomeAssistant(WebAccessState &state,
                                      const uint8_t token[WEB_ACCESS_TOKEN_BYTES],
                                      uint32_t nowUtc) {
  if (state.pairing.state == WebAccessPairing::PENDING) state.pairing = {};
  memcpy(state.stored.homeAssistantToken, token, WEB_ACCESS_TOKEN_BYTES);
  state.stored.homeAssistant = 1;
  state.stored.homeAssistantSinceUtc = nowUtc;
  state.dirty = true;
}

inline void webAccessForgetHomeAssistant(WebAccessState &state) {
  if (state.stored.homeAssistant == 0) return;
  memset(state.stored.homeAssistantToken, 0, WEB_ACCESS_TOKEN_BYTES);
  state.stored.homeAssistant = 0;
  state.stored.homeAssistantSinceUtc = 0;
  state.dirty = true;
}

inline void webAccessTickPairing(WebAccessState &state, uint32_t nowMs) {
  auto &pairing = state.pairing;
  if (pairing.state == WebAccessPairing::NONE ||
      static_cast<int32_t>(nowMs - pairing.untilMs) < 0) {
    return;
  }
  if (pairing.state == WebAccessPairing::PENDING) {
    pairing.state = WebAccessPairing::EXPIRED;
    pairing.untilMs = nowMs + WEB_ACCESS_PAIRING_RESULT_MS;
    return;
  }
  pairing = WebAccessState::Pairing{};
}

// One request at a time; `code` is shown both in Home Assistant and in the
// Web UI prompt so the person approves the request they started.
inline bool webAccessStartPairing(WebAccessState &state, uint32_t id,
                                  uint16_t code, uint32_t nowMs) {
  webAccessTickPairing(state, nowMs);
  if (state.pairing.state != WebAccessPairing::NONE || id == 0) return false;
  auto &pairing = state.pairing;
  pairing.state = WebAccessPairing::PENDING;
  pairing.id = id;
  pairing.untilMs = nowMs + WEB_ACCESS_PAIRING_MS;
  for (int index = 3; index >= 0; --index) {
    pairing.code[index] = static_cast<char>('0' + code % 10);
    code = static_cast<uint16_t>(code / 10);
  }
  pairing.code[4] = '\0';
  return true;
}

inline bool webAccessDecidePairing(WebAccessState &state, uint32_t id, bool allow,
                                   const uint8_t token[WEB_ACCESS_TOKEN_BYTES],
                                   uint32_t nowMs, uint32_t nowUtc) {
  webAccessTickPairing(state, nowMs);
  auto &pairing = state.pairing;
  if (pairing.state != WebAccessPairing::PENDING || pairing.id != id) return false;
  pairing.state = allow ? WebAccessPairing::APPROVED : WebAccessPairing::DENIED;
  pairing.untilMs = nowMs + WEB_ACCESS_PAIRING_RESULT_MS;
  if (allow) {
    memcpy(pairing.token, token, WEB_ACCESS_TOKEN_BYTES);
    webAccessSetHomeAssistant(state, token, nowUtc);
  }
  return true;
}

// An approved token is handed out once; the slot then clears.
inline WebAccessPairing webAccessPairingStatus(
    WebAccessState &state, uint32_t id, uint32_t nowMs,
    uint8_t token[WEB_ACCESS_TOKEN_BYTES]) {
  webAccessTickPairing(state, nowMs);
  auto &pairing = state.pairing;
  if (pairing.state == WebAccessPairing::NONE || pairing.id != id) {
    return WebAccessPairing::NONE;
  }
  const WebAccessPairing result = pairing.state;
  if (result == WebAccessPairing::APPROVED) {
    memcpy(token, pairing.token, WEB_ACCESS_TOKEN_BYTES);
    pairing = WebAccessState::Pairing{};
  }
  return result;
}

// DNS-rebinding guard: a request is served only when its Host names this
// controller. IP literals cannot be rebound; names are the mDNS host (or its
// "-2"-style conflict rename), bare as routers publish the DHCP name or under
// a home-network suffix that no public DNS answers. The port is ignored; a
// request without Host (HTTP/1.0 tools, never a browser) passes.
inline bool webAccessHostAllowed(const char *host, const char *mdnsHost) {
  if (host == nullptr || host[0] == '\0' || host[0] == '[') return true;
  const size_t length = strcspn(host, ":");
  size_t own = mdnsHost == nullptr ? 0 : strlen(mdnsHost);
  if (strspn(host, "0123456789.") == length) return true;
  if (own == 0 || length < own || strncasecmp(host, mdnsHost, own) != 0) return false;
  const size_t renamed = host[own] == '-' ? strspn(host + own + 1, "0123456789") : 0;
  if (renamed > 0) own += renamed + 1;
  for (const char *suffix : {"", ".local", ".lan", ".home.arpa", ".internal"}) {
    if (length - own == strlen(suffix) && strncasecmp(host + own, suffix, length - own) == 0)
      return true;
  }
  return false;
}

// Device-password failures per peer address: a peer that fails
// WEB_ACCESS_PASSWORD_FAILURES times waits WEB_ACCESS_PASSWORD_COOLDOWN_MS,
// so one peer's guesses never lock out another. A slot frees once its peer
// has been quiet that long; while every slot holds a recent failure, new
// peers wait too, which caps rotating addresses at slots × failures guesses
// per cooldown.
constexpr uint8_t WEB_ACCESS_PASSWORD_PEERS = 4;
constexpr uint8_t WEB_ACCESS_PASSWORD_FAILURES = 5;
constexpr uint32_t WEB_ACCESS_PASSWORD_COOLDOWN_MS = 30000;
struct WebAccessPasswordPeer {
  uint32_t peer = 0;  // 0 = free
  uint32_t atMs = 0;  // last failure
  uint8_t failures = 0;
};

// The peer's slot, or a free one claimed for it; nullptr while all are busy.
inline WebAccessPasswordPeer *webAccessPasswordSlot(
    WebAccessPasswordPeer (&peers)[WEB_ACCESS_PASSWORD_PEERS], uint32_t peer, uint32_t nowMs) {
  WebAccessPasswordPeer *free = nullptr;
  for (auto &slot : peers) {
    if (slot.peer == peer) return &slot;
    if (free == nullptr && (slot.peer == 0 || nowMs - slot.atMs >= WEB_ACCESS_PASSWORD_COOLDOWN_MS))
      free = &slot;
  }
  if (free != nullptr) *free = WebAccessPasswordPeer{peer, nowMs, 0};
  return free;
}

inline bool webAccessPasswordCooling(const WebAccessPasswordPeer *slot, uint32_t nowMs) {
  return slot == nullptr || (slot->failures >= WEB_ACCESS_PASSWORD_FAILURES &&
                             nowMs - slot->atMs < WEB_ACCESS_PASSWORD_COOLDOWN_MS);
}

// Records one attempt; true when this failure starts the peer's cooldown.
inline bool webAccessPasswordAttempt(WebAccessPasswordPeer &slot, bool matched, uint32_t nowMs) {
  if (matched) {
    slot = WebAccessPasswordPeer{};
    return false;
  }
  if (slot.failures >= WEB_ACCESS_PASSWORD_FAILURES) slot.failures = 0;
  slot.atMs = nowMs;
  return ++slot.failures == WEB_ACCESS_PASSWORD_FAILURES;
}

// Login off: every session, the Home Assistant token and a pending request
// go; only the notice choice is kept.
inline void webAccessDisable(WebAccessState &state) {
  webAccessRevokeSessions(state, nullptr);
  webAccessForgetHomeAssistant(state);
  state.pairing = WebAccessState::Pairing{};
  if (state.stored.enabled != 0) {
    state.stored.enabled = 0;
    state.dirty = true;
  }
}

}  // namespace openbrewbyweight
