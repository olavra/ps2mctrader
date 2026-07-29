/*
 * PS2 Memory Card Trader
 *
 * Sound effect playback via audsrv. Only the ADPCM path is used: each effect
 * is uploaded to SPU2 memory once at startup and then triggered on one of the
 * 24 hardware voices, so a click costs no streaming and no EE-side mixing.
 */

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <audsrv.h>
#include <stdio.h>

#include "audio.h"
#include "sound_data.h"

/* Output volume in percent. These are UI clicks, so they sit well below full
   scale. It is applied as the master volume rather than per voice: a voice's
   volume can only be set after audsrv_ch_play_adpcm has already started it,
   which would let the attack through at full level. */
#define SFX_VOLUME 35

static audsrv_adpcm_t samples[SFX_COUNT];

/* Set only once every sample is in SPU2 memory; until then (or if audio
   failed to come up at all) audio_play is a no-op. */
static int audio_ready;

void audio_init(void)
{
    int ret, id;

    /* LIBSD is the SPU2 driver audsrv sits on top of. */
    ret = SifLoadModule("rom0:LIBSD", 0, NULL);
    if (ret < 0) {
        printf("Failed to load LIBSD (%d)\n", ret);
        return;
    }

    ret = SifExecModuleBuffer(audsrv_irx, audsrv_irx_size, 0, NULL, &id);
    if (ret < 0) {
        printf("Failed to load audsrv (%d)\n", ret);
        return;
    }

    if (audsrv_init() != 0) {
        printf("audsrv init failed: %s\n", audsrv_get_error_string());
        return;
    }

    audsrv_adpcm_init();
    audsrv_set_volume(SFX_VOLUME);

    /* Indexed by sfx_id. The sizes are runtime symbols in the generated
       blob, hence pointers rather than values. */
    static const struct {
        unsigned char *data;
        const unsigned int *size;
    } blobs[SFX_COUNT] = {
        { sfx_tick,  &sfx_tick_size },
        { sfx_open,  &sfx_open_size },
        { sfx_close, &sfx_close_size },
        { sfx_view,  &sfx_view_size },
    };

    for (int i = 0; i < SFX_COUNT; i++) {
        if (audsrv_load_adpcm(&samples[i], blobs[i].data, (int)*blobs[i].size) != 0) {
            printf("Failed to upload sound %d: %s\n", i, audsrv_get_error_string());
            return;
        }
    }

    audio_ready = 1;
}

void audio_play(sfx_id id)
{
    if (!audio_ready || id < 0 || id >= SFX_COUNT) {
        return;
    }

    /* -1 picks any free voice, so a new effect never cuts off one that is
       still ringing out. If all 24 are busy the sound is simply dropped. */
    int ch = audsrv_ch_play_adpcm(-1, &samples[id]);
    if (ch >= 0) {
        audsrv_adpcm_set_volume_and_pan(ch, MAX_VOLUME, 0);
    }
}
