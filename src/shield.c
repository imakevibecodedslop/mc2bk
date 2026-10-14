// the shield (ShieldItem + BlocksAttacks from Items.SHIELD, ShieldModel,
// ShieldSpecialRenderer, item/shield.json + item/shield_blocking.json display
// transforms). texture entity/shield/shield_base_nopattern.png (64x64), no
// banner patterns.

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "bk_externs.h"
#include "steve.h"
#include "survival.h"
#include "items.h"
#include "pose.h"
#include "shield.h"
#include "sound.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

// --- using it ---

int shield_using(void) {
    const ItemStack *st;
    if (gSurv.use_remaining <= 0) return FALSE;
    st = &gSurv.inv[gSurv.use_slot < INV_TOTAL ? gSurv.use_slot : gSurv.selected];
    return st->count > 0 && st->id == ITEM_SHIELD;
}

int shield_using_offhand(void) {
    return shield_using() && gSurv.use_slot == INV_OFFHAND;
}

// LivingEntity.isBlocking: using it for at least blockDelayTicks (0.25 s)
int shield_blocking(void) {
    return shield_using() && gSurv.use_duration - gSurv.use_remaining >= 5;
}

// LivingEntity.applyItemBlocking with BlocksAttacks.resolveBlockedDamage:
// the one DamageReduction (90 degrees, base 0, factor 1) takes the whole hit
// from a source in front; hurtBlockingItem: ItemDamageFunction(3, 1, 1).
// returns the damage left over
f32 shield_absorb(f32 damage, const f32 *src) {
    f32 dx, dz, l, lx, lz;
    ItemStack *st;

    if (!shield_blocking() || src == NULL || damage <= 0.0f) return damage;
    dx = src[0] - gSteve.pos[0];
    dz = src[2] - gSteve.pos[2];
    l = __builtin_sqrtf(dx * dx + dz * dz);
    if (l < 0.001f) return damage;
    lx = ml_sin_deg(gSteve.yaw);
    lz = ml_cos_deg(gSteve.yaw);
    // acos(look . to_source) > 90 degrees: not blocked
    if ((dx * lx + dz * lz) / l < 0.0f) return damage;
    st = &gSurv.inv[gSurv.use_slot < INV_TOTAL ? gSurv.use_slot : gSurv.selected];
    if (!gSurv.creative && damage >= 3.0f && item_max_damage(st->id) > 0) {
        st->damage += (u16)(1.0f + damage); // floor(base + factor * damage)
        if (st->damage >= item_max_damage(st->id)) {
            // entity event 30: SHIELD_BREAK (0.8, 0.8 + r * 0.4)
            sound_play_player(SND(ITEM_SHIELD_BREAK), 0.8f, 0.8f + sound_random() * 0.4f);
            st->id = 0; // broken
            st->count = 0;
            st->damage = 0;
            gSurv.use_remaining = 0;
        }
    }
    // entity event 29: SHIELD_BLOCK (1.0, 0.8 + r * 0.4)
    sound_play_player(SND(ITEM_SHIELD_BLOCK), 1.0f, 0.8f + sound_random() * 0.4f);
    return 0.0f;
}

// --- display transforms (ItemTransform: translation / 16, rotation x y z,
// scale; the left hand mirrors x translation and y / z rotation) ---

typedef struct {
    f32 rot[3], trans[3], scale[3];
} Disp;

static const Disp disp_normal[SHIELD_CTX_COUNT] = {
    { { 0, 90, 0 }, { 10, 6, -4 }, { 1, 1, 1 } },        // thirdperson_righthand
    { { 0, 90, 0 }, { 10, 6, 12 }, { 1, 1, 1 } },        // thirdperson_lefthand
    { { 0, 180, 5 }, { -10, 1.75f, -10 }, { 1.25f, 1.25f, 1.25f } }, // firstperson_righthand
    { { 0, 180, 5 }, { 10, 0, -10 }, { 1.25f, 1.25f, 1.25f } },      // firstperson_lefthand
    { { 15, -25, -5 }, { 2, 3, 0 }, { 0.65f, 0.65f, 0.65f } },       // gui
    { { 0, 0, 0 }, { 2, 4, 2 }, { 0.25f, 0.25f, 0.25f } },           // ground
};
static const Disp disp_blocking[SHIELD_CTX_COUNT] = {
    { { 45, 155, 0 }, { -3.49f, 11, -2 }, { 1, 1, 1 } },
    { { 45, 155, 0 }, { 11.51f, 7, 2.5f }, { 1, 1, 1 } },
    { { 0, 180, -5 }, { -15, 3.25f, -11 }, { 1.25f, 1.25f, 1.25f } },
    { { 0, 180, -5 }, { 5, 5, -11 }, { 1.25f, 1.25f, 1.25f } },
    { { 15, -25, -5 }, { 2, 3, 0 }, { 0.65f, 0.65f, 0.65f } },
    { { 0, 0, 0 }, { 2, 4, 2 }, { 0.25f, 0.25f, 0.25f } },
};

void shield_display(Pose *p, int ctx, int blocking) {
    const Disp *d = blocking ? &disp_blocking[ctx] : &disp_normal[ctx];
    int left = (ctx == SHIELD_THIRD_LEFT || ctx == SHIELD_FIRST_LEFT);
    f32 m = left ? -1.0f : 1.0f;

    pose_translate(p, m * d->trans[0] / 16.0f, d->trans[1] / 16.0f, d->trans[2] / 16.0f);
    pose_rotate(p, 0, d->rot[0]);
    pose_rotate(p, 1, m * d->rot[1]);
    pose_rotate(p, 2, m * d->rot[2]);
    pose_scale(p, d->scale[0], d->scale[1], d->scale[2]);
    // the item renderer's -0.5, then the shield model's own scale(1, -1, -1)
    pose_translate(p, -0.5f, -0.5f, -0.5f);
    pose_scale(p, 1.0f, -1.0f, -1.0f);
    pose_scale(p, 1.0f / 16.0f, 1.0f / 16.0f, 1.0f / 16.0f);
}

// --- the model ---

// ShieldModel.createLayer: texOffs u, v, box min x y z, size w h d
const ShieldBox shield_boxes[2] = {
    { 0, 0, { -6, -11, -2 }, { 12, 22, 1 } }, // plate
    { 26, 0, { -1, -3, -1 }, { 2, 6, 6 } },   // handle
};

static const u8 quads[6][4] = { { 5, 4, 0, 1 }, { 2, 3, 7, 6 }, { 0, 4, 7, 3 }, { 1, 0, 3, 2 }, { 5, 1, 2, 6 }, { 4, 5, 6, 7 } };
static const u8 shade6[6] = { 128, 255, 153, 204, 153, 204 }; // down up west north east south

// one face of a ModelPart.Cube: the 4 corners (box space), its uvs
void shield_box_face(const ShieldBox *b, int f, f32 c4[4][3], f32 tex[4][2]) {
    f32 x0 = b->min[0], y0 = b->min[1], z0 = b->min[2];
    f32 x1 = x0 + b->size[0], y1 = y0 + b->size[1], z1 = z0 + b->size[2];
    f32 w = b->size[0], h = b->size[1], d = b->size[2];
    f32 u0 = b->u, u1 = u0 + d, u2 = u0 + d + w, u22 = u0 + d + w + w, u3 = u0 + d + w + d, u4 = u3 + w;
    f32 v0 = b->v, v1 = v0 + d, v2 = v0 + d + h;
    f32 c8[8][3] = {
        { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
        { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 },
    };
    const f32 uvs[6][4] = {
        { u1, v0, u2, v1 }, { u2, v1, u22, v0 }, { u0, v1, u1, v2 },
        { u1, v1, u2, v2 }, { u2, v1, u3, v2 }, { u3, v1, u4, v2 },
    };
    int i, k;
    for (i = 0; i < 4; i++) for (k = 0; k < 3; k++) c4[i][k] = c8[quads[f][i]][k];
    // Polygon: vertex 0 (u1, v0), 1 (u0, v0), 2 (u0, v1), 3 (u1, v1)
    tex[0][0] = uvs[f][2]; tex[0][1] = uvs[f][1];
    tex[1][0] = uvs[f][0]; tex[1][1] = uvs[f][1];
    tex[2][0] = uvs[f][0]; tex[2][1] = uvs[f][3];
    tex[3][0] = uvs[f][2]; tex[3][1] = uvs[f][3];
}

u8 shield_face_shade(int f) {
    return shade6[f];
}

#define RING 16
static Vtx ring_vtx[3][RING][12 * 4];
static int ring, ring_frame;

// the model through a pose already in camera space (world pass / hand).
// faces turned away from the viewer are left out (no depth buffer in the
// hand); plate_first: the back of the shield faces the viewer (held), so the
// handle goes on top
void shield_emit(Gfx **gfx, const Pose *p, int view_ortho, int plate_first) {
#ifdef MC_HAVE_TEXTURES
    Vtx *v;
    int bi, f, i, n = 0;

    ring = (ring + 1) % RING;
    if (ring == 0) ring_frame = (ring_frame + 1) % 3;
    v = ring_vtx[ring_frame][ring];
    for (bi = 0; bi < 2; bi++) {
        const ShieldBox *b = &shield_boxes[plate_first ? bi : 1 - bi];
        f32 bm[3] = { b->min[0] + b->size[0] / 2, b->min[1] + b->size[1] / 2, b->min[2] + b->size[2] / 2 }, mid[3];
        pose_apply(p, bm, mid);
        for (f = 0; f < 6; f++) {
            f32 c4[4][3], tex[4][2], pts[4][3], qc[3] = { 0, 0, 0 }, out[3], umin, umax, vmin, vmax;
            Vtx *q = &v[n * 4];
            int visible;

            shield_box_face(b, f, c4, tex);
            for (i = 0; i < 4; i++) {
                int k;
                pose_apply(p, c4[i], pts[i]);
                for (k = 0; k < 3; k++) qc[k] += pts[i][k] / 4.0f;
            }
            for (i = 0; i < 3; i++) out[i] = qc[i] - mid[i];
            visible = view_ortho ? out[2] > 0.0f : (qc[0] * out[0] + qc[1] * out[1] + qc[2] * out[2]) < 0.0f;
            if (!visible) continue;
            umin = umax = tex[0][0];
            vmin = vmax = tex[0][1];
            for (i = 1; i < 4; i++) {
                if (tex[i][0] < umin) umin = tex[i][0];
                if (tex[i][0] > umax) umax = tex[i][0];
                if (tex[i][1] < vmin) vmin = tex[i][1];
                if (tex[i][1] > vmax) vmax = tex[i][1];
            }
            if (umax <= umin || vmax <= vmin) continue;
            for (i = 0; i < 4; i++) {
                q[i].v.ob[0] = (s16)pts[i][0];
                q[i].v.ob[1] = (s16)pts[i][1];
                q[i].v.ob[2] = (s16)pts[i][2];
                q[i].v.flag = 0;
                q[i].v.tc[0] = (s16)(tex[i][0] * 32.0f);
                q[i].v.tc[1] = (s16)(tex[i][1] * 32.0f);
                q[i].v.cn[0] = q[i].v.cn[1] = q[i].v.cn[2] = shade6[f];
                q[i].v.cn[3] = 0xFF;
            }
            gDPLoadTextureTile((*gfx)++, mc_shield_tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 64, 64,
                (s32)umin, (s32)vmin, (s32)umax - 1, (s32)vmax - 1, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
                G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            gSPVertex((*gfx)++, q, 4, 0);
            gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
            n++;
        }
    }
#endif
}
