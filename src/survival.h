#ifndef __MC_SURVIVAL_H__
#define __MC_SURVIVAL_H__

#include <ultra64.h>
#include "mcdata.h"

// minecraft's survival side of the player: inventory, game mode, block
// breaking (MultiPlayerGameMode), item drops (ItemEntity) and pickup.

#define INV_SIZE 36    // Inventory.INVENTORY_SIZE, slots 0-8 are the hotbar
// Inventory.EQUIPMENT_SLOT_MAPPING: 36 feet, 37 legs, 38 chest, 39 head, 40 offhand
#define INV_ARMOR(slot) (39 - (slot)) // McArmor.slot: 0 head .. 3 feet
#define INV_OFFHAND 40
#define INV_TOTAL 41
#define STACK_MAX 64

typedef struct {
    ItemId id;  // item id (see mcdata.h), 0 = empty
    u8 count;
    u8 pad;
    u16 damage; // tool wear
} ItemStack;

typedef struct {
    ItemStack inv[INV_TOTAL]; // + armor and the offhand
    int selected;      // hotbar slot 0-8
    int creative;      // gamemode == GM_CREATIVE (instabuild, infinite materials)
    int gamemode;      // GameType: GM_*
    int prev_gamemode; // MultiPlayerGameMode.previousPlayerMode (-1 none)

    // MultiPlayerGameMode
    int destroying;
    s32 destroy_pos[3];
    f32 destroy_progress;
    int destroy_delay;
    int miss_time;
    int right_click_delay;
    int pick_clicks;   // middle clicks waiting for the next tick

    // LivingEntity.useItem: eating
    int use_remaining; // getUseItemRemainingTicks, 0 = not using
    int use_duration;  // getUseDuration of what's being used
    int use_slot;
} Survival;

extern Survival gSurv;
int surv_mob_mask(u32 actor_id); // the mob a bk enemy counts as (1 << McMob: zombie, cow, cod, spider, chicken, skeleton, creeper, husk, drowned, snow golem, bat)
void surv_mob_sound(u32 actor_id, int death, const f32 pos[3]); // its hurt / death sound
void surv_mob_size(u32 actor_id, f32 *w, f32 *h); // its EntityType size (blocks)
f32 surv_mob_max_health(u32 actor_id); // its MAX_HEALTH (zombie 20, cow 10, cod 3...)
int surv_mob_listed(u32 actor_id); // mapped to a mob by name (limbo, boom boxes, sir slush...)
void surv_equip_sound(ItemId id); // Equippable.equipSound
u16 surv_block_at(s32 x, s32 y, s32 z); // placed block, else bk ground (not dug), else air

// GameType
enum { GM_SURVIVAL, GM_CREATIVE, GM_ADVENTURE, GM_SPECTATOR };
#define GM_SPECTATING() (gSurv.gamemode == GM_SPECTATOR)
// isBlockActionRestricted: adventure and spectator can't break or place
#define GM_BLOCKS_RESTRICTED() (gSurv.gamemode == GM_ADVENTURE || gSurv.gamemode == GM_SPECTATOR)
void surv_set_gamemode(int mode);
extern int surv_starter_kit_pending; // set when a world starts without an inventory file
void surv_starter_kit(void);
void surv_pop_item(s32 cell[3], ItemId id, int count); // Block.popResource
void surv_explode_block(s32 cell[3], BlockId b, f32 radius); // ServerExplosion: drops (ExplosionDecay) + remove

// the selected hotbar item (0 = empty hand), or only if it's a block
ItemId surv_held_item(void);
BlockId surv_held_block(void);
// Inventory.add: returns how many didn't fit
int surv_inv_add(ItemId id, int count);
// Inventory.add for a whole stack (keeps tool damage). returns what didn't fit
int surv_inv_add_stack(ItemStack *st);

// one 20 tick/s tick of the player's hands. eye/dir: where steve looks.
// attack_clicks/use_clicks: presses since last tick. *_held: buttons down now.
void surv_tick(f32 eye[3], f32 dir[3], int attack_clicks, int attack_held, int use_clicks, int use_held);
// the f3 screen's targets (DebugEntryLookingAt: picks at 20 blocks), redone
// each tick while f3 is open
typedef struct {
    int block_hit;   // cameraEntity.pick(20, 0, false)
    s32 cell[3];
    u16 cell_value;  // id + state (bk terrain: just the block it counts as)
    int fluid_hit;   // cameraEntity.pick(20, 0, true) landed in a fluid
    s32 fcell[3];
    u16 fcell_value;
} F3Target;
extern F3Target gF3Target;
// q: drop one of the selected item (or the whole stack with ctrl)
void surv_drop_selected(int whole_stack);
// death: everything in the inventory is dropped
void surv_drop_all(void);
// dropped items: physics + pickup, once per tick
void surv_items_tick(void);
// map changed: drop all item entities and the break in progress
void surv_on_map_load(void);

// drawing (3d pass, camera relative)
void surv_draw_world(Gfx **gfx, Mtx **mtx);

// a bk enemy died to steve: its minecraft stand-in's loot (LivingEntity.dropFromLootTable)
void surv_mob_loot(u32 actor_id, f32 pos[3]);

// BowItem.getPowerForTime (0..1)
f32 surv_bow_power(int time_held);

// food: the FoodProperties of this item, or NULL
const McFood *surv_food(ItemId id);

// the crack stage 0-9 of the block being broken, -1 if none
int surv_destroy_stage(void);

#endif
