// block rendering: one mesh per 16x16x16 section, only faces that aren't
// hidden by an opaque neighbor, minecraft's per-face brightness baked into the
// vertex colors. textures come from generated/mc_textures.h, which
// tools/export_mc_assets.py makes from your own minecraft jar (never
// committed). without it, blocks fall back to flat colors.

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "pose.h"
#include "mc_gbi.h"
#include "redstone.h"
#include "tnt.h"
#include "chest.h"
#include "shapes.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

// textures per face come from mc_blocks[].tex (generated/mc_data.h)

// face order: +y, -y, +z, -z, +x, -x
static const s8 face_dir[6][3] = {
    { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 },
};
// minecraft's directional shading: top 1.0, bottom 0.5, north/south 0.8, east/west 0.6
static const u8 face_shade[6] = { 255, 128, 204, 204, 153, 153 };
// corners of each face on the unit cube, counter-clockwise from outside
static const u8 face_corners[6][4][3] = {
    { { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },
    { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } },
    { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } },
    { { 1, 0, 0 }, { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 } },
    { { 1, 0, 1 }, { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 } },
    { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 } },
};

// --- freeing old meshes a few frames late ---
// the gpu side may still be reading last frame's buffers, so a replaced mesh
// isn't freed right away.

#define GRAVEYARD_SIZE 16384 // (water chunks, patches and walls bury their old buffers)
#define GRAVEYARD_DELAY 8

typedef struct {
    void *ptr;
    u32 frame;
} Grave;

static Grave graveyard[GRAVEYARD_SIZE];
static u32 draw_frame = 0;

static s32 grave_next; // where to start looking for a free slot

static void bury(void *p) {
    s32 i;

    if (p == NULL) {
        return;
    }
    for (i = 0; i < GRAVEYARD_SIZE; i++) {
        s32 k = (grave_next + i) % GRAVEYARD_SIZE;
        if (graveyard[k].ptr == NULL) {
            graveyard[k].ptr = p;
            graveyard[k].frame = draw_frame;
            grave_next = (k + 1) % GRAVEYARD_SIZE;
            return;
        }
    }
    // full: leak rather than risk freeing something in use
    recomp_printf("[mc] mesh graveyard full\n");
}

static void dig_up(void) {
    s32 i;

    for (i = 0; i < GRAVEYARD_SIZE; i++) {
        if (graveyard[i].ptr != NULL && (draw_frame - graveyard[i].frame) > GRAVEYARD_DELAY) {
            recomp_free(graveyard[i].ptr);
            graveyard[i].ptr = NULL;
        }
    }
}

void world_draw_bury(void *p) {
    bury(p);
}

void world_draw_free_mesh(Section *s) {
    bury(s->vtx);
    bury(s->dl);
    s->vtx = NULL;
    s->dl = NULL;
    s->quad_count = 0;
}

// --- meshing ---

static BlockId neighbor_block(Section *s, s32 x, s32 y, s32 z) {
    if (x >= 0 && x < SECTION_SIZE && y >= 0 && y < SECTION_SIZE && z >= 0 && z < SECTION_SIZE) {
        return CELL_ID(s->blocks[(y * SECTION_SIZE + z) * SECTION_SIZE + x]);
    }
    return world_get(s->cx * SECTION_SIZE + x, s->cy * SECTION_SIZE + y, s->cz * SECTION_SIZE + z);
}

// Block.shouldRenderFace, simplified: hidden by opaque neighbors, and glass
// next to glass (minecraft's HalfTransparentBlock.skipRendering)
static int face_visible(BlockId self, BlockId other) {
    if (block_is_opaque(other)) {
        return FALSE;
    }
    if (self == other) {
        return FALSE; // glass next to the same glass etc. (HalfTransparentBlock)
    }
    return TRUE;
}

// a cell's face texture: the state's rotated model if the block has states
static u16 face_texture(u16 cell, s32 f) {
    BlockId b = CELL_ID(cell);
    if (b < MC_BLOCK_COUNT && mc_block_place[b] != PLACE_NONE) {
        return mc_state_tex[b][CELL_STATE(cell) % 6][f];
    }
    return mc_block(b)->tex[f];
}

// the state's quarter turns of that face's texture (clockwise)
static u8 face_uv_turns(u16 cell, s32 f) {
    BlockId b = CELL_ID(cell);
    if (b < MC_BLOCK_COUNT && mc_block_place[b] != PLACE_NONE) {
        return mc_state_rot[b][CELL_STATE(cell) % 6][f];
    }
    return 0;
}

// turn texel coords (0..16) clockwise on the face
static void turn_uv(u8 k, s32 *u, s32 *v) {
    while (k-- > 0) {
        s32 t = *u;
        *u = *v;
        *v = 16 - t;
    }
}

// texture coords (s, t) for a face corner, in texels: t runs down the side faces
static void corner_uv(s32 f, const u8 *c, s32 *u, s32 *v) {
    switch (f) {
        case 0: *u = c[0]; *v = c[2]; break;
        case 1: *u = c[0]; *v = 1 - c[2]; break; // FaceBakery.defaultFaceUV DOWN: v = 16 - z
        case 2: *u = c[0]; *v = 1 - c[1]; break;
        case 3: *u = 1 - c[0]; *v = 1 - c[1]; break;
        case 4: *u = 1 - c[2]; *v = 1 - c[1]; break;
        default: *u = c[2]; *v = 1 - c[1]; break;
    }
    *u *= 16;
    *v *= 16;
}

#ifdef MC_HAVE_MODELS
// a model block's quad ranges: its state's row, or for multipart blocks
// (redstone dust) every part whose condition holds
#define MAX_RANGES 16
static int model_ranges(u16 cell, s32 wx, s32 wy, s32 wz, u16 *first, u16 *count) {
    BlockId b = CELL_ID(cell);
    const McBlockModel *m = &mc_block_models[b];
    int n = 0, i, fam = shapes_family(b);

    if (b == BLOCK_REDSTONE_WIRE || b == BLOCK_FIRE || fam == FAM_FENCE || fam == FAM_PANE) {
        u8 sides[5] = { 1, 1, 1, 1, 1 };
        u32 seed = (u32)wx * 3129871u ^ (u32)wz * 116129781u ^ (u32)wy; // (Mth.getSeed-like)
        if (b == BLOCK_REDSTONE_WIRE) redstone_wire_sides(wx, wy, wz, cell, sides);
        else if (b == BLOCK_FIRE) fire_sides(wx, wy, wz, sides);
        else {
            // fence / pane: the stored sides (north, east, south, west: false 1, true 2)
            int k, mask = CELL_STATE(cell) & 15;
            for (k = 0; k < 4; k++) sides[k] = (mask & (1 << k)) ? 2 : 1;
        }
        for (i = 0; i < m->count && n < MAX_RANGES; i++) {
            const McModelPart *pt = &mc_model_parts[m->first + i];
            int t, ok = FALSE;
            for (t = 0; t < pt->terms && !ok; t++) {
                int k;
                ok = TRUE;
                for (k = 0; k < 5; k++) {
                    if (!(pt->cond[t][k] & sides[k])) ok = FALSE;
                }
            }
            // a random pick among the alternatives, fixed per position and part
            if (ok && pt->alts > 1) {
                u32 h = (seed + (u32)(m->first + i - pt->alt) * 2654435761u) >> 8;
                ok = (int)(h % pt->alts) == pt->alt;
            }
            if (ok) {
                first[n] = pt->first;
                count[n] = pt->count;
                n++;
            }
        }
        return n;
    }
    {
        int st = (mc_block(b)->flags & MCB_TEX_SLOTS) ? shapes_model_state(cell) : redstone_model_state(cell);
        if (st >= m->count) st = 0;
        first[0] = mc_model_states[m->first + st].first;
        count[0] = mc_model_states[m->first + st].count;
        return 1;
    }
}

// a model quad's texture: the family's slot for shaped families
static u16 quad_tex(BlockId b, const McQuad *mq) {
    return (mc_block(b)->flags & MCB_TEX_SLOTS) ? mc_block_slots[b][mq->tex] : mq->tex;
}

static int model_quad_visible(Section *s, s32 x, s32 y, s32 z, const McQuad *mq) {
    if (mq->cull > 5) return TRUE;
    return !block_is_opaque(neighbor_block(s, x + face_dir[mq->cull][0], y + face_dir[mq->cull][1], z + face_dir[mq->cull][2]));
}

// 64 bit products from 32 bit multiplies (no 64 bit helpers in a mod)
static u64 mul64(u64 a, u64 b) {
    u32 al = (u32)a, ah = (u32)(a >> 32), bl = (u32)b, bh = (u32)(b >> 32);
    u64 lo = (u64)al * bl;
    u32 cross = al * bh + ah * bl;
    return lo + ((u64)cross << 32);
}

// Block.getOffset (OffsetType XZ / XYZ, Mth.getSeed(x, 0, z)), in 1/256 block
static void block_offset(BlockId b, s32 x, s32 z, s32 out[3]) {
    s64 seed;
    f32 ox, oy = 0.0f, oz;
    out[0] = out[1] = out[2] = 0;
    if (b >= MC_BLOCK_COUNT || mc_block_offset[b] == 0) return;
    seed = (s64)(s32)(x * 3129871) ^ ((s64)z * 116129781LL);
    seed = (s64)(mul64(mul64((u64)seed, (u64)seed), 42317861ULL) + mul64((u64)seed, 11ULL));
    seed >>= 16;
    ox = ((f32)(s32)(seed & 15) / 15.0f - 0.5f) * 0.5f;
    oz = ((f32)(s32)((seed >> 8) & 15) / 15.0f - 0.5f) * 0.5f;
    if (ox > 0.25f) ox = 0.25f;
    if (ox < -0.25f) ox = -0.25f;
    if (oz > 0.25f) oz = 0.25f;
    if (oz < -0.25f) oz = -0.25f;
    if (mc_block_offset[b] == 2) oy = ((f32)(s32)((seed >> 4) & 15) / 15.0f - 1.0f) * 0.2f;
    out[0] = (s32)(ox * 256.0f);
    out[1] = (s32)(oy * 256.0f);
    out[2] = (s32)(oz * 256.0f);
}

static s32 quad_offset[3]; // (the cell's draw offset, for emit_model_quad)

static void emit_model_quad(Vtx *v, const McQuad *mq, s32 x, s32 y, s32 z, u32 tint) {
    int c;
    u8 shade = face_shade[mq->shade & 7];
    u8 r = shade, g = shade, b = shade;

    if (mq->shade & 0x80) {
        r = (u8)(((tint >> 16) & 0xFF) * shade / 255);
        g = (u8)(((tint >> 8) & 0xFF) * shade / 255);
        b = (u8)((tint & 0xFF) * shade / 255);
    }
    for (c = 0; c < 4; c++) {
        v[c].v.ob[0] = (s16)(x * (s32)MC_BLOCK + (mq->v[c][0] + quad_offset[0]) * (s32)MC_BLOCK / 256);
        v[c].v.ob[1] = (s16)(y * (s32)MC_BLOCK + (mq->v[c][1] + quad_offset[1]) * (s32)MC_BLOCK / 256);
        v[c].v.ob[2] = (s16)(z * (s32)MC_BLOCK + (mq->v[c][2] + quad_offset[2]) * (s32)MC_BLOCK / 256);
        v[c].v.flag = 0;
        v[c].v.tc[0] = mq->uv[c][0];
        v[c].v.tc[1] = mq->uv[c][1];
        v[c].v.cn[0] = r;
        v[c].v.cn[1] = g;
        v[c].v.cn[2] = b;
        v[c].v.cn[3] = 0xFF;
    }
}
#endif

static void rebuild_mesh(Section *s) {
    s32 x, y, z, f, quads = 0, q, t, groups = 0;
    s32 tex_count[MC_TEX_COUNT];
    s32 tex_next[MC_TEX_COUNT];
    Vtx *vtx;
    Gfx *dl, *g;

    world_draw_free_mesh(s);
    s->dirty = FALSE;
    if (s->block_count == 0) {
        return;
    }

    // count visible faces per texture, so the mesh can be sorted by texture
    // (one texture load per group instead of one per face)
    for (t = 0; t < MC_TEX_COUNT; t++) {
        tex_count[t] = 0;
    }
    for (y = 0; y < SECTION_SIZE; y++) {
        for (z = 0; z < SECTION_SIZE; z++) {
            for (x = 0; x < SECTION_SIZE; x++) {
                u16 cell = s->blocks[(y * SECTION_SIZE + z) * SECTION_SIZE + x];
                BlockId b = CELL_ID(cell);
                if (b == BLOCK_AIR || b >= BLOCK_COUNT || (mc_block(b)->flags & (MCB_FLUID | MCB_CHEST))) {
                    continue; // (fluids.c / chest.c draw these)
                }
#ifdef MC_HAVE_MODELS
                if (mc_block(b)->flags & MCB_MODEL) {
                    u16 rf[MAX_RANGES], rc[MAX_RANGES];
                    int nr = model_ranges(cell, s->cx * SECTION_SIZE + x, s->cy * SECTION_SIZE + y, s->cz * SECTION_SIZE + z, rf, rc), r, k;
                    for (r = 0; r < nr; r++) {
                        for (k = 0; k < rc[r]; k++) {
                            const McQuad *mq = &mc_model_quads[rf[r] + k];
                            if (model_quad_visible(s, x, y, z, mq)) {
                                tex_count[quad_tex(b, mq)]++;
                                quads++;
                            }
                        }
                    }
                    continue;
                }
#endif
                for (f = 0; f < 6; f++) {
                    if (face_visible(b, neighbor_block(s, x + face_dir[f][0], y + face_dir[f][1], z + face_dir[f][2]))) {
                        tex_count[face_texture(cell, f)]++;
                        quads++;
                    }
                }
            }
        }
    }
    if (quads == 0) {
        return;
    }
    q = 0;
    for (t = 0; t < MC_TEX_COUNT; t++) {
        tex_next[t] = q;
        q += tex_count[t];
        if (tex_count[t] > 0) {
            groups++;
        }
    }

    // per texture group: a 7 command texture load, then batches of 8 quads
    // (32 verts, the f3dex vertex cache) = 1 vertex load + 8 triangle pairs
    vtx = recomp_alloc(sizeof(Vtx) * 4 * quads);
    dl = recomp_alloc(sizeof(Gfx) * (groups * 7 + (quads / 8 + groups) * 9 + 1));
    if (vtx == NULL || dl == NULL) {
        if (vtx) recomp_free(vtx);
        if (dl) recomp_free(dl);
        return;
    }

    for (y = 0; y < SECTION_SIZE; y++) {
        for (z = 0; z < SECTION_SIZE; z++) {
            for (x = 0; x < SECTION_SIZE; x++) {
                u16 cell = s->blocks[(y * SECTION_SIZE + z) * SECTION_SIZE + x];
                BlockId b = CELL_ID(cell);
                if (b == BLOCK_AIR || b >= BLOCK_COUNT || (mc_block(b)->flags & (MCB_FLUID | MCB_CHEST))) {
                    continue; // (fluids.c / chest.c draw these)
                }
#ifdef MC_HAVE_MODELS
                if (mc_block(b)->flags & MCB_MODEL) {
                    u16 rf[MAX_RANGES], rc[MAX_RANGES];
                    s32 wx = s->cx * SECTION_SIZE + x, wy = s->cy * SECTION_SIZE + y, wz = s->cz * SECTION_SIZE + z;
                    int nr = model_ranges(cell, wx, wy, wz, rf, rc), r, k;
                    // tints: dust by power, plants (short grass, ferns) the grass colour
                    u32 tint = (b == BLOCK_REDSTONE_WIRE) ? redstone_wire_color(CELL_STATE(cell) & 15)
                             : (shapes_family(b) == FAM_PLANT) ? 0x91BD59 : 0xFFFFFF;
                    block_offset(b, wx, wz, quad_offset);
                    for (r = 0; r < nr; r++) {
                        for (k = 0; k < rc[r]; k++) {
                            const McQuad *mq = &mc_model_quads[rf[r] + k];
                            if (model_quad_visible(s, x, y, z, mq)) {
                                emit_model_quad(&vtx[tex_next[quad_tex(b, mq)]++ * 4], mq, x, y, z, tint);
                            }
                        }
                    }
                    continue;
                }
#endif
                for (f = 0; f < 6; f++) {
                    s32 c, slot;
                    u16 tex;

                    if (!face_visible(b, neighbor_block(s, x + face_dir[f][0], y + face_dir[f][1], z + face_dir[f][2]))) {
                        continue;
                    }
                    tex = face_texture(cell, f);
                    slot = tex_next[tex]++;
                    for (c = 0; c < 4; c++) {
                        Vtx *v = &vtx[slot * 4 + c];
                        s32 u, w;

                        corner_uv(f, face_corners[f][c], &u, &w);
                        turn_uv(face_uv_turns(cell, f), &u, &w);
                        v->v.ob[0] = (s16)((x + face_corners[f][c][0]) * (s32)MC_BLOCK);
                        v->v.ob[1] = (s16)((y + face_corners[f][c][1]) * (s32)MC_BLOCK);
                        v->v.ob[2] = (s16)((z + face_corners[f][c][2]) * (s32)MC_BLOCK);
                        v->v.flag = 0;
                        v->v.tc[0] = (s16)(u << 5);
                        v->v.tc[1] = (s16)(w << 5);
                        v->v.cn[0] = v->v.cn[1] = v->v.cn[2] = face_shade[f];
                        v->v.cn[3] = 0xFF;
                    }
                }
            }
        }
    }

    g = dl;
    q = 0;
    for (t = 0; t < MC_TEX_COUNT; t++) {
        s32 end = q + tex_count[t], batch;

        if (tex_count[t] == 0) {
            continue;
        }
#ifdef MC_HAVE_TEXTURES
        gDPLoadTextureBlock(g++, &mc_block_tex[t * 256], G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0,
            G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
#else
        // keep the size math the same without textures
        { int k; for (k = 0; k < 7; k++) gDPNoOp(g++); }
#endif
        for (batch = q; batch < end; batch += 8) {
            s32 n = end - batch;
            s32 i;

            if (n > 8) {
                n = 8;
            }
            gSPVertex(g++, &vtx[batch * 4], n * 4, 0);
            for (i = 0; i < n; i++) {
                gSP2Triangles(g++, i * 4, i * 4 + 1, i * 4 + 2, 0, i * 4, i * 4 + 2, i * 4 + 3, 0);
            }
        }
        q = end;
    }
    gSPEndDisplayList(g++);

    s->vtx = vtx;
    s->dl = dl;
    s->quad_count = (u16)quads;
}

// --- drawing ---

// a single textured block cube through a pose (the hotbar icons and the
// block held in first person). the unit cube 0..1 goes through `p`, which must
// already end in the final coordinates. view_ortho: the viewer looks down -z
// (the hud), otherwise the camera sits at the origin (the hand's view space).
// back faces are dropped on the cpu, so no depth buffer is needed.
// `buf` must hold 24 verts and stay alive until the frame is drawn.
// tex32: if not NULL, one 16x16 rgba8888 texture for every face (crack overlay)
void world_emit_block_cube(Gfx **gfx, BlockId block, const Pose *p, Vtx *buf, int view_ortho, const u32 *tex32) {
    f32 center[3], unit_center[3] = { 0.5f, 0.5f, 0.5f };
    s32 f, c;

    if (block == BLOCK_AIR || block >= BLOCK_COUNT) {
        return;
    }
    pose_apply(p, unit_center, center);
    if (block == BLOCK_CHEST && tex32 == NULL) {
        chest_emit_item(gfx, p, view_ortho); // the chest's item model (ChestModel)
        return;
    }
#ifdef MC_HAVE_MODELS
    // model blocks with a 3d item model (buttons, trapdoors, stairs, fences):
    // its quads facing the viewer (by their own normal), farthest first, out
    // of a per-frame pool (the caller's 24 verts only fit one box)
    if (tex32 == NULL && mc_block_item_models[block].count > 0) {
        static Vtx pool[3][4096];
        static int pool_used, pool_frame = -1;
        const McModelState *im = &mc_block_item_models[block];
        f32 pts[48][4][3], depth[48];
        int idx[48], quad_of[48], n = 0, q, k, i;
        Vtx *out;

        if (pool_frame != (int)gMcFrame) {
            pool_frame = (int)gMcFrame;
            pool_used = 0;
        }
        for (q = 0; q < im->count && q < 48; q++) {
            const McQuad *mq = &mc_model_quads[im->first + q];
            f32 e1[3], e2[3], nrm[3], qc[3] = { 0, 0, 0 };
            for (c = 0; c < 4; c++) {
                f32 l[3] = { mq->v[c][0] / 256.0f, mq->v[c][1] / 256.0f, mq->v[c][2] / 256.0f };
                pose_apply(p, l, pts[n][c]);
                for (k = 0; k < 3; k++) qc[k] += pts[n][c][k] * 0.25f;
            }
            for (k = 0; k < 3; k++) {
                e1[k] = pts[n][1][k] - pts[n][0][k];
                e2[k] = pts[n][2][k] - pts[n][0][k];
            }
            nrm[0] = e1[1] * e2[2] - e1[2] * e2[1];
            nrm[1] = e1[2] * e2[0] - e1[0] * e2[2];
            nrm[2] = e1[0] * e2[1] - e1[1] * e2[0];
            // facing the camera (ortho: the viewer looks down -z; else it sits at the origin)
            if (view_ortho ? nrm[2] <= 0.0f : (qc[0] * nrm[0] + qc[1] * nrm[1] + qc[2] * nrm[2]) >= 0.0f) continue;
            depth[n] = view_ortho ? qc[2] : -(qc[0] * qc[0] + qc[1] * qc[1] + qc[2] * qc[2]);
            quad_of[n] = q;
            idx[n] = n;
            n++;
        }
        for (i = 1; i < n; i++) {
            for (k = i; k > 0 && depth[idx[k - 1]] > depth[idx[k]]; k--) {
                int t = idx[k]; idx[k] = idx[k - 1]; idx[k - 1] = t;
            }
        }
        if (pool_used + n * 4 > 4096) return;
        out = &pool[pool_frame % 3][pool_used];
        pool_used += n * 4;
        (void)buf;
        for (i = 0; i < n; i++) {
            int e = idx[i];
            const McQuad *mq = &mc_model_quads[im->first + quad_of[e]];
            Vtx *v = &out[i * 4];
            f32 e1[3], e2[3], nx;
            for (k = 0; k < 3; k++) {
                e1[k] = pts[e][1][k] - pts[e][0][k];
                e2[k] = pts[e][2][k] - pts[e][0][k];
            }
            nx = e1[1] * e2[2] - e1[2] * e2[1];
            for (c = 0; c < 4; c++) {
                u8 sh = face_shade[mq->shade & 7];
                if (view_ortho) sh = ((mq->shade & 7) == 0) ? 255 : (nx < 0.0f ? 204 : 153);
                v[c].v.ob[0] = (s16)pts[e][c][0];
                v[c].v.ob[1] = (s16)pts[e][c][1];
                v[c].v.ob[2] = (s16)pts[e][c][2];
                v[c].v.flag = 0;
                v[c].v.tc[0] = mq->uv[c][0];
                v[c].v.tc[1] = mq->uv[c][1];
                v[c].v.cn[0] = v[c].v.cn[1] = v[c].v.cn[2] = sh;
                v[c].v.cn[3] = 0xFF;
            }
#ifdef MC_HAVE_TEXTURES
            gDPLoadTextureBlock((*gfx)++, &mc_block_tex[mq->tex * 256], G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0,
                G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
#endif
            gSPVertex((*gfx)++, v, 4, 0);
            gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
        }
        return;
    }
#endif
    if (tex32 != NULL) {
        gDPLoadTextureBlock((*gfx)++, tex32, G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 16, 0,
            G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
    }
    for (f = 0; f < 6; f++) {
        f32 fc_local[3], fc[3], out[3];
        Vtx *v = &buf[f * 4];
        u16 tex = face_texture(block, f);
        int visible;

        for (c = 0; c < 3; c++) {
            fc_local[c] = 0.5f + face_dir[f][c] * 0.5f;
        }
        pose_apply(p, fc_local, fc);
        for (c = 0; c < 3; c++) {
            out[c] = fc[c] - center[c];
        }
        if (view_ortho) {
            visible = out[2] > 0.0f;
        } else {
            visible = (fc[0] * out[0] + fc[1] * out[1] + fc[2] * out[2]) < 0.0f;
        }
        if (!visible) {
            continue;
        }
        for (c = 0; c < 4; c++) {
            f32 local[3], pos[3];
            s32 u, w;

            local[0] = face_corners[f][c][0];
            local[1] = face_corners[f][c][1];
            local[2] = face_corners[f][c][2];
            pose_apply(p, local, pos);
            corner_uv(f, face_corners[f][c], &u, &w);
            turn_uv(face_uv_turns(block, f), &u, &w);
            v[c].v.ob[0] = (s16)pos[0];
            v[c].v.ob[1] = (s16)pos[1];
            v[c].v.ob[2] = (s16)pos[2];
            v[c].v.flag = 0;
            v[c].v.tc[0] = (s16)(u << 5);
            v[c].v.tc[1] = (s16)(w << 5);
            // gui ("side" light): top brightest, the left face lighter than the right
            if (view_ortho) {
                v[c].v.cn[0] = v[c].v.cn[1] = v[c].v.cn[2] = (f == 0) ? 255 : (out[0] < 0.0f ? 204 : 153);
            } else {
                v[c].v.cn[0] = v[c].v.cn[1] = v[c].v.cn[2] = face_shade[f];
            }
            v[c].v.cn[3] = 0xFF;
        }
#ifdef MC_HAVE_TEXTURES
        if (tex32 == NULL) {
            gDPLoadTextureBlock((*gfx)++, &mc_block_tex[tex * 256], G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0,
                G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
        }
#else
        (void)tex;
#endif
        gSPVertex((*gfx)++, v, 4, 0);
        gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
    }
}

// the block model's "particle" texture: grass blocks use dirt, logs their side
u16 world_block_particle_tex(BlockId block) {
    if (block == BLOCK_GRASS) block = BLOCK_DIRT;
    return mc_block(block)->tex[2];
}

// one face of a block at a grid cell, camera relative (identity modelview).
// used for the sides of holes mined into bk's terrain. `buf` needs 4 verts.
void world_emit_block_face(Gfx **gfx, BlockId block, const s32 cell[3], u8 f, const f32 cam[3], Vtx *buf) {
    world_emit_block_face_clip(gfx, block, cell, f, cam, buf, NULL);
}

// same, but a side face's top corners can be pulled down to top_y[corner]
// (world y): hole walls end where bk's sloped ground does
// the 4 vertices of a (clipped) block face; returns its texture, or -1
int world_block_face_clip_vtx(BlockId block, const s32 cell[3], u8 f, const f32 cam[3], Vtx *buf, const f32 *top_y) {
    s32 c;
    u16 tex;

    if (block == BLOCK_AIR || block >= BLOCK_COUNT || f > 5) {
        return -1;
    }
    tex = face_texture(block, f);
    for (c = 0; c < 4; c++) {
        s32 u, w;
        corner_uv(f, face_corners[f][c], &u, &w);
        buf[c].v.ob[0] = (s16)((cell[0] + face_corners[f][c][0]) * MC_BLOCK - cam[0]);
        buf[c].v.ob[1] = (s16)((cell[1] + face_corners[f][c][1]) * MC_BLOCK - cam[1]);
        if (top_y != NULL && f >= 2 && face_corners[f][c][1] == 1) {
            f32 y0 = cell[1] * MC_BLOCK, h = (top_y[c] - y0) / MC_BLOCK;
            if (h < 0.0f) h = 0.0f;
            if (h > 1.0f) h = 1.0f;
            buf[c].v.ob[1] = (s16)(y0 + h * MC_BLOCK - cam[1]);
            w = (s32)((1.0f - h) * 16.0f); // side faces: v = 1 - y
        }
        turn_uv(face_uv_turns(block, f), &u, &w);
        buf[c].v.ob[2] = (s16)((cell[2] + face_corners[f][c][2]) * MC_BLOCK - cam[2]);
        buf[c].v.flag = 0;
        buf[c].v.tc[0] = (s16)(u << 5);
        buf[c].v.tc[1] = (s16)(w << 5);
        buf[c].v.cn[0] = buf[c].v.cn[1] = buf[c].v.cn[2] = face_shade[f];
        buf[c].v.cn[3] = 0xFF;
    }
    return tex;
}

void world_emit_block_face_clip(Gfx **gfx, BlockId block, const s32 cell[3], u8 f, const f32 cam[3], Vtx *buf, const f32 *top_y) {
    s32 c;
    u16 tex;

    if (block == BLOCK_AIR || block >= BLOCK_COUNT || f > 5) {
        return;
    }
    tex = face_texture(block, f);
    for (c = 0; c < 4; c++) {
        s32 u, w;
        corner_uv(f, face_corners[f][c], &u, &w);
        buf[c].v.ob[0] = (s16)((cell[0] + face_corners[f][c][0]) * MC_BLOCK - cam[0]);
        buf[c].v.ob[1] = (s16)((cell[1] + face_corners[f][c][1]) * MC_BLOCK - cam[1]);
        if (top_y != NULL && f >= 2 && face_corners[f][c][1] == 1) {
            f32 y0 = cell[1] * MC_BLOCK, h = (top_y[c] - y0) / MC_BLOCK;
            if (h < 0.0f) h = 0.0f;
            if (h > 1.0f) h = 1.0f;
            buf[c].v.ob[1] = (s16)(y0 + h * MC_BLOCK - cam[1]);
            w = (s32)((1.0f - h) * 16.0f); // side faces: v = 1 - y
        }
        turn_uv(face_uv_turns(block, f), &u, &w);
        buf[c].v.ob[2] = (s16)((cell[2] + face_corners[f][c][2]) * MC_BLOCK - cam[2]);
        buf[c].v.flag = 0;
        buf[c].v.tc[0] = (s16)(u << 5);
        buf[c].v.tc[1] = (s16)(w << 5);
        buf[c].v.cn[0] = buf[c].v.cn[1] = buf[c].v.cn[2] = face_shade[f];
        buf[c].v.cn[3] = 0xFF;
    }
#ifdef MC_HAVE_TEXTURES
    gDPLoadTextureBlock((*gfx)++, &mc_block_tex[tex * 256], G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0,
        G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
#else
    (void)tex;
#endif
    gSPVertex((*gfx)++, buf, 4, 0);
    gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
}

// a flat item (item/generated: the 16x16 sprite on a 1/16 thick slab at
// z 7.5..8.5) through a pose. only the side facing the camera is drawn, so it
// works without a depth buffer. `buf` needs 4 verts.
// a flat item, extruded like ItemModelGenerator.bakeExtrudedSprite: the
// sprite on the south (z 8.5) and north (z 7.5) faces, and getSideFaces: a
// 1-pixel side quad for every opaque pixel edge that borders a transparent
// one (checkTransition), 1/16 deep. each side shows its own pixel's colour
// (the uvs are the pixel inset by 0.1). vertices come from a ring pool that
// outlives the frame. `buf` is unused now (kept for the callers).
#define ITEM_POOL 24576
#define ITEM_PRECISION 4.0f
static Mtx item_unscale;
static int item_unscale_ready;
static Vtx item_pool[ITEM_POOL];
static int item_pool_pos;

// the item's commands go into our own ring of display lists too, so bk's
// display list only gets one call per item
#define ITEM_GFX_POOL 32768
static Gfx item_gfx_pool[ITEM_GFX_POOL];
static int item_gfx_pos;

static Vtx *item_pool_alloc(int n) {
    Vtx *v;
    if (item_pool_pos + n > ITEM_POOL) item_pool_pos = 0;
    v = &item_pool[item_pool_pos];
    item_pool_pos += n;
    return v;
}

static int sprite_opaque(const u32 *sprite, int x, int y) {
    // isTransparent: outside the sprite, or alpha 0
    return x >= 0 && y >= 0 && x < 16 && y < 16 && (sprite[y * 16 + x] & 0xFF) != 0;
}

static void item_quad(Vtx *v, const Pose *p, const f32 q[4][3], const f32 uv[4][2], u8 shade) {
    int i;
    for (i = 0; i < 4; i++) {
        f32 pos[3];
        pose_apply(p, q[i], pos);
        // 4x finer than bk units (a 1/4 scale matrix undoes it): 1-pixel
        // sides are only 5 units wide and whole-unit rounding made items wobble
        v[i].v.ob[0] = (s16)(pos[0] * ITEM_PRECISION + (pos[0] < 0 ? -0.5f : 0.5f));
        v[i].v.ob[1] = (s16)(pos[1] * ITEM_PRECISION + (pos[1] < 0 ? -0.5f : 0.5f));
        v[i].v.ob[2] = (s16)(pos[2] * ITEM_PRECISION + (pos[2] < 0 ? -0.5f : 0.5f));
        v[i].v.flag = 0;
        v[i].v.tc[0] = (s16)(uv[i][0] * 32.0f);
        v[i].v.tc[1] = (s16)(uv[i][1] * 32.0f);
        v[i].v.cn[0] = v[i].v.cn[1] = v[i].v.cn[2] = shade;
        v[i].v.cn[3] = 0xFF;
    }
}

int world_item_both_faces; // a z-buffered pass: draw both big faces (no camera guess)

void world_emit_item_sprite(Gfx **gfx, const u32 *sprite, const Pose *p, Vtx *buf, int view_ortho) {
    f32 c[3], f[3], out[3];
    int front, i, x, y, nq = 0, max_q = 16 * 16 * 4 + 1;
    Vtx *v;

    (void)buf;
    if (sprite == NULL) {
        return;
    }
    {
        f32 lc[3] = { 0.5f, 0.5f, 0.5f }, lf[3] = { 0.5f, 0.5f, 1.0f };
        pose_apply(p, lc, c);
        pose_apply(p, lf, f);
    }
    for (i = 0; i < 3; i++) out[i] = f[i] - c[i];
    if (view_ortho) {
        front = out[2] > 0.0f;
    } else {
        front = (c[0] * out[0] + c[1] * out[1] + c[2] * out[2]) < 0.0f;
    }

    v = item_pool_alloc(max_q * 4);
    // getSideFaces (sprite rows run down: y = 0 is the top of the item)
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            int d;
            if (!sprite_opaque(sprite, x, y)) continue;
            for (d = 0; d < 4; d++) {
                // UP: pixel above empty, DOWN: below, LEFT: x - 1, RIGHT: x + 1
                static const s8 step[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
                f32 q[4][3], uv[4][2], x0, x1, y0, y1;
                int k;

                if (sprite_opaque(sprite, x + step[d][0], y + step[d][1])) continue;
                // the edge in model units (y flipped: 16 - y)
                x0 = x / 16.0f;
                x1 = (x + 1) / 16.0f;
                y0 = (16 - y) / 16.0f;     // top of the pixel
                y1 = (16 - y - 1) / 16.0f; // bottom of the pixel
                switch (d) {
                    case 0: q[0][0] = x0; q[0][1] = y0; q[1][0] = x1; q[1][1] = y0; q[2][0] = x1; q[2][1] = y0; q[3][0] = x0; q[3][1] = y0; break;
                    case 1: q[0][0] = x0; q[0][1] = y1; q[1][0] = x1; q[1][1] = y1; q[2][0] = x1; q[2][1] = y1; q[3][0] = x0; q[3][1] = y1; break;
                    case 2: q[0][0] = x0; q[0][1] = y0; q[1][0] = x0; q[1][1] = y1; q[2][0] = x0; q[2][1] = y1; q[3][0] = x0; q[3][1] = y0; break;
                    default: q[0][0] = x1; q[0][1] = y0; q[1][0] = x1; q[1][1] = y1; q[2][0] = x1; q[2][1] = y1; q[3][0] = x1; q[3][1] = y0; break;
                }
                q[0][2] = q[1][2] = 7.5f / 16.0f;
                q[2][2] = q[3][2] = 8.5f / 16.0f;
                for (k = 0; k < 4; k++) {
                    uv[k][0] = x + 0.5f; // u0..u1 = x + 0.1 .. x + 0.9: one texel
                    uv[k][1] = y + 0.5f;
                }
                if (nq < max_q - 1) item_quad(&v[nq++ * 4], p, q, uv, (d < 2) ? 255 : 204);
            }
        }
    }
    // the visible big face last (no depth buffer in the hand pass); with a
    // depth buffer both, like ItemModelGenerator's south and north faces
    if (world_item_both_faces) {
        // the back face: the same texel at the same corner as the front (seen
        // from behind it's mirrored by itself). mirroring the uvs here put a
        // flipped second copy behind the front's see-through texels: an X
        f32 q[4][3] = { { 0, 1, 7.5f / 16.0f }, { 1, 1, 7.5f / 16.0f }, { 1, 0, 7.5f / 16.0f }, { 0, 0, 7.5f / 16.0f } };
        f32 uv[4][2] = { { 0, 0 }, { 16, 0 }, { 16, 16 }, { 0, 16 } };
        item_quad(&v[nq++ * 4], p, q, uv, 255);
        front = TRUE;
    }
    {
        f32 z = front ? 8.5f / 16.0f : 7.5f / 16.0f;
        f32 q[4][3] = { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 }, { 0, 0, 0 } };
        f32 uv[4][2] = { { 0, 0 }, { 16, 0 }, { 16, 16 }, { 0, 16 } };
        for (i = 0; i < 4; i++) {
            q[i][2] = z;
            if (!front) uv[i][0] = 16.0f - uv[i][0]; // NORTH_FACE_UVS: mirrored
        }
        item_quad(&v[nq++ * 4], p, q, uv, 255);
    }

    {
        int need = 8 + (nq / 8 + 1) + nq + 2;
        Gfx *g, *start;

        if (item_gfx_pos + need > ITEM_GFX_POOL) item_gfx_pos = 0;
        start = g = &item_gfx_pool[item_gfx_pos];
        item_gfx_pos += need + 2;
        if (!item_unscale_ready) {
            guScale(&item_unscale, 1.0f / ITEM_PRECISION, 1.0f / ITEM_PRECISION, 1.0f / ITEM_PRECISION);
            item_unscale_ready = TRUE;
        }
        gSPMatrix(g++, &item_unscale, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
        gDPLoadTextureBlock(g++, sprite, G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 16, 0,
            G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        for (i = 0; i < nq; i += 8) {
            int n = nq - i, k;
            if (n > 8) n = 8;
            gSPVertex(g++, &v[i * 4], n * 4, 0);
            for (k = 0; k < n; k++) {
                gSP2Triangles(g++, k * 4, k * 4 + 1, k * 4 + 2, 0, k * 4, k * 4 + 2, k * 4 + 3, 0);
            }
        }
        gSPPopMatrix(g++, G_MTX_MODELVIEW);
        gSPEndDisplayList(g++);
        gSPDisplayList((*gfx)++, start);
    }
}

// render state for minecraft textures: point sampled (crisp pixels), texture
// times vertex shade, alpha cutout for glass and leaves
void world_set_texture_state(Gfx **gfx) {
    gDPPipeSync((*gfx)++);
    gDPSetCycleType((*gfx)++, G_CYC_1CYCLE);
    gSPClearGeometryMode((*gfx)++, G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_CULL_BOTH | G_FOG);
    gSPSetGeometryMode((*gfx)++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
    if (1) { // (textures always exist now: the block tables need make assets)
        gDPSetTexturePersp((*gfx)++, G_TP_PERSP);
        gDPSetTextureLOD((*gfx)++, G_TL_TILE);
        gDPSetTextureLUT((*gfx)++, G_TT_NONE);
        gDPSetTextureDetail((*gfx)++, G_TD_CLAMP);
        gDPSetTextureConvert((*gfx)++, G_TC_FILT);
        gDPSetTextureFilter((*gfx)++, G_TF_POINT);
        gDPSetCombineKey((*gfx)++, G_CK_NONE);
        gDPSetAlphaCompare((*gfx)++, G_AC_THRESHOLD);
        gDPSetBlendColor((*gfx)++, 0, 0, 0, 0x80);
        gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_TEX_EDGE, G_RM_AA_ZB_TEX_EDGE2);
        gDPSetCombineMode((*gfx)++, G_CC_MODULATERGBA, G_CC_MODULATERGBA);
        gSPTexture((*gfx)++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    } else {
        gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
        gDPSetCombineMode((*gfx)++, G_CC_SHADE, G_CC_SHADE);
        gSPTexture((*gfx)++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    }
}

void world_restore_texture_state(Gfx **gfx) {
    gDPPipeSync((*gfx)++);
    gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
    gDPSetTextureFilter((*gfx)++, G_TF_BILERP);
    gSPTexture((*gfx)++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gDPPipelineMode((*gfx)++, G_PM_1PRIMITIVE);
    gDPSetCycleType((*gfx)++, G_CYC_2CYCLE);
}

// called from the player_draw hook, inside the 3d pass
void world_draw(Gfx **gfx, Mtx **mtx) {
    f32 cam[3];
    s32 i, drawn = 0;
    int state_set = FALSE;

    draw_frame++;
    dig_up();

    viewport_getPosition_vec3f(cam);
    for (i = 0; i < world_section_count(); i++) {
        Section *s = world_section_at(i);
        f32 ox, oy, oz;

        if (s->dirty) {
            rebuild_mesh(s);
        }
        if (s->dl == NULL) {
            continue;
        }
        ox = s->cx * SECTION_SIZE * MC_BLOCK;
        oy = s->cy * SECTION_SIZE * MC_BLOCK;
        oz = s->cz * SECTION_SIZE * MC_BLOCK;
        // bk culls models further than 17000 units from the camera, do the same
        if (ox - cam[0] > 17000.0f || cam[0] - ox > 17000.0f + 1280.0f
            || oy - cam[1] > 17000.0f || cam[1] - oy > 17000.0f + 1280.0f
            || oz - cam[2] > 17000.0f || cam[2] - oz > 17000.0f + 1280.0f) {
            continue;
        }
        if (drawn >= 96) {
            break; // keep some of bk's 700 per-frame matrices for bk
        }

        if (!state_set) {
            world_set_texture_state(gfx);
            state_set = TRUE;
        }

        mlMtxIdent();
        mlMtxTranslate(ox - cam[0], oy - cam[1], oz - cam[2]);
        mlMtxApply(*mtx);
        // tagged so rt64 interpolates it like bk's own models (see mc_gbi.h)
        gEXMatrixGroupSimpleNormal((*gfx)++, MC_ID_SECTION_START + i, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList((*gfx)++, s->dl);
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
        drawn++;
    }

    if (state_set) {
        world_restore_texture_state(gfx);
    }
}
