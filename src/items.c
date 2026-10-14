// items, tools and their rules on top of the generated tables (mcdata.h).

#include "modding.h"
#include "functions.h"
#include "variables.h"

#include "items.h"
#include "world.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

static const McItem *def(ItemId id) {
    if (id < MC_ITEM_FIRST || id >= MC_ITEM_END) {
        return NULL;
    }
    return &mc_items[id - MC_ITEM_FIRST];
}

// drawn as a block (cube / model): block items without a flat item sprite
int item_is_block(ItemId id) {
    return id != 0 && id < MC_BLOCK_COUNT && mc_block_sprite[id] == 0;
}

int item_max_stack(ItemId id) {
    const McItem *d = def(id);
    return d ? d->max_stack : 64;
}

int item_max_damage(ItemId id) {
    const McItem *d = def(id);
    return d ? d->durability : 0;
}

int item_tool_kind(ItemId id) {
    const McItem *d = def(id);
    return d ? d->tool : TOOL_NONE;
}

int item_is_handheld(ItemId id) {
    const McItem *d = def(id);
    return d ? d->handheld : FALSE;
}

const char *item_name(ItemId id) {
    const McItem *d = def(id);
    if (d) return d->name;
    return (id < MC_BLOCK_COUNT) ? mc_blocks[id].name : "?";
}

const u32 *item_sprite(ItemId id) {
    if (id != 0 && id < MC_BLOCK_COUNT && mc_block_sprite[id] != 0) {
        return &mc_block_item_sprites[(mc_block_sprite[id] - 1) * 256];
    }
    if (id >= MC_ITEM_FIRST && id < MC_ITEM_END) {
        return &mc_item_sprites[(id - MC_ITEM_FIRST) * 256];
    }
    return NULL;
}

// Tool.rules: #mineable/<kind> for the tool's kind
static int mineable_with(BlockId block, int kind) {
    u16 f = mc_block(block)->flags;
    switch (kind) {
        case TOOL_PICKAXE: return (f & MCB_MINE_PICKAXE) != 0;
        case TOOL_AXE: return (f & MCB_MINE_AXE) != 0;
        case TOOL_SHOVEL: return (f & MCB_MINE_SHOVEL) != 0;
        case TOOL_HOE: return (f & MCB_MINE_HOE) != 0;
    }
    return FALSE;
}

// is the tool's tier allowed (not in #incorrect_for_<material>_tool)
static int tier_ok(const McItem *d, BlockId block) {
    return (mc_block(block)->incorrect_for & (1 << d->material)) == 0;
}

f32 item_destroy_speed(ItemId held, BlockId block) {
    const McItem *d = def(held);

    if (d == NULL || d->tool == TOOL_NONE) {
        return 1.0f; // Tool.defaultMiningSpeed for anything without a tool component
    }
    if (d->tool == TOOL_SWORD) {
        // Tool rules of a sword: cobweb 15, #sword_efficient 1.5
        return (mc_block(block)->flags & MCB_SWORD_EFFICIENT) ? 1.5f : 1.0f;
    }
    // ToolMaterial.applyToolProperties: incorrect tiers mine at 1 (deniesDrops),
    // the mineable tag at the material's speed
    if (mineable_with(block, d->tool) && tier_ok(d, block)) {
        return d->speed;
    }
    return 1.0f;
}

int item_correct_for_drops(ItemId held, BlockId block) {
    const McItem *d = def(held);

    if (d == NULL || d->tool == TOOL_NONE || d->tool == TOOL_SWORD) {
        return FALSE;
    }
    return mineable_with(block, d->tool) && tier_ok(d, block);
}

int item_damage_per_block(ItemId id) {
    int k = item_tool_kind(id);
    return (k == TOOL_SWORD) ? 2 : (k != TOOL_NONE ? 1 : 0);
}

f32 item_attack_damage(ItemId id) {
    const McItem *d = def(id);
    return d ? d->attack_damage : 1.0f;
}

// Weapon.itemDamagePerAttack: swords and spears 1 (Weapon(1)), the other
// tools 2 (ToolMaterial.applyToolProperties), anything else none
int item_damage_per_attack(ItemId id) {
    int k = item_tool_kind(id);
    if (k == TOOL_NONE) {
        const McItem *d = def(id);
        // spears: a weapon with durability but no mining kind
        return (d != NULL && d->durability > 0 && d->attack_damage > 1.0f) ? 1 : 0;
    }
    return (k == TOOL_SWORD) ? 1 : 2;
}

f32 item_attack_delay(ItemId id) {
    const McItem *d = def(id);
    return 20.0f / (d ? d->attack_speed : 4.0f);
}
