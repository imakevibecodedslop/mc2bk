#ifndef __MC_XP_H__
#define __MC_XP_H__

#include <ultra64.h>

// steve's experience (Player: experienceLevel / totalExperience /
// experienceProgress) and experience orbs (ExperienceOrb)
typedef struct {
    s32 level;
    s32 total;
    f32 progress;
    s32 take_delay; // Player.takeXpDelay
} SteveXp;

extern SteveXp gXp;

s32 xp_needed_for_next_level(void);       // Player.getXpNeededForNextLevel
void xp_give_points(s32 amount);           // Player.giveExperiencePoints
void xp_award(const f32 pos[3], s32 amount); // ExperienceOrb.award (bk units)
void xp_tick(void);                        // every 20 tps tick
void xp_clear(void);                       // map change: orbs go away
void xp_draw(Gfx **gfx, Mtx **mtx, f32 cam[3], f32 pt); // 3d pass
void xp_reset(void);                       // new file / death (no keepInventory)

#endif
