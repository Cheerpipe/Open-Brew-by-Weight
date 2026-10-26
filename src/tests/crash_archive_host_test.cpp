#include "../OpenBrewByWeightCrashArchive.cpp"

#include <cstdlib>
#include <iostream>

using namespace openbrewbyweight;

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void resetArchive() {
  std::fill(fakeCapture.bytes.begin(), fakeCapture.bytes.end(), 0xFF);
  std::fill(fakeHistory.bytes.begin(), fakeHistory.bytes.end(), 0xFF);
  fakeErases = fakeWrites = fakeReads = 0;
  fakeFailEraseAt = fakeFailWriteAt = fakeFailReadAt = 0;
  fakeCaptureValid = true;
  g_panicRecord.magic = 0;
  g_status = {0, CrashArchiveState::UNSUPPORTED, {}, {}};
}

void stageCrash(uint32_t bootId, uint8_t signature) {
  const uint32_t length = 5000;
  std::fill(fakeCapture.bytes.begin(), fakeCapture.bytes.end(), 0xFF);
  std::fill(fakeCapture.bytes.begin(), fakeCapture.bytes.begin() + length,
            signature);
  std::memcpy(fakeCapture.bytes.data(), &length, sizeof(length));
  crashArchivePrepareBoot();
  crashArchiveSetBootId(bootId);
  arduino_panic_info_t info;
  info.core = 1;
  info.pc = reinterpret_cast<void *>(0x42001234);
  info.backtrace_len = 2;
  info.backtrace[0] = 0x42001234;
  info.backtrace[1] = 0x42005678;
  crashArchiveRecordPanic(&info);
}

void boot(uint32_t bootId) {
  crashArchivePrepareBoot();
  crashArchivePromote(bootId, 7);
}

int main() {
  resetArchive();
  arduino_panic_info_t oversized;
  oversized.backtrace_len = 70;
  oversized.backtrace_corrupt = true;
  crashArchiveRecordPanic(&oversized);
  check(validRecord(g_panicRecord) &&
            g_panicRecord.count == CRASH_ADDRESS_CAPACITY &&
            g_panicRecord.flags == 3,
        "panic address bounds or flags were lost");
  stageCrash(1, 0x11);
  boot(2);
  auto status = crashArchiveStatus();
  check(status.count == 1 && status.entries[0].bootId == 1,
        "first crash was not promoted");
  check(status.entries[0].addresses.count == 2 &&
            fakeCapture.bytes[0] == 0xFF,
        "address pair or capture cleanup failed");
  const size_t writes = fakeWrites, erases = fakeErases;
  size_t reads = fakeReads;
  boot(3);
  check(fakeWrites == writes && fakeErases == erases,
        "ordinary reboot rewrote history");
  check(fakeReads == reads + 2, "ordinary reboot re-read a stored dump");
  reads = fakeReads;
  check(crashArchiveStatus(true).count == 1 && fakeReads > reads,
        "download verification skipped the stored dump");
  reads = fakeReads;
  check(crashArchiveStatus(true).count == 1 && fakeReads == reads,
        "verified dump was hashed again");

  stageCrash(3, 0x22);
  fakeFailWriteAt = fakeWrites + 2;
  boot(4);
  check(crashArchiveStatus().count == 1 && fakeCapture.bytes[0] != 0xFF,
        "interrupted copy discarded its capture or prior record");
  fakeFailWriteAt = 0;
  boot(5);
  check(crashArchiveStatus().count == 2 &&
            crashArchiveStatus().entries[0].bootId == 3,
        "interrupted copy was not retried");

  stageCrash(5, 0x33);
  fakeFailReadAt = fakeReads + 1;
  const size_t beforeReadFailure = fakeErases;
  boot(6);
  check(crashArchiveStatus().state == CrashArchiveState::IO_ERROR &&
            fakeErases == beforeReadFailure && fakeCapture.bytes[0] != 0xFF,
        "history read failure evicted a record or capture");
  fakeFailReadAt = 0;
  fakeFailEraseAt = fakeErases + CRASH_HISTORY_SLOT_BYTES / 4096 + 1;
  boot(6);
  status = crashArchiveStatus();
  check(status.count == 2 && status.entries[0].bootId == 5 &&
            status.entries[1].bootId == 3 && fakeCapture.bytes[0] != 0xFF,
        "commit-before-capture-erase recovery setup failed");
  const uint32_t sequence = status.entries[0].sequence;
  fakeFailEraseAt = 0;
  boot(7);
  status = crashArchiveStatus();
  check(status.count == 2 && status.entries[0].sequence == sequence &&
            fakeCapture.bytes[0] == 0xFF,
        "same capture was promoted twice after reboot");

  stageCrash(7, 0x33);
  boot(8);
  status = crashArchiveStatus();
  check(status.count == 2 && status.entries[0].bootId == 7 &&
            status.entries[1].bootId == 5,
        "identical later crash was mistaken for stale capture");

  stageCrash(8, 0x33);
  g_panicRecord.magic = 0;
  boot(9);
  check(crashArchiveStatus().state == CrashArchiveState::BOOT_ID_UNAVAILABLE &&
            fakeCapture.bytes[0] != 0xFF,
        "ambiguous missing-RTC capture was discarded");
  fakeFailEraseAt = fakeErases + fakeCapture.size / 4096 + 1;
  check(!crashArchiveClear() && crashArchiveStatus().count == 2 &&
            crashArchiveStatus().state == CrashArchiveState::IO_ERROR,
        "partial clear miscounted retained records");
  fakeFailEraseAt = 0;
  boot(10);
  check(crashArchiveStatus().count == 2 && fakeCapture.bytes[0] == 0xFF,
        "partial clear resurrected a capture");
  check(crashArchiveClear() && crashArchiveStatus().count == 0,
        "retry after partial clear failed");

  resetArchive();
  stageCrash(11, 0x44);
  fakeCaptureValid = false;
  boot(12);
  check(crashArchiveStatus().count == 0 &&
            crashArchiveStatus().state == CrashArchiveState::INVALID_CAPTURE,
        "invalid ESP-IDF capture was accepted");
  fakeCaptureValid = true;
  boot(13);
  check(crashArchiveStatus().count == 1, "validated capture was not retained");
  fakeHistory.bytes[0] ^= 1;
  boot(14);
  check(crashArchiveStatus().count == 1 && crashArchiveStatus(true).count == 0 &&
            crashArchiveStatus().count == 0,
        "corrupted committed dump survived download verification");
  check(crashArchiveClear() && crashArchiveStatus().count == 0,
        "confirmed archive clear failed");
  return 0;
}
