/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The card reader's bulk-only state machine, run against a fake USB port.
 * What a computer sees when the slot is empty, when it ejects the card, and
 * when a command fails: every failure must end in a status packet the host
 * can read, never in a stalled pipe with nothing behind it - that is the
 * difference between "no medium" and "disk abandoned".
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usbd_core.h"
#include "usbd_msc.h"
#include "usb_scsi.h"

#define OUT_EP 0x02
#define IN_EP 0x81

static int fails = 0;

#define CHECK(what, cond)                                   \
    do {                                                    \
        if(cond) printf("  ok   %s\n", what);               \
        else { printf("  FAIL %s\n", what); fails++; }      \
    } while(0)

/* ---- the fake port: records what the class asks of it ---- */

enum { EV_WRITE, EV_READ, EV_STALL };
typedef struct {
    int kind;
    uint8_t ep;
    uint32_t len;
    uint8_t data[64];
} event_t;

static event_t ev[32];
static int nev;
static uint8_t* cbw_buf;      /* where the class wants the next CBW */
static uint8_t* data_out_buf; /* where it wants WRITE payload */
static uint32_t data_out_len;
static uint32_t pending_in[8]; /* IN writes not yet "completed" */
static int npending;
static uint32_t cap_blocks = 0;
static int reads_called, writes_called;

static event_t* push(int kind, uint8_t ep, uint32_t len) {
    event_t* e = &ev[nev < 32 ? nev : 31];
    nev++;
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->ep = ep;
    e->len = len;
    return e;
}

void usbd_add_endpoint(uint8_t busid, struct usbd_endpoint* ep) {
    (void)busid;
    (void)ep;
}

int usbd_ep_start_write(uint8_t busid, const uint8_t ep, const uint8_t* data, uint32_t len) {
    event_t* e = push(EV_WRITE, ep, len);
    (void)busid;
    if(data && len) memcpy(e->data, data, len < 64 ? len : 64);
    if(npending < 8) pending_in[npending++] = len;
    return 0;
}

int usbd_ep_start_read(uint8_t busid, const uint8_t ep, uint8_t* data, uint32_t len) {
    (void)busid;
    push(EV_READ, ep, len);
    if(len == USB_SIZEOF_MSC_CBW) cbw_buf = data;
    else {
        data_out_buf = data;
        data_out_len = len;
    }
    return 0;
}

int usbd_ep_set_stall(uint8_t busid, const uint8_t ep) {
    (void)busid;
    push(EV_STALL, ep, 0);
    return 0;
}

void usbd_msc_get_cap(uint8_t busid, uint8_t lun, uint32_t* block_num, uint32_t* block_size) {
    (void)busid;
    (void)lun;
    *block_num = cap_blocks;
    *block_size = 512;
}

int usbd_msc_sector_read(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t* buffer, uint32_t length) {
    (void)busid; (void)lun; (void)sector;
    memset(buffer, 0xA5, length);
    reads_called++;
    return 0;
}

int usbd_msc_sector_write(uint8_t busid, uint8_t lun, uint32_t sector, uint8_t* buffer, uint32_t length) {
    (void)busid; (void)lun; (void)sector; (void)buffer; (void)length;
    writes_called++;
    return 0;
}

/* ---- driving the class ---- */

extern void mass_storage_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes);
extern void mass_storage_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes);
extern void msc_storage_notify_handler(uint8_t busid, uint8_t event, void* arg);

static struct usbd_interface intf;

static void bring_up(uint32_t blocks) {
    cap_blocks = blocks;
    nev = 0;
    npending = 0;
    usbd_msc_init_intf(0, &intf, OUT_EP, IN_EP);
    msc_storage_notify_handler(0, USBD_EVENT_CONFIGURED, NULL);
    nev = 0;
}

/* Complete every IN transfer the class has queued, in order, the way the
 * port's TX interrupt would. Each completion may queue another. */
static void drain(void) {
    int guard = 32;
    while(npending && guard--) {
        uint32_t len = pending_in[0];
        memmove(pending_in, pending_in + 1, (npending - 1) * sizeof pending_in[0]);
        npending--;
        mass_storage_bulk_in(0, IN_EP, len);
    }
}

static uint32_t tag = 0x1000;

/* Post one command. dir_in: data phase device->host. Returns nothing; the
 * event log holds what happened. */
static void command(uint8_t dir_in, uint32_t dlen, const uint8_t* cb, uint8_t cblen) {
    struct CBW* c = (struct CBW*)cbw_buf;
    nev = 0;
    memset(c, 0, sizeof *c);
    c->dSignature = MSC_CBW_Signature;
    c->dTag = ++tag;
    c->dDataLength = dlen;
    c->bmFlags = dir_in ? 0x80 : 0x00;
    c->bLUN = 0;
    c->bCBLength = cblen;
    memcpy(c->CB, cb, cblen);
    mass_storage_bulk_out(0, OUT_EP, USB_SIZEOF_MSC_CBW);
    drain();
}

static void bad_signature_command(void) {
    struct CBW* c = (struct CBW*)cbw_buf;
    nev = 0;
    memset(c, 0, sizeof *c);
    c->dSignature = 0xDEADBEEF;
    c->dTag = ++tag;
    c->bCBLength = 6;
    mass_storage_bulk_out(0, OUT_EP, USB_SIZEOF_MSC_CBW);
    drain();
}

/* ---- reading the event log ---- */

static int count(int kind) {
    int n = 0;
    for(int i = 0; i < nev && i < 32; i++) if(ev[i].kind == kind) n++;
    return n;
}

static int stalls_on(uint8_t ep) {
    int n = 0;
    for(int i = 0; i < nev && i < 32; i++) if(ev[i].kind == EV_STALL && ev[i].ep == ep) n++;
    return n;
}

/* The CSW is the last 13-byte IN write. */
static event_t* csw(void) {
    for(int i = (nev < 32 ? nev : 32) - 1; i >= 0; i--)
        if(ev[i].kind == EV_WRITE && ev[i].ep == IN_EP && ev[i].len == USB_SIZEOF_MSC_CSW) return &ev[i];
    return NULL;
}

static uint32_t le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A well-formed command sequence ends with a CSW of the expected status,
 * echoing the tag, followed by a re-armed CBW read. */
static int ends_with_csw(uint8_t status, uint32_t residue) {
    event_t* c = csw();
    if(!c) return 0;
    if(le32(c->data) != MSC_CSW_Signature) return 0;
    if(le32(c->data + 4) != tag) return 0;
    if(le32(c->data + 8) != residue) return 0;
    if(c->data[12] != status) return 0;
    event_t* last = &ev[(nev < 32 ? nev : 32) - 1];
    return last->kind == EV_READ && last->ep == OUT_EP && last->len == USB_SIZEOF_MSC_CBW;
}

/* First IN write before the CSW: the data phase. */
static event_t* data_phase(void) {
    for(int i = 0; i < nev && i < 32; i++)
        if(ev[i].kind == EV_WRITE && ev[i].ep == IN_EP) return &ev[i];
    return NULL;
}

static int data_in_len(void) {
    event_t* d = data_phase();
    return d ? (int)d->len : -1;
}

/* ---- the commands ---- */

static const uint8_t CB_TUR[6] = {SCSI_CMD_TESTUNITREADY};
static const uint8_t CB_SENSE[6] = {SCSI_CMD_REQUESTSENSE, 0, 0, 0, 18, 0};
static const uint8_t CB_INQ[6] = {SCSI_CMD_INQUIRY, 0, 0, 0, 36, 0};
static const uint8_t CB_MODE6[6] = {SCSI_CMD_MODESENSE6, 0, 0x3f, 0, 4, 0};
static const uint8_t CB_RFC[10] = {SCSI_CMD_READFORMATCAPACITIES, 0, 0, 0, 0, 0, 0, 0, 12, 0};
static const uint8_t CB_RC10[10] = {SCSI_CMD_READCAPACITY10};
static const uint8_t CB_RC16[16] = {0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
static const uint8_t CB_READ10[10] = {SCSI_CMD_READ10, 0, 0, 0, 0, 0, 0, 0, 1, 0};
static const uint8_t CB_WRITE10[10] = {SCSI_CMD_WRITE10, 0, 0, 0, 0, 0, 0, 0, 1, 0};
static const uint8_t CB_EJECT[6] = {SCSI_CMD_STARTSTOPUNIT, 0, 0, 0, 0x02, 0};
static const uint8_t CB_START[6] = {SCSI_CMD_STARTSTOPUNIT, 0, 0, 0, 0x01, 0};
static const uint8_t CB_BOGUS[6] = {0x35}; /* SYNCHRONIZE CACHE: unsupported */

static int sense_is(uint8_t key, uint8_t asc, uint8_t asq) {
    command(1, 18, CB_SENSE, 6);
    event_t* d = data_phase();
    if(!d || d->len != 18) return 0;
    if(!ends_with_csw(CSW_STATUS_CMD_PASSED, 0)) return 0;
    return d->data[2] == key && d->data[12] == asc && d->data[13] == asq;
}

static void test_empty_slot(void) {
    printf("empty slot (0 blocks)\n");
    bring_up(0);

    command(0, 0, CB_TUR, 6);
    CHECK("TUR fails with a CSW, no stall", ends_with_csw(CSW_STATUS_CMD_FAILED, 0) && count(EV_STALL) == 0);
    CHECK("sense says NOT READY / MEDIUM NOT PRESENT", sense_is(0x02, 0x3a, 0x00));

    command(1, 8, CB_RC10, 10);
    CHECK("READ CAPACITY(10): empty data phase then failed CSW",
          data_in_len() == 0 && ends_with_csw(CSW_STATUS_CMD_FAILED, 8) && count(EV_STALL) == 0);

    command(1, 32, CB_RC16, 16);
    CHECK("READ CAPACITY(16): empty data phase then failed CSW",
          data_in_len() == 0 && ends_with_csw(CSW_STATUS_CMD_FAILED, 32) && count(EV_STALL) == 0);

    command(1, 12, CB_RFC, 10);
    CHECK("READ FORMAT CAPACITIES: empty data phase then failed CSW",
          data_in_len() == 0 && ends_with_csw(CSW_STATUS_CMD_FAILED, 12));

    reads_called = 0;
    command(1, 512, CB_READ10, 10);
    CHECK("READ(10): failed CSW, card never touched",
          data_in_len() == 0 && ends_with_csw(CSW_STATUS_CMD_FAILED, 512) && reads_called == 0);
    CHECK("sense after READ is still MEDIUM NOT PRESENT", sense_is(0x02, 0x3a, 0x00));

    writes_called = 0;
    command(0, 512, CB_WRITE10, 10);
    CHECK("WRITE(10): OUT pipe stalled once, then failed CSW",
          stalls_on(OUT_EP) == 1 && stalls_on(IN_EP) == 0 &&
          ends_with_csw(CSW_STATUS_CMD_FAILED, 512) && writes_called == 0);

    command(1, 36, CB_INQ, 6);
    CHECK("INQUIRY still answers", data_in_len() == 36 && ends_with_csw(CSW_STATUS_CMD_PASSED, 0));
    CHECK("INQUIRY says removable", data_phase()->data[1] == 0x80);

    command(1, 4, CB_MODE6, 6);
    CHECK("MODE SENSE(6) still answers", data_in_len() == 4 && ends_with_csw(CSW_STATUS_CMD_PASSED, 0));
}

static void test_eject(void) {
    printf("card present, then ejected\n");
    bring_up(1000);

    command(0, 0, CB_TUR, 6);
    CHECK("TUR passes with a card", ends_with_csw(CSW_STATUS_CMD_PASSED, 0));

    command(1, 8, CB_RC10, 10);
    CHECK("READ CAPACITY(10) reports last LBA 999",
          data_in_len() == 8 && le32(data_phase()->data) == 0xe7030000u /* BE 999 */ &&
          ends_with_csw(CSW_STATUS_CMD_PASSED, 0));

    reads_called = 0;
    command(1, 512, CB_READ10, 10);
    CHECK("READ(10) serves data", data_in_len() == 512 && reads_called == 1 &&
          ends_with_csw(CSW_STATUS_CMD_PASSED, 0));

    command(0, 0, CB_EJECT, 6);
    CHECK("eject succeeds", ends_with_csw(CSW_STATUS_CMD_PASSED, 0));
    command(0, 0, CB_TUR, 6);
    CHECK("TUR fails after eject", ends_with_csw(CSW_STATUS_CMD_FAILED, 0));
    CHECK("sense after eject is MEDIUM NOT PRESENT", sense_is(0x02, 0x3a, 0x00));
    command(1, 8, CB_RC10, 10);
    CHECK("READ CAPACITY fails after eject", data_in_len() == 0 && ends_with_csw(CSW_STATUS_CMD_FAILED, 8));
    CHECK("popup flag reports the eject", usbd_msc_set_popup(0));

    command(0, 0, CB_START, 6);
    CHECK("START re-loads the medium", ends_with_csw(CSW_STATUS_CMD_PASSED, 0));
    command(0, 0, CB_TUR, 6);
    CHECK("TUR passes again", ends_with_csw(CSW_STATUS_CMD_PASSED, 0));

    usbd_msc_set_medium(0, 0, false);
    command(0, 0, CB_TUR, 6);
    CHECK("board can withdraw the medium itself", ends_with_csw(CSW_STATUS_CMD_FAILED, 0));
    usbd_msc_set_medium(0, 0, true);
    command(0, 0, CB_TUR, 6);
    CHECK("and give it back", ends_with_csw(CSW_STATUS_CMD_PASSED, 0));

    command(0, 0, CB_EJECT, 6);
    msc_storage_notify_handler(0, USBD_EVENT_RESET, NULL);
    msc_storage_notify_handler(0, USBD_EVENT_CONFIGURED, NULL);
    command(0, 0, CB_TUR, 6);
    CHECK("bus reset (re-plug) brings the medium back", ends_with_csw(CSW_STATUS_CMD_PASSED, 0));
}

static void test_failures_end_in_csw(void) {
    printf("failed commands\n");
    bring_up(1000);

    command(0, 0, CB_BOGUS, 6);
    CHECK("unsupported opcode: failed CSW, no stall",
          ends_with_csw(CSW_STATUS_CMD_FAILED, 0) && count(EV_STALL) == 0);
    CHECK("sense says ILLEGAL REQUEST / INVALID COMMAND", sense_is(0x05, 0x20, 0x00));

    reads_called = 0;
    command(1, 512, CB_READ10, 10);
    CHECK("a READ right after a failure passes", reads_called == 1 && ends_with_csw(CSW_STATUS_CMD_PASSED, 0));

    uint8_t far[10] = {SCSI_CMD_READ10, 0, 0, 0, 0x10, 0, 0, 0, 1, 0}; /* LBA 4096 of 1000 */
    command(1, 512, far, 10);
    CHECK("READ out of range: empty data then failed CSW", data_in_len() == 0 && ends_with_csw(CSW_STATUS_CMD_FAILED, 512));
    CHECK("sense says LBA OUT OF RANGE", sense_is(0x05, 0x21, 0x00));

    uint8_t farw[10] = {SCSI_CMD_WRITE10, 0, 0, 0, 0x10, 0, 0, 0, 1, 0};
    command(0, 512, farw, 10);
    CHECK("WRITE out of range: OUT stalled, failed CSW", stalls_on(OUT_EP) == 1 && ends_with_csw(CSW_STATUS_CMD_FAILED, 512));
    CHECK("sense says LBA OUT OF RANGE", sense_is(0x05, 0x21, 0x00));

    writes_called = 0;
    command(0, 512, CB_WRITE10, 10);
    CHECK("WRITE in range asks for the payload", data_out_len == 512 && count(EV_STALL) == 0);
    memset(data_out_buf, 0x5A, 512);
    nev = 0;
    mass_storage_bulk_out(0, OUT_EP, 512);
    drain();
    CHECK("and passes once it arrives", writes_called == 1 && ends_with_csw(CSW_STATUS_CMD_PASSED, 0));

    bad_signature_command();
    CHECK("malformed CBW: IN stalled, CBW re-armed, no CSW",
          stalls_on(IN_EP) == 1 && csw() == NULL &&
          ev[nev - 1].kind == EV_READ && ev[nev - 1].len == USB_SIZEOF_MSC_CBW);
}

int main(void) {
    test_empty_slot();
    test_eject();
    test_failures_end_in_csw();
    printf(fails ? "%d FAILURE(S)\n" : "all msc checks pass\n", fails);
    return fails ? 1 : 0;
}
