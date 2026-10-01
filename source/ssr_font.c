/* ssr_font.c -- text drawn the way Android's Canvas drew it for the engine
 * (javaImageText*, ssr_text.c). From the Labyrinth 2 / Angry Birds Space
 * ports' TrueType reader (lab_font.c), without their emoji.
 *
 * THE FONT. Android drew this text in its default typeface (Droid Sans /
 * Roboto). The console's shared fonts (pl: Standard, the Chinese, Korean and
 * extension sets) are TrueType too and cover every language the game has
 * (English, French, Italian, German, Spanish, Japanese); a .ttf / .ttc / .otf
 * with "font" in its name in the game folder is used first, if the player
 * wants another look. This is a small reader for both: cmap formats 4 and 12,
 * TrueType collections, loca/glyf with simple and composite glyphs, hmtx for
 * advances (CFF outlines are not read: such a font is passed over). Outlines
 * (quadratic curves, flattened) are filled with the signed-area accumulation
 * method (each edge adds its exact coverage to the cells it crosses; a running
 * sum gives every pixel's coverage), which is anti-aliased without
 * supersampling. Glyphs are looked up in each font in turn. MIT.
 */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#include "ssr.h"
#include "util.h"

const char *dcr_game_root(void); /* dcr_path.c */

typedef struct {
  const uint8_t *d;
  uint32_t size;
  uint32_t dir; /* the table directory (a collection's face: not at 0) */
  uint32_t cmap, loca, glyf, hmtx;
  int upem, loca_long, nhm, nglyphs;
  int ascent, descent, gap;
  int cmap_fmt;
} Face;

#define MAX_FACES 6
static Face g_face[MAX_FACES];
static int g_nface;
static int g_state; /* 0 not tried, 1 ok, -1 none */
static Mutex g_font_lock;

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline int16_t bes16(const uint8_t *p) { return (int16_t)be16(p); }
static inline uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t table(const Face *f, const char *tag) {
  if (f->size < 12 || f->dir > f->size - 12)
    return 0;
  int n = be16(f->d + f->dir + 4);
  for (int i = 0; i < n; i++) {
    const uint8_t *e = f->d + f->dir + 12 + 16 * i;
    if (e + 16 > f->d + f->size)
      return 0;
    if (!memcmp(e, tag, 4)) {
      uint32_t off = be32(e + 8);
      return off < f->size ? off : 0;
    }
  }
  return 0;
}

static int face_open(Face *f, const uint8_t *d, uint32_t size, uint32_t dir) {
  memset(f, 0, sizeof *f);
  f->d = d;
  f->size = size;
  f->dir = dir;
  uint32_t head = table(f, "head"), hhea = table(f, "hhea"), maxp = table(f, "maxp");
  f->cmap = table(f, "cmap");
  f->loca = table(f, "loca");
  f->glyf = table(f, "glyf");
  f->hmtx = table(f, "hmtx");
  if (!head || !hhea || !maxp || !f->cmap || !f->loca || !f->glyf || !f->hmtx)
    return -1;
  f->upem = be16(d + head + 18);
  f->loca_long = bes16(d + head + 50);
  f->ascent = bes16(d + hhea + 4);
  f->descent = bes16(d + hhea + 6);
  f->gap = bes16(d + hhea + 8);
  f->nhm = be16(d + hhea + 34);
  f->nglyphs = be16(d + maxp + 4);
  /* the best Unicode subtable: (3,10) format 12, else (3,1)/(0,x) format 4 */
  int n = be16(d + f->cmap + 2);
  uint32_t best = 0;
  int best_fmt = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t *r = d + f->cmap + 4 + 8 * i;
    int plat = be16(r), enc = be16(r + 2);
    uint32_t off = f->cmap + be32(r + 4);
    if (off + 4 > size)
      continue;
    int fmt = be16(d + off);
    if (fmt == 12 && (plat == 3 || plat == 0) && best_fmt != 12)
      best = off, best_fmt = 12;
    else if (fmt == 4 && !best_fmt && (plat == 3 || plat == 0) && (enc == 1 || plat == 0))
      best = off, best_fmt = 4;
  }
  if (!best || f->upem <= 0)
    return -1;
  f->cmap = best;
  f->cmap_fmt = best_fmt;
  return 0;
}

static int glyph_index(const Face *f, uint32_t cp) {
  const uint8_t *t = f->d + f->cmap;
  if (f->cmap_fmt == 12) {
    uint32_t n = be32(t + 12);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      const uint8_t *g = t + 16 + 12 * mid;
      if (cp < be32(g))
        hi = mid;
      else if (cp > be32(g + 4))
        lo = mid + 1;
      else
        return (int)(be32(g + 8) + (cp - be32(g)));
    }
    return 0;
  }
  if (cp > 0xFFFF)
    return 0;
  int segx2 = be16(t + 6);
  const uint8_t *ends = t + 14, *starts = ends + segx2 + 2, *deltas = starts + segx2,
                *ranges = deltas + segx2;
  int lo = 0, hi = segx2 / 2;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (cp > be16(ends + 2 * mid))
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo >= segx2 / 2 || cp < be16(starts + 2 * lo))
    return 0;
  int ro = be16(ranges + 2 * lo);
  if (!ro)
    return (int)((cp + (uint32_t)be16(deltas + 2 * lo)) & 0xFFFF);
  const uint8_t *g = ranges + 2 * lo + ro + 2 * (cp - be16(starts + 2 * lo));
  if (g + 2 > f->d + f->size)
    return 0;
  int gi = be16(g);
  return gi ? (int)((gi + be16(deltas + 2 * lo)) & 0xFFFF) : 0;
}

static int advance(const Face *f, int g) {
  int i = g < f->nhm ? g : f->nhm - 1;
  return be16(f->d + f->hmtx + 4 * i);
}

static uint32_t glyph_off(const Face *f, int g, uint32_t *len) {
  if (g < 0 || g >= f->nglyphs)
    return 0;
  uint32_t a, b;
  if (f->loca_long) {
    a = be32(f->d + f->loca + 4 * g), b = be32(f->d + f->loca + 4 * g + 4);
  } else {
    a = 2u * be16(f->d + f->loca + 2 * g), b = 2u * be16(f->d + f->loca + 2 * g + 2);
  }
  *len = b > a ? b - a : 0;
  return f->glyf + a;
}

/* ------------------------------------------------------------ outlines */
typedef struct {
  float *acc;
  int w, h;
} Raster;

static void line(Raster *r, float x0, float y0, float x1, float y1) {
  if (y0 == y1)
    return;
  float dir = 1.0f;
  if (y0 > y1) {
    float t = x0; x0 = x1; x1 = t;
    t = y0; y0 = y1; y1 = t;
    dir = -1.0f;
  }
  const float dxdy = (x1 - x0) / (y1 - y0);
  float x = x0;
  if (y0 < 0.0f)
    x -= y0 * dxdy;
  int ys = y0 < 0 ? 0 : (int)y0, ye = (int)ceilf(y1);
  if (ye > r->h)
    ye = r->h;
  for (int y = ys; y < ye; y++) {
    float* row = r->acc + (size_t)y * r->w;
    float top = y0 > (float)y ? y0 : (float)y, bot = y1 < (float)(y + 1) ? y1 : (float)(y + 1);
    float dy = bot - top;
    float xn = x + dxdy * dy;
    float d = dy * dir;
    float xa = x < xn ? x : xn, xb = x < xn ? xn : x;
    if (xa < 0) xa = 0;
    if (xb < 0) xb = 0;
    if (xa > (float)(r->w - 1)) xa = (float)(r->w - 1);
    if (xb > (float)(r->w - 1)) xb = (float)(r->w - 1);
    float xaf = floorf(xa);
    int xai = (int)xaf;
    float xbc = ceilf(xb);
    int xbi = (int)xbc;
    if (xbi <= xai + 1) {
      float xm = 0.5f * (x + xn) - xaf;
      if (xm < 0) xm = 0;
      if (xm > 1) xm = 1;
      row[xai] += d - d * xm;
      if (xai + 1 < r->w)
        row[xai + 1] += d * xm;
    } else {
      float s = 1.0f / (xb - xa);
      float x0f = xa - xaf;
      float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
      float x1f = xb - xbc + 1.0f;
      float am = 0.5f * s * x1f * x1f;
      row[xai] += d * a0;
      if (xbi == xai + 2) {
        row[xai + 1] += d * (1.0f - a0 - am);
      } else {
        float a1 = s * (1.5f - x0f);
        row[xai + 1] += d * (a1 - a0);
        for (int xi = xai + 2; xi < xbi - 1; xi++)
          row[xi] += d * s;
        float a2 = a1 + (float)(xbi - xai - 3) * s;
        row[xbi - 1] += d * (1.0f - a2 - am);
      }
      if (xbi < r->w)
        row[xbi] += d * am;
    }
    x = xn;
  }
}

static void quad(Raster *r, float x0, float y0, float cx, float cy, float x1, float y1) {
  float dd = fabsf(x0 - 2 * cx + x1) + fabsf(y0 - 2 * cy + y1);
  int n = 1 + (int)sqrtf(dd * 4.0f);
  if (n > 24)
    n = 24;
  float px = x0, py = y0;
  for (int i = 1; i <= n; i++) {
    float t = (float)i / (float)n, u = 1.0f - t;
    float qx = u * u * x0 + 2 * u * t * cx + t * t * x1, qy = u * u * y0 + 2 * u * t * cy + t * t * y1;
    line(r, px, py, qx, qy);
    px = qx, py = qy;
  }
}

typedef struct {
  float a, b, c, d, e, f; /* x' = a x + c y + e, y' = b x + d y + f (font units) */
} Xf;

/* Draw glyph g of face f into r: font units -> pixels by `scale`, origin at
 * (ox, oy) pixels (the baseline), y flipped. */
static void draw_glyph(Raster *r, const Face *f, int g, float scale, float ox, float oy, Xf m, int depth) {
  uint32_t len = 0, off = glyph_off(f, g, &len);
  if (!len || off + 10 > f->size || depth > 4)
    return;
  const uint8_t *p = f->d + off;
  int nc = bes16(p);
  if (nc < 0) {
    /* composite */
    const uint8_t *c = p + 10, *end = f->d + off + len;
    for (;;) {
      if (c + 4 > end)
        return;
      uint16_t flags = be16(c), gi = be16(c + 2);
      c += 4;
      float dx, dy;
      if (flags & 1) {
        dx = (float)bes16(c), dy = (float)bes16(c + 2);
        c += 4;
      } else {
        dx = (float)(int8_t)c[0], dy = (float)(int8_t)c[1];
        c += 2;
      }
      float a = 1, b = 0, cc = 0, d = 1;
      if (flags & 8) {
        a = d = bes16(c) / 16384.0f;
        c += 2;
      } else if (flags & 0x40) {
        a = bes16(c) / 16384.0f, d = bes16(c + 2) / 16384.0f;
        c += 4;
      } else if (flags & 0x80) {
        a = bes16(c) / 16384.0f, b = bes16(c + 2) / 16384.0f, cc = bes16(c + 4) / 16384.0f,
        d = bes16(c + 6) / 16384.0f;
        c += 8;
      }
      if (!(flags & 2))
        dx = dy = 0; /* point matching: not supported, placed at the origin */
      Xf n = {m.a * a + m.c * b, m.b * a + m.d * b, m.a * cc + m.c * d, m.b * cc + m.d * d,
              m.a * dx + m.c * dy + m.e, m.b * dx + m.d * dy + m.f};
      draw_glyph(r, f, gi, scale, ox, oy, n, depth + 1);
      if (!(flags & 0x20))
        return;
    }
  }
  if (nc == 0)
    return;
  const uint8_t *ends = p + 10;
  int npts = be16(ends + 2 * (nc - 1)) + 1;
  const uint8_t *ins = ends + 2 * nc;
  const uint8_t *fl = ins + 2 + be16(ins);
  if (npts <= 0 || npts > 8192)
    return;
  uint8_t *flags = malloc((size_t)npts);
  float *xs = malloc(sizeof(float) * (size_t)npts * 2), *ys = xs + npts;
  if (!flags || !xs) {
    free(flags);
    free(xs);
    return;
  }
  const uint8_t *q = fl;
  for (int i = 0; i < npts;) {
    uint8_t v = *q++;
    flags[i++] = v;
    if (v & 8) {
      int rep = *q++;
      while (rep-- > 0 && i < npts)
        flags[i++] = v;
    }
  }
  int v = 0;
  for (int i = 0; i < npts; i++) {
    uint8_t fg = flags[i];
    if (fg & 2) {
      int dv = *q++;
      v += (fg & 16) ? dv : -dv;
    } else if (!(fg & 16)) {
      v += bes16(q);
      q += 2;
    }
    xs[i] = (float)v;
  }
  v = 0;
  for (int i = 0; i < npts; i++) {
    uint8_t fg = flags[i];
    if (fg & 4) {
      int dv = *q++;
      v += (fg & 32) ? dv : -dv;
    } else if (!(fg & 32)) {
      v += bes16(q);
      q += 2;
    }
    ys[i] = (float)v;
  }
  /* to pixels */
  for (int i = 0; i < npts; i++) {
    float fx = m.a * xs[i] + m.c * ys[i] + m.e, fy = m.b * xs[i] + m.d * ys[i] + m.f;
    xs[i] = ox + fx * scale;
    ys[i] = oy - fy * scale;
  }
  int start = 0;
  for (int c = 0; c < nc; c++) {
    int end = be16(ends + 2 * c);
    if (end < start || end >= npts)
      break;
    int n = end - start + 1;
    /* a starting on-curve point: the first on-curve one, or the midpoint of
     * the first two off-curve ones */
    int first = -1;
    for (int k = 0; k < n; k++)
      if (flags[start + k] & 1) {
        first = k;
        break;
      }
    float sx, sy;
    if (first < 0) {
      sx = 0.5f * (xs[start] + xs[start + (1 % n)]);
      sy = 0.5f * (ys[start] + ys[start + (1 % n)]);
      first = 0;
    } else {
      sx = xs[start + first], sy = ys[start + first];
    }
    float cx = sx, cy = sy, ctrlx = 0, ctrly = 0;
    int have_ctrl = 0;
    for (int k = 1; k <= n; k++) {
      int i = start + (first + k) % n;
      float px = xs[i], py = ys[i];
      if (flags[i] & 1) {
        if (have_ctrl)
          quad(r, cx, cy, ctrlx, ctrly, px, py);
        else
          line(r, cx, cy, px, py);
        cx = px, cy = py;
        have_ctrl = 0;
      } else {
        if (have_ctrl) {
          float mx = 0.5f * (ctrlx + px), my = 0.5f * (ctrly + py);
          quad(r, cx, cy, ctrlx, ctrly, mx, my);
          cx = mx, cy = my;
        }
        ctrlx = px, ctrly = py;
        have_ctrl = 1;
      }
    }
    if (have_ctrl)
      quad(r, cx, cy, ctrlx, ctrly, sx, sy);
    else
      line(r, cx, cy, sx, sy);
    start = end + 1;
  }
  free(flags);
  free(xs);
}

/* ------------------------------------------------------------- text */
static uint32_t utf8_next(const char **s) {
  const uint8_t *p = (const uint8_t *)*s;
  uint32_t c = *p++;
  if (c >= 0xF0 && p[0] && p[1] && p[2]) {
    c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u);
    p += 3;
  } else if (c >= 0xE0 && p[0] && p[1]) {
    c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u);
    p += 2;
  } else if (c >= 0xC0 && p[0]) {
    c = (c & 31) << 6 | (p[0] & 63u);
    p += 1;
  }
  *s = (const char *)p;
  return c;
}

/* Labyrinth's colour emoji are not part of this port: nothing matches. */
static int lab_emoji_match(const char *s, int *g, float *adv) {
  (void)s, (void)g, (void)adv;
  return 0;
}

/* An emoji no font here has (no emoji font on the card): nothing, rather
 * than the missing-glyph box. */
static int emoji_cp(uint32_t cp) {
  return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF);
}

/* ---------------------------------------------------- the player's font */
static Face g_user[2];       /* regular, bold */
static int g_have_user[2];
static uint8_t *g_user_file[4]; /* the files they are in (kept) */
static int g_nuser_file;

/* A name table string (id) of a face, as ASCII ('?' for anything else). */
static int face_name(const Face *f, int id, char *out, size_t cap) {
  uint32_t t = table(f, "name");
  out[0] = 0;
  if (!t || t + 6 > f->size)
    return 0;
  const uint8_t *n = f->d + t;
  int count = be16(n + 2);
  uint32_t strings = t + be16(n + 4);
  int best = -1, best_rank = 0;
  for (int i = 0; i < count; i++) {
    const uint8_t *r = n + 6 + 12 * i;
    if (r + 12 > f->d + f->size)
      break;
    int plat = be16(r), lang = be16(r + 4), nid = be16(r + 6);
    if (nid != id)
      continue;
    int rank = plat == 3 && lang == 0x409 ? 3 : plat == 3 ? 2 : plat == 1 && lang == 0 ? 1 : 0;
    if (rank > best_rank)
      best = i, best_rank = rank;
  }
  if (best < 0)
    return 0;
  const uint8_t *r = n + 6 + 12 * best;
  uint32_t len = be16(r + 8), off = strings + be16(r + 10);
  if (off + len > f->size)
    return 0;
  const uint8_t *s = f->d + off;
  size_t k = 0;
  int wide = be16(r) == 3 || be16(r) == 0;
  for (uint32_t i = 0; i < len && k + 1 < cap; i += wide ? 2 : 1) {
    unsigned c = wide ? (unsigned)be16(s + i) : s[i];
    out[k++] = c >= 32 && c < 127 ? (char)c : '?';
  }
  out[k] = 0;
  return (int)k;
}

static int has_word(const char *s, const char *w) {
  size_t n = strlen(w);
  for (; *s; s++)
    if (!strncasecmp(s, w, n))
      return 1;
  return 0;
}

/* Every face of one font file; the regular and bold ones kept (best by
 * PostScript name, then by style). */
static void user_file(const char *name, int *rank) {
  char path[320];
  snprintf(path, sizeof path, "%s/%s", dcr_game_root(), name);
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return;
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  uint8_t *d = len > 12 && len < (64L << 20) ? malloc((size_t)len) : NULL;
  if (!d || fread(d, 1, (size_t)len, fp) != (size_t)len) {
    fclose(fp);
    free(d);
    debugPrintf("[font] %s: could not be read\n", name);
    return;
  }
  fclose(fp);
  uint32_t dirs[32];
  int nd = 0;
  if (!memcmp(d, "ttcf", 4)) {
    uint32_t nf = be32(d + 8);
    for (uint32_t i = 0; i < nf && nd < 32 && 12 + 4 * i + 4 <= (uint32_t)len; i++)
      dirs[nd++] = be32(d + 12 + 4 * i);
  } else {
    dirs[nd++] = 0;
  }
  int used = 0, faces = 0;
  for (int i = 0; i < nd; i++) {
    Face f;
    if (face_open(&f, d, (uint32_t)len, dirs[i]) != 0)
      continue; /* not TrueType outlines (CFF), or broken */
    faces++;
    char ps[80], style[80];
    face_name(&f, 6, ps, sizeof ps);
    face_name(&f, 2, style, sizeof style);
    /* 3: the exact face; 2: Helvetica Neue's by style; 1: another family's */
    int is_bold = !strcasecmp(ps, "HelveticaNeue-Bold") || !strcasecmp(style, "Bold");
    int is_reg = !strcasecmp(ps, "HelveticaNeue") || !strcasecmp(style, "Regular") || !strcasecmp(style, "Roman");
    int k = is_bold ? 1 : is_reg ? 0 : -1;
    if (k < 0)
      continue;
    int r = !strcasecmp(ps, k ? "HelveticaNeue-Bold" : "HelveticaNeue") ? 3 : has_word(ps, "HelveticaNeue") ? 2 : 1;
    if (r > rank[k]) {
      rank[k] = r;
      g_user[k] = f;
      g_have_user[k] = 1;
      used = 1;
      debugPrintf("[font] %s: %s face %s (%s)\n", name, k ? "bold" : "regular", ps[0] ? ps : "?", style);
    }
  }
  if (used && g_nuser_file < 4)
    g_user_file[g_nuser_file++] = d; /* its faces point into it */
  else {
    free(d);
    if (!faces)
      debugPrintf("[font] %s: no TrueType face in it (CFF outlines are not read)\n", name);
  }
}

static void user_fonts(void) {
  DIR *dir = opendir(dcr_game_root());
  if (!dir)
    return;
  int rank[2] = {0, 0};
  struct dirent *e;
  while ((e = readdir(dir))) {
    size_t n = strlen(e->d_name);
    if (n < 5 || !has_word(e->d_name, "font"))
      continue;
    const char *ext = e->d_name + n - 4;
    if (strcasecmp(ext, ".ttc") && strcasecmp(ext, ".ttf") && strcasecmp(ext, ".otf"))
      continue;
    user_file(e->d_name, rank);
  }
  closedir(dir);
  if (!g_have_user[0] && g_have_user[1]) {
    g_user[0] = g_user[1]; /* only a bold face: it is the regular one too */
    g_have_user[0] = 1;
  }
}

/* The face for code point cp (glyph index out): the player's font (its bold
 * face for bold text), then the console's; *fake: bold drawn by widening
 * (a face without a bold style, as Android does). */
static const Face *face_for(uint32_t cp, int bold, int *g, int *fake) {
  *fake = 0;
  if (bold && g_have_user[1] && (*g = glyph_index(&g_user[1], cp)))
    return &g_user[1];
  if (g_have_user[0] && (*g = glyph_index(&g_user[0], cp))) {
    *fake = bold; /* no bold face, or not this character in it */
    return &g_user[0];
  }
  for (int i = 0; i < g_nface; i++) {
    int gi = glyph_index(&g_face[i], cp);
    if (gi) {
      *g = gi;
      *fake = bold;
      return &g_face[i];
    }
  }
  *g = 0;
  *fake = bold;
  return g_have_user[0] ? &g_user[0] : g_nface ? &g_face[0] : NULL;
}

/* the face the line's metrics come from */
static const Face *main_face(void) { return g_have_user[0] ? &g_user[0] : &g_face[0]; }

int ssr_font_init(void) {
  mutexLock(&g_font_lock);
  if (g_state) {
    mutexUnlock(&g_font_lock);
    return g_state > 0 ? 0 : -1;
  }
  g_state = -1;
  user_fonts();
  if (R_FAILED(plInitialize(PlServiceType_User))) {
    debugPrintf("[font] the shared font service is unavailable\n");
  } else {
    static const PlSharedFontType types[] = {PlSharedFontType_Standard, PlSharedFontType_NintendoExt,
                                             PlSharedFontType_ChineseSimplified,
                                             PlSharedFontType_ExtChineseSimplified,
                                             PlSharedFontType_ChineseTraditional, PlSharedFontType_KO};
    for (unsigned i = 0; i < sizeof types / sizeof types[0] && g_nface < MAX_FACES; i++) {
      PlFontData fd;
      if (R_SUCCEEDED(plGetSharedFontByType(&fd, types[i])) && fd.address && fd.size &&
          face_open(&g_face[g_nface], fd.address, fd.size, 0) == 0)
        g_nface++;
    }
  }
  int ok = g_nface || g_have_user[0];
  g_state = ok ? 1 : -1;
  mutexUnlock(&g_font_lock);
  debugPrintf("[font] %s%s; %d of the console's font(s) for the rest\n",
              g_have_user[0] ? "the SD card's font" : "the console's font (a .ttf with \"font\" in its name in switch/sonic_allstars_nx replaces it)",
              g_have_user[0] ? (g_have_user[1] ? ", regular and bold" : ", regular (bold made from it)") : "", g_nface);
  return ok ? 0 : -1;
}

int ssr_font_ready(void) { return g_state > 0; }

/* Bold: the outline drawn twice, the second time this far to the right
 * (Android's fake bold for a face without a bold style widens the same way). */
static float bold_shift(float px) { return px < 20.0f ? 1.0f : px / 20.0f; }

float ssr_font_width(float px, const char *utf8, int bold) {
  if (!utf8)
    return 0;
  if (ssr_font_init() != 0)
    return (float)strlen(utf8) * px * 0.55f;
  float x = 0;
  int fake = 0;
  for (const char *s = utf8; *s;) {
    int eg;
    float eadv;
    int en = lab_emoji_match(s, &eg, &eadv);
    if (en) {
      x += eadv * px;
      s += en;
      continue;
    }
    int g, fk;
    uint32_t cp = utf8_next(&s);
    const Face *f = face_for(cp, bold, &g, &fk);
    if (!g && emoji_cp(cp))
      continue;
    x += (float)advance(f, g) * px / (float)f->upem;
    fake |= fk;
  }
  return x + (fake ? bold_shift(px) : 0.0f);
}

float ssr_font_ascent(float px) {
  if (ssr_font_init() != 0)
    return px * 0.8f;
  const Face *f = main_face();
  return (float)f->ascent * px / (float)f->upem;
}

float ssr_font_descent(float px) {
  if (ssr_font_init() != 0)
    return px * 0.2f;
  const Face *f = main_face();
  return (float)-f->descent * px / (float)f->upem;
}

/* The text's coverage, max-combined into dst (w x h, stride bytes a row):
 * baseline at y, starting at x. */
void ssr_font_draw(uint8_t *dst, int w, int h, int stride, float x, float y, float px,
                   const char *utf8, int bold) {
  if (!utf8 || !*utf8 || ssr_font_init() != 0)
    return;
  float asc = ssr_font_ascent(px), desc = ssr_font_descent(px);
  int top = (int)floorf(y - asc) - 1, bottom = (int)ceilf(y + desc) + 2;
  int left = (int)floorf(x) - 2, right = (int)ceilf(x + ssr_font_width(px, utf8, bold)) + 2;
  if (top < 0)
    top = 0;
  if (bottom > h)
    bottom = h;
  if (left < 0)
    left = 0;
  if (right > w)
    right = w;
  int rw = right - left, rh = bottom - top;
  if (rw <= 0 || rh <= 0)
    return;
  Raster r = {calloc((size_t)rw * rh + 4, sizeof(float)), rw, rh};
  if (!r.acc)
    return;
  const Xf id = {1, 0, 0, 1, 0, 0};
  /* pass 0: the glyphs; pass 1: again, a little to the right, the ones in a
   * face without a bold style (none with the SD card's bold face) */
  int any_fake = 0;
  for (int pass = 0; pass < 2; pass++) {
    if (pass && !any_fake)
      break;
    float cx = x - (float)left + (pass ? bold_shift(px) : 0.0f);
    if (pass) {
      /* the second stroke on its own raster, then max-combined below */
      memset(r.acc, 0, sizeof(float) * (size_t)rw * rh);
    }
    for (const char *s = utf8; *s;) {
      int eg;
      float eadv;
      int en = lab_emoji_match(s, &eg, &eadv);
      if (en) { /* its picture goes there (or nothing) */
        cx += eadv * px;
        s += en;
        continue;
      }
      int g, fk;
      uint32_t cp = utf8_next(&s);
      const Face *f = face_for(cp, bold, &g, &fk);
      if (!g && emoji_cp(cp))
        continue;
      float scale = px / (float)f->upem;
      any_fake |= fk;
      if (!pass || fk)
        draw_glyph(&r, f, g, scale, cx, y - (float)top, id, 0);
      cx += (float)advance(f, g) * scale;
    }
    float acc = 0;
    for (int i = 0; i < rw * rh; i++) {
      if (i % rw == 0)
        acc = 0;
      acc += r.acc[i];
      float a = fabsf(acc);
      int v = a >= 1.0f ? 255 : (int)(a * 255.0f + 0.5f);
      uint8_t *d = dst + (size_t)(top + i / rw) * (size_t)stride + (size_t)(left + i % rw);
      if (v > *d)
        *d = (uint8_t)v;
    }
  }
  free(r.acc);
}
