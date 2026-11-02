#define OPEN_BREW_BY_WEIGHT_HOST_TEST

#include "../OpenBrewByWeightJsonArena.h"

#include <atomic>
#include <cJSON.h>
#include <iostream>
#include <string>
#include <thread>

namespace {

int failures = 0;
// Failure-injection budget large enough that a probe never fails; the amount
// it drops by is the number of allocations made.
constexpr int32_t kAllocationProbe = 1000000;

#define CHECK(condition)                                              \
  do {                                                                \
    if (!(condition)) {                                               \
      std::cerr << __func__ << ":" << __LINE__                        \
                << ": check failed: " << #condition << "\n";          \
      ++failures;                                                     \
      return;                                                         \
    }                                                                 \
  } while (false)

void testValidAndInvalidDocumentsAreIndependent() {
  cJSON *const prior = openbrewbyweight::parseJsonDocument("{\"value\":17}");
  CHECK(prior != nullptr);
  CHECK(openbrewbyweight::parseJsonDocument("{not-json}") == nullptr);
  const cJSON *const value = cJSON_GetObjectItemCaseSensitive(prior, "value");
  CHECK(cJSON_IsNumber(value));
  CHECK(value->valueint == 17);
  cJSON_Delete(prior);
}

void testConcurrentParsesDoNotShareStorage() {
  std::atomic<int> ready{0};
  std::atomic<bool> release{false};
  std::atomic<int> threadErrors{0};
  std::atomic<cJSON *> roots[2] = {};
  std::atomic<cJSON *> values[2] = {};

  auto parse = [&](int index) {
    const char *body = index == 0 ? "{\"worker\":0}" : "{\"worker\":1}";
    cJSON *const root = openbrewbyweight::parseJsonDocument(body);
    roots[index].store(root, std::memory_order_release);
    values[index].store(
        root == nullptr ? nullptr
                        : cJSON_GetObjectItemCaseSensitive(root, "worker"),
        std::memory_order_release);
    ready.fetch_add(1, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    if (root != nullptr) {
      const cJSON *const value = values[index].load(std::memory_order_acquire);
      if (!cJSON_IsNumber(value) || value->valueint != index)
        threadErrors.fetch_add(1, std::memory_order_relaxed);
      cJSON_Delete(root);
    }
  };

  std::thread first(parse, 0);
  std::thread second(parse, 1);
  while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
  const bool bothParsed =
      roots[0].load(std::memory_order_acquire) != nullptr &&
      roots[1].load(std::memory_order_acquire) != nullptr;
  const bool distinctRoots =
      roots[0].load(std::memory_order_acquire) !=
      roots[1].load(std::memory_order_acquire);
  const bool distinctValues =
      values[0].load(std::memory_order_acquire) !=
      values[1].load(std::memory_order_acquire);
  release.store(true, std::memory_order_release);
  first.join();
  second.join();
  CHECK(bothParsed);
  CHECK(distinctRoots);
  CHECK(distinctValues);
  CHECK(threadErrors.load(std::memory_order_relaxed) == 0);
}

void testDepthLimitRejectsWithoutDamagingPriorDocument() {
  cJSON *const prior = openbrewbyweight::parseJsonDocument("{\"safe\":true}");
  CHECK(prior != nullptr);
  std::string deep;
  for (size_t i = 0; i <= openbrewbyweight::JSON_DOCUMENT_MAX_DEPTH; ++i)
    deep.push_back('[');
  deep += '0';
  for (size_t i = 0; i <= openbrewbyweight::JSON_DOCUMENT_MAX_DEPTH; ++i)
    deep.push_back(']');
  CHECK(openbrewbyweight::parseJsonDocument(deep.c_str()) == nullptr);
  CHECK(openbrewbyweight::jsonDocumentLimitRejectedRecently());
  // A 160-byte UI frame can still exceed the parser's recursion budget.
  CHECK(deep.size() <= 160);
  CHECK(openbrewbyweight::parseJsonDocumentWithinLimits(
      deep.c_str(), 160, openbrewbyweight::JSON_DOCUMENT_MAX_VALUES) == nullptr);
  CHECK(openbrewbyweight::jsonDocumentLimitRejectedRecently());
  CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(prior, "safe")));
  cJSON_Delete(prior);
}

void testSizeLimitAndNullRejectCleanly() {
  std::string oversized(openbrewbyweight::JSON_DOCUMENT_MAX_BYTES + 1, ' ');
  CHECK(openbrewbyweight::parseJsonDocument(oversized.c_str()) == nullptr);
  CHECK(openbrewbyweight::jsonDocumentLimitRejectedRecently());
  CHECK(openbrewbyweight::parseJsonDocument(nullptr) == nullptr);
  CHECK(!openbrewbyweight::jsonDocumentLimitRejectedRecently());
}

void testExplicitResponseLimitsDoNotRelaxRequestDefaults() {
  const std::string large = "{\"payload\":\"" + std::string(3000, 'a') + "\"}";
  CHECK(openbrewbyweight::parseJsonDocument(large.c_str()) == nullptr);
  cJSON *root = openbrewbyweight::parseJsonDocumentWithinLimits(
      large.c_str(), 4095, openbrewbyweight::JSON_DOCUMENT_MAX_VALUES);
  CHECK(root != nullptr);
  cJSON_Delete(root);
}

std::string repeatedArrayValues(size_t count) {
  std::string body = "[";
  for (size_t i = 0; i < count; ++i) {
    if (i != 0) body += ',';
    body += '0';
  }
  return body + ']';
}

std::string repeatedObjectValues(size_t count) {
  std::string body = "{";
  for (size_t i = 0; i < count; ++i) {
    if (i != 0) body += ',';
    body += "\"v" + std::to_string(i) + "\":0";
  }
  return body + '}';
}

void testValueLimitAtBoundaryForArraysAndObjects() {
  // The root container is one cJSON value, so it leaves max-1 child values.
  const size_t children = openbrewbyweight::JSON_DOCUMENT_MAX_VALUES - 1;
  cJSON *array = openbrewbyweight::parseJsonDocument(repeatedArrayValues(children).c_str());
  CHECK(array != nullptr);
  cJSON_Delete(array);
  cJSON *object =
      openbrewbyweight::parseJsonDocument(repeatedObjectValues(children).c_str());
  CHECK(object != nullptr);
  cJSON_Delete(object);

  CHECK(openbrewbyweight::parseJsonDocument(
            repeatedArrayValues(children + 1).c_str()) == nullptr);
  CHECK(openbrewbyweight::jsonDocumentLimitRejectedRecently());
  CHECK(openbrewbyweight::parseJsonDocument(
            repeatedObjectValues(children + 1).c_str()) == nullptr);
  CHECK(openbrewbyweight::jsonDocumentLimitRejectedRecently());
}

void testStringsEscapesAndInvalidDocuments() {
  cJSON *const root = openbrewbyweight::parseJsonDocument(
      "{\"text\":\"[,]{\\\"quoted\\\"}\\\\slash\",\"comma\":\",]\"}");
  CHECK(root != nullptr);
  cJSON_Delete(root);
  CHECK(openbrewbyweight::parseJsonDocument("{\"missing\":[1,}") == nullptr);
  CHECK(!openbrewbyweight::jsonDocumentLimitRejectedRecently());
}

// cJSON tolerates raw control characters; the stricter scanner must not hand
// such a document over, or its trailing array escapes the value budget.
void testMalformedDocumentCannotBypassValueBudget() {
  openbrewbyweight::detail::g_hostAllocationsUntilFailure.store(kAllocationProbe);
  const std::string body = "{\"a\":\"\t\",\"b\":" + repeatedArrayValues(400) + "}";
  CHECK(openbrewbyweight::parseJsonDocument(body.c_str()) == nullptr);
  CHECK(openbrewbyweight::parseJsonDocument("[01]") == nullptr);
  CHECK(openbrewbyweight::detail::g_hostAllocationsUntilFailure.exchange(-1) == kAllocationProbe);
  CHECK(!openbrewbyweight::jsonDocumentLimitRejectedRecently());
  CHECK(openbrewbyweight::parseJsonDocumentWithinLimits(
            repeatedArrayValues(8).c_str(), 2047, 8) == nullptr);
  CHECK(openbrewbyweight::jsonDocumentLimitRejectedRecently());
}

void testAllocationFailureLeavesOtherDocumentsAlive() {
  cJSON *prior = openbrewbyweight::parseJsonDocument("{\"keep\":17}");
  CHECK(prior != nullptr);
  openbrewbyweight::detail::g_hostAllocationsUntilFailure.store(kAllocationProbe);
  cJSON *probe = openbrewbyweight::parseJsonDocument("{\"text\":\"escaped\\u00e9\",\"n\":[1,2]}");
  CHECK(probe != nullptr);
  const uint32_t allocations = static_cast<uint32_t>(
      kAllocationProbe - openbrewbyweight::detail::g_hostAllocationsUntilFailure.exchange(-1));
  cJSON_Delete(probe);
  CHECK(allocations > 0);
  for (uint32_t failAt = 0; failAt < allocations; ++failAt) {
    openbrewbyweight::detail::g_hostAllocationsUntilFailure.store(static_cast<int32_t>(failAt));
    cJSON *failed = openbrewbyweight::parseJsonDocument("{\"text\":\"escaped\\u00e9\",\"n\":[1,2]}");
    openbrewbyweight::detail::g_hostAllocationsUntilFailure.store(-1);
    CHECK(failed == nullptr);
    CHECK(cJSON_GetObjectItemCaseSensitive(prior, "keep")->valueint == 17);
  }
  cJSON_Delete(prior);
}

}  // namespace

int main() {
  // Blocks created before the one-time hook installation remain freeable.
  cJSON *legacy = cJSON_Parse("{\"legacy\":true}");
  openbrewbyweight::initJsonParser();
  cJSON_Delete(legacy);
  openbrewbyweight::initJsonParser();
  testValidAndInvalidDocumentsAreIndependent();
  testConcurrentParsesDoNotShareStorage();
  testDepthLimitRejectsWithoutDamagingPriorDocument();
  testSizeLimitAndNullRejectCleanly();
  testExplicitResponseLimitsDoNotRelaxRequestDefaults();
  testValueLimitAtBoundaryForArraysAndObjects();
  testStringsEscapesAndInvalidDocuments();
  testMalformedDocumentCannotBypassValueBudget();
  testAllocationFailureLeavesOtherDocumentsAlive();
  if (failures != 0) {
    std::cerr << failures << " JSON parser host test(s) failed\n";
    return 1;
  }
  std::cout << "JSON parser host tests passed\n";
  return 0;
}
