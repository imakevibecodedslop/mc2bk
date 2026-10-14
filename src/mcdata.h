#ifndef __MC_DATA_H__
#define __MC_DATA_H__

#include <ultra64.h>

// the shapes of minecraft's block / item / recipe tables. the tables
// themselves are read out of the owner's minecraft jar by
// tools/export_mc_assets.py (+ tools/mc_data.py) into generated/mc_data.h,
// like swirly's exporter. ids: 0 = air / empty, 1..MC_BLOCK_COUNT-1 are
// blocks (a block's item has the same id), MC_ITEM_FIRST.. are plain items.

typedef u16 BlockId;
typedef u16 ItemId;

// McBlock.flags
#define MCB_OPAQUE           0x0001 // full opaque cube: hides neighbor faces
#define MCB_REQUIRES_TOOL    0x0002 // requiresCorrectToolForDrops
#define MCB_SWORD_EFFICIENT  0x0004 // #sword_efficient
#define MCB_MINE_PICKAXE     0x0008 // #mineable/pickaxe
#define MCB_MINE_AXE         0x0010
#define MCB_MINE_SHOVEL      0x0020
#define MCB_MINE_HOE         0x0040
#define MCB_UNBREAKABLE      0x0080 // strength -1 (bedrock)
#define MCB_MODEL            0x0100 // drawn from baked element quads (mc_models.py)
#define MCB_NO_COLLISION     0x0200 // Properties.noCollision (dust, torches, levers...)
#define MCB_FLUID            0x0400 // water / lava (LiquidBlock), state = legacy level
#define MCB_WASHED           0x0800 // #washed_away_by_fluids
#define MCB_CHEST            0x1000 // drawn by chest.c (ChestRenderer), has a container
#define MCB_TEX_SLOTS        0x2000 // a shaped family block: its quads' tex is a slot (mc_block_slots)

// tool tiers, ToolMaterial order (bits of McBlock.incorrect_for)
enum { MAT_WOOD, MAT_STONE, MAT_COPPER, MAT_IRON, MAT_GOLD, MAT_DIAMOND, MAT_NETHERITE };

typedef struct {
    u16 item;
    u8 min, max;
    u8 chance;   // 255 = always
} McDrop;

typedef struct {
    const char *name;  // en_us
    u16 tex[6];        // +y, -y, +z, -z, +x, -x (index into mc_block_tex)
    f32 hardness;      // destroyTime
    u16 flags;
    u8 incorrect_for;  // tiers whose tools can't harvest it (#incorrect_for_*_tool)
    McDrop drops[2];   // loot table without silk touch / shears
} McBlock;

typedef struct {
    const char *name;
    u8 max_stack;
    u16 durability;
    u8 tool;           // TOOL_* (items.h), 0 = none
    u8 material;       // MAT_*
    u8 handheld;       // item/handheld model (held like a tool)
    f32 speed;         // ToolMaterial.speed
    f32 attack_damage; // Attributes.ATTACK_DAMAGE with it held
    f32 attack_speed;  // Attributes.ATTACK_SPEED
} McItem;

// Equippable + ArmorMaterial: the item's armor slot (0 head, 1 chest, 2
// legs, 3 feet, 255 not armor), ARMOR / ARMOR_TOUGHNESS / KNOCKBACK_RESISTANCE
// and which equipment texture it wears (mc_armor_tex)
typedef struct {
    u8 slot;
    u8 defense;
    f32 toughness;
    f32 kb_resist;
    u8 asset;
} McArmor;

typedef struct {
    u16 offset, count; // into mc_ingredient_ids, count 0 = empty cell
} McIngredient;

typedef struct {
    u8 w, h;           // shaped size; w 0 = shapeless with h ingredients
    u16 result;
    u8 count;
    McIngredient cells[9];
} McRecipe;

// the recipe book's side of a recipe: CraftingBookCategory (0 misc,
// 1 building, 2 redstone, 3 equipment) and the items whose pickup unlocks it
// (its advancement/recipes/... inventory_changed criteria)
typedef struct {
    u8 category;
    McIngredient unlock;
} McRecipeBook;

// advancements (data/minecraft/advancement, tools/mc_advancements.py).
// flags: 1 show_toast, 2 hidden, 4 has a display. frame: 0 task, 1 goal,
// 2 challenge. x, y: TreeNodePosition (in 28 x 27 cells)
typedef struct {
    const char *title, *desc;
    u16 icon;
    u8 frame, flags;
    s16 parent;
    f32 x, y;
    u8 tab;
    u16 crit_first;
    u8 crit_count;
    u16 req_first;
    u8 req_count;
} McAdvancement;
typedef struct {
    u8 trigger;          // 1 inventory_changed, 2 consume_item, 3 player_killed_entity, 4 entity_killed_player, 0 can't happen here
    u16 pred_first;      // into mc_adv_preds: item sets, every one needed
    u8 pred_count;
} McAdvCrit;

// sounds (tools/mc_sounds.py): WeighedSoundEvents entries (Sound: file,
// weight, attenuation_distance, volume, pitch), events (first entry, count)
// and SoundType (volume, pitch, break / step / place / hit / fall events)
typedef struct {
    u16 file;
    u8 weight, att;
    f32 volume, pitch;
} McSoundEntry;
typedef struct {
    u16 first;
    u8 count;
} McSoundEvent;
typedef struct {
    f32 volume, pitch;
    s16 ev[5];
} McSoundType;

// furnace (AbstractCookingRecipe "smelting", data/minecraft/recipe)
typedef struct {
    McIngredient ing;
    u16 result;
    u8 count;
    u16 time;          // cookingtime, ticks
    f32 experience;    // AbstractCookingRecipe.experience
} McSmelting;

// cookingFuel burn time (Items.java + ContextIntProviders)
typedef struct {
    u16 item;
    u16 ticks;
} McFuel;

// FoodProperties (Foods.java) and how long eating takes (Consumables)
typedef struct {
    u16 item;
    u8 nutrition;
    f32 saturation_modifier;
    u8 always_edible;
    u8 eat_ticks;      // consumeSeconds * 20
} McFood;

// creative tabs (CreativeModeTab)
enum { TAB_CATEGORY, TAB_HOTBAR, TAB_SEARCH, TAB_INVENTORY };
#define TABF_ALIGNED_RIGHT 1
#define TABF_SHOW_TITLE 2
#define TABF_CAN_SCROLL 4
typedef struct {
    const char *title;
    u8 row;            // 0 top, 1 bottom
    u8 col;
    u8 type;           // TAB_*
    u8 flags;          // TABF_*
    u8 background;     // 0 tab_items, 1 tab_item_search, 2 tab_inventory
    u16 icon;
    u16 first, count;  // into mc_tab_items
} McTab;

#if __has_include("../generated/mc_data.h")
#include "../generated/mc_data.h"
#else
#error "run `make assets` first: the block/item tables come from your minecraft jar"
#endif

// a cell in the grid holds the block id plus its block state (facing / axis
// index, see mc_block_place / mc_state_tex) in bits 10-12
// model blocks (generated/mc_textures.h): baked quads. positions in 1/256
// block, uvs in 1/32 texel (gbi tc), cull = face index or 6 (none), shade =
// face index | 0x80 if tinted (redstone dust: tinted by power)
typedef struct {
    s16 v[4][3];
    s16 uv[4][2];
    u16 tex;
    u8 cull;
    u8 shade;
} McQuad;
typedef struct {
    u16 first, count;
    u16 box;           // shaped families: into mc_shape_boxes, outline boxes then collision boxes
    u8 nbox, ncoll;
} McModelState;
typedef struct {
    u8 lo[3], hi[3]; // the state's shape box, 1/16 block
} McBox;
typedef McBox McShapeBox; // (y up to 24: fence / gate collision)
// an item model's display.gui (ItemTransform rotation in degrees, scale)
typedef struct {
    s16 rot[3];
    f32 scale;
} McItemGui;

// shaped block families (tools/mc_families.py)
enum { FAM_NONE, FAM_SLAB, FAM_STAIRS, FAM_DOOR, FAM_TRAPDOOR, FAM_BUTTON, FAM_PLATE, FAM_FENCE, FAM_GATE,
       FAM_PANE, FAM_CARPET, FAM_PLANT, FAM_LADDER, FAM_LANTERN, FAM_CHAIN, FAM_SNOW, FAM_TORCH,
       FAM_WALL_TORCH, FAM_LEVER };
// a multipart case: OR of up to 5 terms, each a mask per property of the
// allowed values. wire: north, east, south, west (1 none, 2 side, 4 up);
// fire: north, east, south, west, up (1 false, 2 true). alt / alts: one of a
// random pick (by block position)
typedef struct {
    u16 first, count;
    u8 terms, alt, alts;
    u8 cond[5][5];
} McModelPart;
typedef struct {
    u16 first, count; // states (variants) or parts (multipart)
    u16 state_row;    // multipart: its single mc_model_states/boxes row
} McBlockModel;

#define CELL_ID(c) ((c) & 0x3FF)
// state: 6 bits (facing / axis, or a model block's state index)
#define CELL_STATE(c) (((c) >> 10) & 0x3F)
#define MAKE_CELL(id, st) ((u16)((id) | ((st) << 10)))
// mc_block_place kinds (getStateForPlacement)
enum { PLACE_NONE, PLACE_HFACING, PLACE_FACING, PLACE_AXIS, PLACE_FACING_TOWARD, PLACE_MODEL, PLACE_FLUID };

// names the hand-written code uses
#define BLOCK_GRASS BLOCK_GRASS_BLOCK
#define BLOCK_PLANKS BLOCK_OAK_PLANKS
#define BLOCK_LOG BLOCK_OAK_LOG
#define BLOCK_LEAVES BLOCK_OAK_LEAVES
#define BLOCK_COUNT MC_BLOCK_COUNT
#define ITEM_FIRST MC_ITEM_FIRST
#define ITEM_END MC_ITEM_END

static inline const McBlock *mc_block(u16 id) {
    return &mc_blocks[(id < MC_BLOCK_COUNT) ? id : 0];
}

#endif
