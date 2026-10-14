#ifndef __MC_TNT_H__
#define __MC_TNT_H__

#include <ultra64.h>

// TNT (TntBlock, PrimedTnt, TntRenderer, ServerExplosion) and fire (FireBlock)
void tnt_prime(const s32 cell[3], int fuse);      // a primed tnt at the block (fuse 80, or short)
void tnt_explode(const f32 center[3], f32 radius); // Level.explode (bk units)
void tnt_tick(void);                               // every 20 tps tick
void tnt_clear(void);                              // map change
void tnt_draw(Gfx **gfx, Mtx **mtx, f32 cam[3], f32 pt);
// BaseFireBlock.canBePlacedAt + place (flint and steel)
int fire_place(const s32 cell[3]);
void fire_sides(s32 x, s32 y, s32 z, u8 out[5]); // north, east, south, west, up: 1 false 2 true

#endif
