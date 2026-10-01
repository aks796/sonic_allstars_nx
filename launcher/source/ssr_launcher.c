/* ssr_launcher.c -- Sonic & SEGA All-Stars Racing's part of the launcher
 * (runtime/launcher: the shared one).
 *
 * Besides its APK the game needs its data: the expansion file
 * (main.20.com.sega.ssasr.obb) or the zip it came in, whatever either is
 * called -- found by what is in it, with the same finder the game program
 * uses (../source/ssr_pack.h). MIT.
 */
#include <stdio.h>
#include <string.h>

#include "launcher.h"
#include "rt_settings.h"
#include "ssr_pack.h"

#define GAME_DIR "sdmc:" PORT_ROOT_PATH
#define OBB_NAME "main.20.com.sega.ssasr.obb"

int port_launcher_check(char *status, size_t scap, char *help, size_t hcap) {
  SsrPack pack;
  help[0] = 0;
  if (ssr_find_data(GAME_DIR, NULL, &pack) == 0) {
    snprintf(status, scap, "game data: %s (%s)", strrchr(pack.path, '/') ? strrchr(pack.path, '/') + 1 : pack.path,
             pack.how);
    return 0;
  }
  snprintf(status, scap, "game data: MISSING");
  snprintf(help, hcap,
           "Copy its expansion file (on Android: Android/obb/com.sega.ssasr/\n"
           OBB_NAME ") to:\n  " GAME_DIR "/\n"
           "or the .zip it came in; any name, nothing needs unpacking.");
  return 1;
}

void port_launcher_instructions(void) {
  printf("  1. put your own copy of the game in " PORT_ROOT_PATH "\n"
         "     (any file names):\n"
         "       the APK (com.sega.ssasr 1.0.1)\n"
         "       its expansion file, " OBB_NAME "\n"
         "       (or the .zip it came in)\n");
}
