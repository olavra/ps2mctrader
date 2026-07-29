/*
 * PS2 Memory Card Trader
 *
 * Basic controller polling via PS2SDK libpad, port 0 slot 0 only
 * (no multitap, no analog/dual shock setup needed for a menu UI).
 */

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <stdio.h>

#include "pad_driver.h"

static char pad_dma_buf[256] __attribute__((aligned(64)));
static u32 old_buttons = 0;

/* Latched left stick, -1.0 to 1.0 per axis. */
static float stick_x, stick_y;

/* Set once the controller has been switched into analog mode. */
static int analog_ready;

/* Fraction of the stick's travel around centre that is ignored, to absorb
   the drift a worn stick has at rest. */
#define STICK_DEADZONE 0.2f

/* Maps a raw 0..255 axis (128 = centre) to -1.0..1.0, with the dead zone
   removed and the remaining travel rescaled so it still reaches 1.0. */
static float axis_to_float(unsigned char raw)
{
    float v = ((float)raw - 128.0f) / 127.0f;

    if (v > STICK_DEADZONE) {
        return (v - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    }
    if (v < -STICK_DEADZONE) {
        return (v + STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    }
    return 0.0f;
}

void pad_driver_init(void)
{
    int ret;

    ret = SifLoadModule("rom0:PADMAN", 0, NULL);
    if (ret < 0) {
        printf("Failed to load PADMAN (%d)\n", ret);
    }

    padInit(0);
    padPortOpen(0, 0, pad_dma_buf);
}

u32 pad_driver_read_new_buttons(void)
{
    struct padButtonStatus buttons;
    u32 pad_data, new_buttons;
    int state;

    state = padGetState(0, 0);
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
        return 0;
    }

    /* Request analog mode once the pad has settled. Done here rather than in
       init so a controller plugged in later still gets configured, and so
       startup never blocks waiting for one. */
    if (!analog_ready && state == PAD_STATE_STABLE) {
        if (padInfoMode(0, 0, PAD_MODETABLE, -1) > 0) {
            padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
        }
        analog_ready = 1;
    }

    if (padRead(0, 0, &buttons) == 0) {
        return 0;
    }

    stick_x = axis_to_float(buttons.ljoy_h);
    stick_y = axis_to_float(buttons.ljoy_v);

    pad_data = 0xffff ^ buttons.btns;
    new_buttons = pad_data & ~old_buttons;
    old_buttons = pad_data;

    return new_buttons;
}

void pad_driver_left_stick(float *x, float *y)
{
    *x = stick_x;
    *y = stick_y;
}
