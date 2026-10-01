/* ssr_battlecard.c -- split screen's BATTLE and VS RACE cards: the mode
 * select's TIME TRIAL and MISSIONS cards (split screen has neither: its third
 * card is BATTLE, its fourth VS RACE, ssr_split_net.c) with the game's own
 * art for them -- the LOCAL menu's HOST BATTLE and HOST RACE cards (two
 * phones, two rockets; two phones, a traffic light: netmenu_cards.star, its
 * HD texture pages 1 and 3), read from the player's own game data (the D0
 * pack, ssr_pack.h; ASSET_FORMATS.md 5), once, and kept in data/.
 *
 * Each goes into its card's sprite's place in its texture page (SoloMenu's
 * card scene 0x131fb476, sprites 0x014dd0ea and 0x1c5d3226: its page table
 * at 0x218b20) while split screen is on; what was there is read back first
 * (the page as a framebuffer's picture) and put back when split screen ends,
 * so a single player's TIME TRIAL and MISSIONS are as they were. MIT.
 */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz/miniz.h>

#include "gl_layer.h"
#include "ssr.h"
#include "ssr_pack.h"
#include "util.h"

const char *dcr_game_root(void); /* dcr_path.c */

#define STAR_NAME "netmenu_cards.star"
#define SC_SOLO_CARDS 0x131fb476u

/* the two cards: the Solo menu's sprite, the art's page in the pack's
 * netmenu_cards (HD), its copy in data/ */
typedef struct {
  const char *what, *was; /* (the log's) */
  uint32_t sprite;
  int page;
  const char *cache;
} CardDef;
static const CardDef k_card[2] = {
    {"BATTLE", "TIME TRIAL", 0x014dd0eau, 1, "port_battle_card2.rgba"},
    {"VS RACE", "MISSIONS", 0x1c5d3226u, 3, "port_race_card.rgba"},
};

/* ------------------------------------------------------------ the art, from the pack */
typedef struct {
  int tried;
  uint8_t *px; /* the card, RGBA, cropped to its outline */
  int w, h;
} Art;
static Art g_art[2];

static uint8_t *read_at(FILE *f, uint64_t off, size_t n) {
  uint8_t *b = malloc(n ? n : 1);
  if (b && ssr_read_at(f, off, b, n) != 0) {
    free(b);
    b = NULL;
  }
  return b;
}

/* the pack's entry `name` (its data's place in the file); 0 if not there */
static int pack_find(FILE *f, uint64_t pack, const char *name, uint64_t *off, uint32_t *len) {
  uint8_t hdr[8];
  if (ssr_read_at(f, pack, hdr, 8) != 0 || hdr[0] != 'D' || hdr[1] != '0')
    return 0;
  const uint32_t hlen = ssr_rd32(hdr + 2), count = ssr_rd16(hdr + 6);
  if (hlen < 8 || hlen > (4u << 20))
    return 0;
  uint8_t *t = read_at(f, pack + 8, hlen - 8);
  if (!t)
    return 0;
  uint64_t data = pack + hlen;
  size_t p = 0;
  const size_t nl = strlen(name);
  int found = 0;
  for (uint32_t i = 0; i < count && p < hlen - 8; i++) {
    const char *n = (const char *)t + p;
    const size_t l = strnlen(n, hlen - 8 - p);
    if (p + l + 9 > hlen - 8)
      break;
    const uint32_t size = ssr_rd32(t + p + l + 5) & 0x7fffffffu;
    if (l >= nl && !strcasecmp(n + l - nl, name)) {
      *off = data, *len = size;
      found = 1;
      break;
    }
    data += size;
    p += l + 9;
  }
  free(t);
  return found;
}

/* a STAr region, unpacked (gzip ones inflated); its length in *n */
static uint8_t *star_region(const uint8_t *s, size_t slen, int r, size_t *n) {
  const int nb = s[6];
  if (r < 0 || r + 1 >= nb + 3 || 8 + 4 * (size_t)(nb + 3) > slen)
    return NULL;
  const uint32_t a = ssr_rd32(s + 8 + 4 * r), b = ssr_rd32(s + 8 + 4 * (r + 1));
  const uint32_t lo = a & 0x7fffffffu, hi = b & 0x7fffffffu;
  if (hi <= lo || hi > slen)
    return NULL;
  if (!(a & 0x80000000u)) {
    uint8_t *o = malloc(hi - lo);
    if (o)
      memcpy(o, s + lo, hi - lo), *n = hi - lo;
    return o;
  }
  /* u32 size, u8 0, then a gzip member (a 10-byte header, no extra fields) */
  if (hi - lo < 5 + 18)
    return NULL;
  const uint8_t *g = s + lo + 5;
  if (g[0] != 0x1f || g[1] != 0x8b || g[2] != 8)
    return NULL;
  size_t out = 0;
  uint8_t *o = tinfl_decompress_mem_to_heap(g + 10, hi - lo - 5 - 10 - 8, &out, 0);
  if (o)
    *n = out;
  return o;
}

/* a card: the HD variant's texture block, its page `want`, its top card */
static uint8_t *decode_card(const uint8_t *s, size_t slen, int want, int *w_out, int *h_out) {
  if (slen < 16 || memcmp(s, "STAr", 4) != 0)
    return NULL;
  const int slots = s[5];
  size_t n = 0;
  uint8_t *map = star_region(s, slen, 0, &n);
  if (!map || n < (size_t)2 * slots * 3) {
    free(map);
    return NULL;
  }
  const int tex_block = map[(1 * slots + 0) * 3 + 2]; /* variant 1 (HD), its scene's */
  free(map);
  uint8_t *g = tex_block ? star_region(s, slen, tex_block + 1, &n) : NULL;
  if (!g || n < 8 || memcmp(g, "GPUC", 4) != 0) {
    free(g);
    return NULL;
  }
  const uint32_t count = ssr_rd32(g + 4);
  size_t off = 8 + 24 * (size_t)count;
  uint8_t *card = NULL;
  for (uint32_t i = 0; i < count && off <= n; i++) {
    const uint8_t *e = g + 8 + 24 * i;
    const int bpp = e[5], pw = (int)ssr_rd16(e + 12), ph = (int)ssr_rd16(e + 14);
    const uint32_t size = ssr_rd32(e + 16);
    if ((int)i == want && bpp == 32 && e[4] == 1 && off + size <= n && (size_t)pw * ph * 4 == size) {
      /* the top card: its outline (alpha), from its first row down to the
       * row before the gap under it (the next card's edge not with it) */
      const uint8_t *px = g + off;
      int x0 = pw, y0 = -1, x1 = -1, y1 = -1;
      for (int y = 0; y < ph; y++) {
        int any = 0;
        for (int x = 0; x < pw; x++)
          if (px[((size_t)y * pw + x) * 4 + 3] > 24) {
            any = 1;
            x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
          }
        if (any && y0 < 0)
          y0 = y;
        if (any)
          y1 = y;
        else if (y0 >= 0)
          break;
      }
      if (x1 > x0 && y1 > y0) {
        const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
        card = malloc((size_t)cw * ch * 4);
        for (int y = 0; card && y < ch; y++)
          memcpy(card + (size_t)y * cw * 4, px + ((size_t)(y0 + y) * pw + x0) * 4, (size_t)cw * 4);
        *w_out = cw, *h_out = ch;
      }
      break;
    }
    off += size;
  }
  free(g);
  return card;
}

static void cache_path(int c, char *out, size_t n) { snprintf(out, n, "%s/data/%s", dcr_game_root(), k_card[c].cache); }

static int art_load(int c) {
  Art *A = &g_art[c];
  if (A->tried)
    return A->px != NULL;
  A->tried = 1;
  char path[600];
  cache_path(c, path, sizeof path);
  FILE *f = fopen(path, "rb");
  if (f) {
    uint32_t wh[2];
    if (fread(wh, 4, 2, f) == 2 && wh[0] && wh[1] && wh[0] <= 1024 && wh[1] <= 1024) {
      A->px = malloc((size_t)wh[0] * wh[1] * 4);
      if (A->px && fread(A->px, 4, (size_t)wh[0] * wh[1], f) == (size_t)wh[0] * wh[1])
        A->w = (int)wh[0], A->h = (int)wh[1];
      else
        free(A->px), A->px = NULL;
    }
    fclose(f);
    if (A->px)
      return 1;
  }
  const SsrPack *pk = ssr_pack();
  FILE *pf = pk ? fopen(pk->path, "rb") : NULL;
  uint64_t off = 0;
  uint32_t len = 0;
  if (!pf || !pack_find(pf, pk->pack_off, STAR_NAME, &off, &len) || len < 16 || len > (64u << 20)) {
    debugPrintf("[battlecard] %s: not found in the game's data\n", STAR_NAME);
    if (pf)
      fclose(pf);
    return 0;
  }
  const u64 t0 = armGetSystemTick();
  uint8_t *star = read_at(pf, off, len);
  fclose(pf);
  if (star)
    A->px = decode_card(star, len, k_card[c].page, &A->w, &A->h);
  free(star);
  if (!A->px) {
    debugPrintf("[battlecard] %s: its %s card could not be read\n", STAR_NAME, k_card[c].what);
    return 0;
  }
  f = fopen(path, "wb");
  if (f) {
    const uint32_t wh[2] = {(uint32_t)A->w, (uint32_t)A->h};
    fwrite(wh, 4, 2, f);
    fwrite(A->px, 4, (size_t)A->w * A->h, f);
    fclose(f);
  }
  debugPrintf("[battlecard] the LOCAL menu's %s card (%dx%d) read in %llu ms, kept in data/\n", k_card[c].what, A->w,
              A->h, (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
  return 1;
}

/* the card scaled to w x h (bilinear) */
static uint8_t *art_fit(const Art *A, int w, int h) {
  uint8_t *o = malloc((size_t)w * h * 4);
  if (!o)
    return NULL;
  for (int y = 0; y < h; y++) {
    const float sy = ((float)y + 0.5f) * (float)A->h / (float)h - 0.5f;
    const int y0 = sy < 0 ? 0 : (int)sy, y1 = y0 + 1 < A->h ? y0 + 1 : A->h - 1;
    const float fy = sy < 0 ? 0 : sy - (float)y0;
    for (int x = 0; x < w; x++) {
      const float sx = ((float)x + 0.5f) * (float)A->w / (float)w - 0.5f;
      const int x0 = sx < 0 ? 0 : (int)sx, x1 = x0 + 1 < A->w ? x0 + 1 : A->w - 1;
      const float fx = sx < 0 ? 0 : sx - (float)x0;
      for (int c = 0; c < 4; c++) {
        const float a = A->px[((size_t)y0 * A->w + x0) * 4 + c], b = A->px[((size_t)y0 * A->w + x1) * 4 + c];
        const float d = A->px[((size_t)y1 * A->w + x0) * 4 + c], e = A->px[((size_t)y1 * A->w + x1) * 4 + c];
        const float v = (a + (b - a) * fx) * (1.0f - fy) + (d + (e - d) * fx) * fy;
        o[((size_t)y * w + x) * 4 + c] = (uint8_t)(v + 0.5f);
      }
    }
  }
  return o;
}

/* ------------------------------------------------------------ into the page, and back */
typedef struct {
  const void *page; /* the page it went into (NULL: none) */
  uint32_t gl;      /* its texture (the engine's number) */
  int x, y, w, h;
  uint8_t *orig;    /* what was there */
  int fails;
} Placed;
static Placed g_placed[2];

/* the engine's own texture calls (DDGLRefresh: its numbers are its own) */
static void page_put(uint32_t gl, int x, int y, int w, int h, const uint8_t *px) {
  void (*bind)(GLenum, GLuint) = (void (*)(GLenum, GLuint))ssr_native("_ZN11DDGLRefresh15ddGLBindTextureEjj");
  void (*sub)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *) =
      (void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))ssr_native(
          "_ZN11DDGLRefresh17ddGLTexSubImage2DEjiiiiijjPKv");
  void (*store)(GLenum, GLint) = (void (*)(GLenum, GLint))dcr_gl_lookup("glPixelStorei");
  const uint32_t *cur = (const uint32_t *)ssr_native("_ZN4GLES17ms_currentTextureE");
  if (!bind || !sub)
    return;
  bind(GL_TEXTURE_2D, gl);
  if (store)
    store(GL_UNPACK_ALIGNMENT, 4);
  sub(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  bind(GL_TEXTURE_2D, cur ? *cur : 0);
}

/* what the page has at the rectangle (read back: the page as a framebuffer's
 * colour); NULL if it cannot be */
static uint8_t *page_get(uint32_t gl, int x, int y, int w, int h) {
  const uint8_t *const *tex = (const uint8_t *const *)ssr_native("_ZN11DDGLRefresh9sTexturesE");
  void (*gen)(GLsizei, GLuint *) = (void (*)(GLsizei, GLuint *))dcr_gl_lookup("glGenFramebuffersOES");
  void (*del)(GLsizei, const GLuint *) = (void (*)(GLsizei, const GLuint *))dcr_gl_lookup("glDeleteFramebuffersOES");
  void (*bindfb)(GLenum, GLuint) = (void (*)(GLenum, GLuint))dcr_gl_lookup("glBindFramebufferOES");
  void (*attach)(GLenum, GLenum, GLenum, GLuint, GLint) =
      (void (*)(GLenum, GLenum, GLenum, GLuint, GLint))dcr_gl_lookup("glFramebufferTexture2DOES");
  GLenum (*status)(GLenum) = (GLenum(*)(GLenum))dcr_gl_lookup("glCheckFramebufferStatusOES");
  void (*read)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) =
      (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))dcr_gl_lookup("glReadPixels");
  void (*geti)(GLenum, GLint *) = (void (*)(GLenum, GLint *))dcr_gl_lookup("glGetIntegerv");
  void (*store)(GLenum, GLint) = (void (*)(GLenum, GLint))dcr_gl_lookup("glPixelStorei");
  if (!tex || !*tex || !gl || !gen || !del || !bindfb || !attach || !status || !read || !geti)
    return NULL;
  const GLuint real = *(const uint32_t *)(*tex + (gl - 1) * 0x78 + 8);
  GLint was = 0;
  geti(GL_FRAMEBUFFER_BINDING_OES, &was);
  GLuint fb = 0;
  gen(1, &fb);
  bindfb(GL_FRAMEBUFFER_OES, fb);
  attach(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, real, 0);
  uint8_t *px = NULL;
  if (status(GL_FRAMEBUFFER_OES) == GL_FRAMEBUFFER_COMPLETE_OES) {
    px = malloc((size_t)w * h * 4);
    if (px) {
      if (store)
        store(GL_PACK_ALIGNMENT, 4);
      read(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
  }
  bindfb(GL_FRAMEBUFFER_OES, (GLuint)was);
  del(1, &fb);
  return px;
}

/* a sprite's texture and its place (its UVs' bounds) */
static const uint8_t *sprite_place(void *loadset, uint32_t sprite, uint32_t *gl, int *x, int *y, int *w, int *h) {
  uint8_t *(*get_tex)(void *mgr, uint32_t id) =
      (uint8_t * (*)(void *, uint32_t)) ssr_native("_ZNK18IPODTextureManager10GetTextureEj");
  uint8_t *tex = get_tex && loadset ? get_tex((uint8_t *)loadset + 0x4c, sprite) : NULL;
  const uint8_t *page = tex ? *(uint8_t *const *)(tex + 0x24) : NULL;
  if (!page)
    return NULL;
  *gl = *(const uint32_t *)page;
  const int pw = *(const int32_t *)(page + 4), ph = *(const int32_t *)(page + 8);
  float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
  for (int i = 0; i < 4; i++) {
    const float u = ((const float *)(tex + 4))[i * 2], v = ((const float *)(tex + 4))[i * 2 + 1];
    u0 = fminf(u0, u), u1 = fmaxf(u1, u), v0 = fminf(v0, v), v1 = fmaxf(v1, v);
  }
  *x = (int)((float)pw * u0), *y = (int)((float)ph * v0);
  *w = (int)lrintf((u1 - u0) * (float)pw), *h = (int)lrintf((v1 - v0) * (float)ph);
  if (!*gl || *w <= 8 || *h <= 8 || *x + *w > pw || *y + *h > ph)
    return NULL;
  return page;
}

/* the page was uploaded again (ssr_xcard.c's hook): the engine's picture */
void ssr_battlecard_page_uploaded(const void *page) {
  for (int c = 0; c < 2; c++) {
    Placed *P = &g_placed[c];
    if (page && page == P->page) {
      free(P->orig);
      P->orig = NULL, P->page = NULL;
    }
  }
}

/* card c: on, its art in its sprite's place; else the sprite put back (if
 * its page is the one it went into -- a page loaded again has its own) */
static void card_frame(int c, void *loadset, int on) {
  Placed *P = &g_placed[c];
  uint32_t gl;
  int x, y, w, h;
  const uint8_t *page = sprite_place(loadset, k_card[c].sprite, &gl, &x, &y, &w, &h);
  if (!on) {
    if (P->page && P->orig && page == P->page && gl == P->gl) {
      page_put(P->gl, P->x, P->y, P->w, P->h, P->orig);
      debugPrintf("[battlecard] the %s card as it was\n", k_card[c].was);
    }
    free(P->orig);
    P->orig = NULL, P->page = NULL;
    return;
  }
  if (P->page && (page != P->page || gl != P->gl)) { /* another page now: its own picture */
    free(P->orig);
    P->orig = NULL, P->page = NULL;
  }
  if (P->page || !page || P->fails > 3 || !art_load(c))
    return;
  uint8_t *orig = page_get(gl, x, y, w, h);
  if (!orig) {
    if (P->fails++ == 0)
      debugPrintf("[battlecard] the %s card's page cannot be read back: left as it is\n", k_card[c].was);
    return;
  }
  uint8_t *px = art_fit(&g_art[c], w, h);
  if (!px) {
    free(orig);
    return;
  }
  page_put(gl, x, y, w, h, px);
  free(px);
  P->page = page, P->gl = gl, P->x = x, P->y = y, P->w = w, P->h = h, P->orig = orig;
  debugPrintf("[battlecard] the mode select's %s card: %s's art (%dx%d at %d,%d of texture %u)\n", k_card[c].was,
              k_card[c].what, w, h, x, y, (unsigned)gl);
}

/* each frame the mode select is player 1's copy's screen: its card scene's
 * load set; on: split screen's (BATTLE, VS RACE), else a single player's
 * (TIME TRIAL, MISSIONS) */
void ssr_battlecard_frame(void *loadset, int on) {
  if (!loadset)
    return;
  for (int c = 0; c < 2; c++)
    card_frame(c, loadset, on);
}
