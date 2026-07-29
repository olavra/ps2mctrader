#ifndef GS_RENDER_H
#define GS_RENDER_H

#include <tamtypes.h>

#include "font_data.h"
/* sprite_id and the per-sprite size table are generated from the PNGs, see
   scripts/gen_sprites_data.py. */
#include "sprites_data.h"

/* Coarse character cell used only by the throwaway render test's col/row
   helpers. The app itself lays text out in logical pixels, since the font is
   proportional - see gs_print_px and gs_text_width. */
#define GS_CHAR_W 8
#define GS_CHAR_H 16

/* Initializes the GS, uploads the font atlas and all icon sprites to
   VRAM. Must be called once before any other gs_render function. */
void gs_render_init(void);

/* Framebuffer height in pixels. Mode-dependent: 448 on NTSC, 512 on PAL.
   Layout that has to sit at the bottom of the screen must derive its y from
   this rather than assume NTSC. Valid only after gs_render_init. */
int gs_screen_height(void);

/* Clears the screen. Call once at the start of building a frame. */
void gs_render_begin_frame(void);

/* Draws the app background gradient; with the depth test off it also writes
   depth 0 everywhere, doubling as the depth-buffer clear. Called implicitly
   by gs_render_begin_frame; exposed for retained draw lists that record
   their own background. */
void gs_draw_background(void);

/* Retained icon drawing. A frozen (unselected) grid icon costs thousands of
   gsKit calls per frame if re-emitted; instead its triangles are recorded
   once into a per-slot prepacked GIF packet (gs_icon_static_begin/tri/end),
   and every enabled packet is sent to the GS by DMA at the end of each frame
   - near-zero EE cost per frame. Packets draw the slot's texture unlit
   (DECAL) with the depth test on, after the oneshot frame content.
   (gsKit's own persistent-queue mechanism is not used: the prebuilt library
   deadlocks in its FINISH protocol on the second replay.)

   Record while the slot's model is loaded, enable it when the tile should
   show, disable while the tile is drawn live (selected/animating), and clear
   everything when leaving the grid screen - enabled packets are sent on
   EVERY frame, whatever else is on screen. */
void gs_icon_static_begin(int slot);
void gs_icon_static_tri(float x1, float y1, int z1, float u1, float v1,
                        float x2, float y2, int z2, float u2, float v2,
                        float x3, float y3, int z3, float u3, float v3);
void gs_icon_static_end(void);
void gs_icon_static_enable(int slot, int on);
void gs_icon_static_clear(void);

/* Flushes the draw queue and flips buffers. Call once after all drawing
   for the frame is done. */
void gs_render_end_frame(void);

/* Blocks until the next vertical blank (polls the GS CSR - no interrupt
   handler, so it does not conflict with anything). Use it to pace the main
   loop at the display refresh rate, replacing the old raw-VBLANK vsync. */
void gs_render_vsync_wait(void);

/* Fills a solid-color rectangle in raw pixel coordinates. Alpha in the
   color (bits 24-31, GS range 0..0x80) is honored by the blend. */
void gs_fill_rect(int x, int y, int w, int h, u32 color);

/* Fills a rectangle with a four-corner gouraud gradient - the PS2 browser
   builds a save's backdrop this way from the four corner colours in
   icon.sys. Corners are upper-left, upper-right, lower-left, lower-right. */
void gs_fill_gradient(int x, int y, int w, int h, u32 ul, u32 ur, u32 ll, u32 lr);

/* Live 3D save-icon rendering. Every save tile is drawn as an actual
   animated model, so there is a pool of icon texture slots in VRAM - one per
   grid tile. gs_icon_tex_upload takes an icon's native 128x128 RGBA5551
   texture and makes it resident in the given slot; gs_icon_tri then draws
   textured triangles with that slot's texture. */
#define GS_ICON_TEX_DIM 128
#define GS_ICON_SLOTS 16

void gs_icon_tex_upload(int slot, const void *tex_rgba5551);

/* Picks how slot's texture is sampled: linear (the default, right for the
   detailed 128x128 textures of PS2 3D icons) or nearest, which a PS1 save's
   16x16 icon needs - linear filtering blurs it away at tile size. Applies to
   both the live and the prepacked draw paths; set it before uploading. */
void gs_icon_tex_filter(int slot, int nearest);

/* Enables/disables the per-pixel depth test. Off is the default and what all
   2D drawing expects (later primitives simply paint over earlier ones);
   turn it on only around the 3D icon. While off, primitives still WRITE
   depth, so the full-screen clear at the start of a frame doubles as the
   depth-buffer clear. */
void gs_depth_test(int enable);

/* Depth values to pass to gs_icon_tri. Larger is nearer, and the frame clear
   leaves the whole screen at 0, so any icon geometry sits in front of it. */
#define GS_DEPTH_MID 16384
#define GS_DEPTH_SCALE 2048.0f

/* Emits one gouraud-shaded textured triangle of slot's icon texture.
   Coordinates are in logical screen pixels, uv in texels (0..127). */
void gs_icon_tri(int slot,
                 float x1, float y1, int z1, float u1, float v1, u32 c1,
                 float x2, float y2, int z2, float u2, float v2, u32 c2,
                 float x3, float y3, int z3, float u3, float v3, u32 c3);

/* Emits one untextured gouraud-shaded triangle in the same coordinate and
   depth space as gs_icon_tri, for placeholder geometry such as the loading
   cube shown before a save's model has been read off the card. */
void gs_flat_tri(float x1, float y1, int z1, u32 c1,
                 float x2, float y2, int z2, u32 c2,
                 float x3, float y3, int z3, u32 c3);

/* Text drawing. The font is proportional and comes in two sizes (FONT_BIG
   for everything read at a glance, FONT_SMALL for dense secondary text such
   as the save titles under the grid tiles - see font_data.h).

   (x, y) is the top-left of the line box in logical pixels; y is also a
   physical pixel row, since only the horizontal axis is squeezed. Embedded
   '\n' characters are skipped rather than starting a new line - callers
   position each line explicitly. Characters outside the atlas (a Shift-JIS
   save title, say) are skipped.

   The atlas stores glyphs already squeezed by GS_WIDESCREEN_SCALE, so each
   one is blitted 1:1 texel to physical pixel and stays sharp; that is why
   the pen advances in physical pixels internally while the API stays in the
   logical 640x448 space every layout is written in. */
void gs_print_font(int font, int x, int y, u32 color, const char *text);
void gs_print_px(int x, int y, u32 color, const char *text); /* FONT_BIG */

/* Width of a string and height of a line, both in logical pixels, for
   centring and for stacking rows. */
int gs_text_width_font(int font, const char *text);
int gs_text_width(const char *text); /* FONT_BIG */
int gs_line_h(int font);

/* Draws one of the button/dpad icon sprites inside a size x size box at
   x, y in logical pixels. The sprite keeps its authored aspect ratio - the
   shoulder buttons are wider than tall - and is centred vertically in the
   box, so hints line up whatever icon they use. SPRITE_NATIVE_SIZE is the
   size that makes it a 1:1 blit; anything else resamples and softens it. */
void gs_sprite_px(sprite_id id, int x, int y, int size);

/* Character-cell wrappers over the two calls above, kept for the throwaway
   render test (src/test_render.c) only. */
void gs_print_at(int col, int row, u32 color, const char *text);
void gs_sprite_at(sprite_id id, int col, int row, int size);

#endif
