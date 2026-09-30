/* ssr_split_race.c -- a split screen race (ssr_split.c: the menus).
 *
 * The race is player 1's copy of the engine's, the game's own -- a single
 * race or a Grand Prix, its AI racers, its items, its voices, its pause menu,
 * its results -- with player 2 in it as a second human racer:
 *
 *  - THE RACERS. The race's setup fills its racers after the first, the
 *    human's, with AI (STCurrGameData::PadOutWithAI(n): n racers already
 *    chosen). In split screen the second is player 2's -- their racer, a
 *    human's -- and the AI fill the rest. A human racer gets the controller
 *    the engine gives the player (STPlayerControllerHuman: ssr_race.c drives
 *    it from player 2's pad) and its AI stand-in for after the finish line.
 *  - THE CAMERAS. The race has one follow camera (g_pGame +0x44); player 2
 *    gets a second, made as the race makes the first (STGameCamera, its
 *    target player 2's racer), updated each tick after their racer, given the
 *    race's own changes of mode (the countdown, the replay after the line).
 *    While player 2's racer is worked on (its update, its controls) the
 *    race's camera is theirs: looking behind, the shakes, the finish's cut.
 *  - THE PICTURE. The engine draws a frame three times (render(), 0x1f1320,
 *    re-implemented): player 1's view and HUD into the top half, player 2's
 *    view into the bottom (the race's camera and visible track section
 *    theirs for it); player 2's HUD over the bottom half, drawn by player 2's
 *    copy (below); then, over the whole screen, what is both players': the
 *    pause menu, the pop-ups, and one long race progress bar across the
 *    middle, as the consoles have it. The 2D scenes of each pass are chosen
 *    (SceneManager::RenderScenes, re-implemented) and laid out for it (each
 *    scene's transform: the half's, or the whole screen's). The fly-by before
 *    the countdown is whole.
 *  - PLAYER 2'S HUD. The HUD is the engine's HUD screen reading its racer
 *    (HUD +0xe0) each tick; its pictures are one set of SumoTool objects. So
 *    player 2's is a HUD screen in player 2's copy of the engine -- its own
 *    objects, animations, textures -- reading player 2's racer in player 1's
 *    race: for each of its ticks, player 2's copy's race globals point at
 *    player 1's race (g_pGame, g_pGameData, the racers, the track, the
 *    projectiles), the only ones the HUD reads.
 *  - THE FINISH. A human over the line ends the race (the race type's
 *    update). In split screen the first over it sees their finish (the
 *    replay camera, FINISHED!, their place) and the race goes on for the
 *    other; it ends when both are over.
 *  - WIPES. Falling off the track closes the "curtain" (PauseMenu::PlayWipe)
 *    over the screen; player 2's falls close player 2's copy's, in their
 *    half. The curtains are widened to their half's width (ssr_race.c).
 * MIT.
 */
#include <GLES/gl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "gl_layer.h"
#include "ssr.h"
#include "ssr_split_priv.h"
#include "util.h"

void *ssr_patch_hook(const char *sym, uint32_t expect, void *dst); /* ssr_patch.c, at load */
int ssr_patch_jump(const char *sym, uint32_t expect, void *dst);
int ssr_split_session(void);                                        /* ssr_split.c */
void ssr_split_fit_now(int engine, int w, int h, int mode);

/* the progress bar (the HUD's scene): its group, its track */
#define OBJ_BAR 0x743100f4u
#define OBJ_BAR_TRACK 0x63d779eeu

/* ------------------------------------------------------------ the engines' functions */
typedef void *(*fn_new)(unsigned n);
static struct {
  int ok;
  uint8_t *base; /* the library's load address (return addresses) */
  uint8_t **game, **gamedata, **racers, **track;
  void **stack;
  void *(*by_hash)(const void *stack, uint32_t hash);
  fn_new new_;
  void (*del)(void *p);
  void (*cam_ctor)(void *cam), (*cam_dtor)(void *cam), (*cam_target)(void *cam, void *target);
  void (*cam_update)(void *cam), (*cam_cut)(void *cam), (*orbital_intro)(void *orbital);
  int (*cam_section)(void *cam);
  void (*set_game_camera)(void);
  void (*race_prerender)(void *game, int a, int b); /* STRacing::PreRender: what the camera sees */
  void (*init_display)(uint32_t, uint32_t, uint32_t);
  uint32_t *fb, *cb, *db;
  int32_t *buf_w, *buf_h, *orient;
  void (*init_window)(uint32_t w, uint32_t h);
  void (*bb_size)(uint32_t w, uint32_t h, uint32_t a, uint32_t b);
  void (*app_render)(void);
  uint8_t *pending;
  void (*flush_present)(void);
  uint8_t *sm;
  void (*list_render)(void *list, int queue);
  void (*list_render_ex)(void *list, int queue, const uint32_t *ex, int n);
  void (*list_render_scene)(void *list, uint32_t hash);
  void (*bg_scenes)(void *sm), (*mid_scenes)(void *sm);
  void (*siff_begin)(uint32_t a, int b), (*siff_end)(void);
  void (*gles_viewport)(void), (*gles_baseproj)(void);
  void (*hud_finished)(void *hud, uint32_t a, int place, float delay);
  void (*hud_enable)(void *hud, uint32_t m), (*hud_activate)(void *hud, uint32_t m), (*hud_disable)(void *hud,
                                                                                                    uint32_t m);
  void (*set_time)(void *game, uint32_t frames, int held); /* STRacing::SetTimeRemaining */
  void (*gen_finish_time)(void *racer, uint32_t now);      /* STRacer::GenerateFinishTime */
  void (*set_finished)(void *racer, int a, int b);         /* STRacer::SetFinished */
  int (*show_object)(void *sm, uint32_t hash, int b);      /* SceneManager::ShowObject */
} E1;

static struct {
  int ok;
  uint8_t **game, **gamedata, **racers, **track, **trackmgr, **projmgr;
  fn_new new_;
  void (*hud_ctor)(void *hud), (*hud_onenter)(void *hud), (*hud_update)(void *hud), (*hud_onexit)(void *hud);
  void (*hud_finished)(void *hud, uint32_t a, int place, float delay);
  void (*hud_enable)(void *hud, uint32_t m), (*hud_activate)(void *hud, uint32_t m), (*hud_deactivate)(void *hud,
                                                                                                       uint32_t m);
  void (*hud_disable)(void *hud, uint32_t m);
  void (*update_scenes)(void *sm);
  void (*post_update)(int n);
  uint8_t *sm;
  fn_succ succ;
  void (*list_render_ex)(void *list, int queue, const uint32_t *ex, int n);
  void (*siff_begin)(uint32_t a, int b), (*siff_end)(void);
  void (*bb_size)(uint32_t w, uint32_t h, uint32_t a, uint32_t b);
  void (*gles_viewport)(void), (*gles_baseproj)(void);
  void (*play_wipe)(int a, int b), (*tick_wipe)(void);
  uint8_t **fx;              /* g_pScreenEffects (its HUD's) */
  void (*fx_render)(void *fx);
} E2;

/* the made scenes of a copy that are not the HUD's (a pass's exclusions:
 * SmoToolRenderList::Render(queue, excluded, n) draws the rest, and takes the
 * objects shown since into its list first -- RenderScene does not) */
#define MAXEX 64
static int non_hud_scenes(uint8_t *sm, fn_succ succ, uint32_t *out) {
  int n = 0;
  for (uint8_t *nd = succ ? scene_first(sm) : NULL; nd && n < MAXEX; nd = succ(nd)) {
    const uint32_t h = scene_hash(nd);
    if (h && !is_hud_scene(h))
      out[n++] = h;
  }
  return n;
}

/* player 1's copy's globals the HUD reads, for player 2's copy's HUD */
static uint8_t **g_e1_trackmgr, **g_e1_projmgr;

/* ------------------------------------------------------------ the race */
static struct {
  int active;        /* a two-player race: both humans found, the second camera made */
  int split;         /* the screen split now */
  int presenting;    /* the race presents itself (its flyover, its racers): one picture, no HUD */
  uint8_t *game;     /* the race (g_pGame) set up */
  uint8_t *r1, *r2;  /* the humans: player 1's, player 2's */
  uint8_t *cam1, *cam2;
  int player;        /* the racer being worked on: 0 player 1's (or none), 1 player 2's */
  int in_racer;      /* inside a human racer's update: 1 player 1's, 2 player 2's */
  int pass;          /* render: 0 whole, 1 player 1's half, 2 player 2's, 3 over both */
  int mirror;        /* a camera mode being copied */
  uint8_t *hud2;     /* player 2's HUD (player 2's copy) */
  int hud2_ready;
  int fin[2];        /* each human over the line (seen) */
  int held;          /* ...one only: the race goes on */
  int clock;         /* ...the other on the clock: 1 player 1, 2 player 2 (0: no clock) */
  uint32_t fin_time; /* ...the first's time over the line (frames) */
  uint32_t clock_ticks;
  int wrongway2;
  unsigned passes, hud2_frames;
  u64 split_at;
} R;

int ssr_split_race_active(void) { return R.active; }
int ssr_split_race_split(void) { return R.split; }
int ssr_split_race_player(void) { return R.player; }
int ssr_split_race_pass(void) { return R.pass; }
int ssr_split_race_is_p2(const void *racer) { return R.active && racer && racer == R.r2; }
void *ssr_split_race_cam2(void) { return R.active ? R.cam2 : NULL; }
void *ssr_split_race_cam1(void) { return R.active ? R.cam1 : NULL; }
/* the race's two humans (NULL: no split race) */
uint8_t *ssr_split_race_racer(int k) { return !R.active ? NULL : k ? R.r2 : R.r1; }
/* a count of the split races set up (each race its own) */
static unsigned g_race_gen;
unsigned ssr_split_race_gen(void) { return g_race_gen; }

/* ------------------------------------------------------------ the setup: player 2's racer */
static void (*o_padout)(uint8_t *cgd, int n);

/* STCurrGameData::PadOutWithAI(n) (0x1260e4): `cgd` is g_pGameData +0x6b8:
 * +0x2c the racers, +0x44 their kinds (0 human, 1 AI, 2 network), +0x64
 * their characters; the first n are chosen, the rest get AI of racers not
 * in the race. */
static void padout(uint8_t *cgd, int n) {
  const int p2 = ssr_engine_current() == 0 && ssr_split_session() ? ssr_split_p2_char() : -1;
  int32_t *kind = (int32_t *)(cgd + 0x44), *chr = (int32_t *)(cgd + 0x64);
  const int count = *(const int32_t *)(cgd + 0x2c);
  if (p2 < 0 || n != 1 || count < 2 || count > 8 || chr[0] == p2) {
    o_padout(cgd, n);
    return;
  }
  chr[1] = p2;
  kind[1] = 0;
  o_padout(cgd, 2);
  kind[0] = kind[1] = 0;
  debugPrintf("[split] the race: player 1 racer %d (%s), player 2 racer %d (%s, a human), %d racers\n", (int)chr[0],
              ssr_race_racer_name(0, chr[0]), p2, ssr_race_racer_name(0, p2), count);
}

/* ------------------------------------------------------------ player 2's camera */
static void (*o_setmode)(uint8_t *cam, int mode, int arg);

static int lr_in(const void *lr, uint32_t lo, uint32_t hi) {
  const uint32_t a = (uint32_t)((const uint8_t *)lr - E1.base);
  return a >= lo && a < hi;
}

/* the replay mode's start (as STRacing::SetRaceState(3) does, 0x154a88) */
static void replay_start(uint8_t *cam) {
  uint8_t *rp = *(uint8_t **)(cam + 4);
  if (rp) {
    *(int32_t *)(rp + 0x1c) = -1;
    *(int32_t *)(rp + 0x18) = 0;
    rp[9] = 0;
    *(int32_t *)(rp + 0x10) = 0;
  }
  E1.cam_cut(cam);
}

/* STGameCamera::SetMode(mode, arg): the race's own changes of its camera's
 * mode (from STRacing's update, its race states, its start) are player 2's
 * camera's too -- the ones from a racer are that racer's */
static void setmode(uint8_t *cam, int mode, int arg) {
  const void *lr = __builtin_return_address(0);
  o_setmode(cam, mode, arg);
  if (!R.active || !R.cam2 || cam != R.cam1 || R.mirror || ssr_engine_current() != 0)
    return;
  if (!lr_in(lr, 0x153c60, 0x1544ec) && !lr_in(lr, 0x1549f8, 0x154ac8) && !lr_in(lr, 0x155c98, 0x1564e4))
    return;
  if (R.held && R.fin[1])
    return; /* player 2 is over the line: their finish's camera stays */
  R.mirror = 1;
  o_setmode(R.cam2, mode, arg);
  if (mode == 6)
    E1.orbital_intro(*(void **)(R.cam2 + 0x18));
  else if (mode == 1)
    replay_start(R.cam2);
  R.mirror = 0;
}

/* ------------------------------------------------------------ the racers' updates */
static void (*o_racer_update)(uint8_t *racer);

static void racer_update(uint8_t *racer) {
  if (!R.active || ssr_engine_current() != 0 || (racer != R.r1 && racer != R.r2)) {
    o_racer_update(racer);
    return;
  }
  if (racer == R.r1) {
    R.in_racer = 1;
    o_racer_update(racer);
    R.in_racer = 0;
    return;
  }
  /* player 2's: the race's camera theirs; the WRONG WAY warning (which the
   * racer's update puts on the HUD screen, player 1's) left to player 2's
   * HUD (hud2_tick) */
  uint8_t *game = *E1.game, *gd = *E1.gamedata;
  uint8_t *cam = *(uint8_t **)(game + 0x44);
  const uint8_t warn = gd[0x7bd];
  gd[0x7bd] = 0;
  *(uint8_t **)(game + 0x44) = R.cam2;
  R.player = 1, R.in_racer = 2;
  o_racer_update(racer);
  E1.cam_update(R.cam2);
  R.player = 0, R.in_racer = 0;
  *(uint8_t **)(game + 0x44) = cam;
  gd[0x7bd] = warn;
  R.wrongway2 = warn && *(const int32_t *)(racer + 0xbbc) > 30 && !racer[0xbc1];
}

/* ------------------------------------------------------------ wipes */
static void (*o_playwipe)(int a, int b);
static void (*o_tickwipe)(void);

/* PauseMenu::PlayWipe (the curtain): player 2's racer's in player 2's copy,
 * player 1's in player 1's, the race's own (its start and end) in both */
static void playwipe(int a, int b) {
  if (!R.active || ssr_engine_current() != 0 || !R.hud2_ready) {
    o_playwipe(a, b);
    return;
  }
  if (R.in_racer != 2)
    o_playwipe(a, b);
  if (R.in_racer != 1)
    AS_ENGINE(1, E2.play_wipe(a, b));
}

static void tickwipe(void) {
  if (!R.active || ssr_engine_current() != 0 || !R.hud2_ready) {
    o_tickwipe();
    return;
  }
  if (R.in_racer != 2)
    o_tickwipe();
  if (R.in_racer != 1)
    AS_ENGINE(1, E2.tick_wipe());
}

/* ------------------------------------------------------------ the finish */
static void (*o_gp_update)(uint8_t *rt);

static uint8_t *hud1(void) { return E1.stack && *E1.stack ? E1.by_hash(*E1.stack, SC_HUD) : NULL; }

static int place_of(const uint8_t *racer) {
  int p = *(const int32_t *)(racer + 0xc54);
  return p < 1 ? 1 : p > 6 ? 6 : p;
}

/* player k over the line with the other still racing: their finish, in
 * their half (the replay camera -- the orbit if the track has no replay
 * cameras -- FINISHED! and their place) */
static void finish_view(int k) {
  uint8_t *cam = k ? R.cam2 : R.cam1, *game = *E1.game;
  if (!cam || !game)
    return;
  R.mirror = 1;
  if (*(const int32_t *)(game + 0x25c)) {
    o_setmode(cam, 1, 0);
    replay_start(cam);
  } else {
    o_setmode(cam, 6, 0);
    E1.orbital_intro(*(void **)(cam + 0x18));
  }
  R.mirror = 0;
}

static void finish_message(int k) {
  uint8_t *racer = k ? R.r2 : R.r1;
  if (k == 0) {
    uint8_t *h = hud1();
    if (h)
      E1.hud_finished(h, 0, place_of(racer), 8.0f);
  } else if (R.hud2_ready && ssr_gfx_use(1, 0) == 0) {
    AS_ENGINE(1, E2.hud_finished(R.hud2, 0, place_of(racer), 8.0f));
    ssr_gfx_use(0, 0);
  }
}

/* THE FINISH'S CLOCK. The first human over the line starts the other's
 * clock: the game's own, as its online races have it (STNetRace_GrandPrix::
 * Update: SetTimeRemaining(1800) -- 30 seconds -- and the HUD's countdown,
 * element 0x10, the digits under the progress bar; STRacing::
 * UpdateTimeRemaining counts it down, beeps the last ten seconds, and at 0
 * calls the race type's OutOfTime). Out of time -- or the last racer still
 * going, everyone else over the line -- the other is placed where they are
 * (their time the game's guess from how far round they are, as it guesses
 * the AI racers' still going at the race's end) and the race ends, as Mario
 * Kart's does. */
#define FINISH_CLOCK (30 * 60) /* frames */
static void (*o_gp_outoftime)(uint8_t *rt);

static void clock_start(int k) {
  uint8_t *game = *E1.game;
  if (!game || !E1.set_time)
    return;
  E1.set_time(game, FINISH_CLOCK, 0);
  R.clock = k + 1, R.clock_ticks = 0;
  R.fin_time = *(const uint32_t *)((k ? R.r1 : R.r2) + 0xb9c); /* (milliseconds) */
  debugPrintf("[split] player %d has %d seconds to finish (the game's clock, as its online races)\n", k + 1,
              FINISH_CLOCK / 60);
}

static void clock_stop(void) {
  uint8_t *game = *E1.game;
  if (!R.clock)
    return;
  if (game && E1.set_time)
    E1.set_time(game, 0, 0);
  R.clock = 0;
}

/* the race's time now: the first over the line's time, and the ticks since
 * (60 a second; the game's times are in milliseconds) */
static uint32_t race_ms_now(void) { return R.fin_time + (R.clock_ticks * 50u + 1u) / 3u; }

/* player k, still racing, placed now (why: the log's) */
static void place_now(int k, const char *why) {
  uint8_t *racer = k ? R.r2 : R.r1;
  if (racer[0xbc1] || !E1.gen_finish_time || !E1.set_finished)
    return;
  E1.gen_finish_time(racer, race_ms_now());
  E1.set_finished(racer, 1, 0);
  const uint32_t t = *(const uint32_t *)(racer + 0xb9c);
  debugPrintf("[split] player %d %s: placed %d (a time of %u:%02u.%03u, the game's guess)\n", k + 1, why,
              place_of(racer), t / 60000u, t / 1000u % 60u, t % 1000u);
}

/* STRaceType_GrandPrix::OutOfTime (the race type's vtable +0x20): the race's
 * end calls it too (GP::Update, after the AI racers' times are guessed: it
 * marks them over the line). A split race's clock out mid-race: the one
 * still racing is placed -- not everyone still going, the AI too, with no
 * times (the race's end guesses those) */
static void gp_outoftime(uint8_t *rt) {
  if (R.active && ssr_engine_current() == 0 && R.held) {
    place_now(R.fin[0] ? 1 : 0, "is out of time");
    return;
  }
  o_gp_outoftime(rt);
}

/* STRaceType::GenerateTimesForUnfinishedAIRacers (vtable +0x74; the race's
 * end, with its human's time: the AI racers still going get a time guessed
 * from how far round they are by then): a split race went on after its
 * first over the line -- the guesses from now */
static void (*o_gen_times)(uint8_t *rt, uint32_t now);
static void gen_times(uint8_t *rt, uint32_t now) {
  if (R.active && ssr_engine_current() == 0 && R.fin_time && race_ms_now() > now)
    now = race_ms_now();
  o_gen_times(rt, now);
}

/* STRaceType_GrandPrix::Update (single races too): the race ends at the
 * first human over the line; here at the last (or at the other's clock) */
static void gp_update(uint8_t *rt) {
  if (!R.active || ssr_engine_current() != 0) {
    o_gp_update(rt);
    return;
  }
  uint8_t *game = *E1.game;
  if (R.held && game) { /* the other on the clock: last of everyone still going, placed now */
    R.clock_ticks++;
    const int k = R.fin[0] ? 1 : 0;
    uint8_t *racer = k ? R.r2 : R.r1;
    const int n = *(const int32_t *)(game + 0x764);
    int going = 0;
    for (int i = 0; i < n && i < 8; i++)
      going += E1.racers[i] && E1.racers[i] != racer && !E1.racers[i][0xbc1];
    if (!going)
      place_now(k, "is the last still racing (everyone else is over the line)");
  }
  for (int k = 0; k < 2; k++) {
    const uint8_t *racer = k ? R.r2 : R.r1;
    if (racer[0xbc1] && !R.fin[k]) {
      R.fin[k] = 1;
      const int early = !R.fin[!k];
      debugPrintf("[split] player %d over the line (place %d)%s\n", k + 1, place_of(racer),
                  early ? ": the race goes on for the other" : ": both over: the race ends");
      if (early) {
        finish_view(k);
        clock_start(!k);
      }
      if (early || k == 1)
        finish_message(k); /* the last's, player 1: the race's end shows it */
    }
  }
  R.held = R.fin[0] != R.fin[1];
  if (!R.held) {
    clock_stop();
    o_gp_update(rt);
    return;
  }
  uint8_t *done = R.fin[0] ? R.r1 : R.r2;
  done[0xbc1] = 0; /* not over the line, for the race's end test */
  o_gp_update(rt);
  done[0xbc1] = 1;
}

/* a HUD's countdown (its element 0x10: the clock's digits) shown or not
 * (HUD::EnableElements shows its objects, ActivateElements animates them in
 * and counts; DisableElements animates them out, off both masks) */
#define EL_CLOCK 0x10u
typedef void (*fn_hud_mask)(void *hud, uint32_t m);
static void hud_clock(uint8_t *hud, int on, fn_hud_mask enable, fn_hud_mask activate, fn_hud_mask disable) {
  const uint32_t cc = *(const uint32_t *)(hud + 0xcc), d0 = *(const uint32_t *)(hud + 0xd0);
  if (on && !(d0 & EL_CLOCK)) {
    if (!(cc & EL_CLOCK))
      enable(hud, EL_CLOCK);
    activate(hud, EL_CLOCK);
  } else if (!on && ((cc | d0) & EL_CLOCK)) {
    disable(hud, EL_CLOCK);
  }
}

/* ------------------------------------------------------------ player 2's HUD */
static void (*o_hud_update)(uint8_t *hud);

/* player 2's copy's race globals, pointed at player 1's race (while its HUD
 * works) */
static struct {
  uint8_t *game, *gamedata, *racers[8], *track, *trackmgr, *projmgr;
} g_saved2;

static void redirect(int on) {
  if (on) {
    g_saved2.game = *E2.game, g_saved2.gamedata = *E2.gamedata;
    memcpy(g_saved2.racers, E2.racers, sizeof g_saved2.racers);
    g_saved2.track = *E2.track, g_saved2.trackmgr = *E2.trackmgr, g_saved2.projmgr = *E2.projmgr;
    *E2.game = *E1.game, *E2.gamedata = *E1.gamedata;
    memcpy(E2.racers, E1.racers, sizeof g_saved2.racers);
    *E2.track = *E1.track, *E2.trackmgr = *g_e1_trackmgr, *E2.projmgr = *g_e1_projmgr;
  } else {
    *E2.game = g_saved2.game, *E2.gamedata = g_saved2.gamedata;
    memcpy(E2.racers, g_saved2.racers, sizeof g_saved2.racers);
    *E2.track = g_saved2.track, *E2.trackmgr = g_saved2.trackmgr, *E2.projmgr = g_saved2.projmgr;
  }
}

/* ------------------------------------------------------------ player 2's copy's HUD project */
/* The HUD's scenes come from its SumoTool project (5: hud_notv), which a copy
 * loads when a race of its own begins (GameState::EnterState's list:
 * ProjectLoader::StartLoad, then UpdateLoad each frame, the files through its
 * ResourceManager). Player 2's copy never has a race of its own: it loads the
 * project while it waits at its racer select (its frames still run: its
 * ResourceManager reads the files), and at the latest when its HUD is made. */
enum { HUD_PROJECT = 5 };
static struct {
  int ok, started, done;
  uint8_t **assets; /* ProjectLoader::m_pAssets[48] */
  int32_t *state;   /* ProjectLoader::m_eState: 1 while a load runs */
  void (*start)(const int32_t *list, uint32_t n);
  void (*unload)(const int32_t *list, uint32_t n);
  void (*update)(void);
  int (*finished)(void);
  void *(*get_rm)(void);
  void (*rm_update)(void *rm);
} P2L;

static void p2l_resolve(void) {
  if (P2L.ok)
    return;
#define N2_(s) ssr_native_in(1, s)
  P2L.assets = (uint8_t **)N2_("_ZN13ProjectLoader9m_pAssetsE");
  P2L.state = (int32_t *)N2_("_ZN13ProjectLoader8m_eStateE");
  P2L.start = (void (*)(const int32_t *, uint32_t))N2_("_ZN13ProjectLoader9StartLoadEPKN15ScreenConstants15SumotoolProjectEj");
  P2L.update = (void (*)(void))N2_("_ZN13ProjectLoader10UpdateLoadEv");
  P2L.unload = (void (*)(const int32_t *, uint32_t))N2_("_ZN13ProjectLoader11UnloadFilesEPKN15ScreenConstants15SumotoolProjectEj");
  P2L.finished = (int (*)(void))N2_("_ZN13ProjectLoader14IsLoadFinishedEv");
  P2L.get_rm = (void *(*)(void))N2_("_ZN13SuApplication18GetResourceManagerEv");
  P2L.rm_update = (void (*)(void *))N2_("_ZN15ResourceManager6UpdateEv");
#undef N2_
  P2L.ok = P2L.assets && P2L.state && P2L.start && P2L.unload && P2L.update && P2L.finished && P2L.get_rm && P2L.rm_update;
}

/* the project's file in memory (its asset loaded: state 2 or 3) */
static int hud_project_loaded2(void) {
  const uint8_t *a = P2L.assets[HUD_PROJECT];
  const int32_t st = a ? *(const int32_t *)(a + 0x68) : -1;
  return st == 2 || st == 3;
}

/* inside player 2's copy (AS_ENGINE(1)): a step of the load; `begin` starts
 * it when no load of the copy's own runs */
static void hud_project_step2(int begin) {
  if (!P2L.ok || P2L.done)
    return;
  if (P2L.started && hud_project_loaded2() && *P2L.state == 0) {
    P2L.done = 1;
    debugPrintf("[split] player 2's copy: the HUD's project loaded\n");
    return;
  }
  if (!P2L.started) {
    if (!begin || *P2L.state != 0)
      return;
    static const int32_t list[1] = {HUD_PROJECT};
    if (P2L.assets[HUD_PROJECT]) { /* one left from a race of its own (a battle): used, not to be installed again */
      P2L.unload(list, 1);
      debugPrintf("[split] player 2's copy: its old HUD project dropped\n");
    }
    P2L.start(list, 1);
    P2L.started = 1;
    debugPrintf("[split] player 2's copy: loading the HUD's project (hud_notv)\n");
    return;
  }
  P2L.update(); /* (its FrontendState does not, at the racer select) */
}

/* ssr_split_net.c: player 2's copy raced itself (a battle): its HUD's
 * project loaded again for the next split race */
void ssr_split_race_hud2_reload(void) {
  if (P2L.done || P2L.started)
    debugPrintf("[split] player 2's copy raced: its HUD's project to load again\n");
  P2L.done = P2L.started = 0;
}

/* ssr_split.c, each frame player 2's copy runs (inside it): at its racer select */
void ssr_split_race_preload2(int at_racer_select) {
  p2l_resolve();
  hud_project_step2(at_racer_select);
}

/* at the latest: before player 2's HUD is made (inside player 2's copy) --
 * its files read here, up to 3 s */
static int hud_project_now2(void) {
  p2l_resolve();
  if (!P2L.ok)
    return 0;
  if (P2L.done)
    return 1;
  const u64 t0 = armGetSystemTick();
  hud_project_step2(1);
  void *rm = P2L.get_rm();
  int k = 0;
  while (!P2L.done && P2L.started && armTicksToNs(armGetSystemTick() - t0) < 3000000000ull) {
    if (rm)
      P2L.rm_update(rm);
    hud_project_step2(1);
    if (!P2L.done)
      svcSleepThread(1000000ull);
    k++;
  }
  debugPrintf("[split] player 2's copy: the HUD's project %s (%d steps, %llu ms, at the race's start)\n",
              P2L.done ? "loaded" : "NOT LOADED", k,
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
  return P2L.done;
}

/* the HUD's elements player 2's does not show: the progress bar (one, across
 * the middle: player 1's), the timer, the touch buttons, the warnings of what
 * comes from behind */
#define HUD2_OFF (0x1u | 0x2u | 0x40u | 0x80u | 0x100u | 0x10000u)

static void hud2_make(void) {
  static int told;
  if (!dcr_config()->player2_hud) {
    if (!told++)
      debugPrintf("[split] player 2's HUD: off ([multiplayer] player2_hud)\n");
    return;
  }
  if (!E2.ok || !ssr_split_e2_up())
    return;
  int have = 0;
  if (ssr_gfx_use(1, 0) != 0)
    return;
  AS_ENGINE(1, have = hud_project_now2());
  ssr_gfx_use(0, 0);
  if (!have) {
    if (!told++)
      debugPrintf("[split] player 2's HUD: its project did not load (no HUD for player 2)\n");
    return;
  }
  /* in its own GL context (its texture uploads: its GL state's cache is its
   * context's) */
  ssr_gfx_use(1, 0);
  AS_ENGINE(1, {
    redirect(1);
    R.hud2 = E2.new_(0x1f0);
    if (R.hud2) {
      E2.hud_ctor(R.hud2);
      *(uint8_t **)(R.hud2 + 0xe0) = R.r2;
      *(uint32_t *)(R.hud2 + 0xcc) &= ~HUD2_OFF;
      *(uint32_t *)(R.hud2 + 0xd0) &= ~HUD2_OFF;
      E2.hud_onenter(R.hud2);
    }
    redirect(0);
  });
  ssr_gfx_use(0, 0);
  int ww, wh;
  ssr_gfx_size(&ww, &wh);
  ssr_split_fit_now(1, ww, wh / 2, 1);
  debugPrintf("[split] player 2's HUD: %s\n", R.hud2 ? "made (player 2's copy)" : "NOT MADE");
}

static void hud2_free(void) {
  if (!R.hud2)
    return;
  *(uint8_t **)(R.hud2 + 0xe0) = NULL; /* its racer is gone (~HUD clears the racer's HUD pointer) */
  ssr_gfx_use(1, 0);
  AS_ENGINE(1, {
    redirect(1);
    if (E2.hud_onexit)
      E2.hud_onexit(R.hud2);
    void (*dtor)(void *) = (*(void (***)(void *))R.hud2)[1]; /* the deleting destructor */
    dtor(R.hud2);
    redirect(0);
  });
  ssr_gfx_use(0, 0);
  R.hud2 = NULL, R.hud2_ready = 0;
}

/* each tick of player 1's HUD: player 2's, as its twin */
static void hud2_tick(uint8_t *h1) {
  if (!R.hud2)
    hud2_make();
  if (!R.hud2)
    return;
  int ww, wh;
  ssr_gfx_size(&ww, &wh);
  if (ssr_gfx_use(1, 0) != 0) /* its own GL context (its HUD's textures, uploaded as it decompresses) */
    return;
  AS_ENGINE(1, {
    redirect(1);
    E2.post_update(1); /* its projects decompressing */
    R.hud2[0xd4] = h1[0xd4], R.hud2[0xd5] = h1[0xd5], R.hud2[0xd6] = h1[0xd6];
    *(uint32_t *)(R.hud2 + 0xcc) &= ~HUD2_OFF;
    *(uint32_t *)(R.hud2 + 0xd0) &= ~HUD2_OFF;
    /* the game's HUD and camera are player 2's for the call: the screen
     * effects' racer is the game's HUD's (STScreenEffects::Update), and the
     * rainbow's splats are for the camera's target */
    uint8_t *game = *E1.game;
    uint8_t *was_cam = *(uint8_t **)(game + 0x44), *was_hud = *(uint8_t **)(game + 0x4c);
    *(uint8_t **)(game + 0x44) = R.cam2, *(uint8_t **)(game + 0x4c) = R.hud2;
    E2.hud_update(R.hud2);
    *(uint8_t **)(game + 0x44) = was_cam, *(uint8_t **)(game + 0x4c) = was_hud;
    if (R.wrongway2) {
      E2.hud_enable(R.hud2, 0x8000);
      E2.hud_activate(R.hud2, 0x8000);
    } else if (*(const int32_t *)(R.hud2 + 0x10) == 3) {
      E2.hud_deactivate(R.hud2, 0x8000);
    }
    if (E2.hud_disable) /* their finish's clock (player 1 over the line; enable and activate: E2.ok's) */
      hud_clock(R.hud2, R.clock == 2, E2.hud_enable, E2.hud_activate, E2.hud_disable);
    redirect(0);
    E2.update_scenes(E2.sm);
    /* its scenes (made since, too): their fit for the half */
    ssr_split_fit_now(1, ww, wh / 2, 1);
    ssr_race_hud_in(1, SSR_HUD_BOTTOM, ssr_split_margin());
  });
  ssr_gfx_use(0, 0);
  const int st = *(const int32_t *)(R.hud2 + 0x10);
  { /* (its start, in the log) */
    static int was = -1, n;
    if (st != was && n < 12) {
      was = st, n++;
      int (*created)(void *, uint32_t) = (int (*)(void *, uint32_t))ssr_native_in(1, "_ZN12SceneManager14IsSceneCreatedEj");
      const int32_t *ls = (const int32_t *)ssr_native_in(1, "_ZN13ProjectLoader8m_eStateE");
      const int32_t *nd = (const int32_t *)ssr_native_in(1, "_ZN13ProjectLoader19m_nNumDecompressingE");
      int made = -1;
      if (created)
        AS_ENGINE(1, made = created(E2.sm, SC_HUD));
      debugPrintf("[split] player 2's HUD: state %d; its scene %s; the loader %d, %d decompressing\n", st,
                  made < 0 ? "?" : made ? "made" : "NOT made", ls ? *ls : -1, nd ? *nd : -1);
      char line[600];
      int o = 0;
      line[0] = 0;
      for (uint8_t *nd2 = scene_first(E2.sm); nd2 && o < 560; nd2 = E2.succ(nd2)) {
        const uint8_t *ent = *(uint8_t *const *)(nd2 + 0x1c);
        o += snprintf(line + o, sizeof line - (size_t)o, " %08x%s", ent ? (unsigned)*(const uint32_t *)ent : 0u,
                      nd2[0x2c] ? "" : "(-)");
      }
      debugPrintf("[split]   its scenes:%s\n", line);
    }
  }
  if (!R.hud2_ready && st >= 1 && st != 4) {
    R.hud2_ready = 1;
    debugPrintf("[split] player 2's HUD is up\n");
  } else if (!R.hud2_ready && st == 4) {
    R.hud2_ready = 1; /* made (its scenes), waiting to show */
  }
}

/* The progress bar's track across the middle: shown each tick the HUD is up,
 * as HUD::UpdateProgressBar shows the portraits (SceneManager::ShowObject:
 * its flag, and the render list, which it goes back into if it was taken
 * out) -- and made active again if a scene's hiding left it off under its
 * group, active (CObjectEntry::ActivateObject, what showing the scene does:
 * the render list refuses an object that is off). Nothing else in the game
 * shows it -- only ShowElements, once, when the HUD comes up -- and after a
 * pause (hardware, round 10) the portraits were back but the track was not
 * drawn, its shown flag set. */
static void bar_show(const uint8_t *hud) {
  static uint8_t *(*getobj)(void *, uint32_t);
  if (!getobj)
    getobj = (uint8_t * (*)(void *, uint32_t)) ssr_native_in(0, "_ZN12SceneManager9GetObjectEj");
  uint8_t *game = *E1.game;
  if (!R.split || !game || !getobj || !E1.show_object || game[0x77d] || !hud[0xd4] ||
      *(const int32_t *)(hud + 0x10) != 3 || !(*(const uint32_t *)(hud + 0xd0) & 1))
    return;
  const int st = *(const int32_t *)(game + 0x76c);
  uint8_t *t = getobj(E1.sm, OBJ_BAR_TRACK);
  const uint8_t *g = t ? *(uint8_t *const *)(t + 0xf0) : NULL;
  if (st < 1 || st > 2 || !t)
    return;
  if (t[0xe5] && g && !g[0xe5]) {
    static int told;
    if (told++ < 4)
      debugPrintf("[split] the bar's track: off under its group (active): made active again\n");
    (*(void (***)(void *))t)[0x2c / 4](t); /* ActivateObject */
  }
  E1.show_object(E1.sm, OBJ_BAR_TRACK, 1);
}

static void hud_update(uint8_t *hud) {
  o_hud_update(hud);
  if (R.active && ssr_engine_current() == 0) {
    if (E1.hud_enable && E1.hud_activate && E1.hud_disable) /* their finish's clock (player 2 over the line) */
      hud_clock(hud, R.clock == 1, E1.hud_enable, E1.hud_activate, E1.hud_disable);
    bar_show(hud);
    hud2_tick(hud);
  }
}

/* player 2's HUD over the window's bottom half, by player 2's copy (its
 * context; the viewport the bottom half: GL's lower half) */
static void hud2_render(int w, int h) {
  if (!R.hud2 || !R.hud2_ready || !E2.ok)
    return;
  void (*mode)(GLenum) = (void (*)(GLenum))dcr_gl_lookup("glMatrixMode");
  void (*ident)(void) = (void (*)(void))dcr_gl_lookup("glLoadIdentity");
  if (ssr_gfx_use(1, 0) != 0)
    return;
  AS_ENGINE(1, {
    E2.bb_size((uint32_t)w, (uint32_t)(h / 2), 3, 8);
    E2.gles_viewport();
    if (mode && ident) {
      mode(GL_PROJECTION);
      E2.gles_baseproj();
      mode(GL_MODELVIEW);
      ident();
    }
    uint32_t ex[MAXEX];
    const int nex = non_hud_scenes(E2.sm, E2.succ, ex);
    E2.siff_begin(0, 0);
    if (E2.fx && *E2.fx && E2.fx_render) /* its screen effects (player 2's rainbow, boost lines...) */
      E2.fx_render(*E2.fx);
    void *list = E2.sm + 0x10;
    for (int q = 2; q >= 0; q--) /* the back, the middle, the front */
      E2.list_render_ex(list, q, ex, nex);
    E2.siff_end();
  });
  ssr_gfx_use(0, 0);
  R.hud2_frames++;
}

/* ------------------------------------------------------------ the scenes' transforms, a pass at a time */
/* Each made scene of player 1's copy has two transforms: its half's (the
 * scenes uniform, W x H/2) and the whole screen's (the game's own fit; the
 * HUD's uniform: the progress bar's portraits round), worked out when the
 * scenes change and put in place for each pass -- with the scissor in the
 * design's units the game works out with them (the rainbow's splash covers
 * it: player 1's, with the whole screen's, covered only the middle of their
 * half). */
#define MAXXF 96
static struct {
  uint8_t *node;
  uint32_t hash;
  float xf[2][14];
} g_xf[MAXXF];
static int g_nxf, g_xf_w, g_xf_h;
static fn_succ g_succ1;
/* the 2D's projection with each set (SiffRenderer's glOrthof: the scene
 * manager's size, +0x90 / +0x94 -- its render list's +0x80 / +0x84) */
static float g_ortho[2][2];

static int xf_stale(int w, int h) {
  if (w != g_xf_w || h != g_xf_h)
    return 1;
  int i = 0;
  for (uint8_t *n = scene_first(E1.sm); n; n = g_succ1(n)) {
    const uint32_t hs = scene_hash(n);
    if (!hs)
      continue;
    if (i >= g_nxf || g_xf[i].node != n || g_xf[i].hash != hs)
      return 1;
    i++;
  }
  return i != g_nxf;
}

static void xf_snap(int k) {
  memcpy(g_ortho[k], E1.sm + 0x90, sizeof g_ortho[k]);
  int i = 0;
  for (uint8_t *n = scene_first(E1.sm); n && i < MAXXF; n = g_succ1(n)) {
    const uint32_t hs = scene_hash(n);
    if (!hs)
      continue;
    g_xf[i].node = n, g_xf[i].hash = hs;
    memcpy(g_xf[i].xf[k], n + SCENE_XF, SCENE_XF_LEN);
    i++;
  }
  g_nxf = i;
}

static void xf_build(int w, int h) {
  ssr_split_fit_now(0, w, h / 2, 1);
  xf_snap(0);
  ssr_split_fit_now(0, w, h, 2);
  xf_snap(1);
  g_xf_w = w, g_xf_h = h;
}

static void xf_apply(int k, int w, int h) {
  memcpy(E1.sm + 0x90, g_ortho[k], sizeof g_ortho[k]);
  for (int i = 0; i < g_nxf; i++) {
    float *f = (float *)(g_xf[i].node + SCENE_XF);
    memcpy(f, g_xf[i].xf[k], SCENE_XF_LEN);
    if (k == 1 && g_xf[i].hash == SC_HUD) {
      /* over both halves: only the band across the middle (the progress
       * bar's) */
      const float band = (float)h * 0.07f;
      f[6] = 0, f[7] = (float)h * 0.5f - band, f[8] = (float)w, f[9] = (float)h * 0.5f + band;
      scene_xf_derive(g_xf[i].node);
    }
  }
}

/* ------------------------------------------------------------ the frame, three times */
/* the functions the frame's hooks need (they may be called before
 * ssr_split_race_init) */
static int r1_ready(void) {
  if (E1.list_render_ex)
    return 1;
#define N1(s) ssr_native_in(0, s)
  E1.list_render = (void (*)(void *, int))N1("_ZN17SmoToolRenderList6RenderE8QUEUE_ID");
  E1.init_display = (void (*)(uint32_t, uint32_t, uint32_t))N1("_ZN10SuPlatform11InitDisplayEjjj");
  E1.fb = (uint32_t *)N1("m_frameBuffer");
  E1.cb = (uint32_t *)N1("m_colourBuffer");
  E1.db = (uint32_t *)N1("m_depthBuffer");
  E1.buf_w = (int32_t *)N1("m_bufferWidth");
  E1.buf_h = (int32_t *)N1("m_bufferHeight");
  E1.orient = (int32_t *)N1("_ZN4GLES17ms_eDisplayOrientE");
  E1.init_window = (void (*)(uint32_t, uint32_t))N1("_ZN13SuApplication10InitWindowEjj");
  E1.bb_size = (void (*)(uint32_t, uint32_t, uint32_t, uint32_t))N1("_ZN4GLES17SetBackBufferSizeEjjjj");
  E1.app_render = (void (*)(void))N1("_ZN13SuApplication6RenderEv");
  E1.pending = (uint8_t *)N1("m_bPendingPresent");
  E1.flush_present = (void (*)(void))N1("_Z12flushPresentv");
  E1.list_render_ex = (void (*)(void *, int, const uint32_t *, int))N1("_ZN17SmoToolRenderList6RenderE8QUEUE_IDPji");
#undef N1
  return E1.list_render_ex != NULL;
}

/* SceneManager::RenderScenes (0x19e834): each pass its scenes */
static void render_scenes(uint8_t *sm) {
  if (sm[0xa0] || !r1_ready())
    return;
  void *list = sm + 0x10;
  if (ssr_engine_current() == 0 && R.pass == 0 && R.presenting) {
    /* the race presenting itself, on the whole screen: no HUD (its
     * elements are in their split screen places already) */
    static const uint32_t hud[2] = {SC_HUD, SC_EFFECTS};
    E1.list_render_ex(list, 0, hud, 2);
    return;
  }
  if (ssr_engine_current() != 0 || R.pass == 0) {
    if (!ssr_split_columns_render(0, sm)) /* the racer select, in its column */
      E1.list_render(list, 0);
    return;
  }
  switch (R.pass) {
  case 1: { /* player 1's half: their HUD */
    uint32_t ex[MAXEX];
    E1.list_render_ex(list, 0, ex, non_hud_scenes(E1.sm, g_succ1, ex));
    break;
  }
  case 2: { /* player 2's half: their HUD is player 2's copy's (hud2_render); the
             * countdown is the race's (player 1's copy's) */
    uint32_t ex[MAXEX + 2];
    int n = non_hud_scenes(E1.sm, g_succ1, ex);
    ex[n++] = SC_HUD, ex[n++] = SC_EFFECTS;
    E1.list_render_ex(list, 0, ex, n);
    break;
  }
  case 3: { /* over both: the progress bar (the HUD's scene, its band), then
             * the pause menu, the pop-ups... */
    uint32_t ex[MAXEX + 2];
    int n = non_hud_scenes(E1.sm, g_succ1, ex);
    ex[n++] = SC_EFFECTS, ex[n++] = SC_COUNTDOWN;
    E1.list_render_ex(list, 0, ex, n);
    static const uint32_t hud[3] = {SC_HUD, SC_EFFECTS, SC_COUNTDOWN};
    E1.list_render_ex(list, 0, hud, 3);
    break;
  }
  default:
    E1.list_render(list, 0);
  }
}

/* SceneManager::RenderMidgroundScenes / RenderBGScenes (0x19e804, 0x19e81c):
 * the menus' back layers (the pause menu's, when paused): over both halves
 * only */
static void render_mid(uint8_t *sm) {
  if (r1_ready() && !sm[0xa0] && (ssr_engine_current() != 0 || R.pass == 0 || R.pass == 3))
    E1.list_render_ex(sm + 0x10, 1, NULL, 0);
}
static void render_bg(uint8_t *sm) {
  if (r1_ready() && !sm[0xa0] && (ssr_engine_current() != 0 || R.pass == 0 || R.pass == 3))
    E1.list_render_ex(sm + 0x10, 2, NULL, 0);
}

/* the original render()'s work at a size (0x1f1320): the camera's aspect,
 * the viewport, the frame, the present flag */
static void render_at(int w, int h) {
  E1.init_display(*E1.fb, *E1.cb, *E1.db);
  E1.init_window((uint32_t)w, (uint32_t)h);
  if (*E1.orient > 1)
    E1.bb_size((uint32_t)h, (uint32_t)w, 3, 8);
  else
    E1.bb_size((uint32_t)w, (uint32_t)h, 3, 8);
  E1.app_render();
  *E1.pending = 1;
  E1.flush_present();
}

/* the 2D over both halves, straight into the window (nothing cleared) */
static void overlay_at(int w, int h) {
  void (*mode)(GLenum) = (void (*)(GLenum))dcr_gl_lookup("glMatrixMode");
  void (*ident)(void) = (void (*)(void))dcr_gl_lookup("glLoadIdentity");
  E1.bb_size((uint32_t)w, (uint32_t)h, 3, 8);
  E1.gles_viewport();
  if (mode && ident) {
    mode(GL_PROJECTION);
    E1.gles_baseproj();
    mode(GL_MODELVIEW);
    ident();
  }
  E1.siff_begin(0, 0);
  E1.bg_scenes(E1.sm);
  E1.mid_scenes(E1.sm);
  render_scenes(E1.sm);
  E1.siff_end();
}

/* The progress bar across the middle (the pass over both halves): its group
 * and its track shown while the race runs, the HUD up and the game not
 * paused, whatever a pause left of their shown flags (bar_show puts the
 * track back in the render list each tick). The track's state -- what
 * CTextureObject::Render tests: its and its group's shown flags, its
 * keyframe's "visible", its and its group's alpha; its render list flags and
 * its place -- in the log at each change. */
static struct {
  uint8_t *o[2];
  uint8_t was[2];
  int n;
} g_barvis;
static void bar_visible(int on) {
  static uint8_t *(*getobj)(void *, uint32_t);
  if (!getobj)
    getobj = (uint8_t * (*)(void *, uint32_t)) ssr_native_in(0, "_ZN12SceneManager9GetObjectEj");
  if (!on) {
    for (int i = 0; i < g_barvis.n; i++)
      g_barvis.o[i][0xde] = g_barvis.was[i];
    g_barvis.n = 0;
    return;
  }
  uint8_t *game = *E1.game, *h1 = hud1();
  if (!getobj || !game || !h1)
    return;
  const int st = *(const int32_t *)(game + 0x76c), hst = *(const int32_t *)(h1 + 0x10);
  const int paused = game[0x77d], shown = h1[0xd4], barbit = (*(const uint32_t *)(h1 + 0xd0) & 1) != 0;
  uint8_t *o[2] = {getobj(E1.sm, OBJ_BAR), getobj(E1.sm, OBJ_BAR_TRACK)};
  { /* (the log: each change) */
    static uint32_t last[4] = {~0u};
    static int told;
    const uint8_t *t = o[1], *pa = t ? *(uint8_t *const *)(t + 0xf0) : NULL;
    const float *pl = t ? (const float *)(t + 0x3c) : NULL;
    uint32_t now[4] = {0};
    if (t) {
      now[0] = (uint32_t)t[0xde] | (uint32_t)t[0xdf] << 1 | (uint32_t)t[0xe0] << 2 | (uint32_t)t[0xe4] << 3 |
               (uint32_t)t[0xe5] << 4 | (uint32_t)(*(const uint16_t *)(t + 0xdc) != 0) << 5 |
               (uint32_t)(*(const uint16_t *)(t + 0x4e) != 0) << 6 | (uint32_t)t[0x3b] << 8 | (uint32_t)t[0xf4] << 16;
      now[1] = (uint32_t)(pl[2] * 100.0f) | (uint32_t)(pl[3] * 100.0f) << 16;
    }
    if (pa)
      now[2] = (uint32_t)pa[0xde] | (uint32_t)pa[0x3b] << 8 | (uint32_t)(*(const uint16_t *)(pa + 0x4e) != 0) << 16 |
               (uint32_t)(pa == o[0]) << 17;
    now[3] = (uint32_t)hst | (uint32_t)shown << 4 | (uint32_t)barbit << 5 | (uint32_t)paused << 6 | (uint32_t)st << 8;
    if (memcmp(now, last, sizeof now) && told < 80) {
      memcpy(last, now, sizeof now);
      told++;
      if (t)
        debugPrintf("[split] the bar's track: shown %d, queued %d, listed %d, off %d, visible %d/%d, alpha %d, "
                    "overrides %02x, at %.3f,%.3f scale %.2f,%.2f; its group %s: shown %d, alpha %d, visible %d; "
                    "the HUD's state %d, up %d, bar bit %d; the race's state %d%s\n",
                    t[0xde], t[0xdf], t[0xe0], t[0xe5], *(const uint16_t *)(t + 0xdc), *(const uint16_t *)(t + 0x4e),
                    t[0x3b], t[0xf4], pl[0], pl[1], pl[2], pl[3], pa == o[0] ? "(0x743100f4)" : pa ? "(another)" : "(none)",
                    pa ? pa[0xde] : -1, pa ? pa[0x3b] : -1, pa ? *(const uint16_t *)(pa + 0x4e) : -1, hst, shown,
                    barbit, st, paused ? ", paused" : "");
      else
        debugPrintf("[split] the bar's track: not made\n");
    }
  }
  if (paused || !shown || hst != 3 || st < 1 || st > 2)
    return;
  g_barvis.n = 0;
  for (int i = 0; i < 2; i++)
    if (o[i]) {
      g_barvis.o[g_barvis.n] = o[i], g_barvis.was[g_barvis.n] = o[i][0xde];
      g_barvis.n++;
      o[i][0xde] = 1;
    }
}

/* render() (0x1f1320), re-implemented: one pass, or a split race's three */
static void render_hook(void) {
  r1_ready();
  const int w = *E1.buf_w, h = *E1.buf_h;
  if (ssr_engine_current() != 0 || !R.split || w <= 0 || h <= 0 || ssr_gfx_pass(0) != 0) {
    render_at(w, h);
    return;
  }
  if (xf_stale(w, h))
    xf_build(w, h);
  uint8_t *game = *E1.game;
  /* player 1's half */
  xf_apply(0, w, h);
  R.pass = 1;
  ssr_race_hud_pass(0, 1);
  render_at(w, h / 2);
  /* player 2's: the race's camera, the track's visible section and what
   * the camera sees (the track's culling, the racers' detail: PreRender,
   * no time passing) theirs */
  if (ssr_gfx_pass(1) == 0) {
    uint8_t *track = *E1.track;
    const int32_t section = track ? *(const int32_t *)(track + 0xc4) : 0;
    *(uint8_t **)(game + 0x44) = R.cam2;
    E1.set_game_camera();
    if (track)
      *(int32_t *)(track + 0xc4) = E1.cam_section(R.cam2);
    E1.race_prerender(game, 0, 0);
    R.pass = 2;
    render_at(w, h / 2);
    *(uint8_t **)(game + 0x44) = R.cam1;
    E1.set_game_camera();
    if (track)
      *(int32_t *)(track + 0xc4) = section;
    E1.race_prerender(game, 0, 0);
  }
  /* the two halves in the window, player 2's HUD over theirs, then what is
   * both players' over it all */
  ssr_gfx_pass(-1);
  ssr_gfx_pass_composite();
  hud2_render(w, h);
  xf_apply(1, w, h);
  R.pass = 3;
  ssr_race_hud_pass(0, 3);
  bar_visible(1);
  overlay_at(w, h);
  bar_visible(0);
  R.pass = 0;
  ssr_race_hud_pass(0, 0);
  R.passes++;
}

/* ------------------------------------------------------------ the results' names */
static void (*o_gen_names)(uint8_t *rs);

/* ResultsScreenGP::GenerateNames (0xe53a8): the human racer's row is named
 * after the licence (player 1's); player 2's row after theirs */
static void gen_names(uint8_t *rs) {
  o_gen_names(rs);
  if (ssr_engine_current() != 0 || !ssr_split_session() || !E1.gamedata || !*E1.gamedata)
    return;
  const uint8_t *cgd = *E1.gamedata + 0x6b8;
  const int count = *(const int32_t *)(cgd + 0x2c);
  const int32_t *kind = (const int32_t *)(cgd + 0x44);
  int humans = 0;
  for (int i = 0; i < count && i < 8; i++)
    if (kind[i] == 0 && ++humans == 2) {
      char *dst = (char *)rs + 0x300 + i * 0x51;
      snprintf(dst, 0x50, "%s", ssr_split_p2_name());
      debugPrintf("[split] the results: racer %d is %s\n", i + 1, dst);
      break;
    }
}

/* STScreenEffects::PreSumoToolRender (0x113e44: the rainbow's splats, the
 * boost's lines, the win's stars, drawn before the 2D): player 1's copy's
 * are player 1's -- not in player 2's half (theirs: hud2_render) */
static void (*o_fx_render)(void *fx);
static void fx_render(void *fx) {
  if (ssr_engine_current() == 0 && R.pass == 2)
    return;
  o_fx_render(fx);
}

/* ------------------------------------------------------------ set up */
void ssr_split_race_patch(void) {
  int n = 0;
  n += (o_fx_render = ssr_patch_hook("_ZN15STScreenEffects17PreSumoToolRenderEv", 0xe59f3054u, (void *)fx_render)) !=
       NULL;
  n += (o_padout = ssr_patch_hook("_ZN14STCurrGameData12PadOutWithAIEi", 0xe92d4ff0u, (void *)padout)) != NULL;
  n += (o_setmode = ssr_patch_hook("_ZN12STGameCamera7SetModeENS_10CameraModeEi", 0xe5903020u, (void *)setmode)) !=
       NULL;
  n += (o_playwipe = ssr_patch_hook("_ZN9PauseMenu8PlayWipeEbb", 0xe92d40f0u, (void *)playwipe)) != NULL;
  n += (o_tickwipe = ssr_patch_hook("_ZN9PauseMenu8TickWipeEv", 0xe59f307cu, (void *)tickwipe)) != NULL;
  n += ssr_patch_jump("_Z6renderv", 0xe92d4070u, (void *)render_hook);
  n += ssr_patch_jump("_ZN12SceneManager12RenderScenesEv", 0xe5d010a0u, (void *)render_scenes);
  n += ssr_patch_jump("_ZN12SceneManager21RenderMidgroundScenesEv", 0xe5d030a0u, (void *)render_mid);
  n += ssr_patch_jump("_ZN12SceneManager14RenderBGScenesEv", 0xe5d030a0u, (void *)render_bg);
  n += (o_gen_names = ssr_patch_hook("_ZN15ResultsScreenGP13GenerateNamesEv", 0xe92d4ff0u, (void *)gen_names)) != NULL;
  debugPrintf("[split] the race's hooks: %d of 10\n", n);
}

void ssr_split_race_init(void) {
#define N1(s) ssr_native_in(0, s)
  E1.base = (uint8_t *)N1("_Z6renderv") - 0x1f1320;
  E1.game = (uint8_t **)N1("g_pGame");
  E1.gamedata = (uint8_t **)N1("g_pGameData");
  E1.racers = (uint8_t **)N1("_ZN8STRacing9ms_pRacerE");
  E1.track = (uint8_t **)N1("_ZN8STRacing9ms_pTrackE");
  g_e1_trackmgr = (uint8_t **)N1("g_pTrackManager");
  g_e1_projmgr = (uint8_t **)N1("g_pProjectileManager");
  E1.stack = (void **)N1("g_pScreenStack");
  E1.by_hash = (void *(*)(const void *, uint32_t))N1("_ZNK11ScreenStack15GetScreenByHashEj");
  E1.new_ = (fn_new)N1("_Znwj");
  E1.del = (void (*)(void *))N1("_ZdlPv");
  E1.cam_ctor = (void (*)(void *))N1("_ZN12STGameCameraC1Ev");
  E1.cam_dtor = (void (*)(void *))N1("_ZN12STGameCameraD1Ev");
  E1.cam_target = (void (*)(void *, void *))N1("_ZN12STGameCamera9SetTargetEP8STEntity");
  E1.cam_update = (void (*)(void *))N1("_ZN12STGameCamera6UpdateEv");
  E1.cam_cut = (void (*)(void *))N1("_ZN12STGameCamera9CameraCutEv");
  E1.cam_section = (int (*)(void *))N1("_ZN12STGameCamera12GetSectionIdEv");
  E1.orbital_intro = (void (*)(void *))N1("_ZN19STCameraModeOrbital7DoIntroEv");
  E1.set_game_camera = (void (*)(void))N1("_Z13SetGameCamerav");
  E1.race_prerender = (void (*)(void *, int, int))N1("_ZN8STRacing9PreRenderEii");
  E1.init_display = (void (*)(uint32_t, uint32_t, uint32_t))N1("_ZN10SuPlatform11InitDisplayEjjj");
  E1.fb = (uint32_t *)N1("m_frameBuffer");
  E1.cb = (uint32_t *)N1("m_colourBuffer");
  E1.db = (uint32_t *)N1("m_depthBuffer");
  E1.buf_w = (int32_t *)N1("m_bufferWidth");
  E1.buf_h = (int32_t *)N1("m_bufferHeight");
  E1.orient = (int32_t *)N1("_ZN4GLES17ms_eDisplayOrientE");
  E1.init_window = (void (*)(uint32_t, uint32_t))N1("_ZN13SuApplication10InitWindowEjj");
  E1.bb_size = (void (*)(uint32_t, uint32_t, uint32_t, uint32_t))N1("_ZN4GLES17SetBackBufferSizeEjjjj");
  E1.app_render = (void (*)(void))N1("_ZN13SuApplication6RenderEv");
  E1.pending = (uint8_t *)N1("m_bPendingPresent");
  E1.flush_present = (void (*)(void))N1("_Z12flushPresentv");
  E1.sm = (uint8_t *)N1("g_scene_manager");
  E1.list_render = (void (*)(void *, int))N1("_ZN17SmoToolRenderList6RenderE8QUEUE_ID");
  E1.list_render_ex = (void (*)(void *, int, const uint32_t *, int))N1("_ZN17SmoToolRenderList6RenderE8QUEUE_IDPji");
  E1.list_render_scene = (void (*)(void *, uint32_t))N1("_ZN17SmoToolRenderList11RenderSceneEj");
  E1.bg_scenes = (void (*)(void *))N1("_ZN12SceneManager14RenderBGScenesEv");
  E1.mid_scenes = (void (*)(void *))N1("_ZN12SceneManager21RenderMidgroundScenesEv");
  E1.siff_begin = (void (*)(uint32_t, int))N1("_ZN12SiffRenderer10BeginFrameEjb");
  E1.siff_end = (void (*)(void))N1("_ZN12SiffRenderer8EndFrameEv");
  E1.gles_viewport = (void (*)(void))N1("_ZN4GLES11SetViewportEv");
  E1.gles_baseproj = (void (*)(void))N1("_ZN4GLES23SetBaseProjectionMatrixEv");
  E1.hud_finished = (void (*)(void *, uint32_t, int, float))N1("_ZN3HUD19ShowFinishedMessageEjif");
  E1.hud_enable = (fn_hud_mask)N1("_ZN3HUD14EnableElementsEj");
  E1.hud_activate = (fn_hud_mask)N1("_ZN3HUD16ActivateElementsEj");
  E1.hud_disable = (fn_hud_mask)N1("_ZN3HUD15DisableElementsEj");
  E1.set_time = (void (*)(void *, uint32_t, int))N1("_ZN8STRacing16SetTimeRemainingEmb");
  E1.gen_finish_time = (void (*)(void *, uint32_t))N1("_ZN7STRacer18GenerateFinishTimeEm");
  E1.set_finished = (void (*)(void *, int, int))N1("_ZN7STRacer11SetFinishedEbb");
  E1.show_object = (int (*)(void *, uint32_t, int))N1("_ZN12SceneManager10ShowObjectEjb");
#undef N1
  g_succ1 = (fn_succ)ssr_addr_in(0, 0x19d9c4, 0xe5902008u);
  E1.ok = E1.game && E1.gamedata && E1.racers && E1.track && E1.stack && E1.by_hash && E1.new_ && E1.cam_ctor &&
          E1.cam_target && E1.cam_update && E1.cam_cut && E1.cam_section && E1.orbital_intro && E1.set_game_camera && E1.race_prerender &&
          E1.init_display && E1.fb && E1.cb && E1.db && E1.buf_w && E1.buf_h && E1.orient && E1.init_window &&
          E1.bb_size && E1.app_render && E1.pending && E1.flush_present && E1.sm && E1.list_render &&
          E1.list_render_ex && E1.list_render_scene && E1.bg_scenes && E1.mid_scenes && E1.siff_begin &&
          E1.siff_end && E1.gles_viewport && E1.gles_baseproj && E1.hud_finished && g_succ1 && g_e1_trackmgr &&
          g_e1_projmgr && o_padout && o_setmode;
  /* the vtables' slots: the racers' update, the Grand Prix's (single races'),
   * the HUD's */
  void **vt = (void **)ssr_native_in(0, "_ZTV7STRacer");
  if (vt && vt[4] == ssr_native_in(0, "_ZN7STRacer6UpdateEv")) {
    o_racer_update = (void (*)(uint8_t *))vt[4];
    vt[4] = (void *)racer_update;
  }
  vt = (void **)ssr_native_in(0, "_ZTV20STRaceType_GrandPrix");
  if (vt && vt[7] == ssr_native_in(0, "_ZN20STRaceType_GrandPrix6UpdateEv")) {
    o_gp_update = (void (*)(uint8_t *))vt[7];
    vt[7] = (void *)gp_update;
  }
  if (vt && vt[10] == ssr_native_in(0, "_ZN20STRaceType_GrandPrix9OutOfTimeEv")) {
    o_gp_outoftime = (void (*)(uint8_t *))vt[10];
    vt[10] = (void *)gp_outoftime;
  }
  if (vt && vt[31] == ssr_native_in(0, "_ZN10STRaceType34GenerateTimesForUnfinishedAIRacersEm")) {
    o_gen_times = (void (*)(uint8_t *, uint32_t))vt[31];
    vt[31] = (void *)gen_times;
  }
  vt = (void **)ssr_native_in(0, "_ZTV3HUD");
  if (vt && vt[5] == ssr_native_in(0, "_ZN3HUD6UpdateEv")) {
    o_hud_update = (void (*)(uint8_t *))vt[5];
    vt[5] = (void *)hud_update;
  }
  E1.ok = E1.ok && o_racer_update && o_gp_update && o_hud_update;
  debugPrintf("[split] the race: player 1's copy's %s; racers' updates %s, the race's end %s, the HUD's %s; the "
              "finish's clock %s\n",
              E1.ok ? "functions found" : "functions NOT ALL FOUND (no split races)", o_racer_update ? "hooked" : "NOT",
              o_gp_update ? "hooked" : "NOT", o_hud_update ? "hooked" : "NOT",
              o_gp_outoftime && o_gen_times && E1.set_time && E1.gen_finish_time && E1.set_finished && E1.hud_disable
                  ? "yes"
                  : "NO");
}

/* player 2's copy's, once it is up */
static void e2_resolve(void) {
  if (E2.ok || !ssr_split_e2_up())
    return;
#define N2_(s) ssr_native_in(1, s)
  E2.game = (uint8_t **)N2_("g_pGame");
  E2.gamedata = (uint8_t **)N2_("g_pGameData");
  E2.racers = (uint8_t **)N2_("_ZN8STRacing9ms_pRacerE");
  E2.track = (uint8_t **)N2_("_ZN8STRacing9ms_pTrackE");
  E2.trackmgr = (uint8_t **)N2_("g_pTrackManager");
  E2.projmgr = (uint8_t **)N2_("g_pProjectileManager");
  E2.new_ = (fn_new)N2_("_Znwj");
  E2.hud_ctor = (void (*)(void *))N2_("_ZN3HUDC1Ev");
  E2.hud_onenter = (void (*)(void *))N2_("_ZN3HUD7OnEnterEv");
  E2.hud_update = (void (*)(void *))N2_("_ZN3HUD6UpdateEv");
  E2.hud_onexit = (void (*)(void *))N2_("_ZN3HUD6OnExitEv");
  E2.hud_finished = (void (*)(void *, uint32_t, int, float))N2_("_ZN3HUD19ShowFinishedMessageEjif");
  E2.hud_enable = (void (*)(void *, uint32_t))N2_("_ZN3HUD14EnableElementsEj");
  E2.hud_activate = (void (*)(void *, uint32_t))N2_("_ZN3HUD16ActivateElementsEj");
  E2.hud_deactivate = (void (*)(void *, uint32_t))N2_("_ZN3HUD18DeactivateElementsEj");
  E2.hud_disable = (void (*)(void *, uint32_t))N2_("_ZN3HUD15DisableElementsEj");
  E2.update_scenes = (void (*)(void *))N2_("_ZN12SceneManager12UpdateScenesEv");
  E2.post_update = (void (*)(int))N2_("_ZN13SuApplication10PostUpdateEi");
  E2.sm = (uint8_t *)N2_("g_scene_manager");
  E2.list_render_ex = (void (*)(void *, int, const uint32_t *, int))N2_("_ZN17SmoToolRenderList6RenderE8QUEUE_IDPji");
  E2.succ = (fn_succ)ssr_addr_in(1, 0x19d9c4, 0xe5902008u);
  E2.siff_begin = (void (*)(uint32_t, int))N2_("_ZN12SiffRenderer10BeginFrameEjb");
  E2.siff_end = (void (*)(void))N2_("_ZN12SiffRenderer8EndFrameEv");
  E2.bb_size = (void (*)(uint32_t, uint32_t, uint32_t, uint32_t))N2_("_ZN4GLES17SetBackBufferSizeEjjjj");
  E2.gles_viewport = (void (*)(void))N2_("_ZN4GLES11SetViewportEv");
  E2.gles_baseproj = (void (*)(void))N2_("_ZN4GLES23SetBaseProjectionMatrixEv");
  E2.play_wipe = (void (*)(int, int))N2_("_ZN9PauseMenu8PlayWipeEbb");
  E2.tick_wipe = (void (*)(void))N2_("_ZN9PauseMenu8TickWipeEv");
  E2.fx = (uint8_t **)N2_("g_pScreenEffects");
  E2.fx_render = (void (*)(void *))N2_("_ZN15STScreenEffects17PreSumoToolRenderEv");
#undef N2_
  E2.ok = E2.game && E2.gamedata && E2.racers && E2.track && E2.trackmgr && E2.projmgr && E2.new_ && E2.hud_ctor &&
          E2.hud_onenter && E2.hud_update && E2.hud_finished && E2.hud_enable && E2.hud_activate &&
          E2.hud_deactivate && E2.update_scenes && E2.post_update && E2.sm && E2.list_render_ex && E2.succ && E2.siff_begin &&
          E2.siff_end && E2.bb_size && E2.gles_viewport && E2.gles_baseproj && E2.play_wipe && E2.tick_wipe;
  debugPrintf("[split] player 2's copy's HUD functions: %s\n", E2.ok ? "found" : "NOT ALL FOUND (no HUD for player 2)");
}

/* ------------------------------------------------------------ each frame */
static void race_end(void) {
  if (R.active)
    debugPrintf("[split] the race is over: %u split frames, player 2's HUD drew %u\n", R.passes, R.hud2_frames);
  uint8_t *h = R.active ? hud1() : NULL;
  if (h && E1.hud_disable && (*(const uint32_t *)(h + 0xcc) & EL_CLOCK))
    E1.hud_disable(h, EL_CLOCK); /* the clock's digits: a split race's only */
  hud2_free();
  /* player 2's camera goes with the race (ours: the race never had it) */
  if (R.cam2 && E1.cam_dtor && E1.del) {
    E1.cam_dtor(R.cam2);
    E1.del(R.cam2);
  }
  memset(&R, 0, sizeof R);
  g_nxf = 0, g_xf_w = g_xf_h = 0;
}

/* P1 and P2 under the two players' portraits on the progress bar across the
 * middle -- side by side on the bar, the two apart, in the portraits' order
 * (its portraits: one a racer, in the racers' order --
 * HUD::UpdateProgressBar; placed as the bar's pass draws them: the HUD
 * scene's whole-screen set, the bar group's place and scale, the portrait's
 * in it) */
static void player_tags(void) {
  static const uint32_t k_mark[8] = {0xa077e4ed, 0xbccb5f44, 0xd2696624, 0xf0d460c7,
                                     0xd6cde4b8, 0xf8cdc82c, 0x3ae1e924, 0x616a0bb0};
  static uint8_t *(*getobj)(void *, uint32_t);
  if (!getobj)
    getobj = (uint8_t * (*)(void *, uint32_t)) ssr_native_in(0, "_ZN12SceneManager9GetObjectEj");
  uint8_t *game = *E1.game;
  const float *xf = NULL;
  for (int i = 0; i < g_nxf && !xf; i++)
    if (g_xf[i].hash == SC_HUD)
      xf = g_xf[i].xf[1];
  if (!getobj || !game || !xf)
    return;
  const uint8_t *bar = getobj(E1.sm, 0x743100f4u);
  if (!bar)
    return;
  const float *g = (const float *)(bar + 0x3c); /* x, y, scale x, y */
  const int n = *(const int32_t *)(game + 0x764);
  int h;
  ssr_gfx_size(NULL, &h);
  float tx[2], ty[2];
  int have[2] = {0, 0};
  for (int k = 0; k < 2; k++) {
    const uint8_t *racer = k ? R.r2 : R.r1;
    int idx = -1;
    for (int i = 0; i < n && i < 8; i++)
      if (E1.racers[i] == racer)
        idx = i;
    const uint8_t *o = idx >= 0 ? getobj(E1.sm, k_mark[idx]) : NULL;
    if (!o || !o[0xde])
      continue;
    const float *m = (const float *)(o + 0x3c);
    const float sx = xf[2] * 960.0f, sy = xf[3] * 640.0f;
    const float x0 = xf[4] + sx * (g[0] + g[2] * m[0]), y0 = xf[5] + sy * (g[1] + g[3] * m[1]);
    tx[k] = x0 + sx * g[2] * 0.0335f;
    ty[k] = y0 + sy * g[3] * 0.1f + (float)h * 0.018f;
    have[k] = 1;
  }
  /* close together: the tags apart, a tag's width between their middles,
   * about the pair's middle (the one further left stays to the left) */
  const float sep = (float)h * 0.052f;
  if (have[0] && have[1] && fabsf(tx[0] - tx[1]) < sep) {
    const float mid = (tx[0] + tx[1]) * 0.5f, s = tx[0] <= tx[1] ? -0.5f : 0.5f;
    tx[0] = mid + s * sep, tx[1] = mid - s * sep;
  }
  for (int k = 0; k < 2; k++)
    if (have[k])
      ssr_prompt_card(k ? SSR_CARD_P2 : SSR_CARD_P1, tx[k], ty[k]);
}

void ssr_split_race_pre(void) {
  if (!E1.ok)
    return;
  uint8_t *game = *E1.game;
  if (!ssr_split_session() || !game) {
    if (R.active || R.game) {
      race_end();
      int w, h;
      ssr_gfx_size(&w, &h);
      ssr_split_fit_now(0, w, h, 0);
    }
    return;
  }
  if (game != R.game) {
    if (R.active)
      race_end();
    R.game = game;
  }
  if (!R.active) {
    /* the humans (once the race's racers are made) and its camera */
    const int n = *(const int32_t *)(game + 0x764);
    uint8_t *cam1 = *(uint8_t **)(game + 0x44);
    if (n < 2 || n > 8 || !cam1)
      return;
    uint8_t *hum[2] = {0};
    int k = 0;
    for (int i = 0; i < n && k < 2; i++) {
      uint8_t *r = E1.racers[i];
      if (r && *(const int32_t *)(r + 0x918) == 0)
        hum[k++] = r;
    }
    if (k < 2)
      return; /* one player's race */
    e2_resolve();
    R.r1 = hum[0], R.r2 = hum[1], R.cam1 = cam1;
    R.cam2 = E1.new_(0x8c);
    if (!R.cam2)
      return;
    /* the camera's constructor sets the screen fader (the race's fade-in is
     * on it now): as it was */
    uint8_t **fader = (uint8_t **)ssr_native_in(0, "g_pScreenFader");
    uint8_t fsave[0x20];
    if (fader && *fader)
      memcpy(fsave, *fader, sizeof fsave);
    E1.cam_ctor(R.cam2);
    if (fader && *fader)
      memcpy(*fader, fsave, sizeof fsave);
    E1.cam_target(R.cam2, R.r2);
    R.mirror = 1;
    o_setmode(R.cam2, *(const int32_t *)(cam1 + 0x20), 0);
    R.mirror = 0;
    R.active = 1;
    g_race_gen++;
    debugPrintf("[split] a two-player race: player 1's racer %d, player 2's %d; player 2's camera made\n",
                *(const int32_t *)(R.r1 + 0x920), *(const int32_t *)(R.r2 + 0x920));
  }
  /* the race made again (a restart): its racers and camera anew */
  {
    const int n = *(const int32_t *)(game + 0x764);
    int seen = 0;
    for (int i = 0; i < n && i < 8; i++)
      seen += E1.racers[i] == R.r1 || E1.racers[i] == R.r2;
    if (seen != 2 || *(uint8_t **)(game + 0x44) != R.cam1) {
      debugPrintf("[split] the race's racers changed (a restart): set up again\n");
      race_end();
      R.game = game;
      return;
    }
  }
  /* the screen split from the countdown on (race state 1 with the camera
   * behind its racer: mode 0), while the race's HUD is the screen (not its
   * loading, not the pages after it). Before it the race presents itself to
   * everyone: the course's flyover (mode 1), then the racers one by one
   * (mode 6) -- one picture, the whole screen, as the consoles' and Mario
   * Kart's split screens show them */
  const uint8_t *top = NULL;
  void *(*top_fn)(const void *) = (void *(*)(const void *))ssr_native_in(0, "_ZNK11ScreenStack12GetTopScreenEv");
  if (top_fn && *E1.stack)
    top = top_fn(*E1.stack);
  int (*loading)(void) = (int (*)(void))ssr_native_in(0, "_ZN13LoadingScreen9IsVisibleEv");
  const int state = *(const int32_t *)(game + 0x76c);
  {
    static int was_state = -1, was_mode = -1;
    const int mode = *(const int32_t *)(R.cam1 + 0x20);
    if (state != was_state || mode != was_mode) {
      was_state = state, was_mode = mode;
      debugPrintf("[split] the race's state %d, player 1's camera mode %d, player 2's %d\n", state, mode,
                  R.cam2 ? *(const int32_t *)(R.cam2 + 0x20) : -1);
    }
  }
  const int on_hud = top && *(const uint32_t *)(top + 4) == SC_HUD && !(loading && loading());
  if (R.split)
    player_tags(); /* (this frame's: last frame's places) */
  const int presenting = state <= 1 && *(const int32_t *)(R.cam1 + 0x20) != 0;
  R.presenting = on_hud && presenting;
  const int split = on_hud && state >= 1 && !presenting;
  if (split != R.split) {
    R.split = split;
    debugPrintf("[split] %s\n", split ? "the race: the screen split" : "the race: one screen");
    if (split)
      R.split_at = armGetSystemTick();
    g_xf_w = 0; /* the transforms again */
    if (!split) {
      int w, h;
      ssr_gfx_size(&w, &h);
      ssr_split_fit_now(0, w, h, 0);
    }
  }
}

/* player 2's pad, for their racer (and + pauses the race, as player 1's) */
void ssr_split_race_pad2(const SsrPad *p) {
  if (!R.active)
    return;
  ssr_race_frame_p2(p, R.split);
  if ((p->down & HidNpadButton_Plus) && R.split) {
    uint8_t *game = *E1.game;
    if (game && !game[0x77d])
      ssr_input_back(); /* the pause menu (player 1 has it) */
  }
}

/* a copy's scenes: made, where (the first reports of a race) */
static void scenes_dump(int e, uint8_t *sm, fn_succ succ) {
  for (uint8_t *n = succ ? scene_first(sm) : NULL; n; n = succ(n)) {
    const uint32_t h = scene_hash(n);
    if (!h)
      continue;
    const float *f = (const float *)(n + SCENE_XF);
    debugPrintf("[split]   copy %d scene 0x%08x: design %.0fx%.0f scale %.3f,%.3f at %.1f,%.1f clip %.0f,%.0f-%.0f,%.0f\n",
                e + 1, (unsigned)h, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]);
  }
}

void ssr_split_race_report(void) {
  static int dumps;
  if (R.active)
    debugPrintf("[split] the race: %u frames split, player 2's HUD %u%s\n", R.passes, R.hud2_frames,
                R.held ? "; one player over the line, the race held" : "");
  if (R.active && R.split && dumps < 2) {
    dumps++;
    uint8_t *h1 = hud1();
    uint8_t *game = *E1.game;
    debugPrintf("[split]   the game's HUD %p (player 1's %p, player 2's %p); racers' HUDs %p %p\n",
                game ? *(void **)(game + 0x4c) : NULL, (void *)h1, (void *)R.hud2,
                R.r1 ? *(void **)(R.r1 + 0x6c8) : NULL, R.r2 ? *(void **)(R.r2 + 0x6c8) : NULL);
    if (h1)
      debugPrintf("[split]   player 1's HUD: state %d, masks %08x %08x, shown %d %d %d\n", *(const int32_t *)(h1 + 0x10),
                  *(const uint32_t *)(h1 + 0xcc), *(const uint32_t *)(h1 + 0xd0), h1[0xd4], h1[0xd5], h1[0xd6]);
    if (R.hud2)
      debugPrintf("[split]   player 2's HUD: state %d, masks %08x %08x, shown %d %d %d, ready %d\n",
                  *(const int32_t *)(R.hud2 + 0x10), *(const uint32_t *)(R.hud2 + 0xcc),
                  *(const uint32_t *)(R.hud2 + 0xd0), R.hud2[0xd4], R.hud2[0xd5], R.hud2[0xd6], R.hud2_ready);
    scenes_dump(0, E1.sm, g_succ1);
    if (E2.ok)
      scenes_dump(1, E2.sm, E2.succ);
    void ssr_race_hud_debug(int engine); /* ssr_race.c */
    ssr_race_hud_debug(0);
    AS_ENGINE(1, ssr_race_hud_debug(1));
    for (int i = 0; i < g_nxf; i++)
      if (g_xf[i].hash == SC_HUD)
        for (int k = 0; k < 2; k++)
          debugPrintf("[split]   player 1's HUD scene, %s: scale %.3f,%.3f at %.1f,%.1f\n", k ? "over both" : "their half",
                      g_xf[i].xf[k][2], g_xf[i].xf[k][3], g_xf[i].xf[k][4], g_xf[i].xf[k][5]);
  }
  R.passes = R.hud2_frames = 0;
}
