/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * sdtest.c - see sdtest.h.
 *
 * The scrub exists because of a real incident: this card's previous life
 * left an eGON boot blob at the 8 KB offset, and the F1C200s BROM boots
 * SD BEFORE SPI-NOR - the board hung in foreign code before U-Boot ever
 * ran. A FAT reformat does not touch that region (it sits between the MBR
 * and the first partition), so any repurposed card can carry the hazard.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "sdtest.h"
#include "ff.h"
#include "sdcard.h"
#include "f1c100s_timer.h"

extern sdcard_t* disk_card(void);
extern uint32_t disk_toggle_width(void);

static FATFS fs;
static int mounted = 0;

int sdtest_is_mounted(void) {
    return mounted;
}

void sdtest_toggle_width(void) {
    uint32_t w = disk_toggle_width();
    mounted = 0;
    printf("[sd] next mount will use %s-bit - press M to remount\r\n",
           (w == (1u << 2)) ? "4" : "1");
}

#define BENCH_BUF ((uint8_t*)0x83400000u)
#define CHUNK (256u * 1024u)
#define CHUNKS 256u /* 64 MB */

void sdtest_mount(void) {
    FRESULT fr;
    DWORD free_clu;
    FATFS* pfs = &fs;
    sdcard_t* c = disk_card();

    memset(&fs, 0, sizeof(fs));
    fr = f_mount(&fs, "", 1);
    if(fr != FR_OK) {
        printf("[sd] mount FAILED fr=%d\r\n", fr);
        mounted = 0;
        return;
    }
    mounted = 1;
    printf("[sd] mounted. card: %lu MB, bus width %s, clk %lu Hz, hc=%lu\r\n",
           (unsigned long)(c->capacity >> 20),
           (c->width == (1u << 2)) ? "4" : "1", (unsigned long)c->clock,
           (unsigned long)c->high_capacity);
    if(f_getfree("", &free_clu, &pfs) == FR_OK)
        printf("     fs type %u, free %lu MB\r\n", fs.fs_type,
               (unsigned long)((uint64_t)free_clu * fs.csize * 512u >> 20));
}

void sdtest_scrub(void) {
    static uint8_t sec[512];
    sdcard_t* c = disk_card();
    uint32_t part_start, i;

    if(!mounted) {
        printf("[sd] mount first (M)\r\n");
        return;
    }
    if(sdcard_read(c, sec, 0, 1) != 1) {
        printf("[sd] MBR read failed\r\n");
        return;
    }
    /* Partition entry 0 LBA start (little-endian at 0x1C6). If the card is
     * superfloppy (no MBR) this reads garbage - require a sane value. */
    part_start = (uint32_t)sec[0x1C6] | ((uint32_t)sec[0x1C7] << 8) |
                 ((uint32_t)sec[0x1C8] << 16) | ((uint32_t)sec[0x1C9] << 24);
    printf("[sd] partition 0 starts at LBA %lu\r\n", (unsigned long)part_start);
    if(part_start < 48) {
        printf("[sd] no gap before partition - nothing to scrub safely\r\n");
        return;
    }
    if(sdcard_read(c, sec, 16, 1) != 1) {
        printf("[sd] sector 16 read failed\r\n");
        return;
    }
    if(memcmp(sec + 4, "eGON", 4) == 0)
        printf("[sd] STALE BOOT BLOB FOUND (eGON at sector 16) - scrubbing\r\n");
    else
        printf("[sd] no eGON signature at sector 16 - scrubbing the region anyway\r\n");
    memset(sec, 0, sizeof(sec));
    for(i = 16; i < 48; i++) {
        if(sdcard_write(c, sec, i, 1) != 1) {
            printf("[sd] scrub write failed at sector %lu\r\n", (unsigned long)i);
            return;
        }
    }
    if(sdcard_read(c, sec, 16, 1) == 1 && memcmp(sec + 4, "eGON", 4) != 0)
        printf("[sd] scrubbed sectors 16..47 - the card can no longer hijack boot\r\n");
}

static uint32_t chunk_sum(const uint8_t* p) {
    /* word-sum: fast, catches the transfer corruption we care about */
    const uint32_t* w = (const uint32_t*)p;
    uint32_t s = 0, i;
    for(i = 0; i < CHUNK / 4; i++)
        s += w[i];
    return s;
}

void sdtest_benchmark(void) {
    static uint32_t sums[CHUNKS];
    static const uint32_t bucket_us[] = {2000, 5000, 10000, 20000, 50000,
                                         100000, 200000, 500000};
    uint32_t hist[9] = {0};
    FIL f;
    FRESULT fr;
    UINT bw;
    uint32_t i, j, t0, us, max_us = 0;
    uint64_t total_us = 0;
    uint32_t seed = 0x12345678;

    if(!mounted) {
        printf("[sd] mount first (M)\r\n");
        return;
    }

    /* deterministic pseudo-random payload - incompressible-ish, repeatable */
    for(i = 0; i < CHUNK / 4; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        ((uint32_t*)BENCH_BUF)[i] = seed;
    }

    fr = f_open(&f, "BENCH.BIN", FA_CREATE_ALWAYS | FA_WRITE);
    if(fr != FR_OK) {
        printf("[sd] open failed fr=%d\r\n", fr);
        return;
    }
    printf("[sd] writing %lu MB in %lu KB chunks...\r\n",
           (unsigned long)(CHUNKS * CHUNK >> 20), (unsigned long)(CHUNK >> 10));

    for(i = 0; i < CHUNKS; i++) {
        wdg_feed();
        /* vary the first word per chunk so every chunk sum differs */
        ((uint32_t*)BENCH_BUF)[0] = i * 0x9E3779B9u;
        sums[i] = chunk_sum(BENCH_BUF);
        t0 = tim_get_cnt(TIM0);
        fr = f_write(&f, BENCH_BUF, CHUNK, &bw);
        us = (uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u;
        if(fr != FR_OK || bw != CHUNK) {
            printf("[sd] write failed at chunk %lu fr=%d\r\n", (unsigned long)i, fr);
            f_close(&f);
            return;
        }
        total_us += us;
        if(us > max_us) max_us = us;
        for(j = 0; j < 8 && us >= bucket_us[j]; j++)
            ;
        hist[j]++;
        if((i & 31) == 31) printf("  %lu/%lu\r\n", (unsigned long)(i + 1),
                                  (unsigned long)CHUNKS);
    }
    t0 = tim_get_cnt(TIM0);
    f_sync(&f);
    us = (uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u;
    f_close(&f);

    printf("[sd] WRITE: %lu KB/s avg, max chunk %lu us, f_sync %lu us\r\n",
           (unsigned long)((uint64_t)CHUNKS * CHUNK * 1000u / (total_us ? total_us : 1)),
           (unsigned long)max_us, (unsigned long)us);
    printf("     latency histogram (<2ms <5 <10 <20 <50 <100 <200 <500 >=500):\r\n    ");
    for(j = 0; j < 9; j++)
        printf(" %lu", (unsigned long)hist[j]);
    printf("\r\n");

    /* read-back verify */
    fr = f_open(&f, "BENCH.BIN", FA_READ);
    if(fr != FR_OK) {
        printf("[sd] reopen failed fr=%d\r\n", fr);
        return;
    }
    total_us = 0;
    for(i = 0; i < CHUNKS; i++) {
        wdg_feed();
        t0 = tim_get_cnt(TIM0);
        fr = f_read(&f, BENCH_BUF, CHUNK, &bw);
        total_us += (uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u;
        if(fr != FR_OK || bw != CHUNK || chunk_sum(BENCH_BUF) != sums[i]) {
            printf("[sd] VERIFY FAILED at chunk %lu (fr=%d bw=%u)\r\n",
                   (unsigned long)i, fr, bw);
            f_close(&f);
            return;
        }
    }
    f_close(&f);
    printf("[sd] READ: %lu KB/s avg, all %lu chunks verified\r\n",
           (unsigned long)((uint64_t)CHUNKS * CHUNK * 1000u / (total_us ? total_us : 1)),
           (unsigned long)CHUNKS);
    f_unlink("BENCH.BIN");
}
