// m2: steve's movement.
//
// steve runs minecraft's player movement (walk/sprint/sneak/jump, gravity,
// friction) on his own 20 tick/s clock, collides with bk's level using the
// game's own raycast, and drags the hidden banjo along so every bk object
// still sees "banjo" where steve is.
//
// how it plugs into banjo's per-frame update (func_80295C14, see NOTES.md):
//   bakey_update   (return) grab a/b/z for steve, banjo sees them released
//   bastick_update (return) grab the stick for steve, banjo sees it centered
//   bs_updateState          banjo's state machine runs with no input
//   baphysics_update (pre)  notice if the game moved banjo (warp/void out/etc)
//   baphysics_update (ret)  run steve's ticks, put banjo at steve's position
//   func_8029858C    (pre)  copy platform pushes on banjo onto steve
//   func_80293F0C    (ret)  undo banjo's own terrain collision, steve is the boss
//
// when the game needs to drive banjo itself (cutscenes, warps, swimming,
// climbing, getting hit...) steve stops driving and just follows banjo.

#include "options.h"
#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"
#include "PR/os_time.h"
#include "core2/ba/physics.h"
#include "bsint.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "mouse.h"
#include "fluids.h"
#include "boat.h"
#include "xp.h"
#include "survival.h"
#include "terrain.h"
#include "items.h"
#include "screen.h"
#include "hotbars.h"
#include "shield.h"
#include "advancements.h"
#include "sound.h"
#include "shapes.h"
#include "particles.h"
#include "core2/anctrl.h"

Steve gSteve;
SteveInput gSteveInput;

// what banjo filters out of his collision, plus his usual 0x1E0000
static u32 steve_collision_flags(void) {
    return (u32)baMarker_8028D694() | 0x1E0000;
}

// --- small math helpers ---

static f32 mc_absf(f32 x) {
    return (x < 0.0f) ? -x : x;
}

static f32 mc_sqrtf(f32 x) {
    return (x > 0.0f) ? __builtin_sqrtf(x) : 0.0f;
}

// atan2 in degrees, good to ~0.01 deg. no libm here.
static f32 mc_atan2_deg(f32 y, f32 x) {
    f32 ax = mc_absf(x);
    f32 ay = mc_absf(y);
    f32 a, s, r;

    if (ax < 1e-6f && ay < 1e-6f) {
        return 0.0f;
    }
    a = (ax > ay) ? (ay / ax) : (ax / ay);
    s = a * a;
    r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) {
        r = 1.57079637f - r;
    }
    if (x < 0.0f) {
        r = 3.14159274f - r;
    }
    if (y < 0.0f) {
        r = -r;
    }
    return r * 57.2957795f;
}

static void vec3_copy(f32 dst[3], f32 src[3]) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

// --- collision against bk's level ---

// line from start to end. on hit, fills hit point + unit normal.
// frames left where bk sees steve as doing a beak buster (see the
// hitbox_getHitboxForActor patch)
int gSteveSmashFrames;

// frames of a faked b press left (right click interaction)
static int steve_b_pulse;

int steve_raycast(f32 start[3], f32 end[3], f32 out_hit[3], f32 out_normal[3]) {
    f32 s[3];

    vec3_copy(s, start);
    vec3_copy(out_hit, end);
    return func_80320B98(s, out_hit, out_normal, steve_collision_flags()) != NULL;
}

int steve_last_hit_was_level(void) {
    return func_803209EC() != NULL;
}

// bk returns the nearest hit of every provider (level, objects, ours), so
// to see only the level, keep going past anything that isn't it
int steve_raycast_level(f32 start[3], f32 end[3], f32 out_hit[3], f32 out_normal[3]) {
    f32 s[3], d[3], len;
    int tries;

    vec3_copy(s, start);
    d[0] = end[0] - start[0];
    d[1] = end[1] - start[1];
    d[2] = end[2] - start[2];
    len = __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 0.001f) return FALSE;
    for (tries = 0; tries < 6; tries++) {
        if (!steve_raycast(s, end, out_hit, out_normal)) {
            return FALSE;
        }
        if (steve_last_hit_was_level()) {
            return TRUE;
        }
        // step just past the object surface and try again
        s[0] = out_hit[0] + d[0] / len * 2.0f;
        s[1] = out_hit[1] + d[1] / len * 2.0f;
        s[2] = out_hit[2] + d[2] / len * 2.0f;
    }
    return FALSE;
}

// current hitbox height in bk units
f32 steve_height(void) {
    // Player.getDefaultDimensions: swimming 0.6 tall, crouching 1.5, standing 1.8
    if (gSteve.swimming) return 0.6f * MC_BLOCK;
    return (gSteve.sneaking ? STEVE_CROUCH_HEIGHT : STEVE_HEIGHT) * MC_BLOCK;
}

// is there a ceiling within `height` above steve's feet (5 rays)?
static int steve_ceiling_within(f32 pos[3], f32 height) {
    static const f32 offsets[5][2] = { { 0, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    f32 radius = STEVE_HALF_WIDTH * MC_BLOCK * 0.9f;
    int i;

    for (i = 0; i < 5; i++) {
        f32 start[3], end[3], hit[3], n[3];

        start[0] = end[0] = pos[0] + offsets[i][0] * radius;
        start[2] = end[2] = pos[2] + offsets[i][1] * radius;
        start[1] = pos[1] + STEVE_CROUCH_HEIGHT * MC_BLOCK - 2.0f;
        end[1] = pos[1] + height;
        if (steve_raycast(start, end, hit, n)) {
            return TRUE;
        }
    }
    return FALSE;
}

// walls: anything steeper than ~45 degrees. shallower stuff is a slope you walk up.
// (talon trot: once bottles teaches it, steeper slopes are walkable, like banjo)
#define WALL_MAX_NORMAL_Y (steve_talon_trot() ? 0.45f : 0.7f)
// floors: anything flatter than ~60 degrees.
#define FLOOR_MIN_NORMAL_Y (steve_talon_trot() ? 0.4f : 0.5f)

static int steve_talon_trot(void) {
    return ability_isUnlocked(ABILITY_10_TALON_TROT);
}

// move horizontally by (dx, dz) bk units, sliding along walls.
// returns TRUE if a wall stopped part of the movement.
static int steve_move_horizontal(f32 pos[3], f32 dx, f32 dz, f32 vel[3]) {
    f32 heights[4];
    f32 radius = STEVE_HALF_WIDTH * MC_BLOCK;
    int collided = FALSE;
    int iter;

    // heights[0]: the feet. slopes too steep to stand on are walls down there
    // too (the step height ray alone let steve walk into them up to the knees)
    heights[0] = 3.0f;
    heights[1] = STEVE_STEP_HEIGHT * MC_BLOCK + 2.0f;
    heights[2] = steve_height() * 0.6f;
    heights[3] = steve_height() - 4.0f;

    for (iter = 0; iter < 3; iter++) {
        f32 len = mc_sqrtf(dx * dx + dz * dz);
        f32 dir[2], side[2];
        f32 best_t = 1e9f;
        f32 best_n[3];
        int h, l;

        if (len < 0.01f) {
            break;
        }
        dir[0] = dx / len;
        dir[1] = dz / len;
        side[0] = -dir[1];
        side[1] = dir[0];

        // 4 heights x 3 points across steve's front (left edge, middle, right edge)
        for (h = 0; h < 4; h++) {
            for (l = -1; l <= 1; l++) {
                f32 start[3], end[3], hit[3], n[3];
                f32 t;

                start[0] = pos[0] + side[0] * radius * l;
                start[1] = pos[1] + heights[h];
                start[2] = pos[2] + side[1] * radius * l;
                end[0] = start[0] + dir[0] * (len + radius);
                end[1] = start[1];
                end[2] = start[2] + dir[1] * (len + radius);

                if (!steve_raycast(start, end, hit, n)) {
                    continue;
                }
                // floors are the vertical pass's job. faces looking down count
                // as walls though (rock leaning out over steve, "wedges"),
                // only a flat ceiling is skipped
                if (n[1] >= WALL_MAX_NORMAL_Y || n[1] <= -0.95f) {
                    continue;
                }
                if (h == 0 && n[1] >= FLOOR_MIN_NORMAL_Y) {
                    continue; // walkable at the feet: the step-up handles it
                }
                t = (hit[0] - start[0]) * dir[0] + (hit[2] - start[2]) * dir[1];
                if (t < best_t) {
                    best_t = t;
                    vec3_copy(best_n, n);
                }
            }
        }

        if (best_t > len + radius) {
            // nothing in the way
            pos[0] += dx;
            pos[2] += dz;
            break;
        }

        collided = TRUE;
        {
            f32 allowed = best_t - radius - 0.5f;
            f32 moved, rem_x, rem_z, nlen, nx, nz, d;

            if (allowed < 0.0f) {
                allowed = 0.0f;
            }
            moved = (allowed < len) ? allowed : len;
            pos[0] += dir[0] * moved;
            pos[2] += dir[1] * moved;

            // slide what's left along the wall
            nlen = mc_sqrtf(best_n[0] * best_n[0] + best_n[2] * best_n[2]);
            if (nlen < 1e-4f) {
                break;
            }
            nx = best_n[0] / nlen;
            nz = best_n[2] / nlen;
            // make the normal face against our movement
            if (nx * dir[0] + nz * dir[1] > 0.0f) {
                nx = -nx;
                nz = -nz;
            }
            rem_x = dir[0] * (len - moved);
            rem_z = dir[1] * (len - moved);
            d = rem_x * nx + rem_z * nz;
            dx = rem_x - nx * d;
            dz = rem_z - nz * d;

            // minecraft zeroes the velocity going into the wall
            d = vel[0] * nx + vel[2] * nz;
            if (d < 0.0f) {
                vel[0] -= nx * d;
                vel[2] -= nz * d;
            }
        }
    }
    return collided;
}

// Entity.collide's step up: a move along the ground that hit something is
// tried again maxUpStep (0.6) higher, and kept if it got further; then steve
// stands on whatever he stepped onto. (the slope handling in the vertical
// pass only knew walkable slopes, so a vertical edge lower than a step, like
// a shallow hole's rim or a low block, stopped him like a wall.)
static int steve_step_up(f32 pos[3], f32 start[3], f32 dx, f32 dz, f32 vel[3], f32 vel_before[3]) {
    static const f32 offsets[5][2] = { { 0, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    f32 radius = STEVE_HALF_WIDTH * MC_BLOCK, step = STEVE_STEP_HEIGHT * MC_BLOCK;
    f32 up[3], v2[3], best = -1e9f, got, had;
    int i, found = FALSE;

    // room overhead to rise (the box expanded up by the step)
    for (i = 0; i < 5; i++) {
        f32 a[3], b[3], hit[3], n[3];
        a[0] = b[0] = start[0] + offsets[i][0] * radius * 0.9f;
        a[2] = b[2] = start[2] + offsets[i][1] * radius * 0.9f;
        a[1] = start[1] + steve_height();
        b[1] = a[1] + step;
        if (steve_raycast(a, b, hit, n)) return FALSE;
    }
    vec3_copy(up, start);
    up[1] += step;
    vec3_copy(v2, vel_before);
    steve_move_horizontal(up, dx, dz, v2);
    got = (up[0] - start[0]) * (up[0] - start[0]) + (up[2] - start[2]) * (up[2] - start[2]);
    had = (pos[0] - start[0]) * (pos[0] - start[0]) + (pos[2] - start[2]) * (pos[2] - start[2]);
    if (got <= had + 0.01f) return FALSE;
    // down onto the step (stepFromGround minus the rise it didn't need)
    for (i = 0; i < 5; i++) {
        f32 a[3], b[3], hit[3], n[3];
        a[0] = b[0] = up[0] + offsets[i][0] * radius * 0.9f;
        a[2] = b[2] = up[2] + offsets[i][1] * radius * 0.9f;
        a[1] = start[1] + step + 2.0f;
        b[1] = start[1] - 1.0f;
        if (steve_raycast(a, b, hit, n) && n[1] >= FLOOR_MIN_NORMAL_Y && hit[1] > best) {
            best = hit[1];
            found = TRUE;
        }
    }
    if (!found) return FALSE;
    pos[0] = up[0];
    pos[1] = best;
    pos[2] = up[2];
    vec3_copy(vel, v2);
    return TRUE;
}

// move vertically by dy bk units. snaps onto floors (including stepping up to
// STEVE_STEP_HEIGHT onto slopes and ledges) and stops at ceilings.
static void steve_move_vertical(f32 pos[3], f32 dy, f32 vel[3], int *on_ground) {
    f32 radius = STEVE_HALF_WIDTH * MC_BLOCK;
    // a few units more than the wall rays' lowest height, so nothing slips between them
    f32 step = STEVE_STEP_HEIGHT * MC_BLOCK + 4.0f;
    // how far above the feet a floor can be and still push steve up onto it.
    // on land that's the step height. swimming into a slope can sink the feet
    // deeper than that (minecraft's box collision never lets that happen), so
    // in water anything below the head counts.
    f32 floor_reach = gSteve.in_water ? steve_height() - 8.0f : step;
    f32 height = steve_height();
    static const f32 offsets[5][2] = { { 0, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    int i;

    if (dy > 0.0f) {
        // going up: check the ceiling above steve's head
        f32 best = 1e9f;

        for (i = 0; i < 5; i++) {
            f32 start[3], end[3], hit[3], n[3];

            start[0] = pos[0] + offsets[i][0] * radius * 0.9f;
            start[1] = pos[1] + height;
            start[2] = pos[2] + offsets[i][1] * radius * 0.9f;
            end[0] = start[0];
            end[1] = start[1] + dy;
            end[2] = start[2];
            if (steve_raycast(start, end, hit, n) && hit[1] < best) {
                best = hit[1];
            }
        }
        if (best < 1e8f) {
            pos[1] = best - height - 0.5f;
            vel[1] = 0.0f;
        } else {
            pos[1] += dy;
        }
        *on_ground = FALSE;

        // going up slowly (swimming) next to a slope can leave the feet inside
        // it. minecraft's box collision never allows that, so push back out.
        {
            f32 floor_best = -1e9f;

            for (i = 0; i < 5; i++) {
                f32 start[3], end[3], hit[3], n[3];

                start[0] = end[0] = pos[0] + offsets[i][0] * radius * 0.9f;
                start[2] = end[2] = pos[2] + offsets[i][1] * radius * 0.9f;
                start[1] = pos[1] + floor_reach;
                end[1] = pos[1];
                if (steve_raycast(start, end, hit, n) && n[1] >= FLOOR_MIN_NORMAL_Y && hit[1] > floor_best) {
                    floor_best = hit[1];
                }
            }
            if (floor_best > pos[1]) {
                pos[1] = floor_best;
            }
        }
        return;
    }

    // going down (or standing): find the highest floor under steve's footprint,
    // starting step height above his feet so slopes and small ledges work
    {
        f32 best = -1e9f;
        int found = FALSE;
        // the highest surface too steep to stand on that the fall would go
        // through (a pillar side leaning out: mm's dirt pillars)
        f32 steep_best = -1e9f, steep_n[3] = { 0.0f, 1.0f, 0.0f };

        for (i = 0; i < 5; i++) {
            f32 start[3], end[3], hit[3], n[3];

            start[0] = pos[0] + offsets[i][0] * radius * 0.9f;
            start[1] = pos[1] + floor_reach;
            start[2] = pos[2] + offsets[i][1] * radius * 0.9f;
            end[0] = start[0];
            // when already on the ground, also look down a step so steve follows
            // slopes downhill instead of bouncing off them
            end[1] = pos[1] + dy - (*on_ground ? step : 1.0f);
            end[2] = start[2];
            if (!steve_raycast(start, end, hit, n)) {
                continue;
            }
            if (n[1] < FLOOR_MIN_NORMAL_Y) {
                // only below the feet: one higher up is a wall beside steve
                if (n[1] > 0.05f && hit[1] <= pos[1] + 1.0f && hit[1] > steep_best) {
                    steep_best = hit[1];
                    steep_n[0] = n[0];
                    steep_n[1] = n[1];
                    steep_n[2] = n[2];
                }
                continue;
            }
            if (hit[1] > best) {
                best = hit[1];
                found = TRUE;
            }
        }

        // falling onto a steep face: minecraft's box can't pass through any
        // surface, and a fall can be ~4 blocks a tick, so steve went straight
        // through a leaning pillar side and had no floor inside it. land on
        // the face and slide off it instead: the move projected onto it
        // (v - (v.n) n with v = (0, dy, 0)), sideways out along its normal
        if (steep_best > -1e8f && (!found || steep_best > best + 4.0f)) { // (a few units: a slope's foot meets its floor)
            f32 ny = steep_n[1];
            f32 rest = dy - (steep_best - pos[1]); // (negative) the fall left below the face
            if (rest > 0.0f) rest = 0.0f;
            pos[0] += -rest * ny * steep_n[0] + steep_n[0] * 1.0f;
            pos[2] += -rest * ny * steep_n[2] + steep_n[2] * 1.0f;
            pos[1] = steep_best + rest * (1.0f - ny * ny);
            vel[1] *= 1.0f - ny * ny;
            *on_ground = FALSE;
            return;
        }

        // like minecraft, stepping up is allowed while on the ground or falling
        if (found) {
            pos[1] = best;
            vel[1] = 0.0f;
            *on_ground = TRUE;
        } else {
            pos[1] += dy;
            *on_ground = FALSE;
        }
    }
}

// --- health ---
// steve has minecraft's 20 hp. bk keeps its honeycombs (enemies, pickups and
// death all go through them), each honeycomb is worth 20 / max honeycombs hp.
// bk changing the honeycombs moves steve's hp, minecraft damage (falling)
// takes honeycombs away.

// --- getting hurt ---
// what minecraft does when the player takes damage (LivingEntity.hurtServer /
// dealDefaultKnockback / knockback, ServerPlayer.indicateDamage, hurtTime):
// 10 ticks of hurt time (red model, hurt camera), 20 of invulnerability, a
// 0.4 knockback away from the attacker, and the direction for the camera tilt.
// bk enemies hit through baMarker_8028D7B8, which then calls func_8028F428 to
// put banjo into his hurt state; while steve plays that's replaced (below) so
// steve gets minecraft's reaction instead.

static int steve_contact_did_damage(void);
s32 baiFrame_getState(void);
static int hurt_src_valid;
static f32 hurt_src[3];

// Player.getHurtSound by damage type (DamageEffects): set just before a
// drowning / burning hurt, back to HURT after
static int hurt_effect;
enum { HURT_PLAIN, HURT_DROWN, HURT_FIRE };

static void steve_on_damaged(void) {
    Steve *s = &gSteve;
    f32 xd = 0.0f, zd = 0.0f;

    // LivingEntity.handleDamageEvent: the hurt sound (the death sound when
    // it's the last one), getSoundVolume 1, getVoicePitch
    {
        f32 pitch = (sound_random() - sound_random()) * 0.2f + 1.0f;
        if (s->health <= 0.0f || item_getCount(ITEM_14_HEALTH) <= 0) {
            sound_play_player(SND(ENTITY_PLAYER_DEATH), 1.0f, pitch);
        } else if (hurt_effect == HURT_DROWN) {
            sound_play_player(SND(ENTITY_PLAYER_HURT_DROWN), 1.0f, pitch);
        } else if (hurt_effect == HURT_FIRE) {
            sound_play_player(SND(ENTITY_PLAYER_HURT_ON_FIRE), 1.0f, pitch);
        } else {
            sound_play_player(SND(ENTITY_PLAYER_HURT), 1.0f, pitch);
        }
        hurt_effect = HURT_PLAIN;
    }

    s->hurt_time = 10;
    s->o_hurt_time = 10.0f;
    if (hurt_src_valid) {
        xd = hurt_src[0] - s->pos[0];
        zd = hurt_src[2] - s->pos[2];
        hurt_src_valid = FALSE;
        // knockback(0.4, xd, zd)
        {
            f32 len = mc_sqrtf(xd * xd + zd * zd);
            if (len > 1e-3f) {
                f32 kx = xd / len * 0.4f, kz = zd / len * 0.4f;
                s->vel[0] = s->vel[0] * 0.5f - kx;
                s->vel[2] = s->vel[2] * 0.5f - kz;
                if (s->on_ground) {
                    f32 vy = s->vel[1] * 0.5f + 0.4f;
                    s->vel[1] = (vy < 0.4f) ? vy : 0.4f;
                }
            }
        }
    }
    // indicateDamage: atan2(zd, xd) in degrees - yRot (yRot = -bk yaw)
    s->hurt_dir = mc_atan2_deg(zd, xd) + s->yaw;
}

// banjo's hurt state trigger (code_7060.c). same as the original when steve
// isn't playing. a patch because it has to stop banjo's hurt state.
RECOMP_PATCH bool func_8028F428(s32 arg0, ActorMarker *marker) {
    if (gSteve.enabled && gSteve.driving) {
        if (!steve_contact_did_damage()) {
            return FALSE; // a 0 damage bump: minecraft doesn't react to those
        }
        if (marker != NULL) {
            Actor *a = marker_getActor(marker);
            if (a != NULL) {
                hurt_src[0] = a->position[0];
                hurt_src[1] = a->position[1];
                hurt_src[2] = a->position[2];
                hurt_src_valid = TRUE;
            }
        }
        baiFrame_startWithValue(1.0f); // 20 ticks, like invulnerableTime
        return FALSE;
    }
    func_80296CB4(arg0);
    func_80296CA8(marker);
    return bs_checkInterrupt(BS_INTR_24) == 2;
}

// most of bk's hurting hits take the honeycomb inside banjo's interrupt
// handler (code_EF50.c, func_8029xxxx switch), and that handler also puts
// banjo into his hurt state, which would take control from steve. so while
// steve plays, these triggers do the handler's damage part themselves:
// respect the invulnerability frames, take one honeycomb, start 1s of
// invulnerability and remember where the hit came from (for knockback and the
// hurt camera). a hit that would take the last honeycomb runs bk's original
// code so the death plays normally.
// returns: 1 handled (hit), 0 ignored (invulnerable), -1 let bk handle it.
void item_dec(s32 item);
bool baMarker_isCollidable(void);

static int killed_by_mob;

static int steve_take_bk_hit(int needs_vulnerable, int needs_collidable, ActorMarker *marker, f32 *pos) {
    if (!gSteve.enabled || !gSteve.driving) {
        return -1;
    }
    if (gSurv.creative || GM_SPECTATING()) {
        return 0; // Abilities.invulnerable: the hit does nothing
    }
    if (needs_vulnerable && baiFrame_getState() == 3) {
        return 0;
    }
    if (needs_collidable && !baMarker_isCollidable()) {
        return 0;
    }
    if (item_getCount(ITEM_14_HEALTH) <= 1) {
        // (entity_killed_player when it lands: the mob the killer counts as)
        killed_by_mob = 0;
        if (marker != NULL && marker_getActor(marker) != NULL) killed_by_mob = surv_mob_mask(marker_getActor(marker)->modelCacheIndex);
        return -1; // the last honeycomb: bk's own hurt + death
    }
    if (marker != NULL) {
        Actor *a = marker_getActor(marker);
        if (a != NULL) {
            hurt_src[0] = a->position[0];
            hurt_src[1] = a->position[1];
            hurt_src[2] = a->position[2];
            hurt_src_valid = TRUE;
        }
    } else if (pos != NULL) {
        hurt_src[0] = pos[0];
        hurt_src[1] = pos[1];
        hurt_src[2] = pos[2];
        hurt_src_valid = TRUE;
    }
    item_dec(ITEM_14_HEALTH);
    baiFrame_startWithValue(1.0f);
    return 1;
}

// BS_INTR_21: big hits (attack types 1-5) and enemy-caused death
RECOMP_PATCH bool func_8028F55C(s32 arg0, ActorMarker *marker) {
    int r = steve_take_bk_hit(TRUE, FALSE, marker, NULL);
    if (r >= 0) return FALSE;
    func_80296CB4(arg0);
    func_80296CA8(marker);
    return bs_checkInterrupt(BS_INTR_21) == 2;
}

// BS_INTR_33: same, without the invulnerability check
RECOMP_PATCH bool func_8028F590(s32 arg0, ActorMarker *marker) {
    int r = steve_take_bk_hit(FALSE, FALSE, marker, NULL);
    if (r >= 0) return FALSE;
    func_80296CB4(arg0);
    func_80296CA8(marker);
    return bs_checkInterrupt(BS_INTR_33) == 2;
}

// BS_INTR_20: hit from a position
RECOMP_PATCH bool func_8028F5C4(s32 arg0, f32 arg1[3]) {
    int r = steve_take_bk_hit(TRUE, FALSE, NULL, arg1);
    if (r >= 0) return FALSE;
    func_80296CB4(arg0);
    func_80296CC0(arg1);
    return bs_checkInterrupt(BS_INTR_20) == 2;
}

// BS_INTR_2E: knocked away from a position (func_8028F5F8 calls this)
void func_80296C90(f32 arg0);
void func_80296C9C(f32 arg0);
RECOMP_PATCH bool func_8028F620(f32 arg0[3], f32 arg1, f32 arg2) {
    int r = steve_take_bk_hit(FALSE, FALSE, NULL, arg0);
    if (r >= 0) return FALSE;
    func_80296C90(arg1);
    func_80296C9C(arg2);
    func_80296CB4(0x10);
    func_80296CC0(arg0);
    return bs_checkInterrupt(BS_INTR_2E) == 2;
}

// BS_INTR_1F_HAZARD: thorns, spikes and other hurting ground
RECOMP_PATCH bool player_checkHazardInterrupt(s32 arg0) {
    int r = steve_take_bk_hit(TRUE, TRUE, NULL, NULL);
    if (r >= 0) return r == 1;
    func_80296CB4(arg0);
    return bs_checkInterrupt(BS_INTR_1F_HAZARD) == 2;
}

// BS_INTR_31
RECOMP_PATCH bool func_8028F530(s32 arg0) {
    int r = steve_take_bk_hit(FALSE, FALSE, NULL, NULL);
    if (r >= 0) return FALSE;
    func_80296CB4(arg0);
    return bs_checkInterrupt(BS_INTR_31) == 2;
}

// the same, for hits that come from a position instead of an actor
RECOMP_PATCH bool func_8028F45C(s32 arg0, f32 arg1[3]) {
    if (gSteve.enabled && gSteve.driving) {
        hurt_src[0] = arg1[0];
        hurt_src[1] = arg1[1];
        hurt_src[2] = arg1[2];
        hurt_src_valid = TRUE;
        baiFrame_startWithValue(1.0f);
        return FALSE;
    }
    func_80296CB4(arg0);
    func_80296CC0(arg1);
    return bs_checkInterrupt(BS_INTR_23) == 2;
}

// honeycombs when bk started processing an enemy contact (baMarker_8028D7B8).
// most contacts are body bumps that do 0 damage but still call the hurt
// function below; only a contact that actually took health is a hit.
static s32 hit_hp_before = -1;

s32 func_8033D594(void *flags); // damage of this contact
s32 func_8033D564(void *flags); // contact type
enum hitbox_e hitbox_getHitboxForActor(ActorMarker *marker);

RECOMP_HOOK("baMarker_8028D7B8") void steve_before_enemy_contact(s32 arg0, ActorMarker *marker, void *flags) {
    hit_hp_before = item_getCount(ITEM_14_HEALTH);
}

RECOMP_HOOK_RETURN("baMarker_8028D7B8") void steve_after_enemy_contact(void) {
    hit_hp_before = -1;
}

// TRUE if this hurt call comes with real damage (or isn't from an enemy contact)
static int steve_contact_did_damage(void) {
    return hit_hp_before < 0 || item_getCount(ITEM_14_HEALTH) < hit_hp_before;
}

static f32 steve_hp_per_honeycomb(void) {
    s32 max = item_getCount(ITEM_15_HEALTH_TOTAL);
    return 20.0f / (f32)(max > 0 ? max : 5);
}

void steve_item_used(void) {
    gSteve.hand_height = 0.0f;
}

void steve_reset_attack_strength(void) {
    gSteve.attack_strength_ticker = 0;
}

// FoodData (26.3), normal difficulty
void steve_exhaust(f32 amount) {
    if (gSurv.creative) {
        return;
    }
    gSteve.exhaustion += amount;
    if (gSteve.exhaustion > 40.0f) gSteve.exhaustion = 40.0f;
}

// FoodData.eat
void steve_eat(int nutrition, f32 saturation_modifier) {
    Steve *s = &gSteve;

    s->food_level += nutrition;
    if (s->food_level > 20) s->food_level = 20;
    s->saturation += nutrition * saturation_modifier * 2.0f;
    if (s->saturation > (f32)s->food_level) s->saturation = (f32)s->food_level;
}

static void steve_heal(f32 amount);
void steve_hurt(f32 amount);
void steve_hurt_ex(f32 amount, int bypass_armor);

static void steve_food_tick(void) {
    Steve *s = &gSteve;
    int hurt = s->health < 20.0f;

    if (gSurv.creative) {
        return;
    }
    if (s->exhaustion > 4.0f) {
        s->exhaustion -= 4.0f;
        if (s->saturation > 0.0f) {
            s->saturation = (s->saturation - 1.0f > 0.0f) ? s->saturation - 1.0f : 0.0f;
        } else if (s->food_level > 0) {
            s->food_level--;
        }
    }
    if (s->saturation > 0.0f && hurt && s->food_level >= 20) {
        if (++s->food_timer >= 10) {
            f32 spent = (s->saturation < 6.0f) ? s->saturation : 6.0f;
            steve_heal(spent / 6.0f);
            steve_exhaust(spent);
            s->food_timer = 0;
        }
    } else if (s->food_level >= 18 && hurt) {
        if (++s->food_timer >= 80) {
            steve_heal(1.0f);
            steve_exhaust(6.0f);
            s->food_timer = 0;
        }
    } else if (s->food_level <= 0) {
        if (++s->food_timer >= 80) {
            if (s->health > 1.0f) { // normal difficulty starves you down to half a heart
                steve_hurt_ex(1.0f, TRUE); // starve
            }
            s->food_timer = 0;
        }
    } else {
        s->food_timer = 0;
    }
}

static void steve_sync_health(void) {
    Steve *s = &gSteve;
    s32 bk;

    // creative / spectator are invulnerable: whatever bk took (hazards,
    // falls), the honeycombs are put back before the hearts copy them
    if (gSurv.creative || GM_SPECTATING()) {
        s32 total = item_getCount(ITEM_15_HEALTH_TOTAL);
        if (item_getCount(ITEM_14_HEALTH) < total && item_getCount(ITEM_14_HEALTH) > 0) {
            item_set(ITEM_14_HEALTH, total);
        }
    }
    bk = item_getCount(ITEM_14_HEALTH);

    if (!s->health_init) {
        s->food_level = 20;
        s->air_supply = 300;
        s->saturation = 5.0f; // FoodConstants.START_SATURATION
        s->health = bk * steve_hp_per_honeycomb();
        s->last_bk_health = bk;
        s->health_init = TRUE;
    }
    if (bk != s->last_bk_health && s->last_bk_health == 0 && bk > 0) {
        // respawned (bk refilled the honeycombs): ServerPlayer.restoreFrom
        // gives a fresh FoodData, full air, no hurt time
        s->food_level = 20;
        s->saturation = 5.0f;
        s->exhaustion = 0.0f;
        s->food_timer = 0;
        s->air_supply = 300;
        s->hurt_time = 0;
        s->invul_time = 0;
        s->health = bk * steve_hp_per_honeycomb();
        s->last_bk_health = bk;
    }
    if (bk != s->last_bk_health) {
        recomp_printf("[mc] honeycombs %d -> %d, map %x pos %d %d %d, fall %d, water %d\n", s->last_bk_health, bk, map_get(),
            (s32)s->pos[0], (s32)s->pos[1], (s32)s->pos[2], (s32)s->fall_distance, player_getWaterState());
        if (bk == 0 && s->last_bk_health > 0) {
            sound_play_player(SND(ENTITY_PLAYER_DEATH), 1.0f, (sound_random() - sound_random()) * 0.2f + 1.0f);
            surv_drop_all(); // Player.dropEquipment on death
            if (killed_by_mob) advancements_killed_by_mob(killed_by_mob); // EntityKilledPlayer
            killed_by_mob = FALSE;
            s->fire_ticks = 0;
            // LivingEntity.dropExperience: getBaseExperienceReward = min(level * 7, 100)
            // (not in spectator), then the respawned player starts from 0
            if (!GM_SPECTATING()) {
                s32 xp = gXp.level * 7;
                xp_award(s->pos, xp > 100 ? 100 : xp);
            }
            xp_reset();
        }
        if (bk < s->last_bk_health && bk > 0) {
            // bk hit us: minecraft's hurt reaction, the hit (a honeycomb's
            // worth) through the armor, and the honeycombs it saved back
            f32 per = steve_hp_per_honeycomb();
            f32 dmg = (s->last_bk_health - bk) * per;
            s32 target;
            // LivingEntity.applyItemBlocking: a shield up toward the hit takes
            // it all (the honeycombs come back, no hurt reaction)
            if (hurt_src_valid && shield_absorb(dmg, hurt_src) <= 0.0f) {
                hurt_src_valid = FALSE;
                item_adjustByDiffWithHud(ITEM_14_HEALTH, s->last_bk_health - bk);
                s->last_bk_health = item_getCount(ITEM_14_HEALTH);
                goto hp_done;
            }
            dmg = steve_armor_absorb(dmg);
            steve_on_damaged();
            s->invul_time = 20;
            steve_exhaust(0.1f); // DamageSource food exhaustion
            s->health -= dmg;
            if (s->health < 0.5f) s->health = 0.5f;
            target = (s32)(s->health / per);
            if ((f32)target * per < s->health) target++;
            if (target > bk) item_adjustByDiffWithHud(ITEM_14_HEALTH, target - bk);
            s->last_bk_health = item_getCount(ITEM_14_HEALTH);
            goto hp_done;
        } else if (bk < s->last_bk_health) {
            steve_on_damaged();
            s->invul_time = 20;
            steve_exhaust(0.1f);
        } else {
            // picked up honeycombs: minecraft's closest food is the honey
            // bottle (nutrition 6, saturation modifier 0.1)
            steve_eat(6 * (bk - s->last_bk_health), 0.1f);
        }
        s->health += (bk - s->last_bk_health) * steve_hp_per_honeycomb();
        s->last_bk_health = bk;
    }
hp_done:
    if (s->health > 20.0f) s->health = 20.0f;
    if (s->health < 0.0f) s->health = 0.0f;

    // bk's honeycombs are the truth: the hearts must always be inside the
    // range that honeycomb count stands for (e.g. 4 of 5 = 12.x .. 16 hp),
    // so they can never drift apart
    {
        f32 per = steve_hp_per_honeycomb();
        f32 hi = bk * per;
        f32 lo = (bk - 1) * per;

        if (s->health > hi) s->health = hi;
        if (bk > 0 && s->health <= lo) s->health = lo + 1.0f;
        if (bk == 0) s->health = 0.0f;
    }
}

// LivingEntity.heal. bk's honeycombs follow, so healing shows on bk's hud too
static void steve_heal(f32 amount) {
    Steve *s = &gSteve;
    f32 per = steve_hp_per_honeycomb();
    s32 bk, target;

    s->health += amount;
    if (s->health > 20.0f) s->health = 20.0f;
    bk = item_getCount(ITEM_14_HEALTH);
    // the honeycombs that cover this much health (rounded up, like a hit:
    // steve_sync_health keeps the hearts inside (bk - 1) * per .. bk * per,
    // so rounding down never gave a honeycomb back and regeneration was lost)
    target = (s32)(s->health / per);
    if ((f32)target * per < s->health) target++;
    if (target > bk) {
        item_adjustByDiffWithHud(ITEM_14_HEALTH, target - bk);
        s->last_bk_health = item_getCount(ITEM_14_HEALTH);
    }
}

// LivingEntity.hurt, simplified: no armor, 10 ticks of invulnerability
// Player.getArmorValue / ARMOR_TOUGHNESS from what's worn
static void steve_armor_stats(int *armor, f32 *tough) {
    int slot;
    *armor = 0;
    *tough = 0.0f;
    for (slot = 0; slot < 4; slot++) {
        const ItemStack *st = &gSurv.inv[INV_ARMOR(slot)];
        if (st->count > 0 && st->id >= ITEM_FIRST && st->id < ITEM_END && mc_item_armor[st->id - ITEM_FIRST].slot == slot) {
            *armor += mc_item_armor[st->id - ITEM_FIRST].defense;
            *tough += mc_item_armor[st->id - ITEM_FIRST].toughness;
        }
    }
}

int steve_armor_value(void) {
    int a;
    f32 t;
    steve_armor_stats(&a, &t);
    return a;
}

// LivingEntity.getDamageAfterArmorAbsorb: hurtArmor (doHurtEquipment: every
// worn piece loses max(1, damage / 4) durability, breaking at its max), then
// CombatRules.getDamageAfterAbsorb with what's left
f32 steve_armor_absorb(f32 damage) {
    int slot, armor;
    f32 tough, toughness, real;

    if (damage <= 0.0f) return damage;
    if (!gSurv.creative) {
        int wear = (int)((damage / 4.0f > 1.0f) ? damage / 4.0f : 1.0f);
        for (slot = 0; slot < 4; slot++) {
            ItemStack *st = &gSurv.inv[INV_ARMOR(slot)];
            if (st->count == 0 || item_max_damage(st->id) == 0) continue;
            st->damage += wear;
            if (st->damage >= item_max_damage(st->id)) {
                st->id = 0; // hurtAndBreak: broken
                st->count = 0;
                st->damage = 0;
            }
        }
    }
    steve_armor_stats(&armor, &tough);
    toughness = 2.0f + tough / 4.0f;
    real = armor - damage / toughness;
    if (real < armor * 0.2f) real = armor * 0.2f;
    if (real > 20.0f) real = 20.0f;
    return damage * (1.0f - real / 25.0f);
}

void steve_hurt(f32 amount) {
    steve_hurt_ex(amount, FALSE);
}

// bypass_armor: DamageTypeTags.BYPASSES_ARMOR (falling, drowning, starving,
// burning)
void steve_hurt_ex(f32 amount, int bypass_armor) {
    Steve *s = &gSteve;
    s32 bk, target;
    f32 per = steve_hp_per_honeycomb();

    // invulnerableTime > 10: still protected from the last hit
    if (s->invul_time > 10 || amount <= 0.0f || gSurv.creative || GM_SPECTATING()) {
        return; // creative: Abilities.invulnerable
    }
    if (!bypass_armor) amount = steve_armor_absorb(amount);
    s->health -= amount;
    if (s->health < 0.0f) s->health = 0.0f;
    s->invul_time = 20;
    steve_on_damaged();
    steve_exhaust(0.1f);

    bk = item_getCount(ITEM_14_HEALTH);
    target = (s32)(s->health / per);
    if ((f32)target * per < s->health) target++; // ceil
    if (target < bk) {
        item_adjustByDiffWithHud(ITEM_14_HEALTH, target - bk);
    }
    s->last_bk_health = item_getCount(ITEM_14_HEALTH);
    recomp_printf("[mc] steve hurt %d, hp %d\n", (s32)amount, (s32)s->health);
    if (s->last_bk_health == 0) {
        bs_setState(BS_41_DIE); // same as bk's own fall damage death (bs/splat.c)
    }
}

// --- water ---
// bk keeps water out of its normal collision and finds it with a separate
// floor probe (code_C4B0.c / code_94A20.c). when that probe says the floor is
// water, the floor y is the water surface. we read it after banjo's terrain
// collision each frame, and keep banjo's own in-water flag off so banjo never
// switches to his swim states: steve swims with minecraft's physics instead.

static int water_valid;
static f32 water_surface;

// func_8028B59C plays a splash when the flag goes from 0 to 1. if we just
// cleared it every frame, bk saw "entering water" every frame (splash spam).
// so give it back its real previous value first, then hide it again.
static u8 real_in_water;
static int hid_in_water;

RECOMP_HOOK("func_8028B59C") void steve_before_water_check(void) {
    if (hid_in_water) {
        D_8037BF61 = real_in_water;
        hid_in_water = FALSE;
    }
}

RECOMP_HOOK_RETURN("func_8028B59C") void steve_after_water_check(void) {
    real_in_water = D_8037BF61;
    if (gSteve.enabled && gSteve.driving) {
        D_8037BF61 = 0;
        hid_in_water = TRUE;
    }
}

static void steve_read_water(void) {
    water_valid = floor_isCurrentFloorunk59() != 0;
    water_surface = floor_getCurrentFloorYPosition();
}

// Entity.updateInWaterStateAndDoFluidPushing + isEyeInFluid
static void steve_update_water(void) {
    Steve *s = &gSteve;

    f32 surface = water_surface;
    int valid = water_valid;

    // water placed in the grid (buckets, fluids.c) counts like bk's water
    {
        f32 grid;
        if (fluid_surface(FLUID_WATER, s->pos, steve_height(), &grid)) {
            if (!valid || grid > surface || s->pos[1] >= surface) surface = grid;
            valid = TRUE;
        }
    }
    s->in_water = valid && s->pos[1] < surface;
    s->water_height = s->in_water ? (surface - s->pos[1]) / MC_BLOCK : 0.0f;
    s->eye_in_water = s->in_water && s->pos[1] + steve_eye_height() < surface;
    // Entity.lavaIgnite (15 s) + lavaHurt (4)
    {
        f32 lava;
        int in_lava = fluid_surface(FLUID_LAVA, s->pos, steve_height(), &lava) && !GM_SPECTATING();
        if (in_lava) {
            if (s->fire_ticks < 300) s->fire_ticks = 300; // igniteForSeconds(15)
            steve_hurt(4.0f);
            s->fall_distance *= 0.5f;
        }
        // water puts it out (InsideBlockEffectType.EXTINGUISH -> clearFire)
        if (s->in_water && s->fire_ticks > 0) s->fire_ticks = 0;
        // Entity.baseTick: 1 damage every 20 ticks while burning (not in lava)
        if (s->fire_ticks > 0) {
            if (s->fire_ticks % 20 == 0 && !in_lava) { hurt_effect = HURT_FIRE; steve_hurt_ex(1.0f, TRUE); } // on_fire
            s->fire_ticks--;
        }
        if (GM_SPECTATING()) s->fire_ticks = 0;
    }
    // EntityFluidInteraction: currents push steve (players aren't pushed while flying)
    if (!s->flying) fluid_push(s->pos, STEVE_HALF_WIDTH, steve_height(), s->vel);
    if (s->in_water) {
        s->fall_distance = 0.0f; // resetFallDistance
    }
}

// gloop's bubbles (clanker's cavern): bk adds 2 of its 6 air bubbles
// while banjo is underwater (marker.c MARKER_6B_GLOOPBUBBLE), but steve
// keeps banjo out of his swim states. steve gets the same third of his air.
RECOMP_HOOK("__baMarker_resolveCollision") void steve_on_player_touch(Prop *other) {
    if (!gSteve.enabled || !gSteve.driving || !gSteve.eye_in_water) return;
    if (!(*((u32 *)(((u32)other) + 8)) & 1) || other->actorProp.marker == NULL) return; // actor props only
    if (other->actorProp.marker->id == 0x6B) {
        gSteve.air_supply += 100;
        if (gSteve.air_supply > 300) gSteve.air_supply = 300;
    }
}

// Entity.baseTick air supply: -1 a tick with your head underwater, 2 damage
// when it reaches -20 (then back to 0), +4 a tick out of water
static void steve_air_tick(void) {
    Steve *s = &gSteve;

    if (s->eye_in_water && !gSurv.creative) {
        s->air_supply--;
        if (s->air_supply == -20) {
            s->air_supply = 0;
            hurt_effect = HURT_DROWN;
            steve_hurt_ex(2.0f, TRUE); // drown
        }
    } else if (s->air_supply < 300) {
        s->air_supply += 4;
        if (s->air_supply > 300) s->air_supply = 300;
    }
}

// --- climbing ---
// bk's climbable trees/poles (ch/climbBase.c and others) call climbSet every
// frame banjo is inside their climb area. steve treats that area like a
// minecraft ladder instead of letting banjo grab on (see the hooks below).

static int steve_climb_frames;

// bk only calls climbSet while banjo is within the pole's radius + 25 units,
// which steve's own collision may never let him reach. so every pole bk
// reports is remembered (per map), and steve checks against all of them
// each tick: LivingEntity.onClimbable is "my box is inside the ladder".
#define MAX_POLES 32
typedef struct {
    f32 bottom[3];
    f32 top_y;
    f32 radius;
    s16 logs_left; // mining: logs in the trunk still standing
    u8 felled;     // the whole tree has been mined away
} Pole;
static Pole poles[MAX_POLES];
static int pole_count;
static s32 poles_map = -1;

// cells of trees that were mined (per map)
#define MAX_TREE_MINED 256
static s32 tree_mined[MAX_TREE_MINED][3];
static int tree_mined_count;
static s32 tree_mined_map = -1;

// the saved world (worldsave.c): this map's mined tree cells
int steve_tree_mined_count(s32 map) {
    return (tree_mined_map == map) ? tree_mined_count : 0;
}

const s32 *steve_tree_mined_cell(int i) {
    return tree_mined[i];
}

void steve_tree_restore_begin(s32 map) {
    tree_mined_map = map;
    tree_mined_count = 0;
}

void steve_tree_restore_cell(s32 x, s32 y, s32 z) {
    if (tree_mined_count >= MAX_TREE_MINED) return;
    tree_mined[tree_mined_count][0] = x;
    tree_mined[tree_mined_count][1] = y;
    tree_mined[tree_mined_count][2] = z;
    tree_mined_count++;
}

static void steve_hide_tree_model(Pole *pl);
static s32 mc_floor_div(f32 v);

static void steve_remember_pole(f32 bottom[3], f32 top[3], f32 radius) {
    int i;

    if (poles_map != map_get()) {
        poles_map = map_get();
        pole_count = 0;
    }
    for (i = 0; i < pole_count; i++) {
        if (poles[i].bottom[0] == bottom[0] && poles[i].bottom[1] == bottom[1] && poles[i].bottom[2] == bottom[2]) {
            return;
        }
    }
    if (pole_count < MAX_POLES) {
        Pole *p = &poles[pole_count++];
        p->bottom[0] = bottom[0];
        p->bottom[1] = bottom[1];
        p->bottom[2] = bottom[2];
        p->top_y = top[1];
        p->radius = radius;
        p->logs_left = (s16)((top[1] - bottom[1]) / MC_BLOCK);
        if (p->logs_left < 1) p->logs_left = 1;
        p->felled = FALSE;
        recomp_printf("[mc] climbable pole %d: r %d, %d..%d\n", pole_count, (s32)radius, (s32)bottom[1], (s32)top[1]);
        // logs mined from it in an earlier visit (the saved world): the same
        // log column as steve_tree_block_at
        if (tree_mined_map == map_get()) {
            s32 lx = mc_floor_div(bottom[0]), lz = mc_floor_div(bottom[2]);
            for (i = 0; i < tree_mined_count; i++) {
                f32 cy = (tree_mined[i][1] + 0.5f) * MC_BLOCK;
                if (tree_mined[i][0] != lx || tree_mined[i][2] != lz) continue;
                if (cy < bottom[1] - MC_BLOCK || cy > top[1] + MC_BLOCK) continue;
                if (--p->logs_left <= 0) {
                    p->felled = TRUE;
                    steve_hide_tree_model(p);
                    break;
                }
            }
        }
    }
}

// trees in bk levels are part of the level model, and the climbable ones are
// these poles. for mining: a cell inside a trunk is an oak log, the cells
// around and above the top of a trunk are its leaves.
static s32 mc_floor_div(f32 v) { s32 i = (s32)(v / MC_BLOCK); return (i * MC_BLOCK > v) ? i - 1 : i; }

u16 steve_tree_block_at(s32 x, s32 y, s32 z) {
    f32 cx = (x + 0.5f) * MC_BLOCK, cy = (y + 0.5f) * MC_BLOCK, cz = (z + 0.5f) * MC_BLOCK;
    int i;

    if (poles_map != map_get()) {
        return BLOCK_AIR;
    }
    for (i = 0; i < pole_count; i++) {
        Pole *pl = &poles[i];
        f32 dx = cx - pl->bottom[0], dz = cz - pl->bottom[2];
        f32 d2 = dx * dx + dz * dz;

        if (pl->felled) continue;

        // one column of logs: the cell holding the pole's axis
        if (x == mc_floor_div(pl->bottom[0]) && z == mc_floor_div(pl->bottom[2])
            && cy >= pl->bottom[1] - MC_BLOCK && cy <= pl->top_y + MC_BLOCK) {
            return BLOCK_LOG;
        }
        if (d2 <= (3.0f * MC_BLOCK) * (3.0f * MC_BLOCK) && cy > pl->top_y && cy <= pl->top_y + 4.0f * MC_BLOCK) {
            return BLOCK_LEAVES;
        }
    }
    return BLOCK_AIR;
}

int steve_tree_is_mined(s32 x, s32 y, s32 z) {
    int i;

    if (tree_mined_map != map_get()) {
        return FALSE;
    }
    for (i = 0; i < tree_mined_count; i++) {
        if (tree_mined[i][0] == x && tree_mined[i][1] == y && tree_mined[i][2] == z) return TRUE;
    }
    return FALSE;
}

// a tree's model is a ModelProp in one of bk's cubes. clearing its active
// bit (unk8_4) is what bk itself checks before drawing it and before using
// its collision (code_A5BC0.c), so the whole tree is gone.
static void steve_hide_tree_model(Pole *pl) {
    f32 probe[3][3];
    int k;

    probe[0][0] = probe[1][0] = probe[2][0] = pl->bottom[0];
    probe[0][2] = probe[1][2] = probe[2][2] = pl->bottom[2];
    probe[0][1] = pl->bottom[1];
    probe[1][1] = (pl->bottom[1] + pl->top_y) * 0.5f;
    probe[2][1] = pl->top_y;
    for (k = 0; k < 3; k++) {
        Cube *cube = cubeList_GetCubeAtPosition_f32(probe[k]);
        int j;

        if (cube == NULL || cube->prop2Ptr == NULL) continue;
        for (j = 0; j < cube->prop2Cnt; j++) {
            Prop *pr = &cube->prop2Ptr[j];
            f32 dx, dz, reach;

            if (pr->is_actor || !pr->is_3d || !pr->unk8_4) continue;
            dx = pr->modelProp.position[0] - pl->bottom[0];
            dz = pr->modelProp.position[2] - pl->bottom[2];
            reach = pl->radius + 60.0f;
            if (dx * dx + dz * dz > reach * reach) continue;
            if (pr->modelProp.position[1] < pl->bottom[1] - 150.0f || pr->modelProp.position[1] > pl->top_y) continue;
            pr->unk8_4 = FALSE;
            recomp_printf("[mc] tree model %d hidden\n", pr->modelProp.model_index);
        }
    }
}

void steve_tree_mine(s32 x, s32 y, s32 z) {
    f32 cx = (x + 0.5f) * MC_BLOCK, cz = (z + 0.5f) * MC_BLOCK;
    int i;

    if (tree_mined_map != map_get()) {
        tree_mined_map = map_get();
        tree_mined_count = 0;
    }
    if (tree_mined_count < MAX_TREE_MINED) {
        tree_mined[tree_mined_count][0] = x;
        tree_mined[tree_mined_count][1] = y;
        tree_mined[tree_mined_count][2] = z;
        tree_mined_count++;
    }
    if (steve_tree_block_at(x, y, z) != BLOCK_LOG) {
        return;
    }
    for (i = 0; i < pole_count; i++) {
        Pole *pl = &poles[i];
        f32 dx = cx - pl->bottom[0], dz = cz - pl->bottom[2];
        f32 trunk = pl->radius + MC_BLOCK * 0.5f;

        if (pl->felled || dx * dx + dz * dz > trunk * trunk) continue;
        if (--pl->logs_left <= 0) {
            pl->felled = TRUE;
            steve_hide_tree_model(pl);
        }
        return;
    }
}

// aiming at a tree object: which of its cells the hit means. anywhere on or
// near the trunk snaps to the trunk's log column at that height (the nearest
// log still standing if that one's gone); up in the canopy, the leaves there
// (or the top log). the nearest tree wins
int steve_tree_pick(const f32 hit[3], const f32 n[3], s32 out[3], u16 *block) {
    int i, best_i = -1;
    f32 best = 1e9f;

    if (poles_map != map_get()) return FALSE;
    for (i = 0; i < pole_count; i++) {
        Pole *pl = &poles[i];
        f32 dx = hit[0] - pl->bottom[0], dz = hit[2] - pl->bottom[2], d2 = dx * dx + dz * dz;
        f32 reach = pl->radius + 0.75f * MC_BLOCK;

        if (pl->felled) continue;
        if (hit[1] < pl->bottom[1] - MC_BLOCK || hit[1] > pl->top_y + 4.5f * MC_BLOCK) continue;
        // (the canopy is wider: leaves reach 3 blocks out)
        if (hit[1] <= pl->top_y + 0.5f * MC_BLOCK ? d2 > reach * reach : d2 > (3.5f * MC_BLOCK) * (3.5f * MC_BLOCK)) continue;
        if (d2 < best) {
            best = d2;
            best_i = i;
        }
    }
    if (best_i < 0) return FALSE;
    {
        Pole *pl = &poles[best_i];
        s32 lx = mc_floor_div(pl->bottom[0]), lz = mc_floor_div(pl->bottom[2]);
        s32 ylo = mc_floor_div(pl->bottom[1] - MC_BLOCK * 0.5f), yhi = mc_floor_div(pl->top_y + MC_BLOCK * 0.5f);
        s32 y = mc_floor_div(hit[1]), k;

        if (hit[1] > pl->top_y + 0.5f * MC_BLOCK) {
            // the canopy: the leaf cell just behind the face that was hit
            s32 c[3];
            for (k = 0; k < 3; k++) c[k] = mc_floor_div(hit[k] - n[k] * 2.0f);
            if (steve_tree_block_at(c[0], c[1], c[2]) == BLOCK_LEAVES && !steve_tree_is_mined(c[0], c[1], c[2])) {
                for (k = 0; k < 3; k++) out[k] = c[k];
                *block = BLOCK_LEAVES;
                return TRUE;
            }
            y = yhi;
        }
        if (y < ylo) y = ylo;
        if (y > yhi) y = yhi;
        // the log at that height, else the nearest one still standing
        for (k = 0; k <= yhi - ylo; k++) {
            s32 ys[2] = { y - k, y + k };
            int j;
            for (j = 0; j < 2; j++) {
                if (ys[j] < ylo || ys[j] > yhi) continue;
                if (steve_tree_block_at(lx, ys[j], lz) == BLOCK_LOG && !steve_tree_is_mined(lx, ys[j], lz)) {
                    out[0] = lx;
                    out[1] = ys[j];
                    out[2] = lz;
                    *block = BLOCK_LOG;
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

static int steve_on_climbable(void) {
    f32 *p = gSteve.pos;
    int i;

    if (steve_climb_frames > 0) {
        return TRUE;
    }
    // LivingEntity.onClimbable: the block at the feet is #climbable (ladders)
    if (shapes_climbable(mc_floor_div(p[0]), mc_floor_div(p[1] + 1.0f), mc_floor_div(p[2]))) {
        return TRUE;
    }
    if (poles_map != map_get()) {
        return FALSE;
    }
    for (i = 0; i < pole_count; i++) {
        Pole *pl = &poles[i];
        f32 dx = p[0] - pl->bottom[0], dz = p[2] - pl->bottom[2];
        // steve's box reaches the trunk, with a few units of slack for bk's
        // trunk collision being a bit wider than the pole radius
        // (+ the 18 units the wall push-out can move him per tick)
        f32 reach = pl->radius + STEVE_HALF_WIDTH * MC_BLOCK + 30.0f;

        if (pl->felled) continue;
        if (dx * dx + dz * dz <= reach * reach && p[1] >= pl->bottom[1] - 2.0f && p[1] <= pl->top_y) {
            return TRUE;
        }
    }
    return FALSE;
}

RECOMP_HOOK("climbSet") void steve_before_climb_set(f32 bottom[3], f32 top[3], f32 radius, u32 arg3) {
    if (gSteve.enabled && gSteve.driving) {
        steve_climb_frames = 2;
        steve_remember_pole(bottom, top, radius);
    }
}

// every climbable pole's own update (ch/climbBase.c), state 2 = ready.
// remembering it here means steve knows the pole before he's near it.
typedef struct {
    f32 top[3];
    s16 unkC;
} ClimbBaseLocal;

RECOMP_HOOK("func_802D77D4") void steve_on_climb_base_update(Actor *this) {
    if (gSteve.enabled && this->state == 2) {
        ClimbBaseLocal *local = (ClimbBaseLocal *)&this->local;
        steve_remember_pole(this->position, local->top, (f32)this->actorTypeSpecificField);
    }
}

// cancel banjo's grab: climbUpdateRegrab only climbs if D_8037C590 is set
RECOMP_HOOK_RETURN("climbSet") void steve_after_climb_set(void) {
    if (gSteve.enabled && gSteve.driving) {
        D_8037C590 = 0;
    }
}

// --- pushing out of the ground ---

void *func_80320DB0(f32 pos[3], f32 radius, f32 out_dir[3], u32 flags);

static void steve_depenetrate(f32 pos[3], int in_water) {
    f32 r = STEVE_HALF_WIDTH * MC_BLOCK;
    f32 h = steve_height();
    f32 heights[3];
    int iter, k;

    heights[0] = r + 2.0f;
    heights[1] = h * 0.5f;
    heights[2] = h - r - 2.0f;
    for (iter = 0; iter < 6; iter++) {
        int pushed = FALSE;
        for (k = 0; k < 3; k++) {
            f32 c[3], n[3] = { 0.0f, 0.0f, 0.0f }, len;

            c[0] = pos[0];
            c[1] = pos[1] + heights[k];
            c[2] = pos[2];
            if (func_80320DB0(c, r, n, steve_collision_flags()) == NULL) {
                continue;
            }
            len = mc_sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len < 1e-4f) {
                continue;
            }
            // the ground should be on the other side of the push: check, and
            // flip only if bk's direction really points into it (ground that
            // way, none behind). at a corner or an edge (a pillar's) neither
            // ray hits, the closest point is the edge itself: keep bk's push.
            // flipping on a miss pushed steve into pillars and through them
            {
                f32 back[3], fwd[3], hit[3], hn[3];
                int i;
                for (i = 0; i < 3; i++) {
                    back[i] = c[i] - n[i] / len * (r + 4.0f);
                    fwd[i] = c[i] + n[i] / len * (r + 4.0f);
                }
                if (!steve_raycast(c, back, hit, hn) && steve_raycast(c, fwd, hit, hn)) {
                    len = -len;
                }
            }
            if (!in_water) {
                // on land the floor code owns up/down: ground under the feet
                // (a walkable push) is left alone, the rest pushes sideways
                f32 ny = n[1] / len, hx = n[0] / len, hz = n[2] / len, hl;
                if (ny >= FLOOR_MIN_NORMAL_Y) {
                    continue;
                }
                hl = mc_sqrtf(hx * hx + hz * hz);
                if (hl < 0.1f) {
                    continue; // straight down from a ceiling: not ours either
                }
                pos[0] += hx / hl * 3.0f;
                pos[2] += hz / hl * 3.0f;
                pushed = TRUE;
                continue;
            }
            pos[0] += n[0] / len * 3.0f;
            pos[1] += n[1] / len * 3.0f;
            pos[2] += n[2] / len * 3.0f;
            pushed = TRUE;
        }
        if (!pushed) {
            break;
        }
    }
}

// --- sneaking at edges ---

// Player.canFallAtLeast: TRUE if there's no floor within step height under
// steve's footprint after moving (dx, dz) blocks
static int steve_can_fall(f32 pos[3], f32 dx, f32 dz) {
    static const f32 offsets[5][2] = { { 0, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    f32 radius = STEVE_HALF_WIDTH * MC_BLOCK;
    int i;

    for (i = 0; i < 5; i++) {
        f32 start[3], end[3], hit[3], n[3];

        // same 0.9 footprint as steve_move_vertical's floor rays, otherwise
        // this lets steve hang out further than the floor check can stand on
        start[0] = pos[0] + dx * MC_BLOCK + offsets[i][0] * radius * 0.9f;
        start[1] = pos[1] + 2.0f;
        start[2] = pos[2] + dz * MC_BLOCK + offsets[i][1] * radius * 0.9f;
        end[0] = start[0];
        end[1] = pos[1] - STEVE_STEP_HEIGHT * MC_BLOCK;
        end[2] = start[2];
        if (steve_raycast(start, end, hit, n) && n[1] >= FLOOR_MIN_NORMAL_Y) {
            return FALSE;
        }
    }
    return TRUE;
}

static void steve_back_off_from_edge(f32 pos[3], f32 *vx, f32 *vz) {
    f32 dx = *vx, dz = *vz;
    f32 sx = (dx > 0.0f) ? 0.05f : -0.05f;
    f32 sz = (dz > 0.0f) ? 0.05f : -0.05f;

    if (steve_can_fall(pos, 0.0f, 0.0f)) {
        return; // isAboveGround: already over nothing, don't fight it
    }
    while (dx != 0.0f && steve_can_fall(pos, dx, 0.0f)) {
        if (mc_absf(dx) <= 0.05f) { dx = 0.0f; break; }
        dx -= sx;
    }
    while (dz != 0.0f && steve_can_fall(pos, 0.0f, dz)) {
        if (mc_absf(dz) <= 0.05f) { dz = 0.0f; break; }
        dz -= sz;
    }
    while (dx != 0.0f && dz != 0.0f && steve_can_fall(pos, dx, dz)) {
        if (mc_absf(dx) <= 0.05f) dx = 0.0f; else dx -= sx;
        if (mc_absf(dz) <= 0.05f) dz = 0.0f; else dz -= sz;
    }
    *vx = dx;
    *vz = dz;
}

// --- animation state (ticked with the physics) ---

#define STEVE_SWING_DURATION 6 // LivingEntity.getCurrentSwingDuration for a player with no effects

f32 steve_lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

f32 steve_partial_tick(void) {
    f32 a = gSteve.tick_accum / MC_TICK_SECONDS;
    return (a < 0.0f) ? 0.0f : (a > 1.0f) ? 1.0f : a;
}

// LivingEntity.SwingState.startIfAble (26.3): can't restart during the first
// half of a swing. restarting the same swing keeps the animation value, so
// getAnimation lerps through the wrap instead of snapping back to 0.
void steve_swing(void) {
    Steve *s = &gSteve;

    if (s->swinging && s->swing_time <= STEVE_SWING_DURATION / 2 && s->swing_time > 0) {
        return;
    }
    if (!s->swinging) {
        s->o_attack_anim = 0.0f;
        s->attack_anim = 0.0f;
    }
    s->swinging = TRUE;
    s->swing_time = 0;
}

// SwingState.getAnimation
f32 steve_attack_anim(f32 pt) {
    Steve *s = &gSteve;

    if (s->attack_anim >= s->o_attack_anim) {
        return steve_lerp(s->o_attack_anim, s->attack_anim, pt);
    }
    return steve_lerp(s->o_attack_anim, s->attack_anim + 1.0f, pt);
}

static void steve_tick_anims(void) {
    Steve *s = &gSteve;
    f32 target_fov;

    // SwingState.tick
    s->o_attack_anim = s->attack_anim;
    if (s->swinging) {
        s->attack_anim = (f32)s->swing_time / (f32)STEVE_SWING_DURATION;
        if (s->attack_anim > 1.0f) s->attack_anim = 1.0f;
        if (s->swing_time++ > STEVE_SWING_DURATION) {
            s->swinging = FALSE;
            s->attack_anim = 0.0f;
        }
    } else {
        s->attack_anim = 0.0f;
    }

    // Camera.tick: the camera's eye height eases toward the real one (crouching)
    s->o_eye_height = s->eye_height;
    s->eye_height += ((s->swimming ? 0.4f : (s->sneaking ? 1.27f : 1.62f)) * MC_BLOCK - s->eye_height) * 0.5f;

    // AbstractClientPlayer.updateBob: bob toward horizontal speed (max 0.1) on the ground
    {
        f32 h = mc_sqrtf(s->vel[0] * s->vel[0] + s->vel[2] * s->vel[2]);
        f32 target = s->on_ground ? ((h < 0.1f) ? h : 0.1f) : 0.0f;

        s->o_bob = s->bob;
        s->bob += (target - s->bob) * 0.4f;
    }

    // LocalPlayer.aiStep: the arm lags behind the view by half each tick.
    // minecraft xRot is + down, yRot is + turning right (bk yaw is the other way).
    {
        f32 dy = -s->yaw - s->y_bob;

        while (dy > 180.0f) dy -= 360.0f;
        while (dy < -180.0f) dy += 360.0f;
        s->o_x_bob = s->x_bob;
        s->o_y_bob = s->y_bob;
        s->x_bob += (-s->pitch - s->x_bob) * 0.5f;
        s->y_bob += dy * 0.5f;
    }

    // Camera.tickFov + AbstractClientPlayer.getFieldOfViewModifier:
    // modifier = (speed / walking speed + 1) / 2, sprinting is speed x1.3
    target_fov = s->sprinting ? 1.15f : 1.0f;
    if (s->flying) target_fov *= 1.1f; // getFieldOfViewModifier: flying
    // drawing a bow: scale = min(ticksUsingItem / 20, 1), * (1 - scale^2 * 0.15)
    if (gSurv.use_remaining > 0 && surv_held_item() == ITEM_BOW) {
        f32 sc = (f32)(gSurv.use_duration - gSurv.use_remaining) / 20.0f;
        if (sc > 1.0f) sc = 1.0f;
        target_fov *= 1.0f - sc * sc * 0.15f;
    }
    s->o_fov_mod = s->fov_mod;
    s->fov_mod += (target_fov - s->fov_mod) * 0.5f;
    if (s->fov_mod < 0.1f) s->fov_mod = 0.1f;
    if (s->fov_mod > 1.5f) s->fov_mod = 1.5f;
}

// --- one minecraft tick ---
// follows LivingEntity.aiStep / travel / moveRelative for a player on land.

// the end of every tick, riding or not (Entity.rideTick still runs the
// passenger's tick): timers, food, the hand, the animations
static void steve_tick_tail(Steve *s) {
    // LivingEntity.baseTick / Entity.baseTick
    s->o_hurt_time = (f32)s->hurt_time;
    if (s->hurt_time > 0) {
        s->hurt_time--;
    }
    if (s->invul_time > 0) {
        s->invul_time--;
    }
    steve_food_tick();

    // FirstPersonHandsAndItems.tick (main hand)
    {
        u16 next = surv_held_item();
        // getItemSwapScale: attack strength recharges over 1/attackSpeed*20 ticks
        f32 swap = (s->attack_strength_ticker + 1.0f) / item_attack_delay(next);
        f32 target, d;

        s->attack_strength_ticker++;
        if (swap > 1.0f) swap = 1.0f;
        s->o_hand_height = s->hand_height;
        target = (s->hand_item != next) ? 0.0f : swap * swap * swap;
        d = target - s->hand_height;
        if (d > 0.4f) d = 0.4f;
        if (d < -0.4f) d = -0.4f;
        s->hand_height += d;
        // shouldInstantlyReplaceVisibleItem: same item (e.g. a tool wearing down)
        if (s->hand_height < 0.1f || s->hand_item == next) {
            s->hand_item = next;
        }
    }
    steve_tick_anims();
    s->tick_count++;
}

f32 gSmoothedTickMs;

static void steve_tick(void) {
    Steve *s = &gSteve;
    SteveInput *in = &gSteveInput;
    if (boat_riding()) {
        // Entity.rideTick: the boat moves, steve sits in it
        s->in_water = s->eye_in_water = FALSE; // (in a boat that isn't under water: not touching water)
        s->water_height = 0.0f;
        boat_rider_tick(s, in);
        if (boat_riding()) {
            steve_tick_tail(s);
            return;
        }
    }
    f32 forward_in, strafe_in;
    f32 look[3], fwd[2], right[2], flen;
    f32 slip, friction, speed;
    f32 ax, az, ilen;

    // directions on the ground plane, from steve's own yaw (the mouse / c
    // buttons turn him in every camera mode, like minecraft)
    (void)look;
    (void)flen;
    fwd[0] = ml_sin_deg(s->yaw);
    fwd[1] = ml_cos_deg(s->yaw);
    right[0] = -fwd[1];
    right[1] = fwd[0];

    // input, like minecraft's forwardImpulse / leftImpulse but analog
    forward_in = in->stick[1];
    strafe_in = in->stick[0];

    steve_update_water();

    // crouching makes steve 1.5 tall. like minecraft, you stay crouched if
    // there's no room to stand up. in water shift sinks instead (no crouch pose).
    // creative flight (LocalPlayer.aiStep): double tap jump within 7 ticks
    if (s->jump_trigger_time > 0) {
        s->jump_trigger_time--;
    }
    if (GM_SPECTATING()) {
        s->flying = TRUE; // spectator: Abilities.flying, always
    } else if (!gSurv.creative) {
        s->flying = FALSE;
    } else if (in->jump && !s->was_jumping) {
        if (s->jump_trigger_time == 0) {
            s->jump_trigger_time = 7;
        } else if (!s->swimming) {
            s->flying = !s->flying;
            if (s->flying && s->on_ground) {
                s->vel[1] = 0.42f; // jumpFromGround
            }
            s->jump_trigger_time = 0;
        }
    }
    s->was_jumping = in->jump;

    // Entity.updateSwimming: sprinting underwater swims (keeps going while in water)
    if (s->swimming) {
        s->swimming = s->sprinting && s->in_water && !s->flying;
    } else {
        s->swimming = s->sprinting && s->eye_in_water && !s->flying;
    }
    // LivingEntity.updateSwimAmount: eases the model into / out of lying down
    s->o_swim_amount = s->swim_amount;
    if (s->swimming) {
        s->swim_amount = (s->swim_amount + 0.09f < 1.0f) ? s->swim_amount + 0.09f : 1.0f;
    } else {
        s->swim_amount = (s->swim_amount - 0.09f > 0.0f) ? s->swim_amount - 0.09f : 0.0f;
    }

    if (in->sneak && !s->in_water && !s->flying) {
        s->sneaking = TRUE;
    } else if (s->sneaking && steve_ceiling_within(s->pos, STEVE_HEIGHT * MC_BLOCK)) {
        s->sneaking = TRUE;
    } else {
        s->sneaking = FALSE;
    }
    if (s->sneaking) {
        forward_in *= 0.3f;
        strafe_in *= 0.3f;
    }
    // LocalPlayer.aiStep: using an item (eating) slows the input to 0.2
    // and stops sprinting
    if (gSurv.use_remaining > 0) {
        forward_in *= 0.2f;
        strafe_in *= 0.2f;
        s->sprinting = FALSE;
        s->sprint_trigger_time = 0;
    }

    // sprint like minecraft (LocalPlayer.aiStep): ctrl (or b) while moving
    // forward, or double tap forward within 7 ticks. it keeps going until you
    // stop pushing forward, sneak, or run into a wall.
    {
        int forward = forward_in >= 0.8f;
        int can_start = forward && !s->sneaking && !s->horizontal_collision && !s->sprinting
            && (s->food_level > 6 || gSurv.creative); // hasEnoughFoodToStartSprinting

        if (s->sprint_trigger_time > 0) {
            s->sprint_trigger_time--;
        }
        if (s->sneaking || forward_in < -0.1f) {
            s->sprint_trigger_time = 0;
        }
        if (can_start) {
            if (!s->had_forward) {
                if (s->sprint_trigger_time > 0) {
                    s->sprinting = TRUE;
                } else {
                    s->sprint_trigger_time = 7; // options.sprintWindow default
                }
            }
            if (in->sprint) {
                s->sprinting = TRUE;
            }
        }
        if (!forward || s->sneaking || s->horizontal_collision) {
            s->sprinting = FALSE;
        }
        s->had_forward = forward;
    }

    // aiStep
    if (s->no_jump_delay > 0) {
        s->no_jump_delay--;
    }
    if (mc_absf(s->vel[0]) < 0.003f) s->vel[0] = 0.0f;
    if (mc_absf(s->vel[1]) < 0.003f) s->vel[1] = 0.0f;
    if (mc_absf(s->vel[2]) < 0.003f) s->vel[2] = 0.0f;

    // flying: jump / shift move up and down at flyingSpeed * 3 per tick
    if (s->flying) {
        int ya = (in->jump ? 1 : 0) - (in->sneak ? 1 : 0);
        s->vel[1] += ya * 0.05f * 3.0f;
    }

    // LivingEntity.aiStep jumping: in water you swim up (jumpInLiquid) unless
    // you're standing in shallow water (fluidJumpThreshold 0.4)
    if (s->flying) {
        // no jumping while flying
    } else if (in->jump && s->in_water && (!s->on_ground || s->water_height > 0.4f)) {
        s->vel[1] += 0.04f;
    } else if (in->jump) {
        if (s->on_ground && s->no_jump_delay == 0) {
            // jumpFromGround
            if (s->vel[1] < 0.42f) {
                s->vel[1] = 0.42f;
            }
            if (s->sprinting) {
                s->vel[0] += fwd[0] * 0.2f;
                s->vel[2] += fwd[1] * 0.2f;
            }
            steve_exhaust(s->sprinting ? 0.2f : 0.05f); // Player.jumpFromGround
            s->no_jump_delay = 10;
        }
    } else {
        s->no_jump_delay = 0;
    }

    // LocalPlayer.aiStep: shift in water sinks (goDownInWater)
    if (s->in_water && in->sneak && !s->flying) {
        s->vel[1] -= 0.04f;
    }

    // Player.travel while swimming: steer up/down with the look direction
    if (s->swimming) {
        f32 look_y = ml_sin_deg(s->pitch);
        f32 mult = (look_y < -0.2f) ? 0.085f : 0.06f;
        // only rise if there's water above the head (or you're jumping)
        if (look_y <= 0.0f || in->jump || s->water_height > 1.0f - 0.1f) {
            s->vel[1] += (look_y - s->vel[1]) * mult;
        }
    }

    forward_in *= 0.98f;
    strafe_in *= 0.98f;

    // travel
    slip = 0.6f; // normal block. ice etc later.
    friction = s->on_ground ? slip * 0.91f : 0.91f;
    if (s->in_water && !s->flying) {
        speed = 0.02f; // travelInFluid: moveRelative(0.02)
    } else if (s->flying) {
        speed = s->sprinting ? 0.1f : 0.05f; // Player.getFlyingSpeed: flyingSpeed (x2 sprinting)
    } else if (s->on_ground) {
        f32 move_speed = 0.1f * (s->sprinting ? 1.3f : 1.0f);
        speed = move_speed * (0.21600002f / (slip * slip * slip));
    } else {
        speed = s->sprinting ? 0.025999999f : 0.02f;
    }

    // moveRelative
    ilen = forward_in * forward_in + strafe_in * strafe_in;
    if (ilen > 1e-7f) {
        if (ilen > 1.0f) {
            ilen = mc_sqrtf(ilen);
            forward_in /= ilen;
            strafe_in /= ilen;
        }
        ax = (fwd[0] * forward_in + right[0] * strafe_in) * speed;
        az = (fwd[1] * forward_in + right[1] * strafe_in) * speed;
        s->vel[0] += ax;
        s->vel[2] += az;
    }

    // LivingEntity.handleOnClimbable: bk's trees and poles act like ladders
    if (steve_on_climbable()) {
        if (s->vel[0] > 0.15f) s->vel[0] = 0.15f;
        if (s->vel[0] < -0.15f) s->vel[0] = -0.15f;
        if (s->vel[2] > 0.15f) s->vel[2] = 0.15f;
        if (s->vel[2] < -0.15f) s->vel[2] = -0.15f;
        if (s->vel[1] < -0.15f) s->vel[1] = -0.15f;
        // isSuppressingSlidingDownLadder: sneaking holds you in place
        if (s->vel[1] < 0.0f && s->sneaking) s->vel[1] = 0.0f;
    }

    // Player.maybeBackOffFromEdge: sneaking never walks you off a ledge
    if (s->sneaking && s->on_ground && s->vel[1] <= 0.0f) {
        steve_back_off_from_edge(s->pos, &s->vel[0], &s->vel[2]);
    }

    // move (with collision), in bk units
    {
        f32 before[3];

        vec3_copy(before, s->pos);
        if (GM_SPECTATING()) {
            // Entity.noPhysics: straight through everything
            s->pos[0] += s->vel[0] * MC_BLOCK;
            s->pos[1] += s->vel[1] * MC_BLOCK;
            s->pos[2] += s->vel[2] * MC_BLOCK;
            s->horizontal_collision = FALSE;
            s->on_ground = FALSE;
        } else {
            f32 vel_before[3], dx = s->vel[0] * MC_BLOCK, dz = s->vel[2] * MC_BLOCK;

            vec3_copy(vel_before, s->vel);
            s->horizontal_collision = steve_move_horizontal(s->pos, dx, dz, s->vel);
            if (s->horizontal_collision && s->on_ground && s->vel[1] <= 0.0f
                && steve_step_up(s->pos, before, dx, dz, s->vel, vel_before)) {
                // (minecraft: stepped, so no horizontal collision this move)
                s->horizontal_collision = FALSE;
            }
            steve_move_vertical(s->pos, s->vel[1] * MC_BLOCK, s->vel, &s->on_ground);
        }

        // WalkAnimationState.update(min(dist * 4, 1), 0.4): dist is blocks moved this tick
        {
            f32 dx = (s->pos[0] - before[0]) / MC_BLOCK;
            f32 dz = (s->pos[2] - before[2]) / MC_BLOCK;
            f32 target = mc_sqrtf(dx * dx + dz * dz) * 4.0f;

            if (target > 1.0f) target = 1.0f;
            s->o_walk_pos = s->walk_pos;
            s->o_walk_speed = s->walk_speed;
            s->walk_speed += (target - s->walk_speed) * 0.4f;
            s->walk_pos += s->walk_speed;

            // LivingEntity.tick body rotation + tickHeadTurn (minecraft degrees,
            // yRot = -bk yaw). the body turns toward where you walk, and the
            // head can't be more than 50 degrees off the body.
            {
                f32 y_rot = -s->yaw;
                f32 target = s->body_rot, diff;

                if (dx * dx + dz * dz > 0.0025000002f) {
                    f32 walk_dir = mc_atan2_deg(dz, dx) - 90.0f;
                    f32 d = y_rot - walk_dir;

                    while (d >= 180.0f) d -= 360.0f;
                    while (d < -180.0f) d += 360.0f;
                    d = mc_absf(d);
                    target = (95.0f < d && d < 265.0f) ? walk_dir - 180.0f : walk_dir;
                }
                if (s->attack_anim > 0.0f) { // swingState.animation > 0
                    target = y_rot;
                }
                s->o_body_rot = s->body_rot;
                diff = target - s->body_rot;
                while (diff >= 180.0f) diff -= 360.0f;
                while (diff < -180.0f) diff += 360.0f;
                s->body_rot += diff * 0.3f;
                diff = y_rot - s->body_rot;
                while (diff >= 180.0f) diff -= 360.0f;
                while (diff < -180.0f) diff += 360.0f;
                if (mc_absf(diff) > 50.0f) {
                    s->body_rot += diff - (diff > 0.0f ? 50.0f : -50.0f);
                }
                // keep body_rot near y_rot so interpolation never spins the long way
                while (s->body_rot - y_rot > 180.0f) { s->body_rot -= 360.0f; s->o_body_rot -= 360.0f; }
                while (s->body_rot - y_rot < -180.0f) { s->body_rot += 360.0f; s->o_body_rot += 360.0f; }
            }

            // Entity.checkFallDamage: add the downward movement, and on landing
            // LivingEntity.calculateFallDamage = floor(fallDistance + 1e-6 - 3)
            {
                f32 dy = (s->pos[1] - before[1]) / MC_BLOCK;

                if (dy < 0.0f) {
                    s->fall_distance -= dy;
                }
                // flying (creative / spectator) never builds up a fall
                // (Abilities.flying in LivingEntity.checkFallDamage)
                if (s->flying || GM_SPECTATING()) {
                    s->fall_distance = 0.0f;
                }
                if (steve_on_climbable()) {
                    s->fall_distance = 0.0f; // handleOnClimbable: resetFallDistance
                } else if (s->on_ground) {
                    s32 dmg = (s32)(s->fall_distance + 1e-6f - 3.0f);
                    // beak buster: landing while sneaking from a fall like the
                    // mace's smash attack (MaceItem.canSmashAttack: fallDistance > 1.5)
                    if (s->sneaking && s->fall_distance > 1.5f && ability_isUnlocked(ABILITY_2_BEAK_BUSTER)) {
                        gSteveSmashFrames = 8;
                        dmg = 0; // the smash takes the landing, like the mace
                        recomp_printf("[mc] smash (beak buster) after %d blocks\n", (s32)s->fall_distance);
                    }
                    if (dmg > 0) {
                        // LivingEntity.causeFallDamage: Player.getFallSounds
                        // (big over 4), then playBlockFallSound (the block under)
                        u16 under = surv_block_at(mc_floor_div(s->pos[0]), mc_floor_div(s->pos[1] - 0.2f * MC_BLOCK), mc_floor_div(s->pos[2]));
                        sound_play_player(dmg > 4 ? SND(ENTITY_PLAYER_BIG_FALL) : SND(ENTITY_PLAYER_SMALL_FALL), 1.0f, 1.0f);
                        if (under != BLOCK_AIR) sound_block_by_player(under, SOUND_FALL);
                        steve_hurt_ex((f32)dmg, TRUE); // fall
                    }
                    s->fall_distance = 0.0f;
                }
            }

            // Player.checkMovementStatistics: sprinting costs 0.1 per block
            if (s->sprinting && s->on_ground) {
                steve_exhaust(0.1f * mc_sqrtf(dx * dx + dz * dz));
            }

            // Entity.applyMovementEmission: walked distance (drives the view bob)
            s->o_walk_dist = s->walk_dist;
            if (s->on_ground) {
                s->walk_dist += mc_sqrtf(dx * dx + dz * dz) * 0.6f;
            }
            // applyMovementEmissionAndPlaySound: moveDist (climbing counts
            // up and down too), a step sound each time it passes nextStep and
            // there's a block under (getOnPos: 0.2 under the feet)
            if (!s->flying && !GM_SPECTATING()) {
                static f32 move_dist, next_step = 1.0f;
                f32 dy = (s->pos[1] - before[1]) / MC_BLOCK;
                int climbing = steve_on_climbable();
                move_dist += (climbing ? mc_sqrtf(dx * dx + dy * dy + dz * dz) : mc_sqrtf(dx * dx + dz * dz)) * 0.6f;
                if (move_dist > next_step && (s->on_ground || climbing)) {
                    s32 bx = mc_floor_div(s->pos[0]), by = mc_floor_div(s->pos[1] - 0.2f * MC_BLOCK), bz = mc_floor_div(s->pos[2]);
                    u16 under = surv_block_at(bx, by, bz);
                    // (standing on bk's own floor that isn't a block: the ground it looks like)
                    if (under == BLOCK_AIR && s->on_ground) under = terrain_block_at(bx, by, bz);
                    if (s->in_water) {
                        // waterSwimSound: Entity.playSwimSound (0.35 x the speed)
                        f32 sp = mc_sqrtf(s->vel[0] * s->vel[0] * 0.2f + s->vel[1] * s->vel[1] + s->vel[2] * s->vel[2] * 0.2f) * 0.35f;
                        sound_play_player(SND(ENTITY_PLAYER_SWIM), sp > 1.0f ? 1.0f : sp, 1.0f + (sound_random() - sound_random()) * 0.4f);
                        next_step = (f32)(s32)move_dist + 1.0f;
                    } else if (under != BLOCK_AIR) {
                        sound_block_by_player(under, SOUND_STEP); // Entity.playStepSound
                        next_step = (f32)(s32)move_dist + 1.0f;
                    }
                }
            }
        }
    }

    // handleRelativeFrictionAndCalculateMovement: climbing goes up at 0.2
    // while pushing into the trunk or holding jump
    if ((s->horizontal_collision || in->jump) && steve_on_climbable()) {
        s->vel[1] = 0.2f;
    }

    if (s->flying) {
        // Player.travel: air travel, then the vertical speed is the old one * 0.6
        f32 orig_y = s->vel[1];
        s->vel[0] *= 0.91f;
        s->vel[2] *= 0.91f;
        s->vel[1] = orig_y * 0.6f;
        if (s->on_ground) {
            s->flying = FALSE; // landing stops flying
        }
    } else if (s->in_water) {
        // travelInFluid (water): slow down (0.9 sprinting), sink at gravity/16
        f32 slow = s->sprinting ? 0.9f : 0.8f;
        int falling = s->vel[1] <= 0.0f;

        s->vel[0] *= slow;
        s->vel[1] *= 0.8f;
        s->vel[2] *= slow;
        // getFluidFallingAdjustedMovement
        if (!s->sprinting) {
            f32 vy = s->vel[1];
            if (falling && mc_absf(vy - 0.005f) >= 0.003f && mc_absf(vy - 0.08f / 16.0f) < 0.003f) {
                s->vel[1] = -0.003f;
            } else {
                s->vel[1] = vy - 0.08f / 16.0f;
            }
        }
        // swimming into a ledge pops you up onto it
        if (s->horizontal_collision && !steve_ceiling_within(s->pos, steve_height() + 0.6f * MC_BLOCK)) {
            s->vel[1] = 0.3f;
        }
    } else {
        // gravity, drag, friction
        s->vel[1] = (s->vel[1] - 0.08f) * 0.98f;
        s->vel[0] *= friction;
        s->vel[2] *= friction;
    }
    steve_air_tick();

    // minecraft's box collision never lets the player end up inside the
    // ground. our ray collision can, mostly while swimming into slopes. so in
    // water, three spheres along the body ask bk's own "does this sphere touch
    // the level" query and push steve out along the direction it gives.
    // (on land too: rays between heights let slopes and wedges into the body)
    if (!GM_SPECTATING()) {
        steve_depenetrate(s->pos, s->in_water);
    }

    steve_tick_tail(s);
}

// --- hooking it into banjo ---

static int steve_prev_driving = FALSE;
static f32 steve_last_set_pos[3];
static int steve_have_last_set = FALSE;

// frames left where steve holds still after the game moved him (map load,
// warp, respawn): bk loads the new map's collision over a few frames, and
// ticking physics before the floor exists drops him out of the level
static int steve_settle_frames;

static void steve_snap_to(f32 p[3]) {
    steve_settle_frames = 60;
    if (mc_absf(p[0] - gSteve.pos[0]) + mc_absf(p[1] - gSteve.pos[1]) + mc_absf(p[2] - gSteve.pos[2]) > 50.0f)
    recomp_printf("[mc] snap to banjo: map %x pos %d %d %d (was %d %d %d)\n", map_get(), (s32)p[0], (s32)p[1], (s32)p[2],
        (s32)gSteve.pos[0], (s32)gSteve.pos[1], (s32)gSteve.pos[2]);
    vec3_copy(gSteve.prev_pos, p);
    vec3_copy(gSteve.pos, p);
    vec3_copy(gSteve.render_pos, p);
    gSteve.vel[0] = gSteve.vel[1] = gSteve.vel[2] = 0.0f;
    gSteve.tick_accum = 0.0f;
    // the game moved us (warp, respawn, cutscene): nothing carries over
    gSteve.hurt_time = 0;
    gSteve.o_hurt_time = 0.0f;
    gSteve.fall_distance = 0.0f;
}

// can steve drive banjo right now, or does the game need banjo?
// bk keeps control for a while after steve hands it a pad jump (flight pad:
// until the flight's over, shock jump pad: until banjo lands again)
static int steve_handoff;

enum transformation_e bsStoredState_getTransformation(void);
int player_isStable(void);
s32 func_80298850(void);

// mumbo's magic: termite, croc, walrus, pumpkin and bee play as bk's
// creature (their climbing, small size, swimming and flying are the point)
int steve_transformed(void) {
    return bsStoredState_getTransformation() != TRANSFORM_1_BANJO;
}

static int steve_can_drive(void) {
    s32 type, group;

    if (!gSteve.enabled || !D_8037BFB8) {
        return FALSE;
    }
    if (steve_transformed()) {
        return FALSE;
    }
    // bk is running a scripted player mode (code_11870.c's stack: the lair's
    // jigsaw podiums, note door glitter, ...): it moves banjo and reads the
    // buttons itself (podium: a add, b leave, z add all, c down remove)
    if (func_80298850() != 0) {
        return FALSE;
    }
    type = baphysics_get_type();
    if (steve_handoff > 0) {
        if (steve_handoff > 1) {
            steve_handoff--;
            return FALSE;
        }
        if (type == BA_PHYSICS_AIRBORN || !player_isStable() || player_movementGroup() == 0xA) {
            return FALSE;
        }
        steve_handoff = 0;
    }
    switch (type) {
        case BA_PHYSICS_UNK1:
        case BA_PHYSICS_NORMAL:
        case BA_PHYSICS_LOCKED_ROTATION:
        case BA_PHYSICS_INVERTED_YAW:
        case BA_PHYSICS_AIRBORN:
            break;
        default:
            return FALSE; // goto (cutscenes/warps), climb, swim/fly, frozen...
    }
    if (player_getWaterState() != 0) {
        return FALSE;
    }
    group = player_movementGroup();
    switch (group) {
        case 1:   // BSGROUP_1_INTR: hurt, talking, jiggy dance, dying
        case 4:   // BSGROUP_4_LOOK: first person
        case 5:   // BSGROUP_5_CLIMB
        case 0xA: // BSGROUP_A_FLYING
        case 0x6: // BSGROUP_6_TURBO_TALON_TRAINERS: the race shoes
        case 0x9: // BSGROUP_9_LONG_LEG: wading boots
        case 0xC: // BSGROUP_C_WALRUS_SLED
        case 0xD: // BSGROUP_D_TRANSFORMING
            return FALSE;
    }
    return TRUE;
}

static void steve_cycle_camera(void) {
    gSteve.camera_type = (gSteve.camera_type + 1) % 3;
    gSteve.first_person = gSteve.camera_type == CAMERA_FIRST_PERSON;
    recomp_printf("[mc] camera type: %d\n", gSteve.camera_type);
}

// Camera.getMaxZoom
f32 steve_camera_max_zoom(f32 eye[3], f32 back_dir[3], f32 dist) {
    int i;

    for (i = 0; i < 8; i++) {
        f32 from[3], to[3], hit[3], n[3];
        int k;

        from[0] = eye[0] + ((i & 1) * 2 - 1) * 0.1f * MC_BLOCK;
        from[1] = eye[1] + (((i >> 1) & 1) * 2 - 1) * 0.1f * MC_BLOCK;
        from[2] = eye[2] + (((i >> 2) & 1) * 2 - 1) * 0.1f * MC_BLOCK;
        for (k = 0; k < 3; k++) {
            to[k] = from[k] + back_dir[k] * dist;
        }
        if (steve_raycast(from, to, hit, n)) {
            f32 dx = hit[0] - eye[0], dy = hit[1] - eye[1], dz = hit[2] - eye[2];
            f32 d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < dist * dist) {
                dist = mc_sqrtf(d2);
            }
        }
    }
    return dist;
}

int gcdialog_hasCurrentTextId(void);

RECOMP_HOOK_RETURN("bakey_update") void steve_on_bakey_update(void) {
    // d-pad up toggles steve. read it here, before anything else looks at it.
    // not while a screen is open: recomp's keyboard puts the d-pad on i/j/k/l,
    // so typing "i" in the creative search would switch steve off
    if (bakey.pressed_count[BUTTON_D_UP] == 1 && !screen_is_open() && !screen_just_closed()) {
        static int first_enable = TRUE;

        gSteve.enabled = !gSteve.enabled;
        steve_have_last_set = FALSE;
        if (gSteve.enabled && first_enable) {
            // start in first person, like minecraft
            first_enable = FALSE;
            gSteve.first_person = TRUE;

            gSteve.yaw = yaw_get();
            gSteve.pitch = 0.0f;
        }
        if (gSteve.fov_mod < 0.1f) {
            gSteve.fov_mod = gSteve.o_fov_mod = 1.0f;
        }
        gSteve.x_bob = gSteve.o_x_bob = -gSteve.pitch;
        gSteve.body_rot = gSteve.o_body_rot = -gSteve.yaw;
        gSteve.eye_height = gSteve.o_eye_height = 1.62f * MC_BLOCK;
        gSteve.y_bob = gSteve.o_y_bob = -gSteve.yaw;
        recomp_printf("[mc] steve mode: %d\n", gSteve.enabled);
    }

    // d-pad right: Options.setCameraType(getCameraType().cycle()), like f5
    // (checked after mouse_frame: recomp's keyboard puts d-pad right on l,
    // and the l key is keyAdvancements)
    int dpad_right = bakey.pressed_count[BUTTON_D_RIGHT] == 1 && !screen_is_open();
    // d-pad left: next hotbar slot
    if (bakey.pressed_count[BUTTON_D_LEFT] == 1 && !screen_is_open()) {
        gSurv.selected = (gSurv.selected + 1) % 9;
    }

    gSteveInput.jump = bakey.pressed_count[BUTTON_A] != 0;
    gSteveInput.sprint = bakey.pressed_count[BUTTON_B] != 0;
    gSteveInput.sneak = bakey.pressed_count[BUTTON_Z] != 0;
    // attack = l (e on keyboard), use = r. presses add up until the next tick.
    gSteveInput.attack_clicks += bakey.pressed_count[BUTTON_L] == 1;
    gSteveInput.use_clicks += bakey.pressed_count[BUTTON_R] == 1;
    gSteveInput.attack_held = bakey.pressed_count[BUTTON_L] != 0;
    gSteveInput.use_held = bakey.pressed_count[BUTTON_R] != 0;
    gSteveInput.look[0] = (bakey.pressed_count[BUTTON_C_RIGHT] != 0) - (bakey.pressed_count[BUTTON_C_LEFT] != 0);
    gSteveInput.look[1] = (bakey.pressed_count[BUTTON_C_UP] != 0) - (bakey.pressed_count[BUTTON_C_DOWN] != 0);

    // mouse + keyboard (only if the companion mod is installed). recomp's own
    // keyboard bindings already give us wasd + space through the n64 controls.
    // mouse: look while playing, free cursor while a screen is open
    mouse_frame(!gSteve.enabled ? 0 : (screen_is_open() ? 2 : 1));
    if (dpad_right && !(gMouse.available && mouse_key_held(KEY_L))) {
        steve_cycle_camera();
    }
    if (gSteve.enabled && steve_prev_driving) {
        screen_frame();
    } else {
        // (banjo mode or a cutscene: no screens. screen_frame is what clears
        // "just closed", so do that here too, or d-pad up stayed blocked
        // forever after a screen was shut in banjo mode)
        if (screen_is_open()) screen_close();
        else screen_clear_just_closed();
    }
    if (gMouse.available) {
        int k;

        if (gMouse.captured) {
            gSteveInput.attack_clicks += gMouse.left_clicks;
            gSurv.pick_clicks += gMouse.middle_clicks;
            gSteveInput.use_clicks += gMouse.right_clicks;
            gSteveInput.attack_held |= gMouse.left_held;
            gSteveInput.use_held |= gMouse.right_held;
        }
        gSteveInput.sprint |= mouse_key_held(KEY_LCTRL);
        gSteveInput.sneak |= mouse_key_held(KEY_LSHIFT); // sneaking also cancels sprint, so shift (= b) sneaks
        // f: swap the held item and the offhand (keySwapOffhand,
        // ServerboundPlayerActionPacket SWAP_ITEM_WITH_OFFHAND)
        if (mouse_key_pressed(KEY_F) && gSteve.enabled && steve_prev_driving && !screen_is_open() && !GM_SPECTATING()) {
            ItemStack t = gSurv.inv[gSurv.selected];
            gSurv.inv[gSurv.selected] = gSurv.inv[INV_OFFHAND];
            gSurv.inv[INV_OFFHAND] = t;
            gSurv.use_remaining = 0; // (stopUsingItem)
        }
        if (mouse_key_pressed(KEY_F5)) {
            steve_cycle_camera();
        }
        // Minecraft.handleKeybinds only runs with no screen open (in a screen
        // the number keys swap the hovered slot: screen.c)
        for (k = 0; k < 9 && !screen_is_open(); k++) {
            if (mouse_key_pressed(KEY_1 + k)) {
                // in creative, x / c + a number loads / saves that hotbar
                // instead (keyLoad/SaveHotbarActivator)
                int load = mouse_key_held(27), save = mouse_key_held(6);
                if (!gSurv.creative || (!load && !save)) {
                    gSurv.selected = k;
                } else if (load) {
                    hotbars_load(k);
                } else {
                    hotbars_save(k);
                }
            }
        }
        if (gMouse.wheel != 0 && gMouse.captured) {
            // ScrollWheelHandler: wheel up = previous slot, wraps around 9
            s32 sl = gSurv.selected - gMouse.wheel;
            while (sl < 0) sl += 9;
            gSurv.selected = sl % 9;
        }
        // f1: hide the gui and the hand (Options.hideGui)
        if (mouse_key_pressed(KEY_F1)) {
            gHideGui = !gHideGui;
        }
        // q drops one, ctrl + q the whole stack (keyDrop, Minecraft.handleKeybinds:
        // only with no screen open; in a screen q drops the hovered slot)
        if (mouse_key_pressed(KEY_Q) && gSteve.enabled && steve_prev_driving && !screen_is_open() && !screen_just_closed()) {
            surv_drop_selected(mouse_key_held(KEY_LCTRL));
        }
        // f3 (released without a combo): debug text. f3 + f4: survival / creative
        {
            static int f3_combo = FALSE;
            if (mouse_key_held(KEY_F3) && mouse_key_pressed(KEY_F4) && gSteve.enabled && steve_prev_driving) {
                // KeyboardHandler: f3 + f4 opens GameModeSwitcherScreen (it
                // then takes f4 itself)
                if (!screen_is_open()) screen_open_gamemode_switcher();
                f3_combo = TRUE;
            }
            if (mouse_key_pressed(KEY_F3)) {
                f3_combo = FALSE;
            }
            if (!mouse_key_held(KEY_F3) && mouse_key_released(KEY_F3) && !f3_combo) {
                gShowDebug = !gShowDebug;
            }
        }
    }

    // a screen is open (or just closed): steve doesn't move or act, like
    // minecraft. keyboard e reaches us as the n64 l button (attack), so l is
    // also ignored until it's let go after a screen closes.
    {
        static int l_blocked = FALSE;

        // ToggleKeyMapping (Options.toggleSprint): a press flips sprinting on / off
        {
            static int prev_down, toggled;
            int down = gSteveInput.sprint;
            if (option_toggle_sprint()) {
                if (down && !prev_down) toggled = !toggled;
                gSteveInput.sprint = toggled;
            } else {
                toggled = FALSE;
            }
            prev_down = down;
        }
        if (gcdialog_hasCurrentTextId() && !screen_is_open() && !screen_just_closed()) {
            // a text box: a / b are its buttons (next / skip, yes / no), the
            // rest (attack, use, sneak, looking) stays steve's
            gSteveInput.jump = gSteveInput.sprint = 0;
        } else if (screen_is_open() || screen_just_closed()) {
            l_blocked = TRUE;
            gSteveInput.jump = gSteveInput.sprint = gSteveInput.sneak = 0;
            gSteveInput.attack_clicks = gSteveInput.use_clicks = 0;
            gSteveInput.attack_held = gSteveInput.use_held = 0;
            gSteveInput.look[0] = gSteveInput.look[1] = 0;
        } else if (l_blocked) {
            if (bakey.pressed_count[BUTTON_L] == 0) {
                l_blocked = FALSE;
            } else if (!gMouse.left_held) {
                gSteveInput.attack_clicks = 0;
                gSteveInput.attack_held = 0;
            }
        }
    }

    // banjo doesn't get these while steve is in control. in first person the
    // c buttons are steve's look, so banjo (and his c-up look mode) lose them too.
    if (gSteve.enabled && steve_prev_driving) {
        static const s32 stolen[] = {
            BUTTON_A, BUTTON_B, BUTTON_Z, BUTTON_L, BUTTON_R,
            BUTTON_C_LEFT, BUTTON_C_RIGHT, BUTTON_C_UP, BUTTON_C_DOWN,
        };
        int count = 9; // steve's camera in every mode, so the c buttons are his
        int i, first = 0;

        // a conversation is up: a and b are bk's (next / skip, yes / no:
        // bottles asking to teach a move), so they stay with bk
        if (gcdialog_hasCurrentTextId()) first = 2;
        // jumping on a flight pad or a shock jump pad: the a press is bk's
        // (bs_getTypeOfJump: BS_23_FLY_ENTER / the shock jump) and bk flies
        // or springs banjo from here, steve takes over again after landing
        if (first == 0 && bakey.pressed_count[BUTTON_A] == 1 && gSteve.on_ground && !GM_SPECTATING()
            && (baflag_isTrue(BA_FLAG_1_ON_FLIGHT_PAD) || baflag_isTrue(BA_FLAG_2_ON_SPRING_PAD))) {
            steve_handoff = 15;
            gSteveInput.jump = FALSE;
            first = 1;
            recomp_printf("[mc] pad jump: bk takes over\n");
        }
        for (i = first; i < count; i++) {
            bakey.pressed_count[stolen[i]] = 0;
            if (bakey.released_count[stolen[i]] < 2) {
                bakey.released_count[stolen[i]] = 2;
            }
        }
        if (first == 0) D_80281138[0].face_button[FACE_BUTTON(BUTTON_B)] = 0;
        // a right click interaction: one fresh b press, like the real button
        if (steve_b_pulse > 0 && gcdialog_hasCurrentTextId()) {
            steve_b_pulse = 0; // (a right click mustn't skip / answer the text box)
        }
        if (steve_b_pulse > 0) {
            steve_b_pulse--;
            // an actor that claimed b (bakey_getAndSetState(B, 2), like
            // bottles does every frame in range) gets the face button only:
            // bakey_update zeroes the press then, so banjo doesn't claw swipe
            // (bottles refuses to talk while banjo is attacking)
            if (bakey.prev_state[BUTTON_B] == 0) {
                bakey.pressed_count[BUTTON_B] = 1;
                bakey.released_count[BUTTON_B] = 0;
            }
            D_80281138[0].face_button[FACE_BUTTON(BUTTON_B)] = 1;
        }
    }
}

RECOMP_HOOK_RETURN("bastick_update") void steve_on_bastick_update(void) {
    gSteveInput.stick[0] = bastick.value[0];
    gSteveInput.stick[1] = bastick.value[1];
    if (screen_is_open()) {
        gSteveInput.stick[0] = gSteveInput.stick[1] = 0.0f; // no walking in menus
    }

    if (gSteve.enabled && steve_prev_driving) {
        bastick.value[0] = bastick.value[1] = 0.0f;
        bastick.distance = 0.0f;
        bastick.angle = 0.0f;
        bastick.zone = 0;
        bastick.zone_position = 0.0f;
        bastick.nonzero_count = 0;
        if (bastick.zeroed_count < 2) {
            bastick.zeroed_count = 2;
        }
    }
}

// how many frames in a row steve has been in control (the camera only goes
// back to steve's view once this has settled, so bk's respawn sequence
// flickering between bk and steve doesn't throw the camera around)
int gSteveDrivingFrames;

RECOMP_HOOK("baphysics_update") void steve_before_physics(void) {
    f32 banjo_pos[3];

    if (!gSteve.enabled) {
        steve_prev_driving = FALSE;
        gSteve.driving = FALSE;
        return;
    }

    _player_getPosition(banjo_pos);
    gSteve.driving = steve_can_drive();
    gSteveDrivingFrames = gSteve.driving ? gSteveDrivingFrames + 1 : 0;

    // if something other than us moved banjo (warp, void out, map load,
    // cutscene), steve goes to where banjo is
    if (!steve_have_last_set
        || mc_absf(banjo_pos[0] - steve_last_set_pos[0]) > 0.5f
        || mc_absf(banjo_pos[1] - steve_last_set_pos[1]) > 0.5f
        || mc_absf(banjo_pos[2] - steve_last_set_pos[2]) > 0.5f) {
        steve_snap_to(banjo_pos);
        gSteve.on_ground = FALSE;
    }
}

// --- looking around and using blocks ---

// minecraft reach in creative is 5 blocks
#define STEVE_REACH (5.0f * MC_BLOCK)
// c buttons turn speed, degrees per second
#define LOOK_YAW_SPEED 180.0f
#define LOOK_PITCH_SPEED 120.0f

f32 steve_eye_height(void) {
    // Player eye heights: swimming 0.4, crouching 1.27, standing 1.62
    if (gSteve.swimming) return 0.4f * MC_BLOCK;
    return (gSteve.sneaking ? 1.27f : 1.62f) * MC_BLOCK;
}

void steve_look_dir(f32 dst[3]) {
    f32 cp = ml_cos_deg(gSteve.pitch);

    dst[0] = ml_sin_deg(gSteve.yaw) * cp;
    dst[1] = ml_sin_deg(gSteve.pitch);
    dst[2] = ml_cos_deg(gSteve.yaw) * cp;
}

// per frame, like minecraft's mouse look (it isn't tied to ticks)
static void steve_update_look(f32 dt) {
    Steve *s = &gSteve;
    f32 mdx, mdy;

    // c buttons
    s->yaw -= gSteveInput.look[0] * LOOK_YAW_SPEED * dt;
    s->pitch += gSteveInput.look[1] * LOOK_PITCH_SPEED * dt;

    // mouse, if the game gives us one (see mouse.c)
    if (mouse_get_look_delta(&mdx, &mdy)) {
        s->yaw -= mdx;
        s->pitch -= mdy;
    }

    if (s->pitch > 90.0f) s->pitch = 90.0f;
    if (s->pitch < -90.0f) s->pitch = -90.0f;
    while (s->yaw >= 360.0f) s->yaw -= 360.0f;
    while (s->yaw < 0.0f) s->yaw += 360.0f;
}

// --- attacking (Minecraft.startAttack / GameRenderer.pick) ---
// minecraft picks the entity under the crosshair within 3 blocks (entity
// interaction range), unless a block is closer. bk has no entity raycast, so
// we walk along the ray and use bk's own "what props touch this sphere" query
// (the same one banjo's touch spheres use) at each step. the first actor wins.
// then bk's own collision code runs on it with an attack hitbox active, so the
// enemy reacts exactly like it does to banjo's attacks.

#define STEVE_ATTACK_REACH (3.0f * MC_BLOCK)
#define ATTACK_STEP 16.0f
#define ATTACK_RADIUS 24.0f

// the first actor along the look ray (within max_dist), or NULL
static Prop *steve_find_actor(f32 eye[3], f32 dir[3], f32 max_dist, f32 reach) {
    ActorMarker *pm = baMarker_get();
    Prop *target = NULL;
    f32 banjo_pos[3], d;
    s16 saved_off[3];
    int i;

    if (pm == NULL || !pm->collidable) {
        return NULL;
    }
    if (max_dist > reach) {
        max_dist = reach;
    }
    for (i = 0; i < 3; i++) {
        saved_off[i] = pm->unk38[i];
        pm->unk38[i] = 0;
    }
    for (d = ATTACK_STEP; d <= max_dist && target == NULL; d += ATTACK_STEP) {
        f32 p[3];
        Prop *prop;

        p[0] = eye[0] + dir[0] * d;
        p[1] = eye[1] + dir[1] * d;
        p[2] = eye[2] + dir[2] * d;
        func_8032F64C(p, pm);
        func_80320ED8(pm, ATTACK_RADIUS, 1);
        while ((prop = func_8032F528()) != NULL) {
            if (target == NULL && prop->is_actor && prop->actorProp.marker != pm) {
                target = prop;
            }
        }
    }
    _player_getPosition(banjo_pos);
    func_8032F64C(banjo_pos, pm);
    for (i = 0; i < 3; i++) {
        pm->unk38[i] = saved_off[i];
    }
    return target;
}

// right click "uses" bk things: bottles, mumbo, brentilda, the lair's
// cauldrons and signs all wait for a fresh b press while banjo stands near.
// steve sends exactly that for one frame (see the bakey hook).
void steve_press_b(void) {
    steve_b_pulse = 1;
}

// Minecraft.startUseItem -> interact with the entity under the crosshair
static int mob_is_mc(ActorMarker *m);
int steve_interact(f32 eye[3], f32 dir[3], f32 max_dist) {
    Prop *p = steve_find_actor(eye, dir, max_dist, STEVE_ATTACK_REACH);
    // a mob has nothing to say (Mob.interact: PASS), the held item goes on
    if (p == NULL || mob_is_mc(p->actorProp.marker)) {
        return FALSE;
    }
    steve_press_b();
    return TRUE;
}

// bk's table of how each actor reacts to banjo's moves (code_B62B0.c
// D_80370AC0, looked up by marker id through D_80383F10). each row has a
// packed reaction per move, in D_80371DC0's order: claw, roll, fast fall,
// peck, barge, beak buster, beak bomb, egg, wonderwing, croc bite, touch.
// bits 11-10 of a reaction are the collision callback it calls (1, 2 = die).
typedef struct {
    u16 id;
    u16 all;
    u16 hit[11];
} BkHitRow;
extern BkHitRow D_80370AC0[];
extern s16 D_80383F10[];

// a punch is closest to banjo's ground attacks: the first of these the
// actor reacts to (a die reaction first). beak buster switches stay the
// smash's (landing while sneaking), eggs stay eggs
static const u8 punch_order[] = { 0, 4, 3, 1, 6 };
static const u8 punch_hitbox[] = {
    HITBOX_4_CLAW, HITBOX_7_ROLL, HITBOX_A_FAST_FALLING, HITBOX_5_PECK,
    HITBOX_2_BEAK_BARGE, HITBOX_1_BEAK_BUSTER, HITBOX_3_BEAK_BOMB,
};
static u8 steve_punch_as; // while steve's hit runs bk's collision: the hitbox to report

// no row in bk's table, or a die reaction to one of banjo's attacks
// (claw, roll, fast fall, peck, barge, buster, bomb)
static int bk_attack_can_kill(ActorMarker *m) {
    const BkHitRow *row;
    int k;

    if (m == NULL || m->id >= 0x2A9 || D_80383F10[m->id] < 0) return TRUE;
    row = &D_80370AC0[D_80383F10[m->id]];
    for (k = 0; k <= 6; k++) {
        if (((row->hit[k] >> 10) & 3) == 2) return TRUE;
    }
    return FALSE;
}

static int steve_punch_hitbox(ActorMarker *m) {
    const BkHitRow *row;
    int i, k, want;

    if (m == NULL || m->id >= 0x2A9 || D_80383F10[m->id] < 0) return 0;
    row = &D_80370AC0[D_80383F10[m->id]];
    for (want = 2; want >= 1; want--) {
        for (i = 0; i < (int)sizeof(punch_order); i++) {
            k = punch_order[i];
            if (((row->hit[k] >> 10) & 3) == want) return punch_hitbox[k];
        }
    }
    return 0;
}

// bk enemies get minecraft health: a few hits instead of bk's one-hit kill.
// the health lives in a marker extension (bkrecomp api), so it's per actor
// and goes away with it. only the killing blow is passed on to bk.
typedef struct {
    f32 damage_taken;
    u32 hurt_until; // gSteve.tick_count: LivingEntity.invulnerableTime (10 ticks)
    u8 used;        // a mob that's had its killing blow (loot dropped once)
    // what minecraft shows on a hurt mob (bk enemies only, mob_is_mc)
    u8 tracked;     // in mob_list
    u8 hurt_time;   // LivingEntity.hurtTime: the red overlay while > 0
    u8 dying;       // killed: LivingEntity.tickDeath runs
    s16 death_time; // LivingEntity.deathTime
    s16 marker_id;  // (a marker reused by another actor gets fresh data)
    f32 kb[3];      // deltaMovement from knockback, blocks per tick
    f32 hop, o_hop; // the knockback hop's height (bk units; only drawn)
    f32 roll0;      // the actor's roll before it fell over
} MobHealth;
static MarkerExtensionId mob_ext;
static int mob_ext_ok;

// the bk actors minecraft's hurt / death look is given to: the enemies
// survival.c maps to a mob by name, and enemies (bk's
// table says touching them costs honeycombs: the d field, bits 6-5, of the
// touch reaction, marker.c baMarker_8028D7B8) that a hit kills (a die
// reaction to one of the punch moves). bosses, switches, eggs and breakables
// keep bk's own reactions
static int mob_is_mc(ActorMarker *m) {
    const BkHitRow *row;
    Actor *a;
    int i;

    if (m == NULL || !m->unk3E_0) return FALSE;
    // the enemies survival.c maps by name are mobs whatever bk does on a hit
    // (limbo falls apart and gets up again, boom boxes blow up instead of
    // biting, sir slush shrugs hits off)
    a = marker_getActor(m);
    if (a != NULL && surv_mob_listed(a->modelCacheIndex)) return TRUE;
    if (m->id >= 0x2A9 || D_80383F10[m->id] < 0) return FALSE;
    row = &D_80370AC0[D_80383F10[m->id]];
    if (((row->hit[10] >> 5) & 3) == 0) return FALSE;
    for (i = 0; i < (int)sizeof(punch_order); i++) {
        if (((row->hit[punch_order[i]] >> 10) & 3) == 2) return TRUE;
    }
    return FALSE;
}

#define MOB_LIST_MAX 32
static ActorMarker *mob_list[MOB_LIST_MAX];
static int mob_count;
static MobHealth *mob_draw_mh; // the tracked mob __marker_draw is drawing
static Actor *mob_hop_actor;   // lifted by its hop for this draw
static f32 mob_hop_dy;

void steve_mobs_clear(void) {
    mob_count = 0;
    mob_draw_mh = NULL;
}

static MobHealth *mob_data(ActorMarker *m) {
    MobHealth *mh;
    if (!mob_ext_ok || m == NULL) return NULL;
    mh = bkrecomp_get_extended_marker_data(m, mob_ext);
    if (mh == NULL || !mh->tracked || mh->marker_id != m->id) return NULL;
    return mh;
}

static void mob_track(ActorMarker *m, MobHealth *mh) {
    int i;
    if (mh->tracked && mh->marker_id == m->id) {
        for (i = 0; i < mob_count; i++) if (mob_list[i] == m) return;
    }
    if (mob_count >= MOB_LIST_MAX) return;
    mh->tracked = TRUE;
    mh->marker_id = m->id;
    mob_list[mob_count++] = m;
}

// LivingEntity.knockback(power, xd, zd): (xd, zd) points from the mob to the
// attacker, so the push is along the hit's direction
static void mob_knockback(MobHealth *mh, f32 power, f32 dx, f32 dz) {
    f32 l = __builtin_sqrtf(dx * dx + dz * dz);
    if (l < 1e-4f) return;
    mh->kb[0] = mh->kb[0] / 2.0f + dx / l * power;
    mh->kb[2] = mh->kb[2] / 2.0f + dz / l * power;
    if (mh->hop <= 0.0f) {
        // onGround: min(0.4, y / 2 + power), y being gravity's -0.0784 a tick
        f32 vy = -0.0784f / 2.0f + power;
        mh->kb[1] = vy > 0.4f ? 0.4f : vy;
    }
}

// the mob's tick (20 / s): hurtTime counts down, the knockback moves it
// (LivingEntity.travel: gravity 0.08, drag 0.98, friction 0.6 * 0.91 on the
// ground, 0.91 in the air), tickDeath: 20 ticks, then the poof
static void mobs_tick(void) {
    int i, k;

    for (i = 0; i < mob_count; i++) {
        ActorMarker *m = mob_list[i];
        MobHealth *mh = mob_data(m);
        Actor *a = (mh != NULL) ? marker_getActor(m) : NULL;

        if (a == NULL || a->despawn_flag) {
            if (mh != NULL) mh->tracked = FALSE;
            mob_list[i--] = mob_list[--mob_count];
            continue;
        }
        if (mh->hurt_time > 0) mh->hurt_time--;
        mh->o_hop = mh->hop;
        if (mh->kb[0] != 0.0f || mh->kb[2] != 0.0f || mh->hop > 0.0f || mh->kb[1] > 0.0f) {
            f32 dy = mh->kb[1] * MC_BLOCK, fr;
            f32 mx = mh->kb[0] * MC_BLOCK, mz = mh->kb[2] * MC_BLOCK;
            f32 from[3], to[3], hit[3], n[3], hm = __builtin_sqrtf(mx * mx + mz * mz);

            // sideways: stopped by walls and slopes too steep to stand on
            // (Entity.collide), then put back on a floor that rose under it.
            // bk's ground enemies hold themselves on the floor with a ray
            // down from where they are: pushed into a slope, that ray starts
            // inside the ground and they stayed sunk in it
            if (hm > 0.01f) {
                from[0] = a->position[0];
                from[1] = a->position[1] + 0.5f * MC_BLOCK;
                from[2] = a->position[2];
                to[0] = from[0] + mx + mx / hm * 10.0f;
                to[1] = from[1];
                to[2] = from[2] + mz + mz / hm * 10.0f;
                if (steve_raycast(from, to, hit, n) && n[1] < 0.7f && n[1] > -0.7f) {
                    mh->kb[0] = mh->kb[2] = 0.0f;
                } else {
                    a->position[0] += mx;
                    a->position[2] += mz;
                    from[0] = to[0] = a->position[0];
                    from[2] = to[2] = a->position[2];
                    from[1] = a->position[1] + hm + 0.5f * MC_BLOCK;
                    to[1] = a->position[1];
                    if (steve_raycast(from, to, hit, n) && n[1] > 0.5f && hit[1] > a->position[1]) {
                        a->position[1] = hit[1];
                    }
                }
            }
            // the hop is only drawn (steve_mob_before_draw): bk keeps the
            // enemy's real height on its floor itself
            if (mh->hop + dy <= 0.0f) {
                mh->hop = 0.0f;
                mh->kb[1] = 0.0f;
            } else {
                mh->hop += dy;
                mh->kb[1] = (mh->kb[1] - 0.08f) * 0.98f;
            }
            fr = mh->hop > 0.0f ? 0.91f : 0.546f;
            mh->kb[0] *= fr;
            mh->kb[2] *= fr;
            if (mh->hop <= 0.0f && mh->kb[0] * mh->kb[0] + mh->kb[2] * mh->kb[2] < 1e-6f) {
                mh->kb[0] = mh->kb[2] = 0.0f;
            }
        }
        if (mh->dying) {
            // (bk's update stays off: its death would fly it away)
            m->actorUpdateFunc = NULL;
            m->unk2C_2 = 0;
            if (++mh->death_time >= 20) {
                // LivingEntity.makePoofParticles: 20 POOF in the box,
                // gaussian * 0.02 motion (a sum of uniforms stands in)
                f32 w, h;
                int n;
                surv_mob_size(a->modelCacheIndex, &w, &h);
                for (n = 0; n < 20; n++) {
                    f32 v[3], q[3];
                    for (k = 0; k < 3; k++) v[k] = (sound_random() + sound_random() + sound_random() - 1.5f) * 2.0f * 0.02f;
                    q[0] = a->position[0] + (w * (2.0f * sound_random() - 1.0f) - v[0] * 10.0f) * MC_BLOCK;
                    q[1] = a->position[1] + (h * sound_random() - v[1] * 10.0f) * MC_BLOCK;
                    q[2] = a->position[2] + (w * (2.0f * sound_random() - 1.0f) - v[2] * 10.0f) * MC_BLOCK;
                    particles_poof(q, v);
                }
                mh->tracked = FALSE;
                mob_list[i--] = mob_list[--mob_count];
                marker_despawn(m);
                continue;
            }
        }
    }
}

// the killing blow on a minecraft-looking mob: bk's update stops (no flying
// away), it can't be touched, the animation holds, and tickDeath runs
static void mob_start_death(ActorMarker *m, MobHealth *mh, Actor *a) {
    mob_track(m, mh);
    if (!mh->tracked) return; // (list full: bk's death as before)
    mh->dying = TRUE;
    mh->death_time = 0;
    mh->roll0 = a->roll;
    m->actorUpdateFunc = NULL;
    m->unk2C_2 = 0;
    m->collidable = FALSE;
    if (a->anctrl != NULL) anctrl_setPlaybackType(a->anctrl, ANIMCTRL_STOPPED);
}

// drawing: LivingEntityRenderer.setupRotations (dead: roll over by
// sqrt((deathTime + partial - 1) / 20 * 1.6) * getFlipDegrees 90) and the
// red overlay (hasRedOverlay = hurtTime > 0 || deathTime > 0)
RECOMP_HOOK("__marker_draw") void steve_mob_before_draw(ActorMarker *m, Gfx **gfx, Mtx **mtx, Vtx **vtx) {
    MobHealth *mh = mob_data(m);
    mob_draw_mh = NULL;
    mob_hop_actor = NULL;
    if (mh == NULL || !m->unk3E_0) return;
    // the knockback hop, interpolated like the entity's position
    if (mh->hop > 0.0f || mh->o_hop > 0.0f) {
        Actor *ha = marker_getActor(m);
        f32 pt = steve_partial_tick();
        if (ha != NULL) {
            mob_hop_dy = mh->o_hop + (mh->hop - mh->o_hop) * pt;
            ha->position[1] += mob_hop_dy;
            mob_hop_actor = ha;
        }
    }
    if (mh->dying) {
        Actor *a = marker_getActor(m);
        f32 fall = ((f32)mh->death_time + steve_partial_tick() - 1.0f) / 20.0f * 1.6f;
        fall = fall > 0.0f ? __builtin_sqrtf(fall) : 0.0f;
        if (fall > 1.0f) fall = 1.0f;
        if (a != NULL) a->roll = mh->roll0 + fall * 90.0f;
    }
    if (mh->hurt_time > 0 || mh->dying) mob_draw_mh = mh;
}

RECOMP_HOOK_RETURN("__marker_draw") void steve_mob_after_draw(void) {
    mob_draw_mh = NULL;
    if (mob_hop_actor != NULL) {
        mob_hop_actor->position[1] -= mob_hop_dy;
        mob_hop_actor = NULL;
    }
}

extern s32 modelRenderColorMode;
extern s32 modelRenderDynAlpha;
void modelRender_setPrimAndEnvColors(s32 env[4], s32 prim[4]);

// OverlayTexture's red (0xB2FF0000): entity.fsh mix(overlay.rgb, color.rgb,
// overlay.a) = color * 0.698 + red * 0.302. bk's models do the same with prim
// and env (code_C97F0 / mmm's flashes): env = how much of the model, prim =
// the colour. only over bk's plain colour modes (2 opaque, 3 faded)
static Gfx **tint_gfx; // the list a tinted model went into

RECOMP_HOOK("modelRender_draw") void steve_mob_tint(Gfx **gfx, Mtx **mtx, f32 pos[3], f32 rot[3], f32 scale, f32 *arg5, void *bin) {
    s32 env[4] = { 0xB2, 0xB2, 0xB2, 0xFF }, prim[4] = { 0xFF, 0x00, 0x00, 0x00 };
    tint_gfx = NULL;
    if (mob_draw_mh == NULL) return;
    if (modelRenderColorMode == 3) env[3] = modelRenderDynAlpha;
    else if (modelRenderColorMode != 2) return;
    modelRender_setPrimAndEnvColors(env, prim);
    tint_gfx = gfx;
}

// bk's plain colour modes never set prim (setup2CycleWhiteEnvDL), so the red
// would stay for whatever model is drawn next: back to black, what bk's own
// coloured draws leave (setup2CycleBlackPrimDL)
RECOMP_HOOK_RETURN("modelRender_draw") void steve_mob_untint(void) {
    if (tint_gfx != NULL) {
        gDPSetPrimColor((*tint_gfx)++, 0, 0, 0, 0, 0, 0);
        tint_gfx = NULL;
    }
}

// while bk runs its kill reaction on a mob that dies the minecraft way, its
// own death sounds are muted (a source started below volume 100 never plays:
// sfxSource_func_8030E2C4 / func_8030E280)
static int steve_mute_bk_sfx;
void sfxsource_setSampleRate(u8 indx, s32 sample_rate);
RECOMP_HOOK("sfxSource_func_8030E2C4") void steve_maybe_mute_sfx(u8 indx) {
    if (steve_mute_bk_sfx && indx != 0) sfxsource_setSampleRate(indx, 0);
}

RECOMP_CALLBACK("*", recomp_on_init) void steve_on_init(void) {
    mob_ext = bkrecomp_extend_marker_all(sizeof(MobHealth));
    mob_ext_ok = TRUE;
}

// Player.attack: damage = ATTACK_DAMAGE * (0.2 + strength^2 * 0.8), * 1.5
// for a crit (falling, not on ground/climbing/in water, strength > 0.9)
static f32 steve_attack_damage(void) {
    Steve *s = &gSteve;
    f32 scale = (f32)s->attack_strength_ticker / item_attack_delay(surv_held_item());
    f32 dmg;

    if (scale > 1.0f) scale = 1.0f;
    dmg = item_attack_damage(surv_held_item()) * (0.2f + scale * scale * 0.8f);
    if (scale > 0.9f && s->fall_distance > 0.0f && !s->on_ground && !s->eye_in_water && !s->sprinting) {
        dmg *= 1.5f;
    }
    return dmg;
}

// LivingEntity.hurt on a bk enemy: minecraft health (MobHealth), 10 tick
// invulnerability, knockback along dir; the killing blow goes to bk as an
// attack and drops the mob's loot. TRUE if the hit landed.
static int steve_hit_invul; // the last hit found the mob still invulnerable
static f32 steve_kb_bonus;    // Player.attack's sprint knockback (0.5) for the next hit

static int steve_damage_actor(Prop *target, f32 dmg, f32 dir[3]) {
    u8 saved_hitbox;
    ActorMarker *hm = target->actorProp.marker;
    Actor *ha = marker_getActor(hm);
    int is_mob = mob_is_mc(hm);
    int mask = (ha != NULL) ? surv_mob_mask(ha->modelCacheIndex) : 1;
    MobHealth *mhd = NULL;
    f32 kb_bonus = steve_kb_bonus;
    // the health of the minecraft mob it stands in for (survival.c)
    f32 max_hp = (ha != NULL) ? surv_mob_max_health(ha->modelCacheIndex) : 20.0f;

    steve_kb_bonus = 0.0f;
    steve_hit_invul = FALSE;
    if (mob_ext_ok) {
        ActorMarker *m = hm;
        MobHealth *mh = bkrecomp_get_extended_marker_data(m, mob_ext);

        mhd = mh;
        if (mh != NULL) {
            if (mh->tracked && mh->marker_id == m->id && mh->dying) return TRUE;
            // dead already (bk's reaction didn't remove it, e.g. mumbo's
            // mountain bull): no more hits, or every hit dropped loot again
            if (is_mob && mh->used) return TRUE;
            if (mh->hurt_until > gSteve.tick_count) {
                steve_hit_invul = TRUE;
                return TRUE; // invulnerable for 10 ticks after a hit
            }
            // bosses, switches, eggs and breakables aren't mobs: every hit
            // is bk's (grunty only swoops by for a moment, a switch is one hit)
            if (is_mob) mh->damage_taken += dmg;
            else mh->damage_taken = max_hp;
            mh->hurt_until = gSteve.tick_count + 10;
            recomp_printf("[mc] hit marker %d for %d/10 (total %d of %d)\n", m->id, (s32)(dmg * 10), (s32)mh->damage_taken, (s32)max_hp);
            if (is_mob && ha != NULL) {
                // LivingEntity.hurtServer: hurtTime = hurtDuration 10,
                // dealDefaultKnockback 0.4 (+ the sprint hit's 0.5),
                // handleDamageEvent's hurt sound (on the killing blow too)
                mob_track(m, mh);
                mh->hurt_time = 10;
                mob_knockback(mh, 0.4f, dir[0], dir[2]);
                if (kb_bonus > 0.0f) mob_knockback(mh, kb_bonus, dir[0], dir[2]);
                surv_mob_sound(ha->modelCacheIndex, FALSE, ha->position);
            }
            if (mh->damage_taken < max_hp) {
                return TRUE;
            }
        }
    }

    // the killing blow: a mob's loot drops where it stood (and KilledTrigger)
    if (ha != NULL && is_mob) {
        if (mhd != NULL) mhd->used = TRUE;
        advancements_killed_mob(mask);
        surv_mob_loot(ha->modelCacheIndex, ha->position);
        surv_mob_sound(ha->modelCacheIndex, TRUE, ha->position); // hurtServer: makeSound(getDeathSound)
    }
    // D_8037BFA0 makes hitbox_getHitboxForActor report an attack (it's the
    // beak bomb hit flag). bk has no "punch" hitbox: the move the actor's
    // reaction row answers to (steve_punch_hitbox), else the beak bomb.
    // (rbb's barge switches only react to a barge, the pink eggs to their
    // own moves)
    {
        ActorMarker *m = target->actorProp.marker;
        Actor *a = marker_getActor(m);
        s32 state_before = (a != NULL) ? a->state : -1;

        saved_hitbox = D_8037BFA0;
        D_8037BFA0 = 1;
        steve_punch_as = steve_punch_hitbox(m);
        steve_mute_bk_sfx = is_mob && mhd != NULL;
        __baMarker_resolveCollision(target);
        steve_punch_as = 0;
        D_8037BFA0 = saved_hitbox;
        // some actors only die to the hits bk uses on them (spiral
        // mountain's veggies: rolls, pecks). the faked beak bomb did nothing
        // to them (same state, still solid): call their die callback, what
        // bk does on the hit that kills them
        // (only for actors bk's table lets an attack kill, or that it doesn't
        // know: a boss that ignored the hit isn't killed this way)
        a = marker_getActor(m);
        if (a != NULL && a->state == state_before && m->collidable && m->dieFunc != NULL && bk_attack_can_kill(m)) {
            recomp_printf("[mc] marker %d ignored the hit: die callback\n", m->id);
            m->dieFunc(m, baMarker_get());
        }
        steve_mute_bk_sfx = FALSE;
        // a mob dies the minecraft way (bk's reaction already ran: its
        // honeycomb / note bundles and kill flags happen as usual)
        a = marker_getActor(m);
        if (is_mob && mhd != NULL && a != NULL && !a->despawn_flag) {
            mob_start_death(m, mhd, a);
        }
    }
    recomp_printf("[mc] hit marker %d\n", target->actorProp.marker->id);
    return TRUE;
}

f32 steve_current_attack_damage(void) {
    return steve_attack_damage();
}

// minecraft can only hit entities. bk's actors that an attack can do
// anything to have a die callback (enemies, breakables); props like benches
// and platforms don't, so a punch at them goes on to the blocks behind
static int steve_hittable(Prop *p) {
    // (or a reaction to one of banjo's attacks in bk's table: rbb's
    // switches only have a collision callback)
    return p != NULL && p->actorProp.marker != NULL
        && (p->actorProp.marker->dieFunc != NULL || steve_punch_hitbox(p->actorProp.marker) != 0
            || mob_is_mc(p->actorProp.marker));
}

int steve_attack(f32 eye[3], f32 dir[3], f32 max_dist) {
    Prop *target = steve_find_actor(eye, dir, max_dist, STEVE_ATTACK_REACH);

    Steve *s = &gSteve;
    f32 scale;
    int full, knockback, crit, sweep, r;

    if (!steve_hittable(target)) {
        return FALSE;
    }
    scale = (f32)s->attack_strength_ticker / item_attack_delay(surv_held_item());
    full = scale > 0.9f;
    knockback = full && s->sprinting;
    crit = full && s->fall_distance > 0.0f && !s->on_ground && !s->eye_in_water && !s->sprinting;
    sweep = FALSE;
    if (full && !crit && !knockback && s->on_ground && surv_held_item() < MC_ITEM_END) {
        const char *n = mc_registry_names[surv_held_item()];
        int l = 0;
        while (n[l]) l++;
        sweep = l > 6 && n[l - 6] == '_' && n[l - 5] == 's' && n[l - 4] == 'w' && n[l - 3] == 'o' && n[l - 2] == 'r' && n[l - 1] == 'd';
    }
    // Player.attack: a sprinting full hit adds knockback 0.5
    steve_kb_bonus = knockback ? 0.5f : 0.0f;
    r = steve_damage_actor(target, steve_attack_damage(), dir);
    // Player.attack's sounds (1, 1 at the player)
    if (steve_hit_invul) sound_play_player(SND(ENTITY_PLAYER_ATTACK_NODAMAGE), 1.0f, 1.0f);
    else {
        if (knockback) sound_play_player(SND(ENTITY_PLAYER_ATTACK_KNOCKBACK), 1.0f, 1.0f);
        if (crit) sound_play_player(SND(ENTITY_PLAYER_ATTACK_CRIT), 1.0f, 1.0f);
        if (sweep) sound_play_player(SND(ENTITY_PLAYER_ATTACK_SWEEP), 1.0f, 1.0f);
        if (!crit && !sweep) sound_play_player(full ? SND(ENTITY_PLAYER_ATTACK_STRONG) : SND(ENTITY_PLAYER_ATTACK_WEAK), 1.0f, 1.0f);
    }
    return r;
}

// an arrow's move this tick (bk units): the first enemy along it takes the hit
int steve_arrow_hit(f32 from[3], f32 to[3], f32 damage) {
    f32 dir[3], len;
    Prop *target;
    int i;

    for (i = 0; i < 3; i++) dir[i] = to[i] - from[i];
    len = __builtin_sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 0.01f) return FALSE;
    for (i = 0; i < 3; i++) dir[i] /= len;
    target = steve_find_actor(from, dir, len, len);
    if (!steve_hittable(target)) return FALSE;
    return steve_damage_actor(target, damage, dir);
}

// would a block at this cell overlap steve?
int steve_overlaps_cell(s32 c[3]) {
    f32 r = STEVE_HALF_WIDTH * MC_BLOCK;
    f32 x0 = c[0] * MC_BLOCK, y0 = c[1] * MC_BLOCK, z0 = c[2] * MC_BLOCK;
    f32 *p = gSteve.pos;

    return (p[0] + r > x0) && (p[0] - r < x0 + MC_BLOCK)
        && (p[1] + steve_height() > y0) && (p[1] < y0 + MC_BLOCK)
        && (p[2] + r > z0) && (p[2] - r < z0 + MC_BLOCK);
}

RECOMP_HOOK_RETURN("baphysics_update") void steve_after_physics(void) {
    Steve *s = &gSteve;

    if (!s->enabled) {
        return;
    }

    // mouse look only while steve is in control. in cutscenes bk turns banjo,
    // and steve's body and head follow that instead (the head used to keep
    // turning with the mouse)
    if (s->driving) {
        steve_update_look(time_getDelta());
    } else {
        s->yaw = yaw_get();
        s->pitch = 0.0f;
        s->body_rot = s->o_body_rot = -s->yaw;
        mouse_get_look_delta(&(f32){ 0.0f }, &(f32){ 0.0f }); // drop the moves
    }
    if (gSteveSmashFrames > 0) {
        gSteveSmashFrames--;
    }
    // spectators touch nothing: bk's touch spheres (enemies, notes, doors)
    // ignore banjo's marker while it isn't collidable
    {
        static int was_spectating;
        ActorMarker *pm = baMarker_get();
        if (pm != NULL && GM_SPECTATING()) {
            pm->collidable = FALSE;
        } else if (pm != NULL && was_spectating) {
            pm->collidable = TRUE;
        }
        was_spectating = GM_SPECTATING();
    }
    if (steve_climb_frames > 0) {
        steve_climb_frames--;
    }

    if (!s->driving) {
        // the game is driving banjo. steve just follows.
        f32 banjo_pos[3];

        _player_getPosition(banjo_pos);
        steve_snap_to(banjo_pos);
        steve_prev_driving = FALSE;
        return;
    }

    // just arrived somewhere: wait for a floor under us (max 60 frames,
    // at least 3 so bk has placed banjo for real)
    if (steve_settle_frames > 0) {
        f32 down[3], hit[3], n[3];

        down[0] = s->pos[0];
        down[1] = s->pos[1] - 1000.0f;
        down[2] = s->pos[2];
        steve_settle_frames--;
        if (steve_settle_frames < 57 && steve_raycast(s->pos, down, hit, n)) {
            steve_settle_frames = 0;
        }
        if (steve_settle_frames < 57 && player_getWaterState() != 0) {
            steve_settle_frames = 0;
        }
        if (steve_settle_frames > 0) {
            // bk owns banjo while a map is being set up (it places him at
            // the entrance over these frames). steve only follows, never
            // pushes banjo back: that kept banjo at the old map's spot and
            // the game reloaded the map forever.
            f32 banjo_pos[3];

            _player_getPosition(banjo_pos);
            vec3_copy(s->prev_pos, banjo_pos);
            vec3_copy(s->pos, banjo_pos);
            vec3_copy(s->render_pos, banjo_pos);
            s->tick_accum = 0.0f;
            s->vel[0] = s->vel[1] = s->vel[2] = 0.0f;
            steve_prev_driving = TRUE;
            return;
        }
    }

    steve_sync_health();
    // steve takes minecraft fall damage, so keep bk's fall damage from also
    // counting (it measures from where this says the fall started)
    bafalldamage_start();

    // fixed 20 tick/s clock, independent of framerate
    s->tick_accum += time_getDelta();
    if (s->tick_accum > 0.25f) {
        s->tick_accum = 0.25f; // don't spiral after a hitch
    }
    while (s->tick_accum >= MC_TICK_SECONDS) {
        // Minecraft.tick: handleKeybinds (attack / use / mining) before the
        // player ticks. clicks since the last tick are all handled here.
        f32 eye[3], dir[3];
        OSTime tick_start = osGetTime();

        eye[0] = s->pos[0];
        eye[1] = s->pos[1] + steve_eye_height();
        eye[2] = s->pos[2];
        steve_look_dir(dir);
        surv_tick(eye, dir, gSteveInput.attack_clicks, gSteveInput.attack_held,
            gSteveInput.use_clicks, gSteveInput.use_held);
        gSteveInput.attack_clicks = 0;
        gSteveInput.use_clicks = 0;

        vec3_copy(s->prev_pos, s->pos);
        steve_tick();
        surv_items_tick();
        mobs_tick();
        // MinecraftServer.tickServer: smoothedTickTimeMillis (the f3 tps
        // line). 46875 osGetTime ticks per ms
        gSmoothedTickMs = gSmoothedTickMs * 0.8f + (f32)(u32)(osGetTime() - tick_start) / 46875.0f * 0.2f;
        s->tick_accum -= MC_TICK_SECONDS;
    }

    // interpolate between the last two ticks for smooth drawing
    {
        f32 a = s->tick_accum / MC_TICK_SECONDS;
        f32 vel_units[3];
        int i;

        // in a boat the view turns with it, interpolated like the boat
        // (LocalPlayer.getViewYRot for a passenger)
        if (boat_riding()) s->yaw += boat_take_turn(a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a));
        for (i = 0; i < 3; i++) {
            s->render_pos[i] = s->prev_pos[i] + (s->pos[i] - s->prev_pos[i]) * a;
            vel_units[i] = s->vel[i] * MC_BLOCK * 20.0f;
        }
        // standing on the ground: bk's player_isStable wants a falling
        // velocity (gravity keeps pulling a grounded banjo). with 0, bottles,
        // mumbo and every "stand still and press b" check refused to talk
        if (s->on_ground && vel_units[1] >= 0.0f) {
            vel_units[1] = -1.0f;
        }
        player_setPosition(s->render_pos);
        // give banjo steve's velocity so his states (falling, landing) make sense
        baphysics_set_velocity(vel_units);
    }
    yaw_set(s->yaw);
    yaw_setIdeal(s->yaw);
    steve_prev_driving = TRUE;
}

// platforms and other game pushes are a one-frame offset added to banjo right
// after physics. steve gets the same push.
RECOMP_HOOK("func_8029858C") void steve_before_apply_offset(void) {
    f32 off[3];
    int i;

    if (!gSteve.enabled || !gSteve.driving || boat_riding()) {
        return; // (in a boat, the boat places steve)
    }
    func_80298540(off);
    for (i = 0; i < 3; i++) {
        gSteve.prev_pos[i] += off[i];
        gSteve.pos[i] += off[i];
        gSteve.render_pos[i] += off[i];
    }
}

// banjo's terrain collision just ran on banjo. steve already did his own, so
// put banjo back exactly where steve is.
RECOMP_HOOK_RETURN("func_80293F0C") void steve_after_banjo_collision(void) {
    if (!gSteve.enabled) {
        return;
    }
    if (gSteve.driving && steve_settle_frames == 0) {
        player_setPosition(gSteve.render_pos);
    }
    _player_getPosition(steve_last_set_pos);
    steve_have_last_set = TRUE;
    steve_read_water();
}

// --- eggs ---
// steve throws bk's own blue eggs like minecraft's egg item: right click,
// in the look direction. bk shoots them as a "common particle" from banjo's
// head (fxegg_head_spawn), which we then aim.
// ThrownEgg / Snowball: shootFromRotation(player, xRot, yRot, 0, 1.5, 1.0)
// = 1.5 blocks/tick. (bk's own egg is 800 units/s, flat.)
#define EGG_SPEED (1.5f * MC_BLOCK * 20.0f)
static int egg_aim_pending;
static f32 egg_aim_pos[3], egg_aim_dir[3];

void steve_throw_egg(f32 eye[3], f32 dir[3]) {
    int i;

    if (item_getCount(ITEM_D_EGGS) <= 0) {
        return;
    }
    for (i = 0; i < 3; i++) {
        egg_aim_dir[i] = dir[i];
        egg_aim_pos[i] = eye[i] + dir[i] * 20.0f;
    }
    egg_aim_pending = TRUE;
    commonParticle_new(COMMON_PARTICLE_1_EGG_HEAD, 1);
    egg_aim_pending = FALSE;
    item_dec(ITEM_D_EGGS);
    ability_use(ABILITY_USED_EGG);
    func_8030E760(SFX_57_KAZOOIE_HEGH, 1.0f, 0x7fff);
}

RECOMP_HOOK_RETURN("fxegg_head_spawn") void steve_after_egg_spawn(void) {
    u8 proj, motion;
    f32 vel[3];

    if (!egg_aim_pending) {
        return;
    }
    proj = commonParticle_getCurrentProjectileIndex();
    motion = func_8033E93C();
    projectile_setPosition(proj, egg_aim_pos);
    func_80344D94(motion, egg_aim_pos);
    vel[0] = egg_aim_dir[0] * EGG_SPEED;
    vel[1] = egg_aim_dir[1] * EGG_SPEED;
    vel[2] = egg_aim_dir[2] * EGG_SPEED;
    func_80344E3C(motion, vel);
}

// --- bk's "what move is banjo doing" for hits ---
// RECOMP_PATCH: the answer has to change, and a hook can't change a return
// value. same as bk's (code_6B30.c), plus steve's smash first.
RECOMP_PATCH enum hitbox_e hitbox_getHitboxForActor(ActorMarker *arg0) {
    if (gSteve.enabled && gSteveSmashFrames > 0) {
        return HITBOX_1_BEAK_BUSTER;
    }
    if (steve_punch_as)
        return steve_punch_as;
    if (D_8037BFA0)
        return HITBOX_3_BEAK_BOMB;

    switch (bs_getState()) {
        case BS_F_BBUSTER:
            return bsbbuster_hitboxActive() ? HITBOX_1_BEAK_BUSTER : HITBOX_0_NONE;
        case BS_BBARGE:
            return bsbbarge_hitboxActive() ? HITBOX_2_BEAK_BARGE : HITBOX_0_NONE;
        case BS_BOMB:
            return bsbfly_bombHitboxActive() ? HITBOX_3_BEAK_BOMB : HITBOX_0_NONE;
        case BS_CLAW:
            if (arg0 && !func_8028AED4(marker_getActor(arg0)->position, 90.0f))
                return HITBOX_0_NONE;
            if (bsclaw_hitboxActive())
                return HITBOX_4_CLAW;
            return HITBOX_8_CLAW_DOWN;
        case BS_11_BPECK:
            if (arg0 && !func_8028AED4(marker_getActor(arg0)->position, 60.0f))
                return HITBOX_0_NONE;
            return bsbpeck_hitboxActive() ? HITBOX_5_PECK : HITBOX_0_NONE;
        case BS_1A_WONDERWING_ENTER:
        case BS_1B_WONDERWING_IDLE:
        case BS_1C_WONDERWING_WALK:
        case BS_1D_WONDERWING_JUMP:
        case BS_1E_WONDERWING_EXIT:
        case BS_A4_WONDERWING_DRONE:
        case BS_A5_WONDERWING_UNKA5:
            return HITBOX_6_WONDERWING;
        case BS_ROLL:
            return bstwirl_hitboxActive() ? HITBOX_7_ROLL : HITBOX_0_NONE;
        case BS_6E_CROC_BITE:
            return bscroc_hitboxActive() ? HITBOX_9_CROC_BITE : HITBOX_0_NONE;
        case BS_5_JUMP:
        case BS_3D_FALL_TUMBLING:
            if (baphysics_get_vertical_velocity() < 0.0f && !player_isStable())
                return HITBOX_A_FAST_FALLING;
        case BS_2F_FALL:
            if (baphysics_get_vertical_velocity() < -1400.0f && !player_isStable())
                return HITBOX_A_FAST_FALLING;
        default:
            return HITBOX_0_NONE;
    }
}

// bk's slide dust (bs/slide.c). banjo's state machine still thinks he's
// sliding when steve stands on a steep slope, and this puffed smoke at
// steve's feet. RECOMP_PATCH: a hook can't skip the original; the body below
// is bk's own, it just doesn't run while steve is in control.
extern s32 D_8037D524;
void func_802589E4(f32 dst[3], f32 yaw, f32 distance);
void func_80352CF4(f32 *pos, f32 *vel, f32 arg2, f32 arg3);

RECOMP_PATCH void func_802B40D0(void) {
    f32 sp44[3];
    f32 sp38[3];
    f32 sp2C[3];
    f32 sp28;

    if (gSteve.enabled && gSteve.driving) {
        return;
    }
    _player_getPosition(sp38);
    sp38[1] += 20.0f;
    D_8037D524++;
    if (!(D_8037D524 < 3))
        D_8037D524 = 0;

    if (D_8037D524) {
        sp28 = mlNormalizeAngle(yaw_get() + 90.0f);
        func_802589E4(sp2C, sp28, randf() * 10.0f + 20.0f);
        sp2C[1] = 0.0f;
    }
    if (D_8037D524 == 1) {
        ml_vec3f_scale(sp2C, -1.0f);
        sp38[0] += sp2C[0];
        sp38[1] += sp2C[1];
        sp38[2] += sp2C[2];
    } else if (D_8037D524 == 2) {
        sp38[0] += sp2C[0];
        sp38[1] += sp2C[1];
        sp38[2] += sp2C[2];
    }
    func_802589E4(sp44, yaw_get(), 40.0f);
    sp44[1] = 50.0f;
    func_80352CF4(sp38, sp44, 10.0f, 150.0f);
}

extern u8 D_8037BF60; // bk's floor flag (playerutils.c)

// bk's floor flag (D_8037BF60) comes from banjo's own floor probe, which
// misses while steve drives (logged: standing next to bottles, floor 0).
// steve's ground is the real one; player_isStable reads this flag.
RECOMP_HOOK_RETURN("func_8028B71C") void steve_floor_flag(void) {
    if (gSteve.enabled && gSteve.driving && gSteve.on_ground) D_8037BF60 = 1;
}

// ServerExplosion.getSeenPercent: rays from a grid over steve's box to the
// center, the share that reach it (bk units)
static f32 steve_seen_percent(const f32 c[3]) {
    f32 w = STEVE_HALF_WIDTH * 2.0f, h = steve_height() / MC_BLOCK;
    f32 xs = 1.0f / (w * 2.0f + 1.0f), ys = 1.0f / (h * 2.0f + 1.0f), zs = xs;
    f32 xo = (1.0f - (f32)(s32)(1.0f / xs) * xs) / 2.0f, zo = xo;
    f32 xx, yy, zz;
    int hits = 0, count = 0;

    for (xx = 0.0f; xx <= 1.0f; xx += xs) {
        for (yy = 0.0f; yy <= 1.0f; yy += ys) {
            for (zz = 0.0f; zz <= 1.0f; zz += zs) {
                f32 from[3], to[3], hit[3], n[3];
                from[0] = gSteve.pos[0] + (-w / 2.0f + xx * w + xo) * MC_BLOCK;
                from[1] = gSteve.pos[1] + yy * h * MC_BLOCK;
                from[2] = gSteve.pos[2] + (-w / 2.0f + zz * w + zo) * MC_BLOCK;
                to[0] = c[0];
                to[1] = c[1];
                to[2] = c[2];
                if (!steve_raycast(from, to, hit, n)) hits++;
                count++;
            }
        }
    }
    return count ? (f32)hits / count : 0.0f;
}

void steve_explosion_hurt(const f32 center[3], f32 radius) {
    f32 dr = radius * 2.0f;
    ActorMarker *pm = baMarker_get();
    Prop *hit[16];
    int nhit = 0, i;

    // steve (not in spectator; creative players still get pushed unless flying)
    if (!GM_SPECTATING()) {
        f32 dx = gSteve.pos[0] - center[0], dy = gSteve.pos[1] - center[1], dz = gSteve.pos[2] - center[2];
        f32 dist = __builtin_sqrtf(dx * dx + dy * dy + dz * dz) / MC_BLOCK / dr;
        if (dist <= 1.0f) {
            f32 eye[3], d[3], len, exposure = steve_seen_percent(center), pw, kb;
            eye[0] = gSteve.pos[0];
            eye[1] = gSteve.pos[1] + steve_eye_height();
            eye[2] = gSteve.pos[2];
            for (i = 0; i < 3; i++) d[i] = eye[i] - center[i];
            len = __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            pw = (1.0f - dist) * exposure;
            // ExplosionDamageCalculator.getEntityDamageAmount
            {
                // (a shield toward the blast blocks it, like any hit with a source)
                f32 dmgx = shield_absorb((pw * pw + pw) / 2.0f * 7.0f * dr + 1.0f, center);
                if (dmgx > 0.0f) steve_hurt(dmgx);
            }
            kb = (1.0f - dist) * exposure; // knockbackMultiplier 1, no resistance
            if (len > 0.01f && !(gSurv.creative && gSteve.flying)) {
                for (i = 0; i < 3; i++) gSteve.vel[i] += d[i] / len * kb;
            }
        }
    }
    // bk's enemies near the blast (the same damage, exposure taken as 1)
    if (pm == NULL) return;
    {
        f32 banjo_pos[3];
        s16 saved_off[3];
        Prop *prop;
        for (i = 0; i < 3; i++) {
            saved_off[i] = pm->unk38[i];
            pm->unk38[i] = 0;
        }
        func_8032F64C((f32 *)center, pm);
        func_80320ED8(pm, dr * MC_BLOCK, 1);
        while ((prop = func_8032F528()) != NULL) {
            if (prop->is_actor && prop->actorProp.marker != pm && prop->actorProp.marker->dieFunc != NULL && nhit < 16) hit[nhit++] = prop;
        }
        _player_getPosition(banjo_pos);
        func_8032F64C(banjo_pos, pm);
        for (i = 0; i < 3; i++) pm->unk38[i] = saved_off[i];
    }
    for (i = 0; i < nhit; i++) {
        Actor *a = marker_getActor(hit[i]->actorProp.marker);
        f32 d[3], dist, pw;
        int k;
        if (a == NULL) continue;
        for (k = 0; k < 3; k++) d[k] = a->position[k] - center[k];
        dist = __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / MC_BLOCK / dr;
        if (dist > 1.0f) continue;
        pw = 1.0f - dist;
        steve_damage_actor(hit[i], (pw * pw + pw) / 2.0f * 7.0f * dr + 1.0f, d);
    }
}

// BaseFireBlock.fireIgnite (for a ServerPlayer) then the inFire damage
void steve_in_fire_block(void) {
    Steve *s = &gSteve;
    if (GM_SPECTATING()) return;
    if (s->fire_ticks < 0) {
        s->fire_ticks++;
    } else {
        s->fire_ticks += 1 + (gSteve.tick_count % 2); // random.nextInt(1, 3)
    }
    if (s->fire_ticks >= 0 && s->fire_ticks < 160) s->fire_ticks = 160; // igniteForSeconds(8)
    steve_hurt(1.0f);
}

int steve_on_fire(void) {
    return gSteve.fire_ticks > 0 && !GM_SPECTATING();
}
