/* ssr_split_audio.c -- split screen's sound: both players heard, each sound
 * once.
 *
 * The game's sound (STAudio over iSAL, its OpenAL underneath) has one
 * listener, the game's camera (g_pGame +0x44):
 *   - a 3D sound's start (STAudio::StartAudioEvent, type 1) is quieter the
 *     further it is from the camera past 1000 units, and not played past
 *     4000 (no panning);
 *   - a character's voice line (STRacer::TriggerCHVO*, the one speech queue:
 *     STAudio::SpeechAdd) is said if the racer is within 2000 of the camera
 *     (STRacer::IsRacerInAudibleRange); the race's start and result lines
 *     are STRacing::GetHumanRacer's (one human's);
 *   - the player's engine: two loops (<character>_Min / _Max) made at the
 *     race's start for one human (STRacing::BeginGame2: STRacer::
 *     CreateHumanOnlyParts), driven by STRacer::Update for a human racer;
 *   - the rival's engine: one set of loops, a human's nearest racer's
 *     (STRacer::UpdateAudioDistanceSounds).
 * A human racer's own sounds (its boost, its items, its knocks) are started
 * in its update, while the game's camera is its player's (ssr_split_race.c).
 *
 * A SPLIT RACE (Grand Prix, single race: one race in player 1's copy):
 *   - player 2's racer gets the player's engine too, made with player 1's at
 *     the race's start (STRacer::Update then drives both humans' alike);
 *   - a 3D sound is heard from the nearer player's camera (Mario Kart's
 *     listeners: an item's hit beside player 2 as loud as one beside player 1);
 *   - a racer near either player's camera says its lines (player 2's
 *     character with them), one after another in the speech queue; the
 *     start's and the result's lines are both players' characters';
 *   - the rival's engine is the human's whose nearest racer is nearer.
 * BATTLE / VS RACE (both copies race, each from its player's camera, their
 * sound mixed: ssr_split.c):
 *   - a 3D sound only in the copy whose player is nearer to it; each copy's
 *     voice lines only its own player's; the race's own sounds (the
 *     countdown: STGameHud; the clock's beeps, the thunder: STRacing) only
 *     player 1's copy's. The music is player 1's copy's (ssr_java.c).
 * MIT.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ssr.h"
#include "ssr_split_priv.h"
#include "util.h"

void *ssr_patch_hook(const char *sym, uint32_t expect, void *dst); /* ssr_patch.c, at load */
int ssr_patch_engine_index(void);
int ssr_split_session(void); /* ssr_split.c */
void *ssr_split_race_cam1(void);
uint8_t *ssr_split_race_racer(int k);
unsigned ssr_split_race_gen(void);

/* STRacer: +0x10 its place (x, y, z: 20.12), +0x99 counts as a rival (the
 * rival's engine), +0x918 its kind (0 a human, 1 AI, 2 the network's),
 * +0xc60 its engine loop (a human's). STGameCamera: +0x28 its place. */
#define RACER_POS 0x10
#define CAM_POS 0x28
#define VOICE_RANGE 0x7d0000 /* 2000 units: IsRacerInAudibleRange's */

typedef struct {
  int ok;
  uintptr_t base; /* the library's load address (callers' addresses) */
  uint8_t **game, **racers;
  void (*chop)(uint8_t *racer);                                              /* CreateHumanOnlyParts */
  void (*start3d)(void *au, uint32_t ev, uint32_t grp, int type, const int32_t *pos); /* StartAudioEvent */
  int (*in_range)(uint8_t *racer);                                           /* IsRacerInAudibleRange */
  void (*intro)(uint8_t *racer);                                             /* TriggerCHVOIntro */
  void (*winlose)(uint8_t *racer, int win);                                  /* TriggerCHVOWinLose */
  void (*distance)(uint8_t *racer);                                          /* UpdateAudioDistanceSounds */
  int (*ev_start)(uint32_t ev, uint32_t grp, int loop);                      /* iSAudioInterface::StartEvent */
  int (*ev_trigger)(uint32_t ev, uint32_t grp);                              /* iSAudioInterface::TriggerEvent */
} Au;
static Au A[2];

static struct {
  unsigned heard_by_p2, other_copys, voices_p2, race_sounds_once;
} g_stat;

static Au *au(void) {
  const int e = ssr_engine_current() & 1;
  Au *a = &A[e];
  if (!a->ok) {
    a->ok = 1;
    a->game = (uint8_t **)ssr_native_in(e, "g_pGame");
    a->racers = (uint8_t **)ssr_native_in(e, "_ZN8STRacing9ms_pRacerE");
    uint8_t *render = (uint8_t *)ssr_native_in(e, "_Z6renderv");
    a->base = render ? (uintptr_t)render - 0x1f1320 : 0;
  }
  return a;
}

static int64_t absd(int64_t v) { return v < 0 ? -v : v; }
/* the largest of the three distances (the game's own measure) */
static int64_t cheb(const int32_t *a, const int32_t *b) {
  int64_t d = absd((int64_t)a[0] - b[0]), t = absd((int64_t)a[1] - b[1]);
  d = t > d ? t : d;
  t = absd((int64_t)a[2] - b[2]);
  return t > d ? t : d;
}
static const int32_t *racer_pos(const uint8_t *r) { return (const int32_t *)(r + RACER_POS); }
static const int32_t *cam_pos(const uint8_t *c) { return (const int32_t *)(c + CAM_POS); }
static int kind(const uint8_t *r) { return *(const int32_t *)(r + 0x918); }

/* this copy's race's racers: n, and the human who is not `not` (NULL) */
static int racers_n(const Au *a) {
  const uint8_t *game = a->game ? *a->game : NULL;
  const int n = game ? *(const int32_t *)(game + 0x764) : 0;
  return a->racers && n > 0 ? (n > 8 ? 8 : n) : 0;
}
static uint8_t *other_human(const Au *a, const uint8_t *not) {
  const int n = racers_n(a);
  for (int i = 0; i < n; i++) {
    uint8_t *r = a->racers[i];
    if (r && r != not && kind(r) == 0)
      return r;
  }
  return NULL;
}
/* a LAN game's two players in this copy: its own (a human), the other's
 * (the network's) */
static int lan_players(const Au *a, uint8_t **own, uint8_t **other) {
  *own = *other = NULL;
  const int n = racers_n(a);
  for (int i = 0; i < n; i++) {
    uint8_t *r = a->racers[i];
    if (!r)
      continue;
    if (kind(r) == 0 && !*own)
      *own = r;
    else if (kind(r) == 2 && !*other)
      *other = r;
  }
  return *own && *other;
}

static int split_race(void) { return ssr_engine_current() == 0 && ssr_split_race_active(); }

/* ------------------------------------------------------------ the engine */
/* STRacer::CreateHumanOnlyParts (at the race's start, for one human): a split
 * race's other human's too */
static void chop_hook(uint8_t *racer) {
  Au *a = au();
  a->chop(racer);
  if (ssr_engine_current() != 0 || !ssr_split_session() || !racer || kind(racer) != 0)
    return;
  uint8_t *r2 = other_human(a, racer);
  if (r2 && !*(const uint32_t *)(r2 + 0xc60)) {
    a->chop(r2);
    debugPrintf("[sound] player 2's racer: the player's engine too (the game makes it for one human)\n");
  }
}

/* the nearest rival's distance to a racer (UpdateAudioDistanceSounds' rivals) */
static int64_t nearest_rival(const Au *a, const uint8_t *h) {
  int64_t best = INT64_MAX;
  const int n = racers_n(a);
  for (int i = 0; i < n; i++) {
    const uint8_t *r = a->racers[i];
    if (r && r != h && r[0x99]) {
      const int64_t d = cheb(racer_pos(h), racer_pos(r));
      best = d < best ? d : best;
    }
  }
  return best;
}

/* STRacer::UpdateAudioDistanceSounds (a human's, each tick): one set of loops
 * -- in a split race the human's whose nearest rival is nearer (not both,
 * each tick overwriting the other's) */
static void distance_hook(uint8_t *racer) {
  Au *a = au();
  if (split_race()) {
    uint8_t *r1 = ssr_split_race_racer(0), *r2 = ssr_split_race_racer(1);
    if (r1 && r2 && (racer == r1 || racer == r2)) {
      const int64_t mine = nearest_rival(a, racer), theirs = nearest_rival(a, racer == r1 ? r2 : r1);
      if (theirs < mine || (theirs == mine && racer == r2))
        return;
    }
  }
  a->distance(racer);
}

/* ------------------------------------------------------------ 3D sounds */
/* STAudio::StartAudioEvent: a 3D sound's volume from the nearer player's
 * camera (a split race), or only in the copy whose player is nearer (LAN) */
static void start3d_hook(void *self, uint32_t ev, uint32_t grp, int type, const int32_t *pos) {
  Au *a = au();
  uint8_t *game = a->game ? *a->game : NULL;
  if (type == 1 && pos && game) {
    if (split_race()) {
      uint8_t *c1 = (uint8_t *)ssr_split_race_cam1(), *c2 = (uint8_t *)ssr_split_race_cam2();
      if (c1 && c2) {
        uint8_t *near = cheb(pos, cam_pos(c2)) < cheb(pos, cam_pos(c1)) ? c2 : c1;
        uint8_t **cam = (uint8_t **)(game + 0x44), *was = *cam;
        g_stat.heard_by_p2 += near == c2 && was != c2;
        *cam = near;
        a->start3d(self, ev, grp, type, pos);
        *cam = was;
        return;
      }
    } else if (ssr_split_net_racing()) {
      uint8_t *own, *other;
      if (lan_players(a, &own, &other) && cheb(pos, racer_pos(other)) < cheb(pos, racer_pos(own))) {
        g_stat.other_copys++;
        return; /* the other player's: their copy plays it */
      }
    }
  }
  a->start3d(self, ev, grp, type, pos);
}

/* ------------------------------------------------------------ voices */
/* STRacer::IsRacerInAudibleRange: near either player's camera (a split
 * race); in a LAN game each copy says only its own player's lines */
static int in_range_hook(uint8_t *racer) {
  Au *a = au();
  if (split_race()) {
    uint8_t *c1 = (uint8_t *)ssr_split_race_cam1(), *c2 = (uint8_t *)ssr_split_race_cam2();
    if (c1 && c2 && racer) {
      const int near1 = cheb(racer_pos(racer), cam_pos(c1)) <= VOICE_RANGE;
      const int near2 = cheb(racer_pos(racer), cam_pos(c2)) <= VOICE_RANGE;
      g_stat.voices_p2 += near2 && !near1;
      return near1 || near2;
    }
  } else if (ssr_split_net_racing() && racer && kind(racer) != 0) {
    return 0; /* the other player's: their copy says it */
  }
  return a->in_range(racer);
}

/* STRacer::TriggerCHVOIntro (the race's start: GetHumanRacer's): a split
 * race's other human's line after it */
static void intro_hook(uint8_t *racer) {
  Au *a = au();
  a->intro(racer);
  if (ssr_engine_current() == 0 && ssr_split_session() && racer && kind(racer) == 0) {
    uint8_t *r2 = other_human(a, racer);
    if (r2)
      a->intro(r2);
  }
}

/* STRacer::TriggerCHVOWinLose (STRaceResultsMenu::Enter: GetHumanRacer's
 * line, a win its place 1 -- 0x166d24 -- or on the podium -- 0x166e10, a Grand
 * Prix's): in a split race player 1's character's line, then player 2's
 * (their place, the same test), once a race (the race ends again for each
 * human over the line) */
static void winlose_hook(uint8_t *racer, int win) {
  Au *a = au();
  if (split_race()) {
    static unsigned said; /* the race (ssr_split_race_gen) whose lines were said */
    const uintptr_t site = (uintptr_t)__builtin_return_address(0) - a->base;
    uint8_t *r1 = ssr_split_race_racer(0), *r2 = ssr_split_race_racer(1);
    if (said == ssr_split_race_gen())
      return;
    said = ssr_split_race_gen();
    if (r1)
      a->winlose(r1, win);
    if (r2) {
      const int p = *(const int32_t *)(r2 + 0xc54);
      a->winlose(r2, site >= 0x166e00 && site < 0x166e20 ? p >= 0 && p <= 3 : p == 1);
    }
    return;
  }
  a->winlose(racer, win);
}

/* ------------------------------------------------------------ the race's own */
/* a caller in the race's own sounds' code: STGameHud (the countdown),
 * STRacing::Update(Net)TimeRemaining (the clock's beeps), STRacing::Update
 * (the thunder) */
static int race_flow(const Au *a, uintptr_t lr) {
  const uintptr_t o = lr - a->base;
  return (o >= 0x161190 && o < 0x162938) || (o >= 0x153704 && o < 0x153a4c) || (o >= 0x153c60 && o < 0x1544ec);
}

static int ev_start_hook(uint32_t ev, uint32_t grp, int loop) {
  Au *a = au();
  if (ssr_engine_current() == 1 && ssr_split_net_racing() && race_flow(a, (uintptr_t)__builtin_return_address(0))) {
    g_stat.race_sounds_once++;
    return 0; /* player 1's copy's */
  }
  return a->ev_start(ev, grp, loop);
}

static int ev_trigger_hook(uint32_t ev, uint32_t grp) {
  Au *a = au();
  if (ssr_engine_current() == 1 && ssr_split_net_racing() && race_flow(a, (uintptr_t)__builtin_return_address(0))) {
    g_stat.race_sounds_once++;
    return 0;
  }
  return a->ev_trigger(ev, grp);
}

/* ------------------------------------------------------------ set up */
/* at each copy's load (ssr_split.c: ssr_split_patch) */
void ssr_split_audio_patch(void) {
  Au *a = &A[ssr_patch_engine_index() & 1];
  int n = 0;
#define HOOK(field, sym, expect, fn) n += (*(void **)&a->field = ssr_patch_hook(sym, expect, (void *)fn)) != NULL
  HOOK(chop, "_ZN7STRacer20CreateHumanOnlyPartsEv", 0xe92d47f0u, chop_hook);
  HOOK(start3d, "_ZN7STAudio15StartAudioEventEjj19STAudioPositionType7STVec32", 0xe92d41f0u, start3d_hook);
  HOOK(in_range, "_ZN7STRacer21IsRacerInAudibleRangeEv", 0xe59f3078u, in_range_hook);
  HOOK(intro, "_ZN7STRacer16TriggerCHVOIntroEv", 0xe92d47f0u, intro_hook);
  HOOK(winlose, "_ZN7STRacer18TriggerCHVOWinLoseEb", 0xe92d41f0u, winlose_hook);
  HOOK(distance, "_ZN7STRacer25UpdateAudioDistanceSoundsEv", 0xe92d4ff0u, distance_hook);
  HOOK(ev_start, "_ZN16iSAudioInterface10StartEventEjjb", 0xe92d41f0u, ev_start_hook);
  HOOK(ev_trigger, "_ZN16iSAudioInterface12TriggerEventEjj", 0xe92d41f0u, ev_trigger_hook);
#undef HOOK
  debugPrintf("[sound] engine %d: split screen's sound, %d of 8 hooks\n", ssr_patch_engine_index() + 1, n);
}

/* (the report, every 10 s while it has news) */
void ssr_split_audio_report(void) {
  if (g_stat.heard_by_p2 || g_stat.other_copys || g_stat.voices_p2 || g_stat.race_sounds_once)
    debugPrintf("[sound] heard from player 2's camera %u; left to the other copy %u; voice lines by player 2's "
                "camera %u; the race's own sounds once %u\n",
                g_stat.heard_by_p2, g_stat.other_copys, g_stat.voices_p2, g_stat.race_sounds_once);
  memset(&g_stat, 0, sizeof g_stat);
}
