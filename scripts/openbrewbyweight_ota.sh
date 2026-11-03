#!/usr/bin/env bash
# Shared resilient OTA client. Source after openbrewbyweight_cli.sh (bash 3.2).

OBBW_OTA_RANGE_ATTEMPTS=3
OBBW_OTA_COMMIT_ATTEMPTS=3
OBBW_OTA_CHUNK_BYTES=65536

obbw_ota_field() {
  node -e '
const fs = require("fs"); let data = {};
try { data = JSON.parse(fs.readFileSync(process.argv[1], "utf8")); } catch (_) {}
const value = process.argv[2].split(".").reduce(
  (node, key) => node == null ? undefined : node[key], data);
process.stdout.write(value == null ? "" : String(value));
' "$OBBW_OTA_BODY_FILE" "$1"
}

obbw_ota_upload_progress() {
  # Render the confirmed byte offset against the entire image.  Uploads are
  # sent in ranges, so curl's own progress bar would otherwise reach 100% for
  # every individual range.
  local received="$1" total="$2" width=50 filled=0 index=0 bar=""
  (( total > 0 )) || return 0
  (( received > total )) && received="$total"
  filled=$((received * width / total))
  while (( index < width )); do
    if (( index < filled )); then
      bar="${bar}#"
    else
      bar="${bar}-"
    fi
    index=$((index + 1))
  done
  printf '\rUpload: [%s] %3s%% (%s / %s KiB)' "$bar" \
      "$((received * 100 / total))" "$((received / 1024))" "$((total / 1024))"
}

obbw_ota_request() {
  # method, path, optional payload file, timeout seconds, extra curl headers
  local method="$1" path="$2" payload="$3" timeout="$4"
  shift 4
  local args
  args=(--show-error --config "$OBBW_OTA_CURL_CONFIG" --connect-timeout 10
        --max-time "$timeout" --output "$OBBW_OTA_BODY_FILE"
        --write-out '%{http_code}' --request "$method")
  : > "$OBBW_OTA_BODY_FILE"
  if [[ -n "$payload" ]]; then
    # The ESP HTTP server is deliberately small. Removing Expect avoids a
    # needless extra round trip before the request body starts flowing.
    args+=(--silent --header "Content-Type: ${OBBW_OTA_CONTENT_TYPE:-application/octet-stream}"
           --header 'Expect:' --data-binary "@$payload")
  else
    args+=(--silent)
  fi
  while (( $# > 0 )); do
    args+=(--header "$1")
    shift
  done
  OBBW_OTA_HTTP_STATUS=""
  set +e
  OBBW_OTA_HTTP_STATUS="$(curl "${args[@]}" "$OBBW_OTA_BASE$path")"
  OBBW_OTA_CURL_EXIT=$?
  set -e
  [[ "$OBBW_OTA_CURL_EXIT" == "0" ]]
}

obbw_ota_status() {
  obbw_ota_request GET /api/v1/ota "" 20 && [[ "$OBBW_OTA_HTTP_STATUS" == "200" ]]
}

obbw_ota_status_quiet() {
  # A connection failure is expected while the controller reboots after a
  # forced OTA.  Keep polling without printing curl's transient timeout.
  obbw_ota_status 2>/dev/null
}

obbw_ota_report_failure() {
  obbw_ota_report_values "$1" "${OBBW_OTA_CURL_EXIT:-}" "${OBBW_OTA_HTTP_STATUS:-}" \
      "$(obbw_ota_field error)" "$(obbw_ota_field message)"
}

obbw_ota_report_values() {
  local operation="$1" curl_exit="$2" http_status="$3" error="$4" message="$5"
  error="$(printf '%s' "$error" | tr '\r\n\t' '   ')"
  message="$(printf '%s' "$message" | tr '\r\n\t' '   ')"
  error="${error:0:96}"
  message="${message:0:300}"
  printf '%s failed: curl=%s HTTP=%s%s%s.\n' \
      "$operation" "${curl_exit:-unknown}" "${http_status:-no-response}" \
      "${error:+ code=$error}" "${message:+ message=$message}" >&2
}

obbw_ota_remote_owns_slot() {
  [[ "$(obbw_ota_field sessionActive)" == "true" ]] ||
      [[ "$(obbw_ota_field state)" == "staged" ]]
}

obbw_ota_remote_matches_image() {
  [[ -n "$(obbw_ota_field transferId)" ]] &&
      [[ "$(obbw_ota_field sha256)" == "$OBBW_OTA_IMAGE_SHA256" ]] &&
      [[ "$(obbw_ota_field expectedBytes)" == "$OBBW_OTA_IMAGE_SIZE" ]] || return 1
  [[ "$(obbw_ota_field sessionArch)" == "$OBBW_OTA_IMAGE_ARCH" ]] &&
      [[ "$(obbw_ota_field sessionHardware)" == "$OBBW_OTA_IMAGE_HARDWARE" ]] &&
      [[ "$(obbw_ota_field sessionMachine)" == "$OBBW_OTA_IMAGE_MACHINE" ]] &&
      [[ "$(obbw_ota_field sessionVersion)" == "$OBBW_OTA_IMAGE_VERSION" ]]
}

obbw_ota_remote_matches_transfer() {
  obbw_ota_remote_matches_image &&
      [[ "$(obbw_ota_field transferId)" == "$OBBW_OTA_TRANSFER_ID" ]]
}

obbw_ota_offset_valid() {
  [[ "$1" =~ ^[0-9]{1,10}$ ]] && (( 10#$1 <= OBBW_OTA_IMAGE_SIZE )) &&
      (( 10#$1 == OBBW_OTA_IMAGE_SIZE || 10#$1 % 4096 == 0 ))
}

obbw_ota_protocol_supported() {
  [[ "$(obbw_ota_field otaProtocolVersion)" == "3" ]]
}

obbw_ota_write_session_body() {
  node -e 'process.stdout.write(JSON.stringify({size:Number(process.argv[1]),sha256:process.argv[2],arch:process.argv[3],hardware:process.argv[4],machine:process.argv[5],version:process.argv[6],transferId:process.argv[7]}))' \
      "$OBBW_OTA_IMAGE_SIZE" "$OBBW_OTA_IMAGE_SHA256" "$OBBW_OTA_IMAGE_ARCH" \
      "$OBBW_OTA_IMAGE_HARDWARE" "$OBBW_OTA_IMAGE_MACHINE" \
      "$OBBW_OTA_IMAGE_VERSION" "$OBBW_OTA_TRANSFER_ID" > "$OBBW_OTA_SESSION_BODY"
}

obbw_ota_staged_matches_image() {
  [[ "$(obbw_ota_field state)" == "staged" ]] || return 1
  obbw_ota_remote_matches_transfer &&
      [[ "$(obbw_ota_field staged.arch)" == "$OBBW_OTA_IMAGE_ARCH" ]] &&
      [[ "$(obbw_ota_field staged.hardware)" == "$OBBW_OTA_IMAGE_HARDWARE" ]] &&
      [[ "$(obbw_ota_field staged.machine)" == "$OBBW_OTA_IMAGE_MACHINE" ]] &&
      [[ "$(obbw_ota_field staged.version)" == "$OBBW_OTA_IMAGE_VERSION" ]] &&
      [[ "$(obbw_ota_field staged.packed)" == "$OBBW_OTA_IMAGE_PACKED" ]]
}

obbw_ota_explain_conflict() {
  # Plain-language report for a different image owning the update slot. The
  # warning color is conditional so redirected logs and CI stay escape-free.
  local warn='' reset='' detail
  if [[ -t 2 ]]; then warn=$'\033[33m'; reset=$'\033[0m'; fi
  if [[ "$(obbw_ota_field state)" == "staged" ]]; then
    detail='fully uploaded and verified, waiting for its commit'
  else
    local received total percent=0
    received="$(obbw_ota_field nextOffset)"
    total="$(obbw_ota_field expectedBytes)"
    if [[ "$received" =~ ^[0-9]+$ ]] && [[ "$total" =~ ^[0-9]+$ ]] &&
        (( total > 0 )); then
      percent=$((received * 100 / total))
    fi
    detail="an interrupted upload at ${percent}%"
  fi
  printf '%sA different firmware image owns the update slot:%s %s.\n' \
      "$warn" "$reset" "$detail" >&2
  echo 'Resumable OTA never replaces another build on its own; discarding drops that progress and restarts from byte zero.' >&2
  printf 'Remote: sha256=%s size=%s arch=%s hardware=%s machine=%s version=%s\n' \
      "$(obbw_ota_field sha256)" "$(obbw_ota_field expectedBytes)" \
      "$(obbw_ota_field sessionArch)" "$(obbw_ota_field sessionHardware)" \
      "$(obbw_ota_field sessionMachine)" "$(obbw_ota_field sessionVersion)" >&2
  printf 'Local:  sha256=%s size=%s arch=%s hardware=%s machine=%s version=%s\n' \
      "$OBBW_OTA_IMAGE_SHA256" "$OBBW_OTA_IMAGE_SIZE" \
      "$OBBW_OTA_IMAGE_ARCH" "$OBBW_OTA_IMAGE_HARDWARE" \
      "$OBBW_OTA_IMAGE_MACHINE" "$OBBW_OTA_IMAGE_VERSION" >&2
}

obbw_ota_confirm_discard() {
  # Unlike the commit prompt's [y/N] (a commit reboots the machine), the cost
  # of wrongly discarding is only re-uploading the remote partial, so Y is
  # the default. stdin may already be consumed by --password-stdin: read the
  # terminal directly.
  local answer
  printf 'Discard it and upload this image instead? [Y/n]: ' > /dev/tty
  IFS= read -r answer < /dev/tty || answer=""
  case "$answer" in
    n|N|no|NO) return 1 ;;
  esac
  return 0
}

obbw_ota_conflict_decision() {
  # yes flag, explicit discard flag. Returns 0 to discard and records the
  # announcement reason in OBBW_OTA_DISCARD_REASON; returns 1 to keep and stop.
  local yes="$1" explicit="$2"
  obbw_ota_explain_conflict
  if [[ "$explicit" == "1" ]]; then
    OBBW_OTA_DISCARD_REASON='requested with --discard-ota-session'
    return 0
  fi
  if [[ "$yes" == "1" ]]; then
    OBBW_OTA_DISCARD_REASON='assumed through --yes'
    return 0
  fi
  if obbw_can_prompt && obbw_ota_confirm_discard; then
    OBBW_OTA_DISCARD_REASON='confirmed interactively'
    return 0
  fi
  echo 'The remote image was kept. To replace it, re-run with --discard-ota-session or use Discard in the Web UI (Admin).' >&2
  return 1
}

obbw_ota_backoff() {
  local attempt="$1" max="$2"
  local seconds=$((attempt * attempt))
  printf 'Transient OTA failure; retrying in %ss (attempt %s/%s).\n' \
      "$seconds" "$((attempt + 1))" "$max" >&2
  sleep "$seconds"
}

obbw_ota_upload() {
  OBBW_OTA_CONTENT_TYPE=application/json
  if ! obbw_ota_request POST /api/v1/ota/session "$OBBW_OTA_SESSION_BODY" 30 ||
      [[ "$OBBW_OTA_HTTP_STATUS" != "200" ]]; then
    obbw_ota_report_failure 'Creating the OTA session'
    return 1
  fi
  if ! obbw_ota_remote_matches_transfer; then
    echo 'The controller returned a different or incomplete OTA session identity.' >&2
    return 1
  fi
  OBBW_OTA_CONTENT_TYPE=application/octet-stream
  local offset
  offset="$(obbw_ota_field nextOffset)"
  if ! obbw_ota_offset_valid "$offset"; then
    printf 'The controller returned invalid nextOffset=%s for image size %s.\n' \
        "${offset:-missing}" "$OBBW_OTA_IMAGE_SIZE" >&2
    return 1
  fi
  local high_water="$offset" attempt=1
  obbw_ota_upload_progress "$offset" "$OBBW_OTA_IMAGE_SIZE"
  while (( offset < OBBW_OTA_IMAGE_SIZE )); do
    if ! obbw_ota_image_matches_declaration; then
      obbw_ota_report_image_changed
      return 1
    fi
    local end=$((offset + OBBW_OTA_CHUNK_BYTES))
    (( end > OBBW_OTA_IMAGE_SIZE )) && end=$OBBW_OTA_IMAGE_SIZE
    local length=$((end - offset))
    dd if="$OBBW_OTA_IMAGE" of="$OBBW_OTA_CHUNK_FILE" bs=1 skip="$offset" count="$length" 2>/dev/null
    while :; do
      if obbw_ota_request PATCH /api/v1/ota "$OBBW_OTA_CHUNK_FILE" 90 \
          "X-OTA-Transfer: $OBBW_OTA_TRANSFER_ID" "X-OTA-Offset: $offset" \
          "X-OTA-Length: $OBBW_OTA_IMAGE_SIZE" \
          "Content-Range: bytes $offset-$((end - 1))/$OBBW_OTA_IMAGE_SIZE" &&
          [[ "$OBBW_OTA_HTTP_STATUS" =~ ^(200|208)$ ]]; then
        local next
        next="$(obbw_ota_field nextOffset)"
        if ! obbw_ota_offset_valid "$next" || (( next != end )); then
          printf 'The controller returned invalid nextOffset=%s after byte %s.\n' \
              "${next:-missing}" "$offset" >&2
          return 1
        fi
        if ! obbw_ota_remote_matches_transfer; then
          echo 'The controller returned a different or incomplete OTA session identity.' >&2
          return 1
        fi
        offset="$next"
        if (( offset > high_water )); then high_water="$offset"; attempt=1; fi
        obbw_ota_upload_progress "$offset" "$OBBW_OTA_IMAGE_SIZE"
        break
      fi
      local failed_curl="$OBBW_OTA_CURL_EXIT" failed_http="$OBBW_OTA_HTTP_STATUS"
      local failed_error failed_message
      failed_error="$(obbw_ota_field error)"
      failed_message="$(obbw_ota_field message)"
      case "$failed_error" in
        SAFETY_LOST|OTA_SHA256_MISMATCH|OTA_SESSION_IDENTITY_MISMATCH|OTA_INVALID_RANGE)
          obbw_ota_report_values 'Uploading the firmware range' \
              "$failed_curl" "$failed_http" "$failed_error" "$failed_message"
          return 1 ;;
      esac
      # Rebuild the range from the accepted offset, which can retreat to a
      # persisted checkpoint after a reboot. Never reuse stale chunk bytes.
      local reconciled=0
      if obbw_ota_request GET /api/v1/ota/session "" 20 &&
          [[ "$OBBW_OTA_HTTP_STATUS" == "200" ]]; then
        if ! obbw_ota_remote_matches_transfer; then
          echo 'OTA session disappeared or changed identity; upload stopped.' >&2
          return 1
        fi
        local next
        next="$(obbw_ota_field nextOffset)"
        if ! obbw_ota_offset_valid "$next" || (( next > end )); then
          echo 'The controller returned an invalid recovery offset.' >&2
          return 1
        fi
        offset="$next"; reconciled=1
        if (( offset > high_water )); then
          high_water="$offset"; attempt=1
          obbw_ota_upload_progress "$offset" "$OBBW_OTA_IMAGE_SIZE"
          break
        fi
      fi
      if (( attempt >= OBBW_OTA_RANGE_ATTEMPTS )); then
        obbw_ota_report_values 'Uploading the firmware range' \
            "$failed_curl" "$failed_http" "$failed_error" "$failed_message"
        return 1
      fi
      obbw_ota_backoff "$attempt" "$OBBW_OTA_RANGE_ATTEMPTS"
      attempt=$((attempt + 1))
      if (( reconciled )); then break; fi
    done
  done
  if ! obbw_ota_image_matches_declaration; then
    obbw_ota_report_image_changed
    return 1
  fi
  printf '\n'
  if ! obbw_ota_staged_matches_image; then
    echo 'The controller did not report the expected verified image identity.' >&2
    return 1
  fi
}

obbw_ota_image_matches_declaration() {
  # Re-checked before every range and before commit: parallel build gates
  # compile into the same build tree and can rewrite the image mid-transfer.
  [[ "$(shasum -a 256 "$OBBW_OTA_IMAGE" | awk '{print $1}')" == "$OBBW_OTA_IMAGE_SHA256" ]]
}

obbw_ota_report_image_changed() {
  local red='' reset=''
  if [[ -t 2 ]]; then red=$'\033[31m'; reset=$'\033[0m'; fi
  printf '%sThe firmware image changed while the upload was running%s: %s.\n' \
      "$red" "$reset" "$OBBW_OTA_IMAGE" >&2
  printf 'The file no longer matches the SHA-256 that the transfer declared, so a concurrent build probably rewrote it. The upload stopped before anything could be flashed from the changed file. Re-run the update once no other build is writing to this image.\n' >&2
}

obbw_ota_commit() {
  local attempt=1
  while (( attempt <= OBBW_OTA_COMMIT_ATTEMPTS )); do
    if obbw_ota_request POST /api/v1/ota/flash "" 30 &&
        [[ "$OBBW_OTA_HTTP_STATUS" == "202" ]] && obbw_ota_remote_matches_transfer; then
      return 0
    fi
    local curl_exit="$OBBW_OTA_CURL_EXIT" http_status="$OBBW_OTA_HTTP_STATUS"
    local error
    error="$(obbw_ota_field error)"
    local message
    message="$(obbw_ota_field message)"
    obbw_ota_report_values "Commit attempt $attempt" "$curl_exit" \
        "$http_status" "$error" "$message"

    # POST /flash is not blindly replayed: first reconcile a lost 202.
    if obbw_ota_status; then
      if obbw_ota_new_boot; then
        echo 'The controller restarted; checking the running image and confirmation.' >&2
        return 0
      fi
      if obbw_ota_remote_matches_transfer &&
          { [[ "$(obbw_ota_field restartPending)" == "true" ]] ||
            [[ "$(obbw_ota_field state)" == "committed" ]]; }; then
        echo 'The controller accepted this image commit; its response was lost.' >&2
        return 0
      fi
    fi
    if (( attempt < OBBW_OTA_COMMIT_ATTEMPTS )) && obbw_ota_staged_matches_image; then
      obbw_ota_backoff "$attempt" "$OBBW_OTA_COMMIT_ATTEMPTS"
      attempt=$((attempt + 1))
      continue
    fi
    return 1
  done
  return 1
}

obbw_ota_new_boot() {
  local boot
  boot="$(obbw_ota_field bootId)"
  [[ "${OBBW_OTA_PRE_BOOT_ID:-}" =~ ^[0-9]+$ ]] &&
      [[ "$boot" =~ ^[0-9]+$ ]] && [[ "$boot" != "$OBBW_OTA_PRE_BOOT_ID" ]]
}

obbw_ota_boot_result() {
  # A matching version (or matching image alone) cannot prove a new boot.
  local boot digest
  boot="$(obbw_ota_field bootId)"
  digest="$(obbw_ota_field running.imageSha256)"
  if [[ ! "${OBBW_OTA_PRE_BOOT_ID:-}" =~ ^[0-9]+$ ]] ||
      [[ ! "$boot" =~ ^[0-9]+$ ]] || [[ ! "$digest" =~ ^[a-f0-9]{64}$ ]]; then
    OBBW_OTA_BOOT_RESULT=unverified
  elif ! obbw_ota_new_boot; then
    OBBW_OTA_BOOT_RESULT=restart-pending
  elif [[ "$digest" != "${OBBW_OTA_IMAGE_DIGEST:-}" ]]; then
    OBBW_OTA_BOOT_RESULT=different-image
  elif [[ "$(obbw_ota_field confirmed)" == "true" ]]; then
    OBBW_OTA_BOOT_RESULT=confirmed
  else
    OBBW_OTA_BOOT_RESULT=confirmation-pending
  fi
}

obbw_ota_cleanup() {
  rm -f "${OBBW_OTA_CURL_CONFIG:-}" "${OBBW_OTA_BODY_FILE:-}" "${OBBW_OTA_SESSION_BODY:-}" "${OBBW_OTA_CHUNK_FILE:-}"
}

obbw_ota_run() {
  # image, requested arch, host, password, yes, wait, skip check, discard conflict
  OBBW_OTA_IMAGE="$1"
  local arch="$2" host="$3" password="$4" yes="$5" wait="$6" \
      skip_local_check="${7:-0}" discard_existing="${8:-0}"
  OBBW_OTA_BASE="http://$host"

  for tool in curl node; do
    command -v "$tool" >/dev/null 2>&1 || {
      echo "$tool not found on PATH." >&2
      return 127
    }
  done

  if [[ "$skip_local_check" == "1" ]]; then
    echo '--no-check cannot perform resumable OTA: SHA-256 and the complete image identity are required.' >&2
    return 2
  else
    local tag_json
    echo 'Local image identity:'
    tag_json="$(node ./scripts/image_tag.js "$OBBW_OTA_IMAGE" --expect-arch "$arch" --json)" || return $?
    OBBW_OTA_IMAGE_ARCH="$(node -e 'process.stdout.write(JSON.parse(process.argv[1]).arch)' "$tag_json")"
    OBBW_OTA_IMAGE_HARDWARE="$(node -e 'process.stdout.write(JSON.parse(process.argv[1]).hardware)' "$tag_json")"
    OBBW_OTA_IMAGE_MACHINE="$(node -e 'process.stdout.write(JSON.parse(process.argv[1]).machine)' "$tag_json")"
    OBBW_OTA_IMAGE_VERSION="$(node -e 'process.stdout.write(JSON.parse(process.argv[1]).version)' "$tag_json")"
    OBBW_OTA_IMAGE_PACKED="$(node -e 'process.stdout.write(String(JSON.parse(process.argv[1]).packed))' "$tag_json")"
    OBBW_OTA_IMAGE_DIGEST="$(node -e 'process.stdout.write(JSON.parse(process.argv[1]).imageSha256)' "$tag_json")"
  fi

  OBBW_OTA_IMAGE_SIZE="$(wc -c < "$OBBW_OTA_IMAGE" | tr -d ' ')"
  OBBW_OTA_IMAGE_SHA256="$(shasum -a 256 "$OBBW_OTA_IMAGE" | awk '{print $1}')"
  OBBW_OTA_TRANSFER_ID=""

  OBBW_OTA_CURL_CONFIG="$(mktemp "${TMPDIR:-/tmp}/openbrewbyweight-ota.XXXXXX")"
  chmod 600 "$OBBW_OTA_CURL_CONFIG"
  OBBW_OTA_BODY_FILE="$(mktemp "${TMPDIR:-/tmp}/openbrewbyweight-ota-body.XXXXXX")"
  OBBW_OTA_SESSION_BODY="$(mktemp "${TMPDIR:-/tmp}/openbrewbyweight-ota-session.XXXXXX")"
  OBBW_OTA_CHUNK_FILE="$(mktemp "${TMPDIR:-/tmp}/openbrewbyweight-ota-chunk.XXXXXX")"
  password="${password//\\/\\\\}"
  password="${password//\"/\\\"}"
  printf 'header = "X-Device-Password: %s"\n' "$password" > "$OBBW_OTA_CURL_CONFIG"
  unset password

  echo
  echo "Querying $OBBW_OTA_BASE ..."
  if ! obbw_ota_status; then
    obbw_ota_report_failure "Querying the controller at $OBBW_OTA_BASE"
    echo 'Check the IP (Admin or Diagnostic in the Web UI; SoftAP is 192.168.4.1).' >&2
    return 1
  fi
  if ! obbw_ota_protocol_supported; then
    echo 'The controller does not support profile-aware resumable OTA protocol v3.' >&2
    echo 'Update it once over USB; no legacy OTA fallback was attempted.' >&2
    return 1
  fi
  if [[ "$(obbw_ota_field available)" != "true" ]]; then
    echo 'This controller has no second application partition.' >&2
    echo 'Update it over USB instead.' >&2
    return 1
  fi
  printf 'Controller: %s · %s\n' "$(obbw_ota_field running.version)" \
      "$(obbw_ota_field running.arch)"
  OBBW_OTA_PRE_BOOT_ID="$(obbw_ota_field bootId)"
  if [[ "$(obbw_ota_field runningIdentityValid)" == "false" ]] ||
      [[ "$(obbw_ota_field running.arch)" != "$OBBW_OTA_IMAGE_ARCH" ]] ||
      [[ "$(obbw_ota_field running.hardware)" != "$OBBW_OTA_IMAGE_HARDWARE" ]] ||
      [[ "$(obbw_ota_field running.machine)" != "$OBBW_OTA_IMAGE_MACHINE" ]]; then
    echo 'The running firmware has no usable Open Brew by Weight image identity.' >&2
    echo 'Update it once over USB before using OTA.' >&2
    return 1
  fi
  if [[ "$(obbw_ota_field safe)" != "true" ]]; then
    printf 'Machine is busy (%s). Wait until Ready.\n' "$(obbw_ota_field lockReason)" >&2
    return 1
  fi

  if obbw_ota_remote_owns_slot; then
    if obbw_ota_remote_matches_image; then
      OBBW_OTA_TRANSFER_ID="$(obbw_ota_field transferId)"
      printf 'Resuming matching OTA session %s at byte %s.\n' \
          "$OBBW_OTA_TRANSFER_ID" "$(obbw_ota_field nextOffset)"
    else
      if ! obbw_ota_conflict_decision "$yes" "$discard_existing"; then
        return 1
      fi
      printf 'Discarding the different OTA session (%s).\n' \
          "$OBBW_OTA_DISCARD_REASON"
      if ! obbw_ota_request POST /api/v1/ota/abort "" 20 ||
          [[ "$OBBW_OTA_HTTP_STATUS" != "200" ]]; then
        obbw_ota_report_failure 'Discarding the existing OTA session'
        return 1
      fi
    fi
  fi
  if [[ -z "${OBBW_OTA_TRANSFER_ID:-}" ]]; then
    OBBW_OTA_TRANSFER_ID="$(node -e 'const c=require("crypto");process.stdout.write(c.randomBytes(18).toString("hex"))')"
  fi
  obbw_ota_write_session_body || return $?

  printf 'Uploading %s (%s KiB, SHA-256 %s)…\n' "$OBBW_OTA_IMAGE" "$((OBBW_OTA_IMAGE_SIZE / 1024))" "$OBBW_OTA_IMAGE_SHA256"
  obbw_ota_upload || return 1

  local staged_version staged_arch
  staged_version="$(obbw_ota_field staged.version)"
  staged_arch="$(obbw_ota_field staged.arch)"
  if [[ -z "$staged_version" ]]; then
    echo 'Controller did not report a verified image.' >&2
    return 1
  fi
  printf '\nVerified image: %s · %s\n' "$staged_version" "$staged_arch"

  if [[ "$yes" != "1" ]]; then
    local answer
    printf 'Commit and reboot the controller? [y/N]: ' > /dev/tty
    IFS= read -r answer < /dev/tty || answer=""
    case "$answer" in
      y|Y|yes|YES) ;;
      *)
        echo 'Cancelled. Discarding the image on the controller.'
        obbw_ota_request POST /api/v1/ota/abort "" 20 || true
        return 1
        ;;
    esac
  fi

  echo 'Committing...'
  if ! obbw_ota_status || ! obbw_ota_staged_matches_image; then
    echo 'Cannot verify the staged transfer immediately before commit.' >&2
    return 1
  fi
  OBBW_OTA_PRE_BOOT_ID="$(obbw_ota_field bootId)"
  obbw_ota_commit || {
    echo 'Could not confirm the boot partition change.' >&2
    return 1
  }

  if [[ "$wait" == "1" ]]; then
    echo 'Waiting for reboot and OTA confirmation (up to 4 minutes)...'
    local deadline=$((SECONDS + 240))
    while (( SECONDS < deadline )); do
      if obbw_ota_status_quiet; then
        obbw_ota_boot_result
        case "$OBBW_OTA_BOOT_RESULT" in
          confirmed) echo 'OTA confirmed by the rebooted firmware.'; return 0 ;;
          different-image)
            echo 'The controller booted another image; the requested update was not confirmed.' >&2
            return 1 ;;
          unverified)
            echo 'Update result unverified: firmware lacks bootId or running.imageSha256.' >&2
            return 1 ;;
        esac
        local last_reason last_error
        last_reason="$(obbw_ota_field confirmBlockReason)"
        last_error="$(obbw_ota_field confirmLastError)"
      fi
      sleep 2
    done
    echo 'The image was committed, but was not confirmed within 4 minutes.' >&2
    printf 'Last state: %s; confirmation reason: %s; ESP-IDF error: %s.\n' \
        "${OBBW_OTA_BOOT_RESULT:-unreachable}" "${last_reason:-unknown}" "${last_error:-unknown}" >&2
    return 1
  fi

  cat <<'EOF'

Commit accepted. The controller reboots once the machine is free.
Boot and confirmation have not been verified. Use --wait-for-confirmation to wait for them.
Confirmation runs automatically; opening the Web UI is not required.
EOF
  return 0
}
