# Application firmware sources live in ../../../src. This component is
# their ESP-IDF registration.
set(OPEN_BREW_BY_WEIGHT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")

set(OPEN_BREW_BY_WEIGHT_SRCS
  "${OPEN_BREW_BY_WEIGHT_DIR}/openBrewByWeight.cpp"
  "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightTaskMutex.cpp"
  "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightResetGuard.cpp"
  "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightCrashArchive.cpp"
  "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightAuditStore.cpp"
  "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightPowerManagement.cpp"
  "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightScaleWorker.cpp"
  "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightNetwork.cpp"
  "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightOta.cpp"
  "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightWebhook.cpp"
  "${CMAKE_BINARY_DIR}/generated/OpenBrewByWeightWebAssetsGzip.cpp"
  "${CMAKE_BINARY_DIR}/generated/OpenBrewByWeightVersion.cpp")

set(OPEN_BREW_BY_WEIGHT_REQUIRES
  arduino-esp32
  EspressoScaleBLE
  OpenBrewByWeightBleRuntime)
if(OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION STREQUAL "linea_micra_cloud")
  list(APPEND OPEN_BREW_BY_WEIGHT_SRCS
    "${OPEN_BREW_BY_WEIGHT_DIR}/machine/OpenBrewByWeightLineaMicraIntegration.cpp"
    "${OPEN_BREW_BY_WEIGHT_DIR}/machine/OpenBrewByWeightMicraService.cpp"
    "${OPEN_BREW_BY_WEIGHT_DIR}/machine/OpenBrewByWeightMicraObservation.cpp")
else()
  list(APPEND OPEN_BREW_BY_WEIGHT_SRCS
    "${OPEN_BREW_BY_WEIGHT_DIR}/machine/OpenBrewByWeightMachineIntegration.cpp")
endif()

idf_component_register(
  SRCS ${OPEN_BREW_BY_WEIGHT_SRCS}
  INCLUDE_DIRS
    "${CMAKE_BINARY_DIR}/generated"
    "${OPEN_BREW_BY_WEIGHT_DIR}"
  REQUIRES
    ${OPEN_BREW_BY_WEIGHT_REQUIRES}
  PRIV_REQUIRES
    espressif__esp_websocket_client
    espressif__cjson
    mbedtls
    nvs_flash
    esp_wifi
    esp_timer
    esp_pm
    esp_system
    esp_driver_gptimer
    bt
    app_update
    esp_http_server
    esp_http_client
    esp_partition
    espcoredump
    esp_hw_support
    # esp_cache_msync: the shot trace's PSRAM write-backs.
    esp_mm
    log
    heap
    soc
    # WiFiGeneric.h includes this even when CONFIG_ARDUINO_SELECTIVE_WiFiProv
    # is unset (selective compilation). main used to get it via auto-REQUIRES.
    espressif__network_provisioning
)


# API error prose for reason-coded responses is generated from the Web UI
# locale catalog (en.json err.*) — the single editable copy of the sentences —
# into the build tree. The same generator runs in the host test build and
# fails the build on any catalog/map/reason disagreement.
find_program(OBBW_NODE_EXECUTABLE node REQUIRED)
set(OBBW_ERROR_TEXTS_HEADER
  "${CMAKE_BINARY_DIR}/generated/OpenBrewByWeightErrorReasonTexts.h")
file(GLOB CONFIGURE_DEPENDS OBBW_ERROR_TEXT_SCANNED
  "${OPEN_BREW_BY_WEIGHT_DIR}/network/*.inc"
  "${OPEN_BREW_BY_WEIGHT_DIR}/diagnostics/*.inc")
add_custom_command(
  OUTPUT "${OBBW_ERROR_TEXTS_HEADER}"
  COMMAND "${OBBW_NODE_EXECUTABLE}"
          "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/generate_error_reason_texts.js"
          --output "${OBBW_ERROR_TEXTS_HEADER}"
  DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/generate_error_reason_texts.js"
    "${OPEN_BREW_BY_WEIGHT_DIR}/web/locales/en.json"
    "${OPEN_BREW_BY_WEIGHT_DIR}/web/js/runtime.js"
    "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightNetwork.h"
    "${OPEN_BREW_BY_WEIGHT_DIR}/OpenBrewByWeightValidationFailure.h"
    ${OBBW_ERROR_TEXT_SCANNED}
  VERBATIM)
add_custom_target(openbrewbyweight_error_reason_texts
  DEPENDS "${OBBW_ERROR_TEXTS_HEADER}")
add_dependencies(${COMPONENT_LIB} openbrewbyweight_error_reason_texts)
# scripts/internal/build-idf writes these sources before every build; the first
# configure of a fresh tree runs before they exist.
set_source_files_properties(
  "${CMAKE_BINARY_DIR}/generated/OpenBrewByWeightWebAssetsGzip.cpp"
  "${CMAKE_BINARY_DIR}/generated/OpenBrewByWeightVersion.cpp"
  PROPERTIES GENERATED TRUE)
