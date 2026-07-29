/*
 * PS1 save icon parsing and rendering - see ps1icon.h.
 */

#include <tamtypes.h>
#include <string.h>
#include <math.h>

#include "ps1icon.h"
#include "gs_render.h"
#include "sjis.h"
#include "ui.h"

/* Title frame field offsets. */
#define HDR_FLAG 0x02
#define HDR_TITLE 0x04
#define HDR_TITLE_LEN 64
#define HDR_CLUT 0x60
#define HDR_BITMAP 0x80
#define BITMAP_BYTES 128

/* Texture tiles are laid out along the top edge of the icon slot's texture. */
#define TEX_DIM GS_ICON_TEX_DIM

/* Animation rate. Real hardware runs icons at roughly 6-10 fps; the exact
   figure is not part of the save, so this is a fixed sensible cadence. */
#define FRAME_SECS 0.125f

/* Cube geometry, identical to the placeholder cube in main.c so a pending
   tile keeps its size and pose when its texture arrives. */
#define CUBE_HALF_SCALE 0.30f   /* half-extent as a fraction of the tile */
#define CUBE_TILT 0.42f         /* fixed forward tilt, radians */
#define CUBE_DEPTH_SCALE 16.0f  /* model Z to GS depth units */
#define CUBE_REST_YAW 0.65f     /* off-axis yaw so a still cube reads as 3D */

/* Colour written where the icon is transparent. A cut-out would let the
   backdrop through the cube and expose its inner faces, so transparent texels
   become the cube's body instead - the same dark slate the app uses behind an
   icon-less save. */
#define TEXEL(r, g, b) \
    ((u16)(((r) >> 3) | (((g) >> 3) << 5) | (((b) >> 3) << 10) | 0x8000))
#define BODY_TEXEL TEXEL(24, 32, 56)

/* Cube corners as unit signs of (x, y, z), four per face in the order
   top-left, top-right, bottom-right, bottom-left. Screen Y grows downward, so
   "top" is -Y. Faces are front (-Z), back, right, left, top, bottom. */
static const signed char face_corner[6][4][3] = {
    { { -1, -1, -1 }, {  1, -1, -1 }, {  1,  1, -1 }, { -1,  1, -1 } },
    { {  1, -1,  1 }, { -1, -1,  1 }, { -1,  1,  1 }, {  1,  1,  1 } },
    { {  1, -1, -1 }, {  1, -1,  1 }, {  1,  1,  1 }, {  1,  1, -1 } },
    { { -1, -1,  1 }, { -1, -1, -1 }, { -1,  1, -1 }, { -1,  1,  1 } },
    { { -1, -1,  1 }, {  1, -1,  1 }, {  1, -1, -1 }, { -1, -1, -1 } },
    { { -1,  1, -1 }, {  1,  1, -1 }, {  1,  1,  1 }, { -1,  1,  1 } },
};

static const float face_normal[6][3] = {
    { 0, 0, -1 }, { 0, 0, 1 },
    { 1, 0, 0 },  { -1, 0, 0 },
    { 0, -1, 0 }, { 0, 1, 0 },
};

/* ------------------------------------------------------------------ title */

/* Transliterates the title frame's 64 Shift-JIS bytes into ASCII (see
   sjis.h - the same table serves the PS2 side). Trailing
   blanks are trimmed: titles are commonly padded out with full-width spaces. */
static void parse_title(const u8 *src, char *out, int out_len)
{
    int n = 0;

    for (int i = 0; i < HDR_TITLE_LEN && n < out_len - 1; i++) {
        u8 c = src[i];

        if (c == 0x00) {
            break;
        }

        int lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF);
        if (lead && i + 1 < HDR_TITLE_LEN) {
            out[n++] = sjis_wide_char((u16)((c << 8) | src[i + 1]));
            i++;
        } else if (c >= 0x20 && c <= 0x7E) {
            out[n++] = (char)c;
        }
    }

    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

/* ----------------------------------------------------------------- parsing */

int ps1icon_parse(const void *buf, int len, ps1icon_model *out)
{
    const u8 *p = (const u8 *)buf;

    out->frames = 0;
    out->title[0] = '\0';

    if (len < PS1ICON_HEADER_SIZE || p[0] != 'S' || p[1] != 'C') {
        return 0;
    }

    parse_title(p + HDR_TITLE, out->title, PS1ICON_TITLE_MAX);

    int frames = p[HDR_FLAG] - 0x10;
    if (frames < 1 || frames > PS1ICON_MAX_FRAMES) {
        return 0; /* not an icon flag: save data, or a PocketStation-only entry */
    }
    out->frames = frames;

    /* CLUT: 16 little-endian X1B5G5R5 entries. The channel order already
       matches the GS's CT16, so an entry only needs its alpha bit forced on;
       an all-zero entry is the format's transparent colour. */
    u16 clut[16];
    for (int i = 0; i < 16; i++) {
        u16 e = (u16)(p[HDR_CLUT + i * 2] | (p[HDR_CLUT + i * 2 + 1] << 8));
        clut[i] = (e == 0) ? BODY_TEXEL : (u16)((e & 0x7FFF) | 0x8000);
    }

    memset(out->tex, 0, sizeof(out->tex));

    /* Every frame becomes a 16x16 tile in one texture, laid out left to right
       along its top edge; ps1icon_draw picks a tile by UV offset. */
    for (int f = 0; f < frames; f++) {
        const u8 *bm = p + HDR_BITMAP + f * BITMAP_BYTES;
        u16 *tile = out->tex + f * PS1ICON_DIM;

        for (int y = 0; y < PS1ICON_DIM; y++) {
            for (int x = 0; x < PS1ICON_DIM; x += 2) {
                u8 b = bm[y * (PS1ICON_DIM / 2) + x / 2];

                tile[y * TEX_DIM + x] = clut[b & 0x0F];
                tile[y * TEX_DIM + x + 1] = clut[b >> 4];
            }
        }
    }

    return 1;
}

/* ---------------------------------------------------------------- drawing */

void ps1icon_upload(const ps1icon_model *m, int slot)
{
    /* 16x16 artwork stretched over a tile has to be sampled point-wise; the
       linear filter the 3D icons use would smear it into a blur. */
    gs_icon_tex_filter(slot, 1);
    gs_icon_tex_upload(slot, m->tex);
}

/* Projects the eight cube corners for a given yaw, and shades the six faces.
   Screen positions go into sx/sy/sz indexed by the face tables' corner sign
   triples, so they are computed per face rather than per shared corner - a
   cube is small enough that this costs nothing and keeps the tables readable. */
static void face_geometry(int cx, int cy, int size, float yaw, int face,
                          float sx[4], float sy[4], int sz[4], u32 *color)
{
    float h = size * CUBE_HALF_SCALE;
    float ca = cosf(yaw), sa = sinf(yaw);
    float ct = cosf(CUBE_TILT), st = sinf(CUBE_TILT);

    for (int i = 0; i < 4; i++) {
        float x = face_corner[face][i][0] * h;
        float y = face_corner[face][i][1] * h;
        float z = face_corner[face][i][2] * h;

        /* Spin about Y, then a fixed X tilt; orthographic projection. */
        float rx = x * ca + z * sa;
        float rz = -x * sa + z * ca;
        float ry = y * ct - rz * st;
        rz = y * st + rz * ct;

        sx[i] = cx + rx;
        sy[i] = cy + ry;
        sz[i] = GS_DEPTH_MID - (int)(rz * CUBE_DEPTH_SCALE);
    }

    /* Same fixed lamp as the placeholder cube, modulating the texture: 128 is
       the GS's neutral value, i.e. the texel passed through unshaded. */
    float nx = face_normal[face][0] * ca + face_normal[face][2] * sa;
    float nz = -face_normal[face][0] * sa + face_normal[face][2] * ca;
    float ny = face_normal[face][1] * ct - nz * st;
    nz = face_normal[face][1] * st + nz * ct;

    float d = nx * 0.40f + ny * -0.58f + nz * -0.71f;
    if (d < 0.0f) {
        d = 0.0f;
    }
    int lum = (int)(128.0f * (0.45f + 0.55f * d));
    *color = UI_RGB(lum, lum, lum);
}

/* The tile of `frame` in texel coordinates, inset by half a texel so sampling
   never lands on the boundary with the next frame's tile. */
static void frame_uv(int frame, float *u0, float *v0, float *u1, float *v1)
{
    *u0 = (float)(frame * PS1ICON_DIM) + 0.5f;
    *v0 = 0.5f;
    *u1 = (float)(frame * PS1ICON_DIM + PS1ICON_DIM) - 0.5f;
    *v1 = (float)PS1ICON_DIM - 0.5f;
}

/* Which animation frame is showing at time `t`. */
static int frame_at(const ps1icon_model *m, float t)
{
    if (m->frames < 2 || t <= 0.0f) {
        return 0;
    }
    return ((int)(t / FRAME_SECS)) % m->frames;
}

void ps1icon_draw(const ps1icon_model *m, int slot,
                  int cx, int cy, int size, float t, float spin)
{
    float u0, v0, u1, v1;
    frame_uv(frame_at(m, t), &u0, &v0, &u1, &v1);

    /* Per-pixel depth: the cube is convex, so the depth test alone resolves
       which faces are visible - nothing needs culling or sorting. */
    gs_depth_test(1);
    for (int f = 0; f < 6; f++) {
        float sx[4], sy[4];
        int sz[4];
        u32 col;

        face_geometry(cx, cy, size, spin, f, sx, sy, sz, &col);

        gs_icon_tri(slot,
            sx[0], sy[0], sz[0], u0, v0, col,
            sx[1], sy[1], sz[1], u1, v0, col,
            sx[2], sy[2], sz[2], u1, v1, col);
        gs_icon_tri(slot,
            sx[0], sy[0], sz[0], u0, v0, col,
            sx[2], sy[2], sz[2], u1, v1, col,
            sx[3], sy[3], sz[3], u0, v1, col);
    }
    gs_depth_test(0);
}

void ps1icon_prepack(const ps1icon_model *m, int slot, int cx, int cy, int size)
{
    float u0, v0, u1, v1;
    frame_uv(0, &u0, &v0, &u1, &v1);
    (void)m;

    gs_icon_static_begin(slot);
    for (int f = 0; f < 6; f++) {
        float sx[4], sy[4];
        int sz[4];
        u32 col;

        face_geometry(cx, cy, size, CUBE_REST_YAW, f, sx, sy, sz, &col);

        /* Static packets draw unlit (DECAL), so the per-face shading computed
           above is dropped here - a frozen tile shows the flat texture. */
        gs_icon_static_tri(
            sx[0], sy[0], sz[0], u0, v0,
            sx[1], sy[1], sz[1], u1, v0,
            sx[2], sy[2], sz[2], u1, v1);
        gs_icon_static_tri(
            sx[0], sy[0], sz[0], u0, v0,
            sx[2], sy[2], sz[2], u1, v1,
            sx[3], sy[3], sz[3], u0, v1);
    }
    gs_icon_static_end();
}

void ps1icon_bg_colors(u32 out[4])
{
    u32 top = UI_RGB(24, 32, 56), bottom = UI_RGB(8, 10, 20);

    out[0] = top;
    out[1] = top;
    out[2] = bottom;
    out[3] = bottom;
}
