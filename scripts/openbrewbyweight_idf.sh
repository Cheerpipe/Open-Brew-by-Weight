#!/usr/bin/env bash
# Shared ESP-IDF helpers. Source after openbrewbyweight_board.sh and
# openbrewbyweight_cli.sh; do not execute this file directly.
#
# Official firmware builds write to build-idf/<hardware>--<machine> and use native
# ESP-IDF NimBLE as the only BLE backend.

IDF_PROJECT_NAME="openbrewbyweight"
IDF_DEFAULT_HOME="${HOME}/esp/esp-idf-v6.1"

obbw_idf_find() {
  if [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/export.sh" ]]; then
    printf '%s' "$IDF_PATH"
    return 0
  fi
  if [[ -f "${IDF_DEFAULT_HOME}/export.sh" ]]; then
    printf '%s' "$IDF_DEFAULT_HOME"
    return 0
  fi
  return 1
}

obbw_idf_active_valid() {
  [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/export.sh" ]] || return 1
  [[ -n "${IDF_PYTHON_ENV_PATH:-}" &&
    -x "${IDF_PYTHON_ENV_PATH}/bin/python" ]] || return 1
  [[ "$(command -v idf.py 2>/dev/null)" == "${IDF_PATH}/tools/idf.py" ]] || return 1
  (OBBW_IDF_QUIET=1 obbw_idf_require_version) >/dev/null 2>&1
}

# A Python upgrade leaves the SDK clone's export.sh pointing at a python_env
# name that no longer exists while an installed, still-valid environment for
# the same IDF release sits unused in ~/.espressif/python_env. Prefer such an
# environment before export.sh so the documented fallback keeps working.
obbw_idf_select_python_env() {
  local idf_root="$1" version_header marker major minor candidate
  version_header="${idf_root}/components/esp_common/include/esp_idf_version.h"
  [[ -f "$version_header" ]] || return 1
  major="$(awk '$1 == "#define" && $2 == "ESP_IDF_VERSION_MAJOR" { print $3 }' "$version_header")"
  minor="$(awk '$1 == "#define" && $2 == "ESP_IDF_VERSION_MINOR" { print $3 }' "$version_header")"
  [[ -n "$major" && -n "$minor" ]] || return 1
  marker="${major}.${minor}"
  while IFS= read -r candidate; do
    [[ -x "${candidate}/bin/python" ]] || continue
    [[ "$(cat "${candidate}/idf_version.txt" 2>/dev/null)" == "${marker}"* ]] || continue
    export IDF_PYTHON_ENV_PATH="${candidate}"
    return 0
  done < <(find "${HOME}/.espressif/python_env" -maxdepth 1 -type d \
    -name "idf${marker}_py*_env" 2>/dev/null | sort -rV)
  return 1
}

# Read-only diagnosis for ./scripts/dev doctor: report which ESP-IDF checkout
# the build scripts would use, which Python environment the activation would
# end up with, and the venv mismatch case (a Python-upgraded SDK clone whose
# export.sh target no longer exists). Reuses obbw_idf_find and the same python
# environment selection as obbw_idf_source without sourcing export.sh, mutating
# the caller's environment, or running idf.py.
obbw_idf_environment_report() {
  local idf_root selected
  idf_root="$(obbw_idf_find)" || return 1
  printf 'IDF_PATH=%s\n' "${idf_root}"
  if [[ -n "${IDF_PYTHON_ENV_PATH:-}" && -x "${IDF_PYTHON_ENV_PATH}/bin/python" ]]; then
    printf 'IDF_PYTHON_ENV_PATH=%s\n' "${IDF_PYTHON_ENV_PATH}"
    printf 'venv_source=active\n'
    printf 'venv_mismatch=no\n'
    return 0
  fi
  # Selection in a subshell: obbw_idf_select_python_env exports, never prints.
  selected="$(OBBW_IDF_QUIET=1 obbw_idf_select_python_env "${idf_root}" && \
    printf '%s' "${IDF_PYTHON_ENV_PATH-}")" || selected=""
  if [[ -n "${selected}" ]]; then
    printf 'IDF_PYTHON_ENV_PATH=%s\n' "${selected}"
    printf 'venv_source=installed-selection\n'
  else
    printf 'venv_source=export.sh\n'
  fi
  if [[ -n "${IDF_PYTHON_ENV_PATH:-}" && ! -x "${IDF_PYTHON_ENV_PATH}/bin/python" ]]; then
    printf 'venv_mismatch=yes\n'
  else
    printf 'venv_mismatch=no\n'
  fi
  return 0
}

obbw_idf_source() {
  local idf_root

  if [[ -n "${IDF_PYTHON_ENV_PATH:-}" ]]; then
    if obbw_idf_active_valid; then
      if [[ "${OBBW_IDF_QUIET:-}" != "1" ]]; then
        echo "ESP-IDF: ${IDF_PATH} (active environment)"
      fi
      return 0
    fi
    unset IDF_PATH IDF_PYTHON_ENV_PATH ESP_PYTHON ESP_IDF_VERSION
    unset IDF_DEACTIVATE_FILE_PATH IDF_TOOLS_EXPORT_CMD IDF_TOOLS_INSTALL_CMD
    hash -r
  fi
  idf_root="$(obbw_idf_find)" || {
    echo "ESP-IDF not found (idf.py / export.sh)." >&2
    echo "Install ESP-IDF v6.1 and run again:" >&2
    echo "  mkdir -p \"\$HOME/esp\" && cd \"\$HOME/esp\"" >&2
    echo "  git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.1" >&2
    echo "  cd esp-idf-v6.1 && ./install.sh esp32s3 && . ./export.sh" >&2
    exit 127
  }
  unset IDF_PYTHON_ENV_PATH ESP_PYTHON
  # The SDK clone's export.sh derives its python_env name from whatever
  # Python built it; after a host Python upgrade that directory may not
  # exist while an installed environment for the same release does.
  # Selecting it here keeps the documented fallback self-healing.
  if [[ -z "${IDF_PYTHON_ENV_PATH:-}" ||
        ! -x "${IDF_PYTHON_ENV_PATH}/bin/python" ]]; then
    obbw_idf_select_python_env "${idf_root}" || true
  fi
  # shellcheck disable=SC1091
  . "${idf_root}/export.sh" >/dev/null
  command -v idf.py >/dev/null 2>&1 || {
    echo "export.sh from ${idf_root} did not put idf.py on PATH." >&2
    exit 127
  }
  obbw_idf_require_version
  if [[ "${OBBW_IDF_QUIET:-}" != "1" ]]; then
    echo "ESP-IDF: ${idf_root}"
  fi
}

# ESP-IDF 6.1 reports its own esp_wifi/wpa_supplicant cross-includes as CMake
# warnings. Hide only those upstream blocks; preserve every other diagnostic.
obbw_idf_filter_output() {
  awk -v idf="${IDF_PATH}/components/" '
    function emit() {
      external = (index(block, idf "esp_wifi/") || index(block, idf "wpa_supplicant/")) && \
                 index(block, "esp_wifi") && index(block, "wpa_supplicant")
      if (!external) printf "%s", block
      block = ""; capture = 0; fflush()
    }
    capture { block = block $0 ORS; if ($0 == "") emit(); next }
    /CMake Warning at .*tools\/cmake\/component_validation\.cmake:/ {
      capture = 1; block = $0 ORS; next
    }
    { print; fflush() }
    END { if (capture) emit() }
  '
}

# Match the v6.1 release used by CI and the component lock.
obbw_idf_require_version() {
  local ver_line ver
  ver_line="$(idf.py --version 2>/dev/null | head -n1 || true)"
  ver="$(printf '%s' "$ver_line" | sed -n 's/.*v\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')"
  if [[ -z "$ver" ]]; then
    ver="$(printf '%s' "$ver_line" | sed -n 's/.*v\([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')"
  fi
  case "$ver" in
    6.1|6.1.0)
      if [[ "${OBBW_IDF_QUIET:-}" != "1" ]]; then
        echo "ESP-IDF version: v${ver} (required: 6.1.0)"
      fi
      ;;
    *)
      echo "ESP-IDF 6.1.0 is required (CI uses v6.1)." >&2
      echo "Found: ${ver_line:-unknown} (parsed: ${ver:-none})" >&2
      echo "Install or point IDF_PATH at v6.1:" >&2
      echo "  git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.1" >&2
      exit 127
      ;;
  esac
}

# True when extra flags request USB Serial/JTAG at boot.
obbw_idf_jtag_enabled() {
  local tok
  # Word-splitting is intentional: extra flags is a space-separated list.
  # shellcheck disable=SC2086
  set -- ${OPENBREWBYWEIGHT_EXTRA_FLAGS-}
  for tok in "$@"; do
    case "$tok" in
      -DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1|OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1)
        return 0
        ;;
    esac
  done
  return 1
}

# Static analysis defaults predate the per-variant build trees: without an
# explicit --build-dir, prefer the newest variant database instead of the
# legacy per-arch directory that recent builds no longer write.
obbw_idf_default_build_dir() {
  local database newest=""
  for database in "$OBBW_CLI_ROOT"/build-idf/*/compile_commands.json; do
    [[ -f "$database" ]] || continue
    if [[ -z "$newest" || "$database" -nt "$newest" ]]; then
      newest="$database"
    fi
  done
  [[ -n "$newest" ]] || return 1
  printf '%s' "${newest%/*}"
}

# Shared preparation for the static analysis scripts: an explicit --build-dir
# wins, then the newest per-variant tree, then the legacy per-arch directory.
# --output-dir must stay a relative path inside the repository so the callers'
# destructive cleanup can never escape it. Relative paths resolve against
# OBBW_CLI_ROOT. Sets build_dir, build_path, output_dir, output_path, and
# compile_commands. Requires the default output directory as $1; returns 2 for
# an unsafe output directory and 1 when the compilation database is missing.
obbw_idf_static_paths() {
  local default_dir
  if obbw_is_set build_dir; then
    build_dir="$(obbw_get build_dir)"
  elif default_dir="$(obbw_idf_default_build_dir)"; then
    build_dir="$default_dir"
  else
    build_dir="build-idf/$OPENBREWBYWEIGHT_ARCH"
  fi
  if obbw_is_set output_dir; then
    output_dir="$(obbw_get output_dir)"
  else
    output_dir="$1"
  fi
  case "$build_dir" in
    /*) build_path="$build_dir" ;;
    *) build_path="$OBBW_CLI_ROOT/$build_dir" ;;
  esac
  # Keep the destructive cleanup strictly inside the repository.
  case "$output_dir" in
    /*|*'..'*)
      echo "Output directory must be a relative path inside the repository." >&2
      return 2
      ;;
  esac
  output_path="$OBBW_CLI_ROOT/$output_dir"
  compile_commands="$build_path/compile_commands.json"
  if [[ ! -f "$compile_commands" ]]; then
    echo "$compile_commands does not exist." >&2
    echo "Build with --hardware and --machine, then pass its directory with --build-dir." >&2
    return 1
  fi
}

# Shared translation-unit coverage audit for the static analysis scripts:
# recreates the output directory, audits the database against the project
# manifest, and saves the report. Returns the audit's exit status.
obbw_idf_audit_translation_units() {
  rm -rf "$output_path"
  mkdir -p "$output_path"
  python3 "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/audit_compile_commands.py" \
    --database "$compile_commands" | tee "$output_path/translation-units.txt"
  return "${PIPESTATUS[0]}"
}

# Call after openbrewbyweight_resolve_board. Sets IDF_PROJECT, IDF_BUILD_DIR,
# IDF_IMAGE, IDF_ELF, IDF_MAP, IDF_SDKCONFIG, IDF_SDKCONFIG_DEFAULTS.
obbw_idf_resolve_paths() {
  IDF_PROJECT="$OBBW_CLI_ROOT/idf"
  IDF_BUILD_DIR="$OBBW_IDF_BUILD_ROOT/${OPENBREWBYWEIGHT_VARIANT:-$OPENBREWBYWEIGHT_ARCH}"
  IDF_IMAGE="$IDF_BUILD_DIR/${IDF_PROJECT_NAME}.bin"
  IDF_ELF="$IDF_BUILD_DIR/${IDF_PROJECT_NAME}.elf"
  IDF_MAP="$IDF_BUILD_DIR/${IDF_PROJECT_NAME}.map"
  IDF_SDKCONFIG="$IDF_BUILD_DIR/sdkconfig"
  IDF_SDKCONFIG_DEFAULTS="$IDF_PROJECT/sdkconfig.defaults;$IDF_PROJECT/sdkconfig.defaults.nimble;$IDF_PROJECT/sdkconfig.defaults.$OPENBREWBYWEIGHT_ARCH"
  if [[ "${OPENBREWBYWEIGHT_MACHINE_INTEGRATION:-none}" == "linea_micra_cloud" ]]; then
    IDF_SDKCONFIG_DEFAULTS="$IDF_SDKCONFIG_DEFAULTS;$IDF_PROJECT/sdkconfig.defaults.micra"
  fi
  if obbw_idf_jtag_enabled; then
    IDF_SDKCONFIG_DEFAULTS="$IDF_SDKCONFIG_DEFAULTS;$IDF_PROJECT/sdkconfig.defaults.jtag"
  fi
  # The optimization level overlay is generated per build from --o0/--og/--o2/--os
  # (empty means the repository default, -O2). Appended last so the chosen level
  # wins over every earlier default file. It must live outside the build dir:
  # obbw_idf_prepare_set_target wipes that directory and idf.py requires every
  # SDKCONFIG_DEFAULTS entry to exist while set-target runs its first cmake.
  IDF_OPT_OVERLAY="$OBBW_IDF_BUILD_ROOT/.defaults/${OPENBREWBYWEIGHT_VARIANT:-$OPENBREWBYWEIGHT_ARCH}.optlevel"
  IDF_SDKCONFIG_DEFAULTS="$IDF_SDKCONFIG_DEFAULTS;$IDF_OPT_OVERLAY"
}

obbw_idf_py_args() {
  obbw_idf_resolve_paths
  # Optional accelerator: an installed ccache turns full rebuilds after flag or
  # sdkconfig changes into cache hits. IDF_CCACHE_ENABLE=0 opts out.
  if [[ -z "${IDF_CCACHE_ENABLE:-}" ]] && command -v ccache >/dev/null 2>&1; then
    export IDF_CCACHE_ENABLE=1
  fi
  # Exported to the wrapper that sources this helper.
  # shellcheck disable=SC2034
  IDF_PY_ARGS=(
    -C "$IDF_PROJECT"
    -B "$IDF_BUILD_DIR"
    -D "SDKCONFIG=$IDF_SDKCONFIG"
    -D "SDKCONFIG_DEFAULTS=$IDF_SDKCONFIG_DEFAULTS"
    -D "IDF_TARGET=esp32s3"
    -D "OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION=${OPENBREWBYWEIGHT_MACHINE_INTEGRATION:-none}"
  )
}

obbw_idf_probe_in_psram() {
  local addr="$1"
  # ESP32-S3 maps instruction/data external RAM around 0x3C000000.
  # Internal DRAM BSS is typically 0x3FC00000.
  case "$addr" in
    0x3c*|0x3C*) return 0 ;;
  esac
  return 1
}

obbw_idf_require_image() {
  obbw_idf_resolve_paths
  if [[ ! -f "$IDF_IMAGE" ]]; then
    echo "$IDF_IMAGE does not exist." >&2
    echo "Build first with the same --hardware and --machine profiles." >&2
    exit 1
  fi
}

# idf.py skips CMake when CMakeCache.txt exists, so a new
# OPENBREWBYWEIGHT_EXTRA_FLAGS env value would otherwise never reach the compiler.
# Drop the cache when the stamp changes. Do not write the stamp here:
# idf.py set-target runs fullclean, which refuses a non-empty directory that
# is not a CMake build (no CMakeCache.txt). Writing the stamp first made the
# first n16r8 tree fail. Call obbw_idf_commit_extra_flags_stamp after
# set-target. CMake still reads OPENBREWBYWEIGHT_EXTRA_FLAGS from the environment.
obbw_idf_sync_extra_flags() {
  obbw_idf_resolve_paths
  local stamp="$IDF_BUILD_DIR/open_brew_by_weight_extra_flags"
  local new="${OPENBREWBYWEIGHT_EXTRA_FLAGS-}"
  local old=""
  mkdir -p "$IDF_BUILD_DIR"
  if [[ -f "$stamp" ]]; then
    old="$(cat "$stamp")"
  fi
  if [[ "$new" != "$old" ]]; then
    echo "Extra compile flags changed; reconfiguring IDF CMake cache"
    echo "  was: ${old:-(none)}"
    echo "  now: ${new:-(none)}"
    rm -f "$IDF_BUILD_DIR/CMakeCache.txt"
  fi
}

obbw_idf_commit_extra_flags_stamp() {
  obbw_idf_resolve_paths
  mkdir -p "$IDF_BUILD_DIR"
  printf '%s\n' "${OPENBREWBYWEIGHT_EXTRA_FLAGS-}" > "$IDF_BUILD_DIR/open_brew_by_weight_extra_flags"
}

obbw_idf_config_fingerprint() {
  python3 - "$IDF_SDKCONFIG_DEFAULTS" "$IDF_PROJECT/main/Kconfig.projbuild" \
    "$OPENBREWBYWEIGHT_ARCH" "${OPENBREWBYWEIGHT_VARIANT:-$OPENBREWBYWEIGHT_ARCH}" \
    "${OPENBREWBYWEIGHT_MACHINE_INTEGRATION:-none}" "${OPENBREWBYWEIGHT_EXTRA_FLAGS-}" <<'PY'
import hashlib, pathlib, sys
h = hashlib.sha256()
defaults, kconfig, *selectors = sys.argv[1:]
for value in selectors:
    h.update(value.encode() + b'\0')
for name in defaults.split(';') + [kconfig]:
    path = pathlib.Path(name)
    h.update(name.encode() + b'\0' + path.read_bytes() + b'\0')
print(h.hexdigest())
PY
}

obbw_idf_refresh_config() {
  local fingerprint="$1" stamp="$IDF_BUILD_DIR/sdkconfig.inputs.sha256" reason=''
  if [[ "${OBBW_CLI_FORCE_SDKCONFIG_REGENERATE:-0}" == "1" ]]; then
    reason='requested by --force-sdkconfig-regenerate'
  elif [[ -f "$IDF_SDKCONFIG" && ! -f "$stamp" ]]; then
    reason='existing tree has no configuration fingerprint'
  elif [[ -f "$stamp" && "$(cat "$stamp")" != "$fingerprint" ]]; then
    reason='repository configuration inputs changed'
  elif [[ ! -f "$IDF_SDKCONFIG" && -f "$IDF_BUILD_DIR/CMakeCache.txt" ]]; then
    reason='sdkconfig is missing'
  fi
  if [[ -n "$reason" ]]; then
    echo "Regenerating sdkconfig: $reason; local menuconfig choices will be discarded"
    rm -f "$IDF_SDKCONFIG" "$IDF_BUILD_DIR/CMakeCache.txt"
  fi
}

# idf.py aborts when a SDKCONFIG_DEFAULTS entry is missing, and set-target
# fullcleans the tree, so the overlay is re-emitted before and after it runs.
obbw_idf_emit_opt_overlay() {
  obbw_idf_resolve_paths
  mkdir -p "$(dirname "$IDF_OPT_OVERLAY")"
  local content
  content="$(printf '# Generated by scripts/internal/build-idf; do not edit.\n%s=y\n' \
    "${OBBW_IDF_OPT_LEVEL_KCONFIG-CONFIG_COMPILER_OPTIMIZATION_PERF}")"
  if [[ ! -f "$IDF_OPT_OVERLAY" ]] || [[ "$(cat "$IDF_OPT_OVERLAY")" != "$content" ]]; then
    printf '%s\n' "$content" > "$IDF_OPT_OVERLAY"
  fi
}

# idf.py set-target always fullcleans. fullclean is a no-op on an empty dir,
# but FatalError if the dir has leftover files and no CMakeCache.txt.
obbw_idf_prepare_set_target() {
  obbw_idf_resolve_paths
  if [[ -f "$IDF_BUILD_DIR/CMakeCache.txt" ]]; then
    return 0
  fi
  echo "Preparing empty IDF build tree for set-target"
  rm -rf "$IDF_BUILD_DIR"
  mkdir -p "$IDF_BUILD_DIR"
}

# Confirm each extra -D flag actually appears on OpenBrewByWeightNetwork.cpp.
obbw_idf_verify_extra_flags() {
  obbw_idf_resolve_paths
  local flags="${OPENBREWBYWEIGHT_EXTRA_FLAGS-}"
  local cc="$IDF_BUILD_DIR/compile_commands.json"
  [[ -n "$flags" ]] || return 0
  case " $flags " in
    *" -D"*) ;;
    *) return 0 ;;
  esac
  if [[ ! -f "$cc" ]]; then
    echo "$cc does not exist; cannot verify extra compile flags." >&2
    exit 1
  fi
  command -v python3 >/dev/null 2>&1 || {
    echo "python3 not found on PATH (needed to verify extra compile flags)." >&2
    exit 127
  }
  python3 - "$cc" "$flags" <<'PY'
import json, sys
cc_path, flags = sys.argv[1], sys.argv[2]
needles = [tok for tok in flags.split() if tok.startswith("-D")]
if not needles:
    sys.exit(0)
entries = json.load(open(cc_path))
cmd = None
for entry in entries:
    path = entry.get("file") or ""
    if path.endswith("OpenBrewByWeightNetwork.cpp"):
        cmd = entry.get("command") or " ".join(entry.get("arguments") or [])
        break
if not cmd:
    sys.stderr.write(
        "compile_commands.json has no OpenBrewByWeightNetwork.cpp entry.\n")
    sys.exit(1)
missing = [flag for flag in needles if flag not in cmd]
if missing:
    sys.stderr.write(
        "Extra -D flags did not reach the compiler "
        "(IDF CMake cache was probably configured with older flags):\n")
    for flag in missing:
        sys.stderr.write("  %s\n" % flag)
    sys.exit(1)
PY
  echo "compile_commands: extra -D flags present on OpenBrewByWeightNetwork.cpp"
}

# ALLOW_BSS / autostart / g_probe, then the same OTA identity check as
# ./scripts/dev build (image_tag.js).
obbw_idf_verify_production_profile() {
  local arch="${1:-$OPENBREWBYWEIGHT_ARCH}"
  local sdkconfig="${2:-$IDF_SDKCONFIG}"
  local flash_size partition psram_mode selector
  case "$arch" in
    n16r8)
      flash_size=16
      partition=partitions-n16r8.csv
      psram_mode=OCT
      ;;
    *)
      echo "Unsupported production architecture: $arch" >&2
      return 1
      ;;
  esac

  local required=(
    "CONFIG_ESPTOOLPY_FLASHSIZE_${flash_size}MB=y"
    "CONFIG_ESPTOOLPY_FLASHSIZE=\"${flash_size}MB\""
    "CONFIG_PARTITION_TABLE_CUSTOM=y"
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${partition}\""
    "CONFIG_PARTITION_TABLE_FILENAME=\"${partition}\""
    "CONFIG_ESPTOOLPY_FLASHMODE_QIO=y"
    # ESPTOOLPY_FLASHMODE stays "dio": esptool flashes the bootloader in DIO
    # and the bootloader upgrades itself to quad while initializing.
    "CONFIG_ESPTOOLPY_FLASHMODE=\"dio\""
    "CONFIG_ESPTOOLPY_FLASHMODE_VAL=1"
    "CONFIG_ESPTOOLPY_FLASHFREQ_80M=y"
    "CONFIG_ESPTOOLPY_FLASHFREQ=\"80m\""
    "CONFIG_SPIRAM_MODE_${psram_mode}=y"
    "CONFIG_SPIRAM_SPEED_80M=y"
    "CONFIG_SPIRAM_SPEED=80"
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768"
    "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048"
    "CONFIG_MMU_PAGE_SIZE_64KB=y"
    "CONFIG_MMU_PAGE_SIZE=0x10000"
    "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y"
    "CONFIG_BOOTLOADER_WDT_ENABLE=y"
    "CONFIG_BOOTLOADER_WDT_TIME_MS=9000"
    "CONFIG_ESP_INT_WDT=y"
    "CONFIG_ESP_INT_WDT_TIMEOUT_MS=800"
    "CONFIG_ESP_TASK_WDT_EN=y"
    "CONFIG_ESP_TASK_WDT_INIT=y"
    "CONFIG_ESP_TASK_WDT_PANIC=y"
    "CONFIG_ESP_TASK_WDT_TIMEOUT_S=5"
    "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y"
    "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y"
    "CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y"
    "CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=0"
    "CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=y"
    "# CONFIG_RMT_TX_ISR_HANDLER_IN_IRAM is not set"
    "# CONFIG_RMT_RX_ISR_HANDLER_IN_IRAM is not set"
    "# CONFIG_RMT_ENCODER_FUNC_IN_IRAM is not set"
    "# CONFIG_SPI_MASTER_ISR_IN_IRAM is not set"
    "# CONFIG_SPI_SLAVE_ISR_IN_IRAM is not set"
    "# CONFIG_GDMA_CTRL_FUNC_IN_IRAM is not set"
    "# CONFIG_GDMA_ISR_HANDLER_IN_IRAM is not set"
    "CONFIG_LWIP_IPV6=y"
    "CONFIG_LWIP_MAX_SOCKETS=18"
    "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8"
    "CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32"
    "CONFIG_ESP_WIFI_RX_BA_WIN=16"
    "CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16"
    "CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32"
    "CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384"
    "CONFIG_LWIP_TCP_WND_DEFAULT=16384"
    "CONFIG_LWIP_TCP_RECVMBOX_SIZE=16"
    "CONFIG_MDNS_MAX_INTERFACES=2"
    "CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y"
    "CONFIG_BT_CTRL_MODEM_SLEEP=y"
    "CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y"
  )
  required+=("CONFIG_SPIRAM_XIP_FROM_PSRAM=y"
             "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y"
             "CONFIG_MDNS_TASK_CREATE_FROM_INTERNAL=y")
  # Admin unlock and remote machine control come only from this run's checked
  # -D defines, never from a retained menuconfig choice; the console follows
  # --jtag, so a release image always has it off.
  required+=("# CONFIG_OPEN_BREW_BY_WEIGHT_DEVELOPMENT is not set"
    "# CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL is not set")
  if obbw_idf_jtag_enabled; then
    required+=("CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1" "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y")
  else
    required+=("CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=0" "CONFIG_ESP_CONSOLE_NONE=y")
  fi
  if [[ "${OPENBREWBYWEIGHT_MACHINE_INTEGRATION:-none}" == "linea_micra_cloud" ]]; then
    required+=("CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y"
      "CONFIG_MBEDTLS_DYNAMIC_BUFFER=y"
      "CONFIG_WS_BUFFER_SIZE=4096")
  elif grep -Fqx 'CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y' "$sdkconfig"; then
    echo "Production profile mismatch: unexpected Micra TLS allocator" >&2
    return 1
  fi
  for selector in "${required[@]}"; do
    if ! grep -Fqx "$selector" "$sdkconfig"; then
      echo "Production profile mismatch for $arch: expected $selector" >&2
      return 1
    fi
  done
  if grep -Fqx 'CONFIG_SPIRAM_MODE_QUAD=y' "$sdkconfig"; then
    echo "Production profile mismatch for $arch: unexpected QUAD PSRAM mode" >&2
    return 1
  fi
  echo "sdkconfig: verified exact $arch flash/PSRAM, partition, MMU, rollback, watchdog, panic, GPTimer, IRAM placement, console, and build-option profile"
}

obbw_idf_verify_firmware() {
  obbw_idf_resolve_paths

  if [[ ! -f "$IDF_SDKCONFIG" ]]; then
    echo "$IDF_SDKCONFIG does not exist; the IDF build left no sdkconfig." >&2
    exit 1
  fi
  obbw_idf_verify_production_profile || exit 1
  if ! grep -q '^CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y$' "$IDF_SDKCONFIG"; then
    echo "ALLOW_BSS is not enabled in $IDF_SDKCONFIG." >&2
    grep 'SPIRAM_ALLOW_BSS' "$IDF_SDKCONFIG" >&2 || true
    exit 1
  fi
  echo "sdkconfig: CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y"

  if ! grep -q '^CONFIG_SPIRAM_USE_MALLOC=y$' "$IDF_SDKCONFIG"; then
    echo "CONFIG_SPIRAM_USE_MALLOC is not enabled in $IDF_SDKCONFIG." >&2
    exit 1
  fi
  echo "sdkconfig: CONFIG_SPIRAM_USE_MALLOC=y"

  if ! grep -q '^CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y$' "$IDF_SDKCONFIG" ||
     ! grep -q '^CONFIG_LOG_MAXIMUM_LEVEL=4$' "$IDF_SDKCONFIG"; then
    echo "ESP-IDF log maximum must be DEBUG so the runtime selector can emit INFO/DEBUG." >&2
    grep 'CONFIG_LOG_MAXIMUM' "$IDF_SDKCONFIG" >&2 || true
    exit 1
  fi
  echo "sdkconfig: CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y"

  if ! grep -q '^CONFIG_AUTOSTART_ARDUINO=y$' "$IDF_SDKCONFIG"; then
    echo "CONFIG_AUTOSTART_ARDUINO is not enabled in $IDF_SDKCONFIG." >&2
    exit 1
  fi
  echo "sdkconfig: CONFIG_AUTOSTART_ARDUINO=y"

  # Default builds must keep the measured -O2 profile; explicit --o0/--og/--os
  # runs verify their requested level instead. Only one choice can be =y.
  local want_opt="${OBBW_IDF_OPT_LEVEL_KCONFIG-CONFIG_COMPILER_OPTIMIZATION_PERF}"
  if ! grep -q "^${want_opt}=y$" "$IDF_SDKCONFIG"; then
    echo "${want_opt} is not enabled in $IDF_SDKCONFIG." >&2
    echo "Delete that sdkconfig (or set the matching level in menuconfig) and rebuild." >&2
    grep 'COMPILER_OPTIMIZATION' "$IDF_SDKCONFIG" >&2 || true
    exit 1
  fi
  echo "sdkconfig: ${want_opt}=y"

  local core0_key
  for core0_key in CONFIG_ARDUINO_EVENT_RUNNING_CORE=0 \
    CONFIG_FREERTOS_TIMER_TASK_AFFINITY_CPU0=y; do
    if ! grep -q "^${core0_key}$" "$IDF_SDKCONFIG"; then
      echo "Core 1 is reserved for control: $IDF_SDKCONFIG lacks ${core0_key}." >&2
      exit 1
    fi
  done

  if ! grep -q '^CONFIG_BT_NIMBLE_ENABLED=y$' "$IDF_SDKCONFIG" ||
     grep -q '^CONFIG_BT_CONTROLLER_ONLY=y$' "$IDF_SDKCONFIG"; then
    echo "Native NimBLE host configuration is inconsistent in $IDF_SDKCONFIG." >&2
    grep -E 'CONFIG_(BT_NIMBLE_ENABLED|BT_CONTROLLER_ONLY)' "$IDF_SDKCONFIG" >&2 || true
    exit 1
  fi
  if ! grep -q '^CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y$' "$IDF_SDKCONFIG"; then
      echo "Production NimBLE must use the physically qualified external allocator." >&2
      exit 1
  fi
  local unused_service
  for unused_service in PROX ANS CTS HTP IPSS TPS IAS LLS SPS HR BAS DIS; do
    if grep -q "^CONFIG_BT_NIMBLE_${unused_service}_SERVICE=y$" "$IDF_SDKCONFIG"; then
      echo "Unused NimBLE ${unused_service} service is enabled in $IDF_SDKCONFIG." >&2
      exit 1
    fi
  done
  for unused_service in DTM_MODE_TEST SM_SIGN_CNT CPFD_CAFD; do
    if grep -q "^CONFIG_BT_NIMBLE_${unused_service}=y$" "$IDF_SDKCONFIG"; then
      echo "Unused NimBLE ${unused_service} feature is enabled in $IDF_SDKCONFIG." >&2
      exit 1
    fi
  done
  if ! grep -q '^CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096$' "$IDF_SDKCONFIG"; then
    echo "NimBLE host stack must remain at the qualified size (4096 bytes)." >&2
    exit 1
  fi
  if [[ -f "$IDF_MAP" ]] && grep -q 'libArduinoBLE.a(' "$IDF_MAP"; then
    echo "ArduinoBLE objects leaked into the production firmware map." >&2
    exit 1
  fi
  echo "BLE backend: native NimBLE host (external allocator); no ArduinoBLE objects in ELF map"

  if [[ ! -f "$IDF_ELF" ]]; then
    echo "$IDF_ELF does not exist." >&2
    exit 1
  fi
  command -v xtensa-esp32s3-elf-nm >/dev/null 2>&1 || {
    echo "xtensa-esp32s3-elf-nm not found (did you run export.sh?)." >&2
    exit 127
  }

  local nm_line addr ble_in_use_line
  ble_in_use_line="$(xtensa-esp32s3-elf-nm "$IDF_ELF" | grep -E '[[:space:]]T[[:space:]]+bleInUse$' || true)"
  if [[ -z "$ble_in_use_line" ]]; then
    echo "ELF does not contain the strong bleInUse override required to retain BLE controller memory." >&2
    exit 1
  fi
  echo "BLE memory retention: $ble_in_use_line"

  nm_line="$(xtensa-esp32s3-elf-nm "$IDF_ELF" | grep -E '[[:space:]]g_probe$|[[:space:]]_ZL[0-9]+g_probe$' || true)"
  if [[ -z "$nm_line" ]]; then
    echo "nm did not find g_probe in $IDF_ELF." >&2
    exit 1
  fi
  addr="0x$(printf '%s' "$nm_line" | awk '{print $1}')"
  echo "g_probe: $nm_line"

  if obbw_idf_probe_in_psram "$addr"; then
    echo "BSS probe in PSRAM ($addr). ALLOW_BSS works with this toolchain."
  else
    echo "BSS probe is NOT in PSRAM ($addr). Expected 0x3c.... Internal DRAM is 0x3fc....." >&2
    if [[ -f "$IDF_MAP" ]]; then
      echo ".map excerpt:" >&2
      grep -n -A2 -E 'g_probe|ext_ram\.bss|extern_ram_seg' "$IDF_MAP" | head -40 >&2 || true
    fi
    exit 1
  fi

  local symbol
  for symbol in activationStores persistedSettings debugLog; do
    nm_line="$(xtensa-esp32s3-elf-nm "$IDF_ELF" | grep -E "[[:space:]]${symbol}$" || true)"
    if [[ -z "$nm_line" ]]; then
      echo "nm did not find ${symbol} in $IDF_ELF." >&2
      exit 1
    fi
    addr="0x$(printf '%s' "$nm_line" | awk '{print $1}')"
    echo "${symbol}: $nm_line"
    if ! obbw_idf_probe_in_psram "$addr"; then
      echo "${symbol} is NOT in PSRAM ($addr). Expected 0x3c.... Internal DRAM is 0x3fc....." >&2
      exit 1
    fi
  done

  nm_line="$(xtensa-esp32s3-elf-nm "$IDF_ELF" | grep -E '[[:space:]][Tt][[:space:]]openBrewByWeightScaleLog$' || true)"
  if [[ -z "$nm_line" ]]; then
    echo "The final ELF has no strong openBrewByWeightScaleLog bridge definition." >&2
    exit 1
  fi
  echo "EspressoScaleBLE log bridge: $nm_line"

  if [[ ! -f "$IDF_IMAGE" ]]; then
    echo "$IDF_IMAGE does not exist, so the OTA marker could not be verified." >&2
    echo "Do not upload an unverified image over Wi-Fi." >&2
    exit 1
  fi
  echo "Image OTA identity:"
  local identity_args=(--expect-arch "$OPENBREWBYWEIGHT_ARCH")
  if [[ -n "${OPENBREWBYWEIGHT_HARDWARE_COMPAT:-}" ]]; then
    identity_args+=(--expect-hardware "$OPENBREWBYWEIGHT_HARDWARE_COMPAT"
                    --expect-machine "$OPENBREWBYWEIGHT_MACHINE_COMPAT")
  fi
  node "$OBBW_CLI_ROOT/scripts/image_tag.js" "$IDF_IMAGE" "${identity_args[@]}"

  obbw_idf_verify_extra_flags
}
