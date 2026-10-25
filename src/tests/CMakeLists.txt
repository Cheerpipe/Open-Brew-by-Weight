find_package(Threads REQUIRED)

execute_process(
  COMMAND "${CMAKE_SOURCE_DIR}/scripts/gen_version.sh"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMAND_ERROR_IS_FATAL ANY)

set(OPENBREWBYWEIGHT_SANITIZER "" CACHE STRING "Host sanitizer")
set(common_options -Wall -Wextra -Werror -pedantic)
if(OPENBREWBYWEIGHT_SANITIZER)
  list(APPEND common_options -fno-omit-frame-pointer -fsanitize=${OPENBREWBYWEIGHT_SANITIZER})
  add_link_options(-fno-omit-frame-pointer -fsanitize=${OPENBREWBYWEIGHT_SANITIZER})
endif()

function(host_test name)
  cmake_parse_arguments(ARG "" "LABEL;SOURCE" "SOURCES;DEFINITIONS;ARGS" ${ARGN})
  if(NOT ARG_SOURCE)
    set(ARG_SOURCE ${name}.cpp)
  endif()
  add_executable(${name} ${ARG_SOURCE} ${ARG_SOURCES})
  target_compile_features(${name} PRIVATE cxx_std_17)
  target_compile_options(${name} PRIVATE ${common_options})
  target_compile_definitions(${name} PRIVATE ${ARG_DEFINITIONS})
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE Threads::Threads)
  add_test(NAME ${name} COMMAND ${name} ${ARG_ARGS})
  set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABEL}")
endfunction()

host_test(power_policy_host_test DEFINITIONS OPEN_BREW_BY_WEIGHT_HOST_TEST
  LABEL "host;control;concurrency")
host_test(timezone_host_test LABEL "host;time")
host_test(scheduled_restart_host_test LABEL "host;control;time")
host_test(ble_runtime_host_test DEFINITIONS OPEN_BREW_BY_WEIGHT_BLE_RUNTIME_HOST_TEST
  LABEL "host;ble;concurrency")
target_include_directories(ble_runtime_host_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(ble_runtime_host_test PROPERTIES LABELS "host;ble;concurrency")

# Exercise actual Micra method bodies with deterministic task/HTTP shims.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(micra_source "${CMAKE_SOURCE_DIR}/src/machine/OpenBrewByWeightMicraService.cpp")
set(micra_extract "${CMAKE_CURRENT_SOURCE_DIR}/extract_micra_cancellation.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${micra_source}" "${micra_extract}"
  "${CMAKE_SOURCE_DIR}/src/machine/OpenBrewByWeightMicraWebSocket.inc")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${micra_extract}"
  "${micra_source}" "${CMAKE_CURRENT_BINARY_DIR}"
  COMMAND_ERROR_IS_FATAL ANY)
host_test(micra_cancellation_host_test DEFINITIONS OPEN_BREW_BY_WEIGHT_HOST_TEST
  LABEL "host;network;control;concurrency")
target_include_directories(micra_cancellation_host_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/micra_stubs" "${CMAKE_CURRENT_BINARY_DIR}")
host_test(micra_stomp_host_test LABEL "host;network;concurrency")
host_test(boot_heap_host_test LABEL "host;resources;network;concurrency")
host_test(ui_stream_transport_host_test LABEL "host;network")

if(OPENBREWBYWEIGHT_SANITIZER STREQUAL "thread")
  foreach(case M09 R25c F03 F04 F17 IT10 IT28 SH01 RACE06)
    host_test(open_brew_by_weight_tsan_${case}
      SOURCE open_brew_by_weight_host_test.cpp ARGS ${case} LABEL "host;concurrency")
    set_target_properties(open_brew_by_weight_tsan_${case} PROPERTIES
      OUTPUT_NAME open_brew_by_weight_tsan_${case})
  endforeach()
  host_test(ota_state_concurrency_host_test LABEL "host;ota;concurrency")
else()
  host_test(open_brew_by_weight_host_test LABEL "host;control")
  host_test(persistence_host_test LABEL "host;persistence")
  host_test(crash_archive_host_test
    DEFINITIONS OPEN_BREW_BY_WEIGHT_CRASH_HOST_TEST LABEL "host;persistence;safety")
  host_test(crash_archive_routes_host_test
    DEFINITIONS OPEN_BREW_BY_WEIGHT_HOST_TEST LABEL "host;network;web")
  host_test(bbw_cutoff_host_test LABEL "host;control;persistence")
  host_test(safety_external_host_test LABEL "host;safety")
  host_test(remote_policy_host_test LABEL "host;safety")
  host_test(remote_lockdown_host_test LABEL "host;safety;web")
  host_test(ota_image_host_test LABEL "host;ota")
  host_test(ota_transfer_host_test LABEL "host;ota")
  host_test(ota_state_concurrency_host_test LABEL "host;ota;concurrency")
  host_test(webhook_error_host_test
    DEFINITIONS OPEN_BREW_BY_WEIGHT_HOST_TEST LABEL "host;network")
  host_test(webhook_worker_host_test LABEL "host;network")
  host_test(web_access_host_test LABEL "host;network")
  host_test(heap_telemetry_host_test LABEL "host;resources")
host_test(audit_log_host_test LABEL "host;diagnostics")
host_test(audit_store_host_test DEFINITIONS OPEN_BREW_BY_WEIGHT_AUDIT_HOST_TEST
  LABEL "host;diagnostics;persistence")
  host_test(scale_profiler_host_test LABEL "host;scale;persistence")
  host_test(resource_owner_host_test LABEL "host;resources")
  host_test(machine_integration_host_test
    SOURCES "${CMAKE_SOURCE_DIR}/src/machine/OpenBrewByWeightMachineIntegration.cpp"
    LABEL "host;control")
  host_test(linea_micra_contract_host_test LABEL "host;network;control")
  host_test(reset_guard_instance_host_test
    SOURCES reset_guard_instance_a.cpp reset_guard_instance_b.cpp
    LABEL "host;safety")
  foreach(machine_type 1 2)
    set(target momentary_machine_type_${machine_type})
    add_executable(${target} momentary_machine_host_test.cpp)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(${target} PRIVATE ${common_options})
    target_compile_definitions(${target} PRIVATE OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=${machine_type})
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(${target} PRIVATE Threads::Threads)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "host;control;safety")
  endforeach()

  set(ble_dir "${CMAKE_SOURCE_DIR}/libraries/EspressoScaleBLE")
  function(ble_test name)
    add_executable(${name} "${ble_dir}/tests/${name}.cpp" ${ARGN})
    target_compile_features(${name} PRIVATE cxx_std_11)
    target_compile_options(${name} PRIVATE ${common_options})
    target_include_directories(${name} PRIVATE "${ble_dir}/tests/stubs" "${ble_dir}/src")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "host;ble")
  endfunction()
  ble_test(scale_ble_portable_test "${ble_dir}/tests/ScaleBleStateMachine.cpp")
  ble_test(nimble_advertisement_test
    "${ble_dir}/src/ScaleProtocolCommon.cpp"
    "${ble_dir}/src/protocols/Registry.cpp"
    "${ble_dir}/src/protocols/Acaia.cpp"
    "${ble_dir}/src/protocols/GenericFf11.cpp"
    "${ble_dir}/src/protocols/Felicita.cpp"
    "${ble_dir}/src/protocols/Eclair.cpp"
    "${ble_dir}/src/protocols/Decent.cpp"
    "${ble_dir}/src/protocols/Difluid.cpp"
    "${ble_dir}/src/protocols/Myscale.cpp"
    "${ble_dir}/src/protocols/WeighMyBru.cpp"
    "${ble_dir}/src/protocols/Varia.cpp"
    "${ble_dir}/src/protocols/Eureka.cpp"
    "${ble_dir}/src/nimble/NimbleAdvertisement.cpp")
  ble_test(nimble_resilience_test "${ble_dir}/src/nimble/NimbleResilience.cpp")
endif()

set(ble_dir "${CMAKE_SOURCE_DIR}/libraries/EspressoScaleBLE")
file(GLOB scale_protocol_sources CONFIGURE_DEPENDS "${ble_dir}/src/protocols/*.cpp")
host_test(nimble_client_test
  SOURCE "${ble_dir}/tests/nimble_client_test.cpp"
  SOURCES "${ble_dir}/src/ScaleProtocolCommon.cpp"
    "${ble_dir}/src/nimble/NimbleAdvertisement.cpp"
    "${ble_dir}/src/nimble/NimbleResilience.cpp" ${scale_protocol_sources}
  DEFINITIONS ESPRESSO_SCALE_BLE_HOST_TEST LABEL "host;ble;concurrency")
target_include_directories(nimble_client_test PRIVATE "${ble_dir}/tests/stubs" "${ble_dir}/src")
set_tests_properties(nimble_client_test PROPERTIES LABELS "host;ble;concurrency")

# The BBW λ sanity pin reads the real per-protocol sensor-lag table.
if(TARGET bbw_cutoff_host_test)
  target_sources(bbw_cutoff_host_test PRIVATE
    "${ble_dir}/src/ScaleProtocolCommon.cpp" ${scale_protocol_sources})
  target_include_directories(bbw_cutoff_host_test PRIVATE "${ble_dir}/src")
endif()

find_path(CJSON_INCLUDE_DIR cJSON.h PATH_SUFFIXES cjson)
find_library(CJSON_LIBRARY cjson)
if(NOT CJSON_INCLUDE_DIR OR NOT CJSON_LIBRARY)
  message(FATAL_ERROR "libcjson is required for host tests")
endif()
host_test(json_arena_host_test LABEL "host;concurrency")
host_test(micra_observation_host_test
  SOURCES "${CMAKE_SOURCE_DIR}/src/machine/OpenBrewByWeightMicraObservation.cpp"
  LABEL "host;network;control;concurrency")
target_include_directories(micra_observation_host_test PRIVATE "${CJSON_INCLUDE_DIR}")
target_link_libraries(micra_observation_host_test PRIVATE "${CJSON_LIBRARY}")
host_test(micra_websocket_host_test DEFINITIONS OPEN_BREW_BY_WEIGHT_HOST_TEST
  SOURCES "${CMAKE_SOURCE_DIR}/src/machine/OpenBrewByWeightMicraObservation.cpp"
  LABEL "host;network;control;concurrency")
target_include_directories(micra_websocket_host_test PRIVATE "${CJSON_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/micra_stubs" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(micra_websocket_host_test PRIVATE "${CJSON_LIBRARY}")
target_include_directories(json_arena_host_test PRIVATE "${CJSON_INCLUDE_DIR}")
target_link_libraries(json_arena_host_test PRIVATE "${CJSON_LIBRARY}")
host_test(integration_request_host_test LABEL "host;network;safety")
target_include_directories(integration_request_host_test PRIVATE "${CJSON_INCLUDE_DIR}")
target_link_libraries(integration_request_host_test PRIVATE "${CJSON_LIBRARY}")
host_test(corridor_host_test LABEL "host;control"
  DEFINITIONS "CORRIDOR_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")
target_include_directories(corridor_host_test PRIVATE "${CJSON_INCLUDE_DIR}")
target_link_libraries(corridor_host_test PRIVATE "${CJSON_LIBRARY}")
host_test(corridor_replay_host_test LABEL "host;control")
host_test(corridor_hint_loop_host_test LABEL "host;control")
