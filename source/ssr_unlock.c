/* ssr_unlock.c -- [game] unlock_all: every racer, track, Grand Prix cup and
 * mission open, the save untouched (../source/docs/UI_EXTRAS.md 1).
 *
 * Four small accessors of STProfileProgressData decide every lock in the
 * game (there are no separate vehicles; the All-Star moves are not locked):
 *   IsCharacterOwned(id)      racers    (character select, the lobby)
 *   IsStageOwned(id)          tracks    (track select, the lobby)
 *   IsGPCupUnlocked(diff,cup) Grand Prix difficulties and cups
 *   GetTotalMissionStars()    missions  (each needs so many stars)
 * They are replaced (an 8-byte jump at load) by the same functions, which
 * answer "open" while the option is on -- no code copies their answers into
 * the save, so turning it off gives back the player's own progress. Where
 * the true answer matters, the caller (the return address) gets it:
 *   - PostGameHandler::ProcessGPResults, the one place that writes an
 *     unlock: a real win still unlocks the next cup for real (and a cup open
 *     only through the option chains nothing);
 *   - the portrait picker: a portrait is saved with the licence, so it only
 *     offers the racers the player owns;
 *   - the shop: what is really owned shows as SOLD, the rest can be bought
 *     (a purchase is real progress).
 * Results raced in content open only through the option (a cup's rank, a
 * mission's stars, times) are the player's own, and are saved. MIT.
 */
#include <stdint.h>
#include <switch.h>

#include "dcr_config.h"
#include "so_util.h"
#include "ssr.h"
#include "util.h"

/* where a return address lands: its offset in the engine copy it is in, and
 * that copy's g_pGameData (split screen has two copies) */
static uintptr_t rel(uintptr_t lr, uint8_t ***gd) {
  lr &= ~(uintptr_t)1; /* a Thumb caller's */
  so_module *m = so_find_module_by_addr((void *)lr);
  if (!m)
    return 0;
  if (gd)
    *gd = (uint8_t **)so_try_find_addr_rx(m, "g_pGameData");
  return lr - (uintptr_t)m->load_virtbase;
}

static int bit(const uint8_t *p, int off, int id) { return (p[off + (id >> 3)] >> (id & 7)) & 1; }

static int cup_index(int d, int c) { /* STProfileProgressData::RealCupIndex */
  if ((d | c) < 0 || d > 3 || c > 5)
    return 0x13;
  const int i = 6 * d + c;
  return i > 0x13 ? 0x13 : i;
}
static int real_cup(const uint8_t *p, int d, int c) {
  const int i = cup_index(d, c);
  return i <= 0x12 ? p[i] & 1 : 0;
}

/* the shop's callers (IsSold's buy gate and card label, the Sega Miles hint) */
static int shop_caller(uintptr_t r) { return r == 0xef33c || r == 0xef7c4 || r == 0xea85c || r == 0xea8b8; }

__attribute__((noinline)) static int is_character_owned(uint8_t *p, int id) {
  const uintptr_t r = rel((uintptr_t)__builtin_return_address(0), NULL);
  if (id > 0x13)
    return 0;
  if (dcr_config()->unlock_all && r != 0xed890 && r != 0xeda28 && !shop_caller(r))
    return 1;
  return bit(p, 0x33, id);
}

__attribute__((noinline)) static int is_stage_owned(uint8_t *p, int id) {
  const uintptr_t r = rel((uintptr_t)__builtin_return_address(0), NULL);
  if (id > 0x17)
    return 0;
  if (dcr_config()->unlock_all && !shop_caller(r))
    return 1;
  return bit(p, 0x36, id);
}

__attribute__((noinline)) static int is_gp_cup_unlocked(uint8_t *p, int d, int c) {
  uint8_t **gdp = NULL;
  const uintptr_t r = rel((uintptr_t)__builtin_return_address(0), &gdp);
  if (dcr_config()->unlock_all && r == 0xdca8c && gdp && *gdp) {
    /* ProcessGPResults asks whether the cup after the one just won is open */
    const uint8_t *gd = *gdp;
    const int pd = *(const int32_t *)(gd + 0x6ec), pc = (int8_t)gd[0x6bc];
    if (!real_cup(p, pd, pc))
      return 1; /* that cup was open only through the option: chain nothing */
    return real_cup(p, d, c);
  }
  if (dcr_config()->unlock_all)
    return cup_index(d, c) <= 0x12;
  return real_cup(p, d, c);
}

__attribute__((noinline)) static uint32_t total_mission_stars(uint8_t *p) {
  if (dcr_config()->unlock_all)
    return 0xffff;
  uint32_t s = 0;
  for (int i = 0; i < 25; i++)
    s += p[0x13 + i] >> 3;
  return s;
}

int ssr_patch_jump(const char *sym, uint32_t expect, void *dst); /* ssr_patch.c, at load */

/* ssr_patch.c, at load (always: the option is read on each call) */
void ssr_unlock_patch(void) {
  int n = 0;
  n += ssr_patch_jump("_ZN21STProfileProgressData16IsCharacterOwnedEi", 0xe3510013u, (void *)is_character_owned);
  n += ssr_patch_jump("_ZN21STProfileProgressData12IsStageOwnedEi", 0xe3510017u, (void *)is_stage_owned);
  n += ssr_patch_jump("_ZN21STProfileProgressData15IsGPCupUnlockedEii", 0xe92d4010u, (void *)is_gp_cup_unlocked);
  n += ssr_patch_jump("_ZN21STProfileProgressData20GetTotalMissionStarsEv", 0xe3a03000u, (void *)total_mission_stars);
  debugPrintf("[unlock] %d of 4 hooks; everything open: %s\n", n, dcr_config()->unlock_all ? "yes" : "no");
}
