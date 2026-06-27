#!/usr/bin/env bash
# Build helper for ESP-IDF v3.3.1 (legacy GNU Make). Self-locating -> the repo is portable.
# Run inside msys64 MINGW32:  MSYSTEM=MINGW32 /c/msys64/usr/bin/bash -l build_idf.sh <make-args...>
set -e
export IDF_PATH="${IDF_PATH:-/c/Users/97254/esp/v3.3.1/esp-idf}"
export PATH="/c/Users/97254/esp/toolchains/xtensa-esp32-elf/bin:$PATH"
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJ"
echo "PROJECT=$(basename "$PROJ")  IDF_PATH=$IDF_PATH  MSYSTEM=$MSYSTEM"
echo "============================================================"
exec make "$@"
