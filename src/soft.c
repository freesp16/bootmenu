/* The menu drawn by the CPU (--render cpu): for a unit whose GPU path fails.
 *
 * The same shapes as the GL renderer, into an 800 x 480 buffer of 0x00RRGGBB words: the rounded box of the fragment
 * shader (its signed distance per pixel: anti-aliased edges, rings, soft shadows), glyphs from the same atlas at their
 * pixel positions, and the icons scaled down by averaging. Each frame is put on the X server in its own format
 * (16 or 24 bits) with XPutImage (x11.c), in an override-redirect window over the screen.
 *
 * Cost: a full frame touches every pixel a few times. The frame's first rectangles (the background and its colour
 * pools, the largest shapes) are the same from frame to frame, so the buffer after them is kept and restored while
 * they repeat. Frames are paced to 30 a second at most. */
#include "bm.h"
#include "soft.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SW 800
#define SH 480
#define PREFIX_MAX 8
#define MAX_IMAGES 64

typedef struct {
    float x, y, w, h, radius, border, soft;
    uint32_t top, bottom;
} RectCall;

static struct {
    uint32_t px[SW * SH];
    int clip[4]; /* x0, y0, x1, y1 */
    /* the frame's leading rectangles and the buffer after them */
    uint32_t base[SW * SH];
    RectCall prefix[PREFIX_MAX], rec[PREFIX_MAX];
    int prefix_n, base_ok;
    int matched, recording, rec_n, skipping;
    uint32_t clear;
    struct { uint8_t *rgba; int w, h; } img[MAX_IMAGES];
    int n_img;
    double last_present, begin_t;
    long frames;
    char info[160];
} S;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float smooth(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}

static void blend(uint32_t *d, unsigned r, unsigned g, unsigned b, unsigned a256) {
    if (a256 == 0) return;
    uint32_t v = *d;
    unsigned dr = v >> 16 & 255, dg = v >> 8 & 255, db = v & 255;
    if (a256 >= 256) dr = r, dg = g, db = b;
    else {
        dr = dr + (((int)r - (int)dr) * (int)a256 >> 8);
        dg = dg + (((int)g - (int)dg) * (int)a256 >> 8);
        db = db + (((int)b - (int)db) * (int)a256 >> 8);
    }
    *d = dr << 16 | dg << 8 | db;
}

int soft_open(char *err, size_t errlen) {
    if (x11_open(SW, SH, 1, err, errlen) != 0) return -1;
    char how[96] = "";
    if (x11_present_prepare(SW, SH, how, sizeof how) != 0) {
        snprintf(err, errlen, "cpu: %s", how);
        return -1;
    }
    snprintf(S.info, sizeof S.info, "backend=cpu surface=%dx%d %s", SW, SH, how);
    return 0;
}

const char *soft_info(void) { return S.info; }

/* ---- the cache of the frame's leading rectangles ---------------------------------------------------------------- */
static void draw_rect(const RectCall *c);

static int same(const RectCall *a, const RectCall *b) { return !memcmp(a, b, sizeof *a); }

/* Any call that is not one of the leading rectangles ends the prefix: drawn as usual from here on. */
static void prefix_end(void) {
    if (S.skipping) {
        /* the frame left the cached sequence early: draw what was skipped, and record it again next frame */
        for (int i = 0; i < SW * SH; i++) S.px[i] = S.clear;
        for (int i = 0; i < S.matched; i++) draw_rect(&S.prefix[i]);
        S.skipping = 0;
        S.base_ok = 0;
    }
    if (S.recording && S.rec_n > 0) {
        memcpy(S.prefix, S.rec, sizeof S.rec);
        S.prefix_n = S.rec_n;
        memcpy(S.base, S.px, sizeof S.px);
        S.base_ok = 1;
    }
    S.recording = 0;
}

void soft_begin(uint32_t rgb) {
    S.clip[0] = 0, S.clip[1] = 0, S.clip[2] = SW, S.clip[3] = SH;
    S.matched = 0, S.rec_n = 0, S.clear = rgb & 0xffffff;
    S.begin_t = bm_now();
    S.skipping = S.base_ok && S.prefix_n > 0;
    S.recording = !S.skipping;
    if (!S.skipping)
        for (int i = 0; i < SW * SH; i++) S.px[i] = rgb & 0xffffff;
}

void soft_clip(int x, int y, int w, int h) {
    prefix_end();
    if (w <= 0) {
        S.clip[0] = 0, S.clip[1] = 0, S.clip[2] = SW, S.clip[3] = SH;
        return;
    }
    S.clip[0] = x < 0 ? 0 : x, S.clip[1] = y < 0 ? 0 : y;
    S.clip[2] = x + w > SW ? SW : x + w, S.clip[3] = y + h > SH ? SH : y + h;
}

/* ---- rectangles: the shader's rounded box, per pixel ------------------------------------------------------------ */
static void draw_rect(const RectCall *c) {
    float hw = c->w * 0.5f, hh = c->h * 0.5f, r = c->radius;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float e = (c->soft > 1 ? c->soft : 1) + 1, s = c->soft > 1 ? c->soft : 1;
    float cx = c->x + hw, cy = c->y + hh, y0 = c->y - e, y1 = c->y + c->h + e;
    int ix0 = (int)floorf(c->x - e), iy0 = (int)floorf(y0), ix1 = (int)ceilf(c->x + c->w + e), iy1 = (int)ceilf(y1);
    if (ix0 < S.clip[0]) ix0 = S.clip[0];
    if (iy0 < S.clip[1]) iy0 = S.clip[1];
    if (ix1 > S.clip[2]) ix1 = S.clip[2];
    if (iy1 > S.clip[3]) iy1 = S.clip[3];
    int grad = c->top != c->bottom;
    for (int py = iy0; py < iy1; py++) {
        float uy = (float)py + 0.5f - cy, qy = fabsf(uy) - hh + r;
        uint32_t col = c->top;
        if (grad) {
            float t = clampf(((float)py + 0.5f - y0) / (y1 - y0), 0, 1);
            col = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                float a = (float)(c->top >> sh & 255), b = (float)(c->bottom >> sh & 255);
                col |= (uint32_t)(a + (b - a) * t + 0.5f) << sh;
            }
        }
        unsigned R = col >> 24, G = col >> 16 & 255, B = col >> 8 & 255, A = col & 255;
        uint32_t *row = S.px + py * SW;
        for (int px = ix0; px < ix1; px++) {
            float qx = fabsf((float)px + 0.5f - cx) - hw + r;
            float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
            float d = (mx > 0 && my > 0 ? sqrtf(mx * mx + my * my) : mx + my) + (qx > qy ? (qx < 0 ? qx : 0)
                                                                                         : (qy < 0 ? qy : 0)) - r;
            float a = 1 - smooth(-0.5f * s, 0.5f * s, d);
            if (c->border > 0) a *= smooth(-0.5f, 0.5f, d + c->border);
            if (a <= 0) continue;
            blend(&row[px], R, G, B, (unsigned)(a * (float)A + 0.5f));
        }
    }
}

void soft_rect(float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom, float border,
               float soft) {
    if (w <= 0 || h <= 0) return;
    RectCall c = {x, y, w, h, radius, border, soft, top, bottom};
    if (S.skipping) {
        if (S.matched < S.prefix_n && same(&c, &S.prefix[S.matched])) {
            if (++S.matched == S.prefix_n) {
                memcpy(S.px, S.base, sizeof S.px); /* the whole cached prefix: its buffer */
                S.skipping = 0;
            }
            return;
        }
        prefix_end(); /* draws the skipped ones */
        S.recording = 0;
    }
    if (S.recording) {
        if (S.rec_n < PREFIX_MAX) S.rec[S.rec_n++] = c;
        else prefix_end();
    }
    draw_rect(&c);
}

/* ---- glyphs and images ------------------------------------------------------------------------------------------ */
void soft_glyph(const uint8_t *atlas, int atlas_w, int sx, int sy, int w, int h, int dx, int dy, uint32_t rgba) {
    prefix_end();
    unsigned R = rgba >> 24, G = rgba >> 16 & 255, B = rgba >> 8 & 255, A = rgba & 255;
    for (int y = 0; y < h; y++) {
        int py = dy + y;
        if (py < S.clip[1] || py >= S.clip[3]) continue;
        const uint8_t *src = atlas + (sy + y) * atlas_w + sx;
        uint32_t *row = S.px + py * SW;
        for (int x = 0; x < w; x++) {
            int px = dx + x;
            if (px < S.clip[0] || px >= S.clip[2] || !src[x]) continue;
            blend(&row[px], R, G, B, (src[x] * A + 127) / 255 + (src[x] * A > 0));
        }
    }
}

int soft_image_create(const uint8_t *rgba, int w, int h) {
    if (S.n_img >= MAX_IMAGES) return -1;
    uint8_t *copy = malloc((size_t)w * (size_t)h * 4);
    if (!copy) return -1;
    memcpy(copy, rgba, (size_t)w * (size_t)h * 4);
    S.img[S.n_img].rgba = copy, S.img[S.n_img].w = w, S.img[S.n_img].h = h;
    return ++S.n_img; /* handles start at 1 */
}

/* Scaled by averaging the source pixels each destination pixel covers (icons are drawn smaller than they are). */
void soft_image(int handle, float x, float y, float w, float h, float alpha) {
    prefix_end();
    if (handle <= 0 || handle > S.n_img || w < 1 || h < 1) return;
    const uint8_t *src = S.img[handle - 1].rgba;
    int iw = S.img[handle - 1].w, ih = S.img[handle - 1].h;
    int x0 = (int)floorf(x + 0.5f), y0 = (int)floorf(y + 0.5f), dw = (int)(w + 0.5f), dh = (int)(h + 0.5f);
    for (int dy = 0; dy < dh; dy++) {
        int py = y0 + dy;
        if (py < S.clip[1] || py >= S.clip[3]) continue;
        int sy0 = dy * ih / dh, sy1 = (dy + 1) * ih / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < dw; dx++) {
            int px = x0 + dx;
            if (px < S.clip[0] || px >= S.clip[2]) continue;
            int sx0 = dx * iw / dw, sx1 = (dx + 1) * iw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    const uint8_t *p = src + ((size_t)sy * (size_t)iw + (size_t)sx) * 4;
                    r += p[0] * p[3], g += p[1] * p[3], b += p[2] * p[3], a += p[3], n++;
                }
            if (!a) continue;
            unsigned A = (unsigned)((float)a / (float)n * alpha + 0.5f);
            blend(&S.px[py * SW + px], r / a, g / a, b / a, A + (A > 0));
        }
    }
}

/* ---- the frame --------------------------------------------------------------------------------------------------- */
int soft_end(const char *shot) {
    prefix_end();
    if (shot && *shot) {
        FILE *f = fopen(shot, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", SW, SH);
            for (int i = 0; i < SW * SH; i++) {
                uint8_t c[3] = {(uint8_t)(S.px[i] >> 16), (uint8_t)(S.px[i] >> 8), (uint8_t)S.px[i]};
                fwrite(c, 1, 3, f);
            }
            fclose(f);
            bm_log("screenshot %s", shot);
        }
    }
    /* the cost of a frame, in the log for the first ones and every 300th */
    if (++S.frames <= 3 || S.frames % 300 == 0)
        bm_log("cpu: frame %ld drawn in %.1f ms%s", S.frames, (bm_now() - S.begin_t) * 1e3,
               S.base_ok ? " (cached background)" : "");
    /* at most 30 frames a second: there is no vsync to wait for */
    double now = bm_now(), wait = S.last_present + 1.0 / 30 - now;
    if (wait > 0) {
        struct timespec ts = {0, (long)(wait * 1e9)};
        nanosleep(&ts, NULL);
    }
    S.last_present = bm_now();
    return x11_present(S.px, SW, SH);
}
