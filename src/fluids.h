#ifndef __MC_FLUIDS_H__
#define __MC_FLUIDS_H__

#include <ultra64.h>

// water and lava in the grid (FlowingFluid, WaterFluid, LavaFluid,
// LiquidBlock, LiquidBlockRenderer, BucketItem). a fluid cell's state is
// LiquidBlock's legacy level: 0 source, 1-7 flowing (amount 8 - level),
// 8+ falling.
enum { FLUID_NONE, FLUID_WATER, FLUID_LAVA };

int fluid_type_at(s32 x, s32 y, s32 z);
// the fluid surface over steve's column at his feet .. head (bk units), or
// FALSE if no grid fluid of that type is there
int fluid_surface(int type, const f32 pos[3], f32 height, f32 *surface);
// BucketItem.use. TRUE if the click did something
int fluid_bucket_use(const f32 eye[3], const f32 dir[3], f32 reach);
// EntityFluidInteraction current: adds the push to vel (blocks / tick)
void fluid_push(const f32 pos[3], f32 half_width, f32 height, f32 vel[3]);
void fluids_on_change(s32 x, s32 y, s32 z); // world_set calls this
void fluids_tick(void);
void fluids_clear(void);
void fluids_draw(Gfx **gfx, Mtx **mtx, f32 cam[3]); // 3d pass, after the opaque things

#endif
