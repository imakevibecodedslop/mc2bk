#ifndef __MC_SHAPES_H__
#define __MC_SHAPES_H__

#include <ultra64.h>
#include "mcdata.h"

// the shaped block families: slabs, stairs, doors, fences, gates, panes,
// ladders, lanterns, chains, snow, carpets, plants (shapes.c)
int shapes_family(BlockId b); // FAM_*
// a family cell's outline (collision = 0) or collision boxes (1/16 block,
// y up to 24); -1 if it isn't a family block
int shapes_boxes(u16 cell, int collision, McBox *out, int max);
int shapes_model_state(u16 cell); // its mc_model_states row (variants) / sides mask (fence, pane)
void shapes_on_set(s32 x, s32 y, s32 z); // world_set changed this cell: updateShape around it
void shapes_suppress(int on);             // no updates while a saved world is put back
// getStateForPlacement: 1 = state set, 0 = not a family block, -1 = can't go there.
// hit = the click inside the new cell (0..1)
int shapes_place(BlockId *block, int *state, s32 pos[3], const s32 normal[3], const f32 hit[3], const f32 look[3]);
void shapes_set_placed(s32 pos[3], u16 cell); // world_set + a door's upper half
// canBeReplaced: a same slab doubled / a snow layer more. TRUE with the new cell
int shapes_merge(BlockId held, s32 cell[3], int replacing_clicked, const s32 normal[3], f32 hit_y, u16 *out);
int shapes_use(s32 pos[3], const f32 look[3]); // doors, fence gates
int shapes_drop_count(u16 cell);               // a double slab: 2, snow: layers
int shapes_climbable(s32 x, s32 y, s32 z);     // ladders (BlockTags.CLIMBABLE)

#endif
