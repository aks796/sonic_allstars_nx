/* ssr_pack.h -- where the game's data is on the SD card, read in place.
 *
 * Sonic & SEGA All-Stars Racing keeps all of its data (models, tracks, UI,
 * sound effects) in one flat archive, packres.png (the "D0" pack of the
 * Android port's DDFile). On a phone it sits STORED inside the expansion
 * file main.20.com.sega.ssasr.obb, and the Java side hands the engine that
 * file's path plus the byte offset and length of packres.png inside it
 * (DemoActivity.P -> nativeSetFileSystem); the engine then fopen()s the path
 * and seeks. Nothing is ever unzipped. The intro movie, intro_full.m4v, is a
 * stored entry of the same file.
 *
 * The player may have that data in any of these, and this finds it in each,
 * without extracting anything:
 *   - the OBB itself (any *.obb in the game folder);
 *   - the download it came in: a zip that STORES the .obb (so the OBB's bytes,
 *     and packres.png's inside them, are contiguous in the outer file);
 *   - an APK that carries the data as assets/packres.png and
 *     assets/intro_full.m4v (the single-APK build of source/tools/build.py).
 * Files are told apart by what is in them, never by their names: the APK is
 * the zip that holds lib/armeabi/libssasr.so (ssr_find_apk), the data the
 * file that holds packres.png (ssr_find_data).
 * Each is a zip whose entries are looked up through its central directory
 * (the same walk as the APK expansion library's ZipResourceFile: local header
 * offset + 30 + name length + extra length), recursing once into a stored
 * .obb. Header-only, stdio only: the 64-bit launcher uses it too. MIT.
 */
#ifndef SSR_PACK_H
#define SSR_PACK_H

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The retail pack (1.0.1 / OBB main.20): its size and entry count. */
#define SSR_PACK_SIZE 410877975u
#define SSR_PACK_ENTRIES 211u

typedef struct {
  char path[768];      /* the file to fopen: the .obb, the .zip or the .apk */
  uint64_t pack_off;   /* packres.png's data in it */
  uint32_t pack_len;
  uint64_t video_off;  /* intro_full.m4v's data in it (0 len: none) */
  uint32_t video_len;
  int nested;          /* 1: the .obb was found inside a zip */
  char how[64];        /* "obb", "zip > obb", "apk assets" */
} SsrPack;

static inline uint32_t ssr_rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t ssr_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline int ssr_read_at(FILE *f, uint64_t off, void *buf, size_t n) {
  if (fseeko(f, (off_t)off, SEEK_SET) != 0)
    return -1;
  return fread(buf, 1, n, f) == n ? 0 : -1;
}

/* One stored-or-not entry of the zip that starts at `base` in `f` and is
 * `size` bytes long. Calls `cb(name, method, data_off, comp_len, len, ctx)` for
 * every entry; a non-zero return stops the walk. -1 if it is not a zip. */
typedef int (*SsrZipEntryFn)(const char *name, int method, uint64_t data_off, uint32_t comp_len,
                             uint32_t len, void *ctx);

static inline int ssr_zip_walk(FILE *f, uint64_t base, uint64_t size, SsrZipEntryFn cb, void *ctx) {
  uint8_t sig[4];
  if (size < 22 || ssr_read_at(f, base, sig, 4) != 0 || ssr_rd32(sig) != 0x04034b50u)
    return -1;
  /* the end-of-central-directory record: within the last 64 KiB + 22 */
  uint32_t tail = size < 65557u ? (uint32_t)size : 65557u;
  uint8_t *t = (uint8_t *)malloc(tail);
  if (!t || ssr_read_at(f, base + size - tail, t, tail) != 0) {
    free(t);
    return -1;
  }
  int64_t eocd = -1;
  for (int64_t i = (int64_t)tail - 22; i >= 0; i--)
    if (t[i] == 'P' && ssr_rd32(t + i) == 0x06054b50u) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    free(t);
    return -1;
  }
  uint32_t count = ssr_rd16(t + eocd + 10), cd_size = ssr_rd32(t + eocd + 12), cd_off = ssr_rd32(t + eocd + 16);
  free(t);
  if ((uint64_t)cd_off + cd_size > size || cd_size > (64u << 20))
    return -1;
  uint8_t *cd = (uint8_t *)malloc(cd_size ? cd_size : 1);
  if (!cd || ssr_read_at(f, base + cd_off, cd, cd_size) != 0) {
    free(cd);
    return -1;
  }
  uint32_t p = 0;
  int stop = 0;
  for (uint32_t i = 0; i < count && !stop; i++) {
    if (p + 46 > cd_size || ssr_rd32(cd + p) != 0x02014b50u)
      break;
    const int method = (int)ssr_rd16(cd + p + 10);
    const uint32_t comp = ssr_rd32(cd + p + 20), len = ssr_rd32(cd + p + 24);
    const uint32_t nlen = ssr_rd16(cd + p + 28), xlen = ssr_rd16(cd + p + 30), clen = ssr_rd16(cd + p + 32);
    const uint32_t lho = ssr_rd32(cd + p + 42);
    if (p + 46 + nlen > cd_size)
      break;
    char name[256];
    uint32_t n = nlen < sizeof name - 1 ? nlen : (uint32_t)sizeof name - 1;
    memcpy(name, cd + p + 46, n);
    name[n] = 0;
    uint8_t lh[30];
    if (ssr_read_at(f, base + lho, lh, 30) == 0 && ssr_rd32(lh) == 0x04034b50u) {
      const uint64_t data = base + lho + 30 + ssr_rd16(lh + 26) + ssr_rd16(lh + 28);
      stop = cb(name, method, data, comp, len, ctx);
    }
    p += 46 + nlen + xlen + clen;
  }
  free(cd);
  return 0;
}

static inline int ssr_ends_with(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  if (a < b)
    return 0;
  for (size_t i = 0; i < b; i++) {
    char x = s[a - b + i], y = suffix[i];
    if (x >= 'A' && x <= 'Z')
      x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z')
      y = (char)(y - 'A' + 'a');
    if (x != y)
      return 0;
  }
  return 1;
}

typedef struct {
  SsrPack *out;
  FILE *f;
  int depth;
  int found_pack;
} SsrWalk;

static int ssr_walk_entry(const char *name, int method, uint64_t data, uint32_t comp, uint32_t len, void *ctx);

static inline int ssr_scan_zip(SsrWalk *w, uint64_t base, uint64_t size) {
  return ssr_zip_walk(w->f, base, size, ssr_walk_entry, w);
}

static int ssr_walk_entry(const char *name, int method, uint64_t data, uint32_t comp, uint32_t len, void *ctx) {
  SsrWalk *w = (SsrWalk *)ctx;
  (void)comp; /* stored entries only: their size is len */
  const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
  if (method != 0) /* only stored entries can be read in place */
    return 0;
  if (!strcmp(base, "packres.png") && len >= 16) {
    uint8_t h[8];
    if (ssr_read_at(w->f, data, h, 8) == 0 && h[0] == 'D' && h[1] == '0') {
      w->out->pack_off = data;
      w->out->pack_len = len;
      w->found_pack = 1;
    }
  } else if (!strcmp(base, "intro_full.m4v")) {
    w->out->video_off = data;
    w->out->video_len = len;
  } else if (w->depth == 0 && ssr_ends_with(base, ".obb") && !w->found_pack) {
    /* a stored expansion file inside the download: look inside it */
    SsrWalk inner = {w->out, w->f, 1, 0};
    if (ssr_scan_zip(&inner, data, len) == 0 && inner.found_pack) {
      w->found_pack = 1;
      w->out->nested = 1;
    }
  }
  return w->found_pack && w->out->video_len ? 1 : 0;
}

/* Look for the pack in the zip-shaped file at `path`. 0 if found. */
static inline int ssr_pack_probe(const char *path, SsrPack *out) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  fseeko(f, 0, SEEK_END);
  uint64_t size = (uint64_t)ftello(f);
  SsrPack p;
  memset(&p, 0, sizeof p);
  SsrWalk w = {&p, f, 0, 0};
  int rc = ssr_scan_zip(&w, 0, size);
  fclose(f);
  if (rc != 0 || !w.found_pack)
    return -1;
  snprintf(p.path, sizeof p.path, "%s", path);
  snprintf(p.how, sizeof p.how, "%s",
           p.nested ? "zip > obb" : ssr_ends_with(path, ".apk") ? "apk assets" : "obb");
  *out = p;
  return 0;
}

/* ---------------------------------------------------------------- the APK */
/* The game's APK, whatever it is called: a zip holding its library,
 * lib/armeabi/libssasr.so (0 if it is one). */
static int ssr_apk_entry(const char *name, int method, uint64_t data, uint32_t comp, uint32_t len, void *ctx) {
  (void)method, (void)data, (void)comp, (void)len;
  if (!strcmp(name, "lib/armeabi/libssasr.so")) {
    *(int *)ctx = 1;
    return 1;
  }
  return 0;
}

static inline int ssr_apk_probe(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  fseeko(f, 0, SEEK_END);
  const uint64_t size = (uint64_t)ftello(f);
  int found = 0;
  const int rc = ssr_zip_walk(f, 0, size, ssr_apk_entry, &found);
  fclose(f);
  return rc == 0 && found ? 0 : -1;
}

/* files of the port's own, never the player's APK or data */
static inline int ssr_port_file(const char *name) {
  static const char *const own[] = {".nro", ".nsp", ".ini", ".log", ".txt", ".so", ".setup", ".update", ".rgba", ".ttf"};
  if (name[0] == '.')
    return 1;
  for (size_t i = 0; i < sizeof own / sizeof own[0]; i++)
    if (ssr_ends_with(name, own[i]))
      return 1;
  return 0;
}

/* The APK in folder `dir`, by its contents: game.apk if it is the game's,
 * else any other file there that is (*.apk first, then the rest, as the
 * player may have named it anything). 0 and its path in `out` if found. */
static inline int ssr_find_apk(const char *dir, char *out, size_t cap) {
  snprintf(out, cap, "%s/game.apk", dir);
  if (ssr_apk_probe(out) == 0)
    return 0;
  for (int pass = 0; pass < 2; pass++) {
    DIR *d = opendir(dir);
    if (!d)
      return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
      const int apk = ssr_ends_with(e->d_name, ".apk");
      if ((pass == 0) != apk || ssr_port_file(e->d_name))
        continue;
      snprintf(out, cap, "%s/%s", dir, e->d_name);
      if (ssr_apk_probe(out) == 0) {
        closedir(d);
        return 0;
      }
    }
    closedir(d);
  }
  out[0] = 0;
  return -1;
}

/* The game's data in folder `dir`, by its contents: the APK itself (a
 * single-APK build), then any file there that holds packres.png -- the OBB,
 * the zip it came in, whatever either is called (*.obb and *.zip first). */
static inline int ssr_find_data(const char *dir, const char *apk, SsrPack *out) {
  if (apk && apk[0] && ssr_pack_probe(apk, out) == 0)
    return 0;
  for (int pass = 0; pass < 2; pass++) {
    DIR *d = opendir(dir);
    if (!d)
      return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
      const int likely = ssr_ends_with(e->d_name, ".obb") || ssr_ends_with(e->d_name, ".zip");
      if ((pass == 0) != likely || ssr_port_file(e->d_name))
        continue;
      char path[768];
      snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
      if (apk && !strcmp(path, apk))
        continue;
      if (ssr_pack_probe(path, out) == 0) {
        closedir(d);
        return 0;
      }
    }
    closedir(d);
  }
  return -1;
}

#endif /* SSR_PACK_H */
