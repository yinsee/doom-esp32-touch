#ifndef DOOM_GENERIC
#define DOOM_GENERIC

#include <stdlib.h>
#include <stdint.h>

/* --- tdoom port -----------------------------------------------------------
 * CMAP256 makes pixel_t an 8-bit palette index instead of uint32_t XRGB, and
 * exports colors[]/palette_changed from i_video.c. We convert straight from
 * palette indices to RGB565 through a LUT in port_video.c, so the engine never
 * builds a 32bpp frame it would only be thrown away.
 *
 * Must match SCREENWIDTH/SCREENHEIGHT in i_video.h -- port_video.c uses these
 * for the source stride, and a mismatch produces diagonal garbage. See the
 * comment there for why native panel resolution was tried and reverted.
 * ------------------------------------------------------------------------ */
#define CMAP256 1
#include "../port/td_res.h"

#define DOOMGENERIC_RESX TD_DOOM_W
#define DOOMGENERIC_RESY TD_DOOM_H

#ifndef DOOMGENERIC_RESX
#define DOOMGENERIC_RESX 640
#endif  // DOOMGENERIC_RESX

#ifndef DOOMGENERIC_RESY
#define DOOMGENERIC_RESY 400
#endif  // DOOMGENERIC_RESY


#ifdef CMAP256

typedef uint8_t pixel_t;

#else  // CMAP256

typedef uint32_t pixel_t;

#endif  // CMAP256


extern pixel_t* DG_ScreenBuffer;

#ifdef __cplusplus
extern "C" {
#endif

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick();


//Implement below functions for your platform
void DG_Init();
void DG_DrawFrame();
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs();
int DG_GetKey(int* pressed, unsigned char* key);
void DG_SetWindowTitle(const char * title);

#ifdef __cplusplus
}
#endif

#endif //DOOM_GENERIC
