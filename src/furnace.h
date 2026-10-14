#ifndef __MC_FURNACE_H__
#define __MC_FURNACE_H__

#include <ultra64.h>
#include "survival.h"

// furnaces placed in the grid (AbstractFurnaceBlockEntity). they keep cooking
// while their screen is closed, until the map changes.

typedef struct {
    int used;
    s32 pos[3];
    ItemStack items[3];   // 0 input, 1 fuel, 2 result
    s32 lit_remaining, lit_total;
    s32 cook_timer, cook_total;
    // recipesUsed: smelting recipe index -> times (awarded as xp on take/break)
    u16 used_recipe[4], used_count[4];
} Furnace;

Furnace *furnace_at(s32 pos[3], int create);
void furnace_set_input(Furnace *f, const ItemStack *st); // setItem(0): resets the cook time if it changed
void furnace_removed(s32 pos[3]);   // the block was broken: contents drop
void furnaces_tick(void);           // every tick
void furnaces_clear(void);          // map change
int furnace_is_fuel(ItemId id);
int furnace_can_smelt(ItemId id);
void furnace_award_xp(Furnace *f, s32 pos[3]); // getRecipesToAwardAndPopExperience

#endif
