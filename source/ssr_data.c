/* ssr_data.c -- finding the game's data on the SD card (see ssr_pack.h).
 *
 * In order: the APK itself (a single APK that carries assets/packres.png),
 * then any file in the game folder that holds the pack -- the expansion file
 * or a zip that stores it (the form the game's data is often downloaded in),
 * whatever either is called (*.obb and *.zip looked at first). Nothing is extracted: the engine
 * reads packres.png in place at the offset found here, as on the phone. MIT.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#include "config.h"
#include "ssr.h"
#include "util.h"

const char *dcr_game_root(void); /* dcr_path.c */

static SsrPack g_pack;
static int g_have;

const SsrPack *ssr_pack(void) { return g_have ? &g_pack : NULL; }

int ssr_data_find(const char *apk) {
  /* the APK itself, then the file in the game folder that holds the pack,
   * whatever it is called (ssr_pack.h) */
  if (ssr_find_data(dcr_game_root(), apk, &g_pack) != 0)
    return -1;
  g_have = 1;
  debugPrintf("[data] the game's data: %s (%s): packres.png at %llu, %u bytes; intro_full.m4v %s\n", g_pack.path,
              g_pack.how, (unsigned long long)g_pack.pack_off, (unsigned)g_pack.pack_len,
              g_pack.video_len ? "found" : "not there");
  if (g_pack.pack_len != SSR_PACK_SIZE)
    debugPrintf("[data] WARNING: this pack is %u bytes, not %u (main.20 for 1.0.1): another version of the game's "
                "data may not match the library\n",
                (unsigned)g_pack.pack_len, (unsigned)SSR_PACK_SIZE);
  return 0;
}
