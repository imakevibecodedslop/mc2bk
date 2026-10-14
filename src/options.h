#ifndef __MC_OPTIONS_H__
#define __MC_OPTIONS_H__

#include <ultra64.h>

int option_starter_kit(void);   // give the kit to new saves
int option_toggle_sprint(void); // Options.toggleSprint
int option_ore_depth_minecraft(void); // ores at minecraft's heights (else 4x shallower, a quarter of the veins)
int option_keep_terrain(void);  // holes + cut trees are saved (the owner's "chaotic" world)
f32 option_fov(void);           // Options.fov (30..110, 70 = Normal)
int option_skin_slim(void);    // PlayerModel slim arms (the skin model option)
f32 option_sound_volume(void); // 0..1, the minecraft sounds' master volume

#endif
