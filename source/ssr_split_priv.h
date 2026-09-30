/* ssr_split_priv.h -- what ssr_split.c (the session, player 2's copy of the
 * engine, the menus) and ssr_split_race.c (the race) share. MIT. */
#ifndef SSR_SPLIT_PRIV_H
#define SSR_SPLIT_PRIV_H
#include <stdint.h>
#include <string.h>

#include "ssr.h"

/* with copy e as the running one (its JNI calls, its threads) */
#define AS_ENGINE(e, ...)                                                                                             \
  do {                                                                                                               \
    const int was_ = ssr_engine_current();                                                                           \
    ssr_engine_set_current(e);                                                                                       \
    __VA_ARGS__;                                                                                                     \
    ssr_engine_set_current(was_);                                                                                    \
  } while (0)

typedef uint8_t *(*fn_succ)(uint8_t *node);
typedef void (*fn_this)(void *self);
typedef void (*fn_dims)(uint32_t w_bits, uint32_t h_bits); /* (float, float): softfp, in r0 / r1 */

/* A copy's scenes: CSceneEntry nodes of the scene manager's tree (its root
 * at *sm, the leftmost first, the successor function not exported:
 * 0x19d9c4). A node: +0x1c its SCNE entry (+0 the scene's hash, +8 its fit
 * mode), +0x2c made, +0x30 its load set (textures), +0x34 design w, h, +0x3c
 * scale x, y, +0x44 offset x, y, +0x4c the scissor x0, y0, x1, y1, +0x5c the
 * scissor in the design's units (CalculateWidescreenSettings works it out
 * last: what the screen shows of the design -- the rainbow's splash covers
 * it, STHUD_SpriteRainbow::PreSumoToolRender, the HUD scene's). */
#define SCENE_XF 0x34               /* a scene's transform: fourteen floats */
#define SCENE_XF_LEN (14 * 4)
/* the scissor in the design's units again, after the transform was set here */
static inline void scene_xf_derive(uint8_t *n) {
  float *f = (float *)(n + SCENE_XF);
  if (f[2] == 0.0f || f[3] == 0.0f)
    return;
  f[10] = (f[6] - f[4]) / f[2], f[11] = (f[7] - f[5]) / f[3];
  f[12] = (f[8] - f[4]) / f[2], f[13] = (f[9] - f[5]) / f[3];
}
static inline uint8_t *scene_first(uint8_t *sm) {
  uint8_t *n = sm ? *(uint8_t **)sm : NULL;
  while (n && *(uint8_t **)(n + 4))
    n = *(uint8_t **)(n + 4);
  return n;
}
static inline uint32_t scene_hash(const uint8_t *n) {
  const uint8_t *ent = n && n[0x2c] ? *(const uint8_t *const *)(n + 0x1c) : NULL;
  return ent ? *(const uint32_t *)ent : 0;
}

/* the HUD's scenes (hud_notv.star): the HUD, the screen effects, the countdown */
#define SC_HUD 0x91c494eau
#define SC_EFFECTS 0xbc4c75e3u
#define SC_COUNTDOWN 0xbecc8d6eu
static inline int is_hud_scene(uint32_t h) { return h == SC_HUD || h == SC_EFFECTS || h == SC_COUNTDOWN; }

/* ssr_split.c: each copy's screens, by class (their vtables) */
enum { VT_TITLE, VT_LICENSE, VT_MAIN, VT_SOLO, VT_CHARSEL, VT_HUD, VT_TRACKSEL, VT_GPSEL, VT_GPDIFF,
       VT_CREATE_LICENSE, VT_KEYBOARD, VT_PORTRAIT, VT_NATION, VT_LICSCREEN, VT_SEGASDK, VT_MISSIONSEL, VT_NET,
       VT_SERVERS, VT_LOBBY, VT_POP_LOST, VT_POP_FAILED, VT_POP_JOIN, VT_POP_WAITING, VT_POP_CONNECTING,
       VT_RESULTS_GP, VT_MILES, VT_REPORT, VT_MPSERVICE, VT_COUNT };
uint8_t *ssr_split_top(int i);                       /* copy i's top screen */
int ssr_split_is(int i, const void *obj, int vt);     /* ...of that class */
int ssr_split_loading(int i);                         /* its loading screen is up */
int ssr_split_popup(int i);                           /* its pop-up: 1 taking input, -1 coming or going, 0 none */
uint8_t *ssr_split_popup_obj(int i);                  /* ...the one taking input */
void ssr_split_wh(int *w, int *h);                    /* the window */
int ssr_split_players_licence(int i);                 /* copy 2 at player 2's licence screens (theirs to pick) */
void ssr_split_place_columns(int i, int on);          /* copy i's scenes in its column (the racer select's way) */
void ssr_split_fit_now(int i, int w, int h, int mode); /* copy i's size and its scenes' fit, now */

/* ssr_split_net.c: split screen's BATTLE and VS RACE (the phone's LOCAL battle and race, over the
 * in-memory LAN) */
void ssr_split_net_choose(int battle);                /* BATTLE (1) or VS RACE (0) chosen (the mode select's A) */
int ssr_split_net_active(void);                       /* a BATTLE / VS RACE session is on */
void ssr_split_net_pre(void);                         /* each frame, before player 1's copy runs */
int ssr_split_net_pad1(SsrPad *p);                    /* player 1's menus' pad, driven while it opens / leaves */
int ssr_split_net_script2(SsrPad *sp, uint8_t *top);  /* player 2's copy's scripted pad: 1 if the battle's */
int ssr_split_net_theirs(const uint8_t *top);         /* player 2's copy's screen is player 2's (the battle's) */
int ssr_split_net_auto_popup(void);                   /* ...its pop-up is the network's (the script answers) */
int ssr_split_net_display(int drew);                  /* the screen's layout for the battle: 1 if set */
void ssr_split_net_frame2(const SsrPad *p);           /* inside player 2's copy, before its frame */
int ssr_split_net_race(void);                         /* both copies race: each draws its half */
int ssr_split_net_racing(void);                       /* either copy in the battle / race or its results */
int ssr_split_net_hud_layout(int engine);             /* SSR_HUD_* in the battle, or -1 */

/* ssr_split.c */
int ssr_split_p2_char(void);         /* player 2's racer (STCharacter), -1 none */
const char *ssr_split_p2_name(void); /* player 2's licence's name */
int ssr_split_e2_up(void);           /* player 2's copy runs */
void ssr_split_e2_size(int w, int h, int uniform); /* its size (and its scenes' fit) */
void ssr_split_e1_size(int w, int h, int uniform); /* player 1's */

int ssr_split_columns_render(int i, uint8_t *sm); /* copy i's scenes, its racer select in its column: 1 if drawn */

/* ssr_split_race.c */
void ssr_split_race_patch(void);     /* at load (player 1's copy) */
void ssr_split_race_init(void);      /* after player 1's copy is up */
void ssr_split_race_pre(void);       /* every frame, before player 1's copy runs */
void ssr_split_race_pad2(const SsrPad *p);
void ssr_split_race_preload2(int at_racer_select); /* each frame player 2's copy runs, inside it */
void ssr_split_race_hud2_reload(void);             /* player 2's copy raced itself: its HUD project again */
int ssr_split_race_active(void);     /* a two-player race is on (its racers made) */
int ssr_split_race_split(void);      /* ...and the screen is split now */
int ssr_split_race_player(void);     /* whose racer the engine is working on now: 0, 1 */
void ssr_split_race_report(void);

#endif
