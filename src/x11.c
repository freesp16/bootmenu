/* An X window for the gl-x11 and cpu renderers on the unit (an override-redirect 800x480 window over the screen) and
 * for the development window on a PC (--render host). libX11 is loaded with dlopen, so the fast fb path never maps
 * it and the binary has no DT_NEEDED on it. Keyboard: Up/Down or Left/Right = turn, Return/space = push,
 * Escape/BackSpace = back, F5/h = home (rescan), q = quit; a mouse click is a touch. */
#define _GNU_SOURCE /* RTLD_DEFAULT */
#include "bm.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _XDisplay Display;
typedef unsigned long XID, Window, KeySym;

/* Only the event fields used here; the union is padded to Xlib's size (24 longs). */
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    Display *display;
    Window window, root, subwindow;
    unsigned long time;
    int x, y, x_root, y_root;
    unsigned int state, keycode_or_button;
} XInputEventLike;
typedef union {
    int type;
    XInputEventLike in;
    long pad[24];
} XEvent;

typedef struct {
    unsigned long background_pixmap, background_pixel, border_pixmap, border_pixel;
    int bit_gravity, win_gravity, backing_store;
    unsigned long backing_planes, backing_pixel;
    int save_under;
    long event_mask, do_not_propagate_mask;
    int override_redirect;
    unsigned long colormap, cursor;
} XSetWindowAttributes;

enum { KeyPress = 2, ButtonPress = 4, ButtonRelease = 5, ClientMessage = 33 };
enum { KeyPressMask = 1L << 0, ButtonPressMask = 1L << 2, ButtonReleaseMask = 1L << 3, ExposureMask = 1L << 15 };
enum { CWBackPixel = 1L << 1, CWOverrideRedirect = 1L << 9, CWEventMask = 1L << 11 };

static struct {
    Display *(*XOpenDisplay)(const char *);
    int (*XDefaultScreen)(Display *);
    Window (*XRootWindow)(Display *, int);
    Window (*XCreateWindow)(Display *, Window, int, int, unsigned, unsigned, unsigned, int, unsigned, void *,
                            unsigned long, XSetWindowAttributes *);
    int (*XMapRaised)(Display *, Window);
    int (*XStoreName)(Display *, Window, const char *);
    int (*XPending)(Display *);
    int (*XNextEvent)(Display *, XEvent *);
    KeySym (*XLookupKeysym)(void *, int);
    int (*XFlush)(Display *);
    int (*XConnectionNumber)(Display *);
    int (*XDisplayWidth)(Display *, int);
    int (*XDisplayHeight)(Display *, int);
    /* the CPU renderer's frames (soft.c) */
    void *(*XDefaultVisual)(Display *, int);
    int (*XDefaultDepth)(Display *, int);
    void *(*XCreateGC)(Display *, unsigned long, unsigned long, void *);
    void *(*XCreateImage)(Display *, void *, unsigned, int, int, char *, unsigned, unsigned, int, int);
    int (*XPutImage)(Display *, unsigned long, void *, void *, int, int, int, int, unsigned, unsigned);
    void *gc, *image;
    int depth, bpp;
    uint8_t *frame;
    Display *dpy;
    Window win;
    int fullscreen, w, h;
} X;

/* A function as the program's own link would find it: the global scope first (where an LD_PRELOAD library comes before
 * libX11), else libX11 itself. */
static void *x_sym(void *lib, const char *name) {
    void *f = dlsym(RTLD_DEFAULT, name);
    return f ? f : dlsym(lib, name);
}

int x11_open(int w, int h, int fullscreen, char *err, size_t errlen) {
    void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        snprintf(err, errlen, "dlopen libX11.so.6: %s", dlerror());
        return -1;
    }
#define SYM(name)                                                                  \
    if (!(*(void **)(&X.name) = x_sym(lib, #name))) {                              \
        snprintf(err, errlen, "libX11 lacks %s", #name);                           \
        return -1;                                                                 \
    }
    SYM(XOpenDisplay) SYM(XDefaultScreen) SYM(XRootWindow) SYM(XCreateWindow) SYM(XMapRaised) SYM(XStoreName)
    SYM(XPending) SYM(XNextEvent) SYM(XLookupKeysym) SYM(XFlush) SYM(XConnectionNumber) SYM(XDisplayWidth)
    SYM(XDisplayHeight) SYM(XDefaultVisual) SYM(XDefaultDepth) SYM(XCreateGC) SYM(XCreateImage) SYM(XPutImage)
#undef SYM
    X.dpy = X.XOpenDisplay(NULL); /* $DISPLAY; the launcher sets :0 on the unit */
    if (!X.dpy) {
        snprintf(err, errlen, "cannot open X display %s", "($DISPLAY)");
        return -1;
    }
    int screen = X.XDefaultScreen(X.dpy);
    if (fullscreen) {
        w = X.XDisplayWidth(X.dpy, screen);
        h = X.XDisplayHeight(X.dpy, screen);
    }
    XSetWindowAttributes a;
    memset(&a, 0, sizeof a);
    a.background_pixel = 0;
    a.override_redirect = fullscreen; /* no window manager on the unit: just cover the screen */
    a.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask | ExposureMask;
    X.win = X.XCreateWindow(X.dpy, X.XRootWindow(X.dpy, screen), 0, 0, (unsigned)w, (unsigned)h, 0, 0 /*CopyFromParent*/,
                            1 /*InputOutput*/, NULL, CWBackPixel | CWOverrideRedirect | CWEventMask, &a);
    X.XStoreName(X.dpy, X.win, "Boot menu");
    X.XMapRaised(X.dpy, X.win);
    X.XFlush(X.dpy);
    X.fullscreen = fullscreen, X.w = w, X.h = h;
    return 0;
}

void *x11_display(void) { return X.dpy; }
unsigned long x11_window(void) { return X.win; }
int x11_fd(void) { return X.dpy ? X.XConnectionNumber(X.dpy) : -1; }

int x11_poll(InputEvent *out, int max, int *quit) {
    int n = 0;
    if (!X.dpy) return 0;
    while (n < max && X.XPending(X.dpy)) {
        XEvent ev;
        X.XNextEvent(X.dpy, &ev);
        InputEvent e = {IN_NONE, 0, 0, 0};
        if (ev.type == KeyPress) {
            KeySym k = X.XLookupKeysym(&ev, 0);
            switch (k) {
            case 0xff52: case 0xff51: e.kind = IN_TURN, e.value = -1; break; /* Up, Left */
            case 0xff54: case 0xff53: e.kind = IN_TURN, e.value = 1; break;  /* Down, Right */
            case 0xff0d: case 0x20: e.kind = IN_PUSH; break;                   /* Return, space */
            case 0xff1b: case 0xff08: e.kind = IN_BACK; break;                 /* Escape, BackSpace */
            case 0xffc2: case 'h': e.kind = IN_HOME; break;                    /* F5, h */
            case 'q': *quit = 1; break;
            default: break;
            }
        } else if ((ev.type == ButtonPress || ev.type == ButtonRelease) && ev.in.keycode_or_button == 1) {
            e.kind = ev.type == ButtonPress ? IN_TOUCH_DOWN : IN_TOUCH_UP;
            e.x = ev.in.x, e.y = ev.in.y;
        } else if (ev.type == ButtonPress && (ev.in.keycode_or_button == 4 || ev.in.keycode_or_button == 5)) {
            e.kind = IN_TURN, e.value = ev.in.keycode_or_button == 4 ? -1 : 1; /* mouse wheel */
        }
        if (e.kind != IN_NONE) out[n++] = e;
    }
    return n;
}

/* ---- the CPU renderer's frames ------------------------------------------------------------------------------------
 * The start of Xlib's XImage (its layout is fixed by the Xlib ABI): the fields read back after XCreateImage. */
typedef struct {
    int width, height, xoffset, format;
    char *data;
    int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth, bytes_per_line, bits_per_pixel;
} XImageHead;

int x11_present_prepare(int w, int h, char *how, size_t howlen) {
    if (!X.dpy) return snprintf(how, howlen, "no X display"), -1;
    int screen = X.XDefaultScreen(X.dpy);
    X.depth = X.XDefaultDepth(X.dpy, screen);
    if (X.depth != 16 && X.depth != 24 && X.depth != 32)
        return snprintf(how, howlen, "X depth %d not supported", X.depth), -1;
    X.bpp = X.depth == 16 ? 2 : 4;
    X.frame = calloc((size_t)w * (size_t)h, (size_t)X.bpp);
    X.gc = X.XCreateGC(X.dpy, X.win, 0, NULL);
    X.image = X.frame ? X.XCreateImage(X.dpy, X.XDefaultVisual(X.dpy, screen), (unsigned)X.depth, 2 /*ZPixmap*/, 0,
                                       (char *)X.frame, (unsigned)w, (unsigned)h, 32, w * X.bpp)
                      : NULL;
    if (!X.image || !X.gc) return snprintf(how, howlen, "XCreateImage failed"), -1;
    const XImageHead *im = X.image;
    if (im->bits_per_pixel != X.bpp * 8 || im->bytes_per_line != w * X.bpp)
        return snprintf(how, howlen, "X image %d bpp, %d bytes a line: not as expected", im->bits_per_pixel,
                        im->bytes_per_line), -1;
    snprintf(how, howlen, "XPutImage depth %d (%s)", X.depth, X.depth == 16 ? "RGB565" : "XRGB8888");
    return 0;
}

/* One frame of 0x00RRGGBB words, converted to the server's format. */
int x11_present(const uint32_t *px, int w, int h) {
    if (!X.image) return -1;
    size_t n = (size_t)w * (size_t)h;
    if (X.bpp == 2) {
        uint16_t *d = (uint16_t *)X.frame;
        for (size_t i = 0; i < n; i++) {
            uint32_t c = px[i];
            d[i] = (uint16_t)((c >> 8 & 0xf800) | (c >> 5 & 0x07e0) | (c >> 3 & 0x001f));
        }
    } else {
        memcpy(X.frame, px, n * 4);
    }
    X.XPutImage(X.dpy, X.win, X.gc, X.image, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    X.XFlush(X.dpy);
    return 0;
}
