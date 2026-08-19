# tdoom — yes, it runs Doom

Doom running on a **$15 ESP32-S3 touchscreen the size of a credit card**. Not a
demo, not a video stream from a PC — the real 1993 engine, rendering on the
microcontroller, at the panel's native width, playable with your thumbs.

**480×288 native · ~26 fps · 240 MHz dual-core · 8 MB PSRAM**

The WAD lives in flash and is memory-mapped, so the 4 MB of game data costs
**zero bytes of RAM**. Doom's zone heap gets 4 MB of PSRAM; the framebuffers and
the renderer's hot arrays sit in internal SRAM, where speed actually matters.

Engine is [doomgeneric](https://github.com/ozkl/doomgeneric) — Chocolate Doom
lineage, so it is vanilla-accurate rather than a reimplementation.

---

## The hardware

A **Guition JC3248W535C**, sold variously as "DIYMORE / Guition HMI W5 ESP32-S3
3.5 inch capacitive touch display". Around $11–18.

- [Manufacturer product page](https://www.guition.com/esp32-display-module/3-5-inch-esp32s3-display-module)
  — the durable reference
- [AliExpress listing](https://www.aliexpress.com/item/1005008495512979.html)
  — a concrete buy link (marketplace URLs rot; search the model number if it 404s)
- [atomic14's board write-up](https://www.atomic14.com/esp32/boards/guition-jc3248w535/)
  — independent specs and gotchas

| | |
|---|---|
| MCU | ESP32-S3, dual-core 240 MHz, 512 KB internal SRAM |
| Memory | 16 MB flash, 8 MB PSRAM |
| Panel | AXS15231B, 320×480 IPS, **QSPI** |
| Touch | AXS15231B integrated capacitive, 2 points, I²C `0x3B` |
| Extras | TF card slot, ~12 free GPIOs |

**No audio.** The board has no codec, amp or speaker, and the ESP32-S3 has no
DAC. Bluetooth speakers are not an option either — A2DP needs Classic Bluetooth
and the S3 is BLE-only. Adding sound means wiring an I²S amp such as a
MAX98357A.

## You supply the WAD

`doom1.wad` is **not** in this repository. The shareware WAD is freely
redistributable, but it is id Software's data and not ours to vendor. Drop your
copy in the project root.

## Quick start

```bash
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,FlashMode=qio,PartitionScheme=custom"

arduino-cli compile --fqbn "$FQBN" --libraries ./libraries --build-path .build-tdoom tdoom
tools/upload.sh                 # NOT a bare `arduino-cli upload` — see below
tools/flash_wad.sh doom1.wad    # once; writes the WAD to its own partition
python3 tools/capture.py 30     # watch it boot
```

`--libraries ./libraries` is **mandatory**: macOS TCC hides
`~/Documents/Arduino/libraries` from `arduino-cli`, which is also why the
library is vendored here.

Use `tools/upload.sh`, not a plain upload. The board serves its USB CDC from the
application, so a crashing build takes the serial port down with it and a normal
upload fails with "Device not configured". The script retries in step with the
re-enumeration.

## Controls

Hold it in **landscape**. The game fills the screen, so the touch zones are
invisible overlays. Both touch points are tracked independently.

```
+--------------------+--------------------+
|        ESC         |        MAP         |
+----------+---------+---------+----------+
|   FWD    |                   |          |
+----+-----+                   |   TURN   |
| SL | SR  |    unbound gap    |  L  |  R |
+----+-----+                   |          |
|   BACK   |                   |          |
+----------+---------+---------+----------+
|        USE         |        FIRE        |
+--------------------+--------------------+
```

The **movement pad** is three horizontal bands — forward across the top 40%,
back across the bottom 40%, and a narrow strafe strip (20%) between them.
Forward and back are what you hold, so they get the generous targets; strafing
is a deliberate sidestep, and a strip you have to aim for keeps it from firing
by accident while walking.

These are zones, not drag sticks — where you touch is what you get. A virtual
analog stick was built and rejected: on glass with no tactile centre, having to
drag before anything happens is slower and less certain than putting a thumb
straight on the direction you want.

**You rarely need FIRE or USE.** An aim assist steers you onto targets within
about ±17° and fires once lined up, and opens doors and switches in reach. It
uses Doom's own line-of-sight checks, skips corpses, and refuses to detonate a
barrel you are standing next to. The manual buttons cover what it deliberately
won't.

In menus the same geography applies: back sits in the ESC corner, the move pad
scrolls up/down, the turn pad nudges sliders, everything else selects. Menus
draw arrows over those zones and respond to a tap, not a hold.

## Making it fast

Getting from "it compiles" to 26 fps took four fixes, in order of how much they
were worth:

1. **Write the framebuffer sequentially.** Walking it in landscape order strides
   640 bytes per pixel, making every one of 153,600 writes its own PSRAM cache
   miss. Iterating the portrait buffer in its natural order — and pushing the
   strided access onto the source read, which is free because that buffer is
   internal SRAM — took it from **5.8 fps to 25 fps**.
2. **Store pixels pre-byte-swapped.** `Arduino_Canvas::flush()` byte-swaps all
   153,600 pixels on the CPU before every DMA chunk. Baking the swap into the
   256-entry palette LUT makes it free (256 swaps per palette change instead of
   153,600 per frame) and lets the flush be a plain DMA: **45.7 → 28.6 ms**.
3. **Triple buffering**, so the blit never waits on the QSPI transfer.
4. **Real dual-core overlap** — engine on core 1, flush on core 0.

Per-stage timings print to serial every 100 frames. Read the `wait` column
first: if it is large, the renderer is not the bottleneck and optimising it is
wasted effort. That is not hypothetical — halving render time once moved the
frame rate not at all.

**Graphic Detail (H/L, shown beside the on-screen fps)** controls how many
columns the 3D view computes: L renders 240 and doubles them, H renders all 480
for a true 1:1 image. **H is the default.**

## Layout

```
tdoom/tdoom.ino          hardware init + two-core frame pipeline
tdoom/partitions.csv     16MB layout: 3MB app, 5MB wad, 8MB FAT for saves
tdoom/src/port/          platform shim (video, input, wad, system)
tdoom/src/doomgeneric/   vendored engine, trimmed; local changes marked "tdoom:"
probe/probe.ino          hardware bring-up probe (touch, flush timing, SD)
tools/                   port detection, retrying uploader, serial capture
```

**`CLAUDE.md` is the file to read before changing anything.** It documents the
board's gotchas and, more usefully, the traps that cost real time — most of
which present as symptoms with no visible connection to their cause. A sample:
`printf` blocks forever when the USB CDC buffer fills and takes the whole
firmware down with it; the SPI driver quietly competes with the render
resolution for internal RAM; Doom's visplane clip arrays are bytes and overflow
above 255 rows.

## Licence

GPL-2.0, because the Doom engine is. See `LICENSE` and `NOTICE.md`.
