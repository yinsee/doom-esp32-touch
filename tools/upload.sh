#!/usr/bin/env bash
#
# Upload the built firmware, retrying until it catches the port.
#
# This board provides its USB CDC from the application itself (CDCOnBoot=cdc).
# If the running firmware crashes, the serial device disappears with it and
# reappears on each reboot -- so a plain `arduino-cli upload` usually fails with
# "Device not configured" even though the board is fine. Retrying in step with
# the re-enumeration gets in during one of those windows.
#
# If every attempt fails, hold the BOOT button while replugging the board to
# force the ROM bootloader, then run this again.
#
# Usage: tools/upload.sh [port] [attempts]

set -uo pipefail

PORT="${1:-$("$(dirname "$0")/find_port.sh")}"
ATTEMPTS="${2:-12}"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,FlashMode=qio,PartitionScheme=custom"
BUILD=".build-tdoom"

cd "$(dirname "$0")/.."

for a in $(seq 1 "$ATTEMPTS"); do
  # Wait up to 10s for the port to exist before trying.
  for _ in $(seq 1 100); do [ -e "$PORT" ] && break; sleep 0.1; done

  if [ ! -e "$PORT" ]; then
    echo "attempt $a: port $PORT never appeared"
    continue
  fi

  OUT=$(arduino-cli upload -p "$PORT" --fqbn "$FQBN" --build-path "$BUILD" tdoom 2>&1)
  if echo "$OUT" | grep -q "Hash of data verified"; then
    echo "attempt $a: SUCCESS"
    echo "$OUT" | tail -4
    exit 0
  fi

  echo "attempt $a: $(echo "$OUT" | grep -Ei 'fatal|error|Errno' | head -1)"
  sleep 1
done

echo "all $ATTEMPTS attempts failed."
echo "Hold the BOOT button while replugging the board, then re-run this script."
exit 1
