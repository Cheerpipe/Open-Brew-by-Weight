#!/usr/bin/env bash
# Shared ESP32-S3 board table. Source from other scripts; do not execute this
# file directly.
#
# There is deliberately no default architecture and no automatic port pick.
# Every caller resolves those through scripts/openbrewbyweight_cli.sh.

# N16R8: 16 MB flash + 8 MB OPI PSRAM with two 3.5 MiB OTA app slots
# (idf/partitions-n16r8.csv). Usable bytes of a single OTA app slot, used by
# the OTA client for a local size pre-check before uploading anything.
OPENBREWBYWEIGHT_SLOT_BYTES_N16R8=3670016

openbrewbyweight_arch_help() {
  cat <<'EOF'
Supported architecture (ESP32-S3 with PSRAM only):
  n16r8  ESP32-S3, 16 MB flash, 8 MB OPI PSRAM

Aliases: esp32s3 and esp32s3-n16r8 → n16r8.
Classic ESP32 and Nano ESP32 are no longer supported.
EOF
}

# Prints ESP32 USB-CDC serial devices, one per line, in glob order.
# macOS: /dev/cu.usbmodem<digits>  Linux: /dev/ttyACM<digits>
# Used to suggest a port at the interactive prompt; it never auto-selects.
openbrewbyweight_detect_ports() {
  local p found=1
  for p in /dev/cu.usbmodem[0-9]*; do
    [[ -e "$p" ]] || continue
    [[ "$p" =~ ^/dev/cu\.usbmodem[0-9]+$ ]] || continue
    printf '%s\n' "$p"
    found=0
  done
  for p in /dev/ttyACM[0-9]*; do
    [[ -e "$p" ]] || continue
    [[ "$p" =~ ^/dev/ttyACM[0-9]+$ ]] || continue
    printf '%s\n' "$p"
    found=0
  done
  return $found
}

# Sets OPENBREWBYWEIGHT_ARCH and OPENBREWBYWEIGHT_SLOT_BYTES.
openbrewbyweight_resolve_board() {
  local arch="${1:-}"
  case "$arch" in
    n16r8|esp32s3-n16r8|esp32s3)
      OPENBREWBYWEIGHT_ARCH="n16r8"
      OPENBREWBYWEIGHT_SLOT_BYTES="$OPENBREWBYWEIGHT_SLOT_BYTES_N16R8"
      ;;
    esp32|esp32c3|nanoesp32|"")
      echo "Unsupported architecture: ${arch:-<empty>}." >&2
      echo "Open Brew by Weight only supports ESP32-S3 n16r8 with PSRAM." >&2
      return 2
      ;;
    *)
      echo "Unsupported architecture: $arch (use n16r8)." >&2
      return 2
      ;;
  esac
}
