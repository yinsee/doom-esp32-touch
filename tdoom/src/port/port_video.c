/*
 * tdoom — frame delivery to the AXS15231B panel.
 *
 * Doom renders 320x200 8-bit palette indices. The panel is a 320x480 portrait
 * QSPI display we drive as 480x320 landscape. This file turns one into the
 * other in a single pass:
 *
 *   palette index --LUT--> RGB565 --rotate 90 CW--> portrait framebuffer
 *
 * Doing it in one pass matters. Upstream doomgeneric would build a 320x200
 * 32bpp XRGB frame (256 KB) and then something else would have to convert it
 * again; CMAP256 (see doomgeneric.h) skips that entirely.
 *
 * Geometry: the 320x200 image is scaled to fill the whole 480x320 landscape
 * field. Touch controls overlay the edges rather than sitting in margins.
 *
 * This file does NOT touch the panel. It fills a back buffer and signals the
 * flush task in tdoom.ino, so the QSPI transfer overlaps the next frame's
 * rendering instead of stalling it.
 *
 * Pixels are stored byte-swapped so the flush can be a raw DMA of the buffer --
 * see RebuildPalette() for why that matters more than anything else here.
 */

#include <string.h>

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "../doomgeneric/doomgeneric.h"
#include "../doomgeneric/doomkeys.h"
#include "../doomgeneric/i_video.h"
#include "td_alloc.h"
#include "td_res.h"

static const char *TAG = "tdoom-video";

/* Panel is physically portrait; we present it as landscape. */
#define PANEL_W 320
#define PANEL_H 480
#define SCR_W   480   /* landscape width  */
#define SCR_H   320   /* landscape height */

/* Full screen: Doom's 320x200 is scaled to the whole 480x320 landscape field.
 * That is 1.5x across and 1.6x down -- both exact ratios (2->3 and 5->8), but
 * rather than special-case them we precompute a source index per destination
 * pixel. One indexed load per pixel, any scale factor, no per-pixel arithmetic.
 *
 * Doom's original 320x200 was always displayed stretched to 4:3, so scaling the
 * two axes by slightly different amounts is faithful to how it actually looked,
 * not a distortion.
 *
 * The touch controls have no margin to live in now, so they overlay the edges
 * of the picture (see port_input.c) -- the same arrangement every phone port of
 * Doom uses. */
/* Shared with port_input.c via td_res.h, so the hints cannot drift away from
 * the zones they mark. */
#define ROW1  TD_ROW1
#define ROW3  TD_ROW3

static uint16_t scale_col[SCR_W];    /* landscape x (== portrait row) -> Doom x */
static uint32_t row_off[PANEL_W];    /* portrait column -> byte offset of the
                                      * Doom row it samples, with the 90-degree
                                      * rotation already folded in */

/* Provided by tdoom.ino: the canvas the flush task is not currently sending. */
extern uint16_t *TD_GetBackBuffer(void);
extern void TD_SubmitFrame(void);

extern int menuactive;      /* Doom's menu state, for the hint overlay   */
extern int messageToPrint;  /* a yes/no prompt is up                     */
extern int detailLevel;     /* 0 = high detail, 1 = low                  */

/* Doom renders into this 320x200 palettized buffer; we read it directly
 * rather than through DG_ScreenBuffer (see I_FinishUpdate). */
extern uint8_t *I_VideoBuffer;

/* Exported by i_video.c under CMAP256. */
extern struct color colors[256];
extern boolean palette_changed;

/* RGB565 values stored byte-swapped for direct DMA -- see RebuildPalette(). */
static uint16_t palette565[256];

/* ---------------------------------------------------------------------------
 * Allocators used by the patched engine sources.
 *
 * These exist because plain malloc() on this board serves internal SRAM, which
 * is far too small for Doom's zone heap but is exactly where the per-frame
 * buffers need to be.
 * ------------------------------------------------------------------------- */

void *DG_AllocZone(size_t bytes)
{
    /* Zone heap: multi-megabyte, accessed unpredictably -> PSRAM. */
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "zone heap: %u bytes in PSRAM -> %p", (unsigned)bytes, p);
    return p;
}

extern void *TD_TakeVideoBuffer(size_t bytes);

void *DG_AllocScreenBuffer(size_t bytes)
{
    /* Prefer the block reserved during setup(): by the time the engine asks,
     * the largest contiguous free region is smaller than the buffer we need. */
    void *pre = TD_TakeVideoBuffer(bytes);
    if (pre != NULL)
    {
        printf("[tdoom] video buffer: %u bytes (pre-reserved) -> %p\n",
               (unsigned)bytes, pre);
        return pre;
    }

    /* Written pixel-by-pixel every frame -> internal SRAM, no exceptions.
     * Falling back to PSRAM would silently halve the frame rate, so make the
     * failure loud instead. */
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == NULL)
    {
        ESP_LOGE(TAG, "OUT OF INTERNAL SRAM for a %u byte screen buffer "
                      "(free %u, largest block %u)",
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        abort();   /* the caller would only deref NULL a moment later */
    }
    else
    {
        printf("[tdoom] screen buffer: %u bytes internal, %u free after\n",
               (unsigned)bytes,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return p;
}

void *TD_PsramAlloc(size_t bytes, const char *what)
{
    /* Zero-filled, because these allocations stand in for .bss. */
    void *p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    if (p == NULL)
    {
        ESP_LOGE(TAG, "PSRAM alloc failed for '%s' (%u bytes)",
                 what, (unsigned)bytes);
        abort();
    }
    ESP_LOGI(TAG, "PSRAM: %-16s %6u bytes -> %p", what, (unsigned)bytes, p);
    return p;
}

void DG_FreeScreenBuffer(void *p)
{
    heap_caps_free(p);
}

/* ------------------------------------------------------------------------- */

static void RebuildPalette(void)
{
    int i;

    for (i = 0; i < 256; i++)
    {
        /* RGB888 -> RGB565, stored BYTE-SWAPPED (big-endian).
         *
         * The panel wants big-endian pixels. Arduino_Canvas::flush() gets there
         * via writePixels(), which swaps all 153600 pixels on the CPU into a
         * bounce buffer before each DMA chunk (Arduino_ESP32QSPI.cpp:346) --
         * that CPU work, not the bus, is what makes a flush cost ~46ms.
         *
         * Swapping here instead makes it free: it happens 256 times per palette
         * change rather than 153600 times per frame, and the framebuffer can
         * then go out through draw16bitBeRGBBitmap() -> writeBytes(), which is
         * a straight DMA from PSRAM with no per-pixel work at all.
         */
        uint16_t c = (uint16_t)(((colors[i].r & 0xF8) << 8) |
                                ((colors[i].g & 0xFC) << 3) |
                                ((colors[i].b & 0xF8) >> 3));
        palette565[i] = (uint16_t)((c >> 8) | (c << 8));
    }
    palette_changed = false;
}


/* ---------------------------------------------------------------------------
 * On-screen overlays: fps readout and menu hints.
 *
 * Drawn straight into the RGB565 framebuffer after the blit, in LANDSCAPE
 * coordinates. Costs a few hundred pixels per frame, nothing against the 153600
 * the blit already wrote.
 *
 * Everything here writes byte-swapped colours, because the whole framebuffer is
 * stored big-endian for the DMA path (see RebuildPalette).
 * ------------------------------------------------------------------------- */

#define BE(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))

#define C_WHITE  BE(0xFFFF)
#define C_BLACK  BE(0x0000)
#define C_GREEN  BE(0x07E0)
#define C_AMBER  BE(0xFD20)

static uint16_t *ov_fb;      /* target for the helpers below */
static unsigned fps_x10 = 0; /* frame rate * 10, for the on-screen readout */

static inline void OvPixel(int lx, int ly, uint16_t color)
{
    if (lx < 0 || lx >= SCR_W || ly < 0 || ly >= SCR_H)
    {
        return;
    }
    /* Same rotation as the blit: landscape (lx,ly) -> portrait index. */
    ov_fb[(size_t)lx * PANEL_W + (PANEL_W - 1 - ly)] = color;
}

static void OvRect(int lx, int ly, int w, int h, uint16_t color)
{
    int i, j;
    for (j = 0; j < h; j++)
    {
        for (i = 0; i < w; i++)
        {
            OvPixel(lx + i, ly + j, color);
        }
    }
}

/* 3x5 glyphs, one bit per pixel, leftmost column in bit 2.
 * 0-9, then '.', 'Y', 'N', 'H', 'L'. */
#define OV_DOT 10
#define OV_Y   11
#define OV_N   12
#define OV_H   13
#define OV_L   14

static const uint8_t ov_digits[15][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7},
    {5,5,7,1,1}, {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1},
    {7,5,7,5,7}, {7,5,7,1,7},
    {0,0,0,0,2},              /* .  */
    {5,5,2,2,2},              /* Y  */
    {5,7,7,7,5},              /* N  */
    {5,5,7,5,5},              /* H  */
    {4,4,4,4,7},              /* L  */
};

static void OvDigit(int lx, int ly, int d, int scale, uint16_t color)
{
    int row, col;
    for (row = 0; row < 5; row++)
    {
        for (col = 0; col < 3; col++)
        {
            if (ov_digits[d][row] & (4 >> col))
            {
                OvRect(lx + col * scale, ly + row * scale, scale, scale, color);
            }
        }
    }
}

/* fps as "NN.N" in the top-right corner, on a dark plate so it stays readable
 * against sky and muzzle flash alike. fps10 is fps * 10. */
static void OvDrawFps(unsigned fps10, int scale)
{
    int glyphs[5];
    int n, i, x, y, w, start;

    /* "NNN.N" needs FIVE slots: hundreds, tens, units, point, tenths.
     * The first version used four and put the point between the tens and the
     * units, so 28.6 fps rendered as "2.8" -- the tenths digit was never even
     * emitted. */
    glyphs[0] = (fps10 / 1000) % 10;   /* hundreds */
    glyphs[1] = (fps10 / 100) % 10;    /* tens     */
    glyphs[2] = (fps10 / 10) % 10;     /* units    */
    glyphs[3] = OV_DOT;                /* .        */
    glyphs[4] = fps10 % 10;            /* tenths   */

    /* Drop leading zeros, but never the digit immediately before the point. */
    start = 0;
    while (start < 2 && glyphs[start] == 0)
    {
        start++;
    }

    n = 5 - start;
    w = n * 4 * scale;

    x = SCR_W - w - 6;
    y = 5;

    OvRect(x - 3, y - 3, w + 6, 5 * scale + 6, C_BLACK);

    for (i = 0; i < n; i++)
    {
        OvDigit(x + i * 4 * scale, y, glyphs[start + i], scale,
                fps10 >= 250 ? C_GREEN : C_AMBER);
    }
}

/* Solid triangle. dir: 0 up, 1 down, 2 left, 3 right.
 *
 * `i` walks from the APEX to the base while the half-width grows, so the apex
 * is whichever end the row/column index starts at. Getting that backwards is
 * how the first version drew both vertical arrows upside down. */
static void OvTriangle(int cx, int cy, int size, int dir, uint16_t color)
{
    int i, half;
    for (i = 0; i < size; i++)
    {
        half = i / 2;
        if (dir == 0)
        {
            /* Up: apex at the TOP, widening downwards. */
            OvRect(cx - half, cy - size / 2 + i, 2 * half + 1, 1, color);
        }
        else if (dir == 1)
        {
            /* Down: apex at the BOTTOM, widening upwards. */
            OvRect(cx - half, cy + size / 2 - i, 2 * half + 1, 1, color);
        }
        else if (dir == 2)
        {
            /* Left: apex at the LEFT, widening rightwards. */
            OvRect(cx - size / 2 + i, cy - half, 1, 2 * half + 1, color);
        }
        else
        {
            /* Right: apex at the RIGHT, widening leftwards. */
            OvRect(cx + size / 2 - i, cy - half, 1, 2 * half + 1, color);
        }
    }
}

/* Detail level beside the fps readout: H = high (full resolution), L = low
 * (R_DrawColumnLow, half horizontal resolution in the 3D view). Without this
 * there is no way to tell which mode a given frame rate belongs to. */
static void OvDrawDetail(int low, int scale)
{
    int x = SCR_W - 6 - (5 * 4 * scale) - (4 * scale) - 4;
    int y = 5;

    OvRect(x - 3, y - 3, 3 * scale + 6, 5 * scale + 6, C_BLACK);
    OvDigit(x, y, low ? OV_L : OV_H, scale, C_WHITE);
}

/* Yes/no prompt hints. These prompts accept only 'y'/'n', and the touch zones
 * are invisible, so without this the dialog is unanswerable-looking. */
static void OvDrawConfirmHints(void)
{
    int s = 4;

    /* NO in the top-left corner -- the same corner that means "back". */
    OvRect(8, 6, 3 * s + 8, 5 * s + 8, C_BLACK);
    OvDigit(12, 10, OV_N, s, C_AMBER);

    /* YES over the rest, anchored bottom-right where "select" lives. */
    OvRect(SCR_W - 4 - (3 * s + 8), SCR_H - 8 - (5 * s + 8),
           3 * s + 8, 5 * s + 8, C_BLACK);
    OvDigit(SCR_W - (3 * s) - 8, SCR_H - (5 * s) - 12, OV_Y, s, C_GREEN);
}

/* Menu hints. The touch zones are invisible, so without these the layout has to
 * be memorised -- which is exactly what made menus confusing. */
static void OvDrawMenuHints(void)
{
    int turncy = (ROW1 + ROW3) / 2;              /* pad band centre y */

    /* Move pad: up / down, drawn inside the triangles that walk forward/back. */
    OvTriangle(TD_MZ_CX, (ROW1 + TD_MZ_CY) / 2, 24, 0, C_WHITE);
    OvTriangle(TD_MZ_CX, (TD_MZ_CY + ROW3) / 2, 24, 1, C_WHITE);

    /* Turn pad: slider left / right. Several Options entries (Screen Size,
     * volumes, sensitivity) respond only to these. */
    OvTriangle((TD_TZ_X0 + TD_TZ_XM) / 2, turncy, 20, 2, C_AMBER);
    OvTriangle((TD_TZ_XM + SCR_W) / 2, turncy, 20, 3, C_AMBER);

    /* Top-left backs out, same corner as ESC in game. */
    OvTriangle(30, ROW1 / 2, 18, 2, C_AMBER);

    /* Bar across the bottom row: tap anywhere along it to select. */
    OvRect(8, SCR_H - 9, SCR_W - 16, 4, C_GREEN);
}

void DG_Init(void)
{
    int i;

    printf("[tdoom] video: %dx%d -> %dx%d fullscreen\n",
           DOOMGENERIC_RESX, DOOMGENERIC_RESY, SCR_W, SCR_H);

    for (i = 0; i < SCR_W; i++)
    {
        scale_col[i] = (uint16_t)((i * DOOMGENERIC_RESX) / SCR_W);
    }
    for (i = 0; i < PANEL_W; i++)
    {
        /* portrait column i is landscape y = PANEL_W-1-i (the rotation), and
         * that maps to a Doom row through the vertical scale. */
        int ly = PANEL_W - 1 - i;
        row_off[i] = (uint32_t)(((ly * DOOMGENERIC_RESY) / SCR_H) * DOOMGENERIC_RESX);
    }

    /* I_SetPalette runs before the first frame, but seed something sane so a
     * crash before then doesn't paint garbage. */
    memset(palette565, 0, sizeof(palette565));
}

void DG_DrawFrame(void)
{
    const uint8_t *src = (const uint8_t *)I_VideoBuffer;
    uint16_t *fb;
    int px, py;

    /* Stage timing. t_ret is when the previous DG_DrawFrame returned, so
     * (t_enter - t_ret) is time doomgeneric spent rendering the game. */
    static uint32_t t_ret;
    uint32_t t_enter = DG_GetTicksMs();
    uint32_t t_got, t_done;

    fb = TD_GetBackBuffer();     /* may block until core 0 releases a buffer */
    t_got = DG_GetTicksMs();

    if (fb == NULL)
    {
        return;
    }

    if (palette_changed)
    {
        RebuildPalette();
    }

    /* Write the destination SEQUENTIALLY. This is the single most important
     * property of this loop.
     *
     * The framebuffer is in PSRAM. Walking it in landscape order means striding
     * by PANEL_W (640 bytes) per pixel, so every write lands on a different
     * cache line -- 153600 misses per frame, which measured 5.8 fps.
     *
     * Iterating the portrait buffer in its natural order instead makes every
     * write sequential, and pushes the strided access onto the SOURCE read.
     * That is free: I_VideoBuffer is 64KB of internal SRAM, which is directly
     * addressable and has no cache lines to miss.
     *
     * For a fixed portrait row py, the landscape x is constant (lx == py), so
     * one destination row reads a single Doom COLUMN, top to bottom.
     */
    for (py = 0; py < PANEL_H; py++)
    {
        const uint8_t *col = src + scale_col[py];   /* Doom x is fixed here */
        uint16_t *dst = fb + (size_t)py * PANEL_W;

        for (px = 0; px < PANEL_W; px++)
        {
            *dst++ = palette565[col[row_off[px]]];
        }
    }

    TD_SubmitFrame();

    /* Frame timing report. Uses printf rather than ESP_LOGI because Arduino
     * ships with the IDF log level at ERROR, so ESP_LOGI is compiled out --
     * Doom's own printf output is what actually reaches the console. */
    /* Overlays go on top of the finished frame, before it is submitted. */
    ov_fb = fb;
    if (messageToPrint)
    {
        OvDrawConfirmHints();
    }
    else if (menuactive)
    {
        OvDrawMenuHints();
    }
    OvDrawFps(fps_x10, 3);
    OvDrawDetail(detailLevel, 3);

    {
        static uint32_t frames, t_last, acc_render, acc_wait, acc_blit;

        t_done = DG_GetTicksMs();
        acc_render += t_enter - t_ret;   /* doomgeneric's own work    */
        acc_wait   += t_got - t_enter;   /* stalled on the flush task */
        acc_blit   += t_done - t_got;    /* our conversion            */

        {
            /* Rolling average over 10 frames: fast enough to respond, slow
             * enough not to flicker. Separate from the 100-frame serial
             * report, which is for analysis rather than watching. */
            static uint32_t r_frames, r_last;
            if (r_last == 0) r_last = t_done;
            if (++r_frames >= 10)
            {
                uint32_t rdt = t_done - r_last;
                if (rdt > 0)
                {
                    fps_x10 = (r_frames * 10000) / rdt;
                    if (fps_x10 > 9999) fps_x10 = 9999;
                }
                r_frames = 0;
                r_last = t_done;
            }
        }

        if (t_last == 0) t_last = t_done;
        if (++frames >= 100)
        {
            uint32_t dt = t_done - t_last;
            if (dt > 0)
            {
                printf("[tdoom] %lu.%lu fps | render %lu ms  wait %lu ms  blit %lu ms\n",
                       (unsigned long)(frames * 1000 / dt),
                       (unsigned long)((frames * 10000 / dt) % 10),
                       (unsigned long)(acc_render / frames),
                       (unsigned long)(acc_wait / frames),
                       (unsigned long)(acc_blit / frames));
            }
            frames = 0;
            acc_render = acc_wait = acc_blit = 0;
            t_last = t_done;
        }
        t_ret = DG_GetTicksMs();
    }
}

void DG_SetWindowTitle(const char *title)
{
    ESP_LOGI(TAG, "WAD title: %s", title ? title : "(none)");
}
