/* ssr.h -- what the Sonic & SEGA All-Stars Racing files share. MIT. */
#ifndef SSR_H
#define SSR_H
#include <stddef.h>
#include <stdint.h>

#include "jni.h"
#include "so_util.h"
#include "ssr_pack.h"

/* ------------------------------------------------------------- Java classes */
#define SSR_JPKG "com/sega/ssasr"
#define C_ACTIVITY   SSR_JPKG "/DemoActivity"
#define C_VIEW       SSR_JPKG "/DemoGLSurfaceView"
#define C_RENDERER   SSR_JPKG "/DemoRenderer"
#define C_NOAH       SSR_JPKG "/NoahWrapper"
#define C_OPENFEINT  SSR_JPKG "/DDOpenFeint"
#define C_AUDIOTRACK "android/media/AudioTrack"

/* ------------------------------------------------------------ loader (ssr_loader.c) */
extern so_module g_mod_game;          /* libssasr.so */
int ssr_load_module(void);            /* 0 on success */
void ssr_run_constructors(void);      /* System.loadLibrary's */
void *ssr_native(const char *symbol); /* an export of the engine, or NULL */
/* Split screen runs a second copy of the engine (ssr_split.c): its own code
 * and data, the same imports. Which copy runs on this thread (0, or 1),
 * inherited by the threads a copy starts. */
int ssr_engine_current(void);
void ssr_engine_set_current(int engine);
extern so_module g_mod_game2;
void *ssr_native_in(int engine, const char *symbol);            /* an export of that copy */
void *ssr_addr_in(int engine, uint32_t vaddr, uint32_t expect); /* a library address in it, checked */
int ssr_load_second_module(void);     /* its load, constructors and JNI_OnLoad; 0 once up */

/* --------------------------------------------------------------- the APK (ssr_apk.c) */
int ssr_apk_init(const char *path);   /* the central directory of the APK */
const char *ssr_apk_path(void);
/* An entry's data: stored -> its offset in the APK file (method 0); 0 found. */
int ssr_apk_locate(const char *name, uint64_t *off, uint32_t *len, int *method);
/* An entry read whole (malloc'd, *len set), or NULL. */
uint8_t *ssr_apk_read(const char *name, size_t *len);

/* ------------------------------------------------------ the game's data (ssr_data.c) */
/* Where packres.png and the intro are (found once at start-up). */
const SsrPack *ssr_pack(void);
int ssr_data_find(const char *apk);   /* 0: found; else the error was reported */

/* ---------------------------------------------------------------- Java (ssr_java.c) */
extern JObj *g_activity;              /* DemoActivity */
extern JObj *g_renderer;              /* DemoRenderer: the java* callbacks' object */
extern JObj *g_view;                  /* DemoGLSurfaceView */
void ssr_java_init(void);
void *ssr_class(const char *name);    /* a class object, for a static native's jclass */
/* The language DemoActivity.onCreate chose: 0 en(GB), 1 fr, 2 it, 3 de, 4 es, 5 ja, 6 en(US) */
int ssr_language(void);
/* javaImageText* (ssr_text.c) */
void ssr_text_init_own(void);
void ssr_text_init_tex(int tex);
void ssr_text_set(int slot, const char *utf8);
int ssr_text_bake(void);
int ssr_text_tex(void);
void ssr_text_terminate(void);

/* ---------------------------------------------------------------- audio (ssr_audio.c) */
void ssr_audio_selftest(void);
void ssr_audio_init(void);
void ssr_audio_pause(int paused);     /* HOME: everything holds */
void ssr_audio_close(void);           /* closing: writes return at once */
unsigned long ssr_audio_mixes(void);
/* The Java AudioTrack OpenAL Soft's Android backend plays through: its
 * methods' handlers (listed in ssr_java.c's table). */
#define SSR_AT_HANDLER(fn) jvalue ssr_##fn(JObj *self, const jvalue *a, const JMethod *m)
SSR_AT_HANDLER(at_min_buffer);
SSR_AT_HANDLER(at_init);
SSR_AT_HANDLER(at_play);
SSR_AT_HANDLER(at_stop);
SSR_AT_HANDLER(at_pause);
SSR_AT_HANDLER(at_flush);
SSR_AT_HANDLER(at_release);
SSR_AT_HANDLER(at_write);
SSR_AT_HANDLER(at_play_state);
SSR_AT_HANDLER(at_state);
SSR_AT_HANDLER(at_rate);
SSR_AT_HANDLER(at_stereo_volume);
SSR_AT_HANDLER(at_zero);
/* DemoActivity's MediaPlayer music (res/raw/music_*.mp3). */
void ssr_music_play(int index, float volume, int loop);
void ssr_music_stop(void);
void ssr_music_pause(void);
void ssr_music_unpause(void);
void ssr_music_volume(float volume);
void ssr_audio_engine_gain(int engine, float gain); /* one copy's sound (split screen) */
/* The intro movie's sound, added to the mix while it plays (ssr_video.c). */
void ssr_video_mix(int16_t *out, int frames, int out_rate);

/* ------------------------------------------- FFmpeg (ssr_media.c, -fno-short-enums) */
typedef struct SsrMp3 SsrMp3;
/* An MP3 read from `path` at [off, off + len) (a stored entry of the APK). */
SsrMp3 *ssr_mp3_open(const char *path, uint64_t off, uint32_t len);
/* Up to `max` frames of interleaved stereo s16 at the file's own rate; 0 at the end. */
int ssr_mp3_read(SsrMp3 *m, int16_t *out, int max);
int ssr_mp3_rate(const SsrMp3 *m);
void ssr_mp3_rewind(SsrMp3 *m);
void ssr_mp3_close(SsrMp3 *m);

/* ------------------------------------------------------------- the intro (ssr_video.c) */
int ssr_video_start(void);            /* 1: it plays (drawn over the frames, sound mixed) */
int ssr_video_playing(void);
void ssr_video_skip(void);
void ssr_video_draw(void);            /* before each present, over the game's picture */

/* -------------------------------------------------------------------- screen (ssr_gfx.c) */
int ssr_gfx_init(void);               /* EGL: an OpenGL ES 1 context on the window */
void ssr_gfx_size(int *w, int *h);
void ssr_gfx_present(void);           /* the overlays, then the swap */
/* The Java side's pictures shown before and over the game (loading.jpg,
 * loading_white.jpg, loading_sumo.jpg): alpha 0..1 each; 0 = not shown. */
void ssr_gfx_splash(float loading, float white, float sumo);
/* split screen (ssr_split.c): the second engine's GL context, an engine's
 * context and framebuffer before it runs, and what the present shows */
enum { SSR_SPLIT_NONE, SSR_SPLIT_RACE, SSR_SPLIT_PIP, SSR_SPLIT_COLUMNS, SSR_SPLIT_JOINING };
int ssr_gfx_second_context(void);
int ssr_gfx_use(int engine, int offscreen);
void ssr_gfx_split(int mode, const float *pip); /* pip: x, y, w, h of the second's inset */
/* the part of an engine's framebuffer shown (texture coordinates: v from the bottom) */
void ssr_gfx_split_source(int engine, float u0, float v0, float u1, float v1);
/* the menus' focus frame around a button (window pixels, top left), or none;
 * player 2's (split screen's racer select: their column) */
void ssr_gfx_focus(int on, float x, float y, float w, float h);
void ssr_gfx_focus2(int on, float x, float y, float w, float h);
/* a split race's passes (ssr_split_race.c, inside player 1's copy's frame,
 * its context current): 0 player 1's half's framebuffer, 1 player 2's, -1
 * the window (0 on success); then the two in the window's halves */
int ssr_gfx_pass(int which);
void ssr_gfx_pass_composite(void);
void ssr_gfx_edge_fade(float x, float y0, float y1, float w); /* this frame: the sky over an edge (split racer select) */
int ssr_gfx_ready(void);

/* ------------------------------------------------------------------ input (ssr_input.c) */
typedef struct {
  uint64_t down, held, up;  /* HidNpadButton_* (A/B swapped when asked) */
  float lx, ly, rx, ry;     /* sticks, -1..1 (up positive) */
  int any_touch;
} SsrPad;
void ssr_input_init(void);
/* Reads the controllers and the touchscreen and sends them to the engine
 * (touches, keys, the accelerometer), on the thread that runs its frames. */
void ssr_input_frame(SsrPad *out);
void ssr_input_rumble(int ms);        /* javaVibrate */
/* The steer the stick / motion give, -1 (left) .. 1 (right). */
float ssr_input_steer(const SsrPad *p);
/* The menus' pointer: where it is, whether A holds it down; 0 if hidden. */
int ssr_input_pointer(float *x, float *y, int *pressed);

/* A finger on the screen, real or virtual (ssr_input.c sends the set). */
typedef struct {
  uint32_t key; /* who holds it (touchscreen finger, pointer, race button) */
  int x, y;     /* window pixels */
} SsrFinger;

/* ----------------------------------------------------------------- races (ssr_race.c) */
void ssr_race_init(void);
void ssr_race_init_in(int engine);    /* a copy's controller hooked (split screen: the second) */
/* the top screen is the race's HUD */
int ssr_race_active(void);
int ssr_race_active_in(int engine);
/* every frame, before nativeProjectRun: the pad for the race's logic ticks
 * (racing), the console-style HUD */
void ssr_race_frame(const SsrPad *p, int racing);
void ssr_race_frame_in(int engine, const SsrPad *p, int racing);
/* the race HUD's layout, every frame while the copy has a race: full-screen
 * (the console's), or split screen's top / bottom strip (margin: the strip's
 * design widths beside the design) */
enum { SSR_HUD_PHONE, SSR_HUD_SPLIT, SSR_HUD_BOTTOM, SSR_HUD_TOP };
void ssr_race_hud_own_bar(int on); /* VS RACE: each copy's progress bar in its half (ssr_race.c) */
void ssr_race_hud_in(int engine, int layout, float margin);
/* a split race's render pass (ssr_split_race.c): what player 1's HUD shows
 * in it (0 none: as laid out; 1 player 1's half; 3 over both) */
void ssr_race_hud_pass(int engine, int pass);
/* player 2's pad, for their racer in player 1's copy's race */
void ssr_race_frame_p2(const SsrPad *p, int racing);
const char *ssr_race_racer_name(int engine, int ch); /* STCharacter -> its name (logs) */
void ssr_race_patch(void);  /* at load (ssr_patch.c): rumble, the tutorial's words */
void ssr_race_report(void);
void ssr_input_release_all(void);     /* focus lost: every key and finger up */
/* 0: DemoActivity's keyboard configuration (cfg[1] is only logged) */
int ssr_input_keyboard_cfg(void);
void ssr_input_back(void);            /* Android's Back, pressed */
void ssr_input_rumble_strength(int ms, float amp);
/* split screen's second player: controller No. 2 (1 if one is there) */
int ssr_input_p2(SsrPad *out);
void ssr_input_split(int on);         /* Joy-Cons held sideways, one a player */
int ssr_input_controllers_2(void);    /* the console's controller screen, for two */
void ssr_input_rumble_player(int player, int ms, float amp);

/* ------------------------------------------------------- button prompts (ssr_prompt.c) */
enum { SSR_PROMPT_NONE, SSR_PROMPT_TITLE, SSR_PROMPT_CAROUSEL, SSR_PROMPT_CAROUSEL_RULES, SSR_PROMPT_CAROUSEL_INFO,
       SSR_PROMPT_CHARACTER, SSR_PROMPT_FOCUS, SSR_PROMPT_FOCUS_NOBACK, SSR_PROMPT_KEYBOARD, SSR_PROMPT_SLIDERS,
       SSR_PROMPT_RULES, SSR_PROMPT_LIST, SSR_PROMPT_PAUSE, SSR_PROMPT_YESNO, SSR_PROMPT_OK, SSR_PROMPT_CONTINUE,
       SSR_PROMPT_SEGAMILES, SSR_PROMPT_LOBBY, SSR_PROMPT_COUNT };
void ssr_prompt_set(int set);                       /* every frame (ssr_menu.c) */
void ssr_prompt_place(float y_centre);              /* ...and where (normalised) */
void ssr_prompt_draw(int screen_w, int screen_h);   /* before each present (ssr_gfx.c) */
void ssr_prompt_banner(const char *utf8);           /* a message at the top (NULL: none) */
/* one button's picture this frame (SSR_ICON_*), centred at x, y (window
 * pixels), h tall: beside the game's own words (the title's PRESS) */
void ssr_prompt_icon(int icon, float cx, float cy, float h, float alpha);
/* split screen: a word in yellow in a player's column this frame (window
 * pixels, its centre) */
enum { SSR_CARD_JOIN, SSR_CARD_CONNECT, SSR_CARD_JOINING, SSR_CARD_READY, SSR_CARD_P1, SSR_CARD_P2, SSR_CARD_COUNT };
void ssr_prompt_card(int kind, float cx, float cy);
/* split screen's racer select: the prompts in two columns (player 1's left,
 * player 2's right: ssr_prompt2_set) */
void ssr_prompt_columns(int on);
void ssr_prompt2_set(int set);
void ssr_prompt_top_half(int on); /* player 1's bar in the top half (split screen's BATTLE) */

/* ----------------------------------------------------------------- menus (ssr_menu.c) */
enum { SSR_MODE_MENU, SSR_MODE_RACE };
void ssr_menu_init(void);
void ssr_menu_patch(void);  /* at load (ssr_patch.c): [debug] log_touches' hit log */
/* every frame: the focus, presses (virtual fingers into want), Back; returns
 * SSR_MODE_RACE when the race's HUD has the controller */
int ssr_menu_frame(const SsrPad *p, int w, int h, SsrFinger *want, int *n, int cap, int touching);
int ssr_menu_tap(uint32_t pntr_hash);  /* a press on a live button of player 1's copy (1 if so) */
/* player 2's copy's menus (ssr_menu2.c: the same, for engine 2, player 2's pad) */
void ssr_menu2_init(void);
int ssr_menu2_frame(const SsrPad *p, int w, int h, SsrFinger *want, int *n, int cap, int touching);

/* ------------------------------------------------------------ split screen (ssr_split.c) */
void ssr_split_init(void);             /* once engine 1 is up */
/* each frame: before the controls (starting, stopping, sizes), and after
 * engine 1's frame (engine 2's; 1 if it drew; its time added to *run) */
void ssr_split_pre(void);
int ssr_split_engine1_offscreen(void); /* engine 1 draws into its framebuffer (split screen's BATTLE) */
int ssr_split_post(uint64_t *run_ticks);
void ssr_split_patch(void);            /* at load (ssr_patch.c): the camera, the race's hooks */
int ssr_split_session(void);           /* split screen chosen (the menus to the main menu) */
int ssr_split_mode_select(void); /* split screen's Solo menu (its words: SELECT MODE) */
int ssr_split_columns(void);           /* the racer select in two columns now */
int ssr_split_running(void);           /* engine 2 runs its frames */
int ssr_split_race(void);              /* the screen is split (a race) */
void ssr_split_view(int engine, int *w, int *h); /* an engine's size for its input */
int ssr_split_hud_layout(int engine);   /* SSR_HUD_* for its race HUD */
float ssr_split_margin(void);
/* the racer select's presses (ssr_menu.c's DAVE model, either copy): 1 if
 * split screen takes the press (READY) */
enum { SSR_DAVE_A, SSR_DAVE_B, SSR_DAVE_TURN, SSR_DAVE_POLL };
int ssr_split_dave(int engine, int event, uint8_t *screen);
int ssr_split_solo_blocked(int page);   /* a Solo menu card split screen does not have */
void ssr_split_net_choose(int battle);  /* the mode select's BATTLE / VS RACE (ssr_split_net.c) */
int ssr_split_net_pad1(SsrPad *p);      /* player 1's menus' pad while BATTLE drives them */
void ssr_split_back2(void);            /* Back, on engine 2 */
/* the race (ssr_split_race.c) */
int ssr_split_race_is_p2(const void *racer); /* player 2's racer */
void *ssr_split_race_cam2(void);             /* player 2's camera */
int ssr_split_race_player(void);             /* the racer being worked on is player 2's (1) */
int ssr_split_race_pass(void);               /* the render pass (0 none, 1, 2, 3) */
void ssr_split_focus(int focused);     /* HOME: engine 2's onPause / onResume */
void ssr_split_exit(void);
void ssr_split_report(void);

/* ------------------------------------------------------------------ boot (ssr_boot.c) */
int ssr_boot_run(void);
void ssr_request_exit(void);
uint64_t ssr_frame_count(void);
int ssr_boot_movie_stopped(void);    /* javaIsMovieStopped */
int ssr_boot_splash_active(void);    /* the splash or the intro is over the game */

#endif
