#ifndef __MC_SOUND_H__
#define __MC_SOUND_H__

#include <ultra64.h>

// minecraft's sounds (sound.c). event ids are generated/mc_sounds.h's SND_*
// (when there's no export they're absent: use the SND() macro)
#if __has_include("../generated/mc_sounds.h")
#include "mcdata.h"
#include "../generated/mc_sounds.h"
#endif
#ifdef MC_HAVE_SOUNDS
#define SND(name) SND_##name
#else
#define SND(name) (-1)
#endif

// SoundType's events, in its order
enum { SOUND_BREAK, SOUND_STEP, SOUND_PLACE, SOUND_HIT, SOUND_FALL };

void sound_play_at(int ev, const f32 pos[3], f32 volume, f32 pitch); // bk units
void sound_play_cell(int ev, s32 x, s32 y, s32 z, f32 volume, f32 pitch); // a block's middle
void sound_play_player(int ev, f32 volume, f32 pitch); // Entity.playSound at steve
void sound_play_ui(int ev, f32 volume, f32 pitch);     // SimpleSoundInstance.forUI
void sound_block(u16 block, int which, s32 x, s32 y, s32 z); // the block's SoundType, minecraft's volume / pitch per use
void sound_block_by_player(u16 block, int which); // step (x 0.15) / fall (x 0.5, pitch x 0.75) at steve
void sound_stop_all(void);
f32 sound_random(void); // 0..1

#endif
