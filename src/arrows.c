// steve's arrows, ported from AbstractArrow.tick (26.3): they fly straight
// along their velocity each tick (a level raycast for the move, then bk's
// enemies along it), slow by 0.99 (air) or 0.6 (water), fall 0.05 per tick,
// stick in the ground (shakeTime 7, gone after 1200 ticks) and can be picked
// up there. damage on a hit: ceil(speed * 2) (+ random(damage / 2 + 2) for a
// crit). drawn with ArrowModel / ArrowRenderer (entity/projectiles/arrow.png).

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "items.h"
#include "pose.h"
#include "mc_gbi.h"
#include "arrows.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif
#include "sound.h"

#define MAX_ARROWS 16

typedef struct {
    int active;
    f32 pos[3], o_pos[3]; // bk units
    f32 vel[3];           // blocks per tick
    f32 x_rot, y_rot, o_x_rot, o_y_rot;
    int in_ground;
    int life;             // ticks stuck in the ground
    int shake;            // shakeTime
    int crit;
    int pickup;           // Pickup.ALLOWED (shot in survival)
} Arrow;

static Arrow arrows[MAX_ARROWS];
static u32 rng = 0x5EED1234u;

static f32 rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return (f32)(rng >> 8) / 16777216.0f;
}

static f32 atan2_deg(f32 y, f32 x) {
    // atan2 in degrees (Mth.atan2 * 180 / PI)
    f32 ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, a, r;
    if (ax < 1e-6f && ay < 1e-6f) return 0.0f;
    a = (ax > ay) ? ay / ax : ax / ay;
    r = a * (0.9998660f + a * a * (-0.3302995f + a * a * (0.1801410f + a * a * (-0.0851330f + a * a * 0.0208351f))));
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159265f - r;
    if (y < 0) r = -r;
    return r * 57.2957795f;
}

static void face_velocity(Arrow *a) {
    f32 h = __builtin_sqrtf(a->vel[0] * a->vel[0] + a->vel[2] * a->vel[2]);
    a->y_rot = atan2_deg(a->vel[0], a->vel[2]);
    a->x_rot = atan2_deg(a->vel[1], h);
}

void arrows_spawn(f32 pos[3], f32 vel[3], int crit, int pickup) {
    int i, slot = 0;

    for (i = 0; i < MAX_ARROWS; i++) {
        if (!arrows[i].active) { slot = i; break; }
        if (arrows[i].life > arrows[slot].life) slot = i; // reuse the oldest stuck one
    }
    {
        Arrow *a = &arrows[slot];
        int k;
        a->active = TRUE;
        for (k = 0; k < 3; k++) {
            a->pos[k] = a->o_pos[k] = pos[k];
            a->vel[k] = vel[k];
        }
        face_velocity(a);
        a->o_x_rot = a->x_rot;
        a->o_y_rot = a->y_rot;
        a->in_ground = FALSE;
        a->life = 0;
        a->shake = 0;
        a->crit = crit;
        a->pickup = pickup;
    }
}

void arrows_clear(void) {
    int i;
    for (i = 0; i < MAX_ARROWS; i++) arrows[i].active = FALSE;
}

static void arrow_tick(Arrow *a) {
    int k;

    for (k = 0; k < 3; k++) a->o_pos[k] = a->pos[k];
    a->o_x_rot = a->x_rot;
    a->o_y_rot = a->y_rot;
    if (a->shake > 0) a->shake--;

    if (a->in_ground) {
        // tickDespawn, and Arrow.playerTouch: pickup when steve walks into it
        f32 dx = gSteve.pos[0] - a->pos[0], dy = gSteve.pos[1] + MC_BLOCK - a->pos[1], dz = gSteve.pos[2] - a->pos[2];
        if (++a->life >= 1200) {
            a->active = FALSE;
            return;
        }
        if (dx * dx + dz * dz < (0.8f * MC_BLOCK) * (0.8f * MC_BLOCK) && dy > -1.2f * MC_BLOCK && dy < 1.2f * MC_BLOCK
            && a->shake <= 0) {
            if (!a->pickup || gSurv.creative || surv_inv_add(ITEM_ARROW, 1) == 0) {
                a->active = FALSE;
            }
        }
        return;
    }

    {
        f32 to[3], hit[3], n[3];
        int stuck;

        for (k = 0; k < 3; k++) to[k] = a->pos[k] + a->vel[k] * MC_BLOCK;
        stuck = steve_raycast(a->pos, to, hit, n);
        // entities first along the part of the move before the wall
        {
            f32 end[3];
            f32 speed = __builtin_sqrtf(a->vel[0] * a->vel[0] + a->vel[1] * a->vel[1] + a->vel[2] * a->vel[2]);
            s32 dmg = (s32)(speed * 2.0f); // baseDamage 2
            if ((f32)dmg < speed * 2.0f) dmg++; // ceil
            if (a->crit) dmg += (s32)(rnd() * (dmg / 2 + 2));
            for (k = 0; k < 3; k++) end[k] = stuck ? hit[k] : to[k];
            if (steve_arrow_hit(a->pos, end, (f32)dmg)) {
                sound_play_at(SND(ENTITY_ARROW_HIT), end, 1.0f, 1.2f / (rnd() * 0.2f + 0.9f)); // onHitEntity
                a->active = FALSE; // discard on hit
                return;
            }
        }
        if (stuck) {
            // onHitBlock: stop just short of the surface, shakeTime 7
            for (k = 0; k < 3; k++) {
                a->pos[k] = hit[k] - a->vel[k] * 0.05f * MC_BLOCK / 3.0f;
                a->vel[k] = 0.0f;
            }
            a->in_ground = TRUE;
            a->shake = 7;
            sound_play_at(SND(ENTITY_ARROW_HIT), a->pos, 1.0f, 1.2f / (rnd() * 0.2f + 0.9f)); // getHitGroundSoundEvent
            return;
        }
        for (k = 0; k < 3; k++) a->pos[k] = to[k];
    }
    face_velocity(a);
    // applyInertia (0.99 in air, getWaterInertia 0.6 in water), applyGravity 0.05
    {
        f32 drag = (player_getWaterState() != 0 && a->pos[1] < gSteve.pos[1]) ? 0.6f : 0.99f;
        for (k = 0; k < 3; k++) a->vel[k] *= drag;
        a->vel[1] -= 0.05f;
    }
}

void arrows_tick(void) {
    int i;
    for (i = 0; i < MAX_ARROWS; i++) {
        if (arrows[i].active) arrow_tick(&arrows[i]);
    }
}

// --- drawing (ArrowRenderer + ArrowModel) ---

#define ARROW_VERTS 12
static Vtx arrow_vtx[3][MAX_ARROWS][ARROW_VERTS];
static int arrow_buf;

// one plane of the model (a 0-thick ModelPart box): corners in model pixels,
// uv in texels of the 32x32 texture
static void arrow_plane(Vtx *v, const Pose *p, const f32 c[4][3], const f32 uv[4][2]) {
    int i;
    for (i = 0; i < 4; i++) {
        f32 pos[3];
        f32 local[3] = { c[i][0] / 16.0f, c[i][1] / 16.0f, c[i][2] / 16.0f };
        pose_apply(p, local, pos);
        v[i].v.ob[0] = (s16)pos[0];
        v[i].v.ob[1] = (s16)pos[1];
        v[i].v.ob[2] = (s16)pos[2];
        v[i].v.flag = 0;
        v[i].v.tc[0] = (s16)(uv[i][0] * 32.0f);
        v[i].v.tc[1] = (s16)(uv[i][1] * 32.0f);
        v[i].v.cn[0] = v[i].v.cn[1] = v[i].v.cn[2] = 0xFF;
        v[i].v.cn[3] = 0xFF;
    }
}

void arrows_draw(Gfx **gfx, Mtx **mtx, f32 cam[3], f32 pt) {
#ifdef MC_HAVE_TEXTURES
    int i, k, loaded = FALSE;

    arrow_buf = (arrow_buf + 1) % 3;
    for (i = 0; i < MAX_ARROWS; i++) {
        Arrow *a = &arrows[i];
        Pose p, cross;
        Vtx *v = arrow_vtx[arrow_buf][i];
        f32 rel[3];

        if (!a->active) continue;
        if (!loaded) {
            gDPLoadTextureBlock((*gfx)++, mc_arrow_tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 32, 32, 0,
                G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            loaded = TRUE;
        }
        for (k = 0; k < 3; k++) rel[k] = (a->o_pos[k] + (a->pos[k] - a->o_pos[k]) * pt - cam[k]) / MC_BLOCK;
        pose_identity(&p);
        pose_scale(&p, MC_BLOCK, MC_BLOCK, MC_BLOCK);
        pose_translate(&p, rel[0], rel[1], rel[2]);
        // ArrowRenderer: rotY(yRot - 90), rotZ(xRot); the model is scaled 0.9
        pose_rotate(&p, 1, (a->o_y_rot + (a->y_rot - a->o_y_rot) * pt) - 90.0f);
        pose_rotate(&p, 2, a->o_x_rot + (a->x_rot - a->o_x_rot) * pt);
        if (a->shake > 0) {
            // setupAnim: the root wobbles while it shakes in the ground
            f32 sh = (f32)a->shake - pt;
            pose_rotate(&p, 2, -ml_sin_deg(sh * 3.0f * 57.2957795f) * sh);
        }
        pose_scale(&p, 0.9f, 0.9f, 0.9f);
        // cross_1 / cross_2: box (-12, -2, 0) 16 x 4 x 0, rotated 45 / 135 about x
        for (k = 0; k < 2; k++) {
            static const f32 c[4][3] = { { -12, -2, 0 }, { 4, -2, 0 }, { 4, 2, 0 }, { -12, 2, 0 } };
            static const f32 uv[4][2] = { { 0, 5 }, { 16, 5 }, { 16, 0 }, { 0, 0 } };
            cross = p;
            pose_rotate(&cross, 0, k == 0 ? 45.0f : 135.0f);
            arrow_plane(&v[k * 4], &cross, c, uv);
        }
        // back: box (0, -2.5, -2.5) 0 x 5 x 5 at x -11, rotated 45 about x, scale 0.8
        {
            static const f32 c[4][3] = { { 0, -2.5f, -2.5f }, { 0, -2.5f, 2.5f }, { 0, 2.5f, 2.5f }, { 0, 2.5f, -2.5f } };
            static const f32 uv[4][2] = { { 0, 10 }, { 5, 10 }, { 5, 5 }, { 0, 5 } };
            cross = p;
            pose_translate(&cross, -11.0f / 16.0f, 0.0f, 0.0f);
            pose_rotate(&cross, 0, 45.0f);
            pose_scale(&cross, 0.8f, 0.8f, 0.8f);
            arrow_plane(&v[8], &cross, c, uv);
        }
        mlMtxIdent();
        mlMtxApply(*mtx);
        gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_ARROW_START + i, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPVertex((*gfx)++, v, ARROW_VERTS, 0);
        gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
        gSP2Triangles((*gfx)++, 4, 5, 6, 0, 4, 6, 7, 0);
        gSP2Triangles((*gfx)++, 8, 9, 10, 0, 8, 10, 11, 0);
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    }
#endif
}
