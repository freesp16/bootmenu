/* What the menu can launch, and the configuration file.
 *
 *  stick root      <stick>/<file>            an ARM executable beside autoexec.bin
 *                  <stick>/<id>.ini          optional settings for it
 *  stick apps/     <stick>/apps/<file>       same, kept in a folder (settings: apps/<id>.ini)
 *                  <stick>/apps/<id>/        a folder app: app.ini (exe=...) or an executable named <id>
 *  embedded        <embedded>/<id>/          copied out of autoexec.bin by autoexec.sh (app.ini + the executable);
 *                                            a .pending file means the copy is still running
 *  official                                     the unit's own application (the launcher restarts it)
 *
 * Settings (.ini / app.ini, key=value, all optional):
 *   name=  description=  exe= (folder apps)  args=  data= (stick folder bound over /root/data; none = no redirect)
 *   conf= (stick file exported as <prefix><KEY>=value)  prefix=  run_from=ram|stick  accent=#rrggbb  hidden=1
 * Defaults: data=<id>-data, conf=<id>.conf, prefix=<ID>_.
 *
 * An executable is compatible when it is a 32-bit little-endian ARM ELF executable (ET_EXEC, or ET_DYN with an
 * interpreter), EABI, soft-float calling convention (the unit's glibc is armel). */
#include "bm.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- small helpers --------------------------------------------------------------------------------------------- */
static void copy(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

/* Calls kv(key, value) for each key=value line of a file (# comments, CR tolerated). 0 = file read. */
static int read_kv(const char *path, void (*kv)(void *, const char *, const char *), void *user) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        kv(user, trim(s), trim(eq + 1));
    }
    fclose(f);
    return 0;
}

/* ---- configuration --------------------------------------------------------------------------------------------- */
void config_defaults(Config *c) {
    memset(c, 0, sizeof *c);
    copy(c->stick, sizeof c->stick, ".");
    copy(c->default_id, sizeof c->default_id, "last");
    c->timeout_s = 8;
    c->scan_root = 1;
    c->show_official = 1;
    c->official_wait_ms = 8000;
}

static void config_kv(void *user, const char *k, const char *v) {
    Config *c = user;
    if (!strcmp(k, "default")) copy(c->default_id, sizeof c->default_id, v);
    else if (!strcmp(k, "timeout")) c->timeout_s = atoi(v);
    else if (!strcmp(k, "scan_root")) c->scan_root = atoi(v);
    else if (!strcmp(k, "show_official") || !strcmp(k, "show_stock")) c->show_official = atoi(v);
    else if (!strcmp(k, "official_wait_ms") || !strcmp(k, "stock_wait_ms")) c->official_wait_ms = atoi(v);
    /* network keys (ssh=, ssh_ip=, ...) belong to the launcher */
}

void config_load(Config *c, const char *path) {
    if (read_kv(path, config_kv, c) == 0)
        bm_log("config %s: default=%s timeout=%d scan_root=%d show_official=%d", path, c->default_id, c->timeout_s,
               c->scan_root, c->show_official);
    else
        bm_log("config %s: not found, defaults (default=%s timeout=%d)", path, c->default_id, c->timeout_s);
    if (c->timeout_s < 0) c->timeout_s = 0;
    if (c->official_wait_ms < 500) c->official_wait_ms = 500;
}

/* ---- ELF check ------------------------------------------------------------------------------------------------- */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int elf_compatible(const char *path, char *why, size_t whylen) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return snprintf(why, whylen, "cannot open"), 0;
    uint8_t h[52];
    int ok = 0;
    if (read(fd, h, sizeof h) != (ssize_t)sizeof h || memcmp(h, "\177ELF", 4) != 0) {
        snprintf(why, whylen, "not an ELF file");
#ifdef BM_DEV_HOST
    } else if (h[4] == 2 && h[5] == 1 && le16(h + 18) == 62) {
        ok = 1; /* development build on a PC: list native x86-64 programs too (never in the armel build) */
#endif
    } else if (h[4] != 1 || h[5] != 1 || le16(h + 18) != 40) {
        snprintf(why, whylen, "not 32-bit little-endian ARM");
    } else {
        uint16_t type = le16(h + 16), phnum = le16(h + 44), phentsize = le16(h + 42);
        uint32_t flags = le32(h + 36), phoff = le32(h + 28);
        char interp[64] = "";
        for (int i = 0; i < phnum && i < 32 && phentsize >= 32; i++) {
            uint8_t ph[32];
            if (pread(fd, ph, sizeof ph, (off_t)phoff + (off_t)i * phentsize) != (ssize_t)sizeof ph) break;
            if (le32(ph) == 3 /* PT_INTERP */) {
                uint32_t off = le32(ph + 4), sz = le32(ph + 16);
                if (sz >= sizeof interp) sz = sizeof interp - 1;
                ssize_t r = pread(fd, interp, sz, (off_t)off);
                interp[r > 0 ? r : 0] = 0;
            }
        }
        if (type != 2 && type != 3) snprintf(why, whylen, "not an executable (e_type %u)", type);
        else if (type == 3 && !*interp) snprintf(why, whylen, "a shared library, not a program");
        else if (flags & 0x400 /* EF_ARM_ABI_FLOAT_HARD */ || strstr(interp, "armhf"))
            snprintf(why, whylen, "hard-float (armhf) build; this unit runs armel");
        else if ((flags & 0xff000000u) != 0x05000000u && (flags & 0xff000000u) != 0x04000000u)
            snprintf(why, whylen, "not an EABI binary (e_flags 0x%08x)", flags);
        else ok = 1;
    }
    close(fd);
    return ok;
}

/* ---- app-info note ------------------------------------------------------------------------------------------------
 * In a PT_NOTE segment: namesz=8, descsz, type=1, "APPINFO\0", then the descriptor: "AINF", u16 format=1,
 * u16 header=128, char name[48], char version[24], char id[40], u16 w, u16 h, u32 pixel format=1 (RGBA8), pixels.
 * Only the program headers and the note headers are read (a few hundred bytes); the pixels when drawn. */
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static void fixed_string(char *dst, size_t n, const uint8_t *src, size_t srclen) {
    size_t k = 0;
    while (k < srclen && k + 1 < n && src[k]) {
        dst[k] = (char)src[k];
        k++;
    }
    dst[k] = 0;
}

int elf_appinfo(App *a) {
    int fd = open(a->exe, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    uint8_t h[64];
    int found = 0;
    if (read(fd, h, sizeof h) == (ssize_t)sizeof h && !memcmp(h, "\177ELF", 4) && h[5] == 1) {
        int is64 = h[4] == 2;
        uint64_t phoff = is64 ? le64(h + 32) : le32(h + 28);
        uint16_t phentsize = le16(h + (is64 ? 54 : 42)), phnum = le16(h + (is64 ? 56 : 44));
        for (int i = 0; i < phnum && i < 32 && !found; i++) {
            uint8_t ph[56];
            if (phentsize > sizeof ph || pread(fd, ph, phentsize, (off_t)(phoff + (uint64_t)i * phentsize)) != phentsize)
                break;
            if (le32(ph) != 4 /* PT_NOTE */) continue;
            uint64_t off = is64 ? le64(ph + 8) : le32(ph + 4), size = is64 ? le64(ph + 32) : le32(ph + 16);
            uint64_t pos = off, end = off + size;
            while (pos + 12 <= end && !found) {
                uint8_t nh[20];
                if (pread(fd, nh, 20, (off_t)pos) != 20) break;
                uint32_t namesz = le32(nh), descsz = le32(nh + 4), type = le32(nh + 8);
                uint64_t desc = pos + 12 + ((namesz + 3u) & ~3u);
                if (namesz == 8 && type == 1 && !memcmp(nh + 12, "APPINFO", 8) && descsz >= 128 && desc + descsz <= end) {
                    uint8_t d[128];
                    if (pread(fd, d, sizeof d, (off_t)desc) == (ssize_t)sizeof d && !memcmp(d, "AINF", 4) &&
                        le16(d + 4) == 1) {
                        uint16_t hdr = le16(d + 6), w = le16(d + 120), ht = le16(d + 122);
                        found = 1;
                        if (!strcmp(a->name, a->id)) fixed_string(a->name, sizeof a->name, d + 8, 48);
                        fixed_string(a->version, sizeof a->version, d + 56, 24);
                        int pot = w >= 16 && w <= 256 && !(w & (w - 1)) && ht == w;
                        if (pot && le32(d + 124) == 1 && hdr >= 128 && (uint64_t)hdr + (uint64_t)w * ht * 4 <= descsz) {
                            a->icon_w = w, a->icon_h = ht;
                            a->icon_off = (long)(desc + hdr);
                        }
                    }
                }
                pos = desc + ((descsz + 3u) & ~3u);
            }
        }
    }
    close(fd);
    return found;
}

int apps_load_icon(const App *a, uint8_t *dst) {
    if (a->icon_w <= 0) return -1;
    int fd = open(a->exe, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = (size_t)a->icon_w * (size_t)a->icon_h * 4;
    ssize_t r = pread(fd, dst, n, (off_t)a->icon_off);
    close(fd);
    return r == (ssize_t)n ? 0 : -1;
}

/* ---- app entries ----------------------------------------------------------------------------------------------- */
static const uint32_t PALETTE[] = {0x2ad4a5, 0x4f8cff, 0x9b7bff, 0xff6b9a, 0xff8a3d, 0x34c6e8, 0x9ad94a};
#define OFFICIAL_ACCENT 0xffb020u

static uint32_t accent_for(const char *id) {
    uint32_t h = 2166136261u;
    for (const char *p = id; *p; p++) h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 16777619u;
    return PALETTE[h % (sizeof PALETTE / sizeof PALETTE[0])];
}

typedef struct {
    App *a;
    int hidden;
} IniCtx;

static void app_kv(void *user, const char *k, const char *v) {
    IniCtx *x = user;
    App *a = x->a;
    if (!strcmp(k, "name")) copy(a->name, sizeof a->name, v);
    else if (!strcmp(k, "description")) copy(a->desc, sizeof a->desc, v);
    else if (!strcmp(k, "exe")) {
        if (*v == '/') copy(a->exe, sizeof a->exe, v);
        else snprintf(a->exe, sizeof a->exe, "%s/%s", a->dir, v);
    } else if (!strcmp(k, "args")) copy(a->args, sizeof a->args, v);
    else if (!strcmp(k, "data")) copy(a->data, sizeof a->data, v);
    else if (!strcmp(k, "conf")) copy(a->conf, sizeof a->conf, v);
    else if (!strcmp(k, "prefix")) copy(a->prefix, sizeof a->prefix, v);
    else if (!strcmp(k, "run_from")) copy(a->run_from, sizeof a->run_from, !strcmp(v, "stick") ? "stick" : "ram");
    else if (!strcmp(k, "accent") && *v == '#') a->accent = (uint32_t)strtoul(v + 1, NULL, 16) & 0xffffff;
    else if (!strcmp(k, "hidden")) x->hidden = atoi(v);
}

/* The id of a file: its name without a trailing .bin / .elf / .app. */
static void id_from_file(char *id, size_t n, const char *file) {
    copy(id, n, file);
    char *dot = strrchr(id, '.');
    if (dot && (!strcasecmp(dot, ".bin") || !strcasecmp(dot, ".elf") || !strcasecmp(dot, ".app"))) *dot = 0;
}

static void app_defaults(App *a, AppSource src, const char *id, const char *dir) {
    memset(a, 0, sizeof *a);
    a->source = src;
    copy(a->id, sizeof a->id, id);
    copy(a->name, sizeof a->name, id);
    copy(a->dir, sizeof a->dir, dir);
    snprintf(a->data, sizeof a->data, "%s-data", id);
    snprintf(a->conf, sizeof a->conf, "%s.conf", id);
    size_t k = 0;
    for (const char *p = id; *p && k + 2 < sizeof a->prefix; p++)
        a->prefix[k++] = isalnum((unsigned char)*p) ? (char)toupper((unsigned char)*p) : '_';
    a->prefix[k++] = '_';
    a->prefix[k] = 0;
    copy(a->run_from, sizeof a->run_from, "ram");
    a->accent = accent_for(id);
    a->ready = 1;
}

static void human_size(char *out, size_t n, long long bytes) {
    if (bytes >= 1024 * 1024) snprintf(out, n, "%.1f MB", bytes / (1024.0 * 1024.0));
    else snprintf(out, n, "%lld KB", (bytes + 1023) / 1024);
}

static void finish_detail(App *a) {
    static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char size[24], date[24] = "", ver[32] = "";
    if (*a->version) snprintf(ver, sizeof ver, "v%s \xC2\xB7 ", a->version);
    human_size(size, sizeof size, a->size);
    time_t t = (time_t)a->mtime;
    struct tm tm;
    if (a->mtime > 0 && localtime_r(&t, &tm) && tm.tm_year > 90)
        snprintf(date, sizeof date, " \xC2\xB7 %d %s %d", tm.tm_mday, mon[tm.tm_mon], tm.tm_year + 1900);
    const char *where = a->source == SRC_STICK ? "On the USB stick" : "Inside autoexec.bin";
    if (!a->ready) snprintf(a->detail, sizeof a->detail, "Unpacking from autoexec.bin\xE2\x80\xA6");
    else if (*a->desc) snprintf(a->detail, sizeof a->detail, "%s \xC2\xB7 %s%s", a->desc, ver, size);
    else snprintf(a->detail, sizeof a->detail, "%s \xC2\xB7 %s%s%s", where, ver, size, date);
}

/* Adds the app if its executable is compatible (or it is still being unpacked). Settings come from `ini`. */
static void consider(AppList *l, AppSource src, const char *id, const char *dir, const char *exe, const char *ini,
                     int pending) {
    if (l->n >= BM_MAX_APPS) return;
    App *a = &l->app[l->n];
    app_defaults(a, src, id, dir);
    if (exe) copy(a->exe, sizeof a->exe, exe);
    IniCtx x = {a, 0};
    if (ini) read_kv(ini, app_kv, &x);
    if (x.hidden) return;
    a->ready = !pending;
    if (!*a->exe) {
        if (pending) a->ready = 0;
        else { bm_log("apps: %s: no executable (exe= in app.ini)", dir); l->skipped++; return; }
    }
    if (a->ready) {
        char why[96];
        struct stat st;
        if (stat(a->exe, &st) != 0 || !S_ISREG(st.st_mode)) {
            bm_log("apps: skip %s: missing", a->exe);
            l->skipped++;
            return;
        }
        if (!elf_compatible(a->exe, why, sizeof why)) {
            bm_log("apps: skip %s: %s", a->exe, why);
            l->skipped++;
            return;
        }
        a->size = (long long)st.st_size;
        a->mtime = (long)st.st_mtime;
        if (elf_appinfo(a))
            bm_log("apps: %s: app info \"%s\" %s, icon %dx%d", a->exe, a->name, a->version, a->icon_w, a->icon_h);
    }
    finish_detail(a);
    l->n++;
}

static int skip_by_name(const char *name) {
    static const char *ext[] = {".wav", ".aif", ".aiff", ".mp3", ".flac", ".txt", ".md", ".log", ".conf", ".ini",
                                ".json", ".prj", ".dat", ".key", ".last", ".sh", ".py", ".png", ".jpg", ".pdf",
                                ".zip", ".gz", ".iso", ".wad", ".upd"};
    if (name[0] == '.' || !strcasecmp(name, "autoexec.bin") || !strcmp(name, "bootmenu")) return 1;
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

/* Executables directly in `dir` (the stick root or apps/), settings in <dir>/<id>.ini. */
static void scan_files(AppList *l, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (skip_by_name(e->d_name)) continue;
        char path[320], id[64], ini[384];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 52) continue;
        char why[96];
        if (!elf_compatible(path, why, sizeof why)) {
            if (strcmp(why, "not an ELF file") != 0) {
                bm_log("apps: skip %s: %s", path, why);
                l->skipped++;
            }
            continue;
        }
        id_from_file(id, sizeof id, e->d_name);
        snprintf(ini, sizeof ini, "%s/%s.ini", dir, id);
        consider(l, SRC_STICK, id, dir, path, ini, 0);
    }
    closedir(d);
}

/* Folder apps: <parent>/<id>/ with app.ini (exe=) or an executable named <id>. */
static void scan_folders(AppList *l, const char *parent, AppSource src) {
    DIR *d = opendir(parent);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[320], ini[384], exe[384], pend[384];
        snprintf(dir, sizeof dir, "%s/%s", parent, e->d_name);
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        snprintf(ini, sizeof ini, "%s/app.ini", dir);
        snprintf(exe, sizeof exe, "%s/%s", dir, e->d_name);
        snprintf(pend, sizeof pend, "%s/.pending", dir);
        int pending = access(pend, F_OK) == 0;
        consider(l, src, e->d_name, dir, access(exe, F_OK) == 0 || pending ? exe : NULL,
                 access(ini, F_OK) == 0 ? ini : NULL, pending);
    }
    closedir(d);
}

static int by_name(const void *a, const void *b) {
    const App *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcasecmp(x->id, y->id);
}

/* The official application's size for each official release (PioneerSP16FirmwareResearch/docs/firmware-reference.md,
 * "Application binary identities"). The sizes are all different, so one stat() tells the installed version; a
 * SHA-256 of the 12 MB file would cost a noticeable part of a second on the A9 at every start. */
static const struct {
    long long bytes;
    const char *version;
} OFFICIAL_RELEASES[] = {
    {10724172, "1.20"}, {11172172, "1.30"}, {11171172, "1.31"},
    {11676132, "1.40"}, {11993612, "1.50"}, {12000396, "1.60"},
};

static void official_entry(App *a) {
    app_defaults(a, SRC_OFFICIAL, "official", "/root/pdj");
    copy(a->name, sizeof a->name, "Pioneer DJ - Toraiz SP-16");
    copy(a->exe, sizeof a->exe, "/root/pdj/EP085_Main_R");
    copy(a->data, sizeof a->data, "none");
    a->accent = OFFICIAL_ACCENT;
    struct stat st;
    if (stat(a->exe, &st) != 0) {
        copy(a->detail, sizeof a->detail, "Official firmware \xC2\xB7 not found on this system");
        bm_log("official: %s not found", a->exe);
        return;
    }
    a->size = (long long)st.st_size;
    a->mtime = (long)st.st_mtime;
    const char *v = NULL;
    for (size_t i = 0; i < sizeof OFFICIAL_RELEASES / sizeof OFFICIAL_RELEASES[0]; i++)
        if (OFFICIAL_RELEASES[i].bytes == a->size) v = OFFICIAL_RELEASES[i].version;
    if (v) {
        snprintf(a->detail, sizeof a->detail, "Official firmware \xC2\xB7 version %s", v);
    } else {
        snprintf(a->detail, sizeof a->detail, "Official firmware \xC2\xB7 unknown version (%lld bytes)", a->size);
    }
    bm_log("official: %s %lld bytes -> %s", a->exe, a->size, v ? v : "unknown version");
}

void apps_scan(AppList *l, const Config *c) {
    double t0 = bm_now();
    l->n = l->skipped = 0;
    /* the official firmware always comes first */
    if (c->show_official) official_entry(&l->app[l->n++]);
    int first = l->n;
    char apps[300];
    snprintf(apps, sizeof apps, "%s/apps", c->stick);
    if (c->scan_root) scan_files(l, c->stick);
    scan_files(l, apps);
    scan_folders(l, apps, SRC_STICK);
    int stick_end = l->n;
    qsort(l->app + first, (size_t)(stick_end - first), sizeof(App), by_name);
    if (*c->embedded) scan_folders(l, c->embedded, SRC_EMBEDDED);
    qsort(l->app + stick_end, (size_t)(l->n - stick_end), sizeof(App), by_name);
    bm_log("apps: %d listed, %d skipped (%.1f ms)", l->n, l->skipped, (bm_now() - t0) * 1e3);
    for (int i = 0; i < l->n; i++)
        bm_log("apps:   [%d] %s@%s \"%s\" %s%s", i, l->app[i].id,
               l->app[i].source == SRC_STICK ? "stick" : l->app[i].source == SRC_EMBEDDED ? "embedded" : "official",
               l->app[i].name, l->app[i].exe, l->app[i].ready ? "" : " (unpacking)");
}

int apps_any_unready(const AppList *l) {
    for (int i = 0; i < l->n; i++)
        if (!l->app[i].ready) return 1;
    return 0;
}

int apps_find(const AppList *l, const char *spec) {
    char id[96];
    copy(id, sizeof id, spec);
    char *at = strchr(id, '@');
    int want = -1; /* -1 any: the stick first, then the image */
    if (at) {
        *at = 0;
        want = !strcmp(at + 1, "stick") ? SRC_STICK : !strcmp(at + 1, "embedded") ? SRC_EMBEDDED : SRC_OFFICIAL;
    }
    if (!strcmp(id, "stock")) copy(id, sizeof id, "official"); /* the name before 1.1 (bootmenu.last, conf) */
    if (!strcmp(id, "official")) want = SRC_OFFICIAL;
    for (int pass = 0; pass < 3; pass++) {
        if (want >= 0 && pass != want) continue;
        for (int i = 0; i < l->n; i++)
            if ((int)l->app[i].source == pass && !strcmp(l->app[i].id, id)) return i;
    }
    return -1;
}

int apps_default(const AppList *l, const Config *c) {
    char spec[96];
    copy(spec, sizeof spec, c->default_id);
    if (!*spec || !strcmp(spec, "none")) return -1;
    if (!strcmp(spec, "last")) {
        char path[300];
        snprintf(path, sizeof path, "%s/bootmenu.last", c->stick);
        FILE *f = fopen(path, "r");
        if (!f) return -1;
        if (!fgets(spec, sizeof spec, f)) spec[0] = 0;
        fclose(f);
        char *t = trim(spec);
        memmove(spec, t, strlen(t) + 1);
        if (!*spec) return -1;
    }
    return apps_find(l, spec);
}

static const char *source_name(AppSource s) {
    return s == SRC_STICK ? "stick" : s == SRC_EMBEDDED ? "embedded" : "official";
}

void apps_remember(const Config *c, const App *a) {
    char path[300], tmp[310];
    snprintf(path, sizeof path, "%s/bootmenu.last", c->stick);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%s@%s\n", a->id, source_name(a->source));
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path) != 0) unlink(tmp);
}

/* 'value' with every ' written as '\'' so the launcher can source the file. */
static void put(FILE *f, const char *key, const char *value) {
    fprintf(f, "%s='", key);
    for (const char *p = value; *p; p++) {
        if (*p == '\'') fputs("'\\''", f);
        else if (*p != '\n' && *p != '\r') fputc(*p, f);
    }
    fputs("'\n", f);
}

int apps_write_choice(const App *a, const char *path) {
    char tmp[300], num[32];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        bm_log("choice: cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }
    put(f, "BM_KIND", a->source == SRC_OFFICIAL ? "official" : "app");
    put(f, "BM_ID", a->id);
    put(f, "BM_NAME", a->name);
    put(f, "BM_SOURCE", source_name(a->source));
    put(f, "BM_EXE", a->exe);
    put(f, "BM_DIR", a->dir);
    put(f, "BM_ARGS", a->args);
    put(f, "BM_DATA", a->data);
    put(f, "BM_CONF", a->conf);
    put(f, "BM_PREFIX", a->prefix);
    put(f, "BM_RUN_FROM", a->run_from);
    snprintf(num, sizeof num, "%lld", a->size);
    put(f, "BM_SIZE", num);
    snprintf(num, sizeof num, "%ld", a->mtime);
    put(f, "BM_MTIME", num);
    int bad = ferror(f);
    if (fclose(f) != 0 || bad || rename(tmp, path) != 0) {
        unlink(tmp);
        bm_log("choice: cannot write %s", path);
        return -1;
    }
    bm_log("choice: %s@%s \"%s\" %s -> %s", a->id, source_name(a->source), a->name, a->exe, path);
    return 0;
}
