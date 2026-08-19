#!/usr/bin/env bash
#
# Write a Doom WAD into the raw `wad` flash partition.
#
# The WAD is not on a filesystem: port_wad.c memory-maps this partition with
# esp_partition_mmap, so lumps are read straight out of the flash cache and the
# WAD costs zero RAM. The offset below must match partitions.csv.
#
# Usage: tools/flash_wad.sh [path-to-doom1.wad] [port]

set -euo pipefail

WAD="${1:-doom1.wad}"
PORT="${2:-$("$(dirname "$0")/find_port.sh")}"
OFFSET=0x310000                 # `wad` partition start, see tdoom/partitions.csv
PART_SIZE=$((0x500000))         # 5 MiB
ESPTOOL="$HOME/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.0/esptool"

if [[ ! -f "$WAD" ]]; then
  echo "error: '$WAD' not found." >&2
  echo "Supply doom1.wad (freely redistributable shareware) in the project root," >&2
  echo "or pass its path as the first argument." >&2
  exit 1
fi

# Reject a file that isn't a WAD before spending 30s writing it to flash.
MAGIC=$(head -c 4 "$WAD")
if [[ "$MAGIC" != "IWAD" && "$MAGIC" != "PWAD" ]]; then
  echo "error: '$WAD' does not start with IWAD/PWAD (got '$MAGIC')." >&2
  exit 1
fi

SIZE=$(wc -c < "$WAD" | tr -d ' ')
if (( SIZE > PART_SIZE )); then
  echo "error: '$WAD' is $SIZE bytes but the wad partition is only $PART_SIZE." >&2
  echo "Enlarge the partition in tdoom/partitions.csv (and shrink 'storage')." >&2
  exit 1
fi

echo "Writing $WAD ($MAGIC, $SIZE bytes) to $OFFSET on $PORT ..."
"$ESPTOOL" --port "$PORT" write-flash "$OFFSET" "$WAD"
echo "Done. Reset the board; port_wad.c will map it at boot."
