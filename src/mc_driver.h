#ifndef MC_DRIVER_H
#define MC_DRIVER_H

typedef struct {
    int type;       /* MC_TYPE_PS2, MC_TYPE_PSX, MC_TYPE_POCKET or MC_TYPE_NONE */
    int present;    /* 1 if a card responded, 0 if the slot is empty */
    int formatted;  /* 1 if the card has a filesystem, only meaningful when present */
} mc_slot_info;

/* Loads the SIO2MAN/MCMAN/MCSERV IOP modules and initializes libmc. */
void mc_driver_init(void);

/* Queries the card currently in the given port (0 = mc0, 1 = mc1) and fills
   *info on success. Returns 0 without touching *info on a transient read
   error (e.g. the shared SIO2 bus is momentarily busy from a hot-swap on
   the other port) so the caller can keep showing its last known state
   instead of flickering to "No Card". Returns 1 on a reliable reading. */
int mc_driver_scan(int port, mc_slot_info *info);

/* Returns a short human-readable label for the given slot, e.g.
   "Playstation 2 Card", "Unformatted" or "No Card". */
const char *mc_driver_slot_label(const mc_slot_info *info);

/* True for both flavours of PS1 card: a plain one (MC_TYPE_PSX) and a
   PocketStation (MC_TYPE_POCKET), which is a PS1 card with a CPU bolted on -
   same filesystem, same saves, same icons, and which of the two mcman reports
   depends on whether the card answers the PocketStation probe. Everything
   that cares about the save format has to accept both. */
int mc_type_is_ps1(int type);

/* Formats the card in the given port. Returns 0 on success, < 0 on error. */
int mc_driver_format(int port);

#define MC_MAX_ENTRIES 48

typedef struct {
    char name[32];
    int is_dir;
    unsigned size;
} mc_entry;

/* Lists the root directory of the card in the given port into `entries`
   (up to max_entries). Returns the number of entries (0 = empty), or a
   negative libmc error code on failure (e.g. -2 = unformatted). */
int mc_driver_list(int port, mc_entry *entries, int max_entries);

/* Copies a file or (one-level-deep) directory named `name` from src_port
   to dst_port. Both cards must already be known-compatible (same card
   type) - this function does not check that itself. Returns 0 on success,
   < 0 on error (e.g. destination full, I/O failure). */
int mc_driver_copy(int src_port, int dst_port, const char *name, int is_dir);

/* Deletes a file or (one-level-deep) directory named `name` from the given
   port. Returns 0 on success, < 0 on error. */
int mc_driver_delete(int port, const char *name, int is_dir);

/* Reads a whole file from the card into `buf` (up to max_len bytes). `path`
   is a full in-card path, e.g. "/SAVEDIR/icon.sys". Returns the number of
   bytes read, or < 0 on error (open failed / not found). */
int mc_driver_read_file(int port, const char *path, void *buf, int max_len);

#endif
