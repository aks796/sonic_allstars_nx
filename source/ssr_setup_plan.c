/* ssr_setup_plan.c -- Sonic & SEGA All-Stars Racing's part of the first
 * launch: its setup plan for the runtime's dcr_setup.c.
 *
 * The bar, in permille of the whole first launch (as the port's own had it):
 *    50- 750  libssasr.so unpacked from the APK (by bytes)
 *   750- 950  the Java class list
 *        1000 the game starts
 * An NRO update stands at 500 while it restarts (RT_SETUP_UPDATE_PERMILLE).
 * The game's data is never unpacked: the expansion file is read in place
 * (ssr_pack.h).
 *
 * .setup keys, which must not change (or every player unpacks again once):
 * libssasr.so, classes.txt. MIT.
 */
#include "config.h"
#include "dcr_setup.h"

static const char *const k_libs[] = {SSR_LIB_GAME};

const RtSetupPlan port_setup_plan = {
    .libs = k_libs,
    .nlibs = 1,
    .libs_what = "Unpacking the game's library",
    .apk_requirement = "This port needs Sonic & SEGA All-Stars Racing 1.0.1 (com.sega.ssasr,\n"
                       "armeabi): use the APK of that version.",
    .libs_p0 = 50,
    .libs_p1 = 750,
    .classes_p0 = 750,
    .classes_p1 = 950,
};
