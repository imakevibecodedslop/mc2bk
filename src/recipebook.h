#ifndef __MC_RECIPEBOOK_H__
#define __MC_RECIPEBOOK_H__

#include <ultra64.h>
#include "survival.h"

// the crafting recipe book (RecipeBookComponent), see recipebook.c
void recipebook_tick(void);              // unlocks (inventory_changed), every tick
ItemId recipebook_take_new_unlock(void);  // next unlocked recipe's result, 0 none (RecipeToast)
void recipebook_reset(void);             // a new world
int recipebook_visible(void);
void recipebook_toggle(void);
int recipebook_narrow(s32 w);
s32 recipebook_left_pos(s32 w, s32 image_w);
int recipebook_contains(s32 w, s32 h, s32 mx, s32 my);
void recipebook_draw(Gfx **gfx, s32 w, s32 h, s32 mx, s32 my, const ItemStack *grid, int grid_w);
void recipebook_draw_ghost(Gfx **gfx, s32 left, s32 top, const ItemStack *grid, int grid_w, s32 gx, s32 gy, s32 rx, s32 ry);
const char *recipebook_tooltip(void);
int recipebook_click(s32 w, s32 h, s32 mx, s32 my, int button, int shift, ItemStack *grid, int grid_w);
void recipebook_clear_ghost(void);
// the search box
int recipebook_search_focused(void);
void recipebook_focus_search(void);
void recipebook_type(char c);
void recipebook_backspace(void);
// saving
int recipebook_save_count(void);
const char *recipebook_save_name(int k);
void recipebook_load_name(const char *name);
int recipebook_settings(void);
void recipebook_set_settings(int f);

#endif
