/* ssr_perf.c -- 60 fps: the clocks, and where each frame's 16.7 ms go.
 *
 * THE CLOCKS (the Asphalt 8 port's a8r_perf.c, whose hardware runs measured
 * the same kind of engine: a 2012-13 phone game's CPU work at the Switch's
 * 1020 MHz, where the phones it was made for ran at 1.5-2 GHz). No performance
 * configuration pairs a faster CPU with a usable GPU -- the boost mode's
 * 1785 MHz comes with a 76.8 MHz GPU -- but the clock driver itself (clkrst,
 * pcv before 8.0.0) sets the CPU alone: [performance] cpu_clock. The system
 * puts its own clock back on a dock change, after sleep or a boost-mode
 * change, so a thread checks it every 250 ms and sets it again; the HOME menu
 * gets the normal clock. With the clock driver, loading frames (dcr_boost.c)
 * are boosted with the CPU clock too, the GPU untouched. In handheld mode the
 * GPU runs at 384 MHz; games may ask for 460.8 (performance configuration
 * 0x92220007): [performance] gpu_boost_handheld. Docked it is at 768 already.
 *
 * THE FRAMES. The engine runs on the main thread (ssr_boot.c): per presented
 * frame this measures nativeProjectRun (the game's logic tick and its GL
 * calls), the present (the port's overlays and eglSwapBuffers), and inside the
 * swap the wait for a free display buffer (nwindowDequeueBuffer, wrapped by
 * the linker): a long wait there is the GPU (or the display) holding every
 * buffer, a long run is the CPU. Every 10 s one [perf] line: the frame rate,
 * refreshes that showed an old frame (at 60 fps), the averages and worsts, the
 * share of frames over 16.7 / 33.3 ms, the clocks. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "ssr.h"
#include "util.h"

/* ---------------------------------------------------------------- the CPU clock */
#define CPU_NORMAL_HZ 1020000000u
#define CPU_BOOST_HZ 1785000000u

static int g_clk_kind; /* 0 none, 1 clkrst, 2 pcv */
static ClkrstSession g_clk;
static Mutex g_clk_mx;
static Thread g_clk_thread;
static volatile int g_clk_stop, g_clk_focus = 1, g_clk_boost;
static int g_clk_thread_up;
static u32 g_clk_want;   /* cpu_clock, in Hz */
static u32 g_clk_resets; /* times the system had put its own clock back */

static Result cpu_get(u32 *hz) {
  return g_clk_kind == 1 ? clkrstGetClockRate(&g_clk, hz) : pcvGetClockRate(PcvModule_CpuBus, hz);
}
static Result cpu_set(u32 hz) {
  return g_clk_kind == 1 ? clkrstSetClockRate(&g_clk, hz) : pcvSetClockRate(PcvModule_CpuBus, hz);
}

/* the clock this moment calls for, set if it is not what the CPU runs at */
static void cpu_apply(void) {
  mutexLock(&g_clk_mx);
  u32 want = !g_clk_focus ? CPU_NORMAL_HZ : g_clk_boost ? CPU_BOOST_HZ : g_clk_want, now = 0;
  if (g_clk_boost && g_clk_want > want)
    want = g_clk_want;
  if (g_clk_kind && R_SUCCEEDED(cpu_get(&now)) && (now + 2000000u < want || now > want + 2000000u)) {
    if (now < want && g_clk_focus && !g_clk_boost && now == CPU_NORMAL_HZ)
      g_clk_resets++;
    cpu_set(want);
  }
  mutexUnlock(&g_clk_mx);
}

static void cpu_watch(void *arg) {
  (void)arg;
  while (!g_clk_stop) {
    svcSleepThread(250000000ll);
    if (g_clk_focus)
      cpu_apply();
  }
}

int ssr_cpu_managed(void) { return g_clk_kind != 0; }

void ssr_cpu_boost(int on) {
  if (!g_clk_kind || g_clk_boost == on)
    return;
  g_clk_boost = on;
  cpu_apply();
}

/* ssr_boot.c's focus handling: the HOME menu or sleep gets the normal clock */
void ssr_perf_focus(int focused) {
  if (!g_clk_kind || g_clk_focus == focused)
    return;
  g_clk_focus = focused;
  cpu_apply();
}

/* the end of the game, a fatal error (atexit), the exit guard: once */
void ssr_perf_exit(void) {
  static int done;
  if (!g_clk_kind || __atomic_exchange_n(&done, 1, __ATOMIC_SEQ_CST))
    return;
  g_clk_stop = 1;
  if (g_clk_thread_up) {
    threadWaitForExit(&g_clk_thread);
    threadClose(&g_clk_thread);
  }
  mutexLock(&g_clk_mx);
  cpu_set(CPU_NORMAL_HZ);
  mutexUnlock(&g_clk_mx);
}

static void cpu_start(void) {
  if (hosversionAtLeast(8, 0, 0)) {
    if (R_SUCCEEDED(clkrstInitialize())) {
      if (R_SUCCEEDED(clkrstOpenSession(&g_clk, PcvModuleId_CpuBus, 3)))
        g_clk_kind = 1;
      else
        clkrstExit();
    }
  } else if (R_SUCCEEDED(pcvInitialize())) {
    g_clk_kind = 2;
  }
  u32 was = 0;
  if (!g_clk_kind || R_FAILED(cpu_get(&was)) || was < 100000000u) {
    debugPrintf("[perf] no access to the CPU clock (%s): it stays the system's; loading is boosted "
                "the system's way\n",
                hosversionAtLeast(8, 0, 0) ? "clkrst" : "pcv");
    if (g_clk_kind == 1)
      clkrstCloseSession(&g_clk), clkrstExit();
    else if (g_clk_kind == 2)
      pcvExit();
    g_clk_kind = 0;
    return;
  }
  mutexInit(&g_clk_mx);
  g_clk_want = (u32)dcr_config()->cpu_clock * 1000000u;
  cpu_apply();
  g_clk_resets = 0;
  u32 now = 0;
  cpu_get(&now);
  debugPrintf("[perf] CPU clock %u MHz (%s): was %u MHz, now %u MHz\n", (unsigned)(g_clk_want / 1000000u),
              g_clk_kind == 1 ? "clkrst" : "pcv", (unsigned)(was / 1000000u), (unsigned)(now / 1000000u));
  if (R_SUCCEEDED(threadCreate(&g_clk_thread, cpu_watch, NULL, NULL, 0x4000, 0x2C, -2)))
    g_clk_thread_up = R_SUCCEEDED(threadStart(&g_clk_thread));
  atexit(ssr_perf_exit);
}

static u32 cpu_mhz_now(void) {
  u32 hz = 0;
  if (g_clk_kind) {
    mutexLock(&g_clk_mx);
    cpu_get(&hz);
    mutexUnlock(&g_clk_mx);
  }
  return hz / 1000000u;
}

/* ---------------------------------------------------------------- the GPU clock */
#define CFG_HANDHELD_GPU_460 0x92220007u
static int g_apm;

/* main(), once config.ini is read: before the start-up boost */
void ssr_perf_clocks(void) {
  static int done;
  if (done++)
    return;
  if (R_FAILED(apmInitialize())) {
    debugPrintf("[perf] apm unavailable: the GPU clock stays the system's\n");
  } else {
    g_apm = 1;
    if (dcr_config()->gpu_boost) {
      Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, CFG_HANDHELD_GPU_460);
      u32 got = 0;
      apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &got);
      debugPrintf("[perf] handheld GPU clock 460.8 MHz (configuration 0x%08x): %s (handheld now 0x%08x)\n",
                  CFG_HANDHELD_GPU_460, R_SUCCEEDED(rc) ? "granted" : "refused", (unsigned)got);
    }
  }
  cpu_start(); /* after the configuration, which sets the CPU's clock too */
}

static const char *clocks_now(void) {
  static char s[80];
  ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
  u32 cfg = 0;
  if (!g_apm || R_FAILED(apmGetPerformanceMode(&mode)) || R_FAILED(apmGetPerformanceConfiguration(mode, &cfg)))
    return "clocks ?";
  const char *gpu = cfg == 0x00010000u ? "GPU 384" : cfg == 0x00010001u ? "GPU 768"
                    : cfg == CFG_HANDHELD_GPU_460                     ? "GPU 460.8"
                    : cfg == 0x92220009u || cfg == 0x9222000Au       ? "CPU boost, GPU 76.8"
                                                                      : "";
  snprintf(s, sizeof s, "%s, configuration 0x%08x%s%s", mode == ApmPerformanceMode_Boost ? "docked" : "handheld",
           (unsigned)cfg, *gpu ? " = " : "", gpu);
  return s;
}

/* ---------------------------------------------------------------- frame times */
static u64 g_wait_now; /* ticks blocked in dequeue since the last frame */

Result __real_nwindowDequeueBuffer(NWindow *nw, s32 *slot, NvMultiFence *fence);
Result __wrap_nwindowDequeueBuffer(NWindow *nw, s32 *slot, NvMultiFence *fence) {
  u64 t0 = armGetSystemTick();
  Result rc = __real_nwindowDequeueBuffer(nw, slot, fence);
  __atomic_fetch_add(&g_wait_now, armGetSystemTick() - t0, __ATOMIC_RELAXED);
  return rc;
}

static volatile uint32_t *g_ticks, *g_renders; /* SuApplication::ms_frameCounter, ms_uRenderCounter */
static uint32_t g_ticks0, g_renders0;
static u64 g_n, g_run, g_run_max, g_present, g_wait, g_wait_max, g_cpu, g_polls;
static u64 g_over16, g_over33, g_worst, g_last_end, g_report_tick, g_cpu_last;

static u64 main_thread_ticks(void) {
  u64 v = 0;
  if (R_FAILED(svcGetInfo(&v, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total)))
    svcGetInfo(&v, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, TickCountInfo_Total);
  return v;
}

/* ssr_boot.c, after each presented frame: the ticks of nativeProjectRun (all
 * calls since the last present) and of the present; polls = calls in which the
 * engine had no tick due */
void ssr_perf_frame(u64 run_ticks, u64 present_ticks, unsigned polls) {
  u64 now = armGetSystemTick();
  g_n++;
  g_run += run_ticks;
  if (run_ticks > g_run_max)
    g_run_max = run_ticks;
  g_present += present_ticks;
  g_polls += polls;
  u64 w = __atomic_exchange_n(&g_wait_now, 0, __ATOMIC_RELAXED);
  g_wait += w;
  if (w > g_wait_max)
    g_wait_max = w;
  u64 c = main_thread_ticks();
  if (g_cpu_last && c >= g_cpu_last)
    g_cpu += c - g_cpu_last;
  g_cpu_last = c;
  if (g_last_end) {
    u64 ft = now - g_last_end;
    g_over16 += ft > armNsToTicks(17500000);
    g_over33 += ft > armNsToTicks(34200000);
    if (ft > g_worst)
      g_worst = ft;
  }
  g_last_end = now;
}

/* every 10 s (ssr_boot.c's report) */
void ssr_perf_report(int target_fps) {
  u64 now = armGetSystemTick();
  if (!g_n || !g_report_tick) {
    g_report_tick = now;
    g_n = g_run = g_run_max = g_present = g_wait = g_wait_max = g_cpu = g_polls = 0;
    g_over16 = g_over33 = g_worst = 0;
    return;
  }
  const double ms = 1000.0 / 19200000.0; /* ticks: 19.2 MHz */
  if (!g_ticks) {
    g_ticks = (volatile uint32_t *)ssr_native("_ZN13SuApplication15ms_frameCounterE");
    g_renders = (volatile uint32_t *)ssr_native("_ZN13SuApplication17ms_uRenderCounterE");
  }
  const uint32_t ticks = g_ticks ? *g_ticks : 0, renders = g_renders ? *g_renders : 0;
  char eng[64] = "";
  if (g_ticks && g_ticks0)
    snprintf(eng, sizeof eng, "; the game: %u logic steps, %u renders", (unsigned)(ticks - g_ticks0),
             (unsigned)(renders - g_renders0));
  g_ticks0 = ticks, g_renders0 = renders;
  const double secs = (double)armTicksToNs(now - g_report_tick) / 1e9;
  u64 refreshes = (u64)(secs * target_fps + 0.5);
  u64 missed = refreshes > g_n ? refreshes - g_n : 0;
  g_report_tick = now;
  char clk[48] = "";
  if (g_clk_kind)
    snprintf(clk, sizeof clk, ", CPU %u MHz", (unsigned)cpu_mhz_now());
  if (g_clk_kind && g_clk_resets)
    snprintf(clk + strlen(clk), sizeof clk - strlen(clk), " (set again %u times)", (unsigned)g_clk_resets);
  debugPrintf("[perf] %.1f fps (%llu frames, %llu of %llu refreshes missed): engine %.1f ms a frame (max %.1f), "
              "present %.1f, of it waiting for a buffer %.1f (max %.1f); main thread on the CPU %.1f ms; "
              "frames over 16.7 ms %llu%%, over 33 ms %llu%%, worst %.0f ms; %llu early calls%s; %s%s\n",
              (double)g_n / secs, (unsigned long long)g_n, (unsigned long long)missed,
              (unsigned long long)refreshes, g_run * ms / g_n, g_run_max * ms, g_present * ms / g_n,
              g_wait * ms / g_n, g_wait_max * ms, g_cpu * ms / g_n, (unsigned long long)(g_over16 * 100 / g_n),
              (unsigned long long)(g_over33 * 100 / g_n), g_worst * ms, (unsigned long long)g_polls, eng,
              clocks_now(), clk);
  g_n = g_run = g_run_max = g_present = g_wait = g_wait_max = g_cpu = g_polls = 0;
  g_over16 = g_over33 = g_worst = 0;
}
