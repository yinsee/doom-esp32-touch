#!/usr/bin/env python3
"""Reset the board and capture its serial output for a fixed window.

arduino-cli's monitor is unreliable on this board (see CLAUDE.md), and a bare
`cat` can't reset the ESP32-S3 to catch setup() output. Toggling DTR/RTS the way
esptool does gives us the boot log from the first line.

The port is opened with retries and reopened whenever it drops: this board uses
native USB CDC, so a crash takes the serial device down with it and the port
re-enumerates. Without reconnect logic a boot loop is invisible -- which is
exactly the case you most need the log for.

Usage: capture.py <seconds> [outfile]
"""
import sys
import time

import serial

import glob

def find_port():
    """The device name encodes USB topology, so it moves when the board is
    replugged into a different hub port -- this board has appeared as both
    usbmodem11201 and usbmodem1201. Never hardcode it."""
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no ESP32 serial port found (/dev/cu.usbmodem*)")
    return ports[0]


PORT = find_port()
BAUD = 115200

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
outfile = sys.argv[2] if len(sys.argv) > 2 else None

sink = open(outfile, "w") if outfile else None
deadline = time.time() + seconds
first_open = True
reconnects = 0


def emit(text):
    sys.stdout.write(text)
    sys.stdout.flush()
    if sink:
        sink.write(text)
        sink.flush()


while time.time() < deadline:
    try:
        ser = serial.Serial(find_port(), BAUD, timeout=0.2)
    except Exception:
        time.sleep(0.2)   # port gone: board resetting, wait for re-enumeration
        continue

    if first_open:
        # esptool's reset sequence: EN low with BOOT high, then release.
        try:
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.1)
            ser.setRTS(False)
            time.sleep(0.05)
            ser.reset_input_buffer()
        except Exception:
            pass
        first_open = False
    else:
        reconnects += 1
        emit("\n<<< serial dropped and reconnected (#%d) — the board reset >>>\n"
             % reconnects)

    try:
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                emit(chunk.decode("utf-8", errors="replace"))
    except Exception:
        pass          # device vanished mid-read: loop round and reopen
    finally:
        try:
            ser.close()
        except Exception:
            pass

if reconnects:
    emit("\n<<< %d reset(s) seen during capture — the board is boot looping >>>\n"
         % reconnects)
if sink:
    sink.close()
