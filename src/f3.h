#ifndef __MC_F3_H__
#define __MC_F3_H__

#include <ultra64.h>

// the f3 debug screen (DebugScreenOverlay), drawn in the hud's gui pass
void f3_draw(Gfx **gfx, s32 w, s32 h);
int f3_crosshair_3d(void);                       // the 3d crosshair replaces the sprite
void f3_draw_crosshair(Gfx **gfx, s32 w, s32 h);

#endif
