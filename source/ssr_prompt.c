/* ssr_prompt.c -- the console editions' button prompts at the bottom of the
 * menus: "(+) SELECT  (B) BACK  (A) OK", white, at the bottom right.
 *
 * The phone game had none (a finger needs no telling). ssr_menu.c picks the
 * set for what has the controller each frame (ssr_prompt_set); this draws it,
 * over the game and under the splash: the buttons' white pictures
 * (resources/buttons/icons.png, drawn by tools/make_button_icons.py and built
 * into the program: ssr_res.S) and the words in the console's shared font
 * (ssr_font.c), white with a soft dark shadow, in the game's language. A set
 * is rasterised once into a texture when it comes up; the bar fades in and
 * out. MIT.
 */
#include <GLES/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "gl_blit.h"
#include "gl_layer.h"
#include "ssr.h"
#include "ssr_button_icons.h"
#include "util.h"

int ssr_font_init(void);
float ssr_font_width(float px, const char *utf8, int bold);
float ssr_font_ascent(float px);
void ssr_font_draw(uint8_t *dst, int w, int h, int stride, float x, float y, float px, const char *utf8, int bold);
uint8_t *ssr_gfx_png(const void *data, int len, int *w, int *h); /* ssr_gfx.c */

extern const uint8_t ssr_res_icons[]; /* ssr_res.S */
extern const uint32_t ssr_res_icons_size;

/* ---------------------------------------------------------------- the words */
enum { W_START, W_SELECT, W_BACK, W_OK, W_RULES, W_INFO, W_STATS, W_SPACE, W_DELETE, W_DONE, W_KEYBOARD,
       W_ADJUST, W_CHANGE, W_SCROLL, W_RESUME, W_CONTINUE, W_SHOP, W_COUNT };

/* the game's languages (ssr_language): 0 en, 1 fr, 2 it, 3 de, 4 es, 5 ja, 6 en-US */
static const char *const k_words[6][W_COUNT] = {
    {"START", "SELECT", "BACK", "OK", "RULES", "INFO", "STATS", "SPACE", "DELETE", "DONE", "KEYBOARD", "ADJUST",
     "CHANGE", "SCROLL", "RESUME", "CONTINUE", "SHOP"},
    {"COMMENCER", "CHOISIR", "RETOUR", "OK", "R\xc3\x88GLES", "INFO", "STATS", "ESPACE", "EFFACER",
     "TERMIN\xc3\x89", "CLAVIER", "R\xc3\x89GLER", "CHANGER", "D\xc3\x89" "FILER", "REPRENDRE", "CONTINUER",
     "BOUTIQUE"},
    {"INIZIA", "SELEZIONA", "INDIETRO", "OK", "REGOLE", "INFO", "STATISTICHE", "SPAZIO", "CANCELLA", "FINE",
     "TASTIERA", "REGOLA", "CAMBIA", "SCORRI", "RIPRENDI", "CONTINUA", "NEGOZIO"},
    {"START", "AUSW\xc3\x84HLEN", "ZUR\xc3\x9c" "CK", "OK", "REGELN", "INFO", "WERTE", "LEERTASTE",
     "L\xc3\x96SCHEN", "FERTIG", "TASTATUR", "EINSTELLEN", "\xc3\x84NDERN", "BL\xc3\x84TTERN", "WEITER", "WEITER",
     "SHOP"},
    {"EMPEZAR", "ELEGIR", "ATR\xc3\x81S", "OK", "REGLAS", "INFO", "ESTAD\xc3\x8DSTICAS", "ESPACIO", "BORRAR",
     "LISTO", "TECLADO", "AJUSTAR", "CAMBIAR", "DESPLAZAR", "CONTINUAR", "CONTINUAR", "TIENDA"},
    {"\xe3\x82\xb9\xe3\x82\xbf\xe3\x83\xbc\xe3\x83\x88",                         /* スタート */
     "\xe9\x81\xb8\xe6\x8a\x9e",                                                 /* 選択 */
     "\xe3\x82\x82\xe3\x81\xa9\xe3\x82\x8b",                                     /* もどる */
     "\xe6\xb1\xba\xe5\xae\x9a",                                                 /* 決定 */
     "\xe3\x83\xab\xe3\x83\xbc\xe3\x83\xab",                                     /* ルール */
     "\xe6\x83\x85\xe5\xa0\xb1",                                                 /* 情報 */
     "\xe3\x82\xb9\xe3\x83\x86\xe3\x83\xbc\xe3\x82\xbf\xe3\x82\xb9",             /* ステータス */
     "\xe3\x82\xb9\xe3\x83\x9a\xe3\x83\xbc\xe3\x82\xb9",                         /* スペース */
     "\xe5\x89\x8a\xe9\x99\xa4",                                                 /* 削除 */
     "\xe5\xae\x8c\xe4\xba\x86",                                                 /* 完了 */
     "\xe3\x82\xad\xe3\x83\xbc\xe3\x83\x9c\xe3\x83\xbc\xe3\x83\x89",             /* キーボード */
     "\xe8\xaa\xbf\xe6\x95\xb4",                                                 /* 調整 */
     "\xe5\xa4\x89\xe6\x9b\xb4",                                                 /* 変更 */
     "\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xad\xe3\x83\xbc\xe3\x83\xab",             /* スクロール */
     "\xe5\x86\x8d\xe9\x96\x8b",                                                 /* 再開 */
     "\xe6\xac\xa1\xe3\x81\xb8",                                                 /* 次へ */
     "\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\x83\xe3\x83\x97"},                        /* ショップ */
};

/* ---------------------------------------------------------------- the sets */
typedef struct {
  int icon, word;
} Item;
#define MAXITEM 6
static const struct {
  Item it[MAXITEM];
  int n;
} k_sets[SSR_PROMPT_COUNT] = {
    [SSR_PROMPT_NONE] = {{{0, 0}}, 0},
    [SSR_PROMPT_TITLE] = {{{SSR_ICON_A, W_START}}, 1},
    [SSR_PROMPT_CAROUSEL] = {{{SSR_ICON_DPAD, W_SELECT}, {SSR_ICON_B, W_BACK}, {SSR_ICON_A, W_OK}}, 3},
    [SSR_PROMPT_CAROUSEL_RULES] = {{{SSR_ICON_DPAD, W_SELECT}, {SSR_ICON_X, W_RULES}, {SSR_ICON_B, W_BACK},
                                    {SSR_ICON_A, W_OK}}, 4},
    [SSR_PROMPT_CAROUSEL_INFO] = {{{SSR_ICON_DPAD, W_SELECT}, {SSR_ICON_X, W_INFO}, {SSR_ICON_B, W_BACK},
                                   {SSR_ICON_A, W_OK}}, 4},
    [SSR_PROMPT_CHARACTER] = {{{SSR_ICON_DPAD, W_SELECT}, {SSR_ICON_Y, W_STATS}, {SSR_ICON_B, W_BACK},
                               {SSR_ICON_A, W_OK}}, 4},
    [SSR_PROMPT_FOCUS] = {{{SSR_ICON_DPAD, W_SELECT}, {SSR_ICON_B, W_BACK}, {SSR_ICON_A, W_OK}}, 3},
    [SSR_PROMPT_FOCUS_NOBACK] = {{{SSR_ICON_DPAD, W_SELECT}, {SSR_ICON_A, W_OK}}, 2},
    [SSR_PROMPT_KEYBOARD] = {{{SSR_ICON_X, W_KEYBOARD}, {SSR_ICON_Y, W_SPACE}, {SSR_ICON_B, W_DELETE},
                              {SSR_ICON_PLUS, W_DONE}, {SSR_ICON_A, W_OK}}, 5},
    [SSR_PROMPT_SLIDERS] = {{{SSR_ICON_DPAD, W_ADJUST}, {SSR_ICON_B, W_BACK}}, 2},
    [SSR_PROMPT_RULES] = {{{SSR_ICON_DPAD, W_CHANGE}, {SSR_ICON_B, W_BACK}}, 2},
    [SSR_PROMPT_LIST] = {{{SSR_ICON_DPAD, W_SCROLL}, {SSR_ICON_B, W_BACK}}, 2},
    [SSR_PROMPT_PAUSE] = {{{SSR_ICON_DPAD, W_SELECT}, {SSR_ICON_B, W_RESUME}, {SSR_ICON_A, W_OK}}, 3},
    [SSR_PROMPT_YESNO] = {{{SSR_ICON_DPAD, W_SELECT}, {SSR_ICON_B, W_BACK}, {SSR_ICON_A, W_OK}}, 3},
    [SSR_PROMPT_OK] = {{{SSR_ICON_A, W_OK}}, 1},
    [SSR_PROMPT_CONTINUE] = {{{SSR_ICON_A, W_CONTINUE}}, 1},
    [SSR_PROMPT_SEGAMILES] = {{{SSR_ICON_X, W_SHOP}, {SSR_ICON_A, W_CONTINUE}}, 2},
    [SSR_PROMPT_LOBBY] = {{{SSR_ICON_DPAD, W_CHANGE}, {SSR_ICON_PLUS, W_START}, {SSR_ICON_B, W_BACK},
                           {SSR_ICON_A, W_OK}}, 4},
};

/* ---------------------------------------------------------------- GL */
static struct {
  void (*GenTextures)(GLsizei, GLuint *);
  void (*DeleteTextures)(GLsizei, const GLuint *);
  void (*BindTexture)(GLenum, GLuint);
  void (*TexParameteri)(GLenum, GLenum, GLint);
  void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
  void (*GetIntegerv)(GLenum, GLint *);
} G;
static int g_gl;

static uint8_t *g_icons; /* the row of pictures, RGBA */
static int g_icons_w, g_icons_h;

static int setup(void) {
  if (g_gl)
    return g_gl > 0;
  g_gl = -1;
#define L(n) (G.n = (void *)dcr_gl_lookup("gl" #n))
  L(GenTextures), L(DeleteTextures), L(BindTexture), L(TexParameteri), L(TexImage2D), L(GetIntegerv);
#undef L
  if (!G.GenTextures || !G.BindTexture || !G.TexImage2D || !G.GetIntegerv)
    return 0;
  g_icons = ssr_gfx_png(ssr_res_icons, (int)ssr_res_icons_size, &g_icons_w, &g_icons_h);
  if (!g_icons || g_icons_h != SSR_ICON_H) {
    debugPrintf("[prompt] the button pictures could not be read: no prompts\n");
    return 0;
  }
  g_gl = 1;
  return 1;
}

/* ---------------------------------------------------------------- the bars */
/* player 1's (the menus'), and split screen's player 2's (their column at the
 * racer select) */
typedef struct {
  int set, lang, h; /* what the texture holds */
  GLuint tex;
  int w_px, h_px;   /* its size */
  int want;         /* the set asked for this frame */
  float fade;
} Bar;
static Bar g_bar[2] = {{-1, -1, 0, 0, 0, 0, 0, 0.0f}, {-1, -1, 0, 0, 0, 0, 0, 0.0f}};
static int g_cols; /* the two columns: player 1's bar in the left half */

/* the icon's picture, scaled (bilinear), alpha-over into dst */
static void put_icon(uint8_t *dst, int dw, int dh, int icon, float x0, float y0, float hpx) {
  const float s = (float)SSR_ICON_H / hpx;
  const int iw = (int)ceilf(k_ssr_icons[icon].w / s), ih = (int)ceilf(hpx);
  for (int y = 0; y < ih; y++)
    for (int x = 0; x < iw; x++) {
      const int dx = (int)x0 + x, dy = (int)y0 + y;
      if (dx < 0 || dy < 0 || dx >= dw || dy >= dh)
        continue;
      float u = ((float)x + 0.5f) * s - 0.5f, v = ((float)y + 0.5f) * s - 0.5f;
      u = u < 0 ? 0 : u > k_ssr_icons[icon].w - 1 ? k_ssr_icons[icon].w - 1 : u;
      v = v < 0 ? 0 : v > SSR_ICON_H - 1 ? SSR_ICON_H - 1 : v;
      const int u0 = (int)u, v0 = (int)v, u1 = u0 + 1 < k_ssr_icons[icon].w ? u0 + 1 : u0,
                v1 = v0 + 1 < SSR_ICON_H ? v0 + 1 : v0;
      const float fu = u - u0, fv = v - v0;
      float c[4];
      for (int k = 0; k < 4; k++) {
        const uint8_t *r0 = g_icons + ((size_t)v0 * g_icons_w + k_ssr_icons[icon].x) * 4,
                      *r1 = g_icons + ((size_t)v1 * g_icons_w + k_ssr_icons[icon].x) * 4;
        c[k] = (r0[u0 * 4 + k] * (1 - fu) + r0[u1 * 4 + k] * fu) * (1 - fv) +
               (r1[u0 * 4 + k] * (1 - fu) + r1[u1 * 4 + k] * fu) * fv;
      }
      uint8_t *o = dst + ((size_t)dy * dw + dx) * 4;
      const float a = c[3] / 255.0f, oa = o[3] / 255.0f, na = a + oa * (1 - a);
      for (int k = 0; k < 3; k++)
        o[k] = na > 0 ? (uint8_t)((c[k] * a + o[k] * oa * (1 - a)) / na + 0.5f) : 0;
      o[3] = (uint8_t)(na * 255.0f + 0.5f);
    }
}

static void build(Bar *bar, int set, int lang, int screen_h) {
  const float k = (float)screen_h / 720.0f;
  const float icon_px = 36.0f * k, text_px = 25.0f * k, gap_in = 8.0f * k, gap_out = 28.0f * k;
  const char *const *words = k_words[lang == 6 ? 0 : lang >= 0 && lang < 6 ? lang : 0];
  float w = 0;
  for (int i = 0; i < k_sets[set].n; i++) {
    const Item *it = &k_sets[set].it[i];
    w += (float)k_ssr_icons[it->icon].w * icon_px / SSR_ICON_H + gap_in + ssr_font_width(text_px, words[it->word], 1);
    if (i)
      w += gap_out;
  }
  const int pad = (int)(6 * k) + 2;
  const int tw = (int)ceilf(w) + pad * 2, th = (int)ceilf(icon_px) + pad * 2;
  uint8_t *rgba = calloc((size_t)tw * th, 4), *cov = calloc((size_t)tw * th, 1), *sh = calloc((size_t)tw * th, 1);
  if (!rgba || !cov || !sh) {
    free(rgba), free(cov), free(sh);
    return;
  }
  /* the words' coverage, then a soft shadow from it */
  const float base = pad + icon_px * 0.5f + ssr_font_ascent(text_px) * 0.5f - text_px * 0.06f;
  float x = pad;
  float icon_x[MAXITEM];
  for (int i = 0; i < k_sets[set].n; i++) {
    const Item *it = &k_sets[set].it[i];
    if (i)
      x += gap_out;
    icon_x[i] = x;
    x += (float)k_ssr_icons[it->icon].w * icon_px / SSR_ICON_H + gap_in;
    ssr_font_draw(cov, tw, th, tw, x, base, text_px, words[it->word], 1);
    x += ssr_font_width(text_px, words[it->word], 1);
  }
  const int off = (int)(2 * k + 0.5f), rad = (int)(2 * k + 0.5f) + 1;
  for (int y = 0; y < th; y++)
    for (int xx = 0; xx < tw; xx++) {
      int m = 0;
      for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++) {
          const int sx = xx - off + dx, sy = y - off + dy;
          if (sx >= 0 && sy >= 0 && sx < tw && sy < th && cov[sy * tw + sx] > m)
            m = cov[sy * tw + sx];
        }
      sh[y * tw + xx] = (uint8_t)(m * 0.55f);
    }
  for (int i = 0; i < tw * th; i++) { /* white words over their dark shadow */
    const float a = cov[i] / 255.0f, s = sh[i] / 255.0f, na = a + s * (1 - a);
    const float c = na > 0 ? (255.0f * a + 10.0f * s * (1 - a)) / na : 0;
    rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = (uint8_t)c;
    rgba[i * 4 + 3] = (uint8_t)(na * 255.0f + 0.5f);
  }
  for (int i = 0; i < k_sets[set].n; i++)
    put_icon(rgba, tw, th, k_sets[set].it[i].icon, icon_x[i], (float)pad, icon_px);
  GLint bound = 0;
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  if (!bar->tex)
    G.GenTextures(1, &bar->tex);
  G.BindTexture(GL_TEXTURE_2D, bar->tex);
  if (G.TexParameteri) {
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
  free(rgba), free(cov), free(sh);
  bar->set = set, bar->lang = lang, bar->h = screen_h, bar->w_px = tw, bar->h_px = th;
}

/* ---------------------------------------------------------------- API */
void ssr_prompt_set(int set) { g_bar[0].want = set >= 0 && set < SSR_PROMPT_COUNT ? set : SSR_PROMPT_NONE; }
void ssr_prompt2_set(int set) { g_bar[1].want = set >= 0 && set < SSR_PROMPT_COUNT ? set : SSR_PROMPT_NONE; }
void ssr_prompt_columns(int on) { g_cols = on; }

static float g_y = 0.93f; /* the bar's centre, normalised (ssr_prompt_place) */
void ssr_prompt_place(float y) { g_y = y < 0.5f ? 0.5f : y > 0.96f ? 0.96f : y; }
static int g_top_half; /* player 1's in the top half (split screen's BATTLE: their copy's half) */
void ssr_prompt_top_half(int on) { g_top_half = on; }

/* ---------------------------------------------------------------- the banner */
/* A line of news at the top of the screen (split screen: "PLAYER 2 IS
 * JOINING..."): white words on a dark rounded band, faded in and out. */
static struct {
  char want[128], built[128];
  int h;
  GLuint tex;
  int w_px, h_px;
  float fade;
} N;

void ssr_prompt_banner(const char *utf8) { snprintf(N.want, sizeof N.want, "%s", utf8 ? utf8 : ""); }

static void banner_build(int screen_h) {
  const float k = (float)screen_h / 720.0f, px = 26.0f * k;
  const int padx = (int)(26 * k), pady = (int)(12 * k);
  const int tw = (int)ceilf(ssr_font_width(px, N.want, 1)) + padx * 2, th = (int)ceilf(px * 1.25f) + pady * 2;
  uint8_t *rgba = calloc((size_t)tw * th, 4), *cov = calloc((size_t)tw * th, 1);
  if (!rgba || !cov) {
    free(rgba), free(cov);
    return;
  }
  ssr_font_draw(cov, tw, th, tw, (float)padx, pady + px * 0.5f + ssr_font_ascent(px) * 0.5f + px * 0.08f, px, N.want,
                1);
  const float r = th * 0.5f; /* the band: a pill, dark blue, 80% */
  for (int y = 0; y < th; y++)
    for (int x = 0; x < tw; x++) {
      const float cx = x < r ? r : x > tw - r ? tw - r : (float)x, dx = x + 0.5f - cx, dy = y + 0.5f - r;
      const float d = sqrtf(dx * dx + dy * dy) - (r - 1.0f);
      const float band = (d < 0 ? 1.0f : d > 1 ? 0.0f : 1.0f - d) * 0.8f;
      const float a = cov[y * tw + x] / 255.0f, na = a + band * (1 - a);
      uint8_t *o = rgba + ((size_t)y * tw + x) * 4;
      o[0] = (uint8_t)(na > 0 ? (255 * a + 8 * band * (1 - a)) / na : 0);
      o[1] = (uint8_t)(na > 0 ? (255 * a + 16 * band * (1 - a)) / na : 0);
      o[2] = (uint8_t)(na > 0 ? (255 * a + 48 * band * (1 - a)) / na : 0);
      o[3] = (uint8_t)(na * 255 + 0.5f);
    }
  GLint bound = 0;
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  if (!N.tex)
    G.GenTextures(1, &N.tex);
  G.BindTexture(GL_TEXTURE_2D, N.tex);
  if (G.TexParameteri) {
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
  free(rgba), free(cov);
  snprintf(N.built, sizeof N.built, "%s", N.want);
  N.h = screen_h, N.w_px = tw, N.h_px = th;
}

static void banner_draw(int screen_w, int screen_h) {
  const float step = dcr_config()->frame_rate >= 60 ? 0.08f : 0.16f;
  if (N.want[0] && (strcmp(N.want, N.built) || N.h != screen_h)) {
    if (N.fade > 0.0f && N.built[0])
      N.fade -= step * 2.0f;
    else
      banner_build(screen_h), N.fade = 0.0f;
  } else {
    N.fade += N.want[0] ? step : -step;
  }
  N.fade = N.fade < 0 ? 0 : N.fade > 1 ? 1 : N.fade;
  if (N.fade > 0.0f && N.tex)
    dcr_blit_alpha(N.tex, (screen_w - N.w_px) / 2, (int)(screen_h * 0.035f), N.w_px, N.h_px, N.fade);
}

/* ---------------------------------------------------------------- lone buttons */
/* The button pictures as one texture (the row), for a button drawn beside
 * the game's own words (ssr_prompt_icon). */
static GLuint g_icons_tex;
static struct {
  int icon;
  float cx, cy, h, alpha;
} g_lone[4];
static int g_nlone;

void ssr_prompt_icon(int icon, float cx, float cy, float h, float alpha) {
  if (g_nlone < 4 && icon >= 0 && icon < SSR_ICON_COUNT && alpha > 0.0f)
    g_lone[g_nlone++] = (typeof(g_lone[0])){icon, cx, cy, h, alpha};
}

static void lone_draw(void) {
  if (g_nlone && !g_icons_tex) {
    GLint bound = 0;
    G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    G.GenTextures(1, &g_icons_tex);
    G.BindTexture(GL_TEXTURE_2D, g_icons_tex);
    if (G.TexParameteri) {
      G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_icons_w, g_icons_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_icons);
    G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
  }
  for (int i = 0; i < g_nlone; i++) {
    const int k = g_lone[i].icon;
    const float w = g_lone[i].h * (float)k_ssr_icons[k].w / SSR_ICON_H, h = g_lone[i].h;
    const float x = g_lone[i].cx - w * 0.5f, y = g_lone[i].cy - h * 0.5f;
    const float u0 = (float)k_ssr_icons[k].x / g_icons_w, u1 = (float)(k_ssr_icons[k].x + k_ssr_icons[k].w) / g_icons_w;
    const GLfloat pos[12] = {x, y, x + w, y, x, y + h, x + w, y, x + w, y + h, x, y + h};
    const GLfloat uv[12] = {u0, 0, u1, 0, u0, 1, u1, 0, u1, 1, u0, 1};
    const GLfloat col[4] = {1, 1, 1, g_lone[i].alpha};
    dcr_blit_mesh(g_icons_tex, pos, uv, 6, col);
  }
  g_nlone = 0;
}

/* ---------------------------------------------------------------- the cards */
/* Split screen's words in a player's column: yellow with a dark edge (the
 * console editions'), in the game's language. A line is "before|BUTTON|after"
 * (the button: A or -). */
static const char *const k_card[6][SSR_CARD_COUNT] = {
    {"PRESS|A|TO JOIN", "|-|CONNECT A CONTROLLER", "PLAYER 2...", "READY!", "P1", "P2"},
    {"APPUIE SUR|A|POUR JOUER", "|-|CONNECTER UNE MANETTE", "JOUEUR 2...", "PR\xc3\x8aT !", "J1", "J2"},
    {"PREMI|A|PER GIOCARE", "|-|COLLEGA UN CONTROLLER", "GIOCATORE 2...", "PRONTO!", "G1", "G2"},
    {"DR\xc3\x9c" "CKE|A|ZUM MITSPIELEN", "|-|CONTROLLER VERBINDEN", "SPIELER 2...", "BEREIT!", "S1", "S2"},
    {"PULSA|A|PARA UNIRTE", "|-|CONECTA UN MANDO", "JUGADOR 2...", "\xc2\xa1LISTO!", "J1", "J2"},
    {"|A|\xe3\x81\xa7\xe5\x8f\x82\xe5\x8a\xa0", "|-|\xe3\x82\xb3\xe3\x83\xb3\xe3\x83\x88\xe3\x83\xad\xe3\x83\xbc"
     "\xe3\x83\xa9\xe3\x83\xbc\xe3\x82\x92\xe6\x8e\xa5\xe7\xb6\x9a",
     "\xe3\x83\x97\xe3\x83\xac\xe3\x82\xa4\xe3\x83\xa4\xe3\x83\xbc" "2...", "OK!", "1P", "2P"},
};
static struct {
  GLuint tex[SSR_CARD_COUNT];
  int w[SSR_CARD_COUNT], h[SSR_CARD_COUNT], lang[SSR_CARD_COUNT], sh[SSR_CARD_COUNT];
  struct {
    int kind;
    float cx, cy;
  } req[6];
  int nreq;
} J = {.lang = {-1, -1, -1, -1, -1, -1}};

void ssr_prompt_card(int kind, float cx, float cy) {
  if (kind >= 0 && kind < SSR_CARD_COUNT && J.nreq < 6)
    J.req[J.nreq].kind = kind, J.req[J.nreq].cx = cx, J.req[J.nreq].cy = cy, J.nreq++;
}

static void card_build(int k, int lang, int screen_h) {
  char pre[64] = "", post[64] = "";
  int icon = -1;
  const char *line = k_card[lang == 6 ? 0 : lang >= 0 && lang < 6 ? lang : 0][k];
  const char *a = strchr(line, '|');
  if (a) {
    snprintf(pre, sizeof pre, "%.*s", (int)(a - line), line);
    icon = a[1] == 'A' ? SSR_ICON_A : SSR_ICON_MINUS;
    snprintf(post, sizeof post, "%s", strchr(a + 1, '|') + 1);
  } else {
    snprintf(pre, sizeof pre, "%s", line);
  }
  const int tag = k == SSR_CARD_P1 || k == SSR_CARD_P2; /* the progress bar's players */
  const float kk = (float)screen_h / 720.0f, px = (k == SSR_CARD_READY ? 46.0f : tag ? 22.0f : 34.0f) * kk,
              ih = 44.0f * kk,
              gap = 12.0f * kk;
  const int edge = (int)((tag ? 2 : 3) * kk + 0.5f) + 1, pad = edge + (int)(4 * kk) + 2;
  float w = ssr_font_width(px, pre, 1) + ssr_font_width(px, post, 1);
  if (icon >= 0)
    w += ih * k_ssr_icons[icon].w / SSR_ICON_H + (pre[0] ? gap : 0) + (post[0] ? gap : 0);
  const float line_h = icon >= 0 ? ih : px * 1.2f;
  const int tw = (int)ceilf(w) + pad * 2, th = (int)ceilf(line_h) + pad * 2;
  uint8_t *rgba = calloc((size_t)tw * th, 4), *cov = calloc((size_t)tw * th, 1), *dk = calloc((size_t)tw * th, 1);
  if (!rgba || !cov || !dk) {
    free(rgba), free(cov), free(dk);
    return;
  }
  const float base = pad + line_h * 0.5f + ssr_font_ascent(px) * 0.5f - px * 0.06f;
  float x = pad, icon_x = 0;
  ssr_font_draw(cov, tw, th, tw, x, base, px, pre, 1);
  x += ssr_font_width(px, pre, 1);
  if (icon >= 0) {
    x += pre[0] ? gap : 0;
    icon_x = x;
    x += ih * k_ssr_icons[icon].w / SSR_ICON_H + (post[0] ? gap : 0);
  }
  ssr_font_draw(cov, tw, th, tw, x, base, px, post, 1);
  for (int y = 0; y < th; y++) /* the dark edge: the words grown by `edge`, a little lower */
    for (int xx = 0; xx < tw; xx++) {
      int m = 0;
      for (int dy = -edge; dy <= edge; dy++)
        for (int dx = -edge; dx <= edge; dx++) {
          const int sx = xx + dx, sy = y + dy - 1;
          if (sx >= 0 && sy >= 0 && sx < tw && sy < th && dx * dx + dy * dy <= edge * edge + 1 &&
              cov[sy * tw + sx] > m)
            m = cov[sy * tw + sx];
        }
      dk[y * tw + xx] = (uint8_t)m;
    }
  /* yellow (the console's), over dark blue; the players' tags in their
   * controllers' colours (the controller screen's: Sonic blue, Knuckles red) */
  const float fr = k == SSR_CARD_P1 ? 80 : k == SSR_CARD_P2 ? 255 : 255;
  const float fg = k == SSR_CARD_P1 ? 160 : k == SSR_CARD_P2 ? 70 : 212;
  const float fb = k == SSR_CARD_P1 ? 255 : k == SSR_CARD_P2 ? 70 : 40;
  for (int i = 0; i < tw * th; i++) {
    const float a2 = cov[i] / 255.0f, e = dk[i] / 255.0f * 0.9f, na = a2 + e * (1 - a2);
    uint8_t *o = rgba + (size_t)i * 4;
    o[0] = (uint8_t)(na > 0 ? (fr * a2 + 10 * e * (1 - a2)) / na : 0);
    o[1] = (uint8_t)(na > 0 ? (fg * a2 + 18 * e * (1 - a2)) / na : 0);
    o[2] = (uint8_t)(na > 0 ? (fb * a2 + 60 * e * (1 - a2)) / na : 0);
    o[3] = (uint8_t)(na * 255 + 0.5f);
  }
  if (icon >= 0)
    put_icon(rgba, tw, th, icon, icon_x, (float)pad, ih);
  GLint bound = 0;
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  if (!J.tex[k])
    G.GenTextures(1, &J.tex[k]);
  G.BindTexture(GL_TEXTURE_2D, J.tex[k]);
  if (G.TexParameteri) {
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
  free(rgba), free(cov), free(dk);
  J.w[k] = tw, J.h[k] = th, J.lang[k] = lang, J.sh[k] = screen_h;
}

static void cards_draw(int screen_h) {
  const int lang = ssr_language();
  for (int i = 0; i < J.nreq; i++) {
    const int k = J.req[i].kind;
    if (!J.tex[k] || J.lang[k] != lang || J.sh[k] != screen_h)
      card_build(k, lang, screen_h);
    if (!J.tex[k])
      continue;
    /* the asking ones breathe, as the title's line does */
    const float t = (float)(armTicksToNs(armGetSystemTick()) % 1600000000ull) * 1e-9f / 1.6f;
    const float a = k == SSR_CARD_JOIN || k == SSR_CARD_JOINING ? 0.75f + 0.25f * cosf(t * 6.2831853f) : 1.0f;
    dcr_blit_alpha(J.tex[k], (int)(J.req[i].cx - J.w[k] * 0.5f), (int)(J.req[i].cy - J.h[k] * 0.5f), J.w[k], J.h[k],
                   a);
  }
  J.nreq = 0;
}

/* ssr_gfx.c, before each present */
void ssr_prompt_draw(int screen_w, int screen_h) {
  if (!setup())
    return;
  lone_draw();
  cards_draw(screen_h);
  banner_draw(screen_w, screen_h);
  if (!dcr_config()->prompts)
    return;
  const float step = dcr_config()->frame_rate >= 60 ? 0.12f : 0.24f;
  const int lang = ssr_language();
  if (!g_cols)
    g_bar[1].want = SSR_PROMPT_NONE;
  for (int k = 0; k < 2; k++) {
    Bar *b = &g_bar[k];
    if (b->want != SSR_PROMPT_NONE && (b->want != b->set || lang != b->lang || screen_h != b->h)) {
      if (b->fade > 0.0f && b->set >= 0 && b->set != SSR_PROMPT_NONE) {
        b->fade -= step * 1.5f; /* the old one out first */
      } else {
        build(b, b->want, lang, screen_h);
        b->fade = 0.0f;
      }
    } else if (b->want == SSR_PROMPT_NONE) {
      b->fade -= step;
    } else {
      b->fade += step;
    }
    b->fade = b->fade < 0 ? 0 : b->fade > 1 ? 1 : b->fade;
    if (b->fade <= 0.0f || !b->tex || b->set <= SSR_PROMPT_NONE)
      continue;
    /* bottom right, on the line of the screen's title (bottom left, "MAIN
     * MENU", which slides: ssr_menu.c follows it), the console editions'
     * place; without a footer, at the bottom. The racer select in two
     * columns: each player's at the bottom right of their half. */
    const float right = k == 0 && g_cols ? 0.47f : 0.97f;
    /* at the foot of the screen, in its corner, under the title's line (in
     * split screen's BATTLE, player 1's at the foot of their half) */
    const float foot = 1.0f - 0.55f * (float)b->h_px / (float)screen_h;
    const float at = k == 0 && g_top_half && !g_cols ? foot - 0.5f : foot;
    const int x = (int)(screen_w * right) - b->w_px, y = (int)(screen_h * at) - b->h_px / 2;
    dcr_blit_alpha(b->tex, x, y, b->w_px, b->h_px, b->fade);
  }
}
