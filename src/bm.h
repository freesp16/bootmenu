/* Boot menu for the SP-16 USB autoexec hook: shared declarations.
 *
 * The program runs as root in place of the official application. It lists the executables it can launch (apps beside
 * autoexec.bin on the stick, apps embedded in autoexec.bin, the official software), draws an OpenGL ES 2 menu on the
 * Vivante GPU and reads the panel's SELECT encoder, its push, BACK/HOME and the LCD touch. It never launches
 * anything itself: it writes the choice as shell assignments (choice.sh) and exits, so the GPU, the framebuffer
 * and the panel link are free again before the launcher (image/autoexec.sh) starts the app.
 *
 * Plain C99 + POSIX, one thread, no allocation after start-up except the app list (rebuilt on a rescan). */
#ifndef BM_H
#define BM_H

#include <stddef.h>
#include <stdint.h>

#ifndef BM_VERSION
#define BM_VERSION "dev" /* tools/build.sh passes the VERSION file (plus the commit for builds between releases) */
#endif
#define BM_MAX_APPS 48

/* ---- log + time (main.c) -------------------------------------------------------------------------------------- */
double bm_now(void);             /* CLOCK_MONOTONIC seconds */
double bm_ms_since_start(void);  /* ms since main() started */
void bm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2))); /* "BOOTMENU <t> ms ..." on stderr */

/* ---- configuration: bootmenu.conf on the stick ---------------------------------------------------------------- */
typedef struct {
    char stick[256];      /* the stick's mount path (--stick) */
    char embedded[256];   /* folder holding the apps unpacked from autoexec.bin (--embedded) */
    char default_id[96];  /* "last", "official", or an app id (optionally id@stick / id@embedded) */
    int timeout_s;        /* auto-launch countdown when a default exists; 0 = wait */
    int scan_root;        /* 1 = list executables in the stick root (beside autoexec.bin) */
    int show_official;       /* 1 = list the official software */
    int official_wait_ms;    /* how long the official app may take to exit after SIGTERM before SIGKILL */
} Config;
void config_defaults(Config *c);
void config_load(Config *c, const char *path); /* missing file = defaults */

/* ---- apps (apps.c) -------------------------------------------------------------------------------------------- */
typedef enum { SRC_STICK, SRC_EMBEDDED, SRC_OFFICIAL } AppSource;

typedef struct {
    AppSource source;
    char id[64];           /* file name without extension, or the apps/<dir> name; "official" for the official software */
    char name[64];         /* display name (ini name=, else the id) */
    char desc[96];         /* ini description= */
    char exe[320];         /* absolute path of the executable */
    char dir[320];         /* the app's folder: its working directory */
    char args[192];        /* ini args= (split on spaces by the launcher) */
    char data[96];         /* folder on the stick bound over /root/data; "none" = no redirect */
    char conf[96];         /* key=value file on the stick exported as <prefix><KEY> */
    char prefix[48];       /* environment prefix for conf */
    char run_from[8];      /* "ram" (copy to /tmp first, default) or "stick" */
    uint32_t accent;       /* 0xRRGGBB tile colour */
    long long size;        /* executable size in bytes */
    long mtime;
    int ready;             /* embedded apps: 1 once autoexec.sh finished copying them out of the image */
    char detail[96];       /* subtitle text: where it is, version, size, date */
    char version[24];      /* from the executable's app-info note, "" if it has none */
    int icon_w, icon_h;    /* app-info icon (RGBA8), 0 = none: draw the letter tile */
    long icon_off;         /* file offset of the icon pixels */
} App;

typedef struct {
    App app[BM_MAX_APPS];
    int n;
    int skipped;           /* executables seen but not compatible (logged) */
} AppList;

/* Scans the stick (root when scan_root, and apps/), the embedded folder, then adds the official entry. */
void apps_scan(AppList *l, const Config *c);
/* Index of the configured default (default_id, "last" = the stick's bootmenu.last), or -1. */
int apps_default(const AppList *l, const Config *c);
/* "id", "id@stick", "id@embedded" or "official" -> index, or -1. */
int apps_find(const AppList *l, const char *spec);
void apps_remember(const Config *c, const App *a);  /* writes <stick>/bootmenu.last */
int apps_any_unready(const AppList *l);
/* Writes the choice as shell assignments; 0 on success. */
int apps_write_choice(const App *a, const char *path);
/* ELF check: 1 = an ARM 32-bit little-endian soft-float executable this unit can run. `why` gets the reason if not. */
int elf_compatible(const char *path, char *why, size_t whylen);
/* The app-info ELF note (owner "APPINFO", see docs/apps.md): display name, version
 * and an RGBA icon. Fills name (when the app has no ini name=), version and the icon fields; 1 when found. */
int elf_appinfo(App *a);
/* Reads the icon pixels (icon_w * icon_h * 4 bytes) into dst; 0 on success. */
int apps_load_icon(const App *a, uint8_t *dst);

/* ---- official app (official.c) -------------------------------------------------------------------------------------- */
int official_signal(int sig);                  /* signals every live EP085_Main_R; returns how many */
int official_running(void);
int official_wait_gone(int timeout_ms);        /* 1 when gone within the timeout */

/* ---- panel link (panel.c) ------------------------------------------------------------------------------------- */
typedef enum { IN_NONE, IN_TURN, IN_PUSH, IN_BACK, IN_HOME, IN_TOUCH_DOWN, IN_TOUCH_UP } InputKind;
typedef struct {
    InputKind kind;
    int value;  /* IN_TURN: detents (+ = clockwise) */
    int x, y;   /* touch: screen pixels (800 x 480) */
} InputEvent;
int panel_open(char *info, size_t infolen);  /* 0 = running */
int panel_fd(void);                          /* -1 when closed */
/* Reads every frame that is ready (non-blocking) and appends decoded events; returns the count added. */
int panel_poll(InputEvent *out, int max);
void panel_close(void);                      /* timer off, close: the device is idle for the next owner */

/* ---- graphics (gfx.c) ----------------------------------------------------------------------------------------- */
typedef enum { FONT_TITLE, FONT_NAME, FONT_BODY, FONT_HINT, FONT_CHIP, FONT_ICON, FONT_SPLASH, FONT_HERO } FontId;

/* "fb" (Vivante, straight to /dev/fb0), "x11" (Vivante into an X window), "host" (Mesa libEGL/libGLESv2 + X11,
 * development only). gfx_load only loads the libraries (safe while the official app still runs); gfx_open makes the
 * window, context, shaders and the atlas texture. */
int gfx_load(const char *backend, char *err, size_t errlen);
int gfx_open(char *err, size_t errlen);
const char *gfx_info(void);
int gfx_width(void);
int gfx_height(void);

void gfx_begin(uint32_t clear_rgb);
/* Rounded rectangle, anti-aliased. rgba = 0xRRGGBBAA. border > 0: only a ring of that width. soft > 0: a blurred
 * edge of that many pixels (shadows and glows). */
void gfx_rect(float x, float y, float w, float h, float radius, uint32_t rgba, float border, float soft);
/* An RGBA8 image (straight alpha, power-of-two side) as a mipmapped texture; returns its handle (> 0). */
int gfx_image_create(const uint8_t *rgba, int w, int h);
void gfx_image(int tex, float x, float y, float w, float h, float alpha);
/* Vertical gradient (top colour -> bottom colour), rounded. */
void gfx_rect_vgrad(float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom);
/* Text; (x, y) = pen on the baseline. Returns the advance in pixels. UTF-8. */
float gfx_text(FontId f, float x, float y, uint32_t rgba, const char *utf8);
float gfx_text_width(FontId f, const char *utf8);
/* Text shortened with an ellipsis to fit max_w. */
float gfx_text_fit(FontId f, float x, float y, uint32_t rgba, const char *utf8, float max_w);
int gfx_font_cap(FontId f);   /* capital height in pixels */
void gfx_clip(int x, int y, int w, int h); /* scissor for what follows; w <= 0 = off */
int gfx_end(void);            /* flush + swap; 0 on success */
int gfx_screenshot(const char *path); /* the next frame (at gfx_end) as a binary PPM */
void gfx_close(void);         /* fb backend: leaves the shown frame on buffer 0 so the X server's view is coherent */

/* ---- X11 window (x11.c): gl-x11 on the unit, the host development window ------------------------------------- */
int x11_open(int w, int h, int fullscreen, char *err, size_t errlen);
void *x11_display(void);
unsigned long x11_window(void);
int x11_fd(void);
int x11_poll(InputEvent *out, int max, int *quit); /* keyboard: arrows = turn, Return = push, Escape = back */

/* ---- menu (ui.c) ---------------------------------------------------------------------------------------------- */
typedef struct Menu Menu;
typedef enum { MENU_IDLE, MENU_LAUNCH, MENU_RESCAN } MenuAction;
/* default_index >= 0 starts selected there with the countdown; default_label tags it ("LAST" / "DEFAULT"). */
Menu *menu_create(AppList *apps, int default_index, int timeout_s, const char *message, const char *default_label);
void menu_set_apps(Menu *m, AppList *apps, int selected);
/* Applies one input event; returns MENU_LAUNCH when the selected entry should start. */
MenuAction menu_input(Menu *m, const InputEvent *e);
/* Advances animations and the countdown to time t; returns MENU_LAUNCH when the countdown ran out. */
MenuAction menu_tick(Menu *m, double t);
int menu_animating(const Menu *m);  /* 1 while something moves (the loop then draws every vsync) */
void menu_draw(Menu *m);
void menu_draw_launch(Menu *m, const char *what); /* the splash left on screen while the app loads */
int menu_selected(const Menu *m);

#endif
