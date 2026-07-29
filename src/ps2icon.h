#ifndef PS2ICON_H
#define PS2ICON_H

#include <tamtypes.h>

/*
 * PS2 save icon library: parses the icon.sys / .ico pair that describes a
 * PlayStation 2 memory card save's 3D icon, and draws it.
 *
 * The library is self-contained and takes file contents as plain buffers, so
 * it does not care whether they came from a memory card, a host filesystem or
 * a .psu archive. It depends only on gs_render for drawing.
 *
 * FORMAT SUMMARY
 *
 * icon.sys is a fixed 964-byte descriptor: magic "PS2D", the save's title in
 * Shift-JIS, the filenames of the .ico files for the normal/copy/delete
 * states, four backdrop corner colours, and a lighting rig of three
 * directional lights plus an ambient term.
 *
 * A .ico is an animated 3D model, not an image:
 *
 *   header (20 bytes)  magic 0x00010000, animation shape count, texture type,
 *                      constant 0x3F800000, vertex count (always a multiple
 *                      of 3 - vertices are consecutive triangles).
 *   vertex segment     per vertex: one 8-byte position per animation shape,
 *                      then an 8-byte normal, then 4 bytes of UV and 4 bytes
 *                      of RGBA. Positions, normals and UVs are 16-bit fixed
 *                      point with 4096 = 1.0.
 *   animation segment  a 20-byte header (timeline length in 1/60s ticks at
 *                      offset 4, playback speed as a float at offset 8, frame
 *                      count at offset 16) followed by one 8-byte record per
 *                      frame (shape id, key count), each trailed by key_count
 *                      8-byte keys of (time, value) floats.
 *   texture segment    128x128 A1B5G5R5. Present when texture type has bit
 *                      0x04, RLE-compressed when it also has bit 0x08.
 *
 * COORDINATE SYSTEM
 *
 * Icons are authored for a camera on the negative Z axis looking towards +Z,
 * with "up" along -Y. Model X and Y therefore map straight to screen X and Y
 * with no flip, and smaller Z is nearer the camera.
 *
 * Triangle winding is not consistent across icon meshes, so back faces cannot
 * be culled by facing - doing so punches holes in the model. Visibility is
 * left entirely to the depth test.
 *
 * ANIMATION
 *
 * Each animation shape is a morph target holding a full set of vertex
 * positions. Each frame record is a weight track for the shape it names: its
 * keys are (time, value) pairs giving that shape's blend weight over the
 * timeline, linearly interpolated between keys and held flat outside them.
 * The timeline is frame_length ticks long and advances anim_speed ticks per
 * 1/60s, wrapping at the end. The pose at any instant is the weighted sum of
 * all shapes, so playback direction comes from the key times, not from the
 * order shapes or frame records are stored in (several icons store them
 * reversed).
 *
 * LIGHTING
 *
 * Final colour is (ambient + sum of diffuse terms) * texel. Each diffuse term
 * is max(dot(-light_dir, normal), 0) * light_colour. The per-vertex RGBA in
 * the .ico takes no part in it. Lights rotate with the model, so shading is
 * fixed relative to the geometry rather than to the world.
 */

/* Hard limits. A model occupies roughly 420 KB. Icons exceeding the shape or
   vertex limit are rejected outright rather than truncated: dropping shapes
   makes the frame list reference targets that no longer exist, which tears
   the geometry apart. */
#define PS2ICON_MAX_VERTS 3072
#define PS2ICON_MAX_SHAPES 16
#define PS2ICON_MAX_FRAMES 64
#define PS2ICON_MAX_KEYS 1024

#define PS2ICON_TEX_DIM 128
#define PS2ICON_TEX_PIXELS (PS2ICON_TEX_DIM * PS2ICON_TEX_DIM)

#define PS2ICON_SYS_SIZE 964
#define PS2ICON_TITLE_MAX 64
#define PS2ICON_NAME_MAX 65

/* Contents of an icon.sys. */
/* The three states a save's icon can be shown in. A save names one .ico per
   state in its icon.sys; most name the same file three times, but some give
   the copy and delete states their own model (a box being carried away, the
   icon crumbling). Saves that leave a state's name blank fall back to the
   normal one. */
typedef enum {
    PS2ICON_STATE_NORMAL,
    PS2ICON_STATE_COPY,
    PS2ICON_STATE_DELETE,
    PS2ICON_STATE_COUNT
} ps2icon_state;

typedef struct {
    char title[PS2ICON_TITLE_MAX];    /* both title lines, joined, ASCII */
    /* .ico per state, indexed by ps2icon_state; empty when unnamed. */
    char icon_file[PS2ICON_STATE_COUNT][PS2ICON_NAME_MAX];
    u32 bg_color[4];                  /* upper-left, upper-right, lower-left,
                                         lower-right, packed as UI_RGB */
    float light_dir[3][3];            /* three normalized light directions */
    float light_col[3][3];            /* their RGB intensities */
    float ambient[3];
} ps2icon_sys;

/* A parsed .ico. Positions, normals and UVs stay in their native 16-bit
   fixed-point form; PS2ICON_FIXED converts them. */
#define PS2ICON_FIXED 4096.0f

typedef struct {
    u32 shapes;                                       /* morph targets */
    u32 verts;                                        /* multiple of 3 */
    u32 frames;                                       /* weight tracks */
    float frame_length;                               /* timeline ticks (60Hz) */
    float anim_speed;                                 /* ticks per 1/60s */
    u8 frame_shape[PS2ICON_MAX_FRAMES];               /* shape each track drives */
    u8 frame_keys[PS2ICON_MAX_FRAMES];                /* keys stored per track */
    u16 frame_key_start[PS2ICON_MAX_FRAMES];          /* track's first key below */
    float key_time[PS2ICON_MAX_KEYS];
    float key_value[PS2ICON_MAX_KEYS];
    s16 pos[PS2ICON_MAX_SHAPES][PS2ICON_MAX_VERTS][3];
    s16 nrm[PS2ICON_MAX_VERTS][3];
    s16 uv[PS2ICON_MAX_VERTS][2];
    u8 col[PS2ICON_MAX_VERTS][4];                     /* parsed, unused: the
                                                         PS2 lights the icon
                                                         from icon.sys alone */
    /* A1B5G5R5, opaque. gsKit uploads this with a DMA chain that reads
       straight from the pointer, and the DMAC drops the low 4 address bits,
       so a misaligned buffer reaches VRAM shifted sideways by several
       texels. 128 is gsKit's documented texture alignment. */
    u16 tex[PS2ICON_TEX_PIXELS] __attribute__((aligned(128)));
} ps2icon_model;

/* Parses a 964-byte icon.sys. Returns 1 on success, 0 if the buffer is too
   short or is not a PS2D descriptor. */
int ps2icon_sys_parse(const void *buf, int len, ps2icon_sys *out);

/* The .ico filename to use for `state`, falling back to the normal one when
   that state names no file of its own. Never NULL; empty when the save names
   no icon at all. */
const char *ps2icon_sys_icon(const ps2icon_sys *sys, ps2icon_state state);

/* Parses a .ico. `sys` supplies the lighting used when shading. Returns 1 on
   success, 0 if the buffer is malformed or exceeds the limits above. */
int ps2icon_parse(const void *buf, int len, ps2icon_model *out);

/* Uploads a model's texture into gs_render icon slot `slot`. Call once per
   model (per slot) before drawing it there. */
void ps2icon_upload(const ps2icon_model *m, int slot);

/* Draws the model centred on (cx, cy), scaled to fit `size` pixels, textured
   from gs_render icon slot `slot`. `t` is elapsed seconds and drives the
   morph; `spin` is the rotation in radians about the vertical axis. With
   `lights` zero the icon.sys rig is bypassed and the texture is shown
   unshaded. Requires ps2icon_upload of this model into that slot. */
void ps2icon_draw(const ps2icon_model *m, const ps2icon_sys *sys, int slot,
                  int cx, int cy, int size, float t, float spin, int lights);

/* Records the model's frozen rest pose (animation time 0, no spin, unlit)
   into slot's prepacked static packet - gs_icon_static_enable then draws it
   every frame at near-zero CPU cost. Same layout maths as ps2icon_draw. */
void ps2icon_prepack(const ps2icon_model *m, const ps2icon_sys *sys, int slot,
                     int cx, int cy, int size);

#endif
