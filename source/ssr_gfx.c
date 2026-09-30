/* ssr_gfx.c -- the screen: EGL, the Java side's pictures, the present.
 *
 * On a phone DemoGLSurfaceView is a GLSurfaceView with an OpenGL ES 1 context
 * (setEGLContextClientVersion(1), the default config chooser: RGB with a
 * 16-bit depth buffer); the engine draws its whole 3D frame into it. Here the
 * same: an ES 1 context on the window (libnx's default NWindow) at the
 * [display] resolution, RGBA8 with a 24-bit depth buffer (16 if there is
 * none). The window's size is what DemoRenderer.onSurfaceChanged reports to
 * the engine (nativeSetScreenSize): it lays itself out for it.
 *
 * Over the GL surface Java laid views of its own: while the engine starts up,
 * the splash pictures (res/raw/loading.jpg -- loading_fr.png in French --,
 * loading_white.jpg and loading_sumo.jpg, FIT_XY over the whole screen, the
 * Sumo one at the bottom, the loading one on top: DemoRenderer.createViews),
 * faded out one after the other while the engine runs underneath; then the
 * intro movie's VideoView. Those are drawn here over the engine's frame just
 * before it is presented (gl_blit.c: the engine's GLES 1 state kept).
 *
 * The frame rate: DemoRenderer paced the engine to a 33 ms frame (it slept
 * what was left of it); swap interval 2 (30 fps) keeps that ([display]
 * frame_rate = 60: every vsync). MIT.
 */
#include "config.h"

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "gl_blit.h"
#include "gl_layer.h"
#include "ssr.h"
#include "util.h"

#if DCR_GL_MESA
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

void dcr_window_prepare(void);        /* android_ndk.c */
void dcr_window_size(int *w, int *h); /* android_ndk.c */

static int g_w = 1280, g_h = 720, g_ok;

void ssr_gfx_size(int *w, int *h) {
  if (w)
    *w = g_w;
  if (h)
    *h = g_h;
}
int ssr_gfx_ready(void) { return g_ok; }

/* ------------------------------------------------------------ EGL */
#if DCR_GL_MESA
static EGLDisplay g_dpy;
static EGLSurface g_win;
static EGLContext g_ctx, g_ctx2; /* the engine's; split screen's second copy's (same share group) */
static EGLConfig g_cfg;
static int g_cur = 0;             /* which of the two is current */
EGLBoolean b_eglSwapBuffers(EGLDisplay d, EGLSurface s); /* gl_mesa.c: frame count, hooks */

static int egl_init(void) {
  if (log_console_active())
    debugPrintf("[gfx] handing the screen from the boot log to the game\n");
  log_console_close(); /* for good: see util.c */
  dcr_window_prepare();
  g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint maj = 0, min = 0;
  if (!eglInitialize(g_dpy, &maj, &min)) {
    debugPrintf("[gfx] eglInitialize failed 0x%x\n", eglGetError());
    return -1;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  /* RGBA8 with depth: 24 (the engine never uses stencil: a depth-only
   * buffer clears cheaper), then 24 + stencil 8, then 16, then anything ES 1 */
  static const EGLint depth_try[][2] = {{24, 0}, {24, 8}, {16, 0}};
  EGLConfig cfg = NULL;
  EGLint n = 0, got_depth = 0, got_stencil = 0;
  for (unsigned i = 0; i < sizeof depth_try / sizeof depth_try[0] && n < 1; i++) {
    const EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
                           EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, depth_try[i][0],
                           EGL_STENCIL_SIZE, depth_try[i][1], EGL_NONE};
    if (!eglChooseConfig(g_dpy, want, &cfg, 1, &n))
      n = 0;
  }
  if (n < 1) {
    static const EGLint any[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_NONE};
    if (!eglChooseConfig(g_dpy, any, &cfg, 1, &n) || n < 1) {
      debugPrintf("[gfx] no ES 1 config (0x%x)\n", eglGetError());
      return -1;
    }
  }
  eglGetConfigAttrib(g_dpy, cfg, EGL_DEPTH_SIZE, &got_depth);
  eglGetConfigAttrib(g_dpy, cfg, EGL_STENCIL_SIZE, &got_stencil);
  g_cfg = cfg;
  g_win = eglCreateWindowSurface(g_dpy, cfg, (EGLNativeWindowType)nwindowGetDefault(), NULL);
  static const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE};
  g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
  if (g_win == EGL_NO_SURFACE || g_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, g_win, g_win, g_ctx)) {
    debugPrintf("[gfx] surface %p / context %p / make current failed (0x%x)\n", g_win, g_ctx, eglGetError());
    return -1;
  }
  const int interval = dcr_config()->frame_rate >= 60 ? 1 : 2;
  eglSwapInterval(g_dpy, interval);
  debugPrintf("[gfx] EGL %d.%d, an OpenGL ES 1 context; depth %d, stencil %d; %d fps\n", maj, min, got_depth,
              got_stencil, 60 / interval);
  return 0;
}
static void egl_swap(void) { b_eglSwapBuffers(g_dpy, g_win); }

#else
static int egl_init(void) { return 0; }
static void egl_swap(void) {}
int ssr_gfx_second_context(void) { return -1; }
int ssr_gfx_use(int e, int offscreen) { return e ? -1 : 0; }
void ssr_gfx_split(int mode, const float *pip) {}
static void draw_split2(void) {}
void ssr_gfx_split_source(int engine, float u0, float v0, float u1, float v1) {}
static void draw_split(void) {}
int ssr_gfx_pass(int which) { return -1; }
void ssr_gfx_pass_composite(void) {}
#endif

/* ------------------------------------------------------ the splash pictures */
typedef void (*t_gen)(GLsizei, GLuint *);
typedef void (*t_del)(GLsizei, const GLuint *);
typedef void (*t_bind)(GLenum, GLuint);
typedef void (*t_teximage)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*t_texparami)(GLenum, GLenum, GLint);
typedef void (*t_geti)(GLenum, GLint *);
typedef void (*t_pixelstorei)(GLenum, GLint);
typedef void (*t_clearcolor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*t_clear)(GLbitfield);
typedef const GLubyte *(*t_getstring)(GLenum);
static struct {
  t_gen GenTextures;
  t_del DeleteTextures;
  t_bind BindTexture;
  t_teximage TexImage2D;
  t_texparami TexParameteri;
  t_geti GetIntegerv;
  t_pixelstorei PixelStorei;
  t_clearcolor ClearColor;
  t_clear Clear;
  t_getstring GetString;
} G;

enum { PIC_LOADING, PIC_WHITE, PIC_SUMO, NPIC };
static GLuint g_pic[NPIC];
static float g_alpha[NPIC];

/* A picture of the APK's (res/raw) as a GL texture; 0 if it cannot be read. */
static GLuint texture_from_apk(const char *name, int *w_out, int *h_out) {
  size_t len = 0;
  uint8_t *data = ssr_apk_read(name, &len);
  if (!data) {
    debugPrintf("[gfx] %s: not in the APK\n", name);
    return 0;
  }
  int w = 0, h = 0, comp = 0;
  uint8_t *px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
  free(data);
  if (!px) {
    debugPrintf("[gfx] %s: cannot decode it (%s)\n", name, stbi_failure_reason());
    return 0;
  }
  GLuint tex = 0;
  GLint bound = 0, align = 4;
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  G.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
  G.GenTextures(1, &tex);
  G.BindTexture(GL_TEXTURE_2D, tex);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
  G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  G.PixelStorei(GL_UNPACK_ALIGNMENT, align);
  G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
  stbi_image_free(px);
  if (w_out)
    *w_out = w;
  if (h_out)
    *h_out = h;
  return tex;
}

static void load_splash(void) {
  /* DemoRenderer.createViews: loading_fr.png in French, loading.jpg else */
  const char *loading = ssr_language() == 1 ? "res/raw/loading_fr.png" : "res/raw/loading.jpg";
  g_pic[PIC_LOADING] = texture_from_apk(loading, NULL, NULL);
  g_pic[PIC_WHITE] = texture_from_apk("res/raw/loading_white.jpg", NULL, NULL);
  g_pic[PIC_SUMO] = texture_from_apk("res/raw/loading_sumo.jpg", NULL, NULL);
}

void ssr_gfx_splash(float loading, float white, float sumo) {
  g_alpha[PIC_LOADING] = loading;
  g_alpha[PIC_WHITE] = white;
  g_alpha[PIC_SUMO] = sumo;
  /* once they are all gone for good, their memory */
  if (loading <= 0 && white <= 0 && sumo <= 0 && G.DeleteTextures) {
    for (int i = 0; i < NPIC; i++)
      if (g_pic[i]) {
        G.DeleteTextures(1, &g_pic[i]);
        g_pic[i] = 0;
      }
  }
}

/* DemoActivity.l (javaLoadDownloadBGTexture): the expansion file's download
 * screen's picture as a texture -- here only if the engine asks. */
int ssr_gfx_download_bg_texture(void) {
  static const char *const bg[] = {"loadingbg_en", "loadingbg_fr", "loadingbg_it", "loadingbg_de", "loadingbg_es",
                                   "loadingbg_jp", "loadingbg_en"};
  int lang = ssr_language();
  char name[64];
  snprintf(name, sizeof name, "res/raw/%s.jpg", bg[lang >= 0 && lang < 7 ? lang : 0]);
  return (int)texture_from_apk(name, NULL, NULL);
}

/* a PNG (or JPEG) in memory as RGBA (free() it), for the port's own pictures */
uint8_t *ssr_gfx_png(const void *data, int len, int *w, int *h) {
  int comp = 0;
  return stbi_load_from_memory(data, len, w, h, &comp, 4);
}

/* ------------------------------------------------------------ the pointer */
/* The menus' pointer (ssr_input.c): a ring, filled while A holds it down,
 * drawn from code (64x64 RGBA: white with a dark edge, anti-aliased). */
#define PTR_SIZE 64
static GLuint g_ptr_tex[2]; /* open, pressed */

static void make_pointer(void) {
  static uint8_t px[PTR_SIZE * PTR_SIZE * 4];
  for (int k = 0; k < 2; k++) {
    for (int y = 0; y < PTR_SIZE; y++)
      for (int x = 0; x < PTR_SIZE; x++) {
        const float dx = (float)x + 0.5f - PTR_SIZE / 2.0f, dy = (float)y + 0.5f - PTR_SIZE / 2.0f;
        const float r = sqrtf(dx * dx + dy * dy);
        /* ring 18..26 px (pressed: a disc to 26), edge 2 px darker */
        const float inner = k ? 0.0f : 17.0f, outer = 27.0f;
        float a = 0.0f, c = 1.0f;
        if (r >= inner - 1.0f && r <= outer + 1.0f) {
          a = fminf(fminf(r - (inner - 1.0f), (outer + 1.0f) - r), 1.0f);
          if (!k && r < inner + 2.0f)
            c = 0.15f; /* the inner edge */
          if (r > outer - 2.0f)
            c = 0.15f; /* the outer edge */
        }
        if (k && r < outer - 2.0f)
          c = 1.0f;
        uint8_t *o = px + (y * PTR_SIZE + x) * 4;
        o[0] = o[1] = o[2] = (uint8_t)(c * 255.0f);
        o[3] = (uint8_t)(a * (k ? 200.0f : 235.0f));
      }
    GLint bound = 0;
    G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    G.GenTextures(1, &g_ptr_tex[k]);
    G.BindTexture(GL_TEXTURE_2D, g_ptr_tex[k]);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PTR_SIZE, PTR_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
  }
}

static void draw_pointer(void) {
  float x, y;
  int pressed;
  if (!g_ptr_tex[0] || !ssr_input_pointer(&x, &y, &pressed))
    return;
  const int size = (int)(40.0f * (float)g_h / 720.0f);
  dcr_blit_alpha(g_ptr_tex[pressed ? 1 : 0], (int)x - size / 2, (int)y - size / 2, size, size, 1.0f);
}

#if DCR_GL_MESA
static GLuint g_white_tex; /* 1x1 white: solid colour quads */
/* ---------------------------------------------------- split screen: two engines
 * Each copy of the engine keeps a GL state cache (bound texture, enable bits)
 * and skips calls it thinks redundant, so the two cannot share a context: the
 * second has its own, in the first's share group (textures seen by both).
 * An engine draws into whatever framebuffer is bound (it binds none itself):
 * in a split race each draws into a framebuffer object of its own (a colour
 * texture, a depth renderbuffer), W x H, its W x H/2 view in the lower half
 * (GL's), and the present puts the two in the window's halves
 * (MULTIPLAYER.md 5, 6). */
typedef void (*t_genfb)(GLsizei, GLuint *);
typedef void (*t_bindfb)(GLenum, GLuint);
typedef void (*t_fbtex)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*t_rbstorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*t_fbrb)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (*t_fbstatus)(GLenum);
static struct {
  t_genfb GenFramebuffers, GenRenderbuffers;
  t_bindfb BindFramebuffer, BindRenderbuffer;
  t_fbtex FramebufferTexture2D;
  t_rbstorage RenderbufferStorage;
  t_fbrb FramebufferRenderbuffer;
  t_fbstatus CheckFramebufferStatus;
  int ok;
} FB;
static struct {
  GLuint fbo, tex, depth;
  int ready; /* 1 made, -1 failed */
} g_fb[3]; /* engine 1's, engine 2's (its context), and a split race's second half (engine 1's context) */

static int fb_setup(void) {
  if (FB.ok)
    return FB.ok > 0;
  FB.ok = -1;
#define LF(n, s) (FB.n = (void *)eglGetProcAddress(s))
  LF(GenFramebuffers, "glGenFramebuffersOES"), LF(GenRenderbuffers, "glGenRenderbuffersOES");
  LF(BindFramebuffer, "glBindFramebufferOES"), LF(BindRenderbuffer, "glBindRenderbufferOES");
  LF(FramebufferTexture2D, "glFramebufferTexture2DOES"), LF(RenderbufferStorage, "glRenderbufferStorageOES");
  LF(FramebufferRenderbuffer, "glFramebufferRenderbufferOES");
  LF(CheckFramebufferStatus, "glCheckFramebufferStatusOES");
#undef LF
  if (FB.GenFramebuffers && FB.GenRenderbuffers && FB.BindFramebuffer && FB.BindRenderbuffer &&
      FB.FramebufferTexture2D && FB.RenderbufferStorage && FB.FramebufferRenderbuffer)
    FB.ok = 1;
  else
    debugPrintf("[gfx] no GL_OES_framebuffer_object: no split screen\n");
  return FB.ok > 0;
}

/* engine e's framebuffer, made in its context (current) */
static int fb_make(int e) {
  if (g_fb[e].ready)
    return g_fb[e].ready > 0;
  g_fb[e].ready = -1;
  if (!fb_setup())
    return 0;
  GLint bound = 0;
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  G.GenTextures(1, &g_fb[e].tex);
  G.BindTexture(GL_TEXTURE_2D, g_fb[e].tex);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_w, g_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
  FB.GenRenderbuffers(1, &g_fb[e].depth);
  FB.BindRenderbuffer(GL_RENDERBUFFER_OES, g_fb[e].depth);
  FB.RenderbufferStorage(GL_RENDERBUFFER_OES, GL_DEPTH_COMPONENT24_OES, g_w, g_h);
  FB.GenFramebuffers(1, &g_fb[e].fbo);
  FB.BindFramebuffer(GL_FRAMEBUFFER_OES, g_fb[e].fbo);
  FB.FramebufferTexture2D(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, g_fb[e].tex, 0);
  FB.FramebufferRenderbuffer(GL_FRAMEBUFFER_OES, GL_DEPTH_ATTACHMENT_OES, GL_RENDERBUFFER_OES, g_fb[e].depth);
  const GLenum st = FB.CheckFramebufferStatus ? FB.CheckFramebufferStatus(GL_FRAMEBUFFER_OES) : 0;
  FB.BindFramebuffer(GL_FRAMEBUFFER_OES, 0);
  g_fb[e].ready = st == GL_FRAMEBUFFER_COMPLETE_OES ? 1 : -1;
  debugPrintf("[gfx] framebuffer %d (%s) %dx%d: %s (0x%x)\n", e, e == 1 ? "engine 2's" : "engine 1's", g_w, g_h,
              g_fb[e].ready > 0 ? "ready" : "INCOMPLETE", (unsigned)st);
  return g_fb[e].ready > 0;
}

/* the second engine's context, in the first's share group */
int ssr_gfx_second_context(void) {
  if (g_ctx2)
    return 0;
  static const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE};
  g_ctx2 = eglCreateContext(g_dpy, g_cfg, g_ctx, attrs);
  debugPrintf("[gfx] the second engine's GL context: %s (0x%x)\n", g_ctx2 != EGL_NO_CONTEXT ? "made" : "FAILED",
              eglGetError());
  if (g_ctx2 == EGL_NO_CONTEXT) {
    g_ctx2 = NULL;
    return -1;
  }
  return 0;
}

/* Before an engine runs (or the present): its context current, and its
 * framebuffer bound (offscreen) or the window's. 0 on success. */
static void ctx2_first_use(void);

int ssr_gfx_use(int e, int offscreen) {
  if (e && !g_ctx2)
    return -1;
  if (g_cur != e) {
    if (!eglMakeCurrent(g_dpy, g_win, g_win, e ? g_ctx2 : g_ctx)) {
      debugPrintf("[gfx] eglMakeCurrent(engine %d) failed 0x%x\n", e + 1, eglGetError());
      return -1;
    }
    g_cur = e;
    if (e)
      ctx2_first_use();
  }
  if (offscreen) {
    if (!fb_make(e))
      return -1;
    FB.BindFramebuffer(GL_FRAMEBUFFER_OES, g_fb[e].fbo);
  } else if (FB.ok > 0) {
    FB.BindFramebuffer(GL_FRAMEBUFFER_OES, 0);
  }
  return 0;
}

/* The second context, the first time it is current: what DemoRenderer's
 * onSurfaceCreated did to the first (glDepthFunc(GL_LEQUAL): the engine
 * never sets it, it counts on it), and whether the two share textures (the
 * log says; the compositing below does not depend on it). */
static void ctx2_first_use(void) {
  static int done;
  if (done)
    return;
  done = 1;
  void (*depth)(GLenum) = (void (*)(GLenum))dcr_gl_lookup("glDepthFunc");
  if (depth)
    depth(GL_LEQUAL);
  GLboolean (*is_tex)(GLuint) = (GLboolean(*)(GLuint))dcr_gl_lookup("glIsTexture");
  debugPrintf("[gfx] the second context: depth test LEQUAL; textures %s with the first\n",
              !is_tex || !g_white_tex ? "(unknown)" : is_tex(g_white_tex) ? "shared" : "NOT shared");
}

/* what the present shows of the two (ssr_split.c, every frame) */
static struct {
  int mode;         /* SSR_SPLIT_* */
  float pip[4];     /* the second engine's inset: x, y, w, h (window pixels) */
  float src[2][4];  /* each engine's part of its framebuffer: u0, v0, u1, v1 */
} g_split = {SSR_SPLIT_NONE, {0}, {{0, 0, 1, 1}, {0, 0, 1, 1}}};
void ssr_gfx_split(int mode, const float *pip) {
  g_split.mode = mode;
  if (pip)
    memcpy(g_split.pip, pip, sizeof g_split.pip);
}
void ssr_gfx_split_source(int e, float u0, float v0, float u1, float v1) {
  float *s = g_split.src[e & 1];
  s[0] = u0, s[1] = v0, s[2] = u1, s[3] = v1;
}

/* engine e's picture (its source part: v0 the bottom row, GL's) into the
 * window rectangle x, y, w, h (from the top left) */
static void composite(int e, float x, float y, float w, float h) {
  if (g_fb[e].ready <= 0)
    return;
  const float *s = g_split.src[e];
  const float u0 = s[0], v0 = s[1], u1 = s[2], v1 = s[3];
  const GLfloat pos[12] = {x, y, x + w, y, x, y + h, x + w, y, x + w, y + h, x, y + h};
  const GLfloat uv[12] = {u0, v1, u1, v1, u0, v0, u1, v1, u1, v0, u0, v0};
  dcr_blit_mesh_opaque(g_fb[e].tex, pos, uv, 6);
}

/* Player 2's picture, drawn by player 2's own context into the window
 * (before the first context's part): its framebuffer's texture is its
 * context's -- sampling it from the first needs the two to share textures,
 * which this Mesa may not do (hardware run: the first context sampled its
 * own texture of that number, a white picture). */
static void draw_split2(void) {
  if (g_split.mode == SSR_SPLIT_NONE || g_fb[1].ready <= 0 || ssr_gfx_use(1, 0) != 0)
    return;
  if (g_split.mode == SSR_SPLIT_RACE)
    composite(1, 0, (float)g_h * 0.5f, (float)g_w, (float)g_h * 0.5f); /* the bottom */
  else if (g_split.mode == SSR_SPLIT_COLUMNS)
    composite(1, (float)g_w * 0.5f, 0, (float)g_w * 0.5f, (float)g_h); /* the right half */
  else if (g_split.mode == SSR_SPLIT_PIP)
    composite(1, g_split.pip[0], g_split.pip[1], g_split.pip[2], g_split.pip[3]);
}

static void draw_split(void) {
  if (g_split.mode == SSR_SPLIT_RACE) {
    /* each engine's W x H/2 view is the lower half of its framebuffer */
    const float half = (float)g_h * 0.5f;
    composite(0, 0, 0, (float)g_w, half); /* player 1: the top (player 2's: draw_split2) */
    /* a dark line between */
    const float t = fmaxf(2.0f, (float)g_h / 360.0f);
    const GLfloat pos[12] = {0, half - t, (float)g_w, half - t, 0, half + t, (float)g_w, half - t, (float)g_w, half + t,
                             0, half + t};
    const GLfloat uv[12] = {0};
    const GLfloat black[4] = {0.02f, 0.03f, 0.08f, 1.0f};
    if (g_white_tex)
      dcr_blit_mesh(g_white_tex, pos, uv, 6, black);
  } else if (g_split.mode == SSR_SPLIT_COLUMNS) {
    /* the shared SELECT RACER: player 2's copy's right half over player 1's
     * (draw_split2) */
  } else if (g_split.mode == SSR_SPLIT_JOINING && g_white_tex) {
    /* player 2's column while their copy gets to their screens: the menus'
     * sky, in bands (the card on it: ssr_prompt.c) */
    enum { BANDS = 24 };
    const float x0 = (float)g_w * 0.5f, x1 = (float)g_w;
    static const float sky[3] = {0.0f, 0.353f, 0.933f}, sea[3] = {0.47f, 0.72f, 0.97f}; /* the menus' sky, top to horizon */
    const GLfloat uv[12] = {0};
    for (int k = 0; k < BANDS; k++) {
      const float y0 = (float)g_h * (float)k / BANDS, y1 = (float)g_h * (float)(k + 1) / BANDS;
      const float t = ((float)k + 0.5f) / BANDS;
      const GLfloat c[4] = {sky[0] + (sea[0] - sky[0]) * t, sky[1] + (sea[1] - sky[1]) * t,
                            sky[2] + (sea[2] - sky[2]) * t, 1.0f};
      const GLfloat pos[12] = {x0, y0, x1, y0, x0, y1, x1, y0, x1, y1, x0, y1};
      dcr_blit_mesh(g_white_tex, pos, uv, 6, c);
    }
  } else if (g_split.mode == SSR_SPLIT_PIP) {
    /* player 2's inset, framed in their controller's colour (the controller
     * screen's red) */
    const float *r = g_split.pip, b = fmaxf(3.0f, (float)g_h / 240.0f);
    const float x0 = r[0] - b, y0 = r[1] - b, x1 = r[0] + r[2] + b, y1 = r[1] + r[3] + b;
    const float edges[4][4] = {{x0, y0, x1, r[1]}, {x0, r[1] + r[3], x1, y1}, {x0, r[1], r[0], r[1] + r[3]},
                               {r[0] + r[2], r[1], x1, r[1] + r[3]}}; /* around the picture (draw_split2) */
    const GLfloat uv[12] = {0};
    const GLfloat red[4] = {0.91f, 0.16f, 0.16f, 0.95f};
    for (int k = 0; k < 4 && g_white_tex; k++) {
      const float *q = edges[k];
      const GLfloat pos[12] = {q[0], q[1], q[2], q[1], q[0], q[3], q[2], q[1], q[2], q[3], q[0], q[3]};
      dcr_blit_mesh(g_white_tex, pos, uv, 6, red);
    }
  }
}
#endif

/* ---------------------------------------------------- split screen: a race's passes
 * (ssr_split_race.c, inside engine 1's frame, its context current): each
 * half drawn W x H/2 into the lower half of a framebuffer of its own, then
 * both into the window, player 1's the top half */
int ssr_gfx_pass(int which) {
  if (g_cur != 0)
    return -1;
  if (which < 0) {
    if (FB.ok > 0)
      FB.BindFramebuffer(GL_FRAMEBUFFER_OES, 0);
    return 0;
  }
  const int k = which ? 2 : 0;
  if (!fb_make(k))
    return -1;
  FB.BindFramebuffer(GL_FRAMEBUFFER_OES, g_fb[k].fbo);
  return 0;
}

static void blit_half(int k, float y) {
  if (g_fb[k].ready <= 0)
    return;
  const float x = 0, w = (float)g_w, h = (float)g_h * 0.5f;
  const GLfloat pos[12] = {x, y, x + w, y, x, y + h, x + w, y, x + w, y + h, x, y + h};
  const GLfloat uv[12] = {0, 0.5f, 1, 0.5f, 0, 0, 1, 0.5f, 1, 0, 0, 0};
  dcr_blit_mesh_opaque(g_fb[k].tex, pos, uv, 6);
}

void ssr_gfx_pass_composite(void) {
  const float half = (float)g_h * 0.5f;
  blit_half(0, 0);
  blit_half(2, half);
  /* a dark line between */
  const float t = fmaxf(1.5f, (float)g_h / 480.0f);
  const GLfloat pos[12] = {0, half - t, (float)g_w, half - t, 0, half + t, (float)g_w, half - t, (float)g_w, half + t,
                           0, half + t};
  const GLfloat uv[12] = {0};
  const GLfloat black[4] = {0.02f, 0.03f, 0.08f, 1.0f};
  if (g_white_tex)
    dcr_blit_mesh(g_white_tex, pos, uv, 6, black);
}

/* ------------------------------------------------------------ the focus */
/* The focused button's glow (ssr_menu.c), in the manner of the Plants vs.
 * Zombies port's Zombatar menu: no outline, light -- a soft Gaussian halo
 * spilling out from the button's edges and a faint rim of light just inside
 * them, added to the picture (additive blending), warm white, breathing
 * gently. One 112x112 texture drawn in code (the halo around a rounded
 * rectangle inset 40 texels), drawn as a 9-slice: its corners at a fixed
 * size, its edges stretched, so it fits any button. It glides from button to
 * button. */
#define GLOW_TEX 112
#define GLOW_INSET 40.0f  /* the button's edge, texels from the texture's */
#define GLOW_CORNER 50    /* texels of the texture's corners */
static GLuint g_glow_tex;
typedef struct {
  int want, shown;
  float to[4], at[4]; /* x, y, w, h: the target, the glow now */
  float fade, t;
} Focus;
static Focus g_foc, g_foc2; /* player 1's; split screen's player 2's */

static void make_glow(void) {
  static uint8_t px[GLOW_TEX * GLOW_TEX * 4];
  const float inset = GLOW_INSET, radius = 12.0f;
  const float sigma_out = 13.0f, sigma_in = 7.0f;
  for (int y = 0; y < GLOW_TEX; y++)
    for (int x = 0; x < GLOW_TEX; x++) {
      const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
      /* signed distance to the rounded rectangle (negative inside) */
      const float lo = inset + radius, hi = GLOW_TEX - inset - radius;
      const float qx = fx < lo ? lo - fx : fx > hi ? fx - hi : 0.0f;
      const float qy = fy < lo ? lo - fy : fy > hi ? fy - hi : 0.0f;
      float d;
      if (qx > 0.0f || qy > 0.0f)
        d = sqrtf(qx * qx + qy * qy) - radius;
      else
        d = -fminf(fminf(fx - inset, GLOW_TEX - inset - fx), fminf(fy - inset, GLOW_TEX - inset - fy));
      float a = d >= 0.0f ? 0.78f * expf(-(d * d) / (2.0f * sigma_out * sigma_out)) /* the halo */
                          : 0.26f * expf(-(d * d) / (2.0f * sigma_in * sigma_in)); /* the rim light */
      uint8_t *o = px + (y * GLOW_TEX + x) * 4;
      o[0] = o[1] = o[2] = 255;
      o[3] = (uint8_t)(fminf(a, 1.0f) * 255.0f + 0.5f);
    }
  GLint bound = 0;
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  G.GenTextures(1, &g_glow_tex);
  G.BindTexture(GL_TEXTURE_2D, g_glow_tex);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLOW_TEX, GLOW_TEX, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
}

/* ssr_menu.c, every frame: the button the focus is on (window pixels from the
 * top left), or none */
static void focus_set(Focus *f, int on, float x, float y, float w, float h) {
  f->want = on;
  if (!on)
    return;
  f->to[0] = x, f->to[1] = y, f->to[2] = w, f->to[3] = h;
  if (!f->shown) /* appearing: straight there */
    memcpy(f->at, f->to, sizeof f->at);
}
void ssr_gfx_focus(int on, float x, float y, float w, float h) { focus_set(&g_foc, on, x, y, w, h); }
void ssr_gfx_focus2(int on, float x, float y, float w, float h) { focus_set(&g_foc2, on, x, y, w, h); }

static void draw_focus_of(Focus *fo) {
  if (!g_glow_tex)
    return;
  const int sixty = dcr_config()->frame_rate >= 60;
  /* fade in / out over ~0.15 s; glide ~30% of the way each frame */
  fo->fade += (fo->want ? 1.0f : -1.0f) * (sixty ? 0.11f : 0.22f);
  fo->fade = fo->fade < 0.0f ? 0.0f : fo->fade > 1.0f ? 1.0f : fo->fade;
  fo->shown = fo->fade > 0.0f;
  if (!fo->shown)
    return;
  const float k = sixty ? 0.30f : 0.51f;
  for (int i = 0; i < 4; i++)
    fo->at[i] += (fo->to[i] - fo->at[i]) * k;
  fo->t += sixty ? 1.0f / 60.0f : 1.0f / 30.0f;
  /* the texture's inset lands on the button's edge */
  const float scale = (float)g_h / 720.0f;
  const float tpx = 0.62f * scale;              /* screen pixels a texel */
  const float cs = GLOW_CORNER * tpx, pad = GLOW_INSET * tpx;
  float x0 = fo->at[0] - pad, y0 = fo->at[1] - pad;
  float x3 = fo->at[0] + fo->at[2] + pad, y3 = fo->at[1] + fo->at[3] + pad;
  if (x3 - x0 < 2 * cs)
    x3 = x0 + 2 * cs;
  if (y3 - y0 < 2 * cs)
    y3 = y0 + 2 * cs;
  const float xs[4] = {x0, x0 + cs, x3 - cs, x3}, ys[4] = {y0, y0 + cs, y3 - cs, y3};
  const float c = (float)GLOW_CORNER / GLOW_TEX;
  const float us[4] = {0.0f, c, 1.0f - c, 1.0f};
  GLfloat pos[9 * 12], uv[9 * 12];
  int n = 0;
  for (int j = 0; j < 3; j++)
    for (int i = 0; i < 3; i++) {
      const float qx[2] = {xs[i], xs[i + 1]}, qy[2] = {ys[j], ys[j + 1]};
      const float qu[2] = {us[i], us[i + 1]}, qv[2] = {us[j], us[j + 1]};
      static const int tri[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
      for (int v = 0; v < 6; v++) {
        pos[n * 2] = qx[tri[v][0]], pos[n * 2 + 1] = qy[tri[v][1]];
        uv[n * 2] = qu[tri[v][0]], uv[n * 2 + 1] = qv[tri[v][1]];
        n++;
      }
    }
  /* warm white light, breathing between 70% and 100% */
  const float pulse = 0.85f + 0.15f * sinf(fo->t * 2.0f * (float)M_PI * 0.9f);
  const GLfloat rgba[4] = {1.0f, 0.93f, 0.66f, fo->fade * pulse};
  dcr_blit_mesh_add(g_glow_tex, pos, uv, n, rgba);
}

/* split screen's racer select: each column's portraits fade into the sky at
 * the edges of their window (ssr_split.c) -- bands of the sky's colour,
 * opaque at the window's edge, clear FADE_W inside it */
enum { MAX_FADES = 4 };
static struct {
  int n;
  float r[MAX_FADES][4]; /* x (the edge), y0, y1, the inward width (+ right, - left) */
} g_fades;
void ssr_gfx_edge_fade(float x, float y0, float y1, float w) {
  if (g_fades.n < MAX_FADES) {
    float *r = g_fades.r[g_fades.n++];
    r[0] = x, r[1] = y0, r[2] = y1, r[3] = w;
  }
}
static void draw_fades(void) {
#if DCR_GL_MESA
  enum { BANDS = 16 };
  const GLfloat uv[12] = {0};
  for (int k = 0; k < g_fades.n && g_white_tex; k++) {
    const float *r = g_fades.r[k];
    for (int b = 0; b < BANDS; b++) {
      const float xa = r[0] + r[3] * (float)b / BANDS, xb = r[0] + r[3] * (float)(b + 1) / BANDS;
      const float x0 = fminf(xa, xb), x1 = fmaxf(xa, xb), t = ((float)b + 0.5f) / BANDS;
      const GLfloat c[4] = {0.0f, 0.353f, 0.933f, (1.0f - t) * (1.0f - t)};
      const GLfloat pos[12] = {x0, r[1], x1, r[1], x0, r[2], x1, r[1], x1, r[2], x0, r[2]};
      dcr_blit_mesh(g_white_tex, pos, uv, 6, c);
    }
  }
#endif
  g_fades.n = 0;
}

static void draw_focus(void) {
  draw_focus_of(&g_foc);
  if (!ssr_split_columns())
    g_foc2.want = 0;
  draw_focus_of(&g_foc2);
}

/* ------------------------------------------------------------ the frames */
#define L(n) (G.n = (void *)dcr_gl_lookup("gl" #n))
int ssr_gfx_init(void) {
  dcr_window_size(&g_w, &g_h);
  if (egl_init() != 0)
    return -1;
  L(GenTextures), L(DeleteTextures), L(BindTexture), L(TexImage2D), L(TexParameteri), L(GetIntegerv);
  L(PixelStorei), L(ClearColor), L(Clear), L(GetString);
  if (!G.GenTextures || !G.BindTexture || !G.TexImage2D || !G.TexParameteri || !G.GetIntegerv || !G.PixelStorei ||
      !G.Clear || !G.ClearColor) {
    debugPrintf("[gfx] the GL driver lacks a basic call\n");
    return -1;
  }
  if (G.GetString)
    debugPrintf("[gfx] %s | %s | %s\n", (const char *)G.GetString(GL_VENDOR), (const char *)G.GetString(GL_RENDERER),
                (const char *)G.GetString(GL_VERSION));
  if (dcr_blit_setup("splash") < 0)
    debugPrintf("[gfx] no overlay drawing: the splash pictures are left out\n");
  load_splash();
  make_pointer();
  make_glow();
#if DCR_GL_MESA
  {
    static const uint8_t white[4] = {255, 255, 255, 255};
    G.GenTextures(1, &g_white_tex);
    G.BindTexture(GL_TEXTURE_2D, g_white_tex);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    G.BindTexture(GL_TEXTURE_2D, 0);
  }
#endif
  G.ClearColor(0, 0, 0, 1);
  G.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  g_ok = 1;
  debugPrintf("[gfx] window %dx%d\n", g_w, g_h);
  return 0;
}

void ssr_gfx_present(void) {
  if (!g_ok)
    return;
  draw_split2();     /* split screen: player 2's picture, by its own context */
  ssr_gfx_use(0, 0); /* the first engine's context, the window */
  draw_split();      /* ...and player 1's */
  draw_fades();
  ssr_prompt_draw(g_w, g_h); /* over the game, under the splash */
  draw_focus();
  draw_pointer();
  /* the Java views over the surface, bottom to top (createViews' order) */
  static const int order[NPIC] = {PIC_SUMO, PIC_WHITE, PIC_LOADING};
  for (int k = 0; k < NPIC; k++) {
    int i = order[k];
    if (g_pic[i] && g_alpha[i] > 0.0f)
      dcr_blit_alpha(g_pic[i], 0, 0, g_w, g_h, g_alpha[i]);
  }
  ssr_video_draw(); /* the intro's VideoView, over everything */
  egl_swap();
}
