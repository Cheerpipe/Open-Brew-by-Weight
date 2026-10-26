#pragma once

// Pure HTTP request-header helpers for the httpd handlers: no I/O and no
// httpd types, so host tests exercise them directly.

#include <cstring>

namespace openbrewbyweight {

// If-None-Match (RFC 9110 13.1.2): a list of entity tags, separated by "," or
// ", ", each optionally weak. A reverse proxy that recompresses the shell
// weakens the ETag it forwards (`W/"..."`) and the browser sends that form
// back, so the weak comparison still revalidates to 304.
inline bool ifNoneMatchContains(const char *list, const char *etag) {
  const size_t length = strlen(etag);
  for (const char *token = list; *token != '\0';) {
    while (*token == ' ' || *token == ',') ++token;
    if (strncmp(token, "W/", 2) == 0) token += 2;
    if (strncmp(token, etag, length) == 0 &&
        (token[length] == '\0' || token[length] == ',' || token[length] == ' '))
      return true;
    while (*token != '\0' && *token != ',') ++token;
  }
  return false;
}

}  // namespace openbrewbyweight
