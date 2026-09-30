/* ssr_boot.c -- the app: what DemoActivity, DemoGLSurfaceView and
 * DemoRenderer did, and the frame loop.
 *
 * Android's order (../source/java-reference/com/sega/ssasr), and what
 * happens here:
 *   DemoActivity <clinit>   System.loadLibrary("ssasr"): the engine's
 *                           constructors (main.c), JNI_OnLoad (OpenAL Soft's:
 *                           it keeps the JavaVM)
 *   DemoActivity.onCreate   nativeSetDeviceID; nativeProjectCfg(1, keyboard),
 *                           (4, trackball); DemoGLSurfaceView's constructor:
 *                           nativeProjectOneTimeInit; nativeReadCPUInfo;
 *                           nativeSetDeviceMakeModel; nativeScreenSizeInit;
 *                           the language; the expansion file is there:
 *                           nativeProjectCfg(6, 1) and nativeSetFileSystem
 *                           (its path, the data dir, packres.png's offset and
 *                           length); nativeProjectCfg(5, language)
 *   onResume                DemoRenderer.nativeResume
 *   the GL thread           onSurfaceCreated: glDepthFunc(GL_LEQUAL);
 *                           onSurfaceChanged: nativeSetScreenSize(w, h, 0, 0);
 *                           onDrawFrame: the splash views' state machine
 *                           (updateLoading) runs; the engine's frame is
 *                           skipped while a picture fades; from the 7th frame
 *                           nativeProjectInit(maxMemory, totalMemory) once,
 *                           then nativeProjectRun(focus) each frame, 1 = quit
 *   touches, keys, sensor   DemoGLSurfaceView (ssr_input.c)
 *   onPause                 nativeSaveState, nativeSetPause, the touches
 *                           cancelled, the music paused
 *   leaving                 nativeSaveState, nativeExit
 * One thread does it all, as the GL thread did for the engine: the
 * controls, the engine's frame, the overlays (the splash pictures, the intro
 * movie), the present. The engine's own threads (its decompressor, OpenAL's
 * mixer) run beside it, and the port's sound mixer (ssr_audio.c).
 *
 * HOME: onPause's calls, the sound held; back: onResume. Android lost the GL
 * context on the way (onSurfaceCreated again: nativeProjectInit again,
 * DDGLRefresh re-uploading every texture); here the context stays. MIT.
 */
#include <GLES/gl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "dcr_config.h"
#include "dcr_time.h"
#include "error.h"
#include "gl_layer.h"
#include "jni.h"
#include "ssr.h"
#include "util.h"

void dcr_watchdog_start(void);
void dcr_boost_poll(void);
void dcr_boost_report(void);
void dcr_boost_launch_end(void);
void dcr_boost_idle(void);
void dcr_boost_frame_begin(void);
void ssr_perf_focus(int focused); /* ssr_perf.c */
void ssr_perf_exit(void);
void ssr_perf_frame(u64 run_ticks, u64 present_ticks, unsigned polls);
void ssr_perf_report(int target_fps);
void ssr_patch_after_make_model(void); /* ssr_patch.c */
void ssr_patch_after_init(void);
void ssr_patch_report(void);
void ssr_clock_resync(void);
void dcr_apkcache_report(void);

/* ------------------------------------------------------------ natives */
typedef jint (*fn_onload)(void *vm, void *reserved);
typedef void (*fn_v)(void *env, void *cls);
typedef void (*fn_z)(void *env, void *cls, jboolean z);
typedef void (*fn_ii)(void *env, void *cls, jint a, jint b);
typedef void (*fn_iiii)(void *env, void *cls, jint a, jint b, jint c, jint d);
typedef jint (*fn_ri)(void *env, void *cls, jint a);
typedef void (*fn_s)(void *env, void *cls, void *s);
typedef void (*fn_ss)(void *env, void *cls, void *a, void *b);
typedef jboolean (*fn_ssii)(void *env, void *cls, void *a, void *b, jint c, jint d);

#define NAT(cls, name) "Java_com_sega_ssasr_" cls "_" name

static void *need(const char *symbol) {
  void *p = ssr_native(symbol);
  if (!p)
    debugPrintf("[boot] MISSING native %s\n", symbol);
  return p;
}

static struct {
  fn_ii cfg;                 /* DemoActivity.nativeProjectCfg(int, int) static */
  fn_s set_device_id;        /* DemoActivity.nativeSetDeviceID(String) static */
  fn_v one_time_init;        /* DemoGLSurfaceView.nativeProjectOneTimeInit() static */
  fn_z read_cpu_info;        /* DemoActivity.nativeReadCPUInfo(boolean) static */
  fn_ss set_make_model;      /* DemoActivity.nativeSetDeviceMakeModel(String, String) (instance) */
  fn_v screen_size_init;     /* DemoActivity.nativeScreenSizeInit() static */
  fn_ssii set_file_system;   /* DemoActivity.nativeSetFileSystem(String, String, int, int) static */
  fn_iiii set_screen_size;   /* DemoRenderer.nativeSetScreenSize(int, int, int, int) */
  fn_ii project_init;        /* DemoRenderer.nativeProjectInit(int maxMemory, int totalMemory) */
  fn_ri project_run;         /* DemoRenderer.nativeProjectRun(int focus) -> 1: quit */
  fn_v resume;               /* DemoRenderer.nativeResume() */
  fn_v save_state;           /* DemoRenderer.nativeSaveState() */
  fn_v exit;                 /* DemoRenderer.nativeExit() */
  fn_v set_pause;            /* DemoActivity.nativeSetPause() static */
  fn_v mp_disconnect;        /* DemoActivity.nativeMPDisconnect() static */
} N;

static void resolve_natives(void) {
  N.cfg = (fn_ii)need(NAT("DemoActivity", "nativeProjectCfg"));
  N.set_device_id = (fn_s)need(NAT("DemoActivity", "nativeSetDeviceID"));
  N.one_time_init = (fn_v)need(NAT("DemoGLSurfaceView", "nativeProjectOneTimeInit"));
  N.read_cpu_info = (fn_z)need(NAT("DemoActivity", "nativeReadCPUInfo"));
  N.set_make_model = (fn_ss)need(NAT("DemoActivity", "nativeSetDeviceMakeModel"));
  N.screen_size_init = (fn_v)need(NAT("DemoActivity", "nativeScreenSizeInit"));
  N.set_file_system = (fn_ssii)need(NAT("DemoActivity", "nativeSetFileSystem"));
  N.set_screen_size = (fn_iiii)need(NAT("DemoRenderer", "nativeSetScreenSize"));
  N.project_init = (fn_ii)need(NAT("DemoRenderer", "nativeProjectInit"));
  N.project_run = (fn_ri)need(NAT("DemoRenderer", "nativeProjectRun"));
  N.resume = (fn_v)need(NAT("DemoRenderer", "nativeResume"));
  N.save_state = (fn_v)need(NAT("DemoRenderer", "nativeSaveState"));
  N.exit = (fn_v)need(NAT("DemoRenderer", "nativeExit"));
  N.set_pause = (fn_v)need(NAT("DemoActivity", "nativeSetPause"));
  N.mp_disconnect = (fn_v)need(NAT("DemoActivity", "nativeMPDisconnect"));
  if (!N.cfg || !N.set_file_system || !N.set_screen_size || !N.project_init || !N.project_run)
    fatal_error("libssasr.so lacks the natives DemoActivity and DemoRenderer call:\n"
                "is the APK Sonic & SEGA All-Stars Racing 1.0.1?");
}

#define ACT ssr_class(C_ACTIVITY)
#define VIEW ssr_class(C_VIEW)
#define ENV g_jni_env

/* ------------------------------------------------------------ state */
static volatile int g_exit, g_focused = 1, g_focus_changed, g_started;
static uint64_t g_frames;
static int g_engine_up; /* nativeProjectInit has run */
static int g_released;  /* nativeProjectRun returned 1: the engine has freed itself */

void ssr_request_exit(void) { g_exit = 1; }
uint64_t ssr_frame_count(void) { return g_frames; }

/* the watchdog: frames presented, and whether a stop is expected */
uint64_t dcr_boot_frames(void) { return dcr_gl_frames(); }
static volatile int g_system_dialog; /* the Switch keyboard is up (ssr_menu.c): no frames, no hang */
void ssr_boot_system_dialog(int on) { g_system_dialog = on; }
int dcr_boot_in_focus(void) { return g_focused && g_started && !g_exit && !g_system_dialog; }

/* ------------------------------------------------ the splash (updateLoading) */
/* DemoRenderer.z: 0 make the views, 1 the loading picture (3 s), 2 it fades,
 * 3 the white one fades, 4 the Sumo picture (3 s), 5 it fades, 6 (Play
 * Games' cloud load: never here), 7 the movie starts, 8 done. The fades take
 * 4.25 of alpha 255 a frame. */
static struct {
  int z;
  float a;      /* the fading picture's alpha, 0..255 */
  u64 since;    /* DemoRenderer.Y */
  int views;    /* DemoActivity.y: the splash (or the movie after it) is up */
  int movie;    /* DemoActivity.x: the movie plays */
  int done;     /* DemoRenderer.A: the engine runs every frame from now */
} g_sp;

static u64 ms_since(u64 t) { return armTicksToNs(armGetSystemTick() - t) / 1000000ull; }

int ssr_boot_movie_stopped(void) { return !(g_sp.movie || g_sp.views); }

static void splash_show(float loading, float white, float sumo) {
  ssr_gfx_splash(loading / 255.0f, white / 255.0f, sumo / 255.0f);
}

/* One step of DemoRenderer.updateLoading; returns whether the engine runs
 * this frame (the Java's "updateLoading() == 0", or A). */
static int splash_step(void) {
  if (g_sp.done)
    return 1;
  switch (g_sp.z) {
  case 0: /* createViews: the three pictures, the loading one on top */
    g_sp.views = 1;
    g_sp.since = armGetSystemTick();
    g_sp.z = 1;
    splash_show(255, 255, 255);
    return 1;
  case 1:
    if (ms_since(g_sp.since) >= 3000)
      g_sp.z = 2, g_sp.a = 255;
    return 1;
  case 2:
    g_sp.a -= 4.25f;
    if (g_sp.a > 0) {
      splash_show(g_sp.a, 255, 255);
    } else {
      splash_show(0, 255, 255);
      g_sp.since = armGetSystemTick();
      g_sp.a = 255, g_sp.z = 3;
    }
    return 0;
  case 3:
    g_sp.a -= 4.25f;
    if (g_sp.a > 0) {
      splash_show(0, g_sp.a, 255);
    } else {
      splash_show(0, 0, 255);
      g_sp.since = armGetSystemTick();
      g_sp.a = 255, g_sp.z = 4;
    }
    return 0;
  case 4:
    if (ms_since(g_sp.since) >= 3000)
      g_sp.a = 255, g_sp.z = 5;
    return 1;
  case 5:
    g_sp.a -= 4.25f;
    if (g_sp.a > 0) {
      splash_show(0, 0, g_sp.a);
    } else {
      splash_show(0, 0, 0);
      g_sp.z = 6;
    }
    return 0;
  case 6:
    g_sp.z = 7;
    return 0;
  case 7: /* DemoActivity.u: the movie over the game */
    splash_show(0, 0, 0);
    g_sp.movie = dcr_config()->intro && ssr_video_start();
    if (!g_sp.movie)
      g_sp.views = 0; /* no movie: the views go (k()) */
    debugPrintf("[boot] splash done; %s\n", g_sp.movie ? "the intro plays" : "no intro");
    g_sp.done = 1;
    g_sp.z = 8;
    return 0;
  default:
    return 1;
  }
}

/* The movie ended or was skipped: DemoActivity.w (y = x = false). */
static void movie_poll(void) {
  if (g_sp.movie && !ssr_video_playing()) {
    g_sp.movie = 0;
    g_sp.views = 0;
    debugPrintf("[boot] the intro is over\n");
  }
}

/* A touch or a button while the splash is up: DemoGLSurfaceView.onTouchEvent
 * jumps to the movie (z = 7); during the movie, it ends it. */
static void splash_skip(void) {
  if (g_sp.movie) {
    ssr_video_skip();
  } else if (!g_sp.done && g_sp.z < 7) {
    splash_show(0, 0, 0);
    g_sp.z = 7;
  }
}

int ssr_boot_splash_active(void) { return !g_sp.done || g_sp.movie; }

/* --------------------------------------------------------- lifecycle */
static AppletHookCookie g_hook;

static void on_applet(AppletHookType type, void *param) {
  if (type == AppletHookType_OnExitRequest) {
    debugPrintf("[applet] the system asked the game to close\n");
    g_exit = 1;
  }
  if (type == AppletHookType_OnFocusState || type == AppletHookType_OnOperationMode) {
    int focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (focused != g_focused) {
      g_focused = focused;
      g_focus_changed = 1;
    }
  }
}

static void save_state(void) {
  if (g_engine_up && !g_released && N.save_state)
    N.save_state(ENV, g_renderer);
}

static void apply_focus(void) {
  if (!g_focus_changed || !g_started)
    return;
  g_focus_changed = 0;
  if (!g_focused) {
    /* DemoActivity.onPause */
    debugPrintf("[boot] focus lost: onPause\n");
    dcr_boost_idle();
    ssr_perf_focus(0); /* the normal CPU clock for the HOME menu */
    if (N.mp_disconnect)
      N.mp_disconnect(ENV, ACT);
    save_state();
    if (N.set_pause)
      N.set_pause(ENV, ACT);
    ssr_split_focus(0);
    ssr_input_release_all(); /* DemoGLSurfaceView.b(): the touches cancelled */
    if (g_sp.movie)
      ssr_video_skip();
    ssr_music_pause();
    ssr_audio_pause(1);
    log_flush_ring();
    dcr_time_suspend();
  } else {
    /* onResume (the context is still there: no onSurfaceCreated) */
    ssr_perf_focus(1);
    dcr_boost_frame_begin();
    ssr_clock_resync(); /* the time away is no game time */
    dcr_time_resume();
    ssr_audio_pause(0);
    ssr_music_unpause();
    if (N.resume)
      N.resume(ENV, g_renderer);
    ssr_split_focus(1);
    debugPrintf("[boot] focus regained: onResume\n");
  }
}

static void exit_guard(void *arg) {
  (void)arg;
  svcSleepThread(5000000000ll);
  debugPrintf("[boot] the game did not close within 5 s: ending the process\n");
  log_flush_ring();
  svcExitProcess();
}

static void exit_guard_start(void) {
  static Thread t;
  if (R_FAILED(threadCreate(&t, exit_guard, NULL, NULL, 0x4000, 0x2B, -2)) || R_FAILED(threadStart(&t)))
    debugPrintf("[boot] no exit backstop thread\n");
}

static void report(void) {
  static u64 last_tick;
  static unsigned long last_presented;
  u64 tick = armGetSystemTick();
  unsigned long presented = (unsigned long)dcr_gl_frames();
  char fps[24] = "";
  if (last_tick)
    snprintf(fps, sizeof fps, " (%.1f fps)",
             (double)(presented - last_presented) * 1e9 / (double)armTicksToNs(tick - last_tick));
  last_tick = tick;
  last_presented = presented;
  struct mallinfo mi = mallinfo();
  debugPrintf("[boot] %lu frames presented%s, %lu audio mixes, %d Java objects; heap %u MB in use\n", presented,
              fps, ssr_audio_mixes(), jni_live_objects(), (unsigned)(mi.uordblks >> 20));
  ssr_perf_report(dcr_config()->frame_rate);
  ssr_patch_report();
  ssr_race_report();
  ssr_split_report();
  dcr_boost_report();
  dcr_apkcache_report();
}

static void housekeeping(int *launch_done, unsigned long *quiet_at, u64 *last_report) {
  dcr_boost_poll();
  unsigned long frames = (unsigned long)dcr_gl_frames();
  if (!*launch_done && frames > 0) {
    *launch_done = 1;
    dcr_boost_launch_end();
    debugPrintf("[boot] first frame presented\n");
  }
  /* From a while after the engine is up the log goes to a RAM ring
   * (util.c), written out every 10 s and by the watchdog. */
  if (g_engine_up && !*quiet_at)
    *quiet_at = frames + 300;
  if (*quiet_at > 1 && frames >= *quiet_at) {
    *quiet_at = 1;
    log_set_quiet(1);
  }
  u64 now = armGetSystemTick();
  if (armTicksToNs(now - *last_report) >= 10000000000ull) {
    *last_report = now;
    report();
    log_flush_ring();
  }
}

/* ------------------------------------------------------------ onCreate */
static void on_create(void) {
  /* Q(): "1543" + the phone's IMEI + ANDROID_ID: an id for SEGA's services,
   * made from the console's device id here (hashed) */
  u64 dev = 0;
  /* (not under Ryujinx: its set:cal has no GetSerialNumber, and an
   * unimplemented service command ends the emulator) */
  if (!dcr_is_emulator() && R_SUCCEEDED(setcalInitialize())) {
    SetCalSerialNumber sn;
    if (R_SUCCEEDED(setcalGetSerialNumber(&sn)))
      for (const char *p = sn.number; *p; p++)
        dev = dev * 131 + (unsigned char)*p;
    setcalExit();
  }
  char id[64];
  snprintf(id, sizeof id, "1543%016llx", (unsigned long long)dev);
  if (N.set_device_id)
    N.set_device_id(ENV, ACT, jni_str(id));

  /* the keyboard (Configuration.keyboard == QWERTY) and the navigation
   * (NAVIGATION_TRACKBALL): the controller's buttons reach the engine as keys
   * (nativeProjectKey), as on a phone with a hardware keyboard */
  N.cfg(ENV, ACT, 1, ssr_input_keyboard_cfg());
  N.cfg(ENV, ACT, 4, 0);

  /* setContentView: DemoGLSurfaceView's constructor */
  if (N.one_time_init)
    N.one_time_init(ENV, VIEW);
  if (N.read_cpu_info)
    N.read_cpu_info(ENV, ACT, 1); /* Build.CPU_ABI contains "v7a" */
  if (N.set_make_model)
    /* the engine keeps both strings for good and strstr()s the model:
     * "Nexus 7" (and a dozen other models) turns on its optimised scene
     * renderer (NATIVE_CONTRACT.md 3.5) -- set by its flag instead, the
     * model staying the Switch (ssr_patch.c) */
    N.set_make_model(ENV, g_activity, jni_str(SSR_DEVICE_MAKE), jni_str(SSR_DEVICE_MODEL));
  ssr_patch_after_make_model();
  if (N.screen_size_init)
    N.screen_size_init(ENV, ACT);

  /* O(): the expansion file is there (ssr_data.c found the data) */
  N.cfg(ENV, ACT, 6, 1);
  /* P(): its path, the app's data dir, packres.png's offset and length */
  const SsrPack *p = ssr_pack();
  debugPrintf("[boot] nativeSetFileSystem(%s, +%llu, %u)\n", p->path, (unsigned long long)p->pack_off,
              (unsigned)p->pack_len);
  if (p->pack_off > 0x7fffffffull)
    fatal_error("The game's data starts past 2 GB into %s:\nthe engine cannot seek there.", p->path);
  N.set_file_system(ENV, ACT, jni_str(p->path), jni_str("/data/data/" DCR_PACKAGE), (jint)p->pack_off,
                    (jint)p->pack_len);
  N.cfg(ENV, ACT, 5, ssr_language());
  /* Japanese: DemoActivity makes its NoahWrapper (Noah, the offer wall),
   * whose setup hands the engine the object whose methods it then calls
   * unchecked from the menus (NATIVE_CONTRACT.md 8.4). The object here
   * answers them all with nothing (ssr_java.c). */
  if (ssr_language() == 5) {
    fn_v noah = (fn_v)need(NAT("NoahWrapper", "nativeNoahSetup"));
    if (noah)
      noah(ENV, jni_singleton(C_NOAH));
  }
}

/* ------------------------------------------------------------ the frames */
typedef void (*t_depthfunc)(GLenum);

/* SuApplication::ms_uRenderCounter: counts the engine's renders. A frame
 * the engine did not draw (nativeProjectRun called too soon after the last:
 * no 60 Hz tick due) must not be presented: the back buffer holds an old
 * picture. */
static volatile uint32_t *g_render_counter;

int ssr_boot_run(void) {
  ssr_java_init();
  resolve_natives();
  g_render_counter = (volatile uint32_t *)ssr_native("_ZN13SuApplication17ms_uRenderCounterE");
  dcr_watchdog_start();

  /* ---- System.loadLibrary: JNI_OnLoad ---- */
  fn_onload onload = (fn_onload)ssr_native("JNI_OnLoad");
  if (onload)
    debugPrintf("[boot] JNI_OnLoad -> 0x%lx\n", (unsigned long)onload(g_jni_vm, NULL));

  ssr_input_init();
  ssr_audio_init();
  on_create();

  /* ---- the GL surface ---- */
  if (ssr_gfx_init() != 0)
    fatal_error("Could not set up the graphics (EGL / OpenGL ES 1): see debug.log.");
  t_depthfunc depth = (t_depthfunc)dcr_gl_lookup("glDepthFunc");
  if (depth)
    depth(GL_LEQUAL); /* onSurfaceCreated */
  int w, h;
  ssr_gfx_size(&w, &h);
  debugPrintf("[boot] onSurfaceChanged -> nativeSetScreenSize(%d, %d)\n", w, h);
  N.set_screen_size(ENV, g_renderer, w, h, 0, 0);
  if (N.resume)
    N.resume(ENV, g_renderer); /* onResume */

  if (!dcr_config()->splash)
    g_sp.z = 7, g_sp.views = 1; /* straight to the movie */
  appletHook(&g_hook, on_applet, NULL);
  g_started = 1;
  debugPrintf("[boot] up; this thread runs the engine's frames now\n");
  log_flush_ring();

  u64 last_report = armGetSystemTick();
  int launch_done = 0;
  unsigned long quiet_at = 0;
  int draws = 0; /* DemoRenderer.V: onDrawFrame calls that reached the engine's part */
  u64 run_ticks = 0; /* in nativeProjectRun since the last present (ssr_perf.c) */
  unsigned polls = 0; /* calls since then in which no engine tick was due */
  while (!g_exit && appletMainLoop()) {
    apply_focus();
    if (!g_focused) {
      svcSleepThread(50000000ll);
      continue;
    }
    movie_poll();
    if (g_engine_up)
      ssr_split_pre(); /* split screen: player 2's copy started / stopped, the sizes */
    SsrPad pad;
    ssr_input_frame(&pad); /* touches, keys and tilt to the engine (not while the splash is up) */
    if (ssr_boot_splash_active() && (pad.down || pad.any_touch))
      splash_skip();
    const uint32_t renders = g_render_counter ? *g_render_counter : 0;
    int engine_ran = 0;
    if (splash_step()) {
      /* onDrawFrame: nativeProjectInit on the 7th, then the frames */
      if (!g_engine_up && ++draws > 6) {
        debugPrintf("[boot] nativeProjectInit\n");
        u64 t0 = armGetSystemTick();
        N.project_init(ENV, g_renderer, SSR_MAX_MEMORY, SSR_TOTAL_MEMORY);
        g_engine_up = 1;
        ssr_patch_after_init(); /* the mipmap bias, FPSCR (ssr_patch.c) */
        debugPrintf("[boot] nativeProjectInit done in %llu ms\n",
                    (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
        ssr_split_init();
        log_flush_ring();
      }
      engine_ran = g_engine_up;
      if (g_engine_up) /* its context, the window (a split race's passes pick their own; split
                        * screen's BATTLE: its framebuffer, its half) */
        ssr_gfx_use(0, ssr_split_engine1_offscreen());
      const u64 r0 = armGetSystemTick();
      const int quit = engine_ran && N.project_run(ENV, g_renderer, 1) == 1;
      run_ticks += armGetSystemTick() - r0;
      if (quit) {
        /* BACK on the title screen: releaseAppData has run already */
        debugPrintf("[boot] the game asked to quit (nativeProjectRun = 1)\n");
        g_released = 1;
        break;
      }
    }
    /* Present when there is something new: the engine rendered (its counter
     * moved), or it has not rendered yet at all (its start-up clears the
     * screen to white each call), or the splash / the intro is over it.
     * Otherwise nothing was drawn -- the engine's 60 Hz tick was not due. */
    const int drew2 = g_engine_up ? ssr_split_post(&run_ticks) : 0; /* player 2's copy, if it runs */
    const int drew = !engine_ran || !g_render_counter || *g_render_counter != renders || *g_render_counter == 0 ||
                     ssr_boot_splash_active() || drew2;
    if (drew) {
      const u64 p0 = armGetSystemTick();
      ssr_gfx_present();
      ssr_perf_frame(run_ticks, armGetSystemTick() - p0, polls);
      run_ticks = 0, polls = 0;
    } else {
      polls++;
      svcSleepThread(2000000ll); /* the engine's next logic tick is due in a few ms */
      continue;
    }
    g_frames++;
    housekeeping(&launch_done, &quiet_at, &last_report);
  }

  /* ---- the way out: onPause, onStop, onDestroy ---- */
  log_set_quiet(0);
  appletUnhook(&g_hook);
  exit_guard_start();
  ssr_perf_exit(); /* the CPU back to its normal clock */
  if (!g_focused)
    dcr_time_resume();
  save_state();
  ssr_split_exit();
  if (g_engine_up && !g_released && N.exit)
    N.exit(ENV, g_renderer); /* onDestroy: releaseAppData */
  ssr_audio_close();
  debugPrintf("[boot] the game has closed\n");
  log_flush_ring();
  return 0;
}
