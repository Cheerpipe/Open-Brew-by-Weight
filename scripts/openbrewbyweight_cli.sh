#!/usr/bin/env bash
# Shared named-flag front end for the Open Brew by Weight developer scripts.
# Source from other scripts; do not execute this file directly.
#
# Build profiles: --release (the default) compiles with neither the admin
# unlock define nor the USB Serial/JTAG console; --development means
# --no-auth-admin --jtag. The profiles are mutually exclusive; --jtag and
# --no-auth-admin also work individually on top of a release profile.
# Except for the per-run keys listed in OBBW_CLI_TRANSIENT_KEYS, every value
# comes from:
#   1. a named flag on the command line
#   2. the matching environment variable
#   3. the hidden store file .openbrewbyweight at the repository root
#   4. an interactive prompt for the keys the command requires
#      (Enter accepts the suggestion shown in brackets)
#
# A required serial port whose device node is missing is treated like an
# unset key: the scripts prompt and suggest the first live USB-CDC path.
#
# Per-run keys use flag, environment, then their default (`en` for the WebUI
# language) and are never stored. After a successful resolve the other values
# are merged into .openbrewbyweight.
# The device password is never loaded, suggested, or persisted (use the hidden
# prompt or environment each run; the public dev facade also supports stdin).
#
# Written for bash 3.2 (the system bash on macOS): no associative arrays,
# no ${var,,} case conversion.

OBBW_CLI_KEYS="port arch speed host password flags image build_dir output_dir webui_language hardware_config machine_config"
OBBW_CLI_SECRET_KEYS="password"
# Extra compiler flags offered when the prompt for --flags is answered with Enter.
OBBW_CLI_DEFAULT_FLAGS='-Werror=deprecated-copy'
# Per-run values never touch the store: a stale build_dir would silently point
# an analysis at the wrong architecture, and stale compile flags could ship
# admin unlock or remote machine control in a later release image.
OBBW_CLI_TRANSIENT_KEYS="flags image build_dir output_dir webui_language hardware_config machine_config"

if [[ -z "${OBBW_CLI_ROOT:-}" ]]; then
  OBBW_CLI_ROOT="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
fi
OBBW_CLI_STORE="$OBBW_CLI_ROOT/.openbrewbyweight"
# Analysis builds (warnings-idf) point this at their own tree so toggling
# analysis-only flags never reconfigures the gate's variant builds.
OBBW_IDF_BUILD_ROOT="${OPENBREWBYWEIGHT_IDF_BUILD_ROOT:-$OBBW_CLI_ROOT/build-idf}"

obbw_upper() {
  printf '%s' "$1" | tr '[:lower:]' '[:upper:]'
}

obbw_is_secret() {
  case " $OBBW_CLI_SECRET_KEYS " in
    *" $1 "*) return 0 ;;
  esac
  return 1
}

obbw_is_transient() {
  case " $OBBW_CLI_TRANSIENT_KEYS " in
    *" $1 "*) return 0 ;;
  esac
  return 1
}

obbw_set() {
  eval "OBBW_$(obbw_upper "$1")=\$2"
}

obbw_get() {
  eval "printf '%s' \"\${OBBW_$(obbw_upper "$1")-}\""
}

obbw_origin_set() {
  eval "OBBW_ORIGIN_$(obbw_upper "$1")=\$2"
}

obbw_origin() {
  eval "printf '%s' \"\${OBBW_ORIGIN_$(obbw_upper "$1")-}\""
}

# A key counts as set when it has an origin, so an explicitly empty --flags
# is distinguishable from "never provided".
obbw_is_set() {
  [[ -n "$(obbw_origin "$1")" ]]
}

obbw_put() {
  obbw_set "$1" "$2"
  obbw_origin_set "$1" "$3"
}

obbw_cli_forget() {
  obbw_set "$1" ""
  obbw_origin_set "$1" ""
}

# True when the path is a present device node (not merely a non-empty string).
obbw_port_exists() {
  [[ -n "$1" && -e "$1" ]]
}

obbw_cli_reset() {
  local key
  OBBW_CLI_YES=0
  OBBW_CLI_WAIT_FOR_CONFIRMATION=0
  OBBW_CLI_NO_CHECK=0
  OBBW_CLI_ERASE_ALL=0
  OBBW_CLI_DISCARD_OTA_SESSION=0
  OBBW_CLI_RELEASE=0
  OBBW_CLI_DEVELOPMENT=0
  OBBW_CLI_NO_AUTH_ADMIN=0
  OBBW_CLI_JTAG=0
  OBBW_CLI_FORCE_SDKCONFIG_REGENERATE=0
  for key in $OBBW_CLI_KEYS; do
    obbw_set "$key" ""
    obbw_origin_set "$key" ""
  done
}

# True when the parsed arguments asked for the command's help.
obbw_cli_help_requested() {
  [[ "$OBBW_CLI_HELP_REQUESTED" == "1" ]]
}

obbw_env_name() {
  case "$1" in
    port) printf 'OPENBREWBYWEIGHT_PORT' ;;
    arch) printf 'OPENBREWBYWEIGHT_ARCH' ;;
    speed) printf 'OPENBREWBYWEIGHT_SPEED' ;;
    host) printf 'OPENBREWBYWEIGHT_HOST' ;;
    password) printf 'OPENBREWBYWEIGHT_DEVICE_PASSWORD' ;;
    flags) printf 'OPENBREWBYWEIGHT_FLAGS' ;;
    image) printf 'OPENBREWBYWEIGHT_IMAGE' ;;
    build_dir) printf 'OPENBREWBYWEIGHT_BUILD_DIR_OVERRIDE' ;;
    output_dir) printf 'OPENBREWBYWEIGHT_OUTPUT_DIR' ;;
    webui_language) printf 'OPENBREWBYWEIGHT_WEBUI_LANGUAGE' ;;
    hardware_config) printf 'OPENBREWBYWEIGHT_HARDWARE' ;;
    machine_config) printf 'OPENBREWBYWEIGHT_MACHINE' ;;
  esac
}

obbw_flag_help() {
  cat <<'EOF'
Named parameters (long and short):
  -p, --port <path>        Serial port, e.g. /dev/cu.usbmodem2101
  -a, --arch <arch>        Architecture for static analysis (n16r8)
  -s, --speed <baud>       Serial monitor baud rate, e.g. 115200
  -H, --host <ip|name>     Controller address for OTA
  -f, --flags "<flags>"    Extra compile flags (single string; not persisted)
  -i, --image <path>       Firmware .bin to flash or upload (not persisted)
  -b, --build-dir <path>   Build directory (static analysis only; not persisted)
  -o, --output-dir <path>  Reports directory (static analysis only; not persisted)
      --webui-language <code>
                           Compile-time WebUI language (default: en; not persisted)
      --hardware <id|json> Exact hardware profile ID or JSON path (not persisted)
      --machine <id|json>  Exact machine profile ID or JSON path (not persisted)
      --development        Build profile: admin unlock (--no-auth-admin) plus
                           the USB Serial/JTAG console (--jtag)
      --release            Build profile: no admin unlock and no JTAG console
                           (the default); given explicitly, it also requires a
                           clean Git tree, untracked files included
      --no-auth-admin      Build-only admin unlock without a device-password
                           session (adds -DOPEN_BREW_BY_WEIGHT_DEVELOPMENT=1 to the
                           build flags; not persisted)
      --jtag               Enable the USB Serial/JTAG console at boot (adds
                           -DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1 to the build flags)
      --o0|--og|--o2|--os  Firmware optimization level for this build (build
                           only; exclusive; not persisted). ESP-IDF Kconfig
                           levels: -O0, -Og, -O2, -Os (default). The saved
                           profile keeps -Os unless a level is passed again.
                           -O1/-O3 are not ESP-IDF levels and are rejected.
      --force-sdkconfig-regenerate
                           Recreate build configuration from repository defaults
                           (build only; discards local menuconfig choices)
      --yes                Assume yes for every OTA prompt (commit, and
                           discarding a different remote image)
      --wait-for-confirmation
                           Wait for verified boot after OTA commit
      --no-check           Skip local image verification before USB flashing;
                           transfer existing output as-is (OTA rejects it)
      --erase-all          Erase the complete chip before a full USB flash
                           (destroys firmware, settings, Wi-Fi and history)
      --discard-ota-session
                           Discard a different partial OTA image without
                           asking
  -h, --help               Show this help

Each parameter comes from the flag, its environment variable, the .openbrewbyweight
file at the repository root, or a prompt. The WebUI language is the exception:
it uses --webui-language, OPENBREWBYWEIGHT_WEBUI_LANGUAGE, then en, without prompting
or persistence. At other prompts, Enter accepts the suggested value in brackets.
A missing or non-existent --port is prompted like the device password, with the
first detected USB-CDC device suggested.
After resolving parameters, the values used are saved to .openbrewbyweight
(the device password is never saved or suggested).
EOF
}

OBBW_CLI_HELP_REQUESTED=0
OBBW_CLI_OPT_LEVEL=""

obbw_cli_die() {
  printf '%s\n' "$1" >&2
}

# Parses named flags. Positional arguments are rejected because these
# scripts used to take them.
obbw_cli_parse() {
  local key value
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --yes)
        OBBW_CLI_YES=1
        shift
        continue
        ;;
      --yes=*)
        printf '%s\n' '--yes does not take a value.' >&2
        return 2
        ;;
      --wait-for-confirmation)
        OBBW_CLI_WAIT_FOR_CONFIRMATION=1
        shift
        continue
        ;;
      --wait-for-confirmation=*)
        printf '%s\n' '--wait-for-confirmation does not take a value.' >&2
        return 2
        ;;
      --force|--force=*)
        printf '%s\n' '--force was removed; use --yes and/or --wait-for-confirmation.' >&2
        return 2
        ;;
      --no-check)
        OBBW_CLI_NO_CHECK=1
        shift
        continue
        ;;
      --no-check=*)
        printf '%s\n' '--no-check does not take a value.' >&2
        return 2
        ;;
      --erase-all)
        OBBW_CLI_ERASE_ALL=1
        shift
        continue
        ;;
      --erase-all=*)
        printf '%s\n' '--erase-all does not take a value.' >&2
        return 2
        ;;
      --discard-ota-session)
        OBBW_CLI_DISCARD_OTA_SESSION=1
        shift
        continue
        ;;
      --discard-ota-session=*)
        printf '%s\n' '--discard-ota-session does not take a value.' >&2
        return 2
        ;;
      --development)
        OBBW_CLI_DEVELOPMENT=1
        shift
        continue
        ;;
      --development=*)
        printf '%s\n' '--development does not take a value.' >&2
        return 2
        ;;
      --release)
        OBBW_CLI_RELEASE=1
        shift
        continue
        ;;
      --release=*)
        printf '%s\n' '--release does not take a value.' >&2
        return 2
        ;;
      --no-auth-admin)
        OBBW_CLI_NO_AUTH_ADMIN=1
        shift
        continue
        ;;
      --no-auth-admin=*)
        printf '%s\n' '--no-auth-admin does not take a value.' >&2
        return 2
        ;;
      --jtag)
        OBBW_CLI_JTAG=1
        shift
        continue
        ;;
      --jtag=*)
        printf '%s\n' '--jtag does not take a value.' >&2
        return 2
        ;;
      --force-sdkconfig-regenerate)
        OBBW_CLI_FORCE_SDKCONFIG_REGENERATE=1
        shift
        continue
        ;;
      --force-sdkconfig-regenerate=*)
        printf '%s\n' '--force-sdkconfig-regenerate does not take a value.' >&2
        return 2
        ;;
      --o0|--og|--o2|--os)
        if [[ -n "$OBBW_CLI_OPT_LEVEL" && "$OBBW_CLI_OPT_LEVEL" != "$1" ]]; then
          printf 'Optimization levels are exclusive: %s already selected.\n' \
            "$OBBW_CLI_OPT_LEVEL" >&2
          return 2
        fi
        OBBW_CLI_OPT_LEVEL="$1"
        shift
        continue
        ;;
      --o1|--o3|--o1=*|--o3=*|--oz|--oz=*|--Ofast|--Ofast=*)
        printf '%s\n' "$1 is not an ESP-IDF Kconfig optimization level." >&2
        printf '%s\n' 'Supported: --o0 (-O0), --og (-Og), --o2 (-O2), --os (-Os).' >&2
        return 2
        ;;
      --*=*)
        key="${1%%=*}"
        value="${1#*=}"
        set -- "$key" "$value" "${@:2}"
        continue
        ;;
      -p|--port) key="port" ;;
      -a|--arch) key="arch" ;;
      -s|--speed) key="speed" ;;
      -H|--host) key="host" ;;
      -f|--flags) key="flags" ;;
      -i|--image) key="image" ;;
      -b|--build-dir) key="build_dir" ;;
      -o|--output-dir) key="output_dir" ;;
      --webui-language) key="webui_language" ;;
      --hardware) key="hardware_config" ;;
      --machine) key="machine_config" ;;
      -h|--help|help)
        OBBW_CLI_HELP_REQUESTED=1
        return 0
        ;;
      -*)
        printf 'Unknown option: %s\n\n' "$1" >&2
        obbw_flag_help >&2
        return 2
        ;;
      *)
        printf 'Positional argument not allowed: %s\n' "$1" >&2
        printf 'Scripts now use named parameters.\n' >&2
        printf 'For example: --port /dev/cu.usbmodem2101 --speed 115200\n\n' >&2
        obbw_flag_help >&2
        return 2
        ;;
    esac
    shift
    if [[ $# -eq 0 ]]; then
      printf 'Missing value for --%s.\n' "$key" >&2
      return 2
    fi
    value="$1"
    shift
    # --flags is one logical string, but people often pass several -D/-W tokens
    # unquoted. Swallow consecutive compiler tokens so the second -D is not
    # treated as an unknown script option.
    if [[ "$key" == "flags" ]]; then
      while [[ $# -gt 0 ]]; do
        case "$1" in
          -D*|-W*|-I*)
            value="$value $1"
            shift
            ;;
          [A-Za-z_]*=*)
            value="$value $1"
            shift
            ;;
          *) break ;;
        esac
      done
    fi
    obbw_put "$key" "$value" "cli"
  done
  if [[ "$OBBW_CLI_DEVELOPMENT" == "1" && "$OBBW_CLI_RELEASE" == "1" ]]; then
    printf '%s\n' '--development and --release are mutually exclusive build profiles.' >&2
    return 2
  fi
  # --development selects the debug profile: admin unlock plus the JTAG
  # console. --release is the explicit form of the default and keeps any
  # individually requested --jtag/--no-auth-admin switches.
  if [[ "$OBBW_CLI_DEVELOPMENT" == "1" ]]; then
    OBBW_CLI_NO_AUTH_ADMIN=1
    OBBW_CLI_JTAG=1
  fi
  return 0
}

# g++ treats a bare MACRO=value token as a linker input. Prefix -D so
# OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=1 is a define, not a missing file.
obbw_normalize_compiler_flags() {
  local raw="$1" out="" tok
  local toks=()
  # Split on whitespace without glob expansion.
  read -r -d '' -a toks <<<"$raw" || true
  set -- ${toks[@]+"${toks[@]}"}
  for tok in "$@"; do
    case "$tok" in
      [A-Za-z_]*=*) tok="-D$tok" ;;
    esac
    out="$out $tok"
  done
  printf '%s' "${out# }"
}

obbw_cli_apply_env() {
  local key name value
  for key in $OBBW_CLI_KEYS; do
    obbw_is_set "$key" && continue
    name="$(obbw_env_name "$key")"
    [[ -n "$name" ]] || continue
    eval "value=\${$name-}"
    # Only an exported, non-empty value counts; an empty export is ignored so
    # a stale `export OPENBREWBYWEIGHT_HOST=` cannot silently win over the store.
    [[ -n "$value" ]] || continue
    obbw_put "$key" "$value" "env"
  done
}

# Prints the compile flags for this run. Flags and profile switches are never
# stored: --development means --no-auth-admin plus --jtag, --release means
# neither, and --no-auth-admin and --jtag also work individually. A define that
# turns on admin unlock or the JTAG console needs its switch, and remote machine
# control is accepted only from this run's command line or prompt, never from
# the environment, so no leftover value can reach a release image.
obbw_cli_effective_flags() {
  local raw token found=0 jtag=0
  raw="$(obbw_get flags)"
  local toks=()
  # Split on whitespace without glob expansion.
  read -r -d '' -a toks <<<"$raw" || true
  set -- ${toks[@]+"${toks[@]}"}
  for token in "$@"; do
    case "${token#-D}" in
      OPEN_BREW_BY_WEIGHT_DEVELOPMENT=0)
        if [[ "$OBBW_CLI_NO_AUTH_ADMIN" == "1" ]]; then
          obbw_cli_die "--no-auth-admin conflicts with OPEN_BREW_BY_WEIGHT_DEVELOPMENT=0 in --flags."
          return 2
        fi
        ;;
      OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=0)
        if [[ "$OBBW_CLI_JTAG" == "1" ]]; then
          obbw_cli_die "--jtag conflicts with OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=0 in --flags."
          return 2
        fi
        ;;
      OPEN_BREW_BY_WEIGHT_DEVELOPMENT=1) found=1 ;;
      OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1) jtag=1 ;;
      OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=0) ;;
      OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=1)
        if [[ "$(obbw_origin flags)" == "env" ]]; then
          obbw_cli_die "Remote machine control is a per-build opt-in: pass it with --flags, not $(obbw_env_name flags)."
          return 2
        fi
        ;;
      OPEN_BREW_BY_WEIGHT_DEVELOPMENT | OPEN_BREW_BY_WEIGHT_DEVELOPMENT=* | \
        OPEN_BREW_BY_WEIGHT_ENABLE_JTAG | OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=* | \
        OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL | OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=*)
        obbw_cli_die "${token#-D} in --flags must be =0 or =1."
        return 2
        ;;
    esac
  done
  if [[ "$found" == "1" && "$OBBW_CLI_NO_AUTH_ADMIN" != "1" ]]; then
    obbw_cli_die "OPEN_BREW_BY_WEIGHT_DEVELOPMENT=1 in --flags needs --no-auth-admin or --development."
    return 2
  fi
  if [[ "$jtag" == "1" && "$OBBW_CLI_JTAG" != "1" ]]; then
    obbw_cli_die "OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1 in --flags needs --jtag or --development."
    return 2
  fi
  if [[ "$OBBW_CLI_JTAG" == "1" && "$jtag" != "1" ]]; then
    raw="${raw:+$raw }-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1"
  fi
  if [[ "$OBBW_CLI_NO_AUTH_ADMIN" != "1" || "$found" == "1" ]]; then
    printf '%s' "$raw"
  else
    printf '%s' "${raw:+$raw }-DOPEN_BREW_BY_WEIGHT_DEVELOPMENT=1"
  fi
}

# Resolves the selected pair once and exports the target/build identity used by
# build, flash, OTA, and combined workflows. The caller must resolve both keys.
obbw_cli_resolve_profiles() {
  local flags="${1:-}" mode="${2:-write}" resolved_arch
  local resolver_flags=()
  [[ "$mode" == "check" ]] && resolver_flags+=(--check-only)
  OBBW_PROFILE_OUTPUT="$(python3 "$OBBW_CLI_ROOT/scripts/resolve_build_profiles.py" \
    --hardware "$(obbw_get hardware_config)" --machine "$(obbw_get machine_config)" \
    --flags="$flags" --output-root "$OBBW_IDF_BUILD_ROOT" \
    ${resolver_flags[@]+"${resolver_flags[@]}"})" || return $?
  resolved_arch="$(printf '%s\n' "$OBBW_PROFILE_OUTPUT" | sed -n 's/^arch=//p')"
  OPENBREWBYWEIGHT_VARIANT="$(printf '%s\n' "$OBBW_PROFILE_OUTPUT" | sed -n 's/^variant=//p')"
  OPENBREWBYWEIGHT_HARDWARE_COMPAT="$(printf '%s\n' "$OBBW_PROFILE_OUTPUT" | sed -n 's/^hardware_compat=//p')"
  OPENBREWBYWEIGHT_MACHINE_COMPAT="$(printf '%s\n' "$OBBW_PROFILE_OUTPUT" | sed -n 's/^machine_compat=//p')"
  OPENBREWBYWEIGHT_MACHINE_INTEGRATION="$(printf '%s\n' "$OBBW_PROFILE_OUTPUT" | sed -n 's/^machine_integration=//p')"
  OPENBREWBYWEIGHT_GENERATED_DIR="$(printf '%s\n' "$OBBW_PROFILE_OUTPUT" | sed -n 's/^generated_dir=//p')"
  if [[ -z "$resolved_arch" || -z "$OPENBREWBYWEIGHT_VARIANT" ||
    -z "$OPENBREWBYWEIGHT_HARDWARE_COMPAT" || -z "$OPENBREWBYWEIGHT_MACHINE_COMPAT" ||
    -z "$OPENBREWBYWEIGHT_MACHINE_INTEGRATION" ||
    -z "$OPENBREWBYWEIGHT_GENERATED_DIR" ]]; then
    obbw_cli_die "Profile resolver returned incomplete build metadata."
    return 2
  fi
  if obbw_is_set arch && { [[ "$(obbw_origin arch)" == "cli" ]] ||
                         [[ "$(obbw_origin arch)" == "env" ]]; } &&
     [[ "$(obbw_get arch)" != "$resolved_arch" ]]; then
    obbw_cli_die "--arch $(obbw_get arch) conflicts with profile target $resolved_arch."
    return 2
  fi
  obbw_put arch "$resolved_arch" profile
  export OPENBREWBYWEIGHT_VARIANT OPENBREWBYWEIGHT_MACHINE_INTEGRATION OPENBREWBYWEIGHT_GENERATED_DIR
}

obbw_cli_load_store() {
  [[ -f "$OBBW_CLI_STORE" ]] || return 0
  local line key value
  while IFS= read -r line || [[ -n "$line" ]]; do
    case "$line" in
      ''|'#'*) continue ;;
      *'='*) ;;
      *) continue ;;
    esac
    key="${line%%=*}"
    value="${line#*=}"
    case " $OBBW_CLI_KEYS " in
      *" $key "*) ;;
      *) continue ;;
    esac
    obbw_is_transient "$key" && continue
    obbw_is_secret "$key" && continue
    obbw_is_set "$key" && continue
    obbw_put "$key" "$value" "store"
  done < "$OBBW_CLI_STORE"
  return 0
}

# Reads one key from the store file without touching session state.
obbw_cli_store_value() {
  local want="$1" line key value
  [[ -f "$OBBW_CLI_STORE" ]] || return 1
  while IFS= read -r line || [[ -n "$line" ]]; do
    case "$line" in
      ''|'#'*) continue ;;
      *'='*) ;;
      *) continue ;;
    esac
    key="${line%%=*}"
    [[ "$key" == "$want" ]] || continue
    value="${line#*=}"
    printf '%s' "$value"
    return 0
  done < "$OBBW_CLI_STORE"
  return 1
}

obbw_display_value() {
  local key="$1" value
  value="$(obbw_get "$key")"
  if obbw_is_secret "$key"; then
    if [[ -n "$value" ]]; then
      printf '********'
    else
      printf '(empty)'
    fi
    return 0
  fi
  if [[ -z "$value" ]]; then
    printf '(empty)'
  else
    printf '%s' "$value"
  fi
}

# Prints which values come from where, and which ones are still missing.
obbw_cli_summary() {
  local wanted="$1" missing="$2"
  local key line_cli="" line_env="" line_store="" line_default=""
  for key in $wanted; do
    obbw_is_set "$key" || continue
    case "$(obbw_origin "$key")" in
      cli) line_cli="$line_cli $key=$(obbw_display_value "$key")" ;;
      env) line_env="$line_env $key=$(obbw_display_value "$key")" ;;
      store) line_store="$line_store $key=$(obbw_display_value "$key")" ;;
      default) line_default="$line_default $key=$(obbw_display_value "$key")" ;;
    esac
  done
  [[ -n "$line_cli" ]] && printf 'From CLI:                %s\n' "${line_cli# }"
  [[ -n "$line_env" ]] && printf 'From environment:        %s\n' "${line_env# }"
  [[ -n "$line_store" ]] && printf 'Stored (.openbrewbyweight):  %s\n' "$line_store"
  [[ -n "$line_default" ]] && printf 'Defaults:                %s\n' "${line_default# }"
  if [[ -n "$missing" ]]; then
    printf 'Missing:                 %s\n' "$(printf '%s' "$missing" | tr ' ' ',' | sed 's/,/, /g')"
  fi
  return 0
}

obbw_can_prompt() {
  [[ -z "${OPENBREWBYWEIGHT_NONINTERACTIVE:-}" ]] &&
    [[ -r /dev/tty ]] && [[ -w /dev/tty ]]
}

obbw_prompt_text() {
  case "$1" in
    port) printf 'Serial port' ;;
    arch) printf 'Architecture (n16r8)' ;;
    speed) printf 'Monitor baud rate' ;;
    host) printf 'Controller address (IP or hostname)' ;;
    password) printf 'Device password' ;;
    flags) printf 'Extra compile flags' ;;
    hardware_config) printf 'Hardware profile JSON' ;;
    machine_config) printf 'Machine profile JSON' ;;
    *) printf '%s' "$1" ;;
  esac
}

# Suggestion shown in [brackets] and used when the user presses Enter.
obbw_prompt_suggestion() {
  local key="$1" current detected
  obbw_is_secret "$key" && return 0

  # Port prompts always prefer a live USB-CDC device over a stale saved path.
  if [[ "$key" == "port" ]]; then
    if declare -f openbrewbyweight_detect_ports >/dev/null 2>&1; then
      detected="$(openbrewbyweight_detect_ports | { IFS= read -r first || true; printf '%s' "${first:-}"; })"
      if [[ -n "$detected" ]]; then
        printf '%s' "$detected"
        return 0
      fi
    fi
    current="$(obbw_get port)"
    if obbw_port_exists "$current"; then
      printf '%s' "$current"
      return 0
    fi
    current="$(obbw_cli_store_value port 2>/dev/null || true)"
    if obbw_port_exists "$current"; then
      printf '%s' "$current"
      return 0
    fi
    printf '%s' '/dev/cu.usbmodem2101'
    return 0
  fi

  current="$(obbw_get "$key")"
  if [[ -n "$current" ]]; then
    printf '%s' "$current"
    return 0
  fi
  obbw_is_transient "$key" || current="$(obbw_cli_store_value "$key" 2>/dev/null || true)"
  if [[ -n "$current" ]]; then
    printf '%s' "$current"
    return 0
  fi
  case "$key" in
    arch) printf 'n16r8' ;;
    speed) printf '115200' ;;
    host) printf '192.168.4.1' ;;
    flags) printf '%s' "$OBBW_CLI_DEFAULT_FLAGS" ;;
    *) printf '' ;;
  esac
}

obbw_prompt_hint() {
  local detected
  case "$1" in
    port)
      if declare -f openbrewbyweight_detect_ports >/dev/null 2>&1; then
        detected="$(openbrewbyweight_detect_ports | tr '\n' ' ')" || detected=""
        if [[ -n "$detected" ]]; then
          printf 'Detected USB ports: %s\n' "${detected% }" > /dev/tty
          printf 'Press Enter to use the suggested port, or type another path.\n' > /dev/tty
        else
          printf 'No USB CDC device detected (/dev/cu.usbmodem* or /dev/ttyACM*).\n' > /dev/tty
        fi
      fi
      ;;
    arch)
      printf 'n16r8 = 16 MB flash / 8 MB PSRAM\n' > /dev/tty
      ;;
  esac
}

obbw_prompt_one() {
  local key="$1" prompt value suggestion
  prompt="$(obbw_prompt_text "$key")"
  suggestion="$(obbw_prompt_suggestion "$key")"
  obbw_prompt_hint "$key"
  if [[ -n "$suggestion" ]]; then
    printf '%s [%s]: ' "$prompt" "$suggestion" > /dev/tty
  else
    printf '%s: ' "$prompt" > /dev/tty
  fi
  if obbw_is_secret "$key"; then
    IFS= read -r -s value < /dev/tty || return 1
    printf '\n' > /dev/tty
  else
    IFS= read -r value < /dev/tty || return 1
  fi
  if [[ -z "$value" && -n "$suggestion" ]]; then
    value="$suggestion"
  fi
  obbw_put "$key" "$value" "prompt"
  return 0
}

obbw_validate_key() {
  local key="$1" value
  value="$(obbw_get "$key")"
  case "$key" in
    arch)
      # openbrewbyweight_resolve_board prints its own diagnostics.
      openbrewbyweight_resolve_board "$value" >/dev/null || return 1
      ;;
    speed)
      case "$value" in
        ''|*[!0-9]*)
          obbw_cli_die "Invalid baud rate: '$value' (digits only)."
          return 1
          ;;
      esac
      ;;
    host)
      if [[ -z "$value" ]]; then
        obbw_cli_die "Parameter --host cannot be empty."
        return 1
      fi
      case "$value" in
        *[[:space:]]*)
          obbw_cli_die "Invalid host: '$value' (must not contain spaces)."
          return 1
          ;;
      esac
      ;;
    port)
      if [[ -z "$value" ]]; then
        obbw_cli_die "Parameter --port cannot be empty."
        return 1
      fi
      if ! obbw_port_exists "$value"; then
        obbw_cli_die "Serial port $value does not exist."
        return 1
      fi
      ;;
    password|hardware_config|machine_config)
      if [[ -z "$value" ]]; then
        obbw_cli_die "Parameter --$(printf '%s' "$key" | tr '_' '-') cannot be empty."
        return 1
      fi
      ;;
    flags)
      if [[ -n "$value" ]]; then
        obbw_put flags "$(obbw_normalize_compiler_flags "$value")" "$(obbw_origin flags)"
      fi
      ;;
    webui_language)
      value="$(printf '%s' "$value" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' | tr '_' '-' | tr '[:upper:]' '[:lower:]')"
      if [[ ! "$value" =~ ^[a-z]{2,3}(-[a-z0-9]{2,8})*$ ]]; then
        obbw_cli_die "Invalid WebUI language: '$(obbw_get webui_language)'."
        return 1
      fi
      obbw_put webui_language "$value" "$(obbw_origin webui_language)"
      ;;
  esac
  return 0
}

# obbw_cli_resolve "<required keys>" ["<optional keys to ask once>"]
# Loads env + store, prints the summary, prompts for what is missing and
# validates the result. Fails with exit code 2 when it cannot prompt.
#
# A required --port whose device node is missing (stale .openbrewbyweight entry,
# vanished USB path, or a bad CLI/env value) is forgotten and prompted the
# same way as an unset password: suggest the first live USB-CDC port, accept
# Enter, then persist the chosen path.
obbw_cli_resolve() {
  local required="$1" optional="${2:-}"
  local wanted="$required $optional"
  local key missing="" port_val

  obbw_cli_apply_env
  obbw_cli_load_store
  if [[ " $wanted " == *" webui_language "* ]] && ! obbw_is_set webui_language; then
    obbw_put webui_language "en" "default"
  fi

  # Drop unusable ports before the missing-key scan so flash/monitor prompt
  # early instead of failing after a long build.
  if [[ " $required " == *" port "* ]] && obbw_is_set port; then
    port_val="$(obbw_get port)"
    if ! obbw_port_exists "$port_val"; then
      if [[ -n "$port_val" ]]; then
        printf 'Serial port %s does not exist.\n' "$port_val" >&2
      fi
      obbw_cli_forget port
    fi
  fi

  for key in $required; do
    obbw_is_set "$key" || missing="$missing $key"
  done
  missing="${missing# }"

  # Optional keys that were never provided stay empty rather than blocking a
  # non-interactive run. Prompting for them only happens when a TTY is there.
  local optional_missing=""
  for key in $optional; do
    obbw_is_set "$key" && continue
    optional_missing="$optional_missing $key"
  done
  optional_missing="${optional_missing# }"

  obbw_cli_summary "$wanted" "$missing"

  if [[ -n "$optional_missing" ]] && obbw_can_prompt; then
    for key in $optional_missing; do
      obbw_prompt_one "$key" || {
        printf 'Input interrupted.\n' >&2
        return 2
      }
    done
  elif [[ -n "$optional_missing" ]]; then
    for key in $optional_missing; do
      obbw_put "$key" "" "default"
    done
  fi

  if [[ -n "$missing" ]]; then
    if ! obbw_can_prompt; then
      printf '\nNo interactive terminal to prompt for: %s\n' "$missing" >&2
      printf 'Pass them as flags or save them in %s.\n' "$OBBW_CLI_STORE" >&2
      case " $missing " in
        *' host '*)
          printf 'The IP appears under Admin/Diagnostic in the Web UI; on SoftAP it is 192.168.4.1.\n' >&2
          ;;
        *' port '*)
          printf 'Plug in the ESP32-S3 over USB and pass --port /dev/cu.usbmodem… (macOS) or /dev/ttyACM… (Linux).\n' >&2
          ;;
      esac
      return 2
    fi
    for key in $missing; do
      obbw_prompt_one "$key" || {
        printf 'Input interrupted.\n' >&2
        return 2
      }
    done
  fi

  for key in $wanted; do
    obbw_is_set "$key" || continue
    while ! obbw_validate_key "$key"; do
      # Wrong path typed at the prompt: ask again instead of aborting the run.
      if [[ "$key" == "port" ]] && obbw_can_prompt; then
        obbw_cli_forget port
        obbw_prompt_one port || {
          printf 'Input interrupted.\n' >&2
          return 2
        }
        continue
      fi
      return 2
    done
  done
  obbw_cli_save
  return 0
}

obbw_cli_save() {
  local key value tmp
  tmp="${TMPDIR:-/tmp}/openbrewbyweight.XXXXXX"
  tmp="$(mktemp "$tmp")" || return 0
  chmod 600 "$tmp" 2>/dev/null || true
  {
    printf '# Values remembered by the Open Brew by Weight scripts.\n'
    printf '# Local file, ignored by git. Does not store the device password.\n'
    for key in $OBBW_CLI_KEYS; do
      obbw_is_transient "$key" && continue
      obbw_is_secret "$key" && continue
      if obbw_is_set "$key"; then
        value="$(obbw_get "$key")"
      else
        value="$(obbw_cli_store_value "$key" 2>/dev/null || true)"
      fi
      [[ -n "$value" ]] || continue
      # A newline in a stored value would split into a second "key=value" line
      # on the next load and could clobber another key.
      case "$value" in
        *$'\n'*) continue ;;
      esac
      printf '%s=%s\n' "$key" "$value"
    done
  } > "$tmp"
  mv -f "$tmp" "$OBBW_CLI_STORE" 2>/dev/null || rm -f "$tmp"
  chmod 600 "$OBBW_CLI_STORE" 2>/dev/null || true
  return 0
}

# Make an explicitly selected firmware image absolute before wrapper scripts
# change to the repository root. The override is deliberately transient: a
# stale release image must never become the next command's surprise default.
obbw_cli_normalize_image() {
  local image dir base origin
  obbw_is_set image || return 0
  image="$(obbw_get image)"
  if [[ ! -f "$image" || ! -r "$image" ]]; then
    printf 'Firmware image does not exist or is not readable: %s\n' "$image" >&2
    return 2
  fi
  dir="$(dirname -- "$image")"
  base="$(basename -- "$image")"
  dir="$(CDPATH='' cd -- "$dir" && pwd -P)" || return 2
  origin="$(obbw_origin image)"
  obbw_put image "$dir/$base" "$origin"
}

# Re-emits the resolved values as flags so wrapper scripts can call children
# without triggering a second round of prompts.
#
# Callers must expand this as ${OBBW_CLI_FORWARD[@]+"${OBBW_CLI_FORWARD[@]}"}:
# bash 3.2 treats "${array[@]}" on an empty array as an unbound variable, which
# under `set -u` aborts the wrapper instead of running the child with no flags.
obbw_cli_flags_for() {
  local key
  OBBW_CLI_FORWARD=()
  for key in "$@"; do
    if [[ "$key" == "yes" ]]; then
      [[ "$OBBW_CLI_YES" == "1" ]] && OBBW_CLI_FORWARD+=(--yes)
      continue
    fi
    if [[ "$key" == "wait_for_confirmation" ]]; then
      [[ "$OBBW_CLI_WAIT_FOR_CONFIRMATION" == "1" ]] &&
        OBBW_CLI_FORWARD+=(--wait-for-confirmation)
      continue
    fi
    if [[ "$key" == "no_check" ]]; then
      [[ "$OBBW_CLI_NO_CHECK" == "1" ]] && OBBW_CLI_FORWARD+=(--no-check)
      continue
    fi
    if [[ "$key" == "erase_all" ]]; then
      [[ "$OBBW_CLI_ERASE_ALL" == "1" ]] && OBBW_CLI_FORWARD+=(--erase-all)
      continue
    fi
    if [[ "$key" == "discard_ota_session" ]]; then
      [[ "$OBBW_CLI_DISCARD_OTA_SESSION" == "1" ]] &&
        OBBW_CLI_FORWARD+=(--discard-ota-session)
      continue
    fi
    if [[ "$key" == "development" ]]; then
      [[ "$OBBW_CLI_DEVELOPMENT" == "1" ]] && OBBW_CLI_FORWARD+=(--development)
      continue
    fi
    if [[ "$key" == "release" ]]; then
      [[ "$OBBW_CLI_RELEASE" == "1" ]] && OBBW_CLI_FORWARD+=(--release)
      continue
    fi
    if [[ "$key" == "no_auth_admin" ]]; then
      [[ "$OBBW_CLI_NO_AUTH_ADMIN" == "1" ]] && OBBW_CLI_FORWARD+=(--no-auth-admin)
      continue
    fi
    if [[ "$key" == "jtag" ]]; then
      [[ "$OBBW_CLI_JTAG" == "1" ]] && OBBW_CLI_FORWARD+=(--jtag)
      continue
    fi
    if [[ "$key" == "force_sdkconfig_regenerate" ]]; then
      [[ "$OBBW_CLI_FORCE_SDKCONFIG_REGENERATE" == "1" ]] &&
        OBBW_CLI_FORWARD+=(--force-sdkconfig-regenerate)
      continue
    fi
    if [[ "$key" == "opt_level" ]]; then
      [[ -n "$OBBW_CLI_OPT_LEVEL" ]] && OBBW_CLI_FORWARD+=("$OBBW_CLI_OPT_LEVEL")
      continue
    fi
    obbw_is_set "$key" || continue
    case "$key" in
      port) OBBW_CLI_FORWARD+=(--port "$(obbw_get port)") ;;
      arch) OBBW_CLI_FORWARD+=(--arch "$(obbw_get arch)") ;;
      speed) OBBW_CLI_FORWARD+=(--speed "$(obbw_get speed)") ;;
      host) OBBW_CLI_FORWARD+=(--host "$(obbw_get host)") ;;
      image) OBBW_CLI_FORWARD+=(--image "$(obbw_get image)") ;;
      # password is deliberately absent: a command line is world-readable
      # through `ps` for as long as the child runs, which for an OTA is
      # minutes. Use obbw_cli_export_secrets instead.
      flags) OBBW_CLI_FORWARD+=(--flags "$(obbw_get flags)") ;;
      webui_language) OBBW_CLI_FORWARD+=(--webui-language "$(obbw_get webui_language)") ;;
      hardware_config) OBBW_CLI_FORWARD+=(--hardware "$(obbw_get hardware_config)") ;;
      machine_config) OBBW_CLI_FORWARD+=(--machine "$(obbw_get machine_config)") ;;
    esac
  done
}

# Hands secrets to a child process through the environment, which other users
# cannot read, rather than through argv, which they can.
obbw_cli_export_secrets() {
  if obbw_is_set password; then
    OPENBREWBYWEIGHT_DEVICE_PASSWORD="$(obbw_get password)"
    export OPENBREWBYWEIGHT_DEVICE_PASSWORD
  fi
}

obbw_cli_reset
