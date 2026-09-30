/* main.c -- boot sequence for the Sonic & SEGA All-Stars Racing wrapper (32-bit).
 *
 * The order here matters; each step says why it is where it is. MIT.
 */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "dcr_config.h"
#include "dcr_migrate.h"
#include "dcr_manifest.h"
#include "dcr_path.h"
#include "dcr_sched.h"
#include "dcr_time.h"
#include "error.h"
#include "nx_init.h"
#include "selfproc.h"
#include "so_util.h"
#include "ssr.h"
#include "util.h"

void dcr_setup_update_from_nro(void); /* dcr_setup.c */
void dcr_setup_from_apk(const char *apk);

static char g_root[256] = "sdmc:" DCR_ROOT_PATH;
const char *dcr_game_root(void) { return g_root; }

/* the player's APK, whatever it is called (ssr_find_apk: by its contents) */
static char g_apk[768];
const char *dcr_apk_path(void) { return g_apk; }

extern volatile uint32_t __dcr_reloc_path __attribute__((visibility("hidden")));

static void report_boot(void) {
  const u64 MB = 1024 * 1024;
  debugPrintf("[boot] === sonicracing_nx: Sonic & SEGA All-Stars Racing (Sumo Digital engine, armeabi) ===\n");
  static const char *const paths[] = {"none needed", "patched through a writable alias (hardware)",
                                      "direct writes (emulator: pseudo-handle refused)"};
  debugPrintf("[boot] text relocations: %s\n", __dcr_reloc_path < 3 ? paths[__dcr_reloc_path] : "?");
  debugPrintf("[heap] total %u MB, used %u MB at start, heap region %u MB, heap %u MB @ %p\n",
              (unsigned)(g_nxinit.total / MB), (unsigned)(g_nxinit.used / MB), (unsigned)(g_nxinit.heap_region / MB),
              (unsigned)(g_nxinit.heap / MB), (void *)g_nxinit.heap_base);
  debugPrintf("[svc] sm=%x applet=%x hid=%x time=%x fs=%x sdmc=%x\n", g_nxinit.rc_sm, g_nxinit.rc_applet,
              g_nxinit.rc_hid, g_nxinit.rc_time, g_nxinit.rc_fs, g_nxinit.rc_sdmc);
  if (R_FAILED(g_nxinit.rc_time))
    debugPrintf("[svc] time service unavailable: clocks fall back to the system tick\n");
}

int main(int argc, char *argv[]) {
  /* the folder's old name: its files moved here first (dcr_migrate.h) */
  DcrMigrate mig;
  dcr_migrate(DCR_OLD_DIR, g_root, &mig);
  mkdir(g_root, 0777);
  log_init(g_root);
  if (mig.moved || mig.failed)
    debugPrintf("[setup] the game folder is %s now: moved %d item(s) from %s (%s)%s%s\n", g_root, mig.moved,
                DCR_OLD_DIR, mig.names, mig.kept ? "; some were here already and stay in the old folder" : "",
                mig.failed ? "; some could not be moved" : "");
  if (dcr_is_emulator()) {
    int dcr_emu_fix_self(void); /* emu_fixups.c: before any of Mesa runs */
    dcr_emu_fix_self();
  }
  log_console_open(); /* blank: text only when asked or for setup work */
  report_boot();

  if (chdir(g_root) != 0)
    debugPrintf("[boot] WARNING: chdir(%s) failed\n", g_root);
  dcr_config_load(); /* config.ini: controls, display, sound, boost */
  void dcr_boost_launch_begin(void);
  dcr_boost_launch_begin(); /* CPU at 1785 MHz until the first picture (dcr_boost.c) */
  if (dcr_config()->boot_log)
    log_console_show_text();
  dcr_time_init();
  dcr_path_prepare_dirs();

  /* A newer build of this program in the launcher NRO: install it and
   * restart into it before anything else happens (dcr_setup.c). */
  dcr_setup_update_from_nro();

  /* the APK: any file in the game folder that holds the game's library */
  char *apk = g_apk;
  if (ssr_find_apk(g_root, g_apk, sizeof g_apk) != 0)
    fatal_error("The game's APK is missing.\n\n"
                "Copy the APK of your own Sonic & SEGA All-Stars Racing\n"
                "(com.sega.ssasr 1.0.1, armeabi) to %s:\n"
                "any name will do. The game's library, music and pictures\n"
                "come from it.",
                g_root);
  void dcr_apkcache_set_path(const char *real);
  dcr_apkcache_set_path(apk);
  if (dcr_manifest_load(apk) != 0)
    fatal_error("%s is unreadable.\n\n"
                "Copy the APK of your own Sonic & SEGA All-Stars Racing\n"
                "(com.sega.ssasr 1.0.1) again: the game's library,\n"
                "music and pictures come from it.",
                apk);
  if (strcmp(dcr_manifest_package(), DCR_PACKAGE))
    debugPrintf("[boot] WARNING: the APK is %s, not %s\n", dcr_manifest_package(), DCR_PACKAGE);
  debugPrintf("[boot] the APK: %s, %s %s (%d)\n", strrchr(apk, '/') ? strrchr(apk, '/') + 1 : apk,
              dcr_manifest_package(), dcr_manifest_version_name(), dcr_manifest_version_code());

  if (dcr_self_process() == INVALID_HANDLE)
    fatal_error("Could not obtain a handle to this process.\n"
                "The loader needs it to map the game's code.");

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
                g_root);
  if (ssr_load_module() != 0)
    fatal_error("Could not load the game library from %s.\n\n"
                "It is unpacked from the APK (" SSR_ABI_DIR ") on launch: delete\n" SSR_LIB_GAME
                " and .setup there to unpack it again.",
                g_root);

  /* The main thread becomes a guest thread like the game's own: priority
   * 59 on cores 0-2, where the kernel time-slices (dcr_sched.c). */
  dcr_sched_init();
  {
    ssr_audio_selftest();
    void dcr_pthread_selftest(void);
    dcr_pthread_selftest();
    void dcr_io_selftest(void);
    dcr_io_selftest();
  }
#if DCR_GL_MESA
  if (dcr_is_emulator() || dcr_config()->gl_selftest) {
    int dcr_gl_selftest(void);
    dcr_gl_selftest();
  }
#endif

  /* DemoActivity's static initialiser: System.loadLibrary("ssasr") -- the
   * engine's constructors now, its JNI_OnLoad and the activity in ssr_boot.c */
  ssr_run_constructors();
  ssr_boot_run();
  debugPrintf("[boot] exiting\n");
  log_flush_ring();
  return 0;
}
