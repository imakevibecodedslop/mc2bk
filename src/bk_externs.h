#ifndef __MC_BK_EXTERNS_H__
#define __MC_BK_EXTERNS_H__

// game functions and globals we use that the decomp headers don't declare (or
// declare in headers that are awkward to include). names must match
// BanjoRecompSyms. see NOTES.md for what each one does.

#include <ultra64.h>

// --- functions ---

// time.c: game delta time in seconds, already capped at 0.05 (3 vi frames)
f32 time_getDelta(void);

// code_AD5B0.c: current map id (enum map_e)
s32 map_get(void);

// ba/model.c: banjo model visibility flag, checked at the top of baModel_draw
void baModel_setVisible(s32 visible);
s32 baModel_isVisible(void);

// code_6DA30.c: queue a string for the 2d print pass. stores the POINTER, so
// the string must stay alive until printbuffer_draw runs that frame.
void print_dialog(s32 x, s32 y, u8 *string);

// player position (code_11460.c)
void _player_getPosition(f32 dst[3]);
void player_setPosition(f32 src[3]);
// one-frame position offset that func_8029858C adds after physics (platforms etc)
void func_80298540(f32 dst[3]);

// ba/physics.c: use "core2/ba/physics.h"

// yaw.c, degrees. yaw 0 faces +z, direction = (sin(yaw), 0, cos(yaw))
void yaw_set(f32 yaw);
void yaw_setIdeal(f32 yaw);
f32 yaw_get(void);

// player state helpers (code_7060.c)
s32 player_getWaterState(void);
s32 player_movementGroup(void);

// ba/marker.c: collision flags banjo filters out, banjo ors 0x1E0000 into it
s32 baMarker_8028D694(void);

// code_999A0.c: line segment vs all collision providers (map + props).
// shortens `end` to the nearest hit, writes the unit normal. returns the hit
// BKCollisionTri* or NULL. tris whose flags & flag_filter are skipped.
void *func_80320B98(f32 start[3], f32 end[3], f32 out_normal[3], u32 flag_filter);
// the getter result of the last line query's hit: the level model bin for
// level hits, NULL for objects (models placed in cubes) and mod providers
void *func_803209EC(void);
Cube *cubeList_GetCubeAtPosition_f32(f32 position[3]);

#include "bkrecomp_api.h"
// moves / eggs
#include "core2/commonParticle.h"
#include "core2/abilityprogress.h"
#include "bs_funcs.h"
int ability_isUnlocked(enum ability_e uid);
void ability_use(enum ability_used ability);
int commonParticle_new(enum common_particle_e particle_id, int arg1);
u8 commonParticle_getCurrentProjectileIndex(void);
u8 func_8033E93C(void);
int bsbfly_bombHitboxActive(void);
s32 bsbbuster_hitboxActive(void);
int bsbbarge_hitboxActive(void);
int bsbpeck_hitboxActive(void);
int bscroc_hitboxActive(void);
int player_isStable(void);
void item_set(s32 item, s32 val);

// code_999A0.c: add a collision provider (line, moving sphere, sphere query
// functions; NULL to skip one). the list is reset every map load.
void func_80320B24(void *line_fn, void *moving_sphere_fn, void *sphere_fn);

// core1/viewport.c: camera. rotation is (pitch, yaw, roll) in degrees. the
// projection is rotZ(-roll) * rotX(-pitch) * rotY(-yaw), looking down -z, so
// yaw y looks along (-sin y, 0, -cos y) and positive pitch looks up.
void viewport_getPosition_vec3f(f32 dst[3]);
void viewport_setPosition_vec3f(f32 src[3]);
void viewport_getRotation_vec3f(f32 dst[3]);
void viewport_setRotation_vec3f(f32 src[3]);
void viewport_getLookVector(f32 dst[3]);
// recomputes the frustum planes and look vector from position/rotation
void viewport_update(void);

// core1/code_15B30.c: 2d fill rectangle in framebuffer pixels
void drawRectangle2D(Gfx **gfx, s32 x, s32 y, s32 w, s32 h, s32 r, s32 g, s32 b);

// core1/ml.c
f32 ml_sin_deg(f32 deg);
f32 ml_cos_deg(f32 deg);

// core1/mlmtx.c: the game's matrix stack
void mlMtxIdent(void);
void mlMtxTranslate(f32 x, f32 y, f32 z);
void mlMtxRotYaw(f32 deg);
void mlMtxScale_xyz(f32 x, f32 y, f32 z);
void mlMtxApply(Mtx *dst);

// --- globals (from bk.us.rev0.datasyms.toml) ---

// bakey.c, filled by bakey_update each frame. index with enum button_e.
typedef struct {
    s32 pressed_count[0xE]; // frames held, 0 = not held
    s32 released_count[0xE];
    u8 state[0xE];
    u8 prev_state[0xE];
} BaKey;
extern BaKey bakey;
// pfsmanager.c: this frame's controller buttons (face_button = press counters)
#include "core1/pfsmanager.h"
extern PfsManagerControllerData D_80281138[4];

// bastick.c, filled by bastick_update each frame
typedef struct {
    f32 zone_position;
    s32 zone;
    f32 zone_markers[5];
    f32 value[2]; // x, y in -1..1
    f32 angle;
    f32 distance;
    s32 zeroed_count;
    s32 nonzero_count;
    u8 locked_at_zero;
} BaStick;
extern BaStick bastick;

// code_7060.c: TRUE while the player exists in the map (player_draw checks it)
extern u8 D_8037BFB8;

// core1/viewport.c: near clip plane, used when the projection is built
extern f32 sViewportNear;
// core1/viewport.c: vertical field of view in degrees (bk default 40)
extern f32 sViewportFOVy;
// rebuilds the projection from the viewport globals and loads an identity modelview
void viewport_setRenderPerspectiveMatrix(Gfx **gfx, Mtx **mtx, f32 near, f32 far);
extern f32 sViewportFar;
extern f32 sViewportAspect;
void guPerspective(Mtx *m, u16 *perspNorm, f32 fovy, f32 aspect, f32 near, f32 far, f32 scale);
void guMtxIdent(Mtx *m);
void guScale(Mtx *m, f32 x, f32 y, f32 z);
void guOrtho(Mtx *m, f32 l, f32 r, f32 b, f32 t, f32 n, f32 f, f32 scale);

// framebuffer size in pixels (320x240 normally)
extern s32 gFramebufferWidth;
extern s32 gFramebufferHeight;

// --- attacking (see steve_attack) ---
#include "prop.h"
ActorMarker *baMarker_get(void);
// move a marker / query props touching a sphere at it / iterate the results
void func_8032F64C(f32 pos[3], ActorMarker *marker);
void func_80320ED8(ActorMarker *marker, f32 radius, s32 arg2);
Prop *func_8032F528(void);
// banjo's collision response with one prop (pickups, enemies, switches...)
void __baMarker_resolveCollision(Prop *other_prop);
// code_6B30.c: while nonzero, banjo's active hitbox is "beak bomb"
extern u8 D_8037BFA0;

// item.c: counts (ITEM_14_HEALTH = honeycombs, ITEM_15_HEALTH_TOTAL = max)
s32 item_getCount(s32 item);
void item_adjustByDiffWithHud(s32 item, s32 diff);

// water, from banjo's floor probe (code_C4B0.c): when the floor under banjo is
// water, "floor y" is the water surface. D_8037BF61 is banjo's in-water flag
// (playerutils.c), it's what puts banjo into his swim states.
s32 floor_isCurrentFloorunk59(void);
f32 floor_getCurrentFloorYPosition(void);
extern u8 D_8037BF61;

// banjo's hurt state plumbing (code_7060.c / ba code), invulnerability frames
void func_80296CB4(s32 arg0);
void func_80296CA8(ActorMarker *marker);
void func_80296CC0(f32 pos[3]);
void baiFrame_startWithValue(f32 seconds);
Actor *marker_getActor(ActorMarker *marker);

// climb.c: set by climbSet when banjo is in a climbable area, makes banjo grab on
extern u8 D_8037C590;

#endif
