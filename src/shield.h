#ifndef __MC_SHIELD_H__
#define __MC_SHIELD_H__

#include <ultra64.h>
#include "pose.h"

// the shield: blocking (ShieldItem, BlocksAttacks) and its model
// (ShieldModel + the item/shield display transforms)
enum { SHIELD_THIRD_RIGHT, SHIELD_THIRD_LEFT, SHIELD_FIRST_RIGHT, SHIELD_FIRST_LEFT, SHIELD_GUI, SHIELD_GROUND, SHIELD_CTX_COUNT };

typedef struct {
    u8 u, v;
    f32 min[3], size[3];
} ShieldBox;
extern const ShieldBox shield_boxes[2];

int shield_using(void);         // the item being used is a shield
int shield_using_offhand(void);
int shield_blocking(void);      // LivingEntity.isBlocking (0.25 s in)
f32 shield_absorb(f32 damage, const f32 *src); // damage left after blocking a hit from src (NULL: no source)
void shield_display(Pose *p, int ctx, int blocking); // display transform + -0.5 + the model's own flip, to model pixels
void shield_emit(Gfx **gfx, const Pose *p, int view_ortho, int plate_first);
void shield_box_face(const ShieldBox *b, int f, f32 c4[4][3], f32 tex[4][2]);
u8 shield_face_shade(int f);

#endif
