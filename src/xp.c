// experience, ported from Player (giveExperiencePoints, getXpNeededForNextLevel,
// giveExperienceLevels, takeXpDelay) and ExperienceOrb (26.3): orbs split by
// getExperienceValue, merge with tryMergeToExisting / scanForMerges (id % 40,
// same value), gravity 0.03, drag 0.98 (* block friction 0.6 on the ground),
// bounce at 0.4, follow the nearest player within 8 blocks, 6000 tick
// lifetime, one orb per 2 ticks picked up (playerTouch). drawn with
// ExperienceOrbRenderer: a camera facing 0.3 scale quad, icon by value,
// colour pulsing with age, alpha 128.

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "pose.h"
#include "mc_gbi.h"
#include "xp.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif
#include "sound.h"

#define MAX_ORBS 32
#define ORB_SIZE 0.5f // EntityType.EXPERIENCE_ORB sized(0.5, 0.5)

typedef struct {
    int active;
    s32 id;               // Entity.getId (merge groups: id % 40)
    s32 value, count;
    f32 pos[3], o_pos[3]; // bk units, bottom center
    f32 vel[3];           // blocks per tick
    s32 age, tick_count;
    int on_ground;
    int following;
} Orb;

SteveXp gXp;
static Orb orbs[MAX_ORBS];
static s32 next_id = 1;
static u32 rng = 0x0E0B1234u;

static f32 rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return (f32)(rng >> 8) / 16777216.0f;
}

static s32 rnd_int(s32 n) {
    s32 r = (s32)(rnd() * (f32)n);
    return r >= n ? n - 1 : r;
}

s32 xp_needed_for_next_level(void) {
    if (gXp.level >= 30) return 112 + (gXp.level - 30) * 9;
    return gXp.level >= 15 ? 37 + (gXp.level - 15) * 5 : 7 + gXp.level * 2;
}

// Player.giveExperienceLevels: the level up sound every 5 levels (not
// within 100 ticks of the last one)
static void give_levels(s32 amount) {
    static u32 last_level_up;
    gXp.level += amount;
    if (amount > 0 && gXp.level % 5 == 0 && (s32)last_level_up < (s32)gSteve.tick_count - 100) {
        f32 vol = gXp.level > 30 ? 1.0f : gXp.level / 30.0f;
        sound_play_player(SND(ENTITY_PLAYER_LEVELUP), vol * 0.75f, 1.0f);
        last_level_up = gSteve.tick_count;
    }
    if (gXp.level < 0) {
        gXp.level = 0;
        gXp.progress = 0.0f;
        gXp.total = 0;
    }
}

void xp_give_points(s32 i) {
    gXp.progress += (f32)i / (f32)xp_needed_for_next_level();
    gXp.total += i;
    if (gXp.total < 0) gXp.total = 0;

    while (gXp.progress < 0.0f) {
        f32 remaining = gXp.progress * xp_needed_for_next_level();
        if (gXp.level > 0) {
            give_levels(-1);
            gXp.progress = 1.0f + remaining / xp_needed_for_next_level();
        } else {
            give_levels(-1);
            gXp.progress = 0.0f;
        }
    }
    while (gXp.progress >= 1.0f) {
        gXp.progress = (gXp.progress - 1.0f) * xp_needed_for_next_level();
        give_levels(1);
        gXp.progress = gXp.progress / xp_needed_for_next_level();
    }
}

void xp_reset(void) {
    gXp.level = 0;
    gXp.total = 0;
    gXp.progress = 0.0f;
    gXp.take_delay = 0;
}

void xp_clear(void) {
    int i;
    for (i = 0; i < MAX_ORBS; i++) orbs[i].active = FALSE;
}

// ExperienceOrb.getExperienceValue
static s32 experience_value(s32 max) {
    static const s32 steps[] = { 2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3 };
    int i;
    for (i = 0; i < 10; i++) {
        if (max >= steps[i]) return steps[i];
    }
    return 1;
}

// ExperienceOrb.getIcon
static int orb_icon(s32 value) {
    static const s32 steps[] = { 2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3 };
    int i;
    for (i = 0; i < 10; i++) {
        if (value >= steps[i]) return 10 - i;
    }
    return 0;
}

static int merge_ok(const Orb *o, s32 id, s32 value) {
    s32 d = o->id - id;
    return o->active && (d % 40) == 0 && o->value == value;
}

// boxes (bk units): an orb's centre is pos + 0.25 block up
static int near_box(const f32 a[3], const f32 b[3], f32 half) {
    f32 h = half * MC_BLOCK;
    return a[0] - b[0] < h && b[0] - a[0] < h && a[1] - b[1] < h && b[1] - a[1] < h && a[2] - b[2] < h && b[2] - a[2] < h;
}

// tryMergeToExisting: an orb with the same value in the 1x1x1 box at pos
static int try_merge_existing(const f32 pos[3], s32 value) {
    s32 id = rnd_int(40);
    int i;
    for (i = 0; i < MAX_ORBS; i++) {
        Orb *o = &orbs[i];
        f32 c[3];
        if (!merge_ok(o, id, value)) continue;
        // AABB.ofSize(pos, 1, 1, 1) against the orb's 0.5 box
        c[0] = o->pos[0];
        c[1] = o->pos[1] + ORB_SIZE * 0.5f * MC_BLOCK;
        c[2] = o->pos[2];
        if (near_box(c, pos, 0.5f + ORB_SIZE * 0.5f)) {
            o->count++;
            o->age = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static void spawn_orb(const f32 pos[3], s32 value) {
    int i, slot = 0;
    Orb *o;

    for (i = 0; i < MAX_ORBS; i++) {
        if (!orbs[i].active) { slot = i; break; }
        if (orbs[i].age > orbs[slot].age) slot = i;
    }
    o = &orbs[slot];
    o->active = TRUE;
    o->id = next_id++;
    o->value = value;
    o->count = 1;
    for (i = 0; i < 3; i++) o->pos[i] = o->o_pos[i] = pos[i];
    // the constructor's random push (roughly = zero)
    o->vel[0] = (rnd() * 0.2f - 0.1f) * 2.0f;
    o->vel[1] = rnd() * 0.2f * 2.0f;
    o->vel[2] = (rnd() * 0.2f - 0.1f) * 2.0f;
    o->age = 0;
    o->tick_count = 0;
    o->on_ground = FALSE;
    o->following = FALSE;
}

void xp_award(const f32 pos[3], s32 amount) {
    while (amount > 0) {
        s32 n = experience_value(amount);
        amount -= n;
        if (!try_merge_existing(pos, n)) spawn_orb(pos, n);
    }
}

// scanForMerges: orbs in the box inflated by 0.5 with the same id group and value
static void scan_merges(Orb *self) {
    int i;
    for (i = 0; i < MAX_ORBS; i++) {
        Orb *o = &orbs[i];
        if (o == self || !merge_ok(o, self->id, self->value)) continue;
        if (near_box(o->pos, self->pos, ORB_SIZE + 0.5f)) {
            self->count += o->count;
            if (o->age < self->age) self->age = o->age;
            o->active = FALSE;
        }
    }
}

// followNearbyPlayer: getNearestPlayer(this, 8), not spectating, not dead
static void follow_player(Orb *o) {
    f32 d[3], len2, len, power;

    {
        f32 dx = gSteve.pos[0] - o->pos[0], dy = gSteve.pos[1] - o->pos[1], dz = gSteve.pos[2] - o->pos[2];
        f32 r2 = (dx * dx + dy * dy + dz * dz) / (MC_BLOCK * MC_BLOCK);
        if (!o->following || r2 > 64.0f || GM_SPECTATING()) {
            o->following = r2 <= 64.0f && !GM_SPECTATING() && gSteve.health > 0.0f;
        }
    }
    if (!o->following) return;
    d[0] = (gSteve.pos[0] - o->pos[0]) / MC_BLOCK;
    d[1] = (gSteve.pos[1] + steve_eye_height() / 2.0f - o->pos[1]) / MC_BLOCK;
    d[2] = (gSteve.pos[2] - o->pos[2]) / MC_BLOCK;
    len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    len = __builtin_sqrtf(len2);
    power = 1.0f - len / 8.0f;
    if (len > 1e-4f) {
        int k;
        for (k = 0; k < 3; k++) o->vel[k] += d[k] / len * (power * power * 0.1f);
    }
}

// Entity.move, like the dropped items: horizontal then vertical rays
static void orb_move(Orb *o, int *below) {
    f32 hit[3], n[3], start[3], end[3];
    f32 half = ORB_SIZE * 0.5f * MC_BLOCK;

    *below = FALSE;
    start[0] = o->pos[0];
    start[1] = o->pos[1] + half;
    start[2] = o->pos[2];
    end[0] = start[0] + o->vel[0] * MC_BLOCK + (o->vel[0] > 0 ? half : -half);
    end[1] = start[1];
    end[2] = start[2] + o->vel[2] * MC_BLOCK + (o->vel[2] > 0 ? half : -half);
    if ((o->vel[0] != 0.0f || o->vel[2] != 0.0f) && steve_raycast(start, end, hit, n) && n[1] < 0.7f && n[1] > -0.7f) {
        o->vel[0] = o->vel[2] = 0.0f;
    } else {
        o->pos[0] += o->vel[0] * MC_BLOCK;
        o->pos[2] += o->vel[2] * MC_BLOCK;
    }
    start[0] = end[0] = o->pos[0];
    start[2] = end[2] = o->pos[2];
    if (o->vel[1] <= 0.0f) {
        start[1] = o->pos[1] + half;
        end[1] = o->pos[1] + o->vel[1] * MC_BLOCK - 0.5f;
        if (steve_raycast(start, end, hit, n) && n[1] > 0.5f) {
            o->pos[1] = hit[1];
            o->on_ground = TRUE;
            *below = TRUE;
        } else {
            o->pos[1] += o->vel[1] * MC_BLOCK;
            o->on_ground = FALSE;
        }
    } else {
        start[1] = o->pos[1] + half;
        end[1] = o->pos[1] + ORB_SIZE * MC_BLOCK + o->vel[1] * MC_BLOCK;
        if (steve_raycast(start, end, hit, n)) {
            o->vel[1] = 0.0f;
        } else {
            o->pos[1] += o->vel[1] * MC_BLOCK;
        }
        o->on_ground = FALSE;
    }
}

// Player.touch: steve's box inflated by (1, 0.5, 1) against the orb's box
static int touches_steve(const Orb *o) {
    f32 *p = gSteve.pos;
    f32 r = (STEVE_HALF_WIDTH + 1.0f) * MC_BLOCK, h = ORB_SIZE * 0.5f * MC_BLOCK;
    if (o->pos[0] + h < p[0] - r || o->pos[0] - h > p[0] + r) return FALSE;
    if (o->pos[2] + h < p[2] - r || o->pos[2] - h > p[2] + r) return FALSE;
    if (o->pos[1] + ORB_SIZE * MC_BLOCK < p[1] - 0.5f * MC_BLOCK) return FALSE;
    if (o->pos[1] > p[1] + (STEVE_HEIGHT + 0.5f) * MC_BLOCK) return FALSE;
    return TRUE;
}

static void orb_tick(Orb *o) {
    f32 fall;
    int below, k;

    for (k = 0; k < 3; k++) o->o_pos[k] = o->pos[k];
    o->tick_count++;
    o->vel[1] -= 0.03f; // applyGravity (getDefaultGravity 0.03)
    if (o->tick_count % 20 == 1) scan_merges(o);
    follow_player(o);
    fall = o->vel[1];
    orb_move(o, &below);
    {
        f32 friction = 0.98f; // getAirDrag
        if (o->on_ground) friction *= 0.6f; // Block.getFriction (default)
        for (k = 0; k < 3; k++) o->vel[k] *= friction;
    }
    if (below && fall < -0.03f) {
        o->vel[1] = -fall * 0.4f;
    }
    if (++o->age >= 6000) {
        o->active = FALSE;
    }
}

void xp_tick(void) {
    int i;

    // Player.tick
    if (gXp.take_delay > 0) gXp.take_delay--;
    for (i = 0; i < MAX_ORBS; i++) {
        Orb *o = &orbs[i];
        if (!o->active) continue;
        orb_tick(o);
        // Player.aiStep -> touch (not in spectator, while alive) -> playerTouch
        if (o->active && !GM_SPECTATING() && gSteve.health > 0.0f && touches_steve(o) && gXp.take_delay == 0) {
            gXp.take_delay = 2;
            xp_give_points(o->value); // (no mending: nothing to repair)
            sound_play_player(SND(ENTITY_EXPERIENCE_ORB_PICKUP), 0.1f, (sound_random() - sound_random()) * 0.35f + 0.9f);
            if (--o->count == 0) o->active = FALSE;
        }
    }
}

#define ORB_VERTS 4
static Vtx orb_vtx[3][MAX_ORBS][ORB_VERTS];
static int orb_buf;

void xp_draw(Gfx **gfx, Mtx **mtx, f32 cam[3], f32 pt) {
#ifdef MC_HAVE_TEXTURES
    f32 vrot[3];
    int i, any = FALSE;

    for (i = 0; i < MAX_ORBS; i++) {
        if (orbs[i].active) any = TRUE;
    }
    if (!any) return;
    orb_buf = (orb_buf + 1) % 3;
    viewport_getRotation_vec3f(vrot);

    // RenderTypes.entityTranslucent: blended, both sides
    gDPPipeSync((*gfx)++);
    gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
    gDPSetCombineMode((*gfx)++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
    for (i = 0; i < MAX_ORBS; i++) {
        Orb *o = &orbs[i];
        Vtx *v = orb_vtx[orb_buf][i];
        Pose p;
        f32 rel[3], rr, age;
        u8 rc, bc;
        int icon, k;

        if (!o->active) continue;
        icon = orb_icon(o->value);
        for (k = 0; k < 3; k++) rel[k] = (o->o_pos[k] + (o->pos[k] - o->o_pos[k]) * pt - cam[k]) / MC_BLOCK;
        // ageInTicks: tickCount + partialTick
        age = (f32)o->tick_count + pt;
        rr = age / 2.0f;
        rc = (u8)((ml_sin_deg(rr * 57.2957795f) + 1.0f) * 0.5f * 255.0f);
        bc = (u8)((ml_sin_deg((rr + 4.18879020f) * 57.2957795f) + 1.0f) * 0.1f * 255.0f);

        pose_identity(&p);
        pose_scale(&p, MC_BLOCK, MC_BLOCK, MC_BLOCK);
        pose_translate(&p, rel[0], rel[1] + 0.1f, rel[2]);
        // camera.orientation: face the camera (bk camera: yaw, then pitch)
        pose_rotate(&p, 1, vrot[1]);
        pose_rotate(&p, 0, vrot[0]);
        pose_scale(&p, 0.3f, 0.3f, 0.3f);
        {
            static const f32 c[4][2] = { { -0.5f, -0.25f }, { 0.5f, -0.25f }, { 0.5f, 0.75f }, { -0.5f, 0.75f } };
            static const u8 uv[4][2] = { { 0, 16 }, { 16, 16 }, { 16, 0 }, { 0, 0 } };
            for (k = 0; k < 4; k++) {
                f32 local[3] = { c[k][0], c[k][1], 0.0f }, pos[3];
                pose_apply(&p, local, pos);
                v[k].v.ob[0] = (s16)pos[0];
                v[k].v.ob[1] = (s16)pos[1];
                v[k].v.ob[2] = (s16)pos[2];
                v[k].v.flag = 0;
                v[k].v.tc[0] = (s16)(uv[k][0] << 5);
                v[k].v.tc[1] = (s16)(uv[k][1] << 5);
                v[k].v.cn[0] = rc;
                v[k].v.cn[1] = 255;
                v[k].v.cn[2] = bc;
                v[k].v.cn[3] = 128;
            }
        }
        gDPLoadTextureBlock((*gfx)++, &mc_xp_orb_tex[icon * 256], G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 16, 0,
            G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        mlMtxIdent();
        mlMtxApply(*mtx);
        gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_XP_ORB_START + o->id % 0x10000, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPVertex((*gfx)++, v, ORB_VERTS, 0);
        gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    }
    gDPPipeSync((*gfx)++);
    gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2);
#endif
}
