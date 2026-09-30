/* ssr_menu2.c -- player 2's copy of the engine's menus (split screen:
 * ssr_split.c), driven as player 1's are: ssr_menu.c again, its state its
 * own, for engine 2 -- its Back, its column's prompts and focus. MIT. */
#include "ssr.h"

#define MENU_ENGINE 1
#define MENU_BACK() ssr_split_back2()
#define MENU_PROMPT_SET(p) ssr_prompt2_set(p)
#define MENU_PROMPT_PLACE(y) ((void)(y))
#define MENU_PROMPT_ICON(i, x, y, h, a) ((void)(i), (void)(x), (void)(y), (void)(h), (void)(a))
#define MENU_FOCUS(on, x, y, w, h) ssr_gfx_focus2(on, x, y, w, h)

/* its exports under their own names */
#define ssr_menu_frame ssr_menu2_frame
#define ssr_menu_init ssr_menu2_init
#define ssr_menu_tap ssr_menu2_tap
#define ssr_menu_screen ssr_menu2_screen
#define ssr_menu_screen_ready ssr_menu2_screen_ready
#define ssr_input_pointer ssr_menu2_input_pointer

int ssr_menu2_tap(uint32_t pntr_hash);
const char *ssr_menu2_screen(void);
int ssr_menu2_screen_ready(void);
int ssr_menu2_input_pointer(float *x, float *y, int *pressed);

#include "ssr_menu.c"
