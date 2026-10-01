/* dcr_config.c -- <game folder>/config.ini, the user's settings: Sonic &
 * SEGA All-Stars Racing's options, on the runtime's INI engine
 * (runtime/source/rt_cfg.c).
 *
 * The options, their order, defaults and help text are the ones earlier
 * builds wrote, so players' config.ini files read the same. Read once at
 * start-up: changes apply the next time the game starts. The file's format
 * is 2: an older file is rewritten (with this build's explanations; the
 * player's values kept), and the options whose default changed move to the
 * new one if they still had the old. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#include "dcr_config.h"
#include "rt_cfg.h"
#include "util.h"

static DcrConfig g_cfg = {
    .steering = SSR_STEER_STICK,
    .stick_tilt = 1.0f,
    .stick_curve = 1.5f,
    .motion_gain = 1.0f,
    .rumble = 1,
    .touch = 1,
    .res_w = 1280,
    .res_h = 720,
    .frame_rate = 60,
    .music_volume = 1.0f,
    .sfx_volume = 1.0f,
    .language = SSR_LANG_AUTO,
    .splash = 1,
    .intro = 1,
    .unlock_all = 1,
    .split_screen = 1,
    .player2_hud = 1,
    .boost = 1,
    .gpu_boost = 1,
    .cpu_clock = 1785,
    .optimised_renderer = 1,
    .speedups = 1,
    .anisotropy = 16,
    .frame_pacing_display = 1,
    .prompts = 1,
};

const DcrConfig *dcr_config(void) { return &g_cfg; }

/* 2: 60 fps and 1080p docked are the port's defaults now */
static const CfgMigrate k_migrate[] = {
    {"display", "frame_rate", "30", "60", 0},
    {"display", "resolution", "720", "auto", 0},
    {"performance", "optimised_renderer", "false", "true", 0},
};

/* dst NULL: derived in apply() below. */
static const CfgOpt k_opts[] = {
    {"controls", "swap_a_b", "false",
     "Swap A and B (true: B accelerates / selects, A brakes / goes back).",
     CFG_BOOL, NULL, &g_cfg.swap_ab},
    {"controls", "steering", "stick",
     "What steers in a race. stick: the left stick (the D-pad steers all the\n"
     "# way). motion: the controller's motion sensors, turned like a steering\n"
     "# wheel (the console in handheld, the Joy-Cons, a Pro Controller). both:\n"
     "# motion, and the stick adds to it. The racer is steered directly: the\n"
     "# game's tilt / slider choice and its tilt calibration do not matter.",
     CFG_CHOICE, "stick,motion,both", &g_cfg.steering},
    {"controls", "stick_sensitivity", "1.0",
     "How far the stick steers: 1 = a full stick is full lock (0.25 to 2).",
     CFG_FLOAT, NULL, &g_cfg.stick_tilt, 0.25f, 2.0f},
    {"controls", "stick_curve", "1.5",
     "The stick's response: 1 = linear, 1.5 = finer near the centre (the\n"
     "# default), up to 3.",
     CFG_FLOAT, NULL, &g_cfg.stick_curve, 1.0f, 3.0f},
    {"controls", "motion_sensitivity", "1.0",
     "Motion steering: 1 = as far as the controller is turned, 2 = twice as\n"
     "# far (0.25 to 4).",
     CFG_FLOAT, NULL, &g_cfg.motion_gain, 0.25f, 4.0f},
    {"controls", "auto_accelerate", "false",
     "Accelerate without holding A, as the phone game did: the car drives\n"
     "# on until you brake (the engine's own auto-accelerate).",
     CFG_BOOL, NULL, &g_cfg.auto_accelerate},
    {"controls", "rumble", "true",
     "Rumble in races (crashes, spin-outs, hits, rough ground: the console\n"
     "# versions' rumble, which the phone game left out) and where the phone\n"
     "# vibrated.",
     CFG_BOOL, NULL, &g_cfg.rumble},
    {"controls", "touchscreen", "true",
     "The touchscreen (handheld) works the menus as on the phone.",
     CFG_BOOL, NULL, &g_cfg.touch},
    {"display", "resolution", "auto",
     "Rendering resolution: auto (1080 if docked when the game starts, else\n"
     "# 720), 720 or 1080. The game lays itself out for either.",
     CFG_CHOICE, "720,1080,auto", NULL},
    {"display", "frame_rate", "60",
     "60: a frame every display refresh, as the console versions ran. 30: the\n"
     "# phone's pace (it drew a frame every 33 ms). The game's logic runs 60\n"
     "# steps a second either way.",
     CFG_TEXT, NULL, NULL},
    {"display", "anisotropic_filtering", "16",
     "Texture sharpness on surfaces seen at an angle (the track ahead): 16, 8,\n"
     "# 4, 2 or off (the phone's plain trilinear filtering).",
     CFG_CHOICE, "16,8,4,2,off", NULL},
    {"display", "button_prompts", "true",
     "The console editions' button prompts at the bottom right of the menus\n"
     "# (\"(+) SELECT  (B) BACK  (A) OK\"), in white.",
     CFG_BOOL, NULL, &g_cfg.prompts},
    {"display", "frame_pacing", "display",
     "display: the game's clock counts the screen's refreshes, one game step a\n"
     "# frame, no stutter. engine: its own millisecond clock, as on the phone.",
     CFG_CHOICE, "display,engine", NULL},
    {"sound", "music_volume", "100",
     "Music volume, 0 to 100 (the game's own music setting applies too).",
     CFG_FLOAT, NULL, NULL, 0.0f, 100.0f},
    {"sound", "effects_volume", "100",
     "Sound effects and voices, 0 to 100.",
     CFG_FLOAT, NULL, NULL, 0.0f, 100.0f},
    {"game", "language", "auto",
     "The game's language: auto (the console's), english, english_us, french,\n"
     "# italian, german, spanish or japanese.",
     CFG_CHOICE, "auto,english,french,italian,german,spanish,japanese,english_us", NULL},
    {"game", "unlock_all", "true",
     "Every racer, track, Grand Prix cup and mission open from the start. Your\n"
     "# save is not changed: false gives back exactly what you have unlocked.\n"
     "# (Races run in content opened this way count as your own results.)",
     CFG_BOOL, NULL, &g_cfg.unlock_all},
    {"game", "splash", "true",
     "The SEGA and Sumo Digital pictures while the game starts.",
     CFG_BOOL, NULL, &g_cfg.splash},
    {"game", "intro_movie", "true",
     "The intro movie after them (any button skips it). It comes from your\n"
     "# expansion file.",
     CFG_BOOL, NULL, &g_cfg.intro},
    {"multiplayer", "split_screen", "true",
     "Two players on one console, as on the consoles: the main menu's SPLIT\n"
     "# SCREEN (the Switch's controller screen for two first; one Joy-Con each,\n"
     "# held sideways, works), then GRAND PRIX or SINGLE RACE, both players'\n"
     "# racers side by side, and races with the AI, player 1 on top. A second\n"
     "# copy of the game runs for player 2's licence, racer select and HUD\n"
     "# (its saves in data/p2, a copy of yours at first). false: the phone's\n"
     "# LOCAL / ONLINE menus.",
     CFG_BOOL, NULL, &g_cfg.split_screen},
    {"multiplayer", "player2_hud", "true",
     "Player 2's half of a split screen race has its own HUD (their position,\n"
     "# lap and item), drawn by player 2's copy of the game. false: none (if it\n"
     "# ever gives trouble).",
     CFG_BOOL, NULL, &g_cfg.player2_hud},
    {"performance", "cpu_clock", "1785",
     "The CPU clock in MHz while the game runs: 1785 (the highest the Switch\n"
     "# uses itself, for its loading screens), 1581, 1428, 1224 or 1020 (the\n"
     "# normal clock). The game's engine was made for 1.5-2 GHz phones: 1785\n"
     "# holds 60 fps in races; lower ones use less battery. The GPU clock is not\n"
     "# affected; the HOME menu gets the normal clock. system: never touch the\n"
     "# CPU clock. An overclocking tool (sys-clk...) always comes first: once it\n"
     "# sets a clock, the game leaves the clock to it.",
     CFG_INT, "1785,1581,1428,1224,1020,system", &g_cfg.cpu_clock},
    {"performance", "gpu_boost_handheld", "true",
     "In handheld mode, the GPU at 460.8 MHz instead of 384 (a clock the system\n"
     "# offers games; more battery). Docked it runs at 768 MHz either way.",
     CFG_BOOL, NULL, &g_cfg.gpu_boost},
    {"performance", "boost_cpu_when_loading", "true",
     "CPU at 1785 MHz while the game starts (until its first picture) and\n"
     "# inside loading frames (those over 50 ms), cpu_clock otherwise.",
     CFG_BOOL, NULL, &g_cfg.boost},
    {"performance", "optimised_renderer", "true",
     "The engine's batched scene renderer (the track drawn by material, fewer\n"
     "# draw calls), which it used on the fastest 2012 phones and tablets (Nexus\n"
     "# 7, Galaxy S III...). false: its standard renderer.",
     CFG_BOOL, NULL, &g_cfg.optimised_renderer},
    {"performance", "engine_speedups", "true",
     "The engine's float maths on the CPU's floating-point unit (it was built\n"
     "# to emulate it in software), and its GL bookkeeping trimmed. false: the\n"
     "# engine exactly as built (slower).",
     CFG_BOOL, NULL, &g_cfg.speedups},
    {"debug", "gl_selftest", "false",
     "Graphics self-test picture at start-up.",
     CFG_BOOL, NULL, &g_cfg.gl_selftest},
    {"debug", "boot_log_on_screen", "false",
     "Show the start-up log on screen at every launch. Off: the log appears only\n"
     "# while something is being set up (first launch, a new APK or NRO).",
     CFG_BOOL, NULL, &g_cfg.boot_log},
    {"debug", "log_java_calls", "false",
     "Write every Java method the game calls to debug.log (slow; for bug reports).",
     CFG_BOOL, NULL, &g_cfg.log_jni},
    {"debug", "log_touches", "false",
     "Write every touch and key sent to the game, every menu press, and each\n"
     "# button the game saw hit (with its screen) to debug.log.",
     CFG_BOOL, NULL, &g_cfg.log_touch},
    /* [config] version = 2: the engine's row, last (CfgTable.version) */
};

/* After every load: what the rows' dst pointers do not fill, and the summary. */
static void apply(void) {
  g_cfg.frame_rate = atoi(rt_config_get("display", "frame_rate")) == 30 ? 30 : 60;
  g_cfg.music_volume = rt_config_float("sound", "music_volume") / 100.0f;
  g_cfg.sfx_volume = rt_config_float("sound", "effects_volume") / 100.0f;
  {
    /* auto, english, french, italian, german, spanish, japanese, english_us ->
     * DemoActivity's H: 0 en, 1 fr, 2 it, 3 de, 4 es, 5 ja, 6 en-US */
    static const int k_lang[] = {SSR_LANG_AUTO, SSR_LANG_EN, SSR_LANG_FR, SSR_LANG_IT, SSR_LANG_DE,
                                 SSR_LANG_ES, SSR_LANG_JA, SSR_LANG_EN_US};
    g_cfg.language = k_lang[rt_config_int("game", "language")];
  }
  {
    static const int k_af[] = {16, 8, 4, 2, 1};
    g_cfg.anisotropy = k_af[rt_config_int("display", "anisotropic_filtering")];
  }
  g_cfg.frame_pacing_display = rt_config_int("display", "frame_pacing") == 0;
  g_cfg.res_w = rt_config()->res_w;
  g_cfg.res_h = rt_config()->res_h;

  const char *r = rt_config_get("display", "resolution");
  const int docked = appletGetOperationMode() == AppletOperationMode_Console;
  static const char *const steer[] = {"stick", "motion", "both"};
  debugPrintf("[config] %dx%d (%s, %s) at %d fps; A/B %s; steering %s (stick x%.2f, motion x%.2f), touch %s, "
              "rumble %s; music %.0f%%, effects %.0f%%; language %d; splash %s, intro %s; CPU boost %s\n",
              g_cfg.res_w, g_cfg.res_h, r, docked ? "docked" : "handheld", g_cfg.frame_rate,
              g_cfg.swap_ab ? "swapped" : "normal", steer[g_cfg.steering], (double)g_cfg.stick_tilt,
              (double)g_cfg.motion_gain, g_cfg.touch ? "on" : "off", g_cfg.rumble ? "on" : "off",
              (double)(g_cfg.music_volume * 100), (double)(g_cfg.sfx_volume * 100), g_cfg.language,
              g_cfg.splash ? "on" : "off", g_cfg.intro ? "on" : "off", g_cfg.boost ? "on" : "off");
}

static const CfgTable k_table = {
    .opts = k_opts,
    .nopts = CFG_COUNT(k_opts),
    .migrate = k_migrate,
    .nmigrate = CFG_COUNT(k_migrate),
    .version = 2,
    .apply = apply,
};

void dcr_config_load(void) { rt_config_load(&k_table); }
