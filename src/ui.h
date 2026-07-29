#ifndef UI_H
#define UI_H

#include <tamtypes.h>

#include "gs_render.h"
#include "sprites_data.h"

/* Every coordinate below is in logical pixels of the 640x448 framebuffer,
   before gs_render's widescreen squeeze. The font is proportional, so lines
   are positioned by pixel and their extents come from gs_text_width rather
   than from a character count.

   Safe-area margins keep content clear of TV overscan. The horizontal one is
   small because the widescreen squeeze already insets everything: logical x 0
   lands at physical x 80, an eighth of the picture in from the left. */
#define UI_MARGIN_X 12
#define UI_MARGIN_Y 32

/* Line heights, and the vertical step between rows of text. */
#define UI_LINE gs_line_h(FONT_BIG)
#define UI_LINE_MID gs_line_h(FONT_MID)
#define UI_LINE_SMALL gs_line_h(FONT_SMALL)

/* Hint bar: a button sprite drawn at its native size, a gap before its
   label, and a wider gap before the next hint. Pinned to the bottom of the
   screen from the live framebuffer height, since that is 448 on NTSC but 512
   on PAL. */
#define UI_HINT_ICON SPRITE_NATIVE_SIZE
#define UI_HINT_GAP 6
#define UI_HINT_SPACING 20
#define UI_FOOTER_Y (gs_screen_height() - UI_MARGIN_Y - UI_HINT_ICON)

/* Indent of a list entry past its '>' cursor marker. The marker is drawn on
   its own rather than as a prefix character, so that a selected and an
   unselected entry start at the same x under a proportional font. */
#define UI_CURSOR_INDENT 32

/* Save browser grid: a card's contents are shown as a page of 4x4 icon
   tiles rather than a text list. Geometry is in logical pixels (the 640x448
   framebuffer, before the widescreen squeeze), since tiles centre their icon
   and title instead of snapping to the character cell grid. */
#define UI_GRID_COLS 4
#define UI_GRID_ROWS 4
#define UI_GRID_PAGE (UI_GRID_COLS * UI_GRID_ROWS)

/* Logical framebuffer size, before the widescreen squeeze. */
#define UI_SCREEN_W 640
#define UI_SCREEN_H 448

/* Icon size in the full-screen single-save view. */
#define UI_ICON_VIEW_SIZE 260

/* Delete confirmation: the save's icon sits near the middle of the screen
   with the question and the YES/NO hints underneath it, so the icon itself
   says which save is about to go. */
#define UI_CONFIRM_ICON_SIZE 160
#define UI_CONFIRM_ICON_Y 180
#define UI_CONFIRM_TITLE_Y 282
#define UI_CONFIRM_TEXT_Y 318

/* Preview rotation. The icon idles at UI_SPIN_AUTO; nudging the left stick
   hands control over, after which it keeps spinning under UI_SPIN_FRICTION
   until UI_SPIN_RESUME seconds of stillness ease it back to the idle spin. */
#define UI_SPIN_AUTO -1.6f      /* radians per second */
#define UI_SPIN_MANUAL 9.0f     /* radians per second at full stick */
#define UI_SPIN_FRICTION 5.0f   /* velocity decay per second */
#define UI_SPIN_RESUME 2.0f     /* seconds of no input before idling again */
#define UI_SPIN_BLEND 1.5f      /* how fast it eases back to the idle spin */

/* Tile geometry. A tile is its icon plus one line of FONT_SMALL underneath,
   and the four rows have to end above the hint bar - so the row height, and
   with it the icon, is whatever fits rather than a fixed number: a 512-line
   PAL framebuffer has 64 lines more to spend on them than a 448-line NTSC
   one, and hardcoding the NTSC size would waste all of it. */
#define UI_TILE_W 150
#define UI_GRID_X 20
#define UI_GRID_Y 68
#define UI_TILE_GAP 8 /* clearance between the last row's title and the hints */
#define UI_TILE_H ((UI_FOOTER_Y - UI_GRID_Y - UI_TILE_GAP) / UI_GRID_ROWS)
/* The icon gets whatever the tile has left after two lines of title (a save
   name is wrapped rather than cut off) and 2 px of padding above, between
   and below. */
#define UI_TILE_ICON (UI_TILE_H - 2 * UI_LINE_SMALL - 8)

/* Packs an RGB color (plus full alpha) into the u32 gs_print_at expects,
   matching the PS2 GS's byte-per-channel register layout (R bits 0-7,
   G bits 8-15, B bits 16-23, A bits 24-31; 0x80 = opaque). */
#define UI_RGB(r, g, b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | (0x80u << 24))

#define UI_COLOR_DEFAULT UI_RGB(255, 255, 255)

#endif
