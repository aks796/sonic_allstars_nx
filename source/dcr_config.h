/* dcr_config.h -- the user's settings, from <game folder>/config.ini (dcr_config.c). */
#ifndef DCR_USER_CONFIG_H
#define DCR_USER_CONFIG_H

/* [controls] steering: what steers when the game steers by tilting the phone. */
enum { SSR_STEER_STICK,  /* the left stick (and the D-pad) */
       SSR_STEER_MOTION, /* the controller's motion sensors, turned like a wheel */
       SSR_STEER_BOTH    /* motion, with the stick adding to it */ };

/* [game] language: the one DemoActivity took from the phone's locale. */
enum { SSR_LANG_AUTO = -1, SSR_LANG_EN = 0, SSR_LANG_FR = 1, SSR_LANG_IT = 2, SSR_LANG_DE = 3, SSR_LANG_ES = 4,
       SSR_LANG_JA = 5, SSR_LANG_EN_US = 6 };

typedef struct {
  int swap_ab;           /* [controls] swap_a_b */
  int steering;          /* [controls] steering: SSR_STEER_* */
  float stick_tilt;      /* [controls] stick_sensitivity: x the tilt a full stick gives */
  float stick_curve;     /* [controls] stick_curve: the response exponent (1 = linear) */
  float motion_gain;     /* [controls] motion_sensitivity: x the controller's own turn */
  int rumble;            /* [controls] rumble: the game's vibration */
  int touch;             /* [controls] touchscreen: the game's own touch controls */
  int auto_accelerate;   /* [controls] auto_accelerate: the engine's own (it drives on until you brake) */
  int res_w, res_h;      /* [display] resolution */
  int frame_rate;        /* [display] frame_rate: 30 (as on the phone) or 60 */
  float music_volume;    /* [sound] music_volume, 0..1 */
  float sfx_volume;      /* [sound] effects_volume, 0..1 */
  int language;          /* [game] language: SSR_LANG_* */
  int splash;            /* [game] splash: the SEGA / Sumo Digital pictures */
  int intro;             /* [game] intro_movie */
  int unlock_all;        /* [game] unlock_all: every racer, track, cup, mission (save untouched) */
  int split_screen;      /* [multiplayer] split_screen: the main menu's SPLIT SCREEN (ssr_split.c) */
  int player2_hud;       /* [multiplayer] player2_hud: player 2's HUD, from player 2's copy (ssr_split_race.c) */
  int boost;             /* [performance] boost_cpu_when_loading */
  int gpu_boost;         /* [performance] gpu_boost_handheld: 460.8 MHz handheld */
  int cpu_clock;         /* [performance] cpu_clock, MHz */
  int optimised_renderer; /* [performance] optimised_renderer: the engine's batched scene renderer */
  int speedups;          /* [performance] engine_speedups: VFP float maths, GL bookkeeping (ssr_patch.c) */
  int anisotropy;        /* [display] anisotropic_filtering: 1 (off) .. 16 */
  int frame_pacing_display; /* [display] frame_pacing = display: the logic clock counts refreshes */
  int prompts;           /* [display] button_prompts: the console's "(A) OK" prompts in the menus */
  int gl_selftest;       /* [debug] gl_selftest */
  int boot_log;          /* [debug] boot_log_on_screen */
  int log_jni;           /* [debug] log_java_calls */
  int log_touch;         /* [debug] log_touches */
} DcrConfig;

/* Read config.ini (writing it with the defaults, or adding missing options,
 * first). Early in main(); the defaults hold until then. */
void dcr_config_load(void);
const DcrConfig *dcr_config(void);

#endif
