// TNT and fire, ported from minecraft 26.3:
//   - TntBlock: primed by flint and steel (useItemOn), by redstone power
//     (onPlace / neighborChanged: hasNeighborSignal), by fire (checkBurnOut)
//     and by explosions (wasExploded: a short fuse, random(fuse / 4) + fuse / 8)
//   - PrimedTnt: fuse 80, gravity 0.04, drag 0.98, on the ground * (0.7, -0.5,
//     0.7), the constructor's little hop, explodes at power 4 at y + 0.0625
//   - TntRenderer: swells in the last 10 ticks (1 + f^4 * 0.3), flashes white
//     every other 5 ticks
//   - ServerExplosion: 16x16x16 edge rays, power * (0.7..1.3), steps of 0.3
//     losing (resistance + 0.3) * 0.3 per block and 0.225 per step, then the
//     entities (steve.c), then the blocks (survival.c: drops with
//     ExplosionDecay)
//   - FireBlock.tick (every 30-39 ticks): ages, burns out, burns flammable
//     neighbours (checkBurnOut, igniting tnt), spreads to air next to
//     flammable blocks (getIgniteOdds), the floor fire model only.
//     BaseFireBlock.entityInside: 1 damage while steve stands in it.

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "prof.h"
#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "terrain.h"
#include "pose.h"
#include "mc_gbi.h"
#include "redstone.h"
#include "particles.h"
#include "tnt.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif
#include "sound.h"

#define MAX_TNT 64

typedef struct {
    int active;
    f32 pos[3], o_pos[3]; // bk units, bottom center
    f32 vel[3];           // blocks per tick
    int fuse;
    int on_ground;
} Primed;

static Primed tnts[MAX_TNT];
static u32 rng = 0x7A7A1234u;

static f32 rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return (f32)(rng >> 8) / 16777216.0f;
}

static int rnd_int(int n) {
    int r;
    if (n <= 1) return 0;
    r = (int)(rnd() * (f32)n);
    return r >= n ? n - 1 : r;
}

void tnt_clear(void) {
    int i;
    for (i = 0; i < MAX_TNT; i++) tnts[i].active = FALSE;
}

void tnt_prime(const s32 cell[3], int fuse) {
    int i, slot = -1;
    f32 rot;
    Primed *t;

    for (i = 0; i < MAX_TNT; i++) {
        if (!tnts[i].active) { slot = i; break; }
    }
    if (slot < 0) return;
    t = &tnts[slot];
    t->active = TRUE;
    sound_play_cell(SND(ENTITY_TNT_PRIMED), cell[0], cell[1], cell[2], 1.0f, 1.0f); // TntBlock.prime
    t->pos[0] = (cell[0] + 0.5f) * MC_BLOCK;
    t->pos[1] = cell[1] * MC_BLOCK;
    t->pos[2] = (cell[2] + 0.5f) * MC_BLOCK;
    for (i = 0; i < 3; i++) t->o_pos[i] = t->pos[i];
    rot = rnd() * 360.0f;
    t->vel[0] = -ml_sin_deg(rot) * 0.02f;
    t->vel[1] = 0.2f;
    t->vel[2] = -ml_cos_deg(rot) * 0.02f;
    t->fuse = fuse;
    t->on_ground = FALSE;
}

// --- the explosion ---

#define MAX_BLOW 1024
static s32 blow[MAX_BLOW][3];
static int blow_count;
// the set as a hash (the rays visit the same cells many times)
#define BLOW_HASH 4096
static s16 blow_hash[BLOW_HASH];

static void add_blow(s32 x, s32 y, s32 z) {
    u32 h = ((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u) & (BLOW_HASH - 1), i;
    for (i = 0; i < BLOW_HASH; i++) {
        s16 *e = &blow_hash[(h + i) & (BLOW_HASH - 1)];
        if (*e == 0) {
            if (blow_count >= MAX_BLOW) return;
            blow[blow_count][0] = x;
            blow[blow_count][1] = y;
            blow[blow_count][2] = z;
            blow_count++;
            *e = (s16)blow_count;
            return;
        }
        if (blow[*e - 1][0] == x && blow[*e - 1][1] == y && blow[*e - 1][2] == z) return;
    }
}

// what's at a cell for the explosion: a grid block, or bk's ground
static BlockId blast_block(s32 x, s32 y, s32 z) {
    BlockId b = world_get(x, y, z);
    if (b != BLOCK_AIR) return b;
    if (!terrain_is_carved(x, y, z) && terrain_cell_in_ground(x, y, z)) return terrain_block_at(x, y, z);
    return BLOCK_AIR;
}

void tnt_explode(const f32 center[3], f32 radius) {
    int xx, yy, zz;
    f32 c[3] = { center[0] / MC_BLOCK, center[1] / MC_BLOCK, center[2] / MC_BLOCK };
    PROF_START(t_all);

    // Explosion's sound (ClientLevel.playLocalSound, 4, (1 + (r - r) * 0.2) * 0.7)
    sound_play_at(SND(ENTITY_GENERIC_EXPLODE), center, 4.0f, (1.0f + (sound_random() - sound_random()) * 0.2f) * 0.7f);

    // calculateExplodedPositions
    blow_count = 0;
    {
        int k;
        for (k = 0; k < BLOW_HASH; k++) blow_hash[k] = 0;
    }
    for (xx = 0; xx < 16; xx++) {
        for (yy = 0; yy < 16; yy++) {
            for (zz = 0; zz < 16; zz++) {
                f32 xd, yd, zd, d, remaining, xp, yp, zp;
                if (!(xx == 0 || xx == 15 || yy == 0 || yy == 15 || zz == 0 || zz == 15)) continue;
                xd = xx / 15.0f * 2.0f - 1.0f;
                yd = yy / 15.0f * 2.0f - 1.0f;
                zd = zz / 15.0f * 2.0f - 1.0f;
                d = __builtin_sqrtf(xd * xd + yd * yd + zd * zd);
                xd /= d;
                yd /= d;
                zd /= d;
                remaining = radius * (0.7f + rnd() * 0.6f);
                xp = c[0];
                yp = c[1];
                zp = c[2];
                while (remaining > 0.0f) {
                    s32 bx = (s32)(xp < 0 ? xp - 1.0f : xp), by = (s32)(yp < 0 ? yp - 1.0f : yp), bz = (s32)(zp < 0 ? zp - 1.0f : zp);
                    BlockId b;
                    if ((f32)bx > xp) bx--;
                    if ((f32)by > yp) by--;
                    if ((f32)bz > zp) bz--;
                    b = blast_block(bx, by, bz);
                    if (b != BLOCK_AIR) {
                        remaining -= (mc_block_resist[b] + 0.3f) * 0.3f;
                        if (remaining > 0.0f) add_blow(bx, by, bz);
                    }
                    xp += xd * 0.3f;
                    yp += yd * 0.3f;
                    zp += zd * 0.3f;
                    remaining -= 0.22500001f;
                }
            }
        }
    }
    // hurtEntities, then interactWithBlocks
    // cells next to the destroyed ground that the ground's surface still runs
    // through (air in the grid's eyes, a slope crossing a corner): cut those
    // too, without drops, or slivers of the level are left floating. only the
    // destroyed cells' neighbours are tested, the rays' air cells aren't
    {
        static const s8 n6[6][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 } };
        int i, d, n = blow_count;
        for (i = 0; i < n; i++) {
            if (world_get(blow[i][0], blow[i][1], blow[i][2]) != BLOCK_AIR) continue; // only around bk ground
            for (d = 0; d < 6; d++) {
                s32 x = blow[i][0] + n6[d][0], y = blow[i][1] + n6[d][1], z = blow[i][2] + n6[d][2];
                if (world_get(x, y, z) != BLOCK_AIR || terrain_is_carved(x, y, z) || terrain_cell_in_ground(x, y, z)) continue;
                if (terrain_cell_touches_ground(x, y, z)) add_blow(x, y, z);
            }
        }
    }
    PROF_END(t_all, "explosion rays");
    {
        PROF_START(t_hurt);
        steve_explosion_hurt(center, radius);
        PROF_END(t_hurt, "explosion entities");
    }
    terrain_carve_begin();
    {
        int i;
        for (i = 0; i < blow_count; i++) {
            s32 *p = blow[i];
            BlockId b = blast_block(p[0], p[1], p[2]);
            if (b == BLOCK_TNT && world_get(p[0], p[1], p[2]) == BLOCK_TNT) {
                // TntBlock.wasExploded: primed with a short fuse, no drop
                world_set(p[0], p[1], p[2], BLOCK_AIR);
                tnt_prime(p, rnd_int(80 / 4) + 80 / 8);
                continue;
            }
            if (b == BLOCK_AIR) {
                terrain_carve(p[0], p[1], p[2]); // a surface-only cell
                continue;
            }
            surv_explode_block(p, b, radius);
        }
    }
    {
        PROF_START(t_end);
        terrain_carve_end();
        PROF_END(t_end, "explosion carving");
    }
    PROF_END(t_all, "explosion total");
    particles_explosion(center);
}

// --- fire ---

static int flame_ignite(BlockId b) {
    return (b < MC_BLOCK_COUNT) ? mc_block_flame[b][0] : 0;
}

static int flame_burn(BlockId b) {
    return (b < MC_BLOCK_COUNT) ? mc_block_flame[b][1] : 0;
}

static const s8 dirs[6][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 } };

// FireBlock.isValidFireLocation: a flammable neighbour
static int valid_fire_location(s32 x, s32 y, s32 z) {
    int d;
    for (d = 0; d < 6; d++) {
        if (flame_ignite(world_get(x + dirs[d][0], y + dirs[d][1], z + dirs[d][2])) > 0) return TRUE;
    }
    return FALSE;
}

// isFaceSturdy(UP) of the block below: full blocks, or bk's ground
static int sturdy_below(s32 x, s32 y, s32 z) {
    BlockId b = world_get(x, y - 1, z);
    if (b != BLOCK_AIR) return (mc_block(b)->flags & MCB_OPAQUE) != 0;
    return !terrain_is_carved(x, y - 1, z) && terrain_cell_in_ground(x, y - 1, z);
}

static int fire_can_survive(s32 x, s32 y, s32 z) {
    return sturdy_below(x, y, z) || valid_fire_location(x, y, z);
}

// FireBlock.getIgniteOdds: air next to flammable blocks
static int ignite_odds(s32 x, s32 y, s32 z) {
    int d, best = 0;
    if (world_get(x, y, z) != BLOCK_AIR) return 0;
    for (d = 0; d < 6; d++) {
        int v = flame_ignite(world_get(x + dirs[d][0], y + dirs[d][1], z + dirs[d][2]));
        if (v > best) best = v;
    }
    return best;
}

static void set_fire(s32 x, s32 y, s32 z, int age) {
    if (!fire_can_survive(x, y, z)) return; // getStateWithAge -> place only where it lasts
    world_set(x, y, z, MAKE_CELL(BLOCK_FIRE, age & 15));
}

// FireBlock.getStateForPlacement / getStateWithAge: on a burnable or sturdy
// floor all sides are false (the floor fire); otherwise each side (and up)
// is true where that neighbour can burn. out: north, east, south, west, up
void fire_sides(s32 x, s32 y, s32 z, u8 out[5]) {
    static const s8 nd[5][3] = { { 0, 0, -1 }, { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 }, { 0, 1, 0 } };
    int i;
    for (i = 0; i < 5; i++) out[i] = 1;
    if (flame_ignite(world_get(x, y - 1, z)) > 0 || sturdy_below(x, y, z)) return;
    for (i = 0; i < 5; i++) {
        if (flame_ignite(world_get(x + nd[i][0], y + nd[i][1], z + nd[i][2])) > 0) out[i] = 2;
    }
}

int fire_place(const s32 cell[3]) {
    // BaseFireBlock.canBePlacedAt: an empty spot where fire can survive
    if (world_get(cell[0], cell[1], cell[2]) != BLOCK_AIR) return FALSE;
    if (!fire_can_survive(cell[0], cell[1], cell[2])) return FALSE;
    world_set(cell[0], cell[1], cell[2], MAKE_CELL(BLOCK_FIRE, 0));
    return TRUE;
}

static void check_burn_out(s32 x, s32 y, s32 z, int chance, int age) {
    BlockId b = world_get(x, y, z);
    int odds = flame_burn(b);
    if (rnd_int(chance) < odds) {
        if (rnd_int(age + 10) < 5) {
            int na = age + rnd_int(5) / 4;
            world_set(x, y, z, BLOCK_AIR);
            set_fire(x, y, z, na > 15 ? 15 : na);
        } else {
            world_set(x, y, z, BLOCK_AIR);
        }
        if (b == BLOCK_TNT) {
            s32 p[3] = { x, y, z };
            tnt_prime(p, 80); // TntBlock.prime
        }
    }
}

#define MAX_FIRES 64
static struct { s32 pos[3]; u32 next; int used; } fires[MAX_FIRES];
static u32 fire_now;

static void fire_tick(s32 x, s32 y, s32 z) {
    u16 cell = world_get_cell(x, y, z);
    int age = CELL_STATE(cell) & 15, na, xx, yy, zz;

    if (!fire_can_survive(x, y, z)) {
        world_set(x, y, z, BLOCK_AIR);
        return;
    }
    na = age + rnd_int(3) / 2;
    if (na > 15) na = 15;
    if (na != age) world_set(x, y, z, MAKE_CELL(BLOCK_FIRE, na));
    // (no infiniburn blocks in the overworld but netherrack, not exported)
    if (!valid_fire_location(x, y, z)) {
        if (!sturdy_below(x, y, z) || age > 3) world_set(x, y, z, BLOCK_AIR);
        return;
    }
    if (age == 15 && rnd_int(4) == 0 && flame_ignite(world_get(x, y - 1, z)) == 0) {
        world_set(x, y, z, BLOCK_AIR);
        return;
    }
    check_burn_out(x + 1, y, z, 300, age);
    check_burn_out(x - 1, y, z, 300, age);
    check_burn_out(x, y - 1, z, 250, age);
    check_burn_out(x, y + 1, z, 250, age);
    check_burn_out(x, y, z - 1, 300, age);
    check_burn_out(x, y, z + 1, 300, age);
    for (xx = -1; xx <= 1; xx++) {
        for (zz = -1; zz <= 1; zz++) {
            for (yy = -1; yy <= 4; yy++) {
                int rate = 100, odds;
                if (xx == 0 && yy == 0 && zz == 0) continue;
                if (yy > 1) rate += (yy - 1) * 100;
                odds = ignite_odds(x + xx, y + yy, z + zz);
                if (odds > 0) {
                    odds = (odds + 40 + 2 * 7) / (age + 30); // difficulty normal (id 2)
                    if (odds > 0 && rnd_int(rate) <= odds) {
                        int sa = age + rnd_int(5) / 4;
                        set_fire(x + xx, y + yy, z + zz, sa > 15 ? 15 : sa);
                    }
                }
            }
        }
    }
}

// the scheduled fire ticks (getFireTickDelay: 30 + random(10)), by position
static void fire_seen(s32 x, s32 y, s32 z) {
    int i, free = -1;
    for (i = 0; i < MAX_FIRES; i++) {
        if (fires[i].used && fires[i].pos[0] == x && fires[i].pos[1] == y && fires[i].pos[2] == z) {
            if ((s32)(fire_now - fires[i].next) >= 0) {
                fires[i].next = fire_now + 30 + rnd_int(10);
                fire_tick(x, y, z);
            }
            return;
        }
        if (!fires[i].used && free < 0) free = i;
    }
    if (free >= 0) {
        // onPlace: the first tick in 30 + random(10)
        fires[free].used = TRUE;
        fires[free].pos[0] = x;
        fires[free].pos[1] = y;
        fires[free].pos[2] = z;
        fires[free].next = fire_now + 30 + rnd_int(10);
    }
}

static void fire_forget_gone(void) {
    int i;
    for (i = 0; i < MAX_FIRES; i++) {
        if (fires[i].used && world_get(fires[i].pos[0], fires[i].pos[1], fires[i].pos[2]) != BLOCK_FIRE) fires[i].used = FALSE;
    }
}

// --- the tick ---

static void primed_tick(Primed *t) {
    f32 hit[3], n[3], start[3], end[3];
    int k;

    for (k = 0; k < 3; k++) t->o_pos[k] = t->pos[k];
    t->vel[1] -= 0.04f; // applyGravity
    // move: like the dropped items, a 0.98 box
    start[0] = t->pos[0];
    start[1] = t->pos[1] + 0.49f * MC_BLOCK;
    start[2] = t->pos[2];
    end[0] = start[0] + t->vel[0] * MC_BLOCK + (t->vel[0] > 0 ? 0.49f : -0.49f) * MC_BLOCK;
    end[1] = start[1];
    end[2] = start[2] + t->vel[2] * MC_BLOCK + (t->vel[2] > 0 ? 0.49f : -0.49f) * MC_BLOCK;
    if ((t->vel[0] != 0.0f || t->vel[2] != 0.0f) && steve_raycast(start, end, hit, n) && n[1] < 0.7f && n[1] > -0.7f) {
        t->vel[0] = t->vel[2] = 0.0f;
    } else {
        t->pos[0] += t->vel[0] * MC_BLOCK;
        t->pos[2] += t->vel[2] * MC_BLOCK;
    }
    start[0] = end[0] = t->pos[0];
    start[2] = end[2] = t->pos[2];
    if (t->vel[1] <= 0.0f) {
        start[1] = t->pos[1] + 0.49f * MC_BLOCK;
        end[1] = t->pos[1] + t->vel[1] * MC_BLOCK - 0.5f;
        if (steve_raycast(start, end, hit, n) && n[1] > 0.5f) {
            t->pos[1] = hit[1];
            t->on_ground = TRUE;
        } else {
            t->pos[1] += t->vel[1] * MC_BLOCK;
            t->on_ground = FALSE;
        }
    } else {
        start[1] = t->pos[1] + 0.49f * MC_BLOCK;
        end[1] = t->pos[1] + 0.98f * MC_BLOCK + t->vel[1] * MC_BLOCK;
        if (steve_raycast(start, end, hit, n)) t->vel[1] = 0.0f;
        else t->pos[1] += t->vel[1] * MC_BLOCK;
        t->on_ground = FALSE;
    }
    for (k = 0; k < 3; k++) t->vel[k] *= 0.98f; // getAirDrag
    if (t->on_ground) {
        t->vel[0] *= 0.7f;
        t->vel[1] *= -0.5f;
        t->vel[2] *= 0.7f;
    }
    if (--t->fuse <= 0) {
        f32 c[3] = { t->pos[0], t->pos[1] + 0.0625f * 0.98f * MC_BLOCK, t->pos[2] };
        t->active = FALSE;
        tnt_explode(c, 4.0f); // DEFAULT_EXPLOSION_POWER
    } else if ((t->fuse & 1) == 0) {
        f32 sp[3] = { t->pos[0], t->pos[1] + 0.5f * MC_BLOCK, t->pos[2] };
        particles_smoke(sp); // ParticleTypes.SMOKE at y + 0.5
    }
}

#define MAX_SCAN 2048
static s32 scan[MAX_SCAN][3];
static int scan_count;
static u32 scan_seen = 0xFFFFFFFF;

void tnt_tick(void) {
    s32 i, n = world_section_count();

    fire_now++;
    terrain_carve_begin(); // every explosion this tick: one flush
    for (i = 0; i < MAX_TNT; i++) {
        if (tnts[i].active) primed_tick(&tnts[i]);
    }
    terrain_carve_end();
    // the grid: powered tnt primes (hasNeighborSignal), fires tick, steve in
    // fire. the tnt / fire cells are found again only when the grid changed
    fire_forget_gone();
    if (scan_seen != world_change_count) {
        scan_count = 0;
        for (i = 0; i < n; i++) {
            Section *s = world_section_at(i);
            s32 k;
            if (s == NULL || s->special_count == 0) continue;
            for (k = 0; k < SECTION_VOLUME && scan_count < MAX_SCAN; k++) {
                BlockId b = CELL_ID(s->blocks[k]);
                if (b != BLOCK_TNT && b != BLOCK_FIRE) continue;
                scan[scan_count][0] = s->cx * SECTION_SIZE + (k % SECTION_SIZE);
                scan[scan_count][1] = s->cy * SECTION_SIZE + (k / (SECTION_SIZE * SECTION_SIZE));
                scan[scan_count][2] = s->cz * SECTION_SIZE + ((k / SECTION_SIZE) % SECTION_SIZE);
                scan_count++;
            }
        }
        scan_seen = world_change_count;
    }
    for (i = 0; i < scan_count; i++) {
        s32 *p = scan[i];
        BlockId b = world_get(p[0], p[1], p[2]);
        if (b == BLOCK_TNT) {
            if (redstone_powered(p)) {
                world_set(p[0], p[1], p[2], BLOCK_AIR);
                tnt_prime(p, 80);
            }
        } else if (b == BLOCK_FIRE) {
            fire_seen(p[0], p[1], p[2]);
            if (world_get(p[0], p[1], p[2]) == BLOCK_FIRE && steve_overlaps_cell(p)) {
                steve_in_fire_block(); // BaseFireBlock.entityInside
            }
        }
    }
}

// --- FlameFeatureRenderer: a burning steve seen from outside ---

#define FLAME_QUADS 8
static Vtx flame_vtx[3][FLAME_QUADS * 4];
static int flame_buf;

static void draw_steve_flames(Gfx **gfx, Mtx **mtx, f32 cam[3]) {
#ifdef MC_HAVE_TEXTURES
    f32 vrot[3], s = STEVE_HALF_WIDTH * 2.0f * 1.4f, r = 0.5f, h, yo = 0.0f, zo = 0.0f;
    Pose p;
    Vtx *v;
    int ss = 0, n = 0, k, i;

    if (!steve_on_fire() || gSteve.first_person || !gSteve.driving) return;
    flame_buf = (flame_buf + 1) % 3;
    v = flame_vtx[flame_buf];
    viewport_getRotation_vec3f(vrot);
    h = (steve_height() / MC_BLOCK) / s;
    pose_identity(&p);
    pose_scale(&p, MC_BLOCK, MC_BLOCK, MC_BLOCK);
    pose_translate(&p, (gSteve.render_pos[0] - cam[0]) / MC_BLOCK, (gSteve.render_pos[1] - cam[1]) / MC_BLOCK,
        (gSteve.render_pos[2] - cam[2]) / MC_BLOCK);
    pose_scale(&p, s, s, s);
    pose_rotate(&p, 1, vrot[1]); // facing the camera
    pose_translate(&p, 0.0f, 0.0f, 0.3f - (s32)h * 0.02f);
    while (h > 0.0f && n < FLAME_QUADS) {
        f32 c[4][3] = { { -r, 0.0f - yo, zo }, { r, 0.0f - yo, zo }, { r, 1.4f - yo, zo }, { -r, 1.4f - yo, zo } };
        int flip = (ss / 2) % 2 == 0;
        // u1 v1, u0 v1, u0 v0, u1 v0 (u swapped every other pair)
        u8 us[4] = { 16, 0, 0, 16 }, vs[4] = { 16, 16, 0, 0 };
        for (k = 0; k < 4; k++) {
            f32 o[3];
            Vtx *q = &v[n * 4 + k];
            pose_apply(&p, c[k], o);
            q->v.ob[0] = (s16)o[0];
            q->v.ob[1] = (s16)o[1];
            q->v.ob[2] = (s16)o[2];
            q->v.flag = 0;
            q->v.tc[0] = (s16)((flip ? 16 - us[k] : us[k]) << 5);
            q->v.tc[1] = (s16)(vs[k] << 5);
            q->v.cn[0] = q->v.cn[1] = q->v.cn[2] = q->v.cn[3] = 0xFF;
        }
        n++;
        h -= 0.45f;
        yo -= 0.45f;
        r *= 0.9f;
        zo -= 0.03f;
        ss++;
    }
    mlMtxIdent();
    mlMtxApply(*mtx);
    gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_TNT_START + 0x100, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    for (i = 0; i < n; i++) {
        // fire_0 on even quads, fire_1 on odd ones
        gDPLoadTextureBlock((*gfx)++, (i % 2 == 0) ? mc_fire_0_tex : mc_fire_1_tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 16, 0,
            G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        gSPVertex((*gfx)++, &v[i * 4], 4, 0);
        gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
    }
    gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
    gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
#endif
}

// --- drawing (TntRenderer) ---

static Vtx tnt_vtx[3][MAX_TNT][24];
static Vtx flash_vtx[3][MAX_TNT][24];
static u32 white_tex[256];
static int tnt_buf;

void tnt_draw(Gfx **gfx, Mtx **mtx, f32 cam[3], f32 pt) {
    int i, any = FALSE;

    draw_steve_flames(gfx, mtx, cam);
    for (i = 0; i < MAX_TNT; i++) {
        if (tnts[i].active) any = TRUE;
    }
    if (!any) return;
    if (white_tex[0] == 0) {
        for (i = 0; i < 256; i++) white_tex[i] = 0xFFFFFFA0; // the white overlay
    }
    tnt_buf = (tnt_buf + 1) % 3;
    for (i = 0; i < MAX_TNT; i++) {
        Primed *t = &tnts[i];
        Pose p;
        f32 rel[3], f, scale;
        int k;

        if (!t->active) continue;
        for (k = 0; k < 3; k++) rel[k] = (t->o_pos[k] + (t->pos[k] - t->o_pos[k]) * pt - cam[k]) / MC_BLOCK;
        pose_identity(&p);
        pose_scale(&p, MC_BLOCK, MC_BLOCK, MC_BLOCK);
        pose_translate(&p, rel[0], rel[1] + 0.5f, rel[2]);
        // fuse < 10: swell
        f = (f32)t->fuse - pt + 1.0f;
        if (f < 10.0f) {
            f32 g = 1.0f - f / 10.0f;
            if (g < 0.0f) g = 0.0f;
            if (g > 1.0f) g = 1.0f;
            g = g * g * g * g;
            scale = 1.0f + g * 0.3f;
            pose_scale(&p, scale, scale, scale);
        }
        pose_translate(&p, -0.5f, -0.5f, -0.5f);
        mlMtxIdent();
        mlMtxApply(*mtx);
        gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_TNT_START + i, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        world_emit_block_cube(gfx, BLOCK_TNT, &p, tnt_vtx[tnt_buf][i], FALSE, NULL);
        if ((t->fuse / 5) % 2 == 0) {
            // renderWhiteSolidBlock: the white flash
            Pose q = p;
            gDPPipeSync((*gfx)++);
            gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
            gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
            pose_translate(&q, -0.002f, -0.002f, -0.002f);
            pose_scale(&q, 1.004f, 1.004f, 1.004f);
            world_emit_block_cube(gfx, BLOCK_TNT, &q, flash_vtx[tnt_buf][i], FALSE, white_tex);
            world_set_texture_state(gfx);
        }
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    }
}
