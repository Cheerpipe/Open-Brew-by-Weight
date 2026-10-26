namespace {

bool crashTarChunk(httpd_req_t *request, const void *data, size_t length) {
  return httpd_resp_send_chunk(request, static_cast<const char *>(data),
                               length) == ESP_OK;
}

bool crashTarHeader(httpd_req_t *request, const char *name, size_t length) {
  char header[512] = {};
  snprintf(header, 100, "%s", name);
  snprintf(header + 100, 8, "%07o", 0600);
  snprintf(header + 108, 8, "%07o", 0);
  snprintf(header + 116, 8, "%07o", 0);
  snprintf(header + 124, 12, "%011lo", static_cast<unsigned long>(length));
  snprintf(header + 136, 12, "%011o", 0);
  memset(header + 148, ' ', 8);
  header[156] = '0';
  memcpy(header + 257, "ustar", 5);
  memcpy(header + 263, "00", 2);
  unsigned sum = 0;
  for (unsigned char byte : header) sum += byte;
  snprintf(header + 148, 8, "%06o", sum);
  header[154] = '\0';
  header[155] = ' ';
  return crashTarChunk(request, header, sizeof(header));
}

bool crashTarPadding(httpd_req_t *request, size_t length) {
  static const char zero[512] = {};
  const size_t padding = (512 - length % 512) % 512;
  return padding == 0 || crashTarChunk(request, zero, padding);
}

void crashHex(const uint8_t *bytes, size_t count, char *output) {
  static constexpr char alphabet[] = "0123456789abcdef";
  for (size_t i = 0; i < count; ++i) {
    output[2 * i] = alphabet[bytes[i] >> 4];
    output[2 * i + 1] = alphabet[bytes[i] & 15];
  }
  output[2 * count] = '\0';
}

bool crashTarText(httpd_req_t *request, const char *path,
                  const char *text) {
  const size_t length = strlen(text);
  return crashTarHeader(request, path, length) &&
         crashTarChunk(request, text, length) &&
         crashTarPadding(request, length);
}

}  // namespace

esp_err_t OpenBrewByWeightNetwork::crashArchiveDownloadHandler(
    httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) return ESP_OK;
  const CrashArchiveStatus archive = crashArchiveStatus(true);
  if (archive.count == 0)
    return sendError(request, STATUS_NOT_FOUND, "NO_CRASHES",
                     "No saved crash records are available.");
  httpd_resp_set_type(request, "application/x-tar");
  httpd_resp_set_hdr(request, "Content-Disposition",
                     "attachment; filename=openbrewbyweight-crashes.tar");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  static const char zero[1024] = {};
  for (uint8_t i = 0; i < archive.count; ++i) {
    const CrashArchiveEntry &entry = archive.entries[i];
    char path[80];
    // Fits the manifest (~600 B) and 60 addresses (~730 B); it also stages the
    // core dump, keeping the internal httpd stack frame near 1 KiB.
    char text[1024];
    char sha[65], elf[65];
    crashHex(entry.dumpSha256, sizeof(entry.dumpSha256), sha);
    crashHex(entry.elfSha256, sizeof(entry.elfSha256), elf);
    snprintf(path, sizeof(path), "crash-%lu/manifest.txt",
             static_cast<unsigned long>(entry.sequence));
    snprintf(text, sizeof(text),
             "Boot ID: %lu\nReset reason: %lu\nCrashed task: %.16s\nCrashed PC: 0x%08lx\nDump bytes: %lu\nDump SHA-256: %s\n"
             "Firmware ELF SHA-256: %s\n"
             "The coredump.bin file is the raw ESP-IDF flash image, including "
             "its length header and SHA-256 trailer. Match the firmware ELF "
             "fingerprint before symbolizing it with ESP-IDF espcoredump. "
             "The address list is the panic callback's bounded view; for "
             "TWDT, inspect the core dump for other affected tasks.\n",
             static_cast<unsigned long>(entry.bootId),
             static_cast<unsigned long>(entry.resetReason),
             entry.crashedTask, static_cast<unsigned long>(entry.crashedPc),
             static_cast<unsigned long>(entry.dumpBytes), sha, elf);
    if (!crashTarText(request, path, text)) return ESP_FAIL;
    snprintf(path, sizeof(path), "crash-%lu/addresses.txt",
             static_cast<unsigned long>(entry.sequence));
    const CrashAddressRecord &addresses = entry.addresses;
    size_t used = snprintf(text, sizeof(text),
                           "Panic core: %lu\nPC: 0x%08lx\n"
                           "Corrupt: %s\nTruncated: %s\n",
                           static_cast<unsigned long>(addresses.core),
                           static_cast<unsigned long>(addresses.pc),
                           addresses.flags & 1 ? "yes" : "no",
                           addresses.flags & 2 ? "yes" : "no");
    for (uint32_t j = 0; j < addresses.count &&
                         j < CRASH_ADDRESS_CAPACITY && used + 16 < sizeof(text);
         ++j)
      used += snprintf(text + used, sizeof(text) - used, "0x%08lx\n",
                       static_cast<unsigned long>(addresses.addresses[j]));
    if (!crashTarText(request, path, text)) return ESP_FAIL;
    snprintf(path, sizeof(path), "crash-%lu/coredump.bin",
             static_cast<unsigned long>(entry.sequence));
    if (!crashTarHeader(request, path, entry.dumpBytes)) return ESP_FAIL;
    for (size_t done = 0; done < entry.dumpBytes; done += sizeof(text)) {
      const size_t bytes = entry.dumpBytes - done < sizeof(text)
                               ? entry.dumpBytes - done : sizeof(text);
      if (!crashArchiveRead(archive.slots[i], done, text, bytes) ||
          !crashTarChunk(request, text, bytes)) return ESP_FAIL;
    }
    if (!crashTarPadding(request, entry.dumpBytes)) return ESP_FAIL;
  }
  return crashTarChunk(request, zero, sizeof(zero)) &&
                 httpd_resp_send_chunk(request, nullptr, 0) == ESP_OK
             ? ESP_OK : ESP_FAIL;
}

esp_err_t OpenBrewByWeightNetwork::crashArchiveClearHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) return ESP_OK;
  return self.runConfirmedClear(
      request, "Stop the cycle and wait for Ready before emptying crashes.",
      "EMPTY_CRASH_ARCHIVE", "CRASH_CLEAR_NOT_CONFIRMED",
      "Confirm crash deletion explicitly.", crashArchiveClear,
      "CRASH_CLEAR_FAILED", "Crash records could not be erased.");
}

esp_err_t OpenBrewByWeightNetwork::auditLogClearHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) return ESP_OK;
  return self.runConfirmedClear(
      request, "Stop the cycle and wait for Ready before emptying the audit log.",
      "EMPTY_AUDIT_LOG", "AUDIT_CLEAR_NOT_CONFIRMED",
      "Confirm audit-log deletion explicitly.", clearAuditStores,
      "AUDIT_CLEAR_FAILED", "Audit records could not be erased.");
}
