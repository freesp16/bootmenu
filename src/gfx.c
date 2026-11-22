/* OpenGL ES 2 renderer for the boot menu, loaded at run time.
 *
 * The firmware ships the Vivante driver in window-system flavours (-fb, -x11, ...) whose libraries share sonames;
 * loading one flavour's libGAL, libEGL and libGLESv2 by file name, in that order and RTLD_GLOBAL, binds the whole
 * stack to that flavour. Never two flavours in one process: the launcher re-runs the menu with --render x11 when fb
 * fails.
 *
 * Drawing: every shape is a quad in one vertex array, drawn with one program and one texture (the glyph atlas), so
 * a frame is one glDrawArrays per clip region (two or three a frame). The fragment shader evaluates a rounded-box
 * signed distance per pixel: anti-aliased edges, rings and soft shadows at no CPU cost; glyph quads sample the
 * atlas instead (radius < 0 marks them). The A9 only fills a few hundred 40-byte vertices a frame. */
#include "bm.h"
#include "gen/font_atlas.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean, EGLenum, GLuint, GLenum, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean, GLubyte;
typedef char GLchar;

enum {
    EGL_ALPHA_SIZE = 0x3021, EGL_BLUE_SIZE = 0x3022, EGL_GREEN_SIZE = 0x3023, EGL_RED_SIZE = 0x3024,
    EGL_SURFACE_TYPE = 0x3033, EGL_NONE = 0x3038, EGL_RENDERABLE_TYPE = 0x3040, EGL_VENDOR = 0x3053,
    EGL_HEIGHT = 0x3056, EGL_WIDTH = 0x3057, EGL_CONTEXT_CLIENT_VERSION = 0x3098, EGL_OPENGL_ES_API = 0x30A0,
    EGL_WINDOW_BIT = 0x0004, EGL_OPENGL_ES2_BIT = 0x0004,
};
enum {
    GL_TRIANGLES = 0x0004, GL_COLOR_BUFFER_BIT = 0x4000, GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406,
    GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31,
    GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_TEXTURE_2D = 0x0DE1, GL_ALPHA = 0x1906,
    GL_RGBA = 0x1908, GL_LINEAR = 0x2601, GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F, GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_PACK_ALIGNMENT = 0x0D05, GL_BLEND = 0x0BE2, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_SCISSOR_TEST = 0x0C11, GL_DITHER = 0x0BD0, GL_LINEAR_MIPMAP_LINEAR = 0x2703,
};

#define EGL_FUNCS(F)                                                                                     \
    F(EGLDisplay, eglGetDisplay, (void *))                                                               \
    F(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))                                       \
    F(EGLBoolean, eglBindAPI, (EGLenum))                                                                 \
    F(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))          \
    F(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, void *, const EGLint *))               \
    F(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))                 \
    F(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))                      \
    F(EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint))                                                 \
    F(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface))                                              \
    F(EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint *))                           \
    F(const char *, eglQueryString, (EGLDisplay, EGLint))                                                \
    F(EGLint, eglGetError, (void))
#define FB_FUNCS(F)                                                                                      \
    F(void *, fbGetDisplayByIndex, (int))                                                                \
    F(void, fbGetDisplayGeometry, (void *, int *, int *))                                                \
    F(void *, fbCreateWindow, (void *, int, int, int, int))
#define GL_FUNCS(F)                                                                                      \
    F(GLuint, glCreateShader, (GLenum))                                                                  \
    F(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))                     \
    F(void, glCompileShader, (GLuint))                                                                   \
    F(void, glGetShaderiv, (GLuint, GLenum, GLint *))                                                    \
    F(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                  \
    F(GLuint, glCreateProgram, (void))                                                                   \
    F(void, glAttachShader, (GLuint, GLuint))                                                            \
    F(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))                                      \
    F(void, glLinkProgram, (GLuint))                                                                     \
    F(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                                   \
    F(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                 \
    F(void, glUseProgram, (GLuint))                                                                      \
    F(GLint, glGetUniformLocation, (GLuint, const GLchar *))                                             \
    F(void, glUniform2f, (GLint, GLfloat, GLfloat))                                                      \
    F(void, glUniform1i, (GLint, GLint))                                                                 \
    F(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                                \
    F(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                                 \
    F(void, glEnable, (GLenum))                                                                          \
    F(void, glDisable, (GLenum))                                                                         \
    F(void, glBlendFunc, (GLenum, GLenum))                                                               \
    F(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                          \
    F(void, glClear, (GLbitfield))                                                                       \
    F(void, glEnableVertexAttribArray, (GLuint))                                                         \
    F(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))            \
    F(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                      \
    F(const GLubyte *, glGetString, (GLenum))                                                            \
    F(void, glGenTextures, (GLsizei, GLuint *))                                                          \
    F(void, glBindTexture, (GLenum, GLuint))                                                             \
    F(void, glTexParameteri, (GLenum, GLenum, GLint))                                                    \
    F(void, glPixelStorei, (GLenum, GLint))                                                              \
    F(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    F(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))                     \
    F(void, glGenerateMipmap, (GLenum))

#define F(ret, name, args) static ret(*name) args;
EGL_FUNCS(F)
FB_FUNCS(F)
GL_FUNCS(F)
#undef F

typedef struct {
    float x, y;       /* screen pixels */
    float u, v;       /* rect: position relative to the centre; glyph: atlas texel coordinates (0..1) */
    uint8_t c[4];
    float hw, hh, radius, border; /* radius -1: glyph (atlas alpha), -2: image (RGBA texture) */
    float soft;
} Vertex;

#define MAX_VERTS (6 * 2400)

static struct {
    char backend[8];
    int w, h;
    EGLDisplay dpy;
    EGLSurface surface;
    EGLContext context;
    GLuint prog, tex, bound; /* tex = the glyph atlas; bound = the texture the pending batch samples */
    char info[512];
    char shot[256];
    int clip[4];
    int n;
    Vertex v[MAX_VERTS];
} G;

static const char *VS =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_col;\n"
    "attribute vec4 a_p;\n"
    "attribute float a_soft;\n"
    "uniform vec2 u_scale;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "varying vec4 v_p;\n"
    "varying float v_soft;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "  v_uv = a_uv; v_col = a_col; v_p = a_p; v_soft = a_soft;\n"
    "}\n";
static const char *FS =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "varying vec4 v_p;\n"
    "varying float v_soft;\n"
    "void main() {\n"
    "  if (v_p.z < -1.5) {\n"
    "    vec4 c = texture2D(u_tex, v_uv);\n"
    "    gl_FragColor = vec4(c.rgb, c.a * v_col.a);\n"
    "    return;\n"
    "  }\n"
    "  if (v_p.z < 0.0) {\n"
    "    gl_FragColor = vec4(v_col.rgb, v_col.a * texture2D(u_tex, v_uv).a);\n"
    "    return;\n"
    "  }\n"
    "  vec2 q = abs(v_uv) - v_p.xy + vec2(v_p.z);\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_p.z;\n"
    "  float s = max(v_soft, 1.0);\n"
    "  float a = 1.0 - smoothstep(-0.5 * s, 0.5 * s, d);\n"
    "  if (v_p.w > 0.0) a *= smoothstep(-0.5, 0.5, d + v_p.w);\n"
    "  gl_FragColor = vec4(v_col.rgb, v_col.a * a);\n"
    "}\n";

static int fail(char *err, size_t errlen, const char *what, const char *detail) {
    snprintf(err, errlen, "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
    return -1;
}

static int egl_fail(char *err, size_t errlen, const char *what) {
    char d[32];
    snprintf(d, sizeof d, "EGL error 0x%x", eglGetError ? (unsigned)eglGetError() : 0u);
    return fail(err, errlen, what, d);
}

int gfx_load(const char *backend, char *err, size_t errlen) {
    snprintf(G.backend, sizeof G.backend, "%s", backend);
    void *e, *l;
    if (strcmp(backend, "host") == 0) {
        setenv("EGL_PLATFORM", "x11", 0); /* Mesa: the native display is an X11 Display* (a user value wins) */
        e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!e) return fail(err, errlen, "dlopen libEGL.so.1", dlerror());
        l = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
        if (!l) return fail(err, errlen, "dlopen libGLESv2.so.2", dlerror());
    } else if (strcmp(backend, "fb") == 0 || strcmp(backend, "x11") == 0) {
        char gal[48], egl[48], gles[48];
        snprintf(gal, sizeof gal, "libGAL-%s.so", backend);
        snprintf(egl, sizeof egl, "libEGL-%s.so", backend);
        snprintf(gles, sizeof gles, "libGLESv2-%s.so.2.0.0", backend);
        if (!dlopen(gal, RTLD_NOW | RTLD_GLOBAL)) return fail(err, errlen, "dlopen libGAL", dlerror());
        e = dlopen(egl, RTLD_NOW | RTLD_GLOBAL);
        if (!e) return fail(err, errlen, "dlopen libEGL", dlerror());
        l = dlopen(gles, RTLD_NOW | RTLD_GLOBAL);
        if (!l) return fail(err, errlen, "dlopen libGLESv2", dlerror());
    } else {
        return fail(err, errlen, "unknown render backend", backend);
    }
    /* The driver stays loaded until the process exits: unloading Vivante's libraries is not worth the risk. */
#define F(ret, name, args) \
    if (!(*(void **)(&name) = dlsym(e, #name))) return fail(err, errlen, "missing EGL symbol", #name);
    EGL_FUNCS(F)
    if (strcmp(backend, "fb") == 0) { FB_FUNCS(F) }
#undef F
#define F(ret, name, args) \
    if (!(*(void **)(&name) = dlsym(l, #name))) return fail(err, errlen, "missing GLES symbol", #name);
    GL_FUNCS(F)
#undef F
    return 0;
}

/* Two framebuffers (vsynced page flips, no tearing) only when video memory already has room, so the driver never
 * has to resize memory the X server mapped. A value set in the environment wins. */
static void fb_prepare(char *line, size_t linelen) {
    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int two = 0;
    if (fd >= 0 && ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0) {
        two = v.yres_virtual >= 2 * v.yres && f.smem_len >= 2u * f.line_length * v.yres;
        snprintf(line, linelen, "fb0 %ux%u %ubpp virtual %ux%u", v.xres, v.yres, v.bits_per_pixel, v.xres_virtual,
                 v.yres_virtual);
    } else {
        snprintf(line, linelen, "fb0 not readable");
    }
    if (fd >= 0) close(fd);
    setenv("FB_MULTI_BUFFER", two ? "2" : "1", 0);
}

static GLuint shader(GLenum type, const char *src, char *err, size_t errlen) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fail(err, errlen, "shader compile", log);
        return 0;
    }
    return s;
}

int gfx_open(char *err, size_t errlen) {
    void *native_display = NULL, *native_window = NULL;
    char fbline[128] = "";
    if (strcmp(G.backend, "fb") == 0) {
        fb_prepare(fbline, sizeof fbline);
        native_display = fbGetDisplayByIndex(0);
        if (!native_display) return fail(err, errlen, "fbGetDisplayByIndex failed", NULL);
        int w = 0, h = 0;
        fbGetDisplayGeometry(native_display, &w, &h);
        native_window = fbCreateWindow(native_display, 0, 0, w, h);
        if (!native_window) return fail(err, errlen, "fbCreateWindow failed", NULL);
    } else {
        if (x11_open(800, 480, strcmp(G.backend, "x11") == 0, err, errlen) != 0) return -1;
        native_display = x11_display();
        native_window = (void *)(uintptr_t)x11_window();
    }

    EGLint major = 0, minor = 0, count = 0;
    G.dpy = eglGetDisplay(native_display);
    if (!G.dpy) return egl_fail(err, errlen, "eglGetDisplay");
    if (!eglInitialize(G.dpy, &major, &minor)) return egl_fail(err, errlen, "eglInitialize");
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                          EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_NONE};
    EGLConfig config = NULL;
    if (!eglChooseConfig(G.dpy, config_attrs, &config, 1, &count) || count < 1)
        return egl_fail(err, errlen, "eglChooseConfig");
    G.surface = eglCreateWindowSurface(G.dpy, config, native_window, NULL);
    if (!G.surface) return egl_fail(err, errlen, "eglCreateWindowSurface");
    static const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    G.context = eglCreateContext(G.dpy, config, NULL, context_attrs);
    if (!G.context) return egl_fail(err, errlen, "eglCreateContext");
    if (!eglMakeCurrent(G.dpy, G.surface, G.surface, G.context)) return egl_fail(err, errlen, "eglMakeCurrent");
    eglSwapInterval(G.dpy, 1);
    eglQuerySurface(G.dpy, G.surface, EGL_WIDTH, &G.w);
    eglQuerySurface(G.dpy, G.surface, EGL_HEIGHT, &G.h);

    GLuint vs = shader(GL_VERTEX_SHADER, VS, err, errlen);
    GLuint fs = vs ? shader(GL_FRAGMENT_SHADER, FS, err, errlen) : 0;
    if (!fs) return -1;
    G.prog = glCreateProgram();
    glAttachShader(G.prog, vs);
    glAttachShader(G.prog, fs);
    glBindAttribLocation(G.prog, 0, "a_pos");
    glBindAttribLocation(G.prog, 1, "a_uv");
    glBindAttribLocation(G.prog, 2, "a_col");
    glBindAttribLocation(G.prog, 3, "a_p");
    glBindAttribLocation(G.prog, 4, "a_soft");
    glLinkProgram(G.prog);
    GLint linked = 0;
    glGetProgramiv(G.prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = "";
        glGetProgramInfoLog(G.prog, sizeof log, NULL, log);
        return fail(err, errlen, "shader link", log);
    }
    glUseProgram(G.prog);
    glUniform2f(glGetUniformLocation(G.prog, "u_scale"), 2.0f / (float)G.w, -2.0f / (float)G.h);
    glUniform1i(glGetUniformLocation(G.prog, "u_tex"), 0);

    glGenTextures(1, &G.tex);
    glBindTexture(GL_TEXTURE_2D, G.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, FONT_ATLAS_W, FONT_ATLAS_H, 0, GL_ALPHA, GL_UNSIGNED_BYTE, font_atlas);
    G.bound = G.tex;

    glViewport(0, 0, G.w, G.h);
    glEnable(GL_BLEND);
    glEnable(GL_DITHER);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int i = 0; i < 5; i++) glEnableVertexAttribArray((GLuint)i);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(Vertex), &G.v[0].x);
    glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(Vertex), &G.v[0].u);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, sizeof(Vertex), &G.v[0].c[0]);
    glVertexAttribPointer(3, 4, GL_FLOAT, 0, sizeof(Vertex), &G.v[0].hw);
    glVertexAttribPointer(4, 1, GL_FLOAT, 0, sizeof(Vertex), &G.v[0].soft);

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *vendor = eglQueryString(G.dpy, EGL_VENDOR);
    snprintf(G.info, sizeof G.info, "backend=%s surface=%dx%d EGL %d.%d (%s) GL_RENDERER=%s GL_VERSION=%s%s%s%s%s",
             G.backend, G.w, G.h, (int)major, (int)minor, vendor ? vendor : "?", renderer ? renderer : "?",
             version ? version : "?", *fbline ? " " : "", fbline, *fbline ? " FB_MULTI_BUFFER=" : "",
             *fbline ? getenv("FB_MULTI_BUFFER") : "");
    return 0;
}

const char *gfx_info(void) { return G.info; }
int gfx_width(void) { return G.w; }
int gfx_height(void) { return G.h; }

static void flush(void) {
    if (G.n) glDrawArrays(GL_TRIANGLES, 0, G.n);
    G.n = 0;
}

void gfx_begin(uint32_t rgb) {
    G.n = 0;
    G.clip[2] = 0;
    glDisable(GL_SCISSOR_TEST);
    glClearColor((float)(rgb >> 16 & 255) / 255.f, (float)(rgb >> 8 & 255) / 255.f, (float)(rgb & 255) / 255.f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

void gfx_clip(int x, int y, int w, int h) {
    flush();
    if (w <= 0) {
        glDisable(GL_SCISSOR_TEST);
        G.clip[2] = 0;
        return;
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, G.h - y - h, w, h); /* GL's origin is bottom-left */
    G.clip[0] = x, G.clip[1] = y, G.clip[2] = w, G.clip[3] = h;
}

static void emit(Vertex *v, const float xy[4][2], const float uv[4][2], const uint8_t col[4][4], float hw, float hh,
                 float radius, float border, float soft);

/* The batch samples one texture: switching flushes it (an icon costs one extra draw call, text after it another). */
static void use_texture(GLuint t) {
    if (G.bound == t) return;
    flush();
    glBindTexture(GL_TEXTURE_2D, t);
    G.bound = t;
}

int gfx_image_create(const uint8_t *rgba, int w, int h) {
    flush();
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); /* 128 px drawn at 52: mipmaps */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, G.bound);
    return (int)t;
}

static Vertex *quad(void);
void gfx_image(int tex, float x, float y, float w, float h, float alpha) {
    if (tex <= 0) return;
    use_texture((GLuint)tex);
    uint8_t c = (uint8_t)(alpha < 0 ? 0 : alpha > 1 ? 255 : alpha * 255 + 0.5f);
    const uint8_t col[4][4] = {{255, 255, 255, c}, {255, 255, 255, c}, {255, 255, 255, c}, {255, 255, 255, c}};
    const float xy[4][2] = {{x, y}, {x + w, y}, {x, y + h}, {x + w, y + h}};
    const float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    emit(quad(), xy, uv, col, 0, 0, -2, 0, 0);
}

static Vertex *quad(void) {
    if (G.n + 6 > MAX_VERTS) flush();
    Vertex *v = &G.v[G.n];
    G.n += 6;
    return v;
}

static void rgba_bytes(uint32_t rgba, uint8_t c[4]) {
    c[0] = (uint8_t)(rgba >> 24), c[1] = (uint8_t)(rgba >> 16), c[2] = (uint8_t)(rgba >> 8), c[3] = (uint8_t)rgba;
}

/* Corners in order top-left, top-right, bottom-left, bottom-right -> two triangles. */
static void emit(Vertex *v, const float xy[4][2], const float uv[4][2], const uint8_t col[4][4], float hw, float hh,
                 float radius, float border, float soft) {
    static const int order[6] = {0, 1, 2, 2, 1, 3};
    for (int k = 0; k < 6; k++) {
        int i = order[k];
        v[k].x = xy[i][0], v[k].y = xy[i][1];
        v[k].u = uv[i][0], v[k].v = uv[i][1];
        memcpy(v[k].c, col[i], 4);
        v[k].hw = hw, v[k].hh = hh, v[k].radius = radius, v[k].border = border, v[k].soft = soft;
    }
}

static void rect4(float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom, float border,
                  float soft) {
    if (w <= 0 || h <= 0) return;
    float hw = w * 0.5f, hh = h * 0.5f;
    if (radius > hw) radius = hw;
    if (radius > hh) radius = hh;
    float e = (soft > 1 ? soft : 1) + 1; /* room for the anti-aliased or blurred edge */
    float x0 = x - e, y0 = y - e, x1 = x + w + e, y1 = y + h + e;
    const float xy[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
    const float uv[4][2] = {{-hw - e, -hh - e}, {hw + e, -hh - e}, {-hw - e, hh + e}, {hw + e, hh + e}};
    uint8_t col[4][4];
    rgba_bytes(top, col[0]);
    rgba_bytes(top, col[1]);
    rgba_bytes(bottom, col[2]);
    rgba_bytes(bottom, col[3]);
    emit(quad(), xy, uv, (const uint8_t(*)[4])col, hw, hh, radius, border, soft);
}

void gfx_rect(float x, float y, float w, float h, float radius, uint32_t rgba, float border, float soft) {
    if ((rgba & 255) == 0) return;
    rect4(x, y, w, h, radius, rgba, rgba, border, soft);
}

void gfx_rect_vgrad(float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom) {
    rect4(x, y, w, h, radius, top, bottom, 0, 0);
}

/* ---- text ------------------------------------------------------------------------------------------------------ */
static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) c = (c & 0x1F) << 6 | (*p++ & 0x3F);
    else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80)
        c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F), p += 2;
    else if (c >= 0x80) c = '?';
    *s = (const char *)p;
    return c;
}

static const AtlasGlyph *glyph(FontId f, uint32_t cp) {
    const AtlasFace *face = &font_faces[f];
    int lo = face->first, hi = face->first + face->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_glyphs[mid].cp == cp) return &font_glyphs[mid];
        if (font_glyphs[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return cp == '?' ? &font_glyphs[face->first] : glyph(f, '?');
}

float gfx_text_width(FontId f, const char *s) {
    float w = 0;
    while (*s) w += glyph(f, utf8_next(&s))->adv64 / 64.0f;
    return w;
}

int gfx_font_cap(FontId f) { return font_faces[f].cap; }

float gfx_text(FontId f, float x, float y, uint32_t rgba, const char *s) {
    if ((rgba & 255) == 0) return gfx_text_width(f, s);
    use_texture(G.tex);
    uint8_t c[4];
    rgba_bytes(rgba, c);
    const uint8_t col[4][4] = {{c[0], c[1], c[2], c[3]}, {c[0], c[1], c[2], c[3]},
                               {c[0], c[1], c[2], c[3]}, {c[0], c[1], c[2], c[3]}};
    const float iw = 1.0f / FONT_ATLAS_W, ih = 1.0f / FONT_ATLAS_H;
    float pen = x, by = floorf(y + 0.5f);
    while (*s) {
        const AtlasGlyph *g = glyph(f, utf8_next(&s));
        if (g->w && g->h) {
            float gx = floorf(pen + 0.5f) + g->xoff, gy = by + g->yoff;
            const float xy[4][2] = {{gx, gy}, {gx + g->w, gy}, {gx, gy + g->h}, {gx + g->w, gy + g->h}};
            float u0 = g->x * iw, v0 = g->y * ih, u1 = (g->x + g->w) * iw, v1 = (g->y + g->h) * ih;
            const float uv[4][2] = {{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}};
            emit(quad(), xy, uv, col, 0, 0, -1, 0, 0);
        }
        pen += g->adv64 / 64.0f;
    }
    return pen - x;
}

float gfx_text_fit(FontId f, float x, float y, uint32_t rgba, const char *s, float max_w) {
    if (gfx_text_width(f, s) <= max_w) return gfx_text(f, x, y, rgba, s);
    char buf[256];
    size_t n = strlen(s);
    if (n >= sizeof buf - 4) n = sizeof buf - 4;
    const float ell = gfx_text_width(f, "\xE2\x80\xA6");
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; /* whole UTF-8 sequences only */
        memcpy(buf, s, n);
        buf[n] = 0;
        if (gfx_text_width(f, buf) + ell <= max_w) break;
    }
    memcpy(buf + n, "\xE2\x80\xA6", 4);
    return gfx_text(f, x, y, rgba, buf);
}

/* ---- frame end ------------------------------------------------------------------------------------------------- */
int gfx_screenshot(const char *path) {
    snprintf(G.shot, sizeof G.shot, "%s", path);
    return 0;
}

static void write_shot(void) {
    size_t n = (size_t)G.w * (size_t)G.h * 4;
    uint8_t *px = malloc(n);
    FILE *f = px ? fopen(G.shot, "wb") : NULL;
    if (f) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, G.w, G.h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        fprintf(f, "P6\n%d %d\n255\n", G.w, G.h);
        for (int y = G.h - 1; y >= 0; y--)
            for (int x = 0; x < G.w; x++) fwrite(px + ((size_t)y * (size_t)G.w + (size_t)x) * 4, 1, 3, f);
        fclose(f);
        bm_log("screenshot %s", G.shot);
    }
    free(px);
    G.shot[0] = 0;
}

int gfx_end(void) {
    flush();
    if (G.shot[0]) write_shot();
    return eglSwapBuffers(G.dpy, G.surface) ? 0 : -1;
}

void gfx_close(void) {
    if (strcmp(G.backend, "fb") != 0) return;
    /* With FB_MULTI_BUFFER=2 the driver pans between two buffers; the X server (and the official app it shows) only
     * draws into the first. The caller swapped the same splash into both buffers, so panning back to 0 keeps the
     * picture and gives X a coherent screen. */
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    struct fb_var_screeninfo v;
    if (fd < 0) return;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && (v.yoffset != 0 || v.xoffset != 0)) {
        v.xoffset = v.yoffset = 0;
        if (ioctl(fd, FBIOPAN_DISPLAY, &v) == 0) bm_log("fb0 panned back to buffer 0");
    }
    close(fd);
}
