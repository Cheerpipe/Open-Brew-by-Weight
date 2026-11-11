#pragma once
#include <cstdint>
uint32_t millis();
inline uint32_t micros() {
  static uint32_t stubNowMs = 0;
  return (stubNowMs += 1) * 1000U;
}
