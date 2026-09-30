/* dcr_config.c -- <game folder>/config.ini, the user's settings.
 *
 * Written with every option, its default and a line of explanation on the
 * first start; an existing file is appended to (options a newer build adds,
 * at the end, with their defaults), so edits and comments survive updates.
 * Plain INI: [section], key = value, # comments; booleans take true/false,
 * yes/no, on/off, 1/0. Read once at start-up: changes apply the next time the
 * game starts. (The machinery is the Crossy Road port's; the options are
 * Sonic & SEGA All-Stars Racing's.) MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>

#include "dcr_build.h"
#include "dcr_config.h"
#include "util.h"

const char *dcr_game_root(void);        /* main.c */
void dcr_window_set_size(int w, int h); /* android_ndk.c */

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

enum { K_BOOL, K_CHOICE, K_TEXT };

/* The file's format. A file of an older format is rewritten (with this
 * build's explanations; the player's values kept), and the options whose
 * default changed move to the new one if they still had the old. */
#define CFG_VERSION "2"
static const struct {
  const char *section, *key, *from, *to;
} k_migrate[] = {
    /* 2: 60 fps and 1080p docked are the port's defaults now */
    {"display", "frame_rate", "30", "60"},
    {"display", "resolution", "720", "auto"},
    {"performance", "optimised_renderer", "false", "true"},
};

typedef struct {
  const char *section, *key, *def, *help;
  int kind;
  const char *choices; /* K_CHOICE: comma-separated, index = value */
} Opt;

static const Opt k_opts[] = {
    {"controls", "swap_a_b", "false",
     "Swap A and B (true: B accelerates / selects, A brakes / goes back).", K_BOOL, NULL},
    {"controls", "steering", "stick",
     "What steers in a race. stick: the left stick (the D-pad steers all the\n"
     "# way). motion: the controller's motion sensors, turned like a steering\n"
     "# wheel (the console in handheld, the Joy-Cons, a Pro Controller). both:\n"
     "# motion, and the stick adds to it. The racer is steered directly: the\n"
     "# game's tilt / slider choice and its tilt calibration do not matter.",
     K_CHOICE, "stick,motion,both"},
    {"controls", "stick_sensitivity", "1.0",
     "How far the stick steers: 1 = a full stick is full lock (0.25 to 2).", K_TEXT, NULL},
    {"controls", "stick_curve", "1.5",
     "The stick's response: 1 = linear, 1.5 = finer near the centre (the\n"
     "# default), up to 3.", K_TEXT, NULL},
    {"controls", "motion_sensitivity", "1.0",
     "Motion steering: 1 = as far as the controller is turned, 2 = twice as\n"
     "# far (0.25 to 4).", K_TEXT, NULL},
    {"controls", "auto_accelerate", "false",
     "Accelerate without holding A, as the phone game did: the car drives\n"
     "# on until you brake (the engine's own auto-accelerate).", K_BOOL, NULL},
    {"controls", "rumble", "true",
     "Rumble in races (crashes, spin-outs, hits, rough ground: the console\n"
     "# versions' rumble, which the phone game left out) and where the phone\n"
     "# vibrated.", K_BOOL, NULL},
    {"controls", "touchscreen", "true",
     "The touchscreen (handheld) works the menus as on the phone.", K_BOOL, NULL},
    {"display", "resolution", "auto",
     "Rendering resolution: auto (1080 if docked when the game starts, else\n"
     "# 720), 720 or 1080. The game lays itself out for either.", K_CHOICE, NULL},
    {"display", "frame_rate", "60",
     "60: a frame every display refresh, as the console versions ran. 30: the\n"
     "# phone's pace (it drew a frame every 33 ms). The game's logic runs 60\n"
     "# steps a second either way.", K_CHOICE, NULL},
    {"display", "anisotropic_filtering", "16",
     "Texture sharpness on surfaces seen at an angle (the track ahead): 16, 8,\n"
     "# 4, 2 or off (the phone's plain trilinear filtering).",
     K_CHOICE, "16,8,4,2,off"},
    {"display", "button_prompts", "true",
     "The console editions' button prompts at the bottom right of the menus\n"
     "# (\"(+) SELECT  (B) BACK  (A) OK\"), in white.", K_BOOL, NULL},
    {"display", "frame_pacing", "display",
     "display: the game's clock counts the screen's refreshes, one game step a\n"
     "# frame, no stutter. engine: its own millisecond clock, as on the phone.",
     K_CHOICE, "display,engine"},
    {"sound", "music_volume", "100", "Music volume, 0 to 100 (the game's own music setting applies too).",
     K_TEXT, NULL},
    {"sound", "effects_volume", "100", "Sound effects and voices, 0 to 100.", K_TEXT, NULL},
    {"game", "language", "auto",
     "The game's language: auto (the console's), english, english_us, french,\n"
     "# italian, german, spanish or japanese.",
     K_CHOICE, "auto,english,french,italian,german,spanish,japanese,english_us"},
    {"game", "unlock_all", "true",
     "Every racer, track, Grand Prix cup and mission open from the start. Your\n"
     "# save is not changed: false gives back exactly what you have unlocked.\n"
     "# (Races run in content opened this way count as your own results.)",
     K_BOOL, NULL},
    {"game", "splash", "true", "The SEGA and Sumo Digital pictures while the game starts.", K_BOOL, NULL},
    {"game", "intro_movie", "true",
     "The intro movie after them (any button skips it). It comes from your\n"
     "# expansion file.", K_BOOL, NULL},
    {"multiplayer", "split_screen", "true",
     "Two players on one console, as on the consoles: the main menu's SPLIT\n"
     "# SCREEN (the Switch's controller screen for two first; one Joy-Con each,\n"
     "# held sideways, works), then GRAND PRIX or SINGLE RACE, both players'\n"
     "# racers side by side, and races with the AI, player 1 on top. A second\n"
     "# copy of the game runs for player 2's licence, racer select and HUD\n"
     "# (its saves in data/p2, a copy of yours at first). false: the phone's\n"
     "# LOCAL / ONLINE menus.",
     K_BOOL, NULL},
    {"multiplayer", "player2_hud", "true",
     "Player 2's half of a split screen race has its own HUD (their position,\n"
     "# lap and item), drawn by player 2's copy of the game. false: none (if it\n"
     "# ever gives trouble).",
     K_BOOL, NULL},
    {"performance", "cpu_clock", "1785",
     "The CPU clock in MHz while the game runs: 1785 (the highest the Switch\n"
     "# uses itself, for its loading screens), 1581, 1428, 1224 or 1020 (the\n"
     "# normal clock). The game's engine was made for 1.5-2 GHz phones: 1785\n"
     "# holds 60 fps in races; lower ones use less battery. The GPU clock is not\n"
     "# affected; the HOME menu gets the normal clock.",
     K_CHOICE, "1785,1581,1428,1224,1020"},
    {"performance", "gpu_boost_handheld", "true",
     "In handheld mode, the GPU at 460.8 MHz instead of 384 (a clock the system\n"
     "# offers games; more battery). Docked it runs at 768 MHz either way.",
     K_BOOL, NULL},
    {"performance", "boost_cpu_when_loading", "true",
     "CPU at 1785 MHz while the game starts (until its first picture) and\n"
     "# inside loading frames (those over 50 ms), cpu_clock otherwise.",
     K_BOOL, NULL},
    {"performance", "optimised_renderer", "true",
     "The engine's batched scene renderer (the track drawn by material, fewer\n"
     "# draw calls), which it used on the fastest 2012 phones and tablets (Nexus\n"
     "# 7, Galaxy S III...). false: its standard renderer.",
     K_BOOL, NULL},
    {"performance", "engine_speedups", "true",
     "The engine's float maths on the CPU's floating-point unit (it was built\n"
     "# to emulate it in software), and its GL bookkeeping trimmed. false: the\n"
     "# engine exactly as built (slower).",
     K_BOOL, NULL},
    {"debug", "gl_selftest", "false", "Graphics self-test picture at start-up.", K_BOOL, NULL},
    {"debug", "boot_log_on_screen", "false",
     "Show the start-up log on screen at every launch. Off: the log appears only\n"
     "# while something is being set up (first launch, a new APK or NRO).",
     K_BOOL, NULL},
    {"debug", "log_java_calls", "false",
     "Write every Java method the game calls to debug.log (slow; for bug reports).", K_BOOL,
     NULL},
    {"debug", "log_touches", "false",
     "Write every touch and key sent to the game, every menu press, and each\n"
     "# button the game saw hit (with its screen) to debug.log.", K_BOOL, NULL},
    {"config", "version", CFG_VERSION, "Settings file format; leave as it is.", K_TEXT, NULL},
};
#define O_COUNT ((int)(sizeof k_opts / sizeof k_opts[0]))

static char g_val[O_COUNT][48];
static int g_have[O_COUNT];

static int opt_index(const char *section, const char *key) {
  for (int i = 0; i < O_COUNT; i++)
    if (!strcmp(k_opts[i].section, section) && !strcmp(k_opts[i].key, key))
      return i;
  return -1;
}

static void path_of(char *out, size_t cap, const char *name) {
  snprintf(out, cap, "%s/%s", dcr_game_root(), name);
}

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
    *--e = 0;
  return s;
}

static void parse(FILE *f) {
  char line[256], section[32] = "";
  while (fgets(line, sizeof line, f)) {
    char *s = trim(line);
    if (!*s || *s == '#' || *s == ';')
      continue;
    if (*s == '[') {
      char *e = strchr(s, ']');
      if (e) {
        *e = 0;
        snprintf(section, sizeof section, "%s", trim(s + 1));
      }
      continue;
    }
    char *eq = strchr(s, '=');
    if (!eq)
      continue;
    *eq = 0;
    char *key = trim(s), *val = trim(eq + 1);
    char *hash = strpbrk(val, "#;");
    if (hash) {
      *hash = 0;
      val = trim(val);
    }
    for (int i = 0; i < O_COUNT; i++)
      if (!strcasecmp(section, k_opts[i].section) && !strcasecmp(key, k_opts[i].key)) {
        snprintf(g_val[i], sizeof g_val[i], "%s", val);
        g_have[i] = 1;
      }
  }
}

static void write_opts(FILE *f, int only_missing) {
  const char *last = NULL;
  for (int i = 0; i < O_COUNT; i++) {
    if (only_missing && g_have[i])
      continue;
    if (!last || strcmp(last, k_opts[i].section)) {
      fprintf(f, "\n[%s]\n", k_opts[i].section);
    }
    last = k_opts[i].section;
    if (k_opts[i].help)
      fprintf(f, "# %s\n", k_opts[i].help);
    fprintf(f, "%s = %s\n", k_opts[i].key, g_val[i]);
  }
}

static int as_bool(int i) {
  const char *v = g_val[i];
  if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1"))
    return 1;
  if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0"))
    return 0;
  debugPrintf("[config] %s = %s: not true/false, using %s\n", k_opts[i].key, v, k_opts[i].def);
  return !strcmp(k_opts[i].def, "true");
}

/* index of the value in the option's choice list, 0 (the first) if unknown */
static int as_choice(int i) {
  const char *v = g_val[i];
  const char *c = k_opts[i].choices;
  for (int idx = 0; c && *c; idx++) {
    const char *e = strchr(c, ',');
    size_t n = e ? (size_t)(e - c) : strlen(c);
    if (strlen(v) == n && !strncasecmp(v, c, n))
      return idx;
    if (!e)
      break;
    c = e + 1;
  }
  if (strcasecmp(v, k_opts[i].def))
    debugPrintf("[config] %s = %s: not one of %s, using %s\n", k_opts[i].key, v,
                k_opts[i].choices, k_opts[i].def);
  return 0;
}

static float as_float(int i, float lo, float hi) {
  float v = (float)atof(g_val[i]);
  if (!(v >= lo && v <= hi)) {
    debugPrintf("[config] %s = %s: not %g..%g, using %s\n", k_opts[i].key, g_val[i], lo, hi,
                k_opts[i].def);
    v = (float)atof(k_opts[i].def);
  }
  return v;
}

void dcr_config_load(void) {
  for (int i = 0; i < O_COUNT; i++)
    snprintf(g_val[i], sizeof g_val[i], "%s", k_opts[i].def);
  char path[300];
  path_of(path, sizeof path, "config.ini");
  FILE *f = fopen(path, "r");
  if (f) {
    parse(f);
    fclose(f);
    const int iv = opt_index("config", "version");
    if (atoi(g_val[iv]) < atoi(CFG_VERSION) || !g_have[iv]) {
      char changed[160] = "";
      for (unsigned m = 0; m < sizeof k_migrate / sizeof k_migrate[0]; m++) {
        const int i = opt_index(k_migrate[m].section, k_migrate[m].key);
        if (i >= 0 && !strcasecmp(g_val[i], k_migrate[m].from)) {
          snprintf(g_val[i], sizeof g_val[i], "%s", k_migrate[m].to);
          snprintf(changed + strlen(changed), sizeof changed - strlen(changed), " %s %s -> %s;",
                   k_migrate[m].key, k_migrate[m].from, k_migrate[m].to);
        }
      }
      snprintf(g_val[iv], sizeof g_val[iv], "%s", CFG_VERSION);
      if ((f = fopen(path, "w"))) {
        fputs("# Sonic & SEGA All-Stars Racing for Switch -- settings.\n"
              "# Changes apply the next time the game starts. Delete this file to get\n"
              "# the defaults back.\n",
              f);
        write_opts(f, 0);
        fclose(f);
      }
      for (int i = 0; i < O_COUNT; i++)
        g_have[i] = 1;
      debugPrintf("[config] config.ini updated to format %s (your settings kept):%s\n", CFG_VERSION,
                  changed[0] ? changed : " no values changed");
    }
    int missing = 0;
    for (int i = 0; i < O_COUNT; i++)
      missing += !g_have[i];
    if (missing && (f = fopen(path, "a"))) {
      fprintf(f, "\n# Added by build %llu (new options, at their defaults):\n",
              (unsigned long long)DCR_BUILD);
      write_opts(f, 1);
      fclose(f);
      debugPrintf("[config] added %d new option%s to config.ini\n", missing, missing > 1 ? "s" : "");
    }
  } else if ((f = fopen(path, "w"))) {
    fputs("# Sonic & SEGA All-Stars Racing for Switch -- settings.\n"
          "# Changes apply the next time the game starts. Delete this file to get\n"
          "# the defaults back.\n",
          f);
    write_opts(f, 0);
    fclose(f);
    debugPrintf("[config] wrote config.ini with the defaults\n");
  }

  g_cfg.swap_ab = as_bool(opt_index("controls", "swap_a_b"));
  g_cfg.steering = as_choice(opt_index("controls", "steering"));
  g_cfg.stick_tilt = as_float(opt_index("controls", "stick_sensitivity"), 0.25f, 2.0f);
  g_cfg.stick_curve = as_float(opt_index("controls", "stick_curve"), 1.0f, 3.0f);
  g_cfg.motion_gain = as_float(opt_index("controls", "motion_sensitivity"), 0.25f, 4.0f);
  g_cfg.rumble = as_bool(opt_index("controls", "rumble"));
  g_cfg.auto_accelerate = as_bool(opt_index("controls", "auto_accelerate"));
  g_cfg.touch = as_bool(opt_index("controls", "touchscreen"));
  g_cfg.frame_rate = atoi(g_val[opt_index("display", "frame_rate")]) == 30 ? 30 : 60;
  g_cfg.music_volume = as_float(opt_index("sound", "music_volume"), 0.0f, 100.0f) / 100.0f;
  g_cfg.sfx_volume = as_float(opt_index("sound", "effects_volume"), 0.0f, 100.0f) / 100.0f;
  {
    /* auto, english, french, italian, german, spanish, japanese, english_us ->
     * DemoActivity's H: 0 en, 1 fr, 2 it, 3 de, 4 es, 5 ja, 6 en-US */
    static const int k_lang[] = {SSR_LANG_AUTO, SSR_LANG_EN, SSR_LANG_FR, SSR_LANG_IT, SSR_LANG_DE,
                                 SSR_LANG_ES, SSR_LANG_JA, SSR_LANG_EN_US};
    g_cfg.language = k_lang[as_choice(opt_index("game", "language"))];
  }
  g_cfg.splash = as_bool(opt_index("game", "splash"));
  g_cfg.intro = as_bool(opt_index("game", "intro_movie"));
  g_cfg.unlock_all = as_bool(opt_index("game", "unlock_all"));
  g_cfg.split_screen = as_bool(opt_index("multiplayer", "split_screen"));
  g_cfg.player2_hud = as_bool(opt_index("multiplayer", "player2_hud"));
  g_cfg.boost = as_bool(opt_index("performance", "boost_cpu_when_loading"));
  {
    static const int k_mhz[] = {1785, 1581, 1428, 1224, 1020};
    g_cfg.cpu_clock = k_mhz[as_choice(opt_index("performance", "cpu_clock"))];
  }
  g_cfg.gpu_boost = as_bool(opt_index("performance", "gpu_boost_handheld"));
  g_cfg.optimised_renderer = as_bool(opt_index("performance", "optimised_renderer"));
  g_cfg.speedups = as_bool(opt_index("performance", "engine_speedups"));
  {
    static const int k_af[] = {16, 8, 4, 2, 1};
    g_cfg.anisotropy = k_af[as_choice(opt_index("display", "anisotropic_filtering"))];
  }
  g_cfg.frame_pacing_display = as_choice(opt_index("display", "frame_pacing")) == 0;
  g_cfg.prompts = as_bool(opt_index("display", "button_prompts"));
  g_cfg.gl_selftest = as_bool(opt_index("debug", "gl_selftest"));
  g_cfg.boot_log = as_bool(opt_index("debug", "boot_log_on_screen"));
  g_cfg.log_jni = as_bool(opt_index("debug", "log_java_calls"));
  g_cfg.log_touch = as_bool(opt_index("debug", "log_touches"));

  const char *r = g_val[opt_index("display", "resolution")];
  int docked = appletGetOperationMode() == AppletOperationMode_Console;
  int h = !strcmp(r, "720") ? 720 : !strcmp(r, "1080") ? 1080 : !strcasecmp(r, "auto") ? (docked ? 1080 : 720) : 0;
  if (!h) {
    debugPrintf("[config] resolution = %s: not auto, 720 or 1080, using auto\n", r);
    h = docked ? 1080 : 720;
  }
  g_cfg.res_h = h;
  g_cfg.res_w = h * 16 / 9;
  dcr_window_set_size(g_cfg.res_w, g_cfg.res_h);

  static const char *const steer[] = {"stick", "motion", "both"};
  debugPrintf("[config] %dx%d (%s, %s) at %d fps; A/B %s; steering %s (stick x%.2f, motion x%.2f), touch %s, "
              "rumble %s; music %.0f%%, effects %.0f%%; language %d; splash %s, intro %s; CPU boost %s\n",
              g_cfg.res_w, g_cfg.res_h, r, docked ? "docked" : "handheld", g_cfg.frame_rate,
              g_cfg.swap_ab ? "swapped" : "normal", steer[g_cfg.steering], (double)g_cfg.stick_tilt,
              (double)g_cfg.motion_gain, g_cfg.touch ? "on" : "off", g_cfg.rumble ? "on" : "off",
              (double)(g_cfg.music_volume * 100), (double)(g_cfg.sfx_volume * 100), g_cfg.language,
              g_cfg.splash ? "on" : "off", g_cfg.intro ? "on" : "off", g_cfg.boost ? "on" : "off");
}
