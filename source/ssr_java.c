/* ssr_java.c -- the Java side of Sonic & SEGA All-Stars Racing, answered in C.
 *
 * The game's Java (com.sega.ssasr: DemoActivity, DemoGLSurfaceView,
 * DemoRenderer, e, NoahWrapper, and 2013's SDKs) does not run here. The
 * engine calls about 70 "java*" methods on the DemoRenderer object it is
 * handed (nativeProjectInit's thiz), and OpenAL Soft uses a Java
 * android.media.AudioTrack. Each handler below does what the decompiled Java
 * does (../source/java-reference), or what it does on a phone with no
 * network, no store and no Google / SEGA / Facebook account:
 *   saves        javaReadRMS / javaWriteRMS: files in the app's files dir
 *                (<root>/data/files), as openFileInput / openFileOutput
 *   music        javaPlayMusic & co: the MediaPlayer of DemoActivity (ssr_audio.c)
 *   text         javaImageText*: com.sega.ssasr.e (ssr_text.c)
 *   rumble       javaVibrate
 *   the movie    javaIsMovieStopped: the splash and the intro (ssr_boot.c)
 *   online       Play Games, SEGA ID, the store, ads, analytics: not signed
 *                in / not available, as the Java behaves without them
 * Unhandled calls return 0 / false / null and are logged once (jni_core.c).
 * MIT.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "dcr_config.h"
#include "jni.h"
#include "ssr.h"
#include "util.h"

const char *dcr_game_root(void); /* dcr_path.c */
int ssr_gfx_download_bg_texture(void); /* ssr_gfx.c */
int ssr_boot_movie_stopped(void);      /* ssr_boot.c */

#define S "Ljava/lang/String;"
#define H(fn) static jvalue fn(JObj *self, const jvalue *a, const JMethod *m)

JObj *g_activity, *g_renderer, *g_view;

void *ssr_class(const char *name) { return jni_class(name)->obj; }

/* ================================================================= misc */
H(h_void) { return jv_none(); }
H(h_false) { return jv_z(0); }
H(h_true) { return jv_z(1); }
H(h_zero) { return jv_i(0); }
H(h_minus1) { return jv_i(-1); }
H(h_null) { return jv_l(NULL); }

static const char *arg_str(const jvalue *a, int i) { return jni_utf(a[i].l); }

H(h_log1) {
  static int n;
  if (n++ < 32)
    debugPrintf("[java] %s(%s)\n", m->name, arg_str(a, 0));
  return jv_none();
}

/* ================================================================ saves */
/* openFileInput / openFileOutput take a bare name (no separators) in the
 * app's files dir. */
static int save_path(char *out, size_t cap, const char *name) {
  if (!name || !*name || strchr(name, '/') || strchr(name, '\\'))
    return -1;
  snprintf(out, cap, "%s/data/files/%s", dcr_game_root(), name);
  return 0;
}

H(h_readRMS) {
  const char *name = arg_str(a, 0);
  char path[512];
  if (save_path(path, sizeof path, name) != 0) {
    debugPrintf("[save] javaReadRMS(%s): not a file name\n", name);
    return jv_l(NULL);
  }
  FILE *f = fopen(path, "rb");
  if (!f)
    return jv_l(NULL); /* FileNotFoundException -> null */
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  JObj *arr = jni_array('B', n > 0 ? (jsize)n : 0);
  if (arr && n > 0 && fread(arr->a.data, 1, (size_t)n, f) != (size_t)n)
    debugPrintf("[save] javaReadRMS(%s): short read\n", name);
  fclose(f);
  return jv_l(arr);
}

static int write_rms(const char *name, JObj *arr, int len) {
  char path[512], tmp[520];
  if (save_path(path, sizeof path, name) != 0 || !arr || arr->kind != JK_ARRAY || len < 0 || len > arr->a.len)
    return 0;
  /* written whole or not at all: the old save stays until the new one is complete */
  snprintf(tmp, sizeof tmp, "%s.part", path);
  FILE *f = fopen(tmp, "wb");
  int ok = f && (len == 0 || fwrite(arr->a.data, 1, (size_t)len, f) == (size_t)len);
  if (f && fclose(f) != 0)
    ok = 0;
  if (ok) {
    unlink(path);
    ok = rename(tmp, path) == 0;
  }
  if (!ok) {
    unlink(tmp);
    debugPrintf("[save] javaWriteRMS(%s, %d bytes) FAILED: %s\n", name, len, strerror(errno));
  }
  return ok;
}

H(h_writeRMS) { return jv_z(write_rms(arg_str(a, 0), a[1].l, a[2].i)); }

/* javaWriteRMSLater: the Java kept the data for a later write that its code
 * never makes; written at once here, so nothing is lost. */
H(h_writeRMSLater) { return jv_z(write_rms(arg_str(a, 0), a[1].l, a[2].i)); }

/* ================================================================ music */
/* the volume is 0..100 (STAudio can pass more: clamped) */
static float music_vol(int v) { return v < 0 ? 0.0f : v > 100 ? 1.0f : (float)v / 100.0f; }

/* Split screen's second copy of the engine (ssr_split.c) asks for the same
 * music as the first: the first's is played, the second's calls ignored. */
int ssr_engine_current(void); /* ssr_loader.c */
#define FIRST_ENGINE_ONLY()        \
  do {                             \
    if (ssr_engine_current() != 0) \
      return jv_none();            \
  } while (0)

H(h_playMusic) {
  FIRST_ENGINE_ONLY();
  ssr_music_play(a[0].i, music_vol(a[1].i), a[2].z);
  return jv_none();
}
H(h_stopMusic) {
  FIRST_ENGINE_ONLY();
  ssr_music_stop();
  return jv_none();
}
H(h_pauseMusic) {
  FIRST_ENGINE_ONLY();
  ssr_music_pause();
  return jv_none();
}
H(h_unpauseMusic) {
  FIRST_ENGINE_ONLY();
  ssr_music_unpause();
  return jv_none();
}
H(h_setMusicVolume) {
  FIRST_ENGINE_ONLY();
  ssr_music_volume(music_vol(a[0].i));
  return jv_none();
}

/* ================================================================= text */
/* (the second copy's: LAN play never draws Java text -- MULTIPLAYER.md 4.2 --
 * and one texture is the first's) */
H(h_textInit) {
  FIRST_ENGINE_ONLY();
  ssr_text_init_own();
  return jv_none();
}
H(h_textInitTex) {
  FIRST_ENGINE_ONLY();
  ssr_text_init_tex(a[0].i);
  return jv_none();
}
H(h_textSet) {
  FIRST_ENGINE_ONLY();
  ssr_text_set(a[0].i, arg_str(a, 1));
  return jv_none();
}
H(h_textBake) { return jv_i(ssr_engine_current() ? 0 : ssr_text_bake()); }
H(h_textTex) { return jv_i(ssr_engine_current() ? 0 : ssr_text_tex()); }
H(h_textTerm) {
  FIRST_ENGINE_ONLY();
  ssr_text_terminate();
  return jv_none();
}

/* ============================================================== the LAN */
/* The game's LOCAL multiplayer, between the engine's two copies (ssr_net.c):
 * each its own address; "connected" while its network session exists (the
 * lobby counts a "no" as a lost connection). */
int ssr_net_my_ip(void);
int ssr_net_broadcast_ip(void);
void *ssr_native_in(int engine, const char *symbol); /* ssr_loader.c */
H(h_netConnected) {
  void **inst = ssr_native_in(ssr_engine_current(), "_ZN14NetworkAndroid11ms_InstanceE");
  return jv_z(inst && *inst);
}
H(h_myIP) { return jv_i(ssr_net_my_ip()); }
H(h_bcastIP) { return jv_i(ssr_net_broadcast_ip()); }

/* ============================================================== device */
H(h_vibrate) {
  ssr_input_rumble(a[0].i);
  return jv_none();
}
H(h_osVersion) { return jv_l(jni_str("4.1.2")); }
H(h_package) { return jv_l(jni_str(PORT_PACKAGE)); }
H(h_appVersion) { return jv_l(jni_str(SSR_VERSION)); }
H(h_movieStopped) { return jv_z(ssr_engine_current() ? 1 : ssr_boot_movie_stopped()); }
H(h_downloadBg) { return jv_i(ssr_gfx_download_bg_texture()); }
H(h_webView) {
  debugPrintf("[java] javaWebView(%s): no browser here\n", arg_str(a, 0));
  return jv_none();
}

/* ================================================================= store */
H(h_iapPrice) { return jv_l(jni_str("")); }
H(h_iapBuy) {
  debugPrintf("[java] javaMakeIAPPurchase(%d): the store is not available\n", a[0].i);
  return jv_none();
}

/* nativeGameServicesSaveGame makes a 0x605-byte array on every save and
 * never deletes it (NATIVE_CONTRACT.md 8.1 #53): Play Games is not there,
 * so the array is dropped here. */
H(h_saveGame) {
  if (a[0].l)
    jni_release(a[0].l);
  return jv_none();
}

/* ============================================================== the table */
#define R C_RENDERER
#define N C_NOAH
const JMethodDef jni_method_defs[] = {
    /* ---- DemoRenderer: saves ---- */
    {R, "javaReadRMS", "(" S ")[B", h_readRMS},
    {R, "javaWriteRMS", "(" S "[BI)Z", h_writeRMS},
    {R, "javaWriteRMSLater", "(" S "[BI)Z", h_writeRMSLater},

    /* ---- sound (the Java's SoundPool methods are empty: OpenAL plays the effects) ---- */
    {R, "javaLoadSound", "()V", h_void},
    {R, "javaPlaySound", "(IIII)V", h_void},
    {R, "javaSoundStop", "(I)V", h_void},
    {R, "javaSoundStopAll", "()V", h_void},
    {R, "javaSoundVol", "(II)V", h_void},
    {R, "javaPauseAudio", "()V", h_void},
    {R, "javaResumeAudio", "()V", h_void},
    {R, "javaPlayMusic", "(IIZ)V", h_playMusic},
    {R, "javaStopMusic", "()V", h_stopMusic},
    {R, "javaPauseMusic", "()V", h_pauseMusic},
    {R, "javaUnpauseMusic", "()V", h_unpauseMusic},
    {R, "javaSetMusicVolume", "(I)V", h_setMusicVolume},

    /* ---- the movie, the loading views ---- */
    {R, "javaPlayMovie", "(II)V", h_void},
    {R, "javaIsMovieStopped", "()Z", h_movieStopped},
    {R, "javaLoadDownloadBGTexture", "()I", h_downloadBg},
    {R, "javaRemoveLoadingView", "()V", h_void},

    /* ---- text (com.sega.ssasr.e) ---- */
    {R, "javaImageTextInitialise", "()V", h_textInit},
    {R, "javaImageTextInitialise", "(I)V", h_textInitTex},
    {R, "javaImageTextSetText", "(I" S ")V", h_textSet},
    {R, "javaImageTextBakeToTexture", "()I", h_textBake},
    {R, "javaImageTextGetTextureId", "()I", h_textTex},
    {R, "javaImageTextTerminate", "()V", h_textTerm},

    /* ---- the device ---- */
    {R, "javaVibrate", "(I)V", h_vibrate},
    {R, "javaGetOSVersion", "()" S, h_osVersion},
    {R, "javaGetPackage", "()" S, h_package},
    {R, "javaGetAppVersion", "()" S, h_appVersion},
    {R, "javaWebView", "(" S ")V", h_webView},

    /* ---- the LAN: the engine's two copies (split screen, ssr_net.c) ---- */
    {R, "javaNetworkConnected", "()Z", h_netConnected},
    {R, "javaGetMyIPAddress", "()I", h_myIP},
    {R, "javaGetBroadcastIP", "()I", h_bcastIP},
    {R, "javaMulticastLock", "(Z)V", h_void},

    /* ---- the store (DDStore / in-app billing): not available ---- */
    {R, "javaGetIAPPriceString", "(I)" S, h_iapPrice},
    {R, "javaMakeIAPPurchase", "(I)V", h_iapBuy},
    {R, "javaIsIAPInProgress", "()Z", h_false},
    {R, "javaDidIAPSucceed", "()Z", h_false},
    {R, "javaIsIAPInYen", "()Z", h_false},

    /* ---- SEGA ID (the SEGA Network SDK): opted out, so no login prompt ---- */
    {R, "javaSegaSDKFBLogin", "()V", h_void},
    {R, "javaSegaSDKHandleAnonymousPlayer", "()V", h_void},
    {R, "javaSegaSDKIsPlayerLoggedIn", "()Z", h_false},
    {R, "javaSegaSDKIsPlayerOptOutOrAlreadyLoggedIn", "()Z", h_true},
    {R, "javaSegaSDKMoreGames", "()V", h_void},

    /* ---- analytics and ads ---- */
    {R, "javaFlurryEventBasic", NULL, h_void},
    {R, "javaFlurryEventSingle", NULL, h_void},
    {R, "javaFlurryEventTriple", NULL, h_void},
    {R, "javaPlayHavenShowAdvert", "(" S ")V", h_void},
    {R, "javaTapjoyShowOffers", "()V", h_void},
    {R, "javaTapjoyShowFullScreenAd", "()V", h_void},
    {R, "javaTapjoyCheckForEarnedPoints", "()V", h_void},
    {R, "javaShowChartBoost", "()V", h_void},

    /* ---- Google Play Games: not signed in (the Java's answers without it) ---- */
    {R, "javaGameServicesSignIn", "()V", h_log1},
    {R, "javaGameServicesSignOut", "()V", h_void},
    {R, "javaGameServicesIsSignedIn", "()Z", h_false},
    {R, "javaGameServicesGetName", "()" S, h_null},
    {R, "javaGameServicesSaveGame", "([B)V", h_saveGame},
    {R, "javaGameServicesLoadGame", "()V", h_void},
    {R, "javaGameServicesLoadAchievements", "()Z", h_false},
    {R, "javaGameServicesIncrementAchievement", "(" S "I)Z", h_false},
    {R, "javaGameServicesAwardAchievement", "(" S ")Z", h_false},
    {R, "javaGameServicesLoadLeaderboard", "(" S "II)Z", h_false},
    {R, "javaGameServicesLoadLeaderboardPage", "(I)Z", h_false},
    {R, "javaGameServicesCloseLeaderboard", "()V", h_void},
    {R, "javaGameServicesSendScore", "(" S "J)Z", h_false},
    {R, "javaGameServicesShowLeaderboard", "(" S ")Z", h_false},
    {R, "javaGameServicesShowPlayerSelect", "(I)Z", h_false},
    {R, "javaGameServicesJoinGame", "(I)Z", h_false},
    {R, "javaGameServicesSendPacket", "([BI[" S "Z)Z", h_false},
    {R, "javaGameServicesLeaveGame", "()V", h_void},
    {R, "javaGameServicesAcceptInvitation", "(" S ")V", h_void},

    /* ---- OpenFeint (long gone; the Java answers "off") ---- */
    {R, "javaOpenFeintEnabled", "()Z", h_false},
    {R, "javaOpenFeintInit", "()V", h_void},
    {R, "javaSetupOpenFeint", "()V", h_void},
    {R, "javaOpenDashBoard", "()V", h_void},
    {R, "javaOpenLeaderboard", "(" S ")V", h_void},
    {R, "javaSubmitScore", "(" S "I)V", h_void},
    {R, "javaLoggedIn", "()Z", h_false},
    {R, "javaOFGetNewScores", "()Z", h_false},
    {R, "javaOFGetReplaceType", "()Z", h_false},
    {R, "javaOFUserChanged", "()Z", h_false},
    {R, "javaGetScoresPoll", "([I)I", h_minus1},
    {R, "getLeaderboardScore", "(I)V", h_void},
    {R, "ddStoreBuy", "(I)V", h_iapBuy},

    /* ---- NoahWrapper (Noah, the Japanese offer wall): not there ---- */
    {N, "javaShowOffers", NULL, h_void},
    {N, "javaShowBanner", NULL, h_void},
    {N, "javaCloseBanner", NULL, h_void},
    {N, "javaShowFullScreenAd", NULL, h_void},
    {N, "javaSetBannerStatus", NULL, h_void},
    {N, "javaSetConnectionStatus", NULL, h_void},
    {N, "javaCollectPoints", NULL, h_void},
    {N, "javaConnect", NULL, h_void},
    {N, "javaSetUID", NULL, h_void},
    {N, "javaOfferFlag", NULL, h_zero},
    {N, "javaRewardFlag", NULL, h_zero},
    {N, "javaSetLicenceChosen", NULL, h_void},
    {N, "javaIsLicenceChosen", NULL, h_false},
    {N, "javaIsBannerShowing", NULL, h_false},
    {N, "javaIsConnected", NULL, h_false},
    {N, "javaCashInPoints", NULL, h_void},
    {N, "javaCurrentPointsPending", NULL, h_zero},
    {N, "javaSetPointsPending", NULL, h_void},

    /* ---- android.media.AudioTrack, for OpenAL Soft (ssr_audio.c) ---- */
    {C_AUDIOTRACK, "getMinBufferSize", "(III)I", ssr_at_min_buffer},
    {C_AUDIOTRACK, "<init>", "(IIIIII)V", ssr_at_init},
    {C_AUDIOTRACK, "play", "()V", ssr_at_play},
    {C_AUDIOTRACK, "stop", "()V", ssr_at_stop},
    {C_AUDIOTRACK, "pause", "()V", ssr_at_pause},
    {C_AUDIOTRACK, "flush", "()V", ssr_at_flush},
    {C_AUDIOTRACK, "release", "()V", ssr_at_release},
    {C_AUDIOTRACK, "write", "([BII)I", ssr_at_write},
    {C_AUDIOTRACK, "getPlayState", "()I", ssr_at_play_state},
    {C_AUDIOTRACK, "getState", "()I", ssr_at_state},
    {C_AUDIOTRACK, "getSampleRate", "()I", ssr_at_rate},
    {C_AUDIOTRACK, "setStereoVolume", "(FF)I", ssr_at_stereo_volume},
    {C_AUDIOTRACK, "setPlaybackHeadPosition", "(I)I", ssr_at_zero},

    {NULL, NULL, NULL, NULL},
};

const JFieldDef jni_field_defs[] = {
    {NULL, NULL, NULL, 0, NULL},
};

/* The app's classes derive from the framework's, so a method the engine asks
 * of one of them is found on its superclass too (jni_core.c). */
const char *const jni_class_supers[][2] = {
    {C_ACTIVITY, "com/google/example/games/basegameutils/BaseGameActivity"},
    {"com/google/example/games/basegameutils/BaseGameActivity", "android/support/v4/app/FragmentActivity"},
    {"android/support/v4/app/FragmentActivity", "android/app/Activity"},
    {"android/app/Activity", "android/content/Context"},
    {C_VIEW, "android/opengl/GLSurfaceView"},
    {"android/opengl/GLSurfaceView", "android/view/SurfaceView"},
    {"android/view/SurfaceView", "android/view/View"},
    {NULL, NULL},
};
const char *const jni_missing_classes[] = {NULL};

/* ------------------------------------------------------------------ init */
static int g_lang = 0;

int ssr_language(void) { return g_lang; }

/* DemoActivity.onCreate's language: the phone's locale -> 0 en (GB), 1 fr,
 * 2 it, 3 de, 4 es, 5 ja, 6 en (US); [game] language overrides it. */
static int console_language(void) {
  u64 code = 0;
  SetLanguage lang = SetLanguage_ENUS;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&code)))
      setMakeLanguage(code, &lang);
    setExit();
  }
  switch (lang) {
  case SetLanguage_FR:
  case SetLanguage_FRCA: return 1;
  case SetLanguage_IT: return 2;
  case SetLanguage_DE: return 3;
  case SetLanguage_ES:
  case SetLanguage_ES419: return 4;
  case SetLanguage_JA: return 5;
  case SetLanguage_ENUS: return 6;
  default: return 0;
  }
}

void ssr_java_init(void) {
  jni_init(); /* g_jni_log: [debug] log_java_calls (jni_core.c) */
  g_lang = dcr_config()->language >= 0 ? dcr_config()->language : console_language();
  static const char *const names[] = {"English (UK)", "French", "Italian", "German", "Spanish", "Japanese",
                                      "English (US)"};
  debugPrintf("[java] language %d: %s\n", g_lang, names[g_lang]);
  g_activity = jni_singleton(C_ACTIVITY);
  g_view = jni_singleton(C_VIEW);
  g_renderer = jni_singleton(C_RENDERER);
}
