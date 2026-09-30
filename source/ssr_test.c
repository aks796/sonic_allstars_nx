/* ssr_test.c -- a scripted run, for testing under an emulator (Ryujinx).
 *
 * Only when <root>/test_script.txt exists (never on a player's card: nothing
 * makes it). Its lines drive both players' controllers, frame by frame:
 *
 *   # a comment
 *   p2 on                   a second controller is there (no controller screen)
 *   wait <Screen> [s]       until player 1's copy shows that screen, taking
 *                           input (its class:
 *                           TitleScreen, MainMenu, CharacterSelectScreen...,
 *                           HUD, PauseMenu, loading, popup)
 *   wait2 <Screen> [s]      the same for player 2's copy
 *   waitlog <text> [s]      until a log line has <text> in it
 *   press p1|p2|both <BTN> [n]  hold a button n frames (6), then 6 frames off
 *   hold p1|p2 <BTN> <n>    hold it n frames (no gap after)
 *   stick p1|p2 <x> <y> <n> the left stick at (x, y) for n frames
 *   sleep <n>               n frames
 *   shot <name>             the next frame saved as <root>/test/<name>.bmp
 *   quit                    end the program
 *
 * Buttons: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT (as the game
 * takes them: after the A/B swap). A wait that runs out saves a picture
 * (timeout-<line>) and ends the script. Everything is logged as [test]. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>

#include "ssr.h"
#include "util.h"

const char *dcr_game_root(void);                  /* main.c */
void dcr_gl_request_capture_named(const char *n); /* gl_mesa.c */
extern void (*dcr_log_tap)(const char *line);     /* util.c */
const char *ssr_menu_screen(void);                /* ssr_menu.c */
const char *ssr_menu2_screen(void);               /* ssr_menu2.c */
int ssr_menu_screen_ready(void);
int ssr_menu2_screen_ready(void);
void log_flush_ring(void);                        /* util.c */

enum { MAX_LINES = 400 };
static char *g_line[MAX_LINES];
static int g_nlines, g_pc, g_on, g_p2;
static int g_wait;         /* frames left in this step */
static int g_timeout;      /* frames left for a wait */
static int g_started;      /* the step at g_pc has begun */
static u64 g_held[2];
static float g_stick[2][2];
static int g_stick_on[2];
static int g_gap;          /* frames with nothing held after a press */
static char g_logwant[128];
static volatile int g_logseen;
static uint64_t g_frame;

int ssr_test_active(void) { return g_on; }
int ssr_test_p2(void) { return g_on && g_p2; }

static void log_tap(const char *line) {
  if (g_logwant[0] && !g_logseen && strstr(line, g_logwant) && !strstr(line, "[test]"))
    g_logseen = 1;
}

void ssr_test_init(void) {
  char path[512];
  snprintf(path, sizeof path, "%s/test_script.txt", dcr_game_root());
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char buf[256];
  while (g_nlines < MAX_LINES && fgets(buf, sizeof buf, f)) {
    char *s = buf;
    while (*s == ' ' || *s == '\t')
      s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
      s[--n] = 0;
    if (!n || *s == '#')
      continue;
    g_line[g_nlines++] = strdup(s);
  }
  fclose(f);
  snprintf(path, sizeof path, "%s/test", dcr_game_root());
  mkdir(path, 0777);
  g_on = 1;
  dcr_log_tap = log_tap;
  debugPrintf("[test] a scripted run: %d steps from test_script.txt\n", g_nlines);
}

static u64 button(const char *b) {
  static const struct {
    const char *n;
    u64 v;
  } k[] = {{"A", HidNpadButton_A},         {"B", HidNpadButton_B},         {"X", HidNpadButton_X},
           {"Y", HidNpadButton_Y},         {"L", HidNpadButton_L},         {"R", HidNpadButton_R},
           {"ZL", HidNpadButton_ZL},       {"ZR", HidNpadButton_ZR},       {"PLUS", HidNpadButton_Plus},
           {"MINUS", HidNpadButton_Minus}, {"UP", HidNpadButton_Up},       {"DOWN", HidNpadButton_Down},
           {"LEFT", HidNpadButton_Left},   {"RIGHT", HidNpadButton_Right}};
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
    if (!strcasecmp(b, k[i].n))
      return k[i].v;
  debugPrintf("[test] no button %s\n", b);
  return 0;
}

static int player_of(const char *s) { return s && (!strcasecmp(s, "p2") || !strcmp(s, "2")); }

static const char *screen_now(int engine) {
  const char *s = engine ? ssr_menu2_screen() : ssr_menu_screen();
  return s ? s : "-";
}

static void fail(const char *why) {
  debugPrintf("[test] step %d (%s): %s -- the script ends (player 1 at %s, player 2 at %s)\n", g_pc + 1,
              g_line[g_pc], why, screen_now(0), screen_now(1));
  char n[32];
  snprintf(n, sizeof n, "timeout-%d", g_pc + 1);
  dcr_gl_request_capture_named(n);
  g_pc = g_nlines;
}

/* one frame of the script (before the controllers are read) */
void ssr_test_frame(void) {
  if (!g_on)
    return;
  g_frame++;
  for (int e = 0; e < 2; e++) { /* each copy's screens as they change */
    static const char *seen[2];
    const char *now = screen_now(e);
    if (now != seen[e]) {
      seen[e] = now;
      debugPrintf("[test] frame %llu: player %d's copy at %s\n", (unsigned long long)g_frame, e + 1, now);
    }
  }
  if (g_pc >= g_nlines) {
    static int told;
    if (!told++)
      debugPrintf("[test] the script is done\n");
    return;
  }
  if (g_gap > 0) {
    g_gap--;
    return;
  }
  while (g_pc < g_nlines) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", g_line[g_pc]);
    char *arg[8] = {0};
    int na = 0;
    for (char *t = strtok(tmp, " \t"); t && na < 8; t = strtok(NULL, " \t"))
      arg[na++] = t;
    const char *op = arg[0];
    const int first = !g_started;
    g_started = 1;
    if (first)
      debugPrintf("[test] step %d, frame %llu: %s\n", g_pc + 1, (unsigned long long)g_frame, g_line[g_pc]);

    if (!strcmp(op, "p2")) {
      g_p2 = arg[1] && !strcasecmp(arg[1], "on");
    } else if (!strcmp(op, "wait") || !strcmp(op, "wait2")) {
      const int e = op[4] == '2';
      if (first)
        g_timeout = (arg[2] ? atoi(arg[2]) : 60) * 60;
      if (!arg[1] || strcmp(screen_now(e), arg[1]) || !(e ? ssr_menu2_screen_ready() : ssr_menu_screen_ready())) {
        if (--g_timeout <= 0)
          fail("timed out");
        return;
      }
    } else if (!strcmp(op, "waitlog")) {
      if (first) {
        /* the text: everything after "waitlog", less a trailing number of seconds */
        const char *rest = g_line[g_pc] + 7;
        while (*rest == ' ')
          rest++;
        snprintf(g_logwant, sizeof g_logwant, "%s", rest);
        g_timeout = 60 * 60;
        char *sp = strrchr(g_logwant, ' ');
        if (sp && atoi(sp + 1) > 0) {
          g_timeout = atoi(sp + 1) * 60;
          *sp = 0;
        }
        g_logseen = 0;
      }
      if (!g_logseen) {
        if (--g_timeout <= 0) {
          fail("timed out");
          g_logwant[0] = 0;
        }
        return;
      }
      g_logwant[0] = 0;
    } else if (!strcmp(op, "press") || !strcmp(op, "hold")) {
      const int pl = player_of(arg[1]), both = arg[1] && !strcasecmp(arg[1], "both");
      if (first) {
        g_held[pl] = arg[2] ? button(arg[2]) : 0;
        if (both)
          g_held[1] = g_held[0];
        g_wait = arg[3] ? atoi(arg[3]) : 6;
      }
      if (g_wait-- > 0)
        return;
      g_held[pl] = 0;
      if (both)
        g_held[1] = 0;
      if (op[0] == 'p')
        g_gap = 6;
    } else if (!strcmp(op, "stick")) {
      const int pl = player_of(arg[1]);
      if (first) {
        g_stick[pl][0] = arg[2] ? (float)atof(arg[2]) : 0;
        g_stick[pl][1] = arg[3] ? (float)atof(arg[3]) : 0;
        g_stick_on[pl] = 1;
        g_wait = arg[4] ? atoi(arg[4]) : 30;
      }
      if (g_wait-- > 0)
        return;
      g_stick_on[pl] = 0;
    } else if (!strcmp(op, "sleep")) {
      if (first)
        g_wait = arg[1] ? atoi(arg[1]) : 60;
      if (g_wait-- > 0)
        return;
    } else if (!strcmp(op, "shot")) {
      dcr_gl_request_capture_named(arg[1] ? arg[1] : "shot");
    } else if (!strcmp(op, "screens")) {
      debugPrintf("[test] player 1 at %s, player 2 at %s\n", screen_now(0), screen_now(1));
    } else if (!strcmp(op, "quit")) {
      debugPrintf("[test] the script is done: quitting\n");
      log_flush_ring();
      exit(0);
    } else {
      debugPrintf("[test] step %d: unknown: %s\n", g_pc + 1, op);
    }
    g_pc++;
    g_started = 0;
    if (g_gap)
      return;
  }
}

/* player `pl`'s buttons and stick, the script's on top of the controller's */
void ssr_test_pad(int pl, u64 *held, float *lx, float *ly) {
  if (!g_on)
    return;
  *held |= g_held[pl & 1];
  if (g_stick_on[pl & 1])
    *lx = g_stick[pl & 1][0], *ly = g_stick[pl & 1][1];
}
