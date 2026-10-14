#ifndef __MC_ORES_H__
#define __MC_ORES_H__

#include <ultra64.h>

// minecraft's underground (ore veins, granite / diorite / andesite / tuff /
// dirt / gravel blobs, deepslate) for a stone cell d blocks under bk's ground
u16 ores_block_at(s32 x, s32 z, s32 d);
void ores_reset(void); // a new map

#endif
