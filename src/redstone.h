#ifndef __MC_REDSTONE_H__
#define __MC_REDSTONE_H__

#include <ultra64.h>
#include "mcdata.h"

// redstone and the blocks you click (levers, buttons, trapdoors), ported from
// RedstoneWireBlock / DefaultRedstoneWireEvaluator, LeverBlock, ButtonBlock,
// RedstoneTorchBlock, RedstoneWallTorchBlock, RedstoneLampBlock,
// TrapDoorBlock, StandingAndWallBlockItem and Level's signal methods (26.3).
//
// cell states (CELL_STATE, 6 bits), the model state is the low bits:
//   lever / buttons:  face * 8 + facing * 2 + powered   (face: floor, wall, ceiling)
//   redstone torch:   lit;  wall torch: facing * 2 + lit
//   torch: 0;  wall torch: facing
//   redstone lamp:    lit
//   trapdoors:        facing * 4 + half * 2 + open, + 16 powered
//   redstone wire:    power, + 16 dot (clicked into a dot)
// facing: 0 south, 1 north, 2 east, 3 west (the mod's face index - 2)

// which mc_model_states row a model cell draws (variants blocks)
int redstone_model_state(u16 cell);
// RedstoneWireBlock.getConnectionState: per side (north, east, south, west)
// 1 none, 2 side, 4 up
void redstone_wire_sides(s32 x, s32 y, s32 z, u16 cell, u8 out[4]);
// RedStoneWireBlock COLORS (power -> rgb)
u32 redstone_wire_color(int power);

// the block an item places (BlockItem.getBlock, redstone -> redstone_wire)
BlockId redstone_item_block(u16 item);
// getStateForPlacement for the model blocks: may swap a standing torch for
// its wall torch. normal = the clicked face's normal (into the new cell),
// hit_y = where on the clicked face (0..1 in the cell, for trapdoors),
// look = steve's look direction. returns FALSE if it can't be placed.
int redstone_place(BlockId *block, int *state, const s32 normal[3], f32 hit_y, const f32 look[3], s32 pos[3]);
// useWithoutItem: levers, buttons, wooden trapdoors, dust (dot / cross).
// TRUE if the click was used
int redstone_use(s32 pos[3]);

int redstone_powered(s32 pos[3]); // Level.hasNeighborSignal
int redstone_plate_pressed(const s32 pos[3]); // steve on the plate at pos
void redstone_tick(void);  // every 20 tps tick: scheduled ticks, then signals
void redstone_clear(void); // map change

#endif
