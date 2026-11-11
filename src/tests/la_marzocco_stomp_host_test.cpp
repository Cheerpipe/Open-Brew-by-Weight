#include "machine/OpenBrewByWeightLaMarzoccoStomp.h"
#include <cassert>
#include <algorithm>
#include <string>
#include <vector>

using namespace openbrewbyweight;
struct Received {
  std::vector<std::string> bodies;
  std::vector<uint32_t> attempts;
  static bool accept(void *context, const LaMarzoccoStompFrame &frame) {
    auto &received = *static_cast<Received *>(context);
    assert(std::string(frame.command) == "MESSAGE");
    assert(std::string(frame.destination) == "/ws/sn/test/dashboard");
    assert(std::string(frame.subscription) == "id:1");
    received.bodies.emplace_back(frame.body, frame.bodyLength);
    received.attempts.push_back(frame.ingressAttempt);
    return true;
  }
};
static std::string frame(const std::string &body, bool length, bool crlf) {
  const char *end = crlf ? "\r\n" : "\n";
  std::string data = std::string("MESSAGE") + end +
      "destination:/ws/sn/test/dashboard" + end + "subscription:id\\c1" + end;
  if (length) data += "content-length:" + std::to_string(body.size()) + end;
  return data + end + body + std::string(1, '\0');
}
int main() {
  // Synthetic STOMP bytes, split at every boundary and one byte at a time.
  for (bool length : {false, true}) for (bool crlf : {false, true}) {
    const auto bytes = frame("{\"widgets\":[]}", length, crlf);
    for (size_t split = 0; split <= bytes.size(); ++split) {
      LaMarzoccoStompDecoder decoder;
      Received received;
      assert(decoder.feed(bytes.data(), split, 1, Received::accept, &received));
      assert(decoder.feed(bytes.data() + split, bytes.size() - split, 2, Received::accept, &received));
      assert(received.bodies.size() == 1 && received.bodies[0] == "{\"widgets\":[]}");
      assert(!decoder.pendingSince());
    }
    LaMarzoccoStompDecoder decoder;
    Received received;
    for (char byte : bytes) assert(decoder.feed(&byte, 1, 1, Received::accept, &received));
    assert(received.bodies.size() == 1);
    const auto batch = "\n\r\n" + bytes + bytes + "\n";
    assert(decoder.feed(batch.data(), batch.size(), 2, Received::accept, &received));
    assert(received.bodies.size() == 3);
  }
  LaMarzoccoStompDecoder decoder;
  Received received;
  const auto oldFrame = frame("{}", true, false);
  assert(decoder.feed(oldFrame.data(), 5, 0, Received::accept, &received, 10));
  const auto joined = oldFrame.substr(5) + oldFrame + oldFrame;
  assert(decoder.feed(joined.data(), joined.size(), 1, Received::accept, &received, 11));
  assert((received.attempts == std::vector<uint32_t>{10, 11, 11}));
  const auto binary = frame(std::string("a\0b", 3), true, false);
  for (size_t split = 0; split <= binary.size(); ++split) {
    assert(decoder.feed(binary.data(), split, 1, Received::accept, &received));
    assert(decoder.feed(binary.data() + split, binary.size() - split, 2, Received::accept, &received));
    assert(received.bodies.back() == std::string("a\0b", 3) && !decoder.pending());
  }
  for (const auto &invalid : {
      std::string(1025, 'M'),
      frame(std::string(16385, 'x'), false, false),
      std::string("MESSAGE\ncontent-length:16385\n\n"),
      std::string("MESSAGE\ncontent-length:-1\n\n"),
      std::string("MESSAGE\ncontent-length:999999999999999999\n\n"),
      std::string("MESSAGE\ncontent-length:0\ncontent-length:0\n\n"),
      std::string("MESSAGE\nsubscription:a\\z\n\n")}) {
    decoder.reset();
    assert(!decoder.feed(invalid.data(), invalid.size(), 1, Received::accept, &received));
  }
  decoder.reset();
  assert(decoder.feed("MESSAGE\n", 8, 1, Received::accept, &received));
  assert(!decoder.incompleteExpired(10000));
  assert(decoder.incompleteExpired(10001));
  assert(!decoder.feed("\n", 1, 10001, Received::accept, &received));
  decoder.reset();
  assert(decoder.feed("MESSAGE\n", 8, UINT32_MAX - 99, Received::accept, &received));
  assert(decoder.incompleteExpired(10000));
  decoder.reset();
  const auto maximum = frame(std::string(16384, 'x'), true, false);
  assert(decoder.feed(maximum.data(), maximum.size(), 1, Received::accept, &received));
  assert(received.bodies.back().size() == 16384);
  // Header storage is reused in place; multiline/binary bodies and subsequent
  // frames must stay intact across callback-sized fragments and decoder resets.
  for (bool length : {false, true}) for (bool crlf : {false, true}) {
    const auto body = "{" + std::string(16382, '\n') + "}";
    auto bytes = frame(body, length, crlf);
    const std::string end = crlf ? "\r\n" : "\n";
    bytes.insert(bytes.find(end + end) + end.size(), std::string(800, 'k') + ":a\\nb\\cc" + end);
    bytes += frame("", length, crlf) + frame("{}", !length, !crlf);
    decoder.reset();
    received = {};
    for (size_t offset = 0; offset < bytes.size(); offset += 1024)
      assert(decoder.feed(bytes.data() + offset, std::min(size_t(1024), bytes.size() - offset),
                          1, Received::accept, &received, 12));
    assert((received.bodies == std::vector<std::string>{body, "", "{}"}));
    assert((received.attempts == std::vector<uint32_t>{12, 12, 12}));
    assert(!decoder.pending());
  }
  decoder.reset();
  const std::string bodyHeader = "MESSAGE\ncontent-length:3\n\n";
  assert(decoder.feed(bodyHeader.data(), bodyHeader.size(), 1, Received::accept, &received));
  assert(!decoder.feed("abc\0", 4, 10001, Received::accept, &received));
  decoder.reset();
  auto unterminated = frame("abc", true, false);
  unterminated.back() = 'x';
  assert(!decoder.feed(unterminated.data(), unterminated.size(), 1, Received::accept, &received));
  decoder.reset();
  std::string batch;
  for (unsigned count = 0; count < 33; ++count) batch += frame("{}", false, false);
  received = {};
  assert(!decoder.feed(batch.data(), batch.size(), 1, Received::accept, &received));
  assert(received.bodies.size() == 32);
}
