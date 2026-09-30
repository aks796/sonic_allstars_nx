/* config.h -- build-wide constants for the Sonic & SEGA All-Stars Racing
 * Switch wrapper.
 *
 * Sonic & SEGA All-Stars Racing (com.sega.ssasr 1.0.1, versionCode 20),
 * armeabi: Sumo Digital's engine, ported to Android by Distinctive
 * Developments, in one native library (libssasr.so, GLES 1.x, OpenAL Soft
 * built in). Its data is the D0 pack packres.png inside the expansion file
 * main.20.com.sega.ssasr.obb. AArch32 host process built against libnx32
 * (see the Makefile). MIT.
 */
#ifndef DCR_CONFIG_H
#define DCR_CONFIG_H

/* Where on the SD card the game files live (sonic_allstars_nx.nro and the
 * player's files; the folder was /switch/sonicracing before: dcr_migrate.h
 * moves it), and the module name. The player's APK may have any name: it is
 * the file that holds lib/armeabi/libssasr.so (ssr_pack.h). */
#define DCR_ROOT_PATH   "/switch/sonic_allstars_nx"
#define SSR_LIB_GAME    "libssasr.so"
#define SSR_ABI_DIR     "lib/armeabi/"
#define DCR_PACKAGE     "com.sega.ssasr"
#define SSR_VERSION     "1.0.1"
#define SSR_VERSION_CODE 20
#define SSR_OBB_NAME    "main.20.com.sega.ssasr.obb" /* the user's own expansion file */
#define SSR_NSP_NAME    "sonicracing_nx.nsp"         /* in the launcher's romfs */

/* The reserved region the game module is mapped into. libssasr.so 1.0.1:
 * highest p_vaddr+p_memsz = 0x734744 (~7.2 MB, 5 MB of it .bss). */
#define SO_REGION_BYTES (16u * 1024 * 1024)

/* Left outside the heap for kernel-side allocations. GPU buffers come from
 * the heap (libdrm_nouveau memaligns them and hands them to nvmap). */
#define GFX_RESERVE_MB  16u

/* Default window size (config.ini [display] resolution changes it). The
 * game is landscape and lays itself out for any size it is given
 * (DemoRenderer.onSurfaceChanged -> nativeSetScreenSize). */
#define DCR_FORCE_SCREEN_W 1280
#define DCR_FORCE_SCREEN_H 720

/* What DemoActivity reports of the device (nativeSetDeviceMakeModel:
 * Build.MANUFACTURER, Build.MODEL; see ../source/docs/NATIVE_CONTRACT.md for
 * what the engine does with them). */
#define SSR_DEVICE_MAKE  "Nintendo"
#define SSR_DEVICE_MODEL "Switch"

/* DemoRenderer.nativeProjectInit(Runtime.maxMemory(), Runtime.totalMemory()):
 * the Dalvik heap's limit and its size at that moment, as a phone of 2013
 * with a large heap reported them. */
#define SSR_MAX_MEMORY   (256 * 1024 * 1024)
#define SSR_TOTAL_MEMORY (16 * 1024 * 1024)

#define DEBUG_LOG 1

/* The renderer: 1 = mesa/nouveau (gl_mesa.c, portlibs32/ from
 * mesa32's build), 0 = null GL (gl_null.c: runs the game, draws
 * nothing). Set by the Makefile. */
#ifndef DCR_GL_MESA
#define DCR_GL_MESA 0
#endif

#endif /* DCR_CONFIG_H */
