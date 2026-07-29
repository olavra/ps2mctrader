#ifndef AUDIO_H
#define AUDIO_H

/* UI sound effects, played through audsrv's ADPCM voices (fire and forget:
   the samples live in SPU2 memory, so playing one costs a single RPC and
   never blocks the render loop). */

typedef enum {
    SFX_TICK,   /* selector moved */
    SFX_OPEN,   /* a memory card was opened */
    SFX_CLOSE,  /* backed out of a view */
    SFX_VIEW,   /* a save was opened full screen */
    SFX_COUNT
} sfx_id;

/* Loads the audsrv IOP module and uploads every sample to the SPU2. Requires
   the IOP-side LoadModuleBuffer patch (see main), since audsrv.irx is
   embedded in the ELF rather than read from a filesystem. Failure is not
   fatal - audio_play then does nothing and the UI runs silent. */
void audio_init(void);

void audio_play(sfx_id id);

#endif
