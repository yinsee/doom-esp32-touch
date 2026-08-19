#!/usr/bin/env bash
# Print the board's serial port.
#
# The device name is not stable -- it encodes the USB topology, so replugging
# into a different hub port moves it (this board has appeared as both
# usbmodem11201 and usbmodem1201). Everything else here calls this rather than
# hardcoding a name.
for p in /dev/cu.usbmodem*; do
  [ -e "$p" ] && { echo "$p"; exit 0; }
done
echo "no ESP32 serial port found (/dev/cu.usbmodem*)" >&2
exit 1
