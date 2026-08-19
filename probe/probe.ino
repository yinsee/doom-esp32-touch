/*
 * tdoom Phase 0 — hardware probe for the Guition JC3248W535C (ESP32-S3, AXS15231B QSPI)
 *
 * Answers three questions before the Doom port design is locked:
 *   1. Does the AXS15231B touch controller report MORE THAN ONE simultaneous point?
 *      (Doom needs move + turn + fire at once. Single-touch forces a compromised
 *       control scheme, so this gates the whole input design.)
 *   2. Where is the TF card slot wired, and does it mount?
 *   3. What is the real full-frame flush time / fps ceiling?
 *
 * See tdoom/CLAUDE.md for the board gotchas this sketch is careful about.
 */

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#define GFX_BL 1 // Backlight GPIO on JC3248W535C

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */);

Arduino_GFX *panel = new Arduino_AXS15231B(
    bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, false /* IPS */,
    320 /* width */, 480 /* height */);

Arduino_Canvas *gfx = new Arduino_Canvas(320 /* width */, 480 /* height */, panel);

#define TOUCH_ADDR 0x3B
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define TOUCH_INT 3

bool touchOnline = false;

// ---------------------------------------------------------------------------
// 1. Multi-touch probe
// ---------------------------------------------------------------------------
//
// The known-good single-point read is: write {0xB5,0xAB,0xA5,0x5A,0,0,0,0x08},
// read 8 bytes back. buf[1] = touch count, then one 4-byte X/Y pair at buf[2..5].
//
// If the controller supports a second point it should appear in the bytes that
// follow, so we ask for a longer payload (the trailing command byte is the
// requested length) and dump the raw response. A second finger showing up as
// nonzero data past byte 8 is the signal we're looking for.

static const uint8_t PROBE_LENS[] = {8, 14, 16, 20};
static const int NUM_PROBE_LENS = sizeof(PROBE_LENS) / sizeof(PROBE_LENS[0]);

// Read `len` bytes using the AXS15231B command packet. Returns bytes actually read.
int readTouchRaw(uint8_t len, uint8_t *buf) {
  const uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, len};
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, sizeof(cmd));
  if (Wire.endTransmission() != 0) return 0;

  int got = Wire.requestFrom((uint8_t)TOUCH_ADDR, len);
  for (int i = 0; i < got && i < len; i++) buf[i] = Wire.read();
  return got;
}

void dumpHex(const uint8_t *buf, int n) {
  for (int i = 0; i < n; i++) {
    if (buf[i] < 16) Serial.print('0');
    Serial.print(buf[i], HEX);
    Serial.print(' ');
  }
}

// Decode a point at a given byte offset using the known nibble-packed layout.
void decodePoint(const uint8_t *buf, int off, uint16_t &x, uint16_t &y) {
  x = ((buf[off + 0] & 0x0F) << 8) | buf[off + 1];
  y = ((buf[off + 2] & 0x0F) << 8) | buf[off + 3];
}

// Run for `seconds`, logging every distinct response. The operator is told to
// press with one finger, then two.
// Repaint the touch-probe screen. Costs a full ~46ms flush, so callers must
// rate-limit it rather than calling it per sample.
void drawTouchScreen(uint8_t count, uint16_t x, uint16_t y, int secsLeft) {
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(RGB565_RED);
  gfx->setTextSize(3);
  gfx->setCursor(20, 30);
  gfx->print("TOUCH TEST");

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(20, 90);
  gfx->print("1 finger, then 2,");
  gfx->setCursor(20, 115);
  gfx->print("then 3 fingers.");
  gfx->setCursor(20, 140);
  gfx->print("Hold each ~3s.");

  gfx->setTextColor(0x9E79);
  gfx->setTextSize(2);
  gfx->setCursor(20, 190);
  gfx->print("time left: ");
  gfx->print(secsLeft);
  gfx->print("s ");

  // Big fingers-detected readout — the whole point of the test.
  gfx->setTextColor(count > 1 ? RGB565_GREEN : RGB565_YELLOW);
  gfx->setTextSize(4);
  gfx->setCursor(20, 240);
  gfx->print("FINGERS: ");
  gfx->print(count);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(20, 300);
  gfx->print("x="); gfx->print(x);
  gfx->print(" y="); gfx->print(y);
  gfx->print("   ");

  if (count > 0 && x < 320 && y < 480) {
    gfx->fillCircle(x, y, 10, RGB565_MAGENTA);
  }

  gfx->flush();
}

void probeMultiTouch(int seconds) {
  Serial.println("\n=== MULTI-TOUCH PROBE ===");
  Serial.println("Press with ONE finger, then TWO fingers, then THREE.");
  Serial.println("Watching for buf[1] > 1 and for a second X/Y pair past byte 8.");
  Serial.println("Format: len=N | <raw hex> | count=C  p0=(x,y) [p1=(x,y)]");

  uint8_t buf[32];
  uint8_t maxCount = 0;
  int maxUsefulLen = 0;
  unsigned long start = millis();
  unsigned long lastLog = 0;
  unsigned long lastDraw = 0;
  uint8_t lastCount = 0;
  uint16_t lastPx = 0, lastPy = 0;

  drawTouchScreen(0, 0, 0, seconds);

  // Cycle ONE length per poll. The first pass hammered all lengths back-to-back
  // every 20ms and the controller returned 0xC8/0xFF filler for most of them --
  // that was the I2C read failing, not the length being unsupported.
  int li = -1;
  while (millis() - start < (unsigned long)seconds * 1000) {
    {
      li = (li + 1) % NUM_PROBE_LENS;
      uint8_t len = PROBE_LENS[li];
      memset(buf, 0, sizeof(buf));
      int got = readTouchRaw(len, buf);
      if (got < 8) continue;

      uint8_t count = buf[1];
      if (count == 0) continue;              // no finger down
      if (buf[0] == 0xC8 || buf[0] == 0xFF) continue;  // I2C read failed, filler
      if (count > 10) continue;              // not a plausible finger count

      if (count > maxCount) maxCount = count;
      if (count > lastCount) lastCount = count;   // peak within this redraw window
      decodePoint(buf, 2, lastPx, lastPy);

      // Is there anything nonzero past the first point? That's the tell.
      bool dataPastFirstPoint = false;
      for (int i = 6; i < got; i++) {
        if (buf[i] != 0) { dataPastFirstPoint = true; break; }
      }
      if (dataPastFirstPoint && len > maxUsefulLen) maxUsefulLen = len;

      if (millis() - lastLog < 120) continue; // throttle serial
      lastLog = millis();

      Serial.print("len=");
      Serial.print(len);
      Serial.print(" | ");
      dumpHex(buf, got);
      Serial.print("| count=");
      Serial.print(count);

      uint16_t x, y;
      decodePoint(buf, 2, x, y);
      Serial.print("  p0=(");
      Serial.print(x); Serial.print(","); Serial.print(y); Serial.print(")");

      // Candidate offsets for a second point: immediately after the first
      // (byte 6) and one 6-byte record later (byte 8) — dump both, let the
      // plausible one (0 <= x < 320, 0 <= y < 480) reveal itself.
      if (got >= 10) {
        decodePoint(buf, 6, x, y);
        Serial.print("  @6=("); Serial.print(x); Serial.print(","); Serial.print(y); Serial.print(")");
      }
      if (got >= 12) {
        decodePoint(buf, 8, x, y);
        Serial.print("  @8=("); Serial.print(x); Serial.print(","); Serial.print(y); Serial.print(")");
      }
      Serial.println();
    }
    // Redraw at ~3Hz. Any faster and the flush cost starves the touch polling.
    if (millis() - lastDraw >= 300) {
      lastDraw = millis();
      int secsLeft = seconds - (int)((millis() - start) / 1000);
      drawTouchScreen(lastCount, lastPx, lastPy, secsLeft < 0 ? 0 : secsLeft);
      lastCount = 0;  // decay, so lifting fingers shows as 0
    }

    delay(35);  // the controller needs breathing room between reads
  }

  Serial.println("--- multi-touch probe result ---");
  Serial.print("  max reported touch count : "); Serial.println(maxCount);
  Serial.print("  longest len with extra data: "); Serial.println(maxUsefulLen);
  if (maxCount > 1) {
    Serial.println("  => MULTI-TOUCH AVAILABLE: twin-stick controls are viable.");
  } else {
    Serial.println("  => single point only (or no multi-finger press was made).");
  }

  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(maxCount > 1 ? RGB565_GREEN : RGB565_YELLOW);
  gfx->setTextSize(3);
  gfx->setCursor(20, 60);
  gfx->print("RESULT");
  gfx->setTextSize(2);
  gfx->setCursor(20, 120);
  gfx->print("max fingers: ");
  gfx->print(maxCount);
  gfx->setCursor(20, 160);
  gfx->print(maxCount > 1 ? "MULTI-TOUCH OK" : "single touch only");
  gfx->flush();
}

// ---------------------------------------------------------------------------
// 2. TF card slot probe
// ---------------------------------------------------------------------------
//
// The slot's wiring isn't in the board docs we have, so try the plausible
// SPI pin sets. A successful SD.begin() identifies the real pinout.

struct SdCandidate { const char *name; int sck, miso, mosi, cs; };

static const SdCandidate SD_CANDIDATES[] = {
    {"A: SCK12 MISO13 MOSI11 CS10", 12, 13, 11, 10},
    {"B: SCK14 MISO13 MOSI11 CS10", 14, 13, 11, 10},
    {"C: SCK41 MISO42 MOSI2  CS38", 41, 42,  2, 38},
    {"D: SCK17 MISO18 MOSI16 CS15", 17, 18, 16, 15},
};
static const int NUM_SD_CANDIDATES = sizeof(SD_CANDIDATES) / sizeof(SD_CANDIDATES[0]);

// NOTE: GPIO 33-37 are the octal PSRAM pins on this board and GPIO 26-32 are the
// flash pins. Driving any of them reboots the chip (TG1WDT_SYS_RST) — an earlier
// version of this probe used 35/36/37 and boot-looped. Candidates below are
// restricted to genuinely free GPIOs.
void probeSdCard() {
  Serial.println("\n=== TF CARD PROBE ===");
  Serial.println("(Insert a FAT-formatted card first; a miss here is not fatal —");
  Serial.println(" the WAD lives in a flash partition, SD is only a future escape hatch.)");

  for (int i = 0; i < NUM_SD_CANDIDATES; i++) {
    const SdCandidate &c = SD_CANDIDATES[i];
    Serial.print("  trying "); Serial.print(c.name); Serial.print(" ... ");

    SPIClass sdspi(HSPI);
    sdspi.begin(c.sck, c.miso, c.mosi, c.cs);
    if (SD.begin(c.cs, sdspi, 4000000)) {
      uint64_t sz = SD.cardSize() / (1024ULL * 1024ULL);
      Serial.print("MOUNTED! card size = "); Serial.print((uint32_t)sz); Serial.println(" MB");
      Serial.println("  >>> RECORD THESE PINS <<<");
      SD.end();
      sdspi.end();
      return;
    }
    Serial.println("no");
    SD.end();
    sdspi.end();
  }
  Serial.println("  no candidate pinout mounted (no card inserted, or different wiring)");
}

// ---------------------------------------------------------------------------
// 3. Flush benchmark
// ---------------------------------------------------------------------------
//
// Earlier bring-up measured ~10ms draw + ~48ms flush = ~17fps. Confirm on
// this unit, because that number is the hard ceiling the whole Doom port is
// designed around.

void benchmarkFlush(int frames) {
  Serial.println("\n=== FLUSH BENCHMARK ===");
  // The QSPI driver splits every frame into chunks of this many pixels and
  // busy-waits on each one, so the transaction count is the thing to watch.
  Serial.print("  chunk size  : "); Serial.print(ESP32QSPI_MAX_PIXELS_AT_ONCE);
  Serial.println(" px");
  Serial.print("  transactions/frame: ");
  Serial.println((320 * 480) / ESP32QSPI_MAX_PIXELS_AT_ONCE);

  uint16_t *fb = (uint16_t *)gfx->getFramebuffer();
  const size_t px = 320 * 480;

  // Time a full-canvas fill (the "draw" half of a frame) separately from the
  // QSPI push, so we know how much budget Doom's renderer actually has.
  unsigned long t0 = millis();
  for (int i = 0; i < frames; i++) {
    uint16_t color = (i & 1) ? 0x001F : 0xF800;
    for (size_t p = 0; p < px; p++) fb[p] = color;
  }
  unsigned long fillMs = millis() - t0;

  t0 = millis();
  for (int i = 0; i < frames; i++) gfx->flush();
  unsigned long flushMs = millis() - t0;

  float fillPer = (float)fillMs / frames;
  float flushPer = (float)flushMs / frames;

  Serial.print("  canvas fill : "); Serial.print(fillPer, 2); Serial.println(" ms/frame");
  Serial.print("  QSPI flush  : "); Serial.print(flushPer, 2); Serial.println(" ms/frame");
  Serial.print("  serial total: "); Serial.print(fillPer + flushPer, 2);
  Serial.print(" ms -> "); Serial.print(1000.0f / (fillPer + flushPer), 1); Serial.println(" fps");
  Serial.print("  flush-only ceiling (dual-core pipelined): ");
  Serial.print(1000.0f / flushPer, 1); Serial.println(" fps");

  // --- The interesting one -------------------------------------------------
  // Arduino_Canvas::flush() -> draw16bitRGBBitmap -> writePixels(), which
  // byte-swaps all 153600 pixels ON THE CPU into a bounce buffer before every
  // DMA chunk (Arduino_ESP32QSPI.cpp:346). draw16bitBeRGBBitmap() instead goes
  // straight to writeBytes() -- pure DMA, no per-pixel work -- if the buffer is
  // already big-endian.
  //
  // For tdoom that swap is FREE: port_video.c builds each frame from a 256-entry
  // palette LUT, so the LUT can just store byte-swapped values.
  uint16_t *be = (uint16_t *)heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
  if (!be) {
    Serial.println("  [skip] BE-buffer benchmark: PSRAM alloc failed");
    return;
  }
  for (size_t p = 0; p < px; p++) be[p] = 0x1F00;  // pre-swapped blue

  t0 = millis();
  for (int i = 0; i < frames; i++) panel->draw16bitBeRGBBitmap(0, 0, be, 320, 480);
  unsigned long bePer100 = millis() - t0;
  float bePer = (float)bePer100 / frames;

  Serial.print("  BE direct-DMA flush: "); Serial.print(bePer, 2);
  Serial.print(" ms/frame -> "); Serial.print(1000.0f / bePer, 1); Serial.println(" fps");
  Serial.print("  speedup vs canvas flush: "); Serial.print(flushPer / bePer, 2);
  Serial.println("x");

  heap_caps_free(be);
}

// ---------------------------------------------------------------------------

void i2cScan() {
  Serial.println("\n=== I2C SCAN (SDA=4 / SCL=8) ===");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("  device at 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      if (addr == TOUCH_ADDR) {
        Serial.println("    (0x3B = AXS15231B -> board confirmed as JC3248W535C)");
        touchOnline = true;
      }
    }
  }
  if (!touchOnline) Serial.println("  !! touch controller NOT found at 0x3B");
}

void showBanner() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_RED);
  gfx->setTextSize(3);
  gfx->setCursor(40, 60);
  gfx->print("tdoom");
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(20, 110);
  gfx->print("Phase 0 probe");
  gfx->setTextColor(0x9E79);
  gfx->setTextSize(2);
  gfx->setCursor(20, 160);
  gfx->print("1 finger, then 2,");
  gfx->setCursor(20, 185);
  gfx->print("then 3. Watch serial.");
  gfx->flush();
}

void setup() {
  Serial.begin(115200);
  // Gotcha 6: without this, USB CDC writes block ~1s each once a monitor
  // detaches, and everything collapses to 1fps.
  Serial.setTxTimeoutMs(0);
  delay(1500);

  Serial.println("\n===========================================");
  Serial.println("  tdoom Phase 0 — JC3248W535C hardware probe");
  Serial.println("===========================================");
  Serial.print("PSRAM size    : "); Serial.println(ESP.getPsramSize());
  Serial.print("PSRAM free    : "); Serial.println(ESP.getFreePsram());
  Serial.print("Internal heap : "); Serial.println(ESP.getFreeHeap());
  Serial.print("Flash size    : "); Serial.println(ESP.getFlashChipSize());
  Serial.print("CPU freq MHz  : "); Serial.println(getCpuFrequencyMhz());

  // Gotcha 5: begin() with NO clock argument. 80MHz corrupts the panel,
  // 60MHz drops to ~1fps.
  if (!gfx->begin()) {
    Serial.println("[ERROR] gfx->begin() failed!");
  } else {
    Serial.println("[OK] display initialized (canvas mode)");
  }

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  gfx->setRotation(0);

  showBanner();

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setTimeOut(25);  // 5ms truncated reads and produced 0xC8 filler
  pinMode(TOUCH_INT, INPUT);

  i2cScan();
  benchmarkFlush(30);

  // Touch probe runs before the SD probe: it is the one result that gates the
  // control design, so nothing riskier is allowed to run ahead of it.
  if (touchOnline) {
    probeMultiTouch(25);
  } else {
    Serial.println("\n[SKIP] multi-touch probe — controller not responding");
  }

  probeSdCard();

  Serial.println("\n=== PROBE COMPLETE ===");
  Serial.println("Touch the screen to keep logging raw packets (len=20).");
}

void loop() {
  static unsigned long last = 0;
  if (!touchOnline || millis() - last < 150) return;
  last = millis();

  uint8_t buf[32];
  memset(buf, 0, sizeof(buf));
  int got = readTouchRaw(20, buf);
  if (got >= 8 && buf[1] != 0) {
    Serial.print("[touch] ");
    dumpHex(buf, got);
    Serial.print("count="); Serial.println(buf[1]);
  }
}
