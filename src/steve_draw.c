// steve's model, ported from minecraft:
//   - parts, pivots, boxes and skin uvs: HumanoidModel.createMesh + PlayerModel
//     (left arm / left leg use their own skin areas like the 64x64 skin)
//   - box faces and uvs: ModelPart.Cube (same corner order and uv rectangles)
//   - animation: HumanoidModel.setupAnim / setupAttackAnimation (WHACK)
//   - placement: LivingEntityRenderer (rotY(180 - bodyRot), scale(-1,-1,1),
//     translate(0,-1.501,0)) and the player's 0.9375 scale
//   - first person hand: FirstPersonHandsAndItemsRenderer.renderPlayerArm
// the skin comes from generated/mc_textures.h (made from your own jar by
// tools/export_mc_assets.py). without it the model is flat colored.
//
// minecraft's world axes match bk's once yaw is flipped: minecraft yRot =
// -bk yaw, both face (sin yaw, 0, cos yaw). so the whole chain is used as is,
// in blocks, then scaled to bk units.
//
// vertices are built on the cpu every frame relative to the camera (the
// modelview is just a translation of 0), and faces turned away from the
// camera are dropped, so the hand can be drawn without a depth buffer.

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "boat.h"
#include "items.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

#define MC_PI 3.14159265f
#define RAD_TO_DEG 57.2957795f

// atan for |x| small-ish (the inventory preview), good to ~0.001 rad
static f32 atan_approx(f32 x) {
    f32 ax = (x < 0.0f) ? -x : x, r;
    int inv = ax > 1.0f;

    if (inv) ax = 1.0f / ax;
    r = ax * (0.9998660f + ax * ax * (-0.3302995f + ax * ax * (0.1801410f + ax * ax * (-0.0851330f + ax * ax * 0.0208351f))));
    if (inv) r = 1.57079637f - r;
    return (x < 0.0f) ? -r : r;
}

static f32 dr_sqrtf(f32 x) {
    return (x > 0.0f) ? __builtin_sqrtf(x) : 0.0f;
}

static f32 sin_r(f32 rad) {
    return ml_sin_deg(rad * RAD_TO_DEG);
}

static f32 cos_r(f32 rad) {
    return ml_cos_deg(rad * RAD_TO_DEG);
}

#include "pose.h"
#include "mc_gbi.h"
#include "shield.h"
#include "options.h"

// --- the model (HumanoidModel / PlayerModel, wide arms) ---

enum { PART_HEAD, PART_BODY, PART_RARM, PART_LARM, PART_RLEG, PART_LLEG, PART_COUNT };

typedef struct {
    f32 box_min[3];  // model pixels, minecraft model space (y down)
    f32 box_size[3];
    u8 tex_u, tex_v; // texOffs on the 64x64 skin
    f32 pivot[3];    // PartPose offset
    u8 flat[3];      // color when there's no skin
} PartDef;

static const PartDef part_defs[PART_COUNT] = {
    { { -4, -8, -4 }, { 8, 8, 8 }, 0, 0, { 0, 0, 0 }, { 0xB4, 0x84, 0x6C } },
    { { -4, 0, -2 }, { 8, 12, 4 }, 16, 16, { 0, 0, 0 }, { 0x00, 0xA8, 0xA8 } },
    { { -3, -2, -2 }, { 4, 12, 4 }, 40, 16, { -5, 2, 0 }, { 0xB4, 0x84, 0x6C } },
    { { -1, -2, -2 }, { 4, 12, 4 }, 32, 48, { 5, 2, 0 }, { 0xB4, 0x84, 0x6C } },
    { { -2, 0, -2 }, { 4, 12, 4 }, 0, 16, { -1.9f, 12, 0 }, { 0x3A, 0x3A, 0x9E } },
    { { -2, 0, -2 }, { 4, 12, 4 }, 16, 48, { 1.9f, 12, 0 }, { 0x3A, 0x3A, 0x9E } },
};

// PlayerModel.createMesh(slim): slim arms are 3 wide (the right one starts
// at -2), set from the skin model option each frame
static int skin_slim;

static void part_box(int part, f32 mn[3], f32 sz[3]) {
    const PartDef *d = &part_defs[part];
    int i;
    for (i = 0; i < 3; i++) {
        mn[i] = d->box_min[i];
        sz[i] = d->box_size[i];
    }
    if (skin_slim && (part == PART_RARM || part == PART_LARM)) {
        sz[0] = 3.0f;
        if (part == PART_RARM) mn[0] = -2.0f;
    }
}

// PlayerModel's outer layer: hat (on the head, +0.5), jacket, sleeves and
// pants (+0.25), each following its part, cut out by the skin's alpha
typedef struct {
    u8 part, u, v;
    f32 grow;
} Overlay;
#define OVERLAY_COUNT 6
static const Overlay overlays[OVERLAY_COUNT] = {
    { PART_HEAD, 32, 0, 0.5f },   // hat
    { PART_BODY, 16, 32, 0.25f }, // jacket
    { PART_RARM, 40, 32, 0.25f }, // right_sleeve
    { PART_LARM, 48, 48, 0.25f }, // left_sleeve
    { PART_RLEG, 0, 32, 0.25f },  // right_pants
    { PART_LLEG, 0, 48, 0.25f },  // left_pants
};

typedef struct {
    f32 x, y, z;          // pivot (can move, e.g. crouching / swinging)
    f32 xRot, yRot, zRot; // radians
} PartPose;

static void part_reset(PartPose *pp, int part) {
    pp->x = part_defs[part].pivot[0];
    pp->y = part_defs[part].pivot[1];
    pp->z = part_defs[part].pivot[2];
    pp->xRot = pp->yRot = pp->zRot = 0.0f;
}

// ModelPart.translateAndRotate: translate(pivot / 16), then rotationZYX
static void part_apply(Pose *p, const PartPose *pp) {
    pose_translate(p, pp->x / 16.0f, pp->y / 16.0f, pp->z / 16.0f);
    pose_rotate(p, 2, pp->zRot * RAD_TO_DEG);
    pose_rotate(p, 1, pp->yRot * RAD_TO_DEG);
    pose_rotate(p, 0, pp->xRot * RAD_TO_DEG);
}

// --- building a part's vertices (ModelPart.Cube) ---

// directional shading for entity faces, like minecraft's entity lighting
// from above-front: up brightest, down darkest
static const u8 cube_face_shade[6] = { 128, 255, 153, 204, 153, 204 }; // down, up, west, north, east, south

// vertex buffers, rotated so the gpu never reads one we're rewriting
#define VTX_BUFFERS 3
#define PARTS_PER_FRAME (PART_COUNT + 1) // third person parts + the hand
static Vtx part_vtx[VTX_BUFFERS][PARTS_PER_FRAME][24];
static u8 part_face_on[VTX_BUFFERS][PARTS_PER_FRAME][6];
static int vtx_buffer;
static Vtx tp_item_vtx[VTX_BUFFERS][2][24]; // right, left hand

static void set_vtx(Vtx *v, const f32 pos[3], f32 u, f32 t, const u8 col[3], u8 shade) {
    v->v.ob[0] = (s16)pos[0];
    v->v.ob[1] = (s16)pos[1];
    v->v.ob[2] = (s16)pos[2];
    v->v.flag = 0;
    v->v.tc[0] = (s16)(u * 32.0f);
    v->v.tc[1] = (s16)(t * 32.0f);
    v->v.cn[0] = (col[0] * shade) / 255;
    v->v.cn[1] = (col[1] * shade) / 255;
    v->v.cn[2] = (col[2] * shade) / 255;
    v->v.cn[3] = 0xFF;
}

// fills 24 verts (6 faces x 4) for one box, positions already in bk units
// relative to the camera. marks faces turned away from the camera as off.
static int build_ortho; // build_part: cull for the gui camera (looks down -z) instead of the world camera

// one ModelPart.Cube: box in model pixels (y down), inflated by `grow`
// (CubeDeformation: the uvs keep the box's own size), texOffs (u, v),
// mirror like Cube(mirror): min x and max x trade places
static void build_box(const Pose *p, const f32 bmin[3], const f32 bsize[3], f32 grow, f32 u, f32 v, int mirror,
    const u8 col[3], Vtx *out, u8 *face_on) {
    f32 minX = bmin[0] - grow, minY = bmin[1] - grow, minZ = bmin[2] - grow;
    f32 maxX = bmin[0] + bsize[0] + grow, maxY = bmin[1] + bsize[1] + grow, maxZ = bmin[2] + bsize[2] + grow;
    f32 w = bsize[0], h = bsize[1], dp = bsize[2];
    f32 u0 = u, u1 = u + dp, u2 = u + dp + w, u22 = u + dp + w + w, u3 = u + dp + w + dp, u4 = u + dp + w + dp + w;
    f32 v0 = v, v1 = v + dp, v2 = v + dp + h;
    f32 corners[8][3]; // t0..t3 (minZ), l0..l3 (maxZ)
    // Cube: faces as corner indices (t0=0 t1=1 t2=2 t3=3 l0=4 l1=5 l2=6 l3=7) + uv rect (u0, v0, u1, v1)
    const u8 face_idx[6][4] = {
        { 5, 4, 0, 1 }, // down  {l1, l0, t0, t1}
        { 2, 3, 7, 6 }, // up    {t2, t3, l3, l2}
        { 0, 4, 7, 3 }, // west  {t0, l0, l3, t3}
        { 1, 0, 3, 2 }, // north {t1, t0, t3, t2}
        { 5, 1, 2, 6 }, // east  {l1, t1, t2, l2}
        { 4, 5, 6, 7 }, // south {l0, l1, l2, l3}
    };
    const f32 face_uv[6][4] = {
        { u1, v0, u2, v1 },
        { u2, v1, u22, v0 },
        { u0, v1, u1, v2 },
        { u1, v1, u2, v2 },
        { u2, v1, u3, v2 },
        { u3, v1, u4, v2 },
    };
    f32 center[3] = { 0.0f, 0.0f, 0.0f };
    int c, f, i;

    if (mirror) {
        f32 t = minX;
        minX = maxX;
        maxX = t;
    }
    for (c = 0; c < 8; c++) {
        f32 m[3];
        int idx = c & 3;

        // t0 (min,min) t1 (max,min) t2 (max,max) t3 (min,max) in x,y
        m[0] = (idx == 1 || idx == 2) ? maxX : minX;
        m[1] = (idx >= 2) ? maxY : minY;
        m[2] = (c >= 4) ? maxZ : minZ;
        m[0] /= 16.0f;
        m[1] /= 16.0f;
        m[2] /= 16.0f;
        pose_apply(p, m, corners[c]);
        for (i = 0; i < 3; i++) {
            center[i] += corners[c][i] * 0.125f;
        }
    }

    for (f = 0; f < 6; f++) {
        const u8 *q = face_idx[f];
        const f32 *r = face_uv[f];
        f32 e1[3], e2[3], n[3], fc[3], out_dot, view_dot;

        for (i = 0; i < 3; i++) {
            e1[i] = corners[q[1]][i] - corners[q[0]][i];
            e2[i] = corners[q[2]][i] - corners[q[0]][i];
            fc[i] = (corners[q[0]][i] + corners[q[2]][i]) * 0.5f;
        }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        // the camera is at the origin. keep faces whose outward side faces it.
        out_dot = n[0] * (fc[0] - center[0]) + n[1] * (fc[1] - center[1]) + n[2] * (fc[2] - center[2]);
        view_dot = n[0] * fc[0] + n[1] * fc[1] + n[2] * fc[2];
        if (out_dot < 0.0f) {
            view_dot = -view_dot;
        }
        if (build_ortho) {
            // outward z toward the viewer
            f32 oz = (out_dot < 0.0f) ? -n[2] : n[2];
            face_on[f] = oz > 0.0f;
        } else {
            face_on[f] = view_dot < 0.0f;
        }

        // Polygon: vertex 0 = (u1, v0), 1 = (u0, v0), 2 = (u0, v1), 3 = (u1, v1)
        // (r = { u0, v0, u1, v1 })
        set_vtx(&out[f * 4 + 0], corners[q[0]], r[2], r[1], col, cube_face_shade[f]);
        set_vtx(&out[f * 4 + 1], corners[q[1]], r[0], r[1], col, cube_face_shade[f]);
        set_vtx(&out[f * 4 + 2], corners[q[2]], r[0], r[3], col, cube_face_shade[f]);
        set_vtx(&out[f * 4 + 3], corners[q[3]], r[2], r[3], col, cube_face_shade[f]);
    }
}

static void build_part(const Pose *p, int part, Vtx *out, u8 *face_on) {
    static const u8 white[3] = { 0xFF, 0xFF, 0xFF };
    const PartDef *d = &part_defs[part];
    const u8 *col;

#ifdef MC_HAVE_TEXTURES
    col = white;
#else
    col = d->flat;
    (void)white;
#endif
    {
        f32 mn[3], sz[3];
        part_box(part, mn, sz);
        build_box(p, mn, sz, 0.0f, d->tex_u, d->tex_v, FALSE, col, out, face_on);
    }
}

static void build_overlay(const Pose *p, int o, Vtx *out, u8 *face_on) {
    static const u8 white[3] = { 0xFF, 0xFF, 0xFF };
    f32 mn[3], sz[3];
    part_box(overlays[o].part, mn, sz);
    build_box(p, mn, sz, overlays[o].grow, overlays[o].u, overlays[o].v, FALSE, white, out, face_on);
}

// --- render state ---

static void steve_set_render_state(Gfx **gfx, int zbuffer) {
    gDPPipeSync((*gfx)++);
    gDPSetCycleType((*gfx)++, G_CYC_1CYCLE);
    gSPClearGeometryMode((*gfx)++, G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_CULL_BOTH | G_ZBUFFER | G_FOG);
    gSPSetGeometryMode((*gfx)++, G_SHADE | G_SHADING_SMOOTH | (zbuffer ? G_ZBUFFER : 0));
#ifdef MC_HAVE_TEXTURES
    gDPSetTexturePersp((*gfx)++, G_TP_PERSP);
    gDPSetTextureLOD((*gfx)++, G_TL_TILE);
    gDPSetTextureLUT((*gfx)++, G_TT_NONE);
    gDPSetTextureDetail((*gfx)++, G_TD_CLAMP);
    gDPSetTextureConvert((*gfx)++, G_TC_FILT);
    gDPSetTextureFilter((*gfx)++, G_TF_POINT);
    gDPSetCombineKey((*gfx)++, G_CK_NONE);
    gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
    if (zbuffer) {
        gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    } else {
        gDPSetRenderMode((*gfx)++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
    }
    gDPSetCombineMode((*gfx)++, G_CC_MODULATERGB, G_CC_MODULATERGB);
    gSPTexture((*gfx)++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
#else
    if (zbuffer) {
        gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    } else {
        gDPSetRenderMode((*gfx)++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
    }
    gDPSetCombineMode((*gfx)++, G_CC_SHADE, G_CC_SHADE);
    gSPTexture((*gfx)++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
#endif
}

// LivingEntityRenderer: hasRedOverlay while hurtTime > 0. minecraft's overlay
// texture is 0xB2FF0000, mixed as mix(red, color, 0.7): 30% red. done in the
// second combiner cycle: (red - color) * 0.3 + color.
static void steve_red_overlay(Gfx **gfx, int zbuffer) {
#ifdef MC_HAVE_TEXTURES
    if (gSteve.hurt_time <= 0) {
        return;
    }
    gDPPipeSync((*gfx)++);
    gDPSetCycleType((*gfx)++, G_CYC_2CYCLE);
    gDPSetPrimColor((*gfx)++, 0, 0, 0xFF, 0x00, 0x00, 77);
    gDPSetCombineLERP((*gfx)++, TEXEL0, 0, SHADE, 0, 0, 0, 0, 1,
        PRIMITIVE, COMBINED, PRIMITIVE_ALPHA, COMBINED, 0, 0, 0, 1);
    if (zbuffer) {
        gDPSetRenderMode((*gfx)++, G_RM_PASS, G_RM_AA_ZB_OPA_SURF2);
    } else {
        gDPSetRenderMode((*gfx)++, G_RM_PASS, G_RM_AA_OPA_SURF2);
    }
#endif
}

// put back what bk's model setup expects. bk models set their own
// combine/render modes, but not always the cycle type.
static void steve_restore_render_state(Gfx **gfx) {
    gDPPipeSync((*gfx)++);
    gDPSetTextureFilter((*gfx)++, G_TF_BILERP);
    gSPTexture((*gfx)++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gDPPipelineMode((*gfx)++, G_PM_1PRIMITIVE);
    gDPSetCycleType((*gfx)++, G_CYC_2CYCLE);
    gSPSetGeometryMode((*gfx)++, G_ZBUFFER);
}

// emits one built part: loads its rectangle of the skin (the whole 64x64
// skin is 8 KB, twice the n64's texture memory), then the visible faces
static void emit_part(Gfx **gfx, int part, Vtx *v, const u8 *face_on) {
    int f;

#ifdef MC_HAVE_TEXTURES
    {
        const PartDef *d = &part_defs[part];
        f32 mn[3], sz[3];
        s32 uls = d->tex_u, ult = d->tex_v, lrs, lrt;
        part_box(part, mn, sz);
        lrs = uls + (s32)(2 * sz[2] + 2 * sz[0]) - 1;
        lrt = ult + (s32)(sz[2] + sz[1]) - 1;

        gDPLoadTextureTile((*gfx)++, mc_skin_tex, G_IM_FMT_RGBA, G_IM_SIZ_16b, MC_SKIN_W, MC_SKIN_H,
            uls, ult, lrs, lrt, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
            G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    }
#endif
    gSPVertex((*gfx)++, v, 24, 0);
    for (f = 0; f < 6; f++) {
        s32 k = f * 4;
        if (face_on[f]) {
            gSP2Triangles((*gfx)++, k, k + 1, k + 2, 0, k, k + 2, k + 3, 0);
        }
    }
}

// an outer layer box: its rectangle of the skin, the visible faces
static void emit_overlay(Gfx **gfx, int o, Vtx *v, const u8 *face_on) {
    int f;
#ifdef MC_HAVE_TEXTURES
    {
        f32 mn[3], sz[3];
        s32 uls = overlays[o].u, ult = overlays[o].v, lrs, lrt;
        part_box(overlays[o].part, mn, sz);
        lrs = uls + (s32)(2 * sz[2] + 2 * sz[0]) - 1;
        lrt = ult + (s32)(sz[2] + sz[1]) - 1;
        gDPLoadTextureTile((*gfx)++, mc_skin_tex, G_IM_FMT_RGBA, G_IM_SIZ_16b, MC_SKIN_W, MC_SKIN_H,
            uls, ult, lrs, lrt, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
            G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    }
#endif
    gSPVertex((*gfx)++, v, 24, 0);
    for (f = 0; f < 6; f++) {
        s32 k = f * 4;
        if (face_on[f]) gSP2Triangles((*gfx)++, k, k + 1, k + 2, 0, k, k + 2, k + 3, 0);
    }
}

// the outer layer's render state: alpha tested (the skin's 1 bit alpha)
static void overlay_state(Gfx **gfx, int zbuffer) {
    gDPPipeSync((*gfx)++);
    gDPSetAlphaCompare((*gfx)++, G_AC_THRESHOLD);
    gDPSetBlendColor((*gfx)++, 0, 0, 0, 0x80);
    if (zbuffer) {
        gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2);
    } else {
        gDPSetRenderMode((*gfx)++, G_RM_AA_TEX_EDGE, G_RM_AA_TEX_EDGE2);
    }
    gDPSetCombineMode((*gfx)++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
}

// --- the custom skin (mc_skin.png in the config folder: the mouse
// companion's native library reads + processLegacySkin's it) ---

#define MOUSE_MOD_ID "bk_minecraft_mouse"
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_skin_poll(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_skin_px(int i));

static void skin_tick(void) {
#ifdef MC_HAVE_TEXTURES
    static int checked, have_mouse, last_gen, frames;
    static u16 default_skin[64 * 64];
    static int saved_default;
    int gen, i;

    skin_slim = option_skin_slim();
    if (!checked) {
        checked = TRUE;
        have_mouse = recomp_is_dependency_met(MOUSE_MOD_ID) == DEPENDENCY_STATUS_FOUND;
    }
    if (!have_mouse || frames++ % 30 != 0) return;
    if (!saved_default) {
        for (i = 0; i < 64 * 64; i++) default_skin[i] = mc_skin_tex[i];
        saved_default = TRUE;
    }
    gen = mc_skin_poll();
    if (gen == last_gen) return;
    last_gen = gen;
    if (gen <= 0) {
        for (i = 0; i < 64 * 64; i++) mc_skin_tex[i] = default_skin[i]; // no file: steve
        recomp_printf("[mc] skin: default\n");
        return;
    }
    // 0xRRGGBBAA -> rgba 5551 (the alpha bit: at least half)
    for (i = 0; i < 64 * 64; i++) {
        u32 c = (u32)mc_skin_px(i);
        u32 r = (c >> 27) & 31, g = (c >> 19) & 31, b = (c >> 11) & 31, a = (c & 0xFF) >= 128;
        mc_skin_tex[i] = (u16)((r << 11) | (g << 6) | (b << 1) | a);
    }
    recomp_printf("[mc] skin: custom (%s)\n", skin_slim ? "slim" : "classic");
#endif
}

// --- third person ---

// HumanoidModel.setupAnim + setupAttackAnimation for what steve can do
// the bow's item model: bow, or bow_pulling_0/1/2 by use_duration * 0.05
// (thresholds 0.65 and 0.9) while it's being drawn
static const u32 *steve_item_sprite(u16 id) {
#ifdef MC_HAVE_TEXTURES
    if (id == ITEM_BOW && gSurv.use_remaining > 0) {
        f32 d = (f32)(gSurv.use_duration - gSurv.use_remaining) * 0.05f;
        int frame = (d >= 0.9f) ? 2 : (d >= 0.65f ? 1 : 0);
        return &mc_bow_pulling[frame * 256];
    }
#endif
    return item_sprite(id);
}

// Mth.rotLerpRad: lerp the shortest way around the circle
static f32 rot_lerp_rad(f32 a, f32 from, f32 to) {
    f32 d = to - from;
    while (d < -MC_PI) d += 2.0f * MC_PI;
    while (d >= MC_PI) d -= 2.0f * MC_PI;
    return from + a * d;
}

static void steve_setup_anim(PartPose pp[PART_COUNT], f32 pt) {
    Steve *s = &gSteve;
    f32 pos = steve_lerp(s->o_walk_pos, s->walk_pos, pt);
    f32 speed = steve_lerp(s->o_walk_speed, s->walk_speed, pt);
    f32 swing = steve_attack_anim(pt);
    int i;

    if (speed > 1.0f) speed = 1.0f;
    for (i = 0; i < PART_COUNT; i++) {
        part_reset(&pp[i], i);
    }

    pp[PART_HEAD].xRot = -s->pitch / RAD_TO_DEG; // minecraft xRot is + looking down
    // netHeadYaw = yHeadRot - yBodyRot (the head turns, the body lags behind)
    {
        f32 d = -s->yaw - steve_lerp(s->o_body_rot, s->body_rot, pt);
        while (d >= 180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        pp[PART_HEAD].yRot = d / RAD_TO_DEG;
    }
    pp[PART_RARM].xRot = cos_r(pos * 0.6662f + MC_PI) * 2.0f * speed * 0.5f;
    pp[PART_LARM].xRot = cos_r(pos * 0.6662f) * 2.0f * speed * 0.5f;
    pp[PART_RLEG].xRot = cos_r(pos * 0.6662f) * 1.4f * speed;
    pp[PART_LLEG].xRot = cos_r(pos * 0.6662f + MC_PI) * 1.4f * speed;
    pp[PART_RLEG].yRot = 0.005f;
    pp[PART_LLEG].yRot = -0.005f;
    pp[PART_RLEG].zRot = 0.005f;
    pp[PART_LLEG].zRot = -0.005f;
    // HumanoidModel.setupAnim, isPassenger (sitting in a boat; the walk
    // animation doesn't run while riding)
    if (boat_riding()) {
        pp[PART_RARM].xRot = 0.0f;
        pp[PART_LARM].xRot = 0.0f;
        pp[PART_RARM].xRot += -MC_PI / 5.0f;
        pp[PART_LARM].xRot += -MC_PI / 5.0f;
        pp[PART_RLEG].xRot = -1.4137167f;
        pp[PART_RLEG].yRot = MC_PI / 10.0f;
        pp[PART_RLEG].zRot = 0.07853982f;
        pp[PART_LLEG].xRot = -1.4137167f;
        pp[PART_LLEG].yRot = -MC_PI / 10.0f;
        pp[PART_LLEG].zRot = -0.07853982f;
    }

    // poseRightArm, ArmPose.ITEM: holding something lifts the arm a little
    // poseLeftArm, ArmPose.ITEM: the offhand's item
    if (gSurv.inv[INV_OFFHAND].count > 0) {
        pp[PART_LARM].xRot = pp[PART_LARM].xRot * 0.5f - MC_PI / 10.0f;
    }
    if (surv_held_item() != 0) {
        pp[PART_RARM].xRot = pp[PART_RARM].xRot * 0.5f - MC_PI / 10.0f;
    }

    // setupAttackAnimation, WHACK, right arm
    if (swing > 0.0f) {
        f32 body_y = sin_r(dr_sqrtf(swing) * MC_PI * 2.0f) * 0.2f;
        f32 inv = 1.0f - swing;
        f32 out_quart = 1.0f - inv * inv * inv * inv; // Ease.outQuart
        f32 aa = sin_r(out_quart * MC_PI);
        f32 bb = sin_r(swing * MC_PI) * -(pp[PART_HEAD].xRot - 0.7f) * 0.75f;

        pp[PART_BODY].yRot = body_y;
        pp[PART_RARM].z = sin_r(body_y) * 5.0f;
        pp[PART_RARM].x = -cos_r(body_y) * 5.0f;
        pp[PART_LARM].z = -sin_r(body_y) * 5.0f;
        pp[PART_LARM].x = cos_r(body_y) * 5.0f;
        pp[PART_RARM].yRot += body_y;
        pp[PART_LARM].yRot += body_y;
        pp[PART_LARM].xRot += body_y;
        pp[PART_RARM].xRot -= aa * 1.2f + bb;
        pp[PART_RARM].yRot += body_y * 2.0f;
        pp[PART_RARM].zRot += sin_r(swing * MC_PI) * -0.4f;
    }

    // ArmPose.BLOCK (using a shield): xRot * 0.5 - 0.9424779, yRot -+pi/6
    if (shield_using()) {
        int part = shield_using_offhand() ? PART_LARM : PART_RARM;
        pp[part].xRot = pp[part].xRot * 0.5f - 0.9424779f;
        pp[part].yRot = (part == PART_RARM ? -1.0f : 1.0f) * (MC_PI / 6.0f);
    }

    // poseRightArm / poseLeftArm, ArmPose.BOW_AND_ARROW: both arms aim along the head
    if (gSurv.use_remaining > 0 && surv_held_item() == ITEM_BOW) {
        pp[PART_RARM].yRot = -0.1f + pp[PART_HEAD].yRot;
        pp[PART_LARM].yRot = 0.1f + pp[PART_HEAD].yRot + 0.4f;
        pp[PART_RARM].xRot = -(MC_PI / 2.0f) + pp[PART_HEAD].xRot;
        pp[PART_LARM].xRot = -(MC_PI / 2.0f) + pp[PART_HEAD].xRot;
    }

    if (s->sneaking) {
        pp[PART_BODY].xRot = 0.5f;
        pp[PART_RARM].xRot += 0.4f;
        pp[PART_LARM].xRot += 0.4f;
        pp[PART_RLEG].z += 4.0f;
        pp[PART_LLEG].z += 4.0f;
        pp[PART_HEAD].y += 4.2f;
        pp[PART_BODY].y += 3.2f;
        pp[PART_LARM].y += 3.2f;
        pp[PART_RARM].y += 3.2f;
    }

    // HumanoidModel.setupSwimAnimation (26.3)
    {
        f32 amt = steve_lerp(s->o_swim_amount, s->swim_amount, pt);

        if (amt > 0.0f) {
            f32 swim_pos = pos - (f32)(s32)(pos / 26.0f) * 26.0f; // animationPos % 26
            f32 q14 = -65.0f * 14.0f + 14.0f * 14.0f;               // quadraticArmUpdate(14)
            // the swinging arm doesn't swim
            f32 ra = (swing > 0.0f) ? 0.0f : amt, la = amt;
            f32 lx, rx, ly, ry, lz, rz;

            pp[PART_HEAD].xRot = rot_lerp_rad(amt, pp[PART_HEAD].xRot, -MC_PI / 4.0f);
            if (swim_pos < 14.0f) {
                f32 q = -65.0f * swim_pos + swim_pos * swim_pos;
                lx = 0.0f; rx = 0.0f;
                ly = MC_PI; ry = MC_PI;
                lz = MC_PI + 1.8707964f * q / q14;
                rz = MC_PI - 1.8707964f * q / q14;
            } else if (swim_pos < 22.0f) {
                f32 t = (swim_pos - 14.0f) / 8.0f;
                lx = rx = MC_PI / 2.0f * t;
                ly = ry = MC_PI;
                lz = 5.012389f - 1.8707964f * t;
                rz = 1.2707963f + 1.8707964f * t;
            } else {
                f32 t = (swim_pos - 22.0f) / 4.0f;
                lx = rx = MC_PI / 2.0f - MC_PI / 2.0f * t;
                ly = ry = MC_PI;
                lz = rz = MC_PI;
            }
            pp[PART_LARM].xRot = rot_lerp_rad(la, pp[PART_LARM].xRot, lx);
            pp[PART_RARM].xRot = pp[PART_RARM].xRot + (rx - pp[PART_RARM].xRot) * ra;
            pp[PART_LARM].yRot = rot_lerp_rad(la, pp[PART_LARM].yRot, ly);
            pp[PART_RARM].yRot = pp[PART_RARM].yRot + (ry - pp[PART_RARM].yRot) * ra;
            pp[PART_LARM].zRot = rot_lerp_rad(la, pp[PART_LARM].zRot, lz);
            pp[PART_RARM].zRot = pp[PART_RARM].zRot + (rz - pp[PART_RARM].zRot) * ra;
            pp[PART_LLEG].xRot += (0.3f * cos_r(pos * 0.33333334f + MC_PI) - pp[PART_LLEG].xRot) * amt;
            pp[PART_RLEG].xRot += (0.3f * cos_r(pos * 0.33333334f) - pp[PART_RLEG].xRot) * amt;
        }
    }

    // AnimationUtils.bobModelPart: arms breathe a little
    {
        f32 age = (f32)s->tick_count + pt;
        pp[PART_RARM].zRot += cos_r(age * 0.09f) * 0.05f + 0.05f;
        pp[PART_RARM].xRot += sin_r(age * 0.067f) * 0.05f;
        pp[PART_LARM].zRot -= cos_r(age * 0.09f) * 0.05f + 0.05f;
        pp[PART_LARM].xRot -= sin_r(age * 0.067f) * 0.05f;
    }
}

// called from the player_draw hook, inside the 3d pass, before banjo draws.
static void steve_draw_body(Gfx **gfx, Mtx **mtx);
static void steve_draw_armor(Gfx **gfx, Mtx **mtx, const Pose *root, const PartPose pp[PART_COUNT]);

// spectators are invisible to themselves (no body in f5)
void steve_draw(Gfx **gfx, Mtx **mtx) {
    if (GM_SPECTATING()) return;
    steve_draw_body(gfx, mtx);
}

// ItemInHandLayer.submitArmWithItem: an item in a hand. the left hand uses
// the item's thirdperson_lefthand transform (ItemTransform.apply with
// leftHand: x translation and y / z rotation mirrored)
static void steve_draw_hand_item(Gfx **gfx, Mtx **mtx, const Pose *root, const PartPose pp[PART_COUNT], u16 held, int left) {
    Pose p = *root;
    
    part_apply(&p, &pp[left ? PART_LARM : PART_RARM]); // translateToHand
    pose_rotate(&p, 0, -90.0f);
    pose_rotate(&p, 1, 180.0f);
    pose_translate(&p, (left ? -1.0f : 1.0f) / 16.0f, 2.0f / 16.0f, -10.0f / 16.0f);
    if (held == ITEM_SHIELD) {
        // the shield's special model (item/shield or item/shield_blocking)
        int blocking = shield_using() && (shield_using_offhand() == left);
        shield_display(&p, left ? SHIELD_THIRD_LEFT : SHIELD_THIRD_RIGHT, blocking);
        world_set_texture_state(gfx);
        mlMtxIdent();
        mlMtxApply(*mtx);
        gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_STEVE_START + PART_COUNT + (left ? 1 : 0), G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        shield_emit(gfx, &p, FALSE, FALSE);
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
        world_restore_texture_state(gfx);
        return;
    }
    if (item_is_block(held)) {
        // block model "thirdperson_righthand": 0 2.5 0, rotation 75 45 0, scale 0.375
        pose_translate(&p, 0.0f, 2.5f / 16.0f, 0.0f);
        pose_rotate(&p, 0, 75.0f);
        pose_rotate(&p, 1, left ? -45.0f : 45.0f);
        pose_scale(&p, 0.375f, 0.375f, 0.375f);
    } else if (item_is_handheld(held)) {
        // item/handheld: translation 0 4 0.5, rotation 0 -90 55, scale 0.85
        pose_translate(&p, 0.0f, 4.0f / 16.0f, 0.5f / 16.0f);
        pose_rotate(&p, 1, left ? 90.0f : -90.0f);
        pose_rotate(&p, 2, left ? -55.0f : 55.0f);
        pose_scale(&p, 0.85f, 0.85f, 0.85f);
    } else {
        // item/generated: translation 0 3 1, scale 0.55
        pose_translate(&p, 0.0f, 3.0f / 16.0f, 1.0f / 16.0f);
        pose_scale(&p, 0.55f, 0.55f, 0.55f);
    }
    pose_translate(&p, -0.5f, -0.5f, -0.5f);

    world_set_texture_state(gfx);
    mlMtxIdent();
    mlMtxApply(*mtx);
    gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_STEVE_START + PART_COUNT + (left ? 1 : 0), G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    if (item_is_block(held)) {
        world_emit_block_cube(gfx, held, &p, tp_item_vtx[vtx_buffer][left], FALSE, NULL);
    } else {
        world_item_both_faces = TRUE; // (the world pass has a depth buffer)
        world_emit_item_sprite(gfx, steve_item_sprite(held), &p, tp_item_vtx[vtx_buffer][left], FALSE);
        world_item_both_faces = FALSE;
    }
    gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
    gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    world_restore_texture_state(gfx);
}

static Vtx overlay_vtx[VTX_BUFFERS][OVERLAY_COUNT + 1][24]; // + the first person sleeve
static u8 overlay_face_on[VTX_BUFFERS][OVERLAY_COUNT + 1][6];

static void steve_draw_body(Gfx **gfx, Mtx **mtx) {
    f32 cam[3], pt = steve_partial_tick();
    PartPose pp[PART_COUNT];
    Pose root;
    int b;

    skin_tick();
    vtx_buffer = (vtx_buffer + 1) % VTX_BUFFERS;
    steve_setup_anim(pp, pt);
    viewport_getPosition_vec3f(cam);

    // LivingEntityRenderer: position, rotY(180 - bodyRot), scale(-1,-1,1),
    // translate(0, -1.501, 0), then the player's 0.9375 scale. in blocks.
    pose_identity(&root);
    pose_scale(&root, MC_BLOCK, MC_BLOCK, MC_BLOCK);
    pose_translate(&root, (gSteve.render_pos[0] - cam[0]) / MC_BLOCK, (gSteve.render_pos[1] - cam[1]) / MC_BLOCK,
        (gSteve.render_pos[2] - cam[2]) / MC_BLOCK);
    pose_rotate(&root, 1, 180.0f - steve_lerp(gSteve.o_body_rot, gSteve.body_rot, pt)); // setupRotations
    // AvatarRenderer.setupRotations: swimming tips the body forward, along
    // the look pitch in water (minecraft xRot = -our pitch)
    {
        f32 amt = steve_lerp(gSteve.o_swim_amount, gSteve.swim_amount, pt);
        if (amt > 0.0f) {
            f32 target = gSteve.in_water ? -90.0f + gSteve.pitch : -90.0f;
            pose_rotate(&root, 0, amt * target);
            if (gSteve.swimming) {
                pose_translate(&root, 0.0f, -1.0f, 0.3f); // isVisuallySwimming
            }
        }
    }
    pose_scale(&root, -1.0f, -1.0f, 1.0f);
    pose_translate(&root, 0.0f, -1.501f, 0.0f);
    pose_scale(&root, 0.9375f, 0.9375f, 0.9375f);

    // vertices are camera relative, so the modelview is identity. each part is
    // its own tagged group with vertex interpolation, so rt64 can smooth the
    // cpu-built vertices between game frames (see mc_gbi.h)
    steve_set_render_state(gfx, TRUE);
    steve_red_overlay(gfx, TRUE);
    for (b = 0; b < PART_COUNT; b++) {
        Pose p = root;

        part_apply(&p, &pp[b]);
        build_part(&p, b, part_vtx[vtx_buffer][b], part_face_on[vtx_buffer][b]);
        mlMtxIdent();
        mlMtxApply(*mtx);
        gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_STEVE_START + b, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        emit_part(gfx, b, part_vtx[vtx_buffer][b], part_face_on[vtx_buffer][b]);
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    }
    // the outer layer (PlayerModel hat, jacket, sleeves, pants)
    overlay_state(gfx, TRUE);
    for (b = 0; b < OVERLAY_COUNT; b++) {
        Pose p = root;
        part_apply(&p, &pp[overlays[b].part]);
        build_overlay(&p, b, overlay_vtx[vtx_buffer][b], overlay_face_on[vtx_buffer][b]);
        mlMtxIdent();
        mlMtxApply(*mtx);
        gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_STEVE_START + 0x10 + b, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        emit_overlay(gfx, b, overlay_vtx[vtx_buffer][b], overlay_face_on[vtx_buffer][b]);
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    }
    gDPPipeSync((*gfx)++);
    gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
    steve_draw_armor(gfx, mtx, &root, pp); // HumanoidArmorLayer
    steve_restore_render_state(gfx);

    // ItemInHandLayer: the main hand's item in the right hand, the
    // offhand's in the left
    if (surv_held_item() != 0) steve_draw_hand_item(gfx, mtx, &root, pp, surv_held_item(), FALSE);
    if (gSurv.inv[INV_OFFHAND].count > 0) steve_draw_hand_item(gfx, mtx, &root, pp, gSurv.inv[INV_OFFHAND].id, TRUE);
}

// --- armor (HumanoidArmorLayer + EquipmentLayerRenderer) ---
// HumanoidModel.createArmorMeshSet(INNER 0.5, OUTER 1.0): the humanoid boxes
// inflated, legs 0.1 less (createBaseArmorMesh), left arm / leg mirrored.
// textures: entity/equipment/humanoid (head, chest, feet) and
// humanoid_leggings (legs), 64 x 32. leather is dyed (DyedItemColor default
// -6265536) with its undyed overlay drawn on top.

typedef struct {
    u8 part;      // which pose it follows
    f32 bmin[3], bsize[3];
    u8 u, v;
    u8 mirror;
    f32 extra;    // on top of the slot's deformation (hat +0.5, legs -0.1)
} ArmorBox;

static const ArmorBox armor_boxes[] = {
    { PART_HEAD, { -4, -8, -4 }, { 8, 8, 8 }, 0, 0, 0, 0.0f },    // 0 head
    { PART_HEAD, { -4, -8, -4 }, { 8, 8, 8 }, 32, 0, 0, 0.5f },   // 1 hat
    { PART_BODY, { -4, 0, -2 }, { 8, 12, 4 }, 16, 16, 0, 0.0f },  // 2 body
    { PART_RARM, { -3, -2, -2 }, { 4, 12, 4 }, 40, 16, 0, 0.0f }, // 3 right arm
    { PART_LARM, { -1, -2, -2 }, { 4, 12, 4 }, 40, 16, 1, 0.0f }, // 4 left arm
    { PART_RLEG, { -2, 0, -2 }, { 4, 12, 4 }, 0, 16, 0, -0.1f },  // 5 right leg
    { PART_LLEG, { -2, 0, -2 }, { 4, 12, 4 }, 0, 16, 1, -0.1f },  // 6 left leg
};
// ADULT_ARMOR_PARTS_PER_SLOT, in the layer's order (chest, legs, feet, head)
static const u8 armor_order[4] = { 1, 2, 3, 0 };
static const u8 armor_parts[4][4] = { { 0, 1, 255, 255 }, { 2, 3, 4, 255 }, { 2, 5, 6, 255 }, { 5, 6, 255, 255 } };

#define ARMOR_MAX_BOXES 20
static Vtx armor_vtx[VTX_BUFFERS][ARMOR_MAX_BOXES][24];
static u8 armor_face_on[VTX_BUFFERS][ARMOR_MAX_BOXES][6];

static void emit_armor_box(Gfx **gfx, const u32 *tex, const ArmorBox *ab, Vtx *v, const u8 *face_on) {
    s32 uls = ab->u, ult = ab->v;
    s32 lrs = uls + (s32)(2 * ab->bsize[2] + 2 * ab->bsize[0]) - 1;
    s32 lrt = ult + (s32)(ab->bsize[2] + ab->bsize[1]) - 1;
    int f;

    gDPLoadTextureTile((*gfx)++, tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 64, 32,
        uls, ult, lrs, lrt, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
        G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gSPVertex((*gfx)++, v, 24, 0);
    for (f = 0; f < 6; f++) {
        s32 k = f * 4;
        if (face_on[f]) gSP2Triangles((*gfx)++, k, k + 1, k + 2, 0, k, k + 2, k + 3, 0);
    }
}

static void steve_draw_armor(Gfx **gfx, Mtx **mtx, const Pose *root, const PartPose pp[PART_COUNT]) {
#ifdef MC_HAVE_TEXTURES
    static const u8 white[3] = { 0xFF, 0xFF, 0xFF };
    static const u8 leather[3] = { 0xA0, 0x65, 0x40 };
    int n = 0, o, k, layer_pass;

    gDPPipeSync((*gfx)++);
    gDPSetCycleType((*gfx)++, G_CYC_1CYCLE);
    gDPSetAlphaCompare((*gfx)++, G_AC_THRESHOLD);
    gDPSetBlendColor((*gfx)++, 0, 0, 0, 0x80);
    gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2);
    gDPSetCombineMode((*gfx)++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
    for (o = 0; o < 4; o++) {
        int slot = armor_order[o];
        const ItemStack *st = &gSurv.inv[INV_ARMOR(slot)];
        const McArmor *am;
        int layer = (slot == 2) ? 1 : 0;
        f32 g = (slot == 2) ? 0.5f : 1.0f;

        if (st->count == 0 || st->id < ITEM_FIRST || st->id >= ITEM_END) continue;
        am = &mc_item_armor[st->id - ITEM_FIRST];
        if (am->slot != slot) continue;
        // leather: the dyed layer, then the overlay
        for (layer_pass = 0; layer_pass < (am->asset == 0 ? 2 : 1); layer_pass++) {
            const u32 *tex = (layer_pass == 0) ? &mc_armor_tex[(am->asset * 2 + layer) * 64 * 32]
                                               : &mc_armor_tex[(MC_ARMOR_ASSETS * 2 + layer) * 64 * 32];
            const u8 *col = (am->asset == 0 && layer_pass == 0) ? leather : white;
            for (k = 0; k < 4 && armor_parts[slot][k] != 255; k++) {
                const ArmorBox *ab = &armor_boxes[armor_parts[slot][k]];
                Pose p = *root;
                if (n >= ARMOR_MAX_BOXES) break;
                part_apply(&p, &pp[ab->part]);
                build_box(&p, ab->bmin, ab->bsize, g + ab->extra + layer_pass * 0.01f, ab->u, ab->v, ab->mirror, col,
                    armor_vtx[vtx_buffer][n], armor_face_on[vtx_buffer][n]);
                mlMtxIdent();
                mlMtxApply(*mtx);
                gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_ARMOR_START + n, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
                gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                emit_armor_box(gfx, tex, ab, armor_vtx[vtx_buffer][n], armor_face_on[vtx_buffer][n]);
                gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
                gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
                n++;
            }
        }
    }
    gDPPipeSync((*gfx)++);
    gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
#endif
}

// --- inventory preview ---

static Vtx gui_part_vtx[VTX_BUFFERS][PART_COUNT][24];
static u8 gui_part_face_on[VTX_BUFFERS][PART_COUNT][6];
// the armor on the preview (same boxes as HumanoidArmorLayer in the world)
static Vtx gui_armor_vtx[VTX_BUFFERS][ARMOR_MAX_BOXES][24];
static Vtx gui_ov_vtx[VTX_BUFFERS][OVERLAY_COUNT][24];
static u8 gui_ov_face_on[VTX_BUFFERS][OVERLAY_COUNT][6];
static u8 gui_armor_face_on[VTX_BUFFERS][ARMOR_MAX_BOXES][6];
static const u32 *gui_armor_tex[ARMOR_MAX_BOXES];
static const ArmorBox *gui_armor_box[ARMOR_MAX_BOXES];

// build the worn armor's boxes for the gui camera; depth just in front of
// the part each one is on (drawn right after it)
static int build_gui_armor(const Pose *root, const PartPose pp[PART_COUNT], const f32 part_depth[PART_COUNT], f32 *depth, int buf) {
    int n = 0;
#ifdef MC_HAVE_TEXTURES
    static const u8 white[3] = { 0xFF, 0xFF, 0xFF };
    static const u8 leather[3] = { 0xA0, 0x65, 0x40 };
    int o, k, layer_pass;

    for (o = 0; o < 4; o++) {
        int slot = armor_order[o];
        const ItemStack *st = &gSurv.inv[INV_ARMOR(slot)];
        const McArmor *am;
        int layer = (slot == 2) ? 1 : 0;
        f32 g = (slot == 2) ? 0.5f : 1.0f;

        if (st->count == 0 || st->id < ITEM_FIRST || st->id >= ITEM_END) continue;
        am = &mc_item_armor[st->id - ITEM_FIRST];
        if (am->slot != slot) continue;
        for (layer_pass = 0; layer_pass < (am->asset == 0 ? 2 : 1); layer_pass++) {
            const u32 *tex = (layer_pass == 0) ? &mc_armor_tex[(am->asset * 2 + layer) * 64 * 32]
                                               : &mc_armor_tex[(MC_ARMOR_ASSETS * 2 + layer) * 64 * 32];
            const u8 *col = (am->asset == 0 && layer_pass == 0) ? leather : white;
            for (k = 0; k < 4 && armor_parts[slot][k] != 255; k++) {
                const ArmorBox *ab = &armor_boxes[armor_parts[slot][k]];
                Pose p = *root;
                if (n >= ARMOR_MAX_BOXES) break;
                part_apply(&p, &pp[ab->part]);
                build_box(&p, ab->bmin, ab->bsize, g + ab->extra + layer_pass * 0.01f, ab->u, ab->v, ab->mirror, col,
                    gui_armor_vtx[buf][n], gui_armor_face_on[buf][n]);
                gui_armor_tex[n] = tex;
                gui_armor_box[n] = ab;
                depth[n] = part_depth[ab->part] + 0.0001f * (n + 1);
                n++;
            }
        }
    }
#endif
    return n;
}

// InventoryScreen.extractEntityInInventoryFollowsMouse + GuiEntityRenderer:
// steve in the box (x0,y0)-(x1,y1) in gui pixels, `size` gui pixels per
// block, turning toward the mouse. drawn through the gui camera (gui.c),
// whose vertices are quarter gui pixels.
void steve_draw_gui(Gfx **gfx, f32 x0, f32 y0, f32 x1, f32 y1, f32 size, f32 mouse_x, f32 mouse_y) {
    f32 cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    // radians, and minecraft then uses angle * 20 as degrees
    f32 x_angle = atan_approx((cx - mouse_x) / 40.0f);
    f32 y_angle = atan_approx((cy - mouse_y) / 40.0f);
    PartPose pp[PART_COUNT];
    Pose root;
    f32 depth[PART_COUNT + ARMOR_MAX_BOXES + OVERLAY_COUNT];
    int order[PART_COUNT + ARMOR_MAX_BOXES + OVERLAY_COUNT], b, i, j, total, n_armor, armor_state = FALSE;
    int buf = vtx_buffer;
#define GUI_OV (PART_COUNT + ARMOR_MAX_BOXES)

    skin_tick();

    steve_setup_anim(pp, steve_partial_tick());
    // render state overrides: head follows the mouse, no look pitch/yaw from the game
    pp[PART_HEAD].xRot = -y_angle * 20.0f / RAD_TO_DEG;
    pp[PART_HEAD].yRot = x_angle * 20.0f / RAD_TO_DEG;

    pose_identity(&root);
    pose_scale(&root, 4.0f, 4.0f, 4.0f); // quarter gui pixels
    // PictureInPictureRenderer: center of the box, scale(size, size, -size)
    pose_translate(&root, cx, cy, 0.0f);
    pose_scale(&root, size, size, -size);
    // translation (0, height / 2 + 0.0625, 0), rotation rotZ(180) * rotX(yAngle * 20)
    pose_translate(&root, 0.0f, 0.9f + 0.0625f, 0.0f);
    pose_rotate(&root, 2, 180.0f);
    pose_rotate(&root, 0, y_angle * 20.0f);
    // LivingEntityRenderer with bodyRot = 180 + xAngle * 20
    pose_rotate(&root, 1, 180.0f - (180.0f + x_angle * 20.0f));
    pose_scale(&root, -1.0f, -1.0f, 1.0f);
    pose_translate(&root, 0.0f, -1.501f, 0.0f);
    pose_scale(&root, 0.9375f, 0.9375f, 0.9375f);

    build_ortho = TRUE;
    for (b = 0; b < PART_COUNT; b++) {
        Pose p = root;
        f32 c[3], zero[3] = { 0.0f, 0.0f, 0.0f };

        part_apply(&p, &pp[b]);
        build_part(&p, b, gui_part_vtx[buf][b], gui_part_face_on[buf][b]);
        pose_apply(&p, zero, c);
        depth[b] = c[2];
        order[b] = b;
    }
    n_armor = build_gui_armor(&root, pp, depth, depth + PART_COUNT, buf);
    total = PART_COUNT + n_armor;
    for (i = PART_COUNT; i < total; i++) order[i] = i;
    // the outer layer: right after its part, before the armor on it
    for (i = 0; i < OVERLAY_COUNT; i++) {
        Pose p = root;
        part_apply(&p, &pp[overlays[i].part]);
        build_overlay(&p, i, gui_ov_vtx[buf][i], gui_ov_face_on[buf][i]);
        depth[GUI_OV + i] = depth[overlays[i].part] + 0.00005f;
        order[total++] = GUI_OV + i;
    }
    build_ortho = FALSE;
    // no depth buffer in the gui: draw the parts (and their armor) farthest first
    for (i = 0; i < total; i++) {
        for (j = i + 1; j < total; j++) {
            if (depth[order[j]] < depth[order[i]]) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
    steve_set_render_state(gfx, FALSE);
    steve_red_overlay(gfx, FALSE);
    for (i = 0; i < total; i++) {
        b = order[i];
        if (b >= GUI_OV) {
            overlay_state(gfx, FALSE);
            emit_overlay(gfx, b - GUI_OV, gui_ov_vtx[buf][b - GUI_OV], gui_ov_face_on[buf][b - GUI_OV]);
            armor_state = TRUE; // (the alpha test is on: back to the skin state next)
            continue;
        }
        if (b >= PART_COUNT) {
#ifdef MC_HAVE_TEXTURES
            if (!armor_state) {
                // armor: cut out by alpha (like the world's armor pass)
                gDPPipeSync((*gfx)++);
                gDPSetAlphaCompare((*gfx)++, G_AC_THRESHOLD);
                gDPSetBlendColor((*gfx)++, 0, 0, 0, 0x80);
                gDPSetRenderMode((*gfx)++, G_RM_AA_TEX_EDGE, G_RM_AA_TEX_EDGE2);
                gDPSetCombineMode((*gfx)++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
                armor_state = TRUE;
            }
            emit_armor_box(gfx, gui_armor_tex[b - PART_COUNT], gui_armor_box[b - PART_COUNT],
                gui_armor_vtx[buf][b - PART_COUNT], gui_armor_face_on[buf][b - PART_COUNT]);
#endif
            continue;
        }
        if (armor_state) {
            steve_set_render_state(gfx, FALSE);
            steve_red_overlay(gfx, FALSE);
            armor_state = FALSE;
        }
        emit_part(gfx, b, gui_part_vtx[buf][b], gui_part_face_on[buf][b]);
    }
    if (armor_state) {
        gDPPipeSync((*gfx)++);
        gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
    }
}

// --- first person hand ---

static Vtx held_vtx[VTX_BUFFERS][24];

// the hand's own camera: a fixed 70 degree projection with no view rotation,
// tagged as its own projection (not interpolated, it never moves) and with an
// identity view matrix, so rt64 doesn't try to place it in the world. the
// modelview group interpolates the hand's vertices (swing, bob) between frames.
// callers pop the modelview group when done.
static f32 identity_view[4][4] __attribute__((aligned(8))) = {
    { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 },
};

static u32 hand_group_id = MC_ID_HAND; // (the offhand's item is its own group)
static Vtx held_vtx_off[VTX_BUFFERS][24];

static void hand_projection(Gfx **gfx, Mtx **mtx) {
    u16 norm;

    guPerspective(*mtx, &norm, MC_FOV, sViewportAspect, 4.0f, 1000.0f, 1.0f);
    gSPPerspNormalize((*gfx)++, norm);
    gEXMatrixGroupSkipAll((*gfx)++, MC_ID_HAND_PROJECTION, G_EX_NOPUSH, G_MTX_PROJECTION, G_EX_EDIT_NONE);
    gSPMatrix((*gfx)++, OS_PHYSICAL_TO_K0((*mtx)++), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gEXSetViewMatrixFloat((*gfx)++, identity_view);
    guMtxIdent(*mtx);
    gEXMatrixGroupSimpleVerts((*gfx)++, hand_group_id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    gSPMatrix((*gfx)++, OS_PHYSICAL_TO_K0((*mtx)++), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
}

// submitArmWithItem (not using the item): applyItemArmTransform, swingArm
// (WHACK), then the block model's firstperson_righthand display transform
// (rotation 0 45 0, scale 0.4) and the item renderer's -0.5 center
// left: the offhand (HumanoidArm.LEFT: invert -1, the item's
// firstperson_lefthand transform, no attack swing)
static void steve_draw_held_block(Gfx **gfx, Mtx **mtx, Pose *pose, f32 attack, u16 block, f32 inv_height, int left) {
    f32 sq = dr_sqrtf(attack), inv = left ? -1.0f : 1.0f;
    int using_this = left ? gSurv.use_slot == INV_OFFHAND : gSurv.use_slot != INV_OFFHAND;
    int drawing_bow = !left && gSurv.use_remaining > 0 && block == ITEM_BOW;
    int eating = using_this && gSurv.use_remaining > 0 && gSurv.use_duration > 0 && !drawing_bow && block != ITEM_SHIELD;

    if (eating) {
        // ItemInHandRenderer.applyEatTransform (right arm)
        f32 use = (f32)gSurv.use_remaining - steve_partial_tick() + 1.0f;
        f32 percent = use / (f32)gSurv.use_duration;
        f32 p27 = percent, k;
        int i;

        if (percent < 0.8f) {
            f32 c = cos_r(use / 4.0f * MC_PI) * 0.1f;
            pose_translate(pose, 0.0f, c < 0.0f ? -c : c, 0.0f);
        }
        for (i = 1; i < 27; i++) p27 *= percent; // percent^27
        k = 1.0f - p27;
        pose_translate(pose, inv * k * 0.6f, k * -0.5f, 0.0f);
        pose_rotate(pose, 1, inv * k * 90.0f);
        pose_rotate(pose, 0, k * 10.0f);
        pose_rotate(pose, 2, inv * k * 30.0f);
    }
    pose_translate(pose, inv * 0.56f, -0.52f + inv_height * -0.6f, -0.72f); // applyItemArmTransform
    if (drawing_bow) {
        // FirstPersonHandsAndItemsRenderer, case BOW (right arm, invert 1)
        f32 held = (f32)gSurv.use_duration - ((f32)gSurv.use_remaining - steve_partial_tick() + 1.0f);
        f32 power = held / 20.0f;
        power = (power * power + power * 2.0f) / 3.0f;
        if (power > 1.0f) power = 1.0f;
        pose_translate(pose, -0.2785682f, 0.18344387f, 0.15731531f);
        pose_rotate(pose, 0, -13.935f);
        pose_rotate(pose, 1, 35.3f);
        pose_rotate(pose, 2, -9.785f);
        if (power > 0.1f) {
            f32 shake = sin_r((held - 0.1f) * 1.3f) * (power - 0.1f);
            pose_translate(pose, 0.0f, shake * 0.004f, 0.0f);
        }
        pose_translate(pose, 0.0f, 0.0f, power * 0.04f);
        pose_scale(pose, 1.0f, 1.0f, 1.0f + power * 0.2f);
        pose_rotate(pose, 1, -45.0f);
    } else if (!left && !eating && !(block == ITEM_SHIELD && using_this && shield_using()) && (gSteve.swinging || attack > 0.0f)) {
        pose_translate(pose, -0.4f * sin_r(sq * MC_PI), 0.2f * sin_r(sq * MC_PI * 2.0f), -0.2f * sin_r(attack * MC_PI));
        // applyItemArmAttackTransform
        pose_rotate(pose, 1, 45.0f + sin_r(attack * attack * MC_PI) * -20.0f);
        pose_rotate(pose, 2, sin_r(sq * MC_PI) * -20.0f);
        pose_rotate(pose, 0, sin_r(sq * MC_PI) * -80.0f);
        pose_rotate(pose, 1, -45.0f);
    }
    if (block == ITEM_SHIELD) {
        // the shield's special model, item/shield(_blocking) firstperson
        // transform. seen from behind: the handle goes over the plate
        shield_display(pose, left ? SHIELD_FIRST_LEFT : SHIELD_FIRST_RIGHT, using_this && shield_using());
        hand_projection(gfx, mtx);
        world_set_texture_state(gfx);
        gSPClearGeometryMode((*gfx)++, G_ZBUFFER);
        gDPSetRenderMode((*gfx)++, G_RM_AA_TEX_EDGE, G_RM_AA_TEX_EDGE2);
        shield_emit(gfx, pose, FALSE, TRUE);
        world_restore_texture_state(gfx);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
        return;
    }
    if (item_is_block(block)) {
        // block model firstperson_righthand: rotation 0 45 0, scale 0.4
        // (firstperson_lefthand: 0 225 0, its y mirrored by apply(leftHand))
        pose_rotate(pose, 1, left ? -225.0f : 45.0f);
        pose_scale(pose, 0.4f, 0.4f, 0.4f);
    } else {
        // item/handheld and item/generated firstperson_righthand:
        // translation 1.13 3.2 1.13, rotation 0 -90 25, scale 0.68
        pose_translate(pose, inv * 1.13f / 16.0f, 3.2f / 16.0f, 1.13f / 16.0f);
        pose_rotate(pose, 1, inv * -90.0f);
        pose_rotate(pose, 2, inv * 25.0f);
        pose_scale(pose, 0.68f, 0.68f, 0.68f);
    }
    pose_translate(pose, -0.5f, -0.5f, -0.5f);

    hand_projection(gfx, mtx);
    world_set_texture_state(gfx);
    gSPClearGeometryMode((*gfx)++, G_ZBUFFER);
    gDPSetRenderMode((*gfx)++, G_RM_AA_TEX_EDGE, G_RM_AA_TEX_EDGE2);
    if (item_is_block(block)) {
        world_emit_block_cube(gfx, block, pose, left ? held_vtx_off[vtx_buffer] : held_vtx[vtx_buffer], FALSE, NULL);
    } else {
        world_emit_item_sprite(gfx, steve_item_sprite(block), pose, held_vtx[vtx_buffer], FALSE);
    }
    world_restore_texture_state(gfx);
    gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
}

// FirstPersonHandsAndItemsRenderer.submitHandsWithItems + renderPlayerArm
// (right arm, empty hand), then AvatarRenderer.renderHand (arm zRot 0.1).
// called at the end of the 3d scene.
static void steve_draw_hand_impl(Gfx **gfx, Mtx **mtx);

// spectators have no hand (GameRenderer: !isSpectator)
void steve_draw_hand(Gfx **gfx, Mtx **mtx) {
    if (GM_SPECTATING()) return;
    steve_draw_hand_impl(gfx, mtx);
}

static void steve_draw_hand_impl(Gfx **gfx, Mtx **mtx) {
    Steve *s = (skin_tick(), &gSteve);
    f32 pt = steve_partial_tick();
    f32 attack = steve_attack_anim(pt);
    f32 sq = dr_sqrtf(attack);
    f32 x_bob = steve_lerp(s->o_x_bob, s->x_bob, pt);
    f32 y_bob = s->o_y_bob + (s->y_bob - s->o_y_bob) * pt;
    f32 view_x = -s->pitch, view_y = -s->yaw, dy;
    // mainHandSwapScale * (1 - lerp(oldMainHandHeight, mainHandHeight))
    f32 inv_height = 1.0f - steve_lerp(s->o_hand_height, s->hand_height, pt);
    PartPose arm;
    Pose pose;
    Vtx *v;
    u8 *face_on;

    // the hand gets its own camera, like minecraft (fixed 70 degree projection,
    // no view rotation), so the renderer's frame interpolation can't make it
    // drift against the world. view space: x right, y up, -z ahead, in blocks.
    pose_identity(&pose);
    pose_scale(&pose, MC_BLOCK, MC_BLOCK, MC_BLOCK);

    // GameRenderer.renderItemInHand applies bobView to the hand too
    {
        f32 wd = -(s->walk_dist + (s->walk_dist - s->o_walk_dist) * pt);
        f32 bob = steve_lerp(s->o_bob, s->bob, pt);
        f32 sw = ml_sin_deg(wd * 180.0f), cw = ml_cos_deg(wd * 180.0f);
        f32 cw2 = ml_cos_deg(wd * 180.0f - 11.4591559f);

        pose_translate(&pose, sw * bob * 0.5f, -(cw < 0.0f ? -cw : cw) * bob, 0.0f);
        pose_rotate(&pose, 2, sw * bob * 3.0f);
        pose_rotate(&pose, 0, (cw2 < 0.0f ? -cw2 : cw2) * bob * 5.0f);
    }

    // arm sway: lags behind where you look
    dy = view_y - y_bob;
    while (dy > 180.0f) dy -= 360.0f;
    while (dy < -180.0f) dy += 360.0f;
    pose_rotate(&pose, 0, (view_x - x_bob) * 0.1f);
    pose_rotate(&pose, 1, dy * 0.1f);

    // holding something: submitArmWithItem's normal branch draws the item, not
    // the arm. hand_item lags the hotbar for the equip animation.
    // the offhand's item on the left (submitHandsWithItems: OFF_HAND)
    if (gSurv.inv[INV_OFFHAND].count > 0) {
        Pose off = pose;
        hand_group_id = MC_ID_HAND + 2;
        steve_draw_held_block(gfx, mtx, &off, 0.0f, gSurv.inv[INV_OFFHAND].id, 0.0f, TRUE);
        hand_group_id = MC_ID_HAND;
    }
    if (s->hand_item != 0) {
        steve_draw_held_block(gfx, mtx, &pose, attack, s->hand_item, inv_height, FALSE);
        return;
    }

    // renderPlayerArm, invert = 1 (right arm), inverseArmHeight = 0
    pose_translate(&pose,
        -0.3f * sin_r(sq * MC_PI) + 0.64000005f,
        0.4f * sin_r(sq * MC_PI * 2.0f) - 0.6f + inv_height * -0.6f,
        -0.4f * sin_r(attack * MC_PI) - 0.71999997f);
    pose_rotate(&pose, 1, 45.0f);
    pose_rotate(&pose, 1, sin_r(sq * MC_PI) * 70.0f);
    pose_rotate(&pose, 2, sin_r(attack * attack * MC_PI) * -20.0f);
    pose_translate(&pose, -1.0f, 3.6f, 3.5f);
    pose_rotate(&pose, 2, 120.0f);
    pose_rotate(&pose, 0, 200.0f);
    pose_rotate(&pose, 1, -135.0f);
    pose_translate(&pose, 5.6f, 0.0f, 0.0f);

    // AvatarRenderer.renderHand: arm.resetPose(), rightArm.zRot = 0.1
    part_reset(&arm, PART_RARM);
    arm.zRot = 0.1f;
    part_apply(&pose, &arm);

    v = part_vtx[vtx_buffer][PART_COUNT];
    face_on = part_face_on[vtx_buffer][PART_COUNT];
    build_part(&pose, PART_RARM, v, face_on);

    hand_projection(gfx, mtx);
    // no depth buffer (minecraft clears depth before the hand). the faces
    // left after culling are the near side of a convex box, so they never
    // overlap each other.
    steve_set_render_state(gfx, FALSE);
    emit_part(gfx, PART_RARM, v, face_on);
    // AvatarRenderer.renderHand: the sleeve after the arm, same pose
    build_overlay(&pose, 2, overlay_vtx[vtx_buffer][OVERLAY_COUNT], overlay_face_on[vtx_buffer][OVERLAY_COUNT]);
    overlay_state(gfx, FALSE);
    emit_overlay(gfx, 2, overlay_vtx[vtx_buffer][OVERLAY_COUNT], overlay_face_on[vtx_buffer][OVERLAY_COUNT]);
    gDPPipeSync((*gfx)++);
    gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
    steve_restore_render_state(gfx);
    gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
}

// ScreenEffectRenderer.submitFire: two fire_1 quads at the sides of the view
// (translate +-0.24, -0.3, rotY -+10 degrees, the quad -0.5..0.5 at z -0.5),
// colour 0xE5FFFFFF, drawn with the hand's camera
static Vtx fire_vtx[3][8];
static int fire_buf;

void steve_draw_fire_overlay(Gfx **gfx, Mtx **mtx) {
#ifdef MC_HAVE_TEXTURES
    int k, side;
    Vtx *v;

    if (!steve_on_fire() || !gSteve.first_person || GM_SPECTATING()) return;
    fire_buf = (fire_buf + 1) % 3;
    v = fire_vtx[fire_buf];
    for (side = 0; side < 2; side++) {
        Pose p;
        static const f32 c[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
        static const u8 uv[4][2] = { { 16, 16 }, { 0, 16 }, { 0, 0 }, { 16, 0 } };
        pose_identity(&p);
        pose_scale(&p, MC_BLOCK, MC_BLOCK, MC_BLOCK);
        pose_translate(&p, side == 0 ? 0.24f : -0.24f, -0.3f, 0.0f);
        pose_rotate(&p, 1, side == 0 ? -10.0f : 10.0f);
        for (k = 0; k < 4; k++) {
            f32 l[3] = { c[k][0], c[k][1], -0.5f }, o[3];
            Vtx *q = &v[side * 4 + k];
            pose_apply(&p, l, o);
            q->v.ob[0] = (s16)o[0];
            q->v.ob[1] = (s16)o[1];
            q->v.ob[2] = (s16)o[2];
            q->v.flag = 0;
            // buildSpriteQuad: u mirrored on the second quad like minecraft's
            q->v.tc[0] = (s16)((side == 0 ? uv[k][0] : 16 - uv[k][0]) << 5);
            q->v.tc[1] = (s16)(uv[k][1] << 5);
            q->v.cn[0] = q->v.cn[1] = q->v.cn[2] = 0xFF;
            q->v.cn[3] = 0xE5;
        }
    }
    hand_projection(gfx, mtx);
    gDPPipeSync((*gfx)++);
    gDPSetCycleType((*gfx)++, G_CYC_1CYCLE);
    gSPClearGeometryMode((*gfx)++, G_LIGHTING | G_CULL_BOTH | G_FOG | G_ZBUFFER);
    gSPSetGeometryMode((*gfx)++, G_SHADE | G_SHADING_SMOOTH);
    gSPTexture((*gfx)++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gDPSetTextureFilter((*gfx)++, G_TF_POINT);
    gDPSetRenderMode((*gfx)++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
    gDPSetCombineMode((*gfx)++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
    gDPLoadTextureBlock((*gfx)++, mc_fire_1_tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 16, 0,
        G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gSPVertex((*gfx)++, v, 8, 0);
    gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
    gSP2Triangles((*gfx)++, 4, 5, 6, 0, 4, 6, 7, 0);
    steve_restore_render_state(gfx);
    gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
#endif
}
