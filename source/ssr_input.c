/* ssr_input.c -- the controllers, the touchscreen and the motion sensors, as
 * DemoGLSurfaceView and DemoActivity's sensor listener fed them to the engine.
 *
 * The phone game is played by touch and tilt (../source/docs/NATIVE_CONTRACT.md
 * 7); the only key it reads is Android's Back. Here:
 *
 * MENUS (ssr_menu.c): a focus on the game's own buttons, pressed with A --
 * a synthesised touch at the button, the way a finger pressed it.
 * RACES (ssr_race.c): the controller drives the racer directly, through the
 * engine's STRacer::SetControls, every logic tick; the HUD's touch buttons
 * are hidden. Neither needs the accelerometer any more: one level sample a
 * frame reaches the engine, as a phone lying still gave it.
 *
 * FINGERS. Every frame the port works out which fingers are "down" -- the
 * touchscreen's own, and virtual ones (a menu press, the free pointer) --
 * and sends the engine the difference the way DemoGLSurfaceView did:
 * nativeTouchHandlePoint(3 up) at a finger's last position for each one
 * gone, (2 move) for each one held, (1 down) for each new one, x/y in the
 * window's pixels. The engine has no pointer ids: it follows its touches by
 * nearest position (at most 5: its arrays' size), and its touch 0 -- the
 * first finger down -- is the only one the menus read.
 *
 * KEYS. Back (nativeProjectKey(3, 1)) is sent where the menus call for it.
 *
 * RUMBLE. javaVibrate(ms), and the races' own (ssr_race.c): HD rumble on the
 * controller in use. MIT.
 */#include <math.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "ssr.h"
#include "util.h"

typedef void (*fn_touch)(void *env, void *cls, jint action, jint x, jint y, jint time);
typedef void (*fn_key)(void *env, void *cls, jint key, jint down);
typedef void (*fn_accel)(void *env, void *cls, jfloat x, jfloat y, jfloat z);

static fn_touch g_touch;
static fn_key g_key;
static fn_accel g_accel;

static PadState g_pad;  /* player 1: No. 1 and the console's own Joy-Cons */
static PadState g_pad2; /* player 2 (split screen): No. 2 */
static HidVibrationDeviceHandle g_vib2[5][2];
static int g_vib2_ok[5], g_vib2_last = -1;
static u64 g_rumble2_until;
static HidSixAxisSensorHandle g_six[4]; /* handheld, full key, joy dual left/right */
static int g_six_ok;
static HidVibrationDeviceHandle g_vib[3][2];
static int g_vib_ok[3];
static u64 g_rumble_until;

int ssr_boot_splash_active(void); /* ssr_boot.c */

static int time_ms(void) { return (int)(armTicksToNs(armGetSystemTick()) / 1000000ull); }

/* ============================================================== keys */
#define K_BACK 3 /* DemoGLSurfaceView.a(): KEYCODE_BACK / KEYCODE_SPACE */

/* Back is a press (the engine latches it; its release is ignored). */
void ssr_input_back(void) {
  if (!g_key)
    return;
  if (dcr_config()->log_touch)
    debugPrintf("[input] Back\n");
  g_key(g_jni_env, ssr_class(C_VIEW), K_BACK, 1);
  g_key(g_jni_env, ssr_class(C_VIEW), K_BACK, 0);
}

int ssr_input_keyboard_cfg(void) { return 0; } /* no keyboard mode exists (cfg[1] is only logged) */

/* ============================================================== fingers */
typedef SsrFinger Finger;

/* The JNI layer tracks 10, but the engine's own touch arrays have 5 slots
 * (CONTROLS.md 1): a sixth finger would write past them. */
#define MAX_FINGERS 5
#define F_SCREEN 0x1000u
#define F_POINTER 0x2000u
#define F_SWIPE 0x2001u

static Finger g_down[MAX_FINGERS]; /* what the engine has down, in the order it got them */
static int g_ndown;

static void send_touch(int action, int x, int y) {
  if (!g_touch)
    return;
  if (dcr_config()->log_touch)
    debugPrintf("[input] touch %d at %d,%d\n", action, x, y);
  g_touch(g_jni_env, ssr_class(C_VIEW), action, x, y, time_ms());
}

/* The engine gets from g_down to `want` (at most MAX_FINGERS). */
static void fingers_apply(const Finger *want, int n) {
  /* gone: up, at the last position the engine had */
  for (int k = 0; k < g_ndown;) {
    int still = 0;
    for (int i = 0; i < n && !still; i++)
      still = want[i].key == g_down[k].key;
    if (still) {
      k++;
      continue;
    }
    send_touch(3, g_down[k].x, g_down[k].y);
    memmove(&g_down[k], &g_down[k + 1], sizeof g_down[0] * (size_t)(g_ndown - k - 1));
    g_ndown--;
  }
  /* held: moved to where they are now (every frame, as the engine expects) */
  for (int k = 0; k < g_ndown; k++)
    for (int i = 0; i < n; i++)
      if (want[i].key == g_down[k].key) {
        g_down[k].x = want[i].x, g_down[k].y = want[i].y;
        send_touch(2, g_down[k].x, g_down[k].y);
      }
  /* new: down */
  for (int i = 0; i < n && g_ndown < MAX_FINGERS; i++) {
    int known = 0;
    for (int k = 0; k < g_ndown && !known; k++)
      known = g_down[k].key == want[i].key;
    if (known)
      continue;
    g_down[g_ndown++] = want[i];
    send_touch(1, want[i].x, want[i].y);
  }
}

/* ============================================================== tilt */
static float g_motion_zero; /* the controller's turn when motion steering began */
static int g_motion_have_zero;

/* The controller in use's six-axis state (the Labyrinth 2 port's read_six). */
static int read_six(HidSixAxisSensorState *out) {
  if (!g_six_ok)
    return 0;
  const u64 style = padGetStyleSet(&g_pad);
  if (style & HidNpadStyleTag_NpadHandheld)
    return hidGetSixAxisSensorStates(g_six[0], out, 1) > 0;
  if (style & HidNpadStyleTag_NpadFullKey)
    return hidGetSixAxisSensorStates(g_six[1], out, 1) > 0;
  if (style & HidNpadStyleTag_NpadJoyDual) {
    const u64 attr = padGetAttributes(&g_pad);
    if (attr & HidNpadAttribute_IsRightConnected)
      return hidGetSixAxisSensorStates(g_six[3], out, 1) > 0;
    if (attr & HidNpadAttribute_IsLeftConnected)
      return hidGetSixAxisSensorStates(g_six[2], out, 1) > 0;
  }
  return 0;
}

/* The controller's turn about the axis out of its face, radians: where
 * gravity lies between its x and y axes (held upright, as a wheel). */
static int motion_angle(float *out) {
  HidSixAxisSensorState s = {0};
  if (!read_six(&s))
    return 0;
  float gx = s.acceleration.x, gy = s.acceleration.y;
  if (gx * gx + gy * gy < 0.04f)
    return 0; /* lying flat: no wheel to turn */
  *out = atan2f(gx, gy);
  return 1;
}

/* The steering, -1 (full left) .. 1 (full right), from the stick / motion. */
float ssr_input_steer(const SsrPad *p) {
  const DcrConfig *c = dcr_config();
  float s = 0.0f;
  if (c->steering != SSR_STEER_MOTION) {
    /* the stick's sideways push: a 12% dead zone (a push up or down does not
     * steer), the rest rescaled to 0..1 (full lock a little before the rim)
     * and curved (^1.5: fine near the centre, full lock at the edge); the
     * D-pad all the way (ssr_race.c eases it in) */
    float x = fabsf(p->lx);
    x = x <= 0.12f ? 0.0f : (x - 0.12f) / (0.94f - 0.12f);
    x = x > 1.0f ? 1.0f : x;
    x = powf(x, c->stick_curve) * (p->lx < 0 ? -1.0f : 1.0f);
    if (p->held & HidNpadButton_Left)
      x = -1.0f;
    if (p->held & HidNpadButton_Right)
      x = 1.0f;
    s += x * c->stick_tilt;
  }
  if (c->steering != SSR_STEER_STICK) {
    float a;
    if (motion_angle(&a)) {
      if (!g_motion_have_zero)
        g_motion_zero = a, g_motion_have_zero = 1;
      float d = a - g_motion_zero;
      if (d > (float)M_PI)
        d -= 2.0f * (float)M_PI;
      if (d < -(float)M_PI)
        d += 2.0f * (float)M_PI;
      s += d / (float)(M_PI / 4) * c->motion_gain; /* 45 degrees: a full turn */
    }
  } else {
    g_motion_have_zero = 0;
  }
  return s < -1.0f ? -1.0f : s > 1.0f ? 1.0f : s;
}

/* The accelerometer: level and still (a phone lying on its back, in the
 * engine's -g units); races steer without it now. */
static void send_level(void) {
  if (g_accel)
    g_accel(g_jni_env, ssr_class(C_VIEW), 0.0f, 0.0f, -1.0f);
}

int ssr_test_active(void); /* ssr_test.c: a scripted run (emulator tests) */
int ssr_test_p2(void);
void ssr_test_pad(int pl, u64 *held, float *lx, float *ly);
void ssr_test_frame(void);
void ssr_test_init(void);

/* ============================================================== the frame */
void ssr_input_init(void) {
  padConfigureInput(2, HidNpadStyleSet_NpadStandard); /* two players: split screen */
  padInitializeDefault(&g_pad);
  padInitialize(&g_pad2, HidNpadIdType_No2);
  hidInitializeTouchScreen();
  Result r0 = hidGetSixAxisSensorHandles(&g_six[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
  Result r1 = hidGetSixAxisSensorHandles(&g_six[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
  Result r2 = hidGetSixAxisSensorHandles(&g_six[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
  if (R_SUCCEEDED(r0) && R_SUCCEEDED(r1) && R_SUCCEEDED(r2)) {
    for (int i = 0; i < 4; i++)
      hidStartSixAxisSensor(g_six[i]);
    g_six_ok = 1;
  } else {
    debugPrintf("[input] no motion sensors (%x %x %x)\n", r0, r1, r2);
  }
  g_vib_ok[0] = R_SUCCEEDED(hidInitializeVibrationDevices(g_vib[0], 2, HidNpadIdType_Handheld,
                                                           HidNpadStyleTag_NpadHandheld));
  g_vib_ok[1] = R_SUCCEEDED(hidInitializeVibrationDevices(g_vib[1], 2, HidNpadIdType_No1,
                                                           HidNpadStyleTag_NpadJoyDual));
  g_vib_ok[2] = R_SUCCEEDED(hidInitializeVibrationDevices(g_vib[2], 2, HidNpadIdType_No1,
                                                           HidNpadStyleTag_NpadFullKey));
  g_touch = (fn_touch)ssr_native("Java_com_sega_ssasr_DemoGLSurfaceView_nativeTouchHandlePoint");
  g_key = (fn_key)ssr_native("Java_com_sega_ssasr_DemoGLSurfaceView_nativeProjectKey");
  g_accel = (fn_accel)ssr_native("Java_com_sega_ssasr_DemoGLSurfaceView_nativeProjectSetAcceleration");
  ssr_race_init();
  ssr_menu_init();
  ssr_test_init();
  debugPrintf("[input] touch %s, keys %s, accelerometer %s; motion sensors %s\n", g_touch ? "yes" : "NO",
              g_key ? "yes" : "NO", g_accel ? "yes" : "NO", g_six_ok ? "on" : "unavailable");
}

void ssr_input_release_all(void) {
  if (g_touch)
    send_touch(4, -1, -1); /* DemoGLSurfaceView.b(): ACTION_CANCEL, every finger */
  g_ndown = 0;
}

static void rumble_off(void) {
  HidVibrationValue v = {0};
  v.freq_low = 160.0f, v.freq_high = 320.0f;
  HidVibrationValue vv[2] = {v, v};
  for (int i = 0; i < 3; i++)
    if (g_vib_ok[i])
      hidSendVibrationValues(g_vib[i], vv, 2);
}

void ssr_input_rumble(int ms) { ssr_input_rumble_strength(ms, 0.6f); }

/* ---------------------------------------------------------------- the pads */
/* One Joy-Con alone (split screen hands one to each player), held sideways
 * with its rail up, as the console's games take it: the face button at the
 * right is A, SL / SR are L / R, its + or - is +, a click of its stick ZL;
 * its stick turned with it (the Labyrinth 2 port's mapping). */
static int is_single(u64 st) {
  return (st & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) &&
         !(st & (HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadFullKey));
}

static u64 single_buttons(u64 st, u64 b) {
  u64 o = b & ~(HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y | HidNpadButton_Up |
                HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_Minus |
                HidNpadButton_StickL | HidNpadButton_StickR);
  if (st & HidNpadStyleTag_NpadJoyLeft) { /* Down at the right, Left below, Up at the left, Right on top */
    if (b & HidNpadButton_Down) o |= HidNpadButton_A;
    if (b & HidNpadButton_Left) o |= HidNpadButton_B;
    if (b & HidNpadButton_Up) o |= HidNpadButton_Y;
    if (b & HidNpadButton_Right) o |= HidNpadButton_X;
  } else { /* X at the right, A below, B at the left, Y on top */
    if (b & HidNpadButton_X) o |= HidNpadButton_A;
    if (b & HidNpadButton_A) o |= HidNpadButton_B;
    if (b & HidNpadButton_B) o |= HidNpadButton_Y;
    if (b & HidNpadButton_Y) o |= HidNpadButton_X;
  }
  if (b & (HidNpadButton_LeftSL | HidNpadButton_RightSL)) o |= HidNpadButton_L;
  if (b & (HidNpadButton_LeftSR | HidNpadButton_RightSR)) o |= HidNpadButton_R;
  if (b & (HidNpadButton_Minus | HidNpadButton_Plus)) o |= HidNpadButton_Plus;
  if (b & (HidNpadButton_StickL | HidNpadButton_StickR)) o |= HidNpadButton_ZL;
  return o;
}

/* a lone Joy-Con's stick, from its own frame (held upright) to the sideways
 * hold's: the left one turned anticlockwise, the right one clockwise */
static void sideways(u64 st, float *x, float *y) {
  const float cx = *x, cy = *y;
  if (st & HidNpadStyleTag_NpadJoyLeft)
    *x = -cy, *y = cx;
  else
    *x = cy, *y = -cx;
}

static u64 swap_ab(u64 b) {
  const u64 ab = HidNpadButton_A | HidNpadButton_B;
  return (b & ~ab) | ((b & HidNpadButton_A) ? HidNpadButton_B : 0) | ((b & HidNpadButton_B) ? HidNpadButton_A : 0);
}

/* one player's controller as they hold it; 1 if there is one */
static int read_pad(PadState *pad, u64 *prev, SsrPad *out) {
  padUpdate(pad);
  memset(out, 0, sizeof *out);
  const u64 st = padGetStyleSet(pad);
  const int single = is_single(st);
  u64 held = padGetButtons(pad);
  if (single)
    held = single_buttons(st, held);
  if (dcr_config()->swap_ab)
    held = swap_ab(held);
  float tlx = 0, tly = 0;
  ssr_test_pad(pad == &g_pad2, &held, &tlx, &tly);
  out->held = held, out->down = held & ~*prev, out->up = *prev & ~held;
  *prev = held;
  const int right_alone = single && (st & HidNpadStyleTag_NpadJoyRight);
  HidAnalogStickState l = padGetStickPos(pad, right_alone ? 1 : 0), r = padGetStickPos(pad, 1);
  out->lx = (float)l.x / 32767.0f, out->ly = (float)l.y / 32767.0f;
  out->rx = single ? 0.0f : (float)r.x / 32767.0f, out->ry = single ? 0.0f : (float)r.y / 32767.0f;
  if (single)
    sideways(st, &out->lx, &out->ly);
  if (tlx != 0 || tly != 0)
    out->lx = tlx, out->ly = tly;
  return st != 0 || (pad == &g_pad2 && ssr_test_p2());
}

/* ---------------------------------------------------------------- player 2 */
/* Split screen's second player: the controller that is No. 2 (a Pro
 * Controller, a pair of Joy-Cons, or one Joy-Con held sideways). */
static u64 g_prev2;
int ssr_input_p2(SsrPad *out) { return read_pad(&g_pad2, &g_prev2, out); }

static int p2_connected(void) {
  padUpdate(&g_pad2);
  return padGetStyleSet(&g_pad2) != 0 || ssr_test_p2();
}

/* Joy-Cons held sideways, one a player, while split screen is on */
void ssr_input_split(int on) {
  static int was = -1;
  if (on == was)
    return;
  was = on;
  hidSetNpadJoyHoldType(on ? HidNpadJoyHoldType_Horizontal : HidNpadJoyHoldType_Vertical);
}

/* The console's controller screen for two players (blocking, as the Switch
 * keyboard); 1 if two are there after it */
void ssr_boot_system_dialog(int on); /* ssr_boot.c */
void ssr_clock_resync(void);         /* ssr_patch.c */
int ssr_input_controllers_2(void) {
  if (ssr_test_active()) /* a scripted run: its own second controller */
    return p2_connected();
  HidLaControllerSupportArg arg;
  hidLaCreateControllerSupportArg(&arg);
  arg.hdr.player_count_min = 2;
  arg.hdr.player_count_max = 2;
  arg.hdr.enable_permit_joy_dual = 1;
  arg.hdr.enable_single_mode = 0;
  arg.hdr.enable_identification_color = 1;
  arg.identification_color[0] = (HidLaControllerSupportArgColor){0x1e, 0x5c, 0xe6, 0xff}; /* Sonic blue */
  arg.identification_color[1] = (HidLaControllerSupportArgColor){0xe8, 0x2a, 0x2a, 0xff}; /* Knuckles red */
  arg.enable_explain_text = 1;
  hidLaSetExplainText(&arg, "Player 1: the top of the screen", HidNpadIdType_No1);
  hidLaSetExplainText(&arg, "Player 2: the bottom", HidNpadIdType_No2);
  HidLaControllerSupportResultInfo info;
  memset(&info, 0, sizeof info);
  HidNpadJoyHoldType hold = 0;
  hidGetNpadJoyHoldType(&hold);
  const u64 t0 = armGetSystemTick();
  ssr_boot_system_dialog(1);
  ssr_audio_pause(1); /* (as the Labyrinth 2 port shows it: the sound held) */
  const Result rc = hidLaShowControllerSupport(&info, &arg);
  ssr_audio_pause(0);
  ssr_boot_system_dialog(0);
  ssr_clock_resync();
  padUpdate(&g_pad);
  const int two = p2_connected();
  debugPrintf("[input] the controller screen: 0x%x, %d player(s) after %llu ms (%s, Joy-Cons %s); player 1's "
              "controller 0x%llx, player 2's 0x%llx: %s\n",
              rc, info.player_count, (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull),
              appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
              hold == HidNpadJoyHoldType_Horizontal ? "sideways" : "upright",
              (unsigned long long)padGetStyleSet(&g_pad), (unsigned long long)padGetStyleSet(&g_pad2),
              two ? "two players" : "no second controller");
  return two;
}

/* rumble on player 2's controller (by its style) */
static void rumble_p2(int ms, float amp) {
  static const u32 tags[5] = {HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadJoyDual,
                              HidNpadStyleTag_NpadJoyLeft, HidNpadStyleTag_NpadJoyRight};
  const u64 st = padGetStyleSet(&g_pad2);
  int k = -1;
  for (int i = 1; i < 5 && k < 0; i++)
    if (st & tags[i])
      k = i;
  if (k < 0)
    return;
  const int n = k >= 3 ? 1 : 2;
  if (!g_vib2_ok[k])
    g_vib2_ok[k] = R_SUCCEEDED(hidInitializeVibrationDevices(g_vib2[k], n, HidNpadIdType_No2, tags[k])) ? 1 : -1;
  if (g_vib2_ok[k] < 0)
    return;
  HidVibrationValue v = {.amp_low = amp, .freq_low = 160.0f, .amp_high = amp, .freq_high = 320.0f};
  HidVibrationValue vv[2] = {v, v};
  hidSendVibrationValues(g_vib2[k], vv, n);
  g_vib2_last = k;
  g_rumble2_until = armGetSystemTick() + armNsToTicks((u64)(ms > 1000 ? 1000 : ms) * 1000000ull);
}

static void rumble2_off(void) {
  const int k = g_vib2_last;
  if (k < 0 || g_vib2_ok[k] <= 0)
    return;
  HidVibrationValue v = {.freq_low = 160.0f, .freq_high = 320.0f};
  HidVibrationValue vv[2] = {v, v};
  hidSendVibrationValues(g_vib2[k], vv, k >= 3 ? 1 : 2);
}

/* rumble for engine copy `player` (0: player 1, 1: player 2) */
void ssr_input_rumble_player(int player, int ms, float amp) {
  if (!dcr_config()->rumble || ms <= 0)
    return;
  if (player)
    rumble_p2(ms, amp);
  else
    ssr_input_rumble_strength(ms, amp);
}

void ssr_input_rumble_strength(int ms, float amp) {
  if (!dcr_config()->rumble || ms <= 0)
    return;
  HidVibrationValue v = {.amp_low = amp, .freq_low = 160.0f, .amp_high = amp, .freq_high = 320.0f};
  HidVibrationValue vv[2] = {v, v};
  u64 style = padGetStyleSet(&g_pad);
  int i = style & HidNpadStyleTag_NpadHandheld ? 0 : style & HidNpadStyleTag_NpadFullKey ? 2 : 1;
  if (g_vib_ok[i])
    hidSendVibrationValues(g_vib[i], vv, 2);
  g_rumble_until = armGetSystemTick() + armNsToTicks((u64)(ms > 1000 ? 1000 : ms) * 1000000ull);
}

static u64 g_last_down; /* player 1's presses, last frame (ssr_split.c: - brings the controller screen) */
u64 ssr_input_last_down(void) { return g_last_down; }

void ssr_input_frame(SsrPad *out) {
  static u64 prev;
  ssr_test_frame();
  read_pad(&g_pad, &prev, out);
  ssr_split_net_pad1(out); /* split screen's BATTLE opening / leaving player 1's LOCAL menu */
  g_last_down = out->down;
  if (g_rumble2_until && armGetSystemTick() >= g_rumble2_until) {
    g_rumble2_until = 0;
    rumble2_off();
  }
  if (g_rumble_until && armGetSystemTick() >= g_rumble_until) {
    g_rumble_until = 0;
    rumble_off();
  }

  /* the engine's size (the window's) */
  int w, h, ww, wh;
  ssr_gfx_size(&ww, &wh);
  ssr_split_view(0, &w, &h);
  Finger want[MAX_FINGERS];
  int n = 0;

  /* the touchscreen's own fingers (1280x720 -> the window) */
  HidTouchScreenState st = {0};
  hidGetTouchScreenStates(&st, 1);
  const int touching = st.count > 0 && dcr_config()->touch;
  out->any_touch = st.count > 0;
  for (int i = 0; touching && i < (int)st.count && n < MAX_FINGERS; i++) {
    const int x = (int)st.touches[i].x * ww / 1280, y = (int)st.touches[i].y * wh / 720;
    if (y < h)
      want[n++] = (Finger){F_SCREEN | st.touches[i].finger_id, x, y};
  }

  /* the race HUD's layout (full-screen, or split screen's top half), every
   * frame of a race -- not only once the controller drives it: its groups
   * slide in while a loading screen or a pop-up still has the menus */
  ssr_race_hud_in(0, ssr_split_hud_layout(0), ssr_split_margin());
  if (ssr_boot_splash_active()) {
    /* DemoGLSurfaceView forwarded no touch until the splash was over (the
     * first one skipped it: ssr_boot.c) */
    fingers_apply(want, 0);
    ssr_race_frame(out, 0);
  } else {
    const int mode = ssr_menu_frame(out, w, h, want, &n, MAX_FINGERS, touching);
    ssr_race_frame(out, mode == SSR_MODE_RACE);
    fingers_apply(want, n);
  }
  send_level();
}
