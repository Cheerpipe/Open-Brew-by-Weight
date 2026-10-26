#pragma once

#include <cJSON.h>
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "OpenBrewByWeightPsram.h"

namespace openbrewbyweight {

// JSON bodies are already bounded by OpenBrewByWeightNetwork::REQUEST_BODY_CAPACITY.
// Keep an independent limit here so callers outside HTTP cannot accidentally
// hand an unbounded C string to cJSON.
constexpr size_t JSON_DOCUMENT_MAX_BYTES = 2047;
constexpr size_t JSON_DOCUMENT_MAX_DEPTH = 32;
// Every cJSON value consumes a heap node. Bound node count independently from
// wire size so a short sequence such as `[0,0,...]` cannot make a request
// allocate an unreviewed number of small blocks.
constexpr size_t JSON_DOCUMENT_MAX_VALUES = 128;

namespace detail {

inline std::atomic<uint32_t> g_jsonLimitRejections{0};
inline thread_local bool g_jsonLimitRejectedRecently = false;

struct JsonStructureScanner {
  const char *body;
  size_t length;
  size_t maxValues;
  size_t cursor = 0;
  size_t values = 0;

  bool atEnd() const { return cursor >= length; }
  char current() const { return atEnd() ? '\0' : body[cursor]; }

  void skipWhitespace() {
    while (!atEnd() && (current() == ' ' || current() == '\n' ||
                       current() == '\r' || current() == '\t')) {
      ++cursor;
    }
  }

  bool consume(char expected) {
    if (current() != expected) return false;
    ++cursor;
    return true;
  }

  bool scanString() {
    if (!consume('"')) return false;
    while (!atEnd()) {
      const unsigned char c = static_cast<unsigned char>(body[cursor++]);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') continue;
      if (atEnd()) return false;
      const char escaped = body[cursor++];
      if (escaped == '"' || escaped == '\\' || escaped == '/' ||
          escaped == 'b' || escaped == 'f' || escaped == 'n' ||
          escaped == 'r' || escaped == 't') {
        continue;
      }
      if (escaped != 'u' || length - cursor < 4) return false;
      for (size_t i = 0; i < 4; ++i) {
        const char hex = body[cursor++];
        if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') ||
              (hex >= 'A' && hex <= 'F'))) {
          return false;
        }
      }
    }
    return false;
  }

  bool scanLiteral(const char *literal) {
    for (; *literal != '\0'; ++literal) {
      if (!consume(*literal)) return false;
    }
    return true;
  }

  bool scanNumber() {
    if (current() == '-') ++cursor;
    if (current() == '0') {
      ++cursor;
    } else {
      if (current() < '1' || current() > '9') return false;
      do {
        ++cursor;
      } while (current() >= '0' && current() <= '9');
    }
    if (current() == '.') {
      ++cursor;
      if (current() < '0' || current() > '9') return false;
      do {
        ++cursor;
      } while (current() >= '0' && current() <= '9');
    }
    if (current() == 'e' || current() == 'E') {
      ++cursor;
      if (current() == '+' || current() == '-') ++cursor;
      if (current() < '0' || current() > '9') return false;
      do {
        ++cursor;
      } while (current() >= '0' && current() <= '9');
    }
    return true;
  }

  bool scanValue() {
    skipWhitespace();
    if (++values > maxValues) return false;
    switch (current()) {
      case '{':
        return scanObject();
      case '[':
        return scanArray();
      case '"':
        return scanString();
      case 't':
        return scanLiteral("true");
      case 'f':
        return scanLiteral("false");
      case 'n':
        return scanLiteral("null");
      default:
        return scanNumber();
    }
  }

  bool scanObject() {
    ++cursor;  // '{'
    skipWhitespace();
    if (consume('}')) return true;
    while (true) {
      if (!scanString()) return false;  // Object keys are not cJSON values.
      skipWhitespace();
      if (!consume(':') || !scanValue()) return false;
      skipWhitespace();
      if (consume('}')) return true;
      if (!consume(',')) return false;
      skipWhitespace();
    }
  }

  bool scanArray() {
    ++cursor;  // '['
    skipWhitespace();
    if (consume(']')) return true;
    while (true) {
      if (!scanValue()) return false;
      skipWhitespace();
      if (consume(']')) return true;
      if (!consume(',')) return false;
      skipWhitespace();
    }
  }

  bool scanDocument() {
    if (!scanValue()) return false;
    skipWhitespace();
    return atEnd();
  }
};

inline bool jsonNestingWithinLimit(const char *body, size_t length, size_t maxDepth) {
  size_t depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t i = 0; i < length; ++i) {
    const char c = body[i];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '{' || c == '[') {
      if (++depth > maxDepth) return false;
    } else if ((c == '}' || c == ']') && depth > 0) {
      --depth;
    }
  }
  return true;
}

// Malformed documents are rejected here, before cJSON allocates: the scanner
// is stricter than cJSON, so delegating them would bypass the value budget.
// overBudget separates budget overruns (size-limit rejections) from syntax.
inline bool jsonStructureAccepted(const char *body, size_t length,
                                  size_t maxValues, bool &overBudget) {
  JsonStructureScanner scanner{body, length, maxValues};
  const bool accepted = scanner.scanDocument();
  overBudget = scanner.values > maxValues;
  return accepted;
}

inline void noteJsonLimitRejection() {
  g_jsonLimitRejectedRecently = true;
  g_jsonLimitRejections.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace detail

// Install one process-wide capability allocator before starting HTTP/parser
// consumers. Never switch hooks per request or reset a shared document arena.
// Every node/string keeps independent ownership; heapCapsFree also accepts
// blocks allocated by the default allocator before initialization.
inline void initJsonParser() {
  static const bool initialized = [] {
    cJSON_Hooks hooks{};
    hooks.malloc_fn = [](size_t bytes) -> void * {
      return allocExternal(bytes, AllocationOwner::JSON);
    };
    hooks.free_fn = heapCapsFree;
    cJSON_InitHooks(&hooks);
    return true;
  }();
  (void)initialized;
}

inline cJSON *parseJsonDocumentWithinLimits(const char *body, size_t maxBytes,
                                            size_t maxValues,
                                            size_t maxDepth = JSON_DOCUMENT_MAX_DEPTH) {
  detail::g_jsonLimitRejectedRecently = false;
  if (body == nullptr || maxBytes == 0 || maxValues == 0) return nullptr;

  const size_t length = strnlen(body, maxBytes + 1);
  bool overLimit = true;
  const bool accepted =
      length <= maxBytes && detail::jsonNestingWithinLimit(body, length, maxDepth) &&
      detail::jsonStructureAccepted(body, length, maxValues, overLimit);
  if (!accepted && overLimit) detail::noteJsonLimitRejection();
  return accepted ? cJSON_ParseWithLengthOpts(body, length + 1, nullptr, 1)
                  : nullptr;
}

inline cJSON *parseJsonDocument(const char *body) {
  return parseJsonDocumentWithinLimits(body, JSON_DOCUMENT_MAX_BYTES,
                                       JSON_DOCUMENT_MAX_VALUES);
}

inline uint32_t jsonDocumentLimitRejections() {
  return detail::g_jsonLimitRejections.load(std::memory_order_relaxed);
}

inline bool jsonDocumentLimitRejectedRecently() {
  return detail::g_jsonLimitRejectedRecently;
}

// Transitional diagnostic API. There is intentionally no JSON arena anymore.
inline bool jsonArenaIsExternal() { return false; }
inline bool jsonArenaExhaustedRecently() {
  return jsonDocumentLimitRejectedRecently();
}

}  // namespace openbrewbyweight
