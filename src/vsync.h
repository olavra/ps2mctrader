#ifndef VSYNC_H
#define VSYNC_H

/* Registers a VBLANK-start interrupt handler/semaphore, used to pace the
   main loop to the display refresh rate. Without this the loop spins
   unthrottled, which makes padRead sampling unreliable (it expects to be
   polled at a roughly consistent rate) and makes tick-based throttling
   (e.g. the memory card scan interval) meaningless in real time. */
void vsync_init(void);

/* Blocks until the next VBLANK-start interrupt. */
void vsync_wait(void);

#endif
