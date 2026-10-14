// minecraft's sounds (SoundManager / SoundEngine.play, WeighedSoundEvents,
// SimpleSoundInstance). the events, entries and the blocks' SoundTypes come
// from tools/mc_sounds.py; the mouse companion mod's native library plays
// the files (mc_sounds/<n>.pcm next to it). without the companion: silence.

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "steve.h"
#include "options.h"
#include "sound.h"

#if __has_include("../generated/mc_sounds.h")
#include "mcdata.h"
#include "../generated/mc_sounds.h"
#endif

#define MOUSE_MOD_ID "bk_minecraft_mouse"
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_snd_play(int file, int vol, int pitch, int pan));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_snd_stop_all(void));

static int available = -1;

static int sound_available(void) {
    if (available < 0) {
        available = recomp_is_dependency_met(MOUSE_MOD_ID) == DEPENDENCY_STATUS_FOUND;
    }
    return available;
}

// RandomSource (the client's, a plain lcg is enough for picking variants)
static u32 seed = 0x2545F491u;
static u32 next_int(u32 bound) {
    seed = seed * 1103515245u + 12345u;
    return bound ? (seed >> 8) % bound : 0;
}
f32 sound_random(void) {
    seed = seed * 1103515245u + 12345u;
    return (f32)(seed >> 8) / 16777216.0f;
}

#ifdef MC_HAVE_SOUNDS

static f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// SoundEngine.play: WeighedSoundEvents.getSound, the instance's volume and
// pitch times the Sound's, linear clamped attenuation over
// max(volume, 1) * attenuation_distance, relative sounds unattenuated
static void play(int ev, const f32 *pos, f32 volume, f32 pitch) {
    const McSoundEvent *e;
    const McSoundEntry *en = NULL;
    int total = 0, i, k;
    f32 v, p, gain = 1.0f, pan = 0.0f;

    if (ev < 0 || ev >= MC_SOUND_EVENT_COUNT || !sound_available()) return;
    e = &mc_sound_events[ev];
    if (e->count == 0) return;
    for (i = 0; i < e->count; i++) total += mc_sound_entries[e->first + i].weight;
    if (total == 0) return;
    k = (int)next_int((u32)total);
    for (i = 0; i < e->count; i++) {
        k -= mc_sound_entries[e->first + i].weight;
        if (k < 0) { en = &mc_sound_entries[e->first + i]; break; }
    }
    if (en == NULL) return;
    v = volume * en->volume;
    p = clampf(pitch * en->pitch, 0.5f, 2.0f);
    if (pos != NULL) {
        f32 eye[3], rot[3], d[3], dist, range = (v > 1.0f ? v : 1.0f) * en->att;
        // the listener is the camera (bk's viewport, which steve's camera sets)
        viewport_getPosition_vec3f(eye);
        viewport_getRotation_vec3f(rot);
        for (i = 0; i < 3; i++) d[i] = (pos[i] - eye[i]) / MC_BLOCK;
        dist = __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        gain = 1.0f - dist / range;
        if (gain <= 0.0f) return;
        if (dist > 0.01f) {
            // which side: the listener's right is (-cos yaw, 0, sin yaw)
            f32 yaw = rot[1] - 180.0f; // (bk's camera yaw faces the other way)
            f32 rx = -ml_cos_deg(yaw), rz = ml_sin_deg(yaw);
            pan = (d[0] * rx + d[2] * rz) / dist;
        }
    }
    v = clampf(v, 0.0f, 1.0f) * option_sound_volume() * gain;
    if (v <= 0.0f) return;
    mc_snd_play(en->file, (int)(v * 4096.0f), (int)(p * 4096.0f), (int)(pan * 4096.0f));
}

void sound_play_at(int ev, const f32 pos[3], f32 volume, f32 pitch) { play(ev, pos, volume, pitch); }
// SimpleSoundInstance.forUI(sound, pitch, volume): relative, no attenuation
void sound_play_ui(int ev, f32 volume, f32 pitch) { play(ev, NULL, volume, pitch); }

void sound_play_cell(int ev, s32 x, s32 y, s32 z, f32 volume, f32 pitch) {
    f32 pos[3];
    pos[0] = (x + 0.5f) * MC_BLOCK;
    pos[1] = (y + 0.5f) * MC_BLOCK;
    pos[2] = (z + 0.5f) * MC_BLOCK;
    play(ev, pos, volume, pitch);
}

// Entity.playSound at steve
void sound_play_player(int ev, f32 volume, f32 pitch) {
    play(ev, gSteve.pos, volume, pitch);
}

// SoundType of a block, and its sounds
static const McSoundType *type_of(u16 block) {
    if (block >= MC_BLOCK_COUNT) block = 0;
    return &mc_sound_types[mc_block_sound_type[block]];
}

// levelEvent 2001 / BlockItem.place: (volume + 1) / 2, pitch * 0.8
void sound_block(u16 block, int which, s32 x, s32 y, s32 z) {
    const McSoundType *t = type_of(block);
    f32 vol = (t->volume + 1.0f) / 2.0f, pitch = t->pitch * 0.8f;
    if (which == SOUND_HIT) {
        vol = (t->volume + 1.0f) / 8.0f; // MultiPlayerGameMode.continueDestroyBlock
        pitch = t->pitch * 0.5f;
    } else if (which == SOUND_STEP) {
        vol = t->volume * 0.15f; // Entity.playStepSound
        pitch = t->pitch;
    } else if (which == SOUND_FALL) {
        vol = t->volume * 0.5f; // LivingEntity.playBlockFallSound
        pitch = t->pitch * 0.75f;
    }
    sound_play_cell(t->ev[which], x, y, z, vol, pitch);
}

// Entity.playStepSound / playBlockFallSound: the block's sound from steve
void sound_block_by_player(u16 block, int which) {
    const McSoundType *t = type_of(block);
    if (which == SOUND_FALL) sound_play_player(t->ev[SOUND_FALL], t->volume * 0.5f, t->pitch * 0.75f);
    else sound_play_player(t->ev[which], t->volume * 0.15f, t->pitch);
}

void sound_stop_all(void) {
    if (sound_available()) mc_snd_stop_all();
}

#else
void sound_play_at(int ev, const f32 pos[3], f32 volume, f32 pitch) { (void)ev; (void)pos; (void)volume; (void)pitch; }
void sound_play_ui(int ev, f32 volume, f32 pitch) { (void)ev; (void)volume; (void)pitch; }
void sound_play_cell(int ev, s32 x, s32 y, s32 z, f32 volume, f32 pitch) { (void)ev; (void)x; (void)y; (void)z; (void)volume; (void)pitch; }
void sound_play_player(int ev, f32 volume, f32 pitch) { (void)ev; (void)volume; (void)pitch; }
void sound_block(u16 block, int which, s32 x, s32 y, s32 z) { (void)block; (void)which; (void)x; (void)y; (void)z; }
void sound_stop_all(void) {}
void sound_block_by_player(u16 block, int which) { (void)block; (void)which; }
#endif
