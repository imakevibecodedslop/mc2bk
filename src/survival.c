// minecraft's survival player, ported from:
//   - Inventory.add / getFreeSlot / getSlotWithRemainingSpace
//   - Minecraft.startAttack / continueAttack / startUseItem (missTime, rightClickDelay)
//   - MultiPlayerGameMode.startDestroyBlock / continueDestroyBlock (destroyDelay 5)
//   - Block.getDestroyProgress + Player.getDestroySpeed (hand only, /5 in the air)
//   - Block.popResource + ItemEntity (tick, gravity 0.04, bounce, pickup delay 10)
//   - ItemEntityRenderer (bob, spin) with the block model's "ground" transform
// block hardness / tool / drop values are from Blocks.java and the loot tables.

#include "options.h"
#include "modding.h"
#include "functions.h"
#include "variables.h"
#include "recomputils.h"

#include "bk_externs.h"
#include "steve.h"
#include "world.h"
#include "survival.h"
#include "pose.h"
#include "mc_gbi.h"
#include "items.h"
#include "screen.h"
#include "terrain.h"
#include "mouse.h"
#include "particles.h"
#include "furnace.h"
#include "arrows.h"
#include "xp.h"
#include "redstone.h"
#include "tnt.h"
#include "fluids.h"
#include "shield.h"
#include "recipebook.h"
#include "advancements.h"
#include "sound.h"
#include "shapes.h"
#include "chest.h"
#include "boat.h"

#if __has_include("../generated/mc_textures.h")
#include "../generated/mc_textures.h"
#endif

Survival gSurv;

// block properties (Blocks.java strength, loot tables, tool tags) are in
// mc_blocks (generated, see mcdata.h)

// can the held item harvest this block (Player.hasCorrectToolForDrops)
static int can_harvest(BlockId b) {
    return !(mc_block(b)->flags & MCB_REQUIRES_TOOL) || item_correct_for_drops(surv_held_item(), b);
}

// Block.getDestroyProgress + Player.getDestroySpeed
static f32 destroy_progress_per_tick(BlockId b) {
    f32 speed = item_destroy_speed(surv_held_item(), b);
    f32 h = mc_block(b)->hardness;

    if (mc_block(b)->flags & MCB_UNBREAKABLE) {
        return 0.0f; // bedrock: destroyTime -1
    }
    if (h <= 0.0f) {
        return 1.0f;
    }
    if (gSteve.eye_in_water) {
        speed /= 5.0f; // SUBMERGED_MINING_SPEED 0.2
    }
    if (!gSteve.on_ground) {
        speed /= 5.0f;
    }
    return speed / h / (can_harvest(b) ? 30.0f : 100.0f);
}

// ItemStack.hurtAndBreak on the held tool
static void damage_held(int amount) {
    ItemStack *st = &gSurv.inv[gSurv.selected];

    if (gSurv.creative || amount <= 0 || item_max_damage(st->id) == 0) {
        return;
    }
    st->damage += amount;
    if (st->damage >= item_max_damage(st->id)) {
        st->id = 0;
        st->count = 0;
        st->damage = 0; // the tool breaks
        // LivingEntity.breakItem
        sound_play_player(SND(ENTITY_ITEM_BREAK), 0.8f, 0.8f + sound_random() * 0.4f);
    }
}

// --- mob loot ---

static f32 rand01(void);
static void pop_resource(s32 cell[3], ItemId id, int count);

// which minecraft mob a bk enemy stands in for (bk has no loot, so each
// enemy borrows the mob it's most like). the order is mc_advancements.py's
// KILLABLE (the kill criteria's mask bits)
enum {
    MOB_ZOMBIE, MOB_COW, MOB_COD, MOB_SPIDER, MOB_CHICKEN, MOB_SKELETON,
    MOB_CREEPER, MOB_HUSK, MOB_DROWNED, MOB_SNOW_GOLEM, MOB_BAT,
};

#define MOB_DROPS 5 // the mc_loot_* rows (export_mc_assets.py MOB_DROPS)

typedef struct {
    const McDrop *loot;   // its loot table (entities/<mob>.json)
    s16 hurt, death;      // getHurtSound / getDeathSound
    f32 w, h;             // EntityType size
    u8 xp_min, xp_max;    // getBaseExperienceReward: Monster 5, Animal / fish 1 + random(3), golem / bat 0
    u8 health;            // Attributes.MAX_HEALTH from createAttributes (default 20)
} McMobInfo;

static const McMobInfo mobs[] = {
    { mc_loot_zombie,     SND(ENTITY_ZOMBIE_HURT),     SND(ENTITY_ZOMBIE_DEATH),     0.6f, 1.95f, 5, 5, 20 },
    { mc_loot_cow,        SND(ENTITY_COW_HURT),        SND(ENTITY_COW_DEATH),        0.9f, 1.4f,  1, 3, 10 },
    { mc_loot_cod,        SND(ENTITY_COD_HURT),        SND(ENTITY_COD_DEATH),        0.5f, 0.3f,  1, 3, 3 },
    { mc_loot_spider,     SND(ENTITY_SPIDER_HURT),     SND(ENTITY_SPIDER_DEATH),     1.4f, 0.9f,  5, 5, 16 },
    { mc_loot_chicken,    SND(ENTITY_CHICKEN_HURT),    SND(ENTITY_CHICKEN_DEATH),    0.4f, 0.7f,  1, 3, 4 },
    { mc_loot_skeleton,   SND(ENTITY_SKELETON_HURT),   SND(ENTITY_SKELETON_DEATH),   0.6f, 1.99f, 5, 5, 20 },
    { mc_loot_creeper,    SND(ENTITY_CREEPER_HURT),    SND(ENTITY_CREEPER_DEATH),    0.6f, 1.7f,  5, 5, 20 },
    { mc_loot_husk,       SND(ENTITY_HUSK_HURT),       SND(ENTITY_HUSK_DEATH),       0.6f, 1.95f, 5, 5, 20 },
    { mc_loot_drowned,    SND(ENTITY_DROWNED_HURT),    SND(ENTITY_DROWNED_DEATH),    0.6f, 1.95f, 5, 5, 20 },
    { mc_loot_snow_golem, SND(ENTITY_SNOW_GOLEM_HURT), SND(ENTITY_SNOW_GOLEM_DEATH), 0.7f, 1.9f,  0, 0, 4 },
    { mc_loot_bat,        SND(ENTITY_BAT_HURT),        SND(ENTITY_BAT_DEATH),        0.5f, 0.9f,  0, 0, 6 },
};

static int mob_kind(u32 actor_id) {
    switch (actor_id) {
        case 0x4:   // ACTOR_4_BIGBUTT
        case 0xE:   // ACTOR_E_BULL_INTRO
            return MOB_COW;
        case 0x67:  // ACTOR_67_SNIPPET
        case 0xF2:  // ACTOR_F2_BLACK_SNIPPET
        case 0xF5:  // ACTOR_F5_MUTIE_SNIPPET
        case 0xA:   // ACTOR_A_CHUMP_FISH
            return MOB_COD;
        case 0x5:   // ACTOR_5_TERMITE (mm's tickers: long legs, spider-ish)
        case 0x134: // ACTOR_134_BUZZBOMB (bgs's dragonflies)
        case 0x380: // ACTOR_380_SCARAB_BEETLE (gv)
            return MOB_SPIDER;    // string: bows, wool
        case 0x29F: // ACTOR_29F_CLUCKER
            return MOB_CHICKEN;   // feathers: arrows
        case 0x34E: // ACTOR_34E_LIMBO (gv's skeletons)
            return MOB_SKELETON;  // bones, arrows
        case 0x2A4: // ACTOR_2A4_BOOM_BOX_SLOW (rbb: they blow up)
        case 0x30D: // ACTOR_30D_BOOM_BOX_FAST
            return MOB_CREEPER;   // gunpowder: tnt
        case 0x34F: // ACTOR_34F_MUMMUM (gv)
            return MOB_HUSK;
        case 0x350: // ACTOR_350_SEAMAN_GRUBLIN (rbb)
            return MOB_DROWNED;
        case 0x124: // ACTOR_124_SIR_SLUSH (fp's snowmen)
            return MOB_SNOW_GOLEM;
        case 0x163: // ACTOR_163_BAT (mmm)
            return MOB_BAT;
    }
    return MOB_ZOMBIE; // grublins and the rest
}

// picked by name above (not just the zombie fallback)
int surv_mob_listed(u32 actor_id) {
    return mob_kind(actor_id) != MOB_ZOMBIE;
}

// the mob a bk enemy counts as, for advancements (bit = McMob)
int surv_mob_mask(u32 actor_id) {
    return 1 << mob_kind(actor_id);
}

// LivingEntity.hurtServer / handleDamageEvent: the mob's hurt or death
// sound, getSoundVolume 1, getVoicePitch (r - r) * 0.2 + 1
void surv_mob_sound(u32 actor_id, int death, const f32 pos[3]) {
    const McMobInfo *m = &mobs[mob_kind(actor_id)];
    sound_play_at(death ? m->death : m->hurt, pos, 1.0f, (sound_random() - sound_random()) * 0.2f + 1.0f);
}

// LivingEntity.getMaxHealth of the mob it stands in for
f32 surv_mob_max_health(u32 actor_id) {
    return (f32)mobs[mob_kind(actor_id)].health;
}

void surv_mob_size(u32 actor_id, f32 *w, f32 *h) {
    const McMobInfo *m = &mobs[mob_kind(actor_id)];
    *w = m->w;
    *h = m->h;
}

void surv_mob_loot(u32 actor_id, f32 pos[3]) {
    const McMobInfo *m = &mobs[mob_kind(actor_id)];
    const McDrop *t = m->loot;
    s32 cell[3];
    int i;

    cell[0] = world_to_block(pos[0]);
    cell[1] = world_to_block(pos[1] + MC_BLOCK * 0.5f);
    cell[2] = world_to_block(pos[2]);
    for (i = 0; i < MOB_DROPS; i++) {
        int n;
        if (t[i].item == 0) continue;
        if (t[i].chance < 255 && rand01() * 255.0f >= (f32)t[i].chance) continue;
        n = t[i].min + (int)(rand01() * (f32)(t[i].max - t[i].min + 1));
        if (n > t[i].max) n = t[i].max;
        if (n > 0) pop_resource(cell, t[i].item, n);
    }
    // LivingEntity.dropExperience
    {
        s32 xp = m->xp_min + (s32)(rand01() * (f32)(m->xp_max - m->xp_min + 1));
        if (xp > m->xp_max) xp = m->xp_max;
        if (xp > 0) xp_award(pos, xp);
    }
}

// --- bow (BowItem) ---

// Player.getProjectile: the first arrow in the inventory
static int find_arrow(void) {
    int i;
    for (i = 0; i < INV_SIZE; i++) {
        if (gSurv.inv[i].count > 0 && gSurv.inv[i].id == ITEM_ARROW) return i;
    }
    return -1;
}

// BowItem.getPowerForTime
f32 surv_bow_power(int time_held) {
    f32 pow = time_held / 20.0f;
    pow = (pow * pow + pow * 2.0f) / 3.0f;
    return pow > 1.0f ? 1.0f : pow;
}

// BowItem.releaseUsing -> shootFromRotation(xRot, yRot, 0, pow * 3, 1.0)
static void bow_release(f32 eye[3], f32 dir[3], int time_held) {
    f32 pow = surv_bow_power(time_held);
    int slot = find_arrow(), k;
    f32 vel[3], pos[3], len;

    if (pow < 0.1f || (slot < 0 && !gSurv.creative)) return;
    // getMovementToShoot: normalize, + triangle(0, 0.0172275 * uncertainty), * pow
    len = __builtin_sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    for (k = 0; k < 3; k++) {
        f32 tri = (rand01() - rand01()) * 0.0172275f;
        vel[k] = (dir[k] / len + tri) * pow * 3.0f;
    }
    // + the shooter's movement (y only in the air)
    vel[0] += gSteve.vel[0];
    vel[2] += gSteve.vel[2];
    if (!gSteve.on_ground) vel[1] += gSteve.vel[1];
    // from the eye, 0.1 lower (getEyeY() - 0.1)
    pos[0] = eye[0];
    pos[1] = eye[1] - 0.1f * MC_BLOCK;
    pos[2] = eye[2];
    arrows_spawn(pos, vel, pow == 1.0f, !gSurv.creative);
    // BowItem.releaseUsing
    sound_play_player(SND(ENTITY_ARROW_SHOOT), 1.0f, 1.0f / (sound_random() * 0.4f + 1.2f) + pow * 0.5f);
    if (!gSurv.creative) {
        ItemStack *a = &gSurv.inv[slot];
        if (--a->count == 0) {
            a->id = 0;
            a->damage = 0;
        }
    }
    damage_held(1); // the bow wears by 1 per shot
    steve_item_used();
}

// --- game modes ---

// MultiPlayerGameMode.setLocalMode + GameType.updatePlayerAbilities
void surv_set_gamemode(int mode) {
    if (mode == gSurv.gamemode) return;
    gSurv.prev_gamemode = gSurv.gamemode;
    gSurv.gamemode = mode;
    gSurv.creative = (mode == GM_CREATIVE);
    gSurv.destroying = FALSE;
    recomp_printf("[mc] game mode %d\n", mode);
}

// --- food ---

const McFood *surv_food(ItemId id) {
    int i;
    if (id == 0) return NULL;
    for (i = 0; i < MC_FOOD_COUNT; i++) {
        if (mc_foods[i].item == id) return &mc_foods[i];
    }
    return NULL;
}

// --- inventory ---

ItemId surv_held_item(void) {
    ItemStack *s = &gSurv.inv[gSurv.selected];
    return (s->count > 0) ? s->id : 0;
}

BlockId surv_held_block(void) {
    ItemId id = surv_held_item();
    // BlockItem.getBlock: block items, and items placing another block (redstone)
    return (id != 0) ? redstone_item_block(id) : BLOCK_AIR;
}

int surv_inv_add_stack(ItemStack *st) {
    int i;

    if (st->count == 0) return 0;
    if (item_max_stack(st->id) == 1) {
        // unstackable: the selected slot if empty, else the first free one
        if (gSurv.inv[gSurv.selected].count == 0) {
            gSurv.inv[gSurv.selected] = *st;
            return 0;
        }
        for (i = 0; i < INV_SIZE; i++) {
            if (gSurv.inv[i].count == 0) {
                gSurv.inv[i] = *st;
                return 0;
            }
        }
        return st->count;
    }
    return surv_inv_add(st->id, st->count);
}

int surv_inv_add(ItemId id, int count) {
    int i;

    // getSlotWithRemainingSpace: selected slot first, then in order
    if (gSurv.inv[gSurv.selected].id == id && gSurv.inv[gSurv.selected].count > 0) {
        int room = item_max_stack(id) - gSurv.inv[gSurv.selected].count;
        int n = (count < room) ? count : room;
        gSurv.inv[gSurv.selected].count += n;
        count -= n;
    }
    for (i = 0; i < INV_SIZE && count > 0; i++) {
        ItemStack *s = &gSurv.inv[i];
        if (s->count > 0 && s->id == id && s->count < item_max_stack(id)) {
            int room = item_max_stack(id) - s->count;
            int n = (count < room) ? count : room;
            s->count += n;
            count -= n;
        }
    }
    // getFreeSlot
    for (i = 0; i < INV_SIZE && count > 0; i++) {
        ItemStack *s = &gSurv.inv[i];
        if (s->count == 0) {
            int n = (count < item_max_stack(id)) ? count : item_max_stack(id);
            s->id = id;
            s->damage = 0;
            s->count = n;
            count -= n;
        }
    }
    return count;
}

// --- dropped items (ItemEntity) ---

#define MAX_ITEMS 32
#define ITEM_LIFETIME 6000 // 5 minutes

typedef struct {
    int active;
    ItemId id;
    u8 count;
    u16 damage;
    f32 pos[3], o_pos[3]; // bk units, bottom center
    f32 vel[3];           // blocks per tick
    int age;
    int pickup_delay;
    f32 bob_offs;
    int on_ground;
} ItemEnt;

static ItemEnt items[MAX_ITEMS];
static u32 rng_state = 0x1234567;

static f32 rand01(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (f32)(rng_state >> 8) / 16777216.0f;
}

// Block.popResource: block center +-0.25, minus half the item height, then
// the ItemEntity constructor's random push
static void pop_resource(s32 cell[3], ItemId id, int count) {
    int i, slot = -1;
    ItemEnt *it;

    for (i = 0; i < MAX_ITEMS; i++) {
        if (!items[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // full: replace the oldest
        int oldest = 0;
        for (i = 1; i < MAX_ITEMS; i++) {
            if (items[i].age > items[oldest].age) oldest = i;
        }
        slot = oldest;
    }
    it = &items[slot];
    it->active = TRUE;
    it->id = id;
    it->count = (u8)count;
    it->damage = 0;
    it->pos[0] = (cell[0] + 0.5f + (rand01() * 0.5f - 0.25f)) * MC_BLOCK;
    it->pos[1] = (cell[1] + 0.5f + (rand01() * 0.5f - 0.25f) - 0.125f) * MC_BLOCK;
    it->pos[2] = (cell[2] + 0.5f + (rand01() * 0.5f - 0.25f)) * MC_BLOCK;
    for (i = 0; i < 3; i++) it->o_pos[i] = it->pos[i];
    it->vel[0] = rand01() * 0.2f - 0.1f;
    it->vel[1] = 0.2f;
    it->vel[2] = rand01() * 0.2f - 0.1f;
    it->age = 0;
    it->pickup_delay = 10; // setDefaultPickUpDelay
    it->bob_offs = rand01() * 3.14159265f * 2.0f;
    it->on_ground = FALSE;
}

// Player.drop (q): LivingEntity.createItemStackToDrop, thrown from the hand
void surv_drop_selected(int whole_stack) {
    ItemStack *st = &gSurv.inv[gSurv.selected];
    f32 look[3], ang, pow2;
    int n, i, slot = -1;
    ItemEnt *it;

    if (st->count == 0 || st->id == ITEM_EGG) {
        return; // eggs are bk's egg counter, they stay with banjo
    }
    n = whole_stack ? st->count : 1;
    for (i = 0; i < MAX_ITEMS; i++) {
        if (!items[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        return;
    }
    it = &items[slot];
    it->active = TRUE;
    it->id = st->id;
    it->count = (u8)n;
    it->damage = st->damage;
    it->pos[0] = gSteve.pos[0];
    it->pos[1] = gSteve.pos[1] + steve_eye_height() - 0.3f * MC_BLOCK;
    it->pos[2] = gSteve.pos[2];
    for (i = 0; i < 3; i++) it->o_pos[i] = it->pos[i];
    steve_look_dir(look); // = (-sinY cosX, -sinX, cosY cosX) in minecraft terms
    ang = rand01() * 3.14159265f * 2.0f;
    pow2 = 0.02f * rand01();
    it->vel[0] = look[0] * 0.3f + ml_cos_deg(ang * 57.2957795f) * pow2;
    it->vel[1] = look[1] * 0.3f + 0.1f + (rand01() - rand01()) * 0.1f;
    it->vel[2] = look[2] * 0.3f + ml_sin_deg(ang * 57.2957795f) * pow2;
    it->age = 0;
    it->pickup_delay = 40;
    it->bob_offs = rand01() * 3.14159265f * 2.0f;
    it->on_ground = FALSE;

    if (!gSurv.creative) {
        st->count -= n;
        if (st->count == 0) { st->id = 0; st->damage = 0; }
    }
    steve_swing(); // LocalPlayer.drop swings the main hand
}

// ItemEntity.tick (no water, no merging)
static void item_tick(ItemEnt *it) {
    f32 hit[3], n[3], start[3], end[3];
    f32 hmove = 0.0f; // how far it went sideways this tick (bk units)
    int i;

    for (i = 0; i < 3; i++) it->o_pos[i] = it->pos[i];
    if (it->pickup_delay > 0) it->pickup_delay--;

    it->vel[1] -= 0.04f; // applyGravity

    // move: horizontal at the item's middle, then vertical (0.25 x 0.25 box)
    start[0] = it->pos[0];
    start[1] = it->pos[1] + 0.125f * MC_BLOCK;
    start[2] = it->pos[2];
    end[0] = start[0] + it->vel[0] * MC_BLOCK + (it->vel[0] > 0 ? 10.0f : -10.0f);
    end[1] = start[1];
    end[2] = start[2] + it->vel[2] * MC_BLOCK + (it->vel[2] > 0 ? 10.0f : -10.0f);
    if ((it->vel[0] != 0.0f || it->vel[2] != 0.0f) && steve_raycast(start, end, hit, n) && n[1] < 0.7f && n[1] > -0.7f) {
        it->vel[0] = it->vel[2] = 0.0f;
    } else {
        it->pos[0] += it->vel[0] * MC_BLOCK;
        it->pos[2] += it->vel[2] * MC_BLOCK;
        hmove = __builtin_sqrtf(it->vel[0] * it->vel[0] + it->vel[2] * it->vel[2]) * MC_BLOCK;
    }

    start[0] = end[0] = it->pos[0];
    start[2] = end[2] = it->pos[2];
    // bk's walkable slopes don't stop the sideways move (minecraft has none:
    // its box would sit on a step), and one rising up to hmove may now be
    // over the item: lift it back out instead of letting it fall through
    if (hmove > 0.0f) {
        start[1] = it->pos[1] + 0.125f * MC_BLOCK + hmove + 1.0f;
        end[1] = it->pos[1];
        if (steve_raycast(start, end, hit, n) && n[1] > 0.5f && hit[1] > it->pos[1]) {
            it->pos[1] = hit[1];
        }
    }
    if (it->vel[1] <= 0.0f) {
        start[1] = it->pos[1] + 0.125f * MC_BLOCK;
        end[1] = it->pos[1] + it->vel[1] * MC_BLOCK - 0.5f;
        if (steve_raycast(start, end, hit, n) && n[1] > 0.5f) {
            it->pos[1] = hit[1];
            it->on_ground = TRUE;
        } else {
            it->pos[1] += it->vel[1] * MC_BLOCK;
            it->on_ground = FALSE;
        }
    } else {
        start[1] = it->pos[1] + 0.25f * MC_BLOCK;
        end[1] = start[1] + it->vel[1] * MC_BLOCK;
        if (steve_raycast(start, end, hit, n)) {
            it->vel[1] = 0.0f;
        } else {
            it->pos[1] += it->vel[1] * MC_BLOCK;
        }
        it->on_ground = FALSE;
    }

    // air drag 0.98, ground friction 0.6 * 0.98, bounce at -0.5
    {
        f32 g = it->on_ground ? 0.98f * 0.6f : 0.98f;
        it->vel[0] *= g;
        it->vel[1] *= 0.98f;
        it->vel[2] *= g;
        if (it->on_ground && it->vel[1] < 0.0f) {
            it->vel[1] *= -0.5f;
        }
    }

    it->age++;
    if (it->age >= ITEM_LIFETIME || it->pos[1] < -100000.0f) {
        it->active = FALSE;
    }
}

// Player.touch: the player's box inflated by (1, 0.5, 1) touching the item
static void item_try_pickup(ItemEnt *it) {
    f32 *p = gSteve.pos;
    f32 r = (STEVE_HALF_WIDTH + 1.0f) * MC_BLOCK;
    f32 ir = 0.125f * MC_BLOCK;
    int left;

    if (it->pickup_delay > 0) {
        return;
    }
    if (it->pos[0] + ir < p[0] - r || it->pos[0] - ir > p[0] + r) return;
    if (it->pos[2] + ir < p[2] - r || it->pos[2] - ir > p[2] + r) return;
    if (it->pos[1] + 0.25f * MC_BLOCK < p[1] - 0.5f * MC_BLOCK) return;
    if (it->pos[1] > p[1] + (STEVE_HEIGHT + 0.5f) * MC_BLOCK) return;

    {
        ItemStack st;
        st.id = it->id;
        st.count = it->count;
        st.damage = it->damage;
        left = surv_inv_add_stack(&st);
    }
    if (left < it->count) {
        // handleTakeItemEntity: the pop
        sound_play_player(SND(ENTITY_ITEM_PICKUP), 0.2f, (sound_random() - sound_random()) * 1.4f + 2.0f);
    }
    if (left == 0) {
        it->active = FALSE;
    } else {
        it->count = (u8)left;
    }
}

void surv_items_tick(void) {
    arrows_tick();
    xp_tick();
    surv_starter_kit();
    redstone_tick();
    tnt_tick();
    fluids_tick();
    chests_tick();
    boats_tick();
    int i;

    particles_tick();
    for (i = 0; i < MAX_ITEMS; i++) {
        if (items[i].active) {
            item_tick(&items[i]);
            if (items[i].active) {
                item_try_pickup(&items[i]);
            }
        }
    }
}

void surv_on_map_load(void) {
    steve_mobs_clear();
    arrows_clear();
    xp_clear();
    redstone_clear();
    tnt_clear();
    fluids_clear();
    boats_clear();
    furnaces_clear();
    int i;

    particles_clear();
    for (i = 0; i < MAX_ITEMS; i++) {
        items[i].active = FALSE;
    }
    gSurv.destroying = FALSE;
    gSurv.destroy_progress = 0.0f;
}

// --- breaking and placing ---

typedef struct {
    int kind; // 0 miss, 1 grid block, 2 bk terrain (level or a tree)
    int tree; // kind 2: a tree object, mined without carving the level
    BlockId block; // what's being targeted (bk terrain counts as grass/dirt/stone/log/leaves)
    s32 cell[3];
    s32 normal[3];
    f32 hit[3], n[3];
    f32 dist;
} Pick;

// Minecraft.pick for blocks: the nearer of the grid and bk's level
static void pick_block(f32 eye[3], f32 dir[3], f32 reach, Pick *out) {
    f32 end[3], hit[3], n[3];
    f32 terrain_dist = 1e9f;
    int terrain;
    VoxelHit vh;
    int i;

    out->kind = 0;
    out->tree = FALSE;
    for (i = 0; i < 3; i++) end[i] = eye[i] + dir[i] * reach;
    terrain = steve_raycast(eye, end, hit, n);
    if (terrain) {
        f32 dx = hit[0] - eye[0], dy = hit[1] - eye[1], dz = hit[2] - eye[2];
        terrain_dist = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    }
    int grid_hit;
    world_pick_mode = TRUE;
    grid_hit = world_raycast(eye, end, &vh);
    world_pick_mode = FALSE;
    if (grid_hit && vh.dist <= terrain_dist + 1.0f) {
        out->kind = 1;
        out->dist = vh.dist;
        for (i = 0; i < 3; i++) {
            out->cell[i] = vh.cell[i];
            out->normal[i] = vh.normal[i];
        }
        out->block = world_get(vh.cell[0], vh.cell[1], vh.cell[2]);
        return;
    }
    if (terrain) {
        int level = steve_last_hit_was_level();

        // the leftovers of carved level triangles are our own collision
        // pieces (not bk's level any more), but they're still level ground
        if (!level) {
            f32 a[3], b[3], pn[3];
            for (i = 0; i < 3; i++) {
                a[i] = eye[i];
                b[i] = end[i];
            }
            if (terrain_line_query(a, b, pn, 0) != NULL) {
                f32 ex = b[0] - hit[0], ey = b[1] - hit[1], ez = b[2] - hit[2];
                if (ex * ex + ey * ey + ez * ez < 4.0f * 4.0f) level = TRUE;
            }
        }

        out->kind = 2;
        out->tree = FALSE;
        out->dist = terrain_dist;
        for (i = 0; i < 3; i++) {
            out->hit[i] = hit[i];
            out->n[i] = n[i];
            // the grid cell just behind the surface we hit is the "block"
            out->cell[i] = world_to_block(hit[i] - n[i] * 2.0f);
            out->normal[i] = (n[i] > 0.7f) ? 1 : (n[i] < -0.7f ? -1 : 0);
        }
        if (level) {
            // bk's walls are paper thin (void behind them): only ground mines
            if (n[1] < 0.3f && !terrain_cell_in_ground(out->cell[0], out->cell[1], out->cell[2])) {
                out->kind = 0;
                return;
            }
            out->block = terrain_block_at(out->cell[0], out->cell[1], out->cell[2]);
            return;
        }
        // an object: only trees can be mined. snap to the tree's log column
        // (or its leaves) near the hit, so the trunk's exact shape and the
        // logs already taken don't matter
        {
            s32 c[3];
            u16 tb;
            if (steve_tree_pick(hit, n, c, &tb)) {
                out->tree = TRUE;
                out->block = tb;
                for (i = 0; i < 3; i++) out->cell[i] = c[i];
                return;
            }
        }
        // (else the old way: walk a block deeper along the look direction
        // until we're in a log or leaves cell)
        for (i = 0; i <= 4; i++) {
            f32 p[3];
            s32 c[3];
            BlockId b;
            int k;

            for (k = 0; k < 3; k++) {
                p[k] = hit[k] + dir[k] * (MC_BLOCK * 0.25f * i) - n[k] * 2.0f;
                c[k] = world_to_block(p[k]);
            }
            b = steve_tree_block_at(c[0], c[1], c[2]);
            if (b != BLOCK_AIR && !steve_tree_is_mined(c[0], c[1], c[2])) {
                out->tree = TRUE;
                out->block = b;
                for (k = 0; k < 3; k++) out->cell[k] = c[k];
                return;
            }
        }
        out->kind = 0; // fences, signs, houses...: not a block
    }
}

// the targeted block for the mining functions below: placed blocks live in
// the grid, everything else is bk terrain that gets carved
static int tgt_terrain;
static int tgt_tree;
static BlockId tgt_block;

// the block in a cell: a placed one, else bk's ground there (not dug)
u16 surv_block_at(s32 x, s32 y, s32 z) {
    BlockId b = world_get(x, y, z);
    if (b != BLOCK_AIR) return b;
    if (!terrain_is_carved(x, y, z) && terrain_cell_in_ground(x, y, z)) return terrain_block_at(x, y, z);
    return BLOCK_AIR;
}

static BlockId target_block(s32 cell[3]) {
    BlockId b = world_get(cell[0], cell[1], cell[2]);
    if (b != BLOCK_AIR) return b;
    if (tgt_terrain && tgt_tree) return steve_tree_is_mined(cell[0], cell[1], cell[2]) ? BLOCK_AIR : tgt_block;
    if (tgt_terrain && !terrain_is_carved(cell[0], cell[1], cell[2])) return tgt_block;
    return BLOCK_AIR;
}

// the block's loot table (no silk touch / shears): each entry with its chance
// (fortune 0) and a uniform count (Block.dropResources -> popResource)
static void drop_loot(s32 cell[3], BlockId b) {
    const McBlock *mb = mc_block(b);
    int i;

    for (i = 0; i < 2; i++) {
        const McDrop *d = &mb->drops[i];
        int n;

        if (d->item == 0) continue;
        if (d->chance < 255 && rand01() * 255.0f >= (f32)d->chance) continue;
        n = d->min + (int)(rand01() * (f32)(d->max - d->min + 1));
        if (n > d->max) n = d->max;
        if (n > 0) pop_resource(cell, d->item, n);
    }
}

// ServerExplosion.interactWithBlocks for one block: its loot with
// ExplosionDecay (each item survives with 1 / radius), then gone. bk's
// ground is carved like mining it.
void surv_explode_block(s32 cell[3], BlockId b, f32 radius) {
    const McBlock *mb = mc_block(b);
    int i;

    if (b == BLOCK_AIR || b == BLOCK_FIRE) return;
    if (b == BLOCK_CHEST) {
        Chest *c = chest_at(cell, FALSE);
        if (c != NULL) {
            int k;
            for (k = 0; k < CHEST_SLOTS; k++) {
                if (c->items[k].count > 0) pop_resource(cell, c->items[k].id, c->items[k].count);
            }
            chest_removed(cell);
        }
    }
    if (b == BLOCK_FURNACE) {
        Furnace *f = furnace_at(cell, FALSE);
        if (f != NULL) {
            int k;
            for (k = 0; k < 3; k++) {
                if (f->items[k].count > 0) pop_resource(cell, f->items[k].id, f->items[k].count);
            }
            furnace_removed(cell);
        }
    }
    for (i = 0; i < 2; i++) {
        const McDrop *d = &mb->drops[i];
        int n, kept = 0, k;
        if (d->item == 0) continue;
        if (d->chance < 255 && rand01() * 255.0f >= (f32)d->chance) continue;
        n = d->min + (int)(rand01() * (f32)(d->max - d->min + 1));
        if (n > d->max) n = d->max;
        for (k = 0; k < n; k++) {
            if (rand01() <= 1.0f / radius) kept++;
        }
        if (kept > 0) pop_resource(cell, d->item, kept);
    }
    if (world_get(cell[0], cell[1], cell[2]) != BLOCK_AIR) {
        world_set(cell[0], cell[1], cell[2], BLOCK_AIR);
    } else {
        terrain_carve(cell[0], cell[1], cell[2]);
    }
}

static void destroy_block(s32 cell[3]) {
    BlockId b = target_block(cell);
    u16 cell_value = world_get_cell(cell[0], cell[1], cell[2]);

    if (b == BLOCK_AIR) {
        return;
    }
    particles_destroy_block(cell, b); // ClientLevel.addDestroyBlockEffect
    sound_block(b, SOUND_BREAK, cell[0], cell[1], cell[2]); // levelEvent 2001's break sound
    if (b == BLOCK_CHEST) {
        // Containers.dropContents
        Chest *c = chest_at(cell, FALSE);
        if (c != NULL) {
            int k;
            for (k = 0; k < CHEST_SLOTS; k++) {
                if (c->items[k].count > 0) pop_resource(cell, c->items[k].id, c->items[k].count);
            }
            chest_removed(cell);
        }
    }
    if (b == BLOCK_FURNACE) {
        // Containers.dropContents (BaseContainerBlockEntity.preRemoveSideEffects)
        Furnace *f = furnace_at(cell, FALSE);
        if (f != NULL) {
            int k;
            for (k = 0; k < 3; k++) {
                if (f->items[k].count > 0) pop_resource(cell, f->items[k].id, f->items[k].count);
            }
            furnace_award_xp(f, cell); // getRecipesToAwardAndPopExperience
            furnace_removed(cell);
        }
    }
    if (world_get(cell[0], cell[1], cell[2]) != BLOCK_AIR) {
        world_set(cell[0], cell[1], cell[2], BLOCK_AIR);
    } else if (tgt_tree) {
        steve_tree_mine(cell[0], cell[1], cell[2]); // trees aren't level: no carving
    } else {
        terrain_carve(cell[0], cell[1], cell[2]); // bk terrain: cut it out of the level
    }
    steve_exhaust(0.005f); // Block.playerDestroy: FoodConstants.EXHAUSTION_MINE
    // Block.playerDestroy -> dropResources, survival only, and only if the
    // held item can harvest it (stone/cobblestone/bricks need a pickaxe)
    if (!gSurv.creative && can_harvest(b)) {
        // (a double slab gives two, snow its layers)
        int k, nd = shapes_drop_count(cell_value);
        // (slab loot: set_count 2 only for type=double, the table here is the double's)
        if (shapes_family(b) == FAM_SLAB && CELL_STATE(cell_value) != 2) pop_resource(cell, b, 1);
        else for (k = 0; k < nd; k++) drop_loot(cell, b);
        // spawnAfterBreak -> tryDropExperience(xpRange.sample) -> popExperience
        if (b < MC_BLOCK_COUNT && mc_block_xp[b][1] > 0) {
            s32 lo = mc_block_xp[b][0], hi = mc_block_xp[b][1];
            s32 n = lo + (s32)(rand01() * (f32)(hi - lo + 1));
            f32 c[3] = { (cell[0] + 0.5f) * MC_BLOCK, (cell[1] + 0.5f) * MC_BLOCK, (cell[2] + 0.5f) * MC_BLOCK };
            if (n > hi) n = hi;
            if (n > 0) xp_award(c, n);
        }
    }
    // Item.mineBlock: tools wear by Tool.damagePerBlock
    damage_held(item_damage_per_block(surv_held_item()));
}

static f32 destroy_ticks; // MultiPlayerGameMode.destroyTicks

static int same_target(s32 cell[3]) {
    return gSurv.destroying && cell[0] == gSurv.destroy_pos[0] && cell[1] == gSurv.destroy_pos[1]
        && cell[2] == gSurv.destroy_pos[2];
}

// MultiPlayerGameMode.startDestroyBlock
static void start_destroy(s32 cell[3]) {
    BlockId b = target_block(cell);
    int i;

    if (GM_BLOCKS_RESTRICTED()) {
        return; // Player.blockActionRestricted (adventure)
    }

    if (gSurv.creative) {
        destroy_block(cell);
        gSurv.destroy_delay = 5;
        return;
    }
    if (same_target(cell)) {
        return;
    }
    gSurv.destroy_progress = 0.0f;
    destroy_ticks = 0.0f;
    if (b != BLOCK_AIR && destroy_progress_per_tick(b) >= 1.0f) {
        destroy_block(cell);
        gSurv.destroying = FALSE;
        return;
    }
    gSurv.destroying = TRUE;
    for (i = 0; i < 3; i++) gSurv.destroy_pos[i] = cell[i];
}

// MultiPlayerGameMode.continueDestroyBlock. returns TRUE while breaking (swing)
static int continue_destroy(s32 cell[3]) {
    BlockId b;

    if (gSurv.destroy_delay > 0) {
        gSurv.destroy_delay--;
        return TRUE;
    }
    if (gSurv.creative) {
        gSurv.destroy_delay = 5;
        destroy_block(cell);
        return TRUE;
    }
    if (!same_target(cell)) {
        start_destroy(cell);
        return TRUE;
    }
    b = target_block(cell);
    if (b == BLOCK_AIR) {
        gSurv.destroying = FALSE;
        return FALSE;
    }
    gSurv.destroy_progress += destroy_progress_per_tick(b);
    // the hit sound every 4 ticks of mining
    if ((s32)destroy_ticks % 4 == 0) sound_block(b, SOUND_HIT, cell[0], cell[1], cell[2]);
    destroy_ticks += 1.0f;
    if (gSurv.destroy_progress >= 1.0f) {
        gSurv.destroying = FALSE;
        destroy_block(cell);
        gSurv.destroy_progress = 0.0f;
        gSurv.destroy_delay = 5;
    }
    return TRUE;
}

static void stop_destroy(void) {
    gSurv.destroying = FALSE;
    gSurv.destroy_progress = 0.0f;
}

int surv_destroy_stage(void) {
    if (!gSurv.destroying || gSurv.destroy_progress <= 0.0f) {
        return -1;
    }
    {
        int s = (int)(gSurv.destroy_progress * 10.0f);
        return (s > 9) ? 9 : s;
    }
}

// Minecraft.startUseItem -> BlockItem.place on the face we look at
// Block.getStateForPlacement for blocks with a facing / axis (state index =
// the mod's face order: up, down, south, north, east, west; axis: y, z, x)
static int place_state(BlockId b, Pick *pk) {
    f32 d[3], n[3];
    int i, axis = 0;

    steve_look_dir(d);
    switch (mc_block_place[b]) {
        case PLACE_HFACING: {
            // getHorizontalDirection().getOpposite()
            int dir = (d[0] * d[0] > d[2] * d[2]) ? (d[0] > 0.0f ? 4 : 5) : (d[2] > 0.0f ? 2 : 3);
            return dir ^ 1;
        }
        case PLACE_FACING:
        case PLACE_FACING_TOWARD: {
            // getNearestLookingDirection() (.getOpposite() for most blocks)
            f32 ax = d[0] < 0 ? -d[0] : d[0], ay = d[1] < 0 ? -d[1] : d[1], az = d[2] < 0 ? -d[2] : d[2];
            int dir;
            if (ay >= ax && ay >= az) dir = d[1] > 0.0f ? 0 : 1;
            else if (ax >= az) dir = d[0] > 0.0f ? 4 : 5;
            else dir = d[2] > 0.0f ? 2 : 3;
            return (mc_block_place[b] == PLACE_FACING) ? (dir ^ 1) : dir;
        }
        case PLACE_AXIS:
            // RotatedPillarBlock: the clicked face's axis
            for (i = 0; i < 3; i++) n[i] = (pk->kind == 1) ? (f32)pk->normal[i] : pk->n[i];
            if (n[0] * n[0] >= n[1] * n[1] && n[0] * n[0] >= n[2] * n[2]) axis = 2;
            else if (n[2] * n[2] >= n[1] * n[1]) axis = 1;
            return axis;
    }
    return 0;
}

// Item.use for armor: Equippable.swapWithEquipmentSlot. the held piece and
// what's worn trade places; into an empty slot the hand empties (creative
// keeps a copy in hand). armor doesn't stack, so it's always one piece
// Equippable.equipSound (the armor material's), 1, 1
void surv_equip_sound(ItemId id) {
    static const s16 snd[8] = { SND(ITEM_ARMOR_EQUIP_LEATHER), SND(ITEM_ARMOR_EQUIP_COPPER), SND(ITEM_ARMOR_EQUIP_CHAIN),
        SND(ITEM_ARMOR_EQUIP_IRON), SND(ITEM_ARMOR_EQUIP_GOLD), SND(ITEM_ARMOR_EQUIP_DIAMOND), SND(ITEM_ARMOR_EQUIP_TURTLE),
        SND(ITEM_ARMOR_EQUIP_NETHERITE) };
    int a;
    if (id < ITEM_FIRST || id >= ITEM_END) return;
    if (mc_item_armor[id - ITEM_FIRST].slot > 3) {
        if (id == ITEM_SHIELD) sound_play_player(SND(ITEM_ARMOR_EQUIP_GENERIC), 1.0f, 1.0f);
        return;
    }
    a = mc_item_armor[id - ITEM_FIRST].asset;
    sound_play_player(a < 8 ? snd[a] : SND(ITEM_ARMOR_EQUIP_GENERIC), 1.0f, 1.0f);
}

static int equip_held(void) {
    ItemStack *hand = &gSurv.inv[gSurv.selected], *worn, h, w;
    int slot;

    if (hand->count == 0 || hand->id < ITEM_FIRST || hand->id >= ITEM_END) return FALSE;
    slot = mc_item_armor[hand->id - ITEM_FIRST].slot;
    if (slot > 3) return FALSE;
    worn = &gSurv.inv[INV_ARMOR(slot)];
    h = *hand;
    w = *worn;
    if (w.count > 0 && w.id == h.id && w.damage == h.damage) return FALSE; // isSameItemSameComponents: FAIL
    *worn = h;
    worn->count = 1;
    if (w.count == 0) {
        if (!gSurv.creative) {
            hand->id = 0;
            hand->count = 0;
            hand->damage = 0;
        }
    } else {
        *hand = w;
    }
    return TRUE;
}

// the blocks bk's ground is made of here: filling a hole with one of these
// mends the ground instead of leaving a block in it
static int block_is_ground_kind(BlockId b) {
    switch (b) {
        case BLOCK_GRASS: case BLOCK_DIRT: case BLOCK_COARSE_DIRT: case BLOCK_PODZOL: case BLOCK_MUD:
        case BLOCK_STONE: case BLOCK_COBBLESTONE: case BLOCK_DEEPSLATE: case BLOCK_GRANITE: case BLOCK_DIORITE:
        case BLOCK_ANDESITE: case BLOCK_TUFF: case BLOCK_SAND: case BLOCK_RED_SAND: case BLOCK_SANDSTONE:
        case BLOCK_GRAVEL: case BLOCK_SNOW_BLOCK: case BLOCK_CLAY: case BLOCK_TERRACOTTA: case BLOCK_PACKED_ICE:
            return TRUE;
    }
    return FALSE;
}

static int use_item(Pick *pk) {
    s32 target[3];
    BlockId held = surv_held_block();

    // BlockBehaviour.useWithoutItem: right clicking a crafting table opens it
    // (sneaking with something in hand skips the block, like minecraft)
    if (pk->kind == 1 && world_get(pk->cell[0], pk->cell[1], pk->cell[2]) == BLOCK_CRAFTING_TABLE
        && !(gSteve.sneaking && surv_held_item() != 0)) {
        screen_open_crafting();
        return TRUE;
    }
    // ChestBlock.useWithoutItem: open its menu (not with a solid block on top)
    if (pk->kind == 1 && world_get(pk->cell[0], pk->cell[1], pk->cell[2]) == BLOCK_CHEST
        && !(gSteve.sneaking && surv_held_item() != 0)) {
        if (!chest_blocked(pk->cell) && !GM_SPECTATING()) screen_open_chest(pk->cell);
        return TRUE;
    }
    // AbstractFurnaceBlock.useWithoutItem: open its menu
    if (pk->kind == 1 && world_get(pk->cell[0], pk->cell[1], pk->cell[2]) == BLOCK_FURNACE
        && !(gSteve.sneaking && surv_held_item() != 0)) {
        screen_open_furnace(pk->cell);
        return TRUE;
    }
    // levers, buttons, wooden trapdoors, redstone dust (useWithoutItem)
    if (pk->kind == 1 && !(gSteve.sneaking && surv_held_item() != 0) && !GM_SPECTATING()
        && redstone_use(pk->cell)) {
        return TRUE;
    }
    // doors and fence gates (DoorBlock / FenceGateBlock.useWithoutItem)
    if (pk->kind == 1 && !(gSteve.sneaking && surv_held_item() != 0) && !GM_SPECTATING()) {
        f32 look[3];
        steve_look_dir(look);
        if (shapes_use(pk->cell, look)) return TRUE;
    }
    if (surv_held_item() == ITEM_FLINT_AND_STEEL && pk->kind != 0 && !GM_BLOCKS_RESTRICTED()) {
        if (pk->kind == 1 && world_get(pk->cell[0], pk->cell[1], pk->cell[2]) == BLOCK_TNT) {
            // TntBlock.useItemOn: prime, remove, the lighter wears by 1
            world_set(pk->cell[0], pk->cell[1], pk->cell[2], BLOCK_AIR);
            tnt_prime(pk->cell, 80);
            damage_held(1);
            return TRUE;
        }
        // FlintAndSteelItem.useOn: fire on the clicked face
        if (pk->kind == 1) {
            target[0] = pk->cell[0] + pk->normal[0];
            target[1] = pk->cell[1] + pk->normal[1];
            target[2] = pk->cell[2] + pk->normal[2];
        } else {
            target[0] = world_to_block(pk->hit[0] + pk->n[0] * (MC_BLOCK * 0.5f));
            target[1] = world_to_block(pk->hit[1] + pk->n[1] * (MC_BLOCK * 0.5f));
            target[2] = world_to_block(pk->hit[2] + pk->n[2] * (MC_BLOCK * 0.5f));
        }
        if (fire_place(target)) {
            damage_held(1);
            return TRUE;
        }
        return FALSE;
    }
    if (held == BLOCK_AIR || pk->kind == 0 || GM_BLOCKS_RESTRICTED()) {
        return FALSE; // (adventure: Player.mayBuild is false)
    }
    // SlabBlock / SnowLayerBlock.canBeReplaced on the clicked block: a slab
    // doubled, a snow layer more
    if (pk->kind == 1) {
        u16 nc;
        if (shapes_merge(held, pk->cell, TRUE, pk->normal, pk->hit[1] / MC_BLOCK - (f32)pk->cell[1], &nc)) {
            world_set(pk->cell[0], pk->cell[1], pk->cell[2], nc);
            sound_block(held, SOUND_PLACE, pk->cell[0], pk->cell[1], pk->cell[2]);
            goto placed;
        }
    }
    if (pk->kind == 1) {
        target[0] = pk->cell[0] + pk->normal[0];
        target[1] = pk->cell[1] + pk->normal[1];
        target[2] = pk->cell[2] + pk->normal[2];
    } else {
        // bk terrain: the cell half a block out along the surface normal
        target[0] = world_to_block(pk->hit[0] + pk->n[0] * (MC_BLOCK * 0.5f));
        target[1] = world_to_block(pk->hit[1] + pk->n[1] * (MC_BLOCK * 0.5f));
        target[2] = world_to_block(pk->hit[2] + pk->n[2] * (MC_BLOCK * 0.5f));
    }
    if (world_get(target[0], target[1], target[2]) == held) {
        // ... or on the one in the cell next to it
        u16 nc;
        s32 nrm0[3];
        int k;
        for (k = 0; k < 3; k++) nrm0[k] = (pk->kind == 1) ? pk->normal[k] : 0;
        if (pk->kind != 1) nrm0[1] = 1;
        if (shapes_merge(held, target, FALSE, nrm0, pk->hit[1] / MC_BLOCK - (f32)target[1], &nc)) {
            world_set(target[0], target[1], target[2], nc);
            sound_block(held, SOUND_PLACE, target[0], target[1], target[2]);
            goto placed;
        }
    }
    if (world_get(target[0], target[1], target[2]) != BLOCK_AIR
        && !(mc_block(world_get(target[0], target[1], target[2]))->flags & MCB_FLUID)) {
        return FALSE;
    }
    if (held < MC_BLOCK_COUNT && mc_block_place[held] == PLACE_MODEL) {
        // the model blocks' getStateForPlacement (torches may become wall torches)
        f32 look[3];
        s32 nrm[3];
        int st, k;
        steve_look_dir(look);
        for (k = 0; k < 3; k++) nrm[k] = (pk->kind == 1) ? pk->normal[k] : (s32)(pk->n[k] > 0.5f ? 1 : (pk->n[k] < -0.5f ? -1 : 0));
        if (nrm[0] == 0 && nrm[1] == 0 && nrm[2] == 0) nrm[1] = 1;
        {
            // the shaped families' getStateForPlacement (stairs, slabs, doors...)
            f32 hitf[3];
            int r;
            for (k = 0; k < 3; k++) {
                hitf[k] = pk->hit[k] / MC_BLOCK - (f32)target[k];
                if (hitf[k] < 0.0f) hitf[k] = 0.0f;
                if (hitf[k] > 1.0f) hitf[k] = 1.0f;
            }
            r = shapes_place(&held, &st, target, nrm, hitf, look);
            if (r < 0) return FALSE;
            if (r == 0 && !redstone_place(&held, &st, nrm, pk->hit[1] / MC_BLOCK - (f32)target[1], look, target)) return FALSE;
        }
        // noCollision blocks can go where steve stands
        if (!(mc_block(held)->flags & MCB_NO_COLLISION) && steve_overlaps_cell(target)) return FALSE;
        if (shapes_family(held) == FAM_DOOR) {
            s32 up[3] = { target[0], target[1] + 1, target[2] };
            if (steve_overlaps_cell(up)) return FALSE;
        }
        shapes_set_placed(target, MAKE_CELL(held, st));
        sound_block(held, SOUND_PLACE, target[0], target[1], target[2]); // BlockItem.place
    } else {
        if (steve_overlaps_cell(target)) return FALSE;
        if (terrain_is_carved(target[0], target[1], target[2])
            && (block_is_ground_kind(held) || held == terrain_block_at(target[0], target[1], target[2]))
            && world_get(target[0], target[1], target[2]) == BLOCK_AIR) {
            // a ground block (or what was dug out of there) back into a dug
            // hole: bk's own triangles and textures come back there
            if (!terrain_uncarve(target[0], target[1], target[2])) return FALSE;
        } else
        world_set(target[0], target[1], target[2], MAKE_CELL(held, place_state(held, pk)));
        if (held == BLOCK_CHEST) chest_at(target, TRUE); // its ChestBlockEntity
        sound_block(held, SOUND_PLACE, target[0], target[1], target[2]); // BlockItem.place
    }
placed:
    if (!gSurv.creative) {
        ItemStack *s = &gSurv.inv[gSurv.selected];
        if (--s->count == 0) {
            s->id = 0;
            s->damage = 0;
        }
    }
    return TRUE;
}

// Minecraft.pickBlock + Inventory.pickSlot (middle click)
static void pick_block_item(BlockId block) {
    int i, slot = -1;

    if (block == BLOCK_AIR) return;
    if (block == BLOCK_GRASS && !gSurv.creative) block = BLOCK_GRASS; // same item
    for (i = 0; i < INV_SIZE; i++) {
        if (gSurv.inv[i].count > 0 && gSurv.inv[i].id == block) { slot = i; break; }
    }
    if (slot >= 0 && slot < 9) {
        gSurv.selected = slot;
        return;
    }
    if (slot >= 9) {
        // swap it into the hotbar: an empty hotbar slot if there is one,
        // otherwise the selected one (Inventory.getSuitableHotbarSlot)
        int target = gSurv.selected;
        ItemStack t;
        if (gSurv.inv[target].count != 0) {
            for (i = 0; i < 9; i++) {
                if (gSurv.inv[(gSurv.selected + i) % 9].count == 0) { target = (gSurv.selected + i) % 9; break; }
            }
        }
        t = gSurv.inv[target];
        gSurv.inv[target] = gSurv.inv[slot];
        gSurv.inv[slot] = t;
        gSurv.selected = target;
        return;
    }
    if (gSurv.creative) {
        // creative: put it in the hotbar (an empty slot first, else the selected one)
        int target = gSurv.selected;
        for (i = 0; i < 9; i++) {
            if (gSurv.inv[(gSurv.selected + i) % 9].count == 0) { target = (gSurv.selected + i) % 9; break; }
        }
        gSurv.inv[target].id = block;
        gSurv.inv[target].count = 1;
        gSurv.inv[target].damage = 0;
        gSurv.selected = target;
    }
}

// Inventory.dropAll on death: every stack thrown in a random direction
void surv_drop_all(void) {
    int i, sel = gSurv.selected;
    ItemStack keep = gSurv.inv[sel];

    if (gSurv.creative) return;
    for (i = 0; i < INV_TOTAL; i++) { // (Inventory.dropAll: armor and the offhand too)
        ItemStack st = gSurv.inv[i];
        if (st.count == 0 || st.id == ITEM_EGG) continue;
        gSurv.inv[sel] = st;
        surv_drop_selected(TRUE);
        // LivingEntity.createItemStackToDrop(randomly): random direction, 0..0.5 power, 0.2 up
        {
            int k;
            for (k = 0; k < MAX_ITEMS; k++) {
                if (items[k].active && items[k].age == 0 && items[k].pickup_delay == 40) {
                    f32 pow = rand01() * 0.5f, ang = rand01() * 360.0f;
                    items[k].vel[0] = -ml_sin_deg(ang) * pow;
                    items[k].vel[1] = 0.2f;
                    items[k].vel[2] = ml_cos_deg(ang) * pow;
                    items[k].pickup_delay = 41; // mark as handled
                }
            }
        }
        if (i != sel) gSurv.inv[i].count = 0, gSurv.inv[i].id = 0;
    }
    (void)keep;
    for (i = 0; i < INV_TOTAL; i++) {
        gSurv.inv[i].count = 0;
        gSurv.inv[i].id = 0;
        gSurv.inv[i].damage = 0;
    }
}

// the egg item follows bk's egg counter (bk is the truth): extra eggs go in
// like a pickup (Inventory.add, stacks of 16), missing ones come out of the
// last stacks first
static void sync_eggs(void) {
    s32 want = item_getCount(ITEM_D_EGGS), have = 0;
    int i;

    for (i = 0; i < INV_SIZE; i++) {
        if (gSurv.inv[i].id == ITEM_EGG) have += gSurv.inv[i].count;
    }
    if (want > have) {
        surv_inv_add(ITEM_EGG, want - have);
    }
    for (i = INV_SIZE - 1; i >= 0 && have > want; i--) {
        ItemStack *st = &gSurv.inv[i];
        if (st->id != ITEM_EGG) continue;
        while (st->count > 0 && have > want) {
            st->count--;
            have--;
        }
        if (st->count == 0) st->id = 0;
    }
}

F3Target gF3Target;

// DebugEntryLookingAt's two picks. blocks: Minecraft.pick at 20 blocks.
// fluids (ClipContext.Fluid.ANY): the first fluid cell along the ray before
// the block hit, walked in eighth blocks
static void f3_pick(f32 eye[3], f32 dir[3]) {
    f32 reach = 20.0f * MC_BLOCK, limit;
    Pick pk;
    int i, k;

    pick_block(eye, dir, reach, &pk);
    gF3Target.block_hit = pk.kind != 0;
    if (pk.kind != 0) {
        for (k = 0; k < 3; k++) gF3Target.cell[k] = pk.cell[k];
        gF3Target.cell_value = (pk.kind == 1) ? world_get_cell(pk.cell[0], pk.cell[1], pk.cell[2]) : pk.block;
    }
    limit = (pk.kind != 0) ? pk.dist : reach;
    gF3Target.fluid_hit = FALSE;
    for (i = 0; i * (MC_BLOCK / 8.0f) <= limit; i++) {
        s32 c[3];
        for (k = 0; k < 3; k++) c[k] = world_to_block(eye[k] + dir[k] * (MC_BLOCK / 8.0f) * i);
        if (fluid_type_at(c[0], c[1], c[2]) != FLUID_NONE) {
            gF3Target.fluid_hit = TRUE;
            for (k = 0; k < 3; k++) gF3Target.fcell[k] = c[k];
            gF3Target.fcell_value = world_get_cell(c[0], c[1], c[2]);
            break;
        }
    }
}

// one hand's turn at startUseItem (the held slot). TRUE if it did
// something. world_first: the main hand also tries boats and actors
static int use_hand(Pick *pk, f32 eye[3], f32 dir[3], f32 reach, int clicked, int world_first) {
    const McFood *food = surv_food(surv_held_item());
    // Minecraft.startUseItem: the entity under the crosshair comes first
    // (interactOn: boats, bk characters), whatever is held; only when that
    // passes do the block and the item get their turn (holding bread while
    // hungry used to eat instead of talking to bottles)
    if (clicked && world_first && boat_interact(eye, dir, (pk->kind != 0) ? pk->dist : reach)) {
        steve_swing(); // AbstractBoat.interact: get in
        return TRUE;
    }
    if (clicked && world_first && steve_interact(eye, dir, (pk->kind != 0) ? pk->dist : 3.0f * MC_BLOCK)) {
        steve_swing();
        return TRUE;
    }
    if (surv_held_item() == ITEM_SHIELD) {
        // ShieldItem.use: startUsingItem (ItemUseAnimation.BLOCK, 72000 ticks)
        if (gSurv.use_remaining > 0) return TRUE;
        gSurv.use_duration = 72000;
        gSurv.use_remaining = 72000;
        gSurv.use_slot = gSurv.selected;
    } else if (surv_held_item() == ITEM_BOW) {
        // BowItem.use: start drawing if there's an arrow (or creative)
        if (gSurv.creative || find_arrow() >= 0) {
            gSurv.use_duration = 72000;
            gSurv.use_remaining = 72000;
            gSurv.use_slot = gSurv.selected;
        } else {
            return FALSE; // (FAIL: the other hand gets its turn)
        }
    } else if (food != NULL && pk->kind != 1) {
        // Item.use with a food component: Player.canEat (hungry, or always
        // edible, or creative) -> startUsingItem
        if (gSteve.food_level < 20 || food->always_edible || gSurv.creative) {
            gSurv.use_duration = food->eat_ticks;
            gSurv.use_remaining = food->eat_ticks;
            gSurv.use_slot = gSurv.selected;
        } else {
            return FALSE; // can't eat now
        }
    } else if (surv_held_item() == ITEM_EGG) {
        // Item.use -> EggItem: throw (only on a real click, like minecraft's 4 tick delay)
        if (clicked) {
            steve_throw_egg(eye, dir);
            steve_swing();
        }
    } else if (clicked && boat_item_use(eye, dir, reach)) {
        steve_swing(); // BoatItem.use
    } else if (clicked && fluid_bucket_use(eye, dir, reach)) {
        steve_swing(); // BucketItem.use
    } else if (use_item(pk)) {
        steve_swing();
        steve_item_used();
    } else if (clicked && equip_held()) {
        steve_swing(); // Equippable.swapWithEquipmentSlot
    } else {
        return FALSE;
    }
    return TRUE;
}

void surv_tick(f32 eye[3], f32 dir[3], int attack_clicks, int attack_held, int use_clicks, int use_held) {
    // Player.blockInteractionRange: 4.5 survival, 5 creative
    f32 reach = (gSurv.creative ? 5.0f : 4.5f) * MC_BLOCK;
    Pick pk;

    sync_eggs();
    // LivingEntity.onEquipItem: a different item in an armor slot or the
    // offhand plays its equip sound (not on the first tick: a loaded file)
    {
        static ItemId worn_before[INV_TOTAL - INV_SIZE];
        static int worn_known;
        int k;
        for (k = 0; k < INV_TOTAL - INV_SIZE; k++) {
            ItemStack *st = &gSurv.inv[INV_SIZE + k];
            ItemId now = st->count > 0 ? st->id : 0;
            if (worn_known && now != 0 && now != worn_before[k]) surv_equip_sound(now);
            worn_before[k] = now;
        }
        worn_known = TRUE;
    }
    recipebook_tick(); // recipes/ advancements: inventory_changed unlocks
    advancements_tick(); // InventoryChangeTrigger + the toasts' recipe unlocks
    furnaces_tick(); // block entities tick with the level
    if (GM_SPECTATING()) {
        // spectators don't attack, mine or use anything
        gSurv.destroying = FALSE;
        gSurv.use_remaining = 0;
        return;
    }

    pick_block(eye, dir, reach, &pk);
    if (gShowDebug) f3_pick(eye, dir);

    // middle click: pick block
    if (gSurv.pick_clicks > 0) {
        gSurv.pick_clicks = 0;
        if (pk.kind != 0) {
            pick_block_item(pk.block == BLOCK_GRASS && pk.kind == 2 ? BLOCK_GRASS : pk.block);
        }
    }

    if (gSurv.miss_time > 0) gSurv.miss_time--;
    if (gSurv.right_click_delay > 0) gSurv.right_click_delay--;

    // keyAttack clicks -> startAttack
    while (attack_clicks-- > 0) {
        if (gSurv.miss_time > 0) {
            continue;
        }
        // entities win when nearer (3 blocks), then blocks
        if (boat_attack(eye, dir, (pk.kind != 0) ? pk.dist : 3.0f * MC_BLOCK, steve_current_attack_damage())) {
            steve_reset_attack_strength(); // Player.attack on a boat (VehicleEntity.hurtServer)
        } else if (steve_attack(eye, dir, (pk.kind != 0) ? pk.dist : 3.0f * MC_BLOCK)) {
            steve_exhaust(0.1f); // Player.attack
            steve_reset_attack_strength();
            damage_held(item_damage_per_attack(surv_held_item())); // Item.hurtEnemy
        } else if (pk.kind != 0) {
            tgt_terrain = pk.kind == 2;
            tgt_tree = pk.kind == 2 && pk.tree;
            tgt_block = pk.block;
            start_destroy(pk.cell);
        } else if (!gSurv.creative) {
            gSurv.miss_time = 10; // swinging at air
            steve_reset_attack_strength();
        }
        steve_swing();
    }

    // LivingEntity.updatingUsingItem: eating counts down while the key is held
    // and the same item stays selected; releaseUsingItem when it's let go
    if (gSurv.use_remaining > 0 && surv_held_item() == ITEM_BOW && gSurv.selected == gSurv.use_slot) {
        // (the bow is only ever drawn from the main hand here)
        if (use_held) {
            gSurv.use_remaining--; // drawing (72000 ticks max)
        } else {
            // releaseUsing
            bow_release(eye, dir, gSurv.use_duration - gSurv.use_remaining);
            gSurv.use_remaining = 0;
        }
        use_clicks = 0;
        use_held = FALSE;
    } else if (gSurv.use_remaining > 0) {
        ItemStack *ust = &gSurv.inv[gSurv.use_slot < INV_TOTAL ? gSurv.use_slot : gSurv.selected];
        int same_hand = gSurv.use_slot == INV_OFFHAND || gSurv.selected == gSurv.use_slot;
        if (ust->count > 0 && ust->id == ITEM_SHIELD && same_hand) {
            // blocking for as long as use is held (updatingUsingItem)
            if (!use_held) gSurv.use_remaining = 0;
            else gSurv.use_remaining--;
        } else if (!use_held || !same_hand || ust->count == 0 || surv_food(ust->id) == NULL) {
            gSurv.use_remaining = 0;
        } else if (--gSurv.use_remaining == 0) {
            // completeUsingItem -> FoodData.eat, the stack shrinks (not in creative)
            const McFood *food = surv_food(ust->id);
            ItemStack *st = ust;
            steve_eat(food->nutrition, food->saturation_modifier);
            advancements_consumed(ust->id); // ConsumeItemTrigger
            // Consumable.onConsume: the eat sound once more, then the burp
            sound_play_player(SND(ENTITY_GENERIC_EAT), sound_random() < 0.5f ? 0.5f : 1.0f, 1.0f + 0.2f * (sound_random() - sound_random()));
            sound_play_player(SND(ENTITY_PLAYER_BURP), 0.5f, 0.9f + sound_random() * 0.1f);
            if (!gSurv.creative && --st->count == 0) {
                st->id = 0;
                st->damage = 0;
            }
        } else {
            // Consumable.shouldEmitParticlesAndSounds: after 21.875% of the
            // time, every 4 ticks
            const McFood *food = surv_food(ust->id);
            int used = food->eat_ticks - gSurv.use_remaining;
            if (used > (int)(food->eat_ticks * 0.21875f) && gSurv.use_remaining % 4 == 0) {
                sound_play_player(SND(ENTITY_GENERIC_EAT), sound_random() < 0.5f ? 0.5f : 1.0f, 1.0f + 0.2f * (sound_random() - sound_random()));
            }
        }
        use_clicks = 0;
        use_held = gSurv.use_remaining > 0 ? FALSE : use_held;
    }

    // keyUse clicks, and holding it repeats every 4 ticks
    while (use_clicks-- > 0 || (use_held && gSurv.right_click_delay == 0)) {
        int clicked = use_clicks >= 0; // a real click, not the hold repeat
        gSurv.right_click_delay = 4;
        // startUseItem: InteractionHand.MAIN_HAND, then OFF_HAND if the main
        // hand did nothing (the offhand's stack goes in the held slot for
        // its turn: everything below works on the held slot)
        if (!use_hand(&pk, eye, dir, reach, clicked, TRUE)) {
            ItemStack *off = &gSurv.inv[INV_OFFHAND], *main = &gSurv.inv[gSurv.selected], t;
            int r = FALSE;
            if (off->count > 0 && off->id != ITEM_BOW) {
                int had_use = gSurv.use_remaining;
                t = *main; *main = *off; *off = t;
                r = use_hand(&pk, eye, dir, reach, clicked, FALSE);
                t = *main; *main = *off; *off = t;
                if (gSurv.use_remaining > 0 && !had_use) gSurv.use_slot = INV_OFFHAND; // eating from the offhand
            }
            if (!r && clicked) {
                // nothing to place: still let bk see a b press, so talking to
                // bottles etc. works without aiming exactly at them
                steve_press_b();
            }
        }
        use_held = FALSE;
    }

    // continueAttack
    if (!attack_held) {
        gSurv.miss_time = 0;
    }
    if (gSurv.miss_time <= 0) {
        if (attack_held && pk.kind != 0) {
            tgt_terrain = pk.kind == 2;
            tgt_tree = pk.kind == 2 && pk.tree;
            tgt_block = pk.block;
            if (continue_destroy(pk.cell)) {
                steve_swing();
                // addBreakingBlockEffect: crack bits fly off the face being hit
                if (!gSurv.creative && target_block(pk.cell) != BLOCK_AIR) {
                    particles_crack(pk.cell, pk.normal, target_block(pk.cell));
                }
            }
        } else {
            stop_destroy();
        }
    }
}

// --- drawing ---

// LevelRenderer.submitBlockOutline: the targeted block's edges in 40% black
// (ARGB.black(102)), line width ~2.5px at 1080p. lines are thin quads facing
// the camera, pulled a hair toward it so they win the depth test.
#define OUTLINE_BOXES 6
static Vtx outline_vtx[3][48 * OUTLINE_BOXES];
static int outline_buf;

static void draw_outline(Gfx **gfx, Mtx **mtx, f32 cam[3]) {
    static const u8 edges[12][2] = {
        { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, // along x
        { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, // along y
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }, // along z
    };
    f32 eye[3], dir[3], corner[8][3];
    f32 win_h = (gMouse.window_h > 0) ? (f32)gMouse.window_h : 1080.0f;
    f32 px = 2.5f * win_h / 1080.0f;
    f32 fov = option_fov() * steve_lerp(gSteve.o_fov_mod, gSteve.fov_mod, steve_partial_tick());
    f32 per_px = 2.0f * ml_sin_deg(fov * 0.5f) / ml_cos_deg(fov * 0.5f) / win_h; // world units per px at distance 1
    Pick pk;
    Vtx *v;
    int e, k, c, nboxes = 1;

    if (!gSteve.driving || screen_is_open()) return;
    eye[0] = gSteve.render_pos[0];
    eye[1] = gSteve.render_pos[1] + steve_eye_height();
    eye[2] = gSteve.render_pos[2];
    steve_look_dir(dir);
    pick_block(eye, dir, (gSurv.creative ? 5.0f : 4.5f) * MC_BLOCK, &pk);
    if (pk.kind == 0) return;

    outline_buf = (outline_buf + 1) % 3;
    {
        // the block's shape: its outline boxes (getShape), each one's edges
        McBox boxes[OUTLINE_BOXES];
        int bi;
        nboxes = 1;
        for (k = 0; k < 3; k++) { boxes[0].lo[k] = 0; boxes[0].hi[k] = 16; }
        if (pk.kind == 1) {
            nboxes = world_cell_boxes(world_get_cell(pk.cell[0], pk.cell[1], pk.cell[2]), FALSE, boxes, OUTLINE_BOXES);
            if (nboxes <= 0) {
                nboxes = 1;
                world_cell_box(world_get_cell(pk.cell[0], pk.cell[1], pk.cell[2]), boxes[0].lo, boxes[0].hi);
            }
        }
        // bk's ground: the cell up to where the ground surface is in it (the
        // whole cell box floated half in the air over slopes)
        if (pk.kind == 2 && !pk.tree) boxes[0].hi[1] = (u8)terrain_cell_surface_top(pk.cell[0], pk.cell[1], pk.cell[2]);
        for (bi = 0; bi < nboxes; bi++) {
        const u8 *lo = boxes[bi].lo, *hi = boxes[bi].hi;
        for (c = 0; c < 8; c++) {
            for (k = 0; k < 3; k++) {
                corner[c][k] = (pk.cell[k] + ((c & (1 << k)) ? (hi[k] > 16 ? 16 : hi[k]) : lo[k]) / 16.0f) * MC_BLOCK - cam[k];
            }
        }
    v = &outline_vtx[outline_buf][bi * 48];
    for (e = 0; e < 12; e++) {
        f32 *a = corner[edges[e][0]], *b = corner[edges[e][1]];
        f32 d[3], m[3], side[3], len, dist, half;

        for (k = 0; k < 3; k++) {
            d[k] = b[k] - a[k];
            m[k] = (a[k] + b[k]) * 0.5f;
        }
        side[0] = d[1] * m[2] - d[2] * m[1];
        side[1] = d[2] * m[0] - d[0] * m[2];
        side[2] = d[0] * m[1] - d[1] * m[0];
        len = __builtin_sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        dist = __builtin_sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        if (len < 1e-3f) len = 1e-3f;
        half = px * dist * per_px * 0.5f;
        for (k = 0; k < 3; k++) side[k] = side[k] / len * half;
        for (c = 0; c < 4; c++) {
            f32 *p = (c == 0 || c == 3) ? a : b;
            f32 sg = (c < 2) ? 1.0f : -1.0f;
            Vtx *o = &v[e * 4 + c];
            for (k = 0; k < 3; k++) {
                o->v.ob[k] = (s16)((p[k] + side[k] * sg) * 0.997f);
            }
            o->v.flag = 0;
            o->v.tc[0] = o->v.tc[1] = 0;
            o->v.cn[0] = o->v.cn[1] = o->v.cn[2] = 0;
            o->v.cn[3] = 102;
        }
    }
        }
    }
    v = outline_vtx[outline_buf];

    mlMtxIdent();
    mlMtxApply(*mtx);
    gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_OUTLINE, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gDPPipeSync((*gfx)++);
    gDPSetCycleType((*gfx)++, G_CYC_1CYCLE);
    gSPClearGeometryMode((*gfx)++, G_LIGHTING | G_TEXTURE_GEN | G_CULL_BOTH | G_FOG);
    gSPSetGeometryMode((*gfx)++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
    gSPTexture((*gfx)++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetCombineMode((*gfx)++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
    for (e = 0; e < 12 * nboxes; e += 2) {
        gSPVertex((*gfx)++, &v[e * 4], 8, 0);
        gSP2Triangles((*gfx)++, 0, 1, 2, 0, 0, 2, 3, 0);
        gSP2Triangles((*gfx)++, 4, 5, 6, 0, 4, 6, 7, 0);
    }
    gDPPipeSync((*gfx)++);
    gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
    gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
}

#define VTX_BUFFERS 3
static Vtx item_vtx[VTX_BUFFERS][MAX_ITEMS][24];
static Vtx crack_vtx[VTX_BUFFERS][24];
static int vtx_buffer;

void surv_draw_world(Gfx **gfx, Mtx **mtx) {
    f32 cam[3], pt = steve_partial_tick();
    int i, any = FALSE, stage;

    vtx_buffer = (vtx_buffer + 1) % VTX_BUFFERS;
    viewport_getPosition_vec3f(cam);
    stage = surv_destroy_stage();
    draw_outline(gfx, mtx, cam);
    particles_draw(gfx, mtx);

    for (i = 0; i < MAX_ITEMS; i++) {
        if (items[i].active) any = TRUE;
    }
    // vertices are camera relative: identity modelview, one tagged group per
    // item so rt64 interpolates its vertices between frames (see mc_gbi.h)
    world_set_texture_state(gfx);
    arrows_draw(gfx, mtx, cam, pt);
    xp_draw(gfx, mtx, cam, pt);
    tnt_draw(gfx, mtx, cam, pt);
    chests_draw(gfx, mtx, cam, pt);
    boats_draw(gfx, mtx, cam, pt);
    fluids_draw(gfx, mtx, cam);
    if (!any && stage < 0) {
        world_restore_texture_state(gfx);
        return;
    }

    // ItemEntityRenderer: bob, spin, then the block model's "ground" display
    // transform (translation 0,3,0 /16, scale 0.25) and the -0.5 model center
    for (i = 0; i < MAX_ITEMS; i++) {
        ItemEnt *it = &items[i];
        f32 age, bob, spin;
        Pose p;
        int k;

        if (!it->active) continue;
        age = (f32)it->age + pt;
        bob = ml_sin_deg((age / 10.0f + it->bob_offs) * 57.2957795f) * 0.1f + 0.1f;
        spin = (age / 20.0f + it->bob_offs) * 57.2957795f;
        pose_identity(&p);
        pose_scale(&p, MC_BLOCK, MC_BLOCK, MC_BLOCK);
        {
            f32 rel[3];
            for (k = 0; k < 3; k++) {
                rel[k] = (it->o_pos[k] + (it->pos[k] - it->o_pos[k]) * pt - cam[k]) / MC_BLOCK;
            }
            pose_translate(&p, rel[0], rel[1], rel[2]);
        }
        pose_translate(&p, 0.0f, bob, 0.0f);
        pose_rotate(&p, 1, spin);
        if (it->id == ITEM_SHIELD) {
            // (its own model: shield_display's ground transform below)
        } else if (item_is_block(it->id)) {
            // block model "ground": translation 0 3 0, scale 0.25
            pose_translate(&p, 0.0f, 3.0f / 16.0f, 0.0f);
            pose_scale(&p, 0.25f, 0.25f, 0.25f);
        } else {
            // item/generated "ground": translation 0 2 0, scale 0.5
            pose_translate(&p, 0.0f, 2.0f / 16.0f, 0.0f);
            pose_scale(&p, 0.5f, 0.5f, 0.5f);
        }
        if (it->id == ITEM_SHIELD) shield_display(&p, SHIELD_GROUND, FALSE);
        else pose_translate(&p, -0.5f, -0.5f, -0.5f);
        mlMtxIdent();
        mlMtxApply(*mtx);
        gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_ITEM_START + i, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        if (it->id == ITEM_SHIELD) {
            shield_emit(gfx, &p, FALSE, FALSE);
        } else if (item_is_block(it->id)) {
            world_emit_block_cube(gfx, it->id, &p, item_vtx[vtx_buffer][i], FALSE, NULL);
        } else {
            world_item_both_faces = TRUE;
            world_emit_item_sprite(gfx, item_sprite(it->id), &p, item_vtx[vtx_buffer][i], FALSE);
            world_item_both_faces = FALSE;
        }
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    }

#ifdef MC_HAVE_TEXTURES
    // the crack overlay on the block being broken (destroy_stage_N)
    if (stage >= 0) {
        Pose p;
        f32 grow = 1.0f / MC_BLOCK; // 1 unit out so it wins the depth test

        gDPPipeSync((*gfx)++);
        gDPSetRenderMode((*gfx)++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
        gDPSetAlphaCompare((*gfx)++, G_AC_NONE);
        pose_identity(&p);
        pose_scale(&p, MC_BLOCK, MC_BLOCK, MC_BLOCK);
        pose_translate(&p, (gSurv.destroy_pos[0] * MC_BLOCK - cam[0]) / MC_BLOCK - grow,
            (gSurv.destroy_pos[1] * MC_BLOCK - cam[1]) / MC_BLOCK - grow,
            (gSurv.destroy_pos[2] * MC_BLOCK - cam[2]) / MC_BLOCK - grow);
        pose_scale(&p, 1.0f + grow * 2.0f, 1.0f + grow * 2.0f, 1.0f + grow * 2.0f);
        mlMtxIdent();
        mlMtxApply(*mtx);
        gEXMatrixGroupSimpleVerts((*gfx)++, MC_ID_CRACK, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
        gSPMatrix((*gfx)++, (*mtx)++, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        world_emit_block_cube(gfx, BLOCK_STONE, &p, crack_vtx[vtx_buffer], FALSE, &mc_destroy_tex[stage * 256]);
        gSPPopMatrix((*gfx)++, G_MTX_MODELVIEW);
        gEXPopMatrixGroup((*gfx)++, G_MTX_MODELVIEW);
    }
#endif

    world_restore_texture_state(gfx);
}

// --- starter kit ---
// a world that's brand new (no inventory file, and bk's file has no jiggies
// or notes yet) starts with a few things. once it's saved it has a file, so
// the kit never comes again on that save.
int surv_starter_kit_pending;
extern s32 jiggyscore_total(void);
extern s32 itemscore_noteScores_getTotal(void);

void surv_starter_kit(void) {
    static const u16 kit[][2] = {
        { ITEM_STONE_SWORD, 1 }, { ITEM_STONE_PICKAXE, 1 }, { ITEM_STONE_AXE, 1 }, { ITEM_STONE_SHOVEL, 1 },
        { ITEM_BREAD, 8 }, { BLOCK_TORCH, 16 }, { BLOCK_CRAFTING_TABLE, 1 },
    };
    int i;
    if (!surv_starter_kit_pending || !gSteve.enabled) return;
    surv_starter_kit_pending = FALSE;
    if (!option_starter_kit()) return; // the option is off
    if (jiggyscore_total() != 0 || itemscore_noteScores_getTotal() != 0) return; // played before
    for (i = 0; i < (int)(sizeof(kit) / sizeof(kit[0])); i++) surv_inv_add(kit[i][0], kit[i][1]);
    recomp_printf("[mc] starter kit given\n");
}

// Block.popResource for others (a broken boat drops its item)
void surv_pop_item(s32 cell[3], ItemId id, int count) {
    pop_resource(cell, id, count);
}
