/*
 * tdoom — touchscreen controls.
 *
 * Layout, in landscape 480x320. The game image fills the screen, so these zones
 * are invisible overlays.
 *
 *   +--------------------+--------------------+
 *   |        ESC         |        MAP         |   ly < 80
 *   +----------+---------+---------+----------+
 *   | MOVE PAD |                   | TURN PAD |
 *   | 160x160  |    dead centre    | 160x160  |   ly 80..239
 *   |  (3x3)   |                   |  L | R   |
 *   +----------+---------+---------+----------+
 *   |        USE         |        FIRE        |   ly >= 240
 *   +--------------------+--------------------+
 *
 * The movement pad is three horizontal bands -- forward on top, back on the
 * bottom, and a narrow strafe strip between them -- so every touch on it moves
 * you. The 160px gap between the pads is unbound.
 *
 * These are ZONES, not drag sticks: where you touch is what you get. A virtual
 * analog stick was tried and felt worse -- on glass with no tactile centre,
 * having to drag before anything happens is slower and less certain than
 * putting a thumb straight on the direction you want.
 *
 * In menus the same geography means: back in the ESC corner, the move pad
 * scrolls up/down, the turn pad nudges sliders left/right, and everything else
 * selects. One layout to learn, not two.
 *
 * Both touch points are evaluated independently and their keys OR'd together,
 * so holding FORWARD while tapping TURN works.
 *
 * ---------------------------------------------------------------------------
 * Why the state is held across failed reads
 *
 * The AXS15231B returns 0xC8/0xFF filler on a large fraction of reads -- the
 * bring-up probe saw it in over half of them. TD_ReadTouch reports that as -1,
 * distinct from a real "0 fingers".
 *
 * Treating -1 as finger-up (as the first version did) meant every corrupt read
 * released the movement key for one tic, and walking visibly stuttered. The
 * fix is to keep the last good state through failures and only release on
 * either a trustworthy zero or TOUCH_HOLD_MS of uninterrupted failure.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../doomgeneric/doomgeneric.h"
#include "../doomgeneric/doomkeys.h"

/* Implemented in tdoom.ino (the I2C read needs the C++ Wire library).
 * Returns 2/1/0 points, or -1 if the read failed. */
extern int TD_ReadTouch(uint8_t *count, uint16_t *x0, uint16_t *y0,
                        uint16_t *x1, uint16_t *y1);

/* Doom's menu state; the control map changes completely when a menu is up. */
extern int menuactive;

/* Set while a yes/no prompt (quit, end game, nightmare skill) is on screen.
 * M_Responder accepts ONLY 'y' / 'n' / escape / space for these, so without a
 * dedicated mapping they cannot be answered at all. */
extern int messageToPrint;
extern int key_menu_confirm;   /* 'y' by default */
extern int key_menu_abort;     /* 'n' by default */

#define PANEL_W 320

/* Zone geometry lives in td_res.h so port_video.c draws its hints over exactly
 * these regions. Two 160x160 pads with a dead gap between them; see that header
 * for the full map. */
#define SCR_W   TD_SCR_W
#define SCR_H   TD_SCR_H
#define ROW1    TD_ROW1
#define ROW3    TD_ROW3

/* Poll the controller at ~40Hz. Doom runs 35 tics/s, and reading faster than
 * this is what provokes the filler responses in the first place. */
#define TOUCH_POLL_MS 25

/* Keep the last good state through this much continuous read failure before
 * concluding the finger really is gone. Long enough to ride out a burst of
 * corrupt reads, short enough that releasing feels immediate. */
#define TOUCH_HOLD_MS 150

typedef enum {
    K_FORWARD, K_BACK,
    K_TURN_L,  K_TURN_R,
    K_STRAFE_L, K_STRAFE_R,
    K_FIRE, K_USE,
    K_ESCAPE, K_ENTER, K_MAP,
    K_YES, K_NO,
    K_COUNT
} td_key_t;

/* Not const: K_YES / K_NO are refreshed from key_menu_confirm /
 * key_menu_abort, which are config variables, not compile-time constants. */
static unsigned char key_codes[K_COUNT] = {
    [K_FORWARD] = KEY_UPARROW,
    [K_BACK]    = KEY_DOWNARROW,
    [K_TURN_L]  = KEY_LEFTARROW,
    [K_TURN_R]  = KEY_RIGHTARROW,
    [K_STRAFE_L] = KEY_STRAFE_L,
    [K_STRAFE_R] = KEY_STRAFE_R,
    [K_FIRE]    = KEY_FIRE,
    [K_USE]     = KEY_USE,
    [K_ESCAPE]  = KEY_ESCAPE,
    [K_ENTER]   = KEY_ENTER,
    [K_MAP]     = KEY_TAB,
    [K_YES]     = 'y',
    [K_NO]      = 'n',
};

/* Keys currently held, and what the engine has already been told. Comparing
 * the two turns continuous touch state into discrete key events. */
static uint16_t cur_keys;
static uint16_t sent_keys;

/* Debounce state (see the header comment). */
static uint32_t last_poll_ms;
static uint32_t last_good_ms;
static uint8_t  finger_was_down;

#define BIT(k) ((uint16_t)1u << (k))

/* Portrait touch coordinates -> the landscape frame the layout uses. Exact
 * inverse of the rotation in port_video.c. */
static void ToLandscape(uint16_t tx, uint16_t ty, int *lx, int *ly)
{
    *lx = (int)ty;
    *ly = PANEL_W - 1 - (int)tx;
}

/* The movement pad: three horizontal bands, with a narrow strafe strip across
 * the middle.
 *
 *      +-------------------------+
 *      |          FWD            |   40%
 *      +------------+------------+
 *      | STRAFE  L  | STRAFE  R  |   20%
 *      +------------+------------+
 *      |          BACK           |   40%
 *      +-------------------------+
 *
 * Forward and back get the generous targets because they are what you hold;
 * strafing is a deliberate sidestep, so a narrow strip you have to aim for
 * keeps it from firing by accident while walking. No dead spots. */
static uint16_t MovePadKeys(int lx, int ly)
{
    if (ly < TD_MZ_SY0)
    {
        return BIT(K_FORWARD);
    }

    if (ly >= TD_MZ_SY1)
    {
        return BIT(K_BACK);
    }

    return (lx < TD_MZ_CX) ? BIT(K_STRAFE_L) : BIT(K_STRAFE_R);
}

static uint16_t GameKeysFor(int lx, int ly)
{
    /* Top row: escape and automap. */
    if (ly < ROW1)
    {
        return (lx < SCR_W / 2) ? BIT(K_ESCAPE) : BIT(K_MAP);
    }

    /* Bottom row: use and fire. The aim assist presses both for you most of the
     * time, so these cover what it deliberately will not -- a door it did not
     * spot, or a barrel too close to detonate safely. */
    if (ly >= ROW3)
    {
        return (lx < SCR_W / 2) ? BIT(K_USE) : BIT(K_FIRE);
    }

    /* Middle band: a pad at each edge, nothing in between. */
    if (lx < TD_MZ_X3)
    {
        return MovePadKeys(lx, ly);
    }

    if (lx >= TD_TZ_X0)
    {
        return (lx < TD_TZ_XM) ? BIT(K_TURN_L) : BIT(K_TURN_R);
    }

    /* Unbound gap -- the part of the picture that matters most. */
    return 0;
}

/* Yes/no prompt mapping. Same geography again: the corner that means "back"
 * everywhere else answers NO, and the area that means "select" answers YES. */
static uint16_t ConfirmKeysFor(int lx, int ly)
{
    if (ly < ROW1 && lx < SCR_W / 2)
    {
        return BIT(K_NO);
    }
    return BIT(K_YES);
}

static uint16_t MenuKeysFor(int lx, int ly)
{
    /* Top row: back on the left (the ESC corner in game), select on the right. */
    if (ly < ROW1)
    {
        return (lx < SCR_W / 2) ? BIT(K_ESCAPE) : BIT(K_ENTER);
    }

    /* Bottom row selects -- where USE/FIRE live in game. */
    if (ly >= ROW3)
    {
        return BIT(K_ENTER);
    }

    /* Movement pad scrolls the highlight: its forward row is up, its back row
     * is down, exactly as in game. */
    /* The move pad's upper half scrolls up, its lower half scrolls down,
     * matching where forward and back are in game. */
    if (lx < TD_MZ_X3)
    {
        return (ly < TD_MZ_CY) ? BIT(K_FORWARD) : BIT(K_BACK);
    }

    /* Turn pad nudges sliders. Doom's Options menu has several slider items
     * (Screen Size, volumes, sensitivity) that respond ONLY to left/right. */
    if (lx >= TD_TZ_X0)
    {
        return (lx < TD_TZ_XM) ? BIT(K_TURN_L) : BIT(K_TURN_R);
    }

    /* The in-game dead centre confirms here: nothing to protect against in a
     * menu, and a large select target is easier to hit. */
    return BIT(K_ENTER);
}

static void SampleTouch(void)
{
    uint8_t count = 0;
    uint16_t tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
    uint32_t now = DG_GetTicksMs();
    int lx, ly;
    uint16_t keys = 0;
    int n;

    /* Rate-limit: DG_GetKey is called several times per tic, but the
     * controller must not be hammered. */
    if (now - last_poll_ms < TOUCH_POLL_MS)
    {
        return;
    }
    last_poll_ms = now;

    n = TD_ReadTouch(&count, &tx0, &ty0, &tx1, &ty1);

    if (n < 0)
    {
        /* Read failed. Hold the current state -- this is the stutter fix. */
        if (now - last_good_ms > TOUCH_HOLD_MS)
        {
            cur_keys = 0;
            finger_was_down = 0;
        }
        return;
    }

    last_good_ms = now;

    if (n == 0)
    {
        cur_keys = 0;
        finger_was_down = 0;
        return;
    }

    ToLandscape(tx0, ty0, &lx, &ly);

    if (messageToPrint || menuactive)
    {
        /* Menus and prompts respond to a TAP, not a hold: holding would scroll
         * the selection away at 35 tics per second. */
        if (!finger_was_down)
        {
            key_codes[K_YES] = (unsigned char)key_menu_confirm;
            key_codes[K_NO]  = (unsigned char)key_menu_abort;

            keys = messageToPrint ? ConfirmKeysFor(lx, ly)
                                  : MenuKeysFor(lx, ly);
        }
    }
    else
    {
        keys = GameKeysFor(lx, ly);

        if (n >= 2)
        {
            ToLandscape(tx1, ty1, &lx, &ly);
            keys |= GameKeysFor(lx, ly);
        }
    }

    finger_was_down = 1;
    cur_keys = keys;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    uint16_t diff;
    int i;

    /* Only re-sample once the previous state has been fully drained, so one
     * sample cannot be reported twice. */
    if (cur_keys == sent_keys)
    {
        SampleTouch();
    }

    diff = cur_keys ^ sent_keys;
    if (diff == 0)
    {
        return 0;
    }

    for (i = 0; i < K_COUNT; i++)
    {
        if (diff & BIT(i))
        {
            int now_down = (cur_keys & BIT(i)) != 0;

            sent_keys ^= BIT(i);   /* this one is now reported */

            *pressed = now_down;
            *key = key_codes[i];
            return 1;
        }
    }

    return 0;
}
