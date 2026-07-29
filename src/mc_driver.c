/*
 * PS2 Memory Card Trader
 *
 * Physical memory card access via PS2SDK libmc. SIO2MAN/MCMAN/MCSERV
 * transparently support both PS1 and PS2 memory cards in slots mc0/mc1
 * through the same API.
 */

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libmc.h>
#include <stdio.h>
#include <string.h>

#include "mc_driver.h"

#define MC_DIR_MAX_ENTRIES 48

/* mcOpen's mode is an MCMAN permission bitfield (SCE_STM_R/W plus
   sceMcFileCreateFile), not the POSIX O_* flags. They must not be mixed up:
   newlib's O_RDONLY is 0, which grants no access and makes every mcRead fail
   with -5, and its O_WRONLY is 1, which MCMAN reads as "readable". */
#define MC_RDONLY 0x01
#define MC_WRONLY 0x02
#define MC_CREAT  0x0200

static sceMcTblGetDir mc_dir_entries[MC_DIR_MAX_ENTRIES] __attribute__((aligned(64)));

void mc_driver_init(void)
{
    int ret;

    ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    if (ret < 0) {
        printf("Failed to load SIO2MAN (%d)\n", ret);
    }

    ret = SifLoadModule("rom0:MCMAN", 0, NULL);
    if (ret < 0) {
        printf("Failed to load MCMAN (%d)\n", ret);
    }

    ret = SifLoadModule("rom0:MCSERV", 0, NULL);
    if (ret < 0) {
        printf("Failed to load MCSERV (%d)\n", ret);
    }

    if (mcInit(MC_TYPE_MC) < 0) {
        printf("Failed to initialize memory card library\n");
    }
}

int mc_driver_scan(int port, mc_slot_info *info)
{
    int type, free_clusters, format, ret;

    mcGetInfo(port, 0, &type, &free_clusters, &format);
    mcSync(0, NULL, &ret);

    /* ret < -2 is a transient access error, not necessarily an empty slot:
       the SIO2 bus is shared between both ports, so a hot-swap on the
       other port can momentarily disturb this one's read too. Bail out
       without touching *info so the caller keeps its last known state. */
    if (ret < -2) {
        return 0;
    }

    if (type == MC_TYPE_NONE) {
        info->type = MC_TYPE_NONE;
        info->present = 0;
        info->formatted = 0;
        return 1;
    }

    info->type = type;
    info->present = 1;

    /* mcGetInfo's format flag is unreliable/edge-triggered, so probe the
       filesystem directly instead: mcGetDir returns -2 for an unformatted
       card. */
    mcGetDir(port, 0, "/*", 0, 1, mc_dir_entries);
    mcSync(0, NULL, &ret);

    info->formatted = (ret != -2);

    return 1;
}

int mc_type_is_ps1(int type)
{
    return type == MC_TYPE_PSX || type == MC_TYPE_POCKET;
}

const char *mc_driver_slot_label(const mc_slot_info *info)
{
    if (!info->present) {
        return "No Card";
    }

    if (!info->formatted) {
        return "Unformatted";
    }

    switch (info->type) {
        case MC_TYPE_PS2:    return "Playstation 2 Card";
        case MC_TYPE_PSX:    return "Playstation 1 Card";
        case MC_TYPE_POCKET: return "PocketStation Card";
        default:              return "Unknown Card";
    }
}

int mc_driver_format(int port)
{
    int ret;

    mcFormat(port, 0);
    mcSync(0, NULL, &ret);

    return ret;
}

int mc_driver_list(int port, mc_entry *entries, int max_entries)
{
    int ret;

    mcGetDir(port, 0, "/*", 0, max_entries, mc_dir_entries);
    mcSync(0, NULL, &ret);

    if (ret < 0) {
        return ret;
    }

    int count = ret;
    if (count > max_entries) {
        count = max_entries;
    }

    for (int i = 0; i < count; i++) {
        strncpy(entries[i].name, (const char *)mc_dir_entries[i].EntryName, sizeof(entries[i].name) - 1);
        entries[i].name[sizeof(entries[i].name) - 1] = '\0';
        entries[i].is_dir = (mc_dir_entries[i].AttrFile & MC_ATTR_SUBDIR) ? 1 : 0;
        entries[i].size = mc_dir_entries[i].FileSizeByte;
    }

    return count;
}

/* Copies one file, given full in-card paths (e.g. "/NAME" or "/DIR/NAME"). */
static int mc_copy_one_file(int src_port, const char *src_path, int dst_port, const char *dst_path)
{
    static char buf[16384] __attribute__((aligned(64)));
    int src_fd, dst_fd, ret;

    mcOpen(src_port, 0, src_path, MC_RDONLY);
    mcSync(0, NULL, &src_fd);
    if (src_fd < 0) {
        return src_fd;
    }

    mcOpen(dst_port, 0, dst_path, MC_WRONLY | MC_CREAT);
    mcSync(0, NULL, &dst_fd);
    if (dst_fd < 0) {
        mcClose(src_fd);
        mcSync(0, NULL, &ret);
        return dst_fd;
    }

    for (;;) {
        int read_bytes, written;

        mcRead(src_fd, buf, sizeof(buf));
        mcSync(0, NULL, &read_bytes);
        if (read_bytes <= 0) {
            break;
        }

        mcWrite(dst_fd, buf, read_bytes);
        mcSync(0, NULL, &written);
        if (written != read_bytes) {
            mcClose(src_fd);
            mcSync(0, NULL, &ret);
            mcClose(dst_fd);
            mcSync(0, NULL, &ret);
            return -1;
        }
    }

    mcClose(src_fd);
    mcSync(0, NULL, &ret);
    mcClose(dst_fd);
    mcSync(0, NULL, &ret);

    return 0;
}

int mc_driver_copy(int src_port, int dst_port, const char *name, int is_dir)
{
    int ret;
    char src_path[70], dst_path[70];

    if (!is_dir) {
        snprintf(src_path, sizeof(src_path), "/%s", name);
        snprintf(dst_path, sizeof(dst_path), "/%s", name);
        return mc_copy_one_file(src_port, src_path, dst_port, dst_path);
    }

    /* PS2 saves are one-level-deep directories: create the directory, then
       copy every file inside it. */
    snprintf(dst_path, sizeof(dst_path), "/%s", name);
    mcMkDir(dst_port, 0, dst_path);
    mcSync(0, NULL, &ret);
    if (ret < 0) {
        return ret;
    }

    char glob_path[70];
    snprintf(glob_path, sizeof(glob_path), "/%s/*", name);

    mcGetDir(src_port, 0, glob_path, 0, MC_DIR_MAX_ENTRIES, mc_dir_entries);
    mcSync(0, NULL, &ret);
    if (ret < 0) {
        return ret;
    }

    int count = ret;
    for (int i = 0; i < count; i++) {
        if (mc_dir_entries[i].AttrFile & MC_ATTR_SUBDIR) {
            continue; /* saves are flat; skip any unexpected nested directory */
        }
        snprintf(src_path, sizeof(src_path), "/%s/%s", name, mc_dir_entries[i].EntryName);
        snprintf(dst_path, sizeof(dst_path), "/%s/%s", name, mc_dir_entries[i].EntryName);
        ret = mc_copy_one_file(src_port, src_path, dst_port, dst_path);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

/* Read granularity. Asking mcRead for more bytes than the file actually has
   left fails outright (-5) instead of returning a short read, so the file is
   pulled in fixed-size chunks and a failure after the first chunk is simply
   treated as end-of-file. */
#define MC_READ_CHUNK 512

int mc_driver_read_file(int port, const char *path, void *buf, int max_len)
{
    int fd, ret, total = 0;

    mcOpen(port, 0, path, MC_RDONLY);
    mcSync(0, NULL, &fd);
    if (fd < 0) {
        return fd;
    }

    while (total < max_len) {
        int got;
        int want = max_len - total;
        if (want > MC_READ_CHUNK) {
            want = MC_READ_CHUNK;
        }

        mcRead(fd, (char *)buf + total, want);
        mcSync(0, NULL, &got);
        if (got <= 0) {
            break; /* error or EOF; whatever was read so far still counts */
        }
        total += got;
        if (got < want) {
            break; /* short read = end of file */
        }
    }

    mcClose(fd);
    mcSync(0, NULL, &ret);

    return total;
}

int mc_driver_delete(int port, const char *name, int is_dir)
{
    int ret;
    char path[70];

    if (!is_dir) {
        snprintf(path, sizeof(path), "/%s", name);
        mcDelete(port, 0, path);
        mcSync(0, NULL, &ret);
        return ret;
    }

    /* Directories must be emptied before they can be removed. */
    char glob_path[70];
    snprintf(glob_path, sizeof(glob_path), "/%s/*", name);

    mcGetDir(port, 0, glob_path, 0, MC_DIR_MAX_ENTRIES, mc_dir_entries);
    mcSync(0, NULL, &ret);
    if (ret < 0) {
        return ret;
    }

    int count = ret;
    for (int i = 0; i < count; i++) {
        /* A subdirectory listing always starts with the "." and ".." links,
           which are not deletable: mcDelete fails on them and would abort the
           whole operation, leaving the directory non-empty and undeletable.
           Saves are flat, so skipping every subdir entry covers both. */
        if (mc_dir_entries[i].AttrFile & MC_ATTR_SUBDIR) {
            continue;
        }
        snprintf(path, sizeof(path), "/%s/%.32s", name, mc_dir_entries[i].EntryName);
        mcDelete(port, 0, path);
        mcSync(0, NULL, &ret);
        if (ret < 0) {
            return ret;
        }
    }

    snprintf(path, sizeof(path), "/%s", name);
    mcDelete(port, 0, path);
    mcSync(0, NULL, &ret);
    return ret;
}
