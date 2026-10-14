// boats, ported from minecraft 26.3:
//   - BoatItem.use: the boat goes where you look (blocks, or a water surface:
//     ClipContext.Fluid.ANY), turned like you, the item used up (survival)
//   - AbstractBoat.tick: getStatus (in / under water, on land, in air),
//     floatBoat (gravity 0.04, buoyancy, frictions 0.05 / 0.9 / 0.45 / land),
//     controlBoat (w +0.04, s -0.005, a / d turn the boat: deltaRotation, a
//     lone turn +0.005), move, paddles (+pi/8 a tick while rowing), 60 ticks
//     under water throws the rider out
//   - positionRider: the rider sits at rideHeight (height / 3) minus the
//     player's vehicle attachment 0.6, turns with the boat, clampRotation
//     keeps the view within 105 degrees of the boat; sneak gets off
//   - VehicleEntity.hurtServer: damage * 10, broken past 40 (creative: at
//     once), drops the boat item; the damage wears off 1 a tick
//   - AbstractBoatRenderer + BoatModel / AbstractBoatModel (paddle animation)
// the water is bk's own (the map model's water triangles, like banjo's floor
// probe) and the grid's. boats aren't saved.

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "items.h"
#include "fluids.h"
#include "pose.h"
#include "mc_gbi.h"
#include "boat.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

#define BOAT_W 1.375f   // EntityType sized(1.375, 0.5625)
#define BOAT_H 0.5625f
#define MAX_BOATS 16

enum { ST_IN_WATER, ST_UNDER_WATER, ST_UNDER_FLOWING_WATER, ST_ON_LAND, ST_IN_AIR };

typedef struct {
    int active;
    s16 map;
    u16 item;             // the boat item (wood type)
    int tex;              // which texture in mc_boat_tex
    f32 pos[3], o_pos[3]; // bk units, bottom middle
    f32 vel[3];           // blocks per tick
    f32 yrot, o_yrot;     // minecraft yRot
    f32 delta_rot;
    int status, old_status;
    f32 water_level;      // blocks
    f32 land_friction;
    f32 out_of_control;
    f32 paddle[2];
    int paddling[2];
    f32 damage;
    int hurt_time, hurt_dir;
} Boat;

static Boat boats[MAX_BOATS];
static int riding = -1;

// the boat items, in the texture order of export_mc_assets.py
static const u16 boat_items[10] = {
    ITEM_OAK_BOAT, ITEM_SPRUCE_BOAT, ITEM_BIRCH_BOAT, ITEM_JUNGLE_BOAT, ITEM_ACACIA_BOAT,
    ITEM_DARK_OAK_BOAT, ITEM_MANGROVE_BOAT, ITEM_CHERRY_BOAT, ITEM_PALE_OAK_BOAT, ITEM_POPLAR_BOAT,
};

int boat_is_item(u16 item) {
    int i;
    for (i = 0; i < 10; i++) if (boat_items[i] == item) return i + 1;
    return 0;
}

int boat_riding(void) {
    return riding >= 0 && boats[riding].active;
}

void boats_clear(void) {
    int i;
    for (i = 0; i < MAX_BOATS; i++) boats[i].active = FALSE;
    riding = -1;
}

// --- bk's water ---

typedef struct {
    f32 n[3];
    f32 y;     // where the query stopped (the surface)
    u32 flags;
    s16 unk14;
    void *unk18;
} WaterHit;
extern void *func_8031BABC(f32 *pos, f32 from, f32 to, u32 flags, WaterHit *out);
extern void *func_80309B48(f32 *start, f32 *end, f32 *normal, u32 flags);

// the water surface over the boat (bk units), bk's or the grid's
static int water_top(const f32 pos[3], f32 *top) {
    f32 p[3] = { pos[0], pos[1], pos[2] };
    WaterHit h;
    int found = FALSE;
    f32 g;
    // like banjo's floor probe: the map model's water, down from above
    if (func_8031BABC(p, 2.0f * MC_BLOCK, -1.5f * MC_BLOCK, 0xF800FF0F, &h) != NULL) {
        *top = h.y;
        found = TRUE;
    }
    {
        f32 q[3] = { pos[0], pos[1] - MC_BLOCK, pos[2] };
        if (fluid_surface(FLUID_WATER, q, 2.5f * MC_BLOCK, &g)) {
            if (!found || g > *top) *top = g;
            found = TRUE;
        }
    }
    return found;
}

// --- AbstractBoat.getStatus ---

static int get_status(Boat *b) {
    f32 top, min_y = b->pos[1] / MC_BLOCK, max_y = min_y + BOAT_H;
    if (water_top(b->pos, &top)) {
        top /= MC_BLOCK;
        if (max_y + 0.001f < top) {
            b->water_level = max_y; // isUnderwater
            return ST_UNDER_WATER;
        }
        if (min_y < top) {
            b->water_level = top; // checkInWater
            return ST_IN_WATER;
        }
    }
    // getGroundFriction: something under the boat (blocks have 0.6)
    {
        f32 a[3] = { b->pos[0], b->pos[1] + 2.0f, b->pos[2] }, e[3] = { b->pos[0], b->pos[1] - 0.001f * MC_BLOCK - 1.0f, b->pos[2] };
        f32 hit[3], n[3];
        if (steve_raycast(a, e, hit, n) && n[1] > 0.5f) {
            b->land_friction = 0.6f;
            return ST_ON_LAND;
        }
    }
    return ST_IN_AIR;
}

// --- AbstractBoat.floatBoat ---

static void float_boat(Boat *b, int ridden) {
    f32 vspeed = -0.04f, buoy = 0.0f, inv = 0.05f;

    if (b->old_status == ST_IN_AIR && b->status != ST_IN_AIR && b->status != ST_ON_LAND) {
        f32 top;
        b->water_level = b->pos[1] / MC_BLOCK + BOAT_H;
        if (water_top(b->pos, &top)) {
            // getWaterLevelAbove - height + 0.101
            b->pos[1] = (top / MC_BLOCK - BOAT_H + 0.101f) * MC_BLOCK;
            b->vel[1] = 0.0f;
        }
        b->status = ST_IN_WATER;
        return;
    }
    if (b->status == ST_IN_WATER) {
        buoy = (b->water_level - b->pos[1] / MC_BLOCK) / BOAT_H;
        inv = 0.9f;
    } else if (b->status == ST_UNDER_FLOWING_WATER) {
        vspeed = -7.0E-4f;
        inv = 0.9f;
    } else if (b->status == ST_UNDER_WATER) {
        buoy = 0.01f;
        inv = 0.45f;
    } else if (b->status == ST_IN_AIR) {
        inv = 0.9f;
    } else if (b->status == ST_ON_LAND) {
        inv = b->land_friction;
        if (ridden) b->land_friction /= 2.0f;
    }
    b->vel[0] *= inv;
    b->vel[1] += vspeed;
    b->vel[2] *= inv;
    b->delta_rot *= inv;
    if (buoy > 0.0f) {
        b->vel[1] = (b->vel[1] + buoy * (0.04f / 0.65f)) * 0.75f;
    }
}

// --- AbstractBoat.controlBoat ---

static void control_boat(Boat *b, int left, int right, int up, int down) {
    f32 acc = 0.0f;
    if (left) b->delta_rot -= 1.0f;
    if (right) b->delta_rot += 1.0f;
    if (right != left && !up && !down) acc += 0.005f;
    b->yrot += b->delta_rot;
    if (up) acc += 0.04f;
    if (down) acc -= 0.005f;
    b->vel[0] += ml_sin_deg(-b->yrot) * acc;
    b->vel[2] += ml_cos_deg(b->yrot) * acc;
    b->paddling[0] = (right && !left) || up;
    b->paddling[1] = (left && !right) || up;
}

// Entity.move, like the other entities: horizontal rays, then the fall
static void move_boat(Boat *b) {
    f32 hit[3], n[3], start[3], end[3];
    f32 half = BOAT_W * 0.5f * MC_BLOCK;

    start[0] = b->pos[0];
    start[1] = b->pos[1] + BOAT_H * 0.5f * MC_BLOCK;
    start[2] = b->pos[2];
    end[0] = start[0] + b->vel[0] * MC_BLOCK + (b->vel[0] > 0 ? half : -half);
    end[1] = start[1];
    end[2] = start[2] + b->vel[2] * MC_BLOCK + (b->vel[2] > 0 ? half : -half);
    if ((b->vel[0] != 0.0f || b->vel[2] != 0.0f) && steve_raycast(start, end, hit, n) && n[1] < 0.7f && n[1] > -0.7f) {
        b->vel[0] = b->vel[2] = 0.0f;
    } else {
        b->pos[0] += b->vel[0] * MC_BLOCK;
        b->pos[2] += b->vel[2] * MC_BLOCK;
    }
    start[0] = end[0] = b->pos[0];
    start[2] = end[2] = b->pos[2];
    if (b->vel[1] <= 0.0f) {
        start[1] = b->pos[1] + BOAT_H * 0.5f * MC_BLOCK;
        end[1] = b->pos[1] + b->vel[1] * MC_BLOCK - 0.5f;
        if (steve_raycast(start, end, hit, n) && n[1] > 0.5f) {
            b->pos[1] = hit[1];
            b->vel[1] = 0.0f;
        } else {
            b->pos[1] += b->vel[1] * MC_BLOCK;
        }
    } else {
        start[1] = b->pos[1] + BOAT_H * 0.5f * MC_BLOCK;
        end[1] = b->pos[1] + BOAT_H * MC_BLOCK + b->vel[1] * MC_BLOCK;
        if (steve_raycast(start, end, hit, n)) b->vel[1] = 0.0f;
        else b->pos[1] += b->vel[1] * MC_BLOCK;
    }
}

static void boat_tick(Boat *b, int ridden, const SteveInput *in) {
    int k;
    for (k = 0; k < 3; k++) b->o_pos[k] = b->pos[k];
    b->o_yrot = b->yrot;
    b->old_status = b->status;
    b->status = get_status(b);
    if (b->status != ST_UNDER_WATER && b->status != ST_UNDER_FLOWING_WATER) b->out_of_control = 0.0f;
    else b->out_of_control += 1.0f;
    if (b->hurt_time > 0) b->hurt_time--;
    if (b->damage > 0.0f) b->damage -= 1.0f;
    if (!ridden) {
        b->paddling[0] = b->paddling[1] = FALSE;
    }
    float_boat(b, ridden);
    if (ridden && in != NULL) {
        f32 sx = in->stick[0], sy = in->stick[1];
        control_boat(b, sx < -0.3f, sx > 0.3f, sy > 0.3f, sy < -0.3f);
    }
    move_boat(b);
    for (k = 0; k < 2; k++) {
        if (b->paddling[k]) b->paddle[k] += 0.39269909f; // pi / 8
        else b->paddle[k] = 0.0f;
    }
}

void boats_tick(void) {
    s16 map = (s16)map_get();
    int i;
    for (i = 0; i < MAX_BOATS; i++) {
        if (!boats[i].active || boats[i].map != map || i == riding) continue;
        boat_tick(&boats[i], FALSE, NULL);
    }
}

// the rider's view turns with the boat like LocalPlayer.getViewYRot for a
// passenger: lerp(yRotO, yRot, partialTick). turn_tick is this tick's turn
// (steve's yaw units), turn_given how much of it the view has already had
static f32 turn_tick, turn_given;

f32 boat_take_turn(f32 pt) {
    f32 want = turn_tick * pt, out = want - turn_given;
    turn_given = want;
    return out;
}

static f32 wrap_deg(f32 a) {
    while (a >= 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

static void dismount(Steve *s) {
    Boat *b = &boats[riding];
    // AbstractBoat.getDismountLocationForPassenger: the way the rider looks,
    // getCollisionHorizontalEscapeVector(width * sqrt 2, rider width, yRot),
    // onto a floor that isn't water; else Entity's: on top of the boat
    f32 yaw = s->yaw + (turn_tick - turn_given), dx = ml_sin_deg(yaw), dz = ml_cos_deg(yaw);
    f32 ax = dx < 0 ? -dx : dx, az = dz < 0 ? -dz : dz, scale = ax > az ? ax : az;
    f32 dist = (BOAT_W * 1.41421356f + STEVE_HALF_WIDTH * 2.0f + 1.0E-5f) / 2.0f;
    f32 tx = b->pos[0] + dx * dist / scale * MC_BLOCK, tz = b->pos[2] + dz * dist / scale * MC_BLOCK;
    f32 a[3] = { tx, b->pos[1] + (BOAT_H + 1.0f) * MC_BLOCK, tz }, e[3] = { tx, b->pos[1] - 1.5f * MC_BLOCK, tz };
    f32 hit[3], n[3], wt;
    int placed = FALSE;
    if (steve_raycast(a, e, hit, n) && n[1] > 0.5f && !(water_top(hit, &wt) && wt > hit[1] + 1.0f)) {
        s32 c[3] = { world_to_block(hit[0]), world_to_block(hit[1] + 1.0f), world_to_block(hit[2]) };
        if (world_get(c[0], c[1], c[2]) == BLOCK_AIR && world_get(c[0], c[1] + 1, c[2]) == BLOCK_AIR) {
            s->pos[0] = hit[0];
            s->pos[1] = hit[1];
            s->pos[2] = hit[2];
            placed = TRUE;
        }
    }
    if (!placed) {
        s->pos[0] = b->pos[0];
        s->pos[1] = b->pos[1] + BOAT_H * MC_BLOCK;
        s->pos[2] = b->pos[2];
    }
    s->prev_pos[0] = s->pos[0]; // (no sliding from the seat to there)
    s->prev_pos[1] = s->pos[1];
    s->prev_pos[2] = s->pos[2];
    s->vel[0] = s->vel[1] = s->vel[2] = 0.0f;
    s->yaw += turn_tick - turn_given;
    turn_tick = turn_given = 0.0f;
    riding = -1;
}

void boat_rider_tick(Steve *s, const SteveInput *in) {
    Boat *b;
    f32 d, t;

    if (!boat_riding()) {
        riding = -1;
        return;
    }
    b = &boats[riding];
    // the vehicle ticks first (Entity.rideTick), then the passenger sits on it
    boat_tick(b, TRUE, in);
    if (b->out_of_control >= 60.0f || in->sneak || b->map != (s16)map_get()) {
        dismount(s); // ejectPassengers / sneak (wantsToStopRiding)
        return;
    }
    // positionRider: (0, rideHeight, 0) minus the player's vehicle attachment (0, 0.6, 0)
    s->pos[0] = b->pos[0];
    s->pos[1] = b->pos[1] + (BOAT_H / 3.0f - 0.6f) * MC_BLOCK;
    s->pos[2] = b->pos[2];
    s->vel[0] = s->vel[1] = s->vel[2] = 0.0f;
    s->fall_distance = 0.0f;
    s->on_ground = FALSE;
    s->sprinting = FALSE;
    // the rider turns with the boat; clampRotation: within 105 degrees of it
    // (steve's yaw is minus minecraft's yRot)
    // (the view gets the turn a frame at a time: boat_take_turn)
    s->yaw += turn_tick - turn_given; // the last tick's turn, all of it now
    turn_tick = -b->delta_rot;
    d = wrap_deg(-(s->yaw + turn_tick) - b->yrot);
    t = d < -105.0f ? -105.0f : (d > 105.0f ? 105.0f : d);
    turn_tick += -(t - d);
    turn_given = 0.0f;
    s->o_body_rot = s->body_rot;
    s->body_rot = b->yrot;
}

// --- picking: a ray against the boats' boxes (bk units) ---

static int pick(const f32 eye[3], const f32 dir[3], f32 reach, f32 *dist_out) {
    s16 map = (s16)map_get();
    int i, best = -1;
    f32 best_t = reach;
    for (i = 0; i < MAX_BOATS; i++) {
        Boat *b = &boats[i];
        f32 lo[3], hi[3], t0 = 0.0f, t1 = reach;
        int k, ok = TRUE;
        if (!b->active || b->map != map || i == riding) continue;
        lo[0] = b->pos[0] - BOAT_W * 0.5f * MC_BLOCK;
        hi[0] = b->pos[0] + BOAT_W * 0.5f * MC_BLOCK;
        lo[1] = b->pos[1];
        hi[1] = b->pos[1] + BOAT_H * MC_BLOCK;
        lo[2] = b->pos[2] - BOAT_W * 0.5f * MC_BLOCK;
        hi[2] = b->pos[2] + BOAT_W * 0.5f * MC_BLOCK;
        for (k = 0; k < 3 && ok; k++) {
            if (dir[k] > -1e-6f && dir[k] < 1e-6f) {
                if (eye[k] < lo[k] || eye[k] > hi[k]) ok = FALSE;
            } else {
                f32 a = (lo[k] - eye[k]) / dir[k], c = (hi[k] - eye[k]) / dir[k];
                if (a > c) { f32 tmp = a; a = c; c = tmp; }
                if (a > t0) t0 = a;
                if (c < t1) t1 = c;
                if (t0 > t1) ok = FALSE;
            }
        }
        if (ok && t0 < best_t) {
            best_t = t0;
            best = i;
        }
    }
    if (dist_out) *dist_out = best_t;
    return best;
}

// Player.interact -> AbstractBoat.interact: ride it (not while sneaking)
int boat_interact(const f32 eye[3], const f32 dir[3], f32 reach) {
    int i = pick(eye, dir, reach, NULL);
    if (i < 0) return FALSE;
    // isSecondaryUseActive (sneaking): PASS, the item in hand gets the click
    if (gSteve.sneaking || boats[i].out_of_control >= 60.0f || GM_SPECTATING()) return FALSE;
    riding = i;
    return TRUE;
}

// VehicleEntity.hurtServer
int boat_attack(const f32 eye[3], const f32 dir[3], f32 reach, f32 damage) {
    int i = pick(eye, dir, reach, NULL);
    Boat *b;
    if (i < 0) return FALSE;
    b = &boats[i];
    b->hurt_dir = -b->hurt_dir;
    if (b->hurt_dir == 0) b->hurt_dir = 1;
    b->hurt_time = 10;
    b->damage += damage * 10.0f;
    if (gSurv.creative) {
        b->active = FALSE; // discard (no drop)
    } else if (b->damage > 40.0f) {
        // destroy: spawnAtLocation(getDropItem())
        s32 cell[3] = { world_to_block(b->pos[0]), world_to_block(b->pos[1] + 1.0f), world_to_block(b->pos[2]) };
        surv_pop_item(cell, b->item, 1);
        b->active = FALSE;
    }
    return TRUE;
}

// BoatItem.use: blocks or any water surface where you look
int boat_item_use(const f32 eye[3], const f32 dir[3], f32 reach) {
    u16 held = surv_held_item();
    f32 end[3], hit[3], n[3], best = reach + 1.0f, place[3];
    int k, slot, kind = boat_is_item(held);

    if (!kind) return FALSE;
    for (k = 0; k < 3; k++) end[k] = eye[k] + dir[k] * reach;
    // the level / blocks
    if (steve_raycast((f32 *)eye, end, hit, n)) {
        f32 dx = hit[0] - eye[0], dy = hit[1] - eye[1], dz = hit[2] - eye[2];
        best = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        for (k = 0; k < 3; k++) place[k] = hit[k];
    }
    // bk's water surfaces (the map model's water triangles)
    {
        f32 s0[3] = { eye[0], eye[1], eye[2] }, e0[3] = { end[0], end[1], end[2] }, wn[3];
        if (func_80309B48(s0, e0, wn, 0xF800FF0F) != NULL) {
            f32 dx = e0[0] - eye[0], dy = e0[1] - eye[1], dz = e0[2] - eye[2];
            f32 d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < best) {
                best = d;
                for (k = 0; k < 3; k++) place[k] = e0[k];
            }
        }
    }
    // the grid's water: step along the ray
    {
        f32 t;
        for (t = 0.0f; t < best && t < reach; t += MC_BLOCK / 8.0f) {
            f32 p[3] = { eye[0] + dir[0] * t, eye[1] + dir[1] * t, eye[2] + dir[2] * t };
            if (fluid_type_at(world_to_block(p[0]), world_to_block(p[1]), world_to_block(p[2])) == FLUID_WATER) {
                best = t;
                for (k = 0; k < 3; k++) place[k] = p[k];
                break;
            }
        }
    }
    if (best > reach) return FALSE;
    for (slot = 0; slot < MAX_BOATS; slot++) if (!boats[slot].active) break;
    if (slot >= MAX_BOATS) return FALSE;
    {
        Boat *b = &boats[slot];
        int j;
        for (j = 0; j < (int)sizeof(Boat); j++) ((u8 *)b)[j] = 0;
        b->active = TRUE;
        b->map = (s16)map_get();
        b->item = held;
        b->tex = kind - 1;
        for (k = 0; k < 3; k++) b->pos[k] = b->o_pos[k] = place[k];
        b->yrot = b->o_yrot = -gSteve.yaw; // setYRot(player.getYRot())
        b->status = b->old_status = ST_IN_AIR;
        b->hurt_dir = 1;
    }
    if (!gSurv.creative) {
        ItemStack *st = &gSurv.inv[gSurv.selected];
        if (--st->count == 0) {
            st->id = 0;
            st->damage = 0;
        }
    }
    return TRUE;
}

// --- drawing (AbstractBoatRenderer, BoatModel) ---

typedef struct {
    s8 u, v;
    f32 x, y, z;
    s8 w, h, d;
} Box;

static const u8 quads[6][4] = { { 5, 4, 0, 1 }, { 2, 3, 7, 6 }, { 0, 4, 7, 3 }, { 1, 0, 3, 2 }, { 5, 1, 2, 6 }, { 4, 5, 6, 7 } };

#define BOAT_VTX (9 * 6 * 4 + 6 * 4) // the parts + the water patch
static const u32 *current_tex; // this boat's 128 x 64 texture
static Vtx boat_vtx[3][MAX_BOATS][BOAT_VTX];
static int boat_buf;

// one ModelPart.Cube (no mirror), its faces lit by which way they face
static int emit_box(Gfx **gfx, const Pose *p, const Box *bx, Vtx *v, int n) {
#ifdef MC_HAVE_TEXTURES
    f32 x0 = bx->x, y0 = bx->y, z0 = bx->z, x1 = x0 + bx->w, y1 = y0 + bx->h, z1 = z0 + bx->d;
    f32 w = bx->w, h = bx->h, d = bx->d;
    f32 u0 = bx->u, u1 = u0 + d, u2 = u0 + d + w, u22 = u0 + d + w + w, u3 = u0 + d + w + d, u4 = u3 + w;
    f32 v0 = bx->v, v1 = v0 + d, v2 = v0 + d + h;
    f32 c8[8][3] = {
        { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
        { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 },
    };
    const f32 uvs[6][4] = {
        { u1, v0, u2, v1 }, { u2, v1, u22, v0 }, { u0, v1, u1, v2 },
        { u1, v1, u2, v2 }, { u2, v1, u3, v2 }, { u3, v1, u4, v2 },
    };
    int f, i;
    for (f = 0; f < 6; f++) {
        f32 tex[4][2], umin, umax, vmin, vmax, pts[4][3], e1[3], e2[3], nr[3], l;
        u8 shade;
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
        for (i = 0; i < 4; i++) pose_apply(p, c8[quads[f][i]], pts[i]);
        for (i = 0; i < 3; i++) {
            e1[i] = pts[1][i] - pts[0][i];
            e2[i] = pts[2][i] - pts[0][i];
        }
        nr[0] = e1[1] * e2[2] - e1[2] * e2[1];
        nr[1] = e1[2] * e2[0] - e1[0] * e2[2];
        nr[2] = e1[0] * e2[1] - e1[1] * e2[0];
        l = __builtin_sqrtf(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
        if (l > 0.0f) for (i = 0; i < 3; i++) nr[i] /= l;
        // a facing based light like the blocks (top bright, bottom dark)
        {
            f32 ny = nr[1] < 0 ? -nr[1] : nr[1], nz = nr[2] < 0 ? -nr[2] : nr[2], nx = nr[0] < 0 ? -nr[0] : nr[0];
            if (ny >= nx && ny >= nz) shade = 220;
            else shade = (nz >= nx) ? 204 : 153;
        }
        for (i = 0; i < 4; i++) {
            q[i].v.ob[0] = (s16)pts[i][0];
            q[i].v.ob[1] = (s16)pts[i][1];
            q[i].v.ob[2] = (s16)pts[i][2];
            q[i].v.flag = 0;
            q[i].v.tc[0] = (s16)(tex[i][0] * 32.0f);
            q[i].v.tc[1] = (s16)(tex[i][1] * 32.0f);
            q[i].v.cn[0] = q[i].v.cn[1] = q[i].v.cn[2] = shade;
            q[i].v.cn[3] = 0xFF;
        }
        gDPLoadTextureTile((*gfx)++, current_tex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 128, 64,
            (s32)umin, (s32)vmin, (s32)umax - 1, (s32)vmax - 1, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR,
            G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        gSPVertex((*gfx)++, q, 4, 0);
        gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
        n++;
    }
#endif
    return n;
}

// RenderTypes.waterMask: depth only (the framebuffer colour is kept, z is
// written), so bk's water plane isn't drawn inside the hull
#define MC_RM_WATER_MASK(clk) (Z_CMP | Z_UPD | IM_RD | CVG_DST_FULL | ZMODE_OPA | FORCE_BL | \
    GBL_c##clk(G_BL_CLR_MEM, G_BL_0, G_BL_CLR_MEM, G_BL_1))

static int emit_mask_box(Gfx **gfx, const Pose *p, Vtx *v, int n) {
    // BoatModel.createWaterPatch: box (-14, -9, -3) 28 x 16 x 3
    f32 x0 = -14, y0 = -9, z0 = -3, x1 = 14, y1 = 7, z1 = 0;
    f32 c8[8][3] = {
        { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
        { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 },
    };
    int f, i;
    for (f = 0; f < 6; f++) {
        Vtx *q = &v[n * 4];
        for (i = 0; i < 4; i++) {
            f32 o[3];
            pose_apply(p, c8[quads[f][i]], o);
            q[i].v.ob[0] = (s16)o[0];
            q[i].v.ob[1] = (s16)o[1];
            q[i].v.ob[2] = (s16)o[2];
            q[i].v.flag = 0;
            q[i].v.tc[0] = q[i].v.tc[1] = 0;
            // opaque alpha: the world pass's alpha compare (threshold 0x80)
            // dropped the whole patch at 0, depth and all
            q[i].v.cn[0] = q[i].v.cn[1] = q[i].v.cn[2] = 0;
            q[i].v.cn[3] = 0xFF;
        }
        gSPVertex((*gfx)++, q, 4, 0);
        gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
        n++;
    }
    return n;
}

static f32 clamped_lerp(f32 f, f32 a, f32 b) {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return a + (b - a) * f;
}

// ModelPart.translateAndRotate: offset, then rotationZYX(z, y, x)
static void part(Pose *p, f32 ox, f32 oy, f32 oz, f32 xr, f32 yr, f32 zr) {
    pose_translate(p, ox, oy, oz);
    if (zr != 0.0f) pose_rotate(p, 2, zr * 57.2957795f);
    if (yr != 0.0f) pose_rotate(p, 1, yr * 57.2957795f);
    if (xr != 0.0f) pose_rotate(p, 0, xr * 57.2957795f);
}

void boats_draw(Gfx **gfx, Mtx **mtx, f32 cam[3], f32 pt) {
#ifdef MC_HAVE_TEXTURES
    s16 map = (s16)map_get();
    int i;
    const f32 PI = 3.14159265f;

    boat_buf = (boat_buf + 1) % 3;
    for (i = 0; i < MAX_BOATS; i++) {
        Boat *b = &boats[i];
        Vtx *v = boat_vtx[boat_buf][i];
        Pose root, p;
        f32 rel[3], yrot, hurt, side_t[2];
        int k, n = 0;

        if (!b->active || b->map != map) continue;
        current_tex = &mc_boat_tex[b->tex * 128 * 64];
        for (k = 0; k < 3; k++) rel[k] = (b->o_pos[k] + (b->pos[k] - b->o_pos[k]) * pt - cam[k]) / MC_BLOCK;
        yrot = b->o_yrot + (b->yrot - b->o_yrot) * pt;
        pose_identity(&root);
        pose_scale(&root, MC_BLOCK, MC_BLOCK, MC_BLOCK);
        pose_translate(&root, rel[0], rel[1], rel[2]);
        // AbstractBoatRenderer.submit
        pose_translate(&root, 0.0f, 0.375f, 0.0f);
        pose_rotate(&root, 1, 180.0f - yrot);
        hurt = (f32)b->hurt_time - pt;
        if (hurt > 0.0f) {
            f32 dmg = b->damage - pt;
            if (dmg < 0.0f) dmg = 0.0f;
            pose_rotate(&root, 0, ml_sin_deg(hurt * 57.2957795f) * hurt * dmg / 10.0f * b->hurt_dir);
        }
        pose_scale(&root, -1.0f, -1.0f, 1.0f);
        pose_rotate(&root, 1, 90.0f);
        pose_scale(&root, 1.0f / 16.0f, 1.0f / 16.0f, 1.0f / 16.0f);

        mlMtxIdent();
        mlMtxApply(*mtx);
        gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_BOAT_START + i, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        {
            static const Box bottom = { 0, 0, -14, -9, -3, 28, 16, 3 };
            static const Box back = { 0, 19, -13, -7, -1, 18, 6, 2 };
            static const Box front = { 0, 27, -8, -7, -1, 16, 6, 2 };
            static const Box right = { 0, 35, -14, -7, -1, 28, 6, 2 };
            static const Box left = { 0, 43, -14, -7, -1, 28, 6, 2 };
            static const Box lp_a = { 62, 0, -1, 0, -5, 2, 2, 18 }, lp_b = { 62, 0, -1.001f, -3, 8, 1, 6, 7 };
            static const Box rp_a = { 62, 20, -1, 0, -5, 2, 2, 18 }, rp_b = { 62, 20, 0.001f, -3, 8, 1, 6, 7 };

            p = root; part(&p, 0, 3, 1, PI / 2, 0, 0); n = emit_box(gfx, &p, &bottom, v, n);
            p = root; part(&p, -15, 4, 4, 0, PI * 3.0f / 2.0f, 0); n = emit_box(gfx, &p, &back, v, n);
            p = root; part(&p, 15, 4, 0, 0, PI / 2, 0); n = emit_box(gfx, &p, &front, v, n);
            p = root; part(&p, 0, 4, -9, 0, PI, 0); n = emit_box(gfx, &p, &right, v, n);
            p = root; part(&p, 0, 4, 9, 0, 0, 0); n = emit_box(gfx, &p, &left, v, n);
            // AbstractBoatModel.animatePaddle, getRowingTime
            for (k = 0; k < 2; k++) {
                side_t[k] = b->paddling[k] ? (b->paddle[k] - PI / 8.0f) + PI / 8.0f * pt : 0.0f;
            }
            {
                f32 xr = clamped_lerp((ml_sin_deg(-side_t[0] * 57.2957795f) + 1.0f) / 2.0f, -PI / 3.0f, -PI / 12.0f);
                f32 yr = clamped_lerp((ml_sin_deg((-side_t[0] + 1.0f) * 57.2957795f) + 1.0f) / 2.0f, -PI / 4.0f, PI / 4.0f);
                p = root; part(&p, 3, -5, 9, xr, yr, PI / 16.0f);
                n = emit_box(gfx, &p, &lp_a, v, n);
                n = emit_box(gfx, &p, &lp_b, v, n);
            }
            {
                f32 xr = clamped_lerp((ml_sin_deg(-side_t[1] * 57.2957795f) + 1.0f) / 2.0f, -PI / 3.0f, -PI / 12.0f);
                f32 yr = PI - clamped_lerp((ml_sin_deg((-side_t[1] + 1.0f) * 57.2957795f) + 1.0f) / 2.0f, -PI / 4.0f, PI / 4.0f);
                p = root; part(&p, 3, -5, -9, xr, yr, PI / 16.0f);
                n = emit_box(gfx, &p, &rp_a, v, n);
                n = emit_box(gfx, &p, &rp_b, v, n);
            }
            // BoatRenderer.submitTypeAdditions: the water patch, unless under water
            if (b->status != ST_UNDER_WATER) {
                gDPPipeSync((*gfx)++);
                gSPTexture((*gfx)++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
                gDPSetCombineMode((*gfx)++, G_CC_SHADE, G_CC_SHADE);
                gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
                gDPSetRenderMode((*gfx)++, MC_RM_WATER_MASK(1), MC_RM_WATER_MASK(2));
                p = root;
                part(&p, 0, -3, 1, PI / 2, 0, 0);
                n = emit_mask_box(gfx, &p, v, n);
                world_set_texture_state(gfx);
            }
        }
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    }
#endif
}
