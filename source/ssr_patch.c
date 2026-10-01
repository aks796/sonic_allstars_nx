/* ssr_patch.c -- the engine made faster, and its pictures sharper
 * (../source/docs/RENDERING.md).
 *
 * The library is built for ARMv5TE soft-float: every float operation in the
 * engine -- 6573 multiplies, 4449 adds, 1204 divides, 1275 compares... under
 * every vector, matrix, physics and skinning step -- is a call into libgcc's
 * integer emulation of IEEE arithmetic, tens of instructions each, on a CPU
 * with a VFPv4 unit idle beside it. Those helpers are replaced with VFP code
 * (the same IEEE single/double results, round to nearest, with FPSCR's
 * flush-to-zero and default-NaN bits clear, which is checked):
 *   in place, 16-20 bytes, where nothing branches into the first 20 bytes:
 *     __aeabi_fadd (fsub/frsub fall into it), fmul, fdiv, cfcmpeq/cfcmple
 *     (fcmpeq/lt/le/ge/gt and cfrcmple call it: only its C and Z flags are
 *     read, which VFP's compare sets the same way), f2iz, f2uiz, f2d, d2f,
 *     dadd (dsub), dmul, ddiv;
 *   by an 8-byte jump: i2f and ui2f (ui2f branches to i2f + 8), to ssr_vfp.S.
 *
 * DDGLRefresh (the Android layer's shadow of every GL buffer and texture, to
 * re-upload them after a lost context -- which never happens here) costs a
 * linear search of up to 3072 slots on every buffer bind (two per draw):
 * getBufferIndex is replaced by a name -> slot cache, checked against the
 * table (a deleted buffer's slot is zeroed, so live names are unique), with
 * the engine's own search behind it. And every per-frame buffer upload was
 * also copied into its shadow: the copy is skipped (only the context-loss
 * refresh reads it). GLES::CheckForErrors (hundreds of glGetError calls a
 * frame, for a debug log) returns at once.
 *
 * FRAME PACING. The engine's logic clock is gettimeofday in ms: a 60 Hz step
 * whenever its accumulator reaches 0.75. Against a display whose refresh is
 * not exactly 1000/60 ms that drifts, and every so often a frame gets two
 * steps or none (a stutter). GetCurrentTime is hooked to count display
 * refreshes instead: each call, the refreshes since the last one (the real
 * time rounded to 1/60 s: 1 at 60 fps, 2 at 30 or after a missed refresh),
 * returned as 50/3 ms each. [display] frame_pacing = engine keeps its own.
 *
 * QUALITY. The optimised scene renderer (STMatCentricRender: static track
 * geometry batched by material at load; the path the 2012 Nexus 7 and Galaxy
 * S III took) is switched on by its flag, the model still "Switch". Every
 * mipmapped texture gets anisotropic filtering (the engine never asks for it;
 * gl_mesa.c routes its glTexParameter calls here), and with it the engine's
 * -1.0 mipmap bias (sharpening for small phone screens, shimmering on a TV)
 * becomes -0.5. MIT.
 */
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "gl_layer.h"
#include "so_util.h"
#include "ssr.h"
#include "util.h"

extern so_module g_mod_game; /* ssr_loader.c */

/* the staging copy of a symbol's code (before so_finalize) */
static uint32_t *staged(so_module *m, const char *sym) {
  uintptr_t a = so_try_find_addr_rx(m, sym);
  if (!a)
    return NULL;
  return (uint32_t *)((uint8_t *)m->load_base + (a - (uintptr_t)m->load_virtbase));
}

/* write `n` words at sym+off if the word there is `expect` (1.0.1's library) */
static int patch(so_module *m, const char *sym, unsigned off, uint32_t expect, const uint32_t *code, int n) {
  uint32_t *p = staged(m, sym);
  if (!p)
    return 0;
  p = (uint32_t *)((uint8_t *)p + off);
  if (p[0] != expect) {
    debugPrintf("[patch] %s+0x%x is 0x%08x, not 0x%08x: left alone (another build of the library?)\n", sym,
                off, (unsigned)p[0], (unsigned)expect);
    return 0;
  }
  memcpy(p, code, (size_t)n * 4);
  return 1;
}

/* an 8-byte absolute jump at sym (ldr pc, [pc, #-4]; .word dst) */
static int jump(so_module *m, const char *sym, uint32_t expect, void *dst) {
  const uint32_t stub[2] = {0xe51ff004u, (uint32_t)(uintptr_t)dst};
  return patch(m, sym, 0, expect, stub, 2);
}

/* ---------------------------------------------------------------- VFP maths */
#define VMOV_S0_R0 0xee000a10u
#define VMOV_S1_R1 0xee001a90u
#define VMOV_R0_S0 0xee100a10u
#define VMOV_D0_R0R1 0xec410b10u
#define VMOV_D1_R2R3 0xec432b11u
#define VMOV_R0R1_D0 0xec510b10u
#define BX_LR 0xe12fff1eu

static const uint32_t k_fadd[] = {VMOV_S0_R0, VMOV_S1_R1, 0xee300a20u /* vadd.f32 s0,s0,s1 */, VMOV_R0_S0, BX_LR};
static const uint32_t k_fmul[] = {VMOV_S0_R0, VMOV_S1_R1, 0xee200a20u /* vmul.f32 */, VMOV_R0_S0, BX_LR};
static const uint32_t k_fdiv[] = {VMOV_S0_R0, VMOV_S1_R1, 0xee800a20u /* vdiv.f32 */, VMOV_R0_S0, BX_LR};
static const uint32_t k_cfcmp[] = {VMOV_S0_R0, VMOV_S1_R1, 0xeeb40a60u /* vcmp.f32 s0,s1 */,
                                   0xeef1fa10u /* vmrs APSR_nzcv,fpscr */, BX_LR};
static const uint32_t k_f2iz[] = {VMOV_S0_R0, 0xeebd0ac0u /* vcvt.s32.f32 (toward zero) */, VMOV_R0_S0, BX_LR};
static const uint32_t k_f2uiz[] = {VMOV_S0_R0, 0xeebc0ac0u /* vcvt.u32.f32 */, VMOV_R0_S0, BX_LR};
static const uint32_t k_f2d[] = {VMOV_S0_R0, 0xeeb71ac0u /* vcvt.f64.f32 d1,s0 */, 0xec510b11u /* vmov r0,r1,d1 */,
                                 BX_LR};
static const uint32_t k_d2f[] = {VMOV_D0_R0R1, 0xeeb70bc0u /* vcvt.f32.f64 s0,d0 */, VMOV_R0_S0, BX_LR};
static const uint32_t k_dadd[] = {VMOV_D0_R0R1, VMOV_D1_R2R3, 0xee300b01u /* vadd.f64 */, VMOV_R0R1_D0, BX_LR};
static const uint32_t k_dmul[] = {VMOV_D0_R0R1, VMOV_D1_R2R3, 0xee200b01u /* vmul.f64 */, VMOV_R0R1_D0, BX_LR};
static const uint32_t k_ddiv[] = {VMOV_D0_R0R1, VMOV_D1_R2R3, 0xee800b01u /* vdiv.f64 */, VMOV_R0R1_D0, BX_LR};
#define N(a) ((int)(sizeof a / sizeof a[0]))

void ssr_vfp_i2f(void); /* ssr_vfp.S */
void ssr_vfp_ui2f(void);

static void patch_float(so_module *m) {
  /* the first word of each in 1.0.1's library */
  int n = 0;
  n += patch(m, "__aeabi_fadd", 0, 0xe1b02080u, k_fadd, N(k_fadd));
  n += patch(m, "__aeabi_fmul", 0, 0xe3a0c0ffu, k_fmul, N(k_fmul));
  n += patch(m, "__aeabi_fdiv", 0, 0xe3a0c0ffu, k_fdiv, N(k_fdiv));
  n += patch(m, "__aeabi_cfcmpeq", 0, 0xe92d400fu, k_cfcmp, N(k_cfcmp));
  n += patch(m, "__aeabi_f2iz", 0, 0xe1a02080u, k_f2iz, N(k_f2iz));
  n += patch(m, "__aeabi_f2uiz", 0, 0xe1b02080u, k_f2uiz, N(k_f2uiz));
  n += patch(m, "__aeabi_f2d", 0, 0xe1b02080u, k_f2d, N(k_f2d));
  n += patch(m, "__aeabi_d2f", 0, 0xe1a02081u, k_d2f, N(k_d2f));
  n += patch(m, "__aeabi_dadd", 0, 0xe92d4030u, k_dadd, N(k_dadd));
  n += patch(m, "__aeabi_dmul", 0, 0xe92d4070u, k_dmul, N(k_dmul));
  n += patch(m, "__aeabi_ddiv", 0, 0xe92d4070u, k_ddiv, N(k_ddiv));
  n += jump(m, "__aeabi_i2f", 0xe2103102u, (void *)ssr_vfp_i2f);
  n += jump(m, "__aeabi_ui2f", 0xe3a03000u, (void *)ssr_vfp_ui2f);
  debugPrintf("[patch] float maths: %d of 13 libgcc helpers now VFP\n", n);
}

/* ---------------------------------------------------------------- DDGLRefresh */
typedef struct {
  uint32_t name;     /* +0 the GL buffer name (0: free) */
  uint32_t *holder;  /* +4 where the game keeps the name (ddGLGenBuffers' array) */
  void *shadow;      /* +8 its copy of the data (ddGLBufferData), or NULL */
  uint8_t pad[0xc];
  uint8_t used;      /* +0x18 */
  uint8_t pad2[3];
} DdBuffer; /* 0x1c bytes */
_Static_assert(sizeof(DdBuffer) == 0x1c, "DDGLRefresh buffer slot");

/* each copy of the engine its own tables (split screen: two), picked by the
 * copy that runs on this thread */
int ssr_engine_current(void); /* ssr_loader.c */
extern so_module g_mod_game2;
static DdBuffer **g_sbuffers[2]; /* &DDGLRefresh::sBuffers */
static int *g_nbuffers[2];       /* &DDGLRefresh::mNumBuffers */
#define SLOT_CACHE 8192
static uint16_t g_slot[2][SLOT_CACHE]; /* GL name -> slot + 1 */
static unsigned g_lookups, g_misses;

/* DDGLRefresh::getBufferIndex(unsigned): the slot of a buffer name, -1 if
 * none -- in copy e's table. One hook per copy (buffer_index0 / 1): the table
 * is the calling code's own, whichever copy the port thinks runs. */
static int buffer_lookup(int e, unsigned name) {
  const DdBuffer *b = *g_sbuffers[e];
  const int n = *g_nbuffers[e];
  uint16_t *slot = g_slot[e];
  g_lookups++;
  if (name && name < SLOT_CACHE) {
    const int s = (int)slot[name] - 1;
    if (s >= 0 && s < n && b[s].name == name)
      return s;
  }
  g_misses++;
  for (int i = 0; i < n; i++)
    if (b[i].name == name) {
      if (name && name < SLOT_CACHE && i < 65535)
        slot[name] = (uint16_t)(i + 1);
      return i;
    }
  static int warned;
  if (warned++ < 8)
    debugPrintf("[patch] DDGLRefresh::getBufferIndex: buffer %u is not in engine %d's table\n", name, e + 1);
  return -1;
}
static int buffer_index0(unsigned name) { return buffer_lookup(0, name); }
static int buffer_index1(unsigned name) { return buffer_lookup(1, name); }

/* DDGLRefresh::ddGLDeleteBuffers(n, names), rewritten: the game's clears the
 * slot getBufferIndex gives without looking -- for a buffer not in the table,
 * slot -1, 28 bytes before the table: another allocation's memory and the
 * table's own heap header (its shadow pointer there even deleted). Here a
 * buffer not in the table is only deleted in GL. */
static void (*g_op_delete[2])(void *);
static void (*g_gl_delete_buffers)(GLsizei n, const GLuint *names);
static void delete_buffers(int e, int n, const unsigned *names) {
  DdBuffer *b = *g_sbuffers[e];
  for (int i = 0; i < n && names; i++) {
    const unsigned nm = names[i];
    if (!nm)
      continue;
    const int s = buffer_lookup(e, nm);
    if (s < 0)
      continue;
    if (b[s].shadow && g_op_delete[e])
      g_op_delete[e](b[s].shadow);
    b[s].shadow = NULL;
    b[s].name = 0;
    b[s].used = 0;
  }
  if (!g_gl_delete_buffers)
    g_gl_delete_buffers = (void (*)(GLsizei, const GLuint *))dcr_gl_lookup("glDeleteBuffers");
  if (g_gl_delete_buffers && n > 0 && names)
    g_gl_delete_buffers(n, names);
}
static void delete_buffers0(int n, const unsigned *names) { delete_buffers(0, n, names); }
static void delete_buffers1(int n, const unsigned *names) { delete_buffers(1, n, names); }

static void patch_gl_bookkeeping(so_module *m) {
  const int e = m == &g_mod_game2;
  g_sbuffers[e] = (DdBuffer **)so_try_find_addr_rx(m, "_ZN11DDGLRefresh8sBuffersE");
  g_nbuffers[e] = (int *)so_try_find_addr_rx(m, "_ZN11DDGLRefresh11mNumBuffersE");
  int n = 0, del = 0;
  g_op_delete[e] = (void (*)(void *))so_try_find_addr_rx(m, "_ZdlPv");
  if (g_sbuffers[e] && g_nbuffers[e]) {
    n += jump(m, "_ZN11DDGLRefresh14getBufferIndexEj", 0xe59f3074u, (void *)(e ? buffer_index1 : buffer_index0));
    if (g_op_delete[e])
      del = jump(m, "_ZN11DDGLRefresh17ddGLDeleteBuffersEiPKj", 0xe92d4ff0u,
                 (void *)(e ? delete_buffers1 : delete_buffers0));
  }
  /* ddGLBufferSubData: return right after glBufferSubData (pop {r4-r6, pc}) */
  static const uint32_t ret[] = {0xe8bd8070u};
  n += patch(m, "_ZN11DDGLRefresh17ddGLBufferSubDataEjllPKv", 0x14, 0xe59f3040u, ret, 1);
  static const uint32_t bxlr[] = {BX_LR};
  n += patch(m, "_ZN4GLES14CheckForErrorsEv", 0, 0xe52de004u, bxlr, 1);
  debugPrintf("[patch] GL bookkeeping: %d of 3 (buffer lookups cached, upload shadows and error checks off); "
              "buffer deletes %s\n", n, del ? "checked" : "the game's own");
}

/* ---------------------------------------------------------------- the clock */
static u64 g_clk_at; /* when the refresh count last moved */
static u64 g_refreshes;

/* GetCurrentTime(): the engine's logic clock, in ms (int64, r0:r1). A gap
 * of over half a second is a stall, not game time (the display held by the
 * system -- its controller screen: hardware run, 17 s -- or a long load):
 * one refresh. Else the game would run on by it, and a network session
 * (split screen's LAN) would drop its peer after 10 s without datagrams. */
static long long clock_ms(void) {
  const u64 now = armGetSystemTick();
  if (!g_clk_at)
    g_clk_at = now;
  const u64 gap_ns = armTicksToNs(now - g_clk_at);
  u64 k = (gap_ns * 60 + 500000000ull) / 1000000000ull;
  if (gap_ns > 500000000ull) {
    static int told;
    if (told++ < 8)
      debugPrintf("[patch] the game clock skipped a %llu ms stall\n", (unsigned long long)(gap_ns / 1000000ull));
    k = 1;
  }
  if (k) {
    g_refreshes += k;
    g_clk_at = now;
  }
  return (long long)(g_refreshes * 50 / 3);
}

/* ssr_boot.c, when the game gets the focus back: the time away is no time */
void ssr_clock_resync(void) { g_clk_at = 0; }

/* ---------------------------------------------------------------- at load */
static so_module *g_loading; /* the library being patched (ssr_patch_engine) */

/* for the other modules' hooks at load (ssr_race.c, ssr_menu.c): 1 if made */
int ssr_patch_jump(const char *sym, uint32_t expect, void *dst) {
  return g_loading ? jump(g_loading, sym, expect, dst) : 0;
}

/* the copy of the engine being patched: 0, or 1 (split screen's second) */
int ssr_patch_engine_index(void) { return g_loading == &g_mod_game2; }

/* ---------------------------------------------------------------- hooks with the original */
/* A hook that still calls the function it replaces: the function's first two
 * instructions (the 8 bytes the jump overwrites) are copied into a
 * trampoline, then a jump back to the third. Instructions that read pc there
 * are rewritten for their new place: a literal load becomes a load of the same
 * word from the trampoline's own pool, `add rd, pc, rm` adds the old pc from
 * the pool, a branch jumps to its old target; anything else that touches pc is
 * refused (no hook). The trampolines live in the library's own code, at the
 * end of its executable segment (0x23e904..0x23f000: the page's unused tail,
 * mapped executable with the rest), so they are sealed with it. */
#define POOL_START 0x23e908u
#define POOL_END 0x23f000u
typedef struct {
  so_module *m;
  uint32_t next; /* vaddr */
} Pool;
static Pool g_pool[2];

static uint32_t *stage_at(so_module *m, uint32_t vaddr) { return (uint32_t *)((uint8_t *)m->load_base + vaddr); }
static uint32_t live_at(so_module *m, uint32_t vaddr) { return (uint32_t)(uintptr_t)m->load_virtbase + vaddr; }

/* relocate one instruction at `pc` into out[] (up to 3 words), literals into
 * lit[] (their slots numbered from *nlit); returns the words written, 0 if it
 * cannot be moved */
enum { LIT_MAX = 6 };
typedef struct {
  uint32_t word[12];
  int n;
  struct {
    int at;          /* the instruction (index in word[]) whose offset points at the literal */
    uint32_t value;
  } lit[LIT_MAX];
  int nlit;
} Tramp;

static int tr_lit(Tramp *t, uint32_t value) {
  if (t->nlit >= LIT_MAX)
    return -1;
  t->lit[t->nlit].at = t->n;
  t->lit[t->nlit].value = value;
  return t->nlit++;
}

/* `ldr rd, [pc, #lit]` (patched with the literal's place later) */
static int tr_ldr_lit(Tramp *t, int rd, uint32_t value) {
  if (tr_lit(t, value) < 0)
    return 0;
  t->word[t->n++] = 0xe59f0000u | ((uint32_t)rd << 12); /* offset filled in */
  return 1;
}

static int tr_move(Tramp *t, so_module *m, uint32_t pc_vaddr) {
  const uint32_t ins = *stage_at(m, pc_vaddr);
  const uint32_t pc = live_at(m, pc_vaddr) + 8; /* what the instruction saw as pc */
  const uint32_t cond = ins >> 28;
  if (cond != 0xe)
    return 0; /* conditional: not in a prologue we move */
  /* push (stmdb sp!, {...}) */
  if ((ins & 0x0fff0000u) == 0x092d0000u) {
    t->word[t->n++] = ins;
    return 1;
  }
  /* b / bl */
  if ((ins & 0x0e000000u) == 0x0a000000u) {
    int32_t off = (int32_t)(ins << 8) >> 6;
    const uint32_t target = pc + (uint32_t)off;
    if (ins & 0x01000000u) { /* bl: ldr ip, =target; blx ip */
      if (!tr_ldr_lit(t, 12, target))
        return 0;
      t->word[t->n++] = 0xe12fff3cu;
    } else {
      if (!tr_ldr_lit(t, 15, target))
        return 0;
    }
    return 1;
  }
  const int rn = (ins >> 16) & 15, rd = (ins >> 12) & 15, rm = ins & 15;
  /* ldr / str with an immediate offset */
  if ((ins & 0x0e000000u) == 0x04000000u) {
    if (rd == 15)
      return 0;
    if (rn != 15) {
      t->word[t->n++] = ins;
      return 1;
    }
    /* a literal: word loads only, pre-indexed, no writeback */
    if ((ins & 0x01700000u) != 0x01100000u)
      return 0;
    const uint32_t off = ins & 0xfff;
    const uint32_t addr = (ins & 0x00800000u) ? pc + off : pc - off;
    const uint32_t vaddr = addr - (uint32_t)(uintptr_t)m->load_virtbase;
    return tr_ldr_lit(t, rd, *stage_at(m, vaddr));
  }
  /* data processing (not multiplies / the misc space) */
  if ((ins & 0x0c000000u) == 0) {
    const int imm = (ins >> 25) & 1;
    if (!imm && (ins & 0x90u) == 0x90u)
      return 0; /* extra loads / stores, multiplies */
    const int op = (ins >> 21) & 15;
    const int uses_rn = op != 13 && op != 15; /* MOV, MVN take no rn */
    const int pc_in = (uses_rn && rn == 15) || (!imm && rm == 15);
    if (rd == 15 && op != 10 && op != 11 && op != 8 && op != 9)
      return 0; /* writes pc */
    if (!pc_in) {
      t->word[t->n++] = ins;
      return 1;
    }
    /* add rd, pc, rm (no shift): ip = the old pc; add rd, ip, rm */
    if (op == 4 && !imm && rn == 15 && rm != 15 && rm != 12 && (ins & 0xff0u) == 0) {
      if (!tr_ldr_lit(t, 12, pc))
        return 0;
      t->word[t->n++] = (ins & ~0x000f0000u) | (12u << 16);
      return 1;
    }
    return 0;
  }
  return 0; /* anything else */
}

/* hook `vaddr` (its first word `expect`) to `dst`; returns the original,
 * callable (its trampoline), or NULL (no hook made) */
void *ssr_patch_hook_at(uint32_t vaddr, uint32_t expect, void *dst) {
  so_module *m = g_loading;
  if (!m)
    return NULL;
  Pool *pool = &g_pool[m == &g_mod_game2];
  if (pool->m != m)
    pool->m = m, pool->next = POOL_START;
  if (*stage_at(m, vaddr) != expect) {
    debugPrintf("[patch] hook at 0x%06x: 0x%08x, not 0x%08x: left alone\n", (unsigned)vaddr,
                (unsigned)*stage_at(m, vaddr), (unsigned)expect);
    return NULL;
  }
  Tramp t;
  memset(&t, 0, sizeof t);
  if (!tr_move(&t, m, vaddr) || !tr_move(&t, m, vaddr + 4)) {
    debugPrintf("[patch] hook at 0x%06x: its first instructions cannot be moved (0x%08x 0x%08x)\n", (unsigned)vaddr,
                (unsigned)stage_at(m, vaddr)[0], (unsigned)stage_at(m, vaddr)[1]);
    return NULL;
  }
  /* back to the third instruction */
  if (!tr_ldr_lit(&t, 15, live_at(m, vaddr + 8)))
    return NULL;
  const uint32_t size = (uint32_t)(t.n + t.nlit) * 4;
  if (pool->next + size > POOL_END) {
    debugPrintf("[patch] hook at 0x%06x: no room for its trampoline\n", (unsigned)vaddr);
    return NULL;
  }
  for (uint32_t a = pool->next; a < pool->next + size; a += 4)
    if (*stage_at(m, a) != 0) {
      debugPrintf("[patch] hook at 0x%06x: the trampolines' space is not empty (0x%06x)\n", (unsigned)vaddr,
                  (unsigned)a);
      return NULL;
    }
  const uint32_t base = pool->next;
  /* the literals after the code; each ldr's offset from its pc (+8) */
  for (int i = 0; i < t.nlit; i++) {
    const uint32_t lit_at = base + (uint32_t)(t.n + i) * 4, ins_at = base + (uint32_t)t.lit[i].at * 4;
    const int32_t off = (int32_t)(lit_at - (ins_at + 8)); /* -4 for a literal right after its ldr */
    if (off < 0)
      t.word[t.lit[i].at] = (t.word[t.lit[i].at] & ~0x00800000u) | (uint32_t)-off; /* U clear: pc - off */
    else
      t.word[t.lit[i].at] |= (uint32_t)off;
    t.word[t.n + i] = t.lit[i].value;
  }
  memcpy(stage_at(m, base), t.word, size);
  pool->next = (base + size + 7) & ~7u;
  const uint32_t stub[2] = {0xe51ff004u, (uint32_t)(uintptr_t)dst};
  memcpy(stage_at(m, vaddr), stub, sizeof stub);
  return (void *)(uintptr_t)live_at(m, base);
}

/* by symbol */
void *ssr_patch_hook(const char *sym, uint32_t expect, void *dst) {
  if (!g_loading)
    return NULL;
  const uintptr_t a = so_try_find_addr_rx(g_loading, sym);
  if (!a) {
    debugPrintf("[patch] hook: %s not found\n", sym);
    return NULL;
  }
  return ssr_patch_hook_at((uint32_t)(a - (uintptr_t)g_loading->load_virtbase), expect, dst);
}

/* the word at a library address (vaddr) is `expect` (to patch several
 * together or none) */
int ssr_patch_word_is(uint32_t vaddr, uint32_t expect) {
  return g_loading && *(const uint32_t *)((uint8_t *)g_loading->load_base + vaddr) == expect;
}

/* one word at a library address (vaddr), if it is `expect` */
int ssr_patch_at(uint32_t vaddr, uint32_t expect, uint32_t value) {
  if (!g_loading)
    return 0;
  uint32_t *p = (uint32_t *)((uint8_t *)g_loading->load_base + vaddr);
  if (*p != expect) {
    debugPrintf("[patch] 0x%06x is 0x%08x, not 0x%08x: left alone\n", (unsigned)vaddr, (unsigned)*p, (unsigned)expect);
    return 0;
  }
  *p = value;
  return 1;
}

/* ssr_loader.c: relocated, not yet sealed */
void ssr_patch_engine(so_module *m) {
  const DcrConfig *c = dcr_config();
  g_loading = m;
  if (c->speedups) {
    patch_float(m);
    patch_gl_bookkeeping(m);
  } else {
    debugPrintf("[patch] [performance] engine_speedups = false: the engine's own float maths and GL bookkeeping\n");
  }
  if (c->frame_pacing_display)
    debugPrintf("[patch] frame pacing: %s\n",
                jump(m, "_Z14GetCurrentTimev", 0xe92d4010u, (void *)clock_ms) ? "the display's refreshes"
                                                                              : "the engine's own clock");
  ssr_race_patch(); /* rumble, the tutorial's words */
  ssr_menu_patch(); /* the sign-in banner; [debug] log_touches */
  void ssr_unlock_patch(void);
  ssr_unlock_patch(); /* [game] unlock_all */
  void ssr_split_patch(void);
  ssr_split_patch(); /* split screen's race camera */
  g_loading = NULL;
}

/* ---------------------------------------------------------------- at run time */
/* after nativeSetDeviceMakeModel, which sets the renderer flag from the model */
void ssr_patch_after_make_model_in(int engine) {
  volatile uint8_t *opt = (volatile uint8_t *)ssr_native_in(engine, "g_useOptimisedSceneRenderer");
  if (opt && dcr_config()->optimised_renderer)
    *opt = 1;
  debugPrintf("[patch] scene renderer: %s\n", opt && *opt ? "the optimised one (batched by material)" : "the standard one");
}

/* after nativeProjectInit (SuApplication::Initialise sets the bias in it) */
void ssr_patch_after_make_model(void) { ssr_patch_after_make_model_in(0); }

void ssr_patch_after_init_in(int engine) {
  uint32_t fpscr;
  __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
  if (fpscr & ((1u << 24) | (1u << 25))) { /* FZ, DN: not IEEE */
    fpscr &= ~((1u << 24) | (1u << 25));
    __asm__ volatile("vmsr fpscr, %0" ::"r"(fpscr));
  }
  float *bias = (float *)ssr_native_in(engine, "_ZN15SuRenderTexture26ms_fGlobalMipmapBiasOffsetE");
  const int af = dcr_config()->anisotropy;
  if (bias && af > 1)
    *bias = -0.5f;
  debugPrintf("[patch] FPSCR 0x%08x; textures: %s filtering, mipmap bias %.2f\n", (unsigned)fpscr,
              af > 1 ? "anisotropic" : "trilinear", bias ? (double)*bias : 0.0);
}
void ssr_patch_after_init(void) { ssr_patch_after_init_in(0); }

/* every 10 s (ssr_boot.c's report) */
void ssr_patch_report(void) {
  if (g_lookups)
    debugPrintf("[patch] buffer lookups %u (%u searched)\n", g_lookups, g_misses);
  g_lookups = g_misses = 0;
}

/* ---------------------------------------------------------------- GL */
/* gl_mesa.c: the engine's (and the port's) glTexParameter calls. A mipmapped
 * minification filter gets anisotropy with it. */
typedef void (*t_texparamf)(GLenum, GLenum, GLfloat);
typedef void (*t_texparami)(GLenum, GLenum, GLint);
static t_texparamf g_tpf;
static t_texparami g_tpi;
static float g_aniso = -1.0f; /* -1: not asked yet */

static void set_aniso(GLenum target, GLenum pname, int param) {
  if (pname != GL_TEXTURE_MIN_FILTER || param < GL_NEAREST_MIPMAP_NEAREST || param > GL_LINEAR_MIPMAP_LINEAR)
    return;
  if (g_aniso < 0.0f) {
    g_aniso = 0.0f;
    const int want = dcr_config()->anisotropy;
    const GLubyte *(*get_string)(GLenum) = (void *)eglGetProcAddress("glGetString");
    void (*get_floatv)(GLenum, GLfloat *) = (void *)eglGetProcAddress("glGetFloatv");
    const char *ext = get_string ? (const char *)get_string(GL_EXTENSIONS) : NULL;
    if (want > 1 && ext && strstr(ext, "GL_EXT_texture_filter_anisotropic") && get_floatv) {
      GLfloat max = 0.0f;
      get_floatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max);
      g_aniso = (float)want < max ? (float)want : max;
    }
    debugPrintf("[gfx] anisotropic filtering: %.0fx\n", (double)g_aniso);
  }
  if (g_aniso > 1.0f)
    g_tpf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, g_aniso);
}

static void wrap_texparamf(GLenum target, GLenum pname, GLfloat param) {
  g_tpf(target, pname, param);
  set_aniso(target, pname, (int)param);
}

static void wrap_texparami(GLenum target, GLenum pname, GLint param) {
  g_tpi(target, pname, param);
  set_aniso(target, pname, param);
}

/* the wrapper for a GL name, or 0 (gl_mesa.c asks for each gl* lookup) */
uintptr_t port_gl_wrap(const char *name, uintptr_t real) {
  if (!real)
    return 0;
  if (!strcmp(name, "glTexParameterf")) {
    g_tpf = (t_texparamf)real;
    return (uintptr_t)wrap_texparamf;
  }
  if (!strcmp(name, "glTexParameteri")) {
    g_tpi = (t_texparami)real;
    if (!g_tpf)
      g_tpf = (t_texparamf)eglGetProcAddress("glTexParameterf");
    return (uintptr_t)wrap_texparami;
  }
  return 0;
}
