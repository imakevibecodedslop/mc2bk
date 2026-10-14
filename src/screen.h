#ifndef __MC_SCREEN_H__
#define __MC_SCREEN_H__

#include <ultra64.h>

// minecraft's inventory and crafting table screens (see screen.c)
int screen_is_open(void);
int screen_just_closed(void); // closed this frame (so e/esc doesn't also act in the world)
void screen_clear_just_closed(void);
void screen_open_inventory(void);
void screen_open_crafting(void);
void screen_open_gamemode_switcher(void); // f3 + f4
void screen_open_furnace(s32 pos[3]); // the furnace block at this grid cell
void screen_open_chest(s32 pos[3]);   // the chest block at this grid cell
void screen_close(void);
void screen_frame(void);                   // input, once per game frame
void screen_draw(Gfx **gfx, s32 w, s32 h); // in gui space, from hud.c
void screen_draw_tooltip(Gfx **gfx, const char **lines, const u32 *colors, int n); // at the cursor

#endif
