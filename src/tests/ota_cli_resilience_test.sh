#!/usr/bin/env bash
# Exercises retry/reconciliation policy without a controller or curl.
set -euo pipefail

repo_root="$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)"
# shellcheck source=scripts/openbrewbyweight_ota.sh
source "$repo_root/scripts/openbrewbyweight_ota.sh"

body_file="$(mktemp "${TMPDIR:-/tmp}/openbrewbyweight-ota-cli-test.XXXXXX")"
image_file="$(mktemp "${TMPDIR:-/tmp}/openbrewbyweight-ota-cli-image.XXXXXX")"
chunk_file="$(mktemp "${TMPDIR:-/tmp}/openbrewbyweight-ota-cli-chunk.XXXXXX")"
session_file="$(mktemp "${TMPDIR:-/tmp}/openbrewbyweight-ota-cli-session.XXXXXX")"
output_file="$(mktemp "${TMPDIR:-/tmp}/openbrewbyweight-ota-cli-output.XXXXXX")"
trap 'rm -f "$body_file" "$image_file" "$chunk_file" "$session_file" "$output_file"' EXIT
node -e 'require("fs").writeFileSync(process.argv[1],Buffer.alloc(8192,90))' "$image_file"
OBBW_OTA_BODY_FILE="$body_file"
OBBW_OTA_SESSION_BODY="$session_file"
OBBW_OTA_CHUNK_FILE="$chunk_file"
OBBW_OTA_IMAGE_ARCH=n16r8
OBBW_OTA_IMAGE_HARDWARE=legacy
OBBW_OTA_IMAGE_MACHINE=legacy
OBBW_OTA_IMAGE_VERSION=1.2.3
OBBW_OTA_IMAGE_PACKED=16908291
OBBW_OTA_IMAGE="$image_file"
OBBW_OTA_IMAGE_SIZE=8192
OBBW_OTA_IMAGE_SHA256="$(shasum -a 256 "$image_file" | awk '{print $1}')"
OBBW_OTA_TRANSFER_ID=0123456789abcdef0123456789abcdef0123
OBBW_OTA_CHUNK_BYTES=4096
OBBW_OTA_RANGE_ATTEMPTS=3
OBBW_OTA_COMMIT_ATTEMPTS=3

failures=0
check() {
  if ! "$@"; then
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
  fi
}

mock_mode=""
mock_patches=0
mock_offset=0
obbw_ota_request() {
  local method="$1" path="$2"
  if [[ "$method" == "POST" && "$path" == "/api/v1/ota/session" ]]; then
    if [[ "$mock_mode" == "session-refused" ]]; then
      printf '{"error":"OTA_SESSION_CONFLICT","message":"A different image owns the slot."}' > "$OBBW_OTA_BODY_FILE"
      OBBW_OTA_CURL_EXIT=0; OBBW_OTA_HTTP_STATUS=409; return 0
    fi
    local response_transfer="$OBBW_OTA_TRANSFER_ID"
    if [[ "$mock_mode" == "wrong-transfer" ]]; then
      response_transfer=another-transfer
    fi
    printf '{"otaProtocolVersion":3,"state":"receiving","transferId":"%s","sha256":"%s","expectedBytes":8192,"sessionArch":"n16r8","sessionHardware":"legacy","sessionMachine":"legacy","sessionVersion":"1.2.3","nextOffset":%s,"chunkBytes":4096}' "$response_transfer" "$OBBW_OTA_IMAGE_SHA256" "$mock_offset" > "$OBBW_OTA_BODY_FILE"
    OBBW_OTA_CURL_EXIT=0; OBBW_OTA_HTTP_STATUS=200; return 0
  fi
  if [[ "$method" == "PATCH" && "$path" == "/api/v1/ota" ]]; then
    mock_patches=$((mock_patches + 1))
    local header sent_offset=""
    for header in "$@"; do case "$header" in 'X-OTA-Offset: '*) sent_offset="${header#X-OTA-Offset: }";; esac; done
    mock_ranges="${mock_ranges:-}${sent_offset},"
    if [[ "$mock_mode" == "rewind" && "$mock_patches" == "2" ]]; then
      mock_offset=0
      printf '{}' > "$OBBW_OTA_BODY_FILE"
      OBBW_OTA_CURL_EXIT=56; OBBW_OTA_HTTP_STATUS=000; return 1
    fi
    if [[ "$mock_mode" == "stalled" || "$mock_mode" == "changed" ]]; then
      printf '{}' > "$OBBW_OTA_BODY_FILE"
      OBBW_OTA_CURL_EXIT=56; OBBW_OTA_HTTP_STATUS=000; return 1
    fi
    if [[ "$mock_mode" == "image-rewritten" ]]; then
      printf 'corrupted' > "$OBBW_OTA_IMAGE"
    fi
    if [[ "$mock_mode" == "retry" && "$mock_patches" == "1" ]]; then
      mock_offset=4096
      printf '{}' > "$OBBW_OTA_BODY_FILE"
      OBBW_OTA_CURL_EXIT=56
      OBBW_OTA_HTTP_STATUS=000
      return 1
    fi
    if [[ "$mock_mode" == "safety" ]]; then
      printf '{"error":"SAFETY_LOST"}' > "$OBBW_OTA_BODY_FILE"
      OBBW_OTA_CURL_EXIT=0
      OBBW_OTA_HTTP_STATUS=409
      return 0
    fi
    if [[ "$mock_mode" == "invalid-patch-offset" ]]; then
      printf '{"state":"receiving","transferId":"%s","sha256":"%s","nextOffset":8193}' "$OBBW_OTA_TRANSFER_ID" "$OBBW_OTA_IMAGE_SHA256" > "$OBBW_OTA_BODY_FILE"
      OBBW_OTA_CURL_EXIT=0
      OBBW_OTA_HTTP_STATUS=200
      return 0
    fi
    mock_offset=$((mock_offset + 4096))
    if (( mock_offset >= 8192 )); then
      printf '{"state":"staged","transferId":"%s","sha256":"%s","expectedBytes":8192,"otaProtocolVersion":3,"sessionArch":"n16r8","sessionHardware":"legacy","sessionMachine":"legacy","sessionVersion":"1.2.3","nextOffset":8192,"staged":{"arch":"n16r8","hardware":"legacy","machine":"legacy","version":"1.2.3","packed":16908291}}' "$OBBW_OTA_TRANSFER_ID" "$OBBW_OTA_IMAGE_SHA256" > "$OBBW_OTA_BODY_FILE"
    else
      printf '{"state":"receiving","transferId":"%s","sha256":"%s","expectedBytes":8192,"otaProtocolVersion":3,"sessionArch":"n16r8","sessionHardware":"legacy","sessionMachine":"legacy","sessionVersion":"1.2.3","nextOffset":%s}' "$OBBW_OTA_TRANSFER_ID" "$OBBW_OTA_IMAGE_SHA256" "$mock_offset" > "$OBBW_OTA_BODY_FILE"
    fi
    OBBW_OTA_CURL_EXIT=0
    OBBW_OTA_HTTP_STATUS=200
    return 0
  fi
  if [[ "$method" == "POST" && "$path" == "/api/v1/ota/flash" ]]; then
    mock_commits=$((${mock_commits:-0} + 1))
    printf '{}' > "$OBBW_OTA_BODY_FILE"
    OBBW_OTA_CURL_EXIT=56
    OBBW_OTA_HTTP_STATUS=000
    return 1
  fi
  if [[ "$method" == "GET" && "$path" == "/api/v1/ota/session" ]]; then
    printf '{"otaProtocolVersion":3,"state":"receiving","transferId":"%s","sha256":"%s","expectedBytes":8192,"sessionArch":"n16r8","sessionHardware":"legacy","sessionMachine":"legacy","sessionVersion":"1.2.3","nextOffset":%s}' "$OBBW_OTA_TRANSFER_ID" "$OBBW_OTA_IMAGE_SHA256" "$mock_offset" > "$OBBW_OTA_BODY_FILE"
    if [[ "$mock_mode" == "changed" ]]; then printf '{"transferId":"other"}' > "$OBBW_OTA_BODY_FILE"; fi
    OBBW_OTA_CURL_EXIT=0; OBBW_OTA_HTTP_STATUS=200; return 0
  fi
  if [[ "$method" == "GET" && "$path" == "/api/v1/ota" ]]; then
    if [[ "$mock_mode" == "lost-commit" ]]; then
      printf '{"state":"committed","restartPending":true,"transferId":"%s","sha256":"%s","expectedBytes":8192,"sessionArch":"n16r8","sessionHardware":"legacy","sessionMachine":"legacy","sessionVersion":"1.2.3","otaProtocolVersion":3}' "$OBBW_OTA_TRANSFER_ID" "$OBBW_OTA_IMAGE_SHA256" > "$OBBW_OTA_BODY_FILE"
    elif [[ "$mock_mode" == "wrong-commit" ]]; then
      printf '{"state":"committed","restartPending":true,"transferId":"other"}' > "$OBBW_OTA_BODY_FILE"
    else
      printf '{"state":"idle","safe":true}' > "$OBBW_OTA_BODY_FILE"
    fi
    OBBW_OTA_CURL_EXIT=0
    OBBW_OTA_HTTP_STATUS=200
    return 0
  fi
  return 1
}

# Avoid real backoff in the policy test.
obbw_ota_backoff() { :; }

mock_mode=retry
mock_patches=0
mock_offset=0
obbw_ota_upload
check test "$mock_patches" -eq 2

mock_mode=rewind
mock_patches=0
mock_offset=0
mock_ranges=""
obbw_ota_upload
check test "$mock_ranges" = '0,4096,0,4096,'

for mock_mode in stalled changed; do
  mock_patches=0; mock_offset=0
  if obbw_ota_upload; then
    echo "FAIL: $mock_mode upload accepted" >&2
    failures=$((failures + 1))
  fi
  if [[ "$mock_mode" == stalled ]]; then check test "$mock_patches" -eq 3
  else check test "$mock_patches" -eq 1
  fi
done

mock_mode=image-rewritten
mock_patches=0
mock_offset=0
if obbw_ota_upload > "$output_file" 2>&1; then
  echo 'FAIL: a rewritten image was uploaded' >&2
  failures=$((failures + 1))
fi
check test "$mock_patches" -eq 1
case "$(<"$output_file")" in
  *'The firmware image changed while the upload was running'*) ;;
  *) echo 'FAIL: a rewritten image was not diagnosed' >&2; failures=$((failures + 1)) ;;
esac
# The rewritten image above must not leak into the remaining cases.
node -e 'require("fs").writeFileSync(process.argv[1],Buffer.alloc(8192,90))' "$OBBW_OTA_IMAGE"

mock_mode=safety
mock_patches=0
mock_offset=0
if obbw_ota_upload; then
  echo 'FAIL: SAFETY_LOST was retried/accepted' >&2
  failures=$((failures + 1))
fi
check test "$mock_patches" -eq 1

mock_mode=lost-commit
obbw_ota_commit

mock_mode=wrong-commit
mock_commits=0
if obbw_ota_commit; then
  echo 'FAIL: commit belonging to another image was accepted' >&2
  failures=$((failures + 1))
fi
check test "$mock_commits" -eq 1

mock_mode=session-refused
if refused_output="$(obbw_ota_upload 2>&1)"; then
  echo 'FAIL: refused OTA session was accepted' >&2
  failures=$((failures + 1))
fi
case "$refused_output" in
  *'HTTP=409 code=OTA_SESSION_CONFLICT message=A different image owns the slot.'*) ;;
  *)
    echo 'FAIL: refused OTA session lost its actionable server error' >&2
    failures=$((failures + 1))
    ;;
esac

mock_mode=wrong-transfer
mock_patches=0
mock_offset=0
if obbw_ota_upload > "$output_file" 2>&1; then
  echo 'FAIL: a different transferId was accepted after session creation' >&2
  failures=$((failures + 1))
fi
check test "$mock_patches" -eq 0
case "$(<"$output_file")" in
  *'different or incomplete OTA session identity'*) ;;
  *) echo 'FAIL: mismatched transferId was not diagnosed' >&2; failures=$((failures + 1)) ;;
esac

mock_mode=invalid-start-offset
mock_patches=0
mock_offset=8193
if obbw_ota_upload > "$output_file" 2>&1; then
  echo 'FAIL: out-of-bounds initial OTA offset was accepted' >&2
  failures=$((failures + 1))
fi
invalid_start_output="$(<"$output_file")"
check test "$mock_patches" -eq 0
case "$invalid_start_output" in
  *'invalid nextOffset=8193 for image size 8192'*) ;;
  *) echo 'FAIL: invalid initial offset was not diagnosed' >&2; failures=$((failures + 1)) ;;
esac

mock_mode=invalid-patch-offset
mock_patches=0
mock_offset=0
if obbw_ota_upload > "$output_file" 2>&1; then
  echo 'FAIL: out-of-bounds PATCH offset was accepted' >&2
  failures=$((failures + 1))
fi
invalid_patch_output="$(<"$output_file")"
check test "$mock_patches" -eq 1
case "$invalid_patch_output" in
  *'invalid nextOffset=8193 after byte 0'*) ;;
  *) echo 'FAIL: invalid PATCH offset was not diagnosed' >&2; failures=$((failures + 1)) ;;
esac

printf '{"otaProtocolVersion":3,"state":"receiving","sessionActive":true,"transferId":"existing-transfer","sha256":"%s","expectedBytes":8192,"sessionArch":"n16r8","sessionHardware":"legacy","sessionMachine":"legacy","sessionVersion":"1.2.3"}' \
    "$OBBW_OTA_IMAGE_SHA256" > "$OBBW_OTA_BODY_FILE"
check obbw_ota_remote_owns_slot
check obbw_ota_remote_matches_image
printf '{"otaProtocolVersion":3,"state":"receiving","sessionActive":true,"transferId":"existing-transfer","sha256":"%s","expectedBytes":8192,"sessionArch":"n16r8","sessionHardware":"legacy","sessionMachine":"legacy"}' \
    "$OBBW_OTA_IMAGE_SHA256" > "$OBBW_OTA_BODY_FILE"
if obbw_ota_remote_matches_image; then
  echo 'FAIL: incomplete v2 session identity matched the image' >&2
  failures=$((failures + 1))
fi
OBBW_OTA_IMAGE_SHA256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
if obbw_ota_remote_matches_image; then
  echo 'FAIL: a different image matched the resumable session' >&2
  failures=$((failures + 1))
fi
OBBW_OTA_IMAGE_SHA256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
printf '{"otaProtocolVersion":2,"state":"receiving","sessionActive":true,"transferId":"old-server-transfer","sha256":"%s","expectedBytes":8192,"nextOffset":4096,"chunkBytes":4096,"running":{"arch":"n16r8","version":"1.2.2"}}' \
    "$OBBW_OTA_IMAGE_SHA256" > "$OBBW_OTA_BODY_FILE"
if obbw_ota_protocol_supported; then
  echo 'FAIL: profile-unaware OTA protocol v2 was accepted' >&2
  failures=$((failures + 1))
fi
if obbw_ota_remote_matches_image; then
  echo 'FAIL: legacy session missing architecture/version was adopted' >&2
  failures=$((failures + 1))
fi
printf '{"state":"idle","available":true}' > "$OBBW_OTA_BODY_FILE"
if obbw_ota_protocol_supported; then
  echo 'FAIL: monolithic legacy OTA was accepted as resumable' >&2
  failures=$((failures + 1))
fi

# Conflict decision matrix for a different image owning the update slot.
obbw_can_prompt() { return "${mock_can_prompt:-1}"; }
obbw_ota_confirm_discard() { return "${mock_discard_answer:-0}"; }
printf '{"otaProtocolVersion":3,"state":"receiving","sessionActive":true,"transferId":"existing-transfer","sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","expectedBytes":8192,"nextOffset":4096,"sessionArch":"n16r8","sessionHardware":"legacy","sessionMachine":"legacy","sessionVersion":"1.0.0"}' > "$OBBW_OTA_BODY_FILE"
obbw_ota_explain_conflict 2> "$output_file"
explain_output="$(<"$output_file")"
case "$explain_output" in
  *'interrupted upload at 50%'*) ;;
  *) echo 'FAIL: conflict explanation lost the upload progress' >&2
     failures=$((failures + 1)) ;;
esac
case "$explain_output" in
  *$'\033'*)
    echo 'FAIL: conflict explanation emitted color without a terminal' >&2
    failures=$((failures + 1)) ;;
esac
printf '{"otaProtocolVersion":3,"state":"staged","transferId":"existing-transfer","sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","expectedBytes":8192,"nextOffset":8192,"sessionArch":"n16r8","sessionHardware":"legacy","sessionMachine":"legacy","sessionVersion":"1.0.0"}' > "$OBBW_OTA_BODY_FILE"
obbw_ota_explain_conflict 2> "$output_file"
case "$(<"$output_file")" in
  *'waiting for its commit'*) ;;
  *) echo 'FAIL: staged conflict was not described as pending commit' >&2
     failures=$((failures + 1)) ;;
esac
printf '{"otaProtocolVersion":3,"state":"receiving","sessionActive":true,"transferId":"existing-transfer","sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","expectedBytes":8192,"nextOffset":4096,"sessionArch":"n16r8","sessionHardware":"legacy","sessionMachine":"legacy","sessionVersion":"1.0.0"}' > "$OBBW_OTA_BODY_FILE"
obbw_ota_conflict_decision 0 1 > /dev/null 2>&1
check test "$OBBW_OTA_DISCARD_REASON" = 'requested with --discard-ota-session'
obbw_ota_conflict_decision 1 0 > /dev/null 2>&1
check test "$OBBW_OTA_DISCARD_REASON" = 'assumed through --yes'
if obbw_ota_conflict_decision 0 0 > "$output_file" 2>&1; then
  echo 'FAIL: conflict was discarded without --yes or a terminal' >&2
  failures=$((failures + 1))
fi
case "$(<"$output_file")" in
  *'was kept. To replace it, re-run with --discard-ota-session'*) ;;
  *) echo 'FAIL: keeping the remote image lost its guidance' >&2
     failures=$((failures + 1)) ;;
esac
mock_can_prompt=0 mock_discard_answer=1
if obbw_ota_conflict_decision 0 0 > "$output_file" 2>&1; then
  echo 'FAIL: a declined discard still continued' >&2
  failures=$((failures + 1))
fi
case "$(<"$output_file")" in
  *'was kept'*) ;;
  *) echo 'FAIL: a declined discard lost its keep notice' >&2
     failures=$((failures + 1)) ;;
esac
mock_can_prompt=0 mock_discard_answer=0
obbw_ota_conflict_decision 0 0 > /dev/null 2>&1
check test "$OBBW_OTA_DISCARD_REASON" = 'confirmed interactively'
unset mock_can_prompt mock_discard_answer

# The real POST body must contain only the seven public session fields.
obbw_ota_write_session_body
check node -e 'const x=JSON.parse(require("fs").readFileSync(process.argv[1]));if(Object.keys(x).sort().join()!=="arch,hardware,machine,sha256,size,transferId,version")process.exit(1)' "$OBBW_OTA_SESSION_BODY"

OBBW_OTA_PRE_BOOT_ID=7
OBBW_OTA_IMAGE_DIGEST=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
for scenario in same-boot wrong-binary pending confirmed legacy; do
  boot=8; digest="$OBBW_OTA_IMAGE_DIGEST"; confirmed=true; result=confirmed
  case "$scenario" in
    same-boot) boot=7; result=restart-pending ;;
    wrong-binary) digest="$OBBW_OTA_IMAGE_SHA256"; result=different-image ;;
    pending) confirmed=false; result=confirmation-pending ;;
    legacy) digest=""; result=unverified ;;
  esac
  printf '{"bootId":%s,"confirmed":%s,"running":{"version":"1.2.3","arch":"n16r8","imageSha256":"%s"}}' \
      "$boot" "$confirmed" "$digest" > "$OBBW_OTA_BODY_FILE"
  obbw_ota_boot_result
  check test "$OBBW_OTA_BOOT_RESULT" = "$result"
done

if (( failures != 0 )); then
  exit 1
fi
echo 'OTA CLI resilience: retry and reconciliation policy OK'
