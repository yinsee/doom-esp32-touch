/*
 * tdoom — the one place the render resolution is defined.
 *
 * It used to be duplicated across i_video.h (SCREENWIDTH/SCREENHEIGHT),
 * doomgeneric.h (DOOMGENERIC_RESX/RESY) and tdoom.ino (the pre-reserved
 * framebuffer size). They drifted: the engine ended up configured for 320x200
 * while the sketch reserved a 480x320 buffer, which fragmented internal RAM
 * badly enough that the Doom task could not be created at all -- a blank
 * screen with no error anywhere.
 *
 * Height is 288, not the panel's 320. At 320 the 153600-byte framebuffer
 * consumed the large internal DRAM region and left a 15860-byte largest free
 * block, too small for the Doom task's 24KB stack. 288 costs 15360 bytes less
 * and upscales to 320 by 1.11x, which is not perceptible. Width is native.
 */

#ifndef TDOOM_TD_RES_H
#define TDOOM_TD_RES_H

#define TD_DOOM_W 480
#define TD_DOOM_H 288

/* The design space Doom's 2D art is authored in. V_DrawPatch scales from this
 * to TD_DOOM_W/H so menus, the title screen and the intermission fill the panel
 * instead of sitting in the top-left corner. The 3D view is unaffected -- it
 * renders natively at TD_DOOM_W/H. */
#define V_BASEW 320
#define V_BASEH 200

/* ---------------------------------------------------------------------------
 * Touch zone geometry, in the landscape 480x320 frame.
 *
 * Shared by port_input.c (which decides what a touch means) and port_video.c
 * (which draws the menu hints over those zones). Defined once so the hints
 * cannot end up pointing at the wrong regions.
 * ------------------------------------------------------------------------- */

#define TD_SCR_W      480          /* landscape width  */
#define TD_SCR_H      320          /* landscape height */

/* Four equal rows of 25%. The middle two form a 160px-tall band holding a
 * 160x160 movement pad on the left and a 160x160 turn pad on the right, with a
 * 160px dead gap between them:
 *
 *      +--------------------+--------------------+   row 0   ly <  80
 *      |        ESC         |        MAP         |
 *      +----------+---------+---------+----------+   ly = 80
 *      |          |                   |          |
 *      | MOVE PAD |    dead centre    | TURN PAD |   rows 1-2, 160 tall
 *      | 160x160  |      160 wide     | 160x160  |
 *      |          |                   |          |
 *      +----------+---------+---------+----------+   ly = 240
 *      |        USE         |        FIRE        |
 *      +--------------------+--------------------+   row 3
 *       lx 0-159    160-319     320-479
 *
 * The dead centre is where the picture matters most and where a thumb is most
 * likely to stray, so nothing is bound there.
 */
#define TD_SCR_W      480          /* landscape width  */
#define TD_SCR_H      320          /* landscape height */

#define TD_ROW        (TD_SCR_H / 4)   /* 80 */
#define TD_ROW1       (TD_ROW * 1)     /* end of the esc/map band    */
#define TD_ROW3       (TD_ROW * 3)     /* start of the use/fire band */

/* Movement pad: 160x160 on the left of the middle band, in three horizontal
 * bands with a narrow strafe strip across the middle:
 *
 *      +-------------------------+
 *      |                         |
 *      |          FWD            |   40%  (64px)
 *      |                         |
 *      +------------+------------+
 *      | STRAFE  L  | STRAFE  R  |   20%  (32px)
 *      +------------+------------+
 *      |                         |
 *      |          BACK           |   40%  (64px)
 *      |                         |
 *      +-------------------------+
 *
 * Forward and back get the generous targets because they are what you hold;
 * strafing is a deliberate sidestep, so a narrow strip you have to aim for is
 * fine and keeps it from being hit by accident while walking. No dead spots --
 * every point on the pad does something.
 */
#define TD_PAD        160              /* both pads are TD_PAD square */

#define TD_MZ_X0      0
#define TD_MZ_X3      TD_PAD                       /* 160 */
#define TD_MZ_CX      (TD_PAD / 2)                 /* 80  */

#define TD_MZ_Y0      TD_ROW1                      /* 80  */
#define TD_MZ_Y3      (TD_ROW1 + TD_PAD)           /* 240 */
#define TD_MZ_CY      (TD_ROW1 + TD_PAD / 2)       /* 160 */

/* The strafe strip: the middle 20% of the pad's height. */
#define TD_MZ_SY0     (TD_ROW1 + (TD_PAD * 40) / 100)   /* 144 */
#define TD_MZ_SY1     (TD_ROW1 + (TD_PAD * 60) / 100)   /* 176 */

/* Turn pad: 160x160 on the right of the same band, split down the middle. */
#define TD_TZ_X0      (TD_SCR_W - TD_PAD)          /* 320 */
#define TD_TZ_XM      (TD_SCR_W - TD_PAD / 2)      /* 400 */

#endif /* TDOOM_TD_RES_H */
