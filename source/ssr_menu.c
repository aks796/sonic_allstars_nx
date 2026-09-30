/* ssr_menu.c -- the game's touch menus with a controller: a focus on the
 * game's own buttons (../source/docs/MENUS.md).
 *
 * Every tappable thing in the menus is a SumoTool pointer area (PNTR), which
 * the screens hit-test by hash; the engine keeps every created object in one
 * tree, and its own hit test leaves an area's four corners, transformed to
 * the screen, in the object. So each frame the port can ask where any button
 * is right now (sliding in, animating) exactly as the game sees it. Which
 * buttons count, and what A, B, L/R, X/Y mean, depends on the screen: a
 * profile per screen class (matched by vtable; seven screens never set their
 * hash), keyed on the screen's own input state so nothing is pressed while it
 * animates. Models:
 *
 *   CAR       a card carousel (the main menu and most others): left / right
 *             (D-pad, stick, L / R) turn it, A picks the front card -- a tap
 *             on it, the game's own release path (locks, sounds);
 *   DAVE      character select: left / right the arrows, A the front racer,
 *             Y the stats;
 *   FOCUS     a focus frame (ssr_gfx.c) on the screen's buttons, moved in 8
 *             directions by the buttons' positions (the Angry Birds Space
 *             port's rule: in line first, then within 60 degrees; a diagonal
 *             takes the nearest that way), A taps the focused button;
 *   LIST      up / down scroll a list (records, achievements);
 *   SLIDERS   up / down choose a slider, left / right move it (audio,
 *             sensitivity);
 *   RULES     up / down a row, left / right its value;
 *   KEYBOARD  the licence name: the Switch keyboard, then the game's own DONE;
 *             cancelled, the game's key grid with the focus;
 *   TAP       "tap anywhere" pages: A continues;
 *   WALK      any other screen: the focus over every live button.
 * Pop-ups (the stack's current one, by class and its button mask) and the
 * dialogs some screens draw themselves take the focus while they are up, on
 * their harmless button (NO, cancel). B is Back where the game reads Android's
 * Back, a tap on the screen's own back / NO / cancel where it does not, and
 * nothing where Back would do harm (the title screen quits on it; under the
 * tutorial's pop-up it would pause).
 *
 * A press is the game's own: a finger down at the button's centre for two
 * logic ticks, then up, then two ticks before the next (screens act on a
 * fresh touch, some on a held one: never longer). Carousels, lists and
 * sliders are also driven by the engine's own functions where a tap cannot
 * (a scroll, a slider's value), under the guards the game itself observes.
 *
 * The right stick brings out a free pointer at any time (A touches, hold to
 * drag); the left stick or the D-pad brings the focus back. The touchscreen
 * always works as on the phone, and hides the focus until the controller is
 * used again. MIT.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "ssr.h"
#include "util.h"

/* The same driver runs player 2's copy of the engine in split screen
 * (ssr_menu2.c: this file again with MENU_ENGINE 1): its menus, player 2's
 * pad, their column's prompts and focus. */
#ifndef MENU_ENGINE
#define MENU_ENGINE 0
#define MENU_BACK() ssr_input_back()
#define MENU_PROMPT_SET(p) ssr_prompt_set(p)
#define MENU_PROMPT_PLACE(y) ssr_prompt_place(y)
#define MENU_PROMPT_ICON(i, x, y, h, a) ssr_prompt_icon(i, x, y, h, a)
#define MENU_FOCUS(on, x, y, w, h) ssr_gfx_focus(on, x, y, w, h)
#endif
#define MENU_NATIVE(s) ssr_native_in(MENU_ENGINE, s)

/* ================================================================ the engine */
typedef void *(*fn_top)(const void *stack);
typedef void *(*fn_byhash)(const void *stack, uint32_t hash);
typedef uint8_t *(*fn_getpntr)(void *om, uint32_t hash);
typedef int (*fn_hit)(void *obj, int x, int y);
typedef void (*fn_this)(void *self);
typedef int (*fn_curpage)(const void *car, uint8_t *wrapped);
typedef void (*fn_sound)(uint32_t event, uint32_t group, int flag);
typedef void (*fn_setstr)(void *gen, const char *s);
typedef int (*fn_bool0)(void);

static struct {
  void **stack;
  fn_top top;
  fn_byhash byhash;
  uint8_t **popups; /* &g_pPopUpStack */
  fn_bool0 loading; /* LoadingScreen::IsVisible (static) */
  void *om;         /* &g_object_manager */
  fn_getpntr pntr;
  fn_hit hit;
  fn_this car_next, car_prev, list_next, list_prev, slider_refresh;
  fn_curpage car_page;
  fn_sound sound;
  const uint32_t *sfx; /* STAudio_Group_SoundEffects */
  fn_setstr set_string;
  const volatile uint32_t *ticks; /* SuApplication::ms_frameCounter: logic ticks */
  uint8_t **game;                 /* &g_pGame */
  void *sm;                       /* &g_scene_manager */
  uint8_t *(*getobj)(void *sm, uint32_t hash); /* SceneManager::GetObject */
  const volatile uint8_t *footer; /* GlobalScreen::ms_bFooterVisible */
  const volatile uint8_t *touched, *touched_last; /* SuApplication::ms_touched[0], ms_touchedLastFrame[0] */
  fn_this lb_rotate, lb_end, lb_finish; /* Listbox::UpdateRotating, OnEndDragEvent, FinishRotating */
  void (*dave_rotate)(void *car, int steps, int b1, int b2); /* DavesBaseCarousel::RotateCarousel */
  fn_this ch_next, ch_prev, ch_update, ch_request;           /* LobbyScreen: its racer picker */
  int (*ch_locked)(void *ls, uint32_t index);
  int (*str_width)(void *sm, uint32_t obj); /* SceneManager::GetStringPixelWidth: a text's, in font pixels */
} E;

#define SND_TICK 0xfb699f5au

static void sound(uint32_t ev) {
  if (E.sound && E.sfx)
    E.sound(ev, *E.sfx, 0);
}

static uint32_t ticks_now(void) {
  static uint32_t frames;
  return E.ticks ? *E.ticks : ++frames;
}

/* ================================================================ rectangles */
typedef struct {
  uint32_t hash;
  float x0, y0, x1, y1; /* window pixels, y down */
} Rect;

static int g_w = 1280, g_h = 720;

/* the engine's own liveness tests, in its order (MENUS.md 1.4) */
static int area_live(const uint8_t *o) {
  const uint8_t *p = *(uint8_t *const *)(o + 0xf0);
  if (p && !p[0xde])
    return 0;
  if (!o[0xde] || !*(const uint16_t *)(o + 0xdc))
    return 0;
  return *(const uint16_t *)((p ? p : o) + 0x4e) != 0;
}

static int area_rect(uint8_t *o, Rect *r) {
  if (!o || !area_live(o))
    return 0;
  E.hit(o, -99999, -99999); /* misses; leaves the corners at o+0x114 */
  const float *c = (const float *)(o + 0x114);
  const uint8_t *s = *(uint8_t *const *)(o + 0x104);
  if (!s)
    return 0;
  const float sx = *(const float *)(s + 0x3c), sy = *(const float *)(s + 0x40);
  const float ox = *(const float *)(s + 0x44), oy = *(const float *)(s + 0x48);
  r->x0 = r->y0 = 1e9f;
  r->x1 = r->y1 = -1e9f;
  for (int i = 0; i < 4; i++) {
    const float x = c[4 * i] * sx + ox, y = c[4 * i + 1] * sy + oy;
    r->x0 = fminf(r->x0, x), r->x1 = fmaxf(r->x1, x);
    r->y0 = fminf(r->y0, y), r->y1 = fmaxf(r->y1, y);
  }
  r->hash = *(const uint32_t *)(o + 0x14);
  /* on the screen, not animated off it, and not degenerate */
  return r->x1 > 1 && r->y1 > 1 && r->x0 < g_w - 1 && r->y0 < g_h - 1 && r->x1 - r->x0 > 2 && r->y1 - r->y0 > 2;
}

static int button_rect(uint32_t h, Rect *r) { return E.pntr && area_rect(E.pntr(E.om, h), r); }

static float cx(const Rect *r) { return (r->x0 + r->x1) * 0.5f; }
static float cy(const Rect *r) { return (r->y0 + r->y1) * 0.5f; }

/* ================================================================ the items */
#define MAXI 72
static Rect g_it[MAXI];
static int g_ni;

static void items_clear(void) { g_ni = 0; }
static void items_add(uint32_t h) {
  if (g_ni < MAXI && button_rect(h, &g_it[g_ni]))
    g_ni++;
}
static void items_from(const uint32_t *hs) {
  for (; hs && *hs; hs++)
    items_add(*hs);
}

/* every live pointer area (screens without a profile); node = object + 4 */
static void walk(uint8_t *node, int depth) {
  if (!node || depth > 48 || g_ni >= MAXI)
    return;
  walk(*(uint8_t **)(node + 4), depth + 1);
  uint8_t *o = node - 4;
  const uint32_t *te = *(const uint32_t **)(o + 0xe8);
  if (g_ni < MAXI && te && te[1] == 0x52544e50u /* PNTR */ && area_rect(o, &g_it[g_ni]))
    g_ni++;
  walk(*(uint8_t **)(node + 8), depth + 1);
}

/* the HUD's and dialogs' areas no screen reads (MENUS.md 1.5) */
static const uint32_t k_dead[] = {0xd8f3ffd2, 0x34d7afae, 0x84b48a0f, 0x048f604a, 0x59d1920a, 0xff8d918a,
                                  0x7916c193, 0xffa690a9, 0x688e600d, 0x2de243ab, 0x3d0d48b1, 0};

static void items_walk(void) {
  g_ni = 0;
  if (E.om)
    walk(*(uint8_t **)E.om, 0);
  for (int i = 0; i < g_ni;) {
    int dead = 0;
    for (const uint32_t *d = k_dead; *d && !dead; d++)
      dead = g_it[i].hash == *d;
    if (dead)
      g_it[i] = g_it[--g_ni];
    else
      i++;
  }
}

static int item_index(uint32_t h) {
  for (int i = 0; i < g_ni; i++)
    if (g_it[i].hash == h)
      return i;
  return -1;
}

/* ================================================================ taps */
#define F_TAP 0x2002u
#define F_POINTER 0x2000u
static struct {
  int phase; /* 0 idle, 1 wanted / down, 2 cooling down */
  int x, y;
  int down;  /* the finger has gone down (at tick t) */
  uint32_t t;
} g_tap;

static int tap_busy(void) { return g_tap.phase != 0; }

static void tap_at(float x, float y) {
  if (g_tap.phase)
    return;
  g_tap.phase = 1, g_tap.down = 0;
  g_tap.x = (int)x, g_tap.y = (int)y;
  if (dcr_config()->log_touch)
    debugPrintf("[menu] tap at %d,%d\n", g_tap.x, g_tap.y);
}
static void tap_rect(const Rect *r) { tap_at(cx(r), cy(r)); }
static int tap_hash(uint32_t h) {
  Rect r;
  if (!button_rect(h, &r))
    return 0;
  tap_rect(&r);
  return 1;
}
static void tap_norm(float x, float y) { tap_at(x * (float)g_w, y * (float)g_h); }
int ssr_menu_tap(uint32_t h) { return !tap_busy() && tap_hash(h); }

/* the tap's finger, if it is down this frame: down through two logic ticks
 * (the runs of this frame and the next), then up, then two ticks without it,
 * so the next press is a fresh one */
static void tap_frame(SsrFinger *want, int *n, int cap) {
  const uint32_t now = ticks_now();
  if (g_tap.phase == 1) {
    if (!g_tap.down) {
      g_tap.down = 1, g_tap.t = now;
    } else if (now - g_tap.t >= 2) {
      g_tap.phase = 2, g_tap.t = now;
      return;
    }
    if (*n < cap)
      want[(*n)++] = (SsrFinger){F_TAP, g_tap.x, g_tap.y};
  } else if (g_tap.phase == 2 && now - g_tap.t >= 2) {
    g_tap.phase = 0;
  }
}

/* ================================================================ the pad */
/* 8-way navigation from the D-pad and the left stick: a press, then repeats */
static struct {
  int dir;       /* 0 none, else 1 + index into k_dirs */
  u64 next;      /* when it repeats */
  int page;      /* L / R: -1, 1 */
  u64 page_next;
} g_nav;
static const float k_dirs[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};

static int dir_of(const SsrPad *p) {
  float x = 0, y = 0; /* y down */
  if (p->held & HidNpadButton_Left)
    x -= 1;
  if (p->held & HidNpadButton_Right)
    x += 1;
  if (p->held & HidNpadButton_Up)
    y -= 1;
  if (p->held & HidNpadButton_Down)
    y += 1;
  if (x == 0 && y == 0 && p->lx * p->lx + p->ly * p->ly > 0.5f * 0.5f) {
    const float a = atan2f(-p->ly, p->lx); /* stick up is positive: flip to y down */
    const int oct = (int)lrintf(a / (float)(M_PI / 4)) & 7; /* 0 right, 2 down, 4 left, 6 up */
    static const float ox[8] = {1, 1, 0, -1, -1, -1, 0, 1}, oy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    x = ox[oct], y = oy[oct];
  }
  for (int i = 0; i < 8; i++)
    if (k_dirs[i][0] == x && k_dirs[i][1] == y)
      return i + 1;
  return 0;
}

/* a navigation event this frame: 1 + direction index, or 0 */
static int nav_event(const SsrPad *p) {
  const int d = dir_of(p);
  const u64 now = armGetSystemTick();
  if (!d) {
    g_nav.dir = 0;
    return 0;
  }
  if (d != g_nav.dir) {
    g_nav.dir = d;
    g_nav.next = now + armNsToTicks(380000000ull);
    return d;
  }
  if (now >= g_nav.next) {
    g_nav.next = now + armNsToTicks(130000000ull);
    return d;
  }
  return 0;
}

/* L / R (and ZL / ZR): -1 / 1 on a press or a repeat */
static int page_event(const SsrPad *p) {
  const int held = (p->held & (HidNpadButton_R | HidNpadButton_ZR)) ? 1
                   : (p->held & (HidNpadButton_L | HidNpadButton_ZL)) ? -1
                                                                      : 0;
  const u64 now = armGetSystemTick();
  if (!held) {
    g_nav.page = 0;
    return 0;
  }
  if (held != g_nav.page) {
    g_nav.page = held;
    g_nav.page_next = now + armNsToTicks(380000000ull);
    return held;
  }
  if (now >= g_nav.page_next) {
    g_nav.page_next = now + armNsToTicks(200000000ull);
    return held;
  }
  return 0;
}

/* ================================================================ the focus */
static struct {
  const void *ctx; /* the screen / pop-up it belongs to */
  int key;         /* ...and its state */
  uint32_t hash;
  Rect r;
  int has;
  int hidden; /* the touchscreen was used: no frame until the controller is */
} F;
static int g_show; /* the frame is drawn this frame */

/* per screen: where the focus was, to go back to it */
#define NMEM 24
static struct {
  const void *ctx;
  int key;
  uint32_t hash;
} g_mem[NMEM];
static int g_mem_next;

static void mem_save(void) {
  if (!F.ctx || !F.has)
    return;
  int slot = -1;
  for (int i = 0; i < NMEM; i++)
    if (g_mem[i].ctx == F.ctx && g_mem[i].key == F.key)
      slot = i;
  if (slot < 0)
    slot = g_mem_next++ % NMEM;
  g_mem[slot].ctx = F.ctx, g_mem[slot].key = F.key, g_mem[slot].hash = F.hash;
}

static uint32_t mem_get(const void *ctx, int key) {
  for (int i = 0; i < NMEM; i++)
    if (g_mem[i].ctx == ctx && g_mem[i].key == key)
      return g_mem[i].hash;
  return 0;
}

/* a new screen or state: the focus it had, or its default, or the first item */
static void focus_enter(const void *ctx, int key, uint32_t dflt) {
  if (F.ctx != ctx || F.key != key) {
    mem_save();
    F.ctx = ctx, F.key = key, F.has = 0;
    const uint32_t m = mem_get(ctx, key);
    F.hash = m ? m : dflt;
  }
}

/* the focused item in this frame's list, or a replacement */
static void focus_refind(uint32_t dflt) {
  int i = F.hash ? item_index(F.hash) : -1;
  if (i < 0 && F.has) { /* it went: the nearest to where it was */
    float best = 1e18f;
    for (int k = 0; k < g_ni; k++) {
      const float dx = cx(&g_it[k]) - cx(&F.r), dy = cy(&g_it[k]) - cy(&F.r), d = dx * dx + dy * dy;
      if (d < best)
        best = d, i = k;
    }
  }
  if (i < 0 && dflt)
    i = item_index(dflt);
  if (i < 0 && g_ni) { /* the first in reading order */
    i = 0;
    for (int k = 1; k < g_ni; k++)
      if (g_it[k].y0 < g_it[i].y0 - 8 || (fabsf(g_it[k].y0 - g_it[i].y0) <= 8 && g_it[k].x0 < g_it[i].x0))
        i = k;
  }
  if (i >= 0) {
    F.hash = g_it[i].hash, F.r = g_it[i], F.has = 1;
  } else {
    F.has = 0;
  }
}

/* how far apart two spans are on one axis (0 when they overlap) */
static float span_gap(float a0, float a1, float b0, float b1) { return b0 > a1 ? b0 - a1 : a0 > b1 ? a0 - b1 : 0.0f; }

/* The best item from the focus in direction (dx, dy), or -1 (the Angry Birds
 * Space port's rule): straight -- in line first (the boxes overlap across the
 * move, or within 35 degrees), the nearest edge to edge, off to the side
 * counting double; then anything within 60 degrees. Diagonal -- anything that
 * way on both axes, the nearest, those nearer the diagonal first. */
static int step(float dx, float dy) {
  if (!F.has)
    return -1;
  const int straight = dx == 0 || dy == 0;
  const float len = sqrtf(dx * dx + dy * dy);
  dx /= len, dy /= len;
  const float fx = cx(&F.r), fy = cy(&F.r);
  int best = -1, wide = -1;
  float bs = 1e18f, ws = 1e18f;
  for (int i = 0; i < g_ni; i++) {
    const Rect *it = &g_it[i];
    if (it->hash == F.hash)
      continue;
    const float vx = cx(it) - fx, vy = cy(it) - fy;
    const float along = vx * dx + vy * dy, across = fabsf(vx * dy - vy * dx);
    if (along < 6.0f)
      continue;
    if (straight) {
      float edge = dx > 0 ? it->x0 - F.r.x1 : dx < 0 ? F.r.x0 - it->x1 : dy > 0 ? it->y0 - F.r.y1 : F.r.y0 - it->y1;
      if (edge < 0)
        edge = 0;
      const float side = dx != 0 ? span_gap(F.r.y0, F.r.y1, it->y0, it->y1) : span_gap(F.r.x0, F.r.x1, it->x0, it->x1);
      const float score = edge + side * 2.0f + across * 0.25f;
      if (side == 0 || across <= along * 0.70f) {
        if (score < bs)
          bs = score, best = i;
      } else if (across <= along * 1.73f && score < ws) {
        ws = score, wide = i;
      }
    } else {
      if (vx * dx <= 4.0f || vy * dy <= 4.0f)
        continue;
      const float dist = sqrtf(vx * vx + vy * vy);
      const float score = dist * (2.0f - along / dist);
      if (score < bs)
        bs = score, best = i;
    }
  }
  return best >= 0 ? best : wide;
}

/* D-pad / stick: move the focus; 1 if it moved */
static int focus_move(int ev) {
  if (!ev || !F.has)
    return 0;
  const int i = step(k_dirs[ev - 1][0], k_dirs[ev - 1][1]);
  if (i < 0)
    return 0;
  F.hash = g_it[i].hash, F.r = g_it[i];
  sound(SND_TICK);
  return 1;
}

/* ================================================================ profiles */
enum { M_NONE, M_TAP, M_CAR, M_DAVE, M_LIST, M_FOCUS, M_SLIDERS, M_RULES, M_KEYBOARD, M_LOBBY, M_WALK };
enum { BK_BACK, BK_TAP, BK_SWALLOW };
enum { DLG_NONE, DLG_YESNO, DLG_OK, DLG_ANY };

#define L_NO 0x3bcad1dfu  /* a dialog's left button (NO, cancel) */
#define R_YES 0xae8100e1u /* its right button (YES, OK, buy) */

static const uint32_t k_controls[] = {0x23f8bbf7, 0x18e8474f, 0x09a613eb, 0x26555911, 0};
static const uint32_t k_license[] = {0xab305dd4, 0xcc015e58, 0x7608760b, 0};
static const uint32_t k_pause[] = {0xaa93b938, 0xc46c3f01, 0x3633e0a5, 0xaa1d2eed, 0};
static const uint32_t k_portrait[] = {0xe18b6c6c, 0xd1eea33c, 0x43c610bd, 0xc995b59e, 0x922458ba, 0x8ea41503,
                                      0xa52311cb, 0x7f641cb2, 0x79644a9e, 0x9945d58a, 0x153af7f3, 0x466a6eb7, 0};
static const uint32_t k_nation[] = {
    0x3fd6df5a, 0x3717f599, 0xc2d51ed8, 0x95b48e27, 0x7232a81f, 0x80fe5925, 0x3ef157ad, 0xf6b4d6c3,
    0x42619675, 0xd3a22ead, 0x65187f56, 0x6b8a3c4e, 0x651a7758, 0x6b7a3cdf, 0x467e9cad, 0x8c1db4d1,
    0x025a7c0d, 0xb25de375, 0x5b6cde53, 0xfefc93a1, 0x1716133d, 0x53805659, 0x16b4d695, 0x61b7e2ed,
    0xf29b45eb, 0x5b27d4b0, 0x9b10da5e, 0x243ac513, 0x3bf17e5b, 0xf62273e6, 0};
#define K_SPACE 0xcfdbc712u
#define K_DEL 0xa4923676u
#define K_DONE 0xe3cae86eu
static const uint32_t k_keys[] = {
    0xdea8bfe4, 0xd4f69a4f, 0xcbbb1305, 0x6652812b, 0x50c330ef, 0xa4865e89, 0x822bdc46, 0xb6fd31c4, 0x38be2c33,
    0x8449462f, 0x79776008, 0xffef5ece, 0x1fe1e9c1, 0x30db2f52, 0x05aaf495, 0x2414a992, 0x3385c8c5, 0x3909ea9e,
    0xf743a0e8, 0x539a0938, 0x487ac101, 0xed08dd35, 0x917a8330, 0x8851261d, 0xca268862, 0x49d92381, 0xa7b7cf02,
    0x1d642710, 0xfc62c7fd, 0xcc6b7ef8, 0x6b092a2b, 0x2898480b, 0x157f1164, 0x2d7a0fc6, 0xceffd227, 0xe20ac74d,
    K_SPACE,    K_DEL,      K_DONE,     0};
static const uint32_t k_seg_sdk[] = {0xae5586e1, 0}; /* "as a guest" (not Facebook) */
/* the LOCAL multiplayer's list of games found (MENUS.md 2: its six rows) */
static const uint32_t k_servers[] = {0xff8fbfb7, 0x4dc17907, 0xf5c4167c, 0x1bfb19a3, 0x2f0acb70, 0xaa090823, 0};

/* sliders: member offset, grab area, step */
typedef struct {
  uint16_t off;
  uint32_t grab;
} SliderDef;
static const SliderDef k_audio[] = {{0x14, 0xd0db7f3b}, {0x6c, 0xd78b7713}, {0xc4, 0xd2879d5c}, {0, 0}};
static const SliderDef k_calib[] = {{0x14, 0x5f1597bc}, {0, 0}};

/* the rules: each row's - and + */
static const uint32_t k_rules[5][2] = {{0xb8bd1be8, 0x6807014f}, {0x6945f584, 0x97e69bc0}, {0x97a53991, 0x42e39bb5},
                                       {0xa0d6fa85, 0x569a1466}, {0xf66803fe, 0xc263479f}};

typedef struct {
  const char *cls;   /* the class (its vtable: _ZTV<len><cls> + 8) */
  uint16_t st_off;   /* its input state (u32) */
  int8_t st_on;      /* ...when it takes input; -1: always */
  uint8_t model;
  uint16_t sub;      /* carousel / list / character carousel member */
  uint8_t nowrap;    /* a carousel that stops at its ends */
  const void *data;  /* FOCUS: hashes; SLIDERS: SliderDef[] */
  uint32_t extra_x, extra_y; /* X / Y tap these */
  uint8_t back;
  uint32_t back_hash;
  int8_t dlg_state;  /* a dialog of its own in this state */
  uint8_t dlg;
  uint32_t dlg_default;
  float tx, ty;      /* TAP: where (normalised) */
  const void *vt;    /* resolved */
} Profile;

static Profile k_prof[] = {
    {"TitleScreen", 0xc, 4, M_TAP, .back = BK_SWALLOW, .tx = 0.5f, .ty = 0.5f},
    {"MainMenu", 0x6c, 3, M_CAR, 0xc},
    {"SoloMenu", 0x6c, 3, M_CAR, 0xc},
    {"GPDiffMenu", 0x6c, 4, M_CAR, 0xc, 1},
    {"GPSelectMenu", 0x70, 4, M_CAR, 0x10, 1},
    {"TrackSelectMenu", 0x70, 3, M_CAR, 0xc, .extra_x = 0x5919b6c7, .extra_y = 0x5919b6c7},
    {"MissionSelectMenu", 0x70, 5, M_CAR, 0xc, 1, .extra_x = 0x7ca98756, .extra_y = 0x7ca98756, .dlg_state = 10,
     .dlg = DLG_OK},
    {"CharacterSelectScreen", 0xdc, 2, M_DAVE, 0x38},
    {"OptionsMenu", 0x6c, 3, M_CAR, 0xc},
    {"ControlsMenu", 0xc, 2, M_FOCUS, .data = k_controls},
    {"CalibrationScreen", 0xc, 2, M_SLIDERS, .data = k_calib},
    {"AudioMenu", 0xc, 2, M_SLIDERS, .data = k_audio},
    {"RulesMenu", 0xc, 2, M_RULES, .back = BK_TAP, .back_hash = 0xa38896dd},
    {"LicenseMenu", 0x6c, 3, M_CAR, 0xc},
    {"SelectLicenseMenu", 0x70, 3, M_CAR, 0x10, .dlg_state = 7, .dlg = DLG_YESNO, .dlg_default = L_NO},
    {"CreateLicenseScreen", 0xc, 11, M_NONE, .back = BK_SWALLOW, .dlg_state = 11, .dlg = DLG_YESNO,
     .dlg_default = R_YES},
    {"KeyboardScreen", 0x420, 3, M_KEYBOARD, .data = k_keys, .back = BK_TAP, .back_hash = K_DEL, .dlg_state = 8,
     .dlg = DLG_OK},
    {"SelectPortraitScreen", 0x14, 3, M_FOCUS, .data = k_portrait, .back = BK_SWALLOW},
    {"SelectNationalityScreen", 0x14, 3, M_FOCUS, .data = k_nation, .back = BK_SWALLOW},
    {"LicenseScreen", 0xc, 4, M_FOCUS, .data = k_license, .dlg_state = 9, .dlg = DLG_YESNO, .dlg_default = L_NO},
    {"RecordsScreen", 0xc, 3, M_LIST, 0x18},
    {"RecordsListboxScreen", 0xc, 7, M_TAP, .tx = 0.5f, .ty = 0.75f},
    {"ChallengesScreen", 0xc, 2, M_LIST, 0x14},
    {"ShoppingScreen", 0x6c, 3, M_CAR, 0xc},
    {"ShoppingMenu", 0x70, 3, M_CAR, 0x10, .dlg_state = 9, .dlg = DLG_YESNO, .dlg_default = L_NO},
    {"ResultsScreenGP", 0x10, 3, M_TAP, .tx = 0.5f, .ty = 0.5f},
    {"ResultsScreenMission", 0, -1, M_TAP, .tx = 0.5f, .ty = 0.5f},
    {"ResultsScreenTimeTrial", 0, -1, M_TAP, .tx = 0.5f, .ty = 0.5f},
    {"SegaMilesScreen", 0x18, 3, M_TAP, .extra_x = 0xa32ca044, .tx = 0.5f, .ty = 0.85f},
    {"ChallengeReportScreen", 0x14, 8, M_TAP, .tx = 0.5f, .ty = 0.5f},
    {"MultiplayerServiceMenu", 0x6c, 3, M_CAR, 0xc, 1},
    {"NetMenu", 0x6c, 4, M_CAR, 0xc},
    {"LobbyScreen", 0xc, -1, M_LOBBY},
    {"SelectServerScreen", 0xc, 2, M_FOCUS, .data = k_servers},
    {"SegaSDKScreen", 0x10, 4, M_FOCUS, .data = k_seg_sdk, .back = BK_SWALLOW},
    {"SplashScreen", 0, -1, M_NONE},
    {"LoadingScreen", 0, -1, M_NONE},
    {"PostGameHandler", 0, -1, M_NONE},
};
#define NPROF ((int)(sizeof k_prof / sizeof k_prof[0]))

/* pop-ups, by class: the default focus and what B does */
enum { PB_BACK, PB_NO, PB_RIGHT, PB_NONE };
typedef struct {
  const char *cls;
  uint8_t b;      /* B */
  uint8_t a_right; /* A's default: the right button (else the left, or the panel) */
  const void *vt;
} PopDef;
static PopDef k_pop[] = {
    {"PausePopUp", PB_NO, 0},        {"WaitingForPlayersPopUp", PB_RIGHT, 1}, {"ConnectingPopUp", PB_BACK, 1},
    {"ConnectionLostPopUp", PB_RIGHT, 1}, {"TutorialPopUp", PB_NONE, 1},
};
#define NPOP ((int)(sizeof k_pop / sizeof k_pop[0]))
static const void *g_vt_tutorial, *g_vt_hud;

/* ================================================================ context */
enum { C_NONE, C_RACE, C_PAUSE, C_POPUP, C_SCREEN, C_DIALOG, C_WALK };
static const char *g_ctx_name = "";

static void back_key(void) { MENU_BACK(); }

/* ================================================================ pop-ups */
static uint32_t g_pop_seen_tick;
static const void *g_pop_seen;

/* the pop-up that takes input now: 1 (in *pp), 0 none, -1 one is coming or
 * going (no input) */
static int popup_live(uint8_t **pp) {
  uint8_t *s = E.popups ? *E.popups : NULL;
  if (!s || !*(const uint32_t *)(s + 0x14))
    return 0;
  if (*(const uint32_t *)s != 3)
    return -1;
  const int cur = *(const int32_t *)(s + 0x18);
  uint8_t *p = cur >= 0 && cur < 3 ? *(uint8_t **)(s + 8 + 4 * cur) : NULL;
  if (!p || *(const uint32_t *)(p + 4) != 2)
    return -1;
  *pp = p;
  return 1;
}

static void popup_frame(uint8_t *p, const SsrPad *pad, int nav) {
  const uint32_t mask = *(const uint32_t *)(p + 0x2c);
  const void *vt = *(void **)p;
  const PopDef *d = NULL;
  for (int i = 0; i < NPOP; i++)
    if (k_pop[i].vt == vt)
      d = &k_pop[i];
  if (g_pop_seen != p) {
    g_pop_seen = p;
    g_pop_seen_tick = ticks_now();
  }
  items_clear();
  if (mask & 1)
    items_add(L_NO);
  if (mask & 2)
    items_add(R_YES);
  const uint32_t dflt = (mask & 2) && (!d || d->a_right) ? R_YES : (mask & 1) ? L_NO : 0;
  focus_enter(p, (int)mask, dflt);
  focus_refind(dflt);
  focus_move(nav);
  g_show = F.has;
  /* the tutorial's pop-up must stay up a second (MENUS.md 3.1) */
  const int early = vt == g_vt_tutorial && ticks_now() - g_pop_seen_tick < 75;
  if ((pad->down & (HidNpadButton_A | HidNpadButton_Plus)) && !early && !tap_busy()) {
    if (F.has)
      tap_rect(&F.r);
    else if (mask & 0x14) /* the panel, or anywhere */
      tap_norm(0.5f, 0.47f);
  }
  if ((pad->down & HidNpadButton_B) && !tap_busy()) {
    /* Back only where the pop-up takes it (flags 4 / 8; the lobby's
     * "connecting"): elsewhere it would fall through to the screen below */
    const int b = d ? d->b : (mask & 0xc) ? PB_BACK : (mask & 1) ? PB_NO : PB_NONE;
    if (b == PB_BACK)
      back_key();
    else if (b == PB_NO)
      tap_hash(L_NO);
    else if (b == PB_RIGHT)
      tap_hash(R_YES);
  }
}

/* ================================================================ the models */
static void car_frame(uint8_t *scr, const Profile *pf, const SsrPad *p, int nav) {
  uint8_t *car = scr + pf->sub;
  const int idle = *(const uint32_t *)(car + 0x38) == 0 && car[0x34];
  int turn = page_event(p);
  if (nav == 1 || nav == 5 || nav == 7)
    turn = -1;
  else if (nav == 2 || nav == 6 || nav == 8)
    turn = 1;
  if (turn && idle && !tap_busy() && E.car_next && E.car_prev) {
    int ok = 1;
    if (pf->nowrap && E.car_page) {
      const int idx = E.car_page(car, NULL), n = *(const int32_t *)(car + 4);
      ok = turn > 0 ? idx < n - 1 : idx > 0;
    }
    if (ok) {
      (turn > 0 ? E.car_next : E.car_prev)(car);
      sound(SND_TICK);
    }
  }
  /* the front card is the choice (no highlight: the carousel shows it);
   * split screen's Solo menu has no TIME TRIAL, no MISSIONS (BATTLE and VS
   * RACE there) */
  Rect r;
  const int front = button_rect(0xdc040b64, &r);
  const int solo_page = MENU_ENGINE == 0 && !strcmp(pf->cls, "SoloMenu") && E.car_page ? E.car_page(car, NULL) : -1;
  const int blocked = solo_page >= 0 && ssr_split_solo_blocked(solo_page);
  if ((p->down & HidNpadButton_A) && idle && !tap_busy() && !blocked) {
    if (front)
      tap_rect(&r);
    else
      tap_norm(0.5f, 0.49f);
  } else if ((p->down & HidNpadButton_A) && idle && blocked && !tap_busy()) {
#if MENU_ENGINE == 0
    /* split screen's BATTLE (the TIME TRIAL card) and VS RACE (the MISSIONS
     * card): the LOCAL games, ssr_split_net.c */
    ssr_split_net_choose(solo_page == 2);
    sound(SND_TICK);
#endif
  }
  const uint32_t extra = (p->down & HidNpadButton_X) ? pf->extra_x : (p->down & HidNpadButton_Y) ? pf->extra_y : 0;
  if (extra && idle && !tap_busy())
    tap_hash(extra);
}

/* The racer carousel (DavesBaseCarousel) turns in a cubic of 0.5 s a step
 * (+0.25 s a step still to go, +0.2 s each further step: InitCarousel's
 * DavesCarouselInit), and a step taken while it turns adds to the turn: so
 * flicks are turns of the carousel itself (RotateCarousel, as its arrows do),
 * taken at once, and the turns are quicker -- for fingers too. */
static struct {
  int dir;
  u64 next;
} g_dave;

static void dave_frame(uint8_t *cs, const SsrPad *p, int nav) {
  uint8_t *car = cs + 0x38;
  float *spin = (float *)(car + 0x48); /* base, per step to go, per further step */
  if (spin[0] > 0.2f)
    spin[0] = 0.2f, spin[1] = 0.1f, spin[2] = 0.08f;
  const uint32_t st = *(const uint32_t *)(car + 0x10);
  const int ready = (st == 1 || st == 2) && !car[0xd] && *(const uint32_t *)(cs + 0x28) == 0;
  /* left / right (stick, D-pad, L / R): a step now, then every 110 ms held */
  (void)nav;
  int dir = 0;
  if ((p->held & (HidNpadButton_Left | HidNpadButton_L | HidNpadButton_ZL)) || p->lx < -0.5f)
    dir = -1;
  else if ((p->held & (HidNpadButton_Right | HidNpadButton_R | HidNpadButton_ZR)) || p->lx > 0.5f)
    dir = 1;
  const u64 now = armGetSystemTick();
  int turn = 0;
  if (dir != g_dave.dir) {
    g_dave.dir = dir, turn = dir;
    g_dave.next = now + armNsToTicks(260000000ull);
  } else if (dir && now >= g_dave.next) {
    turn = dir;
    g_dave.next = now + armNsToTicks(110000000ull);
  }
  /* no frame: the front slot is the choice */
  if (!ready)
    return;
  /* split screen: both players READY, player 1's copy picks now (its racer
   * select is the left column: a tap in its middle) */
  const float tx = MENU_ENGINE == 0 && ssr_split_columns() ? 0.25f : 0.5f;
  if (MENU_ENGINE == 0 && !tap_busy() && ssr_split_dave(MENU_ENGINE, SSR_DAVE_POLL, cs)) {
    tap_norm(tx, 0.52f);
    return;
  }
  if (turn && ssr_split_dave(MENU_ENGINE, SSR_DAVE_TURN, cs))
    turn = 0; /* READY: the racer stays */
  if (turn) {
    if (E.dave_rotate) {
      E.dave_rotate(car, turn, 0, 0);
      sound(SND_TICK);
    } else if (!tap_busy()) {
      tap_hash(turn < 0 ? 0xb78852f4 : 0x5467e858); /* the arrows */
    }
  } else if (tap_busy()) {
    return;
  } else if (p->down & HidNpadButton_A) {
    if (!ssr_split_dave(MENU_ENGINE, SSR_DAVE_A, cs))
      tap_norm(tx, 0.52f); /* a tap anywhere else picks the front racer */
  } else if (p->down & (HidNpadButton_Y | HidNpadButton_X)) {
    /* the stats: open them (their Button, +0x150, is enabled while the box is
     * shut) or shut them (the close Button, +0x168, enabled while it is open);
     * neither while it opens or shuts. A tap on the stats button while it is
     * off falls through to the screen, which picks the racer (MENUS.md). */
    if (cs[0x168])
      tap_hash(0x5d8fdaa0);
    else if (cs[0x150])
      tap_hash(0xe6b77dca);
  }
}

/* the racer select's stats box is open or on its way (its open Button off) */
static int dave_stats_open(const uint8_t *cs) { return cs[0x168] || !cs[0x150]; }

/* Lists (achievements, records): driven through the Listbox's own drag model
 * (UI_EXTRAS.md 4), as a finger dragging it would -- smooth, at a speed from
 * the stick (or the D-pad, L / R quicker), settling on an entry when let go:
 * state 2 (dragging, stable with no finger down), the pixel offset +0x58
 * moved (64 px an entry, + toward earlier entries) and UpdateRotating. */
static struct {
  const void *lb;
  int owned;
  float acc;
} g_lb;

static void list_frame(uint8_t *scr, const Profile *pf, const SsrPad *p, int touching) {
  uint8_t *lb = scr + pf->sub;
  int32_t *st = (int32_t *)(lb + 0x30);
  const int32_t cnt = *(const int32_t *)(lb + 0x0c);
  if (g_lb.lb != lb)
    g_lb.lb = lb, g_lb.owned = 0;
  if (!lb[0x2c] || cnt < 4 || !E.lb_rotate || !E.lb_end || !E.lb_finish)
    return;
  const int finger = touching || (E.touched && (*E.touched || *E.touched_last));
  if (finger) { /* a real touch takes over: hand it a clean spin */
    if (g_lb.owned && *st == 2)
      E.lb_end(lb);
    g_lb.owned = 0;
    return;
  }
  /* entries a second, + = down the list */
  float speed = 0.0f;
  const float y = -p->ly, m = fabsf(y);
  if (m > 0.2f)
    speed = (y < 0 ? -1.0f : 1.0f) * (3.0f + 27.0f * (m - 0.2f) * (m - 0.2f) / 0.64f);
  if (p->held & HidNpadButton_Down)
    speed = 9.0f;
  if (p->held & HidNpadButton_Up)
    speed = -9.0f;
  if (p->held & (HidNpadButton_R | HidNpadButton_ZR))
    speed = 22.0f;
  if (p->held & (HidNpadButton_L | HidNpadButton_ZL))
    speed = -22.0f;
  const float dt = dcr_config()->frame_rate >= 60 ? 1.0f / 60.0f : 1.0f / 30.0f;
  if (speed != 0.0f) {
    if (*st == 1) { /* an arrow's step animating: finish it next tick */
      *(float *)(lb + 0xa4) = *(const float *)(lb + 0xa8);
      return;
    }
    if (*st != 2) { /* take over, as OnBeginDragEvent does */
      const int32_t idx = *(const int32_t *)(lb + 0xb8);
      if (*st == 3) {
        *(int32_t *)(lb + 0x54) = idx;
        *(int32_t *)(lb + 0x58) %= 64;
      } else {
        *(int32_t *)(lb + 0x54) = (idx + 1) % cnt;
        *(int32_t *)(lb + 0x58) = 0;
      }
      memset(lb + 0x80, 0, 0x14);
      lb[0x39] = 0;
      *st = 2;
      g_lb.acc = 0;
      g_lb.owned = 1;
    }
    g_lb.acc -= speed * 64.0f * dt;
    int32_t d = (int32_t)g_lb.acc;
    g_lb.acc -= (float)d;
    const int32_t base = *(const int32_t *)(lb + 0x54);
    int32_t *off = (int32_t *)(lb + 0x58);
    const int32_t lo = (base - (cnt - 1)) * 64, hi = (base - 2) * 64; /* the index stays in [2, count-1] */
    int32_t nv = *off + d;
    nv = nv < lo ? lo : nv > hi ? hi : nv;
    d = nv - *off;
    *off = nv;
    int32_t *ri = (int32_t *)(lb + 0x90);
    if (*ri < 0 || *ri > 2)
      *ri = 0;
    ((int32_t *)(lb + 0x80))[*ri] = d;
    *ri = (*ri + 1) % 3;
    E.lb_rotate(lb);
  } else if (g_lb.owned && *st == 2) { /* let go: glide to the next entry */
    g_lb.owned = 0;
    if (*(const int32_t *)(lb + 0x58) % 64 == 0) {
      E.lb_finish(lb);
    } else {
      E.lb_end(lb);
      lb[0x9c] = 1;
    }
  }
}

static int g_slider; /* the focused slider */
static void sliders_frame(uint8_t *scr, const SliderDef *defs, int audio, const SsrPad *p, int nav, int touching) {
  int n = 0;
  while (defs[n].grab)
    n++;
  if (g_slider >= n)
    g_slider = 0;
  if (nav == 3 && g_slider > 0)
    g_slider--, sound(SND_TICK);
  if (nav == 4 && g_slider < n - 1)
    g_slider++, sound(SND_TICK);
  Rect r;
  if (button_rect(defs[g_slider].grab, &r))
    F.r = r, g_show = 1; /* the knob */
  const int d = nav == 1 ? -1 : nav == 2 ? 1 : 0;
  uint8_t *s = scr + defs[g_slider].off;
  if (!d || touching || !s[0x48])
    return;
  const float lo = *(const float *)(s + 0x10), hi = *(const float *)(s + 0x14);
  float v = *(const float *)(s + 0x18);
  if (audio) { /* 0..1 in tenths; the game keeps (int)(v * 100) */
    v = floorf(v * 10.0f + 0.5f) / 10.0f + d * 0.1f + 0.005f;
  } else {     /* sensitivity 3..7 in quarters */
    v = floorf(v * 4.0f + 0.5f) / 4.0f + d * 0.25f;
  }
  v = v < lo ? lo : v > hi ? hi : v;
  *(float *)(s + 0x18) = v;
  if (E.slider_refresh)
    E.slider_refresh(s);
  sound(SND_TICK);
}

static int g_rule;
static void rules_frame(const SsrPad *p, int nav) {
  if (nav == 3 && g_rule > 0)
    g_rule--, sound(SND_TICK);
  if (nav == 4 && g_rule < 4)
    g_rule++, sound(SND_TICK);
  Rect a, b;
  if (button_rect(k_rules[g_rule][0], &a) && button_rect(k_rules[g_rule][1], &b)) { /* the row: - to + */
    F.r.x0 = fminf(a.x0, b.x0), F.r.y0 = fminf(a.y0, b.y0), F.r.x1 = fmaxf(a.x1, b.x1), F.r.y1 = fmaxf(a.y1, b.y1);
    F.r.hash = k_rules[g_rule][0];
    g_show = 1;
  }
  if (tap_busy())
    return;
  if (nav == 1 || (p->down & HidNpadButton_Y))
    tap_hash(k_rules[g_rule][0]);
  else if (nav == 2 || (p->down & HidNpadButton_A))
    tap_hash(k_rules[g_rule][1]);
}

/* ---------------------------------------------------------------- the lobby */
/* The LOCAL multiplayer's lobby (MULTIPLAYER.md 2.6; split screen's, with
 * player 2 in it). State 5, the racer: left / right (or L / R) its arrows,
 * A picks. State 8, the host's rules: up / down a row (track, laps, items,
 * START), left / right its value, A its + (START on START), + starts the
 * race whenever it can. Its buttons act on a held touch with a debounce: a
 * tap each. Returns the prompt. */
static const uint32_t k_lobby_rows[3][2] = {{0x64dad1c8, 0x98d4b386}, /* track - / + */
                                            {0x3d48a555, 0xa73829e7}, /* laps - / + */
                                            {0xfb94b746, 0xef339808}}; /* items off / on */
#define LOBBY_START 0x63b66c77u
static int g_lobby_row;

/* a lobby row's rectangle: the whole row -- its label (LAPS, POWER UPS; the
 * track's picture) to its + arrow -- from its arrows' touch areas and the
 * scene's layout (lobby.star: labels at x 0.237, the picture from 0.131) */
static int lobby_row_rect(int row, Rect *r) {
  if (row == 3)
    return button_rect(LOBBY_START, r);
  Rect a, b;
  if (!button_rect(k_lobby_rows[row][0], &a) || !button_rect(k_lobby_rows[row][1], &b))
    return 0;
  r->x0 = fminf(a.x0, b.x0), r->y0 = fminf(a.y0, b.y0), r->x1 = fmaxf(a.x1, b.x1), r->y1 = fmaxf(a.y1, b.y1);
  uint8_t *o = E.pntr(E.om, k_lobby_rows[row][0]);
  const uint8_t *sc = o ? *(uint8_t *const *)(o + 0x104) : NULL;
  if (sc) {
    const float sx = *(const float *)(sc + 0x3c), ox = *(const float *)(sc + 0x44);
    const float label = (row == 0 ? 0.131f : 0.225f) * 960.0f * sx + ox;
    r->x0 = fminf(r->x0, label);
    const float h = r->y1 - r->y0;
    r->y0 -= h * 0.12f, r->y1 += h * 0.12f;
  }
  r->hash = k_lobby_rows[row][0];
  return 1;
}

static int lobby_frame(uint8_t *ls, const SsrPad *p, int nav) {
  const uint32_t st = *(const uint32_t *)(ls + 0xc);
  if (st == 5) {
    if (*(const uint32_t *)(ls + 0x1448) || tap_busy()) /* a request on its way */
      return SSR_PROMPT_CAROUSEL;
    int turn = page_event(p);
    if (nav == 1 || nav == 5 || nav == 7)
      turn = -1;
    else if (nav == 2 || nav == 6 || nav == 8)
      turn = 1;
    /* what its arrows and its portrait do (HandleCharacterInput), at once */
    if (turn && E.ch_next && E.ch_prev && E.ch_update) {
      (turn < 0 ? E.ch_prev : E.ch_next)(ls);
      E.ch_update(ls);
      sound(SND_TICK);
    } else if (turn) {
      tap_hash(turn < 0 ? 0x4255a1a2u : 0x838902ddu);
    } else if ((p->down & HidNpadButton_A) && E.ch_request && E.ch_locked) {
      if (!E.ch_locked(ls, *(const uint32_t *)(ls + 0x1444)))
        E.ch_request(ls);
    } else if (p->down & HidNpadButton_A) {
      tap_hash(0xd27629cfu);
    }
    return SSR_PROMPT_CAROUSEL;
  }
  if (st != 8)
    return SSR_PROMPT_NONE;
  Rect r;
  int live[4], any = 0;
  for (int i = 0; i < 4; i++)
    any |= live[i] = lobby_row_rect(i, &r);
  if (!any)
    return SSR_PROMPT_NONE; /* a guest's lobby: the host sets the race */
  if (!live[g_lobby_row])
    for (int i = 0; i < 4; i++)
      if (live[(g_lobby_row + i) % 4]) {
        g_lobby_row = (g_lobby_row + i) % 4;
        break;
      }
  if (nav == 3 || nav == 4) {
    for (int k = 1; k < 4; k++) {
      const int i = (g_lobby_row + (nav == 3 ? 4 - k : k)) % 4;
      if (live[i]) {
        g_lobby_row = i;
        sound(SND_TICK);
        break;
      }
    }
  }
  if (lobby_row_rect(g_lobby_row, &F.r))
    g_show = 1;
  if (tap_busy())
    return SSR_PROMPT_LOBBY;
  if ((p->down & HidNpadButton_Plus) && live[3])
    tap_hash(LOBBY_START);
  else if (g_lobby_row == 3 && (p->down & HidNpadButton_A))
    tap_hash(LOBBY_START);
  else if (g_lobby_row < 3 && (nav == 1 || (p->down & HidNpadButton_Y)))
    tap_hash(k_lobby_rows[g_lobby_row][0]);
  else if (g_lobby_row < 3 && (nav == 2 || (p->down & HidNpadButton_A)))
    tap_hash(k_lobby_rows[g_lobby_row][1]);
  return SSR_PROMPT_LOBBY;
}

/* ---------------------------------------------------------------- the keyboard */
static struct {
  const void *kb; /* the screen the Switch keyboard was shown for */
} g_kbd;

void ssr_boot_system_dialog(int on); /* ssr_boot.c: the watchdog knows */
void ssr_clock_resync(void);         /* ssr_patch.c */

/* the Switch keyboard for the licence name; 1 if a name went in */
static int kbd_switch(uint8_t *kb) {
  char *buf = *(char **)(kb + 0xc);
  int max = *(const int32_t *)(kb + 0x10);
  if (!buf || max <= 0)
    return 0;
  if (max > 32)
    max = 32;
  SwkbdConfig k;
  if (R_FAILED(swkbdCreate(&k, 0)))
    return 0;
  swkbdConfigMakePresetDefault(&k);
  swkbdConfigSetHeaderText(&k, "Your licence name");
  swkbdConfigSetSubText(&k, "Letters, numbers and spaces");
  swkbdConfigSetGuideText(&k, "Name");
  swkbdConfigSetInitialText(&k, buf);
  swkbdConfigSetStringLenMax(&k, (u32)max);
  char out[128] = "";
  ssr_boot_system_dialog(1);
  const Result rc = swkbdShow(&k, out, sizeof out);
  ssr_boot_system_dialog(0);
  swkbdClose(&k);
  ssr_clock_resync();
  if (R_FAILED(rc))
    return 0;
  /* the game's keys: A-Z, 0-9 and space */
  char name[40];
  int n = 0;
  for (const char *c = out; *c && n < max; c++) {
    char ch = *c >= 'a' && *c <= 'z' ? (char)(*c - 32) : *c;
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || (ch == ' ' && n > 0))
      name[n++] = ch;
  }
  while (n > 0 && name[n - 1] == ' ')
    n--;
  name[n] = 0;
  if (!n)
    return 0;
  strcpy(buf, name);
  if (E.set_string)
    E.set_string(kb + 0x14, name);
  debugPrintf("[menu] licence name from the Switch keyboard: %s\n", name);
  return 1;
}

static void keyboard_frame(uint8_t *kb, const SsrPad *p, int nav, int touching) {
  if (g_kbd.kb != kb && !touching && !tap_busy()) {
    g_kbd.kb = kb;
    if (kbd_switch(kb)) {
      tap_hash(K_DONE); /* the game's own confirm */
      return;
    }
  }
  items_clear();
  items_from(k_keys);
  focus_enter(kb, 3, 0xdea8bfe4 /* A */);
  focus_refind(0xdea8bfe4);
  focus_move(nav);
  g_show = F.has;
  if (tap_busy())
    return;
  if ((p->down & HidNpadButton_A) && F.has)
    tap_rect(&F.r);
  else if (p->down & HidNpadButton_Y)
    tap_hash(K_SPACE);
  else if (p->down & HidNpadButton_Plus)
    tap_hash(K_DONE);
  else if ((p->down & HidNpadButton_X) && kbd_switch(kb))
    tap_hash(K_DONE);
}

/* ================================================================ the pointer */
static struct {
  float x, y;
  int on, pressed, back;
  u64 last;
} g_ptr;

int ssr_input_pointer(float *x, float *y, int *pressed) {
  *x = g_ptr.x, *y = g_ptr.y;
  *pressed = g_ptr.pressed;
  return g_ptr.on;
}

/* the right stick: 1 if the pointer has the controller this frame */
static int pointer_frame(const SsrPad *p, int nav, SsrFinger *want, int *n, int cap) {
  if (MENU_ENGINE != 0)
    return 0; /* player 2's copy: no pointer (the one on the screen is player 1's) */
  const u64 now = armGetSystemTick();
  const float m = sqrtf(p->rx * p->rx + p->ry * p->ry);
  if (m > 0.25f) {
    if (!g_ptr.on) {
      g_ptr.on = 1;
      g_ptr.x = F.has ? cx(&F.r) : g_w * 0.5f;
      g_ptr.y = F.has ? cy(&F.r) : g_h * 0.5f;
    }
    /* a curve: fine near the centre, quick at the rim (screen heights a second) */
    const float k = (m - 0.25f) / 0.75f, speed = (0.3f + 1.9f * k * k) * (float)g_h / 60.0f;
    g_ptr.x += p->rx / m * speed, g_ptr.y -= p->ry / m * speed;
    g_ptr.x = fminf(fmaxf(g_ptr.x, 0), (float)g_w - 1), g_ptr.y = fminf(fmaxf(g_ptr.y, 0), (float)g_h - 1);
    g_ptr.last = now;
  }
  if (!g_ptr.on)
    return 0;
  if (nav || armTicksToNs(now - g_ptr.last) > 8000000000ull) { /* the focus is back */
    g_ptr.on = 0;
    return 0;
  }
  g_ptr.pressed = (p->held & HidNpadButton_A) != 0;
  if (g_ptr.pressed && *n < cap) {
    want[(*n)++] = (SsrFinger){F_POINTER, (int)g_ptr.x, (int)g_ptr.y};
    g_ptr.last = now;
  }
  if (p->down & HidNpadButton_B) /* Back, where the game takes it */
    g_ptr.back = 1;
  return 1;
}

/* ================================================================ the chrome */
/* The console look (UI_EXTRAS.md 2, 3): what the phone's menus have that a
 * controller does not need goes, with the engine's own alpha override (object
 * +0xf4 bit 0x80, alpha +0x3b = 0: drawn no more, its "shown" flag and touch
 * area kept, so Back and every hit test behave as before), every frame (the
 * scenes are re-created often). */
static void alpha_hide(uint32_t h) {
  uint8_t *o = E.getobj && E.sm ? E.getobj(E.sm, h) : NULL;
  if (!o)
    return;
  o[0xf4] |= 0x80;
  o[0x3b] = 0;
}

/* a texture object's sprite record (its UVs: floats 1..8), or NULL */
static float *sprite_rec(uint32_t txtr) {
  uint8_t *o = E.getobj && E.sm ? E.getobj(E.sm, txtr) : NULL;
  uint8_t *pay = o ? *(uint8_t **)(o + 0x114) : NULL;
  return pay ? *(float **)(pay + 0x14) : NULL;
}

static void chrome_frame(void) {
  /* the yellow touch back arrows (Back is B: the latch is read first) */
  alpha_hide(0x268a3a94); /* the footer's */
  alpha_hide(0xed9aa461); /* the options' (front end and in game) */
  alpha_hide(0xebcb55c3); /* controls, sensitivity, audio */
  alpha_hide(0x58e5b02c); /* the rules' (RulesMenu still needs it "shown": B taps its area) */
  alpha_hide(0xec71cdba); /* PRIVACY POLICY */
  /* the pause menu's rows: the white end caps and bottom strip are painted
   * into its two textures; both sample only the blue part (the top 75% of
   * the bar's strip), so each row is one clean blue bar */
  float *bar = sprite_rec(0x3f758900), *cap = sprite_rec(0xc217d2e5);
  if (bar && cap && bar != cap) {
    bar[6] = bar[8] = 0.3203125f * 0.75f;
    for (int i = 1; i <= 8; i++)
      cap[i] = bar[i];
  }
}

/* The glow hugs the button's picture, which is smaller than its touch area
 * for some (normalised insets: left, top, right, bottom) */
static void visual_rect(const Rect *r, float out[4]) {
  float l = 0, t = 0, rr = 0, b = 0;
  if (r->hash == L_NO || r->hash == R_YES) /* the dialogs' orange buttons */
    l = 0.025f, t = 0.044f, rr = 0.031f, b = 0.035f;
  for (const uint32_t *h = k_pause; *h; h++)
    if (r->hash == *h) /* the pause rows' bars */
      l = 0.013f, t = 0.015f, rr = 0.017f, b = 0.013f;
  out[0] = r->x0 + l * g_w, out[1] = r->y0 + t * g_h;
  out[2] = (r->x1 - rr * g_w) - out[0], out[3] = (r->y1 - b * g_h) - out[1];
}

/* the prompts ride on the footer's title line (it slides in and out) */
static void place_prompts(void) {
  uint8_t *title = E.footer && *E.footer && E.getobj && E.sm ? E.getobj(E.sm, 0xa7be22bb) : NULL;
  MENU_PROMPT_PLACE(title ? *(const float *)(title + 0x40) - 0.031f : 0.93f);
}

/* ================================================================ the title */
/* TAP TO START is "PRESS" and two spaces in the title's own font (ssr_race.c's
 * words): the game centres the line (TEXT 0x879636eb, anchored at its centre,
 * blinking), and the A button is drawn in the spaces -- the console editions'
 * PRESS (A), centred, a clear gap between. The line's width: the engine's own
 * measure, in the font's pixels (one per design pixel; a space is 40), times
 * the scene's scale (PRESS measured 267 px of 1280 on hardware). */
#define TITLE_TEXT 0x879636ebu
#define TITLE_SPACES (2 * 40.0f)
static void title_frame(void) {
  uint8_t *o = E.getobj && E.sm ? E.getobj(E.sm, TITLE_TEXT) : NULL;
  const uint8_t *sc = o ? *(uint8_t *const *)(o + 0x104) : NULL;
  if (!sc || !o[0xde] || !*(const uint16_t *)(o + 0x4e))
    return;
  const float sx = *(const float *)(sc + 0x3c), sy = *(const float *)(sc + 0x40);
  const float ox = *(const float *)(sc + 0x44), oy = *(const float *)(sc + 0x48);
  const float dw = *(const float *)(sc + 0x34), dh = *(const float *)(sc + 0x38);
  if (sx <= 0 || dw <= 0)
    return;
  const int wf = E.str_width ? E.str_width(E.sm, TITLE_TEXT) : 0;
  const float tw = (wf > 0 && wf < 2000 ? (float)wf : 5 * 40.0f + TITLE_SPACES) * sx; /* "PRESS  " */
  const float cx = *(const float *)(o + 0x3c) * dw * sx + ox, cy = *(const float *)(o + 0x40) * dh * sy + oy;
  const float ih = 0.066f * (float)g_h;
  /* the spaces' middle, a touch right (the letters' outline reaches past
   * their advance) */
  const float icon_cx = cx + tw * 0.5f - TITLE_SPACES * sx * 0.5f + 0.1f * ih;
  MENU_PROMPT_ICON(0 /* A */, icon_cx, cy, ih, o[0x3b] / 255.0f);
}

/* ================================================================ the frame */
static uint8_t *g_seen_screen;
static int g_seen_state = -99;

static int prompt_of(const Profile *pf) {
  switch (pf->model) {
  case M_TAP:
    /* the title: none -- its own line says PRESS (A) (title_frame) */
    return !strcmp(pf->cls, "TitleScreen") ? SSR_PROMPT_NONE : pf->extra_x ? SSR_PROMPT_SEGAMILES : SSR_PROMPT_CONTINUE;
  case M_CAR:
    return pf->extra_x == 0x5919b6c7 ? SSR_PROMPT_CAROUSEL_RULES : pf->extra_x ? SSR_PROMPT_CAROUSEL_INFO
                                                                               : SSR_PROMPT_CAROUSEL;
  case M_DAVE:
    return SSR_PROMPT_CHARACTER;
  case M_LIST:
    return SSR_PROMPT_LIST;
  case M_SLIDERS:
    return SSR_PROMPT_SLIDERS;
  case M_RULES:
    return SSR_PROMPT_RULES;
  case M_KEYBOARD:
    return SSR_PROMPT_KEYBOARD;
  case M_FOCUS:
    return pf->back == BK_SWALLOW ? SSR_PROMPT_FOCUS_NOBACK : SSR_PROMPT_FOCUS;
  default:
    return SSR_PROMPT_NONE;
  }
}

static const Profile *profile_of(const uint8_t *scr) {
  const void *vt = *(void *const *)scr;
  for (int i = 0; i < NPROF; i++)
    if (k_prof[i].vt == vt)
      return &k_prof[i];
  return NULL;
}

static void note_context(const char *what, const void *scr, int state) {
  if (scr == g_seen_screen && state == g_seen_state && what == g_ctx_name)
    return;
  g_seen_screen = (uint8_t *)scr, g_seen_state = state, g_ctx_name = what;
  debugPrintf("[menu] %s (state %d)\n", what, state);
}

/* the dialogs' left button is there when its picture is shown (its touch
 * area never goes: UI_EXTRAS.md 3.3) */
static int left_button_shown(void) {
  uint8_t *o = E.getobj && E.sm ? E.getobj(E.sm, 0x1a916179u) : NULL;
  return !o || o[0xde];
}

/* dialogs a screen draws itself (MENUS.md 3.2): its two buttons */
static void dialog_frame(uint8_t *scr, const Profile *pf, const SsrPad *p, int nav) {
  items_clear();
  if (pf->dlg != DLG_OK && left_button_shown())
    items_add(L_NO);
  items_add(R_YES);
  focus_enter(scr, 1000 + pf->dlg_state, pf->dlg_default ? pf->dlg_default : R_YES);
  focus_refind(pf->dlg_default ? pf->dlg_default : R_YES);
  focus_move(nav);
  g_show = F.has;
  if (tap_busy())
    return;
  if ((p->down & HidNpadButton_A) && F.has)
    tap_rect(&F.r);
  else if (p->down & HidNpadButton_B) /* the harmless answer (they ignore Back) */
    tap_hash(pf->dlg == DLG_OK ? R_YES : L_NO);
}

static int race_finished(const uint8_t *hud) {
  const uint8_t *racer = *(uint8_t *const *)(hud + 0xe0);
  return racer && racer[0xbc1];
}

/* ssr_test.c: what this copy shows (a profile's class; HUD, PauseMenu,
 * loading, or popup:<class>) */
const char *ssr_menu_screen(void) {
  uint8_t *top = E.stack && *E.stack && E.top ? E.top(*E.stack) : NULL;
  if (!top)
    return NULL;
  uint8_t *pop = NULL;
  const int popup = popup_live(&pop);
  if (popup < 0 || (E.loading && E.loading()))
    return "loading";
  if (popup > 0) {
    for (int i = 0; i < NPOP; i++)
      if (k_pop[i].vt == *(void **)pop)
        return k_pop[i].cls;
    return "popup";
  }
  if (*(void **)top == g_vt_hud || *(const uint32_t *)(top + 4) == 0x91c494eau) {
    uint8_t *pm = E.byhash ? E.byhash(*E.stack, 0x1a29a09fu) : NULL;
    const uint32_t pst = pm ? *(const uint32_t *)(pm + 0xc) : 0;
    return pm && pst != 0 && pst != 4 ? "PauseMenu" : "HUD";
  }
  const Profile *pf = profile_of(top);
  return pf ? pf->cls : "other";
}

/* ssr_test.c: the screen takes input now (its profile's input state) */
int ssr_menu_screen_ready(void) {
  uint8_t *top = E.stack && *E.stack && E.top ? E.top(*E.stack) : NULL;
  const Profile *pf = top ? profile_of(top) : NULL;
  if (!pf || pf->st_on < 0)
    return 1;
  return *(const int32_t *)(top + pf->st_off) == pf->st_on;
}

int ssr_menu_frame(const SsrPad *p, int w, int h, SsrFinger *want, int *n, int cap, int touching) {
  g_w = w, g_h = h;
  tap_frame(want, n, cap);
  if (touching)
    F.hidden = 1;
  else if (p->down || fabsf(p->lx) > 0.5f || fabsf(p->ly) > 0.5f)
    F.hidden = 0;
  int mode = SSR_MODE_MENU;
  const int nav = nav_event(p);
  uint8_t *top = E.stack && *E.stack && E.top ? E.top(*E.stack) : NULL;
  uint8_t *pop = NULL;
  const int popup = popup_live(&pop);
  g_show = 0;
  int on_keyboard = 0;
  int prompt = SSR_PROMPT_NONE;

  if (!top || !E.pntr) {
    /* nothing yet */
  } else if (popup < 0 || (E.loading && E.loading())) {
    note_context("loading or a pop-up animating", top, -1);
  } else if (popup > 0) {
    const uint32_t mask = *(uint32_t *)(pop + 0x2c);
    note_context("pop-up", pop, (int)mask);
    prompt = (mask & 3) == 3 ? SSR_PROMPT_YESNO : (mask & 3) ? SSR_PROMPT_OK : (mask & 0x14) ? SSR_PROMPT_CONTINUE
                                                                                       : SSR_PROMPT_NONE;
    if (!pointer_frame(p, nav, want, n, cap))
      popup_frame(pop, p, nav);
  } else if (*(void **)top == g_vt_hud || *(const uint32_t *)(top + 4) == 0x91c494eau) {
    uint8_t *pm = E.byhash ? E.byhash(*E.stack, 0x1a29a09fu) : NULL;
    const uint32_t pst = pm ? *(const uint32_t *)(pm + 0xc) : 0;
    if (pm && pst != 0 && pst != 4) { /* paused: the pause menu is under the HUD */
      note_context("PauseMenu", pm, (int)pst);
      prompt = SSR_PROMPT_PAUSE;
      if (pst == 2 && !pointer_frame(p, nav, want, n, cap)) {
        items_clear();
        items_from(k_pause);
        focus_enter(pm, 2, k_pause[0]);
        focus_refind(k_pause[0]);
        focus_move(nav);
        g_show = F.has;
        if ((p->down & HidNpadButton_A) && F.has && !tap_busy())
          tap_rect(&F.r);
        if (p->down & (HidNpadButton_B | HidNpadButton_Plus))
          back_key(); /* = RESUME */
      }
    } else {
      mode = SSR_MODE_RACE;
      note_context("race", top, 0);
      g_ptr.on = 0;
      if (p->down & HidNpadButton_Plus)
        back_key(); /* pause (split screen: the race's, both players') */
      /* a touch skips the fly-by before the countdown (STRacing::Update:
       * race state 1 with the fly-through flag +0x774 set), and continues
       * from the results once finished */
      const uint8_t *game = E.game ? *E.game : NULL;
      const int intro = game && *(const int32_t *)(game + 0x76c) == 1 && game[0x774];
      const int finished = race_finished(top);
      if (finished)
        prompt = SSR_PROMPT_CONTINUE;
      if ((p->down & HidNpadButton_A) && (intro || finished) && !tap_busy())
        tap_norm(0.5f, 0.5f);
    }
  } else {
    const Profile *pf = profile_of(top);
    const uint32_t st = pf && pf->st_on >= 0 ? *(const uint32_t *)(top + pf->st_off) : 0;
    if (pf && pf->model == M_TAP && pf->back == BK_SWALLOW && pf->tx == 0.5f && !strcmp(pf->cls, "TitleScreen"))
      title_frame();
    if (!pf) {
      note_context("a screen without a profile: every button", top, 0);
      prompt = SSR_PROMPT_FOCUS;
      if (!pointer_frame(p, nav, want, n, cap)) {
        items_walk();
        focus_enter(top, 0, 0);
        focus_refind(0);
        focus_move(nav);
        g_show = F.has;
        if ((p->down & HidNpadButton_A) && F.has && !tap_busy())
          tap_rect(&F.r);
        if (p->down & HidNpadButton_B)
          back_key();
      }
    } else if (pf->dlg_state && (int)st == pf->dlg_state && pf->dlg) {
      on_keyboard = pf->model == M_KEYBOARD; /* its "enter a name" dialog */
      note_context(pf->cls, top, (int)st);
      prompt = pf->dlg == DLG_OK ? SSR_PROMPT_OK : SSR_PROMPT_YESNO;
      if (!pointer_frame(p, nav, want, n, cap))
        dialog_frame(top, pf, p, nav);
    } else if (pf->st_on >= 0 && (int)st != pf->st_on) {
      on_keyboard = pf->model == M_KEYBOARD;
      note_context(pf->cls, top, (int)st); /* animating, loading: no input */
      prompt = prompt_of(pf);
    } else {
      note_context(pf->cls, top, (int)st);
      prompt = prompt_of(pf);
      if (!pointer_frame(p, nav, want, n, cap)) {
        switch (pf->model) {
        case M_TAP:
          if ((p->down & (HidNpadButton_A | (pf->back == BK_SWALLOW ? HidNpadButton_Plus : 0))) && !tap_busy())
            tap_norm(pf->tx, pf->ty);
          if ((p->down & HidNpadButton_X) && pf->extra_x && !tap_busy())
            tap_hash(pf->extra_x);
          if (pf->ty > 0.6f && (p->down & HidNpadButton_Up) && !tap_busy())
            tap_norm(0.5f, 0.25f); /* the records' credits: slower */
          break;
        case M_CAR:
          car_frame(top, pf, p, nav);
          break;
        case M_DAVE:
          dave_frame(top, p, nav);
          break;
        case M_LIST:
          list_frame(top, pf, p, touching);
          break;
        case M_SLIDERS:
          sliders_frame(top, (const SliderDef *)pf->data, pf->data == k_audio, p, nav, touching);
          break;
        case M_RULES:
          rules_frame(p, nav);
          break;
        case M_KEYBOARD:
          on_keyboard = 1;
          keyboard_frame(top, p, nav, touching);
          break;
        case M_LOBBY:
          prompt = lobby_frame(top, p, nav);
          break;
        case M_FOCUS:
          items_clear();
          items_from((const uint32_t *)pf->data);
          focus_enter(top, (int)st, ((const uint32_t *)pf->data)[0]);
          focus_refind(((const uint32_t *)pf->data)[0]);
          focus_move(nav);
          g_show = F.has;
          if ((p->down & HidNpadButton_A) && F.has && !tap_busy())
            tap_rect(&F.r);
          break;
        default:
          break;
        }
        if ((p->down & HidNpadButton_B) && pf->model == M_DAVE && dave_stats_open(top)) {
          if (top[0x168] && !tap_busy()) /* B shuts the stats (not Back) */
            tap_hash(0x5d8fdaa0);
        } else if ((p->down & HidNpadButton_B) && pf->model == M_DAVE && ssr_split_dave(MENU_ENGINE, SSR_DAVE_B, top)) {
          /* split screen: READY taken back (player 2's never leaves) */
        } else if ((p->down & HidNpadButton_B) && pf->model != M_KEYBOARD) {
          if (pf->back == BK_BACK)
            back_key();
          else if (pf->back == BK_TAP && !tap_busy())
            tap_hash(pf->back_hash);
        } else if ((p->down & HidNpadButton_B) && !tap_busy()) {
          tap_hash(K_DEL); /* the keyboard: B deletes */
        }
        /* LicenseScreen: + confirms a new licence (Back does, on the phone) */
        if (pf->data == k_license && (p->down & HidNpadButton_Plus))
          back_key();
      }
    }
  }
  if (g_ptr.back) { /* B with the pointer out: the screen's Back, if it has a harmless one */
    g_ptr.back = 0;
    const Profile *pf = top && mode == SSR_MODE_MENU && popup == 0 ? profile_of(top) : NULL;
    if (pf && pf->back == BK_TAP && !tap_busy())
      tap_hash(pf->back_hash);
    else if (top && popup == 0 && (!pf || pf->back == BK_BACK))
      back_key();
  }
  if (!on_keyboard)
    g_kbd.kb = NULL; /* the next keyboard gets the Switch's again */
  if (ssr_split_race())
    prompt = SSR_PROMPT_NONE; /* the bottom of the screen is player 2's */
  MENU_PROMPT_SET(prompt);
  place_prompts();
  if (E.pntr)
    chrome_frame();
  const int show = g_show && !F.hidden && !g_ptr.on && mode == SSR_MODE_MENU;
  float vr[4];
  visual_rect(&F.r, vr);
  MENU_FOCUS(show, vr[0], vr[1], vr[2], vr[3]);
  return mode;
}

/* ================================================================ set up */
static const void *vt_of(const char *cls) {
  char sym[80];
  snprintf(sym, sizeof sym, "_ZTV%d%s", (int)strlen(cls), cls);
  const uint8_t *v = MENU_NATIVE(sym);
  return v ? v + 8 : NULL;
}

#if MENU_ENGINE == 0
/* [debug] log_touches: every button a touch hit, and which screen was up
 * (SceneManager::IsPointWithinPointerArea, re-implemented: the hook) */
static int hit_logged(void *sm, int x, int y, uint32_t hash) {
  (void)sm;
  uint8_t *o = E.pntr ? E.pntr(E.om, hash) : NULL;
  const int r = o ? E.hit(o, x, y) : 0;
  if (r) {
    const uint8_t *top = E.stack && *E.stack ? E.top(*E.stack) : NULL;
    const Profile *pf = top ? profile_of(top) : NULL;
    debugPrintf("[menu] the game hit 0x%08x at %d,%d on %s\n", (unsigned)hash, x, y,
                pf ? pf->cls : top ? "a screen without a profile" : "?");
  }
  return r;
}

int ssr_patch_jump(const char *sym, uint32_t expect, void *dst); /* ssr_patch.c, at load */
int ssr_patch_at(uint32_t vaddr, uint32_t expect, uint32_t value); /* ssr_patch.c, at load */
void ssr_menu_patch(void) {
  /* the main menu's Google sign-in banner: its three slide animations become
   * nops, so it stays at its creation keyframe, off the top of the screen
   * (and its touch areas with it) (UI_EXTRAS.md 2.4) */
  const int n = ssr_patch_at(0xd41f0, 0xeb0329f8u, 0xe1a00000u) + ssr_patch_at(0xd4384, 0xeb032993u, 0xe1a00000u) +
                ssr_patch_at(0xd4814, 0xeb03286fu, 0xe1a00000u);
  debugPrintf("[menu] the Google sign-in banner: %s\n", n == 3 ? "gone" : "left (not the 1.0.1 library)");
  int ssr_patch_engine_index(void);
  if (dcr_config()->log_touch && ssr_patch_engine_index() == 0) /* its log reads the first copy's menus */
    ssr_patch_jump("_ZN12SceneManager24IsPointWithinPointerAreaEiij", 0xe92d4070u, (void *)hit_logged);
  if (!dcr_config()->split_screen)
    return;
  /* Split screen (ssr_split.c) is the main menu's MULTIPLAYER, as on the
   * consoles: MainMenu::Update's MULTIPLAYER makes the Solo menu (what
   * SINGLE PLAYER makes: new(0x78) SoloMenu(0)) instead of the LOCAL /
   * ONLINE menu (new(0x74) MultiplayerServiceMenu) -- the null test's branch
   * gives way to the page type argument; split screen starts when that menu
   * comes from this card */
  int ssr_patch_word_is(uint32_t vaddr, uint32_t expect);
  int m = 0;
  if (ssr_patch_word_is(0xd4658, 0xe3a00074u) && ssr_patch_word_is(0xd466c, 0x0affffa0u) &&
      ssr_patch_word_is(0xd4670, 0xeb000c00u))
    m = ssr_patch_at(0xd4658, 0xe3a00074u, 0xe3a00078u) + /* mov r0, #0x78 */
        ssr_patch_at(0xd466c, 0x0affffa0u, 0xe3a01000u) + /* mov r1, #0 */
        ssr_patch_at(0xd4670, 0xeb000c00u, 0xeb0071afu);  /* bl SoloMenu::SoloMenu(PageType) */
  debugPrintf("[menu] MULTIPLAYER -> SPLIT SCREEN (the Solo menu, two players): %s\n",
              m == 3 ? "yes" : "NO (not 1.0.1?)");
}
#endif

void ssr_menu_init(void) {
  E.stack = (void **)MENU_NATIVE("g_pScreenStack");
  E.top = (fn_top)MENU_NATIVE("_ZNK11ScreenStack12GetTopScreenEv");
  E.byhash = (fn_byhash)MENU_NATIVE("_ZNK11ScreenStack15GetScreenByHashEj");
  E.popups = (uint8_t **)MENU_NATIVE("g_pPopUpStack");
  E.loading = (fn_bool0)MENU_NATIVE("_ZN13LoadingScreen9IsVisibleEv");
  E.om = MENU_NATIVE("g_object_manager");
  E.pntr = (fn_getpntr)MENU_NATIVE("_ZN13ObjectManager20GetPointerAreaObjectEj");
  E.hit = (fn_hit)MENU_NATIVE("_ZN18CPointerAreaObject24IsPointWithinPointerAreaEii");
  E.car_next = (fn_this)MENU_NATIVE("_ZN8Carousel12ScrollToNextEv");
  E.car_prev = (fn_this)MENU_NATIVE("_ZN8Carousel16ScrollToPreviousEv");
  E.car_page = (fn_curpage)MENU_NATIVE("_ZNK8Carousel17GetCurrentPageIdxEPb");
  E.list_next = (fn_this)MENU_NATIVE("_ZN7Listbox12ScrollToNextEv");
  E.list_prev = (fn_this)MENU_NATIVE("_ZN7Listbox16ScrollToPreviousEv");
  E.slider_refresh = (fn_this)MENU_NATIVE("_ZN6Slider7RefreshEv");
  E.sound = (fn_sound)MENU_NATIVE("_ZN16iSAudioInterface10StartEventEjjb");
  E.sfx = (const uint32_t *)MENU_NATIVE("STAudio_Group_SoundEffects");
  E.set_string = (fn_setstr)MENU_NATIVE("_ZN17SuStringGenerator9SetStringEPKc");
  E.ticks = (const volatile uint32_t *)MENU_NATIVE("_ZN13SuApplication15ms_frameCounterE");
  E.game = (uint8_t **)MENU_NATIVE("g_pGame");
  E.sm = MENU_NATIVE("g_scene_manager");
  E.getobj = (uint8_t * (*)(void *, uint32_t)) MENU_NATIVE("_ZN12SceneManager9GetObjectEj");
  E.footer = (const volatile uint8_t *)MENU_NATIVE("_ZN12GlobalScreen17ms_bFooterVisibleE");
  E.touched = (const volatile uint8_t *)MENU_NATIVE("_ZN13SuApplication10ms_touchedE");
  E.touched_last = (const volatile uint8_t *)MENU_NATIVE("_ZN13SuApplication19ms_touchedLastFrameE");
  E.lb_rotate = (fn_this)MENU_NATIVE("_ZN7Listbox14UpdateRotatingEv");
  E.lb_end = (fn_this)MENU_NATIVE("_ZN7Listbox14OnEndDragEventEv");
  E.lb_finish = (fn_this)MENU_NATIVE("_ZN7Listbox14FinishRotatingEv");
  E.dave_rotate = (void (*)(void *, int, int, int))MENU_NATIVE("_ZN17DavesBaseCarousel14RotateCarouselEibb");
  E.str_width = (int (*)(void *, uint32_t))MENU_NATIVE("_ZN12SceneManager19GetStringPixelWidthEj");
  E.ch_next = (fn_this)MENU_NATIVE("_ZN11LobbyScreen13NextCharacterEv");
  E.ch_prev = (fn_this)MENU_NATIVE("_ZN11LobbyScreen17PreviousCharacterEv");
  E.ch_update = (fn_this)MENU_NATIVE("_ZN11LobbyScreen15UpdateCharacterEv");
  E.ch_request = (fn_this)MENU_NATIVE("_ZN11LobbyScreen16RequestCharacterEv");
  E.ch_locked = (int (*)(void *, uint32_t))MENU_NATIVE("_ZN11LobbyScreen16IsLockedByOthersEj");
  if (!E.pntr || !E.hit || !E.om)
    E.pntr = NULL; /* no buttons: the pointer only */
  int nvt = 0;
  for (int i = 0; i < NPROF; i++)
    nvt += (k_prof[i].vt = vt_of(k_prof[i].cls)) != NULL;
  for (int i = 0; i < NPOP; i++)
    k_pop[i].vt = vt_of(k_pop[i].cls);
  g_vt_tutorial = vt_of("TutorialPopUp");
  g_vt_hud = vt_of("HUD");
  debugPrintf("[menu] %s; %d of %d screen profiles matched to the game's classes\n",
              E.pntr ? "the focus on the game's buttons" : "NO BUTTON ACCESS: the pointer only", nvt, NPROF);
}
