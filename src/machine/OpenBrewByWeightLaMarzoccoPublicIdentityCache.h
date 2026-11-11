#pragma once

#include <cstring>

namespace openbrewbyweight {

struct LaMarzoccoPublicIdentityCache {
  char id[37] = {};
  char publicKeyBase64[128] = {};

  template <typename Derive>
  bool ensure(Derive derive) {
    if (id[0] != '\0') return true;
    if (derive(id, publicKeyBase64)) return true;
    clear();
    return false;
  }

  void clear() {
    std::memset(id, 0, sizeof(id));
    std::memset(publicKeyBase64, 0, sizeof(publicKeyBase64));
  }
};

}  // namespace openbrewbyweight
