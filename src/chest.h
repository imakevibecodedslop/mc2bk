#ifndef __MC_CHEST_H__
#define __MC_CHEST_H__

#include <ultra64.h>
#include "survival.h"

// chests (ChestBlock, ChestBlockEntity, ChestLidController, ChestRenderer).
// single chests only. their contents are kept per map and saved with the
// game (mc_steve_<20 + slot>.bin).
#define CHEST_SLOTS 27

typedef struct {
    int used;
    s16 map;
    s32 pos[3];
    ItemStack items[CHEST_SLOTS];
    f32 openness, o_openness; // ChestLidController
    int viewers;              // ContainerOpenersCounter: steve's screen
} Chest;

Chest *chest_at(const s32 pos[3], int create); // this map's chest at a block
void chest_removed(const s32 pos[3]);          // the block is gone (contents already dropped)
int chest_blocked(const s32 pos[3]);           // ChestBlock.isChestBlockedAt
void chests_tick(void);                        // lids
void chests_draw(Gfx **gfx, Mtx **mtx, f32 cam[3], f32 pt);
void chests_reset(void);                       // a new world: no chests anywhere
#include "pose.h"
void chest_emit_item(Gfx **gfx, const Pose *unit, int view_ortho); // the chest item model in a pose's unit cube

#endif
