/* ssr_text.c -- DemoRenderer.javaImageText*: text drawn by "Java" into a GL texture.
 *
 * The engine has some of its text drawn by the Java side (com.sega.ssasr.e):
 *   javaImageTextInitialise()      e.a(): a new GL texture of its own (linear
 *                                  filtering), a 512x512 ARGB_8888 bitmap
 *                                  uploaded blank, 8 empty lines
 *   javaImageTextInitialise(int t) e.a(t): the engine's texture t instead (the
 *                                  blank bitmap goes to whatever is bound)
 *   javaImageTextSetText(i, s)     line i (0..7) = s
 *   javaImageTextBakeToTexture()   e.c(): the bitmap cleared, each line drawn
 *                                  with a Paint of 28 px, white, left-aligned,
 *                                  a shadow (radius 2, offset 1,1, black), its
 *                                  baseline at i * 64 - ascent; uploaded
 *                                  (GLUtils.texImage2D: premultiplied RGBA) to
 *                                  its own texture (bound first) or, with the
 *                                  engine's, to what is bound; returns the id
 *   javaImageTextGetTextureId()    e.b()
 *   javaImageTextTerminate()       e.d(): its own texture deleted
 * Here the text comes from ssr_font.c (the console's shared fonts: every
 * language the game has), the shadow is the coverage blurred and offset, and
 * the result is premultiplied as Android uploads it. MIT.
 */
#include <GLES/gl.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "gl_layer.h"
#include "ssr.h"
#include "util.h"

#define SIZE 512
#define LINES 8
#define LINE_H 64
#define TEXT_PX 28.0f

int ssr_font_init(void);
float ssr_font_ascent(float px);
void ssr_font_draw(uint8_t *dst, int w, int h, int stride, float x, float y, float px, const char *utf8, int bold);

static struct {
  void (*GenTextures)(GLsizei, GLuint *);
  void (*DeleteTextures)(GLsizei, const GLuint *);
  void (*BindTexture)(GLenum, GLuint);
  void (*TexParameterf)(GLenum, GLenum, GLfloat);
  void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
  void (*PixelStorei)(GLenum, GLint);
  void (*GetIntegerv)(GLenum, GLint *);
} G;
static int g_gl;

static int gl_ready(void) {
  if (!g_gl) {
#define L(n) (G.n = (void *)dcr_gl_lookup("gl" #n))
    L(GenTextures), L(DeleteTextures), L(BindTexture), L(TexParameterf), L(TexImage2D), L(PixelStorei);
    L(GetIntegerv);
#undef L
    g_gl = G.GenTextures && G.BindTexture && G.TexParameterf && G.TexImage2D && G.GetIntegerv ? 1 : -1;
    if (g_gl < 0)
      debugPrintf("[text] the GL driver lacks a call: no text textures\n");
  }
  return g_gl > 0;
}

static GLuint g_tex;     /* e.a */
static int g_engine_tex; /* e.d: the texture is the engine's */
static uint8_t *g_rgba;  /* e.b, premultiplied RGBA */
static char *g_line[LINES];
static uint8_t g_cov[SIZE * SIZE], g_shadow[SIZE * SIZE], g_tmp[SIZE * SIZE];

static void upload(void) {
  GLint align = 4;
  G.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
  if (G.PixelStorei)
    G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
  G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIZE, SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba);
  if (G.PixelStorei)
    G.PixelStorei(GL_UNPACK_ALIGNMENT, align);
}

static void start(void) {
  if (!g_rgba)
    g_rgba = malloc(SIZE * SIZE * 4);
  if (g_rgba)
    memset(g_rgba, 0, SIZE * SIZE * 4);
  for (int i = 0; i < LINES; i++) {
    free(g_line[i]);
    g_line[i] = strdup("");
  }
  ssr_font_init();
}

void ssr_text_init_own(void) {
  if (!gl_ready())
    return;
  G.GenTextures(1, &g_tex);
  g_engine_tex = 0;
  G.BindTexture(GL_TEXTURE_2D, g_tex);
  G.TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  G.TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  start();
  if (g_rgba)
    upload();
  debugPrintf("[text] javaImageTextInitialise(): texture %u\n", g_tex);
}

void ssr_text_init_tex(int tex) {
  if (!gl_ready())
    return;
  g_tex = (GLuint)tex;
  g_engine_tex = 1;
  start();
  if (g_rgba)
    upload(); /* to what the engine has bound, as GLUtils.texImage2D did */
  debugPrintf("[text] javaImageTextInitialise(%d)\n", tex);
}

void ssr_text_set(int slot, const char *utf8) {
  if (slot < 0 || slot >= LINES)
    return;
  free(g_line[slot]);
  g_line[slot] = strdup(utf8 ? utf8 : "");
}

int ssr_text_tex(void) { return (int)g_tex; }

/* a box blur of radius r (horizontal then vertical), once */
static void blur(uint8_t *img, int r) {
  for (int y = 0; y < SIZE; y++) {
    const uint8_t *s = img + y * SIZE;
    uint8_t *d = g_tmp + y * SIZE;
    int sum = 0;
    for (int x = -r; x <= r; x++)
      sum += x >= 0 && x < SIZE ? s[x] : 0;
    for (int x = 0; x < SIZE; x++) {
      d[x] = (uint8_t)(sum / (2 * r + 1));
      int out = x - r, in = x + r + 1;
      sum += (in < SIZE ? s[in] : 0) - (out >= 0 ? s[out] : 0);
    }
  }
  for (int x = 0; x < SIZE; x++) {
    int sum = 0;
    for (int y = -r; y <= r; y++)
      sum += y >= 0 && y < SIZE ? g_tmp[y * SIZE + x] : 0;
    for (int y = 0; y < SIZE; y++) {
      img[y * SIZE + x] = (uint8_t)(sum / (2 * r + 1));
      int out = y - r, in = y + r + 1;
      sum += (in < SIZE ? g_tmp[in * SIZE + x] : 0) - (out >= 0 ? g_tmp[out * SIZE + x] : 0);
    }
  }
}

int ssr_text_bake(void) {
  if ((!g_tex && !g_engine_tex) || !g_rgba || !gl_ready())
    return 0;
  memset(g_cov, 0, sizeof g_cov);
  const float asc = ssr_font_ascent(TEXT_PX);
  int any = 0;
  for (int i = 0; i < LINES; i++)
    if (g_line[i] && g_line[i][0]) {
      ssr_font_draw(g_cov, SIZE, SIZE, SIZE, 0.0f, (float)(i * LINE_H) + asc, TEXT_PX, g_line[i], 0);
      any = 1;
    }
  /* the shadow: the coverage blurred (radius 2) and moved by (1, 1) */
  memset(g_shadow, 0, sizeof g_shadow);
  if (any) {
    for (int y = SIZE - 1; y >= 1; y--)
      memcpy(g_shadow + y * SIZE + 1, g_cov + (y - 1) * SIZE, SIZE - 1);
    blur(g_shadow, 1);
    blur(g_shadow, 1);
  }
  /* white text over its black shadow, premultiplied */
  for (int k = 0; k < SIZE * SIZE; k++) {
    const unsigned t = g_cov[k], s = g_shadow[k];
    const unsigned a = t + (s * (255 - t) + 127) / 255;
    g_rgba[k * 4 + 0] = g_rgba[k * 4 + 1] = g_rgba[k * 4 + 2] = (uint8_t)t;
    g_rgba[k * 4 + 3] = (uint8_t)a;
  }
  if (!g_engine_tex)
    G.BindTexture(GL_TEXTURE_2D, g_tex);
  upload();
  return (int)g_tex;
}

void ssr_text_terminate(void) {
  if (g_tex && !g_engine_tex && gl_ready() && G.DeleteTextures)
    G.DeleteTextures(1, &g_tex);
  for (int i = 0; i < LINES; i++) {
    free(g_line[i]);
    g_line[i] = NULL;
  }
  free(g_rgba);
  g_rgba = NULL;
  g_tex = 0;
  g_engine_tex = 0;
}
