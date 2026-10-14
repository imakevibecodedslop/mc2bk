// mining bk's own terrain (swirly's "carve the level" idea, step 9 in CLAUDE.md).
//
// bk's level is one big triangle model (mapModel: a display list + a vertex
// list + a collision list that indexes the same vertices). when a grid cell is
// mined:
//   - render: every level triangle that touches the cell is cut against the
//     cell's box (sutherland-hodgman, one plane at a time). the triangle is
//     removed from bk's display list in memory, and the triangle command is
//     swapped for a call into our own little list that draws the parts outside
//     the cell (then reloads bk's vertex cache). they're drawn right where the
//     original was, inside bk's list, so they get bk's exact textures, 2-cycle
//     blending, fog and matrix.
//   - collision: the same for the collision triangles. removed ones are made
//     zero-size (all corners on one vertex), the leftover parts go into our
//     collision provider (world.c asks terrain_line_query).
//   - the sides of the hole that are inside the ground get dirt/stone block
//     faces, like swirly did, so you don't look into the void.
// everything is undone by the map reloading (the model is loaded fresh), and
// terrain_reset() runs then.
//
// only the opaque map model is carved (not water / glass surfaces).

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "prof.h"
#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "terrain.h"
#include "ores.h"
#include "mc_gbi.h"
#include "pose.h"
#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

BKGfxList *model_getDisplayList(BKModelBin *bin);
BKTextureList *model_getTextureList(BKModelBin *bin);
BKCollisionList *model_getCollisionList(BKModelBin *bin);
BKModelBin *mapModel_getModelBin(s32 which);

// --- data ---

#define MAX_CARVED 16384 // the holes are saved now, so they add up
static s32 carved[MAX_CARVED][3];
static int carved_count;

typedef struct {
    f32 p[3];
    f32 st[2];
    f32 c[4];
} PV;

typedef struct {
    PV v[3];
    s32 cmd; // the display list command the original triangle was in
} RPiece;

typedef struct {
    f32 p[3][3];
    s32 flags;
    s16 unk6;
} CPiece;

// a level triangle command we replaced with a call into our own list
#define MAX_PATCHES 8192
typedef struct {
    s32 cmd;
    u32 w0, w1;       // the original command
    u8 removed;       // bit 0/1: which of its (1 or 2) triangles are gone
    u8 ntri;
    s32 restore[32];  // vertex commands that rebuild bk's vertex cache after us
    int nrestore;
    Gfx *dl;
    int dl_cap;
    Vtx *vtx;
    int vtx_cap;
    u8 dirty;         // its pieces changed: rebuild its list
} Patch;
static Patch patches[MAX_PATCHES];
static int patch_count;
// patches by command index
#define PATCH_HASH 16384 // power of 2, > MAX_PATCHES
static s16 patch_hash[PATCH_HASH]; // index + 1


static RPiece *rpieces;
static int rpiece_count, rpiece_cap;
static CPiece *cpieces;
static int cpiece_count, cpiece_cap;

// faces filling the sides of holes: the cell that is solid, which face, block
typedef struct {
    s32 cell[3];
    u8 face;
    u16 block;
    f32 top_y[4]; // side faces: where the ground surface is above each corner
} FillFace;
#define MAX_FILL 32768 // (8192 ran out after a lot of tnt: holes without walls, the void)
static FillFace fills[MAX_FILL];
static int fill_count;

static int render_dirty;
static int wall_dirty = TRUE; // the hole walls' display list needs a rebuild

// carved cells as a hash set too (the fluids and explosions ask a lot)
#define CARVED_HASH 32768 // power of 2, > MAX_CARVED (entries are s16: MAX_CARVED < 32767)
static s16 carved_hash[CARVED_HASH]; // index + 1 into carved, 0 = empty

static u32 carved_slot(s32 x, s32 y, s32 z) {
    return ((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u) & (CARVED_HASH - 1);
}

int terrain_is_carved(s32 x, s32 y, s32 z) {
    u32 h = carved_slot(x, y, z), i;
    for (i = 0; i < CARVED_HASH; i++) {
        s16 e = carved_hash[(h + i) & (CARVED_HASH - 1)];
        if (e == 0) return FALSE;
        if (carved[e - 1][0] == x && carved[e - 1][1] == y && carved[e - 1][2] == z) return TRUE;
    }
    return FALSE;
}

static void carved_hash_add(int idx) {
    u32 h = carved_slot(carved[idx][0], carved[idx][1], carved[idx][2]), i;
    for (i = 0; i < CARVED_HASH; i++) {
        s16 *e = &carved_hash[(h + i) & (CARVED_HASH - 1)];
        if (*e == 0) {
            *e = (s16)(idx + 1);
            return;
        }
    }
}

// batch carving (explosions): the patches and the hole walls are rebuilt
// once at the end instead of after every cell
static int carve_batch; // nesting depth
static void carve_flush(void);
void world_draw_bury(void *p);
static void mark_patch_dirty(s32 cmd);
static void grids_dirty(void);
static void walls_changed(void);
#define MAX_PENDING 8192
static f32 pending_lo[MAX_PENDING][3], pending_hi[MAX_PENDING][3];
static s32 pending_cell[MAX_PENDING][3];
static int pending_count;
// the pending cells by position, to find the boxes a triangle touches
#define PEND_HASH 16384 // power of 2, > MAX_PENDING
static s16 pend_hash[PEND_HASH]; // index + 1

static u32 pend_slot(s32 x, s32 y, s32 z) {
    return ((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u) & (PEND_HASH - 1);
}

static int pend_find(s32 x, s32 y, s32 z) {
    u32 h = pend_slot(x, y, z), i;
    for (i = 0; i < PEND_HASH; i++) {
        s16 e = pend_hash[(h + i) & (PEND_HASH - 1)];
        if (e == 0) return -1;
        if (pending_cell[e - 1][0] == x && pending_cell[e - 1][1] == y && pending_cell[e - 1][2] == z) return e - 1;
    }
    return -1;
}

static void pend_add(int idx) {
    u32 h = pend_slot(pending_cell[idx][0], pending_cell[idx][1], pending_cell[idx][2]), i;
    for (i = 0; i < PEND_HASH; i++) {
        s16 *e = &pend_hash[(h + i) & (PEND_HASH - 1)];
        if (*e == 0) {
            *e = (s16)(idx + 1);
            return;
        }
    }
}

static void pend_clear(void) {
    int i;
    for (i = 0; i < PEND_HASH; i++) pend_hash[i] = 0;
    pending_count = 0;
}

static void ground_cache_reset(void);
static void shell_reset(void);
static void surface_reset(void);
static int coll_saved_count; // (see CollSaved)
static void level_min_ok_reset(void);
static void fill_cache_reset(void);
static void touch_cache_reset(void);
static int ghost_count; // (see add_ghost)

void terrain_reset(void) {
    carved_count = 0;
    {
        int k;
        for (k = 0; k < CARVED_HASH; k++) carved_hash[k] = 0;
        fill_cache_reset();
        touch_cache_reset();
    }
    ground_cache_reset();
    shell_reset();
    ores_reset();
    surface_reset();
    level_min_ok_reset();
    ghost_count = 0;
    coll_saved_count = 0;
    patch_count = 0; // the map model is loaded fresh, our edits went with it
    {
        int k;
        for (k = 0; k < PATCH_HASH; k++) patch_hash[k] = 0;
    }
    pend_clear();
    grids_dirty();
    rpiece_count = 0;
    cpiece_count = 0;
    fill_count = 0;
    render_dirty = TRUE;
}

static void *grow(void *p, int *cap, int need, int elem) {
    if (need <= *cap) return p;
    {
        int ncap = (*cap == 0) ? 256 : *cap * 2;
        void *np;
        while (ncap < need) ncap *= 2;
        np = recomp_alloc(ncap * elem);
        if (np == NULL) return p;
        if (p != NULL) {
            u8 *d = np, *s = p;
            int i;
            for (i = 0; i < *cap * elem; i++) d[i] = s[i];
            recomp_free(p);
        }
        *cap = ncap;
        return np;
    }
}

// --- a spatial grid over xz (4 x 4 block columns, hashed) ---
// for the things rays test: the collision pieces, the ghosts, the hole walls.
// a ray only looks at the columns it passes; long rays use the whole list.
#define GRID_CELL (4.0f * MC_BLOCK)
#define GRID_HASH 4096 // power of 2
#define GRID_MAX_SPAN 64 // more columns than this: test everything
typedef void (*AabbFn)(int i, f32 mn[3], f32 mx[3]);
typedef struct {
    int dirty;
    int n;                       // items when built
    int start[GRID_HASH + 1];    // per hashed column: items[start .. start+1)
    int *items, items_cap;
    int *big, nbig, big_cap;     // items wider than GRID_MAX_SPAN columns
    u32 *stamp;                  // per item: the query that last saw it
    int stamp_cap;
    u32 query;
    AabbFn aabb;
} Grid;

static int grid_col(f32 v) {
    s32 c = (s32)(v / GRID_CELL);
    if ((f32)c * GRID_CELL > v) c--;
    return c;
}

static u32 grid_slot(s32 cx, s32 cz) {
    return ((u32)cx * 73856093u ^ (u32)cz * 83492791u) & (GRID_HASH - 1);
}

static int grid_cursor[GRID_HASH];

static void grid_build(Grid *g, int n) {
    int i, pass, total = 0;
    g->n = n;
    g->nbig = 0;
    for (i = 0; i <= GRID_HASH; i++) g->start[i] = 0;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            for (i = 0; i < GRID_HASH; i++) {
                g->start[i + 1] += g->start[i];
                grid_cursor[i] = g->start[i];
            }
            total = g->start[GRID_HASH];
            g->items = grow(g->items, &g->items_cap, total + 1, sizeof(int));
            g->nbig = 0;
        }
        for (i = 0; i < n; i++) {
            f32 mn[3], mx[3];
            s32 x0, x1, z0, z1, x, z;
            g->aabb(i, mn, mx);
            x0 = grid_col(mn[0]);
            x1 = grid_col(mx[0]);
            z0 = grid_col(mn[2]);
            z1 = grid_col(mx[2]);
            if ((x1 - x0 + 1) * (z1 - z0 + 1) > GRID_MAX_SPAN) {
                if (pass == 1) {
                    g->big = grow(g->big, &g->big_cap, g->nbig + 1, sizeof(int));
                    if (g->nbig < g->big_cap) g->big[g->nbig++] = i;
                }
                continue;
            }
            for (x = x0; x <= x1; x++) for (z = z0; z <= z1; z++) {
                u32 h = grid_slot(x, z);
                if (pass == 0) g->start[h + 1]++;
                else if (grid_cursor[h] < g->items_cap) g->items[grid_cursor[h]++] = i;
            }
        }
    }
    g->stamp = grow(g->stamp, &g->stamp_cap, n + 1, sizeof(u32));
    for (i = 0; i < g->stamp_cap; i++) g->stamp[i] = 0;
    g->query = 0;
    g->dirty = FALSE;
}

// the items near the segment a -> b into out (each once), or -1: test them all
#define GRID_OUT 8192
static int grid_out[GRID_OUT];

static int grid_query(Grid *g, int n_now, const f32 a[3], const f32 b[3]) {
    s32 x0, x1, z0, z1, x, z;
    int cnt = 0, i;
    if (g->dirty || g->n != n_now) grid_build(g, n_now);
    x0 = grid_col(a[0] < b[0] ? a[0] : b[0]);
    x1 = grid_col(a[0] < b[0] ? b[0] : a[0]);
    z0 = grid_col(a[2] < b[2] ? a[2] : b[2]);
    z1 = grid_col(a[2] < b[2] ? b[2] : a[2]);
    if ((x1 - x0 + 1) * (z1 - z0 + 1) > GRID_MAX_SPAN) return -1;
    if (++g->query == 0) {
        for (i = 0; i < g->stamp_cap; i++) g->stamp[i] = 0;
        g->query = 1;
    }
    for (x = x0; x <= x1; x++) for (z = z0; z <= z1; z++) {
        u32 h = grid_slot(x, z);
        int k;
        for (k = g->start[h]; k < g->start[h + 1]; k++) {
            int it = g->items[k];
            if (it >= g->stamp_cap || g->stamp[it] == g->query) continue;
            g->stamp[it] = g->query;
            if (cnt >= GRID_OUT) return -1;
            grid_out[cnt++] = it;
        }
    }
    for (i = 0; i < g->nbig; i++) {
        if (cnt >= GRID_OUT) return -1;
        grid_out[cnt++] = g->big[i];
    }
    return cnt;
}

static void add_rpiece(const PV *a, const PV *b, const PV *c, s32 cmd) {
    rpieces = grow(rpieces, &rpiece_cap, rpiece_count + 1, sizeof(RPiece));
    if (rpiece_count >= rpiece_cap) return;
    rpieces[rpiece_count].v[0] = *a;
    rpieces[rpiece_count].v[1] = *b;
    rpieces[rpiece_count].v[2] = *c;
    rpieces[rpiece_count].cmd = cmd;
    rpiece_count++;
}

static void add_cpiece(const PV *a, const PV *b, const PV *c, s32 flags, s16 unk6) {
    int k;
    cpieces = grow(cpieces, &cpiece_cap, cpiece_count + 1, sizeof(CPiece));
    if (cpiece_count >= cpiece_cap) return;
    for (k = 0; k < 3; k++) {
        cpieces[cpiece_count].p[0][k] = a->p[k];
        cpieces[cpiece_count].p[1][k] = b->p[k];
        cpieces[cpiece_count].p[2][k] = c->p[k];
    }
    cpieces[cpiece_count].flags = flags;
    cpieces[cpiece_count].unk6 = unk6;
    cpiece_count++;
}

// --- clipping ---

#define MAXPV 24

static void pv_lerp(const PV *a, const PV *b, f32 t, PV *o) {
    int i;
    for (i = 0; i < 3; i++) o->p[i] = a->p[i] + (b->p[i] - a->p[i]) * t;
    for (i = 0; i < 2; i++) o->st[i] = a->st[i] + (b->st[i] - a->st[i]) * t;
    for (i = 0; i < 4; i++) o->c[i] = a->c[i] + (b->c[i] - a->c[i]) * t;
}

// keep the part of the polygon with sign * (p[axis] - val) >= 0
static int clip_plane(const PV *in, int n, int axis, f32 val, f32 sign, PV *out) {
    int i, m = 0;

    for (i = 0; i < n; i++) {
        const PV *a = &in[i], *b = &in[(i + 1) % n];
        f32 da = sign * (a->p[axis] - val), db = sign * (b->p[axis] - val);

        if (da >= 0.0f) {
            if (m < MAXPV) out[m++] = *a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            f32 t = da / (da - db);
            if (m < MAXPV) pv_lerp(a, b, t, &out[m++]);
        }
    }
    return m;
}

typedef void (*EmitTri)(const PV *a, const PV *b, const PV *c, void *ctx);

static f32 poly_area2(const PV *p, int n) {
    // twice the area of a planar polygon (|sum of cross products|)
    f32 s[3] = { 0, 0, 0 };
    int i;
    for (i = 1; i + 1 < n; i++) {
        f32 e1[3], e2[3];
        int k;
        for (k = 0; k < 3; k++) {
            e1[k] = p[i].p[k] - p[0].p[k];
            e2[k] = p[i + 1].p[k] - p[0].p[k];
        }
        s[0] += e1[1] * e2[2] - e1[2] * e2[1];
        s[1] += e1[2] * e2[0] - e1[0] * e2[2];
        s[2] += e1[0] * e2[1] - e1[1] * e2[0];
    }
    return __builtin_sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
}

static void emit_poly(const PV *p, int n, EmitTri emit, void *ctx) {
    int i;
    if (n < 3 || poly_area2(p, n) < 1.0f) return; // drop slivers
    for (i = 1; i + 1 < n; i++) {
        emit(&p[0], &p[i], &p[i + 1], ctx);
    }
}

// the triangle minus the box [lo, hi]: up to 6 convex pieces
static void subtract_box(const PV *tri, const f32 lo[3], const f32 hi[3], EmitTri emit, void *ctx) {
    PV cur[MAXPV], tmp[MAXPV], out[MAXPV];
    int n = 3, axis, side, i;

    for (i = 0; i < 3; i++) cur[i] = tri[i];
    for (axis = 0; axis < 3 && n >= 3; axis++) {
        for (side = 0; side < 2 && n >= 3; side++) {
            f32 val = side ? hi[axis] : lo[axis];
            f32 out_sign = side ? 1.0f : -1.0f; // outside the box on this plane
            int m;

            m = clip_plane(cur, n, axis, val, out_sign, out);
            emit_poly(out, m, emit, ctx);
            n = clip_plane(cur, n, axis, val, -out_sign, tmp);
            for (i = 0; i < n; i++) cur[i] = tmp[i];
        }
    }
    // what's left of cur is inside the box: gone
}

static int tri_touches_box(const PV *t, const f32 lo[3], const f32 hi[3]) {
    int k;
    for (k = 0; k < 3; k++) {
        f32 mn = t[0].p[k], mx = t[0].p[k];
        if (t[1].p[k] < mn) mn = t[1].p[k];
        if (t[2].p[k] < mn) mn = t[2].p[k];
        if (t[1].p[k] > mx) mx = t[1].p[k];
        if (t[2].p[k] > mx) mx = t[2].p[k];
        if (mx <= lo[k] || mn >= hi[k]) return FALSE;
    }
    // aabb overlap is enough to try: the clip finds out if anything is inside
    return TRUE;
}

// --- carving the existing pieces again (a new cell next to an old one) ---

typedef struct { s32 cmd; } RCtx;
static RPiece *tmp_r;
static int tmp_r_count, tmp_r_cap;

static void emit_r_tmp(const PV *a, const PV *b, const PV *c, void *ctx) {
    RCtx *rc = ctx;
    mark_patch_dirty(rc->cmd);
    tmp_r = grow(tmp_r, &tmp_r_cap, tmp_r_count + 1, sizeof(RPiece));
    if (tmp_r_count >= tmp_r_cap) return;
    tmp_r[tmp_r_count].v[0] = *a;
    tmp_r[tmp_r_count].v[1] = *b;
    tmp_r[tmp_r_count].v[2] = *c;
    tmp_r[tmp_r_count].cmd = rc->cmd;
    tmp_r_count++;
}

typedef struct { s32 flags; s16 unk6; } CCtx;
static CPiece *tmp_c;
static int tmp_c_count, tmp_c_cap;

static void emit_c_tmp(const PV *a, const PV *b, const PV *c, void *ctx) {
    CCtx *cc = ctx;
    int k;
    tmp_c = grow(tmp_c, &tmp_c_cap, tmp_c_count + 1, sizeof(CPiece));
    if (tmp_c_count >= tmp_c_cap) return;
    for (k = 0; k < 3; k++) {
        tmp_c[tmp_c_count].p[0][k] = a->p[k];
        tmp_c[tmp_c_count].p[1][k] = b->p[k];
        tmp_c[tmp_c_count].p[2][k] = c->p[k];
    }
    tmp_c[tmp_c_count].flags = cc->flags;
    tmp_c[tmp_c_count].unk6 = cc->unk6;
    tmp_c_count++;
}

// the pending boxes an aabb overlaps: looked up by cell (a small aabb), or
// all of them (a big triangle)
#define MAX_TOUCH 256
static int boxes_for_aabb(const f32 mn[3], const f32 mx[3], int *out, int max) {
    s32 c0[3], c1[3], x, y, z;
    int n = 0, k;

    for (k = 0; k < 3; k++) {
        c0[k] = world_to_block(mn[k] - 1.0f);
        c1[k] = world_to_block(mx[k] + 1.0f);
    }
    if ((c1[0] - c0[0] + 1) * (c1[1] - c0[1] + 1) * (c1[2] - c0[2] + 1) <= 64) {
        for (x = c0[0]; x <= c1[0]; x++) for (y = c0[1]; y <= c1[1]; y++) for (z = c0[2]; z <= c1[2]; z++) {
            int i = pend_find(x, y, z);
            if (i >= 0) {
                for (k = 0; k < 3; k++) {
                    if (mx[k] <= pending_lo[i][k] || mn[k] >= pending_hi[i][k]) break;
                }
                if (k == 3 && n < max) out[n++] = i;
            }
        }
        return n;
    }
    for (x = 0; x < pending_count; x++) {
        for (k = 0; k < 3; k++) {
            if (mx[k] <= pending_lo[x][k] || mn[k] >= pending_hi[x][k]) break;
        }
        if (k == 3 && n < max) out[n++] = x;
    }
    return n;
}

static void tri_aabb(const PV *t, f32 mn[3], f32 mx[3]) {
    int k;
    for (k = 0; k < 3; k++) {
        mn[k] = mx[k] = t[0].p[k];
        if (t[1].p[k] < mn[k]) mn[k] = t[1].p[k];
        if (t[2].p[k] < mn[k]) mn[k] = t[2].p[k];
        if (t[1].p[k] > mx[k]) mx[k] = t[1].p[k];
        if (t[2].p[k] > mx[k]) mx[k] = t[2].p[k];
    }
}

// the triangle minus every pending box it touches, one box after another.
// FALSE (nothing emitted) if it touches none
#define CUT_MAX 8192 // pieces of one triangle (bk's big ground triangles, many boxes)
static PV cut_a[CUT_MAX][3], cut_b[CUT_MAX][3];
static int cut_na, cut_nb;

static void emit_cut(const PV *a, const PV *b, const PV *c, void *ctx) {
    (void)ctx;
    if (cut_nb >= CUT_MAX) return;
    cut_b[cut_nb][0] = *a;
    cut_b[cut_nb][1] = *b;
    cut_b[cut_nb][2] = *c;
    cut_nb++;
}

static int subtract_boxes(const PV *tri, EmitTri emit, void *ctx) {
    f32 mn[3], mx[3];
    int bx[MAX_TOUCH], nbx, b, i;

    tri_aabb(tri, mn, mx);
    nbx = boxes_for_aabb(mn, mx, bx, MAX_TOUCH);
    if (nbx == 0) return FALSE;
    cut_na = 1;
    cut_a[0][0] = tri[0];
    cut_a[0][1] = tri[1];
    cut_a[0][2] = tri[2];
    for (b = 0; b < nbx && cut_na > 0; b++) {
        const f32 *lo = pending_lo[bx[b]], *hi = pending_hi[bx[b]];
        cut_nb = 0;
        for (i = 0; i < cut_na; i++) {
            if (tri_touches_box(cut_a[i], lo, hi)) {
                subtract_box(cut_a[i], lo, hi, emit_cut, NULL);
            } else if (cut_nb < CUT_MAX) {
                cut_b[cut_nb][0] = cut_a[i][0];
                cut_b[cut_nb][1] = cut_a[i][1];
                cut_b[cut_nb][2] = cut_a[i][2];
                cut_nb++;
            }
        }
        for (i = 0; i < cut_nb; i++) {
            cut_a[i][0] = cut_b[i][0];
            cut_a[i][1] = cut_b[i][1];
            cut_a[i][2] = cut_b[i][2];
        }
        cut_na = cut_nb;
    }
    for (i = 0; i < cut_na; i++) emit(&cut_a[i][0], &cut_a[i][1], &cut_a[i][2], ctx);
    return TRUE;
}

// the pieces from earlier digging, cut again by the new boxes: one pass
static void recarve_pending(void) {
    int i;

    tmp_r_count = 0;
    for (i = 0; i < rpiece_count; i++) {
        RCtx rc;
        rc.cmd = rpieces[i].cmd;
        if (subtract_boxes(rpieces[i].v, emit_r_tmp, &rc)) {
            mark_patch_dirty(rc.cmd); // (even if nothing of it is left)
        } else {
            tmp_r = grow(tmp_r, &tmp_r_cap, tmp_r_count + 1, sizeof(RPiece));
            if (tmp_r_count < tmp_r_cap) tmp_r[tmp_r_count++] = rpieces[i];
        }
    }
    rpieces = grow(rpieces, &rpiece_cap, tmp_r_count, sizeof(RPiece));
    for (i = 0; i < tmp_r_count && i < rpiece_cap; i++) rpieces[i] = tmp_r[i];
    rpiece_count = (tmp_r_count < rpiece_cap) ? tmp_r_count : rpiece_cap;

    tmp_c_count = 0;
    for (i = 0; i < cpiece_count; i++) {
        PV t[3];
        CCtx cc;
        int k, j;
        for (j = 0; j < 3; j++) for (k = 0; k < 3; k++) t[j].p[k] = cpieces[i].p[j][k];
        cc.flags = cpieces[i].flags;
        cc.unk6 = cpieces[i].unk6;
        if (!subtract_boxes(t, emit_c_tmp, &cc)) {
            tmp_c = grow(tmp_c, &tmp_c_cap, tmp_c_count + 1, sizeof(CPiece));
            if (tmp_c_count < tmp_c_cap) tmp_c[tmp_c_count++] = cpieces[i];
        }
    }
    cpieces = grow(cpieces, &cpiece_cap, tmp_c_count, sizeof(CPiece));
    for (i = 0; i < tmp_c_count && i < cpiece_cap; i++) cpieces[i] = tmp_c[i];
    cpiece_count = (tmp_c_count < cpiece_cap) ? tmp_c_count : cpiece_cap;
}

// --- the display list ---

#define OP(w0) (((w0) >> 24) & 0xFF)
#define OP_VTX 0x04
#define OP_DL 0x06
#define OP_TRI1 0xBF
#define OP_TRI2 0xB1
#define OP_ENDDL 0xB8
#define OP_TEXTURE 0xBB
#define OP_SETTIMG 0xFD
#define OP_SETTILE 0xF5
#define OP_LOADBLOCK 0xF3
#define OP_LOADTILE 0xF4
#define OP_LOADTLUT 0xF0
#define OP_SETTILESIZE 0xF2
#define OP_LOADSYNC 0xE6
#define OP_PIPESYNC 0xE7
#define OP_TILESYNC 0xE8

// how many Gfx commands the map's display list has (BKGfxList: count, then the list)
static s32 gfx_count(BKModelBin *bin) {
    BKGfxList *gl = model_getDisplayList(bin);
    s32 n = *(s32 *)gl;
    s32 limit = 0x7FFFFFFF;
    s32 offs[6];
    int i;

    // never read past the next part of the model file
    offs[0] = bin->geo_list_offset_4;
    offs[1] = bin->texture_list_offset_8;
    offs[2] = bin->vtx_list_offset_10;
    offs[3] = bin->collision_list_offset_1C;
    offs[4] = bin->animation_list_offset_18;
    offs[5] = bin->effects_list_setup_24;
    for (i = 0; i < 6; i++) {
        if (offs[i] > bin->gfx_list_offset_C && offs[i] < limit) limit = offs[i];
    }
    if (limit != 0x7FFFFFFF) {
        s32 max_n = (limit - bin->gfx_list_offset_C - 8) / 8;
        if (n <= 0 || n > max_n) n = max_n;
    }
    return n;
}

static void vtx_to_pv(const Vtx *v, PV *o) {
    o->p[0] = v->v.ob[0];
    o->p[1] = v->v.ob[1];
    o->p[2] = v->v.ob[2];
    o->st[0] = v->v.tc[0];
    o->st[1] = v->v.tc[1];
    o->c[0] = v->v.cn[0];
    o->c[1] = v->v.cn[1];
    o->c[2] = v->v.cn[2];
    o->c[3] = v->v.cn[3];
}

typedef struct { s32 cmd; } DCtx;
static void emit_r_main(const PV *a, const PV *b, const PV *c, void *ctx) {
    mark_patch_dirty(((DCtx *)ctx)->cmd);
    add_rpiece(a, b, c, ((DCtx *)ctx)->cmd);
}


static Patch *find_patch(s32 cmd);
static void mark_patch_dirty(s32 cmd) {
    Patch *pt = find_patch(cmd);
    if (pt != NULL) pt->dirty = 1;
}

static Patch *find_patch(s32 cmd) {
    u32 h = ((u32)cmd * 2654435761u) & (PATCH_HASH - 1), i;
    for (i = 0; i < PATCH_HASH; i++) {
        s16 e = patch_hash[(h + i) & (PATCH_HASH - 1)];
        if (e == 0) return NULL;
        if (patches[e - 1].cmd == cmd) return &patches[e - 1];
    }
    return NULL;
}

static void patch_hash_add(int idx) {
    u32 h = ((u32)patches[idx].cmd * 2654435761u) & (PATCH_HASH - 1), i;
    for (i = 0; i < PATCH_HASH; i++) {
        s16 *e = &patch_hash[(h + i) & (PATCH_HASH - 1)];
        if (*e == 0) {
            *e = (s16)(idx + 1);
            return;
        }
    }
}

// (re)write one patch's list: the kept triangle of a TRI2 (with bk's cache
// as it is), our pieces, then bk's vertex loads again so the commands after
// this one find the cache like they expect
static void build_patch(Patch *pt, Gfx *list, const int *idx, int npieces) {
    int i, nv, need_gfx, need_vtx, k;
    Gfx *g;

    need_vtx = npieces * 3 + 1;
    need_gfx = 2 + npieces + (npieces * 3 / 30 + 1) * 2 + pt->nrestore + 2;
    // always new buffers: rt64 may still be drawing a frame that calls the
    // old list (bk's level list points at it), so it's never written to
    // again, only freed a few frames later (the graveyard)
    world_draw_bury(pt->vtx);
    world_draw_bury(pt->dl);
    pt->vtx_cap = need_vtx;
    pt->vtx = recomp_alloc(sizeof(Vtx) * pt->vtx_cap);
    pt->dl_cap = need_gfx;
    pt->dl = recomp_alloc(sizeof(Gfx) * pt->dl_cap);
    if (pt->vtx == NULL || pt->dl == NULL) {
        pt->vtx_cap = pt->dl_cap = 0;
        // out of memory: just leave the hole (a no-op). w0 first: a no-op
        // ignores w1, so rt64 never sees a half written command
        ((volatile u32 *)&list[pt->cmd].words.w0)[0] = 0;
        ((volatile u32 *)&list[pt->cmd].words.w1)[0] = 0;
        return;
    }
    g = pt->dl;
    if (pt->ntri == 2 && pt->removed != 3) {
        u32 keep = (pt->removed & 1) ? pt->w1 : pt->w0;
        g->words.w0 = ((u32)OP_TRI1 << 24);
        g->words.w1 = keep & 0x00FFFFFF;
        g++;
    }
    nv = 0;
    for (i = 0; i < npieces; i++) {
        for (k = 0; k < 3; k++) {
            Vtx *o = &pt->vtx[nv++];
            PV *p = &rpieces[idx[i]].v[k];
            o->v.ob[0] = (s16)p->p[0];
            o->v.ob[1] = (s16)p->p[1];
            o->v.ob[2] = (s16)p->p[2];
            o->v.flag = 0;
            o->v.tc[0] = (s16)p->st[0];
            o->v.tc[1] = (s16)p->st[1];
            o->v.cn[0] = (u8)p->c[0];
            o->v.cn[1] = (u8)p->c[1];
            o->v.cn[2] = (u8)p->c[2];
            o->v.cn[3] = (u8)p->c[3];
        }
    }
    for (i = 0; i < nv; i += 30) {
        int cnt = nv - i, t;
        if (cnt > 30) cnt = 30;
        gSPVertex(g++, &pt->vtx[i], cnt, 0);
        for (t = 0; t + 2 < cnt; t += 3) {
            gSP1Triangle(g++, t, t + 1, t + 2, 0);
        }
    }
    if (nv > 0) {
        for (i = 0; i < pt->nrestore; i++) {
            *g++ = list[pt->restore[i]];
        }
    }
    gSPEndDisplayList(g++);
    // the triangle command becomes "call our list" (G_DL, push)
    // bk's level list is shared by every frame and rt64 reads it on its own
    // thread: write the address first, then the opcode. read in between, the
    // old triangle command with a new w1 only draws a wrong triangle; the old
    // order (opcode first) could make rt64 call the triangle's vertex indices
    // as a display list address and crash
    if (OP(list[pt->cmd].words.w0) != OP_DL) {
        ((volatile u32 *)&list[pt->cmd].words.w0)[0] = 0; // G_SPNOOP while w1 changes
    }
    ((volatile u32 *)&list[pt->cmd].words.w1)[0] = (u32)pt->dl;
    ((volatile u32 *)&list[pt->cmd].words.w0)[0] = (u32)0x06 << 24;
}

static void carve_display_list(void) {
    BKModelBin *bin = mapModel_getModelBin(0);
    Gfx *list;
    BKVertexList *vl;
    s32 n, i;
    int cache[64];
    s32 cache_cmd[64]; // the vertex command that loaded each slot

    if (bin == NULL) return;
    list = model_getDisplayList(bin)->list;
    vl = model_getVtxList(bin);
    n = gfx_count(bin);
    for (i = 0; i < 64; i++) {
        cache[i] = -1;
        cache_cmd[i] = -1;
    }

    for (i = 0; i < n; i++) {
        u32 w0 = list[i].words.w0, w1 = list[i].words.w1;
        u32 op = OP(w0);
        Patch *pt = NULL;

        if (op == 0x06 && (pt = find_patch(i)) != NULL) {
            // one of ours: look at the original triangle command
            w0 = pt->w0;
            w1 = pt->w1;
            op = OP(w0);
        }
        if (op == OP_VTX) {
            // F3DEX: v0 * 2 in bits 16-23, count in bits 10-15, segment 1 = the vertex list
            s32 v0 = ((w0 >> 16) & 0xFF) / 2;
            s32 cnt = (w0 >> 10) & 0x3F;
            s32 base;
            int k;

            for (k = 0; k < cnt && v0 + k < 64; k++) cache_cmd[v0 + k] = i;
            if (((w1 >> 24) & 0x0F) != 1) {
                for (k = 0; k < cnt && v0 + k < 64; k++) cache[v0 + k] = -1;
                continue;
            }
            base = (w1 & 0x00FFFFFF) / sizeof(Vtx);
            for (k = 0; k < cnt && v0 + k < 64; k++) {
                cache[v0 + k] = (base + k < vl->count) ? base + k : -1;
            }
            continue;
        }
        if (op == OP_TRI1 || op == OP_TRI2) {
            u32 words[2];
            int t, ntri = (op == OP_TRI2) ? 2 : 1;
            int removed = pt ? pt->removed : 0, newly = 0;

            words[0] = (op == OP_TRI2) ? w0 : w1;
            words[1] = w1;
            for (t = 0; t < ntri; t++) {
                u32 tw = words[t];
                int ia, ib, ic;
                PV tri[3];
                DCtx dc;

                if (removed & (1 << t)) continue; // already in pieces
                ia = cache[((tw >> 16) & 0xFF) / 2 & 63];
                ib = cache[((tw >> 8) & 0xFF) / 2 & 63];
                ic = cache[(tw & 0xFF) / 2 & 63];
                if (ia < 0 || ib < 0 || ic < 0) continue;
                vtx_to_pv(&vl->vtx_18[ia], &tri[0]);
                vtx_to_pv(&vl->vtx_18[ib], &tri[1]);
                vtx_to_pv(&vl->vtx_18[ic], &tri[2]);
                dc.cmd = i;
                if (!subtract_boxes(tri, emit_r_main, &dc)) continue; // every pending box it touches
                newly |= 1 << t;
            }
            if (newly == 0) continue;
            if (pt == NULL) {
                int k, j;
                if (patch_count >= MAX_PATCHES) continue;
                pt = &patches[patch_count];
                pt->cmd = i;
                patch_hash_add(patch_count);
                patch_count++;
                pt->w0 = w0;
                pt->w1 = w1;
                pt->ntri = ntri;
                pt->removed = 0;
                pt->dl = NULL;
                pt->vtx = NULL;
                pt->dl_cap = pt->vtx_cap = 0;
                // bk's vertex cache right now, as the commands that built it,
                // oldest first so replaying them gives the same slots
                pt->nrestore = 0;
                for (k = 0; k < 64; k++) {
                    s32 c = cache_cmd[k];
                    int dup = FALSE;
                    if (c < 0) continue;
                    for (j = 0; j < pt->nrestore; j++) if (pt->restore[j] == c) dup = TRUE;
                    if (!dup && pt->nrestore < 32) pt->restore[pt->nrestore++] = c;
                }
                for (k = 1; k < pt->nrestore; k++) {
                    for (j = k; j > 0 && pt->restore[j - 1] > pt->restore[j]; j--) {
                        s32 tmp = pt->restore[j];
                        pt->restore[j] = pt->restore[j - 1];
                        pt->restore[j - 1] = tmp;
                    }
                }
            }
            pt->removed |= newly;
            pt->dirty = 1;
            continue;
        }
    }
    render_dirty = TRUE;
}

// every patch whose pieces may have changed (new ones, and older ones
// whose pieces recarve_pieces just cut again) gets its list rebuilt
static int patch_first[MAX_PATCHES + 1], patch_fill[MAX_PATCHES];
static int *piece_order;
static int piece_order_cap;

static void rebuild_patches(void) {
    BKModelBin *bin = mapModel_getModelBin(0);
    Gfx *list;
    int i;
    if (bin == NULL) return;
    list = model_getDisplayList(bin)->list;
    // the pieces grouped by patch (counting sort), so each patch only looks
    // at its own pieces
    for (i = 0; i <= patch_count; i++) patch_first[i] = 0;
    for (i = 0; i < rpiece_count; i++) {
        Patch *pt = find_patch(rpieces[i].cmd);
        if (pt != NULL) patch_first[(pt - patches) + 1]++;
    }
    for (i = 0; i < patch_count; i++) {
        patch_first[i + 1] += patch_first[i];
        patch_fill[i] = patch_first[i];
    }
    piece_order = grow(piece_order, &piece_order_cap, rpiece_count + 1, sizeof(int));
    for (i = 0; i < rpiece_count; i++) {
        Patch *pt = find_patch(rpieces[i].cmd);
        if (pt != NULL && piece_order_cap > patch_fill[pt - patches]) piece_order[patch_fill[pt - patches]++] = i;
    }
    for (i = 0; i < patch_count; i++) {
        if (!patches[i].dirty) continue; // untouched: its list stays as it is
        patches[i].dirty = 0;
        build_patch(&patches[i], list, &piece_order[patch_first[i]], patch_first[i + 1] - patch_first[i]);
    }
    render_dirty = TRUE;
}

// --- collision list ---

typedef struct { s32 flags; s16 unk6; } CCtx2;
static void emit_c_main(const PV *a, const PV *b, const PV *c, void *ctx) {
    CCtx2 *cc = ctx;
    add_cpiece(a, b, c, cc->flags, cc->unk6);
}

// the level's collision triangles as they were before any carving ("ghosts").
// the underground test asks the original ground, so holes dug earlier can't
// make the cells under or beside them look like open air.
typedef struct { f32 p[3][3]; } Ghost;
static Ghost *ghosts;
static int ghost_count, ghost_cap;

static void ghost_aabb(int i, f32 mn[3], f32 mx[3]) {
    int k;
    for (k = 0; k < 3; k++) {
        mn[k] = mx[k] = ghosts[i].p[0][k];
        if (ghosts[i].p[1][k] < mn[k]) mn[k] = ghosts[i].p[1][k];
        if (ghosts[i].p[2][k] < mn[k]) mn[k] = ghosts[i].p[2][k];
        if (ghosts[i].p[1][k] > mx[k]) mx[k] = ghosts[i].p[1][k];
        if (ghosts[i].p[2][k] > mx[k]) mx[k] = ghosts[i].p[2][k];
    }
}
static Grid ghost_grid = { TRUE, 0, { 0 }, NULL, 0, NULL, 0, 0, NULL, 0, 0, ghost_aabb };

static void add_ghost(const PV *tri) {
    int k;
    ghosts = grow(ghosts, &ghost_cap, ghost_count + 1, sizeof(Ghost));
    if (ghost_count >= ghost_cap) return;
    for (k = 0; k < 3; k++) {
        ghosts[ghost_count].p[k][0] = tri[k].p[0];
        ghosts[ghost_count].p[k][1] = tri[k].p[1];
        ghosts[ghost_count].p[k][2] = tri[k].p[2];
    }
    ghost_count++;
}

// nearest ghost hit on start->end: t in 0..1 and the normal, or FALSE
static int ghost_ray(const f32 start[3], const f32 end[3], f32 *out_t, f32 out_n[3]) {
    f32 d[3], best = 2.0f;
    int i, k, found = FALSE;

    int gq, gn, j;
    for (k = 0; k < 3; k++) d[k] = end[k] - start[k];
    gq = grid_query(&ghost_grid, ghost_count, start, end);
    gn = gq < 0 ? ghost_count : gq;
    for (j = 0; j < gn; j++) {
        Ghost *g;
        i = gq < 0 ? j : grid_out[j];
        g = &ghosts[i];
        f32 e1[3], e2[3], pv[3], tv[3], qv[3], det, inv, u, v, t;

        for (k = 0; k < 3; k++) {
            e1[k] = g->p[1][k] - g->p[0][k];
            e2[k] = g->p[2][k] - g->p[0][k];
        }
        pv[0] = d[1] * e2[2] - d[2] * e2[1];
        pv[1] = d[2] * e2[0] - d[0] * e2[2];
        pv[2] = d[0] * e2[1] - d[1] * e2[0];
        det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
        if (det > -1e-6f && det < 1e-6f) continue;
        inv = 1.0f / det;
        for (k = 0; k < 3; k++) tv[k] = start[k] - g->p[0][k];
        u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        qv[0] = tv[1] * e1[2] - tv[2] * e1[1];
        qv[1] = tv[2] * e1[0] - tv[0] * e1[2];
        qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
        v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
        if (t < 0.0f || t >= best) continue;
        best = t;
        found = TRUE;
        // bk's stored winding: normal = e1 x e2 (same as the level's faces)
        out_n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        out_n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        out_n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    }
    if (found) {
        f32 l = __builtin_sqrtf(out_n[0] * out_n[0] + out_n[1] * out_n[1] + out_n[2] * out_n[2]);
        if (l > 0.0f) for (k = 0; k < 3; k++) out_n[k] /= l;
        *out_t = best;
    }
    return found;
}

// the original ground along start->end: the level that's left, or a ghost
static int ground_ray(f32 start[3], f32 end[3], f32 hit[3], f32 n[3]) {
    f32 gt, gn[3], lh[3], ln[3];
    int lev = steve_raycast_level(start, end, lh, ln);
    int gh = ghost_ray(start, end, &gt, gn);
    int k;

    if (gh) {
        f32 gp[3];
        for (k = 0; k < 3; k++) gp[k] = start[k] + (end[k] - start[k]) * gt;
        if (!lev || (gp[0] - start[0]) * (gp[0] - start[0]) + (gp[1] - start[1]) * (gp[1] - start[1]) + (gp[2] - start[2]) * (gp[2] - start[2])
                < (lh[0] - start[0]) * (lh[0] - start[0]) + (lh[1] - start[1]) * (lh[1] - start[1]) + (lh[2] - start[2]) * (lh[2] - start[2])) {
            for (k = 0; k < 3; k++) {
                hit[k] = gp[k];
                n[k] = gn[k];
            }
            return TRUE;
        }
    }
    if (lev) {
        for (k = 0; k < 3; k++) {
            hit[k] = lh[k];
            n[k] = ln[k];
        }
    }
    return lev;
}

// the collision triangles carve_collision shrank to nothing, as they were
typedef struct {
    s32 tri;
    s16 v1, v2;
} CollSaved;
static CollSaved *coll_saved;
static int coll_saved_count, coll_saved_cap;

static void carve_collision(void) {
    BKModelBin *bin = mapModel_getModelBin(0);
    BKCollisionList *cl;
    BKVertexList *vl;
    BKCollisionTri *tris;
    s32 i;

    if (bin == NULL) return;
    cl = model_getCollisionList(bin);
    if (cl == NULL) return;
    vl = model_getVtxList(bin);
    tris = (BKCollisionTri *)((BKCollisionGeo *)(cl + 1) + cl->unk10);
    for (i = 0; i < cl->unk14; i++) {
        BKCollisionTri *t = &tris[i];
        PV tri[3];
        int k;
        CCtx2 cc;

        if (t->unk0[0] == t->unk0[1] && t->unk0[1] == t->unk0[2]) continue; // already removed
        for (k = 0; k < 3; k++) {
            Vtx *v = &vl->vtx_18[t->unk0[k]];
            tri[k].p[0] = v->v.ob[0];
            tri[k].p[1] = v->v.ob[1];
            tri[k].p[2] = v->v.ob[2];
        }
        cc.flags = t->flags;
        cc.unk6 = t->unk6;
        if (!subtract_boxes(tri, emit_c_main, &cc)) continue;
        add_ghost(tri);
        // (remembered, so terrain_uncarve can put it back)
        coll_saved = grow(coll_saved, &coll_saved_cap, coll_saved_count + 1, sizeof(CollSaved));
        if (coll_saved_count < coll_saved_cap) {
            coll_saved[coll_saved_count].tri = i;
            coll_saved[coll_saved_count].v1 = t->unk0[1];
            coll_saved[coll_saved_count].v2 = t->unk0[2];
            coll_saved_count++;
        }
        // zero size: bk's line/sphere tests can't hit it any more
        t->unk0[1] = t->unk0[0];
        t->unk0[2] = t->unk0[0];
    }
}

// --- the sides of holes ---

// TRUE if the cell's center is under the ground: there's an upward facing
// surface above it (casting down from high above)
// is this cell under bk's ground? bk's line test is two-sided, so cast up
// from the cell's center: if the first bit of *level* (not objects like tree
// canopies) above it is a floor facing up, we're looking at the underside
// of the ground we're buried in. depth = how far below that surface.
//
// the answer is remembered per cell, and asked before a cell is carved: once
// a hole is cut the ray would go straight up through it. digging down, a cell
// under a known underground cell is underground too (one block deeper).
#define GROUND_CACHE 65536 // power of 2
#define GROUND_PROBE 32     // a full neighbourhood evicts instead of scanning the table
typedef struct {
    s32 x, y, z;
    f32 depth;      // < 0: not underground
    u8 used;
    s8 floor_below; // the nearest level under the cell's middle is a floor: 1, else 0. -1 = not asked yet
} GroundCell;
static GroundCell ground_cache[GROUND_CACHE];

static GroundCell *ground_slot(s32 x, s32 y, s32 z, int create) {
    u32 h = ((u32)x * 73856093u) ^ ((u32)y * 19349663u) ^ ((u32)z * 83492791u);
    int i;

    for (i = 0; i < GROUND_PROBE; i++) {
        GroundCell *g = &ground_cache[(h + i) & (GROUND_CACHE - 1)];
        if (!g->used) {
            if (!create) return NULL;
            g->used = TRUE;
            g->x = x;
            g->y = y;
            g->z = z;
            g->floor_below = -1;
            return g;
        }
        if (g->x == x && g->y == y && g->z == z) return g;
    }
    if (!create) return NULL;
    // all taken: reuse one of them (a pseudo random one, so hot cells survive)
    {
        GroundCell *g = &ground_cache[(h + (x ^ z ^ y) % GROUND_PROBE) & (GROUND_CACHE - 1)];
        g->used = TRUE;
        g->x = x;
        g->y = y;
        g->z = z;
        g->floor_below = -1;
        return g;
    }
}

static void ground_cache_reset(void) {
    int i;
    for (i = 0; i < GROUND_CACHE; i++) ground_cache[i].used = FALSE;
}

// short ray: only ground right above the cell counts. a long ray beside a
// ledge found some floor far overhead and filled the air with dirt walls.
// deeper cells count by stacking: under a buried cell = one block deeper.
#define GROUND_RAY (2.0f * MC_BLOCK)

// ground surface within two blocks under the cell's middle: the cell is
// above the ground there
static int ground_just_below(s32 x, s32 y, s32 z) {
    f32 a[3], b[3], hit[3], n[3];
    a[0] = b[0] = (x + 0.5f) * MC_BLOCK;
    a[2] = b[2] = (z + 0.5f) * MC_BLOCK;
    a[1] = (y + 0.5f) * MC_BLOCK;
    b[1] = a[1] - GROUND_RAY;
    return ground_ray(a, b, hit, n) && n[1] > 0.05f;
}

// cells found solid by cell_under_shell (past a wall, under the level's
// body): from then on they're underground like any other, so they can be
// mined and blown up, and the walls go on around the holes dug into them.
// kept per map; the carved ones are saved with their depth (worldsave.c), the
// others come back when the walls are worked out again.
#define SHELL_HASH 16384 // power of 2
typedef struct {
    s32 x, y, z;
    f32 depth;
    u8 used;
    u8 edge; // past the level's walls (cell_under_shell): bedrock, the end of the world
} ShellCell;
static ShellCell shell_cells[SHELL_HASH];

static ShellCell *shell_slot(s32 x, s32 y, s32 z, int create) {
    u32 h = ((u32)x * 73856093u) ^ ((u32)y * 19349663u) ^ ((u32)z * 83492791u);
    int i;
    for (i = 0; i < 64; i++) {
        ShellCell *c = &shell_cells[(h + i) & (SHELL_HASH - 1)];
        if (!c->used) {
            if (!create) return NULL;
            c->used = TRUE;
            c->x = x;
            c->y = y;
            c->z = z;
            return c;
        }
        if (c->x == x && c->y == y && c->z == z) return c;
    }
    return NULL;
}

static void shell_reset(void) {
    int i;
    for (i = 0; i < SHELL_HASH; i++) shell_cells[i].used = FALSE;
}

static void shell_put(s32 x, s32 y, s32 z, f32 depth, int edge) {
    ShellCell *c = shell_slot(x, y, z, TRUE);
    if (c != NULL) {
        c->depth = depth;
        c->edge = (u8)edge;
    }
}

void terrain_shell_restore(s32 x, s32 y, s32 z, f32 depth) {
    shell_put(x, y, z, depth, FALSE);
}

// the end of the world past the level's walls is bedrock
static int cell_is_edge(s32 x, s32 y, s32 z) {
    ShellCell *c = shell_slot(x, y, z, FALSE);
    return c != NULL && c->edge;
}

int terrain_carved_shell(int i, f32 *depth) {
    ShellCell *c = shell_slot(carved[i][0], carved[i][1], carved[i][2], FALSE);
    if (c == NULL) return FALSE;
    *depth = c->depth;
    return TRUE;
}

// how many cells up the stacking may climb, and whether the current climb
// ran out of them. a miss that only happened because the climb ran out isn't
// remembered for the cells on the way (it used to mark whole columns of real
// underground cells "not ground" after one deep query: holes without walls)
#define GROUND_CLIMB 24
static int ground_exhausted;
static f32 level_max_y_get(void);

static int cell_inside_ground_r(s32 x, s32 y, s32 z, f32 *depth, int levels) {
    GroundCell *g;
    ShellCell *sc = shell_slot(x, y, z, FALSE);
    f32 c[3], top[3], hit[3], n[3], d = -1.0f;

    if (sc != NULL) {
        if (depth) *depth = sc->depth;
        return TRUE;
    }
    g = ground_slot(x, y, z, FALSE);

    if (g == NULL) {
        // from just above the cell's bottom, so a cell that's only partly in
        // the ground (slopes) still finds the surface. depth is measured from
        // the center: -0.5..0 = partly buried
        c[0] = (x + 0.5f) * MC_BLOCK;
        c[1] = y * MC_BLOCK + 1.0f;
        c[2] = (z + 0.5f) * MC_BLOCK;
        top[0] = c[0];
        top[1] = c[1] + GROUND_RAY;
        top[2] = c[2];
        int got = ground_ray(c, top, hit, n);
        if (got && n[1] < -0.05f) {
            // the first thing over the cell is an underside (a ledge or a
            // thin platform over open air). over the whole cell: the cell
            // is in the open under it. inside the cell: the cell's top part
            // is in the ledge, so look on up from just over that underside
            if (hit[1] >= (y + 1) * MC_BLOCK - 1.0f) {
                got = FALSE;
                d = -1.0f;
                goto ground_known;
            }
            {
                f32 c2[3] = { c[0], hit[1] + 1.0f, c[2] };
                got = ground_ray(c2, top, hit, n);
            }
        }
        if (got && n[1] > 0.05f) {
            d = (hit[1] - (y + 0.5f) * MC_BLOCK) / MC_BLOCK;
        } else if ((y + 1) * MC_BLOCK > level_max_y_get()) {
            // above the whole level: no ground over it, for sure
        } else if (!ground_just_below(x, y, z)) {
            // (a cell standing on the ground is above it: no need to climb
            // looking for a surface far overhead)
            f32 above;
            if (levels <= 0) {
                ground_exhausted = TRUE;
            } else if (cell_inside_ground_r(x, y + 1, z, &above, levels - 1) && above >= 0.5f) {
                d = above + 1.0f;
            }
        }
ground_known:
        if (ground_exhausted && levels < GROUND_CLIMB && d < 0.0f) {
            // (only the cell that was asked about keeps the guess)
            if (depth) *depth = d;
            return FALSE;
        }
        g = ground_slot(x, y, z, TRUE);
        if (g == NULL) {
            if (depth) *depth = d;
            return d >= 0.0f;
        }
        g->depth = d;
    }
    if (depth) *depth = g->depth;
    return g->depth >= 0.0f;
}

static int cell_inside_ground(s32 x, s32 y, s32 z, f32 *depth) {
    ground_exhausted = FALSE;
    return cell_inside_ground_r(x, y, z, depth, GROUND_CLIMB);
}

// any part of the cell under the ground (its depth from the center > -0.5)
static int cell_partly_in_ground(s32 x, s32 y, s32 z, f32 *depth) {
    f32 d;
    cell_inside_ground(x, y, z, &d);
    if (depth) *depth = d;
    return d > -0.5f;
}

// is the cell inside the level's solid body, though no floor is over it?
// bk's levels are shells: past a room's wall, under its floor, there's no
// surface above to find (the floor stops at the wall). a cell there, below
// the ground surface of the dug cell next to it, is solid unless the first
// thing under it is a floor (then it's a real air space: a room under a
// ledge). the void under the level, or the underside of a lower ceiling, is
// solid ground.
static int cell_under_shell(s32 x, s32 y, s32 z, f32 surface_y) {
    GroundCell *g;
    int floor;

    if ((y + 0.5f) * MC_BLOCK >= surface_y) return FALSE;
    g = ground_slot(x, y, z, FALSE);
    if (g != NULL && g->floor_below >= 0) {
        floor = g->floor_below;
    } else {
        f32 a[3], b[3], hit[3], n[3];
        a[0] = b[0] = (x + 0.5f) * MC_BLOCK;
        a[2] = b[2] = (z + 0.5f) * MC_BLOCK;
        a[1] = (y + 0.5f) * MC_BLOCK;
        b[1] = terrain_level_min_y() - MC_BLOCK;
        if (b[1] > a[1] - MC_BLOCK) b[1] = a[1] - MC_BLOCK;
        floor = ground_ray(a, b, hit, n) && n[1] > 0.05f;
        if (g == NULL) {
            cell_inside_ground(x, y, z, NULL); // (makes the cell's entry)
            g = ground_slot(x, y, z, FALSE);
        }
        if (g != NULL) g->floor_below = (s8)floor;
    }
    return !floor;
}

// open air under a ledge: the first thing over the cell's middle (within a
// block) is the underside of something
static int cell_under_overhang(s32 x, s32 y, s32 z) {
    f32 a[3], b[3], hit[3], n[3];
    a[0] = b[0] = (x + 0.5f) * MC_BLOCK;
    a[2] = b[2] = (z + 0.5f) * MC_BLOCK;
    a[1] = (y + 0.5f) * MC_BLOCK;
    b[1] = a[1] + MC_BLOCK;
    return ground_ray(a, b, hit, n) && n[1] < -0.05f;
}

// the ground surface's height above a point (level only), or `fallback`
static f32 surface_y_at(f32 x, f32 y_from, f32 z, f32 fallback) {
    f32 a[3], b[3], hit[3], n[3];
    a[0] = b[0] = x;
    a[2] = b[2] = z;
    a[1] = y_from;
    b[1] = y_from + 3.0f * MC_BLOCK;
    if (ground_ray(a, b, hit, n) && n[1] > 0.05f) return hit[1];
    return fallback;
}

int terrain_cell_in_ground(s32 x, s32 y, s32 z) {
    return cell_partly_in_ground(x, y, z, NULL);
}

// --- what bk's ground looks like ---
// the level's own floor triangles, with the colour you see on them (their
// texture's average colour times their vertex colour), so dug ground can be
// the block it looks like: green grass, grey rock, tan sand, white snow...
// built from the level's display list the first time it's needed.

enum { SM_UNKNOWN, SM_GRASS, SM_DIRT, SM_SAND, SM_SNOW, SM_STONE, SM_ICE, SM_TERRACOTTA, SM_WOOD, SM_MUD, SM_METAL };

// bk's footstep surface for a collision triangle's flags (code_9D640.c:
// the per map table, or the direct one when bit 31 is set). from the levels'
// tables: 1 ground, 2 sand, 3 snow, 4 shallow water, 5 wood, 6 stone,
// 7 rustling (leaves, dry grass), 8 mud, 9 thud, 10 metal
s32 func_803246B4(s32 map_id, s32 arg1);

typedef struct {
    s16 v[3][3];
    u8 mat;
    u8 val;  // display tris: brightness 0..255
    u8 coll; // 1: a collision triangle (mat = its footstep surface)
} SurfTri;
static SurfTri *surf_tris;
static int surf_count, surf_cap, surf_built;
#define SURF_BUCKETS 4096 // power of 2
#define SURF_CELL (4.0f * MC_BLOCK)
static int surf_first[SURF_BUCKETS + 1];
static int *surf_list;
static int surf_list_cap;

#define SURF_TEX_MAX 512
static u8 surf_tex_rgb[SURF_TEX_MAX][3];
static u8 surf_tex_ok[SURF_TEX_MAX]; // 0 not decoded yet, 1 ok, 2 no colour

static void surface_reset(void) {
    surf_count = 0;
    surf_built = FALSE;
}

static u32 surf_bucket(s32 bx, s32 bz) {
    return ((u32)bx * 73856093u ^ (u32)bz * 83492791u) & (SURF_BUCKETS - 1);
}

static s32 surf_col(f32 v) {
    s32 i = (s32)(v / SURF_CELL);
    return ((f32)i * SURF_CELL > v) ? i - 1 : i;
}

// RGBA16 5551
static void rgba16(u16 p, u32 *r, u32 *g, u32 *b, u32 *a) {
    *r = ((p >> 11) & 31) * 255 / 31;
    *g = ((p >> 6) & 31) * 255 / 31;
    *b = ((p >> 1) & 31) * 255 / 31;
    *a = p & 1;
}

// a texture's average colour over its opaque texels (bk's texture types:
// 1 CI4, 2 CI8, 4 RGBA16, 8 RGBA32; the palette comes first for CI)
static int surf_tex_colour(BKTextureList *tl, int i, u8 out[3]) {
    BKTextureHeader *h = &tl->tex_8[i];
    u8 *data = (u8 *)&tl->tex_8[tl->cnt_4] + h->offset_0;
    u32 n = (u32)h->width_8 * h->height_9, k, sr = 0, sg = 0, sb = 0, cnt = 0, r, g, b, a;

    if (n == 0) return FALSE;
    if (h->type_4 & 1) {
        u16 *pal = (u16 *)data;
        u8 *px = data + 0x20;
        for (k = 0; k < n; k++) {
            u8 idx = (k & 1) ? (px[k >> 1] & 15) : (px[k >> 1] >> 4);
            rgba16(pal[idx], &r, &g, &b, &a);
            if (a) { sr += r; sg += g; sb += b; cnt++; }
        }
    } else if (h->type_4 & 2) {
        u16 *pal = (u16 *)data;
        u8 *px = data + 0x200;
        for (k = 0; k < n; k++) {
            rgba16(pal[px[k]], &r, &g, &b, &a);
            if (a) { sr += r; sg += g; sb += b; cnt++; }
        }
    } else if (h->type_4 & 4) {
        u16 *px = (u16 *)data;
        for (k = 0; k < n; k++) {
            rgba16(px[k], &r, &g, &b, &a);
            if (a) { sr += r; sg += g; sb += b; cnt++; }
        }
    } else if (h->type_4 & 8) {
        u32 *px = (u32 *)data;
        for (k = 0; k < n; k++) {
            u32 p = px[k];
            if ((p & 0xFF) >= 0x80) { sr += p >> 24; sg += (p >> 16) & 0xFF; sb += (p >> 8) & 0xFF; cnt++; }
        }
    } else {
        return FALSE; // (intensity textures: no colour of their own)
    }
    if (cnt == 0) return FALSE;
    out[0] = (u8)(sr / cnt);
    out[1] = (u8)(sg / cnt);
    out[2] = (u8)(sb / cnt);
    return TRUE;
}

// the block family a colour looks like
static u8 surf_classify(u32 r, u32 g, u32 b) {
    u32 mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    u32 mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    f32 v = mx / 255.0f, sat = mx ? (f32)(mx - mn) / mx : 0.0f, hue;

    if (v < 0.12f) return SM_UNKNOWN; // too dark to tell (shadowed)
    if (sat < 0.18f) return (v > 0.78f) ? SM_SNOW : SM_STONE;
    if (mx == r) hue = 60.0f * ((f32)((s32)g - (s32)b) / (mx - mn));
    else if (mx == g) hue = 60.0f * ((f32)((s32)b - (s32)r) / (mx - mn)) + 120.0f;
    else hue = 60.0f * ((f32)((s32)r - (s32)g) / (mx - mn)) + 240.0f;
    if (hue < 0.0f) hue += 360.0f;
    if (hue >= 70.0f && hue < 170.0f) return SM_GRASS;
    if (hue >= 170.0f && hue < 260.0f) return (v > 0.6f) ? SM_ICE : SM_STONE;
    if (hue >= 260.0f && hue < 330.0f) return SM_STONE;
    if (hue >= 40.0f) return (v > 0.5f) ? SM_SAND : SM_GRASS; // yellow: sand, olive: grass
    if (hue >= 20.0f) return (v > 0.62f && sat < 0.55f) ? SM_SAND : SM_DIRT;
    return (sat > 0.45f) ? SM_TERRACOTTA : SM_DIRT;
}

static void surf_add(const Vtx *a, const Vtx *b, const Vtx *c, int tex, BKTextureList *tl) {
    f32 e1[3], e2[3], ny, nl;
    u32 r, g, bl;
    SurfTri *t;
    int k;

    for (k = 0; k < 3; k++) {
        e1[k] = (f32)(b->v.ob[k] - a->v.ob[k]);
        e2[k] = (f32)(c->v.ob[k] - a->v.ob[k]);
    }
    ny = e1[2] * e2[0] - e1[0] * e2[2];
    nl = __builtin_sqrtf((e1[1] * e2[2] - e1[2] * e2[1]) * (e1[1] * e2[2] - e1[2] * e2[1]) + ny * ny
        + (e1[0] * e2[1] - e1[1] * e2[0]) * (e1[0] * e2[1] - e1[1] * e2[0]));
    if (nl <= 0.0f) return;
    ny /= nl;
    if (ny < 0.05f && ny > -0.05f) return; // walls: no ground under them
    // the colour you see: texture x vertex colour
    r = ((u32)a->v.cn[0] + b->v.cn[0] + c->v.cn[0]) / 3;
    g = ((u32)a->v.cn[1] + b->v.cn[1] + c->v.cn[1]) / 3;
    bl = ((u32)a->v.cn[2] + b->v.cn[2] + c->v.cn[2]) / 3;
    if (tex >= 0 && tex < SURF_TEX_MAX && tl != NULL) {
        if (surf_tex_ok[tex] == 0) surf_tex_ok[tex] = surf_tex_colour(tl, tex, surf_tex_rgb[tex]) ? 1 : 2;
        if (surf_tex_ok[tex] == 1) {
            r = r * surf_tex_rgb[tex][0] / 255;
            g = g * surf_tex_rgb[tex][1] / 255;
            bl = bl * surf_tex_rgb[tex][2] / 255;
        }
    }
    surf_tris = grow(surf_tris, &surf_cap, surf_count + 1, sizeof(SurfTri));
    if (surf_count >= surf_cap) return;
    t = &surf_tris[surf_count++];
    for (k = 0; k < 3; k++) {
        t->v[0][k] = a->v.ob[k];
        t->v[1][k] = b->v.ob[k];
        t->v[2][k] = c->v.ob[k];
    }
    t->mat = surf_classify(r, g, bl);
    t->coll = 0;
    {
        u32 mx = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl);
        t->val = (u8)(mx > 255 ? 255 : mx);
    }
}

// a floor collision triangle with its footstep surface
static void surf_add_coll(const Vtx *a, const Vtx *b, const Vtx *c, s32 flags) {
    f32 e1[3], e2[3], ny, nl;
    SurfTri *t;
    int k;

    for (k = 0; k < 3; k++) {
        e1[k] = (f32)(b->v.ob[k] - a->v.ob[k]);
        e2[k] = (f32)(c->v.ob[k] - a->v.ob[k]);
    }
    ny = e1[2] * e2[0] - e1[0] * e2[2];
    nl = __builtin_sqrtf((e1[1] * e2[2] - e1[2] * e2[1]) * (e1[1] * e2[2] - e1[2] * e2[1]) + ny * ny
        + (e1[0] * e2[1] - e1[1] * e2[0]) * (e1[0] * e2[1] - e1[1] * e2[0]));
    if (nl <= 0.0f) return;
    ny /= nl;
    if (ny < 0.05f && ny > -0.05f) return;
    surf_tris = grow(surf_tris, &surf_cap, surf_count + 1, sizeof(SurfTri));
    if (surf_count >= surf_cap) return;
    t = &surf_tris[surf_count++];
    for (k = 0; k < 3; k++) {
        t->v[0][k] = a->v.ob[k];
        t->v[1][k] = b->v.ob[k];
        t->v[2][k] = c->v.ob[k];
    }
    t->mat = (u8)func_803246B4(map_get(), flags);
    t->val = 0;
    t->coll = 1;
}

static void surf_tri_cols(const SurfTri *t, s32 *x0, s32 *x1, s32 *z0, s32 *z1) {
    s32 mnx = t->v[0][0], mxx = t->v[0][0], mnz = t->v[0][2], mxz = t->v[0][2];
    int k;
    for (k = 1; k < 3; k++) {
        if (t->v[k][0] < mnx) mnx = t->v[k][0];
        if (t->v[k][0] > mxx) mxx = t->v[k][0];
        if (t->v[k][2] < mnz) mnz = t->v[k][2];
        if (t->v[k][2] > mxz) mxz = t->v[k][2];
    }
    *x0 = surf_col((f32)mnx); *x1 = surf_col((f32)mxx);
    *z0 = surf_col((f32)mnz); *z1 = surf_col((f32)mxz);
    // (huge triangles: only so many columns, the rest find it through others)
    if (*x1 - *x0 > 24) *x1 = *x0 + 24;
    if (*z1 - *z0 > 24) *z1 = *z0 + 24;
}

static void surface_build(void) {
    BKModelBin *bin = mapModel_getModelBin(0);
    BKTextureList *tl;
    BKVertexList *vl;
    Gfx *list;
    s32 n, i, total;
    int cache[64], tex = -1, k;

    surf_built = TRUE;
    surf_count = 0;
    for (i = 0; i < SURF_TEX_MAX; i++) surf_tex_ok[i] = 0;
    if (bin == NULL) return;
    list = model_getDisplayList(bin)->list;
    vl = model_getVtxList(bin);
    tl = model_getTextureList(bin);
    n = gfx_count(bin);
    for (i = 0; i < 64; i++) cache[i] = -1;
    for (i = 0; i < n; i++) {
        u32 w0 = list[i].words.w0, w1 = list[i].words.w1, op = OP(w0);
        Patch *pt;

        if (op == 0x06 && (pt = find_patch(i)) != NULL) {
            w0 = pt->w0; // (the triangle as it was before we carved it)
            w1 = pt->w1;
            op = OP(w0);
        }
        if (op == OP_SETTIMG) {
            // segment 2 = the model's texture data: which texture is that
            tex = -1;
            if (((w1 >> 24) & 0x0F) == 2 && tl != NULL) {
                u32 off = w1 & 0x00FFFFFF;
                for (k = 0; k < tl->cnt_4 && k < SURF_TEX_MAX; k++) {
                    BKTextureHeader *h = &tl->tex_8[k];
                    u32 bits = (h->type_4 & 1) ? 4 : (h->type_4 & 2) ? 8 : (h->type_4 & 4) ? 16 : (h->type_4 & 8) ? 32 : 0;
                    u32 size = bits * h->width_8 * h->height_9 / 8 + ((h->type_4 & 1) ? 0x20 : (h->type_4 & 2) ? 0x200 : 0);
                    if (off >= (u32)h->offset_0 && off < (u32)h->offset_0 + size) { tex = k; break; }
                }
            }
        } else if (op == OP_VTX) {
            s32 v0 = ((w0 >> 16) & 0xFF) / 2, cnt = (w0 >> 10) & 0x3F, base;
            if (((w1 >> 24) & 0x0F) != 1) {
                for (k = 0; k < cnt && v0 + k < 64; k++) cache[v0 + k] = -1;
                continue;
            }
            base = (w1 & 0x00FFFFFF) / sizeof(Vtx);
            for (k = 0; k < cnt && v0 + k < 64; k++) cache[v0 + k] = (base + k < vl->count) ? base + k : -1;
        } else if (op == OP_TRI1 || op == OP_TRI2) {
            u32 words[2];
            int t, ntri = (op == OP_TRI2) ? 2 : 1;
            words[0] = (op == OP_TRI2) ? w0 : w1;
            words[1] = w1;
            for (t = 0; t < ntri; t++) {
                int ia = cache[((words[t] >> 16) & 0xFF) / 2 & 63];
                int ib = cache[((words[t] >> 8) & 0xFF) / 2 & 63];
                int ic = cache[(words[t] & 0xFF) / 2 & 63];
                if (ia < 0 || ib < 0 || ic < 0) continue;
                surf_add(&vl->vtx_18[ia], &vl->vtx_18[ib], &vl->vtx_18[ic], tex, tl);
            }
        }
    }
    // the collision floors (with the ones carving shrank, as they were)
    {
        BKCollisionList *cl = model_getCollisionList(bin);
        if (cl != NULL) {
            BKCollisionTri *tris = (BKCollisionTri *)((BKCollisionGeo *)(cl + 1) + cl->unk10);
            for (i = 0; i < cl->unk14; i++) {
                BKCollisionTri *t = &tris[i];
                if (t->unk0[0] == t->unk0[1] && t->unk0[1] == t->unk0[2]) continue;
                surf_add_coll(&vl->vtx_18[t->unk0[0]], &vl->vtx_18[t->unk0[1]], &vl->vtx_18[t->unk0[2]], t->flags);
            }
            for (i = 0; i < coll_saved_count; i++) {
                BKCollisionTri *t = &tris[coll_saved[i].tri];
                surf_add_coll(&vl->vtx_18[t->unk0[0]], &vl->vtx_18[coll_saved[i].v1], &vl->vtx_18[coll_saved[i].v2], t->flags);
            }
        }
    }
    // bucket the triangles by the 4 block columns they cover (counting sort)
    for (i = 0; i <= SURF_BUCKETS; i++) surf_first[i] = 0;
    total = 0;
    for (i = 0; i < surf_count; i++) {
        s32 x0, x1, z0, z1, x, z;
        surf_tri_cols(&surf_tris[i], &x0, &x1, &z0, &z1);
        for (x = x0; x <= x1; x++) for (z = z0; z <= z1; z++) { surf_first[surf_bucket(x, z) + 1]++; total++; }
    }
    for (i = 0; i < SURF_BUCKETS; i++) surf_first[i + 1] += surf_first[i];
    surf_list = grow(surf_list, &surf_list_cap, total + 1, sizeof(int));
    if (surf_list_cap < total) { surf_count = 0; return; }
    {
        static int fill[SURF_BUCKETS];
        for (i = 0; i < SURF_BUCKETS; i++) fill[i] = surf_first[i];
        for (i = 0; i < surf_count; i++) {
            s32 x0, x1, z0, z1, x, z;
            surf_tri_cols(&surf_tris[i], &x0, &x1, &z0, &z1);
            for (x = x0; x <= x1; x++) for (z = z0; z <= z1; z++) surf_list[fill[surf_bucket(x, z)]++] = i;
        }
    }
    {
        int hist[16] = { 0 };
        for (i = 0; i < surf_count; i++) if (surf_tris[i].coll) hist[surf_tris[i].mat & 15]++;
        recomp_printf("[mc] ground looks: %d floor triangles, %d textures; footstep surfaces 1:%d 2:%d 3:%d 5:%d 6:%d 7:%d 8:%d 9:%d 10:%d 11:%d 12:%d\n",
            surf_count, tl ? tl->cnt_4 : 0, hist[1], hist[2], hist[3], hist[5], hist[6], hist[7], hist[8], hist[9], hist[10], hist[11], hist[12]);
    }
}

// what the ground over a cell looks like: the floor triangle over the cell's
// middle nearest to its surface height
static u8 surface_material(s32 x, s32 z, f32 surface_y, u8 *val_out) {
    f32 px = (x + 0.5f) * MC_BLOCK, pz = (z + 0.5f) * MC_BLOCK, best = 2.5f * MC_BLOCK, best_c = 2.5f * MC_BLOCK;
    u32 bk;
    int i, mat = SM_UNKNOWN, snd = 0, val = 128;

    if (!surf_built) surface_build();
    if (surf_count == 0) return SM_UNKNOWN;
    bk = surf_bucket(surf_col(px), surf_col(pz));
    for (i = surf_first[bk]; i < surf_first[bk + 1]; i++) {
        const SurfTri *t = &surf_tris[surf_list[i]];
        f32 ax = t->v[0][0], az = t->v[0][2];
        f32 e1x = t->v[1][0] - ax, e1z = t->v[1][2] - az, e2x = t->v[2][0] - ax, e2z = t->v[2][2] - az;
        f32 den = e1x * e2z - e2x * e1z, u, w, ty, d;
        if (den > -0.001f && den < 0.001f) continue;
        u = ((px - ax) * e2z - e2x * (pz - az)) / den;
        w = (e1x * (pz - az) - (px - ax) * e1z) / den;
        if (u < -0.001f || w < -0.001f || u + w > 1.001f) continue;
        ty = t->v[0][1] + (t->v[1][1] - t->v[0][1]) * u + (t->v[2][1] - t->v[0][1]) * w;
        d = ty - surface_y;
        if (d < 0.0f) d = -d;
        if (t->coll) {
            if (d < best_c) {
                best_c = d;
                snd = t->mat;
            }
        } else if (d < best && t->mat != SM_UNKNOWN) {
            best = d;
            mat = t->mat;
            val = t->val;
        }
    }
    if (val_out != NULL) *val_out = (u8)val;
    // what bk says it's made of (its footstep sound) wins over the colour
    switch (snd) {
        case 2: return SM_SAND;
        case 3: return SM_SNOW;
        case 5: return SM_WOOD;
        case 6: return (mat == SM_ICE) ? SM_ICE : SM_STONE;
        case 7: return SM_GRASS;
        case 8: return SM_MUD;
        case 10: return SM_METAL;
    }
    return (u8)mat;
}

// what a buried cell is made of, d = how deep it is (blocks): the map's top
// layers, then stone with minecraft's underground in it (ores.c)
static u16 ground_block(s32 x, s32 y, s32 z, f32 depth, int top) {
    s32 d = (depth > 0.0f) ? (s32)depth : 0;
    u8 val = 128;

    // the block family it looks like, from the surface over it
    switch (surface_material(x, z, (y + 0.5f + depth) * MC_BLOCK, &val)) {
        case SM_WOOD:
            // stumps, bridges, floors: light rings are a log's end, darker
            // boards are planks
            if (depth >= 6.0f) return ores_block_at(x, z, d);
            if (val >= 150) return BLOCK_OAK_LOG;
            return (val >= 85) ? BLOCK_SPRUCE_PLANKS : BLOCK_DARK_OAK_PLANKS;
        case SM_MUD:
            return (depth < 4.0f) ? BLOCK_MUD : ores_block_at(x, z, d);
        case SM_METAL:
            return (depth < 2.0f) ? BLOCK_IRON_BLOCK : ores_block_at(x, z, d);
        case SM_GRASS:
            if (top && depth < 1.0f) return BLOCK_GRASS;
            return (depth < 4.0f) ? BLOCK_DIRT : ores_block_at(x, z, d);
        case SM_DIRT:
            return (depth < 4.0f) ? BLOCK_DIRT : ores_block_at(x, z, d);
        case SM_SAND:
            // a beach / desert: sand over sandstone
            if (depth < 3.0f) return BLOCK_SAND;
            return (depth < 6.0f) ? BLOCK_SANDSTONE : ores_block_at(x, z, d);
        case SM_SNOW:
            if (top && depth < 1.0f) return BLOCK_SNOW_BLOCK;
            return (depth < 4.0f) ? BLOCK_DIRT : ores_block_at(x, z, d);
        case SM_STONE:
            return ores_block_at(x, z, d); // rock all the way down
        case SM_ICE:
            return (depth < 2.0f) ? BLOCK_PACKED_ICE : ores_block_at(x, z, d);
        case SM_TERRACOTTA:
            return (depth < 4.0f) ? BLOCK_TERRACOTTA : ores_block_at(x, z, d);
    }
    // can't tell: the map's own layers
    if (map_get() == MAP_7_TTC_TREASURE_TROVE_COVE) {
        if (depth < 3.0f) return BLOCK_SAND;
        if (depth < 6.0f) return BLOCK_SANDSTONE;
        return ores_block_at(x, z, d);
    }
    if (top && map_get() == MAP_27_FP_FREEZEEZY_PEAK && depth < 1.0f) return BLOCK_SNOW_BLOCK;
    if (top && depth < 1.0f) return BLOCK_GRASS;
    if (depth < 4.0f) return BLOCK_DIRT;
    return ores_block_at(x, z, d);
}

u16 terrain_block_at(s32 x, s32 y, s32 z) {
    f32 depth = 0.0f;

    if (cell_is_edge(x, y, z)) return BLOCK_BEDROCK;
    if (!cell_inside_ground(x, y, z, &depth)) {
        // the floor of a hole: one deeper than the dug cell over it (like
        // its wall, see rebuild_fills)
        f32 above;
        depth = 0.0f;
        if (terrain_is_carved(x, y + 1, z) && cell_partly_in_ground(x, y + 1, z, &above)) depth = above + 1.0f;
    }
    return ground_block(x, y, z, depth, TRUE);
}

// what the walls of a hole are made of at this depth (no grass inside)
static u16 fill_block(s32 x, s32 y, s32 z, f32 depth) {
    if (cell_is_edge(x, y, z)) return BLOCK_BEDROCK;
    return ground_block(x, y, z, depth, FALSE);
}

// the walls' corner heights never change (they're measured against the
// level as it was before any digging), so each wall face is worked out once
#define FILL_CACHE 32768 // power of 2
typedef struct {
    s32 x, y, z;
    u8 face, used;
    f32 top_y[4];
} FillCache;
static FillCache fill_cache[FILL_CACHE];

static u32 fill_slot(s32 x, s32 y, s32 z, u8 face) {
    return ((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u ^ (u32)face * 2654435761u) & (FILL_CACHE - 1);
}

static int fill_cache_get(s32 x, s32 y, s32 z, u8 face, f32 out[4]) {
    u32 h = fill_slot(x, y, z, face), i;
    for (i = 0; i < 32; i++) {
        FillCache *c = &fill_cache[(h + i) & (FILL_CACHE - 1)];
        if (!c->used) return FALSE;
        if (c->x == x && c->y == y && c->z == z && c->face == face) {
            int k;
            for (k = 0; k < 4; k++) out[k] = c->top_y[k];
            return TRUE;
        }
    }
    return FALSE;
}

static void fill_cache_put(s32 x, s32 y, s32 z, u8 face, const f32 v[4]) {
    u32 h = fill_slot(x, y, z, face), i;
    for (i = 0; i < 32; i++) {
        FillCache *c = &fill_cache[(h + i) & (FILL_CACHE - 1)];
        if (!c->used || (c->x == x && c->y == y && c->z == z && c->face == face)) {
            int k;
            c->used = TRUE;
            c->x = x;
            c->y = y;
            c->z = z;
            c->face = face;
            for (k = 0; k < 4; k++) c->top_y[k] = v[k];
            return;
        }
    }
}

static void fill_cache_reset(void) {
    int k;
    for (k = 0; k < FILL_CACHE; k++) fill_cache[k].used = FALSE;
}

static void rebuild_fills(void) {
    static const s8 dir[6][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 } };
    static const u8 opposite[6] = { 1, 0, 3, 2, 5, 4 };
    int i, f;

    fill_count = 0;
    // every dug cell was solid ground when it was dug. one the ground test
    // can't place any more (dug past a wall or down under the level's floor
    // before those counted) takes its depth from a buried dug neighbour, so
    // the walls around it come back
    {
        int pass, changed = TRUE;
        for (pass = 0; pass < 16 && changed; pass++) {
            changed = FALSE;
            for (i = 0; i < carved_count; i++) {
                f32 here, nd;
                if (cell_partly_in_ground(carved[i][0], carved[i][1], carved[i][2], &here)) continue;
                for (f = 0; f < 6; f++) {
                    s32 nx = carved[i][0] + dir[f][0], ny = carved[i][1] + dir[f][1], nz = carved[i][2] + dir[f][2];
                    if (!terrain_is_carved(nx, ny, nz) || !cell_partly_in_ground(nx, ny, nz, &nd) || nd < 0.0f) continue;
                    // (the neighbour over it: one deeper; under it: one less)
                    nd += (f == 0) ? 1.0f : (f == 1 ? -1.0f : 0.0f);
                    if (nd < 0.0f) continue;
                    terrain_shell_restore(carved[i][0], carved[i][1], carved[i][2], nd);
                    changed = TRUE;
                    break;
                }
            }
        }
    }
    for (i = 0; i < carved_count; i++) {
        for (f = 0; f < 6; f++) {
            s32 nx = carved[i][0] + dir[f][0], ny = carved[i][1] + dir[f][1], nz = carved[i][2] + dir[f][2];
            f32 depth;
            int shell;

            if (fill_count >= MAX_FILL) return;
            if (terrain_is_carved(nx, ny, nz) || world_get(nx, ny, nz) != BLOCK_AIR) continue;
            shell = FALSE;
            if (!cell_partly_in_ground(nx, ny, nz, &depth)) {
                // under a dug cell that was itself in the ground there is
                // ground too, even when the ray can't tell (steep slopes)
                f32 here;
                int here_in = cell_partly_in_ground(carved[i][0], carved[i][1], carved[i][2], &here);
                if (f == 1 && here_in && cell_under_overhang(nx, ny, nz)) {
                    continue; // dug through a thin ledge: open air under it
                } else if (f == 1 && here_in) {
                    depth = here + 1.0f;
                    // underground from now on: digging it out, the cells
                    // around it get their walls too (else a shaft dug down
                    // past the level's floor had none after the first cell)
                    if (here >= 0.0f) terrain_shell_restore(nx, ny, nz, depth);
                } else if (here_in && here >= 0.0f
                    && cell_under_shell(nx, ny, nz, (carved[i][1] + 0.5f + here) * MC_BLOCK)) {
                    // beside / over / under a buried dug cell, inside the
                    // level's body but with no floor over it (past a wall).
                    // (over it only when the dug cell is more than a block
                    // deep: cell_under_shell wants it under the surface)
                    depth = (f == 0) ? here - 1.0f : here;
                    shell = TRUE;
                    shell_put(nx, ny, nz, depth, TRUE); // solid from now on: the edge of the world
                } else {
                    continue;
                }
            }
            if (shell_slot(nx, ny, nz, FALSE) != NULL) shell = TRUE; // (found on an earlier pass)
            fills[fill_count].cell[0] = nx;
            fills[fill_count].cell[1] = ny;
            fills[fill_count].cell[2] = nz;
            fills[fill_count].face = opposite[f]; // the neighbor's face that looks into the hole
            fills[fill_count].block = fill_block(nx, ny, nz, depth);
            // side walls stop at the ground surface: look up from the bottom
            // of the solid cell, a unit inside it, at each top corner
            {
                static const u8 fc[6][4][3] = {
                    { { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },
                    { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } },
                    { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } },
                    { { 1, 0, 0 }, { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 } },
                    { { 1, 0, 1 }, { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 } },
                    { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 } },
                };
                u8 face = opposite[f];
                int c;
                f32 full = (ny + 1) * MC_BLOCK;

                if (shell) {
                    // (no surface over it to stop at: the whole face)
                    for (c = 0; c < 4; c++) fills[fill_count].top_y[c] = full;
                } else if (!fill_cache_get(nx, ny, nz, face, fills[fill_count].top_y)) {
                    for (c = 0; c < 4; c++) {
                        f32 px = (nx + fc[face][c][0]) * MC_BLOCK, pz = (nz + fc[face][c][2]) * MC_BLOCK;
                        // nudge toward the cell's middle so the ray stays in its column
                        px += (fc[face][c][0] ? -1.0f : 1.0f);
                        pz += (fc[face][c][2] ? -1.0f : 1.0f);
                        // look from a block below so a corner whose ground is
                        // under the cell finds it (a miss used to give full
                        // height: the spikes sticking out into the air)
                        if (depth >= 1.5f) {
                            fills[fill_count].top_y[c] = full;
                        } else {
                            f32 t = surface_y_at(px, (ny - 1) * MC_BLOCK, pz, ny * MC_BLOCK);
                            if (t < ny * MC_BLOCK) t = ny * MC_BLOCK;
                            if (t > full) t = full;
                            fills[fill_count].top_y[c] = t;
                        }
                    }
                    fill_cache_put(nx, ny, nz, face, fills[fill_count].top_y);
                }
            }
            fill_count++;
        }
    }
}

// --- carving a cell ---

int terrain_carved_count(void) {
    return carved_count;
}

const s32 *terrain_carved_cell(int i) {
    return carved[i];
}

int terrain_carve(s32 x, s32 y, s32 z) {
    f32 lo[3], hi[3];

    if (terrain_is_carved(x, y, z) || carved_count >= MAX_CARVED) {
        return FALSE;
    }
    // ask about this cell and its neighbors while the ground is still whole
    {
        static const s8 nb[7][3] = { { 0, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 } };
        int k;
        for (k = 0; k < 7; k++) cell_inside_ground(x + nb[k][0], y + nb[k][1], z + nb[k][2], NULL);
    }
    carved[carved_count][0] = x;
    carved[carved_count][1] = y;
    carved[carved_count][2] = z;
    carved_hash_add(carved_count);
    carved_count++;

    lo[0] = x * MC_BLOCK;
    lo[1] = y * MC_BLOCK;
    lo[2] = z * MC_BLOCK;
    hi[0] = lo[0] + MC_BLOCK;
    hi[1] = lo[1] + MC_BLOCK;
    hi[2] = lo[2] + MC_BLOCK;
    // a hair bigger: ground lying exactly on a block boundary (flat floors at
    // multiples of 80) has to be inside the box to be cut, not touching it
    lo[0] -= 0.5f; lo[1] -= 0.5f; lo[2] -= 0.5f;
    hi[0] += 0.5f; hi[1] += 0.5f; hi[2] += 0.5f;

    if (pending_count < MAX_PENDING) {
        int k;
        for (k = 0; k < 3; k++) {
            pending_lo[pending_count][k] = lo[k];
            pending_hi[pending_count][k] = hi[k];
        }
        pending_cell[pending_count][0] = x;
        pending_cell[pending_count][1] = y;
        pending_cell[pending_count][2] = z;
        pend_add(pending_count);
        pending_count++;
    }
    if (carve_batch == 0) carve_flush();
    return TRUE;
}

// a cell's box into the queue carve_flush cuts
static void pend_push_cell(s32 x, s32 y, s32 z) {
    int k;
    if (pending_count >= MAX_PENDING) return;
    pending_lo[pending_count][0] = x * MC_BLOCK - 0.5f;
    pending_lo[pending_count][1] = y * MC_BLOCK - 0.5f;
    pending_lo[pending_count][2] = z * MC_BLOCK - 0.5f;
    for (k = 0; k < 3; k++) pending_hi[pending_count][k] = pending_lo[pending_count][k] + MC_BLOCK + 1.0f;
    pending_cell[pending_count][0] = x;
    pending_cell[pending_count][1] = y;
    pending_cell[pending_count][2] = z;
    pend_add(pending_count);
    pending_count++;
}

// filling a dug cell back in: bk's ground comes back there. the level's
// display list and collision are put back as they were, then every other
// hole is cut again in one go (like loading the map with its saved holes)
int terrain_uncarve(s32 x, s32 y, s32 z) {
    BKModelBin *bin = mapModel_getModelBin(0);
    int i, idx = -1;

    if (bin == NULL || !terrain_is_carved(x, y, z)) return FALSE;
    for (i = 0; i < carved_count; i++) {
        if (carved[i][0] == x && carved[i][1] == y && carved[i][2] == z) { idx = i; break; }
    }
    if (idx < 0) return FALSE;
    carve_flush(); // (anything still queued first)
    carved_count--;
    for (i = 0; i < 3; i++) carved[idx][i] = carved[carved_count][i];
    for (i = 0; i < CARVED_HASH; i++) carved_hash[i] = 0;
    for (i = 0; i < carved_count; i++) carved_hash_add(i);
    // bk's triangle commands back (the same write order build_patch uses:
    // a no-op first, rt64 may be reading this list right now)
    {
        Gfx *list = model_getDisplayList(bin)->list;
        for (i = 0; i < patch_count; i++) {
            Patch *pt = &patches[i];
            ((volatile u32 *)&list[pt->cmd].words.w0)[0] = 0;
            ((volatile u32 *)&list[pt->cmd].words.w1)[0] = pt->w1;
            ((volatile u32 *)&list[pt->cmd].words.w0)[0] = pt->w0;
            if (pt->dl) world_draw_bury(pt->dl);
            if (pt->vtx) world_draw_bury(pt->vtx);
            pt->dl = NULL;
            pt->vtx = NULL;
        }
    }
    // the collision triangles back
    {
        BKCollisionList *cl = model_getCollisionList(bin);
        if (cl != NULL) {
            BKCollisionTri *tris = (BKCollisionTri *)((BKCollisionGeo *)(cl + 1) + cl->unk10);
            for (i = 0; i < coll_saved_count; i++) {
                tris[coll_saved[i].tri].unk0[1] = coll_saved[i].v1;
                tris[coll_saved[i].tri].unk0[2] = coll_saved[i].v2;
            }
        }
    }
    coll_saved_count = 0;
    patch_count = 0;
    for (i = 0; i < PATCH_HASH; i++) patch_hash[i] = 0;
    rpiece_count = 0;
    cpiece_count = 0;
    ghost_count = 0;
    fill_count = 0;
    pend_clear();
    // and the rest cut again
    for (i = 0; i < carved_count; i++) pend_push_cell(carved[i][0], carved[i][1], carved[i][2]);
    if (carved_count > 0) {
        carve_flush();
    } else {
        rebuild_fills();
        grids_dirty();
    }
    render_dirty = TRUE;
    recomp_printf("[mc] uncarved %d %d %d (%d holes left)\n", x, y, z, carved_count);
    return TRUE;
}

// everything queued since the last flush, cut in one go: the old pieces
// once, the level's display list and collision once (each triangle minus
// every box it touches), then the patches and the walls
static void carve_flush(void) {
    if (pending_count == 0) return;
    {
        PROF_START(t_rc);
        recarve_pending();
        PROF_END(t_rc, "carve: recut pieces");
    }
    {
        PROF_START(t_dl);
        carve_display_list();
        carve_collision();
        PROF_END(t_dl, "carve: level walk");
    }
    pend_clear();
    {
        PROF_START(t_pf);
        rebuild_patches();
        PROF_END(t_pf, "carve: patches");
    }
    {
        PROF_START(t_fl);
        rebuild_fills();
        PROF_END(t_fl, "carve: hole walls");
    }
    grids_dirty();
}

// batches nest: explosions in the same tick share one flush
void terrain_carve_begin(void) {
    carve_batch++;
}

void terrain_carve_end(void) {
    if (carve_batch > 0) carve_batch--;
    if (carve_batch == 0) carve_flush();
}

// any of the cell under the ground surface: its middle, or one of its four
// corner columns (a slope can cross a corner and miss the middle)
#define TOUCH_CACHE 4096
static struct { s32 x, y, z; u8 used, v; } touch_cache[TOUCH_CACHE];

static void touch_cache_reset(void) {
    int k;
    for (k = 0; k < TOUCH_CACHE; k++) touch_cache[k].used = FALSE;
}

int terrain_cell_touches_ground(s32 x, s32 y, s32 z) {
    u32 h = ((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u) & (TOUCH_CACHE - 1);
    f32 d;
    int c, v = FALSE;

    if (touch_cache[h].used && touch_cache[h].x == x && touch_cache[h].y == y && touch_cache[h].z == z) return touch_cache[h].v;
    cell_inside_ground(x, y, z, &d);
    v = d > -1.0f;
    for (c = 0; c < 4 && !v; c++) {
        f32 px = (x + ((c & 1) ? 0.95f : 0.05f)) * MC_BLOCK, pz = (z + ((c & 2) ? 0.95f : 0.05f)) * MC_BLOCK;
        // the first upward surface from the cell's floor: inside the cell?
        f32 top = surface_y_at(px, y * MC_BLOCK, pz, -1e9f);
        v = top > y * MC_BLOCK && top < (y + 1) * MC_BLOCK;
    }
    touch_cache[h].used = TRUE;
    touch_cache[h].x = x;
    touch_cache[h].y = y;
    touch_cache[h].z = z;
    touch_cache[h].v = (u8)v;
    return v;
}

// --- collision provider part (world.c's line query asks this) ---

static void cpiece_aabb(int i, f32 mn[3], f32 mx[3]) {
    int k;
    for (k = 0; k < 3; k++) {
        mn[k] = mx[k] = cpieces[i].p[0][k];
        if (cpieces[i].p[1][k] < mn[k]) mn[k] = cpieces[i].p[1][k];
        if (cpieces[i].p[2][k] < mn[k]) mn[k] = cpieces[i].p[2][k];
        if (cpieces[i].p[1][k] > mx[k]) mx[k] = cpieces[i].p[1][k];
        if (cpieces[i].p[2][k] > mx[k]) mx[k] = cpieces[i].p[2][k];
    }
}

static void fill_aabb(int i, f32 mn[3], f32 mx[3]) {
    int k;
    for (k = 0; k < 3; k++) {
        mn[k] = fills[i].cell[k] * MC_BLOCK;
        mx[k] = mn[k] + MC_BLOCK;
    }
}

static Grid cpiece_grid = { TRUE, 0, { 0 }, NULL, 0, NULL, 0, 0, NULL, 0, 0, cpiece_aabb };
static Grid fill_grid = { TRUE, 0, { 0 }, NULL, 0, NULL, 0, 0, NULL, 0, 0, fill_aabb };

static void grids_dirty(void) {
    walls_changed();
    ghost_grid.dirty = TRUE;
    cpiece_grid.dirty = TRUE;
    fill_grid.dirty = TRUE;
}

static BKCollisionTri piece_tri;

// segment start->end vs the leftover collision pieces. on a hit: shortens end,
// writes the unit normal, returns a tri with the piece's flags.
void *terrain_line_query(f32 start[3], f32 end[3], f32 out_normal[3], u32 flag_filter) {
    f32 d[3], best_t = 1.0f;
    int i, k, best = -1;

    int gq, gn, j;
    for (k = 0; k < 3; k++) d[k] = end[k] - start[k];
    gq = grid_query(&cpiece_grid, cpiece_count, start, end);
    gn = gq < 0 ? cpiece_count : gq;
    for (j = 0; j < gn; j++) {
        CPiece *cp;
        i = gq < 0 ? j : grid_out[j];
        cp = &cpieces[i];
        f32 e1[3], e2[3], pv[3], tv[3], qv[3], det, inv, u, v, t;

        if (cp->flags & flag_filter) continue;
        for (k = 0; k < 3; k++) {
            e1[k] = cp->p[1][k] - cp->p[0][k];
            e2[k] = cp->p[2][k] - cp->p[0][k];
        }
        // moller-trumbore
        pv[0] = d[1] * e2[2] - d[2] * e2[1];
        pv[1] = d[2] * e2[0] - d[0] * e2[2];
        pv[2] = d[0] * e2[1] - d[1] * e2[0];
        det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
        if (det > -1e-6f && det < 1e-6f) continue;
        inv = 1.0f / det;
        for (k = 0; k < 3; k++) tv[k] = start[k] - cp->p[0][k];
        u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        qv[0] = tv[1] * e1[2] - tv[2] * e1[1];
        qv[1] = tv[2] * e1[0] - tv[0] * e1[2];
        qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
        v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
        if (t < 0.0f || t >= best_t) continue;
        best_t = t;
        best = i;
    }
    // the block faces on the sides of holes are solid too (bk's ground is only
    // a surface, there's nothing under it to stand on)
    {
        static const f32 corners[6][4][3] = {
            { { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },
            { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } },
            { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } },
            { { 1, 0, 0 }, { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 } },
            { { 1, 0, 1 }, { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 } },
            { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 } },
        };
        static const s8 fnorm[6][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 } };
        int best_fill = -1;

        int fq = grid_query(&fill_grid, fill_count, start, end), fn = fq < 0 ? fill_count : fq, fj;
        for (fj = 0; fj < fn; fj++) {
            FillFace *ff;
            i = fq < 0 ? fj : grid_out[fj];
            ff = &fills[i];
            int axis = (ff->face < 2) ? 1 : (ff->face < 4 ? 2 : 0);
            f32 plane = (ff->cell[axis] + ((fnorm[ff->face][axis] > 0) ? 1 : 0)) * MC_BLOCK;
            f32 t, p[3];
            int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;

            if (flag_filter & 0) continue;
            if (d[axis] > -1e-6f && d[axis] < 1e-6f) continue;
            t = (plane - start[axis]) / d[axis];
            if (t < 0.0f || t >= best_t) continue;
            for (k = 0; k < 3; k++) p[k] = start[k] + d[k] * t;
            if (p[a1] < ff->cell[a1] * MC_BLOCK || p[a1] > (ff->cell[a1] + 1) * MC_BLOCK) continue;
            if (p[a2] < ff->cell[a2] * MC_BLOCK || p[a2] > (ff->cell[a2] + 1) * MC_BLOCK) continue;
            if (ff->face >= 2) {
                // side walls stop at the ground surface, like the drawn face
                // (top_y at the two top corners, interpolated across the wall)
                int h = (ff->face < 4) ? 0 : 2, c, n = 0;
                f32 ta = 0.0f, tb = 0.0f, u = (p[h] - ff->cell[h] * MC_BLOCK) / MC_BLOCK, top;
                for (c = 0; c < 4; c++) {
                    if (corners[ff->face][c][1] != 1) continue;
                    if (corners[ff->face][c][h] == 0) ta = ff->top_y[c];
                    else tb = ff->top_y[c];
                    n++;
                }
                top = ta + (tb - ta) * u;
                if (n == 2 && p[1] > top + 1.0f) continue;
            }
            best_t = t;
            best_fill = i;
        }
        if (best_fill >= 0) {
            FillFace *ff = &fills[best_fill];
            for (k = 0; k < 3; k++) {
                end[k] = start[k] + d[k] * best_t;
                out_normal[k] = fnorm[ff->face][k];
            }
            piece_tri.flags = 0;
            piece_tri.unk6 = 0;
            return &piece_tri;
        }
    }
    if (best < 0) return NULL;
    {
        CPiece *cp = &cpieces[best];
        f32 e1[3], e2[3], n[3], len;
        for (k = 0; k < 3; k++) {
            e1[k] = cp->p[1][k] - cp->p[0][k];
            e2[k] = cp->p[2][k] - cp->p[0][k];
        }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        len = __builtin_sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0.0f) for (k = 0; k < 3; k++) n[k] /= len;
        // bk tris face the side they're hit from (front faces): face the ray
        if (n[0] * d[0] + n[1] * d[1] + n[2] * d[2] > 0.0f) for (k = 0; k < 3; k++) n[k] = -n[k];
        for (k = 0; k < 3; k++) {
            end[k] = start[k] + d[k] * best_t;
            out_normal[k] = n[k];
        }
        piece_tri.flags = cp->flags;
        piece_tri.unk6 = cp->unk6;
        return &piece_tri;
    }
}

// --- drawing ---

#define VTX_FRAMES 3
static int rvtx_frame;

static void walls_changed(void) {
    wall_dirty = TRUE;
}

// the walls as one display list of their own, rebuilt when they change:
// grouped by texture, 8 faces a vertex load (bk's per-frame list only gets a
// call to it, so lots of holes can't overflow it)
static Vtx *wall_vtx;
static Gfx *wall_dl;
static int wall_count;
void world_draw_bury(void *p);

static void build_walls(void) {
#ifdef MC_HAVE_TEXTURES
    static const f32 zero[3] = { 0.0f, 0.0f, 0.0f };
    static s16 tex_of[MAX_FILL];
    static u16 order[MAX_FILL];
    static int tex_first[MC_TEX_COUNT + 1];
    int i, t, n = 0, q, g_need;
    Gfx *g;
    Vtx *tmp;

    world_draw_bury(wall_vtx);
    world_draw_bury(wall_dl);
    wall_vtx = NULL;
    wall_dl = NULL;
    wall_count = 0;
    wall_dirty = FALSE;
    if (fill_count == 0) return;
    tmp = recomp_alloc(sizeof(Vtx) * 4 * fill_count);
    if (tmp == NULL) return;
    for (i = 0; i < fill_count; i++) {
        tex_of[i] = (s16)world_block_face_clip_vtx(fills[i].block, fills[i].cell, fills[i].face, zero, &tmp[i * 4], fills[i].top_y);
    }
    // counting sort by texture
    for (t = 0; t <= MC_TEX_COUNT; t++) tex_first[t] = 0;
    for (i = 0; i < fill_count; i++) if (tex_of[i] >= 0) tex_first[tex_of[i] + 1]++;
    for (t = 0; t < MC_TEX_COUNT; t++) tex_first[t + 1] += tex_first[t];
    {
        static int cur[MC_TEX_COUNT];
        for (t = 0; t < MC_TEX_COUNT; t++) cur[t] = tex_first[t];
        for (i = 0; i < fill_count; i++) if (tex_of[i] >= 0) order[cur[tex_of[i]]++] = (u16)i;
        n = tex_first[MC_TEX_COUNT];
    }
    wall_vtx = recomp_alloc(sizeof(Vtx) * 4 * (n > 0 ? n : 1));
    // per texture: a 7 command load, then per 8 faces 1 vertex load + 8 triangle pairs
    g_need = 2 + n * 2 + 16;
    for (t = 0; t < MC_TEX_COUNT; t++) if (tex_first[t + 1] > tex_first[t]) g_need += 7 + 1;
    wall_dl = recomp_alloc(sizeof(Gfx) * g_need);
    if (wall_vtx == NULL || wall_dl == NULL) {
        recomp_free(tmp);
        return;
    }
    for (i = 0; i < n; i++) {
        int k;
        for (k = 0; k < 4; k++) wall_vtx[i * 4 + k] = tmp[order[i] * 4 + k];
    }
    recomp_free(tmp);
    g = wall_dl;
    for (t = 0; t < MC_TEX_COUNT; t++) {
        int s0 = tex_first[t], s1 = tex_first[t + 1];
        if (s1 == s0) continue;
        gDPLoadTextureBlock(g++, &mc_block_tex[t * 256], G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0,
            G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
        for (q = s0; q < s1; q += 8) {
            int m = s1 - q, k;
            if (m > 8) m = 8;
            gSPVertex(g++, &wall_vtx[q * 4], m * 4, 0);
            for (k = 0; k < m; k++) gSP2Triangles(g++, k * 4, k * 4 + 1, k * 4 + 2, 0, k * 4, k * 4 + 2, k * 4 + 3, 0);
        }
    }
    gSPEndDisplayList(g++);
    wall_count = n;
#endif
}

void terrain_draw(Gfx **gfx, Mtx **mtx) {
    f32 cam[3];

    if (wall_dirty) build_walls();
    if (wall_count == 0 || wall_dl == NULL) return;
    viewport_getPosition_vec3f(cam);
    // world-space corners and the camera in the matrix: rt64 interpolates
    // the matrix, so the walls move with bk's ground between game frames
    mlMtxIdent();
    mlMtxTranslate(-cam[0], -cam[1], -cam[2]);
    mlMtxApply(*mtx);
    gEXMatrixGroupSimpleNormal((*gfx)++, MC_ID_TERRAIN + 1, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    world_set_texture_state(gfx);
    gSPDisplayList((*gfx)++, wall_dl);
    world_restore_texture_state(gfx);
    gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
    gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
}

// how high bk's ground reaches inside a cell (1/16 block, 1..16): the
// original surface over the cell's middle, 16 if it's above the cell
int terrain_cell_surface_top(s32 x, s32 y, s32 z) {
    f32 top = surface_y_at((x + 0.5f) * MC_BLOCK, y * MC_BLOCK, (z + 0.5f) * MC_BLOCK, (y + 1) * MC_BLOCK);
    s32 h = (s32)((top - y * MC_BLOCK) * 16.0f / MC_BLOCK + 0.999f);
    if (h < 1) h = 1;
    if (h > 16) h = 16;
    return h;
}

// the bottom of the level (the lowest vertex of the map model), bk units:
// like minecraft's world bottom, nothing flows below it
static int level_min_ok;
static f32 level_min_y;

static f32 level_max_y = 100000.0f; // the highest vertex (the ground checks stop climbing there)

f32 terrain_level_min_y(void) {
    if (!level_min_ok) {
        BKModelBin *bin = mapModel_getModelBin(0);
        level_min_y = -100000.0f;
        level_max_y = 100000.0f;
        if (bin != NULL) {
            BKVertexList *vl = model_getVtxList(bin);
            int i;
            if (vl != NULL && vl->count > 0) {
                level_min_y = level_max_y = vl->vtx_18[0].v.ob[1];
                for (i = 1; i < vl->count; i++) {
                    if (vl->vtx_18[i].v.ob[1] < level_min_y) level_min_y = vl->vtx_18[i].v.ob[1];
                    if (vl->vtx_18[i].v.ob[1] > level_max_y) level_max_y = vl->vtx_18[i].v.ob[1];
                }
            }
        }
        level_min_ok = TRUE;
    }
    return level_min_y;
}

static f32 level_max_y_get(void) {
    terrain_level_min_y();
    return level_max_y;
}

static void level_min_ok_reset(void) {
    level_min_ok = FALSE;
}
