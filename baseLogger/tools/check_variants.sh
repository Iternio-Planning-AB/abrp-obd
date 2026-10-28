#!/bin/sh
# Build the non-default config.h variants. Run from anywhere:
#   sh baseLogger/tools/check_variants.sh
# Needs `pio` on PATH (see AGENTS.md, Build).
#
# ENABLE_LOGGING 0, ENABLE_BLE 0 (a cellular-only build) and ENABLE_NETWORK 0 (no transport: ENABLE_CELL 0, with
# WiFi off by default) are built and linked: a link-only break (an unguarded call to a function the flag removes)
# shows up there. The WiFi variants compile the sketch alone (logger.ino.cpp.o), where the flags act, because
# with the Arduino WiFi stack beside BLE the image overflows iram0_0_seg by a few hundred bytes and cannot link.
set -u
cd "$(dirname "$0")/.." || exit 1
obj=.pio/build/esp32dev/src/logger.ino.cpp.o
fail=0
run() {
  echo "== $1"
  shift
  if ! PLATFORMIO_BUILD_FLAGS="$flags" pio run -e esp32dev "$@"; then
    echo "FAILED: $flags"
    fail=1
  fi
}
for flags in \
  "-DENABLE_LOGGING=0" \
  "-DENABLE_BLE=0" \
  "-DENABLE_CELL=0"; do
  run "$flags"
done
for flags in \
  "-DENABLE_WIFI=1 -DENABLE_CELL=0" \
  "-DENABLE_WIFI=1 -DENABLE_CELL=0 -DENABLE_LOGGING=0"; do
  run "$flags" -t "$obj"
done
exit $fail
