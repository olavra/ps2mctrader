/*
 * PS2 Memory Card Trader
 *
 * Minimal VBLANK-start interrupt wait, independent of the graphics
 * library (debug.h's init_scr already owns the video mode setup, so this
 * avoids a second, conflicting graph_initialize call).
 */

#include <kernel.h>

#include "vsync.h"

static int vsync_sema_id;

static int vsync_handler(int cause)
{
    ee_sema_t status;

    iReferSemaStatus(vsync_sema_id, &status);
    if (status.count < status.max_count) {
        iSignalSema(vsync_sema_id);
    }

    ExitHandler();
    return 0;
}

void vsync_init(void)
{
    ee_sema_t sema;

    sema.init_count = 0;
    sema.max_count = 1;
    sema.option = 0;
    vsync_sema_id = CreateSema(&sema);

    AddIntcHandler(INTC_VBLANK_S, vsync_handler, 0);
    EnableIntc(INTC_VBLANK_S);
}

void vsync_wait(void)
{
    WaitSema(vsync_sema_id);
}
