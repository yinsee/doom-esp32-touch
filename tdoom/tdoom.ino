/*
 * tdoom — Doom on the Guition JC3248W535C (ESP32-S3 + AXS15231B QSPI panel).
 *
 * This file owns the hardware: display, touch, storage, and the two-core frame
 * pipeline. The engine itself is doomgeneric under src/doomgeneric/, and the
 * platform shim it needs lives in src/port/.
 *
 * ---------------------------------------------------------------------------
 * Why there is no Arduino_Canvas here
 *
 * Arduino_Canvas::flush() ends in Arduino_ESP32QSPI::writePixels(), which
 * byte-swaps all 153600 pixels on the CPU into a bounce buffer before every DMA
 * chunk. Measured on this board: 45.7 ms per frame, of which only ~15 ms is
 * actual QSPI bus time.
 *
 * So we keep our own framebuffers, store pixels ALREADY byte-swapped (the
 * palette LUT in port_video.c does it for free, 256 entries instead of 153600
 * pixels), and push them with draw16bitBeRGBBitmap() -> writeBytes(), which is
 * a straight DMA out of PSRAM with no per-pixel work.
 *
 * ---------------------------------------------------------------------------
 * The two-core pipeline
 *
 *   core 1 (engine task) : doomgeneric_Tick() -> renders into the back buffer
 *   core 0 (flush task)  : pushes the front buffer over QSPI
 *
 * They swap buffers under a pair of semaphores, so frame time is
 * max(render, flush) rather than render + flush.
 *
 * See CLAUDE.md for the board gotchas (they are not optional).
 */

#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "src/port/td_res.h"

static const char *TAG = "tdoom";

// --- Display ---------------------------------------------------------------

#define GFX_BL  1     // backlight
#define PANEL_W 320   // physical (portrait)
#define PANEL_H 480

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */);

Arduino_GFX *panel = new Arduino_AXS15231B(
    bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, false /* IPS */,
    PANEL_W, PANEL_H);

// --- Touch (AXS15231B integrated capacitive controller) --------------------

#define TOUCH_ADDR 0x3B
#define TOUCH_SDA  4
#define TOUCH_SCL  8
#define TOUCH_INT  3

static bool touch_online = false;

// Task handles, kept so ReportHealth() can read their stack headroom.
static TaskHandle_t h_doom, h_flush;

// Doom's render target, reserved up front -- see setup(). Size comes from
// td_res.h, the same header i_video.h and doomgeneric.h use, so it cannot drift
// out of step with the engine again. It did once: the engine was configured for
// 320x200 while this reserved 480x320, and the oversized reservation
// fragmented internal RAM until the Doom task could not be created.
#define DOOM_W TD_DOOM_W
#define DOOM_H TD_DOOM_H
static void  *td_videobuf = nullptr;
static size_t td_videobuf_size = 0;

// Handed to i_video.c via DG_AllocScreenBuffer (port_video.c).
extern "C" void *TD_TakeVideoBuffer(size_t bytes)
{
  if (td_videobuf == nullptr || bytes > td_videobuf_size)
  {
    printf("[tdoom] video buffer request %u exceeds the %u reserved\n",
           (unsigned)bytes, (unsigned)td_videobuf_size);
    return nullptr;
  }
  return td_videobuf;
}

// --- Frame pipeline --------------------------------------------------------

// THREE buffers, not two.
//
// With two, the blit for frame N+1 cannot start until the flush of frame N has
// finished with the other buffer, so frame time is flush + blit (28.6 + 9 =
// 37.6ms) rather than max(). Measured: render 12ms, blit 9ms, and 18ms spent
// simply WAITING -- core 1 idle nearly half the time.
//
// A third buffer lets the engine blit into a free one while a completed frame
// is still going out, making frame time max(render + blit, flush) = 28.6ms and
// putting us on the flush ceiling where we belong. Costs 300KB of PSRAM, which
// we have in abundance.
#define NUM_FB 3

static uint16_t *fb[NUM_FB];
static volatile int write_idx = 0;         // engine converts into this one
static volatile int read_idx = 0;          // flush task sends this one
static SemaphoreHandle_t sem_frame_ready;  // engine -> flush task
static SemaphoreHandle_t sem_buffer_free;  // flush task -> engine

// NOTE: these are marked individually rather than wrapped in an
// `extern "C" { ... }` block. arduino-cli injects auto-generated prototypes
// (including setup() and loop()) partway down the sketch, and a block here
// swallows them -- giving setup() C linkage and a conflict with Arduino.h.

// Called by port_video.c at the START of building a frame.
//
// This is where the engine blocks, and blocking here is the whole point: by the
// time we arrive, doomgeneric has already run the game tic and rendered into
// I_VideoBuffer (internal SRAM, untouched by the flush task) -- so that work has
// already overlapped with core 0 pushing the previous frame. We only have to
// wait for the framebuffer itself to come free.
extern "C" uint16_t *TD_GetBackBuffer(void)
{
  xSemaphoreTake(sem_buffer_free, portMAX_DELAY);
  return fb[write_idx];
}

// Called by port_video.c once a frame is complete. Publishes it and returns
// immediately -- the engine goes straight on to the next tic while core 0
// pushes this one.
extern "C" void TD_SubmitFrame(void)
{
  // Producer and consumer walk the same ring in the same order, so the flush
  // task's own read_idx always names the frame just submitted -- no shared
  // "which index" variable to race on.
  write_idx = (write_idx + 1) % NUM_FB;
  xSemaphoreGive(sem_frame_ready);
}

// Read one touch sample.
//
// Returns 2, 1, 0 for that many fingers, or -1 if the READ ITSELF FAILED.
//
// That distinction matters more than it looks. This controller returns 0xC8/0xFF
// filler on a large fraction of reads, and an earlier version reported those as
// "0 fingers" -- so every corrupt read looked like the player lifting their
// thumb, and movement stuttered constantly. port_input.c holds the previous
// state through -1 and only releases on a real 0.
//
// Protocol, established by probe/probe.ino against real fingers:
//   byte 0     : status
//   byte 1     : finger count (this controller never reports more than 2)
//   bytes 2-5  : point 0, nibble-packed  x = (b0&0x0F)<<8 | b1
//   bytes 6-7  : sequence counter
//   bytes 8-11 : point 1, same packing (0xFF filler when only one finger)
//   bytes 12-13: sequence counter
//
// A 14-byte read is what gets the SECOND point; the 8-byte read every other
// sketch on this board uses truncates it away. That second point is what makes
// twin-stick controls possible instead of a one-finger compromise.
//
// Failed reads come back as 0xC8/0xFF filler and can carry a plausible-looking
// count byte (a real capture showed "71 0A C8 C8..." decoding as 10 fingers),
// so the filler pattern is checked explicitly rather than trusting buf[0].
extern "C" int TD_ReadTouch(uint8_t *count, uint16_t *x0, uint16_t *y0,
                 uint16_t *x1, uint16_t *y1)
{
  if (!touch_online) return -1;

  const uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x0E};
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, sizeof(cmd));
  if (Wire.endTransmission() != 0) return -1;

  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)14) != 14) return -1;
  uint8_t buf[14];
  for (int i = 0; i < 14; i++) buf[i] = Wire.read();

  // Reject filler: two consecutive 0xC8 anywhere in the point data means the
  // transfer broke down and nothing in the packet can be trusted.
  for (int i = 0; i < 13; i++)
  {
    if (buf[i] == 0xC8 && buf[i + 1] == 0xC8) return -1;
  }

  uint8_t n = buf[1];
  if (n > 2) return -1;            // hardware maxes out at 2 points; more = junk

  *count = 0;
  if (n == 0) return 0;            // a genuine, trustworthy "no fingers"

  uint16_t px = ((buf[2] & 0x0F) << 8) | buf[3];
  uint16_t py = ((buf[4] & 0x0F) << 8) | buf[5];
  if (px >= PANEL_W || py >= PANEL_H) return -1;

  *count = 1;
  *x0 = px;
  *y0 = py;

  if (n >= 2)
  {
    uint16_t qx = ((buf[8] & 0x0F) << 8) | buf[9];
    uint16_t qy = ((buf[10] & 0x0F) << 8) | buf[11];
    // 0xFF filler decodes to 4095; only accept an on-screen second point.
    if (qx < PANEL_W && qy < PANEL_H)
    {
      *count = 2;
      *x1 = qx;
      *y1 = qy;
    }
  }

  return *count;
}

// Engine state, for the health log.
extern "C" int gamestate;
extern "C" int menuactive;
extern "C" int messageToPrint;
extern "C" int gametic;
extern "C" int pagetic;
extern "C" int demosequence;

extern "C" void doomgeneric_Create(int argc, char **argv);
extern "C" void doomgeneric_Tick(void);
extern "C" bool TD_MountStorage(void);

// --- Tasks -----------------------------------------------------------------

// Core 0: nothing but pushing completed frames at the panel.
static void FlushTask(void *arg)
{
  for (;;)
  {
    xSemaphoreTake(sem_frame_ready, portMAX_DELAY);

    // Pixels are already big-endian, so this is a pure DMA of the buffer.
    panel->draw16bitBeRGBBitmap(0, 0, fb[read_idx], PANEL_W, PANEL_H);
    read_idx = (read_idx + 1) % NUM_FB;

    xSemaphoreGive(sem_buffer_free);

    // Let IDLE0 run.
    //
    // With three buffers the engine always has a frame queued, so this loop
    // never blocks on sem_frame_ready and core 0 is saturated -- which starves
    // the idle task and trips the task watchdog ("task_wdt: - IDLE0 (CPU 0)",
    // then a reboot). One tick (1ms against a 28.6ms flush, ~3%) is enough, and
    // is safer than unsubscribing IDLE0 from the watchdog, which would also
    // stop FreeRTOS reclaiming deleted tasks.
    vTaskDelay(1);
  }
}

// Core 1: the game.
static void DoomTask(void *arg)
{
  // "-iwad doom1.wad" never hits a filesystem -- d_iwad.c is patched to hand
  // the name straight through, and port_wad.c maps the flash partition. The
  // name still selects the shareware mission.
  static char *argv[] = {(char *)"tdoom", (char *)"-iwad", (char *)"doom1.wad"};

  ESP_LOGI(TAG, "starting doomgeneric");
  doomgeneric_Create(3, argv);

  for (;;)
  {
    doomgeneric_Tick();
  }
}

// ---------------------------------------------------------------------------

static void Fatal(const char *msg)
{
  ESP_LOGE(TAG, "FATAL: %s", msg);

  // Put it on the glass too -- a board with no serial attached is otherwise
  // just a black screen.
  panel->fillScreen(RGB565_BLACK);
  panel->setTextColor(RGB565_RED);
  panel->setTextSize(2);
  panel->setCursor(10, 40);
  panel->print("tdoom failed:");
  panel->setTextColor(RGB565_WHITE);
  panel->setCursor(10, 80);
  panel->print(msg);

  for (;;) delay(1000);
}

void setup()
{
  Serial.begin(115200);
  // Without this, USB CDC writes block ~1s each once a monitor detaches and the
  // whole game drops to 1fps. Non-negotiable on this board.
  Serial.setTxTimeoutMs(0);
  delay(300);

  // Reserve the Doom framebuffer FIRST, before anything else fragments the heap.
  //
  // At 480x320 it needs 153600 CONTIGUOUS bytes. Allocated late (from inside
  // I_InitGraphics, where upstream does it) that fails even with 188KB free,
  // because by then the QSPI DMA buffers and task stacks have split the largest
  // block down to ~147KB. Grabbing it here, before panel->begin() and before
  // any task exists, is the difference between fitting and not.
  td_videobuf_size = (size_t)DOOM_W * DOOM_H;
  td_videobuf = heap_caps_malloc(td_videobuf_size,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (td_videobuf != nullptr)
  {
    // Palette index 0 is black. Without this, everything Doom never draws over
    // -- the area outside a 320x200 menu or title patch -- shows uninitialised
    // heap as coloured noise.
    memset(td_videobuf, 0, td_videobuf_size);
  }
  printf("[tdoom] QSPI chunk %d px (%d byte transfers) | DMA largest %u\n",
         ESP32QSPI_MAX_PIXELS_AT_ONCE, ESP32QSPI_MAX_PIXELS_AT_ONCE * 2,
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
  printf("[tdoom] reserved %u byte video buffer -> %p (largest block was %u)\n",
         (unsigned)td_videobuf_size, td_videobuf,
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  ESP_LOGI(TAG, "tdoom starting: PSRAM free %u, INTERNAL free %u (largest %u)",
           (unsigned)ESP.getFreePsram(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  // begin() with NO clock argument: 80MHz corrupts this panel, 60MHz drops it
  // to ~1fps. The stock rate is the only one that works.
  if (!panel->begin())
  {
    ESP_LOGE(TAG, "panel begin() failed");
  }
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  panel->fillScreen(RGB565_BLACK);

  // Full-panel buffers in PSRAM: 320*480*2 = 307200 bytes each.
  for (int i = 0; i < NUM_FB; i++)
  {
    // MALLOC_CAP_DMA matters as much as SPIRAM here. Without it the SPI master
    // treats the buffer as non-DMA-capable and allocates an internal bounce
    // buffer for EVERY transfer -- which fails once internal RAM is tight:
    //   spi_master: setup_dma_priv_buffer: Failed to allocate priv TX buffer
    //   assert failed: spi_device_polling_end
    // On the ESP32-S3 the GDMA can read external RAM directly, so asking for a
    // DMA-capable PSRAM block removes the bounce entirely.
    fb[i] = (uint16_t *)heap_caps_malloc(PANEL_W * PANEL_H * 2,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (fb[i] == nullptr) Fatal("PSRAM framebuffer alloc failed");
    memset(fb[i], 0, PANEL_W * PANEL_H * 2);
  }

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setTimeOut(25);   // 5ms truncated reads and returned 0xC8 filler
  pinMode(TOUCH_INT, INPUT);
  Wire.beginTransmission(TOUCH_ADDR);
  touch_online = (Wire.endTransmission() == 0);
  ESP_LOGI(TAG, "touch controller %s", touch_online ? "online" : "NOT FOUND");

  TD_MountStorage();   // savegames; not fatal if it fails

  // Counting, not binary: more than one frame can be in flight now.
  sem_frame_ready = xSemaphoreCreateCounting(NUM_FB, 0);
  // Start with NUM_FB-1 free: the engine may run ahead by that many while one
  // buffer is being flushed. Deliberately not NUM_FB -- letting it get a whole
  // ring ahead would only add input latency, not throughput.
  sem_buffer_free = xSemaphoreCreateCounting(NUM_FB, NUM_FB - 1);
  if (!sem_frame_ready || !sem_buffer_free) Fatal("semaphore alloc failed");

  if (xTaskCreatePinnedToCore(FlushTask, "flush", 4096, nullptr, 3, &h_flush, 0) != pdPASS)
  {
    Fatal("flush task creation failed");
  }

  // 16KB. Measured headroom at 24576 was 21936 bytes -- the task only ever
  // used ~2.6KB -- and internal RAM is needed elsewhere (the SPI driver has to
  // be able to allocate). ReportHealth() prints the live headroom every second;
  // watch it during actual gameplay, not just menus, since BSP recursion is
  // deepest in open maps.
  printf("[tdoom] internal free before tasks: %u (largest %u)\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (xTaskCreatePinnedToCore(DoomTask, "doom", 16384, nullptr, 2, &h_doom, 1) != pdPASS)
  {
    // Loud on purpose. This failed silently for several builds: the firmware
    // ran, the panel initialised, the health log ticked over -- and the game
    // simply never started, showing a blank screen with no error anywhere.
    Fatal("doom task creation failed - not enough contiguous internal RAM");
  }
}

// Report memory and stack headroom once a second. A stack that creeps towards
// zero, or a heap with no room left, corrupts whatever is adjacent -- which
// shows up as visual glitching long before it shows up as a crash.
static void ReportHealth(void)
{
  // pagetic drives the attract mode: D_PageTicker decrements it and calls
  // D_AdvanceDemo at zero. If it is not counting down, the title screen is
  // frozen and no demo will ever start.
  printf("[tdoom] state: gamestate=%d menuactive=%d msg=%d gametic=%d pagetic=%d demoseq=%d\n",
         (int)gamestate, (int)menuactive, (int)messageToPrint, (int)gametic,
         (int)pagetic, (int)demosequence);
  printf("[tdoom] heap: internal free %u largest %u | DMA free %u largest %u | psram free %u | "
         "stack headroom doom %u flush %u\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
         (unsigned)ESP.getFreePsram(),
         h_doom  ? (unsigned)uxTaskGetStackHighWaterMark(h_doom)  : 0u,
         h_flush ? (unsigned)uxTaskGetStackHighWaterMark(h_flush) : 0u);
}

void loop()
{
  // Every 5s, not every 1s. printf blocks when the USB CDC buffer fills (it
  // does NOT honour Serial.setTxTimeoutMs), and chatty logging was hanging the
  // whole firmware.
  static int tick;
  if ((tick++ % 5) == 0)
  {
    ReportHealth();
  }

  // Everything runs in the two pinned tasks; keep the Arduino loop out of the
  // way rather than letting it compete for core 1.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
