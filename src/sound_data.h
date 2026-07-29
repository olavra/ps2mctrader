#ifndef SOUND_DATA_H
#define SOUND_DATA_H

/* UI sound effects in SPU2 ADPCM (.adp) form plus the audsrv IOP module,
   both generated from src/sounds and the PS2SDK by scripts/gen_sound_data.py.
   Loaded by audio.c; nothing else should touch these directly. */

extern unsigned char sfx_tick[];
extern unsigned int sfx_tick_size;

extern unsigned char sfx_open[];
extern unsigned int sfx_open_size;

extern unsigned char sfx_close[];
extern unsigned int sfx_close_size;

extern unsigned char sfx_view[];
extern unsigned int sfx_view_size;

extern unsigned char audsrv_irx[];
extern unsigned int audsrv_irx_size;

#endif
