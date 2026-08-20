# tdoom

Doom (shareware `doom1.wad`) on a **Guition JC3248W535C** — ESP32-S3, 16MB flash,
8MB OPI PSRAM, AXS15231B 320×480 QSPI panel with integrated capacitive touch.

Engine is [doomgeneric](https://github.com/ozkl/doomgeneric) (Chocolate Doom
lineage), vendored and trimmed into `tdoom/src/doomgeneric/`. Our platform layer
is `tdoom/src/port/`.

## Build and upload

```bash
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,FlashMode=qio,PartitionScheme=custom"

arduino-cli compile --fqbn "$FQBN" --libraries ./libraries --build-path .build-tdoom tdoom
tools/upload.sh                 # NOT a bare `arduino-cli upload` -- see below

# serial capture (arduino-cli monitor is unreliable here; this resets the board first)
python3 tools/capture.py 30
```

**Use `tools/upload.sh`, not a bare `arduino-cli upload`.** This board serves its
USB CDC from the application, so if the running firmware crashes the port
disappears with it and reappears on each reboot — a plain upload then fails with
"Device not configured" even though the board is fine. The script retries in step
with the re-enumeration. Same problem applies to `tools/flash_wad.sh`; if it
cannot get in, hold BOOT while replugging to reach the ROM bootloader.

`--libraries ./libraries` is **mandatory** — see gotcha 7.

Flash the WAD once (you supply `doom1.wad`):

```bash
tools/flash_wad.sh doom1.wad
```

## Board gotchas — all verified on hardware

These were established by bringing this board up and by what `probe/probe.ino`
measures here. Ignoring any of them costs hours.

1. **TFT_eSPI cannot drive this panel.** AXS15231B is QSPI-only and TFT_eSPI has
   no QSPI bus support. Use Arduino_GFX. Do not resurrect `User_Setup.h`.
2. **Partial draws are broken.** The panel's 0x2A/0x2B window-address commands
   misbehave, so every frame is a full 320×480 push.
3. **Never overclock the QSPI bus.** `begin()` takes no clock argument.
   `begin(80000000)` corrupts the panel; `begin(60000000)` drops it to ~1 fps.
4. **`Serial.setTxTimeoutMs(0)` immediately after `Serial.begin()`.** Otherwise
   USB CDC writes block ~1 s each once a monitor detaches and everything
   collapses to 1 fps.
5. **`Wire.setTimeOut(25)`.** At 5 ms the touch reads truncate and return `0xC8`
   filler that decodes as plausible-but-wrong finger counts.
6. **GPIO 33–37 are the octal PSRAM pins; 26–32 are flash.** Driving any of them
   reboots the chip with `TG1WDT_SYS_RST`. An early probe used 35/36/37 for an SD
   card test and boot-looped.
7. **macOS TCC hides `~/Documents/Arduino/libraries` from arduino-cli**
   (readdir denied). Libraries are vendored in `libraries/` and must be passed
   with `--libraries ./libraries`.

## Measured performance

**~26 fps in-game at 480x288 native (low detail), scaled to the 480x320 panel.** Per-stage timing is printed every 100
frames by `port_video.c`:

```
[tdoom] 28.6 fps | render 12 ms  wait 11 ms  blit 11 ms
```

`wait` is time core 1 spends blocked on core 0 releasing a buffer. **It is the
number to read first** — if it is large, optimising the renderer is wasted
effort. That is not hypothetical: setting `detailLevel = 1` halved render time
(27 -> 12 ms) and moved the frame rate *not at all*, because the pipeline was
already the constraint.

Panel flush path:

| path | ms/frame | fps |
|---|---|---|
| `Arduino_Canvas::flush()` | 45.7 | 21.9 |
| `draw16bitBeRGBBitmap()` (pre-swapped buffer) | **28.6** | **35.0** |

What actually moved the needle, in order:

1. **Sequential PSRAM writes in the blit (5.8 -> 25 fps).** Walking the
   framebuffer in landscape order strides `PANEL_W` (640 bytes) per pixel, so
   every write is its own cache-line miss — 153600 per frame. Iterating the
   portrait buffer in its natural order makes writes sequential and pushes the
   strided access onto the source read, which is free because `I_VideoBuffer` is
   internal SRAM. **If you touch `DG_DrawFrame`, preserve this property.**
2. **Pre-byte-swapped framebuffer (45.7 -> 28.6 ms/frame).** See below.
3. **Triple buffering (24.7 -> 28.6 fps).** With two buffers the blit cannot
   start until the flush releases the other one, making frame time
   `flush + blit` (37.6 ms) instead of `max()`. A third buffer costs 300 KB of
   PSRAM and removes that serialisation.
4. **Real dual-core overlap.** The first version signalled the flush task then
   immediately blocked on it, and read the buffer index after flipping it — so
   it neither overlapped nor was safe.

**`FlushTask` must call `vTaskDelay(1)`.** With three buffers a frame is always
queued, so the task never blocks, core 0 starves `IDLE0`, and the task watchdog
reboots the board (`task_wdt: - IDLE0 (CPU 0)`). One tick against a 28.6 ms
flush costs ~3%.

Remaining ceiling: frame period is ~35 ms against a ~29 ms flush. The gap is
PSRAM contention — the blit writes 300 KB to PSRAM while the DMA reads 300 KB
out of it, which is also why `blit` rose from 9 to 11 ms once the two genuinely
overlapped.

`Arduino_Canvas::flush()` -> `writePixels()` byte-swaps all 153600 pixels **on
the CPU** into a bounce buffer before each DMA chunk
(`Arduino_ESP32QSPI.cpp:346`). That CPU work, not the QSPI bus, was the
bottleneck — raising the chunk size from 1024 to 8192 pixels (150 transactions
down to 18) bought only 2 ms, but pre-swapping the buffer bought 17 ms.

So tdoom stores its framebuffers **already byte-swapped** and pushes them with
`draw16bitBeRGBBitmap()`, a straight DMA. The swap is free: the palette LUT in
`port_video.c` bakes it in, so it happens 256 times per palette change instead
of 153600 times per frame.

## Aim assist

Touch turning is far too coarse to line up a shot by hand, so the game aims for
you. `G_BuildTiccmd` calls `TD_AimAssist()` (`p_map.c`) every tic; it sweeps for
a target, steers toward it, and fires once lined up. `TD_UsableInFront()` does
the same for doors and switches. Both reuse Doom's own traversal
(`P_AimLineAttack`, and a report-only clone of `PTR_UseTraverse`), so "is there
a target" means exactly what it means to the engine.

### How the sweep works

`angle_t` spans a full circle in 32 bits, so `1<<26` is 1/64 of a circle =
**5.625 degrees** — the same step `P_BulletSlope` uses. The assist sweeps
`TD_AIM_SWEEP_STEPS` (3) either side, giving ~±16.9°.

It sweeps **outward from the crosshair and stops at the first hit**, so the
target nearest the crosshair wins and a target dead ahead costs one traversal
rather than seven. That matters: `P_PathTraverse` is not cheap and this runs
every tic.

### Steering

`p_user.c:147` does `angle += cmd->angleturn<<16`, so `angleturn` is the top 16
bits of an `angle_t` and a signed shift converts a bearing difference straight
into it. The correction is clamped to `TD_AIM_MAX_TURN` (800 units ≈ 4.4°/tic ≈
154°/sec) so it reads as turning rather than snapping.

**Assist steering only applies when `cmd->angleturn == 0`.** Manual input always
wins — an assist that fights the thumb feels broken. Turn roughly toward
something, let go, and the assist finishes the job.

It fires only when the target is inside `TD_AIM_FIRE_CONE` (one step, matching
Doom's own autoaim). Shooting mid-swing would just miss: turn first, then shoot.

### Things that are easy to get wrong here

- **Sweep at `TD_AUTOAIM_RANGE` (16*64 units), not `MISSILERANGE` (32*64).**
  16*64 is what `P_BulletSlope` uses. `MISSILERANGE` engaged targets at twice
  the distance the player's aim assist would, emptying ammo into things across
  the level — and cost frame time, since traversal walks blockmap cells
  proportional to distance (render 23–28ms -> 16–18ms when halved).
- **Check the target is alive.** `TD_WorthShooting()` requires `health > 0` and
  `MF_SHOOTABLE`. `P_KillMobj` clears that flag and `PTR_AimTraverse` skips
  things without it, so corpses should never be picked — but making it explicit
  also skips things mid-death-animation.
- **Don't detonate a barrel you are standing next to.** Barrels call `A_Explode`
  -> `P_RadiusAttack(..., 128)`: 128 damage over a 128-unit radius. The assist
  refuses barrels closer than `TD_BARREL_SAFE_DIST` (192 units, a 1.5x margin).
  Manual fire is unaffected. Side effect: a close barrel between you and an
  enemy blocks the assist entirely — the safe failure, but you may need to step
  aside or fire manually.

Holding `BT_USE` is safe: `P_PlayerThink` acts on the press edge via
`player->usedown`, so a door opens once rather than repeatedly.

## Traps that cost real time here

All confirmed on hardware. Each presented as a symptom with no obvious
connection to its cause.

### printf blocks and hangs the whole firmware

`printf` goes to stdout, which here is the USB CDC. **`Serial.setTxTimeoutMs(0)`
does NOT apply to stdout** — it only affects the Arduino `Serial` object. Once
the CDC buffer fills, `printf` blocks forever and every task that logs stops
with it.

Symptom: the board goes silent at the same point every run, with **no panic, no
reboot, and the port still enumerated**. It looks exactly like a hang in
whatever you were instrumenting. Per-frame logging caused this; the attract mode
was never broken, it just could not report progress. Keep logging sparse (health
report is every 5s) and never log per frame.

### Overlays must be drawn BEFORE TD_SubmitFrame()

`TD_SubmitFrame()` hands the buffer to core 0, which immediately starts the
QSPI DMA out of it. Anything written after that races the flush: the DMA reads
some rows before the write and some after, so button borders blink and plates
appear half-drawn, differently every frame.

It looks like a drawing bug in the overlay code — it is not, the geometry is
fine and a torn read just makes it look wrong. The submit call sat above the
overlay block for a while with a comment directly under it claiming the
opposite.

### The SPI driver competes with the render resolution for internal RAM

The framebuffers live in PSRAM, so `spi_master` allocates an internal DMA bounce
buffer **the size of one chunk** on every transfer. At the old 8192-px chunk
that is 16KB per transfer.

Raising the render resolution to 480 wide grew `I_VideoBuffer` from 64,000 to
138,240 bytes and left only ~14KB of DMA-capable heap. The 16KB request failed
and the driver asserted inside `spi_device_polling_end` — a boot loop with no
visible connection to the resolution change that caused it.
`ESP32QSPI_MAX_PIXELS_AT_ONCE` is 1024 for this reason; chunk size is worth ~2ms
at most.

### Task creation fails silently

When the framebuffer reservation left no contiguous block for the Doom task's
stack, the task never started: firmware ran, panel initialised, health log
ticked over, **blank screen, no error anywhere**. It also made
`uxTaskGetStackHighWaterMark(handle)` return 0, which reads convincingly like a
stack overflow and sends you chasing the wrong bug. Both task creations are
checked and call `Fatal()` now.

### Allocate the framebuffer FIRST

`I_VideoBuffer` needs one contiguous block. Allocated where upstream does it
(inside `I_InitGraphics`) it fails even with plenty free, because the QSPI DMA
buffers and task stacks have already fragmented the large DRAM region. The
sketch reserves it at the top of `setup()`.

### Doom's 2D is authored for 320x200

Menus, title, credits, intermission and the status bar are drawn at fixed
320x200 coordinates. Widening the screen leaves them in the top-left corner
surrounded by whatever was in RAM.

That one fact produced three symptoms that looked unrelated: a menu that did not
fill the screen, "noise pixels" outside the image, and "blinking" — the attract
mode alternating a 320x200 title patch against a full-width 3D render.

`V_DrawPatch`/`V_DrawPatchFlipped` scale from `V_BASEW`x`V_BASEH` to the real
screen, but **only for full-screen draws** (`dest_screen == I_VideoBuffer`); the
small backing buffers use their own coordinate space. The status bar was moved
off `st_backing_screen` (block-copied with `V_CopyRect`, which does no scaling)
so bar and widgets scale together. The scaler uses precomputed edge tables —
dividing inline cost 18ms -> 28ms of render time.

### Resolution must have exactly one definition

It was duplicated across `i_video.h`, `doomgeneric.h` and the sketch's
framebuffer reservation, and drifted: the engine ran 320x200 while the sketch
reserved a 480x320 buffer. The oversized reservation fragmented internal RAM
until the Doom task could not be created. `src/port/td_res.h` is now the only
place it — and the touch zone geometry — is defined.

### Renderer capacities are hardcoded for a 320x168 view

`R_DrawPlanes` checks for opening overflow *after* `R_StoreWallRange` has
already written past the array, so the failure mode is silent corruption —
artifacts while moving — not a clean `I_Error`.

| limit | vanilla | here | why |
|---|---|---|---|
| `MAXOPENINGS` | `SCREENWIDTH*64` | scaled by `SCREENHEIGHT/V_BASEH` | the 64 is a PER-COLUMN budget for a 168-row view |
| `MAXVISPLANES` | 128 | 256 | ~1.5x the geometry per frame |
| `MAXDRAWSEGS` | 256 | 512 | a 480-wide view crosses more segs |
| `MAXVISSPRITES` | 128 | 256 | same |

`MAXOPENINGS` only overflows at maximum screen size, because that is when the
view goes full height. All four live in PSRAM, so headroom costs ~380KB.

### Weapon sprites are placed for a 168-row view

`pspritescale` is `viewwidth/SCREENWIDTH` upstream, which is 1.0 at full screen
— correct only when the screen really is 320x200. On a taller view the weapon
renders at its original size and floats above the status bar. It is
`viewwidth/V_BASEW` here. The horizontal (1.50) and vertical (1.43) ratios
differ and Doom has only ONE psprite scale, so this errs ~5% large on purpose:
the weapon tucks behind the status bar rather than floating.

### Screen size 11 hides the status bar — that is vanilla

`screenblocks == 11` means "fullscreen view, no status bar", and this engine has
no always-on HUD to replace it. Not a bug.

### SCREENWIDTH means two different things

Vanilla Doom assumes `SCREENWIDTH == 320`, so its 2D code uses `SCREENWIDTH`
freely for layout. Here the screen is 480 wide but `V_DrawPatch` scales from a
320x200 DESIGN space, so those two are no longer the same thing and every use
has to be classified by **drawing path**:

| code path | coordinate space | constant |
|---|---|---|
| feeds `V_DrawPatch` | design 320x200 | `V_BASEW` / `V_BASEH` |
| writes straight into the framebuffer | real screen | `SCREENWIDTH` / `SCREENHEIGHT` |

Get it wrong and it fails silently. `wi_stuff.c` computed the stat percentages
at `SCREENWIDTH - SP_STATSX` = 430, past the 320 design width, so the scaler
skipped every column and the intermission screen showed **no percentages at
all** — while the level name and times were merely misplaced by 1.5x and looked
plausible enough to miss.

Fixed in `wi_stuff.c`, `m_menu.c`, `f_finale.c` and `st_stuff.c`. Deliberately
NOT fixed:

- `am_map.c` — the automap draws lines directly into `I_VideoBuffer`, so
  `finit_width = SCREENWIDTH` is correct.
- `f_finale.c`'s background tiling and `F_DrawPatchCol` — raw framebuffer
  writes, likewise real screen space. Only its three `V_DrawPatch` call sites
  were changed.
- `wi_stuff.c:442` — a fake patch that exists *to trigger* a `V_DrawPatch`
  error on Doom II MAP33+. Shareware Doom 1 never reaches it, and "fixing" a
  deliberate error path would be wrong.

## The status bar is repainted in full every frame

Upstream diff-draws it: each widget erases its old value by copying the clean
bar graphic back out of `st_backing_screen`, then redraws. That depends on the
bar living in that buffer at 1:1.

The bar is drawn *scaled* straight to the screen here — `V_CopyRect` does no
scaling, so the backing-buffer route could not survive — which left the widgets
with no working erase. Ammo, health and the face piled new digits on top of old
ones.

`ST_Drawer` now always calls `ST_doRefresh()`, the three `V_CopyRect` erases in
`st_lib.c` are gone (they would paint zeroes over the freshly drawn bar), and
`st_backing_screen` is deleted. Repainting costs ~10K source pixels a frame
against the 153600 the blit already writes.

## Autosave

`G_DoWorldDone` autosaves on entering each new level, via `G_SaveGame` rather
than calling `G_DoSaveGame` directly: that sets `sendsave`, which
`G_BuildTiccmd` turns into a `BT_SPECIAL|BTS_SAVEGAME` button and `G_Ticker`
executes at a safe point in the tic loop. Saving inline would write mid-frame.

Guarded by `usergame && !demoplayback && !netgame` — the attract-mode demos
finish levels too, and without the guard the title screen would silently
overwrite the save every time a demo looped.

Uses slot `TD_AUTOSAVE_SLOT` (5, the last menu slot) so it never overwrites a
save the player made. It fires on *entering* level N+1, so the saved state is
the start of the new level with everything carried in. The first level is not
covered: New Game goes straight to `G_DoLoadLevel` without passing through here.

Note this writes to flash while the WAD is memory-mapped from flash. ESP-IDF
handles it by parking the other core, so the flush task stalls briefly — a
hitch hidden by the level load.

## Render resolution and detail level

Doom renders natively at `TD_DOOM_W` x `TD_DOOM_H` (480x288); the blit scales
288 -> 320 vertically (1.11x, not perceptible) and the width is 1:1.

Height is 288 rather than 320 because at 320 the 153,600-byte framebuffer
consumed the large internal DRAM region and left a 15,860-byte largest free
block — too small for the Doom task's stack.

`detailLevel` (Options -> Graphic Detail, shown as H/L beside the on-screen fps)
changes only how many columns the 3D view computes:

- **L** — `viewwidth = 480 >> 1` = 240 columns, each drawn 2px wide
- **H** — 480 columns, one per screen pixel: true native. **This is the
  default** (`detailLevel = 0` in `m_menu.c`): the frame rate is bounded by the
  QSPI flush and the blit more than by the renderer, so the extra columns
  largely consume idle time that was being wasted.

## Touch input is zones, not drag sticks

The pads map position directly to a key: where you touch is what you get.

Layout is two 32px bands with the pads filling everything between:

```
+--------------------+--------------------+   ly <  32
|        ESC         |        MAP         |
+----------+---------+---------+----------+
| MOVE PAD |    dead centre    | TURN PAD |   ly 32..287
| 160x256  |      160 wide     | 160x256  |
+----------+---------+---------+----------+   ly >= 288
|        USE         |        FIRE        |
+--------------------+--------------------+
```

The bands are thin because they are TAP targets; the pads are what a thumb
rests on, so they get the height. Geometry lives once in `src/port/td_res.h`,
shared by `port_input.c` (what a touch means) and `port_video.c` (where the
hints are drawn) so the two cannot disagree.

The movement pad is three horizontal bands — forward across the top 40%, back
across the bottom 40%, and a narrow strafe strip (20%) between. Forward and
back are what you hold, so they get the generous targets; a strafe strip you
must aim for stops it firing by accident while walking. The turn pad splits down
the middle. No dead spots on either; the 160px gap between them is unbound.

**A virtual analog stick was implemented and rejected.** Each pad became a drag
surface feeding `forwardmove`/`sidemove`/`angleturn` proportionally. It worked
and felt worse: on glass with no tactile centre, having to drag before anything
happens is slower and less certain than putting a thumb on the direction you
want. Do not re-propose it as an obvious improvement.

An earlier movement pad split into four triangles from the centre. That put
strafe on the entire left and right flanks and was too easy to trigger while
walking.

### On-screen buttons

`port_video.c` draws labelled buttons (`OvButton`) for controls that are found
by looking rather than by muscle memory: **MENU** and **MAP** in game, **BACK**
and **SELECT** in menus, **YES**/**NO** on prompts. It carries its own 3x5
glyph set — digits plus only the letters those labels need.

Menus also get four arrow plates (`OvArrowButton`) over the pad cells that act
as a d-pad: up/down on the move pad, left/right on the turn pad. Same plate and
border as the labelled buttons, with a triangle where the text would go — bare
triangles floating over the picture read as a rendering artifact rather than a
control.

Plates are **25% black, not opaque** (`OvDimRect` scales each RGB565 channel to
3/4), so a button never fully hides what is behind it. It is a
read-modify-write, so it has to undo the big-endian storage and swap back;
that is fine for the few thousand pixels involved, and would not be for the
whole frame.

The move and turn pads themselves are deliberately NOT outlined: they are held
rather than hunted for, and drawing them would cover the picture during play.

The frame rate and detail level used to be drawn on screen; they now go to
serial only, every 100 frames.

## Touch controller

Two-point capacitive, I2C `0x3B` on SDA 4 / SCL 8, INT 3. **Read 14 bytes, not
8** — the 8-byte read every other sketch on this board uses truncates the second
point away.

```
byte 0      status
byte 1      finger count (never exceeds 2)
bytes 2-5   point 0, nibble-packed:  x = (b0 & 0x0F) << 8 | b1
bytes 6-7   sequence counter
bytes 8-11  point 1, same packing (0xFF filler when only one finger)
bytes 12-13 sequence counter
```

Failed reads return `0xC8`/`0xFF` filler and can carry a believable count byte —
a real capture showed `71 0A C8 C8 ...` decoding as "10 fingers". Check for the
filler pattern, don't just sanity-check `buf[0]`.

**`TD_ReadTouch` must distinguish a failed read (-1) from a genuine zero
fingers (0).** Filler appears in over half of all reads; reporting it as "no
fingers" releases the movement key for a tic and makes walking stutter badly.
`port_input.c` holds the last good state through failures for `TOUCH_HOLD_MS`.

The port name is **not stable** — it encodes USB topology and has appeared as
both `usbmodem11201` and `usbmodem1201`. Use `tools/find_port.sh`; every script
here already does.

## Architecture

- **`tdoom/tdoom.ino`** — display/touch/storage init, two-core frame pipeline.
  Core 1 runs the engine, core 0 pushes frames, so frame time is
  `max(render, flush)` rather than the sum.
- **`src/port/port_video.c`** — palette-index → RGB565 LUT fused with the 90°
  rotation and the fullscreen scale, in one pass over sequential destination
  memory. 320×200 → 480×320.
- **`src/port/port_wad.c`** — implements doomgeneric's `wad_file_t` against
  `esp_partition_mmap`. Sets `wad_file_t.mapped`, which makes `w_wad.c:399`
  return lump pointers **directly into flash** — the WAD costs zero RAM.
- **`src/port/port_input.c`** — touch zones → Doom keycodes. Both points are
  evaluated independently and OR'd, so move-while-firing works.
- **`src/port/port_system.c`** — timing, and the FAT mount for savegames.

### Local patches to vendored doomgeneric

Kept minimal and each marked with a `tdoom:` comment:

- `doomgeneric.h` — `#define CMAP256` (8-bit palettized frames instead of 32bpp
  XRGB) and lock resolution to 320×200.
- `doomgeneric.c`, `i_video.c` — `DG_ScreenBuffer` is gone entirely. Under
  CMAP256 at scale 1, `I_FinishUpdate` only memcpy'd `I_VideoBuffer` into an
  identical buffer; `port_video.c` reads `I_VideoBuffer` directly, saving 64 KB
  of internal SRAM and a 64 KB copy per frame.
- `r_plane.c`, `r_bsp.c`, `r_things.c`, `p_maputl.c`, `d_loop.c`, `statdump.c`,
  `r_main.c` — large static arrays moved to PSRAM via `TD_LAZY_ARRAY`
  (`src/port/td_alloc.h`). Doom declares ~238 KB of static `.bss` and this chip
  cannot hold it alongside `I_VideoBuffer`. **`EXT_RAM_BSS_ATTR` does not work
  here** — Arduino's prebuilt libs ship with
  `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` unset, so it expands to nothing.
- `i_system.c` — zone heap via `DG_AllocZone()` (PSRAM), 4 MiB.
- `d_iwad.c` — `D_FindWADByName()` returns the name unchanged; there is no
  filesystem holding the WAD, so directory search is meaningless.

Removed upstream files: all `doomgeneric_*.c` platform mains, the SDL/Allegro
sound and music backends, and `w_file_stdc.c` (replaced by `port_wad.c`, which
deliberately reuses the `stdc_wad_file` symbol so `w_file.c` needs no edit).

## Not implemented

**No audio.** The board has no codec, amp, or speaker, and the S3 has no DAC.
Bluetooth speakers are impossible here — A2DP needs Classic Bluetooth (BR/EDR)
and the ESP32-S3 is BLE-only. Adding sound means wiring an I2S amp
(e.g. MAX98357A) to free GPIOs.
