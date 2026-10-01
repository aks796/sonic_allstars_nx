/* config.h -- Sonic & SEGA All-Stars Racing's own constants.
 *
 * Sonic & SEGA All-Stars Racing (com.sega.ssasr 1.0.1, versionCode 20),
 * armeabi: Sumo Digital's engine, ported to Android by Distinctive
 * Developments, in one native library (libssasr.so, GLES 1.x, OpenAL Soft
 * built in). Its data is the D0 pack packres.png inside the expansion file
 * main.20.com.sega.ssasr.obb. The folder, the package and the module region
 * are the runtime's settings (port_config.h). MIT.
 */
#ifndef SSR_CONFIG_H
#define SSR_CONFIG_H

#include "rt_settings.h"

#define SSR_LIB_GAME    "libssasr.so"
#define SSR_ABI_DIR     PORT_ABI_DIR
#define SSR_VERSION     "1.0.1"
#define SSR_VERSION_CODE 20
#define SSR_OBB_NAME    "main.20.com.sega.ssasr.obb" /* the user's own expansion file */

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

#endif /* SSR_CONFIG_H */
