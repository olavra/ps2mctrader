#ifndef MC_ICON_H
#define MC_ICON_H

#include "ps2icon.h"
#include "ps1icon.h"
#include "gs_render.h"

/*
 * Loads save icons off a physical memory card: the glue between mc_driver
 * (card I/O) and the two icon formats - ps2icon for a PS2 save's animated 3D
 * model, ps1icon for a PS1 save's 16x16 bitmap on a cube. A pool of
 * GS_ICON_SLOTS models is kept resident, one per grid tile, each cached by
 * (port, name) so revisiting a page is free. Loading is synchronous per save;
 * the caller spreads calls across frames to keep the UI live.
 *
 * Which format a save uses follows from the card, not from the save: PS1 cards
 * hold PS1 saves as plain files, PS2 cards hold PS2 saves as directories, so
 * every entry point takes a `ps1` flag the caller derives from the card type.
 */

/* Forgets every cached model. Call when a card is opened or its contents
   change, so stale (port, dir) keys can't serve a wrong model. */
void mc_icon_reset(void);

/* True when `slot` holds a ready model for exactly this save, in its normal
   state - a slot showing the save's copy or delete model does not match, so
   the grid reloads the normal one when it comes back. */
int mc_icon_slot_matches(int slot, int port, const char *name);

/* As mc_icon_slot_load, but for the copy/delete variant of a PS2 save's icon
   (see ps2icon_state). A save that names no model for that state, and every
   PS1 save, loads its normal icon instead. */
int mc_icon_slot_load_state(int slot, int port, const char *name, int ps1,
                            ps2icon_state state);

/* Reads the save `name` on the card in `port` into `slot`, replacing whatever
   the slot held: with `ps1` clear, the save directory's icon.sys and .ico;
   with `ps1` set, the title frame that heads the save file. A cache hit (slot
   already holds this save) returns immediately. If `title` is non-NULL it
   receives the save's own title, filled in even when the icon itself fails to
   parse. Returns 1 if the slot holds a drawable model afterwards. */
int mc_icon_slot_load(int slot, int port, const char *name, int ps1,
                      char *title, int title_len);

/* Draws slot's icon centred on (cx, cy) at `size` pixels. `t` is elapsed
   seconds and drives the animation; `spin` is the rotation in radians;
   `lights` zero shows the texture unshaded. No-op unless the slot is ready. */
void mc_icon_slot_draw(int slot, int cx, int cy, int size,
                       float t, float spin, int lights);

/* Slot's four backdrop corner colours (upper-left, upper-right, lower-left,
   lower-right): from its icon.sys for a PS2 save, a fixed neutral gradient
   for a PS1 one. Zeros when the slot is not ready. */
void mc_icon_slot_bg_colors(int slot, u32 out[4]);

/* Records slot's frozen rest pose into its prepacked static packet (see
   gs_icon_static_* in gs_render.h). No-op unless the slot is ready. */
void mc_icon_slot_prepack(int slot, int cx, int cy, int size);

#endif
