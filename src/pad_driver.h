#ifndef PAD_DRIVER_H
#define PAD_DRIVER_H

#include <tamtypes.h>

/* Loads the PADMAN IOP module and opens the port 0 controller.
   Assumes SIO2MAN has already been loaded (see mc_driver_init). */
void pad_driver_init(void);

/* Returns the bitmask of PAD_* buttons newly pressed since the last call.
   Also latches the analog stick positions for pad_driver_left_stick. */
u32 pad_driver_read_new_buttons(void);

/* Left stick position from the most recent read, each axis in -1.0 to 1.0
   with a small dead zone applied and +Y pointing down. Both are 0 on a
   digital-only controller. */
void pad_driver_left_stick(float *x, float *y);

#endif
