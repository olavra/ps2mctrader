/*
 * PS2 Memory Card Trader
 *
 * Menu UI, drawn with gs_render. Lists the physical memory card slots
 * (mc0, mc1) and lets the user navigate between them with Up/Down and open a
 * card with X, formatting it first if needed. A card's saves are browsed as
 * pages of 4x4 icon tiles, each showing the save's 3D icon and its own title;
 * the selected tile animates. X opens a save full-screen over the backdrop
 * its icon.sys defines, O returns. In the grid, Square copies a save to the
 * clipboard, R1 pastes it into a different type-compatible card, Triangle
 * deletes after confirmation, and O goes back.
 */

#include <tamtypes.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <kernel.h>
#include <libpad.h>
#include <libmc.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mc_driver.h"
#include "pad_driver.h"
#include "gs_render.h"
#include "mc_icon.h"
#include "audio.h"
#include "ui.h"

#define SLOT_COUNT 2

/* The main list holds the two card slots plus a trailing Exit entry, so the
   cursor runs over SLOT_COUNT + 1 rows and LIST_EXIT is the index of that
   last one. */
#define LIST_ITEMS (SLOT_COUNT + 1)
#define LIST_EXIT SLOT_COUNT

/* Scan cadence in vsync ticks (~60 Hz NTSC / ~50 Hz PAL), so this is a
   meaningful real-time interval rather than an arbitrary loop count. */
#define SCAN_INTERVAL 30

/* Consecutive differing scans required before a change is trusted. The
   SIO2 bus is shared between both ports, so a hot-swap on one port can
   make the OTHER port's mcGetInfo return a single bogus-but-clean reading
   (not just a comm error) - so every state change, not just error cases,
   needs confirmation before it reaches the UI. Readings don't need to
   match each other exactly, only to keep differing from the currently
   shown state; whatever the reading eventually settles on gets committed
   once, rather than requiring several transitional readings during a swap
   to coincidentally agree with each other. */
#define CONFIRM_TICKS 2

#define COLOR_PS2 UI_RGB(121, 50, 168)
#define COLOR_PS1 UI_RGB(168, 111, 50)

typedef enum {
    STATE_LIST,
    STATE_CONFIRM_FORMAT,
    STATE_CONTENTS,
    STATE_ICON_VIEW,
    STATE_CONFIRM_DELETE,
    STATE_CONFIRM_EXIT
} app_state;

typedef struct {
    int valid;
    int port;
    char name[32];
    int is_dir;
} clipboard_t;

/* Every visible save tile is a live animated 3D model in the mc_icon slot
   pool; a tile's slot is simply its cell index on the current page, so
   flipping pages naturally evicts the previous page's models (revisiting a
   page is a cache hit and repopulates instantly). Models are loaded one per
   frame by icons_tick rather than all at once, so opening a card never
   freezes the UI: tiles whose model is still pending show a placeholder
   cube, and each icon pops in (scale 0 to 1) as its load completes.
   entry_icon_failed marks saves whose icon can't be loaded so they aren't
   retried every frame; those keep the cube for good, both in the grid and
   in the full-screen view. */
static u8 entry_icon_failed[MC_MAX_ENTRIES];
static char entry_title[MC_MAX_ENTRIES][PS2ICON_TITLE_MAX];
static float slot_pop_time[GS_ICON_SLOTS]; /* anim_time when the slot's model
                                              finished loading */

/* Seconds the pop-in (scale 0 to 1) animation takes. */
#define POP_DURATION 0.25f

/* How many tiles may be emitted through gsKit in one frame. Everything else
   on the page is a prepacked static packet, which the GS replays by DMA for
   free; a live tile costs ~4k quadwords of GIF stream and thousands of prim
   calls. Models load one per frame and each pop lasts POP_DURATION, so
   without a cap a whole page's worth of pops overlap: opening a card used to
   put 14+ icons in one frame (~55k quadwords), and past roughly that point
   the GS stops raising its FINISH flag, which hangs gsKit_queue_exec's wait
   for it forever - the app freezes with the last frame on screen. Tiles over
   the budget skip the rest of their pop and freeze immediately, so the page
   still fills at the same rate. The selected tile always draws live and is
   counted in the budget. */
#define GRID_LIVE_ICONS 3

/* Bitmask of cells whose frozen icon is currently prepacked and enabled as
   a static packet (see gs_icon_static_* in gs_render.h). Those tiles cost
   the EE nothing per frame; only the selected icon, loading cubes and icons
   mid-pop are emitted through gsKit each frame. */
static u16 grid_static_cells;

/* Seconds since the contents view was opened, driving the selected icon's
   animation. Advanced once per vsync while that view is on screen. */
static float anim_time;

/* The selected icon's animation and spin are measured from the moment the
   cursor landed on it, not from anim_time directly: selecting a tile always
   restarts its rotation from the front instead of inheriting the phase the
   previously selected icon had reached. sel_anim_cell is the entry the base
   belongs to; -1 forces a restart on the next frame. */
static float sel_anim_base;
static int sel_anim_cell = -1;

/* Full-screen preview state: current rotation and its angular velocity, how
   long the stick has been idle, and whether the icon.sys lighting is applied. */
static float spin_angle;
static float spin_vel;
static float spin_idle;
static int preview_lights = 1;

/* Advances the preview's rotation for one frame. The stick drives the
   velocity directly while it is held; once released the icon coasts to a stop
   and, after UI_SPIN_RESUME seconds, eases back into the idle spin. */
static void update_spin(float dt)
{
    float sx, sy;
    pad_driver_left_stick(&sx, &sy);

    if (sx != 0.0f) {
        /* Squared response: fine control near the centre, full speed at the
           edge. Negated so pushing right spins the icon to the right. */
        float mag = sx * sx;
        spin_vel = (sx > 0.0f ? -mag : mag) * UI_SPIN_MANUAL;
        spin_idle = 0.0f;
    } else {
        spin_idle += dt;

        if (spin_idle < UI_SPIN_RESUME) {
            spin_vel -= spin_vel * UI_SPIN_FRICTION * dt;
        } else {
            spin_vel += (UI_SPIN_AUTO - spin_vel) * UI_SPIN_BLEND * dt;
        }
    }

    spin_angle += spin_vel * dt;
}

/* Resets the browsing state for a (re)listed card: every model cache entry
   is dropped and titles fall back to the raw directory/file name until each
   save's icon.sys is read by the incremental loader. Instant - no card I/O. */
static void contents_reset(const mc_entry *entries, int entry_count)
{
    mc_icon_reset();
    gs_icon_static_clear();
    grid_static_cells = 0;
    sel_anim_cell = -1; /* the cursor's tile is a different save now */

    for (int i = 0; i < entry_count; i++) {
        entry_icon_failed[i] = 0;
        strncpy(entry_title[i], entries[i].name, PS2ICON_TITLE_MAX - 1);
        entry_title[i][PS2ICON_TITLE_MAX - 1] = '\0';
    }
}

/* Whether an entry is a save that can carry an icon. On a PS2 card saves are
   directories (loose files in the root are settings blobs, not saves); on a
   PS1 card every entry is a file and every one of them is a save. */
static int entry_has_icon(const mc_entry *entry, int ps1)
{
    return ps1 ? !entry->is_dir : entry->is_dir;
}

/* Loads at most one pending icon of the current page per call, so the cost
   of reading a save's icon off the card is spread one save per frame instead
   of freezing the screen for the whole page. The selected tile loads first;
   the rest fill in left to right. Titles switch from the directory name to
   the icon.sys title as each save is read. */
static void icons_tick(int port, int ps1, const mc_entry *entries, int entry_count,
                       int file_cursor)
{
    int first = (entry_count > 0) ? (file_cursor / UI_GRID_PAGE) * UI_GRID_PAGE : 0;

    for (int pass = 0; pass < 2; pass++) {
        for (int i = first; i < entry_count && i < first + UI_GRID_PAGE; i++) {
            if (pass == 0 && i != file_cursor) {
                continue; /* first pass: the selected tile only */
            }

            int cell = i - first;
            if (!entry_has_icon(&entries[i], ps1) || entry_icon_failed[i] ||
                mc_icon_slot_matches(cell, port, entries[i].name)) {
                continue;
            }

            if (mc_icon_slot_load(cell, port, entries[i].name, ps1,
                                  entry_title[i], PS2ICON_TITLE_MAX)) {
                slot_pop_time[cell] = anim_time;
            } else {
                entry_icon_failed[i] = 1;
            }
            if (entry_title[i][0] == '\0') {
                strncpy(entry_title[i], entries[i].name, PS2ICON_TITLE_MAX - 1);
                entry_title[i][PS2ICON_TITLE_MAX - 1] = '\0';
            }
            return; /* one load per frame */
        }
    }
}

/* Scale factor for an icon that finished loading `dt` seconds ago: an
   ease-out-back curve from 0 to 1 with a slight overshoot, so models pop
   into the grid instead of blinking in. */
static float pop_scale(float dt)
{
    if (dt <= 0.0f) {
        return 0.0f;
    }

    float x = dt / 0.25f;
    if (x >= 1.0f) {
        return 1.0f;
    }

    float u = x - 1.0f;
    return 1.0f + (2.70158f * u + 1.70158f) * u * u;
}

/* Placeholder for a tile whose model is still being read off the card: a
   flat-shaded blue cube in the same projection as the icons (orthographic,
   spun about Y, with a fixed forward tilt so its top face reads as 3D). */
static void draw_cube(int cx, int cy, int size, float ang)
{
    /* Corner index bits: 0 = +X/-X, 1 = +Y/-Y, 2 = +Z/-Z. */
    static const u8 face[6][4] = {
        { 1, 3, 7, 5 }, { 0, 4, 6, 2 },   /* +X, -X */
        { 2, 6, 7, 3 }, { 0, 1, 5, 4 },   /* +Y, -Y */
        { 4, 5, 7, 6 }, { 0, 2, 3, 1 },   /* +Z, -Z */
    };
    static const float face_n[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 },
        { 0, 1, 0 }, { 0, -1, 0 },
        { 0, 0, 1 }, { 0, 0, -1 },
    };

    float h = size * 0.30f; /* half-extent: rotating silhouette stays in tile */
    float ca = cosf(ang), sa = sinf(ang);
    float ct = cosf(0.42f), st = sinf(0.42f);

    float sx[8], sy[8];
    int sz[8];
    for (int i = 0; i < 8; i++) {
        float x = (i & 1) ? h : -h;
        float y = (i & 2) ? h : -h;
        float z = (i & 4) ? h : -h;

        /* Spin about Y, then a fixed X tilt; orthographic projection. */
        float rx = x * ca + z * sa;
        float rz = -x * sa + z * ca;
        float ry = y * ct - rz * st;
        rz = y * st + rz * ct;

        sx[i] = cx + rx;
        sy[i] = cy + ry;
        sz[i] = GS_DEPTH_MID - (int)(rz * 16.0f);
    }

    gs_depth_test(1);
    for (int f = 0; f < 6; f++) {
        /* Rotate the face normal the same way and light it with a fixed
           directional lamp; convexity plus the depth test handles hidden
           faces without any culling. */
        float nx = face_n[f][0] * ca + face_n[f][2] * sa;
        float nz = -face_n[f][0] * sa + face_n[f][2] * ca;
        float ny = face_n[f][1] * ct - nz * st;
        nz = face_n[f][1] * st + nz * ct;

        float d = nx * 0.40f + ny * -0.58f + nz * -0.71f;
        if (d < 0.0f) {
            d = 0.0f;
        }
        float lum = 0.45f + 0.55f * d;
        u32 col = UI_RGB((int)(70 * lum), (int)(130 * lum), (int)(230 * lum));

        const u8 *q = face[f];
        gs_flat_tri(sx[q[0]], sy[q[0]], sz[q[0]], col,
                    sx[q[1]], sy[q[1]], sz[q[1]], col,
                    sx[q[2]], sy[q[2]], sz[q[2]], col);
        gs_flat_tri(sx[q[0]], sy[q[0]], sz[q[0]], col,
                    sx[q[2]], sy[q[2]], sz[q[2]], col,
                    sx[q[3]], sy[q[3]], sz[q[3]], col);
    }
    gs_depth_test(0);
}

static int mc_slot_equal(const mc_slot_info *a, const mc_slot_info *b)
{
    return a->type == b->type && a->present == b->present && a->formatted == b->formatted;
}

/* Polls one port and applies the confirm-streak debounce described above.
   Only commits a change to slots[port] once readings have differed from
   the currently shown state for CONFIRM_TICKS consecutive scans in a row;
   a single differing or failed scan just counts toward that streak without
   touching the visible state yet. A failed scan counts as "differs from
   present" too, since a card that's really gone tends to keep failing.
   Returns 1 if slots[port] changed, so the caller knows to redraw. */
static int poll_slot(int port, mc_slot_info *slots, int *mismatch_streak)
{
    mc_slot_info observed;

    if (mc_driver_scan(port, &observed)) {
        if (mc_slot_equal(&observed, &slots[port])) {
            mismatch_streak[port] = 0;
            return 0;
        }

        mismatch_streak[port]++;
        if (mismatch_streak[port] >= CONFIRM_TICKS) {
            slots[port] = observed;
            mismatch_streak[port] = 0;
            return 1;
        }
    } else if (slots[port].present) {
        mismatch_streak[port]++;
        if (mismatch_streak[port] >= CONFIRM_TICKS) {
            slots[port].type = MC_TYPE_NONE;
            slots[port].present = 0;
            slots[port].formatted = 0;
            mismatch_streak[port] = 0;
            return 1;
        }
    }

    return 0;
}

static u32 slot_label_color(const mc_slot_info *info)
{
    if (info->present && info->formatted) {
        if (info->type == MC_TYPE_PS2) {
            return COLOR_PS2;
        }
        if (mc_type_is_ps1(info->type)) {
            return COLOR_PS1;
        }
    }

    return UI_COLOR_DEFAULT;
}

/* A paste is only offered when the clipboard holds something, we're
   looking at a different card than the one it came from, and that source
   card is still present and of a compatible type (a PS1 save can't go on a
   PS2 filesystem and vice versa). PS1 and PocketStation cards count as the
   same type here: they share a filesystem, and a save copies between them
   unchanged. */
static int paste_available(const clipboard_t *clip, const mc_slot_info *slots, int viewing_port)
{
    if (!clip->valid || clip->port == viewing_port) {
        return 0;
    }

    const mc_slot_info *src = &slots[clip->port];
    const mc_slot_info *dst = &slots[viewing_port];

    int compatible = (src->type == dst->type) ||
                     (mc_type_is_ps1(src->type) && mc_type_is_ps1(dst->type));

    return src->present && src->formatted && dst->present && dst->formatted && compatible;
}

/* Draws a button-icon sprite followed by its text label on the hint bar,
   returning the x where the next hint can start. Everything is in logical
   pixels; the label is set in the middle size and centred vertically against
   the taller icon box. */
static int hint_icon(sprite_id icon, int x, int y, const char *label)
{
    gs_sprite_px(icon, x, y, UI_HINT_ICON);
    gs_print_font(FONT_MID, x + UI_HINT_ICON + UI_HINT_GAP,
                  y + (UI_HINT_ICON - UI_LINE_MID) / 2, UI_COLOR_DEFAULT, label);

    return x + UI_HINT_ICON + UI_HINT_GAP +
           gs_text_width_font(FONT_MID, label) + UI_HINT_SPACING;
}

/* Length in bytes of the longest prefix of src that fits in max_w logical
   pixels, broken at the last space inside it so words stay whole; a single
   word wider than the line is cut where it stops fitting. *next is where the
   following line resumes, past the space the break consumed. */
static int wrap_point(int font, const char *src, int max_w, int *next)
{
    int w = 0, i = 0, last_space = -1;
    char one[2] = { 0, 0 };

    while (src[i] != '\0') {
        one[0] = src[i];

        int cw = gs_text_width_font(font, one);
        if (w + cw > max_w) {
            break;
        }

        if (src[i] == ' ') {
            last_space = i;
        }
        w += cw;
        i++;
    }

    if (src[i] == '\0' || last_space <= 0) {
        *next = i;
        return i;
    }

    *next = last_space + 1;
    return last_space;
}

/* Splits src into two lines that each fit max_w logical pixels, so a save
   title shows in full under its tile instead of being cut off. A title's own
   line break survives icon.sys parsing as a space, so this usually
   reproduces the break the game intended. Anything past the second line is
   dropped - two lines is what the tile has room for. */
static void wrap_text(int font, const char *src, char *l1, char *l2, int size,
                      int max_w)
{
    int next = 0;
    int n = wrap_point(font, src, max_w, &next);

    if (n > size - 1) {
        n = size - 1;
    }
    memcpy(l1, src, (size_t)n);
    l1[n] = '\0';

    const char *rest = src + next;
    while (*rest == ' ') {
        rest++;
    }

    n = wrap_point(font, rest, max_w, &next);
    if (n > size - 1) {
        n = size - 1;
    }
    memcpy(l2, rest, (size_t)n);
    l2[n] = '\0';
}

static void render_list(mc_slot_info *slots, int cursor)
{
    gs_render_begin_frame();
    int y = UI_MARGIN_Y;
    int tx = UI_MARGIN_X + UI_CURSOR_INDENT;

    gs_print_px(UI_MARGIN_X, y, UI_COLOR_DEFAULT, "PS2 Memory Card Trader");
    y += UI_LINE * 2;

    for (int i = 0; i < SLOT_COUNT; i++) {
        char buf[48];

        if (i == cursor) {
            gs_print_px(UI_MARGIN_X, y, UI_COLOR_DEFAULT, ">");
        }

        snprintf(buf, sizeof(buf), "Memory Card %d: ", i + 1);
        gs_print_px(tx, y, UI_COLOR_DEFAULT, buf);
        gs_print_px(tx + gs_text_width(buf), y, slot_label_color(&slots[i]),
                    mc_driver_slot_label(&slots[i]));
        y += UI_LINE;
    }

    /* Trailing Exit entry, one blank row below the slots so it reads as a
       separate action rather than a third card. */
    y += UI_LINE;
    if (cursor == LIST_EXIT) {
        gs_print_px(UI_MARGIN_X, y, UI_COLOR_DEFAULT, ">");
    }
    gs_print_px(tx, y, UI_COLOR_DEFAULT, "Exit");

    int x = hint_icon(BTN_DPAD, UI_MARGIN_X, UI_FOOTER_Y, "Select");
    hint_icon(BTN_CROSS, x, UI_FOOTER_Y, (cursor == LIST_EXIT) ? "Select" : "Open");

    gs_render_end_frame();
}

static void render_confirm_format(int port)
{
    gs_render_begin_frame();
    int y = UI_MARGIN_Y;
    char buf[48];

    snprintf(buf, sizeof(buf), "Memory Card %d is unformatted.", port + 1);
    gs_print_px(UI_MARGIN_X, y, UI_COLOR_DEFAULT, buf);
    y += UI_LINE * 2;

    gs_print_px(UI_MARGIN_X, y, UI_COLOR_DEFAULT, "Format memory card?");
    y += UI_LINE * 2;

    int x = hint_icon(BTN_CROSS, UI_MARGIN_X, y, "YES");
    hint_icon(BTN_CIRCLE, x, y, "NO");

    gs_render_end_frame();
}

static void render_contents(int port, mc_entry *entries, int entry_count, int file_cursor,
                             const clipboard_t *clip, const mc_slot_info *slots)
{
    gs_render_begin_frame();
    char buf[80];
    int ps1 = mc_type_is_ps1(slots[port].type);

    /* Contents are browsed as pages of UI_GRID_PAGE icon tiles; the cursor is
       an index into the whole entry list, so its page follows it. */
    int page = (entry_count > 0) ? file_cursor / UI_GRID_PAGE : 0;
    int pages = (entry_count + UI_GRID_PAGE - 1) / UI_GRID_PAGE;
    int first = page * UI_GRID_PAGE;

    /* Page flips reuse the same cells for different saves, so any packets
       recorded for the previous page must stop drawing. */
    static int static_page = -1;
    if (first != static_page) {
        gs_icon_static_clear();
        grid_static_cells = 0;
        static_page = first;
    }

    /* Rebase the selected icon's animation whenever the cursor moves, so the
       newly selected tile starts its spin from zero. */
    if (file_cursor != sel_anim_cell) {
        sel_anim_cell = file_cursor;
        sel_anim_base = anim_time;
    }
    float sel_t = anim_time - sel_anim_base;

    /* Live-tile budget for this frame (see GRID_LIVE_ICONS). */
    int live_left = GRID_LIVE_ICONS;

    if (pages > 1) {
        snprintf(buf, sizeof(buf), "Memory Card %d   -   page %d/%d", port + 1, page + 1, pages);
    } else {
        snprintf(buf, sizeof(buf), "Memory Card %d", port + 1);
    }
    gs_print_px(UI_MARGIN_X, UI_MARGIN_Y, UI_COLOR_DEFAULT, buf);

    if (entry_count <= 0) {
        gs_print_px(UI_MARGIN_X, UI_MARGIN_Y + UI_LINE * 2, UI_COLOR_DEFAULT, "(empty)");
    }

    for (int i = first; i < entry_count && i < first + UI_GRID_PAGE; i++) {
        int cell = i - first;
        int tx = UI_GRID_X + (cell % UI_GRID_COLS) * UI_TILE_W;
        int ty = UI_GRID_Y + (cell / UI_GRID_COLS) * UI_TILE_H;

        if (i == file_cursor) {
            gs_fill_rect(tx, ty, UI_TILE_W - 4, UI_TILE_H - 4, UI_RGB(30, 60, 120));
        }

        /* Every tile is a live 3D model, but only the selected one animates
           and spins - it is drawn through gsKit each frame. A frozen
           (unselected, pop finished) icon costs too much to re-emit per
           frame, so it graduates into a prepacked static packet that the GS
           replays by DMA. Pending tiles show the placeholder cube, and a
           freshly loaded model pops in (scale 0 to 1) before freezing.
           Grid icons are unlit; the icon.sys rig only applies in the
           full-screen view. A PS1 save's icon is itself a cube, textured with
           the save's 16x16 bitmap, so the placeholder simply gains a skin. */
        if (entry_has_icon(&entries[i], ps1)) {
            int icx = tx + (UI_TILE_W - 4) / 2;
            int icy = ty + 2 + UI_TILE_ICON / 2;
            int sel = (i == file_cursor);
            u16 bit = (u16)(1u << cell);

            if (mc_icon_slot_matches(cell, port, entries[i].name)) {
                float dt = anim_time - slot_pop_time[cell];

                /* Out of live budget: cut the pop short rather than add
                   another live tile to this frame (see GRID_LIVE_ICONS). */
                if (!sel && dt < POP_DURATION && live_left <= 0) {
                    dt = POP_DURATION;
                }

                if (!sel && dt >= POP_DURATION) {
                    if (!(grid_static_cells & bit)) {
                        mc_icon_slot_prepack(cell, icx, icy, UI_TILE_ICON);
                        gs_icon_static_enable(cell, 1);
                        grid_static_cells |= bit;
                    }
                } else {
                    if (grid_static_cells & bit) {
                        gs_icon_static_enable(cell, 0);
                        grid_static_cells &= (u16)~bit;
                    }
                    int sz = (int)(UI_TILE_ICON * pop_scale(dt));
                    if (sz > 0) {
                        mc_icon_slot_draw(cell, icx, icy, sz,
                                          sel ? sel_t : 0.0f,
                                          sel ? sel_t * UI_SPIN_AUTO : 0.0f, 0);
                        live_left--;
                    }
                }
            } else {
                /* Still pending, or a save that simply has no usable icon -
                   both keep the cube. Fixed off-axis yaw so a still cube
                   reads as 3D instead of a flat rectangle; the selected
                   tile's cube spins. */
                draw_cube(icx, icy, UI_TILE_ICON,
                          sel ? sel_t * UI_SPIN_AUTO : 0.65f);
            }
        }

        /* Title under the icon in the small size, wrapped over the two lines
           the tile reserves and each line centred on its own. */
        char l1[PS2ICON_TITLE_MAX], l2[PS2ICON_TITLE_MAX];
        int text_y = ty + 2 + UI_TILE_ICON + 2;

        wrap_text(FONT_SMALL, entry_title[i], l1, l2, sizeof(l1), UI_TILE_W - 12);
        gs_print_font(FONT_SMALL,
                      tx + (UI_TILE_W - 4 - gs_text_width_font(FONT_SMALL, l1)) / 2,
                      text_y, UI_COLOR_DEFAULT, l1);
        if (l2[0] != '\0') {
            gs_print_font(FONT_SMALL,
                          tx + (UI_TILE_W - 4 - gs_text_width_font(FONT_SMALL, l2)) / 2,
                          text_y + UI_LINE_SMALL, UI_COLOR_DEFAULT, l2);
        }
    }

    /* One hint row: Back, Copy, Paste, Delete (in that fixed order, only the
       currently-applicable ones) aligned left; clipboard indicator aligned
       right on the same row. */
    int x = hint_icon(BTN_CIRCLE, UI_MARGIN_X, UI_FOOTER_Y, "Back");
    if (entry_count > 0) {
        x = hint_icon(BTN_SQUARE, x, UI_FOOTER_Y, "Copy");
    }
    if (paste_available(clip, slots, port)) {
        x = hint_icon(BTN_R1, x, UI_FOOTER_Y, "Paste");
    }
    if (entry_count > 0) {
        x = hint_icon(BTN_TRIANGLE, x, UI_FOOTER_Y, "Delete");
    }

    /* The clipboard indicator shares the hint bar, so it uses the small size:
       a full "mc0://SAVENAME" would not fit beside four hints otherwise. */
    if (clip->valid) {
        snprintf(buf, sizeof(buf), "Clipboard: mc%d://%s", clip->port, clip->name);

        int cx = UI_SCREEN_W - UI_MARGIN_X - gs_text_width_font(FONT_SMALL, buf);
        if (cx < x) {
            cx = x; /* never overlap the hints on the left */
        }
        gs_print_font(FONT_SMALL, cx, UI_FOOTER_Y + (UI_HINT_ICON - UI_LINE_SMALL) / 2,
                      UI_COLOR_DEFAULT, buf);
    }

    gs_render_end_frame();
}

/* Full-screen view of a single save: its icon spinning at the centre over the
   four-corner backdrop icon.sys defines, the way the PS2 browser presents a
   save once you pick it. A save with no usable icon shows the placeholder
   cube over a neutral backdrop instead, and drops the lighting hint since
   the cube has its own fixed lamp. */
static void render_icon_view(const char *title, float t, int slot, int has_icon,
                             int can_light)
{
    gs_render_begin_frame();

    if (has_icon) {
        u32 bg[4];
        mc_icon_slot_bg_colors(slot, bg);
        gs_fill_gradient(0, 0, UI_SCREEN_W, UI_SCREEN_H, bg[0], bg[1], bg[2], bg[3]);

        mc_icon_slot_draw(slot, UI_SCREEN_W / 2, UI_SCREEN_H / 2, UI_ICON_VIEW_SIZE, t,
                          spin_angle, preview_lights);
    } else {
        u32 top = UI_RGB(24, 32, 56), bottom = UI_RGB(8, 10, 20);

        gs_fill_gradient(0, 0, UI_SCREEN_W, UI_SCREEN_H, top, top, bottom, bottom);
        draw_cube(UI_SCREEN_W / 2, UI_SCREEN_H / 2, UI_ICON_VIEW_SIZE, spin_angle);
    }

    gs_print_px((UI_SCREEN_W - gs_text_width(title)) / 2, UI_MARGIN_Y,
                UI_COLOR_DEFAULT, title);

    int x = hint_icon(BTN_CIRCLE, UI_MARGIN_X, UI_FOOTER_Y, "Back");
    /* PS1 icons are flat bitmaps with no normals to light, so the toggle is
       only offered for the 3D models it can actually affect. */
    if (has_icon && can_light) {
        x = hint_icon(BTN_TRIANGLE, x, UI_FOOTER_Y,
                      preview_lights ? "Lights off" : "Lights on");
    }
    hint_icon(BTN_DPAD, x, UI_FOOTER_Y, "Rotate");

    gs_render_end_frame();
}

/* Centres one line of text on a pixel row. */
static void print_centered(int y, u32 color, const char *text)
{
    gs_print_px((UI_SCREEN_W - gs_text_width(text)) / 2, y, color, text);
}

/* Delete confirmation: the save's own icon spinning above the question, the
   way the PS2 browser asks. Saves that name a separate model for the delete
   state show that one (mc_icon_slot_load_state has already put it in the
   slot); the rest keep their normal icon, and a save with no usable icon at
   all falls back to the placeholder cube. `t` is elapsed seconds and drives
   both the animation and the idle spin. The save's icon.sys backdrop is
   deliberately not used here - this is a question, not a preview, so it keeps
   the app's own background. */
static void render_confirm_delete(const char *title, float t, int slot,
                                  int has_icon)
{
    gs_render_begin_frame();

    if (has_icon) {
        mc_icon_slot_draw(slot, UI_SCREEN_W / 2, UI_CONFIRM_ICON_Y,
                          UI_CONFIRM_ICON_SIZE, t, t * UI_SPIN_AUTO, 1);
    } else {
        draw_cube(UI_SCREEN_W / 2, UI_CONFIRM_ICON_Y, UI_CONFIRM_ICON_SIZE,
                  t * UI_SPIN_AUTO);
    }

    print_centered(UI_CONFIRM_TITLE_Y, UI_COLOR_DEFAULT, title);
    print_centered(UI_CONFIRM_TEXT_Y, UI_COLOR_DEFAULT, "Delete this save?");

    /* Both hints centred as one block: icon, gap and label for each, plus the
       spacing between them (hint_icon's trailing spacing is not part of the
       block, hence the one subtraction). */
    int w = (UI_HINT_ICON + UI_HINT_GAP + gs_text_width_font(FONT_MID, "YES") +
             UI_HINT_SPACING) +
            (UI_HINT_ICON + UI_HINT_GAP + gs_text_width_font(FONT_MID, "NO"));
    int x = hint_icon(BTN_CROSS, (UI_SCREEN_W - w) / 2, UI_FOOTER_Y, "YES");
    hint_icon(BTN_CIRCLE, x, UI_FOOTER_Y, "NO");

    gs_render_end_frame();
}

static void render_confirm_exit(void)
{
    gs_render_begin_frame();
    int y = UI_MARGIN_Y;

    gs_print_px(UI_MARGIN_X, y, UI_COLOR_DEFAULT, "Exit MCTrader?");
    y += UI_LINE * 2;

    int x = hint_icon(BTN_CROSS, UI_MARGIN_X, y, "YES");
    hint_icon(BTN_CIRCLE, x, y, "NO");

    gs_render_end_frame();
}

int main(int argc, char *argv[])
{
    sceSifInitRpc(0);

    /* Reset the IOP to a known state and patch its loader to accept modules
       from an EE buffer. audsrv.irx is embedded in this ELF (see audio.c), and
       the stock rom0:LOADFILE only takes module paths - without the patch it
       cannot be loaded at all. Everything the app loads afterwards (SIO2MAN,
       MCMAN, MCSERV, PADMAN, LIBSD) comes from rom0 on the freshly reset IOP,
       so this has to happen before any driver init. */
    while (!SifIopReset("", 0)) { }
    while (!SifIopSync()) { }
    sceSifInitRpc(0);
    SifLoadFileInit();
    sbv_patch_enable_lmb();

    gs_render_init(); /* owns the GS/framebuffer; replaces debug.h's init_scr.
                         Must come up before anything tries to draw. */

    mc_driver_init();
    pad_driver_init();
    audio_init();

    mc_slot_info slots[SLOT_COUNT];
    int mismatch_streak[SLOT_COUNT];
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (!mc_driver_scan(i, &slots[i])) {
            slots[i].type = MC_TYPE_NONE;
            slots[i].present = 0;
            slots[i].formatted = 0;
        }
        mismatch_streak[i] = 0;
    }

    app_state state = STATE_LIST;
    int cursor = 0;
    int viewing_port = 0;
    int scan_counter = 0;
    int quit = 0;

    mc_entry entries[MC_MAX_ENTRIES];
    int entry_count = 0;
    int file_cursor = 0;

    clipboard_t clipboard;
    clipboard.valid = 0;

    /* Whether the delete confirmation has a model to show, decided when that
       screen is opened: its icon is loaded once there, not every frame. */
    int confirm_has_icon = 0;

    render_list(slots, cursor);

    while (1) {
        gs_render_vsync_wait();

        u32 pressed = pad_driver_read_new_buttons();

        /* These screens hold a live animated icon, so they have to be redrawn
           every frame; the others are static and only redraw on change. */
        if (state == STATE_CONTENTS) {
            anim_time += 1.0f / 60.0f;
            icons_tick(viewing_port, mc_type_is_ps1(slots[viewing_port].type),
                       entries, entry_count, file_cursor);
            render_contents(viewing_port, entries, entry_count, file_cursor, &clipboard, slots);
        } else if (state == STATE_ICON_VIEW) {
            anim_time += 1.0f / 60.0f;
            update_spin(1.0f / 60.0f);
            render_icon_view(entry_title[file_cursor], anim_time - sel_anim_base,
                             file_cursor % UI_GRID_PAGE,
                             !entry_icon_failed[file_cursor],
                             !mc_type_is_ps1(slots[viewing_port].type));
        } else if (state == STATE_CONFIRM_DELETE) {
            anim_time += 1.0f / 60.0f;
            render_confirm_delete(entry_title[file_cursor], anim_time - sel_anim_base,
                                  file_cursor % UI_GRID_PAGE, confirm_has_icon);
        }

        scan_counter++;
        if (scan_counter >= SCAN_INTERVAL) {
            scan_counter = 0;

            if (state == STATE_LIST) {
                int changed = 0;
                for (int i = 0; i < SLOT_COUNT; i++) {
                    if (poll_slot(i, slots, mismatch_streak)) {
                        changed = 1;
                    }
                }
                if (changed) {
                    render_list(slots, cursor);
                }
            } else if (state == STATE_CONTENTS) {
                if (poll_slot(viewing_port, slots, mismatch_streak) && !slots[viewing_port].present) {
                    /* The card was removed (and the removal confirmed) while
                       its contents were open; bail out to the list. */
                    gs_icon_static_clear();
                    grid_static_cells = 0;
                    state = STATE_LIST;
                    render_list(slots, cursor);
                }
            }
        }

        if (!pressed) {
            continue;
        }

        switch (state) {
            case STATE_LIST:
                if (pressed & (PAD_UP | PAD_DOWN)) {
                    /* Wraps in both directions over the slots plus Exit. */
                    if (pressed & PAD_UP) {
                        cursor = (cursor == 0) ? LIST_ITEMS - 1 : cursor - 1;
                    } else {
                        cursor = (cursor + 1) % LIST_ITEMS;
                    }
                    audio_play(SFX_TICK);
                    render_list(slots, cursor);
                } else if (pressed & PAD_CROSS) {
                    if (cursor == LIST_EXIT) {
                        state = STATE_CONFIRM_EXIT;
                        render_confirm_exit();
                    } else if (slots[cursor].present) {
                        viewing_port = cursor;
                        if (slots[cursor].formatted) {
                            audio_play(SFX_OPEN);
                            entry_count = mc_driver_list(viewing_port, entries, MC_MAX_ENTRIES);
                            if (entry_count < 0) {
                                entry_count = 0;
                            }
                            contents_reset(entries, entry_count);
                            file_cursor = 0;
                            state = STATE_CONTENTS;
                            render_contents(viewing_port, entries, entry_count, file_cursor, &clipboard, slots);
                        } else {
                            state = STATE_CONFIRM_FORMAT;
                            render_confirm_format(viewing_port);
                        }
                    }
                }
                break;

            case STATE_CONFIRM_FORMAT:
                if (pressed & PAD_CROSS) {
                    audio_play(SFX_OPEN);
                    mc_driver_format(viewing_port);
                    mc_driver_scan(viewing_port, &slots[viewing_port]);
                    entry_count = mc_driver_list(viewing_port, entries, MC_MAX_ENTRIES);
                    if (entry_count < 0) {
                        entry_count = 0;
                    }
                    contents_reset(entries, entry_count);
                    file_cursor = 0;
                    state = STATE_CONTENTS;
                    render_contents(viewing_port, entries, entry_count, file_cursor, &clipboard, slots);
                } else if (pressed & PAD_CIRCLE) {
                    audio_play(SFX_CLOSE);
                    state = STATE_LIST;
                    render_list(slots, cursor);
                }
                break;

            case STATE_CONTENTS:
                if (pressed & PAD_CIRCLE) {
                    audio_play(SFX_CLOSE);
                    gs_icon_static_clear(); /* static packets are sent every frame */
                    grid_static_cells = 0;
                    mc_driver_scan(viewing_port, &slots[viewing_port]);
                    state = STATE_LIST;
                    render_list(slots, cursor);
                } else if (pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT) && entry_count > 0) {
                    /* 2D movement over the icon grid: left/right step one
                       tile (wrapping through the whole list, so it carries
                       across page boundaries), up/down step a full row. */
                    if (pressed & PAD_LEFT) {
                        file_cursor = (file_cursor == 0) ? entry_count - 1 : file_cursor - 1;
                    } else if (pressed & PAD_RIGHT) {
                        file_cursor = (file_cursor + 1) % entry_count;
                    } else if (pressed & PAD_UP) {
                        if (file_cursor >= UI_GRID_COLS) {
                            file_cursor -= UI_GRID_COLS;
                        }
                    } else {
                        if (file_cursor + UI_GRID_COLS < entry_count) {
                            file_cursor += UI_GRID_COLS;
                        }
                    }
                    audio_play(SFX_TICK);
                    /* No redraw here: the grid is one of the screens the main
                       loop already redraws every frame, and rendering it a
                       second time in the same iteration doubles the frame's
                       GIF stream exactly while a page is loading. */
                } else if (pressed & PAD_CROSS && entry_count > 0 &&
                           entry_has_icon(&entries[file_cursor],
                                          mc_type_is_ps1(slots[viewing_port].type))) {
                    /* Open the save on its own, full screen. Usually a
                       cache hit (the grid already loaded it); if its tile was
                       still a cube this blocks for the one read. A save with
                       no usable icon opens too and keeps the cube, so no tile
                       is a dead end; one already known to fail skips the
                       read. */
                    if (!entry_icon_failed[file_cursor] &&
                        !mc_icon_slot_load(file_cursor % UI_GRID_PAGE, viewing_port,
                                           entries[file_cursor].name,
                                           mc_type_is_ps1(slots[viewing_port].type),
                                           entry_title[file_cursor], PS2ICON_TITLE_MAX)) {
                        entry_icon_failed[file_cursor] = 1;
                        if (entry_title[file_cursor][0] == '\0') {
                            strncpy(entry_title[file_cursor], entries[file_cursor].name,
                                    PS2ICON_TITLE_MAX - 1);
                            entry_title[file_cursor][PS2ICON_TITLE_MAX - 1] = '\0';
                        }
                    }
                    audio_play(SFX_VIEW);
                    gs_icon_static_clear(); /* the view draws its own backdrop */
                    grid_static_cells = 0;
                    spin_angle = 0.0f;
                    spin_vel = UI_SPIN_AUTO;
                    spin_idle = UI_SPIN_RESUME;
                    sel_anim_base = anim_time; /* animation restarts with the spin */
                    preview_lights = 1;
                    state = STATE_ICON_VIEW;
                } else if (pressed & PAD_SQUARE && entry_count > 0) {
                    clipboard.valid = 1;
                    clipboard.port = viewing_port;
                    clipboard.is_dir = entries[file_cursor].is_dir;
                    strncpy(clipboard.name, entries[file_cursor].name, sizeof(clipboard.name) - 1);
                    clipboard.name[sizeof(clipboard.name) - 1] = '\0';
                } else if (pressed & PAD_TRIANGLE && entry_count > 0) {
                    /* The confirm screen draws the save's icon full-screen and
                       nothing of the grid, and the frozen grid icons are
                       static packets replayed every frame, so they have to be
                       switched off explicitly. */
                    gs_icon_static_clear();
                    grid_static_cells = 0;

                    /* Swap the cursor's tile slot over to the save's delete
                       icon, if it has one of its own. A save already known to
                       have no usable icon isn't retried; it gets the cube. */
                    confirm_has_icon = !entry_icon_failed[file_cursor] &&
                        entry_has_icon(&entries[file_cursor],
                                       mc_type_is_ps1(slots[viewing_port].type)) &&
                        mc_icon_slot_load_state(file_cursor % UI_GRID_PAGE, viewing_port,
                                                entries[file_cursor].name,
                                                mc_type_is_ps1(slots[viewing_port].type),
                                                PS2ICON_STATE_DELETE);
                    sel_anim_base = anim_time; /* the icon starts from its first frame */
                    state = STATE_CONFIRM_DELETE;
                } else if (pressed & PAD_R1 && paste_available(&clipboard, slots, viewing_port)) {
                    mc_driver_copy(clipboard.port, viewing_port, clipboard.name, clipboard.is_dir);
                    entry_count = mc_driver_list(viewing_port, entries, MC_MAX_ENTRIES);
                    if (entry_count < 0) {
                        entry_count = 0;
                    }
                    if (file_cursor >= entry_count) {
                        file_cursor = entry_count > 0 ? entry_count - 1 : 0;
                    }
                    contents_reset(entries, entry_count);
                }
                break;

            case STATE_ICON_VIEW:
                if (pressed & PAD_CIRCLE) {
                    audio_play(SFX_CLOSE);
                    /* Back in the grid the tile is selected anew, so its spin
                       restarts instead of picking up the preview's phase. */
                    sel_anim_cell = -1;
                    state = STATE_CONTENTS;
                } else if (pressed & PAD_TRIANGLE) {
                    preview_lights = !preview_lights;
                }
                break;

            case STATE_CONFIRM_DELETE:
                if (pressed & PAD_CROSS) {
                    mc_driver_delete(viewing_port, entries[file_cursor].name, entries[file_cursor].is_dir);
                    entry_count = mc_driver_list(viewing_port, entries, MC_MAX_ENTRIES);
                    if (entry_count < 0) {
                        entry_count = 0;
                    }
                    if (file_cursor >= entry_count) {
                        file_cursor = entry_count > 0 ? entry_count - 1 : 0;
                    }
                    contents_reset(entries, entry_count);
                    state = STATE_CONTENTS;
                    render_contents(viewing_port, entries, entry_count, file_cursor, &clipboard, slots);
                } else if (pressed & PAD_CIRCLE) {
                    audio_play(SFX_CLOSE);
                    /* Put the normal icon back in the tile's slot before the
                       grid is drawn again, so it doesn't flash the placeholder
                       cube for the frame it would take icons_tick to notice. */
                    if (confirm_has_icon) {
                        mc_icon_slot_load_state(file_cursor % UI_GRID_PAGE, viewing_port,
                                                entries[file_cursor].name,
                                                mc_type_is_ps1(slots[viewing_port].type),
                                                PS2ICON_STATE_NORMAL);
                    }
                    sel_anim_cell = -1; /* the tile is selected anew */
                    state = STATE_CONTENTS;
                    render_contents(viewing_port, entries, entry_count, file_cursor, &clipboard, slots);
                }
                break;

            case STATE_CONFIRM_EXIT:
                if (pressed & PAD_CROSS) {
                    audio_play(SFX_CLOSE);
                    quit = 1;
                } else if (pressed & PAD_CIRCLE) {
                    audio_play(SFX_CLOSE);
                    state = STATE_LIST;
                    render_list(slots, cursor);
                }
                break;
        }

        if (quit) {
            break;
        }
    }

    /* Hand the machine back to the system browser. LoadExecPS2 resets the IOP
       itself, so the modules this app loaded (audsrv, MCSERV, PADMAN...) go
       away with it and nothing needs tearing down here first.
       Passing argc=0 makes OSDSYS resume on the last-launched icon (the
       memory card slot this ELF booted from) instead of the main browser.
       Passing argv[0]="rom0:OSDSYS" with argc=1 forces it back to the
       default browser/CLOCK page. */
    static char *osdsys_argv[1] = { "rom0:OSDSYS" };
    LoadExecPS2("rom0:OSDSYS", 1, osdsys_argv);

    return 0;
}
