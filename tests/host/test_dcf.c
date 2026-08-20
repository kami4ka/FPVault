/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_dcf.c - host-side unit test for the DCF clip-naming module.
 *
 * dcf.c never touches a filesystem directly - it goes through the injected
 * dcf_ops_t vtable - so the whole test runs against a fake directory tree:
 * a static table of directories, each with a fixed list of (name, is_dir)
 * entries. list_dir walks the table; make_dir records every call so the
 * tests can assert the session directory is created lazily and exactly once.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dcf.h"

/* ---- fake filesystem ---------------------------------------------------- */

#define FS_MAX_DIRS    8
#define FS_MAX_ENTRIES 10
#define FS_MAX_MADE    8

typedef struct {
    const char* name; /* bare 8.3, uppercase (contract of dcf_ops.list_dir) */
    int is_dir;
} fent_t;

typedef struct {
    const char* path; /* NULL path terminates the table */
    fent_t ents[FS_MAX_ENTRIES]; /* NULL name terminates the list */
} fdir_t;

typedef struct {
    fdir_t dirs[FS_MAX_DIRS];
    char made[FS_MAX_MADE][DCF_PATH_MAX]; /* every make_dir call, in order */
    int nmade;
    int list_err; /* nonzero: list_dir reports an I/O error */
} ffs_t;

static int fs_list_dir(void* ctx, const char* path,
                       void (*cb)(void* arg, const char* name, int is_dir),
                       void* arg) {
    ffs_t* fs = ctx;
    if(fs->list_err)
        return -1;
    for(int i = 0; i < FS_MAX_DIRS && fs->dirs[i].path; i++) {
        if(strcmp(fs->dirs[i].path, path))
            continue;
        for(int j = 0; j < FS_MAX_ENTRIES && fs->dirs[i].ents[j].name; j++)
            cb(arg, fs->dirs[i].ents[j].name, fs->dirs[i].ents[j].is_dir);
        return 0;
    }
    return 0; /* missing directory is not an error: 0 entries (see dcf.h) */
}

static int fs_make_dir(void* ctx, const char* path) {
    ffs_t* fs = ctx;
    assert(fs->nmade < FS_MAX_MADE);
    assert(strlen(path) < DCF_PATH_MAX);
    strcpy(fs->made[fs->nmade++], path);
    return 0;
}

static int fs_made_count(const ffs_t* fs, const char* path) {
    int n = 0;
    for(int i = 0; i < fs->nmade; i++)
        if(!strcmp(fs->made[i], path))
            n++;
    return n;
}

static dcf_ops_t fs_ops(ffs_t* fs) {
    dcf_ops_t ops = {fs, fs_list_dir, fs_make_dir};
    return ops;
}

/* ---- cases -------------------------------------------------------------- */

/* Fresh card: no /DCIM at all. Session 100, index 1, and the two mkdirs
 * happen exactly once even across several clips. */
static void case_fresh_card(void) {
    ffs_t fs = {{{0}}, {{0}}, 0, 0};
    dcf_ops_t ops = fs_ops(&fs);
    dcf_t d;
    char path[DCF_PATH_MAX];

    assert(dcf_boot_scan(&d, &ops) == DCF_OK);
    assert(d.session_dir == 100);
    assert(d.next_index == 1);
    assert(d.session_made == 0);
    assert(fs.nmade == 0); /* nothing created before the first clip */

    assert(dcf_next_clip(&d, path) == DCF_OK);
    assert(!strcmp(path, "/DCIM/100FCDVR/FCDV0001.AVI"));
    assert(dcf_next_clip(&d, path) == DCF_OK);
    assert(!strcmp(path, "/DCIM/100FCDVR/FCDV0002.AVI"));

    assert(fs.nmade == 2);
    assert(fs_made_count(&fs, "/DCIM") == 1);
    assert(fs_made_count(&fs, "/DCIM/100FCDVR") == 1);
    printf("  fresh_card: OK\n");
}

/* Existing card with decoys: only NNNFCDVR directories count, and only
 * their FCDV####.AVI files count. 100MEDIA holds FCDV9999.AVI and 99FCDVR
 * holds FCDV5000.AVI - both must be invisible because the DIRECTORY name
 * does not match. "105FCDVR" as a plain file must not bump the session. */
static void case_existing_with_decoys(void) {
    ffs_t fs = {
        {
            {"/DCIM",
             {{"100FCDVR", 1},
              {"103FCDVR", 1},
              {"100MEDIA", 1},  /* wrong tag */
              {"99FCDVR", 1},   /* 2 digits, malformed */
              {"1000FCDVR", 1}, /* 4 digits, malformed */
              {"10AFCDVR", 1},  /* non-digit, malformed */
              {"STRAY.TXT", 0}, /* file in /DCIM, ignored */
              {"105FCDVR", 0},  /* dir-shaped NAME but a file, ignored */
              {0, 0}}},
            {"/DCIM/100FCDVR", {{"FCDV0001.AVI", 0}, {"FCDV0007.AVI", 0}, {0, 0}}},
            {"/DCIM/103FCDVR", {{"FCDV0100.AVI", 0}, {0, 0}}},
            {"/DCIM/100MEDIA", {{"FCDV9999.AVI", 0}, {0, 0}}},
            {"/DCIM/99FCDVR", {{"FCDV5000.AVI", 0}, {0, 0}}},
            {0, {{0, 0}}},
        },
        {{0}},
        0,
        0};
    dcf_ops_t ops = fs_ops(&fs);
    dcf_t d;
    char path[DCF_PATH_MAX];

    assert(dcf_boot_scan(&d, &ops) == DCF_OK);
    assert(d.session_dir == 104);
    assert(d.next_index == 101);

    assert(dcf_next_clip(&d, path) == DCF_OK);
    assert(!strcmp(path, "/DCIM/104FCDVR/FCDV0101.AVI"));
    printf("  existing_with_decoys: OK\n");
}

/* Deletions can leave the max index in an OLDER directory than the max
 * directory - every matching directory must be scanned, not just the
 * highest one. */
static void case_max_index_in_older_dir(void) {
    ffs_t fs = {
        {
            {"/DCIM", {{"101FCDVR", 1}, {"105FCDVR", 1}, {0, 0}}},
            {"/DCIM/101FCDVR", {{"FCDV0200.AVI", 0}, {0, 0}}},
            {"/DCIM/105FCDVR", {{"FCDV0002.AVI", 0}, {0, 0}}},
            {0, {{0, 0}}},
        },
        {{0}},
        0,
        0};
    dcf_ops_t ops = fs_ops(&fs);
    dcf_t d;
    char path[DCF_PATH_MAX];

    assert(dcf_boot_scan(&d, &ops) == DCF_OK);
    assert(d.session_dir == 106);
    assert(d.next_index == 201);

    assert(dcf_next_clip(&d, path) == DCF_OK);
    assert(!strcmp(path, "/DCIM/106FCDVR/FCDV0201.AVI"));
    printf("  max_index_in_older_dir: OK\n");
}

/* Malformed file names inside a matching directory are ignored: wrong digit
 * count, non-digits, wrong prefix, wrong extension, lowercase. */
static void case_malformed_filenames(void) {
    ffs_t fs = {
        {
            {"/DCIM", {{"100FCDVR", 1}, {0, 0}}},
            {"/DCIM/100FCDVR",
             {{"FCDV12.AVI", 0},   /* 2 digits */
              {"FCDVABCD.AVI", 0}, /* non-digits */
              {"XCDV0500.AVI", 0}, /* wrong prefix */
              {"FCDV0500.MP4", 0}, /* wrong extension */
              {"fcdv0900.avi", 0}, /* lowercase */
              {"FCDV0033.AVI", 0}, /* the only valid one */
              {0, 0}}},
            {0, {{0, 0}}},
        },
        {{0}},
        0,
        0};
    dcf_ops_t ops = fs_ops(&fs);
    dcf_t d;
    char path[DCF_PATH_MAX];

    assert(dcf_boot_scan(&d, &ops) == DCF_OK);
    assert(d.next_index == 34);
    assert(d.session_dir == 101);

    assert(dcf_next_clip(&d, path) == DCF_OK);
    assert(!strcmp(path, "/DCIM/101FCDVR/FCDV0034.AVI"));
    printf("  malformed_filenames: OK\n");
}

/* Directory 999 already on the card: no next session possible, per the
 * dcf.h edge policy the scan reports DCF_FULL and the planner refuses to
 * hand out clips (struct stays deterministic but unusable). */
static void case_dir_exhausted(void) {
    ffs_t fs = {
        {
            {"/DCIM", {{"999FCDVR", 1}, {0, 0}}},
            {0, {{0, 0}}},
        },
        {{0}},
        0,
        0};
    dcf_ops_t ops = fs_ops(&fs);
    dcf_t d;
    char path[DCF_PATH_MAX];

    assert(dcf_boot_scan(&d, &ops) == DCF_FULL);
    assert(dcf_next_clip(&d, path) == DCF_FULL);
    assert(fs.nmade == 0); /* nothing created for a full card */
    printf("  dir_exhausted: OK\n");
}

/* Index exhaustion: 9999 is the last usable index, the call after it must
 * report DCF_FULL and keep reporting it. */
static void case_index_exhausted(void) {
    ffs_t fs = {{{0}}, {{0}}, 0, 0};
    dcf_ops_t ops = fs_ops(&fs);
    dcf_t d;
    char path[DCF_PATH_MAX];

    assert(dcf_boot_scan(&d, &ops) == DCF_OK);
    d.next_index = 9999; /* fast-forward to the edge */

    assert(dcf_next_clip(&d, path) == DCF_OK);
    assert(!strcmp(path, "/DCIM/100FCDVR/FCDV9999.AVI"));
    assert(dcf_next_clip(&d, path) == DCF_FULL);
    assert(dcf_next_clip(&d, path) == DCF_FULL); /* stays full */
    printf("  index_exhausted: OK\n");
}

/* Consecutive clips: monotonic indices, same session directory. */
static void case_sequence(void) {
    ffs_t fs = {{{0}}, {{0}}, 0, 0};
    dcf_ops_t ops = fs_ops(&fs);
    dcf_t d;
    char p1[DCF_PATH_MAX], p2[DCF_PATH_MAX], p3[DCF_PATH_MAX];

    assert(dcf_boot_scan(&d, &ops) == DCF_OK);
    assert(dcf_next_clip(&d, p1) == DCF_OK);
    assert(dcf_next_clip(&d, p2) == DCF_OK);
    assert(dcf_next_clip(&d, p3) == DCF_OK);
    assert(!strcmp(p1, "/DCIM/100FCDVR/FCDV0001.AVI"));
    assert(!strcmp(p2, "/DCIM/100FCDVR/FCDV0002.AVI"));
    assert(!strcmp(p3, "/DCIM/100FCDVR/FCDV0003.AVI"));
    assert(!strncmp(p1, p2, 15) && !strncmp(p2, p3, 15)); /* same dir part */
    printf("  sequence: OK\n");
}

/* An I/O error from list_dir must surface as DCF_IOERR, not as an empty
 * card (which would silently restart numbering and risk name collisions). */
static void case_list_dir_error(void) {
    ffs_t fs = {{{0}}, {{0}}, 0, 1 /* list_err */};
    dcf_ops_t ops = fs_ops(&fs);
    dcf_t d;

    assert(dcf_boot_scan(&d, &ops) == DCF_IOERR);
    printf("  list_dir_error: OK\n");
}

int main(void) {
    case_fresh_card();
    case_existing_with_decoys();
    case_max_index_in_older_dir();
    case_malformed_filenames();
    case_dir_exhausted();
    case_index_exhausted();
    case_sequence();
    case_list_dir_error();
    printf("test_dcf: all cases pass\n");
    return 0;
}
