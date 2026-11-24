/* Boot menu: entry point, start-up sequence and the frame loop.
 *
 *   bootmenu --stick DIR [--embedded DIR] [--out FILE] [--render fb|x11|host] [--fallback] [--message TEXT]
 *   bootmenu --stick DIR [--embedded DIR] --resolve SPEC|default [--out FILE]    (no screen: write a choice)
 *   bootmenu --stick DIR [--embedded DIR] --list
 *   bootmenu --decrypt FILE [--key KEYFILE]           (an encrypted program, decrypted, on stdout)
 *   bootmenu --wake-backlight [--settings FILE]       (the LCD back to the official app's brightness, then exit)
 *
 * Start-up order (every step logged with its time, "BOOTMENU <ms> ..."):
 *   1. SIGTERM to the official app            it shuts down while we do 2 and 3
 *   2. load the GL driver (dlopen)         no display access yet
 *   3. scan the stick and the image        reads directory entries and 52-byte ELF headers only
 *   4. wait until the official app is gone    SIGKILL after official_wait_ms; then the backlight (its screen
 *                                             saver may have dimmed it, and only it would undo that)
 *   5. EGL window + context, shader, atlas
 *   6. the panel link
 *   7. first frame
 *
 * Exit codes: 0 a choice was written to --out; 2 graphics failed (the launcher runs us again with --render x11
 * --fallback); 3 graphics failed and --fallback wrote the default (or official) choice; 4 nothing could be chosen;
 * 5 stopped by a signal or q (no choice); 6 the stick was pulled (the launcher starts the official app at once); 1 usage;
 * 7 --decrypt failed; 8 --wake-backlight could not set the backlight. --key: the unit's key file for encrypted programs
 * (default BM_KEY). --settings: the official app's settings file, for its LCD BRIGHTNESS (default BM_SETTINGS). */
#define _GNU_SOURCE
#include "bm.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double t_start;
static volatile sig_atomic_t stop_requested;

double bm_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

double bm_ms_since_start(void) { return (bm_now() - t_start) * 1e3; }

void bm_log(const char *fmt, ...) {
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "BOOTMENU %7.1f ms %s\n", bm_ms_since_start(), line);
}

/* The stick: when it was a mount point at start, it is gone once its mount or its block device node disappears
 * (pulled out). Checked every 200 ms: one read of /proc/mounts. */
static struct {
    int watched;
    char path[256], dev[128];
} stick;

static int mount_of(const char *path, char *dev, size_t devlen) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char src[256], mnt[256];
    int found = 0;
    while (!found && fscanf(f, "%255s %255s %*[^\n]", src, mnt) == 2)
        if (!strcmp(mnt, path)) found = 1, snprintf(dev, devlen, "%s", src);
    fclose(f);
    return found;
}

static void stick_watch_init(const char *path) {
    snprintf(stick.path, sizeof stick.path, "%s", path);
    stick.watched = mount_of(path, stick.dev, sizeof stick.dev);
    bm_log("stick: %s %s%s", path, stick.watched ? "mounted from " : "not a mount point (not watched)",
           stick.watched ? stick.dev : "");
}

static int stick_gone(void) {
    if (!stick.watched) return 0;
    char dev[128];
    if (!mount_of(stick.path, dev, sizeof dev)) return 1;
    return !strncmp(stick.dev, "/dev/", 5) && access(stick.dev, F_OK) != 0;
}

static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void usage(void) {
    fprintf(stderr,
            "Boot menu " BM_VERSION "\n"
            "usage: bootmenu --stick DIR [--embedded DIR] [--conf FILE] [--out FILE]\n"
            "                [--render fb|x11|host] [--fallback] [--message TEXT] [--no-official-stop]\n"
            "                [--screenshot FILE.ppm] [--exit-after FRAMES] [--keys KEYS]\n"
            "       bootmenu --stick DIR [--embedded DIR] --resolve SPEC|default [--out FILE]\n"
            "       bootmenu --stick DIR [--embedded DIR] --list\n"
            "       bootmenu --decrypt FILE [--key KEYFILE]\n"
            "       bootmenu --wake-backlight [--settings FILE]\n"
            "       bootmenu --version\n");
}

typedef struct {
    Config cfg;
    const char *conf, *out, *render, *message, *resolve, *shot, *keys, *decrypt, *settings;
    int fallback, list, no_official_stop, exit_after, wake_backlight;
} Args;

static int parse(Args *a, int argc, char **argv) {
    memset(a, 0, sizeof *a);
    config_defaults(&a->cfg);
    a->out = "/tmp/bootmenu/choice.sh";
    a->render = "fb";
    a->settings = BM_SETTINGS;
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define VAL(dst) if (!v) return -1; dst = v; i++; continue;
        if (!strcmp(k, "--stick")) { if (!v) return -1; snprintf(a->cfg.stick, sizeof a->cfg.stick, "%s", v); i++; continue; }
        if (!strcmp(k, "--embedded")) { if (!v) return -1; snprintf(a->cfg.embedded, sizeof a->cfg.embedded, "%s", v); i++; continue; }
        if (!strcmp(k, "--conf")) { VAL(a->conf) }
        if (!strcmp(k, "--out")) { VAL(a->out) }
        if (!strcmp(k, "--render")) { VAL(a->render) }
        if (!strcmp(k, "--message")) { VAL(a->message) }
        if (!strcmp(k, "--resolve")) { VAL(a->resolve) }
        if (!strcmp(k, "--screenshot")) { VAL(a->shot) }
        if (!strcmp(k, "--keys")) { VAL(a->keys) }
        if (!strcmp(k, "--decrypt")) { VAL(a->decrypt) }
        if (!strcmp(k, "--settings")) { VAL(a->settings) }
        if (!strcmp(k, "--key")) { if (!v) return -1; snprintf(a->cfg.key, sizeof a->cfg.key, "%s", v); i++; continue; }
        if (!strcmp(k, "--exit-after")) { if (!v) return -1; a->exit_after = atoi(v); i++; continue; }
        if (!strcmp(k, "--fallback")) { a->fallback = 1; continue; }
        if (!strcmp(k, "--list")) { a->list = 1; continue; }
        if (!strcmp(k, "--no-official-stop")) { a->no_official_stop = 1; continue; }
        if (!strcmp(k, "--wake-backlight")) { a->wake_backlight = 1; continue; }
        if (!strcmp(k, "--help") || !strcmp(k, "-h")) return -1;
        if (!strcmp(k, "--version")) {
            printf("bootmenu %s\n", BM_VERSION);
            exit(0);
        }
#undef VAL
        fprintf(stderr, "bootmenu: unknown argument %s\n", k);
        return -1;
    }
    return 0;
}

/* The entry used without a screen: the configured default, else the official software, else the first. */
static int headless_pick(const AppList *l, const Config *c) {
    int i = apps_default(l, c);
    if (i < 0) i = apps_find(l, "official");
    if (i < 0 && l->n) i = 0;
    return i;
}

static void stop_official(const Args *a) {
    if (a->no_official_stop) return;
    if (official_wait_gone(a->cfg.official_wait_ms)) {
        bm_log("official: gone");
        return;
    }
    bm_log("official: still running after %d ms, SIGKILL", a->cfg.official_wait_ms);
    official_signal(SIGKILL);
    bm_log("official: %s", official_wait_gone(2000) ? "gone after SIGKILL" : "WARNING still there");
}

/* Once the official app is gone (never while it runs: the backlight is its own then). */
static int restore_backlight(const Args *a) {
    char info[200];
    if (official_running()) {
        bm_log("backlight: left to the official app, still running");
        return -1;
    }
    int rc = backlight_wake(a->settings, info, sizeof info);
    bm_log("%s", info);
    return rc;
}

/* --keys "dddp" scripts the menu for screenshots and tests: d/u = turn, p = push, b = back, h = home, one key every
 * 150 ms from 0.8 s on; '.' waits one step. */
static int scripted(const char *keys, double age, int *next, InputEvent *e) {
    if (!keys || age < 0.8 + 0.15 * *next) return 0;
    int k = (*next)++;
    if (k >= (int)strlen(keys)) return 0;
    switch (keys[k]) {
    case 'd': *e = (InputEvent){IN_TURN, 1, 0, 0}; return 1;
    case 'u': *e = (InputEvent){IN_TURN, -1, 0, 0}; return 1;
    case 'p': *e = (InputEvent){IN_PUSH, 0, 0, 0}; return 1;
    case 'b': *e = (InputEvent){IN_BACK, 0, 0, 0}; return 1;
    case 'h': *e = (InputEvent){IN_HOME, 0, 0, 0}; return 1;
    default: return 0;
    }
}

int main(int argc, char **argv) {
    t_start = bm_now();
    setvbuf(stderr, NULL, _IOLBF, 0);
    static Args a;
    if (parse(&a, argc, argv) != 0) {
        usage();
        return 1;
    }
    /* An encrypted program, decrypted to stdout: the launcher writes it to RAM and runs it. */
    if (a.decrypt) {
        if (!crypt_key_load(a.cfg.key)) return 7;
        return crypt_decrypt_file(a.decrypt, STDOUT_FILENO) == 0 ? 0 : 7;
    }
    /* The launcher's path without the menu (ctl run): the official app was stopped by the launcher. */
    if (a.wake_backlight) return restore_backlight(&a) == 0 ? 0 : 8;
    char conf[320];
    if (a.conf) snprintf(conf, sizeof conf, "%s", a.conf);
    else snprintf(conf, sizeof conf, "%s/bootmenu.conf", a.cfg.stick);
    config_load(&a.cfg, conf);
    static AppList apps;

    /* No screen: --list prints, --resolve writes a choice (the launcher's `ctl run ID` and the headless path). */
    if (a.list || a.resolve) {
        apps_scan(&apps, &a.cfg);
        if (a.list) {
            for (int i = 0; i < apps.n; i++)
                printf("%s@%s\t%s\t%s\n", apps.app[i].id,
                       apps.app[i].source == SRC_STICK ? "stick" : apps.app[i].source == SRC_EMBEDDED ? "embedded" : "official",
                       apps.app[i].name, apps.app[i].exe);
            return 0;
        }
        int i = !strcmp(a.resolve, "default") ? headless_pick(&apps, &a.cfg) : apps_find(&apps, a.resolve);
        if (i < 0) {
            bm_log("resolve: no app %s", a.resolve);
            return 4;
        }
        return apps_write_choice(&apps.app[i], a.out) == 0 ? 0 : 4;
    }

    bm_log("start v" BM_VERSION " render=%s stick=%s embedded=%s", a.render, a.cfg.stick,
           *a.cfg.embedded ? a.cfg.embedded : "-");
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    stick_watch_init(a.cfg.stick);

    /* 1. the official app starts shutting down now */
    if (!a.no_official_stop) bm_log("official: SIGTERM to %d process(es)", official_signal(SIGTERM));

    /* 2. + 3. while it does */
    char err[320] = "";
    int gl_ok = gfx_load(a.render, err, sizeof err) == 0;
    bm_log("gl: %s %s", gl_ok ? "driver loaded" : "load FAILED:", gl_ok ? a.render : err);
    apps_scan(&apps, &a.cfg);

    /* 4. */
    stop_official(&a);
    restore_backlight(&a);
    if (stick_gone()) {
        bm_log("stick: removed during start-up; exit for the official app");
        return 6;
    }

    /* 5. */
    if (gl_ok && gfx_open(err, sizeof err) != 0) gl_ok = 0;
    if (!gl_ok) {
        bm_log("gl: FAILED (%s): %s", a.render, err);
        if (!a.fallback) return 2;
        /* After a failed launch (--message), never pick that default again blindly: give the unit back instead. */
        int i = a.message && *a.message ? apps_find(&apps, "official") : headless_pick(&apps, &a.cfg);
        if (i < 0 || apps_write_choice(&apps.app[i], a.out) != 0) return 4;
        return 3;
    }
    bm_log("gl: %s", gfx_info());

    /* 6. */
    char info[200];
    if (panel_open(info, sizeof info) == 0) bm_log("%s", info);
    else bm_log("%s (no panel input; touch and keys only where an X window exists)", info);

    int def = apps_default(&apps, &a.cfg);
    const char *def_label = !strcmp(a.cfg.default_id, "last") ? "LAST" : "DEFAULT";
    Menu *m = menu_create(&apps, def, a.cfg.timeout_s, a.message, def_label);
    if (def >= 0) bm_log("default: %s (%s), countdown %d s", apps.app[def].id, def_label, a.cfg.timeout_s);

    /* 7. the loop: draw at vsync while something moves, otherwise sleep in poll() until input or a refresh */
    int launch = -1, quit = 0, waiting_unpack = 0;
    long frame = 0;
    int key_step = 0, shot_taken = 0;
    double last_draw = 0, last_rescan = bm_now();
    int x11 = strcmp(a.render, "fb") != 0;
    int pulled = 0;
    double last_stick_check = 0;
    while (!stop_requested && !quit) {
        double t = bm_now();
        if (t - last_stick_check >= 0.2) {
            last_stick_check = t;
            if (stick_gone()) {
                pulled = 1;
                break;
            }
        }
        InputEvent ev[64];
        int n = panel_poll(ev, 64);
        if (x11) n += x11_poll(ev + n, 64 - n, &quit);
        if (n < 64 && scripted(a.keys, t - t_start, &key_step, &ev[n])) n++;
        int action = MENU_IDLE;
        for (int i = 0; i < n && action == MENU_IDLE; i++) {
            if (waiting_unpack) continue; /* the choice is made; only waiting for the copy */
            action = menu_input(m, &ev[i]);
        }
        if (!waiting_unpack && action == MENU_IDLE) action = menu_tick(m, t);
        else menu_tick(m, t);

        if (action == MENU_RESCAN || (apps_any_unready(&apps) && t - last_rescan > 0.4)) {
            int s = menu_selected(m);
            char id[64] = "";
            AppSource src = SRC_STICK;
            if (s >= 0) snprintf(id, sizeof id, "%s", apps.app[s].id), src = apps.app[s].source;
            apps_scan(&apps, &a.cfg);
            int keep = -1;
            for (int i = 0; i < apps.n; i++)
                if (apps.app[i].source == src && !strcmp(apps.app[i].id, id)) keep = i;
            menu_set_apps(m, &apps, keep);
            last_rescan = t;
        }
        if (action == MENU_LAUNCH || waiting_unpack) {
            int s = menu_selected(m);
            if (s >= 0 && apps.app[s].ready) {
                launch = s;
                break;
            }
            waiting_unpack = s >= 0;
            if (!waiting_unpack) continue;
        }

        int moving = menu_animating(m) || n > 0 || waiting_unpack;
        if (moving || t - last_draw > 1.0) {
            if (waiting_unpack) menu_draw_launch(m, "Unpacking from autoexec.bin\xE2\x80\xA6");
            else menu_draw(m);
            /* the screenshot: once the intro and the scripted keys have settled */
            if (a.shot && !shot_taken && t - t_start > 1.0 + 0.15 * (a.keys ? (double)strlen(a.keys) : 0) + 0.4)
                gfx_screenshot(a.shot), shot_taken = 1;
            if (gfx_end() != 0) bm_log("gl: swap failed");
            if (frame == 0) bm_log("first frame on screen");
            frame++;
            last_draw = t;
            if (a.exit_after && frame >= a.exit_after && (!a.shot || shot_taken)) break;
        } else {
            struct pollfd p[2];
            int np = 0;
            if (panel_fd() >= 0) p[np++] = (struct pollfd){panel_fd(), POLLIN, 0};
            if (x11 && x11_fd() >= 0) p[np++] = (struct pollfd){x11_fd(), POLLIN, 0};
            poll(p, (nfds_t)np, 50);
        }
    }

    int rc = pulled ? 6 : 5;
    if (launch >= 0) {
        App *app = &apps.app[launch];
        rc = apps_write_choice(app, a.out) == 0 ? 0 : 4;
        if (rc == 0) {
            apps_remember(&a.cfg, app);
            char what[96];
            snprintf(what, sizeof what, "Starting\xE2\x80\xA6");
            /* twice: with two framebuffers both then hold the splash, which stays up while the app loads */
            for (int k = 0; k < 2; k++) {
                menu_tick(m, bm_now());
                menu_draw_launch(m, what);
                if (a.shot && k == 1 && !shot_taken) gfx_screenshot(a.shot);
                gfx_end();
            }
        }
    } else {
        bm_log(pulled ? "stick: removed; exit at once so the launcher starts the official app"
                      : stop_requested ? "stopped by signal" : "quit without a choice");
    }
    panel_close();
    gfx_close();
    bm_log("exit rc=%d after %ld frames", rc, frame);
    return rc;
}
