#include "../OpenBrewByWeightCrashArchive.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
constexpr const char *STATUS_NOT_FOUND = "404", *STATUS_OK = "200";
constexpr const char *STATUS_UNPROCESSABLE = "422";

struct httpd_req_t {
  bool admin = false;
  bool confirmed = false;
  int failChunk = 0;
  int chunks = 0;
  std::string status;
  std::vector<char> body;
};

int httpd_resp_set_type(httpd_req_t *, const char *) { return ESP_OK; }
int httpd_resp_set_hdr(httpd_req_t *, const char *, const char *) {
  return ESP_OK;
}
int httpd_resp_send_chunk(httpd_req_t *request, const char *data,
                          size_t length) {
  if (++request->chunks == request->failChunk) return ESP_FAIL;
  if (data) request->body.insert(request->body.end(), data, data + length);
  return ESP_OK;
}

namespace openbrewbyweight {
CrashArchiveStatus fakeStatus{};
std::vector<char> fakeDump(5000, 'D');
int clears = 0;
CrashArchiveStatus crashArchiveStatus(bool) { return fakeStatus; }
bool crashArchiveRead(uint8_t, size_t offset, void *output, size_t length) {
  if (offset + length > fakeDump.size()) return false;
  std::memcpy(output, fakeDump.data() + offset, length);
  return true;
}
bool crashArchiveClear() { ++clears; fakeStatus.count = 0; return true; }
int auditClears = 0;
bool clearAuditStores() { ++auditClears; return true; }

class OpenBrewByWeightNetwork {
 public:
  static OpenBrewByWeightNetwork *instance_;
  static esp_err_t crashArchiveDownloadHandler(httpd_req_t *request);
  static esp_err_t crashArchiveClearHandler(httpd_req_t *request);
  static esp_err_t auditLogClearHandler(httpd_req_t *request);
  bool requireAdminUnlock(httpd_req_t *request) {
    if (request->admin) return true;
    sendError(request, "401", "ADMIN_LOCKED", "Unlock Admin.");
    return false;
  }
  static esp_err_t sendError(httpd_req_t *request, const char *status,
                            const char *, const char *) {
    request->status = status;
    return ESP_OK;
  }
  static esp_err_t sendJson(httpd_req_t *request, const char *status,
                           const char *) {
    request->status = status;
    return ESP_OK;
  }
  esp_err_t runConfirmedClear(httpd_req_t *request, const char *, const char *,
                              const char *, const char *, bool (*clearFn)(),
                              const char *, const char *) {
    if (!request->confirmed) return sendError(request, STATUS_UNPROCESSABLE,
                                              "CONFIRM", "Confirm first.");
    return clearFn() ? sendJson(request, STATUS_OK, "{}") : ESP_FAIL;
  }
};
OpenBrewByWeightNetwork *OpenBrewByWeightNetwork::instance_ = nullptr;

#include "../diagnostics/OpenBrewByWeightCrashRoutes.inc"
}  // namespace openbrewbyweight

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

size_t octal(const char *text, size_t length) {
  size_t result = 0;
  for (size_t i = 0; i < length && text[i] >= '0' && text[i] <= '7'; ++i)
    result = result * 8 + text[i] - '0';
  return result;
}

int main() {
  using namespace openbrewbyweight;
  OpenBrewByWeightNetwork network;
  OpenBrewByWeightNetwork::instance_ = &network;
  fakeStatus.count = 2;
  fakeStatus.slots[0] = 0;
  fakeStatus.slots[1] = 1;
  auto &entry = fakeStatus.entries[0];
  entry.sequence = 3;
  entry.bootId = 8;
  entry.dumpBytes = fakeDump.size();
  entry.addresses.count = CRASH_ADDRESS_CAPACITY;
  for (size_t i = 0; i < CRASH_ADDRESS_CAPACITY; ++i)
    entry.addresses.addresses[i] = 0x42001234 + i;
  fakeStatus.entries[1] = entry;
  fakeStatus.entries[1].sequence = 2;

  httpd_req_t locked;
  check(OpenBrewByWeightNetwork::crashArchiveDownloadHandler(&locked) == ESP_OK &&
            locked.status == "401" && locked.body.empty(),
        "raw archive escaped Admin lock");
  httpd_req_t download;
  download.admin = true;
  check(OpenBrewByWeightNetwork::crashArchiveDownloadHandler(&download) == ESP_OK,
        "download failed");
  std::vector<std::string> names;
  size_t position = 0;
  while (position + 512 <= download.body.size() &&
         download.body[position] != 0) {
    const char *header = download.body.data() + position;
    unsigned sum = 0;
    for (size_t i = 0; i < 512; ++i)
      sum += i >= 148 && i < 156 ? ' ' :
                 static_cast<unsigned char>(header[i]);
    check(sum == octal(header + 148, 8), "invalid TAR checksum");
    names.emplace_back(header);
    const size_t length = octal(header + 124, 12);
    position += 512;
    check(position + length <= download.body.size(), "short TAR payload");
    if (names.back().find("/coredump.bin") != std::string::npos)
      check(length == fakeDump.size() &&
                std::memcmp(header + 512, fakeDump.data(), length) == 0,
            "raw dump changed in TAR");
    const std::string payload(header + 512, length);
    if (names.back().find("/addresses.txt") != std::string::npos)
      check(payload.find("0x4200126f\n") != std::string::npos,
            "full address list truncated");
    if (names.back().find("/manifest.txt") != std::string::npos)
      check(payload.find("other affected tasks.\n") != std::string::npos,
            "manifest truncated");
    position += (length + 511) / 512 * 512;
  }
  check(names.size() == 6 && names[0] == "crash-3/manifest.txt" &&
            names[1] == "crash-3/addresses.txt" &&
            names[2] == "crash-3/coredump.bin" &&
            names[3] == "crash-2/manifest.txt" &&
            names[4] == "crash-2/addresses.txt" &&
            names[5] == "crash-2/coredump.bin" && clears == 0,
        "archive contents or download retention wrong");
  check(download.body.size() - position == 1024,
        "TAR end blocks missing");

  httpd_req_t interrupted;
  interrupted.admin = true;
  interrupted.failChunk = 4;
  check(OpenBrewByWeightNetwork::crashArchiveDownloadHandler(&interrupted) == ESP_FAIL &&
            clears == 0 && fakeStatus.count == 2,
        "interrupted download removed data");
  httpd_req_t unconfirmed;
  unconfirmed.admin = true;
  check(OpenBrewByWeightNetwork::crashArchiveClearHandler(&unconfirmed) == ESP_OK &&
            unconfirmed.status == STATUS_UNPROCESSABLE && clears == 0,
        "unconfirmed deletion succeeded");
  httpd_req_t confirmed;
  confirmed.admin = confirmed.confirmed = true;
  check(OpenBrewByWeightNetwork::crashArchiveClearHandler(&confirmed) == ESP_OK &&
            clears == 1 && fakeStatus.count == 0,
        "confirmed deletion failed");
  return 0;
}
