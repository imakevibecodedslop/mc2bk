#ifndef __MC_PARTICLES_H__
#define __MC_PARTICLES_H__

#include <ultra64.h>

// minecraft's block particles (TerrainParticle): breaking a block
// (ClientLevel.addDestroyBlockEffect, 4x4x4) and the crack bits while mining
// (addBreakingParticles, one a tick on the face being hit).
void particles_destroy_block(s32 cell[3], u16 block);
void particles_crack(s32 cell[3], s32 face_normal[3], u16 block);
void particles_tick(void);
void particles_draw(Gfx **gfx, Mtx **mtx);
void particles_clear(void);
void particles_smoke(const f32 pos_bk[3]);     // SmokeParticle
void particles_explosion(const f32 pos_bk[3]); // EXPLOSION_EMITTER (bk units)
void particles_poof(const f32 pos_bk[3], const f32 vel[3]); // ParticleTypes.POOF (vel in blocks / tick)

#endif
