#ifndef __MC_TERRAIN_H__
#define __MC_TERRAIN_H__

#include <ultra64.h>

// mining bk's own level (see terrain.c)

void terrain_reset(void); // map loaded: nothing carved
int terrain_carve(s32 x, s32 y, s32 z); // remove the level inside this grid cell
int terrain_is_carved(s32 x, s32 y, s32 z);
void terrain_carve_begin(void); // batch: rebuild once at terrain_carve_end
void terrain_carve_end(void);
f32 terrain_level_min_y(void); // the level's lowest point (bk units)
int terrain_cell_surface_top(s32 x, s32 y, s32 z); // 1..16: the ground's height in the cell
int terrain_cell_touches_ground(s32 x, s32 y, s32 z); // the surface passes through (or above) the cell
// the cells mined out of this map (for saving)
int terrain_carved_count(void);
const s32 *terrain_carved_cell(int i);
// cells under the level's body with no floor over them, counted as ground
// (see cell_under_shell). saved for the carved ones, put back before carving
int terrain_carved_shell(int i, f32 *depth);
int terrain_uncarve(s32 x, s32 y, s32 z); // fill a dug cell back in: bk's ground returns
void terrain_shell_restore(s32 x, s32 y, s32 z, f32 depth);
// what minecraft block this bit of bk terrain counts as (grass/dirt/stone,
// oak log / leaves for trees)
u16 terrain_block_at(s32 x, s32 y, s32 z);
// is any of this cell under bk's (original) ground? walls/paintings/cliffs
// are thin shells with nothing behind them, so only ground is mineable
int terrain_cell_in_ground(s32 x, s32 y, s32 z);
// line query against the leftover collision pieces (for world.c's provider)
void *terrain_line_query(f32 start[3], f32 end[3], f32 out_normal[3], u32 flag_filter);
void terrain_draw(Gfx **gfx, Mtx **mtx);

#endif
