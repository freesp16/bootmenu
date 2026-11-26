/* An X window for the gl-x11 and cpu renderers on the unit (an override-redirect 800x480 window over the screen) and
 * for the development window on a PC (--render host). libX11 is loaded with dlopen, so the fast fb path never maps
 * it and the binary has no DT_NEEDED on it. Keyboard: Up/Down or Left/Right = turn, Return/space = push,
 * Escape/BackSpace = back, F5/h = home (rescan), q = quit; a mouse click is a touch. */
#define _GNU_SOURCE /* RTLD_DEFAULT */
#include "bm.h"

#include <dirent.h>
#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    /* the empty cursor (x11_hide_cursor) */
    unsigned long (*XCreateBitmapFromData)(Display *, unsigned long, const char *, unsigned, unsigned);
    unsigned long (*XCreatePixmapCursor)(Display *, unsigned long, unsigned long, void *, void *, unsigned, unsigned);
    int (*XDefineCursor)(Display *, Window, unsigned long);
    int (*XFreeCursor)(Display *, unsigned long);
    int (*XFreePixmap)(Display *, unsigned long);
    int (*XUndefineCursor)(Display *, Window);
    int (*XCloseDisplay)(Display *);
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

static int x_load(char *err, size_t errlen) {
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
    SYM(XCreateBitmapFromData) SYM(XCreatePixmapCursor) SYM(XDefineCursor) SYM(XFreeCursor) SYM(XFreePixmap)
    SYM(XUndefineCursor) SYM(XCloseDisplay)
#undef SYM
    return 0;
}

/* --hide-cursor: the X server shows its own cursor, an X, wherever no window sets one, and it starts over (with the X
 * back) when its last client leaves, as between the menu and the app it starts. This makes the root window's cursor
 * an empty one and stays connected, so the server never starts over while the session lasts. Waits up to `wait_s` for
 * the server (at power-on it may not be up yet). SIGTERM (the launcher, after the session) gives the server its own
 * cursor back before the connection closes. Returns 0 then, 1 when it could not start. The server going away ends the
 * process (Xlib's I/O error). */
static volatile sig_atomic_t cursor_stop;
static void cursor_on_term(int sig) {
    (void)sig;
    cursor_stop = 1;
}

/* The X server a DISPLAY names, without its screen (":0.0" and ":0" are one server). Unset is ":0", as the launcher
 * sets it. */
static void display_server(const char *display, char *out, size_t n) {
    snprintf(out, n, "%s", display && *display ? display : ":0");
    char *colon = strrchr(out, ':');
    char *dot = colon ? strchr(colon, '.') : NULL;
    if (dot) *dot = 0;
}

/* The X server a process uses (its DISPLAY), or -1 when its environment cannot be read. */
static int process_server(long pid, char *out, size_t n) {
    static char env[32768];
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/environ", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t len = fread(env, 1, sizeof env - 1, f);
    fclose(f);
    env[len] = 0;
    const char *display = NULL;
    for (size_t i = 0; i < len; i += strlen(env + i) + 1)
        if (!strncmp(env + i, "DISPLAY=", 8)) display = env + i + 8;
    display_server(display, out, n);
    return 0;
}

/* A helper of an earlier session on the same X server (the stick pulled and put back within its session's 30 s grace)
 * would give the cursor back over this one: it is stopped first, and waited for (2 s at most), so its restore comes
 * before the hiding here. This one is connected by then, so the server does not start over in between. Helpers on
 * other servers (another display on a desktop) are left alone. */
static void stop_older_helpers(void) {
    DIR *d = opendir("/proc");
    if (!d) return;
    char mine[128], theirs[128];
    display_server(getenv("DISPLAY"), mine, sizeof mine);
    pid_t self = getpid(), older[8];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 8) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0 || pid == self) continue;
        char path[64], cmd[512];
        snprintf(path, sizeof path, "/proc/%ld/cmdline", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t len = fread(cmd, 1, sizeof cmd - 1, f);
        fclose(f);
        int helper = 0;
        for (size_t i = 0; i < len; i += strlen(cmd + i) + 1) {
            cmd[len] = 0;
            if (!strcmp(cmd + i, "--hide-cursor")) helper = 1;
        }
        if (!helper || process_server(pid, theirs, sizeof theirs) != 0 || strcmp(theirs, mine)) continue;
        if (kill((pid_t)pid, SIGTERM) == 0) older[n++] = (pid_t)pid;
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        double until = bm_now() + 2.0;
        while (kill(older[i], 0) == 0 && bm_now() < until) usleep(20 * 1000);
        bm_log("cursor: the helper of an earlier session (pid %d) %s", (int)older[i],
               kill(older[i], 0) == 0 ? "is still there" : "has stopped");
    }
}

int x11_hide_cursor(double wait_s) {
    char err[200];
    if (x_load(err, sizeof err) != 0) {
        bm_log("cursor: %s", err);
        return 1;
    }
    double until = bm_now() + wait_s;
    while (!(X.dpy = X.XOpenDisplay(NULL))) {
        if (bm_now() > until) {
            bm_log("cursor: no X display after %.0f s: the cursor is left as it is", wait_s);
            return 1;
        }
        usleep(500 * 1000);
    }
    stop_older_helpers();
    static const char empty[1];
    struct {
        unsigned long pixel;
        unsigned short red, green, blue;
        char flags, pad;
    } black = {0, 0, 0, 0, 0, 0}; /* Xlib's XColor */
    Window root = X.XRootWindow(X.dpy, X.XDefaultScreen(X.dpy));
    unsigned long pix = X.XCreateBitmapFromData(X.dpy, root, empty, 1, 1);
    unsigned long cursor = X.XCreatePixmapCursor(X.dpy, pix, pix, &black, &black, 0, 0);
    X.XDefineCursor(X.dpy, root, cursor);
    X.XFreeCursor(X.dpy, cursor);
    X.XFreePixmap(X.dpy, pix);
    X.XFlush(X.dpy);
    bm_log("cursor: the X server's cursor is hidden, holding the connection");
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = cursor_on_term; /* no SA_RESTART: poll() returns at once */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    struct pollfd p = {X.XConnectionNumber(X.dpy), POLLIN, 0};
    while (!cursor_stop) {
        if (poll(&p, 1, 1000) > 0) /* a timeout: a SIGTERM just before poll() is seen within a second */
            while (X.XPending(X.dpy)) { /* no events are asked for. A closed connection ends the process here */
                XEvent ev;
                X.XNextEvent(X.dpy, &ev);
            }
    }
    X.XUndefineCursor(X.dpy, root);
    X.XCloseDisplay(X.dpy); /* flushes */
    bm_log("cursor: the X server's cursor is back");
    return 0;
}

int x11_open(int w, int h, int fullscreen, char *err, size_t errlen) {
    if (x_load(err, errlen) != 0) return -1;
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
