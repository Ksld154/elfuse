/*
 * Synchronous usbdevfs transfers bound a device-reported count
 * (ELFUSE_USB_FIXTURE=loopback)
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Code under test: usbdev_do_control and usbdev_do_bulk in
 * src/syscall/usbdev.c. Linux bounds the count a synchronous CONTROL or BULK IN
 * returns by the length the caller asked for, so a guest may copy that many
 * bytes out of its own buffer. IOKit's count is a device-supplied number; the
 * fixture's 0xF5 command makes it larger than the request, and this checks that
 * neither the return value nor the bytes written into guest memory follow it
 * past the request.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define NODE "/dev/bus/usb/003/001"
#define IFNUM 2
#define EP_IN 0x81

#define USBDEVFS_CONTROL 0xc0185500u
#define USBDEVFS_BULK 0xc0185502u
#define USBDEVFS_CLAIMINTERFACE 0x8004550fu

#define FX_RESET 0xf3
#define FX_OVERREPORT 0xf5

#define REQ_LEN 8
#define EXTRA 24
#define CANARY 0x5a

struct ctrltransfer {
    uint8_t bRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
    uint32_t timeout;
    void *data;
};

struct bulktransfer {
    unsigned int ep, len, timeout;
    void *data;
};

static int fd = -1;

static long io(unsigned long req, void *arg)
{
    int r = ioctl(fd, req, arg);
    return r < 0 ? -errno : r;
}

static long fx_cmd(uint8_t cmd, uint16_t value)
{
    struct ctrltransfer ct = {.bRequestType = 0x40,
                              .bRequest = cmd,
                              .wValue = value,
                              .timeout = 1000};
    return io(USBDEVFS_CONTROL, &ct);
}

/* True when nothing past the first REQ_LEN bytes of buf was touched. */
static int tail_intact(const uint8_t *buf, size_t size)
{
    for (size_t i = REQ_LEN; i < size; i++)
        if (buf[i] != CANARY)
            return 0;
    return 1;
}

static void t_control_in(void)
{
    uint8_t buf[REQ_LEN + EXTRA + 8];
    memset(buf, CANARY, sizeof(buf));
    struct ctrltransfer ct = {.bRequestType = 0xc0, /* vendor, device, IN */
                              .bRequest = 0x01,
                              .wLength = REQ_LEN,
                              .timeout = 1000,
                              .data = buf};
    long r = io(USBDEVFS_CONTROL, &ct);

    TEST("control IN count clamped");
    if (r == REQ_LEN)
        PASS();
    else {
        printf("(got %ld) ", r);
        FAIL("CONTROL IN returned more than wLength");
    }
    TEST("control IN tail untouched");
    if (tail_intact(buf, sizeof(buf)))
        PASS();
    else
        FAIL("CONTROL IN wrote past wLength");
}

static void t_bulk_in(void)
{
    uint8_t buf[REQ_LEN + EXTRA + 8];
    memset(buf, CANARY, sizeof(buf));
    struct bulktransfer bt = {
        .ep = EP_IN, .len = REQ_LEN, .timeout = 1000, .data = buf};
    long r = io(USBDEVFS_BULK, &bt);

    TEST("bulk IN count clamped");
    if (r == REQ_LEN)
        PASS();
    else {
        printf("(got %ld) ", r);
        FAIL("BULK IN returned more than len");
    }
    TEST("bulk IN tail untouched");
    if (tail_intact(buf, sizeof(buf)))
        PASS();
    else
        FAIL("BULK IN wrote past len");
}

int main(void)
{
    printf("test-usbdev-sync-clamp\n");
    fd = open(NODE, O_RDWR);
    TEST("open loopback node");
    if (fd < 0) {
        FAIL("open " NODE " (needs ELFUSE_USB_FIXTURE=loopback)");
        SUMMARY("test-usbdev-sync-clamp");
        return 1;
    }
    PASS();

    unsigned ifn = IFNUM;
    TEST("claim interface");
    if (io(USBDEVFS_CLAIMINTERFACE, &ifn) == 0)
        PASS();
    else
        FAIL("CLAIMINTERFACE");

    TEST("arm over-report");
    if (fx_cmd(FX_RESET, 0) == 0 && fx_cmd(FX_OVERREPORT, EXTRA) == 0)
        PASS();
    else
        FAIL("fixture control plane");

    t_control_in();
    t_bulk_in();

    fx_cmd(FX_RESET, 0);
    close(fd);
    SUMMARY("test-usbdev-sync-clamp");
    return fails ? 1 : 0;
}
