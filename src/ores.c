// what's under bk's ground: minecraft's underground ores and stone variety,
// ported from OreFeature (the vein shape), OreFeatures (sizes, air exposure
// discard chances, what each one replaces), OrePlacements (counts, rarity,
// height ranges) and BiomeDefaultFeatures (the plains list:
// addDefaultUndergroundVariety then addDefaultOres), plus the deepslate layer
// (the overworld surface rule's vertical gradient from y 0 to 8).
//
// bk's levels have no minecraft heights, so a cell's height is its depth
// under the ground over it: the top cell is y 63 and every cell deeper is
// ore_scale() lower (1 like minecraft, the default; or 4 with a quarter of
// the veins, so diamonds are a dozen blocks down at minecraft's density). veins are worked out in that "depth space", per 16 x
// 16 column chunk of the grid like minecraft's chunks, the first time a cell
// near them is asked about. nothing is stored in the save: the same map
// always has the same ores.

#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "bk_externs.h"
#include "world.h"
#include "options.h"
#include "ores.h"

#define SURFACE_Y 63
#define MIN_Y (-64) // DimensionType overworld
#define TOP_Y 319

static s32 ore_scale(void) {
    return option_ore_depth_minecraft() ? 1 : 4;
}

// the minecraft y of a cell d below the surface (d = 0: the top cell)
static s32 mc_y_of(s32 d) {
    return SURFACE_Y - d * ore_scale();
}

// --- a small random source (nextInt / nextFloat / nextDouble) ---

typedef struct {
    u32 s;
} Rand;

static void rand_seed(Rand *r, u32 a, u32 b, u32 c, u32 d) {
    u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du ^ d * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    r->s = h ? h : 0x1234567u;
}

static u32 rand_next(Rand *r) {
    u32 x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

static s32 rand_int(Rand *r, s32 n) {
    return (n <= 1) ? 0 : (s32)(rand_next(r) % (u32)n);
}

static f32 rand_float(Rand *r) {
    return (f32)(rand_next(r) >> 8) / 16777216.0f;
}

// Mth.randomBetweenInclusive
static s32 rand_between(Rand *r, s32 lo, s32 hi) {
    return lo + rand_int(r, hi - lo + 1);
}

// --- the features ---

enum { H_UNIFORM, H_TRAPEZOID };
enum { T_NATURAL_STONE, T_ORE }; // what it replaces (BASE_STONE_OVERWORLD / stone or deepslate ore replaceables)

typedef struct {
    u16 block, deep_block; // the ore and its deepslate version (T_NATURAL_STONE: block only)
    u8 target;
    u8 size;
    f32 discard; // discardChanceOnAirExposure
    s8 count_min, count_max; // CountPlacement (UniformInt for gold_lower); 0, 0 = rarity
    u8 rarity;               // RarityFilter.onAverageOnceEvery
    u8 height;
    s16 lo, hi;              // the height range, minecraft y
} OreDef;

// the plains biome's UNDERGROUND_ORES step, in order. the ones only above y
// 64 (granite/diorite/andesite upper, coal upper, iron upper) can't be under
// bk's ground and are left out
static const OreDef ore_defs[] = {
    { BLOCK_DIRT, 0, T_NATURAL_STONE, 33, 0.0f, 7, 7, 0, H_UNIFORM, 0, 160 },          // ore_dirt
    { BLOCK_GRAVEL, 0, T_NATURAL_STONE, 33, 0.0f, 14, 14, 0, H_UNIFORM, MIN_Y, TOP_Y }, // ore_gravel
    { BLOCK_GRANITE, 0, T_NATURAL_STONE, 64, 0.0f, 2, 2, 0, H_UNIFORM, 0, 60 },        // ore_granite_lower
    { BLOCK_DIORITE, 0, T_NATURAL_STONE, 64, 0.0f, 2, 2, 0, H_UNIFORM, 0, 60 },        // ore_diorite_lower
    { BLOCK_ANDESITE, 0, T_NATURAL_STONE, 64, 0.0f, 2, 2, 0, H_UNIFORM, 0, 60 },       // ore_andesite_lower
    { BLOCK_TUFF, 0, T_NATURAL_STONE, 64, 0.0f, 2, 2, 0, H_UNIFORM, MIN_Y, 0 },        // ore_tuff
    { BLOCK_COAL_ORE, BLOCK_DEEPSLATE_COAL_ORE, T_ORE, 17, 0.5f, 20, 20, 0, H_TRAPEZOID, 0, 192 },          // ore_coal_lower
    { BLOCK_IRON_ORE, BLOCK_DEEPSLATE_IRON_ORE, T_ORE, 9, 0.0f, 10, 10, 0, H_TRAPEZOID, -24, 56 },          // ore_iron_middle
    { BLOCK_IRON_ORE, BLOCK_DEEPSLATE_IRON_ORE, T_ORE, 4, 0.0f, 10, 10, 0, H_UNIFORM, MIN_Y, 72 },          // ore_iron_small
    { BLOCK_GOLD_ORE, BLOCK_DEEPSLATE_GOLD_ORE, T_ORE, 9, 0.5f, 4, 4, 0, H_TRAPEZOID, -64, 32 },            // ore_gold
    { BLOCK_GOLD_ORE, BLOCK_DEEPSLATE_GOLD_ORE, T_ORE, 9, 0.5f, 0, 1, 0, H_UNIFORM, -64, -48 },             // ore_gold_lower
    { BLOCK_REDSTONE_ORE, BLOCK_DEEPSLATE_REDSTONE_ORE, T_ORE, 8, 0.0f, 4, 4, 0, H_UNIFORM, MIN_Y, 15 },    // ore_redstone
    { BLOCK_REDSTONE_ORE, BLOCK_DEEPSLATE_REDSTONE_ORE, T_ORE, 8, 0.0f, 8, 8, 0, H_TRAPEZOID, -96, -32 },   // ore_redstone_lower
    { BLOCK_DIAMOND_ORE, BLOCK_DEEPSLATE_DIAMOND_ORE, T_ORE, 4, 0.5f, 7, 7, 0, H_TRAPEZOID, -144, 16 },     // ore_diamond
    { BLOCK_DIAMOND_ORE, BLOCK_DEEPSLATE_DIAMOND_ORE, T_ORE, 8, 0.5f, 2, 2, 0, H_UNIFORM, -64, -4 },        // ore_diamond_medium
    { BLOCK_DIAMOND_ORE, BLOCK_DEEPSLATE_DIAMOND_ORE, T_ORE, 12, 0.7f, 0, 0, 9, H_TRAPEZOID, -144, 16 },    // ore_diamond_large
    { BLOCK_DIAMOND_ORE, BLOCK_DEEPSLATE_DIAMOND_ORE, T_ORE, 8, 1.0f, 4, 4, 0, H_TRAPEZOID, -144, 16 },     // ore_diamond_buried
    { BLOCK_LAPIS_ORE, BLOCK_DEEPSLATE_LAPIS_ORE, T_ORE, 7, 0.0f, 2, 2, 0, H_TRAPEZOID, -32, 32 },          // ore_lapis
    { BLOCK_LAPIS_ORE, BLOCK_DEEPSLATE_LAPIS_ORE, T_ORE, 7, 1.0f, 4, 4, 0, H_UNIFORM, MIN_Y, 64 },          // ore_lapis_buried
    { BLOCK_COPPER_ORE, BLOCK_DEEPSLATE_COPPER_ORE, T_ORE, 10, 0.0f, 16, 16, 0, H_TRAPEZOID, -16, 112 },    // ore_copper
};
#define ORE_DEF_COUNT ((int)(sizeof(ore_defs) / sizeof(ore_defs[0])))

static s32 gen_scale; // ore_scale() the answers were worked out with

// --- the veins ---
// a chunk's veins are kept as OreFeature.doPlace's spheres (its "data"
// array), and a cell asks them whether it's inside. nothing per cell is
// stored but a cache of answers, so working a chunk out is cheap (a 64 big
// granite blob is 64 spheres, not thousands of cells)

typedef struct {
    f32 x, y, z, r;
} Node;

typedef struct {
    u8 feat;
    u16 first, count;    // nodes
    s32 xs, ys, zs;      // doPlace's xStart / yStart / zStart (cells below them are skipped)
    s32 mn[3], mx[3];    // the cells it can touch
    u32 seed;            // the air exposure discard, per cell
} Vein;

#define MAX_VEINS 160
#define MAX_NODES 2600
typedef struct {
    s32 cx, cz, scale;
    u8 used;
    u32 stamp;
    int nveins, nnodes;
    Vein veins[MAX_VEINS];
    Node nodes[MAX_NODES];
} ChunkVeins;

#define CHUNK_CACHE 48
static ChunkVeins chunk_cache[CHUNK_CACHE];
static u32 chunk_stamp;

// answers already worked out (direct mapped)
#define ANSWER_CACHE 65536 // power of 2
typedef struct {
    s32 x, z;
    s16 d;
    u16 block; // 0 = empty slot
} Answer;
static Answer answers[ANSWER_CACHE];

static u32 cell_hash(s32 x, s32 d, s32 z) {
    return ((u32)x * 73856093u) ^ ((u32)d * 19349663u) ^ ((u32)z * 83492791u);
}

void ores_reset(void) {
    int i;
    for (i = 0; i < CHUNK_CACHE; i++) chunk_cache[i].used = FALSE;
    for (i = 0; i < ANSWER_CACHE; i++) answers[i].block = 0;
}

// Mth.floor
static s32 ifloor(f32 v) {
    s32 i = (s32)v;
    return ((f32)i > v) ? i - 1 : i;
}

// --- OreFeature.place / doPlace's spheres, in depth space (y = -d) ---

static void make_vein(ChunkVeins *ch, Rand *r, int feat, s32 ox, s32 oy, s32 oz) {
    const OreDef *od = &ore_defs[feat];
    s32 size = od->size;
    f32 dir = rand_float(r) * 180.0f; // degrees (random.nextFloat() * PI)
    f32 spread = size / 8.0f;
    s32 max_r = (s32)((size / 16.0f * 2.0f + 1.0f) / 2.0f + 0.999f);
    f32 x0 = ox + ml_sin_deg(dir) * spread, x1 = ox - ml_sin_deg(dir) * spread;
    f32 z0 = oz + ml_cos_deg(dir) * spread, z1 = oz - ml_cos_deg(dir) * spread;
    f32 y0 = (f32)(oy + rand_int(r, 3) - 2), y1 = (f32)(oy + rand_int(r, 3) - 2);
    s32 spread_c = (s32)(spread + 0.999f);
    Vein *v;
    Node *nd;
    int i, j, k, kept = 0;

    if (ch->nveins >= MAX_VEINS || ch->nnodes + size > MAX_NODES) return;
    v = &ch->veins[ch->nveins];
    nd = &ch->nodes[ch->nnodes];
    v->feat = (u8)feat;
    v->first = (u16)ch->nnodes;
    v->xs = ox - spread_c - max_r;
    v->ys = oy - 2 - max_r;
    v->zs = oz - spread_c - max_r;
    v->seed = rand_next(r);
    for (i = 0; i < size; i++) {
        f32 step = (f32)i / size;
        f32 ss = rand_float(r) * size / 16.0f;
        // Mth.sin(PI * step)
        nd[i].r = ((ml_sin_deg(180.0f * step) + 1.0f) * ss + 1.0f) / 2.0f;
        nd[i].x = x0 + (x1 - x0) * step;
        nd[i].y = y0 + (y1 - y0) * step;
        nd[i].z = z0 + (z1 - z0) * step;
    }
    for (i = 0; i < size - 1; i++) {
        if (nd[i].r <= 0.0f) continue;
        for (j = i + 1; j < size; j++) {
            f32 dx, dy, dz, dr;
            if (nd[j].r <= 0.0f) continue;
            dx = nd[i].x - nd[j].x;
            dy = nd[i].y - nd[j].y;
            dz = nd[i].z - nd[j].z;
            dr = nd[i].r - nd[j].r;
            if (dr * dr > dx * dx + dy * dy + dz * dz) {
                if (dr > 0.0f) nd[j].r = -1.0f;
                else nd[i].r = -1.0f;
            }
        }
    }
    // keep the spheres that are left, and the box of cells they reach
    for (k = 0; k < 3; k++) {
        v->mn[k] = 0x7FFFFFFF;
        v->mx[k] = -0x7FFFFFFF;
    }
    for (i = 0; i < size; i++) {
        f32 c[3];
        if (nd[i].r < 0.0f) continue;
        nd[kept] = nd[i];
        c[0] = nd[kept].x; c[1] = nd[kept].y; c[2] = nd[kept].z;
        for (k = 0; k < 3; k++) {
            s32 lo = ifloor(c[k] - nd[kept].r), hi = ifloor(c[k] + nd[kept].r);
            if (lo < v->mn[k]) v->mn[k] = lo;
            if (hi > v->mx[k]) v->mx[k] = hi;
        }
        kept++;
    }
    if (v->mn[0] < v->xs) v->mn[0] = v->xs;
    if (v->mn[1] < v->ys) v->mn[1] = v->ys;
    if (v->mn[2] < v->zs) v->mn[2] = v->zs;
    if (kept == 0 || v->mn[1] > 0) return; // nothing at or under the surface (y <= 0)
    v->count = (u16)kept;
    ch->nnodes += kept;
    ch->nveins++;
}

// one chunk's UNDERGROUND_ORES step: per feature its own random
// (WorldgenRandom.setFeatureSeed), CountPlacement / RarityFilter,
// InSquarePlacement, HeightRangePlacement, then the vein
static void gen_chunk(ChunkVeins *ch, s32 cx, s32 cz) {
    int f;

    ch->cx = cx;
    ch->cz = cz;
    ch->scale = ore_scale();
    ch->used = TRUE;
    ch->nveins = ch->nnodes = 0;
    for (f = 0; f < ORE_DEF_COUNT; f++) {
        const OreDef *od = &ore_defs[f];
        Rand r;
        s32 count, i;

        rand_seed(&r, (u32)map_get(), (u32)cx, (u32)cz, (u32)f);
        if (od->rarity > 0) {
            count = (rand_float(&r) < 1.0f / od->rarity) ? 1 : 0;
        } else {
            count = rand_between(&r, od->count_min, od->count_max);
        }
        for (i = 0; i < count; i++) {
            s32 x = cx * 16 + rand_int(&r, 16), z = cz * 16 + rand_int(&r, 16);
            s32 y, d;

            if (od->height == H_UNIFORM) {
                y = rand_between(&r, od->lo, od->hi);
            } else {
                // TrapezoidHeight with no plateau (triangle)
                s32 range = od->hi - od->lo, ps = range / 2, pe = range - ps;
                y = od->lo + rand_between(&r, 0, pe) + rand_between(&r, 0, ps);
            }
            if (y > SURFACE_Y + 2) continue; // over bk's ground: nothing to replace
            // squeezed 4x shallower, only 1 vein in 4 stays: as much ore per
            // block as minecraft has, just nearer the top
            if (ore_scale() > 1 && rand_int(&r, ore_scale()) != 0) continue;
            d = (SURFACE_Y - y) / ore_scale();
            make_vein(ch, &r, f, x, -d, z);
        }
    }
}

static ChunkVeins *chunk_get(s32 cx, s32 cz) {
    ChunkVeins *oldest = &chunk_cache[0];
    int i;

    chunk_stamp++;
    for (i = 0; i < CHUNK_CACHE; i++) {
        ChunkVeins *c = &chunk_cache[i];
        if (c->used && c->cx == cx && c->cz == cz && c->scale == ore_scale()) {
            c->stamp = chunk_stamp;
            return c;
        }
        if (!c->used || c->stamp < oldest->stamp) oldest = c;
    }
    gen_chunk(oldest, cx, cz);
    oldest->stamp = chunk_stamp;
    return oldest;
}

// which features reach this cell (bit per feature)
static u32 features_at(s32 x, s32 d, s32 z) {
    s32 cx = x >> 4, cz = z >> 4, y = -d, i, j;
    u32 mask = 0;

    for (i = -1; i <= 1; i++) {
        for (j = -1; j <= 1; j++) {
            ChunkVeins *ch = chunk_get(cx + i, cz + j);
            int v;
            for (v = 0; v < ch->nveins; v++) {
                const Vein *vn = &ch->veins[v];
                const OreDef *od;
                int k;

                if (mask & (1u << vn->feat)) continue;
                if (x < vn->mn[0] || x > vn->mx[0] || y < vn->mn[1] || y > vn->mx[1] || z < vn->mn[2] || z > vn->mx[2]) continue;
                for (k = 0; k < vn->count; k++) {
                    const Node *n = &ch->nodes[vn->first + k];
                    f32 xd = (x + 0.5f - n->x) / n->r, yd = (y + 0.5f - n->y) / n->r, zd = (z + 0.5f - n->z) / n->r;
                    if (xd * xd + yd * yd + zd * zd < 1.0f) break;
                }
                if (k == vn->count) continue;
                // canPlaceOre's air check: the cell over the top one is air
                od = &ore_defs[vn->feat];
                if (od->discard > 0.0f && d == 0) {
                    u32 h = (cell_hash(x, d, z) ^ vn->seed) * 0x2C1B3C6Du;
                    h ^= h >> 15;
                    if (od->discard >= 1.0f || (f32)(h >> 8) / 16777216.0f < od->discard) continue;
                }
                mask |= 1u << vn->feat;
            }
        }
    }
    return mask;
}

// a positional random in 0..1 (the deepslate gradient's per block random)
static f32 pos_random(s32 x, s32 d, s32 z) {
    u32 h = cell_hash(x, d, z) * 0x2C1B3C6Du;
    h ^= h >> 15;
    return (f32)(h >> 8) / 16777216.0f;
}

static int is_natural_stone(u16 b) {
    return b == BLOCK_STONE || b == BLOCK_GRANITE || b == BLOCK_DIORITE || b == BLOCK_ANDESITE
        || b == BLOCK_TUFF || b == BLOCK_DEEPSLATE;
}

u16 ores_block_at(s32 x, s32 z, s32 d) {
    s32 y = mc_y_of(d);
    u16 b = BLOCK_STONE;
    Answer *a;
    u32 mask;
    int f;

    if (gen_scale != ore_scale()) {
        ores_reset(); // the depth option changed: everything moves
        gen_scale = ore_scale();
    }
    a = &answers[cell_hash(x, d, z) & (ANSWER_CACHE - 1)];
    if (a->block != 0 && a->x == x && a->z == z && a->d == d) return a->block;
    // VanillaMaterialRules.BEDROCK_FLOOR: verticalGradient("bedrock_floor",
    // bottom, aboveBottom(5)). under the bottom there's nothing but bedrock
    if (y <= MIN_Y || (y < MIN_Y + 5 && pos_random(x ^ 0x5BD1E995, d, z) < (f32)(MIN_Y + 5 - y) / 5.0f)) {
        b = BLOCK_BEDROCK;
        goto done;
    }
    // SurfaceRules verticalGradient("deepslate", 0, 8)
    if (y < 0 || (y < 8 && pos_random(x, d, z) < (8.0f - y) / 8.0f)) b = BLOCK_DEEPSLATE;
    {
        // each feature in order tests what the ones before left
        mask = features_at(x, d, z);
        for (f = 0; mask != 0 && f < ORE_DEF_COUNT; f++) {
            const OreDef *od = &ore_defs[f];
            if (!(mask & (1u << f))) continue;
            if (od->target == T_NATURAL_STONE) {
                if (is_natural_stone(b)) b = od->block;
            } else if (b == BLOCK_DEEPSLATE || b == BLOCK_TUFF) {
                b = od->deep_block; // DEEPSLATE_ORE_REPLACEABLES
            } else if (b == BLOCK_STONE || b == BLOCK_GRANITE || b == BLOCK_DIORITE || b == BLOCK_ANDESITE) {
                b = od->block;      // STONE_ORE_REPLACEABLES
            }
        }
    }
done:
    a->x = x;
    a->z = z;
    a->d = (s16)d;
    a->block = b;
    return b;
}
