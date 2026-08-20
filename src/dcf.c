/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dcf.c - DCF (JEITA CP-3461) clip naming; policy and rationale in dcf.h.
 *
 * Bare-metal constraints (ARM926, no libc beyond string.h): no stdio, no
 * malloc. Paths are assembled with two tiny helpers - a string append and a
 * fixed-width zero-padded decimal - instead of snprintf.
 *
 * The boot scan is two-pass on purpose. Pass one lists /DCIM once and marks
 * every matching NNNFCDVR directory in a bitmap (900 possible numbers ->
 * 113 bytes, cheap). Pass two lists each marked directory for the highest
 * FCDV#### index. Marking first means list_dir is never re-entered from
 * inside its own callback, so the FatFs glue can keep a single DIR/FILINFO
 * on its stack and the host fake stays trivial. Every matching directory is
 * scanned - not just the highest - because deletions can leave the maximum
 * file index in an older directory, and the index must never be reused.
 */
#include <stdint.h>
#include <string.h>
#include "dcf.h"

#define DIR_MIN 100
#define DIR_MAX 999
#define IDX_MAX 9999

#define PFX_LEN (sizeof(DCF_PREFIX) - 1) /* 4 */
#define TAG_LEN (sizeof(DCF_DIRTAG) - 1) /* 5 */
#define EXT_LEN (sizeof(DCF_EXT) - 1)    /* 3 */

/* ---- formatting helpers (no snprintf on target) ------------------------- */

static char* put_str(char* p, const char* s) {
    while(*s)
        *p++ = *s++;
    return p;
}

/* Fixed-width zero-padded decimal, exactly `width` chars, MSD first. */
static char* put_dec(char* p, uint16_t v, int width) {
    for(int i = width - 1; i >= 0; i--) {
        p[i] = (char)('0' + v % 10);
        v /= 10;
    }
    return p + width;
}

/* "/DCIM/NNNFCDVR" - shared by the scan and by clip-path building. */
static char* put_session_dir(char* p, uint16_t nnn) {
    p = put_str(p, "/DCIM/");
    p = put_dec(p, nnn, 3);
    p = put_str(p, DCF_DIRTAG);
    *p = '\0';
    return p;
}

/* ---- name parsing ------------------------------------------------------- */

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* "NNNFCDVR", exactly 8 chars, NNN in 100..999. Returns NNN or -1.
 * Anything else ("100MEDIA", "99FCDVR", "1000FCDVR", "10AFCDVR", files
 * named like directories) is simply not ours and is skipped. */
static int parse_dir_name(const char* n) {
    int v = 0;
    for(int i = 0; i < 3; i++) {
        if(!is_digit(n[i]))
            return -1;
        v = v * 10 + (n[i] - '0');
    }
    if(memcmp(n + 3, DCF_DIRTAG, TAG_LEN) != 0 || n[3 + TAG_LEN] != '\0')
        return -1;
    if(v < DIR_MIN) /* "099FCDVR": three digits but outside DCF range */
        return -1;
    return v;
}

/* "FCDV####.AVI", exactly 12 chars, uppercase. Returns #### or -1. */
static int parse_clip_name(const char* n) {
    int v = 0;
    if(memcmp(n, DCF_PREFIX, PFX_LEN) != 0)
        return -1;
    for(unsigned i = PFX_LEN; i < PFX_LEN + 4; i++) {
        if(!is_digit(n[i]))
            return -1;
        v = v * 10 + (n[i] - '0');
    }
    if(n[PFX_LEN + 4] != '.')
        return -1;
    if(memcmp(n + PFX_LEN + 5, DCF_EXT, EXT_LEN) != 0)
        return -1;
    if(n[PFX_LEN + 5 + EXT_LEN] != '\0')
        return -1;
    return v;
}

/* ---- boot scan ---------------------------------------------------------- */

typedef struct scan {
    uint32_t present[(DIR_MAX - DIR_MIN + 32) / 32]; /* bit per dir number */
    uint16_t max_dir; /* 0 = no matching directory found */
    uint16_t max_idx; /* 0 = no matching clip found */
} scan_t;

static void dcim_cb(void* arg, const char* name, int is_dir) {
    scan_t* s = arg;
    int nnn;
    if(!is_dir) /* a FILE named "105FCDVR" must not bump the session */
        return;
    nnn = parse_dir_name(name);
    if(nnn < 0)
        return;
    s->present[(nnn - DIR_MIN) / 32] |= 1u << ((nnn - DIR_MIN) % 32);
    if(nnn > s->max_dir)
        s->max_dir = (uint16_t)nnn;
}

static void clip_cb(void* arg, const char* name, int is_dir) {
    scan_t* s = arg;
    int idx;
    if(is_dir)
        return;
    idx = parse_clip_name(name);
    if(idx < 0)
        return;
    if(idx > (int)s->max_idx)
        s->max_idx = (uint16_t)idx;
}

int dcf_boot_scan(dcf_t* d, const dcf_ops_t* ops) {
    static scan_t s; /* ~130 bytes; static keeps it off the IRQ-shared stack */
    char path[DCF_PATH_MAX];

    memset(&s, 0, sizeof(s));
    d->ops = ops;
    d->session_made = 0;

    if(ops->list_dir(ops->ctx, "/DCIM", dcim_cb, &s) < 0)
        return DCF_IOERR;

    for(int nnn = DIR_MIN; nnn <= DIR_MAX; nnn++) {
        if(!(s.present[(nnn - DIR_MIN) / 32] & (1u << ((nnn - DIR_MIN) % 32))))
            continue;
        put_session_dir(path, (uint16_t)nnn);
        if(ops->list_dir(ops->ctx, path, clip_cb, &s) < 0)
            return DCF_IOERR;
    }

    d->next_index = (uint16_t)(s.max_idx + 1);
    d->session_dir = s.max_dir ? (uint16_t)(s.max_dir + 1) : DIR_MIN;

    /* Directory numbers exhausted: the struct stays deterministic (so a
     * state dump makes sense) but session_dir 1000 makes dcf_next_clip
     * refuse as well - belt and braces against a caller ignoring us. */
    if(d->session_dir > DIR_MAX)
        return DCF_FULL;
    return DCF_OK;
}

/* ---- clip production ---------------------------------------------------- */

int dcf_next_clip(dcf_t* d, char out[DCF_PATH_MAX]) {
    char* p;

    if(d->session_dir > DIR_MAX || d->next_index > IDX_MAX)
        return DCF_FULL;

    /* Lazy mkdir: a boot that never records leaves no empty directory on
     * the card. session_made is only set once both levels succeed, so a
     * transient I/O error is retried on the next clip. */
    if(!d->session_made) {
        char dir[DCF_PATH_MAX];
        if(d->ops->make_dir(d->ops->ctx, "/DCIM") < 0)
            return DCF_IOERR;
        put_session_dir(dir, d->session_dir);
        if(d->ops->make_dir(d->ops->ctx, dir) < 0)
            return DCF_IOERR;
        d->session_made = 1;
    }

    /* "/DCIM/NNNFCDVR/FCDV####.AVI" = 27 chars + NUL, fits DCF_PATH_MAX. */
    p = put_session_dir(out, d->session_dir);
    *p++ = '/';
    p = put_str(p, DCF_PREFIX);
    p = put_dec(p, d->next_index, 4);
    *p++ = '.';
    p = put_str(p, DCF_EXT);
    *p = '\0';

    d->next_index++;
    return DCF_OK;
}
