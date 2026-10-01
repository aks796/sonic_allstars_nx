/* ssr_main.c -- Sonic & SEGA All-Stars Racing's part of the boot sequence.
 *
 * The runtime's main() (runtime/source/main.c) does everything up to the
 * player's APK: the old folder's move, the log, config.ini, the start-up
 * boost, the NRO self-update, the APK found by its contents (PORT_APK_ROLES:
 * the zip that holds lib/armeabi/libssasr.so, any name). Here: the library
 * and classes.txt out of the APK (ssr_setup_plan.c), the APK's files and the
 * expansion file read in place, the engine loaded; then the engine's
 * constructors and the game. MIT.
 */
#include <switch.h>

#include "config.h"
#include "dcr_path.h"
#include "dcr_setup.h"
#include "error.h"
#include "rt_boot.h"
#include "ssr.h"

/* The error screens' advice when the folder holds no APK of the game. */
const char *port_apk_help(void) {
  return "Copy the APK of your own Sonic & SEGA All-Stars Racing\n"
         "(com.sega.ssasr 1.0.1, armeabi) into that folder:\n"
         "any name will do. The game's library, music and pictures\n"
         "come from it.";
}

int port_load(const char *apk) {
  /* libssasr.so and classes.txt, from the APK when they are missing or it
   * has changed */
  dcr_setup_from_apk(apk);
  if (ssr_apk_init(apk) != 0)
    fatal_error("Could not read the files in %s.\n\nIs it the APK of Sonic & SEGA All-Stars Racing?", apk);
  /* the game's data: the expansion file (or the zip it came in), read in place */
  if (ssr_data_find(apk) != 0)
    fatal_error("The game's data is missing.\n\n"
                "Copy your own expansion file of Sonic & SEGA All-Stars Racing to\n"
                "%s (on Android: Android/obb/com.sega.ssasr/\n"
                SSR_OBB_NAME "), or the .zip it came in; any name will do.\n"
                "It holds packres.png, which the game reads in place.",
                dcr_game_root());
  if (ssr_load_module() != 0)
    fatal_error("Could not load the game library from %s.\n\n"
                "It is unpacked from the APK (" SSR_ABI_DIR ") on launch: delete\n" SSR_LIB_GAME
                " and .setup there to unpack it again.",
                dcr_game_root());
  return 0;
}

/* DemoActivity's static initialiser: System.loadLibrary("ssasr") -- the
 * engine's constructors now, its JNI_OnLoad and the activity in ssr_boot.c */
void port_run(void) {
  ssr_run_constructors();
  ssr_boot_run();
}
