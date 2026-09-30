/* ssr_race.c -- a race, driven with a controller, on a console-style HUD
 * (../source/docs/HUD.md).
 *
 * INPUT. On the phone a race was touch and tilt: any finger down accelerated,
 * the tilt steered, and red / yellow / item buttons on the HUD braked,
 * drifted and fired. None of that is used here. The human player's
 * controller (STPlayerControllerHuman) has one virtual method the engine
 * calls once per 60 Hz logic tick, Update(bool) -- it read the HUD's touch
 * buttons and the tilt and handed them to STRacer::SetControls. Its vtable
 * slot is pointed at controller_update below, which hands SetControls the
 * pad instead: a steer from the stick, D-pad or motion (s19.12, 4096 = full
 * lock, right positive), and ten buttons, three of which the phone never had
 * -- throwing an item backwards, the in-air "stick forward" and looking
 * behind. The engine works out press and release edges from one tick to the
 * next, so a button is passed as held; a press shorter than a tick (inside a
 * frame that ran none) is latched until one runs. A network race (the
 * LOCAL multiplayer) also runs the original's network prelude first: the
 * racer's stage and its state for the other players' packets. In split
 * screen (ssr_split_race.c) player 2's racer is a second human racer in
 * player 1's copy's race: its controller is fed from player 2's pad, with
 * the race's camera theirs while it is (looking behind is their camera's).
 *
 * The layout is Mario Kart 8 Deluxe's, which Switch players know:
 *   A          accelerate          B          brake, reverse
 *   R / ZR     drift (hold); let go in the air: a trick
 *   L / ZL     item, All-Star move (a thrown item: hold to aim, let go;
 *              stick or D-pad down: behind)
 *   X          look behind         +          pause
 *   left stick, D-pad, or motion ([controls] steering): steer
 * [controls] auto_accelerate: the engine's own auto-accelerate (it drives on
 * until you brake).
 *
 * THE HUD. The phone's HUD is already the console's at the top (the racers'
 * progress bar with portraits in the middle, LAP and POS at the right, the
 * timer); what was the phone's is the touch buttons -- pause, brake, drift,
 * the slider's item button and the steering slider itself -- and the item
 * box at the bottom left, where a thumb reached it. Every frame of a race the
 * eight touch sprites get the engine's own alpha override (object +0xf4 bit
 * 0x80, alpha +0x3b = 0: no animation or show call brings them back), and the
 * item box's keyframes all get y -0.60, which puts it at the top left, the
 * console's place, its slide-in and bounce kept.
 *
 * RUMBLE. The engine's STVibration::Vibrate is empty on Android; its four
 * wrappers (long, big, medium, small: crashes, spin-outs, hits, rough
 * ground -- each called for the local player only) are hooked to the
 * controller's rumble: the controller of the copy's player.
 *
 * WORDS. The tutorial's pop-ups name the phone's controls ("TILT THE SCREEN
 * TO STEER", "THE YELLOW BUTTON"). In English (the game's other languages
 * keep their own words) the engine's string lookup, SiffTextManager::
 * GetString -- a binary search over the loaded string tables, rewritten here
 * -- answers those ids with the Switch's buttons. MIT.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "ssr.h"
#include "util.h"

#define HUD_HASH 0x91c494eau

typedef void *(*fn_top)(const void *stack);
typedef void (*fn_upd)(void *self, int active);
typedef void (*fn_setc)(void *racer, const int32_t *steer, int accel, int brake, int drift, int unused, int aim_back,
                        int aim_fwd, int trick, int item, int look_back, int auto_accel);
typedef int (*fn_conn)(const void *net);
typedef void *(*fn_np)(const void *net);
typedef int (*fn_stage)(const void *np);
typedef void (*fn_chstage)(void *np, int stage);
typedef uint8_t *(*fn_getobj)(void *sm, uint32_t hash);

/* the pad, for the ticks of the next nativeProjectRun */
enum { IN_ACCEL = 1, IN_BRAKE = 2, IN_DRIFT = 4, IN_ITEM = 8, IN_BACK = 16, IN_FWD = 32, IN_LOOK = 64 };

/* One copy of the engine (split screen runs two: ssr_split.c): its globals,
 * and the pad of the player it belongs to. */
typedef struct {
  void **stack;       /* &g_pScreenStack */
  fn_top top;         /* ScreenStack::GetTopScreen */
  uint8_t **game;     /* &g_pGame */
  uint8_t **net;      /* &g_pNetwork */
  fn_conn conn;       /* STNetwork::CurrentConnection */
  fn_np netplayer;    /* STNetwork::GetNetPlayer */
  fn_stage my_stage;  /* STNetPlayer::GetMyStage */
  fn_chstage change_stage; /* STNetPlayer::ChangeMyStage */
  fn_setc setc;       /* STRacer::SetControls */
  fn_upd orig_update; /* STPlayerControllerHuman::Update */
  void *sm;           /* &g_scene_manager */
  fn_getobj getobj;   /* SceneManager::GetObject */
  struct {
    float steer;
    uint32_t held, pending;
    int auto_accel;
  } in;
  float steer_now; /* the steer handed over last tick (eased) */
  unsigned ticks_driven, net_ticks;
} Eng;
static Eng g_e[2];

/* split screen: player 2's pad, for their racer in player 1's copy's race */
typedef struct {
  float steer;
  uint32_t held, pending;
  int auto_accel;
  float steer_now;
  unsigned ticks;
} In;
static In g_p2;

/* The steer, eased per 60 Hz tick: turning in at most 0.13 a tick (the centre
 * to full lock in about 130 ms), letting go or turning back at 0.22 a tick --
 * a flick of the stick or a D-pad press turns the wheel, not snaps it. */
static float steer_eased(float *now, float want) {
  float d = want - *now;
  const int out = fabsf(want) > fabsf(*now) && want * *now >= 0.0f;
  const float max = out ? 0.13f : 0.22f;
  d = d > max ? max : d < -max ? -max : d;
  *now += d;
  return *now;
}

int ssr_race_active_in(int engine) {
  const Eng *e = &g_e[engine & 1];
  if (!e->stack || !e->top || !*e->stack)
    return 0;
  const uint8_t *top = e->top(*e->stack);
  return top && *(const uint32_t *)(top + 4) == HUD_HASH;
}
int ssr_race_active(void) { return ssr_race_active_in(0); }

/* ------------------------------------------------------------ input */
/* STPlayerControllerHuman::Update(bool active), the engine's per-tick call:
 * what the original does (0x13045c), with the pad for its touches and tilt */
static void drive(Eng *e, uint8_t *self, int active) {
  uint8_t *racer = *(uint8_t **)(self + 4);
  if (e->net && *e->net && e->conn && e->conn(*e->net)) {
    /* a network race (the LOCAL multiplayer: split screen): the original's
     * prelude (0x1305c4..0x130640) -- this racer's stage to 0xcc (racing)
     * with its start pose kept, once; after that its state for the packets
     * the other players get (vtable +0xc, StoreNetPlayerState) */
    void *np = e->netplayer ? e->netplayer(*e->net) : NULL;
    if (np && e->my_stage(np) != 0xcc && !racer[0xbc1]) {
      e->change_stage(np, 0xcc);
      uint32_t *c = (uint32_t *)self;
      const uint32_t *r = (const uint32_t *)racer;
      c[2] = r[4], c[3] = r[5], c[4] = r[6];
      c[5] = c[6] = c[7] = 0;
      memcpy(self + 0x20, racer + 0x1c, 0x24);
    } else {
      void (*store)(void *) = (*(void (***)(void *))self)[3];
      store(self);
    }
    e->net_ticks++;
  }
  /* the original's early return: an inactive controller only runs in the
   * countdown (race state 1: the start boost is charged by accelerating) */
  if (!active && (!*e->game || *(const int32_t *)(*e->game + 0x76c) != 1))
    return;
  /* split screen: player 2's racer, their pad and their camera (SetControls
   * turns the race's camera to look behind) */
  const int p2 = e == &g_e[0] && ssr_split_race_is_p2(racer);
  uint32_t *held = p2 ? &g_p2.held : &e->in.held, *pending = p2 ? &g_p2.pending : &e->in.pending;
  const float want = p2 ? g_p2.steer : e->in.steer;
  const int auto_accel = p2 ? g_p2.auto_accel : e->in.auto_accel;
  const uint32_t b = *held | *pending;
  *pending = 0;
  float s = want < -1.0f ? -1.0f : want > 1.0f ? 1.0f : want;
  const int32_t steer = (int32_t)lrintf(steer_eased(p2 ? &g_p2.steer_now : &e->steer_now, s) * 4096.0f);
  const int brake = !!(b & IN_BRAKE), drift = !!(b & IN_DRIFT);
  const int accel = (auto_accel || (b & IN_ACCEL)) && !brake;
  uint8_t *game = *e->game, *cam = NULL, *cam2 = p2 ? ssr_split_race_cam2() : NULL;
  if (cam2 && game) {
    cam = *(uint8_t **)(game + 0x44);
    *(uint8_t **)(game + 0x44) = cam2;
  }
  e->setc(racer, &steer, accel, brake, drift, 0, !!(b & IN_BACK), !!(b & IN_FWD), drift, !!(b & IN_ITEM),
          !!(b & IN_LOOK), auto_accel);
  if (cam2 && game)
    *(uint8_t **)(game + 0x44) = cam;
  if (p2)
    g_p2.ticks++;
  else
    e->ticks_driven++;
}

/* one entry per copy: `this` is a heap object, it cannot tell them apart */
static void controller_update_1(void *self, int active) { drive(&g_e[0], self, active); }
static void controller_update_2(void *self, int active) { drive(&g_e[1], self, active); }

static uint32_t buttons_of(const SsrPad *p, u64 keys) {
  uint32_t b = 0;
  /* Mario Kart 8 Deluxe's layout */
  if (keys & HidNpadButton_A)
    b |= IN_ACCEL;
  if (keys & HidNpadButton_B)
    b |= IN_BRAKE;
  if (keys & (HidNpadButton_R | HidNpadButton_ZR))
    b |= IN_DRIFT;
  if (keys & (HidNpadButton_L | HidNpadButton_ZL))
    b |= IN_ITEM;
  if (keys & HidNpadButton_X)
    b |= IN_LOOK;
  /* aiming a thrown item: the stick or the D-pad back / forward */
  if (p->ly < -0.5f || (keys & HidNpadButton_Down))
    b |= IN_BACK;
  if (p->ly > 0.5f || (keys & HidNpadButton_Up))
    b |= IN_FWD;
  return b;
}

/* ------------------------------------------------------------ the HUD */
/* the touch controls' sprites (HUD.md 4.1) */
static const uint32_t k_touch_sprites[] = {
    0x87c8051c, /* pause icon */
    0x1887f7ab, /* red brake (tilt) */
    0x3ae7bfdc, /* yellow drift (tilt) */
    0x26704f8a, /* blue item (slider mode) */
    0x5c1f58ad, /* yellow drift (slider mode) */
    0x35c42ab7, /* red brake (slider mode) */
    0x2ee29f06, /* the slider's track */
    0x0d2b7cd6, /* the slider's knob */
};

/* Objects whose keyframes the layouts rewrite: every frame's x, y and scale
 * (they point into the project's KFRM), from the values the project had --
 * the animations read them (a still object keeps its live frame until one
 * plays; these all animate). A frame is 0x28 bytes: +0 its hash, x y sx sy
 * at +0xc. */
enum { K_POS, K_LAP, K_ITEM, K_ITEM2, K_BAR, K_TRACK, K_WIPE, K_LOGO, K_MARK0, K_COUNT = K_MARK0 + 8 };
static const uint32_t k_kf[K_COUNT] = {
    0x45a846aa, /* POS (y 0.091) */
    0x5d53cb16, /* LAP (y 0) */
    0xf15fbcb1, /* the item box (tilt mode: bottom left, slides in from x -0.25) */
    0x3cc95aed, /* the item box (slider mode: y -0.572) */
    0x743100f4, /* the progress bar (a group: scaled about the design's origin) */
    0x63d779ee, /* its track (a sprite: scaled about its middle) */
    0x19707adb, /* the curtain (PauseMenu::PlayWipe: falling off, the race's start) */
    0xadd6f13d, /* ...its picture */
    /* the bar's eight portraits (slide from keyframe 3e3460a8 to f5fda6b0) */
    0xa077e4ed, 0xbccb5f44, 0xd2696624, 0xf0d460c7, 0xd6cde4b8, 0xf8cdc82c, 0x3ae1e924, 0x616a0bb0,
};
typedef struct {
  uint8_t *frames;
  unsigned n;
  float v[16][4]; /* x, y, sx, sy */
} Orig;
static Orig g_orig[2][K_COUNT];

/* object k's frames, their original values in *g (NULL if not there) */
static uint8_t *kf_frames(Eng *e, int ei, int k, Orig **g, unsigned *n) {
  uint8_t *o = e->getobj(e->sm, k_kf[k]);
  uint8_t *kfe = o ? *(uint8_t **)(o + 0xec) : NULL; /* CKeyFrameEntry */
  uint8_t *fr = kfe ? *(uint8_t **)(kfe + 4) : NULL;
  if (!fr)
    return NULL;
  unsigned c = *(const uint16_t *)(kfe + 8);
  c = c > 16 ? 16 : c;
  Orig *og = &g_orig[ei][k];
  if (og->frames != fr || og->n != c) { /* the HUD's data, loaded (again): its own values */
    og->frames = fr, og->n = c;
    for (unsigned i = 0; i < c; i++)
      memcpy(og->v[i], fr + i * 0x28 + 0xc, 4 * sizeof(float));
  }
  *g = og, *n = c;
  return fr;
}

/* An object at rest on one of its keyframes (its live place, +0x3c, that
 * frame's as it was) moves with the frame when a layout changes it: the
 * game takes a keyframe's place only when an animation plays, and POS / LAP
 * play theirs once, as the HUD slides in -- in BATTLE and VS RACE sometimes
 * before the halves' layout was set (hardware, round 11: player 2's LAP /
 * POS left where the phone has them). Not while the game holds its place
 * (+0xf4: position 0x2, scale 0x8). */
static void live_follow(Eng *e, int k, uint8_t *fr, unsigned n, const float (*was)[4]) {
  uint8_t *o = e->getobj(e->sm, k_kf[k]);
  if (!o || (o[0xf4] & 0x0a))
    return;
  float *lv = (float *)(o + 0x3c);
  for (unsigned i = 0; i < n; i++) {
    const float *now = (const float *)(fr + i * 0x28 + 0xc);
    if (fabsf(lv[0] - was[i][0]) < 1e-4f && fabsf(lv[1] - was[i][1]) < 1e-4f && fabsf(lv[2] - was[i][2]) < 1e-4f &&
        fabsf(lv[3] - was[i][3]) < 1e-4f) {
      if (memcmp(lv, now, 4 * sizeof(float))) {
        memcpy(lv, now, 4 * sizeof(float));
        o[0xe3] = 1; /* its render properties again */
      }
      return;
    }
  }
}

/* object k: x + dx; y (y_set: set_y, else its own) + dy; its scale x sx, sy */
static void kf_edit(Eng *e, int ei, int k, float dx, int y_set, float set_y, float dy, float sx, float sy) {
  Orig *g;
  unsigned n;
  uint8_t *fr = kf_frames(e, ei, k, &g, &n);
  float was[16][4];
  for (unsigned i = 0; fr && i < n; i++) {
    float *f = (float *)(fr + i * 0x28 + 0xc);
    memcpy(was[i], f, sizeof was[i]);
    f[0] = g->v[i][0] + dx;
    f[1] = (y_set ? set_y : g->v[i][1]) + dy;
    f[2] = g->v[i][2] * sx, f[3] = g->v[i][3] * sy;
  }
  if (fr)
    live_follow(e, k, fr, n, (const float (*)[4])was);
}

/* a group scaled s about the point (cx, *) of the design (x only) */
static void kf_stretch_x(Eng *e, int ei, int k, float s, float cx) {
  Orig *g;
  unsigned n;
  uint8_t *fr = kf_frames(e, ei, k, &g, &n);
  for (unsigned i = 0; fr && i < n; i++) {
    float *f = (float *)(fr + i * 0x28 + 0xc);
    f[0] = g->v[i][0] * s + cx * (1.0f - s);
    f[1] = g->v[i][1], f[2] = g->v[i][2] * s, f[3] = g->v[i][3];
  }
}

/* The split race's one progress bar, across the middle of the screen (the
 * consoles'), drawn over both halves at the whole screen's size (the HUD's
 * scene uniform: 1.125 window pixels a design pixel at 720): the group at
 * scale S with the track's middle (0.5005, 0.053) on the screen's (0.5,
 * 0.5); the track K times as long and thinner; the portraits' slide K times
 * as wide about its middle. */
#define BAR_S 0.8f
#define BAR_K 3.05f /* (nine tenths of the screen's width: the console's divide) */
#define BAR_CX 0.5005f
#define BAR_T 1.35f /* its track's thickness: a bar to see, across the divide */
static void bar_across(Eng *e, int ei) {
  Orig *g;
  unsigned n;
  uint8_t *fr = kf_frames(e, ei, K_BAR, &g, &n);
  for (unsigned i = 0; fr && i < n; i++) {
    float *f = (float *)(fr + i * 0x28 + 0xc);
    f[0] = 0.5f - BAR_CX * BAR_S, f[1] = 0.5f - 0.053f * BAR_S, f[2] = f[3] = BAR_S;
  }
  kf_edit(e, ei, K_TRACK, 0, 0, 0, 0, BAR_K, BAR_T);
  for (int m = 0; m < 8; m++) {
    fr = kf_frames(e, ei, K_MARK0 + m, &g, &n);
    for (unsigned i = 0; fr && i < n; i++) {
      float *f = (float *)(fr + i * 0x28 + 0xc);
      const float half = 0.0335f; /* the sprite's half width (0.067, unscaled) */
      f[0] = BAR_CX + (g->v[i][0] + half - BAR_CX) * BAR_K - half;
      f[1] = g->v[i][1], f[2] = g->v[i][2], f[3] = g->v[i][3];
    }
  }
}

/* Objects held transparent (the engine's alpha override: +0xf4 bit 7 keeps
 * the live alpha, +0x3b): given back, with their alpha, when not wanted. */
enum { H_BAR = 0, H_BAR_N = 9, H_TIMER = 9, H_MSG = 10, H_ITEMBOX = 13, H_CINE = 17, H_OVER = 19, H_COUNT = 41 };
static const uint32_t k_hide[H_COUNT] = {
    0x63d779ee, 0xa077e4ed, 0xbccb5f44, 0xd2696624, 0xf0d460c7, 0xd6cde4b8, 0xf8cdc82c, 0x3ae1e924, 0x616a0bb0,
    /* ^ the progress bar's track and its eight portraits */
    0x69e00551,                         /* the lap timer */
    0x45ccec24, 0xdaf6de93, 0xff8782da, /* 1ST, FINISHED!, LET'S RACE! (slide in from beside the screen) */
    0xcb2e9de4, 0xd7316892, 0x53278ab1, 0xe089fac6, /* the item boxes' box and icon (tilt, slider) */
    0x1c1d5b02, 0xccecf253,             /* the cinema's black bars, top and bottom (HUD::CinematicFudge) */
    /* over both halves (the progress bar's pass), what would be in its band:
     * the finish's words, the curtain, the warnings of what comes behind */
    0xdaf6de93, 0x45ccec24, 0xadd6f13d, 0x0839116b, 0x670a844e, 0xc46fcc52, 0x9cf54b15, 0xb24a274b, 0xb8e85b34,
    0xd3d4dde7, 0xa94baadd, 0x3fabcb02, 0x48118113, 0x0a44f302, 0x9dfc0bf0, 0x1af083ae, 0xe4cf5264, 0x37beed79,
    0xc6f24439, 0x5cd19acc, 0x336cff5f, 0x9d2459b8,
};
static struct {
  uint8_t *o;
  uint8_t alpha;
} g_hid[2][H_COUNT];

static void hold(Eng *e, int ei, int k, int hide) {
  uint8_t *o = e->getobj(e->sm, k_hide[k]);
  if (hide && o) {
    if (g_hid[ei][k].o != o)
      g_hid[ei][k].o = o, g_hid[ei][k].alpha = o[0x3b] ? o[0x3b] : 255;
    o[0xf4] |= 0x80;
    o[0x3b] = 0;
  } else if (g_hid[ei][k].o) {
    if (o == g_hid[ei][k].o) {
      o[0xf4] &= (uint8_t)~0x80;
      o[0x3b] = g_hid[ei][k].alpha;
    }
    g_hid[ei][k].o = NULL;
  }
}

/* an object's live x (its local frame: +0x3c), or 0.5 */
static float live_x(Eng *e, uint32_t hash) {
  const uint8_t *o = e->getobj(e->sm, hash);
  return o ? *(const float *)(o + 0x3c) : 0.5f;
}

static int g_layout[2] = {-1, -1};
static int g_own_bar; /* split screen's VS RACE: each copy's own progress bar, in its half */

void ssr_race_hud_own_bar(int on) { g_own_bar = on; }

/* The HUD of copy `engine`'s race, every frame while it has one:
 *  - the phone's touch controls transparent (always);
 *  - SSR_HUD_PHONE: the console layout full-screen: the item box at the top
 *    left, POS above LAP at the top right (their y: HUD.md 5.3);
 *  - SSR_HUD_SPLIT (split screen, player 1's, in the top half), SSR_HUD_TOP
 *    (split screen's BATTLE: player 1's copy's own top half, no bar across)
 *    and SSR_HUD_BOTTOM (player 2's, player 2's copy's, in the bottom half): the
 *    scenes' fit is uniform, `margin` design widths beside the design; the
 *    consoles' split HUD -- the item box and POS / LAP at the half's outer
 *    corners (the top half's at its top, the bottom's at its bottom), no
 *    timer, no cinema bars, the curtain across the half; one progress bar,
 *    player 1's, across the middle of the screen (its own pass:
 *    ssr_race_hud_pass); what is parked beside the phone's screen (the
 *    messages, the item box before its first item) transparent while it is.
 */
void ssr_race_hud_in(int engine, int layout, float margin) {
  Eng *e = &g_e[engine & 1];
  const int ei = engine & 1;
  if (!e->getobj || !e->sm || !e->game || (!*e->game && !(engine == 1 && layout == SSR_HUD_BOTTOM)))
    return;
  for (unsigned i = 0; i < sizeof k_touch_sprites / sizeof k_touch_sprites[0]; i++) {
    uint8_t *o = e->getobj(e->sm, k_touch_sprites[i]);
    if (!o)
      continue; /* the HUD scene is not made yet */
    o[0xf4] |= 0x80; /* keep the live alpha: animations leave it */
    o[0x3b] = 0;     /* ...at 0 */
  }
  const int split = layout != SSR_HUD_PHONE, bottom = layout == SSR_HUD_BOTTOM;
  const float m = split ? margin : 0.0f, down = bottom ? 0.735f : 0.0f;
  kf_edit(e, ei, K_POS, m, 1, 0.0f, down, 1, 1);
  kf_edit(e, ei, K_LAP, m, 1, 0.0906f, down, 1, 1);
  const float item_y = bottom ? 0.02f : -0.60f;
  kf_edit(e, ei, K_ITEM, -m, 1, item_y, 0.0f, 1, 1);
  kf_edit(e, ei, K_ITEM2, -m, 1, item_y, 0.0f, 1, 1);
  if (layout == SSR_HUD_SPLIT) {
    bar_across(e, ei);
  } else {
    kf_edit(e, ei, K_BAR, 0, 0, 0, 0, 1, 1);
    kf_edit(e, ei, K_TRACK, 0, 0, 0, 0, 1, 1);
    for (int k = 0; k < 8; k++)
      kf_edit(e, ei, K_MARK0 + k, 0, 0, 0, 0, 1, 1);
  }
  /* the curtain across the half (its picture as it was) */
  const float s = split ? 1.0f + 2.0f * m : 1.0f;
  kf_stretch_x(e, ei, K_WIPE, s, 0.5f);
  kf_edit(e, ei, K_LOGO, 0, 0, 0, 0, 1.0f / s, 1);
  if (layout != g_layout[ei] && layout == SSR_HUD_PHONE)
    for (int k = 0; k < H_COUNT; k++)
      hold(e, ei, k, 0);
  g_layout[ei] = layout;
  /* the bar: a split race's is player 1's, over both halves only
   * (ssr_race_hud_pass); in VS RACE each copy's own, at the top of its half
   * (where the game has it); none in player 2's HUD of a split race, nor a
   * battle's */
  const int own_bar = g_own_bar && (layout == SSR_HUD_TOP || layout == SSR_HUD_BOTTOM);
  for (int k = H_BAR; k < H_BAR + H_BAR_N; k++)
    hold(e, ei, k, split && !own_bar);
  hold(e, ei, H_TIMER, split);
  hold(e, ei, H_CINE, split), hold(e, ei, H_CINE + 1, split);
  for (int k = 0; k < 3; k++) {
    const float x = live_x(e, k_hide[H_MSG + k]);
    hold(e, ei, H_MSG + k, split && (x < -0.1f || x > 1.1f));
  }
  /* the item box's first place (x -0.25, its keyframes' before they moved):
   * beside the phone's screen, inside a half */
  const int ghost1 = split && fabsf(live_x(e, k_kf[K_ITEM]) + 0.25f) < 0.02f;
  const int ghost2 = split && fabsf(live_x(e, k_kf[K_ITEM2]) + 0.25f) < 0.02f;
  hold(e, ei, H_ITEMBOX + 0, ghost1), hold(e, ei, H_ITEMBOX + 1, ghost1);
  hold(e, ei, H_ITEMBOX + 2, ghost2), hold(e, ei, H_ITEMBOX + 3, ghost2);
}

/* (the logs: POS / LAP, live and their keyframes, in copy `engine`) */
void ssr_race_hud_debug(int engine) {
  Eng *e = &g_e[engine & 1];
  if (!e->getobj || !e->sm)
    return;
  for (int k = K_POS; k <= K_LAP; k++) {
    uint8_t *o = e->getobj(e->sm, k_kf[k]);
    if (!o)
      continue;
    const float *v = (const float *)(o + 0x3c);
    uint8_t *kfe = *(uint8_t **)(o + 0xec);
    uint8_t *fr = kfe ? *(uint8_t **)(kfe + 4) : NULL;
    const unsigned n = kfe ? *(const uint16_t *)(kfe + 8) : 0;
    char line[300];
    int w = snprintf(line, sizeof line, "[race] copy %d %s: live %.3f,%.3f scale %.3f,%.3f; frames", engine + 1,
                     k == K_POS ? "POS" : "LAP", v[0], v[1], v[2], v[3]);
    for (unsigned i = 0; fr && i < n && i < 4; i++) {
      const float *f = (const float *)(fr + i * 0x28 + 0xc);
      w += snprintf(line + w, sizeof line - (size_t)w, " [%.3f,%.3f %.3f,%.3f]", f[0], f[1], f[2], f[3]);
    }
    debugPrintf("%s\n", line);
  }
}

/* a split race's pass (inside the frame): over both halves only the bar;
 * player 1's half everything but it; after the frame, as in player 1's half */
void ssr_race_hud_pass(int engine, int pass) {
  Eng *e = &g_e[engine & 1];
  const int ei = engine & 1;
  if (!e->getobj || !e->sm || g_layout[ei] != SSR_HUD_SPLIT)
    return;
  const int over = pass == 3;
  for (int k = H_BAR; k < H_BAR + H_BAR_N; k++)
    hold(e, ei, k, !over);
  for (int k = H_OVER; k < H_COUNT; k++)
    hold(e, ei, k, over);
}

/* ------------------------------------------------------------ the frame */
/* before copy `engine`'s nativeProjectRun: its player's pad (racing), or
 * nothing */
void ssr_race_frame_in(int engine, const SsrPad *p, int racing) {
  Eng *e = &g_e[engine & 1];
  if (!racing) {
    e->in.held = e->in.pending = 0;
    e->in.steer = 0.0f;
    e->steer_now = 0.0f;
    return;
  }
  e->in.held = buttons_of(p, p->held);
  e->in.pending |= buttons_of(p, p->down);
  e->in.steer = ssr_input_steer(p);
  e->in.auto_accel = dcr_config()->auto_accelerate;
}
void ssr_race_frame(const SsrPad *p, int racing) { ssr_race_frame_in(0, p, racing); }

void ssr_race_frame_p2(const SsrPad *p, int racing) {
  if (!racing) {
    g_p2.held = g_p2.pending = 0;
    g_p2.steer = g_p2.steer_now = 0.0f;
    return;
  }
  g_p2.held = buttons_of(p, p->held);
  g_p2.pending |= buttons_of(p, p->down);
  g_p2.steer = ssr_input_steer(p);
  g_p2.auto_accel = dcr_config()->auto_accelerate;
}

/* ------------------------------------------------------------ rumble */
/* the copy that shakes is the one running (its player's controller); in
 * a split race, the racer the engine is working on (player 2's: theirs) */
static void rumble(int ms, float amp) {
  ssr_input_rumble_player(ssr_engine_current() ? 1 : ssr_split_race_player(), ms, amp);
}
static void rumble_long(void) { rumble(300, 0.75f); }
static void rumble_big(void) { rumble(150, 0.95f); }
static void rumble_medium(void) { rumble(150, 0.65f); }
static void rumble_small(void) { rumble(150, 0.35f); }

/* ------------------------------------------------------------ words */
static struct {
  uint32_t id;
  char text[112];
} g_words[10];
static int g_nwords;

static void word(uint32_t id, const char *text) {
  if (g_nwords < (int)(sizeof g_words / sizeof g_words[0])) {
    g_words[g_nwords].id = id;
    snprintf(g_words[g_nwords].text, sizeof g_words[0].text, "%s", text);
    g_nwords++;
  }
}

#define NBSP "\xc2\xa0" /* keeps "R BUTTON" on one line, as the game's own strings do */

static void words_init(void) {
  const DcrConfig *c = dcr_config();
  const int lang = ssr_language();
  if (lang != 0 && lang != 6)
    return; /* English only: the other languages keep the game's words */
  const char *accel = c->swap_ab ? "B" : "A", *brake = c->swap_ab ? "A" : "B";
  const char *steer = c->steering == SSR_STEER_MOTION ? "TURN THE CONTROLLER" : "USE THE LEFT STICK";
  char s[112];
  if (c->auto_accelerate)
    snprintf(s, sizeof s, "GAS IS AUTOMATIC, %s TO STEER.", steer);
  else
    snprintf(s, sizeof s, "HOLD THE %s" NBSP "BUTTON FOR GAS AND %s TO STEER.", accel, steer);
  word(0x42eae879, s); /* GAS IS AUTOMATIC, TILT THE SCREEN TO STEER. */
  word(0xfe565508, s); /* ...USE THE SLIDER TO STEER. */
  snprintf(s, sizeof s, "BRAKE USING THE %s" NBSP "BUTTON. KEEP HOLDING FOR REVERSE.", brake);
  word(0xd9111d06, s); /* BRAKE USING THE RED BUTTON... */
  word(0xd131a178, "WHILE DRIVING, STEER AND HOLD R OR" NBSP "ZR TO DRIFT.");
  word(0xcd746d83, "KEEP HOLD OF R OR" NBSP "ZR TO KEEP SLIDING.");
  word(0xa5c62615, "DRIFTING CHARGES THE TURBO. RELEASE R OR" NBSP "ZR TO BOOST.");
  word(0x43f20217, "TO USE AN ITEM PRESS L OR" NBSP "ZL.");   /* ...TOUCH THE ITEM'S ICON. */
  word(0x8303ccb7, "TO USE AN ITEM PRESS L OR" NBSP "ZL.");   /* ...TOUCH THE BLUE BUTTON. */
  snprintf(s, sizeof s, "PRESS %s TO CONTINUE", accel);
  word(0x3e4c6353, s); /* TAP TO CONTINUE */
}

/* SiffTextManager::GetString(unsigned id) const: the loaded string tables
 * (a list: +0 next, +8 count, +0xc {hash, string} sorted by hash), searched
 * as the engine does. */
static const char *text_find(const void *const *mgr, uint32_t id) {
  for (const uint8_t *t = *(const uint8_t *const *)mgr; t; t = *(const uint8_t *const *)t) {
    const int n = *(const int32_t *)(t + 8);
    const uint32_t *e = (const uint32_t *)(t + 0xc);
    int lo = 0, hi = n - 1;
    while (hi >= lo) {
      const int mid = lo + (hi - lo) / 2;
      const uint32_t h = e[mid * 2];
      if (id > h)
        lo = mid + 1;
      else if (id == h)
        return (const char *)(uintptr_t)e[mid * 2 + 1];
      else
        hi = mid - 1;
    }
  }
  return NULL;
}

/* The title's TAP TO START (the title project's font has only the letters of
 * its own strings: each word uses those): "PRESS" and two spaces -- the game
 * centres the line, the A button is drawn in the spaces (ssr_menu.c): the
 * console editions' "PRESS (A)", centred, a gap between. */
static const char *title_press(void) {
  switch (ssr_language()) {
  case 1:
    return "APPUIE SUR  ";
  case 2:
    return "PREMI  ";
  case 3:
    return "START MIT  ";
  case 4:
    return "PULSA  ";
  default:
    return "PRESS  ";
  }
}

/* The game's other words, where the console editions differ (split screen,
 * ssr_split.c): the main menu's MULTIPLAYER card is SPLIT SCREEN (English:
 * the menus' font has its letters, other languages keep their word), and the
 * Solo menu reached by it is SELECT MODE (the game's own string), its TIME
 * TRIAL card split screen's BATTLE (the game's own word), its MISSIONS card
 * VS RACE (English; the game's RACE in the others). */
static const void *const *g_text_mgr[2]; /* each copy's (the last asked) */

/* a racer's name (STCharacter), in the game's words: the logs */
const char *ssr_race_racer_name(int engine, int ch) {
  const uint32_t *names = (const uint32_t *)ssr_native_in(engine & 1, "s_uCharacterNames");
  const char *s = names && ch >= 0 && ch < 20 && g_text_mgr[engine & 1] ? text_find(g_text_mgr[engine & 1], names[ch])
                                                                         : NULL;
  return s ? s : "?";
}

static const char *text_get(const void *const *mgr, uint32_t id) {
  g_text_mgr[ssr_engine_current() & 1] = mgr;
  for (int i = 0; i < g_nwords; i++)
    if (g_words[i].id == id)
      return g_words[i].text;
  if (id == 0xe9f4569du) /* TAP TO START */
    return title_press();
  const char *orig = text_find(mgr, id);
  if (dcr_config()->split_screen) {
    const int lang = ssr_language();
    if (id == 0x7fe90337u && (lang == 0 || lang == 6)) /* MULTIPLAYER (the main menu's card) */
      return "SPLIT SCREEN";
    if (id == 0xf087fed3u && ssr_split_mode_select()) { /* SINGLE PLAYER (the Solo menu's footer: SoloMenu::Update) */
      const char *as = text_find(mgr, 0x76c3f025u);  /* SELECT MODE */
      if (as)
        return as;
    }
    if ((id == 0x8a96dccdu || id == 0xcabb26afu) && ssr_split_mode_select()) { /* TIME TRIAL: split screen's BATTLE */
      const char *as = text_find(mgr, 0x45ad07f6u); /* BATTLE */
      if (as)
        return as;
    }
    if (id == 0x8a52df2eu && ssr_split_mode_select()) { /* MISSIONS: split screen's VS RACE (the card's font
                                                        * has its letters); other languages: RACE */
      if (lang == 0 || lang == 6)
        return "VS RACE";
      const char *as = text_find(mgr, 0x3cfa495eu); /* RACE */
      if (as)
        return as;
    }
  }
  return orig;
}

/* ------------------------------------------------------------ voices */
/* Some of the racers' lines are asked for under Audio/wav/CHAVO/ -- a typo in
 * the game's own sound data (the phone never found them either: "NOT found"
 * in the log) -- where the wad has them under CHVO/. ResourceManager::
 * GetFileInfo (the lookup by name) gets the right folder; each corrected name
 * is kept for good (a lookup may keep the pointer). */
typedef int (*fn_file_info)(void *rm, void *lookup, const char *name, int flag);
static fn_file_info o_file_info_e[2]; /* each copy's original */
static struct {
  char *from, *to;
} g_fixed[64];
static int g_nfixed;

/* one hook per copy (file_info0 / 1): the original is the calling copy's own */
static int file_info_in(int e, void *rm, void *lookup, const char *name, int flag) {
  const fn_file_info o_file_info = o_file_info_e[e];
  const char *typo = name ? strstr(name, "/CHAVO/") : NULL;
  if (!typo)
    return o_file_info(rm, lookup, name, flag);
  for (int i = 0; i < g_nfixed; i++)
    if (!strcmp(g_fixed[i].from, name))
      return o_file_info(rm, lookup, g_fixed[i].to, flag);
  const size_t n = strlen(name);
  char *to = malloc(n);
  if (!to)
    return o_file_info(rm, lookup, name, flag);
  memcpy(to, name, (size_t)(typo - name));
  strcpy(to + (typo - name), "/CHVO/");
  strcpy(to + (typo - name) + 6, typo + 7);
  if (g_nfixed < (int)(sizeof g_fixed / sizeof g_fixed[0])) {
    g_fixed[g_nfixed].from = strdup(name);
    g_fixed[g_nfixed].to = to;
    if (g_fixed[g_nfixed].from)
      g_nfixed++;
  }
  static int told;
  if (told++ < 4)
    debugPrintf("[race] a racer's line: %s (the game's data says CHAVO)\n", to);
  return o_file_info(rm, lookup, to, flag);
}
static int file_info0(void *rm, void *lookup, const char *name, int flag) { return file_info_in(0, rm, lookup, name, flag); }
static int file_info1(void *rm, void *lookup, const char *name, int flag) { return file_info_in(1, rm, lookup, name, flag); }

/* ------------------------------------------------------------ set up */
int ssr_patch_jump(const char *sym, uint32_t expect, void *dst); /* ssr_patch.c, at load */
void *ssr_patch_hook(const char *sym, uint32_t expect, void *dst);

/* ssr_patch.c, while the library is being loaded (not yet sealed) */
void ssr_race_patch(void) {
  int n = 0;
  n += ssr_patch_jump("_ZN11STVibration11VibrateLongEv", 0xe3a00001u, (void *)rumble_long);
  n += ssr_patch_jump("_ZN11STVibration10VibrateBigEv", 0xe3a00004u, (void *)rumble_big);
  n += ssr_patch_jump("_ZN11STVibration13VibrateMediumEv", 0xe3a00002u, (void *)rumble_medium);
  n += ssr_patch_jump("_ZN11STVibration12VibrateSmallEv", 0xe3a00001u, (void *)rumble_small);
  const int t = ssr_patch_jump("_ZNK15SiffTextManager9GetStringEj", 0xe5903000u, (void *)text_get);
  int ssr_patch_engine_index(void);
  void *fi = ssr_patch_hook("_ZN15ResourceManager11GetFileInfoEP14SWadLookupDataPKcb", 0xe92d4ff0u,
                            ssr_patch_engine_index() ? (void *)file_info1 : (void *)file_info0);
  if (fi)
    o_file_info_e[ssr_patch_engine_index()] = (fn_file_info)fi;
  else
    debugPrintf("[race] the racers' CHAVO lines: left as the game has them\n");
  debugPrintf("[race] rumble: %d of 4 hooks; the game's words: %s\n", n, t ? "hooked" : "the engine's own");
}

/* copy `engine`'s globals, and its controller's Update pointed at the pad */
void ssr_race_init_in(int engine) {
  Eng *e = &g_e[engine & 1];
#define NAT(s) ssr_native_in(engine, s)
  e->stack = (void **)NAT("g_pScreenStack");
  e->top = (fn_top)NAT("_ZNK11ScreenStack12GetTopScreenEv");
  e->game = (uint8_t **)NAT("g_pGame");
  e->net = (uint8_t **)NAT("g_pNetwork");
  e->conn = (fn_conn)NAT("_ZNK9STNetwork17CurrentConnectionEv");
  e->netplayer = (fn_np)NAT("_ZNK9STNetwork12GetNetPlayerEv");
  e->my_stage = (fn_stage)NAT("_ZNK11STNetPlayer10GetMyStageEv");
  e->change_stage = (fn_chstage)NAT("_ZN11STNetPlayer13ChangeMyStageEi");
  e->setc = (fn_setc)NAT("_ZN7STRacer11SetControlsE6STFx32bbbbbbbbbb");
  e->sm = NAT("g_scene_manager");
  e->getobj = (fn_getobj)NAT("_ZN12SceneManager9GetObjectEj");
  if (!e->netplayer || !e->my_stage || !e->change_stage)
    e->net = NULL; /* no network prelude: network races keep their pad off */
  void **vt = (void **)NAT("_ZTV23STPlayerControllerHuman");
  /* vtable + 8: the first virtual; Update is the third (+0x10) */
  if (vt && e->setc && e->game && vt[4] == NAT("_ZN23STPlayerControllerHuman6UpdateEb")) {
    e->orig_update = (fn_upd)vt[4];
    vt[4] = engine ? (void *)controller_update_2 : (void *)controller_update_1;
  }
#undef NAT
  if (engine == 0)
    words_init();
  debugPrintf("[race] engine %d: screen stack %s; controller %s; network races %s; HUD objects %s; %d of the "
              "tutorial's words in the Switch's buttons\n",
              engine + 1, e->stack && e->top ? "found" : "NOT FOUND",
              e->orig_update ? "driven directly (SetControls)" : "NOT HOOKED: races will not respond",
              e->net ? "driven too" : "NOT DRIVEN", e->getobj && e->sm ? "found" : "NOT FOUND", g_nwords);
}
void ssr_race_init(void) { ssr_race_init_in(0); }

/* every 10 s: the ticks the controller drove (0 while not racing) */
void ssr_race_report(void) {
  for (int i = 0; i < 2; i++) {
    Eng *e = &g_e[i];
    if (e->ticks_driven)
      debugPrintf("[race] engine %d: %u logic steps driven by the controller (%u of them a network race's)\n", i + 1,
                  e->ticks_driven, e->net_ticks);
    e->ticks_driven = e->net_ticks = 0;
  }
  if (g_p2.ticks)
    debugPrintf("[race] player 2's racer: %u logic steps driven by their controller\n", g_p2.ticks);
  g_p2.ticks = 0;
}
