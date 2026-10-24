#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${XMINI_HOST_BUILD_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/xmini-contracts.XXXXXX")}
sanitize=OFF
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize=ON
fi
set -- -S "$root/tests" -B "$out" -DCMAKE_BUILD_TYPE=Debug "-DXMINI_HOST_SANITIZERS=$sanitize"
idf=${ESP_IDF_PATH:-${IDF_PATH:-}}
if [ -n "$idf" ]; then
    set -- "$@" "-DESP_IDF_PATH=$idf"
fi
cmake "$@"
cmake --build "$out" --parallel
ctest --test-dir "$out" -L session --output-on-failure
printf 'Host contract outputs: %s\n' "$out"
