/* dcr_path.c -- turn the paths an Android build of the game asks for into
 * paths on the Switch SD card.
 *
 *   /data/data/<pkg>/...   /data/user/0/<pkg>/...       (internal storage)
 *        -> <root>/data/...      getFilesDir(): the engine chdir()s there and
 *                                keeps its saves in userdata/
 *   /storage/emulated/0/Android/data/<pkg>/...  /sdcard/Android/data/<pkg>/...
 *        -> <root>/external/...
 *   /data/app/<pkg>-1/lib/arm/libfoo.so                (native lib dir)
 *        -> <root>/libfoo.so
 *   /data/app/<pkg>-1/base.apk                         (ApplicationInfo.sourceDir,
 *        -> the player's APK       ANDROID_SOURCE_DIR): whatever it is called
 *                                  (main.c finds it by its contents). The
 *                                  engine's PakLib reads its assets/files straight
 *                                  out of the zip (zziplib)
 *   /assets/...   assets/...                           (device/relative forms)
 *        -> <root>/assets/...
 *
 * Paths that match nothing are returned unchanged. MIT.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "dcr_path.h"
#include "util.h"

const char *dcr_game_root(void); /* main.c: "sdmc:/switch/sonic_allstars_nx" */
const char *dcr_apk_path(void);  /* main.c: the player's APK */

#define PKG DCR_PACKAGE

static int is_sep(char c) { return c == '/' || c == '\\'; }

/* `p` begins with `pre` followed by a separator or end -> the tail, else NULL. */
static const char *after_prefix(const char *p, const char *pre) {
  size_t n = strlen(pre);
  if (!n || strncmp(p, pre, n) != 0)
    return NULL;
  const char *t = p + n;
  if (is_sep(*t))
    return t + 1;
  if (!*t)
    return "";
  return NULL;
}

/* Collapse duplicate separators and turn '\' into '/'. */
static void clean(const char *in, char *out, size_t cap) {
  size_t o = 0;
  int prev_sep = 0;
  for (; *in && o + 1 < cap; in++) {
    char c = *in == '\\' ? '/' : *in;
    if (c == '/') {
      if (prev_sep)
        continue;
      prev_sep = 1;
    } else {
      prev_sep = 0;
    }
    out[o++] = c;
  }
  out[o] = 0;
}

static const char *join(char *out, size_t cap, const char *sub, const char *tail) {
  char t[DCR_PATH_MAX];
  clean(tail, t, sizeof t);
  if (t[0])
    snprintf(out, cap, "%s/%s/%s", dcr_game_root(), sub, t);
  else
    snprintf(out, cap, "%s/%s", dcr_game_root(), sub);
  return out;
}

/* Paths worth tracing (limited): the APK, and the native file-system probes. */
int dcr_path_traced(const char *p) {
  return p && (strstr(p, ".apk") || strstr(p, "main.pak"));
}

static const char *translate(const char *path, char *out, size_t cap);

const char *dcr_translate_path(const char *path, char *out, size_t cap) {
  const char *r = translate(path, out, cap);
  static int logged;
  if (dcr_path_traced(path) && logged < 64) {
    logged++;
    debugPrintf("[path] %s -> %s\n", path, r);
  }
  return r;
}

const char *dcr_cwd(void); /* bionic_io.c: the game's chdir() */

static const char *translate_abs(const char *path, char *out, size_t cap);

static const char *translate(const char *path, char *out, size_t cap) {
  if (!path || !out || !cap)
    return path;
  if (!path[0] || path[0] == '/' || strchr(path, ':'))
    return translate_abs(path, out, cap);
  /* Relative to the game's working directory, as on Android (the engine
   * chdir()s into its files directory and saves in "userdata/..."). */
  char rel[DCR_PATH_MAX];
  const char *t = path;
  while (t[0] == '.' && t[1] == '/')
    t += 2;
  snprintf(rel, sizeof rel, "%s/%s", dcr_cwd(), t);
  const char *r = translate_abs(rel, out, cap);
  if (r != out) {
    snprintf(out, cap, "%s", r);
    r = out;
  }
  return r;
}

static const char *translate_abs(const char *path, char *out, size_t cap) {
  const char *p = path;
  while (!strncmp(p, "file://", 7))
    p += 7;
  if (!strncmp(p, "jar:file://", 11)) {
    const char *bang = strstr(p, "!/");
    if (bang)
      p = bang + 1; /* "/assets/..." */
  }

  /* <root>/... (with or without "sdmc:") is already ours. */
  const char *root = dcr_game_root();
  const char *root_nodev = strchr(root, ':') ? strchr(root, ':') + 1 : root;
  if (!strncmp(p, root, strlen(root)))
    return p == path ? path : (snprintf(out, cap, "%s", p), out);
  if (!strncmp(p, root_nodev, strlen(root_nodev))) {
    snprintf(out, cap, "sdmc:%s", p);
    return out;
  }

  const char *t;
  if ((t = after_prefix(p, "/assets")) || (t = after_prefix(p, "assets")) ||
      (t = after_prefix(p, "./assets")))
    return join(out, cap, "assets", t);

  if ((t = after_prefix(p, "/data/data/" PKG)) || (t = after_prefix(p, "/data/user/0/" PKG)))
    return join(out, cap, "data", t);

  if ((t = after_prefix(p, "/storage/emulated/0/Android/data/" PKG)) ||
      (t = after_prefix(p, "/sdcard/Android/data/" PKG)) ||
      (t = after_prefix(p, "/mnt/sdcard/Android/data/" PKG)))
    return join(out, cap, "external", t);

  if ((t = after_prefix(p, "/storage/emulated/0/Android/obb/" PKG)) ||
      (t = after_prefix(p, "/sdcard/Android/obb/" PKG)))
    return join(out, cap, "obb", t);

  if (!strncmp(p, "/data/app/", 10)) {
    const char *lib = strstr(p, "/lib/arm/");
    if (lib)
      return join(out, cap, ".", lib + 9);
    size_t n = strlen(p);
    if (n > 4 && !strcmp(p + n - 4, ".apk")) {
      snprintf(out, cap, "%s", dcr_apk_path());
      return out;
    }
  }

  if (p != path) {
    snprintf(out, cap, "%s", p);
    return out;
  }
  return path;
}

/* mkdir -p for the directories the game expects Android to have created. */
void dcr_path_prepare_dirs(void) {
  static const char *const dirs[] = {"data", "data/files", "data/cache", "data/shared_prefs",
                                     "external", "external/files", "external/cache"};
  char p[DCR_PATH_MAX];
  for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
    snprintf(p, sizeof p, "%s/%s", dcr_game_root(), dirs[i]);
    mkdir(p, 0777);
  }
}
