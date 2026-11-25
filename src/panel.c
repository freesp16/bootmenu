/* The panel MCU link /dev/subucom_spi0.1: 128-byte RX frames with buttons, encoder counters and the LCD touch.
 *
 * Layout and driver requests as the freesp16 app uses them, written from the sp16-emulator's public documentation
 * (panel_map.py, hardware_stub.c), never from firmware code:
 *   - header byte 0 = 0x01, CRC-16/X-25 over bytes 0..123 stored little-endian at 124;
 *   - buttons: byte 1 bit 1 = SELECT push, bit 2 = BACK, bit 3 = HOME; byte 2 bit 3 = PLAY;
 *   - SELECT encoder: cumulative big-endian 16-bit counter at 0x3c (+ = clockwise); knob 1 at 0x30;
 *   - LCD touch: raw big-endian X at 0x2c, Y at 0x2e, contact inside X 45..967 / Y 45..908, (0, 0) = released;
 *     screen = round((940 - rawX) * 720 / 868) + 40, round((rawY - 87) * 405 / 779) + 29.
 * The driver keeps one transfer timer per device and refuses bits/bytes with EBUSY while it runs; the official app
 * leaves it on, so open() turns it off first and on last, and close() turns it off again (confirmed on hardware).
 * Normal menu input uses RX only. The launcher requests a TX frame during standby.
 *
 * Cost: one CRC over 124 bytes and a few compares per frame, on the menu's own thread between frames. */
#include "bm.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PANEL_PATH "/dev/subucom_spi0.1"
#define FRAME 128

enum {
    REQ_SET_TIMER = 0x40017001UL, REQ_SET_INTERVAL = 0x40047002UL, REQ_SET_BITS = 0x40027003UL,
    REQ_SET_BYTES = 0x40047004UL, REQ_SET_ENDIAN = 0x40017009UL,
    REQ_SEND = 0x40107000UL,
};

static struct {
    int fd;
    int have;               /* a reference frame was seen */
    uint16_t selector, knob1;
    uint8_t buttons[3];     /* bytes 0..2 of the previous frame (only 1 and 2 used) */
    int touch_down, touch_good, last_rx, last_ry, tx, ty;
    long frames, bad;
} P = {-1, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, 0, 0};

static uint16_t crc_x25(const uint8_t *d, int n) {
    uint16_t crc = 0xffff;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)~crc;
}

static int step(int fd, const char *what, unsigned long req, void *arg, char *info, size_t infolen) {
    if (ioctl(fd, req, arg) == 0) return 0;
    snprintf(info, infolen, "panel: %s (0x%08lx) failed: %s", what, req, strerror(errno));
    return -1;
}

int panel_open(char *info, size_t infolen) {
    int fd = open(PANEL_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        snprintf(info, infolen, "panel: cannot open %s: %s", PANEL_PATH, strerror(errno));
        return -1;
    }
    uint16_t bits = 32;
    uint8_t endian = 1, off = 0, on = 1;
    uint32_t bytes = FRAME, interval = 3;
    if (step(fd, "timer off", REQ_SET_TIMER, &off, info, infolen) || step(fd, "set bits", REQ_SET_BITS, &bits, info, infolen) ||
        step(fd, "set endian", REQ_SET_ENDIAN, &endian, info, infolen) ||
        step(fd, "set bytes", REQ_SET_BYTES, &bytes, info, infolen) ||
        step(fd, "set interval", REQ_SET_INTERVAL, &interval, info, infolen) ||
        step(fd, "timer on", REQ_SET_TIMER, &on, info, infolen)) {
        close(fd);
        return -1;
    }
    P.fd = fd;
    P.have = 0;
    snprintf(info, infolen, "panel: %s ok (bits=32 order=1 bytes=128 interval=3 timer=on, RX only)", PANEL_PATH);
    return 0;
}

int panel_fd(void) { return P.fd; }

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static int add(InputEvent *out, int n, int max, InputKind k, int value, int x, int y) {
    if (n < max) out[n] = (InputEvent){k, value, x, y}, n++;
    return n;
}

static int decode(const uint8_t *f, InputEvent *out, int n, int max) {
    uint16_t sel = be16(f + 0x3c), k1 = be16(f + 0x30);
    if (P.have) {
        int16_t d = (int16_t)(uint16_t)(sel - P.selector);
        int16_t d1 = (int16_t)(uint16_t)(k1 - P.knob1);
        if (d) n = add(out, n, max, IN_TURN, d, 0, 0);
        if (d1) n = add(out, n, max, IN_TURN, d1, 0, 0);
        uint8_t pressed1 = (uint8_t)(f[1] & ~P.buttons[1]), pressed2 = (uint8_t)(f[2] & ~P.buttons[2]);
        if (pressed1 & 0x02) n = add(out, n, max, IN_PUSH, 0, 0, 0);
        if (pressed2 & 0x08) n = add(out, n, max, IN_PUSH, 0, 0, 0); /* PLAY launches too */
        if (pressed1 & 0x04) n = add(out, n, max, IN_BACK, 0, 0, 0);
        if (pressed1 & 0x08) n = add(out, n, max, IN_HOME, 0, 0, 0);
    }
    /* Buttons held in the first frame (the push that started nothing) are the reference, never a press. */
    P.selector = sel, P.knob1 = k1, P.buttons[1] = f[1], P.buttons[2] = f[2];
    P.have = 1;

    int rx = be16(f + 0x2c), ry = be16(f + 0x2e);
    int inside = rx >= 45 && rx <= 967 && ry >= 45 && ry <= 908;
    if (!inside) {
        if (P.touch_down) n = add(out, n, max, IN_TOUCH_UP, 0, P.tx, P.ty);
        P.touch_down = P.touch_good = 0;
        return n;
    }
    if (P.touch_good && (abs(rx - P.last_rx) > 48 || abs(ry - P.last_ry) > 48) && !P.touch_down) P.touch_good = 0;
    P.touch_good++;
    P.last_rx = rx, P.last_ry = ry;
    int x = (int)((940 - rx) * 720.0 / 868.0 + (rx <= 940 ? 0.5 : -0.5)) + 40;
    int y = (int)((ry - 87) * 405.0 / 779.0 + (ry >= 87 ? 0.5 : -0.5)) + 29;
    P.tx = x, P.ty = y;
    if (!P.touch_down && P.touch_good >= 3) {
        P.touch_down = 1;
        n = add(out, n, max, IN_TOUCH_DOWN, 0, x, y);
    }
    return n;
}

int panel_poll(InputEvent *out, int max) {
    if (P.fd < 0) return 0;
    uint8_t f[FRAME];
    int n = 0;
    /* At most 32 frames a call (the driver delivers one every few ms; the loop calls this every frame). */
    for (int i = 0; i < 32; i++) {
        /* poll first: do not rely on the driver honouring O_NONBLOCK in read() */
        struct pollfd p = {P.fd, POLLIN, 0};
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) break;
        ssize_t r = read(P.fd, f, FRAME);
        if (r != FRAME) break;
        if (f[0] != 0x01 || crc_x25(f, 124) != (uint16_t)(f[124] | f[125] << 8)) {
            P.bad++;
            continue;
        }
        P.frames++;
        n = decode(f, out, n, max);
    }
    return n;
}

void panel_close(void) {
    if (P.fd < 0) return;
    uint8_t off = 0;
    (void)ioctl(P.fd, REQ_SET_TIMER, &off);
    close(P.fd);
    P.fd = -1;
    bm_log("panel: closed (frames=%ld bad_crc=%ld, timer off)", P.frames, P.bad);
}

int panel_request_standby(void) {
    char error[160];
    if (panel_open(error, sizeof error)) {
        bm_log("%s", error);
        return 1;
    }

    /* 128-byte TX frame with mute and standby set, one part, and a CRC-16/X25 trailer. */
    uint8_t tx[FRAME] = { [0] = 2, [102] = 1, [103] = 1, [110] = 4, [119] = 0xaa };
    uint16_t crc = crc_x25(tx, 124);
    tx[124] = crc & 0xff;
    tx[125] = crc >> 8;
    struct {
        uint8_t multi_id, reserved, part_id, part_count;
        uint32_t length;
        uint64_t address;
    } request = { .part_count = 1, .length = sizeof tx, .address = (uintptr_t)tx };

    double deadline = bm_now() + 20.0;
    do {
        if (step(P.fd, "standby TX", REQ_SEND, &request, error, sizeof error)) {
            bm_log("%s", error);
            panel_close();
            return 1;
        }
        usleep(250000);
    } while (bm_now() < deadline);
    bm_log("panel: standby did not cut power within 20 seconds");
    panel_close();
    return 1;
}
