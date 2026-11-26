/* The panel MCU link /dev/subucom_spi0.1: 128-byte RX frames with buttons, encoder counters and the LCD touch.
 *
 * Both units: header byte 0 = 0x01, CRC-16/X-25 over bytes 0..123 stored little-endian at 124; buttons: byte 1 bit 1 =
 * SELECT push, bit 2 = BACK, bit 3 = HOME; byte 2 bit 3 = PLAY. The driver keeps one transfer timer per device and
 * refuses bits/bytes with EBUSY while it runs, and the official application leaves it on.
 *
 * SP-16, as the freesp16 app uses it, written from the sp16-emulator's public documentation (panel_map.py,
 * hardware_stub.c), never from firmware code:
 *   - SELECT encoder: cumulative big-endian 16-bit counter at 0x3c (+ = clockwise); knob 1 at 0x30;
 *   - LCD touch: raw big-endian X at 0x2c, Y at 0x2e, contact inside X 45..967 / Y 45..908, (0, 0) = released;
 *     screen = round((940 - rawX) * 720 / 868) + 40, round((rawY - 87) * 405 / 779) + 29;
 *   - open() turns the timer off, sets the link up, turns it on; close() turns it off again (confirmed on hardware).
 *     The menu sends no TX frame on the SP-16, except the one that asks for standby (panel_standby()).
 *
 * DJS-1000, the layout seen on its panel link:
 *   - 16-bit counters little-endian as read: the rotary selector at 0x14, knob 1 at 0x08; touch X at 0x5e, Y at 0x60;
 *   - byte 5 bit 4 = POWER; TX byte 0x6c bit 0 holds the outputs muted, bit 1 asks for standby, 0x6d = 0x04;
 *   - the link runs every 2 ms. A link already running with that set-up is taken over as it is (a pause in the
 *     exchange may make the panel MCU mute), else it is set up as on the SP-16. One TX frame is sent: every lamp off
 *     and the outputs muted (the menu plays nothing). The timer is left running on close, on that frame, so the next
 *     program finds the link as the official application wants it. The diagnostic did all three on a unit;
 *   - touch: the SP-16's mapping, until it is checked on a unit (the rows are the full width, so only Y matters, and
 *     the raw values of each touch are logged for that).
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
    REQ_MESSAGE = 0x40107000UL, REQ_SET_TIMER = 0x40017001UL, REQ_GET_TIMER = 0x80017001UL,
    REQ_SET_INTERVAL = 0x40047002UL, REQ_GET_INTERVAL = 0x80047002UL, REQ_SET_BITS = 0x40027003UL,
    REQ_GET_BITS = 0x80027003UL, REQ_SET_BYTES = 0x40047004UL, REQ_GET_BYTES = 0x80047004UL,
    REQ_SET_ENDIAN = 0x40017009UL,
};

typedef struct {
    int big_endian;            /* counters and touch as big-endian words (SP-16) or little-endian (DJS-1000) */
    int selector, knob1;       /* offsets of the two counters that move the selection */
    int touch_x, touch_y;
    int power_byte, power_bit; /* power_byte 0: no POWER in the frame */
    uint32_t interval_ms;
    int take_over;             /* 1: a running link set up as wanted is used as it is, and left running on close */
    int lamps_off;             /* 1: send one frame with every lamp off and the outputs muted */
} Layout;

static const Layout SP16 = {1, 0x3c, 0x30, 0x2c, 0x2e, 0, 0, 3, 0, 0};
static const Layout DJS1000 = {0, 0x14, 0x08, 0x5e, 0x60, 5, 0x10, 2, 1, 1};

static struct {
    int fd;
    const Layout *l;
    int have;               /* a reference frame was seen */
    uint16_t selector, knob1;
    uint8_t buttons[7];     /* bytes 0..6 of the previous frame */
    int touch_down, touch_good, last_rx, last_ry, tx, ty;
    long frames, bad;
    uint8_t txf[FRAME];
    int standby;
} P = {-1, &SP16, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, 0, 0, {0}, 0};

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

/* DJS-1000: the frame the menu leaves on the link (the driver repeats the last one it was given). */
struct transfer {
    uint8_t multi_id, reserved, part_id, part_num;
    uint32_t bytes;
    uint64_t tx_buf;
};

static void send_frame(void) {
    memset(P.txf, 0, sizeof P.txf); /* bytes 1..0x6b: every lamp off */
    P.txf[0] = 0x02;
    P.txf[0x6c] = (uint8_t)(0x01 | (P.standby ? 0x02 : 0));
    P.txf[0x6d] = 0x04;
    uint16_t c = crc_x25(P.txf, 124);
    P.txf[124] = (uint8_t)c, P.txf[125] = (uint8_t)(c >> 8);
    struct transfer t;
    memset(&t, 0, sizeof t);
    t.part_num = 1;
    t.bytes = FRAME;
    t.tx_buf = (uintptr_t)P.txf;
    if (ioctl(P.fd, REQ_MESSAGE, &t) != 0) bm_log("panel: tx frame: %s", strerror(errno));
}

static int running_as_wanted(int fd, const Layout *l) {
    uint8_t on = 0;
    uint16_t bits = 0;
    uint32_t bytes = 0, iv = 0;
    return ioctl(fd, REQ_GET_TIMER, &on) == 0 && on && ioctl(fd, REQ_GET_BITS, &bits) == 0 && bits == 32 &&
           ioctl(fd, REQ_GET_BYTES, &bytes) == 0 && bytes == FRAME && ioctl(fd, REQ_GET_INTERVAL, &iv) == 0 &&
           iv == l->interval_ms;
}

int panel_open(char *info, size_t infolen) {
    P.l = unit()->id == UNIT_DJS1000 ? &DJS1000 : &SP16;
    int fd = open(PANEL_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        snprintf(info, infolen, "panel: cannot open %s: %s", PANEL_PATH, strerror(errno));
        return -1;
    }
    uint16_t bits = 32;
    uint8_t endian = 1, off = 0, on = 1;
    uint32_t bytes = FRAME, interval = P.l->interval_ms;
    int taken = P.l->take_over && running_as_wanted(fd, P.l);
    if (taken) {
        (void)ioctl(fd, REQ_SET_ENDIAN, &endian);
    } else if (step(fd, "timer off", REQ_SET_TIMER, &off, info, infolen) ||
               step(fd, "set bits", REQ_SET_BITS, &bits, info, infolen) ||
               step(fd, "set endian", REQ_SET_ENDIAN, &endian, info, infolen) ||
               step(fd, "set bytes", REQ_SET_BYTES, &bytes, info, infolen) ||
               step(fd, "set interval", REQ_SET_INTERVAL, &interval, info, infolen)) {
        close(fd);
        return -1;
    }
    P.fd = fd;
    P.have = 0;
    if (P.l->lamps_off) send_frame();
    if (!taken && step(fd, "timer on", REQ_SET_TIMER, &on, info, infolen)) {
        close(fd);
        P.fd = -1;
        return -1;
    }
    snprintf(info, infolen, "panel: %s ok (%s, %s, bits=32 order=1 bytes=128 interval=%u, %s)", PANEL_PATH,
             unit()->name, taken ? "taken over running" : "set up", (unsigned)interval,
             P.l->lamps_off ? "lamps off and outputs muted" : "RX only");
    return 0;
}

int panel_fd(void) { return P.fd; }

static uint16_t word(const uint8_t *p) {
    return P.l->big_endian ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]);
}

static int add(InputEvent *out, int n, int max, InputKind k, int value, int x, int y) {
    if (n < max) out[n] = (InputEvent){k, value, x, y}, n++;
    return n;
}

static int decode(const uint8_t *f, InputEvent *out, int n, int max) {
    const Layout *l = P.l;
    uint16_t sel = word(f + l->selector), k1 = word(f + l->knob1);
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
        /* POWER: on release, as the official application does */
        if (l->power_byte && (P.buttons[l->power_byte] & ~f[l->power_byte] & l->power_bit))
            n = add(out, n, max, IN_POWER, 0, 0, 0);
    }
    /* Buttons held in the first frame (the push that started nothing) are the reference, never a press. */
    P.selector = sel, P.knob1 = k1;
    memcpy(P.buttons, f, sizeof P.buttons);
    P.have = 1;

    int rx = word(f + l->touch_x), ry = word(f + l->touch_y);
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
        if (l != &SP16) bm_log("panel: touch raw %d %d -> screen %d %d", rx, ry, x, y);
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

/* SP-16: the frame that mutes the outputs and asks for standby (its bytes are not the DJS-1000's). */
static void send_sp16_standby(void) {
    memset(P.txf, 0, sizeof P.txf);
    P.txf[0] = 0x02;
    P.txf[102] = 1, P.txf[103] = 1, P.txf[110] = 4, P.txf[119] = 0xaa;
    uint16_t c = crc_x25(P.txf, 124);
    P.txf[124] = (uint8_t)c, P.txf[125] = (uint8_t)(c >> 8);
    struct transfer t;
    memset(&t, 0, sizeof t);
    t.part_num = 1;
    t.bytes = FRAME;
    t.tx_buf = (uintptr_t)P.txf;
    if (ioctl(P.fd, REQ_MESSAGE, &t) != 0) bm_log("panel: standby tx frame: %s", strerror(errno));
}

void panel_request_standby(void) {
    if (P.fd < 0) return;
    if (P.l->lamps_off) {
        P.standby = 1;
        send_frame();
    } else {
        send_sp16_standby();
    }
    bm_log("panel: standby requested");
}

/* `bootmenu --standby`, run by the launcher once the boot volume is unmounted: the unit powers off within a moment. The
 * SP-16's frame is sent again every 250 ms; the DJS-1000's is repeated by its driver (the exchange keeps running).
 * Returns only when the power did not go within 20 s. */
int panel_standby(void) {
    char error[160];
    if (panel_open(error, sizeof error)) {
        bm_log("%s", error);
        return 1;
    }
    panel_request_standby();
    double deadline = bm_now() + 20.0;
    do {
        usleep(250000);
        if (!P.l->lamps_off) send_sp16_standby();
    } while (bm_now() < deadline);
    bm_log("panel: standby did not cut power within 20 seconds");
    panel_close();
    return 1;
}

void panel_close(void) {
    if (P.fd < 0) return;
    if (!P.l->take_over) {
        uint8_t off = 0;
        (void)ioctl(P.fd, REQ_SET_TIMER, &off);
    }
    close(P.fd);
    P.fd = -1;
    bm_log("panel: closed (frames=%ld bad_crc=%ld, %s)", P.frames, P.bad, P.l->take_over ? "left running" : "timer off");
}
