#ifndef PS1ICON_H
#define PS1ICON_H

#include <tamtypes.h>

/*
 * PS1 save icon library: parses the title frame that heads a PlayStation 1
 * memory card save and draws its icon mapped onto the faces of a cube.
 *
 * Like ps2icon it takes file contents as a plain buffer, so it does not care
 * whether they came from a physical card, a .VMC image or a host file, and it
 * depends only on gs_render for drawing.
 *
 * FORMAT SUMMARY (see REFERENCE.md for the full tables)
 *
 * A PS1 save has no icon.sys / .ico pair and no 3D model. Everything lives in
 * the first 512 bytes of the save - the title frame:
 *
 *   0x00  2    magic "SC"
 *   0x02  1    icon display flag: 0x11/0x12/0x13 = 1/2/3 animation frames
 *   0x03  1    blocks the save occupies
 *   0x04  64   title, Shift-JIS
 *   0x60  32   CLUT: 16 entries of 16 bit X1B5G5R5, little-endian
 *   0x80  128  icon bitmap, frame 1 (16x16, 4 bpp, low nibble = left pixel)
 *   0x100 128  frame 2
 *   0x180 128  frame 3
 *
 * A CLUT entry of 0x0000 (black, STP clear) means fully transparent. The bit
 * layout otherwise matches the GS's CT16, so a texel is just the entry with
 * the alpha bit forced on.
 *
 * RENDERING
 *
 * A 16x16 bitmap has no geometry to show, so the icon is mapped onto all six
 * faces of a cube - the same cube, projection and lamp the pending-save
 * placeholder in main.c uses, so a tile that starts as a plain cube simply
 * gains its texture once the save has been read. All animation frames are
 * uploaded together as tiles of one texture and the current frame is selected
 * by UV offset, so playback costs nothing beyond the geometry.
 *
 * Icons are drawn with nearest-neighbour filtering: at 16x16 stretched over a
 * tile, linear filtering turns the artwork into a blur.
 */

#define PS1ICON_HEADER_SIZE 512
#define PS1ICON_DIM 16
#define PS1ICON_MAX_FRAMES 3
#define PS1ICON_TITLE_MAX 64

/* The texture handed to gs_render is a full GS_ICON_TEX_DIM square (the icon
   slots are allocated at that size); the frames occupy the top-left corner as
   a row of PS1ICON_DIM tiles and the rest stays unused. */
typedef struct {
    int frames;                     /* 1..3 animation frames */
    char title[PS1ICON_TITLE_MAX];  /* title frame's title, transliterated */
    u16 tex[128 * 128] __attribute__((aligned(128)));
} ps1icon_model;

/* Parses a save's title frame. `buf` must hold at least the first
   PS1ICON_HEADER_SIZE bytes of the save file. Returns 1 on success, 0 if the
   buffer is too short, is not an "SC" title frame, or declares no icon frames
   - `out->title` is still filled in whenever the magic matched, so a save
   with a broken icon can keep its real name. */
int ps1icon_parse(const void *buf, int len, ps1icon_model *out);

/* Uploads a model's texture into gs_render icon slot `slot`. Call once per
   model (per slot) before drawing it there. */
void ps1icon_upload(const ps1icon_model *m, int slot);

/* Draws the icon cube centred on (cx, cy) at `size` pixels, textured from
   gs_render icon slot `slot`. `t` is elapsed seconds and drives the frame
   animation; `spin` is the rotation in radians about the vertical axis.
   Requires ps1icon_upload of this model into that slot. */
void ps1icon_draw(const ps1icon_model *m, int slot,
                  int cx, int cy, int size, float t, float spin);

/* Records the cube's rest pose (frame 0, fixed off-axis yaw) into slot's
   prepacked static packet - see gs_icon_static_* in gs_render.h. */
void ps1icon_prepack(const ps1icon_model *m, int slot, int cx, int cy, int size);

/* The four backdrop corner colours to show behind this icon in the
   full-screen view. PS1 saves carry no equivalent of icon.sys's corner
   colours, so this is a fixed neutral gradient. */
void ps1icon_bg_colors(u32 out[4]);

#endif
