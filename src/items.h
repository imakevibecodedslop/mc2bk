#ifndef __MC_ITEMS_H__
#define __MC_ITEMS_H__

#include <ultra64.h>
#include "mcdata.h"

// items and tools. the tables come from minecraft's own data (mcdata.h):
// Items.java (stacks, durability, tool stats), ToolMaterial.java (tiers), the
// mineable / incorrect_for tags and Blocks.java strengths.

enum tool_kind { TOOL_NONE, TOOL_SWORD, TOOL_SHOVEL, TOOL_PICKAXE, TOOL_AXE, TOOL_HOE };

int item_is_block(ItemId id);
int item_max_stack(ItemId id);    // Item.Properties.stacksTo
int item_max_damage(ItemId id);   // durability, 0 = none
int item_tool_kind(ItemId id);
int item_is_handheld(ItemId id);  // tools use the "handheld" model
const char *item_name(ItemId id); // en_us name, for tooltips
const u32 *item_sprite(ItemId id); // 16x16 rgba8888, NULL for blocks

// Player.getDestroySpeed / Tool rules: mining speed of `held` on `block`
f32 item_destroy_speed(ItemId held, BlockId block);
// Tool.isCorrectForDrops (blocks that need a tool to drop anything)
int item_correct_for_drops(ItemId held, BlockId block);
// Tool.damagePerBlock / Weapon.itemDamagePerAttack
int item_damage_per_block(ItemId id);
int item_damage_per_attack(ItemId id);
f32 item_attack_damage(ItemId id); // Attributes.ATTACK_DAMAGE with this item held
// ticks for a full attack strength (1 / attack speed * 20), hand = 5
f32 item_attack_delay(ItemId id);

#endif
