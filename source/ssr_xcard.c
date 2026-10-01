/* ssr_xcard.c -- the main menu's SPLIT SCREEN card, from the player's own
 * Xbox 360 disc (ssr_split.c).
 *
 * On the Xbox 360 edition split screen is a card of its own (a television
 * with a lightning bolt); the phone game's MULTIPLAYER card shows Amigo,
 * Sonic and AiAi. Nothing of the Xbox edition ships with this port: if the
 * player puts their own disc image (or just its main menu's project) on the
 * SD card, the card is read from it here, once, and kept (decoded) in the
 * port's data folder; the phone's card is drawn over in the main menu's own
 * texture page each time the menu is made. Without it the phone's card stays
 * (its label says SPLIT SCREEN either way: ssr_race.c's words).
 *
 * BUILT IN. When resources/xbox360/splitscreen_card.png is there at build
 * time (the card, decoded from the disc as below), the program carries it
 * (ssr_res.S, DCR_XCARD_BAKED) and needs no disc. The picture is not part of
 * the public source.
 *
 * Where it looks, in <game>/xbox360/ then <game>/:
 *   fe_mainmenu_EN.stz (or any fe_mainmenu_*.stz): the file itself, from the
 *     disc's Resource/SumoToolResources/;
 *   *.iso: the disc image (XDVDFS; its game partition is found by its volume
 *     descriptor, "MICROSOFT*XBOX*MEDIA"), the file read from inside it.
 *
 * The .stz (big-endian): six {offset, unpacked, packed} for its parts; the
 * fourth is the project (Siff chunks: PTEX holds the sprites, their page and
 * UVs, and each page's D3D texture header with its GPU fetch constant), the
 * sixth the pages' texels -- zlib streams both. The card is sprite 0x5cec17be:
 * DXT3, tiled (the Xbox 360's 32x32-block tiles), 16-bit words big-endian. MIT.
 */
#include <GLES/gl.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz/miniz.h>

#include "gl_layer.h"
#include "ssr.h"
#include "util.h"

const char *dcr_game_root(void); /* dcr_path.c */

#define CARD_SPRITE 0x5cec17beu /* SPLIT SCREEN (the multiplayer submenu's card) */
#define CARD_FILE "fe_mainmenu"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static float bef(const uint8_t *p) {
  const uint32_t u = be32(p);
  float f;
  memcpy(&f, &u, 4);
  return f;
}

/* ------------------------------------------------------------ reading */
/* a file on the SD card, from `off` (64-bit: a disc image is 7 GB) */
typedef struct {
  FsFile f;
  int open;
  FILE *fp; /* a loose file */
} Src;

static int src_read(Src *s, s64 off, void *buf, size_t len) {
  if (s->fp) {
    if (fseek(s->fp, (long)off, SEEK_SET) != 0)
      return -1;
    return fread(buf, 1, len, s->fp) == len ? 0 : -1;
  }
  u64 got = 0;
  return R_SUCCEEDED(fsFileRead(&s->f, off, buf, len, FsReadOption_None, &got)) && got == len ? 0 : -1;
}

static void src_close(Src *s) {
  if (s->fp)
    fclose(s->fp);
  if (s->open)
    fsFileClose(&s->f);
  memset(s, 0, sizeof *s);
}

/* "sdmc:/a/b" -> "/a/b" for the fs service */
static int src_open_iso(Src *s, const char *path) {
  memset(s, 0, sizeof *s);
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  const char *p = strchr(path, ':');
  if (!fs || !p)
    return -1;
  char fspath[FS_MAX_PATH];
  snprintf(fspath, sizeof fspath, "%s", p + 1);
  if (R_FAILED(fsFsOpenFile(fs, fspath, FsOpenMode_Read, &s->f)))
    return -1;
  s->open = 1;
  return 0;
}

/* XDVDFS: the entry `name` (one path component) in the directory at
 * sector `dir`, `size` bytes: its sector and size */
static int xdvd_find(Src *s, s64 part, uint32_t dir, uint32_t size, const char *name, uint32_t *sec, uint32_t *len) {
  if (size == 0 || size > 1u << 20)
    return -1;
  uint8_t *d = malloc(size);
  if (!d || src_read(s, part + (s64)dir * 2048, d, size) != 0) {
    free(d);
    return -1;
  }
  /* a binary tree: +0 left, +2 right (in 4-byte words), +4 sector, +8 size,
   * +0xc attributes, +0xd name length, +0xe the name */
  uint32_t at = 0;
  int rc = -1;
  for (int guard = 0; guard < 4096 && at + 14 <= size; guard++) {
    const uint8_t *e = d + at;
    const int nl = e[13];
    if (at + 14 + (uint32_t)nl > size)
      break;
    char n[256];
    memcpy(n, e + 14, (size_t)nl);
    n[nl] = 0;
    const int c = strcasecmp(name, n);
    if (c == 0) {
      *sec = (uint32_t)e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24;
      *len = (uint32_t)e[8] | (uint32_t)e[9] << 8 | (uint32_t)e[10] << 16 | (uint32_t)e[11] << 24;
      rc = 0;
      break;
    }
    const uint32_t next = c < 0 ? ((uint32_t)e[0] | (uint32_t)e[1] << 8) : ((uint32_t)e[2] | (uint32_t)e[3] << 8);
    if (!next || next == 0xffff)
      break;
    at = next * 4;
  }
  free(d);
  return rc;
}

/* a whole file out of a disc image, or NULL */
static uint8_t *iso_file(const char *iso, const char *const *names, size_t *out_len) {
  Src s;
  if (src_open_iso(&s, iso) != 0)
    return NULL;
  static const s64 parts[] = {0xFD90000ll, 0x2080000ll, 0x18300000ll, 0};
  uint8_t *buf = NULL;
  for (unsigned i = 0; i < sizeof parts / sizeof parts[0] && !buf; i++) {
    uint8_t vd[0x20];
    if (src_read(&s, parts[i] + 32 * 2048, vd, sizeof vd) != 0 || memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20) != 0)
      continue;
    uint32_t dir = (uint32_t)vd[20] | (uint32_t)vd[21] << 8 | (uint32_t)vd[22] << 16 | (uint32_t)vd[23] << 24;
    uint32_t size = (uint32_t)vd[24] | (uint32_t)vd[25] << 8 | (uint32_t)vd[26] << 16 | (uint32_t)vd[27] << 24;
    uint32_t sec = 0, len = 0;
    if (xdvd_find(&s, parts[i], dir, size, "Resource", &sec, &len) != 0 ||
        xdvd_find(&s, parts[i], sec, len, "SumoToolResources", &dir, &size) != 0)
      continue;
    for (const char *const *n = names; *n && !buf; n++) {
      if (xdvd_find(&s, parts[i], dir, size, *n, &sec, &len) != 0 || !len || len > 64u << 20)
        continue;
      buf = malloc(len);
      if (buf && src_read(&s, parts[i] + (s64)sec * 2048, buf, len) != 0) {
        free(buf);
        buf = NULL;
      }
      if (buf) {
        *out_len = len;
        debugPrintf("[xcard] %s: %s (%u bytes)\n", iso, *n, len);
      }
    }
  }
  src_close(&s);
  return buf;
}

static uint8_t *loose_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = n > 72 && n < (64l << 20) ? malloc((size_t)n) : NULL;
  if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
    free(b);
    b = NULL;
  }
  fclose(f);
  if (b) {
    *out_len = (size_t)n;
    debugPrintf("[xcard] %s (%ld bytes)\n", path, n);
  }
  return b;
}

static int ends_with(const char *s, const char *suffix) {
  const size_t a = strlen(s), b = strlen(suffix);
  return a >= b && !strcasecmp(s + a - b, suffix);
}

/* the project, from wherever the player put it */
static uint8_t *find_stz(size_t *len) {
  static const char *const names[] = {CARD_FILE "_EN.stz", CARD_FILE "_US.stz", CARD_FILE "_FR.stz",
                                      CARD_FILE "_GE.stz", CARD_FILE "_IT.stz", CARD_FILE "_SP.stz", NULL};
  char dir[2][512];
  snprintf(dir[0], sizeof dir[0], "%s/xbox360", dcr_game_root());
  snprintf(dir[1], sizeof dir[1], "%s", dcr_game_root());
  for (int d = 0; d < 2; d++) {
    char path[1600];
    for (const char *const *n = names; *n; n++) {
      snprintf(path, sizeof path, "%s/%s", dir[d], *n);
      uint8_t *b = loose_file(path, len);
      if (b)
        return b;
    }
    DIR *dp = opendir(dir[d]);
    struct dirent *de;
    while (dp && (de = readdir(dp))) {
      if (!ends_with(de->d_name, ".iso"))
        continue;
      snprintf(path, sizeof path, "%s/%s", dir[d], de->d_name);
      uint8_t *b = iso_file(path, names, len);
      if (b) {
        closedir(dp);
        return b;
      }
    }
    if (dp)
      closedir(dp);
  }
  return NULL;
}

/* ------------------------------------------------------------ decoding */
/* part k of the .stz, inflated; `upto`: only its first bytes (0: all) */
static uint8_t *stz_part(const uint8_t *f, size_t flen, int k, size_t upto, size_t *out_len) {
  const uint32_t off = be32(f + k * 12), usz = be32(f + k * 12 + 4), psz = be32(f + k * 12 + 8);
  if (!usz || off >= flen || psz > flen - off)
    return NULL;
  const size_t want = upto && upto < usz ? upto : usz;
  uint8_t *out = malloc(want);
  if (!out)
    return NULL;
  mz_stream z;
  memset(&z, 0, sizeof z);
  if (mz_inflateInit(&z) != MZ_OK) {
    free(out);
    return NULL;
  }
  z.next_in = f + off, z.avail_in = psz;
  z.next_out = out, z.avail_out = (unsigned)want;
  int rc;
  do
    rc = mz_inflate(&z, MZ_NO_FLUSH);
  while (rc == MZ_OK && z.avail_out);
  const size_t got = want - z.avail_out;
  mz_inflateEnd(&z);
  if (got < want) {
    free(out);
    return NULL;
  }
  *out_len = want;
  return out;
}

/* XGAddress2DTiledOffset: element (x, y) of a tiled surface `w` elements
 * wide (log2 of the element size: `lb`) -> its element index */
static uint32_t tiled_offset(uint32_t x, uint32_t y, uint32_t w, uint32_t lb) {
  const uint32_t aw = (w + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb;
}

static void rgb565(uint32_t c, int *o) {
  o[0] = (int)((c >> 11) & 31) * 255 / 31;
  o[1] = (int)((c >> 5) & 63) * 255 / 63;
  o[2] = (int)(c & 31) * 255 / 31;
}

/* one DXT3 / DXT1 / DXT5 block (little-endian after the swap) into 4x4 RGBA */
static void dxt_block(const uint8_t *b, int kind, uint8_t out[16][4]) {
  const uint8_t *col = kind == 1 ? b : b + 8;
  const uint32_t c0 = col[0] | col[1] << 8, c1 = col[2] | col[3] << 8;
  const uint32_t idx = (uint32_t)col[4] | (uint32_t)col[5] << 8 | (uint32_t)col[6] << 16 | (uint32_t)col[7] << 24;
  int p[4][3];
  rgb565(c0, p[0]), rgb565(c1, p[1]);
  const int four = kind != 1 || c0 > c1;
  for (int k = 0; k < 3; k++) {
    p[2][k] = four ? (2 * p[0][k] + p[1][k]) / 3 : (p[0][k] + p[1][k]) / 2;
    p[3][k] = four ? (p[0][k] + 2 * p[1][k]) / 3 : 0;
  }
  uint8_t a[16];
  if (kind == 3) {
    for (int i = 0; i < 8; i++)
      a[i * 2] = (uint8_t)((b[i] & 15) * 17), a[i * 2 + 1] = (uint8_t)((b[i] >> 4) * 17);
  } else if (kind == 5) {
    int ap[8];
    ap[0] = b[0], ap[1] = b[1];
    if (ap[0] > ap[1])
      for (int i = 1; i < 7; i++)
        ap[1 + i] = ((7 - i) * ap[0] + i * ap[1]) / 7;
    else {
      for (int i = 1; i < 5; i++)
        ap[1 + i] = ((5 - i) * ap[0] + i * ap[1]) / 5;
      ap[6] = 0, ap[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++)
      bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++)
      a[i] = (uint8_t)ap[(bits >> (3 * i)) & 7];
  } else {
    for (int i = 0; i < 16; i++)
      a[i] = 255;
  }
  for (int i = 0; i < 16; i++) {
    const int s = (int)((idx >> (2 * i)) & 3);
    out[i][0] = (uint8_t)p[s][0], out[i][1] = (uint8_t)p[s][1], out[i][2] = (uint8_t)p[s][2];
    out[i][3] = kind == 1 && !four && s == 3 ? 0 : a[i];
  }
}

/* the card out of the project: RGBA, *w x *h */
static uint8_t *decode_card(const uint8_t *f, size_t flen, int *cw, int *ch) {
  size_t plen = 0;
  uint8_t *proj = stz_part(f, flen, 3, 0, &plen);
  if (!proj) {
    debugPrintf("[xcard] the project part does not inflate\n");
    return NULL;
  }
  /* Siff chunks: FourCC, chunk size, payload size, marker; then the payload */
  const uint8_t *pt = NULL;
  uint32_t ptlen = 0;
  for (size_t at = 0; at + 16 <= plen;) {
    const uint32_t cs = be32(proj + at + 4), ps = be32(proj + at + 8);
    if (!cs || at + 16 + ps > plen)
      break;
    if (!memcmp(proj + at, "PTEX", 4))
      pt = proj + at + 16, ptlen = ps;
    at += cs;
  }
  uint8_t *rgba = NULL;
  if (!pt || ptlen < 12) {
    debugPrintf("[xcard] no sprite table\n");
    goto out;
  }
  const uint32_t ns = be32(pt), np = be32(pt + 4), pto = be32(pt + 8);
  const uint8_t *sp = NULL;
  for (uint32_t i = 0; i < ns && 12 + (i + 1) * 40 <= ptlen; i++)
    if (be32(pt + 12 + i * 40) == CARD_SPRITE)
      sp = pt + 12 + i * 40;
  if (!sp) {
    debugPrintf("[xcard] the SPLIT SCREEN card is not in this project\n");
    goto out;
  }
  const uint32_t page = be32(sp + 36);
  if (page >= np || pto + (page + 1) * 8 > ptlen)
    goto out;
  const uint32_t hdr = be32(pt + pto + page * 8), blob = be32(pt + pto + page * 8 + 4);
  if (hdr + 52 > ptlen)
    goto out;
  /* the fetch constant: words 7..12 of the D3D header */
  const uint8_t *fc = pt + hdr + 28;
  const uint32_t f0 = be32(fc), f1 = be32(fc + 4), f2 = be32(fc + 8);
  const int tiled = (int)(f0 >> 31), fmt = (int)(f1 & 0x3f), endian = (int)((f1 >> 6) & 3);
  const uint32_t pitch = ((f0 >> 22) & 0x1ff) * 32, pw = (f2 & 0x1fff) + 1, ph = ((f2 >> 13) & 0x1fff) + 1;
  const int kind = fmt == 18 ? 1 : fmt == 19 ? 3 : fmt == 20 ? 5 : 0;
  if (!kind) {
    debugPrintf("[xcard] page format %d: not a DXT page\n", fmt);
    goto out;
  }
  const uint32_t bs = kind == 1 ? 8 : 16, lb = kind == 1 ? 3 : 4;
  const uint32_t bw = (pw + 3) / 4, bh = (ph + 3) / 4;
  uint32_t pbw = pitch / 4 > bw ? pitch / 4 : bw;
  pbw = (pbw + 31) & ~31u;
  const size_t psize = (size_t)pbw * ((bh + 31) & ~31u) * bs;
  size_t tlen = 0;
  uint8_t *tex = stz_part(f, flen, 5, blob + psize, &tlen);
  if (!tex) {
    debugPrintf("[xcard] the texels do not inflate\n");
    goto out;
  }
  /* the sprite's rectangle (its UVs' bounds) */
  float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
  for (int i = 0; i < 4; i++) {
    const float u = bef(sp + 4 + i * 8), v = bef(sp + 8 + i * 8);
    u0 = fminf(u0, u), u1 = fmaxf(u1, u), v0 = fminf(v0, v), v1 = fmaxf(v1, v);
  }
  const int x0 = (int)lrintf(u0 * (float)pw), y0 = (int)lrintf(v0 * (float)ph);
  const int w = (int)lrintf((u1 - u0) * (float)pw), h = (int)lrintf((v1 - v0) * (float)ph);
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
    free(tex);
    goto out;
  }
  rgba = calloc((size_t)w * h, 4);
  if (rgba) {
    for (int by = y0 / 4; by <= (y0 + h - 1) / 4; by++)
      for (int bx = x0 / 4; bx <= (x0 + w - 1) / 4; bx++) {
        const size_t at = blob + (size_t)(tiled ? tiled_offset((uint32_t)bx, (uint32_t)by, pbw, lb)
                                                : (uint32_t)by * pbw + (uint32_t)bx) * bs;
        if (at + bs > tlen)
          continue;
        uint8_t blk[16];
        memcpy(blk, tex + at, bs);
        if (endian == 1) /* 8-in-16 */
          for (uint32_t i = 0; i < bs; i += 2) {
            const uint8_t t = blk[i];
            blk[i] = blk[i + 1], blk[i + 1] = t;
          }
        uint8_t px[16][4];
        dxt_block(blk, kind, px);
        for (int i = 0; i < 16; i++) {
          const int x = bx * 4 + (i & 3) - x0, y = by * 4 + (i >> 2) - y0;
          if (x >= 0 && y >= 0 && x < w && y < h)
            memcpy(rgba + ((size_t)y * w + x) * 4, px[i], 4);
        }
      }
    *cw = w, *ch = h;
    debugPrintf("[xcard] the SPLIT SCREEN card: %dx%d (page %u, %ux%u, DXT%d%s)\n", w, h, page, pw, ph, kind,
                tiled ? ", tiled" : "");
  }
  free(tex);
out:
  free(proj);
  return rgba;
}

/* ------------------------------------------------------------ the card */
static struct {
  int tried;
  uint8_t *px; /* RGBA, as decoded */
  int w, h;
  const void *done_for; /* the texture page it was drawn into */
  uint32_t done_gl;
  int fails;
} C;

static void cache_path(char *out, size_t n) { snprintf(out, n, "%s/data/port_splitscreen_card.rgba", dcr_game_root()); }

#if DCR_XCARD_BAKED
uint8_t *ssr_gfx_png(const void *data, int len, int *w, int *h); /* ssr_gfx.c */
extern const uint8_t ssr_res_xcard[]; /* ssr_res.S: built in */
extern const uint32_t ssr_res_xcard_size;
#endif

/* the card: the cache, else the one built in, else the player's disc (once a
 * session) */
static int card_load(void) {
  if (C.tried)
    return C.px != NULL;
  C.tried = 1;
  char path[600];
  cache_path(path, sizeof path);
  FILE *f = fopen(path, "rb");
  if (f) {
    uint32_t wh[2];
    if (fread(wh, 4, 2, f) == 2 && wh[0] && wh[1] && wh[0] <= 1024 && wh[1] <= 1024) {
      C.px = malloc((size_t)wh[0] * wh[1] * 4);
      if (C.px && fread(C.px, 4, (size_t)wh[0] * wh[1], f) == (size_t)wh[0] * wh[1])
        C.w = (int)wh[0], C.h = (int)wh[1];
      else {
        free(C.px);
        C.px = NULL;
      }
    }
    fclose(f);
    if (C.px)
      return 1;
  }
#if DCR_XCARD_BAKED
  C.px = ssr_gfx_png(ssr_res_xcard, (int)ssr_res_xcard_size, &C.w, &C.h);
  if (C.px) {
    debugPrintf("[xcard] the SPLIT SCREEN card: built in (%dx%d)\n", C.w, C.h);
    return 1;
  }
#endif
  const u64 t0 = armGetSystemTick();
  size_t len = 0;
  uint8_t *stz = find_stz(&len);
  if (!stz) {
    debugPrintf("[xcard] no Xbox 360 disc (or %s_*.stz) on the SD card: the phone's card, SPLIT SCREEN on it\n",
                CARD_FILE);
    return 0;
  }
  C.px = decode_card(stz, len, &C.w, &C.h);
  free(stz);
  if (!C.px)
    return 0;
  f = fopen(path, "wb");
  if (f) {
    const uint32_t wh[2] = {(uint32_t)C.w, (uint32_t)C.h};
    fwrite(wh, 4, 2, f);
    fwrite(C.px, 4, (size_t)C.w * C.h, f);
    fclose(f);
  }
  debugPrintf("[xcard] decoded in %llu ms, kept in data/\n",
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
  return 1;
}

/* the opaque part's bounds (alpha > 128) */
static void alpha_bounds(const uint8_t *px, int w, int h, int *b) {
  b[0] = w, b[1] = h, b[2] = -1, b[3] = -1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (px[((size_t)y * w + x) * 4 + 3] > 128) {
        if (x < b[0])
          b[0] = x;
        if (y < b[1])
          b[1] = y;
        if (x > b[2])
          b[2] = x;
        if (y > b[3])
          b[3] = y;
      }
  if (b[2] < b[0])
    b[0] = 0, b[1] = 0, b[2] = w - 1, b[3] = h - 1;
}

/* the card redrawn for a sprite w x h: its frame on the phone card's frame
 * (the phone's HD card, 416 x 246: x 2..413, y 12..243 -- the red label
 * strips then line up), bilinear */
static uint8_t *card_fit(int w, int h) {
  uint8_t *out = calloc((size_t)w * h, 4);
  if (!out)
    return NULL;
  int b[4];
  alpha_bounds(C.px, C.w, C.h, b);
  const float dx0 = 2.0f / 416.0f * w, dy0 = 12.0f / 246.0f * h;
  const float dx1 = 414.0f / 416.0f * w, dy1 = 244.0f / 246.0f * h;
  const float sx = (float)(b[2] + 1 - b[0]) / (dx1 - dx0), sy = (float)(b[3] + 1 - b[1]) / (dy1 - dy0);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const float fx = ((float)x + 0.5f - dx0) * sx + (float)b[0] - 0.5f;
      const float fy = ((float)y + 0.5f - dy0) * sy + (float)b[1] - 0.5f;
      if (fx < -0.5f || fy < -0.5f || fx > (float)C.w - 0.5f || fy > (float)C.h - 0.5f)
        continue;
      const int ix = (int)floorf(fx), iy = (int)floorf(fy);
      const float ax = fx - (float)ix, ay = fy - (float)iy;
      float acc[4] = {0};
      for (int k = 0; k < 4; k++) {
        const int px = ix + (k & 1), py = iy + (k >> 1);
        const float wt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
        if (px < 0 || py < 0 || px >= C.w || py >= C.h)
          continue;
        const uint8_t *s = C.px + ((size_t)py * C.w + px) * 4;
        const float a = s[3] * wt;
        acc[0] += s[0] * a, acc[1] += s[1] * a, acc[2] += s[2] * a, acc[3] += a;
      }
      uint8_t *o = out + ((size_t)y * w + x) * 4;
      if (acc[3] > 0.0f) {
        o[0] = (uint8_t)fminf(acc[0] / acc[3], 255.0f);
        o[1] = (uint8_t)fminf(acc[1] / acc[3], 255.0f);
        o[2] = (uint8_t)fminf(acc[2] / acc[3], 255.0f);
      }
      o[3] = (uint8_t)fminf(acc[3], 255.0f);
    }
  return out;
}

/* Each frame the main menu is up (ssr_split.c): the MULTIPLAYER card
 * (sprite 0xaf6cbed3 of the main menu's cards, scene 0x98aaddd5) redrawn in
 * its texture page, once for each page the menu makes. `loadset`: that
 * scene's (CSceneEntry +0x30); its texture manager at +0x4c. */
/* IPODTextureManager::UploadTexture (its pack registered: the whole page,
 * glTexImage2D): a page the card was drawn into is the engine's picture
 * again -- the card goes back in on the next frame */
typedef void (*fn_upload)(void *mgr, void *page, const void *data);
static fn_upload o_upload;
static void page_upload(void *mgr, void *page, const void *data) {
  o_upload(mgr, page, data);
  if (page == C.done_for)
    C.done_for = NULL;
  void ssr_battlecard_page_uploaded(const void *page); /* ssr_battlecard.c */
  ssr_battlecard_page_uploaded(page);
}

void *ssr_patch_hook(const char *sym, uint32_t expect, void *dst); /* ssr_patch.c, at load */
void ssr_xcard_patch(void) {
  o_upload = (fn_upload)ssr_patch_hook("_ZN18IPODTextureManager13UploadTextureEPN4Siff11IPODTexture7TextureEPKv",
                                       0xe92d40f0u, (void *)page_upload);
  if (!o_upload)
    debugPrintf("[xcard] the texture uploads: not hooked\n");
}

void ssr_xcard_frame(void *loadset) {
  if (!loadset || !card_load())
    return;
  uint8_t *(*get_tex)(void *mgr, uint32_t id) = (uint8_t * (*)(void *, uint32_t))
      ssr_native("_ZNK18IPODTextureManager10GetTextureEj");
  const uint32_t *cur = (const uint32_t *)ssr_native("_ZN4GLES17ms_currentTextureE");
  uint8_t *tex = get_tex ? get_tex((uint8_t *)loadset + 0x4c, 0xaf6cbed3u) : NULL;
  const uint8_t *page = tex ? *(uint8_t *const *)(tex + 0x24) : NULL;
  if (!page)
    return;
  const uint32_t gl = *(const uint32_t *)page;
  const int pw = *(const int32_t *)(page + 4), ph = *(const int32_t *)(page + 8);
  if (!gl || pw <= 0 || ph <= 0 || (C.done_for == page && C.done_gl == gl) || C.fails > 600)
    return;
  /* the sprite's rectangle in its page (its UVs' bounds, as uploadTexture
   * places it: CTextureObject::uploadTexture, 0x1a349c) */
  float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
  for (int i = 0; i < 4; i++) {
    const float u = ((const float *)(tex + 4))[i * 2], v = ((const float *)(tex + 4))[i * 2 + 1];
    u0 = fminf(u0, u), u1 = fmaxf(u1, u), v0 = fminf(v0, v), v1 = fmaxf(v1, v);
  }
  const int x = (int)((float)pw * u0), y = (int)((float)ph * v0);
  const int w = (int)lrintf((u1 - u0) * (float)pw), h = (int)lrintf((v1 - v0) * (float)ph);
  if (w <= 8 || h <= 8 || x + w > pw || y + h > ph) {
    debugPrintf("[xcard] the MULTIPLAYER card's place looks wrong (%d,%d %dx%d of %dx%d): left alone\n", x, y, w, h,
                pw, ph);
    return;
  }
  uint8_t *px = card_fit(w, h);
  if (!px)
    return;
  /* the engine's texture numbers are its own (DDGLRefresh::sTextures: each
   * one's GL name, for its context coming back): its own bind and upload */
  void (*bind)(GLenum, GLuint) =
      (void (*)(GLenum, GLuint))ssr_native("_ZN11DDGLRefresh15ddGLBindTextureEjj");
  void (*sub)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *) =
      (void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))ssr_native(
          "_ZN11DDGLRefresh17ddGLTexSubImage2DEjiiiiijjPKv");
  void (*store)(GLenum, GLint) = (void (*)(GLenum, GLint))dcr_gl_lookup("glPixelStorei");
  if (bind && sub) {
    bind(GL_TEXTURE_2D, gl);
    if (store)
      store(GL_UNPACK_ALIGNMENT, 4);
    GLenum (*err)(void) = (GLenum(*)(void))dcr_gl_lookup("glGetError");
    while (err && err() != 0) {
    }
    sub(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const GLenum e = err ? err() : 0;
    bind(GL_TEXTURE_2D, cur ? *cur : 0); /* what the engine's cache says is bound */
    if (e == 0) {
      C.done_for = page, C.done_gl = gl;
      debugPrintf("[xcard] the main menu's MULTIPLAYER card is the Xbox 360's SPLIT SCREEN card (%dx%d at %d,%d of "
                  "texture %u, %dx%d)\n",
                  w, h, x, y, (unsigned)gl, pw, ph);
    } else if (C.fails++ == 0) {
      /* its page not uploaded yet (GL_INVALID_VALUE): again each frame, and
       * as soon as the engine uploads it (page_upload) */
      debugPrintf("[xcard] the card's texture page is not there yet (GL error 0x%x): again when it is\n",
                  (unsigned)e);
    }
  }
  free(px);
}
