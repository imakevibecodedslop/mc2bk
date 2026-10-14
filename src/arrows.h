#ifndef __MC_ARROWS_H__
#define __MC_ARROWS_H__

#include <ultra64.h>

// arrows shot by steve's bow (AbstractArrow / Arrow)
// pos: bk units, vel: blocks per tick (Projectile.shoot already applied)
void arrows_spawn(f32 pos[3], f32 vel[3], int crit, int pickup);
void arrows_tick(void);      // once per 20 tps tick
void arrows_clear(void);     // map change
void arrows_draw(Gfx **gfx, Mtx **mtx, f32 cam[3], f32 pt); // 3d pass, texture state set

#endif
