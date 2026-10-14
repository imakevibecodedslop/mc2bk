// the voxel grid: block storage, grid raycast, and the bk collision provider.

#include "fluids.h"
#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "terrain.h"
#include "shapes.h"

// plenty for building by hand. a full chunk system with eviction is later.
#define MAX_SECTIONS 256

static Section *sections[MAX_SECTIONS];
static s32 section_count = 0;
static Section *last_section = NULL; // lookup cache

int block_is_opaque(BlockId block) {
    return block != BLOCK_AIR && (mc_block(block)->flags & MCB_OPAQUE) != 0;
}

const char *block_name(BlockId block) {
    return mc_block(block)->name;
}

// --- helpers ---

s32 world_to_block(f32 v) {
    f32 b = v / MC_BLOCK;
    s32 i = (s32)b;
    if ((f32)i > b) {
        i--; // floor for negatives
    }
    return i;
}

static s32 floor_div16(s32 v) {
    return (v >= 0) ? (v / SECTION_SIZE) : -((-v + SECTION_SIZE - 1) / SECTION_SIZE);
}

static void mc_memzero(void *p, u32 size) {
    u8 *b = p;
    while (size--) {
        *b++ = 0;
    }
}

// defined in world_draw.c, frees a section's mesh safely
void world_draw_free_mesh(Section *s);

// --- storage ---

// sections by coordinate (open addressing; a miss ends at an empty slot
// instead of looking at every section)
#define SECTION_HASH 1024 // power of 2, > MAX_SECTIONS
static Section *section_hash[SECTION_HASH];

static u32 section_slot(s32 cx, s32 cy, s32 cz) {
    return ((u32)cx * 73856093u ^ (u32)cy * 19349663u ^ (u32)cz * 83492791u) & (SECTION_HASH - 1);
}

static Section *find_section(s32 cx, s32 cy, s32 cz) {
    u32 h, i;

    if (last_section != NULL && last_section->cx == cx && last_section->cy == cy && last_section->cz == cz) {
        return last_section;
    }
    h = section_slot(cx, cy, cz);
    for (i = 0; i < SECTION_HASH; i++) {
        Section *s = section_hash[(h + i) & (SECTION_HASH - 1)];
        if (s == NULL) return NULL;
        if (s->cx == cx && s->cy == cy && s->cz == cz) {
            last_section = s;
            return s;
        }
    }
    return NULL;
}

static void section_hash_add(Section *s) {
    u32 h = section_slot(s->cx, s->cy, s->cz), i;
    for (i = 0; i < SECTION_HASH; i++) {
        Section **e = &section_hash[(h + i) & (SECTION_HASH - 1)];
        if (*e == NULL) {
            *e = s;
            return;
        }
    }
}

static Section *create_section(s32 cx, s32 cy, s32 cz) {
    Section *s;

    if (section_count >= MAX_SECTIONS) {
        recomp_printf("[mc] out of sections\n");
        return NULL;
    }
    s = recomp_alloc(sizeof(Section));
    if (s == NULL) {
        return NULL;
    }
    mc_memzero(s, sizeof(Section));
    s->cx = cx;
    s->cy = cy;
    s->cz = cz;
    s->index = (u16)section_count;
    sections[section_count++] = s;
    section_hash_add(s);
    last_section = s;
    return s;
}

void world_clear(void) {
    s32 i;

    for (i = 0; i < section_count; i++) {
        world_draw_free_mesh(sections[i]);
        recomp_free(sections[i]);
        sections[i] = NULL;
    }
    section_count = 0;
    last_section = NULL;
    for (i = 0; i < SECTION_HASH; i++) section_hash[i] = NULL;
}

// blocks the per tick scans (redstone, tnt / fire, fluids) look for
int block_is_special(BlockId b) {
    if (b == BLOCK_AIR) return FALSE;
    if (b == BLOCK_TNT || b == BLOCK_FIRE || b == BLOCK_WATER || b == BLOCK_LAVA || b == BLOCK_REDSTONE_BLOCK) return TRUE;
    return (mc_block(b)->flags & MCB_MODEL) != 0;
}

BlockId world_get(s32 x, s32 y, s32 z) {
    s32 cx = floor_div16(x), cy = floor_div16(y), cz = floor_div16(z);
    Section *s = find_section(cx, cy, cz);

    if (s == NULL) {
        return BLOCK_AIR;
    }
    x -= cx * SECTION_SIZE;
    y -= cy * SECTION_SIZE;
    z -= cz * SECTION_SIZE;
    return CELL_ID(s->blocks[(y * SECTION_SIZE + z) * SECTION_SIZE + x]);
}

// the raw cell: block id + block state
u16 world_get_cell(s32 x, s32 y, s32 z) {
    s32 cx = floor_div16(x), cy = floor_div16(y), cz = floor_div16(z);
    Section *s = find_section(cx, cy, cz);

    if (s == NULL) return BLOCK_AIR;
    x -= cx * SECTION_SIZE;
    y -= cy * SECTION_SIZE;
    z -= cz * SECTION_SIZE;
    return s->blocks[(y * SECTION_SIZE + z) * SECTION_SIZE + x];
}

static void mark_dirty(s32 x, s32 y, s32 z) {
    Section *s = find_section(floor_div16(x), floor_div16(y), floor_div16(z));
    if (s != NULL) {
        s->dirty = TRUE;
    }
}

void world_set(s32 x, s32 y, s32 z, BlockId block) {
    s32 cx = floor_div16(x), cy = floor_div16(y), cz = floor_div16(z);
    s32 lx = x - cx * SECTION_SIZE, ly = y - cy * SECTION_SIZE, lz = z - cz * SECTION_SIZE;
    Section *s = find_section(cx, cy, cz);
    BlockId *cell;

    if (s == NULL) {
        if (block == BLOCK_AIR) {
            return;
        }
        s = create_section(cx, cy, cz);
        if (s == NULL) {
            return;
        }
    }
    cell = &s->blocks[(ly * SECTION_SIZE + lz) * SECTION_SIZE + lx];
    if (*cell == block) {
        return;
    }
    if (*cell == BLOCK_AIR) {
        s->block_count++;
    } else if (block == BLOCK_AIR) {
        s->block_count--;
    }
    if (block_is_special(CELL_ID(*cell))) s->special_count--;
    if (block_is_special(CELL_ID(block))) s->special_count++;
    {
        // air <-> water / lava, or one fluid level to another: the block mesh
        // doesn't change (fluids.c draws fluids, and they hide no faces)
        BlockId was = CELL_ID(*cell), now = CELL_ID(block);
        int was_soft = was == BLOCK_AIR || (mc_block(was)->flags & MCB_FLUID);
        int now_soft = now == BLOCK_AIR || (mc_block(now)->flags & MCB_FLUID);
        *cell = block;
        world_change_count++;
        fluids_on_change(x, y, z); // LiquidBlock.onPlace / neighborChanged
        if (was_soft && now_soft) return;
    }
    s->dirty = TRUE;

    // neighbors in other sections may show/hide a face now
    if (lx == 0) mark_dirty(x - 1, y, z);
    if (lx == SECTION_SIZE - 1) mark_dirty(x + 1, y, z);
    if (ly == 0) mark_dirty(x, y - 1, z);
    if (ly == SECTION_SIZE - 1) mark_dirty(x, y + 1, z);
    if (lz == 0) mark_dirty(x, y, z - 1);
    if (lz == SECTION_SIZE - 1) mark_dirty(x, y, z + 1);
    shapes_on_set(x, y, z); // updateShape around it (stairs, fences, panes, doors)
}

// the section holding a block, as its world_section_at index, or -1
int world_section_index_at(s32 x, s32 y, s32 z) {
    Section *s = find_section(floor_div16(x), floor_div16(y), floor_div16(z));
    return s ? s->index : -1;
}

s32 world_section_count(void) {
    return section_count;
}

Section *world_section_at(s32 i) {
    return sections[i];
}

// --- raycast (amanatides & woo grid walk) ---

u32 world_change_count; // bumps on every world_set that changes a cell
int world_pick_mode; // aiming (outline / mining / using): noCollision blocks count

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif
#include "redstone.h"
#include "shapes.h"

// a cell's boxes (getShape when aiming, getCollisionShape otherwise): the
// families' boxes, else the model state's one box, else the full cube
int world_cell_boxes(u16 c, int collision, McBox *out, int max) {
    int n = shapes_boxes(c, collision, out, max);
    if (n >= 0) return n;
    if (max < 1) return 0;
    world_cell_box(c, out[0].lo, out[0].hi);
    return 1;
}

void world_cell_box(u16 c, u8 lo[3], u8 hi[3]) {
    int i;
#ifdef MC_HAVE_MODELS
    BlockId b = CELL_ID(c);
    if (b != BLOCK_AIR && (mc_block(b)->flags & MCB_TEX_SLOTS)) {
        // a family block: all its outline boxes together
        McBox bx[8];
        int n = shapes_boxes(c, FALSE, bx, 8), k;
        for (i = 0; i < 3; i++) { lo[i] = 16; hi[i] = 0; }
        for (k = 0; k < n; k++) {
            for (i = 0; i < 3; i++) {
                if (bx[k].lo[i] < lo[i]) lo[i] = bx[k].lo[i];
                if (bx[k].hi[i] > hi[i]) hi[i] = bx[k].hi[i] > 16 ? 16 : bx[k].hi[i];
            }
        }
        if (n == 0) { for (i = 0; i < 3; i++) { lo[i] = 0; hi[i] = 16; } }
        return;
    }
    if (b != BLOCK_AIR && (mc_block(b)->flags & MCB_MODEL)) {
        const McBlockModel *m = &mc_block_models[b];
        const McBox *box;
        if (b == BLOCK_REDSTONE_WIRE || b == BLOCK_FIRE) {
            box = &mc_model_boxes[m->state_row];
        } else {
            int st = redstone_model_state(c);
            if (st >= m->count) st = 0;
            box = &mc_model_boxes[m->first + st];
        }
        for (i = 0; i < 3; i++) {
            lo[i] = box->lo[i];
            hi[i] = box->hi[i];
        }
        return;
    }
#endif
    for (i = 0; i < 3; i++) {
        lo[i] = 0;
        hi[i] = 16;
    }
}

// the model state's box (1/16 block) inside a cell, slab test along the ray
// (s, d in blocks). t in 0..1 of the segment, ax = the axis it came in on
static int one_box_hit(const McBox *box, const s32 cell[3], const f32 s[3], const f32 d[3], f32 *t_out, int *ax_out) {
    f32 t0 = 0.0f, t1 = 1.0f;
    int i, ax = 1;
    for (i = 0; i < 3; i++) {
        f32 lo = cell[i] + box->lo[i] / 16.0f, hi = cell[i] + box->hi[i] / 16.0f;
        if (d[i] > -1e-6f && d[i] < 1e-6f) {
            if (s[i] < lo || s[i] > hi) return FALSE;
        } else {
            f32 a = (lo - s[i]) / d[i], bb = (hi - s[i]) / d[i];
            if (a > bb) { f32 tmp = a; a = bb; bb = tmp; }
            if (a > t0) { t0 = a; ax = i; }
            if (bb < t1) t1 = bb;
            if (t0 > t1) return FALSE;
        }
    }
    *t_out = t0;
    *ax_out = ax;
    return TRUE;
}

static int model_box_hit(u16 c, const s32 cell[3], const f32 s[3], const f32 d[3], f32 *t_out, int *ax_out) {
#ifdef MC_HAVE_MODELS
    McBox boxes[8];
    int n = world_cell_boxes(c, !world_pick_mode, boxes, 8), k, found = FALSE;
    for (k = 0; k < n; k++) {
        f32 t;
        int ax;
        if (one_box_hit(&boxes[k], cell, s, d, &t, &ax) && (!found || t < *t_out)) {
            *t_out = t;
            *ax_out = ax;
            found = TRUE;
        }
    }
    return found;
#else
    return FALSE;
#endif
}

// fences and closed fence gates collide 1.5 high: their box reaches into the
// cell above (CrossCollisionBlock collisionHeight 24)
static int tall_below_hit(const s32 cell[3], const f32 s[3], const f32 d[3], f32 *t_out, int *ax_out) {
    s32 below[3];
    u16 c;
    int fam;
    if (world_pick_mode) return FALSE;
    below[0] = cell[0];
    below[1] = cell[1] - 1;
    below[2] = cell[2];
    if (world_get(below[0], below[1], below[2]) == BLOCK_AIR) return FALSE;
    c = world_get_cell(below[0], below[1], below[2]);
    fam = shapes_family(CELL_ID(c));
    if (fam != FAM_FENCE && fam != FAM_GATE) return FALSE;
    return model_box_hit(c, below, s, d, t_out, ax_out);
}

s32 world_raycast(f32 start[3], f32 end[3], VoxelHit *out) {
    f32 s[3], d[3];
    s32 cell[3], step[3];
    f32 t_max[3], t_delta[3];
    s32 i, iter, last_axis = -1;

    out->hit = FALSE;
    if (section_count == 0) {
        return FALSE;
    }

    for (i = 0; i < 3; i++) {
        s[i] = start[i] / MC_BLOCK;
        d[i] = (end[i] - start[i]) / MC_BLOCK;
        cell[i] = world_to_block(start[i]);
        if (d[i] > 0.0f) {
            step[i] = 1;
            t_delta[i] = 1.0f / d[i];
            t_max[i] = ((f32)(cell[i] + 1) - s[i]) / d[i];
        } else if (d[i] < 0.0f) {
            step[i] = -1;
            t_delta[i] = -1.0f / d[i];
            t_max[i] = ((f32)cell[i] - s[i]) / d[i];
        } else {
            step[i] = 0;
            t_delta[i] = 1e30f;
            t_max[i] = 1e30f;
        }
    }

    // the cell the ray starts in is skipped (it's where the ray comes from),
    // unless it's a thin shape the ray starts outside of: standing on a
    // trapdoor, the floor rays start inside its cell, above its box
    if (world_get(cell[0], cell[1], cell[2]) != BLOCK_AIR) {
        u16 c = world_get_cell(cell[0], cell[1], cell[2]);
        const McBlock *mb = mc_block(CELL_ID(c));
        f32 tt;
        int ax;

        if ((mb->flags & MCB_MODEL) && !(mb->flags & MCB_FLUID) && (!(mb->flags & MCB_NO_COLLISION) || world_pick_mode)
            && model_box_hit(c, cell, s, d, &tt, &ax) && tt > 0.0f && tt <= 1.0f) {
            out->hit = TRUE;
            for (i = 0; i < 3; i++) {
                out->cell[i] = cell[i];
                out->normal[i] = 0;
                out->point[i] = start[i] + (end[i] - start[i]) * tt;
            }
            out->normal[ax] = d[ax] > 0.0f ? -1 : 1;
            {
                f32 dx = out->point[0] - start[0], dy = out->point[1] - start[1], dz = out->point[2] - start[2];
                out->dist = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
            }
            return TRUE;
        }
    }

    for (iter = 0; iter < 512; iter++) {
        f32 t;

        // step into the next cell along whichever axis boundary comes first
        if (t_max[0] < t_max[1] && t_max[0] < t_max[2]) {
            last_axis = 0;
        } else if (t_max[1] < t_max[2]) {
            last_axis = 1;
        } else {
            last_axis = 2;
        }
        t = t_max[last_axis];
        if (t > 1.0f) {
            return FALSE; // reached the end of the segment
        }
        cell[last_axis] += step[last_axis];
        t_max[last_axis] += t_delta[last_axis];

        {
            f32 tt;
            int ax;
            if (tall_below_hit(cell, s, d, &tt, &ax)) {
                out->hit = TRUE;
                for (i = 0; i < 3; i++) {
                    out->cell[i] = cell[i];
                    out->normal[i] = 0;
                    out->point[i] = start[i] + (end[i] - start[i]) * tt;
                }
                out->cell[1] -= 1;
                out->normal[ax] = d[ax] > 0.0f ? -1 : 1;
                {
                    f32 dx = out->point[0] - start[0], dy = out->point[1] - start[1], dz = out->point[2] - start[2];
                    out->dist = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
                }
                return TRUE;
            }
        }
        if (world_get(cell[0], cell[1], cell[2]) != BLOCK_AIR) {
            u16 c = world_get_cell(cell[0], cell[1], cell[2]);
            const McBlock *mb = mc_block(CELL_ID(c));
            if (mb->flags & MCB_FLUID) continue; // ClipContext.Fluid.NONE: rays go through fluids
            if (mb->flags & (MCB_MODEL | MCB_NO_COLLISION)) {
                // getShape / getCollisionShape: the state's box (none for
                // noCollision blocks unless aiming at them)
                f32 tt;
                int ax;
                if ((mb->flags & MCB_NO_COLLISION) && !world_pick_mode) continue;
                if (!model_box_hit(c, cell, s, d, &tt, &ax)) continue;
                out->hit = TRUE;
                for (i = 0; i < 3; i++) {
                    out->cell[i] = cell[i];
                    out->normal[i] = 0;
                    out->point[i] = start[i] + (end[i] - start[i]) * tt;
                }
                out->normal[ax] = d[ax] > 0.0f ? -1 : 1;
                {
                    f32 dx = out->point[0] - start[0], dy = out->point[1] - start[1], dz = out->point[2] - start[2];
                    out->dist = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
                }
                return TRUE;
            }
            out->hit = TRUE;
            for (i = 0; i < 3; i++) {
                out->cell[i] = cell[i];
                out->normal[i] = 0;
                out->point[i] = start[i] + (end[i] - start[i]) * t;
            }
            out->normal[last_axis] = -step[last_axis];
            {
                f32 dx = out->point[0] - start[0], dy = out->point[1] - start[1], dz = out->point[2] - start[2];
                out->dist = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
            }
            return TRUE;
        }
    }
    return FALSE;
}

// --- bk collision provider ---
//
// bk keeps a list of collision providers (code_999A0.c). every line query
// (func_80320B98) asks each one; each shortens `end` to its hit so the
// nearest wins. registering the grid here means banjo's floor check, the
// camera, steve's rays and anything else using line queries see blocks.
// the sphere queries (banjo/enemy walls) aren't done yet, so those pass
// through blocks for now.

// callers read flags and unk6 from the returned tri. flags 0 = plain ground.
static BKCollisionTri voxel_tri = { { 0, 0, 0 }, 0, 0 };

static BKCollisionTri *voxel_line_query(f32 start[3], f32 end[3], f32 out_normal[3], u32 flag_filter) {
    VoxelHit hit;
    BKCollisionTri *piece;

    // leftover pieces of carved bk terrain (shortens end if hit)
    piece = terrain_line_query(start, end, out_normal, flag_filter);
    if (!world_raycast(start, end, &hit)) {
        return piece;
    }
    end[0] = hit.point[0];
    end[1] = hit.point[1];
    end[2] = hit.point[2];
    out_normal[0] = (f32)hit.normal[0];
    out_normal[1] = (f32)hit.normal[1];
    out_normal[2] = (f32)hit.normal[2];
    return &voxel_tri;
}

// bk's two sphere queries (banjo's and the actors' own movement uses them,
// not the line query): the grid's solid boxes against a sphere.
// the box overlap adds each box's push-out direction to out_normal, like
// func_802E879C adds the triangle normals
static int voxel_sphere_hits(const f32 pos[3], f32 radius, f32 out_normal[3]) {
    s32 lo[3], hi[3], x, y, z;
    int k, hit = FALSE;

    for (k = 0; k < 3; k++) {
        out_normal[k] = 0.0f;
        lo[k] = world_to_block(pos[k] - radius);
        hi[k] = world_to_block(pos[k] + radius);
    }
    if (world_section_count() == 0) return FALSE;
    for (x = lo[0]; x <= hi[0]; x++) {
        for (y = lo[1] - 1; y <= hi[1]; y++) { // (one lower: fences reach 1.5 up)
            for (z = lo[2]; z <= hi[2]; z++) {
                u16 c;
                const McBlock *mb;
                McBox boxes[8];
                int nb, bi;

                if (world_get(x, y, z) == BLOCK_AIR) continue;
                c = world_get_cell(x, y, z);
                mb = mc_block(CELL_ID(c));
                if (mb->flags & (MCB_FLUID | MCB_NO_COLLISION)) continue;
                nb = world_cell_boxes(c, TRUE, boxes, 8);
                for (bi = 0; bi < nb; bi++) {
                const u8 *blo = boxes[bi].lo, *bhi = boxes[bi].hi;
                f32 mn[3], mx[3], q[3], d[3], d2 = 0.0f;
                mn[0] = x * MC_BLOCK + blo[0] * (MC_BLOCK / 16.0f);
                mn[1] = y * MC_BLOCK + blo[1] * (MC_BLOCK / 16.0f);
                mn[2] = z * MC_BLOCK + blo[2] * (MC_BLOCK / 16.0f);
                mx[0] = x * MC_BLOCK + bhi[0] * (MC_BLOCK / 16.0f);
                mx[1] = y * MC_BLOCK + bhi[1] * (MC_BLOCK / 16.0f);
                mx[2] = z * MC_BLOCK + bhi[2] * (MC_BLOCK / 16.0f);
                for (k = 0; k < 3; k++) {
                    q[k] = pos[k] < mn[k] ? mn[k] : (pos[k] > mx[k] ? mx[k] : pos[k]);
                    d[k] = pos[k] - q[k];
                    d2 += d[k] * d[k];
                }
                if (d2 >= radius * radius) continue;
                hit = TRUE;
                if (d2 > 0.0001f) {
                    f32 l = __builtin_sqrtf(d2);
                    for (k = 0; k < 3; k++) out_normal[k] += d[k] / l;
                } else {
                    // the centre is inside the box: out the nearest face
                    f32 best = 1e9f;
                    int ax = 1, sg = 1;
                    for (k = 0; k < 3; k++) {
                        if (pos[k] - mn[k] < best) { best = pos[k] - mn[k]; ax = k; sg = -1; }
                        if (mx[k] - pos[k] < best) { best = mx[k] - pos[k]; ax = k; sg = 1; }
                    }
                    out_normal[ax] += (f32)sg;
                }
                }
            }
        }
    }
    return hit;
}

// func_80309EB0's kind: is a sphere at pos touching anything
static BKCollisionTri *voxel_sphere_query(f32 pos[3], f32 radius, f32 out_normal[3], u32 flag_filter) {
    return voxel_sphere_hits(pos, radius, out_normal) ? &voxel_tri : NULL;
}

// func_802E8E88's kind: a sphere moving p1 -> p2. if it ends up touching a
// box, p2 goes back to the last free point (iters halvings) and out_normal
// is the push-out direction there
static BKCollisionTri *voxel_sweep_query(f32 p1[3], f32 p2[3], f32 radius, f32 out_normal[3], s32 iters, u32 flag_filter) {
    f32 n[3], delta[3], mid[3], lo_t = 0.0f, hi_t = 1.0f, l;
    int i, k;

    if (!voxel_sphere_hits(p2, radius, n)) return NULL;
    for (k = 0; k < 3; k++) {
        out_normal[k] = n[k];
        delta[k] = p2[k] - p1[k];
        p2[k] = p1[k];
    }
    for (i = 0; i < iters; i++) {
        f32 t = (lo_t + hi_t) * 0.5f;
        for (k = 0; k < 3; k++) mid[k] = p1[k] + delta[k] * t;
        if (voxel_sphere_hits(mid, radius, n)) {
            for (k = 0; k < 3; k++) out_normal[k] = n[k];
            hi_t = t;
        } else {
            for (k = 0; k < 3; k++) p2[k] = mid[k];
            lo_t = t;
        }
    }
    l = __builtin_sqrtf(out_normal[0] * out_normal[0] + out_normal[1] * out_normal[1] + out_normal[2] * out_normal[2]);
    if (l > 0.0f) for (k = 0; k < 3; k++) out_normal[k] /= l;
    return &voxel_tri;
}

void world_register_collision(void) {
    func_80320B24((void *)voxel_line_query, (void *)voxel_sweep_query, (void *)voxel_sphere_query);
}

// a new map loaded: the provider list was just reset (func_80320B84) and the
// map model registered itself (func_8030A078). add the grid after it.
void worldsave_map_leaving(void);
void worldsave_map_entered(s32 map);

RECOMP_HOOK_RETURN("func_8030A078") void world_on_map_model_init(void) {
    worldsave_map_leaving(); // the old map's blocks and holes are kept
    world_clear();
    terrain_reset();
    world_register_collision();
    surv_on_map_load();
    worldsave_map_entered(map_get());
    recomp_printf("[mc] map %d loaded\n", map_get());
}
