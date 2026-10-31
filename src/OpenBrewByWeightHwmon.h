#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_ipc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "OpenBrewByWeightPsram.h"
#include "soc/spi_mem_reg.h"
#endif

namespace openbrewbyweight {

// Flash cache read mode the bootloader left in SPI0: live hardware state, not
// the image-header request. A QIO build runs DIO when the chip's QE bit
// cannot be set, which is exactly the degradation Diagnostic must surface.
// Same register mapping as bootloader_flash_get_spi_mode(), read directly so
// the app does not link the whole bootloader_flash unit for one value.
inline const char *flashReadModeName() {
#ifdef ARDUINO
  const uint32_t ctrl = READ_PERI_REG(SPI_MEM_CTRL_REG(0));
  if (ctrl & SPI_MEM_FREAD_QIO) return "QIO";
  if (ctrl & SPI_MEM_FREAD_QUAD) return "QOUT";
  if (ctrl & SPI_MEM_FREAD_DIO) return "DIO";
  if (ctrl & SPI_MEM_FREAD_DUAL) return "DOUT";
  return "FASTRD";
#else
  return "DIO";
#endif
}

// The mode the image was compiled for (esptool still writes a DIO header for
// QIO builds; the bootloader enables quad during initialization).
inline const char *flashReadModeBuiltName() {
#if defined(CONFIG_ESPTOOLPY_FLASHMODE_QIO)
  return "QIO";
#elif defined(CONFIG_ESPTOOLPY_FLASHMODE_QOUT)
  return "QOUT";
#elif defined(CONFIG_ESPTOOLPY_FLASHMODE_DOUT)
  return "DOUT";
#else
  return "DIO";
#endif
}

inline bool flashReadModeDegraded() {
  return strcmp(flashReadModeName(), flashReadModeBuiltName()) != 0;
}

// Live MSPI clocks. One core clock select feeds both the flash (CS0) and
// PSRAM (CS1) dividers; mapping per the IDF flash HAL
// (spimem_flash_ll_get_source_freq_mhz). PSRAM mode has no silent
// degradation to detect: wrong module wiring leaves PSRAM absent, which
// the heap metrics already show.
inline uint32_t mspiCoreClockMhz() {
#ifdef ARDUINO
  switch (READ_PERI_REG(SPI_MEM_CORE_CLK_SEL_REG(0)) & SPI_MEM_CORE_CLK_SEL) {
    case 0: return 80;
    case 1: return 120;
    case 2: return 160;
    default: return 240;
  }
#else
  return 0;
#endif
}

inline uint32_t flashClockMhz() {
#ifdef ARDUINO
  const uint32_t clock = READ_PERI_REG(SPI_MEM_CLOCK_REG(0));
  const uint32_t div = (clock & SPI_MEM_CLK_EQU_SYSCLK) ? 1U :
      ((clock >> SPI_MEM_CLKCNT_N_S) & SPI_MEM_CLKCNT_N_V) + 1U;
  return mspiCoreClockMhz() / div;
#else
  return 0;
#endif
}

inline uint32_t psramClockMhz() {
#ifdef ARDUINO
  const uint32_t clock = READ_PERI_REG(SPI_MEM_SRAM_CLK_REG(0));
  const uint32_t div = (clock & SPI_MEM_SCLK_EQU_SYSCLK) ? 1U :
      ((clock >> SPI_MEM_SCLKCNT_N_S) & SPI_MEM_SCLKCNT_N_V) + 1U;
  return mspiCoreClockMhz() / div;
#else
  return 0;
#endif
}

inline const char *psramModeName() {
#if defined(CONFIG_SPIRAM_MODE_OCT)
  return "Octal";
#elif defined(CONFIG_SPIRAM_MODE_QUAD)
  return "Quad";
#else
  return "";
#endif
}

// Dual-core busy load: busy_i in [0,1], total load = busy0+busy1 in [0,2].
// 0.0 = both cores idle; 1.0 = one-core-full equivalent; 2.0 = both cores busy.
constexpr float HWMON_CPU_LOAD_MAX = 2.0f;
constexpr float HWMON_EMA_TAU_1M_S = 60.0f;
constexpr float HWMON_EMA_TAU_5M_S = 300.0f;

struct HwmonSnapshot {
  float cpuLoad5s = 0.0f;
  float cpuLoad1m = 0.0f;
  float cpuLoad5m = 0.0f;
  float cpu0Busy = 0.0f;
  float cpu1Busy = 0.0f;
  bool cpuLoadValid = false;
  uint32_t cpuMhz = 0;
  float tempC = 0.0f;
  float tempPeakC = 0.0f;
  bool tempValid = false;
  uint32_t ramTotalBytes = 0;
  uint32_t ramUsedBytes = 0;
  uint32_t ramFreeBytes = 0;
};

class Hwmon {
 public:
  void begin() {
#ifdef ARDUINO
    sampleStartedUs_ = esp_timer_get_time();
    prevIdleCounter_[0] = idleRunTimeCounter_(0);
#if portNUM_PROCESSORS > 1
    prevIdleCounter_[1] = idleRunTimeCounter_(1);
#else
    prevIdleCounter_[1] = 0;
#endif
    countersPrimed_ = true;
#else
    sampleStartedUs_ = 0;
#endif
  }

#ifndef ARDUINO
  // Host tests: set idle microseconds accumulated for the next sample().
  void hostSetIdleAccumUs(uint64_t idle0Us, uint64_t idle1Us,
                          bool countersFresh = true) {
    idleAccumUs_[0] = idle0Us;
    idleAccumUs_[1] = idle1Us;
    hostCountersFresh_ = countersFresh;
    hostIdleInjected_ = true;
  }
#endif

#ifdef ARDUINO
  HwmonSnapshot sample(uint32_t intervalMs,
                       const HeapCapSnapshot *heap = nullptr) {
#else
  HwmonSnapshot sample(uint32_t intervalMs) {
#endif
    HwmonSnapshot out;
    sampleCpuLoad_(out, intervalMs);

#ifndef ARDUINO
    out.cpuMhz = 80U;
    out.tempValid = true;
    out.tempC = 42.0f;
    if (!tempPeakValid_ || out.tempC > tempPeakC_) {
      tempPeakC_ = out.tempC;
      tempPeakValid_ = true;
    }
    out.tempPeakC = tempPeakC_;
    out.ramTotalBytes = 327680U;
    out.ramFreeBytes = 200000U;
    out.ramUsedBytes = out.ramTotalBytes - out.ramFreeBytes;
#else
    out.cpuMhz = getCpuFrequencyMhz();
    const HeapCapSnapshot used = heap != nullptr ? *heap : sampleHeapCaps();
    out.ramFreeBytes = used.internalFree;
    out.ramTotalBytes = used.internalTotal;
    if (out.ramTotalBytes >= out.ramFreeBytes) {
      out.ramUsedBytes = out.ramTotalBytes - out.ramFreeBytes;
    }

    const float temp = temperatureRead();
    if (std::isfinite(temp) && temp >= -40.0f && temp <= 125.0f) {
      out.tempValid = true;
      out.tempC = temp;
      if (!tempPeakValid_ || temp > tempPeakC_) {
        tempPeakC_ = temp;
        tempPeakValid_ = true;
      }
      out.tempPeakC = tempPeakC_;
    } else if (tempPeakValid_) {
      out.tempPeakC = tempPeakC_;
    }
#endif
    return out;
  }

 private:
  static float clamp01_(float v) {
    if (v < 0.0f) {
      return 0.0f;
    }
    if (v > 1.0f) {
      return 1.0f;
    }
    return v;
  }

  static float clampLoad_(float v) {
    if (v < 0.0f) {
      return 0.0f;
    }
    if (v > HWMON_CPU_LOAD_MAX) {
      return HWMON_CPU_LOAD_MAX;
    }
    return v;
  }

  static float emaStep_(float previous, float sample, float dtS, float tauS) {
    if (!(tauS > 0.0f) || !(dtS > 0.0f) || !std::isfinite(previous) ||
        !std::isfinite(sample)) {
      return sample;
    }
    const float alpha = 1.0f - std::exp(-dtS / tauS);
    return previous + alpha * (sample - previous);
  }

#ifdef ARDUINO
  // FreeRTOS idle run-time counters (µs via esp_timer, uint32 wrap ~71 min).
  // Runtime is committed when a task switches out. Refresh the remote core so
  // a continuously running IDLE task cannot make that core look fully busy.
  static void refreshIdleCounter_(void *) {}

  static bool refreshRemoteIdleCounter_() {
#if portNUM_PROCESSORS > 1
    return esp_ipc_call(1U - static_cast<uint32_t>(xPortGetCoreID()),
                        refreshIdleCounter_, nullptr) == ESP_OK;
#else
    return true;
#endif
  }

  static uint32_t idleRunTimeCounter_(BaseType_t core) {
    return static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(core));
  }

  static uint32_t idleDeltaUs_(uint32_t now, uint32_t prev) {
    // Unsigned wrap-safe delta for configRUN_TIME_COUNTER_TYPE uint32_t.
    return static_cast<uint32_t>(now - prev);
  }
#endif

  void publishPreviousCpu_(HwmonSnapshot &out) const {
    out.cpuLoadValid = false;
    out.cpuLoad5s = cpuLoad1m_;
    out.cpuLoad1m = cpuLoad1m_;
    out.cpuLoad5m = cpuLoad5m_;
    out.cpu0Busy = cpu0Busy_;
    out.cpu1Busy = cpu1Busy_;
  }

  void sampleCpuLoad_(HwmonSnapshot &out, uint32_t intervalMs) {
    uint64_t idle0 = 0;
    uint64_t idle1 = 0;
    uint64_t wallUs = 0;
#ifdef ARDUINO
    const bool countersFresh = refreshRemoteIdleCounter_();
#else
    const bool countersFresh = hostCountersFresh_;
#endif
    if (!countersFresh) {
      publishPreviousCpu_(out);
      return;
    }
#ifdef ARDUINO
    const uint32_t idle0Now = idleRunTimeCounter_(0);
#if portNUM_PROCESSORS > 1
    const uint32_t idle1Now = idleRunTimeCounter_(1);
#else
    const uint32_t idle1Now = 0;
#endif
    const uint64_t nowUs = esp_timer_get_time();
    if (sampleStartedUs_ != 0U && nowUs > sampleStartedUs_) {
      wallUs = nowUs - sampleStartedUs_;
    }
    sampleStartedUs_ = nowUs;
    if (wallUs == 0U && intervalMs > 0U) {
      wallUs = static_cast<uint64_t>(intervalMs) * 1000ULL;
    }
    if (countersPrimed_) {
      idle0 = idleDeltaUs_(idle0Now, prevIdleCounter_[0]);
      idle1 = idleDeltaUs_(idle1Now, prevIdleCounter_[1]);
    }
    prevIdleCounter_[0] = idle0Now;
    prevIdleCounter_[1] = idle1Now;
    countersPrimed_ = true;
    if (idle0 > wallUs) {
      idle0 = wallUs;
    }
    if (idle1 > wallUs) {
      idle1 = wallUs;
    }
#else
    idle0 = idleAccumUs_[0];
    idle1 = idleAccumUs_[1];
    idleAccumUs_[0] = 0;
    idleAccumUs_[1] = 0;
    wallUs =
        intervalMs > 0U ? static_cast<uint64_t>(intervalMs) * 1000ULL : 0U;
    // Host default: half-idle on each core → load 1.0 when nothing injected.
    if (idle0 == 0U && idle1 == 0U && wallUs > 0U && !hostIdleInjected_) {
      idle0 = wallUs / 2U;
      idle1 = wallUs / 2U;
    }
    hostIdleInjected_ = false;
#endif

    if (wallUs == 0U) {
      publishPreviousCpu_(out);
      return;
    }

    // Ignore tiny windows (e.g. boot sample(1)) — idle accounting needs time.
    if (wallUs < 100000ULL) {
      publishPreviousCpu_(out);
      return;
    }

    const float wall = static_cast<float>(wallUs);
    float busy0 = clamp01_(1.0f - static_cast<float>(idle0) / wall);
    float busy1 = clamp01_(1.0f - static_cast<float>(idle1) / wall);
#if defined(ARDUINO) && (portNUM_PROCESSORS < 2)
    busy1 = 0.0f;
#endif
    const float load = clampLoad_(busy0 + busy1);
    const float dtS = wall / 1.0e6f;

    if (!emaPrimed_) {
      cpuLoad1m_ = load;
      cpuLoad5m_ = load;
      emaPrimed_ = true;
    } else {
      cpuLoad1m_ = clampLoad_(emaStep_(cpuLoad1m_, load, dtS, HWMON_EMA_TAU_1M_S));
      cpuLoad5m_ = clampLoad_(emaStep_(cpuLoad5m_, load, dtS, HWMON_EMA_TAU_5M_S));
    }

    cpu0Busy_ = busy0;
    cpu1Busy_ = busy1;
    out.cpu0Busy = busy0;
    out.cpu1Busy = busy1;
    out.cpuLoad5s = load;
    out.cpuLoad1m = cpuLoad1m_;
    out.cpuLoad5m = cpuLoad5m_;
    out.cpuLoadValid = true;
  }

#ifdef ARDUINO
  uint32_t prevIdleCounter_[2] = {};
  bool countersPrimed_ = false;
#endif
#ifndef ARDUINO
  bool hostIdleInjected_ = false;
  bool hostCountersFresh_ = true;
  uint64_t idleAccumUs_[2] = {};
#endif
  uint64_t sampleStartedUs_ = 0;
  float cpu0Busy_ = 0.0f;
  float cpu1Busy_ = 0.0f;
  float cpuLoad1m_ = 0.0f;
  float cpuLoad5m_ = 0.0f;
  bool emaPrimed_ = false;
  float tempPeakC_ = 0.0f;
  bool tempPeakValid_ = false;
};

}  // namespace openbrewbyweight
