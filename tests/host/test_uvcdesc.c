/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The UVC descriptor block, checked for self-consistency.
 *
 * A descriptor whose declared length disagrees with the bytes that follow it
 * does not fail loudly. The host stops walking, the interface quietly does
 * not appear, and the only symptom is a camera that is not there. Every one
 * of those lengths is hand-written arithmetic in src/usbuvc.h, so this
 * checks the arithmetic against the bytes rather than against itself.
 *
 * It builds the same macro the firmware builds, so a change to one is
 * checked here before it ever reaches a board.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WBVAL(x) (x & 0xFF), ((x >> 8) & 0xFF)
#define DBVAL(x) (x & 0xFF), ((x >> 8) & 0xFF), ((x >> 16) & 0xFF), ((x >> 24) & 0xFF)

#define CONFIG_USB_HS
#include "usbuvc.h"

static const uint8_t uvc[] = { UVC_DESCRIPTOR_INIT(0x02, 0x03, 0x05) };

static int fails = 0;

static void eq(const char *what, unsigned long got, unsigned long want)
{
    if (got == want) {
        printf("  ok   %-42s %lu\n", what, got);
    } else {
        printf("  FAIL %-42s got %lu, want %lu\n", what, got, want);
        fails++;
    }
}

/* Walk bLength-chained descriptors from `off` for `len` bytes, checking that
 * every step lands on a descriptor and the last ends exactly on the bound. */
static int walk(const char *what, unsigned off, unsigned len)
{
    unsigned p = off, n = 0;
    while (p < off + len) {
        unsigned b = uvc[p];
        if (b == 0) {
            printf("  FAIL %-42s zero bLength at %u\n", what, p);
            fails++;
            return 0;
        }
        p += b;
        n++;
    }
    if (p != off + len) {
        printf("  FAIL %-42s overran to %u, bound %u\n", what, p, off + len);
        fails++;
        return 0;
    }
    printf("  ok   %-42s %u descriptors, ends exactly\n", what, n);
    return 1;
}

int main(void)
{
    unsigned vc_cs, vs_cs;

    printf("== test_uvcdesc\n");

    eq("UVC_DESCRIPTOR_LEN matches emitted bytes", sizeof(uvc), UVC_DESCRIPTOR_LEN);

    /* Interface association comes first and must cover both interfaces. */
    eq("IAD bLength", uvc[0], 8);
    eq("IAD bDescriptorType", uvc[1], 0x0B);
    eq("IAD bFirstInterface", uvc[2], 0x02);
    eq("IAD bInterfaceCount", uvc[3], 0x02);
    eq("IAD bFunctionClass is video", uvc[4], 0x0E);

    /* VideoControl interface: no endpoints, class 0x0E subclass 0x01. */
    eq("VC bInterfaceNumber", uvc[8 + 2], 0x02);
    eq("VC bNumEndpoints", uvc[8 + 4], 0x00);
    eq("VC bInterfaceSubClass", uvc[8 + 6], 0x01);

    /* VC header declares the length of the class-specific block after it. */
    vc_cs = uvc[17 + 5] | (uvc[17 + 6] << 8);
    eq("VC header wTotalLength", vc_cs, UVC_VC_TOTAL);
    eq("VC header baInterfaceNr points at VS", uvc[17 + 12], 0x03);
    walk("VC class-specific block walks", 17, vc_cs);

    /* Entity ids must match what CherryUSB's class hardcodes: 1, 2, 3. */
    eq("input terminal bTerminalID", uvc[30 + 3], 0x01);
    eq("processing unit bUnitID", uvc[48 + 3], 0x02);
    eq("processing unit bSourceID", uvc[48 + 4], 0x01);
    eq("output terminal bTerminalID", uvc[60 + 3], 0x03);
    eq("output terminal bAssocTerminal", uvc[60 + 6], 0x00);
    eq("output terminal bSourceID", uvc[60 + 7], 0x02);

    /* VideoStreaming interface: one bulk endpoint, one alternate setting. */
    eq("VS bInterfaceNumber", uvc[69 + 2], 0x03);
    eq("VS bAlternateSetting", uvc[69 + 3], 0x00);
    eq("VS bNumEndpoints", uvc[69 + 4], 0x01);
    eq("VS bInterfaceSubClass", uvc[69 + 6], 0x02);

    vs_cs = uvc[78 + 4] | (uvc[78 + 5] << 8);
    eq("VS header wTotalLength", vs_cs, UVC_VS_TOTAL);
    eq("VS header bEndpointAddress", uvc[78 + 6], UVC_IN_EP);
    eq("VS header bTerminalLink is output terminal", uvc[78 + 8], 0x03);
    walk("VS class-specific block walks", 78, vs_cs);

    /* Format announces two frames, and two must follow. */
    eq("MJPEG bNumFrameDescriptors", uvc[92 + 4], 2);
    eq("frame 1 index", uvc[103 + 3], 1);
    eq("frame 1 width", uvc[103 + 5] | (uvc[103 + 6] << 8), 720);
    eq("frame 1 height", uvc[103 + 7] | (uvc[103 + 8] << 8), 480);
    eq("frame 2 index", uvc[133 + 3], 2);
    eq("frame 2 width", uvc[133 + 5] | (uvc[133 + 6] << 8), 720);
    eq("frame 2 height", uvc[133 + 7] | (uvc[133 + 8] << 8), 576);

    /* One alternate setting only: macOS selects interface 3 alt 0 and never
     * an alt 1, so the endpoint has to be here. Observed on the wire. */
    eq("VS bAlternateSetting", uvc[69 + 3], 0x00);

    /* The endpoint closes the block: bulk, IN, and full high-speed size. */
    eq("endpoint bLength", uvc[163], 7);
    eq("endpoint bDescriptorType", uvc[164], 0x05);
    eq("endpoint bEndpointAddress", uvc[165], UVC_IN_EP);
    eq("endpoint bmAttributes is bulk", uvc[166], 0x02);
    eq("endpoint wMaxPacketSize", uvc[167] | (uvc[168] << 8), UVC_MAX_MPS);
    eq("endpoint bInterval is 0 for bulk", uvc[169], 0);

    /* And the whole block chains cleanly from end to end. */
    walk("whole UVC block walks", 0, sizeof(uvc));

    if (fails) {
        printf("test_uvcdesc: %d FAILURES\n", fails);
        return 1;
    }
    printf("test_uvcdesc: all tests pass\n");
    return 0;
}
