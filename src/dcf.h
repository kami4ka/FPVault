/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dcf.h - DCF (JEITA CP-3461) clip naming with per-boot session directories.
 *
 *   /DCIM/100FCDVR/FCDV0001.AVI
 *   /DCIM/100FCDVR/FCDV0002.AVI     <- clips/segments, monotonic
 *   /DCIM/101FCDVR/FCDV0003.AVI     <- next power-on = next directory
 *
 * Rules (chosen deliberately, see docs/ARCHITECTURE.md):
 *   - 8.3 uppercase only (FF_USE_LFN stays 0; DCF requires it anyway);
 *   - the 4-digit file index is monotonic across the WHOLE card and is never
 *     reused, even after deletions - so two clips can never share a name;
 *   - a new session directory per power-up (DCF dir numbers 100..999) groups
 *     clips by flying session without needing an RTC;
 *   - boot-time scan of the card is the authority: no counter file to trust
 *     or corrupt. Self-heals after card swaps and manual deletions.
 *
 * Edge policy: file index past 9999 or directory past 999 -> DCF_FULL; the
 * recorder reports "card needs a cleanup/reformat" rather than overwriting.
 *
 * Filesystem access is injected (vtable) so host unit tests run against a
 * fake directory tree; the target glue wraps FatFs f_opendir/f_readdir.
 */
#pragma once

#include <stdint.h>

#define DCF_PREFIX "FCDV"  /* 4 chars, file names FCDV0001..FCDV9999 */
#define DCF_DIRTAG "FCDVR" /* 5 chars, directories NNNFCDVR */
#define DCF_EXT    "AVI"

#define DCF_OK    0
#define DCF_FULL  (-1) /* directory 999 or index 9999 exhausted */
#define DCF_IOERR (-2)

typedef struct dcf_ops {
    void* ctx;
    /* Call cb(arg, name, is_dir) for every entry in path ("/DCIM" or
     * "/DCIM/100FCDVR"). Names are bare 8.3, no path, uppercase. Returns 0,
     * or negative on I/O error. A missing directory is NOT an error: report
     * 0 entries. */
    int (*list_dir)(void* ctx, const char* path,
                    void (*cb)(void* arg, const char* name, int is_dir), void* arg);
    /* mkdir -p semantics for one level; 0 on success or already-exists. */
    int (*make_dir)(void* ctx, const char* path);
} dcf_ops_t;

typedef struct dcf {
    const dcf_ops_t* ops;
    uint16_t session_dir;  /* NNN of this session's directory, 100..999 */
    uint16_t next_index;   /* next 4-digit file index, 1..9999 */
    uint8_t  session_made; /* directory created lazily on first clip */
} dcf_t;

/* Scan /DCIM: find the highest NNN<DIRTAG> directory and the highest
 * <PREFIX>#### index anywhere in those directories; plan this session as
 * (highest dir + 1, highest index + 1). Fresh card: dir 100, index 1. */
int dcf_boot_scan(dcf_t* d, const dcf_ops_t* ops);

/* Produce the next clip path, creating /DCIM/NNNFCDVR on first use.
 * out must hold at least DCF_PATH_MAX bytes. Advances next_index. */
#define DCF_PATH_MAX 32
int dcf_next_clip(dcf_t* d, char out[DCF_PATH_MAX]);
