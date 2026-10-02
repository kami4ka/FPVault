/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dlog.c - see dlog.h. The part that knows about this board: FatFs to find
 * the file, the SD driver to write it, and interrupts to keep out of the way
 * of. What goes where is decided in dlog_core.c.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include "board.h"
#include "dlog.h"
#include "dlog_core.h"
#include "ff.h"
#include "sdcard.h"
#include "sdtest.h"
#include "armv5_cache.h"
#include "f1c100s_intc.h"
#include "f1c100s_timer.h"

extern sdcard_t* disk_card(void);
extern int disk_raw_init(void);
extern int sdtest_is_mounted(void);
extern void sdtest_unmount(void);

/* One line each, from the modules that own the numbers. */
extern void pipeline_dlog(void);
extern void usbuvc_dlog(void);
extern void usbmsc_dlog(void);
extern void usbuvc_dlog_rate(void);

#define RING ((dlog_ring_t*)DLOG_RING_BASE)

static dlog_t d;
/* Time comes from the free-running 24 MHz timer, not from counting the 1 kHz
 * tick: a card read served inside the USB interrupt holds every other
 * interrupt off for milliseconds, ticks are lost, and a clock made of them
 * ran at 60% speed exactly when the log was most wanted. The tick only has
 * to take a sample often enough that the 32-bit count (179 s) cannot wrap
 * unseen. */
static volatile uint32_t ms = 0;
static uint32_t clk_last = 0, clk_rem = 0;
static uint8_t clk_on = 0;

/* Interrupts masked by the caller. */
static void clock_sample(void) {
    uint32_t now;
    if(!clk_on) return;
    now = tim_get_cnt(TIM0);
    clk_rem += (uint32_t)(clk_last - now); /* counts down */
    clk_last = now;
    ms += clk_rem / (TICKS_PER_SEC / 1000u);
    clk_rem %= (TICKS_PER_SEC / 1000u);
}
static uint32_t last_write_ms = 0;
/* Set once the USB interrupt exists. Mass storage runs inside it, so a card
 * command issued from the main loop has to keep it out for its duration. */
static uint8_t usb_up = 0;

/* ---- interrupts ------------------------------------------------------------
 * Save and restore rather than disable and enable: DLOG() is called from
 * interrupt handlers too, and re-enabling on the way out of one would let it
 * be interrupted by itself. */
static inline uint32_t irq_save(void) {
    uint32_t old, tmp;
    __asm__ __volatile__("mrs %0, cpsr\n"
                         "orr %1, %0, #0x80\n"
                         "msr cpsr_c, %1"
                         : "=&r"(old), "=&r"(tmp));
    return old;
}
static inline void irq_restore(uint32_t old) {
    __asm__ __volatile__("msr cpsr_c, %0" : : "r"(old));
}

/* ---- ring ---------------------------------------------------------------- */
/* The ring is in cacheable memory and a reset does not write the cache back.
 * What is wanted after a reset is exactly the last lines before it, so each
 * append is pushed through to DRAM as it happens. */
static void ring_sync(uint32_t from, uint32_t n) {
    uint32_t base = (uint32_t)RING->data, a = from & (DLOG_RING_SIZE - 1u);
    cache_clean_range((uint32_t)RING, base);
    if(a + n <= DLOG_RING_SIZE) {
        cache_clean_range(base + a, base + a + n);
    } else {
        cache_clean_range(base + a, base + DLOG_RING_SIZE);
        cache_clean_range(base, base + (a + n - DLOG_RING_SIZE));
    }
}

void dlog_printf(const char* fmt, ...) {
    char line[DLOG_LINE_MAX];
    va_list ap;
    uint32_t n, at, flags, t;

    flags = irq_save();
    clock_sample();
    t = ms;
    irq_restore(flags);

    va_start(ap, fmt);
    n = dlog_format(line, sizeof line, t, fmt, ap);
    va_end(ap);

    flags = irq_save();
    at = RING->head;
    dlog_ring_put(RING, (const uint8_t*)line, n);
    irq_restore(flags);
    ring_sync(at, n);
}

/* From the tick interrupt, so interrupts are already masked. */
void dlog_tick(void) {
    clock_sample();
}

uint32_t dlog_now_ms(void) {
    uint32_t flags = irq_save(), t;
    clock_sample();
    t = ms;
    irq_restore(flags);
    return t;
}

void dlog_clock_start(void) {
    uint32_t flags = irq_save();
    clk_last = tim_get_cnt(TIM0);
    clk_on = 1;
    irq_restore(flags);
}

void dlog_boot(void) {
    uint32_t held = dlog_ring_recover(RING);
    ring_sync(0, 0);
    if(held) DLOG("--- %lu bytes above were still unwritten when the board reset ---",
                  (unsigned long)held);
}

/* ---- card ---------------------------------------------------------------- */
static int io_rd(void* ctx, uint32_t lba, uint8_t* buf, uint32_t n) {
    uint64_t got;
    (void)ctx;
    if(usb_up) intc_disable_irq(IRQ_USBOTG);
    got = sdcard_read(disk_card(), buf, lba, n);
    if(usb_up) intc_enable_irq(IRQ_USBOTG);
    return got == n ? 0 : -1;
}

/* The longest the log has held the USB interrupt off, for the per-second
 * line: the log is not free, and its cost has to be visible in itself. */
static uint32_t hold_max_ticks = 0;

uint32_t dlog_hold_max_ms(void) {
    uint32_t m = hold_max_ticks / (TICKS_PER_SEC / 1000u);
    hold_max_ticks = 0;
    return m;
}

static int io_wr(void* ctx, uint32_t lba, const uint8_t* buf, uint32_t n) {
    uint64_t put = 0;
    uint32_t t0, held;
    (void)ctx;
    if(!usb_up) {
        /* Preparing the file, before the main loop exists to feed the
         * watchdog: clearing 4 MB on a slow card takes seconds. */
        wdg_feed();
        return sdcard_write(disk_card(), (uint8_t*)buf, lba, n) == n ? 0 : -1;
    }
    t0 = tim_get_cnt(TIM0);
    intc_disable_irq(IRQ_USBOTG);
    /* Looked at with the host kept out: had it just claimed this space, a
     * write decided on a moment ago would land in its new file. */
    if(!d.abandoned) put = sdcard_write(disk_card(), (uint8_t*)buf, lba, n);
    intc_enable_irq(IRQ_USBOTG);
    held = (uint32_t)(t0 - tim_get_cnt(TIM0));
    if(held > hold_max_ticks) hold_max_ticks = held;
    return put == n ? 0 : -1;
}

static const dlog_io_t io = {io_rd, io_wr, 0};

/* From the FatFs manual's notes on f_expand: walk the file a cluster at a
 * time and see that each follows the one before. */
static int contiguous(FIL* fp) {
    DWORD clst, clsz, step;
    FSIZE_t left = f_size(fp);

    if(f_rewind(fp) != FR_OK || left == 0) return 0;
    clsz = (DWORD)fp->obj.fs->csize * fp->obj.fs->ssize;
    clst = fp->obj.sclust - 1;
    while(left) {
        step = (left >= clsz) ? clsz : (DWORD)left;
        if(f_lseek(fp, f_tell(fp) + step) != FR_OK) return 0;
        if(clst + 1 != fp->clust) return 0;
        clst = fp->clust;
        left -= step;
    }
    return 1;
}

void dlog_prepare(void) {
    FIL fp;
    FRESULT fr;
    uint32_t lba0 = 0;
    int fresh = 0, ok = 0;

    if(disk_raw_init() != 0) {
        printf("[dlog] no card - nothing will be logged\r\n");
        return;
    }
    sdtest_mount();
    if(!sdtest_is_mounted()) {
        printf("[dlog] card will not mount - nothing will be logged\r\n");
        return;
    }

    fr = f_open(&fp, DLOG_FILE_NAME, FA_READ | FA_WRITE);
    if(fr == FR_OK) {
        if(f_size(&fp) == DLOG_FILE_SIZE && fp.obj.fs->ssize == DLOG_SECTOR &&
           contiguous(&fp)) {
            ok = 1;
        } else {
            /* Not a file this firmware made, or no longer in one piece. */
            f_close(&fp);
            f_unlink(DLOG_FILE_NAME);
        }
    }
    if(!ok) {
        fr = f_open(&fp, DLOG_FILE_NAME, FA_CREATE_ALWAYS | FA_READ | FA_WRITE);
        if(fr == FR_OK) {
            /* The 1 asks for the space now, in one piece, or not at all. */
            if(f_expand(&fp, DLOG_FILE_SIZE, 1) == FR_OK &&
               fp.obj.fs->ssize == DLOG_SECTOR) {
                ok = fresh = 1;
            } else {
                f_close(&fp);
                f_unlink(DLOG_FILE_NAME);
                printf("[dlog] no %lu MB of unbroken free space on the card\r\n",
                       (unsigned long)(DLOG_FILE_SIZE >> 20));
            }
        }
    }
    if(ok) {
        lba0 = (uint32_t)(fp.obj.fs->database +
                          (LBA_t)fp.obj.fs->csize * (fp.obj.sclust - 2));
        /* Closed before its sectors are written directly, so FatFs holds no
         * copy of any of them. */
        f_close(&fp);
        if(dlog_core_open(&d, RING, &io, lba0, DLOG_FILE_SIZE / DLOG_SECTOR, fresh) != 0) {
            printf("[dlog] could not read or clear the log file\r\n");
            d.ready = 0;
        }
    }
    sdtest_unmount();

    if(d.ready)
        printf("[dlog] %s %s at sector %lu, continuing at %lu.%03lu of %lu sectors\r\n",
               DLOG_FILE_NAME, fresh ? "created" : "found", (unsigned long)lba0,
               (unsigned long)d.sec, (unsigned long)d.fill,
               (unsigned long)d.nsec);
}

void dlog_activate(void) {
    usb_up = 1;
    if(!d.ready || d.abandoned) return;
    d.active = 1;
    last_write_ms = ms;
    DLOG("log active, sector %lu of %lu", (unsigned long)d.sec, (unsigned long)d.nsec);
}

void dlog_host_wrote(uint32_t sector, uint32_t count) {
    uint8_t was = d.abandoned;
    dlog_core_host_wrote(&d, sector, count);
    if(d.abandoned && !was)
        printf("[dlog] the host wrote into the log file's space - logging stopped\r\n");
}

void dlog_stats(void) {
    pipeline_dlog();
    usbuvc_dlog();
    usbmsc_dlog();
    DLOG("log sector=%lu/%lu errors=%lu%s%s", (unsigned long)d.sec,
         (unsigned long)d.nsec, (unsigned long)d.wr_err, d.full ? " FULL" : "",
         d.abandoned ? " ABANDONED" : "");
}

/* Once a second. While the camera streams, one line of what happened in that
 * second - enough to see which side stopped first when a picture freezes.
 * The counters behind it run whether or not it is printed, so the first
 * line of a stream covers one second and not everything since boot. */
void dlog_second(uint32_t uptime_s) {
    usbuvc_dlog_rate();
    if((uptime_s % 5u) == 0u) dlog_stats();
}

void dlog_poll(void) {
    uint32_t pending, lost, flags;

    if(!d.active) return;

    /* Say how much was dropped, once there is room to say it. */
    if(RING->lost) {
        flags = irq_save();
        lost = RING->lost;
        RING->lost = 0;
        irq_restore(flags);
        DLOG("*** %lu lines lost: the log could not keep up ***", (unsigned long)lost);
    }

    pending = dlog_ring_pending(RING) - d.uncommitted;
    if(pending >= DLOG_SECTOR - d.fill) {
        /* A few at a time: the main loop has a stream to feed. */
        if(dlog_core_flush(&d, 0, 4)) last_write_ms = ms;
    } else if((pending || d.dirty) && ms - last_write_ms >= 1000u) {
        dlog_core_flush(&d, 1, 1);
        last_write_ms = ms;
    }
}
