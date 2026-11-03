#!/bin/sh
# Compatibility entry point. CMake/CTest owns incremental host compilation.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
"$root/scripts/dev" test normal
"$root/scripts/dev" test asan
"$root/scripts/dev" test tsan
"$root/scripts/dev" test ota
"$root/scripts/dev" test tooling
"$root/scripts/dev" test web
python3 "$root/scripts/check_architecture.py"
