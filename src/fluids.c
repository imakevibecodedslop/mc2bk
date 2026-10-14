// water and lava, ported from minecraft 26.3:
//   - FlowingFluid: tick (getNewLiquid: 2 sources make a source for water,
//     a fluid above makes it fall), spread (down first, then the sides,
//     getSpread picks the sides closest to a hole within getSlopeFindDistance),
//     isWaterHole, canPassThroughWall (full blocks stop it), canHoldAnyFluid
//     (air and #washed_away_by_fluids, which are destroyed with their drops)
//   - WaterFluid: drop off 1, slope 4, 5 ticks, makes sources;
//     LavaFluid: drop off 2, slope 2, 30 ticks, falling onto water = stone,
//     LiquidBlock.shouldSpreadLiquid: lava next to water = obsidian (source)
//     or cobblestone; canBeReplacedWith rules
//   - LiquidBlockRenderer: corner heights (calculateAverageHeight), the top,
//     the sides next to non-fluid non-opaque blocks, water tinted 0x3F76E4
//     (plains water colour), the still textures only (no flow texture yet)
//   - BucketItem.use / emptyContents / LiquidBlock.pickupBlock,
//     ItemUtils.createFilledResult (creative keeps its bucket)
// bk's ground counts as solid ground for the fluids.

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "prof.h"
#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "terrain.h"
#include "items.h"
#include "mc_gbi.h"
#include "fluids.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif
#include "sound.h"

static const s8 hdir[4][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { 1, 0, 0 }, { -1, 0, 0 } }; // N S E W
static u32 now;

// --- reading the world ---

typedef struct {
    int type;     // FLUID_*
    int amount;   // 1..8 (source 8)
    int source;
    int falling;
} FState;

static int type_of_block(BlockId b) {
    return b == BLOCK_WATER ? FLUID_WATER : (b == BLOCK_LAVA ? FLUID_LAVA : FLUID_NONE);
}

static BlockId block_of_type(int t) {
    return t == FLUID_WATER ? BLOCK_WATER : BLOCK_LAVA;
}

// LiquidBlock.getFluidState from the legacy level
static FState fstate(s32 x, s32 y, s32 z) {
    u16 c = world_get_cell(x, y, z);
    FState f;
    int lvl = CELL_STATE(c) & 15;
    f.type = type_of_block(CELL_ID(c));
    f.source = f.type != FLUID_NONE && lvl == 0;
    f.falling = lvl >= 8;
    f.amount = f.type == FLUID_NONE ? 0 : (lvl == 0 ? 8 : (lvl < 8 ? 8 - lvl : 8));
    return f;
}

int fluid_type_at(s32 x, s32 y, s32 z) {
    return type_of_block(world_get(x, y, z));
}

// bk's level here (counts as a full block)
static int ground(s32 x, s32 y, s32 z) {
    return world_get(x, y, z) == BLOCK_AIR && !terrain_is_carved(x, y, z) && terrain_cell_in_ground(x, y, z);
}

// a full collision block (getCollisionShape == Shapes.block())
static int full_block(s32 x, s32 y, s32 z) {
    BlockId b = world_get(x, y, z);
    const McBlock *mb;
    if (b == BLOCK_AIR) return ground(x, y, z);
    mb = mc_block(b);
    if (mb->flags & (MCB_FLUID | MCB_NO_COLLISION)) return FALSE;
    return (mb->flags & MCB_MODEL) == 0; // model blocks (trapdoors...) let it by
}

// canPassThroughWall, without the shape cache: full blocks on either side stop it
static int pass_wall(s32 sx, s32 sy, s32 sz, s32 tx, s32 ty, s32 tz) {
    return !full_block(tx, ty, tz) && !full_block(sx, sy, sz);
}

// canHoldAnyFluid: air, fluids and #washed_away_by_fluids
static int can_hold_any(s32 x, s32 y, s32 z) {
    BlockId b = world_get(x, y, z);
    // below the level's bottom is the void (minecraft's world bottom)
    if ((f32)y * MC_BLOCK < terrain_level_min_y() - MC_BLOCK) return FALSE;
    if (b == BLOCK_AIR) return !ground(x, y, z);
    return (mc_block(b)->flags & (MCB_FLUID | MCB_WASHED)) != 0;
}

static int is_source_of(int type, s32 x, s32 y, s32 z) {
    FState f = fstate(x, y, z);
    return f.type == type && f.source;
}

// BlockState.isSolid (for making sources): full blocks and the ground
static int solid(s32 x, s32 y, s32 z) {
    return full_block(x, y, z);
}

static int drop_off(int type) { return type == FLUID_WATER ? 1 : 2; }
static int slope_find(int type) { return type == FLUID_WATER ? 4 : 2; }
static int tick_delay(int type) { return type == FLUID_WATER ? 5 : 30; }

// FluidState.canBeReplacedWith (the fluid already at a cell)
static int replaceable_by(FState at, s32 x, s32 y, s32 z, int other, int down) {
    if (at.type == FLUID_NONE) return TRUE;
    if (at.type == FLUID_WATER) return down && other != FLUID_WATER;
    // lava: getHeight >= 0.44444445 and the other is water
    {
        f32 h = (fstate(x, y + 1, z).type == FLUID_LAVA) ? 1.0f : at.amount / 9.0f;
        return h >= 0.44444445f && other == FLUID_WATER;
    }
}

// --- FlowingFluid ---

static FState new_liquid(int type, s32 x, s32 y, s32 z) {
    int highest = 0, sources = 0, d, amount;
    FState f = { FLUID_NONE, 0, 0, 0 };

    for (d = 0; d < 4; d++) {
        s32 nx = x + hdir[d][0], nz = z + hdir[d][2];
        FState n = fstate(nx, y, nz);
        if (n.type == type && pass_wall(x, y, z, nx, y, nz)) {
            if (n.source) sources++;
            if (n.amount > highest) highest = n.amount;
        }
    }
    if (sources >= 2 && type == FLUID_WATER) { // canConvertToSource (water only)
        if (solid(x, y - 1, z) || is_source_of(type, x, y - 1, z)) {
            f.type = type;
            f.amount = 8;
            f.source = TRUE;
            return f;
        }
    }
    if (fstate(x, y + 1, z).type == type && pass_wall(x, y, z, x, y + 1, z)) {
        f.type = type;
        f.amount = 8;
        f.falling = TRUE;
        return f;
    }
    amount = highest - drop_off(type);
    if (amount <= 0) return f;
    f.type = type;
    f.amount = amount;
    return f;
}

static int maybe_pass(int type, s32 sx, s32 sy, s32 sz, s32 tx, s32 ty, s32 tz) {
    return !is_source_of(type, tx, ty, tz) && can_hold_any(tx, ty, tz) && pass_wall(sx, sy, sz, tx, ty, tz);
}

static int water_hole_raw(int type, s32 x, s32 y, s32 z) {
    if (!pass_wall(x, y, z, x, y - 1, z)) return FALSE;
    return fstate(x, y - 1, z).type == type ? TRUE : can_hold_any(x, y - 1, z);
}

// FlowingFluid.SpreadContext.holeCache: per spread, by offset from the origin
static int ctx_on;
static s32 ctx_origin[3];
static s8 ctx_hole[16][16]; // -1 unknown, 0 / 1
static s8 ctx_pass[16][16][4]; // maybe_pass from that cell to each side

static void ctx_begin(s32 x, s32 y, s32 z) {
    int i, j, d;
    ctx_on = TRUE;
    ctx_origin[0] = x;
    ctx_origin[1] = y;
    ctx_origin[2] = z;
    for (i = 0; i < 16; i++) for (j = 0; j < 16; j++) {
        ctx_hole[i][j] = -1;
        for (d = 0; d < 4; d++) ctx_pass[i][j][d] = -1;
    }
}

static int water_hole(int type, s32 x, s32 y, s32 z) {
    if (ctx_on && y == ctx_origin[1]) {
        s32 i = x - ctx_origin[0] + 8, j = z - ctx_origin[2] + 8;
        if (i >= 0 && i < 16 && j >= 0 && j < 16) {
            if (ctx_hole[i][j] < 0) ctx_hole[i][j] = (s8)water_hole_raw(type, x, y, z);
            return ctx_hole[i][j];
        }
    }
    return water_hole_raw(type, x, y, z);
}

static int ctx_maybe_pass(int type, s32 x, s32 y, s32 z, int d) {
    s32 tx = x + hdir[d][0], tz = z + hdir[d][2];
    s32 i = x - ctx_origin[0] + 8, j = z - ctx_origin[2] + 8;
    if (ctx_on && y == ctx_origin[1] && i >= 0 && i < 16 && j >= 0 && j < 16) {
        if (ctx_pass[i][j][d] < 0) ctx_pass[i][j][d] = (s8)maybe_pass(type, x, y, z, tx, y, tz);
        return ctx_pass[i][j][d];
    }
    return maybe_pass(type, x, y, z, tx, y, tz);
}

static int slope_distance(int type, s32 x, s32 y, s32 z, int pass, int from) {
    int lowest = 1000, d;
    for (d = 0; d < 4; d++) {
        s32 tx, tz;
        if (d == from) continue;
        tx = x + hdir[d][0];
        tz = z + hdir[d][2];
        if (ctx_maybe_pass(type, x, y, z, d)) {
            if (water_hole(type, tx, y, tz)) return pass;
            if (pass < slope_find(type)) {
                int v = slope_distance(type, tx, y, tz, pass + 1, d ^ 1);
                if (v < lowest) lowest = v;
            }
        }
    }
    return lowest;
}

static void set_fluid(s32 x, s32 y, s32 z, FState f) {
    int lvl;
    if (f.type == FLUID_NONE) {
        world_set(x, y, z, BLOCK_AIR);
        return;
    }
    // getLegacyLevel
    lvl = f.source ? 0 : 8 - (f.amount < 8 ? f.amount : 8) + (f.falling ? 8 : 0);
    world_set(x, y, z, MAKE_CELL(block_of_type(f.type), lvl));
}

static void spread_to(int type, s32 x, s32 y, s32 z, int down, FState target) {
    BlockId b = world_get(x, y, z);
    // LavaFluid.spreadTo: lava falling into water makes stone
    if (down && type == FLUID_LAVA && fstate(x, y, z).type == FLUID_WATER) {
        world_set(x, y, z, BLOCK_STONE);
        return;
    }
    if (b != BLOCK_AIR && !(mc_block(b)->flags & MCB_FLUID)) {
        // beforeDestroyingBlock: water drops it, lava just fizzes it away
        s32 c[3] = { x, y, z };
        if (type == FLUID_WATER) surv_explode_block(c, b, 1.0f);
        else world_set(x, y, z, BLOCK_AIR);
    }
    set_fluid(x, y, z, target);
}

static void spread_sides(FState f, s32 x, s32 y, s32 z) {
    int neighbor = f.amount - drop_off(f.type), d, lowest = 1000;
    int pick[4] = { 0, 0, 0, 0 };
    FState nf[4];

    if (f.falling) neighbor = 7;
    if (neighbor <= 0) return;
    ctx_begin(x, y, z);
    // getSpread
    for (d = 0; d < 4; d++) {
        s32 tx = x + hdir[d][0], tz = z + hdir[d][2];
        if (maybe_pass(f.type, x, y, z, tx, y, tz)) {
            FState n = new_liquid(f.type, tx, y, tz);
            int dist = water_hole(f.type, tx, y, tz) ? 0 : slope_distance(f.type, tx, y, tz, 1, d ^ 1);
            if (dist < lowest) {
                int k;
                for (k = 0; k < 4; k++) pick[k] = 0;
            }
            if (dist <= lowest) {
                if (replaceable_by(fstate(tx, y, tz), tx, y, tz, n.type, FALSE) && n.type != FLUID_NONE) {
                    pick[d] = 1;
                    nf[d] = n;
                }
                lowest = dist;
            }
        }
    }
    ctx_on = FALSE; // (spreading changes the world: the cache ends here)
    for (d = 0; d < 4; d++) {
        if (pick[d]) spread_to(f.type, x + hdir[d][0], y, z + hdir[d][2], FALSE, nf[d]);
    }
}

static int source_neighbors(int type, s32 x, s32 y, s32 z) {
    int d, n = 0;
    for (d = 0; d < 4; d++) n += is_source_of(type, x + hdir[d][0], y, z + hdir[d][2]);
    return n;
}

static void spread(FState f, s32 x, s32 y, s32 z) {
    if (f.type == FLUID_NONE) return;
    if (maybe_pass(f.type, x, y, z, x, y - 1, z)) {
        FState nb = new_liquid(f.type, x, y - 1, z);
        if (nb.type != FLUID_NONE && replaceable_by(fstate(x, y - 1, z), x, y - 1, z, nb.type, TRUE)) {
            spread_to(f.type, x, y - 1, z, TRUE, nb);
            if (source_neighbors(f.type, x, y, z) >= 3) spread_sides(f, x, y, z);
            return;
        }
    }
    if (f.source || !water_hole(f.type, x, y, z)) spread_sides(f, x, y, z);
}

// LiquidBlock.shouldSpreadLiquid: lava touching water hardens (TRUE = it did)
static int lava_meets_water(s32 x, s32 y, s32 z) {
    static const s8 d5[5][3] = { { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 1, 0, 0 }, { -1, 0, 0 } };
    int d;
    for (d = 0; d < 5; d++) {
        if (fluid_type_at(x + d5[d][0], y + d5[d][1], z + d5[d][2]) == FLUID_WATER) {
            world_set(x, y, z, fstate(x, y, z).source ? BLOCK_OBSIDIAN : BLOCK_COBBLESTONE);
            return TRUE;
        }
    }
    return FALSE;
}

static void fluid_tick(s32 x, s32 y, s32 z) {
    FState f = fstate(x, y, z);
    if (f.type == FLUID_LAVA && lava_meets_water(x, y, z)) return;
    if (!f.source) {
        FState n = new_liquid(f.type, x, y, z);
        if (n.type == FLUID_NONE) {
            world_set(x, y, z, BLOCK_AIR);
            return;
        }
        if (n.amount != f.amount || n.falling != f.falling || n.source != f.source) {
            set_fluid(x, y, z, n);
            f = n;
        }
    }
    spread(f, x, y, z);
}

typedef struct {
    Vtx *v;
    u8 *flow;
    int cap;
    int start[2], quads[2]; // lava, water
    int nstill[2];          // per type: the still quads come first, then the flowing ones
    u8 dirty;
} FMesh;
static FMesh fmesh[256];
void world_draw_bury(void *p);
static void mark_fluid_section(s32 x, s32 y, s32 z);
static int section_has_fluid_mesh(s32 x, s32 y, s32 z);

// --- scheduled ticks (LevelTicks): only cells that something changed next to ---

#define SCHED_SLOTS 4096 // power of 2
static struct { s32 x, y, z; u32 when; u8 used; } sched[SCHED_SLOTS];
static int sched_count;

static u32 hash3(s32 x, s32 y, s32 z) {
    return ((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u) & (SCHED_SLOTS - 1);
}

// scheduleTick: one per position (a second one isn't added)
static void schedule(s32 x, s32 y, s32 z, int delay) {
    u32 h = hash3(x, y, z), i;
    for (i = 0; i < 64; i++) {
        u32 k = (h + i) & (SCHED_SLOTS - 1);
        if (sched[k].used && sched[k].x == x && sched[k].y == y && sched[k].z == z) return;
        if (!sched[k].used) {
            sched[k].used = TRUE;
            sched[k].x = x;
            sched[k].y = y;
            sched[k].z = z;
            sched[k].when = now + delay;
            sched_count++;
            return;
        }
    }
}

#define MAX_FLUID_CELLS 4096 // one section
static s32 cells[MAX_FLUID_CELLS][3];
static int cell_count;


// world_set: the changed cell and its neighbours that hold fluid get a tick,
// and the meshes around it are redone if fluid is (or was) there
void fluids_on_change(s32 x, s32 y, s32 z) {
    static const s8 d7[7][3] = { { 0, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 } };
    int d, near_fluid = FALSE;
    for (d = 0; d < 7; d++) {
        s32 nx = x + d7[d][0], ny = y + d7[d][1], nz = z + d7[d][2];
        int t = fluid_type_at(nx, ny, nz);
        if (t != FLUID_NONE) {
            schedule(nx, ny, nz, tick_delay(t));
            near_fluid = TRUE;
        }
    }
    if (near_fluid || section_has_fluid_mesh(x, y, z)) mark_fluid_section(x, y, z);
}

void fluids_clear(void) {
    int i;
    for (i = 0; i < SCHED_SLOTS; i++) sched[i].used = FALSE;
    sched_count = 0;
    for (i = 0; i < 256; i++) {
        world_draw_bury(fmesh[i].v);
        world_draw_bury(fmesh[i].flow);
        fmesh[i].v = NULL;
        fmesh[i].flow = NULL;
        fmesh[i].quads[0] = fmesh[i].quads[1] = 0;
        fmesh[i].dirty = FALSE;
    }
}

// the due ticks, run after collecting them (ticks schedule more ticks)
#define MAX_DUE 512
static s32 due_cells[MAX_DUE][3];

void fluids_tick(void) {
    int i, n = 0;
    PROF_START(t_ft);
    now++;
    if (sched_count > 0) {
        for (i = 0; i < SCHED_SLOTS && n < MAX_DUE; i++) {
            if (sched[i].used && (s32)(now - sched[i].when) >= 0) {
                due_cells[n][0] = sched[i].x;
                due_cells[n][1] = sched[i].y;
                due_cells[n][2] = sched[i].z;
                n++;
            }
        }
        // remove them, then rehash what's left (open addressing)
        if (n > 0) {
            static struct { s32 x, y, z; u32 when; } keep[SCHED_SLOTS];
            int kept = 0;
            for (i = 0; i < SCHED_SLOTS; i++) {
                if (sched[i].used && (s32)(now - sched[i].when) < 0) {
                    keep[kept].x = sched[i].x;
                    keep[kept].y = sched[i].y;
                    keep[kept].z = sched[i].z;
                    keep[kept].when = sched[i].when;
                    kept++;
                }
                sched[i].used = FALSE;
            }
            sched_count = 0;
            for (i = 0; i < kept; i++) schedule(keep[i].x, keep[i].y, keep[i].z, (int)(keep[i].when - now));
        }
        for (i = 0; i < n; i++) {
            if (fluid_type_at(due_cells[i][0], due_cells[i][1], due_cells[i][2]) != FLUID_NONE) {
                fluid_tick(due_cells[i][0], due_cells[i][1], due_cells[i][2]);
            }
        }
    }
    if (n > 0) PROF_END(t_ft, "fluid ticks");
}

// --- FlowingFluid.getFlow ---

static f32 own_height(FState f) {
    return f.type == FLUID_NONE ? 0.0f : f.amount / 9.0f;
}

// isSolidFace: a sturdy face (full blocks, the ground), not this fluid
static int solid_face(int type, s32 x, s32 y, s32 z, int up) {
    if (fstate(x, y, z).type == type) return FALSE;
    if (up) return TRUE;
    return full_block(x, y, z);
}

static void flow_of(int type, s32 x, s32 y, s32 z, f32 out[3]) {
    FState self = fstate(x, y, z);
    f32 fx = 0.0f, fz = 0.0f, len;
    int d;

    out[0] = out[1] = out[2] = 0.0f;
    for (d = 0; d < 4; d++) {
        s32 nx = x + hdir[d][0], nz = z + hdir[d][2];
        FState n = fstate(nx, y, nz);
        f32 dist = 0.0f, nh;
        if (!(n.type == FLUID_NONE || n.type == type)) continue; // affectsFlow
        nh = own_height(n);
        if (nh == 0.0f) {
            if (!full_block(nx, y, nz)) { // (#blocks_fluid_flow ~ solid blocks)
                FState b = fstate(nx, y - 1, nz);
                if (b.type == FLUID_NONE || b.type == type) {
                    nh = own_height(b);
                    if (nh > 0.0f) dist = own_height(self) - (nh - 0.8888889f);
                }
            }
        } else if (nh > 0.0f) {
            dist = own_height(self) - nh;
        }
        if (dist != 0.0f) {
            fx += hdir[d][0] * dist;
            fz += hdir[d][2] * dist;
        }
    }
    len = __builtin_sqrtf(fx * fx + fz * fz);
    if (len > 1e-5f) {
        fx /= len;
        fz /= len;
    } else {
        fx = fz = 0.0f;
    }
    out[0] = fx;
    out[2] = fz;
    if (self.falling) {
        for (d = 0; d < 4; d++) {
            s32 nx = x + hdir[d][0], nz = z + hdir[d][2];
            if (solid_face(type, nx, y, nz, FALSE) || solid_face(type, nx, y + 1, nz, FALSE)) {
                out[1] = -6.0f; // flow.normalize().add(0, -6, 0)
                break;
            }
        }
    }
    len = __builtin_sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len > 1e-5f) {
        out[0] /= len;
        out[1] /= len;
        out[2] /= len;
    }
}

// EntityFluidInteraction: the current over steve's box, CurrentAccumulator.applyTo
// for a Player (averaged, not normalized), WATER_FLOW_SCALE / LAVA_SLOW_FLOW_SCALE
void fluid_push(const f32 pos[3], f32 half_width, f32 height, f32 vel[3]) {
    int type;
    for (type = FLUID_WATER; type <= FLUID_LAVA; type++) {
        s32 x0 = world_to_block(pos[0] - (half_width - 0.001f) * MC_BLOCK), x1 = world_to_block(pos[0] + (half_width - 0.001f) * MC_BLOCK);
        s32 z0 = world_to_block(pos[2] - (half_width - 0.001f) * MC_BLOCK), z1 = world_to_block(pos[2] + (half_width - 0.001f) * MC_BLOCK);
        s32 y0 = world_to_block(pos[1] + 0.001f), y1 = world_to_block(pos[1] + height - 0.001f);
        f32 acc[3] = { 0, 0, 0 }, cur_h = 0.0f, scale = type == FLUID_WATER ? 0.014f : 0.0023333333f;
        int count = 0, k;
        s32 x, y, z;
        for (x = x0; x <= x1; x++) {
            for (y = y0; y <= y1; y++) {
                for (z = z0; z <= z1; z++) {
                    FState f = fstate(x, y, z);
                    f32 top, fl[3];
                    if (f.type != type) continue;
                    top = (y + (fstate(x, y + 1, z).type == type ? 1.0f : own_height(f))) * MC_BLOCK;
                    if (top < pos[1]) continue;
                    if ((top - pos[1]) / MC_BLOCK > cur_h) cur_h = (top - pos[1]) / MC_BLOCK;
                    flow_of(type, x, y, z, fl);
                    if (cur_h < 0.4f) for (k = 0; k < 3; k++) fl[k] *= cur_h;
                    for (k = 0; k < 3; k++) acc[k] += fl[k];
                    count++;
                }
            }
        }
        if (count != 0 && acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2] >= 1e-5f) {
            f32 imp[3], len;
            for (k = 0; k < 3; k++) imp[k] = acc[k] / count * scale;
            len = __builtin_sqrtf(imp[0] * imp[0] + imp[1] * imp[1] + imp[2] * imp[2]);
            if ((vel[0] < 0.003f && vel[0] > -0.003f) && (vel[2] < 0.003f && vel[2] > -0.003f) && len < 0.0045f && len > 0.0f) {
                for (k = 0; k < 3; k++) imp[k] = imp[k] / len * 0.0045f;
            }
            for (k = 0; k < 3; k++) vel[k] += imp[k];
        }
    }
}

// --- steve in grid fluid ---

static f32 cell_height(int type, s32 x, s32 y, s32 z) {
    FState f = fstate(x, y, z);
    if (f.type != type) return 0.0f;
    return fstate(x, y + 1, z).type == type ? 1.0f : f.amount / 9.0f; // getHeight
}

int fluid_surface(int type, const f32 pos[3], f32 height, f32 *surface) {
    s32 x = world_to_block(pos[0]), z = world_to_block(pos[2]);
    s32 y0 = world_to_block(pos[1]), y1 = world_to_block(pos[1] + height), y;
    int found = FALSE;
    for (y = y0; y <= y1; y++) {
        f32 h = cell_height(type, x, y, z);
        if (h > 0.0f) {
            f32 top = (y + h) * MC_BLOCK;
            if (!found || top > *surface) *surface = top;
            found = TRUE;
        }
    }
    return found && *surface > pos[1];
}

// --- buckets ---

// the grid walked like Level.clip: fluids as targets (sources only) or not
static int bucket_clip(const f32 eye[3], const f32 dir[3], f32 reach, int sources, s32 cell[3], s32 normal[3]) {
    f32 step = MC_BLOCK / 16.0f, d;
    s32 last[3] = { world_to_block(eye[0]), world_to_block(eye[1]), world_to_block(eye[2]) };
    for (d = step; d <= reach; d += step) {
        f32 p[3] = { eye[0] + dir[0] * d, eye[1] + dir[1] * d, eye[2] + dir[2] * d };
        s32 c[3] = { world_to_block(p[0]), world_to_block(p[1]), world_to_block(p[2]) };
        BlockId b;
        int k, hit = FALSE;
        if (c[0] == last[0] && c[1] == last[1] && c[2] == last[2]) continue;
        b = world_get(c[0], c[1], c[2]);
        if (b != BLOCK_AIR) {
            if (mc_block(b)->flags & MCB_FLUID) hit = sources && fstate(c[0], c[1], c[2]).source;
            else hit = TRUE; // ClipContext.Block.OUTLINE: torches and dust count too
        } else if (ground(c[0], c[1], c[2])) {
            hit = TRUE;
        }
        if (hit) {
            for (k = 0; k < 3; k++) {
                cell[k] = c[k];
                normal[k] = last[k] - c[k];
            }
            // a diagonal step: keep the largest axis only
            if ((normal[0] != 0) + (normal[1] != 0) + (normal[2] != 0) > 1) {
                f32 ax = dir[0] < 0 ? -dir[0] : dir[0], ay = dir[1] < 0 ? -dir[1] : dir[1], az = dir[2] < 0 ? -dir[2] : dir[2];
                if (ax >= ay && ax >= az) { normal[1] = normal[2] = 0; }
                else if (ay >= az) { normal[0] = normal[2] = 0; }
                else { normal[0] = normal[1] = 0; }
            }
            return TRUE;
        }
        for (k = 0; k < 3; k++) last[k] = c[k];
    }
    return FALSE;
}

// ItemUtils.createFilledResult (limitCreativeStackSize)
static void filled_result(ItemId result) {
    ItemStack *st = &gSurv.inv[gSurv.selected];
    if (gSurv.creative) {
        int i, have = FALSE;
        for (i = 0; i < INV_SIZE; i++) {
            if (gSurv.inv[i].id == result && gSurv.inv[i].count > 0) have = TRUE;
        }
        if (!have) surv_inv_add(result, 1);
        return;
    }
    if (--st->count == 0) {
        st->id = result;
        st->count = 1;
        st->damage = 0;
        return;
    }
    if (surv_inv_add(result, 1) != 0) {
        // (the inventory is full: Player.drop - left out, it stays lost)
    }
}

// bk's own water (lakes, rivers, the moat): its water triangles, the same
// query banjo's swimming and the boats use. nearer than any solid ground in
// the way. bk's water can't be emptied, so it's an endless source
extern void *func_80309B48(f32 *start, f32 *end, f32 *normal, u32 flags);

static int bk_water_in_reach(const f32 eye[3], const f32 dir[3], f32 reach) {
    f32 a[3], b[3], n[3], hit[3], sn[3], wd, sd;
    int k;

    for (k = 0; k < 3; k++) {
        a[k] = eye[k];
        b[k] = eye[k] + dir[k] * reach;
    }
    if (func_80309B48(a, b, n, 0xF800FF0F) == NULL) return FALSE; // (b moves to the hit)
    wd = (b[0] - eye[0]) * (b[0] - eye[0]) + (b[1] - eye[1]) * (b[1] - eye[1]) + (b[2] - eye[2]) * (b[2] - eye[2]);
    for (k = 0; k < 3; k++) {
        a[k] = eye[k];
        b[k] = eye[k] + dir[k] * reach;
    }
    if (steve_raycast(a, b, hit, sn)) {
        sd = (hit[0] - eye[0]) * (hit[0] - eye[0]) + (hit[1] - eye[1]) * (hit[1] - eye[1]) + (hit[2] - eye[2]) * (hit[2] - eye[2]);
        if (sd + 1.0f < wd) return FALSE; // something solid is in front of the water
    }
    return TRUE;
}

int fluid_bucket_use(const f32 eye[3], const f32 dir[3], f32 reach) {
    ItemId held = surv_held_item();
    s32 cell[3], n[3], place[3];
    int k;

    if (held != ITEM_BUCKET && held != ITEM_WATER_BUCKET && held != ITEM_LAVA_BUCKET) return FALSE;
    if (GM_BLOCKS_RESTRICTED()) return FALSE;
    if (held == ITEM_BUCKET) {
        // LiquidBlock.pickupBlock: a source becomes air, the bucket fills
        if (bucket_clip(eye, dir, reach, TRUE, cell, n)) {
            FState f = fstate(cell[0], cell[1], cell[2]);
            if (f.type != FLUID_NONE && f.source) {
                world_set(cell[0], cell[1], cell[2], BLOCK_AIR);
                filled_result(f.type == FLUID_WATER ? ITEM_WATER_BUCKET : ITEM_LAVA_BUCKET);
                // Fluid.getPickupSound
                sound_play_cell(f.type == FLUID_WATER ? SND(ITEM_BUCKET_FILL) : SND(ITEM_BUCKET_FILL_LAVA), cell[0], cell[1], cell[2], 1.0f, 1.0f);
                return TRUE;
            }
        }
        // no grid source: bk's water
        if (bk_water_in_reach(eye, dir, reach)) {
            filled_result(ITEM_WATER_BUCKET);
            sound_play_player(SND(ITEM_BUCKET_FILL), 1.0f, 1.0f);
            return TRUE;
        }
        return FALSE;
    }
    // emptyContents at the clicked block's neighbour (or into a replaceable block)
    if (!bucket_clip(eye, dir, reach, FALSE, cell, n)) return FALSE;
    {
        BlockId b = world_get(cell[0], cell[1], cell[2]);
        int replace_clicked = b != BLOCK_AIR && (mc_block(b)->flags & (MCB_WASHED | MCB_FLUID));
        for (k = 0; k < 3; k++) place[k] = replace_clicked ? cell[k] : cell[k] + n[k];
    }
    {
        BlockId b = world_get(place[0], place[1], place[2]);
        FState f;
        if (b != BLOCK_AIR && !(mc_block(b)->flags & (MCB_WASHED | MCB_FLUID))) return FALSE;
        if (b == BLOCK_AIR && ground(place[0], place[1], place[2])) return FALSE;
        if (b != BLOCK_AIR && !(mc_block(b)->flags & MCB_FLUID)) surv_explode_block(place, b, 1.0f); // destroyBlock with drops
        f.type = held == ITEM_WATER_BUCKET ? FLUID_WATER : FLUID_LAVA;
        f.amount = 8;
        f.source = TRUE;
        f.falling = FALSE;
        set_fluid(place[0], place[1], place[2], f);
        // BucketItem.playEmptySound
        sound_play_cell(held == ITEM_WATER_BUCKET ? SND(ITEM_BUCKET_EMPTY) : SND(ITEM_BUCKET_EMPTY_LAVA), place[0], place[1], place[2], 1.0f, 1.0f);
    }
    // getEmptySuccessItem: creative keeps the full bucket
    if (!gSurv.creative) {
        ItemStack *st = &gSurv.inv[gSurv.selected];
        st->id = ITEM_BUCKET;
        st->count = 1;
        st->damage = 0;
    }
    return TRUE;
}

// --- drawing (LiquidBlockRenderer) ---

// getHeight for the renderer: the fluid's height, 0 for an empty spot, -1 for a solid one
static f32 render_height(int type, s32 x, s32 y, s32 z) {
    FState f = fstate(x, y, z);
    if (f.type == type) return fstate(x, y + 1, z).type == type ? 1.0f : f.amount / 9.0f;
    return full_block(x, y, z) ? -1.0f : 0.0f;
}

static void add_weighted(f32 w[2], f32 h) {
    if (h >= 0.8f) {
        w[0] += h * 10.0f;
        w[1] += 10.0f;
    } else if (h >= 0.0f) {
        w[0] += h;
        w[1] += 1.0f;
    }
}

static f32 average_height(int type, f32 self, f32 first, f32 second, s32 cx, s32 cy, s32 cz) {
    f32 w[2] = { 0.0f, 0.0f };
    if (second >= 1.0f || first >= 1.0f) return 1.0f;
    if (first > 0.0f || second > 0.0f) {
        f32 corner = render_height(type, cx, cy, cz);
        if (corner >= 1.0f) return 1.0f;
        add_weighted(w, corner);
    }
    add_weighted(w, self);
    add_weighted(w, second);
    add_weighted(w, first);
    return w[1] > 0.0f ? w[0] / w[1] : 0.0f;
}


static f32 flow_atan2(f32 y, f32 x) {
    f32 ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, a, r;
    if (ax < 1e-6f && ay < 1e-6f) return 0.0f;
    a = (ax > ay) ? ay / ax : ax / ay;
    r = a * (0.9998660f + a * a * (-0.3302995f + a * a * (0.1801410f + a * a * (-0.0851330f + a * a * 0.0208351f))));
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159265f - r;
    if (y < 0) r = -r;
    return r * 57.2957795f;
}

static void quad(Vtx *v, const f32 p[4][3], const f32 uv[4][2], u32 rgb, u8 a, u8 shade) {
    int i;
    for (i = 0; i < 4; i++) {
        v[i].v.ob[0] = (s16)p[i][0];
        v[i].v.ob[1] = (s16)p[i][1];
        v[i].v.ob[2] = (s16)p[i][2];
        v[i].v.flag = 0;
        v[i].v.tc[0] = (s16)(uv[i][0] * 32.0f);
        v[i].v.tc[1] = (s16)(uv[i][1] * 32.0f);
        v[i].v.cn[0] = (u8)(((rgb >> 16) & 0xFF) * shade / 255);
        v[i].v.cn[1] = (u8)(((rgb >> 8) & 0xFF) * shade / 255);
        v[i].v.cn[2] = (u8)((rgb & 0xFF) * shade / 255);
        v[i].v.cn[3] = a;
    }
}

static u8 *cur_flow;
static int build_type(int type, Vtx *base, int q, int cap, const f32 cam[3]) { // cam = the mesh origin
    int i;
    u32 rgb = type == FLUID_WATER ? 0x3F76E4 : 0xFFFFFF;
    u8 a = 0xFF;

    for (i = 0; i < cell_count && q + 6 <= cap; i++) {
        s32 x = cells[i][0], y = cells[i][1], z = cells[i][2];
        f32 hs, hn, hsouth, he, hw, hne, hnw, hse, hsw, ox, oy, oz;
        int d;
        if (fluid_type_at(x, y, z) != type) continue;
        hs = render_height(type, x, y, z);
        hn = render_height(type, x, y, z - 1);
        hsouth = render_height(type, x, y, z + 1);
        he = render_height(type, x + 1, y, z);
        hw = render_height(type, x - 1, y, z);
        if (hs >= 1.0f) {
            hne = hnw = hse = hsw = 1.0f;
        } else {
            hne = average_height(type, hs, hn, he, x + 1, y, z - 1);
            hnw = average_height(type, hs, hn, hw, x - 1, y, z - 1);
            hse = average_height(type, hs, hsouth, he, x + 1, y, z + 1);
            hsw = average_height(type, hs, hsouth, hw, x - 1, y, z + 1);
        }
        ox = x * MC_BLOCK - cam[0];
        oy = y * MC_BLOCK - cam[1];
        oz = z * MC_BLOCK - cam[2];
        // the top, unless the same fluid is above
        if (fluid_type_at(x, y + 1, z) != type && !full_block(x, y + 1, z)) {
            f32 p[4][3] = {
                { ox, oy + (hnw - 0.001f) * MC_BLOCK, oz },
                { ox, oy + (hsw - 0.001f) * MC_BLOCK, oz + MC_BLOCK },
                { ox + MC_BLOCK, oy + (hse - 0.001f) * MC_BLOCK, oz + MC_BLOCK },
                { ox + MC_BLOCK, oy + (hne - 0.001f) * MC_BLOCK, oz },
            };
            f32 fl[3];
            flow_of(type, x, y, z, fl);
            if (fl[0] == 0.0f && fl[2] == 0.0f) {
                f32 uv[4][2] = { { 0, 0 }, { 0, 16 }, { 16, 16 }, { 16, 0 } };
                quad(&base[q * 4], p, uv, rgb, a, 255);
                cur_flow[q++] = 0;
            } else {
                // the flowing sprite (32 px) turned along the flow
                f32 ang = flow_atan2(fl[2], fl[0]) - 90.0f;
                f32 sn = ml_sin_deg(ang) * 0.25f, cs = ml_cos_deg(ang) * 0.25f;
                f32 uv[4][2] = {
                    { 32.0f * (0.5f + (-cs - sn)), 32.0f * (0.5f + (-cs + sn)) },
                    { 32.0f * (0.5f + (-cs + sn)), 32.0f * (0.5f + (cs + sn)) },
                    { 32.0f * (0.5f + (cs + sn)), 32.0f * (0.5f + (cs - sn)) },
                    { 32.0f * (0.5f + (cs - sn)), 32.0f * (0.5f + (-cs - sn)) },
                };
                quad(&base[q * 4], p, uv, rgb, a, 255);
                cur_flow[q++] = 1;
            }
        }
        // the bottom over air
        if (fluid_type_at(x, y - 1, z) != type && !full_block(x, y - 1, z)) {
            f32 p[4][3] = { { ox, oy, oz + MC_BLOCK }, { ox, oy, oz }, { ox + MC_BLOCK, oy, oz }, { ox + MC_BLOCK, oy, oz + MC_BLOCK } };
            f32 uv[4][2] = { { 0, 16 }, { 0, 0 }, { 16, 0 }, { 16, 16 } };
            quad(&base[q * 4], p, uv, rgb, a, 128);
            cur_flow[q++] = 0;
        }
        // the sides next to anything but the same fluid and full blocks
        for (d = 0; d < 4; d++) {
            s32 nx = x + hdir[d][0], nz = z + hdir[d][2];
            f32 h0, h1, x0, z0, x1, z1;
            if (fluid_type_at(nx, y, nz) == type || full_block(nx, y, nz)) continue;
            switch (d) {
                case 0: h0 = hnw; h1 = hne; x0 = ox; z0 = oz; x1 = ox + MC_BLOCK; z1 = oz; break;                       // north
                case 1: h0 = hse; h1 = hsw; x0 = ox + MC_BLOCK; z0 = oz + MC_BLOCK; x1 = ox; z1 = oz + MC_BLOCK; break; // south
                case 2: h0 = hne; h1 = hse; x0 = ox + MC_BLOCK; z0 = oz; x1 = ox + MC_BLOCK; z1 = oz + MC_BLOCK; break; // east
                default: h0 = hsw; h1 = hnw; x0 = ox; z0 = oz + MC_BLOCK; x1 = ox; z1 = oz; break;                    // west
            }
            {
                f32 p[4][3] = { { x0, oy + h0 * MC_BLOCK, z0 }, { x1, oy + h1 * MC_BLOCK, z1 }, { x1, oy, z1 }, { x0, oy, z0 } };
                // the flowing sprite's top left quarter: u 0..0.5, v (1 - h) * 0.5 .. 0.5
                f32 uv[4][2] = { { 0, (1.0f - h0) * 16.0f }, { 16, (1.0f - h1) * 16.0f }, { 16, 16 }, { 0, 16 } };
                quad(&base[q * 4], p, uv, rgb, a, d < 2 ? 204 : 153);
                cur_flow[q++] = 1;
            }
        }
    }
    return q;
}

// one mesh per grid section (16^3), rebuilt only when its fluid changed
static const f32 mesh_origin[3] = { 0.0f, 0.0f, 0.0f }; // fixed (see the teleport note)

// the fluid cells of one section into cells[]
static void gather_section(Section *s) {
    s32 k;
    cell_count = 0;
    if (s == NULL || s->special_count == 0) return;
    for (k = 0; k < SECTION_VOLUME && cell_count < MAX_FLUID_CELLS; k++) {
        BlockId b = CELL_ID(s->blocks[k]);
        if (b != BLOCK_WATER && b != BLOCK_LAVA) continue;
        cells[cell_count][0] = s->cx * SECTION_SIZE + (k % SECTION_SIZE);
        cells[cell_count][1] = s->cy * SECTION_SIZE + (k / (SECTION_SIZE * SECTION_SIZE));
        cells[cell_count][2] = s->cz * SECTION_SIZE + ((k / SECTION_SIZE) % SECTION_SIZE);
        cell_count++;
    }
}

static void rebuild_section_mesh(int si) {
    FMesh *m = &fmesh[si];
    int pass, q = 0, cap;
    gather_section(world_section_at(si));
    m->dirty = FALSE;
    // the old buffers may still be read by the gpu: buried, freed later
    world_draw_bury(m->v);
    world_draw_bury(m->flow);
    m->v = NULL;
    m->flow = NULL;
    m->quads[0] = m->quads[1] = 0;
    if (cell_count == 0) return;
    cap = cell_count * 6;
    m->v = recomp_alloc(sizeof(Vtx) * 4 * cap);
    m->flow = recomp_alloc(cap);
    if (m->v == NULL || m->flow == NULL) return;
    cur_flow = m->flow;
    for (pass = 0; pass < 2; pass++) {
        int s0, k, w;
        m->start[pass] = q;
        q = build_type(pass == 0 ? FLUID_LAVA : FLUID_WATER, m->v, q, cap, mesh_origin);
        m->quads[pass] = q - m->start[pass];
        // still quads first, then flowing (one texture load each, batched loads)
        s0 = m->start[pass];
        w = s0;
        for (k = s0; k < q; k++) {
            if (m->flow[k] == 0) {
                if (k != w) {
                    Vtx t[4];
                    int j;
                    for (j = 0; j < 4; j++) {
                        t[j] = m->v[w * 4 + j];
                        m->v[w * 4 + j] = m->v[k * 4 + j];
                        m->v[k * 4 + j] = t[j];
                    }
                    m->flow[k] = m->flow[w];
                    m->flow[w] = 0;
                }
                w++;
            }
        }
        m->nstill[pass] = w - s0;
    }
}

// mark the section of a cell (and the ones next to it on a border) for a rebuild
static void mark_fluid_section(s32 x, s32 y, s32 z) {
    s32 lx = x & 15, ly = y & 15, lz = z & 15, dx, dy, dz;
    for (dx = (lx == 0 ? -1 : 0); dx <= (lx == 15 ? 1 : 0); dx++)
    for (dy = (ly == 0 ? -1 : 0); dy <= (ly == 15 ? 1 : 0); dy++)
    for (dz = (lz == 0 ? -1 : 0); dz <= (lz == 15 ? 1 : 0); dz++) {
        int i = world_section_index_at(x + dx, y + dy, z + dz);
        if (i >= 0 && i < 256) fmesh[i].dirty = TRUE;
    }
}

static int section_has_fluid_mesh(s32 x, s32 y, s32 z) {
    int i = world_section_index_at(x, y, z);
    return i >= 0 && i < 256 && fmesh[i].quads[0] + fmesh[i].quads[1] > 0;
}

void fluids_draw(Gfx **gfx, Mtx **mtx, f32 cam[3]) {
#ifdef MC_HAVE_TEXTURES
    int pass, i, n = world_section_count(), any = FALSE;
    PROF_START(t_m);

    if (n > 256) n = 256;
    for (i = 0; i < n; i++) {
        if (fmesh[i].dirty) rebuild_section_mesh(i);
        if (fmesh[i].quads[0] + fmesh[i].quads[1] > 0) any = TRUE;
    }
    PROF_END(t_m, "fluid mesh");
    if (!any) return;
    world_set_texture_state(gfx);
    mlMtxIdent();
    mlMtxTranslate(mesh_origin[0] - cam[0], mesh_origin[1] - cam[1], mesh_origin[2] - cam[2]);
    mlMtxApply(*mtx);
    gEXMatrixGroupSimpleNormal((*gfx)++, MC_ID_FLUIDS, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    // lava solid first, then water blended (RenderType.translucent)
    for (pass = 0; pass < 2; pass++) {
        int type = pass == 0 ? FLUID_LAVA : FLUID_WATER, flowing;
        gDPPipeSync((*gfx)++);
        if (type == FLUID_WATER) {
            gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
            gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
        }
        for (flowing = 0; flowing < 2; flowing++) {
            int loaded = FALSE;
            for (i = 0; i < n; i++) {
                FMesh *m = &fmesh[i];
                int k, end, from;
                if (m->v == NULL || m->quads[pass] == 0) continue;
                from = m->start[pass] + (flowing ? m->nstill[pass] : 0);
                end = flowing ? m->start[pass] + m->quads[pass] : m->start[pass] + m->nstill[pass];
                if (end <= from) continue;
                for (k = from; k < end; k += 8) {
                    int nq = end - k, j;
                    if (nq > 8) nq = 8;
                    if (!loaded) {
                        if (flowing) {
                            gDPLoadTextureBlock((*gfx)++, type == FLUID_WATER ? mc_water_flow : mc_lava_flow, G_IM_FMT_RGBA, G_IM_SIZ_32b, 32, 32, 0,
                                G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
                        } else {
                            gDPLoadTextureBlock((*gfx)++, type == FLUID_WATER ? mc_water_still : mc_lava_still, G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 16, 0,
                                G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
                        }
                        loaded = TRUE;
                    }
                    gSPVertex((*gfx)++, &m->v[k * 4], nq * 4, 0);
                    for (j = 0; j < nq; j++) gSP2Triangles((*gfx)++, j * 4, j * 4 + 1, j * 4 + 2, 0, j * 4, j * 4 + 2, j * 4 + 3, 0);
                }
            }
        }
    }
    gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
    gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    world_set_texture_state(gfx);
#endif
}
