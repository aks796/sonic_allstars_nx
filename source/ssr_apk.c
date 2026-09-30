/* ssr_apk.c -- the entries of the APK, read in place.
 *
 * What the port takes from the player's APK besides the library (which
 * dcr_setup.c unpacks once): the music (res/raw/music_*.mp3, stored, streamed
 * by ssr_media.c at their offsets) and the Java side's splash pictures
 * (res/raw/loading*.jpg). The central directory is walked once
 * (ssr_pack.h's zip reader); stored entries are read at their offsets, and
 * deflated ones through miniz. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz/miniz.h>

#include "ssr.h"
#include "util.h"

typedef struct {
  char *name;
  int method;
  uint64_t off;
  uint32_t comp, len;
} Entry;

static Entry *g_e;
static int g_n, g_cap;
static char g_path[512];

const char *ssr_apk_path(void) { return g_path; }

static int add_entry(const char *name, int method, uint64_t off, uint32_t comp, uint32_t len, void *ctx) {
  (void)ctx;
  if (g_n == g_cap) {
    int cap = g_cap ? g_cap * 2 : 1024;
    Entry *e = realloc(g_e, sizeof *e * (size_t)cap);
    if (!e)
      return 1;
    g_e = e, g_cap = cap;
  }
  g_e[g_n].name = strdup(name);
  g_e[g_n].method = method;
  g_e[g_n].off = off;
  g_e[g_n].comp = comp;
  g_e[g_n].len = len;
  if (g_e[g_n].name)
    g_n++;
  return 0;
}

int ssr_apk_init(const char *path) {
  snprintf(g_path, sizeof g_path, "%s", path);
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  fseeko(f, 0, SEEK_END);
  uint64_t size = (uint64_t)ftello(f);
  int rc = ssr_zip_walk(f, 0, size, add_entry, NULL);
  fclose(f);
  debugPrintf("[apk] %s: %d entries\n", path, g_n);
  return rc == 0 && g_n > 0 ? 0 : -1;
}

static const Entry *find(const char *name) {
  for (int i = 0; i < g_n; i++)
    if (!strcmp(g_e[i].name, name))
      return &g_e[i];
  return NULL;
}

int ssr_apk_locate(const char *name, uint64_t *off, uint32_t *len, int *method) {
  const Entry *e = find(name);
  if (!e)
    return -1;
  *off = e->off;
  *len = e->len;
  *method = e->method;
  return 0;
}

uint8_t *ssr_apk_read(const char *name, size_t *len) {
  const Entry *e = find(name);
  if (!e || (e->method != 0 && e->method != 8))
    return NULL;
  FILE *f = fopen(g_path, "rb");
  if (!f)
    return NULL;
  uint8_t *raw = malloc(e->comp ? e->comp : 1);
  int ok = raw && ssr_read_at(f, e->off, raw, e->comp) == 0;
  fclose(f);
  if (!ok) {
    free(raw);
    return NULL;
  }
  if (e->method == 0) {
    *len = e->comp;
    return raw;
  }
  uint8_t *out = malloc(e->len ? e->len : 1);
  size_t got = out ? tinfl_decompress_mem_to_mem(out, e->len, raw, e->comp, 0) : TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
  free(raw);
  if (got != e->len) {
    free(out);
    debugPrintf("[apk] %s: could not inflate it\n", name);
    return NULL;
  }
  *len = e->len;
  return out;
}
