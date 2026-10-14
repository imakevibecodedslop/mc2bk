#ifndef __MC_STEVE_H__
#define __MC_STEVE_H__

#include <ultra64.h>

// 1 minecraft block in bk units. see NOTES.md "units".
#define MC_BLOCK 80.0f

// steve's hitbox, in blocks (minecraft player: 0.6 x 1.8, step height 0.6)
#define STEVE_HALF_WIDTH 0.3f
#define STEVE_HEIGHT 1.8f
#define STEVE_CROUCH_HEIGHT 1.5f // Player crouching dimensions
#define STEVE_STEP_HEIGHT 0.6f

// minecraft runs 20 ticks per second
#define MC_TICK_SECONDS 0.05f

typedef struct {
    int enabled;  // steve mode on (d-pad up)
    int driving;  // steve is moving the hidden banjo this frame (false while the game drives banjo)

    // position of steve's feet in bk units. prev/cur are tick states,
    // render is interpolated between them for this frame.
    f32 prev_pos[3];
    f32 pos[3];
    f32 render_pos[3];

    f32 vel[3];   // blocks per tick, like minecraft's deltaMovement
    f32 yaw;      // bk degrees, 0 = +z. turning right makes it smaller.
    f32 pitch;    // degrees, positive looks up, -90..90

    int first_person; // camera_type == CAMERA_FIRST_PERSON (kept for readability)
    int camera_type;  // minecraft's CameraType, f5 / d-pad right cycles it

    // body rotation (LivingEntity.yBodyRot), minecraft degrees (= -bk yaw)
    f32 body_rot, o_body_rot;


    int on_ground;
    int horizontal_collision;
    int sprinting;
    int sneaking;
    int no_jump_delay;
    int sprint_trigger_time; // LocalPlayer.sprintTriggerTime (double tap w)
    int flying;              // Abilities.flying (creative)
    s32 fire_ticks;          // Entity.remainingFireTicks
    int jump_trigger_time;   // LocalPlayer.jumpTriggerTime (double tap jump)
    int was_jumping;
    int swimming;            // Entity.isSwimming (sprinting underwater)
    int had_forward;

    f32 tick_accum; // seconds not yet simulated
    u32 tick_count;

    // arm swing (LivingEntity.swing / getAttackAnim), 6 ticks long for players
    int swinging;
    int swing_time;
    f32 attack_anim, o_attack_anim;
    f32 eye_height, o_eye_height; // camera eye height (Camera.eyeHeight), bk units

    // fov modifier (Camera.tickFov), 1.0 walking, 1.15 sprinting
    f32 fov_mod, o_fov_mod;

    // WalkAnimationState: limb swing position and speed
    f32 walk_pos, o_walk_pos;
    f32 swim_amount, o_swim_amount; // LivingEntity.swimAmount (0..1, model lies down)
    f32 walk_speed, o_walk_speed;

    // health (minecraft hp, 20 = 10 hearts), synced with bk's honeycombs
    f32 health;
    s32 last_bk_health;
    int health_init;
    f32 fall_distance;
    int hurt_time;    // LivingEntity.hurtTime: 10 after a hit (red flash, hurt camera)
    int invul_time;   // Entity.invulnerableTime: 20 after a hit
    f32 hurt_dir;     // Player.hurtDir: where the hit came from, relative to the look
    f32 o_hurt_time;  // for interpolating the hurt camera

    // FirstPersonHandsAndItems: equip animation
    f32 hand_height, o_hand_height;
    u16 hand_item;              // what the hand shows (catches up with the hotbar)
    int attack_strength_ticker; // LivingEntity.attackStrengthTicker

    // water (Entity.updateInWaterStateAndDoFluidPushing / baseTick)
    int in_water;
    int eye_in_water;
    f32 water_height;    // blocks of water above the feet
    int air_supply;      // 300 = full (Entity.getMaxAirSupply)

    // FoodData
    int food_level;     // 0-20
    f32 saturation;
    f32 exhaustion;
    int food_timer;

    // view bob (ClientAvatarState): walk distance and bob strength
    f32 walk_dist, o_walk_dist;
    f32 bob, o_bob;
    // arm sway (LocalPlayer.xBob/yBob), minecraft convention: x = down, y = turning right
    f32 x_bob, o_x_bob, y_bob, o_y_bob;

} Steve;

// first person camera this frame, set by fpcam.c before the scene draws
typedef struct {
    f32 eye[3];   // steve's eye without view bob
    f32 cam[3];   // where the camera actually is (with bob)
} FpView;
extern FpView gFpView;

// 0..1 between the last tick and the next, for interpolating animations
f32 steve_partial_tick(void);
void steve_mobs_clear(void); // a new map: the hurt / dying mobs list goes
f32 steve_lerp(f32 a, f32 b, f32 t);
f32 steve_attack_anim(f32 pt);

enum { CAMERA_FIRST_PERSON, CAMERA_THIRD_PERSON_BACK, CAMERA_THIRD_PERSON_FRONT };

// third person camera: 8 jittered rays back from the eye, nearest hit wins
f32 steve_camera_max_zoom(f32 eye[3], f32 back_dir[3], f32 dist);

// minecraft's default fov (vertical degrees)
#define MC_FOV 70.0f

void steve_draw_hand(Gfx **gfx, Mtx **mtx);
void steve_draw_fire_overlay(Gfx **gfx, Mtx **mtx); // ScreenEffectRenderer.submitFire
// the player in the inventory screen, following the mouse (gui pixels)
void steve_draw_gui(Gfx **gfx, f32 x0, f32 y0, f32 x1, f32 y1, f32 size, f32 mouse_x, f32 mouse_y);

// FoodData.addExhaustion
int steve_transformed(void); // mumbo turned banjo into something: bk plays it
void steve_exhaust(f32 amount);
void steve_eat(int nutrition, f32 saturation_modifier); // FoodData.eat
// FirstPersonHandsAndItems.itemUsed / Player.resetAttackStrengthTicker
void steve_item_used(void);
void steve_reset_attack_strength(void);

// shared with survival.c
int steve_raycast(f32 start[3], f32 end[3], f32 out_hit[3], f32 out_normal[3]);
void steve_swing(void);
int steve_attack(f32 eye[3], f32 dir[3], f32 max_dist); // TRUE if an actor was hit
int steve_overlaps_cell(s32 c[3]);
// right click on an actor (bottles, mumbo...): a b press for bk. TRUE if one was there
int steve_interact(f32 eye[3], f32 dir[3], f32 max_dist);
void steve_press_b(void);
// an arrow flying from -> to (bk units) this tick: TRUE if it hit an enemy
int steve_arrow_hit(f32 from[3], f32 to[3], f32 damage);
f32 steve_current_attack_damage(void); // Player.attack's damage right now
void steve_hurt(f32 amount); // LivingEntity.hurt (armor, invulnerable time)
void steve_hurt_ex(f32 amount, int bypass_armor);
int steve_armor_value(void);       // Player.getArmorValue
f32 steve_armor_absorb(f32 damage); // getDamageAfterArmorAbsorb (wears the armor)
void steve_in_fire_block(void); // BaseFireBlock.entityInside: fireIgnite + inFire 1
f32 steve_height(void); // the box height (bk units): 1.8 blocks, 1.5 crouched
int steve_on_fire(void);        // Entity.isOnFire
// ServerExplosion.hurtEntities: steve and bk's enemies (center in bk units, radius in blocks)
void steve_explosion_hurt(const f32 center[3], f32 radius);
// right click with the egg item: one of bk's blue eggs, where steve looks
void steve_throw_egg(f32 eye[3], f32 dir[3]);
// oak log / leaves if this grid cell is part of a (climbable) bk tree
u16 steve_tree_block_at(s32 x, s32 y, s32 z);
// like steve_raycast, but only bk's level model counts (objects like tree
// trunks are skipped). *level_only = FALSE gives the plain nearest hit and
// tells if it was the level.
int steve_raycast_level(f32 start[3], f32 end[3], f32 out_hit[3], f32 out_normal[3]);
int steve_last_hit_was_level(void);
// mining trees (they're objects, not level): has this cell been mined
// already, and mine one. the tree vanishes when its last log is gone.
int steve_tree_is_mined(s32 x, s32 y, s32 z);
int steve_tree_pick(const f32 hit[3], const f32 n[3], s32 out[3], u16 *block); // a hit on a tree -> its log / leaf cell
void steve_tree_mine(s32 x, s32 y, s32 z);
// the saved world (worldsave.c): mined tree cells per map
int steve_tree_mined_count(s32 map);     // 0 unless the list is that map's
const s32 *steve_tree_mined_cell(int i);
void steve_tree_restore_begin(s32 map);  // the map's list starts over (then _cell for each)
void steve_tree_restore_cell(s32 x, s32 y, s32 z);

extern Steve gSteve;
extern int gSteveDrivingFrames;

// input captured from the controller before banjo sees it
typedef struct {
    f32 stick[2];
    f32 look[2]; // c buttons as an axis: x = right, y = up (-1..1)
    int jump;   // a held
    int sprint; // b held
    int sneak;  // z held
    int attack_clicks; // presses since the last tick (l / left click)
    int use_clicks;    // (r / right click)
    int attack_held;
    int use_held;
} SteveInput;

extern SteveInput gSteveInput;

extern int gShowDebug; // f3 debug screen (f3.c)
extern f32 gSmoothedTickMs; // ms per game tick, smoothed like the integrated server's
extern int gHideGui;   // f1

// eye height above the feet, bk units (minecraft: 1.62, 1.27 sneaking)
f32 steve_eye_height(void);
void steve_look_dir(f32 dst[3]);

void steve_draw(Gfx **gfx, Mtx **mtx);
void world_draw(Gfx **gfx, Mtx **mtx);

#endif
