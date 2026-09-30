/* ssr_split.c -- split screen, the console editions' way: two players on
 * one Switch, in everything the game has for one (MULTIPLAYER.md; the race
 * itself: ssr_split_race.c).
 *
 * THE MENUS. The main menu's MULTIPLAYER card is the consoles' SPLIT SCREEN
 * (its label, and -- from the player's own Xbox 360 disc, if it is on the SD
 * card: ssr_xcard.c -- its picture). It brings up the Switch's controller
 * screen for two players (as the Labyrinth 2 port does), then the game's own
 * single-player menus: GRAND PRIX (its difficulty, then its cup) and SINGLE
 * RACE (its course) -- player 1's, as the consoles' first player picks. The
 * racer select is the game's own, twice, side by side: player 1's on the
 * left, player 2's on the right, each player's own carousel (the front racer
 * and one either side), each A marks its player READY (B takes it back), and
 * once both are the game goes on. A racer is its player's: the other cannot
 * take it.
 *
 * PLAYER 2'S COPY OF THE ENGINE. The picker on the right is a second copy of
 * the engine (the Labyrinth 2 port's two copies: lab_loader.c / lab_versus.c):
 * its own code, data, saves and GL context. It is made the first time split
 * screen is chosen, goes through its title screen by itself, then asks
 * player 2 for their licence (theirs to pick, or a new one, with the Switch's
 * keyboard: the game's own licence screens, in the right-hand column) the
 * first time, and goes on by itself -- SINGLE PLAYER, SINGLE RACE -- to its
 * racer select, where it waits for the next time. Player 2 drives it through
 * the same controller menus as player 1 (ssr_menu2.c). In a race it draws
 * player 2's HUD (ssr_split_race.c): the race is player 1's copy's, with
 * player 2 in it as a second human racer. It never races itself.
 *
 * Leaving for the main menu ends split screen: player 2's copy waits, still.
 * MIT.
 */
#include <GLES/gl.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "dcr_config.h"
#include "gl_layer.h"
#include "ssr.h"
#include "ssr_split_priv.h"
#include "util.h"

const char *dcr_game_root(void);                /* main.c */
void ssr_patch_after_make_model_in(int engine); /* ssr_patch.c */
void ssr_patch_after_init_in(int engine);
void ssr_clock_resync(void);
void ssr_boot_system_dialog(int on); /* ssr_boot.c: a long wait, not a hang */
void ssr_xcard_frame(void *loadset); /* ssr_xcard.c */
int ssr_patch_jump(const char *sym, uint32_t expect, void *dst); /* ssr_patch.c, at load */
int ssr_patch_engine_index(void);

/* ------------------------------------------------------------ the natives */
typedef void (*fn_v)(void *env, void *cls);
typedef void (*fn_z)(void *env, void *cls, jboolean z);
typedef void (*fn_ii)(void *env, void *cls, jint a, jint b);
typedef void (*fn_iiii)(void *env, void *cls, jint a, jint b, jint c, jint d);
typedef jint (*fn_ri)(void *env, void *cls, jint a);
typedef void (*fn_s)(void *env, void *cls, void *s);
typedef void (*fn_ss)(void *env, void *cls, void *a, void *b);
typedef jboolean (*fn_ssii)(void *env, void *cls, void *a, void *b, jint c, jint d);
typedef void (*fn_touch)(void *env, void *cls, jint action, jint x, jint y, jint time);
typedef void (*fn_key)(void *env, void *cls, jint key, jint down);
typedef void (*fn_accel)(void *env, void *cls, jfloat x, jfloat y, jfloat z);

#define NAT(cls, name) "Java_com_sega_ssasr_" cls "_" name
#define ENV g_jni_env

static struct {
  fn_ii cfg;
  fn_s set_device_id;
  fn_v one_time_init;
  fn_z read_cpu_info;
  fn_ss set_make_model;
  fn_v screen_size_init;
  fn_ssii set_file_system;
  fn_iiii set_screen_size;
  fn_ii project_init;
  fn_ri project_run;
  fn_v resume, save_state, set_pause;
  fn_touch touch;
  fn_key key;
  fn_accel accel;
} N2;

/* ------------------------------------------------------------ each copy */
typedef void *(*fn_top)(const void *stack);
typedef int (*fn_curpage)(const void *car, uint8_t *wrapped);
typedef void (*fn_sound)(uint32_t event, uint32_t group, int flag);
typedef int (*fn_bool0)(void);

static const char *const k_vt_names[VT_COUNT] = {
    "TitleScreen",     "SelectLicenseMenu", "MainMenu",   "SoloMenu",            "CharacterSelectScreen",
    "HUD",             "TrackSelectMenu",   "GPSelectMenu", "GPDiffMenu",        "CreateLicenseScreen",
    "KeyboardScreen",  "SelectPortraitScreen", "SelectNationalityScreen", "LicenseScreen", "SegaSDKScreen",
    "MissionSelectMenu", "NetMenu", "SelectServerScreen", "LobbyScreen", "ConnectionLostPopUp",
    "ConnectionFailedPopUp", "JoinFailedPopUp", "WaitingForPlayersPopUp", "ConnectingPopUp", "ResultsScreenGP",
    "SegaMilesScreen", "ChallengeReportScreen", "MultiplayerServiceMenu"};

typedef struct {
  int ok;
  void **stack;
  fn_top top;
  uint8_t **popups;
  fn_bool0 loading;
  uint8_t **game, **gamedata;
  int32_t *buf_w, *buf_h;
  fn_dims set_handset, set_external;
  uint8_t *sm;
  fn_this calc, recalc_all;
  fn_succ succ;
  const volatile uint32_t *ticks, *renders;
  fn_curpage car_page;
  fn_sound sound;
  const uint32_t *sfx;
  const char *(*lic_name)(void *ctl, uint32_t slot); /* STSaveProfileControl::GetName */
  const void *vt[VT_COUNT];
  int w, h, uniform; /* its size now */
  int cols;          /* its scenes placed in its column */
} Eng;
static Eng g_e[2];

static const void *vt_in(int e, const char *cls) {
  char sym[96];
  snprintf(sym, sizeof sym, "_ZTV%d%s", (int)strlen(cls), cls);
  const uint8_t *v = ssr_native_in(e, sym);
  return v ? v + 8 : NULL;
}

static void eng_resolve(int i) {
  Eng *e = &g_e[i];
#define S_(n) ssr_native_in(i, n)
  e->stack = (void **)S_("g_pScreenStack");
  e->top = (fn_top)S_("_ZNK11ScreenStack12GetTopScreenEv");
  e->popups = (uint8_t **)S_("g_pPopUpStack");
  e->loading = (fn_bool0)S_("_ZN13LoadingScreen9IsVisibleEv");
  e->game = (uint8_t **)S_("g_pGame");
  e->gamedata = (uint8_t **)S_("g_pGameData");
  e->buf_w = (int32_t *)S_("m_bufferWidth");
  e->buf_h = (int32_t *)S_("m_bufferHeight");
  e->set_handset = (fn_dims)S_("_ZN13SuApplication20SetHandsetDimensionsEff");
  e->set_external = (fn_dims)S_("_ZN13SuApplication21SetExternalDimensionsEff");
  e->sm = (uint8_t *)S_("g_scene_manager");
  e->calc = (fn_this)S_("_ZN11CSceneEntry27CalculateWidescreenSettingsEv");
  e->recalc_all = (fn_this)S_("_ZN12SceneManager24RecalcWidescreenSettingsEv");
  e->succ = (fn_succ)ssr_addr_in(i, 0x19d9c4, 0xe5902008u); /* the scene tree's next (not exported) */
  e->ticks = (const volatile uint32_t *)S_("_ZN13SuApplication15ms_frameCounterE");
  e->renders = (const volatile uint32_t *)S_("_ZN13SuApplication17ms_uRenderCounterE");
  e->car_page = (fn_curpage)S_("_ZNK8Carousel17GetCurrentPageIdxEPb");
  e->sound = (fn_sound)S_("_ZN16iSAudioInterface10StartEventEjjb");
  e->sfx = (const uint32_t *)S_("STAudio_Group_SoundEffects");
  e->lic_name = (const char *(*)(void *, uint32_t))S_("_ZN20STSaveProfileControl7GetNameEm");
#undef S_
  for (int k = 0; k < VT_COUNT; k++)
    e->vt[k] = vt_in(i, k_vt_names[k]);
  e->ok = e->stack && e->top && e->game && e->gamedata && e->buf_w && e->buf_h && e->set_handset &&
          e->set_external && e->sm && e->calc && e->recalc_all;
  debugPrintf("[split] engine %d: %s (scene walk %s)\n", i + 1, e->ok ? "its globals found" : "SOMETHING MISSING",
              e->succ ? "yes" : "no");
}

static uint8_t *top_of(int i) {
  Eng *e = &g_e[i];
  return e->ok && *e->stack ? e->top(*e->stack) : NULL;
}
uint8_t *ssr_split_top(int i) { return top_of(i & 1); }
static int is_vt(int i, const uint8_t *obj, int k) { return obj && g_e[i].vt[k] && *(void *const *)obj == g_e[i].vt[k]; }
static int loading(int i) {
  Eng *e = &g_e[i];
  int up = 0;
  if (e->ok && e->loading)
    AS_ENGINE(i, up = e->loading());
  return up;
}

int ssr_split_is(int i, const void *obj, int vt) { return vt >= 0 && vt < VT_COUNT && is_vt(i & 1, obj, vt); }
int ssr_split_loading(int i) { return loading(i & 1); }

/* the pop-up that takes input: 1, none 0, one coming or going -1 */
static int popup_state(int i) {
  Eng *e = &g_e[i];
  uint8_t *s = e->popups ? *e->popups : NULL;
  if (!s || !*(const uint32_t *)(s + 0x14))
    return 0;
  if (*(const uint32_t *)s != 3)
    return -1;
  const int cur = *(const int32_t *)(s + 0x18);
  uint8_t *p = cur >= 0 && cur < 3 ? *(uint8_t **)(s + 8 + 4 * cur) : NULL;
  return p && *(const uint32_t *)(p + 4) == 2 ? 1 : -1;
}

int ssr_split_popup(int i) { return popup_state(i & 1); }
uint8_t *ssr_split_popup_obj(int i) {
  Eng *e = &g_e[i & 1];
  if (popup_state(i & 1) <= 0)
    return NULL;
  uint8_t *s = *e->popups;
  const int cur = *(const int32_t *)(s + 0x18);
  return *(uint8_t **)(s + 8 + 4 * cur);
}

static void play(int i, uint32_t ev) {
  Eng *e = &g_e[i];
  if (e->sound && e->sfx)
    AS_ENGINE(i, e->sound(ev, *e->sfx, 0));
}
#define SND_TICK 0xfb699f5au

/* ------------------------------------------------------------ the state */
static struct {
  int w, h;          /* the window */
  int session;       /* split screen chosen (until the main menu again) */
  int e2;            /* player 2's copy: 0 not made, 1 up, -1 failed */
  int run2;          /* ...runs its frames this frame */
  int cols;          /* the racer select in two columns */
  int main_page;     /* the main menu's front card (while it takes input) */
  int was_main;      /* the main menu was the screen */
  int ready[2];      /* each player's racer chosen (READY) */
  int pick[2];       /* ...which (STCharacter) */
  int go;            /* both ready: player 1's copy picks its racer now */
  int lic_done;      /* player 2 has chosen their licence (this session of the game) */
  unsigned frames2;
  SsrPad p2;
  int p2_there;
  const void *seen2;
  int seen2_state;
  u64 pulse_at;
} S = {.pick = {-1, -1}};

int ssr_split_session(void) { return S.session; }
/* split screen's mode select is player 1's copy's screen (its words: its
 * footer, its cards; the main menu's SINGLE PLAYER card shares the footer's
 * word, so never while the main menu is up) */
int ssr_split_mode_select(void) { return S.session && is_vt(0, top_of(0), VT_SOLO); }
int ssr_split_columns(void) { return S.cols; }
int ssr_split_e2_up(void) { return S.e2 > 0; }
int ssr_split_p2_char(void) { return S.ready[1] || S.pick[1] >= 0 ? S.pick[1] : -1; }
int ssr_split_running(void) { return S.run2; }
int ssr_split_race(void) { return ssr_split_race_split(); }

/* player 2's licence's name, for the results */
const char *ssr_split_p2_name(void) {
  Eng *e = &g_e[1];
  uint8_t *gd = S.e2 > 0 && e->gamedata ? *e->gamedata : NULL;
  const char *n = NULL;
  if (gd && e->lic_name)
    AS_ENGINE(1, n = e->lic_name(gd + 4, gd[0x7ef]));
  return n && n[0] ? n : "PLAYER 2";
}

void ssr_split_wh(int *w, int *h) { *w = S.w, *h = S.h; }

void ssr_split_view(int engine, int *w, int *h) {
  (void)engine;
  ssr_gfx_size(w, h);
}

/* ------------------------------------------------------------ sizes */
/* A copy's size (the phone's window size, as nativeSetScreenSize gave it):
 * render() reads m_bufferWidth / Height each frame (the viewport, the
 * camera's aspect, the touches' scale); the SumoTool scenes take theirs from
 * the handset size when made, or when recalculated (MULTIPLAYER.md 5.2). */
static void set_size(Eng *e, int w, int h) {
  *e->buf_w = w, *e->buf_h = h;
  float fw = (float)w, fh = (float)h;
  uint32_t bw, bh;
  memcpy(&bw, &fw, 4), memcpy(&bh, &fh, 4);
  e->set_handset(bw, bh);
  e->set_external(bw, bh);
}

/* Every made scene's transform again; uniform: the stretched fits (modes
 * 1-4: the menus', the HUD's) worked out as mode 5 -- uniform, the height
 * fitted, centred -- for the call only (the project's own value stays), and
 * every scene but the HUD's clipped to its design area: the games park what
 * slides in beside the phone's screen (MULTIPLAYER.md 5.4). */
static void scenes_fit(Eng *e, int uniform) {
  if (!uniform || !e->succ) {
    e->recalc_all(e->sm);
    return;
  }
  for (uint8_t *n = scene_first(e->sm); n; n = e->succ(n)) {
    if (!n[0x2c])
      continue;
    uint8_t *ent = *(uint8_t **)(n + 0x1c);
    uint32_t *mode = ent ? (uint32_t *)(ent + 8) : NULL;
    const uint32_t was = mode ? *mode : 0;
    if (mode && was >= 1 && was <= 4)
      *mode = 5;
    e->calc(n);
    if (mode)
      *mode = was;
    if (ent && !is_hud_scene(*(const uint32_t *)ent)) {
      float *f = (float *)(n + SCENE_XF);
      f[6] = f[4], f[7] = f[5];
      f[8] = f[4] + f[0] * f[2], f[9] = f[5] + f[1] * f[3];
      scene_xf_derive(n);
    }
  }
}

static void eng_size(int i, int w, int h, int uniform) {
  Eng *e = &g_e[i];
  if (!e->ok || (e->w == w && e->h == h && e->uniform == uniform))
    return;
  e->w = w, e->h = h, e->uniform = uniform;
  AS_ENGINE(i, {
    set_size(e, w, h);
    scenes_fit(e, uniform);
  });
  e->cols = 0;
  debugPrintf("[split] engine %d: %dx%d%s\n", i + 1, w, h, uniform ? " (the scenes uniform)" : "");
}
void ssr_split_e1_size(int w, int h, int uniform) { eng_size(0, w, h, uniform); }

/* a copy's scenes fitted for a size now (the race's passes: ssr_split_race.c):
 * mode 0 the game's own fits; 1 a half's (uniform, all but the HUD's clipped
 * to their design); 2 the whole screen's with the HUD's scenes uniform (the
 * progress bar across the middle) */
void ssr_split_fit_now(int i, int w, int h, int mode) {
  Eng *e = &g_e[i];
  if (!e->ok)
    return;
  AS_ENGINE(i, {
    set_size(e, w, h);
    if (mode == 1 || !e->succ) {
      scenes_fit(e, mode == 1);
    } else {
      e->recalc_all(e->sm);
      if (mode == 2)
        for (uint8_t *n = scene_first(e->sm); n; n = e->succ(n)) {
          uint8_t *ent = n[0x2c] ? *(uint8_t **)(n + 0x1c) : NULL;
          uint32_t *m = ent ? (uint32_t *)(ent + 8) : NULL;
          if (!m || !is_hud_scene(*(const uint32_t *)ent) || *m < 1 || *m > 4)
            continue;
          const uint32_t was = *m;
          *m = 5;
          e->calc(n);
          *m = was;
        }
    }
  });
  e->w = w, e->h = h, e->uniform = mode == 1;
  e->cols = 0;
}
void ssr_split_e2_size(int w, int h, int uniform) { eng_size(1, w, h, uniform); }

/* ------------------------------------------------------------ the columns */
/* The racer select, each copy in its column (player 1's the left half,
 * player 2's the right): each made scene uniformly scaled into the column
 * and clipped to it. The racer select's own (0x9b0badaf: the portraits, the
 * name, the stats) at a scale that shows the front racer and one either side
 * of it; the others (the footer with the racer's name, the backdrop's) the
 * whole design width, at the column's foot. Its racer on its motorbike (3D)
 * moves to the column's middle: set_perspective below. */
#define SC_CHARSEL 0x9b0badafu
#define SC_BACKDROP 0x7bcadbedu /* the menus' sky (and their footer: OBJ_FOOTER) */
#define OBJ_TITLE 0xa7be22bbu   /* the footer's title (GlobalScreen::SetFooterText), in that scene */
#define CHARSEL_SPAN 0.66f         /* the design width a column shows of the portraits (x 0.17..0.83) */
#define OBJ_RACER_NAME 0x7e1795fdu /* the racer select's name (CharacterSelectScreen::Event_HighlightChanged) */
static float g_colshift[2]; /* the 3D picture's shift, in NDC x (set_perspective) */

/* SuApplication::ms_clearColour (RGBA, red the low byte: BeginFrameRender
 * clears to it each frame): in a column, the menus' sky where the phone's
 * picture does not reach -- its top edge's blue, which the sky goes on from
 * (the game's own white otherwise shows) */
#define SKY_CLEAR 0xffee5a00u
static void column_clear(int i, int on) {
  static uint32_t *clr[2];
  static uint32_t was[2];
  static int set[2];
  if (!clr[i])
    clr[i] = (uint32_t *)ssr_native_in(i, "_ZN13SuApplication14ms_clearColourE");
  if (!clr[i])
    return;
  if (on && !set[i])
    was[i] = *clr[i];
  else if (!on && set[i])
    *clr[i] = was[i];
  if (on) {
    static int told;
    if (*clr[i] != SKY_CLEAR && told++ < 6)
      debugPrintf("[split] engine %d's clear colour: %08x\n", i + 1, (unsigned)*clr[i]);
    *clr[i] = SKY_CLEAR; /* each frame: the game sets its own */
  }
  set[i] = on;
}

/* An object's place in a column: its keyframes (the frames of its slides:
 * +0x28 each, x, y, scale x, y from +0xc, of the design) mapped x' = A x + Cx,
 * y' = A y + Cy, scales x A -- for a group, every child's place then maps
 * the same (world = g.xy + pivot + g.s (c - pivot)); given back when the
 * columns go. Its live place (+0x3c) too, when it is one of the frames' other
 * values: a slide begun before they moved keeps its own copy. */
enum { OP_TITLE, OP_STATS, OP_COUNT };
static void obj_place(int i, int slot, uint32_t hash, int on, float A, float Cx, float Cy) {
  static uint8_t *(*getobj[2])(void *, uint32_t);
  static struct {
    uint8_t *frames;
    unsigned n;
    float v[16][4];
  } g[2][OP_COUNT];
  Eng *e = &g_e[i];
  if (!getobj[i])
    getobj[i] = (uint8_t * (*)(void *, uint32_t)) ssr_native_in(i, "_ZN12SceneManager9GetObjectEj");
  uint8_t *o = NULL;
  if (getobj[i])
    AS_ENGINE(i, o = getobj[i](e->sm, hash));
  uint8_t *kfe = o ? *(uint8_t **)(o + 0xec) : NULL;
  uint8_t *fr = kfe ? *(uint8_t **)(kfe + 4) : NULL;
  if (!fr) {
    g[i][slot].frames = NULL;
    return;
  }
  unsigned n = *(const uint16_t *)(kfe + 8);
  n = n > 16 ? 16 : n;
  if (g[i][slot].frames != fr || g[i][slot].n != n) { /* its own values (the scene made again) */
    g[i][slot].frames = fr, g[i][slot].n = n;
    for (unsigned k = 0; k < n; k++)
      memcpy(g[i][slot].v[k], fr + k * 0x28 + 0xc, 4 * sizeof(float));
  }
  float *live = (float *)(o + 0x3c);
  int live_done = 0;
  for (unsigned k = 0; k < n; k++) {
    const float *v = g[i][slot].v[k];
    const float m[4] = {Cx + v[0] * A, Cy + v[1] * A, v[2] * A, v[3] * A};
    float *f = (float *)(fr + k * 0x28 + 0xc);
    memcpy(f, on ? m : v, sizeof m);
    const float *from = on ? v : m, *to = on ? m : v;
    if (!live_done && fabsf(live[0] - from[0]) < 1e-4f && fabsf(live[1] - from[1]) < 1e-4f &&
        fabsf(live[2] - from[2]) < 1e-4f && fabsf(live[3] - from[3]) < 1e-4f) {
      memcpy(live, to, 4 * sizeof(float));
      live_done = 1;
    }
  }
}

/* The menus' sky fills a column (its scene, SC_BACKDROP, fitted to the
 * column's height); the screen's title, in that scene, as the column's other
 * scenes are (its width, at its foot). */
static void title_place(int i, int on) {
  const float W = (float)S.w, H = (float)S.h, cw = W * 0.5f, cx0 = i ? cw : 0.0f;
  const float kc = H / 640.0f, oxc = cx0 + cw * 0.5f - 480.0f * kc; /* the sky's */
  const float kb = cw / 960.0f, oxb = cx0, oyb = H - 640.0f * kb;   /* the column's */
  obj_place(i, OP_TITLE, OBJ_TITLE, on, kb / kc, (oxb - oxc) / kc / 960.0f, oyb / kc / 640.0f);
}

/* The racer select's stats box (the group 0xbac81b34: its panel, its four
 * bars, its close button -- design x 0.515..0.975, y 0.416..0.825, pivot
 * 0.729, 0.656: char_sel.star) smaller in a column, its right edge inside
 * what the column shows of the design (x 0.17..0.83), its middle as high. */
#define STATS_GROUP 0xbac81b34u
#define STATS_A 0.78f
static void stats_place(int i, int on) {
  const float bx = 0.81f - STATS_A * 0.975f, by = 0.62f * (1.0f - STATS_A);
  obj_place(i, OP_STATS, STATS_GROUP, on, STATS_A, (STATS_A - 1.0f) * 0.72917f + bx, (STATS_A - 1.0f) * 0.65625f + by);
}

static void place_columns(int i, int on) {
  Eng *e = &g_e[i];
  if (!e->ok || !e->succ)
    return;
  column_clear(i, on);
  if (on || e->cols) {
    title_place(i, on);
    stats_place(i, on);
  }
  const float shift = on ? (i ? 0.5f : -0.5f) : 0.0f;
  if (on || g_colshift[i] != shift) { /* the camera's projection made again (with the shift) */
    uint8_t *(*cur_cam)(void) = (uint8_t * (*)(void)) ssr_native_in(i, "_ZN12AppFunctions16GetCurrentCameraEv");
    uint8_t *cam = NULL;
    if (cur_cam)
      AS_ENGINE(i, cam = cur_cam());
    if (cam)
      cam[0xc4] = 1;
  }
  g_colshift[i] = shift;
  if (!on) {
    if (e->cols)
      AS_ENGINE(i, scenes_fit(e, 0));
    e->cols = 0;
    return;
  }
  const float W = (float)S.w, H = (float)S.h, cw = W * 0.5f, cx0 = i ? cw : 0.0f;
  static int logged[2];
  AS_ENGINE(i, {
    for (uint8_t *n = scene_first(e->sm); n; n = e->succ(n)) {
      const uint32_t h = scene_hash(n);
      if (!h)
        continue;
      e->calc(n); /* fresh (the full screen's), then placed */
      float *f = (float *)(n + SCENE_XF + 8); /* scale x, y; offset x, y; the scissor x0, y0, x1, y1 */
      float k, ox, oy;
      if (h == SC_CHARSEL) {
        /* the portraits at about the phone's size (in parts: the front one
         * and one either side, the name centred: ssr_split_columns_pre) */
        k = cw / (960.0f * CHARSEL_SPAN);
        ox = cx0 + cw * 0.5f - 480.0f * k;
        oy = (H - 640.0f * k) * 0.5f;
      } else if (h == SC_BACKDROP) {
        /* the menus' sky: the whole column (its middle, the height's fit) */
        k = H / 640.0f;
        ox = cx0 + cw * 0.5f - 480.0f * k;
        oy = 0.0f;
      } else {
        k = cw / 960.0f;
        ox = cx0;
        oy = H - 640.0f * k;
      }
      f[0] = f[1] = k;
      f[2] = ox, f[3] = oy;
      /* clipped to the column, and to the phone's picture: what the game
       * parks above it (the main menu's banner, until it slides in) stays
       * out of sight */
      f[4] = cx0, f[5] = h == SC_CHARSEL ? 0.0f : fmaxf(0.0f, oy), f[6] = cx0 + cw, f[7] = H;
      scene_xf_derive(n);
      if (!logged[i] || (!e->cols && logged[i] < 4))
        debugPrintf("[split] engine %d, in its column: scene 0x%08x\n", i + 1, (unsigned)h);
    }
  });
  if (!e->cols)
    logged[i]++;
  e->cols = 1;
}

/* The racer select in a column, at about the phone's size, in three parts
 * (the scene drawn again for each, placed and clipped for it):
 *   the portraits: the front one and one either side (the consoles' split
 *     screen) -- the row clipped to a window around the front one;
 *   below them (the ribbon, the rest): the whole column;
 *   the racer's name: centred in the column (the phone has it at the foot,
 *     at the left, where a column would cut it off).
 * In design pixels (960 x 640): the row above CS_ROW_END, the name's row
 * CS_NAME_Y0..Y1, its text from CS_NAME_X (left-aligned). Inside copy i's
 * SceneManager::RenderScenes: _pre before its scenes, _scenes after. */
#define CS_ROW_END 211.0f
#define CS_ROW_HALF 262.0f
#define CS_NAME_X 42.0f
#define CS_NAME_Y0 574.0f
#define CS_NAME_Y1 640.0f
static uint8_t *charsel_node(int i) {
  Eng *e = &g_e[i];
  for (uint8_t *n = scene_first(e->sm); n; n = e->succ(n))
    if (scene_hash(n) == SC_CHARSEL)
      return n;
  return NULL;
}

/* 1 if copy i's scenes were drawn here (the racer select in its column) --
 * the others as they are, the racer select's scene after them, in its three
 * parts, each in a clip of its own */
int ssr_split_columns_render(int i, uint8_t *sm) {
  Eng *e = &g_e[i];
  if (!S.cols || !e->cols || !e->succ)
    return 0;
  static void (*render_ex[2])(void *list, int queue, const uint32_t *ex, int n);
  static void (*render_scene[2])(void *list, uint32_t hash);
  static int (*get_w[2])(void *sm, uint32_t obj);
  static void (*flush[2])(void), (*open_[2])(void); /* SiffRenderer::ManagedVBuffer: its batched 2D */
  if (!render_ex[i]) {
    render_ex[i] = (void (*)(void *, int, const uint32_t *, int))ssr_native_in(i, "_ZN17SmoToolRenderList6RenderE8QUEUE_IDPji");
    render_scene[i] = (void (*)(void *, uint32_t))ssr_native_in(i, "_ZN17SmoToolRenderList11RenderSceneEj");
    get_w[i] = (int (*)(void *, uint32_t))ssr_native_in(i, "_ZN12SceneManager19GetStringPixelWidthEj");
    flush[i] = (void (*)(void))ssr_native_in(i, "_ZN12SiffRenderer14ManagedVBuffer5FlushEv");
    open_[i] = (void (*)(void))ssr_native_in(i, "_ZN12SiffRenderer14ManagedVBuffer4OpenEv");
  }
  if (!render_ex[i] || !render_scene[i] || !flush[i] || !open_[i])
    return 0;
  void *list = sm + 0x10;
  uint8_t *node = S.cols ? charsel_node(i) : NULL;
  if (!node)
    return 0;
  static const uint32_t ex[1] = {SC_CHARSEL};
  render_ex[i](list, 0, ex, 1);
  /* the 2D is batched (drawn at the frame's end): each part drawn now, in
   * its clip */
  flush[i]();
  open_[i]();

  float *f = (float *)(node + SCENE_XF + 8), s[8]; /* scale x, y; offset x, y; ... */
  memcpy(s, f, sizeof s);
  const float W = (float)S.w, H = (float)S.h, cw = W * 0.5f, c0 = i ? cw : 0.0f, c1 = c0 + cw;
  const float k = s[0], oy = s[3], cx = s[2] + 480.0f * k;
  const float row = oy + CS_ROW_END * k, n0 = oy + CS_NAME_Y0 * k, n1 = fminf(H, oy + CS_NAME_Y1 * k);
  /* each part in the scene's own clip (its batches' scissor, worked out as
   * they are drawn: SetFullscreenScissorRegion / SetScissorRegion), drawn
   * now while it is set */
#define PART(x0, y0, x1, y1)                                                                                           \
  (f[4] = (x0), f[5] = (y0), f[6] = (x1), f[7] = (y1), render_scene[i](list, SC_CHARSEL), flush[i](), open_[i]())
  /* the portraits: the front one and one either side */
  PART(fmaxf(c0, cx - CS_ROW_HALF * k), 0.0f, fminf(c1, cx + CS_ROW_HALF * k), row);
  /* below them, but for the name's row */
  PART(c0, row, c1, n0);
  if (H > n1)
    PART(c0, n1, c1, H);
  /* the name's row, the name centred */
  const float tw = get_w[i] ? (float)get_w[i](sm, OBJ_RACER_NAME) : 0.0f;
  if (tw > 0.0f) {
    f[2] = (c0 + c1) * 0.5f - (CS_NAME_X + tw * 0.5f) * k;
    PART(c0, n0, c1, n1);
  }
#undef PART
  memcpy(f, s, sizeof s);
  return 1;
}

void ssr_split_place_columns(int i, int on) { place_columns(i & 1, on); }

/* SuMatrix44::SetPerspective(fov, aspect, near, far), the engine's (a GL
 * projection: 0x18fb90), rewritten: the racer select's two columns shift
 * each copy's 3D picture (its racer on the motorbike) to its column's middle.
 * A split race's halves are twice as wide: the camera keeps its vertical view
 * and sees wider across, as the console editions' split screen does. */
static void set_perspective(float *m, float fov, float aspect, float n, float f) {
  const float m5 = 1.0f / tanf(fov * 0.5f);
  memset(m, 0, 16 * sizeof(float));
  m[0] = m5 / aspect;
  m[5] = m5;
  m[8] = -g_colshift[ssr_engine_current() & 1];
  m[10] = (f + n) / (n - f);
  m[11] = -1.0f;
  m[14] = (f + f) * n / (n - f);
}

/* player 2's copy's SceneManager::RenderScenes (0x19e834), as it was, then
 * its racer's name in its column (player 1's: ssr_split_race.c's) */
static void render_scenes2(uint8_t *sm) {
  static void (*list_render)(void *list, int queue);
  if (!list_render)
    list_render = (void (*)(void *, int))ssr_native_in(1, "_ZN17SmoToolRenderList6RenderE8QUEUE_ID");
  if (sm[0xa0] || !list_render)
    return;
  if (!ssr_split_columns_render(1, sm))
    list_render(sm + 0x10, 0);
}

void ssr_split_patch(void) {
  if (!dcr_config()->split_screen)
    return;
  if (ssr_patch_engine_index() == 1 && !ssr_patch_jump("_ZN12SceneManager12RenderScenesEv", 0xe5d010a0u, (void *)render_scenes2))
    debugPrintf("[split] player 2's copy's scenes: its own drawing (no name under its racer)\n");
  if (!ssr_patch_jump("_ZN10SuMatrix4414SetPerspectiveEffff", 0xe92d41f0u, (void *)set_perspective))
    debugPrintf("[split] the camera's projection: the engine's own (no column shift)\n");
  void ssr_split_audio_patch(void); /* ssr_split_audio.c: both players heard */
  ssr_split_audio_patch();
  if (ssr_patch_engine_index() == 0) {
    void ssr_xcard_patch(void); /* ssr_xcard.c */
    ssr_split_race_patch();
    ssr_xcard_patch();
  }
}

/* ------------------------------------------------------------ engine 2's saves */
/* Its data directory is /data/data/<package>/p2 (<game>/data/p2): the first
 * time, a copy of player 1's saves, so that it has what player 1 has
 * unlocked (the licences with it; player 2 picks theirs, or makes one). */
static void copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in)
    return;
  FILE *out = fopen(to, "wb");
  if (out) {
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
      fwrite(buf, 1, n, out);
    fclose(out);
  }
  fclose(in);
}

static void seed_saves(void) {
  char src[512], dst[512], a[800], b[800];
  snprintf(src, sizeof src, "%s/data/files", dcr_game_root());
  snprintf(dst, sizeof dst, "%s/data/p2", dcr_game_root());
  mkdir(dst, 0777);
  snprintf(dst, sizeof dst, "%s/data/p2/files", dcr_game_root());
  mkdir(dst, 0777);
  snprintf(a, sizeof a, "%s/port_seeded", dst);
  struct stat st;
  if (stat(a, &st) == 0)
    return; /* it has its own */
  FILE *mk = fopen(a, "wb");
  if (mk) {
    fputs("player 2's saves began as a copy of player 1's\n", mk);
    fclose(mk);
  }
  DIR *d = opendir(src);
  if (!d)
    return;
  int n = 0;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.')
      continue;
    snprintf(a, sizeof a, "%s/%s", src, de->d_name);
    snprintf(b, sizeof b, "%s/%s", dst, de->d_name);
    if (stat(a, &st) == 0 && S_ISREG(st.st_mode))
      copy_file(a, b), n++;
  }
  closedir(d);
  /* the file shims answer "is it there?" from directory listings made
   * earlier (dcr_dircache.c): these files were written behind their back */
  void dcr_dircache_forget(void);
  dcr_dircache_forget();
  debugPrintf("[split] player 2's saves: %d of player 1's files copied to data/p2/files\n", n);
}

/* ------------------------------------------------------------ engine 2's start */
static void *need2(const char *sym) {
  void *p = ssr_native_in(1, sym);
  if (!p)
    debugPrintf("[split] MISSING native %s\n", sym);
  return p;
}

/* What DemoActivity and its renderer did (ssr_boot.c's order), for the
 * second copy, which runs on this thread marked as the second (its Java
 * calls tell the copies apart by it). */
static int boot2(void) {
  const u64 t0 = armGetSystemTick();
  seed_saves();
  if (ssr_load_second_module() != 0) {
    debugPrintf("[split] the second copy of the engine could not be loaded\n");
    return -1;
  }
  N2.cfg = (fn_ii)need2(NAT("DemoActivity", "nativeProjectCfg"));
  N2.set_device_id = (fn_s)need2(NAT("DemoActivity", "nativeSetDeviceID"));
  N2.one_time_init = (fn_v)need2(NAT("DemoGLSurfaceView", "nativeProjectOneTimeInit"));
  N2.read_cpu_info = (fn_z)need2(NAT("DemoActivity", "nativeReadCPUInfo"));
  N2.set_make_model = (fn_ss)need2(NAT("DemoActivity", "nativeSetDeviceMakeModel"));
  N2.screen_size_init = (fn_v)need2(NAT("DemoActivity", "nativeScreenSizeInit"));
  N2.set_file_system = (fn_ssii)need2(NAT("DemoActivity", "nativeSetFileSystem"));
  N2.set_screen_size = (fn_iiii)need2(NAT("DemoRenderer", "nativeSetScreenSize"));
  N2.project_init = (fn_ii)need2(NAT("DemoRenderer", "nativeProjectInit"));
  N2.project_run = (fn_ri)need2(NAT("DemoRenderer", "nativeProjectRun"));
  N2.resume = (fn_v)need2(NAT("DemoRenderer", "nativeResume"));
  N2.save_state = (fn_v)need2(NAT("DemoRenderer", "nativeSaveState"));
  N2.set_pause = (fn_v)need2(NAT("DemoActivity", "nativeSetPause"));
  N2.touch = (fn_touch)need2(NAT("DemoGLSurfaceView", "nativeTouchHandlePoint"));
  N2.key = (fn_key)need2(NAT("DemoGLSurfaceView", "nativeProjectKey"));
  N2.accel = (fn_accel)need2(NAT("DemoGLSurfaceView", "nativeProjectSetAcceleration"));
  if (!N2.cfg || !N2.set_file_system || !N2.set_screen_size || !N2.project_init || !N2.project_run || !N2.touch)
    return -1;
  if (ssr_gfx_second_context() != 0)
    return -1;
  int rc = 0;
  AS_ENGINE(1, {
    void *act = ssr_class(C_ACTIVITY), *view = ssr_class(C_VIEW);
    if (N2.set_device_id)
      N2.set_device_id(ENV, act, jni_str("1543000000000000p2"));
    N2.cfg(ENV, act, 1, ssr_input_keyboard_cfg());
    N2.cfg(ENV, act, 4, 0);
    if (N2.one_time_init)
      N2.one_time_init(ENV, view);
    if (N2.read_cpu_info)
      N2.read_cpu_info(ENV, act, 1);
    if (N2.set_make_model)
      N2.set_make_model(ENV, g_activity, jni_str(SSR_DEVICE_MAKE), jni_str(SSR_DEVICE_MODEL));
    ssr_patch_after_make_model_in(1);
    if (N2.screen_size_init)
      N2.screen_size_init(ENV, act);
    N2.cfg(ENV, act, 6, 1);
    const SsrPack *p = ssr_pack();
    N2.set_file_system(ENV, act, jni_str(p->path), jni_str("/data/data/" DCR_PACKAGE "/p2"), (jint)p->pack_off,
                       (jint)p->pack_len);
    N2.cfg(ENV, act, 5, ssr_language());
    if (ssr_gfx_use(1, 1) != 0) {
      rc = -1;
    } else {
      N2.set_screen_size(ENV, g_renderer, S.w, S.h, 0, 0);
      if (N2.resume)
        N2.resume(ENV, g_renderer);
      N2.project_init(ENV, g_renderer, SSR_MAX_MEMORY, SSR_TOTAL_MEMORY);
      ssr_patch_after_init_in(1);
      ssr_race_init_in(1);
    }
  });
  ssr_gfx_use(0, 0);
  if (rc)
    return rc;
  eng_resolve(1);
  g_e[1].w = S.w, g_e[1].h = S.h, g_e[1].uniform = 0;
  AS_ENGINE(1, ssr_menu2_init());
  debugPrintf("[split] the second copy is up in %llu ms\n",
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
  return g_e[1].ok ? 0 : -1;
}

/* ------------------------------------------------------------ engine 2's fingers */
/* the menus' fingers (ssr_menu2.c) as engine 2's touches: down, moved, up */
#define MAX_F2 4
static SsrFinger g_f2[MAX_F2];
static int g_nf2;

static int time_ms(void) { return (int)(armTicksToNs(armGetSystemTick()) / 1000000ull); }
static void touch2(int action, int x, int y) {
  if (dcr_config()->log_touch)
    debugPrintf("[split] engine 2 touch %d at %d,%d\n", action, x, y);
  N2.touch(ENV, ssr_class(C_VIEW), action, x, y, time_ms());
}

static void fingers2(const SsrFinger *want, int n) {
  for (int k = 0; k < g_nf2;) {
    int still = 0;
    for (int i = 0; i < n && !still; i++)
      still = want[i].key == g_f2[k].key;
    if (still) {
      k++;
      continue;
    }
    touch2(3, g_f2[k].x, g_f2[k].y);
    memmove(&g_f2[k], &g_f2[k + 1], sizeof g_f2[0] * (size_t)(g_nf2 - k - 1));
    g_nf2--;
  }
  for (int k = 0; k < g_nf2; k++)
    for (int i = 0; i < n; i++)
      if (want[i].key == g_f2[k].key) {
        g_f2[k].x = want[i].x, g_f2[k].y = want[i].y;
        touch2(2, g_f2[k].x, g_f2[k].y);
      }
  for (int i = 0; i < n && g_nf2 < MAX_F2; i++) {
    int known = 0;
    for (int k = 0; k < g_nf2 && !known; k++)
      known = g_f2[k].key == want[i].key;
    if (known)
      continue;
    g_f2[g_nf2++] = want[i];
    touch2(1, want[i].x, want[i].y);
  }
}

/* Back, on engine 2 (its menus' B: ssr_menu2.c) */
void ssr_split_back2(void) {
  if (N2.key) {
    N2.key(ENV, ssr_class(C_VIEW), 3, 1);
    N2.key(ENV, ssr_class(C_VIEW), 3, 0);
  }
}

/* ------------------------------------------------------------ engine 2's way to its racer select */
static void note2(const char *what, const void *scr, int state) {
  if (scr == S.seen2 && state == S.seen2_state)
    return;
  S.seen2 = scr, S.seen2_state = state;
  debugPrintf("[split] engine 2: %s (state %d)\n", what, state);
}

/* a press now and then (its screens act on fresh presses; a carousel
 * turning takes its time) */
static int pulse(void) {
  const u64 now = armGetSystemTick();
  if (armTicksToNs(now - S.pulse_at) < 350000000ull)
    return 0;
  S.pulse_at = now;
  return 1;
}

/* player 2's own screens: the licence (theirs to pick, the first time) and
 * the racer select */
static int e2_players_screen(const uint8_t *top) {
  if (is_vt(1, top, VT_CHARSEL))
    return 1;
  if (S.lic_done)
    return 0;
  return is_vt(1, top, VT_LICENSE) || is_vt(1, top, VT_CREATE_LICENSE) || is_vt(1, top, VT_KEYBOARD) ||
         is_vt(1, top, VT_PORTRAIT) || is_vt(1, top, VT_NATION) || is_vt(1, top, VT_LICSCREEN);
}

int ssr_split_players_licence(int i) {
  uint8_t *top = top_of(i & 1);
  return (i & 1) && !S.lic_done && top && !is_vt(1, top, VT_CHARSEL) && e2_players_screen(top);
}

/* the pad engine 2's menus get when it goes by itself: presses toward its
 * racer select (the menus' own driver does the rest: ssr_menu2.c) -- each a
 * frame held (the menus' directions go by what is held) */
static void script_pad_(SsrPad *sp, uint8_t *top);
static void script_pad(SsrPad *sp, uint8_t *top) {
  script_pad_(sp, top);
  sp->held = sp->down;
}
static void script_pad_(SsrPad *sp, uint8_t *top) {
  if (ssr_split_net_script2(sp, top))
    return; /* split screen's BATTLE: its LOCAL menu, joining (ssr_split_net.c) */
  memset(sp, 0, sizeof *sp);
  const int ps = popup_state(1);
  if (ps < 0 || loading(1) || !top)
    return;
  Eng *e = &g_e[1];
  if (ps > 0) {
    note2("a pop-up", top, 0);
    if (pulse())
      sp->down = HidNpadButton_A;
    return;
  }
  int st = 0;
  if (is_vt(1, top, VT_TITLE)) {
    st = *(const int32_t *)(top + 0xc);
    note2("TitleScreen", top, st);
    if (st == 4 && pulse())
      sp->down = HidNpadButton_A;
  } else if (is_vt(1, top, VT_LICENSE)) {
    note2("SelectLicenseMenu (player 2's to pick)", top, *(const int32_t *)(top + 0x70));
  } else if (is_vt(1, top, VT_MAIN) || is_vt(1, top, VT_SOLO)) {
    const int solo = is_vt(1, top, VT_SOLO);
    st = *(const int32_t *)(top + 0x6c);
    note2(solo ? "SoloMenu" : "MainMenu", top, st);
    if (st == 3 && e->car_page && !*(const uint32_t *)(top + 0xc + 0x38) && pulse()) {
      /* the main menu: SINGLE PLAYER (0); its menu: SINGLE RACE (1) */
      const int want = solo ? 1 : 0, page = e->car_page(top + 0xc, NULL);
      sp->down = page < want ? HidNpadButton_Right : page > want ? HidNpadButton_Left : HidNpadButton_A;
    }
    if (!solo)
      S.lic_done = 1; /* past the licence menu: player 2's licence is set */
  } else if (is_vt(1, top, VT_CHARSEL)) {
    note2("CharacterSelectScreen (waiting for player 2)", top, *(const int32_t *)(top + 0xdc));
  } else if (is_vt(1, top, VT_SEGASDK)) {
    note2("SegaSDKScreen", top, *(const int32_t *)(top + 0x10));
    if (pulse())
      sp->down = HidNpadButton_A; /* the guest's button (its default focus) */
  } else if (is_vt(1, top, VT_TRACKSEL) || is_vt(1, top, VT_GPSEL) || is_vt(1, top, VT_GPDIFF) ||
             is_vt(1, top, VT_MISSIONSEL)) {
    note2("a menu after the racer select: back", top, 0);
    if (pulse())
      sp->down = HidNpadButton_B;
  } else if (!e2_players_screen(top)) {
    note2("another screen", top, *(const int32_t *)(top + 0xc));
    if (pulse())
      sp->down = HidNpadButton_A;
  }
}

/* ------------------------------------------------------------ READY */
/* the racer select's A / B, from either copy's menus (ssr_menu.c's DAVE
 * model): 1 if split screen takes it */
/* the front racer can be picked: the carousel's own test (DavesBaseCarousel::
 * AttemptSelect, 0xbdfd8: the slot's locked flag, +8 of its 12-byte slots at
 * +0x84, the front one +0x14) */
static int available(const uint8_t *cs) {
  const uint8_t *car = cs + 0x38;
  const uint8_t *slots = *(const uint8_t *const *)(car + 0x84);
  const int32_t cur = *(const int32_t *)(car + 0x14);
  return slots && cur >= 0 && cur < 64 ? slots[cur * 0xc + 8] == 0 : 1;
}

int ssr_split_dave(int engine, int ev, uint8_t *cs) {
  if (!S.cols)
    return 0;
  const int me = engine & 1, other = !me;
  switch (ev) {
  case SSR_DAVE_TURN: /* READY: the carousel stays */
    return S.ready[me];
  case SSR_DAVE_B:
    if (S.ready[me]) {
      S.ready[me] = 0;
      play(me, SND_TICK);
      debugPrintf("[split] player %d: not ready\n", me + 1);
      return 1;
    }
    return me == 1; /* player 2 never leaves the racer select */
  case SSR_DAVE_A: {
    if (S.ready[me])
      return 1;
    const int ch = cs ? *(const int32_t *)(cs + 0xd0) : -1;
    if (ch < 0 || !available(cs))
      return me == 1; /* locked: player 1's copy shakes its padlock itself */
    if (S.ready[other] && S.pick[other] == ch) {
      debugPrintf("[split] player %d: racer %d is player %d's\n", me + 1, ch, other + 1);
      return 1;
    }
    S.pick[me] = ch, S.ready[me] = 1;
    debugPrintf("[split] player %d: READY (racer %d, %s)\n", me + 1, ch, ssr_race_racer_name(me, ch));
    if (me == 1) {
      play(1, SND_TICK);
      if (S.ready[0])
        S.go = 1; /* player 1 waited: their copy picks now */
      return 1;
    }
    if (S.ready[1])
      return 0; /* both: player 1's copy picks its racer (the game goes on) */
    play(0, SND_TICK);
    return 1;
  }
  case SSR_DAVE_POLL: /* player 1's copy: pick now? */
    if (S.go) {
      S.go = 0;
      return 1;
    }
    return 0;
  default:
    return 0;
  }
}

/* the Solo menu's cards split screen does not have as they are (TIME TRIAL:
 * split screen's BATTLE; MISSIONS: its VS RACE -- ssr_split_net.c) */
int ssr_split_solo_blocked(int page) { return S.session && (page == 2 || page == 3); }

#define SPLIT_MIX 0.9f /* each copy's sound in BATTLE and VS RACE (each sound in one copy: ssr_split_audio.c) */

/* ------------------------------------------------------------ the session */
static void session_end(const char *why) {
  if (!S.session)
    return;
  S.session = 0;
  S.cols = 0;
  S.ready[0] = S.ready[1] = 0;
  S.pick[0] = S.pick[1] = -1;
  S.go = 0;
  ssr_input_split(0);
  ssr_audio_engine_gain(0, 1.0f);
  ssr_audio_engine_gain(1, 0.0f);
  debugPrintf("[split] split screen off (%s)\n", why);
}

static void session_frame(void) {
  uint8_t *top = top_of(0);
  const int is_main = is_vt(0, top, VT_MAIN), is_solo = is_vt(0, top, VT_SOLO);
  Eng *e = &g_e[0];
  if (is_main && *(const int32_t *)(top + 0x6c) == 3 && e->car_page)
    S.main_page = e->car_page(top + 0xc, NULL);
  if (!S.session) {
    /* MULTIPLAYER (page 1) made the Solo menu (ssr_menu.c's patch): split
     * screen, if two controllers are there */
    if (is_solo && S.was_main && S.main_page == 1) {
      S.was_main = 0;
      debugPrintf("[split] SPLIT SCREEN: the controllers for two\n");
      /* Joy-Cons held sideways first: the controller screen offers a
       * Joy-Con each only then (with them upright it has nothing for two
       * players in handheld mode and closes at once: hardware, round 8) */
      ssr_input_split(1);
      if (!ssr_input_controllers_2()) {
        debugPrintf("[split] one controller: back to the main menu\n");
        ssr_input_split(0);
        ssr_input_back();
        return;
      }
      if (S.e2 == 0) {
        ssr_boot_system_dialog(1);
        const int rc = boot2();
        ssr_boot_system_dialog(0);
        ssr_clock_resync(); /* the time it took is no game time */
        S.e2 = rc == 0 ? 1 : -1;
        if (rc != 0)
          debugPrintf("[split] the second copy did not start: no split screen this session\n");
      }
      if (S.e2 < 0) {
        ssr_input_back();
        return;
      }
      S.session = 1;
      S.ready[0] = S.ready[1] = 0;
      S.go = 0;
      ssr_input_split(1);
      debugPrintf("[split] split screen on\n");
    }
  } else if ((is_main && !ssr_split_net_active()) || is_vt(0, top, VT_TITLE)) {
    session_end("the main menu"); /* (BATTLE's way back passes it: ssr_split_net.c) */
  }
  if (top && !loading(0))
    S.was_main = is_main;
}

/* ------------------------------------------------------------ the main menu's card */
/* the mode select's third card: split screen's BATTLE art (ssr_battlecard.c) */
static void battlecard_frame(void) {
  uint8_t *top = top_of(0);
  if (!is_vt(0, top, VT_SOLO) || !g_e[0].succ || loading(0))
    return;
  for (uint8_t *n = scene_first(g_e[0].sm); n; n = g_e[0].succ(n))
    if (scene_hash(n) == 0x131fb476u) { /* the Solo menu's cards (its page table, 0x218b20) */
      void ssr_battlecard_frame(void *loadset, int on);
      ssr_battlecard_frame(*(void **)(n + 0x30), S.session);
      return;
    }
}

static void xcard_frame(void) {
  uint8_t *top = top_of(0);
  if (!is_vt(0, top, VT_MAIN) || !g_e[0].succ)
    return;
  for (uint8_t *n = scene_first(g_e[0].sm); n; n = g_e[0].succ(n))
    if (scene_hash(n) == 0x98aaddd5u) { /* MAINMENU_CARDS */
      ssr_xcard_frame(*(void **)(n + 0x30));
      return;
    }
}

/* ------------------------------------------------------------ the frame */
void ssr_split_init(void) {
  ssr_gfx_size(&S.w, &S.h);
  eng_resolve(0);
  g_e[0].w = S.w, g_e[0].h = S.h;
  ssr_split_race_init();
  debugPrintf("[split] split screen: %s (the main menu's SPLIT SCREEN)\n",
              dcr_config()->split_screen ? "on" : "off ([multiplayer] split_screen)");
}

void ssr_split_pre(void) {
  if (!dcr_config()->split_screen || !g_e[0].ok)
    return;
  S.p2_there = ssr_input_p2(&S.p2);
  session_frame();
  xcard_frame();
  battlecard_frame();
  if (S.session && S.e2 > 0)
    ssr_split_net_pre(); /* BATTLE (ssr_split_net.c) */
  ssr_split_race_pre();
  if (ssr_split_race_active())
    ssr_split_race_pad2(&S.p2);
  /* the racer select: player 1's copy's, and player 2's beside it */
  uint8_t *top = top_of(0);
  const int cols = S.session && S.e2 > 0 && is_vt(0, top, VT_CHARSEL) && !loading(0);
  if (cols != S.cols) {
    S.cols = cols;
    if (cols)
      S.ready[0] = 0, S.go = 0; /* player 1 picks (again) */
    debugPrintf("[split] %s\n", cols ? "the racer select: two columns" : "one screen");
  }
  if (!ssr_split_race_active() && !ssr_split_net_active())
    place_columns(0, S.cols);
  if (!ssr_split_net_active())
    ssr_prompt_columns(S.cols);
  if (S.cols) {
    const float cw = S.w * 0.5f;
    for (int i = 0; i < 2; i++)
      if (S.ready[i])
        ssr_prompt_card(SSR_CARD_READY, cw * i + cw * 0.5f, S.h * 0.62f);
  }
  /* engine 2 runs while it is on its way to its racer select, and while its
   * column is up */
  uint8_t *top2 = S.e2 > 0 ? top_of(1) : NULL;
  S.run2 = S.session && S.e2 > 0 && !ssr_split_race_active() &&
           (S.cols || !is_vt(1, top2, VT_CHARSEL) || popup_state(1) != 0 || ssr_split_net_active());
  /* the copies' sound: in a split race it is player 1's copy's (the one
   * race, heard from both players' cameras: ssr_split_audio.c; player 2's
   * copy only draws their HUD, which would count down twice); player 2's
   * copy's menus are heard (their column's clicks); in BATTLE and VS RACE
   * each copy's, from its own player's camera, mixed -- each sound in one
   * copy only (ssr_split_audio.c), as Mario Kart's split screen hears both
   * players */
  static int mix = -1;
  const int want = !S.run2 ? 0 : ssr_split_net_racing() ? 2 : 1;
  if (S.e2 > 0 && mix != want) {
    mix = want;
    ssr_audio_engine_gain(0, want == 2 ? SPLIT_MIX : 1.0f);
    ssr_audio_engine_gain(1, want == 2 ? SPLIT_MIX : want == 1 ? 0.8f : 0.0f);
    debugPrintf("[split] the sound: %s\n", want == 2   ? "both copies' (their players' racing, mixed at 90% each)"
                                          : want == 1 ? "player 1's copy's, player 2's copy's menus at 80%"
                                                      : "player 1's copy's");
  }
}

/* player 1's copy draws into its framebuffer (split screen's BATTLE: each
 * copy its half, composed at the present) */
int ssr_split_engine1_offscreen(void) { return ssr_split_net_race(); }

int ssr_split_post(uint64_t *run_ticks) {
  if (!S.run2) {
    ssr_gfx_split(SSR_SPLIT_NONE, NULL);
    return 0;
  }
  Eng *e = &g_e[1];
  uint8_t *top = top_of(1);
  /* player 2's column shows only their own screens (their licence, their
   * racer select); what their copy goes through to get there (its title, its
   * menus: the mode is player 1's to pick) is never theirs to see */
  const int net = ssr_split_net_active();
  const int theirs = top && (e2_players_screen(top) || ssr_split_net_theirs(top)) && !loading(1);
  const int players = (S.cols && top && (e2_players_screen(top) || popup_state(1) > 0)) ||
                      (net && top && !ssr_split_net_auto_popup() &&
                       (ssr_split_net_theirs(top) || (popup_state(1) > 0 && is_vt(1, top, VT_HUD))));
  SsrPad pad;
  const uint32_t renders = *e->renders;
  const u64 t0 = armGetSystemTick();
  int quit = 0;
  /* All of player 2's copy's own code runs as it (AS_ENGINE: its tables,
   * picked by the running copy) and with its GL context current -- what its
   * script does too (a screen pushed over its racer select unloads its cars:
   * their buffers and textures are its context's). Run as player 1's, their
   * buffers were looked up in player 1's copy's table, missed, and
   * DDGLRefresh::ddGLDeleteBuffers wrote over the heap beside player 2's
   * (the game has no check for a buffer it does not know): the crash after a
   * battle (hardware, round 9). */
  AS_ENGINE(1, {
    ssr_gfx_use(1, 1);
    if (players)
      pad = S.p2;
    else
      script_pad(&pad, top);
    if (net) {
      ssr_split_net_frame2(&pad); /* its place, its HUD, its racer (the battle's) */
    } else {
      eng_size(1, S.w, S.h, 0);
      place_columns(1, S.cols);
      ssr_race_frame_in(1, &pad, 0);
    }
    SsrFinger want[MAX_F2];
    int n = 0;
    ssr_menu2_frame(&pad, S.w, S.h, want, &n, MAX_F2, 0);
    fingers2(want, n);
    ssr_split_race_preload2(is_vt(1, top, VT_CHARSEL)); /* the HUD's project, for the race */
    if (!theirs) { /* nothing of its menus in player 2's column: their prompts, their focus */
      ssr_prompt2_set(SSR_PROMPT_NONE);
      ssr_gfx_focus2(0, 0, 0, 0, 0);
    }
    if (N2.accel)
      N2.accel(ENV, ssr_class(C_VIEW), 0.0f, 0.0f, -1.0f);
    ssr_gfx_use(1, 1);
    quit = N2.project_run(ENV, g_renderer, 1) == 1;
  });
  ssr_gfx_use(0, 0);
  if (run_ticks)
    *run_ticks += armGetSystemTick() - t0;
  if (quit) {
    debugPrintf("[split] the second copy quit (nativeProjectRun = 1): split screen off\n");
    S.e2 = -1;
    session_end("player 2's copy quit");
    S.run2 = 0;
  }
  const int drew = *e->renders != renders;
  S.frames2 += drew;
  if (ssr_split_net_display(drew))
    return drew;
  if (!S.cols) {
    ssr_gfx_split(SSR_SPLIT_NONE, NULL);
    return 0;
  }
  /* player 2's column: the right half of its picture; until it is at their
   * licence or their racer select, player 1's scenery there (the racer
   * select's backdrop: its scenes stay in their column) and a word that
   * player 2 comes */
  if (theirs) {
    ssr_gfx_split_source(1, 0.5f, 0.0f, 1.0f, 1.0f);
    ssr_gfx_split(SSR_SPLIT_COLUMNS, NULL);
  } else {
    ssr_gfx_split(SSR_SPLIT_JOINING, NULL);
    ssr_prompt_card(SSR_CARD_JOINING, S.w * 0.75f, S.h * 0.5f);
  }
  return drew;
}

/* HOME: engine 2's onPause / onResume, with engine 1's */
void ssr_split_focus(int focused) {
  if (S.e2 <= 0)
    return;
  AS_ENGINE(1, {
    if (!focused) {
      if (N2.save_state)
        N2.save_state(ENV, g_renderer);
      if (N2.set_pause)
        N2.set_pause(ENV, ssr_class(C_ACTIVITY));
      for (int k = 0; k < g_nf2; k++)
        touch2(3, g_f2[k].x, g_f2[k].y);
      g_nf2 = 0;
    } else if (N2.resume) {
      N2.resume(ENV, g_renderer);
    }
  });
}

void ssr_split_exit(void) {
  if (S.e2 <= 0 || !N2.save_state)
    return;
  AS_ENGINE(1, N2.save_state(ENV, g_renderer));
}

void ssr_split_report(void) {
  if (!S.session && S.e2 == 0)
    return;
  debugPrintf("[split] %s; engine 2 %s, drew %u frames; %s; player 2's controller %s\n",
              S.session ? "split screen on" : "split screen off",
              S.e2 > 0 ? "up" : S.e2 < 0 ? "FAILED" : "not made", S.frames2,
              ssr_split_race_split() ? "the screen split (a race)" : S.cols ? "the racer select in two columns"
                                                                           : "one screen",
              S.p2_there ? "there" : "missing");
  ssr_split_race_report();
  void ssr_split_audio_report(void);
  ssr_split_audio_report();
  S.frames2 = 0;
}

/* the race HUD's layout for engine `engine` (ssr_input.c: player 1's copy) */
int ssr_split_hud_layout(int engine) {
  const int net = ssr_split_net_hud_layout(engine);
  if (net >= 0)
    return net;
  if (engine == 0) /* from the race's start: its elements slide in where they stay (and while the race
                    * presents itself on the whole screen, its HUD is not drawn: ssr_split_race.c) */
    return ssr_split_race_active() ? SSR_HUD_SPLIT : SSR_HUD_PHONE;
  return SSR_HUD_BOTTOM;
}

/* the strip's uniform fit: the side margin, in design widths (960 x 640) */
float ssr_split_margin(void) {
  if (S.w <= 0 || S.h <= 0)
    return 0.0f;
  const float s = (S.h * 0.5f) / 640.0f, band = 960.0f * s;
  return ((float)S.w - band) * 0.5f / band;
}
