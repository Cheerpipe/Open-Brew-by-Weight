#ifndef ScaleBleBackend_h
#define ScaleBleBackend_h

#include <stdint.h>

enum class ScaleBleBackend : uint8_t {
    NimBle
};

static constexpr ScaleBleBackend kScaleBleBuildBackend = ScaleBleBackend::NimBle;

#endif
