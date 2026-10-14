#ifndef __MC_HOTBARS_H__
#define __MC_HOTBARS_H__

#include <ultra64.h>
#include "survival.h"

// creative saved hotbars (HotbarManager / Hotbar): 9 rows of 9, saved to
// ~/.config/BanjoRecompiled/mc_steve_100.bin by item name
int hotbars_is_empty(int h);
const ItemStack *hotbars_get(int h, int i);
void hotbars_save(int h);  // C + number: the current hotbar goes into row h
void hotbars_load(int h);  // X + number: row h replaces the hotbar

// Hud.setOverlayMessage: the text above the hotbar for 60 ticks
void hud_set_overlay_message(const char *text);

#endif
