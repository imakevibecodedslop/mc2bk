#ifndef __MC_ADVANCEMENTS_H__
#define __MC_ADVANCEMENTS_H__

#include <ultra64.h>
#include "mcdata.h"

// advancements: progress, toasts and the screen (advancements.c)
void advancements_tick(void);                 // inventory_changed criteria + the recipe toast
void advancements_consumed(ItemId item);      // ate / drank something
// kills: mob = the minecraft mob the bk enemy counts as (mask: zombie 1,
// cow 2, cod 4: surv_mob_mask)
void advancements_killed_mob(int mob);        // steve's killing blow on a bk enemy
void advancements_killed_by_mob(int mob);     // a bk enemy took the last honeycomb
void advancements_reset(void);                // a new world
int advancements_save_count(void);
void advancements_save_entry(int k, const char **adv, const char **crit);
void advancements_load_entry(const char *adv, const char *crit);
void advancements_draw_toasts(Gfx **gfx, s32 w);
int advancements_open(void);
void advancements_set_open(int o);
void advancements_draw(Gfx **gfx, s32 w, s32 h, s32 mx, s32 my);
int advancements_input(s32 w, s32 h, f32 mx, f32 my, int clicks, int held, int wheel); // TRUE: Done pressed

#endif
