#!/bin/sh
set -eu

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ai_temp_ota_tests.XXXXXX")
trap 'rm -rf "$test_tmp"' EXIT HUP INT TERM
cxx=${CXX:-c++}

# Focused functional suite: never regenerate firmware identity or run static checks.
for test_name in ota_image_host_test ota_transfer_host_test ota_state_concurrency_host_test; do
  "$cxx" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    "$test_dir/$test_name.cpp" -o "$test_tmp/$test_name"
  ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$test_tmp/$test_name"
done
bash "$test_dir/ota_cli_resilience_test.sh"
node "$test_dir/ota_web_resilience_test.js"
