/*
 * Memory card save icon loading - see mc_icon.h.
 */

#include <tamtypes.h>
#include <stdio.h>
#include <string.h>

#include "mc_icon.h"
#include "mc_driver.h"
#include "ps2icon.h"
#include "ps1icon.h"

/* Largest .ico accepted. */
#define ICO_MAX (512 * 1024)

/* libmc DMAs into these, which requires 64-byte alignment; unaligned reads
   come back empty. */
static u8 sys_buf[1024] __attribute__((aligned(64)));
static u8 ico_buf[ICO_MAX] __attribute__((aligned(64)));

/* The resident model pool, one slot per grid tile. Each ps2icon_model is
   ~400 KB and each ps1icon_model 32 KB, so this is a few MB of static memory
   - fine on 32 MB, and far cheaper than re-reading models off the card every
   time a page shows. Both pools are allocated for every slot rather than
   overlapped in a union: a card swap can turn a PS1 page into a PS2 one, and
   the wasted 512 KB buys not having to reason about that. */
typedef struct {
    int ready;
    int ps1;        /* which of the two model pools this slot holds */
    int port;
    char name[40];
    ps2icon_state state;            /* which of the save's icons is loaded */
    char icon[PS2ICON_NAME_MAX];    /* the .ico that produced the model, so a
                                       state change that resolves to the same
                                       file needs no re-read */
    ps2icon_sys sys;
} slot_state;

static ps2icon_model models[GS_ICON_SLOTS];
static ps1icon_model ps1_models[GS_ICON_SLOTS];
static slot_state slots[GS_ICON_SLOTS];

/* Reads a save's icon.sys into sys_buf and parses it into `out`. */
static int read_sys(int port, const char *dir, ps2icon_sys *out)
{
    char path[96];
    snprintf(path, sizeof(path), "/%s/icon.sys", dir);

    int n = mc_driver_read_file(port, path, sys_buf, sizeof(sys_buf));
    return ps2icon_sys_parse(sys_buf, n, out);
}

/* Reads and parses one of the save's .ico files. */
static int read_ico(int port, const char *dir, const char *file,
                    ps2icon_model *out)
{
    char path[96];

    if (file[0] == '\0') {
        return 0;
    }
    snprintf(path, sizeof(path), "/%s/%s", dir, file);

    int n = mc_driver_read_file(port, path, ico_buf, ICO_MAX);
    return ps2icon_parse(ico_buf, n, out);
}

void mc_icon_reset(void)
{
    for (int i = 0; i < GS_ICON_SLOTS; i++) {
        slots[i].ready = 0;
    }
}

/* Reads the title frame that heads a PS1 save file and parses it. Only the
   first PS1ICON_HEADER_SIZE bytes matter, so the rest of the save - up to
   120 KB of game data - is never pulled off the card. */
static int read_ps1(int port, const char *name, ps1icon_model *out)
{
    char path[96];
    snprintf(path, sizeof(path), "/%s", name);

    int n = mc_driver_read_file(port, path, sys_buf, PS1ICON_HEADER_SIZE);
    return ps1icon_parse(sys_buf, n, out);
}

/* Copies `src` into the caller's title buffer, if it asked for one. */
static void give_title(char *title, int title_len, const char *src)
{
    if (title && title_len > 0) {
        strncpy(title, src, title_len - 1);
        title[title_len - 1] = '\0';
    }
}

/* True when the slot already holds this save's icon in this very state. */
static int slot_matches_state(int slot, int port, const char *name,
                              ps2icon_state state)
{
    if (slot < 0 || slot >= GS_ICON_SLOTS) {
        return 0;
    }
    return slots[slot].ready && slots[slot].port == port &&
           slots[slot].state == state && strcmp(slots[slot].name, name) == 0;
}

int mc_icon_slot_matches(int slot, int port, const char *name)
{
    return slot_matches_state(slot, port, name, PS2ICON_STATE_NORMAL);
}

/* Marks the slot as holding this save, once its model is uploaded. `file` is
   the .ico the model came from, empty for a PS1 save. */
static void slot_commit(int slot, int port, const char *name, int ps1,
                        ps2icon_state state, const char *file)
{
    slots[slot].port = port;
    slots[slot].ps1 = ps1;
    slots[slot].state = state;
    strncpy(slots[slot].name, name, sizeof(slots[slot].name) - 1);
    slots[slot].name[sizeof(slots[slot].name) - 1] = '\0';
    strncpy(slots[slot].icon, file, PS2ICON_NAME_MAX - 1);
    slots[slot].icon[PS2ICON_NAME_MAX - 1] = '\0';
    slots[slot].ready = 1;
}

/* Both entry points. A PS1 save has a single icon and no states, so it always
   loads as PS2ICON_STATE_NORMAL and any state asked for is ignored. */
static int slot_load(int slot, int port, const char *name, int ps1,
                     ps2icon_state state, char *title, int title_len)
{
    if (slot < 0 || slot >= GS_ICON_SLOTS) {
        return 0;
    }
    if (ps1) {
        state = PS2ICON_STATE_NORMAL;
    }

    if (slot_matches_state(slot, port, name, state)) {
        give_title(title, title_len,
                   slots[slot].ps1 ? ps1_models[slot].title : slots[slot].sys.title);
        return 1;
    }

    if (title && title_len > 0) {
        title[0] = '\0';
    }

    if (ps1) {
        slots[slot].ready = 0;

        int ok = read_ps1(port, name, &ps1_models[slot]);

        /* A title frame with a broken icon flag still yields the save's
           title, the same way a PS2 save's icon.sys does. */
        give_title(title, title_len, ps1_models[slot].title);
        if (!ok) {
            return 0;
        }

        ps1icon_upload(&ps1_models[slot], slot);
        slot_commit(slot, port, name, 1, state, "");
        return 1;
    }

    /* icon.sys is read into a local first: which .ico the wanted state uses is
       only known afterwards, and it may turn out to be the one the slot
       already holds - most saves name the same file for all three states. */
    ps2icon_sys sys;
    if (!read_sys(port, name, &sys)) {
        slots[slot].ready = 0;
        return 0;
    }

    /* The title is available even when the model itself cannot be parsed. */
    give_title(title, title_len, sys.title);

    const char *file = ps2icon_sys_icon(&sys, state);

    if (slots[slot].ready && !slots[slot].ps1 && slots[slot].port == port &&
        strcmp(slots[slot].name, name) == 0 &&
        strcmp(slots[slot].icon, file) == 0) {
        slots[slot].state = state; /* same model, different state name */
        return 1;
    }

    slots[slot].ready = 0;
    slots[slot].sys = sys;

    if (!read_ico(port, name, file, &models[slot])) {
        return 0;
    }

    ps2icon_upload(&models[slot], slot);
    slot_commit(slot, port, name, 0, state, file);
    return 1;
}

int mc_icon_slot_load(int slot, int port, const char *name, int ps1,
                      char *title, int title_len)
{
    return slot_load(slot, port, name, ps1, PS2ICON_STATE_NORMAL,
                     title, title_len);
}

int mc_icon_slot_load_state(int slot, int port, const char *name, int ps1,
                            ps2icon_state state)
{
    return slot_load(slot, port, name, ps1, state, NULL, 0);
}

void mc_icon_slot_draw(int slot, int cx, int cy, int size,
                       float t, float spin, int lights)
{
    if (slot < 0 || slot >= GS_ICON_SLOTS || !slots[slot].ready) {
        return;
    }

    if (slots[slot].ps1) {
        /* A PS1 icon is a flat bitmap on a cube: it carries no normals, so
           the lighting toggle has nothing to act on and is ignored. */
        ps1icon_draw(&ps1_models[slot], slot, cx, cy, size, t, spin);
        return;
    }
    ps2icon_draw(&models[slot], &slots[slot].sys, slot, cx, cy, size, t, spin, lights);
}

void mc_icon_slot_prepack(int slot, int cx, int cy, int size)
{
    if (slot < 0 || slot >= GS_ICON_SLOTS || !slots[slot].ready) {
        return;
    }

    if (slots[slot].ps1) {
        ps1icon_prepack(&ps1_models[slot], slot, cx, cy, size);
        return;
    }
    ps2icon_prepack(&models[slot], &slots[slot].sys, slot, cx, cy, size);
}

void mc_icon_slot_bg_colors(int slot, u32 out[4])
{
    int ok = (slot >= 0 && slot < GS_ICON_SLOTS && slots[slot].ready);

    if (ok && slots[slot].ps1) {
        ps1icon_bg_colors(out);
        return;
    }

    for (int i = 0; i < 4; i++) {
        out[i] = ok ? slots[slot].sys.bg_color[i] : 0;
    }
}
