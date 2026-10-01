/* port_config.h -- Sonic & SEGA All-Stars Racing's settings for the android32
 * runtime.
 *
 * Macros only: the runtime's C files, its assembly and the launcher all read
 * this (runtime/source/rt_settings.h). What each setting does is next to its
 * default in the runtime; runtime/docs/ lists them all. MIT.
 */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

/* ------------------------------------------------------------------ the game */
#define PORT_TITLE    "Sonic & SEGA All-Stars Racing"
#define PORT_NAME     "sonic_allstars_nx"
#define PORT_PACKAGE  "com.sega.ssasr"
#define PORT_BANNER   "sonicracing_nx: Sonic & SEGA All-Stars Racing (Sumo Digital engine, armeabi)"
/* the folder's name before the rename (moved in on the first start) */
#define PORT_OLD_ROOT_PATHS "/switch/sonicracing"
#define PORT_ABI_DIR  "lib/armeabi/"
/* libssasr.so 1.0.1: highest p_vaddr+p_memsz = 0x734744 (~7.2 MB, 5 MB of
 * it .bss) */
#define PORT_SO_REGION_BYTES (16u * 1024 * 1024)

/* The APK, by what is in it (any name; it is not renamed). */
#define PORT_APK_DESC "Sonic & SEGA All-Stars Racing 1.0.1 (com.sega.ssasr, armeabi)"
#define PORT_APK_ROLES                                                             \
  {.what = "the game", .name = "game.apk",                                       \
   .need = (const char *const[]){"lib/armeabi/libssasr.so", NULL}, .flags = RT_APK_ANY_FILE}
#define PORT_LAUNCHER_START_NOTE "(the first start unpacks the game's library from the APK)"

/* ------------------------------------------------------------------ frames */
#define RT_BOOST_WATCH_THREAD 1 /* the engine's frames block the main thread: a watcher boosts */
#define RT_GL_BLIT            1 /* the button prompts, the split-screen bars (GLES 1 overlays) */
#define RT_PAD_MAX_PLAYERS    2

/* ------------------------------------------------------------------ setup */
#define RT_SETUP_UPDATE_PERMILLE 500

#endif
