// chests, ported from minecraft 26.3:
//   - ChestBlock: placed facing getHorizontalDirection().getOpposite(),
//     useWithoutItem opens the menu unless isChestBlockedAt (a redstone
//     conductor on top), the contents drop when it's broken
//   - ChestBlockEntity / ChestLidController: openness +-0.1 a tick toward
//     open while someone has it open
//   - ChestRenderer + ChestModel (createSingleBodyLayer): bottom, lid, lock,
//     turned by -facing.toYRot() around the block middle, the lid's xRot =
//     -(1 - (1 - openness)^3) * 90 degrees. texture entity/chest/normal.png
// single chests only (no double chests yet). contents live per map and are
// saved with bk's save into mc_steve_<20 + slot>.bin, by registry name.

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "pose.h"
#include "mc_gbi.h"
#include "chest.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

#define MAX_CHESTS 256
static Chest chests[MAX_CHESTS];

Chest *chest_at(const s32 pos[3], int create) {
    s16 map = (s16)map_get();
    int i, free = -1;
    for (i = 0; i < MAX_CHESTS; i++) {
        Chest *c = &chests[i];
        if (c->used && c->map == map && c->pos[0] == pos[0] && c->pos[1] == pos[1] && c->pos[2] == pos[2]) return c;
        if (!c->used && free < 0) free = i;
    }
    if (!create || free < 0) return NULL;
    {
        Chest *c = &chests[free];
        int k;
        c->used = TRUE;
        c->map = map;
        for (k = 0; k < 3; k++) c->pos[k] = pos[k];
        for (k = 0; k < CHEST_SLOTS; k++) {
            c->items[k].id = 0;
            c->items[k].count = 0;
            c->items[k].damage = 0;
        }
        c->openness = c->o_openness = 0.0f;
        c->viewers = 0;
        return c;
    }
}

void chest_removed(const s32 pos[3]) {
    Chest *c = chest_at(pos, FALSE);
    if (c != NULL) c->used = FALSE;
}

void chests_reset(void) {
    int i;
    for (i = 0; i < MAX_CHESTS; i++) chests[i].used = FALSE;
}

// ChestBlock.isChestBlockedAt: BlockState.isRedstoneConductor of the block above
int chest_blocked(const s32 pos[3]) {
    BlockId b = world_get(pos[0], pos[1] + 1, pos[2]);
    return b != BLOCK_AIR && b != BLOCK_REDSTONE_BLOCK && (mc_block(b)->flags & MCB_OPAQUE) != 0;
}

// ChestLidController.tickLid
void chests_tick(void) {
    s16 map = (s16)map_get();
    int i;
    for (i = 0; i < MAX_CHESTS; i++) {
        Chest *c = &chests[i];
        if (!c->used || c->map != map) continue;
        // (a chest is only forgotten when its block is broken / blown up:
        // while a map loads its blocks may not be back yet)
        if (world_get(c->pos[0], c->pos[1], c->pos[2]) != BLOCK_CHEST) continue;
        c->o_openness = c->openness;
        if (c->viewers <= 0 && c->openness > 0.0f) {
            c->openness -= 0.1f;
            if (c->openness < 0.0f) c->openness = 0.0f;
        } else if (c->viewers > 0 && c->openness < 1.0f) {
            c->openness += 0.1f;
            if (c->openness > 1.0f) c->openness = 1.0f;
        }
    }
}

// --- drawing (ChestRenderer) ---

#define CHEST_DRAW_MAX 64
static Vtx chest_vtx[3][CHEST_DRAW_MAX][3 * 6 * 4];
static int chest_buf;

// face index (mod order: up, down, south, north, east, west) -> bk shade
static const u8 face_shade6[6] = { 255, 128, 204, 204, 153, 153 };

static void draw_one(Gfx **gfx, Mtx **mtx, const Chest *c, int slot, f32 cam[3], f32 pt) {
#ifdef MC_HAVE_TEXTURES
    // texOffs u, v; box min x, y, z; size w, h, d; part offset x, y, z
    static const s8 boxes[3][11] = {
        { 0, 19, 1, 0, 1, 14, 10, 14, 0, 0, 0 },   // bottom
        { 0, 0, 1, 0, 0, 14, 5, 14, 0, 9, 1 },     // lid
        { 0, 0, 7, -2, 14, 2, 4, 1, 0, 9, 1 },     // lock
    };
    // DOWN, UP, WEST, NORTH, EAST, SOUTH: corner indices, and the face's mod index
    static const u8 quads[6][4] = { { 5, 4, 0, 1 }, { 2, 3, 7, 6 }, { 0, 4, 7, 3 }, { 1, 0, 3, 2 }, { 5, 1, 2, 6 }, { 4, 5, 6, 7 } };
    static const u8 face_dir[6] = { 1, 0, 5, 3, 4, 2 };
    Vtx *v = chest_vtx[chest_buf][slot];
    int state = CELL_STATE(world_get_cell(c->pos[0], c->pos[1], c->pos[2]));
    f32 yrot, open, lid;
    int b, f, i, n = 0;
    Pose p;

    // Direction.toYRot: south 0, west 90, north 180, east 270 (state = face index)
    switch (state) {
        case 3: yrot = 180.0f; break; // north
        case 4: yrot = 270.0f; break; // east
        case 5: yrot = 90.0f; break;  // west
        default: yrot = 0.0f; break;  // south
    }
    open = c->o_openness + (c->openness - c->o_openness) * pt;
    open = 1.0f - open;
    open = 1.0f - open * open * open;
    lid = -(open * 90.0f);

    pose_identity(&p);
    pose_scale(&p, MC_BLOCK, MC_BLOCK, MC_BLOCK);
    pose_translate(&p, (c->pos[0] * MC_BLOCK - cam[0]) / MC_BLOCK, (c->pos[1] * MC_BLOCK - cam[1]) / MC_BLOCK,
        (c->pos[2] * MC_BLOCK - cam[2]) / MC_BLOCK);
    pose_translate(&p, 0.5f, 0.5f, 0.5f);
    pose_rotate(&p, 1, -yrot);
    pose_translate(&p, -0.5f, -0.5f, -0.5f);
    pose_scale(&p, 1.0f / 16.0f, 1.0f / 16.0f, 1.0f / 16.0f);

    mlMtxIdent();
    mlMtxApply(*mtx);
    gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_CHEST_START + slot, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    for (b = 0; b < 3; b++) {
        const s8 *bx = boxes[b];
        f32 x0 = bx[2], y0 = bx[3], z0 = bx[4];
        f32 x1 = x0 + bx[5], y1 = y0 + bx[6], z1 = z0 + bx[7];
        f32 w = bx[5], h = bx[6], d = bx[7];
        f32 u0 = bx[0], u1 = u0 + d, u2 = u0 + d + w, u22 = u0 + d + w + w, u3 = u0 + d + w + d, u4 = u3 + w;
        f32 v0 = bx[1], v1 = v0 + d, v2 = v0 + d + h;
        f32 c8[8][3] = {
            { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
            { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 },
        };
        const f32 uvs[6][4] = {
            { u1, v0, u2, v1 }, { u2, v1, u22, v0 }, { u0, v1, u1, v2 },
            { u1, v1, u2, v2 }, { u2, v1, u3, v2 }, { u3, v1, u4, v2 },
        };
        Pose part = p;
        // PartPose.offset, then the lid's (and lock's) xRot about it
        pose_translate(&part, bx[8], bx[9], bx[10]);
        if (b > 0) pose_rotate(&part, 0, lid);
        for (f = 0; f < 6; f++) {
            f32 tex[4][2], umin, umax, vmin, vmax;
            Vtx *q = &v[n * 4];
            tex[0][0] = uvs[f][2]; tex[0][1] = uvs[f][1];
            tex[1][0] = uvs[f][0]; tex[1][1] = uvs[f][1];
            tex[2][0] = uvs[f][0]; tex[2][1] = uvs[f][3];
            tex[3][0] = uvs[f][2]; tex[3][1] = uvs[f][3];
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
                f32 o[3];
                pose_apply(&part, c8[quads[f][i]], o);
                q[i].v.ob[0] = (s16)o[0];
                q[i].v.ob[1] = (s16)o[1];
                q[i].v.ob[2] = (s16)o[2];
                q[i].v.flag = 0;
                // absolute texel coords (the tile's size starts at umin, vmin)
                q[i].v.tc[0] = (s16)(tex[i][0] * 32.0f);
                q[i].v.tc[1] = (s16)(tex[i][1] * 32.0f);
                q[i].v.cn[0] = q[i].v.cn[1] = q[i].v.cn[2] = face_shade6[face_dir[f]];
                q[i].v.cn[3] = 0xFF;
            }
            // only this face's part of the 64x64 texture (tmem holds 1024 texels)
            gDPLoadTextureTile((*gfx)++, mc_chest_tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 64, 64,
                (s32)umin, (s32)vmin, (s32)umax - 1, (s32)vmax - 1, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
                G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            gSPVertex((*gfx)++, q, 4, 0);
            gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
            n++;
        }
    }
    gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
    gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
#endif
}

void chests_draw(Gfx **gfx, Mtx **mtx, f32 cam[3], f32 pt) {
    s16 map = (s16)map_get();
    int i, slot = 0;
    chest_buf = (chest_buf + 1) % 3;
    for (i = 0; i < MAX_CHESTS && slot < CHEST_DRAW_MAX; i++) {
        Chest *c = &chests[i];
        f32 dx, dy, dz;
        if (!c->used || c->map != map) continue;
        if (world_get(c->pos[0], c->pos[1], c->pos[2]) != BLOCK_CHEST) continue;
        dx = c->pos[0] * MC_BLOCK - cam[0];
        dy = c->pos[1] * MC_BLOCK - cam[1];
        dz = c->pos[2] * MC_BLOCK - cam[2];
        if (dx * dx + dy * dy + dz * dz > (64.0f * MC_BLOCK) * (64.0f * MC_BLOCK)) continue; // (render distance)
        draw_one(gfx, mtx, c, slot++, cam, pt);
    }
}

// --- saving (mc_steve_<20 + slot>.bin, "MCC1") ---

#define MOUSE_MOD_ID "bk_minecraft_mouse"
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_begin(void));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_put(int b));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_commit(int slot));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_load_open(int slot));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_load_get(int i));
RECOMP_IMPORT(MOUSE_MOD_ID, int mc_save_delete(int slot));

static int io_ok(void) {
    return recomp_is_dependency_met(MOUSE_MOD_ID) == DEPENDENCY_STATUS_FOUND;
}

static void put16(int v) {
    mc_save_put(v & 0xFF);
    mc_save_put((v >> 8) & 0xFF);
}

static ItemId item_by_name(const char *name) {
    int i, k;
    if (name[0] == 0) return 0;
    for (i = 1; i < MC_ITEM_END; i++) {
        const char *r = mc_registry_names[i];
        for (k = 0; r[k] != 0 && r[k] == name[k]; k++) { }
        if (r[k] == 0 && name[k] == 0) return (ItemId)i;
    }
    return 0;
}

RECOMP_HOOK("gameFile_save") void chests_on_game_save(s32 gamenum) {
    int i, n = 0, k;
    if (!io_ok()) return;
    for (i = 0; i < MAX_CHESTS; i++) if (chests[i].used) n++;
    mc_save_begin();
    mc_save_put('M');
    mc_save_put('C');
    mc_save_put('C');
    mc_save_put('1');
    put16(n);
    for (i = 0; i < MAX_CHESTS; i++) {
        Chest *c = &chests[i];
        if (!c->used) continue;
        put16(c->map);
        put16(c->pos[0]);
        put16(c->pos[1]);
        put16(c->pos[2]);
        for (k = 0; k < CHEST_SLOTS; k++) {
            ItemStack *st = &c->items[k];
            const char *name = st->count ? mc_registry_names[st->id] : "";
            int len = 0, j;
            while (name[len]) len++;
            mc_save_put(len);
            for (j = 0; j < len; j++) mc_save_put(name[j]);
            mc_save_put(len ? st->count : 0);
            put16(st->damage);
        }
    }
    mc_save_commit(20 + gamenum);
    recomp_printf("[mc] chests saved for game %d (%d)\n", gamenum, n);
}

static int rd;
static int get8(void) { return mc_load_get(rd++) & 0xFF; }
static int get16(void) { int lo = get8(); return lo | (get8() << 8); }

RECOMP_HOOK("gameFile_load") void chests_on_game_load(s32 gamenum) {
    int len, n, i, k;
    chests_reset();
    if (!io_ok()) return;
    len = mc_load_open(20 + gamenum);
    rd = 0;
    if (len < 6 || get8() != 'M' || get8() != 'C' || get8() != 'C' || get8() != '1') return;
    n = get16();
    for (i = 0; i < n && i < MAX_CHESTS && rd < len; i++) {
        Chest *c = &chests[i];
        c->used = TRUE;
        c->map = (s16)get16();
        c->pos[0] = (s16)get16();
        c->pos[1] = (s16)get16();
        c->pos[2] = (s16)get16();
        c->openness = c->o_openness = 0.0f;
        c->viewers = 0;
        for (k = 0; k < CHEST_SLOTS; k++) {
            char name[65];
            int nl = get8(), j;
            for (j = 0; j < nl; j++) {
                int ch = get8();
                if (j < 64) name[j] = (char)ch;
            }
            name[nl < 64 ? nl : 64] = 0;
            c->items[k].count = (u8)get8();
            c->items[k].damage = (u16)get16();
            c->items[k].id = item_by_name(name);
            if (c->items[k].id == 0 || c->items[k].count == 0) {
                c->items[k].id = 0;
                c->items[k].count = 0;
                c->items[k].damage = 0;
            }
        }
    }
    recomp_printf("[mc] chests loaded for game %d (%d)\n", gamenum, n);
}

RECOMP_HOOK("gameFile_clear") void chests_on_game_clear(s32 gamenum) {
    chests_reset();
    if (io_ok()) mc_save_delete(20 + gamenum);
}

// the chest's item model (template_chest: ChestModel, lid shut) inside the
// unit cube of a pose, for the hand / dropped items / anything drawing a
// block cube. only the faces turned toward the viewer (no depth buffer in the
// hand): view_ortho = the gui's view down -z, else the camera at the origin
#define ITEM_RING 32
static Vtx item_vtx[3][ITEM_RING][18 * 4];
static int item_ring, item_frame;

void chest_emit_item(Gfx **gfx, const Pose *unit, int view_ortho) {
#ifdef MC_HAVE_TEXTURES
    static const s8 boxes[3][11] = {
        { 0, 19, 1, 0, 1, 14, 10, 14, 0, 0, 0 },
        { 0, 0, 1, 0, 0, 14, 5, 14, 0, 9, 1 },
        { 0, 0, 7, -2, 14, 2, 4, 1, 0, 9, 1 },
    };
    static const u8 quads[6][4] = { { 5, 4, 0, 1 }, { 2, 3, 7, 6 }, { 0, 4, 7, 3 }, { 1, 0, 3, 2 }, { 5, 1, 2, 6 }, { 4, 5, 6, 7 } };
    static const u8 face_dir[6] = { 1, 0, 5, 3, 4, 2 };
    Pose p = *unit;
    Vtx *v;

    int b, f, i, n = 0;

    // a ring of vertex buffers that outlives the frame being drawn
    item_ring = (item_ring + 1) % ITEM_RING;
    if (item_ring == 0) item_frame = (item_frame + 1) % 3;
    v = item_vtx[item_frame][item_ring];
    pose_scale(&p, 1.0f / 16.0f, 1.0f / 16.0f, 1.0f / 16.0f);
    for (b = 0; b < 3; b++) {
        const s8 *bx = boxes[b];
        f32 x0 = bx[2] + bx[8], y0 = bx[3] + bx[9], z0 = bx[4] + bx[10];
        f32 x1 = x0 + bx[5], y1 = y0 + bx[6], z1 = z0 + bx[7];
        f32 w = bx[5], h = bx[6], d = bx[7];
        f32 u0 = bx[0], u1 = u0 + d, u2 = u0 + d + w, u22 = u0 + d + w + w, u3 = u0 + d + w + d, u4 = u3 + w;
        f32 v0 = bx[1], v1 = v0 + d, v2 = v0 + d + h;
        f32 c8[8][3] = {
            { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
            { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 },
        };
        const f32 uvs[6][4] = {
            { u1, v0, u2, v1 }, { u2, v1, u22, v0 }, { u0, v1, u1, v2 },
            { u1, v1, u2, v2 }, { u2, v1, u3, v2 }, { u3, v1, u4, v2 },
        };
        for (f = 0; f < 6; f++) {
            f32 tex[4][2], umin, umax, vmin, vmax, pts[4][3], qc[3] = { 0, 0, 0 }, out[3], fc[3];
            int visible;
            Vtx *q = &v[n * 4];
            tex[0][0] = uvs[f][2]; tex[0][1] = uvs[f][1];
            tex[1][0] = uvs[f][0]; tex[1][1] = uvs[f][1];
            tex[2][0] = uvs[f][0]; tex[2][1] = uvs[f][3];
            tex[3][0] = uvs[f][2]; tex[3][1] = uvs[f][3];
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
                int k;
                pose_apply(&p, c8[quads[f][i]], pts[i]);
                for (k = 0; k < 3; k++) qc[k] += pts[i][k] / 4.0f;
            }
            // outward: from the box's own middle to the face's middle
            {
                f32 bm[3] = { (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2 };
                pose_apply(&p, bm, fc);
            }
            for (i = 0; i < 3; i++) out[i] = qc[i] - fc[i];
            visible = view_ortho ? out[2] > 0.0f : (qc[0] * out[0] + qc[1] * out[1] + qc[2] * out[2]) < 0.0f;
            if (!visible) continue;
            for (i = 0; i < 4; i++) {
                q[i].v.ob[0] = (s16)pts[i][0];
                q[i].v.ob[1] = (s16)pts[i][1];
                q[i].v.ob[2] = (s16)pts[i][2];
                q[i].v.flag = 0;
                q[i].v.tc[0] = (s16)(tex[i][0] * 32.0f);
                q[i].v.tc[1] = (s16)(tex[i][1] * 32.0f);
                q[i].v.cn[0] = q[i].v.cn[1] = q[i].v.cn[2] = face_shade6[face_dir[f]];
                q[i].v.cn[3] = 0xFF;
            }
            gDPLoadTextureTile((*gfx)++, mc_chest_tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 64, 64,
                (s32)umin, (s32)vmin, (s32)umax - 1, (s32)vmax - 1, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
                G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            gSPVertex((*gfx)++, q, 4, 0);
            gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
            n++;
        }
    }
#endif
}
