/*
 * PS2 Memory Card Trader
 *
 * GS rendering via gsKit: a proportional bitmap-font atlas (src/font_data.c)
 * and the button icon sprites (src/sprites_data.c), both embedded in the ELF
 * and uploaded to VRAM once at startup. Replaces the old debug.h text
 * console, which cannot render textures - gsKit owns the GS/framebuffer, so
 * this and debug.h are mutually exclusive.
 *
 * Both assets are generated already squeezed by GS_WIDESCREEN_SCALE (see
 * scripts/gen_font_atlas.py and scripts/gen_sprites_data.py) so they blit
 * 1:1 onto physical framebuffer pixels. Drawing them from square sources
 * instead meant a 0.75 downscale that GS_FILTER_NEAREST resolved by dropping
 * every fourth pixel column, which is what made the old 8-px-wide monospace
 * glyphs illegible at native PS2 resolution.
 */

#include <tamtypes.h>
#include <kernel.h>
#include <gsKit.h>
#include <gsInline.h>
#include <stdio.h>
#include <string.h>

#include "gs_render.h"
#include "font_data.h"
#include "sprites_data.h"

static GSGLOBAL *gsGlobal;
static GSTEXTURE font_tex;
static GSTEXTURE sprite_tex[BTN_COUNT];
static GSTEXTURE icon_tex[GS_ICON_SLOTS];

static void upload_texture(GSTEXTURE *tex, unsigned char *data, int w, int h)
{
    tex->Width = w;
    tex->Height = h;
    tex->PSM = GS_PSM_CT32;
    tex->Filter = GS_FILTER_NEAREST; /* glyphs and sprites are authored at the
                                         exact physical pixel size they are
                                         drawn at, so sampling is 1:1 and any
                                         filtering would only soften them */
    tex->Mem = (u32 *)data;
    tex->Clut = NULL;
    tex->VramClut = 0;
    tex->Vram = gsKit_vram_alloc(gsGlobal, gsKit_texture_size(w, h, tex->PSM), GSKIT_ALLOC_USERBUFFER);
    gsKit_texture_upload(gsGlobal, tex);
}

void gs_render_init(void)
{
    printf("gs_render_init: dmaKit_init\n");
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 0);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    printf("gs_render_init: gsKit_init_global\n");
    /* During a page load burst several icons can be mid-pop and drawn live
       in the same frame (~10k textured triangles), far past the default 1 MB
       oneshot render queue - overflow makes gsKit silently drop primitives.
       4 MB gives ample headroom. */
    gsGlobal = gsKit_init_global_custom(4 * 1024 * 1024, GS_RENDER_QUEUE_PER_POOLSIZE);
    printf("gs_render_init: gsGlobal=%p\n", (void *)gsGlobal);

    gsGlobal->Mode = gsKit_check_rom();
    gsGlobal->Width = 640;
    gsGlobal->Height = (gsGlobal->Mode == GS_MODE_PAL) ? 512 : 448;
    gsGlobal->PSM = GS_PSM_CT32;
    gsGlobal->PSMZ = GS_PSMZ_16S;
    /* Enabled for the 3D save icons, whose near-coplanar geometry cannot be
       resolved by sorting whole triangles. 2D drawing keeps the depth test
       off and relies on draw order. */
    gsGlobal->ZBuffering = GS_SETTING_ON;
    gsGlobal->DoubleBuffering = GS_SETTING_ON;
    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    printf("gs_render_init: Mode=%d Width=%d Height=%d\n", gsGlobal->Mode, gsGlobal->Width, gsGlobal->Height);

    printf("gs_render_init: gsKit_init_screen\n");
    gsKit_init_screen(gsGlobal);
    gsKit_mode_switch(gsGlobal, GS_ONESHOT);
    /* ALPHA_BLEND_NORMAL (Cv = (Cs-Cd)*As + Cd, i.e. standard source-over
       using the SOURCE/texture alpha) is defined in gsFontM.h for gsKit's
       own font renderer - more trustworthy than guessing between the
       vaguely-named GS_BLEND_FRONT2BACK/BACK2FRONT presets. */
    /* PABE (the third argument) must stay 0. With PABE=1 the GS blends only
       pixels whose alpha has bit 7 set; since texture alpha lives in the GS
       range 0..0x80, that is true only at exactly 0x80, so antialiased edges
       would be written opaque. */
    gsKit_set_primalpha(gsGlobal, ALPHA_BLEND_NORMAL, 0);
    gsKit_set_test(gsGlobal, GS_ZTEST_OFF);

    printf("gs_render_init: uploading font atlas, Vram before=%u\n", (unsigned)gsGlobal->CurrentPointer);
    upload_texture(&font_tex, font_atlas_data, FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT);
    printf("gs_render_init: font_tex.Vram=%u\n", (unsigned)font_tex.Vram);

    for (int i = 0; i < BTN_COUNT; i++) {
        upload_texture(&sprite_tex[i], sprite_table[i].data,
                       sprite_table[i].w, sprite_table[i].h);
        printf("gs_render_init: sprite %d Vram=%u\n", i, (unsigned)sprite_tex[i].Vram);
    }

    /* VRAM for the live 3D icon pool, one slot per grid tile. The icons'
       native format is RGBA5551 (CT16), 32 KB per slot. */
    for (int i = 0; i < GS_ICON_SLOTS; i++) {
        icon_tex[i].Width = GS_ICON_TEX_DIM;
        icon_tex[i].Height = GS_ICON_TEX_DIM;
        icon_tex[i].PSM = GS_PSM_CT16;
        icon_tex[i].Filter = GS_FILTER_LINEAR;
        icon_tex[i].Mem = NULL;
        icon_tex[i].Clut = NULL;
        icon_tex[i].VramClut = 0;
        icon_tex[i].Vram = gsKit_vram_alloc(gsGlobal,
            gsKit_texture_size(GS_ICON_TEX_DIM, GS_ICON_TEX_DIM, GS_PSM_CT16), GSKIT_ALLOC_USERBUFFER);
    }

    printf("gs_render_init: done\n");
}

int gs_screen_height(void)
{
    return gsGlobal->Height;
}

int gs_line_h(int font)
{
    return font_line_h[font];
}

/* Packs a colour the way the GS wants it in a gouraud vertex: R/G/B in bits
   0-23 and alpha in bits 24-31. Alpha must be 0x80 (fully opaque in the GS
   0..0x80 range) or the background would blend with, instead of replace,
   the previous frame. */
#define GS_BG_RGB(r, g, b) \
    ((u64)((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | (0x80u << 24)))

/* App background: one full-screen four-corner gouraud gradient. */
#define GS_BG_UL GS_BG_RGB(121, 45, 253)
#define GS_BG_UR GS_BG_RGB(68, 28, 135)
#define GS_BG_LL GS_BG_RGB(34, 149, 195)
#define GS_BG_LR GS_BG_RGB(21, 88, 115)

void gs_draw_background(void)
{
    float x1 = (float)gsGlobal->Width;
    float y1 = (float)gsGlobal->Height;

    /* The gradient replaces the usual gsKit_clear: it covers every pixel of
       the framebuffer opaquely and, with the depth test off, still writes
       depth, so it doubles as the depth-buffer clear the 3D icon relies on.
       Drawn in raw framebuffer coordinates rather than through ws_x() - the
       widescreen compression applies to content, while the background has to
       reach the physical screen edges. */
    gsKit_prim_triangle_gouraud_3d(gsGlobal,
        0.0f, 0.0f, 0, x1, 0.0f, 0, 0.0f, y1, 0, GS_BG_UL, GS_BG_UR, GS_BG_LL);
    gsKit_prim_triangle_gouraud_3d(gsGlobal,
        x1, 0.0f, 0, x1, y1, 0, 0.0f, y1, 0, GS_BG_UR, GS_BG_LR, GS_BG_LL);
}

void gs_render_begin_frame(void)
{
    gs_draw_background();
}

/* How long gs_queue_exec waits for a lost FINISH before giving up on it, in
   spins of an uncached GS register read - a few tens of milliseconds, i.e.
   comfortably longer than the heaviest frame this app draws. */
#define GS_FINISH_SPINS 1000000

/* Every chain gsKit sends ends with a write to the GS FINISH register, and
   the next gsKit_queue_exec waits for the GS to raise the matching CSR flag
   before it reuses the queue. That flag sometimes never arrives - it was
   reproducible while a page of save icons loads, which is when frames carry
   the most geometry - and gsKit's wait is an unbounded spin, so the app
   freezes for good with the last frame still on screen.

   So the flag is polled here first, with a bound. If it does arrive (the
   normal case, usually immediately) gsKit's own wait then passes straight
   through. If it does not, FirstFrame tells gsKit to skip that wait for this
   one chain and drawing carries on: the frame that lost its FINISH is the
   only casualty. Nothing is left unsafe by skipping it - gsKit still waits
   for the DMA channel to go idle before it touches the queue again, and the
   queue is double buffered. */
static void gs_queue_exec(void)
{
    volatile u64 *csr = (volatile u64 *)0x12001000;
    int spins = GS_FINISH_SPINS;

    while (!(*csr & 2) && --spins > 0) { }
    gsGlobal->FirstFrame = (*csr & 2) ? 0 : 1;

    gsKit_queue_exec(gsGlobal);
}

/* ------------------------------------------- prepacked static icon packets */

static float ws_x_f(float x);

/* One PACKED-mode GIF packet per icon slot, built once and DMA'd every frame.
   Layout: giftag + 5 A+D setup regs (TEST zon, TEX0, TEX1, CLAMP), then one
   giftag whose PRIM field kicks flat textured triangles with two packed regs
   (UV, XYZ2) per vertex, then a trailing giftag + A+D restoring TEST. */

/* Vertex capacity of one packet; must cover a whole icon (3072 verts).
   qwords per packet: 1+5 setup, 1 prim tag, 2 per vertex, 1+1 trailer. */
#define PREPACK_MAX_VERTS 3072

static u64 prepack_buf[GS_ICON_SLOTS][7 + 2 * PREPACK_MAX_VERTS + 2][2]
    __attribute__((aligned(64)));
static int prepack_verts[GS_ICON_SLOTS]; /* recorded vertices, 0 = no packet */
static int prepack_on[GS_ICON_SLOTS];
static int prepack_cur = -1;

/* GIF packed-mode register descriptors. */
#define GIF_REG_UV 0x3
#define GIF_REG_XYZ2 0x5
#define GIF_REG_AD 0xE

/* PRIM: triangle, flat, textured, UV coordinates, no blending. */
#define PREPACK_PRIM (GS_PRIM_PRIM_TRIANGLE | (1 << 4) | (1 << 8))

#define GIFTAG_LO(nloop, eop, pre, prim, nreg) \
    ((u64)(nloop) | ((u64)(eop) << 15) | ((u64)(pre) << 46) | \
     ((u64)(prim) << 47) | ((u64)(nreg) << 60))

/* TEST register: keep alpha test off, depth test GEQUAL / ALWAYS. */
#define PREPACK_TEST_ON GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 2)
#define PREPACK_TEST_OFF GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 1)

void gs_icon_static_begin(int slot)
{
    if (slot < 0 || slot >= GS_ICON_SLOTS) {
        prepack_cur = -1;
        return;
    }

    prepack_cur = slot;
    prepack_verts[slot] = 0;
    prepack_on[slot] = 0;

    u64 (*q)[2] = prepack_buf[slot];

    /* Setup: 5 A+D writes. TFX DECAL shows the texture unlit exactly as
       stored; CLAMP left at REPEAT to match the parser's tile rebasing. */
    q[0][0] = GIFTAG_LO(5, 0, 0, 0, 1);
    q[0][1] = GIF_REG_AD;
    q[1][0] = PREPACK_TEST_ON;
    q[1][1] = GS_TEST_1;
    q[2][0] = GS_SETREG_TEX0(icon_tex[slot].Vram / 256, 2, GS_PSM_CT16,
                             7, 7, 1, 1 /* TFX DECAL */, 0, 0, 0, 0, 0);
    q[2][1] = GS_TEX0_1;
    /* MMAG/MMIN follow the slot's own filter, so a prepacked PS1 icon stays
       point-sampled exactly like its live counterpart. */
    int linear = (icon_tex[slot].Filter != GS_FILTER_NEAREST);
    q[3][0] = GS_SETREG_TEX1(0, 0, linear, linear, 0, 0, 0);
    q[3][1] = GS_TEX1_1;
    q[4][0] = GS_SETREG_CLAMP(0, 0, 0, 0, 0, 0);
    q[4][1] = GS_CLAMP_1;
    q[5][0] = GS_SETREG_PRMODECONT(1);
    q[5][1] = GS_PRMODECONT;
    /* q[6] is the vertex giftag, patched with the final count in _end. */
}

void gs_icon_static_tri(float x1, float y1, int z1, float u1, float v1,
                        float x2, float y2, int z2, float u2, float v2,
                        float x3, float y3, int z3, float u3, float v3)
{
    if (prepack_cur < 0) {
        return;
    }

    int slot = prepack_cur;
    int n = prepack_verts[slot];
    if (n + 3 > PREPACK_MAX_VERTS) {
        return;
    }

    const float xs[3] = { x1, x2, x3 }, ys[3] = { y1, y2, y3 };
    const float us[3] = { u1, u2, u3 }, vs[3] = { v1, v2, v3 };
    const int zs[3] = { z1, z2, z3 };

    /* PACKED mode: the register descriptors live in the giftag's REGS list,
       so each data qword is pure payload. UV: U 0..13, V 32..45, both 12.4.
       XYZ2: X 0..15, Y 32..47 (12.4), Z in bits 64..95 = low half of the
       second u64, ADC (bit 111) zero so every third vertex kicks a tri. */
    u64 (*q)[2] = &prepack_buf[slot][7 + 2 * n];
    for (int i = 0; i < 3; i++) {
        int iu = gsKit_float_to_int_u(&icon_tex[slot], us[i]);
        int iv = gsKit_float_to_int_v(&icon_tex[slot], vs[i]);
        int ix = gsKit_float_to_int_x(gsGlobal, ws_x_f(xs[i]));
        int iy = gsKit_float_to_int_y(gsGlobal, ys[i]);
        u32 iz = (u32)(zs[i] < 0 ? 0 : zs[i]);

        q[0][0] = (u64)(u16)iu | ((u64)(u16)iv << 32);
        q[0][1] = 0;
        q[1][0] = (u64)(u16)ix | ((u64)(u16)iy << 32);
        q[1][1] = (u64)iz;
        q += 2;
    }

    prepack_verts[slot] = n + 3;
}

void gs_icon_static_end(void)
{
    if (prepack_cur < 0) {
        return;
    }

    int slot = prepack_cur;
    int n = prepack_verts[slot];
    u64 (*q)[2] = prepack_buf[slot];

    /* Vertex giftag: PRIM in the tag (PRE=1), two packed regs per vertex. */
    q[6][0] = GIFTAG_LO(n, 0, 1, PREPACK_PRIM, 2);
    q[6][1] = ((u64)GIF_REG_UV) | ((u64)GIF_REG_XYZ2 << 4);

    /* Trailer: restore the depth test to pass-always for the 2D drawing. */
    int t = 7 + 2 * n;
    q[t][0] = GIFTAG_LO(1, 1, 0, 0, 1);
    q[t][1] = GIF_REG_AD;
    q[t + 1][0] = PREPACK_TEST_OFF;
    q[t + 1][1] = GS_TEST_1;

    /* The DMA reads physical memory, not the cache. */
    SyncDCache(prepack_buf[slot], (u8 *)prepack_buf[slot] + sizeof(prepack_buf[slot]));
    prepack_cur = -1;
}

void gs_icon_static_enable(int slot, int on)
{
    if (slot >= 0 && slot < GS_ICON_SLOTS) {
        prepack_on[slot] = on && prepack_verts[slot] > 0;
    }
}

void gs_icon_static_clear(void)
{
    for (int i = 0; i < GS_ICON_SLOTS; i++) {
        prepack_on[i] = 0;
        prepack_verts[i] = 0;
    }
}

/* Copies every enabled packet into the frame's oneshot queue as one big
   A+D-typed object - the exact allocation path every regular gsKit prim
   uses, so nothing is assumed about the library's DMA chain layout (its
   raw-DMA-tag injection helper corrupts the queue in this prebuilt gsKit).
   The packet already begins with its own giftag, which fills the giftag
   qword gsKit_heap_alloc reserves for every A+D object. ~1 ms of memcpy
   for a full page replaces ~15 ms of per-triangle prim calls. */
static void prepack_inject(void)
{
    for (int i = 0; i < GS_ICON_SLOTS; i++) {
        if (!prepack_on[i]) {
            continue;
        }

        /* 1+5 setup, 1 vertex giftag, 2 per vertex, 1+1 trailer qwords. */
        int qwc = 9 + 2 * prepack_verts[i];
        u64 *p = (u64 *)gsKit_heap_alloc(gsGlobal, qwc - 1, (qwc - 1) * 16, GIF_AD);

        memcpy(p, prepack_buf[i], (size_t)qwc * 16);
    }
}

static void prepack_inject(void);

void gs_render_end_frame(void)
{
    prepack_inject();
    gs_queue_exec();
    gsKit_sync_flip(gsGlobal);
}

void gs_render_vsync_wait(void)
{
    gsKit_vsync_wait();
}

/* The app is shown on a 16:9 widescreen display, which horizontally stretches
   the 4:3 640x448 framebuffer by 4/3. Pre-compress every horizontal extent to
   3/4 about the screen centre so that stretch restores correct proportions
   (round circles, unstretched glyphs) with the content staying centred.
   Applied to both edges of every primitive, so it scales position AND width. */
#define GS_SCREEN_CX 320.0f
#define GS_WIDESCREEN_SCALE 0.75f

static float ws_x_f(float x)
{
    return GS_SCREEN_CX + (x - GS_SCREEN_CX) * GS_WIDESCREEN_SCALE;
}

static float ws_x(int x)
{
    return ws_x_f((float)x);
}

void gs_fill_rect(int x, int y, int w, int h, u32 color)
{
    gsKit_prim_sprite(gsGlobal, ws_x(x), (float)y,
        ws_x(x + w), (float)(y + h), 0, (u64)color);
}

void gs_fill_gradient(int x, int y, int w, int h, u32 ul, u32 ur, u32 ll, u32 lr)
{
    float x0 = ws_x(x), x1 = ws_x(x + w);
    float y0 = (float)y, y1 = (float)(y + h);

    gsKit_prim_triangle_gouraud_3d(gsGlobal,
        x0, y0, 0, x1, y0, 0, x0, y1, 0, (u64)ul, (u64)ur, (u64)ll);
    gsKit_prim_triangle_gouraud_3d(gsGlobal,
        x1, y0, 0, x1, y1, 0, x0, y1, 0, (u64)ur, (u64)lr, (u64)ll);
}

void gs_print_at(int col, int row, u32 color, const char *text)
{
    gs_print_px(col * GS_CHAR_W, row * GS_CHAR_H, color, text);
}

void gs_print_px(int x, int y, u32 color, const char *text)
{
    gs_print_font(FONT_BIG, x, y, color, text);
}

void gs_print_font(int font, int x, int y, u32 color, const char *text)
{
    const font_glyph *tab = font_glyphs[font];
    u64 tint = (u64)color;

    /* The pen runs in physical pixels from the squeezed origin, snapped to a
       whole pixel: the atlas is already squeezed, so advancing it by the
       logical-space scale would both double-compress the spacing and land
       glyphs on fractional pixels, which is exactly what NEAREST cannot
       represent. */
    int px = (int)(ws_x(x) + 0.5f);

    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) {
            continue; /* '\n' and anything not in the atlas */
        }

        const font_glyph *g = &tab[c - FONT_FIRST_CHAR];

        if (g->w > 0) {
            float gx = (float)(px + g->xoff);
            float gy = (float)(y + g->yoff);

            gsKit_prim_sprite_texture_3d(gsGlobal, &font_tex,
                gx, gy, 0, (float)g->u, (float)g->v,
                gx + (float)g->w, gy + (float)g->h, 0,
                (float)(g->u + g->w), (float)(g->v + g->h),
                tint);
        }

        px += g->adv;
    }
}

int gs_text_width_font(int font, const char *text)
{
    const font_glyph *tab = font_glyphs[font];
    int px = 0;

    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (c >= FONT_FIRST_CHAR && c <= FONT_LAST_CHAR) {
            px += tab[c - FONT_FIRST_CHAR].adv;
        }
    }

    /* Advances are physical; callers lay out in logical space, so undo the
       squeeze (round to nearest). */
    return (px * 4 + 1) / 3;
}

int gs_text_width(const char *text)
{
    return gs_text_width_font(FONT_BIG, text);
}

void gs_sprite_at(sprite_id id, int col, int row, int size)
{
    gs_sprite_px(id, col * GS_CHAR_W, row * GS_CHAR_H, size);
}

void gs_sprite_px(sprite_id id, int x, int y, int size)
{
    const sprite_desc *s = &sprite_table[id];

    /* size sets the width; the height follows from the authored aspect so a
       short sprite (the shoulder buttons) is not stretched square, and the
       leftover space is split above and below to keep it centred in the
       size x size box the caller reserved. */
    int w = size;
    int h = size * s->logical_h / s->logical_w;

    y += (size - h) / 2;

    gsKit_prim_sprite_texture_3d(gsGlobal, &sprite_tex[id],
        ws_x(x), (float)y, 0, 0.0f, 0.0f,
        ws_x(x + w), (float)(y + h), 0, (float)s->w, (float)s->h,
        GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
}

void gs_icon_tex_upload(int slot, const void *tex_rgba5551)
{
    if (slot < 0 || slot >= GS_ICON_SLOTS) {
        return;
    }
    icon_tex[slot].Mem = (u32 *)tex_rgba5551;
    gsKit_texture_upload(gsGlobal, &icon_tex[slot]);
}

void gs_icon_tex_filter(int slot, int nearest)
{
    if (slot < 0 || slot >= GS_ICON_SLOTS) {
        return;
    }
    icon_tex[slot].Filter = nearest ? GS_FILTER_NEAREST : GS_FILTER_LINEAR;
}

void gs_icon_tri(int slot,
                 float x1, float y1, int z1, float u1, float v1, u32 c1,
                 float x2, float y2, int z2, float u2, float v2, u32 c2,
                 float x3, float y3, int z3, float u3, float v3, u32 c3)
{
    if (slot < 0 || slot >= GS_ICON_SLOTS) {
        return;
    }
    gsKit_prim_triangle_goraud_texture_3d(gsGlobal, &icon_tex[slot],
        ws_x_f(x1), y1, z1, u1, v1,
        ws_x_f(x2), y2, z2, u2, v2,
        ws_x_f(x3), y3, z3, u3, v3,
        (u64)c1, (u64)c2, (u64)c3);
}

void gs_flat_tri(float x1, float y1, int z1, u32 c1,
                 float x2, float y2, int z2, u32 c2,
                 float x3, float y3, int z3, u32 c3)
{
    gsKit_prim_triangle_gouraud_3d(gsGlobal,
        ws_x_f(x1), y1, z1,
        ws_x_f(x2), y2, z2,
        ws_x_f(x3), y3, z3,
        (u64)c1, (u64)c2, (u64)c3);
}

void gs_depth_test(int enable)
{
    gsKit_set_test(gsGlobal, enable ? GS_ZTEST_ON : GS_ZTEST_OFF);
}

