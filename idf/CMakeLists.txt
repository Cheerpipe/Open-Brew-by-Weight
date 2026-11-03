# Open Brew by Weight firmware built with ESP-IDF (Arduino-ESP32 as a
# component). Official supported builds: ./scripts/dev build →
# build-idf/<variant>.
cmake_minimum_required(VERSION 3.16)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Board defines (PSRAM + Hardware CDC). CDC stays off
# until setup() sees the USB console jumper (GPIO4–GND) and calls Serial.begin
# on HWCDC (OpenBrewByWeightUsbConsole.h), unless OPENBREWBYWEIGHT_EXTRA_FLAGS includes
# -DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1 (ARDUINO_USB_CDC_ON_BOOT=1 + IDF console).
# Product options (remote machine control, buzzer, machine type, JTAG) live in
# main/Kconfig.projbuild and sdkconfig.defaults; OPENBREWBYWEIGHT_EXTRA_FLAGS may
# still pass -D overrides. idf.py does not reconfigure when only an env var
# changes, so the internal build stage writes CMAKE_BINARY_DIR/open_brew_by_weight_extra_flags
# and drops CMakeCache.txt when that stamp changes.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Select optional machine-specific components from the validated profile. A
# direct legacy CMake invocation without that manifest intentionally selects none.
set(_obbw_profile_manifest "${CMAKE_BINARY_DIR}/generated/build-profile.json")
if(NOT DEFINED OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION)
  set(OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION "none")
endif()
if(NOT OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION MATCHES "^(none|linea_micra_cloud)$")
  message(FATAL_ERROR "Invalid OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION=${OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION}")
endif()
if(EXISTS "${_obbw_profile_manifest}")
  file(READ "${_obbw_profile_manifest}" _obbw_profile_json)
  string(REGEX MATCH
    "\"machine_integration\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\""
    _obbw_profile_match "${_obbw_profile_json}")
  if(NOT CMAKE_MATCH_1 MATCHES "^(none|linea_micra_cloud)$")
    message(FATAL_ERROR "Invalid or missing machine_integration in ${_obbw_profile_manifest}")
  endif()
  if(NOT "${OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION}" STREQUAL "${CMAKE_MATCH_1}")
    message(FATAL_ERROR "Machine integration disagrees with ${_obbw_profile_manifest}")
  endif()
endif()

idf_build_set_property(COMPILE_DEFINITIONS "BOARD_HAS_PSRAM" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "ARDUINO_USB_MODE=1" APPEND)

set(_obbw_extra_flags_file "${CMAKE_BINARY_DIR}/open_brew_by_weight_extra_flags")
set(_obbw_extra_raw "")
set(_obbw_cdc_on_boot 0)
set(_obbw_project_only_options "")
if(DEFINED ENV{OPENBREWBYWEIGHT_EXTRA_FLAGS})
  set(_obbw_extra_raw "$ENV{OPENBREWBYWEIGHT_EXTRA_FLAGS}")
endif()
if(_obbw_extra_raw STREQUAL "" AND EXISTS "${_obbw_extra_flags_file}")
  file(READ "${_obbw_extra_flags_file}" _obbw_extra_raw)
  string(STRIP "${_obbw_extra_raw}" _obbw_extra_raw)
endif()

if(NOT _obbw_extra_raw STREQUAL "")
  message(STATUS "OPENBREWBYWEIGHT_EXTRA_FLAGS=${_obbw_extra_raw}")
  separate_arguments(_obbw_extra NATIVE_COMMAND "${_obbw_extra_raw}")
  foreach(_opt IN LISTS _obbw_extra)
    if(_opt STREQUAL "-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1")
      set(_obbw_cdc_on_boot 1)
    endif()
    if(_opt MATCHES "^-f(analyzer|no-inline|keep-inline-functions|no-ipa-sra|no-partial-inlining|no-ipa-cp-clone|no-ipa-icf|no-ipa-pure-const|no-ipa-modref)$")
      # Analysis options (gcc_analyzer, dead-code-idf) are attached below only
      # to project-owned components. Applying them globally makes ESP-IDF's
      # dependency warnings fatal before the firmware can be examined.
      list(APPEND _obbw_project_only_options "${_opt}")
    elseif(_opt MATCHES "^-D(.+)$")
      idf_build_set_property(COMPILE_DEFINITIONS "${CMAKE_MATCH_1}" APPEND)
    elseif(_opt MATCHES "^-W")
      # gcc C rejects C++-only warnings such as -Werror=deprecated-copy (the
      # default extra flag). Keep -W extras on C++ so mbedtls/.c still build.
      idf_build_set_property(CXX_COMPILE_OPTIONS "${_opt}" APPEND)
    else()
      idf_build_set_property(COMPILE_OPTIONS "${_opt}" APPEND)
    endif()
  endforeach()
endif()

idf_build_set_property(COMPILE_DEFINITIONS "ARDUINO_USB_CDC_ON_BOOT=${_obbw_cdc_on_boot}" APPEND)
if(_obbw_cdc_on_boot)
  message(STATUS "USB Serial/JTAG enabled (ARDUINO_USB_CDC_ON_BOOT=1)")
endif()

# The app descriptor reports the release number from VERSION, not `git describe`.
set(_obbw_version_file "${CMAKE_CURRENT_LIST_DIR}/../VERSION")
file(STRINGS "${_obbw_version_file}" PROJECT_VER LIMIT_COUNT 1)
project(openbrewbyweight)

# ninja cmake_check reconfigures when the internal build stage updates the stamp
# or VERSION changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_obbw_extra_flags_file}" "${_obbw_version_file}")

# Arduino-ESP32 compiles Esp.cpp as C++ against IDF SPI flash HAL headers
# that use partial designated initializers (gpspi_flash_ll.h /
# spimem_flash_ll.h). That is Espressif noise, not firmware.
idf_component_get_property(_obbw_arduino_lib espressif__arduino-esp32 COMPONENT_LIB)
target_compile_options(${_obbw_arduino_lib} PRIVATE
  -Wno-deprecated-declarations -Wno-missing-field-initializers)

set(_obbw_project_components main openBrewByWeight EspressoScaleBLE OpenBrewByWeightBleRuntime)

# SDK and managed-component headers are third-party code: project components
# include them as system directories (GCC drops the duplicate -I), so the
# strict warnings below apply to project code only. The SDK components'
# own compilation is unchanged.
idf_build_get_property(_obbw_build_components BUILD_COMPONENTS)
set(_obbw_sdk_include_dirs "")
foreach(_obbw_component IN LISTS _obbw_build_components)
  if(NOT _obbw_component IN_LIST _obbw_project_components)
    idf_component_get_property(_obbw_component_lib ${_obbw_component} COMPONENT_LIB)
    list(APPEND _obbw_sdk_include_dirs
      "$<TARGET_PROPERTY:${_obbw_component_lib},INTERFACE_INCLUDE_DIRECTORIES>")
  endif()
endforeach()

# Analyze only code maintained in this repository. SDK and managed-component
# diagnostics are neither hidden nor downgraded; they are simply outside this
# firmware-focused analyzer pass.
# ESP-IDF relaxes several warnings for every component (tools/cmake/build.cmake:
# -Wno-unused-parameter and -Wno-error=...). Project-owned components restore
# them as errors; component options follow the global ones, so they win.
foreach(_obbw_component IN LISTS _obbw_project_components)
  idf_component_get_property(_obbw_component_lib ${_obbw_component} COMPONENT_LIB)
  target_compile_options(${_obbw_component_lib} PRIVATE ${_obbw_project_only_options})
  target_include_directories(${_obbw_component_lib} SYSTEM PRIVATE ${_obbw_sdk_include_dirs})
  target_compile_options(${_obbw_component_lib} PRIVATE
    -Wunused-parameter -Werror=unused-parameter -Werror=unused-function
    -Werror=unused-variable -Werror=unused-but-set-variable
    -Werror=deprecated-declarations -Werror=extra)
endforeach()
