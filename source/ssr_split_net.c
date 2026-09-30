/* ssr_split_net.c -- split screen's BATTLE and VS RACE: the phone's LOCAL
 * battle and race, the two copies of the engine two phones on the in-memory
 * LAN (ssr_net.c).
 *
 * The game has battles only as a network race (STNetRace_Battle: the LOCAL
 * lobby's HOST BATTLE), and a race of two with no AI racers only as one
 * (STNetRace_GrandPrix: HOST RACE), so these two are the earlier split
 * screen's way (MULTIPLAYER.md; the single races and Grand Prix are one race
 * in player 1's copy: ssr_split_race.c):
 *   - BATTLE (the mode select's third card) and VS RACE (its fourth: ssr_race.c's
 *     words, ssr_battlecard.c's art) have player 1's copy host the game as
 *     the LOCAL menu's HOST BATTLE / HOST RACE does, straight away
 *     (host_game): the lobby (LobbyScreen(type)), open to joiners -- nobody
 *     picks "host" or "join";
 *   - player 2's copy, wherever its menus are, opens its own LOCAL menu,
 *     JOIN, and joins player 1's game (SelectServerScreen: MULTIPLAYER.md
 *     2.4); the first time player 2 picks their licence on the way (their
 *     column);
 *   - SELECT RACER in two columns, each player's own lobby (player 1's at the
 *     left, player 2's at the right: the lobby's panel and racer picker
 *     placed in the column); then player 1's lobby, the whole screen: the
 *     arena or the course, START;
 *   - the battle or race: each copy draws its half (W x H/2, into its
 *     framebuffer: player 1's the top, player 2's the bottom), with its own
 *     HUD at its half's edges (a race's progress bar at the half's top);
 *     each player's controller drives their copy's racer; both copies'
 *     sound, mixed (ssr_split.c). A race's first over the line starts the
 *     other's 30 seconds (the game's online races' own clock);
 *   - the results, each copy's in its half; then the lobby again. Player 1
 *     leaving the lobby (B at SELECT RACER) ends it: player 1's copy goes
 *     back to the mode select, player 2's (its lobby lost) back to where it
 *     was.
 * MIT.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "dcr_config.h"
#include "ssr.h"
#include "ssr_split_priv.h"
#include "util.h"

/* ------------------------------------------------------------ each copy's */
typedef int (*fn_vcall)(void *self);
static struct {
  int ok;
  void *(*new_)(uint32_t n);
  void (*netmenu)(void *self, int type);     /* NetMenu::NetMenu(eNetworkType) */
  void (*solomenu)(void *self, int page);    /* SoloMenu::SoloMenu(PageType) */
  void (*push)(void *stack, void *screen);   /* ScreenStack::PushScreen */
  void (*pop)(void *stack);                  /* ScreenStack::PopScreen */
  void **stack;                              /* &g_pScreenStack */
  void **inst;                               /* &NetworkAndroid::ms_Instance */
  int (*car_page)(const void *car, uint8_t *wrapped);
  void *(*byhash)(const void *stack, uint32_t hash);
  uint8_t **game;
} N[2];

static void resolve(int i) {
  if (N[i].ok)
    return;
#define S_(s) ssr_native_in(i, s)
  N[i].new_ = (void *(*)(uint32_t))S_("_Znwj");
  N[i].netmenu = (void (*)(void *, int))S_("_ZN7NetMenuC1ENS_12eNetworkTypeE");
  N[i].solomenu = (void (*)(void *, int))S_("_ZN8SoloMenuC1E8PageType");
  N[i].push = (void (*)(void *, void *))S_("_ZN11ScreenStack10PushScreenEP6Screen");
  N[i].pop = (void (*)(void *))S_("_ZN11ScreenStack9PopScreenEv");
  N[i].stack = (void **)S_("g_pScreenStack");
  N[i].inst = (void **)S_("_ZN14NetworkAndroid11ms_InstanceE");
  N[i].car_page = (int (*)(const void *, uint8_t *))S_("_ZNK8Carousel17GetCurrentPageIdxEPb");
  N[i].byhash = (void *(*)(const void *, uint32_t))S_("_ZNK11ScreenStack15GetScreenByHashEj");
  N[i].game = (uint8_t **)S_("g_pGame");
#undef S_
  N[i].ok = N[i].new_ && N[i].netmenu && N[i].solomenu && N[i].push && N[i].pop && N[i].stack && N[i].inst && N[i].car_page && N[i].byhash &&
            N[i].game;
  debugPrintf("[lan] engine %d's LOCAL play: %s\n", i + 1, N[i].ok ? "found" : "NOT FOUND");
}

/* copy i's LOCAL menu (NetMenu(0)) over its top screen */
static int push_netmenu(int i) {
  resolve(i);
  if (!N[i].ok || !*N[i].stack)
    return 0;
  void *m = N[i].new_(0x78);
  if (!m)
    return 0;
  N[i].netmenu(m, 0);
  N[i].push(*N[i].stack, m);
  debugPrintf("[lan] engine %d: its LOCAL menu\n", i + 1);
  return 1;
}

/* Player 1's copy hosts the game straight away, as the LOCAL menu's HOST
 * BATTLE / HOST RACE do (NetMenu::Update, 0xd7f60 / 0xd801c): the LAN's
 * server made and named "B:" or "R:" and the licence's name (how joiners
 * learn the game's kind), then its lobby, LobbyScreen(type, false) -- no
 * LOCAL menu on the way. */
enum { NET_RACE = 0, NET_BATTLE = 1 }; /* LobbyScreen::eRaceType */
static int host_game(int type) {
  resolve(0);
  void (*create)(void) = (void (*)(void))ssr_native_in(0, "_ZN20NetworkAndroidServer6CreateEv");
  const char *(*get_name)(void *, uint32_t) =
      (const char *(*)(void *, uint32_t))ssr_native_in(0, "_ZN20STSaveProfileControl7GetNameEm");
  uint8_t **gd = (uint8_t **)ssr_native_in(0, "g_pGameData");
  void (*lobby)(void *, int, int) = (void (*)(void *, int, int))ssr_native_in(0, "_ZN11LobbyScreenC1ENS_9eRaceTypeEb");
  int32_t *nettype = (int32_t *)ssr_native_in(0, "_ZN7NetMenu13s_networkTypeE");
  if (!N[0].ok || !create || !get_name || !gd || !*gd || !lobby || !*N[0].stack)
    return 0;
  if (nettype)
    *nettype = 0; /* LOCAL */
  create();
  void *ni = *N[0].inst;
  if (!ni)
    return 0;
  char name[80];
  const char *lic = get_name(*gd + 4, (*gd)[0x7ef]);
  snprintf(name, sizeof name, "%s:%s", type == NET_BATTLE ? "B" : "R", lic ? lic : "");
  ((void (*)(void *, const char *))(*(void ***)ni)[2])(ni, name); /* Initialise */
  void *ls = N[0].new_(0x1568);
  if (!ls)
    return 0;
  lobby(ls, type, 0);
  N[0].push(*N[0].stack, ls);
  debugPrintf("[lan] engine 1 hosts \"%s\": the %s lobby\n", name, type == NET_BATTLE ? "battle" : "race");
  return 1;
}

/* copy i's LOCAL menu off again, taking input: back to what was under it
 * (its own Back goes on to the LOCAL / ONLINE menu) */
static int pop_netmenu(int i) {
  uint8_t *top = ssr_split_top(i);
  if (!N[i].ok || !ssr_split_is(i, top, VT_NET) || *(const int32_t *)(top + 0x6c) != 4 || ssr_split_popup(i) != 0 ||
      ssr_split_loading(i))
    return 0;
  N[i].pop(*N[i].stack);
  debugPrintf("[lan] engine %d: its LOCAL menu closed\n", i + 1);
  return 1;
}

/* After a battle the game has made its menus again the phone's way (the
 * main menu, MULTIPLAYER, LOCAL, the lobby): leaving, the LOCAL screens go,
 * back to the main menu; there player 1's copy gets split screen's mode
 * select again (the SPLIT SCREEN card's SoloMenu), player 2's goes its usual
 * way from there (ssr_split.c). A step a frame; 1 while it goes on. */
static int way_back(int i) {
  uint8_t *top = ssr_split_top(i);
  if (!N[i].ok || !top || ssr_split_loading(i) || ssr_split_popup(i) != 0)
    return 1;
  if (ssr_split_is(i, top, VT_NET))
    return pop_netmenu(i), 1;
  if (ssr_split_is(i, top, VT_MPSERVICE)) {
    N[i].pop(*N[i].stack);
    debugPrintf("[lan] engine %d: the MULTIPLAYER menu closed\n", i + 1);
    return 1;
  }
  if (i == 0 && ssr_split_is(0, top, VT_MAIN)) {
    void *m = N[0].new_(0x78);
    if (m) {
      N[0].solomenu(m, 0);
      N[0].push(*N[0].stack, m);
      debugPrintf("[lan] engine 1: the mode select again\n");
    }
    return 1;
  }
  return 0;
}

/* copy 1 hosts a LOCAL game that takes joiners (IsServer, not IsOnline) */
static int host_open(void) {
  void *ni = N[0].ok ? *N[0].inst : NULL;
  if (!ni)
    return 0;
  fn_vcall *vt = *(fn_vcall **)ni;
  return vt[0x18 / 4](ni) && !vt[0x14 / 4](ni);
}

/* copy i in a race (its game made) */
static int in_race(int i) { return N[i].ok && *N[i].game != NULL; }

/* copy i's lobby state (LobbyScreen +0xc), -1 when its lobby is not the
 * screen (or a loading screen is over it) */
static int lobby_state(int i) {
  uint8_t *top = ssr_split_top(i);
  if (!top || !ssr_split_is(i, top, VT_LOBBY) || ssr_split_loading(i))
    return -1;
  return *(const int32_t *)(top + 0xc);
}

/* copy i's pause menu is up (under its HUD) */
static int paused(int i) {
  uint8_t *pm = N[i].ok && *N[i].stack ? (uint8_t *)N[i].byhash(*N[i].stack, 0x1a29a09fu) : NULL;
  const uint32_t st = pm ? *(const uint32_t *)(pm + 0xc) : 0;
  return pm && st != 0 && st != 4;
}

/* ------------------------------------------------------------ the session */
enum { B_OFF, B_OPEN, B_LOBBY, B_LEAVE };
static const char *const k_stage[] = {"off", "opening", "in the lobby", "leaving"};
static struct {
  int stage;
  int want;     /* BATTLE or VS RACE chosen: the lobby, next frame */
  int type;     /* ...which (NET_RACE, NET_BATTLE) */
  int pushed2;  /* player 2's copy's LOCAL menu opened */
  int joined2;  /* ...it has been in player 1's lobby */
  int cols;     /* SELECT RACER in two columns */
  int race;     /* both copies race: the screen split */
  int sized;    /* the copies sized for their halves */
  u64 pulse[2]; /* the scripted presses' times */
  u64 since;
} B;

static void stage(int s) {
  if (B.stage != s)
    debugPrintf("[lan] %s -> %s\n", k_stage[B.stage], k_stage[s]);
  B.stage = s;
  B.since = armGetSystemTick();
}
static float secs(void) { return (float)armTicksToNs(armGetSystemTick() - B.since) * 1e-9f; }

/* a press now and then (a menu acts on fresh presses; a carousel turns) */
static int pulse(int i) {
  const u64 now = armGetSystemTick();
  if (armTicksToNs(now - B.pulse[i]) < 350000000ull)
    return 0;
  B.pulse[i] = now;
  return 1;
}

int ssr_split_net_active(void) { return B.stage != B_OFF; }
/* either copy in the battle or race (or its results): both copies' sound
 * (each plays every racer's, heard from its own player's camera -- the two
 * mixed, as Mario Kart's split screen hears both players) */
int ssr_split_net_racing(void) { return B.stage != B_OFF && (in_race(0) || in_race(1)); }
int ssr_split_net_race(void) { return B.race; }
static const char *game_name(void) { return B.type == NET_BATTLE ? "the battle" : "the race"; }

void ssr_split_net_choose(int battle) {
  if (B.stage != B_OFF || !ssr_split_e2_up())
    return;
  B.want = 1;
  B.type = battle ? NET_BATTLE : NET_RACE;
  debugPrintf("[lan] %s: player 1's copy hosts a LOCAL %s, player 2's joins it\n", battle ? "BATTLE" : "VS RACE",
              battle ? "battle" : "race");
}

/* ------------------------------------------------------------ player 1's copy */
/* its LOCAL menu, driven: HOST BATTLE (the third page) or HOST RACE (the
 * second), or back out (leaving) */
int ssr_split_net_pad1(SsrPad *p) {
  if (B.stage == B_OFF)
    return 0;
  uint8_t *top = ssr_split_top(0);
  if (B.stage == B_LEAVE) { /* back to the mode select: nothing of player 1's meanwhile */
    memset(p, 0, sizeof *p);
    if (!ssr_split_is(0, top, VT_SOLO))
      way_back(0);
    return 1;
  }
  if (!ssr_split_is(0, top, VT_NET) || ssr_split_loading(0))
    return 0;
  memset(p, 0, sizeof *p);
  if (ssr_split_popup(0) != 0 || *(const int32_t *)(top + 0x6c) != 4 || !pulse(0))
    return 1;
  const int page = N[0].car_page(top + 0xc, NULL), want = B.type == NET_BATTLE ? 2 : 1;
  p->down = p->held = page == want ? HidNpadButton_A : page < want ? HidNpadButton_Right : HidNpadButton_Left;
  return 1;
}

/* ------------------------------------------------------------ player 2's copy */
/* SelectServerScreen: the host's game (its first row) joined, as a tap on its
 * row would (MULTIPLAYER.md 2.4: Connect, then the screen's "connecting") */
static void join_row0(uint8_t *s) {
  const uint32_t peer = *(const uint32_t *)(s + 0x1848);
  void *ni = *N[1].inst;
  if (!peer || !ni)
    return;
  void (*connect)(void *, uint32_t) = (void (*)(void *, uint32_t))(*(void ***)ni)[0x78 / 4];
  connect(ni, peer);
  *(uint32_t *)(s + 0x1878) = 0;
  *(uint32_t *)(s + 0xc) = 3;
  debugPrintf("[lan] engine 2 joins player 1's game (%u.%u.%u.%u)\n", peer >> 24, (peer >> 16) & 255,
              (peer >> 8) & 255, peer & 255);
}

/* screens player 2's copy leaves for its LOCAL menu (it is on its way to, or
 * waits at, split screen's racer select) */
static int idle_screen(const uint8_t *top) {
  static const int k[] = {VT_MAIN, VT_SOLO, VT_CHARSEL, VT_TRACKSEL, VT_GPSEL, VT_GPDIFF, VT_MISSIONSEL};
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
    if (ssr_split_is(1, top, k[i]))
      return 1;
  return 0;
}

/* player 2's copy shows the network's pop-up ("lost", "failed"): answered by
 * the script (OK: back to the list, to join again), never theirs */
int ssr_split_net_auto_popup(void) {
  uint8_t *pop = ssr_split_popup_obj(1);
  return pop && (ssr_split_is(1, pop, VT_POP_LOST) || ssr_split_is(1, pop, VT_POP_FAILED) ||
                 ssr_split_is(1, pop, VT_POP_JOIN));
}

int ssr_split_net_theirs(const uint8_t *top) {
  if (B.stage == B_OFF || !top)
    return 0;
  if (ssr_split_is(1, top, VT_LOBBY)) {
    const int st = *(const int32_t *)(top + 0xc);
    return st >= 4 && st <= 8;
  }
  return ssr_split_is(1, top, VT_HUD) || *(const uint32_t *)(top + 4) == 0x91c494eau ||
         ssr_split_is(1, top, VT_RESULTS_GP) || ssr_split_is(1, top, VT_MILES) || ssr_split_is(1, top, VT_REPORT) ||
         ssr_split_players_licence(1);
}

/* its pad, from its script: 1 if the battle's (joining, or going back) */
int ssr_split_net_script2(SsrPad *sp, uint8_t *top) {
  resolve(1);
  if (!N[1].ok || !top)
    return 0;
  const int net = ssr_split_is(1, top, VT_NET), servers = ssr_split_is(1, top, VT_SERVERS),
            lobby = ssr_split_is(1, top, VT_LOBBY);
  memset(sp, 0, sizeof *sp);
  const int ps = ssr_split_popup(1);
  if (ps < 0 || ssr_split_loading(1))
    return B.stage != B_OFF || net || servers || lobby;
  if (ps > 0) {
    uint8_t *pop = ssr_split_popup_obj(1);
    if (!(B.stage != B_OFF || net || servers || lobby))
      return 0;
    /* the lobby's own waits end by themselves; the network's "lost" /
     * "failed": OK (back to the list: to join again, or to leave) */
    if (!ssr_split_is(1, pop, VT_POP_WAITING) && !ssr_split_is(1, pop, VT_POP_CONNECTING) && pulse(1))
      sp->down = sp->held = HidNpadButton_A;
    return 1;
  }
  if (B.stage == B_OFF) { /* the battle over: out of its LOCAL screens, back where it was */
    const int mp = ssr_split_is(1, top, VT_MPSERVICE);
    if (servers && *(const int32_t *)(top + 0xc) == 2 && pulse(1))
      sp->down = sp->held = HidNpadButton_B; /* (the list's Back deletes its client) */
    else if ((net || mp) && pulse(1))
      way_back(1);
    return net || servers || lobby || mp;
  }
  if (idle_screen(top)) {
    if (!B.pushed2)
      B.pushed2 = push_netmenu(1);
    return 1;
  }
  if (net) {
    if (*(const int32_t *)(top + 0x6c) == 4 && pulse(1)) {
      const int page = N[1].car_page(top + 0xc, NULL);
      sp->down = sp->held = page == 0 ? HidNpadButton_A : HidNpadButton_Left; /* JOIN */
    }
    return 1;
  }
  if (servers) {
    if (*(const int32_t *)(top + 0xc) == 2 && host_open() && pulse(1))
      join_row0(top);
    return 1;
  }
  return lobby; /* in player 1's game: player 2's own (ssr_split.c gives it their pad) */
}

/* ------------------------------------------------------------ the frame */
/* The lobby's SELECT RACER in a column: its scenes placed as the racer
 * select's are (ssr_split.c: in the column, at its foot, the sky above),
 * but for the lobby's own -- its panel (lobby.star 0xb516e8f0) and its racer
 * picker (0x4c95c583) -- the design's x 20..940 in 96% of the column, from
 * 6% of the height down. */
static void lobby_columns(int i, int on) {
  static int was[2];
  static void (*calc[2])(void *), (*recalc[2])(void *);
  static uint8_t *(*succ[2])(uint8_t *);
  static uint8_t *sm[2];
  if (!calc[i]) {
    calc[i] = (void (*)(void *))ssr_native_in(i, "_ZN11CSceneEntry27CalculateWidescreenSettingsEv");
    recalc[i] = (void (*)(void *))ssr_native_in(i, "_ZN12SceneManager24RecalcWidescreenSettingsEv");
    succ[i] = (uint8_t * (*)(uint8_t *)) ssr_addr_in(i, 0x19d9c4, 0xe5902008u);
    sm[i] = (uint8_t *)ssr_native_in(i, "g_scene_manager");
  }
  if (!calc[i] || !recalc[i] || !succ[i] || !sm[i])
    return;
  if (!on) {
    if (was[i])
      ssr_split_place_columns(i, 0);
    was[i] = 0;
    return;
  }
  ssr_split_place_columns(i, 1);
  int w, h;
  ssr_split_wh(&w, &h);
  const float cw = (float)w * 0.5f, cx0 = i ? cw : 0.0f, k = cw * 0.96f / 920.0f;
  int placed = 0;
  char seen[256];
  int o = 0;
  seen[0] = 0;
  for (uint8_t *n = scene_first(sm[i]); n; n = succ[i](n)) {
    const uint32_t hs = scene_hash(n);
    if (!was[i] && o < 230)
      o += snprintf(seen + o, sizeof seen - (size_t)o, " %08x", (unsigned)hs);
    if (hs != 0xb516e8f0u && hs != 0x4c95c583u)
      continue;
    placed++;
    calc[i](n);
    float *f = (float *)(n + SCENE_XF + 8); /* scale x, y; offset x, y; the clip x0, y0, x1, y1 */
    f[0] = f[1] = k;
    f[2] = cx0 + (cw - 920.0f * k) * 0.5f - 20.0f * k;
    f[3] = (float)h * 0.06f;
    f[4] = cx0, f[5] = f[3], f[6] = cx0 + cw, f[7] = f[3] + 640.0f * k;
    scene_xf_derive(n);
  }
  if (!was[i])
    debugPrintf("[lan] engine %d's lobby: in its column (%d of its scenes:%s)\n", i + 1, placed, seen);
  was[i] = 1;
}

/* SELECT RACER is up: player 1's lobby is being set up or picks, or player 2
 * still does (or is not in yet); both in the lobby proper (state 8) is the
 * arena, player 1's, the whole screen */
static int columns_now(void) {
  if (B.stage != B_LOBBY || B.race)
    return 0;
  const int s1 = lobby_state(0), s2 = lobby_state(1);
  if (s1 < 1 || s1 > 8)
    return 0;
  const int p2_in = s2 >= 4 && s2 <= 8;
  return s1 <= 7 || !p2_in || s2 <= 7;
}

/* both copies' sizes: their halves (a race), else the whole screen */
static void size_both(int race) {
  int w, h;
  ssr_split_wh(&w, &h);
  if (race) { /* every frame: the scenes made since */
    ssr_split_fit_now(0, w, h / 2, 1);
    ssr_split_fit_now(1, w, h / 2, 1);
  } else if (B.sized) {
    ssr_split_fit_now(0, w, h, 0);
    ssr_split_fit_now(1, w, h, 0);
  }
  if (B.sized != race)
    debugPrintf("[lan] %s%s\n", race ? game_name() : "one screen", race ? ": each copy its half" : "");
  B.sized = race;
}

void ssr_split_net_pre(void) {
  resolve(0);
  if (B.want) {
    B.want = 0;
    if (host_game(B.type)) {
      B.pushed2 = B.joined2 = 0;
      stage(B_OPEN);
    }
  }
  ssr_race_hud_own_bar(B.stage != B_OFF && B.type == NET_RACE);
  if (B.stage == B_OFF) {
    ssr_prompt_top_half(0);
    return;
  }
  uint8_t *top = ssr_split_top(0);
  switch (B.stage) {
  case B_OPEN: /* its LOCAL menu, HOST BATTLE: the lobby */
    if (ssr_split_is(0, top, VT_LOBBY))
      stage(B_LOBBY);
    else if (ssr_split_is(0, top, VT_SOLO) && secs() > 3.0f)
      stage(B_OFF); /* backed out */
    break;
  case B_LOBBY:
    if (lobby_state(1) >= 4)
      B.joined2 = 1;
    if ((ssr_split_is(0, top, VT_NET) || ssr_split_is(0, top, VT_SOLO) || ssr_split_is(0, top, VT_MPSERVICE) ||
         ssr_split_is(0, top, VT_MAIN)) &&
        !ssr_split_loading(0))
      stage(B_LEAVE); /* player 1 left the lobby: back to the mode select (after a battle, through the
                       * menus the game made again) */
    break;
  case B_LEAVE:
    if (ssr_split_is(0, top, VT_SOLO) && !ssr_split_loading(0)) {
      stage(B_OFF);
      B.pushed2 = 0;
    } else if (secs() > 12.0f) {
      debugPrintf("[lan] the way back took too long: player 1's copy is theirs again\n");
      stage(B_OFF);
      B.pushed2 = 0;
    }
    break;
  default:
    break;
  }
  /* the screen: split while a copy races (a race's first over the line may
   * be back in the lobby before the other is: up to their clock's 30
   * seconds -- the lobby in the half) and neither shows a loading screen (a
   * loading screen is player 1's, whole) */
  const int race = B.stage == B_LOBBY && (in_race(0) || in_race(1)) && !ssr_split_loading(0) && !ssr_split_loading(1);
  if (race != B.race) {
    B.race = race;
    debugPrintf("[lan] %s: %s\n", game_name(), race ? "the screen split" : "one screen");
    if (race)
      ssr_split_race_hud2_reload(); /* its race's HUD project is its own now */
  }
  const int cols = columns_now();
  if (cols != B.cols) {
    B.cols = cols;
    debugPrintf("[lan] %s\n", cols ? "SELECT RACER: the lobbies in two columns" : "the lobby whole");
  }
  size_both(B.race);
  if (!B.race)
    lobby_columns(0, B.cols);
  ssr_prompt_columns(B.cols);
  ssr_prompt_top_half(B.race);
}

/* inside player 2's copy, before its frame: its place, its HUD, its racer */
void ssr_split_net_frame2(const SsrPad *p) {
  if (B.stage == B_OFF) {
    lobby_columns(1, 0);
    return;
  }
  uint8_t *top = ssr_split_top(1);
  if (ssr_split_players_licence(1)) { /* their licence first, in their column */
    lobby_columns(1, 0);
    ssr_split_place_columns(1, B.cols);
  } else {
    lobby_columns(1, B.cols && ssr_split_is(1, top, VT_LOBBY));
  }
  /* its HUD laid out for the bottom half from its race's start (its loading
   * screen on): the HUD slides in once, to where its keyframes are then */
  if (in_race(1))
    ssr_race_hud_in(1, SSR_HUD_BOTTOM, ssr_split_margin());
  ssr_race_frame_in(1, p, B.race && in_race(1) && !paused(1));
}

/* each copy's HUD for its half from its race's start, as a split race's is
 * from its start (not from the screen's split: the HUD slid in before it,
 * where the phone has it -- hardware, round 11) */
int ssr_split_net_hud_layout(int engine) {
  if (B.stage == B_OFF)
    return -1;
  return in_race(engine & 1) ? (engine ? SSR_HUD_BOTTOM : SSR_HUD_TOP) : SSR_HUD_PHONE;
}

/* the screen's layout for the battle (after player 2's copy ran): 1 if set */
int ssr_split_net_display(int drew) {
  (void)drew;
  if (B.stage == B_OFF)
    return 0;
  int w, h;
  ssr_split_wh(&w, &h);
  if (B.race) {
    ssr_gfx_split_source(0, 0, 0, 1, 0.5f);
    ssr_gfx_split_source(1, 0, 0, 1, 0.5f);
    ssr_gfx_split(SSR_SPLIT_RACE, NULL);
    return 1;
  }
  if (!B.cols) {
    ssr_gfx_split(SSR_SPLIT_NONE, NULL);
    return 1;
  }
  /* SELECT RACER: player 2's column, once their lobby (or their licence) is
   * up; until then a word that they come */
  uint8_t *top = ssr_split_top(1);
  if (ssr_split_net_theirs(top) && !ssr_split_loading(1)) {
    ssr_gfx_split_source(1, 0.5f, 0.0f, 1.0f, 1.0f);
    ssr_gfx_split(SSR_SPLIT_COLUMNS, NULL);
  } else {
    ssr_gfx_split(SSR_SPLIT_JOINING, NULL);
    ssr_prompt_card(SSR_CARD_JOINING, (float)w * 0.75f, (float)h * 0.5f);
  }
  return 1;
}
