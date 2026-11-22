/* The menu screen (800 x 480): header, a list of up to four visible rows, a footer with the controls and the
 * countdown. Everything is redrawn each frame from the state below (a few hundred quads). Selection and scrolling
 * are instant (no animation): an encoder detent shows on the next vsync. */
#include "bm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 800
#define H 480
#define MARGIN 24
#define HEADER_H 72
#define LIST_TOP 86
#define ROW_H 70
#define ROW_GAP 10
#define STRIDE (ROW_H + ROW_GAP)
#define VISIBLE 4
#define LIST_H (VISIBLE * STRIDE - ROW_GAP)
#define FOOTER_Y 418

#define BG_TOP 0x0b0f17u
#define BG_BOTTOM 0x121a2bu
#define TEXT 0xeef1f6ffu
#define TEXT_DIM 0x8b95a8ffu
#define TEXT_FAINT 0x5d6678ffu

struct Menu {
    AppList *apps;
    int sel, pressed;
    float sel_pos, scroll;       /* animated: row index of the highlight, first visible row */
    double t, start_t;
    int timeout_s;
    double countdown_end;        /* 0 = no countdown */
    char message[160];
    double message_until;
    char def_id[64];             /* the default entry (shown with a LAST chip) */
    AppSource def_source;
    char def_label[16];
};

static uint32_t rgba(uint32_t rgb, unsigned a) { return rgb << 8 | (a & 255); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float ease_out(float x) {
    x = clampf(x, 0, 1);
    return 1 - (1 - x) * (1 - x) * (1 - x);
}

static uint32_t mix(uint32_t a, uint32_t b, float f) {
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        float x = (float)(a >> s & 255), y = (float)(b >> s & 255);
        out |= (uint32_t)(x + (y - x) * f + 0.5f) << s;
    }
    return out;
}

static int official_index(const Menu *m) {
    for (int i = 0; i < m->apps->n; i++)
        if (m->apps->app[i].source == SRC_OFFICIAL) return i;
    return -1;
}

Menu *menu_create(AppList *apps, int selected, int timeout_s, const char *message, const char *default_label) {
    Menu *m = calloc(1, sizeof *m);
    m->apps = apps;
    m->sel = selected >= 0 && selected < apps->n ? selected : 0;
    m->sel_pos = (float)m->sel;
    m->scroll = m->sel >= VISIBLE ? (float)(m->sel - VISIBLE + 1) : 0;
    m->pressed = -1;
    m->start_t = m->t = bm_now();
    m->timeout_s = timeout_s;
    if (selected >= 0 && selected < apps->n) {
        snprintf(m->def_id, sizeof m->def_id, "%s", apps->app[selected].id);
        m->def_source = apps->app[selected].source;
        snprintf(m->def_label, sizeof m->def_label, "%s", default_label ? default_label : "DEFAULT");
        if (timeout_s > 0) m->countdown_end = m->t + timeout_s;
    }
    if (message && *message) {
        snprintf(m->message, sizeof m->message, "%s", message);
        m->message_until = m->t + 12;
        m->countdown_end = 0; /* after a failed launch, wait for the user */
    }
    return m;
}

void menu_set_apps(Menu *m, AppList *apps, int selected) {
    m->apps = apps;
    if (selected < 0 || selected >= apps->n) selected = m->sel < apps->n ? m->sel : apps->n - 1;
    m->sel = selected < 0 ? 0 : selected;
}

int menu_selected(const Menu *m) { return m->apps->n ? m->sel : -1; }

static int row_at(const Menu *m, int x, int y) {
    if (x < MARGIN || x > W - MARGIN || y < LIST_TOP || y >= LIST_TOP + LIST_H) return -1;
    float f = (float)(y - LIST_TOP) / STRIDE + m->scroll;
    int i = (int)floorf(f);
    if (f - (float)i > (float)ROW_H / STRIDE) return -1; /* the gap between rows */
    return i >= 0 && i < m->apps->n ? i : -1;
}

MenuAction menu_input(Menu *m, const InputEvent *e) {
    m->countdown_end = 0;   /* any input stops the countdown */
    m->message_until = 0;   /* and dismisses a message */
    int n = m->apps->n;
    switch (e->kind) {
    case IN_TURN:
        if (n) {
            int s = m->sel + e->value;
            m->sel = s < 0 ? 0 : s >= n ? n - 1 : s;
        }
        return MENU_IDLE;
    case IN_PUSH: return n ? MENU_LAUNCH : MENU_IDLE;
    case IN_BACK: {
        int s = official_index(m);
        if (s < 0) return MENU_IDLE;
        if (m->sel == s) return MENU_LAUNCH; /* BACK twice: start the official software */
        m->sel = s;
        return MENU_IDLE;
    }
    case IN_HOME: return MENU_RESCAN;
    case IN_TOUCH_DOWN: m->pressed = row_at(m, e->x, e->y); return MENU_IDLE;
    case IN_TOUCH_UP: {
        int r = row_at(m, e->x, e->y);
        int hit = r >= 0 && r == m->pressed;
        m->pressed = -1;
        if (!hit) return MENU_IDLE;
        m->sel = r;
        return MENU_LAUNCH;
    }
    default: return MENU_IDLE;
    }
}

MenuAction menu_tick(Menu *m, double t) {
    m->t = t;
    /* Selection and scrolling are instant: the highlight is on the new row in the very next frame. */
    m->sel_pos = (float)m->sel;
    if (m->sel < m->scroll) m->scroll = (float)m->sel;
    if (m->sel > m->scroll + VISIBLE - 1) m->scroll = (float)(m->sel - VISIBLE + 1);
    if (m->countdown_end > 0 && t >= m->countdown_end) {
        m->countdown_end = 0;
        return MENU_LAUNCH;
    }
    return MENU_IDLE;
}

int menu_animating(const Menu *m) {
    return m->t - m->start_t < 0.6 || m->sel_pos != (float)m->sel || m->countdown_end > 0 ||
           (m->message_until > 0 && m->t < m->message_until + 0.5) || apps_any_unready(m->apps);
}

/* ---- drawing --------------------------------------------------------------------------------------------------- */
static void background(void) {
    gfx_rect_vgrad(0, 0, W, H, 0, rgba(BG_TOP, 255), rgba(BG_BOTTOM, 255));
    /* two soft colour pools for depth */
    gfx_rect(560, -160, 420, 300, 150, rgba(0x2a5cff, 0x16), 0, 180);
    gfx_rect(-200, 330, 420, 260, 130, rgba(0x2ad4a5, 0x0e), 0, 170);
}

/* The 4 x 4 pad grid used as the logo. */
static void logo(float x, float y, float a) {
    static const uint8_t lit[16] = {1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    for (int i = 0; i < 16; i++) {
        float px = x + (float)(i % 4) * 8, py = y + (float)(i / 4) * 8;
        gfx_rect(px, py, 6, 6, 1.6f, lit[i] ? rgba(0x2ad4a5, (unsigned)(255 * a)) : rgba(0xffffff, (unsigned)(46 * a)), 0, 0);
    }
}

static void spinner(float cx, float cy, float r, double t, uint32_t rgb, float a) {
    for (int i = 0; i < 10; i++) {
        float ang = (float)(i * 0.628318);
        float phase = fmodf((float)(t * 1.4) - i / 10.0f + 10.0f, 1.0f);
        gfx_rect(cx + r * cosf(ang) - 2, cy + r * sinf(ang) - 2, 4, 4, 2, rgba(rgb, (unsigned)(a * (40 + 215 * phase))), 0, 0);
    }
}

/* Where an entry runs from. One colour per label, never the app's own colour, so the same text always looks the
 * same: green = installed on the unit (persistent), blue = live from the USB stick (the stick or this image). */
#define CHIP_INSTALLED 0x45d483u
#define CHIP_LIVE 0x7cc4ffu
#define CHIP_NEUTRAL 0xc8cfdbu
static const char *source_chip(const App *a, uint32_t *rgb) {
    *rgb = a->source == SRC_OFFICIAL ? CHIP_INSTALLED : CHIP_LIVE;
    return a->source == SRC_OFFICIAL ? "INSTALLED \xC2\xB7 PERSISTENT" : "LIVE USB";
}

static float chip(float right, float cy, const char *label, uint32_t rgb, float a, int filled) {
    float tw = gfx_text_width(FONT_CHIP, label), w = tw + 16, x = right - w;
    if (filled) gfx_rect(x, cy - 10, w, 20, 10, rgba(rgb, (unsigned)(0x2a * a)), 0, 0);
    else gfx_rect(x, cy - 10, w, 20, 10, rgba(rgb, (unsigned)(0x55 * a)), 1, 0);
    gfx_text(FONT_CHIP, x + 8, cy + gfx_font_cap(FONT_CHIP) * 0.5f, rgba(rgb, (unsigned)(0xee * a)), label);
    return w;
}

/* App icons from the app-info note, one texture per executable (path, size and date), loaded on first draw. */
static struct {
    char exe[320];
    long long size;
    long mtime;
    int tex; /* -1 = could not be read: draw the letter tile */
} icons[BM_MAX_APPS * 2];
static int icon_count;

static int icon_texture(const App *a) {
    if (a->icon_w <= 0 || !a->ready) return -1;
    for (int i = 0; i < icon_count; i++)
        if (icons[i].size == a->size && icons[i].mtime == a->mtime && !strcmp(icons[i].exe, a->exe)) return icons[i].tex;
    if (icon_count == (int)(sizeof icons / sizeof icons[0])) return -1;
    int tex = -1;
    size_t n = (size_t)a->icon_w * (size_t)a->icon_h * 4;
    uint8_t *px = malloc(n);
    if (px && apps_load_icon(a, px) == 0) tex = gfx_image_create(px, a->icon_w, a->icon_h);
    free(px);
    bm_log("icon: %s %dx%d %s", a->exe, a->icon_w, a->icon_h, tex > 0 ? "loaded" : "could not be read");
    snprintf(icons[icon_count].exe, sizeof icons[icon_count].exe, "%s", a->exe);
    icons[icon_count].size = a->size, icons[icon_count].mtime = a->mtime, icons[icon_count].tex = tex;
    icon_count++;
    return tex;
}

static void icon_tile(float x, float y, float s, const App *a, float alpha, double t) {
    int tex = icon_texture(a);
    if (tex > 0) {
        gfx_rect(x + s * 0.06f, y + 3 + s * 0.06f, s * 0.88f, s * 0.88f, s * 0.22f, rgba(0x000000, (unsigned)(0x50 * alpha)), 0, 8);
        gfx_image(tex, x, y, s, s, alpha);
        return;
    }
    const FontId face = s > 64 ? FONT_HERO : FONT_ICON;
    uint32_t top = mix(a->accent, 0xffffff, 0.18f), bottom = mix(a->accent, 0x000000, 0.22f);
    if (!a->ready) top = mix(top, 0x3a4252, 0.7f), bottom = mix(bottom, 0x2a3040, 0.7f);
    gfx_rect(x, y + 3, s, s, s * 0.26f, rgba(0x000000, (unsigned)(0x50 * alpha)), 0, 8); /* drop shadow */
    gfx_rect_vgrad(x, y, s, s, s * 0.26f, rgba(top, (unsigned)(255 * alpha)), rgba(bottom, (unsigned)(255 * alpha)));
    gfx_rect(x, y, s, s, s * 0.26f, rgba(0xffffff, (unsigned)(0x30 * alpha)), 1, 0); /* rim */
    if (!a->ready) {
        spinner(x + s / 2, y + s / 2, s * 0.26f, t, 0xffffff, alpha);
        return;
    }
    char letter[2] = {a->name[0], 0};
    if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] = (char)(letter[0] - 32);
    float lw = gfx_text_width(face, letter);
    gfx_text(face, x + (s - lw) / 2, y + (s + gfx_font_cap(face)) / 2, rgba(0x0b0f17, (unsigned)(0xe8 * alpha)), letter);
}

static void footer_key(float *x, float y, const char *key, const char *label, float a) {
    float kw = gfx_text_width(FONT_CHIP, key) + 14;
    gfx_rect(*x, y - 15, kw, 21, 6, rgba(0xffffff, (unsigned)(0x14 * a)), 0, 0);
    gfx_rect(*x, y - 15, kw, 21, 6, rgba(0xffffff, (unsigned)(0x22 * a)), 1, 0);
    gfx_text(FONT_CHIP, *x + 7, y - 4.5f + gfx_font_cap(FONT_CHIP) * 0.5f, rgba(0xd8dde6, (unsigned)(255 * a)), key);
    *x += kw + 8;
    *x += gfx_text(FONT_HINT, *x, y, rgba(0x8b95a8, (unsigned)(255 * a)), label) + 22;
}

void menu_draw(Menu *m) {
    const double t = m->t, age = t - m->start_t;
    const float intro = ease_out((float)(age / 0.35));
    const AppList *l = m->apps;
    gfx_begin(BG_TOP);
    background();

    /* header */
    logo(MARGIN + 2, 22, intro);
    gfx_text(FONT_TITLE, 70, 46, rgba(0xeef1f6, (unsigned)(255 * intro)), "Boot menu");
    char count[64];
    int apps = 0;
    for (int i = 0; i < l->n; i++) apps += l->app[i].source != SRC_OFFICIAL;
    snprintf(count, sizeof count, "%d app%s", apps, apps == 1 ? "" : "s");
    float cw = gfx_text_width(FONT_HINT, count);
    gfx_text(FONT_HINT, W - MARGIN - cw, 43, rgba(0x8b95a8, (unsigned)(255 * intro)), count);
    gfx_rect(W - MARGIN - cw - 18, 34, 7, 7, 3.5f, rgba(0x2ad4a5, (unsigned)(255 * intro)), 0, 0);
    gfx_rect(W - MARGIN - cw - 18, 34, 7, 7, 3.5f, rgba(0x2ad4a5, (unsigned)(110 * intro)), 0, 8);
    gfx_rect(MARGIN, HEADER_H, W - 2 * MARGIN, 1, 0, rgba(0xffffff, 0x12), 0, 0);

    /* list */
    gfx_clip(0, LIST_TOP - 6, W, LIST_H + 12);
    if (l->n == 0) {
        gfx_text(FONT_NAME, MARGIN + 8, LIST_TOP + 40, TEXT, "No apps found");
        gfx_text(FONT_BODY, MARGIN + 8, LIST_TOP + 68, TEXT_DIM,
                 "Copy an ARM (armel) executable next to autoexec.bin or into apps/, then push HOME.");
    }
    const float x = MARGIN, w = W - 2 * MARGIN;
    const App *sel = l->n ? &l->app[m->sel] : NULL;
    if (sel) {
        /* the highlight slides between rows */
        float hy = LIST_TOP + (m->sel_pos - m->scroll) * STRIDE;
        float a = intro;
        gfx_rect(x, hy, w, ROW_H, 16, rgba(sel->accent, (unsigned)(0x26 * a)), 0, 22);
        gfx_rect_vgrad(x, hy, w, ROW_H, 16, rgba(0x1f2a40, (unsigned)(255 * a)), rgba(0x182133, (unsigned)(255 * a)));
        if (m->countdown_end > 0 && m->timeout_s > 0) {
            float p = clampf((float)(1 - (m->countdown_end - t) / m->timeout_s), 0, 1);
            gfx_rect(x, hy, w * p, ROW_H, 16, rgba(sel->accent, 0x1c), 0, 0);
        }
        gfx_rect(x, hy, w, ROW_H, 16, rgba(sel->accent, (unsigned)(0xb8 * a)), 1.5f, 0);
    }
    int first = (int)floorf(m->scroll), last = first + VISIBLE + 1;
    for (int i = first < 0 ? 0 : first; i < l->n && i <= last; i++) {
        const App *a = &l->app[i];
        float stagger = ease_out((float)((age - 0.04 * i) / 0.32));
        float y = LIST_TOP + ((float)i - m->scroll) * STRIDE + (1 - stagger) * 18;
        float al = stagger;
        int is_sel = i == m->sel;
        if (!is_sel) {
            gfx_rect(x, y, w, ROW_H, 16, rgba(0xffffff, (unsigned)((i == m->pressed ? 0x16 : 0x09) * al)), 0, 0);
            gfx_rect(x, y, w, ROW_H, 16, rgba(0xffffff, (unsigned)(0x0e * al)), 1, 0);
        }
        icon_tile(x + 10, y + 9, 52, a, al, t);
        float tx = x + 78, right = x + w - 16;
        /* chips on the right: where it comes from, and whether it is the default */
        float cx = right;
        uint32_t chip_rgb;
        const char *where = source_chip(a, &chip_rgb);
        cx -= chip(cx, y + ROW_H / 2.0f, where, chip_rgb, al, 1) + 6;
        if (a->encrypted) cx -= chip(cx, y + ROW_H / 2.0f, "ENCRYPTED", CHIP_NEUTRAL, al, 0) + 6;
        if (a->source == m->def_source && !strcmp(a->id, m->def_id))
            cx -= chip(cx, y + ROW_H / 2.0f, m->def_label, CHIP_NEUTRAL, al, 0) + 6;
        gfx_text_fit(FONT_NAME, tx, y + 32, rgba(is_sel ? 0xffffff : 0xdfe4ec, (unsigned)(255 * al)), a->name, cx - tx - 12);
        gfx_text_fit(FONT_BODY, tx, y + 54, rgba(is_sel ? 0x9aa6bc : 0x7c869a, (unsigned)(255 * al)), a->detail, cx - tx - 12);
    }
    gfx_clip(0, 0, 0, 0);
    /* scroll position */
    if (l->n > VISIBLE) {
        float track = LIST_H, thumb = track * VISIBLE / l->n;
        float ty = LIST_TOP + (track - thumb) * (m->scroll / (float)(l->n - VISIBLE));
        gfx_rect(W - 12, LIST_TOP, 3, track, 1.5f, rgba(0xffffff, 0x10), 0, 0);
        gfx_rect(W - 12, ty, 3, thumb, 1.5f, rgba(0xffffff, 0x50), 0, 0);
    }

    /* footer */
    gfx_rect(MARGIN, FOOTER_Y, W - 2 * MARGIN, 1, 0, rgba(0xffffff, 0x12), 0, 0);
    float fx = MARGIN, fy = 456;
    if (m->message[0] && t < m->message_until) {
        /* a failed launch: the whole footer, until the first input */
        gfx_rect(MARGIN, 432, W - 2 * MARGIN, 34, 10, rgba(0xff5a3c, 0x1e), 0, 0);
        gfx_rect(MARGIN, 432, W - 2 * MARGIN, 34, 10, rgba(0xff5a3c, 0x55), 1, 0);
        gfx_rect(MARGIN + 14, 445, 8, 8, 4, rgba(0xff7a5c, 0xff), 0, 0);
        gfx_text_fit(FONT_HINT, MARGIN + 32, 454, 0xffc4b4ffu, m->message, W - 2 * MARGIN - 48);
        return;
    }
    footer_key(&fx, fy, "TURN", "Choose", intro);
    footer_key(&fx, fy, "PUSH", "Start", intro);
    footer_key(&fx, fy, "BACK", "Official", intro);
    footer_key(&fx, fy, "HOME", "Rescan", intro);
    char right[160] = "";
    uint32_t rcol = TEXT_DIM;
    if (m->countdown_end > 0 && sel) {
        int s = (int)ceil(m->countdown_end - t);
        snprintf(right, sizeof right, "Starting %s in %d s", sel->name, s < 1 ? 1 : s);
        rcol = rgba(0xeef1f6, 0xff);
    } else {
        snprintf(right, sizeof right, "v" BM_VERSION);
        rcol = TEXT_FAINT;
    }
    float maxw = W - MARGIN - fx;
    float rw = gfx_text_width(FONT_HINT, right);
    if (rw > maxw) rw = maxw;
    gfx_text_fit(FONT_HINT, W - MARGIN - rw, fy, rcol, right, maxw);
    if (m->countdown_end > 0 && sel && m->timeout_s > 0) {
        float p = clampf((float)((m->countdown_end - t) / m->timeout_s), 0, 1);
        gfx_rect(W - MARGIN - rw, fy + 8, rw, 2, 1, rgba(0xffffff, 0x18), 0, 0);
        gfx_rect(W - MARGIN - rw, fy + 8, rw * p, 2, 1, rgba(sel->accent, 0xff), 0, 0);
    }
}

void menu_draw_launch(Menu *m, const char *what) {
    const App *a = m->apps->n ? &m->apps->app[m->sel] : NULL;
    gfx_begin(BG_TOP);
    background();
    if (a) {
        const float s = 96, x = (W - s) / 2, y = 128;
        gfx_rect(x, y, s, s, 28, rgba(a->accent, 0x40), 0, 40);
        icon_tile(x, y, s, a, 1, m->t);
        float nw = gfx_text_width(FONT_SPLASH, a->name);
        gfx_text(FONT_SPLASH, (W - nw) / 2, y + s + 58, TEXT, a->name);
    }
    float ww = gfx_text_width(FONT_BODY, what);
    gfx_text(FONT_BODY, (W - ww) / 2, 340, TEXT_DIM, what);
    /* a thin indeterminate bar */
    const float bw = 180, bx = (W - bw) / 2, by = 364;
    gfx_rect(bx, by, bw, 3, 1.5f, rgba(0xffffff, 0x14), 0, 0);
    float p = (float)fmod(m->t * 0.9, 1.0);
    float seg = 56, sx = bx + (bw + seg) * p - seg, ex = sx + seg;
    if (sx < bx) sx = bx;
    if (ex > bx + bw) ex = bx + bw;
    if (ex > sx) gfx_rect(sx, by, ex - sx, 3, 1.5f, rgba(a ? a->accent : 0x2ad4a5, 0xff), 0, 0);
}
