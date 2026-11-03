#define OPEN_BREW_BY_WEIGHT_HOST_TEST
#define OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST
#define OPEN_BREW_BY_WEIGHT_OTA_HOST_TEST

#include "../OpenBrewByWeightOta.cpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace openbrewbyweight {
struct OtaHostTestAccess {
  static TaskMutex &mutex() { return OpenBrewByWeightOta::mutex_; }
};
}  // namespace openbrewbyweight

int main() {
  using namespace openbrewbyweight;
  persistence_host::reset();
  OpenBrewByWeightOta &ota = OpenBrewByWeightOta::instance();
  ota.begin();

  // A range write holds the OTA mutex for seconds; status readers on httpd
  // and the Web UI stream must get the published copy instead of waiting.
  OtaHostTestAccess::mutex().lock();
  auto reader = std::async(std::launch::async, [&]() { return ota.snapshot(false); });
  const bool readerReturned =
      reader.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  OtaHostTestAccess::mutex().unlock();
  if (!readerReturned || !reader.get().available) {
    std::cerr << "OTA status read blocked behind the OTA mutex\n";
    return EXIT_FAILURE;
  }
  const OtaResult invalidIdentity = otaArchIsUsable(ota.snapshot(false).running.arch)
      ? OtaResult::SESSION_IDENTITY_MISMATCH : OtaResult::NO_IDENTITY;

  std::atomic<bool> start{false};
  std::atomic<int> failures{0};
  auto waitForStart = [&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  };

  std::thread writer([&]() {
    waitForStart();
    OtaSessionIdentity identity;
    identity.size = 65536;
    for (int iteration = 0; iteration < 2000; ++iteration) {
      // An empty identity is rejected regardless of the generated board arch.
      if (ota.createSession(identity, static_cast<uint32_t>(iteration)) !=
          invalidIdentity) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      ota.discard();
      (void)ota.serviceBoot(static_cast<uint32_t>(iteration), true, false, false, true, {0, 0});
    }
  });

  std::thread observer([&]() {
    waitForStart();
    for (int iteration = 0; iteration < 10000; ++iteration) {
      const OtaPublishedState published = ota.publishedState();
      const OtaStatusSnapshot snapshot = ota.snapshot(iteration % 2 == 0);
      if (!published.available || !published.confirmed || published.busy ||
          published.pendingVerify || published.rejected ||
          !snapshot.available || !snapshot.confirmed || snapshot.busy ||
          snapshot.sessionActive || snapshot.state != OtaState::IDLE) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  start.store(true, std::memory_order_release);
  writer.join();
  observer.join();

  if (failures.load(std::memory_order_relaxed) != 0) {
    std::cerr << "OTA state concurrency failures: " << failures.load() << "\n";
    return EXIT_FAILURE;
  }
  std::cout << "OTA state publication and transitions are race-free\n";
  return EXIT_SUCCESS;
}
