// a tiny copy of minecraft's PoseStack: 3x4 affine matrix, column vectors,
// every call multiplies on the right. shared by the steve model, the hand and
// the hud's block icons.
#ifndef __MC_POSE_H__
#define __MC_POSE_H__

#include <ultra64.h>
#include "bk_externs.h"

typedef struct {
    f32 m[3][4];
} Pose;

static inline void pose_identity(Pose *p) {
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            p->m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
}

static inline void pose_translate(Pose *p, f32 x, f32 y, f32 z) {
    int i;
    for (i = 0; i < 3; i++) {
        p->m[i][3] += p->m[i][0] * x + p->m[i][1] * y + p->m[i][2] * z;
    }
}

static inline void pose_scale(Pose *p, f32 x, f32 y, f32 z) {
    int i;
    for (i = 0; i < 3; i++) {
        p->m[i][0] *= x;
        p->m[i][1] *= y;
        p->m[i][2] *= z;
    }
}

// rotate around axis 0 = x, 1 = y, 2 = z (right handed, degrees)
static inline void pose_rotate(Pose *p, int axis, f32 deg) {
    f32 c, s;
    int a = (axis + 1) % 3, b = (axis + 2) % 3; // the two columns that mix
    int i;

    if (deg == 0.0f) {
        return;
    }
    c = ml_cos_deg(deg);
    s = ml_sin_deg(deg);
    for (i = 0; i < 3; i++) {
        f32 ca = p->m[i][a], cb = p->m[i][b];
        p->m[i][a] = ca * c + cb * s;
        p->m[i][b] = -ca * s + cb * c;
    }
}

static inline void pose_apply(const Pose *p, const f32 in[3], f32 out[3]) {
    int i;
    for (i = 0; i < 3; i++) {
        out[i] = p->m[i][0] * in[0] + p->m[i][1] * in[1] + p->m[i][2] * in[2] + p->m[i][3];
    }
}

#endif
