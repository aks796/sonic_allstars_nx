/* ssr_loader.c -- loading Sonic & SEGA All-Stars Racing's engine.
 *
 * The APK carries one library, lib/armeabi/libssasr.so: Sumo Digital's engine
 * and the game, Distinctive Developments' Android layer (DD*), OpenAL Soft
 * 1.13 and zlib 1.2.1, built with GCC 4.4.3 for ARMv5TE (soft-float, ARM and
 * Thumb-1). It imports bionic libc/libm, pthreads, a few UDP socket calls,
 * GLES 1.x, liblog and libdl (187 imports: tools/gen_imports.py). Relocations:
 * RELATIVE, ABS32, GLOB_DAT, JUMP_SLOT; one of them patches the code (a text
 * relocation), which is fine: everything is applied in the staging copy
 * before the code is mapped. Java loads it with System.loadLibrary in
 * DemoActivity's static initialiser: its 160 constructors run, then its
 * JNI_OnLoad (OpenAL Soft's: it keeps the JavaVM for its playback thread).
 *
 * KERNEL USER HELPERS. libgcc's linux-atomic.c builds the __sync_* functions
 * on the Linux kernel's user helpers: literals 0xffff0fc0 (__kuser_cmpxchg,
 * 43 of them) and 0xffff0fa0 (__kuser_memory_barrier, 4), loaded and called
 * with blx. Horizon has nothing at 0xffff0000: the literals are pointed at
 * kuser.S before the code is sealed (as the PvZ and Asphalt ports do).
 *
 * The engine never writes its own code, so the runtime's code-space hooks
 * (codespace.h: PvZ's mod patches its engine at run time) answer "not ours".
 * MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "codespace.h"
#include "config.h"
#include "dcr_net.h"
#include "error.h"
#include "imports.h"
#include "ssr.h"
#include "util.h"

const char *dcr_game_root(void); /* main.c */

so_module g_mod_game;

void *ssr_native(const char *symbol) { return (void *)so_try_find_addr_rx(&g_mod_game, symbol); }

/* Which copy of the engine runs on this thread: 0, or 1 (split screen's
 * second). Set by the thread that calls into a copy; threads a copy starts
 * inherit it (bionic_pthread.c). The JNI side (audio tracks, text) tells the
 * copies apart by it. */
static __thread int g_engine_tls;
int ssr_engine_current(void) { return g_engine_tls; }
void ssr_engine_set_current(int engine) { g_engine_tls = engine; }

/* the second copy of the library (split screen, ssr_split.c) */
so_module g_mod_game2;
static int g_mod2_up;

/* an export of copy `engine`, or NULL */
void *ssr_native_in(int engine, const char *symbol) {
  if (engine == 0)
    return ssr_native(symbol);
  return engine == 1 && g_mod2_up > 0 ? (void *)so_try_find_addr_rx(&g_mod_game2, symbol) : NULL;
}

/* a library address (vaddr: a function the library does not export) in copy
 * `engine`, if its first word is `expect`; else NULL */
void *ssr_addr_in(int engine, uint32_t vaddr, uint32_t expect) {
  const so_module *m = engine == 0 ? &g_mod_game : engine == 1 && g_mod2_up > 0 ? &g_mod_game2 : NULL;
  if (!m || !m->load_virtbase || vaddr + 4 > m->load_size)
    return NULL;
  uint8_t *a = (uint8_t *)m->load_virtbase + vaddr;
  return *(const uint32_t *)a == expect ? a : NULL;
}

/* ----------------------------------------------------- kernel helpers */
void dcr_kuser_cmpxchg(void);
void dcr_kuser_memory_barrier(void);

static void fix_kuser_helpers(so_module *m) {
  int cmpxchg = 0, barrier = 0;
  for (int i = 0; i < m->phnum; i++) {
    const Elf32_Phdr *ph = &m->phdr[i];
    if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
      continue;
    uint32_t *w = (uint32_t *)((uintptr_t)((uint8_t *)m->load_base + ph->p_vaddr + 3) & ~3u);
    size_t nw = ph->p_filesz / 4;
    for (size_t k = 0; k < nw; k++) {
      if (w[k] == 0xffff0fc0u) {
        w[k] = (uint32_t)(uintptr_t)dcr_kuser_cmpxchg;
        cmpxchg++;
      } else if (w[k] == 0xffff0fa0u) {
        w[k] = (uint32_t)(uintptr_t)dcr_kuser_memory_barrier;
        barrier++;
      }
    }
  }
  debugPrintf("[boot] %s: kernel user helpers -> kuser.S (%d cmpxchg, %d barrier)%s\n", m->base_name, cmpxchg, barrier,
              cmpxchg == 43 && barrier == 4 ? "" : " -- not the counts of 1.0.1's library");
}

/* ------------------------------------------------------------- loading */
static int load_into(so_module *m) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dcr_game_root(), SSR_LIB_GAME);
  u64 t0 = armGetSystemTick();
  int rc = so_load(m, path, NULL, SO_REGION_BYTES);
  if (rc < 0) {
    const char *why = rc == -1 ? "cannot open it, or it is not a 32-bit ARM ELF"
                    : rc == -2 ? "out of memory"
                    : rc == -3 ? "larger than SO_REGION_BYTES"
                    : rc == -4 ? "too many program headers" : "?";
    debugPrintf("[boot] so_load(%s) failed rc=%d: %s\n", path, rc, why);
    return -1;
  }
  so_relocate(m);
  int missing = so_resolve(m, dcr_imports, dcr_imports_count, 1);
  fix_kuser_helpers(m);
  void ssr_patch_engine(so_module * m); /* ssr_patch.c: VFP maths, GL bookkeeping, the clock */
  ssr_patch_engine(m);
  so_finalize(m);
  so_flush_caches(m);
  debugPrintf("[boot] %s %u KB at %p (%d unresolved imports) in %llu ms\n", m->base_name,
              (unsigned)(m->load_size >> 10), m->load_virtbase, missing,
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
  return 0;
}

int ssr_load_module(void) { return load_into(&g_mod_game); }

/* Split screen: the engine a second time, its own code and data (its
 * relocations resolve its symbols to itself: every global of the game is its
 * own), started as System.loadLibrary started the first -- its constructors,
 * then its JNI_OnLoad (OpenAL's) -- on this thread marked as the second
 * copy's (the Labyrinth 2 port's lab_loader.c load_copy). 0 once it is up. */
int ssr_load_second_module(void) {
  if (g_mod2_up)
    return g_mod2_up > 0 ? 0 : -1;
  g_mod2_up = -1;
  u64 t0 = armGetSystemTick();
  if (load_into(&g_mod_game2) != 0)
    return -1;
  g_mod2_up = 1;
  const int was = g_engine_tls;
  g_engine_tls = 1;
  so_execute_init_array(&g_mod_game2);
  typedef jint (*fn_onload)(void *vm, void *reserved);
  fn_onload onload = (fn_onload)so_try_find_addr_rx(&g_mod_game2, "JNI_OnLoad");
  if (onload)
    onload(g_jni_vm, NULL);
  g_engine_tls = was;
  debugPrintf("[split] the second copy of the engine is loaded (%llu ms)\n",
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
  return 0;
}

/* System.loadLibrary: the constructors (JNI_OnLoad: ssr_boot.c). */
void ssr_run_constructors(void) {
  extern int g_so_trace_ctors;
  g_so_trace_ctors = dcr_is_emulator();
  u64 t0 = armGetSystemTick();
  so_execute_init_array(&g_mod_game);
  debugPrintf("[boot] %s constructors done in %llu ms\n", g_mod_game.base_name,
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
}

/* ------------------------------------------------ the shared runtime's hooks
 * codespace.h: code written at run time by the game's own modules (PvZ's mod
 * did that); this engine never does, so every request is the plain shim's. */
volatile int g_cs_armed;
void *cs_mmap(size_t len, int prot, const void *caller) { return NULL; }
int cs_munmap(void *addr, size_t len) { return 0; }
int cs_mprotect(void *addr, size_t len, int prot, const void *caller) {
  /* The engine's own pages: never a real change (text stays RX, data RW). */
  return so_find_module_by_addr(addr) != NULL;
}
int cs_write(void *dst, const void *src, size_t n, int c, int kind) { return 0; }

/* exc_handler.c: no trampoline pool here. */
int dcr_in_code_pool(const void *p) { return 0; }

/* dcr_net.h: ssr_net.c (the LOCAL multiplayer's UDP between the engine's copies) */
